#include "lifter/fuzz_test_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/functional/function_ref.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/rtl_liveness.h"
#include "lifter/sched_model.h"
#include "lifter/sched_post_reload_projection.h"

namespace rom_nom_nom::lifter {
namespace {

void ApplyRegRenameMapToCfg(const absl::btree_map<int, int>& rename_map, RtlControlFlowGraph* cfg) {
  auto rewrite_expr = [&](RtlExpr* expr) {
    expr->RewriteRegs([&](RtlExpr& n) {
      if (n.IsArgPointerReg()) {
        return;
      }
      if (auto it = rename_map.find(n.RegNo()); it != rename_map.end()) {
        n.SetRegNo(it->second);
      }
    });
  };

  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (ParseRtlLoopNoteKind(blk.instructions[i]).has_value()) {
        continue;
      }
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      rewrite_expr(&parsed->pattern);
      for (RtlRegNote& note : parsed->reg_notes) {
        rewrite_expr(&note.datum);
      }
      blk.instructions[i] = FormatPn(*parsed);
      if (i < blk.rtl_insns.size()) {
        blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
        for (RtlExpr& fu : blk.rtl_insns[i].call_fusage) {
          rewrite_expr(&fu);
        }
      }
    }
  }

  auto remap_int_map = [&](auto* m) {
    absl::btree_map<int, int> ordered;
    for (const auto& [k, v] : *m) {
      auto it = rename_map.find(k);
      ordered[it != rename_map.end() ? it->second : k] = v;
    }
    m->clear();
    for (const auto& [k, v] : ordered) {
      (*m)[k] = v;
    }
  };
  remap_int_map(&cfg->sideband.reg_n_refs);
  remap_int_map(&cfg->sideband.reg_n_sets);
  remap_int_map(&cfg->sideband.reg_n_deaths);
  remap_int_map(&cfg->sideband.reg_live_length);
  remap_int_map(&cfg->sideband.reg_n_calls_crossed);
  remap_int_map(&cfg->sideband.reg_renumber);

  for (auto& [b, live] : cfg->sideband.basic_block_live_at_start) {
    absl::btree_set<int> ordered_live;
    for (int r : live) {
      auto it = rename_map.find(r);
      ordered_live.insert(it != rename_map.end() ? it->second : r);
    }
    live.clear();
    for (int r : ordered_live) {
      live.insert(r);
    }
  }
  for (int& r : cfg->sideband.pointer_regnos) {
    if (auto it = rename_map.find(r); it != rename_map.end()) {
      r = it->second;
    }
  }
  std::sort(cfg->sideband.pointer_regnos.begin(), cfg->sideband.pointer_regnos.end());
  for (int& r : cfg->sideband.saved_gpr_regnos) {
    if (auto it = rename_map.find(r); it != rename_map.end()) {
      r = it->second;
    }
  }
  std::sort(cfg->sideband.saved_gpr_regnos.begin(), cfg->sideband.saved_gpr_regnos.end());
  for (int& r : cfg->sideband.saved_fpr_regnos) {
    if (auto it = rename_map.find(r); it != rename_map.end()) {
      r = it->second;
    }
  }
  std::sort(cfg->sideband.saved_fpr_regnos.begin(), cfg->sideband.saved_fpr_regnos.end());
  cfg->sideband.max_regno = std::max(cfg->sideband.max_regno, NextFreshPseudo(*cfg));
}

bool IsMipsRelativeBranchWord(uint32_t word) {
  const uint32_t op = (word >> 26) & 0x3fu;
  if (op == 1u) {
    // REGIMM branches: bltz(0), bgez(1), bltzl(2), bgezl(3),
    // bltzal(16), bgezal(17), bltzall(18), bgezall(19).
    const uint32_t rt = (word >> 16) & 0x1fu;
    return rt <= 3u || (rt >= 16u && rt <= 19u);
  }
  if ((op >= 4u && op <= 7u) || (op >= 20u && op <= 23u)) {
    // beq, bne, blez, bgtz, beql, bnel, blezl, bgtzl.
    return true;
  }
  if (op == 17u) {
    // COP1 branch: bc1f/bc1t/bc1fl/bc1tl (`rs == 8`).
    return ((word >> 21) & 0x1fu) == 8u;
  }
  return false;
}

