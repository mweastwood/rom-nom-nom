#include "lifter/invert_alter_reg.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;
constexpr int kFramePointerRegno = 30;
constexpr int kStackSlotAlignmentBytes = 8;

// Reaching-definition token sentinels on physical hard-register words `0..67`:
// - `kPinnedAbiToken`: value originates from a hardware-pinned ABI boundary
//   (entry incoming parameter `$a0..$a3` / `$f12..$f15`, `CALL_INSN` hardware
//   return register, or `REG_FUNCTION_VALUE_P` `/i` write).
// - `kUninitEntryToken`: upward-exposed entry token for non-ABI allocatable
//   hard registers so multiple upward-exposed reads of an uninitialized hard
//   register in synthetic snippets unify per signature.
constexpr int kPinnedAbiToken = -1;
constexpr int kUninitEntryToken = -2;

using WordReachList = absl::InlinedVector<int, 4>;
using WordReachArray = std::array<WordReachList, kFirstPseudoRegister>;

std::optional<RtlLoopNoteKind> ExtractLoopNoteKind(const RtlBasicBlock& blk, size_t idx,
                                                   const RtlExpr& pat) {
  if (idx < blk.rtl_insns.size() && blk.rtl_insns[idx].loop_note_kind.has_value()) {
    return blk.rtl_insns[idx].loop_note_kind;
  }
  if (pat.op != RtlOp::kNote) {
    return std::nullopt;
  }
  if (auto lk = ParseRtlLoopNoteKind(pat.str_val); lk.has_value()) {
    return lk;
  }
  for (const auto& item : pat.generic_items) {
    if (auto lk = ParseRtlLoopNoteKind(item.text); lk.has_value()) {
      return lk;
    }
  }
  return std::nullopt;
}

struct ReachState {
  WordReachArray defs{};
  WordReachArray partial_overwriters{};
};

struct RegWebSignature {
  int hard_regno = -1;
  RtlMachineMode mode = RtlMachineMode::kNone;
  bool volatil = false;
  bool in_struct = false;
  bool unchanging = false;

  bool operator==(const RegWebSignature& other) const {
    return hard_regno == other.hard_regno && mode == other.mode && volatil == other.volatil &&
           in_struct == other.in_struct && unchanging == other.unchanging;
  }

  bool operator!=(const RegWebSignature& other) const { return !(*this == other); }

  template <typename H>
  friend H AbslHashValue(H h, const RegWebSignature& s) {
    return H::combine(std::move(h), s.hard_regno, static_cast<int>(s.mode), s.volatil, s.in_struct,
                      s.unchanging);
  }
};

struct RegOccurrence {
  RegWebSignature sig;
  int word_span = 1;
  int ref_weight = 0;
  int set_count = 0;
  bool is_clobber_only = false;
  bool is_top_level_use = false;
};

struct WebComponentStats {
  RegWebSignature sig;
  int pseudo_regno = -1;
  int n_refs = 0;
  int n_sets = 0;
  bool has_non_clobber_ref = false;
};

struct StackSlotStats {
  RtlMachineMode mode = RtlMachineMode::kNone;
  bool unchanging = false;
  bool has_entry_arg_store = false;
  int n_refs = 0;
  int n_sets = 0;
  bool valid = true;
};

class WebUnionFind {
 public:
  explicit WebUnionFind(size_t n) : parent_(n), rank_(n, 0) {
    for (size_t i = 0; i < n; ++i) {
      parent_[i] = static_cast<int>(i);
    }
  }

  int Find(int x) {
    int root = x;
    while (parent_[root] != root) {
      root = parent_[root];
    }
    while (parent_[x] != root) {
      const int next = parent_[x];
      parent_[x] = root;
      x = next;
    }
    return root;
  }

  void Union(int a, int b) {
    int ra = Find(a);
    int rb = Find(b);
    if (ra == rb) {
      return;
    }
    if (rank_[ra] < rank_[rb]) {
      std::swap(ra, rb);
    }
    parent_[rb] = ra;
    if (rank_[ra] == rank_[rb]) {
      ++rank_[ra];
    }
  }

 private:
  std::vector<int> parent_;
  std::vector<uint8_t> rank_;
};

// Returns true if `regno` is an allocatable MIPS GPR (`$2..$25`, plus `$30/s8`
// when `!fp_is_fixed`) or FPR (`$32..$63`). Fixed/reserved registers (`$0`,
// `$1/at`, `$26..$29`, `$30/$fp` when `fp_is_fixed`, `$31/ra`), MD accumulators
// (`64..66`), and `$fcr31` (`67`) are never lifted to pseudos.
bool IsAllocatableHardReg(int regno, bool fp_is_fixed) {
  return (regno >= 2 && regno <= 25) || (!fp_is_fixed && regno == kFramePointerRegno) ||
         (regno >= 32 && regno <= 63);
}

// Returns true if `regno` is a MIPS hardware incoming argument register
// (`$4..$7` / `a0..a3` or `$44..$47` / `$f12..$f15`).
bool IsIncomingArgumentHardReg(int regno) {
  return (regno >= 4 && regno <= 7) || (regno >= 44 && regno <= 47);
}

// Returns true if `regno` is a MIPS hardware scalar/pair function return value
// register (`$2..$3` / `v0..v1` or `$32..$35` / `$f0..$f3`).
bool IsFunctionReturnHardReg(int regno) {
  return (regno == 2 || regno == 3) || (regno >= 32 && regno <= 35);
}

// Mirrors `CALL_USED_REGISTERS` in `config/mips/mips.h:1203-1210`.
bool IsCallUsedHardReg(int regno) {
  if (regno < 0 || regno >= kFirstPseudoRegister) {
    return false;
  }
  if (regno <= 15) return true;
  if (regno >= 24 && regno <= 29) return true;
  if (regno == 31) return true;
  if (regno >= 32 && regno <= 51) return true;
  if (regno >= 64 && regno <= 67) return true;
  return false;
}

bool IsAllocatableMode(RtlMachineMode mode) {
  return mode != RtlMachineMode::kNone && mode != RtlMachineMode::kCC &&
         mode != RtlMachineMode::kBLK && mode != RtlMachineMode::kUnknown;
}

// Returns true if `reg` is an allocatable hard-register node whose full
// multi-word span lies in `IsAllocatableHardReg(..., fp_is_fixed)`. Hardware
// return registers (`/i`) and frame-related/pointer-flagged registers (`/f`)
// are never lifted.
bool IsCandidateHardRegNode(const RtlExpr& reg, bool gp64, bool fp64, bool fp_is_fixed) {
  if (reg.op != RtlOp::kReg || reg.flags.integrated || reg.flags.frame_related ||
      !IsAllocatableMode(reg.mode)) {
    return false;
  }
  const int regno = reg.RegNo();
  if (!IsAllocatableHardReg(regno, fp_is_fixed)) {
    return false;
  }
  const int span = std::max(1, MipsHardRegNregs(regno, reg.mode, gp64, fp64));
  for (int k = 0; k < span; ++k) {
    if (!IsAllocatableHardReg(regno + k, fp_is_fixed)) {
      return false;
    }
  }
  return true;
}

RegWebSignature MakeRegWebSignature(const RtlExpr& reg) {
  return RegWebSignature{
      .hard_regno = reg.RegNo(),
      .mode = reg.mode,
      .volatil = reg.flags.volatil,
      .in_struct = reg.flags.in_struct,
      .unchanging = reg.flags.unchanging,
  };
}

int GetStackSpillModeSizeBytes(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kQI:
      return 1;
    case RtlMachineMode::kHI:
      return 2;
    case RtlMachineMode::kSI:
    case RtlMachineMode::kSF:
      return 4;
    case RtlMachineMode::kDI:
    case RtlMachineMode::kDF:
      return 8;
    default:
      return 0;
  }
}

bool HasNegativeRegNo(const RtlExpr& expr) {
  if (expr.op == RtlOp::kReg && expr.RegNo() < 0) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (HasNegativeRegNo(child)) {
      return true;
    }
  }
  return false;
}

bool ValidateExprStructure(const RtlExpr& expr, size_t num_blocks) {
  if (expr.op == RtlOp::kUnknown || HasNegativeRegNo(expr)) {
    return false;
  }
  if (expr.op == RtlOp::kSequence) {
    if (expr.children.size() < 2) {
      return false;
    }
    for (const RtlExpr& child : expr.children) {
      auto sub = RtlPatternAndNotes::FromSequenceSubInsn(child);
      if (!sub.ok()) {
        return false;
      }
      if (!ValidateExprStructure(sub->pattern, num_blocks)) {
        return false;
      }
      for (const RtlRegNote& note : sub->reg_notes) {
        if (note.kind == RtlMachineMode::kUnknown ||
            (note.list_op == RtlOp::kExprList && !ValidateExprStructure(note.datum, num_blocks))) {
          return false;
        }
      }
    }
  }
  if (expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec) {
    if (expr.children.empty()) {
      return false;
    }
    for (const RtlExpr& child : expr.children) {
      if (child.op == RtlOp::kLabelRef) {
        if (child.str_val.size() < 2 || child.str_val[0] != 'L') {
          return false;
        }
        int lbl_id = -1;
        std::string_view digits(child.str_val.data() + 1, child.str_val.size() - 1);
        if (!absl::SimpleAtoi(digits, &lbl_id) || lbl_id < 0 ||
            static_cast<size_t>(lbl_id) >= num_blocks) {
          return false;
        }
      }
    }
  }
  for (const RtlExpr& child : expr.children) {
    if (!ValidateExprStructure(child, num_blocks)) {
      return false;
    }
  }
  return true;
}

void RecordMaxRegNo(const RtlExpr& expr, int* max_regno) {
  if (expr.op == RtlOp::kReg) {
    *max_regno = std::max(*max_regno, expr.RegNo());
  }
  for (const RtlExpr& child : expr.children) {
    RecordMaxRegNo(child, max_regno);
  }
}

void InvalidateSlotRange(int64_t start_byte, int64_t end_byte, int frame_size_bytes,
                         absl::flat_hash_set<int>* invalidated_slots) {
  if (frame_size_bytes <= 0 || end_byte <= 0 || start_byte >= frame_size_bytes ||
      start_byte >= end_byte) {
    return;
  }
  const int64_t clamped_start = std::max<int64_t>(0, start_byte);
  const int64_t clamped_end = std::min<int64_t>(frame_size_bytes, end_byte);
  const int first_slot =
      static_cast<int>((clamped_start / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
  const int last_slot =
      static_cast<int>(((clamped_end - 1) / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
  for (int s = first_slot; s <= last_slot; s += kStackSlotAlignmentBytes) {
    invalidated_slots->insert(s);
  }
}

std::optional<int64_t> ExtractBaseRelativeConstOffset(const RtlExpr& addr, int base_regno) {
  if (addr.op == RtlOp::kReg &&
      (addr.mode == RtlMachineMode::kSi || addr.mode == RtlMachineMode::kDi) && !addr.flags.Any() &&
      addr.RegNo() == base_regno) {
    return 0;
  }
  if (addr.op == RtlOp::kPlus &&
      (addr.mode == RtlMachineMode::kSi || addr.mode == RtlMachineMode::kDi) && !addr.flags.Any() &&
      addr.children.size() == 2) {
    const RtlExpr& lhs = addr.children[0];
    const RtlExpr& rhs = addr.children[1];
    if (lhs.op == RtlOp::kReg &&
        (lhs.mode == RtlMachineMode::kSi || lhs.mode == RtlMachineMode::kDi) && !lhs.flags.Any() &&
        lhs.RegNo() == base_regno && rhs.op == RtlOp::kConstInt) {
      return rhs.int_val;
    }
  }
  return std::nullopt;
}

std::optional<int64_t> ExtractFpRelativeOffset(const RtlExpr& addr) {
  if (addr.op == RtlOp::kReg && addr.mode == RtlMachineMode::kSI && !addr.flags.Any() &&
      addr.RegNo() == kFramePointerRegno) {
    return 0;
  }
  if (addr.op == RtlOp::kPlus && addr.mode == RtlMachineMode::kSI && !addr.flags.Any() &&
      addr.children.size() == 2) {
    const RtlExpr& lhs = addr.children[0];
    const RtlExpr& rhs = addr.children[1];
    if (lhs.op == RtlOp::kReg && lhs.mode == RtlMachineMode::kSI && !lhs.flags.Any() &&
        lhs.RegNo() == kFramePointerRegno && rhs.op == RtlOp::kConstInt) {
      return rhs.int_val;
    }
  }
  return std::nullopt;
}

void AnalyzeFpStackSlots(const RtlExpr& expr, bool in_set_dest, bool in_note, int ref_weight,
                         int frame_size_bytes, int outgoing_args_size,
                         absl::flat_hash_map<int, StackSlotStats>* slots,
                         absl::flat_hash_set<int>* invalidated_slots) {
  if (expr.op == RtlOp::kSequence) {
    for (const RtlExpr& sub : expr.children) {
      if (!sub.children.empty()) {
        AnalyzeFpStackSlots(sub.children[0], /*in_set_dest=*/false, in_note, ref_weight,
                            frame_size_bytes, outgoing_args_size, slots, invalidated_slots);
      }
      for (size_t idx = 1; idx < sub.children.size(); ++idx) {
        AnalyzeFpStackSlots(sub.children[idx], /*in_set_dest=*/false, /*in_note=*/true, ref_weight,
                            frame_size_bytes, outgoing_args_size, slots, invalidated_slots);
      }
    }
    return;
  }

  if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
    AnalyzeFpStackSlots(expr.children[0], /*in_set_dest=*/true, in_note, ref_weight,
                        frame_size_bytes, outgoing_args_size, slots, invalidated_slots);
    AnalyzeFpStackSlots(expr.children[1], /*in_set_dest=*/false, in_note, ref_weight,
                        frame_size_bytes, outgoing_args_size, slots, invalidated_slots);
    return;
  }

  if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
    const std::optional<int64_t> fp_off = ExtractFpRelativeOffset(expr.children[0]);
    if (fp_off.has_value()) {
      const int64_t rel_off = *fp_off - outgoing_args_size;
      if (expr.mode == RtlMachineMode::kBLK) {
        if (rel_off < frame_size_bytes) {
          const int64_t start_slot =
              (rel_off <= 0) ? 0
                             : ((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
          InvalidateSlotRange(start_slot, frame_size_bytes, frame_size_bytes, invalidated_slots);
        }
        return;
      }
      const int mode_size = GetStackSpillModeSizeBytes(expr.mode);
      if (mode_size <= 0) {
        if (rel_off < frame_size_bytes) {
          const int64_t start_slot =
              (rel_off <= 0) ? 0
                             : ((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
          InvalidateSlotRange(start_slot, frame_size_bytes, frame_size_bytes, invalidated_slots);
        }
        return;
      }
      if (rel_off < frame_size_bytes && rel_off + mode_size > 0) {
        const int slot_base =
            (rel_off >= 0)
                ? static_cast<int>((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes)
                : -1;
        const int expected_rel_off = slot_base + (kStackSlotAlignmentBytes - mode_size);
        if (rel_off < 0 || rel_off + mode_size > frame_size_bytes || rel_off != expected_rel_off ||
            expr.flags.volatil || expr.flags.in_struct || expr.flags.integrated ||
            expr.flags.frame_related) {
          InvalidateSlotRange(rel_off, rel_off + mode_size, frame_size_bytes, invalidated_slots);
        } else {
          auto it = slots->find(slot_base);
          if (it == slots->end()) {
            StackSlotStats st;
            st.mode = expr.mode;
            st.unchanging = expr.flags.unchanging;
            st.n_refs = in_note ? 0 : ref_weight;
            st.n_sets = (!in_note && in_set_dest) ? 1 : 0;
            slots->emplace(slot_base, st);
          } else {
            if (it->second.mode != expr.mode || it->second.unchanging != expr.flags.unchanging) {
              it->second.valid = false;
              invalidated_slots->insert(slot_base);
            } else if (!in_note) {
              it->second.n_refs += ref_weight;
              if (in_set_dest) {
                it->second.n_sets += 1;
              }
            }
          }
        }
      }
      return;
    }
  }

  // Any `$fp + const_int` outside a `(mem ...)` address indicates an
  // address-taken C stack local/aggregate and invalidates all 8-byte slots from
  // its base upward through `frame_size_bytes`.
  if (expr.op == RtlOp::kPlus && expr.children.size() == 2) {
    const std::optional<int64_t> fp_off = ExtractFpRelativeOffset(expr);
    if (fp_off.has_value()) {
      const int64_t rel_off = *fp_off - outgoing_args_size;
      if (rel_off < frame_size_bytes) {
        const int64_t start_slot =
            (rel_off <= 0) ? 0 : ((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
        InvalidateSlotRange(start_slot, frame_size_bytes, frame_size_bytes, invalidated_slots);
      }
      return;
    }
  }
  // Any bare or dynamically-indexed `$fp` escape (e.g. `(plus $fp reg)` or
  // copying `$fp` to a GPR) invalidates the entire local frame `[0, frame_size_bytes)`.
  if (expr.op == RtlOp::kReg && expr.RegNo() == kFramePointerRegno) {
    InvalidateSlotRange(0, frame_size_bytes, frame_size_bytes, invalidated_slots);
    return;
  }

  for (const RtlExpr& child : expr.children) {
    AnalyzeFpStackSlots(child, /*in_set_dest=*/false, in_note, ref_weight, frame_size_bytes,
                        outgoing_args_size, slots, invalidated_slots);
  }
}

bool ContainsCallOp(const RtlExpr& expr);

void MarkEntryArgFpSlotAndStepB0Writes(const RtlExpr& pat, bool gp64, bool fp64,
                                       int frame_size_bytes, int outgoing_args_size,
                                       std::array<bool, kFirstPseudoRegister>* b0_written,
                                       absl::flat_hash_map<int, StackSlotStats>* slots) {
  if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kMem &&
      pat.children[0].children.size() == 1 && pat.children[1].op == RtlOp::kReg) {
    const int src_hr = pat.children[1].RegNo();
    if (IsIncomingArgumentHardReg(src_hr)) {
      const int span = std::max(1, MipsHardRegNregs(src_hr, pat.children[1].mode, gp64, fp64));
      bool any_written = false;
      for (int w = src_hr; w < std::min(kFirstPseudoRegister, src_hr + span); ++w) {
        if ((*b0_written)[w]) {
          any_written = true;
          break;
        }
      }
      if (!any_written) {
        const std::optional<int64_t> fp_off = ExtractFpRelativeOffset(pat.children[0].children[0]);
        if (fp_off.has_value()) {
          const int64_t rel_off = *fp_off - outgoing_args_size;
          if (rel_off >= 0 && rel_off < frame_size_bytes) {
            const int slot_base =
                static_cast<int>((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
            auto it = slots->find(slot_base);
            if (it != slots->end()) {
              it->second.has_entry_arg_store = true;
            }
          }
        }
      }
    }
  }
  auto mark_dest = [&](const RtlExpr& dst) {
    const RtlExpr* d = &dst;
    while ((d->op == RtlOp::kSubreg || d->op == RtlOp::kStrictLowPart ||
            d->op == RtlOp::kZeroExtract || d->op == RtlOp::kSignExtract) &&
           !d->children.empty()) {
      d = &d->children[0];
    }
    if (d->op == RtlOp::kReg && d->RegNo() >= 0 && d->RegNo() < kFirstPseudoRegister) {
      const int hr = d->RegNo();
      const int span = std::max(1, MipsHardRegNregs(hr, d->mode, gp64, fp64));
      for (int w = hr; w < std::min(kFirstPseudoRegister, hr + span); ++w) {
        (*b0_written)[w] = true;
      }
    }
  };
  if (ContainsCallOp(pat)) {
    for (int hr : {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 24, 25, 31, 32, 33,
                   34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51}) {
      (*b0_written)[hr] = true;
    }
  }
  pat.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kSet && n.children.size() == 2) {
      mark_dest(n.children[0]);
    } else if (n.op == RtlOp::kClobber && !n.children.empty()) {
      mark_dest(n.children[0]);
    }
  });
}

bool ContainsCallOp(const RtlExpr& expr) {
  if (expr.op == RtlOp::kCall) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (ContainsCallOp(child)) {
      return true;
    }
  }
  return false;
}

void InsertSortedUniqueToken(int tok, WordReachList* dst) {
  auto it = std::lower_bound(dst->begin(), dst->end(), tok);
  if (it == dst->end() || *it != tok) {
    dst->insert(it, tok);
  }
}

bool MergeWordReachList(const WordReachList& src, WordReachList* dst) {
  bool changed = false;
  for (int tok : src) {
    auto it = std::lower_bound(dst->begin(), dst->end(), tok);
    if (it == dst->end() || *it != tok) {
      dst->insert(it, tok);
      changed = true;
    }
  }
  return changed;
}

bool MergeReachState(const ReachState& src, ReachState* dst) {
  bool changed = false;
  for (int w = 0; w < kFirstPseudoRegister; ++w) {
    if (MergeWordReachList(src.defs[w], &dst->defs[w])) {
      changed = true;
    }
    if (MergeWordReachList(src.partial_overwriters[w], &dst->partial_overwriters[w])) {
      changed = true;
    }
  }
  return changed;
}

void RecordPartialMultiWordOverwrite(int write_start, int write_span, int overwriter_tok,
                                     const std::vector<RegOccurrence>& occs, ReachState* state) {
  const int w_begin = std::max(0, write_start);
  const int w_end = std::min(kFirstPseudoRegister, write_start + std::max(1, write_span));
  absl::InlinedVector<int, 4> overwritten_toks;
  for (int w = w_begin; w < w_end; ++w) {
    for (int tok : state->defs[w]) {
      if (tok >= 0 && !occs[tok].is_clobber_only && tok != overwriter_tok) {
        InsertSortedUniqueToken(tok, &overwritten_toks);
      }
    }
  }
  if (overwritten_toks.empty()) {
    return;
  }
  for (int tok : overwritten_toks) {
    const int tok_start = occs[tok].sig.hard_regno;
    const int tok_end = std::min(kFirstPseudoRegister, tok_start + occs[tok].word_span);
    for (int other_w = std::max(0, tok_start); other_w < tok_end; ++other_w) {
      if (other_w >= w_begin && other_w < w_end) {
        continue;
      }
      if (std::binary_search(state->defs[other_w].begin(), state->defs[other_w].end(), tok)) {
        InsertSortedUniqueToken(overwriter_tok, &state->partial_overwriters[other_w]);
      }
    }
  }
}

void SetWordRangeSingleToken(ReachState* state, int start_word, int span, int token) {
  const int end_word = std::min(kFirstPseudoRegister, start_word + std::max(1, span));
  for (int w = std::max(0, start_word); w < end_word; ++w) {
    state->defs[w].clear();
    state->defs[w].push_back(token);
    state->partial_overwriters[w].clear();
  }
}

void ClearWordRange(ReachState* state, int start_word, int span) {
  const int end_word = std::min(kFirstPseudoRegister, start_word + std::max(1, span));
  for (int w = std::max(0, start_word); w < end_word; ++w) {
    state->defs[w].clear();
    state->partial_overwriters[w].clear();
  }
}

// Registers all candidate `(reg ...)` AST occurrences inside a non-`SEQUENCE`
// body pattern `expr` in deterministic pre-order.
void RegisterPatternOccurrences(const RtlExpr& expr, bool in_set_dest, bool in_clobber,
                                int ref_weight, bool gp64, bool fp64, bool fp_is_fixed,
                                bool lift_di_clobber, std::vector<RegOccurrence>* occs,
                                absl::flat_hash_map<const RtlExpr*, int>* expr_to_occ) {
  if (expr.op == RtlOp::kNote || expr.op == RtlOp::kCodeLabel || expr.op == RtlOp::kBarrier ||
      expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec || expr.op == RtlOp::kReturn) {
    return;
  }

  if (IsCandidateHardRegNode(expr, gp64, fp64, fp_is_fixed)) {
    const int occ_id = static_cast<int>(occs->size());
    const int span = std::max(1, MipsHardRegNregs(expr.RegNo(), expr.mode, gp64, fp64));
    occs->push_back(RegOccurrence{
        .sig = MakeRegWebSignature(expr),
        .word_span = span,
        .ref_weight = in_clobber ? 0 : ref_weight,
        .set_count = (!in_clobber && in_set_dest) ? 1 : 0,
        .is_clobber_only = in_clobber,
    });
    expr_to_occ->emplace(&expr, occ_id);
    return;
  }

  if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
    RegisterPatternOccurrences(expr.children[0], /*in_set_dest=*/true, /*in_clobber=*/false,
                               ref_weight, gp64, fp64, fp_is_fixed, lift_di_clobber, occs,
                               expr_to_occ);
    RegisterPatternOccurrences(expr.children[1], /*in_set_dest=*/false, /*in_clobber=*/false,
                               ref_weight, gp64, fp64, fp_is_fixed, lift_di_clobber, occs,
                               expr_to_occ);
    return;
  }

  if (expr.op == RtlOp::kClobber && !expr.children.empty()) {
    RegisterPatternOccurrences(expr.children[0], /*in_set_dest=*/true, /*in_clobber=*/true,
                               ref_weight, gp64, fp64, fp_is_fixed, lift_di_clobber, occs,
                               expr_to_occ);
    return;
  }

  // On 32-bit GPR MIPS (`!TARGET_64BIT`), `mips.md` `DI`-mode arithmetic and
  // shift expanders (`adddi3`, `subdi3`, `negdi2`, `ashldi3`, `ashrdi3`,
  // `lshrdi3`) allocate their `SImode` carry/temp clobber via
  // `(clobber (match_operand:SI 3 "register_operand" "=&d"))` (`gen_reg_rtx`)
  // rather than `match_scratch`. At `-O0` (`stupid_life_analysis`), register
  // that `SImode` GPR clobber as a single-def occurrence so `InvertAlterReg`
  // lifts it to a pseudo.
  if (lift_di_clobber && !gp64 && !in_clobber && expr.op == RtlOp::kParallel &&
      expr.children.size() == 2 && expr.children[0].op == RtlOp::kSet &&
      expr.children[0].children.size() == 2 &&
      expr.children[0].children[0].mode == RtlMachineMode::kDI &&
      expr.children[0].children[1].mode == RtlMachineMode::kDI &&
      (expr.children[0].children[1].op == RtlOp::kPlus ||
       expr.children[0].children[1].op == RtlOp::kMinus ||
       expr.children[0].children[1].op == RtlOp::kNeg ||
       expr.children[0].children[1].op == RtlOp::kAshift ||
       expr.children[0].children[1].op == RtlOp::kAshiftrt ||
       expr.children[0].children[1].op == RtlOp::kLshiftrt) &&
      expr.children[1].op == RtlOp::kClobber && expr.children[1].children.size() == 1 &&
      expr.children[1].children[0].op == RtlOp::kReg &&
      expr.children[1].children[0].mode == RtlMachineMode::kSI &&
      expr.children[1].children[0].RegNo() >= 2 && expr.children[1].children[0].RegNo() < 32) {
    RegisterPatternOccurrences(expr.children[0], /*in_set_dest=*/false, /*in_clobber=*/false,
                               ref_weight, gp64, fp64, fp_is_fixed, lift_di_clobber, occs,
                               expr_to_occ);
    RegisterPatternOccurrences(expr.children[1].children[0], /*in_set_dest=*/true,
                               /*in_clobber=*/false, ref_weight, gp64, fp64, fp_is_fixed,
                               lift_di_clobber, occs, expr_to_occ);
    return;
  }

  if (expr.op == RtlOp::kMem) {
    for (const RtlExpr& child : expr.children) {
      RegisterPatternOccurrences(child, /*in_set_dest=*/false, /*in_clobber=*/false, ref_weight,
                                 gp64, fp64, fp_is_fixed, lift_di_clobber, occs, expr_to_occ);
    }
    return;
  }

  const bool pass_dest =
      in_set_dest && (expr.op == RtlOp::kSubreg || expr.op == RtlOp::kStrictLowPart ||
                      expr.op == RtlOp::kZeroExtract || expr.op == RtlOp::kSignExtract);
  for (size_t i = 0; i < expr.children.size(); ++i) {
    const bool child_dest = pass_dest && (i == 0);
    RegisterPatternOccurrences(expr.children[i], child_dest, in_clobber, ref_weight, gp64, fp64,
                               fp_is_fixed, lift_di_clobber, occs, expr_to_occ);
  }
}

struct WebCollector {
  const std::vector<RegOccurrence>* occs = nullptr;
  const absl::flat_hash_map<const RtlExpr*, int>* expr_to_occ = nullptr;
  WebUnionFind* uf = nullptr;
  std::vector<bool>* pinned_seed = nullptr;
  std::vector<absl::InlinedVector<int, 4>>* pin_adj = nullptr;
  absl::flat_hash_map<RegWebSignature, int>* uninit_anchors = nullptr;
  absl::flat_hash_map<RegWebSignature, int>* latest_set_occ_by_sig = nullptr;
  std::vector<bool>* stored_to_spill_slot = nullptr;
  const absl::flat_hash_set<int64_t>* reload_spill_offsets = nullptr;
  int spill_base_regno = 29;
  bool allow_unreachable_use_unify = false;
  bool is_optimized = false;

  void AddPinEdge(int a, int b) {
    if (a == b) {
      return;
    }
    auto& adj_a = (*pin_adj)[a];
    if (std::find(adj_a.begin(), adj_a.end(), b) == adj_a.end()) {
      adj_a.push_back(b);
    }
    auto& adj_b = (*pin_adj)[b];
    if (std::find(adj_b.begin(), adj_b.end(), a) == adj_b.end()) {
      adj_b.push_back(a);
    }
  }

  void RecordReadOccurrence(int occ_id, int start_word, int span, const ReachState& reach,
                            std::vector<int>* insn_read_occs) {
    if (insn_read_occs != nullptr) {
      insn_read_occs->push_back(occ_id);
    }
    const RegWebSignature& u_sig = (*occs)[occ_id].sig;
    const int end_word = std::min(kFirstPseudoRegister, start_word + std::max(1, span));
    for (int w = std::max(0, start_word); w < end_word; ++w) {
      if (!reach.partial_overwriters[w].empty()) {
        bool all_same_sig = true;
        for (int tok : reach.defs[w]) {
          if (tok == kPinnedAbiToken || (tok >= 0 && (*occs)[tok].sig != u_sig)) {
            all_same_sig = false;
          }
        }
        for (int pw_tok : reach.partial_overwriters[w]) {
          if (pw_tok < 0 || (*occs)[pw_tok].sig != u_sig) {
            all_same_sig = false;
          }
        }
        if (all_same_sig) {
          for (int pw_tok : reach.partial_overwriters[w]) {
            AddPinEdge(pw_tok, occ_id);
            uf->Union(pw_tok, occ_id);
          }
        } else {
          (*pinned_seed)[occ_id] = true;
          for (int tok : reach.defs[w]) {
            if (tok >= 0) {
              (*pinned_seed)[tok] = true;
            }
          }
          for (int pw_tok : reach.partial_overwriters[w]) {
            if (pw_tok >= 0) {
              (*pinned_seed)[pw_tok] = true;
            }
          }
        }
      }
      if (reach.defs[w].empty()) {
        if (allow_unreachable_use_unify && latest_set_occ_by_sig != nullptr) {
          auto set_it = latest_set_occ_by_sig->find(u_sig);
          if (set_it != latest_set_occ_by_sig->end()) {
            AddPinEdge(set_it->second, occ_id);
            uf->Union(set_it->second, occ_id);
            continue;
          }
        }
        // Reading a hard-register word with zero reaching definitions (e.g. a
        // post-reload `define_split` reading a `PARALLEL` `match_scratch`
        // `CLOBBER` register, or reading a call-clobbered register after a call)
        // must stay pinned on the physical hard register.
        (*pinned_seed)[occ_id] = true;
        continue;
      }
      bool has_same_sig_def = false;
      if (is_optimized && (*occs)[occ_id].is_top_level_use) {
        for (int tok : reach.defs[w]) {
          if (tok >= 0 && (*occs)[tok].sig == u_sig) {
            has_same_sig_def = true;
            break;
          }
        }
      }
      for (int tok : reach.defs[w]) {
        if (tok == kPinnedAbiToken) {
          if (has_same_sig_def) {
            auto [it, inserted] = uninit_anchors->try_emplace(u_sig, occ_id);
            if (!inserted) {
              uf->Union(it->second, occ_id);
            }
          } else {
            (*pinned_seed)[occ_id] = true;
          }
        } else if (tok == kUninitEntryToken) {
          auto [it, inserted] = uninit_anchors->try_emplace(u_sig, occ_id);
          if (!inserted) {
            uf->Union(it->second, occ_id);
          }
        } else if (tok >= 0) {
          AddPinEdge(tok, occ_id);
          if ((*occs)[tok].sig == u_sig) {
            uf->Union(tok, occ_id);
          } else {
            (*pinned_seed)[tok] = true;
            (*pinned_seed)[occ_id] = true;
          }
        }
      }
    }
  }

  void PinReachingDefs(int start_word, int span, const ReachState& reach) {
    const int end_word = std::min(kFirstPseudoRegister, start_word + std::max(1, span));
    for (int w = std::max(0, start_word); w < end_word; ++w) {
      for (int tok : reach.defs[w]) {
        if (tok >= 0) {
          (*pinned_seed)[tok] = true;
        }
      }
      for (int pw_tok : reach.partial_overwriters[w]) {
        if (pw_tok >= 0) {
          (*pinned_seed)[pw_tok] = true;
        }
      }
    }
  }
};

struct InsnNoteContext {
  ReachState pre_reach{};
  ReachState post_reach{};
  absl::InlinedVector<int, 4> read_occs;
  absl::InlinedVector<int, 4> write_occs;
};

struct ActiveLibcallNoteScope {
  ReachState pre_reach{};
  int target_pseudo = -1;
};

int LookupOccId(const RtlExpr& expr, const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ) {
  const auto it = expr_to_occ.find(&expr);
  return (it != expr_to_occ.end()) ? it->second : -1;
}

bool HasRegNoteKind(const RtlPatternAndNotes& pn, RtlMachineMode kind) {
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind == kind) {
      return true;
    }
  }
  return false;
}

int ExtractStandaloneClobberTargetOccId(
    const RtlExpr& pat, const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ) {
  if (pat.op != RtlOp::kClobber || pat.children.empty()) {
    return -1;
  }
  const RtlExpr* target = &pat.children[0];
  if (target->op == RtlOp::kSubreg && !target->children.empty()) {
    target = &target->children[0];
  }
  if (target->op != RtlOp::kReg) {
    return -1;
  }
  return LookupOccId(*target, expr_to_occ);
}

bool HasRegDeadOverlappingRange(absl::Span<const RtlRegNote> reg_notes, int write_start,
                                int write_span, bool gp64, bool fp64) {
  const int w_end = std::min(kFirstPseudoRegister, write_start + std::max(1, write_span));
  for (const RtlRegNote& note : reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg() &&
        !note.datum.IsArgPointerReg()) {
      const int nr = note.datum.RegNo();
      const int nspan = std::max(1, MipsHardRegNregs(nr, note.datum.mode, gp64, fp64));
      if (std::max(write_start, nr) < std::min(w_end, nr + nspan)) {
        return true;
      }
    }
  }
  return false;
}

void UnifyWriteWithClobbers(int occ_id, int write_start, int write_span, bool gp64, bool fp64,
                            const ReachState& state, absl::Span<const RtlRegNote> reg_notes,
                            absl::Span<const int> active_libcall_clobbers,
                            WebCollector* collector) {
  if (occ_id < 0 || collector == nullptr) {
    return;
  }
  const RegWebSignature& w_sig = (*collector->occs)[occ_id].sig;
  for (int clobber_occ : active_libcall_clobbers) {
    if (clobber_occ >= 0 && (*collector->occs)[clobber_occ].sig == w_sig) {
      collector->AddPinEdge(clobber_occ, occ_id);
      collector->uf->Union(clobber_occ, occ_id);
    }
  }
  const int end_word = std::min(kFirstPseudoRegister, write_start + std::max(1, write_span));
  bool has_uninit_entry = false;
  bool has_pinned_abi = false;
  for (int w = std::max(0, write_start); w < end_word; ++w) {
    for (int tok : state.defs[w]) {
      if (tok == kPinnedAbiToken) {
        has_pinned_abi = true;
      } else if (tok == kUninitEntryToken) {
        has_uninit_entry = true;
      } else if (tok >= 0 &&
                 ((*collector->occs)[tok].is_clobber_only ||
                  (*collector->occs)[tok].is_top_level_use) &&
                 (*collector->occs)[tok].sig == w_sig) {
        collector->AddPinEdge(tok, occ_id);
        collector->uf->Union(tok, occ_id);
      }
    }
  }
  if (has_uninit_entry && !has_pinned_abi &&
      !HasRegDeadOverlappingRange(reg_notes, write_start, write_span, gp64, fp64)) {
    // Unify read-modify-write initialization of an uninitialized local pseudo
    // (e.g. byte/halfword/bitfield lane insertion into a union local), unless an
    // explicit `REG_DEAD` note proves the read web died before this write.
    const auto it = collector->uninit_anchors->find(w_sig);
    if (it != collector->uninit_anchors->end()) {
      collector->AddPinEdge(it->second, occ_id);
      collector->uf->Union(it->second, occ_id);
    }
  }
}

void StepExprReads(const RtlExpr& expr, bool gp64, bool fp64, ReachState* state,
                   const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                   WebCollector* collector, std::vector<int>* insn_read_occs) {
  if (expr.op == RtlOp::kNote || expr.op == RtlOp::kCodeLabel || expr.op == RtlOp::kBarrier ||
      expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec || expr.op == RtlOp::kReturn ||
      expr.op == RtlOp::kPc || expr.op == RtlOp::kConstInt || expr.op == RtlOp::kConstDouble ||
      expr.op == RtlOp::kSymbolRef || expr.op == RtlOp::kLabelRef) {
    return;
  }

  if (expr.op == RtlOp::kSubreg && !expr.children.empty() && expr.children[0].op == RtlOp::kReg) {
    const RtlExpr& inner = expr.children[0];
    const int occ_id = LookupOccId(inner, expr_to_occ);
    if (occ_id >= 0 && collector != nullptr) {
      const int r = inner.RegNo();
      const int full_span = std::max(1, MipsHardRegNregs(r, inner.mode, gp64, fp64));
      const int word_off = std::max(0, static_cast<int>(expr.int_val));
      const int sub_span = std::max(1, MipsHardRegNregs(r + word_off, expr.mode, gp64, fp64));
      if (word_off < full_span && sub_span < full_span) {
        const int clamped_span = std::min(sub_span, full_span - word_off);
        collector->RecordReadOccurrence(occ_id, r + word_off, clamped_span, *state, insn_read_occs);
      } else {
        collector->RecordReadOccurrence(occ_id, r, full_span, *state, insn_read_occs);
      }
    }
    return;
  }

  if (expr.op == RtlOp::kReg) {
    const int occ_id = LookupOccId(expr, expr_to_occ);
    if (occ_id >= 0 && collector != nullptr) {
      const int r = expr.RegNo();
      const int span = std::max(1, MipsHardRegNregs(r, expr.mode, gp64, fp64));
      collector->RecordReadOccurrence(occ_id, r, span, *state, insn_read_occs);
    }
    return;
  }

  for (const RtlExpr& child : expr.children) {
    StepExprReads(child, gp64, fp64, state, expr_to_occ, collector, insn_read_occs);
  }
}

void StepSetDestReads(const RtlExpr& dst, bool gp64, bool fp64, ReachState* state,
                      const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                      WebCollector* collector, std::vector<int>* insn_read_occs) {
  if (dst.op == RtlOp::kMem) {
    for (const RtlExpr& child : dst.children) {
      StepExprReads(child, gp64, fp64, state, expr_to_occ, collector, insn_read_occs);
    }
    return;
  }
  if (dst.op == RtlOp::kStrictLowPart || dst.op == RtlOp::kZeroExtract ||
      dst.op == RtlOp::kSignExtract) {
    // Read-modify-write bitfield / strict_low_part destination reads both the
    // underlying register and any bit-offset/width expressions.
    for (const RtlExpr& child : dst.children) {
      StepExprReads(child, gp64, fp64, state, expr_to_occ, collector, insn_read_occs);
    }
    return;
  }
  if (dst.op == RtlOp::kSubreg && !dst.children.empty() && dst.children[0].op != RtlOp::kReg) {
    StepSetDestReads(dst.children[0], gp64, fp64, state, expr_to_occ, collector, insn_read_occs);
  }
}

bool MatchesActiveNoConflictClobber(int occ_id, absl::Span<const int> active_libcall_clobbers,
                                    const std::vector<RegOccurrence>& occs) {
  if (occ_id < 0) {
    return false;
  }
  const RegWebSignature& w_sig = occs[occ_id].sig;
  for (int clobber_occ : active_libcall_clobbers) {
    if (clobber_occ >= 0 && occs[clobber_occ].sig == w_sig) {
      return true;
    }
  }
  return false;
}

void StepSetDestWrite(const RtlExpr& dst, bool is_call_return_dest, bool gp64, bool fp64,
                      absl::Span<const RtlRegNote> reg_notes,
                      absl::Span<const int> active_libcall_clobbers, ReachState* state,
                      const std::vector<RegOccurrence>& occs,
                      const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                      WebCollector* collector, std::vector<int>* insn_write_occs) {
  if (dst.op == RtlOp::kMem || dst.op == RtlOp::kPc) {
    return;
  }

  if (dst.op == RtlOp::kStrictLowPart || dst.op == RtlOp::kZeroExtract ||
      dst.op == RtlOp::kSignExtract) {
    if (!dst.children.empty()) {
      StepSetDestWrite(dst.children[0], is_call_return_dest, gp64, fp64, reg_notes,
                       active_libcall_clobbers, state, occs, expr_to_occ, collector,
                       insn_write_occs);
    }
    return;
  }

  if (dst.op == RtlOp::kSubreg && !dst.children.empty() && dst.children[0].op == RtlOp::kReg) {
    const RtlExpr& inner = dst.children[0];
    const int r = inner.RegNo();
    if (r < 0 || r >= kFirstPseudoRegister) {
      return;
    }
    const int full_span = std::max(1, MipsHardRegNregs(r, inner.mode, gp64, fp64));
    const int word_off = std::max(0, static_cast<int>(dst.int_val));
    const int sub_span = std::max(1, MipsHardRegNregs(r + word_off, dst.mode, gp64, fp64));
    const int write_start = (word_off < full_span && sub_span < full_span) ? (r + word_off) : r;
    const int write_span = (word_off < full_span && sub_span < full_span)
                               ? std::min(sub_span, full_span - word_off)
                               : full_span;
    const int occ_id = LookupOccId(inner, expr_to_occ);
    if (is_call_return_dest || inner.flags.integrated) {
      if (occ_id >= 0 && collector != nullptr) {
        (*collector->pinned_seed)[occ_id] = true;
      }
      RecordPartialMultiWordOverwrite(write_start, write_span, kPinnedAbiToken, occs, state);
      SetWordRangeSingleToken(state, write_start, write_span, kPinnedAbiToken);
    } else if (occ_id >= 0) {
      if (insn_write_occs != nullptr) {
        insn_write_occs->push_back(occ_id);
      }
      if (collector != nullptr && collector->latest_set_occ_by_sig != nullptr) {
        (*collector->latest_set_occ_by_sig)[occs[occ_id].sig] = occ_id;
      }
      UnifyWriteWithClobbers(occ_id, write_start, write_span, gp64, fp64, *state, reg_notes,
                             active_libcall_clobbers, collector);
      if (!MatchesActiveNoConflictClobber(occ_id, active_libcall_clobbers, occs)) {
        RecordPartialMultiWordOverwrite(write_start, write_span, occ_id, occs, state);
      }
      SetWordRangeSingleToken(state, write_start, write_span, occ_id);
    } else {
      RecordPartialMultiWordOverwrite(write_start, write_span, kPinnedAbiToken, occs, state);
      ClearWordRange(state, write_start, write_span);
    }
    return;
  }

  if (dst.op == RtlOp::kReg) {
    const int r = dst.RegNo();
    if (r < 0 || r >= kFirstPseudoRegister) {
      return;
    }
    const int span = std::max(1, MipsHardRegNregs(r, dst.mode, gp64, fp64));
    const int occ_id = LookupOccId(dst, expr_to_occ);
    if (is_call_return_dest || dst.flags.integrated) {
      if (occ_id >= 0 && collector != nullptr) {
        (*collector->pinned_seed)[occ_id] = true;
      }
      RecordPartialMultiWordOverwrite(r, span, kPinnedAbiToken, occs, state);
      SetWordRangeSingleToken(state, r, span, kPinnedAbiToken);
    } else if (occ_id >= 0) {
      if (insn_write_occs != nullptr) {
        insn_write_occs->push_back(occ_id);
      }
      if (collector != nullptr && collector->latest_set_occ_by_sig != nullptr) {
        (*collector->latest_set_occ_by_sig)[occs[occ_id].sig] = occ_id;
      }
      UnifyWriteWithClobbers(occ_id, r, span, gp64, fp64, *state, reg_notes,
                             active_libcall_clobbers, collector);
      RecordPartialMultiWordOverwrite(r, span, occ_id, occs, state);
      SetWordRangeSingleToken(state, r, span, occ_id);
    } else {
      RecordPartialMultiWordOverwrite(r, span, kPinnedAbiToken, occs, state);
      ClearWordRange(state, r, span);
    }
  }
}

void ApplyClobberTarget(const RtlExpr& clobber_expr, bool standalone_top_level,
                        bool has_libcall_note, bool gp64, bool fp64, ReachState* state,
                        const std::vector<RegOccurrence>& occs,
                        const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                        std::vector<int>* insn_write_occs) {
  if (clobber_expr.op != RtlOp::kClobber || clobber_expr.children.empty()) {
    return;
  }
  const RtlExpr* target = &clobber_expr.children[0];
  if (target->op == RtlOp::kSubreg && !target->children.empty()) {
    target = &target->children[0];
  }
  if (target->op != RtlOp::kReg) {
    return;
  }
  const int r = target->RegNo();
  if (r < 0 || r >= kFirstPseudoRegister) {
    return;
  }
  const int span = std::max(1, MipsHardRegNregs(r, target->mode, gp64, fp64));
  const int occ_id = LookupOccId(*target, expr_to_occ);
  if (occ_id >= 0 && insn_write_occs != nullptr) {
    insn_write_occs->push_back(occ_id);
  }
  if (standalone_top_level && has_libcall_note && occ_id >= 0) {
    // `emit_no_conflict_block` (`optabs.c:2485-2525`): `local_alloc` may tie
    // `op0`/`op1` and `target` to the same hard register, so keep `state`
    // live across the `REG_LIBCALL` `CLOBBER` until inner `SET`s write each word.
    return;
  }
  if (standalone_top_level && occ_id >= 0) {
    RecordPartialMultiWordOverwrite(r, span, occ_id, occs, state);
    SetWordRangeSingleToken(state, r, span, occ_id);
  } else {
    RecordPartialMultiWordOverwrite(r, span, kPinnedAbiToken, occs, state);
    ClearWordRange(state, r, span);
  }
}

void ApplyCallFusageAndCallClobbers(const std::vector<RtlExpr>& call_fusage, bool gp64, bool fp64,
                                    ReachState* state, WebCollector* collector) {
  if (collector != nullptr) {
    for (const RtlExpr& item : call_fusage) {
      if (item.op == RtlOp::kUse && !item.children.empty() && item.children[0].op == RtlOp::kReg) {
        const RtlExpr& ureg = item.children[0];
        const int r = ureg.RegNo();
        if (r >= 0 && r < kFirstPseudoRegister) {
          const int span = std::max(1, MipsHardRegNregs(r, ureg.mode, gp64, fp64));
          collector->PinReachingDefs(r, span, *state);
        }
      }
    }
  }

  for (int w = 0; w < kFirstPseudoRegister; ++w) {
    if (IsCallUsedHardReg(w)) {
      state->defs[w].clear();
      state->partial_overwriters[w].clear();
    }
  }
  for (const RtlExpr& item : call_fusage) {
    if (item.op == RtlOp::kClobber && !item.children.empty() &&
        item.children[0].op == RtlOp::kReg) {
      const RtlExpr& creg = item.children[0];
      const int r = creg.RegNo();
      if (r >= 0 && r < kFirstPseudoRegister) {
        const int span = std::max(1, MipsHardRegNregs(r, creg.mode, gp64, fp64));
        ClearWordRange(state, r, span);
      }
    }
  }
}

void StepCallTargetReadsInPattern(const RtlExpr& pat, bool gp64, bool fp64, ReachState* state,
                                  const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                                  WebCollector* collector, std::vector<int>* insn_read_occs) {
  if (pat.op == RtlOp::kCall) {
    for (const RtlExpr& child : pat.children) {
      StepExprReads(child, gp64, fp64, state, expr_to_occ, collector, insn_read_occs);
    }
    return;
  }
  if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
    StepSetDestReads(pat.children[0], gp64, fp64, state, expr_to_occ, collector, insn_read_occs);
    StepExprReads(pat.children[1], gp64, fp64, state, expr_to_occ, collector, insn_read_occs);
    return;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kClobber) {
        if (!elem.children.empty() && elem.children[0].op == RtlOp::kMem) {
          StepSetDestReads(elem.children[0], gp64, fp64, state, expr_to_occ, collector,
                           insn_read_occs);
        }
      } else {
        StepCallTargetReadsInPattern(elem, gp64, fp64, state, expr_to_occ, collector,
                                     insn_read_occs);
      }
    }
  }
}

