#include "lifter/reconstruct_hard_reg_liveness.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

using RegMask = unsigned __int128;

constexpr RegMask kRetCandMask =
    (RegMask{1} << 2) | (RegMask{1} << 3) | (RegMask{1} << 32) | (RegMask{1} << 33);

constexpr RegMask BuildCallClobberedMask() {
  RegMask m = 0;
  for (int r = 1; r <= 15; ++r) m |= (RegMask{1} << r);
  for (int r = 24; r <= 27; ++r) m |= (RegMask{1} << r);
  m |= (RegMask{1} << 31);
  for (int r = 32; r <= 51; ++r) m |= (RegMask{1} << r);
  for (int r = 64; r <= 67; ++r) m |= (RegMask{1} << r);
  return m;
}

constexpr RegMask kCallClobberedMask = BuildCallClobberedMask();

constexpr RegMask BuildArgRegMask() {
  RegMask m = 0;
  for (int r = 4; r <= 7; ++r) m |= (RegMask{1} << r);
  for (int r = 44; r <= 47; ++r) m |= (RegMask{1} << r);
  return m;
}

constexpr RegMask kArgRegMask = BuildArgRegMask();

int ModeByteSize(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kQi:
      return 1;
    case RtlMachineMode::kHi:
      return 2;
    case RtlMachineMode::kSi:
    case RtlMachineMode::kSf:
    case RtlMachineMode::kCc:
      return 4;
    case RtlMachineMode::kDi:
    case RtlMachineMode::kDf:
      return 8;
    case RtlMachineMode::kTi:
    case RtlMachineMode::kTf:
      return 16;
    default:
      return 4;
  }
}

int HardRegNregs(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  const bool is_fp = (regno >= 32 && regno < 64);
  const int unit_bytes = is_fp ? (fp64 ? 8 : 4) : (gp64 ? 8 : 4);
  return std::max(1, (ModeByteSize(mode) + unit_bytes - 1) / unit_bytes);
}

RegMask RegBitMask(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
    return 0;
  }
  const int nregs = HardRegNregs(regno, mode, gp64, fp64);
  RegMask mask = 0;
  for (int r = regno; r < regno + nregs && r < kMipsFirstPseudoRegister; ++r) {
    mask |= (RegMask{1} << r);
  }
  return mask;
}

void DetectRegisterWidthFlags(const RtlControlFlowGraph& cfg, const CompilationProfile* profile,
                              bool* gp64, bool* fp64) {
  *gp64 = false;
  *fp64 = false;
  if (profile != nullptr) {
    for (const std::string& flag : profile->c_flags) {
      if (flag == "-mips3" || flag == "-mips4") {
        *gp64 = true;
        *fp64 = true;
      } else if (flag == "-mips1" || flag == "-mips2") {
        *gp64 = false;
        *fp64 = false;
      } else if (flag == "-mgp64") {
        *gp64 = true;
      } else if (flag == "-mgp32") {
        *gp64 = false;
      } else if (flag == "-mfp64") {
        *fp64 = true;
      } else if (flag == "-mfp32") {
        *fp64 = false;
      }
    }
    return;
  }
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& s : blk.instructions) {
      auto parsed = ParsePatternAndNotes(s);
      if (!parsed.ok()) {
        continue;
      }
      parsed->pattern.Visit([&](const RtlExpr& node) {
        if (node.IsReg() && node.RegNo() >= 32 && node.RegNo() < 64 && (node.RegNo() % 2) == 1) {
          *fp64 = true;
        }
        if (node.IsSet() && node.SetDest().IsReg() && node.SetDest().RegNo() < 32 &&
            node.SetSrc().mode == RtlMachineMode::kDi &&
            (node.SetSrc().op == RtlOp::kPlus || node.SetSrc().op == RtlOp::kMinus ||
             node.SetSrc().op == RtlOp::kAshift || node.SetSrc().op == RtlOp::kAshiftrt ||
             node.SetSrc().op == RtlOp::kLshiftrt || node.SetSrc().op == RtlOp::kSignExtend ||
             node.SetSrc().op == RtlOp::kZeroExtend)) {
          *gp64 = true;
        }
      });
    }
  }
}

bool IsCallPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kCall || (pat.IsSet() && pat.SetSrc().op == RtlOp::kCall)) {
    return true;
  }
  if (pat.IsParallel() && !pat.children.empty()) {
    const RtlExpr& c0 = pat.children[0];
    return c0.op == RtlOp::kCall || (c0.IsSet() && c0.SetSrc().op == RtlOp::kCall);
  }
  return false;
}

bool IsReturnJumpPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.IsParallel() && !pat.children.empty() && pat.children[0].op == RtlOp::kReturn) {
    return true;
  }
  if (pat.op == RtlOp::kSequence && !pat.children.empty() &&
      pat.children[0].op == RtlOp::kJumpInsn && !pat.children[0].children.empty()) {
    return IsReturnJumpPattern(pat.children[0].children[0]);
  }
  return false;
}

bool IsAnyControlTransferPattern(const RtlExpr& pat) {
  if (IsReturnJumpPattern(pat)) {
    return true;
  }
  if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
    return true;
  }
  if (pat.IsParallel() && !pat.children.empty() && pat.children[0].IsSet() &&
      pat.children[0].SetDest().op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kSequence && !pat.children.empty() &&
      pat.children[0].op == RtlOp::kJumpInsn && !pat.children[0].children.empty()) {
    return IsAnyControlTransferPattern(pat.children[0].children[0]);
  }
  return false;
}

bool HasExistingReturnUse(const RtlControlFlowGraph& cfg) {
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok() || parsed->pattern.op != RtlOp::kUse) {
        continue;
      }
      bool has_ret_reg = false;
      parsed->pattern.Visit([&has_ret_reg](const RtlExpr& node) {
        if (node.IsReg()) {
          const int r = node.RegNo();
          if (r == 2 || r == 3 || r == 32 || r == 33) {
            has_ret_reg = true;
          }
        }
      });
      if (has_ret_reg) {
        return true;
      }
    }
  }
  return false;
}

struct ElemSummary {
  RegMask uses = 0;
  RegMask defs = 0;
  RegMask explicit_sets = 0;
  RegMask integrated_ret_regs = 0;
  bool is_call = false;
  bool is_note_or_pseudo = false;
  std::array<RtlMachineMode, kMipsFirstPseudoRegister> use_modes{};
  std::array<RtlMachineMode, kMipsFirstPseudoRegister> def_modes{};
};

