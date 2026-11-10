#include "lifter/invert_reload.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kStackPointerRegnum = 29;
constexpr int kFramePointerRegnum = 30;
constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;

// Recursively validates all `(reg ...)` nodes in `expr`.
absl::Status ValidateRtlExprRegisters(const RtlExpr& expr) {
  if (expr.op == RtlOp::kReg) {
    if (expr.int_val < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: invalid negative register number %d", expr.int_val));
    }
  }
  for (const RtlExpr& child : expr.children) {
    if (absl::Status st = ValidateRtlExprRegisters(child); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

bool IsStackPointerRegExpr(const RtlExpr& expr) {
  return expr.op == RtlOp::kReg && expr.mode == RtlMachineMode::kSi && !expr.flags.Any() &&
         expr.int_val == kStackPointerRegnum;
}

bool SingleElementModifiesStackPointer(const RtlExpr& expr) {
  if ((expr.op == RtlOp::kSet || expr.op == RtlOp::kClobber) && !expr.children.empty()) {
    const RtlExpr* dst = &expr.children[0];
    while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
      dst = &dst->children[0];
    }
    if (dst->op == RtlOp::kReg && dst->int_val == kStackPointerRegnum) {
      return true;
    }
  }
  return false;
}

bool ShouldSkipFramePointerRestorationForInsn(const RtlExpr& pattern) {
  if ((pattern.op == RtlOp::kUse || pattern.op == RtlOp::kClobber) && !pattern.children.empty() &&
      pattern.children[0].op != RtlOp::kMem) {
    return true;
  }
  if (SingleElementModifiesStackPointer(pattern)) {
    return true;
  }
  if (pattern.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pattern.children) {
      if (SingleElementModifiesStackPointer(elem)) {
        return true;
      }
    }
  }
  return false;
}

// Rewrites `(reg:SI 29 sp)` back to `(reg:SI 30 fp)` inside `expr`, reversing
// `reload1.c:eliminate_regs` when `!frame_pointer_needed`.
bool ReverseEliminateRegsInExpr(RtlExpr* expr) {
  if (expr == nullptr) {
    return false;
  }
  if (IsStackPointerRegExpr(*expr)) {
    expr->SetRegNo(kFramePointerRegnum);
    return true;
  }

  bool changed = false;
  if (expr->op == RtlOp::kSet && expr->children.size() >= 2) {
    // Never rewrite a bare `(reg:SI 29 sp)` as the direct `SET_DEST` of a `SET`
    // because `eliminate_regs_in_insn` deletes any single-set whose `SET_DEST`
    // is an eliminable register (`fp`).
    if (!IsStackPointerRegExpr(expr->children[0])) {
      if (ReverseEliminateRegsInExpr(&expr->children[0])) {
        changed = true;
      }
    }
    for (size_t i = 1; i < expr->children.size(); ++i) {
      if (ReverseEliminateRegsInExpr(&expr->children[i])) {
        changed = true;
      }
    }
    return changed;
  }

  for (RtlExpr& child : expr->children) {
    if (ReverseEliminateRegsInExpr(&child)) {
      changed = true;
    }
  }
  return changed;
}

// Checks whether `insn` is a bare hard-register to hard-register move
// `(set (reg:M dst) (reg:M src))` with no `REG_NOTES`, returning `{dst, src}`.
std::optional<std::pair<int, int>> MatchBareHardRegMove(const RtlPatternAndNotes& insn) {
  if (!insn.reg_notes.empty()) {
    return std::nullopt;
  }
  const RtlExpr& pat = insn.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return std::nullopt;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || src.op != RtlOp::kReg || dst.mode != src.mode) {
    return std::nullopt;
  }
  const int dst_reg = dst.RegNo();
  const int src_reg = src.RegNo();
  if (dst_reg < 0 || dst_reg >= kFirstPseudoRegister || src_reg < 0 ||
      src_reg >= kFirstPseudoRegister) {
    return std::nullopt;
  }
  return std::make_pair(dst_reg, src_reg);
}