void StepCallReturnWritesInPattern(const RtlExpr& pat, bool gp64, bool fp64, ReachState* state,
                                   const std::vector<RegOccurrence>& occs,
                                   const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                                   WebCollector* collector, std::vector<int>* insn_write_occs) {
  if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
    StepSetDestWrite(pat.children[0], /*is_call_return_dest=*/true, gp64, fp64, {}, {}, state, occs,
                     expr_to_occ, collector, insn_write_occs);
    return;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kClobber) {
        ApplyClobberTarget(elem, /*standalone_top_level=*/false, /*has_libcall_note=*/false, gp64,
                           fp64, state, occs, expr_to_occ, insn_write_occs);
      }
    }
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        StepSetDestWrite(elem.children[0], /*is_call_return_dest=*/true, gp64, fp64, {}, {}, state,
                         occs, expr_to_occ, collector, insn_write_occs);
      }
    }
  }
}

void CollectWrittenHardRegWordsInPattern(const RtlExpr& pat, bool gp64, bool fp64,
                                         std::array<bool, kFirstPseudoRegister>* written) {
  auto mark_dst = [&](const RtlExpr& dst) {
    const RtlExpr* d = &dst;
    while ((d->op == RtlOp::kSubreg || d->op == RtlOp::kStrictLowPart ||
            d->op == RtlOp::kZeroExtract || d->op == RtlOp::kSignExtract) &&
           !d->children.empty()) {
      d = &d->children[0];
    }
    if (d->op == RtlOp::kReg && d->RegNo() >= 0 && d->RegNo() < kFirstPseudoRegister) {
      const int r = d->RegNo();
      const int span = std::max(1, MipsHardRegNregs(r, d->mode, gp64, fp64));
      for (int w = r; w < std::min(kFirstPseudoRegister, r + span); ++w) {
        (*written)[w] = true;
      }
    }
  };
  if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
    mark_dst(pat.children[0]);
  } else if (pat.op == RtlOp::kClobber && !pat.children.empty()) {
    mark_dst(pat.children[0]);
  } else if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        mark_dst(elem.children[0]);
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
        mark_dst(elem.children[0]);
      }
    }
  }
}

void ApplyRegDeadNotesForUnwrittenWords(const RtlExpr& pat,
                                        const std::vector<RtlRegNote>& reg_notes, bool gp64,
                                        bool fp64, ReachState* state) {
  if (reg_notes.empty()) {
    return;
  }
  std::array<bool, kFirstPseudoRegister> written{};
  CollectWrittenHardRegWordsInPattern(pat, gp64, fp64, &written);
  for (const RtlRegNote& note : reg_notes) {
    if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
      continue;
    }
    const int r = note.datum.RegNo();
    if (r < 0 || r >= kFirstPseudoRegister) {
      continue;
    }
    const int span = std::max(1, MipsHardRegNregs(r, note.datum.mode, gp64, fp64));
    const int end_w = std::min(kFirstPseudoRegister, r + span);
    bool overlaps_write = false;
    for (int w = r; w < end_w; ++w) {
      if (written[w]) {
        overlaps_write = true;
        break;
      }
    }
    if (!overlaps_write) {
      ClearWordRange(state, r, span);
    }
  }
}

// Steps a single non-`SEQUENCE` pattern forward through `state`.
void StepSinglePatternForward(const RtlExpr& pat, const std::vector<RtlRegNote>& reg_notes,
                              bool kill_dead_hard_regs, const std::vector<RtlExpr>& call_fusage,
                              bool is_exit_tail_use, bool has_libcall_note,
                              absl::Span<const int> active_libcall_clobbers, bool gp64, bool fp64,
                              ReachState* state, const std::vector<RegOccurrence>& occs,
                              const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                              WebCollector* collector, InsnNoteContext* out_note_ctx,
                              bool is_o0_scope_start_use = false,
                              bool suppress_exit_use_pin = false) {
  std::vector<int> read_occs;
  std::vector<int> write_occs;
  if (out_note_ctx != nullptr) {
    out_note_ctx->pre_reach = *state;
  }

  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kCodeLabel || pat.op == RtlOp::kBarrier ||
      pat.op == RtlOp::kAddrVec || pat.op == RtlOp::kAddrDiffVec || pat.op == RtlOp::kReturn) {
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
    }
    return;
  }

  if (ContainsCallOp(pat)) {
    StepCallTargetReadsInPattern(pat, gp64, fp64, state, expr_to_occ, collector, &read_occs);
    ApplyCallFusageAndCallClobbers(call_fusage, gp64, fp64, state, collector);
    StepCallReturnWritesInPattern(pat, gp64, fp64, state, occs, expr_to_occ, collector,
                                  &write_occs);
    if (kill_dead_hard_regs) {
      ApplyRegDeadNotesForUnwrittenWords(pat, reg_notes, gp64, fp64, state);
    }
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
      out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
      out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
    }
    return;
  }

  if (pat.op == RtlOp::kUse && !pat.children.empty()) {
    const RtlExpr& inner = pat.children[0];
    if (is_o0_scope_start_use && inner.op == RtlOp::kReg && inner.RegNo() >= 0 &&
        inner.RegNo() < kFirstPseudoRegister) {
      const int r = inner.RegNo();
      const int span = std::max(1, MipsHardRegNregs(r, inner.mode, gp64, fp64));
      const int occ_id = LookupOccId(inner, expr_to_occ);
      if (occ_id >= 0) {
        SetWordRangeSingleToken(state, r, span, occ_id);
        write_occs.push_back(occ_id);
      }
      if (out_note_ctx != nullptr) {
        out_note_ctx->post_reach = *state;
        out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
      }
      return;
    }
    if (inner.op == RtlOp::kReg && inner.RegNo() >= 0 && inner.RegNo() < kFirstPseudoRegister &&
        (inner.flags.integrated ||
         (!inner.flags.volatil && is_exit_tail_use && !suppress_exit_use_pin &&
          IsFunctionReturnHardReg(inner.RegNo()) && !state->defs[inner.RegNo()].empty()))) {
      const int r = inner.RegNo();
      const int span = std::max(1, MipsHardRegNregs(r, inner.mode, gp64, fp64));
      if (collector != nullptr) {
        collector->PinReachingDefs(r, span, *state);
        const int occ_id = LookupOccId(inner, expr_to_occ);
        if (occ_id >= 0) {
          (*collector->pinned_seed)[occ_id] = true;
        }
      }
      if (out_note_ctx != nullptr) {
        out_note_ctx->post_reach = *state;
      }
      return;
    }
    const bool prev_allow = (collector != nullptr) ? collector->allow_unreachable_use_unify : false;
    if (collector != nullptr && collector->latest_set_occ_by_sig != nullptr) {
      collector->allow_unreachable_use_unify = true;
    }
    StepExprReads(inner, gp64, fp64, state, expr_to_occ, collector, &read_occs);
    if (collector != nullptr) {
      collector->allow_unreachable_use_unify = prev_allow;
    }
    if (kill_dead_hard_regs) {
      ApplyRegDeadNotesForUnwrittenWords(pat, reg_notes, gp64, fp64, state);
    }
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
      out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
      out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
    }
    return;
  }

  if (pat.op == RtlOp::kClobber && !pat.children.empty()) {
    if (pat.children[0].op == RtlOp::kMem) {
      StepSetDestReads(pat.children[0], gp64, fp64, state, expr_to_occ, collector, &read_occs);
    } else {
      ApplyClobberTarget(pat, /*standalone_top_level=*/true, has_libcall_note, gp64, fp64, state,
                         occs, expr_to_occ, &write_occs);
    }
    if (kill_dead_hard_regs) {
      ApplyRegDeadNotesForUnwrittenWords(pat, reg_notes, gp64, fp64, state);
    }
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
      out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
      out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
    }
    return;
  }

  if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
    const ReachState pre_write_reach = *state;
    StepSetDestReads(pat.children[0], gp64, fp64, state, expr_to_occ, collector, &read_occs);
    StepExprReads(pat.children[1], gp64, fp64, state, expr_to_occ, collector, &read_occs);
    if (collector != nullptr && collector->is_optimized &&
        collector->stored_to_spill_slot != nullptr && collector->reload_spill_offsets != nullptr &&
        !collector->reload_spill_offsets->empty() && pat.children[0].op == RtlOp::kMem &&
        !pat.children[0].flags.volatil && !pat.children[0].flags.in_struct &&
        pat.children[0].children.size() == 1 && pat.children[1].op == RtlOp::kReg &&
        !pat.children[1].flags.integrated) {
      const int sr = pat.children[1].RegNo();
      const std::optional<int64_t> mem_off =
          ExtractBaseRelativeConstOffset(pat.children[0].children[0], collector->spill_base_regno);
      if (sr >= 0 && sr < kFirstPseudoRegister && mem_off.has_value() &&
          collector->reload_spill_offsets->contains(*mem_off)) {
        const int sspan = std::max(1, MipsHardRegNregs(sr, pat.children[1].mode, gp64, fp64));
        const int send = std::min(kFirstPseudoRegister, sr + sspan);
        for (int w = sr; w < send; ++w) {
          for (int tok : pre_write_reach.defs[w]) {
            if (tok >= 0) {
              (*collector->stored_to_spill_slot)[tok] = true;
            }
          }
        }
      }
    }
    StepSetDestWrite(pat.children[0], /*is_call_return_dest=*/false, gp64, fp64, reg_notes,
                     active_libcall_clobbers, state, occs, expr_to_occ, collector, &write_occs);
    if (collector != nullptr && collector->is_optimized &&
        collector->stored_to_spill_slot != nullptr && pat.children[0].op == RtlOp::kReg &&
        !pat.children[0].flags.Any()) {
      bool is_reload_input_def = (pat.children[0].mode == RtlMachineMode::kSF ||
                                  pat.children[0].mode == RtlMachineMode::kDF) &&
                                 pat.children[1].op == RtlOp::kReg &&
                                 pat.children[1].mode == pat.children[0].mode &&
                                 pat.children[0].RegNo() >= 32 && pat.children[0].RegNo() <= 63 &&
                                 pat.children[1].RegNo() >= 2 && pat.children[1].RegNo() <= 31;
      if (!is_reload_input_def && pat.children[1].op == RtlOp::kReg &&
          pat.children[1].RegNo() >= 0 && pat.children[1].RegNo() < kFirstPseudoRegister) {
        const int src_r = pat.children[1].RegNo();
        const int src_span = std::max(1, MipsHardRegNregs(src_r, pat.children[1].mode, gp64, fp64));
        const int src_end = std::min(kFirstPseudoRegister, src_r + src_span);
        for (int w = src_r; w < src_end && !is_reload_input_def; ++w) {
          for (int tok : pre_write_reach.defs[w]) {
            if (tok >= 0 && (*collector->stored_to_spill_slot)[tok]) {
              is_reload_input_def = true;
              break;
            }
          }
        }
      }
      if (!is_reload_input_def && collector->reload_spill_offsets != nullptr &&
          !collector->reload_spill_offsets->empty() && pat.children[1].op == RtlOp::kMem &&
          !pat.children[1].flags.volatil && !pat.children[1].flags.in_struct &&
          pat.children[1].children.size() == 1) {
        const std::optional<int64_t> load_off = ExtractBaseRelativeConstOffset(
            pat.children[1].children[0], collector->spill_base_regno);
        is_reload_input_def =
            load_off.has_value() && collector->reload_spill_offsets->contains(*load_off);
      }
      if (is_reload_input_def) {
        const int wocc = LookupOccId(pat.children[0], expr_to_occ);
        if (wocc >= 0) {
          (*collector->stored_to_spill_slot)[wocc] = true;
        }
      }
    }
    if (collector != nullptr && collector->is_optimized && active_libcall_clobbers.empty() &&
        pat.children[0].op == RtlOp::kReg && !pat.children[0].flags.integrated &&
        !pat.children[1].IsReg()) {
      const int r = pat.children[0].RegNo();
      if (r >= 0 && r < kFirstPseudoRegister) {
        const int span = std::max(1, MipsHardRegNregs(r, pat.children[0].mode, gp64, fp64));
        bool is_inherited_spill_reload = false;
        if (collector->stored_to_spill_slot != nullptr) {
          const int end_w = std::min(kFirstPseudoRegister, r + span);
          for (int w = r; w < end_w && !is_inherited_spill_reload; ++w) {
            for (int tok : pre_write_reach.defs[w]) {
              if (tok >= 0 && (*collector->stored_to_spill_slot)[tok]) {
                is_inherited_spill_reload = true;
                break;
              }
            }
          }
        }
        const int wocc = LookupOccId(pat.children[0], expr_to_occ);
        if (wocc >= 0 && !is_inherited_spill_reload &&
            !HasRegDeadOverlappingRange(reg_notes, r, span, gp64, fp64)) {
          for (int rocc : read_occs) {
            if (rocc >= 0 && occs[rocc].sig == occs[wocc].sig) {
              collector->uf->Union(rocc, wocc);
              collector->AddPinEdge(rocc, wocc);
            }
          }
        }
      }
    }
    if (kill_dead_hard_regs) {
      ApplyRegDeadNotesForUnwrittenWords(pat, reg_notes, gp64, fp64, state);
    }
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
      out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
      out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
    }
    return;
  }

  if (pat.op == RtlOp::kParallel) {
    // Evaluate all inputs against the pre-`PARALLEL` state before applying any
    // parallel clobbers or destination writes.
    ReachState input_state = *state;
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        StepSetDestReads(elem.children[0], gp64, fp64, &input_state, expr_to_occ, collector,
                         &read_occs);
        StepExprReads(elem.children[1], gp64, fp64, &input_state, expr_to_occ, collector,
                      &read_occs);
      } else if (elem.op == RtlOp::kUse && !elem.children.empty()) {
        StepExprReads(elem.children[0], gp64, fp64, &input_state, expr_to_occ, collector,
                      &read_occs);
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty() &&
                 elem.children[0].op == RtlOp::kMem) {
        StepSetDestReads(elem.children[0], gp64, fp64, &input_state, expr_to_occ, collector,
                         &read_occs);
      } else if (elem.op != RtlOp::kClobber && elem.op != RtlOp::kReturn) {
        StepExprReads(elem, gp64, fp64, &input_state, expr_to_occ, collector, &read_occs);
      }
    }
    *state = std::move(input_state);
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kClobber) {
        ApplyClobberTarget(elem, /*standalone_top_level=*/false, /*has_libcall_note=*/false, gp64,
                           fp64, state, occs, expr_to_occ, &write_occs);
      }
    }
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        StepSetDestWrite(elem.children[0], /*is_call_return_dest=*/false, gp64, fp64, reg_notes,
                         active_libcall_clobbers, state, occs, expr_to_occ, collector, &write_occs);
      }
    }
    if (kill_dead_hard_regs) {
      ApplyRegDeadNotesForUnwrittenWords(pat, reg_notes, gp64, fp64, state);
    }
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
      out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
      out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
    }
    return;
  }

  StepExprReads(pat, gp64, fp64, state, expr_to_occ, collector, &read_occs);
  if (kill_dead_hard_regs) {
    ApplyRegDeadNotesForUnwrittenWords(pat, reg_notes, gp64, fp64, state);
  }
  if (out_note_ctx != nullptr) {
    out_note_ctx->post_reach = *state;
    out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
    out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
  }
}

bool IsTrivialExitTailInsn(const RtlExpr& pat) {
  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber ||
      pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty() && pat.children[0].op == RtlOp::kReturn) {
    return true;
  }
  return false;
}

int LookupPseudoForOcc(int occ_id, WebUnionFind* uf, const std::vector<bool>& pinned_occ,
                       const absl::flat_hash_map<int, WebComponentStats>& comp_stats) {
  if (occ_id < 0 || pinned_occ[occ_id]) {
    return -1;
  }
  const int root = uf->Find(occ_id);
  const auto it = comp_stats.find(root);
  if (it == comp_stats.end() || it->second.pseudo_regno < kFirstPseudoRegister) {
    return -1;
  }
  return it->second.pseudo_regno;
}

int LookupNotePseudoFromOccList(const RegWebSignature& sig, absl::Span<const int> occ_list,
                                const std::vector<RegOccurrence>& occs, WebUnionFind* uf,
                                const std::vector<bool>& pinned_occ,
                                const absl::flat_hash_map<int, WebComponentStats>& comp_stats) {
  for (int occ_id : occ_list) {
    if (occ_id >= 0 && occs[occ_id].sig == sig) {
      const int pseudo = LookupPseudoForOcc(occ_id, uf, pinned_occ, comp_stats);
      if (pseudo >= kFirstPseudoRegister) {
        return pseudo;
      }
    }
  }
  return -1;
}

int LookupNotePseudoFromReach(const RtlExpr& reg, bool gp64, bool fp64, const ReachState& reach,
                              const std::vector<RegOccurrence>& occs, WebUnionFind* uf,
                              const std::vector<bool>& pinned_occ,
                              const absl::flat_hash_map<int, WebComponentStats>& comp_stats) {
  const RegWebSignature sig = MakeRegWebSignature(reg);
  const int r = reg.RegNo();
  const int span = std::max(1, MipsHardRegNregs(r, reg.mode, gp64, fp64));
  const int end_word = std::min(kFirstPseudoRegister, r + span);
  for (int w = std::max(0, r); w < end_word; ++w) {
    for (int tok : reach.defs[w]) {
      if (tok == kPinnedAbiToken || (tok >= 0 && pinned_occ[tok])) {
        return -1;
      }
    }
  }
  for (int w = std::max(0, r); w < end_word; ++w) {
    for (int tok : reach.defs[w]) {
      if (tok >= 0 && occs[tok].sig == sig) {
        const int pseudo = LookupPseudoForOcc(tok, uf, pinned_occ, comp_stats);
        if (pseudo >= kFirstPseudoRegister) {
          return pseudo;
        }
      }
    }
  }
  return -1;
}

