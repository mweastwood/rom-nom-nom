#include "lifter/invert_update_equiv_regs.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

bool ContainsUnknownOp(const RtlExpr& expr) {
  if (expr.op == RtlOp::kUnknown) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (ContainsUnknownOp(child)) {
      return true;
    }
  }
  return false;
}

absl::Status ValidateRegNumbers(const RtlExpr& expr) {
  if (expr.IsReg() && expr.RegNo() < 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("InvertUpdateEquivRegs: negative register number ", expr.RegNo()));
  }
  for (const RtlExpr& child : expr.children) {
    absl::Status status = ValidateRegNumbers(child);
    if (!status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::Status ValidateSideband(const RtlSidebandState& sideband) {
  for (const auto& [pseudo, hard] : sideband.reg_renumber) {
    if (pseudo < kMipsFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertUpdateEquivRegs: invalid reg_renumber key ", pseudo, " (< ",
                       kMipsFirstPseudoRegister, ")"));
    }
    if (hard < -1 || hard >= kMipsFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrCat(
          "InvertUpdateEquivRegs: invalid reg_renumber target ", hard, " for pseudo ", pseudo));
    }
  }
  for (const auto& [regno, refs] : sideband.reg_n_refs) {
    (void)refs;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertUpdateEquivRegs: negative regno in reg_n_refs: ", regno));
    }
  }
  for (const auto& [regno, sets] : sideband.reg_n_sets) {
    (void)sets;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertUpdateEquivRegs: negative regno in reg_n_sets: ", regno));
    }
  }
  for (const auto& [regno, deaths] : sideband.reg_n_deaths) {
    (void)deaths;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertUpdateEquivRegs: negative regno in reg_n_deaths: ", regno));
    }
  }
  for (const auto& [regno, len] : sideband.reg_live_length) {
    (void)len;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertUpdateEquivRegs: negative regno in reg_live_length: ", regno));
    }
  }
  for (const auto& [regno, calls] : sideband.reg_n_calls_crossed) {
    (void)calls;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertUpdateEquivRegs: negative regno in reg_n_calls_crossed: ", regno));
    }
  }
  for (const auto& [bb_idx, live_regs] : sideband.basic_block_live_at_start) {
    if (bb_idx < 0) {
      return absl::InvalidArgumentError(absl::StrCat(
          "InvertUpdateEquivRegs: negative basic_block_live_at_start block index ", bb_idx));
    }
    for (int regno : live_regs) {
      if (regno < 0) {
        return absl::InvalidArgumentError(absl::StrCat(
            "InvertUpdateEquivRegs: negative regno in basic_block_live_at_start: ", regno));
      }
    }
  }
  for (int regno : sideband.saved_gpr_regnos) {
    if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertUpdateEquivRegs: invalid saved_gpr_regno ", regno));
    }
  }
  for (int regno : sideband.saved_fpr_regnos) {
    if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertUpdateEquivRegs: invalid saved_fpr_regno ", regno));
    }
  }
  return absl::OkStatus();
}

// Unwraps optional top-level `(insn ...)`, `(jump_insn ...)`, or `(call_insn ...)`
// wrapper so helpers can uniformly inspect the inner RTL pattern.
const RtlExpr& UnwrapInsnContainer(const RtlExpr& expr) {
  if ((expr.op == RtlOp::kInsn || expr.op == RtlOp::kJumpInsn || expr.op == RtlOp::kCallInsn) &&
      !expr.children.empty()) {
    return expr.children[0];
  }
  return expr;
}

RtlExpr* MutableUnwrapInsnContainer(RtlExpr* expr) {
  if ((expr->op == RtlOp::kInsn || expr->op == RtlOp::kJumpInsn || expr->op == RtlOp::kCallInsn) &&
      !expr->children.empty()) {
    return &expr->children[0];
  }
  return expr;
}

// Matches GCC 2.7.2 `CONSTANT_P(x)` (`rtl.h:269`):
// `LABEL_REF`, `SYMBOL_REF`, `CONST_INT`, `CONST_DOUBLE`, `CONST`, `HIGH`.
bool IsConstantP(const RtlExpr& expr) {
  switch (expr.op) {
    case RtlOp::kConstInt:
    case RtlOp::kConstDouble:
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
    case RtlOp::kConst:
    case RtlOp::kHigh:
      return true;
    default:
      return false;
  }
}

