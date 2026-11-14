#include "lifter/invert_alter_reg.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/strings/numbers.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

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

// Returns true if `regno` is an allocatable MIPS GPR (`$2..$25`) or FPR
// (`$32..$63`). Fixed/reserved registers (`$0`, `$1/at`, `$26..$31`), MD
// accumulators (`64..66`), and `$fcr31` (`67`) are never lifted to pseudos.
bool IsAllocatableHardReg(int regno) {
  return (regno >= 2 && regno <= 25) || (regno >= 32 && regno <= 63);
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
// multi-word span lies in `2..25` or `32..63`. Hardware return registers (`/i`)
// and frame-related/pointer-flagged registers (`/f`) are never lifted.
bool IsCandidateHardRegNode(const RtlExpr& reg, bool gp64, bool fp64) {
  if (reg.op != RtlOp::kReg || reg.flags.integrated || reg.flags.frame_related ||
      !IsAllocatableMode(reg.mode)) {
    return false;
  }
  const int regno = reg.RegNo();
  if (!IsAllocatableHardReg(regno)) {
    return false;
  }
  const int span = std::max(1, MipsHardRegNregs(regno, reg.mode, gp64, fp64));
  for (int k = 0; k < span; ++k) {
    if (!IsAllocatableHardReg(regno + k)) {
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

bool HasFloatStoreFlag(absl::Span<const std::string> c_flags) {
  bool float_store = false;
  for (std::string_view flag : c_flags) {
    if (flag == "-ffloat-store") {
      float_store = true;
    } else if (flag == "-fno-float-store") {
      float_store = false;
    }
  }
  return float_store;
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
      if (tok >= 0 && !occs[tok].is_clobber_only &&
          (overwriter_tok < 0 || !(occs[tok].sig == occs[overwriter_tok].sig))) {
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
                                int ref_weight, bool gp64, bool fp64,
                                std::vector<RegOccurrence>* occs,
                                absl::flat_hash_map<const RtlExpr*, int>* expr_to_occ) {
  if (expr.op == RtlOp::kNote || expr.op == RtlOp::kCodeLabel || expr.op == RtlOp::kBarrier ||
      expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec || expr.op == RtlOp::kReturn) {
    return;
  }

  if (IsCandidateHardRegNode(expr, gp64, fp64)) {
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
                               ref_weight, gp64, fp64, occs, expr_to_occ);
    RegisterPatternOccurrences(expr.children[1], /*in_set_dest=*/false, /*in_clobber=*/false,
                               ref_weight, gp64, fp64, occs, expr_to_occ);
    return;
  }

  if (expr.op == RtlOp::kClobber && !expr.children.empty()) {
    RegisterPatternOccurrences(expr.children[0], /*in_set_dest=*/true, /*in_clobber=*/true,
                               ref_weight, gp64, fp64, occs, expr_to_occ);
    return;
  }

  if (expr.op == RtlOp::kMem) {
    for (const RtlExpr& child : expr.children) {
      RegisterPatternOccurrences(child, /*in_set_dest=*/false, /*in_clobber=*/false, ref_weight,
                                 gp64, fp64, occs, expr_to_occ);
    }
    return;
  }

  const bool pass_dest =
      in_set_dest && (expr.op == RtlOp::kSubreg || expr.op == RtlOp::kStrictLowPart ||
                      expr.op == RtlOp::kZeroExtract || expr.op == RtlOp::kSignExtract);
  for (size_t i = 0; i < expr.children.size(); ++i) {
    const bool child_dest = pass_dest && (i == 0);
    RegisterPatternOccurrences(expr.children[i], child_dest, in_clobber, ref_weight, gp64, fp64,
                               occs, expr_to_occ);
  }
}

struct WebCollector {
  const std::vector<RegOccurrence>* occs = nullptr;
  const absl::flat_hash_map<const RtlExpr*, int>* expr_to_occ = nullptr;
  WebUnionFind* uf = nullptr;
  std::vector<bool>* pinned_seed = nullptr;
  std::vector<absl::InlinedVector<int, 4>>* pin_adj = nullptr;
  absl::flat_hash_map<RegWebSignature, int>* uninit_anchors = nullptr;

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
        // Reading a hard-register word with zero reaching definitions (e.g. a
        // post-reload `define_split` reading a `PARALLEL` `match_scratch`
        // `CLOBBER` register, or reading a call-clobbered register after a call)
        // must stay pinned on the physical hard register.
        (*pinned_seed)[occ_id] = true;
        continue;
      }
      for (int tok : reach.defs[w]) {
        if (tok == kPinnedAbiToken) {
          (*pinned_seed)[occ_id] = true;
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

void UnifyWriteWithClobbers(int occ_id, int write_start, int write_span, const ReachState& state,
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
      } else if (tok >= 0 && (*collector->occs)[tok].is_clobber_only &&
                 (*collector->occs)[tok].sig == w_sig) {
        collector->AddPinEdge(tok, occ_id);
        collector->uf->Union(tok, occ_id);
      }
    }
  }
  if (has_uninit_entry && !has_pinned_abi) {
    // Unify read-modify-write initialization of an uninitialized local pseudo
    // (e.g. byte/halfword/bitfield lane insertion into a union local).
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
      StepSetDestWrite(dst.children[0], is_call_return_dest, gp64, fp64, active_libcall_clobbers,
                       state, occs, expr_to_occ, collector, insn_write_occs);
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
      UnifyWriteWithClobbers(occ_id, write_start, write_span, *state, active_libcall_clobbers,
                             collector);
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
      UnifyWriteWithClobbers(occ_id, r, span, *state, active_libcall_clobbers, collector);
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
    StepSetDestWrite(pat.children[0], /*is_call_return_dest=*/true, gp64, fp64, {}, state, occs,
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
        StepSetDestWrite(elem.children[0], /*is_call_return_dest=*/true, gp64, fp64, {}, state,
                         occs, expr_to_occ, collector, insn_write_occs);
      }
    }
  }
}

// Steps a single non-`SEQUENCE` pattern forward through `state`.
void StepSinglePatternForward(const RtlExpr& pat, const std::vector<RtlExpr>& call_fusage,
                              bool is_exit_tail_use, bool has_libcall_note,
                              absl::Span<const int> active_libcall_clobbers, bool gp64, bool fp64,
                              ReachState* state, const std::vector<RegOccurrence>& occs,
                              const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
                              WebCollector* collector, InsnNoteContext* out_note_ctx) {
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
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
      out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
      out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
    }
    return;
  }

  if (pat.op == RtlOp::kUse && !pat.children.empty()) {
    const RtlExpr& inner = pat.children[0];
    if (inner.op == RtlOp::kReg && inner.RegNo() >= 0 && inner.RegNo() < kFirstPseudoRegister &&
        (inner.flags.integrated || (is_exit_tail_use && IsFunctionReturnHardReg(inner.RegNo())))) {
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
    StepExprReads(inner, gp64, fp64, state, expr_to_occ, collector, &read_occs);
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
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
      out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
      out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
    }
    return;
  }

  if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
    StepSetDestReads(pat.children[0], gp64, fp64, state, expr_to_occ, collector, &read_occs);
    StepExprReads(pat.children[1], gp64, fp64, state, expr_to_occ, collector, &read_occs);
    StepSetDestWrite(pat.children[0], /*is_call_return_dest=*/false, gp64, fp64,
                     active_libcall_clobbers, state, occs, expr_to_occ, collector, &write_occs);
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
        StepSetDestWrite(elem.children[0], /*is_call_return_dest=*/false, gp64, fp64,
                         active_libcall_clobbers, state, occs, expr_to_occ, collector, &write_occs);
      }
    }
    if (out_note_ctx != nullptr) {
      out_note_ctx->post_reach = *state;
      out_note_ctx->read_occs.assign(read_occs.begin(), read_occs.end());
      out_note_ctx->write_occs.assign(write_occs.begin(), write_occs.end());
    }
    return;
  }

  StepExprReads(pat, gp64, fp64, state, expr_to_occ, collector, &read_occs);
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
                          int outgoing_args_size, const InsnNoteContext& note_ctx,
                          const ReachState* retval_libcall_reach,
                          const ActiveLibcallNoteScope* active_libcall_scope,
                          const std::vector<RegOccurrence>& occs, WebUnionFind* uf,
                          const std::vector<bool>& pinned_occ,
                          const absl::flat_hash_map<int, WebComponentStats>& comp_stats,
                          const absl::flat_hash_map<int, int>& slot_to_pseudo) {
  if (IsCandidateHardRegNode(*expr, gp64, fp64)) {
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
    RewriteNoteDatumExpr(&child, note_kind, gp64, fp64, outgoing_args_size, note_ctx,
                         retval_libcall_reach, active_libcall_scope, occs, uf, pinned_occ,
                         comp_stats, slot_to_pseudo);
  }
}

