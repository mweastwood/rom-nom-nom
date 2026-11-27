#include "lifter/stack_frame_slot_classifier.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/rtl_cfg_utils.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kStackPointerRegno = 29;
constexpr int kFramePointerRegno = 30;
constexpr int kFirstPseudoRegno = kMipsFirstPseudoRegister;

struct SaveWord {
  size_t save_idx = 0;
  int total_words = 1;
  int word_bit = 0;
  int word_hr = -1;
  int64_t offset = 0;
  int word_bytes = 4;
};

void CollectWrittenHardRegWords(const RtlExpr& pat, bool gp64, bool fp64,
                                std::array<bool, kFirstPseudoRegno>* written) {
  auto mark_dst = [&](const RtlExpr& dst) {
    const RtlExpr* d = &dst;
    while ((d->op == RtlOp::kSubreg || d->op == RtlOp::kStrictLowPart ||
            d->op == RtlOp::kZeroExtract || d->op == RtlOp::kSignExtract) &&
           !d->children.empty()) {
      d = &d->children[0];
    }
    if (d->op == RtlOp::kReg && d->RegNo() >= 0 && d->RegNo() < kFirstPseudoRegno) {
      const int r = d->RegNo();
      const int span = std::max(1, StackSlotHardRegNregs(r, d->mode, gp64, fp64));
      for (int w = r; w < std::min(kFirstPseudoRegno, r + span); ++w) {
        (*written)[w] = true;
      }
    }
  };
  if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
    mark_dst(pat.children[0]);
  } else if (pat.op == RtlOp::kClobber && !pat.children.empty()) {
    mark_dst(pat.children[0]);
  } else if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        mark_dst(elem.children[0]);
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
        mark_dst(elem.children[0]);
      }
    }
  }
}

