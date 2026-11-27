#include "lifter/reconstruct_o0_final_scan.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_cfg_utils.h"
#include "lifter/stupid_alloc_model.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;
constexpr int kStackPointerRegnum = 29;
constexpr int kFramePointerRegnum = 30;

bool IsControlTransferPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
    return IsControlTransferPattern(pat.children[0]);
  }
  return false;
}

int ModeByteSize(RtlMachineMode mode) {
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
    case RtlMachineMode::kTi:
    case RtlMachineMode::kTf:
      return 16;
    default:
      return 4;
  }
}

bool IsO0PlainArgLoadSrc(const RtlExpr& src) {
  if (src.op == RtlOp::kReg || src.op == RtlOp::kSubreg || src.op == RtlOp::kMem ||
      src.op == RtlOp::kConstInt || src.op == RtlOp::kConstDouble || src.op == RtlOp::kSymbolRef ||
      src.op == RtlOp::kLabelRef || src.op == RtlOp::kConst) {
    return true;
  }
  if (src.op == RtlOp::kPlus && src.children.size() == 2 && src.children[0].op == RtlOp::kReg &&
      (src.children[0].IsArgPointerReg() || src.children[0].RegNo() == kFramePointerRegnum ||
       src.children[0].RegNo() == kStackPointerRegnum) &&
      src.children[1].op == RtlOp::kConstInt) {
    return true;
  }
  return false;
}

std::optional<int64_t> MatchRegPlusConstOffset(const RtlExpr& addr, int base_reg) {
  return MatchO0RegPlusConstOffset(addr, base_reg);
}

