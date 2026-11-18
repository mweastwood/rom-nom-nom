#include "lifter/reconstruct_o0_final_scan.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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

namespace rom_nom_nom::lifter {
namespace {

constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;
constexpr int kStackPointerRegnum = 29;
constexpr int kFramePointerRegnum = 30;
constexpr int kMaxInt = std::numeric_limits<int>::max();

RtlExpr MakeGccHardReg(RtlMachineMode mode, int regno, RtlFlags flags = {}) {
  RtlExpr reg = RtlExpr::MakeReg(mode, regno, flags);
  if (regno == 30) {
    reg.str_val = "$fp";
  }
  return reg;
}

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

bool IsO0OutgoingCallArgHardReg(int hr) {
  return (hr >= 4 && hr <= 7) || (hr >= 44 && hr <= 47);
}

bool IsMipsO0CallUsedHardReg(int h) {
  return h == 1 || (h >= 2 && h <= 15) || h == 24 || h == 25 || h == 31 || (h >= 32 && h <= 51) ||
         (h >= 64 && h <= 67);
}

bool IsMipsO0CalleeSavedRange(int hr, int nw) {
  for (int w = hr; w < hr + nw; ++w) {
    const bool gpr_saved = (w >= 16 && w <= 23);
    const bool fpr_saved = (w >= 52 && w <= 63);
    if (!gpr_saved && !fpr_saved) {
      return false;
    }
  }
  return true;
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

struct O0PrologueInfo {
  size_t prologue_end = 0;
  bool frame_pointer_needed = false;
  int local_frame_bytes = 0;
  int outgoing_args_bytes = 0;
  absl::flat_hash_set<int> valid_top_spill_slot_bases;
};

std::optional<int64_t> MatchRegPlusConstOffset(const RtlExpr& addr, int base_reg) {
  if (addr.IsHardReg() && addr.RegNo() == base_reg) {
    return 0;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsHardReg() &&
      addr.children[0].RegNo() == base_reg && addr.children[1].op == RtlOp::kConstInt) {
    return addr.children[1].int_val;
  }
  return std::nullopt;
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
                     !(jset->SetDest().RegNo() == hr &&
                       jset->SetDest().mode == RtlMachineMode::kDi)) {
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
      bool used_after = false;
      for (size_t j = end_idx + 1; j < blk.size(); ++j) {
        const RtlExpr& jpat = blk[j].pattern;
        if (IsCallPattern(jpat)) {
          for (const RtlExpr& fu : blk[j].call_fusage) {
            if (expr_mentions_hard_pair(fu, hr)) {
              used_after = true;
            }
          }
          break;
        }
        if (jpat.IsSet() && jpat.SetDest().IsHardReg() && jpat.SetDest().RegNo() == hr &&
            jpat.SetDest().mode == RtlMachineMode::kDi &&
            !expr_mentions_hard_pair(jpat.SetSrc(), hr)) {
          break;
        }
        if (expr_mentions_hard_pair(jpat, hr)) {
          used_after = true;
          break;
        }
      }
      if (used_after) {
        continue;
      }
      auto reg_used_after_pair = [&](int target_r) {
        for (size_t j = i + 2; j < blk.size(); ++j) {
          const RtlExpr& jpat = blk[j].pattern;
          if (IsCallPattern(jpat)) {
            for (const RtlExpr& fu : blk[j].call_fusage) {
              if (fu.ContainsReg(target_r)) {
                return true;
              }
            }
            return false;
          }
          if (jpat.IsSet() && jpat.SetDest().IsHardReg() && jpat.SetDest().RegNo() == target_r &&
              !jpat.SetSrc().ContainsReg(target_r)) {
            return false;
          }
          if (jpat.ContainsReg(target_r)) {
            return true;
          }
        }
        return false;
      };
      size_t clob_pos = i;
      const RtlExpr* hi_set = (p0.SetDest().RegNo() == hr) ? &p0 : &p1;
      const RtlExpr* lo_set = (p0.SetDest().RegNo() == hr) ? &p1 : &p0;
      if (i > start_i && hi_set->SetSrc().IsHardReg() &&
          hi_set->SetSrc().mode == RtlMachineMode::kSi) {
        const int sign_hr = hi_set->SetSrc().RegNo();
        const RtlExpr& prev1 = blk[i - 1].pattern;
        if (sign_hr >= 2 && sign_hr <= 25 && sign_hr != hr && sign_hr != hr + 1 &&
            !IsCallPattern(prev1) && prev1.IsSet() && prev1.SetDest().IsHardReg() &&
            prev1.SetDest().RegNo() == sign_hr && prev1.SetDest().mode == RtlMachineMode::kSi &&
            prev1.SetSrc().op == RtlOp::kAshiftrt && prev1.SetSrc().children.size() == 2 &&
            prev1.SetSrc().children[0].IsHardReg() &&
            prev1.SetSrc().children[1].op == RtlOp::kConstInt &&
            prev1.SetSrc().children[1].int_val == 31 && !lo_set->SetSrc().ContainsReg(sign_hr) &&
            !reg_used_after_pair(sign_hr)) {
          clob_pos = i - 1;
          const int src_hr = prev1.SetSrc().children[0].RegNo();
          if (clob_pos > start_i && lo_set->SetSrc().IsMem() && src_hr >= 2 && src_hr <= 25 &&
              src_hr != hr && src_hr != hr + 1 && !reg_used_after_pair(src_hr)) {
            const RtlExpr& prev2 = blk[clob_pos - 1].pattern;
            if (!IsCallPattern(prev2) && prev2.IsSet() && prev2.SetDest().IsHardReg() &&
                prev2.SetDest().RegNo() == src_hr && prev2.SetDest().mode == RtlMachineMode::kSi &&
                prev2.SetSrc() == lo_set->SetSrc()) {
              --clob_pos;
            }
          }
        }
      }
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

void WrapO0NoConflictBlockDiSubregs(size_t prologue_end,
                                    std::vector<std::vector<RtlPatternAndNotes>>* blocks) {
  for (size_t b = 0; b < blocks->size(); ++b) {
    auto& blk = (*blocks)[b];
    const size_t start_i = (b == 0) ? prologue_end : 0;
    for (size_t i = start_i; i + 3 < blk.size(); ++i) {
      if (blk[i].pattern.op != RtlOp::kClobber || blk[i].pattern.children.size() != 1 ||
          !blk[i].pattern.children[0].IsHardReg() ||
          blk[i].pattern.children[0].mode != RtlMachineMode::kDi ||
          blk[i].pattern.children[0].flags.integrated) {
        continue;
      }
      const int hr = blk[i].pattern.children[0].RegNo();
      if (hr < 2 || hr > 24 || (hr % 2) != 0) {
        continue;
      }
      for (size_t k = i + 1; k + 2 < blk.size(); ++k) {
        const RtlExpr& p0 = blk[k].pattern;
        const RtlExpr& p1 = blk[k + 1].pattern;
        const RtlExpr& mv = blk[k + 2].pattern;
        if (p0.IsSet() && p1.IsSet() && mv.IsSet() && p0.SetDest().IsHardReg() &&
            p1.SetDest().IsHardReg() && p0.SetDest().mode == RtlMachineMode::kSi &&
            p1.SetDest().mode == RtlMachineMode::kSi && mv.SetDest().IsHardReg() &&
            mv.SetSrc().IsHardReg() && mv.SetDest().RegNo() == hr && mv.SetSrc().RegNo() == hr &&
            mv.SetDest().mode == RtlMachineMode::kDi) {
          const int r0 = p0.SetDest().RegNo();
          const int r1 = p1.SetDest().RegNo();
          if ((r0 == hr && r1 == hr + 1) || (r0 == hr + 1 && r1 == hr)) {
            blk[k].pattern.children[0] = RtlExpr::MakeSubreg(
                RtlMachineMode::kSi, MakeGccHardReg(RtlMachineMode::kDi, hr, mv.SetDest().flags),
                r0 - hr);
            blk[k + 1].pattern.children[0] = RtlExpr::MakeSubreg(
                RtlMachineMode::kSi, MakeGccHardReg(RtlMachineMode::kDi, hr, mv.SetDest().flags),
                r1 - hr);
            i = k + 2;
            break;
          }
        }
        if (p0.op != RtlOp::kUse && (IsCallPattern(p0) || IsControlTransferPattern(p0) ||
                                     p0.ContainsReg(hr) || p0.ContainsReg(hr + 1))) {
          break;
        }
      }
    }
  }
}

void WrapTrialO0DiSubregs(size_t prologue_end,
                          std::vector<std::vector<RtlPatternAndNotes>>* trial_blocks) {
  WrapO0NoConflictBlockDiSubregs(prologue_end, trial_blocks);
  for (size_t b = 0; b < trial_blocks->size(); ++b) {
    auto& blk = (*trial_blocks)[b];
    const size_t start_i = (b == 0) ? prologue_end : 0;
    for (size_t i = start_i; i < blk.size(); ++i) {
      RtlExpr* set0 = blk[i].pattern.IsSet() ? &blk[i].pattern
                      : (blk[i].pattern.op == RtlOp::kParallel &&
                         !blk[i].pattern.children.empty() && blk[i].pattern.children[0].IsSet())
                          ? &blk[i].pattern.children[0]
                          : nullptr;
      if (set0 == nullptr || IsCallPattern(blk[i].pattern) || !set0->SetDest().IsHardReg() ||
          set0->SetDest().mode != RtlMachineMode::kDi || set0->SetDest().flags.integrated) {
        continue;
      }
      const int hr = set0->SetDest().RegNo();
      const RtlFlags di_flags = set0->SetDest().flags;
      if (hr < 2 || hr > 24 || (hr % 2) != 0 || set0->SetSrc().ContainsReg(hr) ||
          set0->SetSrc().ContainsReg(hr + 1)) {
        continue;
      }
      bool has_si_subword = false;
      size_t end_idx = blk.size();
      for (size_t j = i + 1; j < blk.size(); ++j) {
        const RtlExpr& jpat = blk[j].pattern;
        if (IsCallPattern(jpat)) {
          break;
        }
        if (jpat.IsSet() && jpat.SetSrc().IsHardReg() && jpat.SetSrc().RegNo() == hr &&
            jpat.SetSrc().mode == RtlMachineMode::kDi && !jpat.SetDest().ContainsReg(hr) &&
            !jpat.SetDest().ContainsReg(hr + 1)) {
          end_idx = j;
          break;
        }
        if (jpat.IsSet() && jpat.SetDest().IsHardReg() &&
            (jpat.SetDest().RegNo() == hr || jpat.SetDest().RegNo() == hr + 1) &&
            jpat.SetDest().mode == RtlMachineMode::kSi) {
          has_si_subword = true;
        } else if (jpat.op != RtlOp::kUse && (jpat.ContainsReg(hr) || jpat.ContainsReg(hr + 1))) {
          break;
        }
      }
      if (!has_si_subword || end_idx >= blk.size()) {
        continue;
      }
      bool used_after = false;
      for (size_t j = end_idx + 1; j < blk.size(); ++j) {
        const RtlExpr& jpat = blk[j].pattern;
        if (IsCallPattern(jpat)) {
          for (const RtlExpr& fu : blk[j].call_fusage) {
            if (fu.ContainsReg(hr) || fu.ContainsReg(hr + 1)) {
              used_after = true;
            }
          }
          break;
        }
        if (jpat.op == RtlOp::kUse) {
          continue;
        }
        if (jpat.IsSet() && jpat.SetDest().IsHardReg() && jpat.SetDest().RegNo() == hr &&
            jpat.SetDest().mode == RtlMachineMode::kDi && !jpat.SetSrc().ContainsReg(hr) &&
            !jpat.SetSrc().ContainsReg(hr + 1)) {
          break;
        }
        if (jpat.ContainsReg(hr) || jpat.ContainsReg(hr + 1)) {
          used_after = true;
          break;
        }
      }
      if (used_after) {
        continue;
      }
      for (size_t j = i + 1; j < end_idx; ++j) {
        if (blk[j].pattern.op == RtlOp::kUse) {
          continue;
        }
        blk[j].pattern.Mutate([&](RtlExpr& n) {
          if (n.IsHardReg() && (n.RegNo() == hr || n.RegNo() == hr + 1) &&
              n.mode == RtlMachineMode::kSi && !n.flags.integrated) {
            n = RtlExpr::MakeSubreg(RtlMachineMode::kSi,
                                    MakeGccHardReg(RtlMachineMode::kDi, hr, di_flags),
                                    n.RegNo() - hr);
          }
        });
      }
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
    if (!((scr >= 1 && scr <= 3) || (scr >= 8 && scr <= 15) || scr == 24 || scr == 25)) {
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

bool IsO0FixDiLibcall(const RtlExpr& call_set) {
  if (call_set.SetDest().mode != RtlMachineMode::kDi || call_set.SetDest().RegNo() != 2) {
    return false;
  }
  bool is_fix_di = false;
  call_set.SetSrc().Visit([&](const RtlExpr& n) {
    if (n.IsSymbolRef()) {
      const std::string_view sym = n.SymbolName();
      if (sym == "__fixdfdi" || sym == "__fixsfdi" || sym == "__fixunsdfdi" ||
          sym == "__fixunssfdi") {
        is_fix_di = true;
      }
    }
  });
  return is_fix_di;
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
                !(kset->SetDest().op == RtlOp::kSubreg && !kset->SetDest().children.empty() &&
                  kset->SetDest().children[0].IsHardReg())) {
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

struct O0PseudoSuidSpan {
  int born = kMaxInt;
  int dead = -1;
  int n_refs = 0;
  int n_calls = 0;
  size_t birth_b = 0;
  size_t birth_i = 0;
  RtlMachineMode mode = RtlMachineMode::kSi;
  bool changes_size = false;
  bool live_flag = false;
  bool has_explicit_use_insn = false;
  bool is_entry_param = false;
};

struct O0StupidTimeline {
  int exit_suid = 1;
  int b0_prefix_suid = 1;
  std::vector<int> call_suids;
  std::vector<std::array<bool, 68>> after_hr;
  absl::flat_hash_map<int, O0PseudoSuidSpan> spans;
};

enum class O0ScopeKind { kEntryParam, kB0Local, kBirthLocal };

bool IsO0FpInGprFallback(const O0PseudoSuidSpan& sp, int exp_hr) {
  return (sp.mode == RtlMachineMode::kSf || sp.mode == RtlMachineMode::kDf) && !sp.changes_size &&
         exp_hr >= 2 && exp_hr < 32;
}

struct O0OrderedScopes {
  std::vector<int> entry_params;
  std::vector<int> b0_locals;
  std::vector<int> birth_locals;
  absl::flat_hash_map<int, int> index_in_list;
};

O0OrderedScopes OrderO0Scopes(const O0StupidTimeline& tl,
                              const absl::flat_hash_map<int, int>& exp_renumber,
                              const absl::flat_hash_map<int, O0ScopeKind>& scope_kinds) {
  O0OrderedScopes ord;
  for (const auto& [p, kind] : scope_kinds) {
    if (kind == O0ScopeKind::kEntryParam) {
      ord.entry_params.push_back(p);
    } else if (kind == O0ScopeKind::kB0Local) {
      ord.b0_locals.push_back(p);
    } else {
      ord.birth_locals.push_back(p);
    }
  }
  std::sort(ord.entry_params.begin(), ord.entry_params.end(), [&](int a, int b) {
    const int ba = tl.spans.at(a).born;
    const int bb = tl.spans.at(b).born;
    return ba != bb ? (ba < bb) : (a < b);
  });
  std::sort(ord.b0_locals.begin(), ord.b0_locals.end(), [&](int a, int b) {
    const int ha = exp_renumber.at(a);
    const int hb = exp_renumber.at(b);
    const bool fa = IsO0FpInGprFallback(tl.spans.at(a), ha);
    const bool fb = IsO0FpInGprFallback(tl.spans.at(b), hb);
    if (fa != fb) {
      return !fa;
    }
    return ha != hb ? (ha < hb) : (a < b);
  });
  std::sort(ord.birth_locals.begin(), ord.birth_locals.end(), [&](int a, int b) {
    const int ba = tl.spans.at(a).born;
    const int bb = tl.spans.at(b).born;
    if (ba != bb) {
      return ba > bb;
    }
    const int ha = exp_renumber.at(a);
    const int hb = exp_renumber.at(b);
    return ha != hb ? (ha > hb) : (a > b);
  });
  for (size_t i = 0; i < ord.entry_params.size(); ++i) {
    ord.index_in_list[ord.entry_params[i]] = static_cast<int>(i);
  }
  for (size_t i = 0; i < ord.b0_locals.size(); ++i) {
    ord.index_in_list[ord.b0_locals[i]] = static_cast<int>(i);
  }
  for (size_t i = 0; i < ord.birth_locals.size(); ++i) {
    ord.index_in_list[ord.birth_locals[i]] = static_cast<int>(i);
  }
  return ord;
}

void ComputeO0PostScopeSpan(const O0StupidTimeline& tl,
                            const absl::flat_hash_map<int, O0ScopeKind>& scope_kinds,
                            const O0OrderedScopes& ord, int p, int* eff_born, int* eff_dead,
                            int* alloc_len) {
  const O0PseudoSuidSpan& sp = tl.spans.at(p);
  const int n_entry = static_cast<int>(ord.entry_params.size());
  const int n_b0 = static_cast<int>(ord.b0_locals.size());
  const int n_birth = static_cast<int>(ord.birth_locals.size());
  const int exit_base = tl.exit_suid + n_entry + n_b0 + n_birth;
  auto sk_it = scope_kinds.find(p);
  if (sk_it == scope_kinds.end()) {
    *eff_born = sp.born;
    *eff_dead = sp.dead;
    int shifts =
        (sp.born < tl.b0_prefix_suid && tl.b0_prefix_suid <= sp.dead) ? (n_entry + n_b0) : 0;
    for (int r : ord.birth_locals) {
      const int rb = tl.spans.at(r).born;
      if (sp.born < rb && rb <= sp.dead) {
        ++shifts;
      }
    }
    *alloc_len = (sp.dead - sp.born) + shifts;
    return;
  }
  const int idx = ord.index_in_list.at(p);
  if (sk_it->second == O0ScopeKind::kBirthLocal) {
    *eff_born = sp.born;
    *eff_dead = tl.exit_suid;
    const int new_born =
        sp.born + ((tl.b0_prefix_suid <= sp.born) ? (n_entry + n_b0) : 0) + (n_birth - 1 - idx);
    const int new_dead = exit_base + idx;
    *alloc_len = new_dead - new_born;
    return;
  }
  *eff_born = std::min(sp.born, tl.b0_prefix_suid);
  *eff_dead = tl.exit_suid;
  if (sk_it->second == O0ScopeKind::kB0Local) {
    int births_before = 0;
    for (int r : ord.birth_locals) {
      if (tl.spans.at(r).born < tl.b0_prefix_suid) {
        ++births_before;
      }
    }
    const int new_born = (sp.born < tl.b0_prefix_suid)
                             ? sp.born
                             : (tl.b0_prefix_suid + births_before + n_entry + idx);
    const int new_dead = exit_base + n_birth + (n_b0 - 1 - idx);
    *alloc_len = new_dead - new_born;
    return;
  }
  int births_before = 0;
  for (int r : ord.birth_locals) {
    if (tl.spans.at(r).born <= sp.born) {
      ++births_before;
    }
  }
  const int new_born = std::min(sp.born, tl.b0_prefix_suid) + births_before;
  const int new_dead = exit_base + n_birth + n_b0 + (n_entry - 1 - idx);
  *alloc_len = new_dead - new_born;
}

absl::flat_hash_map<int, int> SimulateO0StupidAlloc(
    bool gp64, bool fp64, const O0StupidTimeline& tl,
    const absl::flat_hash_map<int, int>& exp_renumber,
    const absl::flat_hash_map<int, O0ScopeKind>& scope_kinds, const O0OrderedScopes& ord) {
  auto work_after = tl.after_hr;
  struct SimItem {
    int p = 0;
    int born = 0;
    int dead = 0;
    int alloc_len = 0;
    int refs = 0;
    int calls = 0;
    int exp_hr = 0;
  };
  std::vector<SimItem> items;
  items.reserve(tl.spans.size());
  for (const auto& [p, sp] : tl.spans) {
    if (sp.born == kMaxInt) {
      continue;
    }
    SimItem it{.p = p, .refs = sp.n_refs, .calls = sp.n_calls, .exp_hr = exp_renumber.at(p)};
    ComputeO0PostScopeSpan(tl, scope_kinds, ord, p, &it.born, &it.dead, &it.alloc_len);
    if (scope_kinds.contains(p)) {
      it.refs += 2;
      const bool crosses = std::any_of(tl.call_suids.begin(), tl.call_suids.end(),
                                       [&](int cs) { return cs >= it.born && cs < it.dead; });
      if (crosses) {
        it.calls = std::max(1, it.calls);
      }
    }
    items.push_back(it);
  }
  std::sort(items.begin(), items.end(), [](const SimItem& a, const SimItem& b) {
    if (a.alloc_len != b.alloc_len) {
      return a.alloc_len > b.alloc_len;
    }
    if (a.refs != b.refs) {
      return a.refs < b.refs;
    }
    return a.p < b.p;
  });

  auto find_in_class = [&](int cls, RtlMachineMode mode, int born, int dead, int calls,
                           bool changes_size) -> int {
    std::array<bool, 68> used{};
    for (int f : {0, 1, 26, 27, 28, 29, 30, 31, 67}) {
      used[f] = true;
    }
    if (calls > 0) {
      for (int h = 0; h < 68; ++h) {
        if (IsMipsO0CallUsedHardReg(h)) {
          used[h] = true;
        }
      }
    }
    for (int ins = std::max(0, born); ins < std::min(dead, static_cast<int>(work_after.size()));
         ++ins) {
      for (int h = 0; h < 68; ++h) {
        if (work_after[ins][h]) {
          used[h] = true;
        }
      }
    }
    for (int h = 0; h < 68; ++h) {
      const bool in_cls = (cls == 0)   ? (h >= 2 && h <= 25)
                          : (cls == 1) ? (h >= 32 && h <= 63 && !changes_size)
                                       : (h >= 64 && h <= 66);
      if (!in_cls) {
        used[h] = true;
      }
    }
    for (int r = 0; r < 67; ++r) {
      if (used[r]) {
        continue;
      }
      const int nw = std::max(1, MipsHardRegNregs(r, mode, gp64, fp64));
      const bool mode_ok = (r >= 2 && r <= 25)    ? (nw == 1 || (r % 2) == 0)
                           : (r >= 32 && r <= 63) ? (fp64 || (r % 2) == 0)
                                                  : (r == 64 || (r == 65 && nw == 1));
      if (!mode_ok) {
        continue;
      }
      bool all_free = true;
      for (int j = 1; j < nw; ++j) {
        if (r + j >= 68 || used[r + j]) {
          all_free = false;
          break;
        }
      }
      if (all_free) {
        for (int ins = std::max(0, born); ins < std::min(dead, static_cast<int>(work_after.size()));
             ++ins) {
          for (int j = 0; j < nw; ++j) {
            work_after[ins][r + j] = true;
          }
        }
        return r;
      }
    }
    return -1;
  };

  absl::flat_hash_map<int, int> sim_renum;
  for (const SimItem& it : items) {
    const O0PseudoSuidSpan& sp = tl.spans.at(it.p);
    int got = -1;
    if (it.exp_hr >= 64) {
      got = find_in_class(2, sp.mode, it.born, it.dead, it.calls, sp.changes_size);
    } else if ((sp.mode == RtlMachineMode::kSf || sp.mode == RtlMachineMode::kDf) &&
               !sp.changes_size) {
      got = find_in_class(1, sp.mode, it.born, it.dead, it.calls, sp.changes_size);
      if (got < 0) {
        got = find_in_class(0, sp.mode, it.born, it.dead, it.calls, sp.changes_size);
      }
    } else {
      got = find_in_class(0, sp.mode, it.born, it.dead, it.calls, sp.changes_size);
    }
    sim_renum[it.p] = got;
  }
  return sim_renum;
}

bool IsO0ParallelScratchClobberInsn(const RtlExpr& pat) {
  if (pat.op != RtlOp::kParallel || pat.children.size() < 2) {
    return false;
  }
  bool has_asm = false;
  pat.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kAsmOperands || n.op == RtlOp::kAsmInput) {
      has_asm = true;
    }
  });
  if (has_asm) {
    return false;
  }
  const RtlExpr& main_e = pat.children[0];
  if (!main_e.IsSet()) {
    return false;
  }
  if (main_e.SetDest().mode == RtlMachineMode::kDi && main_e.SetSrc().mode == RtlMachineMode::kDi &&
      (main_e.SetSrc().op == RtlOp::kPlus || main_e.SetSrc().op == RtlOp::kMinus ||
       main_e.SetSrc().op == RtlOp::kNeg || main_e.SetSrc().op == RtlOp::kAshift ||
       main_e.SetSrc().op == RtlOp::kAshiftrt || main_e.SetSrc().op == RtlOp::kLshiftrt)) {
    return false;
  }
  return true;
}

struct TrialO0Model {
  std::vector<std::vector<RtlPatternAndNotes>> blocks;
  std::vector<std::vector<size_t>> orig_idx;
  absl::flat_hash_map<int, int> reg_renumber;
  absl::flat_hash_map<int, RtlMachineMode> pseudo_modes;
};

void FoldTrialO0ReloadScratchesAndMulsidi3Spills(const O0PrologueInfo& pinfo, bool gp64,
                                                 int* next_p, TrialO0Model* m) {
  if (pinfo.frame_pointer_needed && pinfo.local_frame_bytes >= 8 &&
      (pinfo.local_frame_bytes % 8) == 0) {
    const int64_t top_off = pinfo.outgoing_args_bytes + pinfo.local_frame_bytes - 8;
    for (size_t b = 0; b < m->blocks.size(); ++b) {
      auto& blk = m->blocks[b];
      auto& oidx = m->orig_idx[b];
      for (size_t i = 0; i + 2 < blk.size(); ++i) {
        const RtlExpr& mpat = blk[i].pattern;
        if (mpat.op != RtlOp::kParallel || mpat.children.empty() || !mpat.children[0].IsSet() ||
            mpat.children[0].SetDest().mode != RtlMachineMode::kDi ||
            !mpat.children[0].SetDest().IsReg() || mpat.children[0].SetDest().IsArgPointerReg() ||
            mpat.children[0].SetSrc().op != RtlOp::kMult) {
          continue;
        }
        const int mul_r = mpat.children[0].SetDest().RegNo();
        const int mul_hr = (mul_r < kFirstPseudoRegister)
                               ? mul_r
                               : (m->reg_renumber.contains(mul_r) ? m->reg_renumber[mul_r] : -1);
        int sec_p = -1;
        size_t store_idx = 0;
        if (blk[i + 1].pattern.IsSet() && blk[i + 1].pattern.SetSrc().IsReg() &&
            !blk[i + 1].pattern.SetSrc().IsArgPointerReg() &&
            blk[i + 1].pattern.SetSrc().RegNo() == mul_r && blk[i + 1].pattern.SetDest().IsMem() &&
            !blk[i + 1].pattern.SetDest().children.empty() &&
            MatchRegPlusConstOffset(blk[i + 1].pattern.SetDest().children[0],
                                    kFramePointerRegnum) == top_off) {
          store_idx = i + 1;
        }
        if (store_idx == 0 && i + 3 < blk.size() && mul_hr == 64 && blk[i + 1].pattern.IsSet() &&
            blk[i + 1].pattern.SetDest().IsPseudoReg() &&
            blk[i + 1].pattern.SetDest().mode == RtlMachineMode::kDi &&
            blk[i + 1].pattern.SetSrc().IsReg() && !blk[i + 1].pattern.SetSrc().IsArgPointerReg() &&
            blk[i + 1].pattern.SetSrc().RegNo() == mul_r) {
          const int cand_sec = blk[i + 1].pattern.SetDest().RegNo();
          if (blk[i + 2].pattern.IsSet() && blk[i + 2].pattern.SetSrc().IsPseudoReg() &&
              blk[i + 2].pattern.SetSrc().RegNo() == cand_sec &&
              blk[i + 2].pattern.SetDest().IsMem() &&
              !blk[i + 2].pattern.SetDest().children.empty() &&
              MatchRegPlusConstOffset(blk[i + 2].pattern.SetDest().children[0],
                                      kFramePointerRegnum) == top_off) {
            sec_p = cand_sec;
            store_idx = i + 2;
          }
        }
        if (store_idx == 0) {
          continue;
        }
        size_t load_idx = blk.size();
        for (size_t k = store_idx + 1; k + 1 < blk.size(); ++k) {
          if (blk[k].pattern.IsSet() && blk[k].pattern.SetDest().IsPseudoReg() &&
              blk[k].pattern.SetDest().mode == RtlMachineMode::kDi &&
              blk[k].pattern.SetSrc().IsMem() && !blk[k].pattern.SetSrc().children.empty() &&
              MatchRegPlusConstOffset(blk[k].pattern.SetSrc().children[0], kFramePointerRegnum) ==
                  top_off) {
            load_idx = k;
            break;
          }
        }
        if (load_idx >= blk.size()) {
          continue;
        }
        bool other_fp_ref = false;
        for (size_t b2 = 0; b2 < m->blocks.size() && !other_fp_ref; ++b2) {
          for (size_t j2 = 0; j2 < m->blocks[b2].size(); ++j2) {
            if (b2 == b && (j2 == store_idx || j2 == load_idx)) {
              continue;
            }
            m->blocks[b2][j2].pattern.Visit([&](const RtlExpr& n) {
              if (n.op == RtlOp::kMem && n.children.size() == 1) {
                const std::optional<int64_t> off =
                    MatchRegPlusConstOffset(n.children[0], kFramePointerRegnum);
                const int sz = std::max(1, ModeByteSize(n.mode));
                if (off.has_value() && *off < top_off + 8 && *off + sz > top_off) {
                  other_fp_ref = true;
                }
              }
            });
          }
        }
        if (other_fp_ref) {
          continue;
        }
        int spill_p = mul_r;
        if (spill_p < kFirstPseudoRegister) {
          spill_p = (*next_p)++;
          blk[i].pattern.children[0].children[0] = RtlExpr::MakeReg(RtlMachineMode::kDi, spill_p);
        }
        const int load_p = blk[load_idx].pattern.SetDest().RegNo();
        auto replace_load_p_reads = [&](RtlExpr& stmt) {
          if (stmt.IsSet() && stmt.children.size() >= 2) {
            if (!stmt.children[0].IsPseudoReg()) {
              stmt.children[0].Mutate([&](RtlExpr& n) {
                if (n.IsPseudoReg() && n.RegNo() == load_p) {
                  n.SetRegNo(spill_p);
                }
              });
            }
            stmt.children[1].Mutate([&](RtlExpr& n) {
              if (n.IsPseudoReg() && n.RegNo() == load_p) {
                n.SetRegNo(spill_p);
              }
            });
          } else if (stmt.op != RtlOp::kClobber) {
            stmt.Mutate([&](RtlExpr& n) {
              if (n.IsPseudoReg() && n.RegNo() == load_p) {
                n.SetRegNo(spill_p);
              }
            });
          }
        };
        if (blk[load_idx + 1].pattern.op == RtlOp::kParallel) {
          for (RtlExpr& elem : blk[load_idx + 1].pattern.children) {
            replace_load_p_reads(elem);
          }
        } else {
          replace_load_p_reads(blk[load_idx + 1].pattern);
        }
        bool md_busy_between = false;
        for (size_t k = store_idx + 1; k < load_idx && !md_busy_between; ++k) {
          if (IsCallPattern(blk[k].pattern)) {
            md_busy_between = true;
            break;
          }
          blk[k].pattern.Visit([&](const RtlExpr& n) {
            if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
              const int nhr =
                  (n.RegNo() < kFirstPseudoRegister)
                      ? n.RegNo()
                      : (m->reg_renumber.contains(n.RegNo()) ? m->reg_renumber.at(n.RegNo()) : -1);
              if (nhr == 64 || nhr == 65) {
                md_busy_between = true;
              }
            }
          });
        }
        if (sec_p >= 0) {
          m->reg_renumber.erase(sec_p);
          m->pseudo_modes.erase(sec_p);
        }
        m->reg_renumber[spill_p] = md_busy_between ? -1 : 64;
        m->pseudo_modes[spill_p] = RtlMachineMode::kDi;
        oidx[load_idx + 1] = std::min(oidx[load_idx], oidx[load_idx + 1]);
        blk.erase(blk.begin() + static_cast<ptrdiff_t>(load_idx));
        oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(load_idx));
        blk.erase(blk.begin() + static_cast<ptrdiff_t>(i + 1),
                  blk.begin() + static_cast<ptrdiff_t>(store_idx + 1));
        oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(i + 1),
                   oidx.begin() + static_cast<ptrdiff_t>(store_idx + 1));
        bool load_p_still_referenced = false;
        for (const auto& check_blk : m->blocks) {
          for (const RtlPatternAndNotes& check_pn : check_blk) {
            if (check_pn.pattern.ContainsReg(load_p)) {
              load_p_still_referenced = true;
              break;
            }
          }
          if (load_p_still_referenced) {
            break;
          }
        }
        if (!load_p_still_referenced) {
          m->reg_renumber.erase(load_p);
          m->pseudo_modes.erase(load_p);
        }
      }
    }
  }

  // Fold adjacent `mulsidi3` `(reg:DI 64)` -> GPR output reload `(set p_gpr (reg:DI 64))`
  // and single-use `"f"` FPR constraint reloads `(set p_fpr p_gpr)` feeding an FP op.
  absl::flat_hash_map<int, int> trial_ref_counts;
  absl::flat_hash_set<int> trial_use_pseudos;
  for (const auto& blk : m->blocks) {
    for (const RtlPatternAndNotes& pn : blk) {
      if (pn.pattern.op == RtlOp::kUse && !pn.pattern.children.empty() &&
          pn.pattern.children[0].IsPseudoReg()) {
        trial_use_pseudos.insert(pn.pattern.children[0].RegNo());
      }
      pn.pattern.Visit([&](const RtlExpr& n) {
        if (n.IsPseudoReg()) {
          ++trial_ref_counts[n.RegNo()];
        }
      });
    }
  }
  for (size_t b = 0; b < m->blocks.size(); ++b) {
    auto& blk = m->blocks[b];
    auto& oidx = m->orig_idx[b];
    for (size_t i = 0; i + 1 < blk.size(); ++i) {
      if (!gp64 && blk[i].pattern.IsSet() && blk[i].pattern.SetDest().IsReg() &&
          !blk[i].pattern.SetDest().IsArgPointerReg() && !blk[i].pattern.SetDest().flags.volatil &&
          !blk[i].pattern.SetDest().flags.integrated &&
          blk[i].pattern.SetDest().mode == RtlMachineMode::kDi &&
          blk[i].pattern.SetSrc().IsConstInt() && blk[i].pattern.SetSrc().int_val == 0 &&
          blk[i + 1].pattern.IsSet() && blk[i + 1].pattern.SetDest().IsMem() &&
          blk[i + 1].pattern.SetDest().mode == RtlMachineMode::kDi &&
          blk[i + 1].pattern.SetSrc().IsReg() &&
          blk[i + 1].pattern.SetSrc().mode == RtlMachineMode::kDi &&
          blk[i + 1].pattern.SetSrc().RegNo() == blk[i].pattern.SetDest().RegNo() &&
          !blk[i + 1].pattern.SetDest().ContainsReg(blk[i].pattern.SetDest().RegNo())) {
        const int zr = blk[i].pattern.SetDest().RegNo();
        const int zhr = (zr < kFirstPseudoRegister)
                            ? zr
                            : (m->reg_renumber.contains(zr) ? m->reg_renumber[zr] : -1);
        const bool single_pair_use = zr < kFirstPseudoRegister ||
                                     (trial_ref_counts[zr] == 2 && !trial_use_pseudos.contains(zr));
        if (single_pair_use && zhr >= 2 && zhr <= 24) {
          blk[i + 1].pattern.children[1] = RtlExpr::MakeConstInt(0);
          blk[i + 1].reg_notes.erase(
              std::remove_if(blk[i + 1].reg_notes.begin(), blk[i + 1].reg_notes.end(),
                             [zr](const RtlRegNote& n) {
                               return (n.kind == RtlMachineMode::kRegDead ||
                                       n.kind == RtlMachineMode::kRegUnused) &&
                                      n.datum.IsReg() && n.datum.RegNo() == zr;
                             }),
              blk[i + 1].reg_notes.end());
          if (zr >= kFirstPseudoRegister) {
            m->reg_renumber.erase(zr);
            m->pseudo_modes.erase(zr);
          }
          blk.erase(blk.begin() + static_cast<ptrdiff_t>(i));
          oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(i));
          if (i > 0) {
            --i;
          }
          continue;
        }
      }
      if (blk[i].pattern.op == RtlOp::kParallel && !blk[i].pattern.children.empty() &&
          blk[i].pattern.children[0].IsSet() && blk[i].pattern.children[0].SetDest().IsReg() &&
          !blk[i].pattern.children[0].SetDest().IsArgPointerReg() &&
          blk[i].pattern.children[0].SetSrc().op == RtlOp::kMult) {
        const int md_r = blk[i].pattern.children[0].SetDest().RegNo();
        const int md_hr = (md_r < kFirstPseudoRegister)
                              ? md_r
                              : (m->reg_renumber.contains(md_r) ? m->reg_renumber[md_r] : -1);
        if (md_hr == 64 && blk[i + 1].pattern.IsSet() && blk[i + 1].pattern.SetDest().IsReg() &&
            !blk[i + 1].pattern.SetDest().IsArgPointerReg() &&
            blk[i + 1].pattern.SetSrc().IsReg() && !blk[i + 1].pattern.SetSrc().IsArgPointerReg() &&
            blk[i + 1].pattern.SetSrc().RegNo() == md_r) {
          const RtlExpr dst_reg = blk[i + 1].pattern.SetDest();
          const int dst_hr =
              (dst_reg.RegNo() < kFirstPseudoRegister)
                  ? dst_reg.RegNo()
                  : (m->reg_renumber.contains(dst_reg.RegNo()) ? m->reg_renumber[dst_reg.RegNo()]
                                                               : -1);
          const bool next_stores_dst_to_mem =
              (i + 2 < blk.size() && blk[i + 2].pattern.IsSet() &&
               blk[i + 2].pattern.SetDest().IsMem() &&
               blk[i + 2].pattern.SetSrc().ContainsReg(dst_reg.RegNo()));
          if (!next_stores_dst_to_mem && (dst_reg.IsPseudoReg() || (dst_hr >= 2 && dst_hr <= 25))) {
            blk[i].pattern.children[0].children[0] = dst_reg;
            if (md_r >= kFirstPseudoRegister) {
              m->reg_renumber.erase(md_r);
              m->pseudo_modes.erase(md_r);
            }
            blk.erase(blk.begin() + static_cast<ptrdiff_t>(i + 1));
            oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(i + 1));
            continue;
          }
        }
      }
      if (i + 2 < blk.size() && blk[i].pattern.op == RtlOp::kParallel &&
          blk[i].pattern.children.size() == 3 && blk[i].pattern.children[0].IsSet() &&
          blk[i].pattern.children[1].op == RtlOp::kClobber &&
          blk[i].pattern.children[2].op == RtlOp::kClobber &&
          blk[i].pattern.children[0].SetDest().IsReg() &&
          !blk[i].pattern.children[0].SetDest().IsArgPointerReg() &&
          blk[i].pattern.children[0].SetDest().mode == RtlMachineMode::kSi) {
        const int md_r = blk[i].pattern.children[0].SetDest().RegNo();
        const int md_hr = (md_r < kFirstPseudoRegister)
                              ? md_r
                              : (m->reg_renumber.contains(md_r) ? m->reg_renumber[md_r] : -1);
        const RtlOp op0 = blk[i].pattern.children[0].SetSrc().op;
        if ((md_hr == 64 && op0 == RtlOp::kTruncate) || (md_hr == 65 && op0 == RtlOp::kMult)) {
          size_t mv_idx = blk.size();
          for (size_t k = i + 1; k < blk.size(); ++k) {
            const RtlExpr& pk = blk[k].pattern;
            if (pk.IsSet() && pk.SetDest().IsPseudoReg() &&
                pk.SetDest().mode == RtlMachineMode::kSi && pk.SetSrc().IsReg() &&
                !pk.SetSrc().IsArgPointerReg() && pk.SetSrc().RegNo() == md_r) {
              mv_idx = k;
              break;
            }
            bool touches_md = false;
            pk.Visit([&](const RtlExpr& n) {
              if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                const int nhr =
                    (n.RegNo() < kFirstPseudoRegister)
                        ? n.RegNo()
                        : (m->reg_renumber.contains(n.RegNo()) ? m->reg_renumber.at(n.RegNo())
                                                               : -1);
                if (nhr == 64 || nhr == 65) {
                  touches_md = true;
                }
              }
            });
            if (IsControlTransferPattern(pk) || IsCallPattern(pk) || touches_md) {
              break;
            }
          }
          if (mv_idx < blk.size()) {
            const int p_scr = blk[mv_idx].pattern.SetDest().RegNo();
            const int scr_hr = m->reg_renumber.contains(p_scr) ? m->reg_renumber[p_scr] : -1;
            if (scr_hr >= 2 && scr_hr <= 25 && !trial_use_pseudos.contains(p_scr) &&
                trial_ref_counts[p_scr] == 2) {
              size_t u_idx = blk.size();
              for (size_t k = mv_idx + 1; k < blk.size(); ++k) {
                if (blk[k].pattern.ContainsReg(p_scr)) {
                  u_idx = k;
                  break;
                }
                if (IsControlTransferPattern(blk[k].pattern) || IsCallPattern(blk[k].pattern)) {
                  break;
                }
              }
              if (u_idx < blk.size() && blk[u_idx].pattern.IsSet() &&
                  !IsControlTransferPattern(blk[u_idx].pattern)) {
                const RtlExpr& up = blk[u_idx].pattern;
                const RtlOp uop = up.SetSrc().op;
                const bool non_fp = up.SetDest().mode != RtlMachineMode::kSf &&
                                    up.SetDest().mode != RtlMachineMode::kDf &&
                                    up.SetSrc().mode != RtlMachineMode::kSf &&
                                    up.SetSrc().mode != RtlMachineMode::kDf;
                const bool elig =
                    (uop == RtlOp::kFloat && up.SetDest().IsReg()) ||
                    (non_fp && up.SetDest().IsReg() && uop != RtlOp::kReg && uop != RtlOp::kMem &&
                     uop != RtlOp::kDiv && uop != RtlOp::kUdiv && uop != RtlOp::kMod &&
                     uop != RtlOp::kUmod && uop != RtlOp::kMult && uop != RtlOp::kFloat &&
                     uop != RtlOp::kUnsignedFloat);
                if (elig) {
                  const RtlFlags md_flags = blk[i].pattern.children[0].SetDest().flags;
                  m->reg_renumber[p_scr] = md_hr;
                  m->pseudo_modes[p_scr] = RtlMachineMode::kSi;
                  if (md_r >= kFirstPseudoRegister && md_r != p_scr) {
                    m->reg_renumber.erase(md_r);
                    m->pseudo_modes.erase(md_r);
                  }
                  blk[i].pattern.children[0].children[0] =
                      RtlExpr::MakeReg(RtlMachineMode::kSi, p_scr, md_flags);
                  blk.erase(blk.begin() + static_cast<ptrdiff_t>(mv_idx));
                  oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(mv_idx));
                  continue;
                }
              }
            }
          }
        }
      }
      if (blk[i].pattern.IsSet() && blk[i].pattern.SetDest().IsPseudoReg() &&
          (blk[i].pattern.SetDest().mode == RtlMachineMode::kSf ||
           blk[i].pattern.SetDest().mode == RtlMachineMode::kDf) &&
          blk[i].pattern.SetSrc().IsReg() && !blk[i].pattern.SetSrc().IsArgPointerReg()) {
        const int p_fpr = blk[i].pattern.SetDest().RegNo();
        const int src_r = blk[i].pattern.SetSrc().RegNo();
        const int fpr_hr = m->reg_renumber.contains(p_fpr) ? m->reg_renumber[p_fpr] : -1;
        const int src_hr = (src_r < kFirstPseudoRegister)
                               ? src_r
                               : (m->reg_renumber.contains(src_r) ? m->reg_renumber[src_r] : -1);
        if (fpr_hr >= 32 && fpr_hr < 64 && src_hr >= 2 && src_hr < 32 &&
            !trial_use_pseudos.contains(p_fpr) && trial_ref_counts[p_fpr] == 2) {
          size_t op_idx = blk.size();
          for (size_t k = i + 1; k < std::min(blk.size(), i + 3); ++k) {
            if (blk[k].pattern.ContainsReg(p_fpr)) {
              const RtlExpr& kpat = blk[k].pattern;
              const bool is_plain_move = kpat.IsSet() && (kpat.SetSrc().op == RtlOp::kReg ||
                                                          kpat.SetSrc().op == RtlOp::kSubreg);
              if (!is_plain_move && !(kpat.IsSet() && kpat.SetDest().ContainsReg(p_fpr))) {
                op_idx = k;
              }
              break;
            }
          }
          if (op_idx < blk.size()) {
            const RtlExpr src_copy = blk[i].pattern.SetSrc();
            blk[op_idx].pattern.Mutate([&](RtlExpr& n) {
              if (n.IsPseudoReg() && n.RegNo() == p_fpr) {
                n = src_copy;
              }
            });
            m->reg_renumber.erase(p_fpr);
            m->pseudo_modes.erase(p_fpr);
            oidx[op_idx] = std::min(oidx[i], oidx[op_idx]);
            blk.erase(blk.begin() + static_cast<ptrdiff_t>(i));
            oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(i));
            if (i > 0) {
              --i;
            }
          }
        }
      }
    }
  }
}

