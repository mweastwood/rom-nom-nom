#include "lifter/invert_fill_eager_delay_slots.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/reorg_model.h"
#include "lifter/rtl_cfg_utils.h"
#include "lifter/sched_model.h"
#include "lifter/sched_post_reload_projection.h"

namespace rom_nom_nom::lifter {
namespace {

struct EagerStolenSlotSite {
  size_t block_idx = 0;
  size_t slot_idx = 0;
  std::optional<size_t> opposite_block_idx;
  bool thread_if_true = true;
  bool is_annulled = false;
  bool valid = true;
  std::string slot_text;
};

void ShiftEagerSitesOnInsert(std::vector<EagerStolenSlotSite>* sites, size_t insert_idx) {
  if (sites == nullptr) {
    return;
  }
  for (EagerStolenSlotSite& s : *sites) {
    if (!s.valid) {
      continue;
    }
    if (s.block_idx >= insert_idx) {
      ++s.block_idx;
    }
    if (s.opposite_block_idx.has_value() && *s.opposite_block_idx >= insert_idx) {
      ++(*s.opposite_block_idx);
    }
  }
}

void ShiftEagerSitesOnErase(std::vector<EagerStolenSlotSite>* sites, size_t erase_idx) {
  if (sites == nullptr) {
    return;
  }
  for (EagerStolenSlotSite& s : *sites) {
    if (!s.valid) {
      continue;
    }
    if (s.block_idx == erase_idx) {
      s.valid = false;
      continue;
    }
    if (s.block_idx > erase_idx) {
      --s.block_idx;
    }
    if (s.opposite_block_idx.has_value()) {
      if (*s.opposite_block_idx == erase_idx) {
        s.opposite_block_idx.reset();
        s.valid = false;
      } else if (*s.opposite_block_idx > erase_idx) {
        --(*s.opposite_block_idx);
      }
    }
  }
}

void InsertBlockAt(RtlControlFlowGraph* cfg, size_t insert_idx, RtlBasicBlock new_blk,
                   std::vector<EagerStolenSlotSite>* sites) {
  ::rom_nom_nom::lifter::InsertBlockAt(cfg, insert_idx, std::move(new_blk));
  ShiftEagerSitesOnInsert(sites, insert_idx);
}

void EraseBlockAt(RtlControlFlowGraph* cfg, size_t erase_idx,
                  std::vector<EagerStolenSlotSite>* sites = nullptr) {
  cfg->rpo_blocks.erase(cfg->rpo_blocks.begin() + static_cast<ptrdiff_t>(erase_idx));
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    for (size_t& succ : cfg->rpo_blocks[b].successors) {
      if (succ > erase_idx) {
        --succ;
      }
    }
    for (size_t& t : cfg->rpo_blocks[b].addr_vec_targets) {
      if (t > erase_idx) {
        --t;
      }
    }
  }
  ShiftEagerSitesOnErase(sites, erase_idx);
}

bool IsRealActiveInsn(const RtlExpr& pat) {
  return pat.op != RtlOp::kNote && pat.op != RtlOp::kUse && pat.op != RtlOp::kClobber;
}

bool BlockFallsThroughToNext(const RtlControlFlowGraph& cfg,
                             const std::vector<std::vector<size_t>>& preds, size_t b) {
  if (b + 1 >= cfg.rpo_blocks.size()) {
    return false;
  }
  const auto& blk = cfg.rpo_blocks[b];
  if (std::find(blk.successors.begin(), blk.successors.end(), b + 1) == blk.successors.end()) {
    return false;
  }
  if (blk.instructions.empty()) {
    return true;
  }
  bool has_real_active = false;
  for (const std::string& insn_str : blk.instructions) {
    auto p = ParsePatternAndNotes(insn_str);
    if (p.ok() && IsRealActiveInsn(p->pattern)) {
      has_real_active = true;
      break;
    }
  }
  if (!has_real_active && !blk.has_label && b < preds.size() && preds[b].empty()) {
    return false;
  }
  auto parsed = ParsePatternAndNotes(blk.instructions.back());
  if (!parsed.ok()) {
    return false;
  }
  return !IsUncondJumpOrReturnOrTableJumpTopExpr(parsed->pattern);
}

bool IsAnyRegLiveAtBlockEntry(const RtlControlFlowGraph& out, absl::Span<const int> regs,
                              size_t start_b) {
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(out, /*profile=*/nullptr, &gp64, &fp64);

  auto is_reg_used_at_fn_exit = [&](int r, bool include_epilogue_delay) -> bool {
    if (include_epilogue_delay && out.sideband.epilogue_delay_pattern.has_value()) {
      auto ep = ParsePatternAndNotes(*out.sideband.epilogue_delay_pattern);
      if (ep.ok()) {
        if (ExprReferencesRegInSrcOrMem(ep->pattern, r)) {
          return true;
        }
        std::vector<int> ep_defs;
        CollectDefinedRegs(ep->pattern, &ep_defs);
        if (std::find(ep_defs.begin(), ep_defs.end(), r) != ep_defs.end()) {
          return false;
        }
      }
    }
    if (r == 29 || (r == 30 && out.sideband.frame_pointer_needed)) {
      return true;
    }
    if (r == 31) {
      return false;
    }
    bool integrated_ret_match = false;
    auto check_integrated_ret = [&](const RtlExpr& expr) {
      expr.Visit([&](const RtlExpr& node) {
        if (!node.IsReg() || !node.flags.integrated) {
          return;
        }
        const int ireg = node.RegNo();
        if (ireg != 2 && ireg != 3 && ireg != 32 && ireg != 33) {
          return;
        }
        const bool is_fp = (ireg >= 32);
        const bool wide_unit = is_fp ? fp64 : gp64;
        const int nregs =
            (!wide_unit && (node.mode == RtlMachineMode::kDi || node.mode == RtlMachineMode::kDf))
                ? 2
                : 1;
        if (r >= ireg && r < ireg + nregs) {
          integrated_ret_match = true;
        }
      });
    };
    if (out.sideband.epilogue_delay_pattern.has_value()) {
      auto ep = ParsePatternAndNotes(*out.sideband.epilogue_delay_pattern);
      if (ep.ok()) {
        check_integrated_ret(ep->pattern);
      }
    }
    for (const auto& blk : out.rpo_blocks) {
      for (const std::string& s : blk.instructions) {
        auto up = ParsePatternAndNotes(s);
        if (!up.ok()) {
          continue;
        }
        check_integrated_ret(up->pattern);
        if (integrated_ret_match) {
          return true;
        }
        if (up->pattern.op != RtlOp::kUse || up->pattern.children.empty() ||
            !up->pattern.children[0].IsReg()) {
          continue;
        }
        const int u_reg = up->pattern.children[0].RegNo();
        if (u_reg == 31) {
          continue;
        }
        const bool is_fp = (u_reg >= 32 && u_reg < 64);
        const bool wide_unit = is_fp ? fp64 : gp64;
        const RtlMachineMode m = up->pattern.children[0].mode;
        const int nregs =
            (!wide_unit && (m == RtlMachineMode::kDi || m == RtlMachineMode::kDf)) ? 2 : 1;
        if (r >= u_reg && r < u_reg + nregs) {
          return true;
        }
      }
    }
    return integrated_ret_match;
  };

  auto is_call_clobbered_reg = [](int r) -> bool {
    return r == 1 || (r >= 2 && r <= 15) || r == 24 || r == 25 || r == 31 || (r >= 32 && r <= 51);
  };
  auto is_call_arg_reg = [](int r) -> bool { return (r >= 4 && r <= 7) || (r >= 44 && r <= 47); };

  const size_t num_blocks = out.rpo_blocks.size();
  for (int r : regs) {
    if (start_b >= num_blocks) {
      continue;
    }
    std::vector<bool> visited(num_blocks, false);
    std::vector<size_t> worklist;
    visited[start_b] = true;
    worklist.push_back(start_b);
    for (size_t wl_idx = 0; wl_idx < worklist.size(); ++wl_idx) {
      const size_t cur_b = worklist[wl_idx];
      const auto& scan_blk = out.rpo_blocks[cur_b];
      bool killed = false;
      bool handled_succs = false;
      for (size_t idx = 0; idx < scan_blk.instructions.size(); ++idx) {
        const std::string& insn_s = scan_blk.instructions[idx];
        auto p = ParsePatternAndNotes(insn_s);
        if (!p.ok() || IsLoopHeaderNote(p->pattern) || p->pattern.op == RtlOp::kClobber) {
          continue;
        }
        if (p->pattern.op == RtlOp::kUse) {
          if (r != 31 && ExprReferencesRegInSrcOrMem(p->pattern, r)) {
            bool killed_before_exit = false;
            for (size_t j = idx + 1; j < scan_blk.instructions.size(); ++j) {
              auto q = ParsePatternAndNotes(scan_blk.instructions[j]);
              if (!q.ok() || IsLoopHeaderNote(q->pattern) || q->pattern.op == RtlOp::kClobber ||
                  q->pattern.op == RtlOp::kUse) {
                continue;
              }
              if (q->pattern.op == RtlOp::kSequence && q->pattern.children.size() == 2 &&
                  !q->pattern.children[0].flags.unchanging) {
                auto q0 = RtlPatternAndNotes::FromSequenceSubInsn(q->pattern.children[0]);
                auto q1 = RtlPatternAndNotes::FromSequenceSubInsn(q->pattern.children[1]);
                if (q0.ok() && q1.ok() && !ExprReferencesRegInSrcOrMem(q0->pattern, r) &&
                    !ExprReferencesRegInSrcOrMem(q1->pattern, r)) {
                  std::vector<int> q1_defs;
                  CollectDefinedRegs(q1->pattern, &q1_defs);
                  if (std::find(q1_defs.begin(), q1_defs.end(), r) != q1_defs.end()) {
                    killed_before_exit = true;
                  }
                }
              }
              break;
            }
            if (killed_before_exit) {
              killed = true;
              break;
            }
            return true;
          }
          continue;
        }
        if (p->pattern.op == RtlOp::kSequence && p->pattern.children.size() == 2) {
          const RtlExpr& s0 = p->pattern.children[0];
          const RtlExpr& s1 = p->pattern.children[1];
          auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(s0);
          auto sub1 = RtlPatternAndNotes::FromSequenceSubInsn(s1);
          if (sub0.ok() && ExprReferencesRegInSrcOrMem(sub0->pattern, r)) {
            return true;
          }
          if (sub1.ok() && ExprReferencesRegInSrcOrMem(sub1->pattern, r)) {
            return true;
          }
          std::vector<int> s1_defs;
          if (sub1.ok()) {
            CollectDefinedRegs(sub1->pattern, &s1_defs);
          }
          const bool s1_kills = std::find(s1_defs.begin(), s1_defs.end(), r) != s1_defs.end();
          if (s0.op == RtlOp::kCallInsn) {
            if (!s1_kills && is_call_arg_reg(r)) {
              return true;
            }
            if (s1_kills || is_call_clobbered_reg(r)) {
              killed = true;
              break;
            }
            continue;
          }
          if (sub0.ok() && (sub0->pattern.op == RtlOp::kReturn || IsReturnExpr(sub0->pattern))) {
            if (!s1_kills && is_reg_used_at_fn_exit(r, /*include_epilogue_delay=*/false)) {
              return true;
            }
            handled_succs = true;
            break;
          }
          if (!s0.flags.unchanging && s1_kills) {
            killed = true;
            break;
          }
          handled_succs = true;
          for (size_t succ : scan_blk.successors) {
            if (s0.flags.unchanging && s1.flags.in_struct && s1_kills && succ != cur_b + 1) {
              continue;
            }
            if (succ < num_blocks && !visited[succ]) {
              visited[succ] = true;
              worklist.push_back(succ);
            }
          }
          break;
        }
        if (p->pattern.op == RtlOp::kReturn || IsReturnExpr(p->pattern)) {
          if (is_reg_used_at_fn_exit(r, /*include_epilogue_delay=*/true)) {
            return true;
          }
          handled_succs = true;
          break;
        }
        if (ExprReferencesRegInSrcOrMem(p->pattern, r)) {
          return true;
        }
        std::vector<int> insn_defs;
        CollectDefinedRegs(p->pattern, &insn_defs);
        if (std::find(insn_defs.begin(), insn_defs.end(), r) != insn_defs.end()) {
          killed = true;
          break;
        }
        bool has_call = false;
        p->pattern.Visit([&](const RtlExpr& e) {
          if (e.op == RtlOp::kCall) {
            has_call = true;
          }
        });
        if (has_call) {
          if (is_call_arg_reg(r)) {
            return true;
          }
          if (is_call_clobbered_reg(r)) {
            killed = true;
            break;
          }
          continue;
        }
        if (IsAnyJumpOrReturnTopExpr(p->pattern)) {
          handled_succs = true;
          for (size_t succ : scan_blk.successors) {
            if (succ < num_blocks && !visited[succ]) {
              visited[succ] = true;
              worklist.push_back(succ);
            }
          }
          break;
        }
      }
      if (!killed && !handled_succs) {
        if (scan_blk.successors.empty()) {
          if (is_reg_used_at_fn_exit(r, /*include_epilogue_delay=*/true)) {
            return true;
          }
        } else {
          for (size_t succ : scan_blk.successors) {
            if (succ < num_blocks && !visited[succ]) {
              visited[succ] = true;
              worklist.push_back(succ);
            }
          }
        }
      }
    }
  }
  return false;
}

bool IsSlotKilledOnFallthrough(const RtlControlFlowGraph& out, const RtlExpr& slot_pat,
                               size_t start_ft_b) {
  if (!slot_pat.IsSet() || !slot_pat.SetDest().IsReg()) {
    return false;
  }
  bool has_volatile = false;
  slot_pat.Visit([&](const RtlExpr& e) {
    if (e.flags.volatil || e.op == RtlOp::kAsmInput || e.op == RtlOp::kUnspecVolatile) {
      has_volatile = true;
    }
  });
  if (has_volatile) {
    return false;
  }
  std::vector<int> remaining_defs;
  CollectDefinedRegs(slot_pat, &remaining_defs);
  if (remaining_defs.empty()) {
    return false;
  }
  auto is_call_clobbered_reg = [](int r) -> bool {
    return r == 1 || (r >= 2 && r <= 15) || r == 24 || r == 25 || r == 31 || (r >= 32 && r <= 51);
  };
  auto call_reads_reg = [](const RtlExpr& call_pat, int r) -> bool {
    return (r >= 4 && r <= 7) || (r >= 44 && r <= 47) || ExprReferencesRegInSrcOrMem(call_pat, r);
  };
  size_t cur_b = start_ft_b;
  for (int depth = 0; depth < 3 && cur_b < out.rpo_blocks.size(); ++depth) {
    const auto& scan_blk = out.rpo_blocks[cur_b];
    bool stop_chain = false;
    for (const std::string& ft_insn : scan_blk.instructions) {
      auto fp = ParsePatternAndNotes(ft_insn);
      if (!fp.ok() || IsLoopHeaderNote(fp->pattern) || fp->pattern.op == RtlOp::kClobber) {
        continue;
      }
      if (fp->pattern.op == RtlOp::kUse) {
        for (int r : remaining_defs) {
          if (r != 31 && ExprReferencesRegInSrcOrMem(fp->pattern, r)) {
            return false;
          }
        }
        continue;
      }
      if (fp->pattern.op == RtlOp::kSequence && fp->pattern.children.size() == 2) {
        const RtlExpr& s0 = fp->pattern.children[0];
        const RtlExpr& s1 = fp->pattern.children[1];
        auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(s0);
        auto sub1 = RtlPatternAndNotes::FromSequenceSubInsn(s1);
        if (!sub0.ok() || !sub1.ok()) {
          return false;
        }
        std::vector<int> sub1_defs;
        CollectDefinedRegs(sub1->pattern, &sub1_defs);
        if (s0.op == RtlOp::kCallInsn) {
          for (int r : remaining_defs) {
            if (ExprReferencesRegInSrcOrMem(sub1->pattern, r)) {
              return false;
            }
            const bool killed_by_delay =
                std::find(sub1_defs.begin(), sub1_defs.end(), r) != sub1_defs.end();
            if (!killed_by_delay && call_reads_reg(sub0->pattern, r)) {
              return false;
            }
          }
          remaining_defs.erase(std::remove_if(remaining_defs.begin(), remaining_defs.end(),
                                              [&](int r) {
                                                return is_call_clobbered_reg(r) ||
                                                       std::find(sub1_defs.begin(), sub1_defs.end(),
                                                                 r) != sub1_defs.end();
                                              }),
                               remaining_defs.end());
          if (remaining_defs.empty()) {
            return true;
          }
          continue;
        }
        for (int r : remaining_defs) {
          if (ExprReferencesRegInSrcOrMem(sub0->pattern, r)) {
            return false;
          }
        }
        if (!s0.flags.unchanging) {
          for (int r : remaining_defs) {
            if (ExprReferencesRegInSrcOrMem(sub1->pattern, r)) {
              return false;
            }
          }
          remaining_defs.erase(std::remove_if(remaining_defs.begin(), remaining_defs.end(),
                                              [&](int r) {
                                                return std::find(sub1_defs.begin(), sub1_defs.end(),
                                                                 r) != sub1_defs.end();
                                              }),
                               remaining_defs.end());
          if (remaining_defs.empty()) {
            return true;
          }
        }
        if (IsSimpleUncondJumpExpr(sub0->pattern) && !s0.flags.unchanging) {
          break;
        }
        return false;
      }
      if (IsSimpleUncondJumpExpr(fp->pattern)) {
        break;
      }
      bool has_call = false;
      fp->pattern.Visit([&](const RtlExpr& e) {
        if (e.op == RtlOp::kCall) {
          has_call = true;
        }
      });
      if (has_call) {
        for (int r : remaining_defs) {
          if (call_reads_reg(fp->pattern, r)) {
            return false;
          }
        }
        remaining_defs.erase(
            std::remove_if(remaining_defs.begin(), remaining_defs.end(), is_call_clobbered_reg),
            remaining_defs.end());
        if (remaining_defs.empty()) {
          return true;
        }
        continue;
      }
      if (!IsRealActiveInsn(fp->pattern) || IsAnyJumpOrReturnTopExpr(fp->pattern)) {
        stop_chain = true;
        break;
      }
      for (int r : remaining_defs) {
        if (ExprReferencesRegInSrcOrMem(fp->pattern, r)) {
          return false;
        }
      }
      std::vector<int> ft_defs;
      CollectDefinedRegs(fp->pattern, &ft_defs);
      remaining_defs.erase(
          std::remove_if(
              remaining_defs.begin(), remaining_defs.end(),
              [&](int r) { return std::find(ft_defs.begin(), ft_defs.end(), r) != ft_defs.end(); }),
          remaining_defs.end());
      if (remaining_defs.empty()) {
        return true;
      }
    }
    if (stop_chain || scan_blk.successors.size() != 1) {
      break;
    }
    cur_b = scan_blk.successors[0];
  }
  return false;
}

bool ExprHasMemOrVolatile(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&](const RtlExpr& e) {
    if (e.op == RtlOp::kMem || e.flags.volatil || e.op == RtlOp::kAsmInput ||
        e.op == RtlOp::kUnspecVolatile) {
      found = true;
    }
  });
  return found;
}

bool BlockHasZeroRealActiveInsns(const RtlBasicBlock& blk);

