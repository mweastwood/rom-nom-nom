#include "core/rtl_cfg.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"

namespace rom_nom_nom {
namespace {

bool ContainsCallInsnAst(std::string_view pat) {
  auto parsed = ParsePatternAndNotes(pat);
  if (!parsed.ok()) {
    return false;
  }
  bool has_call = false;
  parsed->pattern.Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kCall || node.op == RtlOp::kCallInsn) {
      has_call = true;
    }
  });
  return has_call;
}

std::vector<int> CanonicalizePointerRegnos(absl::Span<const int> regnos) {
  std::vector<int> out;
  out.reserve(regnos.size());
  for (int r : regnos) {
    if (r >= kMipsFirstPseudoRegister) {
      out.push_back(r);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::optional<RtlLoopNoteKind> ExtractEffectiveLoopNoteKind(const RtlBasicBlock& blk, size_t k) {
  if (k < blk.rtl_insns.size() && k < blk.instructions.size() &&
      blk.rtl_insns[k].canonical_pattern == blk.instructions[k]) {
    return blk.rtl_insns[k].loop_note_kind;
  }
  if (k < blk.instructions.size()) {
    return ParseRtlLoopNoteKind(blk.instructions[k]);
  }
  return std::nullopt;
}

std::pair<bool, std::string> ExtractEffectiveCallFusage(const RtlBasicBlock& blk, size_t k) {
  if (k < blk.rtl_insns.size() && blk.rtl_insns[k].has_explicit_call_fusage) {
    return {true, BuildCallFusageChainExpr(blk.rtl_insns[k].call_fusage).ToGccPrintRtlString()};
  }
  if (k < blk.instructions.size()) {
    if (auto parsed = ParsePatternAndNotes(blk.instructions[k]);
        parsed.ok() && parsed->has_explicit_call_fusage) {
      return {true, BuildCallFusageChainExpr(parsed->call_fusage).ToGccPrintRtlString()};
    }
  }
  return {false, ""};
}

}  // namespace

std::optional<RtlLoopNoteKind> ParseRtlLoopNoteKind(std::string_view pattern_or_note_name) {
  std::string_view s = absl::StripAsciiWhitespace(pattern_or_note_name);
  if (s == "NOTE_INSN_LOOP_BEG" || s == "(note NOTE_INSN_LOOP_BEG)") {
    return RtlLoopNoteKind::kLoopBeg;
  }
  if (s == "NOTE_INSN_LOOP_VTOP" || s == "(note NOTE_INSN_LOOP_VTOP)") {
    return RtlLoopNoteKind::kLoopVtop;
  }
  if (s == "NOTE_INSN_LOOP_CONT" || s == "(note NOTE_INSN_LOOP_CONT)") {
    return RtlLoopNoteKind::kLoopCont;
  }
  if (s == "NOTE_INSN_LOOP_END" || s == "(note NOTE_INSN_LOOP_END)") {
    return RtlLoopNoteKind::kLoopEnd;
  }
  return std::nullopt;
}

std::string_view RtlLoopNoteKindToName(RtlLoopNoteKind kind) {
  switch (kind) {
    case RtlLoopNoteKind::kLoopBeg:
      return "NOTE_INSN_LOOP_BEG";
    case RtlLoopNoteKind::kLoopVtop:
      return "NOTE_INSN_LOOP_VTOP";
    case RtlLoopNoteKind::kLoopCont:
      return "NOTE_INSN_LOOP_CONT";
    case RtlLoopNoteKind::kLoopEnd:
      return "NOTE_INSN_LOOP_END";
  }
  return "";
}

std::string_view RtlLoopNoteKindToSexpr(RtlLoopNoteKind kind) {
  switch (kind) {
    case RtlLoopNoteKind::kLoopBeg:
      return "(note NOTE_INSN_LOOP_BEG)";
    case RtlLoopNoteKind::kLoopVtop:
      return "(note NOTE_INSN_LOOP_VTOP)";
    case RtlLoopNoteKind::kLoopCont:
      return "(note NOTE_INSN_LOOP_CONT)";
    case RtlLoopNoteKind::kLoopEnd:
      return "(note NOTE_INSN_LOOP_END)";
  }
  return "";
}

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
  auto parsed = ParsePatternAndNotes(canonical_pattern);
  if (!parsed.ok()) {
    return parsed.status();
  }
  if (!parsed->has_explicit_call_fusage && has_explicit_call_fusage) {
    parsed->has_explicit_call_fusage = true;
    parsed->call_fusage = call_fusage;
  }
  return parsed;
}

void RtlInsn::SetFromExpr(const RtlExpr& expr, absl::Span<const RtlRegNote> notes) {
  canonical_pattern = FormatPatternAndNotes(expr, notes);
  loop_note_kind = ParseRtlLoopNoteKind(canonical_pattern);
}

void RtlInsn::SetFromExprAndNotes(const RtlPatternAndNotes& expr_and_notes) {
  canonical_pattern = expr_and_notes.ToSexprString();
  loop_note_kind = ParseRtlLoopNoteKind(canonical_pattern);
  if (expr_and_notes.has_explicit_call_fusage) {
    has_explicit_call_fusage = true;
    call_fusage = expr_and_notes.call_fusage;
  }
}

bool RtlBasicBlock::HasLoopNotes() const {
  for (const auto& pat : instructions) {
    if (ParseRtlLoopNoteKind(pat).has_value()) {
      return true;
    }
  }
  for (const auto& rinsn : rtl_insns) {
    if (rinsn.loop_note_kind.has_value() ||
        ParseRtlLoopNoteKind(rinsn.canonical_pattern).has_value()) {
      return true;
    }
  }
  return false;
}

std::vector<RtlLoopNoteKind> RtlBasicBlock::LoopNotes() const {
  std::vector<RtlLoopNoteKind> notes;
  for (const auto& pat : instructions) {
    if (auto k = ParseRtlLoopNoteKind(pat); k.has_value()) {
      notes.push_back(*k);
    }
  }
  return notes;
}

void RtlBasicBlock::SyncRtlInsnsFromInstructions() {
  struct SavedCallFusage {
    bool has_explicit = false;
    std::vector<RtlExpr> fusage;
  };
  std::vector<SavedCallFusage> old_calls;
  for (const auto& old_insn : rtl_insns) {
    if (old_insn.has_explicit_call_fusage || ContainsCallInsnAst(old_insn.canonical_pattern)) {
      old_calls.push_back(SavedCallFusage{
          .has_explicit = old_insn.has_explicit_call_fusage,
          .fusage = old_insn.call_fusage,
      });
    }
  }

  std::vector<RtlInsn> old_insns = std::move(rtl_insns);
  std::vector<bool> used(old_insns.size(), false);
  std::vector<bool> inline_explicit_fusage(instructions.size(), false);

  std::vector<RtlInsn> updated;
  updated.reserve(instructions.size());

  for (size_t idx = 0; idx < instructions.size(); ++idx) {
    const std::string& pat = instructions[idx];
    auto parsed_pat = ParsePatternAndNotes(pat);
    if (parsed_pat.ok() && parsed_pat->has_explicit_call_fusage) {
      inline_explicit_fusage[idx] = true;
    }

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
      RtlInsn kept = std::move(old_insns[matched]);
      kept.loop_note_kind = ParseRtlLoopNoteKind(pat);
      if (inline_explicit_fusage[idx]) {
        kept.has_explicit_call_fusage = true;
        kept.call_fusage = std::move(parsed_pat->call_fusage);
      }
      updated.push_back(std::move(kept));
    } else {
      RtlInsn insn;
      insn.canonical_pattern = pat;
      insn.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
      insn.is_synthetic_rtl_expansion = true;
      insn.loop_note_kind = ParseRtlLoopNoteKind(pat);
      if (inline_explicit_fusage[idx]) {
        insn.has_explicit_call_fusage = true;
        insn.call_fusage = std::move(parsed_pat->call_fusage);
      }
      updated.push_back(std::move(insn));
    }
  }

  // Preserve explicit `call_fusage` when `CALL_INSN`s or call `SEQUENCE`s were
  // unwrapped, wrapped, or register-rewritten within the same basic block.
  // When call count is invariant, map strictly in sequential call order so an
  // unwrapped `SEQUENCE` call never swaps `call_fusage` with a later identical-target call.
  std::vector<size_t> updated_call_indices;
  for (size_t k = 0; k < updated.size(); ++k) {
    if (updated[k].has_explicit_call_fusage || ContainsCallInsnAst(updated[k].canonical_pattern)) {
      updated_call_indices.push_back(k);
    }
  }
  if (updated_call_indices.size() == old_calls.size()) {
    for (size_t c = 0; c < updated_call_indices.size(); ++c) {
      const size_t k = updated_call_indices[c];
      if (!inline_explicit_fusage[k]) {
        updated[k].has_explicit_call_fusage = old_calls[c].has_explicit;
        updated[k].call_fusage = old_calls[c].fusage;
      }
    }
  } else {
    for (size_t j = 0; j < old_insns.size(); ++j) {
      if (used[j] || !old_insns[j].has_explicit_call_fusage) {
        continue;
      }
      for (size_t k : updated_call_indices) {
        if (!updated[k].has_explicit_call_fusage) {
          updated[k].has_explicit_call_fusage = true;
          updated[k].call_fusage = old_insns[j].call_fusage;
          break;
        }
      }
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

  // Propagate explicit `call_fusage` across block splits or reconstructions
  // even when no `machine_instructions` are present.
  std::vector<const RtlInsn*> prev_calls;
  for (const auto& pblk : prev_blocks) {
    for (const auto& pinsn : pblk.rtl_insns) {
      if (pinsn.has_explicit_call_fusage || ContainsCallInsnAst(pinsn.canonical_pattern)) {
        prev_calls.push_back(&pinsn);
      }
    }
  }
  std::vector<RtlInsn*> curr_calls;
  for (auto& blk : rpo_blocks) {
    for (auto& rinsn : blk.rtl_insns) {
      if (rinsn.has_explicit_call_fusage || ContainsCallInsnAst(rinsn.canonical_pattern)) {
        curr_calls.push_back(&rinsn);
      }
    }
  }
  if (curr_calls.size() == prev_calls.size()) {
    for (size_t c = 0; c < curr_calls.size(); ++c) {
      if (!curr_calls[c]->has_explicit_call_fusage && prev_calls[c]->has_explicit_call_fusage) {
        curr_calls[c]->has_explicit_call_fusage = true;
        curr_calls[c]->call_fusage = prev_calls[c]->call_fusage;
      }
    }
  } else {
    size_t p_idx = 0;
    for (RtlInsn* c_insn : curr_calls) {
      if (c_insn->has_explicit_call_fusage) {
        continue;
      }
      while (p_idx < prev_calls.size() && !prev_calls[p_idx]->has_explicit_call_fusage) {
        ++p_idx;
      }
      if (p_idx < prev_calls.size()) {
        c_insn->has_explicit_call_fusage = true;
        c_insn->call_fusage = prev_calls[p_idx]->call_fusage;
        ++p_idx;
      }
    }
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

  const std::vector<int> act_ptr_regnos = CanonicalizePointerRegnos(sideband.pointer_regnos);
  const std::vector<int> exp_ptr_regnos =
      CanonicalizePointerRegnos(expected.sideband.pointer_regnos);
  cmp.pointer_regnos_match = (act_ptr_regnos == exp_ptr_regnos);

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
  cmp.loop_notes_match = true;
  cmp.call_fusage_match = true;
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
    if (rpo_blocks[b].LoopNotes() != expected.rpo_blocks[b].LoopNotes()) {
      cmp.loop_notes_match = false;
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
      std::vector<std::pair<bool, std::string>> act_fusages;
      for (size_t k = 0; k < rpo_blocks[b].instructions.size(); ++k) {
        auto f = ExtractEffectiveCallFusage(rpo_blocks[b], k);
        if (f.first || ContainsCallInsnAst(rpo_blocks[b].instructions[k])) {
          act_fusages.push_back(std::move(f));
        }
      }
      std::vector<std::pair<bool, std::string>> exp_fusages;
      for (size_t k = 0; k < expected.rpo_blocks[b].instructions.size(); ++k) {
        auto f = ExtractEffectiveCallFusage(expected.rpo_blocks[b], k);
        if (f.first || ContainsCallInsnAst(expected.rpo_blocks[b].instructions[k])) {
          exp_fusages.push_back(std::move(f));
        }
      }
      if (act_fusages != exp_fusages) {
        cmp.call_fusage_match = false;
      }
    } else {
      bool blk_insns_match = true;
      for (size_t k = 0; k < rpo_blocks[b].instructions.size(); ++k) {
        if (CanonicalizeRtlPattern(rpo_blocks[b].instructions[k]) !=
            CanonicalizeRtlPattern(expected.rpo_blocks[b].instructions[k])) {
          blk_insns_match = false;
        }
        const auto act_note = ExtractEffectiveLoopNoteKind(rpo_blocks[b], k);
        const auto exp_note = ExtractEffectiveLoopNoteKind(expected.rpo_blocks[b], k);
        if (act_note != exp_note) {
          cmp.loop_notes_match = false;
          if (!cmp.mismatch_detail.empty()) {
            absl::StrAppend(&cmp.mismatch_detail, " || ");
          }
          absl::StrAppendFormat(&cmp.mismatch_detail,
                                "RPO blk %zu insn %zu loop_note got %s != exp %s", b, k,
                                act_note.has_value() ? RtlLoopNoteKindToName(*act_note) : "<none>",
                                exp_note.has_value() ? RtlLoopNoteKindToName(*exp_note) : "<none>");
        }
        const auto act_fusage = ExtractEffectiveCallFusage(rpo_blocks[b], k);
        const auto exp_fusage = ExtractEffectiveCallFusage(expected.rpo_blocks[b], k);
        if (act_fusage != exp_fusage) {
          cmp.call_fusage_match = false;
          if (!cmp.mismatch_detail.empty()) {
            absl::StrAppend(&cmp.mismatch_detail, " || ");
          }
          absl::StrAppendFormat(&cmp.mismatch_detail,
                                "RPO blk %zu insn %zu call_fusage got %s != exp %s", b, k,
                                act_fusage.first ? act_fusage.second : "<implicit>",
                                exp_fusage.first ? exp_fusage.second : "<implicit>");
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

  if (!cmp.pointer_regnos_match) {
    if (!cmp.mismatch_detail.empty()) {
      absl::StrAppend(&cmp.mismatch_detail, " || ");
    }
    absl::StrAppendFormat(&cmp.mismatch_detail, "sideband.pointer_regnos got [%s] != exp [%s]",
                          absl::StrJoin(act_ptr_regnos, ","), absl::StrJoin(exp_ptr_regnos, ","));
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

  // Preserve `NOTE_INSN_LOOP_*` markers on blocks preceded by a `BARRIER`
  // (which have zero incoming CFG edges in `flow.c` / `jump.c` yet remain in
  // GCC's linear RTL chain so `loop.c:find_and_verify_loops` sees balanced
  // `NOTE_INSN_LOOP_BEG` .. `NOTE_INSN_LOOP_END` pairs).
  for (size_t u = 1; u < n; ++u) {
    if (visited[u] || !cfg->rpo_blocks[u].HasLoopNotes()) {
      continue;
    }
    RtlBasicBlock& unreachable_blk = cfg->rpo_blocks[u];
    std::vector<std::string> kept_notes;
    for (const std::string& pat : unreachable_blk.instructions) {
      if (ParseRtlLoopNoteKind(pat).has_value()) {
        kept_notes.push_back(pat);
      }
    }
    unreachable_blk.instructions = std::move(kept_notes);
    unreachable_blk.SyncRtlInsnsFromInstructions();
    unreachable_blk.has_label = false;
    unreachable_blk.has_trailing_barrier = false;
    unreachable_blk.addr_vec_targets.clear();

    const std::vector<RtlLoopNoteKind> notes = unreachable_blk.LoopNotes();
    const bool anchor_after_prev = !notes.empty() && notes.back() == RtlLoopNoteKind::kLoopEnd;

    bool inserted = false;
    if (anchor_after_prev) {
      for (size_t p = u; p > 0; --p) {
        auto it = std::find(rpo.begin(), rpo.end(), p - 1);
        if (it != rpo.end()) {
          rpo.insert(it + 1, u);
          inserted = true;
          break;
        }
      }
    } else {
      for (size_t s = u + 1; s < n; ++s) {
        auto it = std::find(rpo.begin(), rpo.end(), s);
        if (it != rpo.end()) {
          rpo.insert(it, u);
          inserted = true;
          break;
        }
      }
    }
    if (!inserted) {
      rpo.push_back(u);
    }
  }

  std::vector<size_t> old_to_new(n, n);
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    old_to_new[rpo[idx]] = idx;
  }
  std::vector<RtlBasicBlock> new_blocks;
  new_blocks.reserve(rpo.size());
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    const size_t old_idx = rpo[idx];
    RtlBasicBlock blk = std::move(cfg->rpo_blocks[old_idx]);
    blk.id = static_cast<uint32_t>(idx);
    std::vector<size_t> new_succs;
    if (!visited[old_idx]) {
      if (idx + 1 < rpo.size()) {
        new_succs.push_back(idx + 1);
      }
    } else {
      new_succs.reserve(blk.successors.size());
      for (size_t s : blk.successors) {
        if (s < n && old_to_new[s] < rpo.size()) {
          new_succs.push_back(old_to_new[s]);
        }
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
