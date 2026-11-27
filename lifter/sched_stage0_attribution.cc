#include "lifter/sched_stage0_attribution.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/rtl_liveness.h"
#include "lifter/sched_model.h"
#include "lifter/sched_post_reload_projection.h"

namespace rom_nom_nom::lifter::sched_model {

using detail::BuildPreferredSched1OrderInBase;
using detail::ClassifyInsnKind;
using detail::CollectFullRegDefs;
using detail::CollectInsnRegReadDefDeaths;
using detail::CollectRegReads;
using detail::CollectSched2ObstructedMemRegEquivAdditions;
using detail::FindStage0Sched2PreimageCandidates;
using detail::FindUniqueInitPosForStoreInSeq;
using detail::HasLoadEquivInvalidatorMovedIntoWindow;
using detail::HasMatchingMemRegEquivNote;
using detail::HasRegDeadNoteCovering;
using detail::HasStoreEquivInvalidatorMovedIntoWindow;
using detail::InsnRegReadDefDeaths;
using detail::IsAbiCallInterfaceHardReg;
using detail::IsCrossClassFprToGprCopyReloadInheritedRead;
using detail::kMipsArgRegFirst;
using detail::kMipsArgRegLast;
using detail::kMipsFpArgRegFirst;
using detail::kMipsFpArgRegLast;
using detail::kMipsFpRegFirst;
using detail::kMipsFpRegLast;
using detail::kMipsFramePointerRegno;
using detail::kMipsReturnAddrRegno;
using detail::kMipsStackPointerRegno;
using detail::MemrefUsedBetweenForPostReloadGate;
using detail::ModeByteSize;
using detail::NormalizeMemForRegEquivMatch;
using detail::PatternDefinesHardRegSpan;
using detail::PatternReadsHardRegSpan;
using detail::SameAddrIgnoringRegNames;
using detail::SameMemSlotIgnoringRegNames;
using detail::SingleSetOf;
using detail::StackSlotAccess;
using detail::StackSlotKey;
using detail::Stage0MemEquivAddition;
using detail::TryExtractPlainStackSlotOffset;
using detail::TryMatchEarlyclobberDiOutputReload;
using detail::TryParsePlainStackSlotMove;
using detail::ValidateEquivMemForPostReloadGate;

namespace {

bool IsReturnJumpPatternForAttribution(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  return pat.op == RtlOp::kParallel && !pat.children.empty() &&
         pat.children[0].op == RtlOp::kReturn;
}

bool IsSimpleNonControlDelayInsn(const RtlExpr& pat) {
  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kCodeLabel || pat.op == RtlOp::kBarrier ||
      pat.op == RtlOp::kReturn || pat.op == RtlOp::kSequence) {
    return false;
  }
  if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kPc) {
    return false;
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
    const RtlExpr& c0 = pat.children[0];
    if (c0.op == RtlOp::kReturn ||
        (c0.op == RtlOp::kSet && c0.children.size() == 2 && c0.SetDest().op == RtlOp::kPc)) {
      return false;
    }
  }
  return true;
}

std::optional<RtlExpr> FindUsedHardRegExprInPattern(const RtlExpr& root, int target_regno) {
  std::optional<RtlExpr> found;
  auto walk = [&](auto& self, const RtlExpr& e, bool is_set_lhs) -> void {
    if (found.has_value()) {
      return;
    }
    if (e.op == RtlOp::kSet && e.children.size() == 2) {
      self(self, e.SetSrc(), false);
      if (e.SetDest().op == RtlOp::kMem && !e.SetDest().children.empty()) {
        self(self, e.SetDest().children[0], false);
      } else {
        self(self, e.SetDest(), true);
      }
      return;
    }
    if (e.op == RtlOp::kClobber) {
      return;
    }
    if (e.op == RtlOp::kReg) {
      if (!is_set_lhs && e.RegNo() == target_regno) {
        found = e;
      }
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch, false);
    }
  };
  walk(walk, root, false);
  return found;
}

struct Stage0OriginLoc {
  size_t orig_insn_idx = 0;
  bool is_delay_slot = false;
  bool had_placeholder_dead = false;
  bool is_epilogue_delay = false;
  bool is_synthetic = false;
  // Temporary `(REG_DEAD arg_reg)` notes attached to this entry-param copy
  // inside `view_cfg` only to mirror `jump.c:434-465` deleted pre-call arg
  // moves; stored per-origin-loc so view-index shifts never leak them back.
  absl::btree_set<int> synthetic_added_dead_regs;
};

bool IsSpRaSavePatternForDelayView(const RtlExpr& pat) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kMem ||
      pat.SetDest().children.empty() || pat.SetSrc().op != RtlOp::kReg ||
      pat.SetSrc().RegNo() != kMipsReturnAddrRegno) {
    return false;
  }
  const RtlExpr& addr = pat.SetDest().children[0];
  return (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
         (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
          addr.children[0].op == RtlOp::kReg &&
          addr.children[0].RegNo() == kMipsStackPointerRegno &&
          addr.children[1].op == RtlOp::kConstInt);
}

bool SlotCanPrecedeRaSaveForDelayView(const RtlExpr& pat) {
  bool writes_mem = false;
  auto check_set = [&](const RtlExpr& elem) {
    if (elem.op == RtlOp::kSet && elem.children.size() == 2 && elem.SetDest().op == RtlOp::kMem) {
      writes_mem = true;
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      check_set(ch);
    }
  } else {
    check_set(pat);
  }
  if (writes_mem) {
    return false;
  }
  absl::btree_set<int> fdefs;
  absl::btree_set<int> pdefs;
  absl::btree_set<int> reads;
  CollectFullRegDefs(pat, &fdefs, &pdefs);
  CollectRegReads(pat, &reads);
  return !fdefs.contains(kMipsStackPointerRegno) && !pdefs.contains(kMipsStackPointerRegno) &&
         !fdefs.contains(kMipsReturnAddrRegno) && !pdefs.contains(kMipsReturnAddrRegno) &&
         !reads.contains(kMipsReturnAddrRegno);
}

struct Stage0SeqInfo {
  RtlExpr head_wrap;
  RtlExpr slot_wrap;
  RtlPatternAndNotes slot_pn;
};

struct Stage0BlockDelayView {
  bool eligible = true;
  std::vector<Stage0OriginLoc> view_to_orig;
  absl::btree_map<size_t, Stage0SeqInfo> seqs;
  absl::btree_set<int> annulled_slot_live_at_end;
  std::optional<size_t> unpacked_non_return_jump_slot_view_idx;
  absl::btree_set<size_t> alignment_touched_view_idxs;
};

void CommitStage0AttributedNotesToBlock(
    const ParsedSchedRegion& base_region, const std::vector<Insn>& updated_base,
    const absl::btree_set<int>& touched_sched_idxs, Stage0BlockDelayView* block_view,
    RtlBasicBlock* orig_blk, std::optional<std::string>* epilogue_delay_pattern = nullptr) {
  const size_t orig_len = orig_blk->instructions.size();
  std::vector<bool> saved_has_fusage(orig_len, false);
  std::vector<std::vector<RtlExpr>> saved_fusage(orig_len);
  for (size_t i = 0; i < orig_len && i < orig_blk->rtl_insns.size(); ++i) {
    if (orig_blk->rtl_insns[i].has_explicit_call_fusage) {
      saved_has_fusage[i] = true;
      saved_fusage[i] = orig_blk->rtl_insns[i].call_fusage;
    }
  }
  auto filter_synthetic_dead_notes = [&](const Stage0OriginLoc& loc,
                                         const std::vector<RtlRegNote>& notes) {
    if (loc.synthetic_added_dead_regs.empty()) {
      return notes;
    }
    std::vector<RtlRegNote> filtered;
    filtered.reserve(notes.size());
    for (const RtlRegNote& n : notes) {
      if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
          loc.synthetic_added_dead_regs.contains(n.datum.RegNo())) {
        continue;
      }
      filtered.push_back(n);
    }
    return filtered;
  };
  for (int si : touched_sched_idxs) {
    const size_t view_idx = base_region.sched_orig_indices[static_cast<size_t>(si)];
    const Stage0OriginLoc& loc = block_view->view_to_orig[view_idx];
    if (loc.is_synthetic) {
      continue;
    }
    if (loc.is_epilogue_delay) {
      if (epilogue_delay_pattern != nullptr && epilogue_delay_pattern->has_value()) {
        const RtlPatternAndNotes& upd = updated_base[static_cast<size_t>(si)].pn;
        *epilogue_delay_pattern = FormatGccPrintRtlPatternAndNotes(
            upd.pattern, filter_synthetic_dead_notes(loc, upd.reg_notes), upd.bracketed_notes,
            /*call_fusage=*/{});
      }
    } else if (loc.is_delay_slot) {
      auto seq_it = block_view->seqs.find(loc.orig_insn_idx);
      if (seq_it != block_view->seqs.end()) {
        seq_it->second.slot_pn.reg_notes =
            filter_synthetic_dead_notes(loc, updated_base[static_cast<size_t>(si)].pn.reg_notes);
        orig_blk->instructions[loc.orig_insn_idx] =
            RtlExpr::MakeSequence({
                                      seq_it->second.head_wrap,
                                      RtlExpr::MakeInsnWrapper(seq_it->second.slot_wrap.op,
                                                               seq_it->second.slot_wrap.flags,
                                                               seq_it->second.slot_pn),
                                  })
                .ToGccPrintRtlString();
      }
    } else {
      const RtlPatternAndNotes& upd = updated_base[static_cast<size_t>(si)].pn;
      orig_blk->instructions[loc.orig_insn_idx] = FormatGccPrintRtlPatternAndNotes(
          upd.pattern, filter_synthetic_dead_notes(loc, upd.reg_notes), upd.bracketed_notes,
          /*call_fusage=*/{});
    }
  }
  orig_blk->SyncRtlInsnsFromInstructions();
  for (size_t i = 0; i < orig_len && i < orig_blk->rtl_insns.size(); ++i) {
    if (saved_has_fusage[i] && !orig_blk->rtl_insns[i].has_explicit_call_fusage) {
      orig_blk->rtl_insns[i].has_explicit_call_fusage = true;
      orig_blk->rtl_insns[i].call_fusage = std::move(saved_fusage[i]);
    }
  }
}

// Returns the subset of general `local-alloc.c:1266-1300, 1851-1886`
// (`combine_regs`) RMW split candidates in `body` whose destination is a
// call-used hard register (`mm.CallUsed(r)`). Call-used hard registers never
// hold call-crossing user accumulators (`local-alloc.c:2106`, `global.c:927`),
// so separating them from callee-saved multi-set accumulators prevents spurious
// splits from distorting `sched.c:2490` (`birthing_insn_p`) priorities.
absl::btree_set<int> CollectCallUsedGeneralCombineRegsRmwSplits(
    absl::Span<const Insn> body, const Sched1ProjectionContext& base_ctx,
    const absl::btree_set<int>& live_at_end_s2, const MachineModel& mm) {
  Sched1ProjectionContext all_ctx = base_ctx;
  all_ctx.allow_post_call_combine_regs_split = true;
  all_ctx.allow_general_combine_regs_split = true;
  all_ctx.forced_combine_regs_split_body_indices.clear();
  const Sched1ProjectionView all_view =
      ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, all_ctx);
  absl::btree_set<int> call_used_splits;
  for (int body_idx : all_view.implicit_combine_regs_split_orig_indices) {
    if (body_idx < 0 || body_idx >= static_cast<int>(body.size())) {
      continue;
    }
    const RtlExpr* s = SingleSetOf(body[static_cast<size_t>(body_idx)].pn.pattern);
    if (s != nullptr && s->SetDest().op == RtlOp::kReg && s->SetDest().RegNo() >= 0 &&
        s->SetDest().RegNo() < kMipsFirstPseudoRegister && mm.CallUsed(s->SetDest().RegNo())) {
      call_used_splits.insert(body_idx);
    }
  }
  return call_used_splits;
}

// Finds a minimal subset of block-local `local-alloc.c:1266-1300, 1851-1886`
// (`combine_regs`) RMW body indices whose implicit `(REG_DEAD r)` splits
// restore `Phase::kSched1` achievability (`sched.c:1732`, `sched.c:2490`) on a
// body that is otherwise non-achievable without those splits. Returns empty if
// `body` is already achievable without splitting or cannot be made achievable.
absl::btree_set<int> SelectMinimalCombineRegsRmwSplits(absl::Span<const Insn> body,
                                                       const Sched1ProjectionContext& base_ctx,
                                                       const absl::btree_set<int>& live_at_end_s2,
                                                       const MachineModel& mm,
                                                       bool allow_general = true) {
  Sched1ProjectionContext unsplit_ctx = base_ctx;
  unsplit_ctx.allow_post_call_combine_regs_split = false;
  unsplit_ctx.allow_call_used_general_combine_regs_split = false;
  unsplit_ctx.allow_general_combine_regs_split = false;
  unsplit_ctx.forced_combine_regs_split_body_indices.clear();
  const Sched1ProjectionView unsplit_view =
      ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, unsplit_ctx);
  if (unsplit_view.core_insns.size() <= 1 ||
      IsAchievable(unsplit_view.core_insns, unsplit_view.facts_s1, mm, Phase::kSched1,
                   unsplit_view.live_at_end_s1)) {
    return {};
  }

  auto shrink_minimal = [&](absl::btree_set<int> kept) -> absl::btree_set<int> {
    const std::vector<int> cands(kept.rbegin(), kept.rend());
    for (int idx : cands) {
      if (kept.size() <= 1) {
        break;
      }
      Sched1ProjectionContext trial_ctx = unsplit_ctx;
      trial_ctx.forced_combine_regs_split_body_indices = kept;
      trial_ctx.forced_combine_regs_split_body_indices.erase(idx);
      const Sched1ProjectionView trial_view =
          ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, trial_ctx);
      if (trial_view.core_insns.size() > 1 &&
          IsAchievable(trial_view.core_insns, trial_view.facts_s1, mm, Phase::kSched1,
                       trial_view.live_at_end_s1)) {
        kept.erase(idx);
      }
    }
    return kept;
  };

  // Prefer the narrower post-call `combine_regs` split set first
  // (`sched.c:1732`, `local-alloc.c:1266-1300`), then call-used general RMWs
  // (excluding callee-saved accumulators), then all general RMWs, before
  // falling back to bounded forward greedy addition when splitting an unrelated
  // callee-saved accumulator breaks `IsAchievable` (`sched.c:2490`).
  absl::btree_set<int> all_general_cands;
  for (bool try_general : {false, true}) {
    if (try_general && !allow_general) {
      break;
    }
    Sched1ProjectionContext all_ctx = base_ctx;
    all_ctx.allow_post_call_combine_regs_split = true;
    all_ctx.allow_call_used_general_combine_regs_split = false;
    all_ctx.allow_general_combine_regs_split = try_general;
    all_ctx.forced_combine_regs_split_body_indices.clear();
    const Sched1ProjectionView candidate_view =
        ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, all_ctx);
    if (try_general) {
      all_general_cands.insert(candidate_view.implicit_combine_regs_split_orig_indices.begin(),
                               candidate_view.implicit_combine_regs_split_orig_indices.end());
    }
    if (candidate_view.core_insns.size() > 1 &&
        !candidate_view.implicit_combine_regs_split_orig_indices.empty() &&
        IsAchievable(candidate_view.core_insns, candidate_view.facts_s1, mm, Phase::kSched1,
                     candidate_view.live_at_end_s1)) {
      return shrink_minimal(
          absl::btree_set<int>(candidate_view.implicit_combine_regs_split_orig_indices.begin(),
                               candidate_view.implicit_combine_regs_split_orig_indices.end()));
    }
  }
  if (!allow_general || all_general_cands.empty()) {
    return {};
  }

  const absl::btree_set<int> call_used_cands =
      CollectCallUsedGeneralCombineRegsRmwSplits(body, base_ctx, live_at_end_s2, mm);
  if (!call_used_cands.empty() && call_used_cands.size() < all_general_cands.size()) {
    Sched1ProjectionContext cu_ctx = unsplit_ctx;
    cu_ctx.forced_combine_regs_split_body_indices = call_used_cands;
    const Sched1ProjectionView cu_view =
        ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, cu_ctx);
    if (cu_view.core_insns.size() > 1 && IsAchievable(cu_view.core_insns, cu_view.facts_s1, mm,
                                                      Phase::kSched1, cu_view.live_at_end_s1)) {
      return shrink_minimal(call_used_cands);
    }
  }

  if (all_general_cands.size() > 1 && all_general_cands.size() <= 16) {
    auto count_view_diffs = [&](const Sched1ProjectionView& v) -> int {
      const std::vector<int> ord = ScheduleSched1ViewCoreOrder(v, mm);
      if (ord.size() != v.core_insns.size()) {
        return static_cast<int>(v.core_insns.size());
      }
      int d = 0;
      for (int i = 0; i < static_cast<int>(ord.size()); ++i) {
        if (ord[static_cast<size_t>(i)] != i) {
          ++d;
        }
      }
      return d;
    };
    absl::btree_set<int> chosen;
    int cur_diffs = count_view_diffs(unsplit_view);
    constexpr int kMaxGreedySteps = 4;
    for (int step = 0; step < kMaxGreedySteps; ++step) {
      int best_idx = -1;
      int best_diffs = cur_diffs;
      for (int idx : all_general_cands) {
        if (chosen.contains(idx)) {
          continue;
        }
        Sched1ProjectionContext trial_ctx = unsplit_ctx;
        trial_ctx.forced_combine_regs_split_body_indices = chosen;
        trial_ctx.forced_combine_regs_split_body_indices.insert(idx);
        const Sched1ProjectionView trial_view =
            ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, trial_ctx);
        if (trial_view.core_insns.size() <= 1) {
          continue;
        }
        if (IsAchievable(trial_view.core_insns, trial_view.facts_s1, mm, Phase::kSched1,
                         trial_view.live_at_end_s1)) {
          chosen.insert(idx);
          return shrink_minimal(std::move(chosen));
        }
        const int nd = count_view_diffs(trial_view);
        if (nd < best_diffs) {
          best_diffs = nd;
          best_idx = idx;
        }
      }
      if (best_idx < 0) {
        break;
      }
      chosen.insert(best_idx);
      cur_diffs = best_diffs;
    }
  }
  return {};
}

// Mirrors `reload1.c:5641, 6102-6130` (`emit_reload_insns`; MIPS does not
// define `PRESERVE_DEATH_INFO_REGNO_P`): returns true when `SF`/`DF` FPR `r`
// read by FP consumer at `pos` was loaded by an earlier cross-bank `"f"`
// GPR->FPR (`mtc1`) input reload `(set (reg:SF/DF r) (reg:SF/DF gpr))`
// (without `REG_EQUIV`/`REG_EQUAL`/`REG_DEAD gpr`) with no intervening touch
// of `r` or `gpr`. Such reload scratch FPRs never receive `(REG_DEAD r)` in
// `.09.greg`.
template <typename GetPnFn, typename GetKindFn>
bool IsCrossBankFprInputReloadOperandImpl(int pos, int r, RtlMachineMode mode,
                                          const MachineModel& mm, const GetPnFn& get_pn,
                                          const GetKindFn& get_kind) {
  if (r < kMipsFpRegFirst || r > kMipsFpRegLast ||
      (mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDf)) {
    return false;
  }
  const RtlPatternAndNotes& cons_pn = get_pn(pos);
  const RtlExpr& cp = cons_pn.pattern;
  if (get_kind(pos) != Insn::Kind::kInsn || cp.op == RtlOp::kUse || cp.op == RtlOp::kClobber ||
      (cp.op == RtlOp::kSet && cp.children.size() == 2 &&
       (cp.SetSrc().op == RtlOp::kReg || cp.SetSrc().op == RtlOp::kMem))) {
    return false;
  }
  const int fpr_nregs = std::max(1, mm.HardRegNRegs(r, mode));
  for (int p = pos - 1; p >= 0; --p) {
    const Insn::Kind pkind = get_kind(p);
    if (pkind == Insn::Kind::kCall || pkind == Insn::Kind::kJump) {
      return false;
    }
    const RtlPatternAndNotes& prev_pn = get_pn(p);
    const RtlExpr& pp = prev_pn.pattern;
    if (pp.op == RtlOp::kSet && pp.children.size() == 2 && pp.SetDest().op == RtlOp::kReg &&
        !pp.SetDest().flags.integrated && pp.SetDest().RegNo() == r && pp.SetDest().mode == mode &&
        pp.SetSrc().op == RtlOp::kReg && !pp.SetSrc().flags.integrated &&
        pp.SetSrc().mode == mode && pp.SetSrc().RegNo() >= 2 &&
        pp.SetSrc().RegNo() < kMipsFpRegFirst) {
      const int gpr = pp.SetSrc().RegNo();
      const int gpr_nregs = std::max(1, mm.HardRegNRegs(gpr, mode));
      for (const RtlRegNote& n : prev_pn.reg_notes) {
        if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual) {
          return false;
        }
        if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg) {
          const int nr = n.datum.RegNo();
          if (nr >= gpr && nr < gpr + gpr_nregs) {
            return false;
          }
        }
      }
      for (int k = p + 1; k < pos; ++k) {
        const RtlExpr& mp = get_pn(k).pattern;
        if (PatternDefinesHardRegSpan(mp, gpr, gpr_nregs, mm) ||
            PatternReadsHardRegSpan(mp, gpr, gpr_nregs, mm)) {
          return false;
        }
      }
      return true;
    }
    if (PatternDefinesHardRegSpan(pp, r, fpr_nregs, mm) ||
        PatternReadsHardRegSpan(pp, r, fpr_nregs, mm)) {
      return false;
    }
  }
  return false;
}

bool IsCrossBankFprInputReloadOperandInView(absl::Span<const std::string> instructions, int pos,
                                            int r, RtlMachineMode mode, const MachineModel& mm) {
  if (r < kMipsFpRegFirst || r > kMipsFpRegLast ||
      (mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDf) || pos < 0 ||
      static_cast<size_t>(pos) >= instructions.size()) {
    return false;
  }
  std::vector<RtlPatternAndNotes> parsed(static_cast<size_t>(pos) + 1);
  for (int i = 0; i <= pos; ++i) {
    auto pn = ParsePatternAndNotes(instructions[static_cast<size_t>(i)]);
    if (!pn.ok()) {
      return false;
    }
    parsed[static_cast<size_t>(i)] = std::move(*pn);
  }
  return IsCrossBankFprInputReloadOperandImpl(
      pos, r, mode, mm,
      [&](int idx) -> const RtlPatternAndNotes& { return parsed[static_cast<size_t>(idx)]; },
      [&](int idx) { return ClassifyInsnKind(parsed[static_cast<size_t>(idx)].pattern); });
}

bool IsCrossBankFprInputReloadOperandInOrder(absl::Span<const Insn> base_block,
                                             absl::Span<const int> order_in_base, int pos, int r,
                                             RtlMachineMode mode, const MachineModel& mm) {
  return IsCrossBankFprInputReloadOperandImpl(
      pos, r, mode, mm,
      [&](int idx) -> const RtlPatternAndNotes& {
        return base_block[static_cast<size_t>(order_in_base[static_cast<size_t>(idx)])].pn;
      },
      [&](int idx) {
        return base_block[static_cast<size_t>(order_in_base[static_cast<size_t>(idx)])].kind;
      });
}

