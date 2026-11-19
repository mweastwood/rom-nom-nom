#include "lifter/post_reload_noop_moves.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

RtlExpr MakeGccHardReg(RtlMachineMode mode, int regno, RtlFlags flags = {}) {
  RtlExpr reg = RtlExpr::MakeReg(mode, regno, flags);
  if (regno == 30) {
    reg.str_val = "$fp";
  }
  return reg;
}

std::string FormatInsnWithoutCallFusage(const RtlPatternAndNotes& pn) {
  return FormatGccPrintRtlPatternAndNotes(pn.pattern, pn.reg_notes, pn.bracketed_notes,
                                          /*call_fusage=*/{});
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

bool IsSimpleUncondJumpInsn(std::string_view pat) {
  auto parsed = ParsePatternAndNotes(pat);
  return parsed.ok() && IsSimpleUncondJumpExpr(parsed->pattern);
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
        blk.instructions[i] = FormatInsnWithoutCallFusage(*parsed);
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

bool IsMipsO32CallArgReg(int regno) {
  return (regno >= 4 && regno <= 7) || (regno >= 44 && regno <= 47);
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

void RestoreDeletedNoopSharedReturnMove(RtlControlFlowGraph* cfg,
                                        const CompilationProfile& profile) {
  const size_t n_blocks = cfg->rpo_blocks.size();
  if (n_blocks < 3) {
    return;
  }
  const size_t exit_b = n_blocks - 1;
  RtlBasicBlock& exit_blk = cfg->rpo_blocks[exit_b];
  if (!exit_blk.successors.empty() || !exit_blk.addr_vec_targets.empty()) {
    return;
  }
  std::optional<size_t> use_idx;
  int ret_hr = -1;
  RtlMachineMode ret_mode = RtlMachineMode::kNone;
  for (size_t i = 0; i < exit_blk.instructions.size(); ++i) {
    auto p = ParsePatternAndNotes(exit_blk.instructions[i]);
    if (!p.ok()) {
      return;
    }
    if (p->pattern.op == RtlOp::kNote) {
      continue;
    }
    if (!use_idx.has_value() && p->pattern.op == RtlOp::kUse && p->pattern.children.size() == 1 &&
        p->pattern.children[0].IsHardReg() && p->pattern.children[0].flags.integrated) {
      const int cand_hr = p->pattern.children[0].RegNo();
      const RtlMachineMode cand_mode = p->pattern.children[0].mode;
      const bool valid_scalar_ret =
          (cand_hr == 2 &&
           (cand_mode == RtlMachineMode::kSi || cand_mode == RtlMachineMode::kDi)) ||
          (cand_hr == 32 && (cand_mode == RtlMachineMode::kSf || cand_mode == RtlMachineMode::kDf));
      if (valid_scalar_ret) {
        use_idx = i;
        ret_hr = cand_hr;
        ret_mode = cand_mode;
        continue;
      }
    }
    return;
  }
  if (!use_idx.has_value()) {
    return;
  }

  std::vector<size_t> exit_preds;
  for (size_t b = 0; b + 1 < n_blocks; ++b) {
    for (size_t s : cfg->rpo_blocks[b].successors) {
      if (s == exit_b) {
        exit_preds.push_back(b);
      }
    }
  }
  if (exit_preds.size() < 2 || exit_preds.back() != exit_b - 1) {
    return;
  }

  bool all_preds_volatil = true;
  for (size_t pb : exit_preds) {
    const RtlBasicBlock& pblk = cfg->rpo_blocks[pb];
    if (pblk.successors.size() != 1 || pblk.successors[0] != exit_b ||
        !pblk.addr_vec_targets.empty()) {
      return;
    }
    std::optional<RtlPatternAndNotes> last_active;
    for (size_t rev = pblk.instructions.size(); rev > 0; --rev) {
      auto p = ParsePatternAndNotes(pblk.instructions[rev - 1]);
      if (!p.ok()) {
        return;
      }
      if (p->pattern.op == RtlOp::kNote || IsSimpleUncondJumpInsn(pblk.instructions[rev - 1])) {
        continue;
      }
      last_active = std::move(*p);
      break;
    }
    if (!last_active.has_value() || !last_active->pattern.IsSet() ||
        !last_active->pattern.SetDest().IsHardReg()) {
      return;
    }
    const RtlExpr& dst = last_active->pattern.SetDest();
    if (dst.RegNo() != ret_hr || dst.mode != ret_mode || dst.flags.integrated) {
      return;
    }
    if (!dst.flags.volatil) {
      all_preds_volatil = false;
    }
  }

  if (!HasDeletedSharedReturnMoveWitness(*cfg, profile, ret_hr, ret_mode)) {
    return;
  }

  RtlPatternAndNotes ret_move;
  const RtlExpr src_reg =
      RtlExpr::MakeReg(ret_mode, ret_hr, RtlFlags{.volatil = all_preds_volatil});
  ret_move.pattern =
      RtlExpr::MakeSet(RtlExpr::MakeReg(ret_mode, ret_hr, RtlFlags{.integrated = true}), src_reg);
  ret_move.reg_notes.push_back(RtlRegNote{
      .kind = RtlMachineMode::kRegDead,
      .list_op = RtlOp::kExprList,
      .datum = src_reg,
  });
  exit_blk.instructions.insert(exit_blk.instructions.begin() + static_cast<ptrdiff_t>(*use_idx),
                               ret_move.ToGccPrintRtlString());
  exit_blk.SyncRtlInsnsFromInstructions();
  RecomputePostReloadLiveAtStart(cfg, profile);
}

struct NaturalLoopInfo {
  size_t header = 0;
  std::vector<bool> members;
};

bool IsPrologueSetupInsn(const RtlExpr& pat) {
  int saved_reg = -1;
  int64_t sp_off = 0;
  if (IsPrologueStackRegSave(pat, &saved_reg, &sp_off)) {
    return true;
  }
  if (!pat.IsSet() || !pat.SetDest().IsHardReg()) {
    return false;
  }
  const int dst = pat.SetDest().RegNo();
  const RtlExpr& src = pat.SetSrc();
  if (dst == 29 && src.op == RtlOp::kPlus && src.children.size() == 2 &&
      src.children[0].IsHardReg() && src.children[0].RegNo() == 29 &&
      src.children[1].IsConstInt() && src.children[1].int_val < 0) {
    return true;
  }
  return dst == 30 && src.IsHardReg() && src.RegNo() == 29;
}

std::optional<size_t> FindEntryBodyInsertPos(const RtlBasicBlock& b0) {
  size_t insert_pos = 0;
  for (size_t i = 0; i < b0.instructions.size(); ++i) {
    auto parsed = ParsePatternAndNotes(b0.instructions[i]);
    if (!parsed.ok()) {
      return std::nullopt;
    }
    if (parsed->pattern.op == RtlOp::kNote || IsPrologueSetupInsn(parsed->pattern)) {
      insert_pos = i + 1;
      continue;
    }
    break;
  }
  return insert_pos;
}

bool HasPreTouchMultiCycleInEntryBlock(const std::vector<RtlPatternAndNotes>& b0_parsed,
                                       const std::vector<InsnSchedSummary>& b0_sums, size_t b0_pos,
                                       int arg_hr, int span) {
  for (size_t i = b0_pos; i < b0_parsed.size(); ++i) {
    const RtlExpr& pat = b0_parsed[i].pattern;
    if (pat.op == RtlOp::kNote) {
      continue;
    }
    if (IsAnyJumpOrReturnTopExpr(pat) || b0_sums[i].is_call) {
      break;
    }
    bool touches_arg = false;
    for (int w = 0; w < span; ++w) {
      if (b0_sums[i].read_regs.test(static_cast<size_t>(arg_hr + w)) ||
          b0_sums[i].written_regs.test(static_cast<size_t>(arg_hr + w))) {
        touches_arg = true;
        break;
      }
    }
    if (touches_arg) {
      break;
    }
    if (HasMultiCycleReadyLatency(pat)) {
      return true;
    }
  }
  return false;
}

int ExtractSetPreferenceSrcReg(const RtlExpr& src, RtlMachineMode expected_mode) {
  if (src.IsHardReg() && src.mode == expected_mode) {
    return src.RegNo();
  }
  if (!src.children.empty() && src.children[0].IsHardReg() &&
      src.children[0].mode == expected_mode && src.op != RtlOp::kCall &&
      src.op != RtlOp::kParallel && src.op != RtlOp::kSequence) {
    return src.children[0].RegNo();
  }
  return -1;
}

std::vector<NaturalLoopInfo> ComputeNaturalLoops(const RtlControlFlowGraph& cfg,
                                                 const std::vector<std::vector<size_t>>& preds,
                                                 std::vector<bool>* any_in_loop) {
  const size_t n = cfg.rpo_blocks.size();
  any_in_loop->assign(n, false);
  absl::flat_hash_map<size_t, size_t> header_to_idx;
  std::vector<NaturalLoopInfo> loops;
  for (size_t u = 0; u < n; ++u) {
    for (size_t h : cfg.rpo_blocks[u].successors) {
      if (h > u || h >= n) {
        continue;
      }
      size_t idx = 0;
      auto [it, inserted] = header_to_idx.try_emplace(h, loops.size());
      if (inserted) {
        loops.push_back(NaturalLoopInfo{.header = h, .members = std::vector<bool>(n, false)});
      }
      idx = it->second;
      auto& members = loops[idx].members;
      members[h] = true;
      std::vector<size_t> stack;
      if (!members[u]) {
        members[u] = true;
        stack.push_back(u);
      }
      while (!stack.empty()) {
        const size_t cur = stack.back();
        stack.pop_back();
        for (size_t p : preds[cur]) {
          if (p >= h && p < n && !members[p]) {
            members[p] = true;
            stack.push_back(p);
          }
        }
      }
    }
  }
  for (const auto& loop : loops) {
    for (size_t b = 0; b < n; ++b) {
      if (loop.members[b]) {
        (*any_in_loop)[b] = true;
      }
    }
  }
  return loops;
}

bool BlockLiveInContains(const RtlControlFlowGraph& cfg, size_t b, int regno) {
  const auto it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
  return it != cfg.sideband.basic_block_live_at_start.end() && it->second.contains(regno);
}

int LookupCallerAllocOrderIndex(int hr, bool is_fpr_class) {
  static constexpr std::array<int, 16> kCallerGprOrder = {2,  3,  4,  5,  6,  7,  8,  9,
                                                          10, 11, 12, 13, 14, 15, 24, 25};
  static constexpr std::array<int, 20> kCallerFprOrder = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                                                          42, 43, 44, 45, 46, 47, 48, 49, 50, 51};
  if (!is_fpr_class) {
    auto it = std::find(kCallerGprOrder.begin(), kCallerGprOrder.end(), hr);
    return (it != kCallerGprOrder.end()) ? static_cast<int>(it - kCallerGprOrder.begin()) : -1;
  }
  auto it = std::find(kCallerFprOrder.begin(), kCallerFprOrder.end(), hr);
  return (it != kCallerFprOrder.end()) ? static_cast<int>(it - kCallerFprOrder.begin()) : -1;
}

bool HasSkippedFreeLowerCallerRegWitness(const RtlControlFlowGraph& cfg,
                                         const std::vector<std::vector<InsnSchedSummary>>& blk_sums,
                                         const std::bitset<64>& written_anywhere,
                                         const NaturalLoopInfo& loop, int pref_hr, int arg_hr,
                                         int span, bool is_fpr_class) {
  static constexpr std::array<int, 16> kCallerGprOrder = {2,  3,  4,  5,  6,  7,  8,  9,
                                                          10, 11, 12, 13, 14, 15, 24, 25};
  static constexpr std::array<int, 20> kCallerFprOrder = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                                                          42, 43, 44, 45, 46, 47, 48, 49, 50, 51};
  const int pref_idx = LookupCallerAllocOrderIndex(pref_hr, is_fpr_class);
  const int order_len = is_fpr_class ? static_cast<int>(kCallerFprOrder.size())
                                     : static_cast<int>(kCallerGprOrder.size());
  if (pref_idx < 0 || pref_idx + 1 >= order_len || (span > 1 && (pref_hr % 2) != 0)) {
    return false;
  }
  const size_t n = cfg.rpo_blocks.size();
  for (size_t b = 0; b < n; ++b) {
    if (!loop.members[b]) {
      continue;
    }
    for (int w = 0; w < span; ++w) {
      if (BlockLiveInContains(cfg, b, pref_hr + w)) {
        return false;
      }
      for (const InsnSchedSummary& s : blk_sums[b]) {
        if (s.read_regs.test(static_cast<size_t>(pref_hr + w)) ||
            s.written_regs.test(static_cast<size_t>(pref_hr + w))) {
          return false;
        }
      }
    }
  }
  for (int idx = pref_idx + 1; idx < order_len; ++idx) {
    const int hr_w = is_fpr_class ? kCallerFprOrder[idx] : kCallerGprOrder[idx];
    if ((hr_w >= arg_hr && hr_w < arg_hr + span) || (hr_w >= pref_hr && hr_w < pref_hr + span) ||
        !written_anywhere.test(static_cast<size_t>(hr_w)) ||
        !BlockLiveInContains(cfg, loop.header, hr_w)) {
      continue;
    }
    return true;
  }
  return false;
}

bool MatchesUnmodifiedLoopArgPreferenceCaseA(
    const RtlControlFlowGraph& cfg, const std::vector<bool>& live_web,
    const std::vector<std::vector<RtlPatternAndNotes>>& blk_parsed,
    const std::vector<std::vector<InsnSchedSummary>>& blk_sums,
    const std::bitset<64>& written_anywhere, const std::vector<NaturalLoopInfo>& loops,
    const std::vector<bool>& any_in_loop, int arg_hr, RtlMachineMode arg_mode, int span) {
  const bool is_fpr_class = (arg_hr >= 32 && arg_hr < 64);
  const size_t n = cfg.rpo_blocks.size();
  for (size_t b = 1; b < n; ++b) {
    if (!live_web[b] || any_in_loop[b]) {
      continue;
    }
    for (const RtlPatternAndNotes& pn : blk_parsed[b]) {
      if (!pn.pattern.IsSet() || !pn.pattern.SetDest().IsHardReg() ||
          (is_fpr_class && pn.pattern.SetDest().mode != arg_mode) ||
          !HasRegDeadForRegno(pn, arg_hr)) {
        continue;
      }
      const int pref_hr = pn.pattern.SetDest().RegNo();
      if (LookupCallerAllocOrderIndex(pref_hr, is_fpr_class) < 0 || pref_hr == arg_hr ||
          ExtractSetPreferenceSrcReg(pn.pattern.SetSrc(), arg_mode) != arg_hr) {
        continue;
      }
      for (const NaturalLoopInfo& loop : loops) {
        if (live_web[loop.header] && BlockLiveInContains(cfg, loop.header, arg_hr) &&
            HasSkippedFreeLowerCallerRegWitness(cfg, blk_sums, written_anywhere, loop, pref_hr,
                                                arg_hr, span, is_fpr_class)) {
          return true;
        }
      }
    }
  }
  return false;
}

std::optional<size_t> FindSequentialLoopPreheaderArgCopyCaseB(
    const RtlControlFlowGraph& cfg, const std::vector<bool>& live_web,
    const std::vector<std::vector<size_t>>& preds,
    const std::vector<std::vector<RtlPatternAndNotes>>& blk_parsed,
    const std::vector<std::vector<InsnSchedSummary>>& blk_sums,
    const std::bitset<64>& written_anywhere, const std::vector<NaturalLoopInfo>& loops,
    const std::vector<bool>& any_in_loop, bool runs_sched2, int arg_hr, RtlMachineMode arg_mode,
    int span) {
  const bool is_fpr_class = (arg_hr >= 32 && arg_hr < 64);
  const size_t n = cfg.rpo_blocks.size();
  const NaturalLoopInfo* writer_loop = nullptr;
  for (size_t b = 1; b < n; ++b) {
    if (!live_web[b]) {
      continue;
    }
    bool writes_b = false;
    for (const InsnSchedSummary& s : blk_sums[b]) {
      for (int w = 0; w < span; ++w) {
        if (s.written_regs.test(static_cast<size_t>(arg_hr + w))) {
          writes_b = true;
          break;
        }
      }
      if (writes_b) {
        break;
      }
    }
    if (!writes_b) {
      continue;
    }
    const NaturalLoopInfo* found = nullptr;
    for (const NaturalLoopInfo& loop : loops) {
      if (loop.members[b]) {
        found = &loop;
        break;
      }
    }
    if (found == nullptr || (writer_loop != nullptr && writer_loop != found)) {
      return std::nullopt;
    }
    writer_loop = found;
  }
  if (writer_loop == nullptr || writer_loop->header < 2) {
    return std::nullopt;
  }
  for (size_t b = 0; b < n; ++b) {
    if (!writer_loop->members[b]) {
      continue;
    }
    for (size_t succ : cfg.rpo_blocks[b].successors) {
      if (succ < n && !writer_loop->members[succ] && BlockLiveInContains(cfg, succ, arg_hr)) {
        return std::nullopt;
      }
    }
  }

  std::optional<size_t> pre_b;
  for (size_t p : preds[writer_loop->header]) {
    if (writer_loop->members[p]) {
      continue;
    }
    if (pre_b.has_value()) {
      return std::nullopt;
    }
    pre_b = p;
  }
  if (!pre_b.has_value() || *pre_b == 0 || *pre_b >= writer_loop->header || !live_web[*pre_b] ||
      any_in_loop[*pre_b] || cfg.rpo_blocks[*pre_b].successors.size() != 1 ||
      !BlockLiveInContains(cfg, *pre_b, arg_hr)) {
    return std::nullopt;
  }
  if (runs_sched2) {
    for (size_t i = 0; i < blk_parsed[*pre_b].size(); ++i) {
      const RtlExpr& pat = blk_parsed[*pre_b][i].pattern;
      if (pat.op == RtlOp::kNote) {
        continue;
      }
      bool reads_any = false;
      for (int w = 0; w < span; ++w) {
        if (blk_sums[*pre_b][i].read_regs.test(static_cast<size_t>(arg_hr + w))) {
          reads_any = true;
        }
      }
      if (reads_any) {
        break;
      }
      if (HasMultiCycleReadyLatency(pat)) {
        return std::nullopt;
      }
    }
  }

  for (size_t b = 0; b < n; ++b) {
    if (!writer_loop->members[b]) {
      continue;
    }
    for (const RtlPatternAndNotes& pn : blk_parsed[b]) {
      if (!pn.pattern.IsSet() || !pn.pattern.SetDest().IsHardReg() ||
          (is_fpr_class && pn.pattern.SetDest().mode != arg_mode)) {
        continue;
      }
      const int pref_hr = pn.pattern.SetDest().RegNo();
      if (LookupCallerAllocOrderIndex(pref_hr, is_fpr_class) < 0 || pref_hr == arg_hr ||
          ExtractSetPreferenceSrcReg(pn.pattern.SetSrc(), arg_mode) != arg_hr) {
        continue;
      }
      for (const NaturalLoopInfo& earlier_loop : loops) {
        if (earlier_loop.header < writer_loop->header &&
            !earlier_loop.members[writer_loop->header] && live_web[earlier_loop.header] &&
            BlockLiveInContains(cfg, earlier_loop.header, arg_hr) &&
            HasSkippedFreeLowerCallerRegWitness(cfg, blk_sums, written_anywhere, earlier_loop,
                                                pref_hr, arg_hr, span, is_fpr_class)) {
          return pre_b;
        }
      }
    }
  }
  return std::nullopt;
}

bool DirectExprOperandMentionsReg(const RtlExpr& src, int regno) {
  auto is_reg_or_subreg_of = [regno](const RtlExpr& e) {
    if (e.IsHardReg() && e.RegNo() == regno) {
      return true;
    }
    return e.IsSubreg() && !e.children.empty() && e.children[0].IsHardReg() &&
           e.children[0].RegNo() == regno;
  };
  if (is_reg_or_subreg_of(src)) {
    return true;
  }
  if (src.op == RtlOp::kMem) {
    return false;
  }
  for (const RtlExpr& ch : src.children) {
    if (is_reg_or_subreg_of(ch)) {
      return true;
    }
  }
  return false;
}

bool MatchesLocalAllocMultiBlockArgSuggCaseC(
    const RtlControlFlowGraph& cfg, const std::vector<bool>& live_web,
    const std::vector<std::vector<RtlPatternAndNotes>>& blk_parsed,
    const std::vector<std::vector<InsnSchedSummary>>& blk_sums, bool gp64, bool fp64, int arg_hr,
    int span) {
  const bool is_fpr_class = (arg_hr >= 32 && arg_hr < 64);
  const int arg_order_idx = LookupCallerAllocOrderIndex(arg_hr, is_fpr_class);
  if (arg_order_idx < 0) {
    return false;
  }
  const size_t n = cfg.rpo_blocks.size();
  for (size_t b = 1; b < n; ++b) {
    if (!live_web[b] || !BlockLiveInContains(cfg, b, arg_hr)) {
      continue;
    }
    bool arg_still_entry = true;
    for (size_t i = 0; i < blk_parsed[b].size() && arg_still_entry; ++i) {
      const RtlPatternAndNotes& pn = blk_parsed[b][i];
      bool writes_arg = false;
      for (int w = 0; w < span; ++w) {
        if (blk_sums[b][i].written_regs.test(static_cast<size_t>(arg_hr + w))) {
          writes_arg = true;
        }
      }
      if (pn.pattern.IsSet() && pn.pattern.SetDest().IsHardReg() &&
          !pn.pattern.SetDest().flags.integrated && !writes_arg && HasRegDeadForRegno(pn, arg_hr) &&
          DirectExprOperandMentionsReg(pn.pattern.SetSrc(), arg_hr)) {
        const int dst_hr = pn.pattern.SetDest().RegNo();
        const RtlMachineMode dst_mode = pn.pattern.SetDest().mode;
        const int dst_span = std::max(1, MipsHardRegNregs(dst_hr, dst_mode, gp64, fp64));
        const int dst_order_idx = LookupCallerAllocOrderIndex(dst_hr, is_fpr_class);
        if (dst_order_idx >= 0 && dst_hr != arg_hr && (dst_span == 1 || (arg_hr % 2) == 0) &&
            arg_hr + dst_span <= (is_fpr_class ? 52 : 26)) {
          bool arg_span_free_before = true;
          for (int w = span; w < dst_span; ++w) {
            if (BlockLiveInContains(cfg, b, arg_hr + w)) {
              arg_span_free_before = false;
            }
            for (size_t p_i = 0; p_i <= i && arg_span_free_before; ++p_i) {
              if (blk_sums[b][p_i].read_regs.test(static_cast<size_t>(arg_hr + w)) ||
                  blk_sums[b][p_i].written_regs.test(static_cast<size_t>(arg_hr + w))) {
                arg_span_free_before = false;
              }
            }
          }
          std::array<bool, 4> word_live_out{};
          bool dst_live_out = false;
          for (size_t succ : cfg.rpo_blocks[b].successors) {
            for (int w = 0; w < dst_span && w < 4; ++w) {
              if (succ < n && BlockLiveInContains(cfg, succ, dst_hr + w)) {
                word_live_out[static_cast<size_t>(w)] = true;
                dst_live_out = true;
              }
            }
          }
          if (arg_span_free_before) {
            bool found_local_death = false;
            bool blocked = false;
            size_t death_j = blk_parsed[b].size();
            for (size_t j = i + 1; j < blk_parsed[b].size(); ++j) {
              if (blk_sums[b][j].is_call) {
                blocked = true;
                break;
              }
              const RtlPatternAndNotes& jpn = blk_parsed[b][j];
              bool any_writes_dst = false;
              bool any_reads_dst = false;
              for (int w = 0; w < dst_span; ++w) {
                if (blk_sums[b][j].written_regs.test(static_cast<size_t>(dst_hr + w))) {
                  any_writes_dst = true;
                }
                if (blk_sums[b][j].read_regs.test(static_cast<size_t>(dst_hr + w))) {
                  any_reads_dst = true;
                }
              }
              const bool j_kills_dst_cleanly = !any_writes_dst && HasRegDeadForRegno(jpn, dst_hr);
              for (int w = 0; w < dst_span; ++w) {
                if (blk_sums[b][j].read_regs.test(static_cast<size_t>(arg_hr + w)) ||
                    (!j_kills_dst_cleanly &&
                     blk_sums[b][j].written_regs.test(static_cast<size_t>(arg_hr + w)))) {
                  blocked = true;
                  break;
                }
              }
              if (blocked) {
                break;
              }
              if (jpn.pattern.IsSet() && jpn.pattern.SetDest().IsHardReg() &&
                  DirectExprOperandMentionsReg(jpn.pattern.SetSrc(), dst_hr)) {
                const int h2 = jpn.pattern.SetDest().RegNo();
                if (h2 != dst_hr) {
                  const int h2_idx = LookupCallerAllocOrderIndex(h2, is_fpr_class);
                  if (h2_idx >= 0 && h2_idx < arg_order_idx) {
                    bool h2_free_across_ij = !BlockLiveInContains(cfg, b, h2);
                    for (size_t m = i; m < j && h2_free_across_ij; ++m) {
                      if ((m > i && blk_sums[b][m].read_regs.test(static_cast<size_t>(h2))) ||
                          blk_sums[b][m].written_regs.test(static_cast<size_t>(h2))) {
                        h2_free_across_ij = false;
                      }
                    }
                    bool h2_pinned_hard_reg = jpn.pattern.SetDest().flags.integrated;
                    if (!h2_pinned_hard_reg && ((h2 >= 4 && h2 <= (gp64 ? 11 : 7)) ||
                                                (h2 >= 44 && h2 <= (gp64 ? 51 : 47)))) {
                      for (size_t k = j + 1; k < blk_parsed[b].size(); ++k) {
                        if (blk_sums[b][k].is_call) {
                          h2_pinned_hard_reg = true;
                          break;
                        }
                        if (blk_sums[b][k].written_regs.test(static_cast<size_t>(h2))) {
                          break;
                        }
                      }
                    }
                    if (h2_free_across_ij && h2_pinned_hard_reg) {
                      blocked = true;
                      break;
                    }
                  }
                }
              }
              if (any_writes_dst && !any_reads_dst) {
                blocked = true;
                break;
              }
              if (any_writes_dst && any_reads_dst && jpn.pattern.IsSet() &&
                  !DirectExprOperandMentionsReg(jpn.pattern.SetSrc(), dst_hr)) {
                found_local_death = true;
                death_j = j;
                break;
              }
              if (!any_writes_dst && HasRegDeadForRegno(jpn, dst_hr)) {
                bool covers_all_words = (dst_span == 1);
                if (!covers_all_words) {
                  std::array<bool, 4> word_dead{};
                  for (const RtlRegNote& note : jpn.reg_notes) {
                    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg()) {
                      const int nr = note.datum.RegNo();
                      const int nw = std::max(1, MipsHardRegNregs(nr, note.datum.mode, gp64, fp64));
                      for (int w = 0; w < dst_span && w < 4; ++w) {
                        if (dst_hr + w >= nr && dst_hr + w < nr + nw) {
                          word_dead[static_cast<size_t>(w)] = true;
                        }
                      }
                    }
                  }
                  covers_all_words = true;
                  for (int w = 0; w < dst_span && w < 4; ++w) {
                    if (!word_dead[static_cast<size_t>(w)]) {
                      covers_all_words = false;
                    }
                  }
                }
                if (covers_all_words) {
                  found_local_death = true;
                  death_j = j;
                  break;
                }
              }
            }
            if (found_local_death && !blocked && dst_live_out) {
              std::array<bool, 4> word_redefined{};
              for (size_t k = death_j; k < blk_parsed[b].size(); ++k) {
                for (int w = 0; w < dst_span && w < 4; ++w) {
                  if (blk_sums[b][k].written_regs.test(static_cast<size_t>(dst_hr + w))) {
                    word_redefined[static_cast<size_t>(w)] = true;
                  }
                }
              }
              for (int w = 0; w < dst_span && w < 4; ++w) {
                if (word_live_out[static_cast<size_t>(w)] &&
                    !word_redefined[static_cast<size_t>(w)]) {
                  blocked = true;
                }
              }
            }
            if (found_local_death && !blocked) {
              return true;
            }
          }
        }
      }
      if (writes_arg || HasRegDeadForRegno(pn, arg_hr)) {
        arg_still_entry = false;
      }
    }
  }
  return false;
}