bool IsUncondTargetStealAfterCond(const RtlControlFlowGraph& out, size_t ub,
                                  const std::vector<std::vector<size_t>>& cur_preds,
                                  const std::vector<bool>& entered_with_filled_end_jump) {
  if (ub == 0 || ub >= out.rpo_blocks.size()) {
    return false;
  }
  const auto& ublk = out.rpo_blocks[ub];
  if (ublk.has_label || ublk.instructions.size() != 1 || ublk.successors.size() != 1 ||
      cur_preds[ub].size() != 1 || cur_preds[ub][0] != ub - 1) {
    return false;
  }
  auto uparsed = ParsePatternAndNotes(ublk.instructions[0]);
  if (!uparsed.ok() || uparsed->pattern.op != RtlOp::kSequence ||
      uparsed->pattern.children.size() != 2 ||
      uparsed->pattern.children[0].op != RtlOp::kJumpInsn ||
      uparsed->pattern.children[1].op != RtlOp::kInsn ||
      uparsed->pattern.children[1].flags.in_struct) {
    return false;
  }
  auto ujump = RtlPatternAndNotes::FromSequenceSubInsn(uparsed->pattern.children[0]);
  auto uslot = RtlPatternAndNotes::FromSequenceSubInsn(uparsed->pattern.children[1]);
  if (!ujump.ok() || !uslot.ok() || !IsSimpleUncondJumpExpr(ujump->pattern) ||
      !uslot->pattern.IsSet() || !uslot->pattern.SetDest().IsReg() ||
      ExprHasMemOrVolatile(uslot->pattern)) {
    return false;
  }
  const int ureg = static_cast<int>(uslot->pattern.SetDest().int_val);
  if (ureg <= 0) {
    return false;
  }
  const size_t utarget_b = ublk.successors[0];
  if (utarget_b > 0 && utarget_b < out.rpo_blocks.size() && utarget_b - 1 != ub &&
      IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&ureg, 1), utarget_b)) {
    const auto& ufall_blk = out.rpo_blocks[utarget_b - 1];
    const bool ufall_reaches_target =
        std::find(ufall_blk.successors.begin(), ufall_blk.successors.end(), utarget_b) !=
        ufall_blk.successors.end();
    bool ufall_falls_through = ufall_reaches_target && !ufall_blk.instructions.empty();
    if (ufall_falls_through) {
      auto uflast = ParsePatternAndNotes(ufall_blk.instructions.back());
      ufall_falls_through = uflast.ok() && !IsUncondJumpOrReturnOrTableJumpTopExpr(uflast->pattern);
    }
    if (ufall_falls_through) {
      for (size_t k = ufall_blk.instructions.size(); k > 0;) {
        --k;
        auto ufp = ParsePatternAndNotes(ufall_blk.instructions[k]);
        if (!ufp.ok() || IsLoopHeaderNote(ufp->pattern) || ufp->pattern.op == RtlOp::kUse ||
            ufp->pattern.op == RtlOp::kClobber) {
          continue;
        }
        if (ufp->pattern.IsSet() && ufp->pattern.SetDest().IsReg() &&
            static_cast<int>(ufp->pattern.SetDest().int_val) == ureg &&
            ufp->pattern != uslot->pattern) {
          return false;
        }
        break;
      }
    }
  }
  const auto& pblk = out.rpo_blocks[ub - 1];
  if (pblk.successors.size() != 2 || pblk.instructions.empty()) {
    return false;
  }
  const size_t prev_target_b = (pblk.successors[0] == ub) ? pblk.successors[1] : pblk.successors[0];
  if (prev_target_b >= out.rpo_blocks.size() || prev_target_b == ub) {
    return false;
  }
  auto plast = ParsePatternAndNotes(pblk.instructions.back());
  if (!plast.ok()) {
    return false;
  }
  if (!entered_with_filled_end_jump[ub - 1] && IsCondJumpExpr(plast->pattern)) {
    const auto& pt_blk = out.rpo_blocks[prev_target_b];
    if (BlockHasZeroRealActiveInsns(pt_blk) && pt_blk.successors.empty() &&
        (ureg == 2 || ureg == 3 || ureg == 32 || ureg == 33)) {
      return false;
    }
    return !IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&ureg, 1), prev_target_b);
  }
  if (plast->pattern.op == RtlOp::kSequence && plast->pattern.children.size() == 2 &&
      plast->pattern.children[0].op == RtlOp::kJumpInsn &&
      plast->pattern.children[1].op == RtlOp::kInsn) {
    auto pjump = RtlPatternAndNotes::FromSequenceSubInsn(plast->pattern.children[0]);
    auto pslot = RtlPatternAndNotes::FromSequenceSubInsn(plast->pattern.children[1]);
    if (!pjump.ok() || !pslot.ok() || !IsCondJumpExpr(pjump->pattern) ||
        pjump->pattern.children.size() != 2 ||
        pjump->pattern.children[1].op != RtlOp::kIfThenElse ||
        pjump->pattern.children[1].children.size() != 3) {
      return false;
    }
    const RtlExpr& pcond = pjump->pattern.children[1].children[0];
    const auto& pt_blk = out.rpo_blocks[prev_target_b];
    bool pt_has_loop_note = false;
    if (!pt_blk.instructions.empty()) {
      auto ptf = ParsePatternAndNotes(pt_blk.instructions.front());
      pt_has_loop_note = ptf.ok() && IsLoopHeaderNote(ptf->pattern);
    }
    const bool pslot_kills_ureg = pslot->pattern.SetDest().IsReg() &&
                                  static_cast<int>(pslot->pattern.SetDest().int_val) == ureg &&
                                  !ExprReferencesRegInSrcOrMem(pslot->pattern, ureg);
    const bool pt_is_empty_ret_exit = BlockHasZeroRealActiveInsns(pt_blk) &&
                                      pt_blk.successors.empty() &&
                                      (ureg == 2 || ureg == 3 || ureg == 32 || ureg == 33);
    const bool ureg_dead_at_prev_target =
        !pt_is_empty_ret_exit &&
        !IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&ureg, 1), prev_target_b);
    if (pcond.op == RtlOp::kEq && prev_target_b > ub - 1 && !pt_has_loop_note &&
        pslot->pattern.IsSet() && (pslot_kills_ureg || ureg_dead_at_prev_target) &&
        pslot->pattern != uslot->pattern) {
      return true;
    }
  }
  return false;
}

bool BlockHasZeroRealActiveInsns(const RtlBasicBlock& blk) {
  for (const std::string& s : blk.instructions) {
    auto p = ParsePatternAndNotes(s);
    if (p.ok() && IsRealActiveInsn(p->pattern)) {
      return false;
    }
  }
  return true;
}

bool ReachesBlockAcrossZeroActiveFallthrough(const RtlControlFlowGraph& cfg, size_t start_b,
                                             size_t goal_b) {
  size_t cur_b = start_b;
  for (size_t step = 0; step <= cfg.rpo_blocks.size() && cur_b < cfg.rpo_blocks.size(); ++step) {
    if (cur_b == goal_b) {
      return true;
    }
    const auto& blk = cfg.rpo_blocks[cur_b];
    if (!BlockHasZeroRealActiveInsns(blk) || blk.successors.size() != 1) {
      return false;
    }
    cur_b = blk.successors[0];
  }
  return false;
}

bool IsOptimizeSkipSequenceBlock(const RtlControlFlowGraph& cfg, size_t b, const RtlExpr& slot_sub,
                                 bool is_cond_jump) {
  const auto& blk = cfg.rpo_blocks[b];
  if (blk.successors.size() != 2) {
    return false;
  }
  if (blk.successors[0] == blk.successors[1]) {
    return true;
  }
  if (is_cond_jump && slot_sub.flags.in_struct && b + 1 < cfg.rpo_blocks.size()) {
    const size_t ft_b = b + 1;
    if (blk.successors[0] == ft_b &&
        ReachesBlockAcrossZeroActiveFallthrough(cfg, ft_b, blk.successors[1])) {
      return true;
    }
    if (blk.successors[1] == ft_b &&
        ReachesBlockAcrossZeroActiveFallthrough(cfg, ft_b, blk.successors[0])) {
      return true;
    }
  }
  return false;
}

bool EqualIgnoringRegFlags(const RtlExpr& a, const RtlExpr& b) {
  if (a.op != b.op || a.mode != b.mode || a.int_val != b.int_val ||
      a.int_val_high != b.int_val_high || a.str_val != b.str_val ||
      a.children.size() != b.children.size()) {
    return false;
  }
  if (a.op != RtlOp::kReg && a.flags != b.flags) {
    return false;
  }
  for (size_t i = 0; i < a.children.size(); ++i) {
    if (!EqualIgnoringRegFlags(a.children[i], b.children[i])) {
      return false;
    }
  }
  return true;
}

std::optional<size_t> FindSharedReturnLatchInsertBlock(
    const RtlControlFlowGraph& out, const std::vector<std::vector<size_t>>& preds,
    size_t fallthrough_b) {
  size_t insert_b = fallthrough_b;
  while (insert_b < out.rpo_blocks.size()) {
    const auto& blk = out.rpo_blocks[insert_b];
    if (blk.has_label || preds[insert_b].size() != 1 || blk.successors.size() != 1 ||
        blk.successors[0] != insert_b + 1 || blk.instructions.empty()) {
      break;
    }
    bool all_notes = true;
    for (const std::string& s : blk.instructions) {
      auto p = ParsePatternAndNotes(s);
      if (!p.ok() || p->pattern.op != RtlOp::kNote) {
        all_notes = false;
        break;
      }
    }
    if (!all_notes) {
      break;
    }
    ++insert_b;
  }
  if (insert_b >= out.rpo_blocks.size()) {
    return std::nullopt;
  }
  size_t cur = insert_b;
  bool saw_shared = false;
  while (cur < out.rpo_blocks.size()) {
    const auto& cblk = out.rpo_blocks[cur];
    if (cblk.has_label || preds[cur].size() >= 2) {
      saw_shared = true;
    }
    const bool is_exit = cblk.successors.empty();
    for (size_t fi = 0; fi < cblk.instructions.size(); ++fi) {
      auto fp = ParsePatternAndNotes(cblk.instructions[fi]);
      if (!fp.ok()) {
        return std::nullopt;
      }
      const bool is_use_or_clobber =
          (fp->pattern.op == RtlOp::kUse || fp->pattern.op == RtlOp::kClobber);
      const bool is_last_return =
          (is_exit && fi + 1 == cblk.instructions.size() && IsReturnExpr(fp->pattern));
      if (!is_use_or_clobber && !is_last_return) {
        return std::nullopt;
      }
    }
    if (is_exit) {
      return saw_shared ? std::optional<size_t>(insert_b) : std::nullopt;
    }
    if (cblk.successors.size() != 1 || cblk.successors[0] != cur + 1) {
      return std::nullopt;
    }
    cur = cur + 1;
  }
  return std::nullopt;
}

enum class FallthroughSequenceResult {
  kHandledOrSkipped,
  kProceedAsTargetSteal,
};

ptrdiff_t FindOppositeArithCompensationInFallthroughBlock(
    const RtlControlFlowGraph& out, const std::vector<std::vector<size_t>>& preds, size_t p_blk,
    size_t target_b, const RtlExpr& slot_pat) {
  if (!slot_pat.IsSet() || !slot_pat.SetDest().IsReg()) {
    return -1;
  }
  const RtlExpr& dst_e = slot_pat.SetDest();
  const RtlExpr& src_e = slot_pat.SetSrc();
  const int dst_r = static_cast<int>(dst_e.int_val);
  if (dst_r <= 0 || (src_e.op != RtlOp::kPlus && src_e.op != RtlOp::kMinus) ||
      src_e.children.size() != 2 || !EqualIgnoringRegFlags(src_e.children[0], dst_e) ||
      ExprReferencesRegInSrcOrMem(src_e.children[1], dst_r)) {
    return -1;
  }
  const size_t ft_b = p_blk + 1;
  if (ft_b >= out.rpo_blocks.size() || out.rpo_blocks[ft_b].has_label || preds[ft_b].size() != 1 ||
      preds[ft_b][0] != p_blk) {
    return -1;
  }
  RtlExpr comp_src = src_e;
  if (src_e.children[1].op == RtlOp::kConstInt) {
    comp_src.children[1].int_val = -src_e.children[1].int_val;
  } else {
    comp_src.op = (src_e.op == RtlOp::kPlus) ? RtlOp::kMinus : RtlOp::kPlus;
  }
  const RtlExpr comp_pat = RtlExpr::MakeSet(dst_e, std::move(comp_src));
  const auto& ft_blk = out.rpo_blocks[ft_b];
  ptrdiff_t comp_idx = -1;
  for (size_t k = 0; k < ft_blk.instructions.size(); ++k) {
    auto kp = ParsePatternAndNotes(ft_blk.instructions[k]);
    if (!kp.ok() || IsLoopHeaderNote(kp->pattern) || kp->pattern.op == RtlOp::kClobber) {
      continue;
    }
    if (kp->pattern.op == RtlOp::kUse) {
      if (dst_r != 31 && ExprReferencesRegInSrcOrMem(kp->pattern, dst_r)) {
        return -1;
      }
      continue;
    }
    if (IsRealActiveInsn(kp->pattern)) {
      if (EqualIgnoringRegFlags(kp->pattern, comp_pat)) {
        comp_idx = static_cast<ptrdiff_t>(k);
      }
      break;
    }
  }
  if (comp_idx < 0) {
    return -1;
  }
  bool live_after = false;
  bool killed_in_ft = false;
  const auto is_call_clobbered_reg = [](int r) -> bool {
    return r == 1 || (r >= 2 && r <= 15) || r == 24 || r == 25 || r == 31 || (r >= 32 && r <= 51);
  };
  for (size_t k = static_cast<size_t>(comp_idx) + 1; k < ft_blk.instructions.size(); ++k) {
    auto qp = ParsePatternAndNotes(ft_blk.instructions[k]);
    if (!qp.ok() || IsLoopHeaderNote(qp->pattern) || qp->pattern.op == RtlOp::kClobber) {
      continue;
    }
    if (qp->pattern.op == RtlOp::kUse) {
      if (dst_r != 31 && ExprReferencesRegInSrcOrMem(qp->pattern, dst_r)) {
        live_after = true;
        break;
      }
      continue;
    }
    if (qp->pattern.op == RtlOp::kSequence && qp->pattern.children.size() == 2) {
      const RtlExpr& s0 = qp->pattern.children[0];
      const RtlExpr& s1 = qp->pattern.children[1];
      auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(s0);
      auto sub1 = RtlPatternAndNotes::FromSequenceSubInsn(s1);
      if ((sub0.ok() && ExprReferencesRegInSrcOrMem(sub0->pattern, dst_r)) ||
          (sub1.ok() && ExprReferencesRegInSrcOrMem(sub1->pattern, dst_r))) {
        live_after = true;
        break;
      }
      std::vector<int> sub1_defs;
      if (sub1.ok()) {
        CollectDefinedRegs(sub1->pattern, &sub1_defs);
      }
      const bool killed_by_delay =
          std::find(sub1_defs.begin(), sub1_defs.end(), dst_r) != sub1_defs.end();
      if (s0.op == RtlOp::kCallInsn) {
        if (!killed_by_delay && ((dst_r >= 4 && dst_r <= 7) || (dst_r >= 44 && dst_r <= 47))) {
          live_after = true;
          break;
        }
        if (killed_by_delay || is_call_clobbered_reg(dst_r)) {
          killed_in_ft = true;
          break;
        }
      } else if (!s0.flags.unchanging && killed_by_delay) {
        killed_in_ft = true;
        break;
      }
      continue;
    }
    if (ExprReferencesRegInSrcOrMem(qp->pattern, dst_r)) {
      live_after = true;
      break;
    }
    std::vector<int> qdefs;
    CollectDefinedRegs(qp->pattern, &qdefs);
    if (std::find(qdefs.begin(), qdefs.end(), dst_r) != qdefs.end()) {
      killed_in_ft = true;
      break;
    }
    bool has_call = false;
    qp->pattern.Visit([&](const RtlExpr& e) {
      if (e.op == RtlOp::kCall) {
        has_call = true;
      }
    });
    if (has_call) {
      if ((dst_r >= 4 && dst_r <= 7) || (dst_r >= 44 && dst_r <= 47)) {
        live_after = true;
        break;
      }
      if (is_call_clobbered_reg(dst_r)) {
        killed_in_ft = true;
        break;
      }
    }
  }
  if (!live_after && !killed_in_ft) {
    if (target_b <= p_blk) {
      live_after = true;
    } else if (ft_blk.successors.empty()) {
      live_after = (dst_r == 2 || dst_r == 3 || dst_r == 32 || dst_r == 33);
    } else {
      for (size_t succ_b : ft_blk.successors) {
        if (IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&dst_r, 1), succ_b)) {
          live_after = true;
          break;
        }
      }
    }
  }
  return live_after ? comp_idx : -1;
}

size_t ComputeTargetStolenInsertPos(const RtlControlFlowGraph& out, size_t tgt_b,
                                    const RtlBasicBlock& tgt_blk, const RtlPatternAndNotes& slot_pn,
                                    const CompilationProfile& profile);

