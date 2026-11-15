#include "lifter/invert_fill_simple_delay_slots.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_multi_insn_idioms.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

bool IsValidRtlExprSubtree(const RtlExpr& root) {
  if (root.op == RtlOp::kReturn) {
    return false;
  }
  bool valid = true;
  root.Visit([&valid](const RtlExpr& node) {
    if (!valid) {
      return;
    }
    if (node.op == RtlOp::kUnknown) {
      valid = false;
      return;
    }
    for (const RtlExpr& child : node.children) {
      if (child.op == RtlOp::kReturn && node.op != RtlOp::kParallel) {
        valid = false;
        return;
      }
    }
    if (node.op == RtlOp::kSequence) {
      if (!node.bracketed_children || node.children.size() < 2) {
        valid = false;
        return;
      }
      for (const RtlExpr& child : node.children) {
        if ((child.op != RtlOp::kInsn && child.op != RtlOp::kJumpInsn &&
             child.op != RtlOp::kCallInsn) ||
            child.children.empty()) {
          valid = false;
          return;
        }
      }
    }
  });
  return valid;
}

bool IsValidRtlPatternSexpr(std::string_view sexpr) {
  auto parsed = ParsePatternAndNotes(sexpr);
  if (!parsed.ok()) {
    return false;
  }
  if (!IsValidRtlExprSubtree(parsed->pattern)) {
    return false;
  }
  for (const RtlRegNote& note : parsed->reg_notes) {
    if (!IsValidRtlExprSubtree(note.datum)) {
      return false;
    }
  }
  return true;
}

bool IsValidCfg(const RtlControlFlowGraph& cfg) {
  if (cfg.rpo_blocks.empty()) {
    return false;
  }
  const size_t num_blocks = cfg.rpo_blocks.size();
  bool has_any_instruction = false;
  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg.rpo_blocks[b];
    if (blk.instructions.empty() && (b + 1 < num_blocks || num_blocks == 1)) {
      return false;
    }
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return false;
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return false;
      }
    }
    for (const std::string& pat : blk.instructions) {
      if (!IsValidRtlPatternSexpr(pat)) {
        return false;
      }
      has_any_instruction = true;
    }
  }
  return has_any_instruction;
}

bool IsReturnExpr(const RtlExpr& expr) {
  return expr.op == RtlOp::kParallel && !expr.children.empty() &&
         expr.children[0].op == RtlOp::kReturn;
}

bool IsSimpleUncondJumpExpr(const RtlExpr& expr) {
  return expr.IsSet() && expr.SetDest().op == RtlOp::kPc && expr.SetSrc().op == RtlOp::kLabelRef;
}

bool IsTableJumpExpr(const RtlExpr& expr) {
  return expr.op == RtlOp::kParallel && expr.children.size() >= 2 && expr.children[0].IsSet() &&
         expr.children[0].SetDest().op == RtlOp::kPc && expr.children[0].SetSrc().IsReg() &&
         expr.children[1].op == RtlOp::kUse && !expr.children[1].children.empty() &&
         expr.children[1].children[0].op == RtlOp::kLabelRef;
}

const RtlExpr* GetCondJumpIfThenElse(const RtlExpr& expr) {
  const RtlExpr* set_expr = nullptr;
  if (expr.IsSet()) {
    set_expr = &expr;
  } else if (expr.op == RtlOp::kParallel && !expr.children.empty() && expr.children[0].IsSet()) {
    set_expr = &expr.children[0];
  }
  if (set_expr == nullptr || set_expr->SetDest().op != RtlOp::kPc) {
    return nullptr;
  }
  const RtlExpr& src = set_expr->SetSrc();
  if (src.op != RtlOp::kIfThenElse || src.children.size() != 3) {
    return nullptr;
  }
  return &src;
}

RtlExpr* GetMutableCondJumpIfThenElse(RtlExpr* expr) {
  RtlExpr* set_expr = nullptr;
  if (expr->IsSet()) {
    set_expr = expr;
  } else if (expr->op == RtlOp::kParallel && !expr->children.empty() && expr->children[0].IsSet()) {
    set_expr = &expr->children[0];
  }
  if (set_expr == nullptr || set_expr->SetDest().op != RtlOp::kPc) {
    return nullptr;
  }
  RtlExpr& src = set_expr->MutableSetSrc();
  if (src.op != RtlOp::kIfThenElse || src.children.size() != 3) {
    return nullptr;
  }
  return &src;
}

bool IsCondJumpExpr(const RtlExpr& expr) {
  const RtlExpr* ite = GetCondJumpIfThenElse(expr);
  if (ite == nullptr || !ite->children[0].IsComparison()) {
    return false;
  }
  const RtlExpr& arm1 = ite->children[1];
  const RtlExpr& arm2 = ite->children[2];
  return (arm1.op == RtlOp::kLabelRef && arm2.op == RtlOp::kPc) ||
         (arm1.op == RtlOp::kPc && arm2.op == RtlOp::kLabelRef);
}

RtlOp ReverseComparisonOp(RtlOp op) {
  switch (op) {
    case RtlOp::kEq:
      return RtlOp::kNe;
    case RtlOp::kNe:
      return RtlOp::kEq;
    case RtlOp::kLt:
      return RtlOp::kGe;
    case RtlOp::kGe:
      return RtlOp::kLt;
    case RtlOp::kLe:
      return RtlOp::kGt;
    case RtlOp::kGt:
      return RtlOp::kLe;
    case RtlOp::kLtu:
      return RtlOp::kGeu;
    case RtlOp::kGeu:
      return RtlOp::kLtu;
    case RtlOp::kLeu:
      return RtlOp::kGtu;
    case RtlOp::kGtu:
      return RtlOp::kLeu;
    default:
      return RtlOp::kUnknown;
  }
}