const RtlExpr* ExtractSingleSetInParallelOrSet(const RtlExpr& pat) {
  if (pat.IsSet()) {
    return &pat;
  }
  if (pat.op == RtlOp::kParallel) {
    const RtlExpr* found = nullptr;
    for (const RtlExpr& elem : pat.children) {
      if (elem.IsSet()) {
        if (found != nullptr) {
          return nullptr;
        }
        found = &elem;
      } else if (elem.op != RtlOp::kClobber && elem.op != RtlOp::kUse) {
        return nullptr;
      }
    }
    return found;
  }
  return nullptr;
}

bool MatchesLocalAllocEntryCopySuggCaseD(const std::vector<RtlPatternAndNotes>& b0_parsed,
                                         const std::vector<InsnSchedSummary>& b0_sums,
                                         size_t b0_pos, bool gp64, bool fp64, int arg_hr,
                                         int span) {
  if (span < 1 || span > 2) {
    return false;
  }
  const bool is_fpr_class = (arg_hr >= 32 && arg_hr < 64);
  const int arg_order_idx = LookupCallerAllocOrderIndex(arg_hr, is_fpr_class);
  if (arg_order_idx <= 0) {
    return false;
  }
  for (const InsnSchedSummary& s : b0_sums) {
    if (s.is_call) {
      return false;
    }
  }
  auto range_touched = [](const std::bitset<68>& bits, int base, int len) {
    for (int w = 0; w < len; ++w) {
      if (bits.test(static_cast<size_t>(base + w))) {
        return true;
      }
    }
    return false;
  };
  for (size_t k = 0; k < b0_pos && k < b0_sums.size(); ++k) {
    if (range_touched(b0_sums[k].written_regs, arg_hr, span)) {
      return false;
    }
  }
  int early_hr = -1;
  int early_span = 1;
  size_t i0 = b0_parsed.size();
  for (size_t i = b0_pos; i < b0_parsed.size(); ++i) {
    const bool reads_arg = range_touched(b0_sums[i].read_regs, arg_hr, span);
    const bool writes_arg = range_touched(b0_sums[i].written_regs, arg_hr, span);
    if (!reads_arg && !writes_arg) {
      continue;
    }
    const RtlPatternAndNotes& pn = b0_parsed[i];
    const RtlExpr* s0 = ExtractSingleSetInParallelOrSet(pn.pattern);
    if (writes_arg || HasRegDeadForRegno(pn, arg_hr) || s0 == nullptr ||
        !s0->SetDest().IsHardReg() || s0->SetDest().flags.integrated ||
        (s0->SetSrc().IsHardReg() && s0->SetSrc().RegNo() == arg_hr) ||
        !DirectExprOperandMentionsReg(s0->SetSrc(), arg_hr)) {
      return false;
    }
    const int d_hr = s0->SetDest().RegNo();
    const int d_span = std::max(1, MipsHardRegNregs(d_hr, s0->SetDest().mode, gp64, fp64));
    const int d_idx = LookupCallerAllocOrderIndex(d_hr, is_fpr_class);
    if (d_idx < 0 || d_idx >= arg_order_idx || d_span != span) {
      return false;
    }
    early_hr = d_hr;
    early_span = d_span;
    i0 = i;
    break;
  }
  if (early_hr < 0) {
    return false;
  }
  auto has_dead_in_range = [&](const RtlPatternAndNotes& pn, int base, int len) {
    for (int w = 0; w < len; ++w) {
      if (HasRegDeadForRegno(pn, base + w)) {
        return true;
      }
    }
    return false;
  };
  auto is_valid_early_rmw = [&](size_t idx) {
    const RtlPatternAndNotes& pn = b0_parsed[idx];
    const RtlExpr* st = ExtractSingleSetInParallelOrSet(pn.pattern);
    if (st == nullptr || !st->SetDest().IsHardReg() || st->SetDest().flags.integrated ||
        st->SetSrc().IsHardReg()) {
      return false;
    }
    const int wr = st->SetDest().RegNo();
    const int wspan = std::max(1, MipsHardRegNregs(wr, st->SetDest().mode, gp64, fp64));
    return wr >= early_hr && wr + wspan <= early_hr + early_span &&
           DirectExprOperandMentionsReg(st->SetSrc(), wr) &&
           !has_dead_in_range(pn, early_hr, early_span);
  };
  size_t i1 = b0_parsed.size();
  for (size_t i = i0 + 1; i < b0_parsed.size(); ++i) {
    if (has_dead_in_range(b0_parsed[i], early_hr, early_span)) {
      return false;
    }
    if (range_touched(b0_sums[i].written_regs, early_hr, early_span) && !is_valid_early_rmw(i)) {
      return false;
    }
    const bool reads_arg = range_touched(b0_sums[i].read_regs, arg_hr, span);
    const bool writes_arg = range_touched(b0_sums[i].written_regs, arg_hr, span);
    if (!writes_arg) {
      if (reads_arg && HasRegDeadForRegno(b0_parsed[i], arg_hr)) {
        // Check for a `!gp64` `DImode` `"=&d"` earlyclobber RMW split by `reload` into
        // `(parallel [(set scr_hr (<op> ... arg_hr ...)) ...]) (REG_DEAD arg_hr)`
        // followed by `(set arg_hr scr_hr)` without `REG_DEAD scr_hr`.
        const RtlExpr* s_ec = ExtractSingleSetInParallelOrSet(b0_parsed[i].pattern);
        if (s_ec != nullptr && s_ec->SetDest().IsHardReg() && !s_ec->SetDest().flags.integrated &&
            !s_ec->SetSrc().IsHardReg() && DirectExprOperandMentionsReg(s_ec->SetSrc(), arg_hr)) {
          const int scr_hr = s_ec->SetDest().RegNo();
          const int scr_span =
              std::max(1, MipsHardRegNregs(scr_hr, s_ec->SetDest().mode, gp64, fp64));
          if (scr_span == span && scr_hr != arg_hr && scr_hr != early_hr) {
            size_t copy_idx = b0_parsed.size();
            for (size_t m = i + 1; m < b0_parsed.size(); ++m) {
              if (has_dead_in_range(b0_parsed[m], early_hr, early_span) ||
                  (range_touched(b0_sums[m].written_regs, early_hr, early_span) &&
                   !is_valid_early_rmw(m))) {
                break;
              }
              const RtlExpr* sm = ExtractSingleSetInParallelOrSet(b0_parsed[m].pattern);
              if (sm != nullptr && sm->SetDest().IsHardReg() && !sm->SetDest().flags.integrated &&
                  sm->SetDest().RegNo() == arg_hr && sm->SetSrc().IsHardReg() &&
                  sm->SetSrc().RegNo() == scr_hr &&
                  !has_dead_in_range(b0_parsed[m], scr_hr, scr_span)) {
                copy_idx = m;
                break;
              }
              if (range_touched(b0_sums[m].read_regs, arg_hr, span) ||
                  range_touched(b0_sums[m].written_regs, arg_hr, span) ||
                  range_touched(b0_sums[m].read_regs, scr_hr, scr_span) ||
                  range_touched(b0_sums[m].written_regs, scr_hr, scr_span)) {
                break;
              }
            }
            if (copy_idx < b0_parsed.size()) {
              i1 = copy_idx;
              break;
            }
          }
        }
        return false;
      }
      continue;
    }
    const RtlPatternAndNotes& pn = b0_parsed[i];
    const RtlExpr* s1 = ExtractSingleSetInParallelOrSet(pn.pattern);
    if (!reads_arg || s1 == nullptr || !s1->SetDest().IsHardReg() ||
        s1->SetDest().flags.integrated || s1->SetDest().RegNo() != arg_hr ||
        !DirectExprOperandMentionsReg(s1->SetSrc(), arg_hr)) {
      return false;
    }
    i1 = i;
    break;
  }
  if (i1 >= b0_parsed.size()) {
    return false;
  }
  for (size_t i = i1 + 1; i < b0_parsed.size(); ++i) {
    const RtlPatternAndNotes& pn = b0_parsed[i];
    const RtlExpr* s2 = ExtractSingleSetInParallelOrSet(pn.pattern);
    if (s2 != nullptr && s2->SetDest().IsHardReg() && s2->SetDest().flags.integrated &&
        s2->SetDest().RegNo() == early_hr && HasRegDeadForRegno(pn, arg_hr) &&
        DirectExprOperandMentionsReg(s2->SetSrc(), early_hr) &&
        DirectExprOperandMentionsReg(s2->SetSrc(), arg_hr)) {
      return true;
    }
    if (range_touched(b0_sums[i].written_regs, arg_hr, span) ||
        has_dead_in_range(pn, early_hr, early_span) || HasRegDeadForRegno(pn, arg_hr)) {
      return false;
    }
    if (range_touched(b0_sums[i].written_regs, early_hr, early_span) && !is_valid_early_rmw(i)) {
      return false;
    }
  }
  return false;
}

