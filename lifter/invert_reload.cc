#include "lifter/invert_reload.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kStackPointerRegnum = 29;
constexpr int kFramePointerRegnum = 30;
constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;

int ModeByteSize(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kQI:
      return 1;
    case RtlMachineMode::kHI:
      return 2;
    case RtlMachineMode::kDI:
    case RtlMachineMode::kDF:
      return 8;
    case RtlMachineMode::kTI:
    case RtlMachineMode::kTF:
      return 16;
    default:
      return 4;
  }
}

// Non-fixed `CALL_USED_REGISTERS` eligible for `caller-save.c` on MIPS
// (`mips.h:1186-1210`): `$2..$15` (`v0..t7`), `$24..$25` (`t8..t9`), and
// `$32..$51` (`$f0..$f19`).
bool IsMipsCallerSavableHardReg(int hr) {
  return (hr >= 2 && hr <= 15) || (hr >= 24 && hr <= 25) || (hr >= 32 && hr <= 51);
}

bool IsCallPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kCall) {
    return true;
  }
  if (pat.op == RtlOp::kSet && pat.children.size() >= 2 && pat.children[1].op == RtlOp::kCall) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (IsCallPattern(elem)) {
        return true;
      }
    }
  }
  return false;
}

std::optional<int64_t> MatchStackSlotAddrOffset(const RtlExpr& addr, bool frame_pointer_needed,
                                                int64_t min_offset) {
  auto is_valid_base = [&](const RtlExpr& base) -> bool {
    if (base.op != RtlOp::kReg || base.mode != RtlMachineMode::kSI || base.flags.Any()) {
      return false;
    }
    if (frame_pointer_needed) {
      return base.RegNo() == kFramePointerRegnum;
    }
    return base.RegNo() == kStackPointerRegnum || base.RegNo() == kFramePointerRegnum;
  };

  int64_t offset = -1;
  if (is_valid_base(addr)) {
    offset = 0;
  } else if (addr.op == RtlOp::kPlus && addr.mode == RtlMachineMode::kSI &&
             addr.children.size() == 2 && is_valid_base(addr.children[0]) &&
             addr.children[1].op == RtlOp::kConstInt) {
    offset = addr.children[1].int_val;
  } else {
    return std::nullopt;
  }
  if (offset < min_offset) {
    return std::nullopt;
  }
  return offset;
}

std::optional<int64_t> MatchCallerSaveStackSlotOffset(const RtlExpr& mem_expr,
                                                      bool frame_pointer_needed,
                                                      int64_t outgoing_args_size) {
  if (mem_expr.op != RtlOp::kMem || mem_expr.flags.Any() || mem_expr.children.size() != 1) {
    return std::nullopt;
  }
  return MatchStackSlotAddrOffset(mem_expr.children[0], frame_pointer_needed, outgoing_args_size);
}

