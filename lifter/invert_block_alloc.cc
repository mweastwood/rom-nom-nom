#include "lifter/invert_block_alloc.h"

#include <algorithm>
#include <cstddef>
#include <string>
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
constexpr int kFramePointerRegno = 30;
constexpr int kReturnAddressRegno = 31;

struct RestoredScratchClobber {
  int regno = -1;
  RtlMachineMode mode = RtlMachineMode::kNone;
  bool consumed_in_notes = false;
};

int ModeByteSize(RtlMachineMode mode, const std::string& raw_mode) {
  if (mode == RtlMachineMode::kUnknown) {
    if (raw_mode == "CQI") return 2;
    if (raw_mode == "CHI" || raw_mode == "CC_FP" || raw_mode == "CC_REV_FP") return 4;
    if (raw_mode == "SC" || raw_mode == "CSI") return 8;
    if (raw_mode == "DC" || raw_mode == "CDI") return 16;
    if (raw_mode == "TC" || raw_mode == "CTI") return 32;
  }
  switch (mode) {
    case RtlMachineMode::kQi:
      return 1;
    case RtlMachineMode::kHi:
      return 2;
    case RtlMachineMode::kSi:
    case RtlMachineMode::kSf:
    case RtlMachineMode::kCc:
      return 4;
    case RtlMachineMode::kDi:
    case RtlMachineMode::kDf:
      return 8;
    case RtlMachineMode::kTi:
    case RtlMachineMode::kTf:
      return 16;
    default:
      return 4;
  }
}

int HardRegSpanCount(int regno, RtlMachineMode mode, const std::string& raw_mode, bool gp64,
                     bool fp64) {
  const int bytes = ModeByteSize(mode, raw_mode);
  const bool is_fpr = (regno >= 32 && regno < 64);
  const int unit = is_fpr ? (fp64 ? 8 : 4) : (gp64 ? 8 : 4);
  return std::max(1, (bytes + unit - 1) / unit);
}

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

RtlExpr* MutableUnwrapInsnContainer(RtlExpr* expr) {
  if ((expr->op == RtlOp::kInsn || expr->op == RtlOp::kJumpInsn || expr->op == RtlOp::kCallInsn) &&
      !expr->children.empty()) {
    return &expr->children[0];
  }
  return expr;
}

bool ContainsCallUnspecVolatileOrAsm(const RtlExpr& expr) {
  if (expr.op == RtlOp::kCall || expr.op == RtlOp::kCallInsn || expr.op == RtlOp::kUnspecVolatile ||
      expr.op == RtlOp::kAsmOperands) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (ContainsCallUnspecVolatileOrAsm(child)) {
      return true;
    }
  }
  return false;
}

bool IsMipsFixedHardReg(int regno) {
  return regno == 0 || regno == 1 || regno == 26 || regno == 27 || regno == 28 || regno == 29 ||
         regno == kReturnAddressRegno || regno == 67;
}

bool ExprContainsOp(const RtlExpr& expr, RtlOp op) {
  if (expr.op == op) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (ExprContainsOp(child, op)) {
      return true;
    }
  }
  return false;
}

// Returns true iff `parallel_expr` matches a `mips.md` `PARALLEL` pattern whose
// `(clobber ...)` operands originate from `(match_scratch ...)`, as opposed to
// `(match_operand ... "register_operand")` (such as `!gp64` `DImode` `adddi3`,
// `subdi3`, `negdi2`, `ashldi3`, `ashrdi3`, `lshrdi3`, or `casesi_internal`).
bool IsMipsMatchScratchParallel(const RtlExpr& parallel_expr) {
  if (parallel_expr.op != RtlOp::kParallel) {
    return false;
  }
  for (const RtlExpr& elem : parallel_expr.children) {
    if (elem.op != RtlOp::kSet || elem.children.size() != 2) {
      continue;
    }
    const RtlExpr& dst = elem.children[0];
    const RtlExpr& src = elem.children[1];
    if (dst.op == RtlOp::kMem && dst.mode == RtlMachineMode::kBlk && src.op == RtlOp::kMem &&
        src.mode == RtlMachineMode::kBlk) {
      return true;
    }
    switch (src.op) {
      case RtlOp::kMult:
      case RtlOp::kDiv:
      case RtlOp::kMod:
      case RtlOp::kUdiv:
      case RtlOp::kUmod:
      case RtlOp::kTruncate:
      case RtlOp::kFfs:
      case RtlOp::kFix:
        return true;
      case RtlOp::kPlus:
        if (ExprContainsOp(src, RtlOp::kMult)) {
          return true;
        }
        break;
      default:
        break;
    }
  }
  return false;
}