void CollectReadRegs(const RtlExpr& expr, bool gp64, bool fp64, ElemSummary* out) {
  if (expr.IsReg()) {
    const int regno = expr.RegNo();
    if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
      const RegMask m = RegBitMask(regno, expr.mode, gp64, fp64);
      out->uses |= m;
      out->use_modes[regno] = expr.mode;
      if (expr.flags.integrated) {
        out->integrated_ret_regs |= (m & kRetCandMask);
      }
    }
    return;
  }
  if (expr.IsSubreg() && !expr.children.empty() && expr.children[0].IsReg()) {
    const int regno = expr.children[0].RegNo();
    if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
      const RegMask m = RegBitMask(regno, expr.mode, gp64, fp64);
      out->uses |= m;
      out->use_modes[regno] = expr.mode;
      if (expr.children[0].flags.integrated) {
        out->integrated_ret_regs |= (m & kRetCandMask);
      }
    }
    return;
  }
  for (const RtlExpr& child : expr.children) {
    CollectReadRegs(child, gp64, fp64, out);
  }
}

void AnalyzePatternAst(const RtlExpr& pat, bool gp64, bool fp64, ElemSummary* out) {
  if (pat.op == RtlOp::kAddrVec || pat.op == RtlOp::kAddrDiffVec || pat.op == RtlOp::kNote) {
    return;
  }
  if (pat.IsParallel()) {
    for (const RtlExpr& child : pat.children) {
      AnalyzePatternAst(child, gp64, fp64, out);
    }
    return;
  }
  if (pat.IsSet()) {
    const RtlExpr& dst = pat.SetDest();
    const RtlExpr& src = pat.SetSrc();
    CollectReadRegs(src, gp64, fp64, out);
    if (dst.IsReg()) {
      const int regno = dst.RegNo();
      if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
        const RegMask m = RegBitMask(regno, dst.mode, gp64, fp64);
        out->defs |= m;
        out->explicit_sets |= m;
        out->def_modes[regno] = dst.mode;
        if (dst.flags.integrated) {
          out->integrated_ret_regs |= (m & kRetCandMask);
        }
      }
    } else if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg()) {
      const int regno = dst.children[0].RegNo();
      if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
        const RegMask m = RegBitMask(regno, dst.mode, gp64, fp64);
        out->defs |= m;
        out->explicit_sets |= m;
        out->def_modes[regno] = dst.mode;
        if (dst.children[0].flags.integrated) {
          out->integrated_ret_regs |= (m & kRetCandMask);
        }
      }
    } else {
      CollectReadRegs(dst, gp64, fp64, out);
    }
    return;
  }
  if (pat.op == RtlOp::kClobber) {
    if (!pat.children.empty()) {
      const RtlExpr* dst = &pat.children[0];
      RtlMachineMode mode = dst->mode;
      if (dst->IsSubreg() && !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (dst->IsReg()) {
        const int regno = dst->RegNo();
        if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
          out->defs |= RegBitMask(regno, mode, gp64, fp64);
          out->def_modes[regno] = mode;
        }
      }
    }
    return;
  }
  if (pat.op == RtlOp::kUse || pat.op == RtlOp::kCall || pat.op == RtlOp::kReturn) {
    CollectReadRegs(pat, gp64, fp64, out);
  }
}

ElemSummary SummarizeSingleElem(const RtlPatternAndNotes& pn, bool force_call, bool gp64, bool fp64,
                                RegMask* pending_arg_defs) {
  ElemSummary s;
  s.use_modes.fill(RtlMachineMode::kSi);
  s.def_modes.fill(RtlMachineMode::kSi);
  const RtlExpr& pat = pn.pattern;
  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber) {
    s.is_note_or_pseudo = true;
  }
  s.is_call = force_call || IsCallPattern(pat);
  AnalyzePatternAst(pat, gp64, fp64, &s);

  if (s.is_call) {
    s.uses |= *pending_arg_defs;
    for (const RtlRegNote& note : pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
        const int r = note.datum.RegNo();
        const RegMask m = RegBitMask(r, note.datum.mode, gp64, fp64) & kArgRegMask;
        s.uses |= m;
      }
    }
    s.defs |= kCallClobberedMask;
    *pending_arg_defs = 0;
  } else if (pat.op != RtlOp::kNote) {
    *pending_arg_defs &= ~(s.defs & ~s.explicit_sets);
    *pending_arg_defs |= (s.explicit_sets & kArgRegMask);
  }
  return s;
}

struct InsnSummary {
  bool valid = false;
  bool is_sequence = false;
  bool is_call_seq = false;
  bool is_annulled_target_seq = false;
  RtlPatternAndNotes top_pn;
  RtlExpr seq_head_wrap;
  RtlExpr seq_slot_wrap;
  RtlPatternAndNotes head_pn;
  RtlPatternAndNotes slot_pn;
  ElemSummary single;
  ElemSummary head;
  ElemSummary slot;
};

struct DataflowResult {
  bool gp64 = false;
  bool fp64 = false;
  RegMask live_at_exit = 0;
  ElemSummary epilogue_delay;
  bool has_epilogue_delay = false;
  std::vector<bool> can_exit;
  std::vector<bool> uses_epilogue_delay_at_exit;
  std::vector<RegMask> live_in;
  std::vector<RegMask> live_out;
  std::vector<std::vector<InsnSummary>> block_summaries;
};

