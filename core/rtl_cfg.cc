#include "core/rtl_cfg.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"

namespace rom_nom_nom {

absl::Span<const RtlPassKind> AllRtlPasses() {
  static constexpr std::array<RtlPassKind, 13> kPasses = {
      RtlPassKind::kRtl,  RtlPassKind::kJump, RtlPassKind::kCse,     RtlPassKind::kLoop,
      RtlPassKind::kCse2, RtlPassKind::kFlow, RtlPassKind::kCombine, RtlPassKind::kSched,
      RtlPassKind::kLreg, RtlPassKind::kGreg, RtlPassKind::kSched2,  RtlPassKind::kJump2,
      RtlPassKind::kDbr,
  };
  return kPasses;
}

std::string_view RtlPassExtension(RtlPassKind pass) {
  switch (pass) {
    case RtlPassKind::kRtl:
      return ".rtl";
    case RtlPassKind::kJump:
      return ".jump";
    case RtlPassKind::kCse:
      return ".cse";
    case RtlPassKind::kLoop:
      return ".loop";
    case RtlPassKind::kCse2:
      return ".cse2";
    case RtlPassKind::kFlow:
      return ".flow";
    case RtlPassKind::kCombine:
      return ".combine";
    case RtlPassKind::kSched:
      return ".sched";
    case RtlPassKind::kLreg:
      return ".lreg";
    case RtlPassKind::kGreg:
      return ".greg";
    case RtlPassKind::kSched2:
      return ".sched2";
    case RtlPassKind::kJump2:
      return ".jump2";
    case RtlPassKind::kDbr:
      return ".dbr";
  }
  return "";
}

absl::StatusOr<RtlExpr> RtlInsn::ParseExpr() const {
  auto parsed = ParsePatternAndNotes(canonical_pattern);
  if (!parsed.ok()) {
    return parsed.status();
  }
  return std::move(parsed->pattern);
}

absl::StatusOr<RtlPatternAndNotes> RtlInsn::ParseExprAndNotes() const {
  return ParsePatternAndNotes(canonical_pattern);
}

void RtlInsn::SetFromExpr(const RtlExpr& expr, absl::Span<const RtlRegNote> notes) {
  canonical_pattern = FormatPatternAndNotes(expr, notes);
}

void RtlInsn::SetFromExprAndNotes(const RtlPatternAndNotes& expr_and_notes) {
  canonical_pattern = expr_and_notes.ToSexprString();
}

void RtlBasicBlock::SyncRtlInsnsFromInstructions() {
  std::vector<RtlInsn> old_insns = std::move(rtl_insns);
  std::vector<bool> used(old_insns.size(), false);

  std::vector<RtlInsn> updated;
  updated.reserve(instructions.size());

  for (const std::string& pat : instructions) {
    size_t matched = old_insns.size();
    for (size_t j = 0; j < old_insns.size(); ++j) {
      if (!used[j] && old_insns[j].canonical_pattern == pat &&
          !old_insns[j].machine_instructions.empty()) {
        matched = j;
        break;
      }
    }
    if (matched == old_insns.size()) {
      for (size_t j = 0; j < old_insns.size(); ++j) {
        if (!used[j] && old_insns[j].canonical_pattern == pat) {
          matched = j;
          break;
        }
      }
    }
    if (matched < old_insns.size()) {
      used[matched] = true;
      updated.push_back(std::move(old_insns[matched]));
    } else {
      RtlInsn insn;
      insn.canonical_pattern = pat;
      insn.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
      insn.is_synthetic_rtl_expansion = true;
      updated.push_back(std::move(insn));
    }
  }

  // Preserve any real `machine_instructions` from `old_insns` whose `canonical_pattern` was
  // rewritten or coalesced in `instructions`.
  if (!updated.empty()) {
    for (size_t j = 0; j < old_insns.size(); ++j) {
      if (used[j] || old_insns[j].machine_instructions.empty()) {
        continue;
      }
      size_t slot = updated.size();
      for (size_t k = 0; k < updated.size(); ++k) {
        if (updated[k].machine_instructions.empty()) {
          slot = k;
          break;
        }
      }
      if (slot < updated.size()) {
        updated[slot].machine_instructions = std::move(old_insns[j].machine_instructions);
        updated[slot].vram = old_insns[j].vram;
        updated[slot].idiom_kind = old_insns[j].idiom_kind;
        updated[slot].is_prologue_insn = old_insns[j].is_prologue_insn;
        updated[slot].is_synthetic_rtl_expansion = false;
      } else {
        auto& dst = updated.back().machine_instructions;
        dst.insert(dst.end(), old_insns[j].machine_instructions.begin(),
                   old_insns[j].machine_instructions.end());
        updated.back().is_synthetic_rtl_expansion = false;
      }
    }
  }

  rtl_insns = std::move(updated);
}

void RtlControlFlowGraph::SyncAllBlocksPreservingMachineInsns(
    const std::vector<RtlBasicBlock>& prev_blocks) {
  for (size_t i = 0; i < rpo_blocks.size(); ++i) {
    rpo_blocks[i].id = static_cast<uint32_t>(i);
    rpo_blocks[i].SyncRtlInsnsFromInstructions();
  }
  if (prev_blocks.empty()) {
    return;
  }

  size_t prev_machine_count = 0;
  for (const auto& pblk : prev_blocks) {
    for (const auto& pinsn : pblk.rtl_insns) {
      prev_machine_count += pinsn.machine_instructions.size();
    }
  }
  if (TotalMachineInsnCount() >= prev_machine_count) {
    return;
  }

  // Collect `vram`s of machine instructions already present in `rpo_blocks`.
  std::vector<uint32_t> present_vrams;
  for (const auto& blk : rpo_blocks) {
    for (const auto& rinsn : blk.rtl_insns) {
      for (const auto& mi : rinsn.machine_instructions) {
        present_vrams.push_back(mi.vram);
      }
    }
  }
  auto is_vram_present = [&](uint32_t vram) {
    for (uint32_t v : present_vrams) {
      if (v == vram) {
        return true;
      }
    }
    return false;
  };

  std::vector<RtlInsn> missing_real_insns;
  for (const auto& pblk : prev_blocks) {
    for (const auto& pinsn : pblk.rtl_insns) {
      if (pinsn.machine_instructions.empty()) {
        continue;
      }
      if (!is_vram_present(pinsn.machine_instructions.front().vram)) {
        missing_real_insns.push_back(pinsn);
        for (const auto& mi : pinsn.machine_instructions) {
          present_vrams.push_back(mi.vram);
        }
      }
    }
  }

  size_t m_idx = 0;
  for (auto& blk : rpo_blocks) {
    for (auto& rinsn : blk.rtl_insns) {
      if (m_idx >= missing_real_insns.size()) {
        break;
      }
      if (rinsn.machine_instructions.empty()) {
        rinsn.machine_instructions = std::move(missing_real_insns[m_idx].machine_instructions);
        rinsn.vram = missing_real_insns[m_idx].vram;
        rinsn.idiom_kind = missing_real_insns[m_idx].idiom_kind;
        rinsn.is_prologue_insn = missing_real_insns[m_idx].is_prologue_insn;
        rinsn.is_synthetic_rtl_expansion = false;
        ++m_idx;
      }
    }
  }
  if (m_idx < missing_real_insns.size()) {
    for (auto it = rpo_blocks.rbegin(); it != rpo_blocks.rend(); ++it) {
      if (!it->rtl_insns.empty()) {
        auto& dst = it->rtl_insns.back().machine_instructions;
        for (; m_idx < missing_real_insns.size(); ++m_idx) {
          dst.insert(dst.end(), missing_real_insns[m_idx].machine_instructions.begin(),
                     missing_real_insns[m_idx].machine_instructions.end());
        }
        it->rtl_insns.back().is_synthetic_rtl_expansion = false;
        break;
      }
    }
  }
}

std::string CanonicalizeRtlPattern(std::string_view pattern) {
  std::string collapsed;
  collapsed.reserve(pattern.size());
  for (size_t i = 0; i < pattern.size();) {
    if (absl::ascii_isspace(pattern[i])) {
      if (!collapsed.empty() && collapsed.back() != ' ') {
        collapsed.push_back(' ');
      }
      while (i < pattern.size() && absl::ascii_isspace(pattern[i])) {
        ++i;
      }
      continue;
    }
    collapsed.push_back(pattern[i++]);
  }
  return collapsed;
}

RtlCfgPassComparison RtlControlFlowGraph::CompareAgainst(
    const RtlControlFlowGraph& expected) const {
  RtlCfgPassComparison cmp;
  cmp.pass_present = true;
  cmp.actual_block_count = rpo_blocks.size();
  cmp.expected_block_count = expected.rpo_blocks.size();
  cmp.actual_insn_count = TotalInsnCount();
  cmp.expected_insn_count = expected.TotalInsnCount();

  cmp.block_count_matches = (cmp.actual_block_count == cmp.expected_block_count);
  if (!cmp.block_count_matches) {
    std::string act_desc;
    for (size_t b = 0; b < rpo_blocks.size(); ++b) {
      absl::StrAppendFormat(&act_desc, "%s%zu(i=%zu->[%s])", b ? " " : "", b,
                            rpo_blocks[b].instructions.size(),
                            absl::StrJoin(rpo_blocks[b].successors, ","));
    }
    std::string exp_desc;
    for (size_t b = 0; b < expected.rpo_blocks.size(); ++b) {
      absl::StrAppendFormat(&exp_desc, "%s%zu(i=%zu->[%s])", b ? " " : "", b,
                            expected.rpo_blocks[b].instructions.size(),
                            absl::StrJoin(expected.rpo_blocks[b].successors, ","));
    }
    cmp.mismatch_detail =
        absl::StrFormat("RPO blocks got %zu {%s} != exp %zu {%s}", cmp.actual_block_count, act_desc,
                        cmp.expected_block_count, exp_desc);
    return cmp;
  }

  cmp.successors_match = true;
  cmp.insn_counts_match = true;
  cmp.instructions_match = true;
  for (size_t b = 0; b < rpo_blocks.size(); ++b) {
    if (rpo_blocks[b].successors != expected.rpo_blocks[b].successors) {
      cmp.successors_match = false;
      if (cmp.mismatch_detail.empty()) {
        cmp.mismatch_detail =
            absl::StrFormat("RPO blk %zu edges got [%s] != exp [%s]", b,
                            absl::StrJoin(rpo_blocks[b].successors, ","),
                            absl::StrJoin(expected.rpo_blocks[b].successors, ","));
      }
    }
    if (rpo_blocks[b].instructions.size() != expected.rpo_blocks[b].instructions.size()) {
      const bool first_count_mismatch = cmp.insn_counts_match;
      cmp.insn_counts_match = false;
      cmp.instructions_match = false;
      if (first_count_mismatch) {
        cmp.mismatch_detail = absl::StrFormat(
            "RPO blk %zu insn count got %zu [%s] != exp %zu [%s]", b,
            rpo_blocks[b].instructions.size(), absl::StrJoin(rpo_blocks[b].instructions, " ; "),
            expected.rpo_blocks[b].instructions.size(),
            absl::StrJoin(expected.rpo_blocks[b].instructions, " ; "));
      }
    } else {
      bool blk_insns_match = true;
      for (size_t k = 0; k < rpo_blocks[b].instructions.size(); ++k) {
        if (CanonicalizeRtlPattern(rpo_blocks[b].instructions[k]) !=
            CanonicalizeRtlPattern(expected.rpo_blocks[b].instructions[k])) {
          blk_insns_match = false;
          break;
        }
      }
      if (!blk_insns_match) {
        cmp.instructions_match = false;
        if (!cmp.mismatch_detail.empty()) {
          absl::StrAppend(&cmp.mismatch_detail, " || ");
        }
        absl::StrAppendFormat(&cmp.mismatch_detail,
                              "RPO blk %zu insn patterns got [%s] != exp [%s]", b,
                              absl::StrJoin(rpo_blocks[b].instructions, " ; "),
                              absl::StrJoin(expected.rpo_blocks[b].instructions, " ; "));
      }
    }
  }
  return cmp;
}

std::string RtlControlFlowGraph::FormatSummary() const {
  std::string out;
  for (size_t b = 0; b < rpo_blocks.size(); ++b) {
    if (b > 0) {
      absl::StrAppend(&out, "\n");
    }
    absl::StrAppendFormat(&out, "B%zu (-> [%s]): %s", b,
                          absl::StrJoin(rpo_blocks[b].successors, ","),
                          absl::StrJoin(rpo_blocks[b].instructions, " ; "));
  }
  return out;
}

void RecomputeRtlCfgRpo(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  const size_t n = cfg->rpo_blocks.size();
  std::vector<bool> visited(n, false);
  std::vector<size_t> post_order;
  post_order.reserve(n);
  auto dfs = [&](auto& self, size_t u) -> void {
    visited[u] = true;
    for (size_t v : cfg->rpo_blocks[u].successors) {
      if (v < n && !visited[v]) {
        self(self, v);
      }
    }
    post_order.push_back(u);
  };
  dfs(dfs, 0);
  std::vector<size_t> rpo(post_order.rbegin(), post_order.rend());
  std::vector<size_t> old_to_new(n, n);
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    old_to_new[rpo[idx]] = idx;
  }
  std::vector<RtlBasicBlock> new_blocks;
  new_blocks.reserve(rpo.size());
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    RtlBasicBlock blk = std::move(cfg->rpo_blocks[rpo[idx]]);
    blk.id = static_cast<uint32_t>(idx);
    std::vector<size_t> new_succs;
    new_succs.reserve(blk.successors.size());
    for (size_t s : blk.successors) {
      if (s < n && old_to_new[s] < rpo.size()) {
        new_succs.push_back(old_to_new[s]);
      }
    }
    blk.successors = std::move(new_succs);
    std::vector<size_t> new_addr_vec_targets;
    new_addr_vec_targets.reserve(blk.addr_vec_targets.size());
    for (size_t s : blk.addr_vec_targets) {
      if (s < n && old_to_new[s] < rpo.size()) {
        new_addr_vec_targets.push_back(old_to_new[s]);
      }
    }
    blk.addr_vec_targets = std::move(new_addr_vec_targets);
    blk.predecessors.clear();
    new_blocks.push_back(std::move(blk));
  }
  for (size_t b = 0; b < new_blocks.size(); ++b) {
    for (size_t s : new_blocks[b].successors) {
      if (s < new_blocks.size()) {
        new_blocks[s].predecessors.push_back(b);
      }
    }
  }
  cfg->rpo_blocks = std::move(new_blocks);
}

}  // namespace rom_nom_nom
