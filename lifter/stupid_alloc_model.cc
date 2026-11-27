#include "lifter/stupid_alloc_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_reg_alloc_common.h"
#include "lifter/rtl_cfg_utils.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kMaxInt = std::numeric_limits<int>::max();

int O0HardRegNregs(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  MipsTargetConfig cfg;
  cfg.gp_64bit = gp64;
  cfg.float_64bit = fp64;
  return HardRegnoNregs(regno, RegModeInfo{.mode = mode}, cfg);
}

int O0ModeByteSize(RtlMachineMode mode) {
  return ModeByteSize(RegModeInfo{.mode = mode});
}

bool IsO0ControlTransferPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
    return IsO0ControlTransferPattern(pat.children[0]);
  }
  return false;
}

// Reconstructs `optabs.c:2485-2525` (`emit_no_conflict_block`) +
// `final.c:1933, 2095-2107` (`alter_subreg`) subword `SUBREG`s inside an
// explicit `(clobber (reg:DI hr)) ... (set (reg:DI hr) (reg:DI hr))` block.
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
        if (p0.op != RtlOp::kUse && (IsCallPattern(p0) || IsO0ControlTransferPattern(p0) ||
                                     p0.ContainsReg(hr) || p0.ContainsReg(hr + 1))) {
          break;
        }
      }
    }
  }
}