bool TryInvertCondJumpExpr(RtlExpr* jump_pat) {
  RtlExpr* ite = GetMutableCondJumpIfThenElse(jump_pat);
  if (ite == nullptr || !ite->children[0].IsComparison()) {
    return false;
  }
  const RtlOp rev = ReverseComparisonOp(ite->children[0].op);
  if (rev == RtlOp::kUnknown) {
    return false;
  }
  ite->children[0].op = rev;
  return true;
}

bool IsAnyJumpOrReturnTopExpr(const RtlExpr& expr) {
  if (expr.op == RtlOp::kSequence && !expr.children.empty()) {
    return expr.children[0].op == RtlOp::kJumpInsn;
  }
  return IsReturnExpr(expr) || IsSimpleUncondJumpExpr(expr) || IsCondJumpExpr(expr) ||
         IsTableJumpExpr(expr) || (expr.IsSet() && expr.SetDest().op == RtlOp::kPc);
}

bool IsUncondJumpOrReturnOrTableJumpTopExpr(const RtlExpr& expr) {
  if (expr.op == RtlOp::kSequence && !expr.children.empty()) {
    if (expr.children[0].op != RtlOp::kJumpInsn || expr.children[0].children.empty()) {
      return false;
    }
    const RtlExpr& inner = expr.children[0].children[0];
    return IsReturnExpr(inner) || IsSimpleUncondJumpExpr(inner) || IsTableJumpExpr(inner);
  }
  return IsReturnExpr(expr) || IsSimpleUncondJumpExpr(expr) || IsTableJumpExpr(expr);
}

void RewriteJumpTargetLabelRef(RtlExpr* expr, std::string_view new_label_tok) {
  expr->Mutate([new_label_tok](RtlExpr& node) {
    if (node.op == RtlOp::kLabelRef) {
      node = RtlExpr::MakeSymbolicLabelRef(std::string(new_label_tok), node.mode);
    }
  });
}

void SynchronizeCanonicalLabels(RtlControlFlowGraph* cfg) {
  const size_t num_blocks = cfg->rpo_blocks.size();
  if (num_blocks == 0) {
    return;
  }
  std::vector<bool> block_needs_label(num_blocks, false);
  std::vector<bool> block_has_tablejump(num_blocks, false);
  std::vector<std::string> old_tablejump_token(num_blocks);

  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg->rpo_blocks[b];
    if (blk.has_label) {
      block_needs_label[b] = true;
    }
    bool ends_with_uncond = false;
    if (!blk.instructions.empty()) {
      auto parsed = ParsePatternAndNotes(blk.instructions.back());
      if (parsed.ok()) {
        ends_with_uncond = IsUncondJumpOrReturnOrTableJumpTopExpr(parsed->pattern);
        const RtlExpr* tbl_expr = nullptr;
        if (IsTableJumpExpr(parsed->pattern)) {
          tbl_expr = &parsed->pattern;
        } else if (parsed->pattern.op == RtlOp::kSequence && !parsed->pattern.children.empty() &&
                   parsed->pattern.children[0].op == RtlOp::kJumpInsn &&
                   !parsed->pattern.children[0].children.empty() &&
                   IsTableJumpExpr(parsed->pattern.children[0].children[0])) {
          tbl_expr = &parsed->pattern.children[0].children[0];
        }
        if (tbl_expr != nullptr) {
          block_has_tablejump[b] = true;
          const RtlExpr& lbl_ref = tbl_expr->children[1].children[0];
          old_tablejump_token[b] = lbl_ref.str_val;
          for (size_t t : blk.addr_vec_targets) {
            if (t < num_blocks) {
              block_needs_label[t] = true;
            }
          }
        }
      }
    }
    if (blk.successors.size() >= 2 && blk.addr_vec_targets.empty()) {
      size_t target_b = blk.successors[0];
      if (target_b == b + 1) {
        target_b = blk.successors[1];
      }
      if (target_b < num_blocks) {
        block_needs_label[target_b] = true;
      }
    }
    for (size_t succ : blk.successors) {
      if (succ < num_blocks && (succ != b + 1 || ends_with_uncond)) {
        block_needs_label[succ] = true;
      }
    }
  }

  std::vector<std::string> block_label_token(num_blocks);
  absl::flat_hash_map<std::string, std::string> tablejump_token_remap;
  size_t next_lbl_idx = 0;
  for (size_t b = 0; b < num_blocks; ++b) {
    cfg->rpo_blocks[b].has_label = block_needs_label[b];
    if (block_needs_label[b]) {
      block_label_token[b] = absl::StrCat("L", next_lbl_idx++);
    }
    if (block_has_tablejump[b]) {
      std::string new_tbl_tok = absl::StrCat("L", next_lbl_idx++);
      if (!old_tablejump_token[b].empty()) {
        tablejump_token_remap[old_tablejump_token[b]] = new_tbl_tok;
      }
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    auto& blk = cfg->rpo_blocks[b];
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      bool modified = false;
      if (!tablejump_token_remap.empty()) {
        auto remap_fn = [&tablejump_token_remap, &modified](RtlExpr& node) {
          if (node.op == RtlOp::kLabelRef && !node.str_val.empty()) {
            auto it = tablejump_token_remap.find(node.str_val);
            if (it != tablejump_token_remap.end() && node.str_val != it->second) {
              node = RtlExpr::MakeSymbolicLabelRef(it->second, node.mode);
              modified = true;
            }
          }
        };
        parsed->pattern.Mutate(remap_fn);
        for (RtlRegNote& note : parsed->reg_notes) {
          note.datum.Mutate(remap_fn);
        }
      }
      if (i + 1 == blk.instructions.size() && !block_has_tablejump[b] && !blk.successors.empty() &&
          IsAnyJumpOrReturnTopExpr(parsed->pattern)) {
        if (blk.successors.size() == 2 && blk.successors[0] == b + 1 &&
            blk.successors[1] != b + 1) {
          std::swap(blk.successors[0], blk.successors[1]);
        }
        size_t target_b = blk.successors[0];
        if (blk.successors.size() >= 2 && blk.successors[0] == b + 1) {
          target_b = blk.successors[1];
        }
        if (target_b < num_blocks && !block_label_token[target_b].empty()) {
          const std::string& target_tok = block_label_token[target_b];
          if (parsed->pattern.op == RtlOp::kSequence && !parsed->pattern.children.empty() &&
              parsed->pattern.children[0].op == RtlOp::kJumpInsn) {
            auto sub_or = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[0]);
            auto slot_or = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[1]);
            if (sub_or.ok() && slot_or.ok()) {
              RewriteJumpTargetLabelRef(&sub_or->pattern, target_tok);
              blk.instructions[i] =
                  RtlExpr::MakeSequence(
                      {
                          RtlExpr::MakeInsnWrapper(parsed->pattern.children[0].op,
                                                   parsed->pattern.children[0].flags, *sub_or),
                          RtlExpr::MakeInsnWrapper(RtlOp::kInsn, parsed->pattern.children[1].flags,
                                                   *slot_or),
                      })
                      .ToGccPrintRtlString();
              continue;
            }
          } else {
            RewriteJumpTargetLabelRef(&parsed->pattern, target_tok);
            modified = true;
          }
        }
      }
      if (modified) {
        if (parsed->pattern.op == RtlOp::kSequence && parsed->pattern.children.size() == 2) {
          auto sub_or = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[0]);
          auto slot_or = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[1]);
          if (sub_or.ok() && slot_or.ok()) {
            blk.instructions[i] =
                RtlExpr::MakeSequence(
                    {
                        RtlExpr::MakeInsnWrapper(parsed->pattern.children[0].op,
                                                 parsed->pattern.children[0].flags, *sub_or),
                        RtlExpr::MakeInsnWrapper(RtlOp::kInsn, parsed->pattern.children[1].flags,
                                                 *slot_or),
                    })
                    .ToGccPrintRtlString();
            continue;
          }
        }
        blk.instructions[i] = parsed->ToGccPrintRtlString();
      }
    }
  }
}