bool RestoreScratchClobbersInInsn(RtlPatternAndNotes* parsed, bool gp64, bool fp64,
                                  absl::flat_hash_set<int>* erased_hard_regs,
                                  absl::flat_hash_map<int, int>* restored_scratch_set_counts,
                                  absl::flat_hash_map<int, int>* restored_scratch_death_counts) {
  if (ContainsCallUnspecVolatileOrAsm(parsed->pattern)) {
    return false;
  }
  RtlExpr* inner = MutableUnwrapInsnContainer(&parsed->pattern);
  if (!IsMipsMatchScratchParallel(*inner)) {
    return false;
  }

  bool is_fpr_dest_fix = false;
  for (const RtlExpr& elem : inner->children) {
    if (elem.op == RtlOp::kSet && elem.children.size() == 2 && elem.children[1].op == RtlOp::kFix &&
        elem.children[0].IsReg() && !elem.children[0].IsArgPointerReg() &&
        elem.children[0].RegNo() >= 32 && elem.children[0].RegNo() < 64) {
      is_fpr_dest_fix = true;
      break;
    }
  }

  std::vector<RestoredScratchClobber> restored_scratches;
  for (RtlExpr& elem : inner->children) {
    if (elem.op == RtlOp::kClobber && elem.children.size() == 1 && elem.children[0].IsReg() &&
        !elem.children[0].IsArgPointerReg()) {
      const int regno = elem.children[0].RegNo();
      const RtlMachineMode mode = elem.children[0].mode;
      const std::string raw_mode = elem.children[0].raw_mode;
      if (is_fpr_dest_fix && regno >= 32 && regno < 64) {
        continue;
      }
      if (regno >= 0 && regno < kMipsFirstPseudoRegister && !IsMipsFixedHardReg(regno)) {
        const int span = HardRegSpanCount(regno, mode, raw_mode, gp64, fp64);
        for (int k = regno; k < regno + span && k < kMipsFirstPseudoRegister; ++k) {
          erased_hard_regs->insert(k);
          (*restored_scratch_set_counts)[k] += 1;
        }
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
          (*restored_scratch_death_counts)[restored.regno] += 1;
          restored.consumed_in_notes = true;
          break;
        }
      }
    }
  }

  return true;
}

void RecordPseudoModesAndExplicitHardRegs(
    const RtlPatternAndNotes& parsed, const std::vector<RtlExpr>& extra_fusage,
    bool frame_pointer_needed, bool gp64, bool fp64,
    absl::flat_hash_map<int, std::pair<RtlMachineMode, std::string>>* pseudo_modes,
    absl::flat_hash_set<int>* explicit_hard_regs) {
  auto visit_expr = [&](const RtlExpr& root) {
    root.Visit([&](const RtlExpr& node) {
      if (!node.IsReg() || node.IsArgPointerReg()) {
        return;
      }
      const int r = node.RegNo();
      if (r >= kMipsFirstPseudoRegister) {
        pseudo_modes->try_emplace(r, std::make_pair(node.mode, node.raw_mode));
        return;
      }
      if (r == kArgPointerRegno || (r == kFramePointerRegno && !frame_pointer_needed)) {
        return;
      }
      const int span = HardRegSpanCount(r, node.mode, node.raw_mode, gp64, fp64);
      for (int k = r; k < r + span && k < kMipsFirstPseudoRegister; ++k) {
        explicit_hard_regs->insert(k);
      }
    });
  };

  visit_expr(parsed.pattern);
  for (const RtlRegNote& note : parsed.reg_notes) {
    if (note.datum.IsReg() && note.datum.RegNo() >= kMipsFirstPseudoRegister) {
      pseudo_modes->try_emplace(note.datum.RegNo(),
                                std::make_pair(note.datum.mode, note.datum.raw_mode));
    }
  }
  for (const RtlExpr& fu : parsed.call_fusage) {
    visit_expr(fu);
  }
  for (const RtlExpr& fu : extra_fusage) {
    visit_expr(fu);
  }
}