FallthroughSequenceResult TryInvertFallthroughThreadSequence(
    RtlControlFlowGraph* out_ptr, std::vector<bool>* entered_with_filled_end_jump_ptr,
    std::vector<EagerStolenSlotSite>* stolen_sites, size_t b, const RtlExpr& jump_sub,
    const RtlPatternAndNotes& jump_pat_notes, const RtlPatternAndNotes& slot_pat_notes,
    bool is_cond_jump, bool is_simple_uncond_jump, const CompilationProfile& profile) {
  RtlControlFlowGraph& out = *out_ptr;
  std::vector<bool>& entered_with_filled_end_jump = *entered_with_filled_end_jump_ptr;
  auto& blk = out.rpo_blocks[b];
  const RtlExpr& jump_pat = jump_pat_notes.pattern;
  auto insert_block_at = [&](size_t insert_idx, RtlBasicBlock new_blk) {
    InsertBlockAt(&out, insert_idx, std::move(new_blk), stolen_sites);
    entered_with_filled_end_jump.insert(
        entered_with_filled_end_jump.begin() + static_cast<ptrdiff_t>(insert_idx), false);
  };

  if (!is_cond_jump) {
    const auto cur_preds = BuildPredecessors(out);
    if (is_simple_uncond_jump &&
        IsUncondTargetStealAfterCond(out, b, cur_preds, entered_with_filled_end_jump)) {
      return FallthroughSequenceResult::kProceedAsTargetSteal;
    }
    return FallthroughSequenceResult::kHandledOrSkipped;
  }

  std::vector<int> slot_defs;
  CollectDefinedRegs(slot_pat_notes.pattern, &slot_defs);
  bool jump_reads_slot_def = false;
  for (int r : slot_defs) {
    if (ExprReferencesRegInSrcOrMem(jump_pat, r)) {
      jump_reads_slot_def = true;
      break;
    }
  }

  // Trailing conditional jump (e.g., loop latch) whose fallthrough block
  // became empty when Stage 0 stripped the function epilogue: restore the
  // trailing fallthrough block holding `slot_pat`, unless `slot_defs` is
  // live at the branch target (proving `slot_pat` was stolen from target).
  if (blk.successors.size() == 1 && b + 1 == out.rpo_blocks.size() &&
      (jump_reads_slot_def || jump_sub.flags.unchanging)) {
    if (jump_sub.flags.unchanging || !IsAnyRegLiveAtBlockEntry(out, slot_defs, blk.successors[0])) {
      blk.instructions.back() = jump_pat_notes.ToGccPrintRtlString();
      RtlBasicBlock new_ft;
      new_ft.id = static_cast<uint32_t>(b + 1);
      new_ft.has_label = false;
      new_ft.instructions.push_back(slot_pat_notes.ToGccPrintRtlString());
      out.rpo_blocks[b].successors.push_back(b + 1);
      out.rpo_blocks.push_back(std::move(new_ft));
      entered_with_filled_end_jump.push_back(false);
      return FallthroughSequenceResult::kHandledOrSkipped;
    }
    return FallthroughSequenceResult::kProceedAsTargetSteal;
  }

  if (blk.successors.size() != 2 || b + 1 >= out.rpo_blocks.size()) {
    return FallthroughSequenceResult::kHandledOrSkipped;
  }
  const size_t fallthrough_b = b + 1;
  const size_t cond_target_b =
      (blk.successors[0] == fallthrough_b) ? blk.successors[1] : blk.successors[0];

  const auto preds = BuildPredecessors(out);
  auto& ft_blk = out.rpo_blocks[fallthrough_b];
  const bool own_fallthrough =
      !ft_blk.has_label && preds[fallthrough_b].size() == 1 && preds[fallthrough_b][0] == b;

  bool ft_stole_from_end_jump = false;
  RtlPatternAndNotes ft_bare_jump_pat_notes;
  if (own_fallthrough && !entered_with_filled_end_jump[fallthrough_b] &&
      ft_blk.instructions.size() == 1) {
    auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
    if (ft_parsed.ok() &&
        (IsSimpleUncondJumpExpr(ft_parsed->pattern) || IsReturnExpr(ft_parsed->pattern))) {
      ft_stole_from_end_jump = true;
      ft_bare_jump_pat_notes = *ft_parsed;
    }
  } else if (own_fallthrough && !entered_with_filled_end_jump[fallthrough_b] &&
             ft_blk.instructions.size() == 2) {
    auto mid_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
    auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[1]);
    if (mid_parsed.ok() && ft_parsed.ok() && mid_parsed->pattern.IsSet() &&
        mid_parsed->pattern.SetDest().IsMem() && IsSimpleUncondJumpExpr(ft_parsed->pattern) &&
        !ExpressionsConflict(mid_parsed->pattern, slot_pat_notes.pattern)) {
      ft_stole_from_end_jump = true;
      ft_bare_jump_pat_notes = *ft_parsed;
    }
  }

  if (ft_stole_from_end_jump && !jump_sub.flags.unchanging) {
    if ((cond_target_b < out.rpo_blocks.size() &&
         IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b)) ||
        (jump_reads_slot_def &&
         IsSlotKilledOnFallthrough(out, slot_pat_notes.pattern, fallthrough_b))) {
      ft_stole_from_end_jump = false;
    } else if (IsReturnExpr(ft_bare_jump_pat_notes.pattern) && !jump_reads_slot_def) {
      bool sets_ret_val_or_mem =
          slot_pat_notes.pattern.IsSet() && slot_pat_notes.pattern.SetDest().IsMem();
      for (int r : slot_defs) {
        if (r == 2 || r == 3 || r == 32 || r == 33) {
          sets_ret_val_or_mem = true;
          break;
        }
      }
      if (!sets_ret_val_or_mem) {
        ft_stole_from_end_jump = false;
      }
    }
  }

  if (!jump_sub.flags.unchanging && jump_reads_slot_def && !ft_stole_from_end_jump &&
      !IsAnyRegLiveAtBlockEntry(out, slot_defs, fallthrough_b) &&
      (IsSlotKilledOnFallthrough(out, slot_pat_notes.pattern, fallthrough_b) ||
       (cond_target_b < out.rpo_blocks.size() &&
        IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b)))) {
    return FallthroughSequenceResult::kProceedAsTargetSteal;
  }

  std::optional<size_t> shared_ret_insert_b;
  if (!jump_sub.flags.unchanging && !jump_reads_slot_def && !ft_stole_from_end_jump &&
      cond_target_b <= b && cond_target_b < out.rpo_blocks.size() && !slot_defs.empty() &&
      !IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b) &&
      !IsSlotKilledOnFallthrough(out, slot_pat_notes.pattern, fallthrough_b)) {
    bool sets_ret_reg = false;
    for (int r : slot_defs) {
      if (r == 2 || r == 3 || r == 32 || r == 33) {
        sets_ret_reg = true;
        break;
      }
    }
    if (sets_ret_reg) {
      shared_ret_insert_b = FindSharedReturnLatchInsertBlock(out, preds, fallthrough_b);
    }
  }
  const bool shared_return_latch_ft = shared_ret_insert_b.has_value();

  bool blocks_earlier_ft_end_jump_steal = false;
  if (!jump_sub.flags.unchanging && !jump_reads_slot_def && !ft_stole_from_end_jump &&
      !shared_return_latch_ft && blk.has_label && !slot_defs.empty() &&
      cond_target_b < out.rpo_blocks.size() &&
      !IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b)) {
    bool slot_def_set_earlier_in_b = false;
    for (size_t bi = 0; bi + 1 < blk.instructions.size(); ++bi) {
      auto bp = ParsePatternAndNotes(blk.instructions[bi]);
      if (!bp.ok() || !IsRealActiveInsn(bp->pattern)) {
        continue;
      }
      std::vector<int> bdefs;
      CollectDefinedRegs(bp->pattern, &bdefs);
      for (int r : slot_defs) {
        if (std::find(bdefs.begin(), bdefs.end(), r) != bdefs.end()) {
          slot_def_set_earlier_in_b = true;
          break;
        }
      }
      if (slot_def_set_earlier_in_b) {
        break;
      }
    }
    if (!slot_def_set_earlier_in_b) {
      for (size_t p_blk : preds[b]) {
        if (p_blk >= b || p_blk + 1 >= out.rpo_blocks.size() ||
            out.rpo_blocks[p_blk].instructions.empty()) {
          continue;
        }
        const auto& p_ft = out.rpo_blocks[p_blk + 1];
        if (p_ft.has_label || preds[p_blk + 1].size() != 1 || preds[p_blk + 1][0] != p_blk ||
            p_ft.instructions.empty()) {
          continue;
        }
        auto p_ft_last = ParsePatternAndNotes(p_ft.instructions.back());
        auto p_last = ParsePatternAndNotes(out.rpo_blocks[p_blk].instructions.back());
        if (!p_ft_last.ok() || !p_last.ok()) {
          continue;
        }
        std::vector<int> p_defs;
        if (!entered_with_filled_end_jump[p_blk + 1] &&
            (IsSimpleUncondJumpExpr(p_ft_last->pattern) || IsReturnExpr(p_ft_last->pattern)) &&
            p_last->pattern.op == RtlOp::kSequence && p_last->pattern.children.size() == 2 &&
            p_last->pattern.children[0].op == RtlOp::kJumpInsn &&
            !p_last->pattern.children[1].flags.in_struct) {
          auto p_jump = RtlPatternAndNotes::FromSequenceSubInsn(p_last->pattern.children[0]);
          auto p_slot = RtlPatternAndNotes::FromSequenceSubInsn(p_last->pattern.children[1]);
          if (p_jump.ok() && p_slot.ok() && IsCondJumpExpr(p_jump->pattern)) {
            CollectDefinedRegs(p_slot->pattern, &p_defs);
          }
        } else if (entered_with_filled_end_jump[p_blk + 1] &&
                   !entered_with_filled_end_jump[p_blk] && p_ft.instructions.size() == 1 &&
                   IsCondJumpExpr(p_last->pattern) && p_ft_last->pattern.op == RtlOp::kSequence &&
                   p_ft_last->pattern.children.size() == 2 &&
                   p_ft_last->pattern.children[0].op == RtlOp::kJumpInsn &&
                   !p_ft_last->pattern.children[1].flags.in_struct) {
          auto ft_jump = RtlPatternAndNotes::FromSequenceSubInsn(p_ft_last->pattern.children[0]);
          auto ft_slot = RtlPatternAndNotes::FromSequenceSubInsn(p_ft_last->pattern.children[1]);
          if (ft_jump.ok() && ft_slot.ok() &&
              (IsSimpleUncondJumpExpr(ft_jump->pattern) || IsReturnExpr(ft_jump->pattern))) {
            CollectDefinedRegs(ft_slot->pattern, &p_defs);
          }
        }
        bool overlaps = false;
        for (int r : p_defs) {
          if (std::find(slot_defs.begin(), slot_defs.end(), r) != slot_defs.end()) {
            overlaps = true;
            break;
          }
        }
        if (overlaps) {
          blocks_earlier_ft_end_jump_steal = true;
          break;
        }
      }
    }
  }
  bool diamond_split_di_subreg_steal = false;
  if (!jump_sub.flags.unchanging && !jump_reads_slot_def && !ft_stole_from_end_jump &&
      !shared_return_latch_ft && !blocks_earlier_ft_end_jump_steal && own_fallthrough &&
      cond_target_b == fallthrough_b + 1 && cond_target_b < out.rpo_blocks.size() &&
      ft_blk.successors.size() == 1 && ft_blk.successors[0] == cond_target_b &&
      !out.rpo_blocks[cond_target_b].instructions.empty() && slot_pat_notes.pattern.IsSet() &&
      slot_pat_notes.pattern.SetDest().IsSubreg() && slot_pat_notes.pattern.SetSrc().IsSubreg() &&
      slot_pat_notes.pattern.SetDest().mode == RtlMachineMode::kSi &&
      slot_pat_notes.pattern.SetSrc().mode == RtlMachineMode::kSi &&
      !slot_pat_notes.pattern.SetDest().children.empty() &&
      !slot_pat_notes.pattern.SetSrc().children.empty() &&
      slot_pat_notes.pattern.SetDest().children[0].IsReg() &&
      slot_pat_notes.pattern.SetSrc().children[0].IsReg() &&
      slot_pat_notes.pattern.SetDest().children[0].mode == RtlMachineMode::kDi &&
      slot_pat_notes.pattern.SetSrc().children[0].mode == RtlMachineMode::kDi) {
    const auto& tgt_blk = out.rpo_blocks[cond_target_b];
    auto t0 = ParsePatternAndNotes(tgt_blk.instructions.front());
    if (t0.ok() && t0->pattern.IsSet() && t0->pattern.SetDest().IsSubreg() &&
        t0->pattern.SetSrc().IsSubreg() && t0->pattern.SetDest().mode == RtlMachineMode::kSi &&
        t0->pattern.SetSrc().mode == RtlMachineMode::kSi &&
        !t0->pattern.SetDest().children.empty() && !t0->pattern.SetSrc().children.empty() &&
        t0->pattern.SetDest().children[0].IsReg() && t0->pattern.SetSrc().children[0].IsReg() &&
        t0->pattern.SetDest().children[0].mode == RtlMachineMode::kDi &&
        t0->pattern.SetSrc().children[0].mode == RtlMachineMode::kDi) {
      const RtlExpr& d0 = slot_pat_notes.pattern.SetDest();
      const RtlExpr& s0 = slot_pat_notes.pattern.SetSrc();
      const RtlExpr& d1 = t0->pattern.SetDest();
      const RtlExpr& s1 = t0->pattern.SetSrc();
      const bool complementary =
          d0.children[0].RegNo() == d1.children[0].RegNo() &&
          s0.children[0].RegNo() == s1.children[0].RegNo() &&
          d0.children[0].RegNo() != s0.children[0].RegNo() &&
          ((d0.int_val == 0 && s0.int_val == 0 && d1.int_val == 1 && s1.int_val == 1) ||
           (d0.int_val == 1 && s0.int_val == 1 && d1.int_val == 0 && s1.int_val == 0));
      if (complementary) {
        bool ft_conflicts = false;
        for (const std::string& fs : ft_blk.instructions) {
          auto fp = ParsePatternAndNotes(fs);
          if (!fp.ok() || IsAnyJumpOrReturnTopExpr(fp->pattern) ||
              ExpressionsConflict(fp->pattern, slot_pat_notes.pattern)) {
            ft_conflicts = true;
            break;
          }
        }
        diamond_split_di_subreg_steal = !ft_conflicts;
      }
    }
  }
  if (diamond_split_di_subreg_steal) {
    blk.instructions.back() = jump_pat_notes.ToGccPrintRtlString();
    out.rpo_blocks[cond_target_b].instructions.insert(
        out.rpo_blocks[cond_target_b].instructions.begin(), slot_pat_notes.ToGccPrintRtlString());
    return FallthroughSequenceResult::kHandledOrSkipped;
  }
  bool target_has_use_before_kill = false;
  if (cond_target_b < out.rpo_blocks.size() && !slot_defs.empty()) {
    for (int r : slot_defs) {
      for (const std::string& ts : out.rpo_blocks[cond_target_b].instructions) {
        auto tp = ParsePatternAndNotes(ts);
        if (!tp.ok() || IsLoopHeaderNote(tp->pattern) || tp->pattern.op == RtlOp::kClobber) {
          continue;
        }
        if (tp->pattern.op == RtlOp::kUse) {
          if (r != 31 && ExprReferencesRegInSrcOrMem(tp->pattern, r)) {
            target_has_use_before_kill = true;
            break;
          }
          continue;
        }
        if (ExprReferencesRegInSrcOrMem(tp->pattern, r)) {
          break;
        }
        std::vector<int> tdefs;
        CollectDefinedRegs(tp->pattern, &tdefs);
        if (std::find(tdefs.begin(), tdefs.end(), r) != tdefs.end()) {
          break;
        }
      }
      if (target_has_use_before_kill) {
        break;
      }
    }
  }
  bool own_ft_dead_on_target_steal = false;
  if (!jump_sub.flags.unchanging && !jump_reads_slot_def && !ft_stole_from_end_jump &&
      !shared_return_latch_ft && !blocks_earlier_ft_end_jump_steal && own_fallthrough &&
      !ft_blk.instructions.empty() && !slot_defs.empty() &&
      !ExprHasMemOrVolatile(slot_pat_notes.pattern) && cond_target_b > b &&
      cond_target_b < out.rpo_blocks.size() && !target_has_use_before_kill &&
      !IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b)) {
    const RtlExpr* ite = GetCondJumpIfThenElse(jump_pat_notes.pattern);
    bool mostly_false_fwd = false;
    if (ite != nullptr && ite->children.size() == 3) {
      const RtlExpr& cond = ite->children[0];
      const bool lbl_first =
          (ite->children[1].op == RtlOp::kLabelRef && ite->children[2].op == RtlOp::kPc);
      const bool pc_first =
          (ite->children[1].op == RtlOp::kPc && ite->children[2].op == RtlOp::kLabelRef);
      if ((lbl_first && cond.op == RtlOp::kEq) || (pc_first && cond.op == RtlOp::kNe)) {
        mostly_false_fwd = true;
      } else if (lbl_first && cond.children.size() == 2 && cond.children[1].IsConstInt() &&
                 cond.children[1].IntVal() == 0 &&
                 (cond.op == RtlOp::kLt || cond.op == RtlOp::kLe || cond.op == RtlOp::kLtu ||
                  cond.op == RtlOp::kLeu)) {
        mostly_false_fwd = true;
      }
    }
    bool ft_first_reads_slot_def = false;
    for (const std::string& fs : ft_blk.instructions) {
      auto fp = ParsePatternAndNotes(fs);
      if (!fp.ok() || !IsRealActiveInsn(fp->pattern)) {
        continue;
      }
      const RtlExpr* check_pat = &fp->pattern;
      RtlPatternAndNotes seq_slot_pn;
      if (fp->pattern.op == RtlOp::kSequence && fp->pattern.children.size() == 2) {
        auto sp = RtlPatternAndNotes::FromSequenceSubInsn(fp->pattern.children[1]);
        if (sp.ok()) {
          seq_slot_pn = std::move(*sp);
          check_pat = &seq_slot_pn.pattern;
        }
      }
      for (int r : slot_defs) {
        if (ExprReferencesRegInSrcOrMem(*check_pat, r)) {
          ft_first_reads_slot_def = true;
          break;
        }
      }
      break;
    }
    bool b_has_simple_cand = false;
    size_t win_start = 0;
    for (size_t bi = 0; bi + 1 < blk.instructions.size(); ++bi) {
      auto bp = ParsePatternAndNotes(blk.instructions[bi]);
      if (!bp.ok() || bp->pattern.op == RtlOp::kSequence || IsAnyJumpOrReturnTopExpr(bp->pattern)) {
        win_start = bi + 1;
        continue;
      }
      bool has_call = false;
      bp->pattern.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kCall) {
          has_call = true;
        }
      });
      if (has_call) {
        win_start = bi + 1;
      }
    }
    for (size_t bi = win_start; bi + 1 < blk.instructions.size(); ++bi) {
      auto bp = ParsePatternAndNotes(blk.instructions[bi]);
      if (!bp.ok() || !IsRealActiveInsn(bp->pattern) || bp->pattern.op == RtlOp::kParallel) {
        continue;
      }
      bool is_load = false;
      if (bp->pattern.IsSet() && !bp->pattern.SetDest().IsMem()) {
        bp->pattern.SetSrc().Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kMem) {
            is_load = true;
          }
        });
      }
      if (is_load || ExpressionsConflict(bp->pattern, jump_pat_notes.pattern)) {
        continue;
      }
      bool blocked_later = false;
      for (size_t bj = bi + 1; bj + 1 < blk.instructions.size(); ++bj) {
        auto lp = ParsePatternAndNotes(blk.instructions[bj]);
        if (lp.ok() && IsRealActiveInsn(lp->pattern) &&
            ExpressionsConflict(bp->pattern, lp->pattern)) {
          blocked_later = true;
          break;
        }
      }
      if (!blocked_later) {
        b_has_simple_cand = true;
        break;
      }
    }
    bool ft_tail_shape_ok = false;
    auto ft_last_p = ParsePatternAndNotes(ft_blk.instructions.back());
    if (ft_last_p.ok()) {
      if (entered_with_filled_end_jump[fallthrough_b]) {
        ft_tail_shape_ok = ExpressionsConflict(slot_pat_notes.pattern, ft_last_p->pattern);
      } else {
        ft_tail_shape_ok = !IsAnyJumpOrReturnTopExpr(ft_last_p->pattern);
      }
    }
    own_ft_dead_on_target_steal =
        mostly_false_fwd && ft_first_reads_slot_def && !b_has_simple_cand && ft_tail_shape_ok;
  }

  if (!jump_sub.flags.unchanging && own_fallthrough &&
      (cond_target_b <= b || (cond_target_b < out.rpo_blocks.size() &&
                              IsAnyRegLiveAtBlockEntry(out, slot_defs, cond_target_b))) &&
      FindOppositeArithCompensationInFallthroughBlock(out, preds, b, cond_target_b,
                                                      slot_pat_notes.pattern) >= 0) {
    return FallthroughSequenceResult::kProceedAsTargetSteal;
  }

  if (!jump_sub.flags.unchanging && !jump_reads_slot_def && !ft_stole_from_end_jump &&
      !shared_return_latch_ft && !blocks_earlier_ft_end_jump_steal &&
      !own_ft_dead_on_target_steal) {
    return FallthroughSequenceResult::kHandledOrSkipped;
  }

  blk.instructions.back() = jump_pat_notes.ToGccPrintRtlString();

  if (ft_stole_from_end_jump &&
      (ft_blk.instructions.size() == 2 || !jump_reads_slot_def || jump_sub.flags.unchanging)) {
    ft_blk.instructions.back() =
        RtlExpr::MakeSequence(
            {
                RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, ft_bare_jump_pat_notes),
                RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, slot_pat_notes),
            })
            .ToGccPrintRtlString();
  } else if (own_fallthrough && !shared_return_latch_ft) {
    const size_t ft_ins_pos =
        ComputeTargetStolenInsertPos(out, fallthrough_b, ft_blk, slot_pat_notes, profile);
    const std::string slot_str = slot_pat_notes.ToGccPrintRtlString();
    ft_blk.instructions.insert(ft_blk.instructions.begin() + static_cast<ptrdiff_t>(ft_ins_pos),
                               slot_str);
    if (stolen_sites != nullptr) {
      stolen_sites->push_back(EagerStolenSlotSite{
          .block_idx = fallthrough_b,
          .slot_idx = ft_ins_pos,
          .opposite_block_idx = cond_target_b,
          .thread_if_true = false,
          .is_annulled = jump_sub.flags.unchanging,
          .valid = true,
          .slot_text = slot_str,
      });
    }
  } else {
    const size_t ins_b = shared_return_latch_ft ? *shared_ret_insert_b : fallthrough_b;
    if (shared_return_latch_ft && ins_b < out.rpo_blocks.size() &&
        !out.rpo_blocks[ins_b].has_label && preds[ins_b].size() == 1) {
      const size_t ft_ins_pos =
          ComputeTargetStolenInsertPos(out, ins_b, out.rpo_blocks[ins_b], slot_pat_notes, profile);
      const std::string slot_str = slot_pat_notes.ToGccPrintRtlString();
      out.rpo_blocks[ins_b].instructions.insert(
          out.rpo_blocks[ins_b].instructions.begin() + static_cast<ptrdiff_t>(ft_ins_pos),
          slot_str);
      if (stolen_sites != nullptr) {
        stolen_sites->push_back(EagerStolenSlotSite{
            .block_idx = ins_b,
            .slot_idx = ft_ins_pos,
            .opposite_block_idx = cond_target_b,
            .thread_if_true = false,
            .is_annulled = jump_sub.flags.unchanging,
            .valid = true,
            .slot_text = slot_str,
        });
      }
    } else {
      RtlBasicBlock new_ft;
      new_ft.has_label = false;
      new_ft.instructions.push_back(slot_pat_notes.ToGccPrintRtlString());
      new_ft.successors = {ins_b + 1};
      insert_block_at(ins_b, std::move(new_ft));
      for (size_t& succ : out.rpo_blocks[ins_b - 1].successors) {
        if (succ == ins_b + 1) {
          succ = ins_b;
          break;
        }
      }
    }
  }
  return FallthroughSequenceResult::kHandledOrSkipped;
}