// Builds a temporary post-prologue trial pseudo model strictly for running
// `SimulateO0StupidAlloc` to decide which hard-register webs require
// `obey_regdecls` `(use (reg:M hr))` scope markers.
TrialO0Model BuildTrialO0PseudoModel(
    const RtlControlFlowGraph& cfg, const O0PrologueInfo& pinfo, bool gp64, bool fp64,
    const std::vector<std::vector<RtlPatternAndNotes>>& all_blocks) {
  TrialO0Model m;
  m.blocks.resize(all_blocks.size());
  m.orig_idx.resize(all_blocks.size());
  for (size_t b = 0; b < all_blocks.size(); ++b) {
    const size_t start_i = (b == 0) ? pinfo.prologue_end : 0;
    for (size_t i = start_i; i < all_blocks[b].size(); ++i) {
      m.blocks[b].push_back(all_blocks[b][i]);
      m.orig_idx[b].push_back(i);
    }
  }

  // Strip `-O0` `PARALLEL` `match_scratch` clobbers (`movstrsi`, `fix_trunc*`,
  // `ffs*`, `mul*`/`div*`/`mod*`) that were `(clobber (scratch:...))` during
  // `stupid_life_analysis`.
  for (auto& blk : m.blocks) {
    for (RtlPatternAndNotes& insn : blk) {
      if (IsO0ParallelScratchClobberInsn(insn.pattern)) {
        for (size_t k = 1; k < insn.pattern.children.size(); ++k) {
          RtlExpr& cl = insn.pattern.children[k];
          if (cl.op == RtlOp::kClobber && cl.children.size() == 1 && cl.children[0].IsHardReg()) {
            cl = RtlExpr::MakeClobber(RtlExpr::MakeScratch(cl.children[0].mode));
          }
        }
      }
    }
  }
  WrapTrialO0DiSubregs(/*prologue_end=*/0, &m.blocks);

  // Partition non-ABI hard-register occurrences into connected def-use webs via
  // forward reaching definitions across `cfg.rpo_blocks`.
  struct Occ {
    int hr = -1;
    RtlMachineMode mode = RtlMachineMode::kSi;
    int span = 1;
    bool pinned = false;
    bool is_set_dest = false;
    bool is_clobber = false;
  };
  std::vector<Occ> occs;
  absl::flat_hash_map<const RtlExpr*, int> expr_to_occ;
  auto is_cand_hr = [&](const RtlExpr& e) {
    if (!e.IsHardReg() || e.flags.integrated) {
      return false;
    }
    const int r = e.RegNo();
    return (r >= 2 && r <= 25) || (r >= 32 && r <= 66);
  };
  auto reg_pat_occs = [&](const auto& self, const RtlExpr& e, bool in_dst, bool in_clob) -> void {
    if (is_cand_hr(e)) {
      const int id = static_cast<int>(occs.size());
      occs.push_back(Occ{
          .hr = e.RegNo(),
          .mode = e.mode,
          .span = std::max(1, MipsHardRegNregs(e.RegNo(), e.mode, gp64, fp64)),
          .is_set_dest = (!in_clob && in_dst),
          .is_clobber = in_clob,
      });
      expr_to_occ.emplace(&e, id);
      return;
    }
    if (e.IsSet()) {
      self(self, e.children[0], true, false);
      self(self, e.children[1], false, false);
      return;
    }
    if (e.op == RtlOp::kClobber && !e.children.empty()) {
      self(self, e.children[0], true, true);
      return;
    }
    if (e.op == RtlOp::kSubreg && !e.children.empty()) {
      self(self, e.children[0], in_dst, in_clob);
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch, false, false);
    }
  };
  for (const auto& blk : m.blocks) {
    for (const RtlPatternAndNotes& pn : blk) {
      reg_pat_occs(reg_pat_occs, pn.pattern, false, false);
      for (const RtlExpr& fu : pn.call_fusage) {
        reg_pat_occs(reg_pat_occs, fu, false, false);
      }
    }
  }

  std::vector<int> parent(occs.size());
  for (size_t i = 0; i < occs.size(); ++i) {
    parent[i] = static_cast<int>(i);
  }
  auto uf_find = [&](int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  auto uf_union = [&](int a, int b) {
    a = uf_find(a);
    b = uf_find(b);
    if (a != b) {
      parent[b] = a;
    }
  };
  std::vector<std::vector<int>> pin_adj(occs.size());
  auto add_pin_edge = [&](int a, int b) {
    pin_adj[a].push_back(b);
    pin_adj[b].push_back(a);
  };

  constexpr int kPinnedTok = -1;
  constexpr int kUninitTok = -2;
  using Reach = std::array<std::vector<int>, kFirstPseudoRegister>;
  std::vector<Reach> reach_in(m.blocks.size());
  if (!m.blocks.empty()) {
    for (int w = 0; w < kFirstPseudoRegister; ++w) {
      if (IsO0OutgoingCallArgHardReg(w)) {
        reach_in[0][w].push_back(kPinnedTok);
      } else if ((w >= 2 && w <= 25) || (w >= 32 && w <= 66)) {
        reach_in[0][w].push_back(kUninitTok);
      }
    }
  }
  auto merge_reach = [](const Reach& src, Reach* dst) {
    bool ch = false;
    for (int w = 0; w < kFirstPseudoRegister; ++w) {
      for (int t : src[w]) {
        if (std::find((*dst)[w].begin(), (*dst)[w].end(), t) == (*dst)[w].end()) {
          (*dst)[w].push_back(t);
          ch = true;
        }
      }
    }
    return ch;
  };

  auto step_block = [&](size_t b, Reach* st, bool collect) {
    bool prev_was_fix_di_copy = false;
    int prev_fix_di_hr = -1;
    bool seen_di_subreg_in_blk[kFirstPseudoRegister] = {};
    for (size_t i = 0; i < m.blocks[b].size(); ++i) {
      const RtlPatternAndNotes& pn = m.blocks[b][i];
      const RtlExpr& pat = pn.pattern;
      if (IsCallPattern(pat)) {
        auto step_read = [&](const RtlExpr& e) {
          e.Visit([&](const RtlExpr& n) {
            if (auto it = expr_to_occ.find(&n); it != expr_to_occ.end() && collect) {
              const Occ& o = occs[it->second];
              for (int w = o.hr; w < std::min(kFirstPseudoRegister, o.hr + o.span); ++w) {
                for (int t : (*st)[w]) {
                  if (t == kPinnedTok) {
                    occs[it->second].pinned = true;
                  } else if (t >= 0) {
                    add_pin_edge(t, it->second);
                    if (occs[t].hr == o.hr && occs[t].mode == o.mode) {
                      uf_union(t, it->second);
                    } else {
                      occs[t].pinned = true;
                      occs[it->second].pinned = true;
                    }
                  }
                }
              }
            }
          });
        };
        pat.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kCall && !n.children.empty()) {
            step_read(n.children[0]);
          }
        });
        for (const RtlExpr& fu : pn.call_fusage) {
          fu.Visit([&](const RtlExpr& n) {
            if (auto it = expr_to_occ.find(&n); it != expr_to_occ.end() && collect) {
              occs[it->second].pinned = true;
              const Occ& o = occs[it->second];
              for (int w = o.hr; w < std::min(kFirstPseudoRegister, o.hr + o.span); ++w) {
                for (int t : (*st)[w]) {
                  if (t >= 0) {
                    occs[t].pinned = true;
                  }
                }
              }
            }
          });
        }
        for (int h = 0; h < kFirstPseudoRegister; ++h) {
          if (IsMipsO0CallUsedHardReg(h)) {
            (*st)[h].clear();
          }
        }
        const RtlExpr* cset =
            pat.IsSet() ? &pat
            : (pat.op == RtlOp::kParallel && !pat.children.empty() && pat.children[0].IsSet())
                ? &pat.children[0]
                : nullptr;
        if (cset != nullptr && cset->SetDest().IsHardReg()) {
          if (auto it = expr_to_occ.find(&cset->SetDest()); it != expr_to_occ.end() && collect) {
            occs[it->second].pinned = true;
          }
          const int r = cset->SetDest().RegNo();
          const int nw = std::max(1, MipsHardRegNregs(r, cset->SetDest().mode, gp64, fp64));
          for (int w = r; w < std::min(kFirstPseudoRegister, r + nw); ++w) {
            (*st)[w] = {kPinnedTok};
          }
          prev_was_fix_di_copy = false;
          if (IsO0FixDiLibcall(*cset) && i + 1 < m.blocks[b].size() &&
              m.blocks[b][i + 1].pattern.IsSet() &&
              m.blocks[b][i + 1].pattern.SetDest().IsHardReg() &&
              m.blocks[b][i + 1].pattern.SetSrc().IsHardReg() &&
              m.blocks[b][i + 1].pattern.SetSrc().RegNo() == 2) {
            prev_fix_di_hr = m.blocks[b][i + 1].pattern.SetDest().RegNo();
          } else {
            prev_fix_di_hr = -1;
          }
        }
        continue;
      }

      auto step_reads = [&](const auto& self, const RtlExpr& e) -> void {
        if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].IsHardReg()) {
          const RtlExpr& in = e.children[0];
          if (auto it = expr_to_occ.find(&in); it != expr_to_occ.end() && collect) {
            const Occ& o = occs[it->second];
            const int w0 = o.hr + std::max(0, static_cast<int>(e.int_val));
            const int snw = std::max(1, MipsHardRegNregs(w0, e.mode, gp64, fp64));
            for (int w = w0; w < std::min(kFirstPseudoRegister, w0 + snw); ++w) {
              for (int t : (*st)[w]) {
                if (t == kPinnedTok) {
                  occs[it->second].pinned = true;
                } else if (t >= 0) {
                  add_pin_edge(t, it->second);
                  if (occs[t].hr == o.hr && occs[t].mode == o.mode) {
                    uf_union(t, it->second);
                  } else {
                    occs[t].pinned = true;
                    occs[it->second].pinned = true;
                  }
                }
              }
            }
          }
          return;
        }
        if (auto it = expr_to_occ.find(&e); it != expr_to_occ.end()) {
          if (collect) {
            const Occ& o = occs[it->second];
            for (int w = o.hr; w < std::min(kFirstPseudoRegister, o.hr + o.span); ++w) {
              for (int t : (*st)[w]) {
                if (t == kPinnedTok) {
                  occs[it->second].pinned = true;
                } else if (t >= 0) {
                  add_pin_edge(t, it->second);
                  if (occs[t].hr == o.hr && occs[t].mode == o.mode) {
                    uf_union(t, it->second);
                  } else {
                    occs[t].pinned = true;
                    occs[it->second].pinned = true;
                  }
                }
              }
            }
          }
          return;
        }
        for (const RtlExpr& ch : e.children) {
          self(self, ch);
        }
      };

      auto step_write = [&](const RtlExpr& dst, bool unify_self_move, int self_src_occ) {
        if (dst.op == RtlOp::kSubreg && !dst.children.empty() && dst.children[0].IsHardReg()) {
          const RtlExpr& in = dst.children[0];
          if (in.mode == RtlMachineMode::kDi && in.RegNo() >= 0 &&
              in.RegNo() < kFirstPseudoRegister) {
            seen_di_subreg_in_blk[in.RegNo()] = true;
          }
          const int w0 = in.RegNo() + std::max(0, static_cast<int>(dst.int_val));
          const int snw = std::max(1, MipsHardRegNregs(w0, dst.mode, gp64, fp64));
          if (auto it = expr_to_occ.find(&in); it != expr_to_occ.end() && !in.flags.integrated) {
            if (collect) {
              for (int w = w0; w < std::min(kFirstPseudoRegister, w0 + snw); ++w) {
                for (int t : (*st)[w]) {
                  if (t >= 0 && occs[t].hr == occs[it->second].hr &&
                      occs[t].mode == occs[it->second].mode) {
                    uf_union(t, it->second);
                  }
                }
              }
            }
            for (int w = w0; w < std::min(kFirstPseudoRegister, w0 + snw); ++w) {
              (*st)[w] = {it->second};
            }
          } else {
            for (int w = w0; w < std::min(kFirstPseudoRegister, w0 + snw); ++w) {
              (*st)[w] = {kPinnedTok};
            }
          }
          return;
        }
        if (dst.IsHardReg()) {
          const int r = dst.RegNo();
          const int nw = std::max(1, MipsHardRegNregs(r, dst.mode, gp64, fp64));
          if (auto it = expr_to_occ.find(&dst); it != expr_to_occ.end() && !dst.flags.integrated) {
            if (collect) {
              if (unify_self_move && self_src_occ >= 0) {
                uf_union(self_src_occ, it->second);
              }
              for (int w = r; w < std::min(kFirstPseudoRegister, r + nw); ++w) {
                for (int t : (*st)[w]) {
                  if (t >= 0 && occs[t].is_clobber && occs[t].hr == occs[it->second].hr &&
                      occs[t].mode == occs[it->second].mode) {
                    uf_union(t, it->second);
                  }
                }
              }
            }
            for (int w = r; w < std::min(kFirstPseudoRegister, r + nw); ++w) {
              (*st)[w] = {it->second};
            }
          } else if (r >= 0 && r < kFirstPseudoRegister) {
            for (int w = r; w < std::min(kFirstPseudoRegister, r + nw); ++w) {
              (*st)[w] = {kPinnedTok};
            }
          }
          return;
        }
        step_reads(step_reads, dst);
      };

      auto handle_elem = [&](const RtlExpr& elem, bool top_clobber) {
        if (elem.IsSet()) {
          const bool is_di_self_move = elem.SetDest().IsHardReg() && elem.SetSrc().IsHardReg() &&
                                       elem.SetDest().mode == RtlMachineMode::kDi &&
                                       elem.SetSrc().mode == RtlMachineMode::kDi &&
                                       elem.SetDest().RegNo() == elem.SetSrc().RegNo() &&
                                       !elem.SetDest().flags.integrated;
          const bool unify_di_self =
              is_di_self_move &&
              (seen_di_subreg_in_blk[elem.SetDest().RegNo()] ||
               (prev_was_fix_di_copy && elem.SetDest().RegNo() == prev_fix_di_hr));
          int self_src_occ = -1;
          if (unify_di_self) {
            if (auto it = expr_to_occ.find(&elem.SetSrc()); it != expr_to_occ.end()) {
              self_src_occ = it->second;
            }
          }
          step_reads(step_reads, elem.SetSrc());
          if (!elem.SetDest().IsHardReg() &&
              !(elem.SetDest().op == RtlOp::kSubreg && !elem.SetDest().children.empty() &&
                elem.SetDest().children[0].IsHardReg())) {
            step_reads(step_reads, elem.SetDest());
          }
          const bool is_this_fix_di_copy =
              prev_fix_di_hr >= 0 && !prev_was_fix_di_copy && elem.SetDest().IsHardReg() &&
              elem.SetDest().RegNo() == prev_fix_di_hr && elem.SetSrc().IsHardReg() &&
              elem.SetSrc().RegNo() == 2 && elem.SetDest().mode == RtlMachineMode::kDi;
          step_write(elem.SetDest(), unify_di_self, self_src_occ);
          prev_was_fix_di_copy = is_this_fix_di_copy;
          if (!is_this_fix_di_copy) {
            prev_fix_di_hr = -1;
          }
          if (unify_di_self) {
            seen_di_subreg_in_blk[elem.SetDest().RegNo()] = false;
          }
        } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
          prev_was_fix_di_copy = false;
          prev_fix_di_hr = -1;
          if (top_clobber && elem.children[0].IsHardReg()) {
            if (auto it = expr_to_occ.find(&elem.children[0]); it != expr_to_occ.end()) {
              const int r = elem.children[0].RegNo();
              const int nw = std::max(1, MipsHardRegNregs(r, elem.children[0].mode, gp64, fp64));
              for (int w = r; w < std::min(kFirstPseudoRegister, r + nw); ++w) {
                (*st)[w] = {it->second};
              }
            }
          }
        } else {
          prev_was_fix_di_copy = false;
          prev_fix_di_hr = -1;
          step_reads(step_reads, elem);
        }
      };

      if (pat.op == RtlOp::kParallel) {
        for (const RtlExpr& ch : pat.children) {
          handle_elem(ch, false);
        }
      } else {
        handle_elem(pat, true);
      }
    }
  };

  bool ch_df = true;
  while (ch_df) {
    ch_df = false;
    for (size_t b = 0; b < m.blocks.size(); ++b) {
      Reach st = reach_in[b];
      step_block(b, &st, false);
      for (size_t succ : cfg.rpo_blocks[b].successors) {
        if (succ < m.blocks.size() && merge_reach(st, &reach_in[succ])) {
          ch_df = true;
        }
      }
      for (size_t succ : cfg.rpo_blocks[b].addr_vec_targets) {
        if (succ < m.blocks.size() && merge_reach(st, &reach_in[succ])) {
          ch_df = true;
        }
      }
    }
  }
  for (size_t b = 0; b < m.blocks.size(); ++b) {
    Reach st = reach_in[b];
    step_block(b, &st, true);
  }

  // Coalesce intra-block same-`hr` RMW `(set hr (... hr ...))` webs under
  // `-O0` `stupid_life_analysis`'s non-overlap SUID condition.
  std::vector<std::vector<int>> insn_suid(m.blocks.size());
  absl::flat_hash_map<int, int> stupid_max_suid;
  absl::flat_hash_map<int, absl::flat_hash_set<int>> stupid_set_suids;
  int build_suid = 0;
  for (size_t b = 0; b < m.blocks.size(); ++b) {
    if (b > 0) {
      build_suid += 2;
    }
    insn_suid[b].resize(m.blocks[b].size(), 0);
    for (size_t i = 0; i < m.blocks[b].size(); ++i) {
      const RtlPatternAndNotes& pn = m.blocks[b][i];
      ++build_suid;
      insn_suid[b][i] = build_suid;
      if (pn.pattern.op == RtlOp::kNote) {
        continue;
      }
      const int s = build_suid;
      pn.pattern.Visit([&](const RtlExpr& n) {
        if (auto it = expr_to_occ.find(&n); it != expr_to_occ.end()) {
          int& mx = stupid_max_suid[uf_find(it->second)];
          mx = std::max(mx, s);
        }
      });
      auto mark_set_suid = [&](const RtlExpr& elem) {
        if (elem.IsSet()) {
          const RtlExpr& d = elem.SetDest();
          const RtlExpr* key =
              (d.op == RtlOp::kSubreg && !d.children.empty()) ? &d.children[0] : &d;
          if (auto dit = expr_to_occ.find(key); dit != expr_to_occ.end()) {
            stupid_set_suids[uf_find(dit->second)].insert(s);
          }
        }
      };
      if (pn.pattern.IsSet()) {
        mark_set_suid(pn.pattern);
      } else if (pn.pattern.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : pn.pattern.children) {
          mark_set_suid(elem);
        }
      }
    }
  }
  for (size_t b = 0; b < m.blocks.size(); ++b) {
    for (size_t i = 0; i < m.blocks[b].size(); ++i) {
      const RtlPatternAndNotes& pn = m.blocks[b][i];
      if (pn.pattern.op == RtlOp::kNote || pn.pattern.IsCallPattern()) {
        continue;
      }
      const int cur_suid = insn_suid[b][i];
      const RtlExpr* s = pn.pattern.IsSet() ? &pn.pattern
                         : (pn.pattern.op == RtlOp::kParallel && !pn.pattern.children.empty() &&
                            pn.pattern.children[0].IsSet())
                             ? &pn.pattern.children[0]
                             : nullptr;
      if (s == nullptr || !s->SetDest().IsHardReg() ||
          (pn.pattern.op == RtlOp::kSet && s->SetSrc().IsHardReg())) {
        continue;
      }
      auto dit = expr_to_occ.find(&s->SetDest());
      if (dit == expr_to_occ.end()) {
        continue;
      }
      auto walk_rmw = [&](const auto& self, const RtlExpr& n, bool inside_mem) -> void {
        if (n.op == RtlOp::kMem) {
          for (const RtlExpr& ch : n.children) {
            self(self, ch, /*inside_mem=*/true);
          }
          return;
        }
        if (auto sit = expr_to_occ.find(&n); sit != expr_to_occ.end()) {
          if (!inside_mem && occs[sit->second].hr == occs[dit->second].hr &&
              occs[sit->second].mode == occs[dit->second].mode) {
            const int root_r = uf_find(sit->second);
            const int root_w = uf_find(dit->second);
            bool has_dead = false;
            for (const RtlRegNote& note : pn.reg_notes) {
              if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg() &&
                  note.datum.RegNo() == occs[sit->second].hr) {
                has_dead = true;
                break;
              }
            }
            if (!has_dead && root_r != root_w) {
              const int max_s = stupid_max_suid[root_r];
              const auto sets_it = stupid_set_suids.find(root_r);
              const bool born_at_prev_insn =
                  sets_it != stupid_set_suids.end() && sets_it->second.contains(cur_suid - 1);
              if (max_s > cur_suid || born_at_prev_insn) {
                const int old_max = std::max(stupid_max_suid[root_r], stupid_max_suid[root_w]);
                absl::flat_hash_set<int> merged_sets = stupid_set_suids[root_w];
                if (sets_it != stupid_set_suids.end()) {
                  merged_sets.insert(sets_it->second.begin(), sets_it->second.end());
                }
                uf_union(sit->second, dit->second);
                const int new_root = uf_find(dit->second);
                stupid_max_suid[new_root] = old_max;
                stupid_set_suids[new_root] = std::move(merged_sets);
              }
            }
          }
          return;
        }
        for (const RtlExpr& ch : n.children) {
          self(self, ch, inside_mem);
        }
      };
      walk_rmw(walk_rmw, s->SetSrc(), /*inside_mem=*/false);
    }
  }

  // Propagate `pinned` across union-find roots and `pin_adj`.
  absl::flat_hash_map<int, std::vector<int>> members;
  for (size_t i = 0; i < occs.size(); ++i) {
    members[uf_find(static_cast<int>(i))].push_back(static_cast<int>(i));
  }
  std::vector<int> q;
  std::vector<bool> pinned(occs.size(), false);
  for (size_t i = 0; i < occs.size(); ++i) {
    if (occs[i].pinned) {
      pinned[i] = true;
      q.push_back(static_cast<int>(i));
    }
  }
  for (size_t h = 0; h < q.size(); ++h) {
    const int u = q[h];
    for (int mem : members[uf_find(u)]) {
      if (!pinned[mem]) {
        pinned[mem] = true;
        q.push_back(mem);
      }
    }
    for (int v : pin_adj[u]) {
      if (!pinned[v]) {
        pinned[v] = true;
        q.push_back(v);
      }
    }
  }

  struct Comp {
    int root = 0;
    int hr = 0;
    RtlMachineMode mode = RtlMachineMode::kSi;
    bool has_set = false;
  };
  std::vector<Comp> comps;
  absl::flat_hash_map<int, size_t> root_idx;
  for (size_t i = 0; i < occs.size(); ++i) {
    if (pinned[i]) {
      continue;
    }
    const int r = uf_find(static_cast<int>(i));
    auto [it, ins] = root_idx.try_emplace(r, comps.size());
    if (ins) {
      comps.push_back(Comp{.root = r, .hr = occs[i].hr, .mode = occs[i].mode});
    }
    if (occs[i].is_set_dest) {
      comps[it->second].has_set = true;
    }
  }
  std::stable_sort(comps.begin(), comps.end(),
                   [](const Comp& a, const Comp& b) { return a.hr < b.hr; });
  absl::flat_hash_map<int, int> root_to_pseudo;
  int next_p = 72;
  for (const Comp& c : comps) {
    if (!c.has_set) {
      continue;
    }
    const int p = next_p++;
    root_to_pseudo[c.root] = p;
    m.reg_renumber[p] = c.hr;
    m.pseudo_modes[p] = c.mode;
  }

  auto rewrite_expr = [&](const auto& self, RtlExpr* orig_e, RtlExpr* dst_e) -> void {
    if (auto it = expr_to_occ.find(orig_e); it != expr_to_occ.end()) {
      const int r = uf_find(it->second);
      if (auto pit = root_to_pseudo.find(r); pit != root_to_pseudo.end()) {
        *dst_e = RtlExpr::MakeReg(dst_e->mode, pit->second, dst_e->flags);
        return;
      }
    }
    for (size_t k = 0; k < orig_e->children.size() && k < dst_e->children.size(); ++k) {
      self(self, &orig_e->children[k], &dst_e->children[k]);
    }
  };
  std::vector<std::vector<RtlPatternAndNotes>> rewritten = m.blocks;
  for (size_t b = 0; b < m.blocks.size(); ++b) {
    for (size_t i = 0; i < m.blocks[b].size(); ++i) {
      rewrite_expr(rewrite_expr, &m.blocks[b][i].pattern, &rewritten[b][i].pattern);
    }
  }
  m.blocks = std::move(rewritten);

  // Fold `-O0` `mulsidi3` top-of-frame 8-byte spill and `=x` / `"f"` constraint
  // reload scratches in the trial model so `SimulateO0StupidAlloc` sees the exact
  // pre-reload live spans.
  FoldTrialO0ReloadScratchesAndMulsidi3Spills(pinfo, gp64, &next_p, &m);

  return m;
}