// Identifies the `-O0` `mips_expand_prologue` instruction prefix `[0 .. prologue_end)`
// in block 0 and any valid top-of-frame 8-byte `alter_reg(..., -1)` `$fp` spill slots.
O0PrologueInfo AnalyzeStage0O0PrologueAndTopSpills(
    const RtlControlFlowGraph& cfg, const std::vector<std::vector<RtlPatternAndNotes>>& all_blocks,
    bool gp64, bool fp64) {
  O0PrologueInfo info;
  info.frame_pointer_needed = cfg.sideband.frame_pointer_needed;
  info.outgoing_args_bytes = std::max(0, cfg.sideband.outgoing_args_size);
  if (all_blocks.empty()) {
    return info;
  }
  const auto& b0 = all_blocks[0];
  size_t sp_dec_idx = b0.size();
  int tsize = 0;
  int const12_val = 0;
  bool has_const12 = false;
  for (size_t i = 0; i < b0.size(); ++i) {
    const RtlExpr& pat = b0[i].pattern;
    if (IsCallPattern(pat) || IsControlTransferPattern(pat)) {
      break;
    }
    if (pat.IsSet() && pat.SetDest().IsHardReg() && pat.SetDest().RegNo() == 12) {
      if (pat.SetSrc().op == RtlOp::kConstInt) {
        const12_val = static_cast<int>(pat.SetSrc().int_val);
        has_const12 = true;
      } else if (pat.SetSrc().op == RtlOp::kIor && pat.SetSrc().children.size() == 2 &&
                 pat.SetSrc().children[0].IsHardReg() && pat.SetSrc().children[0].RegNo() == 12 &&
                 pat.SetSrc().children[1].op == RtlOp::kConstInt && has_const12) {
        const12_val |= static_cast<int>(pat.SetSrc().children[1].int_val);
      }
      continue;
    }
    if (pat.IsSet() && pat.SetDest().IsHardReg() && pat.SetDest().RegNo() == kStackPointerRegnum) {
      const RtlExpr& src = pat.SetSrc();
      if (src.op == RtlOp::kPlus && src.children.size() == 2 && src.children[0].IsHardReg() &&
          src.children[0].RegNo() == kStackPointerRegnum &&
          src.children[1].op == RtlOp::kConstInt && src.children[1].int_val < 0) {
        sp_dec_idx = i;
        tsize = static_cast<int>(-src.children[1].int_val);
      } else if (src.op == RtlOp::kMinus && src.children.size() == 2 &&
                 src.children[0].IsHardReg() && src.children[0].RegNo() == kStackPointerRegnum &&
                 src.children[1].IsHardReg() && src.children[1].RegNo() == 12 && has_const12) {
        sp_dec_idx = i;
        tsize = const12_val;
      }
      break;
    }
  }

  int min_save_offset = tsize;
  bool saw_save_store = false;
  if (sp_dec_idx < b0.size()) {
    info.prologue_end = sp_dec_idx + 1;
    std::optional<int> base13_off;
    int const13_val = 0;
    bool has_const13 = false;
    for (size_t i = sp_dec_idx + 1; i < b0.size(); ++i) {
      const RtlExpr& pat = b0[i].pattern;
      if (IsCallPattern(pat) || IsControlTransferPattern(pat)) {
        break;
      }
      if (pat.IsSet() && pat.SetDest().IsHardReg() && pat.SetDest().RegNo() == 13) {
        if (pat.SetSrc().op == RtlOp::kConstInt) {
          const13_val = static_cast<int>(pat.SetSrc().int_val);
          has_const13 = true;
          info.prologue_end = i + 1;
          continue;
        }
        if (pat.SetSrc().op == RtlOp::kIor && pat.SetSrc().children.size() == 2 &&
            pat.SetSrc().children[0].IsHardReg() && pat.SetSrc().children[0].RegNo() == 13 &&
            pat.SetSrc().children[1].op == RtlOp::kConstInt && has_const13) {
          const13_val |= static_cast<int>(pat.SetSrc().children[1].int_val);
          info.prologue_end = i + 1;
          continue;
        }
        if (pat.SetSrc().op == RtlOp::kPlus && pat.SetSrc().children.size() == 2) {
          const RtlExpr& a = pat.SetSrc().children[0];
          const RtlExpr& b = pat.SetSrc().children[1];
          if ((a.IsHardReg() && a.RegNo() == 12 && b.IsHardReg() &&
               b.RegNo() == kStackPointerRegnum) ||
              (b.IsHardReg() && b.RegNo() == 12 && a.IsHardReg() &&
               a.RegNo() == kStackPointerRegnum)) {
            base13_off = tsize;
            info.prologue_end = i + 1;
            continue;
          }
          if ((a.IsHardReg() && a.RegNo() == 13 && b.IsHardReg() &&
               b.RegNo() == kStackPointerRegnum && has_const13) ||
              (b.IsHardReg() && b.RegNo() == 13 && a.IsHardReg() &&
               a.RegNo() == kStackPointerRegnum && has_const13)) {
            base13_off = const13_val;
            info.prologue_end = i + 1;
            continue;
          }
        }
      }
      if (pat.IsSet() && pat.SetDest().IsMem() && !pat.SetDest().children.empty() &&
          pat.SetSrc().IsHardReg()) {
        const int sr = pat.SetSrc().RegNo();
        const bool is_save_reg =
            (sr >= 16 && sr <= 23) || sr == 30 || sr == 31 || (sr >= 52 && sr <= 63);
        if (is_save_reg) {
          std::optional<int64_t> sp_off =
              MatchRegPlusConstOffset(pat.SetDest().children[0], kStackPointerRegnum);
          if (!sp_off.has_value() && base13_off.has_value()) {
            if (auto b13 = MatchRegPlusConstOffset(pat.SetDest().children[0], 13);
                b13.has_value()) {
              sp_off = *base13_off + *b13;
            }
          }
          if (sp_off.has_value() && *sp_off >= 0 && *sp_off < tsize) {
            saw_save_store = true;
            min_save_offset = std::min(min_save_offset, static_cast<int>(*sp_off));
            info.prologue_end = i + 1;
            continue;
          }
        }
      }
      if (pat.IsSet() && pat.SetDest().IsHardReg() &&
          pat.SetDest().RegNo() == kFramePointerRegnum) {
        info.frame_pointer_needed = true;
        info.prologue_end = i + 1;
        break;
      }
      break;
    }
  } else {
    for (size_t i = 0; i < b0.size(); ++i) {
      const RtlExpr& pat = b0[i].pattern;
      if (!pat.IsSet()) {
        break;
      }
      if (pat.SetDest().IsHardReg() && pat.SetDest().RegNo() >= 4 && pat.SetDest().RegNo() <= 7 &&
          (pat.SetSrc().op == RtlOp::kAshiftrt || pat.SetSrc().op == RtlOp::kLshiftrt) &&
          pat.SetSrc().children.size() == 2 && pat.SetSrc().children[0].IsHardReg() &&
          pat.SetSrc().children[0].RegNo() == pat.SetDest().RegNo() &&
          pat.SetSrc().children[1].op == RtlOp::kConstInt &&
          (pat.SetSrc().children[1].int_val == 8 || pat.SetSrc().children[1].int_val == 16 ||
           pat.SetSrc().children[1].int_val == 24)) {
        info.prologue_end = i + 1;
        continue;
      }
      if (pat.SetDest().IsMem() && !pat.SetDest().children.empty() && pat.SetSrc().IsHardReg() &&
          pat.SetSrc().RegNo() >= 4 && pat.SetSrc().RegNo() <= 7) {
        if (auto off = MatchRegPlusConstOffset(pat.SetDest().children[0], kStackPointerRegnum);
            off.has_value() && *off == (pat.SetSrc().RegNo() - 4) * (gp64 ? 8 : 4)) {
          info.prologue_end = i + 1;
          continue;
        }
      }
      break;
    }
  }

  if (cfg.sideband.frame_size_bytes > 0) {
    info.local_frame_bytes = cfg.sideband.frame_size_bytes;
  } else if (tsize > 0) {
    const int top_of_locals = saw_save_store ? (min_save_offset & ~7) : tsize;
    info.local_frame_bytes = std::max(0, top_of_locals - info.outgoing_args_bytes);
  }

  if (!info.frame_pointer_needed || info.local_frame_bytes < 8 ||
      (info.local_frame_bytes % 8) != 0) {
    return info;
  }

  struct SlotCheck {
    RtlMachineMode mode = RtlMachineMode::kNone;
    int refs = 0;
    bool valid = true;
    bool has_entry_arg_store = false;
  };
  absl::flat_hash_map<int, SlotCheck> slots;
  absl::flat_hash_set<int> bad_slots;
  absl::flat_hash_set<int> used_hrs;
  for (size_t b = 0; b < all_blocks.size(); ++b) {
    const size_t start_i = (b == 0) ? info.prologue_end : 0;
    std::array<bool, kFirstPseudoRegister> b0_written{};
    for (size_t i = start_i; i < all_blocks[b].size(); ++i) {
      const RtlExpr& pat = all_blocks[b][i].pattern;
      pat.Visit([&](const RtlExpr& n) {
        if (n.IsHardReg()) {
          const int r = n.RegNo();
          const int nw = std::max(1, MipsHardRegNregs(r, n.mode, gp64, fp64));
          for (int w = 0; w < nw && r + w < kFirstPseudoRegister; ++w) {
            used_hrs.insert(r + w);
          }
        }
        if (n.IsMem() && !n.children.empty()) {
          if (auto off = MatchRegPlusConstOffset(n.children[0], kFramePointerRegnum);
              off.has_value()) {
            const int rel = static_cast<int>(*off) - info.outgoing_args_bytes;
            if (rel >= 0 && rel < info.local_frame_bytes) {
              const int base = (rel / 8) * 8;
              const int msz = ModeByteSize(n.mode);
              if (n.flags.volatil || n.flags.in_struct || msz <= 0 || msz > 8 ||
                  rel != base + (8 - msz)) {
                bad_slots.insert(base);
              } else {
                SlotCheck& sc = slots[base];
                if (sc.mode == RtlMachineMode::kNone) {
                  sc.mode = n.mode;
                } else if (sc.mode != n.mode) {
                  sc.valid = false;
                }
                ++sc.refs;
              }
            }
          }
        }
      });
      if (b == 0 && pat.IsSet()) {
        if (pat.SetDest().IsMem() && !pat.SetDest().children.empty() && pat.SetSrc().IsHardReg() &&
            IsO0OutgoingCallArgHardReg(pat.SetSrc().RegNo()) && !b0_written[pat.SetSrc().RegNo()]) {
          if (auto off = MatchRegPlusConstOffset(pat.SetDest().children[0], kFramePointerRegnum);
              off.has_value()) {
            const int rel = static_cast<int>(*off) - info.outgoing_args_bytes;
            if (rel >= 0 && rel < info.local_frame_bytes) {
              slots[(rel / 8) * 8].has_entry_arg_store = true;
            }
          }
        }
        if (pat.SetDest().IsHardReg()) {
          const int d = pat.SetDest().RegNo();
          const int nw = std::max(1, MipsHardRegNregs(d, pat.SetDest().mode, gp64, fp64));
          for (int w = 0; w < nw && d + w < kFirstPseudoRegister; ++w) {
            b0_written[d + w] = true;
          }
        }
      }
    }
  }
  bool all_saved_gprs = true;
  for (int hr = 16; hr <= 23; ++hr) {
    if (!used_hrs.contains(hr)) {
      all_saved_gprs = false;
      break;
    }
  }
  bool all_saved_fprs = true;
  for (int hr = 52; hr <= 62; hr += 2) {
    if (!used_hrs.contains(hr)) {
      all_saved_fprs = false;
      break;
    }
  }
  for (int base = info.local_frame_bytes - 8; base >= 0; base -= 8) {
    if (bad_slots.contains(base)) {
      break;
    }
    auto it = slots.find(base);
    if (it == slots.end() || !it->second.valid || it->second.refs <= 0) {
      break;
    }
    if (it->second.mode == RtlMachineMode::kDf && !all_saved_fprs &&
        !it->second.has_entry_arg_store) {
      break;
    }
    if (it->second.mode == RtlMachineMode::kDi && !all_saved_gprs &&
        !it->second.has_entry_arg_store) {
      break;
    }
    info.valid_top_spill_slot_bases.insert(base);
  }
  return info;
}

