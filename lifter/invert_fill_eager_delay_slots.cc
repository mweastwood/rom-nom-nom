#include "lifter/invert_fill_eager_delay_slots.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

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

void CollectDefinedRegs(const RtlExpr& expr, std::vector<int>* out) {
  expr.Visit([out](const RtlExpr& node) {
    if (node.IsSet() || node.op == RtlOp::kClobber) {
      const RtlExpr& dst = node.children[0];
      if (dst.IsReg()) {
        out->push_back(dst.RegNo());
      } else if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg()) {
        out->push_back(dst.children[0].RegNo());
      }
    }
  });
}

bool ExprReferencesRegInSrcOrMem(const RtlExpr& expr, int regno) {
  if (expr.op == RtlOp::kClobber) {
    return !expr.children.empty() && expr.children[0].IsMem() &&
           expr.children[0].ContainsReg(regno);
  }
  if (expr.IsSet()) {
    if (expr.SetSrc().ContainsReg(regno)) {
      return true;
    }
    if (expr.SetDest().IsMem() && expr.SetDest().ContainsReg(regno)) {
      return true;
    }
    return false;
  }
  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& child : expr.children) {
      if (ExprReferencesRegInSrcOrMem(child, regno)) {
        return true;
      }
    }
    return false;
  }
  return expr.ContainsReg(regno);
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

std::vector<std::vector<size_t>> BuildPredecessors(const RtlControlFlowGraph& cfg) {
  const size_t n = cfg.rpo_blocks.size();
  std::vector<std::vector<size_t>> preds(n);
  for (size_t b = 0; b < n; ++b) {
    for (size_t succ : cfg.rpo_blocks[b].successors) {
      if (succ < n) {
        preds[succ].push_back(b);
      }
    }
    for (size_t t : cfg.rpo_blocks[b].addr_vec_targets) {
      if (t < n && std::find(preds[t].begin(), preds[t].end(), b) == preds[t].end()) {
        preds[t].push_back(b);
      }
    }
  }
  return preds;
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

void EraseBlockAt(RtlControlFlowGraph* cfg, size_t erase_idx) {
  cfg->rpo_blocks.erase(cfg->rpo_blocks.begin() + static_cast<ptrdiff_t>(erase_idx));
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    for (size_t& succ : cfg->rpo_blocks[b].successors) {
      if (succ > erase_idx) {
        --succ;
      }
    }
    for (size_t& t : cfg->rpo_blocks[b].addr_vec_targets) {
      if (t > erase_idx) {
        --t;
      }
    }
  }
}

bool IsRealActiveInsn(const RtlExpr& pat) {
  return pat.op != RtlOp::kNote && pat.op != RtlOp::kUse && pat.op != RtlOp::kClobber;
}

bool BlockFallsThroughToNext(const RtlControlFlowGraph& cfg,
                             const std::vector<std::vector<size_t>>& preds, size_t b) {
  if (b + 1 >= cfg.rpo_blocks.size()) {
    return false;
  }
  const auto& blk = cfg.rpo_blocks[b];
  if (std::find(blk.successors.begin(), blk.successors.end(), b + 1) == blk.successors.end()) {
    return false;
  }
  if (blk.instructions.empty()) {
    return true;
  }
  bool has_real_active = false;
  for (const std::string& insn_str : blk.instructions) {
    auto p = ParsePatternAndNotes(insn_str);
    if (p.ok() && IsRealActiveInsn(p->pattern)) {
      has_real_active = true;
      break;
    }
  }
  if (!has_real_active && !blk.has_label && b < preds.size() && preds[b].empty()) {
    return false;
  }
  auto parsed = ParsePatternAndNotes(blk.instructions.back());
  if (!parsed.ok()) {
    return false;
  }
  return !IsUncondJumpOrReturnOrTableJumpTopExpr(parsed->pattern);
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
  return pat.str_val == "NOTE_INSN_LOOP_CONT" || pat.str_val == "NOTE_INSN_LOOP_VTOP";
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

bool IsAnyRegLiveAtBlockEntry(const RtlControlFlowGraph& out, absl::Span<const int> regs,
                              size_t start_b) {
  for (int r : regs) {
    size_t cur_b = start_b;
    for (int depth = 0; depth < 4 && cur_b < out.rpo_blocks.size(); ++depth) {
      const auto& scan_blk = out.rpo_blocks[cur_b];
      bool killed = false;
      bool stop_chain = false;
      for (size_t idx = 0; idx < scan_blk.instructions.size(); ++idx) {
        const std::string& insn_s = scan_blk.instructions[idx];
        auto p = ParsePatternAndNotes(insn_s);
        if (!p.ok() || IsLoopHeaderNote(p->pattern) || p->pattern.op == RtlOp::kClobber) {
          continue;
        }
        if (p->pattern.op == RtlOp::kUse) {
          if (r != 31 && ExprReferencesRegInSrcOrMem(p->pattern, r)) {
            bool killed_before_exit = false;
            for (size_t j = idx + 1; j < scan_blk.instructions.size(); ++j) {
              auto q = ParsePatternAndNotes(scan_blk.instructions[j]);
              if (!q.ok() || IsLoopHeaderNote(q->pattern) || q->pattern.op == RtlOp::kClobber ||
                  q->pattern.op == RtlOp::kUse) {
                continue;
              }
              if (q->pattern.op == RtlOp::kSequence && q->pattern.children.size() == 2 &&
                  !q->pattern.children[0].flags.unchanging) {
                auto q0 = RtlPatternAndNotes::FromSequenceSubInsn(q->pattern.children[0]);
                auto q1 = RtlPatternAndNotes::FromSequenceSubInsn(q->pattern.children[1]);
                if (q0.ok() && q1.ok() && !ExprReferencesRegInSrcOrMem(q0->pattern, r) &&
                    !ExprReferencesRegInSrcOrMem(q1->pattern, r)) {
                  std::vector<int> q1_defs;
                  CollectDefinedRegs(q1->pattern, &q1_defs);
                  if (std::find(q1_defs.begin(), q1_defs.end(), r) != q1_defs.end()) {
                    killed_before_exit = true;
                  }
                }
              }
              break;
            }
            if (killed_before_exit) {
              killed = true;
              break;
            }
            return true;
          }
          continue;
        }
        if (p->pattern.op == RtlOp::kSequence && p->pattern.children.size() == 2) {
          auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(p->pattern.children[0]);
          auto sub1 = RtlPatternAndNotes::FromSequenceSubInsn(p->pattern.children[1]);
          if (sub0.ok() && ExprReferencesRegInSrcOrMem(sub0->pattern, r)) {
            return true;
          }
          if (sub1.ok() && ExprReferencesRegInSrcOrMem(sub1->pattern, r)) {
            return true;
          }
          stop_chain = true;
          break;
        }
        if (ExprReferencesRegInSrcOrMem(p->pattern, r)) {
          return true;
        }
        std::vector<int> insn_defs;
        CollectDefinedRegs(p->pattern, &insn_defs);
        if (std::find(insn_defs.begin(), insn_defs.end(), r) != insn_defs.end()) {
          killed = true;
          break;
        }
        bool has_call = false;
        p->pattern.Visit([&](const RtlExpr& e) {
          if (e.op == RtlOp::kCall) {
            has_call = true;
          }
        });
        if (has_call || IsAnyJumpOrReturnTopExpr(p->pattern)) {
          stop_chain = true;
          break;
        }
      }
      if (killed || stop_chain || scan_blk.successors.size() != 1) {
        break;
      }
      cur_b = scan_blk.successors[0];
    }
  }
  return false;
}

bool IsSlotKilledOnFallthrough(const RtlControlFlowGraph& out, const RtlExpr& slot_pat,
                               size_t start_ft_b) {
  if (!slot_pat.IsSet() || !slot_pat.SetDest().IsReg()) {
    return false;
  }
  bool has_volatile = false;
  slot_pat.Visit([&](const RtlExpr& e) {
    if (e.flags.volatil || e.op == RtlOp::kAsmInput || e.op == RtlOp::kUnspecVolatile) {
      has_volatile = true;
    }
  });
  if (has_volatile) {
    return false;
  }
  std::vector<int> remaining_defs;
  CollectDefinedRegs(slot_pat, &remaining_defs);
  if (remaining_defs.empty()) {
    return false;
  }
  auto is_call_clobbered_reg = [](int r) -> bool {
    return r == 1 || (r >= 2 && r <= 15) || r == 24 || r == 25 || r == 31 || (r >= 32 && r <= 51);
  };
  auto call_reads_reg = [](const RtlExpr& call_pat, int r) -> bool {
    return (r >= 4 && r <= 7) || (r >= 44 && r <= 47) || ExprReferencesRegInSrcOrMem(call_pat, r);
  };
  size_t cur_b = start_ft_b;
  for (int depth = 0; depth < 3 && cur_b < out.rpo_blocks.size(); ++depth) {
    const auto& scan_blk = out.rpo_blocks[cur_b];
    bool stop_chain = false;
    for (const std::string& ft_insn : scan_blk.instructions) {
      auto fp = ParsePatternAndNotes(ft_insn);
      if (!fp.ok() || IsLoopHeaderNote(fp->pattern) || fp->pattern.op == RtlOp::kClobber) {
        continue;
      }
      if (fp->pattern.op == RtlOp::kUse) {
        for (int r : remaining_defs) {
          if (r != 31 && ExprReferencesRegInSrcOrMem(fp->pattern, r)) {
            return false;
          }
        }
        continue;
      }
      if (fp->pattern.op == RtlOp::kSequence && fp->pattern.children.size() == 2) {
        const RtlExpr& s0 = fp->pattern.children[0];
        const RtlExpr& s1 = fp->pattern.children[1];
        auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(s0);
        auto sub1 = RtlPatternAndNotes::FromSequenceSubInsn(s1);
        if (!sub0.ok() || !sub1.ok()) {
          return false;
        }
        std::vector<int> sub1_defs;
        CollectDefinedRegs(sub1->pattern, &sub1_defs);
        if (s0.op == RtlOp::kCallInsn) {
          for (int r : remaining_defs) {
            if (ExprReferencesRegInSrcOrMem(sub1->pattern, r)) {
              return false;
            }
            const bool killed_by_delay =
                std::find(sub1_defs.begin(), sub1_defs.end(), r) != sub1_defs.end();
            if (!killed_by_delay && call_reads_reg(sub0->pattern, r)) {
              return false;
            }
          }
          remaining_defs.erase(std::remove_if(remaining_defs.begin(), remaining_defs.end(),
                                              [&](int r) {
                                                return is_call_clobbered_reg(r) ||
                                                       std::find(sub1_defs.begin(), sub1_defs.end(),
                                                                 r) != sub1_defs.end();
                                              }),
                               remaining_defs.end());
          if (remaining_defs.empty()) {
            return true;
          }
          continue;
        }
        for (int r : remaining_defs) {
          if (ExprReferencesRegInSrcOrMem(sub0->pattern, r)) {
            return false;
          }
        }
        if (!s0.flags.unchanging) {
          for (int r : remaining_defs) {
            if (ExprReferencesRegInSrcOrMem(sub1->pattern, r)) {
              return false;
            }
          }
          remaining_defs.erase(std::remove_if(remaining_defs.begin(), remaining_defs.end(),
                                              [&](int r) {
                                                return std::find(sub1_defs.begin(), sub1_defs.end(),
                                                                 r) != sub1_defs.end();
                                              }),
                               remaining_defs.end());
          if (remaining_defs.empty()) {
            return true;
          }
        }
        if (IsSimpleUncondJumpExpr(sub0->pattern) && !s0.flags.unchanging) {
          break;
        }
        return false;
      }
      if (IsSimpleUncondJumpExpr(fp->pattern)) {
        break;
      }
      bool has_call = false;
      fp->pattern.Visit([&](const RtlExpr& e) {
        if (e.op == RtlOp::kCall) {
          has_call = true;
        }
      });
      if (has_call) {
        for (int r : remaining_defs) {
          if (call_reads_reg(fp->pattern, r)) {
            return false;
          }
        }
        remaining_defs.erase(
            std::remove_if(remaining_defs.begin(), remaining_defs.end(), is_call_clobbered_reg),
            remaining_defs.end());
        if (remaining_defs.empty()) {
          return true;
        }
        continue;
      }
      if (!IsRealActiveInsn(fp->pattern) || IsAnyJumpOrReturnTopExpr(fp->pattern)) {
        stop_chain = true;
        break;
      }
      for (int r : remaining_defs) {
        if (ExprReferencesRegInSrcOrMem(fp->pattern, r)) {
          return false;
        }
      }
      std::vector<int> ft_defs;
      CollectDefinedRegs(fp->pattern, &ft_defs);
      remaining_defs.erase(
          std::remove_if(
              remaining_defs.begin(), remaining_defs.end(),
              [&](int r) { return std::find(ft_defs.begin(), ft_defs.end(), r) != ft_defs.end(); }),
          remaining_defs.end());
      if (remaining_defs.empty()) {
        return true;
      }
    }
    if (stop_chain || scan_blk.successors.size() != 1) {
      break;
    }
    cur_b = scan_blk.successors[0];
  }
  return false;
}

bool ExprHasMemOrVolatile(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&](const RtlExpr& e) {
    if (e.op == RtlOp::kMem || e.flags.volatil || e.op == RtlOp::kAsmInput ||
        e.op == RtlOp::kUnspecVolatile) {
      found = true;
    }
  });
  return found;
}