// Extracts the single `(set dest src)` node from an instruction pattern,
// matching GCC's `single_set(insn)` (either a top-level `SET` or a `PARALLEL`
// containing exactly one `SET` alongside only `USE` or `CLOBBER` elements).
const RtlExpr* FindSingleSet(const RtlExpr& raw_pattern) {
  const RtlExpr& pattern = UnwrapInsnContainer(raw_pattern);
  if (pattern.IsSet()) {
    return &pattern;
  }
  if (pattern.IsParallel()) {
    const RtlExpr* single = nullptr;
    for (const RtlExpr& elem : pattern.children) {
      if (elem.IsSet()) {
        if (single != nullptr) {
          return nullptr;
        }
        single = &elem;
      } else if (elem.op != RtlOp::kUse && elem.op != RtlOp::kClobber) {
        return nullptr;
      }
    }
    return single;
  }
  return nullptr;
}

void CollectAllRegsInExpr(const RtlExpr& expr, absl::flat_hash_set<int>* out_regs) {
  if (expr.IsReg() && expr.RegNo() >= 0) {
    out_regs->insert(expr.RegNo());
  }
  for (const RtlExpr& child : expr.children) {
    CollectAllRegsInExpr(child, out_regs);
  }
}

void CollectDefinedRegsInPattern(const RtlExpr& raw_pattern,
                                 absl::flat_hash_map<int, int>* def_counts) {
  const RtlExpr& pattern = UnwrapInsnContainer(raw_pattern);
  auto record_dest = [&](const RtlExpr& dest_expr) {
    const RtlExpr* d = &dest_expr;
    while (d->op == RtlOp::kSubreg || d->op == RtlOp::kStrictLowPart ||
           d->op == RtlOp::kZeroExtract || d->op == RtlOp::kSignExtract) {
      if (d->children.empty()) {
        break;
      }
      d = &d->children[0];
    }
    if (d->IsReg() && d->RegNo() >= 0) {
      (*def_counts)[d->RegNo()] += 1;
    }
  };

  if ((pattern.op == RtlOp::kSet || pattern.op == RtlOp::kClobber) && !pattern.children.empty()) {
    record_dest(pattern.children[0]);
    return;
  }
  if (pattern.IsParallel()) {
    for (const RtlExpr& elem : pattern.children) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
        record_dest(elem.children[0]);
      }
    }
  }
}

bool IsRegSetInPattern(const RtlExpr& raw_pattern, int regno) {
  absl::flat_hash_map<int, int> defs;
  CollectDefinedRegsInPattern(raw_pattern, &defs);
  return defs.contains(regno);
}

bool ContainsCall(const RtlExpr& expr) {
  if (expr.op == RtlOp::kCall || expr.op == RtlOp::kCallInsn) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (ContainsCall(child)) {
      return true;
    }
  }
  return false;
}

bool WritesToMemory(const RtlExpr& raw_pattern) {
  const RtlExpr& pattern = UnwrapInsnContainer(raw_pattern);
  auto is_mem_dest = [](const RtlExpr& dest_expr) {
    const RtlExpr* d = &dest_expr;
    while (d->op == RtlOp::kSubreg || d->op == RtlOp::kStrictLowPart ||
           d->op == RtlOp::kZeroExtract || d->op == RtlOp::kSignExtract) {
      if (d->children.empty()) {
        break;
      }
      d = &d->children[0];
    }
    return d->op == RtlOp::kMem;
  };
  if (pattern.op == RtlOp::kSet && !pattern.children.empty()) {
    return is_mem_dest(pattern.children[0]);
  }
  if (pattern.IsParallel()) {
    for (const RtlExpr& elem : pattern.children) {
      if (elem.op == RtlOp::kSet && !elem.children.empty() && is_mem_dest(elem.children[0])) {
        return true;
      }
    }
  }
  return false;
}

int CountRegOccurrences(const RtlExpr& expr, int regno) {
  int count = (expr.IsReg() && expr.RegNo() == regno) ? 1 : 0;
  for (const RtlExpr& child : expr.children) {
    count += CountRegOccurrences(child, regno);
  }
  return count;
}

int ReplaceRegInExpr(RtlExpr* expr, int from_regno, const RtlExpr& replacement_reg) {
  if (expr->IsReg() && expr->RegNo() == from_regno) {
    const RtlMachineMode preserved_mode =
        expr->mode != RtlMachineMode::kNone ? expr->mode : replacement_reg.mode;
    *expr = replacement_reg;
    expr->mode = preserved_mode;
    return 1;
  }
  int replaced = 0;
  for (RtlExpr& child : expr->children) {
    replaced += ReplaceRegInExpr(&child, from_regno, replacement_reg);
  }
  return replaced;
}