bool PlanCallerSavePairsForCfg(const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
                               int elim_reg, int64_t min_off, int64_t max_off, bool gp64, bool fp64,
                               bool restrict_hrs, const absl::btree_set<int>& allowed_hrs,
                               std::vector<std::vector<MatchedCallerSaveGroup>>* out_blocks,
                               absl::btree_map<int, StackSlotBound>* out_hr_bounds) {
  const size_t num_blocks = parsed_blocks.size();
  out_blocks->assign(num_blocks, {});
  out_hr_bounds->clear();

  auto match_cs_mem = [&](const RtlExpr& mem, int mode_bytes) -> std::optional<int64_t> {
    if (mem.op != RtlOp::kMem || mem.flags.volatil || mem.flags.in_struct ||
        mem.children.size() != 1 || mode_bytes <= 0) {
      return std::nullopt;
    }
    const std::optional<int64_t> off = ExtractStackBaseOffset(mem.children[0], elim_reg);
    if (!off.has_value() || *off < min_off || *off + mode_bytes > max_off) {
      return std::nullopt;
    }
    return off;
  };

  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& insns = parsed_blocks[b];
    std::vector<size_t> call_indices;
    for (size_t i = 0; i < insns.size(); ++i) {
      if (insns[i].pattern.op == RtlOp::kSequence) {
        call_indices.clear();
        break;
      }
      if (ContainsCallInExpr(insns[i].pattern)) {
        call_indices.push_back(i);
      }
    }
    if (call_indices.empty()) {
      continue;
    }

    absl::btree_map<int, SaveWord> saved_in_slot;
    absl::btree_map<int, SaveWord> needs_restore;
    absl::btree_map<size_t, uint32_t> restored_mask;
    absl::btree_map<size_t, int> expected_words;
    absl::btree_map<size_t, int> save_spans;
    absl::btree_map<size_t, std::vector<MatchedCallerSaveRestore>> restores_per_save;
    absl::btree_map<size_t, std::vector<std::pair<int, StackSlotBound>>> bounds_per_save;
    std::vector<std::pair<size_t, std::vector<SaveWord>>> pending_saves;

    for (size_t i = 0; i < insns.size(); ++i) {
      const RtlPatternAndNotes& pn = insns[i];
      if (ContainsCallInExpr(pn.pattern)) {
        for (const auto& [_, words] : pending_saves) {
          for (const SaveWord& w : words) {
            saved_in_slot[w.word_hr] = w;
          }
        }
        pending_saves.clear();
        needs_restore = saved_in_slot;
        // `caller-save.c:436-467`: registers written by `CALL_INSN`'s own
        // return `SET` are not saved/restored across that call.
        std::array<bool, kFirstPseudoRegno> call_written{};
        CollectWrittenHardRegWords(pn.pattern, gp64, fp64, &call_written);
        for (int w = 0; w < kFirstPseudoRegno; ++w) {
          if (call_written[w]) {
            saved_in_slot.erase(w);
            needs_restore.erase(w);
          }
        }
        continue;
      }

      if (pn.reg_notes.empty() && pn.pattern.IsSet()) {
        const RtlExpr& dst = pn.pattern.SetDest();
        const RtlExpr& src = pn.pattern.SetSrc();
        // Candidate restore: `(set (reg:M hr) (mem:M [elim_reg + off]))`
        // (`caller-save.c:722-761`).
        if (dst.IsHardReg() && !dst.IsArgPointerReg() && !dst.flags.Any() && dst.mode == src.mode) {
          const int hr = dst.RegNo();
          const int nregs = std::max(1, StackSlotHardRegNregs(hr, dst.mode, gp64, fp64));
          const int mode_bytes = StackSlotModeByteSize(dst.mode);
          const int word_bytes = std::max(1, mode_bytes / nregs);
          const std::optional<int64_t> off = match_cs_mem(src, mode_bytes);
          if (off.has_value() && needs_restore.contains(hr)) {
            const size_t s_idx = needs_restore[hr].save_idx;
            bool ok = true;
            for (int k = 0; k < nregs; ++k) {
              const auto it = needs_restore.find(hr + k);
              if (it == needs_restore.end() || it->second.save_idx != s_idx ||
                  it->second.offset != *off + int64_t{word_bytes} * k) {
                ok = false;
                break;
              }
            }
            if (ok) {
              expected_words[s_idx] = needs_restore[hr].total_words;
              for (int k = 0; k < nregs; ++k) {
                restored_mask[s_idx] |= (uint32_t{1} << needs_restore[hr + k].word_bit);
                needs_restore.erase(hr + k);
              }
              restores_per_save[s_idx].push_back(
                  MatchedCallerSaveRestore{.restore_idx = i, .restore_span = nregs});
              continue;
            }
          }
        }
        // Candidate save before an upcoming call:
        // `(set (mem:M [elim_reg + off]) (reg:M hr))` (`caller-save.c:680-720`).
        if (src.IsHardReg() && !src.IsArgPointerReg() && !src.flags.Any() && dst.mode == src.mode &&
            i < call_indices.back()) {
          const int hr = src.RegNo();
          const int nregs = std::max(1, StackSlotHardRegNregs(hr, src.mode, gp64, fp64));
          const int mode_bytes = StackSlotModeByteSize(src.mode);
          const int word_bytes = std::max(1, mode_bytes / nregs);
          const std::optional<int64_t> off = match_cs_mem(dst, mode_bytes);
          if (off.has_value()) {
            bool all_cand = true;
            std::vector<SaveWord> words;
            std::vector<std::pair<int, StackSlotBound>> bounds;
            for (int k = 0; k < nregs; ++k) {
              if (!IsMipsCallerSavableHardReg(hr + k) ||
                  (restrict_hrs && !allowed_hrs.contains(hr + k))) {
                all_cand = false;
                break;
              }
              const int64_t w_off = *off + int64_t{word_bytes} * k;
              words.push_back(SaveWord{
                  .save_idx = i,
                  .total_words = nregs,
                  .word_bit = k,
                  .word_hr = hr + k,
                  .offset = w_off,
                  .word_bytes = word_bytes,
              });
              bounds.emplace_back(hr + k, StackSlotBound{.offset = w_off, .byte_size = word_bytes});
            }
            if (all_cand) {
              save_spans[i] = nregs;
              bounds_per_save[i] = std::move(bounds);
              pending_saves.emplace_back(i, std::move(words));
              continue;
            }
          }
        }
      }

      // `caller-save.c:517-542` (`set_reg_live`): any non-save/non-restore
      // write invalidates saved/pending caller-save state for that hard reg.
      std::array<bool, kFirstPseudoRegno> written{};
      CollectWrittenHardRegWords(pn.pattern, gp64, fp64, &written);
      for (int w = 0; w < kFirstPseudoRegno; ++w) {
        if (written[w]) {
          saved_in_slot.erase(w);
          needs_restore.erase(w);
          pending_saves.erase(
              std::remove_if(pending_saves.begin(), pending_saves.end(),
                             [w](const auto& entry) {
                               return std::any_of(
                                   entry.second.begin(), entry.second.end(),
                                   [w](const SaveWord& sw) { return sw.word_hr == w; });
                             }),
              pending_saves.end());
        }
      }
    }

    // Deterministic ascending `save_idx` iteration via `absl::btree_map`.
    for (const auto& [s_idx, mask] : restored_mask) {
      const int exp_w = expected_words[s_idx];
      const uint32_t full = (exp_w >= 32) ? ~uint32_t{0} : ((uint32_t{1} << exp_w) - 1);
      if (mask != full) {
        continue;
      }
      for (const auto& [hr, bound] : bounds_per_save[s_idx]) {
        auto [it, inserted] = out_hr_bounds->try_emplace(hr, bound);
        if (!inserted && it->second != bound) {
          return false;
        }
      }
      (*out_blocks)[b].push_back(MatchedCallerSaveGroup{
          .save_idx = s_idx,
          .save_span = save_spans[s_idx],
          .restores = std::move(restores_per_save[s_idx]),
          .word_bounds = std::move(bounds_per_save[s_idx]),
      });
    }
  }
  return true;
}