bool BlockHasZeroRealActiveInsns(const RtlBasicBlock& blk);

bool IsUncondTargetStealAfterCond(const RtlControlFlowGraph& out, size_t ub,
                                  const std::vector<std::vector<size_t>>& cur_preds,
                                  const std::vector<bool>& entered_with_filled_end_jump) {
  if (ub == 0 || ub >= out.rpo_blocks.size()) {
    return false;
  }
  const auto& ublk = out.rpo_blocks[ub];
  if (ublk.has_label || ublk.instructions.size() != 1 || ublk.successors.size() != 1 ||
      cur_preds[ub].size() != 1 || cur_preds[ub][0] != ub - 1) {
    return false;
  }
  auto uparsed = ParsePatternAndNotes(ublk.instructions[0]);
  if (!uparsed.ok() || uparsed->pattern.op != RtlOp::kSequence ||
      uparsed->pattern.children.size() != 2 ||
      uparsed->pattern.children[0].op != RtlOp::kJumpInsn ||
      uparsed->pattern.children[1].op != RtlOp::kInsn ||
      uparsed->pattern.children[1].flags.in_struct) {
    return false;
  }
  auto ujump = RtlPatternAndNotes::FromSequenceSubInsn(uparsed->pattern.children[0]);
  auto uslot = RtlPatternAndNotes::FromSequenceSubInsn(uparsed->pattern.children[1]);
  if (!ujump.ok() || !uslot.ok() || !IsSimpleUncondJumpExpr(ujump->pattern) ||
      !uslot->pattern.IsSet() || !uslot->pattern.SetDest().IsReg() ||
      ExprHasMemOrVolatile(uslot->pattern)) {
    return false;
  }
  const int ureg = static_cast<int>(uslot->pattern.SetDest().int_val);
  if (ureg <= 0) {
    return false;
  }
  const size_t utarget_b = ublk.successors[0];
  if (utarget_b > 0 && utarget_b < out.rpo_blocks.size() && utarget_b - 1 != ub &&
      !(BlockHasZeroRealActiveInsns(out.rpo_blocks[utarget_b]) &&
        out.rpo_blocks[utarget_b].successors.empty()) &&
      IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&ureg, 1), utarget_b)) {
    const auto& ufall_blk = out.rpo_blocks[utarget_b - 1];
    const bool ufall_reaches_target =
        std::find(ufall_blk.successors.begin(), ufall_blk.successors.end(), utarget_b) !=
        ufall_blk.successors.end();
    bool ufall_falls_through = ufall_reaches_target && !ufall_blk.instructions.empty();
    if (ufall_falls_through) {
      auto uflast = ParsePatternAndNotes(ufall_blk.instructions.back());
      ufall_falls_through = uflast.ok() && !IsUncondJumpOrReturnOrTableJumpTopExpr(uflast->pattern);
    }
    if (ufall_falls_through) {
      for (size_t k = ufall_blk.instructions.size(); k > 0;) {
        --k;
        auto ufp = ParsePatternAndNotes(ufall_blk.instructions[k]);
        if (!ufp.ok() || IsLoopHeaderNote(ufp->pattern) || ufp->pattern.op == RtlOp::kUse ||
            ufp->pattern.op == RtlOp::kClobber) {
          continue;
        }
        if (ufp->pattern.IsSet() && ufp->pattern.SetDest().IsReg() &&
            static_cast<int>(ufp->pattern.SetDest().int_val) == ureg &&
            ufp->pattern != uslot->pattern) {
          return false;
        }
        break;
      }
    }
  }
  const auto& pblk = out.rpo_blocks[ub - 1];
  if (pblk.successors.size() != 2 || pblk.instructions.empty()) {
    return false;
  }
  const size_t prev_target_b = (pblk.successors[0] == ub) ? pblk.successors[1] : pblk.successors[0];
  if (prev_target_b >= out.rpo_blocks.size() || prev_target_b == ub) {
    return false;
  }
  auto plast = ParsePatternAndNotes(pblk.instructions.back());
  if (!plast.ok()) {
    return false;
  }
  if (!entered_with_filled_end_jump[ub - 1] && IsCondJumpExpr(plast->pattern)) {
    const auto& pt_blk = out.rpo_blocks[prev_target_b];
    if (BlockHasZeroRealActiveInsns(pt_blk) && pt_blk.successors.empty() &&
        (ureg == 2 || ureg == 3 || ureg == 32 || ureg == 33)) {
      return false;
    }
    return !IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&ureg, 1), prev_target_b);
  }
  if (plast->pattern.op == RtlOp::kSequence && plast->pattern.children.size() == 2 &&
      plast->pattern.children[0].op == RtlOp::kJumpInsn &&
      plast->pattern.children[1].op == RtlOp::kInsn) {
    auto pjump = RtlPatternAndNotes::FromSequenceSubInsn(plast->pattern.children[0]);
    auto pslot = RtlPatternAndNotes::FromSequenceSubInsn(plast->pattern.children[1]);
    if (!pjump.ok() || !pslot.ok() || !IsCondJumpExpr(pjump->pattern) ||
        pjump->pattern.children.size() != 2 ||
        pjump->pattern.children[1].op != RtlOp::kIfThenElse ||
        pjump->pattern.children[1].children.size() != 3) {
      return false;
    }
    const RtlExpr& pcond = pjump->pattern.children[1].children[0];
    const auto& pt_blk = out.rpo_blocks[prev_target_b];
    bool pt_has_loop_note = false;
    if (!pt_blk.instructions.empty()) {
      auto ptf = ParsePatternAndNotes(pt_blk.instructions.front());
      pt_has_loop_note = ptf.ok() && IsLoopHeaderNote(ptf->pattern);
    }
    const bool pslot_kills_ureg = pslot->pattern.SetDest().IsReg() &&
                                  static_cast<int>(pslot->pattern.SetDest().int_val) == ureg &&
                                  !ExprReferencesRegInSrcOrMem(pslot->pattern, ureg);
    const bool pt_is_empty_ret_exit = BlockHasZeroRealActiveInsns(pt_blk) &&
                                      pt_blk.successors.empty() &&
                                      (ureg == 2 || ureg == 3 || ureg == 32 || ureg == 33);
    const bool ureg_dead_at_prev_target =
        !pt_is_empty_ret_exit &&
        !IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&ureg, 1), prev_target_b);
    if (pcond.op == RtlOp::kEq && prev_target_b > ub - 1 && !pt_has_loop_note &&
        pslot->pattern.IsSet() && (pslot_kills_ureg || ureg_dead_at_prev_target) &&
        pslot->pattern != uslot->pattern) {
      return true;
    }
  }
  return false;
}