void ReconstructO0NoConflictAndMixedDiSubregs(
    size_t prologue_end, std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  auto expr_mentions_hard_pair = [](const RtlExpr& expr, int hr) {
    bool found = false;
    expr.Visit([&](const RtlExpr& n) {
      if (n.IsHardReg() && (n.RegNo() == hr || n.RegNo() == hr + 1)) {
        found = true;
      }
    });
    return found;
  };

  for (size_t b = 0; b < all_blocks->size(); ++b) {
    auto& blk = (*all_blocks)[b];
    const size_t start_i = (b == 0) ? prologue_end : 0;
    for (size_t i = start_i; i + 2 < blk.size(); ++i) {
      const RtlExpr& p0 = blk[i].pattern;
      const RtlExpr& p1 = blk[i + 1].pattern;
      if (!p0.IsSet() || !p1.IsSet() || IsCallPattern(p0) || IsCallPattern(p1) ||
          !p0.SetDest().IsHardReg() || !p1.SetDest().IsHardReg() || p0.SetDest().flags.integrated ||
          p1.SetDest().flags.integrated || p0.SetDest().mode != RtlMachineMode::kSi ||
          p1.SetDest().mode != RtlMachineMode::kSi) {
        continue;
      }
      const int hr = std::min(p0.SetDest().RegNo(), p1.SetDest().RegNo());
      const int hr_hi = std::max(p0.SetDest().RegNo(), p1.SetDest().RegNo());
      if (hr < 2 || hr > 24 || (hr % 2) != 0 || hr_hi != hr + 1 ||
          expr_mentions_hard_pair(p0.SetSrc(), hr) || expr_mentions_hard_pair(p1.SetSrc(), hr)) {
        continue;
      }
      size_t end_idx = blk.size();
      for (size_t j = i + 2; j < blk.size(); ++j) {
        const RtlExpr& jpat = blk[j].pattern;
        if (IsCallPattern(jpat)) {
          break;
        }
        if (!expr_mentions_hard_pair(jpat, hr)) {
          continue;
        }
        const RtlExpr* jset =
            jpat.IsSet() ? &jpat
            : (jpat.op == RtlOp::kParallel && !jpat.children.empty() && jpat.children[0].IsSet())
                ? &jpat.children[0]
                : nullptr;
        bool valid_di_read = false;
        bool bad_ref = false;
        auto check_read_sub = [&](const RtlExpr& sub) {
          sub.Visit([&](const RtlExpr& n) {
            if (n.IsHardReg() && (n.RegNo() == hr || n.RegNo() == hr + 1)) {
              if (n.RegNo() == hr && n.mode == RtlMachineMode::kDi && !n.flags.integrated) {
                valid_di_read = true;
              } else {
                bad_ref = true;
              }
            }
          });
        };
        if (jset != nullptr) {
          check_read_sub(jset->SetSrc());
          if (!jset->SetDest().IsHardReg()) {
            check_read_sub(jset->SetDest());
          } else if (expr_mentions_hard_pair(jset->SetDest(), hr) &&
                     (jset->SetDest().RegNo() != hr ||
                      jset->SetDest().mode != RtlMachineMode::kDi)) {
            bad_ref = true;
          }
          if (jpat.op == RtlOp::kParallel) {
            for (size_t c = 1; c < jpat.children.size(); ++c) {
              if (expr_mentions_hard_pair(jpat.children[c], hr)) {
                bad_ref = true;
              }
            }
          }
        }
        if (valid_di_read && !bad_ref) {
          end_idx = j;
        }
        break;
      }
      if (end_idx >= blk.size()) {
        continue;
      }
      // If `end_idx` itself overwrites `(reg:DI hr)` (e.g. a `plus:DI` /
      // `minus:DI` / `ashift:DI` RMW), the old `no_conflict` value is dead
      // after `end_idx` regardless of subsequent uses of `hr`.
      const RtlExpr& end_pat = blk[end_idx].pattern;
      const RtlExpr* end_set = end_pat.IsSet() ? &end_pat
                               : (end_pat.op == RtlOp::kParallel && !end_pat.children.empty() &&
                                  end_pat.children[0].IsSet())
                                   ? &end_pat.children[0]
                                   : nullptr;
      const bool end_overwrites_di = end_set != nullptr && end_set->SetDest().IsHardReg() &&
                                     end_set->SetDest().RegNo() == hr &&
                                     end_set->SetDest().mode == RtlMachineMode::kDi;
      bool used_after = false;
      bool killed_lo = end_overwrites_di;
      bool killed_hi = end_overwrites_di;
      for (size_t j = end_idx + 1; j < blk.size() && !(killed_lo && killed_hi); ++j) {
        const RtlExpr& jpat = blk[j].pattern;
        if (IsCallPattern(jpat)) {
          for (const RtlExpr& fu : blk[j].call_fusage) {
            fu.Visit([&](const RtlExpr& n) {
              if (!n.IsHardReg()) {
                return;
              }
              const int r = n.RegNo();
              const int nw =
                  (n.mode == RtlMachineMode::kDi || n.mode == RtlMachineMode::kDf) ? 2 : 1;
              if (!killed_lo && r <= hr && hr < r + nw) {
                used_after = true;
              }
              if (!killed_hi && r <= hr + 1 && hr + 1 < r + nw) {
                used_after = true;
              }
            });
          }
          break;
        }
        auto check_reads = [&](const RtlExpr& sub) {
          sub.Visit([&](const RtlExpr& n) {
            if (!n.IsHardReg()) {
              return;
            }
            const int r = n.RegNo();
            const int nw = (n.mode == RtlMachineMode::kDi || n.mode == RtlMachineMode::kDf) ? 2 : 1;
            if (!killed_lo && r <= hr && hr < r + nw) {
              used_after = true;
            }
            if (!killed_hi && r <= hr + 1 && hr + 1 < r + nw) {
              used_after = true;
            }
          });
        };
        auto check_elem = [&](const RtlExpr& elem) {
          if (elem.IsSet()) {
            check_reads(elem.SetSrc());
            if (!elem.SetDest().IsHardReg()) {
              check_reads(elem.SetDest());
            }
          } else if (elem.op != RtlOp::kClobber) {
            check_reads(elem);
          }
        };
        auto kill_elem = [&](const RtlExpr& elem) {
          const RtlExpr* dst = nullptr;
          if (elem.IsSet()) {
            dst = &elem.SetDest();
          } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
            dst = &elem.children[0];
          }
          if (dst != nullptr && dst->IsHardReg()) {
            const int r = dst->RegNo();
            const int nw =
                (dst->mode == RtlMachineMode::kDi || dst->mode == RtlMachineMode::kDf) ? 2 : 1;
            if (r <= hr && hr < r + nw) {
              killed_lo = true;
            }
            if (r <= hr + 1 && hr + 1 < r + nw) {
              killed_hi = true;
            }
          }
        };
        if (jpat.op == RtlOp::kParallel) {
          for (const RtlExpr& elem : jpat.children) {
            check_elem(elem);
          }
          if (used_after) {
            break;
          }
          for (const RtlExpr& elem : jpat.children) {
            kill_elem(elem);
          }
        } else {
          check_elem(jpat);
          if (used_after) {
            break;
          }
          kill_elem(jpat);
        }
      }
      if (used_after) {
        continue;
      }
      // `optabs.c:2445-2488` (`emit_no_conflict_block`) always emits all setup
      // insns that do not store into words of `target` before `(clobber target)`,
      // placing `(clobber target)` immediately ahead of the first word store.
      const size_t clob_pos = i;
      RtlPatternAndNotes self_mv;
      self_mv.pattern = RtlExpr::MakeSet(MakeGccHardReg(RtlMachineMode::kDi, hr),
                                         MakeGccHardReg(RtlMachineMode::kDi, hr));
      blk.insert(blk.begin() + static_cast<ptrdiff_t>(i + 2), std::move(self_mv));
      RtlPatternAndNotes clob;
      clob.pattern = RtlExpr::MakeClobber(MakeGccHardReg(RtlMachineMode::kDi, hr));
      blk.insert(blk.begin() + static_cast<ptrdiff_t>(clob_pos), std::move(clob));
      i = end_idx + 2;
    }
  }
}