std::vector<std::vector<bool>> BuildCallerSaveInsnMask(
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const std::vector<std::vector<MatchedCallerSaveGroup>>& matches) {
  const size_t num_blocks = parsed_blocks.size();
  std::vector<std::vector<bool>> is_cs(num_blocks);
  for (size_t b = 0; b < num_blocks; ++b) {
    is_cs[b].assign(parsed_blocks[b].size(), false);
    for (const MatchedCallerSaveGroup& ms : matches[b]) {
      is_cs[b][ms.save_idx] = true;
      for (const MatchedCallerSaveRestore& r : ms.restores) {
        is_cs[b][r.restore_idx] = true;
      }
    }
  }
  return is_cs;
}

}  // namespace

int StackSlotModeByteSize(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kQi:
      return 1;
    case RtlMachineMode::kHi:
      return 2;
    case RtlMachineMode::kSi:
    case RtlMachineMode::kSf:
    case RtlMachineMode::kCc:
      return 4;
    case RtlMachineMode::kDi:
    case RtlMachineMode::kDf:
      return 8;
    case RtlMachineMode::kTi:
    case RtlMachineMode::kTf:
      return 16;
    default:
      return 4;
  }
}

int StackSlotHardRegNregs(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  const int bytes = StackSlotModeByteSize(mode);
  const bool is_fpr = (regno >= 32 && regno <= 63);
  const int unit = is_fpr ? (fp64 ? 8 : 4) : (gp64 ? 8 : 4);
  return (bytes + unit - 1) / unit;
}

bool IsMipsCallerSavableHardReg(int hr) {
  return (hr >= 2 && hr <= 15) || hr == 24 || hr == 25 || (hr >= 32 && hr <= 51);
}

