#include "lifter/invert_do_cross_jump.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/compiler_flags.h"
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

bool HasNonShareableEqualOrEquivNote(const RtlPatternAndNotes& parsed) {
  for (const RtlRegNote& note : parsed.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEqual || note.kind == RtlMachineMode::kRegEquiv) {
      if (note.datum.op != RtlOp::kConstInt || note.datum.int_val < -64 ||
          note.datum.int_val > 64) {
        return true;
      }
    }
  }
  return false;
}

// Returns true if `pat` is a non-control, non-use/clobber, non-note instruction
// that `find_cross_jump` (`jump.c`) counts toward `minimum` and can cross-jump
// across basic blocks without altering label reference counts or losing
// non-interned `REG_EQUAL`/`REG_EQUIV` notes under pointer-equality checks.
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
  if (IsNoopRegMoveExpr(expr) || ContainsLabelRefInParsedInsn(*parsed) ||
      HasNonShareableEqualOrEquivNote(*parsed)) {
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

std::bitset<68> MipsCallUsedOrGlobalRegsMask() {
  std::bitset<68> mask;
  for (int r = 1; r <= 15; ++r) mask.set(r);
  mask.set(24);
  mask.set(25);
  mask.set(28);
  mask.set(29);
  mask.set(31);
  for (int r = 32; r <= 51; ++r) mask.set(r);
  for (int r = 64; r <= 67; ++r) mask.set(r);
  return mask;
}

void MarkHardRegSpan(int regno, RtlMachineMode mode, bool gp64, bool fp64, std::bitset<68>* mask) {
  if (regno <= 0 || regno >= 68) {
    return;
  }
  const int nregs = MipsHardRegNregs(regno, mode, gp64, fp64);
  for (int k = 0; k < nregs && regno + k < 68; ++k) {
    mask->set(static_cast<size_t>(regno + k));
  }
}

void CollectReadRegsInSubtree(const RtlExpr& expr, bool gp64, bool fp64,
                              std::bitset<68>* read_regs) {
  expr.Visit([gp64, fp64, read_regs](const RtlExpr& node) {
    if (node.op == RtlOp::kReg) {
      MarkHardRegSpan(node.RegNo(), node.mode, gp64, fp64, read_regs);
    }
  });
}

struct InsnSchedSummary {
  bool valid = false;
  bool is_call = false;
  bool touches_mem = false;
  bool is_single_cycle_int_alu = false;
  std::bitset<68> read_regs;
  std::bitset<68> written_regs;
};

InsnSchedSummary SummarizeInsnForSched2(std::string_view pat, bool gp64, bool fp64) {
  InsnSchedSummary sum;
  auto parsed = ParsePatternAndNotes(pat);
  if (!parsed.ok()) {
    return sum;
  }
  const RtlExpr& expr = parsed->pattern;
  if (expr.op == RtlOp::kNote || expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec ||
      expr.op == RtlOp::kSequence) {
    return sum;
  }
  sum.valid = true;
  sum.is_call = ContainsCallInExpr(expr);
  bool has_non_single_cycle_op = false;
  bool has_non_gpr_reg = false;
  expr.Visit([&](const RtlExpr& node) {
    switch (node.op) {
      case RtlOp::kMem:
      case RtlOp::kUnalignedLoad:
      case RtlOp::kUnalignedStore:
        sum.touches_mem = true;
        break;
      case RtlOp::kMult:
      case RtlOp::kDiv:
      case RtlOp::kUdiv:
      case RtlOp::kMod:
      case RtlOp::kUmod:
      case RtlOp::kSqrt:
      case RtlOp::kFloat:
      case RtlOp::kUnsignedFloat:
      case RtlOp::kFix:
      case RtlOp::kUnsignedFix:
      case RtlOp::kFloatExtend:
      case RtlOp::kFloatTruncate:
      case RtlOp::kUnspecVolatile:
      case RtlOp::kAsmInput:
      case RtlOp::kAsmOperands:
        has_non_single_cycle_op = true;
        break;
      case RtlOp::kReg:
        if (node.RegNo() < 0 || node.RegNo() > 31) {
          has_non_gpr_reg = true;
        }
        break;
      default:
        break;
    }
  });

  auto record_element = [&](const RtlExpr& elem) {
    if (elem.IsSet()) {
      const RtlExpr& dst = elem.SetDest();
      const int dreg = TrueRegnum(dst);
      if ((dst.op == RtlOp::kReg || dst.op == RtlOp::kSubreg) && dreg >= 0) {
        MarkHardRegSpan(dreg, dst.mode, gp64, fp64, &sum.written_regs);
      } else {
        CollectReadRegsInSubtree(dst, gp64, fp64, &sum.read_regs);
      }
      CollectReadRegsInSubtree(elem.SetSrc(), gp64, fp64, &sum.read_regs);
    } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
      const RtlExpr& dst = elem.children[0];
      const int dreg = TrueRegnum(dst);
      if ((dst.op == RtlOp::kReg || dst.op == RtlOp::kSubreg) && dreg >= 0) {
        MarkHardRegSpan(dreg, dst.mode, gp64, fp64, &sum.written_regs);
      } else {
        CollectReadRegsInSubtree(dst, gp64, fp64, &sum.read_regs);
      }
    } else {
      CollectReadRegsInSubtree(elem, gp64, fp64, &sum.read_regs);
    }
  };

  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& child : expr.children) {
      record_element(child);
    }
  } else {
    record_element(expr);
  }
  for (const RtlRegNote& note : parsed->reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
      MarkHardRegSpan(note.datum.RegNo(), note.datum.mode, gp64, fp64, &sum.read_regs);
    }
  }
  if (sum.is_call) {
    sum.written_regs |= MipsCallUsedOrGlobalRegsMask();
  }
  sum.is_single_cycle_int_alu = !sum.is_call && !sum.touches_mem && !has_non_single_cycle_op &&
                                !has_non_gpr_reg && !IsEpilogueOrControlTopExpr(expr);
  return sum;
}

bool HasSched2Dependence(const InsnSchedSummary& earlier, const InsnSchedSummary& later) {
  if (!earlier.valid || !later.valid) {
    return false;
  }
  if (later.is_call) {
    if (earlier.is_call || earlier.touches_mem) {
      return true;
    }
    const std::bitset<68> call_mask = MipsCallUsedOrGlobalRegsMask();
    return ((earlier.read_regs | earlier.written_regs) & call_mask).any();
  }
  return (earlier.written_regs & (later.read_regs | later.written_regs)).any() ||
         (earlier.read_regs & later.written_regs).any();
}

