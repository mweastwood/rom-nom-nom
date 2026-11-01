#include "lifter/rtl_cfg.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"

namespace rom_nom_nom {

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
      cmp.instructions_match = false;
      if (cmp.mismatch_detail.empty()) {
        cmp.mismatch_detail = absl::StrFormat(
            "RPO blk %zu insn count got %zu [%s] != exp %zu [%s]", b,
            rpo_blocks[b].instructions.size(), absl::StrJoin(rpo_blocks[b].instructions, " ; "),
            expected.rpo_blocks[b].instructions.size(),
            absl::StrJoin(expected.rpo_blocks[b].instructions, " ; "));
      }
    }
  }
  return cmp;
}

}  // namespace rom_nom_nom