bool IsLeadingNoopSelfMoveForReg(const RtlPatternAndNotes& pn, int* out_hr,
                                 RtlMachineMode* out_mode) {
  if (!pn.pattern.IsSet() || !pn.pattern.SetDest().IsHardReg() ||
      pn.pattern.SetDest().flags.integrated || !pn.pattern.SetSrc().IsHardReg() ||
      pn.pattern.SetDest().RegNo() != pn.pattern.SetSrc().RegNo() ||
      pn.pattern.SetDest().mode != pn.pattern.SetSrc().mode) {
    return false;
  }
  *out_hr = pn.pattern.SetDest().RegNo();
  *out_mode = pn.pattern.SetDest().mode;
  return IsMipsO32CallArgReg(*out_hr);
}

int LookupAbiArgCanonicalOrder(int hr) {
  static constexpr std::array<int, 8> kAbiArgRegs = {4, 5, 6, 7, 44, 45, 46, 47};
  for (size_t i = 0; i < kAbiArgRegs.size(); ++i) {
    if (kAbiArgRegs[i] == hr) {
      return static_cast<int>(i);
    }
  }
  return 100 + hr;
}

// Detects `!gp64` single-block `DImode` incoming GPR argument pairs (`4:5` or
// `6:7`) whose `(set (reg:DI p_arg) (reg:DI arg_hr))` self-move was deleted as
// a noop move and whose subwords are read strictly via `SImode` word subregs:
// one word `w_early` dies at `i0` on an operation writing `dst_hr` (`dst_hr >
// w_early` in `REG_ALLOC_ORDER`) while the sibling word `w_late` remains live
// until `i1 > i0`, proving `w_early` was still occupied at `i0` as part of the
// 2-word `DImode` parameter pseudo `p_arg`.
bool MatchesLocalAllocSingleBlockDiPairSuggCaseE(const RtlControlFlowGraph& cfg,
                                                 const std::vector<RtlPatternAndNotes>& b0_parsed,
                                                 const std::vector<InsnSchedSummary>& b0_sums,
                                                 size_t b0_pos, bool gp64, int arg_hr) {
  if (gp64 || cfg.rpo_blocks.size() != 1 || (arg_hr != 4 && arg_hr != 6) ||
      !BlockLiveInContains(cfg, 0, arg_hr) || !BlockLiveInContains(cfg, 0, arg_hr + 1)) {
    return false;
  }

  for (int early_off = 0; early_off < 2; ++early_off) {
    const int w_early = arg_hr + early_off;
    const int w_late = arg_hr + (1 - early_off);
    const int early_order = LookupCallerAllocOrderIndex(w_early, /*is_fpr_class=*/false);
    if (early_order < 0) {
      continue;
    }
    std::optional<size_t> death_i0;
    for (size_t i = b0_pos; i < b0_parsed.size(); ++i) {
      if (b0_sums[i].read_regs.test(static_cast<size_t>(w_early)) &&
          HasRegDeadForRegno(b0_parsed[i], w_early)) {
        death_i0 = i;
        break;
      }
      if (b0_sums[i].written_regs.test(static_cast<size_t>(w_early))) {
        break;
      }
    }
    if (!death_i0.has_value() || HasRegDeadForRegno(b0_parsed[*death_i0], w_late)) {
      continue;
    }
    const RtlExpr* s0 = ExtractSingleSetInParallelOrSet(b0_parsed[*death_i0].pattern);
    if (s0 == nullptr || !s0->SetDest().IsHardReg() || s0->SetDest().flags.integrated ||
        s0->SetDest().mode != RtlMachineMode::kSi) {
      continue;
    }
    const int dst_hr = s0->SetDest().RegNo();
    const int dst_order = LookupCallerAllocOrderIndex(dst_hr, /*is_fpr_class=*/false);
    if (dst_order <= early_order) {
      continue;
    }
    std::optional<size_t> death_i1;
    for (size_t i1 = *death_i0 + 1; i1 < b0_parsed.size(); ++i1) {
      if (b0_sums[i1].read_regs.test(static_cast<size_t>(w_late)) &&
          HasRegDeadForRegno(b0_parsed[i1], w_late)) {
        death_i1 = i1;
        break;
      }
      if (b0_sums[i1].written_regs.test(static_cast<size_t>(w_late))) {
        break;
      }
    }
    if (!death_i1.has_value()) {
      continue;
    }
    bool clean_prefix = true;
    for (size_t i = 0; i <= *death_i1; ++i) {
      if (b0_sums[i].is_call || b0_sums[i].written_regs.test(static_cast<size_t>(w_early)) ||
          b0_sums[i].written_regs.test(static_cast<size_t>(w_late)) ||
          (i > *death_i0 && b0_sums[i].read_regs.test(static_cast<size_t>(w_early)))) {
        clean_prefix = false;
        break;
      }
      bool non_si_mention = false;
      b0_parsed[i].pattern.Visit([&](const RtlExpr& node) {
        if (node.IsHardReg() && (node.RegNo() == w_early || node.RegNo() == w_late) &&
            node.mode != RtlMachineMode::kSi) {
          non_si_mention = true;
        }
      });
      if (non_si_mention) {
        clean_prefix = false;
        break;
      }
    }
    if (clean_prefix) {
      return true;
    }
  }
  return false;
}