// Unpacks non-annulled `(sequence [(call_insn|jump_insn) (insn)])` delay slots
// (`reorg.c:2901-2955`) into a read-only pre-`reorg` view (`view_cfg`) plus
// reconstructed liveness (`live_cfg`), recording exact origin locations so
// verified Stage 0 notes can be committed back to `orig_cfg`.
void BuildStage0UnpackedDelayViewCfg(const RtlControlFlowGraph& orig_cfg,
                                     const CompilationProfile& profile,
                                     RtlControlFlowGraph* view_cfg, RtlControlFlowGraph* live_cfg,
                                     std::vector<Stage0BlockDelayView>* block_views,
                                     bool allow_annulled_tail_branch = false) {
  const size_t num_blocks = orig_cfg.rpo_blocks.size();
  *view_cfg = orig_cfg;
  block_views->assign(num_blocks, Stage0BlockDelayView{});

  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& orig_blk = orig_cfg.rpo_blocks[b];
    const size_t m = orig_blk.instructions.size();
    Stage0BlockDelayView& bv = (*block_views)[b];
    size_t effective_end = m;
    while (effective_end > 0) {
      auto tail_pn = ParsePatternAndNotes(orig_blk.instructions[effective_end - 1]);
      if (!tail_pn.ok() || tail_pn->pattern.op != RtlOp::kBarrier) {
        break;
      }
      --effective_end;
    }

    RtlBasicBlock& vblk = view_cfg->rpo_blocks[b];
    vblk.instructions.clear();
    vblk.rtl_insns.clear();
    bv.view_to_orig.clear();
    std::vector<std::optional<std::vector<RtlExpr>>> unpacked_call_fusage;

    for (size_t i = 0; i < effective_end; ++i) {
      auto pn = ParsePatternAndNotes(orig_blk.instructions[i]);
      if (!pn.ok()) {
        bv.eligible = false;
        break;
      }
      if (pn->pattern.op != RtlOp::kSequence) {
        if (i + 1 == effective_end && IsReturnJumpPatternForAttribution(pn->pattern)) {
          continue;
        }
        vblk.instructions.push_back(orig_blk.instructions[i]);
        bv.view_to_orig.push_back(Stage0OriginLoc{.orig_insn_idx = i, .is_delay_slot = false});
        unpacked_call_fusage.push_back(std::nullopt);
        continue;
      }
      if (pn->pattern.children.size() != 2 || pn->pattern.children[1].op != RtlOp::kInsn) {
        bv.eligible = false;
        break;
      }
      const RtlOp head_op = pn->pattern.children[0].op;
      if (pn->pattern.children[0].flags.unchanging || pn->pattern.children[1].flags.unchanging) {
        // `reorg.c:fill_eager_delay_slots`: an annulled branch (`jump_insn/u`)
        // at the end of a block holds an instruction stolen from a successor
        // block, not from this block's `sched1`/`sched2` region.
        if (allow_annulled_tail_branch && head_op == RtlOp::kJumpInsn && i + 1 == effective_end) {
          auto head_pn = RtlPatternAndNotes::FromSequenceSubInsn(pn->pattern.children[0]);
          auto slot_pn = RtlPatternAndNotes::FromSequenceSubInsn(pn->pattern.children[1]);
          if (head_pn.ok() && slot_pn.ok()) {
            CollectRegReads(slot_pn->pattern, &bv.annulled_slot_live_at_end);
            vblk.instructions.push_back(
                FormatGccPrintRtlPatternAndNotes(head_pn->pattern, head_pn->reg_notes,
                                                 head_pn->bracketed_notes, /*call_fusage=*/{}));
            bv.view_to_orig.push_back(Stage0OriginLoc{.orig_insn_idx = i, .is_delay_slot = false});
            unpacked_call_fusage.push_back(std::nullopt);
            continue;
          }
        }
        bv.eligible = false;
        break;
      }
      if (head_op != RtlOp::kJumpInsn && head_op != RtlOp::kCallInsn) {
        bv.eligible = false;
        break;
      }
      if (head_op == RtlOp::kJumpInsn && i + 1 != effective_end) {
        bv.eligible = false;
        break;
      }
      auto head_pn = RtlPatternAndNotes::FromSequenceSubInsn(pn->pattern.children[0]);
      auto slot_pn = RtlPatternAndNotes::FromSequenceSubInsn(pn->pattern.children[1]);
      if (!head_pn.ok() || !slot_pn.ok() || !IsSimpleNonControlDelayInsn(slot_pn->pattern)) {
        bv.eligible = false;
        break;
      }
      bv.seqs[i] = Stage0SeqInfo{
          .head_wrap = pn->pattern.children[0],
          .slot_wrap = pn->pattern.children[1],
          .slot_pn = *slot_pn,
      };
      // Strip `reorg.c:2283-2306` placeholder `(REG_DEAD (const_int 0))` notes
      // on unpacked call delay slots inside `view_cfg` so liveness reconstruction
      // below restores exact post-`sched2` `REG_DEAD` notes for Stage 0 discovery.
      bool had_placeholder_dead = false;
      std::vector<RtlRegNote> clean_slot_notes;
      clean_slot_notes.reserve(slot_pn->reg_notes.size());
      for (const RtlRegNote& note : slot_pn->reg_notes) {
        if (head_op == RtlOp::kCallInsn && note.kind == RtlMachineMode::kRegDead &&
            note.datum.op != RtlOp::kReg && note.datum.op != RtlOp::kMem) {
          had_placeholder_dead = true;
          continue;
        }
        clean_slot_notes.push_back(note);
      }
      const std::string slot_str = FormatGccPrintRtlPatternAndNotes(
          slot_pn->pattern, clean_slot_notes, slot_pn->bracketed_notes, /*call_fusage=*/{});
      const bool is_return =
          (head_op == RtlOp::kJumpInsn && IsReturnJumpPatternForAttribution(head_pn->pattern));
      if (is_return) {
        size_t insert_pos = vblk.instructions.size();
        while (insert_pos > 0) {
          auto prev_pn = ParsePatternAndNotes(vblk.instructions[insert_pos - 1]);
          if (!prev_pn.ok() || prev_pn->pattern.op != RtlOp::kUse) {
            break;
          }
          --insert_pos;
        }
        vblk.instructions.insert(vblk.instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                                 slot_str);
        bv.view_to_orig.insert(bv.view_to_orig.begin() + static_cast<ptrdiff_t>(insert_pos),
                               Stage0OriginLoc{.orig_insn_idx = i,
                                               .is_delay_slot = true,
                                               .had_placeholder_dead = had_placeholder_dead});
        unpacked_call_fusage.insert(
            unpacked_call_fusage.begin() + static_cast<ptrdiff_t>(insert_pos), std::nullopt);
      } else {
        // Mirror `reorg.c:2901-2955` (`fill_simple_delay_slots`): a `CALL_INSN`
        // clobbers `$ra` (`31`), so `reorg.c` skips an immediately preceding
        // prologue `$ra` save `(set (mem sp+off) (reg 31))` when scanning
        // backward for a call delay slot.
        size_t insert_pos = vblk.instructions.size();
        if (head_op == RtlOp::kCallInsn && SlotCanPrecedeRaSaveForDelayView(slot_pn->pattern)) {
          // Only step past a prologue `$ra` save when `slot` does not write a
          // `call_fusage` argument register (which `sched2` keeps after `sw $ra`
          // due to its `REG_DEP_TRUE` edge into `call`).
          absl::btree_set<int> call_arg_reads;
          for (const RtlExpr& fu : head_pn->call_fusage) {
            CollectRegReads(fu, &call_arg_reads);
          }
          if (i < orig_blk.rtl_insns.size() && orig_blk.rtl_insns[i].has_explicit_call_fusage) {
            for (const RtlExpr& fu : orig_blk.rtl_insns[i].call_fusage) {
              CollectRegReads(fu, &call_arg_reads);
            }
          }
          absl::btree_set<int> slot_fdefs;
          absl::btree_set<int> slot_pdefs;
          CollectFullRegDefs(slot_pn->pattern, &slot_fdefs, &slot_pdefs);
          bool writes_call_arg = false;
          for (int r : call_arg_reads) {
            if (slot_fdefs.contains(r) || slot_pdefs.contains(r)) {
              writes_call_arg = true;
              break;
            }
          }
          while (!writes_call_arg && insert_pos > 0) {
            auto prev_pn = ParsePatternAndNotes(vblk.instructions[insert_pos - 1]);
            if (!prev_pn.ok() || !IsSpRaSavePatternForDelayView(prev_pn->pattern)) {
              break;
            }
            --insert_pos;
          }
        }
        if (head_op == RtlOp::kJumpInsn) {
          bv.unpacked_non_return_jump_slot_view_idx = insert_pos;
        }
        vblk.instructions.insert(vblk.instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                                 slot_str);
        bv.view_to_orig.insert(bv.view_to_orig.begin() + static_cast<ptrdiff_t>(insert_pos),
                               Stage0OriginLoc{.orig_insn_idx = i,
                                               .is_delay_slot = true,
                                               .had_placeholder_dead = had_placeholder_dead});
        unpacked_call_fusage.insert(
            unpacked_call_fusage.begin() + static_cast<ptrdiff_t>(insert_pos), std::nullopt);
        vblk.instructions.push_back(FormatGccPrintRtlPatternAndNotes(
            head_pn->pattern, head_pn->reg_notes, head_pn->bracketed_notes, head_pn->call_fusage));
        bv.view_to_orig.push_back(Stage0OriginLoc{.orig_insn_idx = i, .is_delay_slot = false});
        if (head_pn->has_explicit_call_fusage) {
          unpacked_call_fusage.push_back(head_pn->call_fusage);
        } else {
          unpacked_call_fusage.push_back(std::nullopt);
        }
      }
    }
    if (!bv.eligible) {
      vblk = orig_blk;
      continue;
    }
    // Mirrors `reorg.c:3151-3225`, `config/mips/mips.h:2041-2050`, and
    // `config/mips/mips.c:5080, 5191-5217`: unpack an eligible sideband
    // `epilogue_delay_pattern` before trailing `USE`/`CLOBBER`/`NOTE` insns in
    // the exit block so Stage 0 pure-reader `REG_DEAD` attribution sees the
    // complete post-`sched2` exit block.
    if (b + 1 == num_blocks && orig_blk.successors.empty() &&
        orig_cfg.sideband.epilogue_delay_pattern.has_value() &&
        !orig_cfg.sideband.epilogue_delay_pattern->empty()) {
      auto ep_pn = ParsePatternAndNotes(*orig_cfg.sideband.epilogue_delay_pattern);
      bool ep_ok = ep_pn.ok() && IsSimpleNonControlDelayInsn(ep_pn->pattern) &&
                   (!ep_pn->pattern.IsSet() || ep_pn->pattern.SetSrc().op != RtlOp::kMem);
      if (ep_ok) {
        const MachineModel mm_ep = MachineModel::FromProfile(profile);
        ep_pn->pattern.Visit([&](const RtlExpr& n) {
          if (!ep_ok || !n.IsHardReg() || n.IsArgPointerReg()) {
            return;
          }
          const int r = n.RegNo();
          const int span = std::max(1, mm_ep.HardRegNRegs(r, n.mode));
          for (int w = r; w < r + span && w < kMipsFirstPseudoRegister; ++w) {
            const bool is_saved = std::find(orig_cfg.sideband.saved_gpr_regnos.begin(),
                                            orig_cfg.sideband.saved_gpr_regnos.end(),
                                            w) != orig_cfg.sideband.saved_gpr_regnos.end() ||
                                  std::find(orig_cfg.sideband.saved_fpr_regnos.begin(),
                                            orig_cfg.sideband.saved_fpr_regnos.end(),
                                            w) != orig_cfg.sideband.saved_fpr_regnos.end();
            if (w == kMipsStackPointerRegno || w == kMipsReturnAddrRegno ||
                (w == kMipsFramePointerRegno && orig_cfg.sideband.frame_pointer_needed) ||
                is_saved) {
              ep_ok = false;
              return;
            }
          }
        });
      }
      if (ep_ok) {
        size_t insert_pos = vblk.instructions.size();
        while (insert_pos > 0) {
          auto tail_pn = ParsePatternAndNotes(vblk.instructions[insert_pos - 1]);
          if (!tail_pn.ok() ||
              (tail_pn->pattern.op != RtlOp::kUse && tail_pn->pattern.op != RtlOp::kClobber &&
               tail_pn->pattern.op != RtlOp::kNote)) {
            break;
          }
          --insert_pos;
        }
        vblk.instructions.insert(vblk.instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                                 *orig_cfg.sideband.epilogue_delay_pattern);
        bv.view_to_orig.insert(bv.view_to_orig.begin() + static_cast<ptrdiff_t>(insert_pos),
                               Stage0OriginLoc{.orig_insn_idx = 0,
                                               .is_delay_slot = false,
                                               .had_placeholder_dead = false,
                                               .is_epilogue_delay = true});
        unpacked_call_fusage.insert(
            unpacked_call_fusage.begin() + static_cast<ptrdiff_t>(insert_pos), std::nullopt);
        view_cfg->sideband.epilogue_delay_pattern.reset();
      }
    }
    vblk.SyncRtlInsnsFromInstructions();
    for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < bv.view_to_orig.size(); ++vi) {
      if (!bv.view_to_orig[vi].is_delay_slot && !bv.view_to_orig[vi].is_epilogue_delay) {
        if (unpacked_call_fusage[vi].has_value() && !vblk.rtl_insns[vi].has_explicit_call_fusage) {
          vblk.rtl_insns[vi].has_explicit_call_fusage = true;
          vblk.rtl_insns[vi].call_fusage = *unpacked_call_fusage[vi];
          continue;
        }
        const size_t oi = bv.view_to_orig[vi].orig_insn_idx;
        if (oi < orig_blk.rtl_insns.size() && orig_blk.rtl_insns[oi].has_explicit_call_fusage &&
            !vblk.rtl_insns[vi].has_explicit_call_fusage) {
          vblk.rtl_insns[vi].has_explicit_call_fusage = true;
          vblk.rtl_insns[vi].call_fusage = orig_blk.rtl_insns[oi].call_fusage;
        }
      }
    }
  }

  *live_cfg = *view_cfg;
  ReconstructRtlLiveness(
      live_cfg, profile,
      {.reload_completed = true, .update_reg_notes = true, .preserve_phantom_pseudos = false});
  const MachineModel mm = MachineModel::FromProfile(profile);
  // Restore reconstructed `REG_DEAD` notes in `view_cfg` for unpacked call delay
  // slots whose original `(REG_DEAD (const_int 0))` placeholder was cleared by `reorg.c`.
  for (size_t b = 0; b < num_blocks; ++b) {
    if (!(*block_views)[b].eligible) {
      continue;
    }
    RtlBasicBlock& vblk = view_cfg->rpo_blocks[b];
    const RtlBasicBlock& lblk = live_cfg->rpo_blocks[b];
    bool updated_block = false;
    for (size_t vi = 0; vi < vblk.instructions.size() &&
                        vi < (*block_views)[b].view_to_orig.size() && vi < lblk.instructions.size();
         ++vi) {
      if (!(*block_views)[b].view_to_orig[vi].had_placeholder_dead) {
        continue;
      }
      auto vpn = ParsePatternAndNotes(vblk.instructions[vi]);
      auto lpn = ParsePatternAndNotes(lblk.instructions[vi]);
      if (!vpn.ok() || !lpn.ok()) {
        continue;
      }
      std::vector<RtlRegNote> merged;
      for (const RtlRegNote& note : lpn->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead) {
          // `reload1.c:5641, 6102-6130` (`emit_reload_insns`; MIPS does not
          // define `PRESERVE_DEATH_INFO_REGNO_P`): omit `(REG_DEAD fpr)` for a
          // cross-bank `"f"` GPR->FPR (`mtc1`) input reload scratch register.
          if (note.datum.op == RtlOp::kReg &&
              IsCrossBankFprInputReloadOperandInView(vblk.instructions, static_cast<int>(vi),
                                                     note.datum.RegNo(), note.datum.mode, mm)) {
            continue;
          }
          merged.push_back(note);
        }
      }
      for (const RtlRegNote& note : vpn->reg_notes) {
        if (note.kind != RtlMachineMode::kRegDead) {
          merged.push_back(note);
        }
      }
      vblk.instructions[vi] = FormatGccPrintRtlPatternAndNotes(
          vpn->pattern, merged, vpn->bracketed_notes, /*call_fusage=*/{});
      updated_block = true;
    }
    if (updated_block) {
      std::vector<bool> saved_has_fusage(vblk.rtl_insns.size(), false);
      std::vector<std::vector<RtlExpr>> saved_fusage(vblk.rtl_insns.size());
      for (size_t vi = 0; vi < vblk.rtl_insns.size(); ++vi) {
        if (vblk.rtl_insns[vi].has_explicit_call_fusage) {
          saved_has_fusage[vi] = true;
          saved_fusage[vi] = vblk.rtl_insns[vi].call_fusage;
        }
      }
      vblk.SyncRtlInsnsFromInstructions();
      for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < saved_has_fusage.size(); ++vi) {
        if (saved_has_fusage[vi] && !vblk.rtl_insns[vi].has_explicit_call_fusage) {
          vblk.rtl_insns[vi].has_explicit_call_fusage = true;
          vblk.rtl_insns[vi].call_fusage = std::move(saved_fusage[vi]);
        }
      }
    }
  }
}

bool PatternHasVolatileOrAsm(const RtlExpr& pat) {
  bool bad = false;
  pat.Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kAsmInput || node.op == RtlOp::kAsmOperands ||
        (node.op == RtlOp::kMem && node.flags.volatil)) {
      bad = true;
    }
  });
  return bad;
}

void ClassifyMemAccesses(const RtlExpr& pat, bool* reads_mem, bool* writes_mem,
                         bool* all_reads_unchanging) {
  *reads_mem = false;
  *writes_mem = false;
  *all_reads_unchanging = true;
  auto visit_reads = [&](const RtlExpr& expr) {
    expr.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kMem) {
        *reads_mem = true;
        if (!node.flags.unchanging) {
          *all_reads_unchanging = false;
        }
      }
    });
  };
  auto handle = [&](const RtlExpr& elem) {
    if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
      if (elem.SetDest().op == RtlOp::kMem) {
        *writes_mem = true;
        if (!elem.SetDest().children.empty()) {
          visit_reads(elem.SetDest().children[0]);
        }
      }
      visit_reads(elem.SetSrc());
    } else if (elem.op != RtlOp::kClobber && elem.op != RtlOp::kUse) {
      visit_reads(elem);
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      handle(ch);
    }
  } else {
    handle(pat);
  }
}

int64_t StackModeByteSize(RtlMachineMode mode) {
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

bool Stage0DelaySlotCanCommutePast(const Insn& slot, const Insn& earlier, const MachineModel& mm) {
  if (earlier.kind != Insn::Kind::kInsn || earlier.pn.pattern.op == RtlOp::kUse ||
      earlier.pn.pattern.op == RtlOp::kClobber || PatternHasVolatileOrAsm(earlier.pn.pattern) ||
      PatternHasVolatileOrAsm(slot.pn.pattern)) {
    return false;
  }
  const InsnRegReadDefDeaths si = CollectInsnRegReadDefDeaths(slot, mm);
  const InsnRegReadDefDeaths ei = CollectInsnRegReadDefDeaths(earlier, mm);
  for (int r : si.defs) {
    if (ei.defs.contains(r) || ei.reads.contains(r)) {
      return false;
    }
  }
  for (int r : ei.defs) {
    if (si.reads.contains(r)) {
      return false;
    }
  }
  bool s_rmem = false;
  bool s_wmem = false;
  bool s_ro_u = true;
  bool e_rmem = false;
  bool e_wmem = false;
  bool e_ro_u = true;
  ClassifyMemAccesses(slot.pn.pattern, &s_rmem, &s_wmem, &s_ro_u);
  ClassifyMemAccesses(earlier.pn.pattern, &e_rmem, &e_wmem, &e_ro_u);
  if (!s_wmem && !e_wmem) {
    return true;
  }
  if (!s_rmem && !s_wmem) {
    return true;
  }
  if (!e_rmem && !e_wmem) {
    return true;
  }
  // Allow commuting disjoint plain stack-slot accesses (`sched.c:memrefs_conflict_p`)
  // or a plain stack-slot store against a read-only `MEM/u` load.
  const auto s_acc = TryParsePlainStackSlotMove(slot);
  const auto e_acc = TryParsePlainStackSlotMove(earlier);
  if (s_acc.has_value() && e_acc.has_value() && s_acc->key.base_regno == e_acc->key.base_regno) {
    const int64_t s_sz = StackModeByteSize(s_acc->key.mode);
    const int64_t e_sz = StackModeByteSize(e_acc->key.mode);
    if (s_sz > 0 && e_sz > 0 &&
        (s_acc->key.offset + s_sz <= e_acc->key.offset ||
         e_acc->key.offset + e_sz <= s_acc->key.offset)) {
      return true;
    }
  }
  if (s_acc.has_value() && s_acc->is_store && !e_wmem && e_ro_u) {
    return true;
  }
  if (e_acc.has_value() && e_acc->is_store && !s_wmem && s_ro_u) {
    return true;
  }
  return false;
}

// Re-aligns unpacked delay-slot instructions (`reorg.c:2800-2960`
// `fill_simple_delay_slots`) in `view_cfg` when naive placement immediately
// before the branch/call falls outside `Im(F_sched2)`, sliding up to 2 delay
// slots earlier across dependency-independent instructions until
// `InvertBlockOrder(Phase::kSched2)` succeeds.
void AlignStage0DelaySlotsToSched2View(const CompilationProfile& profile, const MachineModel& mm,
                                       RtlControlFlowGraph* view_cfg, RtlControlFlowGraph* live_cfg,
                                       std::vector<Stage0BlockDelayView>* block_views) {
  const RegFacts facts_s2 = RegFacts::FromCfg(*view_cfg, profile, Phase::kSched2);
  bool any_reordered = false;
  const size_t num_blocks = view_cfg->rpo_blocks.size();
  std::vector<absl::btree_set<size_t>> moved_slot_new_view_idxs(num_blocks);

  for (size_t b = 0; b < num_blocks; ++b) {
    Stage0BlockDelayView& bv = (*block_views)[b];
    if (!bv.eligible || bv.seqs.empty()) {
      continue;
    }
    RtlBasicBlock& vblk = view_cfg->rpo_blocks[b];
    const ParsedSchedRegion region = ParseBlockForSchedModel(vblk, b, Phase::kSched2);
    if (!region.valid || region.has_mid_block_loop_barrier || region.sched_block.size() <= 2) {
      continue;
    }
    absl::btree_set<int> live_at_end_s2;
    for (int succ : vblk.successors) {
      if (const auto it = live_cfg->sideband.basic_block_live_at_start.find(succ);
          it != live_cfg->sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    if (InvertBlockOrder(region.sched_block, facts_s2, mm, Phase::kSched2, live_at_end_s2)
            .has_value()) {
      continue;
    }

    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, b);
    const int n = static_cast<int>(region.sched_block.size());
    std::vector<int> slot_sis;
    for (int si = bounds.prologue_end; si < bounds.epilogue_begin; ++si) {
      const size_t vi = region.sched_orig_indices[static_cast<size_t>(si)];
      if (vi < bv.view_to_orig.size() && bv.view_to_orig[vi].is_delay_slot) {
        slot_sis.push_back(si);
      }
    }
    if (slot_sis.empty()) {
      continue;
    }

    // Collect nearest valid earlier positions for each unpacked delay slot.
    constexpr int kMaxSlidesPerSlot = 12;
    std::vector<std::vector<int>> valid_targets(slot_sis.size());
    for (size_t k = 0; k < slot_sis.size(); ++k) {
      const int si = slot_sis[k];
      valid_targets[k].push_back(si);
      const Insn& slot_insn = region.sched_block[static_cast<size_t>(si)];
      int slides = 0;
      for (int pos = si - 1; pos >= bounds.prologue_end && slides < kMaxSlidesPerSlot; --pos) {
        if (!Stage0DelaySlotCanCommutePast(slot_insn, region.sched_block[static_cast<size_t>(pos)],
                                           mm)) {
          break;
        }
        valid_targets[k].push_back(pos);
        ++slides;
      }
    }

    auto apply_slot_moves = [&](absl::Span<const std::pair<int, int>> moves) -> std::vector<int> {
      std::vector<int> perm(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        perm[static_cast<size_t>(i)] = i;
      }
      // Apply moves in descending `from` order so earlier indices stay valid.
      std::vector<std::pair<int, int>> sorted_moves(moves.begin(), moves.end());
      std::sort(sorted_moves.begin(), sorted_moves.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      for (const auto& [from, to] : sorted_moves) {
        if (to >= from || to < 0 || from >= n) {
          continue;
        }
        const int val = perm[static_cast<size_t>(from)];
        perm.erase(perm.begin() + from);
        perm.insert(perm.begin() + to, val);
      }
      return perm;
    };

    auto test_perm = [&](const std::vector<int>& perm) -> bool {
      std::vector<Insn> trial;
      trial.reserve(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        Insn insn = region.sched_block[static_cast<size_t>(perm[static_cast<size_t>(i)])];
        insn.luid = i;
        trial.push_back(std::move(insn));
      }
      return InvertBlockOrder(trial, facts_s2, mm, Phase::kSched2, live_at_end_s2).has_value();
    };

    std::optional<std::vector<int>> chosen_perm;
    // Pass 1: single-slot slide (preferring minimal displacement).
    for (size_t k = 0; k < slot_sis.size() && !chosen_perm.has_value(); ++k) {
      for (size_t t = 1; t < valid_targets[k].size(); ++t) {
        std::vector<int> perm = apply_slot_moves({{slot_sis[k], valid_targets[k][t]}});
        if (test_perm(perm)) {
          chosen_perm = std::move(perm);
          break;
        }
      }
    }
    // Pass 2: pairwise slides when two call/jump delay slots in the same block both moved.
    int pair_budget = 40;
    for (size_t k1 = 0; k1 < slot_sis.size() && !chosen_perm.has_value() && pair_budget > 0; ++k1) {
      for (size_t k2 = k1 + 1; k2 < slot_sis.size() && !chosen_perm.has_value() && pair_budget > 0;
           ++k2) {
        const size_t lim1 = std::min<size_t>(valid_targets[k1].size(), 7);
        const size_t lim2 = std::min<size_t>(valid_targets[k2].size(), 7);
        for (size_t t1 = 1; t1 < lim1 && !chosen_perm.has_value() && pair_budget > 0; ++t1) {
          for (size_t t2 = 1; t2 < lim2 && !chosen_perm.has_value() && pair_budget > 0; ++t2) {
            --pair_budget;
            std::vector<int> perm = apply_slot_moves(
                {{slot_sis[k1], valid_targets[k1][t1]}, {slot_sis[k2], valid_targets[k2][t2]}});
            if (test_perm(perm)) {
              chosen_perm = std::move(perm);
              break;
            }
          }
        }
      }
    }

    if (!chosen_perm.has_value()) {
      continue;
    }

    // Apply `chosen_perm` to `vblk.instructions`, `vblk.rtl_insns`, and `bv.view_to_orig`.
    std::vector<std::string> new_insns = vblk.instructions;
    std::vector<Stage0OriginLoc> new_v2o = bv.view_to_orig;
    std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage(vblk.instructions.size());
    for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < saved_fusage.size(); ++vi) {
      if (vblk.rtl_insns[vi].has_explicit_call_fusage) {
        saved_fusage[vi] = vblk.rtl_insns[vi].call_fusage;
      }
    }
    std::vector<std::optional<std::vector<RtlExpr>>> new_fusage = saved_fusage;
    for (int i = 0; i < n; ++i) {
      const size_t dst_vi = region.sched_orig_indices[static_cast<size_t>(i)];
      const size_t src_vi =
          region.sched_orig_indices[static_cast<size_t>((*chosen_perm)[static_cast<size_t>(i)])];
      new_insns[dst_vi] = vblk.instructions[src_vi];
      new_v2o[dst_vi] = bv.view_to_orig[src_vi];
      new_fusage[dst_vi] = saved_fusage[src_vi];
      if (bv.view_to_orig[src_vi].is_delay_slot && dst_vi < src_vi) {
        moved_slot_new_view_idxs[b].insert(dst_vi);
      }
    }
    vblk.instructions = std::move(new_insns);
    bv.view_to_orig = std::move(new_v2o);
    vblk.SyncRtlInsnsFromInstructions();
    for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < new_fusage.size(); ++vi) {
      if (new_fusage[vi].has_value() && !vblk.rtl_insns[vi].has_explicit_call_fusage) {
        vblk.rtl_insns[vi].has_explicit_call_fusage = true;
        vblk.rtl_insns[vi].call_fusage = *new_fusage[vi];
      }
    }
    any_reordered = true;
  }

  if (any_reordered) {
    *live_cfg = *view_cfg;
    ReconstructRtlLiveness(
        live_cfg, profile,
        {.reload_completed = true, .update_reg_notes = true, .preserve_phantom_pseudos = false});
    for (size_t b = 0; b < num_blocks; ++b) {
      Stage0BlockDelayView& bv = (*block_views)[b];
      if (!bv.eligible) {
        continue;
      }
      RtlBasicBlock& vblk = view_cfg->rpo_blocks[b];
      const RtlBasicBlock& lblk = live_cfg->rpo_blocks[b];
      bool updated = false;
      for (size_t vi = 0; vi < vblk.instructions.size() && vi < bv.view_to_orig.size() &&
                          vi < lblk.instructions.size();
           ++vi) {
        if (!bv.view_to_orig[vi].had_placeholder_dead) {
          continue;
        }
        auto vpn = ParsePatternAndNotes(vblk.instructions[vi]);
        auto lpn = ParsePatternAndNotes(lblk.instructions[vi]);
        if (!vpn.ok() || !lpn.ok()) {
          continue;
        }
        std::vector<RtlRegNote> merged;
        for (const RtlRegNote& note : lpn->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead) {
            merged.push_back(note);
          }
        }
        for (const RtlRegNote& note : vpn->reg_notes) {
          if (note.kind != RtlMachineMode::kRegDead) {
            merged.push_back(note);
          }
        }
        vblk.instructions[vi] = FormatGccPrintRtlPatternAndNotes(
            vpn->pattern, merged, vpn->bracketed_notes, /*call_fusage=*/{});
        updated = true;
      }
      // Transfer pure-reader `REG_DEAD` notes (or `reorg.c:918-920` `(REG_DEAD (const_int 0))`
      // placeholders) off moved delay slots onto the subsequent pure reader where `live_cfg`
      // shows the register actually dies in the pre-`reorg` (post-`sched2`) order.
      for (size_t slot_vi : moved_slot_new_view_idxs[b]) {
        if (slot_vi >= vblk.instructions.size() || slot_vi >= bv.view_to_orig.size() ||
            slot_vi >= lblk.instructions.size() || bv.view_to_orig[slot_vi].had_placeholder_dead) {
          continue;
        }
        auto svpn = ParsePatternAndNotes(vblk.instructions[slot_vi]);
        auto slpn = ParsePatternAndNotes(lblk.instructions[slot_vi]);
        if (!svpn.ok() || !slpn.ok()) {
          continue;
        }
        absl::btree_set<int> sv_fdefs;
        absl::btree_set<int> sv_pdefs;
        CollectFullRegDefs(svpn->pattern, &sv_fdefs, &sv_pdefs);
        absl::btree_set<int> sl_dead_regs;
        for (const RtlRegNote& note : slpn->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
            const int r = note.datum.RegNo();
            const int nr = std::max(1, mm.HardRegNRegs(r, note.datum.mode));
            for (int k = 0; k < nr; ++k) {
              sl_dead_regs.insert(r + k);
            }
          }
        }
        std::vector<size_t> erase_slot_notes;
        absl::btree_set<int> transferred_regs;
        auto try_transfer_reg = [&](int r, int nr, size_t slot_note_idx) -> bool {
          if (r <= 0 || r >= kMipsFirstPseudoRegister || r == 28 || r == kMipsStackPointerRegno ||
              transferred_regs.contains(r)) {
            return false;
          }
          for (int k = 0; k < nr; ++k) {
            if (sv_fdefs.contains(r + k) || sv_pdefs.contains(r + k) ||
                sl_dead_regs.contains(r + k)) {
              return false;
            }
          }
          for (size_t vi2 = slot_vi + 1;
               vi2 < vblk.instructions.size() && vi2 < lblk.instructions.size(); ++vi2) {
            auto v2 = ParsePatternAndNotes(vblk.instructions[vi2]);
            auto l2 = ParsePatternAndNotes(lblk.instructions[vi2]);
            if (!v2.ok() || !l2.ok()) {
              break;
            }
            if (v2->pattern.op == RtlOp::kCall || v2->pattern.op == RtlOp::kUse ||
                (v2->pattern.op == RtlOp::kParallel && !v2->pattern.children.empty() &&
                 (v2->pattern.children[0].op == RtlOp::kCall ||
                  (v2->pattern.children[0].op == RtlOp::kSet &&
                   v2->pattern.children[0].children.size() == 2 &&
                   v2->pattern.children[0].SetSrc().op == RtlOp::kCall)))) {
              break;
            }
            absl::btree_set<int> d2_f;
            absl::btree_set<int> d2_p;
            CollectFullRegDefs(v2->pattern, &d2_f, &d2_p);
            bool v2_defs_r = false;
            for (int k = 0; k < nr; ++k) {
              if (d2_f.contains(r + k) || d2_p.contains(r + k)) {
                v2_defs_r = true;
                break;
              }
            }
            if (v2_defs_r) {
              break;
            }
            std::optional<RtlRegNote> l2_dead_note;
            for (const RtlRegNote& note : l2->reg_notes) {
              if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
                  note.datum.RegNo() == r) {
                l2_dead_note = note;
                break;
              }
            }
            if (l2_dead_note.has_value()) {
              if (!HasRegDeadNoteCovering(v2->reg_notes, r, mm)) {
                v2->reg_notes.push_back(*l2_dead_note);
                vblk.instructions[vi2] = FormatGccPrintRtlPatternAndNotes(
                    v2->pattern, v2->reg_notes, v2->bracketed_notes, /*call_fusage=*/{});
                erase_slot_notes.push_back(slot_note_idx);
                for (int k = 0; k < nr; ++k) {
                  transferred_regs.insert(r + k);
                }
                bv.alignment_touched_view_idxs.insert(slot_vi);
                bv.alignment_touched_view_idxs.insert(vi2);
                updated = true;
                return true;
              }
              break;
            }
          }
          return false;
        };
        for (size_t ni = 0; ni < svpn->reg_notes.size(); ++ni) {
          const RtlRegNote& note = svpn->reg_notes[ni];
          if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
            const int r = note.datum.RegNo();
            const int nr = std::max(1, mm.HardRegNRegs(r, note.datum.mode));
            try_transfer_reg(r, nr, ni);
          }
        }
        if (!erase_slot_notes.empty()) {
          absl::btree_set<size_t> erase_set(erase_slot_notes.begin(), erase_slot_notes.end());
          std::vector<RtlRegNote> kept;
          kept.reserve(svpn->reg_notes.size());
          for (size_t ni = 0; ni < svpn->reg_notes.size(); ++ni) {
            if (!erase_set.contains(ni)) {
              kept.push_back(svpn->reg_notes[ni]);
            }
          }
          vblk.instructions[slot_vi] = FormatGccPrintRtlPatternAndNotes(
              svpn->pattern, kept, svpn->bracketed_notes, /*call_fusage=*/{});
        }
      }
      if (updated) {
        std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage(vblk.rtl_insns.size());
        for (size_t vi = 0; vi < vblk.rtl_insns.size(); ++vi) {
          if (vblk.rtl_insns[vi].has_explicit_call_fusage) {
            saved_fusage[vi] = vblk.rtl_insns[vi].call_fusage;
          }
        }
        vblk.SyncRtlInsnsFromInstructions();
        for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < saved_fusage.size(); ++vi) {
          if (saved_fusage[vi].has_value() && !vblk.rtl_insns[vi].has_explicit_call_fusage) {
            vblk.rtl_insns[vi].has_explicit_call_fusage = true;
            vblk.rtl_insns[vi].call_fusage = *saved_fusage[vi];
          }
        }
      }
    }
  }
}

struct ConstDoubleLoadSpan {
  size_t orig_start = 0;
  size_t orig_len = 0;
  size_t view_start = 0;
  std::string replacement_rtl;
};

RtlExpr MakeVoidConstDoubleExpr(int64_t lo_word, int64_t hi_word) {
  RtlExpr cd;
  cd.op = RtlOp::kConstDouble;
  cd.mode = RtlMachineMode::kNone;
  cd.children = {RtlExpr::MakeCc0()};
  cd.int_val = lo_word;
  cd.int_val_high = hi_word;
  return cd;
}

bool HasRegUnusedNote(const std::vector<RtlRegNote>& notes) {
  for (const RtlRegNote& note : notes) {
    if (note.kind == RtlMachineMode::kRegUnused) {
      return true;
    }
  }
  return false;
}

bool IsPlainNonSpecialSiGprDest(const RtlExpr& expr, int* out_reg) {
  if (expr.op != RtlOp::kReg || expr.IsArgPointerReg() || expr.mode != RtlMachineMode::kSi ||
      expr.flags.volatil || expr.flags.integrated) {
    return false;
  }
  const int r = expr.RegNo();
  if (r < 2 || r > 25) {
    return false;
  }
  *out_reg = r;
  return true;
}