// Reverses `caller-save.c:save_call_clobbered_regs` save/restore pairs inside
// a single basic block when `caller_save_candidate_hard_regs` is non-empty,
// recording stripped `(hard_reg -> stack_offset)` entries into
// `stripped_hr_offsets`.
void ReverseCallerSaveInsnsInBlock(const absl::flat_hash_set<int>& caller_save_candidate_hard_regs,
                                   bool frame_pointer_needed, int64_t outgoing_args_size,
                                   std::vector<RtlPatternAndNotes>* insns,
                                   absl::flat_hash_map<int, int64_t>* stripped_hr_offsets) {
  if (insns == nullptr || insns->empty() || caller_save_candidate_hard_regs.empty()) {
    return;
  }

  std::vector<size_t> call_indices;
  for (size_t i = 0; i < insns->size(); ++i) {
    if (IsCallPattern((*insns)[i].pattern)) {
      call_indices.push_back(i);
    }
  }
  if (call_indices.empty()) {
    return;
  }

  std::vector<bool> drop(insns->size(), false);

  struct ActiveSaveWord {
    size_t save_insn_idx = 0;
    int total_words = 1;
    int64_t offset = 0;
  };
  absl::flat_hash_map<int, ActiveSaveWord> active_saves;
  absl::flat_hash_map<size_t, int> restored_words_per_save;
  absl::flat_hash_map<size_t, std::vector<size_t>> restore_indices_per_save;
  absl::flat_hash_map<size_t, std::vector<std::pair<int, int64_t>>> save_word_slots;

  // Scan the block tracking candidate saves before calls and matching restores
  // after calls (`caller-save.c:399-504`).
  std::vector<std::pair<size_t, std::vector<std::pair<int, int64_t>>>> pending_saves_before_call;

  for (size_t i = 0; i < insns->size(); ++i) {
    const RtlPatternAndNotes& insn = (*insns)[i];
    if (IsCallPattern(insn.pattern)) {
      for (const auto& [save_idx, words] : pending_saves_before_call) {
        const int total_w = static_cast<int>(words.size());
        for (const auto& [w_hr, w_off] : words) {
          active_saves[w_hr] = ActiveSaveWord{
              .save_insn_idx = save_idx,
              .total_words = total_w,
              .offset = w_off,
          };
        }
      }
      pending_saves_before_call.clear();
      continue;
    }

    if (insn.reg_notes.empty() && insn.pattern.op == RtlOp::kSet &&
        insn.pattern.children.size() == 2) {
      const RtlExpr& dst = insn.pattern.children[0];
      const RtlExpr& src = insn.pattern.children[1];

      // Candidate restore after a call: `(set (reg:M hr) (mem:M [sp + off]))`.
      if (dst.op == RtlOp::kReg && !dst.flags.Any() && dst.mode == src.mode) {
        const int hr = dst.RegNo();
        const int nwords = std::max(1, (ModeByteSize(dst.mode) + 3) / 4);
        std::optional<int64_t> off =
            MatchCallerSaveStackSlotOffset(src, frame_pointer_needed, outgoing_args_size);
        if (off.has_value() && active_saves.contains(hr)) {
          bool all_match = true;
          const size_t save_idx = active_saves[hr].save_insn_idx;
          for (int k = 0; k < nwords; ++k) {
            auto it = active_saves.find(hr + k);
            if (it == active_saves.end() || it->second.save_insn_idx != save_idx ||
                it->second.offset != *off + 4 * k) {
              all_match = false;
              break;
            }
          }
          if (all_match) {
            restore_indices_per_save[save_idx].push_back(i);
            restored_words_per_save[save_idx] += nwords;
            const int expected_total = active_saves[hr].total_words;
            for (int k = 0; k < nwords; ++k) {
              active_saves.erase(hr + k);
            }
            if (restored_words_per_save[save_idx] == expected_total) {
              drop[save_idx] = true;
              for (size_t r_idx : restore_indices_per_save[save_idx]) {
                drop[r_idx] = true;
              }
              if (stripped_hr_offsets != nullptr) {
                for (const auto& [s_hr, s_off] : save_word_slots[save_idx]) {
                  (*stripped_hr_offsets)[s_hr] = s_off;
                }
              }
            }
            continue;
          }
        }
      }

      // Candidate save before an upcoming call: `(set (mem:M [sp + off]) (reg:M hr))`.
      if (src.op == RtlOp::kReg && !src.flags.Any() && dst.mode == src.mode &&
          i < call_indices.back()) {
        const int hr = src.RegNo();
        const int nwords = std::max(1, (ModeByteSize(src.mode) + 3) / 4);
        std::optional<int64_t> off =
            MatchCallerSaveStackSlotOffset(dst, frame_pointer_needed, outgoing_args_size);
        if (off.has_value()) {
          bool all_cand = true;
          std::vector<std::pair<int, int64_t>> words;
          for (int k = 0; k < nwords; ++k) {
            if (!caller_save_candidate_hard_regs.contains(hr + k)) {
              all_cand = false;
              break;
            }
            words.emplace_back(hr + k, *off + 4 * k);
          }
          if (all_cand) {
            save_word_slots[i] = words;
            pending_saves_before_call.emplace_back(i, std::move(words));
            continue;
          }
        }
      }
    }

    // Any non-save instruction that writes to a hard reg invalidates an un-crossed
    // pending save or an un-restored post-call save (`caller-save.c:536-541`).
    auto invalidate_written_reg = [&](const RtlExpr& elem) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty() &&
          elem.children[0].op == RtlOp::kReg) {
        const int wr = elem.children[0].RegNo();
        const int nw = std::max(1, (ModeByteSize(elem.children[0].mode) + 3) / 4);
        for (int k = 0; k < nw; ++k) {
          active_saves.erase(wr + k);
          pending_saves_before_call.erase(
              std::remove_if(pending_saves_before_call.begin(), pending_saves_before_call.end(),
                             [&](const auto& entry) {
                               for (const auto& [whr, _] : entry.second) {
                                 if (whr == wr + k) return true;
                               }
                               return false;
                             }),
              pending_saves_before_call.end());
        }
      }
    };
    if (insn.pattern.op == RtlOp::kParallel) {
      for (const RtlExpr& elem : insn.pattern.children) {
        invalidate_written_reg(elem);
      }
    } else {
      invalidate_written_reg(insn.pattern);
    }
  }

  std::vector<RtlPatternAndNotes> filtered;
  filtered.reserve(insns->size());
  for (size_t i = 0; i < insns->size(); ++i) {
    if (!drop[i]) {
      filtered.push_back(std::move((*insns)[i]));
    }
  }
  *insns = std::move(filtered);
}

bool IsControlFlowBoundaryPattern(const RtlExpr& pat) {
  if (IsCallPattern(pat) || pat.op == RtlOp::kCodeLabel || pat.op == RtlOp::kBarrier ||
      pat.op == RtlOp::kReturn || pat.op == RtlOp::kAddrVec || pat.op == RtlOp::kAddrDiffVec) {
    return true;
  }
  if (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (IsControlFlowBoundaryPattern(elem)) {
        return true;
      }
    }
  }
  return false;
}

