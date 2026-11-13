#include "lifter/invert_relax_delay_slots.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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

// Matches `reorg.c:1395-1470` (`mostly_true_jump` + `get_branch_condition`).
int GetMostlyTrueJumpScore(const RtlExpr& jump_pat, size_t block_b, size_t target_b) {
  const RtlExpr* ite = GetCondJumpIfThenElse(jump_pat);
  if (ite == nullptr || !ite->children[0].IsComparison()) {
    return 0;
  }
  const RtlExpr& cond = ite->children[0];
  RtlOp code = cond.op;
  if (ite->children[1].op == RtlOp::kPc) {
    code = ReverseComparisonOp(code);
  }
  if (cond.children.size() == 2 && cond.children[1].IsConstInt() &&
      cond.children[1].IntVal() == 0) {
    switch (code) {
      case RtlOp::kNe:
      case RtlOp::kGt:
      case RtlOp::kGe:
      case RtlOp::kGtu:
      case RtlOp::kGeu:
        return 1;
      case RtlOp::kEq:
      case RtlOp::kLt:
      case RtlOp::kLe:
      case RtlOp::kLtu:
      case RtlOp::kLeu:
        return 0;
      default:
        break;
    }
  }
  return (target_b <= block_b) ? 1 : 0;
}

bool HasRegDeadConstZeroNote(const RtlPatternAndNotes& pat_notes) {
  for (const RtlRegNote& note : pat_notes.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsConstInt() &&
        note.datum.IntVal() == 0) {
      return true;
    }
  }
  return false;
}

bool HasRegDeadRegNote(const RtlPatternAndNotes& pat_notes) {
  for (const RtlRegNote& note : pat_notes.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
      return true;
    }
  }
  return false;
}

int GetSingleSetDestRegNo(const RtlExpr& pat) {
  if (!pat.IsSet()) {
    return -1;
  }
  const RtlExpr& dst = pat.SetDest();
  if (dst.IsReg()) {
    return dst.RegNo();
  }
  if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg()) {
    return dst.children[0].RegNo() + static_cast<int>(dst.int_val);
  }
  return -1;
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

bool SubtreeReferencesHardReg(const RtlExpr& node, int regno) {
  if (node.IsSubreg() && !node.children.empty() && node.children[0].IsReg()) {
    return node.children[0].RegNo() + static_cast<int>(node.int_val) == regno;
  }
  if (node.IsReg()) {
    const int r = node.RegNo();
    const int n = (node.mode == RtlMachineMode::kDi || node.mode == RtlMachineMode::kDf) ? 2 : 1;
    return regno >= r && regno < r + n;
  }
  for (const RtlExpr& child : node.children) {
    if (SubtreeReferencesHardReg(child, regno)) {
      return true;
    }
  }
  return false;
}

bool ExprSetsOrClobbersReg(const RtlExpr& expr, int regno) {
  bool found = false;
  expr.Visit([&found, regno](const RtlExpr& node) {
    if (found) {
      return;
    }
    if (node.IsSet() || node.op == RtlOp::kClobber) {
      const RtlExpr& dst = node.children[0];
      if (dst.IsReg()) {
        const int r = dst.RegNo();
        const int n = (dst.mode == RtlMachineMode::kDi || dst.mode == RtlMachineMode::kDf) ? 2 : 1;
        if (regno >= r && regno < r + n) {
          found = true;
        }
      } else if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg() &&
                 dst.children[0].RegNo() + static_cast<int>(dst.int_val) == regno) {
        found = true;
      }
    }
  });
  return found;
}