// Case A of `WrapO0SingleBlockDiSubregs`: `DImode` def -> `SI` subword writes
// -> `DImode` read without subsequent live use.
bool TryWrapSingleBlockDiSubwordWrites(size_t i, int hr, const RtlFlags& di_flags,
                                       std::vector<RtlPatternAndNotes>* blk) {
  bool has_si_subword = false;
  size_t end_idx = blk->size();
  for (size_t j = i + 1; j < blk->size(); ++j) {
    const RtlExpr& jpat = (*blk)[j].pattern;
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
  if (!has_si_subword || end_idx >= blk->size()) {
    return false;
  }
  bool used_after = false;
  for (size_t j = end_idx + 1; j < blk->size(); ++j) {
    const RtlExpr& jpat = (*blk)[j].pattern;
    if (IsCallPattern(jpat)) {
      for (const RtlExpr& fu : (*blk)[j].call_fusage) {
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
    return false;
  }
  for (size_t j = i + 1; j < end_idx; ++j) {
    if ((*blk)[j].pattern.op == RtlOp::kUse) {
      continue;
    }
    (*blk)[j].pattern.Mutate([&](RtlExpr& n) {
      if (n.IsHardReg() && (n.RegNo() == hr || n.RegNo() == hr + 1) &&
          n.mode == RtlMachineMode::kSi && !n.flags.integrated) {
        n = RtlExpr::MakeSubreg(RtlMachineMode::kSi,
                                MakeGccHardReg(RtlMachineMode::kDi, hr, di_flags), n.RegNo() - hr);
      }
    });
  }
  return true;
}

// Case B of `WrapO0SingleBlockDiSubregs` (`final.c:1933, 2095-2107`
// `alter_subreg` on `operand_subword` consumers such as `muldi3`): a
// single-block computed `(reg:DI hr)` def whose subsequent uses read BOTH words
// (`hr` and `hr + 1`) as `SI` subwords across distinct instructions before
// either word is overwritten.
void TryWrapSingleBlockDiSubwordReads(size_t i, int hr, const RtlFlags& di_flags,
                                      std::vector<RtlPatternAndNotes>* blk) {
  size_t first_word0_j = blk->size();
  size_t first_word1_j = blk->size();
  bool bad_read = false;
  size_t last_read_j = i;
  for (size_t j = i + 1; j < blk->size(); ++j) {
    const RtlExpr& jpat = (*blk)[j].pattern;
    if (IsCallPattern(jpat) || IsO0ControlTransferPattern(jpat)) {
      break;
    }
    if (jpat.op == RtlOp::kUse) {
      continue;
    }
    bool j_read_pair = false;
    auto scan_reads = [&](const RtlExpr& sub) {
      sub.Visit([&](const RtlExpr& n) {
        if (!n.IsHardReg()) {
          return;
        }
        const int r = n.RegNo();
        const int nw = (n.mode == RtlMachineMode::kDi || n.mode == RtlMachineMode::kDf) ? 2 : 1;
        if (r + nw <= hr || r > hr + 1) {
          return;
        }
        if (n.mode == RtlMachineMode::kSi && !n.flags.integrated) {
          if (r == hr) {
            first_word0_j = std::min(first_word0_j, j);
          } else {
            first_word1_j = std::min(first_word1_j, j);
          }
          j_read_pair = true;
        } else {
          bad_read = true;
        }
      });
    };
    bool j_writes_pair = false;
    auto check_elem = [&](const RtlExpr& elem) {
      if (elem.IsSet()) {
        scan_reads(elem.SetSrc());
        if (!elem.SetDest().IsHardReg()) {
          scan_reads(elem.SetDest());
        } else {
          const int r = elem.SetDest().RegNo();
          const int nw = (elem.SetDest().mode == RtlMachineMode::kDi ||
                          elem.SetDest().mode == RtlMachineMode::kDf)
                             ? 2
                             : 1;
          if (r < hr + 2 && hr < r + nw) {
            j_writes_pair = true;
          }
        }
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty() &&
                 elem.children[0].IsHardReg()) {
        const int r = elem.children[0].RegNo();
        const int nw = (elem.children[0].mode == RtlMachineMode::kDi ||
                        elem.children[0].mode == RtlMachineMode::kDf)
                           ? 2
                           : 1;
        if (r < hr + 2 && hr < r + nw) {
          j_writes_pair = true;
        }
      } else {
        scan_reads(elem);
      }
    };
    if (jpat.op == RtlOp::kParallel) {
      for (const RtlExpr& elem : jpat.children) {
        check_elem(elem);
      }
    } else {
      check_elem(jpat);
    }
    if (bad_read) {
      break;
    }
    if (j_read_pair) {
      last_read_j = j;
    }
    if (j_writes_pair) {
      break;
    }
  }
  if (bad_read || first_word0_j >= blk->size() || first_word1_j >= blk->size() ||
      first_word0_j == first_word1_j || last_read_j <= i) {
    return;
  }
  auto wrap_reads = [&](RtlExpr& sub) {
    sub.Mutate([&](RtlExpr& n) {
      if (n.IsHardReg() && (n.RegNo() == hr || n.RegNo() == hr + 1) &&
          n.mode == RtlMachineMode::kSi && !n.flags.integrated) {
        n = RtlExpr::MakeSubreg(RtlMachineMode::kSi,
                                MakeGccHardReg(RtlMachineMode::kDi, hr, di_flags), n.RegNo() - hr);
      }
    });
  };
  auto wrap_elem = [&](RtlExpr& elem) {
    if (elem.IsSet()) {
      wrap_reads(elem.children[1]);
      if (!elem.children[0].IsHardReg()) {
        wrap_reads(elem.children[0]);
      }
    } else if (elem.op != RtlOp::kClobber) {
      wrap_reads(elem);
    }
  };
  for (size_t j = i + 1; j <= last_read_j; ++j) {
    if ((*blk)[j].pattern.op == RtlOp::kUse) {
      continue;
    }
    if ((*blk)[j].pattern.op == RtlOp::kParallel) {
      for (RtlExpr& elem : (*blk)[j].pattern.children) {
        wrap_elem(elem);
      }
    } else {
      wrap_elem((*blk)[j].pattern);
    }
  }
}

// Identifies `PARALLEL` patterns whose `(clobber (reg ...))` operands originate
// from `(match_scratch ...)` during `-O0` `stupid_life_analysis` (`mips.md`:
// `mul*`, `div*`, `mod*`, `ffs*`, `fix_trunc*`, `movstrsi_internal*`).
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

int LookupEffectiveHardReg(const TrialO0Model& model, int regno) {
  if (regno < kFirstPseudoRegister) {
    return regno;
  }
  auto it = model.reg_renumber.find(regno);
  return (it != model.reg_renumber.end()) ? it->second : -1;
}

// Helper 1 of `FoldTrialO0ReloadScratchesAndMulsidi3Spills`: folds `-O0`
// `mulsidi3` top-of-frame 8-byte `$fp` spill/reload (`reload1.c:2320-2395`
// `alter_reg(..., -1)`).
void FoldTrialO0Mulsidi3TopSpills(const O0PrologueInfo& pinfo, int* next_p, TrialO0Model* m) {
  if (!pinfo.frame_pointer_needed || pinfo.local_frame_bytes < 8 ||
      (pinfo.local_frame_bytes % 8) != 0) {
    return;
  }
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
      const int mul_hr = LookupEffectiveHardReg(*m, mul_r);
      int sec_p = -1;
      size_t store_idx = 0;
      if (blk[i + 1].pattern.IsSet() && blk[i + 1].pattern.SetSrc().IsReg() &&
          !blk[i + 1].pattern.SetSrc().IsArgPointerReg() &&
          blk[i + 1].pattern.SetSrc().RegNo() == mul_r && blk[i + 1].pattern.SetDest().IsMem() &&
          !blk[i + 1].pattern.SetDest().children.empty() &&
          MatchO0RegPlusConstOffset(blk[i + 1].pattern.SetDest().children[0],
                                    kFramePointerRegnum) == top_off) {
        store_idx = i + 1;
      }
      if (store_idx == 0 && i + 3 < blk.size() && mul_hr == kHiRegnum &&
          blk[i + 1].pattern.IsSet() && blk[i + 1].pattern.SetDest().IsPseudoReg() &&
          blk[i + 1].pattern.SetDest().mode == RtlMachineMode::kDi &&
          blk[i + 1].pattern.SetSrc().IsReg() && !blk[i + 1].pattern.SetSrc().IsArgPointerReg() &&
          blk[i + 1].pattern.SetSrc().RegNo() == mul_r) {
        const int cand_sec = blk[i + 1].pattern.SetDest().RegNo();
        if (blk[i + 2].pattern.IsSet() && blk[i + 2].pattern.SetSrc().IsPseudoReg() &&
            blk[i + 2].pattern.SetSrc().RegNo() == cand_sec &&
            blk[i + 2].pattern.SetDest().IsMem() &&
            !blk[i + 2].pattern.SetDest().children.empty() &&
            MatchO0RegPlusConstOffset(blk[i + 2].pattern.SetDest().children[0],
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
            MatchO0RegPlusConstOffset(blk[k].pattern.SetSrc().children[0], kFramePointerRegnum) ==
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
                  MatchO0RegPlusConstOffset(n.children[0], kFramePointerRegnum);
              const int sz = std::max(1, O0ModeByteSize(n.mode));
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
            const int nhr = LookupEffectiveHardReg(*m, n.RegNo());
            if (nhr == kHiRegnum || nhr == kLoRegnum) {
              md_busy_between = true;
            }
          }
        });
      }
      if (sec_p >= 0) {
        m->reg_renumber.erase(sec_p);
        m->pseudo_modes.erase(sec_p);
      }
      m->reg_renumber[spill_p] = md_busy_between ? -1 : kHiRegnum;
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

// Helper 2 of `FoldTrialO0ReloadScratchesAndMulsidi3Spills`: folds
// `const_int 0 -> DI reg -> MEM` scratch pairs (`!gp64`).
bool TryFoldTrialZeroDiStoreScratch(bool gp64, const absl::btree_map<int, int>& trial_ref_counts,
                                    const absl::btree_set<int>& trial_use_pseudos, size_t b,
                                    size_t* i_ptr, TrialO0Model* m) {
  auto& blk = m->blocks[b];
  auto& oidx = m->orig_idx[b];
  size_t& i = *i_ptr;
  if (gp64 || !blk[i].pattern.IsSet() || !blk[i].pattern.SetDest().IsReg() ||
      blk[i].pattern.SetDest().IsArgPointerReg() || blk[i].pattern.SetDest().flags.volatil ||
      blk[i].pattern.SetDest().flags.integrated ||
      blk[i].pattern.SetDest().mode != RtlMachineMode::kDi ||
      !blk[i].pattern.SetSrc().IsConstInt() || blk[i].pattern.SetSrc().int_val != 0 ||
      !blk[i + 1].pattern.IsSet() || !blk[i + 1].pattern.SetDest().IsMem() ||
      blk[i + 1].pattern.SetDest().mode != RtlMachineMode::kDi ||
      !blk[i + 1].pattern.SetSrc().IsReg() ||
      blk[i + 1].pattern.SetSrc().mode != RtlMachineMode::kDi ||
      blk[i + 1].pattern.SetSrc().RegNo() != blk[i].pattern.SetDest().RegNo() ||
      blk[i + 1].pattern.SetDest().ContainsReg(blk[i].pattern.SetDest().RegNo())) {
    return false;
  }
  const int zr = blk[i].pattern.SetDest().RegNo();
  const int zhr = LookupEffectiveHardReg(*m, zr);
  auto rc_it = trial_ref_counts.find(zr);
  const int rc = (rc_it != trial_ref_counts.end()) ? rc_it->second : 0;
  const bool single_pair_use =
      zr < kFirstPseudoRegister || (rc == 2 && !trial_use_pseudos.contains(zr));
  if (!single_pair_use || zhr < 2 || zhr > 24) {
    return false;
  }
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
  return true;
}

// Helper 3 of `FoldTrialO0ReloadScratchesAndMulsidi3Spills`: folds adjacent
// `mulsidi3` `(reg:DI 64)` -> GPR output reload and `SECONDARY_INPUT_RELOAD_CLASS`
// `MD_REGS -> MEM` reload (`reload1.c:emit_reload_insns`).
bool TryFoldTrialMulsidi3OutputReload(const absl::btree_map<int, int>& trial_ref_counts,
                                      const absl::btree_set<int>& trial_use_pseudos, size_t b,
                                      size_t i, TrialO0Model* m) {
  auto& blk = m->blocks[b];
  auto& oidx = m->orig_idx[b];
  if (blk[i].pattern.op != RtlOp::kParallel || blk[i].pattern.children.empty() ||
      !blk[i].pattern.children[0].IsSet() || !blk[i].pattern.children[0].SetDest().IsReg() ||
      blk[i].pattern.children[0].SetDest().IsArgPointerReg() ||
      blk[i].pattern.children[0].SetSrc().op != RtlOp::kMult) {
    return false;
  }
  const int md_r = blk[i].pattern.children[0].SetDest().RegNo();
  const int md_hr = LookupEffectiveHardReg(*m, md_r);
  if (md_hr != kHiRegnum || !blk[i + 1].pattern.IsSet() || !blk[i + 1].pattern.SetDest().IsReg() ||
      blk[i + 1].pattern.SetDest().IsArgPointerReg() || !blk[i + 1].pattern.SetSrc().IsReg() ||
      blk[i + 1].pattern.SetSrc().IsArgPointerReg() ||
      blk[i + 1].pattern.SetSrc().RegNo() != md_r) {
    return false;
  }
  const RtlExpr dst_reg = blk[i + 1].pattern.SetDest();
  const int dst_hr = LookupEffectiveHardReg(*m, dst_reg.RegNo());
  const bool next_stores_dst_to_mem =
      (i + 2 < blk.size() && blk[i + 2].pattern.IsSet() && blk[i + 2].pattern.SetDest().IsMem() &&
       blk[i + 2].pattern.SetSrc().ContainsReg(dst_reg.RegNo()));
  if (!next_stores_dst_to_mem && (dst_reg.IsPseudoReg() || (dst_hr >= 2 && dst_hr <= 25))) {
    blk[i].pattern.children[0].children[0] = dst_reg;
    if (md_r >= kFirstPseudoRegister) {
      m->reg_renumber.erase(md_r);
      m->pseudo_modes.erase(md_r);
    }
    blk.erase(blk.begin() + static_cast<ptrdiff_t>(i + 1));
    oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(i + 1));
    return true;
  }
  auto rc_it = trial_ref_counts.find(dst_reg.RegNo());
  const int rc = (rc_it != trial_ref_counts.end()) ? rc_it->second : 0;
  if (next_stores_dst_to_mem && dst_reg.IsPseudoReg() && dst_reg.mode == RtlMachineMode::kDi &&
      !trial_use_pseudos.contains(dst_reg.RegNo()) && rc == 2 &&
      blk[i + 2].pattern.SetSrc() == dst_reg &&
      !blk[i + 2].pattern.SetDest().ContainsReg(dst_reg.RegNo())) {
    blk[i + 2].pattern.children[1] = blk[i].pattern.children[0].SetDest();
    m->reg_renumber.erase(dst_reg.RegNo());
    m->pseudo_modes.erase(dst_reg.RegNo());
    blk.erase(blk.begin() + static_cast<ptrdiff_t>(i + 1));
    oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(i + 1));
    return true;
  }
  return false;
}

// Helper 4 of `FoldTrialO0ReloadScratchesAndMulsidi3Spills`: folds
// `reload1.c:emit_reload_insns` earlyclobber `=&d` output reloads on 32-bit
// `DImode` `adddi3_internal_1` (`config/mips/mips.md:407-412`) and variable
// shifts `ashldi3_internal` (`3722-3727`), `ashrdi3_internal` (`3949-3954`),
// `lshrdi3_internal` (`4174-4179`).
bool TryFoldTrialEarlyclobberDiOutputReload(bool gp64,
                                            const absl::btree_map<int, int>& trial_ref_counts,
                                            const absl::btree_set<int>& trial_use_pseudos, size_t b,
                                            size_t i, TrialO0Model* m) {
  auto& blk = m->blocks[b];
  auto& oidx = m->orig_idx[b];
  if (gp64 || blk[i].pattern.op != RtlOp::kParallel || blk[i].pattern.children.empty() ||
      !blk[i].pattern.children[0].IsSet() || !blk[i].pattern.children[0].SetDest().IsPseudoReg() ||
      blk[i].pattern.children[0].SetDest().mode != RtlMachineMode::kDi ||
      blk[i].pattern.children[0].SetSrc().mode != RtlMachineMode::kDi) {
    return false;
  }
  const RtlExpr& di_src = blk[i].pattern.children[0].SetSrc();
  const RtlOp di_op = di_src.op;
  const bool is_adddi_reg_reg = di_op == RtlOp::kPlus && di_src.children.size() == 2 &&
                                di_src.children[0].IsReg() &&
                                !di_src.children[0].IsArgPointerReg() &&
                                di_src.children[1].IsReg() && !di_src.children[1].IsArgPointerReg();
  const bool is_var_shift_di =
      (di_op == RtlOp::kAshift || di_op == RtlOp::kAshiftrt || di_op == RtlOp::kLshiftrt) &&
      di_src.children.size() == 2 && di_src.children[0].IsReg() &&
      !di_src.children[0].IsArgPointerReg() && di_src.children[1].IsReg() &&
      !di_src.children[1].IsArgPointerReg();
  if (!is_adddi_reg_reg && !is_var_shift_di) {
    return false;
  }
  const int scr_p = blk[i].pattern.children[0].SetDest().RegNo();
  auto rc_it = trial_ref_counts.find(scr_p);
  const int rc = (rc_it != trial_ref_counts.end()) ? rc_it->second : 0;
  if (trial_use_pseudos.contains(scr_p) || rc != 2 || !blk[i + 1].pattern.IsSet() ||
      !blk[i + 1].pattern.SetDest().IsReg() || blk[i + 1].pattern.SetDest().IsArgPointerReg() ||
      blk[i + 1].pattern.SetDest().flags.integrated ||
      blk[i + 1].pattern.SetDest().mode != RtlMachineMode::kDi ||
      !blk[i + 1].pattern.SetSrc().IsPseudoReg() || blk[i + 1].pattern.SetSrc().RegNo() != scr_p) {
    return false;
  }
  const RtlExpr dst_reg = blk[i + 1].pattern.SetDest();
  const int scr_hr = LookupEffectiveHardReg(*m, scr_p);
  const int dst_hr = LookupEffectiveHardReg(*m, dst_reg.RegNo());
  if (scr_hr < 2 || scr_hr > 24 || dst_hr < 2 || dst_hr > 24 || scr_hr == dst_hr) {
    return false;
  }
  bool dst_overlaps_input = false;
  di_src.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
      const int nhr = LookupEffectiveHardReg(*m, n.RegNo());
      if (nhr >= 0) {
        const int nnw = (n.mode == RtlMachineMode::kDi || n.mode == RtlMachineMode::kDf) ? 2 : 1;
        if (nhr < dst_hr + 2 && dst_hr < nhr + nnw) {
          dst_overlaps_input = true;
        }
      }
    }
  });
  if (dst_overlaps_input && is_adddi_reg_reg) {
    const int op1_hr = LookupEffectiveHardReg(*m, di_src.children[0].RegNo());
    if (op1_hr == dst_hr) {
      dst_overlaps_input = false;
    }
  }
  if (!dst_overlaps_input) {
    return false;
  }
  blk[i].pattern.children[0].children[0] = dst_reg;
  m->reg_renumber.erase(scr_p);
  m->pseudo_modes.erase(scr_p);
  blk.erase(blk.begin() + static_cast<ptrdiff_t>(i + 1));
  oidx.erase(oidx.begin() + static_cast<ptrdiff_t>(i + 1));
  return true;
}