// Single-block `local_alloc` witness (`Case F`): when the first active
// instruction of a single-block function is `movstrsi_internal` reading both a
// multi-use unmodified scalar GPR parameter `arg_hr` and a dying GPR `dead_hr`,
// `assign_parms` emitted `(set (reg/v:SI p_arg) (reg:SI arg_hr))` ahead of
// `movstrsi_internal` (surviving `combine` because `p_arg` has >= 2 uses),
// shifting `movstrsi_internal` to `insn_number >= 2` so `regs_live_at` marks
// `dead_hr` live at `movstrsi_internal`'s earlyclobber scratch birth.
bool MatchesLocalAllocEntryFirstInsnMovstrsiCaseF(const RtlControlFlowGraph& cfg,
                                                  const std::vector<RtlPatternAndNotes>& b0_parsed,
                                                  const std::vector<InsnSchedSummary>& b0_sums,
                                                  size_t b0_pos, int arg_hr) {
  if (cfg.rpo_blocks.size() != 1 || b0_pos >= b0_parsed.size() || arg_hr < 4 || arg_hr > 7) {
    return false;
  }
  const RtlPatternAndNotes& pn = b0_parsed[b0_pos];
  if (pn.pattern.op != RtlOp::kParallel || pn.pattern.children.size() != 8 ||
      !pn.pattern.children[0].IsSet() || !pn.pattern.children[0].SetDest().IsMem() ||
      pn.pattern.children[0].SetDest().mode != RtlMachineMode::kBlk ||
      !pn.pattern.children[0].SetSrc().IsMem() ||
      pn.pattern.children[0].SetSrc().mode != RtlMachineMode::kBlk ||
      !b0_sums[b0_pos].read_regs.test(static_cast<size_t>(arg_hr)) ||
      HasRegDeadForRegno(pn, arg_hr)) {
    return false;
  }
  bool read_again_later = false;
  for (size_t i = b0_pos + 1; i < b0_sums.size(); ++i) {
    if (b0_sums[i].read_regs.test(static_cast<size_t>(arg_hr))) {
      read_again_later = true;
      break;
    }
  }
  if (!read_again_later) {
    return false;
  }
  for (const RtlRegNote& n : pn.reg_notes) {
    if (n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg()) {
      const int dead_hr = n.datum.RegNo();
      if (dead_hr >= 2 && dead_hr <= 25 && dead_hr != arg_hr &&
          BlockLiveInContains(cfg, 0, dead_hr)) {
        return true;
      }
    }
  }
  return false;
}