void StepBackwardHardRegLivenessOnInsn(const RtlPatternAndNotes& pn, bool gp64, bool fp64,
                                       uint64_t* live) {
  const RtlExpr& pat = pn.pattern;
  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kCodeLabel || pat.op == RtlOp::kBarrier ||
      pat.op == RtlOp::kAddrVec || pat.op == RtlOp::kAddrDiffVec) {
    return;
  }
  auto reg_mask = [&](int r, RtlMachineMode m) -> uint64_t {
    if (r < 0 || r >= 64) {
      return 0;
    }
    const int nw = std::max(1, MipsHardRegNregs(r, m, gp64, fp64));
    uint64_t mask = 0;
    for (int w = 0; w < nw && r + w < 64; ++w) {
      mask |= (uint64_t{1} << (r + w));
    }
    return mask;
  };
  if (IsCallPattern(pat)) {
    for (int h = 0; h < 64; ++h) {
      if (IsMipsO0CallUsedHardReg(h)) {
        *live &= ~(uint64_t{1} << h);
      }
    }
    for (const RtlExpr& fu : pn.call_fusage) {
      fu.Visit([&](const RtlExpr& n) {
        if (n.IsHardReg()) {
          *live |= reg_mask(n.RegNo(), n.mode);
        }
      });
    }
    auto mark_call_target_reads = [&](const RtlExpr& e) {
      e.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kCall && !n.children.empty()) {
          n.children[0].Visit([&](const RtlExpr& sub) {
            if (sub.IsHardReg()) {
              *live |= reg_mask(sub.RegNo(), sub.mode);
            }
          });
        }
      });
    };
    mark_call_target_reads(pat);
    return;
  }

  uint64_t defs = 0;
  uint64_t uses = 0;
  auto collect_reads = [&](const auto& self, const RtlExpr& e) -> void {
    if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].IsHardReg()) {
      const int r = e.children[0].RegNo();
      const int w = std::max(0, static_cast<int>(e.int_val));
      uses |= reg_mask(r + w, e.mode);
      return;
    }
    if (e.IsHardReg()) {
      uses |= reg_mask(e.RegNo(), e.mode);
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch);
    }
  };
  auto handle_one = [&](const RtlExpr& elem) {
    if (elem.IsSet()) {
      const RtlExpr& dst = elem.SetDest();
      if (dst.op == RtlOp::kSubreg && !dst.children.empty() && dst.children[0].IsHardReg()) {
        const int r = dst.children[0].RegNo();
        const int w = std::max(0, static_cast<int>(dst.int_val));
        defs |= reg_mask(r + w, dst.mode);
      } else if (dst.IsHardReg()) {
        defs |= reg_mask(dst.RegNo(), dst.mode);
      } else {
        collect_reads(collect_reads, dst);
      }
      collect_reads(collect_reads, elem.SetSrc());
    } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
      if (elem.children[0].IsHardReg()) {
        defs |= reg_mask(elem.children[0].RegNo(), elem.children[0].mode);
      } else {
        collect_reads(collect_reads, elem.children[0]);
      }
    } else {
      collect_reads(collect_reads, elem);
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      handle_one(ch);
    }
  } else {
    handle_one(pat);
  }
  *live = (*live & ~defs) | uses;
}