size_t SelectSched2SafeSharedPrefixLen(const std::vector<size_t>& pred_indices,
                                       const RtlControlFlowGraph& cfg,
                                       const std::vector<std::string>& shared_insns,
                                       const std::string& first_epilogue_insn, bool gp64,
                                       bool fp64) {
  if (shared_insns.empty()) {
    return 0;
  }
  std::vector<InsnSchedSummary> shared_sum;
  shared_sum.reserve(shared_insns.size());
  std::optional<size_t> last_call_idx;
  for (size_t k = 0; k < shared_insns.size(); ++k) {
    InsnSchedSummary s = SummarizeInsnForSched2(shared_insns[k], gp64, fp64);
    if (!s.valid) {
      return 0;
    }
    if (s.is_call) {
      last_call_idx = k;
    }
    shared_sum.push_back(s);
  }

  std::vector<InsnSchedSummary> pred_last_active;
  std::vector<bool> pred_all_single_cycle;
  std::vector<std::bitset<68>> pred_mem_load_written_regs;
  std::vector<std::bitset<68>> pred_all_written_regs;
  pred_last_active.reserve(pred_indices.size());
  pred_all_single_cycle.reserve(pred_indices.size());
  pred_mem_load_written_regs.reserve(pred_indices.size());
  pred_all_written_regs.reserve(pred_indices.size());
  for (size_t pred_b : pred_indices) {
    const auto& pred_blk = cfg.rpo_blocks[pred_b];
    bool all_sc = true;
    std::optional<InsnSchedSummary> last_act;
    std::bitset<68> mem_load_defs;
    std::bitset<68> all_defs;
    for (const std::string& pat : pred_blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok() || parsed->pattern.op == RtlOp::kNote ||
          IsEpilogueOrControlTopExpr(parsed->pattern)) {
        continue;
      }
      InsnSchedSummary s = SummarizeInsnForSched2(pat, gp64, fp64);
      if (!s.valid) {
        return 0;
      }
      if (!s.is_single_cycle_int_alu) {
        all_sc = false;
      }
      if (s.touches_mem && !s.is_call && s.written_regs.any()) {
        mem_load_defs |= s.written_regs;
      }
      all_defs |= s.written_regs;
      last_act = s;
    }
    if (!last_act.has_value()) {
      return 0;
    }
    pred_last_active.push_back(*last_act);
    pred_all_single_cycle.push_back(all_sc);
    pred_mem_load_written_regs.push_back(mem_load_defs);
    pred_all_written_regs.push_back(all_defs);
  }

  if (last_call_idx.has_value()) {
    const size_t c_idx = *last_call_idx;
    for (size_t k = 0; k < c_idx; ++k) {
      if (shared_sum[k].touches_mem || !HasSched2Dependence(shared_sum[k], shared_sum[c_idx])) {
        return 0;
      }
    }
    for (size_t p = 0; p < pred_last_active.size(); ++p) {
      if (!HasSched2Dependence(pred_last_active[p], shared_sum[c_idx]) &&
          !pred_all_single_cycle[p]) {
        return 0;
      }
    }
    size_t len = c_idx + 1;
    while (len < shared_sum.size() && shared_sum[len].is_single_cycle_int_alu &&
           HasSched2Dependence(shared_sum[len - 1], shared_sum[len])) {
      ++len;
    }
    return len;
  }

  // Shared tail of memory stores followed by a 1-cycle integer ALU instruction
  // feeding `first_epilogue_insn`: if `join_b` had existed before `sched2`,
  // `sched2` would have hoisted the higher-priority ALU instruction ahead of
  // `shared_sum[0]`. When every predecessor loads the ALU operand from memory
  // (pinning the ALU instruction behind the stores via multi-cycle load
  // latency) and defines a register read by `shared_sum[0]`, the entire
  // `shared_sum` was un-merged before `jump2`.
  if (shared_sum.size() >= 2 && shared_sum[0].touches_mem && shared_sum[0].written_regs.none()) {
    size_t alu_idx = 0;
    while (alu_idx < shared_sum.size() && shared_sum[alu_idx].touches_mem &&
           shared_sum[alu_idx].written_regs.none()) {
      ++alu_idx;
    }
    const InsnSchedSummary epi_sum = SummarizeInsnForSched2(first_epilogue_insn, gp64, fp64);
    if (alu_idx >= 1 && alu_idx + 1 == shared_sum.size() &&
        shared_sum[alu_idx].is_single_cycle_int_alu && epi_sum.valid &&
        HasSched2Dependence(shared_sum[alu_idx], epi_sum)) {
      bool stores_independent_of_alu = true;
      for (size_t k = 0; k < alu_idx; ++k) {
        if (HasSched2Dependence(shared_sum[k], shared_sum[alu_idx])) {
          stores_independent_of_alu = false;
          break;
        }
      }
      bool all_preds_pin_stores_and_alu = stores_independent_of_alu;
      for (size_t p = 0; p < pred_indices.size() && all_preds_pin_stores_and_alu; ++p) {
        if ((pred_mem_load_written_regs[p] & shared_sum[alu_idx].read_regs).none() ||
            (pred_all_written_regs[p] & shared_sum[0].read_regs).none()) {
          all_preds_pin_stores_and_alu = false;
        }
      }
      if (all_preds_pin_stores_and_alu) {
        return shared_sum.size();
      }
    }
  }

  if (!shared_sum[0].is_single_cycle_int_alu) {
    return 0;
  }
  for (const InsnSchedSummary& last_act : pred_last_active) {
    if (!last_act.is_single_cycle_int_alu || !HasSched2Dependence(last_act, shared_sum[0])) {
      return 0;
    }
  }
  size_t len = 1;
  while (len < shared_sum.size() && shared_sum[len].is_single_cycle_int_alu &&
         HasSched2Dependence(shared_sum[len - 1], shared_sum[len])) {
    ++len;
  }
  return len;
}

bool IsMipsCalleeSavedGpr(int regno) {
  return (regno >= 16 && regno <= 23) || regno == 30;
}

bool IsPrologueStackRegSave(const RtlExpr& expr, int* out_saved_reg, int64_t* out_sp_off) {
  if (!expr.IsSet() || expr.SetDest().op != RtlOp::kMem || expr.SetDest().children.size() != 1 ||
      expr.SetSrc().op != RtlOp::kReg) {
    return false;
  }
  const RtlExpr& addr = expr.SetDest().children[0];
  int64_t off = 0;
  if (addr.op == RtlOp::kReg && addr.RegNo() == 29) {
    off = 0;
  } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
             addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == 29 &&
             addr.children[1].op == RtlOp::kConstInt) {
    off = addr.children[1].int_val;
  } else {
    return false;
  }
  *out_saved_reg = expr.SetSrc().RegNo();
  *out_sp_off = off;
  return *out_saved_reg >= 0;
}

bool HasRegDeadForRegno(const RtlPatternAndNotes& parsed, int regno) {
  for (const RtlRegNote& note : parsed.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
        note.datum.RegNo() == regno) {
      return true;
    }
  }
  return false;
}