bool IsMipsHasDelaySlotWord(uint32_t word) {
  if (IsMipsRelativeBranchWord(word)) {
    return true;
  }
  const uint32_t op = (word >> 26) & 0x3fu;
  if (op == 2u || op == 3u) {
    // `j`, `jal`.
    return true;
  }
  if (op == 0u) {
    const uint32_t func = word & 0x3fu;
    return func == 8u || func == 9u;  // `jr`, `jalr`.
  }
  return false;
}

bool IsMipsReturnJumpWord(uint32_t word) {
  // `jr $ra`: SPECIAL (`op == 0`), `rs == 31`, `func == 8`.
  return ((word >> 26) & 0x3fu) == 0u && ((word >> 21) & 0x1fu) == 31u && (word & 0x3fu) == 8u;
}

// Attempts to delete `machine_words[start .. start + del_count)` from `input`,
// returning the rewritten `Stage0FunctionInput` (with updated relative branch
// offsets, relocation `word_index`es, and jump-table `target_word_indices`) or
// `std::nullopt` if deleting that range would break delay-slot pairing or
// invalidate a branch/jump-table target.
std::optional<Stage0FunctionInput> TryDeleteStage0WordRange(const Stage0FunctionInput& input,
                                                            size_t start, size_t del_count) {
  const size_t n = input.machine_words.size();
  const size_t end = start + del_count;
  if (del_count == 0 || end > n || n - del_count < 2) {
    return std::nullopt;
  }
  // Never delete the delay slot of a surviving branch/jump/call immediately
  // before `start`, and never delete a branch/jump/call at `end - 1` while
  // leaving its delay slot orphaned at `end`.
  if (start > 0 && IsMipsHasDelaySlotWord(input.machine_words[start - 1])) {
    return std::nullopt;
  }
  if (end < n && IsMipsHasDelaySlotWord(input.machine_words[end - 1])) {
    return std::nullopt;
  }
  // Never delete a function return (`jr $ra`).
  for (size_t k = start; k < end; ++k) {
    if (IsMipsReturnJumpWord(input.machine_words[k])) {
      return std::nullopt;
    }
  }
  // Never delete a word targeted by any jump-table entry.
  for (const Stage0JumpTable& jt : input.jump_tables) {
    for (size_t target : jt.target_word_indices) {
      if (target >= start && target < end) {
        return std::nullopt;
      }
    }
  }

  auto map_index = [start, end, del_count](int64_t old_idx) -> int64_t {
    return (old_idx >= static_cast<int64_t>(end)) ? (old_idx - static_cast<int64_t>(del_count))
                                                  : old_idx;
  };

  Stage0FunctionInput cand = input;
  cand.machine_words.clear();
  cand.machine_words.reserve(n - del_count);
  for (size_t old_i = 0; old_i < n; ++old_i) {
    if (old_i >= start && old_i < end) {
      continue;
    }
    uint32_t word = input.machine_words[old_i];
    if (IsMipsRelativeBranchWord(word)) {
      const auto old_off = static_cast<int16_t>(word & 0xffffu);
      const int64_t old_target = static_cast<int64_t>(old_i) + 1 + static_cast<int64_t>(old_off);
      if (old_target >= static_cast<int64_t>(start) && old_target < static_cast<int64_t>(end)) {
        return std::nullopt;
      }
      const int64_t new_i = map_index(static_cast<int64_t>(old_i));
      const int64_t new_target = map_index(old_target);
      const int64_t new_off = new_target - (new_i + 1);
      if (new_off < std::numeric_limits<int16_t>::min() ||
          new_off > std::numeric_limits<int16_t>::max()) {
        return std::nullopt;
      }
      word = (word & 0xffff0000u) | static_cast<uint16_t>(static_cast<int16_t>(new_off));
    }
    cand.machine_words.push_back(word);
  }

  // Update relocations: drop relocations belonging to deleted words, shift later ones.
  cand.relocations.clear();
  cand.relocations.reserve(input.relocations.size());
  for (Stage0Relocation rel : input.relocations) {
    if (rel.word_index >= start && rel.word_index < end) {
      continue;
    }
    if (rel.word_index >= end) {
      rel.word_index -= del_count;
    }
    cand.relocations.push_back(std::move(rel));
  }

  // Shift jump-table targets past the deleted range.
  for (Stage0JumpTable& jt : cand.jump_tables) {
    for (size_t& target : jt.target_word_indices) {
      if (target >= end) {
        target -= del_count;
      }
    }
  }
  return cand;
}

}  // namespace