void ReconstructO0TablejumpIndexSelfMoves(
    const RtlControlFlowGraph& cfg, size_t prologue_end, bool gp64, bool fp64,
    std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  const size_t n_blocks = all_blocks->size();
  std::vector<uint64_t> live_in(n_blocks, 0);
  std::vector<uint64_t> live_out(n_blocks, 0);
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = n_blocks; b-- > 0;) {
      uint64_t out_b = 0;
      for (size_t succ : cfg.rpo_blocks[b].successors) {
        if (succ < n_blocks) {
          out_b |= live_in[succ];
        }
      }
      for (size_t succ : cfg.rpo_blocks[b].addr_vec_targets) {
        if (succ < n_blocks) {
          out_b |= live_in[succ];
        }
      }
      uint64_t cur = out_b;
      for (size_t i = (*all_blocks)[b].size(); i-- > 0;) {
        StepBackwardHardRegLivenessOnInsn((*all_blocks)[b][i], gp64, fp64, &cur);
      }
      if (out_b != live_out[b] || cur != live_in[b]) {
        live_out[b] = out_b;
        live_in[b] = cur;
        changed = true;
      }
    }
  }

  for (size_t b = 0; b < n_blocks; ++b) {
    auto& blk = (*all_blocks)[b];
    if (blk.empty()) {
      continue;
    }
    const RtlExpr& last_pat = blk.back().pattern;
    const bool is_tablejump = last_pat.op == RtlOp::kParallel && last_pat.children.size() == 2 &&
                              last_pat.children[0].IsSet() &&
                              last_pat.children[0].SetDest().op == RtlOp::kPc &&
                              last_pat.children[1].op == RtlOp::kUse;
    if (!is_tablejump) {
      continue;
    }
    const size_t start_i = (b == 0) ? prologue_end : 0;
    for (size_t i = start_i; i + 1 < blk.size(); ++i) {
      const RtlExpr& pat = blk[i].pattern;
      if (!pat.IsSet() || !pat.SetDest().IsHardReg() || pat.SetDest().mode != RtlMachineMode::kSi ||
          pat.SetSrc().op != RtlOp::kAshift || pat.SetSrc().children.size() != 2 ||
          !pat.SetSrc().children[0].IsHardReg() ||
          pat.SetSrc().children[1].op != RtlOp::kConstInt ||
          pat.SetSrc().children[1].int_val != 2) {
        continue;
      }
      const int hr_shl = pat.SetDest().RegNo();
      const int hr_idx = pat.SetSrc().children[0].RegNo();
      if (hr_shl < 2 || hr_shl >= 32 || hr_idx < 2 || hr_idx >= 32 || hr_shl == hr_idx) {
        break;
      }
      if (i > start_i && blk[i - 1].pattern.IsSet() && blk[i - 1].pattern.SetDest().IsHardReg() &&
          blk[i - 1].pattern.SetDest().RegNo() == hr_idx &&
          blk[i - 1].pattern.SetSrc().IsHardReg() &&
          !IsO0OutgoingCallArgHardReg(blk[i - 1].pattern.SetSrc().RegNo())) {
        break;
      }
      uint64_t live_after_i = live_out[b];
      for (size_t j = blk.size(); j-- > i + 1;) {
        StepBackwardHardRegLivenessOnInsn(blk[j], gp64, fp64, &live_after_i);
      }
      if ((live_after_i & (uint64_t{1} << hr_idx)) != 0) {
        break;
      }
      RtlPatternAndNotes copy_insn;
      copy_insn.pattern = RtlExpr::MakeSet(MakeGccHardReg(RtlMachineMode::kSi, hr_idx),
                                           MakeGccHardReg(RtlMachineMode::kSi, hr_idx));
      blk.insert(blk.begin() + static_cast<ptrdiff_t>(i), std::move(copy_insn));
      break;
    }
  }
}