std::vector<ConstDoubleLoadSpan> CollectConstDoubleLoadSpans(const RtlBasicBlock& orig_blk,
                                                             const Stage0BlockDelayView& bv) {
  const size_t n = orig_blk.instructions.size();
  std::vector<RtlPatternAndNotes> parsed(n);
  std::vector<bool> ok(n, false);
  for (size_t i = 0; i < n; ++i) {
    if (bv.seqs.contains(i)) {
      continue;
    }
    auto pn = ParsePatternAndNotes(orig_blk.instructions[i]);
    if (pn.ok()) {
      parsed[i] = std::move(*pn);
      ok[i] = true;
    }
  }

  // Map each non-delay-slot original insn index to its index in `view_cfg.rpo_blocks[b]`.
  absl::btree_map<size_t, size_t> orig_to_view;
  for (size_t vi = 0; vi < bv.view_to_orig.size(); ++vi) {
    if (!bv.view_to_orig[vi].is_delay_slot && !bv.view_to_orig[vi].is_epilogue_delay &&
        !bv.view_to_orig[vi].is_synthetic) {
      orig_to_view[bv.view_to_orig[vi].orig_insn_idx] = vi;
    }
  }

  // `varasm.c:1857-1895` (`immed_double_const`), `config/mips/mips.md:3208-3221`
  // (`large_int`), `config/mips/mips.c:1047-1057` (`mips_move_1word`): unsigned
  // 32-bit immediates with bit 31 set (`v < 0`) are represented as
  // `(const_double:VOID (cc0) v 0)`, never split by `large_int`, and emitted as
  // `li $r, v` -> GAS `lui $r, HI>>16; ori $r, $r, LO`.
  std::vector<ConstDoubleLoadSpan> spans;
  for (size_t i = 0; i + 1 < n; ++i) {
    if (!ok[i] || !ok[i + 1]) {
      continue;
    }
    const RtlExpr& p0 = parsed[i].pattern;
    const RtlExpr& p1 = parsed[i + 1].pattern;
    int r0 = -1;
    int r1 = -1;
    if (!p0.IsSet() || !p1.IsSet() || !IsPlainNonSpecialSiGprDest(p0.SetDest(), &r0) ||
        !IsPlainNonSpecialSiGprDest(p1.SetDest(), &r1) || r1 != r0 ||
        p0.SetSrc().op != RtlOp::kConstInt || p1.SetSrc().op != RtlOp::kIor ||
        p1.SetSrc().mode != RtlMachineMode::kSi || p1.SetSrc().children.size() != 2 ||
        p1.SetSrc().children[0].op != RtlOp::kReg ||
        p1.SetSrc().children[0].mode != RtlMachineMode::kSi ||
        p1.SetSrc().children[0].RegNo() != r0 || p1.SetSrc().children[1].op != RtlOp::kConstInt ||
        HasRegUnusedNote(parsed[i + 1].reg_notes)) {
      continue;
    }
    const int64_t hi = p0.SetSrc().int_val;
    const int64_t lo = p1.SetSrc().children[1].int_val;
    if (hi >= 0 || (hi & 0xffff) != 0 || lo <= 0 || lo > 0xffff) {
      continue;
    }
    const int32_t combined =
        static_cast<int32_t>(static_cast<uint32_t>(hi) | static_cast<uint32_t>(lo));
    if (combined >= 0) {
      continue;
    }
    const auto vit = orig_to_view.find(i);
    if (vit == orig_to_view.end()) {
      continue;
    }
    const size_t vs = vit->second;
    if (vs + 2 > bv.view_to_orig.size() || bv.view_to_orig[vs + 1].is_delay_slot ||
        bv.view_to_orig[vs + 1].is_epilogue_delay ||
        bv.view_to_orig[vs + 1].orig_insn_idx != i + 1) {
      continue;
    }
    RtlExpr set_expr = RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kSi, r0),
                                        MakeVoidConstDoubleExpr(combined, 0));
    spans.push_back(ConstDoubleLoadSpan{
        .orig_start = i,
        .orig_len = 2,
        .view_start = vs,
        .replacement_rtl = FormatGccPrintRtlPatternAndNotes(set_expr, {}, {}, {}),
    });
    ++i;
  }
  return spans;
}

RtlBasicBlock ApplyConstDoubleSpansToBlock(const RtlBasicBlock& src_blk,
                                           absl::Span<const ConstDoubleLoadSpan> spans,
                                           const std::vector<bool>& mask, bool use_view_indices,
                                           std::optional<size_t> omit_view_idx = std::nullopt) {
  absl::btree_map<size_t, size_t> start_to_span;
  for (size_t k = 0; k < spans.size(); ++k) {
    if (mask[k]) {
      start_to_span[use_view_indices ? spans[k].view_start : spans[k].orig_start] = k;
    }
  }
  RtlBasicBlock out = src_blk;
  out.instructions.clear();
  out.instructions.reserve(src_blk.instructions.size());
  std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage;
  saved_fusage.reserve(src_blk.instructions.size());

  size_t pos = 0;
  while (pos < src_blk.instructions.size()) {
    if (use_view_indices && omit_view_idx.has_value() && pos == *omit_view_idx) {
      ++pos;
      continue;
    }
    if (const auto it = start_to_span.find(pos); it != start_to_span.end()) {
      const ConstDoubleLoadSpan& sp = spans[it->second];
      out.instructions.push_back(sp.replacement_rtl);
      saved_fusage.push_back(std::nullopt);
      pos += sp.orig_len;
      continue;
    }
    out.instructions.push_back(src_blk.instructions[pos]);
    if (pos < src_blk.rtl_insns.size() && src_blk.rtl_insns[pos].has_explicit_call_fusage) {
      saved_fusage.push_back(src_blk.rtl_insns[pos].call_fusage);
    } else {
      saved_fusage.push_back(std::nullopt);
    }
    ++pos;
  }
  out.SyncRtlInsnsFromInstructions();
  for (size_t i = 0; i < out.rtl_insns.size() && i < saved_fusage.size(); ++i) {
    if (saved_fusage[i].has_value() && !out.rtl_insns[i].has_explicit_call_fusage) {
      out.rtl_insns[i].has_explicit_call_fusage = true;
      out.rtl_insns[i].call_fusage = std::move(*saved_fusage[i]);
    }
  }
  return out;
}

}  // namespace

void CollapseSched1UnsplitConstDoubleLoads(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);
  const MachineModel mm = MachineModel::FromProfile(profile);
  if (flags.opt_level <= 0 || !flags.RunsSched1() || !flags.RunsSched2() || mm.Target64Bit()) {
    return;
  }

  const size_t num_blocks = cfg->rpo_blocks.size();
  RtlControlFlowGraph view_cfg;
  RtlControlFlowGraph live_cfg;
  std::vector<Stage0BlockDelayView> block_views;
  BuildStage0UnpackedDelayViewCfg(*cfg, profile, &view_cfg, &live_cfg, &block_views,
                                  /*allow_annulled_tail_branch=*/true);
  const RegFacts facts_s2 = RegFacts::FromCfg(view_cfg, profile, Phase::kSched2);
  const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(view_cfg, live_cfg, mm);

  for (size_t b = 0; b < num_blocks; ++b) {
    if (!block_views[b].eligible) {
      continue;
    }
    const RtlBasicBlock& vblk = view_cfg.rpo_blocks[b];
    const ParsedSchedRegion base_region = ParseBlockForSchedModel(vblk, b, Phase::kSched2);
    if (!base_region.valid || base_region.has_mid_block_loop_barrier ||
        base_region.sched_block.size() <= 2) {
      continue;
    }
    const std::vector<ConstDoubleLoadSpan> spans =
        CollectConstDoubleLoadSpans(cfg->rpo_blocks[b], block_views[b]);
    if (spans.empty()) {
      continue;
    }

    absl::btree_set<int> live_at_end_s2 = block_views[b].annulled_slot_live_at_end;
    for (int succ : vblk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    const Sched1ProjectionContext base_proj_ctx = BuildSched1ProjectionContextForBlock(b, cfg_ctx);

    auto check_single_view = [&](const RtlBasicBlock& candidate_vblk,
                                 const absl::btree_set<int>& live_end,
                                 bool require_sched2_invert) -> bool {
      const ParsedSchedRegion region = ParseBlockForSchedModel(candidate_vblk, b, Phase::kSched2);
      if (!region.valid || region.has_mid_block_loop_barrier || region.sched_block.size() <= 1) {
        return false;
      }
      if (require_sched2_invert &&
          !InvertBlockOrder(region.sched_block, facts_s2, mm, Phase::kSched2, live_end)
               .has_value()) {
        return false;
      }
      const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, b);
      const int body_len = bounds.epilogue_begin - bounds.prologue_end;
      if (body_len <= 1) {
        return true;
      }
      std::vector<Insn> body;
      body.reserve(static_cast<size_t>(body_len));
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
        Insn insn = region.sched_block[static_cast<size_t>(i)];
        insn.luid = i - bounds.prologue_end;
        body.push_back(std::move(insn));
      }
      Sched1ProjectionContext proj_ctx = base_proj_ctx;
      proj_ctx.excluded_prologue_body_indices =
          FindInterleavedPrologueSaveBodyIndices(region.sched_block, bounds, b, mm);
      Sched1ProjectionContext unsplit_ctx = proj_ctx;
      unsplit_ctx.allow_post_call_combine_regs_split = false;
      unsplit_ctx.allow_general_combine_regs_split = false;
      const Sched1ProjectionView unsplit_view =
          ProjectPostReloadBlockToSched1View(body, mm, live_end, unsplit_ctx);
      if (unsplit_view.core_insns.size() <= 1 ||
          IsAchievable(unsplit_view.core_insns, unsplit_view.facts_s1, mm, Phase::kSched1,
                       unsplit_view.live_at_end_s1)) {
        return true;
      }
      if (HasSched1AchievablePreimageForSched2Block(region.sched_block, b, bounds, unsplit_view,
                                                    unsplit_ctx, cfg_ctx, facts_s2, mm, live_end,
                                                    /*allow_pure_reader_reg_dead_transfer=*/true)) {
        return true;
      }
      if (!SelectMinimalCombineRegsRmwSplits(body, proj_ctx, live_end, mm,
                                             /*allow_general=*/true)
               .empty()) {
        return true;
      }
      return false;
    };

    // If `reorg.c:fill_simple_delay_slots` sank an independent insn into a
    // non-return branch's delay slot, its pre-`reorg` position is not restored
    // until Stage 1; test with and without that unpacked slot consistently for
    // both the uncollapsed baseline and collapsed trials.
    absl::btree_set<int> live_end_without_jump_slot = live_at_end_s2;
    const std::optional<size_t> jump_slot_vi =
        block_views[b].unpacked_non_return_jump_slot_view_idx;
    if (jump_slot_vi.has_value() && *jump_slot_vi < vblk.instructions.size()) {
      if (auto spn = ParsePatternAndNotes(vblk.instructions[*jump_slot_vi]); spn.ok()) {
        CollectRegReads(spn->pattern, &live_end_without_jump_slot);
      }
    }

    auto check_mask = [&](const std::vector<bool>& m, bool require_sched2_invert) -> bool {
      const RtlBasicBlock trial_vblk =
          ApplyConstDoubleSpansToBlock(vblk, spans, m, /*use_view_indices=*/true);
      if (check_single_view(trial_vblk, live_at_end_s2, require_sched2_invert)) {
        return true;
      }
      if (jump_slot_vi.has_value()) {
        const RtlBasicBlock trimmed_vblk =
            ApplyConstDoubleSpansToBlock(vblk, spans, m, /*use_view_indices=*/true, jump_slot_vi);
        if (check_single_view(trimmed_vblk, live_end_without_jump_slot, require_sched2_invert)) {
          return true;
        }
      }
      return false;
    };

    const std::vector<bool> empty_mask(spans.size(), false);
    if (check_mask(empty_mask, /*require_sched2_invert=*/false)) {
      continue;
    }

    std::vector<bool> mask(spans.size(), true);
    if (!check_mask(mask, /*require_sched2_invert=*/true)) {
      continue;
    }

    // Greedily drop any span not needed for `kSched1` achievability.
    for (size_t k = 0; k < spans.size(); ++k) {
      mask[k] = false;
      if (!check_mask(mask, /*require_sched2_invert=*/true)) {
        mask[k] = true;
      }
    }
    bool any_selected = false;
    for (bool bit : mask) {
      if (bit) {
        any_selected = true;
        break;
      }
    }
    if (!any_selected) {
      continue;
    }
    cfg->rpo_blocks[b] =
        ApplyConstDoubleSpansToBlock(cfg->rpo_blocks[b], spans, mask, /*use_view_indices=*/false);
  }
}

namespace {

// If a later pure-reader store `j > i` of a resolved dead register `r`
// `(set (mem M) (reg r))` moves before `i` in the pre-`sched2` order, `r` no
// longer dies at `j`, so `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`)
// rejects any store-`REG_EQUIV (mem M)` note that Stage 0 speculatively attached
// to the initializer of `r`. Strips that spurious `REG_EQUIV (mem M)` note from
// `seq` and records every modified initializer index in `*touched_sched_idxs`.
void StripSpuriousStoreMemRegEquivForMovedReaders(
    absl::Span<const std::pair<int, int>> resolved_insn_regs, int prologue_end, int epilogue_begin,
    absl::Span<const InsnRegReadDefDeaths> insn_info, const PostReloadStackLayout& stack_layout,
    const MachineModel& mm, absl::Span<const int> pos_in_cand, std::vector<Insn>* seq,
    absl::btree_set<int>* touched_sched_idxs) {
  for (const auto& [i, r] : resolved_insn_regs) {
    for (int j = i + 1; j < epilogue_begin; ++j) {
      if (insn_info[static_cast<size_t>(j)].defs.contains(r)) {
        break;
      }
      if (!pos_in_cand.empty() &&
          pos_in_cand[static_cast<size_t>(j)] >= pos_in_cand[static_cast<size_t>(i)]) {
        continue;
      }
      const RtlExpr& jpat = (*seq)[static_cast<size_t>(j)].pn.pattern;
      if (jpat.op == RtlOp::kSet && jpat.children.size() == 2 && jpat.SetDest().op == RtlOp::kMem &&
          !jpat.SetDest().children.empty() && jpat.SetSrc().op == RtlOp::kReg &&
          !jpat.SetSrc().IsArgPointerReg() && jpat.SetSrc().RegNo() == r) {
        const int init_idx = FindUniqueInitPosForStoreInSeq(*seq, j, r, mm);
        if (init_idx >= prologue_end &&
            HasMatchingMemRegEquivNote((*seq)[static_cast<size_t>(init_idx)], jpat.SetDest(),
                                       stack_layout)) {
          auto& inotes = (*seq)[static_cast<size_t>(init_idx)].pn.reg_notes;
          inotes.erase(std::remove_if(inotes.begin(), inotes.end(),
                                      [&](const RtlRegNote& note) {
                                        return note.kind == RtlMachineMode::kRegEquiv &&
                                               SameMemSlotIgnoringRegNames(
                                                   note.datum, jpat.SetDest(), stack_layout);
                                      }),
                       inotes.end());
          touched_sched_idxs->insert(init_idx);
        }
      }
    }
  }
}

// `local-alloc.c:575-600, 1049-1055` (`validate_equiv_mem`): transferring
// `(REG_DEAD r)` onto `to_idx` invalidates any speculative Stage-0
// `(REG_EQUIV (mem M))` on `to_idx` itself (or on an earlier load `p` whose
// dest is still live at `to_idx` in `cand_in_base`) whenever `M` references a
// hard register covered by `r`.
void StripInvalidatedMemRegEquivAfterDeadTransfers(
    absl::Span<const std::pair<int, RtlRegNote>> dead_targets, int prologue_end,
    absl::Span<const int> cand_in_base, absl::Span<const int> pos_in_cand,
    absl::Span<const InsnRegReadDefDeaths> insn_info, const MachineModel& mm,
    std::vector<Insn>* seq, absl::btree_set<int>* touched_sched_idxs) {
  for (const auto& [to_idx, note] : dead_targets) {
    if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
      continue;
    }
    const int dead_r = note.datum.RegNo();
    if (dead_r < 0 || dead_r >= kMipsFirstPseudoRegister) {
      continue;
    }
    const int dead_nr = mm.HardRegNRegs(dead_r, note.datum.mode);
    auto mem_mentions_dead = [&](const RtlExpr& mem_expr) {
      for (int k = 0; k < dead_nr; ++k) {
        if (mem_expr.ContainsReg(dead_r + k)) {
          return true;
        }
      }
      return false;
    };
    const int to_pos = pos_in_cand[static_cast<size_t>(to_idx)];
    for (int p = to_pos; p >= prologue_end; --p) {
      const int si = cand_in_base[static_cast<size_t>(p)];
      auto& notes = (*seq)[static_cast<size_t>(si)].pn.reg_notes;
      const RtlExpr* s = SingleSetOf((*seq)[static_cast<size_t>(si)].pn.pattern);
      if (s == nullptr || s->SetDest().op != RtlOp::kReg ||
          s->SetDest().RegNo() >= kMipsFirstPseudoRegister) {
        continue;
      }
      const int dst_r = s->SetDest().RegNo();
      const int dst_nr = mm.HardRegNRegs(dst_r, s->SetDest().mode);
      bool dst_live_at_to = (p == to_pos);
      if (!dst_live_at_to) {
        dst_live_at_to = true;
        for (int q = p + 1; q <= to_pos; ++q) {
          const int qi = cand_in_base[static_cast<size_t>(q)];
          if (HasRegDeadNoteCovering((*seq)[static_cast<size_t>(qi)].pn.reg_notes, dst_r, mm)) {
            dst_live_at_to = false;
            break;
          }
          for (int k = 0; k < dst_nr; ++k) {
            if (insn_info[static_cast<size_t>(qi)].defs.contains(dst_r + k)) {
              dst_live_at_to = false;
              break;
            }
          }
          if (!dst_live_at_to) {
            break;
          }
        }
      }
      if (!dst_live_at_to) {
        continue;
      }
      std::vector<RtlRegNote> kept;
      kept.reserve(notes.size());
      bool removed = false;
      for (const RtlRegNote& n : notes) {
        if (n.kind == RtlMachineMode::kRegEquiv && n.datum.op == RtlOp::kMem &&
            mem_mentions_dead(n.datum)) {
          removed = true;
          continue;
        }
        kept.push_back(n);
      }
      if (removed) {
        notes = std::move(kept);
        touched_sched_idxs->insert(si);
      }
    }
  }
}

// Companion to `CollectSched2ObstructedMemRegEquivAdditions` (`local-alloc.c:575,
// 580, 589-599, 1049-1055`): strips speculative Stage-0 `REG_EQUIV (mem ...)`
// notes on non-incoming-arg `MEM` loads when `sched2` reordered `REG_DEAD base_r`
// or `REG_DEAD dst_r` so `validate_equiv_mem` spuriously passed in post-`sched2`
// order (`transferred_base`) while failing in the pre-`sched2` `kSched1`
// preimage (`cand_in_base`).
void StripSched2SpuriousLoadMemRegEquivNotes(
    int prologue_end, int epilogue_begin, absl::Span<const int> cand_in_base,
    absl::Span<const int> pos_in_cand, const RegFacts& facts_s2, const MachineModel& mm,
    const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_prologue_body_indices, std::vector<Insn>* transferred_base,
    absl::btree_set<int>* stripped_equiv_targets) {
  const int n = static_cast<int>(transferred_base->size());
  if (static_cast<int>(cand_in_base.size()) != n || static_cast<int>(pos_in_cand.size()) != n) {
    return;
  }
  std::vector<InsnRegReadDefDeaths> info(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    info[static_cast<size_t>(i)] =
        CollectInsnRegReadDefDeaths((*transferred_base)[static_cast<size_t>(i)], mm);
  }
  std::vector<Insn> cand_seq;
  cand_seq.reserve(static_cast<size_t>(n));
  for (int pos = 0; pos < n; ++pos) {
    cand_seq.push_back(
        (*transferred_base)[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])]);
  }
  absl::btree_set<int> excluded_orig_ids;
  for (int i = 0; i < prologue_end; ++i) {
    excluded_orig_ids.insert(i);
  }
  for (int rel : excluded_prologue_body_indices) {
    excluded_orig_ids.insert(prologue_end + rel);
  }
  for (int oid = prologue_end; oid < epilogue_begin; ++oid) {
    if (excluded_orig_ids.contains(oid)) {
      continue;
    }
    const RtlExpr& pat = (*transferred_base)[static_cast<size_t>(oid)].pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kReg ||
        pat.SetSrc().op != RtlOp::kMem || pat.SetSrc().children.empty()) {
      continue;
    }
    const int dst_r = pat.SetDest().RegNo();
    if (dst_r < 0 || dst_r >= kMipsFirstPseudoRegister) {
      continue;
    }
    int64_t arg_off = 0;
    if (stack_layout.valid && TryExtractPlainStackSlotOffset(pat.SetSrc().children[0], &arg_off) &&
        arg_off >= stack_layout.frame_size) {
      continue;
    }
    if (!HasMatchingMemRegEquivNote((*transferred_base)[static_cast<size_t>(oid)], pat.SetSrc(),
                                    stack_layout)) {
      continue;
    }
    if (!HasLoadEquivInvalidatorMovedIntoWindow(
            oid, pat.SetSrc(), dst_r, prologue_end, epilogue_begin, *transferred_base, cand_in_base,
            pos_in_cand, info, facts_s2, mm, stack_layout, excluded_orig_ids,
            /*allow_moved_store_invalidator=*/true)) {
      continue;
    }
    const int cpos = pos_in_cand[static_cast<size_t>(oid)];
    if (ValidateEquivMemForPostReloadGate(cand_seq, cand_in_base, cpos, dst_r, pat.SetSrc(),
                                          facts_s2, mm, stack_layout, excluded_orig_ids)) {
      continue;
    }
    auto& notes = (*transferred_base)[static_cast<size_t>(oid)].pn.reg_notes;
    const RtlExpr norm_mem = NormalizeMemForRegEquivMatch(pat.SetSrc(), stack_layout);
    std::vector<RtlRegNote> kept;
    kept.reserve(notes.size());
    bool removed = false;
    for (const RtlRegNote& note : notes) {
      if (note.kind == RtlMachineMode::kRegEquiv &&
          (note.datum == pat.SetSrc() ||
           NormalizeMemForRegEquivMatch(note.datum, stack_layout) == norm_mem)) {
        removed = true;
        continue;
      }
      kept.push_back(note);
    }
    if (removed) {
      notes = std::move(kept);
      stripped_equiv_targets->insert(oid);
    }
  }
}

// Companion to `StripSched2SpuriousLoadMemRegEquivNotes` (`local-alloc.c:540-603,
// 990-1000` `validate_equiv_mem`, `memref_used_between_p`, `update_equiv_regs`):
// strips speculative Stage-0 `REG_EQUIV (mem ...)` notes placed on the unique
// initializer of a dying `MEM` store when `sched2` moved an address-base
// write/death, call, or overlapping `MEM` access out of `[init, store)` so
// `validate_equiv_mem` spuriously passed in post-`sched2` order while failing
// in the pre-`sched2` `kSched1` preimage (`cand_in_base`).
void StripSched2SpuriousStoreMemRegEquivNotes(
    int prologue_end, int epilogue_begin, absl::Span<const int> cand_in_base,
    absl::Span<const int> pos_in_cand, const RegFacts& facts_s2, const MachineModel& mm,
    const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_prologue_body_indices, std::vector<Insn>* transferred_base,
    absl::btree_set<int>* stripped_equiv_targets) {
  const int n = static_cast<int>(transferred_base->size());
  if (static_cast<int>(cand_in_base.size()) != n || static_cast<int>(pos_in_cand.size()) != n) {
    return;
  }
  std::vector<InsnRegReadDefDeaths> info(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    info[static_cast<size_t>(i)] =
        CollectInsnRegReadDefDeaths((*transferred_base)[static_cast<size_t>(i)], mm);
  }
  std::vector<Insn> cand_seq;
  cand_seq.reserve(static_cast<size_t>(n));
  for (int pos = 0; pos < n; ++pos) {
    cand_seq.push_back(
        (*transferred_base)[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])]);
  }
  absl::btree_set<int> excluded_orig_ids;
  for (int i = 0; i < prologue_end; ++i) {
    excluded_orig_ids.insert(i);
  }
  for (int rel : excluded_prologue_body_indices) {
    excluded_orig_ids.insert(prologue_end + rel);
  }
  for (int oid = prologue_end; oid < epilogue_begin; ++oid) {
    if (excluded_orig_ids.contains(oid)) {
      continue;
    }
    const Insn& store_insn = (*transferred_base)[static_cast<size_t>(oid)];
    const RtlExpr& pat = store_insn.pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kMem ||
        pat.SetDest().children.empty() || pat.SetSrc().op != RtlOp::kReg ||
        pat.SetSrc().IsArgPointerReg()) {
      continue;
    }
    const int src_r = pat.SetSrc().RegNo();
    if (src_r < 0 || src_r >= kMipsFirstPseudoRegister ||
        !HasRegDeadNoteCovering(store_insn.pn.reg_notes, src_r, mm)) {
      continue;
    }
    const int store_pos = pos_in_cand[static_cast<size_t>(oid)];
    const int init_pos = FindUniqueInitPosForStoreInSeq(cand_seq, store_pos, src_r, mm);
    if (init_pos < 0) {
      continue;
    }
    const int init_oid = cand_in_base[static_cast<size_t>(init_pos)];
    if (!HasMatchingMemRegEquivNote((*transferred_base)[static_cast<size_t>(init_oid)],
                                    pat.SetDest(), stack_layout)) {
      continue;
    }
    if (!HasStoreEquivInvalidatorMovedIntoWindow(
            oid, init_pos, pat.SetDest(), prologue_end, epilogue_begin, *transferred_base,
            cand_in_base, pos_in_cand, info, facts_s2, mm, stack_layout, excluded_orig_ids)) {
      continue;
    }
    const bool valid_in_cand =
        ValidateEquivMemForPostReloadGate(cand_seq, cand_in_base, init_pos, src_r, pat.SetDest(),
                                          facts_s2, mm, stack_layout, excluded_orig_ids) &&
        !MemrefUsedBetweenForPostReloadGate(cand_seq, cand_in_base, init_pos, store_pos,
                                            pat.SetDest(), facts_s2, stack_layout,
                                            excluded_orig_ids);
    if (valid_in_cand) {
      continue;
    }
    auto& notes = (*transferred_base)[static_cast<size_t>(init_oid)].pn.reg_notes;
    const RtlExpr norm_mem = NormalizeMemForRegEquivMatch(pat.SetDest(), stack_layout);
    std::vector<RtlRegNote> kept;
    kept.reserve(notes.size());
    bool removed = false;
    for (const RtlRegNote& note : notes) {
      if (note.kind == RtlMachineMode::kRegEquiv &&
          (note.datum == pat.SetDest() ||
           NormalizeMemForRegEquivMatch(note.datum, stack_layout) == norm_mem)) {
        removed = true;
        continue;
      }
      kept.push_back(note);
    }
    if (removed) {
      notes = std::move(kept);
      stripped_equiv_targets->insert(init_oid);
    }
  }
}

