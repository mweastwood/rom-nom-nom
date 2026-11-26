#include "lifter/invert_fill_simple_delay_slots.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_multi_insn_idioms.h"
#include "lifter/post_reload_noop_moves.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_liveness.h"
#include "lifter/sched_model.h"

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
  const bool sync_rtl = (blk->rtl_insns.size() == blk->instructions.size());
  blk->instructions.insert(blk->instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                           std::move(insn_str));
  if (sync_rtl) {
    blk->rtl_insns.insert(blk->rtl_insns.begin() + static_cast<ptrdiff_t>(insert_pos), RtlInsn{});
  }
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

int UnwrappedRegNo(const RtlExpr& expr) {
  if (expr.IsReg()) {
    return expr.RegNo();
  }
  if (expr.IsSubreg() && !expr.children.empty() && expr.children[0].IsReg()) {
    return expr.children[0].RegNo() + static_cast<int>(expr.int_val);
  }
  return -1;
}

bool IsMipsFcmpExpr(const RtlExpr& pat) {
  return pat.IsSet() && pat.SetDest().IsReg() && pat.SetDest().RegNo() == 67;
}

bool IsMipsHiloExpr(const RtlExpr& pat) {
  return pat.op != RtlOp::kParallel && (pat.ContainsReg(64) || pat.ContainsReg(65));
}

bool IsMipsXferExpr(const RtlExpr& pat) {
  if (!pat.IsSet()) {
    return false;
  }
  const int dr = UnwrappedRegNo(pat.SetDest());
  const int sr = UnwrappedRegNo(pat.SetSrc());
  if ((dr >= 0 && dr < 32 && sr >= 32 && sr < 64) || (dr >= 32 && dr < 64 && sr >= 0 && sr < 32)) {
    return true;
  }
  return dr >= 32 && dr < 64 && pat.SetSrc().op == RtlOp::kConstDouble;
}

bool IsSimpleMipsMemAddr(const RtlExpr& addr) {
  if (addr.IsReg() && addr.RegNo() >= 0 && addr.RegNo() < 32) {
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsReg() &&
      addr.children[0].RegNo() >= 0 && addr.children[0].RegNo() < 32 &&
      addr.children[1].IsConstInt() && addr.children[1].int_val >= -32768 &&
      addr.children[1].int_val <= 32767) {
    return true;
  }
  return addr.op == RtlOp::kLoSum && addr.children.size() == 2 && addr.children[0].IsReg() &&
         addr.children[0].RegNo() >= 0 && addr.children[0].RegNo() < 32;
}

bool IsTwoWordConstIntSet(const RtlExpr& pat) {
  if (!pat.IsSet() || !pat.SetDest().IsReg() || pat.SetDest().RegNo() < 0 ||
      pat.SetDest().RegNo() >= 32 || !pat.SetSrc().IsConstInt()) {
    return false;
  }
  const int64_t val = pat.SetSrc().int_val;
  return !(val >= -32768 && val <= 65535) && (static_cast<uint64_t>(val) & 0xffffu) != 0;
}

// Models `mips.md` `define_delay` eligibility (`dslot == no && length == 1`).
bool IsIneligibleForMipsDelaySlot(const RtlExpr& pat, bool gp64, bool fp64) {
  if (IsMemLoadExpr(pat) || pat.op == RtlOp::kParallel || pat.op == RtlOp::kUnalignedStore ||
      IsMipsFcmpExpr(pat) || IsMipsHiloExpr(pat) || IsMipsXferExpr(pat) ||
      IsTwoWordConstIntSet(pat)) {
    return true;
  }
  if (!pat.IsSet()) {
    return false;
  }
  // `config/mips/mips.md:2595-2660` (`floatsidf2`/`floatdidf2`/`floatsisf2`/`floatdisf2`):
  // integer-to-FP conversions emit `mtc1`/`l.s` + `cvt.*` (`length = 3..4 > 1`),
  // so `mips.md:125-138` (`define_delay`) never accepts them in a delay slot.
  if (pat.SetSrc().op == RtlOp::kFloat || pat.SetSrc().op == RtlOp::kUnsignedFloat) {
    return true;
  }
  const int dr = UnwrappedRegNo(pat.SetDest());
  const int sr = UnwrappedRegNo(pat.SetSrc());
  if (!gp64 && pat.SetDest().mode == RtlMachineMode::kDi) {
    return true;
  }
  if (pat.SetDest().mode == RtlMachineMode::kDf &&
      ((!fp64 && (pat.SetDest().IsMem() || (dr >= 0 && dr < 32) || (sr >= 0 && sr < 32))) ||
       (!gp64 && ((dr >= 0 && dr < 32) || (sr >= 0 && sr < 32))))) {
    return true;
  }
  if (pat.SetDest().IsMem() &&
      (pat.SetDest().children.size() != 1 || !IsSimpleMipsMemAddr(pat.SetDest().children[0]))) {
    return true;
  }
  return false;
}

// Verifies that `slot_pn` can be placed at `cand_pos` (`< orig_len`) in
// `new_insns` without any instruction in `[cand_pos .. orig_len - 1]` either
// conflicting with `slot_pn` or being eligible ahead of `slot_pn` during
// `reorg.c:fill_simple_delay_slots`'s backward scan from `head_expr`.
bool CanPlaceUnpackedSlotBeforeIntervening(const std::vector<std::string>& new_insns,
                                           size_t cand_pos, size_t orig_len,
                                           const RtlPatternAndNotes& slot_pn,
                                           const RtlExpr& head_expr, bool gp64, bool fp64,
                                           bool allow_update_reg_dead_notes = false) {
  if (cand_pos >= orig_len || orig_len > new_insns.size()) {
    return false;
  }
  const bool slot_writes_mem = IsMemWriteExpr(slot_pn.pattern);
  const bool slot_has_mem = ExprHasMem(slot_pn.pattern);
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
        (slot_writes_mem && ExprHasMem(parsed->pattern)) ||
        (slot_has_mem && IsMemWriteExpr(parsed->pattern)) ||
        RegNotesConflictWithExpr(slot_pn.reg_notes, parsed->pattern) ||
        (!allow_update_reg_dead_notes &&
         RegNotesConflictWithExpr(parsed->reg_notes, slot_pn.pattern))) {
      return false;
    }
    window.push_back(std::move(*parsed));
  }

  for (size_t m = 0; m < window.size(); ++m) {
    const RtlExpr& m_pat = window[m].pattern;
    if (m_pat.op == RtlOp::kNote || m_pat.op == RtlOp::kUse || m_pat.op == RtlOp::kClobber) {
      continue;
    }
    if (IsIneligibleForMipsDelaySlot(m_pat, gp64, fp64)) {
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

bool IsPureBranchCondChainSetSrc(const RtlExpr& src) {
  if (src.IsComparison() || src.op == RtlOp::kAnd || src.op == RtlOp::kIor) {
    return true;
  }
  return src.op == RtlOp::kXor && src.children.size() == 2 && src.children[1].IsConstInt() &&
         src.children[1].int_val == 1;
}

bool ElemReadsRegBeforeKill(const RtlExpr& elem, int regno, bool* killed) {
  if (elem.op == RtlOp::kNote) {
    return false;
  }
  if (elem.op == RtlOp::kParallel) {
    bool reads = false;
    bool sub_killed = false;
    for (const RtlExpr& child : elem.children) {
      bool ck = false;
      if (ElemReadsRegBeforeKill(child, regno, &ck)) {
        reads = true;
      }
      if (ck) {
        sub_killed = true;
      }
    }
    if (reads) {
      return true;
    }
    if (sub_killed) {
      *killed = true;
    }
    return false;
  }
  bool has_call = false;
  elem.Visit([&has_call](const RtlExpr& n) {
    if (n.op == RtlOp::kCall) {
      has_call = true;
    }
  });
  if (elem.IsSet()) {
    const RtlExpr& dst = elem.SetDest();
    const RtlExpr& src = elem.SetSrc();
    if (src.ContainsReg(regno)) {
      return true;
    }
    const bool pure_reg_dst = dst.IsReg() || (dst.IsSubreg() && !dst.children.empty() &&
                                              dst.children[0].IsReg() && dst.int_val == 0);
    if (!pure_reg_dst && dst.ContainsReg(regno)) {
      return true;
    }
    if (dst.IsReg()) {
      const int dr = dst.RegNo();
      if (dr == regno || ((dst.mode == RtlMachineMode::kDi || dst.mode == RtlMachineMode::kDf) &&
                          dr + 1 == regno)) {
        *killed = true;
      }
    } else if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg() &&
               dst.children[0].RegNo() + static_cast<int>(dst.int_val) == regno) {
      *killed = true;
    }
  } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
    if (elem.children[0].IsMem() && elem.children[0].ContainsReg(regno)) {
      return true;
    }
    if (elem.children[0].IsReg() && elem.children[0].RegNo() == regno) {
      *killed = true;
    }
  } else if (elem.ContainsReg(regno)) {
    return true;
  }
  if (has_call && (regno < 16 || regno == 24 || regno == 25 || regno == 31)) {
    *killed = true;
  }
  return false;
}

bool IsRegLiveInAnySucc(const RtlControlFlowGraph& cfg, size_t b, int regno) {
  const size_t n_blocks = cfg.rpo_blocks.size();
  std::vector<bool> visited(n_blocks, false);
  std::vector<size_t> worklist;
  for (size_t succ : cfg.rpo_blocks[b].successors) {
    if (succ < n_blocks && !visited[succ]) {
      visited[succ] = true;
      worklist.push_back(succ);
    }
  }
  while (!worklist.empty()) {
    const size_t cur = worklist.back();
    worklist.pop_back();
    const RtlBasicBlock& blk = cfg.rpo_blocks[cur];
    bool killed = false;
    auto check_pn = [&](const RtlPatternAndNotes& pn) -> bool {
      for (const RtlExpr& fu : pn.call_fusage) {
        if (fu.op == RtlOp::kUse && fu.ContainsReg(regno)) {
          return true;
        }
      }
      if (pn.pattern.op == RtlOp::kSequence) {
        for (const RtlExpr& sub : pn.pattern.children) {
          if (!sub.children.empty()) {
            if (ElemReadsRegBeforeKill(sub.children[0], regno, &killed)) {
              return true;
            }
            if (killed) {
              return false;
            }
          }
        }
        return false;
      }
      return ElemReadsRegBeforeKill(pn.pattern, regno, &killed);
    };
    const bool has_epilogue_delay = blk.successors.empty() &&
                                    cfg.sideband.epilogue_delay_pattern.has_value() &&
                                    !cfg.sideband.epilogue_delay_pattern->empty();
    bool has_trailing_return_seq = false;
    if (blk.successors.empty() && !blk.instructions.empty()) {
      auto back_p = ParsePatternAndNotes(blk.instructions.back());
      if (back_p.ok() && back_p->pattern.op == RtlOp::kSequence &&
          back_p->pattern.children.size() >= 2 &&
          back_p->pattern.children[0].op == RtlOp::kJumpInsn &&
          !back_p->pattern.children[0].flags.unchanging) {
        has_trailing_return_seq = true;
      }
    }
    const bool defer_exit_return_use = has_epilogue_delay || has_trailing_return_seq;
    auto is_exit_return_use = [](const RtlPatternAndNotes& pn) {
      if (pn.pattern.op != RtlOp::kUse || pn.pattern.children.size() != 1 ||
          !pn.pattern.children[0].IsReg()) {
        return false;
      }
      const RtlExpr& reg = pn.pattern.children[0];
      const int r = reg.RegNo();
      return reg.flags.integrated || r == 2 || r == 3 || r == 32 || r == 33;
    };
    for (const std::string& s : blk.instructions) {
      auto parsed = ParsePatternAndNotes(s);
      if (!parsed.ok() || (defer_exit_return_use && is_exit_return_use(*parsed))) {
        continue;
      }
      if (check_pn(*parsed)) {
        return true;
      }
      if (killed) {
        break;
      }
    }
    if (!killed && has_epilogue_delay) {
      auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
      if (ep.ok() && check_pn(*ep)) {
        return true;
      }
    }
    if (!killed && defer_exit_return_use) {
      for (const std::string& s : blk.instructions) {
        auto parsed = ParsePatternAndNotes(s);
        if (!parsed.ok() || !is_exit_return_use(*parsed)) {
          continue;
        }
        if (check_pn(*parsed)) {
          return true;
        }
        if (killed) {
          break;
        }
      }
    }
    if (!killed) {
      for (size_t s_blk : blk.successors) {
        if (s_blk < n_blocks && !visited[s_blk]) {
          visited[s_blk] = true;
          worklist.push_back(s_blk);
        }
      }
    }
  }
  return false;
}

void CollectReadRegs(const RtlExpr& expr, std::vector<int>* out) {
  auto add_reg = [out](const RtlExpr& node) {
    if (node.IsReg()) {
      const int r = node.RegNo();
      out->push_back(r);
      if (node.mode == RtlMachineMode::kDi || node.mode == RtlMachineMode::kDf) {
        out->push_back(r + 1);
      }
    } else if (node.IsSubreg() && !node.children.empty() && node.children[0].IsReg()) {
      out->push_back(node.children[0].RegNo() + static_cast<int>(node.int_val));
    }
  };
  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& child : expr.children) {
      CollectReadRegs(child, out);
    }
    return;
  }
  if (expr.op == RtlOp::kClobber || expr.op == RtlOp::kNote) {
    return;
  }
  if (expr.IsSet()) {
    if (expr.SetDest().IsMem()) {
      expr.SetDest().Visit([&add_reg](const RtlExpr& n) { add_reg(n); });
    }
    expr.SetSrc().Visit([&add_reg](const RtlExpr& n) { add_reg(n); });
    return;
  }
  expr.Visit([&add_reg](const RtlExpr& n) { add_reg(n); });
}

bool IsRootMultiCycleProducerExpr(const RtlExpr& pat) {
  if (IsMemLoadExpr(pat)) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    bool has_imul_or_idiv = false;
    pat.Visit([&has_imul_or_idiv](const RtlExpr& n) {
      if (n.op == RtlOp::kMult || n.op == RtlOp::kDiv || n.op == RtlOp::kUdiv ||
          n.op == RtlOp::kMod || n.op == RtlOp::kUmod) {
        has_imul_or_idiv = true;
      }
    });
    return has_imul_or_idiv;
  }
  if (pat.IsSet() &&
      (pat.SetDest().mode == RtlMachineMode::kSf || pat.SetDest().mode == RtlMachineMode::kDf)) {
    const RtlOp op = pat.SetSrc().op;
    return op == RtlOp::kPlus || op == RtlOp::kMinus || op == RtlOp::kMult || op == RtlOp::kDiv ||
           op == RtlOp::kSqrt || op == RtlOp::kFloat || op == RtlOp::kFloatExtend ||
           op == RtlOp::kFloatTruncate;
  }
  return false;
}

bool IsAnyMultiCycleProducerExpr(const RtlExpr& pat) {
  return IsRootMultiCycleProducerExpr(pat) || IsMipsXferExpr(pat) || IsMipsHiloExpr(pat) ||
         IsMipsFcmpExpr(pat);
}

bool SlotConsumesMultiCycleProducerInBlock(const std::vector<std::string>& new_insns,
                                           size_t limit_pos, const RtlExpr& slot_pat) {
  std::vector<int> read_regs;
  CollectReadRegs(slot_pat, &read_regs);
  for (int r : read_regs) {
    if (r <= 0 || r == 29 || r >= 67) {
      continue;
    }
    for (size_t k = std::min(limit_pos, new_insns.size()); k > 0; --k) {
      auto pk = ParsePatternAndNotes(new_insns[k - 1]);
      if (!pk.ok()) {
        continue;
      }
      std::vector<int> defs;
      CollectDefinedRegs(pk->pattern, &defs);
      if (std::find(defs.begin(), defs.end(), r) != defs.end()) {
        if (IsAnyMultiCycleProducerExpr(pk->pattern)) {
          return true;
        }
        break;
      }
    }
  }
  return false;
}

size_t StepIntoSched2LatencyShadow(const std::vector<std::string>& new_insns, size_t cur_pos,
                                   size_t orig_insert_pos, const RtlPatternAndNotes& slot_pat,
                                   const RtlExpr& head_expr, bool gp64, bool fp64) {
  if (cur_pos == 0 ||
      SlotConsumesMultiCycleProducerInBlock(new_insns, orig_insert_pos, slot_pat.pattern)) {
    return cur_pos;
  }
  if (!IsMipsXferExpr(slot_pat.pattern) && !IsMipsHiloExpr(slot_pat.pattern)) {
    while (cur_pos > 0) {
      auto prev = ParsePatternAndNotes(new_insns[cur_pos - 1]);
      if (!prev.ok() || (!IsMipsXferExpr(prev->pattern) && !IsMipsHiloExpr(prev->pattern)) ||
          !CanPlaceUnpackedSlotBeforeIntervening(new_insns, cur_pos - 1, orig_insert_pos, slot_pat,
                                                 head_expr, gp64, fp64)) {
        break;
      }
      --cur_pos;
    }
  }
  if (cur_pos == 0) {
    return cur_pos;
  }

  std::vector<RtlExpr> parsed_pats(cur_pos);
  std::optional<size_t> last_root_idx;
  for (size_t k = 0; k < cur_pos; ++k) {
    auto pk = ParsePatternAndNotes(new_insns[k]);
    if (!pk.ok()) {
      return cur_pos;
    }
    parsed_pats[k] = std::move(pk->pattern);
    if (IsRootMultiCycleProducerExpr(parsed_pats[k])) {
      last_root_idx = k;
    }
  }
  if (!last_root_idx.has_value()) {
    return cur_pos;
  }

  size_t root_cluster_start = *last_root_idx;
  while (root_cluster_start > 0 && IsMemLoadExpr(parsed_pats[root_cluster_start])) {
    if (IsMemLoadExpr(parsed_pats[root_cluster_start - 1])) {
      --root_cluster_start;
      continue;
    }
    if (root_cluster_start >= 2 &&
        !IsAnyMultiCycleProducerExpr(parsed_pats[root_cluster_start - 1]) &&
        IsMemLoadExpr(parsed_pats[root_cluster_start - 2])) {
      root_cluster_start -= 2;
      continue;
    }
    break;
  }

  std::bitset<68> tainted;
  std::vector<bool> is_delayed(cur_pos, false);
  for (size_t k = root_cluster_start; k < cur_pos; ++k) {
    const bool is_seed = (k <= *last_root_idx && IsRootMultiCycleProducerExpr(parsed_pats[k]));
    bool reads_tainted = false;
    if (!is_seed) {
      std::vector<int> reads;
      CollectReadRegs(parsed_pats[k], &reads);
      for (int r : reads) {
        if (r > 0 && r < 68 && r != 29 && tainted.test(static_cast<size_t>(r))) {
          reads_tainted = true;
          break;
        }
      }
    }
    is_delayed[k] = reads_tainted;
    std::vector<int> defs;
    CollectDefinedRegs(parsed_pats[k], &defs);
    for (int d : defs) {
      if (d > 0 && d < 68 && d != 29) {
        if (is_seed || reads_tainted) {
          tainted.set(static_cast<size_t>(d));
        } else {
          tainted.reset(static_cast<size_t>(d));
        }
      }
    }
  }

  while (cur_pos > *last_root_idx + 1 && is_delayed[cur_pos - 1]) {
    if (!CanPlaceUnpackedSlotBeforeIntervening(new_insns, cur_pos - 1, orig_insert_pos, slot_pat,
                                               head_expr, gp64, fp64)) {
      break;
    }
    --cur_pos;
  }
  return cur_pos;
}

