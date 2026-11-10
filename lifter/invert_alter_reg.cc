#include "lifter/invert_alter_reg.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/numbers.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;
constexpr int kFramePointerRegno = 30;
constexpr int kStackSlotAlignmentBytes = 8;

struct HardRegGroupKey {
  int hard_regno = -1;
  RtlMachineMode mode = RtlMachineMode::kNone;
  bool volatil = false;
  bool in_struct = false;
  bool unchanging = false;

  bool operator==(const HardRegGroupKey& other) const {
    return hard_regno == other.hard_regno && mode == other.mode && volatil == other.volatil &&
           in_struct == other.in_struct && unchanging == other.unchanging;
  }

  template <typename H>
  friend H AbslHashValue(H h, const HardRegGroupKey& k) {
    return H::combine(std::move(h), k.hard_regno, static_cast<int>(k.mode), k.volatil, k.in_struct,
                      k.unchanging);
  }
};

struct HardRegGroupStats {
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

// Returns true if `regno` is an allocatable MIPS GPR (`$2..$25`) or FPR (`$32..$63`).
// Fixed registers (`$0`, `$1/at`, `$26/k0`, `$27/k1`, `$28/gp`, `$29/sp`, `$30/fp`, `$31/ra`)
// and MD accumulator registers (`64..66`) are never assigned to general pseudos by `alter_reg`.
bool IsAllocatableHardReg(int regno) {
  return (regno >= 2 && regno <= 25) || (regno >= 32 && regno <= 63);
}

// Returns true if `regno` is a MIPS hardware incoming argument register
// (`$4..$7` / `a0..a3` or `$44..$47` / `$f12..$f15`).
bool IsIncomingArgumentHardReg(int regno) {
  return (regno >= 4 && regno <= 7) || (regno >= 44 && regno <= 47);
}

// Returns true if `reg` is an allocatable hard-register node eligible to be a
// candidate for `alter_reg` reversal. Hardware return registers (`/i`
// `integrated`) and prologue frame-related registers (`/f` `frame_related`)
// are never pseudo-registers.
bool IsCandidateHardRegNode(const RtlExpr& reg) {
  if (reg.op != RtlOp::kReg || reg.flags.integrated || reg.flags.frame_related) {
    return false;
  }
  const int regno = reg.RegNo();
  return IsAllocatableHardReg(regno);
}

HardRegGroupKey MakeHardRegGroupKey(const RtlExpr& reg) {
  return HardRegGroupKey{
      .hard_regno = reg.RegNo(),
      .mode = reg.mode,
      .volatil = reg.flags.volatil,
      .in_struct = reg.flags.in_struct,
      .unchanging = reg.flags.unchanging,
  };
}

int GetMachineModeSizeBytes(RtlMachineMode mode) {
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
  if (expr.op == RtlOp::kSequence && expr.children.size() < 2) {
    return false;
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

// If `addr` is `(reg:SI 30 fp)` or `(plus:SI (reg:SI 30 fp) (const_int <offset>))`,
// returns `<offset>`. Otherwise returns `std::nullopt`.
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

// Collects candidate hard-register definitions and references from a destination
// subexpression `<dst>` of `(set <dst> <src>)`.
// Note: if `<dst>` is `(mem ...)`, the memory location is written while any
// registers inside the address of `(mem ...)` are read, not defined.
void CollectPatternRefs(const RtlExpr& expr, bool in_set_dest, bool in_clobber,
                        std::vector<HardRegGroupKey>* order,
                        absl::flat_hash_map<HardRegGroupKey, HardRegGroupStats>* stats) {
  if (IsCandidateHardRegNode(expr)) {
    const HardRegGroupKey key = MakeHardRegGroupKey(expr);
    auto it = stats->find(key);
    if (it == stats->end()) {
      order->push_back(key);
      it = stats->emplace(key, HardRegGroupStats{}).first;
    }
    if (!in_clobber) {
      it->second.n_refs += 1;
      it->second.has_non_clobber_ref = true;
    }
    if (in_set_dest && !in_clobber) {
      it->second.n_sets += 1;
    }
    return;
  }

  if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
    CollectPatternRefs(expr.children[0], /*in_set_dest=*/true, /*in_clobber=*/false, order, stats);
    CollectPatternRefs(expr.children[1], /*in_set_dest=*/false, /*in_clobber=*/false, order, stats);
    return;
  }

  if (expr.op == RtlOp::kClobber && !expr.children.empty()) {
    CollectPatternRefs(expr.children[0], /*in_set_dest=*/true, /*in_clobber=*/true, order, stats);
    return;
  }

  if (expr.op == RtlOp::kMem) {
    for (const RtlExpr& child : expr.children) {
      CollectPatternRefs(child, /*in_set_dest=*/false, /*in_clobber=*/false, order, stats);
    }
    return;
  }

  const bool pass_dest =
      in_set_dest && (expr.op == RtlOp::kSubreg || expr.op == RtlOp::kStrictLowPart ||
                      expr.op == RtlOp::kZeroExtract || expr.op == RtlOp::kSignExtract);
  for (size_t i = 0; i < expr.children.size(); ++i) {
    const bool child_dest = pass_dest && (i == 0);
    CollectPatternRefs(expr.children[i], child_dest, in_clobber, order, stats);
  }
}

// Inspects all `$fp`-relative `MEM` and non-`MEM` occurrences in `expr` to
// identify 8-byte stack slots allocated by `alter_reg` via `assign_stack_local`.
void AnalyzeFpStackSlots(const RtlExpr& expr, bool in_set_dest, bool in_note, int frame_size_bytes,
                         int outgoing_args_size, absl::flat_hash_map<int, StackSlotStats>* slots,
                         absl::flat_hash_set<int>* invalidated_slots) {
  if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
    AnalyzeFpStackSlots(expr.children[0], /*in_set_dest=*/true, in_note, frame_size_bytes,
                        outgoing_args_size, slots, invalidated_slots);
    AnalyzeFpStackSlots(expr.children[1], /*in_set_dest=*/false, in_note, frame_size_bytes,
                        outgoing_args_size, slots, invalidated_slots);
    return;
  }

  if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
    const std::optional<int64_t> fp_off = ExtractFpRelativeOffset(expr.children[0]);
    if (fp_off.has_value()) {
      const int64_t rel_off = *fp_off - outgoing_args_size;
      if (rel_off >= 0 && rel_off < frame_size_bytes) {
        const int slot_base =
            static_cast<int>((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
        const int mode_size = GetMachineModeSizeBytes(expr.mode);
        const int expected_rel_off = slot_base + (kStackSlotAlignmentBytes - mode_size);
        if (mode_size <= 0 || rel_off != expected_rel_off || expr.flags.volatil ||
            expr.flags.in_struct || expr.flags.integrated || expr.flags.frame_related) {
          invalidated_slots->insert(slot_base);
        } else {
          auto it = slots->find(slot_base);
          if (it == slots->end()) {
            StackSlotStats st;
            st.mode = expr.mode;
            st.unchanging = expr.flags.unchanging;
            st.n_refs = in_note ? 0 : 1;
            st.n_sets = (!in_note && in_set_dest) ? 1 : 0;
            slots->emplace(slot_base, st);
          } else {
            if (it->second.mode != expr.mode || it->second.unchanging != expr.flags.unchanging) {
              it->second.valid = false;
              invalidated_slots->insert(slot_base);
            } else {
              if (!in_note) {
                it->second.n_refs += 1;
                if (in_set_dest) {
                  it->second.n_sets += 1;
                }
              }
            }
          }
        }
      }
      return;
    }
  }

  // Any direct `$fp + const_int` or bare `$fp` outside a `(mem ...)` address
  // invalidates that 8-byte slot from being an `alter_reg` spilled pseudo.
  if (expr.op == RtlOp::kPlus && expr.children.size() == 2) {
    const std::optional<int64_t> fp_off = ExtractFpRelativeOffset(expr);
    if (fp_off.has_value()) {
      const int64_t rel_off = *fp_off - outgoing_args_size;
      if (rel_off >= 0 && rel_off < frame_size_bytes) {
        const int slot_base =
            static_cast<int>((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
        invalidated_slots->insert(slot_base);
      }
      return;
    }
  }
  if (expr.op == RtlOp::kReg && expr.RegNo() == kFramePointerRegno) {
    if (0 >= outgoing_args_size && 0 < frame_size_bytes + outgoing_args_size) {
      invalidated_slots->insert(0);
    }
    return;
  }

  for (const RtlExpr& child : expr.children) {
    AnalyzeFpStackSlots(child, /*in_set_dest=*/false, in_note, frame_size_bytes, outgoing_args_size,
                        slots, invalidated_slots);
  }
}

void RewriteExprAlterReg(
    RtlExpr* expr, int outgoing_args_size,
    const absl::flat_hash_map<HardRegGroupKey, HardRegGroupStats>& hard_reg_map,
    const absl::flat_hash_map<int, int>& slot_to_pseudo) {
  if (IsCandidateHardRegNode(*expr)) {
    const HardRegGroupKey key = MakeHardRegGroupKey(*expr);
    auto it = hard_reg_map.find(key);
    if (it != hard_reg_map.end() && it->second.pseudo_regno >= kFirstPseudoRegister) {
      expr->SetRegNo(it->second.pseudo_regno);
      return;
    }
  }

  if (!slot_to_pseudo.empty() && expr->op == RtlOp::kMem && expr->children.size() == 1) {
    const std::optional<int64_t> fp_off = ExtractFpRelativeOffset(expr->children[0]);
    if (fp_off.has_value()) {
      const int64_t rel_off = *fp_off - outgoing_args_size;
      const int slot_base =
          static_cast<int>((rel_off / kStackSlotAlignmentBytes) * kStackSlotAlignmentBytes);
      auto it = slot_to_pseudo.find(slot_base);
      if (it != slot_to_pseudo.end()) {
        RtlExpr replacement = RtlExpr::MakeReg(expr->mode, it->second, expr->flags);
        *expr = std::move(replacement);
        return;
      }
    }
  }

  for (RtlExpr& child : expr->children) {
    RewriteExprAlterReg(&child, outgoing_args_size, hard_reg_map, slot_to_pseudo);
  }
}

}  // namespace

RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg,
                                   int first_pseudo_regno) {
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

  // First pass: collect hard-register group statistics and `$fp`-relative stack
  // slot statistics across all instructions in `allocated_cfg`.
  std::vector<HardRegGroupKey> group_order;
  absl::flat_hash_map<HardRegGroupKey, HardRegGroupStats> group_stats;

  const int frame_size_bytes = allocated_cfg.sideband.frame_size_bytes;
  const int outgoing_args_size = allocated_cfg.sideband.outgoing_args_size;
  const bool can_check_stack_slots = frame_size_bytes >= kStackSlotAlignmentBytes &&
                                     (frame_size_bytes % kStackSlotAlignmentBytes) == 0;
  absl::flat_hash_map<int, StackSlotStats> stack_slots;
  absl::flat_hash_set<int> invalidated_slots;

  for (size_t b = 0; b < num_blocks; ++b) {
    for (const RtlPatternAndNotes& parsed : parsed_blocks[b]) {
      CollectPatternRefs(parsed.pattern, /*in_set_dest=*/false, /*in_clobber=*/false, &group_order,
                         &group_stats);
      if (can_check_stack_slots) {
        AnalyzeFpStackSlots(parsed.pattern, /*in_set_dest=*/false, /*in_note=*/false,
                            frame_size_bytes, outgoing_args_size, &stack_slots, &invalidated_slots);
        for (const RtlRegNote& note : parsed.reg_notes) {
          if (note.list_op == RtlOp::kExprList) {
            AnalyzeFpStackSlots(note.datum, /*in_set_dest=*/false, /*in_note=*/true,
                                frame_size_bytes, outgoing_args_size, &stack_slots,
                                &invalidated_slots);
          }
        }
      }
    }
  }

  int next_pseudo = std::max(first_pseudo_regno, max_existing_regno + 1);

  RtlControlFlowGraph out = allocated_cfg;
  out.sideband.reg_renumber.clear();

  // Assign fresh pseudo-register numbers to eligible hard-register groups.
  // A hard-register group is eligible if it has at least one non-clobber
  // reference in `allocated_cfg` and is not a read-only incoming hardware
  // argument register (`$a0..$a3`, `$f12..$f15`) without `/v`.
  for (const HardRegGroupKey& key : group_order) {
    auto it = group_stats.find(key);
    if (it == group_stats.end() || !it->second.has_non_clobber_ref) {
      continue;
    }
    if (IsIncomingArgumentHardReg(key.hard_regno) && !key.volatil && it->second.n_sets == 0) {
      continue;
    }
    const int pseudo = next_pseudo++;
    it->second.pseudo_regno = pseudo;
    out.sideband.reg_renumber[pseudo] = key.hard_regno;
    out.sideband.reg_n_refs[pseudo] = std::max(1, it->second.n_refs);
    out.sideband.reg_n_sets[pseudo] = it->second.n_sets;
  }

  // Identify contiguous 8-byte `$fp`-relative stack slots at the top of
  // `frame_size_bytes` (`slot_base = frame_size_bytes - 8, frame_size_bytes - 16, ...`)
  // that match `alter_reg`'s `assign_stack_local(mode, size, -1)` layout.
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
    // Assign pseudos in ascending `slot_base` order so `RunAlterReg` allocates
    // them via `assign_stack_local` in the exact same order.
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

  // Second pass: rewrite all lifted hard-register occurrences and stack-slot
  // `MEM`s across both instruction patterns and `REG_NOTES`.
  for (size_t b = 0; b < num_blocks; ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      RtlPatternAndNotes& parsed = parsed_blocks[b][i];
      RewriteExprAlterReg(&parsed.pattern, outgoing_args_size, group_stats, slot_to_pseudo);
      for (RtlRegNote& note : parsed.reg_notes) {
        if (note.list_op == RtlOp::kExprList) {
          RewriteExprAlterReg(&note.datum, outgoing_args_size, group_stats, slot_to_pseudo);
        }
      }
      blk.instructions[i] = parsed.ToSexprString();
    }
    blk.SyncRtlInsnsFromInstructions();
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