std::vector<size_t> GetStratifiedCorpusCaseIndices(int count) {
  if (count <= 0) {
    return {};
  }
  const auto& all_cases = GetSharedRoundTripTestCases();
  const auto& profiles = GetRoundTripCompilerFlagProfiles();
  const size_t num_profiles = profiles.size();
  if (all_cases.empty() || num_profiles == 0) {
    return {};
  }
  std::vector<std::vector<size_t>> by_profile(num_profiles);

  for (size_t i = 0; i < all_cases.size(); ++i) {
    size_t matched = 0;
    for (size_t p = 0; p < num_profiles; ++p) {
      if (all_cases[i].options.c_flags == profiles[p].c_flags) {
        matched = p;
        break;
      }
    }
    by_profile[matched].push_back(i);
  }

  std::vector<size_t> result;
  result.reserve(static_cast<size_t>(count));
  const size_t base_quota = static_cast<size_t>(count) / num_profiles;
  const size_t rem = static_cast<size_t>(count) % num_profiles;

  for (size_t p = 0; p < num_profiles; ++p) {
    const size_t quota = base_quota + (p < rem ? 1 : 0);
    const auto& bucket = by_profile[p];
    if (bucket.empty() || quota == 0) {
      continue;
    }
    for (size_t k = 0; k < quota; ++k) {
      const size_t idx = (k * bucket.size()) / quota;
      result.push_back(bucket[idx]);
    }
  }
  return result;
}

int NextFreshPseudo(const RtlControlFlowGraph& cfg) {
  int next_reg = std::max(cfg.sideband.max_regno, kMipsFirstUserPseudoRegister);
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& raw : blk.instructions) {
      auto parsed = ParsePatternAndNotes(raw);
      if (!parsed.ok()) {
        continue;
      }
      parsed->pattern.Visit([&](const RtlExpr& n) {
        if (n.IsReg() && n.RegNo() >= next_reg) {
          next_reg = n.RegNo() + 1;
        }
      });
      for (const RtlRegNote& note : parsed->reg_notes) {
        note.datum.Visit([&](const RtlExpr& n) {
          if (n.IsReg() && n.RegNo() >= next_reg) {
            next_reg = n.RegNo() + 1;
          }
        });
      }
    }
  }
  return next_reg;
}

std::string FormatPn(const RtlPatternAndNotes& pn) {
  return FormatGccPrintRtlPatternAndNotes(pn.pattern, pn.reg_notes);
}

std::string FormatRegAtom(RtlMachineMode mode, int regno) {
  RtlExpr reg = RtlExpr::MakeReg(mode, regno);
  // GCC 2.7.2 `config/mips/mips.h:3141-3146` (`DEBUG_REGISTER_NAMES`) prints
  // reg 30 as `$fp` (whereas `core/rtl_expr.cc` defaults to `fp`).
  if (regno == 30) {
    reg.str_val = "$fp";
  }
  return reg.ToGccPrintRtlString();
}