void RewritePatternAst(RtlExpr* expr, int outgoing_args_size,
                       const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                       WebUnionFind* uf, const std::vector<bool>& pinned_occ,
                       const absl::flat_hash_map<int, WebComponentStats>& comp_stats,
                       const absl::flat_hash_map<int, int>& slot_to_pseudo) {
  const int occ_id = LookupOccId(*expr, expr_to_occ);
  if (occ_id >= 0) {
    const int pseudo = LookupPseudoForOcc(occ_id, uf, pinned_occ, comp_stats);
    if (pseudo >= kFirstPseudoRegister) {
      expr->SetRegNo(pseudo);
    }
    return;
  }

  if (!slot_to_pseudo.empty() && expr->op == RtlOp::kMem && expr->children.size() == 1) {
    const std::optional<int64_t> fp_off = ExtractFpRelativeOffset(expr->children[0]);
    if (fp_off.has_value()) {
      const int64_t rel_off = *fp_off - outgoing_args_size;
      if (rel_off >= 0) {
        const int slot_base =
            static_cast<int>((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
        const auto it = slot_to_pseudo.find(slot_base);
        if (it != slot_to_pseudo.end()) {
          *expr = RtlExpr::MakeReg(expr->mode, it->second, expr->flags);
          return;
        }
      }
    }
  }

  for (RtlExpr& child : expr->children) {
    RewritePatternAst(&child, outgoing_args_size, expr_to_occ, uf, pinned_occ, comp_stats,
                      slot_to_pseudo);
  }
}

void RewriteNoteDatumExpr(RtlExpr* expr, RtlMachineMode note_kind, bool gp64, bool fp64,
                          bool fp_is_fixed, int outgoing_args_size, const InsnNoteContext& note_ctx,
                          const ReachState* retval_libcall_reach,
                          const ActiveLibcallNoteScope* active_libcall_scope,
                          const std::vector<RegOccurrence>& occs, WebUnionFind* uf,
                          const std::vector<bool>& pinned_occ,
                          const absl::flat_hash_map<int, WebComponentStats>& comp_stats,
                          const absl::flat_hash_map<int, int>& slot_to_pseudo) {
  if (IsCandidateHardRegNode(*expr, gp64, fp64, fp_is_fixed)) {
    const RegWebSignature sig = MakeRegWebSignature(*expr);
    int pseudo = -1;
    if (note_kind == RtlMachineMode::kRegUnused) {
      pseudo =
          LookupNotePseudoFromOccList(sig, note_ctx.write_occs, occs, uf, pinned_occ, comp_stats);
      if (pseudo < kFirstPseudoRegister) {
        pseudo = LookupNotePseudoFromReach(*expr, gp64, fp64, note_ctx.post_reach, occs, uf,
                                           pinned_occ, comp_stats);
      }
    } else if (note_kind == RtlMachineMode::kRegInc) {
      pseudo =
          LookupNotePseudoFromOccList(sig, note_ctx.write_occs, occs, uf, pinned_occ, comp_stats);
      if (pseudo < kFirstPseudoRegister) {
        pseudo =
            LookupNotePseudoFromOccList(sig, note_ctx.read_occs, occs, uf, pinned_occ, comp_stats);
      }
    } else if ((note_kind == RtlMachineMode::kRegEqual || note_kind == RtlMachineMode::kRegEquiv) &&
               retval_libcall_reach != nullptr) {
      pseudo = LookupNotePseudoFromReach(*expr, gp64, fp64, *retval_libcall_reach, occs, uf,
                                         pinned_occ, comp_stats);
      if (pseudo < kFirstPseudoRegister) {
        pseudo = LookupNotePseudoFromReach(*expr, gp64, fp64, note_ctx.pre_reach, occs, uf,
                                           pinned_occ, comp_stats);
      }
    } else if (note_kind == RtlMachineMode::kRegNoConflict) {
      pseudo =
          LookupNotePseudoFromOccList(sig, note_ctx.read_occs, occs, uf, pinned_occ, comp_stats);
      if (active_libcall_scope != nullptr &&
          (pseudo < kFirstPseudoRegister || pseudo == active_libcall_scope->target_pseudo)) {
        pseudo = LookupNotePseudoFromReach(*expr, gp64, fp64, active_libcall_scope->pre_reach, occs,
                                           uf, pinned_occ, comp_stats);
      }
      if (pseudo < kFirstPseudoRegister) {
        pseudo = LookupNotePseudoFromReach(*expr, gp64, fp64, note_ctx.pre_reach, occs, uf,
                                           pinned_occ, comp_stats);
      }
    } else {
      pseudo =
          LookupNotePseudoFromOccList(sig, note_ctx.read_occs, occs, uf, pinned_occ, comp_stats);
      if (pseudo < kFirstPseudoRegister) {
        pseudo = LookupNotePseudoFromReach(*expr, gp64, fp64, note_ctx.pre_reach, occs, uf,
                                           pinned_occ, comp_stats);
      }
    }
    if (pseudo >= kFirstPseudoRegister) {
      expr->SetRegNo(pseudo);
    }
    return;
  }

  if (!slot_to_pseudo.empty() && expr->op == RtlOp::kMem && expr->children.size() == 1) {
    const std::optional<int64_t> fp_off = ExtractFpRelativeOffset(expr->children[0]);
    if (fp_off.has_value()) {
      const int64_t rel_off = *fp_off - outgoing_args_size;
      if (rel_off >= 0) {
        const int slot_base =
            static_cast<int>((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
        const auto it = slot_to_pseudo.find(slot_base);
        if (it != slot_to_pseudo.end()) {
          *expr = RtlExpr::MakeReg(expr->mode, it->second, expr->flags);
          return;
        }
      }
    }
  }

  for (RtlExpr& child : expr->children) {
    RewriteNoteDatumExpr(&child, note_kind, gp64, fp64, fp_is_fixed, outgoing_args_size, note_ctx,
                         retval_libcall_reach, active_libcall_scope, occs, uf, pinned_occ,
                         comp_stats, slot_to_pseudo);
  }
}

void RewriteSequenceSubInsnNotesChain(RtlExpr* chain, bool gp64, bool fp64, bool fp_is_fixed,
                                      int outgoing_args_size, const InsnNoteContext& note_ctx,
                                      const std::vector<RegOccurrence>& occs, WebUnionFind* uf,
                                      const std::vector<bool>& pinned_occ,
                                      const absl::flat_hash_map<int, WebComponentStats>& comp_stats,
                                      const absl::flat_hash_map<int, int>& slot_to_pseudo) {
  if (chain->op == RtlOp::kExprList && chain->mode != RtlMachineMode::kNone &&
      !chain->children.empty()) {
    RewriteNoteDatumExpr(&chain->children[0], chain->mode, gp64, fp64, fp_is_fixed,
                         outgoing_args_size, note_ctx,
                         /*retval_libcall_reach=*/nullptr, /*active_libcall_scope=*/nullptr, occs,
                         uf, pinned_occ, comp_stats, slot_to_pseudo);
    if (chain->children.size() >= 2) {
      RewriteSequenceSubInsnNotesChain(&chain->children[1], gp64, fp64, fp_is_fixed,
                                       outgoing_args_size, note_ctx, occs, uf, pinned_occ,
                                       comp_stats, slot_to_pseudo);
    }
    return;
  }
  if (chain->op == RtlOp::kInsnList && chain->children.size() >= 2) {
    RewriteSequenceSubInsnNotesChain(&chain->children[1], gp64, fp64, fp_is_fixed,
                                     outgoing_args_size, note_ctx, occs, uf, pinned_occ, comp_stats,
                                     slot_to_pseudo);
  }
}

void PrecomputeO0ScopeUseFlags(const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
                               const std::vector<std::vector<bool>>& is_exit_tail, int opt_level,
                               bool gp64, bool fp64,
                               std::vector<std::vector<bool>>* is_o0_scope_start_use,
                               std::vector<std::vector<bool>>* suppress_exit_use_pin) {
  const size_t num_blocks = parsed_blocks.size();
  is_o0_scope_start_use->assign(num_blocks, {});
  suppress_exit_use_pin->assign(num_blocks, {});
  for (size_t b = 0; b < num_blocks; ++b) {
    (*is_o0_scope_start_use)[b].assign(parsed_blocks[b].size(), false);
    (*suppress_exit_use_pin)[b].assign(parsed_blocks[b].size(), false);
  }
  if (opt_level != 0) {
    return;
  }

  std::array<bool, kFirstPseudoRegister> has_non_exit_top_use{};
  for (size_t b = 0; b < num_blocks; ++b) {
    for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
      const RtlExpr& pat = parsed_blocks[b][i].pattern;
      if (!is_exit_tail[b][i] && pat.op == RtlOp::kUse && pat.children.size() == 1 &&
          pat.children[0].IsHardReg() && !pat.children[0].flags.integrated) {
        const int hr = pat.children[0].RegNo();
        if (hr >= 0 && hr < kFirstPseudoRegister) {
          has_non_exit_top_use[hr] = true;
        }
      }
    }
  }

  auto expr_reads_word = [&](const auto& self, const RtlExpr& e, int w, bool in_set_dst) -> bool {
    if (e.op == RtlOp::kSet && e.children.size() == 2) {
      return self(self, e.children[1], w, false) || self(self, e.children[0], w, true);
    }
    if (e.op == RtlOp::kClobber) {
      if (!e.children.empty() && e.children[0].op == RtlOp::kMem) {
        return self(self, e.children[0], w, false);
      }
      return false;
    }
    if (in_set_dst) {
      if (e.op == RtlOp::kReg ||
          (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg)) {
        return false;
      }
    }
    if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].IsHardReg()) {
      const int r = e.children[0].RegNo();
      const int off = std::max(0, static_cast<int>(e.int_val));
      const int sub_span = std::max(1, MipsHardRegNregs(r + off, e.mode, gp64, fp64));
      return w >= r + off && w < r + off + sub_span;
    }
    if (e.IsHardReg() && !e.IsArgPointerReg()) {
      const int r = e.RegNo();
      const int sp = std::max(1, MipsHardRegNregs(r, e.mode, gp64, fp64));
      return w >= r && w < r + sp;
    }
    for (const RtlExpr& ch : e.children) {
      if (self(self, ch, w, false)) {
        return true;
      }
    }
    return false;
  };

  auto insn_writes_word = [&](const RtlExpr& pat, int w) -> bool {
    auto elem_writes = [&](const RtlExpr& el) -> bool {
      if (el.op != RtlOp::kSet && (el.op != RtlOp::kClobber || pat.op == RtlOp::kParallel)) {
        return false;
      }
      if (el.children.empty()) {
        return false;
      }
      const RtlExpr& d = el.children[0];
      if (d.op == RtlOp::kSubreg && !d.children.empty() && d.children[0].IsHardReg()) {
        const int r = d.children[0].RegNo();
        const int off = std::max(0, static_cast<int>(d.int_val));
        const int sub_span = std::max(1, MipsHardRegNregs(r + off, d.mode, gp64, fp64));
        return w >= r + off && w < r + off + sub_span;
      }
      if (d.IsHardReg() && !d.IsArgPointerReg() && !d.flags.integrated) {
        const int r = d.RegNo();
        const int sp = std::max(1, MipsHardRegNregs(r, d.mode, gp64, fp64));
        return w >= r && w < r + sp;
      }
      return false;
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        if (elem_writes(ch)) {
          return true;
        }
      }
      return false;
    }
    return elem_writes(pat);
  };

  for (size_t b = 0; b < num_blocks; ++b) {
    for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
      const RtlExpr& pat = parsed_blocks[b][i].pattern;
      if (pat.op != RtlOp::kUse || pat.children.size() != 1 || !pat.children[0].IsHardReg() ||
          pat.children[0].IsArgPointerReg() || pat.children[0].flags.integrated) {
        continue;
      }
      const RtlExpr& ureg = pat.children[0];
      const int hr = ureg.RegNo();
      if (hr < 0 || hr >= kFirstPseudoRegister) {
        continue;
      }
      if (is_exit_tail[b][i]) {
        if (has_non_exit_top_use[hr]) {
          (*suppress_exit_use_pin)[b][i] = true;
        }
        continue;
      }
      const int span = std::max(1, MipsHardRegNregs(hr, ureg.mode, gp64, fp64));
      bool all_killed_before_read = true;
      for (int w = hr; w < std::min(kFirstPseudoRegister, hr + span); ++w) {
        bool word_killed = false;
        for (size_t k = i + 1; k < parsed_blocks[b].size(); ++k) {
          const RtlPatternAndNotes& kpn = parsed_blocks[b][k];
          if (kpn.pattern.op == RtlOp::kNote || kpn.pattern.op == RtlOp::kUse) {
            continue;
          }
          bool reads_w = expr_reads_word(expr_reads_word, kpn.pattern, w, false);
          if (!reads_w && ContainsCallOp(kpn.pattern)) {
            for (const RtlExpr& fu : kpn.call_fusage) {
              if (fu.op == RtlOp::kUse && expr_reads_word(expr_reads_word, fu, w, false)) {
                reads_w = true;
                break;
              }
            }
          }
          if (reads_w) {
            break;
          }
          if (insn_writes_word(kpn.pattern, w)) {
            word_killed = true;
            break;
          }
        }
        if (!word_killed) {
          all_killed_before_read = false;
          break;
        }
      }
      if (all_killed_before_read) {
        (*is_o0_scope_start_use)[b][i] = true;
      }
    }
  }
}

void PrecomputePairedLibcallScopes(
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const std::vector<RegOccurrence>& occs,
    const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
    std::vector<std::vector<bool>>* is_paired_libcall,
    std::vector<std::vector<bool>>* is_paired_retval) {
  const size_t num_blocks = parsed_blocks.size();
  is_paired_libcall->resize(num_blocks);
  is_paired_retval->resize(num_blocks);
  for (size_t b = 0; b < num_blocks; ++b) {
    const size_t n_insns = parsed_blocks[b].size();
    (*is_paired_libcall)[b].assign(n_insns, false);
    (*is_paired_retval)[b].assign(n_insns, false);
    struct PendingLibcall {
      size_t insn_idx = 0;
      int clobber_occ = -1;
    };
    std::vector<PendingLibcall> pending;
    for (size_t i = 0; i < n_insns; ++i) {
      const RtlPatternAndNotes& pn = parsed_blocks[b][i];
      if (HasRegNoteKind(pn, RtlMachineMode::kRegLibcall)) {
        pending.push_back(PendingLibcall{
            .insn_idx = i,
            .clobber_occ = ExtractStandaloneClobberTargetOccId(pn.pattern, expr_to_occ),
        });
      }
      bool has_call = false;
      pn.pattern.Visit([&](const RtlExpr& e) {
        if (e.op == RtlOp::kCall || e.op == RtlOp::kCallInsn) {
          has_call = true;
        }
      });
      if (has_call) {
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [](const PendingLibcall& p) { return p.clobber_occ >= 0; }),
                      pending.end());
      }
      if (HasRegNoteKind(pn, RtlMachineMode::kRegRetval)) {
        while (!pending.empty()) {
          const PendingLibcall top = pending.back();
          pending.pop_back();
          if (top.clobber_occ >= 0) {
            bool matches_clobber_target = false;
            const RegWebSignature& clob_sig = occs[top.clobber_occ].sig;
            auto check_set = [&](const RtlExpr& s) {
              if (s.op == RtlOp::kSet && !s.children.empty()) {
                const int dst_occ = LookupOccId(s.children[0], expr_to_occ);
                if (dst_occ >= 0 && occs[dst_occ].sig.hard_regno == clob_sig.hard_regno &&
                    occs[dst_occ].sig.mode == clob_sig.mode) {
                  matches_clobber_target = true;
                }
              }
            };
            if (pn.pattern.op == RtlOp::kParallel) {
              for (const RtlExpr& sub : pn.pattern.children) {
                check_set(sub);
              }
            } else {
              check_set(pn.pattern);
            }
            if (!matches_clobber_target) {
              continue;
            }
          }
          (*is_paired_libcall)[b][top.insn_idx] = true;
          (*is_paired_retval)[b][i] = true;
          break;
        }
      }
    }
  }
}

void AssignUnoptimizedStackSlotPseudos(const RtlControlFlowGraph& allocated_cfg,
                                       const std::vector<RegOccurrence>& occs,
                                       const absl::flat_hash_map<int, StackSlotStats>& stack_slots,
                                       const absl::flat_hash_set<int>& invalidated_slots,
                                       int frame_size_bytes, int* next_pseudo,
                                       absl::flat_hash_map<int, int>* slot_to_pseudo,
                                       RtlControlFlowGraph* out) {
  absl::flat_hash_set<int> seen_hard_regs;
  for (int hr : allocated_cfg.sideband.saved_gpr_regnos) {
    seen_hard_regs.insert(hr);
  }
  for (int hr : allocated_cfg.sideband.saved_fpr_regnos) {
    seen_hard_regs.insert(hr);
  }
  for (const RegOccurrence& occ : occs) {
    for (int w = 0; w < occ.word_span; ++w) {
      seen_hard_regs.insert(occ.sig.hard_regno + w);
    }
  }
  bool all_saved_gprs_used = true;
  for (int hr = 16; hr <= 23; ++hr) {
    if (!seen_hard_regs.contains(hr)) {
      all_saved_gprs_used = false;
      break;
    }
  }
  bool all_saved_fprs_used = true;
  for (int hr = 52; hr <= 62; hr += 2) {
    if (!seen_hard_regs.contains(hr)) {
      all_saved_fprs_used = false;
      break;
    }
  }

  std::vector<int> valid_top_slot_bases;
  for (int slot_base = frame_size_bytes - kStackSlotAlignmentBytes; slot_base >= 0;
       slot_base -= kStackSlotAlignmentBytes) {
    if (invalidated_slots.contains(slot_base)) {
      break;
    }
    auto it = stack_slots.find(slot_base);
    if (it == stack_slots.end() || !it->second.valid || it->second.n_refs <= 0) {
      break;
    }
    if (it->second.mode == RtlMachineMode::kDF && !all_saved_fprs_used &&
        !it->second.has_entry_arg_store) {
      break;
    }
    if (it->second.mode == RtlMachineMode::kDI && !all_saved_gprs_used &&
        !it->second.has_entry_arg_store) {
      break;
    }
    valid_top_slot_bases.push_back(slot_base);
  }
  std::reverse(valid_top_slot_bases.begin(), valid_top_slot_bases.end());
  for (int slot_base : valid_top_slot_bases) {
    const StackSlotStats& st = stack_slots.at(slot_base);
    const int pseudo = (*next_pseudo)++;
    (*slot_to_pseudo)[slot_base] = pseudo;
    out->sideband.reg_renumber[pseudo] = -1;
    out->sideband.reg_n_refs[pseudo] = std::max(1, st.n_refs);
    out->sideband.reg_n_sets[pseudo] = st.n_sets;
    out->sideband.frame_size_bytes -= kStackSlotAlignmentBytes;
  }
}

void CoalesceSameRegRenumberRmwPseudos(
    const CompilationProfile& profile, const ParsedCompilerFlags& parsed_flags, bool gp64,
    bool fp64, RtlControlFlowGraph* out,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr) {
  std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks = *all_parsed_blocks_ptr;
  RtlControlFlowGraph live_probe;
  if (parsed_flags.opt_level > 0) {
    live_probe = *out;
    for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
      RtlBasicBlock& blk = live_probe.rpo_blocks[b];
      for (size_t i = 0; i < all_parsed_blocks[b].size(); ++i) {
        const RtlPatternAndNotes& pn = all_parsed_blocks[b][i];
        blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(
            pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{});
      }
      blk.SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < blk.rtl_insns.size() && i < all_parsed_blocks[b].size(); ++i) {
        if (all_parsed_blocks[b][i].has_explicit_call_fusage &&
            !blk.rtl_insns[i].has_explicit_call_fusage) {
          blk.rtl_insns[i].has_explicit_call_fusage = true;
          blk.rtl_insns[i].call_fusage = all_parsed_blocks[b][i].call_fusage;
        }
      }
    }
    ReconstructRtlLiveness(&live_probe, profile,
                           RtlLivenessOptions{
                               .reload_completed = false,
                               .update_reg_notes = false,
                           });
  }
  auto is_non_note_suid_insn = [](const RtlPatternAndNotes& pn) {
    return pn.pattern.op != RtlOp::kNote && pn.pattern.op != RtlOp::kAddrVec &&
           pn.pattern.op != RtlOp::kAddrDiffVec;
  };
  auto block_trailing_extra_suids = [&](size_t b) -> int {
    const RtlBasicBlock& blk = out->rpo_blocks[b];
    bool emitted_any_jump = false;
    bool emitted_uncond_jump_or_return = false;
    if (!all_parsed_blocks[b].empty()) {
      const RtlExpr& tail = all_parsed_blocks[b].back().pattern;
      const RtlExpr* top = &tail;
      if (tail.op == RtlOp::kSequence && !tail.children.empty() &&
          !tail.children[0].children.empty()) {
        top = &tail.children[0].children[0];
      }
      if (top->op == RtlOp::kReturn || (top->op == RtlOp::kParallel && !top->children.empty() &&
                                        top->children[0].op == RtlOp::kReturn)) {
        emitted_any_jump = true;
        emitted_uncond_jump_or_return = true;
      } else if (top->op == RtlOp::kSet && top->children.size() == 2 &&
                 top->children[0].op == RtlOp::kPc) {
        emitted_any_jump = true;
        if (top->children[1].op != RtlOp::kIfThenElse) {
          emitted_uncond_jump_or_return = true;
        }
      } else if (top->op == RtlOp::kParallel && !top->children.empty() &&
                 top->children[0].op == RtlOp::kSet && top->children[0].children.size() == 2 &&
                 top->children[0].children[0].op == RtlOp::kPc) {
        emitted_any_jump = true;
        if (top->children[0].children[1].op != RtlOp::kIfThenElse) {
          emitted_uncond_jump_or_return = true;
        }
      }
    }
    int extra = 0;
    if (!emitted_uncond_jump_or_return && !emitted_any_jump && blk.successors.size() == 1 &&
        blk.successors[0] != b + 1 && blk.successors[0] < out->rpo_blocks.size()) {
      extra += 1;
      emitted_uncond_jump_or_return = true;
    }
    if (emitted_uncond_jump_or_return || blk.has_trailing_barrier) {
      extra += 1;
    }
    if (!blk.addr_vec_targets.empty()) {
      extra += 3;
    }
    return extra;
  };
  auto collect_dest_pseudos = [](const RtlExpr& pat, absl::flat_hash_set<int>* out_set) {
    auto mark_lhs = [&](const RtlExpr& lhs) {
      const RtlExpr* cur = &lhs;
      while (cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
             cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) {
        if (cur->children.empty()) {
          return;
        }
        cur = &cur->children[0];
      }
      if (cur->IsPseudoReg()) {
        out_set->insert(cur->RegNo());
      }
    };
    if ((pat.op == RtlOp::kSet || pat.op == RtlOp::kClobber) && !pat.children.empty()) {
      mark_lhs(pat.children[0]);
    } else if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& elem : pat.children) {
        if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
          mark_lhs(elem.children[0]);
        }
      }
    }
  };

  std::vector<bool> block_needs_label(all_parsed_blocks.size(), false);
  if (parsed_flags.opt_level == 0) {
    const size_t nblocks = all_parsed_blocks.size();
    for (size_t b = 0; b < nblocks; ++b) {
      const RtlBasicBlock& blk = out->rpo_blocks[b];
      if (blk.has_label) {
        block_needs_label[b] = true;
      }
      for (size_t succ : blk.addr_vec_targets) {
        if (succ < nblocks) {
          block_needs_label[succ] = true;
        }
      }
      bool ends_with_uncond = false;
      bool has_branch_label_ref = false;
      if (!all_parsed_blocks[b].empty()) {
        const RtlExpr& tail = all_parsed_blocks[b].back().pattern;
        const RtlExpr* top = &tail;
        if (tail.op == RtlOp::kSequence && !tail.children.empty() &&
            !tail.children[0].children.empty()) {
          top = &tail.children[0].children[0];
        }
        if (top->op == RtlOp::kReturn || (top->op == RtlOp::kParallel && !top->children.empty() &&
                                          top->children[0].op == RtlOp::kReturn)) {
          ends_with_uncond = true;
        } else if (top->op == RtlOp::kSet && top->children.size() == 2 &&
                   top->children[0].op == RtlOp::kPc) {
          if (top->children[1].op != RtlOp::kIfThenElse) {
            ends_with_uncond = true;
          }
          top->Visit([&](const RtlExpr& n) {
            if (n.op == RtlOp::kLabelRef) {
              has_branch_label_ref = true;
            }
          });
        } else if (top->op == RtlOp::kParallel && !top->children.empty() &&
                   top->children[0].op == RtlOp::kSet && top->children[0].children.size() == 2 &&
                   top->children[0].children[0].op == RtlOp::kPc) {
          if (top->children[0].children[1].op != RtlOp::kIfThenElse) {
            ends_with_uncond = true;
          }
          top->Visit([&](const RtlExpr& n) {
            if (n.op == RtlOp::kLabelRef) {
              has_branch_label_ref = true;
            }
          });
        }
      }
      if (has_branch_label_ref && blk.addr_vec_targets.empty() && !blk.successors.empty()) {
        size_t target_b = blk.successors[0];
        if (blk.successors.size() >= 2 && blk.successors[0] == b + 1) {
          target_b = blk.successors[1];
        }
        if (target_b < nblocks) {
          block_needs_label[target_b] = true;
        }
      }
      for (size_t succ : blk.successors) {
        if (succ < nblocks && (succ != b + 1 || ends_with_uncond)) {
          block_needs_label[succ] = true;
        }
      }
    }
  }

  absl::flat_hash_set<int> blocked_coalesce_pseudos;
  absl::flat_hash_map<int, int> stupid_max_suid;
  absl::flat_hash_map<int, absl::flat_hash_set<int>> stupid_set_suids;
  int build_suid = 0;
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    if (parsed_flags.opt_level == 0 && block_needs_label[b]) {
      ++build_suid;
    }
    for (size_t i = 0; i < all_parsed_blocks[b].size(); ++i) {
      const RtlPatternAndNotes& pn = all_parsed_blocks[b][i];
      bool insn_blocks_defs = false;
      for (const RtlRegNote& note : pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegLibcall ||
            (note.kind == RtlMachineMode::kRegRetval &&
             !(pn.pattern.IsSet() && pn.pattern.SetDest().IsPseudoReg() &&
               pn.pattern.SetDest().flags.volatil))) {
          insn_blocks_defs = true;
        }
        if (note.kind == RtlMachineMode::kRegNoConflict) {
          note.datum.Visit([&](const RtlExpr& n) {
            if (n.IsPseudoReg()) {
              blocked_coalesce_pseudos.insert(n.RegNo());
            }
          });
        }
      }
      if (insn_blocks_defs) {
        collect_dest_pseudos(pn.pattern, &blocked_coalesce_pseudos);
      }
      if (parsed_flags.opt_level == 0) {
        ++build_suid;
        if (is_non_note_suid_insn(pn)) {
          const int s = build_suid;
          pn.pattern.Visit([&](const RtlExpr& n) {
            if (n.IsPseudoReg()) {
              int& mx = stupid_max_suid[n.RegNo()];
              mx = std::max(mx, s);
            }
          });
          for (const RtlExpr& fu : pn.call_fusage) {
            fu.Visit([&](const RtlExpr& n) {
              if (n.IsPseudoReg()) {
                int& mx = stupid_max_suid[n.RegNo()];
                mx = std::max(mx, s);
              }
            });
          }
          auto mark_set_suid = [&](const RtlExpr& elem) {
            if (elem.op == RtlOp::kSet && !elem.children.empty()) {
              const RtlExpr& d = elem.children[0];
              if (d.IsPseudoReg()) {
                stupid_set_suids[d.RegNo()].insert(s);
              } else if (d.IsSubreg() && !d.children.empty() && d.children[0].IsPseudoReg()) {
                stupid_set_suids[d.children[0].RegNo()].insert(s);
              }
            }
          };
          if (pn.pattern.op == RtlOp::kSet) {
            mark_set_suid(pn.pattern);
          } else if (pn.pattern.op == RtlOp::kParallel) {
            for (const RtlExpr& elem : pn.pattern.children) {
              mark_set_suid(elem);
            }
          }
        }
      }
    }
    if (parsed_flags.opt_level == 0) {
      build_suid += block_trailing_extra_suids(b);
    }
  }

  absl::flat_hash_map<int, int> pseudo_only_block;
  absl::flat_hash_map<int, RtlMachineMode> pseudo_mode;
  absl::flat_hash_set<int> global_pseudos;
  absl::flat_hash_set<int> full_defined_in_block;
  absl::flat_hash_map<int, int> pseudo_def_count;
  absl::flat_hash_map<int, int> pseudo_read_count;
  absl::flat_hash_set<int> high_part_const_pseudos;
  absl::flat_hash_set<int> const_init_pseudos;
  absl::flat_hash_set<int> and_def_pseudos;
  absl::flat_hash_set<int> mem_init_pseudos;
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    full_defined_in_block.clear();
    for (const RtlPatternAndNotes& pn : all_parsed_blocks[b]) {
      auto note_block = [&](int r, RtlMachineMode mode) {
        pseudo_mode.try_emplace(r, mode);
        auto [it, ins] = pseudo_only_block.try_emplace(r, static_cast<int>(b));
        if (!ins && it->second != static_cast<int>(b)) {
          global_pseudos.insert(r);
        }
      };
      auto scan_reads = [&](const RtlExpr& expr) {
        expr.Visit([&](const RtlExpr& n) {
          if (n.IsPseudoReg()) {
            const int r = n.RegNo();
            note_block(r, n.mode);
            ++pseudo_read_count[r];
            if (!full_defined_in_block.contains(r)) {
              global_pseudos.insert(r);
            }
          }
        });
      };
      auto scan_elem = [&](const RtlExpr& elem) {
        if (elem.op == RtlOp::kClobber) {
          if (!elem.children.empty() && elem.children[0].IsPseudoReg()) {
            const int r = elem.children[0].RegNo();
            note_block(r, elem.children[0].mode);
            ++pseudo_def_count[r];
            full_defined_in_block.insert(r);
          } else {
            scan_reads(elem);
          }
          return;
        }
        if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
          const RtlExpr& d = elem.children[0];
          if (d.IsPseudoReg()) {
            for (size_t k = 1; k < elem.children.size(); ++k) {
              scan_reads(elem.children[k]);
            }
            const int r = d.RegNo();
            note_block(r, d.mode);
            ++pseudo_def_count[r];
            full_defined_in_block.insert(r);
            const RtlExpr& s = elem.children[1];
            if ((s.op == RtlOp::kConstInt && s.int_val != 0 && (s.int_val & 0xffff) == 0) ||
                s.op == RtlOp::kHigh) {
              high_part_const_pseudos.insert(r);
            }
            if (pseudo_def_count[r] == 1 &&
                (s.op == RtlOp::kConstInt || s.op == RtlOp::kConstDouble)) {
              const_init_pseudos.insert(r);
            }
            if (s.op == RtlOp::kAnd) {
              and_def_pseudos.insert(r);
            }
            if (pseudo_def_count[r] == 1 && s.op == RtlOp::kMem) {
              mem_init_pseudos.insert(r);
            }
            return;
          }
          scan_reads(d);
          for (size_t k = 1; k < elem.children.size(); ++k) {
            scan_reads(elem.children[k]);
          }
          return;
        }
        scan_reads(elem);
      };
      if (pn.pattern.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : pn.pattern.children) {
          scan_elem(elem);
        }
      } else {
        scan_elem(pn.pattern);
      }
      for (const RtlExpr& fu : pn.call_fusage) {
        scan_reads(fu);
      }
    }
  }

  absl::flat_hash_map<int, int> rmw_parent;
  auto find_rmw = [&](int r) {
    int cur = r;
    while (true) {
      auto it = rmw_parent.find(cur);
      if (it == rmw_parent.end() || it->second == cur) {
        return cur;
      }
      cur = it->second;
    }
  };
  auto union_rmw = [&](int a, int b) {
    const int ra = find_rmw(a);
    const int rb = find_rmw(b);
    if (ra == rb) {
      return false;
    }
    const int keep = std::min(ra, rb);
    const int drop = std::max(ra, rb);
    rmw_parent[drop] = keep;
    rmw_parent[keep] = keep;
    if (global_pseudos.contains(drop)) {
      global_pseudos.insert(keep);
    }
    pseudo_def_count[keep] += pseudo_def_count[drop];
    pseudo_read_count[keep] += pseudo_read_count[drop];
    if (!const_init_pseudos.contains(drop)) {
      const_init_pseudos.erase(keep);
    }
    if (!high_part_const_pseudos.contains(drop)) {
      high_part_const_pseudos.erase(keep);
    }
    if (parsed_flags.opt_level == 0) {
      stupid_max_suid[keep] = std::max(stupid_max_suid[keep], stupid_max_suid[drop]);
      const auto drop_sets_it = stupid_set_suids.find(drop);
      if (drop_sets_it != stupid_set_suids.end()) {
        stupid_set_suids[keep].insert(drop_sets_it->second.begin(), drop_sets_it->second.end());
      }
    }
    return true;
  };
  auto insn_has_dead_note_for_root = [&](const RtlPatternAndNotes& insn, int root_r) {
    for (const RtlRegNote& note : insn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.IsPseudoReg() &&
          find_rmw(note.datum.RegNo()) == root_r) {
        return true;
      }
    }
    return false;
  };

  bool any_rmw_coalesced = false;
  int scan_suid = 0;
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    if (parsed_flags.opt_level == 0 && block_needs_label[b]) {
      ++scan_suid;
    }
    for (size_t i = 0; i < all_parsed_blocks[b].size(); ++i) {
      const RtlPatternAndNotes& pn = all_parsed_blocks[b][i];
      const int cur_suid = (parsed_flags.opt_level == 0) ? (++scan_suid) : 0;
      const bool counts_suid = parsed_flags.opt_level == 0 && is_non_note_suid_insn(pn);
      if (ContainsCallOp(pn.pattern)) {
        continue;
      }
      const RtlExpr* single_set = nullptr;
      if (pn.pattern.op == RtlOp::kSet && pn.pattern.children.size() == 2) {
        single_set = &pn.pattern;
      } else if (pn.pattern.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : pn.pattern.children) {
          if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
            if (single_set != nullptr) {
              single_set = nullptr;
              break;
            }
            single_set = &elem;
          } else if (elem.op != RtlOp::kClobber && elem.op != RtlOp::kUse) {
            single_set = nullptr;
            break;
          }
        }
      }
      if (single_set == nullptr) {
        continue;
      }
      const RtlExpr& dst = single_set->children[0];
      if (!dst.IsPseudoReg() || blocked_coalesce_pseudos.contains(dst.RegNo())) {
        continue;
      }
      const int p_w = dst.RegNo();
      auto w_ren_it = out->sideband.reg_renumber.find(p_w);
      if (w_ren_it == out->sideband.reg_renumber.end() || w_ren_it->second < 0) {
        continue;
      }
      const int hr = w_ren_it->second;

      auto collect_matching_reads = [&](const RtlExpr& expr, std::vector<int>* cands) {
        auto walk = [&](auto& self, const RtlExpr& n, bool inside_mem) -> void {
          if (n.op == RtlOp::kMem) {
            for (const RtlExpr& ch : n.children) {
              self(self, ch, /*inside_mem=*/true);
            }
            return;
          }
          if (!n.IsPseudoReg()) {
            for (const RtlExpr& ch : n.children) {
              self(self, ch, inside_mem);
            }
            return;
          }
          if (blocked_coalesce_pseudos.contains(n.RegNo())) {
            return;
          }
          const int p_r = n.RegNo();
          const int root_r = find_rmw(p_r);
          const int root_w = find_rmw(p_w);
          if (root_r == root_w) {
            return;
          }
          auto r_ren_it = out->sideband.reg_renumber.find(p_r);
          if (r_ren_it == out->sideband.reg_renumber.end() || r_ren_it->second != hr) {
            return;
          }
          if (n.mode != dst.mode || n.raw_mode != dst.raw_mode || n.flags != dst.flags) {
            return;
          }
          if (insn_has_dead_note_for_root(pn, root_r)) {
            return;
          }
          if (inside_mem ||
              (pn.pattern.op == RtlOp::kSet && single_set->children[1].IsPseudoReg())) {
            return;
          }
          if (parsed_flags.opt_level == 0) {
            if (!counts_suid) {
              return;
            }
            const int max_s = stupid_max_suid[root_r];
            const auto sets_it = stupid_set_suids.find(root_r);
            const bool born_at_prev_insn =
                sets_it != stupid_set_suids.end() && sets_it->second.contains(cur_suid - 1);
            if (max_s > cur_suid || born_at_prev_insn) {
              cands->push_back(p_r);
            }
            return;
          }
          const bool r_global = global_pseudos.contains(root_r);
          const bool w_global = global_pseudos.contains(root_w);
          if (r_global != w_global && !dst.flags.volatil) {
            const RtlOp rhs_op = single_set->children[1].op;
            const bool is_single_def_rmw_init =
                !r_global && w_global && pseudo_def_count[p_r] == 1 &&
                ((b == 0 && pseudo_def_count[root_r] == 1 && pseudo_read_count[root_r] == 1 &&
                  const_init_pseudos.contains(root_r)) ||
                 (pseudo_def_count[root_r] == 1 && high_part_const_pseudos.contains(root_r) &&
                  (rhs_op == RtlOp::kIor || rhs_op == RtlOp::kPlus || rhs_op == RtlOp::kLoSum)) ||
                 (and_def_pseudos.contains(p_r) && rhs_op == RtlOp::kIor &&
                  (pseudo_def_count[root_r] == 1 || mem_init_pseudos.contains(root_r))));
            bool is_global_rmw_tail = false;
            if (!is_single_def_rmw_init && r_global && !w_global) {
              const int dst_span = std::max(1, MipsHardRegNregs(hr, dst.mode, gp64, fp64));
              bool r_used_later_or_live_out = false;
              for (size_t succ : out->rpo_blocks[b].successors) {
                const auto lit =
                    live_probe.sideband.basic_block_live_at_start.find(static_cast<int>(succ));
                if (lit == live_probe.sideband.basic_block_live_at_start.end()) {
                  continue;
                }
                for (int lreg : lit->second) {
                  if (lreg >= kFirstPseudoRegister && find_rmw(lreg) == root_r) {
                    r_used_later_or_live_out = true;
                    break;
                  }
                  int ehr = -1;
                  int lspan = 1;
                  if (lreg >= 0 && lreg < kFirstPseudoRegister) {
                    ehr = lreg;
                  } else if (lreg >= kFirstPseudoRegister) {
                    const auto ren_it = out->sideband.reg_renumber.find(lreg);
                    if (ren_it != out->sideband.reg_renumber.end() && ren_it->second >= 0) {
                      ehr = ren_it->second;
                      const auto mode_it = pseudo_mode.find(lreg);
                      const RtlMachineMode lmode =
                          (mode_it != pseudo_mode.end()) ? mode_it->second : RtlMachineMode::kSI;
                      lspan = std::max(1, MipsHardRegNregs(ehr, lmode, gp64, fp64));
                    }
                  }
                  if (ehr >= 0 && ehr < hr + dst_span && hr < ehr + lspan) {
                    r_used_later_or_live_out = true;
                    break;
                  }
                }
                if (r_used_later_or_live_out) {
                  break;
                }
              }
              size_t last_w_idx = i;
              for (size_t j = i + 1; !r_used_later_or_live_out && j < all_parsed_blocks[b].size();
                   ++j) {
                all_parsed_blocks[b][j].pattern.Visit([&](const RtlExpr& m) {
                  if (m.IsPseudoReg()) {
                    const int mr = find_rmw(m.RegNo());
                    if (mr == root_r) {
                      r_used_later_or_live_out = true;
                    } else if (mr == root_w) {
                      last_w_idx = j;
                    }
                  }
                });
              }
              bool w_crosses_call = false;
              for (size_t j = i + 1; !r_used_later_or_live_out && j <= last_w_idx; ++j) {
                if (ContainsCallOp(all_parsed_blocks[b][j].pattern)) {
                  w_crosses_call = true;
                  break;
                }
              }
              is_global_rmw_tail = !r_used_later_or_live_out && !w_crosses_call;
            }
            if (!is_single_def_rmw_init && !is_global_rmw_tail) {
              return;
            }
          }
          cands->push_back(p_r);
        };
        walk(walk, expr, /*inside_mem=*/false);
      };

      std::vector<int> read_cands;
      collect_matching_reads(single_set->children[1], &read_cands);
      if (pn.pattern.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : pn.pattern.children) {
          if (elem.op == RtlOp::kUse && !elem.children.empty()) {
            collect_matching_reads(elem.children[0], &read_cands);
          }
        }
      }
      for (int p_r : read_cands) {
        if (union_rmw(p_r, p_w)) {
          any_rmw_coalesced = true;
        }
      }
    }
    if (parsed_flags.opt_level == 0) {
      scan_suid += block_trailing_extra_suids(b);
    }
  }

  if (!any_rmw_coalesced) {
    return;
  }
  auto rewrite_rmw_reg = [&](RtlExpr& reg) {
    if (reg.IsPseudoReg()) {
      const int root = find_rmw(reg.RegNo());
      if (root != reg.RegNo()) {
        reg.SetRegNo(root);
      }
    }
  };
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    RtlBasicBlock& blk = out->rpo_blocks[b];
    bool blk_changed = false;
    for (size_t i = 0; i < all_parsed_blocks[b].size(); ++i) {
      const std::string before = blk.instructions[i];
      all_parsed_blocks[b][i].RewriteRegs(rewrite_rmw_reg);
      const RtlPatternAndNotes& pn = all_parsed_blocks[b][i];
      const std::string after = FormatGccPrintRtlPatternAndNotes(
          pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{});
      if (after != before) {
        blk.instructions[i] = after;
        blk_changed = true;
      }
    }
    if (blk_changed) {
      blk.SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < blk.rtl_insns.size() && i < all_parsed_blocks[b].size(); ++i) {
        if (all_parsed_blocks[b][i].has_explicit_call_fusage &&
            !blk.rtl_insns[i].has_explicit_call_fusage) {
          blk.rtl_insns[i].has_explicit_call_fusage = true;
          blk.rtl_insns[i].call_fusage = all_parsed_blocks[b][i].call_fusage;
        }
      }
    }
  }
  if (out->sideband.epilogue_delay_pattern.has_value() &&
      !out->sideband.epilogue_delay_pattern->empty()) {
    if (auto ep = ParsePatternAndNotes(*out->sideband.epilogue_delay_pattern); ep.ok()) {
      ep->RewriteRegs(rewrite_rmw_reg);
      out->sideband.epilogue_delay_pattern = ep->ToGccPrintRtlString();
    }
  }
  for (const auto& [regno, _] : rmw_parent) {
    const int root = find_rmw(regno);
    if (root != regno) {
      out->sideband.reg_renumber.erase(regno);
    }
  }
}