// Reverses `jump.c:jump_optimize` (`noop_moves = 1`, line 432) and
// `sched.c:schedule_insns` (`reload_completed != 0`) deletion of
// `(set (reg:M arg_hr) (reg:M arg_hr))` on incoming scalar ABI parameters
// (`$4..$7`, `$f12`/`$f14`) when `global_alloc` or `local-alloc` (`block_alloc`)
// observed the entry parameter pseudo `p_arg` (`reg_renumber[p_arg] = arg_hr`).
enum class NoopSelfMoveStage {
  kAll,
  kJump2BlockEndOnly,
  kSched2NonBlockEndOnly,
  kSched2PreScheduleInsnsOnly,
};

void RestoreDeletedNoopArgPreferenceMoves(RtlControlFlowGraph* cfg,
                                          const CompilationProfile& profile,
                                          const ParsedCompilerFlags& flags, bool gp64, bool fp64,
                                          NoopSelfMoveStage stage) {
  const size_t n = cfg->rpo_blocks.size();
  if (n == 0) {
    return;
  }
  const std::optional<size_t> b0_pos = FindEntryBodyInsertPos(cfg->rpo_blocks[0]);
  if (!b0_pos.has_value()) {
    return;
  }

  std::vector<std::vector<size_t>> preds(n);
  std::vector<std::vector<RtlPatternAndNotes>> blk_parsed(n);
  std::vector<std::vector<InsnSchedSummary>> blk_sums(n);
  std::bitset<64> written_anywhere;
  for (size_t b = 0; b < n; ++b) {
    for (size_t s : cfg->rpo_blocks[b].successors) {
      if (s < n) {
        preds[s].push_back(b);
      }
    }
    const auto& insns = cfg->rpo_blocks[b].instructions;
    blk_parsed[b].reserve(insns.size());
    blk_sums[b].reserve(insns.size());
    for (const std::string& pat_s : insns) {
      auto p = ParsePatternAndNotes(pat_s);
      if (!p.ok()) {
        return;
      }
      InsnSchedSummary sum = SummarizeInsnForSched2(pat_s, gp64, fp64);
      for (int r = 2; r < 64; ++r) {
        if (sum.written_regs.test(static_cast<size_t>(r))) {
          written_anywhere.set(static_cast<size_t>(r));
        }
      }
      blk_parsed[b].push_back(std::move(*p));
      blk_sums[b].push_back(sum);
    }
  }

  std::vector<bool> any_in_loop;
  const std::vector<NaturalLoopInfo> loops = ComputeNaturalLoops(*cfg, preds, &any_in_loop);
  const bool check_sched2_multi_cycle =
      flags.RunsSched2() && stage != NoopSelfMoveStage::kSched2PreScheduleInsnsOnly;
  const bool b0_needs_new_preheader = !preds[0].empty();

  static constexpr std::array<int, 8> kAbiArgRegs = {4, 5, 6, 7, 44, 45, 46, 47};
  std::vector<std::pair<int, RtlMachineMode>> b0_restored_args;
  std::vector<std::tuple<size_t, int, RtlMachineMode>> preheader_restored_args;
  std::bitset<64> consumed_arg_words;

  // Mark any already-restored leading `(set (reg:M r) (reg:M r))` self-moves in `B0`
  // as consumed so multi-stage restoration (`jump2` -> `sched2` -> pre-`schedule_insns`)
  // is strictly idempotent.
  if (!b0_needs_new_preheader) {
    for (size_t i = *b0_pos; i < blk_parsed[0].size(); ++i) {
      if (blk_parsed[0][i].pattern.op == RtlOp::kNote) {
        continue;
      }
      int existing_hr = -1;
      RtlMachineMode existing_mode = RtlMachineMode::kNone;
      if (!IsLeadingNoopSelfMoveForReg(blk_parsed[0][i], &existing_hr, &existing_mode)) {
        break;
      }
      const int existing_span =
          std::max(1, MipsHardRegNregs(existing_hr, existing_mode, gp64, fp64));
      for (int w = 0; w < existing_span; ++w) {
        if (existing_hr + w < 64) {
          consumed_arg_words.set(static_cast<size_t>(existing_hr + w));
        }
      }
    }
  }

  for (int arg_hr : kAbiArgRegs) {
    if (consumed_arg_words.test(static_cast<size_t>(arg_hr)) ||
        (!fp64 && arg_hr >= 44 && (arg_hr % 2) != 0) || !BlockLiveInContains(*cfg, 0, arg_hr)) {
      continue;
    }

    if (!gp64 && n == 1 && (arg_hr == 4 || arg_hr == 6) &&
        !consumed_arg_words.test(static_cast<size_t>(arg_hr + 1)) &&
        (!check_sched2_multi_cycle ||
         !HasPreTouchMultiCycleInEntryBlock(blk_parsed[0], blk_sums[0], *b0_pos, arg_hr, 2)) &&
        MatchesLocalAllocSingleBlockDiPairSuggCaseE(*cfg, blk_parsed[0], blk_sums[0], *b0_pos, gp64,
                                                    arg_hr)) {
      consumed_arg_words.set(static_cast<size_t>(arg_hr));
      consumed_arg_words.set(static_cast<size_t>(arg_hr + 1));
      b0_restored_args.emplace_back(arg_hr, RtlMachineMode::kDi);
      continue;
    }

    // Infer the scalar parameter mode of `arg_hr` from its entry-exposed reads.
    RtlMachineMode arg_mode = RtlMachineMode::kNone;
    bool mode_conflict = false;
    for (size_t b = 0; b < n && !mode_conflict; ++b) {
      if (!BlockLiveInContains(*cfg, b, arg_hr)) {
        continue;
      }
      for (size_t i = 0; i < blk_parsed[b].size(); ++i) {
        const bool writes_arg = blk_sums[b][i].written_regs.test(static_cast<size_t>(arg_hr));
        const bool reads_arg = blk_sums[b][i].read_regs.test(static_cast<size_t>(arg_hr));
        if (reads_arg) {
          blk_parsed[b][i].pattern.Visit([&](const RtlExpr& node) {
            if (node.IsHardReg() && node.RegNo() == arg_hr) {
              if (arg_mode == RtlMachineMode::kNone) {
                arg_mode = node.mode;
              } else if (arg_mode != node.mode) {
                mode_conflict = true;
              }
            }
          });
        }
        if (writes_arg && !reads_arg) {
          break;
        }
      }
    }
    if (mode_conflict || arg_mode == RtlMachineMode::kNone) {
      continue;
    }
    const int span = std::max(1, MipsHardRegNregs(arg_hr, arg_mode, gp64, fp64));
    if (span > 1 && (arg_hr % 2) != 0) {
      continue;
    }
    bool all_words_live_b0 = true;
    for (int w = 0; w < span; ++w) {
      if (!BlockLiveInContains(*cfg, 0, arg_hr + w)) {
        all_words_live_b0 = false;
      }
    }
    if (!all_words_live_b0 ||
        (!b0_needs_new_preheader && check_sched2_multi_cycle &&
         HasPreTouchMultiCycleInEntryBlock(blk_parsed[0], blk_sums[0], *b0_pos, arg_hr, span))) {
      continue;
    }

    bool written_in_b0 = false;
    if (!b0_needs_new_preheader) {
      for (const InsnSchedSummary& s : blk_sums[0]) {
        for (int w = 0; w < span; ++w) {
          if (s.written_regs.test(static_cast<size_t>(arg_hr + w))) {
            written_in_b0 = true;
            break;
          }
        }
        if (written_in_b0) {
          break;
        }
      }
    }
    if (written_in_b0) {
      if (n == 1 && MatchesLocalAllocEntryCopySuggCaseD(blk_parsed[0], blk_sums[0], *b0_pos, gp64,
                                                        fp64, arg_hr, span)) {
        for (int w = 0; w < span; ++w) {
          consumed_arg_words.set(static_cast<size_t>(arg_hr + w));
        }
        b0_restored_args.emplace_back(arg_hr, arg_mode);
      }
      continue;
    }

    std::vector<bool> live_web(n, false);
    std::vector<size_t> worklist = {0};
    live_web[0] = true;
    while (!worklist.empty()) {
      const size_t u = worklist.back();
      worklist.pop_back();
      bool live_at_end = true;
      for (size_t i = 0; i < blk_sums[u].size(); ++i) {
        bool reads_arg = false;
        bool writes_arg = false;
        for (int w = 0; w < span; ++w) {
          if (blk_sums[u][i].read_regs.test(static_cast<size_t>(arg_hr + w))) {
            reads_arg = true;
          }
          if (blk_sums[u][i].written_regs.test(static_cast<size_t>(arg_hr + w))) {
            writes_arg = true;
          }
        }
        if (!live_at_end && writes_arg) {
          break;
        }
        if (writes_arg && !reads_arg) {
          live_at_end = false;
          break;
        }
        if (HasRegDeadForRegno(blk_parsed[u][i], arg_hr) && !writes_arg) {
          live_at_end = false;
        }
      }
      if (!live_at_end) {
        continue;
      }
      for (size_t v : cfg->rpo_blocks[u].successors) {
        if (v < n && !live_web[v] && BlockLiveInContains(*cfg, v, arg_hr)) {
          live_web[v] = true;
          worklist.push_back(v);
        }
      }
    }

    bool live_in_other = false;
    bool crosses_call = false;
    bool written_in_web = false;
    for (size_t b = 0; b < n && !crosses_call; ++b) {
      if (!live_web[b]) {
        continue;
      }
      if (b > 0 || b0_needs_new_preheader) {
        live_in_other = true;
      }
      bool cur_live = true;
      for (size_t i = 0; i < blk_sums[b].size(); ++i) {
        if (!cur_live) {
          break;
        }
        if (blk_sums[b][i].is_call) {
          crosses_call = true;
          break;
        }
        bool reads_arg = false;
        bool writes_arg = false;
        for (int w = 0; w < span; ++w) {
          if (blk_sums[b][i].read_regs.test(static_cast<size_t>(arg_hr + w))) {
            reads_arg = true;
          }
          if (blk_sums[b][i].written_regs.test(static_cast<size_t>(arg_hr + w))) {
            writes_arg = true;
          }
        }
        if (writes_arg) {
          written_in_web = true;
          if (!reads_arg || HasRegDeadForRegno(blk_parsed[b][i], arg_hr)) {
            cur_live = false;
          }
        } else if (HasRegDeadForRegno(blk_parsed[b][i], arg_hr)) {
          cur_live = false;
        }
      }
    }
    if (crosses_call) {
      continue;
    }
    if (!live_in_other) {
      if (n == 1 && !b0_needs_new_preheader && span == 1 && arg_mode == RtlMachineMode::kSi &&
          MatchesLocalAllocEntryFirstInsnMovstrsiCaseF(*cfg, blk_parsed[0], blk_sums[0], *b0_pos,
                                                       arg_hr)) {
        consumed_arg_words.set(static_cast<size_t>(arg_hr));
        b0_restored_args.emplace_back(arg_hr, arg_mode);
      }
      continue;
    }

    for (int w = 0; w < span; ++w) {
      consumed_arg_words.set(static_cast<size_t>(arg_hr + w));
    }

    if (MatchesLocalAllocMultiBlockArgSuggCaseC(*cfg, live_web, blk_parsed, blk_sums, gp64, fp64,
                                                arg_hr, span)) {
      b0_restored_args.emplace_back(arg_hr, arg_mode);
      continue;
    }

    if ((n < 3 && !b0_needs_new_preheader) || loops.empty()) {
      continue;
    }

    if (!written_in_web) {
      if (MatchesUnmodifiedLoopArgPreferenceCaseA(*cfg, live_web, blk_parsed, blk_sums,
                                                  written_anywhere, loops, any_in_loop, arg_hr,
                                                  arg_mode, span)) {
        b0_restored_args.emplace_back(arg_hr, arg_mode);
      }
      continue;
    }

    const std::optional<size_t> pre_b = FindSequentialLoopPreheaderArgCopyCaseB(
        *cfg, live_web, preds, blk_parsed, blk_sums, written_anywhere, loops, any_in_loop,
        check_sched2_multi_cycle, arg_hr, arg_mode, span);
    if (pre_b.has_value()) {
      b0_restored_args.emplace_back(arg_hr, arg_mode);
      preheader_restored_args.emplace_back(*pre_b, arg_hr, arg_mode);
    }
  }

  if (b0_restored_args.empty()) {
    return;
  }

  auto make_noop_self_move = [](int hr, RtlMachineMode mode) {
    RtlPatternAndNotes pn;
    pn.pattern = RtlExpr::MakeSet(MakeGccHardReg(mode, hr), MakeGccHardReg(mode, hr));
    pn.reg_notes.push_back(RtlRegNote{
        .kind = RtlMachineMode::kRegDead,
        .list_op = RtlOp::kExprList,
        .datum = MakeGccHardReg(mode, hr),
    });
    return FormatInsnWithoutCallFusage(pn);
  };

  const bool skip_block_end = (stage == NoopSelfMoveStage::kSched2NonBlockEndOnly ||
                               stage == NoopSelfMoveStage::kSched2PreScheduleInsnsOnly);
  bool any_inserted = false;
  if (b0_needs_new_preheader) {
    std::vector<std::string> preheader_insns;
    for (size_t k = 0; k < b0_restored_args.size(); ++k) {
      const bool is_block_end = (k + 1 == b0_restored_args.size());
      if ((stage == NoopSelfMoveStage::kJump2BlockEndOnly && !is_block_end) ||
          (skip_block_end && is_block_end)) {
        continue;
      }
      preheader_insns.push_back(
          make_noop_self_move(b0_restored_args[k].first, b0_restored_args[k].second));
    }
    if (!preheader_insns.empty()) {
      for (RtlBasicBlock& blk : cfg->rpo_blocks) {
        blk.id += 1;
        for (size_t& s : blk.successors) {
          s += 1;
        }
        for (size_t& p : blk.predecessors) {
          p += 1;
        }
        for (size_t& t : blk.addr_vec_targets) {
          t += 1;
        }
      }
      cfg->rpo_blocks[0].has_label = true;
      cfg->rpo_blocks[0].predecessors.push_back(0);
      RtlBasicBlock entry_preheader;
      entry_preheader.id = 0;
      entry_preheader.has_label = false;
      entry_preheader.instructions = std::move(preheader_insns);
      entry_preheader.successors = {1};
      entry_preheader.SyncRtlInsnsFromInstructions();
      cfg->rpo_blocks.insert(cfg->rpo_blocks.begin(), std::move(entry_preheader));
      for (auto& [pb, hr, mode] : preheader_restored_args) {
        pb += 1;
      }
      SynchronizeCanonicalLabels(cfg);
      for (RtlBasicBlock& blk : cfg->rpo_blocks) {
        blk.SyncRtlInsnsFromInstructions();
      }
      any_inserted = true;
    }
  } else {
    RtlBasicBlock& b0 = cfg->rpo_blocks[0];
    bool b0_has_trailing_active = false;
    for (size_t idx = *b0_pos; idx < b0.instructions.size(); ++idx) {
      auto p = ParsePatternAndNotes(b0.instructions[idx]);
      if (p.ok() && p->pattern.op != RtlOp::kNote) {
        b0_has_trailing_active = true;
        break;
      }
    }
    bool b0_modified = false;
    for (size_t k = 0; k < b0_restored_args.size(); ++k) {
      const bool is_block_end = !b0_has_trailing_active && (k + 1 == b0_restored_args.size());
      if ((stage == NoopSelfMoveStage::kJump2BlockEndOnly && !is_block_end) ||
          (skip_block_end && is_block_end)) {
        continue;
      }
      const int target_order = LookupAbiArgCanonicalOrder(b0_restored_args[k].first);
      size_t ins_idx = *b0_pos;
      while (ins_idx < b0.instructions.size()) {
        auto p = ParsePatternAndNotes(b0.instructions[ins_idx]);
        if (!p.ok()) {
          break;
        }
        if (p->pattern.op == RtlOp::kNote) {
          ++ins_idx;
          continue;
        }
        int existing_hr = -1;
        RtlMachineMode existing_mode = RtlMachineMode::kNone;
        if (IsLeadingNoopSelfMoveForReg(*p, &existing_hr, &existing_mode) &&
            LookupAbiArgCanonicalOrder(existing_hr) < target_order) {
          ++ins_idx;
          continue;
        }
        break;
      }
      b0.instructions.insert(
          b0.instructions.begin() + static_cast<ptrdiff_t>(ins_idx),
          make_noop_self_move(b0_restored_args[k].first, b0_restored_args[k].second));
      b0_modified = true;
      any_inserted = true;
    }
    if (b0_modified) {
      b0.SyncRtlInsnsFromInstructions();
    }
  }

  for (size_t idx = 0; idx < preheader_restored_args.size(); ++idx) {
    const auto& [pb, hr, mode] = preheader_restored_args[idx];
    bool is_last_for_pb = true;
    for (size_t nxt = idx + 1; nxt < preheader_restored_args.size(); ++nxt) {
      if (std::get<0>(preheader_restored_args[nxt]) == pb) {
        is_last_for_pb = false;
        break;
      }
    }
    RtlBasicBlock& pblk = cfg->rpo_blocks[pb];
    size_t pos = 0;
    while (pos < pblk.instructions.size()) {
      auto p = ParsePatternAndNotes(pblk.instructions[pos]);
      if (!p.ok() || p->pattern.op != RtlOp::kNote) {
        break;
      }
      ++pos;
    }
    bool pb_has_trailing_active = false;
    for (size_t rem = pos; rem < pblk.instructions.size(); ++rem) {
      auto p = ParsePatternAndNotes(pblk.instructions[rem]);
      if (p.ok() && p->pattern.op != RtlOp::kNote) {
        pb_has_trailing_active = true;
        break;
      }
    }
    const bool is_block_end = !pb_has_trailing_active && is_last_for_pb;
    if ((stage == NoopSelfMoveStage::kJump2BlockEndOnly && !is_block_end) ||
        (skip_block_end && is_block_end)) {
      continue;
    }
    const int target_order = LookupAbiArgCanonicalOrder(hr);
    size_t ins_pos = pos;
    while (ins_pos < pblk.instructions.size()) {
      auto p = ParsePatternAndNotes(pblk.instructions[ins_pos]);
      if (!p.ok()) {
        break;
      }
      if (p->pattern.op == RtlOp::kNote) {
        ++ins_pos;
        continue;
      }
      int existing_hr = -1;
      RtlMachineMode existing_mode = RtlMachineMode::kNone;
      if (IsLeadingNoopSelfMoveForReg(*p, &existing_hr, &existing_mode) &&
          LookupAbiArgCanonicalOrder(existing_hr) < target_order) {
        ++ins_pos;
        continue;
      }
      break;
    }
    pblk.instructions.insert(pblk.instructions.begin() + static_cast<ptrdiff_t>(ins_pos),
                             make_noop_self_move(hr, mode));
    pblk.SyncRtlInsnsFromInstructions();
    any_inserted = true;
  }
  if (any_inserted) {
    RecomputePostReloadLiveAtStart(cfg, profile);
  }
}

