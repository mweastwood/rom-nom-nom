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
#include "lifter/sched_model.h"
#include "lifter/stack_frame_slot_classifier.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;
constexpr int kStackPointerRegno = 29;
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

// Mirrors `sched.c:2469-2549` (`birthing_insn_p`, `adjust_priority`) and
// `sched.c:1650-1792` (`sched_analyze_1`): checks whether block `b` (or all
// blocks in `check_blocks`) remains in `Im(F_sched1)` after applying
// `extra_demote` (`pseudo -> hard_reg`).
bool ArePreAllocBlocksSched1Achievable(
    const RtlControlFlowGraph& base_cfg, absl::Span<const size_t> check_blocks,
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks,
    const absl::flat_hash_map<int, int>& extra_demote, const CompilationProfile& profile) {
  if (check_blocks.empty()) {
    return true;
  }
  RtlControlFlowGraph probe = base_cfg;
  auto rewrite_fn = [&](RtlExpr& r) {
    if (r.IsPseudoReg()) {
      if (const auto it = extra_demote.find(r.RegNo()); it != extra_demote.end()) {
        r.SetRegNo(it->second);
        if (it->second == kFramePointerRegno) {
          r.str_val = "$fp";
        }
      }
    }
  };
  for (size_t bi = 0; bi < all_parsed_blocks.size() && bi < probe.rpo_blocks.size(); ++bi) {
    RtlBasicBlock& blk = probe.rpo_blocks[bi];
    blk.instructions.resize(all_parsed_blocks[bi].size());
    for (size_t i = 0; i < all_parsed_blocks[bi].size(); ++i) {
      RtlPatternAndNotes pn = all_parsed_blocks[bi][i];
      if (!extra_demote.empty()) {
        pn.RewriteRegs(rewrite_fn);
      }
      blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(
          pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{});
    }
    blk.SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < blk.rtl_insns.size() && i < all_parsed_blocks[bi].size(); ++i) {
      if (all_parsed_blocks[bi][i].has_explicit_call_fusage &&
          !blk.rtl_insns[i].has_explicit_call_fusage) {
        blk.rtl_insns[i].has_explicit_call_fusage = true;
        blk.rtl_insns[i].call_fusage = all_parsed_blocks[bi][i].call_fusage;
      }
    }
  }
  probe.sideband.reg_n_sets.clear();
  probe.sideband.reg_n_calls_crossed.clear();
  probe.sideband.reg_live_length.clear();
  probe.sideband.basic_block_live_at_start.clear();
  ReconstructRtlLiveness(&probe, profile,
                         RtlLivenessOptions{
                             .reload_completed = false,
                             .update_reg_notes = false,
                             .preserve_phantom_pseudos = true,
                             .include_call_reg_notes = true,
                         });
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  const sched_model::RegFacts facts =
      sched_model::RegFacts::FromCfg(probe, profile, sched_model::Phase::kSched1);
  for (size_t b : check_blocks) {
    if (b >= probe.rpo_blocks.size()) {
      continue;
    }
    const sched_model::ParsedSchedRegion region =
        sched_model::ParseBlockForSchedModel(probe.rpo_blocks[b], b, sched_model::Phase::kSched1);
    if (!region.valid || region.has_mid_block_loop_barrier || region.sched_block.size() <= 1) {
      continue;
    }
    absl::btree_set<int> live_at_end;
    for (int succ : probe.rpo_blocks[b].successors) {
      if (const auto it = probe.sideband.basic_block_live_at_start.find(succ);
          it != probe.sideband.basic_block_live_at_start.end()) {
        live_at_end.insert(it->second.begin(), it->second.end());
      }
    }
    if (!sched_model::IsAchievable(region.sched_block, facts, mm, sched_model::Phase::kSched1,
                                   live_at_end)) {
      return false;
    }
  }
  return true;
}

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
  // `(clobber (match_operand:SI 3 "register_operand" "=d"))` (`gen_reg_rtx`)
  // rather than `match_scratch`. Register any non-fixed `SImode` GPR clobber on
  // these patterns as a single-def occurrence so `InvertAlterReg` lifts it to a
  // pseudo.
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
      // `flow.c:2537-2550` / `sched.c:2888-2890`: post-reload (`reload_completed`),
      // when an instruction reads a subword of a dying multi-word pseudo `p`
      // (`[p_hr, p_hr + p_span)`) while simultaneously writing a disjoint subword
      // of `[p_hr, p_hr + p_span)` to a different pseudo, GCC emits a partial-word
      // `REG_DEAD` note for only the unwritten subword. Restore `(REG_DEAD (reg:M p))`.
      if (pseudo < kFirstPseudoRegister && note_kind == RtlMachineMode::kRegDead) {
        const int nr = expr->RegNo();
        const int nspan = std::max(1, MipsHardRegNregs(nr, expr->mode, gp64, fp64));
        int cand_p = -1;
        RegWebSignature cand_sig{};
        bool ambiguous = false;
        for (int rocc : note_ctx.read_occs) {
          const int rp = LookupPseudoForOcc(rocc, uf, pinned_occ, comp_stats);
          if (rp < kFirstPseudoRegister) {
            continue;
          }
          const int phr = occs[rocc].sig.hard_regno;
          const int pspan = occs[rocc].word_span;
          if (pspan <= nspan || nr < phr || nr + nspan > phr + pspan) {
            continue;
          }
          bool other_subword_written = false;
          for (int wocc : note_ctx.write_occs) {
            const int wp = LookupPseudoForOcc(wocc, uf, pinned_occ, comp_stats);
            if (wp == rp) {
              continue;
            }
            const int whr = occs[wocc].sig.hard_regno;
            const int wspan = occs[wocc].word_span;
            if (std::max(phr, whr) < std::min(phr + pspan, whr + wspan) &&
                std::max(nr, whr) >= std::min(nr + nspan, whr + wspan)) {
              other_subword_written = true;
              break;
            }
          }
          if (!other_subword_written) {
            continue;
          }
          if (cand_p < 0) {
            cand_p = rp;
            cand_sig = occs[rocc].sig;
          } else if (cand_p != rp) {
            ambiguous = true;
            break;
          }
        }
        if (!ambiguous && cand_p >= kFirstPseudoRegister) {
          expr->SetRegNo(cand_p);
          expr->mode = cand_sig.mode;
          expr->raw_mode.clear();
          return;
        }
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
    struct UnpairedClobberSpan {
      size_t clobber_idx = 0;
      size_t limit_idx = 0;
      int clobber_occ = -1;
    };
    std::vector<PendingLibcall> pending;
    std::vector<UnpairedClobberSpan> unpaired_clobbers;
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
        // Per `optabs.c:2485-2525` (`emit_no_conflict_block`) and `sched.c`
        // (`sched1`), a standalone `(clobber (reg:M P))` (`REG_LIBCALL`) of a
        // multi-word pseudo can be hoisted above an unrelated `CALL_INSN`, in
        // which case `global_alloc` allocates `P` to a callee-saved register.
        // Only flush call-clobbered standalone `CLOBBER`s across a call; keep
        // callee-saved ones pending so they pair with their post-call
        // `REG_RETVAL`.
        auto is_callee_saved_clobber = [&](int occ_id) {
          if (occ_id < 0) {
            return false;
          }
          const RegOccurrence& co = occs[occ_id];
          for (int w = 0; w < co.word_span; ++w) {
            if (IsCallUsedHardReg(co.sig.hard_regno + w)) {
              return false;
            }
          }
          return true;
        };
        for (const PendingLibcall& p : pending) {
          if (p.clobber_occ >= 0 && !is_callee_saved_clobber(p.clobber_occ)) {
            unpaired_clobbers.push_back(UnpairedClobberSpan{
                .clobber_idx = p.insn_idx,
                .limit_idx = i,
                .clobber_occ = p.clobber_occ,
            });
          }
        }
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const PendingLibcall& p) {
                                       return p.clobber_occ >= 0 &&
                                              !is_callee_saved_clobber(p.clobber_occ);
                                     }),
                      pending.end());
      }
      if (HasRegNoteKind(pn, RtlMachineMode::kRegRetval)) {
        for (size_t rev = pending.size(); rev > 0; --rev) {
          const size_t pi = rev - 1;
          const PendingLibcall& top = pending[pi];
          if (top.clobber_occ >= 0) {
            bool matches_clobber_target = false;
            const RegWebSignature& clob_sig = occs[top.clobber_occ].sig;
            auto check_set = [&](const RtlExpr& s) {
              if (s.op == RtlOp::kSet && !s.children.empty()) {
                const RtlExpr* dst_node =
                    (s.children[0].op == RtlOp::kSubreg && !s.children[0].children.empty())
                        ? &s.children[0].children[0]
                        : &s.children[0];
                const int dst_occ = LookupOccId(*dst_node, expr_to_occ);
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
          pending.erase(pending.begin() + static_cast<ptrdiff_t>(pi));
          break;
        }
      }
    }
    for (const PendingLibcall& p : pending) {
      if (p.clobber_occ >= 0) {
        unpaired_clobbers.push_back(UnpairedClobberSpan{
            .clobber_idx = p.insn_idx,
            .limit_idx = n_insns,
            .clobber_occ = p.clobber_occ,
        });
      }
    }
    for (const UnpairedClobberSpan& sp : unpaired_clobbers) {
      const RegWebSignature& clob_sig = occs[sp.clobber_occ].sig;
      if (clob_sig.mode != RtlMachineMode::kDi) {
        continue;
      }
      std::optional<size_t> word1_idx;
      std::optional<size_t> word0_idx;
      for (size_t k = sp.clobber_idx + 1; k < sp.limit_idx; ++k) {
        const RtlExpr& kpat = parsed_blocks[b][k].pattern;
        if (!kpat.IsSet() || kpat.SetDest().op != RtlOp::kSubreg ||
            kpat.SetDest().children.empty()) {
          continue;
        }
        const int dst_occ = LookupOccId(kpat.SetDest().children[0], expr_to_occ);
        if (dst_occ < 0 || occs[dst_occ].sig.hard_regno != clob_sig.hard_regno ||
            occs[dst_occ].sig.mode != RtlMachineMode::kDi) {
          continue;
        }
        if (kpat.SetDest().int_val == 1 && !word1_idx.has_value()) {
          word1_idx = k;
        } else if (kpat.SetDest().int_val == 0 && word1_idx.has_value() && !word0_idx.has_value()) {
          word0_idx = k;
          break;
        }
      }
      if (word1_idx.has_value() && word0_idx.has_value() && !(*is_paired_retval)[b][*word0_idx]) {
        (*is_paired_libcall)[b][sp.clobber_idx] = true;
        (*is_paired_retval)[b][*word0_idx] = true;
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
            bool r_crosses_call_before_i = false;
            bool seen_r_def = false;
            for (size_t k = 0; k < i; ++k) {
              bool defs_r = false;
              all_parsed_blocks[b][k].pattern.Visit([&](const RtlExpr& m) {
                if (m.IsPseudoReg() && find_rmw(m.RegNo()) == root_r) {
                  defs_r = true;
                }
              });
              if (defs_r) {
                seen_r_def = true;
              } else if (seen_r_def && ContainsCallOp(all_parsed_blocks[b][k].pattern)) {
                r_crosses_call_before_i = true;
                break;
              }
            }
            bool w_live_at_b_start = false;
            if (const auto lit =
                    live_probe.sideband.basic_block_live_at_start.find(static_cast<int>(b));
                lit != live_probe.sideband.basic_block_live_at_start.end()) {
              for (int lreg : lit->second) {
                if (lreg >= kFirstPseudoRegister && find_rmw(lreg) == root_w) {
                  w_live_at_b_start = true;
                  break;
                }
              }
            }
            const bool is_single_def_rmw_init =
                !r_global && w_global && pseudo_def_count[p_r] == 1 &&
                ((b == 0 && pseudo_def_count[root_r] == 1 && pseudo_read_count[root_r] == 1 &&
                  const_init_pseudos.contains(root_r)) ||
                 (pseudo_def_count[root_r] == 1 && high_part_const_pseudos.contains(root_r) &&
                  (rhs_op == RtlOp::kIor || rhs_op == RtlOp::kPlus || rhs_op == RtlOp::kLoSum)) ||
                 (and_def_pseudos.contains(p_r) && rhs_op == RtlOp::kIor &&
                  (pseudo_def_count[root_r] == 1 || mem_init_pseudos.contains(root_r))) ||
                 (seen_r_def && !r_crosses_call_before_i && !w_live_at_b_start &&
                  pseudo_def_count[root_r] == 1 && pseudo_read_count[root_r] == 1 && hr != 2 &&
                  hr != 32));
            bool is_global_rmw_tail = false;
            if (!is_single_def_rmw_init && r_global && !w_global) {
              const int dst_span = std::max(1, MipsHardRegNregs(hr, dst.mode, gp64, fp64));
              bool r_used_later_or_live_out = false;
              bool r_redefined_after_w = false;
              size_t last_w_idx = i;
              for (size_t j = i + 1; j < all_parsed_blocks[b].size(); ++j) {
                all_parsed_blocks[b][j].pattern.Visit([&](const RtlExpr& m) {
                  if (m.IsPseudoReg() && find_rmw(m.RegNo()) == root_w) {
                    last_w_idx = j;
                  }
                });
              }
              for (size_t j = i + 1; !r_used_later_or_live_out && j < all_parsed_blocks[b].size();
                   ++j) {
                bool j_reads_r = false;
                bool j_writes_r = false;
                auto scan_j = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
                  if (e.op == RtlOp::kSet && e.children.size() >= 2) {
                    self(self, e.children[0], true);
                    for (size_t c = 1; c < e.children.size(); ++c) {
                      self(self, e.children[c], false);
                    }
                    return;
                  }
                  if (in_dst && e.op == RtlOp::kMem) {
                    for (const RtlExpr& ch : e.children) {
                      self(self, ch, false);
                    }
                    return;
                  }
                  if (e.IsPseudoReg() && find_rmw(e.RegNo()) == root_r) {
                    if (in_dst) {
                      j_writes_r = true;
                    } else {
                      j_reads_r = true;
                    }
                    return;
                  }
                  for (const RtlExpr& ch : e.children) {
                    self(self, ch, in_dst);
                  }
                };
                scan_j(scan_j, all_parsed_blocks[b][j].pattern, false);
                if (j_reads_r) {
                  r_used_later_or_live_out = true;
                  break;
                }
                if (j_writes_r) {
                  if (j <= last_w_idx) {
                    r_used_later_or_live_out = true;
                  } else {
                    r_redefined_after_w = true;
                  }
                  break;
                }
              }
              if (!r_used_later_or_live_out && !r_redefined_after_w) {
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
      RtlPatternAndNotes& pn = all_parsed_blocks[b][i];
      if (!pn.reg_notes.empty()) {
        absl::flat_hash_set<int> set_roots;
        auto collect_set_root = [&](const RtlExpr& elem) {
          if (elem.IsSet()) {
            const RtlExpr* d = &elem.SetDest();
            while ((d->op == RtlOp::kSubreg || d->op == RtlOp::kStrictLowPart ||
                    d->op == RtlOp::kZeroExtract || d->op == RtlOp::kSignExtract) &&
                   !d->children.empty()) {
              d = &d->children[0];
            }
            if (d->IsPseudoReg()) {
              set_roots.insert(d->RegNo());
            }
          }
        };
        if (pn.pattern.IsParallel()) {
          for (const RtlExpr& ch : pn.pattern.children) {
            collect_set_root(ch);
          }
        } else {
          collect_set_root(pn.pattern);
        }
        if (!set_roots.empty()) {
          pn.reg_notes.erase(std::remove_if(pn.reg_notes.begin(), pn.reg_notes.end(),
                                            [&](const RtlRegNote& n) {
                                              return n.kind == RtlMachineMode::kRegDead &&
                                                     n.datum.IsPseudoReg() &&
                                                     set_roots.contains(n.datum.RegNo());
                                            }),
                             pn.reg_notes.end());
        }
      }
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
  auto is_tracked_hr = [](int h) { return (h >= 2 && h <= 15) || (h >= 32 && h <= 63); };
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
          if (is_tracked_hr(ohr)) {
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
            if (is_tracked_hr(ohr)) {
              (*conflict_hrs)[d].insert(ohr);
            }
          }
        }
        const int dhr = eff_hr_of(d);
        if (is_tracked_hr(dhr)) {
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
    const CompilationProfile& profile, const ParsedCompilerFlags& parsed_flags, bool fp64,
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
    if (p < kFirstPseudoRegister || !multi_block_pseudos.contains(p)) {
      continue;
    }
    const bool is_gpr_cand = (hr >= 3 && hr <= 15);
    const bool is_fpr_cand = (hr >= 34 && hr <= 62 && (hr % 2) == 0);
    if (!is_gpr_cand && !is_fpr_cand) {
      continue;
    }
    const auto mode_it = pseudo_widest_mode.find(p);
    if (mode_it != pseudo_widest_mode.end()) {
      if (is_gpr_cand && mode_it->second != RtlMachineMode::kSI) {
        continue;
      }
      if (is_fpr_cand && mode_it->second != RtlMachineMode::kSF &&
          mode_it->second != RtlMachineMode::kDF) {
        continue;
      }
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
          } else if (n.IsPseudoReg() && n.RegNo() != p && (hr < 32 || src.IsReg())) {
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
    const auto m1_it = pseudo_widest_mode.find(p1);
    if (m1_it == pseudo_widest_mode.end()) {
      continue;
    }
    for (size_t j = i + 1; j < sorted_cands.size(); ++j) {
      const int p2 = sorted_cands[j];
      const auto m2_it = pseudo_widest_mode.find(p2);
      if (remap.contains(p2) || out->sideband.reg_renumber.at(p2) != hr ||
          m2_it == pseudo_widest_mode.end() || m2_it->second != m1_it->second ||
          conflict_pseudos[p1].contains(p2) || conflict_hrs[p2].contains(hr)) {
        continue;
      }
      const int lo_h = (hr >= 32) ? 32 : 2;
      // `config/mips/mips.h:1268-1279` (`HARD_REGNO_MODE_OK`): on 32-bit FP
      // or `DFmode`, FP registers occupy even-numbered pairs (step 2); on
      // 64-bit FP (`-mfp64`) with `SFmode`, every FP register is valid (step 1).
      const int step_h = (hr >= 32) ? ((fp64 && m1_it->second == RtlMachineMode::kSF) ? 1 : 2) : 1;
      bool complementary_lower_hr = false;
      for (int h = lo_h; h < hr; h += step_h) {
        if (conflict_hrs[p1].contains(h) != conflict_hrs[p2].contains(h)) {
          complementary_lower_hr = true;
          break;
        }
      }
      bool all_lower_covered = true;
      for (int h = lo_h; h < hr; h += step_h) {
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
      if ((prefs & (uint64_t{1} << h_pref)) == 0 || h_pref == h_other ||
          h_pref == kFramePointerRegno) {
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
      struct GlobalShieldCand {
        int q = -1;
        bool hoisted = false;
        size_t n_blks = 0;
      };
      std::vector<GlobalShieldCand> cands;
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
        cands.push_back(GlobalShieldCand{
            .q = q, .hoisted = hoisted, .n_blks = qs_it->second.blocks_ref.size()});
      }
      if (!cands.empty()) {
        std::sort(cands.begin(), cands.end(),
                  [](const GlobalShieldCand& a, const GlobalShieldCand& b) {
                    if (a.hoisted != b.hoisted) {
                      return a.hoisted;
                    }
                    if (a.n_blks != b.n_blks) {
                      return a.n_blks < b.n_blks;
                    }
                    return a.q > b.q;
                  });
        int best_q = cands.front().q;
        // Per `sched.c:2469-2549` (`birthing_insn_p`), demoting a pseudo to a
        // hard register with `reg_n_sets > 1` clears its `birthing_insn_p`
        // boost. When multiple conflicting pseudos can shield `h_pref`, prefer
        // one whose demotion keeps its blocks in `Im(F_sched1)`.
        if (parsed_flags.RunsSched1() && cands.size() > 1) {
          for (const GlobalShieldCand& cand : cands) {
            std::vector<size_t> q_blocks(summaries.at(cand.q).blocks_ref.begin(),
                                         summaries.at(cand.q).blocks_ref.end());
            std::sort(q_blocks.begin(), q_blocks.end());
            absl::flat_hash_map<int, int> trial_demote = demote_to_hr;
            trial_demote[cand.q] = h_pref;
            if (ArePreAllocBlocksSched1Achievable(*out, q_blocks, all_parsed_blocks, trial_demote,
                                                  profile)) {
              best_q = cand.q;
              break;
            }
          }
        }
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
        if (it->second == kFramePointerRegno) {
          r.str_val = "$fp";
        }
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
  return ExtractStackBaseOffset(addr, base_regno);
}

bool ExprOverlapsElimInterval(const RtlExpr& root, int elim_reg, int64_t lo, int64_t hi) {
  return ExprOverlapsStackInterval(root, elim_reg, lo, hi);
}

// Identifies intra-block `caller-save` (`caller-save.c:insert_save_restore`)
// save/restore instructions in `allocated_cfg` via the deterministic
// `stack_frame_slot_classifier` so `InvertAlterReg` leaves their
// `regno_reg_rtx[hr]` operands as unflagged hard registers and preserves
// reaching-definition tokens across `CALL_INSN`s without pinning `/v` webs.
void PrecomputeCallerSavePairs(const RtlControlFlowGraph& allocated_cfg,
                               const ParsedCompilerFlags& compiler_flags, bool gp64, bool fp64,
                               const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
                               std::vector<std::vector<int>>* cs_save_span,
                               std::vector<std::vector<int>>* cs_restore_span) {
  const CallerSaveMatchOptions opts{
      .frame_pointer_needed = allocated_cfg.sideband.frame_pointer_needed,
      .frame_size_bytes = allocated_cfg.sideband.frame_size_bytes,
      .outgoing_args_size = allocated_cfg.sideband.outgoing_args_size,
      .gp64 = gp64,
      .fp64 = fp64,
      .opt_caller_saves_enabled =
          (compiler_flags.opt_level > 0 && compiler_flags.flag_caller_saves),
  };
  CallerSaveCfgClassification cls = ClassifyCallerSavePairsAcrossCfg(parsed_blocks, opts);
  *cs_save_span = std::move(cls.save_span);
  *cs_restore_span = std::move(cls.restore_span);
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

void CleanInvalidOrDuplicateRegDeadNotesOnInsn(RtlPatternAndNotes* pn, bool gp64);

// Rewrites demoted reload-scratch pseudos in `parsed_blocks` back to their
// physical hard registers, erases them from `reg_renumber`, and returns per-
// block, per-insn hard-register bitmasks of the demoted reload scratches so
// `ResolveLocalFalseCombineAndSuggPseudos` ignores post-`local_alloc` reload
// scratches (`toplev.c:3098-3142`, `local-alloc.c:1798, 1824`, `reload1.c:663`).
std::vector<std::vector<uint64_t>> ApplyDemotedReloadScratches(
    bool gp64, bool fp64, const absl::flat_hash_map<int, int>& demote_to_hr,
    std::vector<std::vector<RtlPatternAndNotes>>* parsed_blocks, RtlControlFlowGraph* out) {
  std::vector<std::vector<uint64_t>> scratch_masks(parsed_blocks->size());
  for (size_t b = 0; b < parsed_blocks->size(); ++b) {
    scratch_masks[b].assign((*parsed_blocks)[b].size(), 0);
  }
  if (demote_to_hr.empty()) {
    return scratch_masks;
  }
  for (size_t b = 0; b < parsed_blocks->size(); ++b) {
    auto& blk = (*parsed_blocks)[b];
    for (size_t i = 0; i < blk.size(); ++i) {
      RtlPatternAndNotes& pn = blk[i];
      uint64_t insn_mask = 0;
      auto rewrite_demoted = [&](RtlExpr& r) {
        if (r.IsPseudoReg()) {
          if (const auto it = demote_to_hr.find(r.RegNo()); it != demote_to_hr.end()) {
            const int hr = it->second;
            if (hr >= 0 && hr < 64) {
              const int span = std::max(1, MipsHardRegNregs(hr, r.mode, gp64, fp64));
              for (int w = 0; w < span && hr + w < 64; ++w) {
                insn_mask |= (uint64_t{1} << (hr + w));
              }
            }
            r.SetRegNo(hr);
            if (hr == kFramePointerRegno) {
              r.str_val = "$fp";
            }
          }
        }
      };
      pn.RewriteRegs(rewrite_demoted);
      scratch_masks[b][i] = insn_mask;
      if (insn_mask != 0) {
        CleanInvalidOrDuplicateRegDeadNotesOnInsn(&pn, gp64);
      }
    }
  }
  for (const auto& [p, _] : demote_to_hr) {
    out->sideband.reg_renumber.erase(p);
  }
  return scratch_masks;
}

// Discovers proven 8-byte aligned big-endian `reload` stack-spill slots in
// `[min_off, frame_top)` while tagging caller-save slots. Per
// `reload1.c:658, 1446, 2253, 3499`, `alter_reg` allocates 8-byte aligned spill
// slots both before `setup_save_areas` (initial unallocated pseudos) and after
// `setup_save_areas` (subsequent reload passes calling `spill_hard_reg`).
absl::flat_hash_map<int64_t, RtlMachineMode> FindProvenReloadSpillSlots(
    const ParsedCompilerFlags& compiler_flags, const std::vector<std::vector<int>>& cs_save_span,
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const RtlControlFlowGraph& out) {
  absl::flat_hash_map<int64_t, RtlMachineMode> proven_spill_offs;
  if (compiler_flags.opt_level <= 0 || out.sideband.frame_size_bytes <= 0 ||
      out.sideband.outgoing_args_size < 0) {
    return proven_spill_offs;
  }
  const int elim_reg = out.sideband.frame_pointer_needed ? kFramePointerRegno : 29;
  const int64_t min_off = out.sideband.outgoing_args_size;
  int64_t pre_cs_top = min_off + out.sideband.frame_size_bytes;
  if (((pre_cs_top - min_off) % 8) == 4) {
    bool odd_word_used = false;
    for (size_t b = 0; b < parsed_blocks.size() && !odd_word_used; ++b) {
      for (const RtlPatternAndNotes& pn : parsed_blocks[b]) {
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
  if (cand_bytes < 8 || (cand_bytes % 8) != 0) {
    return proven_spill_offs;
  }
  const int num_slots = static_cast<int>(cand_bytes / 8);
  struct SlotInfo {
    bool invalidated = false;
    bool is_caller_save = false;
    bool has_access = false;
    bool has_store = false;
    bool has_load = false;
    bool has_live_note = false;
    RtlMachineMode mode = RtlMachineMode::kNone;
    int64_t exact_off = -1;
  };
  std::vector<SlotInfo> slots(num_slots);
  for (size_t b = 0; b < parsed_blocks.size(); ++b) {
    for (size_t i = 0; i < parsed_blocks[b].size(); ++i) {
      if (b < cs_save_span.size() && i < cs_save_span[b].size() && cs_save_span[b][i] > 0) {
        const RtlExpr& dst = parsed_blocks[b][i].pattern.SetDest();
        if (dst.op == RtlOp::kMem && dst.children.size() == 1) {
          if (const auto off = ExtractBaseRegOffset(dst.children[0], elim_reg);
              off.has_value() && *off >= min_off && *off < pre_cs_top) {
            const int sz = std::max(1, MipsModeByteSize(dst.mode));
            const int fs = static_cast<int>((*off - min_off) / 8);
            const int ls =
                static_cast<int>(std::min<int64_t>(num_slots - 1, (*off - min_off + sz - 1) / 8));
            for (int s = std::max(0, fs); s <= ls; ++s) {
              slots[s].is_caller_save = true;
            }
          }
        }
      }
    }
  }
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
          const bool is_fp_mode = (e.mode == RtlMachineMode::kSF || e.mode == RtlMachineMode::kDF);
          if (e.flags.volatil || e.flags.in_struct || e.mode == RtlMachineMode::kBLK || sz > 8 ||
              fs != ls || (compiler_flags.flag_float_store && is_fp_mode)) {
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
      const bool pass_dst = in_dst && (e.op == RtlOp::kSubreg || e.op == RtlOp::kStrictLowPart ||
                                       e.op == RtlOp::kZeroExtract || e.op == RtlOp::kSignExtract);
      for (size_t ci = 0; ci < e.children.size(); ++ci) {
        self(self, e.children[ci], in_addr,
             e.op == RtlOp::kSet ? (ci == 0) : (pass_dst && ci == 0));
      }
    };
    walk(walk, root, false, false);
  };

  for (const auto& blk : parsed_blocks) {
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
  while (non_inv_begin > 0 &&
         (!slots[non_inv_begin - 1].invalidated || slots[non_inv_begin - 1].is_caller_save)) {
    --non_inv_begin;
  }
  int first_spill = non_inv_begin;
  if (non_inv_begin > 0) {
    while (first_spill < num_slots && !slots[first_spill].has_access) {
      ++first_spill;
    }
  }
  for (int s = first_spill; s < num_slots; ++s) {
    if (slots[s].has_access && !slots[s].invalidated && !slots[s].is_caller_save) {
      proven_spill_offs[slots[s].exact_off] = slots[s].mode;
    }
  }
  return proven_spill_offs;
}

// Mirrors `reload1.c:517-524, 566-573, 2769-2782, 6583` (`reg_equiv_constant`
// + `reg_last_reload_reg`): in a self-loop block with a unique preheader,
// demote rematerialized `const_double:SF/DF` input reloads into proven
// call-used FP spill regs so `DetectLocalResolutionGroupConflicts` sees them
// as reload scratches rather than injecting false `qty_phys_sugg` preferences.
void DemoteSelfLoopRematerializedConstDoubleScratches(
    bool gp64, bool fp64, const absl::flat_hash_map<int, ScratchCandStats>& stats,
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const RtlControlFlowGraph& out, absl::flat_hash_map<int, int>* demote_to_hr) {
  absl::flat_hash_set<int> proven_fp_spill_hrs;
  for (const auto& [p, hr] : *demote_to_hr) {
    if (hr >= 32 && hr < 52) {
      proven_fp_spill_hrs.insert(hr);
    }
  }
  if (proven_fp_spill_hrs.empty()) {
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
  auto is_barrier_or_branch_or_call = [](const RtlExpr& pat) -> bool {
    if (pat.op == RtlOp::kCall || pat.op == RtlOp::kAsmInput || pat.op == RtlOp::kAsmOperands ||
        pat.op == RtlOp::kUnspecVolatile) {
      return true;
    }
    if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
      return true;
    }
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        if (ch.op == RtlOp::kCall || (ch.IsSet() && ch.SetSrc().op == RtlOp::kCall) ||
            (ch.IsSet() && ch.SetDest().op == RtlOp::kPc)) {
          return true;
        }
      }
    }
    return pat.IsSet() && pat.SetSrc().op == RtlOp::kCall;
  };
  auto insn_touches_hr = [&](const RtlExpr& pat, int hr, int nw) -> bool {
    bool hit = false;
    pat.Visit([&](const RtlExpr& n) {
      if (hit || n.op != RtlOp::kReg || n.IsArgPointerReg()) {
        return;
      }
      const int nhr = eff_hr(n);
      if (nhr < 0) {
        return;
      }
      const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
      if (nhr < hr + nw && hr < nhr + nnw) {
        hit = true;
      }
    });
    return hit;
  };
  auto has_reg_dead_for = [&](const RtlPatternAndNotes& pn, int regno, int hr) -> bool {
    for (const RtlRegNote& n : pn.reg_notes) {
      if (n.kind != RtlMachineMode::kRegDead || n.list_op != RtlOp::kExprList ||
          n.datum.op != RtlOp::kReg || n.datum.IsArgPointerReg()) {
        continue;
      }
      if (n.datum.RegNo() == regno || (hr >= 0 && eff_hr(n.datum) == hr)) {
        return true;
      }
    }
    return false;
  };
  for (size_t b = 0; b < parsed_blocks.size(); ++b) {
    if (b >= out.rpo_blocks.size()) {
      continue;
    }
    bool is_self_loop = false;
    for (size_t succ : out.rpo_blocks[b].successors) {
      if (succ == b) {
        is_self_loop = true;
        break;
      }
    }
    if (!is_self_loop) {
      continue;
    }
    int non_self_preds = 0;
    for (size_t p = 0; p < out.rpo_blocks.size(); ++p) {
      if (p == b) {
        continue;
      }
      for (size_t succ : out.rpo_blocks[p].successors) {
        if (succ == b) {
          ++non_self_preds;
          break;
        }
      }
    }
    if (non_self_preds != 1) {
      continue;
    }
    const auto& blk = parsed_blocks[b];
    for (size_t i = 0; i + 1 < blk.size(); ++i) {
      const RtlPatternAndNotes& pn = blk[i];
      if (!pn.reg_notes.empty() || !pn.pattern.IsSet()) {
        continue;
      }
      const RtlExpr& dst = pn.pattern.SetDest();
      const RtlExpr& src = pn.pattern.SetSrc();
      if (!dst.IsPseudoReg() || dst.flags.Any() || src.op != RtlOp::kConstDouble ||
          dst.mode != src.mode ||
          (dst.mode != RtlMachineMode::kSF && dst.mode != RtlMachineMode::kDF)) {
        continue;
      }
      const int p_cd = dst.RegNo();
      const auto sit = stats.find(p_cd);
      if (sit == stats.end() || sit->second.multi_block || sit->second.first_block != b ||
          sit->second.def_count != 1 || sit->second.use_count <= 0 || sit->second.has_flags ||
          sit->second.blocked || demote_to_hr->contains(p_cd)) {
        continue;
      }
      const auto rit = out.sideband.reg_renumber.find(p_cd);
      if (rit == out.sideband.reg_renumber.end() || !proven_fp_spill_hrs.contains(rit->second)) {
        continue;
      }
      const int cd_hr = rit->second;
      const int use_cnt = sit->second.use_count;
      const int nw = std::max(1, MipsHardRegNregs(cd_hr, dst.mode, gp64, fp64));
      int matched_uses = 0;
      bool valid = true;
      for (size_t k = i + 1; k < blk.size(); ++k) {
        if (blk[k].pattern.ContainsReg(p_cd)) {
          const RtlExpr& up = blk[k].pattern;
          const bool is_plain_copy_or_marker =
              up.op == RtlOp::kUse || up.op == RtlOp::kClobber ||
              (up.IsSet() && (up.SetSrc().op == RtlOp::kReg || up.SetSrc().op == RtlOp::kMem ||
                              up.SetDest().op == RtlOp::kMem));
          if (!is_plain_copy_or_marker && !is_barrier_or_branch_or_call(up) &&
              !has_reg_dead_for(blk[k], p_cd, cd_hr)) {
            int hits = 0;
            up.Visit([&](const RtlExpr& n) {
              if (n.IsPseudoReg() && n.RegNo() == p_cd) {
                ++hits;
              }
            });
            matched_uses += hits;
            if (matched_uses == use_cnt) {
              break;
            }
            continue;
          }
          valid = false;
          break;
        }
        if (is_barrier_or_branch_or_call(blk[k].pattern) ||
            insn_touches_hr(blk[k].pattern, cd_hr, nw)) {
          valid = false;
          break;
        }
      }
      if (valid && matched_uses == use_cnt) {
        (*demote_to_hr)[p_cd] = cd_hr;
      }
    }
  }
}

std::vector<std::vector<uint64_t>> DemoteReloadAndCallerSaveHardScratches(
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

      // 2a. `"f"` input reload: `(set:SF/DF p_fpr src_gpr) ... <fp_op(p_fpr)>`
      // (single-use, or `reload1.c:5186-5258` `choose_reload_regs` multi-use
      // inherited callee-saved-GPR -> call-used-FPR input reload).
      if (dst.IsPseudoReg() &&
          (dst.mode == RtlMachineMode::kSF || dst.mode == RtlMachineMode::kDF) && src.IsReg() &&
          src.mode == dst.mode &&
          is_single_block_single_def_scratch(dst.RegNo(), b, /*req_uses=*/-1)) {
        const int p_fpr = dst.RegNo();
        const int fpr_hr = out->sideband.reg_renumber.at(p_fpr);
        const int src_hr = eff_hr(src);
        const int use_cnt = stats.at(p_fpr).use_count;
        const bool gpr_callee_saved = (src_hr >= 16 && src_hr <= 23) || src_hr == 30;
        const bool allowed_uses =
            (use_cnt == 1) || (compiler_flags.opt_level > 0 && use_cnt >= 2 && fpr_hr >= 32 &&
                               fpr_hr < 52 && gpr_callee_saved);
        if (allowed_uses && fpr_hr >= 32 && fpr_hr <= 63 && src_hr >= 2 && src_hr <= 31) {
          const int nw = std::max(1, MipsHardRegNregs(fpr_hr, dst.mode, gp64, fp64));
          int matched_uses = 0;
          bool valid = true;
          for (size_t k = i + 1; k < blk.size(); ++k) {
            if (blk[k].pattern.ContainsReg(p_fpr)) {
              const RtlExpr& up = blk[k].pattern;
              const bool is_plain_copy_or_marker =
                  up.op == RtlOp::kUse || up.op == RtlOp::kClobber ||
                  (up.IsSet() && (up.SetSrc().op == RtlOp::kReg || up.SetSrc().op == RtlOp::kMem ||
                                  up.SetDest().op == RtlOp::kMem));
              if (!is_plain_copy_or_marker && !is_barrier_or_branch_or_call(up) &&
                  (compiler_flags.opt_level == 0 || !has_reg_dead_for(blk[k], p_fpr, fpr_hr))) {
                int hits = 0;
                up.Visit([&](const RtlExpr& n) {
                  if (n.IsPseudoReg() && n.RegNo() == p_fpr) {
                    ++hits;
                  }
                });
                matched_uses += hits;
                if (matched_uses == use_cnt) {
                  break;
                }
                continue;
              }
              valid = false;
              break;
            }
            if (is_barrier_or_branch_or_call(blk[k].pattern) ||
                insn_touches_hr(blk[k].pattern, fpr_hr, nw)) {
              valid = false;
              break;
            }
          }
          if (valid && matched_uses == use_cnt) {
            demote_to_hr[p_fpr] = fpr_hr;
          }
        }
      }

      // 2b. `opt_level > 0` `"=f"` output reload: `(set:SF/DF p_fpr <fp_op>) ... (set dst_gpr
      // p_fpr)` (plus optional `reload1.c:5186-5249` `choose_reload_regs` inherited FP reads).
      if (compiler_flags.opt_level > 0 && dst.IsPseudoReg() &&
          (dst.mode == RtlMachineMode::kSF || dst.mode == RtlMachineMode::kDF) &&
          src.op != RtlOp::kReg && src.op != RtlOp::kMem && src.op != RtlOp::kConstDouble &&
          src.op != RtlOp::kCall && src.op != RtlOp::kSubreg) {
        const int p_fpr = dst.RegNo();
        const auto sit = stats.find(p_fpr);
        const auto rit = out->sideband.reg_renumber.find(p_fpr);
        if (sit != stats.end() && rit != out->sideband.reg_renumber.end() &&
            !sit->second.multi_block && sit->second.first_block == b &&
            sit->second.def_count == 1 && sit->second.use_count >= 1 && !sit->second.has_flags &&
            !sit->second.blocked && rit->second >= 32 && rit->second <= 63) {
          const int fpr_hr = rit->second;
          const int nw = std::max(1, MipsHardRegNregs(fpr_hr, dst.mode, gp64, fp64));
          int matched_output_reloads = 0;
          int matched_uses = 0;
          bool output_reload_to_hard_arg_gpr = false;
          bool invalid_use = false;
          for (size_t k = i + 1; k < blk.size(); ++k) {
            if (blk[k].pattern.ContainsReg(p_fpr)) {
              const RtlPatternAndNotes& use_pn = blk[k];
              if (use_pn.pattern.IsSet() && use_pn.pattern.SetSrc().IsPseudoReg() &&
                  use_pn.pattern.SetSrc().RegNo() == p_fpr && use_pn.pattern.SetDest().IsReg()) {
                const int d_hr = eff_hr(use_pn.pattern.SetDest());
                const bool valid_d_gpr =
                    (d_hr >= 2 && d_hr <= 25) ||
                    (d_hr == kFramePointerRegno && !out->sideband.frame_pointer_needed);
                if (valid_d_gpr && !has_reg_dead_for(use_pn, p_fpr, fpr_hr)) {
                  if (use_pn.pattern.SetDest().IsHardReg() && d_hr >= 4 && d_hr <= 7) {
                    output_reload_to_hard_arg_gpr = true;
                  }
                  ++matched_output_reloads;
                  ++matched_uses;
                  if (matched_uses == sit->second.use_count) {
                    break;
                  }
                  continue;
                }
                invalid_use = true;
                break;
              }
              const RtlExpr& up = use_pn.pattern;
              const bool is_plain_copy_or_marker =
                  up.op == RtlOp::kUse || up.op == RtlOp::kClobber ||
                  (up.IsSet() && (up.SetSrc().op == RtlOp::kReg || up.SetSrc().op == RtlOp::kMem ||
                                  up.SetDest().op == RtlOp::kMem));
              if (!is_plain_copy_or_marker && !is_barrier_or_branch_or_call(up) &&
                  !has_reg_dead_for(use_pn, p_fpr, fpr_hr)) {
                ++matched_uses;
                if (matched_uses == sit->second.use_count) {
                  break;
                }
                continue;
              }
              invalid_use = true;
              break;
            }
            if (is_barrier_or_branch_or_call(blk[k].pattern) ||
                insn_touches_hr(blk[k].pattern, fpr_hr, nw)) {
              break;
            }
          }
          // `cse.c:481-486`: a multi-use FP value copied to an outgoing call-arg hard register
          // `$a0..$a3` and also read by a pure FP arithmetic insn is a real `FP_REGS` pseudo.
          if (!invalid_use && matched_output_reloads == 1 &&
              matched_uses == sit->second.use_count &&
              (matched_uses <= 1 || !output_reload_to_hard_arg_gpr)) {
            demote_to_hr[p_fpr] = fpr_hr;
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
  const absl::flat_hash_map<int64_t, RtlMachineMode> proven_spill_offs =
      FindProvenReloadSpillSlots(compiler_flags, cs_save_span, *parsed_blocks, *out);
  if (!proven_spill_offs.empty()) {
    const int elim_reg = out->sideband.frame_pointer_needed ? kFramePointerRegno : 29;
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

    for (size_t b = 0; b < parsed_blocks->size(); ++b) {
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

  DemoteInheritedSpillOutputFprScratches(gp64, fp64, stats, proven_spill_output_scratches,
                                         *parsed_blocks, *out, &demote_to_hr);

  if (compiler_flags.opt_level > 0) {
    DemoteSelfLoopRematerializedConstDoubleScratches(gp64, fp64, stats, *parsed_blocks, *out,
                                                     &demote_to_hr);
  }
  absl::erase_if(demote_to_hr, [](const auto& kv) { return kv.second == kFramePointerRegno; });
  return ApplyDemotedReloadScratches(gp64, fp64, demote_to_hr, parsed_blocks, out);
}

// Reverses `alter_reg` on single-block `MD_REGS` (`64 hi` / `65 lo`) pseudos
// whose sole consumer required a GPR `"d"` input reload (`mflo`/`mfhi $gpr`),
// restoring `reg_renumber[p] = 64|65` and `(set (reg:SI gpr) (reg:SI p))`.
void PromoteNonConflictingSingleBlockMdPseudos(
    const ParsedCompilerFlags& compiler_flags, bool gp64,
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
    RtlMachineMode mode = RtlMachineMode::kSI;
  };

  for (size_t b = 0; b < parsed_blocks->size(); ++b) {
    auto& blk = (*parsed_blocks)[b];
    std::vector<MdPromo> promos;
    absl::flat_hash_set<size_t> covered_md_insns;
    for (size_t i = 0; i + 2 < blk.size(); ++i) {
      const RtlExpr& p0 = blk[i].pattern;
      if (p0.op != RtlOp::kParallel || p0.children.size() < 2 || !p0.children[0].IsSet() ||
          !p0.children[0].SetDest().IsHardReg()) {
        continue;
      }
      const int md_hr = p0.children[0].SetDest().RegNo();
      const RtlMachineMode md_mode = p0.children[0].SetDest().mode;
      const RtlOp op0 = p0.children[0].SetSrc().op;
      const bool is_si_md =
          p0.children.size() == 3 && p0.children[1].op == RtlOp::kClobber &&
          p0.children[2].op == RtlOp::kClobber && md_mode == RtlMachineMode::kSI &&
          ((md_hr == 64 && op0 == RtlOp::kTruncate) || (md_hr == 65 && op0 == RtlOp::kMult));
      // At `-O0` (`!gp64`), `stupid.c` allocates single-block `mulsidi3` /
      // `umulsidi3` (`=x` -> `64 hi`) pseudos stored straight to `MEM` to
      // `64 hi`, and `reload1.c` inserts `(set (reg:DI scr) (reg:DI 64 hi))`
      // ahead of `(set (mem:DI ...) (reg:DI scr))`.
      const bool is_o0_di_md = compiler_flags.opt_level == 0 && !gp64 && p0.children.size() == 2 &&
                               p0.children[1].op == RtlOp::kClobber &&
                               md_mode == RtlMachineMode::kDI && md_hr == 64 && op0 == RtlOp::kMult;
      if (!is_si_md && !is_o0_di_md) {
        continue;
      }
      size_t mv_idx = blk.size();
      for (size_t k = i + 1; k < blk.size(); ++k) {
        const RtlExpr& pk = blk[k].pattern;
        if (pk.IsSet() && pk.SetDest().IsPseudoReg() && pk.SetDest().mode == md_mode &&
            pk.SetSrc().IsHardReg() && pk.SetSrc().mode == md_mode &&
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
          rit->second > 25 || (is_o0_di_md && ((rit->second % 2) != 0 || rit->second > 24))) {
        continue;
      }
      const int gpr = rit->second;
      const int gpr_span = is_o0_di_md ? 2 : 1;
      size_t use_idx = blk.size();
      for (size_t k = mv_idx + 1; k < blk.size(); ++k) {
        if (blk[k].pattern.ContainsReg(p_scr)) {
          use_idx = k;
          break;
        }
        bool touches_gpr = false;
        blk[k].pattern.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
            const int nhr = eff_hr(n);
            if (nhr >= gpr && nhr < gpr + gpr_span) {
              touches_gpr = true;
            }
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
          is_o0_di_md ? (uop == RtlOp::kReg && up.SetDest().op == RtlOp::kMem &&
                         up.SetDest().mode == RtlMachineMode::kDI && up.SetSrc().IsPseudoReg() &&
                         up.SetSrc().RegNo() == p_scr && !up.SetDest().ContainsReg(p_scr))
                      : ((uop == RtlOp::kFloat && up.SetDest().IsReg() &&
                          eff_hr(up.SetDest()) >= 32 && eff_hr(up.SetDest()) <= 63) ||
                         (compiler_flags.opt_level > 0 && uop == RtlOp::kReg && non_fp &&
                          up.SetDest().op == RtlOp::kMem && up.SetSrc().IsPseudoReg() &&
                          up.SetSrc().RegNo() == p_scr) ||
                         (non_fp && up.SetDest().IsReg() && uop != RtlOp::kReg &&
                          uop != RtlOp::kMem && uop != RtlOp::kDiv && uop != RtlOp::kUdiv &&
                          uop != RtlOp::kMod && uop != RtlOp::kUmod && uop != RtlOp::kMult &&
                          uop != RtlOp::kFloat && uop != RtlOp::kUnsignedFloat));
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
          .mode = md_mode,
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
          RtlExpr::MakeReg(pr.mode, pr.p_scratch, md_flags);
      blk[pr.mv_idx].pattern = RtlExpr::MakeSet(RtlExpr::MakeReg(pr.mode, pr.scratch_gpr),
                                                RtlExpr::MakeReg(pr.mode, pr.p_scratch, md_flags));
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
          n.datum = RtlExpr::MakeReg(pr.mode, pr.p_scratch, md_flags);
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

// Mirrors `reload1.c:2352-2358` (`alter_reg` -> `assign_stack_local(..., -1)`
// with MIPS `BIGGEST_ALIGNMENT == 8` and big-endian offset `slot_base + (8 - sz)`)
// and `invert_reload.cc:3110-3200`: reload spill slots occupy only the contiguous
// upper 8-byte suffix above any pre-reload local aggregate, address-taken slot,
// `REG_EQUIV` stack slot, or non-big-endian-aligned access (e.g. `SImode` at
// `slot_base + 0` inside a local `s32[]` stack array).
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
  const int num_slots = static_cast<int>((max_off - min_off) / 8);
  struct SlotCheck {
    bool hard_invalidated = false;
    bool non_bigend_off = false;
    bool has_access = false;
    RtlMachineMode mode = RtlMachineMode::kVoid;
    int64_t exact_offset = -1;
  };
  std::vector<SlotCheck> slots(static_cast<size_t>(std::max(0, num_slots)));
  absl::flat_hash_map<int64_t, int> stored_off_bytes;
  absl::flat_hash_set<int64_t> loaded_word_offs;
  int64_t min_escaped_off = max_off;

  auto record_slot_mem = [&](const RtlExpr& mem, int64_t off, bool force_invalidate) {
    if (num_slots <= 0 || off < min_off || off >= max_off) {
      return;
    }
    const int sz = std::max(1, MipsModeByteSize(mem.mode));
    const int64_t rel_lo = off - min_off;
    const int64_t rel_hi = rel_lo + sz;
    const int first_s = static_cast<int>(rel_lo / 8);
    const int last_s = static_cast<int>(std::min<int64_t>(num_slots - 1, (rel_hi - 1) / 8));
    if (force_invalidate || mem.flags.volatil || mem.flags.in_struct ||
        mem.mode == RtlMachineMode::kBlk || sz > 8 || first_s != last_s) {
      for (int s = std::max(0, first_s); s <= last_s; ++s) {
        slots[static_cast<size_t>(s)].hard_invalidated = true;
      }
      return;
    }
    const int64_t slot_base = min_off + 8 * int64_t{first_s};
    const int64_t expected_bigend_off = slot_base + (8 - sz);
    SlotCheck& sc = slots[static_cast<size_t>(first_s)];
    if (off != expected_bigend_off) {
      sc.non_bigend_off = true;
    }
    if (!sc.has_access) {
      sc.has_access = true;
      sc.mode = mem.mode;
      sc.exact_offset = off;
    } else if (sc.mode != mem.mode || sc.exact_offset != off) {
      sc.hard_invalidated = true;
    }
  };

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
      if (expr.children[0].op == RtlOp::kMem && expr.children[0].children.size() == 1) {
        const std::optional<int64_t> off =
            ExtractBaseRelativeConstOffset(expr.children[0].children[0], base_regno);
        if (off.has_value() && *off >= min_off && *off < max_off) {
          record_slot_mem(expr.children[0], *off, /*force_invalidate=*/false);
          if (!expr.children[0].flags.volatil && !expr.children[0].flags.in_struct &&
              expr.children[0].mode != RtlMachineMode::kBlk && expr.children[1].op == RtlOp::kReg &&
              expr.children[1].RegNo() >= 0 && expr.children[1].RegNo() < kFirstPseudoRegister) {
            const int sz = std::max(4, MipsModeByteSize(expr.children[0].mode));
            stored_off_bytes[*off] = std::max(stored_off_bytes[*off], sz);
          }
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
        if (*off >= min_off && *off < max_off) {
          record_slot_mem(expr, *off, /*force_invalidate=*/false);
        }
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
      for (const RtlRegNote& note : parsed_blocks[b][i].reg_notes) {
        if (note.kind == RtlMachineMode::kRegEquiv && note.datum.op == RtlOp::kMem &&
            note.datum.children.size() == 1) {
          if (const std::optional<int64_t> off =
                  ExtractBaseRelativeConstOffset(note.datum.children[0], base_regno);
              off.has_value()) {
            record_slot_mem(note.datum, *off, /*force_invalidate=*/true);
          }
        }
      }
    }
  }

  bool any_hard_invalidated = (min_escaped_off < max_off);
  for (int s = 0; s < num_slots; ++s) {
    if (slots[static_cast<size_t>(s)].hard_invalidated) {
      any_hard_invalidated = true;
      break;
    }
  }
  int non_invalidated_begin = num_slots;
  while (non_invalidated_begin > 0) {
    const SlotCheck& sc = slots[static_cast<size_t>(non_invalidated_begin - 1)];
    if (sc.hard_invalidated || (any_hard_invalidated && sc.non_bigend_off)) {
      break;
    }
    --non_invalidated_begin;
  }
  const int64_t min_valid_spill_off = min_off + 8 * int64_t{non_invalidated_begin};

  for (const auto& [off, sz] : stored_off_bytes) {
    if (off < min_valid_spill_off || off + sz > min_escaped_off) {
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

int LookupMipsAllocOrderIndex(int hr) {
  static constexpr std::array<int, 57> kMipsAllocOrder = {
      2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
      21, 22, 23, 24, 25, 30, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
      45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};
  for (size_t i = 0; i < kMipsAllocOrder.size(); ++i) {
    if (kMipsAllocOrder[i] == hr) {
      return static_cast<int>(i);
    }
  }
  return 1000 + hr;
}

void SyncLiveProbeFromParsedBlocks(
    const CompilationProfile& profile,
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks,
    RtlControlFlowGraph* live_probe) {
  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    RtlBasicBlock& blk = live_probe->rpo_blocks[b];
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
  ReconstructRtlLiveness(live_probe, profile,
                         RtlLivenessOptions{
                             .reload_completed = false,
                             .update_reg_notes = true,
                             .include_call_reg_notes = true,
                         });
}

// Strips any `REG_DEAD` note for a pseudo register totally replaced by a
// `SET`/`CLOBBER` in `pn` (`flow.c:2506-2515` `!dead_or_set_p`,
// `rtlanal.c:1092-1199`). Hard-register `REG_DEAD` notes on a `SET`/`CLOBBER`
// of the same hard register and pre-existing duplicate `REG_DEAD` notes (e.g.,
// `jump.c:452-459` `remove_death` compensation notes) are preserved because
// `alter_reg` (`reload1.c:2309-2326`) only substitutes `REGNO` without deleting
// or deduplicating `REG_DEAD`.
void CleanInvalidOrDuplicateRegDeadNotesOnInsn(RtlPatternAndNotes* pn, bool gp64) {
  if (pn->reg_notes.empty()) {
    return;
  }
  const int word_bytes = gp64 ? 8 : 4;
  absl::flat_hash_set<int> pseudo_set_set;
  auto collect_set = [&](const RtlExpr& elem) {
    if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
      return;
    }
    const RtlExpr* dst = &elem.children[0];
    bool partial_write_wrapper = false;
    bool subreg_word_count_changed = false;
    if (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
      const int outer_bytes = GetStackSpillModeSizeBytes(dst->mode);
      const int inner_bytes = GetStackSpillModeSizeBytes(dst->children[0].mode);
      if (outer_bytes > 0 && inner_bytes > 0 &&
          ((outer_bytes + word_bytes - 1) / word_bytes) !=
              ((inner_bytes + word_bytes - 1) / word_bytes)) {
        subreg_word_count_changed = true;
      }
      dst = &dst->children[0];
    }
    while ((dst->op == RtlOp::kStrictLowPart || dst->op == RtlOp::kZeroExtract ||
            dst->op == RtlOp::kSignExtract) &&
           !dst->children.empty()) {
      partial_write_wrapper = true;
      dst = &dst->children[0];
    }
    if (!partial_write_wrapper && !subreg_word_count_changed && dst->IsPseudoReg()) {
      pseudo_set_set.insert(dst->RegNo());
    }
  };
  if (pn->pattern.IsParallel()) {
    for (const RtlExpr& ch : pn->pattern.children) {
      collect_set(ch);
    }
  } else {
    collect_set(pn->pattern);
  }
  std::vector<RtlRegNote> kept;
  kept.reserve(pn->reg_notes.size());
  for (RtlRegNote& n : pn->reg_notes) {
    if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
        !n.datum.IsArgPointerReg()) {
      const int r = n.datum.RegNo();
      if (r >= kFirstPseudoRegister && pseudo_set_set.contains(r)) {
        continue;
      }
    }
    kept.push_back(std::move(n));
  }
  pn->reg_notes = std::move(kept);
}

// Coalesces single-block local pseudos `p_loc` back into a disjoint global
// pseudo `p_glob` in the same block `b` when both share `(reg_renumber, mode,
// raw_mode, flags)` and `p_glob` either redefines the same register later in `b`
// (a multi-step pointer/carrier update chain in `b`) or enters `B0` live-in
// (sequential subword field overwrites of a `reg/v` union/struct in `B0`).
void CoalesceSameBlockDisjointGlobalChainPseudos(
    const CompilationProfile& profile, const ParsedCompilerFlags& parsed_flags, bool gp64,
    bool fp64, RtlControlFlowGraph* out,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr) {
  if (parsed_flags.opt_level == 0) {
    return;
  }
  auto& all_parsed_blocks = *all_parsed_blocks_ptr;
  RtlControlFlowGraph live_probe = *out;
  SyncLiveProbeFromParsedBlocks(profile, all_parsed_blocks, &live_probe);

  struct RegAppearance {
    absl::flat_hash_set<size_t> blocks;
    RtlMachineMode mode = RtlMachineMode::kNone;
    std::string raw_mode;
    RtlFlags flags;
    bool uniform_shape = true;
    bool has_set = false;
    bool has_reg_equiv = false;
    bool used_before_set = false;
    bool alive_after_redef = false;
    int note_deaths = 0;
  };
  absl::flat_hash_map<int, RegAppearance> app;
  for (size_t b = 0; b < live_probe.rpo_blocks.size(); ++b) {
    absl::flat_hash_set<int> def_so_far;
    absl::flat_hash_set<int> died_so_far;
    for (const std::string& s_insn : live_probe.rpo_blocks[b].instructions) {
      auto pn = ParsePatternAndNotes(s_insn);
      if (!pn.ok()) {
        continue;
      }
      absl::flat_hash_set<int> defs;
      absl::flat_hash_set<int> reads;
      auto scan = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
        if (e.op == RtlOp::kSet && e.children.size() >= 2) {
          self(self, e.children[0], true);
          for (size_t k = 1; k < e.children.size(); ++k) {
            self(self, e.children[k], false);
          }
          return;
        }
        if (in_dst && e.op == RtlOp::kMem) {
          for (const RtlExpr& ch : e.children) {
            self(self, ch, false);
          }
          return;
        }
        if (e.IsPseudoReg()) {
          RegAppearance& a = app[e.RegNo()];
          a.blocks.insert(b);
          if (a.mode == RtlMachineMode::kNone) {
            a.mode = e.mode;
            a.raw_mode = e.raw_mode;
            a.flags = e.flags;
          } else if (a.mode != e.mode || a.raw_mode != e.raw_mode || a.flags != e.flags) {
            a.uniform_shape = false;
          }
          if (in_dst) {
            defs.insert(e.RegNo());
          } else {
            reads.insert(e.RegNo());
          }
          return;
        }
        for (const RtlExpr& ch : e.children) {
          self(self, ch, in_dst);
        }
      };
      scan(scan, pn->pattern, false);
      for (int r : reads) {
        if (!def_so_far.contains(r)) {
          app[r].used_before_set = true;
        }
      }
      for (int d : defs) {
        app[d].has_set = true;
        def_so_far.insert(d);
        if (died_so_far.contains(d)) {
          app[d].alive_after_redef = true;
        }
      }
      for (const RtlRegNote& n : pn->reg_notes) {
        if (n.kind == RtlMachineMode::kRegEquiv) {
          for (int d : defs) {
            app[d].has_reg_equiv = true;
          }
        }
        if ((n.kind == RtlMachineMode::kRegDead || n.kind == RtlMachineMode::kRegUnused) &&
            n.datum.IsPseudoReg()) {
          const int dp = n.datum.RegNo();
          app[dp].blocks.insert(b);
          ++app[dp].note_deaths;
          died_so_far.insert(dp);
          app[dp].alive_after_redef = false;
        }
      }
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

  absl::flat_hash_set<int> is_global;
  for (const auto& [p, hr] : out->sideband.reg_renumber) {
    const auto dit = live_probe.sideband.reg_n_deaths.find(p);
    const auto ait = app.find(p);
    if (p < kFirstPseudoRegister || hr < 0 || hr >= 64 ||
        (out->sideband.frame_pointer_needed && hr == kFramePointerRegno) ||
        live_at_any_start.contains(p) || dit == live_probe.sideband.reg_n_deaths.end() ||
        dit->second != 1 || ait == app.end() || ait->second.blocks.size() != 1 ||
        !ait->second.has_set || ait->second.used_before_set || ait->second.alive_after_redef ||
        ait->second.note_deaths >= 2) {
      is_global.insert(p);
    }
  }

  absl::flat_hash_map<int, int> remap;
  for (size_t b = 0; b < live_probe.rpo_blocks.size(); ++b) {
    std::vector<RtlPatternAndNotes> blk_pns;
    blk_pns.reserve(live_probe.rpo_blocks[b].instructions.size());
    for (const std::string& s : live_probe.rpo_blocks[b].instructions) {
      auto pn = ParsePatternAndNotes(s);
      if (!pn.ok()) {
        return;
      }
      blk_pns.push_back(*std::move(pn));
    }
    const size_t m = blk_pns.size();
    if (m < 2) {
      continue;
    }

    std::vector<int> locals_in_b;
    std::vector<int> globals_in_b;
    for (const auto& [p, hr] : out->sideband.reg_renumber) {
      if (hr < 2 || hr >= 64 || (out->sideband.frame_pointer_needed && hr == kFramePointerRegno)) {
        continue;
      }
      const auto ait = app.find(p);
      if (ait == app.end() || !ait->second.uniform_shape || !ait->second.blocks.contains(b)) {
        continue;
      }
      if (is_global.contains(p)) {
        globals_in_b.push_back(p);
      } else {
        locals_in_b.push_back(p);
      }
    }
    if (locals_in_b.empty() || globals_in_b.empty()) {
      continue;
    }
    std::sort(locals_in_b.begin(), locals_in_b.end());
    std::sort(globals_in_b.begin(), globals_in_b.end());

    for (int p_glob : globals_in_b) {
      const int hr = out->sideband.reg_renumber.at(p_glob);
      const RegAppearance& ga = app.at(p_glob);
      const bool glob_live_in =
          live_probe.sideband.basic_block_live_at_start.contains(static_cast<int>(b)) &&
          live_probe.sideband.basic_block_live_at_start.at(static_cast<int>(b)).contains(p_glob);
      bool glob_live_out = false;
      for (size_t succ : out->rpo_blocks[b].successors) {
        if (live_probe.sideband.basic_block_live_at_start.contains(static_cast<int>(succ)) &&
            live_probe.sideband.basic_block_live_at_start.at(static_cast<int>(succ))
                .contains(p_glob)) {
          glob_live_out = true;
          break;
        }
      }
      std::vector<bool> glob_live_during(m, false);
      bool cur_live = glob_live_in;
      std::optional<size_t> first_death_i;
      std::optional<size_t> redef_after_death_i;
      for (size_t i = 0; i < m; ++i) {
        bool sets_g = false;
        bool reads_g = false;
        blk_pns[i].pattern.Visit([&](const RtlExpr& e) {
          if (e.IsPseudoReg() && e.RegNo() == p_glob) {
            reads_g = true;
          }
        });
        if (blk_pns[i].pattern.IsSet() && blk_pns[i].pattern.SetDest().IsPseudoReg() &&
            blk_pns[i].pattern.SetDest().RegNo() == p_glob) {
          sets_g = true;
        }
        const bool dead_g = std::any_of(blk_pns[i].reg_notes.begin(), blk_pns[i].reg_notes.end(),
                                        [&](const RtlRegNote& n) {
                                          return n.kind == RtlMachineMode::kRegDead &&
                                                 n.datum.IsPseudoReg() && n.datum.RegNo() == p_glob;
                                        });
        if (cur_live || sets_g || reads_g) {
          glob_live_during[i] = true;
        }
        if (sets_g) {
          if (first_death_i.has_value() && !redef_after_death_i.has_value()) {
            redef_after_death_i = i;
          }
          cur_live = true;
        } else if (dead_g) {
          if (!first_death_i.has_value()) {
            first_death_i = i;
          }
          cur_live = false;
        }
      }
      if (!first_death_i.has_value() ||
          (!redef_after_death_i.has_value() &&
           (b != 0 || !live_at_b0_start.contains(p_glob) || glob_live_out))) {
        continue;
      }

      for (int p_loc : locals_in_b) {
        if (remap.contains(p_loc) || out->sideband.reg_renumber.at(p_loc) != hr) {
          continue;
        }
        const RegAppearance& la = app.at(p_loc);
        if (ga.has_reg_equiv || la.has_reg_equiv || la.mode != ga.mode ||
            la.raw_mode != ga.raw_mode || la.flags != ga.flags) {
          continue;
        }
        size_t l_birth = m;
        size_t l_death = 0;
        for (size_t i = 0; i < m; ++i) {
          if (blk_pns[i].pattern.ContainsReg(p_loc)) {
            l_birth = std::min(l_birth, i);
            l_death = std::max(l_death, i);
          }
        }
        if (l_birth >= m || l_birth <= *first_death_i ||
            (redef_after_death_i.has_value() && l_death >= *redef_after_death_i)) {
          continue;
        }
        bool conflict = false;
        for (size_t i = l_birth; i <= l_death; ++i) {
          if (glob_live_during[i] ||
              (IsCallUsedHardReg(hr) && ContainsCallOp(blk_pns[i].pattern))) {
            conflict = true;
            break;
          }
        }
        if (!conflict) {
          remap[p_loc] = p_glob;
          for (size_t i = l_birth; i <= l_death; ++i) {
            glob_live_during[i] = true;
          }
        }
      }
    }
  }

  if (remap.empty()) {
    return;
  }
  for (auto& blk : all_parsed_blocks) {
    for (RtlPatternAndNotes& pn : blk) {
      bool changed = false;
      pn.RewriteRegs([&](RtlExpr& r) {
        if (r.IsPseudoReg()) {
          if (const auto it = remap.find(r.RegNo()); it != remap.end()) {
            r.SetRegNo(it->second);
            changed = true;
          }
        }
      });
      if (changed) {
        CleanInvalidOrDuplicateRegDeadNotesOnInsn(&pn, gp64);
      }
    }
  }
  for (const auto& [dead_p, _] : remap) {
    out->sideband.reg_renumber.erase(dead_p);
  }
}

struct LocalResolutionCandInfo {
  absl::flat_hash_set<size_t> blocks;
  RtlMachineMode mode = RtlMachineMode::kSI;
  std::string raw_mode;
  RtlFlags flags;
  bool uniform_shape = true;
  bool has_set = false;
  bool has_reg_equiv = false;
  bool used_before_set = false;
  bool alive_after_redef = false;
  bool used_in_mem_addr = false;
  bool defined_from_md_reg = false;
  int def_count = 0;
  int read_count = 0;
  int note_deaths = 0;
};

bool CollectLocalResolutionCandInfo(const RtlControlFlowGraph& live_probe,
                                    const absl::flat_hash_map<int, int>& reg_renumber,
                                    absl::flat_hash_map<int, LocalResolutionCandInfo>* info,
                                    std::vector<std::vector<RtlPatternAndNotes>>* live_blocks) {
  live_blocks->assign(live_probe.rpo_blocks.size(), {});
  for (size_t b = 0; b < live_probe.rpo_blocks.size(); ++b) {
    absl::flat_hash_set<int> def_so_far;
    absl::flat_hash_set<int> died_so_far;
    (*live_blocks)[b].reserve(live_probe.rpo_blocks[b].instructions.size());
    for (const std::string& s : live_probe.rpo_blocks[b].instructions) {
      auto pn = ParsePatternAndNotes(s);
      if (!pn.ok()) {
        return false;
      }
      absl::flat_hash_set<int> defs;
      absl::flat_hash_set<int> reads;
      auto scan = [&](const auto& self, const RtlExpr& e, bool in_dst, bool in_mem) -> void {
        if (e.op == RtlOp::kSet && e.children.size() >= 2) {
          if (e.children[0].IsPseudoReg() && e.children[1].op == RtlOp::kReg &&
              !e.children[1].IsArgPointerReg()) {
            int src_hr = e.children[1].RegNo();
            if (src_hr >= kFirstPseudoRegister) {
              const auto rit = reg_renumber.find(src_hr);
              src_hr = (rit != reg_renumber.end()) ? rit->second : -1;
            }
            if (src_hr == 64 || src_hr == 65) {
              (*info)[e.children[0].RegNo()].defined_from_md_reg = true;
            }
          }
          self(self, e.children[0], true, false);
          for (size_t k = 1; k < e.children.size(); ++k) {
            self(self, e.children[k], false, false);
          }
          return;
        }
        if (e.op == RtlOp::kClobber) {
          if (!e.children.empty() && e.children[0].op == RtlOp::kMem) {
            self(self, e.children[0], false, true);
          } else if (!e.children.empty()) {
            e.children[0].Visit([&](const RtlExpr& r) {
              if (r.IsPseudoReg()) {
                defs.insert(r.RegNo());
              }
            });
          }
          return;
        }
        if (e.op == RtlOp::kMem) {
          for (const RtlExpr& ch : e.children) {
            self(self, ch, false, true);
          }
          return;
        }
        if (e.IsPseudoReg()) {
          LocalResolutionCandInfo& li = (*info)[e.RegNo()];
          li.blocks.insert(b);
          if (!li.has_set && !li.used_before_set && li.raw_mode.empty() &&
              li.mode == RtlMachineMode::kSI) {
            li.mode = e.mode;
            li.raw_mode = e.raw_mode;
            li.flags = e.flags;
          } else if (li.mode != e.mode || li.raw_mode != e.raw_mode || li.flags != e.flags) {
            li.uniform_shape = false;
            if (MipsModeByteSize(e.mode) > MipsModeByteSize(li.mode)) {
              li.mode = e.mode;
            }
          }
          if (in_dst) {
            defs.insert(e.RegNo());
            ++li.def_count;
          } else {
            reads.insert(e.RegNo());
            ++li.read_count;
            if (in_mem) {
              li.used_in_mem_addr = true;
            }
          }
          return;
        }
        for (const RtlExpr& ch : e.children) {
          self(self, ch, in_dst, in_mem);
        }
      };
      scan(scan, pn->pattern, false, false);
      for (int r : reads) {
        if (!def_so_far.contains(r)) {
          (*info)[r].used_before_set = true;
        }
      }
      for (int d : defs) {
        (*info)[d].has_set = true;
        def_so_far.insert(d);
        if (died_so_far.contains(d)) {
          (*info)[d].alive_after_redef = true;
        }
      }
      for (const RtlRegNote& n : pn->reg_notes) {
        if (n.kind == RtlMachineMode::kRegEquiv) {
          for (int d : defs) {
            (*info)[d].has_reg_equiv = true;
          }
        }
        if ((n.kind == RtlMachineMode::kRegDead || n.kind == RtlMachineMode::kRegUnused) &&
            n.datum.IsPseudoReg()) {
          const int dp = n.datum.RegNo();
          (*info)[dp].blocks.insert(b);
          ++(*info)[dp].note_deaths;
          died_so_far.insert(dp);
          (*info)[dp].alive_after_redef = false;
        }
      }
      (*live_blocks)[b].push_back(*std::move(pn));
    }
  }
  return true;
}

std::vector<uint64_t> ComputeLocalResolutionHardLiveTicks(
    const RtlControlFlowGraph& live_probe, size_t b, const std::vector<RtlPatternAndNotes>& b_insns,
    bool gp64, bool fp64) {
  const size_t m = b_insns.size();
  std::vector<uint64_t> base_live_ticks(2 * m, 0);
  auto eff_nonlocal_mask = [&](int regno, RtlMachineMode mode) -> uint64_t {
    if (regno < 0 || regno >= 64) {
      return 0;
    }
    uint64_t msk = 0;
    const int span = std::max(1, MipsHardRegNregs(regno, mode, gp64, fp64));
    for (int w = 0; w < span && regno + w < 64; ++w) {
      msk |= (uint64_t{1} << (regno + w));
    }
    return msk;
  };
  uint64_t cur_hard = 0;
  if (const auto lit = live_probe.sideband.basic_block_live_at_start.find(static_cast<int>(b));
      lit != live_probe.sideband.basic_block_live_at_start.end()) {
    for (int r : lit->second) {
      cur_hard |= eff_nonlocal_mask(r, RtlMachineMode::kSI);
    }
  }
  for (size_t i = 0; i < m; ++i) {
    const RtlPatternAndNotes& pn = b_insns[i];
    uint64_t clobber_mask = 0;
    uint64_t set_mask = 0;
    auto scan_defs = [&](const RtlExpr& elem) {
      if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
        return;
      }
      const RtlExpr* dst = &elem.children[0];
      int woff = 0;
      RtlMachineMode dm = dst->mode;
      if (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        woff = static_cast<int>(dst->int_val);
        dst = &dst->children[0];
      }
      if (dst->op == RtlOp::kReg && !dst->IsArgPointerReg()) {
        const int r = (dst->RegNo() < 64) ? (dst->RegNo() + woff) : dst->RegNo();
        const uint64_t msk = eff_nonlocal_mask(r, dm);
        if (elem.op == RtlOp::kClobber) {
          clobber_mask |= msk;
        } else {
          set_mask |= msk;
        }
      }
    };
    if (pn.pattern.IsParallel()) {
      for (const RtlExpr& ch : pn.pattern.children) {
        scan_defs(ch);
      }
    } else {
      scan_defs(pn.pattern);
    }
    cur_hard |= clobber_mask;
    base_live_ticks[2 * i] = cur_hard;
    for (const RtlRegNote& n : pn.reg_notes) {
      if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
          !n.datum.IsArgPointerReg()) {
        cur_hard &= ~eff_nonlocal_mask(n.datum.RegNo(), n.datum.mode);
      }
    }
    cur_hard |= set_mask;
    // Mirrors `global.c:729-767` (`global_conflicts`): `note_stores(...,
    // mark_reg_store)` records birth conflicts for every stored register BEFORE
    // `REG_UNUSED` calls `mark_reg_death`, so an unused hard-reg write still
    // occupies tick `2 * i + 1` against any pseudo live across insn `i`.
    base_live_ticks[2 * i + 1] = cur_hard;
    for (const RtlRegNote& n : pn.reg_notes) {
      if (n.kind == RtlMachineMode::kRegUnused && n.datum.op == RtlOp::kReg &&
          !n.datum.IsArgPointerReg()) {
        cur_hard &= ~eff_nonlocal_mask(n.datum.RegNo(), n.datum.mode);
      }
    }
  }
  return base_live_ticks;
}

bool IsDemotedOutputReloadHardScratch(const std::vector<RtlPatternAndNotes>& raw_blk,
                                      const std::vector<RtlPatternAndNotes>& b_insns,
                                      const std::vector<uint64_t>& base_live_ticks,
                                      const RtlControlFlowGraph& cfg, bool gp64, bool fp64,
                                      size_t i, int dhr, RtlMachineMode dmode) {
  const size_t m = b_insns.size();
  if (i + 1 >= m || !b_insns[i].pattern.IsSet() || b_insns[i].pattern.SetSrc().op == RtlOp::kReg ||
      b_insns[i].pattern.SetSrc().op == RtlOp::kMem) {
    return false;
  }
  uint64_t dmask = 0;
  const int span = std::max(1, MipsHardRegNregs(dhr, dmode, gp64, fp64));
  for (int w = 0; w < span && dhr + w < 64; ++w) {
    dmask |= (uint64_t{1} << (dhr + w));
  }
  if (dhr >= 32 && dhr < 64 && b_insns[i + 1].pattern.IsSet() &&
      b_insns[i + 1].pattern.SetSrc().IsHardReg() &&
      b_insns[i + 1].pattern.SetSrc().RegNo() == dhr) {
    const RtlExpr& nd = b_insns[i + 1].pattern.SetDest();
    int nd_hr = -1;
    if (nd.op == RtlOp::kReg && !nd.IsArgPointerReg()) {
      nd_hr = nd.RegNo();
      if (nd_hr >= kFirstPseudoRegister && cfg.sideband.reg_renumber.contains(nd_hr)) {
        nd_hr = cfg.sideband.reg_renumber.at(nd_hr);
      }
    }
    if (nd_hr >= 2 && nd_hr < 32 && 2 * (i + 1) + 1 < base_live_ticks.size() &&
        (base_live_ticks[2 * (i + 1) + 1] & dmask) == 0) {
      return true;
    }
  }
  const int elim_r = cfg.sideband.frame_pointer_needed ? kFramePointerRegno : 29;
  for (size_t st_k = i + 1; st_k < m; ++st_k) {
    const RtlExpr& kpat = b_insns[st_k].pattern;
    if (kpat.IsSet() && kpat.SetDest().op == RtlOp::kMem && !kpat.SetDest().flags.volatil &&
        !kpat.SetDest().flags.in_struct && kpat.SetDest().children.size() == 1 &&
        kpat.SetSrc().IsHardReg() && kpat.SetSrc().RegNo() == dhr && kpat.SetSrc().mode == dmode &&
        ExtractBaseRegOffset(kpat.SetDest().children[0], elim_r).has_value()) {
      const bool raw_has_dead =
          st_k < raw_blk.size() &&
          std::any_of(raw_blk[st_k].reg_notes.begin(), raw_blk[st_k].reg_notes.end(),
                      [&](const RtlRegNote& n) {
                        return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                               !n.datum.IsArgPointerReg() && n.datum.RegNo() == dhr;
                      });
      return st_k < raw_blk.size() && !raw_has_dead && 2 * st_k + 1 < base_live_ticks.size() &&
             (base_live_ticks[2 * st_k + 1] & dmask) == 0;
    }
    if (ContainsCallOp(kpat) || kpat.op == RtlOp::kSequence ||
        (kpat.IsSet() && kpat.SetDest().op == RtlOp::kPc) || kpat.ContainsReg(dhr)) {
      return false;
    }
  }
  return false;
}

int CountLocalScratchClobbersInBlock(const std::vector<RtlPatternAndNotes>& b_insns) {
  int count = 0;
  for (const RtlPatternAndNotes& pn : b_insns) {
    const RtlExpr& pat = pn.pattern;
    if (pat.op != RtlOp::kParallel || ContainsCallOp(pat)) {
      continue;
    }
    bool is_scratch_par = false;
    for (const RtlExpr& elem : pat.children) {
      if (!elem.IsSet()) {
        continue;
      }
      const RtlOp rop = elem.SetSrc().op;
      if ((elem.SetDest().IsMem() && elem.SetDest().mode == RtlMachineMode::kBlk) ||
          rop == RtlOp::kMult || rop == RtlOp::kDiv || rop == RtlOp::kMod || rop == RtlOp::kUdiv ||
          rop == RtlOp::kUmod || rop == RtlOp::kTruncate || rop == RtlOp::kFfs ||
          rop == RtlOp::kFix) {
        is_scratch_par = true;
        break;
      }
    }
    if (!is_scratch_par) {
      continue;
    }
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kClobber && elem.children.size() == 1 && elem.children[0].IsHardReg() &&
          !elem.children[0].IsArgPointerReg()) {
        const int hr = elem.children[0].RegNo();
        if (hr >= 2 && hr <= 66 && hr != 26 && hr != 27 && hr != 28 && hr != 29 && hr != 31) {
          ++count;
        }
      }
    }
  }
  return count;
}

absl::flat_hash_map<int, uint64_t> ComputeLocalResolutionGlobalHardConflicts(
    const RtlControlFlowGraph& live_probe,
    const std::vector<std::vector<RtlPatternAndNotes>>& live_blocks,
    const absl::flat_hash_map<int, int>& reg_renumber,
    const absl::flat_hash_set<int>& is_local_cand, bool gp64, bool fp64) {
  absl::flat_hash_map<int, uint64_t> conflicts;
  auto resolve_reg = [&](int r) {
    if (r >= kFirstPseudoRegister && is_local_cand.contains(r)) {
      const auto it = reg_renumber.find(r);
      return (it != reg_renumber.end()) ? it->second : -1;
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
        } else if (eff >= kFirstPseudoRegister) {
          global_live.insert(eff);
        }
      }
    }
    for (int gp : global_live) {
      conflicts[gp] |= hard_live;
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
          const int eff = resolve_reg(dst->RegNo());
          if (eff >= kFirstPseudoRegister) {
            global_live.insert(eff);
            conflicts[eff] |= hard_live;
          } else if (eff >= 0 && eff < 64) {
            const int nregs = std::max(1, MipsHardRegNregs(eff, dst->mode, gp64, fp64));
            for (int k = eff; k < eff + nregs && k < 64; ++k) {
              for (int gp : global_live) {
                conflicts[gp] |= (uint64_t{1} << k);
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
      if (ContainsCallOp(pn.pattern)) {
        for (int h = 0; h < 64; ++h) {
          if (IsCallUsedHardReg(h)) {
            for (int gp : global_live) {
              conflicts[gp] |= (uint64_t{1} << h);
            }
            hard_live &= ~(uint64_t{1} << h);
          }
        }
      }
      for (const RtlRegNote& note : pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
            !note.datum.IsArgPointerReg()) {
          const int eff = resolve_reg(note.datum.RegNo());
          if (eff >= kFirstPseudoRegister) {
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
  return conflicts;
}

bool ApplyLocalResolutionRewrites(const absl::flat_hash_map<int, int>& coalesce_into,
                                  const absl::flat_hash_map<int, int>& demote_to_hr, bool gp64,
                                  bool fp64,
                                  std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
                                  RtlControlFlowGraph* out) {
  if (coalesce_into.empty() && demote_to_hr.empty()) {
    return false;
  }
  auto resolve_coalesce_target = [&](int regno) -> int {
    int cur = regno;
    for (size_t hops = 0; hops <= coalesce_into.size(); ++hops) {
      const auto cit = coalesce_into.find(cur);
      if (cit == coalesce_into.end() || cit->second == cur) {
        break;
      }
      cur = cit->second;
    }
    return cur;
  };
  for (auto& blk : *all_parsed_blocks) {
    for (RtlPatternAndNotes& pn : blk) {
      if (!coalesce_into.empty()) {
        bool coalesced_here = false;
        pn.RewriteRegs([&](RtlExpr& r) {
          if (r.IsPseudoReg() && coalesce_into.contains(r.RegNo())) {
            const int target = resolve_coalesce_target(r.RegNo());
            if (target != r.RegNo()) {
              r.SetRegNo(target);
              coalesced_here = true;
            }
          }
        });
        if (coalesced_here) {
          CleanInvalidOrDuplicateRegDeadNotesOnInsn(&pn, gp64);
        }
      }
      if (!demote_to_hr.empty()) {
        bool demoted_here = false;
        pn.RewriteRegs([&](RtlExpr& r) {
          if (r.IsPseudoReg()) {
            if (const auto dit = demote_to_hr.find(r.RegNo()); dit != demote_to_hr.end()) {
              r.SetRegNo(dit->second);
              if (dit->second == kFramePointerRegno) {
                r.str_val = "$fp";
              }
              demoted_here = true;
            }
          }
        });
        if (demoted_here) {
          CleanInvalidOrDuplicateRegDeadNotesOnInsn(&pn, gp64);
        }
      }
    }
  }
  for (const auto& [p, _] : coalesce_into) {
    if (resolve_coalesce_target(p) != p) {
      out->sideband.reg_renumber.erase(p);
    }
  }
  for (const auto& [p, _] : demote_to_hr) {
    out->sideband.reg_renumber.erase(p);
  }
  return true;
}

struct LocalQtyGroup {
  std::vector<int> members;
  int target_hr = -1;
  RtlMachineMode mode = RtlMachineMode::kSI;
  size_t birth_i = 0;
  size_t death_i = 0;
  bool crosses_call = false;
  bool has_any_sugg = false;
  bool wins_via_sugg = false;
  bool needs_resolution = false;
  bool stolen_by_sugg_thief = false;
  size_t max_thief_birth_i = 0;
  uint64_t raw_copy_sugg_mask = 0;
  uint64_t raw_sugg_mask = 0;
  int raw_num_copy_sugg = 0;
  int raw_num_sugg = 0;
  int sugg_rank = 1000000;
  int first_free_sugg_hr = -1;
};

bool IsCallCrossingReloadSpillFalseCombine(
    const RtlControlFlowGraph& out, const RtlControlFlowGraph& live_probe,
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks,
    const std::vector<RtlPatternAndNotes>& b_insns, const std::vector<uint64_t>& all_live_ticks,
    const absl::flat_hash_map<int, size_t>& p_death,
    const absl::flat_hash_set<int64_t>& reload_spill_offsets, size_t b, size_t def_i, int pd,
    const RtlExpr& sr, bool gp64, bool fp64) {
  if (reload_spill_offsets.empty() || sr.op != RtlOp::kReg || sr.IsArgPointerReg()) {
    return false;
  }
  const auto ren_it = out.sideband.reg_renumber.find(pd);
  if (ren_it == out.sideband.reg_renumber.end() || !IsCallUsedHardReg(ren_it->second)) {
    return false;
  }
  const auto cc_it = live_probe.sideband.reg_n_calls_crossed.find(pd);
  if (cc_it != live_probe.sideband.reg_n_calls_crossed.end() && cc_it->second > 0) {
    return false;
  }
  const int sr_no = sr.RegNo();
  const bool dead_at_def = std::any_of(
      b_insns[def_i].reg_notes.begin(), b_insns[def_i].reg_notes.end(), [&](const RtlRegNote& n) {
        return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
               !n.datum.IsArgPointerReg() && n.datum.RegNo() == sr_no;
      });
  if (!dead_at_def) {
    return false;
  }
  const int stack_base_reg = out.sideband.frame_pointer_needed ? kFramePointerRegno : 29;
  size_t load_k = all_parsed_blocks[b].size();
  int64_t spill_off = -1;
  for (size_t rev = def_i; rev > 0; --rev) {
    const size_t k = rev - 1;
    const RtlExpr& kpat = all_parsed_blocks[b][k].pattern;
    if (ContainsCallOp(kpat) || kpat.op == RtlOp::kSequence ||
        (kpat.IsSet() && kpat.SetDest().op == RtlOp::kPc)) {
      return false;
    }
    if (kpat.IsSet() && kpat.SetDest().op == RtlOp::kReg && !kpat.SetDest().IsArgPointerReg() &&
        kpat.SetDest().RegNo() == sr_no) {
      if (kpat.SetSrc().op != RtlOp::kMem || kpat.SetSrc().flags.volatil ||
          kpat.SetSrc().flags.in_struct || kpat.SetSrc().children.size() != 1 ||
          kpat.SetSrc().mode != sr.mode) {
        return false;
      }
      const std::optional<int64_t> off =
          ExtractBaseRelativeConstOffset(kpat.SetSrc().children[0], stack_base_reg);
      if (!off.has_value() || !reload_spill_offsets.contains(*off)) {
        return false;
      }
      load_k = k;
      spill_off = *off;
      break;
    }
    if (kpat.ContainsReg(sr_no)) {
      return false;
    }
  }
  if (load_k >= all_parsed_blocks[b].size()) {
    return false;
  }
  const int slot_sz = std::max(4, MipsModeByteSize(sr.mode));
  auto insn_touches_slot = [&](const RtlExpr& pat, bool* is_store_out) {
    bool touched = false;
    auto walk = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
      if (e.op == RtlOp::kSet && e.children.size() == 2) {
        self(self, e.children[0], true);
        self(self, e.children[1], false);
        return;
      }
      if (e.op == RtlOp::kMem && e.children.size() == 1) {
        const std::optional<int64_t> off =
            ExtractBaseRelativeConstOffset(e.children[0], stack_base_reg);
        if (off.has_value()) {
          const int sz = std::max(4, MipsModeByteSize(e.mode));
          if (*off < spill_off + slot_sz && spill_off < *off + sz) {
            touched = true;
            if (in_dst && is_store_out != nullptr) {
              *is_store_out = true;
            }
          }
          return;
        }
      }
      for (const RtlExpr& ch : e.children) {
        self(self, ch, false);
      }
    };
    walk(walk, pat, false);
    return touched;
  };
  for (size_t ob = 0; ob < all_parsed_blocks.size(); ++ob) {
    if (ob == b) {
      continue;
    }
    for (const RtlPatternAndNotes& opn : all_parsed_blocks[ob]) {
      if (insn_touches_slot(opn.pattern, nullptr)) {
        return false;
      }
    }
  }
  for (size_t k = load_k + 1; k < all_parsed_blocks[b].size(); ++k) {
    if (insn_touches_slot(all_parsed_blocks[b][k].pattern, nullptr)) {
      return false;
    }
  }
  size_t store_k = load_k;
  int calls_crossed = 0;
  int slot_refs = 0;
  for (size_t k = 0; k < load_k; ++k) {
    bool is_store = false;
    if (insn_touches_slot(all_parsed_blocks[b][k].pattern, &is_store)) {
      ++slot_refs;
      if (is_store && store_k == load_k) {
        store_k = k;
      }
    }
    if (store_k < load_k && ContainsCallOp(all_parsed_blocks[b][k].pattern)) {
      ++calls_crossed;
    }
  }
  if (store_k >= load_k || calls_crossed <= 0) {
    return false;
  }
  // `local-alloc.c:1525-1538, 2106, 2121, 2205-2216`: if combining the
  // call-crossing spilled pseudo `ps` with `pd` produces a quantity whose
  // `qty_n_calls_crossed > 0` has NO free callee-saved hard register over
  // `[store_k, death(pd)]` (`$s0..$s7` for GPRs, `$f20..$f31` for FPRs; `$fp`
  // is excluded by `ELIMINABLE_REGS`, `config/mips/mips.h:1186-1209`) and is
  // NOT `CALLER_SAVE_PROFITABLE` (`n_refs <= 4 * calls_crossed`), `local-alloc`
  // leaves the combined qty unallocated (`-1`), so `global-alloc` allocates
  // `ps` (spilled) and `pd` (`target_hr`) separately.
  const int pd_hr = ren_it->second;
  const bool pd_is_gpr = (pd_hr >= 2 && pd_hr < 32);
  const bool pd_is_fpr = (pd_hr >= 32 && pd_hr < 64);
  if ((pd_is_gpr || pd_is_fpr) && p_death.contains(pd)) {
    // Count refs of `pd` and conservatively bound refs of `ps` (its def/pre-call
    // uses on the store source reg plus spill slot loads/stores).
    int pd_refs = 0;
    for (size_t k = def_i; k <= p_death.at(pd) && k < b_insns.size(); ++k) {
      b_insns[k].pattern.Visit([&](const RtlExpr& n) {
        if (n.IsPseudoReg() && n.RegNo() == pd) {
          ++pd_refs;
        }
      });
    }
    int ps_extra_refs = 1;  // `load_k` read at `def_i`.
    bool ps_has_foreign_hard_pref = false;
    const RtlExpr& st_pat = all_parsed_blocks[b][store_k].pattern;
    if (st_pat.IsSet() && st_pat.children.size() == 2 && st_pat.SetSrc().IsReg() &&
        !st_pat.SetSrc().IsArgPointerReg()) {
      const int st_r = st_pat.SetSrc().RegNo();
      auto check_foreign_hard_pref = [&](const RtlExpr& expr) {
        auto walk = [&](const auto& self, const RtlExpr& e) -> void {
          if (ps_has_foreign_hard_pref || e.op == RtlOp::kMem) {
            return;
          }
          if (e.IsHardReg() && !e.IsArgPointerReg()) {
            const int hr = e.RegNo();
            const bool same_bank = pd_is_gpr ? (hr >= 2 && hr <= 25) : (hr >= 32 && hr < 64);
            if (same_bank && hr != pd_hr) {
              ps_has_foreign_hard_pref = true;
            }
            return;
          }
          for (const RtlExpr& ch : e.children) {
            self(self, ch);
          }
        };
        walk(walk, expr);
      };
      // `global.c:805-823, 1535-1611` (`expand_preferences` + `set_preference`):
      // if `ps`'s defining insn or pre-call consumer mentions another same-bank
      // hard register `hr != pd_hr`, `global-alloc` propagates
      // `hard_reg_preferences[ps]` to `pd` and assigns `hr` instead of `pd_hr`.
      for (size_t rev = store_k; rev > 0; --rev) {
        const size_t dk = rev - 1;
        const RtlExpr& dp = all_parsed_blocks[b][dk].pattern;
        if (ContainsCallOp(dp)) {
          break;
        }
        if (dp.IsSet() && dp.children.size() == 2 && dp.SetDest().IsReg() &&
            !dp.SetDest().IsArgPointerReg() && dp.SetDest().RegNo() == st_r) {
          check_foreign_hard_pref(dp.SetSrc());
          break;
        }
      }
      for (size_t k = store_k + 1; k < load_k; ++k) {
        if (ContainsCallOp(all_parsed_blocks[b][k].pattern)) {
          break;
        }
        bool def_st_r = false;
        const RtlExpr& kp = all_parsed_blocks[b][k].pattern;
        if (kp.IsSet() && kp.children.size() == 2) {
          bool reads_st_r = false;
          kp.SetSrc().Visit([&](const RtlExpr& n) {
            if (n.IsReg() && !n.IsArgPointerReg() && n.RegNo() == st_r) {
              ++ps_extra_refs;
              reads_st_r = true;
            }
          });
          if (reads_st_r) {
            check_foreign_hard_pref(kp.SetDest());
          }
          if (kp.SetDest().IsReg() && kp.SetDest().RegNo() == st_r) {
            def_st_r = true;
          }
        }
        if (def_st_r) {
          break;
        }
      }
    }
    const int combined_refs_bound = slot_refs + ps_extra_refs + pd_refs;
    const bool caller_save_unprofitable = (combined_refs_bound <= 4 * calls_crossed);
    if (!ps_has_foreign_hard_pref && caller_save_unprofitable) {
      const size_t t_start = 2 * store_k + 1;
      const size_t t_end = std::max(t_start, 2 * p_death.at(pd));
      const int span = std::max(1, MipsHardRegNregs(pd_hr, sr.mode, gp64, fp64));
      const int saved_lo = pd_is_gpr ? 16 : 52;
      const int saved_hi = pd_is_gpr ? 24 : 64;
      const int saved_step = pd_is_gpr ? (span > 1 ? 2 : 1) : ((!fp64 || span > 1) ? 2 : 1);
      bool any_saved_free = false;
      for (int sreg = saved_lo; sreg + span <= saved_hi; sreg += saved_step) {
        uint64_t sm = 0;
        for (int w = 0; w < span; ++w) {
          sm |= (uint64_t{1} << (sreg + w));
        }
        bool free_here = true;
        for (size_t t = t_start; t <= t_end && t < all_live_ticks.size(); ++t) {
          if ((all_live_ticks[t] & sm) != 0) {
            free_here = false;
            break;
          }
        }
        if (free_here) {
          any_saved_free = true;
          break;
        }
      }
      if (!any_saved_free) {
        return false;
      }
    }
  }
  return true;
}

// Returns true if demoting `g.members.back()` to `g.target_hr` would turn
// `b_insns[g.death_i]` into a hard-register self-RMW carrying an illegal
// self-dead `REG_DEAD` note (`flow.c:2507-2550`, `sched.c:2872`).
bool GroupDemotionCreatesSelfDeadHardRmw(const std::vector<RtlPatternAndNotes>& b_insns,
                                         const LocalQtyGroup& g, bool gp64, bool fp64) {
  if (g.members.empty() || g.death_i >= b_insns.size()) {
    return false;
  }
  const int p = g.members.back();
  const RtlPatternAndNotes& pn = b_insns[g.death_i];
  const bool has_dead =
      std::any_of(pn.reg_notes.begin(), pn.reg_notes.end(), [&](const RtlRegNote& n) {
        return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
               !n.datum.IsArgPointerReg() && n.datum.RegNo() == p;
      });
  if (!has_dead) {
    return false;
  }
  const RtlExpr* st = nullptr;
  if (pn.pattern.IsSet()) {
    st = &pn.pattern;
  } else if (pn.pattern.IsParallel()) {
    for (const RtlExpr& ch : pn.pattern.children) {
      if (ch.IsSet()) {
        if (st != nullptr) {
          st = nullptr;
          break;
        }
        st = &ch;
      }
    }
  }
  if (st == nullptr) {
    return false;
  }
  const RtlExpr* dst = &st->SetDest();
  while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
    dst = &dst->children[0];
  }
  if (!dst->IsHardReg() || dst->IsArgPointerReg() || dst->RegNo() < 0 || dst->RegNo() >= 64) {
    return false;
  }
  auto mask_of = [&](int hr, RtlMachineMode mode) -> uint64_t {
    uint64_t m = 0;
    const int span = std::max(1, MipsHardRegNregs(hr, mode, gp64, fp64));
    for (int w = 0; w < span && hr + w < 64; ++w) {
      m |= (uint64_t{1} << (hr + w));
    }
    return m;
  };
  return (mask_of(dst->RegNo(), dst->mode) & mask_of(g.target_hr, g.mode)) != 0;
}

void DetectLocalResolutionGroupConflicts(
    const RtlControlFlowGraph& cfg, const std::vector<RtlPatternAndNotes>& raw_blk,
    const std::vector<RtlPatternAndNotes>& b_insns,
    const std::vector<uint64_t>& b_reload_scratch_masks,
    const absl::flat_hash_map<int, LocalResolutionCandInfo>& info,
    const absl::flat_hash_map<int, size_t>& p_birth,
    const absl::flat_hash_map<int, size_t>& p_death, const std::vector<int>& b_multi_locals,
    const absl::flat_hash_map<int, size_t>& multi_first_i,
    const absl::flat_hash_map<int, size_t>& multi_last_i,
    const std::vector<uint64_t>& base_live_ticks, const std::vector<uint64_t>& all_live_ticks,
    const absl::flat_hash_set<int>& md_promoted_gprs, int num_scratch_clobbers_in_b, bool gp64,
    bool fp64, const std::vector<int>& group_roots,
    absl::flat_hash_map<int, LocalQtyGroup>* groups_ptr) {
  auto& groups = *groups_ptr;
  const size_t m = b_insns.size();
  auto is_demoted_reload_scratch_at = [&](size_t idx, int hr) {
    return hr >= 0 && hr < 64 && idx < b_reload_scratch_masks.size() &&
           (b_reload_scratch_masks[idx] & (uint64_t{1} << hr)) != 0;
  };
  const int word_bytes = gp64 ? 8 : 4;

  auto span_mask = [&](int hr, RtlMachineMode mode) -> uint64_t {
    uint64_t mask = 0;
    const int span = std::max(1, MipsHardRegNregs(hr, mode, gp64, fp64));
    for (int w = 0; w < span && hr + w < 64; ++w) {
      if (hr + w >= 0) {
        mask |= (uint64_t{1} << (hr + w));
      }
    }
    return mask;
  };
  auto is_valid_mode_reg = [&](int hr, RtlMachineMode mode, bool want_gpr, bool crosses_call) {
    if (want_gpr ? ((hr < 2 || hr > 25) && hr != kFramePointerRegno) : (hr < 32 || hr > 63)) {
      return false;
    }
    if (cfg.sideband.frame_pointer_needed && hr == kFramePointerRegno) {
      return false;
    }
    const int span = std::max(1, MipsHardRegNregs(hr, mode, gp64, fp64));
    // `config/mips/mips.c:3448` (`HARD_REGNO_MODE_OK`): on `-mfp32` (`!fp64`),
    // odd FP registers are never allocatable for any FP mode (`SFmode` or `DFmode`).
    if ((span > 1 || (!want_gpr && !fp64)) && (hr % 2) != 0) {
      return false;
    }
    if (want_gpr && hr == kFramePointerRegno && span > 1) {
      return false;
    }
    if (want_gpr && hr != kFramePointerRegno && hr + span > 26) {
      return false;
    }
    if (!want_gpr && hr + span > 64) {
      return false;
    }
    for (int w = 0; w < span; ++w) {
      if (crosses_call && IsCallUsedHardReg(hr + w)) {
        return false;
      }
    }
    return true;
  };
  auto modes_tieable = [](RtlMachineMode m1, RtlMachineMode m2) {
    const bool f1 = (m1 == RtlMachineMode::kSF || m1 == RtlMachineMode::kDF);
    const bool f2 = (m2 == RtlMachineMode::kSF || m2 == RtlMachineMode::kDF);
    return f1 == f2;
  };
  auto has_dead_note = [](const RtlPatternAndNotes& pn, int regno) {
    return std::any_of(pn.reg_notes.begin(), pn.reg_notes.end(), [&](const RtlRegNote& n) {
      return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
             !n.datum.IsArgPointerReg() && n.datum.RegNo() == regno;
    });
  };
  auto extract_single_set = [](const RtlExpr& pat) -> const RtlExpr* {
    if (pat.IsSet()) {
      return &pat;
    }
    if (pat.IsParallel()) {
      const RtlExpr* found = nullptr;
      for (const RtlExpr& ch : pat.children) {
        if (ch.IsSet()) {
          if (found != nullptr) {
            return nullptr;
          }
          found = &ch;
        }
      }
      return found;
    }
    return nullptr;
  };
  struct RhsRegUse {
    RtlExpr reg;
    int subreg_word = 0;
    bool subreg_gt_word = false;
  };
  auto collect_rhs_regs = [&](const RtlExpr& expr, std::vector<RhsRegUse>* out_regs) {
    auto walk = [&](const auto& self, const RtlExpr& e) -> void {
      if (e.op == RtlOp::kMem) {
        return;
      }
      if (e.op == RtlOp::kReg && !e.IsArgPointerReg()) {
        out_regs->push_back(RhsRegUse{.reg = e});
        return;
      }
      if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg &&
          !e.children[0].IsArgPointerReg()) {
        out_regs->push_back(RhsRegUse{
            .reg = e.children[0],
            .subreg_word = static_cast<int>(e.int_val),
            .subreg_gt_word = MipsModeByteSize(e.children[0].mode) > word_bytes,
        });
        return;
      }
      for (const RtlExpr& ch : e.children) {
        self(self, ch);
      }
    };
    walk(walk, expr);
  };
  auto base_free_on_interval = [&](int hr, RtlMachineMode mode, size_t bi, size_t di) {
    const uint64_t hm = span_mask(hr, mode);
    const size_t t_start = 2 * bi + 1;
    const size_t t_end = std::max(t_start, 2 * di);
    for (size_t t = t_start; t <= t_end && t < base_live_ticks.size(); ++t) {
      if ((base_live_ticks[t] & hm) != 0) {
        return false;
      }
    }
    return true;
  };
  auto all_free_on_interval = [&](int hr, RtlMachineMode mode, size_t bi, size_t di) {
    const uint64_t hm = span_mask(hr, mode);
    const size_t t_start = 2 * bi + 1;
    const size_t t_end = std::max(t_start, 2 * di);
    for (size_t t = t_start; t <= t_end && t < all_live_ticks.size(); ++t) {
      if ((all_live_ticks[t] & hm) != 0) {
        return false;
      }
    }
    return true;
  };

  for (int root : group_roots) {
    LocalQtyGroup& g = groups.at(root);
    const absl::flat_hash_set<int> member_set(g.members.begin(), g.members.end());
    std::array<bool, kFirstPseudoRegister> seen_copy{};
    std::array<bool, kFirstPseudoRegister> seen_arith{};
    auto record_raw_sugg = [&](int shr, bool may_save_copy) {
      if (shr < 0 || shr >= kFirstPseudoRegister) {
        return;
      }
      if (may_save_copy && !seen_copy[shr]) {
        seen_copy[shr] = true;
        ++g.raw_num_copy_sugg;
        if (shr < 64) {
          g.raw_copy_sugg_mask |= (uint64_t{1} << shr);
        }
      } else if (!seen_arith[shr]) {
        seen_arith[shr] = true;
        ++g.raw_num_sugg;
        if (shr < 64) {
          g.raw_sugg_mask |= (uint64_t{1} << shr);
        }
      }
    };
    for (size_t i = g.birth_i; i <= g.death_i && i < m; ++i) {
      if (ContainsCallOp(b_insns[i].pattern)) {
        continue;
      }
      const RtlExpr* st = extract_single_set(b_insns[i].pattern);
      if (st == nullptr) {
        continue;
      }
      bool dst_subreg_gt_word = false;
      int dst_subreg_word = 0;
      const RtlExpr* dst = &st->SetDest();
      while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        if (MipsModeByteSize(dst->children[0].mode) > word_bytes) {
          dst_subreg_gt_word = true;
        }
        dst_subreg_word += static_cast<int>(dst->int_val);
        dst = &dst->children[0];
      }
      if (dst->op != RtlOp::kReg || dst->IsArgPointerReg()) {
        continue;
      }
      const bool plain_reg_copy =
          !dst_subreg_gt_word &&
          (st->SetSrc().op == RtlOp::kReg ||
           (st->SetSrc().op == RtlOp::kSubreg && !st->SetSrc().children.empty() &&
            st->SetSrc().children[0].op == RtlOp::kReg));
      const int ssize = std::max(1, (MipsModeByteSize(dst->mode) + word_bytes - 1) / word_bytes);
      std::vector<RhsRegUse> rhs_uses;
      collect_rhs_regs(st->SetSrc(), &rhs_uses);
      if (dst->IsPseudoReg() && member_set.contains(dst->RegNo())) {
        for (const RhsRegUse& su : rhs_uses) {
          const RtlExpr& sr = su.reg;
          const int usize = std::max(1, (MipsModeByteSize(sr.mode) + word_bytes - 1) / word_bytes);
          const int offset = su.subreg_word - dst_subreg_word;
          if ((offset > 0 && usize + offset > ssize) || (offset < 0 && usize + offset < ssize)) {
            continue;
          }
          if (sr.IsPseudoReg() && member_set.contains(sr.RegNo()) && sr.RegNo() != dst->RegNo() &&
              p_birth.at(dst->RegNo()) == i && p_death.at(sr.RegNo()) == i &&
              has_dead_note(b_insns[i], sr.RegNo()) && modes_tieable(sr.mode, dst->mode)) {
            break;
          }
          if (sr.IsHardReg() && sr.RegNo() >= 0 && sr.RegNo() < kFirstPseudoRegister &&
              !md_promoted_gprs.contains(sr.RegNo()) &&
              !is_demoted_reload_scratch_at(i, sr.RegNo()) && modes_tieable(sr.mode, dst->mode)) {
            record_raw_sugg(sr.RegNo(), plain_reg_copy && !su.subreg_gt_word);
          }
        }
      } else if (dst->IsHardReg() && dst->RegNo() >= 0 && dst->RegNo() < kFirstPseudoRegister &&
                 !is_demoted_reload_scratch_at(i, dst->RegNo()) &&
                 !IsDemotedOutputReloadHardScratch(raw_blk, b_insns, base_live_ticks, cfg, gp64,
                                                   fp64, i, dst->RegNo(), dst->mode)) {
        for (const RhsRegUse& su : rhs_uses) {
          const RtlExpr& sr = su.reg;
          const int usize = std::max(1, (MipsModeByteSize(sr.mode) + word_bytes - 1) / word_bytes);
          const int offset = su.subreg_word - dst_subreg_word;
          if ((offset > 0 && usize + offset > ssize) || (offset < 0 && usize + offset < ssize)) {
            continue;
          }
          if (sr.IsPseudoReg() && member_set.contains(sr.RegNo()) &&
              modes_tieable(sr.mode, dst->mode)) {
            record_raw_sugg(dst->RegNo(), plain_reg_copy && !su.subreg_gt_word);
          }
        }
      }
    }
    if (g.raw_num_copy_sugg > 0) {
      g.sugg_rank = g.raw_num_copy_sugg;
    } else if (g.raw_num_sugg > 0) {
      g.sugg_rank = g.raw_num_sugg * kFirstPseudoRegister;
    } else {
      g.sugg_rank = 1000000;
    }
  }

  std::vector<uint64_t> sugg_occupied_ticks(all_live_ticks.size(), 0);
  auto has_target_hr_sugg = [&](const absl::flat_hash_set<int>& mset, int thr, RtlMachineMode gmode,
                                size_t bi, size_t di) {
    for (size_t i = bi; i <= di && i < m; ++i) {
      const RtlExpr* st = extract_single_set(b_insns[i].pattern);
      if (st == nullptr) {
        continue;
      }
      const RtlExpr* dst = &st->SetDest();
      if (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        dst = &dst->children[0];
      }
      std::vector<RhsRegUse> rhs_uses;
      collect_rhs_regs(st->SetSrc(), &rhs_uses);
      if (dst->IsPseudoReg() && mset.contains(dst->RegNo())) {
        for (const RhsRegUse& su : rhs_uses) {
          if (su.reg.IsPseudoReg() && mset.contains(su.reg.RegNo()) &&
              su.reg.RegNo() != dst->RegNo() && p_birth.at(dst->RegNo()) == i &&
              p_death.at(su.reg.RegNo()) == i && has_dead_note(b_insns[i], su.reg.RegNo()) &&
              modes_tieable(su.reg.mode, dst->mode)) {
            break;
          }
          if (su.reg.IsHardReg() && su.reg.RegNo() == thr && !md_promoted_gprs.contains(thr) &&
              !is_demoted_reload_scratch_at(i, thr) && modes_tieable(su.reg.mode, gmode)) {
            return true;
          }
        }
      } else if (dst->IsHardReg() && dst->RegNo() == thr && modes_tieable(dst->mode, gmode) &&
                 !is_demoted_reload_scratch_at(i, thr) &&
                 !IsDemotedOutputReloadHardScratch(raw_blk, b_insns, base_live_ticks, cfg, gp64,
                                                   fp64, i, thr, dst->mode)) {
        for (const RhsRegUse& su : rhs_uses) {
          if (su.reg.IsPseudoReg() && mset.contains(su.reg.RegNo())) {
            return true;
          }
        }
      }
    }
    return false;
  };
  auto mark_sugg_occupied = [&](const absl::flat_hash_set<int>& mset, int thr, RtlMachineMode gmode,
                                size_t bi, size_t di) {
    if (!base_free_on_interval(thr, gmode, bi, di) ||
        !has_target_hr_sugg(mset, thr, gmode, bi, di)) {
      return;
    }
    const uint64_t hm = span_mask(thr, gmode);
    const size_t t_start = 2 * bi + 1;
    const size_t t_end = std::max(t_start, 2 * di);
    for (size_t t = t_start; t <= t_end && t < sugg_occupied_ticks.size(); ++t) {
      sugg_occupied_ticks[t] |= hm;
    }
  };
  for (int root : group_roots) {
    const LocalQtyGroup& g = groups.at(root);
    mark_sugg_occupied({g.members.begin(), g.members.end()}, g.target_hr, g.mode, g.birth_i,
                       g.death_i);
  }
  for (int pm : b_multi_locals) {
    if (multi_first_i.contains(pm)) {
      mark_sugg_occupied({pm}, cfg.sideband.reg_renumber.at(pm), info.at(pm).mode,
                         multi_first_i.at(pm), multi_last_i.at(pm));
    }
  }
  auto sugg_free_on_interval = [&](int hr, RtlMachineMode mode, size_t bi, size_t di) {
    const uint64_t hm = span_mask(hr, mode);
    const size_t t_start = 2 * bi + 1;
    const size_t t_end = std::max(t_start, 2 * di);
    for (size_t t = t_start; t <= t_end && t < sugg_occupied_ticks.size(); ++t) {
      if (((base_live_ticks[t] | sugg_occupied_ticks[t]) & hm) != 0) {
        return false;
      }
    }
    return true;
  };

  for (int root : group_roots) {
    LocalQtyGroup& g = groups.at(root);
    const bool want_gpr = (g.target_hr < 32);
    if (!base_free_on_interval(g.target_hr, g.mode, g.birth_i, g.death_i)) {
      g.needs_resolution = true;
    }

    auto find_first_free_in_mask = [&](uint64_t mask) -> int {
      for (int shr = (want_gpr ? 2 : 32); shr <= (want_gpr ? 25 : 63); ++shr) {
        if ((mask & (uint64_t{1} << shr)) == 0) {
          continue;
        }
        if (!is_valid_mode_reg(shr, g.mode, want_gpr, g.crosses_call) ||
            !base_free_on_interval(shr, g.mode, g.birth_i, g.death_i)) {
          continue;
        }
        bool blocked_by_earlier_tier = false;
        const size_t b1 = 2 * g.birth_i + 1;
        const size_t d1 = std::max(2 * g.birth_i + 2, 2 * g.death_i);
        for (int prev_root : group_roots) {
          if (prev_root == root) {
            continue;
          }
          const LocalQtyGroup& pg = groups.at(prev_root);
          if (pg.sugg_rank > g.sugg_rank ||
              ((pg.raw_copy_sugg_mask | pg.raw_sugg_mask) & (uint64_t{1} << pg.target_hr)) == 0 ||
              !base_free_on_interval(pg.target_hr, pg.mode, pg.birth_i, pg.death_i)) {
            continue;
          }
          const size_t b2 = 2 * pg.birth_i + 1;
          const size_t d2 = std::max(2 * pg.birth_i + 2, 2 * pg.death_i);
          if (std::max(b1, b2) < std::min(d1, d2) &&
              (span_mask(shr, g.mode) & span_mask(pg.target_hr, pg.mode)) != 0) {
            blocked_by_earlier_tier = true;
            break;
          }
        }
        if (!blocked_by_earlier_tier) {
          return shr;
        }
      }
      return -1;
    };
    if (g.raw_num_copy_sugg > 0) {
      g.first_free_sugg_hr = find_first_free_in_mask(g.raw_copy_sugg_mask);
    }
    if (g.first_free_sugg_hr < 0 && g.raw_num_sugg > 0) {
      g.first_free_sugg_hr = find_first_free_in_mask(g.raw_sugg_mask);
    }

    bool target_sugg = false;
    bool target_copy_sugg = false;
    int best_other_sugg = -1;
    bool other_copy_sugg = false;
    auto consider_hr_sugg = [&](int shr, bool is_copy) {
      if (shr == kFramePointerRegno || !is_valid_mode_reg(shr, g.mode, want_gpr, g.crosses_call) ||
          !base_free_on_interval(shr, g.mode, g.birth_i, g.death_i)) {
        return;
      }
      if (shr != g.target_hr && !sugg_free_on_interval(shr, g.mode, g.birth_i, g.death_i)) {
        return;
      }
      g.has_any_sugg = true;
      if (shr == g.target_hr) {
        target_sugg = true;
        if (is_copy) {
          target_copy_sugg = true;
        }
        return;
      }
      if (best_other_sugg < 0 || (is_copy && !other_copy_sugg) ||
          (is_copy == other_copy_sugg &&
           LookupMipsAllocOrderIndex(shr) < LookupMipsAllocOrderIndex(best_other_sugg))) {
        best_other_sugg = shr;
        other_copy_sugg = is_copy;
      }
    };
    for (int shr = 2; shr < 64; ++shr) {
      if ((g.raw_copy_sugg_mask & (uint64_t{1} << shr)) != 0) {
        consider_hr_sugg(shr, /*is_copy=*/true);
      } else if ((g.raw_sugg_mask & (uint64_t{1} << shr)) != 0) {
        consider_hr_sugg(shr, /*is_copy=*/false);
      }
    }

    if (best_other_sugg >= 0) {
      const bool other_wins =
          !target_sugg || (other_copy_sugg && !target_copy_sugg) ||
          (other_copy_sugg == target_copy_sugg &&
           LookupMipsAllocOrderIndex(best_other_sugg) < LookupMipsAllocOrderIndex(g.target_hr));
      if (other_wins) {
        g.needs_resolution = true;
      } else {
        g.wins_via_sugg = true;
      }
    } else if (target_sugg) {
      g.wins_via_sugg = true;
    }

    if (!g.needs_resolution && !target_sugg) {
      const int target_ord = LookupMipsAllocOrderIndex(g.target_hr);
      for (int h = (want_gpr ? 2 : 32); h <= (want_gpr ? 30 : 63); ++h) {
        if (want_gpr && h > 25 && h != kFramePointerRegno) {
          continue;
        }
        if (LookupMipsAllocOrderIndex(h) >= target_ord) {
          continue;
        }
        if (is_valid_mode_reg(h, g.mode, want_gpr, g.crosses_call) &&
            all_free_on_interval(h, g.mode, g.birth_i, g.death_i)) {
          g.needs_resolution = true;
          break;
        }
      }
    }
  }

  // If `g_thief` has strictly fewer raw suggestions (`sugg_rank` smaller) than
  // an overlapping `g_victim`, `local-alloc.c:qty_sugg_compare` always allocates
  // `g_thief` before `g_victim` regardless of `reg_n_refs`. When `g_thief`'s
  // first free suggested register steals `g_victim.target_hr`, demote `g_victim`
  // so `g_victim.target_hr` is hard-live in `regs_live_at` across the overlap.
  for (int thief_root : group_roots) {
    LocalQtyGroup& g_thief = groups.at(thief_root);
    if (g_thief.sugg_rank >= 1000000 || g_thief.first_free_sugg_hr < 0 ||
        g_thief.first_free_sugg_hr == g_thief.target_hr) {
      continue;
    }
    const uint64_t thief_sugg_mask = span_mask(g_thief.first_free_sugg_hr, g_thief.mode);
    const size_t b1 = 2 * g_thief.birth_i + 1;
    const size_t d1 = std::max(2 * g_thief.birth_i + 2, 2 * g_thief.death_i);
    const bool thief_has_equiv =
        std::any_of(g_thief.members.begin(), g_thief.members.end(),
                    [&](int mp) { return info.contains(mp) && info.at(mp).has_reg_equiv; });
    for (int victim_root : group_roots) {
      if (victim_root == thief_root) {
        continue;
      }
      LocalQtyGroup& g_victim = groups.at(victim_root);
      if (g_thief.sugg_rank >= g_victim.sugg_rank) {
        continue;
      }
      const size_t b2 = 2 * g_victim.birth_i + 1;
      const size_t d2 = std::max(2 * g_victim.birth_i + 2, 2 * g_victim.death_i);
      if (std::max(b1, b2) >= std::min(d1, d2) ||
          (thief_sugg_mask & span_mask(g_victim.target_hr, g_victim.mode)) == 0) {
        continue;
      }
      const bool victim_has_equiv =
          std::any_of(g_victim.members.begin(), g_victim.members.end(),
                      [&](int mp) { return info.contains(mp) && info.at(mp).has_reg_equiv; });
      if (!g_thief.needs_resolution) {
        if (!thief_has_equiv && g_thief.target_hr != kFramePointerRegno &&
            GroupDemotionCreatesSelfDeadHardRmw(b_insns, g_victim, gp64, fp64) &&
            !GroupDemotionCreatesSelfDeadHardRmw(b_insns, g_thief, gp64, fp64)) {
          g_thief.needs_resolution = true;
          g_thief.stolen_by_sugg_thief = true;
        } else {
          g_victim.needs_resolution = true;
          g_victim.stolen_by_sugg_thief = true;
          g_victim.max_thief_birth_i = std::max(g_victim.max_thief_birth_i, g_thief.birth_i);
        }
      } else if (thief_has_equiv && !victim_has_equiv &&
                 base_free_on_interval(g_thief.target_hr, g_thief.mode, g_thief.birth_i,
                                       g_thief.death_i)) {
        g_thief.needs_resolution = false;
        g_victim.needs_resolution = true;
        g_victim.stolen_by_sugg_thief = true;
        g_victim.max_thief_birth_i = std::max(g_victim.max_thief_birth_i, g_thief.birth_i);
      }
    }
  }

  if (group_roots.size() == 3 && num_scratch_clobbers_in_b == 0) {
    const LocalQtyGroup& g0 = groups.at(group_roots[0]);
    LocalQtyGroup& g1 = groups.at(group_roots[1]);
    const LocalQtyGroup& g2 = groups.at(group_roots[2]);
    const bool same_bank = ((g0.target_hr < 32) == (g1.target_hr < 32)) &&
                           ((g2.target_hr < 32) == (g1.target_hr < 32));
    if (same_bank && !g1.has_any_sugg && (!g0.wins_via_sugg || !g2.wins_via_sugg) &&
        g0.death_i >= g1.birth_i && g1.death_i >= g0.birth_i && g2.death_i >= g1.birth_i &&
        g1.death_i >= g2.birth_i &&
        LookupMipsAllocOrderIndex(g1.target_hr) < LookupMipsAllocOrderIndex(g0.target_hr) &&
        LookupMipsAllocOrderIndex(g1.target_hr) < LookupMipsAllocOrderIndex(g2.target_hr)) {
      g1.needs_resolution = true;
    }
    // GCC 2.7.2 `local-alloc.c:1442-1454` (`block_alloc` `case 3:`):
    // `qty_sugg_compare(0, 1)` is called twice on literal indices `0` and `1`
    // rather than `qty_order[0]` and `qty_order[1]`. When `g0` and `g2` both
    // carry hard-reg suggestions while `g1` has none, `qty_sugg_compare(0, 1)
    // > 0` swaps `[0, 1, 2]` -> `[1, 0, 2]`, `qty_sugg_compare(1, 2) < 0` leaves
    // `[1, 0, 2]`, and the second `qty_sugg_compare(0, 1) > 0` swaps back to
    // `[0, 1, 2]`. Consequently `g0` always runs before `g2` in the suggestion
    // pass regardless of `reg_n_refs`, stealing `g2.target_hr` whenever `g0`'s
    // first free suggested register collides with `g2`. Demoting `g1` reduces
    // `next_qty` from `3` to `2` (`case 2:`), restoring normal `qty_sugg_compare`.
    const bool g0_raw_sugg = (g0.raw_num_sugg > 0 || g0.raw_num_copy_sugg > 0);
    const bool g1_raw_sugg = (g1.raw_num_sugg > 0 || g1.raw_num_copy_sugg > 0);
    const bool g2_raw_sugg = (g2.raw_num_sugg > 0 || g2.raw_num_copy_sugg > 0);
    const uint64_t g0_sugg_mask = g0.raw_copy_sugg_mask | g0.raw_sugg_mask;
    if (same_bank && g0_raw_sugg && !g1_raw_sugg && g2_raw_sugg && g2.target_hr >= 0 &&
        g2.target_hr < 64 && g2.target_hr != g0.target_hr &&
        (g0_sugg_mask & (uint64_t{1} << g2.target_hr)) != 0 &&
        base_free_on_interval(g2.target_hr, g0.mode, g0.birth_i, g0.death_i)) {
      const size_t b0 = 2 * g0.birth_i + 1;
      const size_t d0 = std::max(2 * g0.birth_i + 2, 2 * g0.death_i);
      const size_t b2 = 2 * g2.birth_i + 1;
      const size_t d2 = std::max(2 * g2.birth_i + 2, 2 * g2.death_i);
      if (std::max(b0, b2) < std::min(d0, d2)) {
        g1.needs_resolution = true;
      }
    }
  }
}

// When `sr_no` at `(b, use_i)` is a demoted input-reload hard scratch, looks
// backward in `b_insns` for the reload copy `(set (reg sr_no) (reg ps))` with
// `REG_DEAD ps`. After `InvertReload` folds the copy back into `use_i`,
// `local-alloc.c:1857` (`combine_regs`) will see `ps` dying directly in `use_i`
// and attempt to combine `ps` with `use_i`'s destination pseudo.
int FindDyingPseudoBehindDemotedInputReload(const std::vector<RtlPatternAndNotes>& b_insns,
                                            size_t use_i, int sr_no, RtlMachineMode sr_mode) {
  for (size_t rev = use_i; rev > 0; --rev) {
    const size_t k = rev - 1;
    const RtlExpr& kpat = b_insns[k].pattern;
    if (ContainsCallOp(kpat) || kpat.op == RtlOp::kSequence ||
        (kpat.IsSet() && kpat.SetDest().op == RtlOp::kPc)) {
      return -1;
    }
    if (kpat.IsSet() && kpat.SetDest().IsHardReg() && !kpat.SetDest().IsArgPointerReg() &&
        kpat.SetDest().RegNo() == sr_no && kpat.SetDest().mode == sr_mode) {
      if (!kpat.SetSrc().IsPseudoReg() || kpat.SetSrc().mode != sr_mode) {
        return -1;
      }
      const int ps = kpat.SetSrc().RegNo();
      const bool dies_at_reload = std::any_of(
          b_insns[k].reg_notes.begin(), b_insns[k].reg_notes.end(), [&](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                   !n.datum.IsArgPointerReg() && n.datum.RegNo() == ps;
          });
      if (!dies_at_reload) {
        return -1;
      }
      for (size_t j = k + 1; j < use_i; ++j) {
        if (b_insns[j].pattern.ContainsReg(ps)) {
          return -1;
        }
      }
      return ps;
    }
    if (kpat.ContainsReg(sr_no)) {
      return -1;
    }
  }
  return -1;
}

// True iff demoting pseudo `cand_p` to `target_hr` (span `target_mask`) would
// cause `InvertReload` to create an illegal pre-alloc self-setting `REG_DEAD`
// (`flow.c:2505-2520`, `sched.c:2872-2906`) when folding a `!gp64` `DImode`
// `"=&d"` earlyclobber output reload copy `(set (reg:DI cand_p) (reg:DI scr_hr))`
// (`reload1.c:5641` `emit_reload_insns`, `mips.md:3722-3896`) back into its
// preceding `(parallel [(set (reg:DI scr_hr) (op ...)) (clobber ...)])` that
// already carries `REG_DEAD hr'` overlapping `target_mask`.
bool WouldEarlyclobberFoldCreateSelfSetRegDead(int cand_p, uint64_t target_mask,
                                               const std::vector<RtlPatternAndNotes>& b_insns,
                                               size_t birth_i, size_t death_i, bool gp64,
                                               bool fp64) {
  if (gp64) {
    return false;
  }
  auto span_mask = [&](int r, RtlMachineMode md) -> uint64_t {
    if (r < 0 || r >= 64) {
      return 0;
    }
    uint64_t msk = 0;
    const int nw = std::max(1, MipsHardRegNregs(r, md, gp64, fp64));
    for (int w = 0; w < nw && r + w < 64; ++w) {
      msk |= (uint64_t{1} << (r + w));
    }
    return msk;
  };
  const size_t limit = std::min(death_i + 1, b_insns.size());
  for (size_t i = birth_i; i < limit; ++i) {
    const RtlExpr& pat = b_insns[i].pattern;
    if (!pat.IsSet() || !pat.SetDest().IsPseudoReg() || pat.SetDest().RegNo() != cand_p ||
        pat.SetDest().mode != RtlMachineMode::kDI || !pat.SetSrc().IsHardReg() ||
        pat.SetSrc().IsArgPointerReg() || pat.SetSrc().mode != RtlMachineMode::kDI) {
      continue;
    }
    const int scr_hr = pat.SetSrc().RegNo();
    if (scr_hr < 2 || scr_hr > 24) {
      continue;
    }
    const uint64_t scr_mask = span_mask(scr_hr, RtlMachineMode::kDI);
    for (size_t rev = i; rev > 0; --rev) {
      const size_t k = rev - 1;
      const RtlExpr& pk = b_insns[k].pattern;
      if (ContainsCallOp(pk) || pk.op == RtlOp::kSequence ||
          (pk.IsSet() && pk.SetDest().op == RtlOp::kPc)) {
        break;
      }
      if (pk.op == RtlOp::kParallel && pk.children.size() >= 2 && pk.children[0].IsSet() &&
          pk.children[1].op == RtlOp::kClobber && pk.children[0].SetDest().IsHardReg() &&
          !pk.children[0].SetDest().IsArgPointerReg() &&
          !pk.children[0].SetDest().flags.integrated &&
          pk.children[0].SetDest().RegNo() == scr_hr &&
          pk.children[0].SetDest().mode == RtlMachineMode::kDI) {
        const RtlOp op0 = pk.children[0].SetSrc().op;
        if (op0 == RtlOp::kPlus || op0 == RtlOp::kMinus || op0 == RtlOp::kAshift ||
            op0 == RtlOp::kAshiftrt || op0 == RtlOp::kLshiftrt) {
          for (const RtlRegNote& n : b_insns[k].reg_notes) {
            if (n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg() &&
                !n.datum.IsArgPointerReg() &&
                (span_mask(n.datum.RegNo(), n.datum.mode) & target_mask) != 0) {
              return true;
            }
          }
        }
        break;
      }
      bool touches_scr = false;
      pk.Visit([&](const RtlExpr& node) {
        if (!touches_scr && node.IsHardReg() && !node.IsArgPointerReg() &&
            (span_mask(node.RegNo(), node.mode) & scr_mask) != 0) {
          touches_scr = true;
        }
      });
      if (touches_scr) {
        break;
      }
    }
  }
  return false;
}

// True iff demoting pseudo `cand_p` back to `target_hr` (span `target_mask`)
// would create an illegal pre-alloc instruction setting a hard register
// overlapping `target_mask` while also carrying a `REG_DEAD` note overlapping
// `target_mask`, violating `flow.c:2505-2520` / `sched.c:2872-2906`
// (`!dead_or_set_p`):
//   (1) a def of `cand_p` already carries `REG_DEAD hr'` overlapping `target_mask`, OR
//   (2) a death insn carrying `REG_DEAD cand_p` already sets hard reg `hr'`
//       overlapping `target_mask` (e.g., `(set (reg/i:SI 2 v0) (+ P 85)) (REG_DEAD P)`), OR
//   (3) a def of `cand_p` is an `"=&d"` earlyclobber output reload copy whose
//       preceding `PARALLEL` carries `REG_DEAD hr'` overlapping `target_mask`.
// Returns true when any single-SET defining `cand_p` in `[birth_i, death_i]`
// carries a hard-register `REG_DEAD` note overlapping `target_mask` (Case 1 of
// `flow.c:2505-2520` / `sched.c:2872-2906`).
bool HasOverlappingHardRegDeadOnDef(int cand_p, uint64_t target_mask,
                                    const std::vector<RtlPatternAndNotes>& b_insns, size_t birth_i,
                                    size_t death_i, bool gp64, bool fp64) {
  auto span_mask = [&](int r, RtlMachineMode md) -> uint64_t {
    if (r < 0 || r >= 64) {
      return 0;
    }
    uint64_t msk = 0;
    const int nw = std::max(1, MipsHardRegNregs(r, md, gp64, fp64));
    for (int w = 0; w < nw && r + w < 64; ++w) {
      msk |= (uint64_t{1} << (r + w));
    }
    return msk;
  };
  auto extract_single_set = [](const RtlExpr& pat) -> const RtlExpr* {
    if (pat.IsSet()) {
      return &pat;
    }
    if (pat.op == RtlOp::kParallel) {
      const RtlExpr* found = nullptr;
      for (const RtlExpr& ch : pat.children) {
        if (ch.IsSet()) {
          if (found != nullptr) {
            return nullptr;
          }
          found = &ch;
        }
      }
      return found;
    }
    return nullptr;
  };
  const size_t limit = std::min(death_i + 1, b_insns.size());
  for (size_t i = birth_i; i < limit; ++i) {
    const RtlExpr* st = extract_single_set(b_insns[i].pattern);
    if (st == nullptr) {
      continue;
    }
    const RtlExpr* dst = &st->SetDest();
    if (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
      dst = &dst->children[0];
    }
    if (dst->IsPseudoReg() && dst->RegNo() == cand_p) {
      for (const RtlRegNote& n : b_insns[i].reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg() &&
            !n.datum.IsArgPointerReg() &&
            (span_mask(n.datum.RegNo(), n.datum.mode) & target_mask) != 0) {
          return true;
        }
      }
    }
  }
  return false;
}

bool WouldDemoteCreateSelfSetRegDead(int cand_p, uint64_t target_mask,
                                     const std::vector<RtlPatternAndNotes>& b_insns, size_t birth_i,
                                     size_t death_i, bool gp64, bool fp64) {
  if (HasOverlappingHardRegDeadOnDef(cand_p, target_mask, b_insns, birth_i, death_i, gp64, fp64)) {
    return true;
  }
  auto span_mask = [&](int r, RtlMachineMode md) -> uint64_t {
    if (r < 0 || r >= 64) {
      return 0;
    }
    uint64_t msk = 0;
    const int nw = std::max(1, MipsHardRegNregs(r, md, gp64, fp64));
    for (int w = 0; w < nw && r + w < 64; ++w) {
      msk |= (uint64_t{1} << (r + w));
    }
    return msk;
  };
  auto extract_single_set = [](const RtlExpr& pat) -> const RtlExpr* {
    if (pat.IsSet()) {
      return &pat;
    }
    if (pat.op == RtlOp::kParallel) {
      const RtlExpr* found = nullptr;
      for (const RtlExpr& ch : pat.children) {
        if (ch.IsSet()) {
          if (found != nullptr) {
            return nullptr;
          }
          found = &ch;
        }
      }
      return found;
    }
    return nullptr;
  };
  const size_t limit = std::min(death_i + 1, b_insns.size());
  for (size_t i = birth_i; i < limit; ++i) {
    const RtlExpr* st = extract_single_set(b_insns[i].pattern);
    if (st == nullptr) {
      continue;
    }
    const RtlExpr* dst = &st->SetDest();
    if (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
      dst = &dst->children[0];
    }
    if (dst->IsPseudoReg() && dst->RegNo() == cand_p) {
      for (const RtlRegNote& n : b_insns[i].reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg() &&
            !n.datum.IsArgPointerReg() &&
            (span_mask(n.datum.RegNo(), n.datum.mode) & target_mask) != 0) {
          return true;
        }
      }
    } else if (dst->IsHardReg() && !dst->IsArgPointerReg() &&
               (span_mask(dst->RegNo(), dst->mode) & target_mask) != 0) {
      for (const RtlRegNote& n : b_insns[i].reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.IsPseudoReg() &&
            n.datum.RegNo() == cand_p) {
          return true;
        }
      }
    }
  }
  return WouldEarlyclobberFoldCreateSelfSetRegDead(cand_p, target_mask, b_insns, birth_i, death_i,
                                                   gp64, fp64);
}

bool HasMultiClobberParallelDef(const std::vector<RtlPatternAndNotes>& b_insns, int regno) {
  auto is_multi_clobber_parallel = [](const RtlExpr& pat, const auto& matches_dest) {
    if (pat.op != RtlOp::kParallel) {
      return false;
    }
    int clobbers = 0;
    bool distinct_clobbers = false;
    const RtlExpr* first_clobber = nullptr;
    bool sets_target = false;
    for (const RtlExpr& ch : pat.children) {
      if (ch.op == RtlOp::kClobber) {
        ++clobbers;
        if (first_clobber == nullptr) {
          first_clobber = &ch;
        } else if (!(*first_clobber == ch)) {
          distinct_clobbers = true;
        }
      } else if (ch.IsSet() && matches_dest(ch.SetDest())) {
        sets_target = true;
      }
    }
    return sets_target && clobbers >= 2 && distinct_clobbers;
  };
  for (size_t i = 0; i < b_insns.size(); ++i) {
    const RtlExpr& pat = b_insns[i].pattern;
    if (is_multi_clobber_parallel(
            pat, [&](const RtlExpr& d) { return d.IsPseudoReg() && d.RegNo() == regno; })) {
      return true;
    }
    // `reload1.c:6583` (`FoldMdOutputReloads`): an `MD_REGS` (`64 hi`/`65 lo`)
    // output reload `(set (reg:SI regno) (reg:SI 64/65))` following a
    // multi-clobber `PARALLEL` (`mulsi3_internal`, `divmodsi4`) folds `regno`
    // directly into that `PARALLEL`'s `SET_DEST`.
    if (i > 0 && pat.IsSet() && pat.SetDest().IsPseudoReg() && pat.SetDest().RegNo() == regno &&
        pat.SetSrc().IsHardReg() && (pat.SetSrc().RegNo() == 64 || pat.SetSrc().RegNo() == 65)) {
      const int md_hr = pat.SetSrc().RegNo();
      if (is_multi_clobber_parallel(b_insns[i - 1].pattern, [&](const RtlExpr& d) {
            return d.IsHardReg() && d.RegNo() == md_hr;
          })) {
        return true;
      }
    }
  }
  return false;
}

// Picks which member of a `local-alloc.c:combine_regs` quantity group to demote
// to `target_hr`. Mirrors `local-alloc.c:1020, 1855`, `flow.c:mark_set_1`,
// `flow.c:2505-2520`, and `sched.c:2770, 2872-2906`: prefers a member directly
// involved in a `false_pseudo_combine` (so one demotion breaks the false tie
// without stranding an upstream member), and avoids any member whose demotion
// would emit `REG_DEAD hr` on an insn setting `hr`.
int SelectLocalResolutionDemoteMember(const std::vector<int>& members, uint64_t target_mask,
                                      const std::vector<RtlPatternAndNotes>& b_insns,
                                      size_t birth_i, size_t death_i, bool gp64, bool fp64,
                                      const absl::flat_hash_map<int, LocalResolutionCandInfo>& info,
                                      const absl::flat_hash_set<int>& false_pseudo_combine,
                                      bool stolen_by_sugg_thief = false,
                                      size_t max_thief_birth_i = 0) {
  auto span_mask = [&](int r, RtlMachineMode md) -> uint64_t {
    if (r < 0 || r >= 64) {
      return 0;
    }
    uint64_t msk = 0;
    const int nw = std::max(1, MipsHardRegNregs(r, md, gp64, fp64));
    for (int w = 0; w < nw && r + w < 64; ++w) {
      msk |= (uint64_t{1} << (r + w));
    }
    return msk;
  };
  // `local-alloc.c:1266-1300`: if `p` dies at insn `i` defining another member
  // `q` of the same quantity group that also carries a conflicting dying hard
  // register (`qty_phys_sugg != target_hr`), demoting `p` first injects
  // `(REG_DEAD target_hr)` onto `q`'s def without removing `q`'s conflicting
  // hard-reg suggestion (unless `p` itself overlaps an earlier-born
  // `stolen_by_sugg_thief` blocker so demoting `p` already prevents the theft).
  auto poisons_downstream_sugg_member = [&](int p) {
    const size_t limit = std::min(death_i + 1, b_insns.size());
    for (size_t i = birth_i; i < limit; ++i) {
      const RtlExpr* st = nullptr;
      if (b_insns[i].pattern.IsSet()) {
        st = &b_insns[i].pattern;
      } else if (b_insns[i].pattern.op == RtlOp::kParallel) {
        for (const RtlExpr& ch : b_insns[i].pattern.children) {
          if (ch.IsSet()) {
            if (st != nullptr) {
              st = nullptr;
              break;
            }
            st = &ch;
          }
        }
      }
      if (st == nullptr) {
        continue;
      }
      const RtlExpr* dst = &st->SetDest();
      if (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (!dst->IsPseudoReg() || dst->RegNo() == p ||
          std::find(members.begin(), members.end(), dst->RegNo()) == members.end()) {
        continue;
      }
      bool has_dead_p = false;
      bool has_conflicting_hard_dead = false;
      for (const RtlRegNote& n : b_insns[i].reg_notes) {
        if (n.kind != RtlMachineMode::kRegDead || n.datum.IsArgPointerReg()) {
          continue;
        }
        if (n.datum.IsPseudoReg() && n.datum.RegNo() == p) {
          has_dead_p = true;
        } else if (n.datum.IsHardReg() &&
                   (span_mask(n.datum.RegNo(), n.datum.mode) & target_mask) == 0) {
          has_conflicting_hard_dead = true;
        }
      }
      if (has_dead_p && has_conflicting_hard_dead &&
          (!stolen_by_sugg_thief || max_thief_birth_i >= i)) {
        return true;
      }
    }
    return false;
  };
  auto is_clean = [&](int p) {
    const auto info_it = info.find(p);
    return (info_it == info.end() || !info_it->second.has_reg_equiv) &&
           !HasMultiClobberParallelDef(b_insns, p) &&
           !WouldDemoteCreateSelfSetRegDead(p, target_mask, b_insns, birth_i, death_i, gp64,
                                            fp64) &&
           !poisons_downstream_sugg_member(p);
  };
  for (auto it = members.rbegin(); it != members.rend(); ++it) {
    if (false_pseudo_combine.contains(*it) && is_clean(*it)) {
      return *it;
    }
  }
  for (auto it = members.rbegin(); it != members.rend(); ++it) {
    if (is_clean(*it)) {
      return *it;
    }
  }
  return members.back();
}

// When a short quantity group `g` cannot be demoted without creating an illegal
// post-`InvertReload` self-setting `REG_DEAD`, `local-alloc.c:1512, 1598`
// (`qty_compare_1`) prioritizes `g` ahead of an overlapping longer group
// `other_g` holding a lower-`REG_ALLOC_ORDER` register valid for `g.mode`.
// Pinning a clean member of `other_g` to its hard home occupies that lower
// register across `g`'s live range so `find_free_reg` assigns `g` to `g.target_hr`.
bool TryDemoteOverlappingLowerOrderBlocker(
    int root, const LocalQtyGroup& g, const std::vector<int>& group_roots,
    const absl::flat_hash_map<int, LocalQtyGroup>& groups,
    const std::vector<RtlPatternAndNotes>& b_insns,
    const absl::flat_hash_map<int, LocalResolutionCandInfo>& info, bool gp64, bool fp64,
    absl::flat_hash_map<int, int>* demote_to_hr) {
  const bool want_gpr = (g.target_hr < 32);
  const int g_ord = LookupMipsAllocOrderIndex(g.target_hr);
  const int g_nw = std::max(1, MipsHardRegNregs(g.target_hr, g.mode, gp64, fp64));
  auto span_mask = [&](int r, RtlMachineMode md) -> uint64_t {
    if (r < 0 || r >= 64) {
      return 0;
    }
    uint64_t msk = 0;
    const int nw = std::max(1, MipsHardRegNregs(r, md, gp64, fp64));
    for (int w = 0; w < nw && r + w < 64; ++w) {
      msk |= (uint64_t{1} << (r + w));
    }
    return msk;
  };
  for (int other_root : group_roots) {
    if (other_root == root) {
      continue;
    }
    const LocalQtyGroup& other_g = groups.at(other_root);
    if ((other_g.target_hr < 32) != want_gpr || other_g.target_hr == g.target_hr ||
        other_g.target_hr == kFramePointerRegno ||
        LookupMipsAllocOrderIndex(other_g.target_hr) >= g_ord || other_g.death_i < g.birth_i ||
        g.death_i < other_g.birth_i) {
      continue;
    }
    if (g_nw == 2 && (other_g.target_hr % 2) != 0) {
      continue;
    }
    const uint64_t other_mask = span_mask(other_g.target_hr, other_g.mode);
    static const absl::flat_hash_set<int> no_false_combine;
    const int other_p =
        SelectLocalResolutionDemoteMember(other_g.members, other_mask, b_insns, other_g.birth_i,
                                          other_g.death_i, gp64, fp64, info, no_false_combine);
    if (demote_to_hr->contains(other_p)) {
      continue;
    }
    const auto info_it = info.find(other_p);
    if ((info_it != info.end() && info_it->second.has_reg_equiv) ||
        HasMultiClobberParallelDef(b_insns, other_p) ||
        WouldDemoteCreateSelfSetRegDead(other_p, other_mask, b_insns, other_g.birth_i,
                                        other_g.death_i, gp64, fp64)) {
      continue;
    }
    (*demote_to_hr)[other_p] = other_g.target_hr;
    return true;
  }
  return false;
}

// When `cand_demote_p` cannot be demoted without producing an illegal pre-alloc
// self-setting `REG_DEAD` (`flow.c:2505-2520`, `sched.c:2872-2906`), first tries
// breaking a `local-alloc.c:1855` `false_pseudo_combine(src_p, cand_demote_p)`
// from the dying source side (`local-alloc.c:1798`) if clean; otherwise falls
// back to pinning an overlapping lower-order blocker (`local-alloc.c:1512, 1598`).
// Returns true iff an alternative clean demotion was recorded.
bool TryDemoteFalseCombineSourceOrLowerOrderBlocker(
    int cand_demote_p, int root, const LocalQtyGroup& g, const std::vector<int>& group_roots,
    const absl::flat_hash_map<int, LocalQtyGroup>& groups,
    const absl::flat_hash_map<int, int>& false_combine_source_of,
    const absl::flat_hash_map<int, int>& reg_renumber,
    const absl::flat_hash_map<int, size_t>& p_birth,
    const absl::flat_hash_map<int, size_t>& p_death, const std::vector<RtlPatternAndNotes>& b_insns,
    const absl::flat_hash_map<int, LocalResolutionCandInfo>& info, bool gp64, bool fp64,
    absl::flat_hash_map<int, int>* demote_to_hr) {
  auto span_mask = [&](int r, RtlMachineMode md) -> uint64_t {
    if (r < 0 || r >= 64) {
      return 0;
    }
    uint64_t msk = 0;
    const int nw = std::max(1, MipsHardRegNregs(r, md, gp64, fp64));
    for (int w = 0; w < nw && r + w < 64; ++w) {
      msk |= (uint64_t{1} << (r + w));
    }
    return msk;
  };
  if (const auto src_it = false_combine_source_of.find(cand_demote_p);
      src_it != false_combine_source_of.end()) {
    const int src_p = src_it->second;
    const auto sren_it = reg_renumber.find(src_p);
    const auto sinfo_it = info.find(src_p);
    if (sren_it != reg_renumber.end() && sinfo_it != info.end() &&
        sren_it->second != kFramePointerRegno && !sinfo_it->second.has_reg_equiv &&
        p_birth.contains(src_p) && p_death.contains(src_p) &&
        !HasMultiClobberParallelDef(b_insns, src_p) &&
        !WouldDemoteCreateSelfSetRegDead(src_p, span_mask(sren_it->second, sinfo_it->second.mode),
                                         b_insns, p_birth.at(src_p), p_death.at(src_p), gp64,
                                         fp64)) {
      (*demote_to_hr)[src_p] = sren_it->second;
      return true;
    }
  }
  return TryDemoteOverlappingLowerOrderBlocker(root, g, group_roots, groups, b_insns, info, gp64,
                                               fp64, demote_to_hr);
}

// Mirrors `local-alloc.c:1317-1352, 2282-2315` (`no_conflict_p` + `combine_regs`):
// when a `(clobber (reg:M pd))` carrying `REG_LIBCALL` is paired with a closing
// `(set (reg:M pd) (reg:M pd))` carrying `REG_RETVAL` + `REG_EQUAL (op (reg:M' ps) ...)`
// where both `pd` and `ps` are single-block local candidates with tieable modes,
// matching target hard registers, and every intervening insn mentioning `ps`
// carries `REG_NO_CONFLICT ps`, `block_alloc` combines `ps` and `pd` into a
// single quantity.
void UnionNoConflictLibcallQuantities(const std::vector<RtlPatternAndNotes>& b_insns,
                                      const absl::flat_hash_map<int, LocalResolutionCandInfo>& info,
                                      const absl::flat_hash_map<int, int>& reg_renumber,
                                      absl::flat_hash_map<int, int>* uf_parent) {
  auto find_q = [&](int p) {
    int r = p;
    while (uf_parent->at(r) != r) {
      r = uf_parent->at(r);
    }
    int c = p;
    while (c != r) {
      const int nxt = uf_parent->at(c);
      (*uf_parent)[c] = r;
      c = nxt;
    }
    return r;
  };
  auto modes_tieable = [](RtlMachineMode m1, RtlMachineMode m2) {
    const bool f1 = (m1 == RtlMachineMode::kSF || m1 == RtlMachineMode::kDF);
    const bool f2 = (m2 == RtlMachineMode::kSF || m2 == RtlMachineMode::kDF);
    return f1 == f2;
  };
  const size_t m = b_insns.size();
  for (size_t c = 0; c < m; ++c) {
    const RtlPatternAndNotes& cpn = b_insns[c];
    if (cpn.pattern.op != RtlOp::kClobber || cpn.pattern.children.size() != 1 ||
        !cpn.pattern.children[0].IsPseudoReg() ||
        !HasRegNoteKind(cpn, RtlMachineMode::kRegLibcall)) {
      continue;
    }
    const int pd = cpn.pattern.children[0].RegNo();
    if (!uf_parent->contains(pd)) {
      continue;
    }
    for (size_t r_idx = c + 1; r_idx < m; ++r_idx) {
      const RtlPatternAndNotes& rpn = b_insns[r_idx];
      if (!HasRegNoteKind(rpn, RtlMachineMode::kRegRetval)) {
        continue;
      }
      if (!rpn.pattern.IsSet() || !rpn.pattern.SetDest().IsPseudoReg() ||
          rpn.pattern.SetDest().RegNo() != pd || !rpn.pattern.SetSrc().IsPseudoReg() ||
          rpn.pattern.SetSrc().RegNo() != pd) {
        break;
      }
      int ps = -1;
      for (const RtlRegNote& n : rpn.reg_notes) {
        if (n.kind != RtlMachineMode::kRegEqual) {
          continue;
        }
        if (n.datum.IsPseudoReg()) {
          ps = n.datum.RegNo();
        } else if (!n.datum.children.empty() && n.datum.children[0].IsPseudoReg()) {
          ps = n.datum.children[0].RegNo();
        }
        break;
      }
      if (ps < 0 || ps == pd || !uf_parent->contains(ps) ||
          !modes_tieable(info.at(pd).mode, info.at(ps).mode)) {
        break;
      }
      bool ok_no_conflict = true;
      for (size_t k = c + 1; k < r_idx; ++k) {
        if (!b_insns[k].pattern.ContainsReg(ps)) {
          continue;
        }
        bool has_nc = false;
        for (const RtlRegNote& n : b_insns[k].reg_notes) {
          if (n.kind == RtlMachineMode::kRegNoConflict && n.datum.IsPseudoReg() &&
              n.datum.RegNo() == ps) {
            has_nc = true;
            break;
          }
        }
        if (!has_nc) {
          ok_no_conflict = false;
          break;
        }
      }
      if (!ok_no_conflict) {
        break;
      }
      const int hd = reg_renumber.at(pd);
      const int hs = reg_renumber.at(ps);
      if (hd == hs) {
        const int rq = find_q(ps);
        const int dq = find_q(pd);
        if (rq != dq) {
          (*uf_parent)[dq] = rq;
        }
      }
      break;
    }
  }
}

// Per-spill-offset CFG usage summary used to distinguish single-block
// (`reg_basic_block >= 0`, `reg_qty == -2`) from multi-block/multi-def
// (`reg_basic_block == REG_BLOCK_GLOBAL`, `reg_qty == -1`) reload spill slots
// (`local-alloc.c:470-478, 1773-1774`).
struct ReloadSpillSlotSummary {
  absl::flat_hash_map<int64_t, absl::flat_hash_set<size_t>> blocks;
  absl::flat_hash_map<int64_t, int> stores;
};

ReloadSpillSlotSummary SummarizeReloadSpillSlots(
    const std::vector<std::vector<RtlPatternAndNotes>>& live_blocks, int spill_base,
    const absl::flat_hash_set<int64_t>& reload_spill_offsets) {
  ReloadSpillSlotSummary summary;
  if (reload_spill_offsets.empty()) {
    return summary;
  }
  for (size_t bb = 0; bb < live_blocks.size(); ++bb) {
    for (const RtlPatternAndNotes& pn : live_blocks[bb]) {
      pn.pattern.Visit([&](const RtlExpr& e) {
        if (e.op != RtlOp::kMem || e.children.empty() || e.flags.volatil || e.flags.in_struct) {
          return;
        }
        const std::optional<int64_t> off =
            ExtractBaseRelativeConstOffset(e.children[0], spill_base);
        if (off.has_value() && reload_spill_offsets.contains(*off)) {
          summary.blocks[*off].insert(bb);
        }
      });
      auto count_store = [&](const RtlExpr& s) {
        if (!s.IsSet() || s.SetDest().op != RtlOp::kMem || s.SetDest().children.empty() ||
            s.SetDest().flags.volatil || s.SetDest().flags.in_struct) {
          return;
        }
        const std::optional<int64_t> off =
            ExtractBaseRelativeConstOffset(s.SetDest().children[0], spill_base);
        if (off.has_value() && reload_spill_offsets.contains(*off)) {
          summary.stores[*off] += 1;
        }
      };
      if (pn.pattern.IsSet()) {
        count_store(pn.pattern);
      } else if (pn.pattern.IsParallel()) {
        for (const RtlExpr& ch : pn.pattern.children) {
          count_store(ch);
        }
      }
    }
  }
  return summary;
}

// Computes `all_live_ticks` (`local-alloc.c:block_alloc` `regs_live_at` across
// hard regs + local quantities) and identifies `b_locals` pseudos combined by
// `local-alloc.c:1722` (`combine_regs`) into a single-block spilled pseudo
// (`local-alloc.c:470-478, 1773-1774`: `reg_qty[sreg] == -2`), whose merged
// quantity spills during `block_alloc` (`qty_phys_reg == -1`) and is deferred
// to `global.c:global_alloc`. Multi-block/global spilled destinations have
// `reg_qty[sreg] == -1`, so `combine_regs` returns `0` and `p` stays a normal
// local quantity occupying `reg_renumber[p]` in `regs_live_at`.
std::vector<uint64_t> ComputeLocalResolutionAllLiveTicks(
    const RtlControlFlowGraph& live_probe, size_t b, const std::vector<RtlPatternAndNotes>& b_insns,
    const absl::flat_hash_map<int, LocalResolutionCandInfo>& info,
    const absl::flat_hash_map<int, int>& reg_renumber,
    const absl::flat_hash_set<int>& live_at_any_start, int spill_base,
    const absl::flat_hash_set<int64_t>& reload_spill_offsets,
    const ReloadSpillSlotSummary& spill_summary, const std::vector<int>& b_locals,
    const absl::flat_hash_map<int, size_t>& p_birth,
    const absl::flat_hash_map<int, size_t>& p_death, const std::vector<int>& b_multi_locals,
    const absl::flat_hash_map<int, size_t>& multi_first_i,
    const absl::flat_hash_map<int, size_t>& multi_last_i,
    const std::vector<uint64_t>& base_live_ticks, bool gp64, bool fp64) {
  const size_t m = b_insns.size();
  std::vector<uint64_t> all_live_ticks = base_live_ticks;
  auto span_mask = [&](int hr, RtlMachineMode mode) -> uint64_t {
    uint64_t mask = 0;
    const int span = std::max(1, MipsHardRegNregs(hr, mode, gp64, fp64));
    for (int w = 0; w < span && hr + w < 64; ++w) {
      if (hr + w >= 0) {
        mask |= (uint64_t{1} << (hr + w));
      }
    }
    return mask;
  };
  auto has_dead_note = [](const RtlPatternAndNotes& pn, int regno) {
    return std::any_of(pn.reg_notes.begin(), pn.reg_notes.end(), [&](const RtlRegNote& n) {
      return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
             !n.datum.IsArgPointerReg() && n.datum.RegNo() == regno;
    });
  };
  auto extract_single_set = [](const RtlExpr& pat) -> const RtlExpr* {
    if (pat.IsSet()) {
      return &pat;
    }
    if (pat.IsParallel()) {
      const RtlExpr* found = nullptr;
      for (const RtlExpr& ch : pat.children) {
        if (ch.IsSet()) {
          if (found != nullptr) {
            return nullptr;
          }
          found = &ch;
        }
      }
      return found;
    }
    return nullptr;
  };
  auto spill_store_offset_of_reg = [&](size_t idx, int regno,
                                       RtlMachineMode mode) -> std::optional<int64_t> {
    if (reload_spill_offsets.empty() || idx >= m) {
      return std::nullopt;
    }
    const RtlExpr* st = extract_single_set(b_insns[idx].pattern);
    if (st == nullptr || st->SetDest().op != RtlOp::kMem || st->SetDest().children.empty() ||
        st->SetDest().flags.volatil || st->SetDest().flags.in_struct ||
        st->SetSrc().op != RtlOp::kReg || st->SetSrc().RegNo() != regno ||
        st->SetSrc().mode != mode) {
      return std::nullopt;
    }
    const std::optional<int64_t> off =
        ExtractBaseRelativeConstOffset(st->SetDest().children[0], spill_base);
    if (off.has_value() && reload_spill_offsets.contains(*off)) {
      return off;
    }
    return std::nullopt;
  };
  auto is_single_block_spill_slot = [&](int64_t off) {
    const auto bit = spill_summary.blocks.find(off);
    const auto sit = spill_summary.stores.find(off);
    return bit != spill_summary.blocks.end() && bit->second.size() == 1 &&
           bit->second.contains(b) && sit != spill_summary.stores.end() && sit->second == 1;
  };
  // `local-alloc.c:1241-1300` (`block_alloc` -> `combine_regs`): operands are
  // tried left-to-right and the loop `break`s on the first `win = 1`. Thus a
  // second dying same-bank local pseudo operand in `SET_SRC` is never combined
  // with `SET_DEST` when an earlier dying same-bank local pseudo operand wins
  // `combine_regs` first.
  auto is_first_dying_local_src = [&](const RtlExpr& expr, int target_p, size_t insn_idx,
                                      bool target_fp) -> bool {
    int first_winner = -1;
    auto walk = [&](const auto& self, const RtlExpr& e) -> void {
      if (first_winner >= 0 || e.op == RtlOp::kMem) {
        return;
      }
      if (e.op == RtlOp::kReg && !e.IsArgPointerReg()) {
        if (e.IsPseudoReg()) {
          const int r = e.RegNo();
          const bool r_fp = (e.mode == RtlMachineMode::kSF || e.mode == RtlMachineMode::kDF);
          if (r_fp == target_fp && p_birth.contains(r) && has_dead_note(b_insns[insn_idx], r)) {
            first_winner = r;
          }
        }
        return;
      }
      if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg &&
          !e.children[0].IsArgPointerReg()) {
        const RtlExpr& inner = e.children[0];
        if (inner.IsPseudoReg()) {
          const int r = inner.RegNo();
          const bool r_fp =
              (inner.mode == RtlMachineMode::kSF || inner.mode == RtlMachineMode::kDF);
          if (r_fp == target_fp && p_birth.contains(r) && has_dead_note(b_insns[insn_idx], r)) {
            first_winner = r;
          }
        }
        return;
      }
      for (const RtlExpr& ch : e.children) {
        self(self, ch);
      }
    };
    walk(walk, expr);
    return first_winner == target_p;
  };
  auto dies_into_spilled_pseudo = [&](int p, size_t death_i) {
    if (death_i >= m || !has_dead_note(b_insns[death_i], p)) {
      return false;
    }
    if (spill_store_offset_of_reg(death_i, p, info.at(p).mode).has_value()) {
      return true;
    }
    const RtlExpr* st = extract_single_set(b_insns[death_i].pattern);
    if (st == nullptr || st->SetDest().op != RtlOp::kReg || st->SetDest().IsArgPointerReg()) {
      return false;
    }
    const RtlExpr& dst = st->SetDest();
    const bool p_fp =
        (info.at(p).mode == RtlMachineMode::kSF || info.at(p).mode == RtlMachineMode::kDF);
    const bool d_fp = (dst.mode == RtlMachineMode::kSF || dst.mode == RtlMachineMode::kDF);
    if (p_fp != d_fp || !is_first_dying_local_src(st->SetSrc(), p, death_i, p_fp)) {
      return false;
    }
    if (dst.IsPseudoReg()) {
      const int pd = dst.RegNo();
      if (pd == p) {
        return false;
      }
      const auto rit = reg_renumber.find(pd);
      if (rit == reg_renumber.end() || rit->second < 0) {
        // `local-alloc.c:470-478, 1773-1774`: `combine_regs` merges `p` into
        // `pd` only when `reg_qty[pd] != -1` (`reg_basic_block[pd] == b` and
        // `reg_n_deaths[pd] == 1`). Global spilled destinations (`reg_qty == -1`)
        // cannot combine with `p`, so `p` remains a normal local quantity.
        const auto pit = info.find(pd);
        const auto dit = live_probe.sideband.reg_n_deaths.find(pd);
        return !live_at_any_start.contains(pd) && pit != info.end() &&
               pit->second.blocks.size() == 1 && pit->second.blocks.contains(b) &&
               dit != live_probe.sideband.reg_n_deaths.end() && dit->second == 1;
      }
    }
    if (!reload_spill_offsets.empty()) {
      for (size_t k = death_i + 1; k < m; ++k) {
        if (const std::optional<int64_t> off = spill_store_offset_of_reg(k, dst.RegNo(), dst.mode);
            off.has_value()) {
          if (dst.IsHardReg() && dst.RegNo() >= 0 && dst.RegNo() < 64) {
            const uint64_t dm = span_mask(dst.RegNo(), dst.mode);
            const size_t t_s = 2 * death_i + 1;
            const size_t t_e = std::max(t_s, 2 * k);
            for (size_t t = t_s; t <= t_e && t < all_live_ticks.size(); ++t) {
              all_live_ticks[t] &= ~dm;
            }
          }
          return is_single_block_spill_slot(*off);
        }
        if (b_insns[k].pattern.ContainsReg(dst.RegNo()) || ContainsCallOp(b_insns[k].pattern)) {
          break;
        }
      }
    }
    return false;
  };

  // Pre-pass: identify locals dying into spilled pseudos and clear any
  // reload-spill-store hard-register tick windows from `base_live_ticks`
  // before OR'ing surviving local live intervals so the bitmask update is
  // strictly order-independent.
  std::vector<bool> local_dies_into_spill(b_locals.size(), false);
  for (size_t idx = 0; idx < b_locals.size(); ++idx) {
    const int p = b_locals[idx];
    if (!p_birth.contains(p)) {
      continue;
    }
    if (dies_into_spilled_pseudo(p, p_death.at(p))) {
      local_dies_into_spill[idx] = true;
    }
  }
  for (size_t idx = 0; idx < b_locals.size(); ++idx) {
    const int p = b_locals[idx];
    if (!p_birth.contains(p) || local_dies_into_spill[idx]) {
      continue;
    }
    const uint64_t pm = span_mask(reg_renumber.at(p), info.at(p).mode);
    const size_t t_start = 2 * p_birth.at(p) + 1;
    const size_t t_end = std::max(t_start, 2 * p_death.at(p));
    for (size_t t = t_start; t <= t_end && t < all_live_ticks.size(); ++t) {
      all_live_ticks[t] |= pm;
    }
  }
  for (int pm : b_multi_locals) {
    if (!multi_first_i.contains(pm)) {
      continue;
    }
    const uint64_t pmask = span_mask(reg_renumber.at(pm), info.at(pm).mode);
    const size_t t_start = 2 * multi_first_i.at(pm) + 1;
    const size_t t_end = std::max(t_start, 2 * multi_last_i.at(pm));
    for (size_t t = t_start; t <= t_end && t < all_live_ticks.size(); ++t) {
      all_live_ticks[t] |= pmask;
    }
  }
  return all_live_ticks;
}

// Resolves single-block local quantities whose target `reg_renumber` cannot be
// reproduced by `local-alloc.c:block_alloc` (`combine_regs`, `just_try_suggested`,
// or `find_free_reg`), either by coalescing with an earlier disjoint local
// pseudo of the same `(reg_renumber, mode, raw_mode, flags)` in the block or by
// keeping the register pinned as its hard register.
// Mirrors `local-alloc.c:1525-1538, 1855-1870, 2106, 2121, 2205-2216`:
// when a local non-call-crossing GPR pseudo `ps` (`hs` call-used) dies in the
// birth of a call-crossing GPR pseudo `pd` (`hd` callee-saved or `$fp`,
// `pd_calls_crossed > 0`), `local-alloc` combines them into a single call-crossing
// quantity. If all callee-saved GPRs `$s0..$s7` (`16..23`; `$fp` is excluded by
// `ELIMINABLE_REGS` at `local-alloc.c:2121`) are occupied over
// `[birth(ps), death(pd)]` and the combined quantity is not
// `CALLER_SAVE_PROFITABLE` (`n_refs <= 4 * calls_crossed`, `regs.h:165`,
// `local-alloc.c:2209`), `local-alloc` leaves the combined qty unallocated (`-1`) so `global-alloc`
// allocates `ps` (`hs`) and `pd` (`hd`) separately.
bool IsUnallocatableCallCrossingGprCombine(const RtlControlFlowGraph& live_probe,
                                           const std::vector<RtlPatternAndNotes>& b_insns,
                                           const std::vector<uint64_t>& all_live_ticks,
                                           const absl::flat_hash_map<int, size_t>& p_birth,
                                           const absl::flat_hash_map<int, size_t>& p_death, int ps,
                                           int pd, int hs, int hd, RtlMachineMode mode, bool gp64,
                                           bool fp64) {
  if (hs < 2 || hs >= 32 || !IsCallUsedHardReg(hs) || hd < 2 || hd >= 32 || IsCallUsedHardReg(hd)) {
    return false;
  }
  const auto cc_pd = live_probe.sideband.reg_n_calls_crossed.find(pd);
  const int calls_pd = (cc_pd != live_probe.sideband.reg_n_calls_crossed.end()) ? cc_pd->second : 0;
  const auto cc_ps = live_probe.sideband.reg_n_calls_crossed.find(ps);
  const int calls_ps = (cc_ps != live_probe.sideband.reg_n_calls_crossed.end()) ? cc_ps->second : 0;
  const int total_calls = calls_pd + calls_ps;
  if (total_calls <= 0 || !p_birth.contains(ps) || !p_death.contains(pd)) {
    return false;
  }
  int combined_refs = 0;
  const size_t q_birth = std::min(p_birth.at(ps), p_birth.at(pd));
  const size_t q_death = std::max(p_death.at(ps), p_death.at(pd));
  for (size_t k = q_birth; k <= q_death && k < b_insns.size(); ++k) {
    b_insns[k].pattern.Visit([&](const RtlExpr& n) {
      if (n.IsPseudoReg() && (n.RegNo() == ps || n.RegNo() == pd)) {
        ++combined_refs;
      }
    });
  }
  if (combined_refs > 4 * total_calls) {
    return false;
  }
  const size_t t_start = 2 * q_birth + 1;
  const size_t t_end = std::max(t_start, 2 * q_death);
  const int span = std::max(1, MipsHardRegNregs(hd, mode, gp64, fp64));
  for (int sreg = 16; sreg + span <= 24; sreg += (span > 1 ? 2 : 1)) {
    uint64_t sm = 0;
    for (int w = 0; w < span; ++w) {
      sm |= (uint64_t{1} << (sreg + w));
    }
    bool free_here = true;
    for (size_t t = t_start; t <= t_end && t < all_live_ticks.size(); ++t) {
      if ((all_live_ticks[t] & sm) != 0) {
        free_here = false;
        break;
      }
    }
    if (free_here) {
      return false;
    }
  }
  return true;
}

// Mirrors `local-alloc.c:1525-1538, 1855-1870, 2106, 2121, 2205-2216`:
// when a call-crossing `SF`/`DF` pseudo `ps` allocated to `$fp` (`30`,
// excluded from `local-alloc` by `ELIMINABLE_REGS` at `local-alloc.c:2121`)
// dies in the birth of a non-call-crossing `SF`/`DF` pseudo `pd` allocated to
// a call-used FPR (`32..51`), `local-alloc` combines them into a single
// call-crossing quantity. If all callee-saved FPRs (`$f20..$f30`, `52..63`)
// AND all callee-saved GPRs (`$s0..$s7`, `16..23`) are occupied over
// `[birth(ps), death(pd)]` and `combined_refs <= 4 * total_calls`
// (`CALLER_SAVE_PROFITABLE`, `regs.h:165`, `local-alloc.c:2209`),
// `local-alloc` leaves the combined qty unallocated (`-1`) so `global-alloc`
// allocates `ps` (`$fp`) and `pd` (`hd`) separately.
bool IsUnallocatableCallCrossingFpGprCombine(const RtlControlFlowGraph& live_probe,
                                             const std::vector<RtlPatternAndNotes>& b_insns,
                                             const std::vector<uint64_t>& all_live_ticks,
                                             const absl::flat_hash_map<int, size_t>& p_birth,
                                             const absl::flat_hash_map<int, size_t>& p_death,
                                             int ps, int pd, int hs, int hd, RtlMachineMode mode,
                                             bool gp64, bool fp64) {
  if ((mode != RtlMachineMode::kSF && mode != RtlMachineMode::kDF) || hs != kFramePointerRegno ||
      hd < 32 || hd >= 64 || !IsCallUsedHardReg(hd)) {
    return false;
  }
  const auto cc_pd = live_probe.sideband.reg_n_calls_crossed.find(pd);
  const int calls_pd = (cc_pd != live_probe.sideband.reg_n_calls_crossed.end()) ? cc_pd->second : 0;
  const auto cc_ps = live_probe.sideband.reg_n_calls_crossed.find(ps);
  const int calls_ps = (cc_ps != live_probe.sideband.reg_n_calls_crossed.end()) ? cc_ps->second : 0;
  const int total_calls = calls_pd + calls_ps;
  if (total_calls <= 0 || !p_birth.contains(ps) || !p_death.contains(pd)) {
    return false;
  }
  int combined_refs = 0;
  const size_t q_birth = std::min(p_birth.at(ps), p_birth.at(pd));
  const size_t q_death = std::max(p_death.at(ps), p_death.at(pd));
  for (size_t k = q_birth; k <= q_death && k < b_insns.size(); ++k) {
    b_insns[k].pattern.Visit([&](const RtlExpr& n) {
      if (n.IsPseudoReg() && (n.RegNo() == ps || n.RegNo() == pd)) {
        ++combined_refs;
      }
    });
  }
  if (combined_refs > 4 * total_calls) {
    return false;
  }
  const size_t t_start = 2 * q_birth + 1;
  const size_t t_end = std::max(t_start, 2 * q_death);
  const int fp_span = std::max(1, MipsHardRegNregs(hd, mode, gp64, fp64));
  const int fp_step = fp64 ? 1 : 2;
  for (int freg = 52; freg + fp_span <= 64; freg += fp_step) {
    uint64_t fm = 0;
    for (int w = 0; w < fp_span; ++w) {
      fm |= (uint64_t{1} << (freg + w));
    }
    bool free_here = true;
    for (size_t t = t_start; t <= t_end && t < all_live_ticks.size(); ++t) {
      if ((all_live_ticks[t] & fm) != 0) {
        free_here = false;
        break;
      }
    }
    if (free_here) {
      return false;
    }
  }
  const int gp_span = std::max(1, MipsHardRegNregs(hs, mode, gp64, fp64));
  for (int sreg = 16; sreg + gp_span <= 24; sreg += (gp_span > 1 ? 2 : 1)) {
    uint64_t sm = 0;
    for (int w = 0; w < gp_span; ++w) {
      sm |= (uint64_t{1} << (sreg + w));
    }
    bool free_here = true;
    for (size_t t = t_start; t <= t_end && t < all_live_ticks.size(); ++t) {
      if ((all_live_ticks[t] & sm) != 0) {
        free_here = false;
        break;
      }
    }
    if (free_here) {
      return false;
    }
  }
  return true;
}

// True when `g` only needs resolution because a lower-alloc-order register
// appeared free (`local-alloc.c:2158-2181` `find_free_reg`), and an overlapping
// same-bank group `other_g` with a strictly lower `REG_ALLOC_ORDER` target
// register is already scheduled for resolution in this iteration. Deferring `g`
// lets `other_g` become hard-live first so `g` is not spuriously demoted.
bool HasOverlappingLowerOrderPendingResolution(
    int root, const LocalQtyGroup& g, const std::vector<int>& group_roots,
    const absl::flat_hash_map<int, LocalQtyGroup>& groups,
    const absl::flat_hash_set<int>& false_pseudo_combine) {
  if (g.stolen_by_sugg_thief || g.has_any_sugg) {
    return false;
  }
  for (int mp : g.members) {
    if (false_pseudo_combine.contains(mp)) {
      return false;
    }
  }
  const bool want_gpr = (g.target_hr < 32);
  const int g_ord = LookupMipsAllocOrderIndex(g.target_hr);
  const size_t b1 = 2 * g.birth_i + 1;
  const size_t d1 = std::max(2 * g.birth_i + 2, 2 * g.death_i);
  for (int other_root : group_roots) {
    if (other_root == root) {
      continue;
    }
    const LocalQtyGroup& og = groups.at(other_root);
    if (!og.needs_resolution || og.has_any_sugg || (og.target_hr < 32) != want_gpr ||
        LookupMipsAllocOrderIndex(og.target_hr) >= g_ord) {
      continue;
    }
    const size_t b2 = 2 * og.birth_i + 1;
    const size_t d2 = std::max(2 * og.birth_i + 2, 2 * og.death_i);
    if (std::max(b1, b2) < std::min(d1, d2)) {
      return true;
    }
  }
  return false;
}

// Returns true iff pseudo `p` appears on any instruction in `b_insns[bi..di]`
// referencing a reload spill stack slot in `reload_spill_offsets`
// (`reload1.c:2309-2400`). Used to avoid coalescing single-def/single-use
// spill reload pseudos across disjoint lifetimes while still allowing ordinary
// local pseudos to coalesce cleanly into `global_alloc`.
bool PseudoTouchesReloadSpillSlot(const std::vector<RtlPatternAndNotes>& b_insns, int p, size_t bi,
                                  size_t di, int spill_base_r,
                                  const absl::flat_hash_set<int64_t>& reload_spill_offsets) {
  if (reload_spill_offsets.empty()) {
    return false;
  }
  const size_t limit = std::min(di + 1, b_insns.size());
  for (size_t k = bi; k < limit; ++k) {
    if (!b_insns[k].pattern.ContainsReg(p)) {
      continue;
    }
    bool hit = false;
    b_insns[k].pattern.Visit([&](const RtlExpr& e) {
      if (hit || e.op != RtlOp::kMem || e.children.empty() || e.flags.volatil ||
          e.flags.in_struct) {
        return;
      }
      const std::optional<int64_t> off =
          ExtractBaseRelativeConstOffset(e.children[0], spill_base_r);
      if (off.has_value() && reload_spill_offsets.contains(*off)) {
        hit = true;
      }
    });
    if (hit) {
      return true;
    }
  }
  return false;
}

enum class LocalDemoteOutcome {
  kNoAction,
  kDemoted,
};

LocalDemoteOutcome ResolveLocalQtyGroupDemotion(
    size_t b, int root, const LocalQtyGroup& g, const std::vector<int>& group_roots,
    const absl::flat_hash_map<int, LocalQtyGroup>& groups,
    const absl::flat_hash_map<int, int>& false_combine_source_of,
    const absl::flat_hash_set<int>& false_pseudo_combine,
    const absl::flat_hash_set<int>& is_local_cand,
    const absl::flat_hash_map<int, uint64_t>& global_hr_conflicts,
    const absl::flat_hash_map<int, size_t>& p_birth,
    const absl::flat_hash_map<int, size_t>& p_death,
    const absl::flat_hash_map<int, LocalResolutionCandInfo>& info, bool gp64, bool fp64,
    bool runs_sched1, bool block_sched1_achievable_before, const CompilationProfile& profile,
    const std::vector<RtlPatternAndNotes>& b_insns, absl::flat_hash_map<int, int>* demote_to_hr,
    RtlControlFlowGraph* out,
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks) {
  const size_t m = b_insns.size();
  auto span_mask = [&](int r, RtlMachineMode md) -> uint64_t {
    if (r < 0 || r >= 64) {
      return 0;
    }
    uint64_t msk = 0;
    const int nw = std::max(1, MipsHardRegNregs(r, md, gp64, fp64));
    for (int w = 0; w < nw && r + w < 64; ++w) {
      msk |= (uint64_t{1} << (r + w));
    }
    return msk;
  };
  auto extract_single_set = [](const RtlExpr& pat) -> const RtlExpr* {
    if (pat.IsSet()) {
      return &pat;
    }
    if (pat.op == RtlOp::kParallel) {
      const RtlExpr* single = nullptr;
      for (const RtlExpr& ch : pat.children) {
        if (ch.IsSet()) {
          if (single != nullptr) {
            return nullptr;
          }
          single = &ch;
        }
      }
      return single;
    }
    return nullptr;
  };
  auto has_dead_note = [](const RtlPatternAndNotes& pn, int regno) {
    for (const RtlRegNote& n : pn.reg_notes) {
      if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() && n.datum.RegNo() == regno) {
        return true;
      }
    }
    return false;
  };

  const absl::flat_hash_set<int> mset(g.members.begin(), g.members.end());
  bool creates_false_global_pref = false;
  const uint64_t target_mask = span_mask(g.target_hr, g.mode);
  for (size_t i = g.birth_i; i <= g.death_i && i < m && !creates_false_global_pref; ++i) {
    const RtlExpr* st = extract_single_set(b_insns[i].pattern);
    if (st == nullptr) {
      continue;
    }
    const RtlExpr* dst = &st->SetDest();
    if (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
      dst = &dst->children[0];
    }
    const RtlExpr* pref_src = &st->SetSrc();
    if (pref_src->op != RtlOp::kReg && pref_src->op != RtlOp::kSubreg) {
      if (pref_src->op != RtlOp::kMem && pref_src->op != RtlOp::kCall &&
          !pref_src->children.empty()) {
        pref_src = &pref_src->children[0];
      } else {
        pref_src = nullptr;
      }
    }
    if (pref_src != nullptr && pref_src->op == RtlOp::kSubreg && !pref_src->children.empty()) {
      pref_src = &pref_src->children[0];
    }
    if (pref_src != nullptr && pref_src->IsPseudoReg()) {
      const int sr_no = pref_src->RegNo();
      const uint64_t target_bit = uint64_t{1} << g.target_hr;
      if (dst->IsPseudoReg() && mset.contains(dst->RegNo())) {
        const auto cit = global_hr_conflicts.find(sr_no);
        const bool already_conflicts =
            !has_dead_note(b_insns[i], sr_no) ||
            (cit != global_hr_conflicts.end() && (cit->second & target_bit) != 0);
        if (!mset.contains(sr_no) && !is_local_cand.contains(sr_no) && !already_conflicts &&
            out->sideband.reg_renumber.contains(sr_no) &&
            out->sideband.reg_renumber.at(sr_no) != g.target_hr) {
          creates_false_global_pref = true;
        }
        continue;
      }
      if (dst->IsPseudoReg() && !mset.contains(dst->RegNo()) &&
          !is_local_cand.contains(dst->RegNo()) &&
          out->sideband.reg_renumber.contains(dst->RegNo()) &&
          out->sideband.reg_renumber.at(dst->RegNo()) != g.target_hr && mset.contains(sr_no)) {
        const auto cit = global_hr_conflicts.find(dst->RegNo());
        const bool already_conflicts =
            !has_dead_note(b_insns[i], sr_no) ||
            (cit != global_hr_conflicts.end() && (cit->second & target_bit) != 0);
        if (!already_conflicts) {
          creates_false_global_pref = true;
        }
      }
    }
  }
  if (creates_false_global_pref) {
    return LocalDemoteOutcome::kNoAction;
  }

  int cand_demote_p = SelectLocalResolutionDemoteMember(
      g.members, target_mask, b_insns, g.birth_i, g.death_i, gp64, fp64, info, false_pseudo_combine,
      g.stolen_by_sugg_thief, g.max_thief_birth_i);
  const std::array<size_t, 1> check_b = {b};
  auto keeps_sched1 = [&](const absl::flat_hash_map<int, int>& trial_demote) {
    return ArePreAllocBlocksSched1Achievable(*out, check_b, all_parsed_blocks, trial_demote,
                                             profile);
  };
  // Per `sched.c:2469-2549` (`birthing_insn_p`), when block `b` is currently in
  // `Im(F_sched1)` and demoting `cand_demote_p` to `g.target_hr` breaks
  // `Im(F_sched1)`, prefer an alternative clean group member or a clean
  // lower-order blocker demotion that keeps block `b` in `Im(F_sched1)`.
  if (runs_sched1 && block_sched1_achievable_before) {
    absl::flat_hash_map<int, int> trial_default = *demote_to_hr;
    trial_default[cand_demote_p] = g.target_hr;
    if (!keeps_sched1(trial_default)) {
      bool found_member = false;
      for (auto it = g.members.rbegin(); it != g.members.rend(); ++it) {
        const int mp = *it;
        if (mp == cand_demote_p) {
          continue;
        }
        const auto info_it = info.find(mp);
        if ((info_it != info.end() && info_it->second.has_reg_equiv) ||
            HasMultiClobberParallelDef(b_insns, mp) ||
            WouldDemoteCreateSelfSetRegDead(mp, target_mask, b_insns, g.birth_i, g.death_i, gp64,
                                            fp64)) {
          continue;
        }
        absl::flat_hash_map<int, int> trial_m = *demote_to_hr;
        trial_m[mp] = g.target_hr;
        if (keeps_sched1(trial_m)) {
          cand_demote_p = mp;
          found_member = true;
          break;
        }
      }
      if (!found_member) {
        absl::flat_hash_map<int, int> trial_blocker = *demote_to_hr;
        if (TryDemoteFalseCombineSourceOrLowerOrderBlocker(
                cand_demote_p, root, g, group_roots, groups, false_combine_source_of,
                out->sideband.reg_renumber, p_birth, p_death, b_insns, info, gp64, fp64,
                &trial_blocker) &&
            keeps_sched1(trial_blocker)) {
          *demote_to_hr = std::move(trial_blocker);
          return LocalDemoteOutcome::kDemoted;
        }
      }
    }
  }

  // `flow.c:2505-2520` / `sched.c:2872-2906` (`!dead_or_set_p`): avoid
  // demoting `cand_demote_p` to `target_hr` when doing so creates a
  // self-setting `REG_DEAD` (Cases 1–3) by first attempting a clean
  // source-side or lower-order-blocker demotion; if none exists, only
  // block Case 1 (`dst == cand_demote_p` with overlapping hard `REG_DEAD`)
  // so single-member hard-sugg/reload groups still resolve cleanly.
  const bool has_multi_clobber_def = HasMultiClobberParallelDef(b_insns, cand_demote_p);
  if (has_multi_clobber_def || WouldDemoteCreateSelfSetRegDead(cand_demote_p, target_mask, b_insns,
                                                               g.birth_i, g.death_i, gp64, fp64)) {
    if (TryDemoteFalseCombineSourceOrLowerOrderBlocker(cand_demote_p, root, g, group_roots, groups,
                                                       false_combine_source_of,
                                                       out->sideband.reg_renumber, p_birth, p_death,
                                                       b_insns, info, gp64, fp64, demote_to_hr) ||
        has_multi_clobber_def ||
        HasOverlappingHardRegDeadOnDef(cand_demote_p, target_mask, b_insns, g.birth_i, g.death_i,
                                       gp64, fp64)) {
      return LocalDemoteOutcome::kNoAction;
    }
  }
  (*demote_to_hr)[cand_demote_p] = g.target_hr;
  return LocalDemoteOutcome::kDemoted;
}

bool RunSingleLocalResolutionIteration(
    const CompilationProfile& profile, bool gp64, bool fp64,
    const absl::flat_hash_set<int64_t>& reload_spill_offsets,
    const std::vector<std::vector<uint64_t>>& demoted_reload_scratch_masks,
    RtlControlFlowGraph* out, std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr) {
  auto& all_parsed_blocks = *all_parsed_blocks_ptr;
  RtlControlFlowGraph live_probe = *out;
  SyncLiveProbeFromParsedBlocks(profile, all_parsed_blocks, &live_probe);

  using LocalInfo = LocalResolutionCandInfo;
  absl::flat_hash_map<int, LocalInfo> info;
  std::vector<std::vector<RtlPatternAndNotes>> live_blocks;
  if (!CollectLocalResolutionCandInfo(live_probe, out->sideband.reg_renumber, &info,
                                      &live_blocks)) {
    return false;
  }

  absl::flat_hash_set<int> live_at_any_start;
  for (const auto& [_, lset] : live_probe.sideband.basic_block_live_at_start) {
    for (int r : lset) {
      if (r >= kFirstPseudoRegister) {
        live_at_any_start.insert(r);
      }
    }
  }

  absl::flat_hash_set<int> is_local_cand;
  for (const auto& [p, hr] : out->sideband.reg_renumber) {
    const auto dit = live_probe.sideband.reg_n_deaths.find(p);
    const auto iit = info.find(p);
    if (p < kFirstPseudoRegister || hr < 2 || hr >= 64 ||
        (out->sideband.frame_pointer_needed && hr == kFramePointerRegno) ||
        live_at_any_start.contains(p) || dit == live_probe.sideband.reg_n_deaths.end() ||
        dit->second != 1 || iit == info.end() || iit->second.blocks.size() != 1 ||
        !iit->second.has_set || iit->second.used_before_set || iit->second.alive_after_redef ||
        iit->second.note_deaths != 1) {
      continue;
    }
    if (iit->second.def_count == 1 && iit->second.defined_from_md_reg &&
        !iit->second.used_in_mem_addr && iit->second.read_count <= 1) {
      continue;
    }
    const auto cc_it = live_probe.sideband.reg_n_calls_crossed.find(p);
    if (cc_it != live_probe.sideband.reg_n_calls_crossed.end() && cc_it->second > 0 &&
        IsCallUsedHardReg(hr)) {
      continue;
    }
    is_local_cand.insert(p);
  }
  if (is_local_cand.empty()) {
    return false;
  }

  auto span_mask = [&](int hr, RtlMachineMode mode) -> uint64_t {
    uint64_t m = 0;
    const int span = std::max(1, MipsHardRegNregs(hr, mode, gp64, fp64));
    for (int w = 0; w < span && hr + w < 64; ++w) {
      if (hr + w >= 0) {
        m |= (uint64_t{1} << (hr + w));
      }
    }
    return m;
  };
  auto is_valid_mode_reg = [&](int hr, RtlMachineMode mode, bool want_gpr, bool crosses_call) {
    if (want_gpr ? ((hr < 2 || hr > 25) && hr != kFramePointerRegno) : (hr < 32 || hr > 63)) {
      return false;
    }
    if (out->sideband.frame_pointer_needed && hr == kFramePointerRegno) {
      return false;
    }
    const int span = std::max(1, MipsHardRegNregs(hr, mode, gp64, fp64));
    // `config/mips/mips.c:3448` (`HARD_REGNO_MODE_OK`): on `-mfp32` (`!fp64`),
    // odd FP registers are never allocatable for any FP mode (`SFmode` or `DFmode`).
    if ((span > 1 || (!want_gpr && !fp64)) && (hr % 2) != 0) {
      return false;
    }
    if (want_gpr && hr == kFramePointerRegno && span > 1) {
      return false;
    }
    if (want_gpr && hr != kFramePointerRegno && hr + span > 26) {
      return false;
    }
    if (!want_gpr && hr + span > 64) {
      return false;
    }
    for (int w = 0; w < span; ++w) {
      if (crosses_call && IsCallUsedHardReg(hr + w)) {
        return false;
      }
    }
    return true;
  };
  auto has_dead_note = [](const RtlPatternAndNotes& pn, int regno) {
    return std::any_of(pn.reg_notes.begin(), pn.reg_notes.end(), [&](const RtlRegNote& n) {
      return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
             !n.datum.IsArgPointerReg() && n.datum.RegNo() == regno;
    });
  };
  auto has_dead_or_unused_note = [](const RtlPatternAndNotes& pn, int regno) {
    return std::any_of(pn.reg_notes.begin(), pn.reg_notes.end(), [&](const RtlRegNote& n) {
      return (n.kind == RtlMachineMode::kRegDead || n.kind == RtlMachineMode::kRegUnused) &&
             n.datum.op == RtlOp::kReg && !n.datum.IsArgPointerReg() && n.datum.RegNo() == regno;
    });
  };
  auto extract_single_set = [](const RtlExpr& pat) -> const RtlExpr* {
    if (pat.IsSet()) {
      return &pat;
    }
    if (pat.IsParallel()) {
      const RtlExpr* found = nullptr;
      for (const RtlExpr& ch : pat.children) {
        if (ch.IsSet()) {
          if (found != nullptr) {
            return nullptr;
          }
          found = &ch;
        }
      }
      return found;
    }
    return nullptr;
  };

  const absl::flat_hash_map<int, uint64_t> global_hr_conflicts =
      ComputeLocalResolutionGlobalHardConflicts(live_probe, live_blocks, out->sideband.reg_renumber,
                                                is_local_cand, gp64, fp64);
  const int spill_base =
      out->sideband.frame_pointer_needed ? kFramePointerRegno : kStackPointerRegno;
  const ReloadSpillSlotSummary spill_summary =
      SummarizeReloadSpillSlots(live_blocks, spill_base, reload_spill_offsets);
  absl::flat_hash_map<int, int> coalesce_into;
  absl::flat_hash_map<int, int> demote_to_hr;

  for (size_t b = 0; b < live_blocks.size(); ++b) {
    const auto& b_insns = live_blocks[b];
    const size_t m = b_insns.size();
    if (m == 0) {
      continue;
    }
    std::vector<int> b_locals;
    std::vector<int> b_multi_locals;
    for (const auto& [p, hr] : out->sideband.reg_renumber) {
      if (p < kFirstPseudoRegister || hr < 2 || hr >= 64 || !info.contains(p)) {
        continue;
      }
      if (is_local_cand.contains(p) && info.at(p).blocks.contains(b)) {
        b_locals.push_back(p);
      } else if (!is_local_cand.contains(p) && !live_at_any_start.contains(p) &&
                 info.at(p).blocks.size() == 1 && info.at(p).blocks.contains(b) &&
                 !info.at(p).used_before_set) {
        b_multi_locals.push_back(p);
      }
    }
    if (b_locals.empty()) {
      continue;
    }
    std::sort(b_locals.begin(), b_locals.end());
    std::sort(b_multi_locals.begin(), b_multi_locals.end());

    absl::flat_hash_map<int, size_t> p_birth;
    absl::flat_hash_map<int, size_t> p_death;
    for (int p : b_locals) {
      size_t bi = m;
      size_t di = 0;
      for (size_t i = 0; i < m; ++i) {
        if (b_insns[i].pattern.ContainsReg(p) || has_dead_or_unused_note(b_insns[i], p)) {
          bi = std::min(bi, i);
          di = std::max(di, i);
        }
      }
      if (bi < m) {
        p_birth[p] = bi;
        p_death[p] = di;
      }
    }
    absl::flat_hash_map<int, size_t> multi_first_i;
    absl::flat_hash_map<int, size_t> multi_last_i;
    for (int pm : b_multi_locals) {
      size_t bi = m;
      size_t di = 0;
      for (size_t i = 0; i < m; ++i) {
        if (b_insns[i].pattern.ContainsReg(pm) || has_dead_or_unused_note(b_insns[i], pm)) {
          bi = std::min(bi, i);
          di = i;
        }
      }
      if (bi < m) {
        multi_first_i[pm] = bi;
        multi_last_i[pm] = di;
      }
    }

    // Track `regs_live_at` (`2 * m` half-ticks: `2*i` = at birth before non-earlyclobber
    // deaths, `2*i + 1` = after deaths/births of insn `i`) strictly for explicit hard
    // registers (`0 <= regno < 64`). Global pseudos have `reg_renumber == -1` during
    // `local-alloc.c:block_alloc` and occupy zero bits in `regs_live_at`.
    const std::vector<uint64_t> base_live_ticks =
        ComputeLocalResolutionHardLiveTicks(live_probe, b, b_insns, gp64, fp64);

    const std::vector<uint64_t> all_live_ticks = ComputeLocalResolutionAllLiveTicks(
        live_probe, b, b_insns, info, out->sideband.reg_renumber, live_at_any_start, spill_base,
        reload_spill_offsets, spill_summary, b_locals, p_birth, p_death, b_multi_locals,
        multi_first_i, multi_last_i, base_live_ticks, gp64, fp64);

    absl::flat_hash_map<int, int> uf_parent;
    for (int p : b_locals) {
      uf_parent[p] = p;
    }
    auto find_q = [&](int p) {
      int r = p;
      while (uf_parent[r] != r) {
        r = uf_parent[r];
      }
      int c = p;
      while (c != r) {
        const int nxt = uf_parent[c];
        uf_parent[c] = r;
        c = nxt;
      }
      return r;
    };

    absl::flat_hash_set<int> false_pseudo_combine;
    absl::flat_hash_map<int, int> false_combine_source_of;
    auto collect_direct_nonmem_regs = [](const RtlExpr& expr, std::vector<RtlExpr>* out_regs) {
      auto walk = [&](const auto& self, const RtlExpr& e) -> void {
        if (e.op == RtlOp::kMem) {
          return;
        }
        if (e.op == RtlOp::kReg && !e.IsArgPointerReg()) {
          out_regs->push_back(e);
          return;
        }
        if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg &&
            !e.children[0].IsArgPointerReg()) {
          out_regs->push_back(e.children[0]);
          return;
        }
        for (const RtlExpr& ch : e.children) {
          self(self, ch);
        }
      };
      walk(walk, expr);
    };

    auto modes_tieable = [](RtlMachineMode m1, RtlMachineMode m2) {
      const bool f1 = (m1 == RtlMachineMode::kSF || m1 == RtlMachineMode::kDF);
      const bool f2 = (m2 == RtlMachineMode::kSF || m2 == RtlMachineMode::kDF);
      return f1 == f2;
    };

    absl::flat_hash_set<int> md_promoted_gprs;
    for (size_t i = 0; i < m; ++i) {
      const RtlExpr& pat = b_insns[i].pattern;
      if (pat.IsSet() && pat.SetDest().IsHardReg() && !pat.SetDest().IsArgPointerReg() &&
          pat.SetSrc().IsPseudoReg()) {
        const auto rit = out->sideband.reg_renumber.find(pat.SetSrc().RegNo());
        if (rit != out->sideband.reg_renumber.end() && (rit->second == 64 || rit->second == 65)) {
          md_promoted_gprs.insert(pat.SetDest().RegNo());
        }
      }
    }

    for (size_t i = 0; i < m; ++i) {
      const RtlExpr* st = extract_single_set(b_insns[i].pattern);
      if (st == nullptr) {
        continue;
      }
      const RtlExpr* dst = &st->SetDest();
      if (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (!dst->IsPseudoReg() || !uf_parent.contains(dst->RegNo())) {
        continue;
      }
      const int pd = dst->RegNo();
      if (p_birth.at(pd) != i) {
        continue;
      }
      std::vector<RtlExpr> rhs_regs;
      collect_direct_nonmem_regs(st->SetSrc(), &rhs_regs);
      for (const RtlExpr& sr : rhs_regs) {
        if (st->SetSrc().op != RtlOp::kReg && st->SetSrc().op != RtlOp::kMem &&
            IsCallCrossingReloadSpillFalseCombine(*out, live_probe, all_parsed_blocks, b_insns,
                                                  all_live_ticks, p_death, reload_spill_offsets, b,
                                                  i, pd, sr, gp64, fp64)) {
          false_pseudo_combine.insert(pd);
          break;
        }
        if (sr.IsHardReg() && !sr.IsArgPointerReg() && sr.RegNo() >= 0 && sr.RegNo() < 64) {
          if (b < demoted_reload_scratch_masks.size() &&
              i < demoted_reload_scratch_masks[b].size() &&
              (demoted_reload_scratch_masks[b][i] & (uint64_t{1} << sr.RegNo())) != 0) {
            const int ps = FindDyingPseudoBehindDemotedInputReload(b_insns, i, sr.RegNo(), sr.mode);
            if (ps >= 0 && ps != pd && uf_parent.contains(ps) &&
                modes_tieable(info.at(pd).mode, info.at(ps).mode)) {
              const int hs = out->sideband.reg_renumber.at(ps);
              const int hd = out->sideband.reg_renumber.at(pd);
              if (hs != hd &&
                  !IsUnallocatableCallCrossingGprCombine(live_probe, b_insns, all_live_ticks,
                                                         p_birth, p_death, ps, pd, hs, hd,
                                                         info.at(pd).mode, gp64, fp64) &&
                  !IsUnallocatableCallCrossingFpGprCombine(live_probe, b_insns, all_live_ticks,
                                                           p_birth, p_death, ps, pd, hs, hd,
                                                           info.at(pd).mode, gp64, fp64)) {
                false_pseudo_combine.insert(pd);
                false_combine_source_of[pd] = ps;
              }
              break;
            }
          }
          if (i > 0 && st->SetSrc().op == RtlOp::kReg && sr.RegNo() >= 32 && sr.RegNo() < 64 &&
              b_insns[i - 1].pattern.IsSet() && b_insns[i - 1].pattern.SetDest().IsHardReg() &&
              b_insns[i - 1].pattern.SetDest().RegNo() == sr.RegNo() &&
              b_insns[i - 1].pattern.SetSrc().op != RtlOp::kReg &&
              b_insns[i - 1].pattern.SetSrc().op != RtlOp::kMem) {
            std::vector<RtlExpr> prev_rhs;
            collect_direct_nonmem_regs(b_insns[i - 1].pattern.SetSrc(), &prev_rhs);
            bool folded_fpr_false_tie = false;
            for (const RtlExpr& pr : prev_rhs) {
              if (pr.IsHardReg() && !pr.IsArgPointerReg() && pr.RegNo() >= 0 && pr.RegNo() < 64 &&
                  modes_tieable(pr.mode, info.at(pd).mode)) {
                break;
              }
              if (pr.IsPseudoReg() && uf_parent.contains(pr.RegNo()) &&
                  has_dead_note(b_insns[i - 1], pr.RegNo()) && p_death.at(pr.RegNo()) == i - 1 &&
                  modes_tieable(info.at(pd).mode, info.at(pr.RegNo()).mode)) {
                const int pr_hr = out->sideband.reg_renumber.at(pr.RegNo());
                const int pd_hr = out->sideband.reg_renumber.at(pd);
                if (pr_hr != pd_hr) {
                  // `local-alloc.c:1525-1538, 1855-1866, 1928-1938`: when a
                  // `REG_EQUIV` FP pseudo `pr` (`reg_alternate_class == NO_REGS`)
                  // combines with a call-crossing FP pseudo `pd` allocated to a
                  // callee-saved GPR (`pd_hr < 32`) because all callee-saved
                  // FPRs (`$f20..$f30`) are occupied over `[birth(pr), death(pd)]`,
                  // `local-alloc` leaves the combined qty unallocated (`-1`) so
                  // `global-alloc` allocates `pr` and `pd` separately.
                  bool unalloc_fp_qty_falls_to_global = false;
                  const auto cc_it = live_probe.sideband.reg_n_calls_crossed.find(pd);
                  const bool pd_crosses_call =
                      (cc_it != live_probe.sideband.reg_n_calls_crossed.end() && cc_it->second > 0);
                  if (pr_hr >= 32 && pr_hr < 64 && pd_hr >= 2 && pd_hr < 32 && pd_crosses_call &&
                      info.at(pr.RegNo()).has_reg_equiv) {
                    const size_t q_birth = std::min(p_birth.at(pr.RegNo()), p_birth.at(pd));
                    const size_t q_death = std::max(p_death.at(pr.RegNo()), p_death.at(pd));
                    const size_t t_start = 2 * q_birth + 1;
                    const size_t t_end = std::max(t_start, 2 * q_death);
                    bool any_saved_fpr_free = false;
                    const int fp_step = fp64 ? 1 : 2;
                    for (int fr = 52; fr < 64; fr += fp_step) {
                      const uint64_t fm = span_mask(fr, info.at(pd).mode);
                      bool free_here = true;
                      for (size_t t = t_start; t <= t_end && t < all_live_ticks.size(); ++t) {
                        if ((all_live_ticks[t] & fm) != 0) {
                          free_here = false;
                          break;
                        }
                      }
                      if (free_here) {
                        any_saved_fpr_free = true;
                        break;
                      }
                    }
                    unalloc_fp_qty_falls_to_global = !any_saved_fpr_free;
                  }
                  if (!unalloc_fp_qty_falls_to_global) {
                    folded_fpr_false_tie = true;
                  }
                }
                break;
              }
            }
            if (folded_fpr_false_tie) {
              false_pseudo_combine.insert(pd);
              break;
            }
          }
          if (!md_promoted_gprs.contains(sr.RegNo()) && modes_tieable(sr.mode, info.at(pd).mode)) {
            break;
          }
          continue;
        }
        if (!sr.IsPseudoReg() || !uf_parent.contains(sr.RegNo())) {
          continue;
        }
        const int ps = sr.RegNo();
        if (ps == pd || !has_dead_note(b_insns[i], ps) || p_death.at(ps) != i) {
          continue;
        }
        if (!modes_tieable(info.at(pd).mode, info.at(ps).mode)) {
          continue;
        }
        const int hd = out->sideband.reg_renumber.at(pd);
        const int hs = out->sideband.reg_renumber.at(ps);
        // Mirror `local-alloc.c:1750-1884` (`combine_regs`): two local pseudos
        // with tieable modes (`QI`/`HI`/`SI` or `SF`/`DF`) combine into one
        // quantity whenever they share the same target hard register (`hd == hs`),
        // widening `qty_mode` to the larger mode (`local-alloc.c:1882`).
        if (hd == hs) {
          const int rq = find_q(ps);
          const int dq = find_q(pd);
          if (rq != dq) {
            uf_parent[dq] = rq;
          }
        } else if (!IsUnallocatableCallCrossingGprCombine(live_probe, b_insns, all_live_ticks,
                                                          p_birth, p_death, ps, pd, hs, hd,
                                                          info.at(pd).mode, gp64, fp64) &&
                   !IsUnallocatableCallCrossingFpGprCombine(live_probe, b_insns, all_live_ticks,
                                                            p_birth, p_death, ps, pd, hs, hd,
                                                            info.at(pd).mode, gp64, fp64)) {
          false_pseudo_combine.insert(pd);
          false_combine_source_of[pd] = ps;
        }
        break;
      }
    }
    UnionNoConflictLibcallQuantities(b_insns, info, out->sideband.reg_renumber, &uf_parent);

    const int num_scratch_clobbers_in_b = CountLocalScratchClobbersInBlock(b_insns);

    absl::flat_hash_map<int, LocalQtyGroup> groups;
    for (int p : b_locals) {
      if (!p_birth.contains(p)) {
        continue;
      }
      const int root = find_q(p);
      LocalQtyGroup& g = groups[root];
      if (g.members.empty()) {
        g.target_hr = out->sideband.reg_renumber.at(p);
        g.mode = info.at(p).mode;
        g.birth_i = p_birth.at(p);
        g.death_i = p_death.at(p);
      } else {
        if (MipsModeByteSize(info.at(p).mode) > MipsModeByteSize(g.mode)) {
          g.mode = info.at(p).mode;
        }
        g.birth_i = std::min(g.birth_i, p_birth.at(p));
        g.death_i = std::max(g.death_i, p_death.at(p));
      }
      g.members.push_back(p);
      const auto cc = live_probe.sideband.reg_n_calls_crossed.find(p);
      if (cc != live_probe.sideband.reg_n_calls_crossed.end() && cc->second > 0) {
        g.crosses_call = true;
      }
      if (false_pseudo_combine.contains(p)) {
        g.needs_resolution = true;
      }
    }
    for (auto& [_, g] : groups) {
      std::sort(g.members.begin(), g.members.end(), [&](int a, int b_m) {
        if (p_birth.at(a) != p_birth.at(b_m)) {
          return p_birth.at(a) < p_birth.at(b_m);
        }
        if (p_death.at(a) != p_death.at(b_m)) {
          return p_death.at(a) < p_death.at(b_m);
        }
        return a < b_m;
      });
    }

    auto base_free_on_interval = [&](int hr, RtlMachineMode mode, size_t bi, size_t di) {
      const uint64_t hm = span_mask(hr, mode);
      const size_t t_start = 2 * bi + 1;
      const size_t t_end = std::max(t_start, 2 * di);
      for (size_t t = t_start; t <= t_end && t < base_live_ticks.size(); ++t) {
        if ((base_live_ticks[t] & hm) != 0) {
          return false;
        }
      }
      return true;
    };

    std::vector<int> group_roots;
    group_roots.reserve(groups.size());
    for (const auto& [root, _] : groups) {
      group_roots.push_back(root);
    }
    std::sort(group_roots.begin(), group_roots.end(), [&](int a, int b_r) {
      const auto& ga = groups.at(a);
      const auto& gb = groups.at(b_r);
      return (ga.birth_i != gb.birth_i) ? (ga.birth_i < gb.birth_i) : (a < b_r);
    });

    const std::vector<uint64_t> empty_scratch_masks;
    const std::vector<uint64_t>& b_reload_scratch_masks = (b < demoted_reload_scratch_masks.size())
                                                              ? demoted_reload_scratch_masks[b]
                                                              : empty_scratch_masks;
    DetectLocalResolutionGroupConflicts(
        *out, all_parsed_blocks[b], b_insns, b_reload_scratch_masks, info, p_birth, p_death,
        b_multi_locals, multi_first_i, multi_last_i, base_live_ticks, all_live_ticks,
        md_promoted_gprs, num_scratch_clobbers_in_b, gp64, fp64, group_roots, &groups);

    for (int root : group_roots) {
      LocalQtyGroup& g = groups.at(root);
      const bool want_gpr = (g.target_hr < 32);
      if (!g.needs_resolution || HasOverlappingLowerOrderPendingResolution(
                                     root, g, group_roots, groups, false_pseudo_combine)) {
        continue;
      }

      // Prefer coalescing a single-pseudo group with an earlier disjoint single-block
      // pseudo of identical `(target_hr, mode, raw_mode, flags)` in `b` so it promotes
      // cleanly to `global_alloc` (`reg_n_deaths >= 2`). When reload spill slots exist,
      // exclude pseudos that load from or store to a reload spill slot so `InvertReload`
      // sees intact single-def/single-use spill reloads (`reload1.c:2309-2400`).
      const int spill_base_r = out->sideband.frame_pointer_needed ? kFramePointerRegno : 29;
      bool coalesced = false;
      if (g.members.size() == 1 && !g.stolen_by_sugg_thief &&
          (reload_spill_offsets.empty() || !false_pseudo_combine.contains(g.members[0]) ||
           WouldDemoteCreateSelfSetRegDead(g.members[0], span_mask(g.target_hr, g.mode), b_insns,
                                           g.birth_i, g.death_i, gp64, fp64)) &&
          !PseudoTouchesReloadSpillSlot(b_insns, g.members[0], g.birth_i, g.death_i, spill_base_r,
                                        reload_spill_offsets)) {
        const int p_cur = g.members[0];
        const LocalInfo& cur_li = info.at(p_cur);
        const int g_ord = LookupMipsAllocOrderIndex(g.target_hr);
        auto exposes_on_interval = [&](size_t bi, size_t di, int skip_root) {
          for (int other_root : group_roots) {
            if (other_root == root || other_root == skip_root) {
              continue;
            }
            const LocalQtyGroup& other_g = groups.at(other_root);
            if (other_g.target_hr == g.target_hr || (other_g.target_hr < 32) != want_gpr ||
                LookupMipsAllocOrderIndex(other_g.target_hr) <= g_ord || other_g.death_i < bi ||
                di < other_g.birth_i) {
              continue;
            }
            if (is_valid_mode_reg(g.target_hr, other_g.mode, want_gpr, other_g.crosses_call) &&
                base_free_on_interval(g.target_hr, other_g.mode, other_g.birth_i,
                                      other_g.death_i)) {
              return true;
            }
          }
          return false;
        };
        if (cur_li.uniform_shape && !cur_li.has_reg_equiv &&
            !exposes_on_interval(g.birth_i, g.death_i, -1)) {
          for (int pm : b_multi_locals) {
            if (out->sideband.reg_renumber.at(pm) != g.target_hr || !multi_last_i.contains(pm) ||
                multi_last_i.at(pm) >= g.birth_i) {
              continue;
            }
            const LocalInfo& pm_li = info.at(pm);
            if (!pm_li.uniform_shape || pm_li.has_reg_equiv || pm_li.mode != cur_li.mode ||
                pm_li.raw_mode != cur_li.raw_mode || pm_li.flags != cur_li.flags ||
                !base_free_on_interval(g.target_hr, g.mode, multi_last_i.at(pm), g.birth_i) ||
                (multi_first_i.contains(pm) &&
                 PseudoTouchesReloadSpillSlot(b_insns, pm, multi_first_i.at(pm),
                                              multi_last_i.at(pm), spill_base_r,
                                              reload_spill_offsets))) {
              continue;
            }
            coalesce_into[p_cur] = pm;
            multi_last_i[pm] = g.death_i;
            coalesced = true;
            break;
          }
          for (int prev_root : group_roots) {
            if (coalesced) {
              break;
            }
            if (prev_root == root) {
              continue;
            }
            const LocalQtyGroup& prev_g = groups.at(prev_root);
            if (prev_g.members.size() != 1 || prev_g.target_hr != g.target_hr ||
                prev_g.death_i >= g.birth_i) {
              continue;
            }
            const int p_prev = prev_g.members[0];
            if (demote_to_hr.contains(p_prev) ||
                PseudoTouchesReloadSpillSlot(b_insns, p_prev, prev_g.birth_i, prev_g.death_i,
                                             spill_base_r, reload_spill_offsets)) {
              continue;
            }
            const LocalInfo& prev_li = info.at(p_prev);
            if (!prev_li.uniform_shape || prev_li.has_reg_equiv || prev_li.mode != cur_li.mode ||
                prev_li.raw_mode != cur_li.raw_mode || prev_li.flags != cur_li.flags) {
              continue;
            }
            if (!base_free_on_interval(g.target_hr, g.mode, prev_g.death_i, g.birth_i) ||
                exposes_on_interval(prev_g.birth_i, prev_g.death_i, prev_root)) {
              continue;
            }
            // Mirrors `local-alloc.c:472` + `global.c:1060-1210`: coalescing two
            // single-death local pseudos promotes `p_prev` from `local_alloc`
            // (which runs before `global_alloc` and reserves `g.target_hr`
            // against live-through globals) into `global_alloc`, where any
            // live-through global pseudo with a higher `REG_ALLOC_ORDER` slot
            // could steal `g.target_hr`.
            bool exposes_live_through_global = false;
            if (const auto live_it =
                    live_probe.sideband.basic_block_live_at_start.find(static_cast<int>(b));
                live_it != live_probe.sideband.basic_block_live_at_start.end()) {
              for (int p_glob : live_it->second) {
                if (p_glob < 72 || !out->sideband.reg_renumber.contains(p_glob) ||
                    (b == 0 && (!info.contains(p_glob) || info.at(p_glob).blocks.size() <= 1))) {
                  continue;
                }
                const int hr_glob = out->sideband.reg_renumber.at(p_glob);
                if (hr_glob < 0 || hr_glob >= 64 || hr_glob == g.target_hr ||
                    (hr_glob < 32) != want_gpr || LookupMipsAllocOrderIndex(hr_glob) <= g_ord) {
                  continue;
                }
                const auto cc_it = live_probe.sideband.reg_n_calls_crossed.find(p_glob);
                const bool glob_crosses_call =
                    (cc_it != live_probe.sideband.reg_n_calls_crossed.end() && cc_it->second > 0);
                const RtlMachineMode glob_mode =
                    info.contains(p_glob) ? info.at(p_glob).mode : RtlMachineMode::kSi;
                if (is_valid_mode_reg(g.target_hr, glob_mode, want_gpr, glob_crosses_call)) {
                  exposes_live_through_global = true;
                  break;
                }
              }
            }
            if (exposes_live_through_global) {
              continue;
            }
            int target_canon = p_prev;
            for (size_t hops = 0;
                 hops <= coalesce_into.size() && coalesce_into.contains(target_canon); ++hops) {
              target_canon = coalesce_into.at(target_canon);
            }
            if (target_canon != p_cur) {
              coalesce_into[p_cur] = target_canon;
              coalesced = true;
              break;
            }
          }
        }
      }
      if (!coalesced) {
        if (g.target_hr == kFramePointerRegno) {
          if (out->sideband.frame_pointer_needed) {
            continue;
          }
          if (g.members.size() == 1 && info.at(g.members[0]).uniform_shape &&
              !info.at(g.members[0]).has_reg_equiv) {
            const int p_cur = g.members[0];
            const LocalInfo& cur_li = info.at(p_cur);
            auto resolve_canon = [&](int regno) {
              int cur = regno;
              for (size_t hops = 0; hops <= coalesce_into.size() && coalesce_into.contains(cur);
                   ++hops) {
                cur = coalesce_into.at(cur);
              }
              return cur;
            };
            absl::flat_hash_set<size_t> cur_web_blocks = cur_li.blocks;
            for (const auto& [src_p, _] : coalesce_into) {
              if (resolve_canon(src_p) == p_cur && info.contains(src_p)) {
                cur_web_blocks.insert(info.at(src_p).blocks.begin(), info.at(src_p).blocks.end());
              }
            }
            std::vector<int> fp_candidates;
            for (const auto& [other_p, other_hr] : out->sideband.reg_renumber) {
              if (other_p == p_cur || other_hr != kFramePointerRegno) {
                continue;
              }
              const int target_canon = resolve_canon(other_p);
              if (target_canon == p_cur || demote_to_hr.contains(target_canon) ||
                  !info.contains(target_canon)) {
                continue;
              }
              fp_candidates.push_back(target_canon);
            }
            std::sort(fp_candidates.begin(), fp_candidates.end());
            fp_candidates.erase(std::unique(fp_candidates.begin(), fp_candidates.end()),
                                fp_candidates.end());
            for (int target_canon : fp_candidates) {
              const LocalInfo& other_li = info.at(target_canon);
              if (!other_li.uniform_shape || other_li.has_reg_equiv ||
                  other_li.mode != cur_li.mode || other_li.raw_mode != cur_li.raw_mode ||
                  other_li.flags != cur_li.flags) {
                continue;
              }
              bool shares_block = false;
              for (size_t tb : other_li.blocks) {
                if (cur_web_blocks.contains(tb)) {
                  shares_block = true;
                  break;
                }
              }
              for (const auto& [src_p, _] : coalesce_into) {
                if (!shares_block && resolve_canon(src_p) == target_canon && info.contains(src_p)) {
                  for (size_t tb : info.at(src_p).blocks) {
                    if (cur_web_blocks.contains(tb)) {
                      shares_block = true;
                      break;
                    }
                  }
                }
              }
              if (!shares_block) {
                coalesce_into[p_cur] = target_canon;
                coalesced = true;
                break;
              }
            }
          }
          continue;
        }
        const bool runs_sched1 = ParseCompilationProfileFlags(profile).RunsSched1();
        bool block_sched1_achievable_before = false;
        if (runs_sched1) {
          const std::array<size_t, 1> check_b = {b};
          const absl::flat_hash_map<int, int> empty_demote;
          block_sched1_achievable_before = ArePreAllocBlocksSched1Achievable(
              *out, check_b, all_parsed_blocks, empty_demote, profile);
        }
        ResolveLocalQtyGroupDemotion(b, root, g, group_roots, groups, false_combine_source_of,
                                     false_pseudo_combine, is_local_cand, global_hr_conflicts,
                                     p_birth, p_death, info, gp64, fp64, runs_sched1,
                                     block_sched1_achievable_before, profile, b_insns,
                                     &demote_to_hr, out, all_parsed_blocks);
      }
    }
  }

  return ApplyLocalResolutionRewrites(coalesce_into, demote_to_hr, gp64, fp64, &all_parsed_blocks,
                                      out);
}

void ResolveLocalFalseCombineAndSuggPseudos(
    const CompilationProfile& profile, const ParsedCompilerFlags& parsed_flags, bool gp64,
    bool fp64, const absl::flat_hash_set<int64_t>& reload_spill_offsets,
    const std::vector<std::vector<uint64_t>>& demoted_reload_scratch_masks,
    RtlControlFlowGraph* out, std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr) {
  if (parsed_flags.opt_level == 0) {
    return;
  }
  for (int iter = 0; iter < 8; ++iter) {
    if (!RunSingleLocalResolutionIteration(profile, gp64, fp64, reload_spill_offsets,
                                           demoted_reload_scratch_masks, out,
                                           all_parsed_blocks_ptr)) {
      break;
    }
  }
}

// Per `local-alloc.c:470-478` (`local_alloc`), `local-alloc.c:1369-1392`
// (`block_alloc`), `local-alloc.c:1954-2058` (`reg_is_set`/`reg_is_born`/
// `wipe_dead_reg`), and `local-alloc.c:2092-2093` (`find_free_reg`), every
// single-block local pseudo allocated a quantity (`reg_basic_block[p] >= 0`,
// `reg_n_deaths[p] == 1`) must finish `block_alloc` with
// `0 <= qty_birth[q] <= qty_death[q]`. Because MIPS does not define
// `PRESERVE_DEATH_INFO_REGNO_P` (`reload1.c:1994`), `reload()` never strips
// `REG_DEAD` notes from pseudos after `alter_reg`; hence any single-block
// pseudo lacking a `REG_DEAD`/`REG_UNUSED` note after its final `SET`/`CLOBBER`
// in its block either belongs to the same RMW pseudo written at its final read
// (same `reg_renumber`, mode, and flags) or must be demoted back to its hard
// register.
void ResolveDeathlessLocalPseudos(
    bool gp64, bool fp64, RtlControlFlowGraph* out,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr) {
  auto& all_parsed_blocks = *all_parsed_blocks_ptr;
  auto collect_set_dests = [](const RtlExpr& pat,
                              std::vector<std::pair<const RtlExpr*, bool>>* dests) {
    auto add_elem = [&](const RtlExpr& elem) {
      if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
        return;
      }
      const RtlExpr* d = &elem.children[0];
      while ((d->op == RtlOp::kSubreg || d->op == RtlOp::kStrictLowPart ||
              d->op == RtlOp::kZeroExtract || d->op == RtlOp::kSignExtract) &&
             !d->children.empty()) {
        d = &d->children[0];
      }
      if (d->IsPseudoReg()) {
        dests->emplace_back(d, elem.op == RtlOp::kClobber);
      }
    };
    if (pat.IsParallel()) {
      for (const RtlExpr& ch : pat.children) {
        add_elem(ch);
      }
    } else {
      add_elem(pat);
    }
  };

  bool cfg_has_any_death_notes = false;
  for (const auto& blk : all_parsed_blocks) {
    for (const RtlPatternAndNotes& pn : blk) {
      for (const RtlRegNote& note : pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) {
          cfg_has_any_death_notes = true;
          break;
        }
      }
      if (cfg_has_any_death_notes) {
        break;
      }
    }
    if (cfg_has_any_death_notes) {
      break;
    }
  }
  if (!cfg_has_any_death_notes) {
    return;
  }

  for (int iter = 0; iter < 8; ++iter) {
    absl::flat_hash_map<int, absl::flat_hash_set<size_t>> pseudo_blocks;
    absl::flat_hash_map<int, const RtlExpr*> pseudo_sample_expr;
    absl::flat_hash_set<int> equiv_pseudos;
    for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
      for (const RtlPatternAndNotes& pn : all_parsed_blocks[b]) {
        pn.pattern.Visit([&](const RtlExpr& n) {
          if (n.IsPseudoReg()) {
            pseudo_blocks[n.RegNo()].insert(b);
            pseudo_sample_expr.try_emplace(n.RegNo(), &n);
          }
        });
        for (const RtlRegNote& note : pn.reg_notes) {
          if (note.kind == RtlMachineMode::kRegEquiv) {
            std::vector<std::pair<const RtlExpr*, bool>> dests;
            collect_set_dests(pn.pattern, &dests);
            for (const auto& [d, _] : dests) {
              equiv_pseudos.insert(d->RegNo());
            }
          }
          if (note.list_op == RtlOp::kExprList) {
            note.datum.Visit([&](const RtlExpr& n) {
              if (n.IsPseudoReg()) {
                pseudo_blocks[n.RegNo()].insert(b);
              }
            });
          }
        }
        for (const RtlExpr& fu : pn.call_fusage) {
          fu.Visit([&](const RtlExpr& n) {
            if (n.IsPseudoReg()) {
              pseudo_blocks[n.RegNo()].insert(b);
            }
          });
        }
      }
    }

    absl::flat_hash_map<int, int> coalesce_into;
    absl::flat_hash_map<int, int> demote_to_hr;

    for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
      const auto& blk = all_parsed_blocks[b];
      absl::flat_hash_map<int, int> birth;
      absl::flat_hash_map<int, int> death;
      absl::flat_hash_map<int, size_t> first_mention;
      absl::flat_hash_map<int, size_t> last_def;
      absl::flat_hash_map<int, size_t> last_read;

      int insn_num = 0;
      for (size_t i = 0; i < blk.size(); ++i) {
        const RtlPatternAndNotes& pn = blk[i];
        if (pn.pattern.op == RtlOp::kNote) {
          continue;
        }
        ++insn_num;

        for (const RtlRegNote& n : pn.reg_notes) {
          if (n.kind == RtlMachineMode::kRegDead && n.datum.IsPseudoReg()) {
            death[n.datum.RegNo()] = 2 * insn_num;
          }
        }

        std::vector<std::pair<const RtlExpr*, bool>> dests;
        collect_set_dests(pn.pattern, &dests);
        for (const auto& [d, is_clob] : dests) {
          const int p = d->RegNo();
          first_mention.try_emplace(p, i);
          if (!birth.contains(p)) {
            birth[p] = 2 * insn_num - (is_clob ? 1 : 0);
          }
          last_def[p] = i;
          death[p] = -1;
        }

        auto record_reads = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
          if (e.op == RtlOp::kSet && e.children.size() == 2) {
            self(self, e.children[0], true);
            self(self, e.children[1], false);
            return;
          }
          if (e.op == RtlOp::kClobber) {
            return;
          }
          if (in_dst && e.op == RtlOp::kMem) {
            for (const RtlExpr& ch : e.children) {
              self(self, ch, false);
            }
            return;
          }
          if (in_dst && (e.op == RtlOp::kZeroExtract || e.op == RtlOp::kSignExtract) &&
              e.children.size() == 3) {
            self(self, e.children[1], false);
            self(self, e.children[2], false);
            return;
          }
          if (!in_dst && e.IsPseudoReg()) {
            first_mention.try_emplace(e.RegNo(), i);
            last_read[e.RegNo()] = i;
            return;
          }
          for (const RtlExpr& ch : e.children) {
            self(self, ch, in_dst);
          }
        };
        record_reads(record_reads, pn.pattern, false);

        for (const RtlRegNote& n : pn.reg_notes) {
          if (n.kind == RtlMachineMode::kRegUnused && n.datum.IsPseudoReg()) {
            death[n.datum.RegNo()] = 2 * insn_num + 1;
          }
        }
      }

      std::vector<int> deathless_locals;
      for (const auto& [p, bidx] : birth) {
        const auto pbit = pseudo_blocks.find(p);
        if (pbit == pseudo_blocks.end() || pbit->second.size() != 1) {
          continue;
        }
        const auto rit = out->sideband.reg_renumber.find(p);
        if (rit == out->sideband.reg_renumber.end() || rit->second < 0) {
          continue;
        }
        const auto dit = death.find(p);
        if (dit == death.end() || dit->second < bidx) {
          deathless_locals.push_back(p);
        }
      }
      std::sort(deathless_locals.begin(), deathless_locals.end());

      for (int p : deathless_locals) {
        if (coalesce_into.contains(p) || demote_to_hr.contains(p)) {
          continue;
        }
        const int hr = out->sideband.reg_renumber.at(p);
        int coal_partner = -1;
        const auto lrit = last_read.find(p);
        const auto spit = pseudo_sample_expr.find(p);
        if (lrit != last_read.end() && spit != pseudo_sample_expr.end() &&
            !equiv_pseudos.contains(p)) {
          const size_t ri = lrit->second;
          if (!ContainsCallOp(blk[ri].pattern)) {
            std::vector<std::pair<const RtlExpr*, bool>> dests;
            collect_set_dests(blk[ri].pattern, &dests);
            for (const auto& [d, is_clob] : dests) {
              if (is_clob) {
                continue;
              }
              const int p2 = d->RegNo();
              if (p2 == p || equiv_pseudos.contains(p2) || coalesce_into.contains(p2) ||
                  demote_to_hr.contains(p2)) {
                continue;
              }
              const auto r2it = out->sideband.reg_renumber.find(p2);
              const auto pb2it = pseudo_blocks.find(p2);
              const auto fm2it = first_mention.find(p2);
              if (r2it == out->sideband.reg_renumber.end() || r2it->second != hr ||
                  pb2it == pseudo_blocks.end() || pb2it->second.size() != 1 ||
                  fm2it == first_mention.end() || fm2it->second < ri) {
                continue;
              }
              if (d->mode == spit->second->mode && d->raw_mode == spit->second->raw_mode &&
                  d->flags == spit->second->flags) {
                coal_partner = p2;
                break;
              }
            }
          }
        }
        if (coal_partner >= 0) {
          const int keep = std::min(p, coal_partner);
          const int drop = std::max(p, coal_partner);
          coalesce_into[drop] = keep;
        } else {
          // Preserve plain return copies `(set (reg/i:M hr) (reg:M p))` (whose
          // `REG_DEAD p` is stripped by `jump2` after `alter_reg` turns them
          // into self-moves and restored by `ReconstructRtlLiveness`) and
          // multi-use `FP_REGS` pseudos copied into outgoing O32 GPR call-arg
          // registers (`cse.c:481-486`); demote all other deathless local
          // pseudos so `local-alloc.c:2092-2093` never aborts.
          bool must_demote = true;
          if (lrit != last_read.end() && last_def.contains(p) && lrit->second > last_def.at(p)) {
            const RtlExpr& rpat = blk[lrit->second].pattern;
            const bool is_plain_ret_copy =
                rpat.IsSet() && rpat.children.size() == 2 && rpat.SetDest().IsHardReg() &&
                rpat.SetDest().flags.integrated && rpat.SetDest().RegNo() == hr &&
                rpat.SetSrc().IsPseudoReg() && rpat.SetSrc().RegNo() == p;
            // `calls.c:emit_call_1` + `flow.c:2504-2515`: an indirect call's
            // callee-address pseudo `(call (mem <addr reading p>) ...)` has no
            // post-`final` `REG_DEAD` note on the `CALL_INSN` in Stage 0, but
            // `ReconstructRtlLiveness(..., .include_call_reg_notes = true)`
            // restores `(REG_DEAD p)` for `local-alloc` / `sched1`.
            bool is_indirect_call_target = false;
            if (ContainsCallOp(rpat) && hr >= 2 && hr < 32) {
              auto check_call_addr = [&](auto&& self, const RtlExpr& node) -> void {
                if (is_indirect_call_target) {
                  return;
                }
                if (node.op == RtlOp::kCall && !node.children.empty()) {
                  if (node.children[0].ContainsReg(p)) {
                    is_indirect_call_target = true;
                  }
                  return;
                }
                for (const RtlExpr& ch : node.children) {
                  self(self, ch);
                }
              };
              check_call_addr(check_call_addr, rpat);
            }
            if (is_plain_ret_copy || is_indirect_call_target) {
              must_demote = false;
            } else if (hr >= 32 && hr < 64 && !ContainsCallOp(rpat)) {
              for (size_t k = last_def.at(p) + 1; k < lrit->second; ++k) {
                const RtlExpr& kpat = blk[k].pattern;
                if (kpat.IsSet() && kpat.children.size() == 2 && kpat.SetDest().IsHardReg() &&
                    kpat.SetDest().RegNo() >= 4 && kpat.SetDest().RegNo() <= 7 &&
                    kpat.SetSrc().IsPseudoReg() && kpat.SetSrc().RegNo() == p) {
                  must_demote = false;
                  break;
                }
              }
            }
          }
          if (must_demote) {
            demote_to_hr[p] = hr;
          }
        }
      }
    }

    if (!ApplyLocalResolutionRewrites(coalesce_into, demote_to_hr, gp64, fp64,
                                      all_parsed_blocks_ptr, out)) {
      break;
    }
  }
}

void PostProcessPreAlterCfg(const CompilationProfile& profile,
                            const ParsedCompilerFlags& compiler_flags, bool gp64, bool fp64,
                            const absl::flat_hash_set<int64_t>& reload_spill_offsets,
                            int first_pseudo_regno, int input_max_regno,
                            const std::vector<std::vector<int>>& cs_save_span,
                            std::vector<std::vector<RtlPatternAndNotes>>* parsed_blocks,
                            RtlControlFlowGraph* out) {
  const std::vector<std::vector<uint64_t>> demoted_reload_scratch_masks =
      DemoteReloadAndCallerSaveHardScratches(compiler_flags, gp64, fp64, cs_save_span,
                                             parsed_blocks, out);
  CoalesceSameRegRenumberRmwPseudos(profile, compiler_flags, gp64, fp64, out, parsed_blocks);
  if (compiler_flags.opt_level == 0) {
    CoalesceO0ExhaustedCalleeSavedFprPseudos(parsed_blocks, out);
    UnifyO0NoConflictBlockClobberAndSelfMove(parsed_blocks, out);
    PromoteNonConflictingSingleBlockMdPseudos(compiler_flags, gp64, parsed_blocks, out);
  } else {
    CoalesceDisjointMultiBlockSameRegPseudos(profile, compiler_flags, fp64, out, parsed_blocks);
    CoalesceSameBlockDisjointGlobalChainPseudos(profile, compiler_flags, gp64, fp64, out,
                                                parsed_blocks);
    DemoteFalsePreferenceConflictPseudos(profile, compiler_flags, gp64, fp64, out, parsed_blocks);
    PromoteNonConflictingSingleBlockMdPseudos(compiler_flags, gp64, parsed_blocks, out);
    ResolveLocalFalseCombineAndSuggPseudos(profile, compiler_flags, gp64, fp64,
                                           reload_spill_offsets, demoted_reload_scratch_masks, out,
                                           parsed_blocks);
    ResolveDeathlessLocalPseudos(gp64, fp64, out, parsed_blocks);
  }
  CompactAndRefreshPreAlterSideband(first_pseudo_regno, input_max_regno, compiler_flags.opt_level,
                                    parsed_blocks, out);
}

// On R3000 (`!mem_move_cost_6 && !soft_float`), `MEMORY_MOVE_COST == 4` equals
// `REGISTER_MOVE_COST(GR_REGS, FP_REGS) == 4` (`mips.h:2928`), so `regclass.c`
// (`p->cost[GR_REGS] < p->mem_cost`) sets `reg_alternate_class = NO_REGS` for
// any float pseudo participating in an FP operation (`reg_preferred_class =
// FP_REGS`). Neither `local_alloc` nor `global_alloc` can ever assign such a
// pseudo to a GPR; any non-ABI-arg GPR float web feeding an FPR input reload
// into an FP operation is therefore a user-declared explicit hard register.
void PinNonMemMoveCost6GprFloatWebs(
    const ParsedCompilerFlags& compiler_flags,
    const std::vector<std::vector<RtlPatternAndNotes>>& parsed_blocks,
    const std::vector<RegOccurrence>& occs,
    const absl::flat_hash_map<const RtlExpr*, int>& expr_to_occ,
    const absl::flat_hash_map<int, std::vector<int>>& members_by_root, WebUnionFind* uf,
    std::vector<bool>* pinned_seed) {
  if (compiler_flags.opt_level <= 0) {
    return;
  }
  bool soft_float = false;
  int mips_isa = 1;
  std::string cpu_str;
  for (const std::string& tok : compiler_flags.raw_tokens) {
    if (tok == "-msoft-float") {
      soft_float = true;
    } else if (tok == "-mhard-float") {
      soft_float = false;
    } else if (tok == "-mips1") {
      mips_isa = 1;
    } else if (tok == "-mips2") {
      mips_isa = 2;
    } else if (tok == "-mips3") {
      mips_isa = 3;
    } else if (tok == "-mips4") {
      mips_isa = 4;
    } else if (tok == "-m4650") {
      cpu_str = "r4650";
    } else if (tok.compare(0, 6, "-mcpu=") == 0) {
      cpu_str = tok.substr(6);
    }
  }
  bool mem_move_cost_6 = false;
  if (cpu_str.empty() || cpu_str == "default" || cpu_str == "DEFAULT") {
    mem_move_cost_6 = (mips_isa == 2 || mips_isa == 3);
  } else {
    std::string p = cpu_str;
    if (!p.empty() && (p[0] == 'r' || p[0] == 'R')) {
      p.erase(0, 1);
    }
    mem_move_cost_6 = (p == "4000" || p == "4k" || p == "4K" || p == "4400" || p == "6000" ||
                       p == "6k" || p == "6K");
  }
  if (soft_float || mem_move_cost_6) {
    return;
  }

  for (const auto& blk : parsed_blocks) {
    for (size_t i = 0; i + 1 < blk.size(); ++i) {
      const RtlExpr& pat = blk[i].pattern;
      if (!pat.IsSet() || !pat.SetDest().IsHardReg() || pat.SetDest().flags.Any() ||
          !pat.SetSrc().IsHardReg() || pat.SetDest().mode != pat.SetSrc().mode ||
          (pat.SetDest().mode != RtlMachineMode::kSF &&
           pat.SetDest().mode != RtlMachineMode::kDF)) {
        continue;
      }
      const int fpr = pat.SetDest().RegNo();
      const int gpr = pat.SetSrc().RegNo();
      if (fpr < 32 || fpr >= 64 || ((gpr < 8 || gpr > 25) && gpr != kFramePointerRegno)) {
        continue;
      }
      bool feeds_fp_op = false;
      for (size_t k = i + 1; k < blk.size(); ++k) {
        const RtlExpr& kpat = blk[k].pattern;
        if (kpat.IsSet() && kpat.SetSrc().op != RtlOp::kReg && kpat.SetSrc().op != RtlOp::kMem &&
            kpat.SetSrc().op != RtlOp::kConstDouble && kpat.SetSrc().ContainsReg(fpr)) {
          feeds_fp_op = true;
          break;
        }
        if (ContainsCallOp(kpat) || kpat.op == RtlOp::kSequence ||
            (kpat.IsSet() && kpat.SetDest().op == RtlOp::kPc) || kpat.ContainsReg(fpr)) {
          break;
        }
      }
      if (!feeds_fp_op) {
        continue;
      }
      const int occ_id = LookupOccId(pat.SetSrc(), expr_to_occ);
      if (occ_id < 0) {
        continue;
      }
      const int root = uf->Find(occ_id);
      const auto mit = members_by_root.find(root);
      if (mit != members_by_root.end() &&
          std::any_of(mit->second.begin(), mit->second.end(),
                      [&](int m) { return occs[m].set_count > 0; })) {
        (*pinned_seed)[occ_id] = true;
      }
    }
  }
}

// Mirrors `local-alloc.c:700-839, 1004-1007` (`optimize_reg_copy_1`) +
// `reload1.c:5186-5257` (`choose_reload_regs` reload inheritance): returns true
// when `blk[i]` is a cross-class FP->GPR copy `(set (reg:SF/DF r_gpr)
// (reg:SF/DF r_fpr))` carrying `(REG_DEAD r_fpr)` followed in the same block by
// an FPR-only arithmetic consumer reading `r_fpr` with both `r_gpr` and `r_fpr`
// unmodified (and no call) in between. Suppressing the `REG_DEAD r_fpr` kill on
// `blk[i]` during web collection unifies the inherited read with `r_fpr`'s
// pseudo web so `invert_reload.cc` Pass D can reverse the reload inheritance.
bool HasLaterCrossClassReloadInheritedFprRead(const std::vector<RtlPatternAndNotes>& blk, size_t i,
                                              bool gp64, bool fp64) {
  const RtlPatternAndNotes& pn = blk[i];
  if (!pn.pattern.IsSet() || !pn.pattern.SetDest().IsHardReg() ||
      !pn.pattern.SetSrc().IsHardReg() || pn.pattern.SetDest().IsArgPointerReg() ||
      pn.pattern.SetSrc().IsArgPointerReg() ||
      pn.pattern.SetDest().mode != pn.pattern.SetSrc().mode ||
      (pn.pattern.SetDest().mode != RtlMachineMode::kSF &&
       pn.pattern.SetDest().mode != RtlMachineMode::kDF)) {
    return false;
  }
  const int r_gpr = pn.pattern.SetDest().RegNo();
  const int r_fpr = pn.pattern.SetSrc().RegNo();
  if (r_gpr < 0 || r_gpr >= 32 || r_fpr < 32 || r_fpr >= 64) {
    return false;
  }
  const bool has_dead_fpr =
      std::any_of(pn.reg_notes.begin(), pn.reg_notes.end(), [&](const RtlRegNote& n) {
        return n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg() &&
               n.datum.RegNo() == r_fpr;
      });
  if (!has_dead_fpr) {
    return false;
  }
  const int gpr_span = std::max(1, MipsHardRegNregs(r_gpr, pn.pattern.SetDest().mode, gp64, fp64));
  const int fpr_span = std::max(1, MipsHardRegNregs(r_fpr, pn.pattern.SetSrc().mode, gp64, fp64));
  for (size_t m = i + 1; m < blk.size(); ++m) {
    const RtlExpr& mpat = blk[m].pattern;
    if (ContainsCallOp(mpat) || mpat.op == RtlOp::kSequence ||
        (mpat.IsSet() && mpat.SetDest().op == RtlOp::kPc)) {
      return false;
    }
    if (mpat.IsSet() && mpat.SetSrc().op != RtlOp::kReg && mpat.SetSrc().op != RtlOp::kMem &&
        mpat.SetSrc().op != RtlOp::kConstDouble &&
        (mpat.SetSrc().mode == RtlMachineMode::kSF || mpat.SetSrc().mode == RtlMachineMode::kDF ||
         mpat.SetSrc().op == RtlOp::kFix || mpat.SetSrc().op == RtlOp::kUnsignedFix)) {
      for (int w = 0; w < fpr_span; ++w) {
        if (mpat.SetSrc().ContainsReg(r_fpr + w)) {
          return true;
        }
      }
    }
    std::array<bool, kFirstPseudoRegister> written{};
    CollectWrittenHardRegWordsInPattern(mpat, gp64, fp64, &written);
    for (int w = 0; w < gpr_span; ++w) {
      if (r_gpr + w < kFirstPseudoRegister && written[r_gpr + w]) {
        return false;
      }
    }
    for (int w = 0; w < fpr_span; ++w) {
      if (r_fpr + w < kFirstPseudoRegister && written[r_fpr + w]) {
        return false;
      }
    }
  }
  return false;
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
      const RtlPatternAndNotes& pn = parsed_blocks[b][i];
      bool lift_di_clobber = (compiler_flags.opt_level == 0);
      if (!lift_di_clobber && !gp64 && pn.pattern.op == RtlOp::kParallel &&
          pn.pattern.children.size() == 2 && pn.pattern.children[0].IsSet() &&
          pn.pattern.children[1].op == RtlOp::kClobber &&
          pn.pattern.children[1].children.size() == 1 &&
          pn.pattern.children[1].children[0].IsHardReg()) {
        const int chr = pn.pattern.children[1].children[0].RegNo();
        const RtlExpr& sdst = pn.pattern.children[0].SetDest();
        const bool overlaps_dst =
            sdst.IsHardReg() && (chr == sdst.RegNo() || chr == sdst.RegNo() + 1);
        if (!overlaps_dst) {
          for (const RtlRegNote& n : pn.reg_notes) {
            if (n.kind == RtlMachineMode::kRegUnused && n.datum.IsHardReg() &&
                n.datum.RegNo() == chr) {
              lift_di_clobber = true;
              break;
            }
          }
        }
      }
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
    bool ep_lift_di_clobber = (compiler_flags.opt_level == 0);
    if (!ep_lift_di_clobber && !gp64 && epilogue_delay_parsed->pattern.op == RtlOp::kParallel &&
        epilogue_delay_parsed->pattern.children.size() == 2 &&
        epilogue_delay_parsed->pattern.children[0].IsSet() &&
        epilogue_delay_parsed->pattern.children[1].op == RtlOp::kClobber &&
        epilogue_delay_parsed->pattern.children[1].children.size() == 1 &&
        epilogue_delay_parsed->pattern.children[1].children[0].IsHardReg()) {
      const int chr = epilogue_delay_parsed->pattern.children[1].children[0].RegNo();
      for (const RtlRegNote& n : epilogue_delay_parsed->reg_notes) {
        if (n.kind == RtlMachineMode::kRegUnused && n.datum.IsHardReg() && n.datum.RegNo() == chr) {
          ep_lift_di_clobber = true;
          break;
        }
      }
    }
    RegisterPatternOccurrences(epilogue_delay_parsed->pattern, /*in_set_dest=*/false,
                               /*in_clobber=*/false, /*ref_weight=*/1, gp64, fp64, fp_is_fixed,
                               ep_lift_di_clobber, &occs, &expr_to_occ);
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
        compiler_flags.opt_level > 0 && open_libcall_clobbers->empty() && !is_paired_retval[b][i] &&
        !HasLaterCrossClassReloadInheritedFprRead(parsed_blocks[b], i, gp64, fp64);
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
  } else {
    PinNonMemMoveCost6GprFloatWebs(compiler_flags, parsed_blocks, occs, expr_to_occ,
                                   members_by_root, &uf, &pinned_seed);
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

  PostProcessPreAlterCfg(profile, compiler_flags, gp64, fp64, reload_spill_offsets,
                         first_pseudo_regno, allocated_cfg.sideband.max_regno, cs_save_span,
                         &parsed_blocks, &out);
  return out;
}

}  // namespace rom_nom_nom::lifter