std::optional<int64_t> ExtractStackBaseOffset(const RtlExpr& addr, int base_regno) {
  if (addr.op == RtlOp::kReg && !addr.IsArgPointerReg() && addr.RegNo() == base_regno) {
    return 0;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      !addr.children[0].IsArgPointerReg() && addr.children[0].RegNo() == base_regno &&
      addr.children[1].op == RtlOp::kConstInt) {
    return addr.children[1].int_val;
  }
  return std::nullopt;
}

int64_t AlignDownToSpillBucket(int64_t offset, int64_t frame_base) {
  if (offset <= frame_base) {
    return frame_base;
  }
  return frame_base + ((offset - frame_base) & ~(kMipsSpillSlotBucketBytes - 1));
}

bool ExprOverlapsStackInterval(const RtlExpr& root, int base_regno, int64_t interval_lo,
                               int64_t interval_hi) {
  bool overlaps = false;
  auto walk = [&](const auto& self, const RtlExpr& expr, bool in_mem_addr) -> void {
    if (overlaps) {
      return;
    }
    if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
      const std::optional<int64_t> off = ExtractStackBaseOffset(expr.children[0], base_regno);
      if (off.has_value()) {
        const int sz = std::max(1, StackSlotModeByteSize(expr.mode));
        if (*off < interval_hi && *off + sz > interval_lo) {
          overlaps = true;
        }
        return;
      }
      self(self, expr.children[0], /*in_mem_addr=*/true);
      return;
    }
    if (!in_mem_addr) {
      const std::optional<int64_t> off = ExtractStackBaseOffset(expr, base_regno);
      if (off.has_value() && *off >= interval_lo && *off < interval_hi) {
        overlaps = true;
        return;
      }
    }
    for (const RtlExpr& ch : expr.children) {
      self(self, ch, in_mem_addr);
    }
  };
  walk(walk, root, /*in_mem_addr=*/false);
  return overlaps;
}

bool VerifyMonotonicCallerSaveLayout(const absl::btree_map<int, StackSlotBound>& hr_bounds) {
  if (hr_bounds.empty()) {
    return true;
  }
  auto prev = hr_bounds.begin();
  for (auto it = std::next(prev); it != hr_bounds.end(); ++it) {
    if (it->second.offset < prev->second.offset + prev->second.byte_size) {
      return false;
    }
    prev = it;
  }
  return true;
}

StackFrameSlotKind ClassifyStackFrameOffset(
    int64_t offset, int outgoing_args_size, int frame_size_bytes,
    const absl::btree_map<int, StackSlotBound>& caller_save_bounds) {
  if (offset < 0 || outgoing_args_size < 0 || frame_size_bytes < 0) {
    return StackFrameSlotKind::kUnknown;
  }
  if (offset < outgoing_args_size) {
    return StackFrameSlotKind::kOutgoingArg;
  }
  const int64_t frame_top = int64_t{outgoing_args_size} + frame_size_bytes;
  if (offset >= frame_top) {
    return StackFrameSlotKind::kUnknown;
  }
  for (const auto& [_, bound] : caller_save_bounds) {
    if (offset >= bound.offset && offset < bound.offset + bound.byte_size) {
      return StackFrameSlotKind::kCallerSave;
    }
  }
  return StackFrameSlotKind::kSpillOrLocal;
}

