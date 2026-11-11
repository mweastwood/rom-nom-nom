#include "lifter/invert_do_cross_jump.h"

#include <algorithm>
#include <cstddef>
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

// Validates that an `RtlExpr` subtree contains no unknown opcodes and that any
// `(sequence [...])` node is a bracketed sequence of at least 2 valid inner
// insn wrappers.
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
    if (blk.instructions.empty()) {
      const bool is_trailing_end_label_block =
          (b + 1 == num_blocks && num_blocks > 1 && blk.successors.empty() &&
           blk.addr_vec_targets.empty());
      if (!is_trailing_end_label_block) {
        return false;
      }
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
  return expr.IsSet() && expr.SetDest().op == RtlOp::kPc && expr.SetSrc().op == RtlOp::kLabelRef &&
         !expr.SetSrc().str_val.empty();
}

bool IsTableJumpExpr(const RtlExpr& expr) {
  if (expr.op != RtlOp::kParallel || expr.children.size() < 2) {
    return false;
  }
  const RtlExpr& c0 = expr.children[0];
  const RtlExpr& c1 = expr.children[1];
  return c0.IsSet() && c0.SetDest().op == RtlOp::kPc && c0.SetSrc().op == RtlOp::kReg &&
         c1.op == RtlOp::kUse && !c1.children.empty() && c1.children[0].op == RtlOp::kLabelRef &&
         !c1.children[0].str_val.empty();
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

bool IsEpilogueOrControlTopExpr(const RtlExpr& expr) {
  return expr.op == RtlOp::kUse || expr.op == RtlOp::kClobber || IsAnyJumpOrReturnTopExpr(expr);
}

bool IsNoopRegMoveExpr(const RtlExpr& expr) {
  return expr.IsSet() && expr.SetDest().op == RtlOp::kReg && expr.SetSrc().op == RtlOp::kReg &&
         expr.SetDest().RegNo() == expr.SetSrc().RegNo() &&
         expr.SetDest().mode == expr.SetSrc().mode;
}

bool ContainsLabelRefOrSymbolRefInParsedInsn(const RtlPatternAndNotes& parsed) {
  bool found = false;
  auto visit_fn = [&found](const RtlExpr& node) {
    if (node.op == RtlOp::kLabelRef || node.op == RtlOp::kSymbolRef) {
      found = true;
    }
  };
  parsed.pattern.Visit(visit_fn);
  for (const RtlRegNote& note : parsed.reg_notes) {
    note.datum.Visit(visit_fn);
  }
  return found;
}

bool ContainsLabelRefInParsedInsn(const RtlPatternAndNotes& parsed) {
  bool found = false;
  auto visit_fn = [&found](const RtlExpr& node) {
    if (node.op == RtlOp::kLabelRef) {
      found = true;
    }
  };
  parsed.pattern.Visit(visit_fn);
  for (const RtlRegNote& note : parsed.reg_notes) {
    note.datum.Visit(visit_fn);
  }
  return found;
}

bool HasNonConstIntEqualOrEquivNote(const RtlPatternAndNotes& parsed) {
  for (const RtlRegNote& note : parsed.reg_notes) {
    if ((note.kind == RtlMachineMode::kRegEqual || note.kind == RtlMachineMode::kRegEquiv) &&
        note.datum.op != RtlOp::kConstInt) {
      return true;
    }
  }
  return false;
}

// Returns true if `pat` is a non-control, non-use/clobber, non-note instruction
// that `find_cross_jump` (`jump.c`) counts toward `minimum` and can cross-jump
// across basic blocks without altering label reference counts or losing
// non-constant `REG_EQUAL`/`REG_EQUIV` notes under pointer-equality checks.
bool IsShareableCrossJumpInsn(std::string_view pat) {
  auto parsed = ParsePatternAndNotes(pat);
  if (!parsed.ok()) {
    return false;
  }
  const RtlExpr& expr = parsed->pattern;
  if (IsEpilogueOrControlTopExpr(expr) || expr.op == RtlOp::kNote || expr.op == RtlOp::kAddrVec ||
      expr.op == RtlOp::kAddrDiffVec || expr.op == RtlOp::kConstInt ||
      expr.op == RtlOp::kAsmInput || expr.op == RtlOp::kAsmOperands ||
      expr.op == RtlOp::kSequence) {
    return false;
  }
  if (IsNoopRegMoveExpr(expr) || ContainsLabelRefOrSymbolRefInParsedInsn(*parsed) ||
      HasNonConstIntEqualOrEquivNote(*parsed)) {
    return false;
  }
  return true;
}

bool IsSimpleUncondJumpInsn(std::string_view pat) {
  auto parsed = ParsePatternAndNotes(pat);
  return parsed.ok() && IsSimpleUncondJumpExpr(parsed->pattern);
}

bool IsAnyJumpOrReturnInsn(std::string_view pat) {
  auto parsed = ParsePatternAndNotes(pat);
  return parsed.ok() && IsAnyJumpOrReturnTopExpr(parsed->pattern);
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
          RewriteJumpTargetLabelRef(&parsed->pattern, block_label_token[target_b]);
          modified = true;
        }
      }
      if (modified) {
        blk.instructions[i] = parsed->ToGccPrintRtlString();
      }
    }
  }
}

