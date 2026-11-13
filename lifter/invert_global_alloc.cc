#include "lifter/invert_global_alloc.h"

#include <algorithm>
#include <cstddef>
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

constexpr int kFramePointerRegnum = 30;
constexpr int kFirstFprRegnum = 32;
constexpr int kLastFprRegnum = 63;
constexpr int kHiRegnum = 64;
constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;

// Recursively validates all `(reg ...)` nodes in `expr` and updates `max_regno`.
absl::Status ValidateRtlExprRegisters(const RtlExpr& expr, int* max_regno) {
  if (expr.op == RtlOp::kReg) {
    if (expr.int_val < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid negative register number %d", expr.int_val));
    }
    if (max_regno != nullptr) {
      *max_regno = std::max(*max_regno, expr.RegNo() + 1);
    }
  }
  for (const RtlExpr& child : expr.children) {
    if (absl::Status st = ValidateRtlExprRegisters(child, max_regno); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

// Collects all pseudo-register numbers (`>= kFirstPseudoRegister`) read inside
// `expr`.
void CollectUsedPseudoRegsInExpr(const RtlExpr& expr, absl::flat_hash_set<int>* used_pseudos) {
  if (expr.op == RtlOp::kReg) {
    if (expr.RegNo() >= kFirstPseudoRegister) {
      used_pseudos->insert(expr.RegNo());
    }
    return;
  }
  for (const RtlExpr& child : expr.children) {
    CollectUsedPseudoRegsInExpr(child, used_pseudos);
  }
}

// Collects pseudo-register numbers (`>= kFirstPseudoRegister`) read vs. written
// by a single top-level pattern or `PARALLEL` element, matching
// `flow.c:mark_set_regs` and `flow.c:mark_used_regs`.
void CollectInsnElementUsesAndDefs(const RtlExpr& elem, absl::flat_hash_set<int>* used_pseudos,
                                   absl::flat_hash_set<int>* defined_pseudos) {
  if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
    const RtlExpr& dst = elem.children[0];
    // Unwrap `SUBREG` / `STRICT_LOW_PART` / `ZERO_EXTRACT` / `SIGN_EXTRACT` on
    // the destination just as `flow.c:mark_set_1` does.
    const RtlExpr* inner_dst = &dst;
    bool is_bitfield_extract = false;
    while (inner_dst->op == RtlOp::kSubreg || inner_dst->op == RtlOp::kStrictLowPart ||
           inner_dst->op == RtlOp::kZeroExtract || inner_dst->op == RtlOp::kSignExtract) {
      if (inner_dst->op == RtlOp::kZeroExtract || inner_dst->op == RtlOp::kSignExtract) {
        is_bitfield_extract = true;
        for (size_t i = 1; i < inner_dst->children.size(); ++i) {
          CollectUsedPseudoRegsInExpr(inner_dst->children[i], used_pseudos);
        }
      }
      if (inner_dst->children.empty()) {
        break;
      }
      inner_dst = &inner_dst->children[0];
    }

    if (inner_dst->op == RtlOp::kReg) {
      if (inner_dst->RegNo() >= kFirstPseudoRegister) {
        defined_pseudos->insert(inner_dst->RegNo());
        if (is_bitfield_extract) {
          used_pseudos->insert(inner_dst->RegNo());
        }
      }
    } else {
      // Destination is a `(mem ...)` or other non-register expression: any
      // registers inside its address are read (`flow.c:mark_used_regs`).
      CollectUsedPseudoRegsInExpr(dst, used_pseudos);
    }

    for (size_t i = 1; i < elem.children.size(); ++i) {
      CollectUsedPseudoRegsInExpr(elem.children[i], used_pseudos);
    }
    return;
  }

  if (elem.op == RtlOp::kParallel) {
    for (const RtlExpr& sub : elem.children) {
      CollectInsnElementUsesAndDefs(sub, used_pseudos, defined_pseudos);
    }
    return;
  }

  CollectUsedPseudoRegsInExpr(elem, used_pseudos);
}

struct PseudoLivenessSummary {
  absl::flat_hash_set<size_t> blocks_referenced;
  bool has_set_in_block = false;
  bool used_before_set = false;
  bool alive_after_redef = false;
  bool defined_by_di_mult = false;
  int def_count_in_block = 0;
  size_t def_insn_idx = 0;
  size_t death_insn_idx = 0;
  int note_deaths = 0;
};

void CollectDiMultDestRegs(const RtlExpr& pat, absl::flat_hash_set<int>* mult_di_dests) {
  auto check_elem = [&](const RtlExpr& elem) {
    if (elem.op == RtlOp::kSet && elem.children.size() == 2 && elem.children[0].op == RtlOp::kReg &&
        elem.children[0].mode == RtlMachineMode::kDi && elem.children[1].op == RtlOp::kMult &&
        elem.children[1].mode == RtlMachineMode::kDi &&
        elem.children[0].RegNo() >= kFirstPseudoRegister) {
      mult_di_dests->insert(elem.children[0].RegNo());
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& sub : pat.children) {
      check_elem(sub);
    }
  } else {
    check_elem(pat);
  }
}

bool InsnTouchesMdRegsOrCall(const RtlExpr& pat) {
  bool touches = false;
  pat.Visit([&](const RtlExpr& expr) {
    if (touches) {
      return;
    }
    if (expr.op == RtlOp::kCall || expr.op == RtlOp::kMult || expr.op == RtlOp::kDiv ||
        expr.op == RtlOp::kMod || expr.op == RtlOp::kUdiv || expr.op == RtlOp::kUmod) {
      touches = true;
      return;
    }
    if (expr.op == RtlOp::kReg && expr.RegNo() >= kHiRegnum && expr.RegNo() <= kHiRegnum + 2) {
      touches = true;
    }
  });
  return touches;
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg) {
  if (greg_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertGlobalAlloc: input CFG has no basic blocks");
  }

  const size_t num_blocks = greg_cfg.rpo_blocks.size();
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = greg_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: block %zu has out-of-bounds successor %zu (num_blocks=%zu)", b,
            succ, num_blocks));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: block %zu has out-of-bounds addr_vec target %zu (num_blocks=%zu)",
            b, target, num_blocks));
      }
    }
  }

  int max_seen_regno = std::max(greg_cfg.sideband.max_regno, kFirstPseudoRegister);

  for (const auto& [regno, hard_reg] : greg_cfg.sideband.reg_renumber) {
    if (regno < kFirstPseudoRegister || hard_reg < -1 || hard_reg >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: invalid sideband reg_renumber entry (%d -> %d)", regno, hard_reg));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_refs) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_refs", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_sets) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_sets", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_deaths) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_deaths", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_live_length) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative regno %d in sideband reg_live_length", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_calls_crossed) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative regno %d in sideband reg_n_calls_crossed", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }

  absl::flat_hash_set<int> live_at_any_block_start;
  for (const auto& [bb_idx, live_set] : greg_cfg.sideband.basic_block_live_at_start) {
    if (bb_idx < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative block index %d in sideband basic_block_live_at_start",
          bb_idx));
    }
    for (int regno : live_set) {
      if (regno < 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: negative regno %d in sideband basic_block_live_at_start", regno));
      }
      max_seen_regno = std::max(max_seen_regno, regno + 1);
      if (static_cast<size_t>(bb_idx) < num_blocks && regno >= kFirstPseudoRegister) {
        live_at_any_block_start.insert(regno);
      }
    }
  }
  for (int gpr : greg_cfg.sideband.saved_gpr_regnos) {
    if (gpr < 0 || gpr >= kFirstFprRegnum) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid GPR %d in sideband saved_gpr_regnos", gpr));
    }
  }
  for (int fpr : greg_cfg.sideband.saved_fpr_regnos) {
    if (fpr < kFirstFprRegnum || fpr > kLastFprRegnum) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid FPR %d in sideband saved_fpr_regnos", fpr));
    }
  }

  RtlControlFlowGraph out = greg_cfg;
  out.pass = RtlPassKind::kLreg;

  absl::flat_hash_map<int, PseudoLivenessSummary> pseudo_summaries;
  absl::flat_hash_set<int> all_di_mult_pseudos;
  std::vector<std::vector<bool>> block_insn_clobbers_md(out.rpo_blocks.size());

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    absl::flat_hash_set<int> defined_so_far_in_block;
    absl::flat_hash_set<int> died_in_block;
    block_insn_clobbers_md[b].resize(blk.instructions.size(), false);

    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("InvertGlobalAlloc: failed to parse instruction %zu in block %zu: %s",
                            i, b, parsed.status().message()));
      }
      if (parsed->pattern.op == RtlOp::kNil || parsed->pattern.op == RtlOp::kUnknown) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: invalid pattern opcode at instruction %zu in block %zu", i, b));
      }
      if (absl::Status st = ValidateRtlExprRegisters(parsed->pattern, &max_seen_regno); !st.ok()) {
        return st;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (absl::Status st = ValidateRtlExprRegisters(note.datum, &max_seen_regno); !st.ok()) {
          return st;
        }
      }

      block_insn_clobbers_md[b][i] = InsnTouchesMdRegsOrCall(parsed->pattern);
      absl::flat_hash_set<int> insn_di_mult_dests;
      CollectDiMultDestRegs(parsed->pattern, &insn_di_mult_dests);
      for (int p : insn_di_mult_dests) {
        all_di_mult_pseudos.insert(p);
      }

      absl::flat_hash_set<int> insn_used_pseudos;
      absl::flat_hash_set<int> insn_defined_pseudos;
      CollectInsnElementUsesAndDefs(parsed->pattern, &insn_used_pseudos, &insn_defined_pseudos);

      for (int regno : insn_used_pseudos) {
        PseudoLivenessSummary& summary = pseudo_summaries[regno];
        summary.blocks_referenced.insert(b);
        if (!defined_so_far_in_block.contains(regno)) {
          summary.used_before_set = true;
        }
      }

      for (int regno : insn_defined_pseudos) {
        PseudoLivenessSummary& summary = pseudo_summaries[regno];
        summary.blocks_referenced.insert(b);
        summary.has_set_in_block = true;
        ++summary.def_count_in_block;
        summary.def_insn_idx = i;
        if (insn_di_mult_dests.contains(regno)) {
          summary.defined_by_di_mult = true;
        }
        defined_so_far_in_block.insert(regno);
        if (died_in_block.contains(regno)) {
          summary.alive_after_redef = true;
        }
      }

      for (const RtlRegNote& note : parsed->reg_notes) {
        note.datum.Visit([&](const RtlExpr& expr) {
          if (expr.op == RtlOp::kReg && expr.RegNo() >= kFirstPseudoRegister) {
            pseudo_summaries[expr.RegNo()].blocks_referenced.insert(b);
          }
        });
        if ((note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) &&
            note.datum.op == RtlOp::kReg && note.datum.RegNo() >= kFirstPseudoRegister) {
          const int dead_regno = note.datum.RegNo();
          PseudoLivenessSummary& summary = pseudo_summaries[dead_regno];
          ++summary.note_deaths;
          summary.death_insn_idx = i;
          died_in_block.insert(dead_regno);
          summary.alive_after_redef = false;
        }
      }
    }
  }

  out.sideband.max_regno = max_seen_regno;

  // In `-O0` (`stupid.c:stupid_life_analysis`), `flow_analysis` and
  // `local_alloc` do not run; `stupid_life_analysis` (`stupid.c:186-187`)
  // leaves `basic_block_live_at_start` empty and initializes
  // `reg_n_deaths[i] = 1` for every register `0..max_regno - 1` (including
  // fixed `$zero` register `0`, which `flow.c` never kills). Every entry in
  // `reg_renumber` at `-O0` was produced by `kGlobalAlloc`.
  const bool is_stupid_alloc =
      out.sideband.reg_n_deaths.contains(0) && out.sideband.basic_block_live_at_start.empty();

  // Determine which pseudos in `sideband.reg_renumber` were allocated by
  // `global.c:global_alloc` rather than `local-alloc.c:block_alloc`.
  // In GCC 2.7.2 (`local-alloc.c` lines 470-478, 1998-2004, 2113-2129), a
  // pseudo register `p >= 68` can only be allocated by `block_alloc` if:
  //   - `reg_basic_block[p] >= 0`: referenced in exactly one basic block `b`
  //     and not live at the start or end of any basic block,
  //   - `reg_n_deaths[p] == 1`: dies exactly once,
  //   - born (set) in `b` before any use and not redefined after its last death,
  //   - not assigned to `$fp` (`30`) or `MD_REGS` / `ST_REG` (`64..67`).
  std::vector<int> global_pseudos_to_erase;
  for (const auto& [regno, hard_reg] : out.sideband.reg_renumber) {
    bool is_global_alloc_pseudo = false;
    if (is_stupid_alloc || regno < kFirstPseudoRegister || hard_reg < 0 ||
        hard_reg == kFramePointerRegnum || hard_reg >= kHiRegnum) {
      is_global_alloc_pseudo = true;
    } else if (live_at_any_block_start.contains(regno)) {
      is_global_alloc_pseudo = true;
    } else if (auto death_it = out.sideband.reg_n_deaths.find(regno);
               death_it != out.sideband.reg_n_deaths.end() && death_it->second != 1) {
      is_global_alloc_pseudo = true;
    } else {
      auto sum_it = pseudo_summaries.find(regno);
      if (sum_it == pseudo_summaries.end() || sum_it->second.blocks_referenced.size() != 1 ||
          !sum_it->second.has_set_in_block || sum_it->second.used_before_set ||
          sum_it->second.alive_after_redef || sum_it->second.note_deaths >= 2) {
        is_global_alloc_pseudo = true;
      }
    }

    if (is_global_alloc_pseudo) {
      global_pseudos_to_erase.push_back(regno);
    }
  }

  for (int regno : global_pseudos_to_erase) {
    out.sideband.reg_renumber.erase(regno);
  }

  // Restore `MD_REGS` (`kHiRegnum == 64`) assignments made by `block_alloc` for
  // single-block `DImode` widening-multiply pseudos (`mulsidi3`/`umulsidi3`,
  // output constraint `"=x"`) that `reload()` inside `global_alloc`
  // (`global.c:580`, `reload1.c:spill_hard_reg`) spilled back to `-1` without
  // `retry_global_alloc` (`reg_allocno[p] == -1`) when another `DImode`
  // widening multiply required `MD_REGS` as an output reload register.
  if (!is_stupid_alloc && all_di_mult_pseudos.size() >= 2) {
    for (int p : all_di_mult_pseudos) {
      if (greg_cfg.sideband.reg_renumber.contains(p) || live_at_any_block_start.contains(p)) {
        continue;
      }
      if (auto d_it = out.sideband.reg_n_deaths.find(p);
          d_it != out.sideband.reg_n_deaths.end() && d_it->second != 1) {
        continue;
      }
      if (auto s_it = out.sideband.reg_n_sets.find(p);
          s_it != out.sideband.reg_n_sets.end() && s_it->second != 1) {
        continue;
      }
      if (auto c_it = out.sideband.reg_n_calls_crossed.find(p);
          c_it != out.sideband.reg_n_calls_crossed.end() && c_it->second != 0) {
        continue;
      }
      auto sum_it = pseudo_summaries.find(p);
      if (sum_it == pseudo_summaries.end()) {
        continue;
      }
      const PseudoLivenessSummary& sum = sum_it->second;
      if (sum.blocks_referenced.size() != 1 || !sum.has_set_in_block ||
          sum.def_count_in_block != 1 || !sum.defined_by_di_mult || sum.used_before_set ||
          sum.alive_after_redef || sum.note_deaths != 1 || sum.death_insn_idx <= sum.def_insn_idx) {
        continue;
      }
      const size_t bb_idx = *sum.blocks_referenced.begin();
      bool md_conflict_in_range = false;
      for (size_t k = sum.def_insn_idx + 1; k < sum.death_insn_idx; ++k) {
        if (block_insn_clobbers_md[bb_idx][k]) {
          md_conflict_in_range = true;
          break;
        }
      }
      if (md_conflict_in_range) {
        continue;
      }
      bool other_mult_needs_md_reload = false;
      for (int q : all_di_mult_pseudos) {
        if (q == p) {
          continue;
        }
        auto q_it = greg_cfg.sideband.reg_renumber.find(q);
        if (q_it == greg_cfg.sideband.reg_renumber.end() || q_it->second != kHiRegnum) {
          other_mult_needs_md_reload = true;
          break;
        }
      }
      if (other_mult_needs_md_reload) {
        out.sideband.reg_renumber[p] = kHiRegnum;
      }
    }
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