void BuildDisjointMultiBlockCandidateConflicts(
    const RtlControlFlowGraph& out, const RtlControlFlowGraph& live_probe,
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks,
    const absl::flat_hash_set<int>& cand_pseudos,
    absl::flat_hash_map<int, absl::flat_hash_set<int>>* conflict_hrs,
    absl::flat_hash_map<int, absl::flat_hash_set<int>>* conflict_pseudos) {
  auto eff_hr_of = [&](int regno) -> int {
    if (regno >= 0 && regno < kFirstPseudoRegister) {
      return regno;
    }
    const auto it = out.sideband.reg_renumber.find(regno);
    return (it != out.sideband.reg_renumber.end()) ? it->second : -1;
  };
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    absl::flat_hash_set<int> cur_live;
    for (size_t sb : out.rpo_blocks[b].successors) {
      const auto it = live_probe.sideband.basic_block_live_at_start.find(static_cast<int>(sb));
      if (it != live_probe.sideband.basic_block_live_at_start.end()) {
        cur_live.insert(it->second.begin(), it->second.end());
      }
    }
    auto record_live_point = [&]() {
      for (int p : cand_pseudos) {
        if (!cur_live.contains(p)) {
          continue;
        }
        for (int other : cur_live) {
          if (other == p) {
            continue;
          }
          if (other >= kFirstPseudoRegister) {
            (*conflict_pseudos)[p].insert(other);
          }
          const int ohr = eff_hr_of(other);
          if (ohr >= 2 && ohr <= 15) {
            (*conflict_hrs)[p].insert(ohr);
          }
        }
      }
    };
    record_live_point();
    for (size_t rev = all_parsed_blocks[b].size(); rev > 0; --rev) {
      const RtlPatternAndNotes& pn = all_parsed_blocks[b][rev - 1];
      if (pn.pattern.op == RtlOp::kNote) {
        continue;
      }
      absl::flat_hash_set<int> defs;
      absl::flat_hash_set<int> uses;
      auto collect_elem = [&](const RtlExpr& elem) {
        if (elem.IsSet()) {
          if (elem.SetDest().IsReg()) {
            defs.insert(elem.SetDest().RegNo());
          } else {
            elem.SetDest().Visit([&](const RtlExpr& n) {
              if (n.IsReg()) {
                uses.insert(n.RegNo());
              }
            });
          }
          elem.SetSrc().Visit([&](const RtlExpr& n) {
            if (n.IsReg()) {
              uses.insert(n.RegNo());
            }
          });
        } else if (elem.op == RtlOp::kClobber && !elem.children.empty() &&
                   elem.children[0].IsReg()) {
          defs.insert(elem.children[0].RegNo());
        } else {
          elem.Visit([&](const RtlExpr& n) {
            if (n.IsReg()) {
              uses.insert(n.RegNo());
            }
          });
        }
      };
      if (pn.pattern.op == RtlOp::kParallel) {
        for (const RtlExpr& ch : pn.pattern.children) {
          collect_elem(ch);
        }
      } else {
        collect_elem(pn.pattern);
      }
      for (int d : defs) {
        if (cand_pseudos.contains(d)) {
          for (int other : cur_live) {
            if (other == d) {
              continue;
            }
            if (other >= kFirstPseudoRegister) {
              (*conflict_pseudos)[d].insert(other);
            }
            const int ohr = eff_hr_of(other);
            if (ohr >= 2 && ohr <= 15) {
              (*conflict_hrs)[d].insert(ohr);
            }
          }
        }
        const int dhr = eff_hr_of(d);
        if (dhr >= 2 && dhr <= 15) {
          for (int p : cand_pseudos) {
            if (p != d && cur_live.contains(p)) {
              (*conflict_hrs)[p].insert(dhr);
              if (d >= kFirstPseudoRegister) {
                (*conflict_pseudos)[p].insert(d);
              }
            }
          }
        }
      }
      for (int d : defs) {
        cur_live.erase(d);
      }
      cur_live.insert(uses.begin(), uses.end());
      record_live_point();
    }
  }
}

void CoalesceDisjointMultiBlockSameRegPseudos(
    const CompilationProfile& profile, const ParsedCompilerFlags& parsed_flags,
    RtlControlFlowGraph* out, std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr) {
  if (parsed_flags.opt_level == 0) {
    return;
  }
  std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks = *all_parsed_blocks_ptr;
  RtlControlFlowGraph live_probe = *out;
  ReconstructRtlLiveness(&live_probe, profile,
                         RtlLivenessOptions{
                             .reload_completed = false,
                             .update_reg_notes = false,
                         });

  absl::flat_hash_map<int, RtlMachineMode> pseudo_widest_mode;
  absl::flat_hash_set<int> multi_block_pseudos;
  for (const auto& [blk_idx, live_set] : live_probe.sideband.basic_block_live_at_start) {
    (void)blk_idx;
    for (int regno : live_set) {
      if (regno >= kFirstPseudoRegister) {
        multi_block_pseudos.insert(regno);
      }
    }
  }
  absl::flat_hash_map<int, size_t> first_ref_block;
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    for (const RtlPatternAndNotes& pn : all_parsed_blocks[b]) {
      if (pn.pattern.op == RtlOp::kNote) {
        continue;
      }
      pn.pattern.Visit([&](const RtlExpr& n) {
        if (n.IsPseudoReg()) {
          const int p = n.RegNo();
          const auto [it, inserted] = first_ref_block.try_emplace(p, b);
          if (!inserted && it->second != b) {
            multi_block_pseudos.insert(p);
          }
          auto m_it = pseudo_widest_mode.find(p);
          if (m_it == pseudo_widest_mode.end() ||
              MipsModeByteSize(n.mode) > MipsModeByteSize(m_it->second)) {
            pseudo_widest_mode[p] = n.mode;
          }
        }
      });
    }
  }

  absl::flat_hash_set<int> cand_pseudos;
  for (const auto& [p, hr] : out->sideband.reg_renumber) {
    if (p < kFirstPseudoRegister || hr < 3 || hr > 15 || !multi_block_pseudos.contains(p)) {
      continue;
    }
    const auto mode_it = pseudo_widest_mode.find(p);
    if (mode_it != pseudo_widest_mode.end() && mode_it->second != RtlMachineMode::kSI) {
      continue;
    }
    const auto calls_it = live_probe.sideband.reg_n_calls_crossed.find(p);
    if (calls_it != live_probe.sideband.reg_n_calls_crossed.end() && calls_it->second > 0) {
      continue;
    }
    cand_pseudos.insert(p);
  }
  if (cand_pseudos.size() < 2) {
    return;
  }

  absl::flat_hash_set<int> has_hr_pref_or_equiv;
  for (const auto& blk_insns : all_parsed_blocks) {
    for (const RtlPatternAndNotes& pn : blk_insns) {
      for (const RtlRegNote& n : pn.reg_notes) {
        if (n.kind == RtlMachineMode::kRegEquiv && pn.pattern.IsSet() &&
            pn.pattern.SetDest().IsPseudoReg()) {
          has_hr_pref_or_equiv.insert(pn.pattern.SetDest().RegNo());
        }
      }
      if (!pn.pattern.IsSet()) {
        continue;
      }
      const RtlExpr& dst = pn.pattern.SetDest();
      const RtlExpr& src = pn.pattern.SetSrc();
      if (dst.IsPseudoReg() && cand_pseudos.contains(dst.RegNo())) {
        const int p = dst.RegNo();
        const int hr = out->sideband.reg_renumber.at(p);
        src.Visit([&](const RtlExpr& n) {
          if (n.IsHardReg() && !n.IsArgPointerReg() && n.RegNo() == hr) {
            has_hr_pref_or_equiv.insert(p);
          } else if (n.IsPseudoReg() && n.RegNo() != p) {
            const auto oit = out->sideband.reg_renumber.find(n.RegNo());
            if (oit != out->sideband.reg_renumber.end() && oit->second == hr) {
              has_hr_pref_or_equiv.insert(p);
              has_hr_pref_or_equiv.insert(n.RegNo());
            }
          }
        });
      } else if (dst.IsHardReg() && src.IsPseudoReg() && cand_pseudos.contains(src.RegNo()) &&
                 dst.RegNo() == out->sideband.reg_renumber.at(src.RegNo())) {
        has_hr_pref_or_equiv.insert(src.RegNo());
      }
    }
  }
  for (int bad_p : has_hr_pref_or_equiv) {
    cand_pseudos.erase(bad_p);
  }
  if (cand_pseudos.size() < 2) {
    return;
  }

  absl::flat_hash_map<int, absl::flat_hash_set<int>> conflict_hrs;
  absl::flat_hash_map<int, absl::flat_hash_set<int>> conflict_pseudos;
  BuildDisjointMultiBlockCandidateConflicts(*out, live_probe, all_parsed_blocks, cand_pseudos,
                                            &conflict_hrs, &conflict_pseudos);

  std::vector<int> sorted_cands(cand_pseudos.begin(), cand_pseudos.end());
  std::sort(sorted_cands.begin(), sorted_cands.end());
  absl::flat_hash_map<int, int> remap;
  for (size_t i = 0; i < sorted_cands.size(); ++i) {
    const int p1 = sorted_cands[i];
    if (remap.contains(p1)) {
      continue;
    }
    const int hr = out->sideband.reg_renumber.at(p1);
    if (conflict_hrs[p1].contains(hr)) {
      continue;
    }
    for (size_t j = i + 1; j < sorted_cands.size(); ++j) {
      const int p2 = sorted_cands[j];
      if (remap.contains(p2) || out->sideband.reg_renumber.at(p2) != hr ||
          conflict_pseudos[p1].contains(p2) || conflict_hrs[p2].contains(hr)) {
        continue;
      }
      bool complementary_lower_hr = false;
      for (int h = 2; h < hr; ++h) {
        if (conflict_hrs[p1].contains(h) != conflict_hrs[p2].contains(h)) {
          complementary_lower_hr = true;
          break;
        }
      }
      bool all_lower_covered = true;
      for (int h = 2; h < hr; ++h) {
        if (!conflict_hrs[p1].contains(h) && !conflict_hrs[p2].contains(h)) {
          all_lower_covered = false;
          break;
        }
      }
      if (complementary_lower_hr && all_lower_covered) {
        remap[p2] = p1;
        conflict_hrs[p1].insert(conflict_hrs[p2].begin(), conflict_hrs[p2].end());
        conflict_pseudos[p1].insert(conflict_pseudos[p2].begin(), conflict_pseudos[p2].end());
      }
    }
  }
  if (remap.empty()) {
    return;
  }

  auto rewrite_fn = [&](RtlExpr& reg) {
    if (reg.IsPseudoReg()) {
      const auto it = remap.find(reg.RegNo());
      if (it != remap.end()) {
        reg.SetRegNo(it->second);
      }
    }
  };
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    RtlBasicBlock& blk = out->rpo_blocks[b];
    bool blk_changed = false;
    for (size_t i = 0; i < all_parsed_blocks[b].size(); ++i) {
      const std::string before = blk.instructions[i];
      all_parsed_blocks[b][i].RewriteRegs(rewrite_fn);
      const RtlPatternAndNotes& pn = all_parsed_blocks[b][i];
      const std::string after = FormatGccPrintRtlPatternAndNotes(
          pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{});
      if (after != before) {
        blk.instructions[i] = after;
        blk_changed = true;
      }
    }
    if (blk_changed) {
      blk.SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < blk.rtl_insns.size() && i < all_parsed_blocks[b].size(); ++i) {
        if (all_parsed_blocks[b][i].has_explicit_call_fusage &&
            !blk.rtl_insns[i].has_explicit_call_fusage) {
          blk.rtl_insns[i].has_explicit_call_fusage = true;
          blk.rtl_insns[i].call_fusage = all_parsed_blocks[b][i].call_fusage;
        }
      }
    }
  }
  if (out->sideband.epilogue_delay_pattern.has_value() &&
      !out->sideband.epilogue_delay_pattern->empty()) {
    if (auto ep = ParsePatternAndNotes(*out->sideband.epilogue_delay_pattern); ep.ok()) {
      ep->RewriteRegs(rewrite_fn);
      out->sideband.epilogue_delay_pattern = ep->ToGccPrintRtlString();
    }
  }
  for (const auto& [drop_p, _] : remap) {
    out->sideband.reg_renumber.erase(drop_p);
  }
}

bool FirstRtlFormatCharIsExprForPref(RtlOp op) {
  switch (op) {
    case RtlOp::kUnknown:
    case RtlOp::kNil:
    case RtlOp::kPc:
    case RtlOp::kCc0:
    case RtlOp::kReturn:
    case RtlOp::kParallel:
    case RtlOp::kSequence:
    case RtlOp::kCond:
    case RtlOp::kInsn:
    case RtlOp::kJumpInsn:
    case RtlOp::kCallInsn:
    case RtlOp::kCodeLabel:
    case RtlOp::kBarrier:
    case RtlOp::kNote:
    case RtlOp::kAddrVec:
    case RtlOp::kAsmInput:
    case RtlOp::kAsmOperands:
    case RtlOp::kUnspec:
    case RtlOp::kUnspecVolatile:
    case RtlOp::kReg:
    case RtlOp::kScratch:
    case RtlOp::kConstInt:
    case RtlOp::kConstDouble:
    case RtlOp::kConstString:
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
      return false;
    default:
      return true;
  }
}

void RecordFalsePrefSetPreference(const RtlExpr& dest, const RtlExpr& orig_src,
                                  const absl::flat_hash_set<int>& is_global_pseudo,
                                  const absl::flat_hash_map<int, int>& reg_renumber,
                                  absl::flat_hash_map<int, uint64_t>* base_hr_prefs) {
  const RtlExpr* src = &orig_src;
  int offset = 0;
  if (FirstRtlFormatCharIsExprForPref(src->op) && !src->children.empty()) {
    src = &src->children[0];
  }
  int src_regno = -1;
  if (src->op == RtlOp::kReg && !src->IsArgPointerReg()) {
    src_regno = src->RegNo();
  } else if (src->op == RtlOp::kSubreg && !src->children.empty() &&
             src->children[0].op == RtlOp::kReg && !src->children[0].IsArgPointerReg()) {
    src_regno = src->children[0].RegNo();
    offset += static_cast<int>(src->int_val);
  } else {
    return;
  }
  int dest_regno = -1;
  if (dest.op == RtlOp::kReg && !dest.IsArgPointerReg()) {
    dest_regno = dest.RegNo();
  } else if (dest.op == RtlOp::kSubreg && !dest.children.empty() &&
             dest.children[0].op == RtlOp::kReg && !dest.children[0].IsArgPointerReg()) {
    dest_regno = dest.children[0].RegNo();
    offset -= static_cast<int>(dest.int_val);
  } else {
    return;
  }
  auto resolve_local = [&](int r) {
    if (r >= kFirstPseudoRegister && !is_global_pseudo.contains(r)) {
      const auto it = reg_renumber.find(r);
      if (it != reg_renumber.end() && it->second >= 0) {
        return it->second;
      }
    }
    return r;
  };
  src_regno = resolve_local(src_regno);
  dest_regno = resolve_local(dest_regno);
  if (dest_regno >= 0 && dest_regno < 64 && is_global_pseudo.contains(src_regno)) {
    const int adj = dest_regno - offset;
    if (adj >= 0 && adj < 64) {
      (*base_hr_prefs)[src_regno] |= (uint64_t{1} << adj);
    }
  }
  if (src_regno >= 0 && src_regno < 64 && is_global_pseudo.contains(dest_regno)) {
    const int adj = src_regno + offset;
    if (adj >= 0 && adj < 64) {
      (*base_hr_prefs)[dest_regno] |= (uint64_t{1} << adj);
    }
  }
}

// When a single-block `local_alloc` web on `h_pref` was split across blocks by
// delay-slot filling, lifting it to an unallocated `global_alloc` pseudo `q`
// removes `h_pref` from `hard_regs_live` while a conflicting pseudo `p_other`
// (`reg_renumber == h_other != h_pref`) holds an unpruned `set_preference` for
// `h_pref`. Demote `q` back to `h_pref` whenever `global_alloc` would otherwise
// be mathematically unable to assign `(p_other -> h_other, q -> h_pref)`.
void DemoteFalsePreferenceConflictPseudos(
    const CompilationProfile& profile, const ParsedCompilerFlags& parsed_flags, bool gp64,
    bool fp64, RtlControlFlowGraph* out,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr) {
  if (parsed_flags.opt_level == 0) {
    return;
  }
  auto& all_parsed_blocks = *all_parsed_blocks_ptr;
  RtlControlFlowGraph live_probe = *out;
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    RtlBasicBlock& blk = live_probe.rpo_blocks[b];
    for (size_t i = 0; i < all_parsed_blocks[b].size(); ++i) {
      const RtlPatternAndNotes& pn = all_parsed_blocks[b][i];
      blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(
          pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{});
    }
    blk.SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < blk.rtl_insns.size() && i < all_parsed_blocks[b].size(); ++i) {
      if (all_parsed_blocks[b][i].has_explicit_call_fusage &&
          !blk.rtl_insns[i].has_explicit_call_fusage) {
        blk.rtl_insns[i].has_explicit_call_fusage = true;
        blk.rtl_insns[i].call_fusage = all_parsed_blocks[b][i].call_fusage;
      }
    }
  }
  ReconstructRtlLiveness(&live_probe, profile,
                         RtlLivenessOptions{
                             .reload_completed = false,
                             .update_reg_notes = true,
                             .include_call_reg_notes = true,
                         });

  struct CandSummary {
    absl::flat_hash_set<size_t> blocks_ref;
    absl::flat_hash_set<size_t> def_blocks;
    absl::flat_hash_set<size_t> read_blocks;
    bool has_set = false;
    bool used_before_set = false;
    bool alive_after_redef = false;
    bool has_flags = false;
    int note_deaths = 0;
    RtlMachineMode widest_mode = RtlMachineMode::kSI;
  };
  absl::flat_hash_map<int, CandSummary> summaries;
  std::vector<std::vector<RtlPatternAndNotes>> live_blocks(live_probe.rpo_blocks.size());
  for (size_t b = 0; b < live_probe.rpo_blocks.size(); ++b) {
    absl::flat_hash_set<int> defined_so_far;
    absl::flat_hash_set<int> died_in_blk;
    live_blocks[b].reserve(live_probe.rpo_blocks[b].instructions.size());
    for (const std::string& insn_str : live_probe.rpo_blocks[b].instructions) {
      auto pn = ParsePatternAndNotes(insn_str);
      if (!pn.ok()) {
        continue;
      }
      absl::flat_hash_set<int> insn_defs;
      absl::flat_hash_set<int> insn_reads;
      auto scan_pat = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
        if (e.op == RtlOp::kSet && e.children.size() >= 2) {
          self(self, e.children[0], true);
          for (size_t k = 1; k < e.children.size(); ++k) {
            self(self, e.children[k], false);
          }
          return;
        }
        if (e.op == RtlOp::kClobber) {
          if (!e.children.empty() && e.children[0].op == RtlOp::kMem) {
            self(self, e.children[0], false);
          } else if (!e.children.empty()) {
            e.children[0].Visit([&](const RtlExpr& r) {
              if (r.IsPseudoReg()) {
                insn_defs.insert(r.RegNo());
              }
            });
          }
          return;
        }
        if (in_dst && e.op == RtlOp::kSubreg && !e.children.empty() &&
            e.children[0].IsPseudoReg()) {
          insn_defs.insert(e.children[0].RegNo());
          return;
        }
        if (in_dst && e.op == RtlOp::kMem) {
          for (const RtlExpr& ch : e.children) {
            self(self, ch, false);
          }
          return;
        }
        if (e.IsPseudoReg()) {
          CandSummary& s = summaries[e.RegNo()];
          if (MipsModeByteSize(e.mode) > MipsModeByteSize(s.widest_mode)) {
            s.widest_mode = e.mode;
          }
          if (e.flags.Any()) {
            s.has_flags = true;
          }
          if (in_dst) {
            insn_defs.insert(e.RegNo());
          } else {
            insn_reads.insert(e.RegNo());
          }
          return;
        }
        for (const RtlExpr& ch : e.children) {
          self(self, ch, false);
        }
      };
      scan_pat(scan_pat, pn->pattern, false);
      for (int r : insn_reads) {
        CandSummary& s = summaries[r];
        s.blocks_ref.insert(b);
        s.read_blocks.insert(b);
        if (!defined_so_far.contains(r)) {
          s.used_before_set = true;
        }
      }
      for (int d : insn_defs) {
        CandSummary& s = summaries[d];
        s.blocks_ref.insert(b);
        s.def_blocks.insert(b);
        s.has_set = true;
        defined_so_far.insert(d);
        if (died_in_blk.contains(d)) {
          s.alive_after_redef = true;
        }
      }
      for (const RtlRegNote& note : pn->reg_notes) {
        if ((note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) &&
            note.datum.IsPseudoReg()) {
          const int dp = note.datum.RegNo();
          summaries[dp].blocks_ref.insert(b);
          ++summaries[dp].note_deaths;
          died_in_blk.insert(dp);
          summaries[dp].alive_after_redef = false;
        }
      }
      live_blocks[b].push_back(*std::move(pn));
    }
  }

  absl::flat_hash_set<int> live_at_any_start;
  absl::flat_hash_set<int> live_at_b0_start;
  for (const auto& [bb_id, lset] : live_probe.sideband.basic_block_live_at_start) {
    for (int r : lset) {
      if (r >= kFirstPseudoRegister) {
        live_at_any_start.insert(r);
        if (bb_id == 0) {
          live_at_b0_start.insert(r);
        }
      }
    }
  }

  absl::flat_hash_set<int> is_global_pseudo;
  for (const auto& [p, hr] : out->sideband.reg_renumber) {
    const auto d_it = live_probe.sideband.reg_n_deaths.find(p);
    const auto s_it = summaries.find(p);
    if (p < kFirstPseudoRegister || hr < 0 || hr == kFramePointerRegno || hr >= 64 ||
        live_at_any_start.contains(p) || d_it == live_probe.sideband.reg_n_deaths.end() ||
        d_it->second != 1 || s_it == summaries.end() || s_it->second.blocks_ref.size() != 1 ||
        !s_it->second.has_set || s_it->second.used_before_set || s_it->second.alive_after_redef ||
        s_it->second.note_deaths >= 2) {
      is_global_pseudo.insert(p);
    }
  }
  if (is_global_pseudo.size() < 2) {
    return;
  }

  absl::flat_hash_map<int, uint64_t> base_hr_conflicts;
  absl::flat_hash_map<int, uint64_t> base_hr_prefs;
  absl::flat_hash_map<int, absl::flat_hash_set<int>> pseudo_conflicts;
  auto resolve_reg = [&](int r) {
    if (r >= kFirstPseudoRegister && !is_global_pseudo.contains(r)) {
      const auto it = out->sideband.reg_renumber.find(r);
      return (it != out->sideband.reg_renumber.end()) ? it->second : -1;
    }
    return r;
  };

  for (size_t b = 0; b < live_blocks.size(); ++b) {
    uint64_t hard_live = 0;
    absl::flat_hash_set<int> global_live;
    if (const auto lit = live_probe.sideband.basic_block_live_at_start.find(static_cast<int>(b));
        lit != live_probe.sideband.basic_block_live_at_start.end()) {
      for (int r : lit->second) {
        const int eff = resolve_reg(r);
        if (eff >= 0 && eff < 64) {
          hard_live |= (uint64_t{1} << eff);
        } else if (is_global_pseudo.contains(eff)) {
          global_live.insert(eff);
        }
      }
    }
    for (int gp : global_live) {
      base_hr_conflicts[gp] |= hard_live;
      for (int other : global_live) {
        if (other != gp) {
          pseudo_conflicts[gp].insert(other);
        }
      }
    }

    for (const RtlPatternAndNotes& pn : live_blocks[b]) {
      auto visit_stores = [&](bool clobbers_only) {
        auto step_elem = [&](const RtlExpr& elem) {
          if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty() ||
              (clobbers_only != (elem.op == RtlOp::kClobber))) {
            return;
          }
          const RtlExpr* dst = &elem.children[0];
          while ((dst->op == RtlOp::kSubreg || dst->op == RtlOp::kStrictLowPart ||
                  dst->op == RtlOp::kZeroExtract || dst->op == RtlOp::kSignExtract) &&
                 !dst->children.empty()) {
            dst = &dst->children[0];
          }
          if (dst->op != RtlOp::kReg || dst->IsArgPointerReg()) {
            return;
          }
          if (!clobbers_only && elem.op == RtlOp::kSet && elem.children.size() >= 2) {
            RecordFalsePrefSetPreference(*dst, elem.children[1], is_global_pseudo,
                                         out->sideband.reg_renumber, &base_hr_prefs);
          }
          const int eff = resolve_reg(dst->RegNo());
          if (is_global_pseudo.contains(eff)) {
            global_live.insert(eff);
            base_hr_conflicts[eff] |= hard_live;
            for (int other : global_live) {
              if (other != eff) {
                pseudo_conflicts[eff].insert(other);
                pseudo_conflicts[other].insert(eff);
              }
            }
          } else if (eff >= 0 && eff < 64) {
            const int nregs = std::max(1, MipsHardRegNregs(eff, dst->mode, gp64, fp64));
            for (int k = eff; k < eff + nregs && k < 64; ++k) {
              for (int gp : global_live) {
                base_hr_conflicts[gp] |= (uint64_t{1} << k);
              }
              hard_live |= (uint64_t{1} << k);
            }
          }
        };
        if (pn.pattern.op == RtlOp::kParallel) {
          for (int k = static_cast<int>(pn.pattern.children.size()) - 1; k >= 0; --k) {
            step_elem(pn.pattern.children[k]);
          }
        } else {
          step_elem(pn.pattern);
        }
      };
      visit_stores(/*clobbers_only=*/true);
      for (const RtlRegNote& note : pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
            !note.datum.IsArgPointerReg()) {
          const int eff = resolve_reg(note.datum.RegNo());
          if (is_global_pseudo.contains(eff)) {
            global_live.erase(eff);
          } else if (eff >= 0 && eff < 64) {
            const int nregs = std::max(1, MipsHardRegNregs(eff, note.datum.mode, gp64, fp64));
            for (int k = eff; k < eff + nregs && k < 64; ++k) {
              hard_live &= ~(uint64_t{1} << k);
            }
          }
        }
      }
      visit_stores(/*clobbers_only=*/false);
    }
  }

  auto pruned_prefs_of = [&](int gp) -> uint64_t {
    const auto hr_it = out->sideband.reg_renumber.find(gp);
    if (hr_it == out->sideband.reg_renumber.end() || hr_it->second < 2 || hr_it->second >= 64) {
      return 0;
    }
    uint64_t prefs = base_hr_prefs[gp] & ~base_hr_conflicts[gp];
    const auto cc_it = live_probe.sideband.reg_n_calls_crossed.find(gp);
    const bool crosses_call =
        (cc_it != live_probe.sideband.reg_n_calls_crossed.end() && cc_it->second > 0);
    const bool want_gpr = (hr_it->second < 32);
    for (int h = 0; h < 64; ++h) {
      if ((prefs & (uint64_t{1} << h)) == 0) {
        continue;
      }
      const bool valid_bank = want_gpr ? ((h >= 2 && h <= 25) || h == 30) : (h >= 32 && h <= 63);
      if (!valid_bank || (crosses_call && IsCallUsedHardReg(h))) {
        prefs &= ~(uint64_t{1} << h);
      }
    }
    return prefs;
  };

  std::vector<int> sorted_globals(is_global_pseudo.begin(), is_global_pseudo.end());
  std::sort(sorted_globals.begin(), sorted_globals.end());
  absl::flat_hash_map<int, int> demote_to_hr;
  for (int p_other : sorted_globals) {
    if (demote_to_hr.contains(p_other)) {
      continue;
    }
    const int h_other = out->sideband.reg_renumber.at(p_other);
    if (h_other < 2 || h_other >= 64 ||
        MipsHardRegNregs(h_other, summaries[p_other].widest_mode, gp64, fp64) != 1) {
      continue;
    }
    const uint64_t prefs = pruned_prefs_of(p_other);
    if (prefs == 0 || (prefs & (uint64_t{1} << h_other)) != 0) {
      continue;
    }
    for (int h_pref = 2; h_pref < 64; ++h_pref) {
      if ((prefs & (uint64_t{1} << h_pref)) == 0 || h_pref == h_other) {
        continue;
      }
      bool already_shielded = false;
      for (int q : pseudo_conflicts[p_other]) {
        const auto q_hr_it = out->sideband.reg_renumber.find(q);
        if (q_hr_it == out->sideband.reg_renumber.end() || q_hr_it->second != h_pref) {
          continue;
        }
        if (demote_to_hr.contains(q) || (pruned_prefs_of(q) & (uint64_t{1} << h_pref)) != 0) {
          already_shielded = true;
          break;
        }
      }
      if (already_shielded) {
        continue;
      }
      int best_q = -1;
      bool best_hoisted = false;
      size_t best_blocks = std::numeric_limits<size_t>::max();
      for (int q : pseudo_conflicts[p_other]) {
        const auto q_hr_it = out->sideband.reg_renumber.find(q);
        const auto qs_it = summaries.find(q);
        if (q_hr_it == out->sideband.reg_renumber.end() || q_hr_it->second != h_pref ||
            qs_it == summaries.end() || qs_it->second.has_flags || live_at_b0_start.contains(q) ||
            MipsHardRegNregs(h_pref, qs_it->second.widest_mode, gp64, fp64) != 1) {
          continue;
        }
        const auto q_cc = live_probe.sideband.reg_n_calls_crossed.find(q);
        if (q_cc != live_probe.sideband.reg_n_calls_crossed.end() && q_cc->second > 0 &&
            IsCallUsedHardReg(h_pref)) {
          continue;
        }
        bool read_in_def_blk = false;
        for (size_t db : qs_it->second.def_blocks) {
          if (qs_it->second.read_blocks.contains(db)) {
            read_in_def_blk = true;
            break;
          }
        }
        const bool hoisted = !qs_it->second.def_blocks.empty() && !read_in_def_blk;
        const size_t n_blks = qs_it->second.blocks_ref.size();
        if (best_q < 0 || (hoisted != best_hoisted
                               ? hoisted
                               : (n_blks != best_blocks ? n_blks < best_blocks : q > best_q))) {
          best_q = q;
          best_hoisted = hoisted;
          best_blocks = n_blks;
        }
      }
      if (best_q >= 0) {
        demote_to_hr[best_q] = h_pref;
      }
    }
  }
  if (demote_to_hr.empty()) {
    return;
  }
  auto rewrite_fn = [&](RtlExpr& r) {
    if (r.IsPseudoReg()) {
      if (const auto it = demote_to_hr.find(r.RegNo()); it != demote_to_hr.end()) {
        r.SetRegNo(it->second);
      }
    }
  };
  for (auto& blk : all_parsed_blocks) {
    for (RtlPatternAndNotes& pn : blk) {
      pn.RewriteRegs(rewrite_fn);
    }
  }
  if (out->sideband.epilogue_delay_pattern.has_value() &&
      !out->sideband.epilogue_delay_pattern->empty()) {
    if (auto ep = ParsePatternAndNotes(*out->sideband.epilogue_delay_pattern); ep.ok()) {
      ep->RewriteRegs(rewrite_fn);
      out->sideband.epilogue_delay_pattern = ep->ToGccPrintRtlString();
    }
  }
  for (const auto& [q, _] : demote_to_hr) {
    out->sideband.reg_renumber.erase(q);
  }
}