absl::Status ValidateSideband(const RtlSidebandState& sideband) {
  for (const auto& [pseudo, hard] : sideband.reg_renumber) {
    if (pseudo < kMipsFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrCat("InvertBlockAlloc: invalid reg_renumber key ",
                                                     pseudo, " (< ", kMipsFirstPseudoRegister,
                                                     ")"));
    }
    if (hard < -1 || hard >= kMipsFirstPseudoRegister) {
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
    if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: invalid saved_gpr_regno ", regno));
    }
  }
  for (int regno : sideband.saved_fpr_regnos) {
    if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertBlockAlloc: invalid saved_fpr_regno ", regno));
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg,
                                                     const CompilationProfile& profile) {
  if (lreg_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertBlockAlloc: input CFG has no basic blocks");
  }

  const size_t num_blocks = lreg_cfg.rpo_blocks.size();
  absl::Status sideband_status = ValidateSideband(lreg_cfg.sideband);
  if (!sideband_status.ok()) {
    return sideband_status;
  }

  ParsedCompilerFlags flags = ParsedCompilerFlags::DefaultO2WaG();
  if (!profile.c_flags.empty()) {
    flags = ParsedCompilerFlags::Parse(profile.c_flags);
  } else {
    flags.ApplyOptimizationLevelDefaults(profile.optimize_level);
  }
  bool gp64 = false;
  bool fp64 = false;
  for (const std::string& tok : flags.raw_tokens) {
    if (tok == "-mgp64" || tok == "-mips3" || tok == "-mips4") {
      gp64 = true;
    } else if (tok == "-mgp32" || tok == "-mips1" || tok == "-mips2") {
      gp64 = false;
    }
    if (tok == "-mfp64" || tok == "-mips3" || tok == "-mips4") {
      fp64 = true;
    } else if (tok == "-mfp32" || tok == "-mips1" || tok == "-mips2") {
      fp64 = false;
    }
  }

  RtlControlFlowGraph out = lreg_cfg;
  out.pass = RtlPassKind::kLreg;
  out.sideband.reg_renumber.clear();

  absl::flat_hash_map<int, std::pair<RtlMachineMode, std::string>> pseudo_modes;
  absl::flat_hash_set<int> explicit_hard_regs;
  absl::flat_hash_set<int> erased_hard_regs;
  absl::flat_hash_map<int, int> restored_scratch_set_counts;
  absl::flat_hash_map<int, int> restored_scratch_death_counts;
  if (out.sideband.frame_pointer_needed) {
    explicit_hard_regs.insert(kFramePointerRegno);
  }

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
    for (size_t ii = 0; ii < blk.instructions.size(); ++ii) {
      std::string& pat_str = blk.instructions[ii];
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

      if (flags.opt_level > 0 &&
          RestoreScratchClobbersInInsn(&(*parsed), gp64, fp64, &erased_hard_regs,
                                       &restored_scratch_set_counts,
                                       &restored_scratch_death_counts)) {
        pat_str = FormatGccPrintRtlPatternAndNotes(parsed->pattern, parsed->reg_notes,
                                                   parsed->bracketed_notes, /*call_fusage=*/{});
        modified_block = true;
      }

      const std::vector<RtlExpr> empty_fusage;
      const std::vector<RtlExpr>& extra_fusage =
          (ii < blk.rtl_insns.size()) ? blk.rtl_insns[ii].call_fusage : empty_fusage;
      RecordPseudoModesAndExplicitHardRegs(*parsed, extra_fusage, out.sideband.frame_pointer_needed,
                                           gp64, fp64, &pseudo_modes, &explicit_hard_regs);
    }
    if (modified_block) {
      const std::vector<RtlInsn> prev_rtl = blk.rtl_insns;
      blk.SyncRtlInsnsFromInstructions();
      for (size_t k = 0; k < prev_rtl.size() && k < blk.rtl_insns.size(); ++k) {
        if (prev_rtl[k].has_explicit_call_fusage && !blk.rtl_insns[k].has_explicit_call_fusage) {
          blk.rtl_insns[k].has_explicit_call_fusage = true;
          blk.rtl_insns[k].call_fusage = prev_rtl[k].call_fusage;
        }
      }
    }
  }

  if (flags.opt_level > 0) {
    // `flow.c:2046-2047` (`mark_set_1`) and `local-alloc.c:1396-1401`
    // (`block_alloc` / `alloc_qty_for_scratch`): pre-`local_alloc`
    // `(clobber (scratch:M))` operands are not hard-register `SET`/`CLOBBER`s
    // and are never counted in `reg_n_sets[hr]` / `reg_n_deaths[hr]`. Remove
    // the counts attributed to scratch hard regs restored back to `(scratch:M)`.
    for (const auto& [hr, cnt] : restored_scratch_set_counts) {
      if (const auto it = out.sideband.reg_n_sets.find(hr); it != out.sideband.reg_n_sets.end()) {
        it->second -= cnt;
        if (it->second <= 0) {
          out.sideband.reg_n_sets.erase(it);
        }
      }
    }
    for (const auto& [hr, cnt] : restored_scratch_death_counts) {
      if (const auto it = out.sideband.reg_n_deaths.find(hr);
          it != out.sideband.reg_n_deaths.end()) {
        it->second -= cnt;
        if (it->second <= 0) {
          out.sideband.reg_n_deaths.erase(it);
        }
      }
    }
    if (out.sideband.epilogue_delay_pattern.has_value() &&
        !out.sideband.epilogue_delay_pattern->empty()) {
      if (auto ep = ParsePatternAndNotes(*out.sideband.epilogue_delay_pattern); ep.ok()) {
        RecordPseudoModesAndExplicitHardRegs(*ep, {}, out.sideband.frame_pointer_needed, gp64, fp64,
                                             &pseudo_modes, &explicit_hard_regs);
      }
    }
    for (const auto& [pseudo, hr] : lreg_cfg.sideband.reg_renumber) {
      if (pseudo >= kMipsFirstPseudoRegister && hr >= 0 && hr < kMipsFirstPseudoRegister) {
        auto m_it = pseudo_modes.find(pseudo);
        const RtlMachineMode mode =
            (m_it != pseudo_modes.end()) ? m_it->second.first : RtlMachineMode::kSi;
        const std::string raw_mode = (m_it != pseudo_modes.end()) ? m_it->second.second : "";
        const int span = HardRegSpanCount(hr, mode, raw_mode, gp64, fp64);
        for (int k = hr; k < hr + span && k < kMipsFirstPseudoRegister; ++k) {
          erased_hard_regs.insert(k);
        }
      }
    }
    if (!erased_hard_regs.empty()) {
      auto& saved_gprs = out.sideband.saved_gpr_regnos;
      saved_gprs.erase(std::remove_if(saved_gprs.begin(), saved_gprs.end(),
                                      [&](int r) {
                                        const bool callee_saved =
                                            (r >= 16 && r <= 23) || r == kFramePointerRegno;
                                        return callee_saved && erased_hard_regs.contains(r) &&
                                               !explicit_hard_regs.contains(r);
                                      }),
                       saved_gprs.end());
      auto& saved_fprs = out.sideband.saved_fpr_regnos;
      saved_fprs.erase(std::remove_if(saved_fprs.begin(), saved_fprs.end(),
                                      [&](int r) {
                                        return r >= 52 && r <= 63 && erased_hard_regs.contains(r) &&
                                               !explicit_hard_regs.contains(r);
                                      }),
                       saved_fprs.end());
    }
  }

  return out;
}

absl::StatusOr<RtlControlFlowGraph> InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg) {
  return InvertBlockAlloc(lreg_cfg, {.optimize_level = 2, .c_flags = {"-O2"}});
}

}  // namespace rom_nom_nom::lifter