// Reverses `jump.c:jump_optimize` (`noop_moves = 1`, lines 434-465) deletion of
// `(set (reg:M arg_reg) (reg:M saved_reg))` before the entry block's first
// `CALL_INSN` when `saved_reg` (`$16..$23` or `$30`) still holds incoming
// `arg_reg` (`$4..$7`) from an earlier `(set (reg:M saved_reg) (reg:M arg_reg))`
// in the same block.
void RestoreDeletedNoopCallArgMoves(RtlControlFlowGraph* cfg, bool gp64, bool fp64) {
  if (cfg->rpo_blocks.empty()) {
    return;
  }
  RtlBasicBlock& b0 = cfg->rpo_blocks[0];
  if (b0.instructions.size() < 4) {
    return;
  }

  std::vector<RtlPatternAndNotes> parsed_b0;
  std::vector<InsnSchedSummary> sum_b0;
  parsed_b0.reserve(b0.instructions.size());
  sum_b0.reserve(b0.instructions.size());
  std::optional<size_t> first_call_idx;
  std::optional<size_t> ra_save_idx;
  int64_t min_prologue_save_off = std::numeric_limits<int64_t>::max();
  absl::flat_hash_map<int, size_t> gpr_save_idx;

  for (size_t i = 0; i < b0.instructions.size(); ++i) {
    auto parsed = ParsePatternAndNotes(b0.instructions[i]);
    if (!parsed.ok()) {
      return;
    }
    InsnSchedSummary s = SummarizeInsnForSched2(b0.instructions[i], gp64, fp64);
    if (!first_call_idx.has_value()) {
      if (IsAnyJumpOrReturnTopExpr(parsed->pattern)) {
        return;
      }
      if (ContainsCallInExpr(parsed->pattern)) {
        first_call_idx = i;
      } else {
        int saved_reg = -1;
        int64_t sp_off = 0;
        if (IsPrologueStackRegSave(parsed->pattern, &saved_reg, &sp_off)) {
          if (saved_reg == 31 || IsMipsCalleeSavedGpr(saved_reg) ||
              (saved_reg >= 52 && saved_reg < 64)) {
            min_prologue_save_off = std::min(min_prologue_save_off, sp_off);
          }
          if (saved_reg == 31 && !ra_save_idx.has_value()) {
            ra_save_idx = i;
          } else if (IsMipsCalleeSavedGpr(saved_reg) && !gpr_save_idx.contains(saved_reg)) {
            gpr_save_idx[saved_reg] = i;
          }
        }
      }
    }
    parsed_b0.push_back(std::move(*parsed));
    sum_b0.push_back(s);
  }

  if (!first_call_idx.has_value() || !ra_save_idx.has_value()) {
    return;
  }
  const size_t call_idx = *first_call_idx;

  std::bitset<68> read_after_first_call;
  for (size_t i = call_idx + 1; i < sum_b0.size(); ++i) {
    read_after_first_call |= sum_b0[i].read_regs;
  }
  for (size_t b = 1; b < cfg->rpo_blocks.size(); ++b) {
    for (const std::string& pat : cfg->rpo_blocks[b].instructions) {
      InsnSchedSummary s = SummarizeInsnForSched2(pat, gp64, fp64);
      if (s.valid) {
        read_after_first_call |= s.read_regs;
      }
    }
  }

  std::bitset<8> arg_gpr_live_into_call;
  for (int r = 4; r <= 7; ++r) {
    std::optional<size_t> last_def;
    for (size_t i = 0; i < call_idx; ++i) {
      if (sum_b0[i].written_regs.test(static_cast<size_t>(r))) {
        last_def = i;
      }
    }
    if (last_def.has_value()) {
      bool killed_before_call = false;
      for (size_t k = *last_def + 1; k < call_idx; ++k) {
        if (HasRegDeadForRegno(parsed_b0[k], r)) {
          killed_before_call = true;
          break;
        }
      }
      if (!killed_before_call) {
        arg_gpr_live_into_call.set(static_cast<size_t>(r));
      }
    }
  }

  // Track call argument word slots (`$4..$7`) already claimed by floating-point
  // argument registers `$f12` (`44`) and `$f14` (`46`) per `mips.c:function_arg`
  // (`lines 2871-2902`): when `$f12` holds argument 0, `$4` (and `$5` for `!gp64`
  // `DFmode`) cannot also be passed as a GPR argument to the same call.
  const bool is_gp64 = gp64;
  auto fpr_live_into_call = [&](int regno, bool* out_is_df) -> bool {
    std::optional<size_t> last_def;
    for (size_t i = 0; i < call_idx; ++i) {
      if (sum_b0[i].written_regs.test(static_cast<size_t>(regno))) {
        last_def = i;
      }
    }
    if (!last_def.has_value()) {
      return false;
    }
    for (size_t k = *last_def + 1; k < call_idx; ++k) {
      if (HasRegDeadForRegno(parsed_b0[k], regno)) {
        return false;
      }
    }
    if (out_is_df != nullptr) {
      const RtlExpr& def_pat = parsed_b0[*last_def].pattern;
      *out_is_df = def_pat.IsSet() && def_pat.SetDest().mode == RtlMachineMode::kDf;
    }
    return true;
  };
  std::bitset<8> arg_word_occupied_by_fpr;
  bool f12_is_df = false;
  if (fpr_live_into_call(44, &f12_is_df)) {
    arg_word_occupied_by_fpr.set(4);
    if (!is_gp64 && f12_is_df) {
      arg_word_occupied_by_fpr.set(5);
    }
    bool f14_is_df = false;
    if (fpr_live_into_call(46, &f14_is_df)) {
      if (is_gp64) {
        arg_word_occupied_by_fpr.set(5);
      } else if (f12_is_df) {
        arg_word_occupied_by_fpr.set(6);
        if (f14_is_df) {
          arg_word_occupied_by_fpr.set(7);
        }
      } else {
        arg_word_occupied_by_fpr.set(5);
        if (f14_is_df) {
          arg_word_occupied_by_fpr.set(6);
          arg_word_occupied_by_fpr.set(7);
        }
      }
    }
  }

  struct CandidateCopy {
    size_t copy_idx = 0;
    int saved_reg = -1;
    int arg_reg = -1;
    int span = 1;
    RtlMachineMode mode = RtlMachineMode::kSi;
  };
  std::vector<CandidateCopy> candidates;
  std::bitset<8> arg_gpr_saved_in_callee_reg;

  for (size_t i = 0; i < call_idx; ++i) {
    const RtlExpr& pat = parsed_b0[i].pattern;
    if (!IsNormalNonJumpNonCallInsn(pat) || !pat.IsSet() || pat.SetDest().op != RtlOp::kReg ||
        pat.SetSrc().op != RtlOp::kReg) {
      continue;
    }
    const RtlMachineMode mode = pat.SetDest().mode;
    if (mode != pat.SetSrc().mode || (mode != RtlMachineMode::kSi && mode != RtlMachineMode::kSf &&
                                      mode != RtlMachineMode::kDi && mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int saved_reg = pat.SetDest().RegNo();
    const int arg_reg = pat.SetSrc().RegNo();
    const int span = MipsHardRegNregs(saved_reg, mode, gp64, fp64);
    if (!IsMipsCalleeSavedGpr(saved_reg) || arg_reg < 4 || arg_reg + span - 1 > 7) {
      continue;
    }
    if (span == 2 && !IsMipsCalleeSavedGpr(saved_reg + 1)) {
      continue;
    }

    if (HasRegDeadForRegno(parsed_b0[i], arg_reg)) {
      continue;
    }

    bool valid = true;
    for (int off = 0; off < span; ++off) {
      const size_t sr = static_cast<size_t>(saved_reg + off);
      const size_t ar = static_cast<size_t>(arg_reg + off);
      if (!read_after_first_call.test(sr) || arg_word_occupied_by_fpr.test(ar)) {
        valid = false;
      }
      for (size_t k = 0; k < i; ++k) {
        if (sum_b0[k].written_regs.test(ar)) {
          valid = false;
        }
      }
      for (size_t k = i + 1; k < call_idx; ++k) {
        if (sum_b0[k].written_regs.test(sr) || sum_b0[k].written_regs.test(ar) ||
            sum_b0[k].read_regs.test(sr) || sum_b0[k].read_regs.test(ar)) {
          valid = false;
        }
      }
    }
    if (!valid) {
      continue;
    }
    candidates.push_back(CandidateCopy{i, saved_reg, arg_reg, span, mode});
    for (int off = 0; off < span; ++off) {
      arg_gpr_saved_in_callee_reg.set(static_cast<size_t>(arg_reg + off));
    }
  }

  if (candidates.empty()) {
    return;
  }

  if (b0.rtl_insns.size() != b0.instructions.size()) {
    b0.SyncRtlInsnsFromInstructions();
  }

  std::vector<std::string> restored_moves;
  for (const CandidateCopy& cand : candidates) {
    if (b0.rtl_insns[call_idx].has_explicit_call_fusage) {
      bool in_call_fusage = false;
      for (const RtlExpr& existing : b0.rtl_insns[call_idx].call_fusage) {
        if (existing.op == RtlOp::kUse && !existing.children.empty() &&
            existing.children[0].op == RtlOp::kReg &&
            existing.children[0].RegNo() == cand.arg_reg) {
          in_call_fusage = true;
          break;
        }
      }
      if (!in_call_fusage) {
        continue;
      }
    }

    bool save_hoisted_before_ra = false;
    for (int off = 0; off < cand.span; ++off) {
      auto it = gpr_save_idx.find(cand.saved_reg + off);
      if (it != gpr_save_idx.end() && it->second < *ra_save_idx && it->second < cand.copy_idx) {
        save_hoisted_before_ra = true;
      }
    }

    bool higher_arg_written = false;
    for (int r = cand.arg_reg + cand.span; r <= 7; ++r) {
      if (arg_gpr_live_into_call.test(static_cast<size_t>(r))) {
        higher_arg_written = true;
      }
    }
    bool all_lower_args_supplied = true;
    for (int r = 4; r < cand.arg_reg; ++r) {
      // Under MIPS O32, an 8-byte aligned DImode/DFmode argument in $6..$7 skips
      // $5 when $4 holds a 4-byte argument.
      if (cand.span == 2 && cand.arg_reg == 6 && r == 5) {
        continue;
      }
      if (!arg_gpr_live_into_call.test(static_cast<size_t>(r)) &&
          !arg_gpr_saved_in_callee_reg.test(static_cast<size_t>(r)) &&
          !arg_word_occupied_by_fpr.test(static_cast<size_t>(r))) {
        all_lower_args_supplied = false;
      }
    }

    if (!all_lower_args_supplied || (!save_hoisted_before_ra && !higher_arg_written)) {
      continue;
    }

    RtlRegNote dead_note;
    dead_note.kind = RtlMachineMode::kRegDead;
    dead_note.datum = RtlExpr::MakeReg(cand.mode, cand.arg_reg);
    parsed_b0[cand.copy_idx].reg_notes.insert(parsed_b0[cand.copy_idx].reg_notes.begin(),
                                              std::move(dead_note));
    b0.instructions[cand.copy_idx] = parsed_b0[cand.copy_idx].ToGccPrintRtlString();
    b0.rtl_insns[cand.copy_idx].canonical_pattern = b0.instructions[cand.copy_idx];

    RtlPatternAndNotes move_pat;
    move_pat.pattern = RtlExpr::MakeSet(RtlExpr::MakeReg(cand.mode, cand.arg_reg),
                                        RtlExpr::MakeReg(cand.mode, cand.saved_reg));
    restored_moves.push_back(move_pat.ToGccPrintRtlString());
  }

  if (restored_moves.empty()) {
    return;
  }

  size_t insert_pos = call_idx;
  while (insert_pos > 0) {
    int reg = -1;
    int64_t sp_off = 0;
    if (IsPrologueStackRegSave(parsed_b0[insert_pos - 1].pattern, &reg, &sp_off) && sp_off >= 16 &&
        sp_off < min_prologue_save_off) {
      --insert_pos;
    } else {
      break;
    }
  }
  std::vector<RtlInsn> restored_rtl_insns;
  restored_rtl_insns.reserve(restored_moves.size());
  for (const std::string& pat_str : restored_moves) {
    RtlInsn insn;
    insn.canonical_pattern = pat_str;
    insn.is_synthetic_rtl_expansion = true;
    restored_rtl_insns.push_back(std::move(insn));
  }
  b0.instructions.insert(b0.instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                         restored_moves.begin(), restored_moves.end());
  b0.rtl_insns.insert(b0.rtl_insns.begin() + static_cast<ptrdiff_t>(insert_pos),
                      std::make_move_iterator(restored_rtl_insns.begin()),
                      std::make_move_iterator(restored_rtl_insns.end()));
}

bool HasMultiCycleReadyLatency(const RtlExpr& expr) {
  bool multi_cycle = false;
  if (expr.IsSet() && expr.SetDest().op == RtlOp::kMem && !expr.SetDest().flags.volatil) {
    // Store (`memory` unit ready-delay = 1 in `mips.md:161`); only check address/src for loads.
    expr.SetSrc().Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kMem || node.op == RtlOp::kUnalignedLoad) {
        multi_cycle = true;
      }
    });
  } else {
    expr.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kMem || node.op == RtlOp::kUnalignedLoad ||
          node.op == RtlOp::kUnalignedStore) {
        multi_cycle = true;
      }
    });
  }
  expr.Visit([&](const RtlExpr& node) {
    switch (node.op) {
      case RtlOp::kMult:
      case RtlOp::kDiv:
      case RtlOp::kUdiv:
      case RtlOp::kMod:
      case RtlOp::kUmod:
      case RtlOp::kSqrt:
      case RtlOp::kFloat:
      case RtlOp::kUnsignedFloat:
      case RtlOp::kFix:
      case RtlOp::kUnsignedFix:
      case RtlOp::kFloatExtend:
      case RtlOp::kFloatTruncate:
        multi_cycle = true;
        break;
      case RtlOp::kReg:
        if (node.RegNo() >= 32 && node.RegNo() <= 63) {
          multi_cycle = true;
        }
        break;
      default:
        break;
    }
  });
  return multi_cycle;
}

