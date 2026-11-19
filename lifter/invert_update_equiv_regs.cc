#include "lifter/invert_update_equiv_regs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kArgPointerRegno = 0;
constexpr int kStackPointerRegno = 29;
constexpr int kFramePointerRegno = 30;

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

bool IsSymbolicPointerConstant(const RtlExpr& expr) {
  return expr.op == RtlOp::kSymbolRef || expr.op == RtlOp::kConst || expr.op == RtlOp::kLabelRef ||
         expr.op == RtlOp::kHigh;
}

// Extracts the single `(set dest src)` node from an instruction pattern,
// matching GCC's `single_set(insn)`.
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

RtlExpr* FindMutableSingleSet(RtlExpr* raw_pattern) {
  RtlExpr* pattern = MutableUnwrapInsnContainer(raw_pattern);
  if (pattern->IsSet()) {
    return pattern;
  }
  if (pattern->IsParallel()) {
    RtlExpr* single = nullptr;
    for (RtlExpr& elem : pattern->children) {
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

bool IsTerminatorJumpPattern(const RtlExpr& raw_pattern) {
  if (raw_pattern.op == RtlOp::kJumpInsn) {
    return true;
  }
  const RtlExpr& pat = UnwrapInsnContainer(raw_pattern);
  auto is_pc_set_or_ret = [](const RtlExpr& e) {
    return e.op == RtlOp::kReturn ||
           (e.op == RtlOp::kSet && !e.children.empty() && e.children[0].op == RtlOp::kPc);
  };
  if (is_pc_set_or_ret(pat)) {
    return true;
  }
  if (pat.IsParallel()) {
    for (const RtlExpr& elem : pat.children) {
      if (is_pc_set_or_ret(elem)) {
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

int MemModeSizeBytes(const RtlExpr& mem_expr) {
  switch (mem_expr.mode) {
    case RtlMachineMode::kQi:
      return 1;
    case RtlMachineMode::kHi:
      return 2;
    case RtlMachineMode::kSi:
    case RtlMachineMode::kSf:
      return 4;
    case RtlMachineMode::kDi:
    case RtlMachineMode::kDf:
      return 8;
    case RtlMachineMode::kTi:
    case RtlMachineMode::kTf:
      return 16;
    default:
      return 0;
  }
}

bool RtxAddrVariesP(const RtlExpr& expr) {
  if (expr.op == RtlOp::kReg) {
    if (expr.flags.unchanging) {
      return false;
    }
    const int r = expr.RegNo();
    return r != kFramePointerRegno && r != kArgPointerRegno;
  }
  if (expr.op == RtlOp::kLoSum) {
    return expr.children.size() >= 1 && RtxAddrVariesP(expr.children[0]);
  }
  for (const RtlExpr& ch : expr.children) {
    if (RtxAddrVariesP(ch)) {
      return true;
    }
  }
  return false;
}

const RtlExpr& UnwrapMemBaseOffset(const RtlExpr& raw_addr, int64_t* offset_accum) {
  const RtlExpr* cur = &raw_addr;
  while (true) {
    if (cur->op == RtlOp::kHigh && !cur->children.empty()) {
      cur = &cur->children[0];
      continue;
    }
    if (cur->op == RtlOp::kLoSum && cur->children.size() >= 2) {
      cur = &cur->children[1];
      continue;
    }
    if (cur->op == RtlOp::kConst && !cur->children.empty()) {
      cur = &cur->children[0];
      continue;
    }
    if (cur->op == RtlOp::kPlus && cur->children.size() == 2 &&
        cur->children[1].op == RtlOp::kConstInt) {
      *offset_accum += cur->children[1].int_val;
      cur = &cur->children[0];
      continue;
    }
    break;
  }
  return *cur;
}

bool IsStackOrFrameBaseReg(const RtlExpr& expr) {
  return expr.IsReg() && (expr.RegNo() == kStackPointerRegno || expr.RegNo() == kFramePointerRegno);
}

// Mirrors GCC 2.7.2 `sched.c:memrefs_conflict_p` + `true_dependence` (`lines 614-840`).
bool TrueDependence(const RtlExpr& store_mem, const RtlExpr& equiv_mem,
                    const absl::flat_hash_map<int, RtlExpr>* reg_known_values = nullptr) {
  if (equiv_mem.flags.unchanging && !store_mem.flags.unchanging) {
    return false;
  }
  if (equiv_mem.flags.volatil && store_mem.flags.volatil) {
    return true;
  }
  if (store_mem.children.empty() || equiv_mem.children.empty()) {
    return true;
  }

  const int xsize = MemModeSizeBytes(store_mem);
  const int ysize = MemModeSizeBytes(equiv_mem);
  int64_t xoff = 0;
  int64_t yoff = 0;
  const RtlExpr* xbase_ptr = &UnwrapMemBaseOffset(store_mem.children[0], &xoff);
  const RtlExpr* ybase_ptr = &UnwrapMemBaseOffset(equiv_mem.children[0], &yoff);
  if (reg_known_values != nullptr) {
    if (xbase_ptr->IsReg()) {
      if (const auto it = reg_known_values->find(xbase_ptr->RegNo());
          it != reg_known_values->end()) {
        int64_t extra = 0;
        xbase_ptr = &UnwrapMemBaseOffset(it->second, &extra);
        xoff += extra;
      }
    }
    if (ybase_ptr->IsReg()) {
      if (const auto it = reg_known_values->find(ybase_ptr->RegNo());
          it != reg_known_values->end()) {
        int64_t extra = 0;
        ybase_ptr = &UnwrapMemBaseOffset(it->second, &extra);
        yoff += extra;
      }
    }
  }
  const RtlExpr& xbase = *xbase_ptr;
  const RtlExpr& ybase = *ybase_ptr;
  const int64_t c = yoff - xoff;
  auto ranges_overlap = [&]() {
    return xsize == 0 || ysize == 0 || (c >= 0 && xsize > c) || (c < 0 && ysize + c > 0);
  };

  bool memrefs_conflict = true;
  if (xbase == ybase) {
    memrefs_conflict = ranges_overlap();
  } else if ((IsStackOrFrameBaseReg(xbase) && IsConstantP(ybase)) ||
             (IsStackOrFrameBaseReg(ybase) && IsConstantP(xbase))) {
    memrefs_conflict = false;
  } else if (IsConstantP(xbase) && IsConstantP(ybase)) {
    memrefs_conflict = (xbase == ybase) && ranges_overlap();
  }

  if (!memrefs_conflict) {
    return false;
  }

  const bool store_varies = RtxAddrVariesP(store_mem.children[0]);
  const bool equiv_varies = RtxAddrVariesP(equiv_mem.children[0]);
  if (store_mem.flags.in_struct && store_varies && store_mem.mode != RtlMachineMode::kQi &&
      !equiv_mem.flags.in_struct && !equiv_varies) {
    return false;
  }
  if (equiv_mem.flags.in_struct && equiv_varies && equiv_mem.mode != RtlMachineMode::kQi &&
      !store_mem.flags.in_struct && !store_varies) {
    return false;
  }
  return true;
}

bool StoreInvalidatesEquivMem(const RtlExpr& raw_pattern, const absl::flat_hash_set<int>& mem_regs,
                              const RtlExpr& equiv_mem,
                              const absl::flat_hash_map<int, RtlExpr>* reg_known_values = nullptr) {
  const RtlExpr& pattern = UnwrapInsnContainer(raw_pattern);
  auto check_dest = [&](const RtlExpr& raw_dest) -> bool {
    const RtlExpr* d = &raw_dest;
    while (d->op == RtlOp::kSubreg || d->op == RtlOp::kStrictLowPart ||
           d->op == RtlOp::kZeroExtract || d->op == RtlOp::kSignExtract) {
      if (d->children.empty()) {
        break;
      }
      d = &d->children[0];
    }
    if (d->IsReg() && mem_regs.contains(d->RegNo())) {
      return true;
    }
    if (d->op == RtlOp::kMem && TrueDependence(*d, equiv_mem, reg_known_values)) {
      return true;
    }
    return false;
  };

  if ((pattern.op == RtlOp::kSet || pattern.op == RtlOp::kClobber) && !pattern.children.empty()) {
    return check_dest(pattern.children[0]);
  }
  if (pattern.IsParallel()) {
    for (const RtlExpr& elem : pattern.children) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty() &&
          check_dest(elem.children[0])) {
        return true;
      }
    }
  }
  return false;
}

// Mirrors `local-alloc.c:validate_equiv_mem` (`lines 559-603`).
bool CanValidateEquivMem(const std::vector<RtlPatternAndNotes>& parsed_insns, size_t start_idx,
                         int regno, const RtlExpr& mem_expr,
                         const absl::flat_hash_map<int, RtlExpr>* reg_known_values = nullptr) {
  if (mem_expr.flags.volatil) {
    return false;
  }
  absl::flat_hash_set<int> mem_regs;
  CollectAllRegsInExpr(mem_expr, &mem_regs);
  if (mem_regs.contains(regno)) {
    return false;
  }

  for (size_t i = start_idx; i < parsed_insns.size(); ++i) {
    const RtlPatternAndNotes& pn = parsed_insns[i];
    if (pn.pattern.op == RtlOp::kNote || pn.pattern.op == RtlOp::kCodeLabel ||
        pn.pattern.op == RtlOp::kBarrier) {
      continue;
    }
    if (FindRegDeadNoteIndex(pn.reg_notes, regno) >= 0) {
      return true;
    }
    const bool is_const_call = pn.pattern.flags.unchanging;
    if (ContainsCall(pn.pattern) && !mem_expr.flags.unchanging && !is_const_call) {
      return false;
    }
    if (StoreInvalidatesEquivMem(pn.pattern, mem_regs, mem_expr, reg_known_values)) {
      return false;
    }
    for (const RtlRegNote& note : pn.reg_notes) {
      if ((note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegInc) &&
          note.datum.IsReg() && mem_regs.contains(note.datum.RegNo())) {
        return false;
      }
    }
  }
  return false;
}

// Mirrors `local-alloc.c:memref_referenced_p` (`lines 609-666`).
bool MemrefReferencedP(const RtlExpr& memref, const RtlExpr& x,
                       const absl::flat_hash_map<int, RtlExpr>* reg_known_values = nullptr) {
  switch (x.op) {
    case RtlOp::kReg:
    case RtlOp::kConstInt:
    case RtlOp::kConst:
    case RtlOp::kLabelRef:
    case RtlOp::kSymbolRef:
    case RtlOp::kConstDouble:
    case RtlOp::kPc:
    case RtlOp::kCc0:
    case RtlOp::kHigh:
    case RtlOp::kLoSum:
      return false;
    case RtlOp::kMem:
      if (TrueDependence(memref, x, reg_known_values)) {
        return true;
      }
      break;
    case RtlOp::kSet:
      if (x.children.size() == 2) {
        if (x.children[0].op == RtlOp::kMem) {
          if (!x.children[0].children.empty() &&
              MemrefReferencedP(memref, x.children[0].children[0], reg_known_values)) {
            return true;
          }
        } else if (MemrefReferencedP(memref, x.children[0], reg_known_values)) {
          return true;
        }
        return MemrefReferencedP(memref, x.children[1], reg_known_values);
      }
      break;
    default:
      break;
  }
  for (const RtlExpr& ch : x.children) {
    if (MemrefReferencedP(memref, ch, reg_known_values)) {
      return true;
    }
  }
  return false;
}

bool MemrefUsedBetweenP(const std::vector<RtlPatternAndNotes>& parsed_insns, size_t start_idx,
                        size_t end_idx, const RtlExpr& memref,
                        const absl::flat_hash_map<int, RtlExpr>* reg_known_values = nullptr) {
  for (size_t i = start_idx + 1; i <= end_idx && i < parsed_insns.size(); ++i) {
    const RtlPatternAndNotes& pn = parsed_insns[i];
    if (pn.pattern.op == RtlOp::kNote || pn.pattern.op == RtlOp::kCodeLabel ||
        pn.pattern.op == RtlOp::kBarrier) {
      continue;
    }
    if (MemrefReferencedP(memref, UnwrapInsnContainer(pn.pattern), reg_known_values)) {
      return true;
    }
  }
  return false;
}

bool HasSubsequentMatchingStore(
    const std::vector<RtlPatternAndNotes>& parsed_insns, size_t start_idx, int regno,
    const RtlExpr& mem_expr, const absl::flat_hash_map<int, RtlExpr>* reg_known_values = nullptr) {
  if (!CanValidateEquivMem(parsed_insns, start_idx, regno, mem_expr, reg_known_values)) {
    return false;
  }
  for (size_t i = start_idx + 1; i < parsed_insns.size(); ++i) {
    const RtlExpr* set = FindSingleSet(parsed_insns[i].pattern);
    if (set == nullptr) {
      continue;
    }
    const RtlExpr& dest = set->SetDest();
    const RtlExpr& src = set->SetSrc();
    if (dest.op == RtlOp::kMem && src.IsReg() && src.RegNo() == regno && dest == mem_expr &&
        !MemrefUsedBetweenP(parsed_insns, start_idx, i, mem_expr, reg_known_values)) {
      return true;
    }
  }
  return false;
}

std::vector<std::vector<int>> ComputeBlockInsnLoopDepths(const RtlControlFlowGraph& cfg) {
  std::vector<std::vector<int>> depths(cfg.rpo_blocks.size());
  int cur_depth = 1;
  for (size_t b = 0; b < cfg.rpo_blocks.size(); ++b) {
    const auto& insns = cfg.rpo_blocks[b].instructions;
    depths[b].resize(insns.size(), 1);
    for (size_t ii = 0; ii < insns.size(); ++ii) {
      if (std::optional<RtlLoopNoteKind> lk = ParseRtlLoopNoteKind(insns[ii]); lk.has_value()) {
        if (*lk == RtlLoopNoteKind::kLoopBeg) {
          ++cur_depth;
        } else if (*lk == RtlLoopNoteKind::kLoopEnd && cur_depth > 1) {
          --cur_depth;
        }
      }
      depths[b][ii] = cur_depth;
    }
  }
  return depths;
}

struct DeletedEquivCandidateMatch {
  size_t use_insn_idx = 0;
  RtlExpr* target_slot = nullptr;
  RtlExpr equiv_expr;
  RtlMachineMode reg_mode = RtlMachineMode::kSi;
  int score = -1;
};

void SearchDeletedEquivExprInNode(RtlExpr* node, RtlOp parent_op, size_t child_idx,
                                  RtlMachineMode enclosing_mode, bool inside_mem_addr, bool is_ptr,
                                  const absl::flat_hash_set<int>& invariant_mem_regs,
                                  size_t use_insn_idx, DeletedEquivCandidateMatch* best) {
  if (node == nullptr) {
    return;
  }
  if (parent_op == RtlOp::kClobber || parent_op == RtlOp::kUse || parent_op == RtlOp::kAddrVec ||
      parent_op == RtlOp::kAddrDiffVec || parent_op == RtlOp::kConst || parent_op == RtlOp::kHigh ||
      parent_op == RtlOp::kUnspec || parent_op == RtlOp::kUnspecVolatile) {
    return;
  }
  if (parent_op == RtlOp::kCall && child_idx == 1) {
    return;
  }
  if (parent_op == RtlOp::kLoSum && child_idx == 1) {
    return;
  }

  if (parent_op == RtlOp::kSet && child_idx == 0) {
    if (node->op == RtlOp::kMem && !node->children.empty()) {
      SearchDeletedEquivExprInNode(&node->children[0], RtlOp::kMem, 0, RtlMachineMode::kSi,
                                   /*inside_mem_addr=*/true, is_ptr, invariant_mem_regs,
                                   use_insn_idx, best);
    }
    return;
  }

  auto try_record = [&](int score, RtlMachineMode m) {
    if (score > best->score) {
      best->use_insn_idx = use_insn_idx;
      best->target_slot = node;
      best->equiv_expr = *node;
      best->reg_mode = (m != RtlMachineMode::kNone) ? m : RtlMachineMode::kSi;
      best->score = score;
    }
  };

  if (!inside_mem_addr && node->op == RtlOp::kMem && !node->flags.volatil &&
      (parent_op == RtlOp::kSet || parent_op == RtlOp::kSignExtend ||
       parent_op == RtlOp::kZeroExtend)) {
    absl::flat_hash_set<int> mregs;
    CollectAllRegsInExpr(*node, &mregs);
    bool all_invariant = true;
    bool uses_arg_or_frame = false;
    for (int mr : mregs) {
      if (mr == kArgPointerRegno || mr == kFramePointerRegno) {
        uses_arg_or_frame = true;
      } else if (!node->flags.unchanging && !invariant_mem_regs.contains(mr)) {
        all_invariant = false;
        break;
      }
    }
    if (all_invariant) {
      const int score = uses_arg_or_frame ? 100 : (node->flags.unchanging ? 90 : 50);
      try_record(score, node->mode != RtlMachineMode::kNone ? node->mode : enclosing_mode);
    }
  } else if (IsConstantP(*node)) {
    const bool is_branch_label = node->op == RtlOp::kLabelRef &&
                                 (parent_op == RtlOp::kIfThenElse || parent_op == RtlOp::kSet);
    const bool valid_in_mem_addr =
        !inside_mem_addr ||
        (parent_op == RtlOp::kMem && (node->op == RtlOp::kSymbolRef || node->op == RtlOp::kConst));
    const bool ptr_compatible = !IsSymbolicPointerConstant(*node) || is_ptr;
    const bool non_ptr_float_ok = !(is_ptr && node->op == RtlOp::kConstDouble);
    if (!is_branch_label && valid_in_mem_addr && ptr_compatible && non_ptr_float_ok) {
      int score = 70;
      if (IsSymbolicPointerConstant(*node)) {
        score = 85;
      } else if (node->op == RtlOp::kConstInt && node->int_val == 0) {
        score = 60;
      }
      const RtlMachineMode m = node->mode != RtlMachineMode::kNone ? node->mode : enclosing_mode;
      try_record(score, m);
    }
  }

  if (node->op == RtlOp::kConst || node->op == RtlOp::kHigh) {
    return;
  }

  RtlMachineMode child_enclosing_mode =
      node->mode != RtlMachineMode::kNone ? node->mode : enclosing_mode;
  if (node->op == RtlOp::kSet && node->children.size() == 2 &&
      node->children[0].mode != RtlMachineMode::kNone) {
    child_enclosing_mode = node->children[0].mode;
  } else if ((node->op == RtlOp::kEq || node->op == RtlOp::kNe || node->op == RtlOp::kLt ||
              node->op == RtlOp::kLe || node->op == RtlOp::kGt || node->op == RtlOp::kGe ||
              node->op == RtlOp::kLtu || node->op == RtlOp::kLeu || node->op == RtlOp::kGtu ||
              node->op == RtlOp::kGeu) &&
             !node->children.empty() && node->children[0].mode != RtlMachineMode::kNone) {
    child_enclosing_mode = node->children[0].mode;
  }

  const bool next_inside_mem = inside_mem_addr || (node->op == RtlOp::kMem);
  for (size_t i = 0; i < node->children.size(); ++i) {
    SearchDeletedEquivExprInNode(&node->children[i], node->op, i, child_enclosing_mode,
                                 next_inside_mem, is_ptr, invariant_mem_regs, use_insn_idx, best);
  }
}

void PrependEntryBasicBlock(RtlControlFlowGraph* out,
                            std::vector<std::vector<RtlPatternAndNotes>>* block_asts,
                            std::vector<std::vector<int>>* loop_depths) {
  for (RtlBasicBlock& blk : out->rpo_blocks) {
    blk.id += 1;
    for (size_t& succ : blk.successors) {
      succ += 1;
    }
    for (size_t& target : blk.addr_vec_targets) {
      target += 1;
    }
    for (size_t& pred : blk.predecessors) {
      pred += 1;
    }
  }
  RtlBasicBlock entry_blk;
  entry_blk.id = 0;
  entry_blk.successors = {1};
  if (!out->rpo_blocks.empty()) {
    out->rpo_blocks[0].predecessors.insert(out->rpo_blocks[0].predecessors.begin(), 0);
  }
  out->rpo_blocks.insert(out->rpo_blocks.begin(), std::move(entry_blk));
  block_asts->insert(block_asts->begin(), std::vector<RtlPatternAndNotes>{});
  loop_depths->insert(loop_depths->begin(), std::vector<int>{});

  absl::flat_hash_map<int, absl::flat_hash_set<int>> shifted_live;
  for (auto& [bb_idx, regs] : out->sideband.basic_block_live_at_start) {
    shifted_live[bb_idx + 1] = std::move(regs);
  }
  out->sideband.basic_block_live_at_start = std::move(shifted_live);
}

// Restores cross-block single-set / single-use `reg_equiv_init_insn` definitions
// deleted by `local-alloc.c:1096-1115`.
void RestoreDeletedEquivInitInsns(RtlControlFlowGraph* out,
                                  std::vector<std::vector<RtlPatternAndNotes>>* block_asts,
                                  std::vector<std::vector<int>>* loop_depths,
                                  const absl::flat_hash_map<int, int>& ast_reg_occurrences,
                                  absl::flat_hash_set<int>* restored_regnos) {
  std::vector<int> deleted_candidates;
  for (const auto& [regno, n_sets] : out->sideband.reg_n_sets) {
    if (regno < kMipsFirstPseudoRegister || n_sets != 1) {
      continue;
    }
    if (ast_reg_occurrences.contains(regno)) {
      continue;
    }
    const auto refs_it = out->sideband.reg_n_refs.find(regno);
    if (refs_it != out->sideband.reg_n_refs.end() && refs_it->second != 0) {
      continue;
    }
    const auto deaths_it = out->sideband.reg_n_deaths.find(regno);
    if (deaths_it != out->sideband.reg_n_deaths.end() && deaths_it->second != 0) {
      continue;
    }
    bool in_any_live_at_start = false;
    for (const auto& [bb_idx, live_set] : out->sideband.basic_block_live_at_start) {
      if (bb_idx >= 0 && static_cast<size_t>(bb_idx) < out->rpo_blocks.size() &&
          live_set.contains(regno)) {
        in_any_live_at_start = true;
        break;
      }
    }
    if (in_any_live_at_start) {
      deleted_candidates.push_back(regno);
    }
  }
  std::sort(deleted_candidates.begin(), deleted_candidates.end());

  const absl::flat_hash_set<int> ptr_regno_set(out->sideband.pointer_regnos.begin(),
                                               out->sideband.pointer_regnos.end());

  for (int regno : deleted_candidates) {
    if (out->sideband.basic_block_live_at_start.contains(0) &&
        out->sideband.basic_block_live_at_start.at(0).contains(regno)) {
      PrependEntryBasicBlock(out, block_asts, loop_depths);
    }

    const size_t num_blocks = out->rpo_blocks.size();
    absl::flat_hash_set<size_t> live_blocks;
    for (const auto& [bb_idx, live_set] : out->sideband.basic_block_live_at_start) {
      if (bb_idx >= 0 && static_cast<size_t>(bb_idx) < num_blocks && live_set.contains(regno)) {
        live_blocks.insert(static_cast<size_t>(bb_idx));
      }
    }
    if (live_blocks.empty()) {
      continue;
    }

    std::vector<size_t> sink_blocks;
    for (size_t b : live_blocks) {
      bool has_succ_in_live = false;
      for (size_t succ : out->rpo_blocks[b].successors) {
        if (live_blocks.contains(succ)) {
          has_succ_in_live = true;
          break;
        }
      }
      for (size_t target : out->rpo_blocks[b].addr_vec_targets) {
        if (live_blocks.contains(target)) {
          has_succ_in_live = true;
          break;
        }
      }
      if (!has_succ_in_live) {
        sink_blocks.push_back(b);
      }
    }
    if (sink_blocks.size() != 1) {
      continue;
    }
    const size_t b_use = sink_blocks[0];

    std::vector<size_t> pred_candidates;
    for (size_t p = 0; p < num_blocks; ++p) {
      if (live_blocks.contains(p)) {
        continue;
      }
      bool enters_live = false;
      for (size_t succ : out->rpo_blocks[p].successors) {
        if (live_blocks.contains(succ)) {
          enters_live = true;
          break;
        }
      }
      if (enters_live) {
        pred_candidates.push_back(p);
      }
    }
    if (pred_candidates.empty()) {
      continue;
    }

    size_t b_def = pred_candidates[0];
    for (size_t p : pred_candidates) {
      const int end_depth = (*loop_depths)[p].empty() ? 1 : (*loop_depths)[p].back();
      if (end_depth == 1) {
        b_def = p;
        break;
      }
    }
    const int def_depth = (*loop_depths)[b_def].empty() ? 1 : (*loop_depths)[b_def].back();
    if (def_depth != 1) {
      continue;
    }

    absl::flat_hash_set<int> invariant_mem_regs;
    if (auto it = out->sideband.basic_block_live_at_start.find(static_cast<int>(b_use));
        it != out->sideband.basic_block_live_at_start.end()) {
      invariant_mem_regs = it->second;
    }
    for (size_t s : live_blocks) {
      if (s == b_use) {
        continue;
      }
      for (const RtlPatternAndNotes& pn : (*block_asts)[s]) {
        absl::flat_hash_map<int, int> defs;
        CollectDefinedRegsInPattern(pn.pattern, &defs);
        for (const auto& [dr, _] : defs) {
          invariant_mem_regs.erase(dr);
        }
      }
    }

    const bool is_ptr = ptr_regno_set.contains(regno);
    DeletedEquivCandidateMatch best;
    auto& use_asts = (*block_asts)[b_use];
    for (size_t u_idx = 0; u_idx < use_asts.size(); ++u_idx) {
      const int u_depth = (u_idx < (*loop_depths)[b_use].size()) ? (*loop_depths)[b_use][u_idx] : 1;
      RtlPatternAndNotes& u_pn = use_asts[u_idx];
      if (u_depth == 1 && u_pn.pattern.op != RtlOp::kNote && u_pn.pattern.op != RtlOp::kCodeLabel &&
          u_pn.pattern.op != RtlOp::kBarrier) {
        RtlExpr* inner = MutableUnwrapInsnContainer(&u_pn.pattern);
        SearchDeletedEquivExprInNode(inner, RtlOp::kNil, 0, RtlMachineMode::kSi,
                                     /*inside_mem_addr=*/false, is_ptr, invariant_mem_regs, u_idx,
                                     &best);
      }
      absl::flat_hash_map<int, int> defs;
      CollectDefinedRegsInPattern(u_pn.pattern, &defs);
      for (const auto& [dr, _] : defs) {
        invariant_mem_regs.erase(dr);
      }
    }

    if (best.target_slot == nullptr || best.score < 0) {
      continue;
    }

    RtlExpr pseudo_reg = RtlExpr::MakeReg(best.reg_mode, regno);
    if (!is_ptr) {
      pseudo_reg.flags.volatil = true;
    }

    *best.target_slot = pseudo_reg;
    use_asts[best.use_insn_idx].reg_notes.push_back(RtlRegNote{
        .kind = RtlMachineMode::kRegDead,
        .list_op = RtlOp::kExprList,
        .datum = pseudo_reg,
    });

    RtlPatternAndNotes equiv_pn;
    equiv_pn.pattern = RtlExpr::MakeSet(pseudo_reg, best.equiv_expr);
    equiv_pn.reg_notes.push_back(RtlRegNote{
        .kind =
            IsConstantP(best.equiv_expr) ? RtlMachineMode::kRegEqual : RtlMachineMode::kRegEquiv,
        .list_op = RtlOp::kExprList,
        .datum = best.equiv_expr,
    });

    auto& def_asts = (*block_asts)[b_def];
    auto& def_depths = (*loop_depths)[b_def];
    size_t insert_pos = def_asts.size();
    if (!def_asts.empty() && IsTerminatorJumpPattern(def_asts.back().pattern)) {
      insert_pos = def_asts.size() - 1;
    }
    def_asts.insert(def_asts.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                    std::move(equiv_pn));
    def_depths.insert(def_depths.begin() + static_cast<std::ptrdiff_t>(insert_pos), 1);
    out->rpo_blocks[b_def].instructions.insert(
        out->rpo_blocks[b_def].instructions.begin() + static_cast<std::ptrdiff_t>(insert_pos), "");

    out->sideband.reg_n_refs[regno] = 2;
    out->sideband.reg_n_deaths[regno] = 1;
    out->sideband.reg_n_sets[regno] = 1;
    if (auto live_it = out->sideband.reg_live_length.find(regno);
        live_it != out->sideband.reg_live_length.end() && live_it->second > 1) {
      live_it->second /= 2;
    }
    restored_regnos->insert(regno);
  }
}

// Computes how many `CALL_INSN`s pseudo `target_regno` is live across using
// uniform backward CFG liveness over `block_asts` (independent of whether
// `sideband` counts were pre-populated).
int CountStructuralPseudoCallsCrossed(
    const RtlControlFlowGraph& cfg, const std::vector<std::vector<RtlPatternAndNotes>>& block_asts,
    int target_regno) {
  const size_t nb = block_asts.size();
  auto analyze_insn = [target_regno](const RtlPatternAndNotes& pn, bool* reads, bool* kills) {
    *reads = false;
    *kills = false;
    const RtlExpr& pat = UnwrapInsnContainer(pn.pattern);
    if (pat.op == RtlOp::kNote || pat.op == RtlOp::kCodeLabel || pat.op == RtlOp::kBarrier) {
      return;
    }
    auto inspect_elem = [&](const RtlExpr& elem) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
        const RtlExpr& dst = elem.children[0];
        if (dst.IsReg() && dst.RegNo() == target_regno) {
          *kills = true;
        } else if (CountRegOccurrences(dst, target_regno) > 0) {
          *reads = true;
        }
        if (elem.op == RtlOp::kSet && elem.children.size() > 1 &&
            CountRegOccurrences(elem.children[1], target_regno) > 0) {
          *reads = true;
        }
      } else if (CountRegOccurrences(elem, target_regno) > 0) {
        *reads = true;
      }
    };
    if (pat.IsParallel()) {
      for (const RtlExpr& ch : pat.children) {
        inspect_elem(ch);
      }
    } else {
      inspect_elem(pat);
    }
    for (const RtlExpr& fu : pn.call_fusage) {
      if (CountRegOccurrences(fu, target_regno) > 0) {
        *reads = true;
      }
    }
  };

  std::vector<bool> live_in(nb, false);
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t rev = nb; rev > 0; --rev) {
      const size_t b = rev - 1;
      bool cur = false;
      for (size_t succ : cfg.rpo_blocks[b].successors) {
        if (succ < nb && live_in[succ]) {
          cur = true;
          break;
        }
      }
      for (size_t i = block_asts[b].size(); i > 0; --i) {
        bool reads = false;
        bool kills = false;
        analyze_insn(block_asts[b][i - 1], &reads, &kills);
        cur = reads || (!kills && cur);
      }
      if (cur != live_in[b]) {
        live_in[b] = cur;
        changed = true;
      }
    }
  }

  int calls_crossed = 0;
  for (size_t b = 0; b < nb; ++b) {
    bool cur = false;
    for (size_t succ : cfg.rpo_blocks[b].successors) {
      if (succ < nb && live_in[succ]) {
        cur = true;
        break;
      }
    }
    for (size_t i = block_asts[b].size(); i > 0; --i) {
      const RtlPatternAndNotes& pn = block_asts[b][i - 1];
      bool reads = false;
      bool kills = false;
      analyze_insn(pn, &reads, &kills);
      if (ContainsCall(pn.pattern) && cur && !kills) {
        ++calls_crossed;
      }
      cur = reads || (!kills && cur);
    }
  }
  return calls_crossed;
}

// Reverses intra-block `optimize_reg_copy_2` self-copy pairs (`local-alloc.c:874-936`).
void ReverseOptimizeRegCopy2(RtlControlFlowGraph* out,
                             std::vector<std::vector<RtlPatternAndNotes>>* block_asts,
                             const std::vector<std::vector<int>>& loop_depths,
                             const absl::flat_hash_map<int, int>& ast_reg_occurrences,
                             int max_seen_regno) {
  std::vector<int> candidate_dregnos;
  for (const auto& [regno, n_sets] : out->sideband.reg_n_sets) {
    if (regno < kMipsFirstPseudoRegister || n_sets < 1 || ast_reg_occurrences.contains(regno)) {
      continue;
    }
    const auto refs_it = out->sideband.reg_n_refs.find(regno);
    if (refs_it != out->sideband.reg_n_refs.end() && refs_it->second != 0) {
      continue;
    }
    bool in_live_at_start = false;
    for (const auto& [_, live_set] : out->sideband.basic_block_live_at_start) {
      if (live_set.contains(regno)) {
        in_live_at_start = true;
        break;
      }
    }
    if (!in_live_at_start) {
      candidate_dregnos.push_back(regno);
    }
  }
  std::sort(candidate_dregnos.begin(), candidate_dregnos.end());
  absl::flat_hash_set<int> consumed_dregnos;
  int next_synthetic_pseudo = std::max({72, max_seen_regno + 1, out->sideband.max_regno});
  const absl::flat_hash_set<int> ptr_regno_set(out->sideband.pointer_regnos.begin(),
                                               out->sideband.pointer_regnos.end());

  for (size_t b = 0; b < block_asts->size(); ++b) {
    auto& asts = (*block_asts)[b];
    for (size_t ii = 0; ii + 1 < asts.size(); ++ii) {
      const RtlExpr* first_set = FindSingleSet(asts[ii].pattern);
      if (first_set == nullptr || !first_set->SetDest().IsReg() || !first_set->SetSrc().IsReg()) {
        continue;
      }
      const RtlExpr src_reg = first_set->SetSrc();
      const int sregno = src_reg.RegNo();
      if (sregno < kMipsFirstPseudoRegister || first_set->SetDest().RegNo() != sregno ||
          first_set->SetDest().mode != src_reg.mode) {
        continue;
      }
      if (FindRegDeadNoteIndex(asts[ii].reg_notes, sregno) >= 0) {
        continue;
      }

      int span_sets = 1;
      int span_calls = 0;
      bool found_pair = false;
      size_t pp = ii + 1;
      for (; pp < asts.size(); ++pp) {
        const RtlExpr& p_inner = UnwrapInsnContainer(asts[pp].pattern);
        if (p_inner.op == RtlOp::kCodeLabel || IsTerminatorJumpPattern(asts[pp].pattern)) {
          break;
        }
        if (std::optional<RtlLoopNoteKind> lk =
                ParseRtlLoopNoteKind(out->rpo_blocks[b].instructions[pp]);
            lk.has_value() &&
            (*lk == RtlLoopNoteKind::kLoopBeg || *lk == RtlLoopNoteKind::kLoopEnd)) {
          break;
        }
        if (p_inner.op == RtlOp::kNote || p_inner.op == RtlOp::kBarrier) {
          continue;
        }

        const RtlExpr* end_set = FindSingleSet(asts[pp].pattern);
        if (end_set != nullptr && end_set->SetDest().IsReg() && end_set->SetSrc().IsReg() &&
            end_set->SetDest().RegNo() == sregno && end_set->SetSrc().RegNo() == sregno &&
            end_set->SetDest().mode == src_reg.mode &&
            FindRegDeadNoteIndex(asts[pp].reg_notes, sregno) < 0) {
          found_pair = true;
          break;
        }

        if (ContainsCall(asts[pp].pattern)) {
          ++span_calls;
          if (CountStructuralPseudoCallsCrossed(*out, *block_asts, sregno) <= span_calls) {
            break;
          }
        }
        if (IsRegSetInPattern(asts[pp].pattern, sregno)) {
          ++span_sets;
        }
      }
      if (!found_pair) {
        continue;
      }

      int chosen_dregno = -1;
      for (int cand : candidate_dregnos) {
        if (!consumed_dregnos.contains(cand) && out->sideband.reg_n_sets[cand] == span_sets) {
          chosen_dregno = cand;
          consumed_dregnos.insert(cand);
          break;
        }
      }
      if (chosen_dregno < 0 && pp > ii + 1) {
        for (int gap = 72; gap < out->sideband.max_regno; ++gap) {
          if (!ast_reg_occurrences.contains(gap) && !consumed_dregnos.contains(gap) &&
              !out->sideband.reg_renumber.contains(gap)) {
            bool in_live = false;
            for (const auto& [_, live_set] : out->sideband.basic_block_live_at_start) {
              if (live_set.contains(gap)) {
                in_live = true;
                break;
              }
            }
            if (!in_live) {
              chosen_dregno = gap;
              consumed_dregnos.insert(gap);
              break;
            }
          }
        }
        if (chosen_dregno < 0) {
          chosen_dregno = next_synthetic_pseudo++;
        }
      }
      if (chosen_dregno < 0) {
        continue;
      }

      RtlExpr dest_reg = RtlExpr::MakeReg(src_reg.mode, chosen_dregno);
      dest_reg.raw_mode = src_reg.raw_mode;
      if (!ptr_regno_set.contains(chosen_dregno)) {
        dest_reg.flags.volatil = true;
      }

      RtlExpr* mut_first_set = FindMutableSingleSet(&asts[ii].pattern);
      RtlExpr* mut_end_set = FindMutableSingleSet(&asts[pp].pattern);
      if (mut_first_set == nullptr || mut_end_set == nullptr) {
        continue;
      }
      mut_first_set->children[0] = dest_reg;
      asts[ii].reg_notes.insert(asts[ii].reg_notes.begin(), RtlRegNote{
                                                                .kind = RtlMachineMode::kRegDead,
                                                                .list_op = RtlOp::kExprList,
                                                                .datum = src_reg,
                                                            });

      const int depth_ii = (ii < loop_depths[b].size()) ? loop_depths[b][ii] : 1;
      int transferred_refs = depth_ii;
      for (size_t qq = ii + 1; qq < pp; ++qq) {
        const RtlExpr& q_inner = UnwrapInsnContainer(asts[qq].pattern);
        if (q_inner.op == RtlOp::kNote || q_inner.op == RtlOp::kCodeLabel ||
            q_inner.op == RtlOp::kBarrier) {
          continue;
        }
        if (CountRegOccurrences(asts[qq].pattern, sregno) > 0) {
          ReplaceRegInExpr(MutableUnwrapInsnContainer(&asts[qq].pattern), sregno, dest_reg);
          const int depth_qq = (qq < loop_depths[b].size()) ? loop_depths[b][qq] : 1;
          transferred_refs += depth_qq;
        }
      }

      mut_end_set->children[1] = dest_reg;
      asts[pp].reg_notes.insert(asts[pp].reg_notes.begin(), RtlRegNote{
                                                                .kind = RtlMachineMode::kRegDead,
                                                                .list_op = RtlOp::kExprList,
                                                                .datum = dest_reg,
                                                            });
      const int depth_pp = (pp < loop_depths[b].size()) ? loop_depths[b][pp] : 1;
      transferred_refs += depth_pp;

      out->sideband.reg_n_sets.try_emplace(chosen_dregno, span_sets);
      out->sideband.reg_n_refs[chosen_dregno] += transferred_refs;
      if (auto sref_it = out->sideband.reg_n_refs.find(sregno);
          sref_it != out->sideband.reg_n_refs.end() && sref_it->second > transferred_refs) {
        sref_it->second -= transferred_refs;
      }
      out->sideband.reg_n_deaths[chosen_dregno] += 1;
      out->sideband.reg_n_deaths[sregno] += 1;
      if (span_calls > 0) {
        out->sideband.reg_n_calls_crossed[chosen_dregno] += span_calls;
        if (auto sc_it = out->sideband.reg_n_calls_crossed.find(sregno);
            sc_it != out->sideband.reg_n_calls_crossed.end()) {
          sc_it->second = std::max(0, sc_it->second - span_calls);
        }
      }
      if (!out->sideband.reg_live_length.contains(chosen_dregno)) {
        out->sideband.reg_live_length[chosen_dregno] = static_cast<int>(pp - ii);
      }
      out->sideband.max_regno = std::max(out->sideband.max_regno, chosen_dregno + 1);
      ii = pp;
    }
  }
}

// Reverses intra-block `optimize_reg_copy_1` propagations (`local-alloc.c:700-857`).
void ReverseOptimizeRegCopy1(RtlControlFlowGraph* out,
                             std::vector<std::vector<RtlPatternAndNotes>>* block_asts,
                             const std::vector<std::vector<int>>& loop_depths) {
  for (size_t b = 0; b < block_asts->size(); ++b) {
    auto& asts = (*block_asts)[b];
    auto is_live_out_of_block = [&](int regno) {
      for (size_t succ : out->rpo_blocks[b].successors) {
        if (auto it = out->sideband.basic_block_live_at_start.find(static_cast<int>(succ));
            it != out->sideband.basic_block_live_at_start.end() && it->second.contains(regno)) {
          return true;
        }
      }
      return false;
    };

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
      if (dead_idx < 0 || asts[ii].reg_notes[dead_idx].datum.mode != src_reg.mode) {
        continue;
      }

      bool sreg_used_after_copy = is_live_out_of_block(sregno);
      for (size_t jj = ii + 1; jj < asts.size() && !sreg_used_after_copy; ++jj) {
        if (CountRegOccurrences(asts[jj].pattern, sregno) > 0) {
          sreg_used_after_copy = true;
        }
      }
      if (sreg_used_after_copy) {
        continue;
      }

      int length = 0;
      int n_calls = 0;
      for (size_t pp = ii + 1; pp < asts.size(); ++pp) {
        RtlPatternAndNotes& cand_pn = asts[pp];
        const RtlExpr& cand_inner = UnwrapInsnContainer(cand_pn.pattern);
        if (cand_inner.op == RtlOp::kCodeLabel || IsTerminatorJumpPattern(cand_pn.pattern)) {
          break;
        }
        if (pp < out->rpo_blocks[b].instructions.size()) {
          if (std::optional<RtlLoopNoteKind> lk =
                  ParseRtlLoopNoteKind(out->rpo_blocks[b].instructions[pp]);
              lk.has_value() &&
              (*lk == RtlLoopNoteKind::kLoopBeg || *lk == RtlLoopNoteKind::kLoopEnd)) {
            break;
          }
        }
        if (cand_inner.op == RtlOp::kNote || cand_inner.op == RtlOp::kBarrier) {
          continue;
        }
        if (cand_inner.op == RtlOp::kUse || IsRegSetInPattern(cand_pn.pattern, dregno) ||
            IsRegSetInPattern(cand_pn.pattern, sregno)) {
          break;
        }

        ++length;
        if (CountRegOccurrences(cand_pn.pattern, dregno) <= 0) {
          if (ContainsCall(cand_pn.pattern)) {
            ++n_calls;
          }
          continue;
        }

        if (ContainsCall(cand_pn.pattern) || FindRegDeadNoteIndex(cand_pn.reg_notes, dregno) >= 0) {
          break;
        }
        bool dreg_used_after_pp = is_live_out_of_block(dregno);
        for (size_t jj = pp + 1; jj < asts.size() && !dreg_used_after_pp; ++jj) {
          if (CountRegOccurrences(asts[jj].pattern, dregno) > 0) {
            dreg_used_after_pp = true;
          }
        }
        if (!dreg_used_after_pp) {
          break;
        }

        RtlExpr* cand_mut = MutableUnwrapInsnContainer(&cand_pn.pattern);
        const int replaced = ReplaceRegInExpr(cand_mut, dregno, src_reg);
        if (replaced > 0) {
          RtlRegNote moved_dead = std::move(asts[ii].reg_notes[dead_idx]);
          asts[ii].reg_notes.erase(asts[ii].reg_notes.begin() + dead_idx);
          cand_pn.reg_notes.push_back(std::move(moved_dead));

          const int depth = (pp < loop_depths[b].size()) ? loop_depths[b][pp] : 1;
          if (auto live_it = out->sideband.reg_live_length.find(sregno);
              live_it != out->sideband.reg_live_length.end()) {
            live_it->second += length;
          }
          if (n_calls > 0) {
            out->sideband.reg_n_calls_crossed[sregno] += n_calls;
          }
          if (auto sref_it = out->sideband.reg_n_refs.find(sregno);
              sref_it != out->sideband.reg_n_refs.end()) {
            sref_it->second += depth;
          }
          if (auto dref_it = out->sideband.reg_n_refs.find(dregno);
              dref_it != out->sideband.reg_n_refs.end() && dref_it->second > depth) {
            dref_it->second -= depth;
          }
        }
        break;
      }
    }
  }
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg,
                                                          const CompilationProfile& profile) {
  if (lreg_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertUpdateEquivRegs: input CFG has no basic blocks");
  }

  const size_t initial_num_blocks = lreg_cfg.rpo_blocks.size();
  absl::Status sideband_status = ValidateSideband(lreg_cfg.sideband);
  if (!sideband_status.ok()) {
    return sideband_status;
  }

  for (size_t b = 0; b < initial_num_blocks; ++b) {
    const RtlBasicBlock& blk = lreg_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= initial_num_blocks) {
        return absl::InvalidArgumentError(absl::StrCat("InvertUpdateEquivRegs: block ", b,
                                                       " has out-of-bounds successor ", succ,
                                                       " (num_blocks=", initial_num_blocks, ")"));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= initial_num_blocks) {
        return absl::InvalidArgumentError(
            absl::StrCat("InvertUpdateEquivRegs: block ", b, " has out-of-bounds addr_vec target ",
                         target, " (num_blocks=", initial_num_blocks, ")"));
      }
    }
  }

  std::vector<std::vector<RtlPatternAndNotes>> block_asts(initial_num_blocks);
  absl::flat_hash_map<int, int> ast_reg_occurrences;
  int max_seen_regno = kMipsFirstPseudoRegister;

  for (size_t b = 0; b < initial_num_blocks; ++b) {
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
      absl::flat_hash_set<int> regs_in_insn;
      CollectAllRegsInExpr(parsed->pattern, &regs_in_insn);
      for (const RtlRegNote& note : parsed->reg_notes) {
        CollectAllRegsInExpr(note.datum, &regs_in_insn);
      }
      for (int r : regs_in_insn) {
        ast_reg_occurrences[r] += 1;
        max_seen_regno = std::max(max_seen_regno, r);
      }
      if (ii < lreg_cfg.rpo_blocks[b].rtl_insns.size() &&
          lreg_cfg.rpo_blocks[b].rtl_insns[ii].has_explicit_call_fusage &&
          !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = lreg_cfg.rpo_blocks[b].rtl_insns[ii].call_fusage;
      }
      block_asts[b][ii] = *std::move(parsed);
    }
  }

  RtlControlFlowGraph out = lreg_cfg;
  out.pass = RtlPassKind::kSched;

  ParsedCompilerFlags flags = ParsedCompilerFlags::DefaultO2WaG();
  if (!profile.c_flags.empty()) {
    flags = ParsedCompilerFlags::Parse(profile.c_flags);
  } else {
    flags.ApplyOptimizationLevelDefaults(profile.optimize_level);
  }
  if (flags.opt_level == 0) {
    return out;
  }

  std::vector<std::vector<int>> loop_depths = ComputeBlockInsnLoopDepths(out);

  // Pass 1: Restore deleted `reg_equiv_init_insn` definitions (`local-alloc.c:1096-1115`).
  absl::flat_hash_set<int> restored_deleted_equiv_regnos;
  RestoreDeletedEquivInitInsns(&out, &block_asts, &loop_depths, ast_reg_occurrences,
                               &restored_deleted_equiv_regnos);

  // Pass 2: Reverse `optimize_reg_copy_2` self-copy pairs (`local-alloc.c:874-936`).
  if (flags.flag_expensive_optimizations) {
    ReverseOptimizeRegCopy2(&out, &block_asts, loop_depths, ast_reg_occurrences, max_seen_regno);
  }

  // Pass 3: Reverse `optimize_reg_copy_1` propagations (`local-alloc.c:700-857`).
  if (flags.flag_expensive_optimizations) {
    ReverseOptimizeRegCopy1(&out, &block_asts, loop_depths);
  }

  // Recompute AST def counts and block-reference sets after Passes 1..3 for Pass 4.
  const size_t num_blocks = out.rpo_blocks.size();
  absl::flat_hash_map<int, int> ast_def_counts;
  absl::flat_hash_map<int, absl::flat_hash_set<size_t>> reg_blocks_used;
  for (size_t b = 0; b < num_blocks; ++b) {
    for (const RtlPatternAndNotes& pn : block_asts[b]) {
      CollectDefinedRegsInPattern(pn.pattern, &ast_def_counts);
      absl::flat_hash_set<int> regs_in_insn;
      CollectAllRegsInExpr(pn.pattern, &regs_in_insn);
      for (int r : regs_in_insn) {
        reg_blocks_used[r].insert(b);
      }
    }
  }

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

  // Mirror `sched.c:init_alias_analysis` known base values for single-set
  // pseudos carrying constant `REG_EQUIV` / `REG_EQUAL` notes.
  absl::flat_hash_map<int, RtlExpr> reg_known_values;
  for (size_t b = 0; b < num_blocks; ++b) {
    for (const RtlPatternAndNotes& pn : block_asts[b]) {
      const RtlExpr* set = FindSingleSet(pn.pattern);
      if (set == nullptr || !set->SetDest().IsReg()) {
        continue;
      }
      const int r = set->SetDest().RegNo();
      if (r < kMipsFirstPseudoRegister || get_num_sets(r) != 1) {
        continue;
      }
      if (IsConstantP(set->SetSrc())) {
        reg_known_values[r] = set->SetSrc();
      }
      for (const RtlRegNote& note : pn.reg_notes) {
        if ((note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual) &&
            IsConstantP(note.datum)) {
          reg_known_values[r] = note.datum;
          break;
        }
      }
    }
  }

  // Pass 4: Reverse `REG_EQUIV` note promotions/insertions and halve doubled
  // `sideband.reg_live_length` entries (`local-alloc.c:990-1000, 1024-1065`).
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
      if (regno < kMipsFirstPseudoRegister || restored_deleted_equiv_regnos.contains(regno) ||
          get_num_sets(regno) != 1) {
        continue;
      }

      bool halved_live_length = false;
      bool seen_liveness_note_before = false;
      for (auto it = pn.reg_notes.begin(); it != pn.reg_notes.end();) {
        if (it->kind == RtlMachineMode::kRegDead || it->kind == RtlMachineMode::kRegUnused) {
          seen_liveness_note_before = true;
          ++it;
          continue;
        }
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
          if (!seen_liveness_note_before && is_block_local(regno, b) && !(src == it->datum) &&
              HasSubsequentMatchingStore(asts, ii, regno, it->datum, &reg_known_values)) {
            // Attached later by Rule A (`local-alloc.c:990-1000`) when a
            // subsequent store `(set (mem ...) (reg regno))` in the same basic
            // block was visited. Because `insn` had already passed line 1058,
            // this note did not trigger `reg_live_length[regno]` doubling.
            it = pn.reg_notes.erase(it);
            continue;
          }
          if (!halved_live_length) {
            auto live_it = out.sideband.reg_live_length.find(regno);
            if (live_it != out.sideband.reg_live_length.end() && live_it->second > 1) {
              live_it->second /= 2;
            }
            halved_live_length = true;
          }
          if (!seen_liveness_note_before && is_block_local(regno, b) && src.op == RtlOp::kMem &&
              src == it->datum && CanValidateEquivMem(asts, ii, regno, src, &reg_known_values)) {
            it = pn.reg_notes.erase(it);
            continue;
          }
        }
        ++it;
      }
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    const auto& asts = block_asts[b];
    bool modified_block = (blk.instructions.size() != asts.size());
    blk.instructions.resize(asts.size());
    for (size_t ii = 0; ii < asts.size(); ++ii) {
      std::string formatted = FormatGccPrintRtlPatternAndNotes(
          asts[ii].pattern, asts[ii].reg_notes, asts[ii].bracketed_notes, /*call_fusage=*/{});
      if (formatted != blk.instructions[ii]) {
        blk.instructions[ii] = std::move(formatted);
        modified_block = true;
      }
    }
    if (modified_block) {
      blk.SyncRtlInsnsFromInstructions();
      for (size_t ii = 0; ii < blk.rtl_insns.size() && ii < asts.size(); ++ii) {
        if (asts[ii].has_explicit_call_fusage && !blk.rtl_insns[ii].has_explicit_call_fusage) {
          blk.rtl_insns[ii].has_explicit_call_fusage = true;
          blk.rtl_insns[ii].call_fusage = asts[ii].call_fusage;
        }
      }
    }
  }

  return out;
}

absl::StatusOr<RtlControlFlowGraph> InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg) {
  return InvertUpdateEquivRegs(lreg_cfg, {.optimize_level = 2, .c_flags = {"-O2"}});
}

}  // namespace rom_nom_nom::lifter