// Inverts `reorg.c:918-920` when `sched2` moved a pure reader `j` past a
// delay-slot insn `i` without updating `REG_DEAD` (`sched.c:3415-3611`), and
// `reorg.c:emit_delay_sequence` subsequently zeroed `i`'s `(REG_DEAD r)` to
// `(REG_DEAD (const_int 0))`. Resolves such live-after-`i` `(const_int 0)`
// placeholders in-place onto `i` ONLY when the resolved block admits a
// 3-gate-verified `kSched2` preimage (`IsAchievable(kSched1)` + exact `kSched2`
// replay + `PreservesPostReloadOrderSafety(..., allow_transfer=false)`).
bool TryResolveStage0DelaySlotConstZeroInPlace(
    const ParsedSchedRegion& base_region, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionContext& proj_ctx, const CfgSched1Context& cfg_ctx,
    const RegFacts& facts_s2, const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2,
    absl::Span<const AdjacentPair> reload_adjacent,
    absl::Span<const InsnRegReadDefDeaths> insn_info,
    absl::Span<const absl::btree_set<int>> live_after_base,
    const absl::btree_set<int>& alignment_touched_sched_idxs, Stage0BlockDelayView* block_view,
    RtlBasicBlock* orig_blk) {
  const int n = static_cast<int>(base_region.sched_block.size());
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return false;
  }

  std::vector<int> identity_order(static_cast<size_t>(n));
  for (int idx = 0; idx < n; ++idx) {
    identity_order[static_cast<size_t>(idx)] = idx;
  }
  std::vector<Insn> resolved_base = base_region.sched_block;
  absl::btree_set<int> resolved_sched_idxs;
  std::vector<std::pair<int, int>> resolved_insn_regs;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    if (insn_info[static_cast<size_t>(i)].is_call_or_use) {
      continue;
    }
    const size_t vi = base_region.sched_orig_indices[static_cast<size_t>(i)];
    if (vi >= block_view->view_to_orig.size() || !block_view->view_to_orig[vi].is_delay_slot ||
        block_view->view_to_orig[vi].had_placeholder_dead) {
      continue;
    }
    auto& notes = resolved_base[static_cast<size_t>(i)].pn.reg_notes;
    std::vector<size_t> zero_idxs;
    for (size_t ni = 0; ni < notes.size(); ++ni) {
      if (notes[ni].kind == RtlMachineMode::kRegDead && notes[ni].datum.op == RtlOp::kConstInt &&
          notes[ni].datum.int_val == 0) {
        zero_idxs.push_back(ni);
      }
    }
    if (zero_idxs.empty()) {
      continue;
    }
    size_t zero_cursor = 0;
    absl::btree_set<int> handled_regs;
    for (int r : insn_info[static_cast<size_t>(i)].reads) {
      if (zero_cursor >= zero_idxs.size()) {
        break;
      }
      if (r <= 0 || r >= kMipsFirstPseudoRegister || r == 28 || r == kMipsStackPointerRegno ||
          (cfg_ctx.stack_layout.frame_pointer_needed && r == kMipsFramePointerRegno) ||
          handled_regs.contains(r) || HasRegDeadNoteCovering(notes, r, mm)) {
        continue;
      }
      const std::optional<RtlExpr> reg_expr =
          FindUsedHardRegExprInPattern(resolved_base[static_cast<size_t>(i)].pn.pattern, r);
      if (!reg_expr.has_value()) {
        continue;
      }
      const int nr = mm.HardRegNRegs(r, reg_expr->mode);
      bool defs_r = false;
      bool live_after_in_base = false;
      for (int k = 0; k < nr; ++k) {
        if (insn_info[static_cast<size_t>(i)].defs.contains(r + k)) {
          defs_r = true;
          break;
        }
        if (live_after_base[static_cast<size_t>(i)].contains(r + k)) {
          live_after_in_base = true;
        }
      }
      if (defs_r || !live_after_in_base ||
          IsCrossBankFprInputReloadOperandInOrder(base_region.sched_block, identity_order, i, r,
                                                  reg_expr->mode, mm)) {
        continue;
      }
      notes[zero_idxs[zero_cursor++]].datum = *reg_expr;
      resolved_sched_idxs.insert(i);
      resolved_insn_regs.push_back({i, r});
      for (int k = 0; k < nr; ++k) {
        handled_regs.insert(r + k);
      }
    }
  }
  if (resolved_sched_idxs.empty()) {
    return false;
  }

  StripSpuriousStoreMemRegEquivForMovedReaders(
      resolved_insn_regs, prologue_end, epilogue_begin, insn_info, cfg_ctx.stack_layout, mm,
      /*pos_in_cand=*/{}, &resolved_base, &resolved_sched_idxs);

  // Build initial pre-`sched2` preferences placing every later pure reader `j > i`
  // of each resolved dead register `r` before `i`: first at `j`'s earliest
  // DAG-legal position (right after its last `kSched2` dependence target, so
  // equal-priority `kSched1` LUID ties match `08.sched`), and second immediately
  // before `i`.
  const DepGraph resolved_graph = Analyze(resolved_base, facts_s2, mm, Phase::kSched2);
  auto build_pref_with_readers_moved = [&](bool earliest_dag_pos) -> std::vector<int> {
    std::vector<int> pref(static_cast<size_t>(n));
    for (int idx = 0; idx < n; ++idx) {
      pref[static_cast<size_t>(idx)] = idx;
    }
    for (const auto& [i, r] : resolved_insn_regs) {
      std::vector<int> later_readers;
      for (int j = i + 1; j < epilogue_begin; ++j) {
        if (insn_info[static_cast<size_t>(j)].defs.contains(r)) {
          break;
        }
        if (insn_info[static_cast<size_t>(j)].reads.contains(r) &&
            !insn_info[static_cast<size_t>(j)].is_call_or_use) {
          later_readers.push_back(j);
        }
      }
      for (int j : later_readers) {
        auto it_j = std::find(pref.begin(), pref.end(), j);
        auto it_i = std::find(pref.begin(), pref.end(), i);
        if (it_j == pref.end() || it_i == pref.end() || it_j <= it_i) {
          continue;
        }
        pref.erase(it_j);
        it_i = std::find(pref.begin(), pref.end(), i);
        auto insert_it = it_i;
        if (earliest_dag_pos) {
          absl::btree_set<int> deps;
          for (const Link& l : resolved_graph.log_links[static_cast<size_t>(j)]) {
            deps.insert(l.target);
          }
          auto after_last_dep = pref.begin() + prologue_end;
          for (auto it = pref.begin(); it != it_i; ++it) {
            if (deps.contains(*it)) {
              after_last_dep = it + 1;
            }
          }
          if (after_last_dep <= it_i) {
            insert_it = after_last_dep;
          }
        }
        pref.insert(insert_it, j);
      }
    }
    return pref;
  };
  Sched1ProjectionContext unsplit_ctx = proj_ctx;
  unsplit_ctx.allow_post_call_combine_regs_split = false;
  unsplit_ctx.allow_general_combine_regs_split = false;
  std::vector<std::vector<int>> cands;
  for (bool earliest : {true, false}) {
    const std::vector<int> initial_pref = build_pref_with_readers_moved(earliest);
    if (std::optional<std::vector<int>> init_cand =
            InvertBlockOrder(resolved_base, facts_s2, mm, Phase::kSched2, live_at_end_s2,
                             initial_pref, reload_adjacent);
        init_cand.has_value()) {
      cands.push_back(*init_cand);
      // Also run one fixed-point `kSched1` refinement starting from `init_cand`.
      std::vector<Insn> init_seq;
      init_seq.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        Insn insn = resolved_base[static_cast<size_t>((*init_cand)[static_cast<size_t>(pos)])];
        insn.luid = pos;
        init_seq.push_back(std::move(insn));
      }
      std::vector<Insn> init_body;
      init_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        Insn insn = init_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        init_body.push_back(std::move(insn));
      }
      Sched1ProjectionContext init_ctx = unsplit_ctx;
      init_ctx.excluded_prologue_body_indices =
          FindInterleavedPrologueSaveBodyIndices(init_seq, bounds, block_idx, mm);
      const Sched1ProjectionView init_view =
          ProjectPostReloadBlockToSched1View(init_body, mm, live_at_end_s2, init_ctx);
      std::vector<int> pref_in_init;
      if (init_view.core_insns.size() > 1 &&
          BuildPreferredSched1OrderInBase(init_seq, prologue_end, epilogue_begin, init_view,
                                          init_ctx.excluded_prologue_body_indices, mm,
                                          &pref_in_init)) {
        std::vector<int> pref_in_base(static_cast<size_t>(n));
        for (int pos = 0; pos < n; ++pos) {
          pref_in_base[static_cast<size_t>(pos)] =
              (*init_cand)[static_cast<size_t>(pref_in_init[static_cast<size_t>(pos)])];
        }
        if (std::optional<std::vector<int>> refined =
                InvertBlockOrder(resolved_base, facts_s2, mm, Phase::kSched2, live_at_end_s2,
                                 pref_in_base, reload_adjacent);
            refined.has_value()) {
          cands.push_back(std::move(*refined));
        }
      }
    }
  }
  for (auto it = cands.rbegin(); it != cands.rend(); ++it) {
    const std::vector<int>& cand_in_base = *it;
    std::vector<Insn> cand_seq;
    cand_seq.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      Insn insn = resolved_base[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
      insn.luid = pos;
      cand_seq.push_back(std::move(insn));
    }
    std::vector<Insn> cand_body;
    cand_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      Insn insn = cand_seq[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      cand_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext cand_ctx = unsplit_ctx;
    cand_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, block_idx, mm);
    const Sched1ProjectionView cand_view =
        ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
    if (cand_view.core_insns.size() <= 1 ||
        !IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm, Phase::kSched1,
                      cand_view.live_at_end_s1)) {
      continue;
    }
    const DepGraph g_cand = Analyze(cand_seq, facts_s2, mm, Phase::kSched2);
    const ScheduleResult s2_replay =
        Schedule(cand_seq, g_cand, facts_s2, mm, Phase::kSched2, live_at_end_s2);
    if (static_cast<int>(s2_replay.order.size()) != n) {
      continue;
    }
    bool replay_matches = true;
    for (int pos = 0; pos < n; ++pos) {
      if (cand_in_base[static_cast<size_t>(s2_replay.order[static_cast<size_t>(pos)])] != pos) {
        replay_matches = false;
        break;
      }
    }
    if (!replay_matches) {
      continue;
    }
    if (!PreservesPostReloadOrderSafety(resolved_base, cand_in_base, prologue_end, epilogue_begin,
                                        live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                                        /*allow_pure_reader_reg_dead_transfer=*/false,
                                        cfg_ctx.stack_layout,
                                        proj_ctx.excluded_prologue_body_indices)) {
      continue;
    }
    absl::btree_set<int> touched = alignment_touched_sched_idxs;
    touched.insert(resolved_sched_idxs.begin(), resolved_sched_idxs.end());
    CommitStage0AttributedNotesToBlock(base_region, resolved_base, touched, block_view, orig_blk);
    return true;
  }
  return false;
}

// Symmetric inverse of `SelectMinimalCombineRegsRmwSplits`:
// In GCC 2.7.2, `flow.c:2506-2515` (`mark_used_regs`) never attaches `REG_DEAD`
// to a self-modifying RMW `(set r (op r ...))` on a single multi-set pseudo,
// whereas `local-alloc.c:1266-1300` (`combine_regs`) merges distinct single-set
// pseudos onto the same hard register `r`, leaving an erased `REG_DEAD` that
// Stage 0's local RMW heuristics may speculatively restore. When all preimage
// and split passes fail to make the block `Phase::kSched1`-achievable
// (`toplev.c:3028-3053`, `sched.c:2490` `birthing_insn_p`), retracts a minimal
// subset of speculative same-mode self-set `(REG_DEAD r)` notes ONLY when both
// `Phase::kSched2` replay and `Phase::kSched1` achievability succeed.
bool TryRetractSpeculativeSelfSetRmwRegDeadNotes(
    const ParsedSchedRegion& base_region, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionContext& unsplit_base_ctx, const CfgSched1Context& cfg_ctx,
    const RegFacts& facts_s2, const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2,
    const absl::btree_set<int>& alignment_touched_sched_idxs, Stage0BlockDelayView* block_view,
    RtlBasicBlock* orig_blk) {
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return false;
  }
  struct SelfDeadCand {
    int sched_idx = -1;
    int regno = -1;
    RtlMachineMode mode = RtlMachineMode::kVoid;
  };
  std::vector<SelfDeadCand> cands;
  absl::btree_set<int> defined_so_far;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    const Insn& insn = base_region.sched_block[static_cast<size_t>(i)];
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
    const RtlExpr* s = SingleSetOf(insn.pn.pattern);
    if (insn.kind == Insn::Kind::kInsn && s != nullptr && s->SetDest().op == RtlOp::kReg &&
        !s->SetDest().flags.integrated && !s->SetDest().flags.frame_related &&
        s->SetSrc().op != RtlOp::kReg && s->SetSrc().op != RtlOp::kMem &&
        !s->SetSrc().children.empty()) {
      const int r = s->SetDest().RegNo();
      const RtlMachineMode mode = s->SetDest().mode;
      const bool has_blocking_note =
          std::any_of(insn.pn.reg_notes.begin(), insn.pn.reg_notes.end(), [](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual ||
                   n.kind == RtlMachineMode::kRegLibcall || n.kind == RtlMachineMode::kRegRetval ||
                   n.kind == RtlMachineMode::kRegNoConflict;
          });
      const bool has_exact_self_dead =
          std::any_of(insn.pn.reg_notes.begin(), insn.pn.reg_notes.end(), [&](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                   n.datum.RegNo() == r && n.datum.mode == mode;
          });
      bool reads_same_mode_r = false;
      s->SetSrc().Visit([&](const RtlExpr& e) {
        if (e.op == RtlOp::kReg && e.RegNo() == r && e.mode == mode) {
          reads_same_mode_r = true;
        }
      });
      const bool incoming_entry_arg = unsplit_base_ctx.is_entry_block &&
                                      IsAbiCallInterfaceHardReg(r) && !defined_so_far.contains(r);
      if (r >= 2 && r < kMipsFirstPseudoRegister && r != 28 && r != kMipsStackPointerRegno &&
          r != kMipsFramePointerRegno && !has_blocking_note && has_exact_self_dead &&
          reads_same_mode_r && !incoming_entry_arg) {
        cands.push_back(SelfDeadCand{.sched_idx = i, .regno = r, .mode = mode});
      }
    }
    defined_so_far.insert(fdefs.begin(), fdefs.end());
    defined_so_far.insert(pdefs.begin(), pdefs.end());
  }
  if (cands.empty()) {
    return false;
  }

  auto try_retract_subset = [&](absl::Span<const SelfDeadCand> subset, bool allow_sched2_preimage,
                                bool allow_tie_flips) -> bool {
    std::vector<Insn> updated_base = base_region.sched_block;
    absl::btree_set<int> touched;
    for (const SelfDeadCand& c : subset) {
      auto& notes = updated_base[static_cast<size_t>(c.sched_idx)].pn.reg_notes;
      const size_t before = notes.size();
      notes.erase(std::remove_if(notes.begin(), notes.end(),
                                 [&](const RtlRegNote& n) {
                                   return n.kind == RtlMachineMode::kRegDead &&
                                          n.datum.op == RtlOp::kReg && n.datum.RegNo() == c.regno &&
                                          n.datum.mode == c.mode;
                                 }),
                  notes.end());
      if (notes.size() < before) {
        touched.insert(c.sched_idx);
      }
    }
    if (touched.empty() ||
        !InvertBlockOrder(updated_base, facts_s2, mm, Phase::kSched2, live_at_end_s2).has_value()) {
      return false;
    }
    std::vector<Insn> updated_body;
    updated_body.reserve(static_cast<size_t>(body_len));
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      Insn insn = updated_base[static_cast<size_t>(i)];
      insn.luid = i - prologue_end;
      updated_body.push_back(std::move(insn));
    }
    const Sched1ProjectionView verify_view =
        ProjectPostReloadBlockToSched1View(updated_body, mm, live_at_end_s2, unsplit_base_ctx);
    if (verify_view.core_insns.size() <= 1) {
      return false;
    }
    const bool ok = allow_sched2_preimage
                        ? HasSched1AchievablePreimageForSched2Block(
                              updated_base, block_idx, bounds, verify_view, unsplit_base_ctx,
                              cfg_ctx, facts_s2, mm, live_at_end_s2,
                              /*allow_pure_reader_reg_dead_transfer=*/false, allow_tie_flips)
                        : IsAchievable(verify_view.core_insns, verify_view.facts_s1, mm,
                                       Phase::kSched1, verify_view.live_at_end_s1);
    if (!ok) {
      return false;
    }
    touched.insert(alignment_touched_sched_idxs.begin(), alignment_touched_sched_idxs.end());
    CommitStage0AttributedNotesToBlock(base_region, updated_base, touched, block_view, orig_blk);
    return true;
  };

  // Prefer single-note retraction first (minimal change), checking direct
  // `kSched1` achievability before falling back to a primary `kSched2` preimage
  // and (on bounded-size blocks) a tie-flip `kSched2` preimage.
  const std::vector<std::pair<bool, bool>> modes =
      (body_len <= 48 && cands.size() <= 10)
          ? std::vector<std::pair<bool, bool>>{{false, false}, {true, false}, {true, true}}
          : std::vector<std::pair<bool, bool>>{{false, false}, {true, false}};
  for (const auto& [allow_sched2_preimage, allow_tie_flips] : modes) {
    for (const SelfDeadCand& c : cands) {
      if (try_retract_subset(absl::MakeConstSpan(&c, 1), allow_sched2_preimage, allow_tie_flips)) {
        return true;
      }
    }
    if (cands.size() > 1 && cands.size() <= 6) {
      if (try_retract_subset(cands, allow_sched2_preimage, allow_tie_flips)) {
        return true;
      }
    }
  }
  return false;
}

// Mirrors `local-alloc.c:1266-1300, 1722-1886` (`block_alloc` -> `combine_regs`)
// and `config/mips/mips.h:1280-1284` (`MODES_TIEABLE_P`): if a block-local
// single-set instruction `i` (`dst = H_dst`, not reading `H_dst`) carries a
// pure-reader `(REG_DEAD H_u)` on its first dying same-bank tieable operand
// `H_u != H_dst` (where `H_u` was defined earlier in the same block), `local-alloc`
// would unconditionally merge `dst` into `H_u`'s quantity (`local-alloc.c:1855-1860`),
// contradicting `H_dst != H_u`. When `sched2` (`sched.c:3764`) hoisted an earlier
// conflict-free pure reader `j < i` of `H_u` ahead of `i` and `final.c` erased
// `REG_DEAD`, transferring `(REG_DEAD H_u)` from `i` back to `j` (`pos(i) < pos(j)`)
// restores the true pre-`sched2` death site while strictly verifying `kSched2`
// replay, `PreservesPostReloadOrderSafety(..., allow_transfer=false)`, and
// `IsAchievable(kSched1)`.
struct CombineRegsConflictSite {
  int insn_idx = -1;
  int regno = -1;
  size_t note_idx = 0;
};

struct PureReaderRepairMove {
  int from_idx = -1;
  int to_idx = -1;
  int regno = -1;
  size_t note_idx = 0;
};

// Detects the first `local-alloc.c:1266-1300, 1851-1860` (`combine_regs`) or
// `local-alloc.c:2106, 2158-2177` (`find_free_reg` ascending MIPS regno scan,
// `#ifndef REG_ALLOC_ORDER`) pure-reader `(REG_DEAD r)` conflict at `idx`.
std::optional<CombineRegsConflictSite> FindFirstLowerOrderCombineRegsConflict(
    const std::vector<Insn>& seq, const std::vector<InsnRegReadDefDeaths>& info, int idx,
    int prologue_end, int epilogue_begin, const absl::btree_set<int>& live_at_end_s2,
    const MachineModel& mm, bool fp_needed, int extra_dead_reg) {
  if (idx < prologue_end || idx >= epilogue_begin ||
      info[static_cast<size_t>(idx)].is_call_or_use) {
    return std::nullopt;
  }
  auto is_allocatable_bank_reg = [&](int r) {
    return (r >= 2 && r <= 25) || (r == kMipsFramePointerRegno && !fp_needed) ||
           (r >= 32 && r <= 63);
  };
  auto is_float_mode = [](RtlMachineMode m) {
    return m == RtlMachineMode::kSf || m == RtlMachineMode::kDf;
  };
  auto modes_tieable = [&](RtlMachineMode m1, RtlMachineMode m2) {
    return is_float_mode(m1) == is_float_mode(m2);
  };
  auto collect_direct_nonmem_hard_regs = [](const RtlExpr& expr,
                                            std::vector<std::pair<int, RtlMachineMode>>* out_regs) {
    auto walk = [&](const auto& self, const RtlExpr& e) -> void {
      if (e.op == RtlOp::kMem) {
        return;
      }
      if (e.op == RtlOp::kReg && !e.IsArgPointerReg() && e.RegNo() >= 0 &&
          e.RegNo() < kMipsFirstPseudoRegister) {
        out_regs->emplace_back(e.RegNo(), e.mode);
        return;
      }
      if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg &&
          !e.children[0].IsArgPointerReg() && e.children[0].RegNo() >= 0 &&
          e.children[0].RegNo() < kMipsFirstPseudoRegister) {
        out_regs->emplace_back(e.children[0].RegNo(), e.children[0].mode);
        return;
      }
      for (const RtlExpr& ch : e.children) {
        self(self, ch);
      }
    };
    walk(walk, expr);
  };

  const RtlExpr* st = SingleSetOf(seq[static_cast<size_t>(idx)].pn.pattern);
  if (st == nullptr || st->SetDest().op != RtlOp::kReg || st->SetDest().IsArgPointerReg() ||
      st->SetDest().flags.integrated) {
    return std::nullopt;
  }
  const int dst_r = st->SetDest().RegNo();
  const RtlMachineMode dst_mode = st->SetDest().mode;
  if (!is_allocatable_bank_reg(dst_r)) {
    return std::nullopt;
  }
  const int dst_nr = mm.HardRegNRegs(dst_r, dst_mode);
  // `flow.c:2506-2515` + `local-alloc.c:1021, 1266-1300, 1851`: an RMW reading
  // `dst_r` WITHOUT `(REG_DEAD dst_r)` keeps the same multi-set pseudo
  // (`reg_qty != -1`, skipping `combine_regs`), EXCEPT when `idx` carries
  // `REG_EQUIV` (`reg_n_sets == 1` proves `SET_DEST` is a fresh single-set
  // pseudo whose input `dst_r` was a distinct dying pseudo whose `REG_DEAD`
  // was erased after `alter_reg` gave both `dst_r`). In both fresh-dest
  // cases, `combine_regs` tries operands in order, so an earlier dying
  // `ur != dst_r` causes a Conflict Kind 1 false merge.
  const bool has_reg_equiv =
      std::any_of(seq[static_cast<size_t>(idx)].pn.reg_notes.begin(),
                  seq[static_cast<size_t>(idx)].pn.reg_notes.end(),
                  [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegEquiv; });
  bool reads_dst = false;
  bool self_rmw_without_death = false;
  for (int k = 0; k < dst_nr; ++k) {
    if (info[static_cast<size_t>(idx)].reads.contains(dst_r + k)) {
      reads_dst = true;
      if (!info[static_cast<size_t>(idx)].deaths.contains(dst_r + k) && !has_reg_equiv) {
        self_rmw_without_death = true;
        break;
      }
    }
  }
  if (self_rmw_without_death) {
    return std::nullopt;
  }
  // Check that `dst_r` is block-local (not live at block exit without a later
  // redefinition inside `[idx + 1, epilogue_begin)`).
  bool dst_redefined = false;
  for (int k = idx + 1; k < epilogue_begin; ++k) {
    if (info[static_cast<size_t>(k)].defs.contains(dst_r)) {
      dst_redefined = true;
      break;
    }
  }
  if (!dst_redefined && live_at_end_s2.contains(dst_r)) {
    return std::nullopt;
  }

  const auto& notes = seq[static_cast<size_t>(idx)].pn.reg_notes;
  std::vector<std::pair<int, RtlMachineMode>> rhs_regs;
  collect_direct_nonmem_hard_regs(st->SetSrc(), &rhs_regs);
  for (const auto& [ur, um] : rhs_regs) {
    if (!is_allocatable_bank_reg(ur) || (ur < 32) != (dst_r < 32) || !modes_tieable(um, dst_mode)) {
      continue;
    }
    size_t found_ni = notes.size();
    bool dies_here = (ur == extra_dead_reg) || (ur == dst_r && has_reg_equiv);
    if (!dies_here) {
      for (size_t ni = 0; ni < notes.size(); ++ni) {
        if (notes[ni].kind == RtlMachineMode::kRegDead && notes[ni].datum.op == RtlOp::kReg &&
            !notes[ni].datum.IsArgPointerReg() && notes[ni].datum.RegNo() == ur) {
          dies_here = true;
          found_ni = ni;
          break;
        }
      }
    }
    if (!dies_here) {
      continue;
    }
    if (ur == dst_r) {
      return std::nullopt;
    }
    const int u_nr = mm.HardRegNRegs(ur, um);
    bool u_def_at_idx = false;
    for (int k = 0; k < u_nr; ++k) {
      if (info[static_cast<size_t>(idx)].defs.contains(ur + k)) {
        u_def_at_idx = true;
        break;
      }
    }
    if (u_def_at_idx) {
      continue;
    }
    // Verify `ur` was defined earlier in the same block (block-local quantity).
    bool u_defined_locally = false;
    for (int rev = idx - 1; rev >= prologue_end; --rev) {
      if (info[static_cast<size_t>(rev)].defs.contains(ur)) {
        u_defined_locally = !info[static_cast<size_t>(rev)].is_call_or_use;
        break;
      }
    }
    if (!u_defined_locally) {
      continue;
    }
    return CombineRegsConflictSite{.insn_idx = idx, .regno = ur, .note_idx = found_ni};
  }

  // Conflict Kind 2 (`local-alloc.c:2106, 2158-2177` `find_free_reg`,
  // `#ifndef REG_ALLOC_ORDER` at `local-alloc.c:2163`, `global.c:962-985`
  // `find_reg`, `config/mips/mips.h:1192, 1203`):
  // if a pure-reader `(REG_DEAD r)` at `idx` kills a same-bank register `r`
  // strictly earlier in MIPS's default allocation order than `dst_r`, and `r`
  // is valid for `dst_r`'s mode and completely free across `dst_r`'s live
  // interval `[idx, dst_death]` (with no call crossed when `r` is call-used),
  // then neither `local-alloc` nor `global-alloc` could assign `dst_r`
  // instead of `r`. Skip when `reads_dst` because `dst_r` inherits its dying
  // input's quantity via `combine_regs` rather than allocating independently.
  if (reads_dst) {
    return std::nullopt;
  }
  int dst_death = -1;
  bool saw_call = false;
  bool dst_crosses_call = false;
  for (int k = idx + 1; k < epilogue_begin; ++k) {
    if (seq[static_cast<size_t>(k)].kind == Insn::Kind::kCall) {
      saw_call = true;
      bool call_clobbers_dst = false;
      for (int w = 0; w < dst_nr; ++w) {
        if (mm.CallUsed(dst_r + w)) {
          call_clobbers_dst = true;
          break;
        }
      }
      if (call_clobbers_dst) {
        break;
      }
    }
    bool k_reads_dst = false;
    bool k_defs_dst = false;
    bool k_deaths_dst = false;
    for (int w = 0; w < dst_nr; ++w) {
      if (info[static_cast<size_t>(k)].reads.contains(dst_r + w)) {
        k_reads_dst = true;
      }
      if (info[static_cast<size_t>(k)].defs.contains(dst_r + w)) {
        k_defs_dst = true;
      }
      if (info[static_cast<size_t>(k)].deaths.contains(dst_r + w)) {
        k_deaths_dst = true;
      }
    }
    if (k_reads_dst) {
      dst_death = k;
      if (saw_call) {
        dst_crosses_call = true;
      }
      if (k_deaths_dst && !k_defs_dst) {
        break;
      }
    }
    if (k_defs_dst && !k_reads_dst) {
      break;
    }
  }
  if (dst_death <= idx) {
    return std::nullopt;
  }
  auto check_lower_order_dead_reg = [&](int r) -> bool {
    if (!is_allocatable_bank_reg(r) || (r < 32) != (dst_r < 32) || r >= dst_r) {
      return false;
    }
    if (dst_crosses_call && mm.CallUsed(r)) {
      return false;
    }
    const int r_nr = mm.HardRegNRegs(r, dst_mode);
    if (r_nr > 1 && (r % 2) != 0) {
      return false;
    }
    if (r >= 32 && mm.HardRegNRegs(32, RtlMachineMode::kDf) > 1 && (r % 2) != 0) {
      return false;
    }
    if (r < 32 && r != kMipsFramePointerRegno && r + r_nr > 26) {
      return false;
    }
    if (r >= 32 && r + r_nr > 64) {
      return false;
    }
    for (int w = 0; w < r_nr; ++w) {
      if (info[static_cast<size_t>(idx)].defs.contains(r + w)) {
        return false;
      }
      if (dst_crosses_call && mm.CallUsed(r + w)) {
        return false;
      }
    }
    // Check `r` is completely unused over `(idx, dst_death]` (free for `dst_r`).
    // When `r == extra_dead_reg`, we are evaluating the hypothesis that `r`
    // dies at `idx` (so all remaining pure reads of `r` in the same live
    // range are placed before `idx`); only a redefinition of `r` in
    // `(idx, dst_death]` makes `r` busy.
    for (int k = idx + 1; k <= dst_death; ++k) {
      for (int w = 0; w < r_nr; ++w) {
        if (info[static_cast<size_t>(k)].defs.contains(r + w)) {
          return false;
        }
        if (r != extra_dead_reg && info[static_cast<size_t>(k)].reads.contains(r + w)) {
          return false;
        }
      }
    }
    // Verify `r` was defined earlier in `[prologue_end, idx)` (not an ABI arg).
    bool r_defined_locally = false;
    for (int rev = idx - 1; rev >= prologue_end; --rev) {
      if (info[static_cast<size_t>(rev)].defs.contains(r)) {
        r_defined_locally = !info[static_cast<size_t>(rev)].is_call_or_use;
        break;
      }
    }
    return r_defined_locally;
  };
  if (extra_dead_reg >= 0 && check_lower_order_dead_reg(extra_dead_reg)) {
    return CombineRegsConflictSite{
        .insn_idx = idx, .regno = extra_dead_reg, .note_idx = notes.size()};
  }
  for (size_t ni = 0; ni < notes.size(); ++ni) {
    if (notes[ni].kind == RtlMachineMode::kRegDead && notes[ni].datum.op == RtlOp::kReg &&
        !notes[ni].datum.IsArgPointerReg() && check_lower_order_dead_reg(notes[ni].datum.RegNo())) {
      return CombineRegsConflictSite{
          .insn_idx = idx, .regno = notes[ni].datum.RegNo(), .note_idx = ni};
    }
  }
  return std::nullopt;
}

// Verifies a proposed pure-reader `(REG_DEAD r)` transfer `rm` under candidate
// pre-`sched2` permutation `cand_perm` across Gates (a) `kSched2` replay, (b)
// `PreservesPostReloadOrderSafety`, (c) `IsAchievable(kSched1)`, and Stage 0 ->
// Stage 3 inverted-`REG_DEAD` soundness (`flow.c:2506`, `sched.c:3415-3611`,
// `local-alloc.c:540-665, 990-1055`), committing upon full verification.
bool VerifyAndCommitPureReaderTransfer(
    size_t block_idx, const PostReloadBodyBounds& bounds, const Sched1ProjectionContext& proj_ctx,
    const CfgSched1Context& cfg_ctx, const RegFacts& facts_s2, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, absl::Span<const AdjacentPair> reload_adjacent,
    const std::vector<InsnRegReadDefDeaths>& info, const std::vector<Insn>& trial_base,
    const PureReaderRepairMove& rm, const RtlRegNote& moved_note,
    bool allow_moved_store_load_equiv_invalidation, const std::vector<int>& cand_perm,
    std::vector<Insn>* base_block, std::vector<int>* cur_cand,
    absl::btree_set<int>* touched_sched_idxs) {
  const int n = static_cast<int>(trial_base.size());
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (static_cast<int>(cand_perm.size()) != n) {
    return false;
  }
  std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
  for (int p = 0; p < n; ++p) {
    pos_in_cand[static_cast<size_t>(cand_perm[static_cast<size_t>(p)])] = p;
  }
  if (pos_in_cand[static_cast<size_t>(rm.from_idx)] >=
      pos_in_cand[static_cast<size_t>(rm.to_idx)]) {
    return false;
  }
  std::vector<Insn> cand_trial_base = trial_base;
  absl::btree_set<int> stripped_equiv_targets;
  const std::vector<std::pair<int, RtlRegNote>> dead_targets = {{rm.to_idx, moved_note}};
  StripInvalidatedMemRegEquivAfterDeadTransfers(dead_targets, prologue_end, cand_perm, pos_in_cand,
                                                info, mm, &cand_trial_base,
                                                &stripped_equiv_targets);
  StripSched2SpuriousLoadMemRegEquivNotes(
      prologue_end, epilogue_begin, cand_perm, pos_in_cand, facts_s2, mm, cfg_ctx.stack_layout,
      proj_ctx.excluded_prologue_body_indices, &cand_trial_base, &stripped_equiv_targets);
  const bool cand_allow_store_load_inval =
      allow_moved_store_load_equiv_invalidation || !stripped_equiv_targets.empty();
  std::vector<Insn> trial_cand;
  trial_cand.reserve(static_cast<size_t>(n));
  for (int p = 0; p < n; ++p) {
    Insn insn = cand_trial_base[static_cast<size_t>(cand_perm[static_cast<size_t>(p)])];
    insn.luid = p;
    trial_cand.push_back(std::move(insn));
  }
  // Discover `REG_EQUIV (mem ...)` notes valid in `trial_cand` (`cv == 1`) that
  // became obstructed in `cand_trial_base` (`bv == 0`) once `REG_DEAD` moved to
  // the hoisted `rm.to_idx` (`local-alloc.c:540-665, 990-1000, 1049-1055`).
  const std::vector<Stage0MemEquivAddition> equiv_additions =
      CollectSched2ObstructedMemRegEquivAdditions(
          cand_trial_base, trial_cand, cand_perm, prologue_end, epilogue_begin, live_at_end_s2,
          facts_s2, mm, cfg_ctx.stack_layout, proj_ctx.excluded_prologue_body_indices);
  absl::btree_set<int> added_equiv_targets;
  for (const Stage0MemEquivAddition& add : equiv_additions) {
    added_equiv_targets.insert(add.target_sched_idx);
    auto& bnotes = cand_trial_base[static_cast<size_t>(add.target_sched_idx)].pn.reg_notes;
    bnotes.insert(bnotes.begin(), add.note);
    auto& cnotes =
        trial_cand[static_cast<size_t>(pos_in_cand[static_cast<size_t>(add.target_sched_idx)])]
            .pn.reg_notes;
    cnotes.insert(cnotes.begin(), add.note);
  }
  // Gate (a): exact forward `kSched2` replay reproduces `cand_trial_base`.
  const DepGraph g_s2 = Analyze(trial_cand, facts_s2, mm, Phase::kSched2);
  const ScheduleResult s2_res =
      Schedule(trial_cand, g_s2, facts_s2, mm, Phase::kSched2, live_at_end_s2);
  if (static_cast<int>(s2_res.order.size()) != n) {
    return false;
  }
  for (int p = 0; p < n; ++p) {
    if (cand_perm[static_cast<size_t>(s2_res.order[static_cast<size_t>(p)])] != p) {
      return false;
    }
  }
  // Gate (b): strict post-reload safety (`allow_transfer=false`).
  if (!PreservesPostReloadOrderSafety(cand_trial_base, cand_perm, prologue_end, epilogue_begin,
                                      live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                                      /*allow_pure_reader_reg_dead_transfer=*/false,
                                      cfg_ctx.stack_layout, proj_ctx.excluded_prologue_body_indices,
                                      cand_allow_store_load_inval)) {
    return false;
  }
  // Gate (c): `IsAchievable(kSched1)` on `trial_cand`'s body.
  std::vector<Insn> trial_body;
  trial_body.reserve(static_cast<size_t>(body_len));
  for (int p = prologue_end; p < epilogue_begin; ++p) {
    Insn insn = trial_cand[static_cast<size_t>(p)];
    insn.luid = p - prologue_end;
    trial_body.push_back(std::move(insn));
  }
  Sched1ProjectionContext verify_ctx = proj_ctx;
  verify_ctx.allow_post_call_combine_regs_split = false;
  verify_ctx.allow_general_combine_regs_split = false;
  verify_ctx.excluded_prologue_body_indices =
      FindInterleavedPrologueSaveBodyIndices(trial_cand, bounds, block_idx, mm);
  const Sched1ProjectionView verify_view =
      ProjectPostReloadBlockToSched1View(trial_body, mm, live_at_end_s2, verify_ctx);
  if (verify_view.core_insns.size() <= 1 ||
      !IsAchievable(verify_view.core_insns, verify_view.facts_s1, mm, Phase::kSched1,
                    verify_view.live_at_end_s1)) {
    return false;
  }
  // Stage 0 -> Stage 3 soundness gate: when moving `(REG_DEAD r)` to an
  // earlier post-`sched2` instruction (`rm.to_idx < rm.from_idx`), Stage 0
  // leaves `cand_trial_base` in post-`sched2` order and Stage 3 must be
  // able to invert the resulting premature `(REG_DEAD r)` without leaving
  // it inverted for Stage 4/5 (`flow.c:2506`, `sched.c:3415-3611`).
  if (rm.to_idx < rm.from_idx) {
    bool stage3_seed_safe = false;
    for (const InvertedRegDeadSeedMode seed_mode :
         {InvertedRegDeadSeedMode::kHoistEarliest, InvertedRegDeadSeedMode::kPullBeforeDead,
          InvertedRegDeadSeedMode::kDeferAfterReaders}) {
      const std::optional<std::vector<int>> seed_pref = BuildInvertedRegDeadSeedOrderInBase(
          cand_trial_base, prologue_end, epilogue_begin, facts_s2, mm, seed_mode);
      if (!seed_pref.has_value()) {
        continue;
      }
      const std::optional<std::vector<int>> seed_cand =
          InvertBlockOrder(cand_trial_base, facts_s2, mm, Phase::kSched2, live_at_end_s2,
                           *seed_pref, reload_adjacent);
      if (!seed_cand.has_value()) {
        continue;
      }
      std::vector<Insn> seed_seq;
      seed_seq.reserve(static_cast<size_t>(n));
      for (int p = 0; p < n; ++p) {
        Insn insn = cand_trial_base[static_cast<size_t>((*seed_cand)[static_cast<size_t>(p)])];
        insn.luid = p;
        seed_seq.push_back(std::move(insn));
      }
      if (!BuildInvertedRegDeadSeedOrderInBase(seed_seq, prologue_end, epilogue_begin, facts_s2, mm)
               .has_value() &&
          PreservesPostReloadOrderSafety(
              cand_trial_base, *seed_cand, prologue_end, epilogue_begin, live_at_end_s2,
              cfg_ctx.cfg_copy_pairs, facts_s2, mm, /*allow_pure_reader_reg_dead_transfer=*/false,
              cfg_ctx.stack_layout, proj_ctx.excluded_prologue_body_indices,
              cand_allow_store_load_inval)) {
        stage3_seed_safe = true;
        break;
      }
    }
    if (!stage3_seed_safe) {
      return false;
    }
  }
  *base_block = std::move(cand_trial_base);
  *cur_cand = cand_perm;
  touched_sched_idxs->insert(rm.from_idx);
  touched_sched_idxs->insert(rm.to_idx);
  for (int si : stripped_equiv_targets) {
    touched_sched_idxs->insert(si);
  }
  for (int si : added_equiv_targets) {
    touched_sched_idxs->insert(si);
  }
  return true;
}

