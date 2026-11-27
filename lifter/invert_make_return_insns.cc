#include "lifter/invert_make_return_insns.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/reorg_model.h"
#include "lifter/rtl_cfg_utils.h"

namespace rom_nom_nom::lifter {
namespace {

bool IsSingleReturnInternalOrSequenceBlock(const RtlBasicBlock& blk) {
  if (blk.instructions.size() != 1) {
    return false;
  }
  auto parsed = ParsePatternAndNotes(blk.instructions[0]);
  if (!parsed.ok()) {
    return false;
  }
  const RtlExpr& expr = parsed->pattern;
  if (IsReturnExpr(expr)) {
    return true;
  }
  return expr.op == RtlOp::kSequence && !expr.children.empty() &&
         expr.children[0].op == RtlOp::kJumpInsn && !expr.children[0].children.empty() &&
         IsReturnExpr(expr.children[0].children[0]);
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
  if (!IsValidCfg(dbr_return_cfg_x, CfgEmptyBlockPolicy::kAllowAnyEmptyBlock)) {
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

  bool unpacked_return_eager_comp = false;
  if (out.rpo_blocks.size() >= 2) {
    RtlBasicBlock& last_blk = out.rpo_blocks.back();
    const RtlBasicBlock& prev_blk = out.rpo_blocks[out.rpo_blocks.size() - 2];
    const size_t last_id = out.rpo_blocks.size() - 1;
    const bool prev_falls_to_last =
        std::find(prev_blk.successors.begin(), prev_blk.successors.end(), last_id) !=
        prev_blk.successors.end();
    if (!last_blk.has_label && last_blk.successors.empty() && !last_blk.instructions.empty() &&
        !prev_blk.instructions.empty() && prev_falls_to_last) {
      std::optional<size_t> seq_idx;
      bool others_all_use_or_clobber = true;
      for (size_t i = 0; i < last_blk.instructions.size(); ++i) {
        auto pp = ParsePatternAndNotes(last_blk.instructions[i]);
        if (!pp.ok()) {
          others_all_use_or_clobber = false;
          break;
        }
        if (pp->pattern.op == RtlOp::kSequence) {
          if (seq_idx.has_value()) {
            others_all_use_or_clobber = false;
            break;
          }
          seq_idx = i;
        } else if (pp->pattern.op != RtlOp::kUse && pp->pattern.op != RtlOp::kClobber) {
          others_all_use_or_clobber = false;
          break;
        }
      }
      auto prev_seq = ParsePatternAndNotes(prev_blk.instructions.back());
      if (others_all_use_or_clobber && seq_idx.has_value() && prev_seq.ok()) {
        auto ret_seq = ParsePatternAndNotes(last_blk.instructions[*seq_idx]);
        if (ret_seq.ok() && ret_seq->pattern.op == RtlOp::kSequence &&
            ret_seq->pattern.children.size() == 2 && prev_seq->pattern.op == RtlOp::kSequence &&
            prev_seq->pattern.children.size() == 2 &&
            !prev_seq->pattern.children[0].flags.unchanging) {
          auto ret_pn = RtlPatternAndNotes::FromSequenceSubInsn(ret_seq->pattern.children[0]);
          auto slot_pn = RtlPatternAndNotes::FromSequenceSubInsn(ret_seq->pattern.children[1]);
          auto jump_pn = RtlPatternAndNotes::FromSequenceSubInsn(prev_seq->pattern.children[0]);
          auto eager_pn = RtlPatternAndNotes::FromSequenceSubInsn(prev_seq->pattern.children[1]);
          if (ret_pn.ok() && slot_pn.ok() && jump_pn.ok() && eager_pn.ok()) {
            const bool is_ret_pat = ret_pn->pattern.op == RtlOp::kParallel &&
                                    ret_pn->pattern.children.size() == 2 &&
                                    ret_pn->pattern.children[0].op == RtlOp::kReturn &&
                                    ret_pn->pattern.children[1].op == RtlOp::kUse;
            const bool is_cond_jump = jump_pn->pattern.IsSet() &&
                                      jump_pn->pattern.SetDest().op == RtlOp::kPc &&
                                      jump_pn->pattern.SetSrc().op == RtlOp::kIfThenElse;
            if (is_ret_pat && is_cond_jump && eager_pn->pattern.IsSet() &&
                slot_pn->pattern.IsSet() && eager_pn->pattern.SetDest().IsReg() &&
                slot_pn->pattern.SetDest().IsReg() &&
                eager_pn->pattern.SetDest().RegNo() == slot_pn->pattern.SetDest().RegNo() &&
                eager_pn->pattern.SetDest().mode == slot_pn->pattern.SetDest().mode) {
              const RtlExpr& esrc = eager_pn->pattern.SetSrc();
              const RtlExpr& ssrc = slot_pn->pattern.SetSrc();
              bool is_opposite_arith = false;
              if ((esrc.op == RtlOp::kPlus || esrc.op == RtlOp::kMinus) &&
                  esrc.children.size() == 2 && ssrc.children.size() == 2 &&
                  esrc.children[0].IsReg() && ssrc.children[0].IsReg() &&
                  esrc.children[0].RegNo() == eager_pn->pattern.SetDest().RegNo() &&
                  ssrc.children[0].RegNo() == slot_pn->pattern.SetDest().RegNo()) {
                if (esrc.children[1].IsConstInt() && ssrc.children[1].IsConstInt()) {
                  is_opposite_arith = (esrc.op == ssrc.op &&
                                       esrc.children[1].int_val == -ssrc.children[1].int_val) ||
                                      (esrc.op != ssrc.op &&
                                       (ssrc.op == RtlOp::kPlus || ssrc.op == RtlOp::kMinus) &&
                                       esrc.children[1].int_val == ssrc.children[1].int_val);
                } else if ((ssrc.op == RtlOp::kPlus || ssrc.op == RtlOp::kMinus) &&
                           esrc.op != ssrc.op) {
                  RtlExpr erhs = esrc.children[1];
                  RtlExpr srhs = ssrc.children[1];
                  auto strip_flags = [](RtlExpr& n) { n.flags = RtlFlags{}; };
                  erhs.Mutate(strip_flags);
                  srhs.Mutate(strip_flags);
                  is_opposite_arith = (erhs == srhs);
                }
              }
              if (is_opposite_arith) {
                last_blk.instructions.erase(last_blk.instructions.begin() +
                                            static_cast<ptrdiff_t>(*seq_idx));
                last_blk.instructions.insert(last_blk.instructions.begin(),
                                             slot_pn->ToGccPrintRtlString());
                const size_t end_block_id = out.rpo_blocks.size();
                last_blk.successors = {end_block_id};
                last_blk.SyncRtlInsnsFromInstructions();
                RtlBasicBlock end_blk;
                end_blk.id = static_cast<uint32_t>(end_block_id);
                end_blk.has_label = true;
                end_blk.AddPatternOnly(ret_pn->ToGccPrintRtlString());
                out.rpo_blocks.push_back(std::move(end_blk));
                unpacked_return_eager_comp = true;
              }
            }
          }
        }
      }
    }
  }

  // If the final basic block carries an unreferenced label (`LABEL_NUSES == 0`)
  // and would otherwise be mistaken by `RunMakeReturnInsns` for an unused
  // `end_of_function_label`, append a trailing empty `end_of_function_label`
  // block so `RunMakeReturnInsns` deletes the appended label (`reorg.c:4349-4350`)
  // and preserves `out.rpo_blocks.back().has_label`.
  const auto& tail_blk = out.rpo_blocks.back();
  if (!unpacked_return_eager_comp && tail_blk.has_label &&
      !TrailingBlockHasExplicitLabelReference(out) &&
      (tail_blk.instructions.empty() ||
       BlockHasOnlyUseOrClobberInsns(tail_blk, /*skip_notes=*/false) ||
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