// Helper 5 of `FoldTrialO0ReloadScratchesAndMulsidi3Spills`: folds `SImode`
// `hi`/`lo` -> GPR output reloads and `"f"` FPR constraint single-use reloads.
bool TryFoldTrialMdSiAndFprReloads(const absl::btree_map<int, int>& trial_ref_counts,
                                   const absl::btree_set<int>& trial_use_pseudos, size_t b,
                                   size_t* i_ptr, TrialO0Model* m) {
  auto& blk = m->blocks[b];
  auto& oidx = m->orig_idx[b];
  size_t& i = *i_ptr;
  if (i + 2 < blk.size() && blk[i].pattern.op == RtlOp::kParallel &&
      blk[i].pattern.children.size() == 3 && blk[i].pattern.children[0].IsSet() &&
      blk[i].pattern.children[1].op == RtlOp::kClobber &&
      blk[i].pattern.children[2].op == RtlOp::kClobber &&
      blk[i].pattern.children[0].SetDest().IsReg() &&
      !blk[i].pattern.children[0].SetDest().IsArgPointerReg() &&
      blk[i].pattern.children[0].SetDest().mode == RtlMachineMode::kSi) {
    const int md_r = blk[i].pattern.children[0].SetDest().RegNo();
    const int md_hr = LookupEffectiveHardReg(*m, md_r);
    const RtlOp op0 = blk[i].pattern.children[0].SetSrc().op;
    if ((md_hr == kHiRegnum && op0 == RtlOp::kTruncate) ||
        (md_hr == kLoRegnum && op0 == RtlOp::kMult)) {
      size_t mv_idx = blk.size();
      for (size_t k = i + 1; k < blk.size(); ++k) {
        const RtlExpr& pk = blk[k].pattern;
        if (pk.IsSet() && pk.SetDest().IsPseudoReg() && pk.SetDest().mode == RtlMachineMode::kSi &&
            pk.SetSrc().IsReg() && !pk.SetSrc().IsArgPointerReg() && pk.SetSrc().RegNo() == md_r) {
          mv_idx = k;
          break;
        }
        bool touches_md = false;
        pk.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
            const int nhr = LookupEffectiveHardReg(*m, n.RegNo());
            if (nhr == kHiRegnum || nhr == kLoRegnum) {
              touches_md = true;
            }
          }
        });
        if (IsO0ControlTransferPattern(pk) || IsCallPattern(pk) || touches_md) {
          break;
        }
      }
      if (mv_idx < blk.size()) {
        const int p_scr = blk[mv_idx].pattern.SetDest().RegNo();
        const int scr_hr = LookupEffectiveHardReg(*m, p_scr);
        auto rc_it = trial_ref_counts.find(p_scr);
        const int rc = (rc_it != trial_ref_counts.end()) ? rc_it->second : 0;
        if (scr_hr >= 2 && scr_hr <= 25 && !trial_use_pseudos.contains(p_scr) && rc == 2) {
          size_t u_idx = blk.size();
          for (size_t k = mv_idx + 1; k < blk.size(); ++k) {
            if (blk[k].pattern.ContainsReg(p_scr)) {
              u_idx = k;
              break;
            }
            if (IsO0ControlTransferPattern(blk[k].pattern) || IsCallPattern(blk[k].pattern)) {
              break;
            }
          }
          if (u_idx < blk.size() && blk[u_idx].pattern.IsSet() &&
              !IsO0ControlTransferPattern(blk[u_idx].pattern)) {
            const RtlExpr& up = blk[u_idx].pattern;
            const RtlOp uop = up.SetSrc().op;
            const bool non_fp = up.SetDest().mode != RtlMachineMode::kSf &&
                                up.SetDest().mode != RtlMachineMode::kDf &&
                                up.SetSrc().mode != RtlMachineMode::kSf &&
                                up.SetSrc().mode != RtlMachineMode::kDf;
            const bool elig = (uop == RtlOp::kFloat && up.SetDest().IsReg()) ||
                              (non_fp && up.SetDest().IsReg() && uop != RtlOp::kReg &&
                               uop != RtlOp::kMem && uop != RtlOp::kDiv && uop != RtlOp::kUdiv &&
                               uop != RtlOp::kMod && uop != RtlOp::kUmod && uop != RtlOp::kMult &&
                               uop != RtlOp::kFloat && uop != RtlOp::kUnsignedFloat);
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
              return true;
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
    const int fpr_hr = LookupEffectiveHardReg(*m, p_fpr);
    const int src_hr = LookupEffectiveHardReg(*m, src_r);
    auto rc_it = trial_ref_counts.find(p_fpr);
    const int rc = (rc_it != trial_ref_counts.end()) ? rc_it->second : 0;
    if (fpr_hr >= kFirstFprRegnum && fpr_hr <= kLastFprRegnum && src_hr >= 2 && src_hr < 32 &&
        !trial_use_pseudos.contains(p_fpr) && rc == 2) {
      size_t op_idx = blk.size();
      for (size_t k = i + 1; k < std::min(blk.size(), i + 3); ++k) {
        if (blk[k].pattern.ContainsReg(p_fpr)) {
          const RtlExpr& kpat = blk[k].pattern;
          const bool is_plain_move = kpat.IsSet() && (kpat.SetSrc().op == RtlOp::kReg ||
                                                      kpat.SetSrc().op == RtlOp::kSubreg);
          if (!is_plain_move && (!kpat.IsSet() || !kpat.SetDest().ContainsReg(p_fpr))) {
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
        return true;
      }
    }
  }
  return false;
}

// State for partitioning non-ABI hard-register occurrences into connected
// def-use webs (`BuildTrialO0PseudoModel`).
struct TrialHardRegOcc {
  int hr = -1;
  RtlMachineMode mode = RtlMachineMode::kSi;
  int span = 1;
  bool pinned = false;
  bool is_set_dest = false;
  bool is_clobber = false;
};

struct TrialHardRegWebGraph {
  std::vector<TrialHardRegOcc> occs;
  absl::btree_map<const RtlExpr*, int> expr_to_occ;
  std::vector<int> parent;
  std::vector<std::vector<int>> pin_adj;

  int UfFind(int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }
  void UfUnion(int a, int b) {
    a = UfFind(a);
    b = UfFind(b);
    if (a != b) {
      parent[b] = a;
    }
  }
  void AddPinEdge(int a, int b) {
    pin_adj[a].push_back(b);
    pin_adj[b].push_back(a);
  }
};

constexpr int kPinnedTok = -1;
constexpr int kUninitTok = -2;
using TrialReach = std::array<std::vector<int>, kFirstPseudoRegister>;

void CollectTrialHardRegOccurrences(const TrialO0Model& m, bool gp64, bool fp64,
                                    TrialHardRegWebGraph* g) {
  auto is_cand_hr = [](const RtlExpr& e) {
    if (!e.IsHardReg() || e.flags.integrated) {
      return false;
    }
    const int r = e.RegNo();
    return (r >= 2 && r <= 25) || (r >= 32 && r <= 66);
  };
  auto reg_pat_occs = [&](const auto& self, const RtlExpr& e, bool in_dst, bool in_clob) -> void {
    if (is_cand_hr(e)) {
      const int id = static_cast<int>(g->occs.size());
      g->occs.push_back(TrialHardRegOcc{
          .hr = e.RegNo(),
          .mode = e.mode,
          .span = std::max(1, O0HardRegNregs(e.RegNo(), e.mode, gp64, fp64)),
          .is_set_dest = (!in_clob && in_dst),
          .is_clobber = in_clob,
      });
      g->expr_to_occ.emplace(&e, id);
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
  g->parent.resize(g->occs.size());
  for (size_t i = 0; i < g->occs.size(); ++i) {
    g->parent[i] = static_cast<int>(i);
  }
  g->pin_adj.resize(g->occs.size());
}

void StepTrialReachingDefsBlock(const TrialO0Model& m, size_t b, bool gp64, bool fp64, bool collect,
                                TrialHardRegWebGraph* g, TrialReach* st) {
  bool prev_was_fix_di_copy = false;
  int prev_fix_di_hr = -1;
  bool seen_di_subreg_in_blk[kFirstPseudoRegister] = {};
  for (size_t i = 0; i < m.blocks[b].size(); ++i) {
    const RtlPatternAndNotes& pn = m.blocks[b][i];
    const RtlExpr& pat = pn.pattern;
    if (IsCallPattern(pat)) {
      auto step_read = [&](const RtlExpr& e) {
        e.Visit([&](const RtlExpr& n) {
          if (auto it = g->expr_to_occ.find(&n); it != g->expr_to_occ.end() && collect) {
            const TrialHardRegOcc& o = g->occs[it->second];
            for (int w = o.hr; w < std::min(kFirstPseudoRegister, o.hr + o.span); ++w) {
              for (int t : (*st)[w]) {
                if (t == kPinnedTok) {
                  g->occs[it->second].pinned = true;
                } else if (t >= 0) {
                  g->AddPinEdge(t, it->second);
                  if (g->occs[t].hr == o.hr && g->occs[t].mode == o.mode) {
                    g->UfUnion(t, it->second);
                  } else {
                    g->occs[t].pinned = true;
                    g->occs[it->second].pinned = true;
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
          if (auto it = g->expr_to_occ.find(&n); it != g->expr_to_occ.end() && collect) {
            g->occs[it->second].pinned = true;
            const TrialHardRegOcc& o = g->occs[it->second];
            for (int w = o.hr; w < std::min(kFirstPseudoRegister, o.hr + o.span); ++w) {
              for (int t : (*st)[w]) {
                if (t >= 0) {
                  g->occs[t].pinned = true;
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
        if (auto it = g->expr_to_occ.find(&cset->SetDest());
            it != g->expr_to_occ.end() && collect) {
          g->occs[it->second].pinned = true;
        }
        const int r = cset->SetDest().RegNo();
        const int nw = std::max(1, O0HardRegNregs(r, cset->SetDest().mode, gp64, fp64));
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
        if (auto it = g->expr_to_occ.find(&in); it != g->expr_to_occ.end() && collect) {
          const TrialHardRegOcc& o = g->occs[it->second];
          const int w0 = o.hr + std::max(0, static_cast<int>(e.int_val));
          const int snw = std::max(1, O0HardRegNregs(w0, e.mode, gp64, fp64));
          for (int w = w0; w < std::min(kFirstPseudoRegister, w0 + snw); ++w) {
            for (int t : (*st)[w]) {
              if (t == kPinnedTok) {
                g->occs[it->second].pinned = true;
              } else if (t >= 0) {
                g->AddPinEdge(t, it->second);
                if (g->occs[t].hr == o.hr && g->occs[t].mode == o.mode) {
                  g->UfUnion(t, it->second);
                } else {
                  g->occs[t].pinned = true;
                  g->occs[it->second].pinned = true;
                }
              }
            }
          }
        }
        return;
      }
      if (auto it = g->expr_to_occ.find(&e); it != g->expr_to_occ.end()) {
        if (collect) {
          const TrialHardRegOcc& o = g->occs[it->second];
          for (int w = o.hr; w < std::min(kFirstPseudoRegister, o.hr + o.span); ++w) {
            for (int t : (*st)[w]) {
              if (t == kPinnedTok) {
                g->occs[it->second].pinned = true;
              } else if (t >= 0) {
                g->AddPinEdge(t, it->second);
                if (g->occs[t].hr == o.hr && g->occs[t].mode == o.mode) {
                  g->UfUnion(t, it->second);
                } else {
                  g->occs[t].pinned = true;
                  g->occs[it->second].pinned = true;
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
        const int snw = std::max(1, O0HardRegNregs(w0, dst.mode, gp64, fp64));
        if (auto it = g->expr_to_occ.find(&in);
            it != g->expr_to_occ.end() && !in.flags.integrated) {
          if (collect) {
            for (int w = w0; w < std::min(kFirstPseudoRegister, w0 + snw); ++w) {
              for (int t : (*st)[w]) {
                if (t >= 0 && g->occs[t].hr == g->occs[it->second].hr &&
                    g->occs[t].mode == g->occs[it->second].mode) {
                  g->UfUnion(t, it->second);
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
        const int nw = std::max(1, O0HardRegNregs(r, dst.mode, gp64, fp64));
        if (auto it = g->expr_to_occ.find(&dst);
            it != g->expr_to_occ.end() && !dst.flags.integrated) {
          if (collect) {
            if (unify_self_move && self_src_occ >= 0) {
              g->UfUnion(self_src_occ, it->second);
            }
            for (int w = r; w < std::min(kFirstPseudoRegister, r + nw); ++w) {
              for (int t : (*st)[w]) {
                if (t >= 0 && g->occs[t].is_clobber && g->occs[t].hr == g->occs[it->second].hr &&
                    g->occs[t].mode == g->occs[it->second].mode) {
                  g->UfUnion(t, it->second);
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
            is_di_self_move && (seen_di_subreg_in_blk[elem.SetDest().RegNo()] ||
                                (prev_was_fix_di_copy && elem.SetDest().RegNo() == prev_fix_di_hr));
        int self_src_occ = -1;
        if (unify_di_self) {
          if (auto it = g->expr_to_occ.find(&elem.SetSrc()); it != g->expr_to_occ.end()) {
            self_src_occ = it->second;
          }
        }
        step_reads(step_reads, elem.SetSrc());
        const bool dst_is_hard_subreg = elem.SetDest().op == RtlOp::kSubreg &&
                                        !elem.SetDest().children.empty() &&
                                        elem.SetDest().children[0].IsHardReg();
        if (!elem.SetDest().IsHardReg() && !dst_is_hard_subreg) {
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
          if (auto it = g->expr_to_occ.find(&elem.children[0]); it != g->expr_to_occ.end()) {
            const int r = elem.children[0].RegNo();
            const int nw = std::max(1, O0HardRegNregs(r, elem.children[0].mode, gp64, fp64));
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
}

// Coalesces intra-block same-`hr` RMW `(set hr (... hr ...))` webs under
// `-O0` `stupid.c:142-239` non-overlap SUID condition.
void CoalesceTrialIntraBlockRmwWebs(const TrialO0Model& m, TrialHardRegWebGraph* g) {
  std::vector<std::vector<int>> insn_suid(m.blocks.size());
  absl::btree_map<int, int> stupid_max_suid;
  absl::btree_map<int, absl::btree_set<int>> stupid_set_suids;
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
        if (auto it = g->expr_to_occ.find(&n); it != g->expr_to_occ.end()) {
          int& mx = stupid_max_suid[g->UfFind(it->second)];
          mx = std::max(mx, s);
        }
      });
      auto mark_set_suid = [&](const RtlExpr& elem) {
        if (elem.IsSet()) {
          const RtlExpr& d = elem.SetDest();
          const RtlExpr* key =
              (d.op == RtlOp::kSubreg && !d.children.empty()) ? &d.children[0] : &d;
          if (auto dit = g->expr_to_occ.find(key); dit != g->expr_to_occ.end()) {
            stupid_set_suids[g->UfFind(dit->second)].insert(s);
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
      auto dit = g->expr_to_occ.find(&s->SetDest());
      if (dit == g->expr_to_occ.end()) {
        continue;
      }
      auto walk_rmw = [&](const auto& self, const RtlExpr& n, bool inside_mem) -> void {
        if (n.op == RtlOp::kMem) {
          for (const RtlExpr& ch : n.children) {
            self(self, ch, /*inside_mem=*/true);
          }
          return;
        }
        if (auto sit = g->expr_to_occ.find(&n); sit != g->expr_to_occ.end()) {
          if (!inside_mem && g->occs[sit->second].hr == g->occs[dit->second].hr &&
              g->occs[sit->second].mode == g->occs[dit->second].mode) {
            const int root_r = g->UfFind(sit->second);
            const int root_w = g->UfFind(dit->second);
            bool has_dead = false;
            for (const RtlRegNote& note : pn.reg_notes) {
              if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg() &&
                  note.datum.RegNo() == g->occs[sit->second].hr) {
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
                absl::btree_set<int> merged_sets = stupid_set_suids[root_w];
                if (sets_it != stupid_set_suids.end()) {
                  merged_sets.insert(sets_it->second.begin(), sets_it->second.end());
                }
                g->UfUnion(sit->second, dit->second);
                const int new_root = g->UfFind(dit->second);
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
}

// Propagates `pinned` across union-find roots and `pin_adj`, assigns trial
// pseudos `>= 72` in ascending hard-reg order, and rewrites `m->blocks`.
int FinalizeTrialPseudoComponentsAndRewrite(TrialHardRegWebGraph* g, TrialO0Model* m) {
  absl::btree_map<int, std::vector<int>> members;
  for (size_t i = 0; i < g->occs.size(); ++i) {
    members[g->UfFind(static_cast<int>(i))].push_back(static_cast<int>(i));
  }
  std::vector<int> q;
  std::vector<bool> pinned(g->occs.size(), false);
  for (size_t i = 0; i < g->occs.size(); ++i) {
    if (g->occs[i].pinned) {
      pinned[i] = true;
      q.push_back(static_cast<int>(i));
    }
  }
  for (size_t h = 0; h < q.size(); ++h) {
    const int u = q[h];
    for (int mem : members[g->UfFind(u)]) {
      if (!pinned[mem]) {
        pinned[mem] = true;
        q.push_back(mem);
      }
    }
    for (int v : g->pin_adj[u]) {
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
  absl::btree_map<int, size_t> root_idx;
  for (size_t i = 0; i < g->occs.size(); ++i) {
    if (pinned[i]) {
      continue;
    }
    const int r = g->UfFind(static_cast<int>(i));
    auto [it, ins] = root_idx.try_emplace(r, comps.size());
    if (ins) {
      comps.push_back(Comp{.root = r, .hr = g->occs[i].hr, .mode = g->occs[i].mode});
    }
    if (g->occs[i].is_set_dest) {
      comps[it->second].has_set = true;
    }
  }
  std::stable_sort(comps.begin(), comps.end(),
                   [](const Comp& a, const Comp& b) { return a.hr < b.hr; });
  absl::btree_map<int, int> root_to_pseudo;
  int next_p = 72;
  for (const Comp& c : comps) {
    if (!c.has_set) {
      continue;
    }
    const int p = next_p++;
    root_to_pseudo[c.root] = p;
    m->reg_renumber[p] = c.hr;
    m->pseudo_modes[p] = c.mode;
  }

  auto rewrite_expr = [&](const auto& self, RtlExpr* orig_e, RtlExpr* dst_e) -> void {
    if (auto it = g->expr_to_occ.find(orig_e); it != g->expr_to_occ.end()) {
      const int r = g->UfFind(it->second);
      if (auto pit = root_to_pseudo.find(r); pit != root_to_pseudo.end()) {
        *dst_e = RtlExpr::MakeReg(dst_e->mode, pit->second, dst_e->flags);
        return;
      }
    }
    for (size_t k = 0; k < orig_e->children.size() && k < dst_e->children.size(); ++k) {
      self(self, &orig_e->children[k], &dst_e->children[k]);
    }
  };
  std::vector<std::vector<RtlPatternAndNotes>> rewritten = m->blocks;
  for (size_t b = 0; b < m->blocks.size(); ++b) {
    for (size_t i = 0; i < m->blocks[b].size(); ++i) {
      rewrite_expr(rewrite_expr, &m->blocks[b][i].pattern, &rewritten[b][i].pattern);
    }
  }
  m->blocks = std::move(rewritten);
  return next_p;
}

void CollectB0EntryParamCandidates(const TrialO0Model& model, int* hidden_sret_pseudo,
                                   size_t* b0_param_prefix_end,
                                   absl::btree_set<int>* b0_entry_param_cands) {
  bool has_return_use_insn = false;
  for (const auto& blk : model.blocks) {
    for (const RtlPatternAndNotes& pn : blk) {
      if (pn.pattern.op == RtlOp::kUse && pn.pattern.children.size() == 1 &&
          pn.pattern.children[0].flags.integrated) {
        has_return_use_insn = true;
      }
    }
  }

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
        *hidden_sret_pseudo = p0;
        b0_entry_param_cands->insert(p0);
        *b0_param_prefix_end = std::max<size_t>(*b0_param_prefix_end, 1);
      }
    }
  }

  absl::btree_map<int, size_t> b0_arg_pseudo_def;
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
      *b0_param_prefix_end = std::max(*b0_param_prefix_end, i + 1);
      continue;
    }
    if (dst.IsPseudoReg() && dst.mode == RtlMachineMode::kSi && src.op == RtlOp::kAshift &&
        src.children.size() == 2 && src.children[0].IsHardReg() && src.children[0].RegNo() >= 4 &&
        src.children[0].RegNo() <= 7 && !arg_written[src.children[0].RegNo()] &&
        src.children[1].op == RtlOp::kConstInt && src.children[1].int_val == 16) {
      b0_arg_pseudo_def[dst.RegNo()] = i;
      *b0_param_prefix_end = std::max(*b0_param_prefix_end, i + 1);
      continue;
    }
    if (dst.IsMem() && src.IsHardReg() && IsO0OutgoingCallArgHardReg(src.RegNo()) &&
        !arg_written[src.RegNo()]) {
      *b0_param_prefix_end = std::max(*b0_param_prefix_end, i + 1);
      continue;
    }
    if (dst.IsMem() && src.op == RtlOp::kSubreg && !src.children.empty() &&
        src.children[0].IsPseudoReg() && b0_arg_pseudo_def.contains(src.children[0].RegNo())) {
      b0_entry_param_cands->insert(src.children[0].RegNo());
      *b0_param_prefix_end = std::max(*b0_param_prefix_end, i + 1);
      continue;
    }
    if (dst.IsMem() && src.IsPseudoReg() && b0_arg_pseudo_def.contains(src.RegNo())) {
      b0_entry_param_cands->insert(src.RegNo());
      *b0_param_prefix_end = std::max(*b0_param_prefix_end, i + 1);
      continue;
    }
    if (dst.IsHardReg()) {
      arg_written[dst.RegNo()] = true;
    }
    break;
  }
}

void InsertSolvedO0ScopeUseMarkers(const O0PrologueInfo& pinfo, const TrialO0Model& model,
                                   const ScopeSolveResult& solved,
                                   std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  size_t b0_prefix_orig_pos = (solved.b0_param_prefix_end < model.orig_idx[0].size())
                                  ? model.orig_idx[0][solved.b0_param_prefix_end]
                                  : (*all_blocks)[0].size();

  // Insert `birth_locals` scope-start `(use (reg:M hr))` markers (`stmt.c:3498-3499`)
  // in descending `(birth_b, orig_birth_i)` order, stepping backward past any
  // single-instruction reload scratch feeding `orig_i`.
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
      const bool is_reload_scratch_hr =
          (scr >= 1 && scr <= 15) || scr == 24 || scr == 25 || (scr >= 32 && scr <= 51);
      if (!is_reload_scratch_hr || !blk[ins_i].pattern.ContainsReg(scr)) {
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

  // Scope-end `(use (reg:M hr))` markers (`stmt.c:3266-3272` `expand_end_bindings`).
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
}

// Reassigns unused synthesized `movstrsi_internal` (`config/mips/mips.md:3587-3601`)
// `match_scratch` clobbers beyond `used_slots` using `reload1.c:3606-3700`
// (`order_regs_for_reload`) priority: completely unused call-clobbered registers
// (`hard_reg_n_uses[r].uses == 0 && call_used_regs[r]`) are ordered ahead of
// registers referenced elsewhere in the function.
void ReassignUnusedO0MovstrsiScratchClobbers(
    const TrialO0Model& model, const ScopeSolveResult& solved, bool gp64, bool fp64,
    std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  std::array<bool, kFirstPseudoRegister> scope_live_to_exit{};
  for (const auto& [p, _] : solved.scope_kinds) {
    const int hr = model.reg_renumber.at(p);
    const int nw = std::max(1, O0HardRegNregs(hr, solved.tl.spans.at(p).mode, gp64, fp64));
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
        const int nw = std::max(1, O0HardRegNregs(r, n.mode, gp64, fp64));
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

bool IsO0OutgoingCallArgHardReg(int hr) {
  // `config/mips/mips.h:1915-1917` (`FUNCTION_ARG_REGNO_P`): `$a0..$a3` (4..7)
  // and `$f12..$f15` (44..47).
  return (hr >= 4 && hr <= 7) || (hr >= 44 && hr <= 47);
}

bool IsMipsO0CallUsedHardReg(int hr) {
  // `config/mips/mips.h:1203-1210` (`CALL_USED_REGISTERS`, excluding fixed
  // `$0/$k0/$k1/$gp/$sp/$fp`).
  return hr == 1 || (hr >= 2 && hr <= 15) || hr == 24 || hr == 25 || hr == 31 ||
         (hr >= 32 && hr <= 51) || (hr >= 64 && hr <= 67);
}

bool IsMipsO0CalleeSavedRange(int hr, int nw) {
  // `config/mips/mips.h:1203-1210`: callee-saved GPRs `$s0..$s7` (`16..23`)
  // and FPRs `$f20..$f31` (`52..63`).
  for (int w = hr; w < hr + nw; ++w) {
    const bool gpr_saved = (w >= 16 && w <= 23);
    const bool fpr_saved = (w >= 52 && w <= 63);
    if (!gpr_saved && !fpr_saved) {
      return false;
    }
  }
  return true;
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

bool IsO0FpInGprFallback(const O0PseudoSuidSpan& sp, int exp_hr) {
  return (sp.mode == RtlMachineMode::kSf || sp.mode == RtlMachineMode::kDf) && !sp.changes_size &&
         exp_hr >= 2 && exp_hr < 32;
}

std::optional<int64_t> MatchO0RegPlusConstOffset(const RtlExpr& addr, int base_reg) {
  if (addr.IsHardReg() && addr.RegNo() == base_reg) {
    return 0;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsHardReg() &&
      addr.children[0].RegNo() == base_reg && addr.children[1].op == RtlOp::kConstInt) {
    return addr.children[1].int_val;
  }
  return std::nullopt;
}

void WrapO0SingleBlockDiSubregs(size_t prologue_end,
                                std::vector<std::vector<RtlPatternAndNotes>>* blocks) {
  WrapO0NoConflictBlockDiSubregs(prologue_end, blocks);
  for (size_t b = 0; b < blocks->size(); ++b) {
    auto& blk = (*blocks)[b];
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
      if (TryWrapSingleBlockDiSubwordWrites(i, hr, di_flags, &blk)) {
        continue;
      }
      if (!set0->SetSrc().IsMem()) {
        TryWrapSingleBlockDiSubwordReads(i, hr, di_flags, &blk);
      }
    }
  }
}

O0OrderedScopes OrderO0Scopes(const O0StupidTimeline& tl,
                              const absl::btree_map<int, int>& exp_renumber,
                              const absl::btree_map<int, O0ScopeKind>& scope_kinds) {
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
                            const absl::btree_map<int, O0ScopeKind>& scope_kinds,
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

absl::btree_map<int, int> SimulateO0StupidAlloc(
    bool gp64, bool fp64, const O0StupidTimeline& tl, const absl::btree_map<int, int>& exp_renumber,
    const absl::btree_map<int, O0ScopeKind>& scope_kinds, const O0OrderedScopes& ord) {
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
  // Mirrors `stupid.c:287-306` (`stupid_reg_compare`): longest live span first,
  // then fewest references, then ascending pseudo register number.
  std::sort(items.begin(), items.end(), [](const SimItem& a, const SimItem& b) {
    if (a.alloc_len != b.alloc_len) {
      return a.alloc_len > b.alloc_len;
    }
    if (a.refs != b.refs) {
      return a.refs < b.refs;
    }
    return a.p < b.p;
  });

  // Mirrors `stupid.c:323-405` (`stupid_find_reg`) with natural MIPS allocation
  // order `0 .. FIRST_PSEUDO_REGISTER - 1` (`config/mips/mips.h` omits
  // `REG_ALLOC_ORDER`).
  auto find_in_class = [&](int cls, RtlMachineMode mode, int born, int dead, int calls,
                           bool changes_size) -> int {
    std::array<bool, 68> used{};
    // `stupid.c:340-351`: fixed regs (`config/mips/mips.h:1186-1193`) plus
    // eliminable/frame pointer (`config/mips/mips.h:1734-1749`).
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
      const int nw = std::max(1, O0HardRegNregs(r, mode, gp64, fp64));
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

  absl::btree_map<int, int> sim_renum;
  // `stupid.c:261-277`: try preferred class, then alternate class (`FP_REGS` ->
  // `GR_REGS` fallback on MIPS).
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

void FoldTrialO0ReloadScratchesAndMulsidi3Spills(const O0PrologueInfo& pinfo, bool gp64,
                                                 int* next_p, TrialO0Model* model) {
  FoldTrialO0Mulsidi3TopSpills(pinfo, next_p, model);

  absl::btree_map<int, int> trial_ref_counts;
  absl::btree_set<int> trial_use_pseudos;
  for (const auto& blk : model->blocks) {
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
  for (size_t b = 0; b < model->blocks.size(); ++b) {
    auto& blk = model->blocks[b];
    for (size_t i = 0; i + 1 < blk.size(); ++i) {
      if (TryFoldTrialZeroDiStoreScratch(gp64, trial_ref_counts, trial_use_pseudos, b, &i, model)) {
        continue;
      }
      if (TryFoldTrialMulsidi3OutputReload(trial_ref_counts, trial_use_pseudos, b, i, model)) {
        continue;
      }
      if (TryFoldTrialEarlyclobberDiOutputReload(gp64, trial_ref_counts, trial_use_pseudos, b, i,
                                                 model)) {
        continue;
      }
      TryFoldTrialMdSiAndFprReloads(trial_ref_counts, trial_use_pseudos, b, &i, model);
    }
  }
}

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
  WrapO0SingleBlockDiSubregs(/*prologue_end=*/0, &m.blocks);

  TrialHardRegWebGraph graph;
  CollectTrialHardRegOccurrences(m, gp64, fp64, &graph);

  std::vector<TrialReach> reach_in(m.blocks.size());
  if (!m.blocks.empty()) {
    for (int w = 0; w < kFirstPseudoRegister; ++w) {
      if (IsO0OutgoingCallArgHardReg(w)) {
        reach_in[0][w].push_back(kPinnedTok);
      } else if ((w >= 2 && w <= 25) || (w >= 32 && w <= 66)) {
        reach_in[0][w].push_back(kUninitTok);
      }
    }
  }
  auto merge_reach = [](const TrialReach& src, TrialReach* dst) {
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
  bool ch_df = true;
  while (ch_df) {
    ch_df = false;
    for (size_t b = 0; b < m.blocks.size(); ++b) {
      TrialReach st = reach_in[b];
      StepTrialReachingDefsBlock(m, b, gp64, fp64, /*collect=*/false, &graph, &st);
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
    TrialReach st = reach_in[b];
    StepTrialReachingDefsBlock(m, b, gp64, fp64, /*collect=*/true, &graph, &st);
  }

  CoalesceTrialIntraBlockRmwWebs(m, &graph);
  int next_p = FinalizeTrialPseudoComponentsAndRewrite(&graph, &m);
  FoldTrialO0ReloadScratchesAndMulsidi3Spills(pinfo, gp64, &next_p, &m);
  return m;
}

O0StupidTimeline BuildO0StupidTimeline(bool gp64, bool fp64, const TrialO0Model& model,
                                       size_t b0_param_prefix_end,
                                       const absl::btree_set<int>& b0_entry_param_cands) {
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

  // Backward scan mirroring `stupid.c:200-239` and `stupid_mark_refs` (`412-558`).
  std::array<bool, 68> regs_live{};
  int last_call_suid = tl.exit_suid + 10;
  auto mark_refs = [&](const auto& self, const RtlExpr& expr, int suid, size_t b,
                       size_t i) -> void {
    if (expr.op == RtlOp::kSet || expr.op == RtlOp::kClobber) {
      if (!expr.children.empty() && expr.children[0].op == RtlOp::kReg &&
          !expr.children[0].IsArgPointerReg()) {
        const int r = expr.children[0].RegNo();
        if (r >= 0 && r < 68) {
          const int nw = std::max(1, O0HardRegNregs(r, expr.children[0].mode, gp64, fp64));
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
          O0ModeByteSize(expr.mode) != O0ModeByteSize(expr.children[0].mode)) {
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
          const int nw = std::max(1, O0HardRegNregs(r, expr.mode, gp64, fp64));
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
      // `stupid.c:207-209`: OR `regs_live` into `after_insn_hard_regs[suid]`
      // rather than overwriting, preserving bits already set at `suid` by a
      // following `CLOBBER` (`stupid.c:457-459`).
      for (int h = 0; h < 68; ++h) {
        if (regs_live[h]) {
          tl.after_hr[suid][h] = true;
        }
      }
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
        for (int h = 0; h < 68; ++h) {
          if (regs_live[h]) {
            tl.after_hr[suid - 1][h] = true;
            tl.after_hr[suid - 2][h] = true;
          }
        }
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
  absl::btree_map<int, int> canon_for_hr;
  absl::btree_map<int, int> remap;
  for (const auto& [p, hr] : model->reg_renumber) {
    if (hr >= 52 && hr < 64) {
      auto [it, ins] = canon_for_hr.emplace(hr, p);
      if (!ins) {
        remap[p] = it->second;
      }
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
  alive.reserve(model->reg_renumber.size());
  for (const auto& [p, _] : model->reg_renumber) {
    alive.push_back(p);
  }
  std::stable_sort(alive.begin(), alive.end(), [&](int a, int b) {
    return model->reg_renumber.at(a) < model->reg_renumber.at(b);
  });
  absl::btree_map<int, int> renum_p;
  absl::btree_map<int, int> new_hr;
  absl::btree_map<int, RtlMachineMode> new_m;
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

ScopeSolveResult SolveO0ScopesOnModel(bool gp64, bool fp64, const TrialO0Model& model) {
  ScopeSolveResult res;
  if (model.blocks.empty()) {
    res.matched = true;
    return res;
  }

  absl::btree_set<int> b0_entry_param_cands;
  CollectB0EntryParamCandidates(model, &res.hidden_sret_pseudo, &res.b0_param_prefix_end,
                                &b0_entry_param_cands);

  res.tl = BuildO0StupidTimeline(gp64, fp64, model, res.b0_param_prefix_end, b0_entry_param_cands);
  const O0StupidTimeline& tl = res.tl;

  auto is_compatible_from = [&](int p, const O0PseudoSuidSpan& sp, int start_suid) {
    if (sp.born == kMaxInt || sp.has_explicit_use_insn) {
      return false;
    }
    const int hr = model.reg_renumber.at(p);
    const int nw = std::max(1, O0HardRegNregs(hr, sp.mode, gp64, fp64));
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
      const int qnw = std::max(1, O0HardRegNregs(qhr, qsp.mode, gp64, fp64));
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
    const absl::btree_map<int, int> sim =
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
      const int nw = std::max(1, O0HardRegNregs(hr, sp.mode, gp64, fp64));
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
        const int qnw = std::max(1, O0HardRegNregs(got_hr, qsp.mode, gp64, fp64));
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
  const absl::btree_map<int, int> final_sim =
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
  InsertSolvedO0ScopeUseMarkers(pinfo, model, solved, all_blocks);
  ReassignUnusedO0MovstrsiScratchClobbers(model, solved, gp64, fp64, all_blocks);
}

}  // namespace rom_nom_nom::lifter