absl::btree_set<int> CollectAllUsedHardRegsInCfg(const RtlControlFlowGraph& cfg,
                                                 const CompilationProfile& profile) {
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  absl::btree_set<int> used;
  auto mark_expr = [&](const RtlExpr& expr) {
    expr.Visit([&](const RtlExpr& n) {
      if (n.IsHardReg()) {
        const int r = n.RegNo();
        const int nregs = std::max(1, mm.HardRegNRegs(r, n.mode));
        for (int k = 0; k < nregs; ++k) {
          used.insert(r + k);
        }
      }
    });
  };
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      mark_expr(parsed->pattern);
      for (const RtlRegNote& note : parsed->reg_notes) {
        mark_expr(note.datum);
      }
      if (i < blk.rtl_insns.size()) {
        for (const RtlExpr& fu : blk.rtl_insns[i].call_fusage) {
          mark_expr(fu);
        }
      }
    }
  }
  for (const auto& [b, live_set] : cfg.sideband.basic_block_live_at_start) {
    for (int r : live_set) {
      if (r >= 0 && r < kMipsFirstPseudoRegister) {
        used.insert(r);
      }
    }
  }
  for (int r : cfg.sideband.saved_gpr_regnos) {
    used.insert(r);
  }
  for (int r : cfg.sideband.saved_fpr_regnos) {
    used.insert(r);
    used.insert(r + 1);
  }
  if (cfg.sideband.epilogue_delay_pattern.has_value()) {
    auto parsed = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
    if (parsed.ok()) {
      mark_expr(parsed->pattern);
    }
  }
  return used;
}

std::vector<int> PickFreeCallerSavedGprs(const absl::btree_set<int>& used, int count) {
  static constexpr int kCallerGprs[] = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25};
  std::vector<int> out;
  if (count <= 0) {
    return out;
  }
  for (int r : kCallerGprs) {
    if (!used.contains(r)) {
      out.push_back(r);
      if (static_cast<int>(out.size()) == count) {
        break;
      }
    }
  }
  return out;
}

std::vector<int> PickFreeCallerSavedFprs(const absl::btree_set<int>& used, int count) {
  static constexpr int kCallerFprs[] = {36, 38, 40, 42, 48, 50};
  std::vector<int> out;
  if (count <= 0) {
    return out;
  }
  for (int r : kCallerFprs) {
    if (!used.contains(r) && !used.contains(r + 1)) {
      out.push_back(r);
      if (static_cast<int>(out.size()) == count) {
        break;
      }
    }
  }
  return out;
}

std::vector<int> PickFreeCalleeSavedGprs(const absl::btree_set<int>& used, int count) {
  static constexpr int kCalleeGprs[] = {16, 17, 18, 19, 20, 21, 22, 23};
  std::vector<int> out;
  if (count <= 0) {
    return out;
  }
  for (int r : kCalleeGprs) {
    if (!used.contains(r)) {
      out.push_back(r);
      if (static_cast<int>(out.size()) == count) {
        break;
      }
    }
  }
  return out;
}

std::vector<int> PickFreeCalleeSavedFprs(const absl::btree_set<int>& used, int count) {
  static constexpr int kCalleeFprs[] = {52, 54, 56, 58, 60, 62};
  std::vector<int> out;
  if (count <= 0) {
    return out;
  }
  for (int r : kCalleeFprs) {
    if (!used.contains(r) && !used.contains(r + 1)) {
      out.push_back(r);
      if (static_cast<int>(out.size()) == count) {
        break;
      }
    }
  }
  return out;
}

void InsertInsnsAt(RtlBasicBlock* blk, size_t pos, absl::Span<const RtlInsn> to_insert) {
  const size_t clamped = std::min(pos, blk->instructions.size());
  std::vector<std::string> texts;
  texts.reserve(to_insert.size());
  for (const RtlInsn& insn : to_insert) {
    texts.push_back(insn.canonical_pattern);
  }
  blk->instructions.insert(blk->instructions.begin() + static_cast<std::ptrdiff_t>(clamped),
                           texts.begin(), texts.end());
  if (blk->rtl_insns.size() + to_insert.size() == blk->instructions.size()) {
    blk->rtl_insns.insert(blk->rtl_insns.begin() + static_cast<std::ptrdiff_t>(clamped),
                          to_insert.begin(), to_insert.end());
  } else {
    blk->SyncRtlInsnsFromInstructions();
  }
}