O0StupidTimeline BuildO0StupidTimeline(bool gp64, bool fp64, const TrialO0Model& model,
                                       size_t b0_param_prefix_end,
                                       const absl::flat_hash_set<int>& b0_entry_param_cands) {
  O0StupidTimeline tl;
  for (const auto& [p, hr] : model.reg_renumber) {
    if (hr >= 0) {
      O0PseudoSuidSpan sp;
      if (auto it = model.pseudo_modes.find(p); it != model.pseudo_modes.end()) {
        sp.mode = it->second;
      }
      sp.is_entry_param = b0_entry_param_cands.contains(p);
      tl.spans.emplace(p, sp);
    }
  }
  std::vector<std::vector<int>> insn_suids(model.blocks.size());
  int cur_suid = 1;
  for (size_t b = 0; b < model.blocks.size(); ++b) {
    if (b > 0) {
      cur_suid += 2;
    }
    insn_suids[b].resize(model.blocks[b].size());
    for (size_t i = 0; i < model.blocks[b].size(); ++i) {
      if (b == 0 && i == b0_param_prefix_end) {
        tl.b0_prefix_suid = cur_suid;
      }
      const int s = cur_suid++;
      insn_suids[b][i] = s;
      const RtlExpr& pat = model.blocks[b][i].pattern;
      if (IsCallPattern(pat)) {
        tl.call_suids.push_back(s);
      }
      if (pat.op == RtlOp::kUse && pat.children.size() == 1 && pat.children[0].IsPseudoReg()) {
        if (auto it = tl.spans.find(pat.children[0].RegNo()); it != tl.spans.end()) {
          it->second.has_explicit_use_insn = true;
        }
      }
    }
    if (b == 0 && b0_param_prefix_end >= model.blocks[0].size()) {
      tl.b0_prefix_suid = cur_suid;
    }
  }
  tl.exit_suid = cur_suid;
  tl.after_hr.assign(static_cast<size_t>(tl.exit_suid + 3), std::array<bool, 68>{});

  std::array<bool, 68> regs_live{};
  int last_call_suid = tl.exit_suid + 10;
  auto mark_refs = [&](const auto& self, const RtlExpr& expr, int suid, size_t b,
                       size_t i) -> void {
    if (expr.op == RtlOp::kSet || expr.op == RtlOp::kClobber) {
      if (!expr.children.empty() && expr.children[0].op == RtlOp::kReg &&
          !expr.children[0].IsArgPointerReg()) {
        const int r = expr.children[0].RegNo();
        if (r >= 0 && r < 68) {
          const int nw = std::max(1, MipsHardRegNregs(r, expr.children[0].mode, gp64, fp64));
          for (int j = 0; j < nw && r + j < 68; ++j) {
            regs_live[r + j] = false;
            tl.after_hr[suid][r + j] = true;
            if (expr.op == RtlOp::kClobber && suid > 0) {
              tl.after_hr[suid - 1][r + j] = true;
            }
          }
        } else if (auto it = tl.spans.find(r); it != tl.spans.end()) {
          const int wb = suid - (expr.op == RtlOp::kClobber ? 1 : 0);
          it->second.born = wb;
          it->second.birth_b = b;
          it->second.birth_i = i;
          if (it->second.dead < wb + 2) {
            it->second.dead = wb + 2;
            it->second.live_flag = true;
          }
          ++it->second.n_refs;
          if (last_call_suid < it->second.dead) {
            ++it->second.n_calls;
          }
        }
      }
      if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
        self(self, expr.children[1], suid, b, i);
        if (expr.children[0].op != RtlOp::kReg) {
          self(self, expr.children[0], suid, b, i);
        }
      }
      return;
    }
    if (expr.op == RtlOp::kSubreg && !expr.children.empty()) {
      if (expr.children[0].IsPseudoReg() &&
          ModeByteSize(expr.mode) != ModeByteSize(expr.children[0].mode)) {
        if (auto it = tl.spans.find(expr.children[0].RegNo()); it != tl.spans.end()) {
          it->second.changes_size = true;
        }
      }
      self(self, expr.children[0], suid, b, i);
      return;
    }
    if (expr.op == RtlOp::kReg) {
      if (!expr.IsArgPointerReg()) {
        const int r = expr.RegNo();
        if (r >= 0 && r < 68) {
          const int nw = std::max(1, MipsHardRegNregs(r, expr.mode, gp64, fp64));
          for (int j = 0; j < nw && r + j < 68; ++j) {
            regs_live[r + j] = true;
          }
        } else if (auto it = tl.spans.find(r); it != tl.spans.end()) {
          it->second.born = suid;
          it->second.birth_b = b;
          it->second.birth_i = i;
          ++it->second.n_refs;
          if (!it->second.live_flag) {
            it->second.live_flag = true;
            it->second.dead = suid;
          }
        }
      }
      return;
    }
    for (size_t k = expr.children.size(); k-- > 0;) {
      self(self, expr.children[k], suid, b, i);
    }
  };

  for (size_t b = model.blocks.size(); b-- > 0;) {
    for (size_t i = model.blocks[b].size(); i-- > 0;) {
      const int suid = insn_suids[b][i];
      tl.after_hr[suid] = regs_live;
      const RtlPatternAndNotes& pn = model.blocks[b][i];
      mark_refs(mark_refs, pn.pattern, suid, b, i);
      if (IsCallPattern(pn.pattern)) {
        last_call_suid = suid;
        for (int h = 0; h < 68; ++h) {
          if (IsMipsO0CallUsedHardReg(h)) {
            tl.after_hr[suid][h] = true;
            regs_live[h] = false;
          }
        }
        for (size_t k = pn.call_fusage.size(); k-- > 0;) {
          mark_refs(mark_refs, pn.call_fusage[k], suid, b, i);
        }
      }
      if (i == 0 && b > 0 && suid >= 2) {
        tl.after_hr[suid - 1] = regs_live;
        tl.after_hr[suid - 2] = regs_live;
      }
    }
  }
  return tl;
}