bool InsnWritesOrClobbersReg(const RtlExpr& pat, int regno) {
  auto check_elem = [&](const RtlExpr& elem) -> bool {
    if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
      const RtlExpr* dst = &elem.children[0];
      while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (dst->op == RtlOp::kReg) {
        const int wr = dst->RegNo();
        const int nw = (wr >= 0 && wr < kFirstPseudoRegister)
                           ? std::max(1, (ModeByteSize(dst->mode) + 3) / 4)
                           : 1;
        if (regno >= wr && regno < wr + nw) {
          return true;
        }
      }
    }
    return false;
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (check_elem(elem)) {
        return true;
      }
    }
    return false;
  }
  return check_elem(pat);
}

bool ReplaceRegInExpr(RtlExpr* expr, int target_regno, RtlMachineMode target_mode,
                      const RtlExpr& replacement) {
  if (expr == nullptr) {
    return false;
  }
  if (expr->op == RtlOp::kReg && expr->RegNo() == target_regno && expr->mode == target_mode) {
    *expr = replacement;
    return true;
  }
  bool changed = false;
  for (RtlExpr& child : expr->children) {
    if (ReplaceRegInExpr(&child, target_regno, target_mode, replacement)) {
      changed = true;
    }
  }
  return changed;
}

// Replaces reads of `(reg:target_mode target_regno)` inside `pat` (excluding
// bare register destinations of `SET`/`CLOBBER`) with `replacement`.
bool ReplaceReadRegInPattern(RtlExpr* pat, int target_regno, RtlMachineMode target_mode,
                             const RtlExpr& replacement) {
  auto replace_elem = [&](RtlExpr* elem) -> bool {
    if ((elem->op == RtlOp::kSet || elem->op == RtlOp::kClobber) && !elem->children.empty()) {
      bool changed = false;
      RtlExpr& dst = elem->children[0];
      if (dst.op == RtlOp::kMem) {
        if (ReplaceRegInExpr(&dst, target_regno, target_mode, replacement)) {
          changed = true;
        }
      } else if ((dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract) &&
                 dst.children.size() >= 3) {
        for (size_t k = 1; k < dst.children.size(); ++k) {
          if (ReplaceRegInExpr(&dst.children[k], target_regno, target_mode, replacement)) {
            changed = true;
          }
        }
      }
      for (size_t k = 1; k < elem->children.size(); ++k) {
        if (ReplaceRegInExpr(&elem->children[k], target_regno, target_mode, replacement)) {
          changed = true;
        }
      }
      return changed;
    }
    return ReplaceRegInExpr(elem, target_regno, target_mode, replacement);
  };
  if (pat->op == RtlOp::kParallel) {
    bool changed = false;
    for (RtlExpr& elem : pat->children) {
      if (replace_elem(&elem)) {
        changed = true;
      }
    }
    return changed;
  }
  return replace_elem(pat);
}

std::optional<std::pair<int64_t, RtlMachineMode>> MatchScalarSpillSlotMem(const RtlExpr& mem_expr,
                                                                          bool frame_pointer_needed,
                                                                          int64_t min_offset,
                                                                          int64_t max_offset) {
  if (mem_expr.op != RtlOp::kMem || mem_expr.flags.volatil || mem_expr.flags.in_struct ||
      mem_expr.children.size() != 1) {
    return std::nullopt;
  }
  std::optional<int64_t> off =
      MatchStackSlotAddrOffset(mem_expr.children[0], frame_pointer_needed, min_offset);
  if (!off.has_value() || *off >= max_offset) {
    return std::nullopt;
  }
  return std::make_pair(*off, mem_expr.mode);
}

void ReplaceSpillSlotMemsInExpr(RtlExpr* expr, bool frame_pointer_needed, int64_t min_offset,
                                int64_t max_offset,
                                const absl::flat_hash_map<int64_t, RtlExpr>& slot_to_pseudo) {
  if (expr == nullptr) {
    return;
  }
  std::optional<std::pair<int64_t, RtlMachineMode>> m =
      MatchScalarSpillSlotMem(*expr, frame_pointer_needed, min_offset, max_offset);
  if (m.has_value()) {
    auto it = slot_to_pseudo.find(m->first);
    if (it != slot_to_pseudo.end() && it->second.mode == m->second) {
      *expr = it->second;
      return;
    }
  }
  for (RtlExpr& child : expr->children) {
    ReplaceSpillSlotMemsInExpr(&child, frame_pointer_needed, min_offset, max_offset,
                               slot_to_pseudo);
  }
}