bool SlotConsumesUnsatisfiedMultiCycleProducerAtPos(const std::vector<std::string>& new_insns,
                                                    size_t limit_pos, size_t cand_pos,
                                                    const RtlExpr& slot_pat) {
  std::vector<int> read_regs;
  CollectReadRegs(slot_pat, &read_regs);
  for (int r : read_regs) {
    if (r <= 0 || r == 29 || r >= 67) {
      continue;
    }
    for (size_t k = std::min(limit_pos, new_insns.size()); k > 0; --k) {
      auto pk = ParsePatternAndNotes(new_insns[k - 1]);
      if (!pk.ok()) {
        continue;
      }
      std::vector<int> defs;
      CollectDefinedRegs(pk->pattern, &defs);
      if (std::find(defs.begin(), defs.end(), r) != defs.end()) {
        if (IsAnyMultiCycleProducerExpr(pk->pattern)) {
          // A 2-cycle `xfer` (`li.s`/`mtc1`/`mfc1`, `mips.md:define_function_unit`)
          // has its latency fully covered once at least 1 active instruction
          // already separates it from `cand_pos` (`cand_pos >= p_idx + 2`).
          const size_t p_idx = k - 1;
          if (IsMipsXferExpr(pk->pattern) && !IsRootMultiCycleProducerExpr(pk->pattern) &&
              cand_pos >= p_idx + 2) {
            break;
          }
          return true;
        }
        break;
      }
    }
  }
  return false;
}

size_t FindConditionChainInsertPos(const RtlControlFlowGraph& cfg, size_t b,
                                   const std::vector<std::string>& new_insns,
                                   size_t orig_insert_pos, const RtlPatternAndNotes& slot_pat,
                                   const RtlExpr& head_expr, bool runs_sched2, bool gp64,
                                   bool fp64) {
  const RtlExpr* ite = GetCondJumpIfThenElse(head_expr);
  if (ite == nullptr || orig_insert_pos == 0) {
    return orig_insert_pos;
  }
  size_t best_pos = orig_insert_pos;
  auto prev_parsed = ParsePatternAndNotes(new_insns[orig_insert_pos - 1]);
  if (prev_parsed.ok() && prev_parsed->pattern.IsSet() && prev_parsed->pattern.SetDest().IsReg() &&
      !(runs_sched2 && IsMipsFcmpExpr(prev_parsed->pattern)) &&
      (prev_parsed->pattern.SetSrc().IsComparison() ||
       (runs_sched2 && prev_parsed->pattern.SetSrc().IsConstInt() &&
        prev_parsed->pattern.SetDest().RegNo() >= 1 &&
        prev_parsed->pattern.SetDest().RegNo() < 32 &&
        !IsRegLiveInAnySucc(cfg, b, prev_parsed->pattern.SetDest().RegNo()))) &&
      ite->children[0].ContainsReg(prev_parsed->pattern.SetDest().RegNo()) &&
      !ExprHasMem(prev_parsed->pattern) &&
      !ExpressionsConflict(prev_parsed->pattern, slot_pat.pattern) &&
      !RegNotesConflictWithExpr(slot_pat.reg_notes, prev_parsed->pattern) &&
      !RegNotesConflictWithExpr(prev_parsed->reg_notes, slot_pat.pattern)) {
    best_pos = orig_insert_pos - 1;
  }

  absl::flat_hash_set<int> cond_chain_regs;
  ite->children[0].Visit([&](const RtlExpr& n) {
    if (n.IsReg() && n.RegNo() >= 1 && n.RegNo() < 32 && !IsRegLiveInAnySucc(cfg, b, n.RegNo())) {
      cond_chain_regs.insert(n.RegNo());
    }
  });
  if (!cond_chain_regs.empty()) {
    size_t scan_pos = orig_insert_pos;
    while (scan_pos > 0) {
      auto cand = ParsePatternAndNotes(new_insns[scan_pos - 1]);
      if (!cand.ok() || !cand->pattern.IsSet() || !cand->pattern.SetDest().IsReg()) {
        break;
      }
      // Without `sched2`, `reload` (`reload1.c:6239`) emits stack input reloads
      // immediately before the comparison insn that consumes them; do not split
      // an adjacent `[sp/fp+off]` input reload from its condition-chain consumer.
      bool is_non_sched2_stack_input_reload = false;
      if (!runs_sched2 && cand->pattern.SetSrc().IsMem() && !cand->pattern.SetSrc().flags.volatil &&
          cand->pattern.SetSrc().children.size() == 1) {
        const RtlExpr& addr = cand->pattern.SetSrc().children[0];
        is_non_sched2_stack_input_reload =
            (addr.IsReg() && (addr.RegNo() == 29 || addr.RegNo() == 30)) ||
            (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsReg() &&
             (addr.children[0].RegNo() == 29 || addr.children[0].RegNo() == 30) &&
             addr.children[1].IsConstInt());
      }
      if ((runs_sched2 && IsMipsFcmpExpr(cand->pattern)) ||
          (ExprHasMem(cand->pattern) && !is_non_sched2_stack_input_reload) ||
          !(IsPureBranchCondChainSetSrc(cand->pattern.SetSrc()) ||
            is_non_sched2_stack_input_reload ||
            (runs_sched2 && cand->pattern.SetSrc().IsConstInt()))) {
        break;
      }
      const int def_r = cand->pattern.SetDest().RegNo();
      if (def_r < 1 || def_r >= 32 || !cond_chain_regs.contains(def_r) ||
          IsRegLiveInAnySucc(cfg, b, def_r)) {
        break;
      }
      if (!CanPlaceUnpackedSlotBeforeIntervening(new_insns, scan_pos - 1, orig_insert_pos, slot_pat,
                                                 head_expr, gp64, fp64)) {
        break;
      }
      --scan_pos;
      best_pos = scan_pos;
      cand->pattern.SetSrc().Visit([&](const RtlExpr& n) {
        if (n.IsReg() && n.RegNo() >= 1 && n.RegNo() < 32 &&
            !IsRegLiveInAnySucc(cfg, b, n.RegNo())) {
          cond_chain_regs.insert(n.RegNo());
        }
      });
    }
  }
  if (runs_sched2 && best_pos < orig_insert_pos &&
      SlotConsumesUnsatisfiedMultiCycleProducerAtPos(new_insns, orig_insert_pos, best_pos,
                                                     slot_pat.pattern)) {
    return orig_insert_pos;
  }
  return best_pos;
}

bool IsCalleeSavedGprRegno(int r) {
  return (r >= 16 && r <= 23) || r == 30;
}

bool IsCalleeSavedFprRegno(int r) {
  return r >= 52 && r <= 63;
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

void StripNewlyFoldedEarlyclobberDiOutputReloadDeadNotes(
    const std::vector<std::vector<bool>>& is_newly_folded, RtlControlFlowGraph* cfg) {
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    if (b >= is_newly_folded.size() || blk.instructions.size() != is_newly_folded[b].size()) {
      continue;
    }
    std::vector<RtlPatternAndNotes> pns(blk.instructions.size());
    bool all_ok = true;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        all_ok = false;
        break;
      }
      if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
          !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = blk.rtl_insns[i].call_fusage;
      }
      pns[i] = std::move(*parsed);
    }
    if (!all_ok) {
      continue;
    }

    auto collect_di_gpr_uses_defs = [](const RtlPatternAndNotes& pn, uint64_t* uses, uint64_t* defs,
                                       bool* is_call, bool* is_ctrl) {
      *uses = 0;
      *defs = 0;
      *is_call = false;
      *is_ctrl = false;
      const RtlExpr& pat = pn.pattern;
      pat.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kCall) {
          *is_call = true;
        }
        if (n.op == RtlOp::kReturn || (n.IsSet() && n.SetDest().op == RtlOp::kPc)) {
          *is_ctrl = true;
        }
      });
      auto mark_reg = [](const RtlExpr& r, uint64_t* mask) {
        if (!r.IsHardReg()) {
          return;
        }
        const int rn = r.RegNo();
        const int nw = MipsHardRegNregs(rn, r.mode, /*gp64=*/false, /*fp64=*/false);
        for (int w = 0; w < nw; ++w) {
          if (rn + w >= 0 && rn + w < 64) {
            *mask |= (uint64_t{1} << (rn + w));
          }
        }
      };
      auto walk = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
        if (e.op == RtlOp::kMem) {
          for (const RtlExpr& ch : e.children) {
            self(self, ch, false);
          }
          return;
        }
        if (e.op == RtlOp::kSet) {
          self(self, e.SetSrc(), false);
          self(self, e.SetDest(), true);
          return;
        }
        if (e.op == RtlOp::kClobber) {
          if (!e.children.empty()) {
            mark_reg(e.children[0], defs);
          }
          return;
        }
        if (e.IsHardReg()) {
          mark_reg(e, in_dst ? defs : uses);
          return;
        }
        for (const RtlExpr& ch : e.children) {
          self(self, ch, in_dst);
        }
      };
      walk(walk, pat, false);
      if (*is_call) {
        for (int r : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31}) {
          *defs |= (uint64_t{1} << r);
        }
        for (const RtlExpr& fu : pn.call_fusage) {
          walk(walk, fu, false);
        }
      }
    };

    const size_t n = pns.size();
    std::vector<uint64_t> uses_v(n, 0), defs_v(n, 0);
    std::vector<bool> call_v(n, false), ctrl_v(n, false);
    for (size_t i = 0; i < n; ++i) {
      bool is_call = false;
      bool is_ctrl = false;
      collect_di_gpr_uses_defs(pns[i], &uses_v[i], &defs_v[i], &is_call, &is_ctrl);
      call_v[i] = is_call;
      ctrl_v[i] = is_ctrl;
    }
    uint64_t live_out = 0;
    for (size_t succ : blk.successors) {
      const auto it = cfg->sideband.basic_block_live_at_start.find(static_cast<int>(succ));
      if (it != cfg->sideband.basic_block_live_at_start.end()) {
        for (int r : it->second) {
          if (r >= 0 && r < 64) {
            live_out |= (uint64_t{1} << r);
          }
        }
      }
    }
    std::vector<uint64_t> live_after(n, 0);
    uint64_t cur = live_out;
    for (size_t rev = n; rev > 0; --rev) {
      const size_t i = rev - 1;
      live_after[i] = cur;
      cur = (cur & ~defs_v[i]) | uses_v[i];
    }

    bool blk_modified = false;
    for (size_t s = 1; s < n; ++s) {
      const RtlExpr& mv = pns[s].pattern;
      if (!mv.IsSet() || !mv.SetDest().IsHardReg() || mv.SetDest().mode != RtlMachineMode::kDi ||
          !mv.SetSrc().IsHardReg() || mv.SetSrc().mode != RtlMachineMode::kDi) {
        continue;
      }
      const int dst_hr = mv.SetDest().RegNo();
      const int scr_hr = mv.SetSrc().RegNo();
      if (dst_hr < 2 || dst_hr > 24 || scr_hr < 2 || scr_hr > 24 || dst_hr == scr_hr) {
        continue;
      }
      const uint64_t scr_m = (uint64_t{3} << scr_hr);
      if ((live_after[s] & scr_m) != 0) {
        continue;
      }
      for (size_t rev = s; rev > 0; --rev) {
        const size_t p = rev - 1;
        if ((defs_v[p] & scr_m) != 0) {
          const RtlExpr& pp = pns[p].pattern;
          if (pp.op == RtlOp::kParallel && pp.children.size() >= 2 && pp.children[0].IsSet() &&
              pp.children[1].op == RtlOp::kClobber && pp.children[0].SetDest().IsHardReg() &&
              pp.children[0].SetDest().mode == RtlMachineMode::kDi &&
              pp.children[0].SetDest().RegNo() == scr_hr && (uses_v[p] & scr_m) == 0) {
            bool clobber_overlaps_dst = false;
            for (size_t c = 1; c < pp.children.size(); ++c) {
              if (pp.children[c].op == RtlOp::kClobber && !pp.children[c].children.empty() &&
                  pp.children[c].children[0].IsHardReg()) {
                const int chr = pp.children[c].children[0].RegNo();
                const int cnw =
                    MipsHardRegNregs(chr, pp.children[c].children[0].mode, false, false);
                if (std::max(dst_hr, chr) < std::min(dst_hr + 2, chr + cnw)) {
                  clobber_overlaps_dst = true;
                }
              }
            }
            const RtlExpr& rhs = pp.children[0].SetSrc();
            const bool plus_imm_same_dst =
                rhs.op == RtlOp::kPlus && rhs.children.size() == 2 && rhs.children[0].IsHardReg() &&
                rhs.children[0].RegNo() == dst_hr && rhs.children[1].IsConstInt();
            const uint64_t dst_m = (uint64_t{3} << dst_hr);
            if (!clobber_overlaps_dst &&
                ((rhs.op == RtlOp::kPlus && !plus_imm_same_dst) || rhs.op == RtlOp::kMinus ||
                 rhs.op == RtlOp::kAshift || rhs.op == RtlOp::kAshiftrt ||
                 rhs.op == RtlOp::kLshiftrt) &&
                (uses_v[p] & dst_m) != 0) {
              auto& notes = pns[s].reg_notes;
              const size_t before = notes.size();
              notes.erase(std::remove_if(notes.begin(), notes.end(),
                                         [scr_hr](const RtlRegNote& rn) {
                                           return rn.kind == RtlMachineMode::kRegDead &&
                                                  rn.datum.IsReg() && rn.datum.RegNo() == scr_hr;
                                         }),
                          notes.end());
              if (notes.size() != before) {
                blk.instructions[s] = FormatGccPrintRtlPatternAndNotes(
                    pns[s].pattern, pns[s].reg_notes, pns[s].bracketed_notes, /*call_fusage=*/{});
                blk_modified = true;
              }
            }
          }
          break;
        }
        if ((uses_v[p] & scr_m) != 0 || call_v[p] || ctrl_v[p]) {
          break;
        }
      }
    }
    if (blk_modified) {
      blk.SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < blk.rtl_insns.size() && i < pns.size(); ++i) {
        if (pns[i].has_explicit_call_fusage && !blk.rtl_insns[i].has_explicit_call_fusage) {
          blk.rtl_insns[i].has_explicit_call_fusage = true;
          blk.rtl_insns[i].call_fusage = pns[i].call_fusage;
        }
      }
    }
  }
}