bool ExprContainsMulOrDiv(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kMult || node.op == RtlOp::kDiv || node.op == RtlOp::kUdiv ||
        node.op == RtlOp::kMod || node.op == RtlOp::kUmod) {
      found = true;
    }
  });
  return found;
}

void CollectPatternReadRegs(const RtlExpr& expr, bool gp64, bool fp64, std::bitset<68>* read_regs) {
  auto record_elem = [&](const RtlExpr& elem) {
    if (elem.IsSet()) {
      const RtlExpr& dst = elem.SetDest();
      const int dreg = TrueRegnum(dst);
      if (!((dst.op == RtlOp::kReg || dst.op == RtlOp::kSubreg) && dreg >= 0)) {
        CollectReadRegsInSubtree(dst, gp64, fp64, read_regs);
      }
      CollectReadRegsInSubtree(elem.SetSrc(), gp64, fp64, read_regs);
    } else if (elem.op == RtlOp::kClobber) {
      if (!elem.children.empty()) {
        const RtlExpr& dst = elem.children[0];
        const int dreg = TrueRegnum(dst);
        if (!((dst.op == RtlOp::kReg || dst.op == RtlOp::kSubreg) && dreg >= 0)) {
          CollectReadRegsInSubtree(dst, gp64, fp64, read_regs);
        }
      }
    } else {
      CollectReadRegsInSubtree(elem, gp64, fp64, read_regs);
    }
  };
  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : expr.children) {
      record_elem(ch);
    }
  } else {
    record_elem(expr);
  }
}