void CoalesceTrialExhaustedCalleeSavedFprs(TrialO0Model* model) {
  bool has_fp_in_gpr = false;
  for (const auto& [p, hr] : model->reg_renumber) {
    if (hr >= 16 && hr <= 23) {
      const auto it = model->pseudo_modes.find(p);
      if (it != model->pseudo_modes.end() &&
          (it->second == RtlMachineMode::kSf || it->second == RtlMachineMode::kDf)) {
        has_fp_in_gpr = true;
        break;
      }
    }
  }
  if (!has_fp_in_gpr) {
    return;
  }
  absl::flat_hash_map<int, int> canon_for_hr;
  absl::flat_hash_map<int, int> remap;
  std::vector<int> fprs;
  for (const auto& [p, hr] : model->reg_renumber) {
    if (hr >= 52 && hr < 64) {
      fprs.push_back(p);
    }
  }
  std::sort(fprs.begin(), fprs.end());
  for (int p : fprs) {
    auto [it, ins] = canon_for_hr.emplace(model->reg_renumber.at(p), p);
    if (!ins) {
      remap[p] = it->second;
    }
  }
  if (remap.empty()) {
    return;
  }
  for (auto& blk : model->blocks) {
    for (RtlPatternAndNotes& pn : blk) {
      pn.RewriteRegs([&](RtlExpr& r) {
        if (r.IsPseudoReg()) {
          if (auto it = remap.find(r.RegNo()); it != remap.end()) {
            r.SetRegNo(it->second);
          }
        }
      });
    }
  }
  for (const auto& [dead_p, _] : remap) {
    model->reg_renumber.erase(dead_p);
    model->pseudo_modes.erase(dead_p);
  }
  // Re-compact trial pseudos in ascending `hard_regno` order.
  std::vector<int> alive;
  for (const auto& [p, _] : model->reg_renumber) {
    alive.push_back(p);
  }
  std::sort(alive.begin(), alive.end());
  std::stable_sort(alive.begin(), alive.end(), [&](int a, int b) {
    return model->reg_renumber.at(a) < model->reg_renumber.at(b);
  });
  absl::flat_hash_map<int, int> renum_p;
  absl::flat_hash_map<int, int> new_hr;
  absl::flat_hash_map<int, RtlMachineMode> new_m;
  for (size_t i = 0; i < alive.size(); ++i) {
    const int np = 72 + static_cast<int>(i);
    renum_p[alive[i]] = np;
    new_hr[np] = model->reg_renumber.at(alive[i]);
    new_m[np] = model->pseudo_modes.at(alive[i]);
  }
  for (auto& blk : model->blocks) {
    for (RtlPatternAndNotes& pn : blk) {
      pn.RewriteRegs([&](RtlExpr& r) {
        if (r.IsPseudoReg()) {
          if (auto it = renum_p.find(r.RegNo()); it != renum_p.end()) {
            r.SetRegNo(it->second);
          }
        }
      });
    }
  }
  model->reg_renumber = std::move(new_hr);
  model->pseudo_modes = std::move(new_m);
}