// Reverses `sched.c:schedule_insns` (`reload_completed != 0`) deletion of
// `(set (reg:M arg_hr) (reg:M arg_hr))` right before a `CALL_INSN` in `b > 0`
// when `arg_hr` was computed into a multi-block pseudo (`reg_renumber = arg_hr`)
// in another block `b_def != b`, read by non-call instructions, and also read
// in `CALL_INSN_FUNCTION_USAGE` without being rewritten in `b` before the call.
void RestoreSched2PreScheduleCallArgNoopMoves(RtlControlFlowGraph* cfg,
                                              const CompilationProfile& profile, bool gp64,
                                              bool fp64) {
  const size_t n = cfg->rpo_blocks.size();
  if (n < 2) {
    return;
  }
  std::vector<std::vector<RtlPatternAndNotes>> blk_parsed(n);
  std::vector<std::vector<InsnSchedSummary>> blk_sums(n);
  for (size_t b = 0; b < n; ++b) {
    const auto& insns = cfg->rpo_blocks[b].instructions;
    blk_parsed[b].reserve(insns.size());
    blk_sums[b].reserve(insns.size());
    for (const std::string& pat_s : insns) {
      auto p = ParsePatternAndNotes(pat_s);
      if (!p.ok()) {
        return;
      }
      InsnSchedSummary s = SummarizeInsnForSched2(pat_s, gp64, fp64);
      blk_parsed[b].push_back(std::move(*p));
      blk_sums[b].push_back(s);
    }
  }

  std::vector<std::bitset<68>> entry_unmodified_in(n);
  for (int hr : {4, 5, 6, 7, 44, 45, 46, 47}) {
    if (BlockLiveInContains(*cfg, 0, hr)) {
      entry_unmodified_in[0].set(static_cast<size_t>(hr));
    }
  }
  std::vector<std::bitset<68>> def_reaches_in(n);
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < n; ++b) {
      std::bitset<68> cur_entry = entry_unmodified_in[b];
      std::bitset<68> cur_def = def_reaches_in[b];
      for (size_t i = 0; i < blk_parsed[b].size(); ++i) {
        const InsnSchedSummary& s = blk_sums[b][i];
        int self_hr = -1;
        RtlMachineMode self_mode = RtlMachineMode::kNone;
        if (!s.is_call && IsLeadingNoopSelfMoveForReg(blk_parsed[b][i], &self_hr, &self_mode)) {
          continue;
        }
        cur_entry &= ~s.written_regs;
        if (s.is_call) {
          cur_def.reset();
        } else {
          cur_def = (cur_def & ~s.written_regs) | s.written_regs;
        }
      }
      for (size_t succ : cfg->rpo_blocks[b].successors) {
        if (succ >= n) {
          continue;
        }
        for (int hr : {4, 5, 6, 7, 44, 45, 46, 47}) {
          if (!BlockLiveInContains(*cfg, succ, hr)) {
            continue;
          }
          const size_t bit = static_cast<size_t>(hr);
          if (cur_entry.test(bit) && !entry_unmodified_in[succ].test(bit)) {
            entry_unmodified_in[succ].set(bit);
            changed = true;
          }
          if (cur_def.test(bit) && !def_reaches_in[succ].test(bit)) {
            def_reaches_in[succ].set(bit);
            changed = true;
          }
        }
      }
    }
  }

  bool any_inserted = false;
  for (size_t b = 1; b < n; ++b) {
    std::bitset<68> written_since_call;
    std::bitset<68> read_before_call_in_b;
    std::vector<std::string> new_insns;
    std::vector<RtlInsn> new_rtl_insns;
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
    bool blk_changed = false;
    for (size_t i = 0; i < blk_parsed[b].size(); ++i) {
      const RtlPatternAndNotes& pn = blk_parsed[b][i];
      const InsnSchedSummary& s = blk_sums[b][i];
      if (s.is_call && pn.has_explicit_call_fusage) {
        std::vector<std::pair<int, RtlMachineMode>> call_self_moves;
        for (const RtlExpr& item : pn.call_fusage) {
          if (item.op != RtlOp::kUse || item.children.empty() || !item.children[0].IsHardReg()) {
            continue;
          }
          const int hr = item.children[0].RegNo();
          const RtlMachineMode mode = item.children[0].mode;
          const size_t bit = static_cast<size_t>(hr);
          if (!IsMipsO32CallArgReg(hr) || written_since_call.test(bit) ||
              !BlockLiveInContains(*cfg, b, hr) || !def_reaches_in[b].test(bit) ||
              entry_unmodified_in[b].test(bit) || !read_before_call_in_b.test(bit)) {
            continue;
          }
          call_self_moves.emplace_back(hr, mode);
        }
        std::sort(
            call_self_moves.begin(), call_self_moves.end(), [](const auto& a, const auto& b_pair) {
              return LookupAbiArgCanonicalOrder(a.first) < LookupAbiArgCanonicalOrder(b_pair.first);
            });
        for (const auto& [hr, mode] : call_self_moves) {
          RtlPatternAndNotes mv;
          mv.pattern = RtlExpr::MakeSet(MakeGccHardReg(mode, hr), MakeGccHardReg(mode, hr));
          mv.reg_notes.push_back(RtlRegNote{
              .kind = RtlMachineMode::kRegDead,
              .list_op = RtlOp::kExprList,
              .datum = MakeGccHardReg(mode, hr),
          });
          std::string mv_s = FormatInsnWithoutCallFusage(mv);
          RtlInsn rinsn;
          rinsn.canonical_pattern = mv_s;
          rinsn.is_synthetic_rtl_expansion = true;
          new_insns.push_back(std::move(mv_s));
          new_rtl_insns.push_back(std::move(rinsn));
          blk_changed = true;
          any_inserted = true;
        }
        written_since_call.reset();
        read_before_call_in_b.reset();
      } else {
        int self_hr = -1;
        RtlMachineMode self_mode = RtlMachineMode::kNone;
        if (!IsLeadingNoopSelfMoveForReg(pn, &self_hr, &self_mode)) {
          read_before_call_in_b |= (s.read_regs & ~written_since_call);
          written_since_call |= s.written_regs;
        }
      }
      new_insns.push_back(blk.instructions[i]);
      new_rtl_insns.push_back(blk.rtl_insns[i]);
    }
    if (blk_changed) {
      blk.instructions = std::move(new_insns);
      blk.rtl_insns = std::move(new_rtl_insns);
    }
  }
  if (any_inserted) {
    RecomputePostReloadLiveAtStart(cfg, profile);
  }
}

}  // namespace