void ReconstructSpilledPseudosInBlock(bool frame_pointer_needed, int64_t min_offset,
                                      int64_t max_offset,
                                      const absl::flat_hash_map<int64_t, RtlExpr>& slot_to_pseudo,
                                      std::vector<RtlPatternAndNotes>* insns) {
  if (insns == nullptr || insns->empty() || slot_to_pseudo.empty()) {
    return;
  }

  auto match_slot = [&](const RtlExpr& expr) -> std::optional<RtlExpr> {
    std::optional<std::pair<int64_t, RtlMachineMode>> m =
        MatchScalarSpillSlotMem(expr, frame_pointer_needed, min_offset, max_offset);
    if (!m.has_value()) {
      return std::nullopt;
    }
    auto it = slot_to_pseudo.find(m->first);
    if (it == slot_to_pseudo.end() || it->second.mode != m->second) {
      return std::nullopt;
    }
    return it->second;
  };

  // Pass 1: Fold bare input reloads `(set (reg:M r_rel) slot_mem_k)` into the
  // subsequent instruction(s) reading `r_rel` before `r_rel` is overwritten.
  std::vector<bool> drop_input(insns->size(), false);
  for (size_t i = 0; i < insns->size(); ++i) {
    const RtlPatternAndNotes& insn = (*insns)[i];
    if (!insn.reg_notes.empty() || insn.pattern.op != RtlOp::kSet ||
        insn.pattern.children.size() != 2) {
      continue;
    }
    const RtlExpr& dst = insn.pattern.children[0];
    if (dst.op != RtlOp::kReg || dst.RegNo() < 0 || dst.RegNo() >= kFirstPseudoRegister) {
      continue;
    }
    std::optional<RtlExpr> pseudo = match_slot(insn.pattern.children[1]);
    if (!pseudo.has_value() || pseudo->mode != dst.mode) {
      continue;
    }
    const int r_rel = dst.RegNo();
    bool replaced_any = false;
    for (size_t j = i + 1; j < insns->size(); ++j) {
      if (drop_input[j]) {
        continue;
      }
      if (ReplaceReadRegInPattern(&(*insns)[j].pattern, r_rel, dst.mode, *pseudo)) {
        replaced_any = true;
      }
      if (InsnWritesOrClobbersReg((*insns)[j].pattern, r_rel) ||
          InsnWritesOrClobbersReg((*insns)[j].pattern, pseudo->RegNo()) ||
          IsControlFlowBoundaryPattern((*insns)[j].pattern)) {
        break;
      }
    }
    if (replaced_any) {
      drop_input[i] = true;
    }
  }

  std::vector<RtlPatternAndNotes> after_inputs;
  after_inputs.reserve(insns->size());
  for (size_t i = 0; i < insns->size(); ++i) {
    if (!drop_input[i]) {
      after_inputs.push_back(std::move((*insns)[i]));
    }
  }

  // Pass 2: Fold output reloads `(set (reg:M r_rel) <rhs>)` immediately
  // followed by bare `(set slot_mem_k (reg:M r_rel))`, propagating `pseudo_k`
  // to any immediately following reload-inherited reads of `r_rel`.
  std::vector<bool> drop_output(after_inputs.size(), false);
  for (size_t i = 0; i + 1 < after_inputs.size(); ++i) {
    if (drop_output[i]) {
      continue;
    }
    RtlPatternAndNotes& def_insn = after_inputs[i];
    const RtlPatternAndNotes& store_insn = after_inputs[i + 1];
    if (def_insn.pattern.op != RtlOp::kSet || def_insn.pattern.children.size() != 2 ||
        !store_insn.reg_notes.empty() || store_insn.pattern.op != RtlOp::kSet ||
        store_insn.pattern.children.size() != 2) {
      continue;
    }
    const RtlExpr& def_dst = def_insn.pattern.children[0];
    const RtlExpr& store_src = store_insn.pattern.children[1];
    if (def_dst.op != RtlOp::kReg || store_src.op != RtlOp::kReg ||
        def_dst.RegNo() != store_src.RegNo() || def_dst.mode != store_src.mode ||
        def_dst.RegNo() < 0 || def_dst.RegNo() >= kFirstPseudoRegister) {
      continue;
    }
    std::optional<RtlExpr> pseudo = match_slot(store_insn.pattern.children[0]);
    if (!pseudo.has_value() || pseudo->mode != def_dst.mode) {
      continue;
    }
    const int r_rel = def_dst.RegNo();
    const RtlMachineMode rel_mode = def_dst.mode;
    def_insn.pattern.children[0] = *pseudo;
    drop_output[i + 1] = true;

    for (size_t m = i + 2; m < after_inputs.size(); ++m) {
      ReplaceReadRegInPattern(&after_inputs[m].pattern, r_rel, rel_mode, *pseudo);
      if (InsnWritesOrClobbersReg(after_inputs[m].pattern, r_rel) ||
          InsnWritesOrClobbersReg(after_inputs[m].pattern, pseudo->RegNo()) ||
          IsControlFlowBoundaryPattern(after_inputs[m].pattern)) {
        break;
      }
    }
  }

  std::vector<RtlPatternAndNotes> result;
  result.reserve(after_inputs.size());
  for (size_t i = 0; i < after_inputs.size(); ++i) {
    if (drop_output[i]) {
      continue;
    }
    // Pass 3: Rewrite any remaining direct `alter_reg` spill slot `MEM`s in
    // patterns and `REG_NOTES` (such as `REG_DEAD (mem:SI [sp + off_k])`) back
    // to their spilled pseudo `(reg:M p_k)`.
    ReplaceSpillSlotMemsInExpr(&after_inputs[i].pattern, frame_pointer_needed, min_offset,
                               max_offset, slot_to_pseudo);
    for (RtlRegNote& note : after_inputs[i].reg_notes) {
      ReplaceSpillSlotMemsInExpr(&note.datum, frame_pointer_needed, min_offset, max_offset,
                                 slot_to_pseudo);
    }
    result.push_back(std::move(after_inputs[i]));
  }

  *insns = std::move(result);
}