// Reverses `jump.c:jump_optimize` (`noop_moves = 1`, lines 434-465) deletion of
// dead output reloads `(set (reg:DI 64) (reg:DI dst))` emitted when `reload`
// reloads a `REG_RETVAL` self-move on `MD_REGS` (`movdi_internal` has `*d <- *x`
// and `*x <- *d` alternatives, no `x <- x`) and reload inheritance substitutes
// `dst` into the consumer `I_cons` while leaving `REG_DEAD (reg:DI 64)` on
// `I_cons`. During `sched2` (`sched.c:2099-2101`, `2640-2672`), that output
// reload occupies `imuldiv` (`issue-delay = 3`) immediately ahead of `I_cons`,
// allowing 1-2 ready single-cycle GPR instructions to schedule between
// `(set (reg:DI dst) (reg:DI 64))` and `I_cons` before `jump2` deletes the
// redundant `(set (reg:DI 64) (reg:DI dst))` via `find_equiv_reg`.
void RestoreDeletedNoopMdOutputReloads(RtlControlFlowGraph* cfg, bool gp64, bool fp64) {
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    const size_t n = blk.instructions.size();
    if (n < 4) {
      continue;
    }

    std::vector<RtlPatternAndNotes> parsed;
    std::vector<InsnSchedSummary> sum;
    std::vector<std::bitset<68>> pat_reads(n);
    parsed.reserve(n);
    sum.reserve(n);
    bool parse_ok = true;
    for (size_t i = 0; i < n; ++i) {
      auto p = ParsePatternAndNotes(blk.instructions[i]);
      if (!p.ok()) {
        parse_ok = false;
        break;
      }
      sum.push_back(SummarizeInsnForSched2(blk.instructions[i], gp64, fp64));
      if (p->pattern.op != RtlOp::kNote && p->pattern.op != RtlOp::kAddrVec &&
          p->pattern.op != RtlOp::kAddrDiffVec && p->pattern.op != RtlOp::kSequence) {
        CollectPatternReadRegs(p->pattern, gp64, fp64, &pat_reads[i]);
      }
      parsed.push_back(std::move(*p));
    }
    if (!parse_ok) {
      continue;
    }

    std::bitset<68> cur_live;
    if (blk.successors.empty()) {
      cur_live.set(28);
      cur_live.set(29);
      cur_live.set(31);
      if (cfg->sideband.frame_pointer_needed) {
        cur_live.set(30);
      }
      if (cfg->sideband.epilogue_delay_pattern.has_value() &&
          !cfg->sideband.epilogue_delay_pattern->empty()) {
        InsnSchedSummary ep_sum =
            SummarizeInsnForSched2(*cfg->sideband.epilogue_delay_pattern, gp64, fp64);
        std::bitset<68> ep_reads;
        auto ep_parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
        if (ep_parsed.ok()) {
          CollectPatternReadRegs(ep_parsed->pattern, gp64, fp64, &ep_reads);
        }
        cur_live &= ~ep_sum.written_regs;
        cur_live |= ep_reads;
      }
    } else {
      for (size_t succ : blk.successors) {
        auto it = cfg->sideband.basic_block_live_at_start.find(static_cast<int>(succ));
        if (it != cfg->sideband.basic_block_live_at_start.end()) {
          for (int r : it->second) {
            if (r > 0 && r < 68) {
              cur_live.set(static_cast<size_t>(r));
            }
          }
        }
      }
    }

    std::vector<std::bitset<68>> live_after(n);
    for (size_t rev = n; rev > 0; --rev) {
      const size_t i = rev - 1;
      live_after[i] = cur_live;
      if (parsed[i].pattern.op == RtlOp::kNote) {
        continue;
      }
      cur_live &= ~sum[i].written_regs;
      cur_live |= pat_reads[i];
      cur_live.reset(0);
    }

    std::vector<bool> multi_cycle_priority_tainted(n, false);
    std::bitset<68> tainted_regs;
    std::vector<bool> md_from_mul_div_at(n, false);
    bool md_from_mul_div = false;
    for (size_t i = 0; i < n; ++i) {
      md_from_mul_div_at[i] = md_from_mul_div;
      if (parsed[i].pattern.op == RtlOp::kNote) {
        continue;
      }
      if (sum[i].is_call) {
        tainted_regs.reset();
        md_from_mul_div = false;
        continue;
      }
      const bool cur_tainted =
          HasMultiCycleReadyLatency(parsed[i].pattern) || (pat_reads[i] & tainted_regs).any();
      multi_cycle_priority_tainted[i] = cur_tainted;
      if (cur_tainted) {
        tainted_regs |= sum[i].written_regs;
      } else {
        tainted_regs &= ~sum[i].written_regs;
      }
      if (sum[i].written_regs.test(64) || sum[i].written_regs.test(65)) {
        md_from_mul_div = ExprContainsMulOrDiv(parsed[i].pattern);
      }
    }

    std::vector<std::pair<size_t, int>> insertions;
    for (size_t idx_mf = 0; idx_mf < n; ++idx_mf) {
      const RtlExpr& pat_mf = parsed[idx_mf].pattern;
      if (!IsNormalNonJumpNonCallInsn(pat_mf) || !pat_mf.IsSet() ||
          pat_mf.SetDest().op != RtlOp::kReg || pat_mf.SetDest().mode != RtlMachineMode::kDi ||
          pat_mf.SetSrc().op != RtlOp::kReg || pat_mf.SetSrc().mode != RtlMachineMode::kDi ||
          pat_mf.SetSrc().RegNo() != 64) {
        continue;
      }
      const int dst = pat_mf.SetDest().RegNo();
      if (dst < 2 || dst > 24 || (dst % 2) != 0 || md_from_mul_div_at[idx_mf] ||
          live_after[idx_mf].test(64) || live_after[idx_mf].test(65)) {
        continue;
      }
      bool has_reg_unused = false;
      for (const RtlRegNote& note : parsed[idx_mf].reg_notes) {
        if (note.kind == RtlMachineMode::kRegUnused) {
          has_reg_unused = true;
          break;
        }
      }
      if (has_reg_unused) {
        continue;
      }

      size_t active_intervening = 0;
      std::optional<size_t> idx_cons;
      bool span_ok = true;
      for (size_t k = idx_mf + 1; k < n; ++k) {
        const RtlExpr& pk = parsed[k].pattern;
        if (pk.op == RtlOp::kNote) {
          continue;
        }
        if (pat_reads[k].test(static_cast<size_t>(dst)) ||
            pat_reads[k].test(static_cast<size_t>(dst + 1))) {
          idx_cons = k;
          break;
        }
        if (pk.op == RtlOp::kClobber || pk.op == RtlOp::kUse) {
          if (sum[k].written_regs.test(static_cast<size_t>(dst)) ||
              sum[k].written_regs.test(static_cast<size_t>(dst + 1)) ||
              sum[k].written_regs.test(64) || sum[k].written_regs.test(65) ||
              pat_reads[k].test(64) || pat_reads[k].test(65)) {
            span_ok = false;
            break;
          }
          continue;
        }
        ++active_intervening;
        if (active_intervening > 2 || !sum[k].is_single_cycle_int_alu ||
            multi_cycle_priority_tainted[k] || sum[k].written_regs.test(static_cast<size_t>(dst)) ||
            sum[k].written_regs.test(static_cast<size_t>(dst + 1)) ||
            sum[k].written_regs.test(64) || sum[k].written_regs.test(65)) {
          span_ok = false;
          break;
        }
      }
      if (!span_ok || !idx_cons.has_value() || active_intervening < 1 || active_intervening > 2) {
        continue;
      }
      const size_t c_idx = *idx_cons;
      const RtlExpr& pat_cons = parsed[c_idx].pattern;
      if (!IsNormalNonJumpNonCallInsn(pat_cons) || pat_cons.op == RtlOp::kUse ||
          pat_cons.op == RtlOp::kClobber || live_after[c_idx].test(static_cast<size_t>(dst)) ||
          live_after[c_idx].test(static_cast<size_t>(dst + 1))) {
        continue;
      }
      bool nearby_hilo_after = false;
      size_t checked_after = 0;
      for (size_t k = c_idx; k < n && checked_after < 3; ++k) {
        const RtlExpr& pk = parsed[k].pattern;
        if (pk.op == RtlOp::kNote || pk.op == RtlOp::kClobber || pk.op == RtlOp::kUse) {
          continue;
        }
        if (pat_reads[k].test(64) || pat_reads[k].test(65) || sum[k].written_regs.test(64) ||
            sum[k].written_regs.test(65)) {
          nearby_hilo_after = true;
          break;
        }
        ++checked_after;
      }
      if (nearby_hilo_after) {
        continue;
      }

      if (!HasRegDeadForRegno(parsed[idx_mf], 64)) {
        parsed[idx_mf].reg_notes.insert(parsed[idx_mf].reg_notes.begin(),
                                        RtlRegNote{
                                            .kind = RtlMachineMode::kRegDead,
                                            .list_op = RtlOp::kExprList,
                                            .datum = RtlExpr::MakeReg(RtlMachineMode::kDi, 64),
                                        });
        blk.instructions[idx_mf] = parsed[idx_mf].ToGccPrintRtlString();
      }
      if (!HasRegDeadForRegno(parsed[c_idx], 64)) {
        parsed[c_idx].reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegDead,
            .list_op = RtlOp::kExprList,
            .datum = RtlExpr::MakeReg(RtlMachineMode::kDi, 64),
        });
        blk.instructions[c_idx] = parsed[c_idx].ToGccPrintRtlString();
      }
      insertions.push_back({c_idx, dst});
      idx_mf = c_idx;
    }

    if (insertions.empty()) {
      continue;
    }
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
    for (auto it = insertions.rbegin(); it != insertions.rend(); ++it) {
      RtlPatternAndNotes wb_pn;
      wb_pn.pattern = RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, 64),
                                       RtlExpr::MakeReg(RtlMachineMode::kDi, it->second));
      const std::string wb_str = wb_pn.ToGccPrintRtlString();
      RtlInsn wb_rinsn;
      wb_rinsn.canonical_pattern = wb_str;
      wb_rinsn.is_synthetic_rtl_expansion = true;
      blk.instructions.insert(blk.instructions.begin() + static_cast<ptrdiff_t>(it->first), wb_str);
      blk.rtl_insns.insert(blk.rtl_insns.begin() + static_cast<ptrdiff_t>(it->first),
                           std::move(wb_rinsn));
    }
  }
}