void CoalesceO0ExhaustedCalleeSavedFprPseudos(
    std::vector<std::vector<RtlPatternAndNotes>>* all_blocks, RtlControlFlowGraph* out) {
  absl::flat_hash_map<int, RtlMachineMode> pseudo_widest_mode;
  for (const auto& blk : *all_blocks) {
    for (const RtlPatternAndNotes& pn : blk) {
      pn.pattern.Visit([&](const RtlExpr& n) {
        if (n.IsPseudoReg()) {
          auto it = pseudo_widest_mode.find(n.RegNo());
          if (it == pseudo_widest_mode.end() ||
              MipsModeByteSize(n.mode) > MipsModeByteSize(it->second)) {
            pseudo_widest_mode[n.RegNo()] = n.mode;
          }
        }
      });
    }
  }
  bool has_fp_in_callee_saved_gpr = false;
  for (const auto& [p, hr] : out->sideband.reg_renumber) {
    if (hr >= 16 && hr <= 23) {
      const auto it = pseudo_widest_mode.find(p);
      if (it != pseudo_widest_mode.end() &&
          (it->second == RtlMachineMode::kSf || it->second == RtlMachineMode::kDf)) {
        has_fp_in_callee_saved_gpr = true;
        break;
      }
    }
  }
  if (!has_fp_in_callee_saved_gpr) {
    return;
  }
  absl::flat_hash_map<int, int> canonical_for_hr;
  absl::flat_hash_map<int, int> remap;
  std::vector<int> pseudos;
  for (const auto& [p, hr] : out->sideband.reg_renumber) {
    if (hr >= 52 && hr < 64) {
      pseudos.push_back(p);
    }
  }
  std::sort(pseudos.begin(), pseudos.end());
  for (int p : pseudos) {
    const int hr = out->sideband.reg_renumber.at(p);
    auto [it, inserted] = canonical_for_hr.emplace(hr, p);
    if (!inserted) {
      remap[p] = it->second;
    }
  }
  if (remap.empty()) {
    return;
  }
  for (auto& blk : *all_blocks) {
    for (RtlPatternAndNotes& pn : blk) {
      pn.RewriteRegs([&](RtlExpr& r) {
        if (r.IsPseudoReg()) {
          if (auto it = remap.find(r.RegNo()); it != remap.end()) {
            r.SetRegNo(it->second);
          }
        }
      });
    }
  }
  for (const auto& [dead_p, _] : remap) {
    out->sideband.reg_renumber.erase(dead_p);
  }
}

void CompactAndRefreshPreAlterSideband(int first_pseudo_regno, int input_max_regno, int opt_level,
                                       std::vector<std::vector<RtlPatternAndNotes>>* all_blocks,
                                       RtlControlFlowGraph* out) {
  std::vector<int> existing_pseudos;
  existing_pseudos.reserve(out->sideband.reg_renumber.size());
  for (const auto& [p, _] : out->sideband.reg_renumber) {
    if (p >= first_pseudo_regno) {
      existing_pseudos.push_back(p);
    }
  }
  std::sort(existing_pseudos.begin(), existing_pseudos.end(), [&](int a, int b) {
    if (opt_level == 0) {
      const int ha = out->sideband.reg_renumber.at(a);
      const int hb = out->sideband.reg_renumber.at(b);
      const int ka = (ha < 0) ? 1000 : ha;
      const int kb = (hb < 0) ? 1000 : hb;
      if (ka != kb) {
        return ka < kb;
      }
    }
    return a < b;
  });

  absl::flat_hash_set<int> reserved_unowned_pseudos;
  auto collect_unowned = [&](const RtlExpr& expr) {
    expr.Visit([&](const RtlExpr& n) {
      if (n.IsPseudoReg() && n.RegNo() >= first_pseudo_regno &&
          !out->sideband.reg_renumber.contains(n.RegNo())) {
        reserved_unowned_pseudos.insert(n.RegNo());
      }
    });
  };
  for (const auto& blk : *all_blocks) {
    for (const RtlPatternAndNotes& pn : blk) {
      collect_unowned(pn.pattern);
      for (const RtlRegNote& note : pn.reg_notes) {
        if (note.list_op == RtlOp::kExprList) {
          collect_unowned(note.datum);
        }
      }
      for (const RtlExpr& fu : pn.call_fusage) {
        collect_unowned(fu);
      }
    }
  }
  if (out->sideband.epilogue_delay_pattern.has_value() &&
      !out->sideband.epilogue_delay_pattern->empty()) {
    if (auto ep = ParsePatternAndNotes(*out->sideband.epilogue_delay_pattern); ep.ok()) {
      collect_unowned(ep->pattern);
      for (const RtlRegNote& note : ep->reg_notes) {
        if (note.list_op == RtlOp::kExprList) {
          collect_unowned(note.datum);
        }
      }
    }
  }

  absl::flat_hash_map<int, int> renumber_map;
  int next_p = first_pseudo_regno;
  bool any_renamed = false;
  for (int old_p : existing_pseudos) {
    while (reserved_unowned_pseudos.contains(next_p)) {
      ++next_p;
    }
    const int new_p = next_p++;
    renumber_map[old_p] = new_p;
    if (new_p != old_p) {
      any_renamed = true;
    }
  }

  if (any_renamed) {
    absl::flat_hash_map<int, int> new_reg_renumber;
    for (const auto& [p, hr] : out->sideband.reg_renumber) {
      auto it = renumber_map.find(p);
      new_reg_renumber[(it != renumber_map.end()) ? it->second : p] = hr;
    }
    out->sideband.reg_renumber = std::move(new_reg_renumber);

    auto rewrite_p = [&](RtlExpr& r) {
      if (r.IsPseudoReg()) {
        if (auto it = renumber_map.find(r.RegNo()); it != renumber_map.end()) {
          r.SetRegNo(it->second);
        }
      }
    };
    for (auto& blk : *all_blocks) {
      for (RtlPatternAndNotes& pn : blk) {
        pn.RewriteRegs(rewrite_p);
      }
    }
    if (out->sideband.epilogue_delay_pattern.has_value() &&
        !out->sideband.epilogue_delay_pattern->empty()) {
      if (auto ep = ParsePatternAndNotes(*out->sideband.epilogue_delay_pattern); ep.ok()) {
        ep->RewriteRegs(rewrite_p);
        out->sideband.epilogue_delay_pattern = ep->ToGccPrintRtlString();
      }
    }
  }

  out->sideband.reg_n_refs.clear();
  out->sideband.reg_n_sets.clear();
  int max_seen = std::max(kFirstPseudoRegister - 1, next_p - 1);
  for (const auto& [p, _] : out->sideband.reg_renumber) {
    max_seen = std::max(max_seen, p);
  }

  auto scan_pattern = [&](const RtlExpr& root, int weight) {
    const bool is_standalone_clobber = (root.op == RtlOp::kClobber);
    auto walk = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
      if (e.op == RtlOp::kSet && e.children.size() >= 2) {
        self(self, e.children[0], true);
        for (size_t k = 1; k < e.children.size(); ++k) {
          self(self, e.children[k], false);
        }
        return;
      }
      if (e.op == RtlOp::kClobber) {
        if (!e.children.empty()) {
          if (e.children[0].op == RtlOp::kMem) {
            self(self, e.children[0], false);
          } else {
            const RtlExpr* t = &e.children[0];
            while (t->op == RtlOp::kSubreg && !t->children.empty()) {
              t = &t->children[0];
            }
            if (t->IsPseudoReg() && (!is_standalone_clobber || opt_level == 0)) {
              out->sideband.reg_n_sets[t->RegNo()] += 1;
              max_seen = std::max(max_seen, t->RegNo());
            }
          }
        }
        return;
      }
      if (in_dst &&
          (e.op == RtlOp::kZeroExtract || e.op == RtlOp::kSignExtract ||
           e.op == RtlOp::kStrictLowPart) &&
          !e.children.empty()) {
        self(self, e.children[0], true);
        for (size_t k = 1; k < e.children.size(); ++k) {
          self(self, e.children[k], false);
        }
        return;
      }
      if (in_dst && e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].IsPseudoReg()) {
        const int p = e.children[0].RegNo();
        out->sideband.reg_n_refs[p] += weight;
        out->sideband.reg_n_sets[p] += 1;
        max_seen = std::max(max_seen, p);
        return;
      }
      if (e.IsPseudoReg()) {
        const int p = e.RegNo();
        out->sideband.reg_n_refs[p] += weight;
        if (in_dst) {
          out->sideband.reg_n_sets[p] += 1;
        }
        max_seen = std::max(max_seen, p);
        return;
      }
      for (const RtlExpr& ch : e.children) {
        self(self, ch, false);
      }
    };
    if (root.op == RtlOp::kSequence) {
      for (const RtlExpr& sub : root.children) {
        if (!sub.children.empty()) {
          walk(walk, sub.children[0], false);
        }
      }
    } else {
      walk(walk, root, false);
    }
  };

  int loop_depth = 1;
  for (size_t b = 0; b < all_blocks->size(); ++b) {
    RtlBasicBlock& blk = out->rpo_blocks[b];
    for (size_t i = 0; i < (*all_blocks)[b].size(); ++i) {
      if (auto lk = ExtractLoopNoteKind(blk, i, (*all_blocks)[b][i].pattern); lk.has_value()) {
        if (*lk == RtlLoopNoteKind::kLoopBeg) {
          ++loop_depth;
        } else if (*lk == RtlLoopNoteKind::kLoopEnd) {
          loop_depth = std::max(1, loop_depth - 1);
        }
      }
      const int w = (opt_level > 0) ? loop_depth : 1;
      scan_pattern((*all_blocks)[b][i].pattern, w);
      for (const RtlExpr& fu : (*all_blocks)[b][i].call_fusage) {
        scan_pattern(fu, w);
      }
      const RtlPatternAndNotes& pn = (*all_blocks)[b][i];
      blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(
          pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{});
    }
    blk.SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < blk.rtl_insns.size() && i < (*all_blocks)[b].size(); ++i) {
      if ((*all_blocks)[b][i].has_explicit_call_fusage &&
          !blk.rtl_insns[i].has_explicit_call_fusage) {
        blk.rtl_insns[i].has_explicit_call_fusage = true;
        blk.rtl_insns[i].call_fusage = (*all_blocks)[b][i].call_fusage;
      }
    }
  }
  if (out->sideband.epilogue_delay_pattern.has_value() &&
      !out->sideband.epilogue_delay_pattern->empty()) {
    if (auto ep = ParsePatternAndNotes(*out->sideband.epilogue_delay_pattern); ep.ok()) {
      scan_pattern(ep->pattern, 1);
    }
  }
  if (opt_level == 0) {
    // At `-O0`, `cleanup_subreg_operands` inside `reload()` (`reload1.c:2058`)
    // lowers `(set (subreg:SI (reg:DI p) 0/1) ...)` into bare `SImode` writes
    // without re-running `reg_scan`, leaving `reg_n_sets[p]` populated with the
    // pre-`alter_subreg` subword write count even when `p` only survives on
    // whole-register `(use (reg:DI p))` markers in `pre_alter`.
    absl::flat_hash_map<int, RtlMachineMode> pseudo_mode;
    for (const auto& blk : *all_blocks) {
      for (const RtlPatternAndNotes& pn : blk) {
        pn.pattern.Visit([&](const RtlExpr& n) {
          if (n.IsPseudoReg() &&
              MipsModeByteSize(n.mode) > MipsModeByteSize(pseudo_mode[n.RegNo()])) {
            pseudo_mode[n.RegNo()] = n.mode;
          }
        });
      }
    }
    for (const auto& entry : out->sideband.reg_renumber) {
      const int p = entry.first;
      const int hr = entry.second;
      if (hr < 0 || out->sideband.reg_n_sets[p] > 0) {
        continue;
      }
      const auto mit = pseudo_mode.find(p);
      if (mit == pseudo_mode.end() || MipsModeByteSize(mit->second) <= 1) {
        continue;
      }
      const int span = std::max(1, MipsModeByteSize(mit->second) / 4);
      int subword_sets = 0;
      for (const auto& blk : *all_blocks) {
        for (const RtlPatternAndNotes& pn : blk) {
          auto check_set = [&](const RtlExpr& s) {
            if (!s.IsSet() || !s.SetDest().IsReg() ||
                MipsModeByteSize(s.SetDest().mode) >= MipsModeByteSize(mit->second)) {
              return;
            }
            const int d_reg = s.SetDest().RegNo();
            int d_hr = d_reg;
            if (d_reg >= kFirstPseudoRegister) {
              const auto rit = out->sideband.reg_renumber.find(d_reg);
              d_hr = (rit != out->sideband.reg_renumber.end()) ? rit->second : -1;
            }
            if (d_hr >= hr && d_hr < hr + span) {
              ++subword_sets;
            }
          };
          if (pn.pattern.op == RtlOp::kParallel) {
            for (const RtlExpr& ch : pn.pattern.children) {
              check_set(ch);
            }
          } else {
            check_set(pn.pattern);
          }
        }
      }
      if (subword_sets > 0) {
        out->sideband.reg_n_sets[p] = subword_sets;
      }
    }
  }
  for (const auto& [p, _] : out->sideband.reg_renumber) {
    out->sideband.reg_n_refs[p] = std::max(1, out->sideband.reg_n_refs[p]);
    out->sideband.reg_n_sets.try_emplace(p, 0);
  }
  if (out->sideband.reg_renumber.empty()) {
    bool any_pseudo_live_len = false;
    for (const auto& [r, len] : out->sideband.reg_live_length) {
      if (r >= kFirstPseudoRegister && len != 0) {
        any_pseudo_live_len = true;
        break;
      }
    }
    if (!any_pseudo_live_len) {
      out->sideband.reg_live_length.try_emplace(first_pseudo_regno, -1);
      out->sideband.basic_block_live_at_start.try_emplace(0);
    }
  }
  out->sideband.max_regno = std::max(input_max_regno, max_seen + 1);
}

bool IsMipsCallerSavableReg(int hr) {
  return (hr >= 1 && hr <= 15) || hr == 24 || hr == 25 || (hr >= 32 && hr <= 51);
}

bool CanDirectMoveMemAndHardReg(int hr, RtlMachineMode mode) {
  if (hr >= 1 && hr <= 31) {
    return true;
  }
  if (hr >= 32 && hr <= 63) {
    return mode == RtlMachineMode::kSF || mode == RtlMachineMode::kDF ||
           mode == RtlMachineMode::kSI || mode == RtlMachineMode::kDI;
  }
  return false;
}

std::optional<int64_t> ExtractBaseRegOffset(const RtlExpr& addr, int base_regno) {
  if (addr.op == RtlOp::kReg && !addr.IsArgPointerReg() && addr.RegNo() == base_regno) {
    return 0;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      !addr.children[0].IsArgPointerReg() && addr.children[0].RegNo() == base_regno &&
      addr.children[1].op == RtlOp::kConstInt) {
    return addr.children[1].int_val;
  }
  return std::nullopt;
}

bool ExprOverlapsElimInterval(const RtlExpr& root, int elim_reg, int64_t lo, int64_t hi) {
  bool overlaps = false;
  auto walk = [&](const auto& self, const RtlExpr& expr, bool in_mem_addr) -> void {
    if (overlaps) {
      return;
    }
    if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
      const std::optional<int64_t> off = ExtractBaseRegOffset(expr.children[0], elim_reg);
      if (off.has_value()) {
        const int sz = std::max(1, MipsModeByteSize(expr.mode));
        if (*off<hi&& * off + sz> lo) {
          overlaps = true;
        }
        return;
      }
      self(self, expr.children[0], true);
      return;
    }
    if (!in_mem_addr) {
      const std::optional<int64_t> off = ExtractBaseRegOffset(expr, elim_reg);
      if (off.has_value() && *off >= lo && *off < hi) {
        overlaps = true;
        return;
      }
    }
    for (const RtlExpr& ch : expr.children) {
      self(self, ch, in_mem_addr);
    }
  };
  walk(walk, root, false);
  return overlaps;
}

// Identifies intra-block `caller-save` (`caller-save.c:insert_save_restore`)
// save/restore instructions in `allocated_cfg` so `InvertAlterReg` leaves their
// `regno_reg_rtx[hr]` operands as unflagged hard registers and preserves
// reaching-definition tokens across `CALL_INSN`s without pinning `/v` webs.
void PrecomputeCallerSavePairs(const RtlControlFlowGraph& allocated_cfg,
                               const ParsedCompilerFlags& compiler_flags, bool gp64, bool fp64,
                               const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
                               std::vector<std::vector<int>>* cs_save_span,
                               std::vector<std::vector<int>>* cs_restore_span) {
  const size_t num_blocks = parsed_blocks.size();
  cs_save_span->assign(num_blocks, {});
  cs_restore_span->assign(num_blocks, {});
  for (size_t b = 0; b < num_blocks; ++b) {
    (*cs_save_span)[b].assign(parsed_blocks[b].size(), 0);
    (*cs_restore_span)[b].assign(parsed_blocks[b].size(), 0);
  }
  const int frame_size_bytes = allocated_cfg.sideband.frame_size_bytes;
  const int outgoing_args_size = allocated_cfg.sideband.outgoing_args_size;
  if (compiler_flags.opt_level <= 0 || !compiler_flags.flag_caller_saves || frame_size_bytes <= 0 ||
      outgoing_args_size < 0) {
    return;
  }
  const int elim_reg = allocated_cfg.sideband.frame_pointer_needed ? kFramePointerRegno : 29;
  const int64_t min_off = outgoing_args_size;
  const int64_t max_off = min_off + frame_size_bytes;

  struct SaveWord {
    size_t save_idx = 0;
    int total_words = 1;
    int word_bit = 0;
    int word_hr = -1;
    int64_t offset = 0;
    int word_bytes = 4;
  };
  struct MatchedSave {
    size_t save_idx = 0;
    int save_span = 0;
    std::vector<std::pair<size_t, int>> restores;
    std::vector<std::pair<int, std::pair<int64_t, int>>> bounds;
  };

  auto plan_all = [&](bool restrict_hrs, const absl::flat_hash_set<int>& allowed_hrs,
                      std::vector<std::vector<MatchedSave>>* out_blocks,
                      absl::flat_hash_map<int, std::pair<int64_t, int>>* out_hr_bounds) -> bool {
    out_blocks->assign(num_blocks, {});
    out_hr_bounds->clear();
    for (size_t b = 0; b < num_blocks; ++b) {
      const auto& insns = parsed_blocks[b];
      std::vector<size_t> call_indices;
      for (size_t i = 0; i < insns.size(); ++i) {
        if (insns[i].pattern.op == RtlOp::kSequence) {
          call_indices.clear();
          break;
        }
        if (ContainsCallOp(insns[i].pattern)) {
          call_indices.push_back(i);
        }
      }
      if (call_indices.empty()) {
        continue;
      }

      absl::flat_hash_map<int, SaveWord> saved_in_slot;
      absl::flat_hash_map<int, SaveWord> needs_restore;
      absl::flat_hash_map<size_t, uint32_t> restored_mask;
      absl::flat_hash_map<size_t, int> expected_words;
      absl::flat_hash_map<size_t, int> save_spans;
      absl::flat_hash_map<size_t, std::vector<std::pair<size_t, int>>> restores_per_save;
      absl::flat_hash_map<size_t, std::vector<std::pair<int, std::pair<int64_t, int>>>>
          bounds_per_save;
      std::vector<std::pair<size_t, std::vector<SaveWord>>> pending_saves;

      auto match_cs_mem = [&](const RtlExpr& mem, int mode_bytes) -> std::optional<int64_t> {
        if (mem.op != RtlOp::kMem || mem.flags.volatil || mem.flags.in_struct ||
            mem.children.size() != 1 || mode_bytes <= 0) {
          return std::nullopt;
        }
        const std::optional<int64_t> off = ExtractBaseRegOffset(mem.children[0], elim_reg);
        if (!off.has_value() || *off < min_off || *off + mode_bytes > max_off) {
          return std::nullopt;
        }
        return off;
      };

      for (size_t i = 0; i < insns.size(); ++i) {
        const RtlPatternAndNotes& pn = insns[i];
        if (ContainsCallOp(pn.pattern)) {
          for (const auto& [_, words] : pending_saves) {
            for (const SaveWord& w : words) {
              saved_in_slot[w.word_hr] = w;
            }
          }
          pending_saves.clear();
          needs_restore = saved_in_slot;
          // Any register written by `CALL_INSN`'s own return `SET` is not restored from a save.
          std::array<bool, kFirstPseudoRegister> call_written{};
          CollectWrittenHardRegWordsInPattern(pn.pattern, gp64, fp64, &call_written);
          for (int w = 0; w < kFirstPseudoRegister; ++w) {
            if (call_written[w]) {
              saved_in_slot.erase(w);
              needs_restore.erase(w);
            }
          }
          continue;
        }

        if (pn.reg_notes.empty() && pn.pattern.IsSet()) {
          const RtlExpr& dst = pn.pattern.SetDest();
          const RtlExpr& src = pn.pattern.SetSrc();
          if (dst.IsHardReg() && !dst.IsArgPointerReg() && !dst.flags.Any() &&
              dst.mode == src.mode) {
            const int hr = dst.RegNo();
            const int nregs = std::max(1, MipsHardRegNregs(hr, dst.mode, gp64, fp64));
            const int mode_bytes = MipsModeByteSize(dst.mode);
            const int word_bytes = std::max(1, mode_bytes / nregs);
            const std::optional<int64_t> off = match_cs_mem(src, mode_bytes);
            if (off.has_value() && needs_restore.contains(hr)) {
              const size_t s_idx = needs_restore[hr].save_idx;
              bool ok = true;
              for (int k = 0; k < nregs; ++k) {
                const auto it = needs_restore.find(hr + k);
                if (it == needs_restore.end() || it->second.save_idx != s_idx ||
                    it->second.offset != *off + int64_t{word_bytes} * k) {
                  ok = false;
                  break;
                }
              }
              if (ok) {
                expected_words[s_idx] = needs_restore[hr].total_words;
                for (int k = 0; k < nregs; ++k) {
                  restored_mask[s_idx] |= (uint32_t{1} << needs_restore[hr + k].word_bit);
                  needs_restore.erase(hr + k);
                }
                restores_per_save[s_idx].emplace_back(i, nregs);
                continue;
              }
            }
          }
          if (src.IsHardReg() && !src.IsArgPointerReg() && !src.flags.Any() &&
              dst.mode == src.mode && i < call_indices.back()) {
            const int hr = src.RegNo();
            const int nregs = std::max(1, MipsHardRegNregs(hr, src.mode, gp64, fp64));
            const int mode_bytes = MipsModeByteSize(src.mode);
            const int word_bytes = std::max(1, mode_bytes / nregs);
            const std::optional<int64_t> off = match_cs_mem(dst, mode_bytes);
            if (off.has_value()) {
              bool all_cand = true;
              std::vector<SaveWord> words;
              std::vector<std::pair<int, std::pair<int64_t, int>>> bounds;
              for (int k = 0; k < nregs; ++k) {
                if (!IsMipsCallerSavableReg(hr + k) ||
                    (restrict_hrs && !allowed_hrs.contains(hr + k))) {
                  all_cand = false;
                  break;
                }
                const int64_t w_off = *off + int64_t{word_bytes} * k;
                words.push_back(SaveWord{
                    .save_idx = i,
                    .total_words = nregs,
                    .word_bit = k,
                    .word_hr = hr + k,
                    .offset = w_off,
                    .word_bytes = word_bytes,
                });
                bounds.emplace_back(hr + k, std::make_pair(w_off, word_bytes));
              }
              if (all_cand) {
                save_spans[i] = nregs;
                bounds_per_save[i] = std::move(bounds);
                pending_saves.emplace_back(i, std::move(words));
                continue;
              }
            }
          }
        }

        std::array<bool, kFirstPseudoRegister> written{};
        CollectWrittenHardRegWordsInPattern(pn.pattern, gp64, fp64, &written);
        for (int w = 0; w < kFirstPseudoRegister; ++w) {
          if (written[w]) {
            saved_in_slot.erase(w);
            needs_restore.erase(w);
            pending_saves.erase(
                std::remove_if(pending_saves.begin(), pending_saves.end(),
                               [&](const auto& entry) {
                                 return std::any_of(
                                     entry.second.begin(), entry.second.end(),
                                     [&](const SaveWord& sw) { return sw.word_hr == w; });
                               }),
                pending_saves.end());
          }
        }
      }

      for (const auto& [s_idx, mask] : restored_mask) {
        const int exp_w = expected_words[s_idx];
        const uint32_t full = (exp_w >= 32) ? ~uint32_t{0} : ((uint32_t{1} << exp_w) - 1);
        if (mask != full) {
          continue;
        }
        for (const auto& [hr, bound] : bounds_per_save[s_idx]) {
          auto [it, inserted] = out_hr_bounds->try_emplace(hr, bound);
          if (!inserted && it->second != bound) {
            return false;
          }
        }
        (*out_blocks)[b].push_back(MatchedSave{
            .save_idx = s_idx,
            .save_span = save_spans[s_idx],
            .restores = std::move(restores_per_save[s_idx]),
            .bounds = std::move(bounds_per_save[s_idx]),
        });
      }
    }
    return true;
  };

  std::vector<std::vector<MatchedSave>> block_matches;
  absl::flat_hash_map<int, std::pair<int64_t, int>> hr_bounds;
  if (!plan_all(/*restrict_hrs=*/false, {}, &block_matches, &hr_bounds) || hr_bounds.empty()) {
    return;
  }

  auto build_cs_insn_mask = [&](const std::vector<std::vector<MatchedSave>>& matches) {
    std::vector<std::vector<bool>> is_cs(num_blocks);
    for (size_t b = 0; b < num_blocks; ++b) {
      is_cs[b].assign(parsed_blocks[b].size(), false);
      for (const MatchedSave& ms : matches[b]) {
        is_cs[b][ms.save_idx] = true;
        for (const auto& [r_idx, _] : ms.restores) {
          is_cs[b][r_idx] = true;
        }
      }
    }
    return is_cs;
  };

  std::vector<std::vector<bool>> is_cs = build_cs_insn_mask(block_matches);
  absl::flat_hash_set<int> invalid_hrs;
  for (const auto& [hr, bound] : hr_bounds) {
    const int64_t lo = bound.first;
    const int64_t hi = bound.first + bound.second;
    for (size_t b = 0; b < num_blocks && !invalid_hrs.contains(hr); ++b) {
      for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
        for (const RtlRegNote& note : parsed_blocks[b][i].reg_notes) {
          if (note.list_op == RtlOp::kExprList &&
              ExprOverlapsElimInterval(note.datum, elim_reg, lo, hi)) {
            invalid_hrs.insert(hr);
            break;
          }
        }
        if (invalid_hrs.contains(hr) || is_cs[b][i]) {
          continue;
        }
        if (ExprOverlapsElimInterval(parsed_blocks[b][i].pattern, elim_reg, lo, hi)) {
          invalid_hrs.insert(hr);
          break;
        }
      }
    }
  }

  if (!invalid_hrs.empty()) {
    if (invalid_hrs.size() == hr_bounds.size()) {
      return;
    }
    absl::flat_hash_set<int> allowed_hrs;
    for (const auto& [hr, _] : hr_bounds) {
      if (!invalid_hrs.contains(hr)) {
        allowed_hrs.insert(hr);
      }
    }
    if (!plan_all(/*restrict_hrs=*/true, allowed_hrs, &block_matches, &hr_bounds) ||
        hr_bounds.empty()) {
      return;
    }
    is_cs = build_cs_insn_mask(block_matches);
  }

  std::vector<std::pair<int, std::pair<int64_t, int>>> sorted_bounds(hr_bounds.begin(),
                                                                     hr_bounds.end());
  std::sort(sorted_bounds.begin(), sorted_bounds.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t k = 1; k < sorted_bounds.size(); ++k) {
    if (sorted_bounds[k].second.first <
        sorted_bounds[k - 1].second.first + sorted_bounds[k - 1].second.second) {
      return;
    }
  }
  const int64_t min_cs_off = sorted_bounds.front().second.first;
  const int64_t aligned_cs_base = min_off + ((min_cs_off - min_off) & ~int64_t{7});
  if (aligned_cs_base < min_cs_off) {
    for (size_t b = 0; b < num_blocks; ++b) {
      for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
        for (const RtlRegNote& note : parsed_blocks[b][i].reg_notes) {
          if (note.list_op == RtlOp::kExprList &&
              ExprOverlapsElimInterval(note.datum, elim_reg, aligned_cs_base, min_cs_off)) {
            return;
          }
        }
        if (is_cs[b][i]) {
          continue;
        }
        if (ExprOverlapsElimInterval(parsed_blocks[b][i].pattern, elim_reg, aligned_cs_base,
                                     min_cs_off)) {
          return;
        }
      }
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    for (const MatchedSave& ms : block_matches[b]) {
      (*cs_save_span)[b][ms.save_idx] = ms.save_span;
      for (const auto& [r_idx, r_span] : ms.restores) {
        (*cs_restore_span)[b][r_idx] = r_span;
      }
    }
  }
}