// Recursively validates all `(reg ...)` nodes in `expr`.
absl::Status ValidateRtlExprRegisters(const RtlExpr& expr) {
  if (expr.op == RtlOp::kReg) {
    if (expr.int_val < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: invalid negative register number %d", expr.int_val));
    }
  }
  for (const RtlExpr& child : expr.children) {
    if (absl::Status st = ValidateRtlExprRegisters(child); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

bool IsStackPointerRegExpr(const RtlExpr& expr) {
  return expr.op == RtlOp::kReg && expr.mode == RtlMachineMode::kSi && !expr.flags.Any() &&
         expr.int_val == kStackPointerRegnum;
}

bool SingleElementModifiesStackPointer(const RtlExpr& expr) {
  if ((expr.op == RtlOp::kSet || expr.op == RtlOp::kClobber) && !expr.children.empty()) {
    const RtlExpr* dst = &expr.children[0];
    while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
      dst = &dst->children[0];
    }
    if (dst->op == RtlOp::kReg && dst->int_val == kStackPointerRegnum) {
      return true;
    }
  }
  return false;
}

bool ShouldSkipFramePointerRestorationForInsn(const RtlExpr& pattern) {
  if ((pattern.op == RtlOp::kUse || pattern.op == RtlOp::kClobber) && !pattern.children.empty() &&
      pattern.children[0].op != RtlOp::kMem) {
    return true;
  }
  if (SingleElementModifiesStackPointer(pattern)) {
    return true;
  }
  if (pattern.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pattern.children) {
      if (SingleElementModifiesStackPointer(elem)) {
        return true;
      }
    }
  }
  return false;
}

// Rewrites `(reg:SI 29 sp)` back to `(reg:SI 30 fp)` inside `expr`, reversing
// `reload1.c:eliminate_regs` when `!frame_pointer_needed`.
bool ReverseEliminateRegsInExpr(RtlExpr* expr) {
  if (expr == nullptr) {
    return false;
  }
  if (IsStackPointerRegExpr(*expr)) {
    expr->SetRegNo(kFramePointerRegnum);
    return true;
  }

  bool changed = false;
  if (expr->op == RtlOp::kSet && expr->children.size() >= 2) {
    // Never rewrite a bare `(reg:SI 29 sp)` as the direct `SET_DEST` of a `SET`
    // because `eliminate_regs_in_insn` deletes any single-set whose `SET_DEST`
    // is an eliminable register (`fp`).
    if (!IsStackPointerRegExpr(expr->children[0])) {
      if (ReverseEliminateRegsInExpr(&expr->children[0])) {
        changed = true;
      }
    }
    for (size_t i = 1; i < expr->children.size(); ++i) {
      if (ReverseEliminateRegsInExpr(&expr->children[i])) {
        changed = true;
      }
    }
    return changed;
  }

  for (RtlExpr& child : expr->children) {
    if (ReverseEliminateRegsInExpr(&child)) {
      changed = true;
    }
  }
  return changed;
}

// Checks whether `insn` is a bare hard-register to hard-register move
// `(set (reg:M dst) (reg:M src))` with no `REG_NOTES`, returning `{dst, src}`.
std::optional<std::pair<int, int>> MatchBareHardRegMove(const RtlPatternAndNotes& insn) {
  if (!insn.reg_notes.empty()) {
    return std::nullopt;
  }
  const RtlExpr& pat = insn.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return std::nullopt;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || src.op != RtlOp::kReg || dst.mode != src.mode) {
    return std::nullopt;
  }
  const int dst_reg = dst.RegNo();
  const int src_reg = src.RegNo();
  if (dst_reg < 0 || dst_reg >= kFirstPseudoRegister || src_reg < 0 ||
      src_reg >= kFirstPseudoRegister) {
    return std::nullopt;
  }
  return std::make_pair(dst_reg, src_reg);
}