DataflowResult RunHardRegDataflow(const RtlControlFlowGraph& cfg,
                                  const CompilationProfile* profile) {
  DataflowResult res;
  DetectRegisterWidthFlags(cfg, profile, &res.gp64, &res.fp64);
  const size_t n_blocks = cfg.rpo_blocks.size();
  res.can_exit.assign(n_blocks, false);
  res.uses_epilogue_delay_at_exit.assign(n_blocks, false);
  res.live_in.assign(n_blocks, 0);
  res.live_out.assign(n_blocks, 0);
  res.block_summaries.resize(n_blocks);

  RegMask any_integrated_ret = 0;
  if (cfg.sideband.epilogue_delay_pattern.has_value() &&
      !cfg.sideband.epilogue_delay_pattern->empty()) {
    auto ep_pn = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
    if (ep_pn.ok()) {
      RegMask dummy_args = 0;
      res.epilogue_delay =
          SummarizeSingleElem(*ep_pn, /*force_call=*/false, res.gp64, res.fp64, &dummy_args);
      res.has_epilogue_delay = true;
      any_integrated_ret |= res.epilogue_delay.integrated_ret_regs;
    }
  }

  for (size_t b = 0; b < n_blocks; ++b) {
    const RtlBasicBlock& blk = cfg.rpo_blocks[b];
    res.can_exit[b] = blk.successors.empty();
    bool ends_with_ret_insn = false;
    if (!blk.instructions.empty()) {
      auto last_p = ParsePatternAndNotes(blk.instructions.back());
      if (last_p.ok() && IsReturnJumpPattern(last_p->pattern)) {
        ends_with_ret_insn = true;
        res.can_exit[b] = true;
      }
    }
    res.uses_epilogue_delay_at_exit[b] =
        res.can_exit[b] && !ends_with_ret_insn && res.has_epilogue_delay;

    RegMask pending_arg_defs = 0;
    res.block_summaries[b].resize(blk.instructions.size());
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      InsnSummary& isum = res.block_summaries[b][i];
      auto pn = ParsePatternAndNotes(blk.instructions[i]);
      if (!pn.ok()) {
        continue;
      }
      isum.valid = true;
      isum.top_pn = std::move(*pn);
      if (isum.top_pn.pattern.op == RtlOp::kSequence && isum.top_pn.pattern.children.size() == 2) {
        auto h_pn = RtlPatternAndNotes::FromSequenceSubInsn(isum.top_pn.pattern.children[0]);
        auto s_pn = RtlPatternAndNotes::FromSequenceSubInsn(isum.top_pn.pattern.children[1]);
        if (h_pn.ok() && s_pn.ok()) {
          isum.is_sequence = true;
          isum.seq_head_wrap = isum.top_pn.pattern.children[0];
          isum.seq_slot_wrap = isum.top_pn.pattern.children[1];
          isum.head_pn = std::move(*h_pn);
          isum.slot_pn = std::move(*s_pn);
          isum.is_call_seq = (isum.seq_head_wrap.op == RtlOp::kCallInsn);
          isum.is_annulled_target_seq =
              (isum.seq_head_wrap.op == RtlOp::kJumpInsn && isum.seq_head_wrap.flags.unchanging &&
               isum.seq_slot_wrap.flags.in_struct && i + 1 == blk.instructions.size());
          if (isum.is_call_seq) {
            isum.slot = SummarizeSingleElem(isum.slot_pn, /*force_call=*/false, res.gp64, res.fp64,
                                            &pending_arg_defs);
            isum.head = SummarizeSingleElem(isum.head_pn, /*force_call=*/true, res.gp64, res.fp64,
                                            &pending_arg_defs);
          } else {
            isum.head = SummarizeSingleElem(isum.head_pn, /*force_call=*/false, res.gp64, res.fp64,
                                            &pending_arg_defs);
            isum.slot = SummarizeSingleElem(isum.slot_pn, /*force_call=*/false, res.gp64, res.fp64,
                                            &pending_arg_defs);
          }
          any_integrated_ret |= isum.head.integrated_ret_regs | isum.slot.integrated_ret_regs;
          continue;
        }
      }
      isum.single = SummarizeSingleElem(isum.top_pn, /*force_call=*/false, res.gp64, res.fp64,
                                        &pending_arg_defs);
      any_integrated_ret |= isum.single.integrated_ret_regs;
    }
  }

  res.live_at_exit = (RegMask{1} << 29) | (RegMask{1} << 31) | any_integrated_ret;
  if (cfg.sideband.frame_pointer_needed) {
    res.live_at_exit |= (RegMask{1} << 30);
  }

  // Only infer return registers from dataflow in Stage 0 (`profile != nullptr`),
  // where raw assembly does not carry `REG_FUNCTION_VALUE_P` (`reg/i`). In
  // post-reload RTL stages (`profile == nullptr`), `any_integrated_ret` already
  // matches `GccContext::MaterializeFromCfg`'s `current_function_return_rtx`.
  if (profile != nullptr && any_integrated_ret == 0 && n_blocks > 0) {
    std::vector<bool> reachable(n_blocks, false);
    std::vector<size_t> worklist = {0};
    reachable[0] = true;
    while (!worklist.empty()) {
      const size_t u = worklist.back();
      worklist.pop_back();
      for (size_t v : cfg.rpo_blocks[u].successors) {
        if (v < n_blocks && !reachable[v]) {
          reachable[v] = true;
          worklist.push_back(v);
        }
      }
    }

    std::vector<RegMask> live_no_exit_in(n_blocks, 0);
    {
      auto has_ft_edge = [&](size_t b) -> bool {
        if (b + 1 >= n_blocks) return false;
        const auto& succs = cfg.rpo_blocks[b].successors;
        return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
      };
      bool changed_no_exit = true;
      while (changed_no_exit) {
        changed_no_exit = false;
        for (size_t rev = n_blocks; rev > 0; --rev) {
          const size_t b = rev - 1;
          RegMask up = 0;
          for (size_t sb : cfg.rpo_blocks[b].successors) {
            if (sb < n_blocks) {
              up |= live_no_exit_in[sb];
            }
          }
          for (auto it = res.block_summaries[b].rbegin(); it != res.block_summaries[b].rend();
               ++it) {
            if (!it->valid) continue;
            if (it->is_sequence) {
              if (it->is_call_seq) {
                up = (up & ~it->head.defs) | it->head.uses;
                up = (up & ~it->slot.defs) | it->slot.uses;
              } else if (it->is_annulled_target_seq && has_ft_edge(b)) {
                RegMask taken_up = 0;
                for (size_t sb : cfg.rpo_blocks[b].successors) {
                  if (sb != b + 1 && sb < n_blocks) {
                    taken_up |= live_no_exit_in[sb];
                  }
                }
                const RegMask before_slot =
                    ((taken_up & ~it->slot.defs) | it->slot.uses) | live_no_exit_in[b + 1];
                up = (before_slot & ~it->head.defs) | it->head.uses;
              } else {
                up = (up & ~it->slot.defs) | it->slot.uses;
                up = (up & ~it->head.defs) | it->head.uses;
              }
            } else {
              up = (up & ~it->single.defs) | it->single.uses;
            }
          }
          up &= ~RegMask{1};
          if (up != live_no_exit_in[b]) {
            live_no_exit_in[b] = up;
            changed_no_exit = true;
          }
        }
      }
    }

    auto step_def = [](const ElemSummary& e, RegMask state) -> RegMask {
      state &= ~(e.defs & ~e.explicit_sets);
      state |= (e.explicit_sets & kRetCandMask);
      return state;
    };
    auto step_unread_any = [](const ElemSummary& e, RegMask state) -> RegMask {
      state &= ~(e.uses | e.defs);
      state |= (e.explicit_sets & kRetCandMask);
      return state;
    };
    auto step_unread_noncall = [](const ElemSummary& e, RegMask state) -> RegMask {
      state &= ~(e.uses | e.defs);
      if (!e.is_call) {
        state |= (e.explicit_sets & kRetCandMask);
      }
      return state;
    };

    std::vector<RegMask> must_def_in(n_blocks, kRetCandMask);
    std::vector<RegMask> must_unread_in(n_blocks, kRetCandMask);
    std::vector<RegMask> may_noncall_in(n_blocks, 0);
    must_def_in[0] = 0;
    must_unread_in[0] = 0;

    std::vector<RegMask> must_def_exit(n_blocks, kRetCandMask);
    std::vector<RegMask> must_unread_exit(n_blocks, kRetCandMask);
    std::vector<RegMask> may_noncall_exit(n_blocks, 0);

    std::vector<bool> has_pred_init(n_blocks, false);
    has_pred_init[0] = true;

    bool changed_fwd = true;
    while (changed_fwd) {
      changed_fwd = false;
      std::vector<RegMask> next_def_in(n_blocks, kRetCandMask);
      std::vector<RegMask> next_unread_in(n_blocks, kRetCandMask);
      std::vector<RegMask> next_noncall_in = may_noncall_in;
      std::vector<bool> next_has_pred(n_blocks, false);
      next_def_in[0] = 0;
      next_unread_in[0] = 0;
      next_has_pred[0] = true;

      for (size_t b = 0; b < n_blocks; ++b) {
        if (!reachable[b] || !has_pred_init[b]) {
          continue;
        }
        RegMask cur_def = must_def_in[b];
        RegMask cur_unread = must_unread_in[b];
        RegMask cur_noncall = may_noncall_in[b];
        const size_t n_insns = res.block_summaries[b].size();
        const bool is_cond_branch_blk = (cfg.rpo_blocks[b].successors.size() >= 2);

        RegMask taken_def = 0;
        RegMask taken_unread = 0;
        RegMask taken_noncall = 0;
        RegMask ft_def = 0;
        RegMask ft_unread = 0;
        RegMask ft_noncall = 0;
        bool split_branch_edges = false;

        for (size_t i = 0; i < n_insns; ++i) {
          const InsnSummary& isum = res.block_summaries[b][i];
          if (!isum.valid) continue;
          if (isum.is_sequence && !isum.is_call_seq && is_cond_branch_blk && i + 1 == n_insns) {
            const RegMask h_def = step_def(isum.head, cur_def);
            const RegMask h_unread = step_unread_any(isum.head, cur_unread);
            const RegMask h_noncall = step_unread_noncall(isum.head, cur_noncall);
            if (isum.is_annulled_target_seq) {
              ft_def = h_def;
              ft_unread = h_unread;
              ft_noncall = h_noncall;
              taken_def = step_def(isum.slot, h_def);
              taken_unread = step_unread_any(isum.slot, h_unread);
              taken_noncall = step_unread_noncall(isum.slot, h_noncall);
            } else {
              const RegMask raw_def = step_def(isum.slot, h_def);
              const RegMask raw_unread = step_unread_any(isum.slot, h_unread);
              const RegMask raw_noncall = step_unread_noncall(isum.slot, h_noncall);
              RegMask taken_live_no_exit = 0;
              for (size_t sb : cfg.rpo_blocks[b].successors) {
                if (sb != b + 1 && sb < n_blocks) {
                  taken_live_no_exit |= live_no_exit_in[sb];
                }
              }
              const RegMask ft_live_no_exit = (b + 1 < n_blocks) ? live_no_exit_in[b + 1] : 0;
              const RegMask stolen_ft =
                  isum.slot.explicit_sets & ft_live_no_exit & ~taken_live_no_exit;
              const RegMask stolen_tgt =
                  isum.slot.explicit_sets & isum.head.uses & taken_live_no_exit & ~ft_live_no_exit;
              ft_def = (raw_def & ~stolen_tgt) | (h_def & ~isum.slot.defs);
              ft_unread = raw_unread & ~stolen_tgt;
              ft_noncall = raw_noncall & ~stolen_tgt;
              taken_def = (raw_def & ~stolen_ft) | (h_def & ~isum.slot.defs);
              taken_unread = raw_unread & ~stolen_ft;
              taken_noncall = raw_noncall & ~stolen_ft;
            }
            cur_def = ft_def;
            cur_unread = ft_unread;
            cur_noncall = ft_noncall;
            split_branch_edges = true;
            break;
          }
          if (isum.is_sequence) {
            if (isum.is_call_seq) {
              cur_def = step_def(isum.head, step_def(isum.slot, cur_def));
              cur_unread = step_unread_any(isum.head, step_unread_any(isum.slot, cur_unread));
              cur_noncall =
                  step_unread_noncall(isum.head, step_unread_noncall(isum.slot, cur_noncall));
            } else {
              cur_def = step_def(isum.slot, step_def(isum.head, cur_def));
              cur_unread = step_unread_any(isum.slot, step_unread_any(isum.head, cur_unread));
              cur_noncall =
                  step_unread_noncall(isum.slot, step_unread_noncall(isum.head, cur_noncall));
            }
          } else {
            cur_def = step_def(isum.single, cur_def);
            cur_unread = step_unread_any(isum.single, cur_unread);
            cur_noncall = step_unread_noncall(isum.single, cur_noncall);
          }
        }

        must_def_exit[b] = cur_def;
        must_unread_exit[b] = cur_unread;
        may_noncall_exit[b] = cur_noncall;

        for (size_t sb : cfg.rpo_blocks[b].successors) {
          if (sb >= n_blocks) continue;
          const bool is_ft = (sb == b + 1);
          const RegMask e_def = split_branch_edges ? (is_ft ? ft_def : taken_def) : cur_def;
          const RegMask e_unread =
              split_branch_edges ? (is_ft ? ft_unread : taken_unread) : cur_unread;
          const RegMask e_noncall =
              split_branch_edges ? (is_ft ? ft_noncall : taken_noncall) : cur_noncall;
          if (sb != 0) {
            next_def_in[sb] &= e_def;
            next_unread_in[sb] &= e_unread;
            next_has_pred[sb] = true;
          }
          next_noncall_in[sb] |= e_noncall;
        }
      }

      for (size_t b = 0; b < n_blocks; ++b) {
        if (!reachable[b]) continue;
        if (next_has_pred[b] != has_pred_init[b] || next_def_in[b] != must_def_in[b] ||
            next_unread_in[b] != must_unread_in[b] || next_noncall_in[b] != may_noncall_in[b]) {
          has_pred_init[b] = next_has_pred[b];
          must_def_in[b] = next_def_in[b];
          must_unread_in[b] = next_unread_in[b];
          may_noncall_in[b] = next_noncall_in[b];
          changed_fwd = true;
        }
      }
    }

    RegMask all_exits_def = kRetCandMask;
    RegMask all_exits_unread = kRetCandMask;
    RegMask any_exit_noncall = 0;
    bool saw_reachable_exit = false;
    for (size_t b = 0; b < n_blocks; ++b) {
      if (!reachable[b] || !res.can_exit[b]) {
        continue;
      }
      saw_reachable_exit = true;
      RegMask ex_def = must_def_exit[b];
      RegMask ex_unread = must_unread_exit[b];
      RegMask ex_noncall = may_noncall_exit[b];
      if (res.uses_epilogue_delay_at_exit[b]) {
        ex_def = step_def(res.epilogue_delay, ex_def);
        ex_unread = step_unread_any(res.epilogue_delay, ex_unread);
        ex_noncall = step_unread_noncall(res.epilogue_delay, ex_noncall);
      }
      all_exits_def &= ex_def;
      all_exits_unread &= ex_unread;
      any_exit_noncall |= ex_noncall;
    }

    if (saw_reachable_exit) {
      RegMask inferred = ((all_exits_def & any_exit_noncall) | all_exits_unread) & kRetCandMask;
      if ((inferred & (RegMask{1} << 32)) != 0) {
        inferred &= ~((RegMask{1} << 2) | (RegMask{1} << 3));
      } else {
        inferred &= ~(RegMask{1} << 33);
      }
      if ((inferred & (RegMask{1} << 2)) == 0) {
        inferred &= ~(RegMask{1} << 3);
      }
      res.live_at_exit |= inferred;
    }
  }

  std::vector<RegMask> may_ret_def_in(n_blocks, 0);
  std::vector<RegMask> may_ret_def_exit(n_blocks, 0);
  if ((res.live_at_exit & kRetCandMask) != 0 && n_blocks > 0) {
    bool changed_may = true;
    while (changed_may) {
      changed_may = false;
      for (size_t b = 0; b < n_blocks; ++b) {
        RegMask cur = may_ret_def_in[b];
        RegMask taken_may = 0;
        RegMask ft_may = 0;
        bool split_edges = false;
        const size_t n_insns = res.block_summaries[b].size();
        const bool is_cond = (cfg.rpo_blocks[b].successors.size() >= 2);
        for (size_t i = 0; i < n_insns; ++i) {
          const InsnSummary& isum = res.block_summaries[b][i];
          if (!isum.valid) continue;
          if (isum.is_sequence && !isum.is_call_seq && is_cond && i + 1 == n_insns &&
              isum.is_annulled_target_seq) {
            ft_may = cur | (isum.head.explicit_sets & kRetCandMask);
            taken_may = ft_may | (isum.slot.explicit_sets & kRetCandMask);
            cur = ft_may;
            split_edges = true;
            break;
          }
          if (isum.is_sequence) {
            cur |= ((isum.head.explicit_sets | isum.slot.explicit_sets) & kRetCandMask);
          } else {
            cur |= (isum.single.explicit_sets & kRetCandMask);
          }
        }
        RegMask ex_may = cur;
        if (res.uses_epilogue_delay_at_exit[b]) {
          ex_may |= (res.epilogue_delay.explicit_sets & kRetCandMask);
        }
        if (ex_may != may_ret_def_exit[b]) {
          may_ret_def_exit[b] = ex_may;
          changed_may = true;
        }
        for (size_t sb : cfg.rpo_blocks[b].successors) {
          if (sb >= n_blocks) continue;
          const RegMask edge_may = split_edges ? ((sb == b + 1) ? ft_may : taken_may) : cur;
          if ((may_ret_def_in[sb] | edge_may) != may_ret_def_in[sb]) {
            may_ret_def_in[sb] |= edge_may;
            changed_may = true;
          }
        }
      }
    }
  }

  auto exit_live_for_block = [&](size_t b) -> RegMask {
    if (!res.can_exit[b]) {
      return 0;
    }
    const RegMask base_exit_live = (res.live_at_exit & ~kRetCandMask) |
                                   (res.live_at_exit & kRetCandMask & may_ret_def_exit[b]);
    if (res.uses_epilogue_delay_at_exit[b]) {
      return (base_exit_live & ~res.epilogue_delay.defs) | res.epilogue_delay.uses;
    }
    return base_exit_live;
  };

  auto has_fallthrough_edge = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg.rpo_blocks[b].successors;
    return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
  };

  auto backward_transfer = [&](const InsnSummary& isum, size_t b, RegMask after_live) -> RegMask {
    if (!isum.valid) {
      return after_live;
    }
    if (isum.is_sequence) {
      if (isum.is_call_seq) {
        const RegMask before_call = (after_live & ~isum.head.defs) | isum.head.uses;
        return (before_call & ~isum.slot.defs) | isum.slot.uses;
      }
      if (isum.is_annulled_target_seq && has_fallthrough_edge(b)) {
        RegMask taken_live = exit_live_for_block(b);
        for (size_t sb : cfg.rpo_blocks[b].successors) {
          if (sb != b + 1 && sb < n_blocks) {
            taken_live |= res.live_in[sb];
          }
        }
        const RegMask ft_live = res.live_in[b + 1];
        const RegMask before_slot = ((taken_live & ~isum.slot.defs) | isum.slot.uses) | ft_live;
        return (before_slot & ~isum.head.defs) | isum.head.uses;
      }
      const RegMask before_slot = (after_live & ~isum.slot.defs) | isum.slot.uses;
      return (before_slot & ~isum.head.defs) | isum.head.uses;
    }
    return (after_live & ~isum.single.defs) | isum.single.uses;
  };

  bool changed_bwd = true;
  while (changed_bwd) {
    changed_bwd = false;
    for (size_t rev = n_blocks; rev > 0; --rev) {
      const size_t b = rev - 1;
      RegMask out_mask = exit_live_for_block(b);
      for (size_t sb : cfg.rpo_blocks[b].successors) {
        if (sb < n_blocks) {
          out_mask |= res.live_in[sb];
        }
      }
      res.live_out[b] = out_mask;
      RegMask cur = out_mask;
      for (auto it = res.block_summaries[b].rbegin(); it != res.block_summaries[b].rend(); ++it) {
        cur = backward_transfer(*it, b, cur);
      }
      cur &= ~RegMask{1};  // `$0` is hardwired zero.
      if (cur != res.live_in[b]) {
        res.live_in[b] = cur;
        changed_bwd = true;
      }
    }
  }

  return res;
}