void ReconstructO0PrecomputedCallArgSelfMoves(
    size_t prologue_end, bool gp64, bool fp64,
    std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  auto is_dead_addr_scratch_in_window = [&](const std::vector<RtlPatternAndNotes>& blk, size_t idx,
                                            size_t c) {
    const RtlExpr& pat = blk[idx].pattern;
    if (!pat.IsSet() || !pat.SetDest().IsHardReg() || pat.SetDest().mode != RtlMachineMode::kSi) {
      return false;
    }
    const int scr = pat.SetDest().RegNo();
    const bool is_scratch_gpr =
        (scr >= 1 && scr <= 3) || (scr >= 8 && scr <= 15) || scr == 24 || scr == 25;
    if (!is_scratch_gpr) {
      return false;
    }
    bool read_in_mem = false;
    bool non_mem_ref = false;
    for (size_t k = idx + 1; k < c; ++k) {
      const RtlExpr& kpat = blk[k].pattern;
      if (kpat.IsSet() && kpat.SetDest().IsHardReg() && kpat.SetDest().RegNo() == scr) {
        if (kpat.SetSrc().ContainsReg(scr)) {
          read_in_mem = true;
        }
        continue;
      }
      auto scan = [&](const auto& self, const RtlExpr& e, bool inside_mem) -> void {
        if (e.IsMem()) {
          for (const RtlExpr& ch : e.children) {
            self(self, ch, true);
          }
          return;
        }
        if (e.IsHardReg() && e.RegNo() == scr) {
          if (inside_mem) {
            read_in_mem = true;
          } else {
            non_mem_ref = true;
          }
          return;
        }
        for (const RtlExpr& ch : e.children) {
          self(self, ch, inside_mem);
        }
      };
      scan(scan, kpat, false);
    }
    return read_in_mem && !non_mem_ref;
  };

  for (size_t b = 0; b < all_blocks->size(); ++b) {
    auto& blk = (*all_blocks)[b];
    size_t seg_start = (b == 0) ? prologue_end : 0;
    for (size_t c = seg_start; c < blk.size(); ++c) {
      if (!IsCallPattern(blk[c].pattern)) {
        continue;
      }
      absl::flat_hash_set<int> used_arg_hrs;
      for (const RtlExpr& fu : blk[c].call_fusage) {
        if (fu.op == RtlOp::kUse && fu.children.size() == 1 && fu.children[0].IsHardReg()) {
          used_arg_hrs.insert(fu.children[0].RegNo());
        }
      }
      size_t w_start = c;
      while (w_start > seg_start) {
        const RtlExpr& prev = blk[w_start - 1].pattern;
        if (prev.IsSet() && prev.SetDest().IsHardReg() &&
            IsO0OutgoingCallArgHardReg(prev.SetDest().RegNo()) &&
            IsO0PlainArgLoadSrc(prev.SetSrc())) {
          --w_start;
        } else if (is_dead_addr_scratch_in_window(blk, w_start - 1, c)) {
          --w_start;
        } else {
          break;
        }
      }
      struct PendingArgSelfMove {
        int arg_hr = -1;
        RtlMachineMode mode = RtlMachineMode::kSi;
      };
      std::vector<PendingArgSelfMove> pending;
      for (int arg_hr : {4, 5, 6, 7, 44, 46}) {
        if (!used_arg_hrs.contains(arg_hr)) {
          continue;
        }
        size_t last_def = c;
        for (size_t k = seg_start; k < c; ++k) {
          if (blk[k].pattern.IsSet() && blk[k].pattern.SetDest().IsHardReg() &&
              blk[k].pattern.SetDest().RegNo() == arg_hr) {
            last_def = k;
          }
        }
        if (last_def >= c) {
          continue;
        }
        const bool in_load_window = (last_def >= w_start);
        const bool plain_src = IsO0PlainArgLoadSrc(blk[last_def].pattern.SetSrc());
        if (in_load_window && plain_src) {
          continue;
        }
        bool read_between = false;
        const RtlMachineMode m = blk[last_def].pattern.SetDest().mode;
        const int nw = std::max(1, MipsHardRegNregs(arg_hr, m, gp64, fp64));
        for (size_t k = last_def + 1; k < c; ++k) {
          for (int w = arg_hr; w < arg_hr + nw; ++w) {
            if (blk[k].pattern.ContainsReg(w)) {
              read_between = true;
            }
          }
        }
        if (read_between) {
          continue;
        }
        pending.push_back(PendingArgSelfMove{.arg_hr = arg_hr, .mode = m});
      }
      for (const PendingArgSelfMove& pm : pending) {
        size_t ins_pos = c;
        for (size_t k = w_start; k < c; ++k) {
          if (blk[k].pattern.IsSet() && blk[k].pattern.SetDest().IsHardReg() &&
              IsO0OutgoingCallArgHardReg(blk[k].pattern.SetDest().RegNo()) &&
              blk[k].pattern.SetDest().RegNo() > pm.arg_hr) {
            ins_pos = k;
            break;
          }
        }
        RtlPatternAndNotes mv;
        mv.pattern = RtlExpr::MakeSet(MakeGccHardReg(pm.mode, pm.arg_hr),
                                      MakeGccHardReg(pm.mode, pm.arg_hr));
        blk.insert(blk.begin() + static_cast<ptrdiff_t>(ins_pos), std::move(mv));
        ++c;
      }
      seg_start = c + 1;
    }
  }
}

void EnsureTrailingDiSelfMove(std::vector<RtlPatternAndNotes>* blk, size_t copy_idx, int hr,
                              size_t* cur_c) {
  if (copy_idx + 1 < blk->size()) {
    const RtlExpr& nx = (*blk)[copy_idx + 1].pattern;
    if (nx.IsSet() && nx.SetDest().IsHardReg() && nx.SetDest().RegNo() == hr &&
        nx.SetDest().mode == RtlMachineMode::kDi && nx.SetSrc().IsHardReg() &&
        nx.SetSrc().RegNo() == hr && nx.SetSrc().mode == RtlMachineMode::kDi) {
      return;
    }
  }
  RtlPatternAndNotes self_mv;
  self_mv.pattern = RtlExpr::MakeSet(MakeGccHardReg(RtlMachineMode::kDi, hr),
                                     MakeGccHardReg(RtlMachineMode::kDi, hr));
  blk->insert(blk->begin() + static_cast<ptrdiff_t>(copy_idx + 1), std::move(self_mv));
  ++(*cur_c);
}