constexpr int kReturnAddressRegno = 31;

bool IsPostReloadReturnInternalPattern(const RtlExpr& pat) {
  return pat.op == RtlOp::kParallel && pat.children.size() == 2 &&
         pat.children[0].op == RtlOp::kReturn && pat.children[1].op == RtlOp::kUse &&
         pat.children[1].children.size() == 1 && pat.children[1].children[0].IsHardReg() &&
         pat.children[1].children[0].RegNo() == kReturnAddressRegno;
}

bool IsUnconditionalJumpOrTablejumpPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kPc) {
    return pat.children[1].op == RtlOp::kLabelRef || pat.children[1].op == RtlOp::kReg;
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
    return IsUnconditionalJumpOrTablejumpPattern(pat.children[0]);
  }
  return false;
}

bool IsMipsCalleeSavedOrRaReg(int regno) {
  return (regno >= 16 && regno <= 23) || regno == 30 || regno == 31 || (regno >= 52 && regno <= 63);
}

bool SubtreeContainsCallOrCalleeSavedReg(const RtlExpr& expr, bool gp64, bool fp64) {
  bool found = false;
  expr.Visit([&](const RtlExpr& node) {
    if (found) {
      return;
    }
    if (node.op == RtlOp::kCall) {
      found = true;
      return;
    }
    if (node.IsHardReg()) {
      const int r = node.RegNo();
      const int nw = std::max(1, MipsHardRegNregs(r, node.mode, gp64, fp64));
      for (int k = 0; k < nw; ++k) {
        if (IsMipsCalleeSavedOrRaReg(r + k)) {
          found = true;
          return;
        }
      }
    }
  });
  return found;
}

