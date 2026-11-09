#include "lifter/invert_make_return_insns.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

// Validates that an `RtlExpr` subtree contains no unknown opcodes and that any
// `(sequence [...])` node is a bracketed sequence of at least 2 valid inner
// insn wrappers.
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

// Validates that `sexpr` parses cleanly as a structured `RtlPatternAndNotes`
// AST with a recognized RTL operator and well-formed subtrees.
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

// Validates that `cfg` is a structurally well-formed `RtlControlFlowGraph`.
bool IsValidCfg(const RtlControlFlowGraph& cfg) {
  if (cfg.rpo_blocks.empty()) {
    return false;
  }
  const size_t num_blocks = cfg.rpo_blocks.size();
  bool has_any_instruction = false;
  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg.rpo_blocks[b];
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

// Extracts all `(label_ref <tok>)` tokens from `pat`'s core pattern AST.
std::vector<std::string> ExtractLabelRefTokensFromAst(std::string_view pat) {
  std::vector<std::string> tokens;
  auto parsed = ParsePatternAndNotes(pat);
  if (!parsed.ok()) {
    return tokens;
  }
  parsed->pattern.Visit([&tokens](const RtlExpr& node) {
    if (node.op == RtlOp::kLabelRef && node.children.empty()) {
      if (!node.str_val.empty()) {
        tokens.push_back(node.str_val);
      } else {
        tokens.push_back(absl::StrCat(node.int_val));
      }
    }
  });
  return tokens;
}

// Checks if `pat` is a `(sequence [(jump_insn... (return)...) ...])` and, if so,
// replaces the `(return)` pattern inside slot 0's `jump_insn` with
// `(set (pc) (label_ref <end_label>))`, preserving any `jump_insn` flags
// (`/s`, `/v`, `/u`, `/i`, `/f`) and any inner or outer `REG_NOTES`.
bool TryRewriteSequenceReturnToEndJump(std::string* pat, std::string_view end_label,
                                       bool require_from_target_flag) {
  auto parsed = ParsePatternAndNotes(*pat);
  if (!parsed.ok()) {
    return false;
  }
  RtlExpr& seq = parsed->pattern;
  if (seq.op != RtlOp::kSequence || !seq.bracketed_children || seq.children.size() < 2) {
    return false;
  }
  if (require_from_target_flag && !seq.children[1].flags.in_struct) {
    return false;
  }
  RtlExpr& slot0 = seq.children[0];
  if (slot0.op != RtlOp::kJumpInsn || slot0.children.empty() ||
      slot0.children[0].op != RtlOp::kReturn) {
    return false;
  }
  for (size_t i = 1; i < slot0.children.size(); ++i) {
    if (slot0.children[i].op != RtlOp::kExprList && slot0.children[i].op != RtlOp::kInsnList) {
      return false;
    }
  }
  slot0.children[0] =
      RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef(std::string(end_label)));
  *pat = parsed->ToSexprString();
  return true;
}

// Checks if `pat` is a `(sequence [(jump_insn (set (pc) (if_then_else ...))) ...])`
// and replaces any `(label_ref ...)` in slot 0's `jump_insn` core pattern with
// `(label_ref <new_label>)`.
bool TryRewriteSequenceCondJumpLabel(std::string* pat, std::string_view new_label) {
  auto parsed = ParsePatternAndNotes(*pat);
  if (!parsed.ok()) {
    return false;
  }
  RtlExpr& seq = parsed->pattern;
  if (seq.op != RtlOp::kSequence || !seq.bracketed_children || seq.children.size() < 2) {
    return false;
  }
  RtlExpr& slot0 = seq.children[0];
  if (slot0.op != RtlOp::kJumpInsn || slot0.children.empty() || !slot0.children[0].IsSet() ||
      slot0.children[0].SetDest().op != RtlOp::kPc ||
      slot0.children[0].SetSrc().op != RtlOp::kIfThenElse) {
    return false;
  }
  RtlExpr& ite = slot0.children[0].children[1];
  bool replaced = false;
  for (size_t i = 1; i < ite.children.size(); ++i) {
    if (ite.children[i].op == RtlOp::kLabelRef && ite.children[i].children.empty()) {
      ite.children[i].str_val = std::string(new_label);
      ite.children[i].int_val = 0;
      replaced = true;
    }
  }
  if (!replaced) {
    return false;
  }
  *pat = parsed->ToSexprString();
  return true;
}