bool ComputeLoopVtopNeededForTargetSteal(const RtlControlFlowGraph& out,
                                         const std::vector<std::vector<size_t>>& preds, size_t b,
                                         size_t target_b, bool is_cond_jump,
                                         bool pred_falls_through_to_target,
                                         const std::vector<size_t>& same_slot_jumpers,
                                         const RtlExpr& slot_pat) {
  if (out.rpo_blocks[target_b].successors.empty()) {
    return false;
  }
  bool target_is_loop_hdr = (is_cond_jump && target_b <= b);
  if (!target_is_loop_hdr) {
    for (size_t p_blk : preds[target_b]) {
      if (p_blk >= target_b) {
        target_is_loop_hdr = true;
        break;
      }
    }
  }
  if (!target_is_loop_hdr) {
    for (size_t succ : out.rpo_blocks[target_b].successors) {
      if (succ <= target_b) {
        target_is_loop_hdr = true;
        break;
      }
    }
  }
  if (!target_is_loop_hdr && pred_falls_through_to_target && target_b > 0) {
    for (size_t succ : out.rpo_blocks[target_b - 1].successors) {
      if (succ < target_b) {
        target_is_loop_hdr = true;
        break;
      }
    }
  }
  if (!target_is_loop_hdr) {
    return false;
  }
  bool any_jumper_needs_loop_pred = false;
  for (size_t j_blk : same_slot_jumpers) {
    if (out.rpo_blocks[j_blk].instructions.empty()) {
      continue;
    }
    auto j_last = ParsePatternAndNotes(out.rpo_blocks[j_blk].instructions.back());
    if (!j_last.ok() || j_last->pattern.op != RtlOp::kSequence ||
        j_last->pattern.children.empty()) {
      continue;
    }
    auto j_jump = RtlPatternAndNotes::FromSequenceSubInsn(j_last->pattern.children[0]);
    if (!j_jump.ok() || !IsCondJumpExpr(j_jump->pattern) || j_jump->pattern.children.size() != 2 ||
        j_jump->pattern.children[1].op != RtlOp::kIfThenElse ||
        j_jump->pattern.children[1].children.size() != 3) {
      continue;
    }
    const RtlExpr& cond = j_jump->pattern.children[1].children[0];
    const bool cond_predicts_false_without_loop =
        cond.op == RtlOp::kEq ||
        ((cond.op == RtlOp::kLe || cond.op == RtlOp::kLt) && cond.children.size() == 2 &&
         cond.children[1].op == RtlOp::kConstInt && cond.children[1].int_val == 0);
    if (!cond_predicts_false_without_loop) {
      continue;
    }
    auto is_live_at_restored_target = [&](int regno) -> bool {
      if (ExprReferencesRegInSrcOrMem(slot_pat, regno)) {
        return true;
      }
      if (slot_pat.IsSet() && slot_pat.SetDest().IsReg() && !ExprHasMemOrVolatile(slot_pat) &&
          static_cast<int>(slot_pat.SetDest().int_val) == regno) {
        return false;
      }
      return IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&regno, 1), target_b);
    };
    bool ft_has_competing_candidate = false;
    if (j_blk + 1 < out.rpo_blocks.size() && !out.rpo_blocks[j_blk + 1].has_label &&
        preds[j_blk + 1].size() == 1 && preds[j_blk + 1][0] == j_blk) {
      for (const std::string& ft_s : out.rpo_blocks[j_blk + 1].instructions) {
        auto ftp = ParsePatternAndNotes(ft_s);
        if (!ftp.ok() || IsLoopHeaderNote(ftp->pattern) || ftp->pattern.op == RtlOp::kUse ||
            ftp->pattern.op == RtlOp::kClobber) {
          continue;
        }
        if (ftp->pattern.IsSet() && ftp->pattern.SetDest().IsReg() &&
            !ExprHasMemOrVolatile(ftp->pattern)) {
          const int ft_dst = static_cast<int>(ftp->pattern.SetDest().int_val);
          if (ft_dst > 0 && !is_live_at_restored_target(ft_dst)) {
            ft_has_competing_candidate = true;
          }
        }
        break;
      }
      if (!ft_has_competing_candidate && !out.rpo_blocks[j_blk + 1].instructions.empty()) {
        auto ft_back = ParsePatternAndNotes(out.rpo_blocks[j_blk + 1].instructions.back());
        if (ft_back.ok() && ft_back->pattern.op == RtlOp::kSequence &&
            ft_back->pattern.children.size() == 2 &&
            ft_back->pattern.children[0].op == RtlOp::kJumpInsn &&
            !ft_back->pattern.children[0].children.empty() &&
            (IsSimpleUncondJumpExpr(ft_back->pattern.children[0].children[0]) ||
             IsReturnExpr(ft_back->pattern.children[0].children[0])) &&
            !ft_back->pattern.children[1].flags.in_struct &&
            !ft_back->pattern.children[1].children.empty()) {
          const RtlExpr& ft_slot_pat = ft_back->pattern.children[1].children[0];
          if (ft_slot_pat.IsSet() && ft_slot_pat.SetDest().IsReg() &&
              !ExprHasMemOrVolatile(ft_slot_pat)) {
            const int ft_dst = static_cast<int>(ft_slot_pat.SetDest().int_val);
            if (ft_dst > 0 && !is_live_at_restored_target(ft_dst)) {
              bool clobbered_or_used_earlier = false;
              for (size_t fi = 0; fi + 1 < out.rpo_blocks[j_blk + 1].instructions.size(); ++fi) {
                auto fp = ParsePatternAndNotes(out.rpo_blocks[j_blk + 1].instructions[fi]);
                if (!fp.ok() || !IsRealActiveInsn(fp->pattern)) {
                  continue;
                }
                std::vector<int> fdefs;
                CollectDefinedRegs(fp->pattern, &fdefs);
                if (std::find(fdefs.begin(), fdefs.end(), ft_dst) != fdefs.end() ||
                    ExprReferencesRegInSrcOrMem(fp->pattern, ft_dst)) {
                  clobbered_or_used_earlier = true;
                  break;
                }
              }
              if (!clobbered_or_used_earlier) {
                ft_has_competing_candidate = true;
              }
            }
          }
        }
      }
    }
    if (ft_has_competing_candidate) {
      any_jumper_needs_loop_pred = true;
      break;
    }
  }
  if (!any_jumper_needs_loop_pred) {
    return false;
  }
  for (size_t p_blk : preds[target_b]) {
    if (p_blk < target_b &&
        std::find(same_slot_jumpers.begin(), same_slot_jumpers.end(), p_blk) ==
            same_slot_jumpers.end() &&
        !out.rpo_blocks[p_blk].instructions.empty()) {
      auto p_last = ParsePatternAndNotes(out.rpo_blocks[p_blk].instructions.back());
      if (p_last.ok() && p_last->pattern.op == RtlOp::kSequence &&
          p_last->pattern.children.size() == 2 &&
          p_last->pattern.children[0].op == RtlOp::kJumpInsn &&
          !p_last->pattern.children[0].children.empty() &&
          IsCondJumpExpr(p_last->pattern.children[0].children[0]) &&
          !p_last->pattern.children[1].flags.in_struct) {
        return false;
      }
    }
  }
  return true;
}

void TryRecoverMissingFallthroughCompensation(
    RtlControlFlowGraph* out_ptr, std::vector<bool>* entered_with_filled_end_jump_ptr,
    std::vector<EagerStolenSlotSite>* stolen_sites, size_t* rev_idx_ptr, size_t* b_ptr,
    size_t* target_b_ptr, std::vector<size_t>* same_slot_jumpers_ptr,
    std::vector<std::vector<size_t>>* preds_ptr, bool* pred_falls_through_to_target_ptr,
    bool* need_loop_beg_ptr, const RtlExpr& orig_slot_pat) {
  if (!orig_slot_pat.IsSet() || !orig_slot_pat.SetDest().IsReg()) {
    return;
  }
  RtlControlFlowGraph& out = *out_ptr;
  std::vector<bool>& entered_with_filled_end_jump = *entered_with_filled_end_jump_ptr;
  size_t& rev_idx = *rev_idx_ptr;
  size_t& b = *b_ptr;
  size_t& target_b = *target_b_ptr;
  std::vector<size_t>& same_slot_jumpers = *same_slot_jumpers_ptr;
  std::vector<std::vector<size_t>>& preds = *preds_ptr;
  bool& pred_falls_through_to_target = *pred_falls_through_to_target_ptr;
  bool& need_loop_beg = *need_loop_beg_ptr;

  const RtlExpr& dst_e = orig_slot_pat.SetDest();
  const RtlExpr& src_e = orig_slot_pat.SetSrc();
  const int dst_r = static_cast<int>(dst_e.int_val);
  if (dst_r <= 0 || (src_e.op != RtlOp::kPlus && src_e.op != RtlOp::kMinus) ||
      src_e.children.size() != 2 || !EqualIgnoringRegFlags(src_e.children[0], dst_e) ||
      ExprReferencesRegInSrcOrMem(src_e.children[1], dst_r)) {
    return;
  }
  RtlExpr comp_src = src_e;
  if (src_e.children[1].op == RtlOp::kConstInt) {
    comp_src.children[1].int_val = -src_e.children[1].int_val;
  } else {
    comp_src.op = (src_e.op == RtlOp::kPlus) ? RtlOp::kMinus : RtlOp::kPlus;
  }
  const RtlExpr comp_pat = RtlExpr::MakeSet(dst_e, std::move(comp_src));
  for (size_t j_idx = 0; j_idx < same_slot_jumpers.size(); ++j_idx) {
    const size_t p_blk = same_slot_jumpers[j_idx];
    const size_t ft_b = p_blk + 1;
    if (ft_b >= out.rpo_blocks.size() || out.rpo_blocks[ft_b].has_label ||
        preds[ft_b].size() != 1 || preds[ft_b][0] != p_blk ||
        out.rpo_blocks[p_blk].instructions.empty()) {
      continue;
    }
    auto p_last = ParsePatternAndNotes(out.rpo_blocks[p_blk].instructions.back());
    if (!p_last.ok() || p_last->pattern.op != RtlOp::kSequence ||
        p_last->pattern.children.size() != 2 || p_last->pattern.children[0].flags.unchanging) {
      continue;
    }
    auto p_jump = RtlPatternAndNotes::FromSequenceSubInsn(p_last->pattern.children[0]);
    if (!p_jump.ok() || !IsCondJumpExpr(p_jump->pattern)) {
      continue;
    }
    auto& ft_blk = out.rpo_blocks[ft_b];
    ptrdiff_t comp_idx = -1;
    bool earlier_use_of_dst = false;
    for (size_t k = 0; k < ft_blk.instructions.size(); ++k) {
      auto kp = ParsePatternAndNotes(ft_blk.instructions[k]);
      if (!kp.ok() || IsLoopHeaderNote(kp->pattern) || kp->pattern.op == RtlOp::kClobber) {
        continue;
      }
      if (kp->pattern.op == RtlOp::kUse) {
        if (dst_r != 31 && ExprReferencesRegInSrcOrMem(kp->pattern, dst_r)) {
          earlier_use_of_dst = true;
          break;
        }
        continue;
      }
      if (IsRealActiveInsn(kp->pattern)) {
        if (EqualIgnoringRegFlags(kp->pattern, comp_pat)) {
          comp_idx = static_cast<ptrdiff_t>(k);
        }
        break;
      }
    }
    if (earlier_use_of_dst || comp_idx < 0) {
      continue;
    }
    bool live_after = false;
    bool killed_in_ft = false;
    auto is_call_clobbered_reg = [](int r) -> bool {
      return r == 1 || (r >= 2 && r <= 15) || r == 24 || r == 25 || r == 31 || (r >= 32 && r <= 51);
    };
    for (size_t k = static_cast<size_t>(comp_idx) + 1; k < ft_blk.instructions.size(); ++k) {
      auto qp = ParsePatternAndNotes(ft_blk.instructions[k]);
      if (!qp.ok() || IsLoopHeaderNote(qp->pattern) || qp->pattern.op == RtlOp::kClobber) {
        continue;
      }
      if (qp->pattern.op == RtlOp::kUse) {
        if (dst_r != 31 && ExprReferencesRegInSrcOrMem(qp->pattern, dst_r)) {
          live_after = true;
          break;
        }
        continue;
      }
      if (qp->pattern.op == RtlOp::kSequence && qp->pattern.children.size() == 2) {
        const RtlExpr& s0 = qp->pattern.children[0];
        const RtlExpr& s1 = qp->pattern.children[1];
        auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(s0);
        auto sub1 = RtlPatternAndNotes::FromSequenceSubInsn(s1);
        if (sub0.ok() && ExprReferencesRegInSrcOrMem(sub0->pattern, dst_r)) {
          live_after = true;
          break;
        }
        if (sub1.ok() && ExprReferencesRegInSrcOrMem(sub1->pattern, dst_r)) {
          live_after = true;
          break;
        }
        std::vector<int> sub1_defs;
        if (sub1.ok()) {
          CollectDefinedRegs(sub1->pattern, &sub1_defs);
        }
        const bool killed_by_delay =
            std::find(sub1_defs.begin(), sub1_defs.end(), dst_r) != sub1_defs.end();
        if (s0.op == RtlOp::kCallInsn) {
          if (!killed_by_delay && ((dst_r >= 4 && dst_r <= 7) || (dst_r >= 44 && dst_r <= 47))) {
            live_after = true;
            break;
          }
          if (killed_by_delay || is_call_clobbered_reg(dst_r)) {
            killed_in_ft = true;
            break;
          }
        } else if (!s0.flags.unchanging && killed_by_delay) {
          killed_in_ft = true;
          break;
        }
        continue;
      }
      if (ExprReferencesRegInSrcOrMem(qp->pattern, dst_r)) {
        live_after = true;
        break;
      }
      std::vector<int> qdefs;
      CollectDefinedRegs(qp->pattern, &qdefs);
      if (std::find(qdefs.begin(), qdefs.end(), dst_r) != qdefs.end()) {
        killed_in_ft = true;
        break;
      }
      bool has_call = false;
      qp->pattern.Visit([&](const RtlExpr& e) {
        if (e.op == RtlOp::kCall) {
          has_call = true;
        }
      });
      if (has_call) {
        if ((dst_r >= 4 && dst_r <= 7) || (dst_r >= 44 && dst_r <= 47)) {
          live_after = true;
          break;
        }
        if (is_call_clobbered_reg(dst_r)) {
          killed_in_ft = true;
          break;
        }
      }
    }
    if (!live_after && !killed_in_ft) {
      if (target_b <= p_blk) {
        live_after = true;
      } else if (ft_blk.successors.empty()) {
        if (dst_r == 2 || dst_r == 3 || dst_r == 32 || dst_r == 33) {
          live_after = true;
        }
      } else {
        for (size_t succ_b : ft_blk.successors) {
          if (IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&dst_r, 1), succ_b)) {
            live_after = true;
            break;
          }
        }
      }
    }
    if (!live_after) {
      continue;
    }
    need_loop_beg = true;
    ft_blk.instructions.erase(ft_blk.instructions.begin() + comp_idx);
    if (ft_blk.instructions.empty() && !ft_blk.has_label && ft_blk.successors.size() == 1 &&
        ft_blk.successors[0] == ft_b + 1) {
      for (size_t& succ : out.rpo_blocks[p_blk].successors) {
        if (succ == ft_b) {
          succ = ft_b + 1;
          break;
        }
      }
      EraseBlockAt(&out, ft_b, stolen_sites);
      entered_with_filled_end_jump.erase(entered_with_filled_end_jump.begin() +
                                         static_cast<ptrdiff_t>(ft_b));
      if (ft_b <= b && rev_idx > 0) {
        --rev_idx;
      }
      if (b > ft_b) {
        --b;
      }
      if (target_b > ft_b) {
        --target_b;
      }
      for (size_t& sj : same_slot_jumpers) {
        if (sj > ft_b) {
          --sj;
        }
      }
      preds = BuildPredecessors(out);
      pred_falls_through_to_target =
          target_b > 0 && (target_b - 1 != b) && BlockFallsThroughToNext(out, preds, target_b - 1);
    }
  }
}

bool PrefixInsnsAreRedundantInAllJumpers(const RtlControlFlowGraph& out,
                                         const RtlBasicBlock& pred_blk, size_t move_start,
                                         size_t pred_b,
                                         absl::Span<const size_t> same_slot_jumpers) {
  if (move_start == 0 || same_slot_jumpers.empty()) {
    return false;
  }
  std::vector<RtlExpr> prefix_insns;
  std::vector<int> prefix_defs;
  for (size_t i = 0; i < move_start; ++i) {
    auto p = ParsePatternAndNotes(pred_blk.instructions[i]);
    if (!p.ok() || p->pattern.op == RtlOp::kClobber) {
      return false;
    }
    if (!IsRealActiveInsn(p->pattern)) {
      continue;
    }
    if (!p->pattern.IsSet() || !p->pattern.SetDest().IsReg() || ExprHasMemOrVolatile(p->pattern)) {
      return false;
    }
    std::vector<int> defs;
    CollectDefinedRegs(p->pattern, &defs);
    if (defs.empty()) {
      return false;
    }
    for (int r : defs) {
      if (ExprReferencesRegInSrcOrMem(p->pattern, r)) {
        return false;
      }
    }
    for (int r : prefix_defs) {
      if (SubtreeReferencesHardReg(p->pattern, r)) {
        return false;
      }
    }
    prefix_defs.insert(prefix_defs.end(), defs.begin(), defs.end());
    prefix_insns.push_back(std::move(p->pattern));
  }
  if (prefix_insns.empty()) {
    return false;
  }

  for (size_t p_blk : same_slot_jumpers) {
    if (p_blk >= out.rpo_blocks.size() || p_blk + 1 == pred_b) {
      return false;
    }
    const auto& j_blk = out.rpo_blocks[p_blk];
    if (j_blk.instructions.size() < 2) {
      return false;
    }
    auto j_last = ParsePatternAndNotes(j_blk.instructions.back());
    if (!j_last.ok()) {
      return false;
    }
    std::vector<int> jump_defs;
    CollectDefinedRegs(j_last->pattern, &jump_defs);

    if (j_blk.successors.size() == 2 && p_blk + 1 < out.rpo_blocks.size()) {
      for (const std::string& fs : out.rpo_blocks[p_blk + 1].instructions) {
        auto fp = ParsePatternAndNotes(fs);
        if (!fp.ok() || !IsRealActiveInsn(fp->pattern)) {
          continue;
        }
        if (!IsAnyJumpOrReturnTopExpr(fp->pattern) && fp->pattern.op != RtlOp::kSequence) {
          std::vector<int> ft_defs;
          CollectDefinedRegs(fp->pattern, &ft_defs);
          for (int r : ft_defs) {
            if (std::find(prefix_defs.begin(), prefix_defs.end(), r) != prefix_defs.end()) {
              return false;
            }
          }
        }
        break;
      }
    }

    for (const RtlExpr& pref_pat : prefix_insns) {
      for (int r : jump_defs) {
        if (SubtreeReferencesHardReg(pref_pat, r)) {
          return false;
        }
      }
      bool matched = false;
      for (size_t idx = j_blk.instructions.size() - 1; idx > 0; --idx) {
        auto ip = ParsePatternAndNotes(j_blk.instructions[idx - 1]);
        if (!ip.ok()) {
          return false;
        }
        if (ip->pattern.op == RtlOp::kNote || ip->pattern.op == RtlOp::kUse) {
          continue;
        }
        if (ip->pattern.op == RtlOp::kSequence || IsAnyJumpOrReturnTopExpr(ip->pattern)) {
          break;
        }
        bool has_call = false;
        ip->pattern.Visit([&](const RtlExpr& e) {
          if (e.op == RtlOp::kCall) {
            has_call = true;
          }
        });
        if (has_call) {
          break;
        }
        if (EqualIgnoringRegFlags(ip->pattern, pref_pat)) {
          matched = true;
          break;
        }
        std::vector<int> idefs;
        CollectDefinedRegs(ip->pattern, &idefs);
        bool clobbers = false;
        for (int r : idefs) {
          if (SubtreeReferencesHardReg(pref_pat, r)) {
            clobbers = true;
            break;
          }
        }
        if (clobbers) {
          break;
        }
      }
      if (!matched) {
        return false;
      }
    }
  }
  return true;
}