void WriteLiveInToSideband(const DataflowResult& df, RtlControlFlowGraph* cfg) {
  cfg->sideband.basic_block_live_at_start.clear();
  for (size_t b = 0; b < df.live_in.size(); ++b) {
    const RegMask mask = df.live_in[b];
    if (mask == 0) {
      continue;
    }
    auto& dst_set = cfg->sideband.basic_block_live_at_start[static_cast<int>(b)];
    for (int r = 1; r < kMipsFirstPseudoRegister; ++r) {
      if ((mask & (RegMask{1} << r)) != 0) {
        dst_set.insert(r);
      }
    }
  }
}

bool MarkExitReachingReturnSetsInExpr(RtlExpr* pat, RegMask needed_ret, bool gp64, bool fp64) {
  if (needed_ret == 0) {
    return false;
  }
  bool changed = false;
  auto mark_one_set = [&](RtlExpr* elem) {
    if (!elem->IsSet()) {
      return;
    }
    RtlExpr* dst = &elem->MutableSetDest();
    if (dst->IsSubreg() && !dst->children.empty()) {
      dst = &dst->children[0];
    }
    if (dst->IsReg()) {
      const RegMask m = RegBitMask(dst->RegNo(), dst->mode, gp64, fp64);
      if ((m & needed_ret) != 0 && !dst->flags.integrated) {
        dst->flags.integrated = true;
        changed = true;
      }
    }
  };
  if (pat->IsParallel()) {
    for (RtlExpr& child : pat->children) {
      mark_one_set(&child);
    }
  } else {
    mark_one_set(pat);
  }
  return changed;
}