void InsertBlockAt(RtlControlFlowGraph* cfg, size_t insert_idx, RtlBasicBlock new_blk) {
  cfg->rpo_blocks.insert(cfg->rpo_blocks.begin() + static_cast<ptrdiff_t>(insert_idx),
                         std::move(new_blk));
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    if (b == insert_idx) {
      continue;
    }
    for (size_t& succ : cfg->rpo_blocks[b].successors) {
      if (succ >= insert_idx) {
        ++succ;
      }
    }
    for (size_t& t : cfg->rpo_blocks[b].addr_vec_targets) {
      if (t >= insert_idx) {
        ++t;
      }
    }
  }
}

bool IsLoopHeaderNote(const RtlExpr& pat) {
  if (pat.op != RtlOp::kNote) {
    return false;
  }
  for (const auto& item : pat.generic_items) {
    if (item.text == "NOTE_INSN_LOOP_CONT" || item.text == "NOTE_INSN_LOOP_VTOP") {
      return true;
    }
  }
  return false;
}

void PrependInsnToBlockAfterLeadingLoopNotes(RtlBasicBlock* blk, std::string insn_str) {
  size_t insert_pos = 0;
  while (insert_pos < blk->instructions.size()) {
    auto parsed = ParsePatternAndNotes(blk->instructions[insert_pos]);
    if (parsed.ok() && IsLoopHeaderNote(parsed->pattern)) {
      ++insert_pos;
    } else {
      break;
    }
  }
  blk->instructions.insert(blk->instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                           std::move(insn_str));
}

bool ExprHasMem(const RtlExpr& expr) {
  bool has_mem = false;
  expr.Visit([&has_mem](const RtlExpr& node) {
    if (node.IsMem()) {
      has_mem = true;
    }
  });
  return has_mem;
}

void CollectDefinedRegs(const RtlExpr& expr, std::vector<int>* out) {
  expr.Visit([out](const RtlExpr& node) {
    if (node.IsSet() || node.op == RtlOp::kClobber) {
      const RtlExpr& dst = node.children[0];
      if (dst.IsReg()) {
        const int r = dst.RegNo();
        out->push_back(r);
        if (dst.mode == RtlMachineMode::kDi || dst.mode == RtlMachineMode::kDf) {
          out->push_back(r + 1);
        }
      } else if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg()) {
        out->push_back(dst.children[0].RegNo() + static_cast<int>(dst.int_val));
      }
    }
  });
}

bool ExpressionsConflict(const RtlExpr& a, const RtlExpr& b) {
  std::vector<int> a_defs;
  CollectDefinedRegs(a, &a_defs);
  for (int r : a_defs) {
    if (b.ContainsReg(r)) {
      return true;
    }
  }
  std::vector<int> b_defs;
  CollectDefinedRegs(b, &b_defs);
  for (int r : b_defs) {
    if (a.ContainsReg(r)) {
      return true;
    }
  }
  return false;
}