// Returns true when `cfg` was produced by `-O1`/`-O2`/`-O3` (`flow_analysis`
// has attached `REG_DEAD`/`REG_UNUSED` notes or `jump` emitted `return_internal`).
// Unoptimized (`-O0`) CFGs never run `flow_analysis` or `jump2`.
bool HasPostFlowNotesOrReturn(const RtlControlFlowGraph& cfg) {
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok()) {
        continue;
      }
      if (IsReturnExpr(parsed->pattern)) {
        return true;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) {
          return true;
        }
      }
    }
  }
  return false;
}

// `simple_epilogue_p ()` functions (`tsize == 0`) additionally replace the
// trailing empty return-label block with a `return_internal` jump_insn.
bool IsOptimizedJump2Cfg(const RtlControlFlowGraph& cfg) {
  if (cfg.rpo_blocks.size() > 1 && cfg.rpo_blocks.back().instructions.empty()) {
    return false;
  }
  return HasPostFlowNotesOrReturn(cfg);
}

int TrueRegnum(const RtlExpr& expr) {
  if (expr.op == RtlOp::kReg) {
    return expr.RegNo();
  }
  if (expr.op == RtlOp::kSubreg && !expr.children.empty()) {
    const int base = TrueRegnum(expr.children[0]);
    if (base >= 0) {
      return base + static_cast<int>(expr.int_val);
    }
  }
  return -1;
}

bool ContainsCallInExpr(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&found](const RtlExpr& node) {
    if (node.op == RtlOp::kCall) {
      found = true;
    }
  });
  return found;
}

bool IsNormalNonJumpNonCallInsn(const RtlExpr& expr) {
  if (expr.op == RtlOp::kNote || expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec ||
      expr.op == RtlOp::kSequence) {
    return false;
  }
  if (IsAnyJumpOrReturnTopExpr(expr) || ContainsCallInExpr(expr)) {
    return false;
  }
  return true;
}

bool IsMipsO32CallArgReg(int regno) {
  return (regno >= 4 && regno <= 7) || (regno >= 44 && regno <= 47);
}