// Returns true if every instruction in `blk` is a `(use ...)` or `(clobber ...)`
// pseudo-instruction that `find_end_label()` in `reorg.c:786-796` skips backwards
// over when searching for a trailing `CODE_LABEL`.
bool BlockContainsOnlyUseOrClobberInsns(const RtlBasicBlock& blk) {
  if (blk.instructions.empty()) {
    return false;
  }
  for (const std::string& pat : blk.instructions) {
    auto parsed = ParsePatternAndNotes(pat);
    if (!parsed.ok() ||
        (parsed->pattern.op != RtlOp::kUse && parsed->pattern.op != RtlOp::kClobber)) {
      return false;
    }
  }
  return true;
}

bool IsAnyJumpOrReturnTopExpr(const RtlExpr& expr) {
  if (expr.op == RtlOp::kSequence && !expr.children.empty()) {
    return expr.children[0].op == RtlOp::kJumpInsn;
  }
  if (expr.op == RtlOp::kReturn) {
    return true;
  }
  if (expr.op == RtlOp::kParallel && !expr.children.empty()) {
    const RtlExpr& first = expr.children[0];
    if (first.op == RtlOp::kReturn || (first.IsSet() && first.SetDest().op == RtlOp::kPc)) {
      return true;
    }
  }
  return expr.IsSet() && expr.SetDest().op == RtlOp::kPc;
}

// Returns true if `pat` is a stack-pointer adjustment instruction of the form
// `(set (reg:M R) (minus:M (reg:M R) ...))`.
bool IsStackPointerDecrementInsn(std::string_view pat) {
  auto parsed = ParsePatternAndNotes(pat);
  if (!parsed.ok() || !parsed->pattern.IsSet()) {
    return false;
  }
  const RtlExpr& dst = parsed->pattern.SetDest();
  const RtlExpr& src = parsed->pattern.SetSrc();
  if (!dst.IsReg() || src.op != RtlOp::kMinus || src.children.size() != 2 ||
      !src.children[0].IsReg()) {
    return false;
  }
  return dst.RegNo() == src.children[0].RegNo() && dst.str_val == "sp";
}