void ReconstructO0CallReturnSelfMoves(const O0PrologueInfo& pinfo, bool gp64, bool fp64,
                                      std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  for (size_t b = 0; b < all_blocks->size(); ++b) {
    auto& blk = (*all_blocks)[b];
    const size_t start_i = (b == 0) ? pinfo.prologue_end : 0;
    for (size_t c = start_i; c < blk.size(); ++c) {
      if (!IsCallPattern(blk[c].pattern)) {
        continue;
      }
      const RtlExpr* call_set =
          blk[c].pattern.IsSet() ? &blk[c].pattern
          : (blk[c].pattern.op == RtlOp::kParallel && !blk[c].pattern.children.empty() &&
             blk[c].pattern.children[0].IsSet())
              ? &blk[c].pattern.children[0]
              : nullptr;
      if (call_set == nullptr || !call_set->SetDest().IsHardReg()) {
        continue;
      }
      const int ret_hr = call_set->SetDest().RegNo();
      const RtlMachineMode ret_m = call_set->SetDest().mode;
      if (ret_hr != 2 && ret_hr != 32) {
        continue;
      }
      const bool is_fix_di = IsO0FixDiLibcall(*call_set);
      const int nw = std::max(1, MipsHardRegNregs(ret_hr, ret_m, gp64, fp64));

      auto ret_read_after = [&](size_t from_idx) {
        for (size_t k = from_idx; k < blk.size(); ++k) {
          const RtlExpr& kpat = blk[k].pattern;
          if (IsCallPattern(kpat)) {
            for (const RtlExpr& fu : blk[k].call_fusage) {
              for (int w = ret_hr; w < ret_hr + nw; ++w) {
                if (fu.ContainsReg(w)) {
                  return true;
                }
              }
            }
            return false;
          }
          bool read_here = false;
          auto check_expr = [&](const RtlExpr& expr) {
            expr.Visit([&](const RtlExpr& n) {
              if (n.op == RtlOp::kSubreg && !n.children.empty() && n.children[0].IsHardReg()) {
                const int r = n.children[0].RegNo() + std::max(0, static_cast<int>(n.int_val));
                const int r_nw = std::max(1, MipsHardRegNregs(r, n.mode, gp64, fp64));
                if (std::max(r, ret_hr) < std::min(r + r_nw, ret_hr + nw)) {
                  read_here = true;
                }
              } else if (n.IsHardReg()) {
                const int r = n.RegNo();
                const int r_nw = std::max(1, MipsHardRegNregs(r, n.mode, gp64, fp64));
                if (std::max(r, ret_hr) < std::min(r + r_nw, ret_hr + nw)) {
                  read_here = true;
                }
              }
            });
          };
          const RtlExpr* kset =
              kpat.IsSet() ? &kpat
              : (kpat.op == RtlOp::kParallel && !kpat.children.empty() && kpat.children[0].IsSet())
                  ? &kpat.children[0]
                  : nullptr;
          if (kset != nullptr) {
            check_expr(kset->SetSrc());
            if (!kset->SetDest().IsHardReg() &&
                (kset->SetDest().op != RtlOp::kSubreg || kset->SetDest().children.empty() ||
                 !kset->SetDest().children[0].IsHardReg())) {
              check_expr(kset->SetDest());
            }
            if (read_here) {
              return true;
            }
            if (kset->SetDest().IsHardReg()) {
              const int d = kset->SetDest().RegNo();
              const int d_nw = std::max(1, MipsHardRegNregs(d, kset->SetDest().mode, gp64, fp64));
              if (std::max(d, ret_hr) < std::min(d + d_nw, ret_hr + nw)) {
                return false;
              }
            }
          } else {
            check_expr(kpat);
            if (read_here) {
              return true;
            }
          }
        }
        return false;
      };

      if (c + 1 < blk.size() && blk[c + 1].pattern.IsSet() &&
          blk[c + 1].pattern.SetSrc().IsHardReg() &&
          blk[c + 1].pattern.SetSrc().RegNo() == ret_hr &&
          blk[c + 1].pattern.SetSrc().mode == ret_m) {
        const RtlExpr& dst1 = blk[c + 1].pattern.SetDest();
        if (dst1.IsHardReg() && !dst1.flags.integrated && dst1.mode == ret_m) {
          const int dst_hr = dst1.RegNo();
          const bool allocatable = (dst_hr >= 2 && dst_hr <= 25) || (dst_hr >= 32 && dst_hr <= 63);
          if (allocatable && (dst_hr == ret_hr || !ret_read_after(c + 2))) {
            if (is_fix_di) {
              EnsureTrailingDiSelfMove(&blk, c + 1, dst_hr, &c);
            }
            continue;
          }
        }
        if (dst1.IsMem() && !dst1.children.empty() && dst1.mode == ret_m &&
            !pinfo.valid_top_spill_slot_bases.empty()) {
          if (auto off = MatchRegPlusConstOffset(dst1.children[0], kFramePointerRegnum);
              off.has_value()) {
            const int rel = static_cast<int>(*off) - pinfo.outgoing_args_bytes;
            const int msz = ModeByteSize(ret_m);
            const int base = (rel >= 0) ? ((rel / 8) * 8) : -1;
            if (base >= 0 && rel == base + (8 - msz) &&
                pinfo.valid_top_spill_slot_bases.contains(base) && !ret_read_after(c + 2)) {
              continue;
            }
          }
        }
      }

      size_t last_read = c;
      bool mode_mismatch = false;
      for (size_t k = c + 1; k < blk.size(); ++k) {
        const RtlExpr& kpat = blk[k].pattern;
        if (IsCallPattern(kpat)) {
          for (const RtlExpr& fu : blk[k].call_fusage) {
            for (int w = ret_hr; w < ret_hr + nw; ++w) {
              if (fu.ContainsReg(w)) {
                mode_mismatch = true;
              }
            }
          }
          break;
        }
        if (kpat.op == RtlOp::kUse && !kpat.children.empty() && kpat.children[0].flags.integrated) {
          mode_mismatch = true;
          break;
        }
        auto check_reads = [&](const RtlExpr& expr) {
          expr.Visit([&](const RtlExpr& n) {
            if (n.IsHardReg()) {
              const int r = n.RegNo();
              const int r_nw = std::max(1, MipsHardRegNregs(r, n.mode, gp64, fp64));
              if (std::max(r, ret_hr) < std::min(r + r_nw, ret_hr + nw)) {
                if (r == ret_hr && n.mode == ret_m && !n.flags.integrated) {
                  last_read = k;
                } else {
                  mode_mismatch = true;
                }
              }
            }
          });
        };
        const RtlExpr* kset =
            kpat.IsSet() ? &kpat
            : (kpat.op == RtlOp::kParallel && !kpat.children.empty() && kpat.children[0].IsSet())
                ? &kpat.children[0]
                : nullptr;
        if (kset != nullptr) {
          check_reads(kset->SetSrc());
          if (!kset->SetDest().IsHardReg()) {
            check_reads(kset->SetDest());
          }
          if (kset->SetDest().IsHardReg()) {
            const int d = kset->SetDest().RegNo();
            const int d_nw = std::max(1, MipsHardRegNregs(d, kset->SetDest().mode, gp64, fp64));
            if (std::max(d, ret_hr) < std::min(d + d_nw, ret_hr + nw)) {
              break;
            }
          }
        } else {
          check_reads(kpat);
        }
        if (mode_mismatch) {
          break;
        }
      }
      if (mode_mismatch || last_read == c) {
        continue;
      }
      RtlPatternAndNotes cp;
      cp.pattern = RtlExpr::MakeSet(MakeGccHardReg(ret_m, ret_hr), MakeGccHardReg(ret_m, ret_hr));
      blk.insert(blk.begin() + static_cast<ptrdiff_t>(c + 1), std::move(cp));
      ++c;
      if (is_fix_di) {
        EnsureTrailingDiSelfMove(&blk, c, ret_hr, &c);
      }
    }
  }
}