bool TryRepairCombineRegsPureReaderRegDeadConflicts(
    size_t block_idx, const PostReloadBodyBounds& bounds, const Sched1ProjectionContext& proj_ctx,
    const CfgSched1Context& cfg_ctx, const RegFacts& facts_s2, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, absl::Span<const AdjacentPair> reload_adjacent,
    const std::vector<int>& init_cand_in_base, bool allow_moved_store_load_equiv_invalidation,
    std::vector<Insn>* base_block, absl::btree_set<int>* touched_sched_idxs) {
  const int n = static_cast<int>(base_block->size());
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1 || static_cast<int>(init_cand_in_base.size()) != n) {
    return false;
  }

  const bool fp_needed = cfg_ctx.stack_layout.frame_pointer_needed;
  bool any_committed = false;
  std::vector<int> cur_cand = init_cand_in_base;
  constexpr int kMaxRepairs = 4;

  for (int rep = 0; rep < kMaxRepairs; ++rep) {
    std::vector<InsnRegReadDefDeaths> info(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      info[static_cast<size_t>(i)] =
          CollectInsnRegReadDefDeaths((*base_block)[static_cast<size_t>(i)], mm);
    }

    std::vector<PureReaderRepairMove> repairs;
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      const std::optional<CombineRegsConflictSite> conf = FindFirstLowerOrderCombineRegsConflict(
          *base_block, info, i, prologue_end, epilogue_begin, live_at_end_s2, mm, fp_needed,
          /*extra_dead_reg=*/-1);
      if (!conf.has_value() ||
          conf->note_idx >= (*base_block)[static_cast<size_t>(i)].pn.reg_notes.size()) {
        continue;
      }
      const RtlRegNote& dead_note =
          (*base_block)[static_cast<size_t>(i)].pn.reg_notes[conf->note_idx];
      const int r = conf->regno;
      const int nr = mm.HardRegNRegs(r, dead_note.datum.mode);
      for (int j = i - 1; j >= prologue_end; --j) {
        bool j_defs_r = false;
        for (int k = 0; k < nr; ++k) {
          if (info[static_cast<size_t>(j)].defs.contains(r + k)) {
            j_defs_r = true;
            break;
          }
        }
        if (j_defs_r) {
          break;
        }
        if (info[static_cast<size_t>(j)].is_call_or_use) {
          continue;
        }
        bool j_reads_all = true;
        bool j_already_dead = false;
        for (int k = 0; k < nr; ++k) {
          if (!info[static_cast<size_t>(j)].reads.contains(r + k)) {
            j_reads_all = false;
          }
          if (info[static_cast<size_t>(j)].deaths.contains(r + k)) {
            j_already_dead = true;
          }
        }
        if (!j_reads_all || j_already_dead) {
          continue;
        }
        if (!FindFirstLowerOrderCombineRegsConflict(*base_block, info, j, prologue_end,
                                                    epilogue_begin, live_at_end_s2, mm, fp_needed,
                                                    /*extra_dead_reg=*/r)
                 .has_value()) {
          repairs.push_back(PureReaderRepairMove{
              .from_idx = i,
              .to_idx = j,
              .regno = r,
              .note_idx = conf->note_idx,
          });
        }
      }
    }
    if (repairs.empty()) {
      break;
    }

    bool repaired_one = false;
    for (const PureReaderRepairMove& rm : repairs) {
      std::vector<Insn> trial_base = *base_block;
      auto& from_notes = trial_base[static_cast<size_t>(rm.from_idx)].pn.reg_notes;
      const RtlRegNote moved_note = from_notes[rm.note_idx];
      from_notes.erase(from_notes.begin() + static_cast<std::ptrdiff_t>(rm.note_idx));
      trial_base[static_cast<size_t>(rm.to_idx)].pn.reg_notes.push_back(moved_note);

      // Build candidate preference orders placing `rm.to_idx` after `rm.from_idx`
      // and all other same-live-range pure readers of `rm.regno`:
      //   1. `defer_pref`: defer `rm.to_idx` immediately after the last trailing
      //      pure reader (inverts `sched2` hoisting `rm.to_idx` early).
      //   2. `pull_pref`: pull trailing pure readers immediately before `rm.to_idx`
      //      (when `rm.to_idx` has a consumer between `rm.to_idx` and `rm.from_idx`).
      const int moved_nr = mm.HardRegNRegs(rm.regno, moved_note.datum.mode);
      absl::btree_set<int> readers_to_pull;
      readers_to_pull.insert(rm.from_idx);
      for (int m = rm.to_idx + 1; m < epilogue_begin; ++m) {
        bool m_defs_r = false;
        bool m_reads_r = false;
        for (int w = 0; w < moved_nr; ++w) {
          if (info[static_cast<size_t>(m)].defs.contains(rm.regno + w)) {
            m_defs_r = true;
          }
          if (info[static_cast<size_t>(m)].reads.contains(rm.regno + w)) {
            m_reads_r = true;
          }
        }
        if (m_defs_r) {
          break;
        }
        if (m_reads_r && !info[static_cast<size_t>(m)].is_call_or_use) {
          readers_to_pull.insert(m);
        }
      }
      std::vector<int> pos_in_cur(static_cast<size_t>(n), -1);
      for (int p = 0; p < n; ++p) {
        pos_in_cur[static_cast<size_t>(cur_cand[static_cast<size_t>(p)])] = p;
      }
      std::vector<int> trailing_readers;
      for (int p = pos_in_cur[static_cast<size_t>(rm.to_idx)] + 1; p < n; ++p) {
        const int id = cur_cand[static_cast<size_t>(p)];
        if (readers_to_pull.contains(id)) {
          trailing_readers.push_back(id);
        }
      }
      std::vector<std::vector<int>> pref_orders;
      if (!trailing_readers.empty()) {
        const int last_tr_id = trailing_readers.back();
        std::vector<int> defer_pref;
        defer_pref.reserve(static_cast<size_t>(n));
        for (int p = 0; p < n; ++p) {
          const int id = cur_cand[static_cast<size_t>(p)];
          if (id == rm.to_idx) {
            continue;
          }
          defer_pref.push_back(id);
          if (id == last_tr_id) {
            defer_pref.push_back(rm.to_idx);
          }
        }
        pref_orders.push_back(std::move(defer_pref));

        const absl::btree_set<int> trailing_set(trailing_readers.begin(), trailing_readers.end());
        std::vector<int> pull_pref;
        pull_pref.reserve(static_cast<size_t>(n));
        for (int p = 0; p < n; ++p) {
          const int id = cur_cand[static_cast<size_t>(p)];
          if (trailing_set.contains(id)) {
            continue;
          }
          if (id == rm.to_idx) {
            for (int tr_id : trailing_readers) {
              pull_pref.push_back(tr_id);
            }
          }
          pull_pref.push_back(id);
        }
        pref_orders.push_back(std::move(pull_pref));
      } else {
        pref_orders.push_back(cur_cand);
      }

      Sched1ProjectionContext t_ctx = proj_ctx;
      t_ctx.allow_post_call_combine_regs_split = false;
      t_ctx.allow_general_combine_regs_split = false;
      std::optional<Sched1ProjectionView> t_view;
      auto ensure_t_view = [&]() -> const Sched1ProjectionView& {
        if (!t_view.has_value()) {
          std::vector<Insn> t_body;
          t_body.reserve(static_cast<size_t>(body_len));
          for (int p = prologue_end; p < epilogue_begin; ++p) {
            Insn insn = trial_base[static_cast<size_t>(p)];
            insn.luid = p - prologue_end;
            t_body.push_back(std::move(insn));
          }
          t_view = ProjectPostReloadBlockToSched1View(t_body, mm, live_at_end_s2, t_ctx);
        }
        return *t_view;
      };
      std::vector<std::vector<int>> direct_cands;
      for (const std::vector<int>& pref_in_base : pref_orders) {
        const std::optional<std::vector<int>> direct_cand =
            InvertBlockOrder(trial_base, facts_s2, mm, Phase::kSched2, live_at_end_s2, pref_in_base,
                             reload_adjacent);
        if (!direct_cand.has_value()) {
          continue;
        }
        if (VerifyAndCommitPureReaderTransfer(
                block_idx, bounds, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2, reload_adjacent,
                info, trial_base, rm, moved_note, allow_moved_store_load_equiv_invalidation,
                *direct_cand, base_block, &cur_cand, touched_sched_idxs)) {
          any_committed = true;
          repaired_one = true;
          break;
        }
        direct_cands.push_back(*direct_cand);
      }
      if (repaired_one) {
        break;
      }

      // If no direct candidate succeeded, fall back to bounded fixed-point
      // refinement on `trial_base`, seeded first from each `direct_cand` (which
      // already places `rm.from_idx < rm.to_idx`) and then unseeded.
      if (!direct_cands.empty()) {
        std::vector<std::vector<int>> refine_seeds = direct_cands;
        refine_seeds.emplace_back();
        for (const std::vector<int>& seed : refine_seeds) {
          const std::vector<std::vector<int>> refined = FindStage0Sched2PreimageCandidates(
              trial_base, block_idx, bounds, ensure_t_view(), t_ctx, cfg_ctx, facts_s2, mm,
              live_at_end_s2, reload_adjacent, /*allow_pure_reader_reg_dead_transfer=*/false,
              /*allow_tie_flips=*/true, seed, allow_moved_store_load_equiv_invalidation);
          for (auto it = refined.rbegin(); it != refined.rend(); ++it) {
            if (VerifyAndCommitPureReaderTransfer(block_idx, bounds, proj_ctx, cfg_ctx, facts_s2,
                                                  mm, live_at_end_s2, reload_adjacent, info,
                                                  trial_base, rm, moved_note,
                                                  allow_moved_store_load_equiv_invalidation, *it,
                                                  base_block, &cur_cand, touched_sched_idxs)) {
              any_committed = true;
              repaired_one = true;
              break;
            }
          }
          if (repaired_one) {
            break;
          }
        }
        if (repaired_one) {
          break;
        }
      }
    }
    if (!repaired_one) {
      break;
    }
  }
  return any_committed;
}

// Mirrors `jump.c:434-465` (`jump_optimize` with `noop_moves = 1`): when `jump2`
// deleted a redundant pre-call `(set (reg:M arg_reg) (reg:M saved_reg))` before
// `B0`'s first `CALL_INSN` (leaving `arg_reg` in `call_fusage` and stripping
// `(REG_DEAD arg_reg)` from the earlier `(set saved_reg arg_reg)`), temporarily
// restores that synthetic copy inside `view_cfg.rpo_blocks[0]` so Stage 0's
// `AttributeSched2PureReaderRegDeadNotes` sees the complete post-`sched2` block
// (matching Stage 2's `InvertDoCrossJump`) and can attribute erased
// `local-alloc.c:1266-1300` (`combine_regs`) `REG_DEAD` notes on post-call RMWs.
void SynthesizeB0Jump2DeletedPreCallArgCopiesInDelayView(
    const CompilationProfile& profile, const MachineModel& mm, RtlControlFlowGraph* view_cfg,
    RtlControlFlowGraph* live_cfg, std::vector<Stage0BlockDelayView>* block_views) {
  if (view_cfg == nullptr || live_cfg == nullptr || block_views == nullptr ||
      view_cfg->rpo_blocks.empty() || block_views->empty() || !(*block_views)[0].eligible) {
    return;
  }
  RtlBasicBlock& vblk = view_cfg->rpo_blocks[0];
  Stage0BlockDelayView& bv = (*block_views)[0];
  const size_t n_insns = vblk.instructions.size();
  if (n_insns < 3 || vblk.rtl_insns.size() != n_insns || bv.view_to_orig.size() != n_insns) {
    return;
  }

  std::vector<RtlPatternAndNotes> parsed(n_insns);
  std::optional<size_t> first_call_vi;
  for (size_t i = 0; i < n_insns; ++i) {
    auto pn = ParsePatternAndNotes(vblk.instructions[i]);
    if (!pn.ok()) {
      return;
    }
    parsed[i] = std::move(*pn);
    const RtlExpr& pat = parsed[i].pattern;
    if (pat.op == RtlOp::kJumpInsn || pat.op == RtlOp::kReturn ||
        (pat.IsSet() && pat.SetDest().op == RtlOp::kPc)) {
      return;
    }
    bool has_call = false;
    pat.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kCall) {
        has_call = true;
      }
    });
    if (has_call) {
      first_call_vi = i;
      break;
    }
  }
  if (!first_call_vi.has_value() || *first_call_vi < 2) {
    return;
  }

  const size_t call_vi = *first_call_vi;
  const bool has_explicit_fusage = vblk.rtl_insns[call_vi].has_explicit_call_fusage;
  absl::btree_set<int> fusage_regs;
  if (has_explicit_fusage) {
    for (const RtlExpr& fu : vblk.rtl_insns[call_vi].call_fusage) {
      CollectRegReads(fu, &fusage_regs);
    }
  }

  std::vector<absl::btree_set<int>> reads(call_vi);
  std::vector<absl::btree_set<int>> defs(call_vi);
  std::optional<size_t> ra_save_vi;
  absl::btree_map<int, size_t> prologue_save_vi;
  for (size_t i = 0; i < call_vi; ++i) {
    CollectRegReads(parsed[i].pattern, &reads[i]);
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(parsed[i].pattern, &fdefs, &pdefs);
    defs[i] = fdefs;
    defs[i].insert(pdefs.begin(), pdefs.end());
    if (!ra_save_vi.has_value() && IsSpRaSavePatternForDelayView(parsed[i].pattern)) {
      ra_save_vi = i;
    }
    const RtlExpr& pat = parsed[i].pattern;
    if (pat.IsSet() && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kMem &&
        !pat.SetDest().children.empty() && pat.SetSrc().op == RtlOp::kReg) {
      const RtlExpr& addr = pat.SetDest().children[0];
      const bool sp_addr = (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
                           (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                            addr.children[0].op == RtlOp::kReg &&
                            addr.children[0].RegNo() == kMipsStackPointerRegno &&
                            addr.children[1].op == RtlOp::kConstInt);
      if (sp_addr) {
        const int sr = pat.SetSrc().RegNo();
        const int span = std::max(1, mm.HardRegNRegs(sr, pat.SetSrc().mode));
        for (int off = 0; off < span; ++off) {
          prologue_save_vi.try_emplace(sr + off, i);
        }
      }
    }
  }

  const bool fp_needed = view_cfg->sideband.frame_pointer_needed;
  auto is_callee_saved = [&](int r) {
    return (r >= 16 && r <= 23) || (r == kMipsFramePointerRegno && !fp_needed) ||
           (r >= 52 && r <= 63);
  };
  auto is_o32_arg_reg = [](int r) {
    return (r >= kMipsArgRegFirst && r <= kMipsArgRegLast) ||
           (r >= kMipsFpArgRegFirst && r <= kMipsFpArgRegFirst + 3);
  };

  // Track O32 arg regs explicitly defined before `call_vi` and O32 arg words
  // occupied by `$f12`/`$f14` (`config/mips/mips.c:function_arg`, mirroring
  // `invert_do_cross_jump.cc:1759-2089`).
  absl::btree_set<int> live_at_end_s2;
  for (int succ : vblk.successors) {
    if (const auto it = live_cfg->sideband.basic_block_live_at_start.find(succ);
        it != live_cfg->sideband.basic_block_live_at_start.end()) {
      live_at_end_s2.insert(it->second.begin(), it->second.end());
    }
  }
  absl::btree_set<int> read_after_first_call = live_at_end_s2;
  auto collect_reg_read_spans = [&](const RtlExpr& root, absl::btree_set<int>* out) {
    root.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kReg) {
        const int r = node.RegNo();
        if (r >= 0 && r < kMipsFirstPseudoRegister) {
          const int span = std::max(1, mm.HardRegNRegs(r, node.mode));
          for (int off = 0; off < span; ++off) {
            out->insert(r + off);
          }
        }
      }
    });
  };
  for (size_t i = call_vi + 1; i < n_insns; ++i) {
    auto pn = ParsePatternAndNotes(vblk.instructions[i]);
    if (!pn.ok()) {
      continue;
    }
    collect_reg_read_spans(pn->pattern, &read_after_first_call);
    if (vblk.rtl_insns[i].has_explicit_call_fusage) {
      for (const RtlExpr& fu : vblk.rtl_insns[i].call_fusage) {
        collect_reg_read_spans(fu, &read_after_first_call);
      }
    }
  }

  absl::btree_set<int> arg_regs_live_into_call;
  absl::btree_set<int> arg_word_occupied_by_fpr;
  bool arg6_written_as_8byte = false;
  for (int ar = kMipsArgRegFirst; ar <= kMipsArgRegLast; ++ar) {
    for (size_t i = call_vi; i > 0; --i) {
      if (defs[i - 1].contains(ar)) {
        if (!HasRegDeadNoteCovering(parsed[i - 1].reg_notes, ar, mm)) {
          arg_regs_live_into_call.insert(ar);
          if (ar == 6 && parsed[i - 1].pattern.IsSet() &&
              parsed[i - 1].pattern.SetDest().op == RtlOp::kReg &&
              parsed[i - 1].pattern.SetDest().RegNo() == 6 &&
              (parsed[i - 1].pattern.SetDest().mode == RtlMachineMode::kDf ||
               parsed[i - 1].pattern.SetDest().mode == RtlMachineMode::kDi)) {
            arg6_written_as_8byte = true;
          }
        }
        break;
      }
    }
  }
  for (int fr : {kMipsFpArgRegFirst, kMipsFpArgRegFirst + 2}) {
    const int base_word = (fr == kMipsFpArgRegFirst) ? 4 : 6;
    for (size_t i = call_vi; i > 0; --i) {
      if (defs[i - 1].contains(fr) || defs[i - 1].contains(fr + 1)) {
        if (!HasRegDeadNoteCovering(parsed[i - 1].reg_notes, fr, mm)) {
          arg_word_occupied_by_fpr.insert(base_word);
          if (defs[i - 1].contains(fr + 1) ||
              (parsed[i - 1].pattern.IsSet() && parsed[i - 1].pattern.SetDest().op == RtlOp::kReg &&
               parsed[i - 1].pattern.SetDest().mode == RtlMachineMode::kDf)) {
            arg_word_occupied_by_fpr.insert(base_word + 1);
          }
        }
        break;
      }
    }
  }
  if (has_explicit_fusage) {
    if (fusage_regs.contains(kMipsFpArgRegFirst)) {
      arg_word_occupied_by_fpr.insert(4);
      if (fusage_regs.contains(kMipsFpArgRegFirst + 1)) {
        arg_word_occupied_by_fpr.insert(5);
      }
    }
    if (fusage_regs.contains(kMipsFpArgRegFirst + 2)) {
      arg_word_occupied_by_fpr.insert(6);
      if (fusage_regs.contains(kMipsFpArgRegFirst + 3)) {
        arg_word_occupied_by_fpr.insert(7);
      }
    }
  }

  struct DeletedArgCopy {
    size_t copy_vi = 0;
    int saved_reg = -1;
    int arg_reg = -1;
    int saved_span = 1;
    int arg_span = 1;
    RtlMachineMode mode = RtlMachineMode::kNone;
    bool saved_read_before_call = false;
  };
  std::vector<DeletedArgCopy> raw_cands;
  absl::btree_set<int> claimed_arg_regs;
  for (size_t i = 0; i < call_vi; ++i) {
    const RtlExpr& pat = parsed[i].pattern;
    if (!pat.IsSet() || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kReg ||
        pat.SetSrc().op != RtlOp::kReg || pat.SetDest().IsArgPointerReg() ||
        pat.SetSrc().IsArgPointerReg() || pat.SetDest().mode != pat.SetSrc().mode) {
      continue;
    }
    const RtlMachineMode mode = pat.SetDest().mode;
    if (mode != RtlMachineMode::kSi && mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDi &&
        mode != RtlMachineMode::kDf) {
      continue;
    }
    const int saved_reg = pat.SetDest().RegNo();
    const int arg_reg = pat.SetSrc().RegNo();
    const int saved_span = std::max(1, mm.HardRegNRegs(saved_reg, mode));
    const int arg_span = std::max(1, mm.HardRegNRegs(arg_reg, mode));
    if (!is_callee_saved(saved_reg) || (saved_span == 2 && !is_callee_saved(saved_reg + 1))) {
      continue;
    }
    const bool is_gpr_arg =
        (arg_reg >= kMipsArgRegFirst && arg_reg + arg_span - 1 <= kMipsArgRegLast);
    if (!is_gpr_arg && !(has_explicit_fusage && is_o32_arg_reg(arg_reg))) {
      continue;
    }
    if (HasRegDeadNoteCovering(parsed[i].reg_notes, arg_reg, mm)) {
      continue;
    }
    bool valid = true;
    for (int off = 0; off < arg_span; ++off) {
      const int ar = arg_reg + off;
      if (claimed_arg_regs.contains(ar) || (is_gpr_arg && arg_word_occupied_by_fpr.contains(ar)) ||
          (has_explicit_fusage && !fusage_regs.contains(ar))) {
        valid = false;
        break;
      }
      for (size_t k = 0; k < i; ++k) {
        if (defs[k].contains(ar)) {
          valid = false;
          break;
        }
      }
      for (size_t k = i + 1; k < call_vi && valid; ++k) {
        if (defs[k].contains(ar) || reads[k].contains(ar)) {
          valid = false;
        }
      }
    }
    bool saved_read_before_call = false;
    for (int off = 0; off < saved_span && valid; ++off) {
      const int sr = saved_reg + off;
      if (!read_after_first_call.contains(sr)) {
        valid = false;
        break;
      }
      for (size_t k = i + 1; k < call_vi; ++k) {
        if (defs[k].contains(sr)) {
          valid = false;
          break;
        }
        if (reads[k].contains(sr)) {
          saved_read_before_call = true;
        }
      }
    }
    if (!valid) {
      continue;
    }
    raw_cands.push_back(
        DeletedArgCopy{i, saved_reg, arg_reg, saved_span, arg_span, mode, saved_read_before_call});
    for (int off = 0; off < arg_span; ++off) {
      claimed_arg_regs.insert(arg_reg + off);
    }
  }
  if (raw_cands.empty()) {
    return;
  }

  std::vector<DeletedArgCopy> cands;
  for (const DeletedArgCopy& c : raw_cands) {
    bool all_lower_args_supplied = true;
    if (c.arg_reg >= kMipsArgRegFirst && c.arg_reg <= kMipsArgRegLast) {
      for (int r = kMipsArgRegFirst; r < c.arg_reg; ++r) {
        if (c.arg_span == 2 && c.arg_reg == 6 && r == 5) {
          continue;
        }
        if (!arg_regs_live_into_call.contains(r) && !claimed_arg_regs.contains(r) &&
            !arg_word_occupied_by_fpr.contains(r)) {
          all_lower_args_supplied = false;
          break;
        }
      }
    }
    if (!all_lower_args_supplied) {
      continue;
    }
    if (!has_explicit_fusage) {
      bool save_before_ra = false;
      if (ra_save_vi.has_value() && !c.saved_read_before_call) {
        for (int off = 0; off < c.saved_span; ++off) {
          if (const auto pit = prologue_save_vi.find(c.saved_reg + off);
              pit != prologue_save_vi.end() && pit->second < *ra_save_vi &&
              pit->second < c.copy_vi) {
            save_before_ra = true;
            break;
          }
        }
      }
      bool higher_arg_written = false;
      for (int ar = c.arg_reg + c.arg_span; ar <= kMipsArgRegLast; ++ar) {
        if (c.arg_reg == 5 && c.arg_span == 1 && arg6_written_as_8byte) {
          break;
        }
        if (arg_regs_live_into_call.contains(ar)) {
          higher_arg_written = true;
          break;
        }
      }
      if (!save_before_ra && !higher_arg_written) {
        continue;
      }
    }
    cands.push_back(c);
  }
  if (cands.empty()) {
    return;
  }

  // Build a trial copy of `vblk` and `bv` with the synthetic copies inserted,
  // verifying that `InvertBlockOrder(Phase::kSched2)` succeeds before committing.

  auto make_reg_expr = [](RtlMachineMode m, int regno) { return RtlExpr::MakeReg(m, regno); };

  auto is_prologue_save = [](const RtlExpr& pat) {
    if (!pat.IsSet() || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kMem ||
        pat.SetSrc().op != RtlOp::kReg || pat.SetDest().children.empty()) {
      return false;
    }
    const int sr = pat.SetSrc().RegNo();
    const bool saved = (sr == kMipsReturnAddrRegno) || (sr >= 16 && sr <= 23) ||
                       (sr == kMipsFramePointerRegno) || (sr >= 52 && sr <= 63);
    if (!saved) {
      return false;
    }
    const RtlExpr& addr = pat.SetDest().children[0];
    return (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
           (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
            addr.children[0].op == RtlOp::kReg &&
            addr.children[0].RegNo() == kMipsStackPointerRegno &&
            addr.children[1].op == RtlOp::kConstInt);
  };

  auto build_trial = [&](const std::vector<size_t>& insert_positions, RtlBasicBlock* out_blk,
                         Stage0BlockDelayView* out_bv) {
    *out_blk = vblk;
    *out_bv = bv;
    std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage(out_blk->instructions.size());
    for (size_t i = 0; i < out_blk->rtl_insns.size(); ++i) {
      if (out_blk->rtl_insns[i].has_explicit_call_fusage) {
        saved_fusage[i] = out_blk->rtl_insns[i].call_fusage;
      }
    }
    // Attach temporary `(REG_DEAD arg_reg)` on each candidate's entry copy in `out_blk`.
    for (const DeletedArgCopy& c : cands) {
      RtlPatternAndNotes cpn = parsed[c.copy_vi];
      if (!HasRegDeadNoteCovering(cpn.reg_notes, c.arg_reg, mm)) {
        cpn.reg_notes.insert(cpn.reg_notes.begin(), RtlRegNote{
                                                        .kind = RtlMachineMode::kRegDead,
                                                        .list_op = RtlOp::kExprList,
                                                        .datum = make_reg_expr(c.mode, c.arg_reg),
                                                    });
        out_blk->instructions[c.copy_vi] = FormatGccPrintRtlPatternAndNotes(
            cpn.pattern, cpn.reg_notes, cpn.bracketed_notes, /*call_fusage=*/{});
        out_bv->view_to_orig[c.copy_vi].synthetic_added_dead_regs.insert(c.arg_reg);
      }
    }
    // Insert synthetic `(set arg_reg saved_reg)` moves in descending position order
    // so earlier indices stay valid.
    std::vector<size_t> order(cands.size());
    for (size_t idx = 0; idx < cands.size(); ++idx) {
      order[idx] = idx;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      if (insert_positions[a] != insert_positions[b]) {
        return insert_positions[a] > insert_positions[b];
      }
      return a > b;
    });
    for (size_t idx : order) {
      const DeletedArgCopy& c = cands[idx];
      const size_t pos = insert_positions[idx];
      const RtlExpr mv_pat =
          RtlExpr::MakeSet(make_reg_expr(c.mode, c.arg_reg), make_reg_expr(c.mode, c.saved_reg));
      const std::string mv_str = FormatGccPrintRtlPatternAndNotes(mv_pat, {}, {}, {});
      out_blk->instructions.insert(out_blk->instructions.begin() + static_cast<ptrdiff_t>(pos),
                                   mv_str);
      out_bv->view_to_orig.insert(out_bv->view_to_orig.begin() + static_cast<ptrdiff_t>(pos),
                                  Stage0OriginLoc{.is_synthetic = true});
      saved_fusage.insert(saved_fusage.begin() + static_cast<ptrdiff_t>(pos), std::nullopt);
      if (out_bv->unpacked_non_return_jump_slot_view_idx.has_value() &&
          *out_bv->unpacked_non_return_jump_slot_view_idx >= pos) {
        ++(*out_bv->unpacked_non_return_jump_slot_view_idx);
      }
      absl::btree_set<size_t> shifted_align;
      for (size_t ai : out_bv->alignment_touched_view_idxs) {
        shifted_align.insert(ai >= pos ? ai + 1 : ai);
      }
      out_bv->alignment_touched_view_idxs = std::move(shifted_align);
    }
    out_blk->SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < out_blk->rtl_insns.size() && i < saved_fusage.size(); ++i) {
      if (saved_fusage[i].has_value()) {
        out_blk->rtl_insns[i].has_explicit_call_fusage = true;
        out_blk->rtl_insns[i].call_fusage = *saved_fusage[i];
      }
    }
    const size_t new_call_vi = call_vi + cands.size();
    if (!has_explicit_fusage && new_call_vi < out_blk->rtl_insns.size()) {
      absl::btree_set<int> synth_fusage_regs = arg_regs_live_into_call;
      for (const DeletedArgCopy& c : cands) {
        const int span = std::max(1, mm.HardRegNRegs(c.arg_reg, c.mode));
        for (int off = 0; off < span; ++off) {
          synth_fusage_regs.insert(c.arg_reg + off);
        }
      }
      out_blk->rtl_insns[new_call_vi].has_explicit_call_fusage = true;
      out_blk->rtl_insns[new_call_vi].call_fusage.clear();
      for (auto rit = synth_fusage_regs.rbegin(); rit != synth_fusage_regs.rend(); ++rit) {
        out_blk->rtl_insns[new_call_vi].call_fusage.push_back(RtlExpr::MakeUnary(
            RtlOp::kUse, RtlMachineMode::kNone, make_reg_expr(RtlMachineMode::kSi, *rit)));
      }
    }
  };

  auto trial_inverts_s2 = [&](const RtlBasicBlock& cand_blk) -> bool {
    RtlControlFlowGraph tmp_cfg = *view_cfg;
    tmp_cfg.rpo_blocks[0] = cand_blk;
    const ParsedSchedRegion reg = ParseBlockForSchedModel(cand_blk, 0, Phase::kSched2);
    if (!reg.valid || reg.has_mid_block_loop_barrier) {
      return false;
    }
    const RegFacts tmp_facts = RegFacts::FromCfg(tmp_cfg, profile, Phase::kSched2);
    return InvertBlockOrder(reg.sched_block, tmp_facts, mm, Phase::kSched2, live_at_end_s2)
        .has_value();
  };

  // Compute default insertion slots matching `invert_do_cross_jump.cc:2124-2220`.
  std::vector<size_t> default_pos(cands.size(), call_vi);
  for (size_t idx = 0; idx < cands.size(); ++idx) {
    const DeletedArgCopy& c = cands[idx];
    if (ra_save_vi.has_value() && *ra_save_vi > c.copy_vi) {
      if (c.saved_reg >= kMipsFpRegFirst && *ra_save_vi <= c.copy_vi + 2) {
        size_t pos = *ra_save_vi + 1;
        while (pos < call_vi && is_prologue_save(parsed[pos].pattern)) {
          ++pos;
        }
        default_pos[idx] = pos;
      } else {
        size_t pos = *ra_save_vi;
        while (pos > c.copy_vi + 1 && c.saved_reg >= kMipsFpRegFirst &&
               parsed[pos - 1].pattern.IsSet() &&
               parsed[pos - 1].pattern.SetDest().op == RtlOp::kReg &&
               is_o32_arg_reg(parsed[pos - 1].pattern.SetDest().RegNo())) {
          --pos;
        }
        default_pos[idx] = pos;
      }
    }
    // Never place a restored single-cycle GPR->GPR arg copy ahead of an unpacked
    // delay-slot or post-`$ra`-save setup of a lower O32 arg register
    // (`calls.c:expand_call`); multi-cycle FPR->GPR (`xfer` latency 2) copies
    // outrank single-cycle GPR arg setups (`sched.c:priority`, matching
    // `invert_do_cross_jump.cc:2293-2305`).
    if (c.saved_reg < kMipsFpRegFirst && c.arg_reg >= kMipsArgRegFirst &&
        c.arg_reg <= kMipsArgRegLast) {
      for (size_t k = default_pos[idx]; k < call_vi; ++k) {
        bool defines_lower_arg = false;
        for (int ar = kMipsArgRegFirst; ar < c.arg_reg; ++ar) {
          if (defs[k].contains(ar)) {
            defines_lower_arg = true;
            break;
          }
        }
        if (defines_lower_arg) {
          default_pos[idx] = k + 1;
        }
      }
    }
  }

  RtlBasicBlock chosen_blk;
  Stage0BlockDelayView chosen_bv;
  build_trial(default_pos, &chosen_blk, &chosen_bv);
  if (!trial_inverts_s2(chosen_blk)) {
    bool found_inv = false;
    if (cands.size() == 1) {
      for (size_t pos = call_vi; pos > cands[0].copy_vi && !found_inv; --pos) {
        if (pos == default_pos[0]) {
          continue;
        }
        RtlBasicBlock probe_blk;
        Stage0BlockDelayView probe_bv;
        build_trial({pos}, &probe_blk, &probe_bv);
        if (trial_inverts_s2(probe_blk)) {
          chosen_blk = std::move(probe_blk);
          chosen_bv = std::move(probe_bv);
          found_inv = true;
        }
      }
    } else {
      for (size_t idx = 0; idx < cands.size() && !found_inv; ++idx) {
        for (size_t pos = call_vi; pos > cands[idx].copy_vi && !found_inv; --pos) {
          if (pos == default_pos[idx]) {
            continue;
          }
          std::vector<size_t> probe = default_pos;
          probe[idx] = pos;
          RtlBasicBlock probe_blk;
          Stage0BlockDelayView probe_bv;
          build_trial(probe, &probe_blk, &probe_bv);
          if (trial_inverts_s2(probe_blk)) {
            chosen_blk = std::move(probe_blk);
            chosen_bv = std::move(probe_bv);
            found_inv = true;
          }
        }
      }
    }
    // Keep `default_pos` even if `trial_inverts_s2` hasn't succeeded yet when
    // an unpacked call delay slot still needs `AlignStage0DelaySlotsToSched2View`.
  }
  vblk = std::move(chosen_blk);
  bv = std::move(chosen_bv);
  *live_cfg = *view_cfg;
  ReconstructRtlLiveness(
      live_cfg, profile,
      {.reload_completed = true, .update_reg_notes = true, .preserve_phantom_pseudos = false});
}