bool ExprReferencesRegInSrcOrMem(const RtlExpr& expr, int regno) {
  if (expr.op == RtlOp::kClobber) {
    return !expr.children.empty() && expr.children[0].IsMem() &&
           SubtreeReferencesHardReg(expr.children[0], regno);
  }
  if (expr.IsSet()) {
    if (SubtreeReferencesHardReg(expr.SetSrc(), regno)) {
      return true;
    }
    if (expr.SetDest().IsMem() && SubtreeReferencesHardReg(expr.SetDest(), regno)) {
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
  return SubtreeReferencesHardReg(expr, regno);
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

void CollectRegs(const RtlExpr& expr, std::vector<int>* out) {
  expr.Visit([out](const RtlExpr& node) {
    if (node.IsReg()) {
      out->push_back(node.RegNo());
    }
  });
}

std::string ExtractCallSymbolFromInsnExpr(const RtlExpr& expr) {
  const RtlExpr* search_root = &expr;
  if (expr.op == RtlOp::kSequence && !expr.children.empty() &&
      expr.children[0].op == RtlOp::kCallInsn && !expr.children[0].children.empty()) {
    search_root = &expr.children[0].children[0];
  }
  std::string sym;
  search_root->Visit([&sym](const RtlExpr& node) {
    if (sym.empty() && node.op == RtlOp::kCall && !node.children.empty() &&
        node.children[0].IsMem() && node.children[0].MemAddr().IsSymbolRef()) {
      sym = std::string(node.children[0].MemAddr().SymbolName());
    }
  });
  return sym;
}

void RewriteJumpTargetLabelRef(RtlExpr* expr, std::string_view new_label_tok) {
  expr->Mutate([new_label_tok](RtlExpr& node) {
    if (node.op == RtlOp::kLabelRef) {
      node = RtlExpr::MakeSymbolicLabelRef(std::string(new_label_tok), node.mode);
    }
  });
}

// Recomputes canonical label names (`L0`, `L1`, ...) in linear chain order and
// synchronizes `(label_ref L<k>)` across all jump instructions and tablejumps.
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

bool BlockHasOnlyUseOrClobberInsns(const RtlBasicBlock& blk) {
  bool saw_use_or_clobber = false;
  for (const std::string& s : blk.instructions) {
    auto parsed = ParsePatternAndNotes(s);
    if (!parsed.ok()) {
      return false;
    }
    if (parsed->pattern.op == RtlOp::kNote) {
      continue;
    }
    if (parsed->pattern.op != RtlOp::kUse && parsed->pattern.op != RtlOp::kClobber) {
      return false;
    }
    saw_use_or_clobber = true;
  }
  return saw_use_or_clobber;
}

bool IsEligibleForMipsUncondJumpDelaySlot(const RtlExpr& pat) {
  if (!pat.IsSet() || IsAnyJumpOrReturnTopExpr(pat)) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (src.op == RtlOp::kCall || ExprHasMem(src)) {
    return false;
  }
  bool has_special_reg = false;
  pat.Visit([&has_special_reg](const RtlExpr& node) {
    if (node.IsReg() && node.RegNo() >= 64) {
      has_special_reg = true;
    }
  });
  if (has_special_reg) {
    return false;
  }
  if (dst.IsMem()) {
    if (dst.mode != RtlMachineMode::kQi && dst.mode != RtlMachineMode::kHi &&
        dst.mode != RtlMachineMode::kSi && dst.mode != RtlMachineMode::kSf) {
      return false;
    }
    const bool valid_store_src = (src.IsReg() && src.RegNo() >= 0 && src.RegNo() < 64) ||
                                 (src.IsConstInt() && src.IntVal() == 0);
    if (!valid_store_src || dst.children.empty()) {
      return false;
    }
    const RtlExpr& addr = dst.children[0];
    if (addr.IsReg()) {
      return addr.RegNo() >= 0 && addr.RegNo() < 32;
    }
    if (addr.IsConstInt()) {
      return addr.IntVal() >= -32768 && addr.IntVal() <= 32767;
    }
    if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsReg() &&
        addr.children[1].IsConstInt()) {
      return addr.children[1].IntVal() >= -32768 && addr.children[1].IntVal() <= 32767;
    }
    return addr.op == RtlOp::kLoSum;
  }

  const int dst_reg = GetSingleSetDestRegNo(pat);
  if (dst_reg < 0 || dst_reg >= 64) {
    return false;
  }
  if (dst_reg >= 32) {
    bool touches_gpr_or_const_double = false;
    src.Visit([&touches_gpr_or_const_double](const RtlExpr& node) {
      if ((node.IsReg() && node.RegNo() >= 0 && node.RegNo() < 32) ||
          node.op == RtlOp::kConstDouble || node.op == RtlOp::kFix ||
          node.op == RtlOp::kUnsignedFix) {
        touches_gpr_or_const_double = true;
      }
    });
    return !touches_gpr_or_const_double;
  }

  const RtlMachineMode dst_mode = dst.mode;
  if (dst_mode != RtlMachineMode::kQi && dst_mode != RtlMachineMode::kHi &&
      dst_mode != RtlMachineMode::kSi && dst_mode != RtlMachineMode::kSf) {
    return false;
  }
  bool touches_fpr = false;
  src.Visit([&touches_fpr](const RtlExpr& node) {
    if (node.IsReg() && node.RegNo() >= 32 && node.RegNo() < 64) {
      touches_fpr = true;
    }
  });
  if (touches_fpr || src.op == RtlOp::kMult || src.op == RtlOp::kDiv || src.op == RtlOp::kMod ||
      src.op == RtlOp::kUdiv || src.op == RtlOp::kUmod) {
    return false;
  }
  if (src.op == RtlOp::kHigh || src.op == RtlOp::kLoSum) {
    return true;
  }
  bool has_sym_or_label = false;
  src.Visit([&has_sym_or_label](const RtlExpr& node) {
    if (node.op == RtlOp::kSymbolRef || node.op == RtlOp::kLabelRef || node.op == RtlOp::kConst ||
        node.op == RtlOp::kConstDouble) {
      has_sym_or_label = true;
    }
  });
  if (has_sym_or_label) {
    return false;
  }
  if (src.IsConstInt()) {
    const int64_t v = src.IntVal();
    return (v >= -32768 && v <= 65535) ||
           ((v & 0xffff) == 0 && v >= -2147483648LL && v <= 2147483647LL);
  }
  if (src.children.size() == 2 && (src.children[0].IsConstInt() || src.children[1].IsConstInt())) {
    if (src.children[0].IsConstInt()) {
      return false;
    }
    const int64_t v = src.children[1].IntVal();
    switch (src.op) {
      case RtlOp::kAnd:
      case RtlOp::kIor:
      case RtlOp::kXor:
        return v >= 0 && v <= 65535;
      case RtlOp::kPlus:
      case RtlOp::kLt:
      case RtlOp::kLtu:
        return v >= -32768 && v <= 32767;
      case RtlOp::kMinus:
        return v >= -32767 && v <= 32768;
      case RtlOp::kAshift:
      case RtlOp::kAshiftrt:
      case RtlOp::kLshiftrt:
        return v >= 0 && v <= 31;
      case RtlOp::kEq:
      case RtlOp::kNe:
      case RtlOp::kGt:
      case RtlOp::kGtu:
        return v == 0;
      default:
        return false;
    }
  }
  return src.op != RtlOp::kLe && src.op != RtlOp::kGe && src.op != RtlOp::kGeu;
}

void RewrapSharedUseBridgePredecessors(RtlControlFlowGraph* out_ptr) {
  RtlControlFlowGraph& out = *out_ptr;
  const auto preds = BuildPredecessors(out);
  const size_t num_blocks = out.rpo_blocks.size();
  for (size_t b = 0; b + 2 < num_blocks; ++b) {
    auto& blk = out.rpo_blocks[b];
    if (blk.instructions.empty() || blk.successors.size() != 1 || blk.successors[0] != b + 1) {
      continue;
    }
    const auto& bridge_blk = out.rpo_blocks[b + 1];
    if (!bridge_blk.has_label || bridge_blk.successors.size() != 1 ||
        bridge_blk.successors[0] != b + 2 || !BlockHasOnlyUseOrClobberInsns(bridge_blk)) {
      continue;
    }
    auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
    if (!last_parsed.ok() || IsAnyJumpOrReturnTopExpr(last_parsed->pattern)) {
      continue;
    }

    bool shared_target_slot_across_bridge = false;
    for (size_t p1 : preds[b + 1]) {
      if (p1 == b || out.rpo_blocks[p1].instructions.empty()) {
        continue;
      }
      auto p1_last = ParsePatternAndNotes(out.rpo_blocks[p1].instructions.back());
      if (!p1_last.ok() || p1_last->pattern.op != RtlOp::kSequence ||
          p1_last->pattern.children.size() != 2 ||
          p1_last->pattern.children[0].op != RtlOp::kJumpInsn ||
          !p1_last->pattern.children[1].flags.in_struct) {
        continue;
      }
      auto j1 = RtlPatternAndNotes::FromSequenceSubInsn(p1_last->pattern.children[0]);
      auto s1 = RtlPatternAndNotes::FromSequenceSubInsn(p1_last->pattern.children[1]);
      if (!j1.ok() || !s1.ok() || !IsCondJumpExpr(j1->pattern)) {
        continue;
      }
      for (size_t p2 : preds[b + 2]) {
        if (p2 == b + 1 || out.rpo_blocks[p2].instructions.empty()) {
          continue;
        }
        auto p2_last = ParsePatternAndNotes(out.rpo_blocks[p2].instructions.back());
        if (!p2_last.ok() || p2_last->pattern.op != RtlOp::kSequence ||
            p2_last->pattern.children.size() != 2 ||
            p2_last->pattern.children[0].op != RtlOp::kJumpInsn ||
            !p2_last->pattern.children[1].flags.in_struct) {
          continue;
        }
        auto j2 = RtlPatternAndNotes::FromSequenceSubInsn(p2_last->pattern.children[0]);
        auto s2 = RtlPatternAndNotes::FromSequenceSubInsn(p2_last->pattern.children[1]);
        if (j2.ok() && s2.ok() && IsCondJumpExpr(j2->pattern) && s2->pattern == s1->pattern) {
          shared_target_slot_across_bridge = true;
          break;
        }
      }
      if (shared_target_slot_across_bridge) {
        break;
      }
    }
    if (!shared_target_slot_across_bridge) {
      continue;
    }

    if (IsEligibleForMipsUncondJumpDelaySlot(last_parsed->pattern)) {
      for (RtlRegNote& note : last_parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
          note.datum = RtlExpr::MakeConstInt(0);
        }
      }
      RtlPatternAndNotes jump_pat_notes;
      jump_pat_notes.pattern =
          RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"));
      blk.instructions.back() =
          RtlExpr::MakeSequence(
              {
                  RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, jump_pat_notes),
                  RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, *last_parsed),
              })
              .ToGccPrintRtlString();
    } else {
      RtlExpr jump_expr = RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"));
      blk.instructions.push_back(jump_expr.ToGccPrintRtlString());
    }
    out.rpo_blocks[b + 1].has_label = true;
  }
}

bool BlockStartsWithUseInsn(const RtlBasicBlock& blk) {
  for (const std::string& s : blk.instructions) {
    auto parsed = ParsePatternAndNotes(s);
    if (!parsed.ok()) {
      return false;
    }
    if (parsed->pattern.op == RtlOp::kNote) {
      continue;
    }
    return parsed->pattern.op == RtlOp::kUse;
  }
  return false;
}

bool BlockHasActiveNonNoteInsn(const RtlBasicBlock& blk) {
  for (const std::string& s : blk.instructions) {
    auto parsed = ParsePatternAndNotes(s);
    if (parsed.ok() && parsed->pattern.op != RtlOp::kNote) {
      return true;
    }
  }
  return false;
}

bool CfgHasAnyReturnInsn(const RtlControlFlowGraph& cfg) {
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& s : blk.instructions) {
      auto parsed = ParsePatternAndNotes(s);
      if (parsed.ok() && IsReturnExpr(parsed->pattern)) {
        return true;
      }
    }
  }
  return false;
}

bool IsUnoptimizedO0Cfg(const RtlControlFlowGraph& cfg, bool cfg_has_any_sequence) {
  return !cfg_has_any_sequence && !CfgHasAnyReturnInsn(cfg);
}

