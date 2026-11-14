#include "lifter/invert_make_return_insns.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

// Validates that an `RtlExpr` subtree contains no unknown opcodes, no bare
// `(return)` outside a MIPS `return_internal` `(parallel [(return) ...])`, and
// that any `(sequence [...])` node is a bracketed sequence of at least 2 valid
// inner insn wrappers.
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

// Returns true if every instruction in `blk` is a `(use ...)` or `(clobber ...)`
// pseudo-instruction that `reorg.c` skips backwards over when inspecting a
// trailing `end_of_function_label`.
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

bool IsReturnInternalParallelExpr(const RtlExpr& expr) {
  return expr.op == RtlOp::kParallel && !expr.children.empty() &&
         expr.children[0].op == RtlOp::kReturn;
}

bool IsSingleReturnInternalOrSequenceBlock(const RtlBasicBlock& blk) {
  if (blk.instructions.size() != 1) {
    return false;
  }
  auto parsed = ParsePatternAndNotes(blk.instructions[0]);
  if (!parsed.ok()) {
    return false;
  }
  const RtlExpr& expr = parsed->pattern;
  if (IsReturnInternalParallelExpr(expr)) {
    return true;
  }
  return expr.op == RtlOp::kSequence && !expr.children.empty() &&
         expr.children[0].op == RtlOp::kJumpInsn && !expr.children[0].children.empty() &&
         IsReturnInternalParallelExpr(expr.children[0].children[0]);
}

bool IsAnyJumpOrReturnTopExpr(const RtlExpr& expr) {
  if (expr.op == RtlOp::kSequence && !expr.children.empty()) {
    return expr.children[0].op == RtlOp::kJumpInsn;
  }
  if (expr.op == RtlOp::kParallel && !expr.children.empty()) {
    const RtlExpr& first = expr.children[0];
    if (first.op == RtlOp::kReturn || (first.IsSet() && first.SetDest().op == RtlOp::kPc)) {
      return true;
    }
  }
  return expr.IsSet() && expr.SetDest().op == RtlOp::kPc;
}

bool TrailingBlockHasExplicitLabelReference(const RtlControlFlowGraph& cfg) {
  const size_t num_blocks = cfg.rpo_blocks.size();
  const size_t last_idx = num_blocks - 1;
  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg.rpo_blocks[b];
    for (size_t target : blk.addr_vec_targets) {
      if (target == last_idx) {
        return true;
      }
    }
    if (b + 1 != last_idx) {
      for (size_t succ : blk.successors) {
        if (succ == last_idx) {
          return true;
        }
      }
      continue;
    }
    if (blk.instructions.empty() || blk.successors.empty()) {
      continue;
    }
    auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
    if (!last_parsed.ok() || !IsAnyJumpOrReturnTopExpr(last_parsed->pattern)) {
      continue;
    }
    if (blk.successors.size() == 1 && blk.successors[0] == last_idx) {
      return true;
    }
    if (blk.successors.size() >= 2) {
      bool all_last = true;
      for (size_t succ : blk.successors) {
        if (succ != last_idx) {
          all_last = false;
          break;
        }
      }
      if (all_last) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& dbr_return_cfg_x,
                                          const CompilationProfile& profile) {
  if (!IsValidCfg(dbr_return_cfg_x)) {
    return {};
  }

  RtlControlFlowGraph out = dbr_return_cfg_x;
  out.pass = RtlPassKind::kDbr;
  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    out.rpo_blocks[b].id = static_cast<uint32_t>(b);
    if (out.rpo_blocks[b].rtl_insns.size() != out.rpo_blocks[b].instructions.size()) {
      out.rpo_blocks[b].SyncRtlInsnsFromInstructions();
    }
  }

  // If the final basic block carries an unreferenced label (`LABEL_NUSES == 0`)
  // and would otherwise be mistaken by `RunMakeReturnInsns` for an unused
  // `end_of_function_label`, append a trailing empty `end_of_function_label`
  // block so `RunMakeReturnInsns` deletes the appended label (`reorg.c:4349-4350`)
  // and preserves `out.rpo_blocks.back().has_label`.
  const auto& tail_blk = out.rpo_blocks.back();
  if (tail_blk.has_label && !TrailingBlockHasExplicitLabelReference(out) &&
      (tail_blk.instructions.empty() || BlockContainsOnlyUseOrClobberInsns(tail_blk) ||
       IsSingleReturnInternalOrSequenceBlock(tail_blk))) {
    const size_t end_block_id = out.rpo_blocks.size();
    if (!tail_blk.instructions.empty() && tail_blk.successors.empty()) {
      auto last_parsed = ParsePatternAndNotes(tail_blk.instructions.back());
      if (last_parsed.ok() && !IsAnyJumpOrReturnTopExpr(last_parsed->pattern)) {
        out.rpo_blocks.back().successors.push_back(end_block_id);
      }
    }
    RtlBasicBlock end_blk;
    end_blk.id = static_cast<uint32_t>(end_block_id);
    end_blk.has_label = true;
    out.rpo_blocks.push_back(std::move(end_blk));
  }

  RecomputePostReloadLiveAtStart(&out, profile);
  return out;
}

RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& dbr_return_cfg_x) {
  const CompilationProfile default_profile{.optimize_level = 2};
  return InvertMakeReturnInsns(dbr_return_cfg_x, default_profile);
}

}  // namespace rom_nom_nom::lifter
