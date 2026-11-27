#include "lifter/sched_post_reload_projection.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/sched_model.h"

namespace rom_nom_nom::lifter::sched_model {

using detail::AnalyzeWithoutPendingListFlushCap;
using detail::BuildPreferredSched1OrderInBase;
using detail::CollectFullRegDefs;
using detail::CollectInsnRegReadDefDeaths;
using detail::CollectRegReads;
using detail::CollectSched2ObstructedMemRegEquivAdditions;
using detail::FindStage0Sched2PreimageCandidates;
using detail::FindUniqueInitPosForStoreInSeq;
using detail::ForcedReplayResult;
using detail::HasLoadEquivInvalidatorMovedIntoWindow;
using detail::HasMatchingMemRegEquivNote;
using detail::HasRegDeadNoteCovering;
using detail::HasStoreEquivInvalidatorMovedIntoWindow;
using detail::InsnRegReadDefDeaths;
using detail::IsAbiCallInterfaceHardReg;
using detail::IsBlock0PinnedParamCopy;
using detail::IsCrossClassFprToGprCopyReloadInheritedRead;
using detail::IsPureSched1704CallArtifactEdge;
using detail::kMipsArgPointerRegno;
using detail::kMipsArgRegFirst;
using detail::kMipsArgRegLast;
using detail::kMipsAtRegno;
using detail::kMipsCalleeSavedFprFirst;
using detail::kMipsCalleeSavedGprFirst;
using detail::kMipsCalleeSavedGprLast;
using detail::kMipsFpArgRegFirst;
using detail::kMipsFpArgRegLast;
using detail::kMipsFpRegFirst;
using detail::kMipsFpRegLast;
using detail::kMipsFpRetRegLast;
using detail::kMipsFramePointerRegno;
using detail::kMipsKernelRegFirst;
using detail::kMipsMdRegFirst;
using detail::kMipsMdRegLast;
using detail::kMipsRetRegFirst;
using detail::kMipsRetRegLast;
using detail::kMipsReturnAddrRegno;
using detail::kMipsStackPointerRegno;
using detail::kMipsTemp1Regno;
using detail::MemrefUsedBetweenForPostReloadGate;
using detail::ModeByteSize;
using detail::NormalizeMemForRegEquivMatch;
using detail::PatternDefinesHardRegSpan;
using detail::PatternReadsHardRegSpan;
using detail::RunForcedReplay;
using detail::SameAddrIgnoringRegNames;
using detail::SameMemSlotIgnoringRegNames;
using detail::SingleSetOf;
using detail::SolveSupernodeKahn;
using detail::StackSlotAccess;
using detail::StackSlotKey;
using detail::Stage0MemEquivAddition;
using detail::TryExpandUnpairedNoConflictRetvalForSched1;
using detail::TryExtractPlainStackSlotOffset;
using detail::TryMatchEarlyclobberDiOutputReload;
using detail::TryParsePlainStackSlotMove;
using detail::ValidateEquivMemForPostReloadGate;

std::vector<int> ScheduleSched1ViewCoreOrder(const Sched1ProjectionView& view,
                                             const MachineModel& mm) {
  std::vector<Insn> expanded;
  RegFacts expanded_facts;
  std::vector<int> exp_to_orig;
  if (TryExpandUnpairedNoConflictRetvalForSched1(view.core_insns, view.facts_s1, &expanded,
                                                 &expanded_facts, &exp_to_orig)) {
    const DepGraph g_s1 = Analyze(expanded, expanded_facts, mm, Phase::kSched1);
    const ScheduleResult s1_res =
        Schedule(expanded, g_s1, expanded_facts, mm, Phase::kSched1, view.live_at_end_s1);
    std::vector<int> mapped;
    mapped.reserve(view.core_insns.size());
    for (int exp_idx : s1_res.order) {
      if (exp_idx >= 0 && static_cast<size_t>(exp_idx) < exp_to_orig.size() &&
          exp_to_orig[static_cast<size_t>(exp_idx)] >= 0) {
        mapped.push_back(exp_to_orig[static_cast<size_t>(exp_idx)]);
      }
    }
    return mapped;
  }
  const DepGraph g_s1 = Analyze(view.core_insns, view.facts_s1, mm, Phase::kSched1);
  return Schedule(view.core_insns, g_s1, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1)
      .order;
}

std::vector<int> ScheduleClosestSched1CoreOrder(const Sched1ProjectionView& view,
                                                const MachineModel& mm) {
  auto solve_closest = [&](absl::Span<const Insn> insns,
                           const RegFacts& facts) -> std::vector<int> {
    const int n = static_cast<int>(insns.size());
    if (n <= 1) {
      return n == 1 ? std::vector<int>{0} : std::vector<int>{};
    }
    const DepGraph target_graph = Analyze(insns, facts, mm, Phase::kSched1);
    const ForcedReplayResult replay =
        RunForcedReplay(insns, target_graph, facts, mm, Phase::kSched1, view.live_at_end_s1,
                        /*continue_on_conflict=*/true);

    absl::btree_set<std::pair<int, int>> dag_edges;
    for (int cons = 0; cons < n; ++cons) {
      for (const Link& l : target_graph.log_links[static_cast<size_t>(cons)]) {
        dag_edges.insert({l.target, cons});
      }
    }
    if (replay.last_tail >= 0) {
      for (int u = 0; u < replay.last_tail; ++u) {
        dag_edges.insert({u, replay.last_tail});
      }
      for (int k = replay.last_tail; k + 1 < n; ++k) {
        dag_edges.insert({k, k + 1});
      }
    }
    std::vector<int> hard_next(static_cast<size_t>(n), -1);
    std::vector<int> hard_prev(static_cast<size_t>(n), -1);
    for (int i = 1; i < n; ++i) {
      if (target_graph.sched_group_p[static_cast<size_t>(i)]) {
        hard_next[static_cast<size_t>(i - 1)] = i;
        hard_prev[static_cast<size_t>(i)] = i - 1;
      }
    }
    std::vector<int> identity_rank(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      identity_rank[static_cast<size_t>(i)] = i;
    }

    absl::btree_set<std::pair<int, int>> constrained_edges = dag_edges;
    for (const Constraint& c : replay.constraints) {
      if (c.than < 0 || c.than >= n || c.must_have_greater_luid < 0 ||
          c.must_have_greater_luid >= n || c.than == c.must_have_greater_luid) {
        continue;
      }
      const std::pair<int, int> edge = {c.than, c.must_have_greater_luid};
      if (constrained_edges.contains(edge)) {
        continue;
      }
      constrained_edges.insert(edge);
      if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
               .has_value()) {
        constrained_edges.erase(edge);
      }
    }
    const std::optional<std::vector<int>> best_pre =
        SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank);
    if (!best_pre.has_value()) {
      return Schedule(insns, target_graph, facts, mm, Phase::kSched1, view.live_at_end_s1).order;
    }
    std::vector<Insn> permuted;
    permuted.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      Insn copy = insns[static_cast<size_t>((*best_pre)[static_cast<size_t>(pos)])];
      copy.luid = pos;
      permuted.push_back(std::move(copy));
    }
    const DepGraph g_pre = Analyze(permuted, facts, mm, Phase::kSched1);
    const ScheduleResult res =
        Schedule(permuted, g_pre, facts, mm, Phase::kSched1, view.live_at_end_s1);
    std::vector<int> mapped;
    mapped.reserve(static_cast<size_t>(n));
    for (int idx : res.order) {
      mapped.push_back((*best_pre)[static_cast<size_t>(idx)]);
    }
    return mapped;
  };

  std::vector<Insn> expanded;
  RegFacts expanded_facts;
  std::vector<int> exp_to_orig;
  if (TryExpandUnpairedNoConflictRetvalForSched1(view.core_insns, view.facts_s1, &expanded,
                                                 &expanded_facts, &exp_to_orig)) {
    const std::vector<int> exp_order = solve_closest(expanded, expanded_facts);
    std::vector<int> mapped;
    mapped.reserve(view.core_insns.size());
    for (int exp_idx : exp_order) {
      if (exp_idx >= 0 && static_cast<size_t>(exp_idx) < exp_to_orig.size() &&
          exp_to_orig[static_cast<size_t>(exp_idx)] >= 0) {
        mapped.push_back(exp_to_orig[static_cast<size_t>(exp_idx)]);
      }
    }
    return mapped;
  }
  return solve_closest(view.core_insns, view.facts_s1);
}

// =============================================================================
// 9. ProjectPostReloadBlockToSched1View (Read-Only Stage 3 Helper)
// =============================================================================

namespace {

bool ParseStackBaseOffset(const RtlExpr& addr, int* base_regno, int64_t* offset,
                          bool allow_fp_base = true) {
  auto is_stack_base = [allow_fp_base](int r) {
    return r == kMipsStackPointerRegno || (allow_fp_base && r == kMipsFramePointerRegno);
  };
  if (addr.op == RtlOp::kReg) {
    const int r = addr.RegNo();
    if (is_stack_base(r)) {
      *base_regno = r;
      *offset = 0;
      return true;
    }
    return false;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      addr.children[1].op == RtlOp::kConstInt) {
    const int r = addr.children[0].RegNo();
    if (is_stack_base(r)) {
      *base_regno = r;
      *offset = addr.children[1].int_val;
      return true;
    }
  }
  return false;
}

}  // namespace

namespace detail {

std::optional<StackSlotAccess> TryParsePlainStackSlotMove(const Insn& insn, bool allow_fp_base) {
  if (insn.kind != Insn::Kind::kInsn || insn.pn.pattern.op != RtlOp::kSet) {
    return std::nullopt;
  }
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual) {
      return std::nullopt;
    }
  }
  const RtlExpr& dst = insn.pn.pattern.SetDest();
  const RtlExpr& src = insn.pn.pattern.SetSrc();
  if (dst.op == RtlOp::kMem && src.op == RtlOp::kReg && !dst.flags.volatil &&
      !dst.flags.in_struct && !dst.flags.unchanging && !dst.children.empty()) {
    int base = -1;
    int64_t off = 0;
    if (ParseStackBaseOffset(dst.children[0], &base, &off, allow_fp_base)) {
      return StackSlotAccess{.is_store = true, .regno = src.RegNo(), .key = {base, off, dst.mode}};
    }
  }
  if (src.op == RtlOp::kMem && dst.op == RtlOp::kReg && !src.flags.volatil &&
      !src.flags.in_struct && !src.flags.unchanging && !src.children.empty()) {
    int base = -1;
    int64_t off = 0;
    if (ParseStackBaseOffset(src.children[0], &base, &off, allow_fp_base)) {
      return StackSlotAccess{.is_store = false, .regno = dst.RegNo(), .key = {base, off, src.mode}};
    }
  }
  return std::nullopt;
}

}  // namespace detail

namespace {

// Mirrors `config/mips/mips.h:1186-1209, 1734-1749` and `global.c:308-325`:
// `$fp` (reg 30, `FRAME_POINTER_REGNUM`) has `FIXED_REGISTERS[30] == 0` and
// `CALL_USED_REGISTERS[30] == 0`, so when `!frame_pointer_needed`, `global_alloc`
// allocates `$fp` as an ordinary callee-saved GPR.
bool IsFixedOrSpecialHardReg(int r, bool frame_pointer_needed = true) {
  if (r == kMipsFramePointerRegno) {
    return frame_pointer_needed;
  }
  return r == kMipsArgPointerRegno || r == kMipsAtRegno ||
         (r >= kMipsKernelRegFirst && r <= kMipsReturnAddrRegno) || r >= kMipsMdRegFirst;
}

bool IsAbiArgHardReg(int r) {
  return (r >= kMipsArgRegFirst && r <= kMipsArgRegLast) ||
         (r >= kMipsFpArgRegFirst && r <= kMipsFpArgRegLast);
}

}  // namespace

namespace detail {

bool IsAbiCallInterfaceHardReg(int r) {
  return (r >= kMipsRetRegFirst && r <= kMipsRetRegLast) ||
         (r >= kMipsFpRegFirst && r <= kMipsFpRetRegLast) || IsAbiArgHardReg(r);
}

void CollectRegReads(const RtlExpr& x, absl::btree_set<int>* reads) {
  if (x.op == RtlOp::kReg) {
    reads->insert(x.RegNo());
    return;
  }
  if (x.op == RtlOp::kSet) {
    const RtlExpr& dst = x.SetDest();
    if (dst.op == RtlOp::kMem && !dst.children.empty()) {
      CollectRegReads(dst.children[0], reads);
    } else if (dst.op == RtlOp::kStrictLowPart || dst.op == RtlOp::kSubreg ||
               dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract) {
      CollectRegReads(dst, reads);
    }
    CollectRegReads(x.SetSrc(), reads);
    return;
  }
  if (x.op == RtlOp::kClobber) {
    return;
  }
  for (const RtlExpr& ch : x.children) {
    CollectRegReads(ch, reads);
  }
}

void CollectFullRegDefs(const RtlExpr& x, absl::btree_set<int>* full_defs,
                        absl::btree_set<int>* partial_defs) {
  if (x.op == RtlOp::kSet || x.op == RtlOp::kClobber) {
    if (x.children.empty()) {
      return;
    }
    const RtlExpr& dst = x.children[0];
    if (dst.op == RtlOp::kReg) {
      full_defs->insert(dst.RegNo());
    } else if (dst.op == RtlOp::kSubreg || dst.op == RtlOp::kStrictLowPart ||
               dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract) {
      const RtlExpr* inner = &dst;
      while ((inner->op == RtlOp::kSubreg || inner->op == RtlOp::kStrictLowPart ||
              inner->op == RtlOp::kZeroExtract || inner->op == RtlOp::kSignExtract) &&
             !inner->children.empty()) {
        inner = &inner->children[0];
      }
      if (inner->op == RtlOp::kReg) {
        partial_defs->insert(inner->RegNo());
      }
    }
    return;
  }
  if (x.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : x.children) {
      CollectFullRegDefs(ch, full_defs, partial_defs);
    }
  }
}

}  // namespace detail

namespace {

void RenameExprRegs(RtlExpr* x, const absl::btree_map<int, int>& mapping) {
  if (x->op == RtlOp::kReg) {
    if (!x->flags.integrated) {
      if (const auto it = mapping.find(x->RegNo()); it != mapping.end()) {
        x->int_val = it->second;
      }
    }
    return;
  }
  for (RtlExpr& ch : x->children) {
    RenameExprRegs(&ch, mapping);
  }
}

// Matches a `reload1.c:emit_reload_insns` adjacent `hi`/`lo` output-reload pair
// (`mips.md:830-1010` `mulsi3_internal`, `smulsi3_highpart`, `umulsi3_highpart`,
// `mulsidi3`/`umulsidi3`): `(parallel [(set (reg:M hi/lo) <mult/truncate>) ...])`
// followed immediately by `(set (reg:M gpr) (reg:M hi/lo))`.
bool TryMatchAdjacentMdOutputReload(const Insn& prod, const Insn& mv, RtlExpr* out_gpr_dest) {
  if (prod.kind != Insn::Kind::kInsn || mv.kind != Insn::Kind::kInsn) {
    return false;
  }
  if (prod.pn.pattern.op != RtlOp::kParallel || prod.pn.pattern.children.size() < 2) {
    return false;
  }
  const RtlExpr& elem0 = prod.pn.pattern.children[0];
  if (elem0.op != RtlOp::kSet || elem0.children.size() != 2 || elem0.SetDest().op != RtlOp::kReg ||
      elem0.SetDest().flags.Any()) {
    return false;
  }
  const int md_reg = elem0.SetDest().RegNo();
  const RtlMachineMode md_mode = elem0.SetDest().mode;
  const RtlOp rhs_op = elem0.SetSrc().op;
  const bool valid_md_prod =
      (md_reg == kMipsMdRegFirst && md_mode == RtlMachineMode::kDi && rhs_op == RtlOp::kMult) ||
      (md_reg == kMipsMdRegFirst + 1 && md_mode == RtlMachineMode::kSi && rhs_op == RtlOp::kMult) ||
      (md_reg == kMipsMdRegFirst && md_mode == RtlMachineMode::kSi && rhs_op == RtlOp::kTruncate);
  if (!valid_md_prod) {
    return false;
  }
  if (mv.pn.pattern.op != RtlOp::kSet || mv.pn.pattern.children.size() != 2) {
    return false;
  }
  for (const RtlRegNote& note : mv.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual) {
      return false;
    }
  }
  const RtlExpr& mv_dst = mv.pn.pattern.SetDest();
  const RtlExpr& mv_src = mv.pn.pattern.SetSrc();
  if (mv_dst.op != RtlOp::kReg || mv_src.op != RtlOp::kReg) {
    return false;
  }
  if (mv_dst.RegNo() < 0 || mv_dst.RegNo() >= kMipsFpRegFirst || mv_dst.mode != md_mode) {
    return false;
  }
  if (mv_src.RegNo() != md_reg || mv_src.mode != md_mode) {
    return false;
  }
  if (out_gpr_dest != nullptr) {
    *out_gpr_dest = mv_dst;
  }
  return true;
}

}  // namespace

namespace detail {

// True iff `notes` carries a `REG_DEAD` note overlapping register `r`
// (`flow.c:2506-2515`: `flow.c` never emits `REG_DEAD P` on an insn that both
// reads and writes the same pseudo `P`, so a post-reload `(set r (op r ...))`
// with `REG_DEAD r` proves two distinct single-set pre-alloc pseudos shared `r`).
bool HasRegDeadNoteCovering(absl::Span<const RtlRegNote> notes, int r, const MachineModel& mm) {
  for (const RtlRegNote& note : notes) {
    if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
      continue;
    }
    const int d = note.datum.RegNo();
    if (d == r) {
      return true;
    }
    if (d >= 0 && d < kMipsFirstPseudoRegister && r >= 0 && r < kMipsFirstPseudoRegister) {
      const int nr = mm.HardRegNRegs(d, note.datum.mode);
      if (r >= d && r < d + nr) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace detail

namespace {

// Matches a single-use cross-bank `"=f"` FPR->GPR (`mfc1`) output reload
// (`reload1.c:5641, 6239-6285` `emit_reload_insns`) immediately following its
// producing FP arithmetic instruction:
// `(set (reg:SF|DF fpr) <fp_op>) ; (set (reg:SF|DF gpr) (reg:SF|DF fpr))`
// where the move carries no `REG_DEAD fpr` or `REG_EQUIV`/`REG_EQUAL` notes.
bool TryMatchAdjacentFprOutputReload(const Insn& prod_insn, const Insn& mv_insn,
                                     RtlExpr* gpr_dest_out) {
  if (prod_insn.kind != Insn::Kind::kInsn || mv_insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  const RtlExpr& p1 = prod_insn.pn.pattern;
  const RtlExpr& p2 = mv_insn.pn.pattern;
  if (p1.op != RtlOp::kSet || p1.children.size() != 2 || p1.SetDest().op != RtlOp::kReg ||
      p1.SetDest().flags.integrated || p2.op != RtlOp::kSet || p2.children.size() != 2 ||
      p2.SetDest().op != RtlOp::kReg || p2.SetDest().flags.integrated ||
      p2.SetSrc().op != RtlOp::kReg) {
    return false;
  }
  const int fpr = p1.SetDest().RegNo();
  const int gpr = p2.SetDest().RegNo();
  if (fpr < kMipsFpRegFirst || fpr > kMipsFpRegLast || gpr < 2 || gpr >= kMipsFpRegFirst ||
      p2.SetSrc().RegNo() != fpr || p1.SetDest().mode != p2.SetSrc().mode ||
      p2.SetDest().mode != p2.SetSrc().mode ||
      (p1.SetDest().mode != RtlMachineMode::kSf && p1.SetDest().mode != RtlMachineMode::kDf)) {
    return false;
  }
  const RtlOp sop = p1.SetSrc().op;
  if (sop == RtlOp::kReg || sop == RtlOp::kMem || sop == RtlOp::kConstDouble ||
      sop == RtlOp::kCall || sop == RtlOp::kSubreg) {
    return false;
  }
  for (const RtlRegNote& n : mv_insn.pn.reg_notes) {
    if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual ||
        (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
         n.datum.RegNo() == fpr)) {
      return false;
    }
  }
  if (gpr_dest_out != nullptr) {
    *gpr_dest_out = p2.SetDest();
  }
  return true;
}

// Returns true iff hard register `[regno, regno + HardRegNregs(regno, mode))`
// holds no live value after `block[cons_idx]` (either `cons_idx` itself fully
// overwrites/clobbers every sub-register, or no subsequent insn reads any
// sub-register before full overwrite/call-clobber and none is in `live_at_end_s2`).
bool IsRegDeadAfterInsn(absl::Span<const Insn> block, int cons_idx, int regno, RtlMachineMode mode,
                        const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2) {
  const int nregs = std::max(1, mm.HardRegNRegs(regno, mode));
  absl::btree_set<int> remaining;
  for (int o = 0; o < nregs; ++o) {
    remaining.insert(regno + o);
  }
  {
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(block[static_cast<size_t>(cons_idx)].pn.pattern, &fdefs, &pdefs);
    for (int r : fdefs) {
      remaining.erase(r);
    }
    if (remaining.empty()) {
      return true;
    }
  }
  for (size_t k = static_cast<size_t>(cons_idx) + 1; k < block.size(); ++k) {
    const Insn& insn = block[k];
    absl::btree_set<int> reads;
    CollectRegReads(insn.pn.pattern, &reads);
    if (insn.kind == Insn::Kind::kCall) {
      for (const RtlExpr& fu : insn.pn.call_fusage) {
        CollectRegReads(fu, &reads);
      }
    }
    for (int r : remaining) {
      if (reads.contains(r)) {
        return false;
      }
    }
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
    for (int r : fdefs) {
      remaining.erase(r);
    }
    if (insn.kind == Insn::Kind::kCall) {
      for (auto it = remaining.begin(); it != remaining.end();) {
        if (mm.CallUsed(*it)) {
          it = remaining.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (remaining.empty()) {
      return true;
    }
  }
  for (int r : remaining) {
    if (live_at_end_s2.contains(r)) {
      return false;
    }
  }
  return true;
}

// Matches a single-use `"f"` input reload (`reload1.c:5186-5257, 5641,
// 5849-5871, 6118-6125` `choose_reload_regs` + `emit_reload_insns`) preceding
// its consuming FP instruction:
//   - cross-bank GPR->FPR (`mtc1`): `(set (reg:SF|DF fpr) (reg:SF|DF gpr))`
//   - reload-inherited FPR->FPR (`mov.s`/`mov.d`) feeding an output-reloaded
//     FP arithmetic consumer (`md_output_reload_dest.contains(cons_idx)` whose
//     original dest is `fpr`): `(set (reg:SF|DF fpr) (reg:SF|DF src_fpr))`
// where `mv_insn` carries no `REG_EQUIV`/`REG_EQUAL`/`REG_DEAD src` notes (since
// `reload1.c` leaves any original `REG_DEAD` on `cons_insn`, whereas
// `function.c:assign_parms` entry-arg copies carry `REG_DEAD` from `flow.c`)
// and `fpr` is dead immediately after `cons_insn`.
bool TryMatchAdjacentFprInputReload(absl::Span<const Insn> block, int mv_idx, int cons_idx,
                                    const MachineModel& mm,
                                    const absl::btree_set<int>& live_at_end_s2,
                                    const absl::btree_map<int, RtlExpr>& md_output_reload_dest,
                                    int* fpr_out, RtlExpr* src_out) {
  const Insn& mv_insn = block[static_cast<size_t>(mv_idx)];
  const Insn& cons_insn = block[static_cast<size_t>(cons_idx)];
  if (mv_insn.kind != Insn::Kind::kInsn || cons_insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  const RtlExpr& mp = mv_insn.pn.pattern;
  if (mp.op != RtlOp::kSet || mp.children.size() != 2 || mp.SetDest().op != RtlOp::kReg ||
      mp.SetDest().flags.integrated || mp.SetSrc().op != RtlOp::kReg ||
      mp.SetSrc().flags.integrated) {
    return false;
  }
  const int fpr = mp.SetDest().RegNo();
  const int src_reg = mp.SetSrc().RegNo();
  const RtlMachineMode mode = mp.SetDest().mode;
  if (fpr < kMipsFpRegFirst || fpr > kMipsFpRegLast || mp.SetSrc().mode != mode ||
      (mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDf)) {
    return false;
  }
  const bool is_gpr_to_fpr = (src_reg >= 2 && src_reg < kMipsFpRegFirst);
  const bool is_fpr_to_fpr =
      (src_reg >= kMipsFpRegFirst && src_reg <= kMipsFpRegLast && src_reg != fpr);
  if (!is_gpr_to_fpr && !is_fpr_to_fpr) {
    return false;
  }
  const int src_nregs = std::max(1, mm.HardRegNRegs(src_reg, mode));
  for (const RtlRegNote& n : mv_insn.pn.reg_notes) {
    if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual) {
      return false;
    }
    if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg) {
      const int nr = n.datum.RegNo();
      if (nr >= src_reg && nr < src_reg + src_nregs) {
        return false;
      }
    }
  }
  const RtlExpr& cp = cons_insn.pn.pattern;
  if (cp.op == RtlOp::kUse || cp.op == RtlOp::kClobber || cp.op == RtlOp::kNote) {
    return false;
  }
  if (cp.op == RtlOp::kSet && cp.children.size() == 2 && cp.SetDest().op == RtlOp::kReg &&
      (cp.SetSrc().op == RtlOp::kReg || cp.SetSrc().op == RtlOp::kMem)) {
    return false;
  }
  if (is_fpr_to_fpr) {
    // Only fold an FPR->FPR copy when `cons_idx` is an output-reloaded FP
    // arithmetic insn (`md_output_reload_dest`) sharing `fpr` as both its
    // reload-inherited input reload reg and its `"=f"` output reload reg
    // (`reload1.c:5186-5257, 5641`).
    const RtlExpr* cs = SingleSetOf(cp);
    if (!md_output_reload_dest.contains(cons_idx) || cs == nullptr ||
        cs->SetDest().op != RtlOp::kReg || cs->SetDest().RegNo() != fpr) {
      return false;
    }
  }
  absl::btree_set<int> cons_reads;
  CollectRegReads(cp, &cons_reads);
  if (!cons_reads.contains(fpr)) {
    return false;
  }
  if (!IsRegDeadAfterInsn(block, cons_idx, fpr, mode, mm, live_at_end_s2)) {
    return false;
  }
  if (fpr_out != nullptr) {
    *fpr_out = fpr;
  }
  if (src_out != nullptr) {
    *src_out = mp.SetSrc();
  }
  return true;
}

void ReplaceRegInExpr(RtlExpr* expr, int old_reg, const RtlExpr& replacement) {
  if (expr->op == RtlOp::kReg && expr->RegNo() == old_reg && !expr->flags.integrated) {
    *expr = replacement;
    return;
  }
  for (RtlExpr& ch : expr->children) {
    ReplaceRegInExpr(&ch, old_reg, replacement);
  }
}

void ReplaceRegInReadPositions(RtlExpr* pattern, int old_reg, const RtlExpr& replacement) {
  auto handle_node = [&](RtlExpr* node) {
    if (node->op == RtlOp::kSet && node->children.size() == 2) {
      ReplaceRegInExpr(&node->children[1], old_reg, replacement);
      if (node->children[0].op == RtlOp::kMem) {
        ReplaceRegInExpr(&node->children[0], old_reg, replacement);
      }
    } else if (node->op != RtlOp::kClobber) {
      ReplaceRegInExpr(node, old_reg, replacement);
    }
  };
  if (pattern->op == RtlOp::kParallel) {
    for (RtlExpr& ch : pattern->children) {
      handle_node(&ch);
    }
  } else {
    handle_node(pattern);
  }
}

// Returns true when post-call read-modify-write `(set r (op r ...))` at core index `ci`
// on a block-local callee-saved register `r` that crossed an earlier call matches a
// `local-alloc.c:1266-1300, 1851-1886` (`combine_regs`) dying-input register reuse of a
// fresh `reg_n_calls_crossed == 0` pseudo (`sched.c:1732`), which locks the insn after the call.
bool IsPostCallCombineRegsSplitCandidate(absl::Span<const Insn> post_reload_block,
                                         absl::Span<const int> core_to_orig_index, int ci, int r,
                                         int birth_ci, int last_call_ci,
                                         const absl::btree_set<int>& live_at_end_s2,
                                         const MachineModel& mm) {
  if (r < 0 || r >= kMipsFirstPseudoRegister || mm.CallUsed(r) || birth_ci < 0 ||
      last_call_ci <= birth_ci || ci <= last_call_ci) {
    return false;
  }
  const Insn& insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
  const RtlExpr* s = SingleSetOf(insn.pn.pattern);
  if (s == nullptr || s->SetDest().op != RtlOp::kReg || s->SetDest().RegNo() != r ||
      s->SetDest().flags.integrated) {
    return false;
  }
  for (const RtlRegNote& n : insn.pn.reg_notes) {
    if ((n.kind == RtlMachineMode::kRegEquiv && n.datum.op != RtlOp::kMem) ||
        n.kind == RtlMachineMode::kRegEqual || n.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const RtlExpr& rhs = s->SetSrc();
  if (rhs.op == RtlOp::kReg || rhs.op == RtlOp::kMem || rhs.children.empty()) {
    return false;
  }
  // `sched.c:1732` (`reg_n_calls_crossed == 0`) anti-dependence on the preceding
  // call is only needed when `rhs` does not already read a call-used hard reg
  // (e.g. a call return value `$v0`/`$f0`), which already locks the insn after
  // the call via a hard `REG_DEP_TRUE` edge.
  absl::btree_set<int> rhs_reads;
  CollectRegReads(rhs, &rhs_reads);
  for (int rr : rhs_reads) {
    if (rr >= 0 && rr < kMipsFirstPseudoRegister && mm.CallUsed(rr)) {
      return false;
    }
  }
  const RtlMachineMode dst_mode = s->SetDest().mode;
  // Match `local-alloc.c:1266-1300`: `combine_regs` tries operands in order
  // and stops on the first winning dying local operand of the same mode.
  bool direct_combinable_operand = false;
  for (const RtlExpr& ch : rhs.children) {
    if (ch.op == RtlOp::kReg && ch.RegNo() == r && ch.mode == dst_mode) {
      direct_combinable_operand = true;
      break;
    }
    if (ch.op == RtlOp::kReg && ch.mode == dst_mode && ch.RegNo() >= 0 &&
        ch.RegNo() < kMipsFirstPseudoRegister &&
        HasRegDeadNoteCovering(insn.pn.reg_notes, ch.RegNo(), mm)) {
      break;
    }
  }
  if (!direct_combinable_operand) {
    return false;
  }
  const int m = static_cast<int>(core_to_orig_index.size());
  bool saw_call_after = false;
  for (int k = ci + 1; k < m; ++k) {
    const Insn& later = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
    if (later.kind == Insn::Kind::kCall) {
      saw_call_after = true;
      continue;
    }
    absl::btree_set<int> later_reads;
    CollectRegReads(later.pn.pattern, &later_reads);
    if (later_reads.contains(r) && saw_call_after) {
      return false;
    }
    absl::btree_set<int> later_fdefs;
    absl::btree_set<int> later_pdefs;
    CollectFullRegDefs(later.pn.pattern, &later_fdefs, &later_pdefs);
    // `flow.c:2506-2515` + `local-alloc.c:1266-1300`: a non-reading full def of
    // `r` (or an explicit `REG_DEAD r` on a non-defining reader) ends `ci`'s
    // live range cleanly before any later unrelated reuse of callee-saved `r`.
    // A subsequent call-free full RMW (`later_fdefs && later_reads && !saw_call_after`)
    // is the next step of the same post-call `combine_regs` chain (`sched.c:1732`)
    // and keeps scanning forward to verify `r` dies without crossing a later call.
    if (later_fdefs.contains(r) && !later_reads.contains(r)) {
      return true;
    }
    if (later_pdefs.contains(r)) {
      return false;
    }
    if (later_fdefs.contains(r)) {
      if (HasRegDeadNoteCovering(later.pn.reg_notes, r, mm)) {
        return true;
      }
      continue;
    }
    if (later_reads.contains(r) && HasRegDeadNoteCovering(later.pn.reg_notes, r, mm)) {
      return true;
    }
  }
  return !live_at_end_s2.contains(r);
}

// Returns true when read-modify-write `(set r (op r ...))` (or single-`SET`
// `PARALLEL`) at core index `ci` matches a `local-alloc.c:1266-1300, 1851-1886`
// (`combine_regs`) dying-input register reuse of a fresh single-set pseudo
// (`sched.c:2490` `birthing_insn_p`, `sched.c:1732` `reg_n_calls_crossed == 0`).
// Excludes MIPS `lui + ori` 32-bit constant materialization (`config/mips/mips.c`),
// which GCC emits on a single multi-set pseudo without `REG_DEAD`.
bool IsGeneralCombineRegsRmwSplitCandidate(absl::Span<const Insn> post_reload_block,
                                           absl::Span<const int> core_to_orig_index, int ci, int r,
                                           int birth_ci, const absl::btree_set<int>& live_at_end_s2,
                                           const MachineModel& mm) {
  if (birth_ci < 0 || ci <= birth_ci || r < 2 || r >= kMipsFirstPseudoRegister || r == 28 ||
      r == kMipsStackPointerRegno || r == kMipsFramePointerRegno) {
    return false;
  }
  const Insn& insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
  const RtlExpr* s = SingleSetOf(insn.pn.pattern);
  if (s == nullptr || s->SetDest().op != RtlOp::kReg || s->SetDest().RegNo() != r ||
      s->SetDest().flags.integrated || s->SetDest().flags.frame_related) {
    return false;
  }
  for (const RtlRegNote& n : insn.pn.reg_notes) {
    if ((n.kind == RtlMachineMode::kRegEquiv && n.datum.op != RtlOp::kMem) ||
        n.kind == RtlMachineMode::kRegEqual || n.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const RtlExpr& rhs = s->SetSrc();
  if (rhs.op == RtlOp::kReg || rhs.op == RtlOp::kMem || rhs.children.empty()) {
    return false;
  }
  // Exclude MIPS `lui + ori` constant materialization (`(set r high16)` followed
  // by `(set r (ior r low16))`), which stays a single multi-set pseudo in GCC,
  // and exclude subword RMWs onto a wider multi-word def (e.g. `muldi3`
  // high-word `SImode` accumulation into a `DImode` pair): Stage 4 cannot
  // split mixed-mode hard-register live ranges into separate pseudos, so a
  // self-set `REG_DEAD` would survive illegally (`flow.c:2506-2515`,
  // `sched.c:2872-2906`).
  const Insn& birth_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[birth_ci])];
  const RtlExpr* birth_s = SingleSetOf(birth_insn.pn.pattern);
  if (birth_s != nullptr && birth_s->SetDest().op == RtlOp::kReg &&
      birth_s->SetDest().mode != s->SetDest().mode) {
    return false;
  }
  if (rhs.op == RtlOp::kIor && rhs.children.size() == 2 && rhs.children[1].op == RtlOp::kConstInt) {
    if (birth_s != nullptr &&
        ((birth_s->SetSrc().op == RtlOp::kConstInt && (birth_s->SetSrc().int_val & 0xffff) == 0) ||
         birth_s->SetSrc().op == RtlOp::kHigh)) {
      return false;
    }
  }
  // `local-alloc.c:892-925, 1004-1007` (`optimize_reg_copy_1` called from
  // `update_equiv_regs`): if `birth_insn` is a same-mode register-to-register
  // copy `(set r src)` where `src` remains live after `birth_insn`, splitting
  // `r` turns `r` into a `reg_n_sets == 1` copy target and triggers forward
  // copy propagation. Note that `reload1.c` output-reload moves omit
  // `REG_DEAD src` even when `src` is dead after `birth_insn`, so verify `src`
  // actually has a later use or is live-out.
  if (birth_s != nullptr && birth_s->SetDest().op == RtlOp::kReg &&
      birth_s->SetSrc().op == RtlOp::kReg && birth_s->SetSrc().mode == birth_s->SetDest().mode &&
      !birth_s->SetSrc().IsArgPointerReg() &&
      !HasRegDeadNoteCovering(birth_insn.pn.reg_notes, birth_s->SetSrc().RegNo(), mm)) {
    const int copy_src = birth_s->SetSrc().RegNo();
    const int m_body = static_cast<int>(core_to_orig_index.size());
    bool src_live_after_birth = live_at_end_s2.contains(copy_src);
    for (int k = birth_ci + 1; k < m_body; ++k) {
      const Insn& after = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
      if (after.kind == Insn::Kind::kCall) {
        if (mm.CallUsed(copy_src)) {
          src_live_after_birth = false;
          break;
        }
        continue;
      }
      absl::btree_set<int> after_reads;
      CollectRegReads(after.pn.pattern, &after_reads);
      if (after_reads.contains(copy_src)) {
        src_live_after_birth = true;
        break;
      }
      absl::btree_set<int> after_fdefs;
      absl::btree_set<int> after_pdefs;
      CollectFullRegDefs(after.pn.pattern, &after_fdefs, &after_pdefs);
      if (after_fdefs.contains(copy_src)) {
        src_live_after_birth = false;
        break;
      }
    }
    if (src_live_after_birth) {
      return false;
    }
  }
  const RtlMachineMode dst_mode = s->SetDest().mode;
  // Match `local-alloc.c:1266-1300`: `combine_regs` tries operands in order
  // and stops on the first winning dying local operand of the same mode.
  bool direct_combinable_operand = false;
  for (const RtlExpr& ch : rhs.children) {
    if (ch.op == RtlOp::kReg && ch.RegNo() == r && ch.mode == dst_mode) {
      direct_combinable_operand = true;
      break;
    }
    if (ch.op == RtlOp::kReg && ch.mode == dst_mode && ch.RegNo() >= 0 &&
        ch.RegNo() < kMipsFirstPseudoRegister &&
        HasRegDeadNoteCovering(insn.pn.reg_notes, ch.RegNo(), mm)) {
      break;
    }
  }
  if (!direct_combinable_operand) {
    return false;
  }
  // `expmed.c:693-707` (`store_fixed_bit_field`): bitfield read-modify-write
  // sequences pass `subtarget = temp` from `and_optab` into `ior_optab`, so
  // multi-step intra-block `and`/`ior` RMW chains on `r` originate as a single
  // multi-set pseudo rather than distinct `combine_regs` pseudos.
  const bool is_bitwise_rmw = (rhs.op == RtlOp::kAnd || rhs.op == RtlOp::kIor);
  if (is_bitwise_rmw) {
    for (int k = birth_ci + 1; k < ci; ++k) {
      const Insn& mid = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
      if (mid.kind == Insn::Kind::kCall) {
        continue;
      }
      const RtlExpr* ms = SingleSetOf(mid.pn.pattern);
      if (ms != nullptr && ms->SetDest().op == RtlOp::kReg && ms->SetDest().RegNo() == r &&
          (ms->SetSrc().op == RtlOp::kAnd || ms->SetSrc().op == RtlOp::kIor)) {
        return false;
      }
    }
  }
  // Verify the live range of `r` terminates within this block (so both `P_old`
  // and `P_new` are block-local and deterministically tied by `local-alloc.c:1855`).
  const int m = static_cast<int>(core_to_orig_index.size());
  for (int k = ci + 1; k < m; ++k) {
    const Insn& later = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
    if (later.kind == Insn::Kind::kCall) {
      if (mm.CallUsed(r)) {
        return true;
      }
      continue;
    }
    absl::btree_set<int> later_reads;
    CollectRegReads(later.pn.pattern, &later_reads);
    absl::btree_set<int> later_fdefs;
    absl::btree_set<int> later_pdefs;
    CollectFullRegDefs(later.pn.pattern, &later_fdefs, &later_pdefs);
    if (later_pdefs.contains(r)) {
      return false;
    }
    if (later_fdefs.contains(r)) {
      if (!later_reads.contains(r)) {
        return true;
      }
      // `flow.c:2505-2520`, `local-alloc.c:1266-1300`: a later RMW carrying
      // `REG_DEAD r` (e.g. a return-value `reg/i` RMW killing the last local
      // pseudo in `$v0` before redefining `$v0` as live-at-exit) ends the local
      // live range of `r` within this block.
      if (HasRegDeadNoteCovering(later.pn.reg_notes, r, mm)) {
        return true;
      }
      if (is_bitwise_rmw) {
        const RtlExpr* ls = SingleSetOf(later.pn.pattern);
        if (ls != nullptr && (ls->SetSrc().op == RtlOp::kAnd || ls->SetSrc().op == RtlOp::kIor)) {
          return false;
        }
      }
    }
  }
  return !live_at_end_s2.contains(r);
}

// Returns true when register `regno` (spanning `mode`) written/read at `idx`
// does NOT stay live out of `block` without an intervening redefinition or
// clobbering call (i.e. it is a scratch reload register rather than a hard reg
// live across the block exit).
bool RegValueDoesNotReachBlockEnd(absl::Span<const Insn> block, int idx, int regno,
                                  RtlMachineMode mode, const MachineModel& mm,
                                  const absl::btree_set<int>& live_at_end_s2) {
  const int nregs = std::max(1, mm.HardRegNRegs(regno, mode));
  absl::btree_set<int> remaining;
  for (int k = 0; k < nregs; ++k) {
    if (live_at_end_s2.contains(regno + k)) {
      remaining.insert(regno + k);
    }
  }
  if (remaining.empty()) {
    return true;
  }
  const int n = static_cast<int>(block.size());
  for (int i = idx + 1; i < n; ++i) {
    const Insn& insn = block[static_cast<size_t>(i)];
    if (insn.kind == Insn::Kind::kCall) {
      for (auto it = remaining.begin(); it != remaining.end();) {
        if (mm.CallUsed(*it)) {
          it = remaining.erase(it);
        } else {
          ++it;
        }
      }
      if (remaining.empty()) {
        return true;
      }
      continue;
    }
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
    for (int d : fdefs) {
      remaining.erase(d);
    }
    if (remaining.empty()) {
      return true;
    }
  }
  return false;
}

// Identifies a distinct pre-reload spilled-pseudo live range on a stack slot:
// `(slot_offset, version_id)` where `version_id == -1` denotes a cross-block
// live-in/shared slot and `version_id >= 0` is the defining store's `orig_idx`
// for non-overlapping intra-block pseudos sharing `spill_stack_slot[from_reg]`
// (`reload1.c:2305-2306, 2362-2367`).
using SpillPseudoKey = std::pair<int64_t, int>;

struct SpillSlotFoldInfo {
  // Maps producer orig_idx -> spill pseudo key for folded output-reload spill stores.
  absl::btree_map<int, SpillPseudoKey> store_slot_by_prod;
  // Maps consumer orig_idx -> (hard_reg -> (spill pseudo key, mode)) for folded single-use
  // input-reload spill loads.
  absl::btree_map<int, absl::btree_map<int, std::pair<SpillPseudoKey, RtlMachineMode>>>
      load_subs_by_cons;
  // Maps folded input-reload orig_idx -> consuming orig_idx.
  absl::btree_map<int, int> input_reload_orig_consumer;
  // Maps excluded intra-block spill-load orig_idx (not folded into a single consumer) to
  // `(hard_reg, spill pseudo key)` so multi-use reloads still map `cur_pseudo[hard_reg]` to
  // `P_slot`.
  absl::btree_map<int, std::pair<int, SpillPseudoKey>> multi_use_load_slot_by_orig;
  // Maps core in-place `reload1.c:1968-1992` spill-slot load orig_idx
  // (`(set (reg arg/ret) (mem (sp + spill_off)))` feeding a call `call_fusage` or
  // `(use (reg/i ...))`) to `(spill pseudo key, mode)` so `ProjectPostReloadBlockToSched1View`
  // keeps it as a core `(set (reg arg/ret) (reg P_slot))` copy.
  absl::btree_map<int, std::pair<SpillPseudoKey, RtlMachineMode>> in_place_spill_load_slot_by_orig;
  // Maps block-0 direct spilled entry-parameter copy store orig_idx
  // (`function.c:assign_parms` + `reload1.c:2309-2400` `alter_reg`:
  // `(set (mem:M [sp/fp+off]) (reg:M arg_reg))` with no prior producer in B0)
  // to `(spill pseudo key, mode)` so `ProjectPostReloadBlockToSched1View` keeps it as
  // a core `(set (reg:M P_slot) (reg:M arg_reg))` entry-parameter copy.
  absl::btree_map<int, std::pair<SpillPseudoKey, RtlMachineMode>>
      direct_entry_arg_spill_slot_by_store;
  // Spill pseudo keys whose spilled pseudo is live-out to another basic block.
  absl::btree_set<SpillPseudoKey> live_out_slot_keys;
};

// Returns true when stack store `s_idx` of `(set (mem:M [sp/fp+off]) (reg:M s_reg))`
// is an intra-block output-reload spill store whose reload register `s_reg` is
// inherited directly (`reload1.c:5186-5257`, `reload1.c:3973-3981`) by a later
// consumer carrying the spilled pseudo's in-place `MEM`-rewritten `REG_DEAD`
// note (`reload1.c:1968-1992` `PUT_CODE (reg, MEM)`), so no intra-block
// input-reload load from `key` was emitted.
bool HasInheritedSpillSlotDeadConsumer(absl::Span<const Insn> post_reload_block, int s_idx,
                                       int s_reg, const StackSlotKey& key, const MachineModel& mm,
                                       bool fp_needed) {
  const int n = static_cast<int>(post_reload_block.size());
  const int s_nregs = std::max(1, mm.HardRegNRegs(s_reg, key.mode));
  for (int c = s_idx + 1; c < n; ++c) {
    const Insn& cand = post_reload_block[static_cast<size_t>(c)];
    bool has_matching_mem_dead = false;
    for (const RtlRegNote& note : cand.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem &&
          note.datum.mode == key.mode && !note.datum.children.empty()) {
        int nb = -1;
        int64_t noff = 0;
        if (ParseStackBaseOffset(note.datum.children[0], &nb, &noff, fp_needed) &&
            nb == key.base_regno && noff == key.offset) {
          has_matching_mem_dead = true;
          break;
        }
      }
    }
    if (has_matching_mem_dead) {
      absl::btree_set<int> c_reads;
      CollectRegReads(cand.pn.pattern, &c_reads);
      for (int k = 0; k < s_nregs; ++k) {
        if (c_reads.contains(s_reg + k)) {
          return true;
        }
      }
      return false;
    }
    if (cand.kind == Insn::Kind::kJump) {
      break;
    }
    if (cand.kind == Insn::Kind::kCall) {
      for (int k = 0; k < s_nregs; ++k) {
        if (mm.CallUsed(s_reg + k)) {
          return false;
        }
      }
      continue;
    }
    if (auto m_acc = TryParsePlainStackSlotMove(cand, fp_needed);
        m_acc.has_value() && m_acc->key.offset == key.offset) {
      break;
    }
    absl::btree_set<int> c_fdefs;
    absl::btree_set<int> c_pdefs;
    CollectFullRegDefs(cand.pn.pattern, &c_fdefs, &c_pdefs);
    for (int k = 0; k < s_nregs; ++k) {
      if (c_fdefs.contains(s_reg + k) || c_pdefs.contains(s_reg + k)) {
        return false;
      }
    }
  }
  return false;
}

// Identifies intra-block (non-caller-save), inherited intra-block, and cross-block
// `reload1.c:1968-1992, 2309-2419, 5186-5257, 5641-6160` (`alter_reg` /
// `choose_reload_regs` / `emit_reload_insns`) output-reload spill stores,
// input-reload spill loads, and in-place spill-slot loads into call-arg/return
// registers so `ProjectPostReloadBlockToSched1View` reconstructs the true
// pre-reload spilled pseudos `P_slot` and data-dependence edges across calls
// and scratch reuses.
SpillSlotFoldInfo FoldReloadSpillsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context,
    const absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>>& slot_stores,
    const absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>>& slot_loads,
    const absl::btree_set<int>& intra_reload_spill_store_indices,
    const absl::btree_set<int>& intra_reload_spill_load_indices,
    const absl::btree_map<int, RtlExpr>& md_output_reload_dest,
    std::vector<bool>* is_spill_overhead, absl::btree_map<int, int>* output_reload_orig_owner) {
  SpillSlotFoldInfo info;
  const int n = static_cast<int>(post_reload_block.size());
  const bool fp_needed = !context.stack_layout.valid || context.stack_layout.frame_pointer_needed;
  const bool can_fold_cross_block =
      context.has_cfg_context && context.stack_layout.valid && !context.has_stack_addr_escape &&
      context.stack_layout.frame_size > context.stack_layout.starting_frame_offset;
  const int frame_base =
      context.stack_layout.frame_pointer_needed ? kMipsFramePointerRegno : kMipsStackPointerRegno;
  auto is_cross_spill_slot_key = [&](const StackSlotKey& key) {
    return can_fold_cross_block &&
           (key.base_regno == frame_base || key.base_regno == kMipsStackPointerRegno) &&
           key.offset >= context.stack_layout.starting_frame_offset &&
           key.offset < context.stack_layout.frame_size;
  };

  absl::btree_set<int64_t> folded_intra_store_offsets;
  struct FoldedStoreRecord {
    int s_idx = -1;
    int prod_idx = -1;
    bool is_direct_entry_arg = false;
    RtlMachineMode mode = RtlMachineMode::kSi;
  };
  struct FoldedLoadRecord {
    enum class Kind { kFoldedSingleUse, kInPlace, kMultiUse };
    int64_t slot_off = 0;
    int l_idx = -1;
    int cons_idx = -1;
    int l_reg = -1;
    RtlMachineMode mode = RtlMachineMode::kSi;
    Kind kind = Kind::kFoldedSingleUse;
  };
  absl::btree_map<int64_t, std::vector<FoldedStoreRecord>> folded_stores_by_off;
  absl::btree_set<int64_t> cross_live_out_offsets;
  absl::btree_set<int64_t> cross_from_other_offsets;
  std::vector<FoldedLoadRecord> folded_loads;
  absl::btree_map<std::pair<int, int64_t>, int> cons_slot_load_idx;
  absl::btree_set<int> claimed_store_prods;
  absl::btree_set<std::pair<int, int>> claimed_cons_lregs;

  // (a) Output-reload spill stores (`reload1.c:5641-6155`): intra-block
  //     non-caller-save spill stores, inherited intra-block spill stores
  //     (`reload1.c:1968-1992, 5186-5257`), and cross-block spill stores.
  for (const auto& [key, stores] : slot_stores) {
    const bool is_cross_live_out =
        is_cross_spill_slot_key(key) && context.other_block_spill_load_offsets.contains(key.offset);
    if (is_cross_spill_slot_key(key) &&
        context.other_block_spill_store_counts.contains(key.offset)) {
      cross_from_other_offsets.insert(key.offset);
    }
    const bool is_local_spill_slot =
        !context.stack_layout.valid ||
        ((key.base_regno == frame_base || key.base_regno == kMipsStackPointerRegno) &&
         key.offset >= context.stack_layout.starting_frame_offset &&
         key.offset < context.stack_layout.frame_size);
    for (const auto& [s_idx, s_reg] : stores) {
      const bool is_intra_non_cs = intra_reload_spill_store_indices.contains(s_idx);
      const bool is_cross = !(*is_spill_overhead)[static_cast<size_t>(s_idx)] && is_cross_live_out;
      const bool is_entry_arg_spill_cand =
          context.is_entry_block && !context.has_stack_addr_escape && is_local_spill_slot &&
          (is_intra_non_cs || is_cross_live_out) &&
          ((s_reg >= kMipsArgRegFirst && s_reg <= kMipsArgRegLast) ||
           (s_reg >= kMipsFpArgRegFirst && s_reg <= kMipsFpArgRegFirst + 3));
      const bool is_inherited_intra =
          !is_intra_non_cs && !is_cross && !(*is_spill_overhead)[static_cast<size_t>(s_idx)] &&
          is_local_spill_slot &&
          HasInheritedSpillSlotDeadConsumer(post_reload_block, s_idx, s_reg, key, mm, fp_needed);
      if (!is_intra_non_cs && !is_cross && !is_entry_arg_spill_cand && !is_inherited_intra) {
        continue;
      }
      if (s_reg < 1 || s_reg >= kMipsFirstPseudoRegister ||
          IsFixedOrSpecialHardReg(s_reg, fp_needed) ||
          !RegValueDoesNotReachBlockEnd(post_reload_block, s_idx, s_reg, key.mode, mm,
                                        live_at_end_s2)) {
        continue;
      }
      const int s_nregs = std::max(1, mm.HardRegNRegs(s_reg, key.mode));
      int prod_idx = -1;
      for (int p = s_idx - 1; p >= 0; --p) {
        if ((*is_spill_overhead)[static_cast<size_t>(p)]) {
          continue;
        }
        const Insn& cand = post_reload_block[static_cast<size_t>(p)];
        if (cand.kind == Insn::Kind::kCall || cand.kind == Insn::Kind::kJump) {
          break;
        }
        if (auto m_acc = TryParsePlainStackSlotMove(cand, fp_needed);
            m_acc.has_value() && m_acc->key.offset == key.offset) {
          break;
        }
        const auto md_it = md_output_reload_dest.find(p);
        if (md_it != md_output_reload_dest.end() && md_it->second.op == RtlOp::kReg &&
            md_it->second.RegNo() == s_reg && md_it->second.mode == key.mode &&
            !md_it->second.flags.integrated) {
          prod_idx = p;
          break;
        }
        const RtlExpr* ps = SingleSetOf(cand.pn.pattern);
        if (ps != nullptr && ps->SetDest().op == RtlOp::kReg && ps->SetDest().RegNo() == s_reg &&
            ps->SetDest().mode == key.mode && !ps->SetDest().flags.integrated) {
          if (!TryParsePlainStackSlotMove(cand, fp_needed).has_value()) {
            prod_idx = p;
          }
          break;
        }
        absl::btree_set<int> p_fdefs;
        absl::btree_set<int> p_pdefs;
        CollectFullRegDefs(cand.pn.pattern, &p_fdefs, &p_pdefs);
        bool clobbers_sreg = false;
        for (int k = 0; k < s_nregs; ++k) {
          if (p_fdefs.contains(s_reg + k) || p_pdefs.contains(s_reg + k)) {
            clobbers_sreg = true;
            break;
          }
        }
        if (clobbers_sreg) {
          break;
        }
      }
      if (prod_idx >= 0 && !claimed_store_prods.contains(prod_idx)) {
        (*is_spill_overhead)[static_cast<size_t>(s_idx)] = true;
        (*output_reload_orig_owner)[s_idx] = prod_idx;
        claimed_store_prods.insert(prod_idx);
        folded_stores_by_off[key.offset].push_back({s_idx, prod_idx, false, key.mode});
        if (is_intra_non_cs) {
          folded_intra_store_offsets.insert(key.offset);
        }
        if (is_cross_live_out) {
          cross_live_out_offsets.insert(key.offset);
        }
      } else if (prod_idx < 0 && is_entry_arg_spill_cand) {
        // `function.c:assign_parms` + `reload1.c:2309-2400` (`alter_reg`):
        // a block-0 entry-parameter copy whose destination pseudo was spilled
        // to a stack slot lowers directly to `(set (mem:M [sp/fp+off]) (reg:M arg))`
        // with no prior producer in B0. Preserve it as a core `(set P_slot arg)`
        // copy rather than dropping or leaving it as an opaque stack store.
        bool clean_from_entry = true;
        for (int p = s_idx - 1; p >= 0; --p) {
          const Insn& cand = post_reload_block[static_cast<size_t>(p)];
          if (cand.kind == Insn::Kind::kCall || cand.kind == Insn::Kind::kJump) {
            clean_from_entry = false;
            break;
          }
          if (auto m_acc = TryParsePlainStackSlotMove(cand, fp_needed);
              m_acc.has_value() && m_acc->key.offset == key.offset) {
            clean_from_entry = false;
            break;
          }
          absl::btree_set<int> p_fdefs;
          absl::btree_set<int> p_pdefs;
          CollectFullRegDefs(cand.pn.pattern, &p_fdefs, &p_pdefs);
          for (int k = 0; k < s_nregs; ++k) {
            if (p_fdefs.contains(s_reg + k) || p_pdefs.contains(s_reg + k)) {
              clean_from_entry = false;
              break;
            }
          }
          if (!clean_from_entry) {
            break;
          }
        }
        if (clean_from_entry) {
          (*is_spill_overhead)[static_cast<size_t>(s_idx)] = false;
          folded_stores_by_off[key.offset].push_back({s_idx, s_idx, true, key.mode});
          if (is_intra_non_cs || slot_loads.contains(key)) {
            folded_intra_store_offsets.insert(key.offset);
          }
          if (is_cross_live_out) {
            cross_live_out_offsets.insert(key.offset);
          }
        }
      }
    }
  }

  // (b) Input-reload spill loads (`reload1.c:5641-6135`) and in-place spill-slot
  //     loads (`reload1.c:1968-1992` `PUT_CODE(reg, MEM)`): both intra-block
  //     non-caller-save spill loads and cross-block spill loads.
  for (const auto& [key, loads] : slot_loads) {
    const bool is_cross_from_other =
        is_cross_spill_slot_key(key) && context.other_block_spill_store_counts.contains(key.offset);
    if (is_cross_from_other) {
      cross_from_other_offsets.insert(key.offset);
    }
    const bool is_folded_intra = folded_intra_store_offsets.contains(key.offset);
    for (const auto& [l_idx, l_reg] : loads) {
      const bool is_intra_non_cs =
          intra_reload_spill_load_indices.contains(l_idx) && is_folded_intra;
      const bool is_cross =
          !(*is_spill_overhead)[static_cast<size_t>(l_idx)] && is_cross_from_other;
      if (!is_folded_intra && !is_cross) {
        continue;
      }
      if (l_reg < 1 || l_reg >= kMipsFirstPseudoRegister ||
          IsFixedOrSpecialHardReg(l_reg, fp_needed)) {
        continue;
      }
      const int l_nregs = std::max(1, mm.HardRegNRegs(l_reg, key.mode));
      int cons_idx = -1;
      bool feeds_call_or_use = false;
      for (int c = l_idx + 1; c < n; ++c) {
        if ((*is_spill_overhead)[static_cast<size_t>(c)]) {
          continue;
        }
        const Insn& cand = post_reload_block[static_cast<size_t>(c)];
        if (cand.kind == Insn::Kind::kCall) {
          absl::btree_set<int> fu_reads;
          for (const RtlExpr& fu : cand.pn.call_fusage) {
            CollectRegReads(fu, &fu_reads);
          }
          for (int k = 0; k < l_nregs; ++k) {
            if (fu_reads.contains(l_reg + k)) {
              feeds_call_or_use = true;
              break;
            }
          }
          break;
        }
        if (cand.pn.pattern.op == RtlOp::kUse) {
          absl::btree_set<int> use_reads;
          CollectRegReads(cand.pn.pattern, &use_reads);
          for (int k = 0; k < l_nregs; ++k) {
            if (use_reads.contains(l_reg + k)) {
              feeds_call_or_use = true;
              break;
            }
          }
          break;
        }
        if (auto m_acc = TryParsePlainStackSlotMove(cand, fp_needed);
            m_acc.has_value() && m_acc->is_store && m_acc->key.offset == key.offset) {
          break;
        }
        absl::btree_set<int> c_reads;
        CollectRegReads(cand.pn.pattern, &c_reads);
        bool reads_lreg = false;
        for (int k = 0; k < l_nregs; ++k) {
          if (c_reads.contains(l_reg + k)) {
            reads_lreg = true;
            break;
          }
        }
        if (reads_lreg) {
          if (c_reads.contains(l_reg) && !TryParsePlainStackSlotMove(cand, fp_needed).has_value() &&
              IsRegDeadAfterInsn(post_reload_block, c, l_reg, key.mode, mm, live_at_end_s2) &&
              !claimed_cons_lregs.contains({c, l_reg})) {
            cons_idx = c;
          }
          break;
        }
        absl::btree_set<int> c_fdefs;
        absl::btree_set<int> c_pdefs;
        CollectFullRegDefs(cand.pn.pattern, &c_fdefs, &c_pdefs);
        bool clobbers_lreg = false;
        for (int k = 0; k < l_nregs; ++k) {
          if (c_fdefs.contains(l_reg + k) || c_pdefs.contains(l_reg + k)) {
            clobbers_lreg = true;
            break;
          }
        }
        if (clobbers_lreg) {
          break;
        }
      }
      if (feeds_call_or_use) {
        (*is_spill_overhead)[static_cast<size_t>(l_idx)] = false;
        folded_loads.push_back(
            {key.offset, l_idx, -1, l_reg, key.mode, FoldedLoadRecord::Kind::kInPlace});
      } else if (cons_idx >= 0 && (is_folded_intra || is_cross)) {
        (*is_spill_overhead)[static_cast<size_t>(l_idx)] = true;
        claimed_cons_lregs.insert({cons_idx, l_reg});
        info.input_reload_orig_consumer[l_idx] = cons_idx;
        cons_slot_load_idx[{cons_idx, key.offset}] = l_idx;
        folded_loads.push_back({key.offset, l_idx, cons_idx, l_reg, key.mode,
                                FoldedLoadRecord::Kind::kFoldedSingleUse});
      } else if (is_intra_non_cs) {
        folded_loads.push_back(
            {key.offset, l_idx, -1, l_reg, key.mode, FoldedLoadRecord::Kind::kMultiUse});
      }
    }
  }

  // (c) Assign disjoint `SpillPseudoKey` versions per stack slot, mirroring
  //     `reload1.c:2305-2306, 2362-2367` (`alter_reg` reusing `spill_stack_slot[from_reg]`
  //     for all non-overlapping pseudos spilled from the same hard register).
  absl::btree_map<int64_t, absl::btree_map<int, int>> store_ver_by_off;
  auto prev_store_version = [&](int64_t slot_off, int before_idx) -> int {
    const auto oit = store_ver_by_off.find(slot_off);
    if (oit == store_ver_by_off.end()) {
      return -1;
    }
    auto it = oit->second.lower_bound(before_idx);
    if (it == oit->second.begin()) {
      return -1;
    }
    --it;
    return it->second;
  };
  for (auto& [slot_off, recs] : folded_stores_by_off) {
    std::sort(recs.begin(), recs.end(), [](const FoldedStoreRecord& a, const FoldedStoreRecord& b) {
      return a.s_idx < b.s_idx;
    });
    const bool is_cross_from_other = cross_from_other_offsets.contains(slot_off);
    const bool is_cross_live_out = cross_live_out_offsets.contains(slot_off);
    for (const FoldedStoreRecord& rec : recs) {
      int ver = rec.s_idx;
      if (!rec.is_direct_entry_arg) {
        if (const auto lit = cons_slot_load_idx.find({rec.prod_idx, slot_off});
            lit != cons_slot_load_idx.end() && lit->second < rec.s_idx) {
          ver = prev_store_version(slot_off, lit->second);
        } else if (is_cross_from_other && is_cross_live_out && recs.size() == 1) {
          ver = -1;
        }
      }
      store_ver_by_off[slot_off][rec.s_idx] = ver;
      const SpillPseudoKey sp_key{slot_off, ver};
      if (rec.is_direct_entry_arg) {
        info.direct_entry_arg_spill_slot_by_store[rec.s_idx] = {sp_key, rec.mode};
      } else {
        info.store_slot_by_prod[rec.prod_idx] = sp_key;
      }
    }
    if (is_cross_live_out && !recs.empty()) {
      const int last_ver = store_ver_by_off[slot_off][recs.back().s_idx];
      info.live_out_slot_keys.insert({slot_off, last_ver});
    }
  }
  for (const FoldedLoadRecord& lrec : folded_loads) {
    const SpillPseudoKey sp_key{lrec.slot_off, prev_store_version(lrec.slot_off, lrec.l_idx)};
    if (lrec.kind == FoldedLoadRecord::Kind::kInPlace) {
      info.in_place_spill_load_slot_by_orig[lrec.l_idx] = {sp_key, lrec.mode};
    } else if (lrec.kind == FoldedLoadRecord::Kind::kFoldedSingleUse) {
      info.load_subs_by_cons[lrec.cons_idx][lrec.l_reg] = {sp_key, lrec.mode};
    } else {
      info.multi_use_load_slot_by_orig[lrec.l_idx] = {lrec.l_reg, sp_key};
    }
  }
  return info;
}

struct IntraBlockSpillSlotClassification {
  absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>> slot_stores;
  absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>> slot_loads;
  absl::btree_map<int, int> spill_store_reg;
  std::vector<std::tuple<int, int, int>> preserved_across_call_ranges;
  absl::btree_set<int> caller_save_store_indices;
  absl::btree_set<int> intra_reload_spill_store_indices;
  absl::btree_set<int> intra_reload_spill_load_indices;
};

// Scans plain stack-slot moves in `post_reload_block`, separating
// `caller-save.c:480-580` (`insert_save`/`insert_restore`) same-reg across-call
// pairs from `reload1.c:2309-2400, 5641-6160` intra-block spill store/load
// pairs (crossing a call, or leaf register-file saturation where
// `local-alloc.c:2140-2181` / `global.c:345-360` allocated a callee-saved reg
// of the same class before spilling any pseudo to the stack, `mips.h:1203`).
// Mirrors `reload1.c:657-658, 2352-2358` (`alter_reg` -> `assign_stack_local(..., -1)`
// with MIPS `BIGGEST_ALIGNMENT == 8` and big-endian offset `slot_base + (8 - sz)`)
// and `reload1.c:1445-1446` + `caller-save.c:315-316` (`setup_save_areas` ->
// `assign_stack_local(..., 0)` at 4-byte alignment AFTER initial `alter_reg`):
// returns the lowest stack offset in `[layout.starting_frame_offset, layout.frame_size)`
// that can hold a `reload` spill slot (the contiguous upper 8-byte suffix above
// any pre-reload local aggregate, `REG_EQUIV` stack slot, or non-big-endian-aligned
// access such as `SImode` at `slot_base + 0` inside a local `s32[]` array),
// excluding `caller-save.c:315-316` call-used save/restore slots.
int64_t ComputeMinValidReloadSpillOffset(absl::Span<const Insn> post_reload_block,
                                         const PostReloadStackLayout& layout,
                                         const MachineModel& mm, bool frame_pointer_needed) {
  if (!layout.valid || layout.frame_size <= layout.starting_frame_offset) {
    return layout.valid ? layout.starting_frame_offset : 0;
  }
  const int64_t min_off = layout.starting_frame_offset;
  const int64_t raw_max_off = (layout.callee_saved_area_offset >= 0)
                                  ? std::min(layout.frame_size, layout.callee_saved_area_offset)
                                  : layout.frame_size;
  const int64_t max_off =
      (raw_max_off > min_off) ? (min_off + 8 * ((raw_max_off - min_off) / 8)) : min_off;
  const int num_slots = static_cast<int>((max_off - min_off) / 8);
  if (num_slots <= 0) {
    return max_off;
  }
  // Exclude `caller-save.c:315-316` save/restore slots (allocated after
  // `reload1.c:657-658` `alter_reg` at natural 4-byte alignment) so consecutive
  // 4-byte caller-save slots above an `alter_reg` spill slot do not look like a
  // pre-reload local array.
  absl::btree_set<int64_t> caller_save_offsets;
  const bool has_call =
      std::any_of(post_reload_block.begin(), post_reload_block.end(),
                  [](const Insn& insn) { return insn.kind == Insn::Kind::kCall; });
  if (has_call) {
    struct CallerSaveSlotInfo {
      bool ok = true;
      int regno = -1;
      bool saw_store = false;
      bool saw_load = false;
      bool saw_store_before_call = false;
      bool saw_load_after_call = false;
    };
    absl::btree_map<int64_t, CallerSaveSlotInfo> cs_info;
    bool seen_any_call = false;
    for (const Insn& insn : post_reload_block) {
      if (insn.kind == Insn::Kind::kCall) {
        seen_any_call = true;
        for (auto& [off, info] : cs_info) {
          if (info.saw_store) {
            info.saw_store_before_call = true;
          }
        }
        continue;
      }
      for (const RtlRegNote& note : insn.pn.reg_notes) {
        if ((note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegDead) &&
            note.datum.op == RtlOp::kMem && !note.datum.children.empty()) {
          int base = -1;
          int64_t off = 0;
          if (ParseStackBaseOffset(note.datum.children[0], &base, &off, frame_pointer_needed) &&
              off >= min_off && off < max_off) {
            cs_info[off].ok = false;
          }
        }
      }
      const RtlExpr& pat = insn.pn.pattern;
      if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
        int base = -1;
        int64_t off = 0;
        if (pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
            !pat.SetDest().flags.volatil && !pat.SetDest().flags.in_struct &&
            !pat.SetDest().flags.unchanging && pat.SetSrc().op == RtlOp::kReg &&
            ParseStackBaseOffset(pat.SetDest().children[0], &base, &off, frame_pointer_needed) &&
            off >= min_off && off < max_off) {
          const int r = pat.SetSrc().RegNo();
          CallerSaveSlotInfo& info = cs_info[off];
          if (r >= 0 && r < kMipsFirstPseudoRegister && mm.CallUsed(r) &&
              (info.regno < 0 || info.regno == r)) {
            info.regno = r;
            info.saw_store = true;
          } else {
            info.ok = false;
          }
          continue;
        }
        if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kMem &&
            !pat.SetSrc().children.empty() && !pat.SetSrc().flags.volatil &&
            !pat.SetSrc().flags.in_struct && !pat.SetSrc().flags.unchanging &&
            ParseStackBaseOffset(pat.SetSrc().children[0], &base, &off, frame_pointer_needed) &&
            off >= min_off && off < max_off) {
          const int r = pat.SetDest().RegNo();
          CallerSaveSlotInfo& info = cs_info[off];
          if (r >= 0 && r < kMipsFirstPseudoRegister && mm.CallUsed(r) &&
              (info.regno < 0 || info.regno == r)) {
            info.regno = r;
            info.saw_load = true;
            if (seen_any_call) {
              info.saw_load_after_call = true;
            }
          } else {
            info.ok = false;
          }
          continue;
        }
      }
      pat.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kMem && !node.children.empty()) {
          int base = -1;
          int64_t off = 0;
          if (ParseStackBaseOffset(node.children[0], &base, &off, frame_pointer_needed) &&
              off >= min_off && off < max_off) {
            cs_info[off].ok = false;
          }
        }
      });
    }
    for (const auto& [off, info] : cs_info) {
      if (info.ok && info.saw_store && info.saw_load && info.saw_store_before_call &&
          info.saw_load_after_call) {
        caller_save_offsets.insert(off);
      }
    }
  }
  struct SlotCheck {
    bool hard_invalidated = false;
    bool non_bigend_off = false;
    bool has_access = false;
    RtlMachineMode mode = RtlMachineMode::kVoid;
    int64_t exact_offset = -1;
  };
  std::vector<SlotCheck> slots(static_cast<size_t>(num_slots));
  auto record_mem = [&](const RtlExpr& mem, bool force_invalidate) {
    if (mem.op != RtlOp::kMem || mem.children.empty()) {
      return;
    }
    int base = -1;
    int64_t off = 0;
    if (!ParseStackBaseOffset(mem.children[0], &base, &off, frame_pointer_needed) ||
        off < min_off || off >= max_off) {
      return;
    }
    if (!force_invalidate && caller_save_offsets.contains(off)) {
      return;
    }
    const int sz = std::max(1, ModeByteSize(mem.mode));
    const int64_t rel_lo = off - min_off;
    const int64_t rel_hi = rel_lo + sz;
    const int first_s = static_cast<int>(rel_lo / 8);
    const int last_s = static_cast<int>(std::min<int64_t>(num_slots - 1, (rel_hi - 1) / 8));
    if (force_invalidate || mem.flags.volatil || mem.flags.in_struct ||
        mem.mode == RtlMachineMode::kBlk || sz > 8 || first_s != last_s) {
      for (int s = std::max(0, first_s); s <= last_s; ++s) {
        slots[static_cast<size_t>(s)].hard_invalidated = true;
      }
      return;
    }
    const int64_t expected_off = min_off + 8 * int64_t{first_s} + (8 - sz);
    SlotCheck& sc = slots[static_cast<size_t>(first_s)];
    if (off != expected_off) {
      sc.non_bigend_off = true;
    }
    if (!sc.has_access) {
      sc.has_access = true;
      sc.mode = mem.mode;
      sc.exact_offset = off;
    } else if (sc.mode != mem.mode || sc.exact_offset != off) {
      sc.hard_invalidated = true;
    }
  };
  for (const Insn& insn : post_reload_block) {
    insn.pn.pattern.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kMem) {
        record_mem(node, /*force_invalidate=*/false);
      }
    });
    for (const RtlRegNote& note : insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegEquiv && note.datum.op == RtlOp::kMem) {
        record_mem(note.datum, /*force_invalidate=*/true);
      }
    }
  }
  bool any_hard_invalidated = false;
  for (int s = 0; s < num_slots; ++s) {
    if (slots[static_cast<size_t>(s)].hard_invalidated) {
      any_hard_invalidated = true;
      break;
    }
  }
  int non_inv_begin = num_slots;
  while (non_inv_begin > 0) {
    const SlotCheck& sc = slots[static_cast<size_t>(non_inv_begin - 1)];
    if (sc.hard_invalidated || (any_hard_invalidated && sc.non_bigend_off)) {
      break;
    }
    --non_inv_begin;
  }
  return min_off + 8 * int64_t{non_inv_begin};
}

IntraBlockSpillSlotClassification ClassifyIntraBlockSpillAndCallerSaveSlots(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm, bool has_stack_addr_escape,
    std::vector<bool>* is_spill_overhead, bool frame_pointer_needed = true,
    const PostReloadStackLayout& stack_layout = {}) {
  IntraBlockSpillSlotClassification out;
  const int n = static_cast<int>(post_reload_block.size());
  const int64_t min_valid_reload_spill_off = std::max(
      ComputeMinValidReloadSpillOffset(post_reload_block, stack_layout, mm, frame_pointer_needed),
      stack_layout.valid ? stack_layout.min_valid_reload_spill_offset : int64_t{0});
  absl::btree_set<int64_t> mem_reg_dead_slot_offsets;
  for (int i = 0; i < n; ++i) {
    for (const RtlRegNote& note : post_reload_block[static_cast<size_t>(i)].pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem &&
          !note.datum.children.empty()) {
        int base = -1;
        int64_t off = 0;
        if (ParseStackBaseOffset(note.datum.children[0], &base, &off, frame_pointer_needed)) {
          mem_reg_dead_slot_offsets.insert(off);
        }
      }
    }
    if ((*is_spill_overhead)[static_cast<size_t>(i)]) {
      continue;
    }
    if (auto acc = TryParsePlainStackSlotMove(post_reload_block[static_cast<size_t>(i)],
                                              frame_pointer_needed);
        acc.has_value()) {
      if (acc->is_store) {
        out.slot_stores[acc->key].push_back({i, acc->regno});
        out.spill_store_reg[i] = acc->regno;
      } else {
        out.slot_loads[acc->key].push_back({i, acc->regno});
      }
    }
  }
  for (const auto& [key, stores] : out.slot_stores) {
    const auto lit = out.slot_loads.find(key);
    if (lit == out.slot_loads.end() || stores.empty()) {
      continue;
    }
    const bool valid_reload_spill_key =
        !stack_layout.valid || mem_reg_dead_slot_offsets.contains(key.offset) ||
        (key.offset >= min_valid_reload_spill_off && key.offset < stack_layout.frame_size);
    const int first_store_idx = stores.front().first;
    bool has_following_load = false;
    for (const auto& [l_idx, l_reg] : lit->second) {
      if (l_idx > first_store_idx) {
        has_following_load = true;
        break;
      }
    }
    if (!has_following_load) {
      continue;
    }
    for (const auto& [s_idx, s_reg] : stores) {
      for (const auto& [l_idx, l_reg] : lit->second) {
        if (s_idx >= l_idx) {
          continue;
        }
        bool saw_call = false;
        bool redef_in_span = false;
        bool s_reg_clobbered_in_span = false;
        bool saw_callee_saved_in_class = false;
        const bool is_fp_slot = (s_reg >= kMipsFpRegFirst && s_reg <= kMipsFpRegLast);
        for (int k = s_idx + 1; k < l_idx; ++k) {
          const Insn& mid = post_reload_block[static_cast<size_t>(k)];
          if (mid.kind == Insn::Kind::kCall) {
            saw_call = true;
            continue;
          }
          absl::btree_set<int> mf;
          absl::btree_set<int> mp;
          CollectFullRegDefs(mid.pn.pattern, &mf, &mp);
          if (mf.contains(s_reg) || mp.contains(s_reg)) {
            s_reg_clobbered_in_span = true;
            if (s_reg == l_reg) {
              redef_in_span = true;
            }
          }
          for (int d : mf) {
            if (!IsFixedOrSpecialHardReg(d, frame_pointer_needed) && d > 0 &&
                d < kMipsFirstPseudoRegister &&
                ((d >= kMipsFpRegFirst && d <= kMipsFpRegLast) == is_fp_slot) && !mm.CallUsed(d)) {
              saw_callee_saved_in_class = true;
            }
          }
        }
        // `caller-save.c:410-462` (`save_call_clobbered_regs` /
        // `restore_referenced_regs`) only saves call-used hard regs that stay
        // live (unredefined) until restored and never carry a `reload1.c:2309`
        // (`alter_reg`) `(REG_DEAD (mem [sp/fp+off]))` spilled-pseudo note.
        if (s_reg == l_reg && mm.CallUsed(s_reg) && saw_call && !redef_in_span &&
            !mem_reg_dead_slot_offsets.contains(key.offset)) {
          (*is_spill_overhead)[static_cast<size_t>(s_idx)] = true;
          (*is_spill_overhead)[static_cast<size_t>(l_idx)] = true;
          out.preserved_across_call_ranges.push_back({s_idx, l_idx, s_reg});
          out.caller_save_store_indices.insert(s_idx);
        } else if (valid_reload_spill_key) {
          (*is_spill_overhead)[static_cast<size_t>(s_idx)] = true;
          (*is_spill_overhead)[static_cast<size_t>(l_idx)] = true;
          // Per `reload1.c:2309-2400, 5641-6160` (`alter_reg` /
          // `emit_reload_insns`), whenever an intra-block reload spill store/load
          // pair is folded out of the `sched1` view and carries a proven spill
          // death note or clobbers `s_reg` before reloading into `l_reg`,
          // `FoldReloadSpillsForSched1View` must remap the store producer and
          // load consumer onto a common spilled pseudo `P_slot` so pre-reload
          // data-dependence chains are preserved (excluding trivial adjacent
          // leaf stack-struct copies with no intervening clobber).
          const bool proven_intra_spill =
              mem_reg_dead_slot_offsets.contains(key.offset) || s_reg_clobbered_in_span;
          if (saw_call || (proven_intra_spill && (s_reg != l_reg || redef_in_span)) ||
              (!has_stack_addr_escape && saw_callee_saved_in_class)) {
            out.intra_reload_spill_store_indices.insert(s_idx);
            out.intra_reload_spill_load_indices.insert(l_idx);
          }
        }
      }
    }
  }
  return out;
}

bool ExtractBaseMemOffset(const RtlExpr& mem, int base_regno, bool is_argp, int64_t* off_out) {
  if (mem.op != RtlOp::kMem || mem.children.empty()) {
    return false;
  }
  const RtlExpr& addr = mem.children[0];
  auto matches_base = [&](const RtlExpr& e) {
    if (e.op != RtlOp::kReg) {
      return false;
    }
    if (is_argp) {
      return e.IsArgPointerReg();
    }
    return !e.IsArgPointerReg() && e.RegNo() == base_regno;
  };
  if (matches_base(addr)) {
    *off_out = 0;
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && matches_base(addr.children[0]) &&
      addr.children[1].op == RtlOp::kConstInt) {
    *off_out = addr.children[1].int_val;
    return true;
  }
  return false;
}

// Normalizes a `MEM` expression's stack base (`$sp`/`$fp`/`argp`) to a common
}  // namespace

namespace detail {

// pre-reload representation (`mips.h:1649-1651, 1756-1764`, `reload1.c:2641`
// `eliminate_regs`) so post-reload `$sp`-eliminated `MEM` operands preserve
// distinct pre-reload `$fp`/`argp`/`$sp` base-register aliasing in `sched1` and
// compare equal to un-eliminated `REG_EQUIV` note datums.
RtlExpr NormalizeMemForRegEquivMatch(const RtlExpr& mem, const PostReloadStackLayout& layout) {
  if (!layout.valid || mem.op != RtlOp::kMem || mem.children.empty()) {
    return mem;
  }
  const int elim_to_reg =
      layout.frame_pointer_needed ? kMipsFramePointerRegno : kMipsStackPointerRegno;
  int64_t off = 0;
  if (!ExtractBaseMemOffset(mem, elim_to_reg, /*is_argp=*/false, &off)) {
    return mem;
  }
  RtlExpr out = mem;
  if (off >= layout.frame_size) {
    const int64_t arg_off = off - layout.frame_size;
    out.children[0] = (arg_off == 0) ? RtlExpr::MakeArgPointerReg()
                                     : RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi,
                                                           RtlExpr::MakeArgPointerReg(),
                                                           RtlExpr::MakeConstInt(arg_off));
    return out;
  }
  if (!layout.frame_pointer_needed && (off >= layout.starting_frame_offset || off < 0)) {
    out.children[0] =
        (off == 0)
            ? RtlExpr::MakeReg(RtlMachineMode::kSi, kMipsFramePointerRegno)
            : RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi,
                                  RtlExpr::MakeReg(RtlMachineMode::kSi, kMipsFramePointerRegno),
                                  RtlExpr::MakeConstInt(off));
    return out;
  }
  return out;
}

}  // namespace detail

namespace {

void UneliminateStackMemsForSched1(RtlExpr* expr, const PostReloadStackLayout& layout) {
  if (!layout.valid || expr == nullptr) {
    return;
  }
  if (expr->op == RtlOp::kMem) {
    *expr = NormalizeMemForRegEquivMatch(*expr, layout);
    return;
  }
  for (RtlExpr& ch : expr->children) {
    UneliminateStackMemsForSched1(&ch, layout);
  }
}

// Derives `live_at_end_s1` and `facts_s1` (`reg_n_sets`, `reg_n_calls_crossed`,
// `max_reg_num`) for a projected pre-reload `Sched1ProjectionView`
// (`flow.c:2065`, `sched.c:1732, 2469-2501`).
// Mirrors `flow.c:1916-1958, 2035-2049, 2065` (`mark_set_regs` / `mark_set_1`):
// increments `reg_n_sets` for every `SET`/`CLOBBER` destination in `pat`,
// expanding multi-register hard-register destinations across
// `[regno, regno + HARD_REGNO_NREGS(regno, mode))` so `sched.c:2469-2501`
// (`birthing_insn_p`) observes exact `reg_n_sets[i]` for sub-registers of
// wide hard-register writes (e.g., `DFmode`/`DImode` argument registers).
void CountFlowMarkSet1RegNSets(const RtlExpr& pat, const MachineModel& mm,
                               absl::btree_map<int, int>* reg_n_sets) {
  auto count_one = [&](const RtlExpr& elem) {
    if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
      return;
    }
    const RtlExpr* reg = &elem.children[0];
    while ((reg->op == RtlOp::kSubreg || reg->op == RtlOp::kZeroExtract ||
            reg->op == RtlOp::kSignExtract || reg->op == RtlOp::kStrictLowPart) &&
           !reg->children.empty()) {
      reg = &reg->children[0];
    }
    if (reg->op != RtlOp::kReg) {
      return;
    }
    const int regno = reg->RegNo();
    if (regno < 0) {
      return;
    }
    if (regno < kMipsFirstPseudoRegister) {
      const int nregs = std::max(1, mm.HardRegNRegs(regno, reg->mode));
      for (int k = 0; k < nregs && regno + k < kMipsFirstPseudoRegister; ++k) {
        (*reg_n_sets)[regno + k] += 1;
      }
    } else {
      (*reg_n_sets)[regno] += 1;
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      count_one(ch);
    }
  } else {
    count_one(pat);
  }
}

void DeriveSched1ViewFacts(const Sched1ProjectionContext& context, const MachineModel& mm,
                           const absl::btree_set<int>& live_at_end_s2,
                           const absl::btree_map<int, int>& cur_pseudo,
                           const absl::btree_set<int>& live_in_pseudos,
                           const absl::btree_map<int, int>& live_in_reg_for_hard_reg,
                           const absl::btree_map<SpillPseudoKey, int>& spill_slot_pseudo,
                           const absl::btree_set<int>& spill_live_out_pseudos, int next_pseudo,
                           Sched1ProjectionView* view) {
  const int m = static_cast<int>(view->core_insns.size());
  for (int r : live_at_end_s2) {
    if (const auto it = cur_pseudo.find(r); it != cur_pseudo.end()) {
      view->live_at_end_s1.insert(it->second);
    } else {
      view->live_at_end_s1.insert(r);
    }
  }
  // Mirrors `sched.c:2325-2367, 2469-2501`: trailing `(use (reg/i ...))`
  // return-value markers at `[bounds.epilogue_begin .. n)` make the return
  // register live at the end of the schedulable body for `birthing_insn_p`.
  for (int r : context.epilogue_use_regs) {
    if (const auto it = cur_pseudo.find(r); it != cur_pseudo.end()) {
      view->live_at_end_s1.insert(it->second);
    } else {
      view->live_at_end_s1.insert(r);
    }
  }
  for (int sp_p : spill_live_out_pseudos) {
    view->live_at_end_s1.insert(sp_p);
  }
  view->facts_s1.max_reg_num = next_pseudo;

  absl::btree_map<int, int> first_touch;
  absl::btree_map<int, int> last_touch;
  absl::btree_map<int, RtlMachineMode> view_reg_mode;
  std::vector<int> call_indices;
  for (int ci = 0; ci < m; ++ci) {
    const Insn& insn = view->core_insns[static_cast<size_t>(ci)];
    if (insn.kind == Insn::Kind::kCall) {
      call_indices.push_back(ci);
    }
    insn.pn.pattern.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kReg && node.RegNo() >= kMipsFirstPseudoRegister &&
          node.mode != RtlMachineMode::kVoid) {
        view_reg_mode.try_emplace(node.RegNo(), node.mode);
      }
    });
    CountFlowMarkSet1RegNSets(insn.pn.pattern, mm, &view->facts_s1.reg_n_sets);
    absl::btree_set<int> full_defs;
    absl::btree_set<int> partial_defs;
    CollectFullRegDefs(insn.pn.pattern, &full_defs, &partial_defs);
    for (int r : full_defs) {
      first_touch.try_emplace(r, ci);
      last_touch[r] = ci;
    }
    for (int r : partial_defs) {
      first_touch.try_emplace(r, ci);
      last_touch[r] = ci;
    }
    absl::btree_set<int> reads;
    CollectRegReads(insn.pn.pattern, &reads);
    for (int r : reads) {
      first_touch.try_emplace(r, ci);
      last_touch[r] = ci;
    }
  }
  for (int r : live_in_pseudos) {
    first_touch[r] = -1;
  }
  if (!context.has_cfg_context) {
    for (int r : live_in_pseudos) {
      // Standalone single-block fallback: assume live-in pseudos were defined in a predecessor.
      view->facts_s1.reg_n_sets[r] += 1;
    }
  } else {
    // Whole-function `flow.c` / `sched.c:2469-2501` `reg_n_sets` accounting:
    // add cross-block reaching defs once per cross-block web (so incoming params with
    // 0 prior sets keep `reg_n_sets == 1`, while multi-block loop/join regs get `>= 2`).
    auto lookup_mode_map = [&](const absl::btree_map<std::pair<int, RtlMachineMode>, int>& m_map,
                               int hr, int view_reg) -> int {
      if (view_reg >= kMipsFirstPseudoRegister) {
        if (const auto mit = view_reg_mode.find(view_reg); mit != view_reg_mode.end()) {
          int sum = 0;
          if (const auto it = m_map.find({hr, mit->second}); it != m_map.end()) {
            sum += it->second;
          }
          if (const auto wit = m_map.find({hr, RtlMachineMode::kVoid}); wit != m_map.end()) {
            sum += wit->second;
          }
          return sum;
        }
      }
      int best = 0;
      for (const auto& [k, cnt] : m_map) {
        if (k.first == hr) {
          best = std::max(best, cnt);
        }
      }
      return best;
    };
    auto lookup_cross_block_reaching = [&](int hr, int view_reg, bool is_entry_endpoint) -> int {
      if (context.has_per_endpoint_web_defs) {
        return lookup_mode_map(
            is_entry_endpoint ? context.entry_web_other_defs : context.exit_web_other_defs, hr,
            view_reg);
      }
      if (context.has_reaching_defs_by_mode && view_reg >= kMipsFirstPseudoRegister) {
        return lookup_mode_map(context.cross_block_reaching_defs_by_mode, hr, view_reg);
      }
      const auto it = context.cross_block_reaching_defs.find(hr);
      return (it != context.cross_block_reaching_defs.end()) ? it->second : 0;
    };
    absl::btree_set<int> added_cross_block;
    for (const auto& [hr, view_reg] : live_in_reg_for_hard_reg) {
      if (const int reach = lookup_cross_block_reaching(hr, view_reg, /*is_entry_endpoint=*/true);
          reach > 0) {
        view->facts_s1.reg_n_sets[view_reg] += reach;
        added_cross_block.insert(view_reg);
      }
    }
    for (int hr : live_at_end_s2) {
      const auto pit = cur_pseudo.find(hr);
      const int view_reg = (pit != cur_pseudo.end()) ? pit->second : hr;
      if (!added_cross_block.contains(view_reg) && view->facts_s1.reg_n_sets.contains(view_reg)) {
        if (const int reach =
                lookup_cross_block_reaching(hr, view_reg, /*is_entry_endpoint=*/false);
            reach > 0) {
          view->facts_s1.reg_n_sets[view_reg] += reach;
          added_cross_block.insert(view_reg);
        }
      }
    }
    // `flow.c:2065` (`mark_set_1`) counts every explicit `SET` function-wide for
    // unrenamed hard regs (both `/i` return-value regs and ABI call-arg regs in
    // `abi_pinned_defs`), so `sched.c:2469-2501` (`birthing_insn_p`) sees the
    // true whole-function `reg_n_sets[hr]` for every unrenamed hard reg.
    std::vector<int> unrenamed_hard_defs;
    const bool fp_needed = !context.stack_layout.valid || context.stack_layout.frame_pointer_needed;
    for (const auto& [r, cnt] : view->facts_s1.reg_n_sets) {
      if (r >= 0 && r < kMipsFirstPseudoRegister && !IsFixedOrSpecialHardReg(r, fp_needed) &&
          !added_cross_block.contains(r)) {
        unrenamed_hard_defs.push_back(r);
      }
    }
    for (int hr : unrenamed_hard_defs) {
      if (const auto it = context.other_block_total_defs.find(hr);
          it != context.other_block_total_defs.end() && it->second > 0) {
        view->facts_s1.reg_n_sets[hr] += it->second;
        added_cross_block.insert(hr);
      }
    }
    for (const auto& [slot_key, sp_p] : spill_slot_pseudo) {
      if (slot_key.second == -1 || spill_live_out_pseudos.contains(sp_p)) {
        if (const auto it = context.other_block_spill_store_counts.find(slot_key.first);
            it != context.other_block_spill_store_counts.end() && it->second > 0) {
          view->facts_s1.reg_n_sets[sp_p] += it->second;
        }
      }
    }
  }
  for (int r : view->live_at_end_s1) {
    last_touch[r] = m;
  }
  for (const auto& [r, first_ci] : first_touch) {
    if (r < kMipsFirstPseudoRegister) {
      continue;
    }
    const int last_ci = last_touch[r];
    int crossed = 0;
    for (int call_ci : call_indices) {
      if (first_ci < call_ci && call_ci < last_ci) {
        ++crossed;
      }
    }
    if (crossed > 0) {
      view->facts_s1.reg_n_calls_crossed[r] = crossed;
    }
  }
}

// Block-0 leading entry-parameter copy pinning (`sched.c:3191-3215`):
// excludes leading `(set dst src)` insns whose unwrapped `src` is a hard reg
// from `view->core_insns` (after recording their `facts_s1` defs) so
// `Schedule`/`IsAchievable` matches `schedule_block`'s advanced `head`.
void PinLeadingEntryParamCopiesForSched1View(Sched1ProjectionView* view) {
  if (view->core_insns.size() <= 1) {
    return;
  }
  absl::btree_set<int> word_copied_args;
  size_t pinned_count = 0;
  while (pinned_count + 1 < view->core_insns.size()) {
    const Insn& ci_insn = view->core_insns[pinned_count];
    if (ci_insn.kind != Insn::Kind::kInsn || ci_insn.pn.pattern.op != RtlOp::kSet ||
        ci_insn.pn.pattern.children.size() < 2) {
      break;
    }
    const RtlExpr& pat = ci_insn.pn.pattern;
    const RtlExpr* src = &pat.SetSrc();
    while ((src->op == RtlOp::kSubreg || src->op == RtlOp::kSignExtend ||
            src->op == RtlOp::kZeroExtend || src->op == RtlOp::kSignExtract ||
            src->op == RtlOp::kZeroExtract) &&
           !src->children.empty()) {
      src = &src->children[0];
    }
    if (src->op != RtlOp::kReg || src->RegNo() >= kMipsFirstPseudoRegister) {
      break;
    }
    if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kReg) {
      const RtlExpr& dst = pat.SetDest();
      const RtlExpr& s = pat.SetSrc();
      const int sr = s.RegNo();
      if ((s.mode == RtlMachineMode::kSi || s.mode == RtlMachineMode::kDi) &&
          sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast) {
        word_copied_args.insert(sr);
      } else if ((dst.mode == RtlMachineMode::kQi || dst.mode == RtlMachineMode::kHi) &&
                 (s.mode == RtlMachineMode::kQi || s.mode == RtlMachineMode::kHi) &&
                 sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast &&
                 !word_copied_args.contains(sr)) {
        break;
      }
    }
    ++pinned_count;
  }
  if (pinned_count == 0) {
    return;
  }
  view->pinned_prefix_orig_indices.assign(
      view->core_to_orig_index.begin(),
      view->core_to_orig_index.begin() + static_cast<ptrdiff_t>(pinned_count));
  view->core_insns.erase(view->core_insns.begin(),
                         view->core_insns.begin() + static_cast<ptrdiff_t>(pinned_count));
  view->core_to_orig_index.erase(
      view->core_to_orig_index.begin(),
      view->core_to_orig_index.begin() + static_cast<ptrdiff_t>(pinned_count));
  for (size_t j = 0; j < view->core_insns.size(); ++j) {
    view->core_insns[j].luid = static_cast<int>(j);
  }
  const int pcount_i = static_cast<int>(pinned_count);
  absl::btree_map<int, int> shifted_output_reload;
  for (const auto& [orig_i, core_i] : view->output_reload_after_core) {
    if (core_i >= pcount_i) {
      shifted_output_reload[orig_i] = core_i - pcount_i;
    } else {
      view->pinned_prefix_orig_indices.push_back(orig_i);
    }
  }
  view->output_reload_after_core = std::move(shifted_output_reload);
  absl::btree_map<int, int> shifted_input_reload;
  for (const auto& [orig_i, core_i] : view->input_reload_before_core) {
    if (core_i >= pcount_i) {
      shifted_input_reload[orig_i] = core_i - pcount_i;
    } else {
      view->pinned_prefix_orig_indices.push_back(orig_i);
    }
  }
  view->input_reload_before_core = std::move(shifted_input_reload);
}

// Identifies non-entry-block hard registers that represent a single
// loop-carried / cross-block multi-def user variable (`flow.c:2065`
// `mark_set_1`, `sched.c:2469-2501` `birthing_insn_p`, `global.c:find_reg`),
// mirroring Stage 4's `CoalesceSameBlockDisjointGlobalChainPseudos`: live-in
// (read before first write), live-out (`live_at_end_s2`), and redefined in the
// block with uniform mode and without `REG_EQUIV`/`REG_EQUAL`, self-`REG_DEAD`,
// ABI pinning, or intervening call clobbers.
absl::btree_set<int> FindCoalescedGlobalCarrierHardRegs(
    absl::Span<const Insn> post_reload_block, absl::Span<const int> core_to_orig_index,
    const std::vector<absl::btree_set<int>>& abi_pinned_defs,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context,
    const MachineModel& mm) {
  absl::btree_set<int> result;
  if (!context.has_cfg_context || context.is_entry_block) {
    return result;
  }
  const int m = static_cast<int>(core_to_orig_index.size());
  bool has_call = false;
  for (int ci = 0; ci < m; ++ci) {
    if (post_reload_block[static_cast<size_t>(core_to_orig_index[ci])].kind == Insn::Kind::kCall) {
      has_call = true;
      break;
    }
  }
  const bool fp_needed = !context.stack_layout.valid || context.stack_layout.frame_pointer_needed;
  for (int r : live_at_end_s2) {
    if (r < 2 || r >= kMipsFirstPseudoRegister || IsFixedOrSpecialHardReg(r, fp_needed) ||
        (has_call && mm.CallUsed(r))) {
      continue;
    }
    if (context.has_per_endpoint_web_defs && !context.entry_exit_same_web_regs.contains(r)) {
      continue;
    }
    bool saw_read_before_def = false;
    bool saw_reg_dead = false;
    bool saw_def_after_reg_dead = false;
    bool saw_def = false;
    bool disqualified = false;
    RtlMachineMode expected_mode = RtlMachineMode::kVoid;
    for (int ci = 0; ci < m && !disqualified; ++ci) {
      const Insn& insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
      if (insn.kind == Insn::Kind::kCall || insn.pn.pattern.op == RtlOp::kUse) {
        continue;
      }
      absl::btree_set<int> reads;
      CollectRegReads(insn.pn.pattern, &reads);
      if (reads.contains(r) && !saw_def) {
        saw_read_before_def = true;
      }
      insn.pn.pattern.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kReg && node.RegNo() == r && node.mode != RtlMachineMode::kVoid) {
          if (expected_mode == RtlMachineMode::kVoid) {
            expected_mode = node.mode;
          } else if (expected_mode != node.mode) {
            disqualified = true;
          }
        }
      });
      absl::btree_set<int> fdefs;
      absl::btree_set<int> pdefs;
      CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
      if (pdefs.contains(r)) {
        disqualified = true;
        break;
      }
      if (fdefs.contains(r)) {
        if (!saw_read_before_def || abi_pinned_defs[static_cast<size_t>(ci)].contains(r) ||
            HasRegDeadNoteCovering(insn.pn.reg_notes, r, mm)) {
          disqualified = true;
          break;
        }
        const RtlExpr* s = SingleSetOf(insn.pn.pattern);
        if (s == nullptr || s->SetDest().op != RtlOp::kReg || s->SetDest().RegNo() != r ||
            s->SetDest().flags.integrated) {
          disqualified = true;
          break;
        }
        for (const RtlRegNote& n : insn.pn.reg_notes) {
          if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual ||
              n.kind == RtlMachineMode::kRegNoConflict) {
            disqualified = true;
            break;
          }
        }
        if (saw_reg_dead) {
          saw_def_after_reg_dead = true;
        }
        saw_def = true;
      } else if (reads.contains(r) && HasRegDeadNoteCovering(insn.pn.reg_notes, r, mm)) {
        saw_reg_dead = true;
      }
    }
    if (!disqualified && saw_read_before_def && saw_def && saw_def_after_reg_dead) {
      result.insert(r);
    }
  }
  return result;
}

void CollectDirectNonMemRegReads(const RtlExpr& x,
                                 std::vector<std::pair<int, RtlMachineMode>>* regs) {
  if (x.op == RtlOp::kMem) {
    return;
  }
  if (x.op == RtlOp::kReg) {
    regs->push_back({x.RegNo(), x.mode});
    return;
  }
  for (const RtlExpr& child : x.children) {
    CollectDirectNonMemRegReads(child, regs);
  }
}

// Mirrors `local-alloc.c:1266-1297, 1369-1375, 1765-1820, 2145-2182` and
// `config/mips/mips.h:1280` (`MODES_TIEABLE_P`): when a local single-block
// `SET` of hard register `r` reads a different genuine unrenamed physical hard
// register `s != r` (either an entry-block incoming ABI argument or a prior
// unrenamed hard-reg def in this block, never a cross-block global pseudo per
// `local-alloc.c:1765`) of the same bank/width that dies at `ci` (`REG_DEAD s`),
// `combine_regs` records `s` in `qty_phys_sugg` / `qty_phys_copy_sugg` and
// `wipe_dead_reg` clears `s` from `regs_live_at[2 * ci]`. If `s` remains free
// across `r`'s local live range `(ci, death_ci)` and `death_ci` does not itself
// suggest `r`, `find_free_reg` (`just_try_suggested = 1`) would allocate `s`
// instead of `r`; Stage 4 (`InvertAlterReg`) therefore keeps `r` unrenamed on
// physical hard register `r`.
bool HasLocalDyingHardRegSuggestionConflict(
    absl::Span<const Insn> post_reload_block, absl::Span<const int> core_to_orig_index, int ci,
    int r, const absl::btree_set<int>& reads, const absl::btree_map<int, int>& read_map,
    const absl::btree_set<int>& unrenamed_hard_def_regs, bool is_entry_block,
    const absl::btree_set<int>& live_at_end_s2, const MachineModel& mm,
    bool frame_pointer_needed = true) {
  if (r < 0 || r >= kMipsFirstPseudoRegister || IsFixedOrSpecialHardReg(r, frame_pointer_needed) ||
      reads.contains(r)) {
    return false;
  }
  const Insn& def_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
  if (def_insn.kind == Insn::Kind::kCall) {
    return false;
  }
  const RtlExpr* sset = SingleSetOf(def_insn.pn.pattern);
  if (sset == nullptr || sset->SetDest().op != RtlOp::kReg || sset->SetDest().RegNo() != r) {
    return false;
  }
  for (const RtlRegNote& note : def_insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const RtlMachineMode r_mode = sset->SetDest().mode;
  const int r_nregs = std::max(1, mm.HardRegNRegs(r, r_mode));
  const bool r_is_fp = (r >= kMipsFpRegFirst && r < kMipsMdRegFirst);
  if (r >= kMipsMdRegFirst) {
    return false;
  }
  std::vector<std::pair<int, RtlMachineMode>> direct_srcs;
  CollectDirectNonMemRegReads(sset->SetSrc(), &direct_srcs);
  std::vector<std::pair<int, int>> cand_suggs;  // (s_reg, s_nregs)
  for (const auto& [s, s_mode] : direct_srcs) {
    if (s < 0 || s >= kMipsMdRegFirst || s == r ||
        IsFixedOrSpecialHardReg(s, frame_pointer_needed)) {
      continue;
    }
    if (read_map.contains(s)) {
      continue;
    }
    const bool is_genuine_hard_reg =
        unrenamed_hard_def_regs.contains(s) || (is_entry_block && IsAbiCallInterfaceHardReg(s));
    if (!is_genuine_hard_reg) {
      continue;
    }
    const bool s_is_fp = (s >= kMipsFpRegFirst);
    const int s_nregs = std::max(1, mm.HardRegNRegs(s, s_mode));
    if (s_is_fp != r_is_fp || s_nregs != r_nregs) {
      continue;
    }
    if (!HasRegDeadNoteCovering(def_insn.pn.reg_notes, s, mm)) {
      continue;
    }
    cand_suggs.push_back({s, s_nregs});
  }
  if (cand_suggs.empty()) {
    return false;
  }
  const int m = static_cast<int>(core_to_orig_index.size());
  int death_ci = -1;
  bool terminated_in_block = false;
  for (int k = ci + 1; k < m; ++k) {
    const Insn& k_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
    if (k_insn.kind == Insn::Kind::kCall) {
      absl::btree_set<int> fu_reads;
      for (const RtlExpr& fu : k_insn.pn.call_fusage) {
        CollectRegReads(fu, &fu_reads);
      }
      for (int w = 0; w < r_nregs; ++w) {
        if (fu_reads.contains(r + w)) {
          death_ci = k;
        }
      }
      if (mm.CallUsed(r)) {
        terminated_in_block = true;
        break;
      }
      continue;
    }
    absl::btree_set<int> k_reads;
    CollectRegReads(k_insn.pn.pattern, &k_reads);
    bool reads_r = false;
    for (int w = 0; w < r_nregs; ++w) {
      if (k_reads.contains(r + w)) {
        reads_r = true;
        break;
      }
    }
    absl::btree_set<int> k_fdefs;
    absl::btree_set<int> k_pdefs;
    CollectFullRegDefs(k_insn.pn.pattern, &k_fdefs, &k_pdefs);
    bool defs_r = false;
    for (int w = 0; w < r_nregs; ++w) {
      if (k_fdefs.contains(r + w) || k_pdefs.contains(r + w)) {
        defs_r = true;
        break;
      }
    }
    // `optabs.c:2485-2525` (`emit_no_conflict_block`) + `local-alloc.c:1317-1352`:
    // a `REG_LIBCALL` `(clobber (reg:DI dh))` overlapping `r` does not end `r`'s
    // live range when subsequent subword insns read `r` under `REG_NO_CONFLICT`.
    if (!reads_r && defs_r && k_insn.pn.pattern.op == RtlOp::kClobber &&
        k_insn.pn.pattern.children.size() == 1 && k_insn.pn.pattern.children[0].op == RtlOp::kReg &&
        k_insn.pn.pattern.children[0].mode == RtlMachineMode::kDi &&
        std::any_of(k_insn.pn.reg_notes.begin(), k_insn.pn.reg_notes.end(),
                    [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegLibcall; })) {
      const int dh = k_insn.pn.pattern.children[0].RegNo();
      bool has_nc_reader = false;
      for (int m_k = k + 1; m_k < m; ++m_k) {
        const Insn& m_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[m_k])];
        for (const RtlRegNote& mn : m_insn.pn.reg_notes) {
          if (mn.kind == RtlMachineMode::kRegNoConflict && mn.datum.op == RtlOp::kReg &&
              mn.datum.RegNo() == r && (dh == r || dh + 1 == r)) {
            has_nc_reader = true;
            break;
          }
        }
        if (has_nc_reader) {
          break;
        }
      }
      if (has_nc_reader) {
        continue;
      }
    }
    if (reads_r) {
      death_ci = k;
      if (HasRegDeadNoteCovering(k_insn.pn.reg_notes, r, mm)) {
        terminated_in_block = true;
        break;
      }
      const bool is_nc_subreg_write = std::any_of(
          k_insn.pn.reg_notes.begin(), k_insn.pn.reg_notes.end(), [r](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegNoConflict && n.datum.op == RtlOp::kReg &&
                   n.datum.RegNo() == r;
          });
      if (defs_r && !is_nc_subreg_write) {
        // Non-dying RMW extends the same pseudo; do not treat as a single-def conflict.
        return false;
      }
    } else if (defs_r) {
      terminated_in_block = true;
      break;
    }
  }
  if (death_ci < 0 || (!terminated_in_block && live_at_end_s2.contains(r))) {
    return false;
  }
  // If `death_ci` itself sets `r` (e.g. return `(set (reg/i r) ...)`), `r` is also
  // suggested at `death_ci` (`local-alloc.c:1824`) and may beat `s`.
  const Insn& death_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[death_ci])];
  if (const RtlExpr* dset = SingleSetOf(death_insn.pn.pattern);
      dset != nullptr && dset->SetDest().op == RtlOp::kReg && dset->SetDest().RegNo() == r) {
    return false;
  }
  for (const auto& [s, s_nregs] : cand_suggs) {
    bool s_busy = false;
    for (int k = ci + 1; k <= death_ci && !s_busy; ++k) {
      const Insn& k_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
      if (k_insn.kind == Insn::Kind::kCall) {
        if (k < death_ci && mm.CallUsed(s)) {
          s_busy = true;
          break;
        }
        absl::btree_set<int> fu_reads;
        for (const RtlExpr& fu : k_insn.pn.call_fusage) {
          CollectRegReads(fu, &fu_reads);
        }
        for (int w = 0; w < s_nregs; ++w) {
          if (fu_reads.contains(s + w)) {
            s_busy = true;
            break;
          }
        }
        continue;
      }
      absl::btree_set<int> k_reads;
      CollectRegReads(k_insn.pn.pattern, &k_reads);
      for (int w = 0; w < s_nregs; ++w) {
        if (k_reads.contains(s + w)) {
          s_busy = true;
          break;
        }
      }
      if (!s_busy && k < death_ci) {
        absl::btree_set<int> k_fdefs;
        absl::btree_set<int> k_pdefs;
        CollectFullRegDefs(k_insn.pn.pattern, &k_fdefs, &k_pdefs);
        for (int w = 0; w < s_nregs; ++w) {
          if (k_fdefs.contains(s + w) || k_pdefs.contains(s + w)) {
            s_busy = true;
            break;
          }
        }
      }
    }
    if (!s_busy) {
      return true;
    }
  }
  return false;
}

// Computes exact backward hard-register (`0..63`) liveness masks immediately
// AFTER each core instruction in `core_to_orig_index` from `live_at_end_s2`,
// excluding reload-scratch hard registers folded back onto spilled pseudos
// (`reload1.c:2309-2400, 5641-6160`) and preserving call-used hard registers
// saved/restored across `CALL_INSN` (`caller-save.c:349-620`).
std::vector<uint64_t> ComputeCoreHardRegLiveAfterMasks(
    absl::Span<const Insn> post_reload_block, absl::Span<const int> core_to_orig_index,
    const absl::btree_set<int>& live_at_end_s2, const SpillSlotFoldInfo& cross_spill,
    const std::vector<std::tuple<int, int, int>>& preserved_across_call_ranges,
    const MachineModel& mm) {
  const int m = static_cast<int>(core_to_orig_index.size());
  std::vector<uint64_t> live_after(static_cast<size_t>(m), 0);
  uint64_t live = 0;
  for (int r : live_at_end_s2) {
    if (r >= 0 && r < 64) {
      live |= (uint64_t{1} << r);
    }
  }
  for (int k = m - 1; k >= 0; --k) {
    live_after[static_cast<size_t>(k)] = live;
    const int orig_k = core_to_orig_index[static_cast<size_t>(k)];
    const Insn& insn = post_reload_block[static_cast<size_t>(orig_k)];
    if (insn.kind == Insn::Kind::kCall) {
      uint64_t preserved_mask = 0;
      for (const auto& [s_idx, l_idx, s_reg] : preserved_across_call_ranges) {
        if (s_idx < orig_k && orig_k < l_idx && s_reg >= 0 && s_reg < 64) {
          int nr = 1;
          if (s_idx >= 0 && s_idx < static_cast<int>(post_reload_block.size())) {
            const RtlExpr* ss =
                SingleSetOf(post_reload_block[static_cast<size_t>(s_idx)].pn.pattern);
            if (ss != nullptr) {
              nr = std::max(1, mm.HardRegNRegs(s_reg, ss->SetSrc().mode));
            }
          }
          for (int w = 0; w < nr && s_reg + w < 64; ++w) {
            preserved_mask |= (uint64_t{1} << (s_reg + w));
          }
        }
      }
      for (int r = 0; r < 64; ++r) {
        if (mm.CallUsed(r) && (preserved_mask & (uint64_t{1} << r)) == 0) {
          live &= ~(uint64_t{1} << r);
        }
      }
      absl::btree_set<int> fu_reads;
      for (const RtlExpr& fu : insn.pn.call_fusage) {
        CollectRegReads(fu, &fu_reads);
      }
      for (int r : fu_reads) {
        if (r >= 0 && r < 64) {
          live |= (uint64_t{1} << r);
        }
      }
      continue;
    }
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
    if (!cross_spill.store_slot_by_prod.contains(orig_k)) {
      for (int r : fdefs) {
        if (r >= 0 && r < 64) {
          live &= ~(uint64_t{1} << r);
        }
      }
    }
    absl::btree_set<int> k_reads;
    CollectRegReads(insn.pn.pattern, &k_reads);
    if (const auto lit = cross_spill.load_subs_by_cons.find(orig_k);
        lit != cross_spill.load_subs_by_cons.end()) {
      for (const auto& [hr, slot_mode] : lit->second) {
        const int nr = std::max(1, mm.HardRegNRegs(hr, slot_mode.second));
        for (int w = 0; w < nr; ++w) {
          k_reads.erase(hr + w);
        }
      }
    }
    for (int r : k_reads) {
      if (r >= 0 && r < 64) {
        live |= (uint64_t{1} << r);
      }
    }
  }
  return live_after;
}

int LocalAllocQtyPriority(int n_refs, int size_nregs, int span) {
  if (n_refs < 2 || span <= 0) {
    return 0;
  }
  int log2_refs = 0;
  for (int v = n_refs; v > 1; v >>= 1) {
    ++log2_refs;
  }
  return static_cast<int>(
      (static_cast<double>(log2_refs * n_refs * size_nregs) / static_cast<double>(span)) * 10000.0);
}

// Mirrors `local-alloc.c:1512-1539, 1597-1623` (`block_alloc` / `qty_compare_1`):
// checks whether `post_reload_block[core_to_orig_index[k]]` starts a block-local,
// non-call-crossing, non-suggested pseudo on `low_r` whose allocation priority is
// `<= r_pri` (so `q_r`, born earlier at `ci < k`, is allocated before `q_low` and
// sees `low_r` as free during `find_free_reg`). If so, sets `*low_death_out` to
// `q_low`'s death core index and returns true.
bool IsDeferredLowerPriorityLocalDef(absl::Span<const Insn> post_reload_block,
                                     absl::Span<const int> core_to_orig_index,
                                     const SpillSlotFoldInfo& cross_spill, int k, int low_r,
                                     int r_nregs, int r_pri,
                                     const absl::btree_set<int>& live_at_end_s2,
                                     const MachineModel& mm, int* low_death_out) {
  const int orig_k = core_to_orig_index[static_cast<size_t>(k)];
  if (cross_spill.store_slot_by_prod.contains(orig_k)) {
    return false;
  }
  const Insn& def_insn = post_reload_block[static_cast<size_t>(orig_k)];
  if (def_insn.kind == Insn::Kind::kCall) {
    return false;
  }
  const RtlExpr* sset = SingleSetOf(def_insn.pn.pattern);
  if (sset == nullptr || sset->SetDest().op != RtlOp::kReg || sset->SetDest().RegNo() != low_r ||
      sset->SetDest().flags.integrated || sset->SetSrc().op == RtlOp::kReg) {
    return false;
  }
  const int low_nregs = std::max(1, mm.HardRegNRegs(low_r, sset->SetDest().mode));
  if (low_nregs != r_nregs) {
    return false;
  }
  for (const RtlRegNote& note : def_insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const bool low_is_fp = (low_r >= kMipsFpRegFirst && low_r < kMipsMdRegFirst);
  std::vector<std::pair<int, RtlMachineMode>> direct_srcs;
  CollectDirectNonMemRegReads(sset->SetSrc(), &direct_srcs);
  for (const auto& [s, s_mode] : direct_srcs) {
    if (s < 0 || s >= kMipsMdRegFirst) {
      continue;
    }
    const bool s_is_fp = (s >= kMipsFpRegFirst);
    if (s_is_fp == low_is_fp && HasRegDeadNoteCovering(def_insn.pn.reg_notes, s, mm)) {
      return false;
    }
  }
  const int m = static_cast<int>(core_to_orig_index.size());
  int death_j = -1;
  int n_refs = 1;
  bool terminated = false;
  for (int j = k + 1; j < m; ++j) {
    const int orig_j = core_to_orig_index[static_cast<size_t>(j)];
    const Insn& j_insn = post_reload_block[static_cast<size_t>(orig_j)];
    if (j_insn.kind == Insn::Kind::kCall) {
      return false;
    }
    absl::btree_set<int> j_reads;
    CollectRegReads(j_insn.pn.pattern, &j_reads);
    if (const auto lit = cross_spill.load_subs_by_cons.find(orig_j);
        lit != cross_spill.load_subs_by_cons.end()) {
      for (const auto& [hr, slot_mode] : lit->second) {
        const int nr = std::max(1, mm.HardRegNRegs(hr, slot_mode.second));
        for (int w = 0; w < nr; ++w) {
          j_reads.erase(hr + w);
        }
      }
    }
    bool reads_low = false;
    for (int w = 0; w < low_nregs; ++w) {
      if (j_reads.contains(low_r + w)) {
        reads_low = true;
        break;
      }
    }
    absl::btree_set<int> j_fdefs;
    absl::btree_set<int> j_pdefs;
    CollectFullRegDefs(j_insn.pn.pattern, &j_fdefs, &j_pdefs);
    bool defs_low = false;
    for (int w = 0; w < low_nregs; ++w) {
      if (j_fdefs.contains(low_r + w) || j_pdefs.contains(low_r + w)) {
        defs_low = true;
        break;
      }
    }
    const bool spills_dest = cross_spill.store_slot_by_prod.contains(orig_j);
    if (reads_low) {
      if (j_insn.pn.pattern.op == RtlOp::kUse) {
        return false;
      }
      // If `j` copies `low_r` directly to a hard reg (e.g. call arg / return),
      // `q_low` could receive a hard-reg suggestion (`qty_sugg_order`).
      const RtlExpr* j_set = SingleSetOf(j_insn.pn.pattern);
      if (j_set != nullptr && j_set->SetDest().op == RtlOp::kReg &&
          j_set->SetDest().flags.integrated) {
        return false;
      }
      ++n_refs;
      death_j = j;
      if (defs_low && spills_dest) {
        // `local-alloc.c:1851-1886` (`combine_regs`): a dying read feeding a
        // spilled-output def is combined into the spilled pseudo's quantity.
        return false;
      }
      if (HasRegDeadNoteCovering(j_insn.pn.reg_notes, low_r, mm)) {
        if (defs_low) {
          return false;
        }
        terminated = true;
        break;
      }
      if (defs_low) {
        return false;
      }
    } else if (defs_low) {
      terminated = true;
      break;
    }
  }
  if (death_j <= k) {
    return false;
  }
  if (!terminated) {
    for (int w = 0; w < low_nregs; ++w) {
      if (live_at_end_s2.contains(low_r + w)) {
        return false;
      }
    }
  }
  const int low_pri = LocalAllocQtyPriority(n_refs, low_nregs, death_j - k);
  if (low_pri > r_pri) {
    return false;
  }
  *low_death_out = death_j;
  return true;
}

// Mirrors `local-alloc.c:2158-2181, 2243-2263` (`find_free_reg` under
// `#ifndef REG_ALLOC_ORDER` + `post_mark_life`, `config/mips/mips.h:1186-1209, 1255-1274`):
// if a block-local single-set non-call, non-copy definition of hard register `r`
// at `ci` has no dying same-bank operand at `ci` (so `combine_regs` cannot tie
// it to an earlier quantity), dies strictly within the block at `death_ci`
// without crossing a call and without overwriting `r` at `death_ci`, and a
// strictly lower same-bank call-used allocatable register `low_r < r` is free
// across `[ci, death_ci)`, then `find_free_reg` would allocate `low_r` instead
// of `r`; Stage 4/5 therefore keeps `r` unrenamed on physical hard register `r`.
bool HasLocalLowerAllocOrderFreeRegConflict(absl::Span<const Insn> post_reload_block,
                                            absl::Span<const int> core_to_orig_index,
                                            absl::Span<const uint64_t> live_after_core, int ci,
                                            int r, const absl::btree_set<int>& reads,
                                            const absl::btree_set<int>& live_at_end_s2,
                                            const SpillSlotFoldInfo& cross_spill,
                                            const MachineModel& mm) {
  if (r < 2 || r >= kMipsMdRegFirst || IsFixedOrSpecialHardReg(r) || reads.contains(r)) {
    return false;
  }
  const Insn& def_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
  if (def_insn.kind == Insn::Kind::kCall) {
    return false;
  }
  const RtlExpr* sset = SingleSetOf(def_insn.pn.pattern);
  if (sset == nullptr || sset->SetDest().op != RtlOp::kReg || sset->SetDest().RegNo() != r ||
      sset->SetDest().flags.integrated || sset->SetSrc().op == RtlOp::kReg) {
    return false;
  }
  for (const RtlRegNote& note : def_insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const RtlMachineMode r_mode = sset->SetDest().mode;
  const int r_nregs = std::max(1, mm.HardRegNRegs(r, r_mode));
  const bool r_is_fp = (r >= kMipsFpRegFirst && r < kMipsMdRegFirst);
  if (!mm.CallUsed(r)) {
    return false;
  }
  // If `def_insn` consumes any dying same-bank direct operand (or folded
  // spilled-pseudo operand), `combine_regs` (`local-alloc.c:1266-1300, 1851-1886`)
  // could merge `r` into an earlier quantity.
  const int orig_ci = core_to_orig_index[static_cast<size_t>(ci)];
  const auto ci_spill_it = cross_spill.load_subs_by_cons.find(orig_ci);
  std::vector<std::pair<int, RtlMachineMode>> direct_srcs;
  CollectDirectNonMemRegReads(sset->SetSrc(), &direct_srcs);
  for (const auto& [s, s_mode] : direct_srcs) {
    if (s < 0 || s >= kMipsMdRegFirst) {
      continue;
    }
    const bool s_is_fp = (s >= kMipsFpRegFirst);
    if (s_is_fp == r_is_fp &&
        (HasRegDeadNoteCovering(def_insn.pn.reg_notes, s, mm) ||
         (ci_spill_it != cross_spill.load_subs_by_cons.end() && ci_spill_it->second.contains(s)))) {
      return false;
    }
  }

  const int m = static_cast<int>(core_to_orig_index.size());
  int death_ci = -1;
  int r_n_refs = 1;
  bool terminated_in_block = false;
  for (int k = ci + 1; k < m; ++k) {
    const int orig_k = core_to_orig_index[static_cast<size_t>(k)];
    const Insn& k_insn = post_reload_block[static_cast<size_t>(orig_k)];
    if (k_insn.kind == Insn::Kind::kCall) {
      return false;
    }
    absl::btree_set<int> k_reads;
    CollectRegReads(k_insn.pn.pattern, &k_reads);
    if (const auto lit = cross_spill.load_subs_by_cons.find(orig_k);
        lit != cross_spill.load_subs_by_cons.end()) {
      for (const auto& [hr, slot_mode] : lit->second) {
        const int nr = std::max(1, mm.HardRegNRegs(hr, slot_mode.second));
        for (int w = 0; w < nr; ++w) {
          k_reads.erase(hr + w);
        }
      }
    }
    bool reads_r = false;
    for (int w = 0; w < r_nregs; ++w) {
      if (k_reads.contains(r + w)) {
        reads_r = true;
        break;
      }
    }
    absl::btree_set<int> k_fdefs;
    absl::btree_set<int> k_pdefs;
    CollectFullRegDefs(k_insn.pn.pattern, &k_fdefs, &k_pdefs);
    bool defs_r = false;
    for (int w = 0; w < r_nregs; ++w) {
      if (k_fdefs.contains(r + w) || k_pdefs.contains(r + w)) {
        defs_r = true;
        break;
      }
    }
    const bool spills_dest = cross_spill.store_slot_by_prod.contains(orig_k);
    if (reads_r) {
      if (k_insn.pn.pattern.op == RtlOp::kUse) {
        return false;
      }
      ++r_n_refs;
      death_ci = k;
      if (defs_r && spills_dest) {
        // `reload1.c:5641-6155` + `local-alloc.c:2158-2181`: when `k`'s output
        // pseudo was spilled (`reg_renumber == -1`), post-reload `k` only writes
        // `r` as a scratch output-reload register for the spill store; if `k`
        // carries `(REG_DEAD r)`, `r`'s local live range terminates at `k`.
        if (HasRegDeadNoteCovering(k_insn.pn.reg_notes, r, mm)) {
          terminated_in_block = true;
          break;
        }
        return false;
      }
      if (HasRegDeadNoteCovering(k_insn.pn.reg_notes, r, mm)) {
        if (defs_r) {
          return false;
        }
        terminated_in_block = true;
        break;
      }
      if (defs_r) {
        return false;
      }
    } else if (defs_r) {
      terminated_in_block = true;
      break;
    }
  }
  if (death_ci <= ci) {
    return false;
  }
  if (!terminated_in_block) {
    for (int w = 0; w < r_nregs; ++w) {
      if (live_at_end_s2.contains(r + w)) {
        return false;
      }
    }
  }

  const int r_pri = LocalAllocQtyPriority(r_n_refs, r_nregs, death_ci - ci);
  const int bank_lo = r_is_fp ? kMipsFpRegFirst : 2;
  const int step = r_is_fp ? (mm.TargetFloat64() ? r_nregs : 2) : (mm.Target64Bit() ? 1 : r_nregs);
  const uint64_t live_after_ci = live_after_core[static_cast<size_t>(ci)];
  for (int low_r = bank_lo; low_r < r; low_r += step) {
    bool candidate_ok = true;
    for (int w = 0; w < r_nregs; ++w) {
      const int sub = low_r + w;
      if (IsFixedOrSpecialHardReg(sub) || !mm.CallUsed(sub) ||
          (live_after_ci & (uint64_t{1} << sub)) != 0) {
        candidate_ok = false;
        break;
      }
    }
    if (!candidate_ok) {
      continue;
    }
    bool low_busy = false;
    for (int k = ci + 1; k <= death_ci && !low_busy; ++k) {
      const int orig_k = core_to_orig_index[static_cast<size_t>(k)];
      const Insn& k_insn = post_reload_block[static_cast<size_t>(orig_k)];
      absl::btree_set<int> k_reads;
      CollectRegReads(k_insn.pn.pattern, &k_reads);
      if (const auto lit = cross_spill.load_subs_by_cons.find(orig_k);
          lit != cross_spill.load_subs_by_cons.end()) {
        for (const auto& [hr, slot_mode] : lit->second) {
          const int nr = std::max(1, mm.HardRegNRegs(hr, slot_mode.second));
          for (int w = 0; w < nr; ++w) {
            k_reads.erase(hr + w);
          }
        }
      }
      for (int w = 0; w < r_nregs; ++w) {
        if (k_reads.contains(low_r + w)) {
          low_busy = true;
          break;
        }
      }
      if (!low_busy && k < death_ci && !cross_spill.store_slot_by_prod.contains(orig_k)) {
        absl::btree_set<int> k_fdefs;
        absl::btree_set<int> k_pdefs;
        CollectFullRegDefs(k_insn.pn.pattern, &k_fdefs, &k_pdefs);
        bool defs_low = false;
        for (int w = 0; w < r_nregs; ++w) {
          if (k_fdefs.contains(low_r + w) || k_pdefs.contains(low_r + w)) {
            defs_low = true;
            break;
          }
        }
        if (defs_low) {
          int low_death = -1;
          if (IsDeferredLowerPriorityLocalDef(post_reload_block, core_to_orig_index, cross_spill, k,
                                              low_r, r_nregs, r_pri, live_at_end_s2, mm,
                                              &low_death)) {
            k = low_death;
          } else {
            low_busy = true;
          }
        }
      }
    }
    if (!low_busy) {
      return true;
    }
  }
  return false;
}

// Folds `reload1.c:5186-5249, 5641` cross-bank `"=f"` FPR->GPR (`mfc1`) output
// reloads `(set (reg:SF|DF gpr) (reg:SF|DF fpr))` (including across
// `sched2`-interleaved insns, excluded block-0 prologue saves, and pure FP
// readers inheriting `fpr`) back onto their producing FP arithmetic insn.
void FoldFprOutputReloadsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, std::vector<bool>* is_spill_overhead,
    absl::btree_map<int, RtlExpr>* md_output_reload_dest,
    absl::btree_map<int, int>* output_reload_orig_owner,
    absl::btree_map<int, absl::btree_map<int, RtlExpr>>* fpr_input_reload_subs) {
  const int n = static_cast<int>(post_reload_block.size());
  for (int mv_idx = 1; mv_idx < n; ++mv_idx) {
    if ((*is_spill_overhead)[static_cast<size_t>(mv_idx)]) {
      continue;
    }
    const Insn& mv = post_reload_block[static_cast<size_t>(mv_idx)];
    const RtlExpr& mv_pat = mv.pn.pattern;
    if (mv.kind != Insn::Kind::kInsn || mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
        mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetDest().flags.integrated ||
        mv_pat.SetSrc().op != RtlOp::kReg || mv_pat.SetDest().mode != mv_pat.SetSrc().mode ||
        (mv_pat.SetDest().mode != RtlMachineMode::kSf &&
         mv_pat.SetDest().mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int dst_gpr = mv_pat.SetDest().RegNo();
    const int src_fpr = mv_pat.SetSrc().RegNo();
    if (dst_gpr < 2 || dst_gpr >= kMipsFpRegFirst || src_fpr < kMipsFpRegFirst ||
        src_fpr > kMipsFpRegLast) {
      continue;
    }
    const int dst_nregs = std::max(1, mm.HardRegNRegs(dst_gpr, mv_pat.SetDest().mode));
    const int fpr_nregs = std::max(1, mm.HardRegNRegs(src_fpr, mv_pat.SetSrc().mode));
    // Scan forward for post-`mv` pure FP readers inheriting `src_fpr`
    // (`reload1.c:5186-5249, 5641` `choose_reload_regs` + `emit_reload_insns`).
    std::vector<int> post_inherited_readers;
    int last_fpr_use_idx = mv_idx;
    for (int k = mv_idx + 1; k < n; ++k) {
      if ((*is_spill_overhead)[static_cast<size_t>(k)]) {
        continue;
      }
      const Insn& after = post_reload_block[static_cast<size_t>(k)];
      if (after.kind == Insn::Kind::kCall || after.kind == Insn::Kind::kJump) {
        break;
      }
      absl::btree_set<int> a_fdefs;
      absl::btree_set<int> a_pdefs;
      absl::btree_set<int> a_reads;
      CollectFullRegDefs(after.pn.pattern, &a_fdefs, &a_pdefs);
      CollectRegReads(after.pn.pattern, &a_reads);
      bool reads_fpr = false;
      bool defs_fpr = false;
      for (int w = 0; w < fpr_nregs; ++w) {
        if (a_reads.contains(src_fpr + w)) {
          reads_fpr = true;
        }
        if (a_fdefs.contains(src_fpr + w) || a_pdefs.contains(src_fpr + w)) {
          defs_fpr = true;
        }
      }
      if (reads_fpr) {
        const RtlExpr& ap = after.pn.pattern;
        const bool is_pure_fp_reader = after.kind == Insn::Kind::kInsn && ap.op == RtlOp::kSet &&
                                       ap.children.size() == 2 && ap.SetSrc().op != RtlOp::kReg &&
                                       ap.SetSrc().op != RtlOp::kMem &&
                                       ap.SetSrc().op != RtlOp::kConstDouble && !defs_fpr;
        if (!is_pure_fp_reader) {
          break;
        }
        post_inherited_readers.push_back(k);
        last_fpr_use_idx = k;
      }
      if (defs_fpr) {
        break;
      }
      bool defs_dst = false;
      for (int w = 0; w < dst_nregs; ++w) {
        if (a_fdefs.contains(dst_gpr + w) || a_pdefs.contains(dst_gpr + w)) {
          defs_dst = true;
          break;
        }
      }
      if (defs_dst) {
        break;
      }
    }
    if (!IsRegDeadAfterInsn(post_reload_block, last_fpr_use_idx, src_fpr, mv_pat.SetSrc().mode, mm,
                            live_at_end_s2)) {
      continue;
    }
    std::vector<int> inherited_readers;
    for (int prod_idx = mv_idx - 1; prod_idx >= 0; --prod_idx) {
      if ((*is_spill_overhead)[static_cast<size_t>(prod_idx)]) {
        continue;
      }
      const Insn& prod = post_reload_block[static_cast<size_t>(prod_idx)];
      if (prod.kind == Insn::Kind::kCall) {
        break;
      }
      RtlExpr gpr_dest;
      // Do not fold a call-used GPR copy `(set (reg:SF/DF a0..t9) (reg:SF/DF $fN))`
      // when other FP insns also read `$fN`: in that case the copy is a genuine
      // pre-reload ABI/call-argument move rather than a `reload1.c:5641`
      // `emit_reload_insns` output reload of a pseudo allocated to a GPR.
      if (!md_output_reload_dest->contains(prod_idx) &&
          (!mm.CallUsed(dst_gpr) ||
           (inherited_readers.empty() && post_inherited_readers.empty())) &&
          TryMatchAdjacentFprOutputReload(prod, mv, &gpr_dest)) {
        (*is_spill_overhead)[static_cast<size_t>(mv_idx)] = true;
        (*md_output_reload_dest)[prod_idx] = gpr_dest;
        (*output_reload_orig_owner)[mv_idx] = prod_idx;
        for (int mid_idx : inherited_readers) {
          (*fpr_input_reload_subs)[mid_idx][src_fpr] = gpr_dest;
        }
        for (int after_idx : post_inherited_readers) {
          (*fpr_input_reload_subs)[after_idx][src_fpr] = gpr_dest;
        }
        break;
      }
      absl::btree_set<int> p_fdefs;
      absl::btree_set<int> p_pdefs;
      absl::btree_set<int> p_reads;
      CollectFullRegDefs(prod.pn.pattern, &p_fdefs, &p_pdefs);
      CollectRegReads(prod.pn.pattern, &p_reads);
      bool defines_fpr = false;
      bool reads_fpr = false;
      for (int k = 0; k < fpr_nregs; ++k) {
        if (p_fdefs.contains(src_fpr + k) || p_pdefs.contains(src_fpr + k)) {
          defines_fpr = true;
        }
        if (p_reads.contains(src_fpr + k)) {
          reads_fpr = true;
        }
      }
      if (defines_fpr) {
        break;
      }
      bool touches_dst = false;
      for (int k = 0; k < dst_nregs; ++k) {
        if (p_fdefs.contains(dst_gpr + k) || p_pdefs.contains(dst_gpr + k) ||
            p_reads.contains(dst_gpr + k)) {
          touches_dst = true;
          break;
        }
      }
      if (touches_dst) {
        break;
      }
      if (reads_fpr) {
        if (prod.kind != Insn::Kind::kInsn) {
          break;
        }
        inherited_readers.push_back(prod_idx);
      }
    }
  }
}

// Folds `reload1.c:emit_reload_insns` cross-bank `"f"` GPR->FPR (`mtc1`) input
// reloads `(set (reg:SF|DF fpr) (reg:SF|DF gpr))` (including across
// `sched2`-interleaved independent insns and excluded block-0 prologue saves)
// back onto their single FP consumer `cons_idx`.
void FoldFprInputReloadsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2,
    const absl::btree_map<int, RtlExpr>& md_output_reload_dest,
    std::vector<bool>* is_spill_overhead,
    absl::btree_map<int, absl::btree_map<int, RtlExpr>>* fpr_input_reload_subs,
    absl::btree_map<int, int>* fpr_input_reload_orig_consumer) {
  const int n = static_cast<int>(post_reload_block.size());
  for (int cons_idx = 1; cons_idx < n; ++cons_idx) {
    if ((*is_spill_overhead)[static_cast<size_t>(cons_idx)]) {
      continue;
    }
    const Insn& cons = post_reload_block[static_cast<size_t>(cons_idx)];
    if (cons.kind != Insn::Kind::kInsn) {
      continue;
    }
    absl::btree_set<int> cons_reads;
    CollectRegReads(cons.pn.pattern, &cons_reads);
    bool reads_any_fpr = false;
    for (int r : cons_reads) {
      if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
        reads_any_fpr = true;
        break;
      }
    }
    if (!reads_any_fpr) {
      continue;
    }
    int matched = 0;
    absl::btree_set<int> blocked_fprs;
    absl::btree_set<int> clobbered_fprs;
    absl::btree_set<int> clobbered_gprs;
    for (int mv_idx = cons_idx - 1; mv_idx >= 0 && matched < 2; --mv_idx) {
      const Insn& mid = post_reload_block[static_cast<size_t>(mv_idx)];
      if (mid.kind == Insn::Kind::kCall || mid.kind == Insn::Kind::kJump) {
        break;
      }
      if (!(*is_spill_overhead)[static_cast<size_t>(mv_idx)]) {
        int fpr = -1;
        RtlExpr src_reg_expr;
        if (TryMatchAdjacentFprInputReload(post_reload_block, mv_idx, cons_idx, mm, live_at_end_s2,
                                           md_output_reload_dest, &fpr, &src_reg_expr) &&
            !(*fpr_input_reload_subs)[cons_idx].contains(fpr)) {
          const int fpr_nregs = std::max(1, mm.HardRegNRegs(fpr, src_reg_expr.mode));
          const int src_reg = src_reg_expr.RegNo();
          const int src_nregs = std::max(1, mm.HardRegNRegs(src_reg, src_reg_expr.mode));
          const bool src_is_fpr = (src_reg >= kMipsFpRegFirst && src_reg <= kMipsFpRegLast);
          bool conflict = false;
          for (int k = 0; k < fpr_nregs && !conflict; ++k) {
            if (blocked_fprs.contains(fpr + k)) {
              conflict = true;
            }
          }
          for (int k = 0; k < src_nregs && !conflict; ++k) {
            if (src_is_fpr ? clobbered_fprs.contains(src_reg + k)
                           : clobbered_gprs.contains(src_reg + k)) {
              conflict = true;
            }
          }
          if (!conflict) {
            (*is_spill_overhead)[static_cast<size_t>(mv_idx)] = true;
            (*fpr_input_reload_subs)[cons_idx][fpr] = std::move(src_reg_expr);
            (*fpr_input_reload_orig_consumer)[mv_idx] = cons_idx;
            ++matched;
            continue;
          }
        }
      }
      absl::btree_set<int> mid_fdefs;
      absl::btree_set<int> mid_pdefs;
      absl::btree_set<int> mid_reads;
      CollectFullRegDefs(mid.pn.pattern, &mid_fdefs, &mid_pdefs);
      CollectRegReads(mid.pn.pattern, &mid_reads);
      for (int r : mid_fdefs) {
        if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
          blocked_fprs.insert(r);
          clobbered_fprs.insert(r);
        } else if (r >= 0 && r < kMipsFpRegFirst) {
          clobbered_gprs.insert(r);
        }
      }
      for (int r : mid_pdefs) {
        if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
          blocked_fprs.insert(r);
          clobbered_fprs.insert(r);
        } else if (r >= 0 && r < kMipsFpRegFirst) {
          clobbered_gprs.insert(r);
        }
      }
      for (int r : mid_reads) {
        if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
          blocked_fprs.insert(r);
        }
      }
    }
    if (matched == 0 && fpr_input_reload_subs->contains(cons_idx) &&
        (*fpr_input_reload_subs)[cons_idx].empty()) {
      fpr_input_reload_subs->erase(cons_idx);
    }
  }

  // `reload1.c:5186-5258` (`choose_reload_regs` inheritance): a cross-bank
  // callee-saved-GPR -> call-used-FPR input reload `(set (reg:SF/DF fpr) (reg:SF/DF gpr))`
  // can be inherited across 2+ pure FP consumers until `fpr` is overwritten or
  // dies at its final pure FP consumer.
  auto is_pure_fp_consumer_insn = [](const Insn& insn) -> bool {
    if (insn.kind != Insn::Kind::kInsn) {
      return false;
    }
    const RtlExpr& p = insn.pn.pattern;
    if (p.op != RtlOp::kSet || p.children.size() != 2) {
      return false;
    }
    const RtlOp rop = p.SetSrc().op;
    return rop != RtlOp::kReg && rop != RtlOp::kMem && rop != RtlOp::kConstDouble &&
           rop != RtlOp::kCall;
  };
  for (int mv_idx = 0; mv_idx + 1 < n; ++mv_idx) {
    if ((*is_spill_overhead)[static_cast<size_t>(mv_idx)]) {
      continue;
    }
    const Insn& mv = post_reload_block[static_cast<size_t>(mv_idx)];
    const RtlExpr& mv_pat = mv.pn.pattern;
    if (mv.kind != Insn::Kind::kInsn || mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
        mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetSrc().op != RtlOp::kReg ||
        mv_pat.SetDest().flags.integrated || mv_pat.SetSrc().flags.integrated ||
        mv_pat.SetDest().mode != mv_pat.SetSrc().mode ||
        (mv_pat.SetDest().mode != RtlMachineMode::kSf &&
         mv_pat.SetDest().mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int fpr = mv_pat.SetDest().RegNo();
    const int gpr = mv_pat.SetSrc().RegNo();
    const bool gpr_callee_saved = (gpr >= 16 && gpr <= 23) || gpr == 30;
    if (fpr < kMipsFpRegFirst || fpr >= 52 || !gpr_callee_saved) {
      continue;
    }
    const int fpr_nw = std::max(1, mm.HardRegNRegs(fpr, mv_pat.SetDest().mode));
    const int gpr_nw = std::max(1, mm.HardRegNRegs(gpr, mv_pat.SetSrc().mode));
    std::vector<int> consumers;
    bool closed = false;
    for (int k = mv_idx + 1; k < n; ++k) {
      if ((*is_spill_overhead)[static_cast<size_t>(k)]) {
        continue;
      }
      const Insn& insn = post_reload_block[static_cast<size_t>(k)];
      if (insn.kind == Insn::Kind::kCall || insn.kind == Insn::Kind::kJump) {
        break;
      }
      absl::btree_set<int> k_fdefs;
      absl::btree_set<int> k_pdefs;
      absl::btree_set<int> k_reads;
      CollectFullRegDefs(insn.pn.pattern, &k_fdefs, &k_pdefs);
      CollectRegReads(insn.pn.pattern, &k_reads);
      bool reads_fpr = false;
      bool defs_fpr = false;
      bool defs_gpr = false;
      for (int w = 0; w < fpr_nw; ++w) {
        if (k_reads.contains(fpr + w)) {
          reads_fpr = true;
        }
        if (k_fdefs.contains(fpr + w) || k_pdefs.contains(fpr + w)) {
          defs_fpr = true;
        }
      }
      for (int w = 0; w < gpr_nw; ++w) {
        if (k_fdefs.contains(gpr + w) || k_pdefs.contains(gpr + w)) {
          defs_gpr = true;
        }
      }
      if (reads_fpr) {
        if (!is_pure_fp_consumer_insn(insn) ||
            (fpr_input_reload_subs->contains(k) && (*fpr_input_reload_subs)[k].contains(fpr))) {
          break;
        }
        consumers.push_back(k);
        if (defs_fpr || IsRegDeadAfterInsn(post_reload_block, k, fpr, mv_pat.SetDest().mode, mm,
                                           live_at_end_s2)) {
          closed = true;
          break;
        }
        if (defs_gpr) {
          break;
        }
        continue;
      }
      if (defs_fpr || defs_gpr) {
        break;
      }
    }
    if (closed && consumers.size() >= 2) {
      (*is_spill_overhead)[static_cast<size_t>(mv_idx)] = true;
      for (int c_idx : consumers) {
        (*fpr_input_reload_subs)[c_idx][fpr] = mv_pat.SetSrc();
      }
      (*fpr_input_reload_orig_consumer)[mv_idx] = consumers.front();
    }
  }
}

struct ConstDoubleReloadFoldInfo {
  // Maps consumer orig_idx -> (fpr_reg -> (const_double_key, mode)).
  absl::btree_map<int, absl::btree_map<int, std::pair<std::string, RtlMachineMode>>>
      load_subs_by_cons;
  // Maps folded `const_double` reload orig_idx -> first consuming orig_idx.
  absl::btree_map<int, int> input_reload_orig_consumer;
};

// Mirrors `reload1.c:517-524, 566-573, 6583` (`reg_equiv_constant`) and
// `FoldSpilledConstDoubleReloads` in `invert_reload.cc`: in a self-loop block
// with a unique preheader, an unallocated loop-invariant `const_double:SF/DF`
// pseudo is rematerialized as `(set (reg:SF/DF spill_fpr) (const_double ...))`
// (with empty `reg_notes`) into a proven call-used FP spill register before
// pure FP consumers. Fold those rematerialized input reloads onto a shared
// live-in preheader pseudo per distinct constant key.
ConstDoubleReloadFoldInfo FoldSpilledConstDoubleReloadsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context,
    const absl::btree_map<int, absl::btree_map<int, RtlExpr>>& fpr_input_reload_subs,
    const absl::btree_map<int, int>& output_reload_orig_owner, const SpillSlotFoldInfo& cross_spill,
    std::vector<bool>* is_spill_overhead) {
  ConstDoubleReloadFoldInfo info;
  if (!context.has_self_loop_preheader) {
    return info;
  }
  const int n = static_cast<int>(post_reload_block.size());
  absl::btree_set<int> proven_fp_spill_regs;
  auto add_fpr_spill = [&](int r, RtlMachineMode mode) {
    if (r >= kMipsFpRegFirst && r < 52) {
      const int nw = std::max(1, mm.HardRegNRegs(r, mode));
      for (int w = 0; w < nw; ++w) {
        if (r + w < 52) {
          proven_fp_spill_regs.insert(r + w);
        }
      }
    }
  };
  for (const auto& [cons_idx, subs] : fpr_input_reload_subs) {
    for (const auto& [fpr, expr] : subs) {
      add_fpr_spill(fpr, expr.mode);
    }
  }
  for (const auto& [mv_orig, prod_orig] : output_reload_orig_owner) {
    if (mv_orig >= 0 && mv_orig < n) {
      const RtlExpr& p = post_reload_block[static_cast<size_t>(mv_orig)].pn.pattern;
      if (p.op == RtlOp::kSet && p.children.size() == 2 && p.SetSrc().op == RtlOp::kReg) {
        add_fpr_spill(p.SetSrc().RegNo(), p.SetSrc().mode);
      }
    }
  }
  for (const auto& [cons_idx, subs] : cross_spill.load_subs_by_cons) {
    for (const auto& [l_reg, slot_and_mode] : subs) {
      add_fpr_spill(l_reg, slot_and_mode.second);
    }
  }
  for (const auto& [prod_orig, slot_off] : cross_spill.store_slot_by_prod) {
    if (prod_orig >= 0 && prod_orig < n) {
      const RtlExpr* s = SingleSetOf(post_reload_block[static_cast<size_t>(prod_orig)].pn.pattern);
      if (s != nullptr && s->SetDest().op == RtlOp::kReg) {
        add_fpr_spill(s->SetDest().RegNo(), s->SetDest().mode);
      }
    }
  }
  if (proven_fp_spill_regs.empty()) {
    return info;
  }

  auto is_pure_fp_consumer = [](const Insn& insn) -> bool {
    if (insn.kind != Insn::Kind::kInsn) {
      return false;
    }
    const RtlExpr& p = insn.pn.pattern;
    if (p.op != RtlOp::kSet || p.children.size() != 2) {
      return false;
    }
    const RtlOp rop = p.SetSrc().op;
    return rop != RtlOp::kReg && rop != RtlOp::kMem && rop != RtlOp::kConstDouble &&
           rop != RtlOp::kCall;
  };
  for (int i = 0; i + 1 < n; ++i) {
    if ((*is_spill_overhead)[static_cast<size_t>(i)]) {
      continue;
    }
    const Insn& insn = post_reload_block[static_cast<size_t>(i)];
    if (insn.kind != Insn::Kind::kInsn || !insn.pn.reg_notes.empty()) {
      continue;
    }
    const RtlExpr& pat = insn.pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kReg ||
        pat.SetDest().flags.Any() || pat.SetSrc().op != RtlOp::kConstDouble ||
        pat.SetDest().mode != pat.SetSrc().mode ||
        (pat.SetDest().mode != RtlMachineMode::kSf && pat.SetDest().mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int fpr = pat.SetDest().RegNo();
    if (fpr < kMipsFpRegFirst || fpr >= 52 || !proven_fp_spill_regs.contains(fpr)) {
      continue;
    }
    const int nw = std::max(1, mm.HardRegNRegs(fpr, pat.SetDest().mode));
    std::vector<int> consumers;
    bool closed = false;
    for (int k = i + 1; k < n; ++k) {
      if ((*is_spill_overhead)[static_cast<size_t>(k)]) {
        continue;
      }
      const Insn& c_insn = post_reload_block[static_cast<size_t>(k)];
      if (c_insn.kind == Insn::Kind::kCall || c_insn.kind == Insn::Kind::kJump) {
        break;
      }
      absl::btree_set<int> k_fdefs;
      absl::btree_set<int> k_pdefs;
      absl::btree_set<int> k_reads;
      CollectFullRegDefs(c_insn.pn.pattern, &k_fdefs, &k_pdefs);
      CollectRegReads(c_insn.pn.pattern, &k_reads);
      bool reads_fpr = false;
      bool defs_fpr = false;
      for (int w = 0; w < nw; ++w) {
        if (k_reads.contains(fpr + w)) {
          reads_fpr = true;
        }
        if (k_fdefs.contains(fpr + w) || k_pdefs.contains(fpr + w)) {
          defs_fpr = true;
        }
      }
      if (reads_fpr) {
        if (!is_pure_fp_consumer(c_insn) ||
            (fpr_input_reload_subs.contains(k) && fpr_input_reload_subs.at(k).contains(fpr)) ||
            (cross_spill.load_subs_by_cons.contains(k) &&
             cross_spill.load_subs_by_cons.at(k).contains(fpr))) {
          break;
        }
        consumers.push_back(k);
        if (defs_fpr ||
            IsRegDeadAfterInsn(post_reload_block, k, fpr, pat.SetDest().mode, mm, live_at_end_s2)) {
          closed = true;
          break;
        }
        continue;
      }
      if (defs_fpr) {
        break;
      }
    }
    if (!closed || consumers.empty()) {
      continue;
    }
    std::string cd_key = std::to_string(static_cast<int>(pat.SetDest().mode)) + ":" +
                         std::to_string(pat.SetSrc().int_val) + ":" +
                         std::to_string(pat.SetSrc().int_val_high);
    for (int64_t ev : pat.SetSrc().extra_ints) {
      cd_key += ":" + std::to_string(ev);
    }
    (*is_spill_overhead)[static_cast<size_t>(i)] = true;
    info.input_reload_orig_consumer[i] = consumers.front();
    for (int c_idx : consumers) {
      info.load_subs_by_cons[c_idx][fpr] = {cd_key, pat.SetDest().mode};
    }
  }
  return info;
}

// Matches a `reload1.c:5599-5665` (`emit_reload_insns`) earlyclobber `DImode`
}  // namespace

namespace detail {

// GPR output-reload pair: `prod` is a `DImode` `PARALLEL` (`mips.md:407-411`
// `adddi3_internal_1` `"=d,&d"` / `"0,d"`, `mips.md:3722-3726`
// `ashldi3_internal` `"=&d"`, `mips.md:3949-3953` `ashrdi3_internal` `"=&d"`,
// `mips.md:4174-4178` `lshrdi3_internal` `"=&d"`) writing `(reg:DI reload_hr)`
// with an input overlapping `orig_hr` (triggering earlyclobber), and `mv` is
// `(set (reg:DI orig_hr) (reg:DI reload_hr))`.
bool TryMatchEarlyclobberDiOutputReload(const Insn& prod, const Insn& mv, const MachineModel& mm,
                                        int* out_orig_hr, int* out_reload_hr) {
  const RtlExpr& mv_pat = mv.pn.pattern;
  if (mv.kind != Insn::Kind::kInsn || mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
      mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetSrc().op != RtlOp::kReg ||
      mv_pat.SetDest().mode != RtlMachineMode::kDi || mv_pat.SetSrc().mode != RtlMachineMode::kDi) {
    return false;
  }
  const int orig_hr = mv_pat.SetDest().RegNo();
  const int reload_hr = mv_pat.SetSrc().RegNo();
  if (orig_hr < 2 || orig_hr >= kMipsFpRegFirst - 1 || reload_hr < 2 ||
      reload_hr >= kMipsFpRegFirst - 1 || orig_hr == reload_hr || (orig_hr % 2) != 0 ||
      (reload_hr % 2) != 0) {
    return false;
  }
  const int nw = mm.HardRegNRegs(orig_hr, RtlMachineMode::kDi);
  if (nw != 2) {
    return false;
  }
  const RtlExpr& ppat = prod.pn.pattern;
  if (prod.kind != Insn::Kind::kInsn || ppat.op != RtlOp::kParallel || ppat.children.size() != 2 ||
      ppat.children[0].op != RtlOp::kSet || ppat.children[0].children.size() != 2 ||
      ppat.children[1].op != RtlOp::kClobber || ppat.children[1].children.size() != 1 ||
      ppat.children[0].SetDest().op != RtlOp::kReg || ppat.children[0].SetDest().flags.integrated ||
      ppat.children[0].SetDest().mode != RtlMachineMode::kDi ||
      ppat.children[0].SetDest().RegNo() != reload_hr ||
      ppat.children[1].children[0].op != RtlOp::kReg) {
    return false;
  }
  const int clob_hr = ppat.children[1].children[0].RegNo();
  if (clob_hr >= orig_hr && clob_hr < orig_hr + nw) {
    return false;
  }
  const RtlExpr& rhs = ppat.children[0].SetSrc();
  if (rhs.children.size() != 2) {
    return false;
  }
  auto expr_overlaps_orig = [&](const RtlExpr& e) {
    bool hit = false;
    e.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kReg && !node.IsArgPointerReg()) {
        const int nr = node.RegNo();
        if (nr >= 0 && nr < kMipsFirstPseudoRegister) {
          const int nnw = std::max(1, mm.HardRegNRegs(nr, node.mode));
          if (std::max(orig_hr, nr) < std::min(orig_hr + nw, nr + nnw)) {
            hit = true;
          }
        }
      }
    });
    return hit;
  };
  bool triggered = false;
  if (rhs.op == RtlOp::kPlus) {
    const bool lhs_is_orig = rhs.children[0].op == RtlOp::kReg &&
                             rhs.children[0].mode == RtlMachineMode::kDi &&
                             rhs.children[0].RegNo() == orig_hr;
    triggered = !lhs_is_orig && expr_overlaps_orig(rhs.children[1]);
  } else if ((rhs.op == RtlOp::kAshift || rhs.op == RtlOp::kAshiftrt ||
              rhs.op == RtlOp::kLshiftrt) &&
             rhs.children[1].op == RtlOp::kReg) {
    triggered = expr_overlaps_orig(rhs);
  }
  if (!triggered) {
    return false;
  }
  if (out_orig_hr != nullptr) {
    *out_orig_hr = orig_hr;
  }
  if (out_reload_hr != nullptr) {
    *out_reload_hr = reload_hr;
  }
  return true;
}

}  // namespace detail

namespace {

// Folds `reload1.c:5599-5665` (`emit_reload_insns`) earlyclobber `DImode` GPR
// output-reload moves `(set (reg:DI orig) (reg:DI reload))` back onto their
// owning `PARALLEL` when `reload` dies at the move and no intervening insn
// reads/writes `orig` or `reload`, so `sched1` sees the single pre-reload insn.
void FoldEarlyclobberDiOutputReloadsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, std::vector<bool>* is_spill_overhead,
    absl::btree_map<int, RtlExpr>* md_output_reload_dest,
    absl::btree_map<int, int>* output_reload_orig_owner) {
  const int n = static_cast<int>(post_reload_block.size());
  for (int mv_idx = 1; mv_idx < n; ++mv_idx) {
    if ((*is_spill_overhead)[static_cast<size_t>(mv_idx)]) {
      continue;
    }
    const Insn& mv = post_reload_block[static_cast<size_t>(mv_idx)];
    const RtlExpr& mv_pat = mv.pn.pattern;
    if (mv.kind != Insn::Kind::kInsn || mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
        mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetSrc().op != RtlOp::kReg ||
        mv_pat.SetDest().mode != RtlMachineMode::kDi ||
        mv_pat.SetSrc().mode != RtlMachineMode::kDi) {
      continue;
    }
    const int orig_hr = mv_pat.SetDest().RegNo();
    const int reload_hr = mv_pat.SetSrc().RegNo();
    if (orig_hr < 2 || orig_hr >= kMipsFpRegFirst - 1 || reload_hr < 2 ||
        reload_hr >= kMipsFpRegFirst - 1 || orig_hr == reload_hr || (orig_hr % 2) != 0 ||
        (reload_hr % 2) != 0 || mm.HardRegNRegs(orig_hr, RtlMachineMode::kDi) != 2) {
      continue;
    }
    if (!IsRegDeadAfterInsn(post_reload_block, mv_idx, reload_hr, RtlMachineMode::kDi, mm,
                            live_at_end_s2)) {
      continue;
    }
    for (int prod_idx = mv_idx - 1; prod_idx >= 0; --prod_idx) {
      if ((*is_spill_overhead)[static_cast<size_t>(prod_idx)]) {
        continue;
      }
      const Insn& prod = post_reload_block[static_cast<size_t>(prod_idx)];
      if (prod.kind == Insn::Kind::kCall) {
        break;
      }
      if (!md_output_reload_dest->contains(prod_idx) &&
          TryMatchEarlyclobberDiOutputReload(prod, mv, mm, nullptr, nullptr)) {
        (*is_spill_overhead)[static_cast<size_t>(mv_idx)] = true;
        (*md_output_reload_dest)[prod_idx] = mv_pat.SetDest();
        (*output_reload_orig_owner)[mv_idx] = prod_idx;
        break;
      }
      absl::btree_set<int> p_fdefs;
      absl::btree_set<int> p_pdefs;
      absl::btree_set<int> p_reads;
      CollectFullRegDefs(prod.pn.pattern, &p_fdefs, &p_pdefs);
      CollectRegReads(prod.pn.pattern, &p_reads);
      bool blocked = false;
      for (int w = 0; w < 2; ++w) {
        if (p_fdefs.contains(reload_hr + w) || p_pdefs.contains(reload_hr + w) ||
            p_reads.contains(reload_hr + w) || p_fdefs.contains(orig_hr + w) ||
            p_pdefs.contains(orig_hr + w) || p_reads.contains(orig_hr + w)) {
          blocked = true;
          break;
        }
      }
      if (blocked) {
        break;
      }
    }
  }
}

struct PostReloadSpillAndReloadPlan {
  IntraBlockSpillSlotClassification intra_slots;
  absl::btree_map<int, RtlExpr> md_output_reload_dest;
  absl::btree_map<int, absl::btree_map<int, RtlExpr>> fpr_input_reload_subs;
  SpillSlotFoldInfo cross_spill;
  ConstDoubleReloadFoldInfo const_double_reloads;
};

// Phase 1 of `ProjectPostReloadBlockToSched1View`: identifies matched intra-block
// plain stack-spill / caller-save pairs, folds `reload1.c:emit_reload_insns`
// `hi`/`lo`/FPR/earlyclobber-DI/spill/const-double reload moves, selects
// `view->core_to_orig_index`, and anchors output/input reloads to core insns
// (`toplev.c:3103`, `function.c:5516`, `caller-save.c:455-462`, `reload1.c:5186-6160`).
PostReloadSpillAndReloadPlan IdentifyIntraBlockSpillAndCallerSavePairs(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context,
    bool fp_needed, Sched1ProjectionView* view) {
  PostReloadSpillAndReloadPlan plan;
  const int n = static_cast<int>(post_reload_block.size());
  std::vector<bool> is_spill_overhead(static_cast<size_t>(n), false);
  for (int idx : context.excluded_prologue_body_indices) {
    if (idx >= 0 && idx < n) {
      is_spill_overhead[static_cast<size_t>(idx)] = true;
    }
  }
  plan.intra_slots = ClassifyIntraBlockSpillAndCallerSaveSlots(
      post_reload_block, mm, context.has_stack_addr_escape, &is_spill_overhead, fp_needed,
      context.stack_layout);

  // Fold `reload1.c:emit_reload_insns` `hi`/`lo` (`mfhi`/`mflo`) output-reload
  // and `MD_REGS` input-reload moves (including across `sched2`-interleaved
  // insns) and adjacent cross-bank `"=f"` FPR->GPR (`mfc1`) output-reload moves
  // back onto their producing insn so `sched1` sees the single pre-reload insn.
  absl::btree_map<int, int> output_reload_orig_owner;
  absl::btree_map<int, int> md_input_reload_orig_consumer;
  for (int mv_idx = 1; mv_idx < n; ++mv_idx) {
    if (is_spill_overhead[static_cast<size_t>(mv_idx)]) {
      continue;
    }
    const Insn& mv = post_reload_block[static_cast<size_t>(mv_idx)];
    const RtlExpr& mv_pat = mv.pn.pattern;
    if (mv.kind == Insn::Kind::kInsn && mv_pat.op == RtlOp::kSet && mv_pat.children.size() == 2 &&
        mv_pat.SetSrc().op == RtlOp::kReg &&
        (mv_pat.SetSrc().RegNo() == kMipsMdRegFirst ||
         mv_pat.SetSrc().RegNo() == kMipsMdRegFirst + 1) &&
        mv_pat.SetDest().op == RtlOp::kReg && mv_pat.SetDest().RegNo() >= 0 &&
        mv_pat.SetDest().RegNo() < kMipsFpRegFirst) {
      const int md_reg = mv_pat.SetSrc().RegNo();
      const int dst_reg = mv_pat.SetDest().RegNo();
      const int dst_nregs = mm.HardRegNRegs(dst_reg, mv_pat.SetDest().mode);
      // Per `reload1.c:5641` (`emit_reload_insns`): when a pseudo is allocated
      // to `hi`/`lo` (`MD_REGS`), reload emits an input reload `(set gpr lo/hi)`
      // immediately before the consuming insn, which retains `(REG_DEAD lo/hi)`.
      // Anchor such input reloads before their consumer (`input_reload_before_core`)
      // rather than immediately after the `mult`/`div` producer.
      int md_input_cons_idx = -1;
      for (int c_idx = mv_idx + 1; c_idx < n; ++c_idx) {
        if (is_spill_overhead[static_cast<size_t>(c_idx)]) {
          continue;
        }
        const Insn& cons = post_reload_block[static_cast<size_t>(c_idx)];
        if (cons.kind == Insn::Kind::kCall || cons.kind == Insn::Kind::kJump) {
          break;
        }
        absl::btree_set<int> c_fdefs;
        absl::btree_set<int> c_pdefs;
        absl::btree_set<int> c_reads;
        CollectFullRegDefs(cons.pn.pattern, &c_fdefs, &c_pdefs);
        CollectRegReads(cons.pn.pattern, &c_reads);
        bool reads_dst = false;
        bool defs_dst = false;
        for (int k = 0; k < dst_nregs; ++k) {
          if (c_reads.contains(dst_reg + k)) {
            reads_dst = true;
          }
          if (c_fdefs.contains(dst_reg + k) || c_pdefs.contains(dst_reg + k)) {
            defs_dst = true;
          }
        }
        if (reads_dst && HasRegDeadNoteCovering(cons.pn.reg_notes, md_reg, mm)) {
          md_input_cons_idx = c_idx;
          break;
        }
        if (defs_dst || c_fdefs.contains(md_reg) || c_pdefs.contains(md_reg)) {
          break;
        }
      }
      for (int prod_idx = mv_idx - 1; prod_idx >= 0; --prod_idx) {
        if (is_spill_overhead[static_cast<size_t>(prod_idx)]) {
          continue;
        }
        const Insn& prod = post_reload_block[static_cast<size_t>(prod_idx)];
        if (prod.kind == Insn::Kind::kCall) {
          break;
        }
        RtlExpr gpr_dest;
        if (!plan.md_output_reload_dest.contains(prod_idx) &&
            TryMatchAdjacentMdOutputReload(prod, mv, &gpr_dest)) {
          is_spill_overhead[static_cast<size_t>(mv_idx)] = true;
          plan.md_output_reload_dest[prod_idx] = std::move(gpr_dest);
          if (md_input_cons_idx >= 0) {
            md_input_reload_orig_consumer[mv_idx] = md_input_cons_idx;
          } else {
            output_reload_orig_owner[mv_idx] = prod_idx;
          }
          break;
        }
        absl::btree_set<int> p_fdefs;
        absl::btree_set<int> p_pdefs;
        absl::btree_set<int> p_reads;
        CollectFullRegDefs(prod.pn.pattern, &p_fdefs, &p_pdefs);
        CollectRegReads(prod.pn.pattern, &p_reads);
        if (p_fdefs.contains(md_reg) || p_pdefs.contains(md_reg)) {
          break;
        }
        bool touches_dst = false;
        for (int k = 0; k < dst_nregs; ++k) {
          if (p_fdefs.contains(dst_reg + k) || p_pdefs.contains(dst_reg + k) ||
              p_reads.contains(dst_reg + k)) {
            touches_dst = true;
            break;
          }
        }
        if (touches_dst) {
          break;
        }
      }
    }
  }

  // Fold `reload1.c:5186-5249, 5641` cross-bank `"=f"` FPR->GPR (`mfc1`) output
  // reloads and `"f"` GPR->FPR (`mtc1`) input reloads (including across
  // `sched2`-interleaved independent insns and excluded block-0 prologue saves)
  // back onto their owning FP arithmetic insns.
  absl::btree_map<int, int> fpr_input_reload_orig_consumer;
  FoldFprOutputReloadsForSched1View(post_reload_block, mm, live_at_end_s2, &is_spill_overhead,
                                    &plan.md_output_reload_dest, &output_reload_orig_owner,
                                    &plan.fpr_input_reload_subs);
  FoldFprInputReloadsForSched1View(post_reload_block, mm, live_at_end_s2,
                                   plan.md_output_reload_dest, &is_spill_overhead,
                                   &plan.fpr_input_reload_subs, &fpr_input_reload_orig_consumer);
  FoldEarlyclobberDiOutputReloadsForSched1View(post_reload_block, mm, live_at_end_s2,
                                               &is_spill_overhead, &plan.md_output_reload_dest,
                                               &output_reload_orig_owner);

  // Fold intra-block (non-caller-save) and cross-block `reload1.c:2309-2400, 5641-6160`
  // output-reload spill stores and input-reload spill loads back onto their spilled pseudos.
  plan.cross_spill = FoldReloadSpillsForSched1View(
      post_reload_block, mm, live_at_end_s2, context, plan.intra_slots.slot_stores,
      plan.intra_slots.slot_loads, plan.intra_slots.intra_reload_spill_store_indices,
      plan.intra_slots.intra_reload_spill_load_indices, plan.md_output_reload_dest,
      &is_spill_overhead, &output_reload_orig_owner);
  plan.const_double_reloads = FoldSpilledConstDoubleReloadsForSched1View(
      post_reload_block, mm, live_at_end_s2, context, plan.fpr_input_reload_subs,
      output_reload_orig_owner, plan.cross_spill, &is_spill_overhead);

  // Select core instructions (preserving exact relative order) and anchor output-reload moves
  // and non-caller-save output-spill stores immediately after their owning core insn.
  std::vector<int> orig_to_core(static_cast<size_t>(n), -1);
  for (int i = 0; i < n; ++i) {
    if (!is_spill_overhead[static_cast<size_t>(i)]) {
      orig_to_core[static_cast<size_t>(i)] = static_cast<int>(view->core_to_orig_index.size());
      view->core_to_orig_index.push_back(i);
    }
  }
  if (view->core_to_orig_index.empty()) {
    return plan;
  }
  for (const auto& [mv_orig, prod_orig] : output_reload_orig_owner) {
    if (orig_to_core[static_cast<size_t>(prod_orig)] >= 0) {
      view->output_reload_after_core[mv_orig] = orig_to_core[static_cast<size_t>(prod_orig)];
    }
  }
  for (const auto& [mv_orig, cons_orig] : md_input_reload_orig_consumer) {
    if (orig_to_core[static_cast<size_t>(cons_orig)] >= 0) {
      view->input_reload_before_core[mv_orig] = orig_to_core[static_cast<size_t>(cons_orig)];
    }
  }
  for (const auto& [load_orig, cons_orig] : plan.cross_spill.input_reload_orig_consumer) {
    if (orig_to_core[static_cast<size_t>(cons_orig)] >= 0) {
      view->input_reload_before_core[load_orig] = orig_to_core[static_cast<size_t>(cons_orig)];
    }
  }
  for (const auto& [mv_orig, cons_orig] : fpr_input_reload_orig_consumer) {
    if (orig_to_core[static_cast<size_t>(cons_orig)] >= 0) {
      view->input_reload_before_core[mv_orig] = orig_to_core[static_cast<size_t>(cons_orig)];
    }
  }
  for (const auto& [cd_orig, cons_orig] : plan.const_double_reloads.input_reload_orig_consumer) {
    if (orig_to_core[static_cast<size_t>(cons_orig)] >= 0) {
      view->input_reload_before_core[cd_orig] = orig_to_core[static_cast<size_t>(cons_orig)];
    }
  }
  for (const auto& [s_idx, s_reg] : plan.intra_slots.spill_store_reg) {
    if (!is_spill_overhead[static_cast<size_t>(s_idx)]) {
      continue;
    }
    if (plan.intra_slots.caller_save_store_indices.contains(s_idx)) {
      // `caller-save.c:455-462` (`save_call_clobbered_regs`): emit caller-save
      // stores immediately before the `CALL_INSN` they save across.
      for (int next = s_idx + 1; next < n; ++next) {
        if (post_reload_block[static_cast<size_t>(next)].kind == Insn::Kind::kCall &&
            orig_to_core[static_cast<size_t>(next)] >= 0) {
          view->input_reload_before_core[s_idx] = orig_to_core[static_cast<size_t>(next)];
          break;
        }
      }
      continue;
    }
    for (int prev = s_idx - 1; prev >= 0; --prev) {
      if (orig_to_core[static_cast<size_t>(prev)] < 0) {
        continue;
      }
      absl::btree_set<int> fdefs;
      absl::btree_set<int> pdefs;
      CollectFullRegDefs(post_reload_block[static_cast<size_t>(prev)].pn.pattern, &fdefs, &pdefs);
      if (fdefs.contains(s_reg) || pdefs.contains(s_reg)) {
        view->output_reload_after_core[s_idx] = orig_to_core[static_cast<size_t>(prev)];
      }
      break;
    }
  }
  return plan;
}

// Phase 2 of `ProjectPostReloadBlockToSched1View`: determines ABI-pinned hard-reg
// defs directly feeding `CALL_INSN` `call_fusage` or return-value `(use (reg/i ...))`
// (`function.c:5595`, `calls.c:emit_call_1`).
std::vector<absl::btree_set<int>> CollectAbiCallInterfacePinnedDefs(
    absl::Span<const Insn> post_reload_block, absl::Span<const int> core_to_orig_index,
    const absl::btree_map<int, RtlExpr>& md_output_reload_dest, const MachineModel& mm) {
  const int m = static_cast<int>(core_to_orig_index.size());
  std::vector<absl::btree_set<int>> abi_pinned_defs(static_cast<size_t>(m));
  for (int ci = 0; ci < m; ++ci) {
    const Insn& insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
    absl::btree_set<int> needed_arg_regs;
    if (insn.kind == Insn::Kind::kCall) {
      for (const RtlExpr& fu : insn.pn.call_fusage) {
        CollectRegReads(fu, &needed_arg_regs);
      }
    } else if (insn.pn.pattern.op == RtlOp::kUse && !insn.pn.pattern.children.empty() &&
               insn.pn.pattern.children[0].op == RtlOp::kReg &&
               insn.pn.pattern.children[0].flags.integrated) {
      const RtlExpr& ur = insn.pn.pattern.children[0];
      const int base_r = ur.RegNo();
      if (base_r >= 0 && base_r < kMipsFirstPseudoRegister) {
        const int nr = mm.HardRegNRegs(base_r, ur.mode);
        for (int k = 0; k < nr; ++k) {
          needed_arg_regs.insert(base_r + k);
        }
      }
    } else {
      continue;
    }
    for (int prev = ci - 1; prev >= 0 && !needed_arg_regs.empty(); --prev) {
      const Insn& pinsn = post_reload_block[static_cast<size_t>(core_to_orig_index[prev])];
      if (pinsn.kind == Insn::Kind::kCall) {
        break;
      }
      absl::btree_set<int> fdefs;
      absl::btree_set<int> pdefs;
      const int prev_orig = core_to_orig_index[static_cast<size_t>(prev)];
      CollectFullRegDefs(pinsn.pn.pattern, &fdefs, &pdefs);
      if (const auto md_it = md_output_reload_dest.find(prev_orig);
          md_it != md_output_reload_dest.end()) {
        fdefs.insert(md_it->second.RegNo());
      }
      for (int r : fdefs) {
        if (needed_arg_regs.contains(r)) {
          abi_pinned_defs[static_cast<size_t>(prev)].insert(r);
          needed_arg_regs.erase(r);
        }
      }
      for (int r : pdefs) {
        if (needed_arg_regs.contains(r)) {
          abi_pinned_defs[static_cast<size_t>(prev)].insert(r);
        }
      }
    }
  }
  return abi_pinned_defs;
}

struct IntraBlockSsaState {
  int next_pseudo = kMipsFirstPseudoRegister;
  absl::btree_map<int, int> global_carrier_pseudo;
  absl::btree_map<int, int> cur_pseudo;
  absl::btree_map<int, int> cur_pseudo_birth_ci;
  // Physical hard registers explicitly defined earlier in this block as
  // unrenamed hard registers (ABI call returns/args, `/i` returns, or
  // `local-alloc.c:1798` dying hard-reg suggestion conflicts) so subsequent
  // reads stay bound to that physical hard register rather than inventing a
  // disconnected live-in pseudo.
  absl::btree_set<int> unrenamed_hard_def_regs;
  // Active `optabs.c:2485-2525` (`emit_no_conflict_block`) destination pseudos
  // whose `DImode` hard home `dh` overlaps a still-live `REG_NO_CONFLICT` input
  // register (`local-alloc.c:1317-1352`); keeps `cur_pseudo[src]` intact until
  // the final subword write terminates the input's live range.
  absl::btree_map<int, std::pair<int, int>> active_no_conflict_di;  // dh -> (pseudo, src_reg)
  int last_call_ci = -1;
  absl::btree_set<int> live_in_pseudos;
  // Maps hard reg `hr` read before first write in this block to its view reg
  // (either its fresh live-in pseudo or `hr` itself for unrenamed ABI regs).
  absl::btree_map<int, int> live_in_reg_for_hard_reg;
  // Spilled-slot pseudos keyed by `(stack_offset, version_id)` (`reload1.c:2309, 2362-2367`).
  absl::btree_map<SpillPseudoKey, int> spill_slot_pseudo;
  absl::btree_map<std::string, int> const_double_pseudo;
  absl::btree_set<int> spill_live_out_pseudos;

  int GetOrCreateConstDoublePseudo(const std::string& cd_key) {
    const auto it = const_double_pseudo.find(cd_key);
    if (it != const_double_pseudo.end()) {
      return it->second;
    }
    const int p = next_pseudo++;
    const_double_pseudo[cd_key] = p;
    live_in_pseudos.insert(p);
    return p;
  }

  int GetOrCreateSpillPseudo(const SpillPseudoKey& slot_key, bool mark_live_in_if_new) {
    const auto it = spill_slot_pseudo.find(slot_key);
    if (it != spill_slot_pseudo.end()) {
      return it->second;
    }
    const int p = next_pseudo++;
    spill_slot_pseudo[slot_key] = p;
    if (mark_live_in_if_new) {
      live_in_pseudos.insert(p);
    }
    return p;
  }
};

// Rewrites folded reload operands (`md_output_reload_dest`, FPR/spill/const-double
// input reloads, direct entry-arg spills) on `insn` before SSA renaming.
void ApplyInsnReloadSubstitutionsForSched1Ssa(int orig_idx,
                                              const PostReloadSpillAndReloadPlan& plan,
                                              IntraBlockSsaState* ssa, Insn* insn,
                                              int* direct_entry_arg_spill_pseudo,
                                              int* direct_entry_arg_spill_src_reg,
                                              absl::btree_set<int>* spill_clobbered_hard_regs) {
  if (const auto de_it = plan.cross_spill.direct_entry_arg_spill_slot_by_store.find(orig_idx);
      de_it != plan.cross_spill.direct_entry_arg_spill_slot_by_store.end() &&
      insn->pn.pattern.op == RtlOp::kSet && insn->pn.pattern.children.size() == 2 &&
      insn->pn.pattern.children[1].op == RtlOp::kReg) {
    const int sp_pseudo =
        ssa->GetOrCreateSpillPseudo(de_it->second.first, /*mark_live_in_if_new=*/false);
    if (plan.cross_spill.live_out_slot_keys.contains(de_it->second.first)) {
      ssa->spill_live_out_pseudos.insert(sp_pseudo);
    }
    *direct_entry_arg_spill_pseudo = sp_pseudo;
    *direct_entry_arg_spill_src_reg = insn->pn.pattern.children[1].RegNo();
    RtlExpr dst_reg;
    dst_reg.op = RtlOp::kReg;
    dst_reg.mode = de_it->second.second;
    dst_reg.int_val = sp_pseudo;
    insn->pn.pattern.children[0] = std::move(dst_reg);
  }
  if (const auto md_it = plan.md_output_reload_dest.find(orig_idx);
      md_it != plan.md_output_reload_dest.end()) {
    if (insn->pn.pattern.op == RtlOp::kParallel && !insn->pn.pattern.children.empty() &&
        insn->pn.pattern.children[0].op == RtlOp::kSet &&
        insn->pn.pattern.children[0].children.size() == 2) {
      insn->pn.pattern.children[0].children[0] = md_it->second;
    } else if (insn->pn.pattern.op == RtlOp::kSet && insn->pn.pattern.children.size() == 2) {
      insn->pn.pattern.children[0] = md_it->second;
    }
  }
  if (const auto in_it = plan.fpr_input_reload_subs.find(orig_idx);
      in_it != plan.fpr_input_reload_subs.end()) {
    for (const auto& [fpr, gpr_src] : in_it->second) {
      ReplaceRegInReadPositions(&insn->pn.pattern, fpr, gpr_src);
    }
  }
  if (const auto ip_it = plan.cross_spill.in_place_spill_load_slot_by_orig.find(orig_idx);
      ip_it != plan.cross_spill.in_place_spill_load_slot_by_orig.end() &&
      insn->pn.pattern.op == RtlOp::kSet && insn->pn.pattern.children.size() == 2) {
    const int sp_pseudo =
        ssa->GetOrCreateSpillPseudo(ip_it->second.first, /*mark_live_in_if_new=*/true);
    RtlExpr repl;
    repl.op = RtlOp::kReg;
    repl.mode = ip_it->second.second;
    repl.int_val = sp_pseudo;
    insn->pn.pattern.children[1] = std::move(repl);
  }
  if (const auto sp_in_it = plan.cross_spill.load_subs_by_cons.find(orig_idx);
      sp_in_it != plan.cross_spill.load_subs_by_cons.end()) {
    for (const auto& [l_reg, slot_and_mode] : sp_in_it->second) {
      const int sp_pseudo =
          ssa->GetOrCreateSpillPseudo(slot_and_mode.first, /*mark_live_in_if_new=*/true);
      RtlExpr repl;
      repl.op = RtlOp::kReg;
      repl.mode = slot_and_mode.second;
      repl.int_val = sp_pseudo;
      ReplaceRegInReadPositions(&insn->pn.pattern, l_reg, repl);
      spill_clobbered_hard_regs->insert(l_reg);
    }
  }
  if (const auto cd_in_it = plan.const_double_reloads.load_subs_by_cons.find(orig_idx);
      cd_in_it != plan.const_double_reloads.load_subs_by_cons.end()) {
    for (const auto& [fpr_reg, key_and_mode] : cd_in_it->second) {
      const int cd_pseudo = ssa->GetOrCreateConstDoublePseudo(key_and_mode.first);
      RtlExpr repl;
      repl.op = RtlOp::kReg;
      repl.mode = key_and_mode.second;
      repl.int_val = cd_pseudo;
      ReplaceRegInReadPositions(&insn->pn.pattern, fpr_reg, repl);
      spill_clobbered_hard_regs->insert(fpr_reg);
    }
  }
}

// Preserves ABI call/return semantics while renaming indirect call address
// operands and updating call-clobbered SSA mappings (`calls.c:emit_call_1`).
void RewriteCallOrUseInsnInSched1Ssa(
    int ci, int orig_idx,
    const std::vector<std::tuple<int, int, int>>& preserved_across_call_ranges,
    const MachineModel& mm, bool fp_needed, IntraBlockSsaState* ssa, Insn* insn) {
  if (insn->kind == Insn::Kind::kCall) {
    ssa->last_call_ci = ci;
    // Per `calls.c:emit_call_1`: the `CALL_INSN` return destination
    // (`$v0`/`$f0`), `(clobber $ra)`, and `call_fusage` are physical ABI
    // hard regs, whereas an indirect call's callee address `(call (mem
    // <addr>) ...)` is an ordinary pseudo in `.07.sched` that `lreg`/`greg`
    // may allocate to any caller-saved GPR (commonly `$v0`/`$v1`).
    absl::btree_map<int, int> call_addr_map;
    for (const auto& [hr, pr] : ssa->cur_pseudo) {
      if (!IsFixedOrSpecialHardReg(hr, fp_needed)) {
        call_addr_map[hr] = pr;
      }
    }
    auto rename_call_addrs = [&](auto&& self, RtlExpr* node) -> void {
      if (node->op == RtlOp::kCall && !node->children.empty()) {
        RenameExprRegs(&node->children[0], call_addr_map);
        return;
      }
      for (RtlExpr& ch : node->children) {
        self(self, &ch);
      }
    };
    rename_call_addrs(rename_call_addrs, &insn->pn.pattern);
    // Clobber call-used hard regs not protected by an excluded caller-save spill range.
    for (auto it = ssa->cur_pseudo.begin(); it != ssa->cur_pseudo.end();) {
      const int hr = it->first;
      bool preserved = false;
      for (const auto& [s_idx, l_idx, s_reg] : preserved_across_call_ranges) {
        if (s_reg == hr && s_idx < orig_idx && orig_idx < l_idx) {
          preserved = true;
          break;
        }
      }
      if (mm.CallUsed(hr) && !preserved) {
        ssa->cur_pseudo_birth_ci.erase(hr);
        it = ssa->cur_pseudo.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = ssa->unrenamed_hard_def_regs.begin();
         it != ssa->unrenamed_hard_def_regs.end();) {
      if (mm.CallUsed(*it)) {
        it = ssa->unrenamed_hard_def_regs.erase(it);
      } else {
        ++it;
      }
    }
    absl::btree_set<int> call_fdefs;
    absl::btree_set<int> call_pdefs;
    CollectFullRegDefs(insn->pn.pattern, &call_fdefs, &call_pdefs);
    for (int r : call_fdefs) {
      if (r >= 0 && r < kMipsFirstPseudoRegister && !IsFixedOrSpecialHardReg(r, fp_needed)) {
        ssa->cur_pseudo.erase(r);
        ssa->cur_pseudo_birth_ci.erase(r);
        ssa->unrenamed_hard_def_regs.insert(r);
      }
    }
  }
  insn->pn.reg_notes.clear();
}

// Phase 3 of `ProjectPostReloadBlockToSched1View`: forward intra-block SSA
// live-range splitting preserving `flow.c:2506-2515`, `local-alloc.c:1266-1450`,
// `optabs.c:2485-2525`, and `reload1.c:2641` (`eliminate_regs`) invariants.
IntraBlockSsaState RunIntraBlockSsaSplitting(
    absl::Span<const Insn> post_reload_block, const PostReloadSpillAndReloadPlan& plan,
    const std::vector<absl::btree_set<int>>& abi_pinned_defs, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context,
    bool fp_needed, Sched1ProjectionView* view) {
  const int m = static_cast<int>(view->core_to_orig_index.size());
  const absl::btree_set<int> global_carrier_regs = FindCoalescedGlobalCarrierHardRegs(
      post_reload_block, view->core_to_orig_index, abi_pinned_defs, live_at_end_s2, context, mm);
  const std::vector<uint64_t> live_after_core = ComputeCoreHardRegLiveAfterMasks(
      post_reload_block, view->core_to_orig_index, live_at_end_s2, plan.cross_spill,
      plan.intra_slots.preserved_across_call_ranges, mm);
  IntraBlockSsaState ssa;
  view->core_insns.reserve(static_cast<size_t>(m));
  bool in_entry_param_prefix = context.is_entry_block;
  int prev_orig_idx = -1;

  for (int ci = 0; ci < m; ++ci) {
    const int orig_idx = view->core_to_orig_index[static_cast<size_t>(ci)];
    for (int skipped = prev_orig_idx + 1; skipped < orig_idx; ++skipped) {
      if (const auto mit = plan.cross_spill.multi_use_load_slot_by_orig.find(skipped);
          mit != plan.cross_spill.multi_use_load_slot_by_orig.end()) {
        const int sp_pseudo =
            ssa.GetOrCreateSpillPseudo(mit->second.second, /*mark_live_in_if_new=*/false);
        ssa.cur_pseudo[mit->second.first] = sp_pseudo;
        ssa.cur_pseudo_birth_ci[mit->second.first] = -1;
        ssa.unrenamed_hard_def_regs.erase(mit->second.first);
      }
    }
    prev_orig_idx = orig_idx;
    Insn insn = post_reload_block[static_cast<size_t>(orig_idx)];
    insn.luid = ci;
    int direct_entry_arg_spill_pseudo = -1;
    int direct_entry_arg_spill_src_reg = -1;
    absl::btree_set<int> spill_clobbered_hard_regs;
    ApplyInsnReloadSubstitutionsForSched1Ssa(
        orig_idx, plan, &ssa, &insn, &direct_entry_arg_spill_pseudo,
        &direct_entry_arg_spill_src_reg, &spill_clobbered_hard_regs);
    const bool is_leading_entry_param_copy = in_entry_param_prefix && IsBlock0PinnedParamCopy(insn);
    if (!is_leading_entry_param_copy) {
      in_entry_param_prefix = false;
    }

    if (insn.kind == Insn::Kind::kCall || insn.pn.pattern.op == RtlOp::kUse) {
      RewriteCallOrUseInsnInSched1Ssa(ci, orig_idx, plan.intra_slots.preserved_across_call_ranges,
                                      mm, fp_needed, &ssa, &insn);
      view->core_insns.push_back(std::move(insn));
      continue;
    }

    absl::btree_set<int> reads;
    CollectRegReads(insn.pn.pattern, &reads);
    absl::btree_map<int, int> read_map;
    for (int r : reads) {
      if (r >= kMipsFirstPseudoRegister || IsFixedOrSpecialHardReg(r, fp_needed)) {
        continue;
      }
      if (const auto it = ssa.cur_pseudo.find(r); it != ssa.cur_pseudo.end()) {
        read_map[r] = it->second;
      } else if (ssa.unrenamed_hard_def_regs.contains(r)) {
        // Keep reads of an earlier unrenamed hard-reg def in this block bound
        // to the physical hard register so `def -> use` edges stay intact.
      } else if (!IsAbiCallInterfaceHardReg(r) || global_carrier_regs.contains(r)) {
        const int fresh = ssa.next_pseudo++;
        ssa.cur_pseudo[r] = fresh;
        ssa.cur_pseudo_birth_ci[r] = -1;
        ssa.live_in_pseudos.insert(fresh);
        ssa.live_in_reg_for_hard_reg.try_emplace(r, fresh);
        if (global_carrier_regs.contains(r)) {
          ssa.global_carrier_pseudo[r] = fresh;
        }
        read_map[r] = fresh;
      } else {
        ssa.live_in_reg_for_hard_reg.try_emplace(r, r);
      }
    }

    absl::btree_set<int> full_defs;
    absl::btree_set<int> partial_defs;
    CollectFullRegDefs(insn.pn.pattern, &full_defs, &partial_defs);

    absl::btree_map<int, int> def_map;
    // Handle `optabs.c:2485-2525` (`emit_no_conflict_block`) `REG_LIBCALL`
    // `(clobber (reg:DI dh))` overlapping a still-live `REG_NO_CONFLICT` input
    // without clobbering `cur_pseudo[src_reg]` before the subword writes.
    bool handled_no_conflict_clobber = false;
    if (insn.pn.pattern.op == RtlOp::kClobber && insn.pn.pattern.children.size() == 1 &&
        insn.pn.pattern.children[0].op == RtlOp::kReg &&
        insn.pn.pattern.children[0].mode == RtlMachineMode::kDi &&
        insn.pn.pattern.children[0].RegNo() >= 0 &&
        insn.pn.pattern.children[0].RegNo() < kMipsFirstPseudoRegister &&
        std::any_of(insn.pn.reg_notes.begin(), insn.pn.reg_notes.end(),
                    [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegLibcall; })) {
      const int dh = insn.pn.pattern.children[0].RegNo();
      int nc_src = -1;
      for (int k = ci + 1; k < m; ++k) {
        const RtlPatternAndNotes& kpn =
            post_reload_block[static_cast<size_t>(view->core_to_orig_index[k])].pn;
        if (kpn.pattern.op == RtlOp::kSet && kpn.pattern.children.size() == 2 &&
            kpn.pattern.SetDest().op == RtlOp::kSubreg && !kpn.pattern.SetDest().children.empty() &&
            kpn.pattern.SetDest().children[0].op == RtlOp::kReg &&
            kpn.pattern.SetDest().children[0].mode == RtlMachineMode::kDi &&
            kpn.pattern.SetDest().children[0].RegNo() == dh) {
          for (const RtlRegNote& n : kpn.reg_notes) {
            if (n.kind == RtlMachineMode::kRegNoConflict && n.datum.op == RtlOp::kReg &&
                (n.datum.RegNo() == dh || n.datum.RegNo() == dh + 1)) {
              nc_src = n.datum.RegNo();
              break;
            }
          }
        }
        if (nc_src >= 0) {
          break;
        }
      }
      if (nc_src >= 0) {
        const int fresh = ssa.next_pseudo++;
        def_map[dh] = fresh;
        ssa.active_no_conflict_di[dh] = {fresh, nc_src};
        handled_no_conflict_clobber = true;
      }
    }
    for (int r : partial_defs) {
      if (handled_no_conflict_clobber || r >= kMipsFirstPseudoRegister ||
          IsFixedOrSpecialHardReg(r, fp_needed) ||
          abi_pinned_defs[static_cast<size_t>(ci)].contains(r)) {
        continue;
      }
      if (const auto nc_it = ssa.active_no_conflict_di.find(r);
          nc_it != ssa.active_no_conflict_di.end()) {
        const int nc_pseudo = nc_it->second.first;
        const int nc_src = nc_it->second.second;
        def_map[r] = nc_pseudo;
        if (HasRegDeadNoteCovering(insn.pn.reg_notes, nc_src, mm)) {
          ssa.cur_pseudo[r] = nc_pseudo;
          ssa.cur_pseudo_birth_ci[r] = ci;
          ssa.active_no_conflict_di.erase(nc_it);
        }
        continue;
      }
      if (const auto it = ssa.cur_pseudo.find(r); it != ssa.cur_pseudo.end()) {
        def_map[r] = it->second;
      } else if (!IsAbiCallInterfaceHardReg(r)) {
        const int fresh = ssa.next_pseudo++;
        ssa.cur_pseudo[r] = fresh;
        ssa.cur_pseudo_birth_ci[r] = ci;
        def_map[r] = fresh;
      }
    }
    for (int r : full_defs) {
      if (handled_no_conflict_clobber || r >= kMipsFirstPseudoRegister ||
          IsFixedOrSpecialHardReg(r, fp_needed)) {
        continue;
      }
      if (abi_pinned_defs[static_cast<size_t>(ci)].contains(r)) {
        ssa.cur_pseudo.erase(r);
        ssa.cur_pseudo_birth_ci.erase(r);
        ssa.unrenamed_hard_def_regs.insert(r);
        continue;
      }
      // Keep `/i` return-value sets unrenamed (`RenameExprRegs` also skips `/i`).
      const RtlExpr* s = SingleSetOf(insn.pn.pattern);
      if (s != nullptr && s->SetDest().op == RtlOp::kReg && s->SetDest().RegNo() == r &&
          s->SetDest().flags.integrated) {
        ssa.cur_pseudo.erase(r);
        ssa.cur_pseudo_birth_ci.erase(r);
        ssa.unrenamed_hard_def_regs.insert(r);
        continue;
      }
      // `flow.c:2506-2515` never attaches `REG_DEAD P` when an insn both reads
      // and writes the same pseudo `P`. Therefore:
      //  - `(set r (op r ...))` WITHOUT `REG_DEAD r` => same multi-set pseudo
      //    (induction variable / accumulator, `reg_n_sets >= 2`, `birthing_insn_p == false`),
      //    UNLESS it is a post-call dying-input reuse tied by `local-alloc.c:1266-1300, 1855`
      //    (`combine_regs`) whose `reg_n_calls_crossed == 0` locks it after the call
      //    (`sched.c:1732`);
      //  - `(set r (op r ...))` WITH `REG_DEAD r` => dying-input register reuse
      //    (`local-alloc.c:1395-1450`), so LHS is a distinct fresh single-set pseudo!
      const auto birth_it = ssa.cur_pseudo_birth_ci.find(r);
      const int birth_ci = (birth_it != ssa.cur_pseudo_birth_ci.end()) ? birth_it->second : -1;
      const bool has_reg_dead = HasRegDeadNoteCovering(insn.pn.reg_notes, r, mm);
      const bool forced_combine_split =
          !has_reg_dead && context.forced_combine_regs_split_body_indices.contains(orig_idx);
      const bool post_call_combine_split =
          context.allow_post_call_combine_regs_split && !has_reg_dead &&
          IsPostCallCombineRegsSplitCandidate(post_reload_block, view->core_to_orig_index, ci, r,
                                              birth_ci, ssa.last_call_ci, live_at_end_s2, mm);
      const bool general_combine_split =
          (context.allow_general_combine_regs_split ||
           (context.allow_call_used_general_combine_regs_split && mm.CallUsed(r))) &&
          !has_reg_dead && !post_call_combine_split &&
          IsGeneralCombineRegsRmwSplitCandidate(post_reload_block, view->core_to_orig_index, ci, r,
                                                birth_ci, live_at_end_s2, mm);
      const bool implicit_combine_split =
          forced_combine_split || post_call_combine_split || general_combine_split;
      // `function.c:assign_parms` + `flow.c:2506-2515`: in block 0, an incoming
      // ABI parameter hard register (`$a0-$a3`, `$f12`, `$f14`) read before any
      // prior def in the block is a physical argument register, never the same
      // pseudo as a computed result written back into that hard register.
      const bool incoming_abi_arg_rmw =
          context.is_entry_block && IsAbiCallInterfaceHardReg(r) && !ssa.cur_pseudo.contains(r);
      // `emit-rtl.c:515-517` (`gen_reg_rtx`): a pre-allocation pseudo register
      // has one fixed machine mode. An in-place mode-changing conversion/overwrite
      // `(set (reg:M2 r) (... (reg:M1 r) ...))` where every read of `r` in the
      // RHS has a distinct mode `M1 != M2` (e.g. `float_extend:DF (reg:SF $f24)`
      // -> `(reg:DF $f24)`) cannot be the same pre-allocation pseudo even when
      // `REG_DEAD (reg:M1 r)` was omitted (such as delay-slot `cvt.d.s`).
      bool mode_changing_rmw = false;
      if (reads.contains(r) && s != nullptr && s->SetDest().op == RtlOp::kReg &&
          s->SetDest().RegNo() == r && s->SetDest().mode != RtlMachineMode::kVoid &&
          s->SetDest().mode != RtlMachineMode::kNone) {
        const RtlMachineMode dst_mode = s->SetDest().mode;
        bool saw_r_read = false;
        bool all_diff_mode = true;
        s->SetSrc().Visit([&](const RtlExpr& node) {
          if (node.op == RtlOp::kReg && node.RegNo() == r) {
            saw_r_read = true;
            if (node.mode == dst_mode || node.mode == RtlMachineMode::kVoid ||
                node.mode == RtlMachineMode::kNone) {
              all_diff_mode = false;
            }
          }
        });
        mode_changing_rmw = saw_r_read && all_diff_mode;
      }
      const auto sp_out_it = plan.cross_spill.store_slot_by_prod.find(orig_idx);
      const bool feeds_cross_spill = sp_out_it != plan.cross_spill.store_slot_by_prod.end() &&
                                     s != nullptr && s->SetDest().op == RtlOp::kReg &&
                                     s->SetDest().RegNo() == r;
      if (reads.contains(r) && !has_reg_dead && !implicit_combine_split && !incoming_abi_arg_rmw &&
          !mode_changing_rmw && !feeds_cross_spill) {
        if (const auto it = read_map.find(r); it != read_map.end()) {
          def_map[r] = it->second;
          ssa.cur_pseudo[r] = it->second;
        }
        continue;
      }
      if (!has_reg_dead && !implicit_combine_split && !mode_changing_rmw && !feeds_cross_spill) {
        if (const auto git = ssa.global_carrier_pseudo.find(r);
            git != ssa.global_carrier_pseudo.end()) {
          def_map[r] = git->second;
          ssa.cur_pseudo[r] = git->second;
          continue;
        }
      }
      if (reads.contains(r) && implicit_combine_split) {
        view->implicit_combine_regs_split_orig_indices.push_back(orig_idx);
      }
      if (!feeds_cross_spill && !is_leading_entry_param_copy &&
          (HasLocalDyingHardRegSuggestionConflict(post_reload_block, view->core_to_orig_index, ci,
                                                  r, reads, read_map, ssa.unrenamed_hard_def_regs,
                                                  context.is_entry_block, live_at_end_s2, mm,
                                                  fp_needed) ||
           HasLocalLowerAllocOrderFreeRegConflict(post_reload_block, view->core_to_orig_index,
                                                  live_after_core, ci, r, reads, live_at_end_s2,
                                                  plan.cross_spill, mm))) {
        ssa.cur_pseudo.erase(r);
        ssa.cur_pseudo_birth_ci.erase(r);
        ssa.unrenamed_hard_def_regs.insert(r);
        continue;
      }
      int fresh = -1;
      if (feeds_cross_spill) {
        fresh = ssa.GetOrCreateSpillPseudo(sp_out_it->second, /*mark_live_in_if_new=*/false);
        if (plan.cross_spill.live_out_slot_keys.contains(sp_out_it->second)) {
          ssa.spill_live_out_pseudos.insert(fresh);
        }
      } else {
        fresh = ssa.next_pseudo++;
      }
      ssa.cur_pseudo[r] = fresh;
      ssa.cur_pseudo_birth_ci[r] = ci;
      ssa.unrenamed_hard_def_regs.erase(r);
      def_map[r] = fresh;
    }
    for (int clob_r : spill_clobbered_hard_regs) {
      if (!full_defs.contains(clob_r) && !partial_defs.contains(clob_r)) {
        ssa.cur_pseudo.erase(clob_r);
        ssa.cur_pseudo_birth_ci.erase(clob_r);
        ssa.unrenamed_hard_def_regs.erase(clob_r);
      }
    }
    if (direct_entry_arg_spill_pseudo >= 0 && direct_entry_arg_spill_src_reg >= 0 &&
        !HasRegDeadNoteCovering(insn.pn.reg_notes, direct_entry_arg_spill_src_reg, mm)) {
      ssa.cur_pseudo[direct_entry_arg_spill_src_reg] = direct_entry_arg_spill_pseudo;
      ssa.cur_pseudo_birth_ci[direct_entry_arg_spill_src_reg] = ci;
      ssa.unrenamed_hard_def_regs.erase(direct_entry_arg_spill_src_reg);
    }

    // Apply `read_map` to uses (RHS / MEM dest address) and `def_map` to defined dests.
    auto rename_set_or_clobber = [&](RtlExpr* node) {
      if (node->op == RtlOp::kSet && node->children.size() == 2) {
        RenameExprRegs(&node->children[1], read_map);
        if (node->children[0].op == RtlOp::kMem) {
          RenameExprRegs(&node->children[0], read_map);
        } else {
          RenameExprRegs(&node->children[0], def_map);
        }
      } else if (node->op == RtlOp::kClobber && !node->children.empty()) {
        RenameExprRegs(&node->children[0], def_map);
      } else {
        RenameExprRegs(node, read_map);
      }
    };
    if (insn.pn.pattern.op == RtlOp::kParallel) {
      for (RtlExpr& ch : insn.pn.pattern.children) {
        rename_set_or_clobber(&ch);
      }
    } else {
      rename_set_or_clobber(&insn.pn.pattern);
    }
    // Mirror `reload1.c:2641` (`eliminate_regs`, `mips.h:1649-1651`):
    // un-eliminate `$sp`-relative local stack slots (`$fp`) and incoming stack
    // arguments (`argp`) so `sched1` sees the exact pre-reload distinct stack
    // base registers for `sched.c:true_dependence` memory aliasing.
    UneliminateStackMemsForSched1(&insn.pn.pattern, context.stack_layout);

    // Keep non-liveness notes (`REG_EQUIV`/`REG_EQUAL`) with renamed regs; drop stale `REG_DEAD`.
    std::vector<RtlRegNote> kept_notes;
    for (RtlRegNote note : insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) {
        continue;
      }
      RenameExprRegs(&note.datum, read_map);
      kept_notes.push_back(std::move(note));
    }
    insn.pn.reg_notes = std::move(kept_notes);
    view->core_insns.push_back(std::move(insn));
  }
  return ssa;
}

}  // namespace

Sched1ProjectionView ProjectPostReloadBlockToSched1View(absl::Span<const Insn> post_reload_block,
                                                        const MachineModel& mm,
                                                        const absl::btree_set<int>& live_at_end_s2,
                                                        const Sched1ProjectionContext& context) {
  Sched1ProjectionView view;
  if (post_reload_block.empty()) {
    return view;
  }

  const bool fp_needed = !context.stack_layout.valid || context.stack_layout.frame_pointer_needed;

  // 1. Identify spill/caller-save pairs and fold post-reload moves onto core insns.
  const PostReloadSpillAndReloadPlan plan = IdentifyIntraBlockSpillAndCallerSavePairs(
      post_reload_block, mm, live_at_end_s2, context, fp_needed, &view);
  if (view.core_to_orig_index.empty()) {
    return view;
  }

  // 2. Determine ABI-pinned hard-reg defs feeding `CALL_INSN` `call_fusage` or `(use (reg/i ...))`.
  const std::vector<absl::btree_set<int>> abi_pinned_defs = CollectAbiCallInterfacePinnedDefs(
      post_reload_block, view.core_to_orig_index, plan.md_output_reload_dest, mm);

  // 3. Forward intra-block SSA live-range splitting.
  const IntraBlockSsaState ssa = RunIntraBlockSsaSplitting(
      post_reload_block, plan, abi_pinned_defs, mm, live_at_end_s2, context, fp_needed, &view);

  // 4. Derive `live_at_end_s1` and `facts_s1` (`reg_n_sets`, `reg_n_calls_crossed`, `max_reg_num`).
  DeriveSched1ViewFacts(context, mm, live_at_end_s2, ssa.cur_pseudo, ssa.live_in_pseudos,
                        ssa.live_in_reg_for_hard_reg, ssa.spill_slot_pseudo,
                        ssa.spill_live_out_pseudos, ssa.next_pseudo, &view);

  // 5. Block-0 leading entry-parameter copy pinning (`sched.c:3191-3215`).
  if (context.is_entry_block) {
    PinLeadingEntryParamCopiesForSched1View(&view);
  }

  return view;
}

namespace {

bool IsReturnInternalPattern(const RtlExpr& pat) {
  return pat.op == RtlOp::kParallel && !pat.children.empty() &&
         pat.children[0].op == RtlOp::kReturn;
}

bool IsPrologueSpDecrementPattern(const RtlExpr& pat) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || dst.RegNo() != kMipsStackPointerRegno || src.children.size() != 2 ||
      src.children[0].op != RtlOp::kReg || src.children[0].RegNo() != kMipsStackPointerRegno) {
    return false;
  }
  if (src.op == RtlOp::kMinus) {
    return (src.children[1].op == RtlOp::kConstInt && src.children[1].int_val > 0) ||
           (src.children[1].op == RtlOp::kReg && src.children[1].RegNo() == kMipsTemp1Regno);
  }
  return src.op == RtlOp::kPlus && src.children[1].op == RtlOp::kConstInt &&
         src.children[1].int_val < 0;
}

bool IsPrologueFpSetupPattern(const RtlExpr& pat) {
  return pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
         pat.children[0].RegNo() == kMipsFramePointerRegno && !pat.children[0].flags.volatil &&
         pat.children[1].op == RtlOp::kReg && pat.children[1].RegNo() == kMipsStackPointerRegno;
}

bool IsProloguePrefixPattern(const RtlExpr& pat) {
  if (IsPrologueSpDecrementPattern(pat) || IsPrologueFpSetupPattern(pat)) {
    return true;
  }
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (dst.op == RtlOp::kMem && !dst.children.empty() && src.op == RtlOp::kReg) {
    const int sr = src.RegNo();
    const bool is_saved = (sr >= kMipsCalleeSavedGprFirst && sr <= kMipsCalleeSavedGprLast) ||
                          sr == kMipsFramePointerRegno || sr == kMipsReturnAddrRegno ||
                          (sr >= kMipsCalleeSavedFprFirst && sr <= kMipsFpRegLast);
    if (is_saved) {
      const RtlExpr& addr = dst.children[0];
      const bool sp_based = (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
                            (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                             addr.children[0].op == RtlOp::kReg &&
                             addr.children[0].RegNo() == kMipsStackPointerRegno &&
                             addr.children[1].op == RtlOp::kConstInt);
      if (sp_based) {
        return true;
      }
    }
  }
  return false;
}

bool IsEpilogueSuffixPattern(const RtlExpr& pat) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (dst.op == RtlOp::kReg && dst.RegNo() == kMipsStackPointerRegno) {
    if (src.op == RtlOp::kReg && src.RegNo() == kMipsFramePointerRegno) {
      return true;
    }
    if (src.op == RtlOp::kPlus && src.children.size() == 2 && src.children[0].op == RtlOp::kReg &&
        src.children[0].RegNo() == kMipsStackPointerRegno &&
        src.children[1].op == RtlOp::kConstInt) {
      return true;
    }
  }
  if (dst.op == RtlOp::kReg && src.op == RtlOp::kMem && !src.children.empty()) {
    const int dr = dst.RegNo();
    const bool is_saved = (dr >= kMipsCalleeSavedGprFirst && dr <= kMipsCalleeSavedGprLast) ||
                          dr == kMipsFramePointerRegno || dr == kMipsReturnAddrRegno ||
                          (dr >= kMipsCalleeSavedFprFirst && dr <= kMipsFpRegLast);
    if (is_saved) {
      const RtlExpr& addr = src.children[0];
      const bool sp_based = (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
                            (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                             addr.children[0].op == RtlOp::kReg &&
                             addr.children[0].RegNo() == kMipsStackPointerRegno &&
                             addr.children[1].op == RtlOp::kConstInt);
      if (sp_based) {
        return true;
      }
    }
  }
  return false;
}

enum class PreReloadStackBase {
  kOther,
  kSp,
  kFp,
  kArgp,
};

PreReloadStackBase ClassifyPreReloadStackBase(const RtlExpr& mem,
                                              const PostReloadStackLayout& layout) {
  if (!layout.valid || mem.op != RtlOp::kMem || mem.children.empty()) {
    return PreReloadStackBase::kOther;
  }
  const RtlExpr& a = mem.children[0];
  int base_reg = -1;
  int64_t off = 0;
  if (a.op == RtlOp::kReg) {
    base_reg = a.RegNo();
  } else if (a.op == RtlOp::kPlus && a.children.size() == 2 && a.children[0].op == RtlOp::kReg &&
             a.children[1].op == RtlOp::kConstInt) {
    base_reg = a.children[0].RegNo();
    off = a.children[1].int_val;
  } else {
    return PreReloadStackBase::kOther;
  }
  if (!layout.frame_pointer_needed) {
    if (base_reg != kMipsStackPointerRegno) {
      return PreReloadStackBase::kOther;
    }
    if (layout.frame_size > 0) {
      if (off >= layout.frame_size) {
        return PreReloadStackBase::kArgp;
      }
      if (off >= layout.starting_frame_offset) {
        return PreReloadStackBase::kFp;
      }
      return PreReloadStackBase::kSp;
    }
    return (off >= 0) ? PreReloadStackBase::kArgp : PreReloadStackBase::kSp;
  }
  if (base_reg == kMipsFramePointerRegno) {
    return (off >= layout.frame_size) ? PreReloadStackBase::kArgp : PreReloadStackBase::kFp;
  }
  if (base_reg == kMipsStackPointerRegno) {
    return PreReloadStackBase::kSp;
  }
  return PreReloadStackBase::kOther;
}

// Mirrors `local-alloc.c:547` + `sched.c:636-669, 817-840` (`true_dependence`)
// on post-reload `$sp`/`$fp`-addressed `MEM`s whose pre-reload bases differed
// (`$sp` vs `$fp` vs `argp`) before `reload1.c:eliminate_regs` (`mips.h:1649, 1756`).
bool PreReloadDistinctStackBasesConflict(const RtlExpr& write_mem, const RtlExpr& read_mem,
                                         const PostReloadStackLayout& layout) {
  if (read_mem.flags.unchanging && !write_mem.flags.unchanging) {
    return false;
  }
  const PreReloadStackBase wb = ClassifyPreReloadStackBase(write_mem, layout);
  const PreReloadStackBase rb = ClassifyPreReloadStackBase(read_mem, layout);
  if (wb == PreReloadStackBase::kOther || rb == PreReloadStackBase::kOther || wb == rb) {
    return false;
  }
  const bool w_varies = (wb == PreReloadStackBase::kSp);
  const bool r_varies = (rb == PreReloadStackBase::kSp);
  if (write_mem.flags.in_struct && w_varies && write_mem.mode != RtlMachineMode::kQi &&
      !read_mem.flags.in_struct && !r_varies) {
    return false;
  }
  if (read_mem.flags.in_struct && r_varies && read_mem.mode != RtlMachineMode::kQi &&
      !write_mem.flags.in_struct && !w_varies) {
    return false;
  }
  return true;
}

}  // namespace

namespace detail {

// Exact `local-alloc.c:540-603` (`validate_equiv_mem`) predicate using
// `TrueDependence` (`local-alloc.c:547`) and `REG_DEAD` death cutoff
// (`local-alloc.c:580-581`).
bool ValidateEquivMemForPostReloadGate(absl::Span<const Insn> seq, absl::Span<const int> orig_ids,
                                       int start_idx, int target_reg, const RtlExpr& mem_expr,
                                       const RegFacts& facts_s2, const MachineModel& mm,
                                       const PostReloadStackLayout& stack_layout,
                                       const absl::btree_set<int>& excluded_prologue_orig_ids) {
  if (mem_expr.op != RtlOp::kMem || mem_expr.children.empty() || mem_expr.flags.volatil) {
    return false;
  }
  absl::btree_set<int> addr_regs;
  auto insert_span = [&](int r, RtlMachineMode mode, absl::btree_set<int>* s) {
    if (r < 0 || r >= kMipsFirstPseudoRegister) {
      return;
    }
    const int nr = mm.HardRegNRegs(r, mode);
    for (int k = 0; k < nr; ++k) {
      s->insert(r + k);
    }
  };
  auto collect_addr = [&](auto& self, const RtlExpr& node) -> void {
    if (node.op == RtlOp::kReg) {
      insert_span(node.RegNo(), node.mode, &addr_regs);
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch);
    }
  };
  collect_addr(collect_addr, mem_expr.children[0]);

  auto store_modifies_equiv = [&](const RtlExpr& pat) -> bool {
    bool modified = false;
    auto check_elem = [&](const RtlExpr& elem) {
      if (modified) {
        return;
      }
      if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
        return;
      }
      const RtlExpr& dest = elem.children[0];
      if (dest.op == RtlOp::kMem) {
        if (TrueDependence(dest, mem_expr, facts_s2, Phase::kSched2) ||
            PreReloadDistinctStackBasesConflict(dest, mem_expr, stack_layout)) {
          modified = true;
        }
        return;
      }
      const RtlExpr* inner = &dest;
      while ((inner->op == RtlOp::kSubreg || inner->op == RtlOp::kStrictLowPart ||
              inner->op == RtlOp::kZeroExtract || inner->op == RtlOp::kSignExtract) &&
             !inner->children.empty()) {
        inner = &inner->children[0];
      }
      if (inner->op == RtlOp::kReg && inner->RegNo() >= 0 &&
          inner->RegNo() < kMipsFirstPseudoRegister) {
        const int nr = mm.HardRegNRegs(inner->RegNo(), inner->mode);
        for (int k = 0; k < nr; ++k) {
          if (addr_regs.contains(inner->RegNo() + k)) {
            modified = true;
            return;
          }
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        check_elem(ch);
      }
    } else {
      check_elem(pat);
    }
    return modified;
  };

  const int n = static_cast<int>(seq.size());
  for (int j = start_idx; j < n; ++j) {
    if (excluded_prologue_orig_ids.contains(orig_ids[static_cast<size_t>(j)])) {
      continue;
    }
    if (j > start_idx &&
        HasRegDeadNoteCovering(seq[static_cast<size_t>(j)].pn.reg_notes, target_reg, mm)) {
      return true;
    }
    if (seq[static_cast<size_t>(j)].kind == Insn::Kind::kCall && !mem_expr.flags.unchanging &&
        !seq[static_cast<size_t>(j)].is_const_call) {
      return false;
    }
    if (store_modifies_equiv(seq[static_cast<size_t>(j)].pn.pattern)) {
      return false;
    }
    for (const RtlRegNote& note : seq[static_cast<size_t>(j)].pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
          note.datum.RegNo() >= 0 && note.datum.RegNo() < kMipsFirstPseudoRegister) {
        const int nr = mm.HardRegNRegs(note.datum.RegNo(), note.datum.mode);
        for (int k = 0; k < nr; ++k) {
          if (addr_regs.contains(note.datum.RegNo() + k)) {
            return false;
          }
        }
      }
    }
  }
  return false;
}

// Mirrors `local-alloc.c:609-665, 996` (`memref_used_between_p`), accounting for
// `reload1.c:eliminate_regs` having rewritten pre-reload `$fp`/`argp` accesses
// onto `$sp` (`29`) so disjoint post-reload `$sp` offsets from distinct pre-reload
// stack bases still alias under `local-alloc.c`'s pre-reload `true_dependence`.
bool MemrefUsedBetweenForPostReloadGate(absl::Span<const Insn> seq, absl::Span<const int> orig_ids,
                                        int start_idx, int end_idx, const RtlExpr& mem_expr,
                                        const RegFacts& facts_s2,
                                        const PostReloadStackLayout& stack_layout,
                                        const absl::btree_set<int>& excluded_prologue_orig_ids) {
  auto is_sp_addressed = [](const RtlExpr& m) {
    if (m.op != RtlOp::kMem || m.children.empty()) {
      return false;
    }
    const RtlExpr& addr = m.children[0];
    return (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
           (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
            addr.children[0].op == RtlOp::kReg &&
            addr.children[0].RegNo() == kMipsStackPointerRegno);
  };
  const bool store_uses_sp = !stack_layout.valid && is_sp_addressed(mem_expr);
  bool used = false;
  auto walk = [&](auto& self, const RtlExpr& node) -> void {
    if (used) {
      return;
    }
    if (node.op == RtlOp::kMem) {
      if ((store_uses_sp && is_sp_addressed(node)) ||
          PreReloadDistinctStackBasesConflict(mem_expr, node, stack_layout) ||
          TrueDependence(mem_expr, node, facts_s2, Phase::kSched2) ||
          AntiDependence(node, mem_expr, facts_s2, Phase::kSched2)) {
        used = true;
        return;
      }
    }
    if (node.op == RtlOp::kSet && node.children.size() == 2 && node.SetDest().op == RtlOp::kMem) {
      if (!node.SetDest().children.empty()) {
        self(self, node.SetDest().children[0]);
      }
      self(self, node.SetSrc());
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch);
    }
  };
  for (int j = start_idx + 1; j < end_idx && !used; ++j) {
    if (excluded_prologue_orig_ids.contains(orig_ids[static_cast<size_t>(j)])) {
      continue;
    }
    walk(walk, seq[static_cast<size_t>(j)].pn.pattern);
  }
  return used;
}

bool TryExtractPlainStackSlotOffset(const RtlExpr& addr, int64_t* off_out) {
  if (addr.op == RtlOp::kReg &&
      (addr.RegNo() == kMipsStackPointerRegno || addr.RegNo() == kMipsFramePointerRegno)) {
    if (off_out != nullptr) {
      *off_out = 0;
    }
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      (addr.children[0].RegNo() == kMipsStackPointerRegno ||
       addr.children[0].RegNo() == kMipsFramePointerRegno) &&
      addr.children[1].op == RtlOp::kConstInt) {
    if (off_out != nullptr) {
      *off_out = addr.children[1].int_val;
    }
    return true;
  }
  return false;
}

}  // namespace detail

namespace {

bool HasNoEquivOrEqualNotes(const Insn& insn) {
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace detail {

bool PatternReadsHardRegSpan(const RtlExpr& pattern, int r, int nr, const MachineModel& mm) {
  bool reads_r = false;
  auto check_reads = [&](auto& self, const RtlExpr& node) -> void {
    if (reads_r) {
      return;
    }
    if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kMipsFirstPseudoRegister) {
      const int cnr = mm.HardRegNRegs(node.RegNo(), node.mode);
      if (node.RegNo() < r + nr && r < node.RegNo() + cnr) {
        reads_r = true;
      }
      return;
    }
    if (node.op == RtlOp::kSet && node.children.size() == 2) {
      if (node.SetDest().op == RtlOp::kMem && !node.SetDest().children.empty()) {
        self(self, node.SetDest().children[0]);
      } else if (node.SetDest().op != RtlOp::kReg) {
        self(self, node.SetDest());
      }
      self(self, node.SetSrc());
      return;
    }
    if (node.op == RtlOp::kClobber) {
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch);
    }
  };
  check_reads(check_reads, pattern);
  return reads_r;
}

}  // namespace detail

namespace {

bool InsnReadsHardRegSpan(const Insn& insn, int r, int nr, const MachineModel& mm) {
  return PatternReadsHardRegSpan(insn.pn.pattern, r, nr, mm);
}

}  // namespace

namespace detail {

bool PatternDefinesHardRegSpan(const RtlExpr& pattern, int r, int nr, const MachineModel& mm) {
  bool defs_r = false;
  auto check_elem = [&](const RtlExpr& elem) {
    if (defs_r || elem.op != RtlOp::kSet || elem.children.size() != 2) {
      return;
    }
    const RtlExpr& d = elem.SetDest();
    if (d.op == RtlOp::kReg && d.RegNo() >= 0 && d.RegNo() < kMipsFirstPseudoRegister) {
      const int dnr = mm.HardRegNRegs(d.RegNo(), d.mode);
      if (d.RegNo() < r + nr && r < d.RegNo() + dnr) {
        defs_r = true;
      }
    }
  };
  if (pattern.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pattern.children) {
      check_elem(ch);
    }
  } else {
    check_elem(pattern);
  }
  return defs_r;
}

}  // namespace detail

namespace {

bool InsnDefinesHardRegSpan(const Insn& insn, int r, int nr, const MachineModel& mm) {
  return PatternDefinesHardRegSpan(insn.pn.pattern, r, nr, mm);
}

struct HardRegLivenessSummary {
  absl::btree_set<int> live_at_entry;
  absl::btree_set<std::pair<int, int>> interferences;
  absl::btree_set<int> call_crossing_regs;
  absl::btree_map<int, bool> mem_equiv_valid;
};

HardRegLivenessSummary ComputeHardRegLivenessSummary(
    absl::Span<const Insn> seq, absl::Span<const int> orig_ids,
    const absl::btree_set<int>& live_at_end, const RegFacts& facts_s2, const MachineModel& mm,
    const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_prologue_orig_ids) {
  HardRegLivenessSummary out;
  const int n = static_cast<int>(seq.size());
  std::vector<absl::btree_set<int>> reads(static_cast<size_t>(n));
  std::vector<absl::btree_set<int>> full_defs(static_cast<size_t>(n));
  std::vector<absl::btree_set<int>> partial_defs(static_cast<size_t>(n));
  std::vector<bool> is_call(static_cast<size_t>(n), false);

  auto insert_span = [&](int r, RtlMachineMode mode, absl::btree_set<int>* s) {
    if (r < 0 || r >= kMipsFirstPseudoRegister) {
      return;
    }
    const int nr = mm.HardRegNRegs(r, mode);
    for (int k = 0; k < nr; ++k) {
      s->insert(r + k);
    }
  };
  auto walk_reads = [&](auto& self, const RtlExpr& node, absl::btree_set<int>* dst) -> void {
    if (node.op == RtlOp::kReg) {
      insert_span(node.RegNo(), node.mode, dst);
      return;
    }
    if (node.op == RtlOp::kSet && node.children.size() == 2) {
      if (node.SetDest().op == RtlOp::kMem && !node.SetDest().children.empty()) {
        self(self, node.SetDest().children[0], dst);
      } else if (node.SetDest().op != RtlOp::kReg) {
        self(self, node.SetDest(), dst);
      }
      self(self, node.SetSrc(), dst);
      return;
    }
    if (node.op == RtlOp::kClobber) {
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch, dst);
    }
  };
  auto walk_defs = [&](const RtlExpr& pat, absl::btree_set<int>* fd, absl::btree_set<int>* pd) {
    auto handle = [&](const RtlExpr& elem) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        const RtlExpr& d = elem.SetDest();
        if (d.op == RtlOp::kReg) {
          insert_span(d.RegNo(), d.mode, fd);
        } else if (d.op != RtlOp::kMem) {
          const RtlExpr* cur = &d;
          while ((cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
                  cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) &&
                 !cur->children.empty()) {
            cur = &cur->children[0];
          }
          if (cur->op == RtlOp::kReg) {
            insert_span(cur->RegNo(), cur->mode, pd);
          }
        }
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty() &&
                 elem.children[0].op == RtlOp::kReg) {
        insert_span(elem.children[0].RegNo(), elem.children[0].mode, fd);
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        handle(ch);
      }
    } else {
      handle(pat);
    }
  };

  for (int i = 0; i < n; ++i) {
    is_call[static_cast<size_t>(i)] = (seq[static_cast<size_t>(i)].kind == Insn::Kind::kCall);
    walk_reads(walk_reads, seq[static_cast<size_t>(i)].pn.pattern, &reads[static_cast<size_t>(i)]);
    for (const RtlExpr& fu : seq[static_cast<size_t>(i)].pn.call_fusage) {
      walk_reads(walk_reads, fu, &reads[static_cast<size_t>(i)]);
    }
    walk_defs(seq[static_cast<size_t>(i)].pn.pattern, &full_defs[static_cast<size_t>(i)],
              &partial_defs[static_cast<size_t>(i)]);
  }

  // Track `full_live` over the entire sequence for `live_at_entry`, and
  // `body_live` excluding post-reload interleaved prologue instructions
  // (`excluded_prologue_orig_ids`, emitted after `reload` by
  // `mips.c:mips_expand_prologue` / `toplev.c:3098-3103`) for `interferences`
  // and `call_crossing_regs` so hoisting a prologue callee-save store back to
  // the prologue never looks like a dropped pre-reload interference.
  absl::btree_set<int> full_live;
  for (int r : live_at_end) {
    if (r >= 0 && r < kMipsFirstPseudoRegister) {
      full_live.insert(r);
    }
  }
  absl::btree_set<int> body_live = full_live;
  for (int i = n - 1; i >= 0; --i) {
    const bool is_excluded_prologue =
        excluded_prologue_orig_ids.contains(orig_ids[static_cast<size_t>(i)]);
    if (!is_excluded_prologue) {
      absl::btree_set<int> all_defs = full_defs[static_cast<size_t>(i)];
      all_defs.insert(partial_defs[static_cast<size_t>(i)].begin(),
                      partial_defs[static_cast<size_t>(i)].end());
      for (int d : all_defs) {
        for (int l : body_live) {
          if (d != l) {
            out.interferences.insert({std::min(d, l), std::max(d, l)});
          }
        }
        for (int d2 : all_defs) {
          if (d < d2) {
            out.interferences.insert({d, d2});
          }
        }
      }
      for (int d : full_defs[static_cast<size_t>(i)]) {
        body_live.erase(d);
      }
      if (is_call[static_cast<size_t>(i)]) {
        for (auto it = body_live.begin(); it != body_live.end();) {
          if (mm.CallUsed(*it)) {
            it = body_live.erase(it);
          } else {
            out.call_crossing_regs.insert(*it);
            ++it;
          }
        }
      }
      body_live.insert(reads[static_cast<size_t>(i)].begin(), reads[static_cast<size_t>(i)].end());
    }
    for (int d : full_defs[static_cast<size_t>(i)]) {
      full_live.erase(d);
    }
    if (is_call[static_cast<size_t>(i)]) {
      for (auto it = full_live.begin(); it != full_live.end();) {
        if (mm.CallUsed(*it)) {
          it = full_live.erase(it);
        } else {
          ++it;
        }
      }
    }
    full_live.insert(reads[static_cast<size_t>(i)].begin(), reads[static_cast<size_t>(i)].end());
  }
  out.live_at_entry = std::move(full_live);

  // Exact `local-alloc.c:540-603, 990-1055` (`validate_equiv_mem`) status for every
  // `MEM` load and store so post-reload reordering never flips `update_equiv_regs`.
  for (int i = 0; i < n; ++i) {
    if (excluded_prologue_orig_ids.contains(orig_ids[static_cast<size_t>(i)])) {
      continue;
    }
    const RtlExpr& pat = seq[static_cast<size_t>(i)].pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
      continue;
    }
    if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kMem &&
        !pat.SetSrc().children.empty()) {
      const int target_reg = pat.SetDest().RegNo();
      // `function.c:3832-3834` (`assign_parms`) attaches `REG_EQUIV` to incoming
      // stack-parameter loads before `local-alloc`; `local-alloc.c:1049-1055`
      // (`if (note == 0 && ... && validate_equiv_mem(...))`) never calls
      // `validate_equiv_mem` when `REG_EQUIV` is already present.
      int64_t arg_off = 0;
      const bool is_incoming_arg_with_equiv =
          stack_layout.valid &&
          TryExtractPlainStackSlotOffset(pat.SetSrc().children[0], &arg_off) &&
          arg_off >= stack_layout.frame_size &&
          std::any_of(seq[static_cast<size_t>(i)].pn.reg_notes.begin(),
                      seq[static_cast<size_t>(i)].pn.reg_notes.end(), [](const RtlRegNote& n) {
                        return n.kind == RtlMachineMode::kRegEquiv && n.datum.op == RtlOp::kMem;
                      });
      if (target_reg >= 0 && target_reg < kMipsFirstPseudoRegister && !is_incoming_arg_with_equiv) {
        out.mem_equiv_valid[orig_ids[static_cast<size_t>(i)]] =
            ValidateEquivMemForPostReloadGate(seq, orig_ids, i, target_reg, pat.SetSrc(), facts_s2,
                                              mm, stack_layout, excluded_prologue_orig_ids);
      }
    } else if (pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
               pat.SetSrc().op == RtlOp::kReg) {
      const int target_reg = pat.SetSrc().RegNo();
      if (target_reg >= 0 && target_reg < kMipsFirstPseudoRegister) {
        int init_idx = -1;
        for (int j = i - 1; j >= 0; --j) {
          if (is_call[static_cast<size_t>(j)]) {
            break;
          }
          if (full_defs[static_cast<size_t>(j)].contains(target_reg)) {
            const RtlExpr& jpat = seq[static_cast<size_t>(j)].pn.pattern;
            // `local-alloc.c:1021` skips `reg_n_sets[regno] != 1`; `flow.c:2506-2515`
            // never attaches `REG_DEAD` on a read-modify-write of the same pseudo,
            // so a self-reading def without `REG_DEAD` proves `reg_n_sets >= 2`.
            const bool is_multiset_rmw =
                reads[static_cast<size_t>(j)].contains(target_reg) &&
                !HasRegDeadNoteCovering(seq[static_cast<size_t>(j)].pn.reg_notes, target_reg, mm);
            if (!is_multiset_rmw && jpat.op == RtlOp::kSet && jpat.children.size() == 2 &&
                jpat.SetDest().op == RtlOp::kReg && jpat.SetDest().RegNo() == target_reg) {
              init_idx = j;
            }
            break;
          }
        }
        bool equiv_ok = false;
        if (init_idx >= 0 &&
            ValidateEquivMemForPostReloadGate(seq, orig_ids, init_idx, target_reg, pat.SetDest(),
                                              facts_s2, mm, stack_layout,
                                              excluded_prologue_orig_ids) &&
            !MemrefUsedBetweenForPostReloadGate(seq, orig_ids, init_idx, i, pat.SetDest(), facts_s2,
                                                stack_layout, excluded_prologue_orig_ids)) {
          equiv_ok = true;
        }
        out.mem_equiv_valid[orig_ids[static_cast<size_t>(i)]] = equiv_ok;
      }
    }
  }
  return out;
}

void CollectHardRegDefBaseRegnos(const RtlExpr& pat, absl::btree_map<int, int>* counts,
                                 absl::btree_map<int, RtlMachineMode>* last_def_mode = nullptr) {
  auto record = [&](const RtlExpr& elem) {
    if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
      const RtlExpr* dst = &elem.children[0];
      while ((dst->op == RtlOp::kSubreg || dst->op == RtlOp::kStrictLowPart ||
              dst->op == RtlOp::kZeroExtract || dst->op == RtlOp::kSignExtract) &&
             !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (dst->op == RtlOp::kReg && dst->RegNo() >= 0 && dst->RegNo() < kMipsFirstPseudoRegister) {
        (*counts)[dst->RegNo()] += 1;
        if (last_def_mode != nullptr && elem.op == RtlOp::kSet &&
            dst->mode != RtlMachineMode::kVoid) {
          (*last_def_mode)[dst->RegNo()] = dst->mode;
        }
      }
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      record(ch);
    }
  } else {
    record(pat);
  }
}

// Matches leading `< 4`-byte by-value struct/union argument right-shifts
// emitted by `mips_expand_prologue` (`config/mips/mips.c:4976-4992`) before
// any `$sp` adjustment:
//   `(set (reg:SI r) (ashiftrt:SI (reg:SI r) (const_int 8|16|24)))`
// for strictly increasing `r in [4, 7]` with only flow-liveness notes
// (matching `StripPrologueAndEpilogue`'s `HasOnlyFlowLivenessNotes` guard).
bool MatchPrologueStructArgShiftPrefixInsn(const Insn& insn, int* out_regno) {
  if (insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  for (const RtlRegNote& n : insn.pn.reg_notes) {
    if (n.kind != RtlMachineMode::kRegDead && n.kind != RtlMachineMode::kRegUnused) {
      return false;
    }
  }
  const RtlExpr& pat = insn.pn.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (dst.op != RtlOp::kReg || dst.mode != RtlMachineMode::kSi || dst.flags.volatil ||
      dst.flags.in_struct || dst.flags.integrated) {
    return false;
  }
  const int r = dst.RegNo();
  if (r < 4 || r > 7) {
    return false;
  }
  if (src.op != RtlOp::kAshiftrt || src.mode != RtlMachineMode::kSi || src.children.size() != 2 ||
      src.children[0].op != RtlOp::kReg || src.children[0].mode != RtlMachineMode::kSi ||
      src.children[0].RegNo() != r || src.children[0].flags.volatil ||
      src.children[0].flags.in_struct || src.children[0].flags.integrated ||
      src.children[1].op != RtlOp::kConstInt) {
    return false;
  }
  const int64_t sh = src.children[1].int_val;
  if (sh != 8 && sh != 16 && sh != 24) {
    return false;
  }
  *out_regno = r;
  return true;
}

// Matches a pre-`$sp`-decrement variadic GP argument home store emitted by
// `mips_expand_prologue` (`config/mips/mips.c:4996-5020`):
//   `(set (mem:<SI|DI> <$sp or $sp + (r - 4) * word_bytes>) (reg:<SI|DI> r))`
// for `r in [4, 7]` carrying only flow-liveness notes.
bool MatchPrologueVariadicArgStorePrefixInsn(const Insn& insn, int* out_regno) {
  if (insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  for (const RtlRegNote& n : insn.pn.reg_notes) {
    if (n.kind != RtlMachineMode::kRegDead && n.kind != RtlMachineMode::kRegUnused) {
      return false;
    }
  }
  const RtlExpr& pat = insn.pn.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (dst.op != RtlOp::kMem || dst.children.size() != 1 || dst.flags.volatil ||
      dst.flags.in_struct || src.op != RtlOp::kReg || src.flags.volatil || src.flags.in_struct ||
      dst.mode != src.mode ||
      (dst.mode != RtlMachineMode::kSi && dst.mode != RtlMachineMode::kDi)) {
    return false;
  }
  const int r = src.RegNo();
  if (r < kMipsArgRegFirst || r > kMipsArgRegLast) {
    return false;
  }
  const int word_bytes = (dst.mode == RtlMachineMode::kDi) ? 8 : 4;
  const int64_t expected_offset = static_cast<int64_t>(r - kMipsArgRegFirst) * word_bytes;
  const RtlExpr& addr = dst.children[0];
  if (expected_offset == 0) {
    if (addr.op != RtlOp::kReg || addr.RegNo() != kMipsStackPointerRegno) {
      return false;
    }
  } else {
    if (addr.op != RtlOp::kPlus || addr.children.size() != 2 ||
        addr.children[0].op != RtlOp::kReg || addr.children[0].RegNo() != kMipsStackPointerRegno ||
        addr.children[1].op != RtlOp::kConstInt || addr.children[1].int_val != expected_offset) {
      return false;
    }
  }
  *out_regno = r;
  return true;
}

}  // namespace

PostReloadBodyBounds FindPostReloadBodyBounds(absl::Span<const Insn> sched_block,
                                              size_t block_idx) {
  const int n = static_cast<int>(sched_block.size());
  int prologue_end = 0;
  if (block_idx == 0) {
    int last_shift_reg = 3;
    while (prologue_end < n) {
      int shift_reg = -1;
      if (MatchPrologueStructArgShiftPrefixInsn(sched_block[static_cast<size_t>(prologue_end)],
                                                &shift_reg) &&
          shift_reg > last_shift_reg) {
        last_shift_reg = shift_reg;
        ++prologue_end;
        continue;
      }
      break;
    }
    // Skip contiguous leading variadic GP argument home stores emitted by
    // `mips_expand_prologue` (`config/mips/mips.c:4996-5020`) prior to `$sp`
    // decrement: they form a contiguous suffix `[min_reg .. $a3]` and precede a
    // prologue `$sp` decrement before any call, jump, or other `$sp` write.
    {
      int scan = prologue_end;
      absl::btree_set<int> vararg_regs;
      while (scan < n) {
        int vararg_reg = -1;
        if (!MatchPrologueVariadicArgStorePrefixInsn(sched_block[static_cast<size_t>(scan)],
                                                     &vararg_reg) ||
            !vararg_regs.insert(vararg_reg).second) {
          break;
        }
        ++scan;
      }
      if (!vararg_regs.empty()) {
        const int min_reg = *vararg_regs.begin();
        const bool is_contiguous_suffix =
            *vararg_regs.rbegin() == kMipsArgRegLast &&
            static_cast<int>(vararg_regs.size()) == (kMipsArgRegLast - min_reg + 1);
        bool has_sp_dec_ahead = false;
        if (is_contiguous_suffix) {
          for (int k = scan; k < n; ++k) {
            const Insn& ki = sched_block[static_cast<size_t>(k)];
            if (ki.kind == Insn::Kind::kCall || ki.kind == Insn::Kind::kJump) {
              break;
            }
            if (IsPrologueSpDecrementPattern(ki.pn.pattern)) {
              has_sp_dec_ahead = true;
              break;
            }
            if (ki.pn.pattern.op == RtlOp::kSet && ki.pn.pattern.children.size() == 2 &&
                ki.pn.pattern.SetDest().op == RtlOp::kReg &&
                ki.pn.pattern.SetDest().RegNo() == kMipsStackPointerRegno) {
              break;
            }
          }
        }
        if (has_sp_dec_ahead) {
          prologue_end = scan;
        }
      }
    }
    while (prologue_end < n &&
           IsProloguePrefixPattern(sched_block[static_cast<size_t>(prologue_end)].pn.pattern)) {
      ++prologue_end;
    }
  }
  int epilogue_begin = n;
  int scan = n;
  while (scan > prologue_end) {
    const RtlExpr& pat = sched_block[static_cast<size_t>(scan - 1)].pn.pattern;
    if (pat.op == RtlOp::kUse || pat.op == RtlOp::kReturn || IsReturnInternalPattern(pat)) {
      --scan;
    } else {
      break;
    }
  }
  int ep = scan;
  while (ep > prologue_end &&
         IsEpilogueSuffixPattern(sched_block[static_cast<size_t>(ep - 1)].pn.pattern)) {
    --ep;
  }
  if (ep < scan) {
    epilogue_begin = ep;
  } else {
    // Even in frameless leaf functions with no stack-pop epilogue, trailing
    // `(return)` / `return_internal` insns are emitted by
    // `thread_prologue_and_epilogue_insns` (`toplev.c:3103`, `function.c:5516`,
    // `mips.c:mips_expand_epilogue`) after `reload` and do not exist in `sched1`.
    // Trailing `(use (reg/i ...))` markers DO exist in `sched1` and remain inside
    // `[prologue_end, epilogue_begin)`.
    while (epilogue_begin > prologue_end) {
      const RtlExpr& pat = sched_block[static_cast<size_t>(epilogue_begin - 1)].pn.pattern;
      if (pat.op == RtlOp::kReturn || IsReturnInternalPattern(pat)) {
        --epilogue_begin;
      } else {
        break;
      }
    }
  }
  return PostReloadBodyBounds{.prologue_end = prologue_end, .epilogue_begin = epilogue_begin};
}

absl::btree_set<int> FindInterleavedPrologueSaveBodyIndices(absl::Span<const Insn> sched_block,
                                                            const PostReloadBodyBounds& bounds,
                                                            size_t block_idx,
                                                            const MachineModel& mm) {
  absl::btree_set<int> excluded;
  if (block_idx != 0 || bounds.epilogue_begin <= bounds.prologue_end) {
    return excluded;
  }
  auto match_callee_save_store = [](const RtlExpr& pat, int* out_reg,
                                    RtlMachineMode* out_mode) -> bool {
    if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
      return false;
    }
    const RtlExpr& dst = pat.SetDest();
    const RtlExpr& src = pat.SetSrc();
    if (dst.op != RtlOp::kMem || dst.children.empty() || dst.flags.volatil ||
        src.op != RtlOp::kReg) {
      return false;
    }
    const int sr = src.RegNo();
    const bool is_saved = (sr >= kMipsCalleeSavedGprFirst && sr <= kMipsCalleeSavedGprLast) ||
                          sr == kMipsFramePointerRegno || sr == kMipsReturnAddrRegno ||
                          (sr >= kMipsCalleeSavedFprFirst && sr <= kMipsFpRegLast);
    if (!is_saved) {
      return false;
    }
    const RtlExpr& addr = dst.children[0];
    const bool sp_based =
        (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
        (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
         addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == kMipsStackPointerRegno &&
         addr.children[1].op == RtlOp::kConstInt);
    if (!sp_based) {
      return false;
    }
    *out_reg = sr;
    *out_mode = src.mode;
    return true;
  };

  bool saw_sp_dec = false;
  absl::btree_set<int> saved_regs;
  for (int i = 0; i < bounds.prologue_end; ++i) {
    const RtlExpr& pat = sched_block[static_cast<size_t>(i)].pn.pattern;
    if (IsPrologueSpDecrementPattern(pat)) {
      saw_sp_dec = true;
    }
    if (IsPrologueFpSetupPattern(pat)) {
      saved_regs.insert(kMipsFramePointerRegno);
    }
    int sr = -1;
    RtlMachineMode smode = RtlMachineMode::kVoid;
    if (match_callee_save_store(pat, &sr, &smode)) {
      const int nr = mm.HardRegNRegs(sr, smode);
      for (int k = 0; k < nr; ++k) {
        saved_regs.insert(sr + k);
      }
    }
  }
  auto collect_written_hard_regs = [&](const RtlExpr& pat, absl::btree_set<int>* out_defs) {
    auto handle = [&](const RtlExpr& elem) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
        const RtlExpr* cur = &elem.children[0];
        if (cur->op == RtlOp::kMem) {
          return;
        }
        while ((cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
                cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) &&
               !cur->children.empty()) {
          cur = &cur->children[0];
        }
        if (cur->op == RtlOp::kReg && cur->RegNo() >= 0 &&
            cur->RegNo() < kMipsFirstPseudoRegister) {
          const int nr = mm.HardRegNRegs(cur->RegNo(), cur->mode);
          for (int k = 0; k < nr; ++k) {
            out_defs->insert(cur->RegNo() + k);
          }
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        handle(ch);
      }
    } else {
      handle(pat);
    }
  };

  absl::btree_set<int> body_writes;
  bool body_touched_sp = false;
  for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
    const Insn& insn = sched_block[static_cast<size_t>(i)];
    if (insn.kind == Insn::Kind::kCall || insn.kind == Insn::Kind::kJump) {
      break;
    }
    const RtlExpr& pat = insn.pn.pattern;
    if (!saw_sp_dec) {
      if (insn.kind == Insn::Kind::kInsn && HasNoEquivOrEqualNotes(insn) &&
          IsPrologueSpDecrementPattern(pat) && !body_touched_sp &&
          !body_writes.contains(kMipsStackPointerRegno)) {
        saw_sp_dec = true;
        excluded.insert(i - bounds.prologue_end);
        continue;
      }
      if (pat.ContainsReg(kMipsStackPointerRegno)) {
        body_touched_sp = true;
      }
      collect_written_hard_regs(pat, &body_writes);
      continue;
    }
    if (insn.kind == Insn::Kind::kInsn && HasNoEquivOrEqualNotes(insn)) {
      if (IsPrologueFpSetupPattern(pat) && !body_writes.contains(kMipsFramePointerRegno) &&
          !saved_regs.contains(kMipsFramePointerRegno)) {
        excluded.insert(i - bounds.prologue_end);
        saved_regs.insert(kMipsFramePointerRegno);
        continue;
      }
      int sr = -1;
      RtlMachineMode smode = RtlMachineMode::kVoid;
      if (match_callee_save_store(pat, &sr, &smode)) {
        const int nr = mm.HardRegNRegs(sr, smode);
        bool conflict = false;
        for (int k = 0; k < nr; ++k) {
          if (saved_regs.contains(sr + k) || body_writes.contains(sr + k)) {
            conflict = true;
            break;
          }
        }
        if (!conflict) {
          excluded.insert(i - bounds.prologue_end);
          for (int k = 0; k < nr; ++k) {
            saved_regs.insert(sr + k);
          }
          continue;
        }
      }
    }
    absl::btree_set<int> defs;
    collect_written_hard_regs(pat, &defs);
    if (defs.contains(kMipsStackPointerRegno)) {
      break;
    }
    body_writes.insert(defs.begin(), defs.end());
  }
  return excluded;
}

bool IsAdjacentReloadContiguityPair(const Insn& first_insn, const Insn& second_insn,
                                    const MachineModel& mm,
                                    const PostReloadStackLayout& stack_layout) {
  auto is_plausible_spill_slot_offset = [&](int64_t off, bool need_store, bool need_load) {
    // `caller-save.c:455-462` emits caller-save stores before `CALL_INSN`
    // (not adjacent to the defining insn), whereas `caller-save.c:410-411,
    // 568-615` (`restore_referenced_regs`) emits caller-save restores
    // immediately before the first post-call consumer.
    if (need_load && stack_layout.caller_save_slot_offsets.contains(off)) {
      return false;
    }
    if (stack_layout.valid && stack_layout.frame_size > 0) {
      const int64_t min_spill =
          std::max(stack_layout.starting_frame_offset, stack_layout.min_valid_reload_spill_offset);
      if (off < min_spill || off >= stack_layout.frame_size) {
        return false;
      }
    }
    if (stack_layout.has_stack_slot_access_info) {
      if (need_store && !stack_layout.stack_slot_store_offsets.contains(off)) {
        return false;
      }
      if (need_load && !stack_layout.stack_slot_load_offsets.contains(off)) {
        return false;
      }
    }
    return true;
  };
  // (a) Stack-slot input reload or (b) cross-bank GPR->FPR (`mtc1`) input reload.
  if (first_insn.kind == Insn::Kind::kInsn && second_insn.kind != Insn::Kind::kCall &&
      HasNoEquivOrEqualNotes(first_insn)) {
    const RtlExpr& pat = first_insn.pn.pattern;
    if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg &&
        !pat.SetDest().flags.integrated) {
      const int r = pat.SetDest().RegNo();
      const int nr = mm.HardRegNRegs(r, pat.SetDest().mode);
      if (r > 0 && r < kMipsFirstPseudoRegister && r != kMipsStackPointerRegno &&
          r != kMipsFramePointerRegno) {
        const RtlExpr& src = pat.SetSrc();
        int64_t load_off = 0;
        const bool is_stack_load =
            (src.op == RtlOp::kMem && !src.children.empty() && !src.flags.volatil &&
             !src.flags.in_struct && !src.flags.unchanging &&
             TryExtractPlainStackSlotOffset(src.children[0], &load_off) &&
             is_plausible_spill_slot_offset(load_off, /*need_store=*/true, /*need_load=*/false));
        bool is_gpr_to_fpr_reload =
            (r >= kMipsFpRegFirst && r <= kMipsFpRegLast && src.op == RtlOp::kReg &&
             src.RegNo() >= 2 && src.RegNo() < kMipsFpRegFirst &&
             (pat.SetDest().mode == RtlMachineMode::kSf ||
              pat.SetDest().mode == RtlMachineMode::kDf));
        if (is_gpr_to_fpr_reload) {
          // `reload1.c:5849-5871, 6118-6125` never attaches `REG_DEAD gpr` onto
          // an emitted `mtc1` input reload (whereas `function.c:assign_parms`
          // entry-parameter copies carry `REG_DEAD gpr` from `flow.c`).
          const int gpr = src.RegNo();
          const int gnr = std::max(1, mm.HardRegNRegs(gpr, src.mode));
          for (const RtlRegNote& n : first_insn.pn.reg_notes) {
            if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                n.datum.RegNo() >= gpr && n.datum.RegNo() < gpr + gnr) {
              is_gpr_to_fpr_reload = false;
              break;
            }
          }
        }
        if ((is_stack_load || is_gpr_to_fpr_reload) &&
            InsnReadsHardRegSpan(second_insn, r, nr, mm)) {
          return true;
        }
      }
    }
  }
  // (c) Stack-slot output spill immediately after its defining insn.
  if (first_insn.kind == Insn::Kind::kInsn && second_insn.kind == Insn::Kind::kInsn &&
      HasNoEquivOrEqualNotes(second_insn)) {
    const RtlExpr& pat2 = second_insn.pn.pattern;
    int64_t store_off = 0;
    if (pat2.op == RtlOp::kSet && pat2.children.size() == 2 && pat2.SetDest().op == RtlOp::kMem &&
        !pat2.SetDest().children.empty() && !pat2.SetDest().flags.volatil &&
        !pat2.SetDest().flags.in_struct && !pat2.SetDest().flags.unchanging &&
        pat2.SetSrc().op == RtlOp::kReg &&
        TryExtractPlainStackSlotOffset(pat2.SetDest().children[0], &store_off) &&
        is_plausible_spill_slot_offset(store_off, /*need_store=*/false, /*need_load=*/true)) {
      const int r = pat2.SetSrc().RegNo();
      if (r > 0 && r < kMipsFirstPseudoRegister && r != kMipsStackPointerRegno &&
          r != kMipsFramePointerRegno) {
        const int nr = mm.HardRegNRegs(r, pat2.SetSrc().mode);
        if (InsnDefinesHardRegSpan(first_insn, r, nr, mm)) {
          return true;
        }
      }
    }
  }
  // (d) Cross-bank `"=f"` FPR->GPR (`mfc1`) output reload immediately after its FP op.
  return TryMatchAdjacentFprOutputReload(first_insn, second_insn, /*gpr_dest_out=*/nullptr);
}

namespace {

struct BlockWebEndpointSummary {
  absl::btree_map<int, RtlMachineMode> gen_def_mode;
  absl::btree_set<int> kill_regs;
  absl::btree_set<int> upward_use_regs;
  absl::btree_set<int> upward_call_fusage_use_regs;
  absl::btree_set<int> upward_return_use_regs;
  absl::btree_set<int> upward_use_rmw_regs;
  absl::btree_set<int> live_in_regs;
  // Exit-reaching defs of an ABI call-arg register not yet counted as
  // intra-block unrenamed hard-reg defs; promoted to `block_unrenamed_hard_defs`
  // only when the def actually reaches a downstream `CALL_INSN_FUNCTION_USAGE`.
  absl::btree_set<int> uncounted_exit_def_regs;
};

// Computes exact CFG reaching-definition webs (`flow.c:reg_n_sets`,
// `sched.c:2469-2501` `birthing_insn_p`) per hard register so disjoint webs on
// the same hard register in unrelated blocks never inflate `reg_n_sets`.
void ComputeExactCfgWebDefCounts(const RtlControlFlowGraph& cfg,
                                 const std::vector<BlockWebEndpointSummary>& blk_info,
                                 CfgSched1Context* out) {
  const size_t num_blocks = cfg.rpo_blocks.size();
  out->block_entry_web_other_defs.assign(num_blocks, {});
  out->block_exit_web_other_defs.assign(num_blocks, {});
  out->block_entry_exit_same_web_regs.assign(num_blocks, {});
  if (num_blocks == 0) {
    return;
  }

  std::vector<std::vector<size_t>> preds(num_blocks);
  for (size_t b = 0; b < num_blocks; ++b) {
    for (int succ : cfg.rpo_blocks[b].successors) {
      if (succ >= 0 && static_cast<size_t>(succ) < num_blocks) {
        preds[static_cast<size_t>(succ)].push_back(b);
      }
    }
  }

  absl::btree_set<int> active_hard_regs;
  for (size_t b = 0; b < num_blocks; ++b) {
    for (const auto& [r, _] : blk_info[b].gen_def_mode) {
      active_hard_regs.insert(r);
    }
  }

  auto modes_compatible = [](RtlMachineMode a, RtlMachineMode b) {
    return a == RtlMachineMode::kVoid || b == RtlMachineMode::kVoid || a == b;
  };

  for (int r : active_hard_regs) {
    std::vector<absl::btree_set<size_t>> reach_in(num_blocks);
    std::vector<absl::btree_set<size_t>> reach_out(num_blocks);
    bool changed = true;
    for (size_t iter = 0; iter <= num_blocks + 2 && changed; ++iter) {
      changed = false;
      for (size_t b = 0; b < num_blocks; ++b) {
        absl::btree_set<size_t> new_in;
        for (size_t p : preds[b]) {
          new_in.insert(reach_out[p].begin(), reach_out[p].end());
        }
        absl::btree_set<size_t> new_out;
        if (blk_info[b].gen_def_mode.contains(r)) {
          new_out.insert(b);
        } else if (!blk_info[b].kill_regs.contains(r)) {
          new_out = new_in;
        }
        if (new_in != reach_in[b] || new_out != reach_out[b]) {
          reach_in[b] = std::move(new_in);
          reach_out[b] = std::move(new_out);
          changed = true;
        }
      }
    }

    // Union defining blocks that jointly reach a common upward-exposed use,
    // live-in join point, or non-dying RMW in the same machine mode (`flow.c`).
    std::vector<size_t> parent(num_blocks);
    for (size_t b = 0; b < num_blocks; ++b) {
      parent[b] = b;
    }
    auto find_root = [&](size_t x) {
      while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
      }
      return x;
    };
    auto unite = [&](size_t a, size_t b) {
      const size_t ra = find_root(a);
      const size_t rb = find_root(b);
      if (ra != rb) {
        if (ra < rb) {
          parent[rb] = ra;
        } else {
          parent[ra] = rb;
        }
      }
    };

    for (size_t u = 0; u < num_blocks; ++u) {
      if (reach_in[u].empty()) {
        continue;
      }
      const bool needs_join = blk_info[u].upward_use_regs.contains(r) ||
                              blk_info[u].live_in_regs.contains(r) ||
                              blk_info[u].upward_use_rmw_regs.contains(r);
      if (!needs_join) {
        continue;
      }
      std::vector<size_t> defs(reach_in[u].begin(), reach_in[u].end());
      for (size_t i = 0; i < defs.size(); ++i) {
        const RtlMachineMode mi = blk_info[defs[i]].gen_def_mode.at(r);
        for (size_t j = i + 1; j < defs.size(); ++j) {
          const RtlMachineMode mj = blk_info[defs[j]].gen_def_mode.at(r);
          if (modes_compatible(mi, mj)) {
            unite(defs[i], defs[j]);
          }
        }
        if (blk_info[u].upward_use_rmw_regs.contains(r) && blk_info[u].gen_def_mode.contains(r) &&
            modes_compatible(mi, blk_info[u].gen_def_mode.at(r))) {
          unite(defs[i], u);
        }
      }
    }

    absl::btree_map<size_t, absl::btree_set<size_t>> web_members;
    for (size_t b = 0; b < num_blocks; ++b) {
      if (blk_info[b].gen_def_mode.contains(r)) {
        web_members[find_root(b)].insert(b);
      }
    }

    absl::btree_set<size_t> call_fusage_reaching_defs;
    absl::btree_set<size_t> return_use_reaching_defs;
    if (IsAbiCallInterfaceHardReg(r)) {
      for (size_t u = 0; u < num_blocks; ++u) {
        if (blk_info[u].upward_call_fusage_use_regs.contains(r)) {
          call_fusage_reaching_defs.insert(reach_in[u].begin(), reach_in[u].end());
        }
        if (blk_info[u].upward_return_use_regs.contains(r)) {
          return_use_reaching_defs.insert(reach_in[u].begin(), reach_in[u].end());
        }
      }
      // `calls.c:emit_call_1` + `flow.c:2065`: count an exit-reaching def of
      // `r` as an unrenamed hard-reg def only when it actually reaches a
      // downstream `CALL_INSN_FUNCTION_USAGE` or `(use (reg/i ...))` return
      // read (not when killed earlier by an intervening block's def, where the
      // upstream def was a pseudo colored onto `$v0`/`$f0`).
      for (size_t b : call_fusage_reaching_defs) {
        if (blk_info[b].uncounted_exit_def_regs.contains(r)) {
          out->block_unrenamed_hard_defs[b][r] += 1;
          out->cfg_unrenamed_hard_defs[r] += 1;
        }
      }
      for (size_t b : return_use_reaching_defs) {
        if (!call_fusage_reaching_defs.contains(b) &&
            blk_info[b].uncounted_exit_def_regs.contains(r)) {
          out->block_unrenamed_hard_defs[b][r] += 1;
          out->cfg_unrenamed_hard_defs[r] += 1;
        }
      }
    }

    for (size_t b = 0; b < num_blocks; ++b) {
      if (const auto git = blk_info[b].gen_def_mode.find(r);
          git != blk_info[b].gen_def_mode.end()) {
        const auto& members = web_members.at(find_root(b));
        int other_cnt = static_cast<int>(members.size()) - (members.contains(b) ? 1 : 0);
        if (call_fusage_reaching_defs.contains(b)) {
          // `calls.c:emit_call_1` + `flow.c:2065`: an ABI call-arg hard reg
          // feeding a downstream `CALL_INSN_FUNCTION_USAGE` remains an unrenamed
          // hard register in `sched1`, so `reg_n_sets[r]` counts every explicit
          // pre-reload unrenamed hard-reg `SET` of `r` across other blocks as
          // well as any other defs joined into the same CFG reaching web.
          int own = 0;
          if (const auto bit = out->block_unrenamed_hard_defs[b].find(r);
              bit != out->block_unrenamed_hard_defs[b].end()) {
            own = bit->second;
          }
          const auto tit = out->cfg_unrenamed_hard_defs.find(r);
          const int total = (tit != out->cfg_unrenamed_hard_defs.end()) ? tit->second : 0;
          other_cnt = std::max(other_cnt, std::max(0, total - own));
        }
        if (other_cnt > 0) {
          out->block_exit_web_other_defs[b][{r, git->second}] = other_cnt;
        }
      }
      if (!reach_in[b].empty() &&
          (blk_info[b].upward_use_regs.contains(r) || blk_info[b].live_in_regs.contains(r))) {
        absl::btree_map<RtlMachineMode, absl::btree_set<size_t>> by_mode;
        const bool b_has_gen_def = blk_info[b].gen_def_mode.contains(r);
        const size_t b_root = b_has_gen_def ? find_root(b) : num_blocks;
        bool entry_exit_connected = false;
        for (size_t d : reach_in[b]) {
          const RtlMachineMode md = blk_info[d].gen_def_mode.at(r);
          const size_t d_root = find_root(d);
          if (b_has_gen_def && d_root == b_root) {
            entry_exit_connected = true;
          }
          for (size_t m_blk : web_members.at(d_root)) {
            if (m_blk != b) {
              by_mode[md].insert(m_blk);
            }
          }
        }
        if (entry_exit_connected) {
          out->block_entry_exit_same_web_regs[b].insert(r);
        }
        for (const auto& [md, defs_set] : by_mode) {
          if (!defs_set.empty()) {
            out->block_entry_web_other_defs[b][{r, md}] = static_cast<int>(defs_set.size());
          }
        }
      }
    }
  }
}

// Returns true when `root` references `$sp` (or `$fp` when used as the frame
// pointer) outside any `(mem ...)` address expression (e.g. taking the address
// of a local stack object `(set r (plus sp off))` or adjusting `$sp`).
bool PatternHasStackPointerEscape(const RtlExpr& root, bool frame_pointer_needed) {
  bool escaped = false;
  auto walk = [&](auto& self, const RtlExpr& node, bool inside_mem_addr) -> void {
    if (escaped) {
      return;
    }
    if (node.op == RtlOp::kUse || node.op == RtlOp::kClobber) {
      return;
    }
    if (node.op == RtlOp::kMem) {
      for (const RtlExpr& ch : node.children) {
        self(self, ch, /*inside_mem_addr=*/true);
      }
      return;
    }
    if (node.op == RtlOp::kReg) {
      if (!inside_mem_addr && !node.IsArgPointerReg()) {
        const int r = node.RegNo();
        if (r == kMipsStackPointerRegno || (frame_pointer_needed && r == kMipsFramePointerRegno)) {
          escaped = true;
        }
      }
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch, inside_mem_addr);
    }
  };
  walk(walk, root, /*inside_mem_addr=*/false);
  return escaped;
}

}  // namespace

CfgSched1Context ComputeCfgSched1Context(const RtlControlFlowGraph& cfg,
                                         const RtlControlFlowGraph& live_cfg,
                                         const MachineModel& mm,
                                         absl::Span<const int> extra_b0_defs) {
  const size_t num_blocks = cfg.rpo_blocks.size();
  CfgSched1Context out;
  out.block_total_defs.resize(num_blocks);
  out.block_epilogue_use_regs.resize(num_blocks);
  out.block_unrenamed_hard_defs.resize(num_blocks);
  out.block_reaching_defs.resize(num_blocks);
  out.block_reaching_defs_by_mode.resize(num_blocks);
  out.block_spill_store_counts.resize(num_blocks);
  out.block_spill_load_offsets.resize(num_blocks);
  std::vector<BlockWebEndpointSummary> web_blk_info(num_blocks);
  out.stack_layout.valid = true;
  out.stack_layout.has_stack_slot_access_info = true;
  bool any_call_in_cfg = false;
  int64_t max_call_arg_bytes = 0;
  std::vector<Insn> all_body_insns_for_spill_bounds;
  absl::btree_set<int> cfg_return_use_regs;
  absl::btree_set<int> cfg_call_fusage_regs;
  for (size_t b = 0; b < num_blocks; ++b) {
    const ParsedSchedRegion pre_reg = ParseBlockForSchedModel(cfg.rpo_blocks[b], b, Phase::kSched2);
    if (!pre_reg.valid) {
      continue;
    }
    for (const Insn& insn : pre_reg.sched_block) {
      if (insn.pn.pattern.op == RtlOp::kUse && !insn.pn.pattern.children.empty() &&
          insn.pn.pattern.children[0].op == RtlOp::kReg &&
          insn.pn.pattern.children[0].flags.integrated) {
        const int r = insn.pn.pattern.children[0].RegNo();
        const int nr = std::max(1, mm.HardRegNRegs(r, insn.pn.pattern.children[0].mode));
        for (int k = 0; k < nr && r + k < kMipsFirstPseudoRegister; ++k) {
          cfg_return_use_regs.insert(r + k);
        }
      }
      if (insn.kind == Insn::Kind::kCall) {
        absl::btree_set<int> fu_reads;
        for (const RtlExpr& fu : insn.pn.call_fusage) {
          CollectRegReads(fu, &fu_reads);
        }
        for (int r : fu_reads) {
          if (r >= 0 && r < kMipsFirstPseudoRegister) {
            cfg_call_fusage_regs.insert(r);
          }
        }
      }
    }
  }
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = cfg.rpo_blocks[b];
    const ParsedSchedRegion region = ParseBlockForSchedModel(blk, b, Phase::kSched2);
    if (!region.valid) {
      continue;
    }
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, b);
    const absl::btree_set<int> excluded_prologue =
        FindInterleavedPrologueSaveBodyIndices(region.sched_block, bounds, b, mm);
    for (size_t i = 0; i < region.sched_block.size(); ++i) {
      const Insn& insn = region.sched_block[i];
      const RtlExpr& pat = insn.pn.pattern;
      if (b == 0) {
        if (IsPrologueSpDecrementPattern(pat) && out.stack_layout.frame_size == 0 &&
            pat.SetSrc().children[1].op == RtlOp::kConstInt) {
          out.stack_layout.frame_size = (pat.SetSrc().op == RtlOp::kMinus)
                                            ? pat.SetSrc().children[1].int_val
                                            : -pat.SetSrc().children[1].int_val;
        }
        if (IsPrologueFpSetupPattern(pat)) {
          out.stack_layout.frame_pointer_needed = true;
        }
        const int ii = static_cast<int>(i);
        const bool is_prologue_region =
            (ii < bounds.prologue_end) ||
            (ii >= bounds.prologue_end && excluded_prologue.contains(ii - bounds.prologue_end));
        if (is_prologue_region && pat.op == RtlOp::kSet && pat.children.size() == 2 &&
            pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
            pat.SetSrc().op == RtlOp::kReg) {
          const int sr = pat.SetSrc().RegNo();
          const bool is_cs_save_reg =
              (sr == 31 || sr == 30 ||
               (sr >= 0 && sr < kMipsFirstPseudoRegister && !mm.CallUsed(sr)));
          int64_t save_off = 0;
          if (is_cs_save_reg &&
              TryExtractPlainStackSlotOffset(pat.SetDest().children[0], &save_off) &&
              save_off >= 0) {
            if (out.stack_layout.callee_saved_area_offset < 0 ||
                save_off < out.stack_layout.callee_saved_area_offset) {
              out.stack_layout.callee_saved_area_offset = save_off;
            }
          }
        }
      }
      if (insn.kind == Insn::Kind::kCall) {
        any_call_in_cfg = true;
        auto scan_call = [&](const RtlExpr& e) {
          if (e.op == RtlOp::kCall && e.children.size() >= 2 &&
              e.children[1].op == RtlOp::kConstInt) {
            max_call_arg_bytes = std::max(max_call_arg_bytes, e.children[1].int_val);
          } else if (e.op == RtlOp::kSet && e.children.size() == 2 &&
                     e.SetSrc().op == RtlOp::kCall && e.SetSrc().children.size() >= 2 &&
                     e.SetSrc().children[1].op == RtlOp::kConstInt) {
            max_call_arg_bytes = std::max(max_call_arg_bytes, e.SetSrc().children[1].int_val);
          }
        };
        if (pat.op == RtlOp::kParallel) {
          for (const RtlExpr& ch : pat.children) {
            scan_call(ch);
          }
        } else {
          scan_call(pat);
        }
      }
    }

    absl::btree_set<int> live_at_end;
    for (int succ : blk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end.insert(it->second.begin(), it->second.end());
      }
    }
    // Identify matched intra-block `caller-save.c:480-580` (`insert_save` /
    // `insert_restore`) restore loads `(set r (mem sp+off))` paired with a
    // pre-call store `(set (mem sp+off) r)` in block `b`. These post-reload
    // caller-save restores do not exist in `flow.c` / `sched1` (`sched.c:2490`)
    // and must not inflate `reg_n_sets` for loop-invariant pseudos live into `b`.
    absl::btree_set<int> caller_save_restore_indices;
    {
      absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>> blk_stores;
      std::vector<int> blk_calls;
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
        if (excluded_prologue.contains(i - bounds.prologue_end)) {
          continue;
        }
        const Insn& body_insn = region.sched_block[static_cast<size_t>(i)];
        if (body_insn.kind == Insn::Kind::kCall) {
          blk_calls.push_back(i);
          continue;
        }
        if (auto acc = TryParsePlainStackSlotMove(body_insn); acc.has_value()) {
          if (acc->is_store) {
            blk_stores[acc->key].push_back({i, acc->regno});
          } else if (const auto sit = blk_stores.find(acc->key); sit != blk_stores.end()) {
            for (const auto& [s_idx, s_reg] : sit->second) {
              if (s_idx < i && s_reg == acc->regno) {
                for (int c_idx : blk_calls) {
                  if (s_idx < c_idx && c_idx < i) {
                    caller_save_restore_indices.insert(i);
                    break;
                  }
                }
                if (caller_save_restore_indices.contains(i)) {
                  break;
                }
              }
            }
          }
        }
      }
    }

    absl::btree_map<int, RtlMachineMode> last_def_mode;
    BlockWebEndpointSummary& winfo = web_blk_info[b];
    if (const auto lit = live_cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
        lit != live_cfg.sideband.basic_block_live_at_start.end()) {
      for (int r : lit->second) {
        if (r >= 0 && r < kMipsFirstPseudoRegister) {
          winfo.live_in_regs.insert(r);
        }
      }
    }
    absl::btree_set<int> killed_so_far;
    // Identify ABI `call_fusage` argument defs inside block `b` (matching
    // `ProjectPostReloadBlockToSched1View`'s `abi_pinned_defs`).
    std::vector<absl::btree_set<int>> blk_call_arg_defs(region.sched_block.size());
    for (int ci = bounds.prologue_end; ci < bounds.epilogue_begin; ++ci) {
      if (region.sched_block[static_cast<size_t>(ci)].kind != Insn::Kind::kCall) {
        continue;
      }
      absl::btree_set<int> needed;
      for (const RtlExpr& fu : region.sched_block[static_cast<size_t>(ci)].pn.call_fusage) {
        CollectRegReads(fu, &needed);
      }
      for (int prev = ci - 1; prev >= bounds.prologue_end && !needed.empty(); --prev) {
        if (excluded_prologue.contains(prev - bounds.prologue_end)) {
          continue;
        }
        if (region.sched_block[static_cast<size_t>(prev)].kind == Insn::Kind::kCall) {
          break;
        }
        absl::btree_map<int, int> pdefs;
        absl::btree_map<int, RtlMachineMode> pmodes;
        CollectHardRegDefBaseRegnos(region.sched_block[static_cast<size_t>(prev)].pn.pattern,
                                    &pdefs, &pmodes);
        for (const auto& [r, cnt] : pdefs) {
          const int nr = std::max(1, mm.HardRegNRegs(r, pmodes[r]));
          bool hit = false;
          for (int k = 0; k < nr; ++k) {
            if (needed.erase(r + k) > 0) {
              hit = true;
            }
          }
          if (hit) {
            blk_call_arg_defs[static_cast<size_t>(prev)].insert(r);
          }
        }
      }
    }
    auto has_integrated_def = [](const RtlExpr& pat, int regno) {
      auto check = [&](const RtlExpr& e) {
        return e.op == RtlOp::kSet && e.children.size() == 2 && e.SetDest().op == RtlOp::kReg &&
               e.SetDest().RegNo() == regno && e.SetDest().flags.integrated;
      };
      if (pat.op == RtlOp::kParallel) {
        for (const RtlExpr& ch : pat.children) {
          if (check(ch)) {
            return true;
          }
        }
        return false;
      }
      return check(pat);
    };
    absl::btree_map<int, RtlMachineMode> active_def_mode;
    absl::btree_map<int, int> last_body_def_idx;
    absl::btree_set<std::pair<int, int>> counted_unrenamed_insn_reg;
    // Hard registers whose incoming live-in value already died via `REG_DEAD`
    // on an earlier instruction in this block (`flow.c:2337, 2505-2520`,
    // `mark_used_regs`); any later `SET` starts a new disjoint live range rather
    // than an RMW of the incoming web.
    absl::btree_set<int> incoming_dead_so_far;
    for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
      if (excluded_prologue.contains(i - bounds.prologue_end)) {
        continue;
      }
      const Insn& body_insn = region.sched_block[static_cast<size_t>(i)];
      const RtlExpr& pat = body_insn.pn.pattern;
      absl::btree_set<int> insn_reads;
      CollectRegReads(pat, &insn_reads);
      absl::btree_set<int> fusage_reads;
      for (const RtlExpr& fu : body_insn.pn.call_fusage) {
        CollectRegReads(fu, &fusage_reads);
      }
      for (int r : fusage_reads) {
        if (r >= 0 && r < kMipsFirstPseudoRegister && !killed_so_far.contains(r)) {
          winfo.upward_call_fusage_use_regs.insert(r);
          winfo.upward_use_regs.insert(r);
        }
      }
      for (int r : insn_reads) {
        if (r >= 0 && r < kMipsFirstPseudoRegister && !killed_so_far.contains(r)) {
          winfo.upward_use_regs.insert(r);
        }
      }
      if (body_insn.kind == Insn::Kind::kCall) {
        for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
          if (mm.CallUsed(r)) {
            killed_so_far.insert(r);
            active_def_mode.erase(r);
          }
        }
      }
      if (!caller_save_restore_indices.contains(i)) {
        absl::btree_map<int, int> insn_def_counts;
        absl::btree_map<int, RtlMachineMode> insn_def_modes;
        CollectHardRegDefBaseRegnos(pat, &insn_def_counts, &insn_def_modes);
        for (const auto& [r, cnt] : insn_def_counts) {
          out.block_total_defs[b][r] += cnt;
          last_body_def_idx[r] = i;
          bool read_later_in_block = false;
          for (int k = i + 1; k < bounds.epilogue_begin; ++k) {
            if (excluded_prologue.contains(k - bounds.prologue_end)) {
              continue;
            }
            const Insn& later = region.sched_block[static_cast<size_t>(k)];
            absl::btree_set<int> later_reads;
            CollectRegReads(later.pn.pattern, &later_reads);
            for (const RtlExpr& fu : later.pn.call_fusage) {
              CollectRegReads(fu, &later_reads);
            }
            if (later_reads.contains(r)) {
              read_later_in_block = true;
              break;
            }
            absl::btree_map<int, int> later_defs;
            absl::btree_map<int, RtlMachineMode> later_modes;
            CollectHardRegDefBaseRegnos(later.pn.pattern, &later_defs, &later_modes);
            if (later_defs.contains(r) || (later.kind == Insn::Kind::kCall && mm.CallUsed(r))) {
              break;
            }
          }
          const bool is_dead_unused_def = !read_later_in_block && !live_at_end.contains(r);
          const bool is_unrenamed_hard =
              (body_insn.kind == Insn::Kind::kCall) || has_integrated_def(pat, r) ||
              blk_call_arg_defs[static_cast<size_t>(i)].contains(r) || is_dead_unused_def ||
              (r >= kMipsMdRegFirst && r <= kMipsMdRegLast);
          const RtlMachineMode dmode = insn_def_modes[r];
          const int nw = std::max(1, mm.HardRegNRegs(r, dmode));
          if (is_unrenamed_hard) {
            for (int w = 0; w < nw && r + w < kMipsFirstPseudoRegister; ++w) {
              out.block_unrenamed_hard_defs[b][r + w] += cnt;
              counted_unrenamed_insn_reg.insert({i, r + w});
            }
          }
          last_def_mode[r] = dmode;
          if (winfo.upward_use_regs.contains(r) && !killed_so_far.contains(r) &&
              !incoming_dead_so_far.contains(r) &&
              !HasRegDeadNoteCovering(body_insn.pn.reg_notes, r, mm)) {
            winfo.upward_use_rmw_regs.insert(r);
          }
          for (int w = 0; w < nw && r + w < kMipsFirstPseudoRegister; ++w) {
            killed_so_far.insert(r + w);
            active_def_mode.erase(r + w);
          }
          active_def_mode[r] = dmode;
        }
      }
      for (const RtlRegNote& note : body_insn.pn.reg_notes) {
        if (note.kind != RtlMachineMode::kRegDead || !note.datum.IsReg()) {
          continue;
        }
        const int nr = note.datum.RegNo();
        if (nr < 0 || nr >= kMipsFirstPseudoRegister) {
          continue;
        }
        const int nw = std::max(1, mm.HardRegNRegs(nr, note.datum.mode));
        for (int w = 0; w < nw && nr + w < kMipsFirstPseudoRegister; ++w) {
          if (!killed_so_far.contains(nr + w)) {
            incoming_dead_so_far.insert(nr + w);
          }
        }
      }
      if (body_insn.kind != Insn::Kind::kCall &&
          PatternHasStackPointerEscape(pat, out.stack_layout.frame_pointer_needed)) {
        out.has_stack_addr_escape = true;
      }
      if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
        int64_t slot_off = 0;
        if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kMem &&
            !pat.SetSrc().children.empty() && !pat.SetSrc().flags.volatil &&
            !pat.SetSrc().flags.in_struct && !pat.SetSrc().flags.unchanging &&
            TryExtractPlainStackSlotOffset(pat.SetSrc().children[0], &slot_off)) {
          out.stack_layout.stack_slot_load_offsets.insert(slot_off);
          out.block_spill_load_offsets[b].insert(slot_off);
        } else if (pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
                   !pat.SetDest().flags.volatil && !pat.SetDest().flags.in_struct &&
                   !pat.SetDest().flags.unchanging && pat.SetSrc().op == RtlOp::kReg &&
                   TryExtractPlainStackSlotOffset(pat.SetDest().children[0], &slot_off)) {
          out.stack_layout.stack_slot_store_offsets.insert(slot_off);
          out.block_spill_store_counts[b][slot_off] += 1;
          out.cfg_spill_store_counts[slot_off] += 1;
        }
      }
      // `reload1.c` inherited spill slots may have no explicit reload `(set reg (mem sp+off))`
      // when every reader inherits the reload register directly, leaving only
      // `(REG_DEAD (mem sp+off))` on the final consumer (`reload1.c:6134-6140`).
      for (const RtlRegNote& note : body_insn.pn.reg_notes) {
        int64_t dead_off = 0;
        if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem &&
            !note.datum.children.empty() && !note.datum.flags.volatil &&
            !note.datum.flags.in_struct && !note.datum.flags.unchanging &&
            TryExtractPlainStackSlotOffset(note.datum.children[0], &dead_off)) {
          out.stack_layout.stack_slot_load_offsets.insert(dead_off);
          out.block_spill_load_offsets[b].insert(dead_off);
        }
      }
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg &&
          pat.SetSrc().op == RtlOp::kReg) {
        const int d = pat.SetDest().RegNo();
        const int s = pat.SetSrc().RegNo();
        // `global.c:1010-1014` and `local-alloc.c:2148` only apply hard-reg copy
        // preferences within compatible register classes (`GR_REGS` vs `FP_REGS`);
        // cross-bank GPR<->FPR moves (including `reload1.c` `mtc1`/`mfc1` reloads)
        // never create copy-tie coalescing between disjoint register files.
        const bool same_bank = (d < kMipsFpRegFirst && s < kMipsFpRegFirst) ||
                               (d >= kMipsFpRegFirst && d <= kMipsFpRegLast &&
                                s >= kMipsFpRegFirst && s <= kMipsFpRegLast);
        if (same_bank && d >= 0 && d < kMipsFirstPseudoRegister && s >= 0 &&
            s < kMipsFirstPseudoRegister && d != s) {
          const int dn = mm.HardRegNRegs(d, pat.SetDest().mode);
          const int sn = mm.HardRegNRegs(s, pat.SetSrc().mode);
          for (int kd = 0; kd < dn; ++kd) {
            for (int ks = 0; ks < sn; ++ks) {
              const int rd = d + kd;
              const int rs = s + ks;
              if (rd != rs) {
                out.cfg_copy_pairs.insert({std::min(rd, rs), std::max(rd, rs)});
              }
            }
          }
        }
      }
    }
    // Identify dedicated `caller-save.c:248-340, 575-630` save-area slots
    // (`s_reg == l_reg`, `CallUsed(s_reg)`, `saw_call`, `!redef_before_call`):
    // `insert_save`/`insert_restore` emit around the `CALL_INSN`, never
    // contiguously with the defining/consuming insn (`reload1.c:emit_reload_insns`).
    if (bounds.epilogue_begin > bounds.prologue_end) {
      const size_t blen = static_cast<size_t>(bounds.epilogue_begin - bounds.prologue_end);
      std::vector<bool> cs_overhead(blen, false);
      for (int rel : excluded_prologue) {
        if (rel >= 0 && static_cast<size_t>(rel) < blen) {
          cs_overhead[static_cast<size_t>(rel)] = true;
        }
      }
      const absl::Span<const Insn> body_span =
          absl::MakeConstSpan(region.sched_block).subspan(bounds.prologue_end, blen);
      const IntraBlockSpillSlotClassification cs_slots = ClassifyIntraBlockSpillAndCallerSaveSlots(
          body_span, mm, /*has_stack_addr_escape=*/true, &cs_overhead);
      for (int s_rel : cs_slots.caller_save_store_indices) {
        if (auto acc = TryParsePlainStackSlotMove(body_span[static_cast<size_t>(s_rel)]);
            acc.has_value() && mm.CallUsed(acc->regno)) {
          out.stack_layout.caller_save_slot_offsets.insert(acc->key.offset);
        }
      }
    }
    // Record upward-exposed reads from trailing `(use ...)` return markers and
    // any un-unpacked `epilogue_delay_pattern` on exit blocks.
    for (int i = bounds.epilogue_begin; i < static_cast<int>(region.sched_block.size()); ++i) {
      const RtlExpr& pat = region.sched_block[static_cast<size_t>(i)].pn.pattern;
      if (pat.op == RtlOp::kUse) {
        absl::btree_set<int> use_reads;
        CollectRegReads(pat, &use_reads);
        for (int r : use_reads) {
          if (r >= 0 && r < kMipsFirstPseudoRegister) {
            out.block_epilogue_use_regs[b].insert(r);
            if (!killed_so_far.contains(r)) {
              winfo.upward_return_use_regs.insert(r);
              winfo.upward_use_regs.insert(r);
            }
          }
        }
      }
    }
    if (blk.successors.empty() && cfg.sideband.epilogue_delay_pattern.has_value() &&
        !cfg.sideband.epilogue_delay_pattern->empty()) {
      if (auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern); ep.ok()) {
        absl::btree_set<int> ep_reads;
        CollectRegReads(ep->pattern, &ep_reads);
        for (int r : ep_reads) {
          if (r >= 0 && r < kMipsFirstPseudoRegister && !killed_so_far.contains(r)) {
            winfo.upward_use_regs.insert(r);
          }
        }
      }
    }
    if (b == 0) {
      for (int hr : extra_b0_defs) {
        if (hr >= 0 && hr < kMipsFirstPseudoRegister && !out.block_total_defs[0].contains(hr)) {
          out.block_total_defs[0][hr] += 1;
          out.block_unrenamed_hard_defs[0][hr] += 1;
          if (!active_def_mode.contains(hr)) {
            active_def_mode[hr] = RtlMachineMode::kVoid;
            killed_so_far.insert(hr);
          }
        }
      }
    }
    winfo.kill_regs = std::move(killed_so_far);
    for (const auto& [r, cnt] : out.block_total_defs[b]) {
      out.cfg_total_defs[r] += cnt;
      if (live_at_end.contains(r)) {
        out.block_reaching_defs[b].insert(r);
        out.cfg_reaching_defs[r] += 1;
        if (last_body_def_idx.contains(r) &&
            !counted_unrenamed_insn_reg.contains({last_body_def_idx[r], r})) {
          if (cfg_return_use_regs.contains(r) || cfg_call_fusage_regs.contains(r)) {
            winfo.uncounted_exit_def_regs.insert(r);
          }
        }
        const auto mit = last_def_mode.find(r);
        const RtlMachineMode def_mode =
            (mit != last_def_mode.end()) ? mit->second : RtlMachineMode::kVoid;
        out.block_reaching_defs_by_mode[b].insert({r, def_mode});
        out.cfg_reaching_defs_by_mode[{r, def_mode}] += 1;
      }
    }
    for (const auto& [r, cnt] : out.block_unrenamed_hard_defs[b]) {
      out.cfg_unrenamed_hard_defs[r] += cnt;
    }
    for (const auto& [r, md] : active_def_mode) {
      if (live_at_end.contains(r)) {
        winfo.gen_def_mode[r] = md;
      }
    }
    for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
      if (!excluded_prologue.contains(i - bounds.prologue_end)) {
        all_body_insns_for_spill_bounds.push_back(region.sched_block[static_cast<size_t>(i)]);
      }
    }
    // Track non-call-used hard registers live across any `CALL_INSN` in `b`
    // (`global.c:927`, `local-alloc.c:2106`).
    {
      absl::btree_set<int> cur_live = live_at_end;
      for (int i = bounds.epilogue_begin - 1; i >= bounds.prologue_end; --i) {
        if (excluded_prologue.contains(i - bounds.prologue_end)) {
          continue;
        }
        const Insn& insn = region.sched_block[static_cast<size_t>(i)];
        absl::btree_set<int> fdefs;
        absl::btree_set<int> pdefs;
        CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
        for (int d : fdefs) {
          cur_live.erase(d);
        }
        if (insn.kind == Insn::Kind::kCall) {
          for (auto it = cur_live.begin(); it != cur_live.end();) {
            if (mm.CallUsed(*it)) {
              it = cur_live.erase(it);
            } else {
              out.stack_layout.cfg_call_crossing_regs.insert(*it);
              ++it;
            }
          }
        }
        absl::btree_set<int> rds;
        CollectRegReads(insn.pn.pattern, &rds);
        for (const RtlExpr& fu : insn.pn.call_fusage) {
          CollectRegReads(fu, &rds);
        }
        for (int r : rds) {
          if (r >= 0 && r < kMipsFirstPseudoRegister) {
            cur_live.insert(r);
          }
        }
      }
    }
    for (int64_t off : out.block_spill_load_offsets[b]) {
      out.cfg_spill_load_block_counts[off] += 1;
    }
  }
  ComputeExactCfgWebDefCounts(cfg, web_blk_info, &out);
  if (any_call_in_cfg) {
    // `config/mips/mips.c:4465`, `mips.h:1649`: outgoing arg area is at least
    // `4 * UNITS_PER_WORD` (16 bytes for `$a0-$a3`) and 8-byte aligned.
    const int64_t min_args = std::max<int64_t>(16, max_call_arg_bytes);
    out.stack_layout.starting_frame_offset = (min_args + 7) & ~int64_t{7};
  }
  out.stack_layout.min_valid_reload_spill_offset = ComputeMinValidReloadSpillOffset(
      all_body_insns_for_spill_bounds, out.stack_layout, mm, out.stack_layout.frame_pointer_needed);
  // Mirror `FoldSpilledConstDoubleReloads` (`invert_reload.cc`): mark self-loop
  // blocks with a unique non-self predecessor (`reload1.c:517-524, 566-573`).
  out.block_has_self_loop_preheader.assign(cfg.rpo_blocks.size(), false);
  for (size_t b = 0; b < cfg.rpo_blocks.size(); ++b) {
    const auto& succs = cfg.rpo_blocks[b].successors;
    if (std::find(succs.begin(), succs.end(), static_cast<int>(b)) == succs.end()) {
      continue;
    }
    int pre_count = 0;
    for (size_t p = 0; p < cfg.rpo_blocks.size(); ++p) {
      if (p == b) {
        continue;
      }
      const auto& psuccs = cfg.rpo_blocks[p].successors;
      if (std::find(psuccs.begin(), psuccs.end(), static_cast<int>(b)) != psuccs.end()) {
        ++pre_count;
      }
    }
    if (pre_count == 1) {
      out.block_has_self_loop_preheader[b] = true;
    }
  }
  return out;
}

Sched1ProjectionContext BuildSched1ProjectionContextForBlock(size_t block_idx,
                                                             const CfgSched1Context& ctx) {
  Sched1ProjectionContext proj;
  proj.has_cfg_context = true;
  proj.is_entry_block = (block_idx == 0);
  for (const auto& [r, total] : ctx.cfg_unrenamed_hard_defs) {
    int own = 0;
    if (const auto it = ctx.block_unrenamed_hard_defs[block_idx].find(r);
        it != ctx.block_unrenamed_hard_defs[block_idx].end()) {
      own = it->second;
    }
    if (total > own) {
      proj.other_block_total_defs[r] = total - own;
    }
  }
  for (const auto& [r, reach] : ctx.cfg_reaching_defs) {
    const int own = ctx.block_reaching_defs[block_idx].contains(r) ? 1 : 0;
    if (reach > own) {
      proj.cross_block_reaching_defs[r] = reach - own;
    }
  }
  if (block_idx < ctx.block_reaching_defs_by_mode.size()) {
    proj.has_reaching_defs_by_mode = true;
    for (const auto& [key, reach] : ctx.cfg_reaching_defs_by_mode) {
      const int own = ctx.block_reaching_defs_by_mode[block_idx].contains(key) ? 1 : 0;
      if (reach > own) {
        proj.cross_block_reaching_defs_by_mode[key] = reach - own;
      }
    }
  }
  if (block_idx < ctx.block_entry_web_other_defs.size() &&
      block_idx < ctx.block_exit_web_other_defs.size()) {
    proj.has_per_endpoint_web_defs = true;
    proj.entry_web_other_defs = ctx.block_entry_web_other_defs[block_idx];
    proj.exit_web_other_defs = ctx.block_exit_web_other_defs[block_idx];
    if (block_idx < ctx.block_entry_exit_same_web_regs.size()) {
      proj.entry_exit_same_web_regs = ctx.block_entry_exit_same_web_regs[block_idx];
    }
  }
  if (block_idx < ctx.block_epilogue_use_regs.size()) {
    proj.epilogue_use_regs = ctx.block_epilogue_use_regs[block_idx];
  }
  proj.stack_layout = ctx.stack_layout;
  proj.has_stack_addr_escape = ctx.has_stack_addr_escape;
  if (block_idx < ctx.block_has_self_loop_preheader.size()) {
    proj.has_self_loop_preheader = ctx.block_has_self_loop_preheader[block_idx];
  }
  if (block_idx < ctx.block_spill_store_counts.size()) {
    for (const auto& [off, total] : ctx.cfg_spill_store_counts) {
      int own = 0;
      if (const auto it = ctx.block_spill_store_counts[block_idx].find(off);
          it != ctx.block_spill_store_counts[block_idx].end()) {
        own = it->second;
      }
      if (total > own) {
        proj.other_block_spill_store_counts[off] = total - own;
      }
    }
  }
  if (block_idx < ctx.block_spill_load_offsets.size()) {
    for (const auto& [off, blk_cnt] : ctx.cfg_spill_load_block_counts) {
      const int own = ctx.block_spill_load_offsets[block_idx].contains(off) ? 1 : 0;
      if (blk_cnt > own) {
        proj.other_block_spill_load_offsets.insert(off);
      }
    }
  }
  return proj;
}

namespace {}  // namespace

namespace detail {

InsnRegReadDefDeaths CollectInsnRegReadDefDeaths(const Insn& insn, const MachineModel& mm) {
  InsnRegReadDefDeaths out;
  out.is_call_or_use = (insn.kind == Insn::Kind::kCall || insn.pn.pattern.op == RtlOp::kUse);
  auto insert_reg = [&](int r, RtlMachineMode mode, absl::btree_set<int>* dst) {
    if (r < 0) {
      return;
    }
    const int nr = (r < kMipsFirstPseudoRegister) ? mm.HardRegNRegs(r, mode) : 1;
    for (int k = 0; k < nr; ++k) {
      dst->insert(r + k);
    }
  };
  auto walk_reads = [&](auto& self, const RtlExpr& node) -> void {
    if (node.op == RtlOp::kReg) {
      insert_reg(node.RegNo(), node.mode, &out.reads);
      return;
    }
    if (node.op == RtlOp::kSet && node.children.size() == 2) {
      if (node.SetDest().op == RtlOp::kMem && !node.SetDest().children.empty()) {
        self(self, node.SetDest().children[0]);
      } else if (node.SetDest().op != RtlOp::kReg) {
        self(self, node.SetDest());
      }
      self(self, node.SetSrc());
      return;
    }
    if (node.op == RtlOp::kClobber) {
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch);
    }
  };
  auto walk_defs = [&](const RtlExpr& pat) {
    auto handle = [&](const RtlExpr& elem) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        const RtlExpr& d = elem.SetDest();
        if (d.op == RtlOp::kReg) {
          insert_reg(d.RegNo(), d.mode, &out.defs);
        } else if (d.op != RtlOp::kMem) {
          const RtlExpr* cur = &d;
          while ((cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
                  cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) &&
                 !cur->children.empty()) {
            cur = &cur->children[0];
          }
          if (cur->op == RtlOp::kReg) {
            insert_reg(cur->RegNo(), cur->mode, &out.defs);
          }
        }
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
        const RtlExpr* cur = &elem.children[0];
        while (cur->op == RtlOp::kSubreg && !cur->children.empty()) {
          cur = &cur->children[0];
        }
        if (cur->op == RtlOp::kReg) {
          insert_reg(cur->RegNo(), cur->mode, &out.defs);
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        handle(ch);
      }
    } else {
      handle(pat);
    }
  };
  walk_reads(walk_reads, insn.pn.pattern);
  for (const RtlExpr& fu : insn.pn.call_fusage) {
    walk_reads(walk_reads, fu);
    walk_defs(fu);
  }
  walk_defs(insn.pn.pattern);
  if (insn.kind == Insn::Kind::kCall) {
    for (int hr = 0; hr < kMipsFirstPseudoRegister; ++hr) {
      if (mm.CallUsed(hr)) {
        out.defs.insert(hr);
      }
    }
  }
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
      insert_reg(note.datum.RegNo(), note.datum.mode, &out.deaths);
    }
  }
  return out;
}

}  // namespace detail

namespace {

bool IsMdOutputReloadInsn(const Insn& insn) {
  const RtlExpr& pat = insn.pn.pattern;
  return pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetSrc().op == RtlOp::kReg &&
         (pat.SetSrc().RegNo() == kMipsMdRegFirst || pat.SetSrc().RegNo() == kMipsMdRegFirst + 1);
}

}  // namespace

bool IsAdjacentMdOutputReloadPair(const Insn& first_insn, const Insn& second_insn) {
  // Preserve `reload1.c:emit_reload_insns` `mfhi`/`mflo` followed immediately by
  // its consumer carrying `(REG_DEAD hi|lo)` so `InvertReload` never loses the
  // `hi`/`lo` death attribution on the consumer.
  if (IsMdOutputReloadInsn(first_insn)) {
    const int md_reg = first_insn.pn.pattern.SetSrc().RegNo();
    for (const RtlRegNote& note : second_insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
          note.datum.RegNo() == md_reg) {
        return true;
      }
    }
  }
  if (!IsMdOutputReloadInsn(second_insn)) {
    return false;
  }
  const int md_reg = second_insn.pn.pattern.SetSrc().RegNo();
  if (IsMdOutputReloadInsn(first_insn) && first_insn.pn.pattern.SetSrc().RegNo() != md_reg) {
    return true;
  }
  auto sets_md_reg = [md_reg](const RtlExpr& elem) -> bool {
    if (elem.op != RtlOp::kSet || elem.children.size() != 2 || elem.SetDest().op != RtlOp::kReg) {
      return false;
    }
    const int d = elem.SetDest().RegNo();
    if (d == md_reg) {
      return true;
    }
    return d == kMipsMdRegFirst && elem.SetDest().mode == RtlMachineMode::kDi &&
           md_reg == kMipsMdRegFirst + 1;
  };
  const RtlExpr& p1 = first_insn.pn.pattern;
  if (p1.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : p1.children) {
      if (sets_md_reg(ch)) {
        return true;
      }
    }
    return false;
  }
  return sets_md_reg(p1);
}

namespace {}  // namespace

namespace detail {

bool BuildPreferredSched1OrderInBase(absl::Span<const Insn> base_block, int prologue_end,
                                     int epilogue_begin, const Sched1ProjectionView& base_view,
                                     const absl::btree_set<int>& excluded_prologue_body_indices,
                                     const MachineModel& mm, std::vector<int>* preferred_out,
                                     absl::Span<const int> extra_pinned_body_indices,
                                     absl::Span<const int> explicit_s1_order) {
  const int n = static_cast<int>(base_block.size());
  const int body_len = epilogue_begin - prologue_end;
  const int num_core = static_cast<int>(base_view.core_insns.size());
  std::vector<int> s1_order;
  if (!explicit_s1_order.empty()) {
    if (static_cast<int>(explicit_s1_order.size()) != num_core) {
      return false;
    }
    s1_order.assign(explicit_s1_order.begin(), explicit_s1_order.end());
  } else {
    s1_order = ScheduleSched1ViewCoreOrder(base_view, mm);
    if (static_cast<int>(s1_order.size()) != num_core) {
      return false;
    }
  }
  std::vector<int> body_to_core(static_cast<size_t>(body_len), -1);
  for (int ci = 0; ci < num_core; ++ci) {
    body_to_core[static_cast<size_t>(base_view.core_to_orig_index[static_cast<size_t>(ci)])] = ci;
  }
  absl::btree_set<int> pinned_prefix_set(base_view.pinned_prefix_orig_indices.begin(),
                                         base_view.pinned_prefix_orig_indices.end());
  absl::btree_set<int> extra_pinned_core_set;
  std::vector<int> extra_pinned_cores;
  for (int rel : extra_pinned_body_indices) {
    if (rel >= 0 && rel < body_len) {
      const int ci = body_to_core[static_cast<size_t>(rel)];
      if (ci >= 0 && extra_pinned_core_set.insert(ci).second) {
        extra_pinned_cores.push_back(ci);
      }
    }
  }
  std::vector<int> interleaved_prologue_bis;
  std::vector<int> pinned_prefix_bis;
  std::vector<std::vector<int>> before_core(static_cast<size_t>(num_core));
  std::vector<std::vector<int>> after_core(static_cast<size_t>(num_core));
  std::vector<int> after_last_core;
  for (int rel = 0; rel < body_len; ++rel) {
    if (body_to_core[static_cast<size_t>(rel)] >= 0) {
      continue;
    }
    const int bi = prologue_end + rel;
    if (excluded_prologue_body_indices.contains(rel)) {
      interleaved_prologue_bis.push_back(bi);
      continue;
    }
    if (pinned_prefix_set.contains(rel)) {
      pinned_prefix_bis.push_back(bi);
      continue;
    }
    if (const auto ait = base_view.output_reload_after_core.find(rel);
        ait != base_view.output_reload_after_core.end() && ait->second >= 0 &&
        ait->second < num_core) {
      after_core[static_cast<size_t>(ait->second)].push_back(bi);
      continue;
    }
    if (const auto bit = base_view.input_reload_before_core.find(rel);
        bit != base_view.input_reload_before_core.end() && bit->second >= 0 &&
        bit->second < num_core) {
      before_core[static_cast<size_t>(bit->second)].push_back(bi);
      continue;
    }
    int next_core = -1;
    for (int k = rel + 1; k < body_len; ++k) {
      if (body_to_core[static_cast<size_t>(k)] >= 0) {
        next_core = body_to_core[static_cast<size_t>(k)];
        break;
      }
    }
    if (next_core >= 0) {
      before_core[static_cast<size_t>(next_core)].push_back(bi);
    } else {
      after_last_core.push_back(bi);
    }
  }
  preferred_out->clear();
  preferred_out->reserve(static_cast<size_t>(n));
  for (int i = 0; i < prologue_end; ++i) {
    preferred_out->push_back(i);
  }
  for (int pbi : interleaved_prologue_bis) {
    preferred_out->push_back(pbi);
  }
  for (int ppbi : pinned_prefix_bis) {
    preferred_out->push_back(ppbi);
  }
  for (int ci : extra_pinned_cores) {
    for (int aux_bi : before_core[static_cast<size_t>(ci)]) {
      preferred_out->push_back(aux_bi);
    }
    preferred_out->push_back(prologue_end + base_view.core_to_orig_index[static_cast<size_t>(ci)]);
    for (int aux_bi : after_core[static_cast<size_t>(ci)]) {
      preferred_out->push_back(aux_bi);
    }
  }
  for (int ci : s1_order) {
    if (extra_pinned_core_set.contains(ci)) {
      continue;
    }
    for (int aux_bi : before_core[static_cast<size_t>(ci)]) {
      preferred_out->push_back(aux_bi);
    }
    preferred_out->push_back(prologue_end + base_view.core_to_orig_index[static_cast<size_t>(ci)]);
    for (int aux_bi : after_core[static_cast<size_t>(ci)]) {
      preferred_out->push_back(aux_bi);
    }
  }
  for (int aux_bi : after_last_core) {
    preferred_out->push_back(aux_bi);
  }
  for (int i = epilogue_begin; i < n; ++i) {
    preferred_out->push_back(i);
  }
  return static_cast<int>(preferred_out->size()) == n;
}

}  // namespace detail

namespace {

// Mirrors `reload1.c:2185-2250, 2675-2720`: `eliminate_regs` only rewrites
// `REG_NOTES` on insns whose pattern triggers register elimination. When a
// store-equiv initializer's pattern has no stack-pointer/frame-pointer operand,
// its `REG_EQUIV (mem ...)` note retains its pre-elimination `$fp`/`argp` base.
RtlExpr UneliminateMemForRegEquivNoteIfNeeded(const RtlExpr& mem, const RtlExpr& init_pat,
                                              const PostReloadStackLayout& layout) {
  if (!layout.valid) {
    return mem;
  }
  const int elim_to_reg =
      layout.frame_pointer_needed ? kMipsFramePointerRegno : kMipsStackPointerRegno;
  bool pat_triggers_elim = false;
  init_pat.Visit([&](const RtlExpr& node) {
    if (pat_triggers_elim) {
      return;
    }
    if (node.op == RtlOp::kReg &&
        (node.IsArgPointerReg() ||
         (!node.IsArgPointerReg() &&
          (node.RegNo() == elim_to_reg || node.RegNo() == kMipsFramePointerRegno)))) {
      pat_triggers_elim = true;
    }
  });
  if (pat_triggers_elim) {
    return mem;
  }
  return NormalizeMemForRegEquivMatch(mem, layout);
}

}  // namespace

namespace detail {

bool SameAddrIgnoringRegNames(const RtlExpr& a, const RtlExpr& b) {
  if (a.op != b.op || a.mode != b.mode || a.children.size() != b.children.size()) {
    return false;
  }
  if (a.op == RtlOp::kReg) {
    return a.IsArgPointerReg() == b.IsArgPointerReg() &&
           (a.IsArgPointerReg() || a.RegNo() == b.RegNo());
  }
  if (a.int_val != b.int_val || a.int_val_high != b.int_val_high || a.str_val != b.str_val) {
    return false;
  }
  for (size_t i = 0; i < a.children.size(); ++i) {
    if (!SameAddrIgnoringRegNames(a.children[i], b.children[i])) {
      return false;
    }
  }
  return true;
}

bool SameMemSlotIgnoringRegNames(const RtlExpr& a, const RtlExpr& b,
                                 const PostReloadStackLayout& layout) {
  if (a.op != RtlOp::kMem || b.op != RtlOp::kMem || a.mode != b.mode || a.children.empty() ||
      b.children.empty()) {
    return false;
  }
  if (SameAddrIgnoringRegNames(a.children[0], b.children[0])) {
    return true;
  }
  const RtlExpr na = NormalizeMemForRegEquivMatch(a, layout);
  const RtlExpr nb = NormalizeMemForRegEquivMatch(b, layout);
  return !na.children.empty() && !nb.children.empty() &&
         SameAddrIgnoringRegNames(na.children[0], nb.children[0]);
}

bool HasMatchingMemRegEquivNote(const Insn& insn, const RtlExpr& target_mem,
                                const PostReloadStackLayout& layout) {
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEquiv &&
        SameMemSlotIgnoringRegNames(note.datum, target_mem, layout)) {
      return true;
    }
  }
  return false;
}

// Locates the unique preceding single-set initializer of `target_reg` before
// `store_pos` in `seq`, mirroring `local-alloc.c:990-1027` and
// `ComputeHardRegLivenessSummary`.
int FindUniqueInitPosForStoreInSeq(absl::Span<const Insn> seq, int store_pos, int target_reg,
                                   const MachineModel& mm) {
  for (int j = store_pos - 1; j >= 0; --j) {
    const Insn& jinsn = seq[static_cast<size_t>(j)];
    if (jinsn.kind == Insn::Kind::kCall) {
      return -1;
    }
    const InsnRegReadDefDeaths jinfo = CollectInsnRegReadDefDeaths(jinsn, mm);
    if (!jinfo.defs.contains(target_reg)) {
      continue;
    }
    const bool is_multiset_rmw = jinfo.reads.contains(target_reg) &&
                                 !HasRegDeadNoteCovering(jinsn.pn.reg_notes, target_reg, mm);
    const RtlExpr& jpat = jinsn.pn.pattern;
    if (!is_multiset_rmw && jpat.op == RtlOp::kSet && jpat.children.size() == 2 &&
        jpat.SetDest().op == RtlOp::kReg && !jpat.SetDest().IsArgPointerReg() &&
        jpat.SetDest().RegNo() == target_reg) {
      return j;
    }
    return -1;
  }
  return -1;
}

// Mirrors `local-alloc.c:540-603, 1049-1055` (`validate_equiv_mem`,
// `update_equiv_regs`): returns true when reordering from post-`sched2`
// `base_block` to pre-`sched2` `cand_in_base` moves an invalidator `d` of
// `MEM` load `oid` (`dst_r = mem_src`) from outside `[oid, base_dst_death_pos)`
// into `[load_pos, cand_dst_death_pos)`:
//   - `(REG_DEAD base_r)` for a register mentioned in `mem_src` (`sched2` can
//     hoist `d` ahead of `oid` or hoist `REG_DEAD dst_r` ahead of `d`), or
//   - a base-register write, non-const `CALL_INSN` (when `!mem_src.flags.unchanging`),
//     or aliasing `SET`/`CLOBBER` `MEM` store `d` that lay after `dst_r`'s death
//     in `base_block` (`base_dst_death_pos >= 0 && d >= base_dst_death_pos`, i.e.
//     `sched2` hoisted `REG_DEAD dst_r` ahead of `d` without crossing `oid`).
bool HasLoadEquivInvalidatorMovedIntoWindow(
    int oid, const RtlExpr& mem_src, int dst_r, int prologue_end, int epilogue_begin,
    absl::Span<const Insn> base_block, absl::Span<const int> cand_in_base,
    absl::Span<const int> pos_in_cand, absl::Span<const InsnRegReadDefDeaths> insn_info,
    const RegFacts& facts_s2, const MachineModel& mm, const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_orig_ids, bool allow_moved_store_invalidator) {
  if (mem_src.op != RtlOp::kMem || mem_src.children.empty() || mem_src.flags.volatil) {
    return false;
  }
  const int load_pos = pos_in_cand[static_cast<size_t>(oid)];
  int cand_dst_death_pos = -1;
  for (int p = load_pos + 1; p < epilogue_begin; ++p) {
    const int oi = cand_in_base[static_cast<size_t>(p)];
    if (insn_info[static_cast<size_t>(oi)].deaths.contains(dst_r) ||
        insn_info[static_cast<size_t>(oi)].defs.contains(dst_r)) {
      cand_dst_death_pos = p;
      break;
    }
  }
  int base_dst_death_pos = -1;
  for (int b = oid + 1; b < epilogue_begin; ++b) {
    if (insn_info[static_cast<size_t>(b)].deaths.contains(dst_r) ||
        insn_info[static_cast<size_t>(b)].defs.contains(dst_r)) {
      base_dst_death_pos = b;
      break;
    }
  }
  absl::btree_set<int> addr_regs;
  mem_src.children[0].Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kMipsFirstPseudoRegister) {
      const int nr = std::max(1, mm.HardRegNRegs(node.RegNo(), node.mode));
      for (int k = 0; k < nr; ++k) {
        addr_regs.insert(node.RegNo() + k);
      }
    }
  });
  auto store_modifies_mem = [&](const RtlExpr& pat) -> bool {
    bool modified = false;
    auto check_part = [&](const RtlExpr& p) {
      if (modified) {
        return;
      }
      if ((p.op == RtlOp::kSet || p.op == RtlOp::kClobber) && !p.children.empty() &&
          p.children[0].op == RtlOp::kMem) {
        if (TrueDependence(p.children[0], mem_src, facts_s2, Phase::kSched2) ||
            PreReloadDistinctStackBasesConflict(p.children[0], mem_src, stack_layout)) {
          modified = true;
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& sub : pat.children) {
        check_part(sub);
      }
    } else {
      check_part(pat);
    }
    return modified;
  };
  for (int d = prologue_end; d < epilogue_begin; ++d) {
    if (excluded_orig_ids.contains(d)) {
      continue;
    }
    const bool was_in_base_window =
        (d >= oid) && (base_dst_death_pos < 0 || d < base_dst_death_pos);
    if (was_in_base_window) {
      continue;
    }
    const int d_pos = pos_in_cand[static_cast<size_t>(d)];
    if (d_pos < load_pos || (cand_dst_death_pos >= 0 && d_pos >= cand_dst_death_pos)) {
      continue;
    }
    for (int dead_r : insn_info[static_cast<size_t>(d)].deaths) {
      if (addr_regs.contains(dead_r)) {
        return true;
      }
    }
    if (base_dst_death_pos >= 0 && d >= base_dst_death_pos) {
      for (int def_r : insn_info[static_cast<size_t>(d)].defs) {
        if (addr_regs.contains(def_r)) {
          return true;
        }
      }
      const Insn& d_insn = base_block[static_cast<size_t>(d)];
      if (!mem_src.flags.unchanging) {
        if (d_insn.kind == Insn::Kind::kCall && !d_insn.pn.pattern.flags.unchanging) {
          return true;
        }
        if (allow_moved_store_invalidator && store_modifies_mem(d_insn.pn.pattern)) {
          return true;
        }
      }
    }
  }
  return false;
}

// Mirrors `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`,
// `memref_used_between_p`, `update_equiv_regs`): returns true when reordering
// from post-`sched2` `base_block` to pre-`sched2` `cand_in_base` moves an
// instruction `d` that invalidates `mem_dst` (base-reg write/death, non-const
// call, or overlapping `MEM` access) from outside `[init_oid, store_oid)` into
// `[init_pos, store_pos)`.
bool HasStoreEquivInvalidatorMovedIntoWindow(
    int store_oid, int init_pos, const RtlExpr& mem_dst, int prologue_end, int epilogue_begin,
    absl::Span<const Insn> base_block, absl::Span<const int> cand_in_base,
    absl::Span<const int> pos_in_cand, absl::Span<const InsnRegReadDefDeaths> insn_info,
    const RegFacts& facts_s2, const MachineModel& mm, const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_orig_ids) {
  if (init_pos < 0 || mem_dst.op != RtlOp::kMem || mem_dst.children.empty() ||
      mem_dst.flags.volatil) {
    return false;
  }
  const int store_pos = pos_in_cand[static_cast<size_t>(store_oid)];
  const int init_oid = cand_in_base[static_cast<size_t>(init_pos)];
  if (store_pos <= init_pos) {
    return false;
  }
  absl::btree_set<int> addr_regs;
  mem_dst.children[0].Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kMipsFirstPseudoRegister) {
      const int nr = std::max(1, mm.HardRegNRegs(node.RegNo(), node.mode));
      for (int k = 0; k < nr; ++k) {
        addr_regs.insert(node.RegNo() + k);
      }
    }
  });
  for (int d = prologue_end; d < epilogue_begin; ++d) {
    if (excluded_orig_ids.contains(d)) {
      continue;
    }
    const bool was_in_base_window = (init_oid < store_oid) && (d >= init_oid && d < store_oid);
    if (was_in_base_window) {
      continue;
    }
    const int d_pos = pos_in_cand[static_cast<size_t>(d)];
    if (d_pos < init_pos || d_pos >= store_pos) {
      continue;
    }
    for (int r : insn_info[static_cast<size_t>(d)].defs) {
      if (addr_regs.contains(r)) {
        return true;
      }
    }
    for (int r : insn_info[static_cast<size_t>(d)].deaths) {
      if (addr_regs.contains(r)) {
        return true;
      }
    }
    const Insn& d_insn = base_block[static_cast<size_t>(d)];
    if (d_insn.kind == Insn::Kind::kCall && !mem_dst.flags.unchanging) {
      return true;
    }
    bool mem_conflict = false;
    d_insn.pn.pattern.Visit([&](const RtlExpr& node) {
      if (!mem_conflict && node.op == RtlOp::kMem &&
          (TrueDependence(node, mem_dst, facts_s2, Phase::kSched2) ||
           TrueDependence(mem_dst, node, facts_s2, Phase::kSched2) ||
           AntiDependence(node, mem_dst, facts_s2, Phase::kSched2) ||
           PreReloadDistinctStackBasesConflict(node, mem_dst, stack_layout))) {
        mem_conflict = true;
      }
    });
    if (mem_conflict) {
      return true;
    }
  }
  return false;
}

}  // namespace detail

namespace {

// Mirrors `local-alloc.c:700-839, 1004-1007` (`optimize_reg_copy_1`) +
// `reload1.c:5186-5257` (`choose_reload_regs` reload inheritance): when
// `copy_insn` is a cross-class FP->GPR copy `(set (reg:SF/DF r_gpr)
// (reg:SF/DF r_fpr))` carrying `(REG_DEAD r_fpr)` and `read_insn` is a later
// FPR-only consumer reading `r_fpr` with both `r_gpr` and `r_fpr` unmodified
// (and no call) in between, `reload()` inherited `r_fpr` for `r_gpr` while
// leaving `(REG_DEAD r_fpr)` on `copy_insn` (matching `invert_reload.cc`
// Pass D).
bool IsFprOnlySchedRhsExpr(const RtlExpr& rhs) {
  const bool fp_mode = (rhs.mode == RtlMachineMode::kSF || rhs.mode == RtlMachineMode::kDF);
  switch (rhs.op) {
    case RtlOp::kPlus:
    case RtlOp::kMinus:
    case RtlOp::kMult:
    case RtlOp::kDiv:
    case RtlOp::kNeg:
    case RtlOp::kAbs:
    case RtlOp::kSqrt:
    case RtlOp::kFloat:
    case RtlOp::kUnsignedFloat:
    case RtlOp::kFloatExtend:
    case RtlOp::kFloatTruncate:
      return fp_mode;
    case RtlOp::kFix:
    case RtlOp::kUnsignedFix:
    case RtlOp::kEq:
    case RtlOp::kNe:
    case RtlOp::kLt:
    case RtlOp::kLe:
    case RtlOp::kGt:
    case RtlOp::kGe:
      return !rhs.children.empty() && (rhs.children[0].mode == RtlMachineMode::kSF ||
                                       rhs.children[0].mode == RtlMachineMode::kDF);
    default:
      return false;
  }
}

}  // namespace

namespace detail {

bool IsCrossClassFprToGprCopyReloadInheritedRead(absl::Span<const Insn> block,
                                                 absl::Span<const int> order_in_block, int copy_pos,
                                                 int read_pos, int r, const MachineModel& mm) {
  if (copy_pos < 0 || read_pos <= copy_pos || read_pos >= static_cast<int>(order_in_block.size())) {
    return false;
  }
  const Insn& copy_insn = block[static_cast<size_t>(order_in_block[static_cast<size_t>(copy_pos)])];
  const Insn& read_insn = block[static_cast<size_t>(order_in_block[static_cast<size_t>(read_pos)])];
  const RtlExpr* cs = SingleSetOf(copy_insn.pn.pattern);
  if (cs == nullptr || cs->SetDest().op != RtlOp::kReg || cs->SetSrc().op != RtlOp::kReg ||
      cs->SetDest().IsArgPointerReg() || cs->SetSrc().IsArgPointerReg() ||
      cs->SetDest().mode != cs->SetSrc().mode ||
      (cs->SetDest().mode != RtlMachineMode::kSF && cs->SetDest().mode != RtlMachineMode::kDF)) {
    return false;
  }
  const int r_gpr = cs->SetDest().RegNo();
  const int r_fpr = cs->SetSrc().RegNo();
  if (r_gpr < 0 || r_gpr >= kMipsFpRegFirst || r_fpr < kMipsFpRegFirst ||
      r_fpr >= kMipsFirstPseudoRegister) {
    return false;
  }
  const int fpr_nr = std::max(1, mm.HardRegNRegs(r_fpr, cs->SetSrc().mode));
  if (r < r_fpr || r >= r_fpr + fpr_nr) {
    return false;
  }
  const RtlExpr* rs = SingleSetOf(read_insn.pn.pattern);
  if (rs == nullptr || !IsFprOnlySchedRhsExpr(rs->SetSrc())) {
    return false;
  }
  const int gpr_nr = std::max(1, mm.HardRegNRegs(r_gpr, cs->SetDest().mode));
  for (int p = copy_pos + 1; p < read_pos; ++p) {
    const Insn& mid = block[static_cast<size_t>(order_in_block[static_cast<size_t>(p)])];
    if (mid.kind == Insn::Kind::kCall || InsnDefinesHardRegSpan(mid, r_gpr, gpr_nr, mm) ||
        InsnDefinesHardRegSpan(mid, r_fpr, fpr_nr, mm)) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

namespace {

// Returns true when `cand_in_base` drops a per-live-range same-class
// hard-register interference `(lo, hi)` (`lo < hi`) on a block-local,
// non-call-crossing, call-used ordinary def of `hi` whose overlapping `lo`
// in `base_block` was also an ordinary non-reload def. Per
// `local-alloc.c:2158-2182` (`find_free_reg` ascending regno scan when
// `REG_ALLOC_ORDER` is undefined in `config/mips/mips.h`), exposing `lo` as
// free across `hi`'s entire live range forces `local-alloc` to assign `lo`
// instead of `hi` and Stage 4 to demote both to hard registers.
bool DropsPerLiveRangeLocalFindFreeRegInterference(
    absl::Span<const Insn> base_block, absl::Span<const int> cand_in_base, int prologue_end,
    int epilogue_begin, const absl::btree_set<int>& excluded_prologue_orig_ids,
    const absl::btree_set<int>& live_at_end_s2, const HardRegLivenessSummary& base_sum,
    const absl::btree_set<std::pair<int, int>>& cfg_copy_pairs,
    absl::Span<const InsnRegReadDefDeaths> insn_info, const PostReloadStackLayout& stack_layout,
    const MachineModel& mm) {
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return false;
  }
  auto same_reg_file = [](int a, int b) {
    return (a < kMipsFpRegFirst && b < kMipsFpRegFirst) ||
           (a >= kMipsFpRegFirst && a <= kMipsFpRegLast && b >= kMipsFpRegFirst &&
            b <= kMipsFpRegLast);
  };
  auto is_ordinary_local_def = [&](const Insn& insn, int rel, int r, RtlMachineMode* mode_out,
                                   const std::vector<absl::btree_set<int>>& live_after) {
    if (insn.kind != Insn::Kind::kInsn || IsMdOutputReloadInsn(insn) ||
        TryParsePlainStackSlotMove(insn, stack_layout.frame_pointer_needed).has_value()) {
      return false;
    }
    const RtlExpr* s = SingleSetOf(insn.pn.pattern);
    if (s == nullptr || s->SetDest().op != RtlOp::kReg || s->SetDest().RegNo() != r ||
        s->SetDest().flags.integrated || s->SetDest().flags.frame_related ||
        s->SetSrc().op == RtlOp::kReg) {
      return false;
    }
    // Exclude `reload1.c:517-524, 6583` rematerialized `const_double` reloads
    // (inserted post-`local-alloc` with empty `reg_notes` into FP spill regs
    // and NO `(REG_DEAD r)` on their last use), as well as `reload1.c:5641-6155`
    // spilled-output and cross-bank FPR->GPR (`mfc1`) output-reload producers
    // whose reload register `r` dies at the reload store/copy (`local-alloc`
    // never assigned `r` to those reload scratches).
    const bool is_unnoted_const_double =
        (s->SetSrc().op == RtlOp::kConstDouble && insn.pn.reg_notes.empty());
    const int nw = std::max(1, mm.HardRegNRegs(r, s->SetDest().mode));
    for (int j = rel + 1; j < body_len; ++j) {
      const int j_oid = prologue_end + j;
      if (excluded_prologue_orig_ids.contains(j_oid)) {
        continue;
      }
      const Insn& next = base_block[static_cast<size_t>(j_oid)];
      if (next.kind == Insn::Kind::kCall || next.kind == Insn::Kind::kJump) {
        break;
      }
      bool dead_after_j = true;
      for (int w = 0; w < nw; ++w) {
        if (live_after[static_cast<size_t>(j)].contains(r + w)) {
          dead_after_j = false;
          break;
        }
      }
      if (is_unnoted_const_double && dead_after_j) {
        bool reads_r = false;
        bool has_reg_dead_r = false;
        for (int w = 0; w < nw; ++w) {
          if (insn_info[static_cast<size_t>(j_oid)].reads.contains(r + w)) {
            reads_r = true;
          }
          if (insn_info[static_cast<size_t>(j_oid)].deaths.contains(r + w)) {
            has_reg_dead_r = true;
          }
        }
        if (reads_r && !has_reg_dead_r) {
          return false;
        }
      }
      if (auto sm = TryParsePlainStackSlotMove(next, stack_layout.frame_pointer_needed);
          sm.has_value() && sm->is_store && sm->regno == r && dead_after_j) {
        return false;
      }
      const RtlExpr* ns = SingleSetOf(next.pn.pattern);
      if (r >= kMipsFpRegFirst && ns != nullptr && ns->SetDest().op == RtlOp::kReg &&
          ns->SetSrc().op == RtlOp::kReg && ns->SetSrc().RegNo() == r &&
          ns->SetDest().RegNo() >= 2 && ns->SetDest().RegNo() < kMipsFpRegFirst && dead_after_j) {
        return false;
      }
      bool defs_r = false;
      for (int w = 0; w < nw; ++w) {
        if (insn_info[static_cast<size_t>(j_oid)].defs.contains(r + w)) {
          defs_r = true;
          break;
        }
      }
      if (defs_r || dead_after_j) {
        break;
      }
    }
    if (mode_out != nullptr) {
      *mode_out = s->SetDest().mode;
    }
    return true;
  };
  absl::btree_set<int> copy_tied_regs;
  for (const auto& [a, b] : cfg_copy_pairs) {
    copy_tied_regs.insert(a);
    copy_tied_regs.insert(b);
  }
  for (int rel = 0; rel < body_len; ++rel) {
    const int oid = prologue_end + rel;
    if (excluded_prologue_orig_ids.contains(oid)) {
      continue;
    }
    const RtlExpr* cs = SingleSetOf(base_block[static_cast<size_t>(oid)].pn.pattern);
    if (cs != nullptr && cs->SetDest().op == RtlOp::kReg && cs->SetSrc().op == RtlOp::kReg) {
      copy_tied_regs.insert(cs->SetDest().RegNo());
      copy_tied_regs.insert(cs->SetSrc().RegNo());
    }
  }
  auto compute_live_after = [&](absl::Span<const int> order) {
    std::vector<absl::btree_set<int>> live_after(static_cast<size_t>(body_len));
    absl::btree_set<int> live = live_at_end_s2;
    for (int rel = body_len - 1; rel >= 0; --rel) {
      const int oid = order[static_cast<size_t>(prologue_end + rel)];
      if (excluded_prologue_orig_ids.contains(oid)) {
        live_after[static_cast<size_t>(rel)] = live;
        continue;
      }
      live_after[static_cast<size_t>(rel)] = live;
      const Insn& insn = base_block[static_cast<size_t>(oid)];
      const auto& info = insn_info[static_cast<size_t>(oid)];
      if (insn.kind == Insn::Kind::kCall) {
        for (auto it = live.begin(); it != live.end();) {
          if (*it >= 0 && *it < kMipsFirstPseudoRegister && mm.CallUsed(*it)) {
            it = live.erase(it);
          } else {
            ++it;
          }
        }
      } else {
        for (int d : info.defs) {
          live.erase(d);
        }
      }
      for (int r : info.reads) {
        if (r >= 0 && r < kMipsFirstPseudoRegister) {
          live.insert(r);
        }
      }
    }
    return live_after;
  };
  std::vector<int> base_order(base_block.size());
  for (size_t i = 0; i < base_block.size(); ++i) {
    base_order[i] = static_cast<int>(i);
  }
  const std::vector<absl::btree_set<int>> base_live_after = compute_live_after(base_order);
  const std::vector<absl::btree_set<int>> cand_live_after = compute_live_after(cand_in_base);
  std::vector<int> orig_to_cand_rel(base_block.size(), -1);
  for (int rel = 0; rel < body_len; ++rel) {
    orig_to_cand_rel[static_cast<size_t>(cand_in_base[static_cast<size_t>(prologue_end + rel)])] =
        rel;
  }
  for (int base_rel = 0; base_rel < body_len; ++base_rel) {
    const int oid = prologue_end + base_rel;
    if (excluded_prologue_orig_ids.contains(oid)) {
      continue;
    }
    const Insn& def_insn = base_block[static_cast<size_t>(oid)];
    const RtlExpr* s = SingleSetOf(def_insn.pn.pattern);
    if (s == nullptr || s->SetDest().op != RtlOp::kReg) {
      continue;
    }
    const int hi = s->SetDest().RegNo();
    RtlMachineMode hi_mode = RtlMachineMode::kSi;
    // Exclude RMWs (`local-alloc.c:1266-1300` `combine_regs` continuations) so
    // `base_rel` is the true birth of a standalone local qty.
    if (hi < 2 || hi >= kMipsFirstPseudoRegister ||
        IsFixedOrSpecialHardReg(hi, stack_layout.frame_pointer_needed) || !mm.CallUsed(hi) ||
        IsAbiCallInterfaceHardReg(hi) || copy_tied_regs.contains(hi) ||
        insn_info[static_cast<size_t>(oid)].reads.contains(hi) ||
        !is_ordinary_local_def(def_insn, base_rel, hi, &hi_mode, base_live_after)) {
      continue;
    }
    if (hi >= kMipsFpRegFirst && (hi % 2) != 0) {
      continue;
    }
    const bool hi_needs_even = (mm.HardRegNRegs(hi, hi_mode) >= 2) || (hi >= kMipsFpRegFirst);
    if (!base_live_after[static_cast<size_t>(base_rel)].contains(hi)) {
      continue;
    }
    // Find the end of this standalone local live range of `hi` in `base_block`.
    int base_end_rel = -1;
    bool crosses_call = false;
    for (int k = base_rel + 1; k < body_len; ++k) {
      const int k_oid = prologue_end + k;
      if (base_block[static_cast<size_t>(k_oid)].kind == Insn::Kind::kCall) {
        crosses_call = true;
        break;
      }
      if (!base_live_after[static_cast<size_t>(k)].contains(hi)) {
        base_end_rel = k;
        break;
      }
    }
    if (base_end_rel <= base_rel + 1 || crosses_call) {
      continue;
    }
    // Exclude live ranges ending at an RMW of `hi` (where `combine_regs` ties
    // `hi` into a continuing qty past `base_end_rel`).
    if (insn_info[static_cast<size_t>(prologue_end + base_end_rel)].defs.contains(hi)) {
      continue;
    }
    // Collect straddling same-class `lo < hi` born strictly inside
    // `(base_rel, base_end_rel)` at an independent ordinary def that stays live
    // past `base_end_rel` (`local-alloc.c:2158-2182` `find_free_reg`).
    absl::btree_set<int> straddling_lo_regs;
    for (int k = base_rel + 1; k < base_end_rel; ++k) {
      const int k_oid = prologue_end + k;
      if (excluded_prologue_orig_ids.contains(k_oid)) {
        continue;
      }
      const Insn& lo_insn = base_block[static_cast<size_t>(k_oid)];
      const RtlExpr* ls = SingleSetOf(lo_insn.pn.pattern);
      if (ls == nullptr || ls->SetDest().op != RtlOp::kReg) {
        continue;
      }
      const int lo = ls->SetDest().RegNo();
      if (lo < 2 || lo >= hi || !same_reg_file(lo, hi) || !mm.CallUsed(lo) ||
          IsFixedOrSpecialHardReg(lo, stack_layout.frame_pointer_needed) ||
          base_sum.live_at_entry.contains(lo) || (hi_needs_even && (lo % 2) != 0) ||
          base_live_after[static_cast<size_t>(k - 1)].contains(lo) ||
          insn_info[static_cast<size_t>(k_oid)].reads.contains(lo) ||
          insn_info[static_cast<size_t>(k_oid)].reads.contains(hi) ||
          !is_ordinary_local_def(lo_insn, k, lo, nullptr, base_live_after)) {
        continue;
      }
      // Require the SAME live range of `lo` born at `k` to remain continuously
      // live past `base_end_rel` without dying or being redefined in `(k, base_end_rel]`.
      bool continuously_live = true;
      for (int j = k; j <= base_end_rel; ++j) {
        if (!base_live_after[static_cast<size_t>(j)].contains(lo) ||
            (j > k && insn_info[static_cast<size_t>(prologue_end + j)].defs.contains(lo))) {
          continuously_live = false;
          break;
        }
      }
      if (continuously_live) {
        straddling_lo_regs.insert(lo);
      }
    }
    // Symmetric leading-straddler case (`local-alloc.c:2158-2182` `find_free_reg`):
    // same-class call-used `lo < hi` already live entering `base_rel` (born before
    // `base_rel` at an ordinary non-reload local def) that dies strictly inside
    // `(base_rel, base_end_rel)` without being redefined later inside
    // `[base_rel, base_end_rel)`. Hoisting `lo`'s death ahead of `base_rel`
    // exposes `lo` as free across `hi`'s entire live range. Skip zero-input
    // `const_double` `hi` producers because forward `Schedule(kSched1)` freely
    // sinks zero-input constants next to their consumer past `lo`'s death.
    if (base_rel > 0 && s->SetSrc().op != RtlOp::kConstDouble) {
      for (int lo : base_live_after[static_cast<size_t>(base_rel - 1)]) {
        if (lo < 2 || lo >= hi || !same_reg_file(lo, hi) || !mm.CallUsed(lo) ||
            IsFixedOrSpecialHardReg(lo, stack_layout.frame_pointer_needed) ||
            base_sum.live_at_entry.contains(lo) || (hi_needs_even && (lo % 2) != 0) ||
            straddling_lo_regs.contains(lo) ||
            insn_info[static_cast<size_t>(oid)].reads.contains(lo) ||
            insn_info[static_cast<size_t>(oid)].defs.contains(lo)) {
          continue;
        }
        int lo_death = -1;
        bool redefined_in_window = false;
        for (int j = base_rel; j < base_end_rel; ++j) {
          if (insn_info[static_cast<size_t>(prologue_end + j)].defs.contains(lo)) {
            redefined_in_window = true;
            break;
          }
          if (lo_death < 0 && !base_live_after[static_cast<size_t>(j)].contains(lo)) {
            lo_death = j;
          }
        }
        if (redefined_in_window || lo_death <= base_rel) {
          continue;
        }
        int lo_birth = -1;
        bool lo_crosses_call = false;
        for (int j = base_rel - 1; j >= 0; --j) {
          const int j_oid = prologue_end + j;
          if (base_block[static_cast<size_t>(j_oid)].kind == Insn::Kind::kCall) {
            lo_crosses_call = true;
            break;
          }
          if (insn_info[static_cast<size_t>(j_oid)].defs.contains(lo)) {
            lo_birth = j;
            break;
          }
        }
        if (lo_crosses_call || lo_birth < 0) {
          continue;
        }
        const Insn& lo_birth_insn = base_block[static_cast<size_t>(prologue_end + lo_birth)];
        if (is_ordinary_local_def(lo_birth_insn, lo_birth, lo, nullptr, base_live_after)) {
          straddling_lo_regs.insert(lo);
        }
      }
    }
    if (straddling_lo_regs.empty()) {
      continue;
    }
    // `local-alloc.c:2158-2182` (`find_free_reg` ascending regno scan): dropping
    // a straddling interference with `lo < hi` only causes `find_free_reg` to
    // re-assign `hi`'s qty to `lo` when every lower allocatable call-used
    // same-class register `r < hi` was actually occupied during `hi`'s live
    // range `[base_rel, base_end_rel)` in `base_block`.
    absl::btree_set<int> base_range_occupied;
    for (int k = base_rel; k < base_end_rel; ++k) {
      base_range_occupied.insert(base_live_after[static_cast<size_t>(k)].begin(),
                                 base_live_after[static_cast<size_t>(k)].end());
      if (k > base_rel) {
        const auto& kdefs = insn_info[static_cast<size_t>(prologue_end + k)].defs;
        base_range_occupied.insert(kdefs.begin(), kdefs.end());
      }
    }
    const int class_first = (hi >= kMipsFpRegFirst) ? kMipsFpRegFirst : 2;
    const int class_step = hi_needs_even ? 2 : 1;
    bool all_lower_occupied = true;
    for (int r = class_first; r < hi; r += class_step) {
      if (!mm.CallUsed(r) || IsFixedOrSpecialHardReg(r, stack_layout.frame_pointer_needed)) {
        continue;
      }
      bool covered = base_range_occupied.contains(r);
      if (hi_needs_even && r + 1 < kMipsFirstPseudoRegister) {
        covered = covered || base_range_occupied.contains(r + 1);
      }
      if (!covered) {
        all_lower_occupied = false;
        break;
      }
    }
    if (!all_lower_occupied) {
      continue;
    }
    const int cand_rel = orig_to_cand_rel[static_cast<size_t>(oid)];
    if (cand_rel < 0 || !cand_live_after[static_cast<size_t>(cand_rel)].contains(hi)) {
      continue;
    }
    absl::btree_set<int> cand_overlaps;
    for (int k = cand_rel; k < body_len; ++k) {
      if (!cand_live_after[static_cast<size_t>(k)].contains(hi)) {
        break;
      }
      for (int lo : cand_live_after[static_cast<size_t>(k)]) {
        if (straddling_lo_regs.contains(lo)) {
          cand_overlaps.insert(lo);
        }
      }
    }
    if (cand_overlaps.size() < straddling_lo_regs.size()) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool PreservesPostReloadOrderSafety(absl::Span<const Insn> base_block,
                                    absl::Span<const int> cand_in_base, int prologue_end,
                                    int epilogue_begin, const absl::btree_set<int>& live_at_end_s2,
                                    const absl::btree_set<std::pair<int, int>>& cfg_copy_pairs,
                                    const RegFacts& facts_s2, const MachineModel& mm,
                                    bool allow_pure_reader_reg_dead_transfer,
                                    const PostReloadStackLayout& stack_layout,
                                    const absl::btree_set<int>& excluded_prologue_body_indices,
                                    bool allow_moved_store_load_equiv_invalidation) {
  const int n = static_cast<int>(base_block.size());
  if (static_cast<int>(cand_in_base.size()) != n) {
    return false;
  }
  std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
  for (int pos = 0; pos < n; ++pos) {
    pos_in_cand[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])] = pos;
  }
  for (int i = 0; i < prologue_end; ++i) {
    if (pos_in_cand[static_cast<size_t>(i)] != i) {
      return false;
    }
  }
  for (int i = epilogue_begin; i < n; ++i) {
    if (pos_in_cand[static_cast<size_t>(i)] != i) {
      return false;
    }
  }

  // 1. Preserve every semantic hard-reg / memory / call dependence edge of `base_block`
  //    (excluding asymmetric `sched.c:1704` post-call `call_used_regs[0]` artifact edges
  //    and position-dependent `sched.c:1740` `pending_lists_length > 32` flush-cap
  //    artifacts; forward `kSched2` replay verification in Gate (a) independently
  //    enforces exact `pending_lists_length > 32` scheduling equivalence).
  const DepGraph base_graph =
      AnalyzeWithoutPendingListFlushCap(base_block, facts_s2, mm, Phase::kSched2);
  for (int cons = 0; cons < n; ++cons) {
    for (const Link& l : base_graph.log_links[static_cast<size_t>(cons)]) {
      if (pos_in_cand[static_cast<size_t>(l.target)] >= pos_in_cand[static_cast<size_t>(cons)] &&
          !IsPureSched1704CallArtifactEdge(base_block, base_graph, l.target, cons, mm)) {
        return false;
      }
    }
  }

  // 2. Preserve adjacent `hi`/`lo` (`64`/`65`) output-reload and stack/mtc1 reload
  //    contiguity (`reload1.c:emit_reload_insns`).
  absl::btree_set<int> md_output_regs;
  for (int i = 0; i + 1 < n; ++i) {
    const RtlExpr& p2 = base_block[static_cast<size_t>(i + 1)].pn.pattern;
    if (IsAdjacentMdOutputReloadPair(base_block[static_cast<size_t>(i)],
                                     base_block[static_cast<size_t>(i + 1)]) &&
        pos_in_cand[static_cast<size_t>(i + 1)] != pos_in_cand[static_cast<size_t>(i)] + 1) {
      return false;
    }
    if (IsMdOutputReloadInsn(base_block[static_cast<size_t>(i + 1)])) {
      if (p2.SetDest().op == RtlOp::kReg && p2.SetDest().RegNo() >= 0 &&
          p2.SetDest().RegNo() < kMipsFirstPseudoRegister) {
        const int nr = mm.HardRegNRegs(p2.SetDest().RegNo(), p2.SetDest().mode);
        for (int k = 0; k < nr; ++k) {
          md_output_regs.insert(p2.SetDest().RegNo() + k);
        }
      }
    }
    if (IsAdjacentReloadContiguityPair(base_block[static_cast<size_t>(i)],
                                       base_block[static_cast<size_t>(i + 1)], mm, stack_layout) &&
        pos_in_cand[static_cast<size_t>(i + 1)] != pos_in_cand[static_cast<size_t>(i)] + 1) {
      return false;
    }
  }

  // 3. Preserve `REG_DEAD` reader order (`sched.c:3415-3611, 3764-3830`).
  //    Post-reload `sched2` (`reload_completed == 1`) does not recompute `REG_DEAD`
  //    notes, so a pure reader `i` that held `REG_DEAD r` in `.09.greg` can be
  //    hoisted ahead of pure reader `j` in `.10.sched2` (`i < j` in `base_block`,
  //    `pos_in_cand[i] > pos_in_cand[j]` in `cand_in_base`). Allow restoring `i`
  //    back to the last reader of `r` in `cand_in_base` when neither `i` nor `j`
  //    is a call/`USE` or writes `r`, and no intervening instruction writes `r`.
  std::vector<InsnRegReadDefDeaths> insn_info(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    insn_info[static_cast<size_t>(i)] =
        CollectInsnRegReadDefDeaths(base_block[static_cast<size_t>(i)], mm);
  }
  for (int i = 0; i < n; ++i) {
    if (insn_info[static_cast<size_t>(i)].deaths.empty()) {
      continue;
    }
    for (int j = 0; j < n; ++j) {
      if (i == j) {
        continue;
      }
      const bool flip_j_was_before =
          (j < i && pos_in_cand[static_cast<size_t>(j)] > pos_in_cand[static_cast<size_t>(i)]);
      const bool flip_i_was_before =
          (i < j && pos_in_cand[static_cast<size_t>(i)] > pos_in_cand[static_cast<size_t>(j)]);
      if (!flip_j_was_before && !flip_i_was_before) {
        continue;
      }
      for (int r : insn_info[static_cast<size_t>(i)].deaths) {
        if (!insn_info[static_cast<size_t>(j)].reads.contains(r)) {
          continue;
        }
        if (insn_info[static_cast<size_t>(i)].is_call_or_use ||
            insn_info[static_cast<size_t>(j)].is_call_or_use ||
            insn_info[static_cast<size_t>(i)].defs.contains(r) ||
            insn_info[static_cast<size_t>(j)].defs.contains(r)) {
          return false;
        }
        const int base_lo = std::min(i, j);
        const int base_hi = std::max(i, j);
        for (int k = base_lo + 1; k < base_hi; ++k) {
          if (insn_info[static_cast<size_t>(k)].defs.contains(r)) {
            return false;
          }
        }
        const int cand_lo =
            std::min(pos_in_cand[static_cast<size_t>(i)], pos_in_cand[static_cast<size_t>(j)]);
        const int cand_hi =
            std::max(pos_in_cand[static_cast<size_t>(i)], pos_in_cand[static_cast<size_t>(j)]);
        for (int p = cand_lo + 1; p < cand_hi; ++p) {
          if (insn_info[static_cast<size_t>(cand_in_base[static_cast<size_t>(p)])].defs.contains(
                  r)) {
            return false;
          }
        }
        if (flip_j_was_before) {
          if (!allow_pure_reader_reg_dead_transfer &&
              !IsCrossClassFprToGprCopyReloadInheritedRead(
                  base_block, cand_in_base, pos_in_cand[static_cast<size_t>(i)],
                  pos_in_cand[static_cast<size_t>(j)], r, mm)) {
            return false;
          }
        } else if (!allow_pure_reader_reg_dead_transfer) {
          for (int p = pos_in_cand[static_cast<size_t>(i)] + 1; p < n; ++p) {
            const int m = cand_in_base[static_cast<size_t>(p)];
            if (insn_info[static_cast<size_t>(m)].defs.contains(r)) {
              break;
            }
            if (insn_info[static_cast<size_t>(m)].reads.contains(r)) {
              return false;
            }
          }
        }
      }
    }
  }

  // 4. Preserve hard-register live-at-entry, call crossings (`caller-save.c`), exact
  //    `local-alloc.c:540-603, 990-1055` (`validate_equiv_mem`) status, and monotonic
  //    copy-tie / `hi`-`lo` allocation-order interferences (`global.c:962-985`,
  //    `local-alloc.c:1399, 2158-2181`).
  std::vector<int> base_ids(static_cast<size_t>(n));
  std::vector<Insn> cand_seq;
  cand_seq.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    base_ids[static_cast<size_t>(i)] = i;
    cand_seq.push_back(base_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(i)])]);
  }
  absl::btree_set<int> excluded_prologue_orig_ids;
  for (int i = 0; i < prologue_end; ++i) {
    excluded_prologue_orig_ids.insert(i);
  }
  for (int rel : excluded_prologue_body_indices) {
    excluded_prologue_orig_ids.insert(prologue_end + rel);
  }
  const HardRegLivenessSummary base_sum = ComputeHardRegLivenessSummary(
      base_block, base_ids, live_at_end_s2, facts_s2, mm, stack_layout, excluded_prologue_orig_ids);
  const HardRegLivenessSummary cand_sum =
      ComputeHardRegLivenessSummary(cand_seq, cand_in_base, live_at_end_s2, facts_s2, mm,
                                    stack_layout, excluded_prologue_orig_ids);
  if (base_sum.live_at_entry != cand_sum.live_at_entry) {
    return false;
  }
  if (base_sum.call_crossing_regs != cand_sum.call_crossing_regs) {
    return false;
  }
  // When `allow_pure_reader_reg_dead_transfer` is true (Stage 0 discovery probe),
  // defer `mem_equiv_valid` verification until after the pure-reader `REG_DEAD`
  // transfers are applied (where `PreservesPostReloadOrderSafety(..., false)`
  // enforces strict `mem_equiv_valid` consistency on the transferred notes).
  if (!allow_pure_reader_reg_dead_transfer &&
      base_sum.mem_equiv_valid != cand_sum.mem_equiv_valid) {
    for (const auto& [oid, bv] : base_sum.mem_equiv_valid) {
      const auto cit = cand_sum.mem_equiv_valid.find(oid);
      const bool cv = (cit != cand_sum.mem_equiv_valid.end()) && cit->second;
      if (bv == cv) {
        continue;
      }
      // `local-alloc.c:990-1000, 1049-1055`: if a `MEM` load (or the initializing
      // insn of a dying `MEM` store) already carries a matching `REG_EQUIV` note
      // from pre-`sched2` `update_equiv_regs`, transitioning `validate_equiv_mem`
      // from `false` (broken by `sched2` reordering) back to `true` restores the
      // exact pre-`sched2` equivalence condition.
      if (!bv && cv) {
        const Insn& base_insn = base_block[static_cast<size_t>(oid)];
        const RtlExpr& pat = base_insn.pn.pattern;
        if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
          if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kMem &&
              HasMatchingMemRegEquivNote(base_insn, pat.SetSrc(), stack_layout)) {
            continue;
          }
          if (pat.SetDest().op == RtlOp::kMem && pat.SetSrc().op == RtlOp::kReg &&
              !pat.SetSrc().IsArgPointerReg()) {
            const int store_pos = pos_in_cand[static_cast<size_t>(oid)];
            const int init_pos =
                FindUniqueInitPosForStoreInSeq(cand_seq, store_pos, pat.SetSrc().RegNo(), mm);
            if (init_pos >= 0 && HasMatchingMemRegEquivNote(cand_seq[static_cast<size_t>(init_pos)],
                                                            pat.SetDest(), stack_layout)) {
              continue;
            }
          }
        }
      }
      // Symmetric `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`) case:
      // when `sched2` moved a non-dying store `oid` `(set (mem M) (reg r))` AFTER
      // `r`'s true death instruction `d < oid`, `validate_equiv_mem` on `base_block`
      // stopped scanning at `d` and spuriously reported `bv = true`. Moving `oid`
      // back ahead of `d` (`pos_in_cand[oid] < pos_in_cand[d]`) restores `cv = false`,
      // matching the absence of a `REG_EQUIV (mem M)` note on `r`'s initializer.
      if (bv && !cv) {
        const Insn& base_insn = base_block[static_cast<size_t>(oid)];
        const RtlExpr& pat = base_insn.pn.pattern;
        if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kMem &&
            pat.SetSrc().op == RtlOp::kReg && !pat.SetSrc().IsArgPointerReg()) {
          const int r = pat.SetSrc().RegNo();
          const int store_pos = pos_in_cand[static_cast<size_t>(oid)];
          const int init_pos = FindUniqueInitPosForStoreInSeq(cand_seq, store_pos, r, mm);
          if (init_pos >= 0 && !HasMatchingMemRegEquivNote(cand_seq[static_cast<size_t>(init_pos)],
                                                           pat.SetDest(), stack_layout)) {
            if (!HasRegDeadNoteCovering(base_insn.pn.reg_notes, r, mm)) {
              bool moved_ahead_of_death = false;
              for (int d = oid - 1; d >= prologue_end; --d) {
                if (insn_info[static_cast<size_t>(d)].deaths.contains(r) &&
                    pos_in_cand[static_cast<size_t>(d)] > store_pos) {
                  moved_ahead_of_death = true;
                  break;
                }
                if (insn_info[static_cast<size_t>(d)].defs.contains(r)) {
                  break;
                }
              }
              if (moved_ahead_of_death) {
                continue;
              }
            }
            // Symmetric `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`,
            // `memref_used_between_p`, `update_equiv_regs`) store case: when
            // `sched2` moved an address-base write/death, call, or overlapping
            // `MEM` access out of `[init, store)`, `base_block` spuriously saw
            // `bv = true`. Moving it back into `[init_pos, store_pos)` restores
            // `cv = false`, matching the absence of `REG_EQUIV` on `init_pos`.
            if (HasStoreEquivInvalidatorMovedIntoWindow(oid, init_pos, pat.SetDest(), prologue_end,
                                                        epilogue_begin, base_block, cand_in_base,
                                                        pos_in_cand, insn_info, facts_s2, mm,
                                                        stack_layout, excluded_prologue_orig_ids)) {
              continue;
            }
          }
        }
        // Symmetric `local-alloc.c:540-603, 1049-1055` (`validate_equiv_mem`,
        // `update_equiv_regs`) load case: when `sched2` hoisted a `(REG_DEAD base_r)`
        // insn `d` ahead of `oid` or hoisted `REG_DEAD dst_r` ahead of an invalidator
        // `d` (base-reg write/death, non-const call, or aliasing `MEM` store),
        // `validate_equiv_mem` on `base_block` missed `d` and spuriously reported
        // `bv = true`. Restoring `d` into `[load_pos, cand_dst_death_pos)` restores
        // `cv = false`, matching the absence of a `REG_EQUIV (mem ...)` note on `oid`.
        if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg &&
            pat.SetSrc().op == RtlOp::kMem &&
            !HasMatchingMemRegEquivNote(base_insn, pat.SetSrc(), stack_layout) &&
            HasLoadEquivInvalidatorMovedIntoWindow(
                oid, pat.SetSrc(), pat.SetDest().RegNo(), prologue_end, epilogue_begin, base_block,
                cand_in_base, pos_in_cand, insn_info, facts_s2, mm, stack_layout,
                excluded_prologue_orig_ids, allow_moved_store_load_equiv_invalidation)) {
          continue;
        }
      }
      return false;
    }
    for (const auto& [oid, cv] : cand_sum.mem_equiv_valid) {
      if (!base_sum.mem_equiv_valid.contains(oid) && cv) {
        return false;
      }
    }
  }
  // Adding an interference between distinct hard registers `a != b` preserves coloring
  // validity; only dropping an interference `(a, b)` can trigger copy-tie coalescing
  // (`global.c:find_reg` / `local-alloc.c:1399`) or lower-numbered `REG_ALLOC_ORDER`
  // selection (`reload1.c:3619-3663` `order_regs_for_reload`) on `hi`/`lo` output reloads.
  absl::btree_set<int> non_md_body_defined_regs;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    if (excluded_prologue_orig_ids.contains(i)) {
      continue;
    }
    const Insn& insn = base_block[static_cast<size_t>(i)];
    if (IsMdOutputReloadInsn(insn) || insn.kind == Insn::Kind::kCall) {
      continue;
    }
    for (int d : insn_info[static_cast<size_t>(i)].defs) {
      if (d > 0 && d < kMipsFirstPseudoRegister) {
        non_md_body_defined_regs.insert(d);
      }
    }
  }
  auto same_reg_file = [](int a, int b) {
    return (a < kMipsFpRegFirst && b < kMipsFpRegFirst) ||
           (a >= kMipsFpRegFirst && a <= kMipsFpRegLast && b >= kMipsFpRegFirst &&
            b <= kMipsFpRegLast);
  };
  std::optional<HardRegLivenessSummary> base_sum_without_reload_spill_stores;
  auto get_base_interferences_without_reload_spill_stores =
      [&]() -> const absl::btree_set<std::pair<int, int>>& {
    if (!base_sum_without_reload_spill_stores.has_value()) {
      absl::btree_set<int> excluded_with_spills = excluded_prologue_orig_ids;
      absl::btree_set<int> body_defined_so_far;
      for (int i = prologue_end; i < epilogue_begin; ++i) {
        if (excluded_prologue_orig_ids.contains(i)) {
          continue;
        }
        // `reload1.c:emit_reload_insns` emits output reload spill stores
        // `(set (mem [sp+off]) (reg r))` immediately after the defining insn of `r`
        // with `REG_NOTES = NULL_RTX` (no `REG_DEAD r`). When post-reload `sched2`
        // defers such a spill store past a reload-inherited copy `(set (reg d) (reg r))`
        // (`reload1.c:5186-5257`), it creates a spurious post-`sched2` interference
        // `(r, d)` that never existed in `.09.greg` / `.08.lreg`.
        if (auto acc = TryParsePlainStackSlotMove(base_block[static_cast<size_t>(i)],
                                                  stack_layout.frame_pointer_needed);
            acc.has_value() && acc->is_store &&
            stack_layout.stack_slot_load_offsets.contains(acc->key.offset) &&
            !stack_layout.caller_save_slot_offsets.contains(acc->key.offset) &&
            !HasRegDeadNoteCovering(base_block[static_cast<size_t>(i)].pn.reg_notes, acc->regno,
                                    mm) &&
            body_defined_so_far.contains(acc->regno)) {
          excluded_with_spills.insert(i);
        }
        for (int d : insn_info[static_cast<size_t>(i)].defs) {
          if (d >= 0 && d < kMipsFirstPseudoRegister) {
            body_defined_so_far.insert(d);
          }
        }
      }
      base_sum_without_reload_spill_stores = ComputeHardRegLivenessSummary(
          base_block, base_ids, live_at_end_s2, facts_s2, mm, stack_layout, excluded_with_spills);
    }
    return base_sum_without_reload_spill_stores->interferences;
  };
  auto reg_crosses_call = [&](int r) {
    if (cand_sum.call_crossing_regs.contains(r)) {
      return true;
    }
    return stack_layout.cfg_call_crossing_regs.contains(r) &&
           (base_sum.live_at_entry.contains(r) || live_at_end_s2.contains(r));
  };
  // `reload1.c:5599-5665` (`emit_reload_insns`): an earlyclobber `DImode` GPR
  // output-reload copy `(set (reg:DI orig) (reg:DI reload))` reunited adjacent
  // to its owning `PARALLEL` is folded away by `InvertReload` before
  // `local-alloc`/`global-alloc` ever runs, so a spurious post-`sched2`
  // interference between `orig` and the pure reload scratch `reload` never
  // causes copy-tie coalescing.
  absl::btree_set<std::pair<int, int>> earlyclobber_reload_copy_pairs;
  for (int pos = prologue_end + 1; pos < epilogue_begin; ++pos) {
    int orig_hr = -1;
    int reload_hr = -1;
    if (!TryMatchEarlyclobberDiOutputReload(cand_seq[static_cast<size_t>(pos - 1)],
                                            cand_seq[static_cast<size_t>(pos)], mm, &orig_hr,
                                            &reload_hr) ||
        !IsRegDeadAfterInsn(cand_seq, pos, reload_hr, RtlMachineMode::kDi, mm, live_at_end_s2) ||
        base_sum.live_at_entry.contains(reload_hr) ||
        base_sum.live_at_entry.contains(reload_hr + 1)) {
      continue;
    }
    bool pure_reload_scratch = true;
    for (int k = prologue_end; k < epilogue_begin; ++k) {
      if (k == pos - 1 || k == pos) {
        continue;
      }
      const auto& ki = insn_info[static_cast<size_t>(cand_in_base[static_cast<size_t>(k)])];
      if (ki.reads.contains(reload_hr) || ki.reads.contains(reload_hr + 1) ||
          ki.defs.contains(reload_hr) || ki.defs.contains(reload_hr + 1)) {
        pure_reload_scratch = false;
        break;
      }
    }
    if (pure_reload_scratch) {
      for (int w1 = 0; w1 < 2; ++w1) {
        for (int w2 = 0; w2 < 2; ++w2) {
          const int a = orig_hr + w1;
          const int b = reload_hr + w2;
          earlyclobber_reload_copy_pairs.insert({std::min(a, b), std::max(a, b)});
        }
      }
    }
  }
  for (const std::pair<int, int>& edge : base_sum.interferences) {
    if (cand_sum.interferences.contains(edge)) {
      continue;
    }
    // `local-alloc.c:2106` / `global.c:927` (`COPY_HARD_REG_SET (..., call_used_reg_set)`):
    // a call-crossing register excludes `call_used_reg_set`, so dropping an
    // interference between a call-used register and a call-crossing register
    // can never coalesce them via copy-tie preference (`global.c:find_reg`).
    const bool call_crossing_vs_call_used =
        (mm.CallUsed(edge.first) && reg_crosses_call(edge.second)) ||
        (mm.CallUsed(edge.second) && reg_crosses_call(edge.first));
    if (cfg_copy_pairs.contains(edge) && same_reg_file(edge.first, edge.second) &&
        !call_crossing_vs_call_used) {
      if (earlyclobber_reload_copy_pairs.contains(edge) ||
          !get_base_interferences_without_reload_spill_stores().contains(edge)) {
        continue;
      }
      return false;
    }
    const int lo = edge.first;
    const int hi = edge.second;
    // `reload1.c:3619-3663` (`order_regs_for_reload`): an unused hard reg `hi`
    // that appears solely as a reload-inserted `mfhi`/`mflo` output register
    // (`hard_reg_n_uses[hi] == 0`) is strictly preferred ahead of any register
    // `lo` already live or defined in the body (`hard_reg_n_uses[lo] > 0`).
    const bool hi_is_pure_md_reload =
        md_output_regs.contains(hi) && !non_md_body_defined_regs.contains(hi) &&
        !base_sum.live_at_entry.contains(hi) && !live_at_end_s2.contains(hi);
    const bool lo_is_used_reg = non_md_body_defined_regs.contains(lo) ||
                                base_sum.live_at_entry.contains(lo) || live_at_end_s2.contains(lo);
    const bool md_blocked =
        md_output_regs.contains(hi) && !(hi_is_pure_md_reload && lo_is_used_reg);
    // `local-alloc.c:2106` / `global.c:927` (`COPY_HARD_REG_SET (..., call_used_reg_set)`):
    // a call-crossing register `hi` excludes `call_used_reg_set`, so dropping an
    // interference with a call-used register `lo` cannot change `hi`'s allocation.
    if (md_blocked && !(mm.CallUsed(lo) && reg_crosses_call(hi)) && same_reg_file(lo, hi)) {
      return false;
    }
  }
  return !DropsPerLiveRangeLocalFindFreeRegInterference(
      base_block, cand_in_base, prologue_end, epilogue_begin, excluded_prologue_orig_ids,
      live_at_end_s2, base_sum, cfg_copy_pairs, insn_info, stack_layout, mm);
}

std::vector<int> CollectDisplacedBlock0EntryArgCopyBodyIndices(
    absl::Span<const Insn> post_reload_body, const Sched1ProjectionView& view,
    const absl::btree_set<int>& excluded_prologue_body_indices, const MachineModel& mm,
    bool frame_pointer_needed) {
  std::vector<int> displaced;
  const int body_len = static_cast<int>(post_reload_body.size());
  if (body_len <= 1 || view.core_insns.empty()) {
    return displaced;
  }

  // Find the first call/jump boundary in `post_reload_body`: entry-parameter copies
  // (`function.c:3668-3742, 5129-5144`, `sched.c:3191-3215`) cannot cross a call/jump.
  int limit = body_len;
  int first_call_idx = -1;
  for (int i = 0; i < body_len; ++i) {
    if (post_reload_body[static_cast<size_t>(i)].kind == Insn::Kind::kCall) {
      first_call_idx = i;
      limit = i;
      break;
    }
    if (post_reload_body[static_cast<size_t>(i)].kind == Insn::Kind::kJump) {
      limit = i;
      break;
    }
  }
  if (limit <= 1) {
    return displaced;
  }

  // Identify hard regs directly feeding the first call's `call_fusage` so we never
  // mistake an outgoing call-argument move `(set a0 a1)` for an entry-param copy.
  std::vector<absl::btree_set<int>> call_arg_defs(static_cast<size_t>(limit));
  if (first_call_idx >= 0) {
    absl::btree_set<int> needed_args;
    for (const RtlExpr& fu : post_reload_body[static_cast<size_t>(first_call_idx)].pn.call_fusage) {
      CollectRegReads(fu, &needed_args);
    }
    for (int prev = first_call_idx - 1; prev >= 0 && !needed_args.empty(); --prev) {
      if (excluded_prologue_body_indices.contains(prev)) {
        continue;
      }
      const InsnRegReadDefDeaths info =
          CollectInsnRegReadDefDeaths(post_reload_body[static_cast<size_t>(prev)], mm);
      for (int d : info.defs) {
        if (needed_args.contains(d)) {
          call_arg_defs[static_cast<size_t>(prev)].insert(d);
          needed_args.erase(d);
        }
      }
    }
  }

  absl::btree_map<int, int> orig_to_core_idx;
  for (int ci = 0; ci < static_cast<int>(view.core_to_orig_index.size()); ++ci) {
    orig_to_core_idx[view.core_to_orig_index[static_cast<size_t>(ci)]] = ci;
  }
  absl::btree_set<int> pinned_set(view.pinned_prefix_orig_indices.begin(),
                                  view.pinned_prefix_orig_indices.end());
  absl::btree_set<int> written_hard_regs;
  absl::btree_set<int> word_copied_args;

  auto is_incoming_arg_reg = [](int r) {
    return (r >= kMipsArgRegFirst && r <= kMipsArgRegLast) ||
           (r >= kMipsFpArgRegFirst && r <= kMipsFpArgRegFirst + 3);
  };

  for (int rel = 0; rel < limit; ++rel) {
    if (excluded_prologue_body_indices.contains(rel)) {
      continue;
    }
    const Insn& insn = post_reload_body[static_cast<size_t>(rel)];
    const RtlExpr& pat = insn.pn.pattern;

    if (pinned_set.contains(rel)) {
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg &&
          pat.SetSrc().op == RtlOp::kReg) {
        const RtlExpr& s = pat.SetSrc();
        if ((s.mode == RtlMachineMode::kSi || s.mode == RtlMachineMode::kDi) &&
            s.RegNo() >= kMipsArgRegFirst && s.RegNo() <= kMipsArgRegLast) {
          word_copied_args.insert(s.RegNo());
        }
      }
      const InsnRegReadDefDeaths info = CollectInsnRegReadDefDeaths(insn, mm);
      written_hard_regs.insert(info.defs.begin(), info.defs.end());
      continue;
    }

    // Include `/i` (`flags.integrated`) return-reg entry-parameter copies (e.g.,
    // sret hidden pointer `(set (reg/i:SI 2 v0) (reg:SI 4 a0))` colored directly
    // to `$v0` in `local-alloc`, `function.c:assign_parms, 5129-5144`,
    // `sched.c:3191-3215`) provided `dr` has not already been written earlier,
    // and block-0 direct spilled entry-parameter copies lowered to
    // `(set (mem:M [sp/fp+off]) (reg:M arg))` (`reload1.c:2309-2400`) whose
    // projected `sched1` view destination is a pseudo register.
    bool matched_displaced = false;
    const auto core_it = orig_to_core_idx.find(rel);
    const bool is_direct_spilled_entry_copy =
        core_it != orig_to_core_idx.end() && insn.kind == Insn::Kind::kInsn &&
        pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kMem &&
        pat.SetSrc().op == RtlOp::kReg &&
        view.core_insns[static_cast<size_t>(core_it->second)].pn.pattern.op == RtlOp::kSet &&
        view.core_insns[static_cast<size_t>(core_it->second)].pn.pattern.children.size() == 2 &&
        view.core_insns[static_cast<size_t>(core_it->second)].pn.pattern.SetDest().op ==
            RtlOp::kReg &&
        view.core_insns[static_cast<size_t>(core_it->second)].pn.pattern.SetDest().RegNo() >=
            kMipsFirstPseudoRegister;
    if (is_direct_spilled_entry_copy) {
      const RtlExpr& src_r = pat.SetSrc();
      const int sr = src_r.RegNo();
      if (is_incoming_arg_reg(sr)) {
        const int snr = mm.HardRegNRegs(sr, src_r.mode);
        bool src_clobbered = false;
        for (int k = 0; k < snr; ++k) {
          if (written_hard_regs.contains(sr + k)) {
            src_clobbered = true;
            break;
          }
        }
        bool narrow_unpaired = false;
        if ((pat.SetDest().mode == RtlMachineMode::kQi ||
             pat.SetDest().mode == RtlMachineMode::kHi) &&
            (src_r.mode == RtlMachineMode::kQi || src_r.mode == RtlMachineMode::kHi) &&
            sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast && !word_copied_args.contains(sr)) {
          narrow_unpaired = true;
        }
        if (!src_clobbered && !narrow_unpaired) {
          matched_displaced = true;
          displaced.push_back(rel);
          if ((src_r.mode == RtlMachineMode::kSi || src_r.mode == RtlMachineMode::kDi) &&
              sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast) {
            word_copied_args.insert(sr);
          }
        }
      }
    } else if (core_it != orig_to_core_idx.end() && insn.kind == Insn::Kind::kInsn &&
               pat.op == RtlOp::kSet && pat.children.size() == 2 &&
               pat.SetDest().op == RtlOp::kReg && !pat.SetDest().IsArgPointerReg() &&
               !pat.SetDest().flags.frame_related &&
               (!pat.SetDest().flags.integrated ||
                !written_hard_regs.contains(pat.SetDest().RegNo()))) {
      const RtlExpr& dst = pat.SetDest();
      const int dr = dst.RegNo();
      const bool non_fixed_dst = dr > 0 && dr < kMipsFirstPseudoRegister &&
                                 !IsFixedOrSpecialHardReg(dr, frame_pointer_needed);
      const RtlExpr* inner_src = &pat.SetSrc();
      while ((inner_src->op == RtlOp::kSubreg || inner_src->op == RtlOp::kSignExtend ||
              inner_src->op == RtlOp::kZeroExtend || inner_src->op == RtlOp::kSignExtract ||
              inner_src->op == RtlOp::kZeroExtract) &&
             !inner_src->children.empty()) {
        inner_src = &inner_src->children[0];
      }
      if (non_fixed_dst && inner_src->op == RtlOp::kReg && !inner_src->IsArgPointerReg() &&
          is_incoming_arg_reg(inner_src->RegNo()) && inner_src->RegNo() != dr &&
          !call_arg_defs[static_cast<size_t>(rel)].contains(dr)) {
        const int sr = inner_src->RegNo();
        const int snr = mm.HardRegNRegs(sr, inner_src->mode);
        bool src_clobbered = false;
        for (int k = 0; k < snr; ++k) {
          if (written_hard_regs.contains(sr + k)) {
            src_clobbered = true;
            break;
          }
        }
        // Mirror `sched.c:3197-3214` + `ParseBlockForSchedModel:5654-5667`: do not
        // treat an un-word-copied narrow `QI`/`HI` copy from `$a0-$a3` as pinned.
        bool narrow_unpaired = false;
        if (pat.SetSrc().op == RtlOp::kReg &&
            (dst.mode == RtlMachineMode::kQi || dst.mode == RtlMachineMode::kHi) &&
            (pat.SetSrc().mode == RtlMachineMode::kQi ||
             pat.SetSrc().mode == RtlMachineMode::kHi) &&
            sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast && !word_copied_args.contains(sr)) {
          narrow_unpaired = true;
        }
        // Do not hoist a caller-saved scratch `mtc1` input reload (`reload1.c`)
        // that sits contiguously before its FP consumer; callee-saved FPRs
        // (`$f20-$f30`) are `assign_parms` copies, never scratch input reloads.
        bool is_scratch_mtc1_reload = false;
        if (dr >= kMipsFpRegFirst && dr < kMipsCalleeSavedFprFirst && sr < kMipsFpRegFirst &&
            rel + 1 < body_len &&
            IsAdjacentReloadContiguityPair(insn, post_reload_body[static_cast<size_t>(rel + 1)],
                                           mm)) {
          is_scratch_mtc1_reload = true;
        }
        if (!src_clobbered && !narrow_unpaired && !is_scratch_mtc1_reload) {
          matched_displaced = true;
          displaced.push_back(rel);
          if (pat.SetSrc().op == RtlOp::kReg &&
              (pat.SetSrc().mode == RtlMachineMode::kSi ||
               pat.SetSrc().mode == RtlMachineMode::kDi) &&
              sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast) {
            word_copied_args.insert(sr);
          }
        }
      }
    }
    const InsnRegReadDefDeaths info = CollectInsnRegReadDefDeaths(insn, mm);
    written_hard_regs.insert(info.defs.begin(), info.defs.end());
    (void)matched_displaced;
  }
  return displaced;
}

std::optional<std::vector<int>> BuildInvertedRegDeadSeedOrderInBase(
    absl::Span<const Insn> base_block, int prologue_end, int epilogue_begin,
    const RegFacts& facts_s2, const MachineModel& mm, InvertedRegDeadSeedMode mode) {
  const int n = static_cast<int>(base_block.size());
  if (epilogue_begin - prologue_end <= 1) {
    return std::nullopt;
  }
  std::vector<int> seed_in_base(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    seed_in_base[static_cast<size_t>(i)] = i;
  }
  const std::vector<int> identity_order = seed_in_base;
  const DepGraph base_graph =
      AnalyzeWithoutPendingListFlushCap(base_block, facts_s2, mm, Phase::kSched2);
  bool any_inverted = false;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    const Insn& i_insn = base_block[static_cast<size_t>(i)];
    if (i_insn.kind == Insn::Kind::kCall || i_insn.pn.pattern.op == RtlOp::kUse) {
      continue;
    }
    for (const RtlRegNote& note : i_insn.pn.reg_notes) {
      if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
        continue;
      }
      const int r = note.datum.RegNo();
      if (r <= 0 || r >= kMipsFirstPseudoRegister) {
        continue;
      }
      const int nr = std::max(1, mm.HardRegNRegs(r, note.datum.mode));
      if (InsnDefinesHardRegSpan(i_insn, r, nr, mm)) {
        continue;
      }
      std::vector<int> later_readers;
      for (int j = i + 1; j < epilogue_begin; ++j) {
        const Insn& j_insn = base_block[static_cast<size_t>(j)];
        if (InsnDefinesHardRegSpan(j_insn, r, nr, mm)) {
          break;
        }
        if (j_insn.kind != Insn::Kind::kCall && j_insn.pn.pattern.op != RtlOp::kUse &&
            InsnReadsHardRegSpan(j_insn, r, nr, mm)) {
          if (IsCrossClassFprToGprCopyReloadInheritedRead(base_block, identity_order, i, j, r,
                                                          mm)) {
            continue;
          }
          later_readers.push_back(j);
          // Per `local-alloc.c:580-599` (`validate_equiv_mem`), if `j` is a
          // `MEM` load carrying `(REG_EQUIV (mem ... r ...))`, `i`'s
          // `(REG_DEAD r)` cannot lie inside `[j, death_of_dst)` without
          // invalidating the `REG_EQUIV` note; include `dst`'s death insn so
          // `kPullBeforeDead` / `kDeferAfterReaders` keeps `[j, death_of_dst]`
          // ahead of `i`.
          const RtlExpr* js = SingleSetOf(j_insn.pn.pattern);
          if (js != nullptr && js->SetDest().op == RtlOp::kReg &&
              !js->SetDest().IsArgPointerReg() && js->SetSrc().op == RtlOp::kMem) {
            bool has_equiv_on_r = false;
            for (const RtlRegNote& jn : j_insn.pn.reg_notes) {
              if (jn.kind == RtlMachineMode::kRegEquiv && jn.datum.op == RtlOp::kMem) {
                for (int w = 0; w < nr; ++w) {
                  if (jn.datum.ContainsReg(r + w)) {
                    has_equiv_on_r = true;
                    break;
                  }
                }
              }
              if (has_equiv_on_r) {
                break;
              }
            }
            if (has_equiv_on_r) {
              const int dst_r = js->SetDest().RegNo();
              const int dst_nr = std::max(1, mm.HardRegNRegs(dst_r, js->SetDest().mode));
              for (int d = j + 1; d < epilogue_begin; ++d) {
                const Insn& d_insn = base_block[static_cast<size_t>(d)];
                if (InsnDefinesHardRegSpan(d_insn, r, nr, mm)) {
                  break;
                }
                bool kills_dst = InsnDefinesHardRegSpan(d_insn, dst_r, dst_nr, mm);
                for (int w = 0; w < dst_nr && !kills_dst; ++w) {
                  if (HasRegDeadNoteCovering(d_insn.pn.reg_notes, dst_r + w, mm)) {
                    kills_dst = true;
                  }
                }
                if (kills_dst) {
                  if (d_insn.kind != Insn::Kind::kCall && d_insn.pn.pattern.op != RtlOp::kUse) {
                    later_readers.push_back(d);
                    // Keep any `reload1.c:5641-6155` (`emit_reload_insns`)
                    // adjacent output-reload / spill-store insns attached to
                    // `d` atomic with `d` so neither `kPullBeforeDead` nor
                    // `kDeferAfterReaders` splits a `reload_adjacent` pair.
                    int adj = d;
                    while (
                        adj + 1 < epilogue_begin &&
                        (IsAdjacentMdOutputReloadPair(base_block[static_cast<size_t>(adj)],
                                                      base_block[static_cast<size_t>(adj + 1)]) ||
                         IsAdjacentReloadContiguityPair(base_block[static_cast<size_t>(adj)],
                                                        base_block[static_cast<size_t>(adj + 1)],
                                                        mm))) {
                      ++adj;
                      later_readers.push_back(adj);
                    }
                  }
                  break;
                }
              }
            }
          }
        }
      }
      if (mode == InvertedRegDeadSeedMode::kPullBeforeDead && !later_readers.empty()) {
        auto it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
        if (it_i != seed_in_base.end()) {
          const absl::btree_set<int> later_set(later_readers.begin(), later_readers.end());
          absl::btree_set<int> after_i_set;
          std::vector<int> stack;
          absl::btree_set<int> to_pull;
          for (auto it = it_i + 1; it != seed_in_base.end(); ++it) {
            after_i_set.insert(*it);
            if (later_set.contains(*it) && to_pull.insert(*it).second) {
              stack.push_back(*it);
            }
          }
          // Include transitive in-window `kSched2` ancestors of `later_readers`
          // positioned after `i` so pulling them ahead of `i` preserves
          // `base_graph` topological order (e.g., anti-dep ancestors reusing
          // the destination reg of a later reader).
          bool depends_on_i = false;
          while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            if (cur + 1 < epilogue_begin && after_i_set.contains(cur + 1) &&
                (IsAdjacentMdOutputReloadPair(base_block[static_cast<size_t>(cur)],
                                              base_block[static_cast<size_t>(cur + 1)]) ||
                 IsAdjacentReloadContiguityPair(base_block[static_cast<size_t>(cur)],
                                                base_block[static_cast<size_t>(cur + 1)], mm)) &&
                to_pull.insert(cur + 1).second) {
              stack.push_back(cur + 1);
            }
            for (const Link& l : base_graph.log_links[static_cast<size_t>(cur)]) {
              if (l.target == i) {
                depends_on_i = true;
                break;
              }
              if (after_i_set.contains(l.target) && to_pull.insert(l.target).second) {
                stack.push_back(l.target);
              }
            }
            if (depends_on_i) {
              break;
            }
          }
          if (!depends_on_i && !to_pull.empty()) {
            std::vector<int> trailing;
            trailing.reserve(to_pull.size());
            for (auto it = it_i + 1; it != seed_in_base.end(); ++it) {
              if (to_pull.contains(*it)) {
                trailing.push_back(*it);
              }
            }
            seed_in_base.erase(std::remove_if(seed_in_base.begin(), seed_in_base.end(),
                                              [&](int node) { return to_pull.contains(node); }),
                               seed_in_base.end());
            it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
            seed_in_base.insert(it_i, trailing.begin(), trailing.end());
            any_inverted = true;
            continue;
          }
        }
      }
      if (mode == InvertedRegDeadSeedMode::kDeferAfterReaders && !later_readers.empty()) {
        auto it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
        auto it_last = it_i;
        for (int j : later_readers) {
          auto it_j = std::find(seed_in_base.begin(), seed_in_base.end(), j);
          if (it_j != seed_in_base.end() && it_j > it_last) {
            it_last = it_j;
          }
        }
        if (it_i != seed_in_base.end() && it_last > it_i) {
          // Prefer deferring `i` together with its in-window forward `base_graph`
          // descendants immediately after the last later reader whenever no
          // later reader transitively depends on `i` (exact inverse of `sched2`
          // hoisting `i` early).
          const absl::btree_set<int> later_set(later_readers.begin(), later_readers.end());
          absl::btree_set<int> fwd_closure = {i};
          bool later_depends_on_i = false;
          for (auto it = it_i + 1; it <= it_last; ++it) {
            bool dep = false;
            for (const Link& l : base_graph.log_links[static_cast<size_t>(*it)]) {
              if (fwd_closure.contains(l.target)) {
                dep = true;
                break;
              }
            }
            if (dep) {
              if (later_set.contains(*it)) {
                later_depends_on_i = true;
                break;
              }
              fwd_closure.insert(*it);
            }
          }
          if (!later_depends_on_i) {
            const int last_node = *it_last;
            std::vector<int> deferred;
            deferred.reserve(fwd_closure.size());
            for (auto it = it_i; it <= it_last; ++it) {
              if (fwd_closure.contains(*it)) {
                deferred.push_back(*it);
              }
            }
            seed_in_base.erase(std::remove_if(seed_in_base.begin(), seed_in_base.end(),
                                              [&](int node) { return fwd_closure.contains(node); }),
                               seed_in_base.end());
            auto ins_it = std::find(seed_in_base.begin(), seed_in_base.end(), last_node);
            seed_in_base.insert(ins_it + 1, deferred.begin(), deferred.end());
            any_inverted = true;
            continue;
          }
        }
      }
      for (int j : later_readers) {
        auto it_j = std::find(seed_in_base.begin(), seed_in_base.end(), j);
        auto it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
        if (it_j == seed_in_base.end() || it_i == seed_in_base.end() || it_j <= it_i) {
          continue;
        }
        // Collect `j` together with all transitive `kSched2` ancestors currently
        // positioned after `i` in `seed_in_base` so hoisting `j` ahead of `i`
        // preserves topological order of `base_graph` (e.g., an address-offset
        // shift between `i` and `j` feeding `j`).
        absl::btree_set<int> after_i_set;
        for (auto it = it_i + 1; it != seed_in_base.end(); ++it) {
          after_i_set.insert(*it);
        }
        absl::btree_set<int> to_hoist = {j};
        std::vector<int> stack = {j};
        bool depends_on_i = false;
        while (!stack.empty()) {
          const int cur = stack.back();
          stack.pop_back();
          for (const Link& l : base_graph.log_links[static_cast<size_t>(cur)]) {
            if (l.target == i) {
              depends_on_i = true;
              break;
            }
            if (after_i_set.contains(l.target) && to_hoist.insert(l.target).second) {
              stack.push_back(l.target);
            }
          }
          if (depends_on_i) {
            break;
          }
        }
        if (depends_on_i) {
          continue;
        }
        std::vector<int> hoist_seq;
        absl::btree_set<int> external_deps;
        for (int node : seed_in_base) {
          if (to_hoist.contains(node)) {
            hoist_seq.push_back(node);
            for (const Link& l : base_graph.log_links[static_cast<size_t>(node)]) {
              if (!to_hoist.contains(l.target)) {
                external_deps.insert(l.target);
              }
            }
          }
        }
        seed_in_base.erase(std::remove_if(seed_in_base.begin(), seed_in_base.end(),
                                          [&](int node) { return to_hoist.contains(node); }),
                           seed_in_base.end());
        it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
        auto insert_it = seed_in_base.begin() + prologue_end;
        for (auto it = seed_in_base.begin(); it != it_i; ++it) {
          if (external_deps.contains(*it)) {
            insert_it = it + 1;
          }
        }
        if (insert_it > it_i) {
          insert_it = it_i;
        }
        seed_in_base.insert(insert_it, hoist_seq.begin(), hoist_seq.end());
        any_inverted = true;
      }
    }
  }
  if (!any_inverted) {
    return std::nullopt;
  }
  return seed_in_base;
}

std::vector<std::pair<int, int>> ExtractCoreSched2Edges(absl::Span<const Insn> cur_seq,
                                                        int prologue_end, int epilogue_begin,
                                                        const Sched1ProjectionView& view,
                                                        const RegFacts& facts_s2,
                                                        const MachineModel& mm) {
  std::vector<std::pair<int, int>> edges;
  const int n = static_cast<int>(cur_seq.size());
  const int m = static_cast<int>(view.core_insns.size());
  if (m <= 1 || prologue_end < 0 || epilogue_begin > n || epilogue_begin <= prologue_end) {
    return edges;
  }
  const DepGraph g_s2 = AnalyzeWithoutPendingListFlushCap(cur_seq, facts_s2, mm, Phase::kSched2);
  std::vector<int> seq_to_core(static_cast<size_t>(n), -1);
  for (int ci = 0; ci < m; ++ci) {
    const int si = prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)];
    if (si >= prologue_end && si < epilogue_begin) {
      seq_to_core[static_cast<size_t>(si)] = ci;
    }
  }
  // Compute transitive reachability in `g_s2` over `[prologue_end, epilogue_begin)`
  // (excluding pure `sched.c:1704` post-call artifact edges) and restrict to core
  // instructions. Because `cur_seq` is already topologically ordered in `g_s2`,
  // every edge has `src < dst`.
  const int words = (m + 63) / 64;
  std::vector<std::vector<uint64_t>> reach_core(
      static_cast<size_t>(n), std::vector<uint64_t>(static_cast<size_t>(words), 0));
  for (int si = epilogue_begin - 1; si >= prologue_end; --si) {
    const int ci = seq_to_core[static_cast<size_t>(si)];
    if (ci >= 0) {
      reach_core[static_cast<size_t>(si)][static_cast<size_t>(ci / 64)] |=
          (uint64_t{1} << (ci % 64));
    }
  }
  for (int cons = epilogue_begin - 1; cons >= prologue_end; --cons) {
    for (const Link& l : g_s2.log_links[static_cast<size_t>(cons)]) {
      if (l.target < prologue_end || l.target >= cons) {
        continue;
      }
      if (IsPureSched1704CallArtifactEdge(cur_seq, g_s2, l.target, cons, mm)) {
        continue;
      }
      for (int w = 0; w < words; ++w) {
        reach_core[static_cast<size_t>(l.target)][static_cast<size_t>(w)] |=
            reach_core[static_cast<size_t>(cons)][static_cast<size_t>(w)];
      }
    }
  }
  for (int u = 0; u < m; ++u) {
    const int si = prologue_end + view.core_to_orig_index[static_cast<size_t>(u)];
    if (si < prologue_end || si >= epilogue_begin) {
      continue;
    }
    const auto& bits = reach_core[static_cast<size_t>(si)];
    for (int v = u + 1; v < m; ++v) {
      if ((bits[static_cast<size_t>(v / 64)] & (uint64_t{1} << (v % 64))) != 0) {
        edges.emplace_back(u, v);
      }
    }
  }
  return edges;
}

std::vector<std::pair<int, int>> ExtractCoreSched2ReplayEdges(
    absl::Span<const Insn> target_sched_block, absl::Span<const int> cur_to_target,
    int prologue_end, int epilogue_begin, const Sched1ProjectionView& view,
    const RegFacts& facts_s2, const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2,
    bool preserve_reg_dead_reader_order) {
  std::vector<std::pair<int, int>> edges;
  const int n = static_cast<int>(target_sched_block.size());
  const int m = static_cast<int>(view.core_insns.size());
  if (m <= 1 || n <= 1 || static_cast<int>(cur_to_target.size()) != n || prologue_end < 0 ||
      epilogue_begin > n || epilogue_begin <= prologue_end) {
    return edges;
  }
  std::vector<int> target_to_cur(static_cast<size_t>(n), -1);
  for (int pos = 0; pos < n; ++pos) {
    const int t = cur_to_target[static_cast<size_t>(pos)];
    if (t < 0 || t >= n || target_to_cur[static_cast<size_t>(t)] != -1) {
      return edges;
    }
    target_to_cur[static_cast<size_t>(t)] = pos;
  }
  std::vector<int> target_to_core(static_cast<size_t>(n), -1);
  for (int ci = 0; ci < m; ++ci) {
    const int cur_pos = prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)];
    if (cur_pos >= prologue_end && cur_pos < epilogue_begin) {
      target_to_core[static_cast<size_t>(cur_to_target[static_cast<size_t>(cur_pos)])] = ci;
    }
  }

  const DepGraph target_graph =
      AnalyzeWithoutPendingListFlushCap(target_sched_block, facts_s2, mm, Phase::kSched2);
  const ForcedReplayResult replay = RunForcedReplay(target_sched_block, target_graph, facts_s2, mm,
                                                    Phase::kSched2, live_at_end_s2);

  absl::btree_set<std::pair<int, int>> constrained_edges;
  for (int cons = 0; cons < n; ++cons) {
    for (const Link& l : target_graph.log_links[static_cast<size_t>(cons)]) {
      if (l.target < 0 || l.target >= n || l.target == cons) {
        continue;
      }
      if (IsPureSched1704CallArtifactEdge(target_sched_block, target_graph, l.target, cons, mm)) {
        continue;
      }
      constrained_edges.insert({l.target, cons});
    }
  }
  if (replay.last_tail >= 0) {
    for (int u = 0; u < replay.last_tail; ++u) {
      constrained_edges.insert({u, replay.last_tail});
    }
    for (int k = replay.last_tail; k + 1 < n; ++k) {
      constrained_edges.insert({k, k + 1});
    }
  }
  std::vector<int> hard_next(static_cast<size_t>(n), -1);
  std::vector<int> hard_prev(static_cast<size_t>(n), -1);
  for (int i = 1; i < n; ++i) {
    if (target_graph.sched_group_p[static_cast<size_t>(i)]) {
      hard_next[static_cast<size_t>(i - 1)] = i;
      hard_prev[static_cast<size_t>(i)] = i - 1;
    }
  }
  std::vector<int> identity_rank(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    identity_rank[static_cast<size_t>(i)] = i;
  }
  for (const Constraint& c : replay.constraints) {
    if (c.than < 0 || c.than >= n || c.must_have_greater_luid < 0 ||
        c.must_have_greater_luid >= n || c.than == c.must_have_greater_luid) {
      continue;
    }
    const std::pair<int, int> edge = {c.than, c.must_have_greater_luid};
    if (constrained_edges.contains(edge)) {
      continue;
    }
    constrained_edges.insert(edge);
    if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
             .has_value()) {
      constrained_edges.erase(edge);
    }
  }

  // When strict `PreservesPostReloadOrderSafety(..., allow_transfer=false)` is
  // enforced (Stage 3), every pure reader `j < i` of a register killed at `i`
  // (`REG_DEAD r`, with no intervening def of `r`) must stay ahead of `i`
  // (`sched.c:3415-3611`). Projecting `(j -> i)` keeps `CollectSched1TieFlipCoreOrders`
  // from spending search budget on Gate-(b)-violating `REG_DEAD` reader flips.
  if (preserve_reg_dead_reader_order) {
    std::vector<InsnRegReadDefDeaths> insn_info(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      insn_info[static_cast<size_t>(i)] =
          CollectInsnRegReadDefDeaths(target_sched_block[static_cast<size_t>(i)], mm);
    }
    for (int i = 0; i < n; ++i) {
      for (int r : insn_info[static_cast<size_t>(i)].deaths) {
        for (int j = i - 1; j >= 0; --j) {
          if (insn_info[static_cast<size_t>(j)].reads.contains(r)) {
            const std::pair<int, int> edge = {j, i};
            if (!constrained_edges.contains(edge)) {
              constrained_edges.insert(edge);
              if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
                       .has_value()) {
                constrained_edges.erase(edge);
              }
            }
          }
          if (insn_info[static_cast<size_t>(j)].defs.contains(r)) {
            break;
          }
        }
        // Symmetric inverted-`REG_DEAD` case (`sched.c:3415-3611`): when `sched2`
        // hoisted the pure-reader `(REG_DEAD r)` holder `i` ahead of a later pure
        // reader `j > i` of the same live range of `r`, every valid pre-`sched2`
        // preimage must also place `j` before `i` (`edge = {j, i}`).
        if (!insn_info[static_cast<size_t>(i)].is_call_or_use &&
            !insn_info[static_cast<size_t>(i)].defs.contains(r)) {
          for (int j = i + 1; j < n; ++j) {
            if (insn_info[static_cast<size_t>(j)].defs.contains(r)) {
              break;
            }
            if (!insn_info[static_cast<size_t>(j)].is_call_or_use &&
                insn_info[static_cast<size_t>(j)].reads.contains(r)) {
              const std::pair<int, int> edge = {j, i};
              if (!constrained_edges.contains(edge)) {
                constrained_edges.insert(edge);
                if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
                         .has_value()) {
                  constrained_edges.erase(edge);
                }
              }
            }
          }
        }
      }
    }
  }

  // Project constraints on excluded input/output reload instructions onto their
  // owning core instruction (`reload1.c:emit_reload_insns` places input reloads
  // immediately before and output reloads immediately after their owner, with no
  // other core instruction intervening).
  std::vector<int> target_reload_owner(static_cast<size_t>(n), -1);
  for (const auto& [body_idx, ci] : view.input_reload_before_core) {
    const int cur_pos = prologue_end + body_idx;
    if (cur_pos >= prologue_end && cur_pos < epilogue_begin && ci >= 0 && ci < m) {
      target_reload_owner[static_cast<size_t>(cur_to_target[static_cast<size_t>(cur_pos)])] = ci;
    }
  }
  for (const auto& [body_idx, ci] : view.output_reload_after_core) {
    const int cur_pos = prologue_end + body_idx;
    if (cur_pos >= prologue_end && cur_pos < epilogue_begin && ci >= 0 && ci < m) {
      target_reload_owner[static_cast<size_t>(cur_to_target[static_cast<size_t>(cur_pos)])] = ci;
    }
  }
  auto add_reload_projected_edges = [&](int from_t, int to_t) {
    if (from_t < 0 || from_t >= n || to_t < 0 || to_t >= n || from_t == to_t) {
      return;
    }
    const std::pair<int, int> edge = {from_t, to_t};
    if (constrained_edges.contains(edge)) {
      return;
    }
    constrained_edges.insert(edge);
    if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
             .has_value()) {
      constrained_edges.erase(edge);
    }
  };
  const std::vector<std::pair<int, int>> base_constrained(constrained_edges.begin(),
                                                          constrained_edges.end());
  for (const auto& [body_idx, ci] : view.input_reload_before_core) {
    const int cur_pos = prologue_end + body_idx;
    const int owner_cur = (ci >= 0 && ci < m)
                              ? (prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)])
                              : -1;
    if (cur_pos < prologue_end || cur_pos >= epilogue_begin || owner_cur < prologue_end ||
        owner_cur >= epilogue_begin) {
      continue;
    }
    const int t_r = cur_to_target[static_cast<size_t>(cur_pos)];
    const int t_u = cur_to_target[static_cast<size_t>(owner_cur)];
    for (const auto& [u, v] : base_constrained) {
      if (u == t_r && v != t_u && target_reload_owner[static_cast<size_t>(v)] != ci) {
        add_reload_projected_edges(t_u, v);
      }
    }
  }
  for (const auto& [body_idx, ci] : view.output_reload_after_core) {
    const int cur_pos = prologue_end + body_idx;
    const int owner_cur = (ci >= 0 && ci < m)
                              ? (prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)])
                              : -1;
    if (cur_pos < prologue_end || cur_pos >= epilogue_begin || owner_cur < prologue_end ||
        owner_cur >= epilogue_begin) {
      continue;
    }
    const int t_s = cur_to_target[static_cast<size_t>(cur_pos)];
    const int t_u = cur_to_target[static_cast<size_t>(owner_cur)];
    for (const auto& [u, v] : base_constrained) {
      if (v == t_s && u != t_u && target_reload_owner[static_cast<size_t>(u)] != ci) {
        add_reload_projected_edges(u, t_u);
      }
    }
  }

  const std::optional<std::vector<int>> topo =
      SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank);
  if (!topo.has_value()) {
    return edges;
  }
  std::vector<std::vector<int>> succs(static_cast<size_t>(n));
  for (const auto& [u, v] : constrained_edges) {
    succs[static_cast<size_t>(u)].push_back(v);
  }
  const int words = (m + 63) / 64;
  std::vector<std::vector<uint64_t>> reach_core(
      static_cast<size_t>(n), std::vector<uint64_t>(static_cast<size_t>(words), 0));
  for (int idx = n - 1; idx >= 0; --idx) {
    const int u = (*topo)[static_cast<size_t>(idx)];
    const int ci = target_to_core[static_cast<size_t>(u)];
    if (ci >= 0) {
      reach_core[static_cast<size_t>(u)][static_cast<size_t>(ci / 64)] |=
          (uint64_t{1} << (ci % 64));
    }
    for (int v : succs[static_cast<size_t>(u)]) {
      for (int w = 0; w < words; ++w) {
        reach_core[static_cast<size_t>(u)][static_cast<size_t>(w)] |=
            reach_core[static_cast<size_t>(v)][static_cast<size_t>(w)];
      }
    }
  }
  for (int u = 0; u < m; ++u) {
    const int cur_pos = prologue_end + view.core_to_orig_index[static_cast<size_t>(u)];
    if (cur_pos < prologue_end || cur_pos >= epilogue_begin) {
      continue;
    }
    const int t_u = cur_to_target[static_cast<size_t>(cur_pos)];
    const auto& bits = reach_core[static_cast<size_t>(t_u)];
    for (int v = 0; v < m; ++v) {
      if (v != u && (bits[static_cast<size_t>(v / 64)] & (uint64_t{1} << (v % 64))) != 0) {
        edges.emplace_back(u, v);
      }
    }
  }
  return edges;
}

namespace {

// Conflict-driven `sched1` tie-flip solver (`sched.c:2428` `rank_for_schedule`,
// `sched.c:2557-2609` `schedule_insn`, `toplev.c:3028-3087`): given a `kSched1`
// schedule `base_res` that violates one or more post-reload `core_s2_edges`
// `(u -> v)` (meaning backward `sched1` scheduled `v` AFTER `u` instead of
// BEFORE `u`), promotes `v` or its blocking `g_s1` consumer chain earlier in
// the backward schedule via cumulative equal-priority LUID tie-flips until
// `core_s2_edges` violations drop to 0.
void CollectConflictDrivenSched2Orders(
    const Sched1ProjectionView& view, const DepGraph& g_s1, const ScheduleResult& base_res,
    const absl::btree_set<std::pair<int, int>>& dag_edges, const std::vector<int>& hard_next,
    const std::vector<int>& hard_prev, const std::vector<int>& base_order,
    absl::Span<const std::pair<int, int>> core_s2_edges, const MachineModel& mm, int max_orders,
    int non_zero_cap, absl::btree_set<std::vector<int>>* seen_orders,
    std::vector<std::vector<int>>* out) {
  const int m = static_cast<int>(view.core_insns.size());
  if (core_s2_edges.empty() || m <= 1 || static_cast<int>(out->size()) >= max_orders) {
    return;
  }
  std::vector<int> base_rank(static_cast<size_t>(m), 0);
  for (int pos = 0; pos < m; ++pos) {
    base_rank[static_cast<size_t>(base_order[static_cast<size_t>(pos)])] = pos;
  }
  // Build reverse `g_s1` consumer adjacency (`prod -> consumers`).
  std::vector<std::vector<int>> consumers_of(static_cast<size_t>(m));
  for (int cons = 0; cons < m; ++cons) {
    for (const Link& l : g_s1.log_links[static_cast<size_t>(cons)]) {
      if (l.target >= 0 && l.target < m && l.target != cons) {
        consumers_of[static_cast<size_t>(l.target)].push_back(cons);
      }
    }
  }

  struct EvalState {
    absl::btree_set<std::pair<int, int>> extra_edges;
    std::vector<int> input_to_orig;
    ScheduleResult res;
    std::vector<int> mapped;
    int num_s2_viol = 0;
    int first_viol_step = 0;
    int s1_diffs = 0;
  };

  auto score_schedule = [&](const ScheduleResult& res, absl::Span<const int> input_to_orig,
                            std::vector<int>* mapped_out, int* num_viol_out, int* first_viol_out,
                            int* s1_diffs_out) {
    mapped_out->resize(static_cast<size_t>(m));
    std::vector<int> pos_in_mapped(static_cast<size_t>(m), -1);
    int diffs = 0;
    for (int pos = 0; pos < m; ++pos) {
      const int orig = input_to_orig[static_cast<size_t>(res.order[static_cast<size_t>(pos)])];
      (*mapped_out)[static_cast<size_t>(pos)] = orig;
      pos_in_mapped[static_cast<size_t>(orig)] = pos;
      if (orig != pos) {
        ++diffs;
      }
    }
    int viol = 0;
    int first_v = m;
    for (const auto& [u, v] : core_s2_edges) {
      if (pos_in_mapped[static_cast<size_t>(v)] < pos_in_mapped[static_cast<size_t>(u)]) {
        ++viol;
        const int u_step = m - 1 - pos_in_mapped[static_cast<size_t>(u)];
        first_v = std::min(first_v, u_step);
      }
    }
    *num_viol_out = viol;
    *first_viol_out = first_v;
    *s1_diffs_out = diffs;
  };

  auto extract_promotion_sets =
      [&](const EvalState& cur,
          bool include_u_demote) -> std::vector<absl::btree_set<std::pair<int, int>>> {
    std::vector<int> step_of_orig(static_cast<size_t>(m), -1);
    for (int s = 0; s < static_cast<int>(cur.res.trace.size()); ++s) {
      const int orig =
          cur.input_to_orig[static_cast<size_t>(cur.res.trace[static_cast<size_t>(s)].chosen)];
      step_of_orig[static_cast<size_t>(orig)] = s;
    }
    // Collect violated `core_s2_edges` `(u, v)` ordered by earliest backward step
    // `step_of_orig[u]`.
    std::vector<std::tuple<int, int, int, int>> viols;
    for (const auto& [u, v] : core_s2_edges) {
      const int su = step_of_orig[static_cast<size_t>(u)];
      const int sv = step_of_orig[static_cast<size_t>(v)];
      if (su >= 0 && sv >= 0 && su < sv) {
        viols.emplace_back(su, sv, u, v);
      }
    }
    std::sort(viols.begin(), viols.end());

    std::vector<absl::btree_set<std::pair<int, int>>> edge_sets;
    absl::btree_set<absl::btree_set<std::pair<int, int>>> dedup;
    auto push_set = [&](absl::btree_set<std::pair<int, int>> es) {
      if (!es.empty() && dedup.insert(es).second) {
        edge_sets.push_back(std::move(es));
      }
    };

    auto emit_promote_node = [&](int target_orig, int max_shift) {
      const int st_target = step_of_orig[static_cast<size_t>(target_orig)];
      if (st_target <= 0) {
        return;
      }
      absl::btree_set<std::pair<int, int>> accum;
      const int low_step = std::max(0, st_target - max_shift);
      for (int t = st_target - 1; t >= low_step; --t) {
        const StepTrace& st = cur.res.trace[static_cast<size_t>(t)];
        if (st.ready_before_select.size() != st.priorities.size()) {
          continue;
        }
        int chosen_pri = 0;
        bool found_chosen = false;
        int target_pri = 0;
        bool found_target = false;
        for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
          const int r_in = st.ready_before_select[k];
          if (r_in == st.chosen) {
            chosen_pri = st.priorities[k];
            found_chosen = true;
          }
          if (cur.input_to_orig[static_cast<size_t>(r_in)] == target_orig) {
            target_pri = st.priorities[k];
            found_target = true;
          }
        }
        if (found_chosen && found_target && target_pri == chosen_pri) {
          const int chosen_orig = cur.input_to_orig[static_cast<size_t>(st.chosen)];
          if (chosen_orig != target_orig) {
            accum.insert({chosen_orig, target_orig});
            push_set(accum);
          }
        }
      }
    };

    auto emit_demote_node = [&](int target_orig) {
      const int st_target = step_of_orig[static_cast<size_t>(target_orig)];
      if (st_target < 0 || st_target >= static_cast<int>(cur.res.trace.size())) {
        return;
      }
      const StepTrace& st = cur.res.trace[static_cast<size_t>(st_target)];
      if (st.ready_before_select.size() != st.priorities.size()) {
        return;
      }
      int target_pri = 0;
      bool found_target = false;
      for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
        if (st.ready_before_select[k] == st.chosen) {
          target_pri = st.priorities[k];
          found_target = true;
          break;
        }
      }
      if (!found_target) {
        return;
      }
      absl::btree_set<std::pair<int, int>> accum;
      for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
        const int other_orig = cur.input_to_orig[static_cast<size_t>(st.ready_before_select[k])];
        if (other_orig != target_orig && st.priorities[k] == target_pri) {
          push_set({{target_orig, other_orig}});
          accum.insert({target_orig, other_orig});
        }
      }
      if (accum.size() > 1) {
        push_set(std::move(accum));
      }
    };

    const int max_viols = std::min<int>(3, static_cast<int>(viols.size()));
    for (int vi = 0; vi < max_viols; ++vi) {
      const auto [su, sv, u, v] = viols[static_cast<size_t>(vi)];
      const int shift = std::max(1, sv - su) + 3;
      // (a) Promote `v` directly if `v` was already ready before `sv`.
      emit_promote_node(v, shift);
      // (b) Promote `v`'s latest-scheduled blocking `g_s1` consumers (depth 1 and 2).
      std::vector<std::pair<int, int>> d1_cons;
      for (int c : consumers_of[static_cast<size_t>(v)]) {
        const int sc = step_of_orig[static_cast<size_t>(c)];
        if (sc >= 0 && sc < sv) {
          d1_cons.emplace_back(-sc, c);
        }
      }
      std::sort(d1_cons.begin(), d1_cons.end());
      for (size_t ci = 0; ci < std::min<size_t>(2, d1_cons.size()); ++ci) {
        const int c = d1_cons[ci].second;
        emit_promote_node(c, shift);
        std::vector<std::pair<int, int>> d2_cons;
        for (int c2 : consumers_of[static_cast<size_t>(c)]) {
          const int sc2 = step_of_orig[static_cast<size_t>(c2)];
          if (sc2 >= 0 && sc2 < step_of_orig[static_cast<size_t>(c)]) {
            d2_cons.emplace_back(-sc2, c2);
          }
        }
        std::sort(d2_cons.begin(), d2_cons.end());
        for (size_t ci2 = 0; ci2 < std::min<size_t>(2, d2_cons.size()); ++ci2) {
          emit_promote_node(d2_cons[ci2].second, shift);
        }
      }
      // (c) Demote `u` at `su` (`sched.c:2428` `rank_for_schedule`). Pass 1
      //     keeps single-edge demotes of `u` itself; Pass 2 (when Pass 1 finds
      //     no zero-violation order) adds multi-edge `u` demotes and walks
      //     `u`'s consumer chain (up to depth 4) so pushing a sunk `u`-consumer
      //     later in backward time pulls `u` ahead of `v`.
      if (!include_u_demote) {
        const StepTrace& st_u = cur.res.trace[static_cast<size_t>(su)];
        if (st_u.ready_before_select.size() == st_u.priorities.size()) {
          int u_pri = 0;
          bool found_u = false;
          for (size_t k = 0; k < st_u.ready_before_select.size(); ++k) {
            if (st_u.ready_before_select[k] == st_u.chosen) {
              u_pri = st_u.priorities[k];
              found_u = true;
              break;
            }
          }
          if (found_u) {
            for (size_t k = 0; k < st_u.ready_before_select.size(); ++k) {
              const int other_orig =
                  cur.input_to_orig[static_cast<size_t>(st_u.ready_before_select[k])];
              if (other_orig != u && st_u.priorities[k] == u_pri) {
                push_set({{u, other_orig}});
              }
            }
          }
        }
      } else {
        emit_demote_node(u);
        std::vector<int> u_frontier = {u};
        absl::btree_set<int> u_seen = {u};
        for (int depth = 0; depth < 4 && !u_frontier.empty(); ++depth) {
          std::vector<std::pair<int, int>> next_cons;
          for (int cur_u : u_frontier) {
            const int scur = step_of_orig[static_cast<size_t>(cur_u)];
            for (int c : consumers_of[static_cast<size_t>(cur_u)]) {
              const int sc = step_of_orig[static_cast<size_t>(c)];
              if (sc >= 0 && sc < scur && !u_seen.contains(c)) {
                next_cons.emplace_back(-sc, c);
              }
            }
          }
          std::sort(next_cons.begin(), next_cons.end());
          u_frontier.clear();
          for (size_t ci = 0; ci < next_cons.size() && u_frontier.size() < 2; ++ci) {
            const int c = next_cons[ci].second;
            if (u_seen.insert(c).second) {
              emit_demote_node(c);
              u_frontier.push_back(c);
            }
          }
        }
      }
    }
    return edge_sets;
  };

  EvalState base_state;
  base_state.input_to_orig = base_order;
  base_state.res = base_res;
  score_schedule(base_state.res, base_state.input_to_orig, &base_state.mapped,
                 &base_state.num_s2_viol, &base_state.first_viol_step, &base_state.s1_diffs);
  if (base_state.num_s2_viol == 0) {
    return;
  }

  std::vector<std::vector<int>> zero_viol_orders;
  int non_zero_added = 0;
  constexpr int kMaxConflictSteps = 16;
  // Pass 1: pure `v`-promotion hill climb (monotonic preservation of all
  // promote-solvable blocks). Pass 2: if Pass 1 finds no zero-violation order,
  // retry with `u`-consumer-chain demotion enabled (`sched.c:2428`).
  for (const bool include_u_demote : {false, true}) {
    if (!zero_viol_orders.empty() || static_cast<int>(out->size()) >= max_orders) {
      break;
    }
    EvalState cur = base_state;
    absl::btree_set<absl::btree_set<std::pair<int, int>>> visited_sets;
    visited_sets.insert({});
    for (int step = 0; step < kMaxConflictSteps && static_cast<int>(out->size()) < max_orders;
         ++step) {
      const std::vector<absl::btree_set<std::pair<int, int>>> promos =
          extract_promotion_sets(cur, include_u_demote);
      if (promos.empty()) {
        break;
      }
      bool has_best = false;
      EvalState best;
      for (const auto& add_edges : promos) {
        absl::btree_set<std::pair<int, int>> next_extra = cur.extra_edges;
        bool conflict_with_existing = false;
        for (const auto& [a, b] : add_edges) {
          if (next_extra.contains({b, a})) {
            conflict_with_existing = true;
            break;
          }
          next_extra.insert({a, b});
        }
        if (conflict_with_existing || !visited_sets.insert(next_extra).second) {
          continue;
        }
        absl::btree_set<std::pair<int, int>> edges = dag_edges;
        edges.insert(next_extra.begin(), next_extra.end());
        const std::optional<std::vector<int>> flipped_input =
            SolveSupernodeKahn(m, edges, hard_next, hard_prev, base_rank);
        if (!flipped_input.has_value()) {
          continue;
        }
        std::vector<Insn> flipped_core;
        flipped_core.reserve(static_cast<size_t>(m));
        for (int pos = 0; pos < m; ++pos) {
          Insn insn =
              view.core_insns[static_cast<size_t>((*flipped_input)[static_cast<size_t>(pos)])];
          insn.luid = pos;
          flipped_core.push_back(std::move(insn));
        }
        const DepGraph g_flip = Analyze(flipped_core, view.facts_s1, mm, Phase::kSched1);
        ScheduleResult res_flip =
            Schedule(flipped_core, g_flip, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
        if (static_cast<int>(res_flip.order.size()) != m) {
          continue;
        }
        EvalState cand;
        cand.extra_edges = std::move(next_extra);
        cand.input_to_orig = *flipped_input;
        cand.res = std::move(res_flip);
        score_schedule(cand.res, cand.input_to_orig, &cand.mapped, &cand.num_s2_viol,
                       &cand.first_viol_step, &cand.s1_diffs);
        if (cand.num_s2_viol == 0) {
          if (seen_orders->insert(cand.mapped).second) {
            zero_viol_orders.push_back(cand.mapped);
          }
        } else if (non_zero_added < non_zero_cap && static_cast<int>(out->size()) < max_orders &&
                   seen_orders->insert(cand.mapped).second) {
          out->push_back(cand.mapped);
          ++non_zero_added;
        }
        auto better = [](const EvalState& a, const EvalState& b) {
          return std::tie(a.num_s2_viol, b.first_viol_step, a.s1_diffs) <
                 std::tie(b.num_s2_viol, a.first_viol_step, b.s1_diffs);
        };
        if (better(cand, cur) && (!has_best || better(cand, best))) {
          best = std::move(cand);
          has_best = true;
        }
      }
      if (!zero_viol_orders.empty() || !has_best) {
        break;
      }
      cur = std::move(best);
    }
  }
  if (!zero_viol_orders.empty()) {
    out->insert(out->begin(), zero_viol_orders.begin(), zero_viol_orders.end());
    if (static_cast<int>(out->size()) > max_orders) {
      out->resize(static_cast<size_t>(max_orders));
    }
  }
}

}  // namespace

std::vector<std::vector<int>> CollectSched1TieFlipCoreOrders(
    const Sched1ProjectionView& view, const MachineModel& mm, int max_orders,
    absl::Span<const std::pair<int, int>> core_s2_edges,
    absl::Span<const std::pair<int, int>> core_s2_replay_edges) {
  std::vector<std::vector<int>> out;
  const int m = static_cast<int>(view.core_insns.size());
  if (m <= 1 || max_orders <= 0) {
    return out;
  }
  const DepGraph g_s1 = Analyze(view.core_insns, view.facts_s1, mm, Phase::kSched1);
  const ScheduleResult base_res =
      Schedule(view.core_insns, g_s1, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
  if (static_cast<int>(base_res.order.size()) != m) {
    return out;
  }

  absl::btree_set<std::pair<int, int>> dag_edges;
  for (int cons = 0; cons < m; ++cons) {
    for (const Link& l : g_s1.log_links[static_cast<size_t>(cons)]) {
      dag_edges.insert({l.target, cons});
    }
  }
  std::vector<int> hard_next(static_cast<size_t>(m), -1);
  std::vector<int> hard_prev(static_cast<size_t>(m), -1);
  for (int i = 1; i < m; ++i) {
    if (g_s1.sched_group_p[static_cast<size_t>(i)]) {
      hard_next[static_cast<size_t>(i - 1)] = i;
      hard_prev[static_cast<size_t>(i)] = i - 1;
    }
  }
  std::vector<int> identity_rank(static_cast<size_t>(m));
  for (int i = 0; i < m; ++i) {
    identity_rank[static_cast<size_t>(i)] = i;
  }

  auto extract_ties = [&](const ScheduleResult& res, absl::Span<const int> input_to_orig,
                          int branch_limit) -> std::vector<std::pair<int, int>> {
    int first_div_step = static_cast<int>(res.trace.size());
    for (int step_idx = 0; step_idx < static_cast<int>(res.trace.size()); ++step_idx) {
      const int chosen_orig =
          input_to_orig[static_cast<size_t>(res.trace[static_cast<size_t>(step_idx)].chosen)];
      const int expected_orig = m - 1 - step_idx;
      if (chosen_orig != expected_orig) {
        first_div_step = step_idx;
        break;
      }
    }
    struct TieCandidate {
      int after_div = 0;
      int dist = 0;
      int step_idx = 0;
      int chosen_orig = -1;
      int other_orig = -1;
      bool operator<(const TieCandidate& rhs) const {
        return std::tie(after_div, dist, step_idx, chosen_orig, other_orig) <
               std::tie(rhs.after_div, rhs.dist, rhs.step_idx, rhs.chosen_orig, rhs.other_orig);
      }
    };
    std::vector<TieCandidate> ties;
    absl::btree_set<std::pair<int, int>> seen_pairs;
    for (int step_idx = 0; step_idx < static_cast<int>(res.trace.size()); ++step_idx) {
      const StepTrace& st = res.trace[static_cast<size_t>(step_idx)];
      if (st.ready_before_select.size() < 2 ||
          st.ready_before_select.size() != st.priorities.size()) {
        continue;
      }
      int chosen_pri = 0;
      bool found_chosen = false;
      for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
        if (st.ready_before_select[k] == st.chosen) {
          chosen_pri = st.priorities[k];
          found_chosen = true;
          break;
        }
      }
      if (!found_chosen) {
        continue;
      }
      const int chosen_orig = input_to_orig[static_cast<size_t>(st.chosen)];
      for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
        const int other = st.ready_before_select[k];
        if (other == st.chosen || st.priorities[k] != chosen_pri) {
          continue;
        }
        const int other_orig = input_to_orig[static_cast<size_t>(other)];
        if (!seen_pairs.insert({chosen_orig, other_orig}).second) {
          continue;
        }
        ties.push_back(TieCandidate{
            .after_div = (step_idx > first_div_step) ? 1 : 0,
            .dist = std::abs(step_idx - first_div_step),
            .step_idx = step_idx,
            .chosen_orig = chosen_orig,
            .other_orig = other_orig,
        });
      }
    }
    std::sort(ties.begin(), ties.end());
    std::vector<std::pair<int, int>> result;
    for (const TieCandidate& tc : ties) {
      if (static_cast<int>(result.size()) >= branch_limit) {
        break;
      }
      result.push_back({tc.chosen_orig, tc.other_orig});
    }
    return result;
  };

  absl::btree_set<std::vector<int>> seen_orders;
  seen_orders.insert(base_res.order);
  seen_orders.insert(identity_rank);

  absl::btree_set<std::pair<int, int>> joint_edges = dag_edges;
  for (const auto& e : core_s2_edges) {
    joint_edges.insert(e);
  }
  absl::btree_set<std::pair<int, int>> joint_replay_edges = joint_edges;
  for (const auto& e : core_s2_replay_edges) {
    joint_replay_edges.insert(e);
  }
  const bool has_replay_edges =
      !core_s2_replay_edges.empty() && joint_replay_edges.size() > joint_edges.size();
  // Combined `kSched2` scoring edge set (DAG + forced-replay LUID tie-break
  // constraints when available) so seeds and conflict-driven promotion respect
  // exact `sched2` LUID tie-breaks (`sched.c:2428`).
  std::vector<std::pair<int, int>> combined_s2_edges(core_s2_edges.begin(), core_s2_edges.end());
  if (has_replay_edges) {
    absl::btree_set<std::pair<int, int>> merged(core_s2_edges.begin(), core_s2_edges.end());
    for (const auto& e : core_s2_replay_edges) {
      merged.insert(e);
    }
    combined_s2_edges.assign(merged.begin(), merged.end());
  }

  // First run conflict-driven resolution of violated `core_s2_edges` so orders
  // in `Im(F_sched1) ∩ Topo(g_s2)` are placed at the front of `out`, while
  // capping non-zero-violation intermediates so canonical `.combine` seeds and
  // BFS tie-flips are never starved.
  const int conflict_non_zero_cap = std::max(1, max_orders / 3);
  CollectConflictDrivenSched2Orders(view, g_s1, base_res, dag_edges, hard_next, hard_prev,
                                    identity_rank, combined_s2_edges, mm, max_orders,
                                    conflict_non_zero_cap, &seen_orders, &out);

  // Canonical pre-`sched1` (`.06.combine`) topological seeds (`expr.c:4027`
  // `expand_expr`, `sched.c:2428` `rank_for_schedule` LUID tie-breaking):
  // post-reload `sched2` frequently sinks multi-cycle FP/ALU consumers past
  // independent instructions or hoists independent loads above their consuming
  // ALU ops, trapping default-LUID `sched1` replay in stale post-`sched2` LUID
  // tie-breaks. Evaluating `Schedule(Q_seed, kSched1)` on ASAP, critical-path,
  // and sink-rooted / consumer-clustered expression-tree DFS post-order seeds
  // (under `g_s1 ∪ core_s2_replay_edges`, `g_s1 ∪ core_s2_edges`, and `g_s1`)
  // recovers un-sunk `Im(F_sched1)` candidates; zero-violation seeds are
  // prioritized ahead of partial states.
  auto count_s2_violations = [&](absl::Span<const int> mapped) -> int {
    std::vector<int> pos_in_mapped(static_cast<size_t>(m), -1);
    for (int pos = 0; pos < m; ++pos) {
      pos_in_mapped[static_cast<size_t>(mapped[static_cast<size_t>(pos)])] = pos;
    }
    int viol = 0;
    for (const auto& [u, v] : combined_s2_edges) {
      if (pos_in_mapped[static_cast<size_t>(v)] < pos_in_mapped[static_cast<size_t>(u)]) {
        ++viol;
      }
    }
    return viol;
  };
  std::vector<std::vector<int>> zero_viol_seeds;
  std::vector<std::pair<int, std::vector<int>>> non_zero_seeds;
  struct SeedConflictCandidate {
    int viol = 0;
    std::vector<int> seed_input;
    ScheduleResult res_seed;
  };
  std::optional<SeedConflictCandidate> best_seed_for_conflict;
  auto try_add_seed_rank_with_edges = [&](const std::vector<int>& rank_vec,
                                          const absl::btree_set<std::pair<int, int>>& seed_edges) {
    const std::optional<std::vector<int>> seed_input =
        SolveSupernodeKahn(m, seed_edges, hard_next, hard_prev, rank_vec);
    if (!seed_input.has_value()) {
      return;
    }
    std::vector<Insn> seed_core;
    seed_core.reserve(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      Insn insn = view.core_insns[static_cast<size_t>((*seed_input)[static_cast<size_t>(pos)])];
      insn.luid = pos;
      seed_core.push_back(std::move(insn));
    }
    const DepGraph g_seed = Analyze(seed_core, view.facts_s1, mm, Phase::kSched1);
    ScheduleResult res_seed =
        Schedule(seed_core, g_seed, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
    if (static_cast<int>(res_seed.order.size()) != m) {
      return;
    }
    std::vector<int> mapped(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      mapped[static_cast<size_t>(pos)] =
          (*seed_input)[static_cast<size_t>(res_seed.order[static_cast<size_t>(pos)])];
    }
    const int viol = count_s2_violations(mapped);
    if (viol == 0) {
      if (seen_orders.insert(mapped).second) {
        zero_viol_seeds.push_back(std::move(mapped));
      }
    } else {
      if (seen_orders.insert(mapped).second) {
        non_zero_seeds.emplace_back(viol, std::move(mapped));
      }
      if (!best_seed_for_conflict.has_value() || viol < best_seed_for_conflict->viol) {
        best_seed_for_conflict = SeedConflictCandidate{
            .viol = viol,
            .seed_input = *seed_input,
            .res_seed = std::move(res_seed),
        };
      }
    }
  };
  auto try_add_seed_rank = [&](const std::vector<int>& rank_vec) {
    if (has_replay_edges) {
      try_add_seed_rank_with_edges(rank_vec, joint_replay_edges);
    }
    if (!core_s2_edges.empty()) {
      try_add_seed_rank_with_edges(rank_vec, joint_edges);
    }
    try_add_seed_rank_with_edges(rank_vec, dag_edges);
  };

  {
    std::vector<bool> has_consumer(static_cast<size_t>(m), false);
    std::vector<bool> has_true_consumer(static_cast<size_t>(m), false);
    std::vector<int> true_indeg(static_cast<size_t>(m), 0);
    for (int u = 0; u < m; ++u) {
      for (const Link& l : g_s1.log_links[static_cast<size_t>(u)]) {
        if (l.target < 0 || l.target >= m || l.target == u) {
          continue;
        }
        has_consumer[static_cast<size_t>(l.target)] = true;
        if (l.kind == DepKind::kTrue) {
          has_true_consumer[static_cast<size_t>(l.target)] = true;
          ++true_indeg[static_cast<size_t>(u)];
        }
      }
    }

    auto build_dfs_postorder_rank = [&](bool reverse_roots, bool true_deps_first,
                                        bool consumer_clustered) {
      std::vector<int> finish_rank(static_cast<size_t>(m), m);
      std::vector<bool> visited(static_cast<size_t>(m), false);
      int next_rank = 0;
      auto dfs = [&](auto& self, int u) -> void {
        if (visited[static_cast<size_t>(u)]) {
          return;
        }
        visited[static_cast<size_t>(u)] = true;
        std::vector<int> true_preds;
        std::vector<int> other_preds;
        for (const Link& l : g_s1.log_links[static_cast<size_t>(u)]) {
          if (l.target < 0 || l.target >= m || l.target == u) {
            continue;
          }
          if (true_deps_first && l.kind == DepKind::kTrue) {
            true_preds.push_back(l.target);
          } else {
            other_preds.push_back(l.target);
          }
        }
        std::sort(true_preds.begin(), true_preds.end());
        std::sort(other_preds.begin(), other_preds.end());
        for (int p : true_preds) {
          self(self, p);
        }
        for (int p : other_preds) {
          self(self, p);
        }
        finish_rank[static_cast<size_t>(u)] = next_rank++;
      };
      if (consumer_clustered) {
        // Cluster hoisted leaf loads/constants directly before their first
        // consuming ALU/store/call instruction (`expr.c:4027` `expand_expr`)
        // while preserving the relative order of consumers.
        for (int u = 0; u < m; ++u) {
          if (true_indeg[static_cast<size_t>(u)] > 0 ||
              !has_true_consumer[static_cast<size_t>(u)]) {
            dfs(dfs, u);
          }
        }
        for (int u = 0; u < m; ++u) {
          dfs(dfs, u);
        }
      } else if (reverse_roots) {
        for (int u = m - 1; u >= 0; --u) {
          if (!has_consumer[static_cast<size_t>(u)]) {
            dfs(dfs, u);
          }
        }
        for (int u = m - 1; u >= 0; --u) {
          dfs(dfs, u);
        }
      } else {
        for (int u = 0; u < m; ++u) {
          if (!has_consumer[static_cast<size_t>(u)]) {
            dfs(dfs, u);
          }
        }
        for (int u = 0; u < m; ++u) {
          dfs(dfs, u);
        }
      }
      return finish_rank;
    };
    try_add_seed_rank(build_dfs_postorder_rank(/*reverse_roots=*/false, /*true_deps_first=*/true,
                                               /*consumer_clustered=*/true));
    try_add_seed_rank(build_dfs_postorder_rank(/*reverse_roots=*/false, /*true_deps_first=*/true,
                                               /*consumer_clustered=*/false));
    try_add_seed_rank(build_dfs_postorder_rank(/*reverse_roots=*/true, /*true_deps_first=*/true,
                                               /*consumer_clustered=*/false));

    std::vector<int> asap_depth(static_cast<size_t>(m), 0);
    for (int u = 0; u < m; ++u) {
      for (const Link& l : g_s1.log_links[static_cast<size_t>(u)]) {
        if (l.target >= 0 && l.target < u) {
          asap_depth[static_cast<size_t>(u)] = std::max(
              asap_depth[static_cast<size_t>(u)], asap_depth[static_cast<size_t>(l.target)] + 1);
        }
      }
    }
    std::vector<int> asap_order(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
      asap_order[static_cast<size_t>(i)] = i;
    }
    std::sort(asap_order.begin(), asap_order.end(), [&](int a, int b) {
      if (asap_depth[static_cast<size_t>(a)] != asap_depth[static_cast<size_t>(b)]) {
        return asap_depth[static_cast<size_t>(a)] < asap_depth[static_cast<size_t>(b)];
      }
      return a < b;
    });
    std::vector<int> asap_rank(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      asap_rank[static_cast<size_t>(asap_order[static_cast<size_t>(pos)])] = pos;
    }
    try_add_seed_rank(asap_rank);

    std::vector<int> dist_to_sink(static_cast<size_t>(m), 0);
    for (int u = m - 1; u >= 0; --u) {
      for (const Link& l : g_s1.log_links[static_cast<size_t>(u)]) {
        if (l.target >= 0 && l.target < u) {
          dist_to_sink[static_cast<size_t>(l.target)] =
              std::max(dist_to_sink[static_cast<size_t>(l.target)],
                       dist_to_sink[static_cast<size_t>(u)] + 1);
        }
      }
    }
    std::vector<int> crit_order(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
      crit_order[static_cast<size_t>(i)] = i;
    }
    std::sort(crit_order.begin(), crit_order.end(), [&](int a, int b) {
      if (dist_to_sink[static_cast<size_t>(a)] != dist_to_sink[static_cast<size_t>(b)]) {
        return dist_to_sink[static_cast<size_t>(a)] > dist_to_sink[static_cast<size_t>(b)];
      }
      return a < b;
    });
    std::vector<int> crit_rank(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      crit_rank[static_cast<size_t>(crit_order[static_cast<size_t>(pos)])] = pos;
    }
    try_add_seed_rank(crit_rank);
  }
  std::vector<std::vector<int>> seed_conflict_out;
  if (zero_viol_seeds.empty() && best_seed_for_conflict.has_value()) {
    CollectConflictDrivenSched2Orders(
        view, g_s1, best_seed_for_conflict->res_seed, dag_edges, hard_next, hard_prev,
        best_seed_for_conflict->seed_input, combined_s2_edges, mm, max_orders,
        /*non_zero_cap=*/std::max(1, max_orders / 3), &seen_orders, &seed_conflict_out);
  }
  // Assemble `out` in strict `combined_s2_edges` quality order:
  //   (1) zero-violation orders (from conflict-driven runs and canonical seeds),
  //   (2) lowest-violation partial states (leaving room for BFS tie-flips if needed).
  std::vector<std::pair<int, std::vector<int>>> ranked_candidates;
  for (std::vector<int>& ord : zero_viol_seeds) {
    ranked_candidates.emplace_back(0, std::move(ord));
  }
  for (std::vector<int>& ord : seed_conflict_out) {
    const int v = count_s2_violations(ord);
    ranked_candidates.emplace_back(v, std::move(ord));
  }
  for (std::vector<int>& ord : out) {
    const int v = count_s2_violations(ord);
    ranked_candidates.emplace_back(v, std::move(ord));
  }
  for (auto& [v, ord] : non_zero_seeds) {
    ranked_candidates.emplace_back(v, std::move(ord));
  }
  std::stable_sort(ranked_candidates.begin(), ranked_candidates.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  out.clear();
  // Always reserve room for local `extract_ties` BFS flips around
  // `first_div_step` (`sched.c:2428` `rank_for_schedule`) so global canonical
  // seeds never starve single-step tie resolution on near-converged blocks.
  const int bfs_reserve = std::min(max_orders / 2, std::max(4, max_orders / 3));
  const int seed_budget = std::max(1, max_orders - bfs_reserve);
  for (auto& [v, ord] : ranked_candidates) {
    if (static_cast<int>(out.size()) >= seed_budget) {
      break;
    }
    out.push_back(std::move(ord));
  }

  constexpr int kMaxTieFlipDepth = 3;
  constexpr int kMaxBfsVisits = 24;
  struct BfsState {
    absl::btree_set<std::pair<int, int>> extra_edges;
    int depth = 0;
  };
  std::deque<BfsState> worklist;
  absl::btree_set<absl::btree_set<std::pair<int, int>>> visited_edge_sets;
  visited_edge_sets.insert({});
  for (const auto& [u, v] : extract_ties(base_res, identity_rank, /*branch_limit=*/8)) {
    absl::btree_set<std::pair<int, int>> e = {{u, v}};
    if (visited_edge_sets.insert(e).second) {
      worklist.push_back(BfsState{.extra_edges = std::move(e), .depth = 1});
    }
  }

  int visits = 0;
  while (!worklist.empty() && static_cast<int>(out.size()) < max_orders && visits < kMaxBfsVisits) {
    BfsState st = std::move(worklist.front());
    worklist.pop_front();
    ++visits;

    absl::btree_set<std::pair<int, int>> edges = dag_edges;
    edges.insert(st.extra_edges.begin(), st.extra_edges.end());
    const std::optional<std::vector<int>> flipped_input =
        SolveSupernodeKahn(m, edges, hard_next, hard_prev, identity_rank);
    if (!flipped_input.has_value()) {
      continue;
    }
    std::vector<Insn> flipped_core;
    flipped_core.reserve(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      Insn insn = view.core_insns[static_cast<size_t>((*flipped_input)[static_cast<size_t>(pos)])];
      insn.luid = pos;
      flipped_core.push_back(std::move(insn));
    }
    const DepGraph g_flip = Analyze(flipped_core, view.facts_s1, mm, Phase::kSched1);
    const ScheduleResult res_flip =
        Schedule(flipped_core, g_flip, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
    if (static_cast<int>(res_flip.order.size()) != m) {
      continue;
    }
    std::vector<int> mapped(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      mapped[static_cast<size_t>(pos)] =
          (*flipped_input)[static_cast<size_t>(res_flip.order[static_cast<size_t>(pos)])];
    }
    if (seen_orders.insert(mapped).second) {
      out.push_back(std::move(mapped));
    }
    if (st.depth < kMaxTieFlipDepth && static_cast<int>(out.size()) < max_orders) {
      for (const auto& [u, v] : extract_ties(res_flip, *flipped_input, /*branch_limit=*/4)) {
        if (st.extra_edges.contains({u, v}) || st.extra_edges.contains({v, u})) {
          continue;
        }
        absl::btree_set<std::pair<int, int>> nxt_edges = st.extra_edges;
        nxt_edges.insert({u, v});
        if (visited_edge_sets.insert(nxt_edges).second) {
          worklist.push_back(BfsState{.extra_edges = std::move(nxt_edges), .depth = st.depth + 1});
        }
      }
    }
  }
  return out;
}

std::vector<std::vector<int>> CollectSched1SunkConsumerHoistCoreOrders(
    const Sched1ProjectionView& view, const MachineModel& mm, int max_orders) {
  std::vector<std::vector<int>> out;
  const int m = static_cast<int>(view.core_insns.size());
  if (m <= 2 || max_orders <= 0) {
    return out;
  }
  const DepGraph dg = Analyze(view.core_insns, view.facts_s1, mm, Phase::kSched1);
  const ScheduleResult sr =
      Schedule(view.core_insns, dg, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
  if (static_cast<int>(sr.order.size()) != m) {
    return out;
  }

  std::vector<int> sched_pos(static_cast<size_t>(m), -1);
  for (int pos = 0; pos < m; ++pos) {
    const int u = sr.order[static_cast<size_t>(pos)];
    if (u >= 0 && u < m) {
      sched_pos[static_cast<size_t>(u)] = pos;
    }
  }

  std::vector<int> min_true_cons(static_cast<size_t>(m), -1);
  std::vector<int> min_any_cons(static_cast<size_t>(m), -1);
  for (int c = 0; c < m; ++c) {
    for (const Link& lk : dg.log_links[static_cast<size_t>(c)]) {
      if (lk.target < 0 || lk.target >= c) {
        continue;
      }
      if (min_any_cons[static_cast<size_t>(lk.target)] < 0) {
        min_any_cons[static_cast<size_t>(lk.target)] = c;
      }
      if (lk.kind == DepKind::kTrue && min_true_cons[static_cast<size_t>(lk.target)] < 0) {
        min_true_cons[static_cast<size_t>(lk.target)] = c;
      }
    }
  }

  auto make_hoist_order = [&](int consumer, int target_pos) -> std::optional<std::vector<int>> {
    if (target_pos < 0 || target_pos >= consumer || consumer >= m) {
      return std::nullopt;
    }
    std::vector<bool> in_closure(static_cast<size_t>(m), false);
    in_closure[static_cast<size_t>(consumer)] = true;
    for (int u = consumer; u > target_pos; --u) {
      if (!in_closure[static_cast<size_t>(u)]) {
        continue;
      }
      for (const Link& lk : dg.log_links[static_cast<size_t>(u)]) {
        if (lk.target >= target_pos && lk.target < u) {
          in_closure[static_cast<size_t>(lk.target)] = true;
        }
      }
    }
    if (in_closure[static_cast<size_t>(target_pos)]) {
      return std::nullopt;
    }
    std::vector<int> ord;
    ord.reserve(static_cast<size_t>(m));
    for (int u = 0; u < target_pos; ++u) {
      ord.push_back(u);
    }
    for (int u = target_pos; u <= consumer; ++u) {
      if (in_closure[static_cast<size_t>(u)]) {
        ord.push_back(u);
      }
    }
    for (int u = target_pos; u <= consumer; ++u) {
      if (!in_closure[static_cast<size_t>(u)]) {
        ord.push_back(u);
      }
    }
    for (int u = consumer + 1; u < m; ++u) {
      ord.push_back(u);
    }
    return ord;
  };

  absl::btree_set<std::vector<int>> seen;
  auto emit = [&](int consumer, int target_pos) {
    if (static_cast<int>(out.size()) >= max_orders) {
      return;
    }
    std::optional<std::vector<int>> ord = make_hoist_order(consumer, target_pos);
    if (ord.has_value() && seen.insert(*ord).second) {
      out.push_back(std::move(*ord));
    }
  };

  for (int p = 0; p < m && static_cast<int>(out.size()) < max_orders; ++p) {
    const int sp = sched_pos[static_cast<size_t>(p)];
    if (sp < 0) {
      continue;
    }
    const int c_p = min_true_cons[static_cast<size_t>(p)] >= 0
                        ? min_true_cons[static_cast<size_t>(p)]
                        : min_any_cons[static_cast<size_t>(p)];
    if (c_p < 0) {
      continue;
    }
    int g_min = -1;
    int min_c_g = -1;
    for (int g = p + 1; g < m; ++g) {
      if (sched_pos[static_cast<size_t>(g)] >= 0 && sched_pos[static_cast<size_t>(g)] < sp) {
        if (g_min < 0) {
          g_min = g;
        }
        const int c_g = min_true_cons[static_cast<size_t>(g)] >= 0
                            ? min_true_cons[static_cast<size_t>(g)]
                            : min_any_cons[static_cast<size_t>(g)];
        if (c_g >= 0 && (min_c_g < 0 || c_g < min_c_g)) {
          min_c_g = c_g;
        }
      }
    }
    if (g_min < 0) {
      continue;
    }
    if (min_c_g >= 0 && c_p > min_c_g) {
      emit(c_p, min_c_g);
      if (g_min > p && g_min < min_c_g) {
        emit(c_p, g_min);
      }
    } else if (c_p > g_min) {
      emit(c_p, g_min);
    }
  }
  return out;
}

std::vector<std::vector<int>> CollectSched1704CallHoistCandidatePrefs(
    absl::Span<const Insn> cur_seq, int prologue_end, int epilogue_begin,
    const Sched1ProjectionView& cur_view, const RegFacts& facts_s2, const MachineModel& mm) {
  std::vector<std::vector<int>> prefs;
  const int n = static_cast<int>(cur_seq.size());
  const int m = static_cast<int>(cur_view.core_insns.size());
  if (n <= 2 || m <= 1 || prologue_end < 0 || epilogue_begin > n ||
      epilogue_begin <= prologue_end) {
    return prefs;
  }
  const std::vector<int> s1_order = ScheduleSched1ViewCoreOrder(cur_view, mm);
  if (static_cast<int>(s1_order.size()) != m) {
    return prefs;
  }
  std::vector<int> s1_pos(static_cast<size_t>(m), -1);
  for (int pos = 0; pos < m; ++pos) {
    s1_pos[static_cast<size_t>(s1_order[static_cast<size_t>(pos)])] = pos;
  }

  const DepGraph g_s2 = AnalyzeWithoutPendingListFlushCap(cur_seq, facts_s2, mm, Phase::kSched2);
  std::vector<int> call_seq_indices;
  for (int ci = 0; ci < m; ++ci) {
    if (cur_view.core_insns[static_cast<size_t>(ci)].kind == Insn::Kind::kCall) {
      call_seq_indices.push_back(prologue_end +
                                 cur_view.core_to_orig_index[static_cast<size_t>(ci)]);
    }
  }
  if (call_seq_indices.empty()) {
    return prefs;
  }

  // Maps call_seq_idx -> list of callee-saved insn seq indices to hoist immediately before it.
  absl::btree_map<int, std::vector<int>> displaced_by_call;
  absl::btree_map<int, std::vector<int>> all_by_call;
  for (int ci = 0; ci < m; ++ci) {
    const int seq_i = prologue_end + cur_view.core_to_orig_index[static_cast<size_t>(ci)];
    if (seq_i <= call_seq_indices.front() ||
        cur_view.core_insns[static_cast<size_t>(ci)].kind == Insn::Kind::kCall) {
      continue;
    }
    int prev_call = -1;
    for (int c_seq : call_seq_indices) {
      if (c_seq < seq_i) {
        prev_call = c_seq;
      } else {
        break;
      }
    }
    if (prev_call < 0 || !IsPureSched1704CallArtifactEdge(cur_seq, g_s2, prev_call, seq_i, mm)) {
      continue;
    }
    // Verify `seq_i` has no semantic `kSched2` dependency on `prev_call` or any
    // instruction in `(prev_call, seq_i)` (excluding `sched.c:1704` artifacts).
    bool has_post_call_dep = false;
    for (const Link& l : g_s2.log_links[static_cast<size_t>(seq_i)]) {
      if (l.target < prev_call || l.target >= seq_i) {
        continue;
      }
      if (l.target == prev_call &&
          IsPureSched1704CallArtifactEdge(cur_seq, g_s2, prev_call, seq_i, mm)) {
        continue;
      }
      has_post_call_dep = true;
      break;
    }
    if (has_post_call_dep) {
      continue;
    }
    all_by_call[prev_call].push_back(seq_i);
    if (s1_pos[static_cast<size_t>(ci)] != ci) {
      displaced_by_call[prev_call].push_back(seq_i);
    }
  }

  auto build_hoisted_pref = [&](const absl::btree_map<int, std::vector<int>>& by_call) {
    if (by_call.empty()) {
      return;
    }
    absl::btree_set<int> hoisted_set;
    for (const auto& [c_seq, items] : by_call) {
      for (int s : items) {
        hoisted_set.insert(s);
      }
    }
    std::vector<int> pref;
    pref.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      if (hoisted_set.contains(pos)) {
        continue;
      }
      if (const auto it = by_call.find(pos); it != by_call.end()) {
        for (int s : it->second) {
          pref.push_back(s);
        }
      }
      pref.push_back(pos);
    }
    if (static_cast<int>(pref.size()) == n &&
        std::find(prefs.begin(), prefs.end(), pref) == prefs.end()) {
      prefs.push_back(std::move(pref));
    }
  };
  constexpr int kMaxIndividualHoists = 6;
  int individual_emitted = 0;
  for (const auto& [c_seq, items] : displaced_by_call) {
    for (int s : items) {
      if (individual_emitted >= kMaxIndividualHoists) {
        break;
      }
      absl::btree_map<int, std::vector<int>> single;
      single[c_seq] = {s};
      build_hoisted_pref(single);
      ++individual_emitted;
    }
    if (individual_emitted >= kMaxIndividualHoists) {
      break;
    }
  }
  build_hoisted_pref(displaced_by_call);
  build_hoisted_pref(all_by_call);
  return prefs;
}

namespace {}  // namespace

namespace detail {

// Collects `REG_EQUIV (mem ...)` notes (`local-alloc.c:540-665, 990-1000, 1049-1055`)
// that were valid in the pre-`sched2` candidate order `cand_seq` (`mem_equiv_valid == 1`)
// but obstructed in the post-`sched2` order `base_block` (`mem_equiv_valid == 0`) by
// `sched2` reordering an independent store, load, or call across the live range.
std::vector<Stage0MemEquivAddition> CollectSched2ObstructedMemRegEquivAdditions(
    absl::Span<const Insn> base_block, absl::Span<const Insn> cand_seq,
    absl::Span<const int> cand_in_base, int prologue_end, int epilogue_begin,
    const absl::btree_set<int>& live_at_end_s2, const RegFacts& facts_s2, const MachineModel& mm,
    const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_prologue_body_indices,
    const absl::btree_set<int>& split_rmw_orig_ids) {
  std::vector<Stage0MemEquivAddition> additions;
  const int n = static_cast<int>(base_block.size());
  std::vector<int> base_ids(static_cast<size_t>(n));
  std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
  for (int i = 0; i < n; ++i) {
    base_ids[static_cast<size_t>(i)] = i;
    pos_in_cand[static_cast<size_t>(cand_in_base[static_cast<size_t>(i)])] = i;
  }
  absl::btree_set<int> excluded_prologue_orig_ids;
  for (int i = 0; i < prologue_end; ++i) {
    excluded_prologue_orig_ids.insert(i);
  }
  for (int rel : excluded_prologue_body_indices) {
    excluded_prologue_orig_ids.insert(prologue_end + rel);
  }

  // Identify matched intra-block local-frame spill/caller-save stack slots so we
  // never synthesize `REG_EQUIV` notes for `reload1.c`/`caller-save.c` spills.
  // Mirrors `reload1.c:2352-2358` + `function.c:682-704`: exclude local stack
  // arrays/aggregates below `min_valid_reload_spill_offset`.
  const int64_t min_spill_off =
      stack_layout.valid
          ? std::max(stack_layout.starting_frame_offset, stack_layout.min_valid_reload_spill_offset)
          : int64_t{0};
  absl::btree_map<StackSlotKey, int> first_store_pos;
  absl::btree_set<StackSlotKey> matched_spill_slots;
  for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
    const int oid = cand_in_base[static_cast<size_t>(pos)];
    if (excluded_prologue_orig_ids.contains(oid)) {
      continue;
    }
    if (auto acc = TryParsePlainStackSlotMove(cand_seq[static_cast<size_t>(pos)]);
        acc.has_value()) {
      const bool in_spill_frame =
          !stack_layout.valid ||
          (acc->key.offset >= min_spill_off && acc->key.offset < stack_layout.frame_size);
      if (!in_spill_frame) {
        continue;
      }
      if (acc->is_store) {
        first_store_pos.try_emplace(acc->key, pos);
      } else if (const auto it = first_store_pos.find(acc->key);
                 it != first_store_pos.end() && it->second < pos) {
        matched_spill_slots.insert(acc->key);
      }
    }
  }

  // Identify defs in `cand_seq` directly feeding a `CALL_INSN`'s `call_fusage`
  // (hard regs `< FIRST_PSEUDO_REGISTER` in `.08.lreg`, skipped by `local-alloc.c:1020`).
  std::vector<absl::btree_set<int>> call_arg_defs_in_cand(static_cast<size_t>(n));
  for (int ci = prologue_end; ci < epilogue_begin; ++ci) {
    if (cand_seq[static_cast<size_t>(ci)].kind != Insn::Kind::kCall) {
      continue;
    }
    absl::btree_set<int> needed;
    for (const RtlExpr& fu : cand_seq[static_cast<size_t>(ci)].pn.call_fusage) {
      CollectRegReads(fu, &needed);
    }
    for (int prev = ci - 1; prev >= prologue_end && !needed.empty(); --prev) {
      if (cand_seq[static_cast<size_t>(prev)].kind == Insn::Kind::kCall) {
        break;
      }
      const InsnRegReadDefDeaths info =
          CollectInsnRegReadDefDeaths(cand_seq[static_cast<size_t>(prev)], mm);
      for (int d : info.defs) {
        if (needed.contains(d)) {
          call_arg_defs_in_cand[static_cast<size_t>(prev)].insert(d);
          needed.erase(d);
        }
      }
    }
  }

  const HardRegLivenessSummary base_sum = ComputeHardRegLivenessSummary(
      base_block, base_ids, live_at_end_s2, facts_s2, mm, stack_layout, excluded_prologue_orig_ids);
  const HardRegLivenessSummary cand_sum =
      ComputeHardRegLivenessSummary(cand_seq, cand_in_base, live_at_end_s2, facts_s2, mm,
                                    stack_layout, excluded_prologue_orig_ids);

  auto is_allocatable_hard_reg = [&](int r) {
    return (r >= 2 && r <= 25) ||
           (r == kMipsFramePointerRegno && !stack_layout.frame_pointer_needed) ||
           (r >= kMipsFpRegFirst && r <= kMipsFpRegLast);
  };
  auto is_matched_spill_mem = [&](const Insn& insn) {
    if (auto acc = TryParsePlainStackSlotMove(insn); acc.has_value()) {
      return matched_spill_slots.contains(acc->key);
    }
    return false;
  };

  for (const auto& [oid, cv] : cand_sum.mem_equiv_valid) {
    if (!cv || oid < prologue_end || oid >= epilogue_begin ||
        excluded_prologue_orig_ids.contains(oid)) {
      continue;
    }
    const auto bit = base_sum.mem_equiv_valid.find(oid);
    const bool bv = (bit != base_sum.mem_equiv_valid.end()) && bit->second;
    const Insn& base_insn = base_block[static_cast<size_t>(oid)];
    const RtlExpr& pat = base_insn.pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || is_matched_spill_mem(base_insn)) {
      continue;
    }
    const int cpos = pos_in_cand[static_cast<size_t>(oid)];
    if (cpos < prologue_end || cpos >= epilogue_begin) {
      continue;
    }

    // (a) Load-equiv (`local-alloc.c:1049-1055`): `(set (reg R) (mem M))`.
    // `ValidateEquivMemForPostReloadGate` already requires `REG_DEAD R` inside
    // block `b`, proving this live range ends within the single basic block even
    // if hard reg `R` (e.g. `$v0`) is later redefined for a live-out value.
    if (pat.SetDest().op == RtlOp::kReg && !pat.SetDest().IsArgPointerReg() &&
        !pat.SetDest().flags.integrated && !pat.SetDest().flags.frame_related &&
        pat.SetSrc().op == RtlOp::kMem && !pat.SetSrc().children.empty() &&
        !pat.SetSrc().flags.volatil) {
      const int r = pat.SetDest().RegNo();
      bool death_is_split_rmw = false;
      if (bv && !split_rmw_orig_ids.empty()) {
        // When `SelectMinimalCombineRegsRmwSplits` splits a self-RMW terminating
        // `r`'s live range, `SynthesizePostReloadMemRegEquivNotes` previously
        // skipped `oid` as a multi-set pseudo even if `base_sum` scanned past
        // the unsplit RMW (`local-alloc.c:1021, 1049-1055`).
        for (int q = cpos + 1; q < epilogue_begin; ++q) {
          const int q_orig = cand_in_base[static_cast<size_t>(q)];
          if (excluded_prologue_orig_ids.contains(q_orig)) {
            continue;
          }
          if (HasRegDeadNoteCovering(cand_seq[static_cast<size_t>(q)].pn.reg_notes, r, mm)) {
            death_is_split_rmw = split_rmw_orig_ids.contains(q_orig);
            break;
          }
          if (CollectInsnRegReadDefDeaths(cand_seq[static_cast<size_t>(q)], mm).defs.contains(r)) {
            break;
          }
        }
      }
      if (bv && !death_is_split_rmw) {
        continue;
      }
      if (!is_allocatable_hard_reg(r) ||
          call_arg_defs_in_cand[static_cast<size_t>(cpos)].contains(r)) {
        continue;
      }
      bool has_existing = false;
      for (const RtlRegNote& note : base_insn.pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual ||
            note.kind == RtlMachineMode::kRegLibcall || note.kind == RtlMachineMode::kRegRetval) {
          has_existing = true;
          break;
        }
      }
      if (!has_existing) {
        additions.push_back(Stage0MemEquivAddition{
            .target_sched_idx = oid,
            .note =
                RtlRegNote{
                    .kind = RtlMachineMode::kRegEquiv,
                    .list_op = RtlOp::kExprList,
                    .datum = pat.SetSrc(),
                },
        });
      }
      continue;
    }

    // (b) Store-equiv (`local-alloc.c:990-1000`): `(set (mem M) (reg R))`.
    if (pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
        !pat.SetDest().flags.volatil && pat.SetSrc().op == RtlOp::kReg &&
        !pat.SetSrc().IsArgPointerReg() && !pat.SetSrc().flags.integrated) {
      const int r = pat.SetSrc().RegNo();
      if (!is_allocatable_hard_reg(r)) {
        continue;
      }
      const int init_pos = FindUniqueInitPosForStoreInSeq(cand_seq, cpos, r, mm);
      if (init_pos < prologue_end || init_pos >= cpos) {
        continue;
      }
      const int init_orig = cand_in_base[static_cast<size_t>(init_pos)];
      if (bv && !split_rmw_orig_ids.contains(init_orig)) {
        continue;
      }
      if (excluded_prologue_orig_ids.contains(init_orig) ||
          call_arg_defs_in_cand[static_cast<size_t>(init_pos)].contains(r)) {
        continue;
      }
      const Insn& init_insn = base_block[static_cast<size_t>(init_orig)];
      const RtlExpr& ipat = init_insn.pn.pattern;
      // `local-alloc.c:990` (`update_equiv_regs`) requires `GET_CODE (SET_SRC (set)) == REG`
      // on the store; when the store's source mode differs from the initializing `SET_DEST`
      // mode (e.g. `(reg:HI r)` stored from `(set (reg:SI r) ...)`), pre-reload RTL used a
      // `(subreg:HI (reg:SI P) 0)` (`alter_subreg`), so `local-alloc.c:990` never attaches
      // a store-`REG_EQUIV` note.
      if (ipat.op != RtlOp::kSet || ipat.children.size() != 2 || ipat.SetDest().op != RtlOp::kReg ||
          ipat.SetDest().mode != pat.SetSrc().mode || ipat.SetDest().flags.integrated ||
          ipat.SetDest().flags.frame_related) {
        continue;
      }
      if (HasMatchingMemRegEquivNote(init_insn, pat.SetDest(), stack_layout)) {
        continue;
      }
      additions.push_back(Stage0MemEquivAddition{
          .target_sched_idx = init_orig,
          .note =
              RtlRegNote{
                  .kind = RtlMachineMode::kRegEquiv,
                  .list_op = RtlOp::kExprList,
                  .datum = UneliminateMemForRegEquivNoteIfNeeded(pat.SetDest(), ipat, stack_layout),
              },
      });
    }
  }
  return additions;
}

}  // namespace detail

namespace {

// Reorders `base_block` by `cand_in_base`, and when `transfer_pure_reader_dead`
// is true (Stage 0 discovery), mirrors `AttributeSched2PureReaderRegDeadNotes`
// (`flow.c:2504-2515`, `sched.c:3415-3611`) by transferring pure-reader
// `(REG_DEAD r)` notes from `i` to the pre-`sched2` last reader `last_reader`
// (`last_reader < i` in `base_block` and `pos_in_cand[i] < pos_in_cand[last_reader]`)
// so `ProjectPostReloadBlockToSched1View` never projects stale `sched2`-order deaths.
std::vector<Insn> BuildTransferredCandSeqForStage0Projection(absl::Span<const Insn> base_block,
                                                             absl::Span<const int> cand_in_base,
                                                             int prologue_end, int epilogue_begin,
                                                             const MachineModel& mm,
                                                             bool transfer_pure_reader_dead) {
  const int n = static_cast<int>(base_block.size());
  std::vector<Insn> cand_seq;
  cand_seq.reserve(static_cast<size_t>(n));
  for (int pos = 0; pos < n; ++pos) {
    Insn insn = base_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
    insn.luid = pos;
    cand_seq.push_back(std::move(insn));
  }
  if (!transfer_pure_reader_dead || epilogue_begin <= prologue_end) {
    return cand_seq;
  }
  std::vector<InsnRegReadDefDeaths> insn_info(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    insn_info[static_cast<size_t>(i)] =
        CollectInsnRegReadDefDeaths(base_block[static_cast<size_t>(i)], mm);
  }
  std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
  for (int pos = 0; pos < n; ++pos) {
    pos_in_cand[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])] = pos;
  }
  struct PureTransfer {
    int from_pos = -1;
    size_t note_idx = 0;
    int to_pos = -1;
    RtlRegNote note;
  };
  std::vector<PureTransfer> transfers;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    if (insn_info[static_cast<size_t>(i)].is_call_or_use) {
      continue;
    }
    const auto& notes = base_block[static_cast<size_t>(i)].pn.reg_notes;
    for (size_t ni = 0; ni < notes.size(); ++ni) {
      const RtlRegNote& note = notes[ni];
      if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
        continue;
      }
      const int r = note.datum.RegNo();
      if (r < 0 || r >= kMipsFirstPseudoRegister) {
        continue;
      }
      const int nr = mm.HardRegNRegs(r, note.datum.mode);
      bool i_defs_r = false;
      for (int k = 0; k < nr; ++k) {
        if (insn_info[static_cast<size_t>(i)].defs.contains(r + k)) {
          i_defs_r = true;
          break;
        }
      }
      if (i_defs_r) {
        continue;
      }
      int last_reader = -1;
      for (int p = pos_in_cand[static_cast<size_t>(i)] + 1; p < epilogue_begin; ++p) {
        const int m = cand_in_base[static_cast<size_t>(p)];
        bool m_defs_r = false;
        for (int k = 0; k < nr; ++k) {
          if (insn_info[static_cast<size_t>(m)].defs.contains(r + k)) {
            m_defs_r = true;
            break;
          }
        }
        if (m_defs_r) {
          break;
        }
        bool m_reads_all = true;
        for (int k = 0; k < nr; ++k) {
          if (!insn_info[static_cast<size_t>(m)].reads.contains(r + k)) {
            m_reads_all = false;
            break;
          }
        }
        if (m_reads_all) {
          last_reader = m;
        }
      }
      if (last_reader < 0 || last_reader >= i ||
          insn_info[static_cast<size_t>(last_reader)].is_call_or_use) {
        continue;
      }
      bool already_dead = false;
      for (int k = 0; k < nr; ++k) {
        if (insn_info[static_cast<size_t>(last_reader)].deaths.contains(r + k)) {
          already_dead = true;
          break;
        }
      }
      if (already_dead) {
        continue;
      }
      bool intervening_def = false;
      const int lo = std::min(i, last_reader);
      const int hi = std::max(i, last_reader);
      for (int k = lo + 1; k < hi && !intervening_def; ++k) {
        for (int w = 0; w < nr; ++w) {
          if (insn_info[static_cast<size_t>(k)].defs.contains(r + w)) {
            intervening_def = true;
            break;
          }
        }
      }
      if (!intervening_def) {
        transfers.push_back(PureTransfer{
            .from_pos = pos_in_cand[static_cast<size_t>(i)],
            .note_idx = ni,
            .to_pos = pos_in_cand[static_cast<size_t>(last_reader)],
            .note = note,
        });
      }
    }
  }
  if (transfers.empty()) {
    return cand_seq;
  }
  absl::btree_map<int, absl::btree_set<size_t>> erase_by_pos;
  for (const PureTransfer& tr : transfers) {
    erase_by_pos[tr.from_pos].insert(tr.note_idx);
  }
  for (const auto& [from_pos, idxs] : erase_by_pos) {
    auto& notes = cand_seq[static_cast<size_t>(from_pos)].pn.reg_notes;
    std::vector<RtlRegNote> kept;
    kept.reserve(notes.size());
    for (size_t ni = 0; ni < notes.size(); ++ni) {
      if (!idxs.contains(ni)) {
        kept.push_back(std::move(notes[ni]));
      }
    }
    notes = std::move(kept);
  }
  for (const PureTransfer& tr : transfers) {
    cand_seq[static_cast<size_t>(tr.to_pos)].pn.reg_notes.push_back(tr.note);
  }
  return cand_seq;
}

}  // namespace

namespace detail {

// Runs bounded fixed-point discovery (`<= 4` rounds) for Stage 0 `sched2` preimage
// candidates under `allow_pure_reader_reg_dead_transfer=true`, optionally hoisting
// displaced block-0 entry-parameter copies (`sched.c:3191-3215`) on a second pass.
std::vector<std::vector<int>> FindStage0Sched2PreimageCandidates(
    absl::Span<const Insn> base_block, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionView& base_view, const Sched1ProjectionContext& proj_ctx,
    const CfgSched1Context& cfg_ctx, const RegFacts& facts_s2, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, absl::Span<const AdjacentPair> reload_adjacent,
    bool allow_pure_reader_reg_dead_transfer, bool allow_tie_flips,
    absl::Span<const int> initial_seed_in_base, bool allow_moved_store_load_equiv_invalidation) {
  std::vector<std::vector<int>> primary_candidates;
  std::vector<std::vector<int>> tie_candidates;
  const int n = static_cast<int>(base_block.size());
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  const bool fp_needed = !proj_ctx.stack_layout.valid || proj_ctx.stack_layout.frame_pointer_needed;
  constexpr int kMaxRounds = 4;

  const bool has_custom_seed = (static_cast<int>(initial_seed_in_base.size()) == n);
  Sched1ProjectionView seeded_view;
  absl::btree_set<int> seeded_excluded;
  if (has_custom_seed) {
    const std::vector<Insn> seed_seq = BuildTransferredCandSeqForStage0Projection(
        base_block, initial_seed_in_base, prologue_end, epilogue_begin, mm,
        allow_pure_reader_reg_dead_transfer);
    std::vector<Insn> seed_body;
    seed_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      Insn insn = seed_seq[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      seed_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext seed_ctx = proj_ctx;
    seeded_excluded = FindInterleavedPrologueSaveBodyIndices(seed_seq, bounds, block_idx, mm);
    seed_ctx.excluded_prologue_body_indices = seeded_excluded;
    seeded_view = ProjectPostReloadBlockToSched1View(seed_body, mm, live_at_end_s2, seed_ctx);
  }

  auto run_pass = [&](bool hoist_b0_args, bool run_tie_flips, bool use_closest_s1 = false) -> bool {
    std::vector<int> cur_to_base(static_cast<size_t>(n));
    if (has_custom_seed) {
      cur_to_base.assign(initial_seed_in_base.begin(), initial_seed_in_base.end());
    } else {
      for (int i = 0; i < n; ++i) {
        cur_to_base[static_cast<size_t>(i)] = i;
      }
    }
    Sched1ProjectionView cur_view = has_custom_seed ? seeded_view : base_view;
    absl::btree_set<int> cur_excluded =
        has_custom_seed ? seeded_excluded : proj_ctx.excluded_prologue_body_indices;
    absl::btree_set<std::vector<int>> tried_prefs;

    for (int round = 0; round < kMaxRounds; ++round) {
      std::vector<Insn> cur_seq = BuildTransferredCandSeqForStage0Projection(
          base_block, cur_to_base, prologue_end, epilogue_begin, mm,
          allow_pure_reader_reg_dead_transfer);
      std::vector<int> extra_pinned;
      if (hoist_b0_args && block_idx == 0) {
        std::vector<Insn> cur_body;
        cur_body.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          Insn insn = cur_seq[static_cast<size_t>(pos)];
          insn.luid = pos - prologue_end;
          cur_body.push_back(std::move(insn));
        }
        extra_pinned = CollectDisplacedBlock0EntryArgCopyBodyIndices(cur_body, cur_view,
                                                                     cur_excluded, mm, fp_needed);
        if (round == 0 && extra_pinned.empty()) {
          return false;
        }
      }
      std::vector<int> closest_s1;
      if (use_closest_s1) {
        closest_s1 = ScheduleClosestSched1CoreOrder(cur_view, mm);
      }
      std::vector<int> pref_in_cur;
      if (!BuildPreferredSched1OrderInBase(cur_seq, prologue_end, epilogue_begin, cur_view,
                                           cur_excluded, mm, &pref_in_cur, extra_pinned,
                                           closest_s1)) {
        break;
      }
      std::vector<int> pref_in_base(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        pref_in_base[static_cast<size_t>(pos)] =
            cur_to_base[static_cast<size_t>(pref_in_cur[static_cast<size_t>(pos)])];
      }
      if (!tried_prefs.insert(pref_in_base).second) {
        break;
      }
      const std::optional<std::vector<int>> cand_in_base = InvertBlockOrder(
          base_block, facts_s2, mm, Phase::kSched2, live_at_end_s2, pref_in_base, reload_adjacent);
      if (!cand_in_base.has_value()) {
        break;
      }
      bool differs = false;
      for (int pos = 0; pos < n; ++pos) {
        if ((*cand_in_base)[static_cast<size_t>(pos)] != pos) {
          differs = true;
          break;
        }
      }
      if (!differs) {
        break;
      }
      if (!PreservesPostReloadOrderSafety(base_block, *cand_in_base, prologue_end, epilogue_begin,
                                          live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                                          allow_pure_reader_reg_dead_transfer, cfg_ctx.stack_layout,
                                          proj_ctx.excluded_prologue_body_indices,
                                          allow_moved_store_load_equiv_invalidation)) {
        break;
      }
      if (!run_tie_flips) {
        primary_candidates.push_back(*cand_in_base);
      }

      std::vector<Insn> cand_seq = BuildTransferredCandSeqForStage0Projection(
          base_block, *cand_in_base, prologue_end, epilogue_begin, mm,
          allow_pure_reader_reg_dead_transfer);
      std::vector<Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        Insn insn = cand_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        cand_body.push_back(std::move(insn));
      }
      Sched1ProjectionContext cand_ctx = proj_ctx;
      cand_ctx.excluded_prologue_body_indices =
          FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, block_idx, mm);
      Sched1ProjectionView cand_view =
          ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
      if (cand_view.core_insns.size() <= 1 ||
          IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm, Phase::kSched1,
                       cand_view.live_at_end_s1)) {
        return true;
      }
      cur_to_base = *cand_in_base;
      cur_excluded = std::move(cand_ctx.excluded_prologue_body_indices);
      cur_view = std::move(cand_view);
    }

    if (!run_tie_flips) {
      return false;
    }

    // Bounded `sched.c:1704` callee-saved call-hoist and equal-priority `kSched1`
    // LUID tie-flip search (`sched.c:2428` `rank_for_schedule`) when the default
    // LUID tie-breaks did not reach `Im(F_sched1)`.
    std::vector<Insn> cur_seq = BuildTransferredCandSeqForStage0Projection(
        base_block, cur_to_base, prologue_end, epilogue_begin, mm,
        allow_pure_reader_reg_dead_transfer);
    std::vector<int> extra_pinned;
    if (hoist_b0_args && block_idx == 0) {
      std::vector<Insn> cur_body;
      cur_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        Insn insn = cur_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        cur_body.push_back(std::move(insn));
      }
      extra_pinned = CollectDisplacedBlock0EntryArgCopyBodyIndices(cur_body, cur_view, cur_excluded,
                                                                   mm, fp_needed);
    }

    auto count_s1_diffs = [&](const Sched1ProjectionView& v) -> int {
      const std::vector<int> ord = ScheduleSched1ViewCoreOrder(v, mm);
      if (ord.size() != v.core_insns.size()) {
        return static_cast<int>(v.core_insns.size());
      }
      int diffs = 0;
      for (int i = 0; i < static_cast<int>(ord.size()); ++i) {
        if (ord[static_cast<size_t>(i)] != i) {
          ++diffs;
        }
      }
      return diffs;
    };

    struct BestResidualState {
      int s1_diffs = std::numeric_limits<int>::max();
      std::vector<int> to_base;
      std::vector<Insn> seq;
      Sched1ProjectionView view;
      absl::btree_set<int> excluded;
      std::vector<int> extra_pinned;
    };
    const int cur_s1_diffs = count_s1_diffs(cur_view);
    BestResidualState best_residual;

    auto refine_from_initial_pref = [&](const std::vector<int>& init_pref_in_base,
                                        bool record_best) -> bool {
      std::vector<int> seed_to_base;
      std::vector<Insn> seed_seq;
      Sched1ProjectionView seed_view;
      absl::btree_set<int> seed_excluded;
      std::vector<int> seed_extra_pinned;
      std::vector<int> pref_in_base = init_pref_in_base;
      constexpr int kMaxSeedRefineRounds = 4;
      for (int sround = 0; sround < kMaxSeedRefineRounds; ++sround) {
        if (sround > 0) {
          std::vector<int> pref_in_cur;
          if (!BuildPreferredSched1OrderInBase(seed_seq, prologue_end, epilogue_begin, seed_view,
                                               seed_excluded, mm, &pref_in_cur,
                                               seed_extra_pinned)) {
            break;
          }
          pref_in_base.resize(static_cast<size_t>(n));
          for (int pos = 0; pos < n; ++pos) {
            pref_in_base[static_cast<size_t>(pos)] =
                seed_to_base[static_cast<size_t>(pref_in_cur[static_cast<size_t>(pos)])];
          }
        }
        if (!tried_prefs.insert(pref_in_base).second) {
          break;
        }
        const std::optional<std::vector<int>> cand_in_base =
            InvertBlockOrder(base_block, facts_s2, mm, Phase::kSched2, live_at_end_s2, pref_in_base,
                             reload_adjacent);
        if (!cand_in_base.has_value()) {
          break;
        }
        bool differs = false;
        for (int pos = 0; pos < n; ++pos) {
          if ((*cand_in_base)[static_cast<size_t>(pos)] != pos) {
            differs = true;
            break;
          }
        }
        const bool ok_safe =
            differs &&
            PreservesPostReloadOrderSafety(
                base_block, *cand_in_base, prologue_end, epilogue_begin, live_at_end_s2,
                cfg_ctx.cfg_copy_pairs, facts_s2, mm, allow_pure_reader_reg_dead_transfer,
                cfg_ctx.stack_layout, proj_ctx.excluded_prologue_body_indices,
                allow_moved_store_load_equiv_invalidation);
        if (!ok_safe) {
          break;
        }
        tie_candidates.push_back(*cand_in_base);
        std::vector<Insn> cand_seq = BuildTransferredCandSeqForStage0Projection(
            base_block, *cand_in_base, prologue_end, epilogue_begin, mm,
            allow_pure_reader_reg_dead_transfer);
        std::vector<Insn> cand_body;
        cand_body.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          Insn insn = cand_seq[static_cast<size_t>(pos)];
          insn.luid = pos - prologue_end;
          cand_body.push_back(std::move(insn));
        }
        Sched1ProjectionContext cand_ctx = proj_ctx;
        cand_ctx.excluded_prologue_body_indices =
            FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, block_idx, mm);
        Sched1ProjectionView cand_view =
            ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
        const bool is_ach = cand_view.core_insns.size() <= 1 ||
                            IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                         Phase::kSched1, cand_view.live_at_end_s1);
        if (is_ach) {
          return true;
        }
        seed_to_base = *cand_in_base;
        seed_seq = std::move(cand_seq);
        seed_excluded = std::move(cand_ctx.excluded_prologue_body_indices);
        seed_view = std::move(cand_view);
        if (hoist_b0_args && block_idx == 0) {
          seed_extra_pinned = CollectDisplacedBlock0EntryArgCopyBodyIndices(
              cand_body, seed_view, seed_excluded, mm, fp_needed);
        }
        if (record_best) {
          const int nd = count_s1_diffs(seed_view);
          if (nd > 0 && nd < best_residual.s1_diffs) {
            best_residual.s1_diffs = nd;
            best_residual.to_base = seed_to_base;
            best_residual.seq = seed_seq;
            best_residual.view = seed_view;
            best_residual.excluded = seed_excluded;
            best_residual.extra_pinned = seed_extra_pinned;
          }
        }
      }
      return false;
    };

    // (a) Hoist callee-saved insns sunk across a call (`sched.c:1704, 2235`).
    for (const std::vector<int>& hoist_in_cur : CollectSched1704CallHoistCandidatePrefs(
             cur_seq, prologue_end, epilogue_begin, cur_view, facts_s2, mm)) {
      std::vector<int> hoist_in_base(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        hoist_in_base[static_cast<size_t>(pos)] =
            cur_to_base[static_cast<size_t>(hoist_in_cur[static_cast<size_t>(pos)])];
      }
      if (refine_from_initial_pref(hoist_in_base, /*record_best=*/true)) {
        return true;
      }
    }

    auto run_tie_flips_on_state =
        [&](const std::vector<int>& st_to_base, const std::vector<Insn>& st_seq,
            const Sched1ProjectionView& st_view, const absl::btree_set<int>& st_excluded,
            const std::vector<int>& st_extra_pinned, int max_orders, bool record_best) -> bool {
      const std::vector<std::pair<int, int>> core_s2_edges =
          ExtractCoreSched2Edges(st_seq, prologue_end, epilogue_begin, st_view, facts_s2, mm);
      const std::vector<std::pair<int, int>> core_s2_replay_edges = ExtractCoreSched2ReplayEdges(
          base_block, st_to_base, prologue_end, epilogue_begin, st_view, facts_s2, mm,
          live_at_end_s2, /*preserve_reg_dead_reader_order=*/!allow_pure_reader_reg_dead_transfer);
      const std::vector<std::vector<int>> tie_flip_orders = CollectSched1TieFlipCoreOrders(
          st_view, mm, max_orders, core_s2_edges, core_s2_replay_edges);
      for (const std::vector<int>& alt_s1_order : tie_flip_orders) {
        std::vector<int> pref_in_cur;
        if (!BuildPreferredSched1OrderInBase(st_seq, prologue_end, epilogue_begin, st_view,
                                             st_excluded, mm, &pref_in_cur, st_extra_pinned,
                                             alt_s1_order)) {
          continue;
        }
        std::vector<int> pref_in_base(static_cast<size_t>(n));
        for (int pos = 0; pos < n; ++pos) {
          pref_in_base[static_cast<size_t>(pos)] =
              st_to_base[static_cast<size_t>(pref_in_cur[static_cast<size_t>(pos)])];
        }
        if (refine_from_initial_pref(pref_in_base, record_best)) {
          return true;
        }
      }
      return false;
    };

    // (b) Tie-flip search on the default-LUID fixed point `cur_view`.
    if (run_tie_flips_on_state(cur_to_base, cur_seq, cur_view, cur_excluded, extra_pinned,
                               /*max_orders=*/24, /*record_best=*/true)) {
      return true;
    }

    // (c) Also try tie-flips starting from original `base_view` when default-LUID
    //     fixed-point rounds altered consumer ordering (`expr.c:4027`, `sched.c:2428`).
    bool cur_differs_from_base = false;
    for (int pos = 0; pos < n; ++pos) {
      if (cur_to_base[static_cast<size_t>(pos)] != pos) {
        cur_differs_from_base = true;
        break;
      }
    }
    if (cur_differs_from_base) {
      std::vector<int> id_to_base(static_cast<size_t>(n));
      std::vector<Insn> id_seq;
      id_seq.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        id_to_base[static_cast<size_t>(pos)] = pos;
        Insn insn = base_block[static_cast<size_t>(pos)];
        insn.luid = pos;
        id_seq.push_back(std::move(insn));
      }
      std::vector<int> base_extra_pinned;
      if (hoist_b0_args && block_idx == 0) {
        std::vector<Insn> base_body_only;
        base_body_only.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          Insn insn = id_seq[static_cast<size_t>(pos)];
          insn.luid = pos - prologue_end;
          base_body_only.push_back(std::move(insn));
        }
        base_extra_pinned = CollectDisplacedBlock0EntryArgCopyBodyIndices(
            base_body_only, base_view, proj_ctx.excluded_prologue_body_indices, mm, fp_needed);
      }
      if (run_tie_flips_on_state(id_to_base, id_seq, base_view,
                                 proj_ctx.excluded_prologue_body_indices, base_extra_pinned,
                                 /*max_orders=*/24, /*record_best=*/true)) {
        return true;
      }
    }

    auto run_sunk_consumer_hoists_on_state =
        [&](const std::vector<int>& st_to_base, const std::vector<Insn>& st_seq,
            const Sched1ProjectionView& st_view, const absl::btree_set<int>& st_excluded,
            const std::vector<int>& st_extra_pinned, int max_orders, bool record_best) -> bool {
      const std::vector<std::vector<int>> hoist_orders =
          CollectSched1SunkConsumerHoistCoreOrders(st_view, mm, max_orders);
      for (const std::vector<int>& alt_s1_order : hoist_orders) {
        std::vector<int> pref_in_cur;
        if (!BuildPreferredSched1OrderInBase(st_seq, prologue_end, epilogue_begin, st_view,
                                             st_excluded, mm, &pref_in_cur, st_extra_pinned,
                                             alt_s1_order)) {
          continue;
        }
        std::vector<int> pref_in_base(static_cast<size_t>(n));
        for (int pos = 0; pos < n; ++pos) {
          pref_in_base[static_cast<size_t>(pos)] =
              st_to_base[static_cast<size_t>(pref_in_cur[static_cast<size_t>(pos)])];
        }
        if (refine_from_initial_pref(pref_in_base, record_best)) {
          return true;
        }
      }
      return false;
    };

    // (d) Undo `sched2` consumer-sinking and run one bounded nested tie-flip
    //     pass on the single strictly-best residual state.
    if (best_residual.s1_diffs < cur_s1_diffs && !best_residual.to_base.empty()) {
      const BestResidualState snap1 = best_residual;
      if (run_sunk_consumer_hoists_on_state(snap1.to_base, snap1.seq, snap1.view, snap1.excluded,
                                            snap1.extra_pinned, /*max_orders=*/12,
                                            /*record_best=*/true)) {
        return true;
      }
      const BestResidualState snap2 = best_residual;
      if (run_tie_flips_on_state(snap2.to_base, snap2.seq, snap2.view, snap2.excluded,
                                 snap2.extra_pinned, /*max_orders=*/12, /*record_best=*/true)) {
        return true;
      }
      if (best_residual.s1_diffs < snap2.s1_diffs && !best_residual.to_base.empty()) {
        const BestResidualState snap3 = best_residual;
        if (run_sunk_consumer_hoists_on_state(snap3.to_base, snap3.seq, snap3.view, snap3.excluded,
                                              snap3.extra_pinned, /*max_orders=*/12,
                                              /*record_best=*/false)) {
          return true;
        }
      }
    } else if (cur_s1_diffs > 0) {
      if (run_sunk_consumer_hoists_on_state(cur_to_base, cur_seq, cur_view, cur_excluded,
                                            extra_pinned, /*max_orders=*/12,
                                            /*record_best=*/false)) {
        return true;
      }
    }
    return false;
  };

  // Tier 1: default-LUID fixed-point search (`hoist=false`, then block-0 `hoist=true`).
  bool primary_solved = run_pass(/*hoist_b0_args=*/false, /*run_tie_flips=*/false);
  if (block_idx == 0) {
    primary_solved = run_pass(/*hoist_b0_args=*/true, /*run_tie_flips=*/false) || primary_solved;
  }
  // Tier 1b: minimal-edit `ScheduleClosestSched1CoreOrder` fixed-point search
  // preserving already-achievable equal-priority prefixes/suffixes.
  if (!primary_solved) {
    primary_solved =
        run_pass(/*hoist_b0_args=*/false, /*run_tie_flips=*/false, /*use_closest_s1=*/true);
    if (!primary_solved && block_idx == 0) {
      primary_solved =
          run_pass(/*hoist_b0_args=*/true, /*run_tie_flips=*/false, /*use_closest_s1=*/true);
    }
  }
  // Tier 2: bounded equal-priority LUID tie-flip fallback, ordered ahead of
  // `primary_candidates` so reverse iteration (`rbegin()`) always tries every
  // primary default-LUID candidate before any tie-flip candidate.
  if (!primary_solved && allow_tie_flips) {
    if (!run_pass(/*hoist_b0_args=*/false, /*run_tie_flips=*/true) && block_idx == 0) {
      (void)run_pass(/*hoist_b0_args=*/true, /*run_tie_flips=*/true);
    }
  }
  std::vector<std::vector<int>> candidates;
  candidates.reserve(tie_candidates.size() + primary_candidates.size());
  for (std::vector<int>& tc : tie_candidates) {
    candidates.push_back(std::move(tc));
  }
  for (std::vector<int>& pc : primary_candidates) {
    candidates.push_back(std::move(pc));
  }
  return candidates;
}

}  // namespace detail

bool HasSched1AchievablePreimageForSched2Block(
    absl::Span<const Insn> cand_sched, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionView& cand_view, const Sched1ProjectionContext& proj_ctx,
    const CfgSched1Context& cfg_ctx, const RegFacts& facts_s2, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, bool allow_pure_reader_reg_dead_transfer,
    bool allow_tie_flips) {
  if (cand_view.core_insns.size() <= 1 || IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                                       Phase::kSched1, cand_view.live_at_end_s1)) {
    return true;
  }
  const int n = static_cast<int>(cand_sched.size());
  const int body_len = bounds.epilogue_begin - bounds.prologue_end;
  if (n <= 2 || body_len <= 1) {
    return false;
  }
  std::vector<AdjacentPair> reload_adjacent;
  for (int i = 0; i + 1 < n; ++i) {
    if (IsAdjacentMdOutputReloadPair(cand_sched[static_cast<size_t>(i)],
                                     cand_sched[static_cast<size_t>(i + 1)]) ||
        IsAdjacentReloadContiguityPair(cand_sched[static_cast<size_t>(i)],
                                       cand_sched[static_cast<size_t>(i + 1)], mm,
                                       cfg_ctx.stack_layout)) {
      reload_adjacent.push_back(AdjacentPair{.first = i, .second = i + 1});
    }
  }
  const std::vector<std::vector<int>> candidates = FindStage0Sched2PreimageCandidates(
      cand_sched, block_idx, bounds, cand_view, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2,
      reload_adjacent, allow_pure_reader_reg_dead_transfer, allow_tie_flips);
  for (const std::vector<int>& cand_in_base : candidates) {
    std::vector<Insn> pre_seq = BuildTransferredCandSeqForStage0Projection(
        cand_sched, cand_in_base, bounds.prologue_end, bounds.epilogue_begin, mm,
        allow_pure_reader_reg_dead_transfer);
    std::vector<Insn> pre_body;
    pre_body.reserve(static_cast<size_t>(body_len));
    for (int pos = bounds.prologue_end; pos < bounds.epilogue_begin; ++pos) {
      Insn insn = pre_seq[static_cast<size_t>(pos)];
      insn.luid = pos - bounds.prologue_end;
      pre_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext pre_ctx = proj_ctx;
    pre_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(pre_seq, bounds, block_idx, mm);
    const Sched1ProjectionView pre_view =
        ProjectPostReloadBlockToSched1View(pre_body, mm, live_at_end_s2, pre_ctx);
    if (pre_view.core_insns.size() <= 1 || IsAchievable(pre_view.core_insns, pre_view.facts_s1, mm,
                                                        Phase::kSched1, pre_view.live_at_end_s1)) {
      return true;
    }
  }
  return false;
}

}  // namespace rom_nom_nom::lifter::sched_model