RtlInsn MakeRtlInsn(std::string pattern, std::vector<RtlExpr> call_fusage,
                    bool has_explicit_fusage) {
  RtlInsn insn;
  insn.canonical_pattern = std::move(pattern);
  insn.is_synthetic_rtl_expansion = true;
  insn.loop_note_kind = ParseRtlLoopNoteKind(insn.canonical_pattern);
  insn.has_explicit_call_fusage = has_explicit_fusage;
  insn.call_fusage = std::move(call_fusage);
  return insn;
}

void RefreshLivenessPreservingMemDeadNotes(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile, bool reload_completed,
                                           bool update_reg_notes) {
  absl::btree_map<std::pair<size_t, size_t>, std::vector<RtlRegNote>> saved_mem_dead;
  if (reload_completed && update_reg_notes) {
    for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
      const RtlBasicBlock& blk = cfg->rpo_blocks[b];
      for (size_t i = 0; i < blk.instructions.size(); ++i) {
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok()) {
          continue;
        }
        for (const RtlRegNote& note : parsed->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.IsMem()) {
            saved_mem_dead[{b, i}].push_back(note);
          }
        }
      }
    }
  }

  const bool do_notes = update_reg_notes && (profile.optimize_level > 0);
  ReconstructRtlLiveness(cfg, profile,
                         {.reload_completed = reload_completed,
                          .update_reg_notes = do_notes,
                          .preserve_phantom_pseudos = !reload_completed});

  for (const auto& [key, mem_notes] : saved_mem_dead) {
    auto& blk = cfg->rpo_blocks[key.first];
    if (key.second >= blk.instructions.size()) {
      continue;
    }
    auto parsed = ParsePatternAndNotes(blk.instructions[key.second]);
    if (!parsed.ok()) {
      continue;
    }
    for (const RtlRegNote& mn : mem_notes) {
      parsed->reg_notes.push_back(mn);
    }
    blk.instructions[key.second] = FormatPn(*parsed);
    blk.SyncRtlInsnsFromInstructions();
  }
  cfg->sideband.max_regno = std::max(cfg->sideband.max_regno, NextFreshPseudo(*cfg));
}

bool ApplyPseudoAlphaRename(RtlControlFlowGraph* cfg, std::mt19937_64* rng) {
  absl::btree_set<int> pseudos;
  auto collect_pseudos = [&](const RtlExpr& expr) {
    expr.Visit([&](const RtlExpr& n) {
      if (n.IsReg() && n.RegNo() >= kMipsFirstUserPseudoRegister) {
        pseudos.insert(n.RegNo());
      }
    });
  };
  for (const RtlBasicBlock& blk : cfg->rpo_blocks) {
    for (const std::string& raw : blk.instructions) {
      auto parsed = ParsePatternAndNotes(raw);
      if (!parsed.ok()) {
        continue;
      }
      collect_pseudos(parsed->pattern);
      for (const RtlRegNote& note : parsed->reg_notes) {
        collect_pseudos(note.datum);
      }
    }
    for (const RtlInsn& insn : blk.rtl_insns) {
      for (const RtlExpr& fusage : insn.call_fusage) {
        collect_pseudos(fusage);
      }
    }
  }
  for (const auto& [b, live] : cfg->sideband.basic_block_live_at_start) {
    for (int r : live) {
      if (r >= kMipsFirstUserPseudoRegister) {
        pseudos.insert(r);
      }
    }
  }
  for (const auto& [r, _] : cfg->sideband.reg_n_refs) {
    if (r >= kMipsFirstUserPseudoRegister) {
      pseudos.insert(r);
    }
  }
  for (const auto& [r, _] : cfg->sideband.reg_n_sets) {
    if (r >= kMipsFirstUserPseudoRegister) {
      pseudos.insert(r);
    }
  }
  for (const auto& [r, _] : cfg->sideband.reg_n_deaths) {
    if (r >= kMipsFirstUserPseudoRegister) {
      pseudos.insert(r);
    }
  }
  for (const auto& [r, _] : cfg->sideband.reg_live_length) {
    if (r >= kMipsFirstUserPseudoRegister) {
      pseudos.insert(r);
    }
  }
  for (const auto& [r, _] : cfg->sideband.reg_n_calls_crossed) {
    if (r >= kMipsFirstUserPseudoRegister) {
      pseudos.insert(r);
    }
  }
  for (const auto& [r, _] : cfg->sideband.reg_renumber) {
    if (r >= kMipsFirstUserPseudoRegister) {
      pseudos.insert(r);
    }
  }
  for (int r : cfg->sideband.pointer_regnos) {
    if (r >= kMipsFirstUserPseudoRegister) {
      pseudos.insert(r);
    }
  }
  if (pseudos.empty()) {
    return false;
  }

  std::vector<int> orig_list(pseudos.begin(), pseudos.end());
  std::vector<int> targets = orig_list;
  const int shift = 1 + static_cast<int>((*rng)() % 8);
  if (targets.size() >= 2) {
    std::shuffle(targets.begin(), targets.end(), *rng);
  }
  bool changed = false;
  for (size_t i = 0; i < orig_list.size(); ++i) {
    if (targets[i] != orig_list[i]) {
      changed = true;
    }
  }
  if (!changed || ((*rng)() % 2 == 0)) {
    for (int& t : targets) {
      t += shift;
    }
  }
  absl::btree_map<int, int> rename_map;
  for (size_t i = 0; i < orig_list.size(); ++i) {
    rename_map[orig_list[i]] = targets[i];
  }
  ApplyRegRenameMapToCfg(rename_map, cfg);
  return true;
}