// Demotes `reload`-introduced scratch registers back to physical hard registers
// (`regno_reg_rtx[hr]`) before RMW coalescing so `InvertAlterReg` produces the
// exact pre-`alter_reg` boundary expected by `SubfunctionId::kReload`.
struct ScratchCandStats {
  int def_count = 0;
  int use_count = 0;
  size_t first_block = 0;
  bool seen_block = false;
  bool multi_block = false;
  bool has_flags = false;
  bool blocked = false;
};

absl::flat_hash_map<int, ScratchCandStats> CollectScratchCandStats(
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const RtlControlFlowGraph& out) {
  absl::flat_hash_map<int, ScratchCandStats> stats;
  auto record_reg = [&](const RtlExpr& r, size_t b, bool is_def, bool is_use, bool block_it) {
    if (!r.IsPseudoReg()) {
      return;
    }
    ScratchCandStats& st = stats[r.RegNo()];
    if (!st.seen_block) {
      st.seen_block = true;
      st.first_block = b;
    } else if (st.first_block != b) {
      st.multi_block = true;
    }
    if (is_def) {
      ++st.def_count;
    }
    if (is_use) {
      ++st.use_count;
    }
    if (r.flags.Any()) {
      st.has_flags = true;
    }
    if (block_it) {
      st.blocked = true;
    }
  };

  auto scan_expr = [&](const auto& self, const RtlExpr& e, size_t b, bool in_dst,
                       bool top_use_or_fusage) -> void {
    if (e.op == RtlOp::kSequence) {
      e.Visit([&](const RtlExpr& n) {
        if (n.IsPseudoReg()) {
          record_reg(n, b, false, false, /*block_it=*/true);
        }
      });
      return;
    }
    if (e.op == RtlOp::kUse) {
      for (const RtlExpr& ch : e.children) {
        self(self, ch, b, false, /*top_use_or_fusage=*/true);
      }
      return;
    }
    if (e.op == RtlOp::kClobber) {
      if (!e.children.empty()) {
        if (e.children[0].op == RtlOp::kMem) {
          self(self, e.children[0], b, false, top_use_or_fusage);
        } else {
          e.children[0].Visit([&](const RtlExpr& n) {
            if (n.IsPseudoReg()) {
              record_reg(n, b, true, false, /*block_it=*/true);
            }
          });
        }
      }
      return;
    }
    if (e.op == RtlOp::kSet && e.children.size() >= 2) {
      self(self, e.children[0], b, true, top_use_or_fusage);
      for (size_t k = 1; k < e.children.size(); ++k) {
        self(self, e.children[k], b, false, top_use_or_fusage);
      }
      return;
    }
    if (in_dst &&
        (e.op == RtlOp::kSubreg || e.op == RtlOp::kStrictLowPart || e.op == RtlOp::kZeroExtract ||
         e.op == RtlOp::kSignExtract) &&
        !e.children.empty()) {
      e.Visit([&](const RtlExpr& n) {
        if (n.IsPseudoReg()) {
          record_reg(n, b, true, true, /*block_it=*/true);
        }
      });
      return;
    }
    if (in_dst && e.op == RtlOp::kMem) {
      for (const RtlExpr& ch : e.children) {
        self(self, ch, b, false, top_use_or_fusage);
      }
      return;
    }
    if (e.IsPseudoReg()) {
      record_reg(e, b, in_dst, !in_dst, top_use_or_fusage);
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch, b, false, top_use_or_fusage);
    }
  };

  for (size_t b = 0; b < parsed_blocks.size(); ++b) {
    for (const RtlPatternAndNotes& pn : parsed_blocks[b]) {
      scan_expr(scan_expr, pn.pattern, b, false, false);
      for (const RtlExpr& fu : pn.call_fusage) {
        scan_expr(scan_expr, fu, b, false, true);
      }
      for (const RtlRegNote& note : pn.reg_notes) {
        if (note.list_op == RtlOp::kExprList && note.kind != RtlMachineMode::kRegDead &&
            note.kind != RtlMachineMode::kRegUnused) {
          note.datum.Visit([&](const RtlExpr& n) {
            if (n.IsPseudoReg()) {
              stats[n.RegNo()].blocked = true;
            }
          });
        }
      }
    }
  }
  if (out.sideband.epilogue_delay_pattern.has_value() &&
      !out.sideband.epilogue_delay_pattern->empty()) {
    if (auto ep = ParsePatternAndNotes(*out.sideband.epilogue_delay_pattern); ep.ok()) {
      ep->pattern.Visit([&](const RtlExpr& n) {
        if (n.IsPseudoReg()) {
          stats[n.RegNo()].blocked = true;
        }
      });
    }
  }
  return stats;
}

void DemoteInheritedSpillOutputFprScratches(
    bool gp64, bool fp64, const absl::flat_hash_map<int, ScratchCandStats>& stats,
    const absl::flat_hash_set<int>& proven_spill_output_scratches,
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const RtlControlFlowGraph& out, absl::flat_hash_map<int, int>* demote_to_hr) {
  if (proven_spill_output_scratches.empty()) {
    return;
  }
  auto eff_hr = [&](const RtlExpr& r) -> int {
    if (r.op != RtlOp::kReg || r.IsArgPointerReg()) {
      return -1;
    }
    if (r.RegNo() < kFirstPseudoRegister) {
      return r.RegNo();
    }
    const auto it = out.sideband.reg_renumber.find(r.RegNo());
    return (it != out.sideband.reg_renumber.end()) ? it->second : -1;
  };
  auto has_only_dead_or_unused_notes = [](const RtlPatternAndNotes& pn) {
    for (const RtlRegNote& note : pn.reg_notes) {
      if (note.list_op != RtlOp::kExprList ||
          (note.kind != RtlMachineMode::kRegDead && note.kind != RtlMachineMode::kRegUnused)) {
        return false;
      }
    }
    return true;
  };
  auto has_reg_dead_for = [&](const RtlPatternAndNotes& pn, int p, int hr) {
    for (const RtlRegNote& note : pn.reg_notes) {
      if (note.list_op == RtlOp::kExprList && note.kind == RtlMachineMode::kRegDead &&
          note.datum.op == RtlOp::kReg && !note.datum.IsArgPointerReg() &&
          (note.datum.RegNo() == p || eff_hr(note.datum) == hr)) {
        return true;
      }
    }
    return false;
  };
  auto insn_touches_hr = [&](const RtlExpr& pat, int hr, int nw) {
    bool hit = false;
    pat.Visit([&](const RtlExpr& n) {
      if (hit || n.op != RtlOp::kReg || n.IsArgPointerReg()) {
        return;
      }
      const int nhr = eff_hr(n);
      if (nhr >= 0) {
        const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
        if (nhr < hr + nw && hr < nhr + nnw) {
          hit = true;
        }
      }
    });
    return hit;
  };
  auto is_barrier_or_branch_or_call = [](const RtlExpr& pat) {
    if (ContainsCallOp(pat) || pat.op == RtlOp::kReturn || pat.op == RtlOp::kCodeLabel ||
        pat.op == RtlOp::kBarrier || pat.op == RtlOp::kSequence) {
      return true;
    }
    const auto is_pc_set = [](const RtlExpr& s) {
      return s.IsSet() && s.SetDest().op == RtlOp::kPc;
    };
    if (is_pc_set(pat)) {
      return true;
    }
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        if (ch.op == RtlOp::kReturn || is_pc_set(ch)) {
          return true;
        }
      }
    }
    return false;
  };

  for (size_t b = 0; b < parsed_blocks.size(); ++b) {
    const auto& blk = parsed_blocks[b];
    for (size_t i = 0; i + 1 < blk.size(); ++i) {
      const RtlPatternAndNotes& pn = blk[i];
      if (!pn.pattern.IsSet() || !has_only_dead_or_unused_notes(pn)) {
        continue;
      }
      const RtlExpr& dst = pn.pattern.SetDest();
      const RtlExpr& src = pn.pattern.SetSrc();
      if (!dst.IsPseudoReg() ||
          (dst.mode != RtlMachineMode::kSF && dst.mode != RtlMachineMode::kDF) ||
          !src.IsPseudoReg() || src.mode != dst.mode ||
          !proven_spill_output_scratches.contains(src.RegNo())) {
        continue;
      }
      const int p_in = dst.RegNo();
      const auto sit = stats.find(p_in);
      if (sit == stats.end() || sit->second.def_count != 1 || sit->second.use_count != 1 ||
          sit->second.multi_block || sit->second.first_block != b || sit->second.has_flags ||
          sit->second.blocked) {
        continue;
      }
      const auto rit = out.sideband.reg_renumber.find(p_in);
      if (rit == out.sideband.reg_renumber.end() || rit->second < 32 || rit->second > 63) {
        continue;
      }
      const int fpr_hr = rit->second;
      const int nw = std::max(1, MipsHardRegNregs(fpr_hr, dst.mode, gp64, fp64));
      for (size_t k = i + 1; k < blk.size(); ++k) {
        if (blk[k].pattern.ContainsReg(p_in)) {
          const RtlExpr& up = blk[k].pattern;
          if (up.IsSet() && up.SetDest().IsPseudoReg()) {
            const auto dit = demote_to_hr->find(up.SetDest().RegNo());
            if (dit != demote_to_hr->end() && dit->second == fpr_hr &&
                !has_reg_dead_for(blk[k], p_in, fpr_hr)) {
              (*demote_to_hr)[p_in] = fpr_hr;
            }
          }
          break;
        }
        if (is_barrier_or_branch_or_call(blk[k].pattern) ||
            insn_touches_hr(blk[k].pattern, fpr_hr, nw)) {
          break;
        }
      }
    }
  }
}

void DemoteReloadAndCallerSaveHardScratches(
    const ParsedCompilerFlags& compiler_flags, bool gp64, bool fp64,
    const std::vector<std::vector<int>>& cs_save_span,
    std::vector<std::vector<RtlPatternAndNotes>>* parsed_blocks, RtlControlFlowGraph* out) {
  const absl::flat_hash_map<int, ScratchCandStats> stats =
      CollectScratchCandStats(*parsed_blocks, *out);

  auto eff_hr = [&](const RtlExpr& r) -> int {
    if (r.op != RtlOp::kReg || r.IsArgPointerReg()) {
      return -1;
    }
    if (r.RegNo() < kFirstPseudoRegister) {
      return r.RegNo();
    }
    const auto it = out->sideband.reg_renumber.find(r.RegNo());
    return (it != out->sideband.reg_renumber.end()) ? it->second : -1;
  };
  auto is_single_block_single_def_scratch = [&](int p, size_t b, int req_uses) -> bool {
    const auto it = stats.find(p);
    if (it == stats.end()) {
      return false;
    }
    const ScratchCandStats& st = it->second;
    if (st.multi_block || st.first_block != b || st.def_count != 1 || st.has_flags || st.blocked ||
        st.use_count <= 0 || (req_uses >= 0 && st.use_count != req_uses)) {
      return false;
    }
    const auto rit = out->sideband.reg_renumber.find(p);
    return rit != out->sideband.reg_renumber.end() && rit->second >= 0 &&
           rit->second < kFirstPseudoRegister;
  };
  auto has_reg_dead_for = [](const RtlPatternAndNotes& pn, int p, int hr) -> bool {
    for (const RtlRegNote& n : pn.reg_notes) {
      if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
          (n.datum.RegNo() == p || (hr >= 0 && n.datum.RegNo() == hr))) {
        return true;
      }
    }
    return false;
  };
  auto has_only_dead_or_unused_notes = [](const RtlPatternAndNotes& pn) -> bool {
    for (const RtlRegNote& n : pn.reg_notes) {
      if (n.list_op == RtlOp::kExprList && n.kind != RtlMachineMode::kRegDead &&
          n.kind != RtlMachineMode::kRegUnused) {
        return false;
      }
    }
    return true;
  };
  auto insn_touches_hr = [&](const RtlExpr& pat, int hr, int nw) -> bool {
    bool hit = false;
    pat.Visit([&](const RtlExpr& n) {
      if (hit || n.op != RtlOp::kReg || n.IsArgPointerReg()) {
        return;
      }
      const int nhr = eff_hr(n);
      if (nhr >= 0) {
        const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
        if (nhr < hr + nw && hr < nhr + nnw) {
          hit = true;
        }
      }
    });
    return hit;
  };
  auto insn_touches_hr_except_pseudo = [&](const RtlExpr& pat, int hr, int nw,
                                           int ignore_pseudo) -> bool {
    bool hit = false;
    pat.Visit([&](const RtlExpr& n) {
      if (hit || n.op != RtlOp::kReg || n.IsArgPointerReg() ||
          (n.IsPseudoReg() && n.RegNo() == ignore_pseudo)) {
        return;
      }
      const int nhr = eff_hr(n);
      if (nhr >= 0) {
        const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
        if (nhr < hr + nw && hr < nhr + nnw) {
          hit = true;
        }
      }
    });
    return hit;
  };
  auto is_barrier_or_branch_or_call = [](const RtlExpr& pat) -> bool {
    if (ContainsCallOp(pat) || pat.op == RtlOp::kReturn || pat.op == RtlOp::kCodeLabel ||
        pat.op == RtlOp::kBarrier || pat.op == RtlOp::kSequence) {
      return true;
    }
    const auto is_pc_set = [](const RtlExpr& s) {
      return s.IsSet() && s.SetDest().op == RtlOp::kPc;
    };
    if (is_pc_set(pat)) {
      return true;
    }
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        if (ch.op == RtlOp::kReturn || is_pc_set(ch)) {
          return true;
        }
      }
    }
    return false;
  };

  absl::flat_hash_map<int, int> demote_to_hr;
  absl::flat_hash_set<int> proven_spill_output_scratches;

  // 1. `-O0` `mulsidi3` top-of-frame `$fp` 8-byte `DImode` spill scratches.
  if (compiler_flags.opt_level == 0 && out->sideband.frame_pointer_needed &&
      out->sideband.outgoing_args_size >= 0) {
    const bool has_top_fp_off = (out->sideband.frame_size_bytes >= kStackSlotAlignmentBytes);
    const int64_t top_fp_off = static_cast<int64_t>(out->sideband.outgoing_args_size) +
                               out->sideband.frame_size_bytes - kStackSlotAlignmentBytes;
    auto match_top_di_spill_slot = [&](const RtlExpr& m) -> int {
      if (m.IsPseudoReg() && m.mode == RtlMachineMode::kDI) {
        const auto rit = out->sideband.reg_renumber.find(m.RegNo());
        if (rit != out->sideband.reg_renumber.end() && rit->second < 0) {
          return m.RegNo();
        }
      }
      if (!has_top_fp_off || m.op != RtlOp::kMem || m.flags.volatil ||
          m.mode != RtlMachineMode::kDI || m.children.size() != 1) {
        return -1;
      }
      const std::optional<int64_t> off = ExtractFpRelativeOffset(m.children[0]);
      return (off.has_value() && *off == top_fp_off) ? 0 : -1;
    };
    for (size_t b = 0; b < parsed_blocks->size(); ++b) {
      const auto& blk = (*parsed_blocks)[b];
      for (size_t i = 0; i + 2 < blk.size(); ++i) {
        const RtlExpr& mpat = blk[i].pattern;
        if (mpat.op != RtlOp::kParallel || mpat.children.empty() || !mpat.children[0].IsSet() ||
            mpat.children[0].SetDest().mode != RtlMachineMode::kDI ||
            !mpat.children[0].SetDest().IsReg() || mpat.children[0].SetSrc().op != RtlOp::kMult) {
          continue;
        }
        const int mul_r = mpat.children[0].SetDest().RegNo();
        int cand_sec_p = -1;
        int spill_tag = -1;
        size_t store_end = 0;
        const RtlExpr& s1 = blk[i + 1].pattern;
        if (s1.IsSet() && s1.SetSrc().IsReg() && s1.SetSrc().RegNo() == mul_r) {
          const int tag = match_top_di_spill_slot(s1.SetDest());
          if (tag >= 0) {
            if (mul_r >= kFirstPseudoRegister) {
              cand_sec_p = mul_r;
            }
            spill_tag = tag;
            store_end = i + 1;
          }
        }
        if (store_end == 0 && i + 3 < blk.size() && eff_hr(mpat.children[0].SetDest()) == 64 &&
            s1.IsSet() && s1.SetDest().IsPseudoReg() && s1.SetDest().mode == RtlMachineMode::kDI &&
            s1.SetSrc().IsReg() && s1.SetSrc().RegNo() == mul_r) {
          const int sec_p = s1.SetDest().RegNo();
          const RtlExpr& s2 = blk[i + 2].pattern;
          const int tag = s2.IsSet() ? match_top_di_spill_slot(s2.SetDest()) : -1;
          if (tag >= 0 && s2.SetSrc().IsPseudoReg() && s2.SetSrc().RegNo() == sec_p) {
            cand_sec_p = sec_p;
            spill_tag = tag;
            store_end = i + 2;
          }
        }
        if (store_end == 0) {
          continue;
        }
        for (size_t k = store_end + 1; k + 1 < blk.size(); ++k) {
          if (blk[k].pattern.IsSet() && blk[k].pattern.SetDest().IsReg() &&
              match_top_di_spill_slot(blk[k].pattern.SetSrc()) == spill_tag) {
            if (mul_r >= kFirstPseudoRegister &&
                is_single_block_single_def_scratch(mul_r, b, /*req_uses=*/1)) {
              demote_to_hr[mul_r] = out->sideband.reg_renumber.at(mul_r);
            }
            if (cand_sec_p >= kFirstPseudoRegister &&
                is_single_block_single_def_scratch(cand_sec_p, b, /*req_uses=*/1)) {
              demote_to_hr[cand_sec_p] = out->sideband.reg_renumber.at(cand_sec_p);
            }
            const RtlExpr& ldst = blk[k].pattern.SetDest();
            const RtlExpr& upat = blk[k + 1].pattern;
            const RtlExpr* u_set = upat.IsSet() ? &upat
                                   : (upat.op == RtlOp::kParallel && !upat.children.empty() &&
                                      upat.children[0].IsSet())
                                       ? &upat.children[0]
                                       : nullptr;
            const bool rmw_same_hr = u_set != nullptr && u_set->SetDest().IsPseudoReg() &&
                                     ldst.IsPseudoReg() && eff_hr(u_set->SetDest()) == eff_hr(ldst);
            if (!rmw_same_hr && ldst.IsPseudoReg() &&
                is_single_block_single_def_scratch(ldst.RegNo(), b, /*req_uses=*/1) &&
                upat.ContainsReg(ldst.RegNo())) {
              demote_to_hr[ldst.RegNo()] = out->sideband.reg_renumber.at(ldst.RegNo());
            }
            break;
          }
        }
      }
    }
  }

  // 2. Single-use cross-bank `"f"` FPR input reloads & `"=f"` FPR output reloads.
  for (size_t b = 0; b < parsed_blocks->size(); ++b) {
    const auto& blk = (*parsed_blocks)[b];
    for (size_t i = 0; i + 1 < blk.size(); ++i) {
      const RtlPatternAndNotes& pn = blk[i];
      if (!pn.pattern.IsSet() || !has_only_dead_or_unused_notes(pn)) {
        continue;
      }
      const RtlExpr& dst = pn.pattern.SetDest();
      const RtlExpr& src = pn.pattern.SetSrc();

      // 2a. `"f"` input reload: `(set:SF/DF p_fpr src_gpr) ... <fp_op(p_fpr)>`.
      if (dst.IsPseudoReg() &&
          (dst.mode == RtlMachineMode::kSF || dst.mode == RtlMachineMode::kDF) && src.IsReg() &&
          src.mode == dst.mode &&
          is_single_block_single_def_scratch(dst.RegNo(), b, /*req_uses=*/1)) {
        const int p_fpr = dst.RegNo();
        const int fpr_hr = out->sideband.reg_renumber.at(p_fpr);
        const int src_hr = eff_hr(src);
        if (fpr_hr >= 32 && fpr_hr <= 63 && src_hr >= 2 && src_hr <= 31) {
          const int nw = std::max(1, MipsHardRegNregs(fpr_hr, dst.mode, gp64, fp64));
          for (size_t k = i + 1; k < blk.size(); ++k) {
            if (blk[k].pattern.ContainsReg(p_fpr)) {
              const RtlExpr& up = blk[k].pattern;
              const bool is_plain_copy_or_marker =
                  up.op == RtlOp::kUse || up.op == RtlOp::kClobber ||
                  (up.IsSet() && (up.SetSrc().op == RtlOp::kReg || up.SetSrc().op == RtlOp::kMem ||
                                  up.SetDest().op == RtlOp::kMem));
              if (!is_plain_copy_or_marker && !is_barrier_or_branch_or_call(up) &&
                  (compiler_flags.opt_level == 0 || !has_reg_dead_for(blk[k], p_fpr, fpr_hr))) {
                demote_to_hr[p_fpr] = fpr_hr;
              }
              break;
            }
            if (is_barrier_or_branch_or_call(blk[k].pattern) ||
                insn_touches_hr(blk[k].pattern, fpr_hr, nw)) {
              break;
            }
          }
        }
      }

      // 2b. `opt_level > 0` `"=f"` output reload: `(set:SF/DF p_fpr <fp_op>) ... (set dst_gpr
      // p_fpr)`.
      if (compiler_flags.opt_level > 0 && dst.IsPseudoReg() &&
          (dst.mode == RtlMachineMode::kSF || dst.mode == RtlMachineMode::kDF) &&
          src.op != RtlOp::kReg && src.op != RtlOp::kMem && src.op != RtlOp::kConstDouble &&
          src.op != RtlOp::kCall && src.op != RtlOp::kSubreg &&
          is_single_block_single_def_scratch(dst.RegNo(), b, /*req_uses=*/1)) {
        const int p_fpr = dst.RegNo();
        const int fpr_hr = out->sideband.reg_renumber.at(p_fpr);
        if (fpr_hr >= 32 && fpr_hr <= 63) {
          const int nw = std::max(1, MipsHardRegNregs(fpr_hr, dst.mode, gp64, fp64));
          for (size_t k = i + 1; k < blk.size(); ++k) {
            if (blk[k].pattern.ContainsReg(p_fpr)) {
              const RtlPatternAndNotes& out_pn = blk[k];
              if (out_pn.pattern.IsSet() && out_pn.pattern.SetSrc().IsPseudoReg() &&
                  out_pn.pattern.SetSrc().RegNo() == p_fpr && out_pn.pattern.SetDest().IsReg()) {
                const int d_hr = eff_hr(out_pn.pattern.SetDest());
                const bool valid_d_gpr =
                    (d_hr >= 2 && d_hr <= 25) ||
                    (d_hr == kFramePointerRegno && !out->sideband.frame_pointer_needed);
                if (valid_d_gpr && !has_reg_dead_for(out_pn, p_fpr, fpr_hr)) {
                  demote_to_hr[p_fpr] = fpr_hr;
                }
              }
              break;
            }
            if (is_barrier_or_branch_or_call(blk[k].pattern) ||
                insn_touches_hr(blk[k].pattern, fpr_hr, nw)) {
              break;
            }
          }
        }
      }
    }
  }

  // 2c. `!gp64` `DImode` `"=&d"` earlyclobber output reload scratches.
  if (!gp64) {
    for (size_t b = 0; b < parsed_blocks->size(); ++b) {
      const auto& blk = (*parsed_blocks)[b];
      for (size_t i = 0; i + 1 < blk.size(); ++i) {
        const RtlExpr& p0 = blk[i].pattern;
        if (p0.op != RtlOp::kParallel || p0.children.size() < 2 || !p0.children[0].IsSet() ||
            p0.children[1].op != RtlOp::kClobber || !p0.children[0].SetDest().IsPseudoReg() ||
            p0.children[0].SetDest().mode != RtlMachineMode::kDI) {
          continue;
        }
        const RtlOp op0 = p0.children[0].SetSrc().op;
        if (op0 != RtlOp::kPlus && op0 != RtlOp::kMinus && op0 != RtlOp::kAshift &&
            op0 != RtlOp::kAshiftrt && op0 != RtlOp::kLshiftrt) {
          continue;
        }
        const int p_scr = p0.children[0].SetDest().RegNo();
        if (!is_single_block_single_def_scratch(p_scr, b, /*req_uses=*/1)) {
          continue;
        }
        const int scr_hr = out->sideband.reg_renumber.at(p_scr);
        if (scr_hr < 2 || scr_hr > 24) {
          continue;
        }
        for (size_t k = i + 1; k < blk.size(); ++k) {
          const RtlExpr& pk = blk[k].pattern;
          if (pk.ContainsReg(p_scr)) {
            if (pk.IsSet() && pk.SetDest().IsReg() &&
                (compiler_flags.opt_level > 0 || pk.SetDest().IsPseudoReg()) &&
                pk.SetDest().mode == RtlMachineMode::kDI && pk.SetSrc().IsPseudoReg() &&
                pk.SetSrc().RegNo() == p_scr && !has_reg_dead_for(blk[k], p_scr, scr_hr)) {
              const int dst_hr = eff_hr(pk.SetDest());
              if (dst_hr >= 2 && dst_hr <= 24 && dst_hr != scr_hr &&
                  insn_touches_hr(p0.children[0].SetSrc(), dst_hr, 2)) {
                demote_to_hr[p_scr] = scr_hr;
              }
            }
            break;
          }
          if (is_barrier_or_branch_or_call(pk) || insn_touches_hr(pk, scr_hr, 2)) {
            break;
          }
        }
      }
    }

    if (compiler_flags.opt_level == 0) {
      for (size_t b = 0; b < parsed_blocks->size(); ++b) {
        const auto& blk = (*parsed_blocks)[b];
        for (size_t i = 0; i + 1 < blk.size(); ++i) {
          const RtlPatternAndNotes& p0 = blk[i];
          const RtlPatternAndNotes& p1 = blk[i + 1];
          if (!p0.reg_notes.empty() || !p0.pattern.IsSet() || !p0.pattern.SetDest().IsPseudoReg() ||
              p0.pattern.SetDest().mode != RtlMachineMode::kDI ||
              !p0.pattern.SetSrc().IsConstInt() || p0.pattern.SetSrc().int_val != 0 ||
              !has_only_dead_or_unused_notes(p1) || !p1.pattern.IsSet() ||
              p1.pattern.SetDest().op != RtlOp::kMem ||
              p1.pattern.SetDest().mode != RtlMachineMode::kDI ||
              !p1.pattern.SetSrc().IsPseudoReg() ||
              p1.pattern.SetSrc().mode != RtlMachineMode::kDI ||
              p1.pattern.SetSrc().RegNo() != p0.pattern.SetDest().RegNo() ||
              p1.pattern.SetDest().ContainsReg(p0.pattern.SetDest().RegNo())) {
            continue;
          }
          const int p_scr = p0.pattern.SetDest().RegNo();
          if (!is_single_block_single_def_scratch(p_scr, b, /*req_uses=*/1)) {
            continue;
          }
          const int scr_hr = out->sideband.reg_renumber.at(p_scr);
          if (scr_hr >= 2 && scr_hr <= 24 && (scr_hr % 2) == 0 &&
              !has_reg_dead_for(p1, p_scr, scr_hr)) {
            demote_to_hr[p_scr] = scr_hr;
          }
        }
      }
    }
  }

  if (compiler_flags.opt_level > 0) {
    // 2d. `opt_level > 0` `movstrsi` earlyclobber unused output reload destinations.
    for (size_t b = 0; b < parsed_blocks->size(); ++b) {
      const auto& blk = (*parsed_blocks)[b];
      for (size_t i = 0; i + 1 < blk.size(); ++i) {
        const RtlExpr& p0 = blk[i].pattern;
        if (p0.op != RtlOp::kParallel || p0.children.size() < 5 || !p0.children[0].IsSet() ||
            p0.children[0].SetDest().op != RtlOp::kMem ||
            p0.children[0].SetDest().mode != RtlMachineMode::kBLK ||
            p0.children[0].SetSrc().op != RtlOp::kMem ||
            p0.children[0].SetSrc().mode != RtlMachineMode::kBLK) {
          continue;
        }
        absl::flat_hash_set<int> clob_hrs;
        for (size_t c = 1; c < p0.children.size(); ++c) {
          if (p0.children[c].op == RtlOp::kClobber && !p0.children[c].children.empty() &&
              p0.children[c].children[0].IsReg()) {
            const int chr = eff_hr(p0.children[c].children[0]);
            if (chr >= 2 && chr <= 25) {
              clob_hrs.insert(chr);
            }
          }
        }
        for (size_t k = i + 1; k < blk.size() && k <= i + 2; ++k) {
          const RtlPatternAndNotes& pnk = blk[k];
          if (!pnk.reg_notes.empty() || !pnk.pattern.IsSet() ||
              !pnk.pattern.SetDest().IsPseudoReg() || !pnk.pattern.SetSrc().IsHardReg() ||
              !clob_hrs.contains(pnk.pattern.SetSrc().RegNo())) {
            break;
          }
          const int p_dst = pnk.pattern.SetDest().RegNo();
          const auto sit = stats.find(p_dst);
          const auto rit = out->sideband.reg_renumber.find(p_dst);
          if (sit != stats.end() && rit != out->sideband.reg_renumber.end() && rit->second >= 2 &&
              rit->second <= 25 && !sit->second.multi_block && sit->second.first_block == b &&
              sit->second.def_count == 1 && sit->second.use_count == 0 && !sit->second.has_flags &&
              !sit->second.blocked) {
            demote_to_hr[p_dst] = rit->second;
          }
        }
      }
    }
  }

  // 3. `opt_level > 0` proven 8-byte big-endian `reload` stack-spill input/output scratches.
  if (compiler_flags.opt_level > 0 && out->sideband.frame_size_bytes > 0 &&
      out->sideband.outgoing_args_size >= 0) {
    const int elim_reg = out->sideband.frame_pointer_needed ? kFramePointerRegno : 29;
    const int64_t min_off = out->sideband.outgoing_args_size;
    int64_t pre_cs_top = min_off + out->sideband.frame_size_bytes;
    int64_t min_cs_seen = pre_cs_top;
    bool any_cs = false;
    for (size_t b = 0; b < parsed_blocks->size(); ++b) {
      for (size_t i = 0; i < (*parsed_blocks)[b].size(); ++i) {
        if (b < cs_save_span.size() && i < cs_save_span[b].size() && cs_save_span[b][i] > 0) {
          const RtlExpr& dst = (*parsed_blocks)[b][i].pattern.SetDest();
          if (dst.op == RtlOp::kMem && dst.children.size() == 1) {
            if (const auto off = ExtractBaseRegOffset(dst.children[0], elim_reg); off.has_value()) {
              min_cs_seen = std::min(min_cs_seen, *off);
              any_cs = true;
            }
          }
        }
      }
    }
    if (any_cs) {
      pre_cs_top = min_off + ((min_cs_seen - min_off) & ~int64_t{7});
    } else if (((pre_cs_top - min_off) % 8) == 4) {
      bool odd_word_used = false;
      for (size_t b = 0; b < parsed_blocks->size() && !odd_word_used; ++b) {
        for (const RtlPatternAndNotes& pn : (*parsed_blocks)[b]) {
          if (ExprOverlapsElimInterval(pn.pattern, elim_reg, pre_cs_top - 4, pre_cs_top)) {
            odd_word_used = true;
            break;
          }
          for (const RtlRegNote& n : pn.reg_notes) {
            if (n.list_op == RtlOp::kExprList &&
                ExprOverlapsElimInterval(n.datum, elim_reg, pre_cs_top - 4, pre_cs_top)) {
              odd_word_used = true;
              break;
            }
          }
        }
      }
      if (!odd_word_used) {
        pre_cs_top -= 4;
      }
    }

    const int64_t cand_bytes = pre_cs_top - min_off;
    if (cand_bytes >= 8 && (cand_bytes % 8) == 0) {
      const int num_slots = static_cast<int>(cand_bytes / 8);
      struct SlotInfo {
        bool invalidated = false;
        bool has_access = false;
        bool has_store = false;
        bool has_load = false;
        bool has_live_note = false;
        RtlMachineMode mode = RtlMachineMode::kNone;
        int64_t exact_off = -1;
      };
      std::vector<SlotInfo> slots(num_slots);
      int min_escaped = num_slots;
      auto inspect_slot_expr = [&](const RtlExpr& root, bool in_note, bool is_live_note) {
        auto walk = [&](const auto& self, const RtlExpr& e, bool in_addr, bool in_dst) -> void {
          if (e.op == RtlOp::kMem && e.children.size() == 1) {
            const auto off = ExtractBaseRegOffset(e.children[0], elim_reg);
            if (off.has_value() && *off >= min_off && *off < pre_cs_top) {
              const int sz = std::max(1, MipsModeByteSize(e.mode));
              const int64_t rel_lo = *off - min_off;
              const int64_t rel_hi = rel_lo + sz;
              const int fs = static_cast<int>(rel_lo / 8);
              const int ls = static_cast<int>(std::min<int64_t>(num_slots - 1, (rel_hi - 1) / 8));
              const bool is_fp_mode =
                  (e.mode == RtlMachineMode::kSF || e.mode == RtlMachineMode::kDF);
              if (e.flags.volatil || e.flags.in_struct || e.mode == RtlMachineMode::kBLK ||
                  sz > 8 || fs != ls || (compiler_flags.flag_float_store && is_fp_mode)) {
                for (int s = std::max(0, fs); s <= ls; ++s) {
                  slots[s].invalidated = true;
                }
                return;
              }
              const int64_t expected_off = min_off + 8 * int64_t{fs} + (8 - sz);
              SlotInfo& si = slots[fs];
              if (*off != expected_off) {
                si.invalidated = true;
                return;
              }
              if (!si.has_access) {
                si.has_access = true;
                si.mode = e.mode;
                si.exact_off = *off;
              } else if (si.mode != e.mode || si.exact_off != *off) {
                si.invalidated = true;
              }
              if (is_live_note) {
                si.has_live_note = true;
              } else if (!in_note) {
                if (in_dst) {
                  si.has_store = true;
                } else {
                  si.has_load = true;
                }
              }
              return;
            }
            self(self, e.children[0], true, false);
            return;
          }
          if (!in_addr) {
            const auto off = ExtractBaseRegOffset(e, elim_reg);
            if (off.has_value() && *off >= min_off && *off < pre_cs_top) {
              const int s = static_cast<int>((*off - min_off) / 8);
              if (s >= 0 && s < num_slots) {
                slots[s].invalidated = true;
                min_escaped = std::min(min_escaped, s);
              }
              return;
            }
          }
          const bool pass_dst =
              in_dst && (e.op == RtlOp::kSubreg || e.op == RtlOp::kStrictLowPart ||
                         e.op == RtlOp::kZeroExtract || e.op == RtlOp::kSignExtract);
          for (size_t ci = 0; ci < e.children.size(); ++ci) {
            self(self, e.children[ci], in_addr,
                 e.op == RtlOp::kSet ? (ci == 0) : (pass_dst && ci == 0));
          }
        };
        walk(walk, root, false, false);
      };

      for (const auto& blk : *parsed_blocks) {
        for (const RtlPatternAndNotes& pn : blk) {
          inspect_slot_expr(pn.pattern, false, false);
          for (const RtlRegNote& n : pn.reg_notes) {
            if (n.list_op == RtlOp::kExprList) {
              const bool live_n =
                  (n.kind == RtlMachineMode::kRegDead || n.kind == RtlMachineMode::kRegUnused);
              inspect_slot_expr(n.datum, true, live_n);
            }
          }
          for (const RtlExpr& fu : pn.call_fusage) {
            inspect_slot_expr(fu, false, false);
          }
        }
      }
      for (int s = min_escaped; s < num_slots; ++s) {
        if (slots[s].has_store && !slots[s].has_load && !slots[s].has_live_note) {
          slots[s].invalidated = true;
        }
      }
      int non_inv_begin = num_slots;
      while (non_inv_begin > 0 && !slots[non_inv_begin - 1].invalidated) {
        --non_inv_begin;
      }
      int first_spill = non_inv_begin;
      if (non_inv_begin > 0) {
        while (first_spill < num_slots && !slots[first_spill].has_access) {
          ++first_spill;
        }
      }
      absl::flat_hash_map<int64_t, RtlMachineMode> proven_spill_offs;
      for (int s = first_spill; s < num_slots; ++s) {
        if (slots[s].has_access) {
          proven_spill_offs[slots[s].exact_off] = slots[s].mode;
        }
      }

      auto is_proven_spill_mem = [&](const RtlExpr& m) -> bool {
        if (m.op != RtlOp::kMem || m.flags.volatil || m.flags.in_struct || m.children.size() != 1) {
          return false;
        }
        const auto off = ExtractBaseRegOffset(m.children[0], elim_reg);
        if (!off.has_value()) {
          return false;
        }
        const auto it = proven_spill_offs.find(*off);
        return it != proven_spill_offs.end() && it->second == m.mode;
      };

      for (size_t b = 0; b < parsed_blocks->size() && !proven_spill_offs.empty(); ++b) {
        const auto& blk = (*parsed_blocks)[b];
        for (size_t i = 0; i < blk.size(); ++i) {
          const RtlPatternAndNotes& pn = blk[i];
          if (!pn.pattern.IsSet()) {
            continue;
          }
          const RtlExpr& dst = pn.pattern.SetDest();
          const RtlExpr& src = pn.pattern.SetSrc();

          // Input reload load `(set p_rel slot_mem)`.
          if (pn.reg_notes.empty() && dst.IsPseudoReg() && is_proven_spill_mem(src) &&
              is_single_block_single_def_scratch(dst.RegNo(), b, /*req_uses=*/-1)) {
            const int p_rel = dst.RegNo();
            const int rel_hr = out->sideband.reg_renumber.at(p_rel);
            const ScratchCandStats& st = stats.at(p_rel);
            if (st.use_count == 1 && i + 1 < blk.size() && blk[i + 1].pattern.IsSet() &&
                blk[i + 1].pattern.SetDest().IsReg() && blk[i + 1].pattern.SetSrc().IsPseudoReg() &&
                blk[i + 1].pattern.SetSrc().RegNo() == p_rel &&
                CanDirectMoveMemAndHardReg(eff_hr(blk[i + 1].pattern.SetDest()), dst.mode)) {
              continue;
            }
            size_t last_use_idx = i;
            int seen_uses = 0;
            bool aborted = false;
            const int nw = std::max(1, MipsHardRegNregs(rel_hr, dst.mode, gp64, fp64));
            for (size_t k = i + 1; k < blk.size(); ++k) {
              int hits = 0;
              blk[k].pattern.Visit([&](const RtlExpr& n) {
                if (n.IsPseudoReg() && n.RegNo() == p_rel) {
                  ++hits;
                }
              });
              if (hits > 0) {
                seen_uses += hits;
                last_use_idx = k;
                if (seen_uses == st.use_count) {
                  break;
                }
              }
              if (is_barrier_or_branch_or_call(blk[k].pattern) ||
                  insn_touches_hr_except_pseudo(blk[k].pattern, rel_hr, nw, p_rel)) {
                aborted = true;
                break;
              }
            }
            if (!aborted && seen_uses == st.use_count && last_use_idx > i &&
                !has_reg_dead_for(blk[last_use_idx], p_rel, rel_hr)) {
              demote_to_hr[p_rel] = rel_hr;
            }
          }

          // Output reload store `(set slot_mem p_scr)` without `REG_DEAD` on `I_store`.
          if (is_proven_spill_mem(dst) && src.IsPseudoReg() && has_only_dead_or_unused_notes(pn) &&
              is_single_block_single_def_scratch(src.RegNo(), b, /*req_uses=*/-1)) {
            const int p_scr = src.RegNo();
            const int scr_hr = out->sideband.reg_renumber.at(p_scr);
            if (!has_reg_dead_for(pn, p_scr, scr_hr)) {
              const int nw = std::max(1, MipsHardRegNregs(scr_hr, src.mode, gp64, fp64));
              int pre_store_uses = 1;
              for (size_t rev = i; rev > 0; --rev) {
                const size_t d_idx = rev - 1;
                const RtlExpr& dp = blk[d_idx].pattern;
                if (dp.IsSet() && dp.SetDest().IsPseudoReg() && dp.SetDest().RegNo() == p_scr) {
                  const bool plain_direct_reg =
                      dp.SetSrc().op == RtlOp::kReg &&
                      CanDirectMoveMemAndHardReg(eff_hr(dp.SetSrc()), src.mode) &&
                      stats.at(p_scr).use_count == 1;
                  bool dying_in_place_reuse =
                      !dp.SetSrc().ContainsReg(p_scr) && has_reg_dead_for(blk[d_idx], -1, scr_hr);
                  if (!dying_in_place_reuse && !dp.SetSrc().ContainsReg(p_scr) &&
                      dp.SetSrc().op != RtlOp::kReg) {
                    bool all_overlapping_die_at_d = true;
                    bool saw_overlapping = false;
                    dp.SetSrc().Visit([&](const RtlExpr& n) {
                      if (!all_overlapping_die_at_d || n.op != RtlOp::kReg || n.IsArgPointerReg()) {
                        return;
                      }
                      const int nhr = eff_hr(n);
                      if (nhr < 0) {
                        return;
                      }
                      const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
                      if (nhr >= scr_hr + nw || scr_hr >= nhr + nnw) {
                        return;
                      }
                      saw_overlapping = true;
                      if (!n.IsPseudoReg() || n.RegNo() == p_scr) {
                        all_overlapping_die_at_d = false;
                        return;
                      }
                      const int r = n.RegNo();
                      const auto sit = stats.find(r);
                      if (sit == stats.end() || sit->second.multi_block ||
                          sit->second.first_block != b || sit->second.blocked) {
                        all_overlapping_die_at_d = false;
                        return;
                      }
                      for (size_t m = d_idx + 1; m < blk.size(); ++m) {
                        if (blk[m].pattern.ContainsReg(r)) {
                          all_overlapping_die_at_d = false;
                          return;
                        }
                      }
                    });
                    if (saw_overlapping && all_overlapping_die_at_d) {
                      dying_in_place_reuse = true;
                    }
                  }
                  bool post_store_uses_ok = (pre_store_uses == stats.at(p_scr).use_count);
                  if (!post_store_uses_ok && pre_store_uses < stats.at(p_scr).use_count) {
                    int seen_post = pre_store_uses;
                    for (size_t m = i + 1; m < blk.size(); ++m) {
                      int m_hits = 0;
                      blk[m].pattern.Visit([&](const RtlExpr& n) {
                        if (n.IsPseudoReg() && n.RegNo() == p_scr) {
                          ++m_hits;
                        }
                      });
                      if (m_hits > 0) {
                        seen_post += m_hits;
                        if (seen_post == stats.at(p_scr).use_count) {
                          post_store_uses_ok = !has_reg_dead_for(blk[m], p_scr, scr_hr);
                          break;
                        }
                      }
                      if (is_barrier_or_branch_or_call(blk[m].pattern) ||
                          insn_touches_hr_except_pseudo(blk[m].pattern, scr_hr, nw, p_scr)) {
                        break;
                      }
                    }
                  }
                  if (!plain_direct_reg && post_store_uses_ok &&
                      (!insn_touches_hr(dp.SetSrc(), scr_hr, nw) || dying_in_place_reuse)) {
                    demote_to_hr[p_scr] = scr_hr;
                    proven_spill_output_scratches.insert(p_scr);
                  }
                  break;
                }
                int mid_hits = 0;
                dp.Visit([&](const RtlExpr& n) {
                  if (n.IsPseudoReg() && n.RegNo() == p_scr) {
                    ++mid_hits;
                  }
                });
                pre_store_uses += mid_hits;
                if (is_barrier_or_branch_or_call(dp) ||
                    insn_touches_hr_except_pseudo(dp, scr_hr, nw, p_scr)) {
                  break;
                }
              }
            }
          }
        }
      }
    }
  }

  DemoteInheritedSpillOutputFprScratches(gp64, fp64, stats, proven_spill_output_scratches,
                                         *parsed_blocks, *out, &demote_to_hr);

  if (demote_to_hr.empty()) {
    return;
  }
  auto rewrite_demoted = [&](RtlExpr& r) {
    if (r.IsPseudoReg()) {
      if (const auto it = demote_to_hr.find(r.RegNo()); it != demote_to_hr.end()) {
        r.SetRegNo(it->second);
      }
    }
  };
  for (auto& blk : *parsed_blocks) {
    for (RtlPatternAndNotes& pn : blk) {
      pn.RewriteRegs(rewrite_demoted);
    }
  }
  for (const auto& [p, _] : demote_to_hr) {
    out->sideband.reg_renumber.erase(p);
  }
}