// Mirrors Stage 1 -> Stage 2 order (`reorg` delay-slot alignment before
// `jump2`-deleted pre-call arg-copy restoration) in Stage 0's delay view,
// re-aligning if needed and rolling back `B0` if the synthetic copies fail
// `kSched2` inversion.
void ApplyStage0B0Jump2DeletedArgCopiesWithRollback(
    const CompilationProfile& profile, const MachineModel& mm, RtlControlFlowGraph* view_cfg,
    RtlControlFlowGraph* live_cfg, std::vector<Stage0BlockDelayView>* block_views) {
  if (view_cfg == nullptr || live_cfg == nullptr || block_views == nullptr ||
      view_cfg->rpo_blocks.empty()) {
    return;
  }
  const size_t b0_pre_synth_len = view_cfg->rpo_blocks[0].instructions.size();
  const RtlControlFlowGraph pre_synth_view_cfg = *view_cfg;
  const RtlControlFlowGraph pre_synth_live_cfg = *live_cfg;
  const std::vector<Stage0BlockDelayView> pre_synth_block_views = *block_views;
  SynthesizeB0Jump2DeletedPreCallArgCopiesInDelayView(profile, mm, view_cfg, live_cfg, block_views);
  if (view_cfg->rpo_blocks[0].instructions.size() == b0_pre_synth_len) {
    return;
  }
  auto b0_inverts_s2 = [&]() -> bool {
    const ParsedSchedRegion b0_reg =
        ParseBlockForSchedModel(view_cfg->rpo_blocks[0], 0, Phase::kSched2);
    if (!b0_reg.valid || b0_reg.has_mid_block_loop_barrier) {
      return false;
    }
    const RegFacts b0_facts_s2 = RegFacts::FromCfg(*view_cfg, profile, Phase::kSched2);
    absl::btree_set<int> b0_live_end;
    for (int succ : view_cfg->rpo_blocks[0].successors) {
      if (const auto it = live_cfg->sideband.basic_block_live_at_start.find(succ);
          it != live_cfg->sideband.basic_block_live_at_start.end()) {
        b0_live_end.insert(it->second.begin(), it->second.end());
      }
    }
    return InvertBlockOrder(b0_reg.sched_block, b0_facts_s2, mm, Phase::kSched2, b0_live_end)
        .has_value();
  };
  if (b0_inverts_s2()) {
    return;
  }
  AlignStage0DelaySlotsToSched2View(profile, mm, view_cfg, live_cfg, block_views);
  if (!b0_inverts_s2()) {
    *view_cfg = pre_synth_view_cfg;
    *live_cfg = pre_synth_live_cfg;
    *block_views = pre_synth_block_views;
  }
}

// `reload1.c:5599-5665` (`emit_reload_insns`) + `jump.c:452-459` (`jump_optimize`):
// when `sched2` separates an earlyclobber `DImode` GPR `PARALLEL` `prod` from
// its output-reload move `mv: (set (reg:DI orig) (reg:DI reload))` with an
// intervening pure reader of `orig`, `AttributeSched2PureReaderRegDeadNotes`
// validates the `.10.sched2` preimage with `(REG_DEAD orig)` on `prod`; before
// committing back to `.12.dbr` (post-`jump2`), strip that `(REG_DEAD orig)` on
// `prod` because `jump2` unconditionally consumes it scanning backward from `mv`
// (and Stage 2 `RestoreJump2ConsumedRegDeadNotes` restores it onto `prod`).
void StripJump2ConsumedEarlyclobberOutputReloadDeaths(int prologue_end, int epilogue_begin,
                                                      const MachineModel& mm,
                                                      std::vector<Insn>* transferred_base) {
  if (transferred_base == nullptr) {
    return;
  }
  for (int mv_idx = prologue_end + 1; mv_idx < epilogue_begin; ++mv_idx) {
    const RtlExpr& mv_pat = (*transferred_base)[static_cast<size_t>(mv_idx)].pn.pattern;
    if ((*transferred_base)[static_cast<size_t>(mv_idx)].kind != Insn::Kind::kInsn ||
        mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
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
    bool saw_intervening_orig_read = false;
    for (int p = mv_idx - 1; p >= prologue_end; --p) {
      const Insn& pi = (*transferred_base)[static_cast<size_t>(p)];
      if (pi.kind == Insn::Kind::kCall || pi.kind == Insn::Kind::kJump) {
        break;
      }
      int matched_orig = -1;
      int matched_reload = -1;
      if (saw_intervening_orig_read &&
          TryMatchEarlyclobberDiOutputReload(pi, (*transferred_base)[static_cast<size_t>(mv_idx)],
                                             mm, &matched_orig, &matched_reload) &&
          matched_orig == orig_hr && matched_reload == reload_hr) {
        auto& notes = (*transferred_base)[static_cast<size_t>(p)].pn.reg_notes;
        auto it = std::find_if(notes.begin(), notes.end(), [&](const RtlRegNote& n) {
          return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                 n.datum.RegNo() == orig_hr;
        });
        if (it != notes.end()) {
          notes.erase(it);
        }
        break;
      }
      if (HasRegDeadNoteCovering(pi.pn.reg_notes, orig_hr, mm)) {
        break;
      }
      const InsnRegReadDefDeaths info = CollectInsnRegReadDefDeaths(pi, mm);
      if (info.defs.contains(orig_hr) || info.defs.contains(orig_hr + 1) ||
          info.defs.contains(reload_hr) || info.defs.contains(reload_hr + 1)) {
        break;
      }
      if (info.reads.contains(orig_hr) || info.reads.contains(orig_hr + 1)) {
        saw_intervening_orig_read = true;
      }
    }
  }
}

struct Stage0PureReaderNoteTransfer {
  int from_idx = -1;
  size_t note_idx = 0;
  int to_idx = -1;
  RtlRegNote note;
};

struct Stage0PureReaderBlockState {
  size_t block_idx = 0;
  const ParsedSchedRegion& base_region;
  const PostReloadBodyBounds& bounds;
  absl::Span<const Insn> base_body;
  const Sched1ProjectionContext& proj_ctx;
  const Sched1ProjectionContext& unsplit_base_ctx;
  const CfgSched1Context& cfg_ctx;
  const RegFacts& facts_s2;
  const MachineModel& mm;
  const absl::btree_set<int>& live_at_end_s2;
  absl::Span<const AdjacentPair> reload_adjacent;
  absl::Span<const InsnRegReadDefDeaths> insn_info;
  absl::Span<const absl::btree_set<int>> live_after_base;
  const absl::btree_set<int>& alignment_touched_sched_idxs;
};

// Collects pure-reader `(REG_DEAD r)` transfers and `reorg.c:918-920` delay-slot
// `(REG_DEAD (const_int 0))` placeholder resolutions for a candidate pre-`sched2`
// permutation `cand_in_base` (`flow.c:2504-2515`, `sched.c:3415-3611`).
std::vector<Stage0PureReaderNoteTransfer> CollectPureReaderDeadTransfersForCandidate(
    const Stage0PureReaderBlockState& st, const Stage0BlockDelayView& block_view,
    absl::Span<const int> cand_in_base, absl::Span<const int> pos_in_cand) {
  const int prologue_end = st.bounds.prologue_end;
  const int epilogue_begin = st.bounds.epilogue_begin;
  std::vector<Stage0PureReaderNoteTransfer> transfers;

  auto find_cand_last_reader = [&](int i, int r, int nr) -> int {
    int last_reader = -1;
    for (int p = pos_in_cand[static_cast<size_t>(i)] + 1; p < epilogue_begin; ++p) {
      const int m = cand_in_base[static_cast<size_t>(p)];
      bool m_defs_r = false;
      for (int k = 0; k < nr; ++k) {
        if (st.insn_info[static_cast<size_t>(m)].defs.contains(r + k)) {
          m_defs_r = true;
          break;
        }
      }
      if (m_defs_r) {
        break;
      }
      bool m_reads_all = true;
      for (int k = 0; k < nr; ++k) {
        if (!st.insn_info[static_cast<size_t>(m)].reads.contains(r + k)) {
          m_reads_all = false;
          break;
        }
      }
      if (m_reads_all) {
        last_reader = m;
      }
    }
    // A `sched2` pure-reader `REG_DEAD` inversion (`flow.c:2504-2515`,
    // `sched.c:3415-3611`) only occurs when `sched2` hoisted the
    // pre-`sched2` last reader `last_reader` ahead of `i` (`last_reader <
    // i` in post-`sched2` `base_region` order while `pos_in_cand[i] <
    // pos_in_cand[last_reader]`). Reject `last_reader >= i` so a
    // candidate that keeps `i` ahead of `last_reader` never undoes an
    // already-inverted prologue-hoisted `REG_DEAD` (`function.c:5516`).
    if (last_reader < 0 || last_reader >= i ||
        st.insn_info[static_cast<size_t>(last_reader)].is_call_or_use) {
      return -1;
    }
    for (int k = 0; k < nr; ++k) {
      if (st.insn_info[static_cast<size_t>(last_reader)].deaths.contains(r + k)) {
        return -1;
      }
    }
    const int lo = std::min(i, last_reader);
    const int hi = std::max(i, last_reader);
    for (int k = lo + 1; k < hi; ++k) {
      for (int w = 0; w < nr; ++w) {
        if (st.insn_info[static_cast<size_t>(k)].defs.contains(r + w)) {
          return -1;
        }
      }
    }
    return last_reader;
  };

  for (int i = prologue_end; i < epilogue_begin; ++i) {
    if (st.insn_info[static_cast<size_t>(i)].is_call_or_use) {
      continue;
    }
    const auto& notes = st.base_region.sched_block[static_cast<size_t>(i)].pn.reg_notes;
    std::vector<size_t> const_zero_note_idxs;
    for (size_t ni = 0; ni < notes.size(); ++ni) {
      const RtlRegNote& note = notes[ni];
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kConstInt &&
          note.datum.int_val == 0) {
        const_zero_note_idxs.push_back(ni);
        continue;
      }
      if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
        continue;
      }
      const int r = note.datum.RegNo();
      if (r < 0 || r >= kMipsFirstPseudoRegister) {
        continue;
      }
      const int nr = st.mm.HardRegNRegs(r, note.datum.mode);
      bool i_defs_r = false;
      for (int k = 0; k < nr; ++k) {
        if (st.insn_info[static_cast<size_t>(i)].defs.contains(r + k)) {
          i_defs_r = true;
          break;
        }
      }
      if (i_defs_r) {
        continue;
      }
      const int last_reader = find_cand_last_reader(i, r, nr);
      if (last_reader >= 0 &&
          !IsCrossClassFprToGprCopyReloadInheritedRead(
              st.base_region.sched_block, cand_in_base, pos_in_cand[static_cast<size_t>(i)],
              pos_in_cand[static_cast<size_t>(last_reader)], r, st.mm)) {
        transfers.push_back(Stage0PureReaderNoteTransfer{
            .from_idx = i,
            .note_idx = ni,
            .to_idx = last_reader,
            .note = note,
        });
      }
    }
    // `reorg.c:918-920`: delay-slot instructions carry `(REG_DEAD (const_int 0))`
    // placeholders; if `i` was moved from before `last_reader` by `sched2` or
    // `reorg.c:fill_simple_delay_slots`, move one `(const_int 0)` death to
    // `last_reader` as a concrete `(REG_DEAD (reg ...))` note.
    const size_t vidx = st.base_region.sched_orig_indices[static_cast<size_t>(i)];
    if (!const_zero_note_idxs.empty() && block_view.view_to_orig[vidx].is_delay_slot) {
      size_t zero_cursor = 0;
      absl::btree_set<int> handled_regs;
      for (int r : st.insn_info[static_cast<size_t>(i)].reads) {
        if (zero_cursor >= const_zero_note_idxs.size()) {
          break;
        }
        if (r <= 0 || r >= kMipsFirstPseudoRegister || r == 28 || r == kMipsStackPointerRegno ||
            (st.cfg_ctx.stack_layout.frame_pointer_needed && r == kMipsFramePointerRegno) ||
            handled_regs.contains(r)) {
          continue;
        }
        const std::optional<RtlExpr> reg_expr = FindUsedHardRegExprInPattern(
            st.base_region.sched_block[static_cast<size_t>(i)].pn.pattern, r);
        if (!reg_expr.has_value()) {
          continue;
        }
        const int nr = st.mm.HardRegNRegs(r, reg_expr->mode);
        bool defs_r = false;
        bool live_after_in_base = false;
        for (int k = 0; k < nr; ++k) {
          if (st.insn_info[static_cast<size_t>(i)].defs.contains(r + k)) {
            defs_r = true;
            break;
          }
          if (st.live_after_base[static_cast<size_t>(i)].contains(r + k)) {
            live_after_in_base = true;
          }
        }
        if (defs_r) {
          // `flow.c:2506`, `stmt.c:expand_return`, `reorg.c:918-920`: when
          // a delay-slot return-value `SET` `(set (reg/i:M r) (... r ...))`
          // overwrites a dying pseudo allocated to hard reg `r`, `reorg`
          // zeroes `(REG_DEAD (reg:M r))` to `(REG_DEAD (const_int 0))`.
          // Resolve it in-place onto `i` so `CollectSched2ObstructedMemRegEquivAdditions`
          // observes the exact death boundary of `r` at `i`.
          const RtlExpr* s_dest =
              SingleSetOf(st.base_region.sched_block[static_cast<size_t>(i)].pn.pattern);
          if (s_dest != nullptr && s_dest->SetDest().op == RtlOp::kReg &&
              s_dest->SetDest().flags.integrated && s_dest->SetDest().RegNo() == r &&
              !HasRegDeadNoteCovering(notes, r, st.mm)) {
            transfers.push_back(Stage0PureReaderNoteTransfer{
                .from_idx = i,
                .note_idx = const_zero_note_idxs[zero_cursor++],
                .to_idx = i,
                .note =
                    RtlRegNote{
                        .kind = RtlMachineMode::kRegDead,
                        .list_op = RtlOp::kExprList,
                        .datum = *reg_expr,
                    },
            });
            for (int k = 0; k < nr; ++k) {
              handled_regs.insert(r + k);
            }
          }
          continue;
        }
        if (IsCrossBankFprInputReloadOperandInOrder(st.base_region.sched_block, cand_in_base,
                                                    pos_in_cand[static_cast<size_t>(i)], r,
                                                    reg_expr->mode, st.mm)) {
          continue;
        }
        int target_idx = -1;
        if (!live_after_in_base) {
          target_idx = find_cand_last_reader(i, r, nr);
        } else if (!HasRegDeadNoteCovering(notes, r, st.mm)) {
          // `reorg.c:918-920` + `sched.c:schedule_block`: `sched2` moved a
          // pure reader of `r` after `i` (keeping `r` live after `i` in
          // `base_region`), while in `cand_in_base` (`kSched1` preimage)
          // `i` itself is the true last reader of `r`. Resolve one
          // `(const_int 0)` placeholder in-place onto `i` so `Pass01`'s
          // `ResolvePostReloadConstZeroRegDeadNotes` preserves `i`'s death
          // instead of attributing `(REG_DEAD r)` to the later `sched2` reader.
          bool live_after_in_cand = false;
          for (int k = 0; k < nr; ++k) {
            if (st.live_at_end_s2.contains(r + k)) {
              live_after_in_cand = true;
              break;
            }
          }
          for (int p = pos_in_cand[static_cast<size_t>(i)] + 1;
               p < epilogue_begin && !live_after_in_cand; ++p) {
            const int m = cand_in_base[static_cast<size_t>(p)];
            bool m_defs_all = true;
            for (int k = 0; k < nr; ++k) {
              if (st.insn_info[static_cast<size_t>(m)].reads.contains(r + k)) {
                live_after_in_cand = true;
                break;
              }
              if (!st.insn_info[static_cast<size_t>(m)].defs.contains(r + k)) {
                m_defs_all = false;
              }
            }
            if (m_defs_all) {
              break;
            }
          }
          if (!live_after_in_cand) {
            target_idx = i;
          }
        }
        if (target_idx >= 0) {
          transfers.push_back(Stage0PureReaderNoteTransfer{
              .from_idx = i,
              .note_idx = const_zero_note_idxs[zero_cursor++],
              .to_idx = target_idx,
              .note =
                  RtlRegNote{
                      .kind = RtlMachineMode::kRegDead,
                      .list_op = RtlOp::kExprList,
                      .datum = *reg_expr,
                  },
          });
          for (int k = 0; k < nr; ++k) {
            handled_regs.insert(r + k);
          }
        }
      }
    }
  }
  return transfers;
}

enum class PreimagePassResult { kNotSolved, kSolvedWithoutMutation, kCommitted };

// Evaluates candidate `kSched2` preimages in reverse priority order, applying
// pure-reader `REG_DEAD` transfers, `local-alloc.c:540-665, 990-1055`
// `REG_EQUIV` recovery, optional `local-alloc.c:1266-1300, 1851-1886`
// (`combine_regs`) RMW splits, and strict `kSched2`/`kSched1` verification.
PreimagePassResult TryAttributeBlockViaPreimageCandidates(
    const Stage0PureReaderBlockState& st, absl::Span<const std::vector<int>> cands,
    bool allow_post_call_rmw_splits, bool allow_general_rmw_splits,
    Stage0BlockDelayView* block_view, RtlBasicBlock* out_block,
    std::optional<std::string>* out_epilogue_delay_pattern) {
  const int n = static_cast<int>(st.base_region.sched_block.size());
  const int prologue_end = st.bounds.prologue_end;
  const int epilogue_begin = st.bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  bool saw_zero_mutation_solution = false;
  absl::btree_set<std::vector<int>> tried_cands;

  for (auto cand_it = cands.rbegin(); cand_it != cands.rend(); ++cand_it) {
    const std::vector<int>& cand_in_base = *cand_it;
    if (!tried_cands.insert(cand_in_base).second) {
      continue;
    }
    std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
    for (int pos = 0; pos < n; ++pos) {
      pos_in_cand[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])] = pos;
    }

    const std::vector<Stage0PureReaderNoteTransfer> transfers =
        CollectPureReaderDeadTransfersForCandidate(st, *block_view, cand_in_base, pos_in_cand);

    // Apply pure-reader `REG_DEAD` transfers, then discover any `sched2`-obstructed
    // `REG_EQUIV (mem ...)` notes (`local-alloc.c:540-665, 990-1000, 1049-1055`).
    std::vector<Insn> transferred_base = st.base_region.sched_block;
    absl::btree_map<int, absl::btree_set<size_t>> erase_indices;
    for (const Stage0PureReaderNoteTransfer& tr : transfers) {
      erase_indices[tr.from_idx].insert(tr.note_idx);
    }
    for (const auto& [from_idx, idxs] : erase_indices) {
      auto& notes = transferred_base[static_cast<size_t>(from_idx)].pn.reg_notes;
      std::vector<RtlRegNote> kept;
      kept.reserve(notes.size());
      for (size_t ni = 0; ni < notes.size(); ++ni) {
        if (!idxs.contains(ni)) {
          kept.push_back(std::move(notes[ni]));
        }
      }
      notes = std::move(kept);
    }
    for (const Stage0PureReaderNoteTransfer& tr : transfers) {
      transferred_base[static_cast<size_t>(tr.to_idx)].pn.reg_notes.push_back(tr.note);
    }
    std::vector<std::pair<int, int>> resolved_dead_regs;
    for (const Stage0PureReaderNoteTransfer& tr : transfers) {
      if (tr.note.datum.op == RtlOp::kReg) {
        resolved_dead_regs.push_back({tr.to_idx, tr.note.datum.RegNo()});
      }
    }
    absl::btree_set<int> stripped_equiv_targets;
    if (!resolved_dead_regs.empty()) {
      StripSpuriousStoreMemRegEquivForMovedReaders(
          resolved_dead_regs, prologue_end, epilogue_begin, st.insn_info, st.cfg_ctx.stack_layout,
          st.mm, pos_in_cand, &transferred_base, &stripped_equiv_targets);
    }
    std::vector<std::pair<int, RtlRegNote>> dead_targets;
    dead_targets.reserve(transfers.size());
    for (const Stage0PureReaderNoteTransfer& tr : transfers) {
      dead_targets.emplace_back(tr.to_idx, tr.note);
    }
    StripInvalidatedMemRegEquivAfterDeadTransfers(dead_targets, prologue_end, cand_in_base,
                                                  pos_in_cand, st.insn_info, st.mm,
                                                  &transferred_base, &stripped_equiv_targets);
    StripSched2SpuriousLoadMemRegEquivNotes(prologue_end, epilogue_begin, cand_in_base, pos_in_cand,
                                            st.facts_s2, st.mm, st.cfg_ctx.stack_layout,
                                            st.proj_ctx.excluded_prologue_body_indices,
                                            &transferred_base, &stripped_equiv_targets);
    StripSched2SpuriousStoreMemRegEquivNotes(
        prologue_end, epilogue_begin, cand_in_base, pos_in_cand, st.facts_s2, st.mm,
        st.cfg_ctx.stack_layout, st.proj_ctx.excluded_prologue_body_indices, &transferred_base,
        &stripped_equiv_targets);

    std::vector<Insn> transferred_cand;
    transferred_cand.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      Insn insn = transferred_base[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
      insn.luid = pos;
      transferred_cand.push_back(std::move(insn));
    }

    const std::vector<Stage0MemEquivAddition> equiv_additions =
        CollectSched2ObstructedMemRegEquivAdditions(
            st.base_region.sched_block, transferred_cand, cand_in_base, prologue_end,
            epilogue_begin, st.live_at_end_s2, st.facts_s2, st.mm, st.cfg_ctx.stack_layout,
            st.proj_ctx.excluded_prologue_body_indices);
    absl::btree_set<int> added_equiv_targets;
    for (const Stage0MemEquivAddition& add : equiv_additions) {
      added_equiv_targets.insert(add.target_sched_idx);
      auto& bnotes = transferred_base[static_cast<size_t>(add.target_sched_idx)].pn.reg_notes;
      bnotes.insert(bnotes.begin(), add.note);
      auto& cnotes = transferred_cand[static_cast<size_t>(
                                          pos_in_cand[static_cast<size_t>(add.target_sched_idx)])]
                         .pn.reg_notes;
      cnotes.insert(cnotes.begin(), add.note);
    }

    std::vector<Insn> cand_body;
    cand_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      Insn insn = transferred_cand[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      cand_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext cand_proj_ctx = st.proj_ctx;
    cand_proj_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(transferred_cand, st.bounds, st.block_idx, st.mm);
    Sched1ProjectionContext unsplit_cand_ctx = cand_proj_ctx;
    unsplit_cand_ctx.allow_post_call_combine_regs_split = false;
    unsplit_cand_ctx.allow_general_combine_regs_split = false;
    const Sched1ProjectionView unsplit_cand_view =
        ProjectPostReloadBlockToSched1View(cand_body, st.mm, st.live_at_end_s2, unsplit_cand_ctx);
    absl::btree_set<int> rmw_dead_sched_idxs;
    if (unsplit_cand_view.core_insns.size() <= 1) {
      continue;
    }
    const bool unsplit_ach = IsAchievable(unsplit_cand_view.core_insns, unsplit_cand_view.facts_s1,
                                          st.mm, Phase::kSched1, unsplit_cand_view.live_at_end_s1);
    if (!unsplit_ach) {
      if (!allow_post_call_rmw_splits && !allow_general_rmw_splits) {
        continue;
      }
      // Restore minimal erased `(REG_DEAD r)` + store-equiv `(REG_EQUIV (mem ...))`
      // notes on block-local `combine_regs` RMWs whenever the preimage only
      // achieves `kSched1` with those splits (`local-alloc.c:1266-1300, 1851-1886,
      // 990-1000`, `sched.c:1732, 2490`).
      const absl::btree_set<int> cand_rmw_splits = SelectMinimalCombineRegsRmwSplits(
          cand_body, cand_proj_ctx, st.live_at_end_s2, st.mm, allow_general_rmw_splits);
      if (cand_rmw_splits.empty()) {
        continue;
      }
      for (int body_idx : cand_rmw_splits) {
        const int cand_pos = prologue_end + body_idx;
        const int orig_si = cand_in_base[static_cast<size_t>(cand_pos)];
        const RtlExpr* s = SingleSetOf(transferred_cand[static_cast<size_t>(cand_pos)].pn.pattern);
        if (s != nullptr && s->SetDest().op == RtlOp::kReg &&
            !HasRegDeadNoteCovering(transferred_cand[static_cast<size_t>(cand_pos)].pn.reg_notes,
                                    s->SetDest().RegNo(), st.mm)) {
          const RtlRegNote dead_note{
              .kind = RtlMachineMode::kRegDead,
              .list_op = RtlOp::kExprList,
              .datum = s->SetDest(),
          };
          auto& bnotes = transferred_base[static_cast<size_t>(orig_si)].pn.reg_notes;
          bnotes.insert(bnotes.begin(), dead_note);
          auto& cnotes = transferred_cand[static_cast<size_t>(cand_pos)].pn.reg_notes;
          cnotes.insert(cnotes.begin(), dead_note);
          auto& body_notes = cand_body[static_cast<size_t>(body_idx)].pn.reg_notes;
          body_notes.insert(body_notes.begin(), dead_note);
          rmw_dead_sched_idxs.insert(orig_si);
        }
      }
      if (rmw_dead_sched_idxs.empty()) {
        continue;
      }
      const std::vector<Stage0MemEquivAddition> split_equivs =
          CollectSched2ObstructedMemRegEquivAdditions(
              st.base_region.sched_block, transferred_cand, cand_in_base, prologue_end,
              epilogue_begin, st.live_at_end_s2, st.facts_s2, st.mm, st.cfg_ctx.stack_layout,
              st.proj_ctx.excluded_prologue_body_indices, rmw_dead_sched_idxs);
      for (const Stage0MemEquivAddition& add : split_equivs) {
        if (!added_equiv_targets.insert(add.target_sched_idx).second) {
          continue;
        }
        auto& bnotes = transferred_base[static_cast<size_t>(add.target_sched_idx)].pn.reg_notes;
        bnotes.insert(bnotes.begin(), add.note);
        const int cpos = pos_in_cand[static_cast<size_t>(add.target_sched_idx)];
        auto& cnotes = transferred_cand[static_cast<size_t>(cpos)].pn.reg_notes;
        cnotes.insert(cnotes.begin(), add.note);
        if (cpos >= prologue_end && cpos < epilogue_begin) {
          auto& body_notes = cand_body[static_cast<size_t>(cpos - prologue_end)].pn.reg_notes;
          body_notes.insert(body_notes.begin(), add.note);
        }
        rmw_dead_sched_idxs.insert(add.target_sched_idx);
      }
      const Sched1ProjectionView verify_cand_view =
          ProjectPostReloadBlockToSched1View(cand_body, st.mm, st.live_at_end_s2, unsplit_cand_ctx);
      if (verify_cand_view.core_insns.size() <= 1 ||
          !IsAchievable(verify_cand_view.core_insns, verify_cand_view.facts_s1, st.mm,
                        Phase::kSched1, verify_cand_view.live_at_end_s1)) {
        continue;
      }
    }

    // Strictly verify remaining gates:
    //   (a) forward `kSched2` replay on `transferred_cand` reproduces `transferred_base`,
    //   (b) `PreservesPostReloadOrderSafety(..., allow_transfer=false)` passes.
    const DepGraph g_cand_s2 = Analyze(transferred_cand, st.facts_s2, st.mm, Phase::kSched2);
    const ScheduleResult s2_replay = Schedule(transferred_cand, g_cand_s2, st.facts_s2, st.mm,
                                              Phase::kSched2, st.live_at_end_s2);
    if (static_cast<int>(s2_replay.order.size()) != n) {
      continue;
    }
    bool replay_matches = true;
    for (int pos = 0; pos < n; ++pos) {
      if (cand_in_base[static_cast<size_t>(s2_replay.order[static_cast<size_t>(pos)])] != pos) {
        replay_matches = false;
        break;
      }
    }
    const bool safe_ok =
        replay_matches && PreservesPostReloadOrderSafety(
                              transferred_base, cand_in_base, prologue_end, epilogue_begin,
                              st.live_at_end_s2, st.cfg_ctx.cfg_copy_pairs, st.facts_s2, st.mm,
                              /*allow_pure_reader_reg_dead_transfer=*/false,
                              st.cfg_ctx.stack_layout, st.proj_ctx.excluded_prologue_body_indices,
                              /*allow_moved_store_load_equiv_invalidation=*/
                              !stripped_equiv_targets.empty());
    if (!safe_ok) {
      continue;
    }

    absl::btree_set<int> combine_repair_touched;
    TryRepairCombineRegsPureReaderRegDeadConflicts(
        st.block_idx, st.bounds, st.proj_ctx, st.cfg_ctx, st.facts_s2, st.mm, st.live_at_end_s2,
        st.reload_adjacent, cand_in_base,
        /*allow_moved_store_load_equiv_invalidation=*/!stripped_equiv_targets.empty(),
        &transferred_base, &combine_repair_touched);

    if (transfers.empty() && equiv_additions.empty() && stripped_equiv_targets.empty() &&
        rmw_dead_sched_idxs.empty() && st.alignment_touched_sched_idxs.empty() &&
        combine_repair_touched.empty()) {
      saw_zero_mutation_solution = true;
      continue;
    }

    // Commit the verified `REG_DEAD` transfers, `REG_EQUIV` recoveries, and
    // `combine_regs` RMW `REG_DEAD` attributions back to `out_block`.
    absl::btree_set<int> touched_sched_idxs = st.alignment_touched_sched_idxs;
    for (const Stage0PureReaderNoteTransfer& tr : transfers) {
      touched_sched_idxs.insert(tr.from_idx);
      touched_sched_idxs.insert(tr.to_idx);
    }
    for (const Stage0MemEquivAddition& add : equiv_additions) {
      touched_sched_idxs.insert(add.target_sched_idx);
    }
    for (int si : stripped_equiv_targets) {
      touched_sched_idxs.insert(si);
    }
    for (int si : rmw_dead_sched_idxs) {
      touched_sched_idxs.insert(si);
    }
    for (int si : combine_repair_touched) {
      touched_sched_idxs.insert(si);
    }
    StripJump2ConsumedEarlyclobberOutputReloadDeaths(prologue_end, epilogue_begin, st.mm,
                                                     &transferred_base);
    CommitStage0AttributedNotesToBlock(st.base_region, transferred_base, touched_sched_idxs,
                                       block_view, out_block, out_epilogue_delay_pattern);
    return PreimagePassResult::kCommitted;
  }
  return saw_zero_mutation_solution ? PreimagePassResult::kSolvedWithoutMutation
                                    : PreimagePassResult::kNotSolved;
}