bool ApplyPostReloadHardRegAlphaRename(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                                       std::mt19937_64* rng) {
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  const absl::btree_set<int> used = CollectAllUsedHardRegsInCfg(*cfg, profile);
  absl::btree_set<int> pinned;
  for (int r : {0,  1,  2,  3,  4,  5,  6,  7,  26, 27, 28, 29, 31,
                32, 33, 34, 35, 44, 45, 46, 47, 64, 65, 66, 67}) {
    pinned.insert(r);
  }
  if (cfg->sideband.frame_pointer_needed) {
    pinned.insert(30);
  }
  bool has_prologue_sp_adjust = false;
  if (!cfg->rpo_blocks.empty()) {
    const sched_model::ParsedSchedRegion r0 =
        sched_model::ParseBlockForSchedModel(cfg->rpo_blocks[0], 0, sched_model::Phase::kSched2);
    if (r0.valid) {
      has_prologue_sp_adjust =
          (sched_model::FindPostReloadBodyBounds(r0.sched_block, 0).prologue_end > 0);
    }
  }
  if (has_prologue_sp_adjust) {
    for (int r : cfg->sideband.saved_gpr_regnos) {
      pinned.insert(r);
    }
    for (int r : cfg->sideband.saved_fpr_regnos) {
      pinned.insert(r);
      pinned.insert(r + 1);
    }
  }

  absl::btree_set<int> wide_regs;
  absl::btree_set<int> body_regs;
  for (const RtlBasicBlock& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      parsed->pattern.Visit([&](const RtlExpr& n) {
        if (!n.IsHardReg()) {
          return;
        }
        const int r = n.RegNo();
        body_regs.insert(r);
        if (n.flags.integrated) {
          pinned.insert(r);
        }
        if (mm.HardRegNRegs(r, n.mode) > 1 || (r >= 32 && r < 64)) {
          wide_regs.insert(r);
          if (r % 2 == 1) {
            pinned.insert(r);
            pinned.insert(r - 1);
          } else if (mm.HardRegNRegs(r, n.mode) > 1) {
            wide_regs.insert(r + 1);
          }
        }
      });
      if (i < blk.rtl_insns.size()) {
        for (const RtlExpr& fu : blk.rtl_insns[i].call_fusage) {
          fu.Visit([&](const RtlExpr& n) {
            if (n.IsHardReg()) {
              pinned.insert(n.RegNo());
            }
          });
        }
      }
    }
  }
  if (cfg->sideband.epilogue_delay_pattern.has_value()) {
    auto parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (parsed.ok()) {
      parsed->pattern.Visit([&](const RtlExpr& n) {
        if (n.IsHardReg()) {
          pinned.insert(n.RegNo());
        }
      });
    }
  }

  absl::btree_map<int, int> rename_map;
  absl::btree_set<int> occupied = used;
  auto try_rename_from_pool = [&](const std::vector<int>& pool, bool is_pair) {
    std::vector<int> free_targets;
    for (int cand : pool) {
      if (!occupied.contains(cand) && (!is_pair || !occupied.contains(cand + 1))) {
        free_targets.push_back(cand);
      }
    }
    std::shuffle(free_targets.begin(), free_targets.end(), *rng);
    size_t free_cursor = 0;
    for (int r : pool) {
      if (free_cursor >= free_targets.size()) {
        break;
      }
      if (!body_regs.contains(r) || pinned.contains(r) || rename_map.contains(r)) {
        continue;
      }
      if (is_pair) {
        if (r % 2 != 0 || pinned.contains(r + 1)) {
          continue;
        }
        const int dst = free_targets[free_cursor++];
        rename_map[r] = dst;
        rename_map[r + 1] = dst + 1;
        occupied.insert(dst);
        occupied.insert(dst + 1);
      } else {
        if (wide_regs.contains(r)) {
          continue;
        }
        const int dst = free_targets[free_cursor++];
        rename_map[r] = dst;
        occupied.insert(dst);
      }
    }
  };

  const std::vector<int> caller_gprs = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25};
  const std::vector<int> callee_gprs = {16, 17, 18, 19, 20, 21, 22, 23};
  const std::vector<int> caller_fprs = {36, 38, 40, 42, 48, 50};
  const std::vector<int> callee_fprs = {52, 54, 56, 58, 60, 62};
  try_rename_from_pool(caller_gprs, /*is_pair=*/false);
  try_rename_from_pool(callee_gprs, /*is_pair=*/false);
  try_rename_from_pool(caller_fprs, /*is_pair=*/true);
  try_rename_from_pool(callee_fprs, /*is_pair=*/true);
  try_rename_from_pool({8, 10, 12, 14, 24}, /*is_pair=*/true);
  try_rename_from_pool({16, 18, 20, 22}, /*is_pair=*/true);

  if (rename_map.empty()) {
    return false;
  }
  ApplyRegRenameMapToCfg(rename_map, cfg);
  return true;
}