bool ExprHasMemReferencingReg(const RtlExpr& expr, int regno) {
  bool found = false;
  expr.Visit([&found, regno](const RtlExpr& node) {
    if (node.IsMem() && node.ContainsReg(regno)) {
      found = true;
    }
  });
  return found;
}

bool RegNotesConflictWithExpr(const std::vector<RtlRegNote>& notes, const RtlExpr& expr) {
  for (const RtlRegNote& note : notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg() &&
        expr.ContainsReg(note.datum.RegNo())) {
      return true;
    }
  }
  return false;
}

bool IsMemWriteExpr(const RtlExpr& expr) {
  if (expr.IsSet() && expr.SetDest().IsMem()) {
    return true;
  }
  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& child : expr.children) {
      if (child.IsSet() && child.SetDest().IsMem()) {
        return true;
      }
    }
  }
  return false;
}

bool IsMemLoadExpr(const RtlExpr& expr) {
  return expr.IsSet() && !expr.SetDest().IsMem() && ExprHasMem(expr.SetSrc());
}

// Verifies that `slot_pn` can be placed at `cand_pos` (`< orig_len`) in
// `new_insns` without any instruction in `[cand_pos .. orig_len - 1]` either
// conflicting with `slot_pn` or being eligible ahead of `slot_pn` during
// `reorg.c:fill_simple_delay_slots`'s backward scan from `head_expr`.
bool CanPlaceUnpackedSlotBeforeIntervening(const std::vector<std::string>& new_insns,
                                           size_t cand_pos, size_t orig_len,
                                           const RtlPatternAndNotes& slot_pn,
                                           const RtlExpr& head_expr) {
  if (cand_pos >= orig_len || orig_len > new_insns.size()) {
    return false;
  }
  std::vector<RtlPatternAndNotes> window;
  window.reserve(orig_len - cand_pos);
  for (size_t i = cand_pos; i < orig_len; ++i) {
    auto parsed = ParsePatternAndNotes(new_insns[i]);
    if (!parsed.ok()) {
      return false;
    }
    if (IsAnyJumpOrReturnTopExpr(parsed->pattern) || parsed->pattern.op == RtlOp::kCall) {
      return false;
    }
    bool has_call = false;
    parsed->pattern.Visit([&has_call](const RtlExpr& n) {
      if (n.op == RtlOp::kCall) {
        has_call = true;
      }
    });
    if (has_call) {
      return false;
    }
    if (ExpressionsConflict(parsed->pattern, slot_pn.pattern) ||
        RegNotesConflictWithExpr(slot_pn.reg_notes, parsed->pattern) ||
        RegNotesConflictWithExpr(parsed->reg_notes, slot_pn.pattern)) {
      return false;
    }
    window.push_back(std::move(*parsed));
  }

  for (size_t m = 0; m < window.size(); ++m) {
    const RtlExpr& m_pat = window[m].pattern;
    if (m_pat.op == RtlOp::kNote || m_pat.op == RtlOp::kUse || m_pat.op == RtlOp::kClobber) {
      continue;
    }
    // On MIPS (`mips.md:80`), memory loads (`type == load`) have `dslot = yes`
    // and `PARALLEL` instructions have `length > 1`, so neither is eligible for
    // a branch delay slot.
    if (IsMemLoadExpr(m_pat) || m_pat.op == RtlOp::kParallel) {
      continue;
    }
    if (ExpressionsConflict(m_pat, head_expr)) {
      continue;
    }
    const bool m_writes_mem = IsMemWriteExpr(m_pat);
    const bool m_has_mem = ExprHasMem(m_pat);
    bool blocked_by_later = false;
    for (size_t later = m + 1; later < window.size(); ++later) {
      const RtlExpr& l_pat = window[later].pattern;
      if (ExpressionsConflict(m_pat, l_pat) || (m_writes_mem && ExprHasMem(l_pat)) ||
          (m_has_mem && IsMemWriteExpr(l_pat))) {
        blocked_by_later = true;
        break;
      }
    }
    if (!blocked_by_later) {
      return false;
    }
  }
  return true;
}

bool IsCalleeSavedGprRegno(int r) {
  return (r >= 16 && r <= 23) || r == 30;
}

bool IsSpStackSaveOfReg(const RtlExpr& pat, int* out_reg) {
  if (!pat.IsSet() || !pat.SetDest().IsMem() || pat.SetDest().children.size() != 1 ||
      !pat.SetSrc().IsReg()) {
    return false;
  }
  const RtlExpr& addr = pat.SetDest().children[0];
  const bool on_sp =
      (addr.IsReg() && addr.RegNo() == 29) ||
      (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsReg() &&
       addr.children[0].RegNo() == 29 && addr.children[1].IsConstInt());
  if (!on_sp) {
    return false;
  }
  *out_reg = pat.SetSrc().RegNo();
  return *out_reg >= 0;
}