void RecombineReorgSplitIdioms(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                               std::optional<size_t> unpacked_epilogue_delay_idx) {
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
    for (size_t i = 0; i < cfg->rpo_blocks[b].instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(cfg->rpo_blocks[b].instructions[i]);
      if (!parsed.ok()) {
        return;
      }
      if (i < cfg->rpo_blocks[b].rtl_insns.size() &&
          cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage &&
          !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = cfg->rpo_blocks[b].rtl_insns[i].call_fusage;
      }
      parsed_blocks[b].push_back(std::move(*parsed));
    }
  }
  bool has_unpacked_epilogue_delay = false;
  if (unpacked_epilogue_delay_idx.has_value() &&
      *unpacked_epilogue_delay_idx < parsed_blocks.back().size() &&
      parsed_blocks.back()[*unpacked_epilogue_delay_idx].reg_notes.empty()) {
    parsed_blocks.back()[*unpacked_epilogue_delay_idx].reg_notes.push_back(
        RtlRegNote{.kind = RtlMachineMode::kVoid, .datum = RtlExpr::MakeConstInt(0)});
    has_unpacked_epilogue_delay = true;
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

  if (!recombined_split_movstrsi && !folded_multi_insn && !has_unpacked_epilogue_delay) {
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
      std::string rendered = FormatGccPrintRtlPatternAndNotes(
          pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{});
      preserved_insns[b].push_back(rendered);
      cfg->rpo_blocks[b].instructions.push_back(std::move(rendered));
    }
    cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < cfg->rpo_blocks[b].rtl_insns.size() && i < parsed_blocks[b].size();
         ++i) {
      if (parsed_blocks[b][i].has_explicit_call_fusage &&
          !cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage) {
        cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage = true;
        cfg->rpo_blocks[b].rtl_insns[i].call_fusage = parsed_blocks[b][i].call_fusage;
      }
    }
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

  if (folded_multi_insn && !gp64) {
    StripNewlyFoldedEarlyclobberDiOutputReloadDeadNotes(is_newly_folded, cfg);
  }
  NormalizePostReloadHardRegNotes(cfg, profile);
  if (folded_multi_insn) {
    if (flags.opt_level > 0) {
      RestoreNoConflictExtendClobbers(cfg, gp64, fp64, flags.RunsSched2());
    }
    SynthesizePostReloadMemRegEquivNotes(cfg, profile);
    // `toplev.c:3028-3034, 3105-3118`, `sched.c:2779-3018, 3415-3611`:
    // `ReconstructRtlLiveness` above rebuilt `REG_DEAD` after folding
    // delay-slot-split multi-insn idioms (such as a trailing `adddi3` split
    // across `j $31`); re-run `sched2` pure-reader / `combine_regs` `REG_DEAD`
    // attribution and `sched` note ordering just like Stage 0
    // (`InvertFinalScanInsnLinesToCfg`).
    if (flags.RunsSched1()) {
      if (flags.RunsSched2()) {
        sched_model::AttributeSched2PureReaderRegDeadNotes(cfg, profile);
      }
      CanonicalizeCfgLivenessNoteOrder(cfg, profile, /*sched_note_order=*/true,
                                       /*reload_completed=*/true);
    }
  }

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
        for (size_t i = 0; i < cfg->rpo_blocks[b].rtl_insns.size() && i < parsed_blocks[b].size();
             ++i) {
          if (parsed_blocks[b][i].has_explicit_call_fusage &&
              !cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage) {
            cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage = true;
            cfg->rpo_blocks[b].rtl_insns[i].call_fusage = parsed_blocks[b][i].call_fusage;
          }
        }
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

enum class MipsSchedCpuKind {
  kR3000,
  kR6000,
  kR4600,
  kR4000,
};

MipsSchedCpuKind ClassifyMipsSchedCpu(const ParsedCompilerFlags& flags) {
  std::optional<std::string_view> explicit_mcpu;
  for (const std::string& tok : flags.raw_tokens) {
    const std::string_view sv = tok;
    if (sv.substr(0, 6) == "-mcpu=") {
      explicit_mcpu = sv.substr(6);
    }
  }
  if (explicit_mcpu.has_value()) {
    const std::string_view cpu = *explicit_mcpu;
    if (cpu == "r3000" || cpu == "R3000" || cpu == "3000" || cpu == "r2000" || cpu == "R2000" ||
        cpu == "2000") {
      return MipsSchedCpuKind::kR3000;
    }
    if (cpu == "r6000" || cpu == "R6000" || cpu == "6000") {
      return MipsSchedCpuKind::kR6000;
    }
    if (cpu == "r4600" || cpu == "R4600" || cpu == "4600" || cpu == "orion" || cpu == "r4650" ||
        cpu == "R4650" || cpu == "4650") {
      return MipsSchedCpuKind::kR4600;
    }
    return MipsSchedCpuKind::kR4000;
  }
  if (flags.mips_isa == 1) {
    return MipsSchedCpuKind::kR3000;
  }
  if (flags.mips_isa == 2) {
    return MipsSchedCpuKind::kR6000;
  }
  return MipsSchedCpuKind::kR4000;
}

size_t MipsLoadShadowSlots(MipsSchedCpuKind cpu) {
  return (cpu == MipsSchedCpuKind::kR3000 || cpu == MipsSchedCpuKind::kR4600) ? 1 : 2;
}

size_t MipsFpProducerShadowSlots(const RtlExpr& p, MipsSchedCpuKind cpu) {
  if (IsMemLoadExpr(p)) {
    return MipsLoadShadowSlots(cpu);
  }
  if (IsMipsXferExpr(p)) {
    return 1;
  }
  if (!p.IsSet() ||
      (p.SetDest().mode != RtlMachineMode::kSf && p.SetDest().mode != RtlMachineMode::kDf)) {
    return 0;
  }
  const RtlOp op = p.SetSrc().op;
  if (op == RtlOp::kPlus || op == RtlOp::kMinus) {
    if (cpu == MipsSchedCpuKind::kR3000) return 1;
    if (cpu == MipsSchedCpuKind::kR6000) return 2;
    return 3;
  }
  if (op == RtlOp::kMult) {
    if (p.SetDest().mode == RtlMachineMode::kSf) {
      if (cpu == MipsSchedCpuKind::kR3000) return 3;
      if (cpu == MipsSchedCpuKind::kR6000) return 4;
      if (cpu == MipsSchedCpuKind::kR4600) return 7;
      return 6;
    }
    if (cpu == MipsSchedCpuKind::kR3000) return 4;
    if (cpu == MipsSchedCpuKind::kR6000) return 5;
    return 7;
  }
  return 0;
}

bool HasDeletedHigherArgNoopCallMoveInEntryBlock(const RtlControlFlowGraph& out,
                                                 const std::vector<RtlExpr>& parsed_pats,
                                                 const RtlExpr& slot_pat) {
  if (out.rpo_blocks.empty() || !slot_pat.IsSet() || !slot_pat.SetDest().IsReg()) {
    return false;
  }
  const int slot_dst = slot_pat.SetDest().RegNo();
  if (slot_dst < 4 || slot_dst > 6) {
    return false;
  }
  std::optional<size_t> first_call_raw_idx;
  for (size_t idx = 0; idx < out.rpo_blocks[0].instructions.size(); ++idx) {
    auto p = ParsePatternAndNotes(out.rpo_blocks[0].instructions[idx]);
    if (!p.ok()) {
      continue;
    }
    bool has_call = false;
    p->pattern.Visit([&has_call](const RtlExpr& n) {
      if (n.op == RtlOp::kCall) {
        has_call = true;
      }
    });
    if (has_call) {
      first_call_raw_idx = idx;
      break;
    }
  }
  if (!first_call_raw_idx.has_value()) {
    return false;
  }
  auto is_read_after_first_call = [&](int sr) -> bool {
    auto insn_reads_sr = [sr](const std::string& s) -> bool {
      auto p = ParsePatternAndNotes(s);
      if (!p.ok()) {
        return false;
      }
      if (p->pattern.op == RtlOp::kSequence && p->pattern.children.size() == 2) {
        for (const RtlExpr& sub : p->pattern.children) {
          auto sp = RtlPatternAndNotes::FromSequenceSubInsn(sub);
          if (!sp.ok()) {
            continue;
          }
          std::vector<int> rds;
          CollectReadRegs(sp->pattern, &rds);
          if (std::find(rds.begin(), rds.end(), sr) != rds.end()) {
            return true;
          }
        }
        return false;
      }
      std::vector<int> rds;
      CollectReadRegs(p->pattern, &rds);
      return std::find(rds.begin(), rds.end(), sr) != rds.end();
    };
    for (size_t idx = *first_call_raw_idx + 1; idx < out.rpo_blocks[0].instructions.size(); ++idx) {
      if (insn_reads_sr(out.rpo_blocks[0].instructions[idx])) {
        return true;
      }
    }
    for (size_t b = 1; b < out.rpo_blocks.size(); ++b) {
      for (const std::string& s : out.rpo_blocks[b].instructions) {
        if (insn_reads_sr(s)) {
          return true;
        }
      }
    }
    return false;
  };
  for (size_t c_idx = 0; c_idx < parsed_pats.size(); ++c_idx) {
    const RtlExpr& p = parsed_pats[c_idx];
    if (!p.IsSet() || !p.SetDest().IsReg() || !p.SetSrc().IsReg() ||
        p.SetDest().mode != RtlMachineMode::kSi || p.SetSrc().mode != RtlMachineMode::kSi) {
      continue;
    }
    const int sr = p.SetDest().RegNo();
    const int ar = p.SetSrc().RegNo();
    if (!IsCalleeSavedGprRegno(sr) || ar <= slot_dst || ar > 7 || slot_pat.ContainsReg(sr) ||
        slot_pat.ContainsReg(ar)) {
      continue;
    }
    bool valid = true;
    for (size_t k = 0; k < c_idx; ++k) {
      std::vector<int> defs;
      CollectDefinedRegs(parsed_pats[k], &defs);
      if (std::find(defs.begin(), defs.end(), ar) != defs.end()) {
        valid = false;
        break;
      }
    }
    for (size_t k = c_idx + 1; valid && k < parsed_pats.size(); ++k) {
      if (parsed_pats[k].ContainsReg(sr) || parsed_pats[k].ContainsReg(ar)) {
        valid = false;
      }
    }
    if (valid && is_read_after_first_call(sr)) {
      return true;
    }
  }
  return false;
}

size_t MaybeStepBeforeUnshadowedLoadStorePair(const std::vector<std::string>& new_insns,
                                              size_t slot_insert_pos, size_t orig_insert_pos,
                                              const RtlPatternAndNotes& slot_pat,
                                              const RtlExpr& head_expr, bool gp64, bool fp64) {
  if (slot_insert_pos < 2 || IsRootMultiCycleProducerExpr(slot_pat.pattern) ||
      SlotConsumesMultiCycleProducerInBlock(new_insns, orig_insert_pos, slot_pat.pattern)) {
    return slot_insert_pos;
  }
  auto prev_parsed = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
  auto load_parsed = ParsePatternAndNotes(new_insns[slot_insert_pos - 2]);
  if (!prev_parsed.ok() || !load_parsed.ok() || !prev_parsed->pattern.IsSet() ||
      !prev_parsed->pattern.SetDest().IsMem() ||
      !IsIneligibleForMipsDelaySlot(prev_parsed->pattern, gp64, fp64) ||
      !IsMemLoadExpr(load_parsed->pattern) || !load_parsed->pattern.SetDest().IsReg() ||
      !prev_parsed->pattern.ContainsReg(load_parsed->pattern.SetDest().RegNo()) ||
      !CanPlaceUnpackedSlotBeforeIntervening(new_insns, slot_insert_pos - 1, orig_insert_pos,
                                             slot_pat, head_expr, gp64, fp64)) {
    return slot_insert_pos;
  }
  std::vector<int> slot_reads;
  CollectReadRegs(slot_pat.pattern, &slot_reads);
  for (int r : slot_reads) {
    if (r <= 0 || r >= 68) {
      continue;
    }
    for (size_t k = 0; k + 1 < slot_insert_pos; ++k) {
      auto pk = ParsePatternAndNotes(new_insns[k]);
      if (!pk.ok()) {
        return slot_insert_pos;
      }
      std::vector<int> kdefs;
      CollectDefinedRegs(pk->pattern, &kdefs);
      if (std::find(kdefs.begin(), kdefs.end(), r) != kdefs.end()) {
        return slot_insert_pos;
      }
    }
  }
  return slot_insert_pos - 1;
}