// Returns true if any basic block in `cfg` contains a `CALL_INSN` or a
// `SEQUENCE` wrapping a `CALL_INSN`.
bool CfgContainsCallInsn(const RtlControlFlowGraph& cfg) {
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok()) {
        continue;
      }
      const RtlExpr& expr = parsed->pattern;
      if (expr.op == RtlOp::kSequence && !expr.children.empty() &&
          expr.children[0].op == RtlOp::kCallInsn) {
        return true;
      }
      bool has_call = false;
      expr.Visit([&has_call](const RtlExpr& node) {
        if (node.op == RtlOp::kCall || node.op == RtlOp::kCallInsn) {
          has_call = true;
        }
      });
      if (has_call) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& dbr_return_cfg_x) {
  if (!IsValidCfg(dbr_return_cfg_x)) {
    return {};
  }

  RtlControlFlowGraph out = dbr_return_cfg_x;
  out.pass = RtlPassKind::kDbr;

  absl::flat_hash_set<std::string> used_label_tokens;
  for (const auto& blk : out.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      for (std::string& tok : ExtractLabelRefTokensFromAst(pat)) {
        used_label_tokens.insert(std::move(tok));
      }
    }
  }

  // Check if the trailing basic block is already an empty end-of-function label
  // block (or a `(use ...)` / `(clobber ...)` block with no earlier bare
  // `(return)`, since `find_end_label()` in `reorg.c:786-815` skips backwards
  // over trailing `USE` and `CLOBBER` insns to reuse the label before them),
  // and determine whether any existing block explicitly jumps or branches to it.
  const size_t num_blocks = out.rpo_blocks.size();
  bool has_earlier_bare_return = false;
  for (size_t b = 0; b + 1 < num_blocks; ++b) {
    const auto& blk = out.rpo_blocks[b];
    if (!blk.instructions.empty()) {
      auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
      if (last_parsed.ok() && last_parsed->pattern.op == RtlOp::kReturn) {
        has_earlier_bare_return = true;
        break;
      }
    }
  }

  bool reuse_trailing_empty_block = false;
  std::string existing_end_label;
  if (out.rpo_blocks.back().instructions.empty() ||
      (BlockContainsOnlyUseOrClobberInsns(out.rpo_blocks.back()) && !has_earlier_bare_return)) {
    reuse_trailing_empty_block = true;
    const size_t last_idx = num_blocks - 1;
    for (size_t b = 0; b + 1 < num_blocks; ++b) {
      const auto& blk = out.rpo_blocks[b];
      if (blk.instructions.empty() || !existing_end_label.empty()) {
        continue;
      }
      const std::vector<std::string> refs = ExtractLabelRefTokensFromAst(blk.instructions.back());
      if (refs.empty()) {
        continue;
      }
      bool explicitly_targets_last = false;
      if (blk.successors.size() >= 2) {
        bool all_equal_last = true;
        for (size_t succ : blk.successors) {
          if (succ != last_idx) {
            all_equal_last = false;
          } else if (succ != b + 1) {
            explicitly_targets_last = true;
          }
        }
        if (all_equal_last) {
          explicitly_targets_last = true;
        }
      } else if (blk.successors.size() == 1 && blk.successors[0] == last_idx) {
        explicitly_targets_last = true;
      }
      if (explicitly_targets_last) {
        existing_end_label = refs[0];
      }
    }
  }

  const size_t end_block_id = reuse_trailing_empty_block ? (num_blocks - 1) : num_blocks;
  std::string end_label = existing_end_label;
  if (end_label.empty()) {
    size_t candidate_id = end_block_id;
    do {
      end_label = absl::StrFormat("L%zu", candidate_id++);
    } while (used_label_tokens.contains(end_label));
  }

  bool has_prologue_or_calls = CfgContainsCallInsn(out);
  if (!has_prologue_or_calls && !out.rpo_blocks.empty()) {
    for (const std::string& pat : out.rpo_blocks[0].instructions) {
      if (IsStackPointerDecrementInsn(pat)) {
        has_prologue_or_calls = true;
        break;
      }
    }
  }
  bool converted_any = false;
  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    auto& blk = out.rpo_blocks[b];
    blk.id = b;
    bool modified_blk = false;
    const bool require_from_target_flag =
        (out.rpo_blocks.size() > 1 && blk.instructions.size() >= 2) || has_prologue_or_calls;
    for (std::string& pat : blk.instructions) {
      if (TryRewriteSequenceReturnToEndJump(&pat, end_label, require_from_target_flag)) {
        blk.successors = {end_block_id};
        converted_any = true;
        modified_blk = true;
      }
    }
    if (modified_blk || blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }

  if (converted_any) {
    if (reuse_trailing_empty_block) {
      out.rpo_blocks.back().has_label = true;
    } else {
      auto& prev_last_blk = out.rpo_blocks.back();
      if (prev_last_blk.successors.empty() && !prev_last_blk.instructions.empty()) {
        auto last_parsed = ParsePatternAndNotes(prev_last_blk.instructions.back());
        if (last_parsed.ok() && !IsAnyJumpOrReturnTopExpr(last_parsed->pattern)) {
          prev_last_blk.successors.push_back(end_block_id);
        }
      }
      RtlBasicBlock end_blk;
      end_blk.id = end_block_id;
      end_blk.has_label = true;
      out.rpo_blocks.push_back(std::move(end_blk));
    }
  } else if (!reuse_trailing_empty_block && !out.rpo_blocks.back().instructions.empty() &&
             out.rpo_blocks.back().successors.empty()) {
    // Check if the final basic block ends with an active non-jump/non-return
    // instruction (`reorg.c:4084-4128`): `make_return_insns` split the first
    // bare `(return)` into a labeled `ret_b` block and redirected conditional
    // `SEQUENCE` jumps targeting `real_end_of_function_label` to `ret_b`,
    // deleting the trailing `real_end_of_function_label` block.
    bool handled_split_ret = false;
    auto tail_last = ParsePatternAndNotes(out.rpo_blocks.back().instructions.back());
    if (tail_last.ok() && !IsAnyJumpOrReturnTopExpr(tail_last->pattern)) {
      size_t split_ret_b = out.rpo_blocks.size();
      for (size_t b = 1; b < out.rpo_blocks.size(); ++b) {
        const auto& blk = out.rpo_blocks[b];
        if (blk.instructions.size() != 1 || !blk.successors.empty()) {
          continue;
        }
        auto ret_parsed = ParsePatternAndNotes(blk.instructions[0]);
        if (!ret_parsed.ok() || ret_parsed->pattern.op != RtlOp::kReturn) {
          continue;
        }
        const auto& prev_blk = out.rpo_blocks[b - 1];
        if (prev_blk.instructions.empty() || prev_blk.successors != std::vector<size_t>{b}) {
          continue;
        }
        auto prev_last = ParsePatternAndNotes(prev_blk.instructions.back());
        if (!prev_last.ok() || IsAnyJumpOrReturnTopExpr(prev_last->pattern)) {
          continue;
        }
        split_ret_b = b;
        break;
      }
      if (split_ret_b < out.rpo_blocks.size()) {
        const bool tail_is_use_or_clobber =
            BlockContainsOnlyUseOrClobberInsns(out.rpo_blocks.back());
        bool any_other_block_targets_last = false;
        if (tail_is_use_or_clobber) {
          for (size_t b = 0; b + 1 < num_blocks; ++b) {
            if (b == split_ret_b - 1) {
              continue;
            }
            for (size_t succ : out.rpo_blocks[b].successors) {
              if (succ == num_blocks - 1 && succ != b + 1) {
                any_other_block_targets_last = true;
              }
            }
          }
        }
        const bool reuse_tail_block_as_end =
            tail_is_use_or_clobber && !any_other_block_targets_last;
        const std::string split_end_label = end_label;
        const size_t split_target_id = reuse_tail_block_as_end ? (num_blocks - 1) : end_block_id;
        bool all_other_preds_rewritten = true;
        bool redirected_any = false;
        for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
          if (b == split_ret_b - 1) {
            continue;
          }
          auto& blk = out.rpo_blocks[b];
          bool targets_ret_b = false;
          for (size_t succ : blk.successors) {
            if (succ == split_ret_b) {
              targets_ret_b = true;
            }
          }
          if (!targets_ret_b) {
            continue;
          }
          if (!blk.instructions.empty() &&
              TryRewriteSequenceCondJumpLabel(&blk.instructions.back(), split_end_label)) {
            for (size_t& succ : blk.successors) {
              if (succ == split_ret_b) {
                succ = split_target_id;
              }
            }
            blk.SyncRtlInsnsFromInstructions();
            redirected_any = true;
          } else {
            all_other_preds_rewritten = false;
          }
        }
        if (all_other_preds_rewritten && redirected_any) {
          out.rpo_blocks[split_ret_b - 1].instructions.push_back(
              out.rpo_blocks[split_ret_b].instructions[0]);
          out.rpo_blocks[split_ret_b - 1].successors.clear();
          out.rpo_blocks[split_ret_b - 1].SyncRtlInsnsFromInstructions();
          out.rpo_blocks.erase(out.rpo_blocks.begin() + static_cast<ptrdiff_t>(split_ret_b));
          for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
            out.rpo_blocks[b].id = b;
            for (size_t& succ : out.rpo_blocks[b].successors) {
              if (succ > split_ret_b) {
                --succ;
              }
            }
            for (size_t& target : out.rpo_blocks[b].addr_vec_targets) {
              if (target > split_ret_b) {
                --target;
              }
            }
          }
          if (reuse_tail_block_as_end) {
            out.rpo_blocks.back().has_label = true;
          } else {
            const size_t new_end_id = out.rpo_blocks.size();
            out.rpo_blocks.back().successors.push_back(new_end_id);
            RtlBasicBlock end_blk;
            end_blk.id = new_end_id;
            end_blk.has_label = true;
            out.rpo_blocks.push_back(std::move(end_blk));
          }
          handled_split_ret = true;
        }
      }
    }
    if (!handled_split_ret && BlockContainsOnlyUseOrClobberInsns(out.rpo_blocks.back())) {
      // When the final basic block contains only `(use ...)` / `(clobber ...)`
      // instructions and was preceded by an earlier `(return)` block,
      // `RunMakeReturnInsns` deleted a trailing unused `end_of_function_label`
      // (`--LABEL_NUSES == 0`) after the `(use ...)` block. Re-appending that
      // trailing empty labeled block ensures `find_end_label()` selects (and
      // deletes) the trailing label rather than mistaking the `(use ...)` block's
      // label for `end_of_function_label`.
      RtlBasicBlock end_blk;
      end_blk.id = end_block_id;
      end_blk.has_label = true;
      out.rpo_blocks.push_back(std::move(end_blk));
    }
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
