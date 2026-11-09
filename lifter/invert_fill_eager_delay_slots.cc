#include "lifter/invert_fill_eager_delay_slots.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

bool IsValidRtlExprSubtree(const RtlExpr& root) {
  bool valid = true;
  root.Visit([&valid](const RtlExpr& node) {
    if (!valid) {
      return;
    }
    if (node.op == RtlOp::kUnknown) {
      valid = false;
      return;
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
  if (expr.op == RtlOp::kReturn) {
    return true;
  }
  if (expr.op == RtlOp::kParallel && !expr.children.empty() &&
      expr.children[0].op == RtlOp::kReturn) {
    return true;
  }
  return false;
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

std::string FormatFlagsSuffix(const RtlFlags& flags) {
  std::string out;
  if (flags.in_struct) out.append("/s");
  if (flags.volatil) out.append("/v");
  if (flags.unchanging) out.append("/u");
  if (flags.integrated) out.append("/i");
  if (flags.frame_related) out.append("/f");
  return out;
}

absl::StatusOr<RtlPatternAndNotes> ExtractSequenceSubInsn(const RtlExpr& sub_insn) {
  if (sub_insn.children.empty()) {
    return absl::InvalidArgumentError("Empty SEQUENCE sub-insn");
  }
  std::string combined = sub_insn.children[0].ToSexprString();
  for (size_t i = 1; i < sub_insn.children.size(); ++i) {
    absl::StrAppend(&combined, " ", sub_insn.children[i].ToSexprString());
  }
  return ParsePatternAndNotes(combined);
}

std::string FormatSequenceString(RtlOp head_op, const RtlFlags& head_flags,
                                 const RtlPatternAndNotes& head_pat, const RtlFlags& slot_flags,
                                 const RtlPatternAndNotes& slot_pat) {
  return absl::StrCat("(", RtlExpr::OpToString(RtlOp::kSequence), " [(",
                      RtlExpr::OpToString(head_op), FormatFlagsSuffix(head_flags), " ",
                      head_pat.ToSexprString(), ") (", RtlExpr::OpToString(RtlOp::kInsn),
                      FormatFlagsSuffix(slot_flags), " ", slot_pat.ToSexprString(), ")])");
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
        if (IsTableJumpExpr(parsed->pattern)) {
          block_has_tablejump[b] = true;
          const RtlExpr& lbl_ref = parsed->pattern.children[1].children[0];
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
            auto sub_or = ExtractSequenceSubInsn(parsed->pattern.children[0]);
            auto slot_or = ExtractSequenceSubInsn(parsed->pattern.children[1]);
            if (sub_or.ok() && slot_or.ok()) {
              RewriteJumpTargetLabelRef(&sub_or->pattern, target_tok);
              blk.instructions[i] = FormatSequenceString(
                  parsed->pattern.children[0].op, parsed->pattern.children[0].flags, *sub_or,
                  parsed->pattern.children[1].flags, *slot_or);
              continue;
            }
          } else {
            RewriteJumpTargetLabelRef(&parsed->pattern, target_tok);
            modified = true;
          }
        }
      }
      if (modified) {
        blk.instructions[i] = parsed->ToSexprString();
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

}  // namespace

RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& dbr_eager_cfg_x) {
  if (!IsValidCfg(dbr_eager_cfg_x)) {
    return {};
  }

  RtlControlFlowGraph out = dbr_eager_cfg_x;
  out.pass = RtlPassKind::kDbr;

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
    auto jump_pat_notes = ExtractSequenceSubInsn(jump_sub);
    auto slot_pat_notes = ExtractSequenceSubInsn(slot_sub);
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
    // without `/s`) on a conditional jump (`is_cond_jump`).
    if (!slot_sub.flags.in_struct) {
      if (!is_cond_jump || blk.successors.size() != 2 || b + 1 >= out.rpo_blocks.size()) {
        continue;
      }
      const size_t fallthrough_b = b + 1;

      std::vector<int> slot_defs;
      CollectDefinedRegs(slot_pat_notes->pattern, &slot_defs);
      bool jump_reads_slot_def = false;
      for (int r : slot_defs) {
        if (ExprReferencesRegInSrcOrMem(jump_pat, r)) {
          jump_reads_slot_def = true;
          break;
        }
      }

      const auto preds = BuildPredecessors(out);
      auto& ft_blk = out.rpo_blocks[fallthrough_b];
      const bool own_fallthrough =
          !ft_blk.has_label && preds[fallthrough_b].size() == 1 && preds[fallthrough_b][0] == b;

      bool ft_stole_from_end_jump = false;
      RtlPatternAndNotes ft_bare_jump_pat_notes;
      if (own_fallthrough && ft_blk.instructions.size() == 1) {
        auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
        if (ft_parsed.ok() &&
            (IsSimpleUncondJumpExpr(ft_parsed->pattern) || IsReturnExpr(ft_parsed->pattern))) {
          ft_stole_from_end_jump = true;
          ft_bare_jump_pat_notes = *ft_parsed;
        }
      } else if (own_fallthrough && ft_blk.instructions.size() == 2) {
        auto mid_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
        auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[1]);
        if (mid_parsed.ok() && ft_parsed.ok() && mid_parsed->pattern.IsSet() &&
            mid_parsed->pattern.SetDest().IsMem() && IsSimpleUncondJumpExpr(ft_parsed->pattern) &&
            !ExpressionsConflict(mid_parsed->pattern, slot_pat_notes->pattern)) {
          ft_stole_from_end_jump = true;
          ft_bare_jump_pat_notes = *ft_parsed;
        }
      }

      if (!jump_sub.flags.unchanging && !jump_reads_slot_def && !ft_stole_from_end_jump) {
        continue;
      }

      // Unpack `b`'s conditional jump.
      blk.instructions.back() = jump_pat_notes->ToSexprString();

      // If `fallthrough_b` ends with a bare unconditional jump or return from
      // which `slot_pat` was stolen via `steal_delay_list_from_fallthrough`,
      // re-wrap that terminating jump into a `SEQUENCE`.
      if (ft_stole_from_end_jump &&
          (ft_blk.instructions.size() == 2 || !jump_reads_slot_def || jump_sub.flags.unchanging)) {
        ft_blk.instructions.back() = FormatSequenceString(
            RtlOp::kJumpInsn, RtlFlags{}, ft_bare_jump_pat_notes, RtlFlags{}, *slot_pat_notes);
      } else if (own_fallthrough) {
        ft_blk.instructions.insert(ft_blk.instructions.begin(), slot_pat_notes->ToSexprString());
      } else {
        // `!own_fallthrough`: insert a new fallthrough block `b + 1` that holds
        // `slot_pat` and falls through to the old `fallthrough_b`.
        RtlBasicBlock new_ft;
        new_ft.has_label = false;
        new_ft.instructions.push_back(slot_pat_notes->ToSexprString());
        new_ft.successors = {fallthrough_b + 1};
        InsertBlockAt(&out, fallthrough_b, std::move(new_ft));
        // After inserting at `fallthrough_b = b + 1`, `InsertBlockAt` shifted
        // all successors `>= fallthrough_b` by +1, so `blk`'s fallthrough edge
        // must point to `b + 1` (`fallthrough_b`).
        for (size_t& succ : out.rpo_blocks[b].successors) {
          if (succ == fallthrough_b + 1) {
            succ = fallthrough_b;
            break;
          }
        }
      }
      continue;
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

    const std::string slot_pat_only_sexpr = slot_pat_notes->pattern.ToSexprString();
    const auto preds = BuildPredecessors(out);
    const bool pred_falls_through_to_target =
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
            other_last->pattern.children[1].op == RtlOp::kInsn &&
            other_last->pattern.children[1].flags.in_struct) {
          auto other_jump = ExtractSequenceSubInsn(other_last->pattern.children[0]);
          auto other_slot = ExtractSequenceSubInsn(other_last->pattern.children[1]);
          if (other_jump.ok() && other_slot.ok()) {
            const bool other_is_cond = IsCondJumpExpr(other_jump->pattern);
            const bool other_is_uncond = IsSimpleUncondJumpExpr(other_jump->pattern);
            if (other_is_cond || other_is_uncond) {
              size_t other_target =
                  other_blk.successors.empty() ? out.rpo_blocks.size() : other_blk.successors[0];
              if (other_is_cond && other_blk.successors.size() == 2) {
                other_target = (other_blk.successors[0] == p_blk + 1) ? other_blk.successors[1]
                                                                      : other_blk.successors[0];
              }
              if (other_target == target_b &&
                  other_slot->pattern.ToSexprString() == slot_pat_only_sexpr) {
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

    // Unpack the terminating `SEQUENCE` of every block in `same_slot_jumpers`.
    for (size_t p_blk : same_slot_jumpers) {
      auto& p_block = out.rpo_blocks[p_blk];
      auto p_last = ParsePatternAndNotes(p_block.instructions.back());
      if (p_last.ok() && p_last->pattern.op == RtlOp::kSequence &&
          !p_last->pattern.children.empty()) {
        auto p_jump = ExtractSequenceSubInsn(p_last->pattern.children[0]);
        if (p_jump.ok()) {
          p_block.instructions.back() = p_jump->ToSexprString();
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
        if (last_active_parsed.ok() &&
            last_active_parsed->pattern.ToSexprString() == slot_pat_only_sexpr) {
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
            }
            out.rpo_blocks[pred_b].has_label = true;
            if (!has_other_jumpers) {
              auto& tgt_blk = out.rpo_blocks[target_b];
              out.rpo_blocks[pred_b].instructions.insert(out.rpo_blocks[pred_b].instructions.end(),
                                                         tgt_blk.instructions.begin(),
                                                         tgt_blk.instructions.end());
              out.rpo_blocks[pred_b].successors = tgt_blk.successors;
              out.rpo_blocks[pred_b].addr_vec_targets = tgt_blk.addr_vec_targets;
              EraseBlockAt(&out, target_b);
              if (target_b <= b && rev_idx > 0) {
                --rev_idx;
              }
            }
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
            InsertBlockAt(&out, old_target_b, std::move(new_blk));
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
            continue;
          }
        }
      }
    }

    // Case T2: `!pred_falls_through_to_target` and `!has_other_jumpers` (all
    // predecessors of `target_b` are in `same_slot_jumpers`).
    if (!pred_falls_through_to_target && !has_other_jumpers) {
      auto& tgt_blk = out.rpo_blocks[target_b];
      tgt_blk.instructions.insert(tgt_blk.instructions.begin(), slot_pat_notes->ToSexprString());
      tgt_blk.has_label = true;
      continue;
    }

    // Case T3: `target_b` is shared with other predecessors (`has_other_jumpers`
    // is true, or `pred_falls_through_to_target` is true without matching Case T1).
    if (!pred_falls_through_to_target) {
      const size_t old_target_b = target_b;
      RtlBasicBlock tramp_blk;
      tramp_blk.has_label = true;
      tramp_blk.instructions.push_back(slot_pat_notes->ToSexprString());
      tramp_blk.successors = {old_target_b + 1};
      InsertBlockAt(&out, old_target_b, std::move(tramp_blk));
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
    } else {
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
          out.rpo_blocks.back().instructions.empty()) {
        insert_pos = out.rpo_blocks.size() - 1;
      }

      RtlPatternAndNotes uncond_jump;
      uncond_jump.pattern =
          RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"));
      RtlBasicBlock tramp_blk;
      tramp_blk.has_label = true;
      tramp_blk.instructions.push_back(FormatSequenceString(
          RtlOp::kJumpInsn, RtlFlags{}, uncond_jump, RtlFlags{}, *slot_pat_notes));
      const size_t shifted_target_b = (target_b >= insert_pos) ? (target_b + 1) : target_b;
      tramp_blk.successors = {shifted_target_b};
      InsertBlockAt(&out, insert_pos, std::move(tramp_blk));
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
    }
  }

  SynchronizeCanonicalLabels(&out);
  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    out.rpo_blocks[b].id = static_cast<uint32_t>(b);
    out.rpo_blocks[b].SyncRtlInsnsFromInstructions();
  }
  return out;
}

}  // namespace rom_nom_nom::lifter