// In `jump.c:jump_optimize` (lines 434-465, `if (noop_moves)`), every normal
// `INSN` whose body is `(set <dest> <src>)` with `sreg = true_regnum(src) >= 0`,
// `dreg = true_regnum(dest) >= 0`, and `sreg != dreg` walks backward via
// `prev_nonnote_insn` up to the nearest `CODE_LABEL` and calls
// `remove_death(dreg, trial)` on the first instruction `trial` that carries a
// `REG_DEAD` note for `dreg`. When a `REG_DEAD` note for `dreg` survived into
// `.11.jump2` (because an intervening `REG_DEAD` note or label in `.10.sched2`
// was consumed during the forward pass), prepending a compensating `REG_DEAD`
// note on `trial` ensures `remove_death` consumes the compensating note and
// preserves `trial`'s surviving `REG_DEAD` note.
void RestoreNoopMovesConsumedDeaths(RtlControlFlowGraph* cfg) {
  struct FlatInsn {
    size_t b = 0;
    size_t i = 0;
    RtlPatternAndNotes parsed;
    std::vector<int> original_dead_regnos;
    std::vector<RtlRegNote> original_dead_notes;
    std::vector<int> call_arg_dead_regnos;
    bool modified = false;
  };

  const size_t num_blocks = cfg->rpo_blocks.size();
  size_t run_start_b = 0;
  while (run_start_b < num_blocks) {
    size_t run_end_b = run_start_b + 1;
    while (run_end_b < num_blocks && !cfg->rpo_blocks[run_end_b].has_label) {
      ++run_end_b;
    }

    std::vector<FlatInsn> run_insns;
    for (size_t b = run_start_b; b < run_end_b; ++b) {
      const auto& blk = cfg->rpo_blocks[b];
      for (size_t i = 0; i < blk.instructions.size(); ++i) {
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok() || parsed->pattern.op == RtlOp::kNote) {
          continue;
        }
        FlatInsn entry;
        entry.b = b;
        entry.i = i;
        entry.parsed = std::move(*parsed);
        for (const RtlRegNote& note : entry.parsed.reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
            entry.original_dead_regnos.push_back(note.datum.RegNo());
            entry.original_dead_notes.push_back(note);
          }
        }
        run_insns.push_back(std::move(entry));
      }
    }

    for (size_t idx = 0; idx < run_insns.size(); ++idx) {
      if (!ContainsCallInExpr(run_insns[idx].parsed.pattern)) {
        continue;
      }
      for (size_t prev = idx; prev > 0;) {
        --prev;
        const RtlExpr& prev_expr = run_insns[prev].parsed.pattern;
        if (ContainsCallInExpr(prev_expr) || IsAnyJumpOrReturnTopExpr(prev_expr)) {
          break;
        }
        if (IsNormalNonJumpNonCallInsn(prev_expr) && prev_expr.IsSet() &&
            prev_expr.SetDest().op == RtlOp::kReg) {
          const int dst_regno = prev_expr.SetDest().RegNo();
          if (!IsMipsO32CallArgReg(dst_regno)) {
            continue;
          }
          bool already_added = false;
          for (int existing : run_insns[idx].call_arg_dead_regnos) {
            if (existing == dst_regno) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            run_insns[idx].call_arg_dead_regnos.push_back(dst_regno);
          }
        }
      }
    }

    for (size_t idx = 0; idx < run_insns.size(); ++idx) {
      const RtlExpr& expr = run_insns[idx].parsed.pattern;
      if (!IsNormalNonJumpNonCallInsn(expr) || !expr.IsSet()) {
        continue;
      }
      const int sreg = TrueRegnum(expr.SetSrc());
      const int dreg = TrueRegnum(expr.SetDest());
      if (sreg < 0 || dreg < 0 || sreg == dreg) {
        continue;
      }
      for (size_t prev = idx; prev > 0;) {
        --prev;
        bool matched_call_arg = false;
        for (size_t k = 0; k < run_insns[prev].call_arg_dead_regnos.size(); ++k) {
          if (run_insns[prev].call_arg_dead_regnos[k] == dreg) {
            run_insns[prev].call_arg_dead_regnos.erase(
                run_insns[prev].call_arg_dead_regnos.begin() + static_cast<ptrdiff_t>(k));
            matched_call_arg = true;
            break;
          }
        }
        if (matched_call_arg) {
          break;
        }
        bool matched = false;
        for (size_t k = 0; k < run_insns[prev].original_dead_regnos.size(); ++k) {
          if (run_insns[prev].original_dead_regnos[k] == dreg) {
            run_insns[prev].parsed.reg_notes.insert(run_insns[prev].parsed.reg_notes.begin(),
                                                    run_insns[prev].original_dead_notes[k]);
            run_insns[prev].modified = true;
            matched = true;
            break;
          }
        }
        if (matched) {
          break;
        }
      }
    }

    for (const FlatInsn& entry : run_insns) {
      if (entry.modified) {
        cfg->rpo_blocks[entry.b].instructions[entry.i] = entry.parsed.ToGccPrintRtlString();
      }
    }

    run_start_b = run_end_b;
  }
}

}  // namespace

RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) {
  if (!IsValidCfg(jump2_cfg_x)) {
    return RtlControlFlowGraph{};
  }

  RtlControlFlowGraph out = jump2_cfg_x;
  out.pass = RtlPassKind::kSched2;

  const size_t num_blocks = out.rpo_blocks.size();
  if (IsOptimizedJump2Cfg(out)) {
    std::vector<std::vector<size_t>> preds(num_blocks);
    std::vector<bool> is_addr_vec_target(num_blocks, false);
    for (size_t b = 0; b < num_blocks; ++b) {
      for (size_t s : out.rpo_blocks[b].successors) {
        preds[s].push_back(b);
      }
      for (size_t t : out.rpo_blocks[b].addr_vec_targets) {
        is_addr_vec_target[t] = true;
      }
    }

    for (size_t join_b = 1; join_b < num_blocks; ++join_b) {
      if (preds[join_b].size() < 2 || is_addr_vec_target[join_b]) {
        continue;
      }
      const auto& join_blk = out.rpo_blocks[join_b];
      if (join_blk.instructions.empty()) {
        continue;
      }

      // For `jump_optimize` (`find_cross_jump` + `do_cross_jump`) to merge
      // duplicated instructions from all predecessors back into `join_b` and
      // delete the split label, `join_b - 1` must be a single-successor
      // fallthrough predecessor of `join_b`, and every other predecessor of
      // `join_b` must be a forward (`pred_b < join_b`) single-successor block
      // ending with a simple unconditional jump `(set (pc) (label_ref ...))`
      // to `join_b`.
      bool has_fallthrough_pred = false;
      bool all_preds_valid = true;
      for (size_t pred_b : preds[join_b]) {
        if (pred_b >= join_b) {
          all_preds_valid = false;
          break;
        }
        const auto& pred_blk = out.rpo_blocks[pred_b];
        if (pred_blk.instructions.empty() || pred_blk.successors.size() != 1 ||
            pred_blk.successors[0] != join_b || !pred_blk.addr_vec_targets.empty()) {
          all_preds_valid = false;
          break;
        }
        // Ensure no non-terminator instruction in `pred_blk` references a label.
        for (size_t i = 0; i + 1 < pred_blk.instructions.size(); ++i) {
          auto parsed = ParsePatternAndNotes(pred_blk.instructions[i]);
          if (!parsed.ok() || ContainsLabelRefInParsedInsn(*parsed)) {
            all_preds_valid = false;
            break;
          }
        }
        if (!all_preds_valid) {
          break;
        }
        if (pred_b + 1 == join_b && !IsAnyJumpOrReturnInsn(pred_blk.instructions.back())) {
          auto parsed_last = ParsePatternAndNotes(pred_blk.instructions.back());
          if (!parsed_last.ok() || ContainsLabelRefInParsedInsn(*parsed_last)) {
            all_preds_valid = false;
            break;
          }
          has_fallthrough_pred = true;
        } else if (!IsSimpleUncondJumpInsn(pred_blk.instructions.back())) {
          all_preds_valid = false;
          break;
        }
      }
      if (!all_preds_valid || !has_fallthrough_pred) {
        continue;
      }

      // Partition `join_blk.instructions` into leading shareable cross-jump
      // instructions and trailing epilogue/control instructions. Require at
      // least 2 shareable instructions so that duplicating all but the last
      // into predecessors still leaves at least 1 active non-jump, non-return
      // instruction at the head of `join_b` (preventing `jump_optimize` from
      // redirecting jumps to `RETURN` or threading them past `join_b` before
      // `find_cross_jump` runs).
      std::vector<std::string> shared_insns;
      std::vector<std::string> epilogue_insns;
      for (const std::string& pat : join_blk.instructions) {
        if (epilogue_insns.empty() && IsShareableCrossJumpInsn(pat)) {
          shared_insns.push_back(pat);
        } else {
          epilogue_insns.push_back(pat);
        }
      }
      if (shared_insns.size() < 2) {
        continue;
      }
      epilogue_insns.insert(epilogue_insns.begin(), shared_insns.back());
      shared_insns.pop_back();
      if (shared_insns.empty() || epilogue_insns.empty()) {
        continue;
      }

      for (size_t pred_b : preds[join_b]) {
        auto& pred_blk = out.rpo_blocks[pred_b];
        if (IsSimpleUncondJumpInsn(pred_blk.instructions.back())) {
          std::string jmp = std::move(pred_blk.instructions.back());
          pred_blk.instructions.pop_back();
          for (const std::string& s_pat : shared_insns) {
            pred_blk.instructions.push_back(s_pat);
          }
          pred_blk.instructions.push_back(std::move(jmp));
        } else {
          for (const std::string& s_pat : shared_insns) {
            pred_blk.instructions.push_back(s_pat);
          }
        }
      }
      out.rpo_blocks[join_b].instructions = std::move(epilogue_insns);
      break;
    }
  }

  SynchronizeCanonicalLabels(&out);
  RestoreNoopMovesConsumedDeaths(&out);
  for (auto& blk : out.rpo_blocks) {
    blk.SyncRtlInsnsFromInstructions();
  }
  RecomputePostReloadLiveAtStart(&out);
  return out;
}

}  // namespace rom_nom_nom::lifter
