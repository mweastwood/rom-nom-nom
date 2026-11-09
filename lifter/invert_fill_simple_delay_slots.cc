#include "lifter/invert_fill_simple_delay_slots.h"

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

std::string FormatTopLevelPatternAndNotes(const RtlPatternAndNotes& pn) {
  std::string out = pn.ToSexprString();
  if (pn.pattern.op == RtlOp::kParallel) {
    const std::string pat_str = pn.pattern.ToSexprString();
    std::string new_pat = "(parallel[ ";
    for (size_t i = 0; i < pn.pattern.children.size(); ++i) {
      if (i > 0) {
        new_pat.push_back(' ');
      }
      new_pat.append(pn.pattern.children[i].ToSexprString());
    }
    new_pat.append(" ] )");
    out.replace(0, pat_str.size(), new_pat);
  }
  return out;
}

std::string FormatSequenceString(RtlOp head_op, const RtlFlags& head_flags,
                                 const RtlPatternAndNotes& head_pat, const RtlFlags& slot_flags,
                                 const RtlPatternAndNotes& slot_pat) {
  return absl::StrCat("(", RtlExpr::OpToString(RtlOp::kSequence), " [(",
                      RtlExpr::OpToString(head_op), FormatFlagsSuffix(head_flags), " ",
                      FormatTopLevelPatternAndNotes(head_pat), ") (",
                      RtlExpr::OpToString(RtlOp::kInsn), FormatFlagsSuffix(slot_flags), " ",
                      FormatTopLevelPatternAndNotes(slot_pat), ")])");
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
        if (parsed->pattern.op == RtlOp::kSequence && parsed->pattern.children.size() == 2) {
          auto sub_or = ExtractSequenceSubInsn(parsed->pattern.children[0]);
          auto slot_or = ExtractSequenceSubInsn(parsed->pattern.children[1]);
          if (sub_or.ok() && slot_or.ok()) {
            blk.instructions[i] = FormatSequenceString(parsed->pattern.children[0].op,
                                                       parsed->pattern.children[0].flags, *sub_or,
                                                       parsed->pattern.children[1].flags, *slot_or);
            continue;
          }
        }
        blk.instructions[i] = FormatTopLevelPatternAndNotes(*parsed);
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

}  // namespace

RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x) {
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
      auto head_pat = ExtractSequenceSubInsn(head_sub);
      auto slot_pat = ExtractSequenceSubInsn(slot_sub);
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
          new_insns.push_back(FormatTopLevelPatternAndNotes(uninverted_jump));
          if (out.rpo_blocks[b].successors[0] == out.rpo_blocks[b].successors[1] &&
              b + 1 < out.rpo_blocks.size()) {
            out.rpo_blocks[b].instructions = std::move(new_insns);
            RtlBasicBlock ft_blk;
            ft_blk.id = static_cast<uint32_t>(b + 1);
            ft_blk.has_label = false;
            ft_blk.instructions = {FormatTopLevelPatternAndNotes(*slot_pat)};
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
                                                    FormatTopLevelPatternAndNotes(*slot_pat));
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
      new_insns.insert(new_insns.begin() + static_cast<ptrdiff_t>(slot_insert_pos),
                       FormatTopLevelPatternAndNotes(*slot_pat));
      new_insns.push_back(FormatTopLevelPatternAndNotes(*head_pat));
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

  return out;
}

}  // namespace rom_nom_nom::lifter