void ApplyPostEagerDelaySlotNormalizations(RtlControlFlowGraph* out_ptr,
                                           std::vector<EagerStolenSlotSite>* stolen_sites,
                                           const CompilationProfile& profile) {
  RtlControlFlowGraph& out = *out_ptr;
  for (auto& blk : out.rpo_blocks) {
    for (size_t i = 0; i + 1 < blk.instructions.size(); ++i) {
      auto p0 = ParsePatternAndNotes(blk.instructions[i]);
      auto p1 = ParsePatternAndNotes(blk.instructions[i + 1]);
      if (!p0.ok() || !p1.ok() || !p0->pattern.IsSet() || !p1->pattern.IsSet()) {
        continue;
      }
      const RtlExpr& d0 = p0->pattern.SetDest();
      const RtlExpr& s0 = p0->pattern.SetSrc();
      const RtlExpr& d1 = p1->pattern.SetDest();
      const RtlExpr& s1 = p1->pattern.SetSrc();
      if (!d0.IsSubreg() || !s0.IsSubreg() || !d1.IsSubreg() || !s1.IsSubreg() ||
          d0.mode != RtlMachineMode::kSi || s0.mode != RtlMachineMode::kSi ||
          d1.mode != RtlMachineMode::kSi || s1.mode != RtlMachineMode::kSi || d0.children.empty() ||
          s0.children.empty() || d1.children.empty() || s1.children.empty() ||
          !d0.children[0].IsReg() || !s0.children[0].IsReg() || !d1.children[0].IsReg() ||
          !s1.children[0].IsReg() || d0.children[0].mode != RtlMachineMode::kDi ||
          s0.children[0].mode != RtlMachineMode::kDi ||
          d1.children[0].mode != RtlMachineMode::kDi ||
          s1.children[0].mode != RtlMachineMode::kDi) {
        continue;
      }
      const int dst_r = d0.children[0].RegNo();
      const int src_r = s0.children[0].RegNo();
      if (dst_r != d1.children[0].RegNo() || src_r != s1.children[0].RegNo() || dst_r == src_r) {
        continue;
      }
      const bool matched_subreg_pair =
          (d0.int_val == 0 && s0.int_val == 0 && d1.int_val == 1 && s1.int_val == 1) ||
          (d0.int_val == 1 && s0.int_val == 1 && d1.int_val == 0 && s1.int_val == 0);
      if (!matched_subreg_pair) {
        continue;
      }
      RtlPatternAndNotes combined;
      combined.pattern = RtlExpr::MakeSet(d0.children[0], s0.children[0]);
      for (const RtlRegNote& n : p0->reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() && n.datum.RegNo() == src_r) {
          combined.reg_notes.push_back(n);
        }
      }
      for (const RtlRegNote& n : p1->reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() && n.datum.RegNo() == src_r &&
            combined.reg_notes.empty()) {
          combined.reg_notes.push_back(n);
        }
      }
      blk.instructions[i] = combined.ToGccPrintRtlString();
      blk.instructions.erase(blk.instructions.begin() + static_cast<ptrdiff_t>(i + 1));
    }
  }

  const auto final_preds = BuildPredecessors(out);
  // Restore redundant pure register `SET`s deleted from an unfilled intermediate
  // conditional-branch block by `reorg.c:3701-3704` (`redundant_insn` during
  // `steal_delay_list_from_target`).
  for (size_t b = 0; b + 2 < out.rpo_blocks.size(); ++b) {
    const auto& blk_b = out.rpo_blocks[b];
    if (blk_b.successors.size() != 2 || blk_b.instructions.size() < 2) {
      continue;
    }
    const size_t mid_b = (blk_b.successors[0] == b + 1) ? blk_b.successors[1] : blk_b.successors[0];
    if (mid_b == b + 1 || mid_b >= out.rpo_blocks.size() || final_preds[mid_b].size() != 1 ||
        final_preds[mid_b][0] != b) {
      continue;
    }
    auto b_jmp = ParsePatternAndNotes(blk_b.instructions.back());
    auto b_prev = ParsePatternAndNotes(blk_b.instructions[blk_b.instructions.size() - 2]);
    if (!b_jmp.ok() || !b_prev.ok() || b_jmp->pattern.op == RtlOp::kSequence ||
        !IsCondJumpExpr(b_jmp->pattern) || !b_prev->pattern.IsSet() ||
        !b_prev->pattern.SetDest().IsReg() || ExprHasMemOrVolatile(b_prev->pattern)) {
      continue;
    }
    const int dst_r = b_prev->pattern.SetDest().RegNo();
    if (dst_r <= 0 || dst_r >= 32) {
      continue;
    }
    auto& mid_blk = out.rpo_blocks[mid_b];
    if (mid_blk.successors.size() != 2 || mid_b + 1 >= out.rpo_blocks.size()) {
      continue;
    }
    const size_t nxt_b =
        (mid_blk.successors[0] == mid_b + 1) ? mid_blk.successors[1] : mid_blk.successors[0];
    if (nxt_b == mid_b + 1 || nxt_b >= out.rpo_blocks.size() || final_preds[nxt_b].size() != 1 ||
        final_preds[nxt_b][0] != mid_b) {
      continue;
    }
    int mid_active = 0;
    RtlExpr mid_sole_pat;
    for (const std::string& ms : mid_blk.instructions) {
      auto mp = ParsePatternAndNotes(ms);
      if (mp.ok() && IsRealActiveInsn(mp->pattern)) {
        ++mid_active;
        mid_sole_pat = mp->pattern;
      }
    }
    if (mid_active != 1 || mid_sole_pat.op == RtlOp::kSequence || !IsCondJumpExpr(mid_sole_pat) ||
        !mid_sole_pat.ContainsReg(dst_r)) {
      continue;
    }
    if (IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&dst_r, 1), b + 1) ||
        IsAnyRegLiveAtBlockEntry(out, absl::MakeConstSpan(&dst_r, 1), mid_b + 1)) {
      continue;
    }
    RtlExpr nxt_first_pat;
    bool has_nxt_first = false;
    for (const std::string& ns : out.rpo_blocks[nxt_b].instructions) {
      auto np = ParsePatternAndNotes(ns);
      if (np.ok() && IsRealActiveInsn(np->pattern)) {
        nxt_first_pat = np->pattern;
        has_nxt_first = true;
        break;
      }
    }
    if (!has_nxt_first || !(nxt_first_pat == b_prev->pattern)) {
      continue;
    }
    RtlPatternAndNotes dup_set;
    dup_set.pattern = b_prev->pattern;
    size_t ins_pos = 0;
    while (ins_pos < mid_blk.instructions.size()) {
      auto mp = ParsePatternAndNotes(mid_blk.instructions[ins_pos]);
      if (mp.ok() && IsLoopHeaderNote(mp->pattern)) {
        ++ins_pos;
      } else {
        break;
      }
    }
    mid_blk.instructions.insert(mid_blk.instructions.begin() + static_cast<ptrdiff_t>(ins_pos),
                                dup_set.ToGccPrintRtlString());
    if (profile.optimize_level > 0 && !dup_set.pattern.SetSrc().ContainsReg(dst_r) &&
        b_jmp->pattern.ContainsReg(dst_r)) {
      const bool has_dead =
          std::any_of(b_jmp->reg_notes.begin(), b_jmp->reg_notes.end(), [&](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                   !n.datum.IsArgPointerReg() && n.datum.RegNo() == dst_r;
          });
      if (!has_dead) {
        RtlExpr read_expr = b_prev->pattern.SetDest();
        read_expr.flags = RtlFlags{};
        b_jmp->pattern.Visit([&](const RtlExpr& e) {
          if (e.IsReg() && !e.IsArgPointerReg() && e.RegNo() == dst_r) {
            read_expr = e;
          }
        });
        b_jmp->reg_notes.insert(b_jmp->reg_notes.begin(), RtlRegNote{
                                                              .kind = RtlMachineMode::kRegDead,
                                                              .list_op = RtlOp::kExprList,
                                                              .datum = std::move(read_expr),
                                                          });
        out.rpo_blocks[b].instructions.back() = b_jmp->ToGccPrintRtlString();
      }
    }
  }

  for (size_t ret_b = 1; ret_b < out.rpo_blocks.size(); ++ret_b) {
    auto& ret_blk = out.rpo_blocks[ret_b];
    if (!ret_blk.has_label || !ret_blk.successors.empty() || ret_blk.instructions.size() != 1 ||
        (final_preds[ret_b].size() < 2 && ret_b + 1 == out.rpo_blocks.size())) {
      continue;
    }
    auto ret_p = ParsePatternAndNotes(ret_blk.instructions[0]);
    if (!ret_p.ok() || !IsReturnExpr(ret_p->pattern)) {
      continue;
    }
    auto& prev_blk = out.rpo_blocks[ret_b - 1];
    if (!BlockFallsThroughToNext(out, final_preds, ret_b - 1) || prev_blk.instructions.size() < 2) {
      continue;
    }
    auto prev_last = ParsePatternAndNotes(prev_blk.instructions.back());
    if (!prev_last.ok() || prev_last->pattern.op != RtlOp::kUse ||
        prev_last->pattern.children.empty() || !prev_last->pattern.children[0].IsReg() ||
        !prev_last->pattern.children[0].flags.integrated) {
      continue;
    }
    ret_blk.instructions.insert(ret_blk.instructions.begin(), prev_blk.instructions.back());
    prev_blk.instructions.pop_back();
  }

  for (size_t b = 0; b + 1 < out.rpo_blocks.size();) {
    const auto& blk = out.rpo_blocks[b];
    if (blk.instructions.empty() && blk.addr_vec_targets.empty() && blk.successors.size() == 1 &&
        blk.successors[0] == b + 1) {
      if (blk.has_label) {
        out.rpo_blocks[b + 1].has_label = true;
      }
      for (auto& other : out.rpo_blocks) {
        for (size_t& succ : other.successors) {
          if (succ == b) {
            succ = b + 1;
          }
        }
        for (size_t& target : other.addr_vec_targets) {
          if (target == b) {
            target = b + 1;
          }
        }
      }
      EraseBlockAt(&out, b, stolen_sites);
      continue;
    }
    ++b;
  }
}

bool IsOptimizeSkipForm2ToUseOrClobberTarget(const RtlControlFlowGraph& out,
                                             const std::vector<std::vector<size_t>>& preds,
                                             size_t b, size_t target_b) {
  if (b + 1 >= out.rpo_blocks.size() || target_b >= out.rpo_blocks.size() ||
      preds[target_b].size() <= 1) {
    return false;
  }
  const auto& ft_blk = out.rpo_blocks[b + 1];
  const auto& tgt_blk = out.rpo_blocks[target_b];
  bool tgt_starts_with_use_or_clobber_before_active = false;
  bool saw_leading_use_or_clobber = false;
  for (const std::string& t_insn : tgt_blk.instructions) {
    auto tp = ParsePatternAndNotes(t_insn);
    if (!tp.ok() || IsLoopHeaderNote(tp->pattern)) {
      continue;
    }
    if (tp->pattern.op == RtlOp::kUse || tp->pattern.op == RtlOp::kClobber) {
      saw_leading_use_or_clobber = true;
      continue;
    }
    if (IsRealActiveInsn(tp->pattern)) {
      tgt_starts_with_use_or_clobber_before_active = saw_leading_use_or_clobber;
    }
    break;
  }
  if (!tgt_starts_with_use_or_clobber_before_active || ft_blk.has_label ||
      preds[b + 1].size() != 1 || preds[b + 1][0] != b || ft_blk.instructions.size() != 1) {
    return false;
  }
  auto ft_parsed = ParsePatternAndNotes(ft_blk.instructions[0]);
  return ft_parsed.ok() && ((IsSimpleUncondJumpExpr(ft_parsed->pattern) &&
                             ft_blk.successors.size() == 1 && ft_blk.successors[0] == target_b) ||
                            (IsReturnExpr(ft_parsed->pattern) && ft_blk.successors.empty()));
}

bool ShouldPlaceFallthroughTrampBeforeFinalUse(const RtlControlFlowGraph& out, size_t target_b,
                                               bool merged_onto_use_blk,
                                               bool pred_falls_through_to_target,
                                               bool has_other_jumpers) {
  if (merged_onto_use_blk || !pred_falls_through_to_target ||
      !BlockStartsWithUseOrClobberInsn(out.rpo_blocks[target_b])) {
    return false;
  }
  return (target_b + 1 == out.rpo_blocks.size()) && !has_other_jumpers;
}

void CollectSameSlotJumpersToTarget(const RtlControlFlowGraph& out,
                                    const std::vector<std::vector<size_t>>& preds, size_t b,
                                    size_t target_b, bool pred_falls_through_to_target,
                                    const RtlExpr& orig_slot_pat,
                                    const std::vector<bool>& entered_with_filled_end_jump,
                                    std::vector<size_t>* same_slot_jumpers,
                                    bool* has_other_jumpers) {
  same_slot_jumpers->clear();
  *has_other_jumpers = false;
  for (size_t p_blk : preds[target_b]) {
    if (p_blk == target_b - 1 && p_blk != b) {
      const auto& p_succs = out.rpo_blocks[p_blk].successors;
      bool ends_with_jump = false;
      if (!out.rpo_blocks[p_blk].instructions.empty()) {
        auto p_last = ParsePatternAndNotes(out.rpo_blocks[p_blk].instructions.back());
        if (p_last.ok()) {
          ends_with_jump = IsAnyJumpOrReturnTopExpr(p_last->pattern);
        }
      }
      if (!ends_with_jump ||
          (pred_falls_through_to_target && p_succs.size() == 1 && p_succs[0] == target_b)) {
        continue;
      }
    }
    if (p_blk == b) {
      same_slot_jumpers->push_back(b);
      continue;
    }
    const auto& other_blk = out.rpo_blocks[p_blk];
    bool matched_same_slot = false;
    if (!other_blk.instructions.empty() &&
        (other_blk.successors.size() != 2 || other_blk.successors[0] != other_blk.successors[1])) {
      auto other_last = ParsePatternAndNotes(other_blk.instructions.back());
      if (other_last.ok() && other_last->pattern.op == RtlOp::kSequence &&
          other_last->pattern.children.size() == 2 &&
          other_last->pattern.children[0].op == RtlOp::kJumpInsn &&
          other_last->pattern.children[1].op == RtlOp::kInsn) {
        auto other_jump = RtlPatternAndNotes::FromSequenceSubInsn(other_last->pattern.children[0]);
        auto other_slot = RtlPatternAndNotes::FromSequenceSubInsn(other_last->pattern.children[1]);
        if (other_jump.ok() && other_slot.ok() &&
            !IsOptimizeSkipSequenceBlock(out, p_blk, other_last->pattern.children[1],
                                         IsCondJumpExpr(other_jump->pattern))) {
          const bool other_is_cond = IsCondJumpExpr(other_jump->pattern);
          const bool other_is_uncond = IsSimpleUncondJumpExpr(other_jump->pattern);
          std::vector<int> other_slot_defs;
          CollectDefinedRegs(other_slot->pattern, &other_slot_defs);
          bool other_jump_reads_slot_def = false;
          for (int r : other_slot_defs) {
            if (ExprReferencesRegInSrcOrMem(other_jump->pattern, r)) {
              other_jump_reads_slot_def = true;
              break;
            }
          }
          size_t other_target =
              other_blk.successors.empty() ? out.rpo_blocks.size() : other_blk.successors[0];
          if (other_is_cond && other_blk.successors.size() == 2) {
            other_target = (other_blk.successors[0] == p_blk + 1) ? other_blk.successors[1]
                                                                  : other_blk.successors[0];
          }
          const bool other_from_target =
              other_last->pattern.children[1].flags.in_struct ||
              (other_is_uncond &&
               IsUncondTargetStealAfterCond(out, p_blk, preds, entered_with_filled_end_jump)) ||
              (other_is_cond && !other_last->pattern.children[0].flags.unchanging &&
               other_blk.successors.size() == 2 && p_blk + 1 < out.rpo_blocks.size() &&
               FindOppositeArithCompensationInFallthroughBlock(out, preds, p_blk, other_target,
                                                               other_slot->pattern) >= 0) ||
              (other_is_cond && !other_last->pattern.children[0].flags.unchanging &&
               other_jump_reads_slot_def && other_blk.successors.size() == 2 &&
               p_blk + 1 < out.rpo_blocks.size() &&
               !IsAnyRegLiveAtBlockEntry(out, other_slot_defs, p_blk + 1) &&
               (IsSlotKilledOnFallthrough(out, other_slot->pattern, p_blk + 1) ||
                (other_target < out.rpo_blocks.size() &&
                 IsAnyRegLiveAtBlockEntry(out, other_slot_defs, other_target))));
          if (other_from_target && (other_is_cond || other_is_uncond) && other_target == target_b &&
              EqualIgnoringRegFlags(other_slot->pattern, orig_slot_pat)) {
            matched_same_slot = true;
          }
        }
      }
    }
    if (matched_same_slot) {
      same_slot_jumpers->push_back(p_blk);
    } else {
      *has_other_jumpers = true;
    }
  }
}

void RestoreSubwordConstSlotModeIfNeeded(const RtlControlFlowGraph& out,
                                         const std::vector<std::vector<size_t>>& preds, size_t b,
                                         size_t target_b, const RtlExpr& orig_slot_pat,
                                         RtlPatternAndNotes* slot_pat_notes) {
  if (!slot_pat_notes->pattern.IsSet() || !slot_pat_notes->pattern.SetDest().IsReg() ||
      slot_pat_notes->pattern.SetDest().mode != RtlMachineMode::kSi) {
    return;
  }
  const int dst_r = static_cast<int>(slot_pat_notes->pattern.SetDest().int_val);
  const RtlExpr& src_e = slot_pat_notes->pattern.SetSrc();
  std::optional<int64_t> const_val;
  if (src_e.op == RtlOp::kConstInt) {
    const_val = src_e.int_val;
  } else if (src_e.op == RtlOp::kPlus && src_e.children.size() == 2 && src_e.children[0].IsReg() &&
             src_e.children[0].int_val == 0 && src_e.children[1].IsReg() &&
             src_e.children[1].int_val == 0) {
    const_val = 0;
  }
  if (!const_val.has_value() || dst_r <= 0) {
    return;
  }
  const auto& blk = out.rpo_blocks[b];
  bool prior_same_si_set = false;
  bool dst_clobbered_in_b = false;
  for (size_t idx = blk.instructions.size() - 1; idx > 0; --idx) {
    auto bp = ParsePatternAndNotes(blk.instructions[idx - 1]);
    if (!bp.ok() || !IsRealActiveInsn(bp->pattern)) {
      continue;
    }
    if (bp->pattern == orig_slot_pat) {
      prior_same_si_set = true;
      break;
    }
    std::vector<int> bdefs;
    CollectDefinedRegs(bp->pattern, &bdefs);
    if (std::find(bdefs.begin(), bdefs.end(), dst_r) != bdefs.end()) {
      dst_clobbered_in_b = true;
      break;
    }
  }
  if (!prior_same_si_set && !dst_clobbered_in_b && b > 0 && !blk.has_label &&
      preds[b].size() == 1 && preds[b][0] == b - 1 && !out.rpo_blocks[b - 1].instructions.empty()) {
    auto prev_p = ParsePatternAndNotes(out.rpo_blocks[b - 1].instructions.back());
    if (prev_p.ok() && prev_p->pattern.op == RtlOp::kSequence &&
        prev_p->pattern.children.size() == 2 && !prev_p->pattern.children[0].flags.unchanging) {
      auto prev_s = RtlPatternAndNotes::FromSequenceSubInsn(prev_p->pattern.children[1]);
      if (prev_s.ok() && prev_s->pattern == orig_slot_pat) {
        prior_same_si_set = true;
      }
    }
  }
  if (!prior_same_si_set) {
    return;
  }
  for (const std::string& ts : out.rpo_blocks[target_b].instructions) {
    auto tp = ParsePatternAndNotes(ts);
    if (!tp.ok() || !IsRealActiveInsn(tp->pattern)) {
      continue;
    }
    bool has_qi = false;
    bool has_hi = false;
    bool has_other_mode = false;
    auto scan_src_reg_modes = [&](const RtlExpr& root) {
      auto visit_sub = [&](const RtlExpr& sub) {
        sub.Visit([&](const RtlExpr& n) {
          if (n.IsReg() && static_cast<int>(n.int_val) == dst_r) {
            if (n.mode == RtlMachineMode::kQI) {
              has_qi = true;
            } else if (n.mode == RtlMachineMode::kHI) {
              has_hi = true;
            } else {
              has_other_mode = true;
            }
          }
        });
      };
      if (root.IsSet()) {
        if (root.SetDest().op == RtlOp::kMem) {
          visit_sub(root.SetDest());
        }
        visit_sub(root.SetSrc());
      } else {
        visit_sub(root);
      }
    };
    scan_src_reg_modes(tp->pattern);
    if (!has_other_mode && has_qi && !has_hi && *const_val >= -128 && *const_val <= 255) {
      RtlExpr dst_reg =
          RtlExpr::MakeReg(RtlMachineMode::kQI, dst_r, slot_pat_notes->pattern.SetDest().flags);
      if (dst_r == 30) {
        dst_reg.str_val = "$fp";
      }
      slot_pat_notes->pattern =
          RtlExpr::MakeSet(std::move(dst_reg), RtlExpr::MakeConstInt(*const_val));
    } else if (!has_other_mode && has_hi && !has_qi && *const_val >= -32768 &&
               *const_val <= 65535) {
      RtlExpr dst_reg =
          RtlExpr::MakeReg(RtlMachineMode::kHI, dst_r, slot_pat_notes->pattern.SetDest().flags);
      if (dst_r == 30) {
        dst_reg.str_val = "$fp";
      }
      slot_pat_notes->pattern =
          RtlExpr::MakeSet(std::move(dst_reg), RtlExpr::MakeConstInt(*const_val));
    }
    break;
  }
}

void PrependLoopVtopAndBegIfNeeded(RtlControlFlowGraph* out,
                                   std::vector<bool>* entered_with_filled_end_jump,
                                   std::vector<EagerStolenSlotSite>* stolen_sites, size_t hdr_b,
                                   bool need_loop_vtop, bool need_loop_beg, size_t b,
                                   size_t* rev_idx) {
  if (hdr_b >= out->rpo_blocks.size()) {
    return;
  }
  if (need_loop_vtop) {
    auto& hdr_blk = out->rpo_blocks[hdr_b];
    bool already_vtop = false;
    for (const std::string& s : hdr_blk.instructions) {
      auto hp = ParsePatternAndNotes(s);
      if (hp.ok() && IsLoopHeaderNote(hp->pattern)) {
        already_vtop = true;
        break;
      }
    }
    if (!already_vtop && hdr_b > 0 && !out->rpo_blocks[hdr_b - 1].instructions.empty()) {
      auto prev_p = ParsePatternAndNotes(out->rpo_blocks[hdr_b - 1].instructions.back());
      if (prev_p.ok() && IsLoopHeaderNote(prev_p->pattern)) {
        already_vtop = true;
      }
    }
    if (!already_vtop) {
      hdr_blk.instructions.insert(hdr_blk.instructions.begin(),
                                  RtlExpr::MakeNote("NOTE_INSN_LOOP_VTOP").ToGccPrintRtlString());
    }
  }
  if (!need_loop_beg) {
    return;
  }
  bool already_beg = false;
  bool prev_emits_jump = false;
  if (hdr_b > 0) {
    const auto& prev_blk = out->rpo_blocks[hdr_b - 1];
    if (!prev_blk.instructions.empty()) {
      auto pl = ParsePatternAndNotes(prev_blk.instructions.back());
      prev_emits_jump = pl.ok() && IsAnyJumpOrReturnTopExpr(pl.value().pattern);
    }
    if (!prev_emits_jump && (prev_blk.successors.size() != 1 || prev_blk.successors[0] == hdr_b)) {
      for (auto it = prev_blk.instructions.rbegin(); it != prev_blk.instructions.rend(); ++it) {
        auto pp = ParsePatternAndNotes(*it);
        if (!pp.ok() || pp->pattern.op != RtlOp::kNote) {
          break;
        }
        for (const auto& item : pp->pattern.generic_items) {
          if (item.text == "NOTE_INSN_LOOP_BEG") {
            already_beg = true;
          }
        }
      }
    }
  }
  if (already_beg) {
    return;
  }
  if (hdr_b > 0 && !prev_emits_jump && out->rpo_blocks[hdr_b - 1].successors.size() == 1 &&
      out->rpo_blocks[hdr_b - 1].successors[0] == hdr_b) {
    out->rpo_blocks[hdr_b - 1].instructions.push_back(
        RtlExpr::MakeNote("NOTE_INSN_LOOP_BEG").ToGccPrintRtlString());
    return;
  }
  bool prev_fell_through = false;
  size_t prev_ft_slot = 0;
  if (hdr_b > 0) {
    const auto& prev_blk = out->rpo_blocks[hdr_b - 1];
    bool prev_uncond = false;
    if (!prev_blk.instructions.empty()) {
      auto pl = ParsePatternAndNotes(prev_blk.instructions.back());
      prev_uncond = pl.ok() && IsUncondJumpOrReturnOrTableJumpTopExpr(pl->pattern);
    }
    if (!prev_uncond) {
      for (size_t si = 0; si < prev_blk.successors.size(); ++si) {
        if (prev_blk.successors[si] == hdr_b) {
          prev_fell_through = true;
          prev_ft_slot = si;
          break;
        }
      }
    }
  }
  out->rpo_blocks[hdr_b].has_label = true;
  RtlBasicBlock beg_blk;
  beg_blk.has_label = false;
  beg_blk.instructions = {RtlExpr::MakeNote("NOTE_INSN_LOOP_BEG").ToGccPrintRtlString()};
  beg_blk.successors = {hdr_b + 1};
  InsertBlockAt(out, hdr_b, std::move(beg_blk), stolen_sites);
  entered_with_filled_end_jump->insert(
      entered_with_filled_end_jump->begin() + static_cast<ptrdiff_t>(hdr_b), false);
  if (prev_fell_through) {
    out->rpo_blocks[hdr_b - 1].successors[prev_ft_slot] = hdr_b;
  }
  if (b >= hdr_b) {
    ++(*rev_idx);
  }
}