bool HasRegNote(const std::vector<RtlRegNote>& notes, RtlMachineMode kind, int regno) {
  for (const RtlRegNote& n : notes) {
    if (n.kind == kind && n.datum.IsReg() && n.datum.RegNo() == regno) {
      return true;
    }
  }
  return false;
}

size_t CountConstZeroRegDeadNotes(const std::vector<RtlRegNote>& notes) {
  size_t count = 0;
  for (const RtlRegNote& n : notes) {
    if (n.kind == RtlMachineMode::kRegDead && n.datum.IsConstInt() && n.datum.int_val == 0) {
      ++count;
    }
  }
  return count;
}

bool AttachNotesToElem(RtlPatternAndNotes* pn, const ElemSummary& esum, RegMask live_after,
                       RegMask fixed_mask, bool gp64, bool fp64, bool in_delay_slot = false) {
  if (esum.is_call || esum.is_note_or_pseudo) {
    return false;
  }
  bool changed = false;
  const RegMask dead_mask = esum.uses & ~esum.defs & ~live_after & ~fixed_mask;
  const RegMask unused_mask = esum.defs & ~live_after & ~fixed_mask;
  if (in_delay_slot) {
    // In `reorg.c:emit_delay_sequence` (`reorg.c:915-920`), every `REG_DEAD` note on a
    // `SEQUENCE` delay-slot instruction has its datum overwritten with `const0_rtx`
    // (`(const_int 0)`).
    size_t expected_dead_count = 0;
    for (int r = 1; r < kMipsFirstPseudoRegister;) {
      if ((dead_mask & (RegMask{1} << r)) != 0) {
        ++expected_dead_count;
        r += HardRegNregs(r, esum.use_modes[r], gp64, fp64);
      } else {
        ++r;
      }
    }
    size_t existing_zero_dead = CountConstZeroRegDeadNotes(pn->reg_notes);
    while (existing_zero_dead < expected_dead_count) {
      pn->reg_notes.push_back(RtlRegNote{
          .kind = RtlMachineMode::kRegDead,
          .list_op = RtlOp::kExprList,
          .datum = RtlExpr::MakeConstInt(0),
      });
      ++existing_zero_dead;
      changed = true;
    }
  } else {
    for (int r = 1; r < kMipsFirstPseudoRegister;) {
      if ((dead_mask & (RegMask{1} << r)) != 0) {
        const RtlMachineMode mode = esum.use_modes[r];
        if (!HasRegNote(pn->reg_notes, RtlMachineMode::kRegDead, r)) {
          pn->reg_notes.push_back(RtlRegNote{
              .kind = RtlMachineMode::kRegDead,
              .list_op = RtlOp::kExprList,
              .datum = RtlExpr::MakeReg(mode, r),
          });
          changed = true;
        }
        r += HardRegNregs(r, mode, gp64, fp64);
      } else {
        ++r;
      }
    }
  }
  for (int r = 1; r < kMipsFirstPseudoRegister;) {
    if ((unused_mask & (RegMask{1} << r)) != 0) {
      const RtlMachineMode mode = esum.def_modes[r];
      if (!HasRegNote(pn->reg_notes, RtlMachineMode::kRegUnused, r)) {
        pn->reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegUnused,
            .list_op = RtlOp::kExprList,
            .datum = RtlExpr::MakeReg(mode, r),
        });
        changed = true;
      }
      r += HardRegNregs(r, mode, gp64, fp64);
    } else {
      ++r;
    }
  }
  return changed;
}

}  // namespace