int FindRegDeadNoteIndex(const std::vector<RtlRegNote>& notes, int regno) {
  for (size_t i = 0; i < notes.size(); ++i) {
    if (notes[i].kind == RtlMachineMode::kRegDead && notes[i].datum.IsReg() &&
        notes[i].datum.RegNo() == regno) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Checks whether `validate_equiv_mem(start_insn, regno, mem_expr)` in
// `local-alloc.c:588-655` succeeds within `parsed_insns` starting after
// `start_idx`.
bool CanValidateEquivMem(const std::vector<RtlPatternAndNotes>& parsed_insns, size_t start_idx,
                         int regno, const RtlExpr& mem_expr) {
  if (mem_expr.flags.volatil) {
    return false;
  }
  absl::flat_hash_set<int> mem_regs;
  CollectAllRegsInExpr(mem_expr, &mem_regs);
  if (mem_regs.contains(regno)) {
    return false;
  }
  for (const RtlRegNote& note : parsed_insns[start_idx].reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg() &&
        mem_regs.contains(note.datum.RegNo())) {
      return false;
    }
  }

  for (size_t i = start_idx + 1; i < parsed_insns.size(); ++i) {
    const RtlPatternAndNotes& pn = parsed_insns[i];
    if (pn.pattern.op == RtlOp::kNote || pn.pattern.op == RtlOp::kCodeLabel) {
      break;
    }
    const bool mentions_reg = CountRegOccurrences(pn.pattern, regno) > 0;
    const bool reg_dies = FindRegDeadNoteIndex(pn.reg_notes, regno) >= 0;
    if (mentions_reg && IsRegSetInPattern(pn.pattern, regno)) {
      return false;
    }
    if (mem_expr.flags.unchanging) {
      if (reg_dies) {
        return true;
      }
      continue;
    }
    if (ContainsCall(pn.pattern)) {
      return false;
    }
    for (int mr : mem_regs) {
      if (IsRegSetInPattern(pn.pattern, mr)) {
        return false;
      }
    }
    if (WritesToMemory(pn.pattern)) {
      return false;
    }
    if (reg_dies) {
      return true;
    }
  }
  return true;
}

// Checks whether a subsequent instruction in `parsed_insns` after `start_idx`
// is a single-set store `(set <mem_expr> (reg:M regno))` that triggers Rule A
// (`local-alloc.c:990-1000`).
bool HasSubsequentMatchingStore(const std::vector<RtlPatternAndNotes>& parsed_insns,
                                size_t start_idx, int regno, const RtlExpr& mem_expr) {
  for (size_t i = start_idx + 1; i < parsed_insns.size(); ++i) {
    const RtlExpr* set = FindSingleSet(parsed_insns[i].pattern);
    if (set == nullptr) {
      continue;
    }
    const RtlExpr& dest = set->SetDest();
    const RtlExpr& src = set->SetSrc();
    if (dest.op == RtlOp::kMem && src.IsReg() && src.RegNo() == regno && dest == mem_expr) {
      return true;
    }
  }
  return false;
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg) {
  if (lreg_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertUpdateEquivRegs: input CFG has no basic blocks");
  }

  const size_t num_blocks = lreg_cfg.rpo_blocks.size();
  absl::Status sideband_status = ValidateSideband(lreg_cfg.sideband);
  if (!sideband_status.ok()) {
    return sideband_status;
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = lreg_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrCat("InvertUpdateEquivRegs: block ", b,
                                                       " has out-of-bounds successor ", succ,
                                                       " (num_blocks=", num_blocks, ")"));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrCat("InvertUpdateEquivRegs: block ", b,
                                                       " has out-of-bounds addr_vec target ",
                                                       target, " (num_blocks=", num_blocks, ")"));
      }
    }
  }

  std::vector<std::vector<RtlPatternAndNotes>> block_asts(num_blocks);
  absl::flat_hash_map<int, int> ast_def_counts;
  absl::flat_hash_map<int, absl::flat_hash_set<size_t>> reg_blocks_used;

  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& insns = lreg_cfg.rpo_blocks[b].instructions;
    block_asts[b].resize(insns.size());
    for (size_t ii = 0; ii < insns.size(); ++ii) {
      const std::string& pat_str = insns[ii];
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(pat_str);
      if (!parsed.ok()) {
        return parsed.status();
      }
      if (parsed->pattern.IsNil() || ContainsUnknownOp(parsed->pattern)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "InvertUpdateEquivRegs: unrecognized RTL op in instruction pattern: ", pat_str));
      }
      absl::Status reg_status = ValidateRegNumbers(parsed->pattern);
      if (!reg_status.ok()) {
        return reg_status;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (ContainsUnknownOp(note.datum)) {
          return absl::InvalidArgumentError(
              absl::StrCat("InvertUpdateEquivRegs: unrecognized RTL op in note: ", pat_str));
        }
        absl::Status note_reg_status = ValidateRegNumbers(note.datum);
        if (!note_reg_status.ok()) {
          return note_reg_status;
        }
      }
      CollectDefinedRegsInPattern(parsed->pattern, &ast_def_counts);
      absl::flat_hash_set<int> regs_in_insn;
      CollectAllRegsInExpr(parsed->pattern, &regs_in_insn);
      for (int r : regs_in_insn) {
        reg_blocks_used[r].insert(b);
      }
      block_asts[b][ii] = *std::move(parsed);
    }
  }

  RtlControlFlowGraph out = lreg_cfg;
  out.pass = RtlPassKind::kSched;

  auto get_num_sets = [&](int regno) -> int {
    auto it = out.sideband.reg_n_sets.find(regno);
    if (it != out.sideband.reg_n_sets.end()) {
      return it->second;
    }
    auto ast_it = ast_def_counts.find(regno);
    return ast_it != ast_def_counts.end() ? ast_it->second : 0;
  };

  auto is_block_local = [&](int regno, size_t b) -> bool {
    for (const auto& [_, live_set] : out.sideband.basic_block_live_at_start) {
      if (live_set.contains(regno)) {
        return false;
      }
    }
    auto it = reg_blocks_used.find(regno);
    if (it == reg_blocks_used.end()) {
      return false;
    }
    return it->second.size() == 1 && it->second.contains(b);
  };

  // Pass 1: Reverse intra-block `optimize_reg_copy_1` transformations.
  for (size_t b = 0; b < num_blocks; ++b) {
    auto& asts = block_asts[b];
    for (size_t ii = 0; ii + 1 < asts.size(); ++ii) {
      const RtlExpr* copy_set = FindSingleSet(asts[ii].pattern);
      if (copy_set == nullptr) {
        continue;
      }
      const RtlExpr& dest_reg = copy_set->SetDest();
      const RtlExpr& src_reg = copy_set->SetSrc();
      if (!dest_reg.IsReg() || !src_reg.IsReg()) {
        continue;
      }
      const int dregno = dest_reg.RegNo();
      const int sregno = src_reg.RegNo();
      if (dregno < kMipsFirstPseudoRegister || sregno < kMipsFirstPseudoRegister ||
          dregno == sregno) {
        continue;
      }
      const int dead_idx = FindRegDeadNoteIndex(asts[ii].reg_notes, sregno);
      if (dead_idx < 0) {
        continue;
      }

      RtlPatternAndNotes& next_pn = asts[ii + 1];
      const RtlExpr& next_inner = UnwrapInsnContainer(next_pn.pattern);
      if (next_inner.op == RtlOp::kUse || next_inner.op == RtlOp::kNote ||
          next_inner.op == RtlOp::kCodeLabel) {
        continue;
      }
      if (IsRegSetInPattern(next_pn.pattern, dregno) ||
          IsRegSetInPattern(next_pn.pattern, sregno)) {
        continue;
      }
      if (FindRegDeadNoteIndex(next_pn.reg_notes, dregno) >= 0) {
        continue;
      }
      if (CountRegOccurrences(next_pn.pattern, dregno) <= 0) {
        continue;
      }

      bool dreg_used_after_next = false;
      bool sreg_used_after_copy = false;
      for (size_t jj = ii + 1; jj < asts.size(); ++jj) {
        if (CountRegOccurrences(asts[jj].pattern, sregno) > 0) {
          sreg_used_after_copy = true;
          break;
        }
        if (jj > ii + 1 && CountRegOccurrences(asts[jj].pattern, dregno) > 0) {
          dreg_used_after_next = true;
        }
      }
      if (sreg_used_after_copy || !dreg_used_after_next) {
        continue;
      }

      RtlExpr* next_mut = MutableUnwrapInsnContainer(&next_pn.pattern);
      const int replaced = ReplaceRegInExpr(next_mut, dregno, src_reg);
      if (replaced > 0) {
        RtlRegNote moved_dead = std::move(asts[ii].reg_notes[dead_idx]);
        asts[ii].reg_notes.erase(asts[ii].reg_notes.begin() + dead_idx);
        next_pn.reg_notes.push_back(std::move(moved_dead));

        auto live_it = out.sideband.reg_live_length.find(sregno);
        if (live_it != out.sideband.reg_live_length.end()) {
          live_it->second += 1;
        }
        auto sref_it = out.sideband.reg_n_refs.find(sregno);
        if (sref_it != out.sideband.reg_n_refs.end()) {
          sref_it->second += replaced;
        }
        auto dref_it = out.sideband.reg_n_refs.find(dregno);
        if (dref_it != out.sideband.reg_n_refs.end() && dref_it->second > replaced) {
          dref_it->second -= replaced;
        }
      }
    }
  }

  // Pass 2: Reverse `REG_EQUIV` note promotions/insertions and halve doubled
  // `sideband.reg_live_length` entries.
  for (size_t b = 0; b < num_blocks; ++b) {
    auto& asts = block_asts[b];
    for (size_t ii = 0; ii < asts.size(); ++ii) {
      RtlPatternAndNotes& pn = asts[ii];
      if (pn.reg_notes.empty()) {
        continue;
      }
      const RtlExpr* set = FindSingleSet(pn.pattern);
      if (set == nullptr) {
        continue;
      }
      const RtlExpr& dest = set->SetDest();
      const RtlExpr& src = set->SetSrc();
      if (!dest.IsReg()) {
        continue;
      }
      const int regno = dest.RegNo();
      if (regno < kMipsFirstPseudoRegister || get_num_sets(regno) != 1) {
        continue;
      }

      bool halved_live_length = false;
      for (auto it = pn.reg_notes.begin(); it != pn.reg_notes.end();) {
        if (it->kind != RtlMachineMode::kRegEquiv) {
          ++it;
          continue;
        }

        if (IsConstantP(it->datum)) {
          // `local-alloc.c:1031-1032` mutated `REG_EQUAL <const>` in-place to
          // `REG_EQUIV <const>`, and line 1064 doubled `reg_live_length[regno]`.
          it->kind = RtlMachineMode::kRegEqual;
          it->raw_kind.clear();
          if (!halved_live_length) {
            auto live_it = out.sideband.reg_live_length.find(regno);
            if (live_it != out.sideband.reg_live_length.end() && live_it->second > 1) {
              live_it->second /= 2;
            }
            halved_live_length = true;
          }
          ++it;
          continue;
        }

        if (it->datum.op == RtlOp::kMem) {
          if (is_block_local(regno, b) && !(src == it->datum) &&
              HasSubsequentMatchingStore(asts, ii, regno, it->datum)) {
            // Attached later by Rule A (`local-alloc.c:990-1000`) when a
            // subsequent store `(set (mem ...) (reg regno))` in the same basic
            // block was visited. Because `insn` had already passed line 1058,
            // this note did not trigger `reg_live_length[regno]` doubling.
            it = pn.reg_notes.erase(it);
            continue;
          }
          if (src.op == RtlOp::kMem) {
            // `note` was present at `local-alloc.c:1058` (either added at line
            // 1054 from `SET_SRC(set)` or pre-existing from `assign_parms`),
            // so `reg_live_length[regno]` was doubled at line 1064.
            if (!halved_live_length) {
              auto live_it = out.sideband.reg_live_length.find(regno);
              if (live_it != out.sideband.reg_live_length.end() && live_it->second > 1) {
                live_it->second /= 2;
              }
              halved_live_length = true;
            }
            if (is_block_local(regno, b) && src == it->datum &&
                CanValidateEquivMem(asts, ii, regno, src)) {
              it = pn.reg_notes.erase(it);
              continue;
            }
          }
        }
        ++it;
      }
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    const auto& asts = block_asts[b];
    bool modified_block = false;
    for (size_t ii = 0; ii < blk.instructions.size(); ++ii) {
      std::string formatted = asts[ii].ToSexprString();
      if (formatted != blk.instructions[ii]) {
        blk.instructions[ii] = std::move(formatted);
        modified_block = true;
      }
    }
    if (modified_block) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