CallerSaveCfgClassification ClassifyCallerSavePairsAcrossCfg(
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const CallerSaveMatchOptions& options) {
  CallerSaveCfgClassification result;
  const size_t num_blocks = parsed_blocks.size();
  result.block_matches.assign(num_blocks, {});
  result.save_span.assign(num_blocks, {});
  result.restore_span.assign(num_blocks, {});
  for (size_t b = 0; b < num_blocks; ++b) {
    result.save_span[b].assign(parsed_blocks[b].size(), 0);
    result.restore_span[b].assign(parsed_blocks[b].size(), 0);
  }

  if (!options.opt_caller_saves_enabled || options.frame_size_bytes <= 0 ||
      options.outgoing_args_size < 0) {
    return result;
  }

  const int elim_reg = options.frame_pointer_needed ? kFramePointerRegno : kStackPointerRegno;
  const int64_t min_off = options.outgoing_args_size;
  const int64_t max_off = min_off + options.frame_size_bytes;

  std::vector<std::vector<MatchedCallerSaveGroup>> block_matches;
  absl::btree_map<int, StackSlotBound> hr_bounds;
  if (!PlanCallerSavePairsForCfg(parsed_blocks, elim_reg, min_off, max_off, options.gp64,
                                 options.fp64, /*restrict_hrs=*/false, {}, &block_matches,
                                 &hr_bounds) ||
      hr_bounds.empty()) {
    return result;
  }

  std::vector<std::vector<bool>> is_cs = BuildCallerSaveInsnMask(parsed_blocks, block_matches);
  absl::btree_set<int> invalid_hrs;
  for (const auto& [hr, bound] : hr_bounds) {
    const int64_t lo = bound.offset;
    const int64_t hi = bound.offset + bound.byte_size;
    for (size_t b = 0; b < num_blocks && !invalid_hrs.contains(hr); ++b) {
      for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
        for (const RtlRegNote& note : parsed_blocks[b][i].reg_notes) {
          if (note.list_op == RtlOp::kExprList &&
              ExprOverlapsStackInterval(note.datum, elim_reg, lo, hi)) {
            invalid_hrs.insert(hr);
            break;
          }
        }
        if (invalid_hrs.contains(hr) || is_cs[b][i]) {
          continue;
        }
        if (ExprOverlapsStackInterval(parsed_blocks[b][i].pattern, elim_reg, lo, hi)) {
          invalid_hrs.insert(hr);
          break;
        }
      }
    }
  }

  if (!invalid_hrs.empty()) {
    if (invalid_hrs.size() == hr_bounds.size()) {
      return result;
    }
    absl::btree_set<int> allowed_hrs;
    for (const auto& [hr, _] : hr_bounds) {
      if (!invalid_hrs.contains(hr)) {
        allowed_hrs.insert(hr);
      }
    }
    if (!PlanCallerSavePairsForCfg(parsed_blocks, elim_reg, min_off, max_off, options.gp64,
                                   options.fp64, /*restrict_hrs=*/true, allowed_hrs, &block_matches,
                                   &hr_bounds) ||
        hr_bounds.empty()) {
      return result;
    }
    is_cs = BuildCallerSaveInsnMask(parsed_blocks, block_matches);
  }

  if (!VerifyMonotonicCallerSaveLayout(hr_bounds)) {
    return result;
  }

  const int64_t min_cs_off = hr_bounds.begin()->second.offset;
  const int64_t aligned_cs_base = AlignDownToSpillBucket(min_cs_off, min_off);
  if (aligned_cs_base < min_cs_off) {
    for (size_t b = 0; b < num_blocks; ++b) {
      for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
        for (const RtlRegNote& note : parsed_blocks[b][i].reg_notes) {
          if (note.list_op == RtlOp::kExprList &&
              ExprOverlapsStackInterval(note.datum, elim_reg, aligned_cs_base, min_cs_off)) {
            return result;
          }
        }
        if (is_cs[b][i]) {
          continue;
        }
        if (ExprOverlapsStackInterval(parsed_blocks[b][i].pattern, elim_reg, aligned_cs_base,
                                      min_cs_off)) {
          return result;
        }
      }
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    for (const MatchedCallerSaveGroup& ms : block_matches[b]) {
      result.save_span[b][ms.save_idx] = ms.save_span;
      for (const MatchedCallerSaveRestore& r : ms.restores) {
        result.restore_span[b][r.restore_idx] = r.restore_span;
      }
    }
  }
  result.block_matches = std::move(block_matches);
  result.hr_slot_bounds = std::move(hr_bounds);
  result.aligned_caller_save_base = aligned_cs_base;
  return result;
}

}  // namespace rom_nom_nom::lifter