// Reverses `reload1.c:emit_reload_insns` / `subst_reloads` for earlyclobber
// `(clobber (reg:M ...))` operands in a `PARALLEL` instruction where `reload`
// substituted a reload register `r_reload` into `(clobber (reg:M r_reload))`
// and emitted `(set (reg:M r_orig) (reg:M r_reload))` immediately after the
// `PARALLEL`.
void ReverseParallelClobberOutputReloads(std::vector<RtlPatternAndNotes>* insns,
                                         absl::flat_hash_set<int>* reversed_reload_regs) {
  if (insns == nullptr || insns->empty()) {
    return;
  }
  std::vector<RtlPatternAndNotes> out;
  out.reserve(insns->size());

  for (size_t i = 0; i < insns->size(); ++i) {
    RtlPatternAndNotes current = (*insns)[i];
    if (current.pattern.op == RtlOp::kParallel) {
      absl::flat_hash_set<int> set_dest_regs;
      absl::flat_hash_set<int> set_referenced_regs;
      absl::flat_hash_map<int, size_t> clobber_reg_to_child_idx;

      for (size_t c = 0; c < current.pattern.children.size(); ++c) {
        const RtlExpr& elem = current.pattern.children[c];
        if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
          if (elem.children[0].op == RtlOp::kReg) {
            set_dest_regs.insert(elem.children[0].RegNo());
          }
          elem.Visit([&](const RtlExpr& node) {
            if (node.op == RtlOp::kReg) {
              set_referenced_regs.insert(node.RegNo());
            }
          });
        } else if (elem.op == RtlOp::kClobber && !elem.children.empty() &&
                   elem.children[0].op == RtlOp::kReg) {
          const int clob_reg = elem.children[0].RegNo();
          if (clob_reg >= 0 && clob_reg < kFirstPseudoRegister) {
            clobber_reg_to_child_idx[clob_reg] = c;
          }
        }
      }

      if (!clobber_reg_to_child_idx.empty()) {
        size_t lookahead = i + 1;
        while (lookahead < insns->size()) {
          std::optional<std::pair<int, int>> move = MatchBareHardRegMove((*insns)[lookahead]);
          if (!move.has_value()) {
            break;
          }
          const auto [orig_reg, reload_reg] = *move;
          auto clob_it = clobber_reg_to_child_idx.find(reload_reg);
          if (clob_it == clobber_reg_to_child_idx.end() || set_dest_regs.contains(reload_reg) ||
              !set_referenced_regs.contains(orig_reg)) {
            break;
          }
          const size_t child_idx = clob_it->second;
          current.pattern.children[child_idx].children[0].SetRegNo(orig_reg);
          clobber_reg_to_child_idx.erase(clob_it);
          if (reversed_reload_regs != nullptr) {
            reversed_reload_regs->insert(reload_reg);
          }
          ++lookahead;
        }
        i = lookahead - 1;
      }
    }
    out.push_back(std::move(current));
  }

  *insns = std::move(out);
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertReload(const RtlControlFlowGraph& reload_cfg) {
  if (reload_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertReload: input CFG has no basic blocks");
  }

  const size_t num_blocks = reload_cfg.rpo_blocks.size();
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = reload_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: block %zu has out-of-bounds successor %zu (num_blocks=%zu)", b, succ,
            num_blocks));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: block %zu has out-of-bounds addr_vec target %zu (num_blocks=%zu)", b,
            target, num_blocks));
      }
    }
  }

  for (const auto& [regno, hard_reg] : reload_cfg.sideband.reg_renumber) {
    if (regno < kFirstPseudoRegister || hard_reg < -1 || hard_reg >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertReload: invalid sideband reg_renumber entry (%d -> %d)", regno, hard_reg));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_refs) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_refs", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_sets) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_sets", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_deaths) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_deaths", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_live_length) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_live_length", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_calls_crossed) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertReload: negative regno %d in sideband reg_n_calls_crossed", regno));
    }
  }

  RtlControlFlowGraph out = reload_cfg;
  out.pass = RtlPassKind::kLreg;

  std::vector<std::vector<RtlPatternAndNotes>> all_parsed_blocks(out.rpo_blocks.size());
  absl::flat_hash_set<int> referenced_pseudos;
  absl::flat_hash_map<int, int> pseudo_max_bytes;

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    const RtlBasicBlock& blk = out.rpo_blocks[b];
    std::vector<RtlPatternAndNotes>& parsed_insns = all_parsed_blocks[b];
    parsed_insns.reserve(blk.instructions.size());

    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("InvertReload: failed to parse instruction %zu in block %zu: %s", i, b,
                            parsed.status().message()));
      }
      if (parsed->pattern.op == RtlOp::kNil || parsed->pattern.op == RtlOp::kUnknown) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: invalid pattern opcode at instruction %zu in block %zu", i, b));
      }
      if (absl::Status st = ValidateRtlExprRegisters(parsed->pattern); !st.ok()) {
        return st;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (absl::Status st = ValidateRtlExprRegisters(note.datum); !st.ok()) {
          return st;
        }
      }
      auto record_pseudo = [&](const RtlExpr& node) {
        if (node.op == RtlOp::kReg && node.RegNo() >= kFirstPseudoRegister) {
          referenced_pseudos.insert(node.RegNo());
          int& max_b = pseudo_max_bytes[node.RegNo()];
          max_b = std::max(max_b, ModeByteSize(node.mode));
        }
      };
      parsed->pattern.Visit(record_pseudo);
      for (const RtlRegNote& note : parsed->reg_notes) {
        note.datum.Visit(record_pseudo);
      }
      parsed_insns.push_back(*std::move(parsed));
    }
  }

  absl::flat_hash_set<int> caller_save_candidate_hard_regs;
  for (const auto& [regno, hard_reg] : out.sideband.reg_renumber) {
    if (regno >= kFirstPseudoRegister && hard_reg >= 0 && hard_reg < kFirstPseudoRegister) {
      auto calls_it = out.sideband.reg_n_calls_crossed.find(regno);
      if (calls_it != out.sideband.reg_n_calls_crossed.end() && calls_it->second > 0) {
        const int bytes = pseudo_max_bytes.contains(regno) ? pseudo_max_bytes[regno] : 4;
        const int nregs = std::max(1, (bytes + 3) / 4);
        for (int k = 0; k < nregs; ++k) {
          if (IsMipsCallerSavableHardReg(hard_reg + k)) {
            caller_save_candidate_hard_regs.insert(hard_reg + k);
          }
        }
      }
    }
  }

  bool restored_any_fp = false;
  absl::flat_hash_set<int> reversed_reload_regs;
  absl::flat_hash_set<int> remaining_hard_regs;
  absl::flat_hash_map<int, int64_t> stripped_caller_save_hr_offsets;

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    ReverseCallerSaveInsnsInBlock(
        caller_save_candidate_hard_regs, out.sideband.frame_pointer_needed,
        out.sideband.outgoing_args_size, &all_parsed_blocks[b], &stripped_caller_save_hr_offsets);
  }

  if (!stripped_caller_save_hr_offsets.empty()) {
    int min_hr = -1;
    int64_t min_off = 0;
    for (const auto& [hr, off] : stripped_caller_save_hr_offsets) {
      if (min_hr < 0 || off < min_off || (off == min_off && hr < min_hr)) {
        min_hr = hr;
        min_off = off;
      }
    }
    int unobserved_earlier_slots = 0;
    for (int cand_hr : caller_save_candidate_hard_regs) {
      if (cand_hr < min_hr && !stripped_caller_save_hr_offsets.contains(cand_hr)) {
        ++unobserved_earlier_slots;
      }
    }
    const int64_t pre_caller_save_top = min_off - 4 * int64_t{unobserved_earlier_slots};
    int64_t pre_reload_frame_top = pre_caller_save_top;

    // Partition unallocated pseudos (`reg_n_refs[p] > 0`, `reg_renumber[p] < 0`)
    // into active spilled pseudos (`reg_n_sets[p] > 0 || reg_n_deaths[p] > 0`)
    // and phantom pseudos (`reg_n_sets[p] == 0 && reg_n_deaths[p] == 0`, whose
    // instructions were eliminated by `local-alloc.c:optimize_reg_copy_*`
    // without zeroing `reg_n_refs[p]`). In `reload1.c:658`, `alter_reg(p, -1)`
    // allocates an 8-byte `BIGGEST_ALIGNMENT` stack slot for both categories,
    // while phantom pseudos have zero memory references in post-reload RTL.
    std::vector<int> active_spilled_pseudos;
    std::vector<int> phantom_spilled_pseudos;
    for (const auto& [p, nrefs] : out.sideband.reg_n_refs) {
      if (p >= kFirstPseudoRegister && nrefs > 0 && !out.sideband.reg_renumber.contains(p) &&
          !referenced_pseudos.contains(p)) {
        const auto sets_it = out.sideband.reg_n_sets.find(p);
        const int nsets = (sets_it != out.sideband.reg_n_sets.end()) ? sets_it->second : 0;
        const auto deaths_it = out.sideband.reg_n_deaths.find(p);
        const int ndeaths = (deaths_it != out.sideband.reg_n_deaths.end()) ? deaths_it->second : 0;
        if (nsets > 0 || ndeaths > 0) {
          active_spilled_pseudos.push_back(p);
        } else {
          phantom_spilled_pseudos.push_back(p);
        }
      }
    }
    std::sort(active_spilled_pseudos.begin(), active_spilled_pseudos.end());
    std::sort(phantom_spilled_pseudos.begin(), phantom_spilled_pseudos.end());

    if (!active_spilled_pseudos.empty() && pre_caller_save_top > out.sideband.outgoing_args_size) {
      absl::flat_hash_map<int64_t, RtlMachineMode> candidate_slots;
      auto collect_slots = [&](const RtlExpr& root) {
        root.Visit([&](const RtlExpr& node) {
          std::optional<std::pair<int64_t, RtlMachineMode>> m =
              MatchScalarSpillSlotMem(node, out.sideband.frame_pointer_needed,
                                      out.sideband.outgoing_args_size, pre_caller_save_top);
          if (m.has_value()) {
            candidate_slots.try_emplace(m->first, m->second);
          }
        });
      };
      for (const std::vector<RtlPatternAndNotes>& blk_insns : all_parsed_blocks) {
        for (const RtlPatternAndNotes& insn : blk_insns) {
          collect_slots(insn.pattern);
          for (const RtlRegNote& note : insn.reg_notes) {
            collect_slots(note.datum);
          }
        }
      }

      std::vector<std::pair<int64_t, RtlMachineMode>> sorted_slots(candidate_slots.begin(),
                                                                   candidate_slots.end());
      std::sort(sorted_slots.begin(), sorted_slots.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

      if (sorted_slots.size() >= active_spilled_pseudos.size()) {
        const size_t start_slot = sorted_slots.size() - active_spilled_pseudos.size();
        absl::flat_hash_map<int64_t, RtlExpr> slot_to_pseudo;
        for (size_t k = 0; k < active_spilled_pseudos.size(); ++k) {
          const auto [slot_off, slot_mode] = sorted_slots[start_slot + k];
          slot_to_pseudo.emplace(slot_off, RtlExpr::MakeReg(slot_mode, active_spilled_pseudos[k]));
        }
        const int64_t min_spill_off = sorted_slots[start_slot].first;
        for (std::vector<RtlPatternAndNotes>& blk_insns : all_parsed_blocks) {
          ReconstructSpilledPseudosInBlock(out.sideband.frame_pointer_needed, min_spill_off,
                                           pre_caller_save_top, slot_to_pseudo, &blk_insns);
        }
        int64_t phantoms_before_min_active = 0;
        for (int phantom_p : phantom_spilled_pseudos) {
          if (phantom_p < active_spilled_pseudos.front()) {
            ++phantoms_before_min_active;
          }
        }
        pre_reload_frame_top =
            std::max<int64_t>(out.sideband.outgoing_args_size,
                              (min_spill_off & ~int64_t{7}) - 8 * phantoms_before_min_active);
      }
    } else if (active_spilled_pseudos.empty() && !phantom_spilled_pseudos.empty() &&
               pre_caller_save_top > out.sideband.outgoing_args_size) {
      pre_reload_frame_top =
          std::max<int64_t>(out.sideband.outgoing_args_size,
                            (pre_caller_save_top & ~int64_t{7}) -
                                8 * static_cast<int64_t>(phantom_spilled_pseudos.size()));
    }

    out.sideband.frame_size_bytes = static_cast<int>(
        std::max<int64_t>(0, pre_reload_frame_top - out.sideband.outgoing_args_size));
  }

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    std::vector<RtlPatternAndNotes>& parsed_insns = all_parsed_blocks[b];

    for (RtlPatternAndNotes& insn : parsed_insns) {
      if (!out.sideband.frame_pointer_needed &&
          !ShouldSkipFramePointerRestorationForInsn(insn.pattern)) {
        if (ReverseEliminateRegsInExpr(&insn.pattern)) {
          restored_any_fp = true;
          // In `reload1.c:reload_as_needed` (lines 3836-3847), `(use (mem ...))`
          // and `(clobber (mem ...))` have `XEXP(MEM, 0)` eliminated in-place
          // before `eliminate_regs_in_insn(insn, 1)` is called, so `val == 0`
          // and `REG_NOTES(insn)` are not rewritten by `eliminate_regs_in_insn`.
          if (insn.pattern.op != RtlOp::kUse && insn.pattern.op != RtlOp::kClobber) {
            for (RtlRegNote& note : insn.reg_notes) {
              ReverseEliminateRegsInExpr(&note.datum);
            }
          }
        }
      }
    }

    ReverseParallelClobberOutputReloads(&parsed_insns, &reversed_reload_regs);

    blk.instructions.clear();
    blk.instructions.reserve(parsed_insns.size());
    for (const RtlPatternAndNotes& insn : parsed_insns) {
      insn.pattern.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kFirstPseudoRegister) {
          remaining_hard_regs.insert(node.RegNo());
        }
      });
      blk.instructions.push_back(insn.ToSexprString());
    }
    blk.SyncRtlInsnsFromInstructions();
  }

  if (!reversed_reload_regs.empty()) {
    for (const auto& [_, hard_reg] : out.sideband.reg_renumber) {
      if (hard_reg >= 0 && hard_reg < kFirstPseudoRegister) {
        remaining_hard_regs.insert(hard_reg);
      }
    }
    auto& saved_gprs = out.sideband.saved_gpr_regnos;
    saved_gprs.erase(std::remove_if(saved_gprs.begin(), saved_gprs.end(),
                                    [&](int gpr) {
                                      return reversed_reload_regs.contains(gpr) &&
                                             !remaining_hard_regs.contains(gpr);
                                    }),
                     saved_gprs.end());
  }

  if (restored_any_fp && !out.sideband.basic_block_live_at_start.empty()) {
    out.sideband.basic_block_live_at_start[0].insert(kFramePointerRegnum);
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