void InvertRelaxUncondAndMergedDelaySlots(RtlControlFlowGraph* out_ptr,
                                          std::vector<bool>* flavour_4a_restored_blocks_ptr) {
  RtlControlFlowGraph& out = *out_ptr;
  std::vector<bool>& flavour_4a_restored_blocks = *flavour_4a_restored_blocks_ptr;

  // Pass 1: Invert Category 1 (non-annulled `SEQUENCE` unconditional jump to
  // `b + 1` unwrapped by `reorg.c:3939-3973`, Subcase II.5) and Category 2
  // (bare unconditional jump to `b + 1` after a non-jump `parallel` instruction
  // deleted by `reorg.c:3775`, Subcase I.1).
  {
    const auto preds = BuildPredecessors(out);
    const size_t num_blocks = out.rpo_blocks.size();
    for (size_t b = 0; b + 1 < num_blocks; ++b) {
      auto& blk = out.rpo_blocks[b];
      if (blk.successors.size() != 1 || blk.successors[0] != b + 1 || blk.instructions.empty() ||
          preds[b + 1].size() < 2) {
        continue;
      }
      auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
      if (!last_parsed.ok()) {
        continue;
      }

      // Category 2: `b` ends with a non-jump `parallel` instruction (which
      // cannot occupy a delay slot per `mips.md:125-138`) and `b - 1` ends with
      // a conditional `SEQUENCE` with `insn/s` targeting `b + 1` and falling
      // through to `b`.
      if (b > 0 && last_parsed->pattern.op == RtlOp::kParallel &&
          !IsAnyJumpOrReturnTopExpr(last_parsed->pattern) &&
          !out.rpo_blocks[b - 1].instructions.empty()) {
        const auto& prev_blk = out.rpo_blocks[b - 1];
        auto prev_last = ParsePatternAndNotes(prev_blk.instructions.back());
        if (prev_last.ok() && prev_last->pattern.op == RtlOp::kSequence &&
            prev_last->pattern.children.size() == 2 &&
            prev_last->pattern.children[0].op == RtlOp::kJumpInsn &&
            prev_last->pattern.children[1].flags.in_struct && prev_blk.successors.size() == 2 &&
            ((prev_blk.successors[0] == b + 1 && prev_blk.successors[1] == b) ||
             (prev_blk.successors[0] == b && prev_blk.successors[1] == b + 1))) {
          RtlExpr jump_expr =
              RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"));
          blk.instructions.push_back(jump_expr.ToGccPrintRtlString());
          out.rpo_blocks[b + 1].has_label = true;
          continue;
        }
      }

      // Category 1: `b` ends with a single-set instruction that was in the
      // delay slot of `(sequence [(jump_insn (set (pc) (label_ref L_next))) ...])`.
      const int last_dst_reg = GetSingleSetDestRegNo(last_parsed->pattern);
      if (last_dst_reg < 0 || IsAnyJumpOrReturnTopExpr(last_parsed->pattern)) {
        continue;
      }

      bool should_wrap_uncond_seq = false;
      if (b > 0 && !out.rpo_blocks[b - 1].instructions.empty()) {
        const auto& prev_blk = out.rpo_blocks[b - 1];
        auto prev_last = ParsePatternAndNotes(prev_blk.instructions.back());
        if (prev_last.ok() && prev_last->pattern.op == RtlOp::kSequence &&
            prev_last->pattern.children.size() == 2 &&
            prev_last->pattern.children[0].op == RtlOp::kJumpInsn &&
            !prev_last->pattern.children[0].children.empty()) {
          const RtlExpr& prev_jump_pat = prev_last->pattern.children[0].children[0];
          // Subcase 1a: `b - 1` ends with an unconditional `SEQUENCE` jump to
          // `b + 1` (e.g. switch decision tree arms or shared epilogue jumps).
          if (blk.instructions.size() == 1 && BlockStartsWithUseInsn(out.rpo_blocks[b + 1]) &&
              IsSimpleUncondJumpExpr(prev_jump_pat) && prev_blk.successors.size() == 1 &&
              prev_blk.successors[0] == b + 1 && !HasRegDeadRegNote(*last_parsed)) {
            should_wrap_uncond_seq = true;
          }
          // Subcase 1b: `b` is a 1-instruction fallthrough block of a
          // conditional `SEQUENCE` in `b - 1` with `insn/s` targeting `b + 1`,
          // setting the same destination register, where either `b`'s
          // instruction has `REG_DEAD (const_int 0)` or `b - 1` is annulled
          // (`/u`) or `b + 1` does not start with `(use ...)`.
          if (!should_wrap_uncond_seq && blk.instructions.size() == 1 && !blk.has_label &&
              IsCondJumpExpr(prev_jump_pat) && prev_last->pattern.children[1].flags.in_struct &&
              prev_blk.successors.size() == 2 &&
              ((prev_blk.successors[0] == b + 1 && prev_blk.successors[1] == b) ||
               (prev_blk.successors[0] == b && prev_blk.successors[1] == b + 1))) {
            const int prev_slot_dst =
                GetSingleSetDestRegNo(prev_last->pattern.children[1].children[0]);
            if (prev_slot_dst == last_dst_reg &&
                (HasRegDeadConstZeroNote(*last_parsed) ||
                 prev_last->pattern.children[0].flags.unchanging ||
                 (!HasRegDeadRegNote(*last_parsed) && !ExprHasMem(last_parsed->pattern) &&
                  !ExprHasMem(prev_last->pattern.children[1].children[0]) &&
                  !ExprReferencesRegInSrcOrMem(prev_last->pattern.children[1].children[0],
                                               last_dst_reg)) ||
                 (!HasRegDeadRegNote(*last_parsed) &&
                  !BlockStartsWithUseInsn(out.rpo_blocks[b + 1])))) {
              should_wrap_uncond_seq = true;
            }
          }
        }
      }

      if (should_wrap_uncond_seq) {
        RtlPatternAndNotes jump_pat_notes;
        jump_pat_notes.pattern =
            RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"));
        blk.instructions.back() =
            RtlExpr::MakeSequence(
                {
                    RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, jump_pat_notes),
                    RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, *last_parsed),
                })
                .ToGccPrintRtlString();
        out.rpo_blocks[b + 1].has_label = true;
      }
    }
  }

  // Pass 2: Invert Category 4 (`try_merge_delay_insns`, `reorg.c:4038-4043` &
  // `1775-1930`, Subcase II.7).
  {
    const auto preds = BuildPredecessors(out);
    const size_t num_blocks = out.rpo_blocks.size();
    for (size_t b = 0; b < num_blocks; ++b) {
      if (flavour_4a_restored_blocks[b]) {
        continue;
      }
      auto& blk = out.rpo_blocks[b];
      if (blk.instructions.empty() || blk.successors.size() != 2) {
        continue;
      }
      const size_t fallthrough_b = b + 1;
      size_t target_b = blk.successors[0];
      if (target_b == fallthrough_b) {
        target_b = blk.successors[1];
      }
      if (fallthrough_b >= num_blocks || target_b >= num_blocks) {
        continue;
      }

      auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
      if (!last_parsed.ok() || last_parsed->pattern.op != RtlOp::kSequence ||
          last_parsed->pattern.children.size() != 2) {
        continue;
      }
      const RtlExpr& jump_sub = last_parsed->pattern.children[0];
      const RtlExpr& slot_sub = last_parsed->pattern.children[1];
      if (jump_sub.op != RtlOp::kJumpInsn || jump_sub.flags.unchanging ||
          slot_sub.flags.in_struct || jump_sub.children.empty() || slot_sub.children.empty()) {
        continue;
      }
      auto jump_pat_notes = RtlPatternAndNotes::FromSequenceSubInsn(jump_sub);
      auto slot_pat_notes = RtlPatternAndNotes::FromSequenceSubInsn(slot_sub);
      if (!jump_pat_notes.ok() || !slot_pat_notes.ok() ||
          !IsCondJumpExpr(jump_pat_notes->pattern)) {
        continue;
      }
      const int slot_dst_reg = GetSingleSetDestRegNo(slot_pat_notes->pattern);
      if (slot_dst_reg < 0) {
        continue;
      }

      // Flavour 4a: Fallthrough block `b + 1` (`!has_label`, single pred `b`)
      // consists of a single bare conditional jump whose branch target is also
      // `target_b` (`reorg.c:1866-1884` on `next_to_match = next_active_insn(insn)`).
      auto& ft_blk = out.rpo_blocks[fallthrough_b];
      const size_t ft_target_b =
          (ft_blk.successors.size() == 2 && ft_blk.successors[0] == fallthrough_b + 1)
              ? ft_blk.successors[1]
              : (ft_blk.successors.empty() ? num_blocks : ft_blk.successors[0]);
      if (!ft_blk.has_label && preds[fallthrough_b].size() == 1 && preds[fallthrough_b][0] == b &&
          ft_blk.instructions.size() == 1 && ft_blk.successors.size() == 2 &&
          ft_target_b == target_b) {
        auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
        if (ft_parsed.ok() && IsCondJumpExpr(ft_parsed->pattern) &&
            !ExprReferencesRegInSrcOrMem(ft_parsed->pattern, slot_dst_reg)) {
          bool slot_dst_live_on_ft_fallthrough = ExprHasMem(slot_pat_notes->pattern);
          if (!slot_dst_live_on_ft_fallthrough && fallthrough_b + 1 < out.rpo_blocks.size()) {
            slot_dst_live_on_ft_fallthrough = IsAnyDefinedHardRegLiveAtBlockEntry(
                out, slot_pat_notes->pattern, fallthrough_b + 1);
          }
          RtlFlags new_b_jump_flags = jump_sub.flags;
          if (slot_dst_live_on_ft_fallthrough) {
            new_b_jump_flags.unchanging = true;
          }
          RtlFlags new_b_slot_flags = slot_sub.flags;
          new_b_slot_flags.in_struct = true;
          blk.instructions.back() =
              RtlExpr::MakeSequence(
                  {
                      RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, new_b_jump_flags, *jump_pat_notes),
                      RtlExpr::MakeInsnWrapper(RtlOp::kInsn, new_b_slot_flags, *slot_pat_notes),
                  })
                  .ToGccPrintRtlString();

          RtlFlags ft_slot_flags = slot_sub.flags;
          ft_slot_flags.in_struct = true;
          ft_blk.instructions[0] =
              RtlExpr::MakeSequence(
                  {
                      RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, *ft_parsed),
                      RtlExpr::MakeInsnWrapper(RtlOp::kInsn, ft_slot_flags, *slot_pat_notes),
                  })
                  .ToGccPrintRtlString();
          flavour_4a_restored_blocks[b] = true;
          flavour_4a_restored_blocks[fallthrough_b] = true;
          continue;
        }
      }

      // Flavour 4b: Target block `target_b` (`own_thread_p`: single pred `b`,
      // no fallthrough from `target_b - 1`) consists of a single bare
      // conditional jump (`reorg.c:4041-4043` + `1866-1884`).
      auto& tgt_blk = out.rpo_blocks[target_b];
      const bool target_is_own_thread =
          preds[target_b].size() == 1 && preds[target_b][0] == b &&
          (target_b == 0 || std::find(out.rpo_blocks[target_b - 1].successors.begin(),
                                      out.rpo_blocks[target_b - 1].successors.end(),
                                      target_b) == out.rpo_blocks[target_b - 1].successors.end());
      if (target_is_own_thread && tgt_blk.instructions.size() == 1 &&
          tgt_blk.successors.size() == 2) {
        auto tgt_parsed = ParsePatternAndNotes(tgt_blk.instructions[0]);
        if (tgt_parsed.ok() && IsCondJumpExpr(tgt_parsed->pattern) &&
            !ExprReferencesRegInSrcOrMem(tgt_parsed->pattern, slot_dst_reg)) {
          tgt_blk.instructions[0] =
              RtlExpr::MakeSequence(
                  {
                      RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, *tgt_parsed),
                      RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, *slot_pat_notes),
                  })
                  .ToGccPrintRtlString();
          flavour_4a_restored_blocks[b] = true;
          flavour_4a_restored_blocks[target_b] = true;
          continue;
        }
      }

      // Flavour 4c: Fallthrough block `b + 1` (`!has_label`, single pred `b`)
      // had a normal instruction matching `slot_pat_notes` deleted by
      // `try_merge_delay_insns` (`reorg.c:1816-1853`).
      if (!ft_blk.has_label && preds[fallthrough_b].size() == 1 && preds[fallthrough_b][0] == b &&
          !ft_blk.instructions.empty()) {
        auto ft_last = ParsePatternAndNotes(ft_blk.instructions.back());
        if (target_b == b + 2 && ft_blk.successors.size() == 1 && ft_blk.successors[0] == b + 2 &&
            ft_last.ok()) {
          bool conflict = false;
          for (const std::string& s : ft_blk.instructions) {
            auto p = ParsePatternAndNotes(s);
            if (p.ok() && (p->pattern.op == RtlOp::kSequence ||
                           ExprReferencesRegInSrcOrMem(p->pattern, slot_dst_reg) ||
                           ExprSetsOrClobbersReg(p->pattern, slot_dst_reg))) {
              conflict = true;
              break;
            }
          }
          if (!conflict && ft_last->pattern.op == RtlOp::kNote) {
            RtlFlags new_b_jump_flags = jump_sub.flags;
            new_b_jump_flags.unchanging = true;
            RtlFlags new_b_slot_flags = slot_sub.flags;
            new_b_slot_flags.in_struct = true;
            blk.instructions.back() =
                RtlExpr::MakeSequence(
                    {
                        RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, new_b_jump_flags,
                                                 *jump_pat_notes),
                        RtlExpr::MakeInsnWrapper(RtlOp::kInsn, new_b_slot_flags, *slot_pat_notes),
                    })
                    .ToGccPrintRtlString();
            ft_blk.instructions.push_back(slot_pat_notes->ToGccPrintRtlString());
            flavour_4a_restored_blocks[b] = true;
            continue;
          }
          if (!conflict && ft_last->pattern.op != RtlOp::kNote &&
              !ExprHasMem(slot_pat_notes->pattern) &&
              (ExprReferencesRegInSrcOrMem(jump_pat_notes->pattern, slot_dst_reg) ||
               (blk.has_label && blk.instructions.size() == 1))) {
            bool tgt_reads_slot_dst_first = false;
            for (const std::string& ts : tgt_blk.instructions) {
              auto tp = ParsePatternAndNotes(ts);
              if (!tp.ok()) {
                break;
              }
              if (ExprReferencesRegInSrcOrMem(tp->pattern, slot_dst_reg)) {
                tgt_reads_slot_dst_first = true;
                break;
              }
              if (ExprSetsOrClobbersReg(tp->pattern, slot_dst_reg)) {
                break;
              }
            }
            if (tgt_reads_slot_dst_first) {
              RtlFlags new_b_slot_flags = slot_sub.flags;
              new_b_slot_flags.in_struct = true;
              blk.instructions.back() =
                  RtlExpr::MakeSequence(
                      {
                          RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, jump_sub.flags,
                                                   *jump_pat_notes),
                          RtlExpr::MakeInsnWrapper(RtlOp::kInsn, new_b_slot_flags, *slot_pat_notes),
                      })
                      .ToGccPrintRtlString();
              ft_blk.instructions.push_back(slot_pat_notes->ToGccPrintRtlString());
              flavour_4a_restored_blocks[b] = true;
              continue;
            }
          }
        }

        if (target_b == b + 2 && !tgt_blk.instructions.empty()) {
          auto ft_first = ParsePatternAndNotes(ft_blk.instructions.front());
          auto tgt_first = ParsePatternAndNotes(tgt_blk.instructions.front());
          if (ft_first.ok() && tgt_first.ok() && ft_first->pattern.op == RtlOp::kSequence) {
            const std::string ft_sym = ExtractCallSymbolFromInsnExpr(ft_first->pattern);
            const std::string tgt_sym = ExtractCallSymbolFromInsnExpr(tgt_first->pattern);
            if (!ft_sym.empty() && ft_sym == tgt_sym &&
                !ExprSetsOrClobbersReg(ft_first->pattern, slot_dst_reg)) {
              RtlFlags new_b_slot_flags = slot_sub.flags;
              new_b_slot_flags.in_struct = true;
              blk.instructions.back() =
                  RtlExpr::MakeSequence(
                      {
                          RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, jump_sub.flags,
                                                   *jump_pat_notes),
                          RtlExpr::MakeInsnWrapper(RtlOp::kInsn, new_b_slot_flags, *slot_pat_notes),
                      })
                      .ToGccPrintRtlString();
              ft_blk.instructions.insert(ft_blk.instructions.begin(),
                                         slot_pat_notes->ToGccPrintRtlString());
              continue;
            }
          }
        }
      }
    }
  }
}

}  // namespace

