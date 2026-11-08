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
bool TryRewriteSequenceReturnToEndJump(std::string* pat, std::string_view end_label) {
  auto parsed = ParsePatternAndNotes(*pat);
  if (!parsed.ok()) {
    return false;
  }
  RtlExpr& seq = parsed->pattern;
  if (seq.op != RtlOp::kSequence || !seq.bracketed_children || seq.children.size() < 2) {
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
  // block, and determine whether any existing block explicitly jumps or
  // branches to it (as opposed to merely falling through from `last_idx - 1`).
  const size_t num_blocks = out.rpo_blocks.size();
  bool reuse_trailing_empty_block = false;
  std::string existing_end_label;
  if (out.rpo_blocks.back().instructions.empty()) {
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

  bool converted_any = false;
  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    auto& blk = out.rpo_blocks[b];
    blk.id = b;
    bool modified_blk = false;
    for (std::string& pat : blk.instructions) {
      if (TryRewriteSequenceReturnToEndJump(&pat, end_label)) {
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
      RtlBasicBlock end_blk;
      end_blk.id = end_block_id;
      out.rpo_blocks.push_back(std::move(end_blk));
    }
  } else if (!reuse_trailing_empty_block &&
             BlockContainsOnlyUseOrClobberInsns(out.rpo_blocks.back())) {
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

  return out;
}

}  // namespace rom_nom_nom::lifter