// Restores minimal erased `(REG_DEAD r)` and obstructed `REG_EQUIV (mem ...)`
// notes for identity-order `local-alloc.c:1266-1300, 1851-1886` (`combine_regs`)
// RMW splits (`sched.c:1732, 2490`).
bool TryAttributeIdentityCombineRegsRmwSplits(
    const Stage0PureReaderBlockState& st, bool allow_general, Stage0BlockDelayView* block_view,
    RtlBasicBlock* out_block, std::optional<std::string>* out_epilogue_delay_pattern) {
  const int n = static_cast<int>(st.base_region.sched_block.size());
  const int prologue_end = st.bounds.prologue_end;
  const int epilogue_begin = st.bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  const absl::btree_set<int> id_rmw_splits = SelectMinimalCombineRegsRmwSplits(
      st.base_body, st.proj_ctx, st.live_at_end_s2, st.mm, allow_general);
  if (id_rmw_splits.empty()) {
    return false;
  }
  std::vector<Insn> updated_base = st.base_region.sched_block;
  absl::btree_set<int> touched;
  for (int body_idx : id_rmw_splits) {
    const int si = prologue_end + body_idx;
    const RtlExpr* s = SingleSetOf(updated_base[static_cast<size_t>(si)].pn.pattern);
    if (s != nullptr && s->SetDest().op == RtlOp::kReg &&
        !HasRegDeadNoteCovering(updated_base[static_cast<size_t>(si)].pn.reg_notes,
                                s->SetDest().RegNo(), st.mm)) {
      auto& notes = updated_base[static_cast<size_t>(si)].pn.reg_notes;
      notes.insert(notes.begin(), RtlRegNote{
                                      .kind = RtlMachineMode::kRegDead,
                                      .list_op = RtlOp::kExprList,
                                      .datum = s->SetDest(),
                                  });
      touched.insert(si);
    }
  }
  if (touched.empty() ||
      !InvertBlockOrder(updated_base, st.facts_s2, st.mm, Phase::kSched2, st.live_at_end_s2)
           .has_value()) {
    return false;
  }
  std::vector<int> id_perm(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    id_perm[static_cast<size_t>(i)] = i;
  }
  const std::vector<Stage0MemEquivAddition> split_equivs =
      CollectSched2ObstructedMemRegEquivAdditions(
          st.base_region.sched_block, updated_base, id_perm, prologue_end, epilogue_begin,
          st.live_at_end_s2, st.facts_s2, st.mm, st.cfg_ctx.stack_layout,
          st.proj_ctx.excluded_prologue_body_indices, touched);
  for (const Stage0MemEquivAddition& add : split_equivs) {
    auto& bnotes = updated_base[static_cast<size_t>(add.target_sched_idx)].pn.reg_notes;
    bnotes.insert(bnotes.begin(), add.note);
    touched.insert(add.target_sched_idx);
  }
  std::vector<Insn> updated_body;
  updated_body.reserve(static_cast<size_t>(body_len));
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    Insn insn = updated_base[static_cast<size_t>(i)];
    insn.luid = i - prologue_end;
    updated_body.push_back(std::move(insn));
  }
  const Sched1ProjectionView verify_view = ProjectPostReloadBlockToSched1View(
      updated_body, st.mm, st.live_at_end_s2, st.unsplit_base_ctx);
  if (verify_view.core_insns.size() <= 1 ||
      !IsAchievable(verify_view.core_insns, verify_view.facts_s1, st.mm, Phase::kSched1,
                    verify_view.live_at_end_s1)) {
    return false;
  }
  TryRepairCombineRegsPureReaderRegDeadConflicts(
      st.block_idx, st.bounds, st.proj_ctx, st.cfg_ctx, st.facts_s2, st.mm, st.live_at_end_s2,
      st.reload_adjacent, id_perm, /*allow_moved_store_load_equiv_invalidation=*/false,
      &updated_base, &touched);
  touched.insert(st.alignment_touched_sched_idxs.begin(), st.alignment_touched_sched_idxs.end());
  CommitStage0AttributedNotesToBlock(st.base_region, updated_base, touched, block_view, out_block,
                                     out_epilogue_delay_pattern);
  return true;
}

}  // namespace

void AttributeSched2PureReaderRegDeadNotes(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);
  if (flags.opt_level <= 0 || !flags.RunsSched1() || !flags.RunsSched2()) {
    return;
  }
  const MachineModel mm = MachineModel::FromProfile(profile);
  const size_t num_blocks = cfg->rpo_blocks.size();

  RtlControlFlowGraph view_cfg;
  RtlControlFlowGraph live_cfg;
  std::vector<Stage0BlockDelayView> block_views;
  BuildStage0UnpackedDelayViewCfg(*cfg, profile, &view_cfg, &live_cfg, &block_views);
  AlignStage0DelaySlotsToSched2View(profile, mm, &view_cfg, &live_cfg, &block_views);
  ApplyStage0B0Jump2DeletedArgCopiesWithRollback(profile, mm, &view_cfg, &live_cfg, &block_views);
  const RegFacts facts_s2 = RegFacts::FromCfg(view_cfg, profile, Phase::kSched2);
  const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(view_cfg, live_cfg, mm);

  for (size_t b = 0; b < num_blocks; ++b) {
    if (!block_views[b].eligible) {
      continue;
    }
    const RtlBasicBlock& vblk = view_cfg.rpo_blocks[b];
    const ParsedSchedRegion base_region = ParseBlockForSchedModel(vblk, b, Phase::kSched2);
    if (!base_region.valid || base_region.has_mid_block_loop_barrier) {
      continue;
    }
    const int n = static_cast<int>(base_region.sched_block.size());
    if (n <= 2) {
      continue;
    }
    absl::btree_set<int> live_at_end_s2;
    for (int succ : vblk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(base_region.sched_block, b);
    const int prologue_end = bounds.prologue_end;
    const int epilogue_begin = bounds.epilogue_begin;
    const int body_len = epilogue_begin - prologue_end;
    if (body_len <= 1) {
      continue;
    }
    std::vector<Insn> base_body;
    base_body.reserve(static_cast<size_t>(body_len));
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      Insn insn = base_region.sched_block[static_cast<size_t>(i)];
      insn.luid = i - prologue_end;
      base_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext proj_ctx = BuildSched1ProjectionContextForBlock(b, cfg_ctx);
    proj_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(base_region.sched_block, bounds, b, mm);

    absl::btree_set<int> alignment_touched_sched_idxs;
    if (!block_views[b].alignment_touched_view_idxs.empty()) {
      for (int si = 0; si < n; ++si) {
        if (block_views[b].alignment_touched_view_idxs.contains(
                base_region.sched_orig_indices[static_cast<size_t>(si)])) {
          alignment_touched_sched_idxs.insert(si);
        }
      }
    }

    Sched1ProjectionContext unsplit_base_ctx = proj_ctx;
    unsplit_base_ctx.allow_post_call_combine_regs_split = false;
    unsplit_base_ctx.allow_general_combine_regs_split = false;
    const Sched1ProjectionView unsplit_base_view =
        ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, unsplit_base_ctx);
    if (unsplit_base_view.core_insns.size() <= 1 ||
        IsAchievable(unsplit_base_view.core_insns, unsplit_base_view.facts_s1, mm, Phase::kSched1,
                     unsplit_base_view.live_at_end_s1)) {
      std::vector<Insn> repaired_base = base_region.sched_block;
      absl::btree_set<int> early_touched = alignment_touched_sched_idxs;
      if (unsplit_base_view.core_insns.size() > 1) {
        std::vector<int> id_cand(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
          id_cand[static_cast<size_t>(i)] = i;
        }
        std::vector<AdjacentPair> early_reload_adj;
        for (int i = 0; i + 1 < n; ++i) {
          if (IsAdjacentMdOutputReloadPair(base_region.sched_block[static_cast<size_t>(i)],
                                           base_region.sched_block[static_cast<size_t>(i + 1)]) ||
              IsAdjacentReloadContiguityPair(base_region.sched_block[static_cast<size_t>(i)],
                                             base_region.sched_block[static_cast<size_t>(i + 1)],
                                             mm, cfg_ctx.stack_layout)) {
            early_reload_adj.push_back(AdjacentPair{.first = i, .second = i + 1});
          }
        }
        TryRepairCombineRegsPureReaderRegDeadConflicts(
            b, bounds, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2, early_reload_adj, id_cand,
            /*allow_moved_store_load_equiv_invalidation=*/false, &repaired_base, &early_touched);
      }
      if (!early_touched.empty() && unsplit_base_view.core_insns.size() > 1) {
        CommitStage0AttributedNotesToBlock(base_region, repaired_base, early_touched,
                                           &block_views[b], &cfg->rpo_blocks[b],
                                           &cfg->sideband.epilogue_delay_pattern);
      }
      continue;
    }

    const Sched1ProjectionView base_view =
        ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, proj_ctx);
    std::vector<AdjacentPair> reload_adjacent;
    for (int i = 0; i + 1 < n; ++i) {
      if (IsAdjacentMdOutputReloadPair(base_region.sched_block[static_cast<size_t>(i)],
                                       base_region.sched_block[static_cast<size_t>(i + 1)]) ||
          IsAdjacentReloadContiguityPair(base_region.sched_block[static_cast<size_t>(i)],
                                         base_region.sched_block[static_cast<size_t>(i + 1)], mm,
                                         cfg_ctx.stack_layout)) {
        reload_adjacent.push_back(AdjacentPair{.first = i, .second = i + 1});
      }
    }
    const std::vector<std::vector<int>> base_preimages =
        FindStage0Sched2PreimageCandidates(base_region.sched_block, b, bounds, base_view, proj_ctx,
                                           cfg_ctx, facts_s2, mm, live_at_end_s2, reload_adjacent);

    std::vector<InsnRegReadDefDeaths> insn_info(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      insn_info[static_cast<size_t>(i)] =
          CollectInsnRegReadDefDeaths(base_region.sched_block[static_cast<size_t>(i)], mm);
    }
    std::vector<absl::btree_set<int>> live_after_base(static_cast<size_t>(n));
    {
      absl::btree_set<int> cur_live = live_at_end_s2;
      for (int i = n - 1; i >= 0; --i) {
        live_after_base[static_cast<size_t>(i)] = cur_live;
        for (int d : insn_info[static_cast<size_t>(i)].defs) {
          cur_live.erase(d);
        }
        for (int r : insn_info[static_cast<size_t>(i)].reads) {
          cur_live.insert(r);
        }
      }
    }

    const Stage0PureReaderBlockState block_state{
        .block_idx = b,
        .base_region = base_region,
        .bounds = bounds,
        .base_body = base_body,
        .proj_ctx = proj_ctx,
        .unsplit_base_ctx = unsplit_base_ctx,
        .cfg_ctx = cfg_ctx,
        .facts_s2 = facts_s2,
        .mm = mm,
        .live_at_end_s2 = live_at_end_s2,
        .reload_adjacent = reload_adjacent,
        .insn_info = insn_info,
        .live_after_base = live_after_base,
        .alignment_touched_sched_idxs = alignment_touched_sched_idxs,
    };

    // Pass 1a: post-call `combine_regs` identity splits (`sched.c:1732`,
    // `local-alloc.c:1266-1300`) match Stage 3's default projection and run
    // before pure-reorder exit so Stage 0 and Stage 3 never disagree.
    if (TryAttributeIdentityCombineRegsRmwSplits(block_state, /*allow_general=*/false,
                                                 &block_views[b], &cfg->rpo_blocks[b],
                                                 &cfg->sideband.epilogue_delay_pattern)) {
      continue;
    }

    // Pass 1b: try `kSched2` preimages with post-call `combine_regs` RMW splits
    // (without general intra-block RMW splits).
    if (TryAttributeBlockViaPreimageCandidates(
            block_state, base_preimages,
            /*allow_post_call_rmw_splits=*/true,
            /*allow_general_rmw_splits=*/false, &block_views[b], &cfg->rpo_blocks[b],
            &cfg->sideband.epilogue_delay_pattern) != PreimagePassResult::kNotSolved) {
      continue;
    }

    // Pass 1c: invert `reorg.c:918-920` in-place when `sched2` moved a pure
    // reader past a delay-slot insn carrying `(REG_DEAD (const_int 0))`.
    if (TryResolveStage0DelaySlotConstZeroInPlace(
            base_region, b, bounds, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2,
            reload_adjacent, insn_info, live_after_base, alignment_touched_sched_idxs,
            &block_views[b], &cfg->rpo_blocks[b])) {
      continue;
    }

    // Pass 2: general intra-block `combine_regs` identity RMW splits when no
    // zero-split or post-call `kSched2` preimage exists.
    if (TryAttributeIdentityCombineRegsRmwSplits(block_state, /*allow_general=*/true,
                                                 &block_views[b], &cfg->rpo_blocks[b],
                                                 &cfg->sideband.epilogue_delay_pattern)) {
      continue;
    }

    // Pass 3: discover `kSched2` preimages with general `combine_regs` RMW splits
    // enabled so blocks needing BOTH a `sched2` reorder and an RMW `(REG_DEAD r)`
    // attribution are found cleanly (trying call-used-only RMW splits first so
    // callee-saved multi-set accumulators do not distort `birthing_insn_p`).
    std::vector<std::vector<int>> split_preimages = base_preimages;
    Sched1ProjectionContext cu_proj_ctx = proj_ctx;
    cu_proj_ctx.allow_call_used_general_combine_regs_split = true;
    const Sched1ProjectionView cu_base_view =
        ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, cu_proj_ctx);
    if (!cu_base_view.implicit_combine_regs_split_orig_indices.empty()) {
      for (std::vector<int>& gc : FindStage0Sched2PreimageCandidates(
               base_region.sched_block, b, bounds, cu_base_view, cu_proj_ctx, cfg_ctx, facts_s2, mm,
               live_at_end_s2, reload_adjacent)) {
        split_preimages.push_back(std::move(gc));
      }
    }
    Sched1ProjectionContext gen_proj_ctx = proj_ctx;
    gen_proj_ctx.allow_general_combine_regs_split = true;
    const Sched1ProjectionView gen_base_view =
        ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, gen_proj_ctx);
    if (!gen_base_view.implicit_combine_regs_split_orig_indices.empty()) {
      std::vector<std::vector<int>> gen_cands = FindStage0Sched2PreimageCandidates(
          base_region.sched_block, b, bounds, gen_base_view, gen_proj_ctx, cfg_ctx, facts_s2, mm,
          live_at_end_s2, reload_adjacent);
      for (std::vector<int>& gc : gen_cands) {
        split_preimages.push_back(std::move(gc));
      }
    }
    if (!split_preimages.empty()) {
      if (TryAttributeBlockViaPreimageCandidates(
              block_state, split_preimages,
              /*allow_post_call_rmw_splits=*/true,
              /*allow_general_rmw_splits=*/true, &block_views[b], &cfg->rpo_blocks[b],
              &cfg->sideband.epilogue_delay_pattern) != PreimagePassResult::kNotSolved) {
        continue;
      }
    }

    // Pass 4: symmetric inverse of Pass 2 (`flow.c:2506-2515`, `sched.c:2490`)
    // — when all preimage and split passes fail to make the block
    // `kSched1`-achievable, retract a minimal speculative self-set `REG_DEAD`
    // if doing so restores both `kSched2` and `kSched1` achievability.
    (void)TryRetractSpeculativeSelfSetRmwRegDeadNotes(
        base_region, b, bounds, unsplit_base_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2,
        alignment_touched_sched_idxs, &block_views[b], &cfg->rpo_blocks[b]);
  }
}

namespace {

template <typename Fn>
void ForEachStmtInPattern(const RtlExpr& pat, Fn&& fn) {
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      fn(ch);
    }
  } else {
    fn(pat);
  }
}

template <typename Fn>
void ForEachMemInExpr(const RtlExpr& node, Fn&& fn) {
  if (node.op == RtlOp::kMem) {
    fn(node);
    return;
  }
  for (const RtlExpr& ch : node.children) {
    ForEachMemInExpr(ch, fn);
  }
}

template <typename Fn>
void ForEachMemInExprMut(RtlExpr* node, Fn&& fn) {
  if (node->op == RtlOp::kMem) {
    fn(node);
    return;
  }
  for (RtlExpr& ch : node->children) {
    ForEachMemInExprMut(&ch, fn);
  }
}

bool ExtractBasePlusConst(const RtlExpr& addr, int* reg_out, int64_t* off_out) {
  if (addr.op == RtlOp::kReg) {
    *reg_out = addr.RegNo();
    *off_out = 0;
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      addr.children[1].op == RtlOp::kConstInt) {
    *reg_out = addr.children[0].RegNo();
    *off_out = addr.children[1].int_val;
    return true;
  }
  return false;
}

// Extracts the base symbol name and whether an explicit non-zero offset is
// present from a static address expression (`varasm.c:588-604`, `mips.md`).
bool ExtractStaticSymbolFromAddr(const RtlExpr& addr, std::string* sym_out, bool* has_offset_out) {
  if (addr.op == RtlOp::kSymbolRef && !addr.str_val.empty()) {
    *sym_out = addr.str_val;
    *has_offset_out = false;
    return true;
  }
  if (addr.op == RtlOp::kConst && addr.children.size() == 1) {
    return ExtractStaticSymbolFromAddr(addr.children[0], sym_out, has_offset_out);
  }
  if (addr.op == RtlOp::kLoSum && addr.children.size() == 2) {
    return ExtractStaticSymbolFromAddr(addr.children[1], sym_out, has_offset_out);
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2) {
    for (int i = 0; i < 2; ++i) {
      const RtlExpr& a = addr.children[static_cast<size_t>(i)];
      const RtlExpr& b = addr.children[static_cast<size_t>(1 - i)];
      if (a.op == RtlOp::kSymbolRef && !a.str_val.empty() && b.op == RtlOp::kConstInt) {
        *sym_out = a.str_val;
        *has_offset_out = (b.int_val != 0);
        return true;
      }
    }
  }
  return false;
}

struct SymRegOrigin {
  std::string symbol;
  bool aggregate_access = false;
  bool operator<(const SymRegOrigin& o) const {
    if (symbol != o.symbol) return symbol < o.symbol;
    return aggregate_access < o.aggregate_access;
  }
  bool operator==(const SymRegOrigin& o) const {
    return symbol == o.symbol && aggregate_access == o.aggregate_access;
  }
};

using SymOriginMap = absl::btree_map<int, absl::btree_set<SymRegOrigin>>;

void ResolveStaticSymbolsFromAddr(const RtlExpr& addr, const SymOriginMap& origins,
                                  absl::btree_set<SymRegOrigin>* out) {
  std::string sym;
  bool has_off = false;
  if (ExtractStaticSymbolFromAddr(addr, &sym, &has_off)) {
    out->insert(SymRegOrigin{.symbol = std::move(sym), .aggregate_access = has_off});
    return;
  }
  int base = -1;
  int64_t off = 0;
  if (ExtractBasePlusConst(addr, &base, &off)) {
    if (const auto it = origins.find(base); it != origins.end()) {
      for (const SymRegOrigin& orig : it->second) {
        out->insert(SymRegOrigin{.symbol = orig.symbol,
                                 .aggregate_access = orig.aggregate_access || (off != 0)});
      }
    }
  }
}

bool ResolveUniqueStaticSymbolFromAddr(const RtlExpr& addr, const SymOriginMap& origins,
                                       std::string* sym_out, bool* agg_out) {
  absl::btree_set<SymRegOrigin> resolved;
  ResolveStaticSymbolsFromAddr(addr, origins, &resolved);
  if (resolved.empty()) return false;
  const std::string& first_sym = resolved.begin()->symbol;
  bool agg = false;
  for (const SymRegOrigin& o : resolved) {
    if (o.symbol != first_sym) return false;
    agg = agg || o.aggregate_access;
  }
  *sym_out = first_sym;
  *agg_out = agg;
  return true;
}

// Propagates static-symbol address provenance sets through a single
// instruction's `SET` destination (`varasm.c:588-604`).
void UpdateSymRegOriginsForInsn(const Insn& insn, const MachineModel& mm, SymOriginMap* origins) {
  if (insn.kind == Insn::Kind::kCall) {
    for (auto it = origins->begin(); it != origins->end();) {
      it = mm.CallUsed(it->first) ? origins->erase(it) : std::next(it);
    }
    return;
  }
  const RtlExpr& pat = insn.pn.pattern;
  if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg) {
    const int dst = pat.SetDest().RegNo();
    const RtlExpr& src = pat.SetSrc();
    std::string sym;
    bool has_off = false;
    if (ExtractStaticSymbolFromAddr(src, &sym, &has_off) ||
        (src.op == RtlOp::kHigh && src.children.size() == 1 &&
         ExtractStaticSymbolFromAddr(src.children[0], &sym, &has_off))) {
      (*origins)[dst] = {SymRegOrigin{.symbol = std::move(sym), .aggregate_access = has_off}};
      return;
    }
    int base = -1;
    int64_t off = 0;
    if (ExtractBasePlusConst(src, &base, &off)) {
      if (const auto it = origins->find(base); it != origins->end() && !it->second.empty()) {
        absl::btree_set<SymRegOrigin> next;
        for (const SymRegOrigin& o : it->second) {
          next.insert(SymRegOrigin{.symbol = o.symbol,
                                   .aggregate_access = o.aggregate_access || (off != 0)});
        }
        (*origins)[dst] = std::move(next);
        return;
      }
    } else if (src.op == RtlOp::kPlus && src.children.size() == 2 &&
               src.children[0].op == RtlOp::kReg && src.children[1].op == RtlOp::kReg) {
      absl::btree_set<SymRegOrigin> next;
      for (int r : {src.children[0].RegNo(), src.children[1].RegNo()}) {
        if (const auto it = origins->find(r); it != origins->end()) {
          for (const SymRegOrigin& o : it->second) {
            next.insert(SymRegOrigin{.symbol = o.symbol, .aggregate_access = true});
          }
        }
      }
      if (!next.empty()) {
        (*origins)[dst] = std::move(next);
        return;
      }
    } else if (src.op == RtlOp::kMinus && src.children.size() == 2 &&
               src.children[0].op == RtlOp::kReg) {
      if (const auto it = origins->find(src.children[0].RegNo());
          it != origins->end() && !it->second.empty()) {
        absl::btree_set<SymRegOrigin> next;
        for (const SymRegOrigin& o : it->second) {
          next.insert(SymRegOrigin{.symbol = o.symbol, .aggregate_access = true});
        }
        (*origins)[dst] = std::move(next);
        return;
      }
    }
    origins->erase(dst);
    return;
  }
  absl::btree_set<int> fdefs, pdefs;
  CollectFullRegDefs(pat, &fdefs, &pdefs);
  for (int r : fdefs) origins->erase(r);
  for (int r : pdefs) origins->erase(r);
}

struct CfgReadOnlySymbolInfo {
  absl::btree_set<std::string> read_only_symbols;
  absl::btree_set<std::string> aggregate_symbols;
  std::vector<SymOriginMap> entry_sym_origins;
};