RtlControlFlowGraph InvertRelaxDelaySlots(const RtlControlFlowGraph& dbr_relaxed_cfg_x) {
  if (!IsValidCfg(dbr_relaxed_cfg_x)) {
    return {};
  }

  RtlControlFlowGraph out = dbr_relaxed_cfg_x;
  out.pass = RtlPassKind::kDbr;

  bool cfg_has_any_sequence = false;
  for (const auto& blk : out.rpo_blocks) {
    for (const std::string& s : blk.instructions) {
      auto parsed = ParsePatternAndNotes(s);
      if (parsed.ok() && parsed->pattern.op == RtlOp::kSequence) {
        cfg_has_any_sequence = true;
        break;
      }
    }
    if (cfg_has_any_sequence) {
      break;
    }
  }

  if (IsUnoptimizedO0Cfg(out, cfg_has_any_sequence)) {
    SynchronizeCanonicalLabels(&out);
    for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
      out.rpo_blocks[b].id = static_cast<uint32_t>(b);
      out.rpo_blocks[b].SyncRtlInsnsFromInstructions();
    }
    return out;
  }

  std::vector<bool> flavour_4a_restored_blocks(out.rpo_blocks.size(), false);
  InvertRelaxUncondAndMergedDelaySlots(&out, &flavour_4a_restored_blocks);

  // Pass 3: Invert Category 3 (non-annulled `SEQUENCE` conditional branch
  // inverted around a bare unconditional jump to the end-of-function or shared
  // return epilogue label, `reorg.c:3993-4034`, Subcase II.6).
  {
    size_t b = 0;
    while (b < out.rpo_blocks.size()) {
      auto& blk = out.rpo_blocks[b];
      if (blk.instructions.empty() || blk.successors.size() != 2 ||
          b + 1 >= out.rpo_blocks.size()) {
        ++b;
        continue;
      }
      const size_t fallthrough_b = b + 1;
      size_t target_b = blk.successors[0];
      if (target_b == fallthrough_b) {
        target_b = blk.successors[1];
      }
      if (target_b == fallthrough_b || target_b >= out.rpo_blocks.size()) {
        ++b;
        continue;
      }

      auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
      if (!last_parsed.ok() || last_parsed->pattern.op != RtlOp::kSequence ||
          last_parsed->pattern.children.size() != 2) {
        ++b;
        continue;
      }
      const RtlExpr& jump_sub = last_parsed->pattern.children[0];
      const RtlExpr& slot_sub = last_parsed->pattern.children[1];
      if (jump_sub.op != RtlOp::kJumpInsn) {
        ++b;
        continue;
      }
      auto jump_pat_notes = RtlPatternAndNotes::FromSequenceSubInsn(jump_sub);
      auto slot_pat_notes = RtlPatternAndNotes::FromSequenceSubInsn(slot_sub);
      if (!jump_pat_notes.ok() || !slot_pat_notes.ok() ||
          !IsCondJumpExpr(jump_pat_notes->pattern)) {
        ++b;
        continue;
      }

      const auto& ft_blk = out.rpo_blocks[fallthrough_b];
      bool ft_only_from_b = true;
      for (size_t pb = 0; pb < out.rpo_blocks.size(); ++pb) {
        if (pb == b) {
          continue;
        }
        const auto& psuccs = out.rpo_blocks[pb].successors;
        if (std::find(psuccs.begin(), psuccs.end(), fallthrough_b) != psuccs.end()) {
          ft_only_from_b = false;
          break;
        }
      }
      bool ft_has_note_only_bridge = false;
      if ((!ft_blk.has_label || ft_only_from_b) && !ft_blk.instructions.empty() &&
          !BlockHasActiveNonNoteInsn(ft_blk) && ft_blk.successors.size() == 1 &&
          ft_blk.successors[0] == fallthrough_b + 1 && fallthrough_b + 1 < out.rpo_blocks.size() &&
          BlockHasActiveNonNoteInsn(out.rpo_blocks[fallthrough_b + 1])) {
        bool next_has_label_or_jump_pred = out.rpo_blocks[fallthrough_b + 1].has_label;
        for (size_t pb = 0; !next_has_label_or_jump_pred && pb < out.rpo_blocks.size(); ++pb) {
          if (pb == fallthrough_b) {
            continue;
          }
          const auto& psuccs = out.rpo_blocks[pb].successors;
          if (std::find(psuccs.begin(), psuccs.end(), fallthrough_b + 1) != psuccs.end()) {
            next_has_label_or_jump_pred = true;
          }
        }
        if (next_has_label_or_jump_pred) {
          ft_has_note_only_bridge = true;
        }
      }
      if (!ft_has_note_only_bridge && (ft_blk.has_label || !BlockHasActiveNonNoteInsn(ft_blk))) {
        ++b;
        continue;
      }
      const size_t eff_ft_b = ft_has_note_only_bridge ? (fallthrough_b + 1) : fallthrough_b;
      const auto& eff_ft_blk = out.rpo_blocks[eff_ft_b];

      // Check whether `target_b` is either:
      // (a) the trailing empty `end_of_function_label` block targeted by the
      //     deleted bare unconditional jump `next`, OR
      // (b) the shared return epilogue block starting with `(use ...)` where
      //     `ft_blk` does not end with an unconditional jump/`SEQUENCE` (and if
      //     `ft_blk` ends with a conditional `SEQUENCE`, `ft_blk` starts with
      //     loop notes).
      if (jump_sub.flags.unchanging) {
        ++b;
        continue;
      }
      bool pred_of_target_starts_with_same_slot = false;
      if (target_b > 0) {
        const auto& pred_of_target = out.rpo_blocks[target_b - 1];
        const bool pred_falls_through =
            std::find(pred_of_target.successors.begin(), pred_of_target.successors.end(),
                      target_b) != pred_of_target.successors.end();
        if (pred_falls_through) {
          for (const std::string& s : pred_of_target.instructions) {
            auto p = ParsePatternAndNotes(s);
            if (p.ok() && p->pattern.op != RtlOp::kNote && p->pattern.op != RtlOp::kUse &&
                p->pattern.op != RtlOp::kClobber) {
              if (p->pattern == slot_pat_notes->pattern) {
                pred_of_target_starts_with_same_slot = true;
              }
              break;
            }
          }
        }
      }
      bool pred_of_target_ends_with_use = false;
      if (target_b > 0 && out.rpo_blocks[target_b - 1].instructions.size() >= 2 &&
          std::find(out.rpo_blocks[target_b - 1].successors.begin(),
                    out.rpo_blocks[target_b - 1].successors.end(),
                    target_b) != out.rpo_blocks[target_b - 1].successors.end()) {
        auto pred_last = ParsePatternAndNotes(out.rpo_blocks[target_b - 1].instructions.back());
        if (pred_last.ok() &&
            (pred_last->pattern.op == RtlOp::kUse || pred_last->pattern.op == RtlOp::kClobber)) {
          pred_of_target_ends_with_use = true;
        }
      }
      bool only_b_and_prev_jump_to_target = target_b > 0;
      for (size_t pb = 0; only_b_and_prev_jump_to_target && pb < out.rpo_blocks.size(); ++pb) {
        const auto& pblk = out.rpo_blocks[pb];
        if (std::find(pblk.addr_vec_targets.begin(), pblk.addr_vec_targets.end(), target_b) !=
            pblk.addr_vec_targets.end()) {
          only_b_and_prev_jump_to_target = false;
          break;
        }
        if (pb == b || pb + 1 == target_b) {
          continue;
        }
        if (std::find(pblk.successors.begin(), pblk.successors.end(), target_b) !=
            pblk.successors.end()) {
          only_b_and_prev_jump_to_target = false;
          break;
        }
      }
      const bool is_flavour_4a_restored =
          b < flavour_4a_restored_blocks.size() && flavour_4a_restored_blocks[b];
      const bool target_is_trailing_empty_return_label =
          !is_flavour_4a_restored && slot_sub.flags.in_struct && !ft_has_note_only_bridge &&
          (target_b + 1 == out.rpo_blocks.size() && out.rpo_blocks[target_b].instructions.empty() &&
           (out.rpo_blocks[fallthrough_b].successors.size() == 1 ||
            (out.rpo_blocks[fallthrough_b].successors.size() == 2 &&
             pred_of_target_ends_with_use)) &&
           !pred_of_target_starts_with_same_slot);
      const bool target_is_return_after_use =
          target_b > 0 && out.rpo_blocks[target_b].successors.empty() &&
          out.rpo_blocks[target_b].instructions.size() == 1 && pred_of_target_ends_with_use &&
          only_b_and_prev_jump_to_target && [&]() {
            auto ft_l = ParsePatternAndNotes(eff_ft_blk.instructions.back());
            auto tp = ParsePatternAndNotes(out.rpo_blocks[target_b].instructions[0]);
            return ft_l.ok() && !IsUncondJumpOrReturnOrTableJumpTopExpr(ft_l.value().pattern) &&
                   tp.ok() && IsReturnExpr(tp->pattern);
          }();
      bool target_is_shared_epilogue_jump = false;
      if (!is_flavour_4a_restored && slot_sub.flags.in_struct &&
          !target_is_trailing_empty_return_label &&
          (BlockStartsWithUseInsn(out.rpo_blocks[target_b]) || target_is_return_after_use)) {
        auto ft_last = ParsePatternAndNotes(eff_ft_blk.instructions.back());
        auto ft_first = ParsePatternAndNotes(eff_ft_blk.instructions.front());
        if (ft_last.ok() && ft_first.ok()) {
          if (!IsAnyJumpOrReturnTopExpr(ft_last->pattern)) {
            target_is_shared_epilogue_jump = true;
          } else if ((ft_has_note_only_bridge || ft_first->pattern.op == RtlOp::kNote) &&
                     !IsUncondJumpOrReturnOrTableJumpTopExpr(ft_last->pattern)) {
            target_is_shared_epilogue_jump = true;
          } else if (HasRegDeadConstZeroNote(*slot_pat_notes) &&
                     !IsUncondJumpOrReturnOrTableJumpTopExpr(ft_last->pattern)) {
            target_is_shared_epilogue_jump = true;
          } else if (target_b > 0 && target_b - 1 != b && !pred_of_target_starts_with_same_slot &&
                     std::find(out.rpo_blocks[target_b - 1].successors.begin(),
                               out.rpo_blocks[target_b - 1].successors.end(),
                               target_b) != out.rpo_blocks[target_b - 1].successors.end() &&
                     !out.rpo_blocks[target_b - 1].instructions.empty()) {
            auto pred_last_parsed =
                ParsePatternAndNotes(out.rpo_blocks[target_b - 1].instructions.back());
            if (pred_last_parsed.ok() && !IsAnyJumpOrReturnTopExpr(pred_last_parsed->pattern) &&
                pred_last_parsed->pattern != slot_pat_notes->pattern) {
              bool other_jumper_has_same_target_slot = false;
              const bool prev_last_has_reg_dead_reg_note = HasRegDeadRegNote(*pred_last_parsed);
              const bool target_is_use_bridge =
                  !prev_last_has_reg_dead_reg_note &&
                  !IsUncondJumpOrReturnOrTableJumpTopExpr(ft_last->pattern) &&
                  out.rpo_blocks[target_b].successors.size() == 1 &&
                  out.rpo_blocks[target_b].successors[0] == target_b + 1;
              if (!IsUncondJumpOrReturnOrTableJumpTopExpr(ft_last->pattern)) {
                for (size_t ob = 0; ob < out.rpo_blocks.size(); ++ob) {
                  if (ob == b || out.rpo_blocks[ob].instructions.empty()) {
                    continue;
                  }
                  const auto& oblk = out.rpo_blocks[ob];
                  bool jumps_to_shared_ret = false;
                  for (size_t s : oblk.successors) {
                    if (s != ob + 1 &&
                        (s == target_b || (target_is_use_bridge && s == target_b + 1))) {
                      jumps_to_shared_ret = true;
                      break;
                    }
                  }
                  if (!jumps_to_shared_ret) {
                    continue;
                  }
                  auto ol = ParsePatternAndNotes(oblk.instructions.back());
                  if (ol.ok() && ol->pattern.op == RtlOp::kSequence &&
                      ol->pattern.children.size() == 2 &&
                      ol->pattern.children[0].op == RtlOp::kJumpInsn &&
                      ol->pattern.children[1].flags.in_struct) {
                    auto os = RtlPatternAndNotes::FromSequenceSubInsn(ol->pattern.children[1]);
                    if (os.ok() && os->pattern == slot_pat_notes->pattern) {
                      other_jumper_has_same_target_slot = true;
                      break;
                    }
                  }
                }
              }
              if (!other_jumper_has_same_target_slot) {
                target_is_shared_epilogue_jump = true;
              }
            }
          }
        }
      } else if (!is_flavour_4a_restored && !ft_has_note_only_bridge &&
                 !jump_sub.flags.unchanging && !pred_of_target_starts_with_same_slot &&
                 ft_blk.successors.size() == 2 && !ft_blk.instructions.empty() &&
                 (out.rpo_blocks[target_b].successors.empty() ||
                  (slot_sub.flags.in_struct && target_b == b + 2 && !ft_blk.has_label))) {
        const int slot_dst_reg = GetSingleSetDestRegNo(slot_pat_notes->pattern);
        const bool is_adjacent_in_struct =
            slot_sub.flags.in_struct && target_b == b + 2 && !ft_blk.has_label;
        const bool is_return_cond_ref =
            out.rpo_blocks[target_b].successors.empty() &&
            ExprReferencesRegInSrcOrMem(jump_pat_notes->pattern, slot_dst_reg);
        if (slot_dst_reg > 0 && !ExprHasMem(slot_pat_notes->pattern) &&
            (is_adjacent_in_struct || is_return_cond_ref)) {
          bool blk_defines_slot_reg_earlier = is_adjacent_in_struct && !is_return_cond_ref;
          for (size_t i = 0; !blk_defines_slot_reg_earlier && i + 1 < blk.instructions.size();
               ++i) {
            auto bp = ParsePatternAndNotes(blk.instructions[i]);
            if (bp.ok() && ExprSetsOrClobbersReg(bp->pattern, slot_dst_reg)) {
              blk_defines_slot_reg_earlier = true;
              break;
            }
          }
          bool tgt_reads_slot_reg_first = is_return_cond_ref;
          if (!tgt_reads_slot_reg_first) {
            for (const std::string& ts : out.rpo_blocks[target_b].instructions) {
              auto tp = ParsePatternAndNotes(ts);
              if (!tp.ok()) {
                break;
              }
              if (ExprReferencesRegInSrcOrMem(tp->pattern, slot_dst_reg)) {
                tgt_reads_slot_reg_first = true;
                break;
              }
              if (ExprSetsOrClobbersReg(tp->pattern, slot_dst_reg)) {
                break;
              }
            }
          }
          bool ft_prefix_clean = blk_defines_slot_reg_earlier && tgt_reads_slot_reg_first;
          for (size_t i = 0; ft_prefix_clean && i + 1 < ft_blk.instructions.size(); ++i) {
            auto fp = ParsePatternAndNotes(ft_blk.instructions[i]);
            if (!fp.ok() || IsAnyJumpOrReturnTopExpr(fp->pattern) ||
                !ExtractCallSymbolFromInsnExpr(fp->pattern).empty() ||
                ExprReferencesRegInSrcOrMem(fp->pattern, slot_dst_reg) ||
                ExprSetsOrClobbersReg(fp->pattern, slot_dst_reg)) {
              ft_prefix_clean = false;
            }
          }
          if (ft_prefix_clean) {
            auto ft_last = ParsePatternAndNotes(ft_blk.instructions.back());
            if (ft_last.ok() && ft_last->pattern.op == RtlOp::kSequence &&
                ft_last->pattern.children.size() == 2 &&
                ft_last->pattern.children[0].op == RtlOp::kJumpInsn &&
                !ft_last->pattern.children[0].flags.unchanging) {
              auto ft_jump = RtlPatternAndNotes::FromSequenceSubInsn(ft_last->pattern.children[0]);
              auto ft_slot = RtlPatternAndNotes::FromSequenceSubInsn(ft_last->pattern.children[1]);
              if (ft_jump.ok() && ft_slot.ok() && IsCondJumpExpr(ft_jump->pattern) &&
                  !ExprReferencesRegInSrcOrMem(ft_jump->pattern, slot_dst_reg) &&
                  !ExprReferencesRegInSrcOrMem(ft_slot->pattern, slot_dst_reg) &&
                  ExprSetsOrClobbersReg(ft_slot->pattern, slot_dst_reg)) {
                target_is_shared_epilogue_jump = true;
              }
            }
          }
        }
      }
      if (!target_is_trailing_empty_return_label && !target_is_shared_epilogue_jump) {
        ++b;
        continue;
      }

      if (!TryInvertCondJumpExpr(&jump_pat_notes->pattern)) {
        ++b;
        continue;
      }

      RtlFlags new_slot_flags = slot_sub.flags;
      new_slot_flags.in_struct = false;
      blk.instructions.back() =
          RtlExpr::MakeSequence(
              {
                  RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, jump_sub.flags, *jump_pat_notes),
                  RtlExpr::MakeInsnWrapper(RtlOp::kInsn, new_slot_flags, *slot_pat_notes),
              })
              .ToGccPrintRtlString();

      // Check if `ft_blk` starts with leading `(note ...)` instructions before
      // a conditional `SEQUENCE`, where deleting the bare unconditional jump in
      // `reorg.c:3993-4034` merged those notes onto the front of `ft_blk`
      // ahead of the original fallthrough `code_label`.
      size_t leading_notes = 0;
      if (!target_is_trailing_empty_return_label) {
        while (leading_notes < out.rpo_blocks[fallthrough_b].instructions.size()) {
          auto p = ParsePatternAndNotes(out.rpo_blocks[fallthrough_b].instructions[leading_notes]);
          if (p.ok() && p->pattern.op == RtlOp::kNote) {
            ++leading_notes;
          } else {
            break;
          }
        }
      }
      auto ft_last_check = ParsePatternAndNotes(out.rpo_blocks[fallthrough_b].instructions.back());
      const bool split_leading_notes =
          (!ft_has_note_only_bridge && !target_is_trailing_empty_return_label &&
           leading_notes >= 2 &&
           leading_notes < out.rpo_blocks[fallthrough_b].instructions.size() &&
           ft_last_check.ok() && ft_last_check->pattern.op == RtlOp::kSequence &&
           IsCondJumpExpr(ft_last_check->pattern.children[0].children[0]));

      out.rpo_blocks[target_b].has_label = true;
      if (!ft_has_note_only_bridge) {
        out.rpo_blocks[fallthrough_b].has_label = true;
      }

      RtlBasicBlock tramp_blk;
      tramp_blk.id = static_cast<uint32_t>(b + 1);
      tramp_blk.has_label = false;
      tramp_blk.instructions = {
          RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"))
              .ToGccPrintRtlString()};
      const size_t shifted_target_b = (target_b >= b + 1) ? (target_b + 1) : target_b;
      tramp_blk.successors = {shifted_target_b};
      InsertBlockAt(&out, b + 1, std::move(tramp_blk));
      flavour_4a_restored_blocks.insert(
          flavour_4a_restored_blocks.begin() + static_cast<ptrdiff_t>(b + 1), false);
      if (ft_has_note_only_bridge) {
        out.rpo_blocks[b + 2].has_label = false;
        out.rpo_blocks[b + 3].has_label = true;
        out.rpo_blocks[b].successors = {b + 3, b + 1};
        b += 3;
        continue;
      }
      out.rpo_blocks[b].successors = {b + 2, b + 1};

      if (split_leading_notes) {
        // Split `b + 2` into an unlabeled note block at `b + 2` falling through
        // to the labeled `SEQUENCE` block at `b + 3`, and point `b` to `b + 3`.
        std::vector<std::string> note_insns(
            out.rpo_blocks[b + 2].instructions.begin(),
            out.rpo_blocks[b + 2].instructions.begin() + static_cast<ptrdiff_t>(leading_notes));
        out.rpo_blocks[b + 2].instructions.erase(
            out.rpo_blocks[b + 2].instructions.begin(),
            out.rpo_blocks[b + 2].instructions.begin() + static_cast<ptrdiff_t>(leading_notes));
        RtlBasicBlock note_blk;
        note_blk.id = static_cast<uint32_t>(b + 2);
        note_blk.has_label = false;
        note_blk.instructions = std::move(note_insns);
        note_blk.successors = {b + 3};
        InsertBlockAt(&out, b + 2, std::move(note_blk));
        flavour_4a_restored_blocks.insert(
            flavour_4a_restored_blocks.begin() + static_cast<ptrdiff_t>(b + 2), false);
        out.rpo_blocks[b + 3].has_label = true;
        out.rpo_blocks[b].successors = {b + 3, b + 1};
        b += 3;
        continue;
      }

      b += 2;
    }
  }

  // Pass 4: Invert Category 5 (`reorg.c:3829-3851`, Subcase I.3: bare
  // conditional jump in `b` followed by bare unconditional jump in `b + 1`
  // where `mostly_true_jump` caused `relax_delay_slots` to invert `b` and swap
  // targets with `b + 1`).
  {
    const auto preds = BuildPredecessors(out);
    const size_t num_blocks = out.rpo_blocks.size();
    for (size_t b = 0; b + 1 < num_blocks; ++b) {
      auto& blk = out.rpo_blocks[b];
      auto& next_blk = out.rpo_blocks[b + 1];
      if (blk.instructions.empty() || blk.successors.size() != 2 || next_blk.has_label ||
          preds[b + 1].size() != 1 || preds[b + 1][0] != b || next_blk.instructions.size() != 1 ||
          next_blk.successors.size() != 1) {
        continue;
      }
      const size_t t1 = (blk.successors[0] == b + 1) ? blk.successors[1] : blk.successors[0];
      const size_t t2 = next_blk.successors[0];
      if (t1 == t2 || t1 >= num_blocks || t2 >= num_blocks) {
        continue;
      }
      auto b_last = ParsePatternAndNotes(blk.instructions.back());
      auto next_only = ParsePatternAndNotes(next_blk.instructions[0]);
      if (!b_last.ok() || !next_only.ok() || !IsCondJumpExpr(b_last->pattern) ||
          !IsSimpleUncondJumpExpr(next_only->pattern)) {
        continue;
      }
      RtlPatternAndNotes candidate_b_last = *b_last;
      if (!TryInvertCondJumpExpr(&candidate_b_last.pattern)) {
        continue;
      }
      // In `reorg.c:3834`, `relax_delay_slots` inverted `b` and swapped targets
      // with `b + 1` because `mostly_true_jump > 0`.
      if (GetMostlyTrueJumpScore(b_last->pattern, b, t1) > 0 ||
          GetMostlyTrueJumpScore(candidate_b_last.pattern, b, t2) > 0) {
        blk.instructions.back() = candidate_b_last.ToGccPrintRtlString();
        blk.successors = {t2, b + 1};
        next_blk.successors = {t1};
        out.rpo_blocks[t2].has_label = true;
        out.rpo_blocks[t1].has_label = true;
      }
    }
  }

  // Pass 5: Additional relaxation subcases in `reorg.c`:
  // 7b: Un-thread conditional jumps through a labeled trampoline block
  //     (`reorg.c:3768-3782` & `3883-3898`, `follow_jumps`).
  {
    const auto preds = BuildPredecessors(out);
    const size_t num_blocks = out.rpo_blocks.size();
    for (size_t tramp_b = 1; tramp_b < num_blocks; ++tramp_b) {
      const auto& tramp_blk = out.rpo_blocks[tramp_b];
      if (tramp_blk.has_label || preds[tramp_b].size() != 1 || preds[tramp_b][0] != tramp_b - 1 ||
          tramp_blk.instructions.size() != 1 || tramp_blk.successors.size() != 1) {
        continue;
      }
      const size_t final_b = tramp_blk.successors[0];
      if (final_b <= tramp_b + 1 || final_b >= num_blocks) {
        continue;
      }
      auto tramp_insn = ParsePatternAndNotes(tramp_blk.instructions[0]);
      if (!tramp_insn.ok() || !IsSimpleUncondJumpExpr(tramp_insn->pattern)) {
        continue;
      }
      const auto& prev_blk = out.rpo_blocks[tramp_b - 1];
      if (prev_blk.instructions.empty() || prev_blk.successors.size() != 2) {
        continue;
      }
      auto prev_last = ParsePatternAndNotes(prev_blk.instructions.back());
      if (!prev_last.ok() || prev_last->pattern.op != RtlOp::kSequence ||
          prev_last->pattern.children.size() != 2 ||
          prev_last->pattern.children[0].op != RtlOp::kJumpInsn ||
          !prev_last->pattern.children[0].flags.unchanging ||
          prev_last->pattern.children[0].children.empty() ||
          !IsCondJumpExpr(prev_last->pattern.children[0].children[0])) {
        continue;
      }
      for (size_t b = 0; b + 1 < tramp_b; ++b) {
        auto& blk = out.rpo_blocks[b];
        if (blk.instructions.empty() || blk.successors.size() != 2) {
          continue;
        }
        auto b_last = ParsePatternAndNotes(blk.instructions.back());
        if (!b_last.ok() || b_last->pattern.op != RtlOp::kSequence ||
            b_last->pattern.children.size() != 2 ||
            b_last->pattern.children[0].op != RtlOp::kJumpInsn ||
            b_last->pattern.children[0].flags.unchanging ||
            b_last->pattern.children[0].children.empty() ||
            !IsCondJumpExpr(b_last->pattern.children[0].children[0])) {
          continue;
        }
        for (size_t& succ : blk.successors) {
          if (succ == final_b && succ != b + 1) {
            succ = tramp_b;
            out.rpo_blocks[tramp_b].has_label = true;
          }
        }
      }
    }
  }

  // 7c: Target-block redundant instruction re-vectoring
  // (`reorg.c:3902-3914`, `redundant_insn(trial, insn, 0)`).
  {
    const auto preds = BuildPredecessors(out);
    for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
      auto& blk = out.rpo_blocks[b];
      if (blk.instructions.size() < 2 || blk.successors.size() != 2) {
        continue;
      }
      const size_t target_b = (blk.successors[0] == b + 1) ? blk.successors[1] : blk.successors[0];
      if (target_b == 0 || target_b >= out.rpo_blocks.size()) {
        continue;
      }
      auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
      if (!last_parsed.ok() || last_parsed->pattern.op != RtlOp::kSequence ||
          last_parsed->pattern.children.size() != 2 ||
          last_parsed->pattern.children[0].op != RtlOp::kJumpInsn ||
          last_parsed->pattern.children[0].children.empty() ||
          last_parsed->pattern.children[1].children.empty()) {
        continue;
      }

      if (target_b == b + 2 && IsCondJumpExpr(last_parsed->pattern.children[0].children[0])) {
        auto& ft_blk = out.rpo_blocks[b + 1];
        auto& tgt_blk = out.rpo_blocks[target_b];
        const size_t min_ft_insns = last_parsed->pattern.children[0].flags.unchanging ? 1 : 2;
        if (!ft_blk.has_label && preds[b + 1].size() == 1 && preds[b + 1][0] == b &&
            ft_blk.instructions.size() >= min_ft_insns && ft_blk.successors.size() == 1 &&
            ft_blk.successors[0] == target_b && tgt_blk.has_label && preds[target_b].size() == 2 &&
            !tgt_blk.instructions.empty() && !BlockStartsWithUseInsn(tgt_blk)) {
          auto tgt_first = ParsePatternAndNotes(tgt_blk.instructions.front());
          auto cand_parsed = ParsePatternAndNotes(ft_blk.instructions.back());
          const RtlOp cand_src_op = (cand_parsed.ok() && cand_parsed->pattern.IsSet())
                                        ? cand_parsed->pattern.SetSrc().op
                                        : RtlOp::kUnknown;
          const bool cand_is_const_src =
              cand_src_op == RtlOp::kConstInt || cand_src_op == RtlOp::kConstDouble ||
              cand_src_op == RtlOp::kConst || cand_src_op == RtlOp::kSymbolRef ||
              cand_src_op == RtlOp::kLabelRef;
          if (tgt_first.ok() && tgt_first->pattern.op != RtlOp::kNote && cand_parsed.ok() &&
              cand_parsed->pattern.IsSet() && !ExprHasMem(cand_parsed->pattern) &&
              cand_is_const_src) {
            const int cand_dst = GetSingleSetDestRegNo(cand_parsed->pattern);
            if (cand_dst >= 0 &&
                !ExprReferencesRegInSrcOrMem(last_parsed->pattern.children[0].children[0],
                                             cand_dst) &&
                !ExprSetsOrClobbersReg(last_parsed->pattern.children[1].children[0], cand_dst)) {
              bool ft_conflict = false;
              for (size_t fi = 0; fi + 1 < ft_blk.instructions.size(); ++fi) {
                auto fp = ParsePatternAndNotes(ft_blk.instructions[fi]);
                if (!fp.ok() || ExprReferencesRegInSrcOrMem(fp->pattern, cand_dst) ||
                    ExprSetsOrClobbersReg(fp->pattern, cand_dst)) {
                  ft_conflict = true;
                  break;
                }
              }
              bool tgt_reads_cand_dst = false;
              if (!ft_conflict) {
                for (const std::string& t_insn : tgt_blk.instructions) {
                  auto tp = ParsePatternAndNotes(t_insn);
                  if (!tp.ok()) {
                    break;
                  }
                  if (ExprReferencesRegInSrcOrMem(tp->pattern, cand_dst)) {
                    tgt_reads_cand_dst = true;
                    break;
                  }
                  if (ExprSetsOrClobbersReg(tp->pattern, cand_dst)) {
                    break;
                  }
                }
              }
              if (!ft_conflict && tgt_reads_cand_dst) {
                std::vector<int> cand_regs;
                CollectRegs(cand_parsed->pattern, &cand_regs);
                bool matched_without_conflict = false;
                for (size_t i = blk.instructions.size() - 1; i > 0;) {
                  --i;
                  auto prev_p = ParsePatternAndNotes(blk.instructions[i]);
                  if (!prev_p.ok()) {
                    break;
                  }
                  if (prev_p->pattern == cand_parsed->pattern) {
                    bool conflict = false;
                    for (size_t k = i + 1; k < blk.instructions.size(); ++k) {
                      auto kp = ParsePatternAndNotes(blk.instructions[k]);
                      if (!kp.ok()) {
                        conflict = true;
                        break;
                      }
                      for (int r : cand_regs) {
                        if (ExprSetsOrClobbersReg(kp->pattern, r)) {
                          conflict = true;
                          break;
                        }
                      }
                    }
                    if (!conflict) {
                      matched_without_conflict = true;
                    }
                    break;
                  }
                }
                if (matched_without_conflict) {
                  std::string moved_insn = std::move(ft_blk.instructions.back());
                  ft_blk.instructions.pop_back();
                  tgt_blk.instructions.insert(tgt_blk.instructions.begin(), std::move(moved_insn));
                  if (ft_blk.instructions.empty()) {
                    out.rpo_blocks.erase(out.rpo_blocks.begin() + static_cast<ptrdiff_t>(b + 1));
                    for (size_t rb = 0; rb < out.rpo_blocks.size(); ++rb) {
                      out.rpo_blocks[rb].id = static_cast<uint32_t>(rb);
                      for (size_t& s : out.rpo_blocks[rb].successors) {
                        if (s > b + 1) {
                          --s;
                        }
                      }
                      for (size_t& t : out.rpo_blocks[rb].addr_vec_targets) {
                        if (t > b + 1) {
                          --t;
                        }
                      }
                    }
                    out.rpo_blocks[b].successors = {b + 1, b + 1};
                  }
                  continue;
                }
              }
            }
          }
        }
      }

      if (last_parsed->pattern.children[0].flags.unchanging) {
        continue;
      }

      const size_t cand_b = target_b - 1;
      if (cand_b <= b + 1) {
        continue;
      }
      const auto& cand_blk = out.rpo_blocks[cand_b];
      if (!cand_blk.has_label || preds[cand_b].size() != 1 || preds[cand_b][0] == b ||
          preds[cand_b][0] == cand_b - 1 || cand_blk.instructions.size() != 1 ||
          cand_blk.successors.size() != 1 || cand_blk.successors[0] != target_b) {
        continue;
      }
      auto cand_parsed = ParsePatternAndNotes(cand_blk.instructions[0]);
      if (!cand_parsed.ok() || !cand_parsed->pattern.IsSet() || ExprHasMem(cand_parsed->pattern)) {
        continue;
      }
      const int cand_dst = GetSingleSetDestRegNo(cand_parsed->pattern);
      if (cand_dst < 0 ||
          ExprReferencesRegInSrcOrMem(last_parsed->pattern.children[0].children[0], cand_dst)) {
        continue;
      }
      std::vector<int> cand_regs;
      CollectRegs(cand_parsed->pattern, &cand_regs);

      bool matched_without_conflict = false;
      for (size_t i = blk.instructions.size() - 1; i > 0;) {
        --i;
        auto prev_p = ParsePatternAndNotes(blk.instructions[i]);
        if (!prev_p.ok()) {
          break;
        }
        if (prev_p->pattern == cand_parsed->pattern) {
          // Verify no instruction between `i + 1` and `blk.instructions.back()`
          // (including the delay slot) modifies any register in `cand_regs`.
          bool conflict = false;
          for (size_t k = i + 1; k < blk.instructions.size(); ++k) {
            auto kp = ParsePatternAndNotes(blk.instructions[k]);
            if (!kp.ok()) {
              conflict = true;
              break;
            }
            for (int r : cand_regs) {
              if (ExprSetsOrClobbersReg(kp->pattern, r)) {
                conflict = true;
                break;
              }
            }
          }
          if (!conflict) {
            matched_without_conflict = true;
          }
          break;
        }
      }
      if (matched_without_conflict) {
        for (size_t& succ : blk.successors) {
          if (succ == target_b) {
            succ = cand_b;
          }
        }
      }
    }
  }

  RewrapSharedUseBridgePredecessors(&out);

  // 7a: Standalone bare conditional branch inverted around a bare unconditional
  // jump (`reorg.c:3787-3818`, Subcase I.2) when the input CFG has no
  // `SEQUENCE` wrappers.
  if (!cfg_has_any_sequence) {
    for (size_t b = 0; b + 1 < out.rpo_blocks.size(); ++b) {
      auto& blk = out.rpo_blocks[b];
      if (blk.instructions.empty() || blk.successors.size() != 2) {
        continue;
      }
      const size_t fallthrough_b = b + 1;
      const size_t target_b =
          (blk.successors[0] == fallthrough_b) ? blk.successors[1] : blk.successors[0];
      if (target_b <= b || target_b == fallthrough_b || target_b >= out.rpo_blocks.size()) {
        continue;
      }
      auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
      if (!last_parsed.ok() || !IsCondJumpExpr(last_parsed->pattern)) {
        continue;
      }
      // Skip if `b + 1` was already handled by Category 5 (bare unconditional
      // jump).
      if (out.rpo_blocks[fallthrough_b].instructions.size() == 1) {
        auto ft_only = ParsePatternAndNotes(out.rpo_blocks[fallthrough_b].instructions[0]);
        if (ft_only.ok() && IsSimpleUncondJumpExpr(ft_only->pattern)) {
          continue;
        }
      }
      const bool target_is_trailing_empty =
          (target_b + 1 == out.rpo_blocks.size() && out.rpo_blocks[target_b].instructions.empty());
      if (!target_is_trailing_empty && !out.rpo_blocks[fallthrough_b].successors.empty()) {
        continue;
      }
      if (!TryInvertCondJumpExpr(&last_parsed->pattern)) {
        continue;
      }
      blk.instructions.back() = last_parsed->ToGccPrintRtlString();
      out.rpo_blocks[fallthrough_b].has_label = true;

      RtlBasicBlock tramp_blk;
      tramp_blk.id = static_cast<uint32_t>(b + 1);
      tramp_blk.has_label = false;
      out.rpo_blocks[target_b].has_label = true;
      tramp_blk.instructions = {
          RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"))
              .ToGccPrintRtlString()};
      const size_t shifted_target_b = (target_b >= b + 1) ? (target_b + 1) : target_b;
      tramp_blk.successors = {shifted_target_b};
      InsertBlockAt(&out, b + 1, std::move(tramp_blk));
      out.rpo_blocks[b].successors = {b + 2, b + 1};
      break;
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