size_t ComputeSimpleDelaySlotUnpackPos(const RtlControlFlowGraph& out, size_t b, bool is_last_insn,
                                       const RtlExpr& head_sub, const RtlPatternAndNotes& head_pat,
                                       const RtlPatternAndNotes& slot_pat,
                                       const std::vector<std::string>& new_insns,
                                       const ParsedCompilerFlags& flags, bool gp64, bool fp64) {
  const bool runs_sched2 = flags.RunsSched2();
  const MipsSchedCpuKind sched_cpu = ClassifyMipsSchedCpu(flags);
  size_t slot_insert_pos = new_insns.size();
  while (slot_insert_pos > 0) {
    auto prev_parsed = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
    if (prev_parsed.ok() &&
        (prev_parsed->pattern.op == RtlOp::kUse || prev_parsed->pattern.op == RtlOp::kClobber)) {
      --slot_insert_pos;
    } else {
      break;
    }
  }
  const size_t orig_insert_pos = slot_insert_pos;
  if (IsIneligibleForMipsDelaySlot(slot_pat.pattern, gp64, fp64)) {
    return orig_insert_pos;
  }

  if (is_last_insn && head_sub.op == RtlOp::kJumpInsn && IsReturnExpr(head_pat.pattern) &&
      runs_sched2 && slot_insert_pos > 0 && slot_pat.pattern.IsSet() &&
      slot_pat.pattern.SetDest().IsReg() && slot_pat.pattern.SetDest().flags.integrated &&
      !ExprHasMem(slot_pat.pattern)) {
    auto prev_parsed = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
    if (prev_parsed.ok() && prev_parsed->pattern.IsSet() &&
        prev_parsed->pattern.SetDest().IsMem() &&
        IsIneligibleForMipsDelaySlot(prev_parsed->pattern, gp64, fp64) &&
        !ExpressionsConflict(prev_parsed->pattern, slot_pat.pattern)) {
      --slot_insert_pos;
    }
    return slot_insert_pos;
  }

  if (is_last_insn && head_sub.op == RtlOp::kJumpInsn && IsSimpleUncondJumpExpr(head_pat.pattern) &&
      out.rpo_blocks[b].successors.size() == 1 && slot_insert_pos > 0 &&
      !ExprHasMem(slot_pat.pattern)) {
    if (TargetFallthroughPredEndsWithSlotPat(out, b, out.rpo_blocks[b].successors[0],
                                             slot_pat.pattern)) {
      auto prev_parsed = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
      if (prev_parsed.ok() && prev_parsed->pattern.IsSet() &&
          prev_parsed->pattern.SetDest().IsReg() && ExprHasMem(prev_parsed->pattern.SetSrc()) &&
          !ExpressionsConflict(prev_parsed->pattern, slot_pat.pattern)) {
        --slot_insert_pos;
        if (runs_sched2) {
          slot_insert_pos = MaybeStepBeforeUnshadowedLoadStorePair(
              new_insns, slot_insert_pos, orig_insert_pos, slot_pat, head_pat.pattern, gp64, fp64);
        }
        return slot_insert_pos;
      }
    }
    if (runs_sched2) {
      slot_insert_pos = StepIntoSched2LatencyShadow(new_insns, slot_insert_pos, orig_insert_pos,
                                                    slot_pat, head_pat.pattern, gp64, fp64);
      slot_insert_pos = MaybeStepBeforeUnshadowedLoadStorePair(
          new_insns, slot_insert_pos, orig_insert_pos, slot_pat, head_pat.pattern, gp64, fp64);
    }
    return slot_insert_pos;
  }

  int slot_saved_r = -1;
  const bool is_ra_stack_save =
      runs_sched2 && IsSpStackSaveOfReg(slot_pat.pattern, &slot_saved_r) && slot_saved_r == 31;
  if (is_last_insn && head_sub.op == RtlOp::kJumpInsn && IsCondJumpExpr(head_pat.pattern) &&
      slot_insert_pos > 0 && (!ExprHasMem(slot_pat.pattern) || is_ra_stack_save) &&
      !ExpressionsConflict(head_pat.pattern, slot_pat.pattern)) {
    const auto& succs = out.rpo_blocks[b].successors;
    const bool is_self_loop_latch = std::find(succs.begin(), succs.end(), b) != succs.end();
    bool placed_earlier = false;

    if (!ExprHasMem(slot_pat.pattern) && is_self_loop_latch && slot_pat.pattern.IsSet() &&
        slot_pat.pattern.SetDest().IsReg() &&
        slot_pat.pattern.SetDest().mode == RtlMachineMode::kSi &&
        slot_pat.pattern.SetSrc().op == RtlOp::kPlus &&
        slot_pat.pattern.SetSrc().children.size() == 2 &&
        slot_pat.pattern.SetSrc().children[0].IsReg() &&
        slot_pat.pattern.SetSrc().children[1].IsConstInt()) {
      const int ptr_reg = slot_pat.pattern.SetDest().RegNo();
      if (ptr_reg == slot_pat.pattern.SetSrc().children[0].RegNo() && ptr_reg >= 1 &&
          ptr_reg < 32 && ptr_reg != 29 && ptr_reg != 30) {
        std::optional<size_t> last_mem_idx;
        for (size_t k = 0; k < slot_insert_pos; ++k) {
          auto pk = ParsePatternAndNotes(new_insns[k]);
          if (pk.ok() && ExprHasMemReferencingReg(pk->pattern, ptr_reg)) {
            last_mem_idx = k;
          }
        }
        if (last_mem_idx.has_value()) {
          size_t target_idx = *last_mem_idx + 1;
          if (runs_sched2) {
            const size_t load_shadow_cap = MipsLoadShadowSlots(sched_cpu);
            for (size_t k = *last_mem_idx + 1; k < slot_insert_pos; ++k) {
              auto pk = ParsePatternAndNotes(new_insns[k]);
              if (!pk.ok()) {
                break;
              }
              if (!IsMemLoadExpr(pk->pattern) || !pk->pattern.SetDest().IsReg()) {
                continue;
              }
              const int ld_r = pk->pattern.SetDest().RegNo();
              std::optional<size_t> first_cons_idx;
              size_t existing_shadow = 0;
              for (size_t u = k + 1; u < slot_insert_pos; ++u) {
                auto pu = ParsePatternAndNotes(new_insns[u]);
                if (!pu.ok()) {
                  break;
                }
                if (pu->pattern.ContainsReg(ld_r)) {
                  first_cons_idx = u;
                  break;
                }
                const RtlOp uop = pu->pattern.op;
                if (uop != RtlOp::kClobber && uop != RtlOp::kUse && uop != RtlOp::kNote) {
                  ++existing_shadow;
                }
              }
              if (!first_cons_idx.has_value() && head_pat.pattern.ContainsReg(ld_r)) {
                first_cons_idx = slot_insert_pos;
              }
              if (first_cons_idx.has_value() && existing_shadow < load_shadow_cap) {
                target_idx = k + 1;
              }
            }
          }
          if (target_idx < slot_insert_pos &&
              CanPlaceUnpackedSlotBeforeIntervening(new_insns, target_idx, slot_insert_pos,
                                                    slot_pat, head_pat.pattern, gp64, fp64)) {
            slot_insert_pos = target_idx;
            placed_earlier = true;
          }
        }
      }
    }

    if (!placed_earlier && !ExprHasMem(slot_pat.pattern) && b == 0 && !is_self_loop_latch &&
        slot_pat.pattern.IsSet() && slot_pat.pattern.SetDest().IsReg() &&
        slot_pat.pattern.SetSrc().IsReg() &&
        slot_pat.pattern.SetDest().mode == RtlMachineMode::kSi &&
        slot_pat.pattern.SetSrc().mode == RtlMachineMode::kSi) {
      const int dst_reg = slot_pat.pattern.SetDest().RegNo();
      const int src_reg = slot_pat.pattern.SetSrc().RegNo();
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
        bool has_unfilled_load_after_ra = false;
        if (runs_sched2 && ra_save_pos.has_value()) {
          const size_t load_shadow_cap = MipsLoadShadowSlots(sched_cpu);
          for (size_t k = *ra_save_pos + 1; k < slot_insert_pos; ++k) {
            auto pk = ParsePatternAndNotes(new_insns[k]);
            if (!pk.ok() || !IsMemLoadExpr(pk->pattern) || !pk->pattern.SetDest().IsReg()) {
              continue;
            }
            const int ld_r = pk->pattern.SetDest().RegNo();
            std::optional<size_t> first_cons_idx;
            size_t existing_shadow = 0;
            for (size_t u = k + 1; u < slot_insert_pos; ++u) {
              auto pu = ParsePatternAndNotes(new_insns[u]);
              if (!pu.ok()) {
                break;
              }
              if (pu->pattern.ContainsReg(ld_r)) {
                first_cons_idx = u;
                break;
              }
              const RtlOp uop = pu->pattern.op;
              if (uop != RtlOp::kClobber && uop != RtlOp::kUse && uop != RtlOp::kNote) {
                ++existing_shadow;
              }
            }
            if (!first_cons_idx.has_value() && head_pat.pattern.ContainsReg(ld_r)) {
              first_cons_idx = slot_insert_pos;
            }
            if (first_cons_idx.has_value() && existing_shadow < load_shadow_cap) {
              has_unfilled_load_after_ra = true;
              break;
            }
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
          } else if (!(runs_sched2 && has_unfilled_load_after_ra) && dst_reg >= 4 && dst_reg <= 7 &&
                     src_reg >= 4 && src_reg <= 7) {
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
            CanPlaceUnpackedSlotBeforeIntervening(new_insns, *cand_pos, slot_insert_pos, slot_pat,
                                                  head_pat.pattern, gp64, fp64)) {
          slot_insert_pos = *cand_pos;
          placed_earlier = true;
        }
      }
    }

    if (!placed_earlier) {
      if (!(runs_sched2 && ExprHasMem(slot_pat.pattern))) {
        slot_insert_pos = FindConditionChainInsertPos(out, b, new_insns, slot_insert_pos, slot_pat,
                                                      head_pat.pattern, runs_sched2, gp64, fp64);
      }
      if (runs_sched2) {
        slot_insert_pos = StepIntoSched2LatencyShadow(new_insns, slot_insert_pos, orig_insert_pos,
                                                      slot_pat, head_pat.pattern, gp64, fp64);
        if (slot_insert_pos == orig_insert_pos && slot_insert_pos > 0 && slot_pat.pattern.IsSet() &&
            slot_pat.pattern.SetDest().IsReg() && slot_pat.pattern.SetSrc().IsConstInt() &&
            !head_pat.pattern.ContainsReg(slot_pat.pattern.SetDest().RegNo())) {
          auto prev = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
          if (prev.ok() && prev->pattern.op == RtlOp::kParallel &&
              IsRootMultiCycleProducerExpr(prev->pattern)) {
            std::vector<int> prev_defs;
            CollectDefinedRegs(prev->pattern, &prev_defs);
            bool prev_feeds_head = false;
            for (int d : prev_defs) {
              if (d >= 1 && d < 32 && head_pat.pattern.ContainsReg(d)) {
                prev_feeds_head = true;
                break;
              }
            }
            bool prev_has_block_producer = false;
            if (!prev_feeds_head) {
              std::vector<int> prev_reads;
              CollectReadRegs(prev->pattern, &prev_reads);
              for (int r : prev_reads) {
                if (r <= 0 || r == 29 || r >= 67) {
                  continue;
                }
                for (size_t k = 0; k + 1 < slot_insert_pos; ++k) {
                  auto pk = ParsePatternAndNotes(new_insns[k]);
                  if (!pk.ok()) {
                    continue;
                  }
                  std::vector<int> kdefs;
                  CollectDefinedRegs(pk->pattern, &kdefs);
                  if (std::find(kdefs.begin(), kdefs.end(), r) != kdefs.end()) {
                    prev_has_block_producer = true;
                    break;
                  }
                }
                if (prev_has_block_producer) {
                  break;
                }
              }
            }
            if (!prev_feeds_head && prev_has_block_producer &&
                CanPlaceUnpackedSlotBeforeIntervening(new_insns, slot_insert_pos - 1,
                                                      orig_insert_pos, slot_pat, head_pat.pattern,
                                                      gp64, fp64)) {
              --slot_insert_pos;
            }
          }
        }
      }
    }
    return slot_insert_pos;
  }

  if (head_sub.op == RtlOp::kCallInsn && runs_sched2 && slot_insert_pos > 0) {
    bool has_earlier_call = false;
    std::optional<size_t> ra_save_pos;
    std::bitset<68> written_so_far;
    std::vector<RtlExpr> parsed_pats(slot_insert_pos);
    std::vector<bool> is_true_prologue_save(slot_insert_pos, false);
    for (size_t k = 0; k < slot_insert_pos; ++k) {
      auto pk = ParsePatternAndNotes(new_insns[k]);
      if (!pk.ok()) {
        return slot_insert_pos;
      }
      parsed_pats[k] = std::move(pk->pattern);
      parsed_pats[k].Visit([&has_earlier_call](const RtlExpr& n) {
        if (n.op == RtlOp::kCall) {
          has_earlier_call = true;
        }
      });
      int saved_r = -1;
      if (b == 0 && !has_earlier_call && IsSpStackSaveOfReg(parsed_pats[k], &saved_r)) {
        if (saved_r == 31 && !ra_save_pos.has_value()) {
          ra_save_pos = k;
        }
        if (saved_r == 31 || ((IsCalleeSavedGprRegno(saved_r) || IsCalleeSavedFprRegno(saved_r)) &&
                              !written_so_far.test(static_cast<size_t>(saved_r)))) {
          is_true_prologue_save[k] = true;
        }
      }
      std::vector<int> defs;
      CollectDefinedRegs(parsed_pats[k], &defs);
      for (int d : defs) {
        if (d >= 0 && d < 68) {
          written_so_far.set(static_cast<size_t>(d));
        }
      }
    }

    if (b == 0 && !has_earlier_call && ra_save_pos.has_value() && slot_pat.pattern.IsSet() &&
        slot_pat.pattern.SetDest().IsReg()) {
      const int dst_r = slot_pat.pattern.SetDest().RegNo();
      if (IsCalleeSavedGprRegno(dst_r) || IsCalleeSavedFprRegno(dst_r)) {
        for (size_t k = 0; k < *ra_save_pos; ++k) {
          int saved_r = -1;
          if (is_true_prologue_save[k] && IsSpStackSaveOfReg(parsed_pats[k], &saved_r) &&
              saved_r == dst_r) {
            if (k + 1 < slot_insert_pos &&
                CanPlaceUnpackedSlotBeforeIntervening(new_insns, k + 1, orig_insert_pos, slot_pat,
                                                      head_pat.pattern, gp64, fp64)) {
              return k + 1;
            }
            break;
          }
        }
      }
    }

    if (IsRootMultiCycleProducerExpr(slot_pat.pattern) && !IsMemLoadExpr(slot_pat.pattern) &&
        slot_pat.pattern.IsSet() && slot_pat.pattern.SetDest().IsReg() &&
        (slot_pat.pattern.SetDest().RegNo() == 44 || slot_pat.pattern.SetDest().RegNo() == 46)) {
      std::vector<int> slot_reads;
      CollectReadRegs(slot_pat.pattern, &slot_reads);
      size_t min_ready_pos = 0;
      for (int r : slot_reads) {
        if (r <= 0 || r == 29 || r >= 67) {
          continue;
        }
        std::optional<size_t> r_prod_idx;
        for (size_t k = 0; k < slot_insert_pos; ++k) {
          std::vector<int> kdefs;
          CollectDefinedRegs(parsed_pats[k], &kdefs);
          if (std::find(kdefs.begin(), kdefs.end(), r) != kdefs.end()) {
            r_prod_idx = k;
          }
        }
        if (r_prod_idx.has_value()) {
          const size_t delay_shadow =
              MipsFpProducerShadowSlots(parsed_pats[*r_prod_idx], sched_cpu);
          size_t ready_pos = *r_prod_idx + 1;
          size_t shadow_seen = 0;
          while (ready_pos < slot_insert_pos && shadow_seen < delay_shadow) {
            const RtlOp mop = parsed_pats[ready_pos].op;
            if (mop != RtlOp::kClobber && mop != RtlOp::kUse && mop != RtlOp::kNote) {
              ++shadow_seen;
            }
            ++ready_pos;
          }
          min_ready_pos = std::max(min_ready_pos, ready_pos);
        }
      }
      const size_t slot_call_shadow = MipsFpProducerShadowSlots(slot_pat.pattern, sched_cpu);
      if (slot_call_shadow > 0) {
        size_t call_ready_pos = slot_insert_pos;
        size_t shadow_seen = 0;
        while (call_ready_pos > 0 && shadow_seen < slot_call_shadow) {
          --call_ready_pos;
          const RtlOp mop = parsed_pats[call_ready_pos].op;
          if (mop != RtlOp::kClobber && mop != RtlOp::kUse && mop != RtlOp::kNote) {
            ++shadow_seen;
          }
        }
        if (shadow_seen == slot_call_shadow) {
          min_ready_pos = std::max(min_ready_pos, call_ready_pos);
        }
      }
      size_t fp_arg_pos = slot_insert_pos;
      while (fp_arg_pos > min_ready_pos) {
        std::vector<int> pdefs;
        CollectDefinedRegs(parsed_pats[fp_arg_pos - 1], &pdefs);
        bool writes_fpr = false;
        for (int d : pdefs) {
          if (d >= 32 && d <= 66) {
            writes_fpr = true;
            break;
          }
        }
        if (writes_fpr ||
            !CanPlaceUnpackedSlotBeforeIntervening(new_insns, fp_arg_pos - 1, orig_insert_pos,
                                                   slot_pat, head_pat.pattern, gp64, fp64)) {
          break;
        }
        --fp_arg_pos;
      }
      if (fp_arg_pos < slot_insert_pos) {
        return fp_arg_pos;
      }
    }

    const bool slot_consumes_multicycle =
        SlotConsumesMultiCycleProducerInBlock(new_insns, orig_insert_pos, slot_pat.pattern);
    const bool has_deleted_higher_arg_noop_move =
        b == 0 && !has_earlier_call && !slot_consumes_multicycle &&
        HasDeletedHigherArgNoopCallMoveInEntryBlock(out, parsed_pats, slot_pat.pattern);
    bool stepped_multicycle_across_xfer = false;
    while (slot_insert_pos > 0) {
      const RtlExpr& m_pat = parsed_pats[slot_insert_pos - 1];
      int saved_r = -1;
      const bool is_ra_or_prologue_save =
          !slot_consumes_multicycle && (is_true_prologue_save[slot_insert_pos - 1] ||
                                        (IsSpStackSaveOfReg(m_pat, &saved_r) && saved_r == 31));
      const bool is_non_simple_store =
          !(slot_consumes_multicycle && is_true_prologue_save[slot_insert_pos - 1]) &&
          m_pat.IsSet() && m_pat.SetDest().IsMem() &&
          IsIneligibleForMipsDelaySlot(m_pat, gp64, fp64);
      const bool is_callee_saved_fpr_mtc1 =
          has_deleted_higher_arg_noop_move && IsMipsXferExpr(m_pat) && m_pat.IsSet() &&
          m_pat.SetDest().IsReg() && IsCalleeSavedFprRegno(m_pat.SetDest().RegNo()) &&
          !IsMipsXferExpr(slot_pat.pattern) && !IsMipsHiloExpr(slot_pat.pattern);
      const bool is_inter_xfer_shadow =
          slot_consumes_multicycle && !stepped_multicycle_across_xfer &&
          IsRootMultiCycleProducerExpr(slot_pat.pattern) && !IsMemLoadExpr(slot_pat.pattern) &&
          IsMipsXferExpr(m_pat) && m_pat.IsSet() && m_pat.SetDest().IsReg() &&
          IsCalleeSavedGprRegno(m_pat.SetDest().RegNo()) && !IsMipsXferExpr(slot_pat.pattern) &&
          !IsMipsHiloExpr(slot_pat.pattern);
      if (!is_ra_or_prologue_save && !is_non_simple_store && !is_callee_saved_fpr_mtc1 &&
          !is_inter_xfer_shadow) {
        break;
      }
      if (!CanPlaceUnpackedSlotBeforeIntervening(new_insns, slot_insert_pos - 1, orig_insert_pos,
                                                 slot_pat, head_pat.pattern, gp64, fp64)) {
        break;
      }
      if (is_inter_xfer_shadow) {
        stepped_multicycle_across_xfer = true;
      }
      --slot_insert_pos;
    }
    return slot_insert_pos;
  }

  if (head_sub.op == RtlOp::kCallInsn && !runs_sched2 && slot_insert_pos > 0) {
    auto call_arg_slot_rank = [](const RtlExpr& pat) -> int {
      if (!pat.IsSet() || !pat.SetDest().IsReg() || pat.SetDest().flags.integrated) {
        return -1;
      }
      switch (pat.SetDest().RegNo()) {
        case 4:
        case 44:
          return 0;
        case 5:
        case 46:
          return 1;
        case 6:
          return 2;
        case 7:
          return 3;
        default:
          return -1;
      }
    };
    auto is_multi_insn_arg_move = [&](const RtlExpr& pat) -> bool {
      if (!pat.IsSet() || !pat.SetDest().IsReg()) {
        return false;
      }
      const RtlMachineMode dm = pat.SetDest().mode;
      const int dr = pat.SetDest().RegNo();
      if (!gp64 && dm == RtlMachineMode::kDi && dr < 32) {
        return true;
      }
      if (!fp64 && dm == RtlMachineMode::kDf &&
          (dr < 32 || (pat.SetSrc().IsReg() && pat.SetSrc().RegNo() < 32))) {
        return true;
      }
      return false;
    };
    auto is_adjacent_output_reload_of_prev = [&](size_t idx, const RtlPatternAndNotes& mv) -> bool {
      if (idx == 0 || !mv.pattern.IsSet() || !mv.pattern.SetDest().IsReg() ||
          !mv.pattern.SetSrc().IsReg()) {
        return false;
      }
      const int dst_hr = mv.pattern.SetDest().RegNo();
      const int src_hr = mv.pattern.SetSrc().RegNo();
      if (dst_hr >= 32 || !((src_hr >= 32 && src_hr < 64) || src_hr == 64 || src_hr == 65)) {
        return false;
      }
      for (const RtlRegNote& note : mv.reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg() &&
            note.datum.RegNo() == src_hr) {
          return false;
        }
      }
      auto prev2_p = ParsePatternAndNotes(new_insns[idx - 1]);
      if (!prev2_p.ok()) {
        return false;
      }
      const RtlExpr* pset = nullptr;
      if (prev2_p->pattern.IsSet()) {
        pset = &prev2_p->pattern;
      } else if (prev2_p->pattern.op == RtlOp::kParallel && !prev2_p->pattern.children.empty() &&
                 prev2_p->pattern.children[0].IsSet()) {
        pset = &prev2_p->pattern.children[0];
      }
      return pset != nullptr && pset->SetDest().IsReg() && pset->SetDest().RegNo() == src_hr &&
             !pset->SetSrc().IsReg();
    };
    const int slot_rank = call_arg_slot_rank(slot_pat.pattern);
    while (slot_rank >= 0 && slot_insert_pos > 0) {
      auto prev_p = ParsePatternAndNotes(new_insns[slot_insert_pos - 1]);
      if (!prev_p.ok() || !is_multi_insn_arg_move(prev_p->pattern) ||
          call_arg_slot_rank(prev_p->pattern) <= slot_rank ||
          // Without `sched2`, `reload1.c:6237-6246` (`emit_reload_insns`) emits
          // output reloads immediately after the producer insn; never insert an
          // unpacked delay-slot insn between a producer and its output reload.
          is_adjacent_output_reload_of_prev(slot_insert_pos - 1, *prev_p) ||
          ExpressionsConflict(prev_p->pattern, slot_pat.pattern) ||
          RegNotesConflictWithExpr(slot_pat.reg_notes, prev_p->pattern) ||
          RegNotesConflictWithExpr(prev_p->reg_notes, slot_pat.pattern)) {
        break;
      }
      --slot_insert_pos;
    }
  }
  return slot_insert_pos;
}

struct UnpackedDelaySlotSite {
  size_t block_idx = 0;
  size_t slot_idx = 0;
  size_t head_idx = 0;
  size_t orig_block_size = 0;
  bool head_is_call = false;
};