bool TargetFallthroughPredEndsWithSlotPat(const RtlControlFlowGraph& cfg, size_t jumper_b,
                                          size_t target_b, const RtlExpr& slot_pat) {
  if (target_b == 0 || target_b >= cfg.rpo_blocks.size() || target_b - 1 == jumper_b) {
    return false;
  }
  const auto& pred_blk = cfg.rpo_blocks[target_b - 1];
  bool has_ft_edge = false;
  for (size_t s : pred_blk.successors) {
    if (s == target_b) {
      has_ft_edge = true;
      break;
    }
  }
  if (!has_ft_edge || pred_blk.instructions.empty()) {
    return false;
  }
  for (size_t idx = pred_blk.instructions.size(); idx > 0; --idx) {
    auto p = ParsePatternAndNotes(pred_blk.instructions[idx - 1]);
    if (!p.ok()) {
      return false;
    }
    if (p->pattern.op == RtlOp::kNote || p->pattern.op == RtlOp::kUse ||
        p->pattern.op == RtlOp::kClobber) {
      continue;
    }
    if (IsUncondJumpOrReturnOrTableJumpTopExpr(p->pattern)) {
      return false;
    }
    return p->pattern == slot_pat;
  }
  return false;
}

void RecombineReorgSplitIdioms(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (flags.opt_level <= 0 || cfg->rpo_blocks.empty()) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);

  std::vector<std::vector<RtlPatternAndNotes>> parsed_blocks(cfg->rpo_blocks.size());
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    parsed_blocks[b].reserve(cfg->rpo_blocks[b].instructions.size());
    for (const std::string& pat_str : cfg->rpo_blocks[b].instructions) {
      auto parsed = ParsePatternAndNotes(pat_str);
      if (!parsed.ok()) {
        return;
      }
      parsed_blocks[b].push_back(std::move(*parsed));
    }
  }

  bool recombined_split_movstrsi = false;
  for (size_t b = 0; b < parsed_blocks.size(); ++b) {
    if (TryRecombineAdjacentSplitMovstrsiInBlock(&parsed_blocks[b],
                                                 /*mark_newly_folded_sentinel=*/true, gp64)) {
      recombined_split_movstrsi = true;
    }
  }

  bool folded_multi_insn = false;
  absl::flat_hash_set<int> compare_di_pairs;
  absl::flat_hash_set<int> existing_di_regs;
  bool has_di_witness = false;
  if (!gp64) {
    for (size_t b = 0; b < parsed_blocks.size(); ++b) {
      if (TryFoldGp32DiMultiInsnIdiomsInBlock(*cfg, b, /*mark_newly_folded_sentinel=*/true,
                                              &parsed_blocks[b])) {
        folded_multi_insn = true;
      }
    }
    CollectCompareDiamondDiPairs(parsed_blocks, &compare_di_pairs);
    bool has_any_di = false;
    CollectExistingDiEvenRegs(parsed_blocks, &existing_di_regs, &has_any_di);
    has_di_witness = has_any_di || !compare_di_pairs.empty();
  }
  const MipsMovstrsiFoldOptions movstrsi_opts{
      .gp64 = gp64,
      .fp64 = fp64,
      .has_di_witness = has_di_witness,
      .skip_unfilled_dbr_jump_tail = false,
      .allow_delay_slot_split_movstrsi = false,
      .mark_newly_folded_sentinel = true,
  };
  for (size_t b = 0; b < parsed_blocks.size(); ++b) {
    if (TryFoldMovstrsiBlockMovesInBlock(*cfg, b, movstrsi_opts, &parsed_blocks[b])) {
      folded_multi_insn = true;
    }
  }
  if (has_di_witness) {
    const int frame_size = DetectStackFrameSize(*cfg);
    const MipsWitnessedDiFoldOptions witnessed_opts{
        .allow_noncontiguous_reg_move = true,
        .mark_newly_folded_sentinel = true,
    };
    if (TryFoldGp32DiWitnessedIdioms(*cfg, frame_size, compare_di_pairs, existing_di_regs,
                                     witnessed_opts, &parsed_blocks)) {
      folded_multi_insn = true;
    }
  }

  if (!recombined_split_movstrsi && !folded_multi_insn) {
    return;
  }

  bool has_remaining_sequence = false;
  for (const auto& blk : parsed_blocks) {
    for (const auto& pn : blk) {
      if (pn.pattern.op == RtlOp::kSequence) {
        has_remaining_sequence = true;
        break;
      }
    }
  }

  std::vector<std::vector<bool>> is_newly_folded(cfg->rpo_blocks.size());
  std::vector<std::vector<std::string>> preserved_insns(cfg->rpo_blocks.size());
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    is_newly_folded[b].resize(parsed_blocks[b].size(), false);
    preserved_insns[b].reserve(parsed_blocks[b].size());
    cfg->rpo_blocks[b].instructions.clear();
    cfg->rpo_blocks[b].instructions.reserve(parsed_blocks[b].size());
    for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
      RtlPatternAndNotes& pn = parsed_blocks[b][i];
      if (!pn.reg_notes.empty() && pn.reg_notes.front().kind == RtlMachineMode::kVoid) {
        pn.reg_notes.clear();
        is_newly_folded[b][i] = true;
      }
      std::string rendered = pn.ToGccPrintRtlString();
      preserved_insns[b].push_back(rendered);
      cfg->rpo_blocks[b].instructions.push_back(std::move(rendered));
    }
    cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
  }

  if (has_remaining_sequence) {
    RecomputePostReloadLiveAtStart(cfg, profile);
    return;
  }

  const auto saved_max_regno = cfg->sideband.max_regno;
  const auto saved_n_refs = cfg->sideband.reg_n_refs;
  const auto saved_n_sets = cfg->sideband.reg_n_sets;
  const auto saved_n_deaths = cfg->sideband.reg_n_deaths;
  const auto saved_live_length = cfg->sideband.reg_live_length;
  const auto saved_calls_crossed = cfg->sideband.reg_n_calls_crossed;

  ReconstructRtlLiveness(cfg, profile, {.reload_completed = true, .update_reg_notes = true});

  if (!folded_multi_insn) {
    for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
      if (cfg->rpo_blocks[b].instructions.size() != preserved_insns[b].size()) {
        continue;
      }
      bool restored_any = false;
      for (size_t i = 0; i < preserved_insns[b].size(); ++i) {
        if (!is_newly_folded[b][i] && cfg->rpo_blocks[b].instructions[i] != preserved_insns[b][i]) {
          cfg->rpo_blocks[b].instructions[i] = std::move(preserved_insns[b][i]);
          restored_any = true;
        }
      }
      if (restored_any) {
        cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
      }
    }
  }

  cfg->sideband.max_regno = saved_max_regno;
  cfg->sideband.reg_n_refs = saved_n_refs;
  cfg->sideband.reg_n_sets = saved_n_sets;
  cfg->sideband.reg_n_deaths = saved_n_deaths;
  cfg->sideband.reg_live_length = saved_live_length;
  cfg->sideband.reg_n_calls_crossed = saved_calls_crossed;
}