void RewriteSequenceSubInsnNotesChain(RtlExpr* chain, bool gp64, bool fp64, int outgoing_args_size,
                                      const InsnNoteContext& note_ctx,
                                      const std::vector<RegOccurrence>& occs, WebUnionFind* uf,
                                      const std::vector<bool>& pinned_occ,
                                      const absl::flat_hash_map<int, WebComponentStats>& comp_stats,
                                      const absl::flat_hash_map<int, int>& slot_to_pseudo) {
  if (chain->op == RtlOp::kExprList && chain->mode != RtlMachineMode::kNone &&
      !chain->children.empty()) {
    RewriteNoteDatumExpr(&chain->children[0], chain->mode, gp64, fp64, outgoing_args_size, note_ctx,
                         /*retval_libcall_reach=*/nullptr, /*active_libcall_scope=*/nullptr, occs,
                         uf, pinned_occ, comp_stats, slot_to_pseudo);
    if (chain->children.size() >= 2) {
      RewriteSequenceSubInsnNotesChain(&chain->children[1], gp64, fp64, outgoing_args_size,
                                       note_ctx, occs, uf, pinned_occ, comp_stats, slot_to_pseudo);
    }
    return;
  }
  if (chain->op == RtlOp::kInsnList && chain->children.size() >= 2) {
    RewriteSequenceSubInsnNotesChain(&chain->children[1], gp64, fp64, outgoing_args_size, note_ctx,
                                     occs, uf, pinned_occ, comp_stats, slot_to_pseudo);
  }
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
    for (const std::string& pat : blk.instructions) {
      if (pat.empty()) {
        return RtlControlFlowGraph{};
      }
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok()) {
        return RtlControlFlowGraph{};
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
  const bool float_store = HasFloatStoreFlag(profile.c_flags);
  const bool can_check_stack_slots =
      compiler_flags.opt_level == 0 && allocated_cfg.sideband.frame_pointer_needed &&
      !float_store && outgoing_args_size >= 0 && frame_size_bytes >= kStackSlotAlignmentBytes &&
      (frame_size_bytes % kStackSlotAlignmentBytes) == 0;
  absl::flat_hash_map<int, StackSlotStats> stack_slots;
  absl::flat_hash_set<int> invalidated_slots;

  int loop_depth = 1;
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = allocated_cfg.rpo_blocks[b];
    for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
      if (auto lk = ParseRtlLoopNoteKind(blk.instructions[i]); lk.has_value()) {
        if (*lk == RtlLoopNoteKind::kLoopBeg) {
          ++loop_depth;
        } else if (*lk == RtlLoopNoteKind::kLoopEnd) {
          loop_depth = std::max(1, loop_depth - 1);
        }
      }
      const int ref_weight = (compiler_flags.opt_level > 0) ? loop_depth : 1;
      const RtlPatternAndNotes& pn = parsed_blocks[b][i];
      if (pn.pattern.op == RtlOp::kSequence) {
        for (const RtlExpr& sub : pn.pattern.children) {
          if (!sub.children.empty()) {
            RegisterPatternOccurrences(sub.children[0], /*in_set_dest=*/false,
                                       /*in_clobber=*/false, ref_weight, gp64, fp64, &occs,
                                       &expr_to_occ);
          }
        }
      } else {
        RegisterPatternOccurrences(pn.pattern, /*in_set_dest=*/false, /*in_clobber=*/false,
                                   ref_weight, gp64, fp64, &occs, &expr_to_occ);
      }

      if (can_check_stack_slots) {
        AnalyzeFpStackSlots(pn.pattern, /*in_set_dest=*/false, /*in_note=*/false, ref_weight,
                            frame_size_bytes, outgoing_args_size, &stack_slots, &invalidated_slots);
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
                               /*in_clobber=*/false, /*ref_weight=*/1, gp64, fp64, &occs,
                               &expr_to_occ);
    if (can_check_stack_slots) {
      AnalyzeFpStackSlots(epilogue_delay_parsed->pattern, /*in_set_dest=*/false, /*in_note=*/false,
                          /*ref_weight=*/1, frame_size_bytes, outgoing_args_size, &stack_slots,
                          &invalidated_slots);
    }
  }

  // Pass 2: Forward reaching-definitions fixed-point dataflow across `rpo_blocks`.
  std::vector<ReachState> reach_in(num_blocks);
  for (int w = 0; w < kFirstPseudoRegister; ++w) {
    if (IsIncomingArgumentHardReg(w)) {
      reach_in[0].defs[w].push_back(kPinnedAbiToken);
    } else if (IsAllocatableHardReg(w)) {
      reach_in[0].defs[w].push_back(kUninitEntryToken);
    }
  }

  auto step_insn_or_sequence = [&](size_t b, size_t i, ReachState* state,
                                   std::vector<int>* open_libcall_clobbers, WebCollector* collector,
                                   InsnNoteContext* top_note_ctx,
                                   std::vector<InsnNoteContext>* seq_sub_note_ctxs,
                                   ReachState* out_annulled_fallthrough_state,
                                   bool* out_has_annulled_split) {
    *out_has_annulled_split = false;
    const RtlPatternAndNotes& pn = parsed_blocks[b][i];
    const bool has_libcall_note = HasRegNoteKind(pn, RtlMachineMode::kRegLibcall);
    const bool has_retval_note = HasRegNoteKind(pn, RtlMachineMode::kRegRetval);
    if (has_libcall_note) {
      open_libcall_clobbers->push_back(
          ExtractStandaloneClobberTargetOccId(pn.pattern, expr_to_occ));
    }
    auto pop_libcall_if_retval = [&]() {
      if (has_retval_note && !open_libcall_clobbers->empty()) {
        open_libcall_clobbers->pop_back();
      }
    };

    const std::vector<RtlExpr> fusage = effective_call_fusage(b, i, pn);
    if (pn.pattern.op != RtlOp::kSequence) {
      StepSinglePatternForward(pn.pattern, fusage, is_exit_tail[b][i], has_libcall_note,
                               *open_libcall_clobbers, gp64, fp64, state, occs, expr_to_occ,
                               collector, top_note_ctx);
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
        StepSinglePatternForward(pn.pattern.children[s].children[0], {}, /*is_exit_tail_use=*/false,
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
      StepSinglePatternForward(pn.pattern.children[0].children[0], {}, /*is_exit_tail_use=*/false,
                               /*has_libcall_note=*/false, *open_libcall_clobbers, gp64, fp64,
                               state, occs, expr_to_occ, collector, head_ctx);
      if (is_annulled_target && out_annulled_fallthrough_state != nullptr) {
        *out_annulled_fallthrough_state = *state;
        *out_has_annulled_split = true;
      }
      for (size_t s = 1; s < num_subs; ++s) {
        InsnNoteContext* sub_ctx =
            (seq_sub_note_ctxs != nullptr) ? &(*seq_sub_note_ctxs)[s] : nullptr;
        StepSinglePatternForward(pn.pattern.children[s].children[0], {}, /*is_exit_tail_use=*/false,
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
      std::vector<int> open_libcall_clobbers;
      ReachState ft_state{};
      bool has_annulled_split = false;
      for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
        step_insn_or_sequence(b, i, &cur, &open_libcall_clobbers, /*collector=*/nullptr,
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
  WebUnionFind uf(occs.size());
  std::vector<bool> pinned_seed(occs.size(), false);
  std::vector<absl::InlinedVector<int, 4>> pin_adj(occs.size());
  absl::flat_hash_map<RegWebSignature, int> uninit_anchors;
  WebCollector collector{
      .occs = &occs,
      .expr_to_occ = &expr_to_occ,
      .uf = &uf,
      .pinned_seed = &pinned_seed,
      .pin_adj = &pin_adj,
      .uninit_anchors = &uninit_anchors,
  };

  std::vector<std::vector<InsnNoteContext>> block_note_ctxs(num_blocks);
  std::vector<std::vector<std::vector<InsnNoteContext>>> block_seq_note_ctxs(num_blocks);
  InsnNoteContext epilogue_delay_note_ctx;
  bool has_epilogue_delay_ctx = false;

  for (size_t b = 0; b < num_blocks; ++b) {
    ReachState cur = reach_in[b];
    std::vector<int> open_libcall_clobbers;
    const size_t n = parsed_blocks[b].size();
    block_note_ctxs[b].resize(n);
    block_seq_note_ctxs[b].resize(n);
    ReachState unused_ft{};
    bool unused_split = false;
    for (size_t i = 0; i < n; ++i) {
      step_insn_or_sequence(b, i, &cur, &open_libcall_clobbers, &collector, &block_note_ctxs[b][i],
                            &block_seq_note_ctxs[b][i], &unused_ft, &unused_split);
    }
    const RtlBasicBlock& blk = allocated_cfg.rpo_blocks[b];
    if (epilogue_delay_parsed.has_value() && blk.successors.empty() &&
        blk.addr_vec_targets.empty()) {
      ReachState ep_state = cur;
      InsnNoteContext ep_ctx;
      StepSinglePatternForward(epilogue_delay_parsed->pattern, {}, /*is_exit_tail_use=*/false,
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
      valid_top_slot_bases.push_back(slot_base);
    }
    std::reverse(valid_top_slot_bases.begin(), valid_top_slot_bases.end());
    for (int slot_base : valid_top_slot_bases) {
      const StackSlotStats& st = stack_slots.at(slot_base);
      const int pseudo = next_pseudo++;
      slot_to_pseudo[slot_base] = pseudo;
      out.sideband.reg_renumber[pseudo] = -1;
      out.sideband.reg_n_refs[pseudo] = std::max(1, st.n_refs);
      out.sideband.reg_n_sets[pseudo] = st.n_sets;
      out.sideband.frame_size_bytes -= kStackSlotAlignmentBytes;
    }
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

      const bool has_libcall_note = HasRegNoteKind(parsed, RtlMachineMode::kRegLibcall);
      const bool has_retval_note = HasRegNoteKind(parsed, RtlMachineMode::kRegRetval);
      if (has_libcall_note) {
        const int clobber_occ = ExtractStandaloneClobberTargetOccId(parsed.pattern, expr_to_occ);
        const int target_pseudo = LookupPseudoForOcc(clobber_occ, &uf, pinned_occ, comp_stats);
        open_libcall_scopes.push_back(ActiveLibcallNoteScope{
            .pre_reach = note_ctx.pre_reach,
            .target_pseudo = target_pseudo,
        });
      }
      std::optional<ActiveLibcallNoteScope> popped_libcall_scope;
      if (has_retval_note && !open_libcall_scopes.empty()) {
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
              RewriteSequenceSubInsnNotesChain(&sub.children[extra_idx], gp64, fp64,
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
          RewriteNoteDatumExpr(&note.datum, note.kind, gp64, fp64, outgoing_args_size, note_ctx,
                               retval_reach_ptr, active_scope_ptr, occs, &uf, pinned_occ,
                               comp_stats, slot_to_pseudo);
        }
      }
      blk.instructions[i] = parsed.ToSexprString();
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
          RewriteNoteDatumExpr(&note.datum, note.kind, gp64, fp64, outgoing_args_size,
                               epilogue_delay_note_ctx, /*retval_libcall_reach=*/nullptr,
                               /*active_libcall_scope=*/nullptr, occs, &uf, pinned_occ, comp_stats,
                               slot_to_pseudo);
        }
      }
    }
    out.sideband.epilogue_delay_pattern = epilogue_delay_parsed->ToGccPrintRtlString();
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