// Strips `(set dst (call ...))` down to `(call ...)` to match
// `reorg.c:582-585` (`mark_set_resources(..., include_delayed_effects=0)`), where
// the call return register is set by the callee rather than the `CALL_INSN` itself.
RtlExpr BuildCallHeadExprForDelaySlotConflict(const RtlExpr& head_pat) {
  auto strip_call_set = [](const RtlExpr& e) -> RtlExpr {
    if (e.op == RtlOp::kSet && e.children.size() == 2 && e.SetSrc().op == RtlOp::kCall) {
      return e.SetSrc();
    }
    return e;
  };
  if (head_pat.op == RtlOp::kParallel) {
    RtlExpr out = head_pat;
    for (RtlExpr& ch : out.children) {
      ch = strip_call_set(ch);
    }
    return out;
  }
  return strip_call_set(head_pat);
}

// Conservative `reorg.c:2901-2955` (`fill_simple_delay_slots`) backward-scan
// check: verifies that placing `slot_pn` at `cand_blk_pos` in `without_slot`
// (where `head` sits at `orig_head_blk`) ensures `fill_simple_delay_slots` skips
// all intervening insns `[cand_blk_pos .. orig_head_blk - 1]` and selects
// `slot_pn` without altering `update_reg_dead_notes` (`reorg.c:2283-2305`).
bool CanReorgBackwardScanPickSlotAt(const std::vector<std::string>& without_slot,
                                    size_t cand_blk_pos, size_t orig_head_blk,
                                    const RtlPatternAndNotes& slot_pn,
                                    const RtlExpr& head_conflict_expr, bool gp64, bool fp64,
                                    int small_data_threshold,
                                    bool allow_update_reg_dead_notes = false) {
  if (cand_blk_pos > orig_head_blk || orig_head_blk >= without_slot.size()) {
    return false;
  }
  if (cand_blk_pos == orig_head_blk) {
    return true;
  }
  if (!CanPlaceUnpackedSlotBeforeIntervening(without_slot, cand_blk_pos, orig_head_blk, slot_pn,
                                             head_conflict_expr, gp64, fp64,
                                             allow_update_reg_dead_notes)) {
    return false;
  }
  // Under `-G N` (`small_data_threshold > 0`), guard against small-data
  // (`SYMBOL_REF_FLAG`) single-insn stores `(set (mem (symbol_ref ...)) (reg ...))`,
  // which have `dslot == no && length == 1` in `mips.md:79-82, 125-138` and would
  // otherwise be selected ahead of `slot_pn`. Under `-G 0` (`small_data_threshold == 0`),
  // symbolic stores are always 2-word (`length == 2, dslot == no`).
  if (small_data_threshold == 0) {
    return true;
  }
  std::vector<RtlExpr> window_pats;
  window_pats.reserve(orig_head_blk - cand_blk_pos);
  for (size_t i = cand_blk_pos; i < orig_head_blk; ++i) {
    auto parsed = ParsePatternAndNotes(without_slot[i]);
    if (!parsed.ok()) {
      return false;
    }
    window_pats.push_back(std::move(parsed->pattern));
  }
  auto is_potential_sdata_store = [](const RtlExpr& pat) {
    if (!pat.IsSet() || !pat.SetDest().IsMem() || pat.SetDest().children.size() != 1 ||
        !pat.SetSrc().IsReg()) {
      return false;
    }
    const RtlExpr& addr = pat.SetDest().children[0];
    return addr.op == RtlOp::kSymbolRef || addr.op == RtlOp::kConst;
  };
  for (size_t m = 0; m < window_pats.size(); ++m) {
    const RtlExpr& m_pat = window_pats[m];
    if (!is_potential_sdata_store(m_pat) || ExpressionsConflict(m_pat, head_conflict_expr)) {
      continue;
    }
    bool blocked_by_later = false;
    for (size_t later = m + 1; later < window_pats.size(); ++later) {
      if (ExpressionsConflict(m_pat, window_pats[later]) || ExprHasMem(window_pats[later])) {
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

// Per `reorg.c:2901-2955` (`fill_simple_delay_slots`), `mips.md:453-480`
// (`adddi3_internal_1` big-endian `define_split`), and `mips.md:679-704`
// (`subdi3_internal` big-endian `define_split`), when `reorg` scans backwards
// past delay-slot-ineligible `length > 1` insns (e.g. `xor:DI`, `sw:DI`),
// splits a `DImode` add/sub `PARALLEL` in place, and moves its final word
// (`addu dH, dH, tmp` or `subu dH, dH, tmp`) into the delay slot, hoist the
// unpacked delay-slot insn back behind those intervening insns so
// `RecombineReorgSplitIdioms` can fold all 4 split words back into the
// pre-`reorg` `DImode` `PARALLEL`.
void HoistDelaySlotSplitDiAddSubInsns(RtlControlFlowGraph* cfg,
                                      std::vector<UnpackedDelaySlotSite>* sites,
                                      const ParsedCompilerFlags& flags, bool gp64, bool fp64) {
  if (cfg == nullptr || sites == nullptr || gp64 || flags.opt_level <= 0 || sites->empty()) {
    return;
  }
  auto plain_si_gpr = [](const RtlExpr& e, int* out_reg) -> bool {
    if (!e.IsHardReg() || e.mode != RtlMachineMode::kSi) {
      return false;
    }
    const int r = e.RegNo();
    if (r < 2 || r > 25) {
      return false;
    }
    *out_reg = r;
    return true;
  };
  auto is_even_pair = [](int hi) { return hi >= 2 && hi <= 24 && (hi % 2) == 0; };

  std::vector<bool> consumed(sites->size(), false);
  for (size_t si = 0; si < sites->size(); ++si) {
    const UnpackedDelaySlotSite& site = (*sites)[si];
    if (site.block_idx >= cfg->rpo_blocks.size()) {
      continue;
    }
    RtlBasicBlock& blk = cfg->rpo_blocks[site.block_idx];
    if (site.slot_idx >= blk.instructions.size() || site.head_idx >= blk.instructions.size() ||
        site.slot_idx >= site.head_idx) {
      continue;
    }
    auto slot_pn = ParsePatternAndNotes(blk.instructions[site.slot_idx]);
    auto head_pn = ParsePatternAndNotes(blk.instructions[site.head_idx]);
    if (!slot_pn.ok() || !head_pn.ok() || !slot_pn->pattern.IsSet()) {
      continue;
    }
    int d_hi = -1;
    if (!plain_si_gpr(slot_pn->pattern.SetDest(), &d_hi) || !is_even_pair(d_hi)) {
      continue;
    }
    const RtlExpr& rhs = slot_pn->pattern.SetSrc();
    if ((rhs.op != RtlOp::kPlus && rhs.op != RtlOp::kMinus) || rhs.children.size() != 2) {
      continue;
    }
    int c0 = -1;
    int c1 = -1;
    if (!plain_si_gpr(rhs.children[0], &c0) || !plain_si_gpr(rhs.children[1], &c1)) {
      continue;
    }
    const bool is_add = (rhs.op == RtlOp::kPlus);
    int tmp = -1;
    if (c0 == d_hi) {
      tmp = c1;
    } else if (is_add && c1 == d_hi) {
      tmp = c0;
    } else {
      continue;
    }
    const int d_lo = d_hi + 1;
    if (tmp == d_hi || tmp == d_lo) {
      continue;
    }

    // Scan backwards for the 3-insn split prefix `[k-2, k-1, k]` (`k + 1 < site.slot_idx`).
    std::optional<size_t> target_insert_pos;
    for (size_t k = site.slot_idx; k >= 3; --k) {
      const size_t p2_idx = k - 1;
      if (p2_idx + 1 >= site.slot_idx) {
        continue;
      }
      auto p0 = ParsePatternAndNotes(blk.instructions[p2_idx - 2]);
      auto p1 = ParsePatternAndNotes(blk.instructions[p2_idx - 1]);
      auto p2 = ParsePatternAndNotes(blk.instructions[p2_idx]);
      if (!p0.ok() || !p1.ok() || !p2.ok() || !p0->pattern.IsSet() || !p1->pattern.IsSet() ||
          !p2->pattern.IsSet()) {
        continue;
      }
      bool matched = false;
      if (is_add) {
        int r0_d = -1, r0_a = -1, r0_b = -1;
        int r1_d = -1, r1_a = -1, r1_b = -1;
        int r2_d = -1, r2_a = -1, r2_b = -1;
        if (plain_si_gpr(p0->pattern.SetDest(), &r0_d) && r0_d == d_lo &&
            p0->pattern.SetSrc().op == RtlOp::kPlus && p0->pattern.SetSrc().children.size() == 2 &&
            plain_si_gpr(p0->pattern.SetSrc().children[0], &r0_a) &&
            plain_si_gpr(p0->pattern.SetSrc().children[1], &r0_b) &&
            plain_si_gpr(p1->pattern.SetDest(), &r1_d) && r1_d == tmp &&
            p1->pattern.SetSrc().op == RtlOp::kLtu && p1->pattern.SetSrc().children.size() == 2 &&
            plain_si_gpr(p1->pattern.SetSrc().children[0], &r1_a) && r1_a == d_lo &&
            plain_si_gpr(p1->pattern.SetSrc().children[1], &r1_b) &&
            (r1_b == r0_a || r1_b == r0_b) && plain_si_gpr(p2->pattern.SetDest(), &r2_d) &&
            r2_d == d_hi && p2->pattern.SetSrc().op == RtlOp::kPlus &&
            p2->pattern.SetSrc().children.size() == 2 &&
            plain_si_gpr(p2->pattern.SetSrc().children[0], &r2_a) &&
            plain_si_gpr(p2->pattern.SetSrc().children[1], &r2_b) && is_even_pair(r2_a) &&
            is_even_pair(r2_b) &&
            ((r0_a == r2_a + 1 && r0_b == r2_b + 1) || (r0_a == r2_b + 1 && r0_b == r2_a + 1))) {
          matched = true;
        }
      } else {
        int r0_d = -1, r0_a = -1, r0_b = -1;
        int r1_d = -1, r1_a = -1, r1_b = -1;
        int r2_d = -1, r2_a = -1, r2_b = -1;
        if (plain_si_gpr(p0->pattern.SetDest(), &r0_d) && r0_d == tmp &&
            p0->pattern.SetSrc().op == RtlOp::kLtu && p0->pattern.SetSrc().children.size() == 2 &&
            plain_si_gpr(p0->pattern.SetSrc().children[0], &r0_a) &&
            plain_si_gpr(p0->pattern.SetSrc().children[1], &r0_b) &&
            plain_si_gpr(p1->pattern.SetDest(), &r1_d) && r1_d == d_lo &&
            p1->pattern.SetSrc().op == RtlOp::kMinus && p1->pattern.SetSrc().children.size() == 2 &&
            plain_si_gpr(p1->pattern.SetSrc().children[0], &r1_a) && r1_a == r0_a &&
            plain_si_gpr(p1->pattern.SetSrc().children[1], &r1_b) && r1_b == r0_b &&
            plain_si_gpr(p2->pattern.SetDest(), &r2_d) && r2_d == d_hi &&
            p2->pattern.SetSrc().op == RtlOp::kMinus && p2->pattern.SetSrc().children.size() == 2 &&
            plain_si_gpr(p2->pattern.SetSrc().children[0], &r2_a) &&
            plain_si_gpr(p2->pattern.SetSrc().children[1], &r2_b) && is_even_pair(r2_a) &&
            is_even_pair(r2_b) && r0_a == r2_a + 1 && r0_b == r2_b + 1) {
          matched = true;
        }
      }
      if (!matched) {
        continue;
      }
      std::vector<std::string> without_slot = blk.instructions;
      without_slot.erase(without_slot.begin() + static_cast<ptrdiff_t>(site.slot_idx));
      const RtlExpr head_conflict_expr =
          site.head_is_call ? BuildCallHeadExprForDelaySlotConflict(head_pn->pattern)
                            : head_pn->pattern;
      if (CanReorgBackwardScanPickSlotAt(without_slot, /*cand_blk_pos=*/p2_idx + 1,
                                         /*orig_head_blk=*/site.head_idx - 1, *slot_pn,
                                         head_conflict_expr, gp64, fp64, flags.small_data_threshold,
                                         /*allow_update_reg_dead_notes=*/true)) {
        target_insert_pos = p2_idx + 1;
        break;
      }
    }
    if (!target_insert_pos.has_value()) {
      continue;
    }
    const size_t dst_pos = *target_insert_pos;
    const size_t src_pos = site.slot_idx;
    std::string moved_str = std::move(blk.instructions[src_pos]);
    blk.instructions.erase(blk.instructions.begin() + static_cast<ptrdiff_t>(src_pos));
    blk.instructions.insert(blk.instructions.begin() + static_cast<ptrdiff_t>(dst_pos),
                            std::move(moved_str));
    if (blk.rtl_insns.size() == blk.instructions.size()) {
      RtlInsn moved_rtl = std::move(blk.rtl_insns[src_pos]);
      blk.rtl_insns.erase(blk.rtl_insns.begin() + static_cast<ptrdiff_t>(src_pos));
      blk.rtl_insns.insert(blk.rtl_insns.begin() + static_cast<ptrdiff_t>(dst_pos),
                           std::move(moved_rtl));
    } else {
      blk.SyncRtlInsnsFromInstructions();
    }
    consumed[si] = true;
    for (size_t oj = si + 1; oj < sites->size(); ++oj) {
      UnpackedDelaySlotSite& other = (*sites)[oj];
      if (other.block_idx != site.block_idx) {
        continue;
      }
      if (other.slot_idx >= dst_pos && other.slot_idx < src_pos) {
        ++other.slot_idx;
      }
      if (other.head_idx >= dst_pos && other.head_idx < src_pos) {
        ++other.head_idx;
      }
    }
  }
  std::vector<UnpackedDelaySlotSite> remaining;
  remaining.reserve(sites->size());
  for (size_t si = 0; si < sites->size(); ++si) {
    if (!consumed[si]) {
      remaining.push_back((*sites)[si]);
    }
  }
  *sites = std::move(remaining);
}

// Forward-verified Stage 1 delay-slot preimage selector for `flags.RunsSched1()`:
// when the heuristic unpack position of a `(sequence [head, slot])` leaves its
// block outside `Im(F_sched1)`, searches alternative legal `reorg.c:2901-2955`
// preimage positions within the same basic block body, strictly gated on:
//   (1) exact `reorg.c:2901-2955` backward-scan uniqueness (`CanReorgBackwardScanPickSlotAt`),
//   (2) post-reload `Pass04`/`Pass05` safety (`sched_model::PreservesPostReloadOrderSafety`),
//   (3) `Pass03` `sched2` achievability (`sched_model::IsAchievable(kSched2)` when `RunsSched2`),
//   (4) `Pass06` `sched1` achievability (`sched_model::IsAchievable(kSched1)`).
// Per `jump.c:1978` (`find_cross_jump`) and `sched.c:2362-2407`, when `jump2`
// cross-jumps a multi-cycle FP arithmetic consumer into the head of a join
// block `b` AFTER `sched1`/`sched2`, the combined block `[prefix, rest_of_b]`
// never existed during `sched1`/`sched2`. Do not let `SelectSched1Achievable`
// push that unpacked delay slot back past the join block's branch condition
// chain before Stage 2 (`InvertDoCrossJump`) splits the cross-jumped prefix.
bool IsCrossJumpedFpConsumerDelaySlot(const RtlControlFlowGraph& cfg, size_t b,
                                      const RtlPatternAndNotes& slot_pn, size_t slot_idx,
                                      size_t head_idx) {
  if (b == 0 || b >= cfg.rpo_blocks.size() || slot_idx + 1 >= head_idx) {
    return false;
  }
  const RtlExpr& pat = slot_pn.pattern;
  if (!pat.IsSet() || !pat.SetDest().IsHardReg()) {
    return false;
  }
  const int dst_r = pat.SetDest().RegNo();
  if (dst_r < 32 || dst_r >= 64) {
    return false;
  }
  const RtlOp op = pat.SetSrc().op;
  if (op != RtlOp::kPlus && op != RtlOp::kMinus && op != RtlOp::kMult && op != RtlOp::kDiv &&
      op != RtlOp::kSqrt) {
    return false;
  }
  std::vector<int> dead_fprs;
  for (const RtlRegNote& note : slot_pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
      const int r = note.datum.RegNo();
      if (r >= 32 && r < 52 && pat.SetSrc().ContainsReg(r)) {
        dead_fprs.push_back(r);
      }
    }
  }
  if (dead_fprs.empty()) {
    return false;
  }
  std::vector<size_t> preds;
  for (size_t p = 0; p < cfg.rpo_blocks.size(); ++p) {
    for (size_t s : cfg.rpo_blocks[p].successors) {
      if (s == b) {
        preds.push_back(p);
        break;
      }
    }
  }
  if (preds.size() < 2) {
    return false;
  }
  bool has_ft = false;
  for (size_t p : preds) {
    if (p >= b || cfg.rpo_blocks[p].successors.size() != 1 ||
        cfg.rpo_blocks[p].instructions.empty()) {
      return false;
    }
    auto last_p = ParsePatternAndNotes(cfg.rpo_blocks[p].instructions.back());
    if (!last_p.ok()) {
      return false;
    }
    if (p + 1 == b && !IsAnyJumpOrReturnTopExpr(last_p->pattern)) {
      has_ft = true;
    } else if (!IsSimpleUncondJumpExpr(last_p->pattern)) {
      return false;
    }
  }
  if (!has_ft) {
    return false;
  }
  for (int dead_fpr : dead_fprs) {
    bool all_preds_def = true;
    for (size_t p : preds) {
      bool pred_def_multicycle = false;
      for (const std::string& s : cfg.rpo_blocks[p].instructions) {
        auto pp = ParsePatternAndNotes(s);
        if (!pp.ok()) {
          continue;
        }
        std::vector<int> defs;
        CollectDefinedRegs(pp->pattern, &defs);
        if (std::find(defs.begin(), defs.end(), dead_fpr) != defs.end()) {
          pred_def_multicycle = IsRootMultiCycleProducerExpr(pp->pattern);
        }
      }
      if (!pred_def_multicycle) {
        all_preds_def = false;
        break;
      }
    }
    if (all_preds_def) {
      return true;
    }
  }
  return false;
}

void SelectSched1AchievableDelaySlotPositions(RtlControlFlowGraph* cfg,
                                              absl::Span<const UnpackedDelaySlotSite> sites,
                                              const CompilationProfile& profile,
                                              const ParsedCompilerFlags& flags, bool gp64,
                                              bool fp64) {
  if (sites.empty()) {
    return;
  }
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  const sched_model::RegFacts facts_s2 =
      sched_model::RegFacts::FromCfg(*cfg, profile, sched_model::Phase::kSched2);
  const std::vector<int> restored_b0_args = PredictRestoredEntryArgSelfMoveRegs(*cfg, profile);
  const sched_model::CfgSched1Context cfg_ctx =
      sched_model::ComputeCfgSched1Context(*cfg, *cfg, mm, restored_b0_args);

  for (const UnpackedDelaySlotSite& site : sites) {
    if (site.block_idx >= cfg->rpo_blocks.size()) {
      continue;
    }
    RtlBasicBlock& blk = cfg->rpo_blocks[site.block_idx];
    if (blk.instructions.size() != site.orig_block_size || site.slot_idx >= site.head_idx ||
        site.head_idx >= blk.instructions.size()) {
      continue;
    }

    const sched_model::ParsedSchedRegion region =
        sched_model::ParseBlockForSchedModel(blk, site.block_idx, sched_model::Phase::kSched2);
    if (!region.valid || region.has_mid_block_loop_barrier || region.sched_block.size() <= 2) {
      continue;
    }

    const int n = static_cast<int>(region.sched_block.size());
    int slot_s = -1;
    int head_s = -1;
    for (int i = 0; i < n; ++i) {
      if (region.sched_orig_indices[static_cast<size_t>(i)] == site.slot_idx) {
        slot_s = i;
      } else if (region.sched_orig_indices[static_cast<size_t>(i)] == site.head_idx) {
        head_s = i;
      }
    }
    if (slot_s < 0 || head_s < 0 || slot_s >= head_s) {
      continue;
    }
    if (IsCrossJumpedFpConsumerDelaySlot(*cfg, site.block_idx,
                                         region.sched_block[static_cast<size_t>(slot_s)].pn,
                                         site.slot_idx, site.head_idx)) {
      continue;
    }

    const sched_model::PostReloadBodyBounds bounds =
        sched_model::FindPostReloadBodyBounds(region.sched_block, site.block_idx);
    // For return delay slots, `(use (reg/i ...))` return-value markers sit at
    // `[bounds.epilogue_begin, head_s)` between the unpacked delay slot and
    // `(return)` at `head_s` (`reorg.c:2912` skips `USE` insns during its
    // backward scan). Allow `head_s > bounds.epilogue_begin` when every insn in
    // `[bounds.epilogue_begin, head_s)` is a `USE` marker.
    bool head_after_return_uses = (head_s > bounds.epilogue_begin);
    for (int k = bounds.epilogue_begin; head_after_return_uses && k < head_s; ++k) {
      if (region.sched_block[static_cast<size_t>(k)].pn.pattern.op != RtlOp::kUse) {
        head_after_return_uses = false;
      }
    }
    if (slot_s < bounds.prologue_end || slot_s >= bounds.epilogue_begin ||
        (head_s > bounds.epilogue_begin && !head_after_return_uses)) {
      continue;
    }
    const int body_len = bounds.epilogue_begin - bounds.prologue_end;
    if (body_len <= 1) {
      continue;
    }

    absl::btree_set<int> live_at_end_s2;
    for (size_t succ : blk.successors) {
      if (const auto it = cfg->sideband.basic_block_live_at_start.find(static_cast<int>(succ));
          it != cfg->sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }

    std::vector<sched_model::Insn> base_body;
    base_body.reserve(static_cast<size_t>(body_len));
    for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
      sched_model::Insn insn = region.sched_block[static_cast<size_t>(i)];
      insn.luid = i - bounds.prologue_end;
      base_body.push_back(std::move(insn));
    }
    sched_model::Sched1ProjectionContext proj_ctx =
        sched_model::BuildSched1ProjectionContextForBlock(site.block_idx, cfg_ctx);
    proj_ctx.excluded_prologue_body_indices = sched_model::FindInterleavedPrologueSaveBodyIndices(
        region.sched_block, bounds, site.block_idx, mm);
    const sched_model::Sched1ProjectionView base_view =
        sched_model::ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, proj_ctx);
    const bool base_s2_ach = !flags.RunsSched2() ||
                             sched_model::IsAchievable(region.sched_block, facts_s2, mm,
                                                       sched_model::Phase::kSched2, live_at_end_s2);
    const bool base_s1_direct =
        base_view.core_insns.size() <= 1 ||
        sched_model::IsAchievable(base_view.core_insns, base_view.facts_s1, mm,
                                  sched_model::Phase::kSched1, base_view.live_at_end_s1);

    // Mirrors `caller-save.c:559-646` (`insert_restore`) + `reorg.c:2901-2955`:
    // if `slot_s` is the first post-call user of a separated caller-save restore
    // `cs_idx` (`cs_idx + 1 < slot_s`) and neither sinking `cs_idx` to `slot_s`
    // nor hoisting `slot_s` to `cs_idx + 1` replays `region.sched_block` under
    // `kSched2`, then `reorg.c` pulled `slot_s` past an independent instruction
    // (such as a branch compare `slt`). Allow searching earlier `reorg.c`-valid
    // placements `cand_s` where `cs_idx` and `slot_s` CAN be re-adjoined.
    int cs_restore_s = -1;
    bool base_needs_cs_readjoin = false;
    auto check_sched2_replays_seq = [&](const std::vector<sched_model::Insn>& cand_in,
                                        const std::vector<sched_model::Insn>& target_seq) -> bool {
      const sched_model::DepGraph g =
          sched_model::Analyze(cand_in, facts_s2, mm, sched_model::Phase::kSched2);
      const sched_model::ScheduleResult sr = sched_model::Schedule(
          cand_in, g, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2);
      if (sr.order.size() != target_seq.size()) {
        return false;
      }
      for (size_t pos = 0; pos < target_seq.size(); ++pos) {
        if (cand_in[static_cast<size_t>(sr.order[pos])].pn.pattern.ToSexprString() !=
            target_seq[pos].pn.pattern.ToSexprString()) {
          return false;
        }
      }
      return true;
    };
    auto make_rotated_seq = [](const std::vector<sched_model::Insn>& seq, int first, int middle,
                               int last) {
      std::vector<sched_model::Insn> out = seq;
      std::rotate(out.begin() + first, out.begin() + middle, out.begin() + last);
      for (int pos = 0; pos < static_cast<int>(out.size()); ++pos) {
        out[static_cast<size_t>(pos)].luid = pos;
      }
      return out;
    };
    auto extract_cs_restore_reg = [&](const sched_model::Insn& ki) -> int {
      if (ki.kind != sched_model::Insn::Kind::kInsn) {
        return -1;
      }
      const RtlExpr& kpat = ki.pn.pattern;
      if (!kpat.IsSet() || !kpat.SetDest().IsHardReg() || kpat.SetDest().flags.integrated ||
          !kpat.SetSrc().IsMem() || kpat.SetSrc().children.empty()) {
        return -1;
      }
      const RtlExpr& addr = kpat.SetSrc().children[0];
      int64_t off = -1;
      if (addr.IsHardReg() && addr.RegNo() == 29) {
        off = 0;
      } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                 addr.children[0].IsHardReg() && addr.children[0].RegNo() == 29 &&
                 addr.children[1].IsConstInt()) {
        off = addr.children[1].int_val;
      } else {
        return -1;
      }
      if (!cfg_ctx.stack_layout.caller_save_slot_offsets.contains(off)) {
        return -1;
      }
      const int cs_r = kpat.SetDest().RegNo();
      return (cs_r > 0 && cs_r < kMipsFirstPseudoRegister && mm.CallUsed(cs_r)) ? cs_r : -1;
    };
    auto make_cs_adjacent_preimage = [&](const std::vector<sched_model::Insn>& seq) {
      std::vector<sched_model::Insn> out = seq;
      const int len = static_cast<int>(out.size());
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin && i < len; ++i) {
        const int cs_r = extract_cs_restore_reg(out[static_cast<size_t>(i)]);
        if (cs_r < 0) {
          continue;
        }
        int user = -1;
        for (int m = i + 1; m < bounds.epilogue_begin && m < len; ++m) {
          if (out[static_cast<size_t>(m)].kind == sched_model::Insn::Kind::kCall ||
              out[static_cast<size_t>(m)].kind == sched_model::Insn::Kind::kJump) {
            break;
          }
          if (out[static_cast<size_t>(m)].pn.pattern.ContainsReg(cs_r)) {
            user = m;
            break;
          }
        }
        if (user > i + 1 && out[static_cast<size_t>(user)].kind == sched_model::Insn::Kind::kInsn) {
          std::rotate(out.begin() + i + 1, out.begin() + user, out.begin() + user + 1);
        }
      }
      for (int pos = 0; pos < len; ++pos) {
        out[static_cast<size_t>(pos)].luid = pos;
      }
      return out;
    };
    if (flags.RunsSched2() && base_s2_ach &&
        !cfg_ctx.stack_layout.caller_save_slot_offsets.empty() &&
        region.sched_block[static_cast<size_t>(slot_s)].kind == sched_model::Insn::Kind::kInsn) {
      for (int k = slot_s - 1; k >= bounds.prologue_end; --k) {
        const sched_model::Insn& ki = region.sched_block[static_cast<size_t>(k)];
        if (ki.kind == sched_model::Insn::Kind::kCall ||
            ki.kind == sched_model::Insn::Kind::kJump) {
          break;
        }
        const int cs_r = extract_cs_restore_reg(ki);
        if (cs_r < 0 ||
            !region.sched_block[static_cast<size_t>(slot_s)].pn.pattern.ContainsReg(cs_r)) {
          continue;
        }
        bool touched_between = false;
        for (int m = k + 1; m < slot_s; ++m) {
          if (region.sched_block[static_cast<size_t>(m)].pn.pattern.ContainsReg(cs_r)) {
            touched_between = true;
            break;
          }
        }
        if (!touched_between && k + 1 < slot_s) {
          const bool sink_ok = check_sched2_replays_seq(
              make_rotated_seq(region.sched_block, k, k + 1, slot_s), region.sched_block);
          const bool hoist_all_ok = check_sched2_replays_seq(
              make_cs_adjacent_preimage(region.sched_block), region.sched_block);
          if (!sink_ok && !hoist_all_ok) {
            cs_restore_s = k;
            base_needs_cs_readjoin = true;
          }
          break;
        }
      }
    }

    if (!base_needs_cs_readjoin &&
        (base_s1_direct ||
         (base_s2_ach && flags.RunsSched2() &&
          sched_model::HasSched1AchievablePreimageForSched2Block(
              region.sched_block, site.block_idx, bounds, base_view, proj_ctx, cfg_ctx, facts_s2,
              mm, live_at_end_s2, /*allow_pure_reader_reg_dead_transfer=*/false)))) {
      continue;
    }

    std::vector<std::string> without_slot_strs = blk.instructions;
    without_slot_strs.erase(without_slot_strs.begin() + static_cast<ptrdiff_t>(site.slot_idx));
    const size_t orig_head_blk = site.head_idx - 1;
    const RtlPatternAndNotes& slot_pn = region.sched_block[static_cast<size_t>(slot_s)].pn;
    const RtlExpr head_conflict_expr =
        site.head_is_call ? BuildCallHeadExprForDelaySlotConflict(
                                region.sched_block[static_cast<size_t>(head_s)].pn.pattern)
                          : region.sched_block[static_cast<size_t>(head_s)].pn.pattern;

    // Order candidate positions by minimal displacement from `slot_s` (earlier on ties).
    const int cand_limit = std::min(head_s, bounds.epilogue_begin);
    std::vector<int> candidates;
    for (int c = bounds.prologue_end; c < cand_limit; ++c) {
      if (c != slot_s) {
        candidates.push_back(c);
      }
    }
    std::sort(candidates.begin(), candidates.end(), [slot_s](int a, int b) {
      const int da = std::abs(a - slot_s);
      const int db = std::abs(b - slot_s);
      return (da != db) ? (da < db) : (a < b);
    });

    struct ReorgDeadTransfer {
      size_t slot_note_idx = 0;
      int target_sched_idx = -1;
      size_t target_blk_idx = 0;
      RtlRegNote note;
    };

    std::optional<size_t> chosen_blk_pos;
    std::vector<ReorgDeadTransfer> chosen_transfers;
    std::optional<size_t> tier2_blk_pos;
    std::vector<ReorgDeadTransfer> tier2_transfers;
    std::optional<size_t> tier3_blk_pos;
    std::vector<ReorgDeadTransfer> tier3_transfers;
    int tier2_checks = 0;
    constexpr int kMaxTier2Checks = 6;
    for (int cand_s : candidates) {
      // On block 0, `Pass03` (`InvertThreadPrologueAndEpilogue` + GCC
      // `thread_prologue_and_epilogue`, `toplev.c:3103`, `mips.c:expand_prologue`)
      // strips and re-emits interleaved prologue callee-save stores in canonical
      // order at the top of block 0 before running `sched2`. Moving a body insn
      // across an interleaved prologue save when `base_sched` is already in
      // `Im(F_sched2)` creates a non-canonical order that `sched2` cannot
      // reproduce from the canonical prologue.
      if (base_s2_ach && site.block_idx == 0 && !proj_ctx.excluded_prologue_body_indices.empty()) {
        const int lo_s = std::min(slot_s, cand_s);
        const int hi_s = std::max(slot_s, cand_s);
        bool crosses_prologue_save = false;
        for (int rel : proj_ctx.excluded_prologue_body_indices) {
          const int p_s = bounds.prologue_end + rel;
          if (p_s >= lo_s && p_s <= hi_s) {
            crosses_prologue_save = true;
            break;
          }
        }
        if (crosses_prologue_save) {
          continue;
        }
      }
      size_t cand_blk_pos = orig_head_blk;
      if (cand_s < slot_s) {
        cand_blk_pos = region.sched_orig_indices[static_cast<size_t>(cand_s)];
      } else if (cand_s < head_s - 1) {
        cand_blk_pos = region.sched_orig_indices[static_cast<size_t>(cand_s + 1)] - 1;
      }

      // Inverse of `reorg.c:2283-2306` (`update_reg_dead_notes`): when `reorg.c`
      // pulls `slot_pn` forward past an intervening pure reader `p` that held
      // `REG_DEAD r` (where `reg_referenced_p(r, PATTERN(slot_pn))`), `reorg.c`
      // moves `REG_DEAD r` from `p` onto `slot_pn`. When moving `slot_pn` back
      // before `[cand_s .. slot_s - 1]`, restore each such `REG_DEAD r` onto the
      // last intervening pure reader `p` in `[cand_s .. slot_s - 1]`.
      std::vector<sched_model::Insn> trial_base = region.sched_block;
      RtlPatternAndNotes trial_slot_pn = slot_pn;
      std::vector<std::string> trial_without_slot = without_slot_strs;
      std::vector<ReorgDeadTransfer> trial_transfers;
      if (cand_s < slot_s) {
        for (size_t ni = 0; ni < slot_pn.reg_notes.size(); ++ni) {
          const RtlRegNote& note = slot_pn.reg_notes[ni];
          if (note.kind != RtlMachineMode::kRegDead || !note.datum.IsReg()) {
            continue;
          }
          const int dead_r = note.datum.RegNo();
          if (dead_r < 0 || dead_r >= kMipsFirstPseudoRegister) {
            continue;
          }
          int last_reader_s = -1;
          for (int k = cand_s; k < slot_s; ++k) {
            const sched_model::Insn& k_insn = region.sched_block[static_cast<size_t>(k)];
            if (k_insn.kind != sched_model::Insn::Kind::kInsn ||
                k_insn.pn.pattern.op == RtlOp::kUse || k_insn.pn.pattern.op == RtlOp::kClobber ||
                k_insn.pn.pattern.op == RtlOp::kNote) {
              continue;
            }
            if (k_insn.pn.pattern.ContainsReg(dead_r)) {
              last_reader_s = k;
            }
          }
          if (last_reader_s >= 0) {
            const size_t target_orig_blk =
                region.sched_orig_indices[static_cast<size_t>(last_reader_s)];
            trial_transfers.push_back(ReorgDeadTransfer{
                .slot_note_idx = ni,
                .target_sched_idx = last_reader_s,
                .target_blk_idx = target_orig_blk,
                .note = note,
            });
          }
        }
        if (!trial_transfers.empty()) {
          absl::btree_set<size_t> erased_ni;
          absl::btree_map<int, std::vector<RtlRegNote>> added_by_sched;
          for (const ReorgDeadTransfer& tr : trial_transfers) {
            erased_ni.insert(tr.slot_note_idx);
            added_by_sched[tr.target_sched_idx].push_back(tr.note);
          }
          std::vector<RtlRegNote> kept_slot_notes;
          kept_slot_notes.reserve(slot_pn.reg_notes.size());
          for (size_t ni = 0; ni < slot_pn.reg_notes.size(); ++ni) {
            if (!erased_ni.contains(ni)) {
              kept_slot_notes.push_back(slot_pn.reg_notes[ni]);
            }
          }
          trial_slot_pn.reg_notes = kept_slot_notes;
          trial_base[static_cast<size_t>(slot_s)].pn.reg_notes = std::move(kept_slot_notes);
          for (const auto& [ts, notes] : added_by_sched) {
            auto& dst_notes = trial_base[static_cast<size_t>(ts)].pn.reg_notes;
            dst_notes.insert(dst_notes.end(), notes.begin(), notes.end());
            const size_t orig_idx = region.sched_orig_indices[static_cast<size_t>(ts)];
            const size_t ws_idx = (orig_idx < site.slot_idx) ? orig_idx : (orig_idx - 1);
            if (ws_idx < trial_without_slot.size()) {
              trial_without_slot[ws_idx] = FormatGccPrintRtlPatternAndNotes(
                  trial_base[static_cast<size_t>(ts)].pn.pattern,
                  trial_base[static_cast<size_t>(ts)].pn.reg_notes,
                  trial_base[static_cast<size_t>(ts)].pn.bracketed_notes, /*call_fusage=*/{});
            }
          }
        }
      }

      // Gate 1: exact `reorg.c:2901-2955` backward-scan uniqueness (allowing
      // `reorg.c:2283-2306` `update_reg_dead_notes` transfer when `trial_transfers`
      // restores `REG_DEAD` onto an intervening pure reader).
      if (!CanReorgBackwardScanPickSlotAt(trial_without_slot, cand_blk_pos, orig_head_blk,
                                          trial_slot_pn, head_conflict_expr, gp64, fp64,
                                          flags.small_data_threshold,
                                          /*allow_update_reg_dead_notes=*/
                                          !trial_transfers.empty())) {
        continue;
      }

      // Construct `cand_in_base`: erase `slot_s`, re-insert at `cand_s`.
      std::vector<int> cand_in_base;
      cand_in_base.reserve(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        if (i != slot_s) {
          cand_in_base.push_back(i);
        }
      }
      cand_in_base.insert(cand_in_base.begin() + cand_s, slot_s);

      // Gate 2: post-reload DAG, reload contiguity, `REG_DEAD`, liveness, equiv, interferences.
      if (!sched_model::PreservesPostReloadOrderSafety(
              trial_base, cand_in_base, bounds.prologue_end, bounds.epilogue_begin, live_at_end_s2,
              cfg_ctx.cfg_copy_pairs, facts_s2, mm,
              /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
              proj_ctx.excluded_prologue_body_indices)) {
        continue;
      }

      std::vector<sched_model::Insn> cand_sched;
      cand_sched.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        sched_model::Insn insn =
            trial_base[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
        insn.luid = pos;
        cand_sched.push_back(std::move(insn));
      }

      // Gate 3: `Pass03` `sched2` achievability.
      if (flags.RunsSched2() &&
          !sched_model::IsAchievable(cand_sched, facts_s2, mm, sched_model::Phase::kSched2,
                                     live_at_end_s2)) {
        continue;
      }
      if (base_needs_cs_readjoin) {
        if (cand_s <= cs_restore_s) {
          continue;
        }
        const bool cand_hoist_all_ok =
            check_sched2_replays_seq(make_cs_adjacent_preimage(cand_sched), cand_sched);
        const bool cand_sink_ok =
            (cand_s == cs_restore_s + 1) ||
            check_sched2_replays_seq(
                make_rotated_seq(cand_sched, cs_restore_s, cs_restore_s + 1, cand_s), cand_sched);
        if (!cand_hoist_all_ok && !cand_sink_ok) {
          continue;
        }
      }

      if ((!base_s2_ach || base_needs_cs_readjoin) && !tier3_blk_pos.has_value()) {
        tier3_blk_pos = cand_blk_pos;
        tier3_transfers = trial_transfers;
      }

      // Gate 4: `Pass06` `sched1` achievability (Tier 1: direct `IsAchievable(kSched1)`;
      // Tier 2: `cand_sched` admits a `kSched2`-verified preimage in `Im(F_sched1)` when
      // `base_sched` does not; Tier 3 fallback when `!base_s2_ach` restores `Im(F_sched2)`).
      std::vector<sched_model::Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int pos = bounds.prologue_end; pos < bounds.epilogue_begin; ++pos) {
        sched_model::Insn insn = cand_sched[static_cast<size_t>(pos)];
        insn.luid = pos - bounds.prologue_end;
        cand_body.push_back(std::move(insn));
      }
      sched_model::Sched1ProjectionContext cand_ctx = proj_ctx;
      cand_ctx.excluded_prologue_body_indices = sched_model::FindInterleavedPrologueSaveBodyIndices(
          cand_sched, bounds, site.block_idx, mm);
      const sched_model::Sched1ProjectionView cand_view =
          sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
      if (cand_view.core_insns.size() > 1 &&
          sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                    sched_model::Phase::kSched1, cand_view.live_at_end_s1)) {
        chosen_blk_pos = cand_blk_pos;
        chosen_transfers = std::move(trial_transfers);
        break;
      }
      if (flags.RunsSched2() && !tier2_blk_pos.has_value() && tier2_checks < kMaxTier2Checks) {
        ++tier2_checks;
        if (sched_model::HasSched1AchievablePreimageForSched2Block(
                cand_sched, site.block_idx, bounds, cand_view, cand_ctx, cfg_ctx, facts_s2, mm,
                live_at_end_s2)) {
          tier2_blk_pos = cand_blk_pos;
          tier2_transfers = std::move(trial_transfers);
        }
      }
    }

    if (!chosen_blk_pos.has_value() && tier2_blk_pos.has_value()) {
      chosen_blk_pos = tier2_blk_pos;
      chosen_transfers = std::move(tier2_transfers);
    } else if (!chosen_blk_pos.has_value() && tier3_blk_pos.has_value()) {
      chosen_blk_pos = tier3_blk_pos;
      chosen_transfers = std::move(tier3_transfers);
    }

    if (!chosen_blk_pos.has_value()) {
      continue;
    }

    if (!chosen_transfers.empty()) {
      absl::btree_set<size_t> erased_ni;
      absl::btree_map<size_t, std::vector<RtlRegNote>> added_by_blk;
      for (const ReorgDeadTransfer& tr : chosen_transfers) {
        erased_ni.insert(tr.slot_note_idx);
        added_by_blk[tr.target_blk_idx].push_back(tr.note);
      }
      if (auto parsed_slot = ParsePatternAndNotes(blk.instructions[site.slot_idx]);
          parsed_slot.ok()) {
        std::vector<RtlRegNote> kept;
        kept.reserve(parsed_slot->reg_notes.size());
        for (size_t ni = 0; ni < parsed_slot->reg_notes.size(); ++ni) {
          if (!erased_ni.contains(ni)) {
            kept.push_back(std::move(parsed_slot->reg_notes[ni]));
          }
        }
        parsed_slot->reg_notes = std::move(kept);
        blk.instructions[site.slot_idx] = FormatGccPrintRtlPatternAndNotes(
            parsed_slot->pattern, parsed_slot->reg_notes, parsed_slot->bracketed_notes,
            /*call_fusage=*/{});
      }
      for (const auto& [target_blk_idx, notes] : added_by_blk) {
        if (target_blk_idx >= blk.instructions.size()) {
          continue;
        }
        if (auto parsed_target = ParsePatternAndNotes(blk.instructions[target_blk_idx]);
            parsed_target.ok()) {
          parsed_target->reg_notes.insert(parsed_target->reg_notes.end(), notes.begin(),
                                          notes.end());
          blk.instructions[target_blk_idx] = FormatGccPrintRtlPatternAndNotes(
              parsed_target->pattern, parsed_target->reg_notes, parsed_target->bracketed_notes,
              /*call_fusage=*/{});
        }
      }
    }

    const bool has_synced_rtl =
        chosen_transfers.empty() && (blk.rtl_insns.size() == blk.instructions.size());
    std::string moved_str = std::move(blk.instructions[site.slot_idx]);
    blk.instructions.erase(blk.instructions.begin() + static_cast<ptrdiff_t>(site.slot_idx));
    blk.instructions.insert(blk.instructions.begin() + static_cast<ptrdiff_t>(*chosen_blk_pos),
                            std::move(moved_str));
    if (has_synced_rtl) {
      RtlInsn moved_rtl = std::move(blk.rtl_insns[site.slot_idx]);
      blk.rtl_insns.erase(blk.rtl_insns.begin() + static_cast<ptrdiff_t>(site.slot_idx));
      blk.rtl_insns.insert(blk.rtl_insns.begin() + static_cast<ptrdiff_t>(*chosen_blk_pos),
                           std::move(moved_rtl));
    } else {
      const std::vector<RtlInsn> prev_rtl = blk.rtl_insns;
      blk.SyncRtlInsnsFromInstructions();
      for (size_t old_i = 0; old_i < prev_rtl.size(); ++old_i) {
        if (!prev_rtl[old_i].has_explicit_call_fusage) {
          continue;
        }
        size_t new_i = old_i;
        if (old_i == site.slot_idx) {
          new_i = *chosen_blk_pos;
        } else {
          if (old_i > site.slot_idx) {
            --new_i;
          }
          if (new_i >= *chosen_blk_pos) {
            ++new_i;
          }
        }
        if (new_i < blk.rtl_insns.size() && !blk.rtl_insns[new_i].has_explicit_call_fusage) {
          blk.rtl_insns[new_i].has_explicit_call_fusage = true;
          blk.rtl_insns[new_i].call_fusage = prev_rtl[old_i].call_fusage;
        }
      }
    }
  }
}