// Reverses `reload1.c:emit_reload_insns` / `subst_reloads` for earlyclobber
// `(clobber (reg:M ...))` operands in a `PARALLEL` instruction where `reload`
// substituted a reload register `r_reload` into `(clobber (reg:M r_reload))`
// and emitted `(set (reg:M r_orig) (reg:M r_reload))` immediately after the
// `PARALLEL`.
void ReverseParallelClobberOutputReloads(std::vector<RtlPatternAndNotes>* insns,
                                         absl::flat_hash_set<int>* reversed_reload_regs) {
  if (insns == nullptr || insns->empty()) {
    return;
  }
  std::vector<RtlPatternAndNotes> out;
  out.reserve(insns->size());

  for (size_t i = 0; i < insns->size(); ++i) {
    RtlPatternAndNotes current = (*insns)[i];
    if (current.pattern.op == RtlOp::kParallel) {
      absl::flat_hash_set<int> set_dest_regs;
      absl::flat_hash_set<int> set_referenced_regs;
      absl::flat_hash_map<int, size_t> clobber_reg_to_child_idx;

      for (size_t c = 0; c < current.pattern.children.size(); ++c) {
        const RtlExpr& elem = current.pattern.children[c];
        if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
          if (elem.children[0].op == RtlOp::kReg) {
            set_dest_regs.insert(elem.children[0].RegNo());
          }
          elem.Visit([&](const RtlExpr& node) {
            if (node.op == RtlOp::kReg) {
              set_referenced_regs.insert(node.RegNo());
            }
          });
        } else if (elem.op == RtlOp::kClobber && !elem.children.empty() &&
                   elem.children[0].op == RtlOp::kReg) {
          const int clob_reg = elem.children[0].RegNo();
          if (clob_reg >= 0 && clob_reg < kFirstPseudoRegister) {
            clobber_reg_to_child_idx[clob_reg] = c;
          }
        }
      }

      if (!clobber_reg_to_child_idx.empty()) {
        size_t lookahead = i + 1;
        while (lookahead < insns->size()) {
          std::optional<std::pair<int, int>> move = MatchBareHardRegMove((*insns)[lookahead]);
          if (!move.has_value()) {
            break;
          }
          const auto [orig_reg, reload_reg] = *move;
          auto clob_it = clobber_reg_to_child_idx.find(reload_reg);
          if (clob_it == clobber_reg_to_child_idx.end() || set_dest_regs.contains(reload_reg) ||
              !set_referenced_regs.contains(orig_reg)) {
            break;
          }
          const size_t child_idx = clob_it->second;
          current.pattern.children[child_idx].children[0].SetRegNo(orig_reg);
          clobber_reg_to_child_idx.erase(clob_it);
          if (reversed_reload_regs != nullptr) {
            reversed_reload_regs->insert(reload_reg);
          }
          ++lookahead;
        }
        i = lookahead - 1;
      }
    }
    out.push_back(std::move(current));
  }

  *insns = std::move(out);
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertReload(const RtlControlFlowGraph& reload_cfg) {
  if (reload_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertReload: input CFG has no basic blocks");
  }

  const size_t num_blocks = reload_cfg.rpo_blocks.size();
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = reload_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: block %zu has out-of-bounds successor %zu (num_blocks=%zu)", b, succ,
            num_blocks));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: block %zu has out-of-bounds addr_vec target %zu (num_blocks=%zu)", b,
            target, num_blocks));
      }
    }
  }

  for (const auto& [regno, hard_reg] : reload_cfg.sideband.reg_renumber) {
    if (regno < kFirstPseudoRegister || hard_reg < -1 || hard_reg >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertReload: invalid sideband reg_renumber entry (%d -> %d)", regno, hard_reg));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_refs) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_refs", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_sets) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_sets", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_deaths) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_deaths", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_live_length) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_live_length", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_calls_crossed) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertReload: negative regno %d in sideband reg_n_calls_crossed", regno));
    }
  }

  RtlControlFlowGraph out = reload_cfg;
  out.pass = RtlPassKind::kLreg;

  bool restored_any_fp = false;
  absl::flat_hash_set<int> reversed_reload_regs;
  absl::flat_hash_set<int> remaining_hard_regs;

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    std::vector<RtlPatternAndNotes> parsed_insns;
    parsed_insns.reserve(blk.instructions.size());

    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("InvertReload: failed to parse instruction %zu in block %zu: %s", i, b,
                            parsed.status().message()));
      }
      if (parsed->pattern.op == RtlOp::kNil || parsed->pattern.op == RtlOp::kUnknown) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: invalid pattern opcode at instruction %zu in block %zu", i, b));
      }
      if (absl::Status st = ValidateRtlExprRegisters(parsed->pattern); !st.ok()) {
        return st;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (absl::Status st = ValidateRtlExprRegisters(note.datum); !st.ok()) {
          return st;
        }
      }

      if (!out.sideband.frame_pointer_needed &&
          !ShouldSkipFramePointerRestorationForInsn(parsed->pattern)) {
        if (ReverseEliminateRegsInExpr(&parsed->pattern)) {
          restored_any_fp = true;
          // In `reload1.c:reload_as_needed` (lines 3836-3847), `(use (mem ...))`
          // and `(clobber (mem ...))` have `XEXP(MEM, 0)` eliminated in-place
          // before `eliminate_regs_in_insn(insn, 1)` is called, so `val == 0`
          // and `REG_NOTES(insn)` are not rewritten by `eliminate_regs_in_insn`.
          if (parsed->pattern.op != RtlOp::kUse && parsed->pattern.op != RtlOp::kClobber) {
            for (RtlRegNote& note : parsed->reg_notes) {
              ReverseEliminateRegsInExpr(&note.datum);
            }
          }
        }
      }
      parsed_insns.push_back(*std::move(parsed));
    }

    ReverseParallelClobberOutputReloads(&parsed_insns, &reversed_reload_regs);

    blk.instructions.clear();
    blk.instructions.reserve(parsed_insns.size());
    for (const RtlPatternAndNotes& insn : parsed_insns) {
      insn.pattern.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kFirstPseudoRegister) {
          remaining_hard_regs.insert(node.RegNo());
        }
      });
      blk.instructions.push_back(insn.ToSexprString());
    }
    blk.SyncRtlInsnsFromInstructions();
  }

  if (!reversed_reload_regs.empty()) {
    for (const auto& [_, hard_reg] : out.sideband.reg_renumber) {
      if (hard_reg >= 0 && hard_reg < kFirstPseudoRegister) {
        remaining_hard_regs.insert(hard_reg);
      }
    }
    auto& saved_gprs = out.sideband.saved_gpr_regnos;
    saved_gprs.erase(std::remove_if(saved_gprs.begin(), saved_gprs.end(),
                                    [&](int gpr) {
                                      return reversed_reload_regs.contains(gpr) &&
                                             !remaining_hard_regs.contains(gpr);
                                    }),
                     saved_gprs.end());
  }

  if (restored_any_fp && !out.sideband.basic_block_live_at_start.empty()) {
    out.sideband.basic_block_live_at_start[0].insert(kFramePointerRegnum);
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