// Identifies non-volatile static symbols that are read without ever being
// written or having their address escaped across `view_cfg` (`varasm.c:601-604`),
// propagating static-symbol register provenance across CFG edges via RPO fixpoint.
CfgReadOnlySymbolInfo CollectCfgReadOnlyStaticSymbols(const RtlControlFlowGraph& view_cfg,
                                                      const MachineModel& mm) {
  const size_t nb = view_cfg.rpo_blocks.size();
  std::vector<ParsedSchedRegion> regions(nb);
  for (size_t b = 0; b < nb; ++b) {
    regions[b] = ParseBlockForSchedModel(view_cfg.rpo_blocks[b], b, Phase::kSched2);
  }
  std::vector<SymOriginMap> in_origins(nb);
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < nb; ++b) {
      SymOriginMap cur = in_origins[b];
      if (regions[b].valid) {
        for (const Insn& insn : regions[b].sched_block) {
          UpdateSymRegOriginsForInsn(insn, mm, &cur);
        }
      }
      for (int succ : view_cfg.rpo_blocks[b].successors) {
        if (succ < 0 || static_cast<size_t>(succ) >= nb) continue;
        SymOriginMap& dst_map = in_origins[static_cast<size_t>(succ)];
        for (const auto& [reg, syms] : cur) {
          auto& dst_set = dst_map[reg];
          const size_t before = dst_set.size();
          dst_set.insert(syms.begin(), syms.end());
          if (dst_set.size() != before) changed = true;
        }
      }
    }
  }

  absl::btree_set<std::string> read_syms, written_or_escaped, aggregate_syms;
  for (size_t b = 0; b < nb; ++b) {
    if (!regions[b].valid) continue;
    SymOriginMap origins = in_origins[b];
    for (const Insn& insn : regions[b].sched_block) {
      if (insn.kind == Insn::Kind::kCall) {
        absl::btree_set<int> used_regs;
        for (const RtlExpr& fu : insn.pn.call_fusage) CollectRegReads(fu, &used_regs);
        for (int r : used_regs) {
          if (const auto it = origins.find(r); it != origins.end()) {
            for (const SymRegOrigin& o : it->second) written_or_escaped.insert(o.symbol);
          }
        }
        UpdateSymRegOriginsForInsn(insn, mm, &origins);
        continue;
      }
      auto handle_read_mem = [&](const RtlExpr& mem) {
        if (mem.children.empty()) return;
        absl::btree_set<SymRegOrigin> resolved;
        ResolveStaticSymbolsFromAddr(mem.children[0], origins, &resolved);
        for (const SymRegOrigin& o : resolved) {
          if (mem.flags.volatil) {
            written_or_escaped.insert(o.symbol);
          } else {
            read_syms.insert(o.symbol);
            if (o.aggregate_access) aggregate_syms.insert(o.symbol);
          }
        }
      };
      ForEachStmtInPattern(insn.pn.pattern, [&](const RtlExpr& stmt) {
        if (stmt.op == RtlOp::kUse && !stmt.children.empty() &&
            stmt.children[0].op == RtlOp::kReg) {
          if (const auto it = origins.find(stmt.children[0].RegNo()); it != origins.end()) {
            for (const SymRegOrigin& o : it->second) written_or_escaped.insert(o.symbol);
          }
          return;
        }
        if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
          if (stmt.SetDest().op == RtlOp::kMem && !stmt.SetDest().children.empty()) {
            absl::btree_set<SymRegOrigin> dst_syms;
            ResolveStaticSymbolsFromAddr(stmt.SetDest().children[0], origins, &dst_syms);
            for (const SymRegOrigin& o : dst_syms) written_or_escaped.insert(o.symbol);
            if (stmt.SetSrc().op == RtlOp::kReg) {
              if (const auto it = origins.find(stmt.SetSrc().RegNo()); it != origins.end()) {
                for (const SymRegOrigin& o : it->second) written_or_escaped.insert(o.symbol);
              }
            }
            std::string sym;
            bool agg = false;
            if (ExtractStaticSymbolFromAddr(stmt.SetSrc(), &sym, &agg)) {
              written_or_escaped.insert(sym);
            }
          }
          ForEachMemInExpr(stmt.SetSrc(), handle_read_mem);
        } else {
          ForEachMemInExpr(stmt, handle_read_mem);
        }
      });
      UpdateSymRegOriginsForInsn(insn, mm, &origins);
    }
  }
  CfgReadOnlySymbolInfo info;
  for (const std::string& s : read_syms) {
    if (!written_or_escaped.contains(s)) info.read_only_symbols.insert(s);
  }
  info.aggregate_symbols = std::move(aggregate_syms);
  info.entry_sym_origins = std::move(in_origins);
  return info;
}

struct MemAliasFlagEdit {
  int sched_idx = -1;
  bool is_write = false;
  bool set_in_struct = false;
  bool set_unchanging = false;
};

struct MemAliasGroup {
  enum class Kind { kReadOnlySymbol, kReadOnlyPtr, kAggregatePtrOffset, kLocalStackArray };
  Kind kind = Kind::kReadOnlySymbol;
  std::vector<MemAliasFlagEdit> edits;
};

// Collects candidate erased `MEM_UNCHANGING_P` (`/u`) and `MEM_IN_STRUCT_P`
// (`/s`) semantic groups in `sched_block` (`varasm.c:601-604`,
// `expr.c:4534, 4571-4586, 4889`).
std::vector<MemAliasGroup> CollectErasedMemAliasGroupsForBlock(
    absl::Span<const Insn> sched_block, const PostReloadBodyBounds& bounds,
    const absl::btree_set<int>& excluded_prologue_body_indices,
    const PostReloadStackLayout& stack_layout, const CfgReadOnlySymbolInfo& ro_syms,
    const SymOriginMap& entry_sym_origins, const MachineModel& mm) {
  const int n = static_cast<int>(sched_block.size());
  const bool fp_needed = stack_layout.valid && stack_layout.frame_pointer_needed;
  auto is_general_ptr_reg = [&](int r) {
    return (r >= 2 && r <= 25) || (r == kMipsFramePointerRegno && !fp_needed);
  };

  absl::btree_map<std::string, std::vector<MemAliasFlagEdit>> sym_edits;
  absl::btree_map<int, int> reg_ver;
  // Tracks transitive `(reg, version)` pointer origins so writing through a
  // derived address (`a2 = a1 + (idx << 2)`) marks `a1` as written and reading
  // through a pointer derived from a written base (`q = out + idx`) suppresses
  // `/u` on `q`.
  absl::btree_map<int, absl::btree_set<std::pair<int, int>>> ptr_bases;
  absl::btree_map<std::pair<int, int>, std::vector<MemAliasFlagEdit>> ptr_ro_reads;
  absl::btree_map<std::pair<int, int>, absl::btree_set<std::pair<int, int>>> ptr_key_origins;
  absl::btree_set<std::pair<int, int>> ptr_written_vers;
  absl::btree_map<std::pair<int, int>, std::vector<MemAliasFlagEdit>> ptr_agg_edits;

  struct StackAccessInfo {
    int sched_idx = -1;
    bool is_write = false;
    bool already_in_struct = false;
  };
  absl::btree_map<std::pair<RtlMachineMode, int64_t>, std::vector<StackAccessInfo>> stack_accesses;
  SymOriginMap origins = entry_sym_origins;

  auto get_ptr_bases = [&](int r) -> absl::btree_set<std::pair<int, int>> {
    if (const auto it = ptr_bases.find(r); it != ptr_bases.end()) return it->second;
    if (is_general_ptr_reg(r)) return {{r, reg_ver[r]}};
    return {};
  };

  auto record_mem = [&](int si, const RtlExpr& mem, bool is_write) {
    if (mem.op != RtlOp::kMem || mem.children.empty() || mem.flags.volatil) return;
    const RtlExpr& addr = mem.children[0];
    std::string sym;
    bool sym_agg = false;
    if (!is_write && ResolveUniqueStaticSymbolFromAddr(addr, origins, &sym, &sym_agg) &&
        ro_syms.read_only_symbols.contains(sym)) {
      const bool want_s =
          (sym_agg || ro_syms.aggregate_symbols.contains(sym)) && mem.mode != RtlMachineMode::kQi;
      if (!mem.flags.unchanging || (want_s && !mem.flags.in_struct)) {
        sym_edits[sym].push_back({si, false, want_s, true});
      }
      return;
    }

    int base = -1;
    int64_t off = 0;
    if (!ExtractBasePlusConst(addr, &base, &off)) return;

    const bool is_frame_base =
        (base == kMipsStackPointerRegno) || (fp_needed && base == kMipsFramePointerRegno);
    if (is_frame_base && stack_layout.valid && mem.mode != RtlMachineMode::kQi) {
      const bool in_local_area = off >= stack_layout.starting_frame_offset &&
                                 (stack_layout.frame_size <= 0 || off < stack_layout.frame_size) &&
                                 (stack_layout.callee_saved_area_offset < 0 ||
                                  off < stack_layout.callee_saved_area_offset) &&
                                 !stack_layout.caller_save_slot_offsets.contains(off);
      const bool is_body = si >= bounds.prologue_end && si < bounds.epilogue_begin &&
                           !excluded_prologue_body_indices.contains(si - bounds.prologue_end);
      if (in_local_area && is_body) {
        stack_accesses[{mem.mode, off}].push_back({si, is_write, mem.flags.in_struct});
      }
      return;
    }

    if (!is_general_ptr_reg(base) || origins.contains(base)) return;
    const std::pair<int, int> key{base, reg_ver[base]};
    if (is_write) {
      ptr_written_vers.insert(key);
      for (const auto& origin_key : get_ptr_bases(base)) ptr_written_vers.insert(origin_key);
    } else {
      const bool want_s = (off != 0 && mem.mode != RtlMachineMode::kQi);
      if (!mem.flags.unchanging || (want_s && !mem.flags.in_struct)) {
        ptr_ro_reads[key].push_back({si, false, want_s, true});
        const auto base_origins = get_ptr_bases(base);
        ptr_key_origins[key].insert(base_origins.begin(), base_origins.end());
      }
    }
    if (off != 0 && mem.mode != RtlMachineMode::kQi && !mem.flags.in_struct) {
      ptr_agg_edits[key].push_back({si, is_write, true, false});
    }
  };

  for (int si = 0; si < n; ++si) {
    const Insn& insn = sched_block[static_cast<size_t>(si)];
    ForEachStmtInPattern(insn.pn.pattern, [&](const RtlExpr& stmt) {
      if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
        if (stmt.SetDest().op == RtlOp::kMem) record_mem(si, stmt.SetDest(), /*is_write=*/true);
        ForEachMemInExpr(stmt.SetSrc(), [&](const RtlExpr& m) { record_mem(si, m, false); });
      } else {
        ForEachMemInExpr(stmt, [&](const RtlExpr& m) { record_mem(si, m, false); });
      }
    });
    // Propagate pointer-base provenance and advance register versions.
    if (insn.kind == Insn::Kind::kCall) {
      for (int r = 2; r <= 25; ++r) {
        if (mm.CallUsed(r)) {
          ++reg_ver[r];
          ptr_bases.erase(r);
        }
      }
    } else {
      absl::btree_set<std::pair<int, int>> derived;
      int single_dst = -1;
      const RtlExpr& pat = insn.pn.pattern;
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg) {
        single_dst = pat.SetDest().RegNo();
        const RtlExpr& src = pat.SetSrc();
        int b = -1;
        int64_t o = 0;
        if (ExtractBasePlusConst(src, &b, &o)) {
          derived = get_ptr_bases(b);
        } else if (src.op == RtlOp::kPlus && src.children.size() == 2 &&
                   src.children[0].op == RtlOp::kReg && src.children[1].op == RtlOp::kReg) {
          derived = get_ptr_bases(src.children[0].RegNo());
          for (const auto& k : get_ptr_bases(src.children[1].RegNo())) derived.insert(k);
        } else if (src.op == RtlOp::kMinus && src.children.size() == 2 &&
                   src.children[0].op == RtlOp::kReg) {
          derived = get_ptr_bases(src.children[0].RegNo());
        }
      }
      absl::btree_set<int> fdefs, pdefs;
      CollectFullRegDefs(pat, &fdefs, &pdefs);
      for (int r : fdefs) {
        ++reg_ver[r];
        ptr_bases.erase(r);
      }
      for (int r : pdefs) {
        ++reg_ver[r];
        ptr_bases.erase(r);
      }
      if (single_dst >= 0 && !derived.empty() && is_general_ptr_reg(single_dst)) {
        derived.insert({single_dst, reg_ver[single_dst]});
        ptr_bases[single_dst] = std::move(derived);
      }
    }
    UpdateSymRegOriginsForInsn(insn, mm, &origins);
  }

  std::vector<MemAliasGroup> groups;
  for (auto& [sym, edits] : sym_edits) {
    if (!edits.empty()) groups.push_back({MemAliasGroup::Kind::kReadOnlySymbol, std::move(edits)});
  }
  for (auto& [key, edits] : ptr_ro_reads) {
    bool tainted = ptr_written_vers.contains(key);
    if (!tainted) {
      if (const auto it = ptr_key_origins.find(key); it != ptr_key_origins.end()) {
        for (const auto& orig : it->second) {
          if (ptr_written_vers.contains(orig)) {
            tainted = true;
            break;
          }
        }
      }
    }
    if (!tainted && !edits.empty()) {
      groups.push_back({MemAliasGroup::Kind::kReadOnlyPtr, std::move(edits)});
    }
  }
  for (auto& [key, edits] : ptr_agg_edits) {
    if (!edits.empty()) {
      groups.push_back({MemAliasGroup::Kind::kAggregatePtrOffset, std::move(edits)});
    }
  }
  // Contiguous local stack-array runs per element mode (`expr.c:4534, 4889`).
  absl::btree_map<RtlMachineMode, std::vector<int64_t>> offsets_by_mode;
  for (const auto& [mo, _] : stack_accesses) offsets_by_mode[mo.first].push_back(mo.second);
  for (const auto& [mode, offs] : offsets_by_mode) {
    const int64_t sz = ModeByteSize(mode);
    if (sz <= 0 || offs.size() < 2) continue;
    for (size_t i = 0; i < offs.size();) {
      size_t j = i + 1;
      while (j < offs.size() && offs[j] == offs[j - 1] + sz) ++j;
      if (j - i >= 2) {
        bool has_write = false, has_read = false;
        std::vector<MemAliasFlagEdit> edits;
        for (size_t k = i; k < j; ++k) {
          for (const StackAccessInfo& acc : stack_accesses[{mode, offs[k]}]) {
            has_write = has_write || acc.is_write;
            has_read = has_read || !acc.is_write;
            if (!acc.already_in_struct) edits.push_back({acc.sched_idx, acc.is_write, true, false});
          }
        }
        if (has_write && has_read && !edits.empty()) {
          groups.push_back({MemAliasGroup::Kind::kLocalStackArray, std::move(edits)});
        }
      }
      i = j;
    }
  }
  return groups;
}

void ApplyMemAliasEditToBlock(const MemAliasFlagEdit& edit,
                              const PostReloadStackLayout& stack_layout, std::vector<Insn>* block,
                              absl::btree_set<int>* touched_out) {
  Insn* insn = &(*block)[static_cast<size_t>(edit.sched_idx)];
  auto mark_mem = [&](RtlExpr* mem) {
    if (mem->op != RtlOp::kMem || mem->flags.volatil) return;
    if (edit.set_in_struct) mem->flags.in_struct = true;
    if (edit.set_unchanging) mem->flags.unchanging = true;
  };
  auto handle_stmt = [&](RtlExpr* stmt) {
    if (stmt->op == RtlOp::kSet && stmt->children.size() == 2) {
      if (edit.is_write) {
        if (stmt->children[0].op == RtlOp::kMem) mark_mem(&stmt->children[0]);
      } else {
        ForEachMemInExprMut(&stmt->children[1], mark_mem);
      }
    } else if (!edit.is_write) {
      ForEachMemInExprMut(stmt, mark_mem);
    }
  };
  if (insn->pn.pattern.op == RtlOp::kParallel) {
    for (RtlExpr& ch : insn->pn.pattern.children) handle_stmt(&ch);
  } else {
    handle_stmt(&insn->pn.pattern);
  }
  if (touched_out != nullptr) touched_out->insert(edit.sched_idx);

  // Keep matching `REG_EQUIV (mem ...)` notes consistent with `SET_SRC`
  // (`local-alloc.c:1049-1055`, Rule B) or a subsequent store `SET_DEST`
  // (`local-alloc.c:990-1000`, Rule A, where `reload1.c:3375-3385`
  // `eliminate_regs_in_insn` leaves `$fp` in `REG_NOTES`).
  if (insn->pn.pattern.op != RtlOp::kSet || insn->pn.pattern.children.size() != 2) return;
  const RtlExpr& ref_mem = edit.is_write ? insn->pn.pattern.SetDest() : insn->pn.pattern.SetSrc();
  if (ref_mem.op != RtlOp::kMem || ref_mem.children.empty()) return;
  const int lo = edit.is_write ? 0 : edit.sched_idx;
  const int hi = edit.is_write ? edit.sched_idx : edit.sched_idx + 1;
  for (int k = lo; k < hi; ++k) {
    bool synced = false;
    for (RtlRegNote& note : (*block)[static_cast<size_t>(k)].pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegEquiv &&
          SameMemSlotIgnoringRegNames(note.datum, ref_mem, stack_layout) &&
          (note.datum.flags.in_struct != ref_mem.flags.in_struct ||
           note.datum.flags.unchanging != ref_mem.flags.unchanging)) {
        note.datum.flags.in_struct = ref_mem.flags.in_struct;
        note.datum.flags.unchanging = ref_mem.flags.unchanging;
        synced = true;
      }
    }
    if (synced && touched_out != nullptr) touched_out->insert(k);
  }
}

void SyncRegEquivMemFlagsFromUpdated(const std::vector<RtlRegNote>& upd_notes,
                                     std::vector<RtlRegNote>* target_notes) {
  for (RtlRegNote& tn : *target_notes) {
    if (tn.kind != RtlMachineMode::kRegEquiv || tn.datum.op != RtlOp::kMem ||
        tn.datum.children.empty()) {
      continue;
    }
    for (const RtlRegNote& un : upd_notes) {
      if (un.kind == RtlMachineMode::kRegEquiv && un.datum.op == RtlOp::kMem &&
          !un.datum.children.empty() && un.datum.mode == tn.datum.mode &&
          SameAddrIgnoringRegNames(un.datum.children[0], tn.datum.children[0])) {
        tn.datum.flags.in_struct = un.datum.flags.in_struct;
        tn.datum.flags.unchanging = un.datum.flags.unchanging;
        break;
      }
    }
  }
}

void CommitStage0AttributedMemAliasFlagsToBlock(
    const ParsedSchedRegion& base_region, const std::vector<Insn>& updated_base,
    const absl::btree_set<int>& touched_sched_idxs, Stage0BlockDelayView* block_view,
    RtlBasicBlock* orig_blk, std::optional<std::string>* epilogue_delay_pattern) {
  const size_t orig_len = orig_blk->instructions.size();
  std::vector<bool> saved_has_fusage(orig_len, false);
  std::vector<std::vector<RtlExpr>> saved_fusage(orig_len);
  for (size_t i = 0; i < orig_len && i < orig_blk->rtl_insns.size(); ++i) {
    if (orig_blk->rtl_insns[i].has_explicit_call_fusage) {
      saved_has_fusage[i] = true;
      saved_fusage[i] = orig_blk->rtl_insns[i].call_fusage;
    }
  }
  for (int si : touched_sched_idxs) {
    const Stage0OriginLoc& loc =
        block_view->view_to_orig[base_region.sched_orig_indices[static_cast<size_t>(si)]];
    if (loc.is_synthetic) continue;
    const RtlPatternAndNotes& upd = updated_base[static_cast<size_t>(si)].pn;
    if (loc.is_epilogue_delay) {
      if (epilogue_delay_pattern != nullptr && epilogue_delay_pattern->has_value()) {
        if (auto parsed = ParsePatternAndNotes(**epilogue_delay_pattern); parsed.ok()) {
          parsed->pattern = upd.pattern;
          SyncRegEquivMemFlagsFromUpdated(upd.reg_notes, &parsed->reg_notes);
          *epilogue_delay_pattern = parsed->ToGccPrintRtlString();
        }
      }
    } else if (loc.is_delay_slot) {
      if (auto it = block_view->seqs.find(loc.orig_insn_idx); it != block_view->seqs.end()) {
        it->second.slot_pn.pattern = upd.pattern;
        SyncRegEquivMemFlagsFromUpdated(upd.reg_notes, &it->second.slot_pn.reg_notes);
        orig_blk->instructions[loc.orig_insn_idx] =
            RtlExpr::MakeSequence(
                {
                    it->second.head_wrap,
                    RtlExpr::MakeInsnWrapper(it->second.slot_wrap.op, it->second.slot_wrap.flags,
                                             it->second.slot_pn),
                })
                .ToGccPrintRtlString();
      }
    } else if (auto parsed = ParsePatternAndNotes(orig_blk->instructions[loc.orig_insn_idx]);
               parsed.ok()) {
      parsed->pattern = upd.pattern;
      SyncRegEquivMemFlagsFromUpdated(upd.reg_notes, &parsed->reg_notes);
      orig_blk->instructions[loc.orig_insn_idx] = parsed->ToGccPrintRtlString();
    }
  }
  orig_blk->SyncRtlInsnsFromInstructions();
  for (size_t i = 0; i < orig_len && i < orig_blk->rtl_insns.size(); ++i) {
    if (saved_has_fusage[i] && !orig_blk->rtl_insns[i].has_explicit_call_fusage) {
      orig_blk->rtl_insns[i].has_explicit_call_fusage = true;
      orig_blk->rtl_insns[i].call_fusage = std::move(saved_fusage[i]);
    }
  }
}

}  // namespace

bool AttributeSched2ErasedMemAliasFlags(RtlControlFlowGraph* cfg,
                                        const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) return false;
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);
  if (flags.opt_level <= 0 || !flags.RunsSched1() || !flags.RunsSched2()) return false;
  const MachineModel mm = MachineModel::FromProfile(profile);

  RtlControlFlowGraph view_cfg, live_cfg;
  std::vector<Stage0BlockDelayView> block_views;
  BuildStage0UnpackedDelayViewCfg(*cfg, profile, &view_cfg, &live_cfg, &block_views);
  AlignStage0DelaySlotsToSched2View(profile, mm, &view_cfg, &live_cfg, &block_views);
  ApplyStage0B0Jump2DeletedArgCopiesWithRollback(profile, mm, &view_cfg, &live_cfg, &block_views);
  const RegFacts facts_s2 = RegFacts::FromCfg(view_cfg, profile, Phase::kSched2);
  const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(view_cfg, live_cfg, mm);
  const CfgReadOnlySymbolInfo ro_syms = CollectCfgReadOnlyStaticSymbols(view_cfg, mm);
  bool any_changed = false;

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    if (!block_views[b].eligible) continue;
    const RtlBasicBlock& vblk = view_cfg.rpo_blocks[b];
    const ParsedSchedRegion base_region = ParseBlockForSchedModel(vblk, b, Phase::kSched2);
    if (!base_region.valid || base_region.has_mid_block_loop_barrier ||
        base_region.sched_block.size() <= 2) {
      continue;
    }
    absl::btree_set<int> live_at_end_s2;
    for (int succ : vblk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(base_region.sched_block, b);
    const int body_len = bounds.epilogue_begin - bounds.prologue_end;
    if (body_len <= 1) continue;
    Sched1ProjectionContext proj_ctx = BuildSched1ProjectionContextForBlock(b, cfg_ctx);
    proj_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(base_region.sched_block, bounds, b, mm);

    const SymOriginMap empty_origins;
    const SymOriginMap& block_origins =
        (b < ro_syms.entry_sym_origins.size()) ? ro_syms.entry_sym_origins[b] : empty_origins;
    const std::vector<MemAliasGroup> raw_groups = CollectErasedMemAliasGroupsForBlock(
        base_region.sched_block, bounds, proj_ctx.excluded_prologue_body_indices,
        cfg_ctx.stack_layout, ro_syms, block_origins, mm);
    if (raw_groups.empty()) continue;

    auto compute_dep_sig = [&](const std::vector<Insn>& cand_block) -> std::vector<int> {
      std::vector<int> sig;
      const DepGraph s2 = Analyze(cand_block, facts_s2, mm, Phase::kSched2);
      for (const auto& links : s2.log_links) {
        for (const Link& l : links) {
          sig.push_back(l.target);
          sig.push_back(static_cast<int>(l.kind));
        }
        sig.push_back(-1);
      }
      std::vector<Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
        Insn insn = cand_block[static_cast<size_t>(i)];
        insn.luid = i - bounds.prologue_end;
        cand_body.push_back(std::move(insn));
      }
      const Sched1ProjectionView cand_view =
          ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, proj_ctx);
      const DepGraph s1 = Analyze(cand_view.core_insns, cand_view.facts_s1, mm, Phase::kSched1);
      sig.push_back(-2);
      for (const auto& links : s1.log_links) {
        for (const Link& l : links) {
          sig.push_back(l.target);
          sig.push_back(static_cast<int>(l.kind));
        }
        sig.push_back(-1);
      }
      sig.push_back(-3);
      for (size_t i = 0; i < cand_block.size(); ++i) {
        for (const RtlRegNote& note : cand_block[i].pn.reg_notes) {
          if (note.kind == RtlMachineMode::kRegEquiv && note.datum.op == RtlOp::kMem) {
            sig.push_back(static_cast<int>(i));
            sig.push_back((note.datum.flags.in_struct ? 1 : 0) |
                          (note.datum.flags.unchanging ? 2 : 0));
          }
        }
      }
      return sig;
    };

    auto apply_group_subset = [&](const std::vector<MemAliasGroup>& src_groups,
                                  const std::vector<bool>& active,
                                  absl::btree_set<int>* touched_out) -> std::vector<Insn> {
      std::vector<Insn> cand = base_region.sched_block;
      for (size_t g = 0; g < src_groups.size(); ++g) {
        if (!active[g]) continue;
        for (const MemAliasFlagEdit& edit : src_groups[g].edits) {
          ApplyMemAliasEditToBlock(edit, cfg_ctx.stack_layout, &cand, touched_out);
        }
      }
      return cand;
    };

    // Prune groups whose `/s`/`/u` flags do not change any `kSched2`/`kSched1`
    // dependence edge or `REG_EQUIV` note flag.
    const std::vector<int> base_sig = compute_dep_sig(base_region.sched_block);
    const std::vector<int> all_raw_sig =
        compute_dep_sig(apply_group_subset(raw_groups, std::vector<bool>(raw_groups.size(), true),
                                           /*touched_out=*/nullptr));
    std::vector<MemAliasGroup> groups;
    for (size_t g = 0; g < raw_groups.size(); ++g) {
      std::vector<bool> single(raw_groups.size(), false);
      single[g] = true;
      bool active = (compute_dep_sig(apply_group_subset(raw_groups, single, nullptr)) != base_sig);
      if (!active && raw_groups.size() > 1) {
        std::vector<bool> drop(raw_groups.size(), true);
        drop[g] = false;
        active = (compute_dep_sig(apply_group_subset(raw_groups, drop, nullptr)) != all_raw_sig);
      }
      if (active) groups.push_back(raw_groups[g]);
    }
    if (groups.empty()) continue;

    absl::btree_map<std::vector<int>, bool> verify_cache_fast;
    absl::btree_map<std::vector<int>, bool> verify_cache_tie;
    auto verify_candidate = [&](const std::vector<Insn>& cand_block, bool allow_tie_flips) -> bool {
      const std::vector<int> sig = compute_dep_sig(cand_block);
      auto& cache = allow_tie_flips ? verify_cache_tie : verify_cache_fast;
      if (const auto it = cache.find(sig); it != cache.end()) return it->second;
      if (allow_tie_flips) {
        if (const auto it = verify_cache_fast.find(sig);
            it != verify_cache_fast.end() && it->second) {
          return cache[sig] = true;
        }
      }
      if (!InvertBlockOrder(cand_block, facts_s2, mm, Phase::kSched2, live_at_end_s2).has_value()) {
        verify_cache_fast[sig] = false;
        verify_cache_tie[sig] = false;
        return false;
      }
      std::vector<Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
        Insn insn = cand_block[static_cast<size_t>(i)];
        insn.luid = i - bounds.prologue_end;
        cand_body.push_back(std::move(insn));
      }
      const Sched1ProjectionView cand_view =
          ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, proj_ctx);
      const bool ok = HasSched1AchievablePreimageForSched2Block(
          cand_block, b, bounds, cand_view, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2,
          /*allow_pure_reader_reg_dead_transfer=*/true, allow_tie_flips);
      cache[sig] = ok;
      if (ok && !allow_tie_flips) verify_cache_tie[sig] = true;
      return ok;
    };

    const bool base_s2_ok =
        InvertBlockOrder(base_region.sched_block, facts_s2, mm, Phase::kSched2, live_at_end_s2)
            .has_value();
    if (base_s2_ok && verify_candidate(base_region.sched_block, /*allow_tie_flips=*/false)) {
      continue;
    }
    // Blocks exceeding `sched.c:1740` (`pending_lists_length > 32`) can exceed
    // 48 body insns while still needing tie-flips (`sched.c:2428`) to verify
    // erased `/s`/`/u` alias flags once fast verification on the base block fails.
    const bool allow_tie_tier = !base_s2_ok || body_len <= 128;
    if (base_s2_ok && allow_tie_tier &&
        verify_candidate(base_region.sched_block, /*allow_tie_flips=*/true)) {
      continue;
    }

    auto build_with_mask = [&](const std::vector<bool>& active,
                               absl::btree_set<int>* touched_out) -> std::vector<Insn> {
      return apply_group_subset(groups, active, touched_out);
    };

    std::vector<std::vector<bool>> seed_masks;
    seed_masks.push_back(std::vector<bool>(groups.size(), true));
    std::vector<bool> no_ptr_ro(groups.size(), true), s_only(groups.size(), false),
        u_only(groups.size(), false);
    for (size_t g = 0; g < groups.size(); ++g) {
      const bool is_s = groups[g].kind == MemAliasGroup::Kind::kAggregatePtrOffset ||
                        groups[g].kind == MemAliasGroup::Kind::kLocalStackArray;
      s_only[g] = is_s;
      u_only[g] = !is_s;
      if (groups[g].kind == MemAliasGroup::Kind::kReadOnlyPtr) no_ptr_ro[g] = false;
    }
    seed_masks.push_back(no_ptr_ro);
    seed_masks.push_back(s_only);
    seed_masks.push_back(u_only);
    for (size_t g = 0; g < groups.size(); ++g) {
      if (groups[g].kind == MemAliasGroup::Kind::kReadOnlyPtr) {
        std::vector<bool> drop_one(groups.size(), true);
        drop_one[g] = false;
        seed_masks.push_back(std::move(drop_one));
        std::vector<bool> add_one = no_ptr_ro;
        add_one[g] = true;
        seed_masks.push_back(std::move(add_one));
      }
    }
    for (size_t g = 0; g < groups.size(); ++g) {
      std::vector<bool> single(groups.size(), false);
      single[g] = true;
      seed_masks.push_back(std::move(single));
    }

    std::vector<bool> chosen_mask;
    bool chosen_tie_flips = false;
    const std::vector<bool> tie_tiers =
        allow_tie_tier ? std::vector<bool>{false, true} : std::vector<bool>{false};
    for (bool tier : tie_tiers) {
      for (const std::vector<bool>& mask : seed_masks) {
        if (verify_candidate(build_with_mask(mask, /*touched_out=*/nullptr), tier)) {
          chosen_mask = mask;
          chosen_tie_flips = tier;
          break;
        }
      }
      if (!chosen_mask.empty()) break;
    }
    if (chosen_mask.empty()) continue;

    // Greedily minimize to the minimal subset of semantic alias groups needed.
    for (size_t g = 0; g < groups.size(); ++g) {
      if (!chosen_mask[g]) continue;
      chosen_mask[g] = false;
      if (!verify_candidate(build_with_mask(chosen_mask, /*touched_out=*/nullptr),
                            chosen_tie_flips)) {
        chosen_mask[g] = true;
      }
    }
    absl::btree_set<int> touched_sched_idxs;
    const std::vector<Insn> final_block = build_with_mask(chosen_mask, &touched_sched_idxs);
    if (!touched_sched_idxs.empty()) {
      CommitStage0AttributedMemAliasFlagsToBlock(base_region, final_block, touched_sched_idxs,
                                                 &block_views[b], &cfg->rpo_blocks[b],
                                                 &cfg->sideband.epilogue_delay_pattern);
      any_changed = true;
    }
  }
  return any_changed;
}

}  // namespace rom_nom_nom::lifter::sched_model