// Inverse of `reorg.c:3097-3139` (`fill_simple_delay_slots` unconditional-jump
// target-thread stealing): when a simple unconditional jump `pred_b -> target_b`
// has no eligible delay-slot insn in `pred_b` (`reorg.c:2901-2955`), GCC copies
// the first insn `S` of `target_b` into the jump's delay slot and redirects the
// jump past `S`, leaving the original `S` at the tail of the fallthrough
// predecessor `ft_b = target_b - 1`. Restores `S` to the head of `target_b` and
// drops the stolen delay-slot copy from every jumping predecessor.
void InvertSimpleJumpTargetStolenDelaySlots(RtlControlFlowGraph* cfg,
                                            const ParsedCompilerFlags& flags, bool gp64,
                                            bool fp64) {
  const size_t num_blocks = cfg->rpo_blocks.size();
  if (num_blocks < 2) {
    return;
  }
  std::vector<std::vector<size_t>> preds(num_blocks);
  for (size_t b = 0; b < num_blocks; ++b) {
    for (size_t succ : cfg->rpo_blocks[b].successors) {
      if (succ < num_blocks) {
        preds[succ].push_back(b);
      }
    }
  }

  auto block_has_eligible_simple_delay_slot = [&](const RtlBasicBlock& blk, size_t jump_idx) {
    std::vector<RtlExpr> window;
    for (size_t i = jump_idx; i-- > 0;) {
      auto pn = ParsePatternAndNotes(blk.instructions[i]);
      if (!pn.ok()) {
        return true;
      }
      const RtlExpr& pat = pn->pattern;
      if (pat.op == RtlOp::kNote || pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber) {
        continue;
      }
      if (pat.op == RtlOp::kSequence || pat.op == RtlOp::kCall || IsAnyJumpOrReturnTopExpr(pat)) {
        break;
      }
      bool has_call = false;
      pat.Visit([&has_call](const RtlExpr& n) {
        if (n.op == RtlOp::kCall) {
          has_call = true;
        }
      });
      if (has_call) {
        break;
      }
      bool blocked_by_later = false;
      const bool m_writes_mem = IsMemWriteExpr(pat);
      const bool m_has_mem = ExprHasMem(pat);
      for (const RtlExpr& l_pat : window) {
        if (ExpressionsConflict(pat, l_pat) || (m_writes_mem && ExprHasMem(l_pat)) ||
            (m_has_mem && IsMemWriteExpr(l_pat))) {
          blocked_by_later = true;
          break;
        }
      }
      bool is_potential_sdata_store = false;
      if (flags.small_data_threshold > 0 && pat.IsSet() && pat.SetDest().IsMem() &&
          pat.SetDest().children.size() == 1 && pat.SetSrc().IsReg()) {
        const RtlExpr& addr = pat.SetDest().children[0];
        is_potential_sdata_store = (addr.op == RtlOp::kSymbolRef || addr.op == RtlOp::kConst);
      }
      if (!blocked_by_later &&
          (!IsIneligibleForMipsDelaySlot(pat, gp64, fp64) || is_potential_sdata_store)) {
        return true;
      }
      window.push_back(pat);
    }
    return false;
  };

  for (size_t target_b = 1; target_b < num_blocks; ++target_b) {
    if (preds[target_b].size() < 2) {
      continue;
    }
    const size_t ft_b = target_b - 1;
    if (ft_b > 0 && preds[ft_b].empty()) {
      continue;
    }
    const RtlBasicBlock& ft_blk = cfg->rpo_blocks[ft_b];
    if (ft_blk.successors.size() != 1 || ft_blk.successors[0] != target_b) {
      continue;
    }
    std::optional<size_t> ft_last_idx;
    size_t ft_real_insns = 0;
    bool ft_falls_through = true;
    for (size_t i = 0; i < ft_blk.instructions.size(); ++i) {
      auto pn = ParsePatternAndNotes(ft_blk.instructions[i]);
      if (!pn.ok()) {
        ft_falls_through = false;
        break;
      }
      if (pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kUse ||
          pn->pattern.op == RtlOp::kClobber) {
        continue;
      }
      if (pn->pattern.op == RtlOp::kBarrier || IsAnyJumpOrReturnTopExpr(pn->pattern)) {
        ft_falls_through = false;
        break;
      }
      ft_last_idx = i;
      ++ft_real_insns;
    }
    if (!ft_falls_through || !ft_last_idx.has_value() || ft_real_insns < 2) {
      continue;
    }
    auto ft_last_pn = ParsePatternAndNotes(ft_blk.instructions[*ft_last_idx]);
    if (!ft_last_pn.ok() || ft_last_pn->pattern.op == RtlOp::kSequence ||
        ft_last_pn->pattern.op == RtlOp::kCall || IsCallPattern(ft_last_pn->pattern) ||
        IsIneligibleForMipsDelaySlot(ft_last_pn->pattern, gp64, fp64)) {
      continue;
    }
    // Find the last real instruction before `ft_last_idx` in `ft_b`. Because
    // `jump.c:do_cross_jump` (`toplev.c:3142`, `jump_optimize (insns, 1, 1, 0)`)
    // runs immediately before `dbr_schedule` (`reorg.c`), if `S` had truly been
    // at the head of `target_b` before `reorg.c:3097-3139`, any identical
    // predecessor tail `prev_pred == prev_ft` would already have been
    // cross-jumped into `target_b`.
    std::optional<RtlExpr> ft_prev_pat;
    for (size_t i = *ft_last_idx; i-- > 0;) {
      auto pn = ParsePatternAndNotes(ft_blk.instructions[i]);
      if (!pn.ok()) {
        break;
      }
      if (pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kUse ||
          pn->pattern.op == RtlOp::kClobber) {
        continue;
      }
      ft_prev_pat = pn->pattern;
      break;
    }

    struct JumpPredMatch {
      size_t pred_b = 0;
      size_t seq_idx = 0;
      std::string unwrapped_head;
    };
    std::vector<JumpPredMatch> jump_matches;
    bool all_preds_match = true;
    for (size_t pred_b : preds[target_b]) {
      if (pred_b == ft_b) {
        continue;
      }
      const RtlBasicBlock& pblk = cfg->rpo_blocks[pred_b];
      if (pblk.successors.size() != 1 || pblk.successors[0] != target_b) {
        all_preds_match = false;
        break;
      }
      std::optional<size_t> seq_idx;
      for (size_t i = pblk.instructions.size(); i-- > 0;) {
        auto pn = ParsePatternAndNotes(pblk.instructions[i]);
        if (!pn.ok()) {
          break;
        }
        if (pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kBarrier) {
          continue;
        }
        seq_idx = i;
        break;
      }
      if (!seq_idx.has_value()) {
        all_preds_match = false;
        break;
      }
      auto spn = ParsePatternAndNotes(pblk.instructions[*seq_idx]);
      if (!spn.ok() || spn->pattern.op != RtlOp::kSequence || spn->pattern.children.size() != 2) {
        all_preds_match = false;
        break;
      }
      const RtlExpr& head_sub = spn->pattern.children[0];
      const RtlExpr& slot_sub = spn->pattern.children[1];
      if (head_sub.op != RtlOp::kJumpInsn || slot_sub.op != RtlOp::kInsn ||
          head_sub.flags.unchanging || slot_sub.flags.unchanging) {
        all_preds_match = false;
        break;
      }
      auto head_pn = RtlPatternAndNotes::FromSequenceSubInsn(head_sub);
      auto slot_pn = RtlPatternAndNotes::FromSequenceSubInsn(slot_sub);
      if (!head_pn.ok() || !slot_pn.ok() || !head_pn->pattern.IsSet() ||
          head_pn->pattern.SetDest().op != RtlOp::kPc ||
          head_pn->pattern.SetSrc().op != RtlOp::kLabelRef ||
          !(slot_pn->pattern == ft_last_pn->pattern) ||
          block_has_eligible_simple_delay_slot(pblk, *seq_idx)) {
        all_preds_match = false;
        break;
      }
      if (ft_prev_pat.has_value()) {
        std::optional<RtlExpr> pred_prev_pat;
        for (size_t i = *seq_idx; i-- > 0;) {
          auto pn = ParsePatternAndNotes(pblk.instructions[i]);
          if (!pn.ok()) {
            break;
          }
          if (pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kUse ||
              pn->pattern.op == RtlOp::kClobber) {
            continue;
          }
          pred_prev_pat = pn->pattern;
          break;
        }
        if (pred_prev_pat.has_value() && *pred_prev_pat == *ft_prev_pat) {
          all_preds_match = false;
          break;
        }
      }
      jump_matches.push_back(JumpPredMatch{
          .pred_b = pred_b,
          .seq_idx = *seq_idx,
          .unwrapped_head = FormatGccPrintRtlPatternAndNotes(
              head_pn->pattern, head_pn->reg_notes, head_pn->bracketed_notes, /*call_fusage=*/{}),
      });
    }
    if (!all_preds_match || jump_matches.empty()) {
      continue;
    }

    // Move `ft_last` from the tail of `ft_b` to the head of `target_b` and drop
    // the stolen copy from every jumping predecessor's delay slot.
    RtlBasicBlock& mut_ft = cfg->rpo_blocks[ft_b];
    std::string moved_insn = mut_ft.instructions[*ft_last_idx];
    const std::vector<RtlInsn> prev_ft_rtl = mut_ft.rtl_insns;
    mut_ft.instructions.erase(mut_ft.instructions.begin() + static_cast<ptrdiff_t>(*ft_last_idx));
    mut_ft.SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < prev_ft_rtl.size(); ++i) {
      if (i == *ft_last_idx || !prev_ft_rtl[i].has_explicit_call_fusage) {
        continue;
      }
      const size_t ni = (i < *ft_last_idx) ? i : (i - 1);
      if (ni < mut_ft.rtl_insns.size() && !mut_ft.rtl_insns[ni].has_explicit_call_fusage) {
        mut_ft.rtl_insns[ni].has_explicit_call_fusage = true;
        mut_ft.rtl_insns[ni].call_fusage = prev_ft_rtl[i].call_fusage;
      }
    }

    PrependInsnToBlockAfterLeadingLoopNotes(&cfg->rpo_blocks[target_b], std::move(moved_insn));
    for (const JumpPredMatch& jm : jump_matches) {
      RtlBasicBlock& pblk = cfg->rpo_blocks[jm.pred_b];
      const std::vector<RtlInsn> prev_p_rtl = pblk.rtl_insns;
      pblk.instructions[jm.seq_idx] = jm.unwrapped_head;
      pblk.SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < prev_p_rtl.size() && i < pblk.rtl_insns.size(); ++i) {
        if (prev_p_rtl[i].has_explicit_call_fusage && !pblk.rtl_insns[i].has_explicit_call_fusage) {
          pblk.rtl_insns[i].has_explicit_call_fusage = true;
          pblk.rtl_insns[i].call_fusage = prev_p_rtl[i].call_fusage;
        }
      }
    }
  }
}

}  // namespace

RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x,
                                               const CompilationProfile& profile) {
  if (!IsValidCfg(dbr_simple_cfg_x)) {
    return {};
  }

  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(dbr_simple_cfg_x, &profile, &gp64, &fp64);

  RtlControlFlowGraph out = dbr_simple_cfg_x;
  out.pass = RtlPassKind::kJump2;
  InvertSimpleJumpTargetStolenDelaySlots(&out, flags, gp64, fp64);
  std::vector<UnpackedDelaySlotSite> unpacked_sites;

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    std::vector<std::string> new_insns;
    std::vector<std::pair<bool, std::vector<RtlExpr>>> new_fusage;
    std::vector<UnpackedDelaySlotSite> block_sites;
    new_insns.reserve(out.rpo_blocks[b].instructions.size() + 2);
    new_fusage.reserve(out.rpo_blocks[b].instructions.size() + 2);
    bool inserted_optimize_skip_block = false;
    auto push_orig_fusage = [&](size_t idx) {
      if (idx < out.rpo_blocks[b].rtl_insns.size() &&
          out.rpo_blocks[b].rtl_insns[idx].has_explicit_call_fusage) {
        new_fusage.emplace_back(true, out.rpo_blocks[b].rtl_insns[idx].call_fusage);
      } else {
        new_fusage.emplace_back(false, std::vector<RtlExpr>{});
      }
    };
    auto apply_fusage_to_block = [&](RtlBasicBlock* blk) {
      blk->SyncRtlInsnsFromInstructions();
      for (size_t k = 0; k < new_fusage.size() && k < blk->rtl_insns.size(); ++k) {
        if (new_fusage[k].first && !blk->rtl_insns[k].has_explicit_call_fusage) {
          blk->rtl_insns[k].has_explicit_call_fusage = true;
          blk->rtl_insns[k].call_fusage = new_fusage[k].second;
        }
      }
    };

    for (size_t i = 0; i < out.rpo_blocks[b].instructions.size(); ++i) {
      const std::string& insn_str = out.rpo_blocks[b].instructions[i];
      auto parsed = ParsePatternAndNotes(insn_str);
      if (!parsed.ok() || parsed->pattern.op != RtlOp::kSequence ||
          parsed->pattern.children.size() < 2) {
        new_insns.push_back(insn_str);
        push_orig_fusage(i);
        continue;
      }

      const RtlExpr& head_sub = parsed->pattern.children[0];
      const RtlExpr& slot_sub = parsed->pattern.children[1];
      auto head_pat = RtlPatternAndNotes::FromSequenceSubInsn(head_sub);
      auto slot_pat = RtlPatternAndNotes::FromSequenceSubInsn(slot_sub);
      if (!head_pat.ok() || !slot_pat.ok()) {
        new_insns.push_back(insn_str);
        push_orig_fusage(i);
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
          new_fusage.emplace_back(false, std::vector<RtlExpr>{});
          if (out.rpo_blocks[b].successors[0] == out.rpo_blocks[b].successors[1] &&
              b + 1 < out.rpo_blocks.size()) {
            out.rpo_blocks[b].instructions = std::move(new_insns);
            apply_fusage_to_block(&out.rpo_blocks[b]);
            RtlBasicBlock ft_blk;
            ft_blk.id = static_cast<uint32_t>(b + 1);
            ft_blk.has_label = false;
            ft_blk.instructions = {slot_pat->ToGccPrintRtlString()};
            ft_blk.SyncRtlInsnsFromInstructions();
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

      const size_t slot_insert_pos = ComputeSimpleDelaySlotUnpackPos(
          out, b, is_last_insn, head_sub, *head_pat, *slot_pat, new_insns, flags, gp64, fp64);
      for (UnpackedDelaySlotSite& prev_site : block_sites) {
        if (prev_site.slot_idx >= slot_insert_pos) {
          ++prev_site.slot_idx;
        }
        if (prev_site.head_idx >= slot_insert_pos) {
          ++prev_site.head_idx;
        }
      }
      new_insns.insert(new_insns.begin() + static_cast<ptrdiff_t>(slot_insert_pos),
                       FormatGccPrintRtlPatternAndNotes(slot_pat->pattern, slot_pat->reg_notes,
                                                        slot_pat->bracketed_notes,
                                                        /*call_fusage=*/{}));
      new_fusage.insert(new_fusage.begin() + static_cast<ptrdiff_t>(slot_insert_pos),
                        {false, std::vector<RtlExpr>{}});
      const size_t head_insert_pos = new_insns.size();
      new_insns.push_back(FormatGccPrintRtlPatternAndNotes(head_pat->pattern, head_pat->reg_notes,
                                                           head_pat->bracketed_notes,
                                                           /*call_fusage=*/{}));
      if (head_pat->has_explicit_call_fusage) {
        new_fusage.emplace_back(true, head_pat->call_fusage);
      } else {
        push_orig_fusage(i);
      }
      if (head_sub.op == RtlOp::kJumpInsn || head_sub.op == RtlOp::kCallInsn) {
        block_sites.push_back(UnpackedDelaySlotSite{
            .block_idx = b,
            .slot_idx = slot_insert_pos,
            .head_idx = head_insert_pos,
            .orig_block_size = 0,
            .head_is_call = (head_sub.op == RtlOp::kCallInsn),
        });
      }
    }

    if (inserted_optimize_skip_block) {
      ++b;
    } else {
      const size_t final_block_size = new_insns.size();
      for (UnpackedDelaySlotSite& site : block_sites) {
        site.orig_block_size = final_block_size;
        unpacked_sites.push_back(site);
      }
      out.rpo_blocks[b].instructions = std::move(new_insns);
      apply_fusage_to_block(&out.rpo_blocks[b]);
    }
  }

  std::optional<size_t> unpacked_epilogue_delay_idx;
  if (out.sideband.epilogue_delay_pattern.has_value() &&
      !out.sideband.epilogue_delay_pattern->empty() && !out.rpo_blocks.empty()) {
    auto ep_pn = ParsePatternAndNotes(*out.sideband.epilogue_delay_pattern);
    bool eligible_epilogue_delay =
        ep_pn.ok() && ep_pn->pattern.op != RtlOp::kSequence && ep_pn->pattern.op != RtlOp::kNote &&
        ep_pn->pattern.op != RtlOp::kUse && ep_pn->pattern.op != RtlOp::kClobber &&
        !IsAnyJumpOrReturnTopExpr(ep_pn->pattern) && !IsCallPattern(ep_pn->pattern);
    if (eligible_epilogue_delay) {
      if (ep_pn->pattern.IsSet() && ep_pn->pattern.SetSrc().op == RtlOp::kMem) {
        eligible_epilogue_delay = false;
      }
      const absl::flat_hash_set<int> saved_gprs(out.sideband.saved_gpr_regnos.begin(),
                                                out.sideband.saved_gpr_regnos.end());
      const absl::flat_hash_set<int> saved_fprs(out.sideband.saved_fpr_regnos.begin(),
                                                out.sideband.saved_fpr_regnos.end());
      ep_pn->pattern.Visit([&](const RtlExpr& n) {
        if (!eligible_epilogue_delay || !n.IsHardReg() || n.IsArgPointerReg()) {
          return;
        }
        const int r = n.RegNo();
        const int span = std::max(1, MipsHardRegNregs(r, n.mode, gp64, fp64));
        for (int w = r; w < r + span && w < kMipsFirstPseudoRegister; ++w) {
          if (w == 29 || w == 31 || (w == 30 && out.sideband.frame_pointer_needed) ||
              saved_gprs.contains(w) || saved_fprs.contains(w)) {
            eligible_epilogue_delay = false;
            return;
          }
        }
      });
    }
    if (eligible_epilogue_delay) {
      RtlBasicBlock& exit_blk = out.rpo_blocks.back();
      size_t insert_pos = exit_blk.instructions.size();
      while (insert_pos > 0) {
        auto tail_pn = ParsePatternAndNotes(exit_blk.instructions[insert_pos - 1]);
        if (!tail_pn.ok() ||
            (tail_pn->pattern.op != RtlOp::kUse && tail_pn->pattern.op != RtlOp::kClobber &&
             tail_pn->pattern.op != RtlOp::kNote)) {
          break;
        }
        --insert_pos;
      }
      const std::vector<RtlInsn> prev_exit_rtl = exit_blk.rtl_insns;
      exit_blk.instructions.insert(
          exit_blk.instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
          *out.sideband.epilogue_delay_pattern);
      exit_blk.SyncRtlInsnsFromInstructions();
      for (size_t k = 0; k < prev_exit_rtl.size(); ++k) {
        const size_t dst_k = (k < insert_pos) ? k : (k + 1);
        if (dst_k < exit_blk.rtl_insns.size() && prev_exit_rtl[k].has_explicit_call_fusage &&
            !exit_blk.rtl_insns[dst_k].has_explicit_call_fusage) {
          exit_blk.rtl_insns[dst_k].has_explicit_call_fusage = true;
          exit_blk.rtl_insns[dst_k].call_fusage = prev_exit_rtl[k].call_fusage;
        }
      }
      out.sideband.epilogue_delay_pattern.reset();
      unpacked_epilogue_delay_idx = insert_pos;
    }
  }

  SynchronizeCanonicalLabels(&out);

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    out.rpo_blocks[b].id = static_cast<uint32_t>(b);
    const std::vector<RtlInsn> prev_rtl = out.rpo_blocks[b].rtl_insns;
    out.rpo_blocks[b].SyncRtlInsnsFromInstructions();
    for (size_t k = 0; k < prev_rtl.size() && k < out.rpo_blocks[b].rtl_insns.size(); ++k) {
      if (prev_rtl[k].has_explicit_call_fusage &&
          !out.rpo_blocks[b].rtl_insns[k].has_explicit_call_fusage) {
        out.rpo_blocks[b].rtl_insns[k].has_explicit_call_fusage = true;
        out.rpo_blocks[b].rtl_insns[k].call_fusage = prev_rtl[k].call_fusage;
      }
    }
  }
  const std::vector<PreservedFallthroughZeroDead> preserved_ft_zero_dead =
      CollectFallthroughRelaxedZeroDeadInsns(out);
  RecomputePostReloadLiveAtStart(&out, profile);
  HoistDelaySlotSplitDiAddSubInsns(&out, &unpacked_sites, flags, gp64, fp64);
  RecombineReorgSplitIdioms(&out, profile, unpacked_epilogue_delay_idx);
  ResolvePostReloadConstZeroRegDeadNotes(&out, profile);
  RestoreFallthroughRelaxedZeroDeadInsns(&out, preserved_ft_zero_dead);
  if (flags.RunsSched1()) {
    SelectSched1AchievableDelaySlotPositions(&out, unpacked_sites, profile, flags, gp64, fp64);
    // Re-canonicalize `REG_DEAD`/`REG_UNUSED` order after resolving delay-slot
    // `(const_int 0)` datums when `sched1` (`sched.c:2779-3018, 3573`) produced
    // the pre-reload `REG_DEAD` notes.
    CanonicalizeCfgLivenessNoteOrder(&out, profile, /*sched_note_order=*/true,
                                     /*reload_completed=*/true);
  } else if (flags.opt_level > 0 && !flags.RunsSched2()) {
    // Mirrors `reload1.c:6583` (`emit_reload_insns`) + `reorg.c:2901-2955`:
    // at `-O1` (where neither `sched1` nor `sched2` runs), `reorg.c` can pull
    // an independent constant-load / move ahead of a `JUMP_INSN`'s stack-slot
    // input reload `(set (reg r) (mem [sp/fp+off]))` into the jump's delay slot.
    // Re-adjoin the input reload immediately before the jump when `reorg.c`'s
    // backward scan uniquely re-selects the unpacked slot from before the reload.
    for (const UnpackedDelaySlotSite& site : unpacked_sites) {
      if (site.head_is_call || site.block_idx >= out.rpo_blocks.size()) {
        continue;
      }
      RtlBasicBlock& blk = out.rpo_blocks[site.block_idx];
      if (blk.instructions.size() != site.orig_block_size || site.slot_idx == 0 ||
          site.slot_idx + 1 != site.head_idx || site.head_idx >= blk.instructions.size()) {
        continue;
      }
      auto prev_pn = ParsePatternAndNotes(blk.instructions[site.slot_idx - 1]);
      auto slot_pn = ParsePatternAndNotes(blk.instructions[site.slot_idx]);
      auto head_pn = ParsePatternAndNotes(blk.instructions[site.head_idx]);
      if (!prev_pn.ok() || !slot_pn.ok() || !head_pn.ok() || !IsCondJumpExpr(head_pn->pattern)) {
        continue;
      }
      const RtlExpr& ppat = prev_pn->pattern;
      if (!ppat.IsSet() || !ppat.SetDest().IsHardReg() || ppat.SetDest().flags.integrated ||
          !ppat.SetSrc().IsMem() || ppat.SetSrc().flags.volatil || ppat.SetSrc().flags.in_struct ||
          ppat.SetSrc().children.empty()) {
        continue;
      }
      bool prev_has_equiv = false;
      for (const RtlRegNote& n : prev_pn->reg_notes) {
        if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual) {
          prev_has_equiv = true;
          break;
        }
      }
      if (prev_has_equiv) {
        continue;
      }
      const RtlExpr& paddr = ppat.SetSrc().children[0];
      const bool prev_is_stack_load =
          (paddr.IsHardReg() && (paddr.RegNo() == 29 || paddr.RegNo() == 30)) ||
          (paddr.op == RtlOp::kPlus && paddr.children.size() == 2 &&
           paddr.children[0].IsHardReg() &&
           (paddr.children[0].RegNo() == 29 || paddr.children[0].RegNo() == 30) &&
           paddr.children[1].IsConstInt());
      const int reload_r = ppat.SetDest().RegNo();
      if (!prev_is_stack_load || reload_r < 2 || reload_r >= 64 ||
          !head_pn->pattern.ContainsReg(reload_r)) {
        continue;
      }
      bool reload_live_out = false;
      for (size_t succ : blk.successors) {
        if (const auto lit = out.sideband.basic_block_live_at_start.find(static_cast<int>(succ));
            lit != out.sideband.basic_block_live_at_start.end() && lit->second.contains(reload_r)) {
          reload_live_out = true;
          break;
        }
      }
      if (reload_live_out) {
        continue;
      }
      const RtlExpr& spat = slot_pn->pattern;
      if (!spat.IsSet() || !spat.SetDest().IsHardReg() || spat.SetDest().RegNo() == reload_r ||
          spat.SetDest().RegNo() == 29 || spat.SetDest().RegNo() == 30 ||
          spat.ContainsReg(reload_r) ||
          (!spat.SetSrc().IsConstInt() && !spat.SetSrc().IsHardReg())) {
        continue;
      }
      std::vector<std::string> without_slot = blk.instructions;
      without_slot.erase(without_slot.begin() + static_cast<ptrdiff_t>(site.slot_idx));
      if (!CanReorgBackwardScanPickSlotAt(without_slot, site.slot_idx - 1, site.head_idx - 1,
                                          *slot_pn, head_pn->pattern, gp64, fp64,
                                          flags.small_data_threshold,
                                          /*allow_update_reg_dead_notes=*/false)) {
        continue;
      }
      std::swap(blk.instructions[site.slot_idx - 1], blk.instructions[site.slot_idx]);
      if (blk.rtl_insns.size() == blk.instructions.size()) {
        std::swap(blk.rtl_insns[site.slot_idx - 1], blk.rtl_insns[site.slot_idx]);
      }
    }
  }

  return out;
}

RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x) {
  const CompilationProfile default_profile{.optimize_level = 2};
  return InvertFillSimpleDelaySlots(dbr_simple_cfg_x, default_profile);
}

}  // namespace rom_nom_nom::lifter