struct ScopeSolveResult {
  bool matched = false;
  int hidden_sret_pseudo = -1;
  size_t b0_param_prefix_end = 0;
  O0StupidTimeline tl;
  absl::flat_hash_map<int, O0ScopeKind> scope_kinds;
  O0OrderedScopes ord;
};

ScopeSolveResult SolveO0ScopesOnModel(bool gp64, bool fp64, const TrialO0Model& model) {
  ScopeSolveResult res;
  if (model.blocks.empty()) {
    res.matched = true;
    return res;
  }
  bool has_return_use_insn = false;
  for (const auto& blk : model.blocks) {
    for (const RtlPatternAndNotes& pn : blk) {
      if (pn.pattern.op == RtlOp::kUse && pn.pattern.children.size() == 1 &&
          pn.pattern.children[0].flags.integrated) {
        has_return_use_insn = true;
      }
    }
  }

  absl::flat_hash_set<int> b0_entry_param_cands;
  {
    const auto& b0 = model.blocks[0];
    if (!has_return_use_insn && !b0.empty() && b0[0].pattern.IsSet() &&
        b0[0].pattern.SetDest().IsPseudoReg() &&
        b0[0].pattern.SetDest().mode == RtlMachineMode::kSi && b0[0].pattern.SetSrc().IsHardReg() &&
        b0[0].pattern.SetSrc().RegNo() == 4) {
      const int p0 = b0[0].pattern.SetDest().RegNo();
      if (model.reg_renumber.contains(p0) && model.reg_renumber.at(p0) == 2) {
        bool stored_to_stack_slot = false;
        for (size_t i = 1; i < b0.size(); ++i) {
          if (b0[i].pattern.IsSet() && b0[i].pattern.SetDest().IsMem() &&
              b0[i].pattern.SetSrc().ContainsReg(p0)) {
            stored_to_stack_slot = true;
            break;
          }
        }
        bool used_after_b0_0 = false;
        for (size_t b = 0; b < model.blocks.size(); ++b) {
          for (size_t i = (b == 0 ? 1 : 0); i < model.blocks[b].size(); ++i) {
            if (model.blocks[b][i].pattern.ContainsReg(p0)) {
              used_after_b0_0 = true;
            }
          }
        }
        if (!stored_to_stack_slot && used_after_b0_0) {
          res.hidden_sret_pseudo = p0;
          b0_entry_param_cands.insert(p0);
          res.b0_param_prefix_end = std::max<size_t>(res.b0_param_prefix_end, 1);
        }
      }
    }

    absl::flat_hash_map<int, size_t> b0_arg_pseudo_def;
    std::array<bool, kFirstPseudoRegister> arg_written{};
    for (size_t i = 0; i < b0.size(); ++i) {
      const RtlExpr& pat = b0[i].pattern;
      if (!pat.IsSet()) {
        break;
      }
      const RtlExpr& dst = pat.SetDest();
      const RtlExpr& src = pat.SetSrc();
      if (dst.IsPseudoReg() && dst.mode == RtlMachineMode::kSi && src.IsHardReg() &&
          src.RegNo() >= 4 && src.RegNo() <= 7 && !arg_written[src.RegNo()]) {
        b0_arg_pseudo_def[dst.RegNo()] = i;
        res.b0_param_prefix_end = std::max(res.b0_param_prefix_end, i + 1);
        continue;
      }
      if (dst.IsPseudoReg() && dst.mode == RtlMachineMode::kSi && src.op == RtlOp::kAshift &&
          src.children.size() == 2 && src.children[0].IsHardReg() && src.children[0].RegNo() >= 4 &&
          src.children[0].RegNo() <= 7 && !arg_written[src.children[0].RegNo()] &&
          src.children[1].op == RtlOp::kConstInt && src.children[1].int_val == 16) {
        b0_arg_pseudo_def[dst.RegNo()] = i;
        res.b0_param_prefix_end = std::max(res.b0_param_prefix_end, i + 1);
        continue;
      }
      if (dst.IsMem() && src.IsHardReg() && IsO0OutgoingCallArgHardReg(src.RegNo()) &&
          !arg_written[src.RegNo()]) {
        res.b0_param_prefix_end = std::max(res.b0_param_prefix_end, i + 1);
        continue;
      }
      if (dst.IsMem() && src.op == RtlOp::kSubreg && !src.children.empty() &&
          src.children[0].IsPseudoReg() && b0_arg_pseudo_def.contains(src.children[0].RegNo())) {
        b0_entry_param_cands.insert(src.children[0].RegNo());
        res.b0_param_prefix_end = std::max(res.b0_param_prefix_end, i + 1);
        continue;
      }
      if (dst.IsMem() && src.IsPseudoReg() && b0_arg_pseudo_def.contains(src.RegNo())) {
        b0_entry_param_cands.insert(src.RegNo());
        res.b0_param_prefix_end = std::max(res.b0_param_prefix_end, i + 1);
        continue;
      }
      if (dst.IsHardReg()) {
        arg_written[dst.RegNo()] = true;
      }
      break;
    }
  }

  res.tl = BuildO0StupidTimeline(gp64, fp64, model, res.b0_param_prefix_end, b0_entry_param_cands);
  const O0StupidTimeline& tl = res.tl;

  auto is_compatible_from = [&](int p, const O0PseudoSuidSpan& sp, int start_suid) {
    if (sp.born == kMaxInt || sp.has_explicit_use_insn) {
      return false;
    }
    const int hr = model.reg_renumber.at(p);
    const int nw = std::max(1, MipsHardRegNregs(hr, sp.mode, gp64, fp64));
    if (hr < 2 || hr >= 64) {
      return false;
    }
    const bool crosses_call_to_exit =
        std::any_of(tl.call_suids.begin(), tl.call_suids.end(),
                    [&](int cs) { return cs >= start_suid && cs < tl.exit_suid; });
    if (crosses_call_to_exit && !IsMipsO0CalleeSavedRange(hr, nw)) {
      return false;
    }
    for (int ins = std::max(0, start_suid); ins < tl.exit_suid; ++ins) {
      for (int w = hr; w < hr + nw; ++w) {
        if (tl.after_hr[ins][w]) {
          return false;
        }
      }
    }
    for (const auto& [q, qsp] : tl.spans) {
      if (q == p || qsp.born == kMaxInt) {
        continue;
      }
      const int qhr = model.reg_renumber.at(q);
      const int qnw = std::max(1, MipsHardRegNregs(qhr, qsp.mode, gp64, fp64));
      if (std::max(hr, qhr) < std::min(hr + nw, qhr + qnw) &&
          std::max(start_suid, qsp.born) < std::min(tl.exit_suid, qsp.dead)) {
        return false;
      }
    }
    return true;
  };

  for (const auto& [p, sp] : tl.spans) {
    if (sp.is_entry_param && is_compatible_from(p, sp, sp.born)) {
      res.scope_kinds[p] = O0ScopeKind::kEntryParam;
    }
  }

  for (size_t iter = 0; iter < tl.spans.size() * 2; ++iter) {
    const O0OrderedScopes cur_ord = OrderO0Scopes(tl, model.reg_renumber, res.scope_kinds);
    const absl::flat_hash_map<int, int> sim =
        SimulateO0StupidAlloc(gp64, fp64, tl, model.reg_renumber, res.scope_kinds, cur_ord);
    bool all_match = true;
    for (const auto& [q, got_hr] : sim) {
      if (got_hr != model.reg_renumber.at(q)) {
        all_match = false;
        break;
      }
    }
    if (all_match) {
      break;
    }

    int best_p = -1;
    int best_hr = kMaxInt;
    int best_tier = kMaxInt;
    O0ScopeKind best_kind = O0ScopeKind::kBirthLocal;
    for (const auto& [p, sp] : tl.spans) {
      if (sp.is_entry_param) {
        continue;
      }
      auto sk_it = res.scope_kinds.find(p);
      if (sk_it != res.scope_kinds.end() && sk_it->second != O0ScopeKind::kBirthLocal) {
        continue;
      }
      const bool already_birth_local = (sk_it != res.scope_kinds.end());
      if (!already_birth_local && !is_compatible_from(p, sp, sp.born)) {
        continue;
      }
      const int hr = model.reg_renumber.at(p);
      if (sim.at(p) == hr) {
        continue;
      }
      const int nw = std::max(1, MipsHardRegNregs(hr, sp.mode, gp64, fp64));
      const int fallback_rank = IsO0FpInGprFallback(sp, hr) ? 1 : 0;
      int p_born = 0;
      int p_dead = 0;
      int p_len = 0;
      ComputeO0PostScopeSpan(tl, res.scope_kinds, cur_ord, p, &p_born, &p_dead, &p_len);
      bool stolen_by_other = false;
      bool needs_b0 = already_birth_local;
      for (const auto& [q, got_hr] : sim) {
        if (q == p || got_hr < 0 || got_hr == model.reg_renumber.at(q)) {
          continue;
        }
        const auto& qsp = tl.spans.at(q);
        const int qnw = std::max(1, MipsHardRegNregs(got_hr, qsp.mode, gp64, fp64));
        if (std::max(hr, got_hr) < std::min(hr + nw, got_hr + qnw)) {
          int q_born = 0;
          int q_dead = 0;
          int q_len = 0;
          ComputeO0PostScopeSpan(tl, res.scope_kinds, cur_ord, q, &q_born, &q_dead, &q_len);
          if (std::max(p_born, q_born) < std::min(p_dead, q_dead)) {
            stolen_by_other = true;
            if (q_len >= (tl.exit_suid - sp.born + 1)) {
              needs_b0 = true;
            }
          }
        }
      }
      int tier = kMaxInt;
      O0ScopeKind cand_kind = O0ScopeKind::kBirthLocal;
      if (stolen_by_other) {
        if (needs_b0 && !is_compatible_from(p, sp, tl.b0_prefix_suid)) {
          continue;
        }
        tier = fallback_rank;
        cand_kind = needs_b0 ? O0ScopeKind::kB0Local : O0ScopeKind::kBirthLocal;
      } else if (IsMipsO0CalleeSavedRange(hr, nw)) {
        if (already_birth_local && !is_compatible_from(p, sp, tl.b0_prefix_suid)) {
          continue;
        }
        tier = 2 + fallback_rank;
        cand_kind = already_birth_local ? O0ScopeKind::kB0Local : O0ScopeKind::kBirthLocal;
      }
      if (tier < best_tier ||
          (tier == best_tier && (hr < best_hr || (hr == best_hr && p < best_p)))) {
        best_tier = tier;
        best_hr = hr;
        best_p = p;
        best_kind = cand_kind;
      }
    }
    if (best_p < 0) {
      break;
    }
    res.scope_kinds[best_p] = best_kind;
  }

  res.ord = OrderO0Scopes(tl, model.reg_renumber, res.scope_kinds);
  const absl::flat_hash_map<int, int> final_sim =
      SimulateO0StupidAlloc(gp64, fp64, tl, model.reg_renumber, res.scope_kinds, res.ord);
  res.matched = true;
  for (const auto& [q, got_hr] : final_sim) {
    if (got_hr != model.reg_renumber.at(q)) {
      res.matched = false;
      break;
    }
  }
  return res;
}