size_t ComputeTargetStolenInsertPos(const RtlControlFlowGraph& out, size_t tgt_b,
                                    const RtlBasicBlock& tgt_blk, const RtlPatternAndNotes& slot_pn,
                                    const CompilationProfile& profile) {
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (!flags.RunsSched2() || ExprHasMemOrVolatile(slot_pn.pattern) || !slot_pn.pattern.IsSet() ||
      !slot_pn.pattern.SetDest().IsReg() || slot_pn.pattern.SetDest().RegNo() >= 32) {
    return 0;
  }
  size_t load_shadow_cap = (flags.mips_isa == 1) ? 1 : 2;
  for (const std::string& tok : flags.raw_tokens) {
    const std::string_view sv = tok;
    if (sv.substr(0, 6) == "-mcpu=") {
      const std::string_view cpu = sv.substr(6);
      load_shadow_cap =
          (cpu == "r3000" || cpu == "R3000" || cpu == "3000" || cpu == "r2000" || cpu == "R2000" ||
           cpu == "2000" || cpu == "r4600" || cpu == "R4600" || cpu == "4600" || cpu == "orion" ||
           cpu == "r4650" || cpu == "R4650" || cpu == "4650")
              ? 1
              : 2;
    }
  }
  bool slot_multi_cycle = false;
  slot_pn.pattern.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kMult || n.op == RtlOp::kDiv || n.op == RtlOp::kUdiv ||
        n.op == RtlOp::kMod || n.op == RtlOp::kUmod ||
        (n.IsReg() && n.RegNo() >= 32 && n.RegNo() <= 66)) {
      slot_multi_cycle = true;
    }
  });
  if (slot_multi_cycle) {
    return 0;
  }
  auto notes_conflict = [](const std::vector<RtlRegNote>& notes, const RtlExpr& expr) -> bool {
    for (const RtlRegNote& note : notes) {
      if (note.datum.IsReg() && expr.ContainsReg(note.datum.RegNo())) {
        return true;
      }
    }
    return false;
  };
  const int slot_dst = slot_pn.pattern.SetDest().RegNo();
  size_t ins_pos = 0;
  while (ins_pos < tgt_blk.instructions.size()) {
    auto lead = ParsePatternAndNotes(tgt_blk.instructions[ins_pos]);
    if (!lead.ok()) {
      break;
    }
    if (lead->pattern.op == RtlOp::kNote) {
      ++ins_pos;
      continue;
    }
    bool lead_has_call = false;
    bool lead_has_mem_read = false;
    if (lead->pattern.IsSet() && lead->pattern.SetDest().IsReg()) {
      lead->pattern.SetSrc().Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kCall) {
          lead_has_call = true;
        }
        if (n.op == RtlOp::kMem) {
          lead_has_mem_read = true;
        }
      });
    }
    if (lead_has_call || !lead_has_mem_read ||
        ExpressionsConflict(lead->pattern, slot_pn.pattern) ||
        notes_conflict(lead->reg_notes, slot_pn.pattern) ||
        notes_conflict(slot_pn.reg_notes, lead->pattern)) {
      break;
    }
    const int load_dst = lead->pattern.SetDest().RegNo();
    std::vector<const std::string*> scan_insns;
    for (size_t k = ins_pos + 1; k < tgt_blk.instructions.size(); ++k) {
      scan_insns.push_back(&tgt_blk.instructions[k]);
    }
    if (tgt_blk.successors.size() == 1) {
      const size_t succ_b = tgt_blk.successors[0];
      if (succ_b > tgt_b && succ_b < out.rpo_blocks.size()) {
        for (const std::string& s : out.rpo_blocks[succ_b].instructions) {
          scan_insns.push_back(&s);
        }
      }
    }
    bool load_consumed_later = false;
    bool slot_consumed_first = false;
    for (size_t k = 0; k < scan_insns.size(); ++k) {
      auto pk = ParsePatternAndNotes(*scan_insns[k]);
      if (!pk.ok()) {
        break;
      }
      if (pk->pattern.ContainsReg(load_dst)) {
        if (load_shadow_cap == 1 && k == 0 && pk->pattern.IsSet() &&
            pk->pattern.SetDest().IsMem() && pk->pattern.ContainsReg(slot_dst) &&
            ins_pos + 2 < tgt_blk.instructions.size()) {
          bool store_has_symbolic_addr = false;
          pk->pattern.SetDest().Visit([&store_has_symbolic_addr](const RtlExpr& n) {
            if (n.op == RtlOp::kSymbolRef || n.op == RtlOp::kLabelRef || n.op == RtlOp::kConst) {
              store_has_symbolic_addr = true;
            }
          });
          if (store_has_symbolic_addr) {
            auto tail_seq = ParsePatternAndNotes(tgt_blk.instructions.back());
            if (tail_seq.ok() && tail_seq->pattern.op == RtlOp::kSequence &&
                tail_seq->pattern.children.size() == 2 &&
                tail_seq->pattern.children[0].op == RtlOp::kJumpInsn) {
              auto j_pn = RtlPatternAndNotes::FromSequenceSubInsn(tail_seq->pattern.children[0]);
              auto s_pn = RtlPatternAndNotes::FromSequenceSubInsn(tail_seq->pattern.children[1]);
              if (j_pn.ok() && s_pn.ok() && j_pn->pattern.IsSet() &&
                  j_pn->pattern.SetDest().op == RtlOp::kPc &&
                  j_pn->pattern.SetSrc().op == RtlOp::kLabelRef &&
                  !ExpressionsConflict(lead->pattern, s_pn->pattern)) {
                bool tail_intervening_ok = true;
                for (size_t ti = ins_pos + 1; ti + 1 < tgt_blk.instructions.size(); ++ti) {
                  auto tp = ParsePatternAndNotes(tgt_blk.instructions[ti]);
                  if (!tp.ok() || ExpressionsConflict(tp->pattern, s_pn->pattern)) {
                    tail_intervening_ok = false;
                    break;
                  }
                }
                if (tail_intervening_ok) {
                  break;
                }
              }
            }
          }
        }
        load_consumed_later = true;
        break;
      }
      if (pk->pattern.ContainsReg(slot_dst)) {
        const bool is_pure_mem_store = pk->pattern.IsSet() && pk->pattern.SetDest().IsMem();
        if (is_pure_mem_store) {
          bool mem_read_after_store = false;
          for (size_t m = k + 1; m < scan_insns.size(); ++m) {
            auto pm = ParsePatternAndNotes(*scan_insns[m]);
            if (!pm.ok()) {
              break;
            }
            if (pm->pattern.IsSet() && !pm->pattern.SetDest().IsMem()) {
              pm->pattern.SetSrc().Visit([&](const RtlExpr& n) {
                if (n.op == RtlOp::kMem) {
                  mem_read_after_store = true;
                }
              });
            } else if (!pm->pattern.IsSet()) {
              pm->pattern.Visit([&](const RtlExpr& n) {
                if (n.op == RtlOp::kMem || n.op == RtlOp::kCall) {
                  mem_read_after_store = true;
                }
              });
            }
            if (mem_read_after_store) {
              break;
            }
          }
          if (!mem_read_after_store) {
            continue;
          }
        }
        slot_consumed_first = true;
        break;
      }
    }
    if (!load_consumed_later || slot_consumed_first) {
      break;
    }
    ++ins_pos;
  }
  return ins_pos;
}

bool IsInsnSkippedByFillSlotsFromThread(const sched_model::Insn& insn,
                                        const absl::btree_set<int>* opposite_live_regs,
                                        const EagerStolenSlotSite& site,
                                        const ParsedCompilerFlags& flags,
                                        const sched_model::MachineModel& mm) {
  if (insn.kind != sched_model::Insn::Kind::kInsn) {
    return false;
  }
  const RtlExpr& pat = insn.pn.pattern;
  if (pat.op == RtlOp::kSequence || pat.op == RtlOp::kBarrier || pat.op == RtlOp::kUse ||
      pat.op == RtlOp::kClobber) {
    return false;
  }
  // `reorg.c:3386, 3405-3407` + `mips.md:79-82, 125-138`:
  // instructions failing `(dslot == no && length == 1)` are always skipped.
  if (!IsEligibleForMipsDelaySlot(
          pat, MipsDelaySlotOptions{
                   .gp64 = mm.Target64Bit(),
                   .fp64 = mm.TargetFloat64(),
                   .mips_isa = flags.mips_isa,
                   .allow_sdata_symbolic_store = (flags.small_data_threshold > 0),
                   .kind = MipsDelaySlotKind::kBranch,
               })) {
    return true;
  }
  // `reorg.c:3375-3378, 3389-3407` + `mips.md:125-128`:
  // MIPS has `ANNUL_IFTRUE == (nil)` (so fallthrough steals can never annul) and
  // `ANNUL_IFFALSE` requires `branch_likely == yes` (`-mips2`+). When annulling
  // is unavailable, any insn that may trap, modifies memory (`opposite_needed.memory`),
  // or sets a register live at the opposite branch target is skipped.
  const bool can_annul = site.thread_if_true && (flags.mips_isa >= 2);
  if (!can_annul && !site.is_annulled && opposite_live_regs != nullptr) {
    if (PatternMayTrapForReorg(pat) || (pat.IsSet() && pat.SetDest().IsMem())) {
      return true;
    }
    if (pat.IsSet() && pat.children.size() == 2) {
      const RtlExpr* dst = &pat.SetDest();
      while ((dst->op == RtlOp::kSubreg || dst->op == RtlOp::kStrictLowPart ||
              dst->op == RtlOp::kZeroExtract || dst->op == RtlOp::kSignExtract) &&
             !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (dst->IsReg() && dst->RegNo() >= 0 && dst->RegNo() < 72) {
        const int nr = mm.HardRegNRegs(dst->RegNo(), dst->mode);
        for (int k = 0; k < nr; ++k) {
          if (opposite_live_regs->contains(dst->RegNo() + k)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

// Forward-verified Stage 1 preimage selector for `reorg.c:3258-3460`
// (`fill_slots_from_thread`): when restoring a stolen delay-slot insn at the
// head of an owned target/fallthrough block leaves the block outside
// `Im(F_sched1)`, searches later positions `cand_s > slot_s` in the same block
// strictly gated on:
//   (1) exact `reorg.c:3316-3460` forward-scan skip legality,
//   (2) post-reload `Pass04`/`Pass05` safety (`PreservesPostReloadOrderSafety`),
//   (3) `Pass03` `sched2` achievability (`IsAchievable(kSched2)` when `RunsSched2`),
//   (4) `Pass06` `sched1` achievability (`IsAchievable(kSched1)`).
void SelectSched1AchievableEagerStolenPositions(RtlControlFlowGraph* cfg,
                                                absl::Span<const EagerStolenSlotSite> sites,
                                                const CompilationProfile& profile) {
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (cfg == nullptr || sites.empty() || flags.opt_level <= 0 || !flags.RunsSched1()) {
    return;
  }
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  const sched_model::RegFacts facts_s2 =
      sched_model::RegFacts::FromCfg(*cfg, profile, sched_model::Phase::kSched2);
  const sched_model::CfgSched1Context cfg_ctx =
      sched_model::ComputeCfgSched1Context(*cfg, *cfg, mm);

  bool any_moved = false;
  for (const EagerStolenSlotSite& site : sites) {
    if (!site.valid || site.block_idx >= cfg->rpo_blocks.size()) {
      continue;
    }
    RtlBasicBlock& blk = cfg->rpo_blocks[site.block_idx];
    size_t slot_idx = site.slot_idx;
    if (slot_idx >= blk.instructions.size() || blk.instructions[slot_idx] != site.slot_text) {
      bool found = false;
      for (size_t i = 0; i < blk.instructions.size(); ++i) {
        if (blk.instructions[i] == site.slot_text) {
          slot_idx = i;
          found = true;
          break;
        }
        auto pi = ParsePatternAndNotes(blk.instructions[i]);
        if (pi.ok() && pi->pattern.op != RtlOp::kNote) {
          break;
        }
      }
      if (!found) {
        continue;
      }
    }

    const sched_model::ParsedSchedRegion region =
        sched_model::ParseBlockForSchedModel(blk, site.block_idx, sched_model::Phase::kSched2);
    if (!region.valid || region.has_mid_block_loop_barrier || region.sched_block.size() <= 2) {
      continue;
    }
    const int n = static_cast<int>(region.sched_block.size());
    int slot_s = -1;
    for (int i = 0; i < n; ++i) {
      if (region.sched_orig_indices[static_cast<size_t>(i)] == slot_idx) {
        slot_s = i;
        break;
      }
    }
    if (slot_s < 0) {
      continue;
    }

    const sched_model::PostReloadBodyBounds bounds =
        sched_model::FindPostReloadBodyBounds(region.sched_block, site.block_idx);
    if (slot_s < bounds.prologue_end || slot_s >= bounds.epilogue_begin) {
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
    if (base_view.core_insns.size() <= 1 ||
        sched_model::IsAchievable(base_view.core_insns, base_view.facts_s1, mm,
                                  sched_model::Phase::kSched1, base_view.live_at_end_s1)) {
      continue;
    }

    absl::btree_set<int> opposite_live_regs;
    const absl::btree_set<int>* opp_live_ptr = nullptr;
    if (site.opposite_block_idx.has_value() && *site.opposite_block_idx < cfg->rpo_blocks.size()) {
      if (const auto it = cfg->sideband.basic_block_live_at_start.find(
              static_cast<int>(*site.opposite_block_idx));
          it != cfg->sideband.basic_block_live_at_start.end()) {
        opposite_live_regs.insert(it->second.begin(), it->second.end());
        opp_live_ptr = &opposite_live_regs;
      }
    }

    const RtlPatternAndNotes& slot_pn = region.sched_block[static_cast<size_t>(slot_s)].pn;
    std::optional<size_t> chosen_blk_pos;
    for (int cand_s = slot_s + 1; cand_s < bounds.epilogue_begin; ++cand_s) {
      const sched_model::Insn& k_insn = region.sched_block[static_cast<size_t>(cand_s)];
      if (k_insn.kind != sched_model::Insn::Kind::kInsn ||
          ExpressionsConflict(k_insn.pn.pattern, slot_pn.pattern)) {
        break;
      }
      if (!IsInsnSkippedByFillSlotsFromThread(k_insn, opp_live_ptr, site, flags, mm)) {
        break;
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
              region.sched_block, cand_in_base, bounds.prologue_end, bounds.epilogue_begin,
              live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
              /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
              proj_ctx.excluded_prologue_body_indices)) {
        continue;
      }

      std::vector<sched_model::Insn> cand_sched;
      cand_sched.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        sched_model::Insn insn =
            region.sched_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
        insn.luid = pos;
        cand_sched.push_back(std::move(insn));
      }

      // Gate 3: `Pass03` `sched2` achievability.
      if (flags.RunsSched2() &&
          !sched_model::IsAchievable(cand_sched, facts_s2, mm, sched_model::Phase::kSched2,
                                     live_at_end_s2)) {
        continue;
      }

      // Gate 4: `Pass06` `sched1` achievability.
      std::vector<sched_model::Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int pos = bounds.prologue_end; pos < bounds.epilogue_begin; ++pos) {
        sched_model::Insn insn = cand_sched[static_cast<size_t>(pos)];
        insn.luid = pos - bounds.prologue_end;
        cand_body.push_back(std::move(insn));
      }
      sched_model::Sched1ProjectionContext cand_proj_ctx = proj_ctx;
      cand_proj_ctx.excluded_prologue_body_indices =
          sched_model::FindInterleavedPrologueSaveBodyIndices(cand_sched, bounds, site.block_idx,
                                                              mm);
      const sched_model::Sched1ProjectionView cand_view =
          sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2,
                                                          cand_proj_ctx);
      if (cand_view.core_insns.size() <= 1 ||
          !sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                     sched_model::Phase::kSched1, cand_view.live_at_end_s1)) {
        continue;
      }

      chosen_blk_pos = region.sched_orig_indices[static_cast<size_t>(cand_s)];
      break;
    }

    if (!chosen_blk_pos.has_value()) {
      continue;
    }
    const bool has_synced_rtl = (blk.rtl_insns.size() == blk.instructions.size());
    std::string moved_str = std::move(blk.instructions[slot_idx]);
    blk.instructions.erase(blk.instructions.begin() + static_cast<ptrdiff_t>(slot_idx));
    const size_t insert_at = *chosen_blk_pos;
    blk.instructions.insert(blk.instructions.begin() + static_cast<ptrdiff_t>(insert_at),
                            std::move(moved_str));
    if (has_synced_rtl) {
      RtlInsn moved_rtl = std::move(blk.rtl_insns[slot_idx]);
      blk.rtl_insns.erase(blk.rtl_insns.begin() + static_cast<ptrdiff_t>(slot_idx));
      blk.rtl_insns.insert(blk.rtl_insns.begin() + static_cast<ptrdiff_t>(insert_at),
                           std::move(moved_rtl));
    }
    any_moved = true;
  }
  if (any_moved) {
    RecomputePostReloadLiveAtStart(cfg, profile);
  }
}

absl::flat_hash_set<int> CollectOrigUnfilledCondBranchBlockIds(const RtlControlFlowGraph& cfg) {
  absl::flat_hash_set<int> ids;
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    if (blk.successors.size() != 2 || blk.instructions.empty()) {
      continue;
    }
    auto p = ParsePatternAndNotes(blk.instructions.back());
    if (p.ok() && p->pattern.op != RtlOp::kSequence && IsCondJumpExpr(p->pattern)) {
      ids.insert(blk.id);
    }
  }
  return ids;
}

// Mirrors `combine.c:10831-10846` + `reorg.c:3294, 3375-3377, 3691-3730`:
// When `combine` simplifies an instruction at the head of a loop-latch block
// `fall_b` and emits a zero-code `(use (reg:M d)) (REG_DEAD d)` before
// `fall_b`'s first active insn `(set (reg:M d) <rhs>)`, `mark_target_live_regs`
// propagates `d` live around any def-free path from an enclosing branch target
// `target_b` back to `fall_b`, preventing `fill_eager_delay_slots` from pulling
// that length-1 non-trapping `SET` into an unfilled conditional branch at `b`.
// Restore `(use (reg:M d)) (REG_DEAD d)` when all conditions hold.
void RestoreCombineDeadUseForUnfilledEagerBranch(
    RtlControlFlowGraph* cfg, const absl::flat_hash_set<int>& orig_unfilled_cond_branch_ids,
    const CompilationProfile& profile) {
  if (cfg == nullptr || profile.optimize_level <= 0 || cfg->rpo_blocks.size() < 2) {
    return;
  }
  const size_t n_blocks = cfg->rpo_blocks.size();
  const auto preds = BuildPredecessors(*cfg);

  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  auto reg_covers_gpr = [gp64](const RtlExpr& reg_expr, int target_regno) -> bool {
    if (!reg_expr.IsReg() || reg_expr.IsArgPointerReg()) {
      return false;
    }
    const int base_r = reg_expr.RegNo();
    const int span =
        (!gp64 && (reg_expr.mode == RtlMachineMode::kDi || reg_expr.mode == RtlMachineMode::kDf))
            ? 2
            : 1;
    return target_regno >= base_r && target_regno < base_r + span;
  };
  auto block_kills_gpr = [&](size_t blk_idx, int regno) -> bool {
    const bool call_clobbers = (regno < 16 || regno == 24 || regno == 25);
    for (const std::string& raw : cfg->rpo_blocks[blk_idx].instructions) {
      auto parsed = ParsePatternAndNotes(raw);
      if (!parsed.ok()) {
        return true;
      }
      const RtlExpr& pat = parsed->pattern;
      if (pat.op == RtlOp::kNote || pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber) {
        continue;
      }
      bool killed = false;
      auto check_pat = [&](const auto& self, const RtlExpr& expr) -> void {
        if (killed) {
          return;
        }
        if (expr.op == RtlOp::kCall && call_clobbers) {
          killed = true;
          return;
        }
        if (expr.IsSet() && reg_covers_gpr(expr.SetDest(), regno)) {
          killed = true;
          return;
        }
        for (const RtlExpr& ch : expr.children) {
          self(self, ch);
        }
      };
      check_pat(check_pat, pat);
      if (killed) {
        return true;
      }
    }
    return false;
  };

  std::vector<int> inserted_use_regs;
  for (size_t b = 0; b + 1 < n_blocks; ++b) {
    const RtlBasicBlock& blk = cfg->rpo_blocks[b];
    if (!orig_unfilled_cond_branch_ids.contains(blk.id) || blk.successors.size() != 2 ||
        blk.instructions.empty()) {
      continue;
    }
    const size_t fall_b = b + 1;
    const size_t target_b = (blk.successors[0] == fall_b) ? blk.successors[1] : blk.successors[0];
    if (target_b == fall_b || target_b >= n_blocks || preds[fall_b].size() != 1 ||
        preds[fall_b][0] != b) {
      continue;
    }
    auto jmp_pn = ParsePatternAndNotes(blk.instructions.back());
    if (!jmp_pn.ok() || jmp_pn->pattern.op == RtlOp::kSequence ||
        !IsCondJumpExpr(jmp_pn->pattern)) {
      continue;
    }

    RtlBasicBlock& fall_blk = cfg->rpo_blocks[fall_b];
    size_t first_idx = fall_blk.instructions.size();
    RtlPatternAndNotes first_pn;
    bool has_prior_use = false;
    for (size_t i = 0; i < fall_blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(fall_blk.instructions[i]);
      if (!parsed.ok()) {
        first_idx = fall_blk.instructions.size();
        break;
      }
      if (parsed->pattern.op == RtlOp::kUse) {
        has_prior_use = true;
        continue;
      }
      if (!IsRealActiveInsn(parsed->pattern)) {
        continue;
      }
      first_idx = i;
      first_pn = std::move(*parsed);
      break;
    }
    if (has_prior_use || first_idx >= fall_blk.instructions.size()) {
      continue;
    }
    const RtlExpr& fpat = first_pn.pattern;
    if (!fpat.IsSet() || !fpat.SetDest().IsReg() || fpat.SetDest().IsArgPointerReg() ||
        fpat.SetDest().mode != RtlMachineMode::kSi || fpat.SetSrc().op != RtlOp::kConstInt ||
        fpat.SetSrc().int_val < -32768 || fpat.SetSrc().int_val > 65535) {
      continue;
    }
    const int dst_r = fpat.SetDest().RegNo();
    if (dst_r <= 0 || dst_r >= 32 || dst_r == 28 || dst_r == 29 || dst_r == 31 ||
        jmp_pn->pattern.ContainsReg(dst_r)) {
      continue;
    }
    if (IsAnyRegLiveAtBlockEntry(*cfg, absl::MakeConstSpan(&dst_r, 1), target_b)) {
      continue;
    }
    // Check whether `fall_b` is reachable from `target_b` along a path that
    // does not kill `dst_r` before `fall_b`, so `(use (reg:SI dst_r))` at the
    // head of `fall_b` propagates `dst_r` into `opposite_needed` at `target_b`.
    std::vector<bool> visited(n_blocks, false);
    std::vector<size_t> worklist = {target_b};
    visited[target_b] = true;
    bool reaches_fall_without_kill = false;
    while (!worklist.empty()) {
      const size_t cur = worklist.back();
      worklist.pop_back();
      if (cur == fall_b) {
        reaches_fall_without_kill = true;
        break;
      }
      if (block_kills_gpr(cur, dst_r)) {
        continue;
      }
      for (size_t succ : cfg->rpo_blocks[cur].successors) {
        if (succ < n_blocks && !visited[succ]) {
          visited[succ] = true;
          worklist.push_back(succ);
        }
      }
    }
    if (!reaches_fall_without_kill) {
      continue;
    }
    RtlPatternAndNotes use_pn;
    RtlExpr use_reg = fpat.SetDest();
    use_reg.flags = RtlFlags{};
    use_pn.pattern = RtlExpr::MakeUse(use_reg);
    use_pn.reg_notes.push_back(RtlRegNote{
        .kind = RtlMachineMode::kRegDead,
        .list_op = RtlOp::kExprList,
        .datum = std::move(use_reg),
    });
    fall_blk.instructions.insert(fall_blk.instructions.begin() + static_cast<ptrdiff_t>(first_idx),
                                 use_pn.ToGccPrintRtlString());
    inserted_use_regs.push_back(dst_r);
  }
  if (inserted_use_regs.empty()) {
    return;
  }
  // Mirrors `reorg.c:2415-2430, 2613-2654` (`mark_target_live_regs`):
  // `mark_target_live_regs` walks from the block after the preceding `BARRIER`
  // to `target`, accumulating `REG_DEAD` notes into `pending_dead_regs` and
  // clearing them from `current_live_regs` at the next `CODE_LABEL`. Once the
  // restored `(use (reg:SI dst_r))` makes `dst_r` live around the enclosing
  // loop, remove any now-stale `REG_DEAD` note for `dst_r` on instructions
  // where `dst_r` is live-out (up to the preceding definition of `dst_r`).
  RecomputePostReloadLiveAtStart(cfg, profile);
  for (int dst_r : inserted_use_regs) {
    const bool call_clobbers = (dst_r < 16 || dst_r == 24 || dst_r == 25);
    for (size_t k = 0; k < cfg->rpo_blocks.size(); ++k) {
      RtlBasicBlock& blk = cfg->rpo_blocks[k];
      bool live_after = false;
      for (size_t succ : blk.successors) {
        if (succ < cfg->rpo_blocks.size() &&
            IsAnyRegLiveAtBlockEntry(*cfg, absl::MakeConstSpan(&dst_r, 1), succ)) {
          live_after = true;
          break;
        }
      }
      if (!live_after) {
        continue;
      }
      for (size_t rev = blk.instructions.size(); rev > 0; --rev) {
        const size_t i = rev - 1;
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok()) {
          break;
        }
        if (parsed->pattern.op == RtlOp::kNote || parsed->pattern.op == RtlOp::kUse ||
            parsed->pattern.op == RtlOp::kClobber) {
          continue;
        }
        bool changed_notes = false;
        std::vector<RtlRegNote> kept_notes;
        kept_notes.reserve(parsed->reg_notes.size());
        for (RtlRegNote& note : parsed->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg() &&
              !note.datum.IsArgPointerReg() && note.datum.RegNo() == dst_r) {
            changed_notes = true;
            continue;
          }
          kept_notes.push_back(std::move(note));
        }
        if (changed_notes) {
          parsed->reg_notes = std::move(kept_notes);
          blk.instructions[i] = parsed->ToGccPrintRtlString();
        }
        bool defines_dst = false;
        auto check_def = [&](const auto& self, const RtlExpr& expr) -> void {
          if (defines_dst) {
            return;
          }
          if (expr.op == RtlOp::kCall && call_clobbers) {
            defines_dst = true;
            return;
          }
          if (expr.IsSet() && reg_covers_gpr(expr.SetDest(), dst_r)) {
            defines_dst = true;
            return;
          }
          for (const RtlExpr& ch : expr.children) {
            self(self, ch);
          }
        };
        check_def(check_def, parsed->pattern);
        if (defines_dst) {
          break;
        }
      }
    }
  }
}

}  // namespace

RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& dbr_eager_cfg_x,
                                              const CompilationProfile& profile) {
  if (!IsValidCfg(dbr_eager_cfg_x)) {
    return {};
  }
  const absl::flat_hash_set<int> orig_unfilled_cond_branch_ids =
      CollectOrigUnfilledCondBranchBlockIds(dbr_eager_cfg_x);

  RtlControlFlowGraph out = dbr_eager_cfg_x;
  out.pass = RtlPassKind::kDbr;

  std::vector<bool> entered_with_filled_end_jump(out.rpo_blocks.size(), false);
  for (size_t i = 0; i < out.rpo_blocks.size(); ++i) {
    if (out.rpo_blocks[i].instructions.empty()) {
      continue;
    }
    auto p = ParsePatternAndNotes(out.rpo_blocks[i].instructions.back());
    if (p.ok() && p->pattern.op == RtlOp::kSequence && p->pattern.children.size() == 2 &&
        p->pattern.children[0].op == RtlOp::kJumpInsn) {
      entered_with_filled_end_jump[i] = true;
    }
  }
  std::vector<EagerStolenSlotSite> stolen_sites;
  auto insert_block_at = [&](size_t insert_idx, RtlBasicBlock new_blk) {
    InsertBlockAt(&out, insert_idx, std::move(new_blk), &stolen_sites);
    entered_with_filled_end_jump.insert(
        entered_with_filled_end_jump.begin() + static_cast<ptrdiff_t>(insert_idx), false);
  };
  auto erase_block_at = [&](size_t erase_idx) {
    EraseBlockAt(&out, erase_idx, &stolen_sites);
    entered_with_filled_end_jump.erase(entered_with_filled_end_jump.begin() +
                                       static_cast<ptrdiff_t>(erase_idx));
  };

  // Iterate in reverse block order to invert forward `fill_eager_delay_slots`
  // transformations (`fill_slots_from_thread`, `steal_delay_list_from_target`,
  // `steal_delay_list_from_fallthrough`).
  for (size_t rev_idx = out.rpo_blocks.size(); rev_idx > 0; --rev_idx) {
    size_t b = rev_idx - 1;
    if (b >= out.rpo_blocks.size()) {
      continue;
    }
    auto& blk = out.rpo_blocks[b];
    if (blk.instructions.empty()) {
      continue;
    }
    // `optimize_skip` sequences have `same_succ == true` and belong to
    // `fill_simple_delay_slots`.
    if (blk.successors.size() == 2 && blk.successors[0] == blk.successors[1]) {
      continue;
    }

    auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
    if (!last_parsed.ok() || last_parsed->pattern.op != RtlOp::kSequence ||
        last_parsed->pattern.children.size() != 2) {
      continue;
    }
    const RtlExpr& jump_sub = last_parsed->pattern.children[0];
    const RtlExpr& slot_sub = last_parsed->pattern.children[1];
    if (jump_sub.op != RtlOp::kJumpInsn || slot_sub.op != RtlOp::kInsn ||
        jump_sub.children.empty() || slot_sub.children.empty()) {
      continue;
    }
    auto jump_pat_notes = RtlPatternAndNotes::FromSequenceSubInsn(jump_sub);
    auto slot_pat_notes = RtlPatternAndNotes::FromSequenceSubInsn(slot_sub);
    if (!jump_pat_notes.ok() || !slot_pat_notes.ok()) {
      continue;
    }
    const RtlExpr& jump_pat = jump_pat_notes->pattern;
    const bool is_cond_jump = IsCondJumpExpr(jump_pat);
    const bool is_simple_uncond_jump = IsSimpleUncondJumpExpr(jump_pat);
    if (!is_cond_jump && !is_simple_uncond_jump) {
      continue;
    }
    if (IsOptimizeSkipSequenceBlock(out, b, slot_sub, is_cond_jump)) {
      continue;
    }

    // Case A: Fallthrough-thread (`!slot_sub.flags.in_struct`, i.e. `insn`
    // without `/s`) on a conditional jump (`is_cond_jump`), unless assembly
    // erased `INSN_FROM_TARGET_P` (`insn/s`) on a non-annulled target-thread
    // steal whose destination register is immediately killed on `fallthrough_b`
    // (or read-before-written at `target_b`).
    if (!slot_sub.flags.in_struct) {
      if (TryInvertFallthroughThreadSequence(&out, &entered_with_filled_end_jump, &stolen_sites, b,
                                             jump_sub, *jump_pat_notes, *slot_pat_notes,
                                             is_cond_jump, is_simple_uncond_jump, profile) ==
          FallthroughSequenceResult::kHandledOrSkipped) {
        continue;
      }
    }

    // Case B: Target-thread (`slot_sub.flags.in_struct == true`, i.e. `insn/s`,
    // with `same_succ == false`).
    size_t target_b = blk.successors[0];
    if (is_cond_jump && blk.successors.size() == 2) {
      const size_t fallthrough_b = b + 1;
      target_b = (blk.successors[0] == fallthrough_b) ? blk.successors[1] : blk.successors[0];
    }
    if (target_b >= out.rpo_blocks.size()) {
      continue;
    }

    const RtlExpr orig_slot_pat = slot_pat_notes->pattern;
    auto preds = BuildPredecessors(out);
    if (is_cond_jump && jump_sub.flags.unchanging && blk.successors.size() == 2 &&
        IsOptimizeSkipForm2ToUseOrClobberTarget(out, preds, b, target_b)) {
      continue;
    }
    bool pred_falls_through_to_target =
        target_b > 0 && (target_b - 1 != b) && BlockFallsThroughToNext(out, preds, target_b - 1);

    std::vector<size_t> same_slot_jumpers;
    bool has_other_jumpers = false;
    CollectSameSlotJumpersToTarget(out, preds, b, target_b, pred_falls_through_to_target,
                                   orig_slot_pat, entered_with_filled_end_jump, &same_slot_jumpers,
                                   &has_other_jumpers);

    const bool need_loop_vtop = ComputeLoopVtopNeededForTargetSteal(
        out, preds, b, target_b, is_cond_jump, pred_falls_through_to_target, same_slot_jumpers,
        slot_pat_notes->pattern);
    bool need_loop_beg = false;
    auto maybe_prepend_loop_vtop = [&](size_t hdr_b) {
      PrependLoopVtopAndBegIfNeeded(&out, &entered_with_filled_end_jump, &stolen_sites, hdr_b,
                                    need_loop_vtop, need_loop_beg, b, &rev_idx);
    };

    RestoreSubwordConstSlotModeIfNeeded(out, preds, b, target_b, orig_slot_pat, &*slot_pat_notes);

    // Invert `reorg.c:3528-3586` opposite-arithmetic fallthrough compensation:
    // when a non-annulled conditional jump in `same_slot_jumpers` stole
    // `(set D (plus/minus:M D OTHER))` (`OTHER` not mentioning `D`) and `D` was
    // live on its `own_fallthrough` block `p_blk + 1`, `fill_eager_delay_slots`
    // emitted the opposite adjustment (`-C` or `PLUS <-> MINUS`) immediately
    // after `p_blk`.
    TryRecoverMissingFallthroughCompensation(
        &out, &entered_with_filled_end_jump, &stolen_sites, &rev_idx, &b, &target_b,
        &same_slot_jumpers, &preds, &pred_falls_through_to_target, &need_loop_beg, orig_slot_pat);

    // Unpack the terminating `SEQUENCE` of every block in `same_slot_jumpers`.
    for (size_t p_blk : same_slot_jumpers) {
      auto& p_block = out.rpo_blocks[p_blk];
      auto p_last = ParsePatternAndNotes(p_block.instructions.back());
      if (p_last.ok() && p_last->pattern.op == RtlOp::kSequence &&
          !p_last->pattern.children.empty()) {
        auto p_jump = RtlPatternAndNotes::FromSequenceSubInsn(p_last->pattern.children[0]);
        if (p_jump.ok()) {
          p_block.instructions.back() = p_jump->ToGccPrintRtlString();
        }
      }
    }

    // If `reorg.c:next_active_insn` skipped a `(use ...)`/`(clobber ...)`-only
    // block `target_b - 1` and inserted `target_b`'s label solely for
    // `same_slot_jumpers`, re-join `target_b` onto `target_b - 1` so Case T1
    // inspects `target_b - 2`.
    if (pred_falls_through_to_target && !has_other_jumpers && target_b >= 2 &&
        (target_b - 2 != b) && BlockHasZeroRealActiveInsns(out.rpo_blocks[target_b - 1]) &&
        BlockFallsThroughToNext(out, preds, target_b - 2)) {
      const auto& prev2_blk = out.rpo_blocks[target_b - 2];
      ptrdiff_t prev2_active = -1;
      for (size_t i = 0; i < prev2_blk.instructions.size(); ++i) {
        auto p = ParsePatternAndNotes(prev2_blk.instructions[i]);
        if (p.ok() && IsRealActiveInsn(p->pattern)) {
          prev2_active = static_cast<ptrdiff_t>(i);
        }
      }
      if (prev2_active >= 0) {
        auto p2 = ParsePatternAndNotes(prev2_blk.instructions[static_cast<size_t>(prev2_active)]);
        if (p2.ok() && EqualIgnoringRegFlags(p2->pattern, orig_slot_pat)) {
          const size_t old_target_b = target_b;
          const size_t use_blk_b = target_b - 1;
          for (size_t& p_blk : same_slot_jumpers) {
            for (size_t& succ : out.rpo_blocks[p_blk].successors) {
              if (succ == old_target_b) {
                succ = use_blk_b;
              }
            }
            if (p_blk > old_target_b) {
              --p_blk;
            }
          }
          auto& use_blk = out.rpo_blocks[use_blk_b];
          const auto& ret_blk = out.rpo_blocks[old_target_b];
          use_blk.instructions.insert(use_blk.instructions.end(), ret_blk.instructions.begin(),
                                      ret_blk.instructions.end());
          use_blk.successors = ret_blk.successors;
          use_blk.addr_vec_targets = ret_blk.addr_vec_targets;
          erase_block_at(old_target_b);
          if (old_target_b <= b && rev_idx > 0) {
            --rev_idx;
          }
          if (b > old_target_b) {
            --b;
          }
          target_b = use_blk_b;
          preds = BuildPredecessors(out);
          pred_falls_through_to_target = true;
          has_other_jumpers = false;
          for (size_t p_blk : preds[target_b]) {
            if (p_blk == target_b - 1 && p_blk != b) {
              continue;
            }
            if (std::find(same_slot_jumpers.begin(), same_slot_jumpers.end(), p_blk) ==
                same_slot_jumpers.end()) {
              has_other_jumpers = true;
              break;
            }
          }
        }
      }
    }

    // Check Case T1: `!own_target` copy where `pred_b = target_b - 1` falls
    // through to `target_b` and its last real active instruction (`IsRealActiveInsn`)
    // matches `slot_pat_only_sexpr`.
    if (pred_falls_through_to_target) {
      const size_t pred_b = target_b - 1;
      auto& pred_blk = out.rpo_blocks[pred_b];
      ptrdiff_t last_active_idx = -1;
      size_t active_count = 0;
      for (size_t i = 0; i < pred_blk.instructions.size(); ++i) {
        auto p = ParsePatternAndNotes(pred_blk.instructions[i]);
        if (p.ok() && IsRealActiveInsn(p->pattern)) {
          last_active_idx = static_cast<ptrdiff_t>(i);
          ++active_count;
        }
      }
      if (last_active_idx >= 0) {
        auto last_active_parsed =
            ParsePatternAndNotes(pred_blk.instructions[static_cast<size_t>(last_active_idx)]);
        if (last_active_parsed.ok() &&
            EqualIgnoringRegFlags(last_active_parsed->pattern, orig_slot_pat)) {
          size_t move_start = static_cast<size_t>(last_active_idx);
          bool target_has_loop_note = need_loop_vtop;
          for (const std::string& ts : out.rpo_blocks[target_b].instructions) {
            auto tp = ParsePatternAndNotes(ts);
            if (!tp.ok() || tp->pattern.op != RtlOp::kNote) {
              break;
            }
            if (IsLoopHeaderNote(tp->pattern)) {
              target_has_loop_note = true;
              break;
            }
          }
          while (move_start > 0) {
            auto prev_note = ParsePatternAndNotes(pred_blk.instructions[move_start - 1]);
            if (prev_note.ok() && IsLoopHeaderNote(prev_note->pattern)) {
              bool is_loop_cont = false;
              for (const auto& item : prev_note->pattern.generic_items) {
                if (item.text == "NOTE_INSN_LOOP_CONT") {
                  is_loop_cont = true;
                  break;
                }
              }
              if (active_count > 1 && is_loop_cont && target_has_loop_note) {
                break;
              }
              --move_start;
            } else {
              break;
            }
          }

          bool gives_earlier_cond_own_fallthrough = false;
          if (active_count == 1 && move_start == 0 && !pred_blk.has_label && pred_b > 0 &&
              pred_b - 1 != b &&
              std::find(same_slot_jumpers.begin(), same_slot_jumpers.end(), pred_b - 1) ==
                  same_slot_jumpers.end() &&
              out.rpo_blocks[pred_b - 1].successors.size() == 2 &&
              !out.rpo_blocks[pred_b - 1].instructions.empty()) {
            auto earlier_last =
                ParsePatternAndNotes(out.rpo_blocks[pred_b - 1].instructions.back());
            if (earlier_last.ok() && earlier_last->pattern.op == RtlOp::kSequence &&
                earlier_last->pattern.children.size() == 2 &&
                earlier_last->pattern.children[0].op == RtlOp::kJumpInsn &&
                earlier_last->pattern.children[1].flags.in_struct) {
              gives_earlier_cond_own_fallthrough = true;
            }
          }

          if ((active_count == 1 && move_start == 0 && !gives_earlier_cond_own_fallthrough) ||
              PrefixInsnsAreRedundantInAllJumpers(out, pred_blk, move_start, pred_b,
                                                  same_slot_jumpers)) {
            // `pred_b` itself was the target block before its label shifted to
            // `target_b`. Redirect all `same_slot_jumpers` to `pred_b`.
            for (size_t p_blk : same_slot_jumpers) {
              for (size_t& succ : out.rpo_blocks[p_blk].successors) {
                if (succ == target_b) {
                  succ = pred_b;
                }
              }
              if (p_blk + 1 == pred_b && out.rpo_blocks[p_blk].successors.size() == 1 &&
                  !out.rpo_blocks[p_blk].instructions.empty()) {
                bool prev_inserts_before_pred = false;
                if (p_blk > 0 && !out.rpo_blocks[p_blk - 1].instructions.empty() &&
                    out.rpo_blocks[p_blk - 1].successors.size() == 2 &&
                    (out.rpo_blocks[p_blk - 1].successors[0] == pred_b ||
                     out.rpo_blocks[p_blk - 1].successors[1] == pred_b)) {
                  auto prev_last =
                      ParsePatternAndNotes(out.rpo_blocks[p_blk - 1].instructions.back());
                  if (prev_last.ok() && prev_last->pattern.op == RtlOp::kSequence &&
                      prev_last->pattern.children.size() == 2 &&
                      prev_last->pattern.children[0].op == RtlOp::kJumpInsn &&
                      prev_last->pattern.children[1].flags.in_struct) {
                    prev_inserts_before_pred = true;
                  }
                }
                auto p_last = ParsePatternAndNotes(out.rpo_blocks[p_blk].instructions.back());
                if (!prev_inserts_before_pred && p_last.ok() &&
                    IsSimpleUncondJumpExpr(p_last->pattern)) {
                  out.rpo_blocks[p_blk].instructions.pop_back();
                }
              }
            }
            out.rpo_blocks[pred_b].has_label = true;
            if (!has_other_jumpers) {
              auto& tgt_blk = out.rpo_blocks[target_b];
              out.rpo_blocks[pred_b].instructions.insert(out.rpo_blocks[pred_b].instructions.end(),
                                                         tgt_blk.instructions.begin(),
                                                         tgt_blk.instructions.end());
              out.rpo_blocks[pred_b].successors = tgt_blk.successors;
              out.rpo_blocks[pred_b].addr_vec_targets = tgt_blk.addr_vec_targets;
              erase_block_at(target_b);
              if (target_b <= b && rev_idx > 0) {
                --rev_idx;
              }
            }
            maybe_prepend_loop_vtop(pred_b);
            continue;
          }

          if (!has_other_jumpers) {
            // Move `move_start .. end` from `pred_b` to the beginning of
            // `target_b`.
            std::vector<std::string> moved_slice(
                pred_blk.instructions.begin() + static_cast<ptrdiff_t>(move_start),
                pred_blk.instructions.end());
            pred_blk.instructions.erase(
                pred_blk.instructions.begin() + static_cast<ptrdiff_t>(move_start),
                pred_blk.instructions.end());
            auto& tgt_blk = out.rpo_blocks[target_b];
            const size_t t1_ins_pos =
                (moved_slice.size() == 1)
                    ? ComputeTargetStolenInsertPos(out, target_b, tgt_blk, *slot_pat_notes, profile)
                    : 0;
            tgt_blk.instructions.insert(
                tgt_blk.instructions.begin() + static_cast<ptrdiff_t>(t1_ins_pos),
                moved_slice.begin(), moved_slice.end());
            if (moved_slice.size() == 1) {
              std::optional<size_t> opp_b;
              if (is_cond_jump && same_slot_jumpers.size() == 1 && b + 1 < out.rpo_blocks.size()) {
                opp_b = b + 1;
              }
              stolen_sites.push_back(EagerStolenSlotSite{
                  .block_idx = target_b,
                  .slot_idx = t1_ins_pos,
                  .opposite_block_idx = opp_b,
                  .thread_if_true = true,
                  .is_annulled = jump_sub.flags.unchanging,
                  .valid = true,
                  .slot_text = moved_slice[0],
              });
            }
            tgt_blk.has_label = true;
            if (pred_blk.instructions.empty() && pred_blk.addr_vec_targets.empty() &&
                pred_blk.successors.size() == 1 && pred_blk.successors[0] == target_b) {
              for (auto& other : out.rpo_blocks) {
                for (size_t& succ : other.successors) {
                  if (succ == pred_b) {
                    succ = target_b;
                  }
                }
                for (size_t& target : other.addr_vec_targets) {
                  if (target == pred_b) {
                    target = target_b;
                  }
                }
              }
              erase_block_at(pred_b);
              if (pred_b <= b && rev_idx > 0) {
                --rev_idx;
              }
              if (b > pred_b) {
                --b;
              }
              --target_b;
            }
            maybe_prepend_loop_vtop(target_b);
            continue;
          } else {
            // `has_other_jumpers` is true: `move_start .. end` was a separate
            // fallthrough block `new_blk` between `pred_b` and `target_b`
            // targeted by `same_slot_jumpers`.
            std::vector<std::string> blk_slice(
                pred_blk.instructions.begin() + static_cast<ptrdiff_t>(move_start),
                pred_blk.instructions.end());
            pred_blk.instructions.erase(
                pred_blk.instructions.begin() + static_cast<ptrdiff_t>(move_start),
                pred_blk.instructions.end());

            const size_t old_target_b = target_b;
            RtlBasicBlock new_blk;
            new_blk.has_label = true;
            new_blk.instructions = std::move(blk_slice);
            new_blk.successors = {old_target_b + 1};
            insert_block_at(old_target_b, std::move(new_blk));
            // `pred_b = old_target_b - 1` had `old_target_b` as its fallthrough
            // successor, which `InsertBlockAt` shifted to `old_target_b + 1`.
            // Restore `pred_b`'s fallthrough successor to `old_target_b` (`new_blk`).
            for (size_t& succ : out.rpo_blocks[pred_b].successors) {
              if (succ == old_target_b + 1) {
                succ = old_target_b;
                break;
              }
            }
            for (size_t p_blk : same_slot_jumpers) {
              const size_t new_p_blk = (p_blk >= old_target_b) ? p_blk + 1 : p_blk;
              for (size_t& succ : out.rpo_blocks[new_p_blk].successors) {
                if (succ == old_target_b + 1) {
                  succ = old_target_b;
                  break;
                }
              }
            }
            if (b >= old_target_b) {
              ++rev_idx;
            }
            maybe_prepend_loop_vtop(old_target_b);
            continue;
          }
        }
      }
    }

    // Case T2: `!pred_falls_through_to_target` and `!has_other_jumpers` (all
    // predecessors of `target_b` are in `same_slot_jumpers`), OR the only other
    // predecessor of `target_b` is unlabeled `b + 1` (`preds[b + 1] == {b}`),
    // which relied on `b`'s non-annulled steal of `slot_defs` via
    // `reorg.c:redundant_insn` to read `slot_defs` at `target_b`.
    bool dominated_ft_jumper_shares_target = false;
    std::vector<int> t2_slot_defs;
    CollectDefinedRegs(slot_pat_notes->pattern, &t2_slot_defs);
    if (!pred_falls_through_to_target && has_other_jumpers && b + 1 < out.rpo_blocks.size() &&
        !out.rpo_blocks[b + 1].has_label && preds[b + 1].size() == 1 && preds[b + 1][0] == b &&
        !t2_slot_defs.empty() && IsAnyRegLiveAtBlockEntry(out, t2_slot_defs, target_b)) {
      bool only_ft_other_jumper = true;
      for (size_t p_blk : preds[target_b]) {
        if (p_blk != b + 1 && std::find(same_slot_jumpers.begin(), same_slot_jumpers.end(),
                                        p_blk) == same_slot_jumpers.end()) {
          only_ft_other_jumper = false;
          break;
        }
      }
      bool slot_kills_own_defs = true;
      for (int dr : t2_slot_defs) {
        if (ExprReferencesRegInSrcOrMem(slot_pat_notes->pattern, dr)) {
          slot_kills_own_defs = false;
          break;
        }
      }
      if (only_ft_other_jumper && slot_kills_own_defs) {
        bool touched_or_live_outside_target = false;
        auto insn_touches_slot_defs = [&](const std::string& insn_s) {
          auto ip = ParsePatternAndNotes(insn_s);
          if (!ip.ok()) {
            return false;
          }
          std::vector<int> defs;
          CollectDefinedRegs(ip->pattern, &defs);
          for (int dr : t2_slot_defs) {
            if (ExprReferencesRegInSrcOrMem(ip->pattern, dr) ||
                std::find(defs.begin(), defs.end(), dr) != defs.end()) {
              return true;
            }
          }
          return false;
        };
        for (const std::string& bs : out.rpo_blocks[b].instructions) {
          if (insn_touches_slot_defs(bs)) {
            touched_or_live_outside_target = true;
            break;
          }
        }
        for (const std::string& fs : out.rpo_blocks[b + 1].instructions) {
          if (touched_or_live_outside_target || insn_touches_slot_defs(fs)) {
            touched_or_live_outside_target = true;
            break;
          }
        }
        for (size_t ft_succ : out.rpo_blocks[b + 1].successors) {
          if (touched_or_live_outside_target) {
            break;
          }
          if (ft_succ != target_b && IsAnyRegLiveAtBlockEntry(out, t2_slot_defs, ft_succ)) {
            touched_or_live_outside_target = true;
          }
        }
        dominated_ft_jumper_shares_target = !touched_or_live_outside_target;
      }
    }
    if (!pred_falls_through_to_target &&
        (!has_other_jumpers || dominated_ft_jumper_shares_target)) {
      auto& tgt_blk = out.rpo_blocks[target_b];
      const size_t tgt_ins_pos =
          ComputeTargetStolenInsertPos(out, target_b, tgt_blk, *slot_pat_notes, profile);
      const std::string slot_str = slot_pat_notes->ToGccPrintRtlString();
      tgt_blk.instructions.insert(
          tgt_blk.instructions.begin() + static_cast<ptrdiff_t>(tgt_ins_pos), slot_str);
      std::optional<size_t> opp_b;
      if (is_cond_jump && same_slot_jumpers.size() == 1 && b + 1 < out.rpo_blocks.size()) {
        opp_b = b + 1;
      }
      stolen_sites.push_back(EagerStolenSlotSite{
          .block_idx = target_b,
          .slot_idx = tgt_ins_pos,
          .opposite_block_idx = opp_b,
          .thread_if_true = true,
          .is_annulled = jump_sub.flags.unchanging,
          .valid = true,
          .slot_text = slot_str,
      });
      tgt_blk.has_label = true;
      maybe_prepend_loop_vtop(target_b);
      continue;
    }

    // Case T3: `target_b` is shared with other predecessors (`has_other_jumpers`
    // is true, or `pred_falls_through_to_target` is true without matching Case T1).
    if (!pred_falls_through_to_target) {
      const size_t old_target_b = target_b;
      RtlBasicBlock tramp_blk;
      tramp_blk.has_label = true;
      tramp_blk.instructions.push_back(slot_pat_notes->ToGccPrintRtlString());
      tramp_blk.successors = {old_target_b + 1};
      insert_block_at(old_target_b, std::move(tramp_blk));
      if (b >= old_target_b) {
        ++rev_idx;
      }
      for (size_t p_blk : same_slot_jumpers) {
        const size_t new_p_blk = (p_blk >= old_target_b) ? p_blk + 1 : p_blk;
        for (size_t& succ : out.rpo_blocks[new_p_blk].successors) {
          if (succ == old_target_b + 1) {
            succ = old_target_b;
            break;
          }
        }
      }
      maybe_prepend_loop_vtop(old_target_b);
    } else {
      // Before synthesizing a new `tramp_blk`, check if an existing unlabeled
      // reachable block `cand_b` already holds `[<slot_pat>; j target_b]` (or
      // its `fill_simple_delay_slots` `SEQUENCE` wrapper) whose `CODE_LABEL`
      // was deleted by `redirect_jump` when `same_slot_jumpers` stole from it
      // via `steal_delay_list_from_target`.
      std::optional<size_t> existing_tramp_b;
      bool has_back_edge_to_target = false;
      for (size_t p_blk : preds[target_b]) {
        if (p_blk >= target_b) {
          has_back_edge_to_target = true;
          break;
        }
      }
      if (!has_back_edge_to_target) {
        for (size_t cand_b = 0; cand_b < out.rpo_blocks.size(); ++cand_b) {
          if (cand_b == target_b || out.rpo_blocks[cand_b].has_label || preds[cand_b].empty()) {
            continue;
          }
          bool conflicts_with_jumper = false;
          for (size_t p_blk : same_slot_jumpers) {
            if (cand_b == p_blk || cand_b == p_blk + 1) {
              conflicts_with_jumper = true;
              break;
            }
          }
          if (conflicts_with_jumper) {
            continue;
          }
          const auto& cand_blk = out.rpo_blocks[cand_b];
          if (cand_blk.successors.size() != 1 ||
              (cand_blk.successors[0] != target_b &&
               !ReachesBlockAcrossZeroActiveFallthrough(out, cand_blk.successors[0], target_b))) {
            continue;
          }
          if (cand_blk.instructions.size() == 2) {
            auto cs = ParsePatternAndNotes(cand_blk.instructions[0]);
            auto cj = ParsePatternAndNotes(cand_blk.instructions[1]);
            if (cs.ok() && cj.ok() && IsSimpleUncondJumpExpr(cj->pattern) &&
                cs->pattern == orig_slot_pat) {
              out.rpo_blocks[cand_b].instructions = {
                  RtlExpr::MakeSequence(
                      {
                          RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, *cj),
                          RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, *cs),
                      })
                      .ToGccPrintRtlString()};
              existing_tramp_b = cand_b;
              break;
            }
          } else if (cand_blk.instructions.size() == 1) {
            auto cseq = ParsePatternAndNotes(cand_blk.instructions[0]);
            if (cseq.ok() && cseq->pattern.op == RtlOp::kSequence &&
                cseq->pattern.children.size() == 2 &&
                cseq->pattern.children[0].op == RtlOp::kJumpInsn &&
                !cseq->pattern.children[1].flags.in_struct) {
              auto cj = RtlPatternAndNotes::FromSequenceSubInsn(cseq->pattern.children[0]);
              auto cs = RtlPatternAndNotes::FromSequenceSubInsn(cseq->pattern.children[1]);
              if (cj.ok() && cs.ok() && IsSimpleUncondJumpExpr(cj->pattern) &&
                  cs->pattern == orig_slot_pat) {
                existing_tramp_b = cand_b;
                break;
              }
            }
          }
        }
      }
      if (existing_tramp_b.has_value()) {
        const size_t cand_b = *existing_tramp_b;
        out.rpo_blocks[cand_b].has_label = true;
        for (size_t p_blk : same_slot_jumpers) {
          for (size_t& succ : out.rpo_blocks[p_blk].successors) {
            if (succ == target_b) {
              succ = cand_b;
              break;
            }
          }
        }
        maybe_prepend_loop_vtop(cand_b);
        continue;
      }

      // `target_b - 1` falls through to `target_b`, so we cannot insert a block
      // directly before `target_b` without breaking `target_b - 1`'s fallthrough.
      // Instead, insert a block containing `(sequence [(jump_insn (set (pc)
      // (label_ref L_target))) (insn <slot_pat_notes>)])` immediately after a
      // block ending with an unconditional jump or return (so `MaterializeFromCfg`
      // emits a `BARRIER` before `tramp_blk` and `jump.c:delete_insn` deletes
      // `tramp_blk` when its label becomes unused).
      // If `reorg.c:next_active_insn` skipped an unlabeled `USE`/`CLOBBER`-only
      // block `target_b - 1` when stealing from `tramp_blk` (`j L_old`), merge
      // `target_b` back onto `target_b - 1` so `L_old` sits before the `USE`/
      // `CLOBBER` insns as in `pre_state`.
      bool merged_onto_use_blk = false;
      if (pred_falls_through_to_target && !has_other_jumpers && target_b >= 2 &&
          target_b - 1 != b && !out.rpo_blocks[target_b - 1].instructions.empty()) {
        bool only_use_or_clobber = true;
        for (const std::string& s : out.rpo_blocks[target_b - 1].instructions) {
          auto p = ParsePatternAndNotes(s);
          if (!p.ok() || (p->pattern.op != RtlOp::kUse && p->pattern.op != RtlOp::kClobber)) {
            only_use_or_clobber = false;
            break;
          }
        }
        if (only_use_or_clobber) {
          merged_onto_use_blk = true;
          const size_t old_target_b = target_b;
          const size_t use_blk_b = target_b - 1;
          for (size_t& p_blk : same_slot_jumpers) {
            for (size_t& succ : out.rpo_blocks[p_blk].successors) {
              if (succ == old_target_b) {
                succ = use_blk_b;
              }
            }
            if (p_blk > old_target_b) {
              --p_blk;
            }
          }
          auto& use_blk = out.rpo_blocks[use_blk_b];
          const auto& tgt_blk = out.rpo_blocks[old_target_b];
          use_blk.instructions.insert(use_blk.instructions.end(), tgt_blk.instructions.begin(),
                                      tgt_blk.instructions.end());
          use_blk.successors = tgt_blk.successors;
          use_blk.addr_vec_targets = tgt_blk.addr_vec_targets;
          erase_block_at(old_target_b);
          if (old_target_b <= b && rev_idx > 0) {
            --rev_idx;
          }
          if (b > old_target_b) {
            --b;
          }
          target_b = use_blk_b;
        }
      }

      out.rpo_blocks[target_b].has_label = true;
      const bool place_fallthrough_tramp_before_final_use =
          ShouldPlaceFallthroughTrampBeforeFinalUse(
              out, target_b, merged_onto_use_blk, pred_falls_through_to_target, has_other_jumpers);
      size_t insert_pos = out.rpo_blocks.size();
      bool found_barrier_pos = false;
      if (place_fallthrough_tramp_before_final_use) {
        insert_pos = target_b;
      } else {
        for (size_t b_scan = out.rpo_blocks.size(); b_scan > 0; --b_scan) {
          const auto& scan_blk = out.rpo_blocks[b_scan - 1];
          if (scan_blk.instructions.empty()) {
            continue;
          }
          auto scan_last = ParsePatternAndNotes(scan_blk.instructions.back());
          if (scan_last.ok() && IsUncondJumpOrReturnOrTableJumpTopExpr(scan_last->pattern) &&
              !IsTableJumpExpr(scan_last->pattern)) {
            insert_pos = b_scan;
            found_barrier_pos = true;
            break;
          }
        }
        if (!found_barrier_pos && !out.rpo_blocks.empty() &&
            BlockHasZeroRealActiveInsns(out.rpo_blocks.back())) {
          insert_pos = out.rpo_blocks.size() - 1;
        }
      }

      const size_t shifted_target_b = (target_b >= insert_pos) ? (target_b + 1) : target_b;
      RtlBasicBlock tramp_blk;
      tramp_blk.has_label = true;
      if (!found_barrier_pos && insert_pos == target_b) {
        tramp_blk.instructions.push_back(slot_pat_notes->ToGccPrintRtlString());
      } else {
        RtlPatternAndNotes uncond_jump;
        uncond_jump.pattern =
            RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"));
        tramp_blk.instructions.push_back(
            RtlExpr::MakeSequence(
                {
                    RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{}, uncond_jump),
                    RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, *slot_pat_notes),
                })
                .ToGccPrintRtlString());
      }
      tramp_blk.successors = {shifted_target_b};
      insert_block_at(insert_pos, std::move(tramp_blk));
      if (insert_pos + 1 < out.rpo_blocks.size()) {
        out.rpo_blocks[insert_pos + 1].has_label = true;
      }
      if (b >= insert_pos) {
        ++rev_idx;
      }
      for (size_t p_blk : same_slot_jumpers) {
        const size_t new_p_blk = (p_blk >= insert_pos) ? (p_blk + 1) : p_blk;
        for (size_t& succ : out.rpo_blocks[new_p_blk].successors) {
          if (succ == shifted_target_b) {
            succ = insert_pos;
            break;
          }
        }
      }
      maybe_prepend_loop_vtop(insert_pos);
    }
  }

  ApplyPostEagerDelaySlotNormalizations(&out, &stolen_sites, profile);
  RestoreCombineDeadUseForUnfilledEagerBranch(&out, orig_unfilled_cond_branch_ids, profile);

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    if (blk.successors.size() != 1 || blk.successors[0] == b + 1 ||
        blk.successors[0] >= out.rpo_blocks.size() || !blk.addr_vec_targets.empty()) {
      continue;
    }
    bool ends_with_jump = false;
    for (size_t k = blk.instructions.size(); k > 0;) {
      --k;
      auto parsed = ParsePatternAndNotes(blk.instructions[k]);
      if (!parsed.ok() || parsed->pattern.op == RtlOp::kNote) {
        continue;
      }
      if (parsed->pattern.op == RtlOp::kSequence) {
        if (!parsed->pattern.children.empty() && !parsed->pattern.children[0].children.empty() &&
            IsAnyJumpOrReturnTopExpr(parsed->pattern.children[0].children[0])) {
          ends_with_jump = true;
        }
      } else if (IsAnyJumpOrReturnTopExpr(parsed->pattern)) {
        ends_with_jump = true;
      }
      break;
    }
    if (!ends_with_jump) {
      out.rpo_blocks[blk.successors[0]].has_label = true;
      blk.instructions.push_back(
          RtlExpr::MakeSet(RtlExpr::MakePc(), RtlExpr::MakeSymbolicLabelRef("L0"))
              .ToGccPrintRtlString());
    }
  }

  SynchronizeCanonicalLabels(&out);
  out.SyncAllBlocksPreservingMachineInsns(dbr_eager_cfg_x.rpo_blocks);
  RecomputePostReloadLiveAtStart(&out, profile);
  SelectSched1AchievableEagerStolenPositions(&out, stolen_sites, profile);
  return out;
}

RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& dbr_eager_cfg_x) {
  const CompilationProfile default_profile{.optimize_level = 2};
  return InvertFillEagerDelaySlots(dbr_eager_cfg_x, default_profile);
}

}  // namespace rom_nom_nom::lifter
