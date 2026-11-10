#include "lifter/invert_block_alloc.h"

#include <cstddef>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kStatusRegnum = 67;
constexpr int kFirstPseudoRegister = 68;
constexpr int kReturnAddressRegno = 31;

struct RestoredScratchClobber {
  int regno = -1;
  RtlMachineMode mode = RtlMachineMode::kNone;
  bool consumed_in_notes = false;
};

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
        absl::StrCat("InvertBlockAlloc: negative register number ", expr.RegNo()));
  }
  for (const RtlExpr& child : expr.children) {
    absl::Status status = ValidateRegNumbers(child);
    if (!status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

bool ContainsCallOrUnspecVolatile(const RtlExpr& expr) {
  if (expr.op == RtlOp::kCall || expr.op == RtlOp::kUnspecVolatile) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (ContainsCallOrUnspecVolatile(child)) {
      return true;
    }
  }
  return false;
}

bool RestoreScratchClobbersInInsn(RtlPatternAndNotes* parsed) {
  if (parsed->pattern.op != RtlOp::kParallel || ContainsCallOrUnspecVolatile(parsed->pattern)) {
    return false;
  }

  std::vector<RestoredScratchClobber> restored_scratches;
  for (RtlExpr& elem : parsed->pattern.children) {
    if (elem.op == RtlOp::kClobber && elem.children.size() == 1 && elem.children[0].IsReg()) {
      const int regno = elem.children[0].RegNo();
      const RtlMachineMode mode = elem.children[0].mode;
      if (regno >= 0 && regno < kFirstPseudoRegister && regno != kReturnAddressRegno) {
        restored_scratches.push_back(RestoredScratchClobber{
            .regno = regno,
            .mode = mode,
            .consumed_in_notes = false,
        });
        elem.children[0] = RtlExpr::MakeScratch(mode);
      }
    }
  }

  if (restored_scratches.empty()) {
    return false;
  }

  // In GCC (`flow.c:mark_set_1`), the `REG_UNUSED` note attached for a
  // `(clobber (scratch:M))` shares the exact same `SCRATCH` rtx pointer as the
  // clobber operand, so `block_alloc` mutates both in-place to `(reg:M regno)`.
  // Restore matching `REG_UNUSED` notes back to `(scratch:M)` in forward order.
  for (RtlRegNote& note : parsed->reg_notes) {
    if (note.kind == RtlMachineMode::kRegUnused && note.datum.IsReg()) {
      for (RestoredScratchClobber& restored : restored_scratches) {
        if (!restored.consumed_in_notes && restored.regno == note.datum.RegNo() &&
            restored.mode == note.datum.mode) {
          note.datum = RtlExpr::MakeScratch(restored.mode);
          restored.consumed_in_notes = true;
          break;
        }
      }
    }
  }

  return true;
}

absl::Status ValidateSideband(const RtlSidebandState& sideband) {
  for (const auto& [pseudo, hard] : sideband.reg_renumber) {
    if (pseudo < kStatusRegnum) {
      return absl::InvalidArgumentError(absl::StrCat("InvertBlockAlloc: invalid reg_renumber key ",
                                                     pseudo, " (< ", kStatusRegnum, ")"));
    }
    if (hard < -1 || hard >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrCat(
          "InvertBlockAlloc: invalid reg_renumber target ", hard, " for pseudo ", pseudo));
    }
  }
  for (const auto& [regno, refs] : sideband.reg_n_refs) {
    (void)refs;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: negative regno in reg_n_refs: ", regno));
    }
  }
  for (const auto& [regno, sets] : sideband.reg_n_sets) {
    (void)sets;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: negative regno in reg_n_sets: ", regno));
    }
  }
  for (const auto& [regno, deaths] : sideband.reg_n_deaths) {
    (void)deaths;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: negative regno in reg_n_deaths: ", regno));
    }
  }
  for (const auto& [regno, len] : sideband.reg_live_length) {
    (void)len;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: negative regno in reg_live_length: ", regno));
    }
  }
  for (const auto& [regno, calls] : sideband.reg_n_calls_crossed) {
    (void)calls;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: negative regno in reg_n_calls_crossed: ", regno));
    }
  }
  for (const auto& [bb_idx, live_regs] : sideband.basic_block_live_at_start) {
    if (bb_idx < 0) {
      return absl::InvalidArgumentError(absl::StrCat(
          "InvertBlockAlloc: negative basic_block_live_at_start block index ", bb_idx));
    }
    for (int regno : live_regs) {
      if (regno < 0) {
        return absl::InvalidArgumentError(
            absl::StrCat("InvertBlockAlloc: negative regno in basic_block_live_at_start: ", regno));
      }
    }
  }
  for (int regno : sideband.saved_gpr_regnos) {
    if (regno < 0 || regno >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: invalid saved_gpr_regno ", regno));
    }
  }
  for (int regno : sideband.saved_fpr_regnos) {
    if (regno < 0 || regno >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: invalid saved_fpr_regno ", regno));
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg) {
  if (lreg_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertBlockAlloc: input CFG has no basic blocks");
  }

  const size_t num_blocks = lreg_cfg.rpo_blocks.size();
  absl::Status sideband_status = ValidateSideband(lreg_cfg.sideband);
  if (!sideband_status.ok()) {
    return sideband_status;
  }

  RtlControlFlowGraph out = lreg_cfg;
  out.pass = RtlPassKind::kLreg;
  out.sideband.reg_renumber.clear();

  for (size_t b = 0; b < num_blocks; ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrCat("InvertBlockAlloc: block ", b,
                                                       " has out-of-bounds successor ", succ,
                                                       " (num_blocks=", num_blocks, ")"));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrCat("InvertBlockAlloc: block ", b,
                                                       " has out-of-bounds addr_vec target ",
                                                       target, " (num_blocks=", num_blocks, ")"));
      }
    }

    bool modified_block = false;
    for (std::string& pat_str : blk.instructions) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(pat_str);
      if (!parsed.ok()) {
        return parsed.status();
      }
      if (parsed->pattern.IsNil() || ContainsUnknownOp(parsed->pattern)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "InvertBlockAlloc: unrecognized RTL op in instruction pattern: ", pat_str));
      }
      absl::Status reg_status = ValidateRegNumbers(parsed->pattern);
      if (!reg_status.ok()) {
        return reg_status;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (ContainsUnknownOp(note.datum)) {
          return absl::InvalidArgumentError(
              absl::StrCat("InvertBlockAlloc: unrecognized RTL op in note: ", pat_str));
        }
        absl::Status note_reg_status = ValidateRegNumbers(note.datum);
        if (!note_reg_status.ok()) {
          return note_reg_status;
        }
      }

      if (RestoreScratchClobbersInInsn(&(*parsed))) {
        pat_str = parsed->ToSexprString();
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