void ReconstructPostDbrHardRegLivenessAndReturnUse(RtlControlFlowGraph* cfg,
                                                   const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }

  DataflowResult df = RunHardRegDataflow(*cfg, &profile);
  const bool attach_notes = (profile.optimize_level > 0);
  if (attach_notes) {
    WriteLiveInToSideband(df, cfg);
  } else {
    cfg->sideband.basic_block_live_at_start.clear();
  }

  const RegMask exit_ret_mask = df.live_at_exit & kRetCandMask;
  const size_t n_blocks = cfg->rpo_blocks.size();

  // Backward propagation of exit-reaching return register values (`exit_ret_mask`)
  // so only genuine return-value `SET`s receive `REG_FUNCTION_VALUE_P` (`reg/i`).
  std::vector<RegMask> ret_need_in(n_blocks, 0);
  std::vector<RegMask> ret_need_out(n_blocks, 0);
  if (exit_ret_mask != 0) {
    if (df.has_epilogue_delay && cfg->sideband.epilogue_delay_pattern.has_value()) {
      auto ep_pn = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
      if (ep_pn.ok() &&
          MarkExitReachingReturnSetsInExpr(&ep_pn->pattern, exit_ret_mask, df.gp64, df.fp64)) {
        cfg->sideband.epilogue_delay_pattern = ep_pn->ToGccPrintRtlString();
      }
    }
    bool changed_ret = true;
    while (changed_ret) {
      changed_ret = false;
      for (size_t rev = n_blocks; rev > 0; --rev) {
        const size_t b = rev - 1;
        RegMask out_need = 0;
        if (df.can_exit[b]) {
          out_need |= df.uses_epilogue_delay_at_exit[b] ? (exit_ret_mask & ~df.epilogue_delay.defs)
                                                        : exit_ret_mask;
        }
        for (size_t sb : cfg->rpo_blocks[b].successors) {
          if (sb < n_blocks) {
            out_need |= ret_need_in[sb];
          }
        }
        ret_need_out[b] = out_need;
        RegMask cur_need = out_need;
        for (auto it = df.block_summaries[b].rbegin(); it != df.block_summaries[b].rend(); ++it) {
          if (!it->valid) continue;
          if (it->is_sequence) {
            cur_need &= ~(it->slot.defs | it->head.defs);
          } else {
            cur_need &= ~it->single.defs;
          }
        }
        if (cur_need != ret_need_in[b]) {
          ret_need_in[b] = cur_need;
          changed_ret = true;
        }
      }
    }
  }

  if (exit_ret_mask == 0 && !attach_notes) {
    return;
  }

  const RegMask fixed_mask = (RegMask{1} << 0) | (RegMask{1} << 28) | (RegMask{1} << 29) |
                             (cfg->sideband.frame_pointer_needed ? (RegMask{1} << 30) : RegMask{0});

  auto has_fallthrough_edge = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg->rpo_blocks[b].successors;
    return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
  };

  auto exit_live_for_block = [&](size_t b) -> RegMask {
    if (!df.can_exit[b]) {
      return 0;
    }
    if (df.uses_epilogue_delay_at_exit[b]) {
      return (df.live_at_exit & ~df.epilogue_delay.defs) | df.epilogue_delay.uses;
    }
    return df.live_at_exit;
  };

  for (size_t b = 0; b < n_blocks; ++b) {
    bool block_changed = false;
    RegMask cur_live = df.live_out[b];
    RegMask cur_ret_need = ret_need_out[b];

    for (size_t rev_i = df.block_summaries[b].size(); rev_i > 0; --rev_i) {
      const size_t i = rev_i - 1;
      InsnSummary& isum = df.block_summaries[b][i];
      if (!isum.valid) {
        continue;
      }
      bool insn_changed = false;
      if (isum.is_sequence) {
        if (isum.is_call_seq) {
          if (MarkExitReachingReturnSetsInExpr(&isum.head_pn.pattern, cur_ret_need, df.gp64,
                                               df.fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.head.defs;
          if (MarkExitReachingReturnSetsInExpr(&isum.slot_pn.pattern, cur_ret_need, df.gp64,
                                               df.fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.slot.defs;

          const RegMask before_call = (cur_live & ~isum.head.defs) | isum.head.uses;
          if (attach_notes &&
              AttachNotesToElem(&isum.slot_pn, isum.slot, before_call, fixed_mask, df.gp64, df.fp64,
                                /*in_delay_slot=*/true)) {
            insn_changed = true;
          }
          cur_live = (before_call & ~isum.slot.defs) | isum.slot.uses;
        } else {
          RegMask slot_after_live = cur_live;
          RegMask before_slot = (cur_live & ~isum.slot.defs) | isum.slot.uses;
          RegMask slot_ret_need = cur_ret_need;
          if (isum.is_annulled_target_seq && has_fallthrough_edge(b)) {
            RegMask taken_live = exit_live_for_block(b);
            RegMask taken_ret_need = df.can_exit[b] ? exit_ret_mask : 0;
            for (size_t sb : cfg->rpo_blocks[b].successors) {
              if (sb != b + 1 && sb < n_blocks) {
                taken_live |= df.live_in[sb];
                taken_ret_need |= ret_need_in[sb];
              }
            }
            slot_after_live = taken_live;
            before_slot = ((taken_live & ~isum.slot.defs) | isum.slot.uses) | df.live_in[b + 1];
            slot_ret_need = taken_ret_need;
            cur_ret_need = (taken_ret_need & ~isum.slot.defs) | ret_need_in[b + 1];
          } else {
            cur_ret_need &= ~isum.slot.defs;
          }

          if (MarkExitReachingReturnSetsInExpr(&isum.slot_pn.pattern, slot_ret_need, df.gp64,
                                               df.fp64)) {
            insn_changed = true;
          }
          if (MarkExitReachingReturnSetsInExpr(&isum.head_pn.pattern, cur_ret_need, df.gp64,
                                               df.fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.head.defs;

          if (attach_notes) {
            if (AttachNotesToElem(&isum.slot_pn, isum.slot, slot_after_live, fixed_mask, df.gp64,
                                  df.fp64, /*in_delay_slot=*/true)) {
              insn_changed = true;
            }
            if (AttachNotesToElem(&isum.head_pn, isum.head, before_slot, fixed_mask, df.gp64,
                                  df.fp64, /*in_delay_slot=*/false)) {
              insn_changed = true;
            }
          }
          cur_live = (before_slot & ~isum.head.defs) | isum.head.uses;
        }
        if (insn_changed) {
          cfg->rpo_blocks[b].instructions[i] =
              RtlExpr::MakeSequence(
                  {
                      RtlExpr::MakeInsnWrapper(isum.seq_head_wrap.op, isum.seq_head_wrap.flags,
                                               isum.head_pn),
                      RtlExpr::MakeInsnWrapper(isum.seq_slot_wrap.op, isum.seq_slot_wrap.flags,
                                               isum.slot_pn),
                  })
                  .ToGccPrintRtlString();
          block_changed = true;
        }
      } else {
        if (MarkExitReachingReturnSetsInExpr(&isum.top_pn.pattern, cur_ret_need, df.gp64,
                                             df.fp64)) {
          insn_changed = true;
        }
        cur_ret_need &= ~isum.single.defs;

        if (attach_notes &&
            AttachNotesToElem(&isum.top_pn, isum.single, cur_live, fixed_mask, df.gp64, df.fp64)) {
          insn_changed = true;
        }
        cur_live = (cur_live & ~isum.single.defs) | isum.single.uses;
        if (insn_changed) {
          cfg->rpo_blocks[b].instructions[i] = isum.top_pn.ToGccPrintRtlString();
          block_changed = true;
        }
      }
    }
    if (block_changed) {
      cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
    }
  }

  // Insert the trailing `(use (reg/i:<mode> <regno>))` pseudo-instruction when
  // the function returns a value and the final RPO block can reach exit.
  if (exit_ret_mask != 0 && df.can_exit[n_blocks - 1] && !HasExistingReturnUse(*cfg)) {
    bool saw_r2_di = false;
    bool saw_f0_df = false;
    auto inspect_ret_modes = [&](const RtlExpr& expr) {
      expr.Visit([&](const RtlExpr& node) {
        if (node.IsReg() && node.flags.integrated) {
          if (node.RegNo() == 2 && node.mode == RtlMachineMode::kDi) {
            saw_r2_di = true;
          } else if (node.RegNo() == 32 && node.mode == RtlMachineMode::kDf) {
            saw_f0_df = true;
          }
        }
      });
    };
    if (cfg->sideband.epilogue_delay_pattern.has_value()) {
      auto ep_pn = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
      if (ep_pn.ok()) {
        inspect_ret_modes(ep_pn->pattern);
      }
    }
    for (const auto& blk : cfg->rpo_blocks) {
      for (const std::string& pat : blk.instructions) {
        auto parsed = ParsePatternAndNotes(pat);
        if (parsed.ok()) {
          inspect_ret_modes(parsed->pattern);
        }
      }
    }

    int ret_regno = -1;
    RtlMachineMode ret_mode = RtlMachineMode::kSi;
    if ((exit_ret_mask & ((RegMask{1} << 2) | (RegMask{1} << 3))) != 0) {
      ret_regno = 2;
      ret_mode = (((exit_ret_mask & (RegMask{1} << 3)) != 0) || saw_r2_di) ? RtlMachineMode::kDi
                                                                           : RtlMachineMode::kSi;
    } else if ((exit_ret_mask & ((RegMask{1} << 32) | (RegMask{1} << 33))) != 0) {
      ret_regno = 32;
      ret_mode = (((exit_ret_mask & (RegMask{1} << 33)) != 0) || saw_f0_df) ? RtlMachineMode::kDf
                                                                            : RtlMachineMode::kSf;
    }

    if (ret_regno >= 0) {
      std::string use_sexpr =
          RtlExpr::MakeUse(RtlExpr::MakeReg(ret_mode, ret_regno, RtlFlags{.integrated = true}))
              .ToGccPrintRtlString();
      RtlBasicBlock& last_blk = cfg->rpo_blocks[n_blocks - 1];
      bool ends_with_ret_jump = false;
      bool ends_with_ret_seq = false;
      if (!last_blk.instructions.empty()) {
        auto last_parsed = ParsePatternAndNotes(last_blk.instructions.back());
        ends_with_ret_jump = last_parsed.ok() && IsReturnJumpPattern(last_parsed->pattern);
        ends_with_ret_seq = ends_with_ret_jump && last_parsed->pattern.op == RtlOp::kSequence;
      }
      bool inserted_use = false;
      if (ends_with_ret_jump && !ends_with_ret_seq && last_blk.has_label) {
        // In `reorg.c:find_end_label` (`reorg.c:820-834`), a shared `end_of_function_label`
        // before a leaf `RETURN` is emitted AFTER the function's trailing `(use (reg/i:...))`,
        // so `last_blk` (`end_of_function_label:`) must never begin with `(use ...)` ahead of
        // `RETURN` (otherwise `reorg.c:get_label_before(next_active_insn(end_label))` splits
        // `end_of_function_label` during forward replay). Place `(use ...)` on the fallthrough
        // into `last_blk` when `n_blocks - 2` falls through to `last_blk`.
        if (n_blocks >= 2) {
          RtlBasicBlock& prev_blk = cfg->rpo_blocks[n_blocks - 2];
          const bool has_edge_to_last =
              std::find(prev_blk.successors.begin(), prev_blk.successors.end(), n_blocks - 1) !=
              prev_blk.successors.end();
          bool prev_ends_with_jump = false;
          if (!prev_blk.instructions.empty()) {
            auto prev_last = ParsePatternAndNotes(prev_blk.instructions.back());
            prev_ends_with_jump = prev_last.ok() && IsAnyControlTransferPattern(prev_last->pattern);
          }
          if (has_edge_to_last && prev_blk.successors.size() == 1 && !prev_ends_with_jump) {
            prev_blk.instructions.push_back(std::move(use_sexpr));
            prev_blk.SyncRtlInsnsFromInstructions();
            inserted_use = true;
          } else if (has_edge_to_last && prev_blk.addr_vec_targets.empty() &&
                     prev_blk.successors.size() >= 2 &&
                     !(prev_blk.successors.size() == 2 &&
                       prev_blk.successors[0] == prev_blk.successors[1])) {
            const size_t insert_idx = n_blocks - 1;
            RtlBasicBlock use_blk;
            use_blk.id = static_cast<uint32_t>(insert_idx);
            use_blk.has_label = false;
            use_blk.instructions = {std::move(use_sexpr)};
            use_blk.successors = {insert_idx + 1};
            use_blk.SyncRtlInsnsFromInstructions();
            cfg->rpo_blocks.insert(cfg->rpo_blocks.begin() + static_cast<ptrdiff_t>(insert_idx),
                                   std::move(use_blk));
            cfg->rpo_blocks[insert_idx + 1].id = static_cast<uint32_t>(insert_idx + 1);
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
            auto& cond_succs = cfg->rpo_blocks[insert_idx - 1].successors;
            if (!cond_succs.empty()) {
              if (cond_succs[0] == insert_idx + 1 && cond_succs.back() != insert_idx + 1) {
                cond_succs[0] = insert_idx;
              } else {
                cond_succs.back() = insert_idx;
              }
            }
            inserted_use = true;
          }
        }
      } else if (ends_with_ret_jump) {
        last_blk.instructions.insert(last_blk.instructions.end() - 1, std::move(use_sexpr));
        last_blk.SyncRtlInsnsFromInstructions();
        inserted_use = true;
      } else {
        last_blk.instructions.push_back(std::move(use_sexpr));
        last_blk.SyncRtlInsnsFromInstructions();
        inserted_use = true;
      }

      if (inserted_use && attach_notes) {
        const DataflowResult updated_df = RunHardRegDataflow(*cfg, &profile);
        WriteLiveInToSideband(updated_df, cfg);
      }
    }
  }
}

void RecomputePostReloadLiveAtStart(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr || cfg->rpo_blocks.empty() ||
      static_cast<int>(cfg->pass) < static_cast<int>(RtlPassKind::kGreg)) {
    return;
  }
  for (const auto& [block_idx, live_set] : cfg->sideband.basic_block_live_at_start) {
    (void)block_idx;
    for (int regno : live_set) {
      if (regno >= kMipsFirstPseudoRegister) {
        return;
      }
    }
  }
  bool has_pseudo = false;
  bool has_optimized_indicator = cfg->sideband.epilogue_delay_pattern.has_value() &&
                                 !cfg->sideband.epilogue_delay_pattern->empty();
  auto check_pseudo = [&has_pseudo](const RtlExpr& node) {
    if (node.IsReg() && node.RegNo() >= kMipsFirstPseudoRegister) {
      has_pseudo = true;
    }
  };
  auto check_notes = [&has_optimized_indicator](const std::vector<RtlRegNote>& notes) {
    for (const RtlRegNote& note : notes) {
      if (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) {
        has_optimized_indicator = true;
      }
    }
  };
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    auto ep_parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (ep_parsed.ok()) {
      ep_parsed->pattern.Visit(check_pseudo);
      check_notes(ep_parsed->reg_notes);
    }
    if (has_pseudo) {
      return;
    }
  }
  for (const auto& blk : cfg->rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok()) {
        continue;
      }
      parsed->pattern.Visit(check_pseudo);
      check_notes(parsed->reg_notes);
      if (parsed->pattern.op == RtlOp::kSequence ||
          (parsed->pattern.IsParallel() && IsReturnJumpPattern(parsed->pattern))) {
        has_optimized_indicator = true;
      }
      if (parsed->pattern.op == RtlOp::kSequence) {
        for (const RtlExpr& sub : parsed->pattern.children) {
          auto sub_pn = RtlPatternAndNotes::FromSequenceSubInsn(sub);
          if (sub_pn.ok()) {
            check_notes(sub_pn->reg_notes);
          }
        }
      }
      if (has_pseudo) {
        return;
      }
    }
  }
  if (cfg->sideband.basic_block_live_at_start.empty() && !has_optimized_indicator) {
    return;
  }
  const DataflowResult df = RunHardRegDataflow(*cfg, /*profile=*/nullptr);
  WriteLiveInToSideband(df, cfg);
}

}  // namespace rom_nom_nom::lifter