bool HasConstZeroRegDeadNote(const std::vector<RtlRegNote>& notes) {
  for (const RtlRegNote& note : notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsConstInt() &&
        note.datum.int_val == 0) {
      return true;
    }
  }
  return false;
}

struct PreservedFallthroughZeroDead {
  size_t block_idx = 0;
  RtlExpr pattern;
  std::string insn_str;
};

std::vector<PreservedFallthroughZeroDead> CollectFallthroughRelaxedZeroDeadInsns(
    const RtlControlFlowGraph& cfg) {
  std::vector<PreservedFallthroughZeroDead> saved;
  for (size_t b = 0; b < cfg.rpo_blocks.size(); ++b) {
    const RtlBasicBlock& blk = cfg.rpo_blocks[b];
    if (blk.successors.empty()) {
      continue;
    }
    size_t real_insn_count = 0;
    std::optional<size_t> last_real_idx;
    RtlPatternAndNotes last_real_pn;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        real_insn_count = 0;
        break;
      }
      if (parsed->pattern.op == RtlOp::kNote || parsed->pattern.op == RtlOp::kUse ||
          parsed->pattern.op == RtlOp::kClobber) {
        continue;
      }
      ++real_insn_count;
      last_real_idx = i;
      last_real_pn = std::move(*parsed);
    }
    if (real_insn_count >= 2 && last_real_idx.has_value() &&
        !IsAnyJumpOrReturnTopExpr(last_real_pn.pattern) &&
        HasConstZeroRegDeadNote(last_real_pn.reg_notes)) {
      saved.push_back(PreservedFallthroughZeroDead{
          .block_idx = b,
          .pattern = std::move(last_real_pn.pattern),
          .insn_str = blk.instructions[*last_real_idx],
      });
    }
  }
  return saved;
}

void RestoreFallthroughRelaxedZeroDeadInsns(
    RtlControlFlowGraph* cfg, const std::vector<PreservedFallthroughZeroDead>& saved) {
  for (const PreservedFallthroughZeroDead& entry : saved) {
    if (entry.block_idx >= cfg->rpo_blocks.size()) {
      continue;
    }
    RtlBasicBlock& blk = cfg->rpo_blocks[entry.block_idx];
    std::optional<size_t> last_real_idx;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        last_real_idx.reset();
        break;
      }
      if (parsed->pattern.op == RtlOp::kNote || parsed->pattern.op == RtlOp::kUse ||
          parsed->pattern.op == RtlOp::kClobber) {
        continue;
      }
      last_real_idx = i;
    }
    if (!last_real_idx.has_value()) {
      continue;
    }
    auto cur_parsed = ParsePatternAndNotes(blk.instructions[*last_real_idx]);
    if (cur_parsed.ok() && cur_parsed->pattern == entry.pattern &&
        blk.instructions[*last_real_idx] != entry.insn_str) {
      blk.instructions[*last_real_idx] = entry.insn_str;
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

}  // namespace

RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x,
                                               const CompilationProfile& profile) {
  if (!IsValidCfg(dbr_simple_cfg_x)) {
    return {};
  }

  RtlControlFlowGraph out = dbr_simple_cfg_x;
  out.pass = RtlPassKind::kJump2;

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    std::vector<std::string> new_insns;
    new_insns.reserve(out.rpo_blocks[b].instructions.size() + 2);
    bool inserted_optimize_skip_block = false;

    for (size_t i = 0; i < out.rpo_blocks[b].instructions.size(); ++i) {
      const std::string& insn_str = out.rpo_blocks[b].instructions[i];
      auto parsed = ParsePatternAndNotes(insn_str);
      if (!parsed.ok() || parsed->pattern.op != RtlOp::kSequence ||
          parsed->pattern.children.size() < 2) {
        new_insns.push_back(insn_str);
        continue;
      }

      const RtlExpr& head_sub = parsed->pattern.children[0];
      const RtlExpr& slot_sub = parsed->pattern.children[1];
      auto head_pat = RtlPatternAndNotes::FromSequenceSubInsn(head_sub);
      auto slot_pat = RtlPatternAndNotes::FromSequenceSubInsn(slot_sub);
      if (!head_pat.ok() || !slot_pat.ok()) {
        new_insns.push_back(insn_str);
        continue;
      }

      // Check for `reorg.c:optimize_skip` (`jump_insn/u` + `insn/s` on a conditional branch).
      const bool is_last_insn = (i + 1 == out.rpo_blocks[b].instructions.size());
      if (is_last_insn && head_sub.op == RtlOp::kJumpInsn && head_sub.flags.unchanging &&
          slot_sub.flags.in_struct && IsCondJumpExpr(head_pat->pattern) &&
          out.rpo_blocks[b].successors.size() == 2) {
        RtlPatternAndNotes uninverted_jump = *head_pat;
        if (TryInvertCondJumpExpr(&uninverted_jump.pattern)) {
          new_insns.push_back(uninverted_jump.ToGccPrintRtlString());
          if (out.rpo_blocks[b].successors[0] == out.rpo_blocks[b].successors[1] &&
              b + 1 < out.rpo_blocks.size()) {
            out.rpo_blocks[b].instructions = std::move(new_insns);
            RtlBasicBlock ft_blk;
            ft_blk.id = static_cast<uint32_t>(b + 1);
            ft_blk.has_label = false;
            ft_blk.instructions = {slot_pat->ToGccPrintRtlString()};
            ft_blk.successors = {b + 2};
            InsertBlockAt(&out, b + 1, std::move(ft_blk));
            out.rpo_blocks[b + 2].has_label = true;
            out.rpo_blocks[b].successors = {b + 2, b + 1};
            inserted_optimize_skip_block = true;
            break;
          }
          if (out.rpo_blocks[b].successors[0] != out.rpo_blocks[b].successors[1] &&
              b + 1 < out.rpo_blocks.size()) {
            PrependInsnToBlockAfterLeadingLoopNotes(&out.rpo_blocks[b + 1],
                                                    slot_pat->ToGccPrintRtlString());
            continue;
          }
        }
      }

      // Standard simple delay slot (`JUMP_INSN` or `CALL_INSN`): unpack `slot_pat`
      // immediately before `head_pat` (and before any immediately preceding
      // `(use ...)` or `(clobber ...)` pseudo-instructions in `new_insns` that
      // `fill_simple_delay_slots` scanned backwards past).
      size_t slot_insert_pos = new_insns.size();
      while (slot_insert_pos > 0) {
        auto prev_parsed = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
        if (prev_parsed.ok() && (prev_parsed->pattern.op == RtlOp::kUse ||
                                 prev_parsed->pattern.op == RtlOp::kClobber)) {
          --slot_insert_pos;
        } else {
          break;
        }
      }
      if (is_last_insn && head_sub.op == RtlOp::kJumpInsn &&
          IsSimpleUncondJumpExpr(head_pat->pattern) && out.rpo_blocks[b].successors.size() == 1 &&
          slot_insert_pos > 0 && !ExprHasMem(slot_pat->pattern) &&
          TargetFallthroughPredEndsWithSlotPat(out, b, out.rpo_blocks[b].successors[0],
                                               slot_pat->pattern)) {
        auto prev_parsed = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
        if (prev_parsed.ok() && prev_parsed->pattern.IsSet() &&
            prev_parsed->pattern.SetDest().IsReg() && ExprHasMem(prev_parsed->pattern.SetSrc()) &&
            !ExpressionsConflict(prev_parsed->pattern, slot_pat->pattern)) {
          --slot_insert_pos;
        }
      }
      if (is_last_insn && head_sub.op == RtlOp::kJumpInsn && IsCondJumpExpr(head_pat->pattern) &&
          slot_insert_pos > 0 && !ExprHasMem(slot_pat->pattern) &&
          !ExpressionsConflict(head_pat->pattern, slot_pat->pattern)) {
        const auto& succs = out.rpo_blocks[b].successors;
        const bool is_self_loop_latch = std::find(succs.begin(), succs.end(), b) != succs.end();
        bool placed_earlier = false;

        // Rule 1: In a self-loop latch block, place a pointer post-increment
        // `(set (reg:SI P) (plus:SI (reg:SI P) (const_int step)))` immediately
        // after the last memory access referencing `P` when every intervening
        // instruction is skipped by `reorg.c:fill_simple_delay_slots`.
        if (is_self_loop_latch && slot_pat->pattern.IsSet() &&
            slot_pat->pattern.SetDest().IsReg() &&
            slot_pat->pattern.SetDest().mode == RtlMachineMode::kSi &&
            slot_pat->pattern.SetSrc().op == RtlOp::kPlus &&
            slot_pat->pattern.SetSrc().children.size() == 2 &&
            slot_pat->pattern.SetSrc().children[0].IsReg() &&
            slot_pat->pattern.SetSrc().children[1].IsConstInt()) {
          const int ptr_reg = slot_pat->pattern.SetDest().RegNo();
          if (ptr_reg == slot_pat->pattern.SetSrc().children[0].RegNo() && ptr_reg >= 1 &&
              ptr_reg < 32 && ptr_reg != 29 && ptr_reg != 30) {
            std::optional<size_t> last_mem_idx;
            for (size_t k = 0; k < slot_insert_pos; ++k) {
              auto pk = ParsePatternAndNotes(new_insns[k]);
              if (pk.ok() && ExprHasMemReferencingReg(pk->pattern, ptr_reg)) {
                last_mem_idx = k;
              }
            }
            if (last_mem_idx.has_value() && *last_mem_idx + 1 < slot_insert_pos &&
                CanPlaceUnpackedSlotBeforeIntervening(new_insns, *last_mem_idx + 1, slot_insert_pos,
                                                      *slot_pat, head_pat->pattern)) {
              slot_insert_pos = *last_mem_idx + 1;
              placed_earlier = true;
            }
          }
        }

        // Rule 2: In function entry block `B0`, place a prologue/incoming-arg
        // register copy `(set (reg:SI dst) (reg:SI src))` immediately after
        // `sw dst` (when `sw dst` was scheduled ahead of `sw $31`) or after an
        // incoming-arg save `(set (reg:SI s_reg) (reg:SI dst))` preceding
        // `sw $31` when every intervening instruction is skipped by
        // `reorg.c:fill_simple_delay_slots`.
        if (!placed_earlier && b == 0 && !is_self_loop_latch && slot_pat->pattern.IsSet() &&
            slot_pat->pattern.SetDest().IsReg() && slot_pat->pattern.SetSrc().IsReg() &&
            slot_pat->pattern.SetDest().mode == RtlMachineMode::kSi &&
            slot_pat->pattern.SetSrc().mode == RtlMachineMode::kSi) {
          const int dst_reg = slot_pat->pattern.SetDest().RegNo();
          const int src_reg = slot_pat->pattern.SetSrc().RegNo();
          if (dst_reg >= 1 && dst_reg < 32 && src_reg >= 1 && src_reg < 32 && dst_reg != src_reg &&
              dst_reg != 29 && src_reg != 29) {
            std::optional<size_t> ra_save_pos;
            std::optional<size_t> cand_pos;
            for (size_t k = 0; k < slot_insert_pos; ++k) {
              auto pk = ParsePatternAndNotes(new_insns[k]);
              if (!pk.ok()) {
                break;
              }
              int saved_r = -1;
              if (IsSpStackSaveOfReg(pk->pattern, &saved_r) && saved_r == 31 &&
                  !ra_save_pos.has_value()) {
                ra_save_pos = k;
              }
            }
            if (ra_save_pos.has_value()) {
              if (IsCalleeSavedGprRegno(dst_reg)) {
                for (size_t k = 0; k < *ra_save_pos; ++k) {
                  auto pk = ParsePatternAndNotes(new_insns[k]);
                  int saved_r = -1;
                  if (pk.ok() && IsSpStackSaveOfReg(pk->pattern, &saved_r) && saved_r == dst_reg) {
                    cand_pos = k + 1;
                    break;
                  }
                }
              } else if (dst_reg >= 4 && dst_reg <= 7 && src_reg >= 4 && src_reg <= 7) {
                for (size_t k = 0; k < *ra_save_pos; ++k) {
                  auto pk = ParsePatternAndNotes(new_insns[k]);
                  if (pk.ok() && pk->pattern.IsSet() && pk->pattern.SetDest().IsReg() &&
                      pk->pattern.SetSrc().IsReg() &&
                      IsCalleeSavedGprRegno(pk->pattern.SetDest().RegNo()) &&
                      pk->pattern.SetSrc().RegNo() == dst_reg) {
                    cand_pos = k + 1;
                    break;
                  }
                }
              }
            }
            if (cand_pos.has_value() && *cand_pos < slot_insert_pos &&
                CanPlaceUnpackedSlotBeforeIntervening(new_insns, *cand_pos, slot_insert_pos,
                                                      *slot_pat, head_pat->pattern)) {
              slot_insert_pos = *cand_pos;
              placed_earlier = true;
            }
          }
        }

        if (!placed_earlier) {
          const RtlExpr* ite = GetCondJumpIfThenElse(head_pat->pattern);
          auto prev_parsed = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
          if (ite != nullptr && prev_parsed.ok() && prev_parsed->pattern.IsSet() &&
              prev_parsed->pattern.SetDest().IsReg() &&
              prev_parsed->pattern.SetSrc().IsComparison() &&
              ite->children[0].ContainsReg(prev_parsed->pattern.SetDest().RegNo()) &&
              !ExprHasMem(prev_parsed->pattern) &&
              !ExpressionsConflict(prev_parsed->pattern, slot_pat->pattern) &&
              !RegNotesConflictWithExpr(slot_pat->reg_notes, prev_parsed->pattern) &&
              !RegNotesConflictWithExpr(prev_parsed->reg_notes, slot_pat->pattern)) {
            --slot_insert_pos;
          }
        }
      }
      new_insns.insert(new_insns.begin() + static_cast<ptrdiff_t>(slot_insert_pos),
                       slot_pat->ToGccPrintRtlString());
      new_insns.push_back(head_pat->ToGccPrintRtlString());
    }

    if (inserted_optimize_skip_block) {
      ++b;
    } else {
      out.rpo_blocks[b].instructions = std::move(new_insns);
    }
  }

  if (out.sideband.epilogue_delay_pattern.has_value() &&
      !out.sideband.epilogue_delay_pattern->empty() && !out.rpo_blocks.empty() &&
      out.rpo_blocks.back().instructions.empty()) {
    out.rpo_blocks.back().instructions.push_back(*out.sideband.epilogue_delay_pattern);
    out.sideband.epilogue_delay_pattern.reset();
  }

  SynchronizeCanonicalLabels(&out);

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    out.rpo_blocks[b].id = static_cast<uint32_t>(b);
    out.rpo_blocks[b].SyncRtlInsnsFromInstructions();
  }
  const std::vector<PreservedFallthroughZeroDead> preserved_ft_zero_dead =
      CollectFallthroughRelaxedZeroDeadInsns(out);
  RecomputePostReloadLiveAtStart(&out, profile);
  RecombineReorgSplitIdioms(&out, profile);
  ResolvePostReloadConstZeroRegDeadNotes(&out, profile);
  RestoreFallthroughRelaxedZeroDeadInsns(&out, preserved_ft_zero_dead);

  return out;
}

RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x) {
  const CompilationProfile default_profile{.optimize_level = 2};
  return InvertFillSimpleDelaySlots(dbr_simple_cfg_x, default_profile);
}

}  // namespace rom_nom_nom::lifter