void RestoreSched2DeletedNoopMoves(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (flags.opt_level <= 0 || !flags.RunsSched2()) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  RestoreDeletedNoopSharedReturnMove(cfg, profile);
  RestoreDeletedNoopArgPreferenceMoves(cfg, profile, flags, gp64, fp64,
                                       NoopSelfMoveStage::kSched2NonBlockEndOnly);
}

void RestoreSched2PreScheduleDeletedNoopMoves(RtlControlFlowGraph* cfg,
                                              const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (flags.opt_level <= 0 || !flags.RunsSched2()) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  RestoreDeletedNoopArgPreferenceMoves(cfg, profile, flags, gp64, fp64,
                                       NoopSelfMoveStage::kSched2PreScheduleInsnsOnly);
  RestoreSched2PreScheduleCallArgNoopMoves(cfg, profile, gp64, fp64);
}

void RestoreJump2DeletedNoopMoves(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (flags.opt_level <= 0) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  if (!flags.RunsSched2()) {
    RestoreDeletedNoopSharedReturnMove(cfg, profile);
    RestoreDeletedNoopArgPreferenceMoves(cfg, profile, flags, gp64, fp64, NoopSelfMoveStage::kAll);
  } else {
    RestoreDeletedNoopArgPreferenceMoves(cfg, profile, flags, gp64, fp64,
                                         NoopSelfMoveStage::kJump2BlockEndOnly);
  }
}

}  // namespace rom_nom_nom::lifter