void ReconstructO0SubwordTruncatingSubregs(
    size_t prologue_end, bool gp64, bool fp64,
    std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  for (size_t b = 0; b < all_blocks->size(); ++b) {
    auto& blk = (*all_blocks)[b];
    const size_t start_i = (b == 0) ? prologue_end : 0;
    for (size_t i = start_i; i < blk.size(); ++i) {
      RtlExpr& pat = blk[i].pattern;
      if (!pat.IsSet() || IsCallPattern(pat) || !pat.SetDest().IsHardReg() ||
          pat.SetDest().flags.integrated || pat.SetDest().mode != RtlMachineMode::kSi) {
        continue;
      }
      // MIPS `lb`/`lbu`/`lh`/`lhu` disassemble as `(zero_extend:SI (mem:QI|HI ...))`
      // or `(sign_extend:SI (mem:QI|HI ...))`, which at `-O0` may represent a
      // narrow `movqi`/`movhi` load into a `QImode`/`HImode` pseudo rather than
      // an `SImode` pseudo truncated via `alter_subreg`.
      if ((pat.SetSrc().op == RtlOp::kZeroExtend || pat.SetSrc().op == RtlOp::kSignExtend) &&
          !pat.SetSrc().children.empty() && pat.SetSrc().children[0].IsMem()) {
        continue;
      }
      const int hr = pat.SetDest().RegNo();
      if (hr < 2 || hr > 25) {
        continue;
      }
      bool has_subword_read = false;
      bool unsafe_escape = false;
      size_t last_read = i;
      for (size_t k = i + 1; k < blk.size(); ++k) {
        const RtlExpr& kpat = blk[k].pattern;
        if (IsCallPattern(kpat)) {
          for (const RtlExpr& fu : blk[k].call_fusage) {
            if (fu.ContainsReg(hr)) {
              unsafe_escape = true;
            }
          }
          break;
        }
        if (kpat.op == RtlOp::kUse && !kpat.children.empty() && kpat.children[0].ContainsReg(hr)) {
          unsafe_escape = true;
          break;
        }
        auto scan_read_expr = [&](const RtlExpr& expr) {
          expr.Visit([&](const RtlExpr& n) {
            if (n.op == RtlOp::kSubreg && !n.children.empty() && n.children[0].IsHardReg() &&
                n.children[0].RegNo() == hr) {
              unsafe_escape = true;
              return;
            }
            if (n.IsHardReg()) {
              const int r = n.RegNo();
              const int nw = std::max(1, MipsHardRegNregs(r, n.mode, gp64, fp64));
              if (hr >= r && hr < r + nw) {
                if (r == hr && !n.flags.integrated &&
                    (n.mode == RtlMachineMode::kQi || n.mode == RtlMachineMode::kHi ||
                     n.mode == RtlMachineMode::kSi)) {
                  last_read = k;
                  if (n.mode == RtlMachineMode::kQi || n.mode == RtlMachineMode::kHi) {
                    has_subword_read = true;
                  }
                } else {
                  unsafe_escape = true;
                }
              }
            }
          });
        };
        if (kpat.IsSet()) {
          scan_read_expr(kpat.SetSrc());
          if (!kpat.SetDest().IsHardReg()) {
            scan_read_expr(kpat.SetDest());
          } else {
            const int d = kpat.SetDest().RegNo();
            const int nw = std::max(1, MipsHardRegNregs(d, kpat.SetDest().mode, gp64, fp64));
            if (hr >= d && hr < d + nw) {
              if (d != hr) {
                unsafe_escape = true;
              }
              break;
            }
          }
        } else {
          scan_read_expr(kpat);
        }
        if (unsafe_escape) {
          break;
        }
      }
      if (!has_subword_read || unsafe_escape || last_read == i) {
        continue;
      }
      const RtlFlags si_flags = pat.SetDest().flags;
      auto rewrite_read = [&](RtlExpr& n) {
        if (n.IsHardReg() && n.RegNo() == hr && !n.flags.integrated &&
            (n.mode == RtlMachineMode::kQi || n.mode == RtlMachineMode::kHi)) {
          n = RtlExpr::MakeSubreg(n.mode, MakeGccHardReg(RtlMachineMode::kSi, hr, si_flags), 0);
        }
      };
      for (size_t k = i + 1; k <= last_read; ++k) {
        if (blk[k].pattern.IsSet()) {
          blk[k].pattern.children[1].Mutate(rewrite_read);
          if (!blk[k].pattern.children[0].IsHardReg()) {
            blk[k].pattern.children[0].Mutate(rewrite_read);
          }
        } else {
          blk[k].pattern.Mutate(rewrite_read);
        }
      }
    }
  }
}

}  // namespace

void ReconstructO0FinalScanDeletions(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty() || profile.optimize_level != 0) {
    return;
  }

  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);

  std::vector<std::vector<RtlPatternAndNotes>> all_blocks(cfg->rpo_blocks.size());
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    const RtlBasicBlock& blk = cfg->rpo_blocks[b];
    all_blocks[b].reserve(blk.instructions.size());
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> pn = ParsePatternAndNotes(blk.instructions[i]);
      if (!pn.ok()) {
        return;
      }
      if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
          !pn->has_explicit_call_fusage) {
        pn->has_explicit_call_fusage = true;
        pn->call_fusage = blk.rtl_insns[i].call_fusage;
      }
      all_blocks[b].push_back(std::move(*pn));
    }
  }

  const O0PrologueInfo pinfo = AnalyzeStage0O0PrologueAndTopSpills(*cfg, all_blocks, gp64, fp64);

  ReconstructO0NoConflictAndMixedDiSubregs(pinfo.prologue_end, &all_blocks);
  ReconstructO0SubwordTruncatingSubregs(pinfo.prologue_end, gp64, fp64, &all_blocks);
  ReconstructO0TablejumpIndexSelfMoves(*cfg, pinfo.prologue_end, gp64, fp64, &all_blocks);
  ReconstructO0PrecomputedCallArgSelfMoves(pinfo.prologue_end, gp64, fp64, &all_blocks);
  ReconstructO0CallReturnSelfMoves(pinfo, gp64, fp64, &all_blocks);
  ReconstructO0ObeyRegdeclsScopeUses(*cfg, pinfo, gp64, fp64, &all_blocks);
  WrapO0SingleBlockDiSubregs(pinfo.prologue_end, &all_blocks);

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    blk.instructions.clear();
    blk.instructions.reserve(all_blocks[b].size());
    for (const RtlPatternAndNotes& pn : all_blocks[b]) {
      const bool is_top_call = pn.pattern.op != RtlOp::kSequence && IsCallPattern(pn.pattern);
      blk.instructions.push_back(FormatGccPrintRtlPatternAndNotes(
          pn.pattern, is_top_call ? absl::Span<const RtlRegNote>{} : pn.reg_notes,
          pn.bracketed_notes, /*call_fusage=*/{}));
    }
    blk.SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < blk.rtl_insns.size() && i < all_blocks[b].size(); ++i) {
      if (all_blocks[b][i].has_explicit_call_fusage && !blk.rtl_insns[i].has_explicit_call_fusage) {
        blk.rtl_insns[i].has_explicit_call_fusage = true;
        blk.rtl_insns[i].call_fusage = all_blocks[b][i].call_fusage;
      }
    }
  }
}

}  // namespace rom_nom_nom::lifter