void ReconstructO0ObeyRegdeclsScopeUses(const RtlControlFlowGraph& cfg, const O0PrologueInfo& pinfo,
                                        bool gp64, bool fp64,
                                        std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  TrialO0Model model = BuildTrialO0PseudoModel(cfg, pinfo, gp64, fp64, *all_blocks);
  ScopeSolveResult solved = SolveO0ScopesOnModel(gp64, fp64, model);
  if (!solved.matched) {
    CoalesceTrialExhaustedCalleeSavedFprs(&model);
    solved = SolveO0ScopesOnModel(gp64, fp64, model);
  }
  if (solved.scope_kinds.empty() && solved.hidden_sret_pseudo < 0) {
    return;
  }

  size_t b0_prefix_orig_pos = (solved.b0_param_prefix_end < model.orig_idx[0].size())
                                  ? model.orig_idx[0][solved.b0_param_prefix_end]
                                  : (*all_blocks)[0].size();

  // Insert `birth_locals` scope-start `(use (reg:M hr))` markers in descending
  // `(birth_b, orig_birth_i)` order (`ord.birth_locals` is already sorted in
  // descending `born` order). Step backward past any single-instruction reload
  // scratch feeding `orig_i` so `emit_reload_insns` input reloads stay adjacent
  // to `orig_i`.
  for (int p : solved.ord.birth_locals) {
    const O0PseudoSuidSpan& sp = solved.tl.spans.at(p);
    const int hr = model.reg_renumber.at(p);
    auto& blk = (*all_blocks)[sp.birth_b];
    const size_t min_i = (sp.birth_b == 0) ? pinfo.prologue_end : 0;
    size_t ins_i = model.orig_idx[sp.birth_b][sp.birth_i];
    while (ins_i > min_i && ins_i < blk.size()) {
      const RtlExpr& prev = blk[ins_i - 1].pattern;
      if (!prev.IsSet() || !prev.SetDest().IsHardReg() || prev.SetDest().RegNo() == hr) {
        break;
      }
      const int scr = prev.SetDest().RegNo();
      if (!((scr >= 1 && scr <= 15) || scr == 24 || scr == 25 || (scr >= 32 && scr <= 51))) {
        break;
      }
      if (!blk[ins_i].pattern.ContainsReg(scr)) {
        break;
      }
      bool used_after = false;
      for (size_t k = ins_i + 1; k < blk.size(); ++k) {
        if (IsCallPattern(blk[k].pattern)) {
          break;
        }
        if (blk[k].pattern.IsSet() && blk[k].pattern.SetDest().IsHardReg() &&
            blk[k].pattern.SetDest().RegNo() == scr && !blk[k].pattern.SetSrc().ContainsReg(scr)) {
          break;
        }
        if (blk[k].pattern.ContainsReg(scr)) {
          used_after = true;
          break;
        }
      }
      if (used_after) {
        break;
      }
      --ins_i;
    }
    RtlPatternAndNotes u;
    u.pattern = RtlExpr::MakeUse(MakeGccHardReg(sp.mode, hr));
    blk.insert(blk.begin() + static_cast<ptrdiff_t>(ins_i), std::move(u));
    if (sp.birth_b == 0 && ins_i <= b0_prefix_orig_pos) {
      ++b0_prefix_orig_pos;
    }
  }

  if (!solved.ord.entry_params.empty() || !solved.ord.b0_locals.empty()) {
    auto& b0 = (*all_blocks)[0];
    size_t pos = std::min(b0_prefix_orig_pos, b0.size());
    for (int p : solved.ord.entry_params) {
      RtlPatternAndNotes u;
      u.pattern =
          RtlExpr::MakeUse(MakeGccHardReg(solved.tl.spans.at(p).mode, model.reg_renumber.at(p)));
      b0.insert(b0.begin() + static_cast<ptrdiff_t>(pos++), std::move(u));
    }
    for (int p : solved.ord.b0_locals) {
      RtlPatternAndNotes u;
      u.pattern =
          RtlExpr::MakeUse(MakeGccHardReg(solved.tl.spans.at(p).mode, model.reg_renumber.at(p)));
      b0.insert(b0.begin() + static_cast<ptrdiff_t>(pos++), std::move(u));
    }
  }

  auto& exit_blk = all_blocks->back();
  for (int p : solved.ord.birth_locals) {
    RtlPatternAndNotes u;
    u.pattern =
        RtlExpr::MakeUse(MakeGccHardReg(solved.tl.spans.at(p).mode, model.reg_renumber.at(p)));
    exit_blk.push_back(std::move(u));
  }
  for (auto it = solved.ord.b0_locals.rbegin(); it != solved.ord.b0_locals.rend(); ++it) {
    RtlPatternAndNotes u;
    u.pattern =
        RtlExpr::MakeUse(MakeGccHardReg(solved.tl.spans.at(*it).mode, model.reg_renumber.at(*it)));
    exit_blk.push_back(std::move(u));
  }
  for (auto it = solved.ord.entry_params.rbegin(); it != solved.ord.entry_params.rend(); ++it) {
    RtlPatternAndNotes u;
    u.pattern =
        RtlExpr::MakeUse(MakeGccHardReg(solved.tl.spans.at(*it).mode, model.reg_renumber.at(*it)));
    exit_blk.push_back(std::move(u));
  }
  if (solved.hidden_sret_pseudo >= 0) {
    RtlFlags ret_flags;
    ret_flags.integrated = true;
    RtlPatternAndNotes ret_set;
    ret_set.pattern = RtlExpr::MakeSet(MakeGccHardReg(RtlMachineMode::kSi, 2, ret_flags),
                                       MakeGccHardReg(RtlMachineMode::kSi, 2));
    exit_blk.push_back(std::move(ret_set));
    RtlPatternAndNotes ret_use;
    ret_use.pattern = RtlExpr::MakeUse(MakeGccHardReg(RtlMachineMode::kSi, 2, ret_flags));
    exit_blk.push_back(std::move(ret_use));
  }

  // Reassign any unused synthesized `movstrsi` scratch clobbers (`mips.md`
  // `movstrsi_internal` `match_scratch` operands beyond `used_slots`) using
  // `reload1.c:order_regs_for_reload` priority: registers completely unused
  // across the function (`hard_reg_n_uses[r].uses == 0`, neither allocated to
  // a pseudo nor `regs_explicitly_used[r]`) are ordered ahead of any register
  // used elsewhere in the function.
  std::array<bool, kFirstPseudoRegister> scope_live_to_exit{};
  for (const auto& [p, _] : solved.scope_kinds) {
    const int hr = model.reg_renumber.at(p);
    const int nw = std::max(1, MipsHardRegNregs(hr, solved.tl.spans.at(p).mode, gp64, fp64));
    for (int w = hr; w < std::min(kFirstPseudoRegister, hr + nw); ++w) {
      scope_live_to_exit[w] = true;
    }
  }
  auto is_movstrsi_parallel = [](const RtlExpr& pat, int64_t* out_used_slots) -> bool {
    if (pat.op != RtlOp::kParallel || pat.children.size() != 8 || !pat.children[0].IsSet() ||
        !pat.children[0].SetDest().IsMem() ||
        pat.children[0].SetDest().mode != RtlMachineMode::kBlk ||
        pat.children[5].op != RtlOp::kUse || pat.children[5].children.empty() ||
        !pat.children[5].children[0].IsConstInt() || pat.children[6].op != RtlOp::kUse ||
        pat.children[6].children.empty() || !pat.children[6].children[0].IsConstInt()) {
      return false;
    }
    const int64_t bytes = pat.children[5].children[0].int_val;
    const int64_t align = std::max<int64_t>(1, pat.children[6].children[0].int_val);
    const int64_t unit = (align >= 4) ? 4 : (align >= 2 ? 2 : 1);
    *out_used_slots = std::min<int64_t>(
        4,
        (bytes / 4) + ((unit == 4) ? ((bytes % 4 != 0) ? 1 : 0)
                                   : (unit == 2 ? ((bytes % 4) / 2 + (bytes % 2)) : (bytes % 4))));
    return true;
  };
  std::array<bool, kFirstPseudoRegister> globally_used{};
  auto mark_expr_hard_regs = [&](const RtlExpr& expr) {
    expr.Visit([&](const RtlExpr& n) {
      if (n.IsHardReg() && n.RegNo() >= 0 && n.RegNo() < kFirstPseudoRegister) {
        const int r = n.RegNo();
        const int nw = std::max(1, MipsHardRegNregs(r, n.mode, gp64, fp64));
        for (int w = r; w < std::min(kFirstPseudoRegister, r + nw); ++w) {
          globally_used[w] = true;
        }
      }
    });
  };
  for (const auto& blk : *all_blocks) {
    for (const RtlPatternAndNotes& insn : blk) {
      int64_t used_slots = 0;
      if (is_movstrsi_parallel(insn.pattern, &used_slots)) {
        mark_expr_hard_regs(insn.pattern.children[0]);
        for (int64_t s = 0; s < used_slots; ++s) {
          mark_expr_hard_regs(insn.pattern.children[1 + static_cast<size_t>(s)]);
        }
      } else {
        mark_expr_hard_regs(insn.pattern);
      }
      for (const RtlExpr& fu : insn.call_fusage) {
        mark_expr_hard_regs(fu);
      }
    }
  }
  static constexpr int kCallerSavedGprOrder[] = {2,  3,  4,  5,  6,  7,  8,  9,
                                                 10, 11, 12, 13, 14, 15, 24, 25};
  for (auto& blk : *all_blocks) {
    for (size_t i = 0; i < blk.size(); ++i) {
      RtlExpr& pat = blk[i].pattern;
      int64_t used_slots = 0;
      if (!is_movstrsi_parallel(pat, &used_slots)) {
        continue;
      }
      std::array<bool, kFirstPseudoRegister> busy = scope_live_to_exit;
      pat.children[0].Visit([&](const RtlExpr& n) {
        if (n.IsHardReg() && n.RegNo() >= 0 && n.RegNo() < kFirstPseudoRegister) {
          busy[n.RegNo()] = true;
        }
      });
      for (int64_t s = 0; s < used_slots; ++s) {
        const RtlExpr& cl = pat.children[1 + static_cast<size_t>(s)];
        if (cl.op == RtlOp::kClobber && !cl.children.empty() && cl.children[0].IsHardReg()) {
          busy[cl.children[0].RegNo()] = true;
        }
      }
      size_t order_idx = 0;
      if (used_slots > 0 && pat.children[static_cast<size_t>(used_slots)].op == RtlOp::kClobber &&
          !pat.children[static_cast<size_t>(used_slots)].children.empty() &&
          pat.children[static_cast<size_t>(used_slots)].children[0].IsHardReg()) {
        const int last_r = pat.children[static_cast<size_t>(used_slots)].children[0].RegNo();
        for (size_t oi = 0; oi < std::size(kCallerSavedGprOrder); ++oi) {
          if (kCallerSavedGprOrder[oi] == last_r) {
            order_idx = (oi + 1) % std::size(kCallerSavedGprOrder);
            break;
          }
        }
      }
      for (size_t s = static_cast<size_t>(used_slots); s < 4; ++s) {
        RtlExpr& cl = pat.children[1 + s];
        if (cl.op != RtlOp::kClobber || cl.children.empty() || !cl.children[0].IsHardReg()) {
          continue;
        }
        bool assigned = false;
        for (size_t step = 0; step < std::size(kCallerSavedGprOrder); ++step) {
          const int cand =
              kCallerSavedGprOrder[(order_idx + step) % std::size(kCallerSavedGprOrder)];
          if (!busy[cand] && !globally_used[cand]) {
            cl.children[0] = MakeGccHardReg(RtlMachineMode::kSi, cand);
            busy[cand] = true;
            order_idx = (order_idx + step + 1) % std::size(kCallerSavedGprOrder);
            assigned = true;
            break;
          }
        }
        if (assigned) {
          continue;
        }
        const int cur_r = cl.children[0].RegNo();
        if (!busy[cur_r]) {
          busy[cur_r] = true;
          continue;
        }
        for (size_t step = 0; step < std::size(kCallerSavedGprOrder); ++step) {
          const int cand =
              kCallerSavedGprOrder[(order_idx + step) % std::size(kCallerSavedGprOrder)];
          if (!busy[cand]) {
            cl.children[0] = MakeGccHardReg(RtlMachineMode::kSi, cand);
            busy[cand] = true;
            order_idx = (order_idx + step + 1) % std::size(kCallerSavedGprOrder);
            break;
          }
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
  WrapO0NoConflictBlockDiSubregs(pinfo.prologue_end, &all_blocks);

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