bool BlockHasZeroRealActiveInsns(const RtlBasicBlock& blk) {
  for (const std::string& s : blk.instructions) {
    auto p = ParsePatternAndNotes(s);
    if (p.ok() && IsRealActiveInsn(p->pattern)) {
      return false;
    }
  }
  return true;
}

std::optional<size_t> FindSharedReturnLatchInsertBlock(
    const RtlControlFlowGraph& out, const std::vector<std::vector<size_t>>& preds,
    size_t fallthrough_b) {
  size_t insert_b = fallthrough_b;
  while (insert_b < out.rpo_blocks.size()) {
    const auto& blk = out.rpo_blocks[insert_b];
    if (blk.has_label || preds[insert_b].size() != 1 || blk.successors.size() != 1 ||
        blk.successors[0] != insert_b + 1 || blk.instructions.empty()) {
      break;
    }
    bool all_notes = true;
    for (const std::string& s : blk.instructions) {
      auto p = ParsePatternAndNotes(s);
      if (!p.ok() || p->pattern.op != RtlOp::kNote) {
        all_notes = false;
        break;
      }
    }
    if (!all_notes) {
      break;
    }
    ++insert_b;
  }
  if (insert_b >= out.rpo_blocks.size()) {
    return std::nullopt;
  }
  size_t cur = insert_b;
  bool saw_shared = false;
  while (cur < out.rpo_blocks.size()) {
    const auto& cblk = out.rpo_blocks[cur];
    if (cblk.has_label || preds[cur].size() >= 2) {
      saw_shared = true;
    }
    const bool is_exit = cblk.successors.empty();
    for (size_t fi = 0; fi < cblk.instructions.size(); ++fi) {
      auto fp = ParsePatternAndNotes(cblk.instructions[fi]);
      if (!fp.ok()) {
        return std::nullopt;
      }
      const bool is_use_or_clobber =
          (fp->pattern.op == RtlOp::kUse || fp->pattern.op == RtlOp::kClobber);
      const bool is_last_return =
          (is_exit && fi + 1 == cblk.instructions.size() && IsReturnExpr(fp->pattern));
      if (!is_use_or_clobber && !is_last_return) {
        return std::nullopt;
      }
    }
    if (is_exit) {
      return saw_shared ? std::optional<size_t>(insert_b) : std::nullopt;
    }
    if (cblk.successors.size() != 1 || cblk.successors[0] != cur + 1) {
      return std::nullopt;
    }
    cur = cur + 1;
  }
  return std::nullopt;
}

}  // namespace

RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& dbr_eager_cfg_x) {
  if (!IsValidCfg(dbr_eager_cfg_x)) {
    return {};
  }

  RtlControlFlowGraph out = dbr_eager_cfg_x;
  out.pass = RtlPassKind::kDbr;

  std::vector<bool> entered_with_filled_end_jump(out.rpo_blocks.size(), false);
  for (size_t i = 0; i < out.rpo_blocks.size(); ++i) {
    if (out.rpo_blocks[i].instructions.empty()) {
      continue;
    }
    auto p = ParsePatternAndNotes(out.rpo_blocks[i].instructions.back());
    if (p.ok() && p->pattern.op == RtlOp::kSequence && p->pattern.children.size() == 2 &&
        p->pattern.children[0].op == RtlOp::kJumpInsn) {
      entered_with_filled_end_jump[i] = true;
    }
  }
  auto insert_block_at = [&](size_t insert_idx, RtlBasicBlock new_blk) {
    InsertBlockAt(&out, insert_idx, std::move(new_blk));
    entered_with_filled_end_jump.insert(
        entered_with_filled_end_jump.begin() + static_cast<ptrdiff_t>(insert_idx), false);
  };
  auto erase_block_at = [&](size_t erase_idx) {
    EraseBlockAt(&out, erase_idx);
    entered_with_filled_end_jump.erase(entered_with_filled_end_jump.begin() +
                                       static_cast<ptrdiff_t>(erase_idx));
  };

  // Iterate in reverse block order to invert forward `fill_eager_delay_slots`
  // transformations (`fill_slots_from_thread`, `steal_delay_list_from_target`,
  // `steal_delay_list_from_fallthrough`).
  for (size_t rev_idx = out.rpo_blocks.size(); rev_idx > 0; --rev_idx) {
    size_t b = rev_idx - 1;
    if (b >= out.rpo_blocks.size()) {
      continue;
    }
    auto& blk = out.rpo_blocks[b];
    if (blk.instructions.empty()) {
      continue;
    }
    // `optimize_skip` sequences have `same_succ == true` and belong to
    // `fill_simple_delay_slots`.
    if (blk.successors.size() == 2 && blk.successors[0] == blk.successors[1]) {
      continue;
    }

    auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
    if (!last_parsed.ok() || last_parsed->pattern.op != RtlOp::kSequence ||
        last_parsed->pattern.children.size() != 2) {
      continue;
    }
    const RtlExpr& jump_sub = last_parsed->pattern.children[0];
    const RtlExpr& slot_sub = last_parsed->pattern.children[1];
    if (jump_sub.op != RtlOp::kJumpInsn || slot_sub.op != RtlOp::kInsn ||
        jump_sub.children.empty() || slot_sub.children.empty()) {
      continue;
    }
    auto jump_pat_notes = RtlPatternAndNotes::FromSequenceSubInsn(jump_sub);
    auto slot_pat_notes = RtlPatternAndNotes::FromSequenceSubInsn(slot_sub);
    if (!jump_pat_notes.ok() || !slot_pat_notes.ok()) {
      continue;
    }
    const RtlExpr& jump_pat = jump_pat_notes->pattern;
    const bool is_cond_jump = IsCondJumpExpr(jump_pat);
    const bool is_simple_uncond_jump = IsSimpleUncondJumpExpr(jump_pat);
    if (!is_cond_jump && !is_simple_uncond_jump) {
      continue;
    }

    // Case A: Fallthrough-thread (`!slot_sub.flags.in_struct`, i.e. `insn`
    // without `/s`) on a conditional jump (`is_cond_jump`), unless assembly
    // erased `INSN_FROM_TARGET_P` (`insn/s`) on a non-annulled target-thread
    // steal whose destination register is immediately killed on `fallthrough_b`
    // (or read-before-written at `target_b`).
    bool non_annulled_target_steal = false;
    if (!slot_sub.flags.in_struct) {
      if (!is_cond_jump) {
        const auto cur_preds = BuildPredecessors(out);
        if (is_simple_uncond_jump &&
            IsUncondTargetStealAfterCond(out, b, cur_preds, entered_with_filled_end_jump)) {
          non_annulled_target_steal = true;
        } else {
          continue;
        }
      }
      if (!non_annulled_target_steal) {
        std::vector<int> slot_defs;
        CollectDefinedRegs(slot_pat_notes->pattern, &slot_defs);
        bool jump_reads_slot_def = false;
        for (int r : slot_defs) {
          if (ExprReferencesRegInSrcOrMem(jump_pat, r)) {
            jump_reads_slot_def = true;
            break;
          }
        }

        // Trailing conditional jump (e.g., loop latch) whose fallthrough block
        // became empty when Stage 0 stripped the function epilogue: restore the
        // trailing fallthrough block holding `slot_pat`, unless `slot_defs` is
        // live at the branch target (proving `slot_pat` was stolen from target).
        if (blk.successors.size() == 1 && b + 1 == out.rpo_blocks.size() &&
            (jump_reads_slot_def || jump_sub.flags.unchanging)) {
          if (jump_sub.flags.unchanging ||
              !IsAnyRegLiveAtBlockEntry(out, slot_defs, blk.successors[0])) {
            blk.instructions.back() = jump_pat_notes->ToGccPrintRtlString();
            RtlBasicBlock new_ft;
            new_ft.id = static_cast<uint32_t>(b + 1);
            new_ft.has_label = false;
            new_ft.instructions.push_back(slot_pat_notes->ToGccPrintRtlString());
            out.rpo_blocks[b].successors.push_back(b + 1);
            out.rpo_blocks.push_back(std::move(new_ft));
            entered_with_filled_end_jump.push_back(false);
            continue;
          }
          non_annulled_target_steal = true;
        }

        if (!non_annulled_target_steal) {
          if (blk.successors.size() != 2 || b + 1 >= out.rpo_blocks.size()) {
            continue;
          }
          const size_t fallthrough_b = b + 1;
          const size_t cond_target_b =
              (blk.successors[0] == fallthrough_b) ? blk.successors[1] : blk.successors[0];

          const auto preds = BuildPredecessors(out);
          auto& ft_blk = out.rpo_blocks[fallthrough_b];
          const bool own_fallthrough =
              !ft_blk.has_label && preds[fallthrough_b].size() == 1 && preds[fallthrough_b][0] == b;

          bool ft_stole_from_end_jump = false;
          RtlPatternAndNotes ft_bare_jump_pat_notes;
          if (own_fallthrough && !entered_with_filled_end_jump[fallthrough_b] &&
              ft_blk.instructions.size() == 1) {
            auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
            if (ft_parsed.ok() &&
                (IsSimpleUncondJumpExpr(ft_parsed->pattern) || IsReturnExpr(ft_parsed->pattern))) {
              ft_stole_from_end_jump = true;
              ft_bare_jump_pat_notes = *ft_parsed;
            }
          } else if (own_fallthrough && !entered_with_filled_end_jump[fallthrough_b] &&
                     ft_blk.instructions.size() == 2) {
            auto mid_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
            auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[1]);
            if (mid_parsed.ok() && ft_parsed.ok() && mid_parsed->pattern.IsSet() &&
                mid_parsed->pattern.SetDest().IsMem() &&
                IsSimpleUncondJumpExpr(ft_parsed->pattern) &&
                !ExpressionsConflict(mid_parsed->pattern, slot_pat_notes->pattern)) {
              ft_stole_from_end_jump = true;
              ft_bare_jump_pat_notes = *ft_parsed;
            }
          }

          if (ft_stole_from_end_jump && !jump_sub.flags.unchanging) {
            if ((cond_target_b < out.rpo_blocks.size() &&
                 IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b)) ||
                (jump_reads_slot_def &&
                 IsSlotKilledOnFallthrough(out, slot_pat_notes->pattern, fallthrough_b))) {
              ft_stole_from_end_jump = false;
            } else if (IsReturnExpr(ft_bare_jump_pat_notes.pattern) && !jump_reads_slot_def) {
              bool sets_ret_val_or_mem =
                  slot_pat_notes->pattern.IsSet() && slot_pat_notes->pattern.SetDest().IsMem();
              for (int r : slot_defs) {
                if (r == 2 || r == 3 || r == 32 || r == 33) {
                  sets_ret_val_or_mem = true;
                  break;
                }
              }
              if (!sets_ret_val_or_mem) {
                ft_stole_from_end_jump = false;
              }
            }
          }

          if (!jump_sub.flags.unchanging && jump_reads_slot_def && !ft_stole_from_end_jump &&
              !IsAnyRegLiveAtBlockEntry(out, slot_defs, fallthrough_b) &&
              (IsSlotKilledOnFallthrough(out, slot_pat_notes->pattern, fallthrough_b) ||
               (cond_target_b < out.rpo_blocks.size() &&
                IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b)))) {
            non_annulled_target_steal = true;
          }

          std::optional<size_t> shared_ret_insert_b;
          if (!non_annulled_target_steal && !jump_sub.flags.unchanging && !jump_reads_slot_def &&
              !ft_stole_from_end_jump && cond_target_b <= b &&
              cond_target_b < out.rpo_blocks.size() && !slot_defs.empty() &&
              !IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b) &&
              !IsSlotKilledOnFallthrough(out, slot_pat_notes->pattern, fallthrough_b)) {
            bool sets_ret_reg = false;
            for (int r : slot_defs) {
              if (r == 2 || r == 3 || r == 32 || r == 33) {
                sets_ret_reg = true;
                break;
              }
            }
            if (sets_ret_reg) {
              shared_ret_insert_b = FindSharedReturnLatchInsertBlock(out, preds, fallthrough_b);
            }
          }
          const bool shared_return_latch_ft = shared_ret_insert_b.has_value();

          if (!non_annulled_target_steal) {
            if (!jump_sub.flags.unchanging && !jump_reads_slot_def && !ft_stole_from_end_jump &&
                !shared_return_latch_ft) {
              continue;
            }

            // Unpack `b`'s conditional jump.
            blk.instructions.back() = jump_pat_notes->ToGccPrintRtlString();

            // If `fallthrough_b` ends with a bare unconditional jump or return from
            // which `slot_pat` was stolen via `steal_delay_list_from_fallthrough`,
            // re-wrap that terminating jump into a `SEQUENCE`.
            if (ft_stole_from_end_jump && (ft_blk.instructions.size() == 2 ||
                                           !jump_reads_slot_def || jump_sub.flags.unchanging)) {
              ft_blk.instructions.back() =
                  RtlExpr::MakeSequence(
                      {
                          RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{},
                                                   ft_bare_jump_pat_notes),
                          RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, *slot_pat_notes),
                      })
                      .ToGccPrintRtlString();
            } else if (own_fallthrough && !shared_return_latch_ft) {
              ft_blk.instructions.insert(ft_blk.instructions.begin(),
                                         slot_pat_notes->ToGccPrintRtlString());
            } else {
              const size_t ins_b = shared_return_latch_ft ? *shared_ret_insert_b : fallthrough_b;
              RtlBasicBlock new_ft;
              new_ft.has_label = false;
              new_ft.instructions.push_back(slot_pat_notes->ToGccPrintRtlString());
              new_ft.successors = {ins_b + 1};
              insert_block_at(ins_b, std::move(new_ft));
              // `InsertBlockAt` shifted all successors `>= ins_b` by +1, so
              // `ins_b - 1`'s fallthrough edge must point to `ins_b` (`new_ft`).
              for (size_t& succ : out.rpo_blocks[ins_b - 1].successors) {
                if (succ == ins_b + 1) {
                  succ = ins_b;
                  break;
                }
              }
            }
            continue;
          }
        }
      }
    }

    // Case B: Target-thread (`slot_sub.flags.in_struct == true`, i.e. `insn/s`,
    // with `same_succ == false`).
    size_t target_b = blk.successors[0];
    if (is_cond_jump && blk.successors.size() == 2) {
      const size_t fallthrough_b = b + 1;
      target_b = (blk.successors[0] == fallthrough_b) ? blk.successors[1] : blk.successors[0];
    }
    if (target_b >= out.rpo_blocks.size()) {
      continue;
    }

    const RtlExpr orig_slot_pat = slot_pat_notes->pattern;
    auto preds = BuildPredecessors(out);
    if (is_cond_jump && jump_sub.flags.unchanging && blk.successors.size() == 2 &&
        b + 1 < out.rpo_blocks.size() && preds[target_b].size() > 1) {
      // In `reorg.c`, any non-owning target steal in `fill_eager_delay_slots`
      // (`fill_slots_from_thread` with `own_target == 0` or
      // `steal_delay_list_from_target`) redirects the branch to
      // `get_label_before(new_thread)` where `new_thread` comes from
      // `next_active_insn(...)`, which skips `USE` and `CLOBBER` insns.
      // Thus if a shared `target_b` begins with `(use ...)` or `(clobber ...)`
      // before a real active insn (and is not Case T1), `fill_eager_delay_slots`
      // could never have redirected `b` to `target_b`: this `SEQUENCE` came
      // from `fill_simple_delay_slots:optimize_skip` Form 2 (`reorg.c:1168-1172`).
      const auto& ft_blk = out.rpo_blocks[b + 1];
      const auto& tgt_blk = out.rpo_blocks[target_b];
      bool tgt_starts_with_use_or_clobber_before_active = false;
      bool saw_leading_use_or_clobber = false;
      for (const std::string& t_insn : tgt_blk.instructions) {
        auto tp = ParsePatternAndNotes(t_insn);
        if (!tp.ok() || IsLoopHeaderNote(tp->pattern)) {
          continue;
        }
        if (tp->pattern.op == RtlOp::kUse || tp->pattern.op == RtlOp::kClobber) {
          saw_leading_use_or_clobber = true;
          continue;
        }
        if (IsRealActiveInsn(tp->pattern)) {
          tgt_starts_with_use_or_clobber_before_active = saw_leading_use_or_clobber;
        }
        break;
      }
      if (tgt_starts_with_use_or_clobber_before_active && !ft_blk.has_label &&
          preds[b + 1].size() == 1 && preds[b + 1][0] == b && ft_blk.instructions.size() == 1) {
        auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
        if (ft_parsed.ok() &&
            ((IsSimpleUncondJumpExpr(ft_parsed->pattern) && ft_blk.successors.size() == 1 &&
              ft_blk.successors[0] == target_b) ||
             (IsReturnExpr(ft_parsed->pattern) && ft_blk.successors.empty()))) {
          continue;
        }
      }
    }
    bool pred_falls_through_to_target =
        target_b > 0 && (target_b - 1 != b) && BlockFallsThroughToNext(out, preds, target_b - 1);

    // Find all jumping predecessors of `target_b` (including `b`) that have an
    // `insn/s` delay slot with the same `slot_pat_only_sexpr` targeting
    // `target_b`, as well as whether `target_b` has any other jumping
    // predecessors.
    std::vector<size_t> same_slot_jumpers;
    bool has_other_jumpers = false;
    for (size_t p_blk : preds[target_b]) {
      if (p_blk == target_b - 1 && p_blk != b) {
        const auto& p_succs = out.rpo_blocks[p_blk].successors;
        bool ends_with_jump = false;
        if (!out.rpo_blocks[p_blk].instructions.empty()) {
          auto p_last = ParsePatternAndNotes(out.rpo_blocks[p_blk].instructions.back());
          if (p_last.ok()) {
            ends_with_jump = IsAnyJumpOrReturnTopExpr(p_last->pattern);
          }
        }
        if (!ends_with_jump ||
            (pred_falls_through_to_target && p_succs.size() == 1 && p_succs[0] == target_b)) {
          continue;
        }
      }
      if (p_blk == b) {
        same_slot_jumpers.push_back(b);
        continue;
      }
      const auto& other_blk = out.rpo_blocks[p_blk];
      bool matched_same_slot = false;
      if (!other_blk.instructions.empty() &&
          !(other_blk.successors.size() == 2 &&
            other_blk.successors[0] == other_blk.successors[1])) {
        auto other_last = ParsePatternAndNotes(other_blk.instructions.back());
        if (other_last.ok() && other_last->pattern.op == RtlOp::kSequence &&
            other_last->pattern.children.size() == 2 &&
            other_last->pattern.children[0].op == RtlOp::kJumpInsn &&
            other_last->pattern.children[1].op == RtlOp::kInsn) {
          auto other_jump =
              RtlPatternAndNotes::FromSequenceSubInsn(other_last->pattern.children[0]);
          auto other_slot =
              RtlPatternAndNotes::FromSequenceSubInsn(other_last->pattern.children[1]);
          if (other_jump.ok() && other_slot.ok()) {
            const bool other_is_cond = IsCondJumpExpr(other_jump->pattern);
            const bool other_is_uncond = IsSimpleUncondJumpExpr(other_jump->pattern);
            std::vector<int> other_slot_defs;
            CollectDefinedRegs(other_slot->pattern, &other_slot_defs);
            bool other_jump_reads_slot_def = false;
            for (int r : other_slot_defs) {
              if (ExprReferencesRegInSrcOrMem(other_jump->pattern, r)) {
                other_jump_reads_slot_def = true;
                break;
              }
            }
            size_t other_target =
                other_blk.successors.empty() ? out.rpo_blocks.size() : other_blk.successors[0];
            if (other_is_cond && other_blk.successors.size() == 2) {
              other_target = (other_blk.successors[0] == p_blk + 1) ? other_blk.successors[1]
                                                                    : other_blk.successors[0];
            }
            const bool other_from_target =
                other_last->pattern.children[1].flags.in_struct ||
                (other_is_uncond &&
                 IsUncondTargetStealAfterCond(out, p_blk, preds, entered_with_filled_end_jump)) ||
                (other_is_cond && !other_last->pattern.children[0].flags.unchanging &&
                 other_jump_reads_slot_def && other_blk.successors.size() == 2 &&
                 p_blk + 1 < out.rpo_blocks.size() &&
                 !IsAnyRegLiveAtBlockEntry(out, other_slot_defs, p_blk + 1) &&
                 (IsSlotKilledOnFallthrough(out, other_slot->pattern, p_blk + 1) ||
                  (other_target < out.rpo_blocks.size() &&
                   IsAnyRegLiveAtBlockEntry(out, other_slot_defs, other_target))));
            if (other_from_target && (other_is_cond || other_is_uncond)) {
              if (other_target == target_b && other_slot->pattern == orig_slot_pat) {
                matched_same_slot = true;
              }
            }
          }
        }
      }
      if (matched_same_slot) {
        same_slot_jumpers.push_back(p_blk);
      } else {
        has_other_jumpers = true;
      }
    }

    bool need_loop_vtop = false;
    if (!out.rpo_blocks[target_b].successors.empty()) {
      bool target_is_loop_hdr = (is_cond_jump && target_b <= b);
      if (!target_is_loop_hdr) {
        for (size_t p_blk : preds[target_b]) {
          if (p_blk >= target_b) {
            target_is_loop_hdr = true;
            break;
          }
        }
      }
      if (target_is_loop_hdr) {
        bool any_jumper_needs_loop_pred = false;
        for (size_t j_blk : same_slot_jumpers) {
          if (out.rpo_blocks[j_blk].instructions.empty()) {
            continue;
          }
          auto j_last = ParsePatternAndNotes(out.rpo_blocks[j_blk].instructions.back());
          if (!j_last.ok() || j_last->pattern.op != RtlOp::kSequence ||
              j_last->pattern.children.empty()) {
            continue;
          }
          auto j_jump = RtlPatternAndNotes::FromSequenceSubInsn(j_last->pattern.children[0]);
          if (!j_jump.ok() || !IsCondJumpExpr(j_jump->pattern) ||
              j_jump->pattern.children.size() != 2 ||
              j_jump->pattern.children[1].op != RtlOp::kIfThenElse ||
              j_jump->pattern.children[1].children.size() != 3) {
            continue;
          }
          const RtlExpr& cond = j_jump->pattern.children[1].children[0];
          const bool cond_predicts_false_without_loop =
              cond.op == RtlOp::kEq ||
              ((cond.op == RtlOp::kLe || cond.op == RtlOp::kLt) && cond.children.size() == 2 &&
               cond.children[1].op == RtlOp::kConstInt && cond.children[1].int_val == 0);
          if (!cond_predicts_false_without_loop) {
            continue;
          }
          bool ft_has_competing_candidate = false;
          if (j_blk + 1 < out.rpo_blocks.size() && !out.rpo_blocks[j_blk + 1].has_label &&
              preds[j_blk + 1].size() == 1 && preds[j_blk + 1][0] == j_blk) {
            for (const std::string& ft_s : out.rpo_blocks[j_blk + 1].instructions) {
              auto ftp = ParsePatternAndNotes(ft_s);
              if (!ftp.ok() || IsLoopHeaderNote(ftp->pattern) || ftp->pattern.op == RtlOp::kUse ||
                  ftp->pattern.op == RtlOp::kClobber) {
                continue;
              }
              if (ftp->pattern.IsSet() && ftp->pattern.SetDest().IsReg() &&
                  !ExprHasMemOrVolatile(ftp->pattern)) {
                const int ft_dst = static_cast<int>(ftp->pattern.SetDest().int_val);
                if (ft_dst > 0 &&
                    !IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&ft_dst, 1), target_b)) {
                  ft_has_competing_candidate = true;
                }
              }
              break;
            }
          }
          if (ft_has_competing_candidate) {
            any_jumper_needs_loop_pred = true;
            break;
          }
        }
        if (any_jumper_needs_loop_pred) {
          bool forward_fallthrough_jumper_conflict = false;
          for (size_t p_blk : preds[target_b]) {
            if (p_blk < target_b &&
                std::find(same_slot_jumpers.begin(), same_slot_jumpers.end(), p_blk) ==
                    same_slot_jumpers.end() &&
                !out.rpo_blocks[p_blk].instructions.empty()) {
              auto p_last = ParsePatternAndNotes(out.rpo_blocks[p_blk].instructions.back());
              if (p_last.ok() && p_last->pattern.op == RtlOp::kSequence &&
                  p_last->pattern.children.size() == 2 &&
                  !p_last->pattern.children[1].flags.in_struct) {
                forward_fallthrough_jumper_conflict = true;
                break;
              }
            }
          }
          need_loop_vtop = !forward_fallthrough_jumper_conflict;
        }
      }
    }
    auto maybe_prepend_loop_vtop = [&](size_t hdr_b) {
      if (!need_loop_vtop || hdr_b >= out.rpo_blocks.size()) {
        return;
      }
      auto& hdr_blk = out.rpo_blocks[hdr_b];
      if (!hdr_blk.instructions.empty()) {
        auto first_p = ParsePatternAndNotes(hdr_blk.instructions.front());
        if (first_p.ok() && IsLoopHeaderNote(first_p->pattern)) {
          return;
        }
      }
      if (hdr_b > 0 && !out.rpo_blocks[hdr_b - 1].instructions.empty()) {
        auto prev_p = ParsePatternAndNotes(out.rpo_blocks[hdr_b - 1].instructions.back());
        if (prev_p.ok() && IsLoopHeaderNote(prev_p->pattern)) {
          return;
        }
      }
      hdr_blk.instructions.insert(hdr_blk.instructions.begin(),
                                  RtlExpr::MakeNote("NOTE_INSN_LOOP_VTOP").ToGccPrintRtlString());
    };

    // If `slot_pat_notes` is `(set (reg:SI R) (const_int C))` lifted from
    // `li $R, C` (which erased a narrower `QI`/`HI` destination mode) and `b`
    // (or `b - 1`'s non-annulled fallthrough delay slot) already sets `(reg:SI R)`
    // to `C` before `b`'s branch while `target_b`'s first active instruction
    // reads `(reg:QI R)` / `(reg:HI R)`, restore the subword register mode so
    // `reorg.c:redundant_insn` does not falsely delete the target instruction.
    if (slot_pat_notes->pattern.IsSet() && slot_pat_notes->pattern.SetDest().IsReg() &&
        slot_pat_notes->pattern.SetDest().mode == RtlMachineMode::kSi) {
      const int dst_r = static_cast<int>(slot_pat_notes->pattern.SetDest().int_val);
      const RtlExpr& src_e = slot_pat_notes->pattern.SetSrc();
      std::optional<int64_t> const_val;
      if (src_e.op == RtlOp::kConstInt) {
        const_val = src_e.int_val;
      } else if (src_e.op == RtlOp::kPlus && src_e.children.size() == 2 &&
                 src_e.children[0].IsReg() && src_e.children[0].int_val == 0 &&
                 src_e.children[1].IsReg() && src_e.children[1].int_val == 0) {
        const_val = 0;
      }
      if (const_val.has_value() && dst_r > 0) {
        bool prior_same_si_set = false;
        bool dst_clobbered_in_b = false;
        for (size_t idx = blk.instructions.size() - 1; idx > 0; --idx) {
          auto bp = ParsePatternAndNotes(blk.instructions[idx - 1]);
          if (!bp.ok() || !IsRealActiveInsn(bp->pattern)) {
            continue;
          }
          if (bp->pattern == orig_slot_pat) {
            prior_same_si_set = true;
            break;
          }
          std::vector<int> bdefs;
          CollectDefinedRegs(bp->pattern, &bdefs);
          if (std::find(bdefs.begin(), bdefs.end(), dst_r) != bdefs.end()) {
            dst_clobbered_in_b = true;
            break;
          }
        }
        if (!prior_same_si_set && !dst_clobbered_in_b && b > 0 && !blk.has_label &&
            preds[b].size() == 1 && preds[b][0] == b - 1 &&
            !out.rpo_blocks[b - 1].instructions.empty()) {
          auto prev_p = ParsePatternAndNotes(out.rpo_blocks[b - 1].instructions.back());
          if (prev_p.ok() && prev_p->pattern.op == RtlOp::kSequence &&
              prev_p->pattern.children.size() == 2 &&
              !prev_p->pattern.children[0].flags.unchanging) {
            auto prev_s = RtlPatternAndNotes::FromSequenceSubInsn(prev_p->pattern.children[1]);
            if (prev_s.ok() && prev_s->pattern == orig_slot_pat) {
              prior_same_si_set = true;
            }
          }
        }
        if (prior_same_si_set) {
          for (const std::string& ts : out.rpo_blocks[target_b].instructions) {
            auto tp = ParsePatternAndNotes(ts);
            if (!tp.ok() || !IsRealActiveInsn(tp->pattern)) {
              continue;
            }
            bool has_qi = false;
            bool has_hi = false;
            bool has_other_mode = false;
            auto scan_src_reg_modes = [&](const RtlExpr& root) {
              auto visit_sub = [&](const RtlExpr& sub) {
                sub.Visit([&](const RtlExpr& n) {
                  if (n.IsReg() && static_cast<int>(n.int_val) == dst_r) {
                    if (n.mode == RtlMachineMode::kQI) {
                      has_qi = true;
                    } else if (n.mode == RtlMachineMode::kHI) {
                      has_hi = true;
                    } else {
                      has_other_mode = true;
                    }
                  }
                });
              };
              if (root.IsSet()) {
                if (root.SetDest().op == RtlOp::kMem) {
                  visit_sub(root.SetDest());
                }
                visit_sub(root.SetSrc());
              } else {
                visit_sub(root);
              }
            };
            scan_src_reg_modes(tp->pattern);
            if (!has_other_mode && has_qi && !has_hi && *const_val >= -128 && *const_val <= 255) {
              slot_pat_notes->pattern =
                  RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kQI, dst_r,
                                                    slot_pat_notes->pattern.SetDest().flags),
                                   RtlExpr::MakeConstInt(*const_val));
            } else if (!has_other_mode && has_hi && !has_qi && *const_val >= -32768 &&
                       *const_val <= 65535) {
              slot_pat_notes->pattern =
                  RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kHI, dst_r,
                                                    slot_pat_notes->pattern.SetDest().flags),
                                   RtlExpr::MakeConstInt(*const_val));
            }
            break;
          }
        }
      }
    }

    // Unpack the terminating `SEQUENCE` of every block in `same_slot_jumpers`.
    for (size_t p_blk : same_slot_jumpers) {
      auto& p_block = out.rpo_blocks[p_blk];
      auto p_last = ParsePatternAndNotes(p_block.instructions.back());
      if (p_last.ok() && p_last->pattern.op == RtlOp::kSequence &&
          !p_last->pattern.children.empty()) {
        auto p_jump = RtlPatternAndNotes::FromSequenceSubInsn(p_last->pattern.children[0]);
        if (p_jump.ok()) {
          p_block.instructions.back() = p_jump->ToGccPrintRtlString();
        }
      }
    }

    // If `reorg.c:next_active_insn` skipped a `(use ...)`/`(clobber ...)`-only
    // block `target_b - 1` and inserted `target_b`'s label solely for
    // `same_slot_jumpers`, re-join `target_b` onto `target_b - 1` so Case T1
    // inspects `target_b - 2`.
    if (pred_falls_through_to_target && !has_other_jumpers && target_b >= 2 &&
        (target_b - 2 != b) && BlockHasZeroRealActiveInsns(out.rpo_blocks[target_b - 1]) &&
        BlockFallsThroughToNext(out, preds, target_b - 2)) {
      const auto& prev2_blk = out.rpo_blocks[target_b - 2];
      ptrdiff_t prev2_active = -1;
      for (size_t i = 0; i < prev2_blk.instructions.size(); ++i) {
        auto p = ParsePatternAndNotes(prev2_blk.instructions[i]);
        if (p.ok() && IsRealActiveInsn(p->pattern)) {
          prev2_active = static_cast<ptrdiff_t>(i);
        }
      }
      if (prev2_active >= 0) {
        auto p2 = ParsePatternAndNotes(prev2_blk.instructions[static_cast<size_t>(prev2_active)]);
        if (p2.ok() && p2->pattern == orig_slot_pat) {
          const size_t old_target_b = target_b;
          const size_t use_blk_b = target_b - 1;
          for (size_t& p_blk : same_slot_jumpers) {
            for (size_t& succ : out.rpo_blocks[p_blk].successors) {
              if (succ == old_target_b) {
                succ = use_blk_b;
              }
            }
            if (p_blk > old_target_b) {
              --p_blk;
            }
          }
          auto& use_blk = out.rpo_blocks[use_blk_b];
          const auto& ret_blk = out.rpo_blocks[old_target_b];
          use_blk.instructions.insert(use_blk.instructions.end(), ret_blk.instructions.begin(),
                                      ret_blk.instructions.end());
          use_blk.successors = ret_blk.successors;
          use_blk.addr_vec_targets = ret_blk.addr_vec_targets;
          erase_block_at(old_target_b);
          if (old_target_b <= b && rev_idx > 0) {
            --rev_idx;
          }
          if (b > old_target_b) {
            --b;
          }
          target_b = use_blk_b;
          preds = BuildPredecessors(out);
          pred_falls_through_to_target = true;
          has_other_jumpers = false;
          for (size_t p_blk : preds[target_b]) {
            if (p_blk == target_b - 1 && p_blk != b) {
              continue;
            }
            if (std::find(same_slot_jumpers.begin(), same_slot_jumpers.end(), p_blk) ==
                same_slot_jumpers.end()) {
              has_other_jumpers = true;
              break;
            }
          }
        }
      }
    }

    // Check Case T1: `!own_target` copy where `pred_b = target_b - 1` falls
    // through to `target_b` and its last real active instruction (`IsRealActiveInsn`)
    // matches `slot_pat_only_sexpr`.
    if (pred_falls_through_to_target) {
      const size_t pred_b = target_b - 1;
      auto& pred_blk = out.rpo_blocks[pred_b];
      ptrdiff_t last_active_idx = -1;
      size_t active_count = 0;
      for (size_t i = 0; i < pred_blk.instructions.size(); ++i) {
        auto p = ParsePatternAndNotes(pred_blk.instructions[i]);
        if (p.ok() && IsRealActiveInsn(p->pattern)) {
          last_active_idx = static_cast<ptrdiff_t>(i);
          ++active_count;
        }
      }
      if (last_active_idx >= 0) {
        auto last_active_parsed =
            ParsePatternAndNotes(pred_blk.instructions[static_cast<size_t>(last_active_idx)]);
        if (last_active_parsed.ok() && last_active_parsed->pattern == orig_slot_pat) {
          size_t move_start = static_cast<size_t>(last_active_idx);
          while (move_start > 0) {
            auto prev_note = ParsePatternAndNotes(pred_blk.instructions[move_start - 1]);
            if (prev_note.ok() && IsLoopHeaderNote(prev_note->pattern)) {
              --move_start;
            } else {
              break;
            }
          }

          if (active_count == 1 && move_start == 0) {
            // `pred_b` itself was the target block before its label shifted to
            // `target_b`. Redirect all `same_slot_jumpers` to `pred_b`.
            for (size_t p_blk : same_slot_jumpers) {
              for (size_t& succ : out.rpo_blocks[p_blk].successors) {
                if (succ == target_b) {
                  succ = pred_b;
                }
              }
              if (p_blk + 1 == pred_b && out.rpo_blocks[p_blk].successors.size() == 1 &&
                  !out.rpo_blocks[p_blk].instructions.empty()) {
                bool prev_inserts_before_pred = false;
                if (p_blk > 0 && !out.rpo_blocks[p_blk - 1].instructions.empty() &&
                    out.rpo_blocks[p_blk - 1].successors.size() == 2 &&
                    (out.rpo_blocks[p_blk - 1].successors[0] == pred_b ||
                     out.rpo_blocks[p_blk - 1].successors[1] == pred_b)) {
                  auto prev_last =
                      ParsePatternAndNotes(out.rpo_blocks[p_blk - 1].instructions.back());
                  if (prev_last.ok() && prev_last->pattern.op == RtlOp::kSequence &&
                      prev_last->pattern.children.size() == 2 &&
                      prev_last->pattern.children[0].op == RtlOp::kJumpInsn &&
                      prev_last->pattern.children[1].flags.in_struct) {
                    prev_inserts_before_pred = true;
                  }
                }
                auto p_last = ParsePatternAndNotes(out.rpo_blocks[p_blk].instructions.back());
                if (!prev_inserts_before_pred && p_last.ok() &&
                    IsSimpleUncondJumpExpr(p_last->pattern)) {
                  out.rpo_blocks[p_blk].instructions.pop_back();
                }
              }
            }
            out.rpo_blocks[pred_b].has_label = true;
            if (!has_other_jumpers) {
              auto& tgt_blk = out.rpo_blocks[target_b];
              out.rpo_blocks[pred_b].instructions.insert(out.rpo_blocks[pred_b].instructions.end(),
                                                         tgt_blk.instructions.begin(),
                                                         tgt_blk.instructions.end());
              out.rpo_blocks[pred_b].successors = tgt_blk.successors;
              out.rpo_blocks[pred_b].addr_vec_targets = tgt_blk.addr_vec_targets;
              erase_block_at(target_b);
              if (target_b <= b && rev_idx > 0) {
                --rev_idx;
              }
            }
            maybe_prepend_loop_vtop(pred_b);
            continue;
          }

          if (!has_other_jumpers) {
            // Move `move_start .. end` from `pred_b` to the beginning of
            // `target_b`.
            std::vector<std::string> moved_slice(
                pred_blk.instructions.begin() + static_cast<ptrdiff_t>(move_start),
                pred_blk.instructions.end());
            pred_blk.instructions.erase(
                pred_blk.instructions.begin() + static_cast<ptrdiff_t>(move_start),
                pred_blk.instructions.end());
            auto& tgt_blk = out.rpo_blocks[target_b];
            tgt_blk.instructions.insert(tgt_blk.instructions.begin(), moved_slice.begin(),
                                        moved_slice.end());
            tgt_blk.has_label = true;
            maybe_prepend_loop_vtop(target_b);
            continue;
          } else {
            // `has_other_jumpers` is true: `move_start .. end` was a separate
            // fallthrough block `new_blk` between `pred_b` and `target_b`
            // targeted by `same_slot_jumpers`.
            std::vector<std::string> blk_slice(
                pred_blk.instructions.begin() + static_cast<ptrdiff_t>(move_start),
                pred_blk.instructions.end());
            pred_blk.instructions.erase(
                pred_blk.instructions.begin() + static_cast<ptrdiff_t>(move_start),
                pred_blk.instructions.end());

            const size_t old_target_b = target_b;
            RtlBasicBlock new_blk;
            new_blk.has_label = true;
            new_blk.instructions = std::move(blk_slice);
            new_blk.successors = {old_target_b + 1};
            insert_block_at(old_target_b, std::move(new_blk));
            // `pred_b = old_target_b - 1` had `old_target_b` as its fallthrough
            // successor, which `InsertBlockAt` shifted to `old_target_b + 1`.
            // Restore `pred_b`'s fallthrough successor to `old_target_b` (`new_blk`).
            for (size_t& succ : out.rpo_blocks[pred_b].successors) {
              if (succ == old_target_b + 1) {
                succ = old_target_b;
                break;
              }
            }
            for (size_t p_blk : same_slot_jumpers) {
              const size_t new_p_blk = (p_blk >= old_target_b) ? p_blk + 1 : p_blk;
              for (size_t& succ : out.rpo_blocks[new_p_blk].successors) {
                if (succ == old_target_b + 1) {
                  succ = old_target_b;
                  break;
                }
              }
            }
            if (b >= old_target_b) {
              ++rev_idx;
            }
            maybe_prepend_loop_vtop(old_target_b);
            continue;
          }
        }
      }
    }

    // Case T2: `!pred_falls_through_to_target` and `!has_other_jumpers` (all
    // predecessors of `target_b` are in `same_slot_jumpers`).
    if (!pred_falls_through_to_target && !has_other_jumpers) {
      auto& tgt_blk = out.rpo_blocks[target_b];
      tgt_blk.instructions.insert(tgt_blk.instructions.begin(),
                                  slot_pat_notes->ToGccPrintRtlString());
      tgt_blk.has_label = true;
      maybe_prepend_loop_vtop(target_b);
      continue;
    }

    // Case T3: `target_b` is shared with other predecessors (`has_other_jumpers`
    // is true, or `pred_falls_through_to_target` is true without matching Case T1).
    if (!pred_falls_through_to_target) {
      const size_t old_target_b = target_b;
      RtlBasicBlock tramp_blk;
      tramp_blk.has_label = true;
      tramp_blk.instructions.push_back(slot_pat_notes->ToGccPrintRtlString());
      tramp_blk.successors = {old_target_b + 1};
      insert_block_at(old_target_b, std::move(tramp_blk));
      if (b >= old_target_b) {
        ++rev_idx;
      }
      for (size_t p_blk : same_slot_jumpers) {
        const size_t new_p_blk = (p_blk >= old_target_b) ? p_blk + 1 : p_blk;
        for (size_t& succ : out.rpo_blocks[new_p_blk].successors) {
          if (succ == old_target_b + 1) {
            succ = old_target_b;
            break;
          }
        }
      }
      maybe_prepend_loop_vtop(old_target_b);
    } else {
      // Before synthesizing a new `tramp_blk`, check if an existing unlabeled
      // reachable block `cand_b` already holds `[<slot_pat>; j target_b]` (or
      // its `fill_simple_delay_slots` `SEQUENCE` wrapper) whose `CODE_LABEL`
      // was deleted by `redirect_jump` when `same_slot_jumpers` stole from it
      // via `steal_delay_list_from_target`.
      std::optional<size_t> existing_tramp_b;
      bool has_back_edge_to_target = false;
      for (size_t p_blk : preds[target_b]) {
        if (p_blk >= target_b) {
          has_back_edge_to_target = true;
          break;
        }
      }
      if (!has_back_edge_to_target) {
        for (size_t cand_b = 0; cand_b < out.rpo_blocks.size(); ++cand_b) {
          if (cand_b == target_b || out.rpo_blocks[cand_b].has_label || preds[cand_b].empty()) {
            continue;
          }
          bool conflicts_with_jumper = false;
          for (size_t p_blk : same_slot_jumpers) {
            if (cand_b == p_blk || cand_b == p_blk + 1) {
              conflicts_with_jumper = true;
              break;
            }
          }
          if (conflicts_with_jumper) {
            continue;
          }
          const auto& cand_blk = out.rpo_blocks[cand_b];
          if (cand_blk.successors.size() != 1 || cand_blk.successors[0] != target_b) {
            continue;
          }
          if (cand_blk.instructions.size() == 2) {
            auto cs = ParsePatternAndNotes(cand_blk.instructions[0]);
            auto cj = ParsePatternAndNotes(cand_blk.instructions[1]);
            if (cs.ok() && cj.ok() && IsSimpleUncondJumpExpr(cj->pattern) &&
                cs->pattern == orig_slot_pat) {
              out.rpo_blocks[cand_b].instructions = {
                  RtlExpr::MakeSequence(
                      {
                          RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, *cj),
                          RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, *cs),
                      })
                      .ToGccPrintRtlString()};
              existing_tramp_b = cand_b;
              break;
            }
          } else if (cand_blk.instructions.size() == 1) {
            auto cseq = ParsePatternAndNotes(cand_blk.instructions[0]);
            if (cseq.ok() && cseq->pattern.op == RtlOp::kSequence &&
                cseq->pattern.children.size() == 2 &&
                cseq->pattern.children[0].op == RtlOp::kJumpInsn &&
                !cseq->pattern.children[1].flags.in_struct) {
              auto cj = RtlPatternAndNotes::FromSequenceSubInsn(cseq->pattern.children[0]);
              auto cs = RtlPatternAndNotes::FromSequenceSubInsn(cseq->pattern.children[1]);
              if (cj.ok() && cs.ok() && IsSimpleUncondJumpExpr(cj->pattern) &&
                  cs->pattern == orig_slot_pat) {
                existing_tramp_b = cand_b;
                break;
              }
            }
          }
        }
      }
      if (existing_tramp_b.has_value()) {
        const size_t cand_b = *existing_tramp_b;
        out.rpo_blocks[cand_b].has_label = true;
        for (size_t p_blk : same_slot_jumpers) {
          for (size_t& succ : out.rpo_blocks[p_blk].successors) {
            if (succ == target_b) {
              succ = cand_b;
              break;
            }
          }
        }
        maybe_prepend_loop_vtop(cand_b);
        continue;
      }

      // `target_b - 1` falls through to `target_b`, so we cannot insert a block
      // directly before `target_b` without breaking `target_b - 1`'s fallthrough.
      // Instead, insert a block containing `(sequence [(jump_insn (set (pc)
      // (label_ref L_target))) (insn <slot_pat_notes>)])` immediately after a
      // block ending with an unconditional jump or return (so `MaterializeFromCfg`
      // emits a `BARRIER` before `tramp_blk` and `jump.c:delete_insn` deletes
      // `tramp_blk` when its label becomes unused).
      out.rpo_blocks[target_b].has_label = true;
      size_t insert_pos = out.rpo_blocks.size();
      bool found_barrier_pos = false;
      for (size_t b_scan = out.rpo_blocks.size(); b_scan > 0; --b_scan) {
        const auto& scan_blk = out.rpo_blocks[b_scan - 1];
        if (scan_blk.instructions.empty()) {
          continue;
        }
        auto scan_last = ParsePatternAndNotes(scan_blk.instructions.back());
        if (scan_last.ok() && IsUncondJumpOrReturnOrTableJumpTopExpr(scan_last->pattern) &&
            !IsTableJumpExpr(scan_last->pattern)) {
          insert_pos = b_scan;
          found_barrier_pos = true;
          break;
        }
      }
      if (!found_barrier_pos && !out.rpo_blocks.empty() &&
          BlockHasZeroRealActiveInsns(out.rpo_blocks.back())) {
        insert_pos = out.rpo_blocks.size() - 1;
      }

      RtlPatternAndNotes uncond_jump;
      uncond_jump.pattern =
          RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"));
      RtlBasicBlock tramp_blk;
      tramp_blk.has_label = true;
      tramp_blk.instructions.push_back(
          RtlExpr::MakeSequence(
              {
                  RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, uncond_jump),
                  RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, *slot_pat_notes),
              })
              .ToGccPrintRtlString());
      const size_t shifted_target_b = (target_b >= insert_pos) ? (target_b + 1) : target_b;
      tramp_blk.successors = {shifted_target_b};
      insert_block_at(insert_pos, std::move(tramp_blk));
      if (insert_pos + 1 < out.rpo_blocks.size()) {
        out.rpo_blocks[insert_pos + 1].has_label = true;
      }
      if (b >= insert_pos) {
        ++rev_idx;
      }
      for (size_t p_blk : same_slot_jumpers) {
        const size_t new_p_blk = (p_blk >= insert_pos) ? (p_blk + 1) : p_blk;
        for (size_t& succ : out.rpo_blocks[new_p_blk].successors) {
          if (succ == shifted_target_b) {
            succ = insert_pos;
            break;
          }
        }
      }
      maybe_prepend_loop_vtop(insert_pos);
    }
  }

  SynchronizeCanonicalLabels(&out);
  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    out.rpo_blocks[b].id = static_cast<uint32_t>(b);
    out.rpo_blocks[b].SyncRtlInsnsFromInstructions();
  }
  RecomputePostReloadLiveAtStart(&out);
  return out;
}

}  // namespace rom_nom_nom::lifter
