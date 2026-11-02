#include "lifter/rtl_cfg.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"

namespace rom_nom_nom {

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

namespace {

bool IsRtlMetadataFlagList(std::string_view rest, size_t* consumed) {
  if (rest.empty() || rest[0] != '/') {
    return false;
  }
  size_t pos = 0;
  while (pos < rest.size() && rest[pos] == '/') {
    ++pos;
    if (pos >= rest.size()) {
      return false;
    }
    size_t start = pos;
    while (pos < rest.size() && (rest[pos] == 'v' || rest[pos] == 's' || rest[pos] == 'i' ||
                                 rest[pos] == 'u' || rest[pos] == 'f')) {
      ++pos;
    }
    if (pos == start) {
      return false;
    }
  }
  if (pos < rest.size() && rest[pos] != ':' && !absl::ascii_isspace(rest[pos]) &&
      rest[pos] != ')') {
    return false;
  }
  *consumed = pos;
  return true;
}

bool ParseHardGprExpr(std::string_view expr, std::string_view* out_mode, int* out_regno,
                      std::string_view* out_name) {
  if (!absl::StartsWith(expr, "(reg:") || !absl::EndsWith(expr, ")")) {
    return false;
  }
  std::string_view inner = expr.substr(5, expr.size() - 6);
  size_t sp1 = inner.find(' ');
  if (sp1 == std::string_view::npos) {
    return false;
  }
  std::string_view mode = inner.substr(0, sp1);
  if (mode != "QI" && mode != "HI" && mode != "SI" && mode != "SF") {
    return false;
  }
  std::string_view after_mode = inner.substr(sp1 + 1);
  size_t sp2 = after_mode.find(' ');
  if (sp2 == std::string_view::npos) {
    return false;
  }
  int regno = -1;
  if (!absl::SimpleAtoi(after_mode.substr(0, sp2), &regno) || regno < 0 || regno >= 32) {
    return false;
  }
  std::string_view name = after_mode.substr(sp2 + 1);
  if (name.empty() || name.find(' ') != std::string_view::npos ||
      name.find('(') != std::string_view::npos || name.find(')') != std::string_view::npos) {
    return false;
  }
  *out_mode = mode;
  *out_regno = regno;
  *out_name = name;
  return true;
}

std::string CanonicalizeHardGprSubregs(std::string s) {
  constexpr std::string_view kPrefix = "(subreg:";
  size_t pos = 0;
  while ((pos = s.find(kPrefix, pos)) != std::string::npos) {
    const std::string_view s_sv = s;
    size_t mode_start = pos + kPrefix.size();
    size_t sp1 = s.find(' ', mode_start);
    if (sp1 == std::string::npos) {
      break;
    }
    std::string_view outer_mode = s_sv.substr(mode_start, sp1 - mode_start);
    if (outer_mode != "QI" && outer_mode != "HI" && outer_mode != "SI" && outer_mode != "SF") {
      pos = sp1;
      continue;
    }
    if (sp1 + 1 >= s.size() || s[sp1 + 1] != '(') {
      pos = sp1;
      continue;
    }
    size_t reg_end = s.find(')', sp1 + 1);
    if (reg_end == std::string::npos) {
      break;
    }
    std::string_view reg_expr = s_sv.substr(sp1 + 1, reg_end - sp1);
    std::string_view inner_mode;
    int regno = -1;
    std::string_view reg_name;
    if (!ParseHardGprExpr(reg_expr, &inner_mode, &regno, &reg_name)) {
      pos = sp1 + 1;
      continue;
    }
    std::string_view tail = s_sv.substr(reg_end + 1);
    if (!absl::StartsWith(tail, " 0)")) {
      pos = sp1 + 1;
      continue;
    }
    std::string replacement = absl::StrFormat("(reg:%s %d %s)", outer_mode, regno, reg_name);
    s.replace(pos, (reg_end + 1 + 3) - pos, replacement);
  }
  return s;
}

std::string_view ReverseBranchComparisonCode(std::string_view op) {
  if (op == "eq") return "ne";
  if (op == "ne") return "eq";
  if (op == "lt") return "ge";
  if (op == "ge") return "lt";
  if (op == "gt") return "le";
  if (op == "le") return "gt";
  if (op == "ltu") return "geu";
  if (op == "geu") return "ltu";
  if (op == "gtu") return "leu";
  if (op == "leu") return "gtu";
  return "";
}

}  // namespace

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
    if (pattern[i] == '(') {
      std::string_view rest = pattern.substr(i + 1);
      if (absl::StartsWith(rest, "label_ref")) {
        collapsed.append("(label_ref)");
        i += 1 + std::string_view("label_ref").size();
        while (i < pattern.size() && pattern[i] != ')') {
          ++i;
        }
        if (i < pattern.size() && pattern[i] == ')') {
          ++i;
        }
        continue;
      }
      for (std::string_view code : {"reg", "mem", "symbol_ref", "subreg", "const_double", "insn",
                                    "jump_insn", "call_insn"}) {
        if (absl::StartsWith(rest, code)) {
          size_t flag_consumed = 0;
          if (IsRtlMetadataFlagList(rest.substr(code.size()), &flag_consumed)) {
            collapsed.push_back('(');
            collapsed.append(code);
            i += 1 + code.size() + flag_consumed;
            goto next_char;
          }
        }
      }
    }
    collapsed.push_back(pattern[i++]);
  next_char:;
  }

  collapsed = CanonicalizeHardGprSubregs(std::move(collapsed));

  // Canonicalize inverted branch polarity `(if_then_else (rev_op:SI ...) (pc) (label_ref))` ->
  // `(if_then_else (op:SI ...) (label_ref) (pc))`, which emits identical branch assembly in
  // `final.c` (`config/mips/mips.md`).
  constexpr std::string_view kInvBranchPrefix = "(set (pc) (if_then_else (";
  constexpr std::string_view kInvBranchSuffix = ") (pc) (label_ref)))";
  if (absl::StartsWith(collapsed, kInvBranchPrefix) &&
      absl::EndsWith(collapsed, kInvBranchSuffix)) {
    const std::string_view collapsed_sv = collapsed;
    std::string_view mid =
        collapsed_sv.substr(kInvBranchPrefix.size(),
                            collapsed.size() - kInvBranchPrefix.size() - kInvBranchSuffix.size());
    size_t colon = mid.find(":SI ");
    if (colon != std::string_view::npos) {
      std::string_view rev_op = ReverseBranchComparisonCode(mid.substr(0, colon));
      if (!rev_op.empty()) {
        collapsed =
            absl::StrCat(kInvBranchPrefix, rev_op, mid.substr(colon), ") (label_ref) (pc)))");
      }
    }
  }

  // Canonicalize 32-bit hard-GPR (`0..31`) bit-copy moves, loads, and stores (`movsf_internal1` /
  // `movhi_internal1` / `movqi_internal1` vs `movsi_internal1`).
  if (absl::StartsWith(collapsed, "(set ") && absl::EndsWith(collapsed, ")")) {
    const std::string_view collapsed_sv = collapsed;
    std::string_view body = collapsed_sv.substr(5, collapsed.size() - 6);
    if (!body.empty() && body[0] == '(') {
      int depth = 0;
      size_t lhs_end = std::string_view::npos;
      for (size_t idx = 0; idx < body.size(); ++idx) {
        if (body[idx] == '(') {
          ++depth;
        } else if (body[idx] == ')') {
          --depth;
          if (depth == 0) {
            lhs_end = idx;
            break;
          }
        }
      }
      if (lhs_end != std::string_view::npos && lhs_end + 2 < body.size() &&
          body[lhs_end + 1] == ' ') {
        std::string_view lhs = body.substr(0, lhs_end + 1);
        std::string_view rhs = body.substr(lhs_end + 2);
        std::string_view lhs_mode;
        int lhs_regno = -1;
        std::string_view lhs_name;
        std::string_view rhs_mode;
        int rhs_regno = -1;
        std::string_view rhs_name;
        const bool lhs_is_gpr = ParseHardGprExpr(lhs, &lhs_mode, &lhs_regno, &lhs_name);
        const bool rhs_is_gpr = ParseHardGprExpr(rhs, &rhs_mode, &rhs_regno, &rhs_name);
        if (lhs_is_gpr && rhs_is_gpr && lhs_mode == rhs_mode && lhs_mode != "SI") {
          collapsed = absl::StrFormat("(set (reg:SI %d %s) (reg:SI %d %s))", lhs_regno, lhs_name,
                                      rhs_regno, rhs_name);
        } else if (rhs_is_gpr && rhs_mode == "SF" && absl::StartsWith(lhs, "(mem:SF ")) {
          collapsed = absl::StrFormat("(set (mem:SI %s (reg:SI %d %s))", lhs.substr(8), rhs_regno,
                                      rhs_name);
        } else if (lhs_is_gpr && lhs_mode == "SF" && absl::StartsWith(rhs, "(mem:SF ")) {
          collapsed = absl::StrFormat("(set (reg:SI %d %s) (mem:SI %s)", lhs_regno, lhs_name,
                                      rhs.substr(8));
        }
      }
    }
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
      cmp.insn_counts_match = false;
      cmp.instructions_match = false;
      if (cmp.mismatch_detail.empty()) {
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

}  // namespace rom_nom_nom