RtlControlFlowGraph GreedyShrinkRtlCfg(
    const RtlControlFlowGraph& failing_cfg, const CompilationProfile& profile,
    bool reload_completed, absl::FunctionRef<bool(const RtlControlFlowGraph&)> still_fails,
    int max_passes, int max_checks) {
  RtlControlFlowGraph best = failing_cfg;
  int checks = 0;

  if (best.rpo_blocks.size() > 1 && checks < max_checks) {
    RtlControlFlowGraph single = best;
    single.rpo_blocks.resize(1);
    single.rpo_blocks[0].successors.clear();
    single.rpo_blocks[0].predecessors.clear();
    if (!single.rpo_blocks[0].instructions.empty()) {
      auto parsed = ParsePatternAndNotes(single.rpo_blocks[0].instructions.back());
      if (parsed.ok() && parsed->pattern.IsSet() && parsed->pattern.SetDest().op == RtlOp::kPc) {
        single.rpo_blocks[0].instructions.pop_back();
        if (!single.rpo_blocks[0].rtl_insns.empty()) {
          single.rpo_blocks[0].rtl_insns.pop_back();
        }
      }
    }
    RefreshLivenessPreservingMemDeadNotes(&single, profile, reload_completed,
                                          /*update_reg_notes=*/true);
    ++checks;
    if (!single.rpo_blocks[0].instructions.empty() && still_fails(single)) {
      best = std::move(single);
    }
  }

  for (size_t chunk : {size_t{4}, size_t{1}}) {
    for (int pass = 0; pass < max_passes && best.TotalInsnCount() > 3 && checks < max_checks;
         ++pass) {
      bool shrunk_this_pass = false;
      for (size_t b = 0; b < best.rpo_blocks.size() && checks < max_checks; ++b) {
        size_t rev = best.rpo_blocks[b].instructions.size();
        while (rev > 0 && checks < max_checks) {
          const size_t del_count = std::min(chunk, rev);
          const size_t start = rev - del_count;
          rev = start;
          if (best.rpo_blocks[b].instructions.size() <= del_count) {
            continue;
          }
          bool touches_jump = false;
          if (best.rpo_blocks.size() > 1) {
            for (size_t k = start; k < start + del_count; ++k) {
              auto parsed = ParsePatternAndNotes(best.rpo_blocks[b].instructions[k]);
              if (parsed.ok() && parsed->pattern.IsSet() &&
                  parsed->pattern.SetDest().op == RtlOp::kPc) {
                touches_jump = true;
                break;
              }
            }
          }
          if (touches_jump) {
            continue;
          }
          RtlControlFlowGraph cand = best;
          cand.rpo_blocks[b].instructions.erase(
              cand.rpo_blocks[b].instructions.begin() + static_cast<std::ptrdiff_t>(start),
              cand.rpo_blocks[b].instructions.begin() +
                  static_cast<std::ptrdiff_t>(start + del_count));
          if (start + del_count <= cand.rpo_blocks[b].rtl_insns.size()) {
            cand.rpo_blocks[b].rtl_insns.erase(
                cand.rpo_blocks[b].rtl_insns.begin() + static_cast<std::ptrdiff_t>(start),
                cand.rpo_blocks[b].rtl_insns.begin() +
                    static_cast<std::ptrdiff_t>(start + del_count));
          }
          if (!reload_completed) {
            RefreshLivenessPreservingMemDeadNotes(&cand, profile, reload_completed,
                                                  /*update_reg_notes=*/true);
            bool has_dead_subreg = false;
            for (const RtlBasicBlock& cb : cand.rpo_blocks) {
              for (const std::string& raw : cb.instructions) {
                auto p = ParsePatternAndNotes(raw);
                if (!p.ok() || !p->pattern.IsSet() || !p->pattern.SetDest().IsSubreg()) {
                  continue;
                }
                for (const RtlRegNote& rn : p->reg_notes) {
                  if (rn.kind == RtlMachineMode::kRegUnused) {
                    has_dead_subreg = true;
                    break;
                  }
                }
                if (has_dead_subreg) {
                  break;
                }
              }
              if (has_dead_subreg) {
                break;
              }
            }
            if (has_dead_subreg) {
              continue;
            }
          }
          ++checks;
          if (still_fails(cand)) {
            best = std::move(cand);
            shrunk_this_pass = true;
            if (best.TotalInsnCount() <= 3) {
              break;
            }
          }
        }
      }
      if (!shrunk_this_pass) {
        break;
      }
    }
  }
  return best;
}

Stage0FunctionInput GreedyShrinkStage0Input(
    const Stage0FunctionInput& failing_input,
    absl::FunctionRef<bool(const Stage0FunctionInput&)> still_fails, int max_passes,
    int max_checks) {
  Stage0FunctionInput best = failing_input;
  int checks = 0;

  for (size_t chunk : {size_t{4}, size_t{2}, size_t{1}}) {
    for (int pass = 0; pass < max_passes && best.machine_words.size() > 2 && checks < max_checks;
         ++pass) {
      bool shrunk_this_pass = false;
      size_t rev = best.machine_words.size();
      while (rev > 0 && checks < max_checks && best.machine_words.size() > 2) {
        const size_t del_count = std::min(chunk, rev);
        const size_t start = rev - del_count;
        rev = start;
        auto cand = TryDeleteStage0WordRange(best, start, del_count);
        if (!cand.has_value()) {
          continue;
        }
        ++checks;
        if (still_fails(*cand)) {
          best = std::move(*cand);
          shrunk_this_pass = true;
        }
      }
      if (!shrunk_this_pass) {
        break;
      }
    }
  }
  return best;
}

}  // namespace rom_nom_nom::lifter