// Reverses `alter_reg` on single-block `MD_REGS` (`64 hi` / `65 lo`) pseudos
// whose sole consumer required a GPR `"d"` input reload (`mflo`/`mfhi $gpr`),
// restoring `reg_renumber[p] = 64|65` and `(set (reg:SI gpr) (reg:SI p))`.
void PromoteNonConflictingSingleBlockMdPseudos(
    const ParsedCompilerFlags& compiler_flags,
    std::vector<std::vector<RtlPatternAndNotes>>* parsed_blocks, RtlControlFlowGraph* out) {
  struct MdPseudoSummary {
    int def_count = 0;
    int use_count = 0;
    size_t first_block = 0;
    bool seen_block = false;
    bool multi_block = false;
    bool has_flags = false;
    bool blocked = false;
  };
  absl::flat_hash_map<int, MdPseudoSummary> summaries;
  for (size_t b = 0; b < parsed_blocks->size(); ++b) {
    for (const RtlPatternAndNotes& pn : (*parsed_blocks)[b]) {
      auto rec = [&](const RtlExpr& r, bool is_def, bool is_use, bool blk_flag) {
        if (!r.IsPseudoReg()) {
          return;
        }
        MdPseudoSummary& s = summaries[r.RegNo()];
        if (!s.seen_block) {
          s.seen_block = true;
          s.first_block = b;
        } else if (s.first_block != b) {
          s.multi_block = true;
        }
        if (is_def) ++s.def_count;
        if (is_use) ++s.use_count;
        if (r.flags.Any()) s.has_flags = true;
        if (blk_flag) s.blocked = true;
      };
      if (pn.pattern.op == RtlOp::kUse || pn.pattern.op == RtlOp::kSequence) {
        pn.pattern.Visit([&](const RtlExpr& n) { rec(n, false, false, true); });
      } else if (pn.pattern.IsSet()) {
        if (pn.pattern.SetDest().IsPseudoReg()) {
          rec(pn.pattern.SetDest(), true, false, false);
        } else {
          pn.pattern.SetDest().Visit([&](const RtlExpr& n) {
            rec(n, pn.pattern.SetDest().op != RtlOp::kMem, true,
                pn.pattern.SetDest().op != RtlOp::kMem);
          });
        }
        pn.pattern.SetSrc().Visit([&](const RtlExpr& n) { rec(n, false, true, false); });
      } else {
        pn.pattern.Visit([&](const RtlExpr& n) { rec(n, false, true, true); });
      }
      for (const RtlExpr& fu : pn.call_fusage) {
        fu.Visit([&](const RtlExpr& n) { rec(n, false, false, true); });
      }
      for (const RtlRegNote& note : pn.reg_notes) {
        if (note.list_op == RtlOp::kExprList && note.kind != RtlMachineMode::kRegDead &&
            note.kind != RtlMachineMode::kRegUnused) {
          note.datum.Visit([&](const RtlExpr& n) {
            if (n.IsPseudoReg()) {
              summaries[n.RegNo()].blocked = true;
            }
          });
        }
      }
    }
  }

  auto eff_hr = [&](const RtlExpr& r) -> int {
    if (r.op != RtlOp::kReg || r.IsArgPointerReg()) {
      return -1;
    }
    if (r.RegNo() < kFirstPseudoRegister) {
      return r.RegNo();
    }
    const auto it = out->sideband.reg_renumber.find(r.RegNo());
    return (it != out->sideband.reg_renumber.end()) ? it->second : -1;
  };
  auto expr_touches_md = [&](const RtlExpr& pat) -> bool {
    bool hit = false;
    pat.Visit([&](const RtlExpr& n) {
      if (hit || n.op != RtlOp::kReg || n.IsArgPointerReg()) {
        return;
      }
      const int hr = eff_hr(n);
      if (hr == 64 || hr == 65) {
        hit = true;
      }
    });
    return hit;
  };
  auto is_ctrl = [](const RtlExpr& pat) -> bool {
    if (ContainsCallOp(pat) || pat.op == RtlOp::kReturn || pat.op == RtlOp::kCodeLabel ||
        pat.op == RtlOp::kBarrier || pat.op == RtlOp::kSequence || pat.op == RtlOp::kNote) {
      return true;
    }
    if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
      return true;
    }
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        if (ch.op == RtlOp::kReturn || (ch.IsSet() && ch.SetDest().op == RtlOp::kPc)) {
          return true;
        }
      }
    }
    return false;
  };

  struct MdPromo {
    size_t prod_idx = 0;
    size_t mv_idx = 0;
    size_t use_idx = 0;
    int p_scratch = -1;
    int md_hr = -1;
    int scratch_gpr = -1;
  };

  for (size_t b = 0; b < parsed_blocks->size(); ++b) {
    auto& blk = (*parsed_blocks)[b];
    std::vector<MdPromo> promos;
    absl::flat_hash_set<size_t> covered_md_insns;
    for (size_t i = 0; i + 2 < blk.size(); ++i) {
      const RtlExpr& p0 = blk[i].pattern;
      if (p0.op != RtlOp::kParallel || p0.children.size() != 3 || !p0.children[0].IsSet() ||
          p0.children[1].op != RtlOp::kClobber || p0.children[2].op != RtlOp::kClobber ||
          !p0.children[0].SetDest().IsHardReg() ||
          p0.children[0].SetDest().mode != RtlMachineMode::kSI) {
        continue;
      }
      const int md_hr = p0.children[0].SetDest().RegNo();
      const RtlOp op0 = p0.children[0].SetSrc().op;
      if (!((md_hr == 64 && op0 == RtlOp::kTruncate) || (md_hr == 65 && op0 == RtlOp::kMult))) {
        continue;
      }
      size_t mv_idx = blk.size();
      for (size_t k = i + 1; k < blk.size(); ++k) {
        const RtlExpr& pk = blk[k].pattern;
        if (pk.IsSet() && pk.SetDest().IsPseudoReg() && pk.SetDest().mode == RtlMachineMode::kSI &&
            pk.SetSrc().IsHardReg() && pk.SetSrc().mode == RtlMachineMode::kSI &&
            pk.SetSrc().RegNo() == md_hr) {
          mv_idx = k;
          break;
        }
        if (is_ctrl(pk) || expr_touches_md(pk)) {
          break;
        }
      }
      if (mv_idx >= blk.size()) {
        continue;
      }
      bool dead_on_mv = false;
      for (const RtlRegNote& n : blk[mv_idx].reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
            n.datum.RegNo() == md_hr) {
          dead_on_mv = true;
          break;
        }
      }
      if (dead_on_mv) {
        continue;
      }
      const int p_scr = blk[mv_idx].pattern.SetDest().RegNo();
      const auto sit = summaries.find(p_scr);
      const auto rit = out->sideband.reg_renumber.find(p_scr);
      if (sit == summaries.end() || sit->second.multi_block || sit->second.first_block != b ||
          sit->second.def_count != 1 || sit->second.use_count != 1 || sit->second.has_flags ||
          sit->second.blocked || rit == out->sideband.reg_renumber.end() || rit->second < 2 ||
          rit->second > 25) {
        continue;
      }
      const int gpr = rit->second;
      size_t use_idx = blk.size();
      for (size_t k = mv_idx + 1; k < blk.size(); ++k) {
        if (blk[k].pattern.ContainsReg(p_scr)) {
          use_idx = k;
          break;
        }
        bool touches_gpr = false;
        blk[k].pattern.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kReg && !n.IsArgPointerReg() && eff_hr(n) == gpr) {
            touches_gpr = true;
          }
        });
        if (is_ctrl(blk[k].pattern) || expr_touches_md(blk[k].pattern) || touches_gpr) {
          break;
        }
      }
      if (use_idx >= blk.size() || !blk[use_idx].pattern.IsSet() || is_ctrl(blk[use_idx].pattern)) {
        continue;
      }
      const RtlExpr& up = blk[use_idx].pattern;
      const RtlOp uop = up.SetSrc().op;
      const bool non_fp =
          up.SetDest().mode != RtlMachineMode::kSF && up.SetDest().mode != RtlMachineMode::kDF &&
          up.SetSrc().mode != RtlMachineMode::kSF && up.SetSrc().mode != RtlMachineMode::kDF;
      const bool elig =
          (uop == RtlOp::kFloat && up.SetDest().IsReg() && eff_hr(up.SetDest()) >= 32 &&
           eff_hr(up.SetDest()) <= 63) ||
          (compiler_flags.opt_level > 0 && uop == RtlOp::kReg && non_fp &&
           up.SetDest().op == RtlOp::kMem && up.SetSrc().IsPseudoReg() &&
           up.SetSrc().RegNo() == p_scr) ||
          (non_fp && up.SetDest().IsReg() && uop != RtlOp::kReg && uop != RtlOp::kMem &&
           uop != RtlOp::kDiv && uop != RtlOp::kUdiv && uop != RtlOp::kMod && uop != RtlOp::kUmod &&
           uop != RtlOp::kMult && uop != RtlOp::kFloat && uop != RtlOp::kUnsignedFloat);
      if (!elig) {
        continue;
      }
      bool has_dead_p_or_gpr = false;
      for (const RtlRegNote& n : blk[use_idx].reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
            (n.datum.RegNo() == p_scr || n.datum.RegNo() == gpr)) {
          has_dead_p_or_gpr = true;
          break;
        }
      }
      if (compiler_flags.opt_level > 0 && has_dead_p_or_gpr) {
        continue;
      }
      promos.push_back(MdPromo{
          .prod_idx = i,
          .mv_idx = mv_idx,
          .use_idx = use_idx,
          .p_scratch = p_scr,
          .md_hr = md_hr,
          .scratch_gpr = gpr,
      });
      covered_md_insns.insert(i);
      covered_md_insns.insert(mv_idx);
    }
    if (promos.empty()) {
      continue;
    }
    if (compiler_flags.opt_level > 0) {
      bool other_md = false;
      for (size_t i = 0; i < blk.size(); ++i) {
        if (!covered_md_insns.contains(i) && expr_touches_md(blk[i].pattern)) {
          other_md = true;
          break;
        }
      }
      if (other_md) {
        continue;
      }
    }
    for (const MdPromo& pr : promos) {
      const RtlFlags md_flags = blk[pr.prod_idx].pattern.children[0].SetDest().flags;
      out->sideband.reg_renumber[pr.p_scratch] = pr.md_hr;
      blk[pr.prod_idx].pattern.children[0].children[0] =
          RtlExpr::MakeReg(RtlMachineMode::kSI, pr.p_scratch, md_flags);
      blk[pr.mv_idx].pattern =
          RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kSI, pr.scratch_gpr),
                           RtlExpr::MakeReg(RtlMachineMode::kSI, pr.p_scratch, md_flags));
      blk[pr.use_idx].pattern.Mutate([&](RtlExpr& n) {
        if (n.IsPseudoReg() && n.RegNo() == pr.p_scratch) {
          n.SetRegNo(pr.scratch_gpr);
        }
      });
      auto& notes = blk[pr.use_idx].reg_notes;
      notes.erase(std::remove_if(notes.begin(), notes.end(),
                                 [&](const RtlRegNote& n) {
                                   return n.kind == RtlMachineMode::kRegDead &&
                                          n.datum.op == RtlOp::kReg &&
                                          (n.datum.RegNo() == pr.p_scratch ||
                                           n.datum.RegNo() == pr.scratch_gpr);
                                 }),
                  notes.end());
      for (RtlRegNote& n : notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
            n.datum.RegNo() == pr.md_hr) {
          n.datum = RtlExpr::MakeReg(RtlMachineMode::kSI, pr.p_scratch, md_flags);
        }
      }
    }
  }
}

void UnifyO0NoConflictBlockClobberAndSelfMove(
    std::vector<std::vector<RtlPatternAndNotes>>* parsed_blocks, RtlControlFlowGraph* out) {
  for (auto& blk : *parsed_blocks) {
    for (size_t c_idx = 0; c_idx + 3 < blk.size(); ++c_idx) {
      const RtlExpr& cpat = blk[c_idx].pattern;
      if (cpat.op != RtlOp::kClobber || cpat.children.size() != 1 ||
          !cpat.children[0].IsHardReg() || cpat.children[0].mode != RtlMachineMode::kDI ||
          cpat.children[0].flags.integrated) {
        continue;
      }
      const int hr = cpat.children[0].RegNo();
      if (hr < 2 || hr > 24 || (hr % 2) != 0) {
        continue;
      }
      for (size_t k = c_idx + 1; k + 2 < blk.size(); ++k) {
        const RtlExpr& p0 = blk[k].pattern;
        const RtlExpr& p1 = blk[k + 1].pattern;
        const RtlExpr& mv = blk[k + 2].pattern;
        if (p0.IsSet() && p1.IsSet() && mv.IsSet() && p0.SetDest().IsHardReg() &&
            p1.SetDest().IsHardReg() && p0.SetDest().mode == RtlMachineMode::kSI &&
            p1.SetDest().mode == RtlMachineMode::kSI && mv.SetDest().IsPseudoReg() &&
            mv.SetDest().mode == RtlMachineMode::kDI && mv.SetSrc().IsHardReg() &&
            mv.SetSrc().mode == RtlMachineMode::kDI && mv.SetSrc().RegNo() == hr) {
          const int r0 = p0.SetDest().RegNo();
          const int r1 = p1.SetDest().RegNo();
          const int p = mv.SetDest().RegNo();
          const auto rit = out->sideband.reg_renumber.find(p);
          if (((r0 == hr && r1 == hr + 1) || (r0 == hr + 1 && r1 == hr)) &&
              rit != out->sideband.reg_renumber.end() && rit->second == hr) {
            const RtlExpr p_reg = RtlExpr::MakeReg(RtlMachineMode::kDI, p, mv.SetDest().flags);
            blk[c_idx].pattern.children[0] = p_reg;
            blk[k + 2].pattern.children[1] = p_reg;
            c_idx = k + 2;
            break;
          }
        }
        if (ContainsCallOp(p0) || p0.op == RtlOp::kCodeLabel || p0.op == RtlOp::kBarrier ||
            (p0.IsSet() && p0.SetDest().op == RtlOp::kPc) || p0.ContainsReg(hr) ||
            p0.ContainsReg(hr + 1)) {
          break;
        }
      }
    }
  }
}

absl::flat_hash_set<int64_t> CollectReloadSpillSlotOffsets(
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const std::vector<std::vector<int>>& cs_save_span,
    const std::vector<std::vector<int>>& cs_restore_span, int base_regno, int frame_size_bytes,
    int outgoing_args_size) {
  absl::flat_hash_set<int64_t> result;
  if (frame_size_bytes <= 0) {
    return result;
  }
  const int64_t min_off = std::max(0, outgoing_args_size);
  const int64_t max_off = min_off + frame_size_bytes;
  absl::flat_hash_map<int64_t, int> stored_off_bytes;
  absl::flat_hash_set<int64_t> loaded_word_offs;
  int64_t min_escaped_off = max_off;

  auto scan_expr = [&](auto& self, const RtlExpr& expr, bool in_set_dest) -> void {
    if (expr.op == RtlOp::kSequence) {
      for (const RtlExpr& sub : expr.children) {
        if (!sub.children.empty()) {
          self(self, sub.children[0], /*in_set_dest=*/false);
        }
      }
      return;
    }
    if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
      if (expr.children[0].op == RtlOp::kMem && !expr.children[0].flags.volatil &&
          !expr.children[0].flags.in_struct && expr.children[0].mode != RtlMachineMode::kBlk &&
          expr.children[0].children.size() == 1 && expr.children[1].op == RtlOp::kReg &&
          expr.children[1].RegNo() >= 0 && expr.children[1].RegNo() < kFirstPseudoRegister) {
        const std::optional<int64_t> off =
            ExtractBaseRelativeConstOffset(expr.children[0].children[0], base_regno);
        if (off.has_value() && *off >= min_off && *off < max_off) {
          const int sz = std::max(4, MipsModeByteSize(expr.children[0].mode));
          stored_off_bytes[*off] = std::max(stored_off_bytes[*off], sz);
        }
      }
      self(self, expr.children[0], /*in_set_dest=*/true);
      self(self, expr.children[1], /*in_set_dest=*/false);
      return;
    }
    if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
      const std::optional<int64_t> off =
          ExtractBaseRelativeConstOffset(expr.children[0], base_regno);
      if (off.has_value()) {
        if (!in_set_dest && !expr.flags.volatil && !expr.flags.in_struct &&
            expr.mode != RtlMachineMode::kBlk && *off >= min_off && *off < max_off) {
          const int sz = std::max(4, MipsModeByteSize(expr.mode));
          for (int k = 0; k < sz; k += 4) {
            loaded_word_offs.insert(*off + k);
          }
        }
        return;
      }
    }
    if (expr.op == RtlOp::kPlus && expr.children.size() == 2) {
      const std::optional<int64_t> off = ExtractBaseRelativeConstOffset(expr, base_regno);
      if (off.has_value() && *off < max_off) {
        min_escaped_off = std::min(min_escaped_off, std::max(min_off, *off));
        return;
      }
    }
    for (const RtlExpr& ch : expr.children) {
      self(self, ch, /*in_set_dest=*/false);
    }
  };

  for (size_t b = 0; b < parsed_blocks.size(); ++b) {
    for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
      if (cs_save_span[b][i] > 0 || cs_restore_span[b][i] > 0) {
        continue;
      }
      scan_expr(scan_expr, parsed_blocks[b][i].pattern, /*in_set_dest=*/false);
    }
  }

  for (const auto& [off, sz] : stored_off_bytes) {
    if (off + sz > min_escaped_off) {
      continue;
    }
    bool any_loaded = false;
    for (int k = 0; k < sz; k += 4) {
      if (loaded_word_offs.contains(off + k)) {
        any_loaded = true;
        break;
      }
    }
    if (any_loaded) {
      for (int k = 0; k < sz; k += 4) {
        result.insert(off + k);
      }
    }
  }
  return result;
}

void PostProcessPreAlterCfg(const CompilationProfile& profile,
                            const ParsedCompilerFlags& compiler_flags, bool gp64, bool fp64,
                            int first_pseudo_regno, int input_max_regno,
                            const std::vector<std::vector<int>>& cs_save_span,
                            std::vector<std::vector<RtlPatternAndNotes>>* parsed_blocks,
                            RtlControlFlowGraph* out) {
  DemoteReloadAndCallerSaveHardScratches(compiler_flags, gp64, fp64, cs_save_span, parsed_blocks,
                                         out);
  CoalesceSameRegRenumberRmwPseudos(profile, compiler_flags, gp64, fp64, out, parsed_blocks);
  if (compiler_flags.opt_level == 0) {
    CoalesceO0ExhaustedCalleeSavedFprPseudos(parsed_blocks, out);
    UnifyO0NoConflictBlockClobberAndSelfMove(parsed_blocks, out);
  } else {
    CoalesceDisjointMultiBlockSameRegPseudos(profile, compiler_flags, out, parsed_blocks);
    DemoteFalsePreferenceConflictPseudos(profile, compiler_flags, gp64, fp64, out, parsed_blocks);
  }
  PromoteNonConflictingSingleBlockMdPseudos(compiler_flags, parsed_blocks, out);
  CompactAndRefreshPreAlterSideband(first_pseudo_regno, input_max_regno, compiler_flags.opt_level,
                                    parsed_blocks, out);
}

}  // namespace

RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg,
                                   int first_pseudo_regno) {
  const CompilationProfile default_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  return InvertAlterReg(allocated_cfg, default_profile, first_pseudo_regno);
}

RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg,
                                   const CompilationProfile& profile, int first_pseudo_regno) {
  if (allocated_cfg.rpo_blocks.empty() || first_pseudo_regno < kFirstPseudoRegister) {
    return RtlControlFlowGraph{};
  }

  const size_t num_blocks = allocated_cfg.rpo_blocks.size();
  std::vector<std::vector<RtlPatternAndNotes>> parsed_blocks(num_blocks);
  int max_existing_regno = first_pseudo_regno - 1;

  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = allocated_cfg.rpo_blocks[b];
    if (blk.id != static_cast<uint32_t>(b)) {
      return RtlControlFlowGraph{};
    }
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return RtlControlFlowGraph{};
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return RtlControlFlowGraph{};
      }
    }
    parsed_blocks[b].reserve(blk.instructions.size());
    for (size_t k = 0; k < blk.instructions.size(); ++k) {
      const std::string& pat = blk.instructions[k];
      if (pat.empty()) {
        return RtlControlFlowGraph{};
      }
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok()) {
        return RtlControlFlowGraph{};
      }
      if (k < blk.rtl_insns.size() && blk.rtl_insns[k].has_explicit_call_fusage &&
          !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = blk.rtl_insns[k].call_fusage;
      }
      if (!ValidateExprStructure(parsed->pattern, num_blocks)) {
        return RtlControlFlowGraph{};
      }
      RecordMaxRegNo(parsed->pattern, &max_existing_regno);
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kUnknown ||
            (note.list_op == RtlOp::kExprList && !ValidateExprStructure(note.datum, num_blocks))) {
          return RtlControlFlowGraph{};
        }
        if (note.list_op == RtlOp::kExprList) {
          RecordMaxRegNo(note.datum, &max_existing_regno);
        }
      }
      parsed_blocks[b].push_back(*std::move(parsed));
    }
  }

  std::optional<RtlPatternAndNotes> epilogue_delay_parsed;
  if (allocated_cfg.sideband.epilogue_delay_pattern.has_value() &&
      !allocated_cfg.sideband.epilogue_delay_pattern->empty()) {
    auto ep_parsed = ParsePatternAndNotes(*allocated_cfg.sideband.epilogue_delay_pattern);
    if (!ep_parsed.ok() || !ValidateExprStructure(ep_parsed->pattern, num_blocks)) {
      return RtlControlFlowGraph{};
    }
    RecordMaxRegNo(ep_parsed->pattern, &max_existing_regno);
    for (const RtlRegNote& note : ep_parsed->reg_notes) {
      if (note.kind == RtlMachineMode::kUnknown ||
          (note.list_op == RtlOp::kExprList && !ValidateExprStructure(note.datum, num_blocks))) {
        return RtlControlFlowGraph{};
      }
      if (note.list_op == RtlOp::kExprList) {
        RecordMaxRegNo(note.datum, &max_existing_regno);
      }
    }
    epilogue_delay_parsed = *std::move(ep_parsed);
  }

  const ParsedCompilerFlags compiler_flags = ParseCompilationProfileFlags(profile);
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(allocated_cfg, &profile, &gp64, &fp64);

  // Reconstruct missing `call_fusage` on a read-only analysis copy so synthetic
  // `CALL_INSN` inputs without explicit `call_fusage` still preserve outgoing
  // ABI argument registers without altering `has_explicit_call_fusage` on `out`.
  RtlControlFlowGraph fusage_cfg = allocated_cfg;
  for (RtlBasicBlock& blk : fusage_cfg.rpo_blocks) {
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }
  ReconstructMissingCallFusage(&fusage_cfg, profile);

  auto effective_call_fusage = [&](size_t b, size_t i,
                                   const RtlPatternAndNotes& pn) -> std::vector<RtlExpr> {
    if (pn.has_explicit_call_fusage) {
      return pn.call_fusage;
    }
    if (pn.pattern.op == RtlOp::kSequence && !pn.pattern.children.empty() &&
        pn.pattern.children[0].op == RtlOp::kCallInsn) {
      auto sub = RtlPatternAndNotes::FromSequenceSubInsn(pn.pattern.children[0]);
      if (sub.ok() && sub->has_explicit_call_fusage) {
        return sub->call_fusage;
      }
    }
    if (i < fusage_cfg.rpo_blocks[b].rtl_insns.size() &&
        fusage_cfg.rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage) {
      return fusage_cfg.rpo_blocks[b].rtl_insns[i].call_fusage;
    }
    return {};
  };

  // Precompute which instructions sit in the exit tail of an exit block (for
  // recognizing unflagged `(use (reg:M 2 v0))` return uses in synthetic tests).
  std::vector<std::vector<bool>> is_exit_tail(num_blocks);
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = allocated_cfg.rpo_blocks[b];
    const size_t n = parsed_blocks[b].size();
    is_exit_tail[b].assign(n, false);
    if (blk.successors.empty() && blk.addr_vec_targets.empty()) {
      for (size_t rev = n; rev > 0; --rev) {
        const size_t idx = rev - 1;
        if (!IsTrivialExitTailInsn(parsed_blocks[b][idx].pattern)) {
          break;
        }
        is_exit_tail[b][idx] = true;
      }
    }
  }

  // Pass 1: Register all candidate hard-register AST occurrences with loop-depth
  // reference weights, and analyze `$fp`-relative 8-byte `alter_reg` stack slots
  // strictly when `opt_level == 0 && frame_pointer_needed && !float_store` (at
  // `opt_level > 0`, `reload` owns stack-slot elimination into `InvertReload`).
  std::vector<RegOccurrence> occs;
  absl::flat_hash_map<const RtlExpr*, int> expr_to_occ;

  const int frame_size_bytes = allocated_cfg.sideband.frame_size_bytes;
  const int outgoing_args_size = allocated_cfg.sideband.outgoing_args_size;
  const bool fp_is_fixed =
      allocated_cfg.sideband.frame_pointer_needed || !compiler_flags.flag_omit_frame_pointer;
  const bool float_store = compiler_flags.flag_float_store;
  const bool can_check_stack_slots =
      compiler_flags.opt_level == 0 && allocated_cfg.sideband.frame_pointer_needed &&
      !float_store && outgoing_args_size >= 0 && frame_size_bytes >= kStackSlotAlignmentBytes &&
      (frame_size_bytes % kStackSlotAlignmentBytes) == 0;
  absl::flat_hash_map<int, StackSlotStats> stack_slots;
  absl::flat_hash_set<int> invalidated_slots;
  std::array<bool, kFirstPseudoRegister> b0_written_hard_regs{};

  std::vector<std::vector<int>> cs_save_span;
  std::vector<std::vector<int>> cs_restore_span;
  PrecomputeCallerSavePairs(allocated_cfg, compiler_flags, gp64, fp64, parsed_blocks, &cs_save_span,
                            &cs_restore_span);

  int loop_depth = 1;
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = allocated_cfg.rpo_blocks[b];
    for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
      if (auto lk = ExtractLoopNoteKind(blk, i, parsed_blocks[b][i].pattern); lk.has_value()) {
        if (*lk == RtlLoopNoteKind::kLoopBeg) {
          ++loop_depth;
        } else if (*lk == RtlLoopNoteKind::kLoopEnd) {
          loop_depth = std::max(1, loop_depth - 1);
        }
      }
      const int ref_weight = (compiler_flags.opt_level > 0) ? loop_depth : 1;
      const bool lift_di_clobber = (compiler_flags.opt_level == 0);
      const RtlPatternAndNotes& pn = parsed_blocks[b][i];
      if (cs_save_span[b][i] > 0 || cs_restore_span[b][i] > 0) {
        continue;
      }
      if (pn.pattern.op == RtlOp::kSequence) {
        for (const RtlExpr& sub : pn.pattern.children) {
          if (!sub.children.empty()) {
            RegisterPatternOccurrences(sub.children[0], /*in_set_dest=*/false,
                                       /*in_clobber=*/false, ref_weight, gp64, fp64, fp_is_fixed,
                                       lift_di_clobber, &occs, &expr_to_occ);
          }
        }
      } else {
        RegisterPatternOccurrences(pn.pattern, /*in_set_dest=*/false, /*in_clobber=*/false,
                                   ref_weight, gp64, fp64, fp_is_fixed, lift_di_clobber, &occs,
                                   &expr_to_occ);
        if (pn.pattern.op == RtlOp::kUse && pn.pattern.children.size() == 1) {
          const int u_occ = LookupOccId(pn.pattern.children[0], expr_to_occ);
          if (u_occ >= 0) {
            occs[u_occ].is_top_level_use = true;
          }
        }
      }

      if (can_check_stack_slots) {
        AnalyzeFpStackSlots(pn.pattern, /*in_set_dest=*/false, /*in_note=*/false, ref_weight,
                            frame_size_bytes, outgoing_args_size, &stack_slots, &invalidated_slots);
        if (b == 0) {
          MarkEntryArgFpSlotAndStepB0Writes(pn.pattern, gp64, fp64, frame_size_bytes,
                                            outgoing_args_size, &b0_written_hard_regs,
                                            &stack_slots);
        }
        for (const RtlRegNote& note : pn.reg_notes) {
          if (note.list_op == RtlOp::kExprList) {
            AnalyzeFpStackSlots(note.datum, /*in_set_dest=*/false, /*in_note=*/true, ref_weight,
                                frame_size_bytes, outgoing_args_size, &stack_slots,
                                &invalidated_slots);
          }
        }
      }
    }
  }

  if (epilogue_delay_parsed.has_value()) {
    RegisterPatternOccurrences(epilogue_delay_parsed->pattern, /*in_set_dest=*/false,
                               /*in_clobber=*/false, /*ref_weight=*/1, gp64, fp64, fp_is_fixed,
                               /*lift_di_clobber=*/(compiler_flags.opt_level == 0), &occs,
                               &expr_to_occ);
    if (can_check_stack_slots) {
      AnalyzeFpStackSlots(epilogue_delay_parsed->pattern, /*in_set_dest=*/false, /*in_note=*/false,
                          /*ref_weight=*/1, frame_size_bytes, outgoing_args_size, &stack_slots,
                          &invalidated_slots);
    }
  }

  std::vector<std::vector<bool>> is_o0_scope_start_use;
  std::vector<std::vector<bool>> suppress_exit_use_pin;
  PrecomputeO0ScopeUseFlags(parsed_blocks, is_exit_tail, compiler_flags.opt_level, gp64, fp64,
                            &is_o0_scope_start_use, &suppress_exit_use_pin);

  std::vector<std::vector<bool>> is_paired_libcall;
  std::vector<std::vector<bool>> is_paired_retval;
  PrecomputePairedLibcallScopes(parsed_blocks, occs, expr_to_occ, &is_paired_libcall,
                                &is_paired_retval);

  // Pass 2: Forward reaching-definitions fixed-point dataflow across `rpo_blocks`.
  std::vector<ReachState> reach_in(num_blocks);
  for (int w = 0; w < kFirstPseudoRegister; ++w) {
    if (IsIncomingArgumentHardReg(w) || (!fp_is_fixed && w == kFramePointerRegno)) {
      reach_in[0].defs[w].push_back(kPinnedAbiToken);
    } else if (IsAllocatableHardReg(w, fp_is_fixed)) {
      reach_in[0].defs[w].push_back(kUninitEntryToken);
    }
  }

  auto step_insn_or_sequence = [&](size_t b, size_t i, ReachState* state, ReachState* cs_saved,
                                   std::vector<int>* open_libcall_clobbers, WebCollector* collector,
                                   InsnNoteContext* top_note_ctx,
                                   std::vector<InsnNoteContext>* seq_sub_note_ctxs,
                                   ReachState* out_annulled_fallthrough_state,
                                   bool* out_has_annulled_split) {
    *out_has_annulled_split = false;
    const RtlPatternAndNotes& pn = parsed_blocks[b][i];
    const bool has_libcall_note = HasRegNoteKind(pn, RtlMachineMode::kRegLibcall);
    if (is_paired_libcall[b][i]) {
      open_libcall_clobbers->push_back(
          ExtractStandaloneClobberTargetOccId(pn.pattern, expr_to_occ));
    }
    auto pop_libcall_if_retval = [&]() {
      if (is_paired_retval[b][i] && !open_libcall_clobbers->empty()) {
        open_libcall_clobbers->pop_back();
      }
    };

    if (cs_save_span[b][i] > 0) {
      if (top_note_ctx != nullptr) {
        top_note_ctx->pre_reach = *state;
        top_note_ctx->post_reach = *state;
      }
      const int hr = pn.pattern.SetSrc().RegNo();
      for (int k = 0; k < cs_save_span[b][i] && hr + k < kFirstPseudoRegister; ++k) {
        cs_saved->defs[hr + k] = state->defs[hr + k];
        cs_saved->partial_overwriters[hr + k] = state->partial_overwriters[hr + k];
      }
      pop_libcall_if_retval();
      return;
    }
    if (cs_restore_span[b][i] > 0) {
      if (top_note_ctx != nullptr) {
        top_note_ctx->pre_reach = *state;
      }
      const int hr = pn.pattern.SetDest().RegNo();
      for (int k = 0; k < cs_restore_span[b][i] && hr + k < kFirstPseudoRegister; ++k) {
        state->defs[hr + k] = cs_saved->defs[hr + k];
        state->partial_overwriters[hr + k] = cs_saved->partial_overwriters[hr + k];
      }
      if (top_note_ctx != nullptr) {
        top_note_ctx->post_reach = *state;
      }
      pop_libcall_if_retval();
      return;
    }

    const std::vector<RtlExpr> fusage = effective_call_fusage(b, i, pn);
    const bool kill_dead_hard_regs =
        compiler_flags.opt_level > 0 && open_libcall_clobbers->empty() && !is_paired_retval[b][i];
    if (pn.pattern.op != RtlOp::kSequence) {
      StepSinglePatternForward(pn.pattern, pn.reg_notes, kill_dead_hard_regs, fusage,
                               is_exit_tail[b][i], has_libcall_note, *open_libcall_clobbers, gp64,
                               fp64, state, occs, expr_to_occ, collector, top_note_ctx,
                               is_o0_scope_start_use[b][i], suppress_exit_use_pin[b][i]);
      bool is_o0_di_self_move_after_init = false;
      if (compiler_flags.opt_level == 0 && i > 0 && pn.pattern.op == RtlOp::kSet &&
          pn.pattern.children.size() == 2 && pn.pattern.children[0].IsHardReg() &&
          pn.pattern.children[1].IsHardReg() && !pn.pattern.children[0].flags.integrated &&
          pn.pattern.children[0].mode == RtlMachineMode::kDi &&
          pn.pattern.children[0].RegNo() == pn.pattern.children[1].RegNo()) {
        const int hr = pn.pattern.children[0].RegNo();
        const RtlExpr& prev_pat = parsed_blocks[b][i - 1].pattern;
        if (prev_pat.op == RtlOp::kSet && prev_pat.children.size() == 2) {
          if (prev_pat.children[0].op == RtlOp::kSubreg && !prev_pat.children[0].children.empty() &&
              prev_pat.children[0].children[0].IsHardReg() &&
              prev_pat.children[0].children[0].RegNo() == hr) {
            is_o0_di_self_move_after_init = true;
          } else if (prev_pat.children[0].IsHardReg() && prev_pat.children[0].RegNo() == hr &&
                     prev_pat.children[1].IsHardReg() && prev_pat.children[1].RegNo() == 2 &&
                     prev_pat.children[1].mode == RtlMachineMode::kDi) {
            is_o0_di_self_move_after_init = true;
          }
        }
      }
      if (collector != nullptr && pn.pattern.op == RtlOp::kSet && pn.pattern.children.size() == 2 &&
          ((i > 0 && HasRegNoteKind(parsed_blocks[b][i - 1], RtlMachineMode::kRegRetval)) ||
           is_o0_di_self_move_after_init ||
           (compiler_flags.opt_level == 0 && HasRegNoteKind(pn, RtlMachineMode::kRegEqual) &&
            !HasRegNoteKind(pn, RtlMachineMode::kRegRetval)))) {
        const int dst_occ = LookupOccId(pn.pattern.children[0], expr_to_occ);
        const int src_occ = LookupOccId(pn.pattern.children[1], expr_to_occ);
        if (dst_occ >= 0 && src_occ >= 0 && occs[dst_occ].sig == occs[src_occ].sig) {
          collector->uf->Union(src_occ, dst_occ);
          collector->AddPinEdge(src_occ, dst_occ);
        }
      }
      pop_libcall_if_retval();
      return;
    }

    if (top_note_ctx != nullptr) {
      top_note_ctx->pre_reach = *state;
    }
    const size_t num_subs = pn.pattern.children.size();
    if (seq_sub_note_ctxs != nullptr) {
      seq_sub_note_ctxs->resize(num_subs);
    }

    if (pn.pattern.children[0].op == RtlOp::kCallInsn) {
      const RtlExpr& call_pat = pn.pattern.children[0].children[0];
      std::vector<int> call_reads;
      std::vector<int> call_writes;
      const ReachState pre_call_reach = *state;
      StepCallTargetReadsInPattern(call_pat, gp64, fp64, state, expr_to_occ, collector,
                                   &call_reads);
      for (size_t s = 1; s < num_subs; ++s) {
        InsnNoteContext* sub_ctx =
            (seq_sub_note_ctxs != nullptr) ? &(*seq_sub_note_ctxs)[s] : nullptr;
        StepSinglePatternForward(pn.pattern.children[s].children[0], {},
                                 /*kill_dead_hard_regs=*/false, {}, /*is_exit_tail_use=*/false,
                                 /*has_libcall_note=*/false, *open_libcall_clobbers, gp64, fp64,
                                 state, occs, expr_to_occ, collector, sub_ctx);
      }
      ApplyCallFusageAndCallClobbers(fusage, gp64, fp64, state, collector);
      StepCallReturnWritesInPattern(call_pat, gp64, fp64, state, occs, expr_to_occ, collector,
                                    &call_writes);
      if (seq_sub_note_ctxs != nullptr) {
        (*seq_sub_note_ctxs)[0].pre_reach = pre_call_reach;
        (*seq_sub_note_ctxs)[0].post_reach = *state;
        (*seq_sub_note_ctxs)[0].read_occs.assign(call_reads.begin(), call_reads.end());
        (*seq_sub_note_ctxs)[0].write_occs.assign(call_writes.begin(), call_writes.end());
      }
    } else {
      const bool is_annulled_target =
          num_subs == 2 && pn.pattern.children[0].op == RtlOp::kJumpInsn &&
          pn.pattern.children[0].flags.unchanging && pn.pattern.children[1].flags.in_struct &&
          i + 1 == parsed_blocks[b].size();
      InsnNoteContext* head_ctx =
          (seq_sub_note_ctxs != nullptr) ? &(*seq_sub_note_ctxs)[0] : nullptr;
      StepSinglePatternForward(pn.pattern.children[0].children[0], {},
                               /*kill_dead_hard_regs=*/false, {}, /*is_exit_tail_use=*/false,
                               /*has_libcall_note=*/false, *open_libcall_clobbers, gp64, fp64,
                               state, occs, expr_to_occ, collector, head_ctx);
      if (is_annulled_target && out_annulled_fallthrough_state != nullptr) {
        *out_annulled_fallthrough_state = *state;
        *out_has_annulled_split = true;
      }
      for (size_t s = 1; s < num_subs; ++s) {
        InsnNoteContext* sub_ctx =
            (seq_sub_note_ctxs != nullptr) ? &(*seq_sub_note_ctxs)[s] : nullptr;
        StepSinglePatternForward(pn.pattern.children[s].children[0], {},
                                 /*kill_dead_hard_regs=*/false, {}, /*is_exit_tail_use=*/false,
                                 /*has_libcall_note=*/false, *open_libcall_clobbers, gp64, fp64,
                                 state, occs, expr_to_occ, collector, sub_ctx);
      }
    }
    if (top_note_ctx != nullptr) {
      top_note_ctx->post_reach = *state;
    }
    pop_libcall_if_retval();
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < num_blocks; ++b) {
      ReachState cur = reach_in[b];
      ReachState cs_saved{};
      std::vector<int> open_libcall_clobbers;
      ReachState ft_state{};
      bool has_annulled_split = false;
      for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
        step_insn_or_sequence(b, i, &cur, &cs_saved, &open_libcall_clobbers, /*collector=*/nullptr,
                              /*top_note_ctx=*/nullptr, /*seq_sub_note_ctxs=*/nullptr, &ft_state,
                              &has_annulled_split);
      }
      const RtlBasicBlock& blk = allocated_cfg.rpo_blocks[b];
      for (size_t succ : blk.successors) {
        const ReachState& edge_state = (has_annulled_split && succ == b + 1) ? ft_state : cur;
        if (MergeReachState(edge_state, &reach_in[succ])) {
          changed = true;
        }
      }
      for (size_t target : blk.addr_vec_targets) {
        if (MergeReachState(cur, &reach_in[target])) {
          changed = true;
        }
      }
    }
  }

  // Pass 3: Single converged forward sweep collecting def-use web unifications,
  // ABI pin seeds, and per-instruction pre/post reaching states for `REG_NOTES`.
  const int spill_base_regno =
      allocated_cfg.sideband.frame_pointer_needed ? kFramePointerRegno : 29;
  const absl::flat_hash_set<int64_t> reload_spill_offsets =
      (compiler_flags.opt_level > 0)
          ? CollectReloadSpillSlotOffsets(parsed_blocks, cs_save_span, cs_restore_span,
                                          spill_base_regno, frame_size_bytes, outgoing_args_size)
          : absl::flat_hash_set<int64_t>{};
  WebUnionFind uf(occs.size());
  std::vector<bool> pinned_seed(occs.size(), false);
  std::vector<bool> stored_to_spill_slot(occs.size(), false);
  std::vector<absl::InlinedVector<int, 4>> pin_adj(occs.size());
  absl::flat_hash_map<RegWebSignature, int> uninit_anchors;
  absl::flat_hash_map<RegWebSignature, int> latest_set_occ_by_sig;
  WebCollector collector{
      .occs = &occs,
      .expr_to_occ = &expr_to_occ,
      .uf = &uf,
      .pinned_seed = &pinned_seed,
      .pin_adj = &pin_adj,
      .uninit_anchors = &uninit_anchors,
      .latest_set_occ_by_sig = (compiler_flags.opt_level == 0) ? &latest_set_occ_by_sig : nullptr,
      .stored_to_spill_slot = (compiler_flags.opt_level > 0) ? &stored_to_spill_slot : nullptr,
      .reload_spill_offsets = (compiler_flags.opt_level > 0) ? &reload_spill_offsets : nullptr,
      .spill_base_regno = spill_base_regno,
      .is_optimized = (compiler_flags.opt_level > 0),
  };

  std::vector<std::vector<InsnNoteContext>> block_note_ctxs(num_blocks);
  std::vector<std::vector<std::vector<InsnNoteContext>>> block_seq_note_ctxs(num_blocks);
  InsnNoteContext epilogue_delay_note_ctx;
  bool has_epilogue_delay_ctx = false;

  for (size_t b = 0; b < num_blocks; ++b) {
    ReachState cur = reach_in[b];
    ReachState cs_saved{};
    std::vector<int> open_libcall_clobbers;
    const size_t n = parsed_blocks[b].size();
    block_note_ctxs[b].resize(n);
    block_seq_note_ctxs[b].resize(n);
    ReachState unused_ft{};
    bool unused_split = false;
    for (size_t i = 0; i < n; ++i) {
      step_insn_or_sequence(b, i, &cur, &cs_saved, &open_libcall_clobbers, &collector,
                            &block_note_ctxs[b][i], &block_seq_note_ctxs[b][i], &unused_ft,
                            &unused_split);
    }
    const RtlBasicBlock& blk = allocated_cfg.rpo_blocks[b];
    if (epilogue_delay_parsed.has_value() && blk.successors.empty() &&
        blk.addr_vec_targets.empty()) {
      ReachState ep_state = cur;
      InsnNoteContext ep_ctx;
      StepSinglePatternForward(epilogue_delay_parsed->pattern, epilogue_delay_parsed->reg_notes,
                               /*kill_dead_hard_regs=*/false, {}, /*is_exit_tail_use=*/false,
                               /*has_libcall_note=*/false, {}, gp64, fp64, &ep_state, occs,
                               expr_to_occ, &collector, &ep_ctx);
      if (!has_epilogue_delay_ctx) {
        epilogue_delay_note_ctx = std::move(ep_ctx);
        has_epilogue_delay_ctx = true;
      } else {
        MergeReachState(ep_ctx.pre_reach, &epilogue_delay_note_ctx.pre_reach);
        MergeReachState(ep_ctx.post_reach, &epilogue_delay_note_ctx.post_reach);
      }
    }
  }

  // Propagate `pinned_occ` across all overlapping `(def, use)` pairs and union-find
  // equivalence classes so connected ABI webs stay hardware registers uniformly.
  absl::flat_hash_map<int, std::vector<int>> members_by_root;
  members_by_root.reserve(occs.size());
  for (size_t i = 0; i < occs.size(); ++i) {
    members_by_root[uf.Find(static_cast<int>(i))].push_back(static_cast<int>(i));
  }

  // At `-O0` (`obey_regdecls`), `expand_decl` (`stmt.c:3499`) always emits a
  // leading `(use (reg/v:M p))` before any `SET` of a normal `register` `VAR_DECL`,
  // whereas explicit `register ... asm("$reg")` `VAR_DECL`s bypass `expand_decl`
  // (`make_decl_rtl`) while still receiving `expand_end_bindings` (`stmt.c:3271`)
  // trailing `(use (reg/v:M hr))`. Pin any `reg/v` web whose first occurrence is
  // a `SET` and that later has a top-level `(use (reg/v:M hr))`.
  if (compiler_flags.opt_level == 0) {
    for (const auto& [root, members] : members_by_root) {
      if (members.empty() || !occs[members[0]].sig.volatil) {
        continue;
      }
      if (occs[members[0]].set_count > 0 && !occs[members[0]].is_top_level_use) {
        const bool has_later_scope_use = std::any_of(
            members.begin() + 1, members.end(), [&](int m) { return occs[m].is_top_level_use; });
        if (has_later_scope_use) {
          pinned_seed[members[0]] = true;
        }
      }
    }
  }

  std::vector<bool> pinned_occ = pinned_seed;
  std::vector<bool> visited_root(occs.size(), false);
  std::vector<int> bfs_queue;
  for (size_t i = 0; i < occs.size(); ++i) {
    if (pinned_occ[i]) {
      bfs_queue.push_back(static_cast<int>(i));
    }
  }
  for (size_t head = 0; head < bfs_queue.size(); ++head) {
    const int cur_occ = bfs_queue[head];
    const int root = uf.Find(cur_occ);
    if (!visited_root[root]) {
      visited_root[root] = true;
      for (int member : members_by_root[root]) {
        if (!pinned_occ[member]) {
          pinned_occ[member] = true;
          bfs_queue.push_back(member);
        }
      }
    }
    for (int neighbor : pin_adj[cur_occ]) {
      if (!pinned_occ[neighbor]) {
        pinned_occ[neighbor] = true;
        bfs_queue.push_back(neighbor);
      }
    }
  }

  // Pass 4: Aggregate web statistics and assign fresh pseudo-register numbers
  // (`>= first_pseudo_regno`) in deterministic RPO encounter order.
  std::vector<int> root_order;
  absl::flat_hash_map<int, WebComponentStats> comp_stats;
  for (size_t i = 0; i < occs.size(); ++i) {
    if (pinned_occ[i]) {
      continue;
    }
    const int root = uf.Find(static_cast<int>(i));
    auto [it, inserted] = comp_stats.try_emplace(root, WebComponentStats{.sig = occs[i].sig});
    if (!occs[i].is_clobber_only) {
      if (!it->second.has_non_clobber_ref) {
        root_order.push_back(root);
      }
      it->second.has_non_clobber_ref = true;
      it->second.n_refs += occs[i].ref_weight;
      it->second.n_sets += occs[i].set_count;
    }
  }

  if (compiler_flags.opt_level == 0) {
    // At `-O0`, `stupid_reg_compare` (`stupid.c:287-306`) breaks equal
    // `(len, reg_n_refs)` ties in ascending `regno` order (`r1 - r2`), so
    // ordering lifted webs by ascending allocated `hard_regno` assigns lower
    // pseudo numbers to lower-numbered hardware registers.
    std::stable_sort(root_order.begin(), root_order.end(), [&](int a, int b) {
      return comp_stats.at(a).sig.hard_regno < comp_stats.at(b).sig.hard_regno;
    });
  }

  int next_pseudo = std::max(first_pseudo_regno, max_existing_regno + 1);

  RtlControlFlowGraph out = allocated_cfg;
  out.sideband.reg_renumber.clear();
  out.sideband.reg_n_refs.clear();
  out.sideband.reg_n_sets.clear();
  out.sideband.reg_n_deaths.clear();
  out.sideband.reg_live_length.clear();
  out.sideband.reg_n_calls_crossed.clear();

  for (int root : root_order) {
    auto it = comp_stats.find(root);
    if (it == comp_stats.end() || !it->second.has_non_clobber_ref) {
      continue;
    }
    const int pseudo = next_pseudo++;
    it->second.pseudo_regno = pseudo;
    out.sideband.reg_renumber[pseudo] = it->second.sig.hard_regno;
    out.sideband.reg_n_refs[pseudo] = std::max(1, it->second.n_refs);
    out.sideband.reg_n_sets[pseudo] = it->second.n_sets;
  }

  // Identify contiguous 8-byte `$fp`-relative stack slots at the top of
  // `frame_size_bytes` (`slot_base = frame_size_bytes - 8, frame_size_bytes - 16, ...`)
  // allocated by `alter_reg(..., -1)` via `assign_stack_local(mode, size, -1)`.
  absl::flat_hash_map<int, int> slot_to_pseudo;
  if (can_check_stack_slots) {
    AssignUnoptimizedStackSlotPseudos(allocated_cfg, occs, stack_slots, invalidated_slots,
                                      frame_size_bytes, &next_pseudo, &slot_to_pseudo, &out);
  }

  out.sideband.max_regno = std::max(out.sideband.max_regno, next_pseudo);

  // Pass 5: Rewrite lifted hard-register webs and stack-slot `MEM`s across all
  // instruction patterns, `SEQUENCE` sub-instructions, `REG_NOTES`, and any
  // `epilogue_delay_pattern`.
  std::vector<ActiveLibcallNoteScope> open_libcall_scopes;
  for (size_t b = 0; b < num_blocks; ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      RtlPatternAndNotes& parsed = parsed_blocks[b][i];
      const InsnNoteContext& note_ctx = block_note_ctxs[b][i];

      if (is_paired_libcall[b][i]) {
        const int clobber_occ = ExtractStandaloneClobberTargetOccId(parsed.pattern, expr_to_occ);
        const int target_pseudo = LookupPseudoForOcc(clobber_occ, &uf, pinned_occ, comp_stats);
        open_libcall_scopes.push_back(ActiveLibcallNoteScope{
            .pre_reach = note_ctx.pre_reach,
            .target_pseudo = target_pseudo,
        });
      }
      std::optional<ActiveLibcallNoteScope> popped_libcall_scope;
      if (is_paired_retval[b][i] && !open_libcall_scopes.empty()) {
        popped_libcall_scope = std::move(open_libcall_scopes.back());
        open_libcall_scopes.pop_back();
      }
      const ActiveLibcallNoteScope* active_scope_ptr =
          popped_libcall_scope.has_value()
              ? &(*popped_libcall_scope)
              : (!open_libcall_scopes.empty() ? &open_libcall_scopes.back() : nullptr);
      const ReachState* retval_reach_ptr =
          popped_libcall_scope.has_value() ? &popped_libcall_scope->pre_reach : nullptr;

      if (parsed.pattern.op == RtlOp::kSequence) {
        for (size_t s = 0; s < parsed.pattern.children.size(); ++s) {
          RtlExpr& sub = parsed.pattern.children[s];
          if (!sub.children.empty()) {
            RewritePatternAst(&sub.children[0], outgoing_args_size, expr_to_occ, &uf, pinned_occ,
                              comp_stats, slot_to_pseudo);
          }
          if (s < block_seq_note_ctxs[b][i].size()) {
            for (size_t extra_idx = 1; extra_idx < sub.children.size(); ++extra_idx) {
              RewriteSequenceSubInsnNotesChain(&sub.children[extra_idx], gp64, fp64, fp_is_fixed,
                                               outgoing_args_size, block_seq_note_ctxs[b][i][s],
                                               occs, &uf, pinned_occ, comp_stats, slot_to_pseudo);
            }
          }
        }
      } else {
        RewritePatternAst(&parsed.pattern, outgoing_args_size, expr_to_occ, &uf, pinned_occ,
                          comp_stats, slot_to_pseudo);
      }

      for (RtlRegNote& note : parsed.reg_notes) {
        if (note.list_op == RtlOp::kExprList) {
          RewriteNoteDatumExpr(&note.datum, note.kind, gp64, fp64, fp_is_fixed, outgoing_args_size,
                               note_ctx, retval_reach_ptr, active_scope_ptr, occs, &uf, pinned_occ,
                               comp_stats, slot_to_pseudo);
        }
      }

      blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(
          parsed.pattern, parsed.reg_notes, parsed.bracketed_notes, /*call_fusage=*/{});
    }
    blk.SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < blk.rtl_insns.size() && i < parsed_blocks[b].size(); ++i) {
      if (parsed_blocks[b][i].has_explicit_call_fusage &&
          !blk.rtl_insns[i].has_explicit_call_fusage) {
        blk.rtl_insns[i].has_explicit_call_fusage = true;
        blk.rtl_insns[i].call_fusage = parsed_blocks[b][i].call_fusage;
      }
    }
  }

  if (epilogue_delay_parsed.has_value()) {
    RewritePatternAst(&epilogue_delay_parsed->pattern, outgoing_args_size, expr_to_occ, &uf,
                      pinned_occ, comp_stats, slot_to_pseudo);
    if (has_epilogue_delay_ctx) {
      for (RtlRegNote& note : epilogue_delay_parsed->reg_notes) {
        if (note.list_op == RtlOp::kExprList) {
          RewriteNoteDatumExpr(&note.datum, note.kind, gp64, fp64, fp_is_fixed, outgoing_args_size,
                               epilogue_delay_note_ctx, /*retval_libcall_reach=*/nullptr,
                               /*active_libcall_scope=*/nullptr, occs, &uf, pinned_occ, comp_stats,
                               slot_to_pseudo);
        }
      }
    }
    out.sideband.epilogue_delay_pattern = epilogue_delay_parsed->ToGccPrintRtlString();
  }

  PostProcessPreAlterCfg(profile, compiler_flags, gp64, fp64, first_pseudo_regno,
                         allocated_cfg.sideband.max_regno, cs_save_span, &parsed_blocks, &out);
  return out;
}

}  // namespace rom_nom_nom::lifter