// Reverses `jump2` (`jump.c:289-306, 2065-2081`) insertion of trailing
// `return_internal` (`(parallel [(return) (use (reg:SI 31 ra))])`) on zero-frame
// leaf functions (`HAVE_return` == `reload_completed && simple_epilogue_p()`,
// `mips.md:5928-5941`, `mips.c:5398-5407`).
//
// In KMC GCC 2.7.2 MIPS, `define_expand "epilogue"` (`mips.md:6056`) is
// commented out, so `.09.greg` and `.10.sched2` never contain `return_internal`;
// `jump2` appends `emit_jump_insn(gen_return()); emit_barrier();` at
// `get_last_insn()` when `simple_epilogue_p()` holds (`total_size == 0`) and
// `get_last_insn()` is a non-`BARRIER` non-`NOTE` item. Note that `jump2`
// never converts early jumps `(set (pc) (label_ref L_exit))` in `b < n - 1`
// into `return_internal` because `redirect_exp` (`jump.c:3801`) attempts bare
// `(return)`, which `validate_change` rejects on MIPS.
bool UnexpandPostReloadTrailingReturnInsn(RtlControlFlowGraph* cfg,
                                          const CompilationProfile& profile) {
  if (profile.optimize_level <= 0 || cfg->rpo_blocks.empty() ||
      cfg->sideband.frame_size_bytes != 0 || cfg->sideband.frame_pointer_needed ||
      cfg->sideband.outgoing_args_size != 0) {
    return false;
  }

  const size_t n = cfg->rpo_blocks.size();
  if (n == 1 && cfg->rpo_blocks[0].instructions.size() == 1 && !cfg->rpo_blocks[0].has_label) {
    // Removing `return_internal` from a 1-block unlabeled 1-insn CFG would leave
    // zero non-`NOTE` items in the RTL chain (`jump.c:296-300` `insn == 0`).
    return false;
  }

  std::vector<size_t> ret_blocks;
  for (size_t b = 0; b < n; ++b) {
    const auto& insns = cfg->rpo_blocks[b].instructions;
    for (size_t i = 0; i < insns.size(); ++i) {
      const auto parsed = ParsePatternAndNotes(insns[i]);
      if (!parsed.ok()) {
        return false;
      }
      if (IsPostReloadReturnInternalPattern(parsed->pattern)) {
        if (i + 1 != insns.size() || !parsed->reg_notes.empty()) {
          return false;
        }
        ret_blocks.push_back(b);
      }
    }
  }
  // In KMC GCC 2.7.2 MIPS, `gen_return()` is emitted at most once per zero-frame
  // leaf function (at `get_last_insn()` in `jump.c:303, 2079` or `reorg.c:833`),
  // because `redirect_exp` (`jump.c:3801`) attempts bare `(return)` which fails
  // `validate_change`. If hand-written assembly contains 2+ `jr $ra` exits
  // (`ret_blocks.size() > 1`), `jump2` + `dbr_schedule` cannot synthesize the
  // non-trailing `return_internal`s from a `return_internal`-free `.10.sched2`,
  // so preserving them is required for $f(g(x)) = x$.
  if (ret_blocks.size() != 1) {
    return false;
  }

  const size_t ret_b = ret_blocks[0];
  {
    const RtlBasicBlock& ret_blk = cfg->rpo_blocks[ret_b];
    if (!ret_blk.successors.empty() || !ret_blk.addr_vec_targets.empty()) {
      return false;
    }
    if (ret_blk.instructions.size() >= 2) {
      const auto prev_parsed =
          ParsePatternAndNotes(ret_blk.instructions[ret_blk.instructions.size() - 2]);
      if (!prev_parsed.ok() || IsUnconditionalJumpOrTablejumpPattern(prev_parsed->pattern)) {
        return false;
      }
    } else if (!ret_blk.has_label && ret_b >= 1) {
      const RtlBasicBlock& pred_blk = cfg->rpo_blocks[ret_b - 1];
      const bool pred_falls_through =
          std::find(pred_blk.successors.begin(), pred_blk.successors.end(), ret_b) !=
          pred_blk.successors.end();
      if (!pred_falls_through || pred_blk.instructions.empty()) {
        return false;
      }
      const auto pred_last = ParsePatternAndNotes(pred_blk.instructions.back());
      if (!pred_last.ok() || IsUnconditionalJumpOrTablejumpPattern(pred_last->pattern)) {
        return false;
      }
    }
  }

  if (ret_b != n - 1) {
    const RtlBasicBlock& tail_blk = cfg->rpo_blocks[n - 1];
    if (tail_blk.instructions.empty() || !tail_blk.addr_vec_targets.empty()) {
      return false;
    }
    const auto tail_last = ParsePatternAndNotes(tail_blk.instructions.back());
    if (!tail_last.ok() || !IsUnconditionalJumpOrTablejumpPattern(tail_last->pattern)) {
      return false;
    }
  }

  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  for (size_t b = 0; b < n; ++b) {
    const auto& insns = cfg->rpo_blocks[b].instructions;
    const size_t limit = (b == ret_b) ? (insns.size() - 1) : insns.size();
    for (size_t i = 0; i < limit; ++i) {
      const auto parsed = ParsePatternAndNotes(insns[i]);
      if (!parsed.ok() || SubtreeContainsCallOrCalleeSavedReg(parsed->pattern, gp64, fp64)) {
        return false;
      }
    }
  }
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    const auto ep_parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (!ep_parsed.ok() || SubtreeContainsCallOrCalleeSavedReg(ep_parsed->pattern, gp64, fp64)) {
      return false;
    }
  }

  if (ret_b == n - 1) {
    RtlBasicBlock& mut_last = cfg->rpo_blocks[n - 1];
    mut_last.instructions.pop_back();
    if (!mut_last.rtl_insns.empty()) {
      mut_last.rtl_insns.pop_back();
    }
    mut_last.has_trailing_barrier = false;
    mut_last.successors.clear();

    if (n > 1 && mut_last.instructions.empty() && !mut_last.has_label &&
        mut_last.addr_vec_targets.empty()) {
      auto& pred_succs = cfg->rpo_blocks[n - 2].successors;
      pred_succs.erase(std::remove(pred_succs.begin(), pred_succs.end(), n - 1), pred_succs.end());
      cfg->rpo_blocks.pop_back();
    }
  } else {
    size_t total_labels = 0;
    for (size_t b = 0; b < n; ++b) {
      if (cfg->rpo_blocks[b].has_label) {
        ++total_labels;
      }
      if (!cfg->rpo_blocks[b].addr_vec_targets.empty()) {
        ++total_labels;
      }
    }

    const size_t exit_idx = n;
    const std::string exit_lbl = absl::StrCat("L", total_labels);
    RtlBasicBlock exit_blk;
    exit_blk.id = static_cast<uint32_t>(exit_idx);
    exit_blk.has_label = true;
    cfg->rpo_blocks.push_back(std::move(exit_blk));

    RtlBasicBlock& mut_b = cfg->rpo_blocks[ret_b];
    if (mut_b.rtl_insns.size() != mut_b.instructions.size()) {
      mut_b.SyncRtlInsnsFromInstructions();
    }
    const RtlExpr jump_expr =
        RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef(exit_lbl));
    mut_b.rtl_insns.back().SetFromExpr(jump_expr);
    mut_b.instructions.back() = mut_b.rtl_insns.back().canonical_pattern;
    mut_b.has_trailing_barrier = true;
    mut_b.successors = {exit_idx};
  }

  // When `.12.dbr` ended in a backward jump and interior `return_internal`
  // blocks were un-expanded (either in Stage 1 `make_return_insns` or above in
  // `jump2`), `jump2` had deleted the trailing `end_of_function_label` and its
  // `(use (reg/i ...))` once all interior jumps became `RETURN`. Restore
  // `(use (reg/i:<mode> <regno>))` in the trailing exit block if `cfg` marks a
  // return register with `REG_FUNCTION_VALUE_P` (`reg/i`) and has no existing
  // return `USE`.
  if (!cfg->rpo_blocks.empty() && cfg->rpo_blocks.back().successors.empty()) {
    bool has_existing_ret_use = false;
    bool saw_r2 = false;
    bool saw_r3 = false;
    bool saw_r2_di = false;
    bool saw_f0 = false;
    bool saw_f1 = false;
    bool saw_f0_df = false;
    for (const RtlBasicBlock& blk : cfg->rpo_blocks) {
      for (const std::string& pat_s : blk.instructions) {
        const auto parsed = ParsePatternAndNotes(pat_s);
        if (!parsed.ok()) {
          continue;
        }
        if (parsed->pattern.op == RtlOp::kUse) {
          parsed->pattern.Visit([&](const RtlExpr& node) {
            if (node.IsHardReg()) {
              const int r = node.RegNo();
              if (r == 2 || r == 3 || r == 32 || r == 33) {
                has_existing_ret_use = true;
              }
            }
          });
        }
        parsed->pattern.Visit([&](const RtlExpr& node) {
          if (!node.IsHardReg() || !node.flags.integrated) {
            return;
          }
          const int r = node.RegNo();
          if (r == 2) {
            saw_r2 = true;
            if (node.mode == RtlMachineMode::kDi) {
              saw_r2_di = true;
            }
          } else if (r == 3) {
            saw_r3 = true;
          } else if (r == 32) {
            saw_f0 = true;
            if (node.mode == RtlMachineMode::kDf) {
              saw_f0_df = true;
            }
          } else if (r == 33) {
            saw_f1 = true;
          }
        });
      }
    }
    if (!has_existing_ret_use && (saw_r2 || saw_r3 || saw_f0 || saw_f1)) {
      const int ret_regno = (saw_r2 || saw_r3) ? 2 : 32;
      const RtlMachineMode ret_mode =
          (ret_regno == 2) ? ((saw_r3 || saw_r2_di) ? RtlMachineMode::kDi : RtlMachineMode::kSi)
                           : ((saw_f1 || saw_f0_df) ? RtlMachineMode::kDf : RtlMachineMode::kSf);
      RtlBasicBlock& exit_blk = cfg->rpo_blocks.back();
      RtlInsn ret_use_insn;
      ret_use_insn.SetFromExpr(
          RtlExpr::MakeUse(RtlExpr::MakeReg(ret_mode, ret_regno, RtlFlags{.integrated = true})));
      exit_blk.AddInsn(std::move(ret_use_insn));
    }
  }

  cfg->sideband.saved_gpr_regnos.erase(
      std::remove(cfg->sideband.saved_gpr_regnos.begin(), cfg->sideband.saved_gpr_regnos.end(),
                  kReturnAddressRegno),
      cfg->sideband.saved_gpr_regnos.end());
  RecomputePostReloadLiveAtStart(cfg, profile);
  return true;
}

}  // namespace

RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) {
  return InvertDoCrossJump(jump2_cfg_x, CompilationProfile{.optimize_level = 2});
}

RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                      const CompilationProfile& profile) {
  if (!IsValidCfg(jump2_cfg_x)) {
    return RtlControlFlowGraph{};
  }

  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  const bool is_optimized = flags.opt_level > 0;
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(jump2_cfg_x, &profile, &gp64, &fp64);

  RtlControlFlowGraph out = jump2_cfg_x;
  out.pass = RtlPassKind::kSched2;

  const size_t num_blocks = out.rpo_blocks.size();
  if (is_optimized) {
    std::vector<std::vector<size_t>> preds(num_blocks);
    std::vector<bool> is_addr_vec_target(num_blocks, false);
    std::vector<bool> modified_blocks(num_blocks, false);
    for (size_t b = 0; b < num_blocks; ++b) {
      for (size_t s : out.rpo_blocks[b].successors) {
        preds[s].push_back(b);
      }
      for (size_t t : out.rpo_blocks[b].addr_vec_targets) {
        is_addr_vec_target[t] = true;
      }
    }

    for (size_t join_b = 1; join_b < num_blocks; ++join_b) {
      if (preds[join_b].size() < 2 || is_addr_vec_target[join_b] || modified_blocks[join_b]) {
        continue;
      }
      bool pred_already_modified = false;
      for (size_t pred_b : preds[join_b]) {
        if (modified_blocks[pred_b]) {
          pred_already_modified = true;
          break;
        }
      }
      if (pred_already_modified) {
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
      if (out.rpo_blocks[join_b].rtl_insns.size() != out.rpo_blocks[join_b].instructions.size()) {
        out.rpo_blocks[join_b].SyncRtlInsnsFromInstructions();
      }
      std::vector<std::string> shared_insns;
      std::vector<RtlInsn> shared_rtl_insns;
      std::vector<std::string> epilogue_insns;
      std::vector<RtlInsn> epilogue_rtl_insns;
      for (size_t idx = 0; idx < join_blk.instructions.size(); ++idx) {
        const std::string& pat = join_blk.instructions[idx];
        const RtlInsn& rinsn = join_blk.rtl_insns[idx];
        if (epilogue_insns.empty() && IsShareableCrossJumpInsn(pat)) {
          shared_insns.push_back(pat);
          shared_rtl_insns.push_back(rinsn);
        } else {
          epilogue_insns.push_back(pat);
          epilogue_rtl_insns.push_back(rinsn);
        }
      }
      if (shared_insns.size() < 2) {
        continue;
      }
      epilogue_insns.insert(epilogue_insns.begin(), shared_insns.back());
      epilogue_rtl_insns.insert(epilogue_rtl_insns.begin(), shared_rtl_insns.back());
      shared_insns.pop_back();
      shared_rtl_insns.pop_back();
      const size_t safe_len = SelectSched2SafeSharedPrefixLen(preds[join_b], out, shared_insns,
                                                              epilogue_insns.front(), gp64, fp64);
      if (safe_len == 0) {
        continue;
      }
      if (safe_len < shared_insns.size()) {
        epilogue_insns.insert(epilogue_insns.begin(),
                              shared_insns.begin() + static_cast<ptrdiff_t>(safe_len),
                              shared_insns.end());
        epilogue_rtl_insns.insert(epilogue_rtl_insns.begin(),
                                  shared_rtl_insns.begin() + static_cast<ptrdiff_t>(safe_len),
                                  shared_rtl_insns.end());
        shared_insns.resize(safe_len);
        shared_rtl_insns.resize(safe_len);
      }
      if (shared_insns.empty() || epilogue_insns.empty()) {
        continue;
      }

      for (size_t pred_b : preds[join_b]) {
        auto& pred_blk = out.rpo_blocks[pred_b];
        if (pred_blk.rtl_insns.size() != pred_blk.instructions.size()) {
          pred_blk.SyncRtlInsnsFromInstructions();
        }
        if (IsSimpleUncondJumpInsn(pred_blk.instructions.back())) {
          std::string jmp = std::move(pred_blk.instructions.back());
          RtlInsn jmp_rinsn = std::move(pred_blk.rtl_insns.back());
          pred_blk.instructions.pop_back();
          pred_blk.rtl_insns.pop_back();
          for (size_t k = 0; k < shared_insns.size(); ++k) {
            pred_blk.instructions.push_back(shared_insns[k]);
            pred_blk.rtl_insns.push_back(shared_rtl_insns[k]);
          }
          pred_blk.instructions.push_back(std::move(jmp));
          pred_blk.rtl_insns.push_back(std::move(jmp_rinsn));
        } else {
          for (size_t k = 0; k < shared_insns.size(); ++k) {
            pred_blk.instructions.push_back(shared_insns[k]);
            pred_blk.rtl_insns.push_back(shared_rtl_insns[k]);
          }
        }
        modified_blocks[pred_b] = true;
      }
      out.rpo_blocks[join_b].instructions = std::move(epilogue_insns);
      out.rpo_blocks[join_b].rtl_insns = std::move(epilogue_rtl_insns);
      modified_blocks[join_b] = true;
    }
  }

  SynchronizeCanonicalLabels(&out);
  for (auto& blk : out.rpo_blocks) {
    blk.SyncRtlInsnsFromInstructions();
  }
  RecomputePostReloadLiveAtStart(&out, profile);
  if (is_optimized) {
    RestoreNoopMovesConsumedDeaths(&out);
    RestoreDeletedNoopCallArgMoves(&out, gp64, fp64);
    if (!gp64 && flags.RunsSched2()) {
      RestoreDeletedNoopMdOutputReloads(&out, gp64, fp64);
    }
    for (auto& blk : out.rpo_blocks) {
      blk.SyncRtlInsnsFromInstructions();
    }
    UnexpandPostReloadTrailingReturnInsn(&out, profile);
  }
  return out;
}

}  // namespace rom_nom_nom::lifter
