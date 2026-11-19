#include "lifter/invert_reload.h"

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
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kStackPointerRegnum = 29;
constexpr int kFramePointerRegnum = 30;
constexpr int kReturnAddressRegnum = 31;
constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;

// Non-fixed `CALL_USED_REGISTERS` eligible for `caller-save.c` on MIPS
// (`mips.h:1186-1210`): `$2..$15` (`v0..t7`), `$24..$25` (`t8..t9`), and
// `$32..$51` (`$f0..$f19`).
bool IsMipsCallerSavableHardReg(int hr) {
  return (hr >= 2 && hr <= 15) || (hr >= 24 && hr <= 25) || (hr >= 32 && hr <= 51);
}

bool IsControlFlowBoundaryPattern(const RtlExpr& pat) {
  if (IsCallPattern(pat) || pat.op == RtlOp::kCodeLabel || pat.op == RtlOp::kBarrier ||
      pat.op == RtlOp::kReturn || pat.op == RtlOp::kAddrVec || pat.op == RtlOp::kAddrDiffVec) {
    return true;
  }
  if (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (IsControlFlowBoundaryPattern(elem)) {
        return true;
      }
    }
  }
  return false;
}

bool IsFloatingPointMode(RtlMachineMode mode) {
  return mode == RtlMachineMode::kSF || mode == RtlMachineMode::kDF || mode == RtlMachineMode::kTF;
}

int EffectiveHardReg(int regno, const absl::flat_hash_map<int, int>& reg_renumber) {
  if (regno >= 0 && regno < kFirstPseudoRegister) {
    return regno;
  }
  if (regno >= kFirstPseudoRegister) {
    auto it = reg_renumber.find(regno);
    if (it != reg_renumber.end() && it->second >= 0 && it->second < kFirstPseudoRegister) {
      return it->second;
    }
  }
  return -1;
}

bool SingleElementModifiesStackPointer(const RtlExpr& expr) {
  if ((expr.op == RtlOp::kSet || expr.op == RtlOp::kClobber) && !expr.children.empty()) {
    const RtlExpr* dst = &expr.children[0];
    while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
      dst = &dst->children[0];
    }
    if (dst->IsHardReg() && dst->RegNo() == kStackPointerRegnum) {
      return true;
    }
  }
  return false;
}

bool InsnCallsAllocaPattern(const RtlExpr& pat) {
  auto check_elem = [](const RtlExpr& elem) -> bool {
    if (elem.op != RtlOp::kSet || elem.children.size() < 2) {
      return false;
    }
    const RtlExpr& dst = elem.children[0];
    const RtlExpr& src = elem.children[1];
    if (!dst.IsHardReg() || dst.RegNo() != kStackPointerRegnum) {
      return false;
    }
    if (src.op == RtlOp::kMinus && src.children.size() == 2 && src.children[0].IsHardReg() &&
        src.children[0].RegNo() == kStackPointerRegnum) {
      return true;
    }
    if (src.op == RtlOp::kPlus && src.children.size() == 2 && src.children[0].IsHardReg() &&
        src.children[0].RegNo() == kStackPointerRegnum && src.children[1].op != RtlOp::kConstInt) {
      return true;
    }
    return false;
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (check_elem(elem)) {
        return true;
      }
    }
    return false;
  }
  return check_elem(pat);
}

struct MipsReloadFrameLayout {
  bool gp64 = false;
  bool fp64 = false;
  bool single_float = false;
  bool float_store = false;
  int units_per_word = 4;
  int units_per_fpreg = 4;
  bool calls_alloca = false;
  int elim_to_reg = kStackPointerRegnum;
  int64_t starting_frame_offset = 0;
  int64_t post_reload_var_size = 0;
  int64_t gp_reg_rounded = 0;
  int64_t fp_reg_rounded = 0;
  int64_t post_reload_tsize = 0;
  bool has_incoming_arg_mem_access = false;
};

MipsReloadFrameLayout ComputeMipsReloadFrameLayout(
    const RtlControlFlowGraph& reload_cfg, const CompilationProfile& profile,
    const ParsedCompilerFlags& parsed_flags,
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks,
    const absl::flat_hash_map<int, RtlMachineMode>& pseudo_mode_map) {
  MipsReloadFrameLayout layout;
  DetectMipsRegisterWidthFlags(reload_cfg, &profile, &layout.gp64, &layout.fp64);
  layout.single_float = parsed_flags.flag_single_float;
  layout.float_store = parsed_flags.flag_float_store;
  layout.units_per_word = layout.gp64 ? 8 : 4;
  layout.units_per_fpreg = layout.fp64 ? 8 : 4;
  layout.elim_to_reg =
      reload_cfg.sideband.frame_pointer_needed ? kFramePointerRegnum : kStackPointerRegnum;
  layout.starting_frame_offset = reload_cfg.sideband.outgoing_args_size;
  // Reconstruct `compute_frame_size(get_frame_size())` (`mips.c:4534-4666`) as it
  // stood during `reload()` (`reload1.c:2958` `INITIAL_ELIMINATION_OFFSET`):
  //   - At `-O0`, upstream `InvertAlterReg` already lifted each unallocated pseudo's
  //     8-byte `$fp` `alter_reg` slot and subtracted `8` per slot from
  //     `sideband.frame_size_bytes` (`lifter/invert_alter_reg.cc:1897-1900`), whereas
  //     during `reload()` those slots were part of `get_frame_size()`. Adding back
  //     `8 * count(reg_renumber == -1)` restores the exact post-`alter_reg`
  //     `var_size` used by `eliminate_regs`.
  //   - On non-PIC N64 (`TARGET_ABICALLS == 0`), `extra_size` (`$gp` save slot) is
  //     always `0`.
  int64_t pre_lifted_unalloc_slots = 0;
  for (const auto& [regno, hr] : reload_cfg.sideband.reg_renumber) {
    if (regno >= kFirstPseudoRegister && hr == -1) {
      ++pre_lifted_unalloc_slots;
    }
  }
  layout.post_reload_var_size =
      ((std::max<int64_t>(0, reload_cfg.sideband.frame_size_bytes) + 7) & ~int64_t{7}) +
      8 * pre_lifted_unalloc_slots;

  std::array<bool, kFirstPseudoRegister> ever_live{};
  for (int gpr : reload_cfg.sideband.saved_gpr_regnos) {
    if (gpr >= 0 && gpr < 32) {
      ever_live[gpr] = true;
    }
  }
  for (int fpr : reload_cfg.sideband.saved_fpr_regnos) {
    if (fpr >= 32 && fpr < 64) {
      ever_live[fpr] = true;
    }
  }
  for (const auto& [regno, hr] : reload_cfg.sideband.reg_renumber) {
    if (regno >= kFirstPseudoRegister && hr >= 0 && hr < kFirstPseudoRegister) {
      auto mode_it = pseudo_mode_map.find(regno);
      const RtlMachineMode mode =
          (mode_it != pseudo_mode_map.end()) ? mode_it->second : RtlMachineMode::kSI;
      const int nregs = std::max(1, MipsHardRegNregs(hr, mode, layout.gp64, layout.fp64));
      for (int k = 0; k < nregs && hr + k < kFirstPseudoRegister; ++k) {
        ever_live[hr + k] = true;
      }
    }
  }

  auto mark_written_hard_regs = [&](const RtlExpr& elem) {
    if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
      const RtlExpr* dst = &elem.children[0];
      while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (dst->IsHardReg()) {
        const int hr = dst->RegNo();
        const int nregs = std::max(1, MipsHardRegNregs(hr, dst->mode, layout.gp64, layout.fp64));
        for (int k = 0; k < nregs && hr + k < kFirstPseudoRegister; ++k) {
          ever_live[hr + k] = true;
        }
      }
    }
  };

  for (const std::vector<RtlPatternAndNotes>& blk_insns : all_parsed_blocks) {
    for (const RtlPatternAndNotes& insn : blk_insns) {
      if (InsnCallsAllocaPattern(insn.pattern)) {
        layout.calls_alloca = true;
      }
      if (IsCallPattern(insn.pattern)) {
        ever_live[kReturnAddressRegnum] = true;
      }
      if (insn.pattern.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : insn.pattern.children) {
          mark_written_hard_regs(elem);
        }
      } else {
        mark_written_hard_regs(insn.pattern);
      }
    }
  }

  int64_t args_size = (std::max<int64_t>(0, layout.starting_frame_offset) + 7) & ~int64_t{7};
  if (args_size == 0 && layout.calls_alloca) {
    args_size = 4 * int64_t{layout.units_per_word};
  }

  int64_t gp_reg_size = 0;
  for (int r = 0; r < 32; ++r) {
    const bool callee_saved_gpr = (r >= 16 && r <= 23) || r == kFramePointerRegnum;
    const bool must_save = (ever_live[r] && callee_saved_gpr) ||
                           (r == kFramePointerRegnum && reload_cfg.sideband.frame_pointer_needed) ||
                           (r == kReturnAddressRegnum && ever_live[kReturnAddressRegnum]);
    if (must_save) {
      gp_reg_size += layout.units_per_word;
    }
  }

  const int fp_inc = (layout.fp64 || layout.single_float) ? 1 : 2;
  int64_t fp_reg_size = 0;
  for (int r = 32; r < 64; r += fp_inc) {
    if (r >= 52 && ever_live[r]) {
      fp_reg_size += int64_t{fp_inc} * layout.units_per_fpreg;
    }
  }

  layout.gp_reg_rounded = (gp_reg_size + 7) & ~int64_t{7};
  layout.fp_reg_rounded = (fp_reg_size + 7) & ~int64_t{7};
  layout.post_reload_tsize =
      layout.post_reload_var_size + args_size + layout.gp_reg_rounded + layout.fp_reg_rounded;

  return layout;
}

bool IsElimToRegExpr(const RtlExpr& expr, int elim_to_reg) {
  return expr.IsHardReg() && expr.mode == RtlMachineMode::kSI && !expr.flags.Any() &&
         expr.RegNo() == elim_to_reg;
}

std::optional<int64_t> ExtractElimToRegOffset(const RtlExpr& addr, int elim_to_reg) {
  if (IsElimToRegExpr(addr, elim_to_reg)) {
    return 0;
  }
  if (addr.op == RtlOp::kPlus && addr.mode == RtlMachineMode::kSI && addr.children.size() == 2) {
    if (IsElimToRegExpr(addr.children[0], elim_to_reg) && addr.children[1].op == RtlOp::kConstInt) {
      return addr.children[1].int_val;
    }
    if (addr.children[0].op == RtlOp::kPlus && addr.children[0].mode == RtlMachineMode::kSI &&
        addr.children[0].children.size() == 2 && addr.children[1].op == RtlOp::kConstInt) {
      if (IsElimToRegExpr(addr.children[0].children[0], elim_to_reg) ||
          IsElimToRegExpr(addr.children[0].children[1], elim_to_reg)) {
        return addr.children[1].int_val;
      }
    }
    if (IsElimToRegExpr(addr.children[0], elim_to_reg) ||
        IsElimToRegExpr(addr.children[1], elim_to_reg)) {
      return 0;
    }
  }
  return std::nullopt;
}

std::optional<int64_t> MatchStackSlotAddrOffset(const RtlExpr& addr, bool frame_pointer_needed,
                                                int64_t min_offset) {
  auto is_valid_base = [&](const RtlExpr& base) -> bool {
    if (!base.IsHardReg() || base.mode != RtlMachineMode::kSI || base.flags.Any()) {
      return false;
    }
    if (frame_pointer_needed) {
      return base.RegNo() == kFramePointerRegnum;
    }
    return base.RegNo() == kStackPointerRegnum || base.RegNo() == kFramePointerRegnum;
  };

  int64_t offset = -1;
  if (is_valid_base(addr)) {
    offset = 0;
  } else if (addr.op == RtlOp::kPlus && addr.mode == RtlMachineMode::kSI &&
             addr.children.size() == 2 && is_valid_base(addr.children[0]) &&
             addr.children[1].op == RtlOp::kConstInt) {
    offset = addr.children[1].int_val;
  } else {
    return std::nullopt;
  }
  if (offset < min_offset) {
    return std::nullopt;
  }
  return offset;
}

struct PseudoUseSummary {
  int def_count = 0;
  int use_count = 0;
  size_t only_block = 0;
  bool multi_block = false;
  bool used_in_special_context = false;
};

absl::flat_hash_map<int, PseudoUseSummary> BuildPseudoUseSummaries(
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks) {
  absl::flat_hash_map<int, PseudoUseSummary> summaries;

  auto record_reg = [&](int regno, size_t b, bool is_def, bool special) {
    if (regno < kFirstPseudoRegister) {
      return;
    }
    auto [it, inserted] = summaries.try_emplace(regno, PseudoUseSummary{.only_block = b});
    if (!inserted && it->second.only_block != b) {
      it->second.multi_block = true;
    }
    if (is_def) {
      ++it->second.def_count;
    } else {
      ++it->second.use_count;
    }
    if (special) {
      it->second.used_in_special_context = true;
    }
  };

  auto scan_expr_reads = [&](const RtlExpr& root, size_t b, bool special) {
    root.Visit([&](const RtlExpr& node) {
      if (node.IsPseudoReg()) {
        record_reg(node.RegNo(), b, /*is_def=*/false, special);
      }
    });
  };

  auto scan_elem = [&](const RtlExpr& elem, size_t b, bool is_call_insn) {
    if (elem.op == RtlOp::kUse || elem.op == RtlOp::kClobber) {
      const bool is_clobber_reg =
          elem.op == RtlOp::kClobber && !elem.children.empty() && elem.children[0].IsPseudoReg();
      if (is_clobber_reg) {
        record_reg(elem.children[0].RegNo(), b, /*is_def=*/true, /*special=*/true);
      } else {
        scan_expr_reads(elem, b, /*special=*/true);
      }
      return;
    }
    if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
      const RtlExpr& dst = elem.children[0];
      const RtlExpr* base_dst = &dst;
      while (base_dst->op == RtlOp::kSubreg && !base_dst->children.empty()) {
        base_dst = &base_dst->children[0];
      }
      if (base_dst->IsPseudoReg()) {
        record_reg(base_dst->RegNo(), b, /*is_def=*/true, /*special=*/is_call_insn);
        if (dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract ||
            dst.op == RtlOp::kStrictLowPart) {
          scan_expr_reads(dst, b, /*special=*/is_call_insn);
        }
      } else {
        scan_expr_reads(dst, b, /*special=*/is_call_insn);
      }
      for (size_t k = 1; k < elem.children.size(); ++k) {
        scan_expr_reads(elem.children[k], b, /*special=*/is_call_insn);
      }
      return;
    }
    scan_expr_reads(elem, b, /*special=*/is_call_insn);
  };

  for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
    for (const RtlPatternAndNotes& insn : all_parsed_blocks[b]) {
      const bool is_call = IsCallPattern(insn.pattern);
      if (insn.pattern.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : insn.pattern.children) {
          scan_elem(elem, b, is_call);
        }
      } else {
        scan_elem(insn.pattern, b, is_call);
      }
      for (const RtlExpr& fu : insn.call_fusage) {
        scan_expr_reads(fu, b, /*special=*/true);
      }
    }
  }
  return summaries;
}

bool IsSingleBlockScratchPseudo(int regno, size_t block_idx,
                                const absl::flat_hash_map<int, int>& reg_renumber,
                                const absl::flat_hash_map<int, PseudoUseSummary>& summaries) {
  if (regno < kFirstPseudoRegister || !reg_renumber.contains(regno)) {
    return false;
  }
  auto it = summaries.find(regno);
  if (it == summaries.end()) {
    return false;
  }
  return !it->second.multi_block && it->second.only_block == block_idx &&
         it->second.def_count == 1 && !it->second.used_in_special_context;
}

void RemoveDeadOrUnusedNotesForReg(std::vector<RtlRegNote>* notes, int regno) {
  if (notes == nullptr || notes->empty()) {
    return;
  }
  notes->erase(std::remove_if(notes->begin(), notes->end(),
                              [regno](const RtlRegNote& note) {
                                return (note.kind == RtlMachineMode::kRegDead ||
                                        note.kind == RtlMachineMode::kRegUnused) &&
                                       note.datum.op == RtlOp::kReg &&
                                       !note.datum.IsArgPointerReg() && note.datum.RegNo() == regno;
                              }),
               notes->end());
}

bool InsnHasOnlyOptionalDeadNoteOn(const RtlPatternAndNotes& insn, int allowed_dead_regno) {
  for (const RtlRegNote& note : insn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
        !note.datum.IsArgPointerReg() && note.datum.RegNo() == allowed_dead_regno) {
      continue;
    }
    return false;
  }
  return true;
}

bool InsnHasOnlyDeadOrUnusedNotes(const RtlPatternAndNotes& insn) {
  for (const RtlRegNote& note : insn.reg_notes) {
    if (note.kind != RtlMachineMode::kRegDead && note.kind != RtlMachineMode::kRegUnused) {
      return false;
    }
  }
  return true;
}

bool InsnWritesOrClobbersRegOrEffHr(const RtlExpr& pat,
                                    const absl::flat_hash_map<int, int>& reg_renumber, bool gp64,
                                    bool fp64, int target_regno, int target_eff_hr) {
  auto check_elem = [&](const RtlExpr& elem) -> bool {
    if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
      const RtlExpr* dst = &elem.children[0];
      while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (dst->op == RtlOp::kReg && !dst->IsArgPointerReg()) {
        const int wr = dst->RegNo();
        if (wr == target_regno) {
          return true;
        }
        if (target_eff_hr >= 0) {
          const int wr_hr = EffectiveHardReg(wr, reg_renumber);
          if (wr_hr >= 0) {
            const int nw = std::max(1, MipsHardRegNregs(wr_hr, dst->mode, gp64, fp64));
            if (target_eff_hr >= wr_hr && target_eff_hr < wr_hr + nw) {
              return true;
            }
          }
        }
      }
    }
    return false;
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (check_elem(elem)) {
        return true;
      }
    }
    return false;
  }
  return check_elem(pat);
}

// Reverses large-frame (`|offset| >= 32768`) stack-offset reloads emitted by
// `reload.c:find_reloads_address_part` / `reload1.c:gen_reload` (`li r_tmp, C`
// + `addu r_addr, r_tmp, $sp/$fp` -> `(mem (reg r_addr))`) and `addsi3`
// constant-operand reloads (`li r_tmp, C` + `addu dst, $sp/$fp, r_tmp`) back
// into `(plus:SI (reg:SI to_reg) (const_int C))` prior to 3-region stack
// address un-elimination.
void ReverseLargeStackOffsetReloadsInBlock(
    size_t block_idx, int elim_to_reg, bool gp64, bool fp64,
    const absl::flat_hash_map<int, int>& reg_renumber,
    const absl::flat_hash_map<int, PseudoUseSummary>& summaries,
    std::vector<RtlPatternAndNotes>* insns, absl::flat_hash_set<int>* reversed_reload_regs,
    absl::flat_hash_set<int>* proven_spill_regs, absl::flat_hash_set<int>* folded_scratch_pseudos) {
  if (insns == nullptr || insns->size() < 2) {
    return;
  }

  auto is_local_scratch_reg = [&](int r) -> bool {
    if (r >= 0 && r < kFirstPseudoRegister) {
      return r >= 2 && r <= 25;
    }
    return IsSingleBlockScratchPseudo(r, block_idx, reg_renumber, summaries);
  };

  std::vector<bool> drop(insns->size(), false);

  for (size_t i = 0; i + 1 < insns->size(); ++i) {
    if (drop[i]) {
      continue;
    }
    const RtlPatternAndNotes& li_insn = (*insns)[i];
    if (!InsnHasOnlyDeadOrUnusedNotes(li_insn) || li_insn.pattern.op != RtlOp::kSet ||
        li_insn.pattern.children.size() != 2) {
      continue;
    }
    const RtlExpr& li_dst = li_insn.pattern.children[0];
    const RtlExpr& li_src = li_insn.pattern.children[1];
    if (li_dst.op != RtlOp::kReg || li_dst.IsArgPointerReg() ||
        li_dst.mode != RtlMachineMode::kSI || li_src.op != RtlOp::kConstInt) {
      continue;
    }
    const int64_t big_off = li_src.int_val;
    if (big_off >= -32768 && big_off < 32768) {
      continue;
    }
    const int r_const = li_dst.RegNo();
    if (!is_local_scratch_reg(r_const)) {
      continue;
    }
    const int hr_const = EffectiveHardReg(r_const, reg_renumber);

    RtlPatternAndNotes& add_insn = (*insns)[i + 1];
    if (add_insn.pattern.op != RtlOp::kSet || add_insn.pattern.children.size() != 2) {
      continue;
    }
    const RtlExpr& add_dst = add_insn.pattern.children[0];
    const RtlExpr& add_src = add_insn.pattern.children[1];
    if (add_dst.op != RtlOp::kReg || add_dst.IsArgPointerReg() ||
        add_dst.mode != RtlMachineMode::kSI || add_src.op != RtlOp::kPlus ||
        add_src.mode != RtlMachineMode::kSI || add_src.children.size() != 2) {
      continue;
    }
    const RtlExpr& lhs = add_src.children[0];
    const RtlExpr& rhs = add_src.children[1];
    const bool const_is_lhs = lhs.op == RtlOp::kReg && !lhs.IsArgPointerReg() &&
                              lhs.RegNo() == r_const && IsElimToRegExpr(rhs, elim_to_reg);
    const bool const_is_rhs = IsElimToRegExpr(lhs, elim_to_reg) && rhs.op == RtlOp::kReg &&
                              !rhs.IsArgPointerReg() && rhs.RegNo() == r_const;
    if (!const_is_lhs && !const_is_rhs) {
      continue;
    }

    // Check that `r_const` is not referenced anywhere after `i + 1` in this block.
    bool r_const_used_later = false;
    if (r_const >= kFirstPseudoRegister) {
      auto sum_it = summaries.find(r_const);
      if (sum_it == summaries.end() || sum_it->second.use_count != 1) {
        r_const_used_later = true;
      }
    } else {
      for (size_t k = i + 2; k < insns->size(); ++k) {
        if ((*insns)[k].pattern.ContainsReg(r_const)) {
          r_const_used_later = true;
          break;
        }
        if (InsnWritesOrClobbersRegOrEffHr((*insns)[k].pattern, reg_renumber, gp64, fp64, r_const,
                                           hr_const)) {
          break;
        }
      }
    }
    if (r_const_used_later) {
      continue;
    }

    const int r_addr = add_dst.RegNo();
    const int hr_addr = EffectiveHardReg(r_addr, reg_renumber);

    // Form 1: `gen_reload` address reload (`hr_addr == hr_const`) feeding
    // `(mem:M (reg:SI r_addr))` in subsequent instruction(s).
    if (hr_addr >= 0 && hr_addr == hr_const && is_local_scratch_reg(r_addr) &&
        InsnHasOnlyDeadOrUnusedNotes(add_insn) && i + 2 < insns->size()) {
      bool valid_mem_only = true;
      int mem_replacements = 0;
      size_t end_use = i + 1;
      for (size_t k = i + 2; k < insns->size(); ++k) {
        const RtlExpr& pat = (*insns)[k].pattern;
        int total_reg_hits = 0;
        int mem_addr_hits = 0;
        pat.Visit([&](const RtlExpr& node) {
          if (node.op == RtlOp::kReg && !node.IsArgPointerReg() && node.RegNo() == r_addr) {
            ++total_reg_hits;
          }
          if (node.op == RtlOp::kMem && node.children.size() == 1) {
            const RtlExpr& maddr = node.children[0];
            if (maddr.op == RtlOp::kReg && !maddr.IsArgPointerReg() && maddr.RegNo() == r_addr &&
                maddr.mode == RtlMachineMode::kSI) {
              ++mem_addr_hits;
            }
          }
        });
        if (total_reg_hits > 0) {
          // If `r_addr` is also the `SET_DEST` of `k` (overwriting `r_addr`),
          // subtract that write hit.
          int write_hits = 0;
          if (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kReg &&
              !pat.children[0].IsArgPointerReg() && pat.children[0].RegNo() == r_addr) {
            write_hits = 1;
          }
          if (total_reg_hits - write_hits != mem_addr_hits || mem_addr_hits == 0) {
            valid_mem_only = false;
            break;
          }
          mem_replacements += mem_addr_hits;
          end_use = k;
        }
        if (InsnWritesOrClobbersRegOrEffHr(pat, reg_renumber, gp64, fp64, r_addr, hr_addr) ||
            IsControlFlowBoundaryPattern(pat)) {
          break;
        }
      }

      if (r_addr >= kFirstPseudoRegister) {
        auto sum_it = summaries.find(r_addr);
        if (sum_it == summaries.end() || sum_it->second.use_count != mem_replacements) {
          valid_mem_only = false;
        }
      }

      if (valid_mem_only && mem_replacements > 0) {
        const RtlExpr stack_plus = RtlExpr::MakeBinary(
            RtlOp::kPlus, RtlMachineMode::kSI, RtlExpr::MakeReg(RtlMachineMode::kSI, elim_to_reg),
            RtlExpr::MakeConstInt(big_off));
        bool consumer_writes_hr_addr = false;
        for (size_t k = i + 2; k <= end_use; ++k) {
          if (hr_addr >= 0 &&
              InsnWritesOrClobbersRegOrEffHr((*insns)[k].pattern, reg_renumber, gp64, fp64,
                                             /*target_regno=*/-1, hr_addr)) {
            consumer_writes_hr_addr = true;
          }
          (*insns)[k].pattern.Mutate([&](RtlExpr& node) {
            if (node.op == RtlOp::kMem && node.children.size() == 1 &&
                node.children[0].op == RtlOp::kReg && !node.children[0].IsArgPointerReg() &&
                node.children[0].RegNo() == r_addr &&
                node.children[0].mode == RtlMachineMode::kSI) {
              node.children[0] = stack_plus;
            }
          });
          RemoveDeadOrUnusedNotesForReg(&(*insns)[k].reg_notes, r_addr);
        }
        drop[i] = true;
        drop[i + 1] = true;
        if (reversed_reload_regs != nullptr && hr_addr >= 0) {
          reversed_reload_regs->insert(hr_addr);
        }
        if (proven_spill_regs != nullptr && hr_addr >= 0 && !consumer_writes_hr_addr) {
          proven_spill_regs->insert(hr_addr);
        }
        if (folded_scratch_pseudos != nullptr) {
          if (r_const >= kFirstPseudoRegister) folded_scratch_pseudos->insert(r_const);
          if (r_addr >= kFirstPseudoRegister) folded_scratch_pseudos->insert(r_addr);
        }
        ++i;
        continue;
      }
    }

    // Form 2: `addsi3` constant-operand reload `(set dst (plus to_reg r_const))`.
    if (const_is_rhs) {
      const int add_dst_hr = EffectiveHardReg(add_dst.RegNo(), reg_renumber);
      add_insn.pattern.children[1] = RtlExpr::MakeBinary(
          RtlOp::kPlus, RtlMachineMode::kSI, RtlExpr::MakeReg(RtlMachineMode::kSI, elim_to_reg),
          RtlExpr::MakeConstInt(big_off));
      RemoveDeadOrUnusedNotesForReg(&add_insn.reg_notes, r_const);
      drop[i] = true;
      if (reversed_reload_regs != nullptr && hr_const >= 0) {
        reversed_reload_regs->insert(hr_const);
      }
      if (proven_spill_regs != nullptr && hr_const >= 0 && add_dst_hr != hr_const) {
        proven_spill_regs->insert(hr_const);
      }
      if (folded_scratch_pseudos != nullptr && r_const >= kFirstPseudoRegister) {
        folded_scratch_pseudos->insert(r_const);
      }
    }
  }

  std::vector<RtlPatternAndNotes> out;
  out.reserve(insns->size());
  for (size_t i = 0; i < insns->size(); ++i) {
    if (!drop[i]) {
      out.push_back(std::move((*insns)[i]));
    }
  }
  *insns = std::move(out);
}

std::optional<int64_t> MatchCallerSaveStackSlotOffset(const RtlExpr& mem_expr,
                                                      bool frame_pointer_needed,
                                                      int64_t outgoing_args_size,
                                                      int64_t max_frame_top) {
  if (mem_expr.op != RtlOp::kMem || mem_expr.flags.Any() || mem_expr.children.size() != 1) {
    return std::nullopt;
  }
  std::optional<int64_t> off =
      MatchStackSlotAddrOffset(mem_expr.children[0], frame_pointer_needed, outgoing_args_size);
  if (!off.has_value() || *off >= max_frame_top) {
    return std::nullopt;
  }
  return off;
}

struct CallerSaveHardSaveLift {
  size_t save_insn_idx = 0;
  size_t call_insn_idx = 0;
  int hard_regno = -1;
  int target_pseudo = -1;
  RtlMachineMode mode = RtlMachineMode::kSI;
  std::vector<size_t> hard_restore_indices;
};

struct CallerSaveHardRestoreMove {
  size_t restore_insn_idx = 0;
  int hard_regno = -1;
  int src_pseudo = -1;
  RtlMachineMode mode = RtlMachineMode::kSI;
};

struct CallerSaveBlockPlan {
  std::vector<bool> drop_insn;
  std::vector<bool> replaced_caller_save_insn;
  std::vector<std::pair<int, int>> pseudo_unions;  // (from_rest_pseudo, to_save_pseudo)
  std::vector<CallerSaveHardSaveLift> hard_save_lifts;
  std::vector<CallerSaveHardRestoreMove> hard_restore_moves;
  absl::flat_hash_map<int, std::pair<int64_t, int>> hr_slot_bounds;  // hr -> (offset, byte_size)
  bool conflict = false;
};

CallerSaveBlockPlan PlanCallerSaveInBlock(size_t block_idx,
                                          const std::vector<RtlPatternAndNotes>& insns,
                                          const absl::flat_hash_set<int>& rtl_cross_call_hrs,
                                          bool restrict_allowed_hrs,
                                          const absl::flat_hash_set<int>& allowed_hrs,
                                          const absl::flat_hash_map<int, int>& reg_renumber,
                                          bool frame_pointer_needed, bool gp64, bool fp64,
                                          int64_t outgoing_args_size, int64_t max_frame_top) {
  CallerSaveBlockPlan plan;
  plan.drop_insn.assign(insns.size(), false);
  plan.replaced_caller_save_insn.assign(insns.size(), false);
  if (insns.empty()) {
    return plan;
  }

  std::vector<size_t> call_indices;
  for (size_t i = 0; i < insns.size(); ++i) {
    if (IsCallPattern(insns[i].pattern)) {
      call_indices.push_back(i);
    }
  }
  if (call_indices.empty()) {
    return plan;
  }

  auto can_lift_pre_call_hard_save = [&](size_t save_idx, int hr, int nregs,
                                         RtlMachineMode mode) -> bool {
    const absl::flat_hash_map<int, int> empty_renum;
    for (int idx = static_cast<int>(save_idx) - 1; idx >= 0; --idx) {
      const RtlPatternAndNotes& prev = insns[static_cast<size_t>(idx)];
      bool writes_range = false;
      for (int k = 0; k < nregs; ++k) {
        if (InsnWritesOrClobbersRegOrEffHr(prev.pattern, empty_renum, gp64, fp64, hr + k, hr + k)) {
          writes_range = true;
          break;
        }
      }
      if (writes_range) {
        if (IsCallPattern(prev.pattern)) {
          return true;
        }
        return prev.pattern.IsSet() && prev.pattern.SetDest().IsHardReg() &&
               prev.pattern.SetDest().RegNo() == hr && prev.pattern.SetDest().mode == mode;
      }
      if (IsCallPattern(prev.pattern) || IsControlFlowBoundaryPattern(prev.pattern)) {
        return false;
      }
    }
    return block_idx == 0;
  };

  // When `caller-save.c:insert_save_restore` saves and restores a hard register
  // carrying a flag-bearing pseudo (`reg/v`, `reg/s`, or `reg/u`), its emitted
  // `regno_reg_rtx[hr]` operands carry empty `RtlFlags`, causing `InvertAlterReg`
  // to pin both the pre-call and post-call webs on hard register `hr`. Verify
  // that both sides can be cleanly lifted to a single cross-call pseudo.
  auto can_lift_flagged_hard_save_and_restore = [&](size_t save_idx, size_t restore_idx, int hr,
                                                    int nregs, RtlMachineMode mode) -> bool {
    if (!can_lift_pre_call_hard_save(save_idx, hr, nregs, mode)) {
      return false;
    }
    const absl::flat_hash_map<int, int> empty_renum;
    auto check_read_expr = [&](const RtlExpr& root, bool* saw_read, bool* saw_flagged) -> bool {
      bool ok = true;
      auto inspect_elem = [&](const RtlExpr& elem) {
        if (elem.op == RtlOp::kUse || elem.op == RtlOp::kClobber) {
          return;
        }
        auto visit_node = [&](const RtlExpr& node) {
          if (!ok || !node.IsHardReg()) {
            return;
          }
          const int r = node.RegNo();
          const int nw = std::max(1, MipsHardRegNregs(r, node.mode, gp64, fp64));
          if (r < hr + nregs && hr < r + nw) {
            if (r != hr || node.mode != mode) {
              ok = false;
              return;
            }
            *saw_read = true;
            if (node.flags.volatil || node.flags.in_struct || node.flags.unchanging) {
              *saw_flagged = true;
            }
          }
        };
        if (elem.op == RtlOp::kSet && !elem.children.empty()) {
          const RtlExpr& dst = elem.children[0];
          if (dst.op == RtlOp::kMem) {
            dst.Visit(visit_node);
          } else if ((dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract) &&
                     dst.children.size() >= 3) {
            for (size_t k = 1; k < dst.children.size(); ++k) {
              dst.children[k].Visit(visit_node);
            }
          }
          for (size_t k = 1; k < elem.children.size(); ++k) {
            elem.children[k].Visit(visit_node);
          }
          return;
        }
        elem.Visit(visit_node);
      };
      if (root.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : root.children) {
          inspect_elem(elem);
        }
      } else {
        inspect_elem(root);
      }
      return ok;
    };
    auto touches_hr_range = [&](const RtlExpr& root) -> bool {
      bool found = false;
      root.Visit([&](const RtlExpr& node) {
        if (node.IsHardReg()) {
          const int r = node.RegNo();
          const int nw = std::max(1, MipsHardRegNregs(r, node.mode, gp64, fp64));
          if (r < hr + nregs && hr < r + nw) {
            found = true;
          }
        }
      });
      return found;
    };

    bool saw_flagged = false;
    bool unused_pre_read = false;
    for (int idx = static_cast<int>(save_idx) - 1; idx >= 0; --idx) {
      const RtlPatternAndNotes& prev = insns[static_cast<size_t>(idx)];
      for (const RtlExpr& fu : prev.call_fusage) {
        if (touches_hr_range(fu)) {
          return false;
        }
      }
      bool writes_range = false;
      for (int k = 0; k < nregs; ++k) {
        if (InsnWritesOrClobbersRegOrEffHr(prev.pattern, empty_renum, gp64, fp64, hr + k, hr + k)) {
          writes_range = true;
          break;
        }
      }
      if (writes_range) {
        if (prev.pattern.IsSet() && prev.pattern.SetDest().IsHardReg() &&
            prev.pattern.SetDest().RegNo() == hr && prev.pattern.SetDest().mode == mode) {
          const RtlFlags& df = prev.pattern.SetDest().flags;
          if (df.volatil || df.in_struct || df.unchanging) {
            saw_flagged = true;
          }
        }
        break;
      }
      if (!check_read_expr(prev.pattern, &unused_pre_read, &saw_flagged)) {
        return false;
      }
    }

    bool saw_post_read = false;
    bool terminated = false;
    for (size_t m = restore_idx + 1; m < insns.size(); ++m) {
      const RtlPatternAndNotes& post = insns[m];
      if (post.pattern.op == RtlOp::kUse && touches_hr_range(post.pattern)) {
        return false;
      }
      for (const RtlExpr& fu : post.call_fusage) {
        if (touches_hr_range(fu)) {
          return false;
        }
      }
      if (!check_read_expr(post.pattern, &saw_post_read, &saw_flagged)) {
        return false;
      }
      int writes_count = 0;
      for (int k = 0; k < nregs; ++k) {
        if (InsnWritesOrClobbersRegOrEffHr(post.pattern, empty_renum, gp64, fp64, hr + k, hr + k)) {
          ++writes_count;
        }
      }
      if (writes_count == nregs) {
        terminated = true;
        break;
      }
      if (writes_count > 0) {
        return false;
      }
      for (const RtlRegNote& note : post.reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg() &&
            note.datum.RegNo() == hr && note.datum.mode == mode) {
          terminated = true;
          break;
        }
      }
      if (terminated || IsControlFlowBoundaryPattern(post.pattern)) {
        break;
      }
    }
    return saw_post_read && saw_flagged && terminated;
  };

  struct SaveWordInfo {
    size_t save_insn_idx = 0;
    size_t call_insn_idx = 0;
    int save_regno = -1;
    int total_words = 1;
    int word_bit = 0;
    int word_index = 0;
    int64_t offset = 0;
    int word_bytes = 4;
  };

  absl::flat_hash_map<int, SaveWordInfo> saved_in_slot;
  absl::flat_hash_map<int, SaveWordInfo> needs_restore;
  absl::flat_hash_map<size_t, uint32_t> restored_word_mask_per_save;
  absl::flat_hash_map<size_t, int> expected_words_per_save;
  absl::flat_hash_map<size_t, std::vector<size_t>> restore_indices_per_save;
  absl::flat_hash_map<size_t, std::vector<std::pair<int, int>>> unions_per_save;
  absl::flat_hash_map<size_t, CallerSaveHardSaveLift> hard_save_lift_per_save;
  absl::flat_hash_map<size_t, std::vector<CallerSaveHardRestoreMove>> hard_restore_moves_per_save;
  absl::flat_hash_map<size_t, std::vector<std::pair<int, std::pair<int64_t, int>>>>
      save_word_bounds;

  std::vector<std::pair<size_t, std::vector<SaveWordInfo>>> pending_saves_before_call;

  for (size_t i = 0; i < insns.size(); ++i) {
    const RtlPatternAndNotes& insn = insns[i];
    if (IsCallPattern(insn.pattern)) {
      for (auto& [_, words] : pending_saves_before_call) {
        for (SaveWordInfo& winfo : words) {
          winfo.call_insn_idx = i;
          saved_in_slot[winfo.word_index] = winfo;
        }
      }
      pending_saves_before_call.clear();
      needs_restore = saved_in_slot;
      continue;
    }

    if (insn.pattern.op == RtlOp::kSet && insn.pattern.children.size() == 2) {
      const RtlExpr& dst = insn.pattern.children[0];
      const RtlExpr& src = insn.pattern.children[1];

      // Candidate restore after a call: `(set (reg:M r_rest) (mem:M [sp + off]))`.
      if (dst.op == RtlOp::kReg && !dst.IsArgPointerReg() && !dst.flags.Any() &&
          dst.mode == src.mode && insn.reg_notes.empty()) {
        const int r_rest = dst.RegNo();
        const int hr = EffectiveHardReg(r_rest, reg_renumber);
        const int nregs = (hr >= 0) ? std::max(1, MipsHardRegNregs(hr, dst.mode, gp64, fp64)) : 0;
        const int mode_bytes = MipsModeByteSize(dst.mode);
        const int word_bytes = (nregs > 0) ? std::max(1, mode_bytes / nregs) : 4;
        std::optional<int64_t> off = MatchCallerSaveStackSlotOffset(
            src, frame_pointer_needed, outgoing_args_size, max_frame_top);
        if (hr >= 0 && off.has_value() && needs_restore.contains(hr)) {
          bool all_match = true;
          const size_t save_idx = needs_restore[hr].save_insn_idx;
          const size_t call_idx = needs_restore[hr].call_insn_idx;
          const int save_regno = needs_restore[hr].save_regno;
          const bool save_is_pseudo = save_regno >= kFirstPseudoRegister;
          const bool rest_is_pseudo = r_rest >= kFirstPseudoRegister;
          const bool hr_already_crosses_call = rtl_cross_call_hrs.contains(hr);
          bool lift_both_hard_webs = false;
          if (!save_is_pseudo && !rest_is_pseudo) {
            if (!hr_already_crosses_call) {
              if (nregs == needs_restore[hr].total_words &&
                  can_lift_flagged_hard_save_and_restore(save_idx, i, hr, nregs, dst.mode)) {
                lift_both_hard_webs = true;
              } else {
                all_match = false;
              }
            }
          } else if (save_is_pseudo != rest_is_pseudo) {
            if (hr_already_crosses_call || nregs != needs_restore[hr].total_words) {
              all_match = false;
            } else if (!save_is_pseudo && rest_is_pseudo &&
                       !can_lift_pre_call_hard_save(save_idx, hr, nregs, dst.mode)) {
              all_match = false;
            }
          } else if (hr_already_crosses_call) {
            all_match = false;
          }
          for (int k = 0; k < nregs && all_match; ++k) {
            auto it = needs_restore.find(hr + k);
            if (it == needs_restore.end() || it->second.save_insn_idx != save_idx ||
                it->second.offset != *off + int64_t{word_bytes} * k) {
              all_match = false;
            }
          }
          if (all_match) {
            expected_words_per_save[save_idx] = needs_restore[hr].total_words;
            for (int k = 0; k < nregs; ++k) {
              restored_word_mask_per_save[save_idx] |=
                  (uint32_t{1} << needs_restore[hr + k].word_bit);
              needs_restore.erase(hr + k);
            }
            if (save_is_pseudo && rest_is_pseudo) {
              restore_indices_per_save[save_idx].push_back(i);
              if (r_rest != save_regno) {
                unions_per_save[save_idx].emplace_back(r_rest, save_regno);
              }
            } else if (!save_is_pseudo && rest_is_pseudo) {
              restore_indices_per_save[save_idx].push_back(i);
              auto [lift_it, inserted] =
                  hard_save_lift_per_save.try_emplace(save_idx, CallerSaveHardSaveLift{
                                                                    .save_insn_idx = save_idx,
                                                                    .call_insn_idx = call_idx,
                                                                    .hard_regno = save_regno,
                                                                    .target_pseudo = r_rest,
                                                                    .mode = dst.mode,
                                                                });
              if (!inserted && r_rest != lift_it->second.target_pseudo) {
                unions_per_save[save_idx].emplace_back(r_rest, lift_it->second.target_pseudo);
              }
            } else if (save_is_pseudo && !rest_is_pseudo) {
              hard_restore_moves_per_save[save_idx].push_back(CallerSaveHardRestoreMove{
                  .restore_insn_idx = i,
                  .hard_regno = r_rest,
                  .src_pseudo = save_regno,
                  .mode = dst.mode,
              });
            } else if (lift_both_hard_webs) {
              restore_indices_per_save[save_idx].push_back(i);
              auto [lift_it, _] =
                  hard_save_lift_per_save.try_emplace(save_idx, CallerSaveHardSaveLift{
                                                                    .save_insn_idx = save_idx,
                                                                    .call_insn_idx = call_idx,
                                                                    .hard_regno = save_regno,
                                                                    .target_pseudo = -1,
                                                                    .mode = dst.mode,
                                                                });
              lift_it->second.hard_restore_indices.push_back(i);
            } else {
              restore_indices_per_save[save_idx].push_back(i);
            }
            continue;
          }
        }
      }

      // Candidate save before an upcoming call: `(set (mem:M [sp + off]) (reg:M r_save))`.
      if (src.op == RtlOp::kReg && !src.IsArgPointerReg() && !src.flags.Any() &&
          dst.mode == src.mode && i < call_indices.back() &&
          InsnHasOnlyOptionalDeadNoteOn(insn, src.RegNo())) {
        const int r_save = src.RegNo();
        const bool is_pseudo = r_save >= kFirstPseudoRegister;
        const int hr = EffectiveHardReg(r_save, reg_renumber);
        if (hr >= 0 && (!is_pseudo || !rtl_cross_call_hrs.contains(hr))) {
          const int nregs = std::max(1, MipsHardRegNregs(hr, src.mode, gp64, fp64));
          const int mode_bytes = MipsModeByteSize(src.mode);
          const int word_bytes = (nregs > 0) ? std::max(1, mode_bytes / nregs) : 4;
          std::optional<int64_t> off = MatchCallerSaveStackSlotOffset(
              dst, frame_pointer_needed, outgoing_args_size, max_frame_top);
          if (off.has_value() && *off + mode_bytes <= max_frame_top) {
            bool all_cand = true;
            std::vector<SaveWordInfo> words;
            std::vector<std::pair<int, std::pair<int64_t, int>>> bounds;
            for (int k = 0; k < nregs; ++k) {
              if (!IsMipsCallerSavableHardReg(hr + k) ||
                  (restrict_allowed_hrs && !allowed_hrs.contains(hr + k)) ||
                  (!is_pseudo && !rtl_cross_call_hrs.contains(hr + k) && nregs > 1)) {
                all_cand = false;
                break;
              }
              const int64_t w_off = *off + int64_t{word_bytes} * k;
              words.push_back(SaveWordInfo{
                  .save_insn_idx = i,
                  .save_regno = r_save,
                  .total_words = nregs,
                  .word_bit = k,
                  .word_index = hr + k,
                  .offset = w_off,
                  .word_bytes = word_bytes,
              });
              bounds.emplace_back(hr + k, std::make_pair(w_off, word_bytes));
            }
            if (all_cand) {
              save_word_bounds[i] = std::move(bounds);
              pending_saves_before_call.emplace_back(i, std::move(words));
              continue;
            }
          }
        }
      }
    }

    // Any non-save/non-restore instruction writing to a hard register invalidates
    // `saved_in_slot`, `needs_restore`, and `pending_saves_before_call` (`caller-save.c:536-541`).
    auto invalidate_written_reg = [&](const RtlExpr& elem) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
        const RtlExpr* dst = &elem.children[0];
        while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
          dst = &dst->children[0];
        }
        if (dst->op == RtlOp::kReg && !dst->IsArgPointerReg()) {
          const int wr_hr = EffectiveHardReg(dst->RegNo(), reg_renumber);
          if (wr_hr >= 0) {
            const int nw = std::max(1, MipsHardRegNregs(wr_hr, dst->mode, gp64, fp64));
            for (int k = 0; k < nw; ++k) {
              saved_in_slot.erase(wr_hr + k);
              needs_restore.erase(wr_hr + k);
              pending_saves_before_call.erase(
                  std::remove_if(pending_saves_before_call.begin(), pending_saves_before_call.end(),
                                 [&](const auto& entry) {
                                   for (const SaveWordInfo& winfo : entry.second) {
                                     if (winfo.word_index == wr_hr + k) {
                                       return true;
                                     }
                                   }
                                   return false;
                                 }),
                  pending_saves_before_call.end());
            }
          }
        }
      }
    };
    if (insn.pattern.op == RtlOp::kParallel) {
      for (const RtlExpr& elem : insn.pattern.children) {
        invalidate_written_reg(elem);
      }
    } else {
      invalidate_written_reg(insn.pattern);
    }
  }

  for (const auto& [save_idx, mask] : restored_word_mask_per_save) {
    const int expected_w = expected_words_per_save[save_idx];
    const uint32_t full_mask =
        (expected_w >= 32) ? ~uint32_t{0} : ((uint32_t{1} << expected_w) - 1);
    if (mask != full_mask) {
      continue;
    }
    auto lift_it = hard_save_lift_per_save.find(save_idx);
    if (lift_it != hard_save_lift_per_save.end()) {
      plan.replaced_caller_save_insn[save_idx] = true;
      plan.hard_save_lifts.push_back(lift_it->second);
    } else {
      plan.drop_insn[save_idx] = true;
    }
    for (size_t r_idx : restore_indices_per_save[save_idx]) {
      plan.drop_insn[r_idx] = true;
    }
    auto hrm_it = hard_restore_moves_per_save.find(save_idx);
    if (hrm_it != hard_restore_moves_per_save.end()) {
      for (const CallerSaveHardRestoreMove& hrm : hrm_it->second) {
        plan.replaced_caller_save_insn[hrm.restore_insn_idx] = true;
        plan.hard_restore_moves.push_back(hrm);
      }
    }
    for (const auto& u : unions_per_save[save_idx]) {
      plan.pseudo_unions.push_back(u);
    }
    for (const auto& [hr, bound] : save_word_bounds[save_idx]) {
      auto [it, inserted] = plan.hr_slot_bounds.try_emplace(hr, bound);
      if (!inserted && it->second != bound) {
        plan.conflict = true;
      }
    }
  }

  return plan;
}

bool ExprOverlapsByteInterval(const RtlExpr& root, bool frame_pointer_needed, int elim_to_reg,
                              int64_t interval_lo, int64_t interval_hi) {
  bool overlaps = false;
  auto walk = [&](auto& self, const RtlExpr& expr, bool inside_mem_addr) -> void {
    if (overlaps) {
      return;
    }
    if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
      std::optional<int64_t> off =
          MatchStackSlotAddrOffset(expr.children[0], frame_pointer_needed, /*min_offset=*/0);
      if (off.has_value()) {
        const int sz = std::max(1, MipsModeByteSize(expr.mode));
        if (*off<interval_hi&& * off + sz> interval_lo) {
          overlaps = true;
          return;
        }
        // Stack slot `MEM` handled directly; do not treat its own base+offset
        // child as an outside-`MEM` address-taken reference.
        return;
      }
      self(self, expr.children[0], /*inside_mem_addr=*/true);
      return;
    }
    if (!inside_mem_addr) {
      std::optional<int64_t> addr_off = ExtractElimToRegOffset(expr, elim_to_reg);
      if (addr_off.has_value() && *addr_off >= interval_lo && *addr_off < interval_hi) {
        overlaps = true;
        return;
      }
    }
    for (const RtlExpr& child : expr.children) {
      self(self, child, inside_mem_addr);
    }
  };
  walk(walk, root, /*inside_mem_addr=*/false);
  return overlaps;
}

struct CallerSaveReversalOutcome {
  bool stripped_any = false;
  int64_t pre_caller_save_top = 0;
  absl::flat_hash_set<int> unified_pseudos;
};

CallerSaveReversalOutcome ReverseCallerSaveAcrossCfg(
    const ParsedCompilerFlags& parsed_flags, const MipsReloadFrameLayout& layout,
    bool frame_pointer_needed, int frame_size_bytes,
    const absl::flat_hash_set<int>& rtl_cross_call_hrs, int* max_existing_regno,
    absl::flat_hash_map<int, RtlMachineMode>* pseudo_widest_mode,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
    absl::flat_hash_map<int, int>* reg_renumber) {
  CallerSaveReversalOutcome outcome;
  const int64_t max_frame_top = layout.starting_frame_offset + std::max(0, frame_size_bytes);
  outcome.pre_caller_save_top = max_frame_top;

  const bool caller_save_enabled =
      !rtl_cross_call_hrs.empty() || (parsed_flags.opt_level > 0 && parsed_flags.flag_caller_saves);
  if (!caller_save_enabled || frame_size_bytes <= 0) {
    return outcome;
  }

  std::vector<CallerSaveBlockPlan> block_plans(all_parsed_blocks->size());
  absl::flat_hash_map<int, std::pair<int64_t, int>> global_hr_bounds;
  bool any_candidate = false;
  const absl::flat_hash_set<int> empty_allowed;

  for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
    block_plans[b] = PlanCallerSaveInBlock(
        b, (*all_parsed_blocks)[b], rtl_cross_call_hrs, /*restrict_allowed_hrs=*/false,
        empty_allowed, *reg_renumber, frame_pointer_needed, layout.gp64, layout.fp64,
        layout.starting_frame_offset, max_frame_top);
    if (block_plans[b].conflict) {
      return outcome;
    }
    for (const auto& [hr, bound] : block_plans[b].hr_slot_bounds) {
      any_candidate = true;
      auto [it, inserted] = global_hr_bounds.try_emplace(hr, bound);
      if (!inserted && it->second != bound) {
        return outcome;
      }
    }
  }

  if (!any_candidate) {
    if (parsed_flags.opt_level > 0 && parsed_flags.flag_caller_saves) {
      int64_t silent_cs_bytes = 0;
      for (int cand_hr : rtl_cross_call_hrs) {
        silent_cs_bytes += (cand_hr < 32) ? layout.units_per_word : layout.units_per_fpreg;
      }
      int64_t pre_cs_top = max_frame_top;
      if (silent_cs_bytes > 0) {
        const int64_t rel_est = (max_frame_top - silent_cs_bytes) - layout.starting_frame_offset;
        if (rel_est >= 0) {
          pre_cs_top = layout.starting_frame_offset + (rel_est & ~int64_t{7});
        }
      } else {
        // In `reload1.c:663`, `assign_stack_local(BLKmode, 0, 0)` rounds
        // `frame_offset` to an 8-byte boundary before `setup_save_areas()`, and
        // `setup_save_areas()` (`caller-save.c:315`, `align = 0`) is the only
        // allocator in `reload()` that can advance `frame_offset` by an odd
        // 4-byte GPR slot. Thus any unaccessed trailing 4-byte word at
        // `(max_frame_top - starting_frame_offset) % 8 == 4` is structurally an
        // unused iteration-0 `setup_save_areas()` slot.
        const int64_t rel_top = max_frame_top - layout.starting_frame_offset;
        if (rel_top >= 4 && (rel_top % 8) == 4) {
          pre_cs_top = max_frame_top - 4;
        }
      }
      if (pre_cs_top < max_frame_top) {
        bool overlaps = false;
        for (size_t b = 0; b < all_parsed_blocks->size() && !overlaps; ++b) {
          for (const RtlPatternAndNotes& insn : (*all_parsed_blocks)[b]) {
            if (ExprOverlapsByteInterval(insn.pattern, frame_pointer_needed, layout.elim_to_reg,
                                         pre_cs_top, max_frame_top)) {
              overlaps = true;
              break;
            }
            for (const RtlRegNote& note : insn.reg_notes) {
              if (ExprOverlapsByteInterval(note.datum, frame_pointer_needed, layout.elim_to_reg,
                                           pre_cs_top, max_frame_top)) {
                overlaps = true;
                break;
              }
            }
            if (overlaps) {
              break;
            }
          }
        }
        if (!overlaps) {
          outcome.pre_caller_save_top = pre_cs_top;
          outcome.stripped_any = true;
        }
      }
    }
    return outcome;
  }

  // Filter out any candidate `hr` whose stack interval `[off, off + sz)` is
  // touched by ANY non-dropped instruction or ANY `REG_NOTE` anywhere in the
  // CFG (`caller-save` never places `MEM` in `REG_NOTES`; only `alter_reg`
  // rewrites `REG_DEAD` / `REG_UNUSED` in-place to `MEM`).
  absl::flat_hash_set<int> invalid_hrs;
  for (const auto& [hr, bound] : global_hr_bounds) {
    const int64_t lo = bound.first;
    const int64_t hi = bound.first + bound.second;
    for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
      const auto& blk_insns = (*all_parsed_blocks)[b];
      for (size_t i = 0; i < blk_insns.size(); ++i) {
        for (const RtlRegNote& note : blk_insns[i].reg_notes) {
          if (ExprOverlapsByteInterval(note.datum, frame_pointer_needed, layout.elim_to_reg, lo,
                                       hi)) {
            invalid_hrs.insert(hr);
            break;
          }
        }
        if (invalid_hrs.contains(hr)) {
          break;
        }
        if (block_plans[b].drop_insn[i] || block_plans[b].replaced_caller_save_insn[i]) {
          continue;
        }
        if (ExprOverlapsByteInterval(blk_insns[i].pattern, frame_pointer_needed, layout.elim_to_reg,
                                     lo, hi)) {
          invalid_hrs.insert(hr);
          break;
        }
      }
      if (invalid_hrs.contains(hr)) {
        break;
      }
    }
  }

  if (!invalid_hrs.empty()) {
    // Re-plan excluding `invalid_hrs` if any existed.
    if (invalid_hrs.size() == global_hr_bounds.size()) {
      return outcome;
    }
    absl::flat_hash_set<int> allowed_hrs;
    for (const auto& [hr, _] : global_hr_bounds) {
      if (!invalid_hrs.contains(hr)) {
        allowed_hrs.insert(hr);
      }
    }
    global_hr_bounds.clear();
    for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
      block_plans[b] = PlanCallerSaveInBlock(
          b, (*all_parsed_blocks)[b], rtl_cross_call_hrs, /*restrict_allowed_hrs=*/true,
          allowed_hrs, *reg_renumber, frame_pointer_needed, layout.gp64, layout.fp64,
          layout.starting_frame_offset, max_frame_top);
      for (const auto& [hr, bound] : block_plans[b].hr_slot_bounds) {
        global_hr_bounds.try_emplace(hr, bound);
      }
    }
    if (global_hr_bounds.empty()) {
      return outcome;
    }
  }

  // Verify `setup_save_areas` (`caller-save.c:282-331`) monotonic hard-register
  // to stack-offset ordering (`hr_a < hr_b => off_a < off_b`).
  std::vector<std::pair<int, std::pair<int64_t, int>>> sorted_hr_bounds(global_hr_bounds.begin(),
                                                                        global_hr_bounds.end());
  std::sort(sorted_hr_bounds.begin(), sorted_hr_bounds.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t k = 1; k < sorted_hr_bounds.size(); ++k) {
    const int64_t prev_end =
        sorted_hr_bounds[k - 1].second.first + sorted_hr_bounds[k - 1].second.second;
    if (sorted_hr_bounds[k].second.first < prev_end) {
      return outcome;
    }
  }

  const int min_hr = sorted_hr_bounds.front().first;
  const int64_t min_off = sorted_hr_bounds.front().second.first;
  int64_t earlier_bytes = 0;
  for (int cand_hr : rtl_cross_call_hrs) {
    if (cand_hr < min_hr && !global_hr_bounds.contains(cand_hr)) {
      earlier_bytes += (cand_hr < 32) ? layout.units_per_word : layout.units_per_fpreg;
    }
  }

  // In `reload1.c:663`, `assign_stack_local(BLKmode, 0, 0)` rounds
  // `frame_offset` to an 8-byte boundary immediately before `setup_save_areas`.
  const int64_t rel_est_off = (min_off - earlier_bytes) - layout.starting_frame_offset;
  if (rel_est_off < 0) {
    return outcome;
  }
  const int64_t pre_cs_top = layout.starting_frame_offset + (rel_est_off & ~int64_t{7});
  if (pre_cs_top < min_off) {
    for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
      const auto& blk_insns = (*all_parsed_blocks)[b];
      for (size_t i = 0; i < blk_insns.size(); ++i) {
        for (const RtlRegNote& note : blk_insns[i].reg_notes) {
          if (ExprOverlapsByteInterval(note.datum, frame_pointer_needed, layout.elim_to_reg,
                                       pre_cs_top, min_off)) {
            return outcome;
          }
        }
        if (block_plans[b].drop_insn[i] || block_plans[b].replaced_caller_save_insn[i]) {
          continue;
        }
        if (ExprOverlapsByteInterval(blk_insns[i].pattern, frame_pointer_needed, layout.elim_to_reg,
                                     pre_cs_top, min_off)) {
          return outcome;
        }
      }
    }
  }

  // Apply Union-Find across `(from_rest_pseudo -> to_save_pseudo)` pairs.
  absl::flat_hash_map<int, int> parent;
  auto find_root = [&](int p) {
    int cur = p;
    while (parent.contains(cur) && parent[cur] != cur) {
      cur = parent[cur];
    }
    int walk = p;
    while (parent.contains(walk) && parent[walk] != cur) {
      int nxt = parent[walk];
      parent[walk] = cur;
      walk = nxt;
    }
    return cur;
  };

  for (const CallerSaveBlockPlan& plan : block_plans) {
    for (const auto& [from_p, to_p] : plan.pseudo_unions) {
      if (!parent.contains(from_p)) parent[from_p] = from_p;
      if (!parent.contains(to_p)) parent[to_p] = to_p;
      const int r_from = find_root(from_p);
      const int r_to = find_root(to_p);
      if (r_from != r_to) {
        parent[r_from] = r_to;
      }
    }
  }

  absl::flat_hash_set<int> replaced_from_pseudos;
  for (const auto& [p, _] : parent) {
    const int root = find_root(p);
    if (root != p) {
      replaced_from_pseudos.insert(p);
      outcome.unified_pseudos.insert(root);
    }
  }

  for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
    std::vector<RtlPatternAndNotes>& blk_insns = (*all_parsed_blocks)[b];
    absl::flat_hash_map<size_t, std::vector<RtlPatternAndNotes>> insert_before;
    absl::flat_hash_map<size_t, std::vector<RtlPatternAndNotes>> insert_after;

    auto rewrite_read_hr_to_pseudo = [&](RtlExpr* pat, int target_hr, RtlMachineMode target_mode,
                                         int target_pseudo) {
      auto rewrite_elem = [&](RtlExpr* elem) {
        if (elem->op == RtlOp::kUse || elem->op == RtlOp::kClobber) {
          return;
        }
        auto mut = [&](RtlExpr& node) {
          if (node.IsHardReg() && node.RegNo() == target_hr && node.mode == target_mode) {
            node = RtlExpr::MakeReg(target_mode, target_pseudo, node.flags);
          }
        };
        if (elem->op == RtlOp::kSet && !elem->children.empty()) {
          RtlExpr& dst = elem->children[0];
          if (dst.op == RtlOp::kMem) {
            dst.Mutate(mut);
          } else if ((dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract) &&
                     dst.children.size() >= 3) {
            for (size_t k = 1; k < dst.children.size(); ++k) {
              dst.children[k].Mutate(mut);
            }
          }
          for (size_t k = 1; k < elem->children.size(); ++k) {
            elem->children[k].Mutate(mut);
          }
          return;
        }
        elem->Mutate(mut);
      };
      if (pat->op == RtlOp::kParallel) {
        for (RtlExpr& elem : pat->children) {
          rewrite_elem(&elem);
        }
      } else {
        rewrite_elem(pat);
      }
    };

    std::vector<CallerSaveHardSaveLift> sorted_lifts = block_plans[b].hard_save_lifts;
    std::sort(sorted_lifts.begin(), sorted_lifts.end(),
              [](const auto& a, const auto& c) { return a.save_insn_idx < c.save_insn_idx; });
    absl::flat_hash_map<int, int> fresh_pseudo_by_hr;
    for (const CallerSaveHardSaveLift& lift : sorted_lifts) {
      const int nregs =
          std::max(1, MipsHardRegNregs(lift.hard_regno, lift.mode, layout.gp64, layout.fp64));

      const absl::flat_hash_map<int, int> empty_renum;
      std::optional<size_t> def_idx;
      for (int idx = static_cast<int>(lift.save_insn_idx) - 1; idx >= 0; --idx) {
        bool writes = false;
        for (int k = 0; k < nregs; ++k) {
          if (InsnWritesOrClobbersRegOrEffHr(blk_insns[static_cast<size_t>(idx)].pattern,
                                             empty_renum, layout.gp64, layout.fp64,
                                             lift.hard_regno + k, lift.hard_regno + k)) {
            writes = true;
            break;
          }
        }
        if (writes) {
          def_idx = static_cast<size_t>(idx);
          break;
        }
      }

      int p_dst = -1;
      if (lift.target_pseudo >= kFirstPseudoRegister) {
        p_dst = parent.contains(lift.target_pseudo) ? find_root(lift.target_pseudo)
                                                    : lift.target_pseudo;
      } else if (def_idx.has_value() && block_plans[b].drop_insn[*def_idx] &&
                 fresh_pseudo_by_hr.contains(lift.hard_regno)) {
        p_dst = fresh_pseudo_by_hr.at(lift.hard_regno);
      } else {
        *max_existing_regno = std::max(kFirstPseudoRegister - 1, *max_existing_regno) + 1;
        p_dst = *max_existing_regno;
        fresh_pseudo_by_hr[lift.hard_regno] = p_dst;
        (*reg_renumber)[p_dst] = lift.hard_regno;
        if (pseudo_widest_mode != nullptr) {
          (*pseudo_widest_mode)[p_dst] = lift.mode;
        }
      }
      outcome.unified_pseudos.insert(p_dst);

      size_t rewrite_from = 0;
      if (def_idx.has_value()) {
        rewrite_from = *def_idx + 1;
        if (!block_plans[b].drop_insn[*def_idx]) {
          RtlPatternAndNotes& def_insn = blk_insns[*def_idx];
          if (!IsCallPattern(def_insn.pattern) && def_insn.pattern.IsSet() &&
              def_insn.pattern.SetDest().IsHardReg() &&
              def_insn.pattern.SetDest().RegNo() == lift.hard_regno &&
              def_insn.pattern.SetDest().mode == lift.mode) {
            def_insn.pattern.MutableSetDest() =
                RtlExpr::MakeReg(lift.mode, p_dst, def_insn.pattern.SetDest().flags);
            RemoveDeadOrUnusedNotesForReg(&def_insn.reg_notes, lift.hard_regno);
          } else {
            insert_after[*def_idx].push_back(RtlPatternAndNotes{
                .pattern = RtlExpr::MakeSet(RtlExpr::MakeReg(lift.mode, p_dst),
                                            RtlExpr::MakeReg(lift.mode, lift.hard_regno)),
            });
          }
        }
      } else {
        insert_before[0].push_back(RtlPatternAndNotes{
            .pattern = RtlExpr::MakeSet(RtlExpr::MakeReg(lift.mode, p_dst),
                                        RtlExpr::MakeReg(lift.mode, lift.hard_regno)),
        });
      }

      bool overwritten_before_call = false;
      for (size_t m = rewrite_from; m < lift.call_insn_idx && m < blk_insns.size(); ++m) {
        if (block_plans[b].drop_insn[m] || block_plans[b].replaced_caller_save_insn[m] ||
            m == lift.save_insn_idx) {
          continue;
        }
        rewrite_read_hr_to_pseudo(&blk_insns[m].pattern, lift.hard_regno, lift.mode, p_dst);
        RemoveDeadOrUnusedNotesForReg(&blk_insns[m].reg_notes, lift.hard_regno);
        for (int k = 0; k < nregs; ++k) {
          if (InsnWritesOrClobbersRegOrEffHr(blk_insns[m].pattern, empty_renum, layout.gp64,
                                             layout.fp64, lift.hard_regno + k,
                                             lift.hard_regno + k)) {
            overwritten_before_call = true;
            break;
          }
        }
        if (overwritten_before_call) {
          break;
        }
      }
      if (!overwritten_before_call && lift.call_insn_idx < blk_insns.size()) {
        rewrite_read_hr_to_pseudo(&blk_insns[lift.call_insn_idx].pattern, lift.hard_regno,
                                  lift.mode, p_dst);
      }
      for (size_t rest_idx : lift.hard_restore_indices) {
        block_plans[b].drop_insn[rest_idx] = true;
        for (size_t m = rest_idx + 1; m < blk_insns.size(); ++m) {
          if (block_plans[b].drop_insn[m] || block_plans[b].replaced_caller_save_insn[m]) {
            continue;
          }
          rewrite_read_hr_to_pseudo(&blk_insns[m].pattern, lift.hard_regno, lift.mode, p_dst);
          for (RtlRegNote& note : blk_insns[m].reg_notes) {
            if (note.kind != RtlMachineMode::kRegDead && note.kind != RtlMachineMode::kRegUnused) {
              note.datum.Mutate([&](RtlExpr& node) {
                if (node.IsHardReg() && node.RegNo() == lift.hard_regno && node.mode == lift.mode) {
                  node = RtlExpr::MakeReg(lift.mode, p_dst, node.flags);
                }
              });
            }
          }
          RemoveDeadOrUnusedNotesForReg(&blk_insns[m].reg_notes, lift.hard_regno);
          bool overwritten_post = false;
          for (int k = 0; k < nregs; ++k) {
            if (InsnWritesOrClobbersRegOrEffHr(blk_insns[m].pattern, empty_renum, layout.gp64,
                                               layout.fp64, lift.hard_regno + k,
                                               lift.hard_regno + k)) {
              overwritten_post = true;
              break;
            }
          }
          if (overwritten_post || IsControlFlowBoundaryPattern(blk_insns[m].pattern)) {
            break;
          }
        }
      }
      block_plans[b].drop_insn[lift.save_insn_idx] = true;
    }

    for (const CallerSaveHardRestoreMove& hrm : block_plans[b].hard_restore_moves) {
      const int p_src =
          parent.contains(hrm.src_pseudo) ? find_root(hrm.src_pseudo) : hrm.src_pseudo;
      outcome.unified_pseudos.insert(p_src);
      blk_insns[hrm.restore_insn_idx] = RtlPatternAndNotes{
          .pattern = RtlExpr::MakeSet(RtlExpr::MakeReg(hrm.mode, hrm.hard_regno),
                                      RtlExpr::MakeReg(hrm.mode, p_src)),
      };
      outcome.stripped_any = true;
    }

    std::vector<RtlPatternAndNotes> kept;
    kept.reserve(blk_insns.size() + insert_before.size() + insert_after.size());
    for (size_t i = 0; i < blk_insns.size(); ++i) {
      if (auto pre_it = insert_before.find(i); pre_it != insert_before.end()) {
        for (RtlPatternAndNotes& ins : pre_it->second) {
          kept.push_back(std::move(ins));
        }
      }
      if (block_plans[b].drop_insn[i]) {
        outcome.stripped_any = true;
      } else {
        if (!replaced_from_pseudos.empty()) {
          blk_insns[i].RewriteRegs([&](RtlExpr& reg) {
            if (reg.IsPseudoReg() && parent.contains(reg.RegNo())) {
              const int root = find_root(reg.RegNo());
              if (root != reg.RegNo()) {
                reg.SetRegNo(root);
              }
            }
          });
        }
        // Strip any stale intermediate `REG_DEAD`/`REG_UNUSED` notes on unified
        // cross-call pseudos; exact pre-reload death notes are re-synthesized by
        // `ReconstructRtlLiveness`.
        for (int u_p : outcome.unified_pseudos) {
          RemoveDeadOrUnusedNotesForReg(&blk_insns[i].reg_notes, u_p);
        }
        kept.push_back(std::move(blk_insns[i]));
      }
      if (auto post_it = insert_after.find(i); post_it != insert_after.end()) {
        for (RtlPatternAndNotes& ins : post_it->second) {
          kept.push_back(std::move(ins));
        }
      }
    }
    blk_insns = std::move(kept);
  }

  for (int dead_p : replaced_from_pseudos) {
    reg_renumber->erase(dead_p);
  }

  // Ensure `pre_cs_top` does not truncate any surviving stack access above
  // `caller-save` (e.g. if `new_spill_reg` allocated a slot above `caller-save`).
  int64_t max_surviving_local_end = pre_cs_top;
  const int64_t post_var_limit = layout.starting_frame_offset + layout.post_reload_var_size;
  auto record_surviving_mem = [&](const RtlExpr& node) {
    if (node.op == RtlOp::kMem && node.children.size() == 1) {
      std::optional<int64_t> off = MatchStackSlotAddrOffset(node.children[0], frame_pointer_needed,
                                                            layout.starting_frame_offset);
      if (off.has_value() && *off < post_var_limit) {
        max_surviving_local_end =
            std::max<int64_t>(max_surviving_local_end, *off + MipsModeByteSize(node.mode));
      }
    }
  };
  for (const auto& blk_insns : *all_parsed_blocks) {
    for (const RtlPatternAndNotes& insn : blk_insns) {
      insn.pattern.Visit(record_surviving_mem);
      for (const RtlRegNote& note : insn.reg_notes) {
        note.datum.Visit(record_surviving_mem);
      }
    }
  }
  outcome.pre_caller_save_top = std::max(pre_cs_top, max_surviving_local_end);
  return outcome;
}

bool ReplaceRegInExpr(RtlExpr* expr, int target_regno, RtlMachineMode target_mode,
                      const RtlExpr& replacement) {
  if (expr == nullptr) {
    return false;
  }
  if (expr->op == RtlOp::kReg && !expr->IsArgPointerReg() && expr->RegNo() == target_regno &&
      (target_mode == RtlMachineMode::kNone || expr->mode == target_mode)) {
    *expr = replacement;
    return true;
  }
  bool changed = false;
  for (RtlExpr& child : expr->children) {
    if (ReplaceRegInExpr(&child, target_regno, target_mode, replacement)) {
      changed = true;
    }
  }
  return changed;
}

bool ReplaceReadRegInPattern(RtlExpr* pat, int target_regno, RtlMachineMode target_mode,
                             const RtlExpr& replacement) {
  auto replace_elem = [&](RtlExpr* elem) -> bool {
    if (elem->op == RtlOp::kUse || elem->op == RtlOp::kClobber) {
      return false;
    }
    if (elem->op == RtlOp::kSet && !elem->children.empty()) {
      bool changed = false;
      RtlExpr& dst = elem->children[0];
      if (dst.op == RtlOp::kMem) {
        if (ReplaceRegInExpr(&dst, target_regno, target_mode, replacement)) {
          changed = true;
        }
      } else if ((dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract) &&
                 dst.children.size() >= 3) {
        for (size_t k = 1; k < dst.children.size(); ++k) {
          if (ReplaceRegInExpr(&dst.children[k], target_regno, target_mode, replacement)) {
            changed = true;
          }
        }
      }
      for (size_t k = 1; k < elem->children.size(); ++k) {
        if (ReplaceRegInExpr(&elem->children[k], target_regno, target_mode, replacement)) {
          changed = true;
        }
      }
      return changed;
    }
    return ReplaceRegInExpr(elem, target_regno, target_mode, replacement);
  };
  if (pat->op == RtlOp::kParallel) {
    bool changed = false;
    for (RtlExpr& elem : pat->children) {
      if (replace_elem(&elem)) {
        changed = true;
      }
    }
    return changed;
  }
  return replace_elem(pat);
}

std::optional<std::pair<int64_t, RtlMachineMode>> MatchScalarSpillSlotMem(const RtlExpr& mem_expr,
                                                                          bool frame_pointer_needed,
                                                                          int64_t min_offset,
                                                                          int64_t max_offset) {
  if (mem_expr.op != RtlOp::kMem || mem_expr.flags.volatil || mem_expr.flags.in_struct ||
      mem_expr.mode == RtlMachineMode::kBLK || mem_expr.children.size() != 1) {
    return std::nullopt;
  }
  std::optional<int64_t> off =
      MatchStackSlotAddrOffset(mem_expr.children[0], frame_pointer_needed, min_offset);
  if (!off.has_value() || *off >= max_offset) {
    return std::nullopt;
  }
  return std::make_pair(*off, mem_expr.mode);
}

bool CanDirectMoveBetweenMemAndHardReg(int eff_hr, RtlMachineMode mode) {
  if (eff_hr < 0) {
    return false;
  }
  if (eff_hr >= 0 && eff_hr < 32) {
    return true;
  }
  if (eff_hr >= 32 && eff_hr < 64) {
    return mode == RtlMachineMode::kSF || mode == RtlMachineMode::kDF ||
           mode == RtlMachineMode::kSI || mode == RtlMachineMode::kDI;
  }
  return false;
}

void ReplaceSpillSlotMemsInExpr(RtlExpr* expr, bool frame_pointer_needed, int64_t min_offset,
                                int64_t max_offset,
                                const absl::flat_hash_map<int64_t, RtlExpr>& slot_to_pseudo) {
  if (expr == nullptr) {
    return;
  }
  std::optional<std::pair<int64_t, RtlMachineMode>> m =
      MatchScalarSpillSlotMem(*expr, frame_pointer_needed, min_offset, max_offset);
  if (m.has_value()) {
    auto it = slot_to_pseudo.find(m->first);
    if (it != slot_to_pseudo.end() && it->second.mode == m->second) {
      RtlExpr repl = it->second;
      if (expr->flags.unchanging) {
        repl.flags.unchanging = true;
      }
      *expr = std::move(repl);
      return;
    }
  }
  for (RtlExpr& child : expr->children) {
    ReplaceSpillSlotMemsInExpr(&child, frame_pointer_needed, min_offset, max_offset,
                               slot_to_pseudo);
  }
}

// Mirrors GCC `rtlanal.c:rtx_addr_varies_p` on a `MEM` address: true iff `addr`
// depends on a nested `MEM` or any register other than `$sp` (`29`), `$fp`
// (`30`), or `arg_pointer_rtx` (`(reg:SI 0 argp)`).
bool IsVaryingMemAddress(const RtlExpr& addr, bool frame_pointer_needed) {
  bool varies = false;
  addr.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kMem) {
      varies = true;
    } else if (n.op == RtlOp::kReg) {
      if (n.IsArgPointerReg()) {
        return;
      }
      const int r = n.RegNo();
      if (r != kStackPointerRegnum && !(frame_pointer_needed && r == kFramePointerRegnum)) {
        varies = true;
      }
    }
  });
  return varies;
}

// When `sched2` (`-O2+`) moved a non-unchanging stack store downward past
// `pat` (or moved `pat` upward past that stack store), GCC
// `sched.c:true_dependence` (lines 828-840) proved zero memory dependence
// between that stack store and any varying-address `MEM` read in `pat`. Note
// that in GCC `rtlanal.c:rtx_varies_p` (lines 101-102), `stack_pointer_rtx`
// (`$sp`) is treated as varying (`rtx_addr_varies_p((mem (+ $sp off))) == 1`),
// so when `!frame_pointer_needed` the ONLY way `true_dependence` returns `0` on
// a varying `MEM` read `x` is `RTX_UNCHANGING_P(x)` (`/u`).
void MarkVaryingMemsCrossedBySunkStackStore(RtlExpr* expr, bool is_lhs_dest,
                                            bool frame_pointer_needed,
                                            bool allow_unchanging_reads) {
  if (expr == nullptr) {
    return;
  }
  if (expr->op == RtlOp::kSet && expr->children.size() == 2) {
    MarkVaryingMemsCrossedBySunkStackStore(&expr->children[0], /*is_lhs_dest=*/true,
                                           frame_pointer_needed, allow_unchanging_reads);
    MarkVaryingMemsCrossedBySunkStackStore(&expr->children[1], /*is_lhs_dest=*/false,
                                           frame_pointer_needed, allow_unchanging_reads);
    return;
  }
  if (expr->op == RtlOp::kMem && !expr->children.empty()) {
    MarkVaryingMemsCrossedBySunkStackStore(&expr->children[0], /*is_lhs_dest=*/false,
                                           frame_pointer_needed, allow_unchanging_reads);
    if (!expr->flags.volatil && IsVaryingMemAddress(expr->children[0], frame_pointer_needed)) {
      if (!is_lhs_dest && allow_unchanging_reads) {
        expr->flags.unchanging = true;
      }
      if (expr->mode != RtlMachineMode::kQI && expr->mode != RtlMachineMode::kBLK) {
        expr->flags.in_struct = true;
      }
    }
    return;
  }
  for (RtlExpr& ch : expr->children) {
    MarkVaryingMemsCrossedBySunkStackStore(&ch, /*is_lhs_dest=*/false, frame_pointer_needed,
                                           allow_unchanging_reads);
  }
}

bool InsnWritesVaryingNonStackMem(const RtlExpr& pat, bool frame_pointer_needed) {
  auto check_one = [&](const RtlExpr& e) -> bool {
    if (e.op == RtlOp::kSet && !e.children.empty() && e.children[0].op == RtlOp::kMem &&
        !e.children[0].children.empty()) {
      return IsVaryingMemAddress(e.children[0].children[0], frame_pointer_needed);
    }
    return false;
  };
  if (check_one(pat)) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      if (check_one(ch)) {
        return true;
      }
    }
  }
  return false;
}

// Dual rule when `sched2` hoisted a fixed-address scalar stack load upward
// across `pat`: any varying-address non-`QImode` `MEM` store in `pat` must have
// had `MEM_IN_STRUCT_P` (`/s`) so `sched.c:true_dependence(store, stack_load)`
// returned `0`.
void MarkVaryingMemStoresCrossedByHoistedStackLoad(RtlExpr* expr, bool frame_pointer_needed) {
  if (expr == nullptr) {
    return;
  }
  if (expr->op == RtlOp::kSet && expr->children.size() == 2) {
    RtlExpr& lhs = expr->children[0];
    if (lhs.op == RtlOp::kMem && !lhs.children.empty() && !lhs.flags.volatil &&
        lhs.mode != RtlMachineMode::kQI && lhs.mode != RtlMachineMode::kBLK &&
        IsVaryingMemAddress(lhs.children[0], frame_pointer_needed)) {
      lhs.flags.in_struct = true;
    }
    return;
  }
  if (expr->op == RtlOp::kParallel) {
    for (RtlExpr& ch : expr->children) {
      MarkVaryingMemStoresCrossedByHoistedStackLoad(&ch, frame_pointer_needed);
    }
  }
}

void ReconstructSpilledPseudosInBlock(size_t block_idx, int opt_level, bool frame_pointer_needed,
                                      bool gp64, bool fp64, bool allow_pseudo_reload_reg,
                                      int64_t min_offset, int64_t max_offset,
                                      const absl::flat_hash_map<int, int>& reg_renumber,
                                      const absl::flat_hash_map<int, PseudoUseSummary>& summaries,
                                      const absl::flat_hash_map<int64_t, RtlExpr>& slot_to_pseudo,
                                      std::vector<RtlPatternAndNotes>* insns,
                                      absl::flat_hash_set<int>* reversed_reload_regs,
                                      absl::flat_hash_set<int>* proven_spill_regs,
                                      absl::flat_hash_set<int>* folded_scratch_pseudos,
                                      absl::flat_hash_set<int>* target_note_pseudos) {
  if (insns == nullptr || insns->empty() || slot_to_pseudo.empty()) {
    return;
  }

  auto match_slot = [&](const RtlExpr& expr) -> std::optional<RtlExpr> {
    std::optional<std::pair<int64_t, RtlMachineMode>> m =
        MatchScalarSpillSlotMem(expr, frame_pointer_needed, min_offset, max_offset);
    if (!m.has_value()) {
      return std::nullopt;
    }
    auto it = slot_to_pseudo.find(m->first);
    if (it == slot_to_pseudo.end() || it->second.mode != m->second) {
      return std::nullopt;
    }
    RtlExpr repl = it->second;
    if (expr.flags.unchanging) {
      repl.flags.unchanging = true;
    }
    return repl;
  };

  auto is_eligible_reload_reg = [&](int regno) -> bool {
    if (regno >= 0 && regno < kFirstPseudoRegister) {
      return true;
    }
    if (!allow_pseudo_reload_reg) {
      return false;
    }
    return IsSingleBlockScratchPseudo(regno, block_idx, reg_renumber, summaries);
  };

  // Pass 1: Fold input reloads `(set (reg:M r_rel) slot_mem_k)` into subsequent
  // instruction(s) reading `r_rel` before `r_rel` (or its effective hard reg)
  // is overwritten.
  std::vector<bool> drop_input(insns->size(), false);
  for (size_t i = 0; i < insns->size(); ++i) {
    const RtlPatternAndNotes& insn = (*insns)[i];
    if (!insn.reg_notes.empty() || insn.pattern.op != RtlOp::kSet ||
        insn.pattern.children.size() != 2) {
      continue;
    }
    const RtlExpr& dst = insn.pattern.children[0];
    if (dst.op != RtlOp::kReg || dst.IsArgPointerReg() || !is_eligible_reload_reg(dst.RegNo())) {
      continue;
    }
    std::optional<RtlExpr> pseudo = match_slot(insn.pattern.children[1]);
    if (!pseudo.has_value() || pseudo->mode != dst.mode) {
      continue;
    }

    const int r_rel = dst.RegNo();
    const int eff_hr = EffectiveHardReg(r_rel, reg_renumber);

    // Check whether the sole consumer of `r_rel` is a plain register-to-register
    // move `(set (reg:M r_dst) (reg:M r_rel))` where `r_dst` could load directly
    // from memory in `mips.md` without an input reload (meaning `.08.lreg` had
    // an explicit move through `r_rel`).
    if (r_rel >= kFirstPseudoRegister && i + 1 < insns->size()) {
      const RtlExpr& next_pat = (*insns)[i + 1].pattern;
      if (next_pat.op == RtlOp::kSet && next_pat.children.size() == 2 &&
          next_pat.children[0].op == RtlOp::kReg && !next_pat.children[0].IsArgPointerReg() &&
          next_pat.children[1].op == RtlOp::kReg && !next_pat.children[1].IsArgPointerReg() &&
          next_pat.children[1].RegNo() == r_rel && next_pat.children[0].mode == dst.mode) {
        const int dst_hr = EffectiveHardReg(next_pat.children[0].RegNo(), reg_renumber);
        auto sum_it = summaries.find(r_rel);
        if (CanDirectMoveBetweenMemAndHardReg(dst_hr, dst.mode) && sum_it != summaries.end() &&
            sum_it->second.use_count == 1) {
          continue;
        }
      }
    }

    // For pseudo `r_rel`, confirm ALL uses of `r_rel` fall within the foldable
    // consumer window before `eff_hr` is overwritten or control flow splits.
    if (r_rel >= kFirstPseudoRegister) {
      int replaceable_reads = 0;
      for (size_t j = i + 1; j < insns->size(); ++j) {
        if (drop_input[j]) {
          continue;
        }
        RtlExpr probe = (*insns)[j].pattern;
        int before_hits = 0;
        probe.Visit([&](const RtlExpr& node) {
          if (node.op == RtlOp::kReg && !node.IsArgPointerReg() && node.RegNo() == r_rel) {
            ++before_hits;
          }
        });
        if (before_hits > 0 && ReplaceReadRegInPattern(&probe, r_rel, dst.mode, *pseudo)) {
          int after_hits = 0;
          probe.Visit([&](const RtlExpr& node) {
            if (node.op == RtlOp::kReg && !node.IsArgPointerReg() && node.RegNo() == r_rel) {
              ++after_hits;
            }
          });
          replaceable_reads += (before_hits - after_hits);
        }
        if (InsnWritesOrClobbersRegOrEffHr((*insns)[j].pattern, reg_renumber, gp64, fp64, r_rel,
                                           eff_hr) ||
            InsnWritesOrClobbersRegOrEffHr((*insns)[j].pattern, reg_renumber, gp64, fp64,
                                           pseudo->RegNo(), /*target_eff_hr=*/-1) ||
            IsControlFlowBoundaryPattern((*insns)[j].pattern)) {
          break;
        }
      }
      auto sum_it = summaries.find(r_rel);
      if (sum_it == summaries.end() || replaceable_reads != sum_it->second.use_count ||
          replaceable_reads == 0) {
        continue;
      }
    }

    int replaced_insn_count = 0;
    size_t last_replaced_idx = 0;
    bool consumer_writes_eff_hr = false;
    const int eff_nw =
        (eff_hr >= 0) ? std::max(1, MipsHardRegNregs(eff_hr, dst.mode, gp64, fp64)) : 1;
    for (size_t j = i + 1; j < insns->size(); ++j) {
      if (drop_input[j]) {
        continue;
      }
      if (ReplaceReadRegInPattern(&(*insns)[j].pattern, r_rel, dst.mode, *pseudo)) {
        ++replaced_insn_count;
        last_replaced_idx = j;
        if (eff_hr >= 0) {
          for (int w = 0; w < eff_nw; ++w) {
            if (InsnWritesOrClobbersRegOrEffHr((*insns)[j].pattern, reg_renumber, gp64, fp64,
                                               /*target_regno=*/-1, eff_hr + w)) {
              consumer_writes_eff_hr = true;
            }
          }
        }
        if (!(*insns)[j].pattern.ContainsReg(r_rel)) {
          RemoveDeadOrUnusedNotesForReg(&(*insns)[j].reg_notes, r_rel);
        }
      }
      if (InsnWritesOrClobbersRegOrEffHr((*insns)[j].pattern, reg_renumber, gp64, fp64, r_rel,
                                         eff_hr) ||
          InsnWritesOrClobbersRegOrEffHr((*insns)[j].pattern, reg_renumber, gp64, fp64,
                                         pseudo->RegNo(), /*target_eff_hr=*/-1) ||
          IsControlFlowBoundaryPattern((*insns)[j].pattern)) {
        break;
      }
    }
    if (replaced_insn_count > 0) {
      drop_input[i] = true;
      if (reversed_reload_regs != nullptr && eff_hr >= 0) {
        for (int w = 0; w < eff_nw; ++w) {
          reversed_reload_regs->insert(eff_hr + w);
        }
      }
      if (proven_spill_regs != nullptr && eff_hr >= 0 && !consumer_writes_eff_hr) {
        for (int w = 0; w < eff_nw; ++w) {
          proven_spill_regs->insert(eff_hr + w);
        }
      }
      if (folded_scratch_pseudos != nullptr && r_rel >= kFirstPseudoRegister) {
        folded_scratch_pseudos->insert(r_rel);
      }

      // On `InvertAlterReg` outputs at `-O1+`: `reload.c` emits single-consumer
      // input reloads immediately adjacent to their consumer; hoisting the
      // consumer back to `i + 1` restores its pre-`reload` `INSN_LUID` order
      // across pure register ALU insns and not-yet-folded spill-slot loads,
      // provided we never cross an instruction that itself requires a reload
      // (already-folded spilled pseudos, GPR<->FPR or `HI:LO` register-class
      // constraint reload moves, or non-spill `MEM`s at `-O2+`).
      bool hoisted_consumer = false;
      if (allow_pseudo_reload_reg && opt_level >= 1 && replaced_insn_count == 1 &&
          last_replaced_idx > i + 1) {
        const RtlExpr& cons_pat = (*insns)[last_replaced_idx].pattern;
        bool cons_has_mem = false;
        cons_pat.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kMem) cons_has_mem = true;
        });
        if (!cons_has_mem && cons_pat.op == RtlOp::kSet && cons_pat.children.size() == 2 &&
            cons_pat.children[0].op == RtlOp::kReg && !cons_pat.children[0].IsArgPointerReg()) {
          const int cons_dst = cons_pat.children[0].RegNo();
          const int cons_dst_hr = EffectiveHardReg(cons_dst, reg_renumber);
          const int cons_dst_nw =
              (cons_dst_hr >= 0)
                  ? std::max(1,
                             MipsHardRegNregs(cons_dst_hr, cons_pat.children[0].mode, gp64, fp64))
                  : 1;
          std::vector<std::pair<int, std::pair<int, int>>> cons_reads;
          cons_pat.children[1].Visit([&](const RtlExpr& n) {
            if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
              const int rr = n.RegNo();
              const int rhr = EffectiveHardReg(rr, reg_renumber);
              const int rnw =
                  (rhr >= 0) ? std::max(1, MipsHardRegNregs(rhr, n.mode, gp64, fp64)) : 1;
              cons_reads.push_back({rr, {rhr, rnw}});
            }
          });

          bool can_hoist = true;
          for (size_t k = i + 1; k < last_replaced_idx && can_hoist; ++k) {
            if (drop_input[k] || IsCallPattern((*insns)[k].pattern) ||
                IsControlFlowBoundaryPattern((*insns)[k].pattern)) {
              can_hoist = false;
              break;
            }
            const RtlExpr& mid_pat = (*insns)[k].pattern;
            if (opt_level >= 2) {
              if (mid_pat.op == RtlOp::kSet && mid_pat.children.size() == 2 &&
                  mid_pat.children[0].op == RtlOp::kReg && !mid_pat.children[0].IsArgPointerReg() &&
                  mid_pat.children[1].op == RtlOp::kReg && !mid_pat.children[1].IsArgPointerReg()) {
                const int md_hr = EffectiveHardReg(mid_pat.children[0].RegNo(), reg_renumber);
                const int ms_hr = EffectiveHardReg(mid_pat.children[1].RegNo(), reg_renumber);
                const bool md_fpr = (md_hr >= 32 && md_hr < 64);
                const bool ms_fpr = (ms_hr >= 32 && ms_hr < 64);
                const bool md_hilo = (md_hr >= 64 && md_hr < 66);
                const bool ms_hilo = (ms_hr >= 64 && ms_hr < 66);
                if ((md_hr >= 0 && ms_hr >= 0 && md_fpr != ms_fpr) || md_hilo || ms_hilo) {
                  can_hoist = false;
                  break;
                }
              }
              mid_pat.Visit([&](const RtlExpr& n) {
                if (n.op == RtlOp::kMem && !match_slot(n).has_value()) {
                  can_hoist = false;
                } else if (n.IsPseudoReg() && !reg_renumber.contains(n.RegNo())) {
                  can_hoist = false;
                } else if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                  const int nhr = EffectiveHardReg(n.RegNo(), reg_renumber);
                  if (nhr >= 64 && nhr < 66) {
                    can_hoist = false;
                  }
                }
              });
              if (!can_hoist) {
                break;
              }
            }
            if (mid_pat.ContainsReg(cons_dst)) {
              can_hoist = false;
              break;
            }
            if (cons_dst_hr >= 0) {
              for (int h = cons_dst_hr; h < cons_dst_hr + cons_dst_nw; ++h) {
                if (InsnWritesOrClobbersRegOrEffHr(mid_pat, reg_renumber, gp64, fp64, cons_dst,
                                                   h)) {
                  can_hoist = false;
                  break;
                }
              }
              mid_pat.Visit([&](const RtlExpr& n) {
                if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                  const int nhr = EffectiveHardReg(n.RegNo(), reg_renumber);
                  if (nhr >= 0) {
                    const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
                    if (nhr < cons_dst_hr + cons_dst_nw && cons_dst_hr < nhr + nnw) {
                      can_hoist = false;
                    }
                  }
                }
              });
            }
            for (const auto& [rr, hr_span] : cons_reads) {
              if (InsnWritesOrClobbersRegOrEffHr(mid_pat, reg_renumber, gp64, fp64, rr, -1)) {
                can_hoist = false;
                break;
              }
              if (hr_span.first >= 0) {
                for (int h = hr_span.first; h < hr_span.first + hr_span.second; ++h) {
                  if (InsnWritesOrClobbersRegOrEffHr(mid_pat, reg_renumber, gp64, fp64, rr, h)) {
                    can_hoist = false;
                    break;
                  }
                }
              }
            }
          }
          if (can_hoist) {
            std::rotate(insns->begin() + static_cast<ptrdiff_t>(i + 1),
                        insns->begin() + static_cast<ptrdiff_t>(last_replaced_idx),
                        insns->begin() + static_cast<ptrdiff_t>(last_replaced_idx + 1));
            std::rotate(drop_input.begin() + static_cast<ptrdiff_t>(i + 1),
                        drop_input.begin() + static_cast<ptrdiff_t>(last_replaced_idx),
                        drop_input.begin() + static_cast<ptrdiff_t>(last_replaced_idx + 1));
            hoisted_consumer = true;
          }
        }
      }
      if (!hoisted_consumer && allow_pseudo_reload_reg && opt_level >= 2 &&
          last_replaced_idx > i + 1) {
        for (size_t k = i + 1; k < last_replaced_idx; ++k) {
          if (!drop_input[k]) {
            MarkVaryingMemStoresCrossedByHoistedStackLoad(&(*insns)[k].pattern,
                                                          frame_pointer_needed);
          }
        }
      }
    }
  }

  std::vector<RtlPatternAndNotes> after_inputs;
  after_inputs.reserve(insns->size());
  for (size_t i = 0; i < insns->size(); ++i) {
    if (!drop_input[i]) {
      after_inputs.push_back(std::move((*insns)[i]));
    }
  }

  // Recompute local pseudo use counts on `after_inputs` so output-reload
  // folding has exact remaining reference counts after Pass 1.
  absl::flat_hash_map<int, int> remaining_pseudo_uses;
  for (const RtlPatternAndNotes& insn : after_inputs) {
    auto count_reads = [&](const RtlExpr& elem) {
      if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
        if (!elem.children[0].IsPseudoReg()) {
          elem.children[0].Visit([&](const RtlExpr& n) {
            if (n.IsPseudoReg()) ++remaining_pseudo_uses[n.RegNo()];
          });
        }
        for (size_t k = 1; k < elem.children.size(); ++k) {
          elem.children[k].Visit([&](const RtlExpr& n) {
            if (n.IsPseudoReg()) ++remaining_pseudo_uses[n.RegNo()];
          });
        }
      } else {
        elem.Visit([&](const RtlExpr& n) {
          if (n.IsPseudoReg()) ++remaining_pseudo_uses[n.RegNo()];
        });
      }
    };
    if (insn.pattern.op == RtlOp::kParallel) {
      for (const RtlExpr& elem : insn.pattern.children) {
        count_reads(elem);
      }
    } else {
      count_reads(insn.pattern);
    }
  }

  // Pass 2: Fold output reloads `(set (reg:M r_rel) <rhs>)` followed (either
  // immediately or across non-interfering `sched2`-scheduled instructions when
  // `allow_pseudo_reload_reg` is true) by `(set slot_mem_k (reg:M r_rel))`
  // (with optional `REG_DEAD r_rel` on `store_insn`), propagating `pseudo_k`
  // to any reload-inherited reads.
  std::vector<bool> drop_output(after_inputs.size(), false);
  for (size_t i = 0; i + 1 < after_inputs.size(); ++i) {
    if (drop_output[i]) {
      continue;
    }
    RtlPatternAndNotes& def_insn = after_inputs[i];
    if (def_insn.pattern.op != RtlOp::kSet || def_insn.pattern.children.size() != 2) {
      continue;
    }
    const RtlExpr& def_dst = def_insn.pattern.children[0];
    if (def_dst.op != RtlOp::kReg || def_dst.IsArgPointerReg() ||
        !is_eligible_reload_reg(def_dst.RegNo())) {
      continue;
    }

    const int r_rel = def_dst.RegNo();
    const RtlMachineMode rel_mode = def_dst.mode;
    const int eff_hr = EffectiveHardReg(r_rel, reg_renumber);

    // Do not fold a plain register-to-register copy `(set r_rel r_src)` if
    // `r_src` could store directly to `slot_mem` without a reload.
    if (r_rel >= kFirstPseudoRegister && def_insn.pattern.children[1].op == RtlOp::kReg &&
        !def_insn.pattern.children[1].IsArgPointerReg()) {
      const int src_hr = EffectiveHardReg(def_insn.pattern.children[1].RegNo(), reg_renumber);
      if (CanDirectMoveBetweenMemAndHardReg(src_hr, rel_mode) &&
          remaining_pseudo_uses[r_rel] == 1) {
        continue;
      }
    }

    const size_t max_store_idx = allow_pseudo_reload_reg ? (after_inputs.size() - 1) : (i + 1);
    for (size_t store_idx = i + 1; store_idx <= max_store_idx; ++store_idx) {
      if (drop_output[store_idx]) {
        continue;
      }
      const RtlPatternAndNotes& store_insn = after_inputs[store_idx];
      if (store_insn.pattern.op == RtlOp::kSet && store_insn.pattern.children.size() == 2 &&
          store_insn.pattern.children[1].op == RtlOp::kReg &&
          !store_insn.pattern.children[1].IsArgPointerReg() &&
          store_insn.pattern.children[1].RegNo() == r_rel &&
          store_insn.pattern.children[1].mode == rel_mode &&
          InsnHasOnlyOptionalDeadNoteOn(store_insn, r_rel)) {
        std::optional<RtlExpr> pseudo = match_slot(store_insn.pattern.children[0]);
        if (pseudo.has_value() && pseudo->mode == rel_mode) {
          bool pre_store_conflict = false;
          int pre_store_inherited_reads = 0;
          for (size_t k = i + 1; k < store_idx; ++k) {
            if (drop_output[k]) {
              continue;
            }
            if (InsnWritesOrClobbersRegOrEffHr(after_inputs[k].pattern, reg_renumber, gp64, fp64,
                                               pseudo->RegNo(), /*target_eff_hr=*/-1)) {
              pre_store_conflict = true;
              break;
            }
            bool accesses_same_slot = false;
            after_inputs[k].pattern.Visit([&](const RtlExpr& n) {
              std::optional<RtlExpr> sp = match_slot(n);
              if (sp.has_value() && sp->RegNo() == pseudo->RegNo()) {
                accesses_same_slot = true;
              }
            });
            if (accesses_same_slot) {
              pre_store_conflict = true;
              break;
            }
            int before_hits = 0;
            after_inputs[k].pattern.Visit([&](const RtlExpr& n) {
              if (n.op == RtlOp::kReg && !n.IsArgPointerReg() && n.RegNo() == r_rel) {
                ++before_hits;
              }
            });
            if (before_hits > 0) {
              RtlExpr probe = after_inputs[k].pattern;
              int after_hits = before_hits;
              if (ReplaceReadRegInPattern(&probe, r_rel, rel_mode, *pseudo)) {
                after_hits = 0;
                probe.Visit([&](const RtlExpr& n) {
                  if (n.op == RtlOp::kReg && !n.IsArgPointerReg() && n.RegNo() == r_rel) {
                    ++after_hits;
                  }
                });
              }
              if (after_hits != 0) {
                pre_store_conflict = true;
                break;
              }
              pre_store_inherited_reads += before_hits;
            }
          }

          if (!pre_store_conflict) {
            int post_store_inherited_reads = 0;
            for (size_t m = store_idx + 1; m < after_inputs.size(); ++m) {
              if (drop_output[m]) {
                continue;
              }
              RtlExpr probe = after_inputs[m].pattern;
              int before_hits = 0;
              probe.Visit([&](const RtlExpr& n) {
                if (n.op == RtlOp::kReg && !n.IsArgPointerReg() && n.RegNo() == r_rel) {
                  ++before_hits;
                }
              });
              if (before_hits > 0 && ReplaceReadRegInPattern(&probe, r_rel, rel_mode, *pseudo)) {
                int after_hits = 0;
                probe.Visit([&](const RtlExpr& n) {
                  if (n.op == RtlOp::kReg && !n.IsArgPointerReg() && n.RegNo() == r_rel) {
                    ++after_hits;
                  }
                });
                post_store_inherited_reads += (before_hits - after_hits);
              }
              if (InsnWritesOrClobbersRegOrEffHr(after_inputs[m].pattern, reg_renumber, gp64, fp64,
                                                 r_rel, eff_hr) ||
                  InsnWritesOrClobbersRegOrEffHr(after_inputs[m].pattern, reg_renumber, gp64, fp64,
                                                 pseudo->RegNo(), /*target_eff_hr=*/-1) ||
                  IsControlFlowBoundaryPattern(after_inputs[m].pattern)) {
                break;
              }
            }

            const bool count_ok = (r_rel < kFirstPseudoRegister) ||
                                  (1 + pre_store_inherited_reads + post_store_inherited_reads ==
                                   remaining_pseudo_uses[r_rel]);

            if (count_ok) {
              bool rhs_mentions_eff_hr = false;
              const int rel_nw =
                  (eff_hr >= 0) ? std::max(1, MipsHardRegNregs(eff_hr, rel_mode, gp64, fp64)) : 1;
              if (eff_hr >= 0) {
                def_insn.pattern.children[1].Visit([&](const RtlExpr& n) {
                  if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                    const int nhr = EffectiveHardReg(n.RegNo(), reg_renumber);
                    if (nhr >= 0) {
                      const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
                      if (nhr < eff_hr + rel_nw && eff_hr < nhr + nnw) {
                        rhs_mentions_eff_hr = true;
                        // When `reload` reused a dying input register `n`
                        // (allocated pseudo or explicit incoming/local hard
                        // register) in-place as the output-reload scratch
                        // `r_rel`, `reload` suppressed `REG_DEAD (reg n)` on
                        // `def_insn`. Mark `n` in `target_note_pseudos` so
                        // `ReconstructRtlLiveness` restores its pre-reload
                        // `REG_DEAD` note.
                        if (!n.IsArgPointerReg() && target_note_pseudos != nullptr) {
                          target_note_pseudos->insert(n.RegNo());
                        }
                      }
                    }
                  }
                });
              }
              def_insn.pattern.children[0] = *pseudo;
              RemoveDeadOrUnusedNotesForReg(&def_insn.reg_notes, r_rel);
              if (reversed_reload_regs != nullptr && eff_hr >= 0) {
                for (int w = 0; w < rel_nw; ++w) {
                  reversed_reload_regs->insert(eff_hr + w);
                }
              }
              if (proven_spill_regs != nullptr && eff_hr >= 0 && !rhs_mentions_eff_hr) {
                for (int w = 0; w < rel_nw; ++w) {
                  proven_spill_regs->insert(eff_hr + w);
                }
              }
              if (folded_scratch_pseudos != nullptr && r_rel >= kFirstPseudoRegister) {
                folded_scratch_pseudos->insert(r_rel);
              }

              for (size_t k = i + 1; k < store_idx; ++k) {
                if (!drop_output[k] &&
                    ReplaceReadRegInPattern(&after_inputs[k].pattern, r_rel, rel_mode, *pseudo)) {
                  if (!after_inputs[k].pattern.ContainsReg(r_rel)) {
                    RemoveDeadOrUnusedNotesForReg(&after_inputs[k].reg_notes, r_rel);
                  }
                }
              }
              for (size_t m = store_idx + 1; m < after_inputs.size(); ++m) {
                if (drop_output[m]) {
                  continue;
                }
                if (ReplaceReadRegInPattern(&after_inputs[m].pattern, r_rel, rel_mode, *pseudo)) {
                  if (!after_inputs[m].pattern.ContainsReg(r_rel)) {
                    RemoveDeadOrUnusedNotesForReg(&after_inputs[m].reg_notes, r_rel);
                  }
                }
                if (InsnWritesOrClobbersRegOrEffHr(after_inputs[m].pattern, reg_renumber, gp64,
                                                   fp64, r_rel, eff_hr) ||
                    InsnWritesOrClobbersRegOrEffHr(after_inputs[m].pattern, reg_renumber, gp64,
                                                   fp64, pseudo->RegNo(), /*target_eff_hr=*/-1) ||
                    IsControlFlowBoundaryPattern(after_inputs[m].pattern)) {
                  break;
                }
              }

              // When `sched2` hoisted a `spill_regs`-scratch `def_insn`
              // (`!rhs_mentions_eff_hr`) above an intervening instruction whose
              // own spilled-pseudo reload was folded in Pass 1 (which would
              // otherwise contend for `spill_regs` scratch `eff_hr` and break
              // post-store reload inheritance), sink `def_insn` back to
              // `store_idx` whenever dependency-free. Never sink
              // `find_dummy_reload` (`rhs_mentions_eff_hr`) definitions, whose
              // post-store `find_equiv_reg` inheritance is unaffected (`goal` is
              // a pseudo with `reg_equiv_memory_loc == 0` during `reload_as_needed`).
              bool intervening_spilled_pseudo_ref = false;
              if (!rhs_mentions_eff_hr && store_idx > i + 1) {
                for (size_t k = i + 1; k < store_idx && !intervening_spilled_pseudo_ref; ++k) {
                  if (drop_output[k]) {
                    continue;
                  }
                  after_inputs[k].pattern.Visit([&](const RtlExpr& n) {
                    if (n.IsPseudoReg() && !reg_renumber.contains(n.RegNo())) {
                      intervening_spilled_pseudo_ref = true;
                    }
                  });
                }
              }
              bool can_sink_to_store = allow_pseudo_reload_reg && !rhs_mentions_eff_hr &&
                                       intervening_spilled_pseudo_ref && store_idx > i + 1 &&
                                       pre_store_inherited_reads == 0 &&
                                       post_store_inherited_reads > 0;
              if (can_sink_to_store) {
                std::vector<std::pair<int, RtlMachineMode>> rhs_regs;
                bool rhs_reads_mem = false;
                def_insn.pattern.children[1].Visit([&](const RtlExpr& n) {
                  if (n.op == RtlOp::kMem) {
                    rhs_reads_mem = true;
                  } else if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                    rhs_regs.emplace_back(n.RegNo(), n.mode);
                  }
                });
                for (size_t k = i + 1; k < store_idx && can_sink_to_store; ++k) {
                  if (drop_output[k]) {
                    continue;
                  }
                  const RtlExpr& mid = after_inputs[k].pattern;
                  if (IsCallPattern(mid) || IsControlFlowBoundaryPattern(mid)) {
                    can_sink_to_store = false;
                    break;
                  }
                  if (rhs_reads_mem) {
                    auto writes_mem = [](const RtlExpr& e) {
                      return e.op == RtlOp::kSet && !e.children.empty() &&
                             e.children[0].op == RtlOp::kMem;
                    };
                    if (writes_mem(mid)) {
                      can_sink_to_store = false;
                      break;
                    }
                    if (mid.op == RtlOp::kParallel) {
                      for (const RtlExpr& ch : mid.children) {
                        if (writes_mem(ch)) {
                          can_sink_to_store = false;
                          break;
                        }
                      }
                    }
                    if (!can_sink_to_store) {
                      break;
                    }
                  }
                  for (const auto& [rr, rmode] : rhs_regs) {
                    const int rr_hr = EffectiveHardReg(rr, reg_renumber);
                    const int rr_nw =
                        (rr_hr >= 0) ? std::max(1, MipsHardRegNregs(rr_hr, rmode, gp64, fp64)) : 1;
                    for (int w = 0; w < rr_nw; ++w) {
                      if (InsnWritesOrClobbersRegOrEffHr(mid, reg_renumber, gp64, fp64, rr,
                                                         rr_hr >= 0 ? rr_hr + w : -1)) {
                        can_sink_to_store = false;
                        break;
                      }
                    }
                    if (!can_sink_to_store) {
                      break;
                    }
                    if (rr < kFirstPseudoRegister) {
                      for (const RtlRegNote& note : after_inputs[k].reg_notes) {
                        if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg() &&
                            note.datum.RegNo() == rr) {
                          can_sink_to_store = false;
                          break;
                        }
                      }
                    }
                    if (!can_sink_to_store) {
                      break;
                    }
                  }
                }
              }

              if (can_sink_to_store) {
                after_inputs[store_idx] = std::move(def_insn);
                drop_output[i] = true;
              } else {
                if (allow_pseudo_reload_reg && opt_level >= 2 && store_idx > i + 1) {
                  bool seen_varying_store = false;
                  for (size_t prev = 0; prev <= i; ++prev) {
                    if (IsCallPattern(after_inputs[prev].pattern)) {
                      seen_varying_store = false;
                    } else if (!drop_output[prev] &&
                               InsnWritesVaryingNonStackMem(after_inputs[prev].pattern,
                                                            frame_pointer_needed)) {
                      seen_varying_store = true;
                    }
                  }
                  for (size_t k = i + 1; k < store_idx; ++k) {
                    if (!drop_output[k]) {
                      MarkVaryingMemsCrossedBySunkStackStore(
                          &after_inputs[k].pattern,
                          /*is_lhs_dest=*/false, frame_pointer_needed, !seen_varying_store);
                      if (InsnWritesVaryingNonStackMem(after_inputs[k].pattern,
                                                       frame_pointer_needed)) {
                        seen_varying_store = true;
                      }
                    }
                  }
                }
                drop_output[store_idx] = true;
              }
              break;
            }
          }
        }
      }

      if (IsCallPattern(store_insn.pattern) || IsControlFlowBoundaryPattern(store_insn.pattern) ||
          InsnWritesOrClobbersRegOrEffHr(store_insn.pattern, reg_renumber, gp64, fp64, r_rel,
                                         eff_hr)) {
        break;
      }
    }
  }

  std::vector<RtlPatternAndNotes> result;
  result.reserve(after_inputs.size());
  for (size_t i = 0; i < after_inputs.size(); ++i) {
    if (drop_output[i]) {
      continue;
    }
    // Pass 3: Rewrite any remaining direct `alter_reg` spill slot `MEM`s in
    // patterns and `REG_NOTES` (such as `mov*` direct reg<->mem moves or
    // `REG_DEAD (mem:SI [sp + off_k])`) back to `(reg:M p_k)`.
    ReplaceSpillSlotMemsInExpr(&after_inputs[i].pattern, frame_pointer_needed, min_offset,
                               max_offset, slot_to_pseudo);
    for (RtlRegNote& note : after_inputs[i].reg_notes) {
      ReplaceSpillSlotMemsInExpr(&note.datum, frame_pointer_needed, min_offset, max_offset,
                                 slot_to_pseudo);
    }
    result.push_back(std::move(after_inputs[i]));
  }

  *insns = std::move(result);
}

struct SpillReversalOutcome {
  int64_t pre_reload_frame_top = 0;
  absl::flat_hash_set<int> reconstructed_pseudos;
  std::vector<int> phantom_pseudos;
};

SpillReversalOutcome ReconstructSpilledPseudosAcrossCfg(
    const ParsedCompilerFlags& parsed_flags, const MipsReloadFrameLayout& layout,
    bool frame_pointer_needed, int64_t pre_caller_save_top,
    const absl::flat_hash_map<int, int>& reg_renumber, int max_existing_regno,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
    absl::flat_hash_set<int>* reversed_reload_regs, absl::flat_hash_set<int>* proven_spill_regs,
    absl::flat_hash_set<int>* folded_scratch_pseudos,
    absl::flat_hash_set<int>* target_note_pseudos) {
  SpillReversalOutcome outcome;
  outcome.pre_reload_frame_top = pre_caller_save_top;

  if (parsed_flags.opt_level <= 0 || pre_caller_save_top <= layout.starting_frame_offset) {
    return outcome;
  }

  const absl::flat_hash_map<int, PseudoUseSummary> summaries =
      BuildPseudoUseSummaries(*all_parsed_blocks);

  // `alter_reg(p, -1)` allocates 8-byte `BIGGEST_ALIGNMENT` big-endian stack
  // slots at the top of the pre-`caller-save` frame (`rel_slot_base = 8 * k`,
  // accessed at `slot_base + (8 - ModeByteSize(M))`).
  const int64_t rel_top = pre_caller_save_top - layout.starting_frame_offset;
  if (rel_top < 8 || (rel_top % 8) != 0) {
    return outcome;
  }

  struct EightByteSlotInfo {
    bool invalidated = false;
    bool has_access = false;
    bool has_load = false;
    bool has_store = false;
    bool has_liveness_note = false;
    RtlMachineMode mode = RtlMachineMode::kNone;
    int64_t exact_offset = -1;
  };
  const int num_slots = static_cast<int>(rel_top / 8);
  std::vector<EightByteSlotInfo> slots(num_slots);
  int min_escaped_slot = num_slots;

  auto analyze_expr = [&](const RtlExpr& root, bool in_reg_note, bool is_liveness_note) {
    auto walk = [&](auto& self, const RtlExpr& expr, bool inside_mem_addr,
                    bool is_set_dest) -> void {
      if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
        self(self, expr.children[0], /*inside_mem_addr=*/false, /*is_set_dest=*/true);
        self(self, expr.children[1], /*inside_mem_addr=*/false, /*is_set_dest=*/false);
        return;
      }
      if (expr.op == RtlOp::kClobber && !expr.children.empty()) {
        self(self, expr.children[0], /*inside_mem_addr=*/false, /*is_set_dest=*/true);
        return;
      }
      if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
        std::optional<int64_t> off = MatchStackSlotAddrOffset(
            expr.children[0], frame_pointer_needed, layout.starting_frame_offset);
        if (off.has_value() && *off < pre_caller_save_top) {
          const int sz = std::max(1, MipsModeByteSize(expr.mode));
          const int64_t rel_lo = *off - layout.starting_frame_offset;
          const int64_t rel_hi = rel_lo + sz;
          const int first_s = static_cast<int>(rel_lo / 8);
          const int last_s = static_cast<int>(std::min<int64_t>(num_slots - 1, (rel_hi - 1) / 8));
          if (expr.flags.volatil || expr.flags.in_struct || expr.mode == RtlMachineMode::kBLK ||
              sz > 8 || first_s != last_s ||
              (layout.float_store && IsFloatingPointMode(expr.mode))) {
            for (int s = std::max(0, first_s); s <= last_s; ++s) {
              slots[s].invalidated = true;
            }
            return;
          }
          const int64_t slot_base = layout.starting_frame_offset + 8 * int64_t{first_s};
          const int64_t expected_bigend_off = slot_base + (8 - sz);
          EightByteSlotInfo& info = slots[first_s];
          if (*off != expected_bigend_off) {
            info.invalidated = true;
            return;
          }
          if (!info.has_access) {
            info.has_access = true;
            info.mode = expr.mode;
            info.exact_offset = *off;
          } else if (info.mode != expr.mode || info.exact_offset != *off) {
            info.invalidated = true;
          }
          if (is_liveness_note) {
            info.has_liveness_note = true;
          } else if (!in_reg_note) {
            if (is_set_dest) {
              info.has_store = true;
            } else {
              info.has_load = true;
            }
          }
          return;
        }
        self(self, expr.children[0], /*inside_mem_addr=*/true, /*is_set_dest=*/false);
        return;
      }
      if (!inside_mem_addr) {
        std::optional<int64_t> addr_off = ExtractElimToRegOffset(expr, layout.elim_to_reg);
        if (addr_off.has_value() && *addr_off >= layout.starting_frame_offset &&
            *addr_off < pre_caller_save_top) {
          const int s = static_cast<int>((*addr_off - layout.starting_frame_offset) / 8);
          if (s >= 0 && s < num_slots) {
            slots[s].invalidated = true;
            min_escaped_slot = std::min(min_escaped_slot, s);
          }
          return;
        }
      }
      const bool pass_dest =
          is_set_dest && (expr.op == RtlOp::kSubreg || expr.op == RtlOp::kStrictLowPart ||
                          expr.op == RtlOp::kZeroExtract || expr.op == RtlOp::kSignExtract);
      for (size_t ci = 0; ci < expr.children.size(); ++ci) {
        self(self, expr.children[ci], inside_mem_addr, pass_dest && ci == 0);
      }
    };
    walk(walk, root, /*inside_mem_addr=*/false, /*is_set_dest=*/false);
  };

  for (const auto& blk_insns : *all_parsed_blocks) {
    for (const RtlPatternAndNotes& insn : blk_insns) {
      analyze_expr(insn.pattern, /*in_reg_note=*/false, /*is_liveness_note=*/false);
      for (const RtlRegNote& note : insn.reg_notes) {
        const bool is_live_note =
            (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused);
        analyze_expr(note.datum, /*in_reg_note=*/true, is_live_note);
      }
      for (const RtlExpr& fu : insn.call_fusage) {
        analyze_expr(fu, /*in_reg_note=*/false, /*is_liveness_note=*/false);
      }
    }
  }

  for (int s = min_escaped_slot; s < num_slots; ++s) {
    if (slots[s].has_store && !slots[s].has_load && !slots[s].has_liveness_note) {
      slots[s].invalidated = true;
    }
  }

  // Find the contiguous suffix `[non_invalidated_begin, num_slots)` of
  // 8-byte-aligned local slots not invalidated by pre-reload local aggregates,
  // address-taken variables, or non-big-endian-aligned accesses.
  int non_invalidated_begin = num_slots;
  while (non_invalidated_begin > 0 && !slots[non_invalidated_begin - 1].invalidated) {
    --non_invalidated_begin;
  }

  // If pre-reload local frame slots exist (`non_invalidated_begin > 0`, e.g.,
  // an address-taken local array whose base slot was invalidated above), any
  // contiguous unaccessed slots immediately above `non_invalidated_begin - 1`
  // stay in `pre_reload_frame_size` as upper elements of that local aggregate.
  // When `non_invalidated_begin == 0`, no RTL-referenced pre-reload local
  // aggregate exists and `[0, num_slots)` starts directly at
  // `starting_frame_offset` (see residual ambiguity #4 below).
  int first_spill_slot = non_invalidated_begin;
  if (non_invalidated_begin > 0) {
    while (first_spill_slot < num_slots && !slots[first_spill_slot].has_access) {
      ++first_spill_slot;
    }
  }
  if (first_spill_slot == num_slots) {
    return outcome;
  }

  // Across `[first_spill_slot, num_slots)`, allocate pseudo numbers in ascending
  // 8-byte slot order (`reload1.c:654-658` runs `alter_reg(i, -1)` in ascending
  // regno order before `reload1.c:663` and `setup_save_areas()` at `reload1.c:1446`).
  // Each accessed slot becomes a real spilled pseudo, and each unreferenced
  // 8-byte gap becomes a synthesized phantom spilled pseudo (`combine.c:56`
  // leaves `reg_n_refs > 0, reg_n_sets == 0` when `combine` eliminates all RTL
  // references to a temporary after `flow_analysis`, so under high register
  // pressure `local-alloc`/`global-alloc` leave `reg_renumber[p] == -1` and
  // `alter_reg(p, -1)` allocates an unused 8-byte slot).
  //
  // Structural distinction from iteration-0 `caller-save` (`setup_save_areas`,
  // `caller-save.c:224-227, 315`) unused slots:
  //   1. `alter_reg(p, -1)` runs BEFORE `setup_save_areas()`, so whenever any
  //      active caller-save `MEM` or call-crossing call-clobbered hard reg
  //      exists, `ReverseCallerSaveAcrossCfg` sets `pre_caller_save_top` at the
  //      base of the caller-save region, excluding unused `setup_save_areas()`
  //      slots above `pre_caller_save_top`.
  //   2. `setup_save_areas()` uses `align = 0` (4-byte GPR alignment) whereas
  //      `alter_reg(p, -1)` and `reload1.c:663` use 8-byte `BIGGEST_ALIGNMENT`,
  //      so any trailing 4-byte odd-GPR `setup_save_areas()` slot is stripped
  //      by `ReverseCallerSaveAcrossCfg`.
  //   3. Residual structural ambiguity (unused `caller-save` multiple-of-8 gap):
  //      if `flag_caller_saves` is active on a non-leaf function that exhausted
  //      all callee-saved regs (`global.c:1078-1094`), iteration 0
  //      `setup_save_areas()` allocated an exact multiple of 8 bytes (e.g., two
  //      4-byte GPRs), AND `reload` subsequently spilled *every* such
  //      call-clobbered hard reg onto rematerializable constants (leaving zero
  //      active caller-save `MEM`s and zero call-crossing call-clobbered hard
  //      regs in `.09.greg`), the resulting 8-byte unused frame gap is
  //      bit-for-bit indistinguishable in `.09.greg` from an unused `combine`
  //      phantom `alter_reg` slot and is attributed to a phantom spilled pseudo.
  //   4. Residual structural ambiguity (`non_invalidated_begin == 0` with an
  //      unreferenced local aggregate): when a function declares an unreferenced
  //      8-byte-aligned local aggregate at `starting_frame_offset` (e.g.,
  //      `char buf[8];` allocated by `stmt.c:expand_decl` -> `assign_stack_local`
  //      with zero RTL occurrences in `.08.lreg` and no invalidated local slot
  //      above it), `reload` preserves that unaccessed 8-byte frame gap
  //      identically to an unallocated phantom `alter_reg(p, -1)` pseudo slot.
  //      Both pre-images satisfy $f(g(x)) = x$ across `SubfunctionId::kReload`;
  //      `InvertReload` canonically attributes the gap to a phantom spilled
  //      pseudo (`frame_size_bytes == 0` plus phantom `p`), which upstream
  //      `InvertGlobalAlloc`..`InvertExpandDecl` must be aware of.
  int next_pseudo = std::max(kFirstPseudoRegister, max_existing_regno + 1);
  absl::flat_hash_map<int64_t, RtlExpr> slot_to_pseudo;
  for (int s = first_spill_slot; s < num_slots; ++s) {
    const int p = next_pseudo++;
    if (slots[s].has_access) {
      slot_to_pseudo.emplace(slots[s].exact_offset, RtlExpr::MakeReg(slots[s].mode, p));
      outcome.reconstructed_pseudos.insert(p);
    } else {
      outcome.phantom_pseudos.push_back(p);
    }
  }

  const int64_t min_spill_base = layout.starting_frame_offset + 8 * int64_t{first_spill_slot};
  for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
    ReconstructSpilledPseudosInBlock(
        b, parsed_flags.opt_level, frame_pointer_needed, layout.gp64, layout.fp64,
        /*allow_pseudo_reload_reg=*/true, min_spill_base, pre_caller_save_top, reg_renumber,
        summaries, slot_to_pseudo, &(*all_parsed_blocks)[b], reversed_reload_regs,
        proven_spill_regs, folded_scratch_pseudos, target_note_pseudos);
  }
  outcome.pre_reload_frame_top = min_spill_base;
  return outcome;
}

bool ShouldSkipStackAddressUneliminationForInsn(const RtlExpr& pattern, bool frame_pointer_needed) {
  if ((pattern.op == RtlOp::kUse || pattern.op == RtlOp::kClobber) && !pattern.children.empty() &&
      pattern.children[0].op != RtlOp::kMem) {
    return true;
  }
  if (SingleElementModifiesStackPointer(pattern)) {
    return true;
  }
  if (pattern.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pattern.children) {
      if (SingleElementModifiesStackPointer(elem)) {
        return true;
      }
    }
  }
  if (frame_pointer_needed && pattern.op == RtlOp::kSet && !pattern.children.empty() &&
      pattern.children[0].IsHardReg() && pattern.children[0].RegNo() == kFramePointerRegnum) {
    return true;
  }
  return false;
}

enum class StackOffsetRegion {
  kOutgoingArgsSp,
  kLocalFrameFp,
  kIncomingArgsArgp,
};

StackOffsetRegion ClassifyStackOffsetRegion(int64_t offset, bool inside_mem,
                                            bool frame_pointer_needed, bool is_variadic,
                                            const MipsReloadFrameLayout& layout) {
  if (!frame_pointer_needed && offset < layout.starting_frame_offset) {
    // Special case: when `post_reload_tsize == 0` (meaning `starting_frame_offset == 0`
    // as well), `offset < 0` stays in local frame while `offset >= 0` is handled below.
    if (offset >= 0) {
      return StackOffsetRegion::kOutgoingArgsSp;
    }
    return StackOffsetRegion::kLocalFrameFp;
  }
  if (offset > layout.post_reload_tsize) {
    return StackOffsetRegion::kIncomingArgsArgp;
  }
  if (offset == layout.post_reload_tsize) {
    if (inside_mem) {
      return StackOffsetRegion::kIncomingArgsArgp;
    }
    // Outside `MEM` (`(plus to_reg (const_int tsize))` or bare `to_reg` when
    // `tsize == 0`): `fp + post_reload_var_size + starting_frame_offset` and
    // `argp + 0` coincide whenever `gp_reg_rounded == 0 && fp_reg_rounded == 0`.
    //   - When `post_reload_var_size > 0` in a non-variadic function with zero
    //     saved registers (`gp_reg_rounded == 0 && fp_reg_rounded == 0`) and no
    //     direct `MEM` access to incoming stack arguments
    //     (`!layout.has_incoming_arg_mem_access`), `offset == tsize` has a
    //     residual many-to-one ambiguity: pre-reload `$fp + tsize`
    //     (`&local_buf[N]` one-past-the-end of the local stack buffer) and
    //     pre-reload `(reg:SI 0 argp)` (taking `&stack_param` of an incoming
    //     stack parameter without ever loading any stack parameter via `MEM`)
    //     eliminate to the exact same `.09.greg` expression `$sp + tsize`, so
    //     both pre-images compile to identical `reload` output ($f(g(x)) = x$
    //     holds for both). `InvertReload` canonically chooses `kLocalFrameFp`
    //     (`&local_buf[N]`), because forming a one-past-the-end sentinel pointer
    //     to an already-allocated local buffer is standard in leaf loops whereas
    //     taking `&stack_param` in a zero-saved-reg leaf that allocates local
    //     stack storage yet never reads any stack parameter is pathological.
    //   - Otherwise (including `post_reload_tsize == 0` on a zero-frame leaf
    //     taking `&stack_param` or `va_start`, where no local stack object exists
    //     to form a one-past-the-end pointer), `offset == tsize` belongs to the
    //     caller's incoming argument build area `kIncomingArgsArgp` (`(reg:SI 0 argp)`).
    if (layout.gp_reg_rounded == 0 && layout.fp_reg_rounded == 0 &&
        layout.post_reload_var_size > 0 && !is_variadic && !layout.has_incoming_arg_mem_access) {
      return StackOffsetRegion::kLocalFrameFp;
    }
    return StackOffsetRegion::kIncomingArgsArgp;
  }
  return StackOffsetRegion::kLocalFrameFp;
}

bool ReverseEliminateRegsInExpr(RtlExpr* expr, bool inside_mem, bool frame_pointer_needed,
                                bool is_variadic, const MipsReloadFrameLayout& layout,
                                bool* restored_any_fp) {
  if (expr == nullptr) {
    return false;
  }

  const int to_reg = layout.elim_to_reg;

  if (expr->op == RtlOp::kMem && expr->children.size() == 1) {
    return ReverseEliminateRegsInExpr(&expr->children[0], /*inside_mem=*/true, frame_pointer_needed,
                                      is_variadic, layout, restored_any_fp);
  }

  if (expr->op == RtlOp::kSet && expr->children.size() >= 2) {
    bool changed = false;
    // Never rewrite bare `(reg:SI 29 sp)` / `(reg:SI 30 fp)` as direct `SET_DEST`,
    // because `eliminate_regs_in_insn` deletes any single-set whose `SET_DEST` is
    // an eliminable register (`fp` or `argp`).
    if (!IsElimToRegExpr(expr->children[0], to_reg)) {
      if (ReverseEliminateRegsInExpr(&expr->children[0], /*inside_mem=*/false, frame_pointer_needed,
                                     is_variadic, layout, restored_any_fp)) {
        changed = true;
      }
    }
    for (size_t i = 1; i < expr->children.size(); ++i) {
      if (ReverseEliminateRegsInExpr(&expr->children[i], /*inside_mem=*/false, frame_pointer_needed,
                                     is_variadic, layout, restored_any_fp)) {
        changed = true;
      }
    }
    return changed;
  }

  // Case 1: `(plus:SI (reg:SI to_reg) (const_int off))`.
  if (expr->op == RtlOp::kPlus && expr->mode == RtlMachineMode::kSI && expr->children.size() == 2 &&
      IsElimToRegExpr(expr->children[0], to_reg) && expr->children[1].op == RtlOp::kConstInt) {
    const int64_t off = expr->children[1].int_val;
    const StackOffsetRegion region =
        ClassifyStackOffsetRegion(off, inside_mem, frame_pointer_needed, is_variadic, layout);
    if (region == StackOffsetRegion::kOutgoingArgsSp) {
      return false;
    }
    if (region == StackOffsetRegion::kLocalFrameFp) {
      if (!frame_pointer_needed) {
        expr->children[0].SetRegNo(kFramePointerRegnum);
        expr->children[0].str_val = "$fp";
        if (restored_any_fp != nullptr) *restored_any_fp = true;
        return true;
      }
      return false;
    }
    const int64_t arg_off = off - layout.post_reload_tsize;
    if (arg_off == 0) {
      *expr = RtlExpr::MakeArgPointerReg();
    } else {
      expr->children[0] = RtlExpr::MakeArgPointerReg();
      expr->children[1] = RtlExpr::MakeConstInt(arg_off);
    }
    return true;
  }

  // Case 2: Indexed `form_sum` `(plus:SI (plus:SI (reg:SI to_reg) <idx>) (const_int off))`
  // or `(plus:SI <idx> (plus:SI (reg:SI to_reg) (const_int off)))`.
  if (expr->op == RtlOp::kPlus && expr->mode == RtlMachineMode::kSI && expr->children.size() == 2 &&
      expr->children[0].op == RtlOp::kPlus && expr->children[0].mode == RtlMachineMode::kSI &&
      expr->children[0].children.size() == 2 && expr->children[1].op == RtlOp::kConstInt) {
    RtlExpr& inner = expr->children[0];
    const int to_idx = IsElimToRegExpr(inner.children[0], to_reg)
                           ? 0
                           : (IsElimToRegExpr(inner.children[1], to_reg) ? 1 : -1);
    if (to_idx >= 0) {
      const int other_idx = 1 - to_idx;
      bool changed =
          ReverseEliminateRegsInExpr(&inner.children[other_idx], /*inside_mem=*/false,
                                     frame_pointer_needed, is_variadic, layout, restored_any_fp);
      const int64_t off = expr->children[1].int_val;
      const StackOffsetRegion region =
          ClassifyStackOffsetRegion(off, inside_mem, frame_pointer_needed, is_variadic, layout);
      if (region == StackOffsetRegion::kLocalFrameFp && !frame_pointer_needed) {
        inner.children[to_idx].SetRegNo(kFramePointerRegnum);
        inner.children[to_idx].str_val = "$fp";
        if (restored_any_fp != nullptr) *restored_any_fp = true;
        changed = true;
      } else if (region == StackOffsetRegion::kIncomingArgsArgp) {
        const int64_t arg_off = off - layout.post_reload_tsize;
        inner.children[to_idx] = RtlExpr::MakeArgPointerReg();
        if (arg_off == 0) {
          RtlExpr collapsed = std::move(inner);
          *expr = std::move(collapsed);
        } else {
          expr->children[1] = RtlExpr::MakeConstInt(arg_off);
        }
        changed = true;
      }
      return changed;
    }
  }

  // Case 3: Bare `(reg:SI to_reg)` (`offset == 0`, including inside
  // `(plus:SI (reg:SI to_reg) <idx>)` when `off == 0`).
  if (IsElimToRegExpr(*expr, to_reg)) {
    const StackOffsetRegion region = ClassifyStackOffsetRegion(
        /*offset=*/0, inside_mem, frame_pointer_needed, is_variadic, layout);
    if (region == StackOffsetRegion::kOutgoingArgsSp) {
      return false;
    }
    if (region == StackOffsetRegion::kLocalFrameFp) {
      if (!frame_pointer_needed) {
        expr->SetRegNo(kFramePointerRegnum);
        expr->str_val = "$fp";
        if (restored_any_fp != nullptr) *restored_any_fp = true;
        return true;
      }
      return false;
    }
    *expr = RtlExpr::MakeArgPointerReg();
    return true;
  }

  bool changed = false;
  for (RtlExpr& child : expr->children) {
    if (ReverseEliminateRegsInExpr(&child, inside_mem, frame_pointer_needed, is_variadic, layout,
                                   restored_any_fp)) {
      changed = true;
    }
  }
  return changed;
}

bool IsHardRegRangeDeadAfterInsn(
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks,
    const std::vector<RtlBasicBlock>& rpo_blocks, size_t block_idx,
    const std::vector<RtlPatternAndNotes>& cur_insns, size_t after_idx,
    const std::vector<bool>* cur_dropped, int start_hr, int nwords,
    const absl::flat_hash_map<int, int>& reg_renumber, bool gp64, bool fp64,
    bool check_implicit_return_regs = true);

// Reverses `reload1.c:emit_reload_insns` / `subst_reloads` for earlyclobber
// `(clobber (reg:M ...))` operands in a `PARALLEL` instruction where `reload`
// substituted a reload register `r_reload` into `(clobber (reg:M r_reload))`
// and emitted `(set (reg:M r_orig) (reg:M r_reload))` immediately after the
// `PARALLEL`. Supports both raw hard-register post-states and `InvertAlterReg`
// pseudo-lifted post-states.
void ReverseParallelClobberOutputReloads(
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks,
    const std::vector<RtlBasicBlock>& rpo_blocks, size_t block_idx,
    std::vector<RtlPatternAndNotes>* insns, int opt_level, bool gp64, bool fp64,
    const absl::flat_hash_map<int, int>& reg_renumber,
    const absl::flat_hash_map<int, PseudoUseSummary>& summaries,
    absl::flat_hash_set<int>* reversed_reload_regs, absl::flat_hash_set<int>* proven_spill_regs,
    absl::flat_hash_set<int>* folded_scratch_pseudos,
    absl::flat_hash_map<int, int>* unify_pseudo_map,
    absl::flat_hash_set<int>* unified_target_pseudos) {
  if (insns == nullptr || insns->empty()) {
    return;
  }
  std::vector<RtlPatternAndNotes> out;
  out.reserve(insns->size());

  for (size_t i = 0; i < insns->size(); ++i) {
    RtlPatternAndNotes current = (*insns)[i];
    if (current.pattern.op == RtlOp::kParallel) {
      absl::flat_hash_set<int> set_dest_hrs;
      absl::flat_hash_map<int, RtlExpr> set_hr_to_reg_expr;
      absl::flat_hash_map<int, size_t> clobber_hr_to_child_idx;

      for (size_t c = 0; c < current.pattern.children.size(); ++c) {
        const RtlExpr& elem = current.pattern.children[c];
        if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
          if (elem.children[0].op == RtlOp::kReg && !elem.children[0].IsArgPointerReg()) {
            const int d_hr = EffectiveHardReg(elem.children[0].RegNo(), reg_renumber);
            if (d_hr >= 0) {
              set_dest_hrs.insert(d_hr);
            }
          }
          elem.Visit([&](const RtlExpr& node) {
            if (node.op == RtlOp::kReg && !node.IsArgPointerReg()) {
              const int hr = EffectiveHardReg(node.RegNo(), reg_renumber);
              if (hr >= 0) {
                set_hr_to_reg_expr.try_emplace(hr, node);
              }
            }
          });
        } else if (elem.op == RtlOp::kClobber && !elem.children.empty() &&
                   elem.children[0].op == RtlOp::kReg && !elem.children[0].IsArgPointerReg()) {
          const int clob_hr = EffectiveHardReg(elem.children[0].RegNo(), reg_renumber);
          if (clob_hr >= 0) {
            clobber_hr_to_child_idx[clob_hr] = c;
          }
        }
      }

      bool consumed_set_dest_reload = false;
      if (i + 1 < insns->size() && !current.pattern.children.empty() &&
          current.pattern.children[0].op == RtlOp::kSet &&
          current.pattern.children[0].children.size() == 2) {
        const RtlExpr& set_dst = current.pattern.children[0].children[0];
        const RtlExpr& set_rhs = current.pattern.children[0].children[1];
        const RtlPatternAndNotes& mv = (*insns)[i + 1];
        if (set_dst.op == RtlOp::kReg && !set_dst.IsArgPointerReg() &&
            InsnHasOnlyDeadOrUnusedNotes(mv) && mv.pattern.op == RtlOp::kSet &&
            mv.pattern.children.size() == 2) {
          const RtlExpr& dst = mv.pattern.children[0];
          const RtlExpr& src = mv.pattern.children[1];
          if (dst.op == RtlOp::kReg && src.op == RtlOp::kReg && !dst.IsArgPointerReg() &&
              !src.IsArgPointerReg() && (opt_level > 0 || !dst.flags.integrated) &&
              dst.mode == set_dst.mode && src.mode == set_dst.mode &&
              src.RegNo() == set_dst.RegNo()) {
            const int orig_hr = EffectiveHardReg(dst.RegNo(), reg_renumber);
            const int reload_hr = EffectiveHardReg(src.RegNo(), reg_renumber);
            const int nw = (dst.mode == RtlMachineMode::kDi) ? 2 : 1;
            bool scratch_single_use = false;
            if (src.IsPseudoReg()) {
              auto sum_it = summaries.find(src.RegNo());
              scratch_single_use = sum_it != summaries.end() && !sum_it->second.multi_block &&
                                   sum_it->second.def_count == 1 && sum_it->second.use_count == 1;
            } else if (src.IsHardReg() && reload_hr >= 2 && reload_hr < 32 &&
                       mv.reg_notes.empty()) {
              scratch_single_use = IsHardRegRangeDeadAfterInsn(
                  all_parsed_blocks, rpo_blocks, block_idx, *insns, i + 1,
                  /*cur_dropped=*/nullptr, reload_hr, nw, reg_renumber, gp64, fp64);
            }
            bool clobber_overlaps_orig = false;
            for (int w = 0; w < nw; ++w) {
              if (clobber_hr_to_child_idx.contains(orig_hr + w)) {
                clobber_overlaps_orig = true;
              }
            }
            bool earlyclobber_triggered = false;
            if (orig_hr >= 2 && orig_hr < 32 && reload_hr >= 2 && reload_hr < 32 &&
                orig_hr != reload_hr && scratch_single_use && !clobber_overlaps_orig) {
              const bool is_di_bin_or_shift =
                  dst.mode == RtlMachineMode::kDi &&
                  (set_rhs.op == RtlOp::kPlus || set_rhs.op == RtlOp::kMinus ||
                   set_rhs.op == RtlOp::kAshift || set_rhs.op == RtlOp::kAshiftrt ||
                   set_rhs.op == RtlOp::kLshiftrt);
              const bool plus_imm_same_orig =
                  set_rhs.op == RtlOp::kPlus && set_rhs.children.size() == 2 &&
                  set_rhs.children[0].op == RtlOp::kReg && !set_rhs.children[0].IsArgPointerReg() &&
                  EffectiveHardReg(set_rhs.children[0].RegNo(), reg_renumber) == orig_hr &&
                  set_rhs.children[1].op == RtlOp::kConstInt;
              if ((is_di_bin_or_shift && !plus_imm_same_orig) || set_rhs.op == RtlOp::kFfs) {
                set_rhs.Visit([&](const RtlExpr& n) {
                  if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                    const int in_hr = EffectiveHardReg(n.RegNo(), reg_renumber);
                    const int in_nw = (n.mode == RtlMachineMode::kDi) ? 2 : 1;
                    if (in_hr >= 0 &&
                        std::max(orig_hr, in_hr) < std::min(orig_hr + nw, in_hr + in_nw)) {
                      earlyclobber_triggered = true;
                    }
                  }
                });
              }
            }
            if (earlyclobber_triggered) {
              current.pattern.children[0].children[0] = dst;
              if (reversed_reload_regs != nullptr) {
                for (int w = 0; w < nw; ++w) {
                  reversed_reload_regs->insert(reload_hr + w);
                }
              }
              if (proven_spill_regs != nullptr) {
                for (int w = 0; w < nw; ++w) {
                  proven_spill_regs->insert(reload_hr + w);
                }
              }
              if (folded_scratch_pseudos != nullptr && src.IsPseudoReg()) {
                folded_scratch_pseudos->insert(src.RegNo());
              }
              if (unified_target_pseudos != nullptr && dst.IsPseudoReg()) {
                unified_target_pseudos->insert(dst.RegNo());
              }
              consumed_set_dest_reload = true;
            }
          }
        }
      }

      std::vector<RtlPatternAndNotes> pre_copies;
      if (!clobber_hr_to_child_idx.empty()) {
        size_t lookahead = i + (consumed_set_dest_reload ? 2 : 1);
        while (lookahead < insns->size()) {
          const RtlPatternAndNotes& mv = (*insns)[lookahead];
          if (!InsnHasOnlyDeadOrUnusedNotes(mv) || mv.pattern.op != RtlOp::kSet ||
              mv.pattern.children.size() != 2) {
            break;
          }
          const RtlExpr& dst = mv.pattern.children[0];
          const RtlExpr& src = mv.pattern.children[1];
          if (dst.op != RtlOp::kReg || src.op != RtlOp::kReg || dst.IsArgPointerReg() ||
              src.IsArgPointerReg() || dst.mode != src.mode) {
            break;
          }
          const int orig_hr = EffectiveHardReg(dst.RegNo(), reg_renumber);
          const int reload_hr = EffectiveHardReg(src.RegNo(), reg_renumber);
          auto clob_it = clobber_hr_to_child_idx.find(reload_hr);
          auto orig_it = set_hr_to_reg_expr.find(orig_hr);
          if (clob_it == clobber_hr_to_child_idx.end() || set_dest_hrs.contains(reload_hr) ||
              orig_it == set_hr_to_reg_expr.end() || orig_it->second.mode != dst.mode) {
            break;
          }
          if (src.IsPseudoReg()) {
            auto sum_it = summaries.find(src.RegNo());
            if (sum_it == summaries.end() || sum_it->second.multi_block ||
                sum_it->second.use_count != 1 || sum_it->second.def_count != 0) {
              break;
            }
          }
          const RtlExpr orig_in_par = orig_it->second;
          bool lifted_addr_pseudo_copy = false;
          if (dst.IsPseudoReg() && !orig_in_par.IsPseudoReg()) {
            auto dst_sum_it = summaries.find(dst.RegNo());
            if (dst_sum_it == summaries.end() || dst_sum_it->second.multi_block ||
                dst_sum_it->second.def_count != 1) {
              break;
            }
            if (dst_sum_it->second.use_count > 0) {
              if (!orig_in_par.IsHardReg() || set_dest_hrs.contains(orig_hr) ||
                  current.pattern.children.empty() ||
                  current.pattern.children[0].op != RtlOp::kSet) {
                break;
              }
              const bool referenced_at_or_before =
                  std::any_of(insns->begin(), insns->begin() + static_cast<std::ptrdiff_t>(i + 1),
                              [&](const RtlPatternAndNotes& prev) {
                                return prev.pattern.ContainsReg(dst.RegNo());
                              });
              if (referenced_at_or_before ||
                  !ReplaceRegInExpr(&current.pattern.children[0], orig_in_par.RegNo(),
                                    orig_in_par.mode, dst)) {
                break;
              }
              RtlPatternAndNotes pre_copy;
              pre_copy.pattern = RtlExpr::MakeSet(dst, orig_in_par);
              pre_copies.push_back(std::move(pre_copy));
              RemoveDeadOrUnusedNotesForReg(&current.reg_notes, orig_in_par.RegNo());
              if (unified_target_pseudos != nullptr) {
                unified_target_pseudos->insert(dst.RegNo());
              }
              lifted_addr_pseudo_copy = true;
            }
          }
          const size_t child_idx = clob_it->second;
          current.pattern.children[child_idx].children[0] = orig_in_par;
          clobber_hr_to_child_idx.erase(clob_it);
          if (reversed_reload_regs != nullptr && reload_hr >= 0) {
            reversed_reload_regs->insert(reload_hr);
          }
          if (proven_spill_regs != nullptr && reload_hr >= 0) {
            proven_spill_regs->insert(reload_hr);
          }
          if (folded_scratch_pseudos != nullptr && src.IsPseudoReg()) {
            folded_scratch_pseudos->insert(src.RegNo());
          }
          if (folded_scratch_pseudos != nullptr && dst.IsPseudoReg() &&
              !orig_in_par.IsPseudoReg() && !lifted_addr_pseudo_copy) {
            folded_scratch_pseudos->insert(dst.RegNo());
          }
          if (dst.IsPseudoReg() && orig_in_par.IsPseudoReg() &&
              dst.RegNo() != orig_in_par.RegNo()) {
            if (unify_pseudo_map != nullptr) {
              (*unify_pseudo_map)[dst.RegNo()] = orig_in_par.RegNo();
            }
            if (unified_target_pseudos != nullptr) {
              unified_target_pseudos->insert(orig_in_par.RegNo());
            }
            RemoveDeadOrUnusedNotesForReg(&current.reg_notes, orig_in_par.RegNo());
          }
          ++lookahead;
        }
        i = lookahead - 1;
      } else if (consumed_set_dest_reload) {
        ++i;
      }
      for (auto it = pre_copies.rbegin(); it != pre_copies.rend(); ++it) {
        out.push_back(std::move(*it));
      }
    }
    out.push_back(std::move(current));
  }

  *insns = std::move(out);
}

bool IsFprOnlyRhsExpr(const RtlExpr& rhs) {
  const bool fp_mode = (rhs.mode == RtlMachineMode::kSF || rhs.mode == RtlMachineMode::kDF);
  switch (rhs.op) {
    case RtlOp::kPlus:
    case RtlOp::kMinus:
    case RtlOp::kMult:
    case RtlOp::kDiv:
    case RtlOp::kNeg:
    case RtlOp::kAbs:
    case RtlOp::kSqrt:
    case RtlOp::kFloat:
    case RtlOp::kUnsignedFloat:
    case RtlOp::kFloatExtend:
    case RtlOp::kFloatTruncate:
      return fp_mode;
    case RtlOp::kFix:
    case RtlOp::kUnsignedFix:
      return !rhs.children.empty() && (rhs.children[0].mode == RtlMachineMode::kSF ||
                                       rhs.children[0].mode == RtlMachineMode::kDF);
    case RtlOp::kEq:
    case RtlOp::kNe:
    case RtlOp::kLt:
    case RtlOp::kLe:
    case RtlOp::kGt:
    case RtlOp::kGe:
      return !rhs.children.empty() && (rhs.children[0].mode == RtlMachineMode::kSF ||
                                       rhs.children[0].mode == RtlMachineMode::kDF);
    default:
      return false;
  }
}

bool IsFprOnlyDestRhsExpr(const RtlExpr& rhs) {
  if (rhs.mode != RtlMachineMode::kSF && rhs.mode != RtlMachineMode::kDF) {
    return false;
  }
  switch (rhs.op) {
    case RtlOp::kPlus:
    case RtlOp::kMinus:
    case RtlOp::kMult:
    case RtlOp::kDiv:
    case RtlOp::kNeg:
    case RtlOp::kAbs:
    case RtlOp::kSqrt:
    case RtlOp::kFloat:
    case RtlOp::kUnsignedFloat:
    case RtlOp::kFloatExtend:
    case RtlOp::kFloatTruncate:
      return true;
    default:
      return false;
  }
}

bool IsHardRegRangeDeadAfterInsn(
    const std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks,
    const std::vector<RtlBasicBlock>& rpo_blocks, size_t block_idx,
    const std::vector<RtlPatternAndNotes>& cur_insns, size_t after_idx,
    const std::vector<bool>* cur_dropped, int start_hr, int nwords,
    const absl::flat_hash_map<int, int>& reg_renumber, bool gp64, bool fp64,
    bool check_implicit_return_regs) {
  if (start_hr < 0 || nwords <= 0) {
    return false;
  }
  const uint32_t init_mask = (nwords >= 32) ? ~uint32_t{0} : ((uint32_t{1} << nwords) - 1);
  auto overlaps_word_mask = [&](int r, RtlMachineMode m) -> uint32_t {
    const int hr = EffectiveHardReg(r, reg_renumber);
    if (hr < 0) {
      return 0;
    }
    const int nw = std::max(1, MipsHardRegNregs(hr, m, gp64, fp64));
    uint32_t mask = 0;
    for (int k = 0; k < nw; ++k) {
      const int off = (hr + k) - start_hr;
      if (off >= 0 && off < nwords) {
        mask |= (uint32_t{1} << off);
      }
    }
    return mask;
  };

  auto step_insn_mask = [&](const RtlPatternAndNotes& pn, uint32_t* mask) -> bool {
    const RtlExpr& pat = pn.pattern;
    uint32_t read_mask = 0;
    auto scan_reads = [&](const RtlExpr& elem) {
      if (elem.op == RtlOp::kClobber) {
        return;
      }
      if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
        const RtlExpr& d = elem.children[0];
        if (d.op != RtlOp::kReg) {
          d.Visit([&](const RtlExpr& n) {
            if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
              read_mask |= overlaps_word_mask(n.RegNo(), n.mode);
            }
          });
        }
        for (size_t c = 1; c < elem.children.size(); ++c) {
          elem.children[c].Visit([&](const RtlExpr& n) {
            if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
              read_mask |= overlaps_word_mask(n.RegNo(), n.mode);
            }
          });
        }
        return;
      }
      elem.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
          read_mask |= overlaps_word_mask(n.RegNo(), n.mode);
        }
      });
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& elem : pat.children) {
        scan_reads(elem);
      }
    } else {
      scan_reads(pat);
    }
    for (const RtlExpr& fu : pn.call_fusage) {
      scan_reads(fu);
    }
    if ((read_mask & *mask) != 0) {
      return false;
    }

    if (IsCallPattern(pat)) {
      for (int w = 0; w < nwords; ++w) {
        const int hr = start_hr + w;
        if ((hr >= 0 && hr <= 15) || hr == 24 || hr == 25 || hr == 31 || (hr >= 32 && hr <= 51) ||
            (hr >= 64 && hr <= 67)) {
          *mask &= ~(uint32_t{1} << w);
        }
      }
    }
    auto scan_kills = [&](const RtlExpr& elem) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty() &&
          elem.children[0].op == RtlOp::kReg && !elem.children[0].IsArgPointerReg()) {
        *mask &= ~overlaps_word_mask(elem.children[0].RegNo(), elem.children[0].mode);
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& elem : pat.children) {
        scan_kills(elem);
      }
    } else {
      scan_kills(pat);
    }
    return true;
  };

  auto has_live_exit_reg = [&](uint32_t mask) -> bool {
    for (int w = 0; w < nwords; ++w) {
      if ((mask & (uint32_t{1} << w)) != 0) {
        const int hr = start_hr + w;
        if (hr == 29 || hr == 30 ||
            (check_implicit_return_regs && (hr == 2 || hr == 3 || hr == 32 || hr == 33))) {
          return true;
        }
      }
    }
    return false;
  };

  uint32_t alive_mask = init_mask;
  for (size_t k = after_idx + 1; k < cur_insns.size() && alive_mask != 0; ++k) {
    if (cur_dropped != nullptr && k < cur_dropped->size() && (*cur_dropped)[k]) {
      continue;
    }
    if (!step_insn_mask(cur_insns[k], &alive_mask)) {
      return false;
    }
  }

  if (alive_mask == 0) {
    return true;
  }
  if (block_idx >= rpo_blocks.size()) {
    return false;
  }
  if (rpo_blocks[block_idx].successors.empty()) {
    return !has_live_exit_reg(alive_mask);
  }

  std::vector<uint32_t> seen_entry_masks(all_parsed_blocks.size(), 0);
  std::vector<std::pair<size_t, uint32_t>> worklist;
  for (size_t succ : rpo_blocks[block_idx].successors) {
    if (succ >= all_parsed_blocks.size() || succ >= rpo_blocks.size()) {
      return false;
    }
    const uint32_t new_bits = alive_mask & ~seen_entry_masks[succ];
    if (new_bits != 0) {
      seen_entry_masks[succ] |= new_bits;
      worklist.emplace_back(succ, new_bits);
    }
  }

  while (!worklist.empty()) {
    auto [blk, mask] = worklist.back();
    worklist.pop_back();
    const std::vector<RtlPatternAndNotes>& blk_insns = all_parsed_blocks[blk];
    for (size_t k = 0; k < blk_insns.size() && mask != 0; ++k) {
      if (!step_insn_mask(blk_insns[k], &mask)) {
        return false;
      }
    }
    if (mask == 0) {
      continue;
    }
    if (rpo_blocks[blk].successors.empty()) {
      if (has_live_exit_reg(mask)) {
        return false;
      }
      continue;
    }
    for (size_t succ : rpo_blocks[blk].successors) {
      if (succ >= all_parsed_blocks.size() || succ >= rpo_blocks.size()) {
        return false;
      }
      const uint32_t new_bits = mask & ~seen_entry_masks[succ];
      if (new_bits != 0) {
        seen_entry_masks[succ] |= new_bits;
        worklist.emplace_back(succ, new_bits);
      }
    }
  }
  return true;
}

int FirstRegPreorderIndex(const RtlExpr& expr, int target_regno) {
  int idx = 0;
  int found = -1;
  expr.Visit([&](const RtlExpr& n) {
    if (found < 0 && n.op == RtlOp::kReg && !n.IsArgPointerReg() && n.RegNo() == target_regno) {
      found = idx;
    }
    ++idx;
  });
  return (found >= 0) ? found : idx;
}

int CountRegOccurrencesInExpr(const RtlExpr& expr, int target_regno) {
  int count = 0;
  expr.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kReg && !n.IsArgPointerReg() && n.RegNo() == target_regno) {
      ++count;
    }
  });
  return count;
}

bool ExprContainsRegInsideMem(const RtlExpr& expr, int target_regno, bool inside_mem) {
  if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg() && expr.RegNo() == target_regno) {
    return inside_mem;
  }
  const bool next_inside_mem = inside_mem || expr.op == RtlOp::kMem;
  for (const RtlExpr& ch : expr.children) {
    if (ExprContainsRegInsideMem(ch, target_regno, next_inside_mem)) {
      return true;
    }
  }
  return false;
}

bool FoldMdGprInputReloads(const std::vector<RtlBasicBlock>& rpo_blocks, int opt_level, bool gp64,
                           bool fp64, const absl::flat_hash_map<int, int>& reg_renumber,
                           const absl::flat_hash_map<int, PseudoUseSummary>& summaries,
                           std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
                           absl::flat_hash_set<int>* reversed_reload_regs,
                           absl::flat_hash_set<int>* proven_spill_regs,
                           absl::flat_hash_set<int>* folded_scratch_pseudos,
                           absl::flat_hash_set<int>* target_note_pseudos) {
  bool any_folded = false;
  for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
    std::vector<RtlPatternAndNotes>& insns = (*all_parsed_blocks)[b];
    if (insns.size() < 2) {
      continue;
    }
    std::vector<bool> drop_md_in(insns.size(), false);
    for (int idx = static_cast<int>(insns.size()) - 2; idx >= 0; --idx) {
      const size_t i = static_cast<size_t>(idx);
      if (drop_md_in[i]) {
        continue;
      }
      const RtlPatternAndNotes& in_insn = insns[i];
      if (in_insn.pattern.op != RtlOp::kSet || in_insn.pattern.children.size() != 2) {
        continue;
      }
      const RtlExpr& r_gpr_expr = in_insn.pattern.children[0];
      const RtlExpr& p_md_expr = in_insn.pattern.children[1];
      if (r_gpr_expr.op != RtlOp::kReg || !p_md_expr.IsPseudoReg() ||
          r_gpr_expr.IsArgPointerReg() || r_gpr_expr.flags.Any() || p_md_expr.flags.Any() ||
          r_gpr_expr.mode != RtlMachineMode::kSI || p_md_expr.mode != RtlMachineMode::kSI ||
          !InsnHasOnlyOptionalDeadNoteOn(in_insn, p_md_expr.RegNo())) {
        continue;
      }
      const int r_gpr = r_gpr_expr.RegNo();
      const int p_md = p_md_expr.RegNo();
      const int gpr_hr = EffectiveHardReg(r_gpr, reg_renumber);
      const int md_hr = EffectiveHardReg(p_md, reg_renumber);
      if (gpr_hr < 2 || gpr_hr >= 32 || (md_hr != 64 && md_hr != 65)) {
        continue;
      }
      if (r_gpr >= kFirstPseudoRegister) {
        auto sum_it = summaries.find(r_gpr);
        if (sum_it == summaries.end() || sum_it->second.multi_block ||
            sum_it->second.def_count != 1 || sum_it->second.use_count != 1) {
          continue;
        }
      }

      for (size_t op_idx = i + 1; op_idx < insns.size(); ++op_idx) {
        if (drop_md_in[op_idx]) {
          continue;
        }
        RtlPatternAndNotes& op_insn = insns[op_idx];
        const RtlOp rhs_op =
            (op_insn.pattern.op == RtlOp::kSet && op_insn.pattern.children.size() == 2)
                ? op_insn.pattern.children[1].op
                : RtlOp::kUnknown;
        const bool is_opt_mem_store =
            opt_level > 0 && rhs_op == RtlOp::kReg && op_insn.pattern.op == RtlOp::kSet &&
            !op_insn.pattern.children.empty() && op_insn.pattern.children[0].op == RtlOp::kMem;
        const bool unsupported_rhs_op = (!is_opt_mem_store && rhs_op == RtlOp::kReg) ||
                                        rhs_op == RtlOp::kMem || rhs_op == RtlOp::kDiv ||
                                        rhs_op == RtlOp::kUdiv || rhs_op == RtlOp::kMod ||
                                        rhs_op == RtlOp::kUmod || rhs_op == RtlOp::kMult;
        if (op_insn.pattern.op == RtlOp::kSet && op_insn.pattern.children.size() == 2 &&
            !IsCallPattern(op_insn.pattern) && !IsControlFlowBoundaryPattern(op_insn.pattern) &&
            !unsupported_rhs_op && !op_insn.pattern.children[0].ContainsReg(r_gpr) &&
            CountRegOccurrencesInExpr(op_insn.pattern.children[1], r_gpr) == 1 &&
            !ExprContainsRegInsideMem(op_insn.pattern.children[1], r_gpr, false)) {
          const bool overwritten_by_op =
              op_insn.pattern.children[0].op == RtlOp::kReg &&
              !op_insn.pattern.children[0].IsArgPointerReg() &&
              EffectiveHardReg(op_insn.pattern.children[0].RegNo(), reg_renumber) == gpr_hr &&
              op_insn.pattern.children[0].mode == r_gpr_expr.mode;
          if (r_gpr < kFirstPseudoRegister && !overwritten_by_op &&
              !IsHardRegRangeDeadAfterInsn(*all_parsed_blocks, rpo_blocks, b, insns, op_idx,
                                           &drop_md_in, gpr_hr, 1, reg_renumber, gp64, fp64,
                                           /*check_implicit_return_regs=*/false)) {
            break;
          }
          if (!ReplaceRegInExpr(&op_insn.pattern.children[1], r_gpr, r_gpr_expr.mode, p_md_expr)) {
            break;
          }
          if (!overwritten_by_op) {
            RemoveDeadOrUnusedNotesForReg(&op_insn.reg_notes, r_gpr);
          }
          if (p_md >= kFirstPseudoRegister) {
            for (RtlRegNote& ex : op_insn.reg_notes) {
              if (ex.kind == RtlMachineMode::kRegDead && ex.datum.IsHardReg() &&
                  !ex.datum.IsArgPointerReg() && ex.datum.RegNo() == md_hr) {
                ex.datum.SetRegNo(p_md);
              }
            }
            RemoveDeadOrUnusedNotesForReg(&op_insn.reg_notes, md_hr);
            if (target_note_pseudos != nullptr) {
              target_note_pseudos->insert(p_md);
            }
          }
          for (const RtlRegNote& n : in_insn.reg_notes) {
            if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                !n.datum.IsArgPointerReg()) {
              const int dead_reg = n.datum.RegNo();
              const bool already_has_dead = std::any_of(
                  op_insn.reg_notes.begin(), op_insn.reg_notes.end(), [&](const RtlRegNote& ex) {
                    return ex.kind == RtlMachineMode::kRegDead && ex.datum.op == RtlOp::kReg &&
                           !ex.datum.IsArgPointerReg() && ex.datum.RegNo() == dead_reg;
                  });
              if (!already_has_dead) {
                const int my_ord = FirstRegPreorderIndex(op_insn.pattern.children[1], dead_reg);
                auto ins_it = std::find_if(
                    op_insn.reg_notes.begin(), op_insn.reg_notes.end(), [&](const RtlRegNote& ex) {
                      if (ex.kind == RtlMachineMode::kRegEquiv) {
                        return false;
                      }
                      if (ex.kind != RtlMachineMode::kRegDead || ex.datum.op != RtlOp::kReg ||
                          ex.datum.IsArgPointerReg()) {
                        return true;
                      }
                      return FirstRegPreorderIndex(op_insn.pattern.children[1], ex.datum.RegNo()) >
                             my_ord;
                    });
                op_insn.reg_notes.insert(ins_it, n);
              }
              if (target_note_pseudos != nullptr && n.datum.IsPseudoReg() &&
                  r_gpr >= kFirstPseudoRegister) {
                target_note_pseudos->insert(dead_reg);
              }
            }
          }
          drop_md_in[i] = true;
          any_folded = true;
          if (reversed_reload_regs != nullptr) {
            reversed_reload_regs->insert(gpr_hr);
          }
          if (proven_spill_regs != nullptr && !overwritten_by_op) {
            proven_spill_regs->insert(gpr_hr);
          }
          if (folded_scratch_pseudos != nullptr && r_gpr >= kFirstPseudoRegister) {
            folded_scratch_pseudos->insert(r_gpr);
          }
          break;
        }

        const bool inter_conflict =
            (r_gpr < kFirstPseudoRegister) || opt_level < 2 || op_insn.pattern.op == RtlOp::kNote ||
            IsCallPattern(op_insn.pattern) || IsControlFlowBoundaryPattern(op_insn.pattern) ||
            op_insn.pattern.ContainsReg(r_gpr) ||
            InsnWritesOrClobbersRegOrEffHr(op_insn.pattern, reg_renumber, gp64, fp64, p_md,
                                           md_hr) ||
            InsnWritesOrClobbersRegOrEffHr(op_insn.pattern, reg_renumber, gp64, fp64, r_gpr,
                                           gpr_hr);
        if (inter_conflict) {
          break;
        }
      }
    }
    std::vector<RtlPatternAndNotes> compact;
    compact.reserve(insns.size());
    for (size_t i = 0; i < insns.size(); ++i) {
      if (!drop_md_in[i]) {
        compact.push_back(std::move(insns[i]));
      }
    }
    insns = std::move(compact);
  }
  return any_folded;
}

bool IsSingleUseMdScratchHardGprBeforeCall(const std::vector<RtlPatternAndNotes>& insns,
                                           size_t mv_idx, const std::vector<bool>& drop_hi,
                                           int dst_hr,
                                           const absl::flat_hash_map<int, int>& reg_renumber,
                                           bool gp64, bool fp64) {
  if (dst_hr < 2 || dst_hr >= 32) {
    return false;
  }
  size_t op_idx = insns.size();
  for (size_t k = mv_idx + 1; k < insns.size(); ++k) {
    if (drop_hi[k]) {
      continue;
    }
    const RtlPatternAndNotes& op_insn = insns[k];
    if (op_insn.pattern.op == RtlOp::kSet && op_insn.pattern.children.size() == 2 &&
        !IsCallPattern(op_insn.pattern) && !IsControlFlowBoundaryPattern(op_insn.pattern) &&
        !op_insn.pattern.children[0].ContainsReg(dst_hr) &&
        CountRegOccurrencesInExpr(op_insn.pattern.children[1], dst_hr) == 1 &&
        !ExprContainsRegInsideMem(op_insn.pattern.children[1], dst_hr, false)) {
      const RtlOp rhs_op = op_insn.pattern.children[1].op;
      if (rhs_op != RtlOp::kReg && rhs_op != RtlOp::kMem && rhs_op != RtlOp::kDiv &&
          rhs_op != RtlOp::kUdiv && rhs_op != RtlOp::kMod && rhs_op != RtlOp::kUmod &&
          rhs_op != RtlOp::kMult) {
        op_idx = k;
      }
      break;
    }
    if (op_insn.pattern.ContainsReg(dst_hr) || IsCallPattern(op_insn.pattern) ||
        IsControlFlowBoundaryPattern(op_insn.pattern) ||
        InsnWritesOrClobbersRegOrEffHr(op_insn.pattern, reg_renumber, gp64, fp64, dst_hr, dst_hr)) {
      return false;
    }
  }
  if (op_idx >= insns.size()) {
    return false;
  }
  bool saw_arg0_setup = false;
  for (size_t k = op_idx + 1; k < insns.size(); ++k) {
    if (drop_hi[k]) {
      continue;
    }
    const RtlPatternAndNotes& cur = insns[k];
    if (IsCallPattern(cur.pattern)) {
      return saw_arg0_setup;
    }
    if (cur.pattern.ContainsReg(dst_hr) || IsControlFlowBoundaryPattern(cur.pattern) ||
        InsnWritesOrClobbersRegOrEffHr(cur.pattern, reg_renumber, gp64, fp64, dst_hr, dst_hr)) {
      return false;
    }
    if (cur.pattern.op == RtlOp::kSet && cur.pattern.children.size() == 2 &&
        cur.pattern.children[0].IsHardReg() && cur.pattern.children[0].RegNo() == 4) {
      saw_arg0_setup = true;
    }
  }
  return false;
}

bool FoldMdOutputReloads(const std::vector<RtlBasicBlock>& rpo_blocks, int opt_level, bool gp64,
                         bool fp64, const absl::flat_hash_map<int, int>& reg_renumber,
                         const absl::flat_hash_map<int, PseudoUseSummary>& summaries,
                         std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
                         absl::flat_hash_set<int>* reversed_reload_regs,
                         absl::flat_hash_set<int>* proven_spill_regs,
                         absl::flat_hash_set<int>* folded_scratch_pseudos,
                         absl::flat_hash_set<int>* target_note_pseudos) {
  const auto saved_blocks_before_md = *all_parsed_blocks;
  absl::flat_hash_set<int> md_folded_scratches;
  absl::flat_hash_set<int> md_proven_scratches;
  absl::flat_hash_set<int> md_target_notes;
  bool folded_md_64 = false;
  bool folded_md_65 = false;

  for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
    std::vector<RtlPatternAndNotes>& insns = (*all_parsed_blocks)[b];
    if (insns.size() < 2) {
      continue;
    }
    std::vector<bool> drop_hi(insns.size(), false);
    for (size_t i = 0; i + 1 < insns.size(); ++i) {
      if (drop_hi[i]) {
        continue;
      }
      RtlPatternAndNotes& mul_insn = insns[i];
      if (mul_insn.pattern.op != RtlOp::kParallel || mul_insn.pattern.children.size() < 2) {
        continue;
      }
      RtlExpr& elem0 = mul_insn.pattern.children[0];
      if (elem0.op != RtlOp::kSet || elem0.children.size() != 2 ||
          elem0.children[0].op != RtlOp::kReg || elem0.children[0].IsArgPointerReg() ||
          elem0.children[0].flags.Any()) {
        continue;
      }
      const int md_regno = elem0.children[0].RegNo();
      const int md_hr = EffectiveHardReg(md_regno, reg_renumber);
      const RtlMachineMode md_mode = elem0.children[0].mode;
      const RtlOp rhs_op = elem0.children[1].op;
      const bool valid_md_pattern =
          (md_hr == 64 && md_mode == RtlMachineMode::kDI && rhs_op == RtlOp::kMult) ||
          (md_hr == 65 && md_mode == RtlMachineMode::kSI && rhs_op == RtlOp::kMult) ||
          (md_hr == 64 && md_mode == RtlMachineMode::kSI && rhs_op == RtlOp::kTruncate);
      if (!valid_md_pattern) {
        continue;
      }
      const int md_nw = std::max(1, MipsHardRegNregs(md_hr, md_mode, gp64, fp64));
      if (md_regno >= kFirstPseudoRegister) {
        auto sum_it = summaries.find(md_regno);
        if (sum_it == summaries.end() || sum_it->second.multi_block ||
            sum_it->second.def_count != 1 || sum_it->second.use_count != 1) {
          continue;
        }
      }

      const size_t max_md_mv_idx =
          (opt_level == 0 || opt_level >= 2) ? (insns.size() - 1) : (i + 1);
      for (size_t mv_idx = i + 1; mv_idx <= max_md_mv_idx; ++mv_idx) {
        if (drop_hi[mv_idx]) {
          continue;
        }
        const RtlPatternAndNotes& mv_insn = insns[mv_idx];
        if (mv_insn.pattern.op == RtlOp::kSet && mv_insn.pattern.children.size() == 2) {
          const RtlExpr& mv_dst = mv_insn.pattern.children[0];
          const RtlExpr& mv_src = mv_insn.pattern.children[1];
          const bool valid_mv_dst =
              mv_dst.IsPseudoReg() || (mv_dst.IsHardReg() && mv_dst.RegNo() < 32);
          const bool matches_primary = mv_src.op == RtlOp::kReg && !mv_src.IsArgPointerReg() &&
                                       mv_src.mode == md_mode && mv_src.RegNo() == md_regno &&
                                       InsnHasOnlyOptionalDeadNoteOn(mv_insn, md_regno);
          const bool pseudo_md_dies_at_mv =
              md_regno < kFirstPseudoRegister ||
              std::any_of(
                  mv_insn.reg_notes.begin(), mv_insn.reg_notes.end(), [&](const RtlRegNote& n) {
                    return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                           !n.datum.IsArgPointerReg() && n.datum.RegNo() == md_regno;
                  });
          bool allowed_non_adjacent_input_reload = true;
          if (mv_idx > i + 1 && (md_regno < kFirstPseudoRegister || opt_level < 2)) {
            allowed_non_adjacent_input_reload = false;
            if (mv_dst.IsPseudoReg()) {
              auto dst_sum = summaries.find(mv_dst.RegNo());
              if (dst_sum != summaries.end() && !dst_sum->second.multi_block &&
                  dst_sum->second.def_count == 1 && dst_sum->second.use_count == 1) {
                allowed_non_adjacent_input_reload = true;
              }
            } else if (mv_dst.IsHardReg() && mv_dst.RegNo() >= 2 && mv_dst.RegNo() < 32) {
              allowed_non_adjacent_input_reload = IsSingleUseMdScratchHardGprBeforeCall(
                  insns, mv_idx, drop_hi, mv_dst.RegNo(), reg_renumber, gp64, fp64);
            }
          }
          if (valid_mv_dst && mv_dst.mode == md_mode && matches_primary && pseudo_md_dies_at_mv &&
              allowed_non_adjacent_input_reload) {
            const int dst_hr = EffectiveHardReg(mv_dst.RegNo(), reg_renumber);
            if (dst_hr < 32) {
              const int dst_nw =
                  (dst_hr >= 0) ? std::max(1, MipsHardRegNregs(dst_hr, mv_dst.mode, gp64, fp64))
                                : 1;
              bool intervening_conflict = false;
              for (size_t k = i + 1; k < mv_idx; ++k) {
                if (drop_hi[k]) {
                  continue;
                }
                if (insns[k].pattern.ContainsReg(mv_dst.RegNo())) {
                  intervening_conflict = true;
                  break;
                }
                if (dst_hr >= 0) {
                  for (int h = dst_hr; h < dst_hr + dst_nw; ++h) {
                    if (InsnWritesOrClobbersRegOrEffHr(insns[k].pattern, reg_renumber, gp64, fp64,
                                                       mv_dst.RegNo(), h)) {
                      intervening_conflict = true;
                      break;
                    }
                  }
                  if (!intervening_conflict) {
                    insns[k].pattern.Visit([&](const RtlExpr& n) {
                      if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                        const int nhr = EffectiveHardReg(n.RegNo(), reg_renumber);
                        if (nhr >= 0) {
                          const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
                          if (nhr < dst_hr + dst_nw && dst_hr < nhr + nnw) {
                            intervening_conflict = true;
                          }
                        }
                      }
                    });
                  }
                  if (intervening_conflict) {
                    break;
                  }
                }
              }
              if (!intervening_conflict &&
                  (md_regno >= kFirstPseudoRegister ||
                   IsHardRegRangeDeadAfterInsn(*all_parsed_blocks, rpo_blocks, b, insns, mv_idx,
                                               &drop_hi, md_hr, md_nw, reg_renumber, gp64, fp64))) {
                if (md_regno < kFirstPseudoRegister && mv_dst.IsHardReg() && !mv_dst.flags.Any() &&
                    mv_dst.mode == RtlMachineMode::kSI &&
                    IsSingleUseMdScratchHardGprBeforeCall(insns, mv_idx, drop_hi, dst_hr,
                                                          reg_renumber, gp64, fp64)) {
                  md_proven_scratches.insert(dst_hr);
                }
                elem0.children[0] = mv_dst;
                RemoveDeadOrUnusedNotesForReg(&mul_insn.reg_notes, md_regno);
                if (mv_dst.IsPseudoReg()) {
                  md_target_notes.insert(mv_dst.RegNo());
                  for (size_t k = mv_idx + 1; k < insns.size(); ++k) {
                    if (drop_hi[k]) {
                      continue;
                    }
                    if (insns[k].pattern.ContainsReg(mv_dst.RegNo())) {
                      for (RtlRegNote& rn : insns[k].reg_notes) {
                        if (rn.kind == RtlMachineMode::kRegDead && rn.datum.IsHardReg() &&
                            !rn.datum.IsArgPointerReg() && rn.datum.RegNo() == md_hr) {
                          rn.datum = mv_dst;
                        }
                      }
                    }
                    if (IsControlFlowBoundaryPattern(insns[k].pattern) ||
                        InsnWritesOrClobbersRegOrEffHr(insns[k].pattern, reg_renumber, gp64, fp64,
                                                       mv_dst.RegNo(), md_hr)) {
                      break;
                    }
                  }
                }
                drop_hi[mv_idx] = true;
                if (md_mode == RtlMachineMode::kDI) {
                  folded_md_64 = true;
                  folded_md_65 = true;
                } else if (md_hr == 64) {
                  folded_md_64 = true;
                } else if (md_hr == 65) {
                  folded_md_65 = true;
                }
                if (md_regno >= kFirstPseudoRegister) {
                  md_folded_scratches.insert(md_regno);
                }
              }
            }
            break;
          }
        }
        if (IsControlFlowBoundaryPattern(mv_insn.pattern) ||
            mv_insn.pattern.ContainsReg(md_regno) ||
            InsnWritesOrClobbersRegOrEffHr(mv_insn.pattern, reg_renumber, gp64, fp64, md_regno,
                                           64) ||
            InsnWritesOrClobbersRegOrEffHr(mv_insn.pattern, reg_renumber, gp64, fp64, md_regno,
                                           65) ||
            InsnWritesOrClobbersRegOrEffHr(mv_insn.pattern, reg_renumber, gp64, fp64, md_regno,
                                           66)) {
          break;
        }
      }
    }
    std::vector<RtlPatternAndNotes> compact;
    compact.reserve(insns.size());
    for (size_t i = 0; i < insns.size(); ++i) {
      if (!drop_hi[i]) {
        compact.push_back(std::move(insns[i]));
      }
    }
    insns = std::move(compact);
  }

  bool explicit_md_survives = false;
  if (folded_md_64 || folded_md_65) {
    auto check_non_clobber_md = [&](const auto& self, const RtlExpr& expr) -> void {
      if (expr.op == RtlOp::kClobber) {
        return;
      }
      if (expr.IsHardReg()) {
        const int hr = expr.RegNo();
        const int nw = std::max(1, MipsHardRegNregs(hr, expr.mode, gp64, fp64));
        if (folded_md_64 && hr <= 64 && 64 < hr + nw) {
          explicit_md_survives = true;
        }
        if (folded_md_65 && hr <= 65 && 65 < hr + nw) {
          explicit_md_survives = true;
        }
        return;
      }
      for (const RtlExpr& ch : expr.children) {
        self(self, ch);
      }
    };
    for (const auto& blk_insns : *all_parsed_blocks) {
      for (const RtlPatternAndNotes& insn : blk_insns) {
        check_non_clobber_md(check_non_clobber_md, insn.pattern);
      }
    }
  }
  if (explicit_md_survives) {
    *all_parsed_blocks = saved_blocks_before_md;
    return false;
  }
  if (folded_scratch_pseudos != nullptr) {
    folded_scratch_pseudos->insert(md_folded_scratches.begin(), md_folded_scratches.end());
  }
  if (reversed_reload_regs != nullptr) {
    reversed_reload_regs->insert(md_proven_scratches.begin(), md_proven_scratches.end());
  }
  if (proven_spill_regs != nullptr) {
    proven_spill_regs->insert(md_proven_scratches.begin(), md_proven_scratches.end());
  }
  if (target_note_pseudos != nullptr) {
    target_note_pseudos->insert(md_target_notes.begin(), md_target_notes.end());
  }
  return folded_md_64 || folded_md_65;
}

bool RestoreO0MdScratchClobbers(bool gp64, bool fp64,
                                const absl::flat_hash_map<int, int>& reg_renumber,
                                const absl::flat_hash_map<int, PseudoUseSummary>& summaries,
                                std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
                                absl::flat_hash_set<int>* folded_scratch_pseudos) {
  bool non_clobber_md_used[3] = {false, false, false};
  auto mark_non_clobber_md = [&](const auto& self, const RtlExpr& expr) -> void {
    if (expr.op == RtlOp::kClobber) {
      return;
    }
    if (expr.IsHardReg()) {
      const int hr = expr.RegNo();
      const int nw = std::max(1, MipsHardRegNregs(hr, expr.mode, gp64, fp64));
      for (int h = std::max(64, hr); h <= std::min(66, hr + nw - 1); ++h) {
        non_clobber_md_used[h - 64] = true;
      }
      return;
    }
    for (const RtlExpr& ch : expr.children) {
      self(self, ch);
    }
  };
  for (const auto& blk_insns : *all_parsed_blocks) {
    for (const RtlPatternAndNotes& insn : blk_insns) {
      mark_non_clobber_md(mark_non_clobber_md, insn.pattern);
    }
  }

  auto contains_asm = [](const RtlExpr& root) {
    bool found = false;
    root.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kAsmOperands || n.op == RtlOp::kAsmInput) {
        found = true;
      }
    });
    return found;
  };
  bool any_restored = false;
  for (auto& blk_insns : *all_parsed_blocks) {
    for (RtlPatternAndNotes& insn : blk_insns) {
      if (insn.pattern.op != RtlOp::kParallel || insn.pattern.children.size() < 2 ||
          contains_asm(insn.pattern)) {
        continue;
      }
      for (size_t c_idx = 1; c_idx < insn.pattern.children.size(); ++c_idx) {
        RtlExpr& clob = insn.pattern.children[c_idx];
        if (clob.op != RtlOp::kClobber || clob.children.size() != 1 ||
            clob.children[0].op != RtlOp::kReg || clob.children[0].IsArgPointerReg()) {
          continue;
        }
        const int c_reg = clob.children[0].RegNo();
        const int c_hr = EffectiveHardReg(c_reg, reg_renumber);
        if (c_hr < 64 || c_hr > 66) {
          continue;
        }
        const int c_nw = std::max(1, MipsHardRegNregs(c_hr, clob.children[0].mode, gp64, fp64));
        bool md_busy = false;
        for (int h = c_hr; h < c_hr + c_nw && h <= 66; ++h) {
          if (non_clobber_md_used[h - 64]) {
            md_busy = true;
            break;
          }
        }
        if (md_busy) {
          continue;
        }
        if (c_reg >= kFirstPseudoRegister) {
          auto sum_it = summaries.find(c_reg);
          if (sum_it == summaries.end() || sum_it->second.use_count != 0) {
            continue;
          }
          if (folded_scratch_pseudos != nullptr) {
            folded_scratch_pseudos->insert(c_reg);
          }
        }
        RemoveDeadOrUnusedNotesForReg(&insn.reg_notes, c_reg);
        clob.children[0] = RtlExpr::MakeScratch(clob.children[0].mode);
        any_restored = true;
      }
    }
  }
  return any_restored;
}

void ReverseMdAndDiZeroConstraintReloads(
    const std::vector<RtlBasicBlock>& rpo_blocks, int opt_level, bool gp64, bool fp64,
    const absl::flat_hash_map<int, int>& reg_renumber,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
    absl::flat_hash_map<int, PseudoUseSummary>* summaries_ptr,
    absl::flat_hash_set<int>* reversed_reload_regs, absl::flat_hash_set<int>* proven_spill_regs,
    absl::flat_hash_set<int>* folded_scratch_pseudos,
    absl::flat_hash_set<int>* target_note_pseudos) {
  absl::flat_hash_map<int, PseudoUseSummary>& summaries = *summaries_ptr;

  // Pass A.0: Fold `DImode` `(const_int 0)` store reloads on 32-bit GPR MIPS.
  if (!gp64) {
    bool folded_di_zero = false;
    for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
      std::vector<RtlPatternAndNotes>& insns = (*all_parsed_blocks)[b];
      if (insns.size() < 2) {
        continue;
      }
      std::vector<bool> drop_zero(insns.size(), false);
      for (size_t i = 0; i + 1 < insns.size(); ++i) {
        if (drop_zero[i]) {
          continue;
        }
        const RtlPatternAndNotes& ld_zero = insns[i];
        RtlPatternAndNotes& st_mem = insns[i + 1];
        if (ld_zero.pattern.op != RtlOp::kSet || ld_zero.pattern.children.size() != 2 ||
            !ld_zero.reg_notes.empty() || st_mem.pattern.op != RtlOp::kSet ||
            st_mem.pattern.children.size() != 2) {
          continue;
        }
        const RtlExpr& r_tmp = ld_zero.pattern.children[0];
        const RtlExpr& zero_rhs = ld_zero.pattern.children[1];
        const RtlExpr& mem_dst = st_mem.pattern.children[0];
        const RtlExpr& st_src = st_mem.pattern.children[1];
        if (!r_tmp.IsPseudoReg() || r_tmp.flags.Any() || r_tmp.mode != RtlMachineMode::kDI ||
            zero_rhs.op != RtlOp::kConstInt || zero_rhs.int_val != 0 || mem_dst.op != RtlOp::kMem ||
            mem_dst.mode != RtlMachineMode::kDI || !st_src.IsPseudoReg() || st_src.flags.Any() ||
            st_src.mode != RtlMachineMode::kDI || st_src.RegNo() != r_tmp.RegNo()) {
          continue;
        }
        const int p_tmp = r_tmp.RegNo();
        const int tmp_hr = EffectiveHardReg(p_tmp, reg_renumber);
        if (tmp_hr < 2 || tmp_hr >= 31) {
          continue;
        }
        auto sum_it = summaries.find(p_tmp);
        if (sum_it == summaries.end() || sum_it->second.multi_block ||
            sum_it->second.def_count != 1 || sum_it->second.use_count != 1) {
          continue;
        }
        bool addr_conflicts = false;
        mem_dst.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
            const int nhr = EffectiveHardReg(n.RegNo(), reg_renumber);
            if (n.RegNo() == p_tmp || nhr == tmp_hr || nhr == tmp_hr + 1) {
              addr_conflicts = true;
            }
          }
        });
        if (addr_conflicts) {
          continue;
        }
        st_mem.pattern.children[1] = RtlExpr::MakeConstInt(0);
        RemoveDeadOrUnusedNotesForReg(&st_mem.reg_notes, p_tmp);
        drop_zero[i] = true;
        folded_di_zero = true;
        if (folded_scratch_pseudos != nullptr) {
          folded_scratch_pseudos->insert(p_tmp);
        }
      }
      std::vector<RtlPatternAndNotes> compact;
      compact.reserve(insns.size());
      for (size_t i = 0; i < insns.size(); ++i) {
        if (!drop_zero[i]) {
          compact.push_back(std::move(insns[i]));
        }
      }
      insns = std::move(compact);
    }
    if (folded_di_zero) {
      summaries = BuildPseudoUseSummaries(*all_parsed_blocks);
    }
  }

  // Pass A.1: Fold `"d"` GPR input reloads from `MD_REGS`-allocated pseudos
  // (`64 hi` / `65 lo`) before `Pass A` folds multi-use / address / copy output
  // reloads into `GR_REGS`.
  if (FoldMdGprInputReloads(rpo_blocks, opt_level, gp64, fp64, reg_renumber, summaries,
                            all_parsed_blocks, reversed_reload_regs, proven_spill_regs,
                            folded_scratch_pseudos, target_note_pseudos)) {
    summaries = BuildPseudoUseSummaries(*all_parsed_blocks);
  }

  // Pass A: Fold remaining `MD_REGS` (`64 hi` / `65 lo`) output reloads into
  // `GR_REGS`.
  if (FoldMdOutputReloads(rpo_blocks, opt_level, gp64, fp64, reg_renumber, summaries,
                          all_parsed_blocks, reversed_reload_regs, proven_spill_regs,
                          folded_scratch_pseudos, target_note_pseudos)) {
    summaries = BuildPseudoUseSummaries(*all_parsed_blocks);
  }

  // Pass A.3: At `-O0`, restore `(clobber (scratch:...))` on `match_scratch`
  // `MD_REGS` clobbers so `stupid_mark_refs` does not mark `MD_REGS` live.
  if (opt_level == 0 && RestoreO0MdScratchClobbers(gp64, fp64, reg_renumber, summaries,
                                                   all_parsed_blocks, folded_scratch_pseudos)) {
    summaries = BuildPseudoUseSummaries(*all_parsed_blocks);
  }
}

void ReverseFprConstraintReloads(const std::vector<RtlBasicBlock>& rpo_blocks, int opt_level,
                                 bool gp64, bool fp64,
                                 const absl::flat_hash_map<int, int>& reg_renumber,
                                 const absl::flat_hash_map<int, PseudoUseSummary>& summaries,
                                 std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
                                 absl::flat_hash_set<int>* reversed_reload_regs,
                                 absl::flat_hash_set<int>* proven_spill_regs,
                                 absl::flat_hash_set<int>* folded_scratch_pseudos,
                                 absl::flat_hash_set<int>* target_note_pseudos,
                                 absl::flat_hash_set<int>* combine_priority_dead_regs) {
  for (size_t b = 0; b < all_parsed_blocks->size(); ++b) {
    std::vector<RtlPatternAndNotes>& insns = (*all_parsed_blocks)[b];
    if (insns.size() < 2) {
      continue;
    }

    // Pass B: Fold FPR (`32..63`) input reloads for `"f"`-only FP operations
    // (scanning backward so consecutive input reloads into the same FP op fold
    // cleanly, and scanning forward across non-interfering `sched2` insns when
    // `!only_hard_reload_regs`):
    //   `(set (reg:SF/DF r_fpr) (reg:SF/DF r_src))`
    //   ... (optional non-interfering `sched2` insns)
    //   `(set <op_dst> <fp_only_rhs(... r_fpr ...)>)`
    {
      std::vector<bool> drop_fp_in(insns.size(), false);
      for (int idx = static_cast<int>(insns.size()) - 2; idx >= 0; --idx) {
        const size_t i = static_cast<size_t>(idx);
        if (drop_fp_in[i]) {
          continue;
        }
        const RtlPatternAndNotes& in_insn = insns[i];
        if (in_insn.pattern.op != RtlOp::kSet || in_insn.pattern.children.size() != 2) {
          continue;
        }
        const RtlExpr& r_fpr_expr = in_insn.pattern.children[0];
        const RtlExpr& r_src_expr = in_insn.pattern.children[1];
        if (r_fpr_expr.op != RtlOp::kReg || r_src_expr.op != RtlOp::kReg ||
            r_fpr_expr.IsArgPointerReg() || r_src_expr.IsArgPointerReg() ||
            r_fpr_expr.flags.Any() || r_fpr_expr.mode != r_src_expr.mode ||
            (r_fpr_expr.mode != RtlMachineMode::kSF && r_fpr_expr.mode != RtlMachineMode::kDF) ||
            !InsnHasOnlyOptionalDeadNoteOn(in_insn, r_src_expr.RegNo())) {
          continue;
        }
        if (!r_src_expr.IsPseudoReg()) {
          if (!r_src_expr.IsHardReg() || r_src_expr.RegNo() < 2 || r_src_expr.RegNo() >= 32) {
            continue;
          }
        }
        const int r_fpr = r_fpr_expr.RegNo();
        const int fpr_hr = EffectiveHardReg(r_fpr, reg_renumber);
        const int src_hr = EffectiveHardReg(r_src_expr.RegNo(), reg_renumber);
        if (fpr_hr < 32 || fpr_hr >= 64 || src_hr >= 32) {
          continue;
        }
        if (!r_fpr_expr.IsHardReg()) {
          continue;
        }
        const int nw = std::max(1, MipsHardRegNregs(fpr_hr, r_fpr_expr.mode, gp64, fp64));
        const int src_nw =
            (src_hr >= 0) ? std::max(1, MipsHardRegNregs(src_hr, r_src_expr.mode, gp64, fp64)) : 1;

        const size_t max_op_idx =
            (opt_level == 0) ? std::min(i + 2, insns.size() - 1) : (insns.size() - 1);
        for (size_t op_idx = i + 1; op_idx <= max_op_idx; ++op_idx) {
          if (drop_fp_in[op_idx]) {
            continue;
          }
          RtlPatternAndNotes& op_insn = insns[op_idx];
          if (op_insn.pattern.op == RtlOp::kSet && op_insn.pattern.children.size() == 2 &&
              IsFprOnlyRhsExpr(op_insn.pattern.children[1]) &&
              op_insn.pattern.children[1].ContainsReg(r_fpr)) {
            const bool overwritten_by_op =
                op_insn.pattern.children[0].op == RtlOp::kReg &&
                !op_insn.pattern.children[0].IsArgPointerReg() &&
                EffectiveHardReg(op_insn.pattern.children[0].RegNo(), reg_renumber) == fpr_hr &&
                op_insn.pattern.children[0].mode == r_fpr_expr.mode;
            if (r_fpr < kFirstPseudoRegister && !overwritten_by_op &&
                !IsHardRegRangeDeadAfterInsn(*all_parsed_blocks, rpo_blocks, b, insns, op_idx,
                                             &drop_fp_in, fpr_hr, nw, reg_renumber, gp64, fp64)) {
              break;
            }
            if (!ReplaceRegInExpr(&op_insn.pattern.children[1], r_fpr, r_fpr_expr.mode,
                                  r_src_expr)) {
              break;
            }
            if (!overwritten_by_op) {
              RemoveDeadOrUnusedNotesForReg(&op_insn.reg_notes, r_fpr);
            }
            for (const RtlRegNote& n : in_insn.reg_notes) {
              if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                  !n.datum.IsArgPointerReg()) {
                const int dead_reg = n.datum.RegNo();
                const bool already_has_dead = std::any_of(
                    op_insn.reg_notes.begin(), op_insn.reg_notes.end(), [&](const RtlRegNote& ex) {
                      return ex.kind == RtlMachineMode::kRegDead && ex.datum.op == RtlOp::kReg &&
                             !ex.datum.IsArgPointerReg() && ex.datum.RegNo() == dead_reg;
                    });
                if (!already_has_dead) {
                  const int my_ord = FirstRegPreorderIndex(op_insn.pattern.children[1], dead_reg);
                  auto ins_it =
                      std::find_if(op_insn.reg_notes.begin(), op_insn.reg_notes.end(),
                                   [&](const RtlRegNote& ex) {
                                     if (ex.kind == RtlMachineMode::kRegEquiv) {
                                       return false;
                                     }
                                     if (ex.kind != RtlMachineMode::kRegDead ||
                                         ex.datum.op != RtlOp::kReg || ex.datum.IsArgPointerReg()) {
                                       return true;
                                     }
                                     return FirstRegPreorderIndex(op_insn.pattern.children[1],
                                                                  ex.datum.RegNo()) > my_ord;
                                   });
                  op_insn.reg_notes.insert(ins_it, n);
                }
                if (target_note_pseudos != nullptr && n.datum.IsPseudoReg() &&
                    r_fpr >= kFirstPseudoRegister) {
                  target_note_pseudos->insert(dead_reg);
                }
              }
            }
            drop_fp_in[i] = true;
            if (reversed_reload_regs != nullptr) {
              for (int k = 0; k < nw; ++k) {
                reversed_reload_regs->insert(fpr_hr + k);
              }
            }
            if (proven_spill_regs != nullptr && !overwritten_by_op) {
              for (int k = 0; k < nw; ++k) {
                proven_spill_regs->insert(fpr_hr + k);
              }
            }
            if (folded_scratch_pseudos != nullptr && r_fpr >= kFirstPseudoRegister) {
              folded_scratch_pseudos->insert(r_fpr);
            }
            break;
          }

          bool inter_conflict =
              (r_fpr < kFirstPseudoRegister) || opt_level < 2 || IsCallPattern(op_insn.pattern) ||
              IsControlFlowBoundaryPattern(op_insn.pattern) || op_insn.pattern.ContainsReg(r_fpr) ||
              InsnWritesOrClobbersRegOrEffHr(op_insn.pattern, reg_renumber, gp64, fp64,
                                             r_src_expr.RegNo(), -1);
          for (int w = 0; w < nw && !inter_conflict; ++w) {
            if (InsnWritesOrClobbersRegOrEffHr(op_insn.pattern, reg_renumber, gp64, fp64, r_fpr,
                                               fpr_hr + w)) {
              inter_conflict = true;
            }
          }
          for (int w = 0; src_hr >= 0 && w < src_nw && !inter_conflict; ++w) {
            if (InsnWritesOrClobbersRegOrEffHr(op_insn.pattern, reg_renumber, gp64, fp64, -1,
                                               src_hr + w)) {
              inter_conflict = true;
            }
          }
          if (inter_conflict) {
            break;
          }
        }
      }
      std::vector<RtlPatternAndNotes> compact;
      compact.reserve(insns.size());
      for (size_t i = 0; i < insns.size(); ++i) {
        if (!drop_fp_in[i]) {
          compact.push_back(std::move(insns[i]));
        }
      }
      insns = std::move(compact);
    }

    // Pass C: Fold FPR (`32..63`) output reloads for `"=f"`-only FP operations
    // targeting a GPR (`0..31`) or unallocated pseudo:
    //   `(set (reg:SF/DF r_fpr) <fp_only_dest_rhs>)`
    //   ... (optional non-interfering `sched2` insns when `!only_hard_reload_regs`)
    //   `(set (reg:SF/DF r_dst) (reg:SF/DF r_fpr))`
    {
      std::vector<bool> drop_fp_out(insns.size(), false);
      for (size_t i = 0; i + 1 < insns.size(); ++i) {
        if (drop_fp_out[i]) {
          continue;
        }
        RtlPatternAndNotes& op_insn = insns[i];
        if (op_insn.pattern.op != RtlOp::kSet || op_insn.pattern.children.size() != 2) {
          continue;
        }
        const RtlExpr& r_fpr_expr = op_insn.pattern.children[0];
        if (r_fpr_expr.op != RtlOp::kReg || r_fpr_expr.IsArgPointerReg() ||
            r_fpr_expr.flags.Any() || !IsFprOnlyDestRhsExpr(op_insn.pattern.children[1])) {
          continue;
        }
        const int r_fpr = r_fpr_expr.RegNo();
        if (!r_fpr_expr.IsHardReg()) {
          continue;
        }
        const int fpr_hr = EffectiveHardReg(r_fpr, reg_renumber);
        if (fpr_hr < 32 || fpr_hr >= 64) {
          continue;
        }
        const int nw = std::max(1, MipsHardRegNregs(fpr_hr, r_fpr_expr.mode, gp64, fp64));
        bool rhs_uses_fpr_hr = false;
        op_insn.pattern.children[1].Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
            const int nhr = EffectiveHardReg(n.RegNo(), reg_renumber);
            if (nhr >= 0) {
              const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
              if (nhr < fpr_hr + nw && fpr_hr < nhr + nnw) {
                rhs_uses_fpr_hr = true;
              }
            }
          }
        });

        const size_t max_mv_idx = (opt_level < 2) ? (i + 1) : (insns.size() - 1);
        for (size_t mv_idx = i + 1; mv_idx <= max_mv_idx; ++mv_idx) {
          if (drop_fp_out[mv_idx]) {
            continue;
          }
          const RtlPatternAndNotes& mv_insn = insns[mv_idx];
          if (mv_insn.pattern.op == RtlOp::kSet && mv_insn.pattern.children.size() == 2) {
            const RtlExpr& mv_dst = mv_insn.pattern.children[0];
            const RtlExpr& mv_src = mv_insn.pattern.children[1];
            const int dst_hr = (mv_dst.op == RtlOp::kReg && !mv_dst.IsArgPointerReg())
                                   ? EffectiveHardReg(mv_dst.RegNo(), reg_renumber)
                                   : 64;
            if (opt_level == 0 && r_fpr >= kFirstPseudoRegister &&
                !(mv_dst.IsPseudoReg() && dst_hr >= 2 && dst_hr < 32)) {
              break;
            }
            // `dst_hr` in `[0, 32)` (GPR or GPR-allocated pseudo) or `-1`
            // (unallocated pseudo) requires an `"=f"` FPR output reload.
            if (mv_dst.op == RtlOp::kReg && !mv_dst.IsArgPointerReg() && mv_src.op == RtlOp::kReg &&
                !mv_src.IsArgPointerReg() && r_fpr == mv_src.RegNo() &&
                r_fpr_expr.mode == mv_src.mode && mv_dst.mode == mv_src.mode && dst_hr < 32 &&
                InsnHasOnlyOptionalDeadNoteOn(mv_insn, mv_src.RegNo())) {
              const int dst_nw =
                  (dst_hr >= 0) ? std::max(1, MipsHardRegNregs(dst_hr, mv_dst.mode, gp64, fp64))
                                : 1;

              const RtlExpr mv_dst_copy = mv_dst;
              bool fpr_or_cfg_conflict = (mv_idx > i + 1 && dst_hr >= 0 && rhs_uses_fpr_hr);
              bool dst_interferes_before_mv = false;
              for (size_t k = i + 1; k < mv_idx && !fpr_or_cfg_conflict; ++k) {
                if (drop_fp_out[k]) {
                  continue;
                }
                const RtlExpr& mid = insns[k].pattern;
                if (IsCallPattern(mid) || IsControlFlowBoundaryPattern(mid) ||
                    mid.ContainsReg(r_fpr)) {
                  fpr_or_cfg_conflict = true;
                  break;
                }
                for (int w = 0; w < nw && !fpr_or_cfg_conflict; ++w) {
                  if (InsnWritesOrClobbersRegOrEffHr(mid, reg_renumber, gp64, fp64, r_fpr,
                                                     fpr_hr + w)) {
                    fpr_or_cfg_conflict = true;
                  }
                }
                if (fpr_or_cfg_conflict) {
                  break;
                }
                if (mid.ContainsReg(mv_dst_copy.RegNo()) ||
                    InsnWritesOrClobbersRegOrEffHr(mid, reg_renumber, gp64, fp64,
                                                   mv_dst_copy.RegNo(), -1)) {
                  dst_interferes_before_mv = true;
                }
                if (dst_hr >= 0 && !dst_interferes_before_mv) {
                  for (int w = 0; w < dst_nw; ++w) {
                    if (InsnWritesOrClobbersRegOrEffHr(mid, reg_renumber, gp64, fp64,
                                                       mv_dst_copy.RegNo(), dst_hr + w)) {
                      dst_interferes_before_mv = true;
                      break;
                    }
                  }
                  if (!dst_interferes_before_mv) {
                    mid.Visit([&](const RtlExpr& n) {
                      if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                        const int nhr = EffectiveHardReg(n.RegNo(), reg_renumber);
                        if (nhr >= 0) {
                          const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
                          if (nhr < dst_hr + dst_nw && dst_hr < nhr + nnw) {
                            dst_interferes_before_mv = true;
                          }
                        }
                      }
                    });
                  }
                }
              }

              bool sink_op_to_mv = false;
              bool pre_mv_conflict = fpr_or_cfg_conflict;
              if (!pre_mv_conflict && dst_interferes_before_mv) {
                bool can_sink = (opt_level >= 2);
                bool rhs_has_mem = false;
                std::vector<std::pair<int, RtlMachineMode>> rhs_regs;
                if (can_sink) {
                  op_insn.pattern.children[1].Visit([&](const RtlExpr& n) {
                    if (n.op == RtlOp::kMem) {
                      rhs_has_mem = true;
                    } else if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                      rhs_regs.emplace_back(n.RegNo(), n.mode);
                    }
                  });
                }
                auto insn_writes_mem = [](const RtlExpr& pat) {
                  auto elem_writes_mem = [](const RtlExpr& elem) {
                    if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) &&
                        !elem.children.empty()) {
                      const RtlExpr* d = &elem.children[0];
                      while (d->op == RtlOp::kSubreg && !d->children.empty()) {
                        d = &d->children[0];
                      }
                      return d->op == RtlOp::kMem;
                    }
                    return false;
                  };
                  if (pat.op == RtlOp::kParallel) {
                    return std::any_of(pat.children.begin(), pat.children.end(), elem_writes_mem);
                  }
                  return elem_writes_mem(pat);
                };
                for (size_t k = i + 1; k < mv_idx && can_sink; ++k) {
                  if (drop_fp_out[k]) {
                    continue;
                  }
                  const RtlExpr& mid = insns[k].pattern;
                  if (rhs_has_mem && insn_writes_mem(mid)) {
                    can_sink = false;
                    break;
                  }
                  for (const auto& [r_no, r_mode] : rhs_regs) {
                    if (InsnWritesOrClobbersRegOrEffHr(mid, reg_renumber, gp64, fp64, r_no, -1)) {
                      can_sink = false;
                      break;
                    }
                    const int r_hr = EffectiveHardReg(r_no, reg_renumber);
                    if (r_hr >= 0) {
                      const int r_nw = std::max(1, MipsHardRegNregs(r_hr, r_mode, gp64, fp64));
                      for (int w = 0; w < r_nw; ++w) {
                        if (InsnWritesOrClobbersRegOrEffHr(mid, reg_renumber, gp64, fp64, r_no,
                                                           r_hr + w)) {
                          can_sink = false;
                          break;
                        }
                      }
                      if (!can_sink) {
                        break;
                      }
                    }
                  }
                }
                if (can_sink) {
                  sink_op_to_mv = true;
                } else {
                  pre_mv_conflict = true;
                }
              }

              if (!pre_mv_conflict) {
                auto count_fpr_read_hits = [&](const RtlExpr& pat) -> int {
                  int hits = 0;
                  auto visit_read = [&](const RtlExpr& sub) {
                    sub.Visit([&](const RtlExpr& n) {
                      if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                        if (r_fpr >= kFirstPseudoRegister) {
                          if (n.RegNo() == r_fpr) ++hits;
                        } else if (n.IsHardReg()) {
                          const int nhr = n.RegNo();
                          const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
                          if (nhr < fpr_hr + nw && fpr_hr < nhr + nnw) ++hits;
                        }
                      }
                    });
                  };
                  auto scan_elem = [&](const RtlExpr& elem) {
                    if (elem.op == RtlOp::kClobber) {
                      return;
                    }
                    if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
                      if (elem.children[0].op != RtlOp::kReg) {
                        visit_read(elem.children[0]);
                      }
                      for (size_t c = 1; c < elem.children.size(); ++c) {
                        visit_read(elem.children[c]);
                      }
                      return;
                    }
                    visit_read(elem);
                  };
                  if (pat.op == RtlOp::kParallel) {
                    for (const RtlExpr& elem : pat.children) {
                      scan_elem(elem);
                    }
                  } else {
                    scan_elem(pat);
                  }
                  return hits;
                };

                // Scan forward for any reload-inherited reads of `r_fpr` (in `mv_idx + 1 ..`)
                // before `r_fpr` or `mv_dst` is overwritten or control flow splits.
                int inherited_reads = 0;
                size_t last_inh_idx = mv_idx;
                bool inh_conflict = false;
                for (size_t m = mv_idx + 1; m < insns.size(); ++m) {
                  if (drop_fp_out[m]) {
                    continue;
                  }
                  const int before_hits = count_fpr_read_hits(insns[m].pattern);
                  if (before_hits > 0) {
                    RtlExpr probe = insns[m].pattern;
                    int after_hits = before_hits;
                    if (IsFprOnlyRhsExpr(probe.op == RtlOp::kSet && probe.children.size() == 2
                                             ? probe.children[1]
                                             : probe) &&
                        ReplaceReadRegInPattern(&probe, r_fpr, r_fpr_expr.mode, mv_dst)) {
                      after_hits = count_fpr_read_hits(probe);
                    }
                    if (after_hits != 0) {
                      inh_conflict = true;
                      break;
                    }
                    inherited_reads += before_hits;
                    last_inh_idx = m;
                  }

                  bool kills_fpr_or_dst =
                      InsnWritesOrClobbersRegOrEffHr(insns[m].pattern, reg_renumber, gp64, fp64,
                                                     mv_dst.RegNo(), -1) ||
                      IsControlFlowBoundaryPattern(insns[m].pattern) ||
                      IsCallPattern(insns[m].pattern);
                  for (int k = 0; k < nw && !kills_fpr_or_dst; ++k) {
                    if (InsnWritesOrClobbersRegOrEffHr(insns[m].pattern, reg_renumber, gp64, fp64,
                                                       r_fpr, fpr_hr + k)) {
                      kills_fpr_or_dst = true;
                    }
                  }
                  if (dst_hr >= 0 && !kills_fpr_or_dst) {
                    for (int k = 0; k < dst_nw; ++k) {
                      if (InsnWritesOrClobbersRegOrEffHr(insns[m].pattern, reg_renumber, gp64, fp64,
                                                         mv_dst.RegNo(), dst_hr + k)) {
                        kills_fpr_or_dst = true;
                        break;
                      }
                    }
                  }
                  if (kills_fpr_or_dst) {
                    break;
                  }
                }

                bool uses_ok = !inh_conflict;
                if (uses_ok) {
                  if (r_fpr >= kFirstPseudoRegister) {
                    int rem_uses = 0;
                    for (size_t m = i + 1; m < insns.size(); ++m) {
                      if (drop_fp_out[m]) continue;
                      rem_uses += count_fpr_read_hits(insns[m].pattern);
                      if (InsnWritesOrClobbersRegOrEffHr(insns[m].pattern, reg_renumber, gp64, fp64,
                                                         r_fpr, -1)) {
                        break;
                      }
                    }
                    auto sum_it = summaries.find(r_fpr);
                    if (sum_it == summaries.end() || sum_it->second.multi_block ||
                        1 + inherited_reads != rem_uses) {
                      uses_ok = false;
                    }
                  } else if (!IsHardRegRangeDeadAfterInsn(*all_parsed_blocks, rpo_blocks, b, insns,
                                                          last_inh_idx, &drop_fp_out, fpr_hr, nw,
                                                          reg_renumber, gp64, fp64)) {
                    uses_ok = false;
                  }
                }

                if (uses_ok) {
                  bool rhs_mentions_fpr = false;
                  std::optional<RtlExpr> rhs_fpr_input_reg;
                  op_insn.pattern.children[1].Visit([&](const RtlExpr& n) {
                    if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
                      const int nhr = EffectiveHardReg(n.RegNo(), reg_renumber);
                      if (nhr >= 0) {
                        const int nnw = std::max(1, MipsHardRegNregs(nhr, n.mode, gp64, fp64));
                        if (nhr < fpr_hr + nw && fpr_hr < nhr + nnw) {
                          rhs_mentions_fpr = true;
                          if (!rhs_fpr_input_reg.has_value()) {
                            rhs_fpr_input_reg = n;
                          }
                        }
                      }
                    }
                  });
                  const RtlExpr saved_r_fpr_expr = r_fpr_expr;
                  op_insn.pattern.children[0] = mv_dst_copy;
                  if (op_insn.pattern.children[1].ContainsReg(r_fpr)) {
                    op_insn.reg_notes.erase(
                        std::remove_if(op_insn.reg_notes.begin(), op_insn.reg_notes.end(),
                                       [&](const RtlRegNote& n) {
                                         return n.kind == RtlMachineMode::kRegUnused &&
                                                n.datum.op == RtlOp::kReg &&
                                                !n.datum.IsArgPointerReg() &&
                                                n.datum.RegNo() == r_fpr;
                                       }),
                        op_insn.reg_notes.end());
                  } else {
                    RemoveDeadOrUnusedNotesForReg(&op_insn.reg_notes, r_fpr);
                  }
                  if (rhs_fpr_input_reg.has_value()) {
                    const int prio_reg = rhs_fpr_input_reg->RegNo();
                    op_insn.reg_notes.erase(
                        std::remove_if(op_insn.reg_notes.begin(), op_insn.reg_notes.end(),
                                       [&](const RtlRegNote& n) {
                                         return n.kind == RtlMachineMode::kRegDead &&
                                                n.datum.op == RtlOp::kReg &&
                                                !n.datum.IsArgPointerReg() &&
                                                n.datum.RegNo() == prio_reg;
                                       }),
                        op_insn.reg_notes.end());
                    auto prio_ins_it = std::find_if(
                        op_insn.reg_notes.begin(), op_insn.reg_notes.end(),
                        [](const RtlRegNote& ex) { return ex.kind != RtlMachineMode::kRegEquiv; });
                    op_insn.reg_notes.insert(prio_ins_it, RtlRegNote{
                                                              .kind = RtlMachineMode::kRegDead,
                                                              .datum = *rhs_fpr_input_reg,
                                                          });
                    if (combine_priority_dead_regs != nullptr) {
                      combine_priority_dead_regs->insert(prio_reg);
                    }
                  }
                  if (inherited_reads > 0) {
                    for (size_t m = mv_idx + 1; m <= last_inh_idx; ++m) {
                      if (!drop_fp_out[m] &&
                          ReplaceReadRegInPattern(&insns[m].pattern, r_fpr, saved_r_fpr_expr.mode,
                                                  mv_dst_copy)) {
                        if (!insns[m].pattern.ContainsReg(r_fpr)) {
                          RemoveDeadOrUnusedNotesForReg(&insns[m].reg_notes, r_fpr);
                        }
                      }
                    }
                    if (target_note_pseudos != nullptr && mv_dst_copy.IsPseudoReg()) {
                      target_note_pseudos->insert(mv_dst_copy.RegNo());
                    }
                  }
                  if (sink_op_to_mv) {
                    insns[mv_idx] = std::move(op_insn);
                    drop_fp_out[i] = true;
                  } else {
                    drop_fp_out[mv_idx] = true;
                  }
                  if (reversed_reload_regs != nullptr) {
                    for (int k = 0; k < nw; ++k) {
                      reversed_reload_regs->insert(fpr_hr + k);
                    }
                  }
                  if (proven_spill_regs != nullptr && !rhs_mentions_fpr) {
                    for (int k = 0; k < nw; ++k) {
                      proven_spill_regs->insert(fpr_hr + k);
                    }
                  }
                  if (folded_scratch_pseudos != nullptr && r_fpr >= kFirstPseudoRegister) {
                    folded_scratch_pseudos->insert(r_fpr);
                  }
                  break;
                }
              }
            }
          }

          bool stop_scan = IsCallPattern(mv_insn.pattern) ||
                           IsControlFlowBoundaryPattern(mv_insn.pattern) ||
                           mv_insn.pattern.ContainsReg(r_fpr);
          for (int w = 0; w < nw && !stop_scan; ++w) {
            if (InsnWritesOrClobbersRegOrEffHr(mv_insn.pattern, reg_renumber, gp64, fp64, r_fpr,
                                               fpr_hr + w)) {
              stop_scan = true;
            }
          }
          if (stop_scan) {
            break;
          }
        }
      }
      std::vector<RtlPatternAndNotes> compact;
      compact.reserve(insns.size());
      for (size_t i = 0; i < insns.size(); ++i) {
        if (!drop_fp_out[i]) {
          compact.push_back(std::move(insns[i]));
        }
      }
      insns = std::move(compact);
    }

    // Pass D: Reverse `reload()` inheritance of an FPR-allocated pseudo `p_fpr`
    // past its own pre-reload `REG_DEAD p_fpr` note on `(set (reg:SF/DF r_gpr) (reg:SF/DF p_fpr))`.
    {
      for (size_t i = 0; i + 1 < insns.size(); ++i) {
        const RtlPatternAndNotes& mv_insn = insns[i];
        if (mv_insn.pattern.op != RtlOp::kSet || mv_insn.pattern.children.size() != 2) {
          continue;
        }
        const RtlExpr& r_gpr_expr = mv_insn.pattern.children[0];
        const RtlExpr& p_fpr_expr = mv_insn.pattern.children[1];
        if (!r_gpr_expr.IsHardReg() || r_gpr_expr.RegNo() >= 32 || !p_fpr_expr.IsPseudoReg() ||
            r_gpr_expr.mode != p_fpr_expr.mode ||
            (r_gpr_expr.mode != RtlMachineMode::kSF && r_gpr_expr.mode != RtlMachineMode::kDF)) {
          continue;
        }
        const int r_gpr = r_gpr_expr.RegNo();
        const int p_fpr = p_fpr_expr.RegNo();
        const int fpr_hr = EffectiveHardReg(p_fpr, reg_renumber);
        if (fpr_hr < 32 || fpr_hr >= 64) {
          continue;
        }
        const bool has_dead_p = std::any_of(
            mv_insn.reg_notes.begin(), mv_insn.reg_notes.end(), [&](const RtlRegNote& n) {
              return n.kind == RtlMachineMode::kRegDead && n.datum.IsPseudoReg() &&
                     n.datum.RegNo() == p_fpr;
            });
        if (!has_dead_p) {
          continue;
        }
        const int gpr_nw = std::max(1, MipsHardRegNregs(r_gpr, r_gpr_expr.mode, gp64, fp64));
        const int fpr_nw = std::max(1, MipsHardRegNregs(fpr_hr, p_fpr_expr.mode, gp64, fp64));
        for (size_t m = i + 1; m < insns.size(); ++m) {
          RtlExpr& m_pat = insns[m].pattern;
          if (m_pat.op == RtlOp::kSet && m_pat.children.size() == 2 &&
              IsFprOnlyRhsExpr(m_pat.children[1]) && m_pat.children[1].ContainsReg(p_fpr)) {
            if (ReplaceReadRegInPattern(&m_pat, p_fpr, p_fpr_expr.mode,
                                        RtlExpr::MakeReg(r_gpr_expr.mode, r_gpr)) &&
                !m_pat.ContainsReg(p_fpr)) {
              RemoveDeadOrUnusedNotesForReg(&insns[m].reg_notes, p_fpr);
            }
          }
          bool stop = IsCallPattern(m_pat) || IsControlFlowBoundaryPattern(m_pat);
          for (int w = 0; w < fpr_nw && !stop; ++w) {
            if (InsnWritesOrClobbersRegOrEffHr(m_pat, reg_renumber, gp64, fp64, p_fpr,
                                               fpr_hr + w)) {
              stop = true;
            }
          }
          for (int w = 0; w < gpr_nw && !stop; ++w) {
            if (InsnWritesOrClobbersRegOrEffHr(m_pat, reg_renumber, gp64, fp64, r_gpr + w,
                                               r_gpr + w)) {
              stop = true;
            }
          }
          if (stop) {
            break;
          }
        }
      }
    }
  }
}

void ReverseRegisterConstraintReloadsAcrossCfg(
    const std::vector<RtlBasicBlock>& rpo_blocks, int opt_level, bool gp64, bool fp64,
    const absl::flat_hash_map<int, int>& reg_renumber,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
    absl::flat_hash_set<int>* reversed_reload_regs, absl::flat_hash_set<int>* proven_spill_regs,
    absl::flat_hash_set<int>* folded_scratch_pseudos, absl::flat_hash_set<int>* target_note_pseudos,
    absl::flat_hash_set<int>* combine_priority_dead_regs) {
  absl::flat_hash_map<int, PseudoUseSummary> summaries =
      BuildPseudoUseSummaries(*all_parsed_blocks);
  ReverseMdAndDiZeroConstraintReloads(
      rpo_blocks, opt_level, gp64, fp64, reg_renumber, all_parsed_blocks, &summaries,
      reversed_reload_regs, proven_spill_regs, folded_scratch_pseudos, target_note_pseudos);
  ReverseFprConstraintReloads(rpo_blocks, opt_level, gp64, fp64, reg_renumber, summaries,
                              all_parsed_blocks, reversed_reload_regs, proven_spill_regs,
                              folded_scratch_pseudos, target_note_pseudos,
                              combine_priority_dead_regs);
}

// On MIPS (`#ifndef SMALL_REGISTER_CLASSES`), `reload1.c:486, 709, 2221, 3660`
// places every hard register in `regs_explicitly_used` (including
// `CALL_INSN_FUNCTION_USAGE`) into `bad_spill_regs` / `forbidden_regs`,
// forbidding `reload()` from ever choosing it as a `spill_regs` reload scratch
// register (`new_spill_reg` aborts if `forbidden_regs[hr]` is set). Therefore,
// any hard register `hr` in `proven_spill_regs` (excluding `combine_reloads` /
// `find_dummy_reload` operand reuse) could NOT have been in
// `CALL_INSN_FUNCTION_USAGE` or explicitly referenced as a non-`scratch_list`
// hard register in pre-reload RTL.
void CleanupReversedReloadRegsExplicitUses(
    const MipsReloadFrameLayout& layout, const absl::flat_hash_set<int>& proven_spill_regs,
    RtlControlFlowGraph* out, std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks,
    absl::flat_hash_map<int, RtlMachineMode>* pseudo_widest_mode,
    absl::flat_hash_set<int>* target_note_pseudos) {
  if (proven_spill_regs.empty()) {
    return;
  }

  // Lift any surviving explicit hard-register def/use web for `hr` in
  // `proven_spill_regs` to an allocated pseudo `p` with `reg_renumber[p] = hr`.
  int max_reg = std::max(kFirstPseudoRegister - 1, out->sideband.max_regno - 1);
  for (const auto& [r, _] : out->sideband.reg_renumber) {
    max_reg = std::max(max_reg, r);
  }
  for (const auto& blk_insns : *all_parsed_blocks) {
    for (const RtlPatternAndNotes& insn : blk_insns) {
      insn.pattern.Visit([&](const RtlExpr& node) {
        if (node.IsPseudoReg()) {
          max_reg = std::max(max_reg, node.RegNo());
        }
      });
    }
  }

  auto non_clobber_mentions_hr = [](const RtlExpr& pat, int hr) -> bool {
    if (pat.op == RtlOp::kClobber) {
      return false;
    }
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& elem : pat.children) {
        if (elem.op != RtlOp::kClobber && elem.ContainsReg(hr)) {
          return true;
        }
      }
      return false;
    }
    return pat.ContainsReg(hr);
  };

  std::vector<int> sorted_proven_spill_regs(proven_spill_regs.begin(), proven_spill_regs.end());
  std::sort(sorted_proven_spill_regs.begin(), sorted_proven_spill_regs.end());
  for (int hr : sorted_proven_spill_regs) {
    if (hr < 2 || hr >= 64 || hr == 28 || hr == kStackPointerRegnum || hr == kFramePointerRegnum ||
        hr == kReturnAddressRegnum) {
      continue;
    }

    // Check whether `hr` is referenced outside `PARALLEL` `scratch_list`
    // `(clobber (reg hr))` elements, and reject if `hr` appears in a `USE` /
    // `CALL_INSN` or can be read along an unkilled path from entry block 0.
    bool blocked = false;
    bool has_ref = false;
    RtlMachineMode widest_mode = RtlMachineMode::kSI;
    const size_t nblocks = all_parsed_blocks->size();
    std::vector<bool> blk_reads_before_write(nblocks, false);
    std::vector<bool> blk_writes(nblocks, false);

    for (size_t b = 0; b < nblocks && !blocked; ++b) {
      bool written_in_blk = false;
      for (const RtlPatternAndNotes& insn : (*all_parsed_blocks)[b]) {
        if (!non_clobber_mentions_hr(insn.pattern, hr)) {
          if (insn.pattern.op == RtlOp::kClobber && insn.pattern.ContainsReg(hr)) {
            blocked = true;
            break;
          }
          continue;
        }
        if (insn.pattern.op == RtlOp::kUse || insn.pattern.op == RtlOp::kClobber ||
            IsCallPattern(insn.pattern)) {
          blocked = true;
          break;
        }
        if (insn.pattern.op == RtlOp::kParallel) {
          for (const RtlExpr& elem : insn.pattern.children) {
            if (elem.op == RtlOp::kUse && elem.ContainsReg(hr)) {
              blocked = true;
              break;
            }
          }
          if (blocked) {
            break;
          }
        }
        if (!written_in_blk) {
          RtlExpr probe = insn.pattern;
          if (ReplaceReadRegInPattern(&probe, hr, RtlMachineMode::kNone,
                                      RtlExpr::MakeReg(RtlMachineMode::kSI, 0))) {
            blk_reads_before_write[b] = true;
          }
          if (insn.pattern.op == RtlOp::kSet && !insn.pattern.children.empty() &&
              insn.pattern.children[0].IsHardReg() && insn.pattern.children[0].RegNo() == hr) {
            written_in_blk = true;
          } else if (insn.pattern.op == RtlOp::kParallel && !insn.pattern.children.empty() &&
                     insn.pattern.children[0].op == RtlOp::kSet &&
                     !insn.pattern.children[0].children.empty() &&
                     insn.pattern.children[0].children[0].IsHardReg() &&
                     insn.pattern.children[0].children[0].RegNo() == hr) {
            written_in_blk = true;
          } else {
            blocked = true;
            break;
          }
        }
        auto scan_mode = [&](const RtlExpr& sub) {
          sub.Visit([&](const RtlExpr& n) {
            if (n.IsHardReg() && n.RegNo() == hr) {
              if (!has_ref || MipsModeByteSize(n.mode) > MipsModeByteSize(widest_mode)) {
                widest_mode = n.mode;
              }
              has_ref = true;
            }
          });
        };
        if (insn.pattern.op == RtlOp::kParallel) {
          for (const RtlExpr& elem : insn.pattern.children) {
            if (elem.op != RtlOp::kClobber) {
              scan_mode(elem);
            }
          }
        } else {
          scan_mode(insn.pattern);
        }
      }
      blk_writes[b] = written_in_blk;
    }
    if (!blocked && has_ref && nblocks > 0) {
      std::vector<bool> entry_reaches(nblocks, false);
      entry_reaches[0] = true;
      bool changed = true;
      while (changed) {
        changed = false;
        for (size_t b = 0; b < nblocks; ++b) {
          if (!entry_reaches[b] || blk_writes[b]) {
            continue;
          }
          for (size_t succ : out->rpo_blocks[b].successors) {
            if (succ < nblocks && !entry_reaches[succ]) {
              entry_reaches[succ] = true;
              changed = true;
            }
          }
        }
      }
      for (size_t b = 0; b < nblocks; ++b) {
        if (entry_reaches[b] && blk_reads_before_write[b]) {
          blocked = true;
          break;
        }
      }
    }
    if (blocked || !has_ref) {
      continue;
    }

    int cross_block_pseudo = -1;
    bool used_across_blocks = false;
    int seen_blocks = 0;
    for (const auto& blk_insns : *all_parsed_blocks) {
      bool blk_has = false;
      for (const RtlPatternAndNotes& insn : blk_insns) {
        if (non_clobber_mentions_hr(insn.pattern, hr)) {
          blk_has = true;
          break;
        }
      }
      if (blk_has) {
        ++seen_blocks;
      }
    }
    if (seen_blocks > 1) {
      used_across_blocks = true;
      cross_block_pseudo = ++max_reg;
      out->sideband.reg_renumber[cross_block_pseudo] = hr;
      (*pseudo_widest_mode)[cross_block_pseudo] = widest_mode;
      target_note_pseudos->insert(cross_block_pseudo);
    }

    auto rewrite_expr_hr = [](auto& self, RtlExpr& e, int target_hr, int new_p) -> void {
      if (e.IsHardReg() && e.RegNo() == target_hr) {
        e = RtlExpr::MakeReg(e.mode, new_p, e.flags);
        return;
      }
      for (RtlExpr& ch : e.children) {
        self(self, ch, target_hr, new_p);
      }
    };

    for (auto& blk_insns : *all_parsed_blocks) {
      int active_p = cross_block_pseudo;
      for (RtlPatternAndNotes& insn : blk_insns) {
        if (!non_clobber_mentions_hr(insn.pattern, hr)) {
          continue;
        }
        if (!used_across_blocks && insn.pattern.op == RtlOp::kSet &&
            insn.pattern.children.size() == 2 && insn.pattern.children[0].IsHardReg() &&
            insn.pattern.children[0].RegNo() == hr) {
          if (active_p >= kFirstPseudoRegister) {
            rewrite_expr_hr(rewrite_expr_hr, insn.pattern.children[1], hr, active_p);
          }
          active_p = ++max_reg;
          out->sideband.reg_renumber[active_p] = hr;
          (*pseudo_widest_mode)[active_p] = insn.pattern.children[0].mode;
          target_note_pseudos->insert(active_p);
          insn.pattern.children[0] = RtlExpr::MakeReg(insn.pattern.children[0].mode, active_p,
                                                      insn.pattern.children[0].flags);
          RemoveDeadOrUnusedNotesForReg(&insn.reg_notes, hr);
          continue;
        }
        if (active_p < kFirstPseudoRegister) {
          active_p = ++max_reg;
          out->sideband.reg_renumber[active_p] = hr;
          (*pseudo_widest_mode)[active_p] = widest_mode;
          target_note_pseudos->insert(active_p);
        }
        if (insn.pattern.op == RtlOp::kParallel) {
          for (RtlExpr& elem : insn.pattern.children) {
            if (elem.op != RtlOp::kClobber) {
              rewrite_expr_hr(rewrite_expr_hr, elem, hr, active_p);
            }
          }
          for (RtlRegNote& note : insn.reg_notes) {
            if (note.kind != RtlMachineMode::kRegUnused || !insn.pattern.ContainsReg(hr)) {
              rewrite_expr_hr(rewrite_expr_hr, note.datum, hr, active_p);
            }
          }
        } else {
          insn.RewriteRegs([&](RtlExpr& reg) {
            if (reg.IsHardReg() && reg.RegNo() == hr) {
              reg = RtlExpr::MakeReg(reg.mode, active_p, reg.flags);
            }
          });
        }
      }
    }
  }
}

void RestorePreReloadMovstrsiFirstInsnScratches(
    const CompilationProfile& profile, bool gp64, bool fp64,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr, RtlControlFlowGraph* out) {
  if (profile.optimize_level <= 0) {
    return;
  }
  RtlControlFlowGraph live_cfg = *out;
  ReconstructRtlLiveness(&live_cfg, profile,
                         RtlLivenessOptions{.reload_completed = false, .update_reg_notes = false});

  static constexpr std::array<int, 25> kGprAllocOrder = {
      2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 16, 17, 18, 19, 20, 21, 22, 23, 30};
  auto extract_mem_base_hr = [&](const RtlExpr& mem) -> int {
    if (!mem.IsMem() || mem.children.empty()) {
      return -1;
    }
    const RtlExpr& addr = mem.children[0];
    const RtlExpr* base = &addr;
    if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[1].IsConstInt()) {
      base = &addr.children[0];
    }
    if (base->op != RtlOp::kReg || base->IsArgPointerReg()) {
      return -1;
    }
    return EffectiveHardReg(base->RegNo(), out->sideband.reg_renumber);
  };

  auto& all_parsed_blocks = *all_parsed_blocks_ptr;
  for (size_t b = 0; b < all_parsed_blocks.size() && b < out->rpo_blocks.size(); ++b) {
    size_t first_idx = all_parsed_blocks[b].size();
    for (size_t i = 0; i < all_parsed_blocks[b].size(); ++i) {
      if (all_parsed_blocks[b][i].pattern.op != RtlOp::kNote) {
        first_idx = i;
        break;
      }
    }
    if (first_idx >= all_parsed_blocks[b].size()) {
      continue;
    }
    RtlPatternAndNotes& pn = all_parsed_blocks[b][first_idx];
    if (pn.pattern.op != RtlOp::kParallel || pn.pattern.children.size() != 8 ||
        !pn.pattern.children[0].IsSet() || !pn.pattern.children[0].SetDest().IsMem() ||
        pn.pattern.children[0].SetDest().mode != RtlMachineMode::kBlk ||
        !pn.pattern.children[0].SetSrc().IsMem() ||
        pn.pattern.children[0].SetSrc().mode != RtlMachineMode::kBlk ||
        pn.pattern.children[7].op != RtlOp::kUse || pn.pattern.children[7].children.empty() ||
        !pn.pattern.children[7].children[0].IsConstInt() ||
        pn.pattern.children[7].children[0].int_val != 0) {
      continue;
    }
    bool all_hard_clobbers = true;
    for (size_t k = 1; k <= 4; ++k) {
      if (pn.pattern.children[k].op != RtlOp::kClobber || pn.pattern.children[k].children.empty() ||
          !pn.pattern.children[k].children[0].IsHardReg()) {
        all_hard_clobbers = false;
        break;
      }
    }
    if (!all_hard_clobbers) {
      continue;
    }
    const int dst_hr = extract_mem_base_hr(pn.pattern.children[0].SetDest());
    const int src_hr = extract_mem_base_hr(pn.pattern.children[0].SetSrc());
    if (src_hr < 2 || src_hr > 25) {
      continue;
    }
    const bool src_dies_here =
        std::any_of(pn.reg_notes.begin(), pn.reg_notes.end(), [&](const RtlRegNote& n) {
          return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                 !n.datum.IsArgPointerReg() &&
                 EffectiveHardReg(n.datum.RegNo(), out->sideband.reg_renumber) == src_hr;
        });
    if (!src_dies_here) {
      continue;
    }

    uint64_t live_after = 0;
    if (const auto lit = live_cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
        lit != live_cfg.sideband.basic_block_live_at_start.end()) {
      for (int r : lit->second) {
        if (r >= 0 && r < 64) {
          live_after |= (uint64_t{1} << r);
        }
      }
    }
    for (const RtlRegNote& n : pn.reg_notes) {
      if (n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg() && !n.datum.IsArgPointerReg()) {
        const int r = n.datum.RegNo();
        const int nw = std::max(1, MipsHardRegNregs(r, n.datum.mode, gp64, fp64));
        for (int w = 0; w < nw && r + w < 64; ++w) {
          if (r + w >= 0) {
            live_after &= ~(uint64_t{1} << (r + w));
          }
        }
      }
    }
    if (out->sideband.frame_pointer_needed) {
      live_after |= (uint64_t{1} << kFramePointerRegnum);
    }

    std::array<int, 4> la_scratches = {-1, -1, -1, -1};
    uint64_t used_mask = live_after;
    bool all_found = true;
    for (size_t s = 0; s < 4; ++s) {
      for (int cand : kGprAllocOrder) {
        if (cand == kFramePointerRegnum) {
          continue;
        }
        if ((used_mask & (uint64_t{1} << cand)) == 0) {
          la_scratches[s] = cand;
          used_mask |= (uint64_t{1} << cand);
          break;
        }
      }
      if (la_scratches[s] < 0) {
        all_found = false;
        break;
      }
    }
    if (!all_found) {
      continue;
    }

    bool non_colliding_slots_match = true;
    for (size_t s = 0; s < 4; ++s) {
      const int cur_hr = pn.pattern.children[1 + s].children[0].RegNo();
      const int pre_hr = la_scratches[s];
      if (pre_hr != src_hr && pre_hr != dst_hr && cur_hr != pre_hr) {
        non_colliding_slots_match = false;
        break;
      }
    }
    if (!non_colliding_slots_match) {
      continue;
    }

    bool changed = false;
    for (size_t s = 0; s < 4; ++s) {
      const int cur_hr = pn.pattern.children[1 + s].children[0].RegNo();
      const int pre_hr = la_scratches[s];
      if (cur_hr != pre_hr && (pre_hr == src_hr || pre_hr == dst_hr)) {
        pn.pattern.children[1 + s].children[0].SetRegNo(pre_hr);
        for (RtlRegNote& n : pn.reg_notes) {
          if (n.kind == RtlMachineMode::kRegUnused && n.datum.IsHardReg() &&
              n.datum.RegNo() == cur_hr) {
            n.datum.SetRegNo(pre_hr);
            break;
          }
        }
        changed = true;
      }
    }
    if (changed) {
      out->rpo_blocks[b].instructions[first_idx] = FormatGccPrintRtlPatternAndNotes(
          pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{});
      out->rpo_blocks[b].SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < out->rpo_blocks[b].rtl_insns.size() && i < all_parsed_blocks[b].size();
           ++i) {
        if (all_parsed_blocks[b][i].has_explicit_call_fusage &&
            !out->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage) {
          out->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage = true;
          out->rpo_blocks[b].rtl_insns[i].call_fusage = all_parsed_blocks[b][i].call_fusage;
        }
      }
    }
  }
}

void FinalizePreReloadLivenessAndSideband(
    const CompilationProfile& profile, int input_max_regno, bool restored_any_fp,
    const absl::flat_hash_set<int>& target_note_pseudos,
    const absl::flat_hash_set<int>& combine_priority_dead_regs,
    const std::vector<int>& phantom_pseudos,
    std::vector<std::vector<RtlPatternAndNotes>>* all_parsed_blocks_ptr, RtlControlFlowGraph* out) {
  std::vector<std::vector<RtlPatternAndNotes>>& all_parsed_blocks = *all_parsed_blocks_ptr;
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*out, &profile, &gp64, &fp64);
  RestorePreReloadMovstrsiFirstInsnScratches(profile, gp64, fp64, &all_parsed_blocks, out);

  auto compute_insn_tied_hard_dead_mask = [&](const RtlExpr& pat) -> uint64_t {
    uint64_t alloc_pseudo_set_mask = 0;
    uint64_t direct_hard_set_mask = 0;
    auto collect_elem = [&](const RtlExpr& elem) {
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
      if (dst->IsPseudoReg()) {
        const auto rit = out->sideband.reg_renumber.find(dst->RegNo());
        if (rit != out->sideband.reg_renumber.end() && rit->second >= 0 && rit->second < 64) {
          const int hr = rit->second + woff;
          const int nw = std::max(1, MipsHardRegNregs(hr, dm, gp64, fp64));
          for (int w = 0; w < nw && hr + w < 64; ++w) {
            if (hr + w >= 0) {
              alloc_pseudo_set_mask |= (uint64_t{1} << (hr + w));
            }
          }
        }
      } else if (dst->IsHardReg() && !dst->IsArgPointerReg()) {
        const int hr = dst->RegNo() + woff;
        const int nw = std::max(1, MipsHardRegNregs(hr, dm, gp64, fp64));
        for (int w = 0; w < nw && hr + w < 64; ++w) {
          if (hr + w >= 0) {
            direct_hard_set_mask |= (uint64_t{1} << (hr + w));
          }
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        collect_elem(ch);
      }
    } else {
      collect_elem(pat);
    }
    return alloc_pseudo_set_mask & ~direct_hard_set_mask;
  };

  bool has_tied_hard_dead_cand = false;
  if (profile.optimize_level > 0) {
    for (const auto& blk : all_parsed_blocks) {
      for (const RtlPatternAndNotes& pn : blk) {
        if (!IsCallPattern(pn.pattern) && compute_insn_tied_hard_dead_mask(pn.pattern) != 0) {
          has_tied_hard_dead_cand = true;
          break;
        }
      }
      if (has_tied_hard_dead_cand) {
        break;
      }
    }
  }
  const bool run_note_updates = !target_note_pseudos.empty() || has_tied_hard_dead_cand;

  RtlControlFlowGraph liveness_cfg = *out;
  ReconstructRtlLiveness(&liveness_cfg, profile,
                         RtlLivenessOptions{
                             .reload_completed = false,
                             .update_reg_notes = run_note_updates,
                         });

  auto same_reg_note = [](const RtlRegNote& a, const RtlRegNote& b) {
    return a.kind == b.kind && a.datum.op == RtlOp::kReg && b.datum.op == RtlOp::kReg &&
           !a.datum.IsArgPointerReg() && !b.datum.IsArgPointerReg() &&
           a.datum.RegNo() == b.datum.RegNo();
  };
  auto format_cfg_insn = [](const RtlPatternAndNotes& pn) {
    const bool is_top_call = pn.pattern.op != RtlOp::kSequence && IsCallPattern(pn.pattern);
    return FormatGccPrintRtlPatternAndNotes(
        pn.pattern, is_top_call ? absl::Span<const RtlRegNote>{} : pn.reg_notes, pn.bracketed_notes,
        /*call_fusage=*/{});
  };

  if (run_note_updates) {
    for (size_t b = 0; b < out->rpo_blocks.size(); ++b) {
      RtlBasicBlock& blk = out->rpo_blocks[b];
      const RtlBasicBlock& live_blk = liveness_cfg.rpo_blocks[b];
      bool block_notes_changed = false;
      for (size_t i = 0; i < all_parsed_blocks[b].size() && i < live_blk.instructions.size(); ++i) {
        absl::StatusOr<RtlPatternAndNotes> live_pn = ParsePatternAndNotes(live_blk.instructions[i]);
        if (!live_pn.ok()) {
          continue;
        }
        RtlPatternAndNotes& dst_pn = all_parsed_blocks[b][i];
        if (IsCallPattern(dst_pn.pattern)) {
          continue;
        }
        if (dst_pn.pattern.op == RtlOp::kUse && dst_pn.pattern.children.size() == 1 &&
            dst_pn.pattern.children[0].IsPseudoReg()) {
          const int up = dst_pn.pattern.children[0].RegNo();
          const auto sets_it = liveness_cfg.sideband.reg_n_sets.find(up);
          if (sets_it == liveness_cfg.sideband.reg_n_sets.end() || sets_it->second == 0) {
            continue;
          }
        }
        const bool is_unallocated_pseudo_def =
            dst_pn.pattern.op == RtlOp::kSet && dst_pn.pattern.children.size() == 2 &&
            dst_pn.pattern.children[0].IsPseudoReg() &&
            !out->sideband.reg_renumber.contains(dst_pn.pattern.children[0].RegNo());
        const uint64_t tied_hard_dead_mask =
            (profile.optimize_level > 0) ? compute_insn_tied_hard_dead_mask(dst_pn.pattern) : 0;
        auto is_erasable_managed_note = [&](const RtlRegNote& note) {
          if (note.kind != RtlMachineMode::kRegDead && note.kind != RtlMachineMode::kRegUnused) {
            return false;
          }
          if (note.datum.IsPseudoReg()) {
            return target_note_pseudos.contains(note.datum.RegNo());
          }
          if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg() &&
              !note.datum.IsArgPointerReg()) {
            if (is_unallocated_pseudo_def && target_note_pseudos.contains(note.datum.RegNo())) {
              return true;
            }
            const int hr = note.datum.RegNo();
            if (hr >= 0 && hr < 64 && tied_hard_dead_mask != 0) {
              const int nw = std::max(1, MipsHardRegNregs(hr, note.datum.mode, gp64, fp64));
              for (int w = 0; w < nw && hr + w < 64; ++w) {
                if ((tied_hard_dead_mask & (uint64_t{1} << (hr + w))) != 0) {
                  return true;
                }
              }
            }
          }
          return false;
        };

        std::vector<size_t> wanted_live_indices;
        for (size_t ni = 0; ni < live_pn->reg_notes.size(); ++ni) {
          const RtlRegNote& note = live_pn->reg_notes[ni];
          if (is_erasable_managed_note(note)) {
            wanted_live_indices.push_back(ni);
          }
        }
        const size_t before_sz = dst_pn.reg_notes.size();
        absl::flat_hash_set<size_t> matched_wanted;
        std::vector<RtlRegNote> kept_notes;
        kept_notes.reserve(dst_pn.reg_notes.size());
        dst_pn.reg_notes.erase(
            std::remove_if(dst_pn.reg_notes.begin(), dst_pn.reg_notes.end(),
                           [&](const RtlRegNote& existing) {
                             const bool is_dup = (existing.kind == RtlMachineMode::kRegDead ||
                                                  existing.kind == RtlMachineMode::kRegUnused) &&
                                                 std::any_of(kept_notes.begin(), kept_notes.end(),
                                                             [&](const RtlRegNote& k) {
                                                               return same_reg_note(k, existing);
                                                             });
                             if (is_dup && is_erasable_managed_note(existing)) {
                               return true;
                             }
                             for (size_t wi : wanted_live_indices) {
                               if (!matched_wanted.contains(wi) &&
                                   same_reg_note(existing, live_pn->reg_notes[wi])) {
                                 matched_wanted.insert(wi);
                                 kept_notes.push_back(existing);
                                 return false;
                               }
                             }
                             if (is_erasable_managed_note(existing)) {
                               return true;
                             }
                             kept_notes.push_back(existing);
                             return false;
                           }),
            dst_pn.reg_notes.end());
        if (dst_pn.reg_notes.size() != before_sz) {
          block_notes_changed = true;
        }
        for (size_t wi : wanted_live_indices) {
          if (matched_wanted.contains(wi)) {
            continue;
          }
          const RtlRegNote& wanted = live_pn->reg_notes[wi];
          auto insert_pos = dst_pn.reg_notes.end();
          for (size_t succ_i = wi + 1; succ_i < live_pn->reg_notes.size(); ++succ_i) {
            const RtlRegNote& succ = live_pn->reg_notes[succ_i];
            if (succ.kind == RtlMachineMode::kRegEquiv) {
              continue;
            }
            if (succ.kind == RtlMachineMode::kRegDead && succ.datum.op == RtlOp::kReg &&
                !succ.datum.IsArgPointerReg() &&
                combine_priority_dead_regs.contains(succ.datum.RegNo())) {
              continue;
            }
            auto found_it = std::find_if(
                dst_pn.reg_notes.begin(), dst_pn.reg_notes.end(),
                [&](const RtlRegNote& existing) { return same_reg_note(existing, succ); });
            if (found_it != dst_pn.reg_notes.end()) {
              insert_pos = found_it;
              break;
            }
          }
          dst_pn.reg_notes.insert(insert_pos, wanted);
          block_notes_changed = true;
        }
        if (block_notes_changed) {
          blk.instructions[i] = format_cfg_insn(dst_pn);
        }
      }
      if (block_notes_changed) {
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
  }

  out->sideband.basic_block_live_at_start =
      std::move(liveness_cfg.sideband.basic_block_live_at_start);
  out->sideband.reg_n_refs = std::move(liveness_cfg.sideband.reg_n_refs);
  out->sideband.reg_n_sets = std::move(liveness_cfg.sideband.reg_n_sets);
  out->sideband.reg_n_deaths = std::move(liveness_cfg.sideband.reg_n_deaths);
  out->sideband.reg_live_length = std::move(liveness_cfg.sideband.reg_live_length);
  out->sideband.reg_n_calls_crossed = std::move(liveness_cfg.sideband.reg_n_calls_crossed);
  out->sideband.max_regno = std::max(input_max_regno, liveness_cfg.sideband.max_regno);
  std::vector<int> unalloc_reg_entries;
  for (const auto& [reg_p, hr] : out->sideband.reg_renumber) {
    out->sideband.max_regno = std::max(out->sideband.max_regno, reg_p + 1);
    if (hr < 0) {
      unalloc_reg_entries.push_back(reg_p);
    }
  }
  for (int reg_p : unalloc_reg_entries) {
    out->sideband.reg_renumber.erase(reg_p);
  }
  for (int phantom_p : phantom_pseudos) {
    out->sideband.reg_renumber.erase(phantom_p);
    out->sideband.reg_n_sets.erase(phantom_p);
    out->sideband.reg_n_calls_crossed.erase(phantom_p);
    out->sideband.reg_n_refs[phantom_p] = 2;
    out->sideband.reg_n_deaths[phantom_p] = 1;
    out->sideband.reg_live_length[phantom_p] = 2;
    out->sideband.max_regno = std::max(out->sideband.max_regno, phantom_p + 1);
  }
  {
    bool any_pseudo_live_len = false;
    for (const auto& [r, len] : out->sideband.reg_live_length) {
      if (r >= kFirstPseudoRegister && len != 0) {
        any_pseudo_live_len = true;
        break;
      }
    }
    if (!any_pseudo_live_len) {
      out->sideband.reg_live_length.try_emplace(kFirstPseudoRegister, -1);
      out->sideband.basic_block_live_at_start.try_emplace(0);
    }
  }

  if (restored_any_fp && !out->sideband.basic_block_live_at_start.empty()) {
    out->sideband.basic_block_live_at_start[0].insert(kFramePointerRegnum);
  }
}

void RestoreO0ParallelScratchClobbers(const MipsReloadFrameLayout& layout,
                                      std::vector<std::vector<RtlPatternAndNotes>>* all_blocks,
                                      RtlControlFlowGraph* out,
                                      absl::flat_hash_set<int>* reversed_reload_regs,
                                      absl::flat_hash_set<int>* folded_scratch_pseudos) {
  const auto post_cleanup_summaries = BuildPseudoUseSummaries(*all_blocks);
  bool non_clobber_md_used[3] = {false, false, false};
  auto mark_non_clobber_md = [&](const auto& self, const RtlExpr& expr) -> void {
    if (expr.op == RtlOp::kClobber) {
      return;
    }
    if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg()) {
      const int hr = EffectiveHardReg(expr.RegNo(), out->sideband.reg_renumber);
      if (hr >= 0) {
        const int nw = std::max(1, MipsHardRegNregs(hr, expr.mode, layout.gp64, layout.fp64));
        for (int h = std::max(64, hr); h <= std::min(66, hr + nw - 1); ++h) {
          non_clobber_md_used[h - 64] = true;
        }
      }
      return;
    }
    for (const RtlExpr& ch : expr.children) {
      self(self, ch);
    }
  };
  for (const auto& blk_insns : *all_blocks) {
    for (const RtlPatternAndNotes& insn : blk_insns) {
      mark_non_clobber_md(mark_non_clobber_md, insn.pattern);
    }
  }
  auto contains_asm = [](const RtlExpr& root) {
    bool found = false;
    root.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kAsmOperands || n.op == RtlOp::kAsmInput) {
        found = true;
      }
    });
    return found;
  };
  for (auto& blk_insns : *all_blocks) {
    for (RtlPatternAndNotes& insn : blk_insns) {
      RtlExpr& pat = insn.pattern;
      if (pat.op != RtlOp::kParallel || pat.children.size() < 2 || !pat.children[0].IsSet() ||
          contains_asm(pat)) {
        continue;
      }
      const RtlExpr& dst = pat.children[0].SetDest();
      const RtlExpr& src = pat.children[0].SetSrc();
      const bool is_blk_move = pat.children.size() >= 5 && dst.IsMem() &&
                               dst.mode == RtlMachineMode::kBlk && src.IsMem() &&
                               src.mode == RtlMachineMode::kBlk;
      const bool is_fix_or_ffs = (src.op == RtlOp::kFix || src.op == RtlOp::kFfs);
      for (size_t k = 1; k < pat.children.size(); ++k) {
        RtlExpr& elem = pat.children[k];
        if (elem.op != RtlOp::kClobber || elem.children.size() != 1) {
          continue;
        }
        if (is_blk_move && elem.children[0].IsHardReg() && elem.children[0].RegNo() < 32) {
          reversed_reload_regs->insert(elem.children[0].RegNo());
          elem = RtlExpr::MakeClobber(RtlExpr::MakeScratch(elem.children[0].mode));
          continue;
        }
        if (elem.children[0].op != RtlOp::kReg || elem.children[0].IsArgPointerReg()) {
          continue;
        }
        const int c_reg = elem.children[0].RegNo();
        const int c_hr = EffectiveHardReg(c_reg, out->sideband.reg_renumber);
        if (is_fix_or_ffs) {
          if (c_reg >= kFirstPseudoRegister) {
            auto sum_it = post_cleanup_summaries.find(c_reg);
            if (sum_it == post_cleanup_summaries.end() || sum_it->second.use_count != 0) {
              continue;
            }
            folded_scratch_pseudos->insert(c_reg);
            out->sideband.reg_renumber.erase(c_reg);
          }
          if (c_hr >= 0 && c_hr < kFirstPseudoRegister) {
            reversed_reload_regs->insert(c_hr);
          }
          RemoveDeadOrUnusedNotesForReg(&insn.reg_notes, c_reg);
          elem = RtlExpr::MakeClobber(RtlExpr::MakeScratch(elem.children[0].mode));
          continue;
        }
        if (c_hr >= 64 && c_hr <= 66) {
          const int c_nw =
              std::max(1, MipsHardRegNregs(c_hr, elem.children[0].mode, layout.gp64, layout.fp64));
          bool md_busy = false;
          for (int h = c_hr; h < c_hr + c_nw && h <= 66; ++h) {
            if (non_clobber_md_used[h - 64]) {
              md_busy = true;
              break;
            }
          }
          if (md_busy) {
            continue;
          }
          if (c_reg >= kFirstPseudoRegister) {
            auto sum_it = post_cleanup_summaries.find(c_reg);
            if (sum_it == post_cleanup_summaries.end() || sum_it->second.use_count != 0) {
              continue;
            }
            folded_scratch_pseudos->insert(c_reg);
            out->sideband.reg_renumber.erase(c_reg);
          }
          RemoveDeadOrUnusedNotesForReg(&insn.reg_notes, c_reg);
          elem = RtlExpr::MakeClobber(RtlExpr::MakeScratch(elem.children[0].mode));
        }
      }
    }
  }
}

void RestoreCrossClassCallArgEquivReads(bool expensive_opts,
                                        const absl::flat_hash_map<int, int>& reg_renumber,
                                        bool gp64, bool fp64,
                                        std::vector<std::vector<RtlPatternAndNotes>>* all_blocks) {
  if (!expensive_opts) {
    return;
  }
  auto is_fp_arith_op = [](RtlOp op) {
    return op == RtlOp::kPlus || op == RtlOp::kMinus || op == RtlOp::kMult || op == RtlOp::kDiv ||
           op == RtlOp::kNeg || op == RtlOp::kAbs;
  };
  for (auto& blk : *all_blocks) {
    for (size_t i = 0; i + 1 < blk.size(); ++i) {
      const RtlExpr& ipat = blk[i].pattern;
      if (!ipat.IsSet() || !ipat.SetDest().IsHardReg() || ipat.SetDest().IsArgPointerReg() ||
          !ipat.SetSrc().IsPseudoReg()) {
        continue;
      }
      const int d_hr = ipat.SetDest().RegNo();
      const int s_reg = ipat.SetSrc().RegNo();
      const RtlMachineMode mode = ipat.SetDest().mode;
      if (d_hr < 4 || d_hr > 7 || ipat.SetSrc().mode != mode ||
          (mode != RtlMachineMode::kSF && mode != RtlMachineMode::kDF)) {
        continue;
      }
      const int s_hr = EffectiveHardReg(s_reg, reg_renumber);
      if (s_hr < 32 || s_hr > 63) {
        continue;
      }
      const bool already_dead =
          std::any_of(blk[i].reg_notes.begin(), blk[i].reg_notes.end(), [&](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                   n.datum.RegNo() == s_reg;
          });
      if (already_dead) {
        continue;
      }
      for (size_t p = i + 1; p < blk.size(); ++p) {
        RtlExpr& ppat = blk[p].pattern;
        if (ppat.op == RtlOp::kCodeLabel || ppat.op == RtlOp::kNote || ppat.op == RtlOp::kBarrier) {
          if (ppat.op == RtlOp::kCodeLabel) {
            break;
          }
          continue;
        }
        if (IsCallPattern(ppat) || ppat.op == RtlOp::kReturn ||
            (ppat.IsSet() && ppat.SetDest().op == RtlOp::kPc) || ppat.op == RtlOp::kUse ||
            InsnWritesOrClobbersRegOrEffHr(ppat, reg_renumber, gp64, fp64, s_reg, -1) ||
            InsnWritesOrClobbersRegOrEffHr(ppat, reg_renumber, gp64, fp64, d_hr, d_hr)) {
          break;
        }
        auto dead_it = std::find_if(
            blk[p].reg_notes.begin(), blk[p].reg_notes.end(), [&](const RtlRegNote& n) {
              return n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                     n.datum.RegNo() == s_reg && n.datum.mode == mode;
            });
        if (dead_it != blk[p].reg_notes.end()) {
          if (ppat.IsSet() && is_fp_arith_op(ppat.SetSrc().op) && p == i + 1) {
            int replaced = 0;
            ppat.children[1].Mutate([&](RtlExpr& n) {
              if (n.IsPseudoReg() && n.RegNo() == s_reg && n.mode == mode) {
                n = ipat.SetDest();
                ++replaced;
              }
            });
            if (replaced > 0) {
              RtlRegNote moved = std::move(*dead_it);
              blk[p].reg_notes.erase(dead_it);
              blk[i].reg_notes.push_back(std::move(moved));
            }
          }
          break;
        }
      }
    }
  }
}

void FoldO0Mulsidi3TopStackSpills(const MipsReloadFrameLayout& /*layout*/, int* max_existing_regno,
                                  std::vector<std::vector<RtlPatternAndNotes>>* all_blocks,
                                  RtlControlFlowGraph* out,
                                  absl::flat_hash_set<int>* reversed_reload_regs,
                                  absl::flat_hash_set<int>* folded_scratch_pseudos) {
  auto record_folded_reg = [&](int r) {
    const int hr = EffectiveHardReg(r, out->sideband.reg_renumber);
    if (hr >= 0 && hr < kFirstPseudoRegister) {
      reversed_reload_regs->insert(hr);
      if (hr < 64) {
        reversed_reload_regs->insert(hr + 1);
      }
    }
    if (r >= kFirstPseudoRegister) {
      folded_scratch_pseudos->insert(r);
    }
  };

  auto match_top_fp_di_mem = [&](const RtlExpr& expr) -> bool {
    if (out->sideband.frame_size_bytes < 8 || expr.op != RtlOp::kMem || expr.flags.volatil ||
        expr.mode != RtlMachineMode::kDi || expr.children.size() != 1) {
      return false;
    }
    const int64_t expected_off = out->sideband.frame_size_bytes - 8;
    const RtlExpr& addr = expr.children[0];
    if (expected_off == 0) {
      return addr.op == RtlOp::kReg && addr.RegNo() == kFramePointerRegnum;
    }
    return addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
           addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == kFramePointerRegnum &&
           addr.children[1].op == RtlOp::kConstInt && addr.children[1].int_val == expected_off;
  };

  for (auto& blk : *all_blocks) {
    for (size_t i = 0; i + 2 < blk.size(); ++i) {
      const RtlExpr& mpat = blk[i].pattern;
      if (mpat.op != RtlOp::kParallel || mpat.children.empty() || !mpat.children[0].IsSet() ||
          mpat.children[0].SetDest().mode != RtlMachineMode::kDi ||
          !mpat.children[0].SetDest().IsReg() || mpat.children[0].SetSrc().op != RtlOp::kMult) {
        continue;
      }
      const int mul_r = mpat.children[0].SetDest().RegNo();
      int sec_r = -1;
      int spill_p = -1;
      bool from_fp_mem = false;
      size_t store_end = 0;
      const RtlExpr& s1 = blk[i + 1].pattern;
      if (s1.IsSet() && s1.SetSrc().IsReg() && s1.SetSrc().RegNo() == mul_r) {
        if (s1.SetDest().IsPseudoReg() && s1.SetDest().mode == RtlMachineMode::kDi) {
          const int cand_p = s1.SetDest().RegNo();
          auto ren_it = out->sideband.reg_renumber.find(cand_p);
          if (ren_it != out->sideband.reg_renumber.end() && ren_it->second < 0) {
            spill_p = cand_p;
            store_end = i + 1;
          }
        } else if (match_top_fp_di_mem(s1.SetDest())) {
          from_fp_mem = true;
          store_end = i + 1;
        }
      }
      if (spill_p < 0 && !from_fp_mem && i + 3 < blk.size() &&
          EffectiveHardReg(mul_r, out->sideband.reg_renumber) == 64 && s1.IsSet() &&
          s1.SetDest().IsReg() && s1.SetDest().mode == RtlMachineMode::kDi && s1.SetSrc().IsReg() &&
          s1.SetSrc().RegNo() == mul_r) {
        const int cand_sec = s1.SetDest().RegNo();
        const RtlExpr& s2 = blk[i + 2].pattern;
        if (s2.IsSet() && s2.SetSrc().IsReg() && s2.SetSrc().RegNo() == cand_sec) {
          if (s2.SetDest().IsPseudoReg() && s2.SetDest().mode == RtlMachineMode::kDi) {
            const int cand_p = s2.SetDest().RegNo();
            auto ren_it = out->sideband.reg_renumber.find(cand_p);
            if (ren_it != out->sideband.reg_renumber.end() && ren_it->second < 0) {
              sec_r = cand_sec;
              spill_p = cand_p;
              store_end = i + 2;
            }
          } else if (match_top_fp_di_mem(s2.SetDest())) {
            sec_r = cand_sec;
            from_fp_mem = true;
            store_end = i + 2;
          }
        }
      }
      if (spill_p < 0 && !from_fp_mem) {
        continue;
      }
      size_t load_idx = blk.size();
      for (size_t k = store_end + 1; k + 1 < blk.size(); ++k) {
        if (blk[k].pattern.IsSet() && blk[k].pattern.SetDest().IsReg() &&
            !blk[k].pattern.SetDest().IsArgPointerReg() &&
            blk[k].pattern.SetDest().mode == RtlMachineMode::kDi) {
          if (!from_fp_mem && blk[k].pattern.SetSrc().IsPseudoReg() &&
              blk[k].pattern.SetSrc().RegNo() == spill_p) {
            load_idx = k;
            break;
          }
          if (from_fp_mem && match_top_fp_di_mem(blk[k].pattern.SetSrc())) {
            load_idx = k;
            break;
          }
        }
      }
      if (load_idx >= blk.size()) {
        continue;
      }
      if (from_fp_mem) {
        const int64_t top_off = out->sideband.frame_size_bytes - 8;
        bool other_fp_ref = false;
        for (size_t b2 = 0; b2 < all_blocks->size() && !other_fp_ref; ++b2) {
          for (size_t j2 = 0; j2 < (*all_blocks)[b2].size(); ++j2) {
            if (&(*all_blocks)[b2] == &blk && (j2 == store_end || j2 == load_idx)) {
              continue;
            }
            (*all_blocks)[b2][j2].pattern.Visit([&](const RtlExpr& n) {
              if (n.op == RtlOp::kMem && n.children.size() == 1) {
                const RtlExpr& a = n.children[0];
                if ((top_off == 0 && a.op == RtlOp::kReg && a.RegNo() == kFramePointerRegnum) ||
                    (a.op == RtlOp::kPlus && a.children.size() == 2 &&
                     a.children[0].op == RtlOp::kReg &&
                     a.children[0].RegNo() == kFramePointerRegnum &&
                     a.children[1].op == RtlOp::kConstInt && a.children[1].int_val >= top_off)) {
                  other_fp_ref = true;
                }
              }
            });
          }
        }
        if (other_fp_ref) {
          continue;
        }
      }
      const int load_r = blk[load_idx].pattern.SetDest().RegNo();
      RtlExpr& upat = blk[load_idx + 1].pattern;
      RtlExpr* u_set =
          upat.IsSet() ? &upat
          : (upat.op == RtlOp::kParallel && !upat.children.empty() && upat.children[0].IsSet())
              ? &upat.children[0]
              : nullptr;
      if (u_set == nullptr || !u_set->SetSrc().ContainsReg(load_r)) {
        continue;
      }
      bool md_busy_between = false;
      for (size_t k = store_end + 1; k < load_idx && !md_busy_between; ++k) {
        if (IsCallPattern(blk[k].pattern)) {
          md_busy_between = true;
          break;
        }
        blk[k].pattern.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kReg && !n.IsArgPointerReg()) {
            const int nhr = EffectiveHardReg(n.RegNo(), out->sideband.reg_renumber);
            if (nhr == 64 || nhr == 65) {
              md_busy_between = true;
            }
          }
        });
      }
      if (from_fp_mem) {
        spill_p = ++(*max_existing_regno);
        out->sideband.frame_size_bytes -= 8;
      }
      out->sideband.reg_renumber[spill_p] = md_busy_between ? -1 : 64;
      blk[i].pattern.children[0].children[0] = RtlExpr::MakeReg(RtlMachineMode::kDi, spill_p);
      u_set->children[1].Mutate([&](RtlExpr& n) {
        if (n.IsReg() && !n.IsArgPointerReg() && n.RegNo() == load_r &&
            n.mode == RtlMachineMode::kDi) {
          n = RtlExpr::MakeReg(RtlMachineMode::kDi, spill_p);
        }
      });
      record_folded_reg(mul_r);
      if (sec_r >= 0) {
        record_folded_reg(sec_r);
      }
      if (!(u_set->SetDest().IsPseudoReg() && u_set->SetDest().RegNo() == load_r)) {
        record_folded_reg(load_r);
      } else {
        const int hr = EffectiveHardReg(load_r, out->sideband.reg_renumber);
        if (hr >= 0 && hr < kFirstPseudoRegister) {
          reversed_reload_regs->insert(hr);
          if (hr < 64) {
            reversed_reload_regs->insert(hr + 1);
          }
        }
      }
      blk.erase(blk.begin() + static_cast<ptrdiff_t>(load_idx));
      blk.erase(blk.begin() + static_cast<ptrdiff_t>(i + 1),
                blk.begin() + static_cast<ptrdiff_t>(store_end + 1));
    }
  }
}

absl::Status ValidateRtlExprRegisters(const RtlExpr& expr) {
  if (expr.op == RtlOp::kReg) {
    if (expr.int_val < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: invalid negative register number %d", expr.int_val));
    }
  }
  for (const RtlExpr& child : expr.children) {
    if (absl::Status st = ValidateRtlExprRegisters(child); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertReload(const RtlControlFlowGraph& reload_cfg) {
  CompilationProfile default_profile;
  default_profile.optimize_level = 2;
  default_profile.c_flags = {"-O2"};
  return InvertReload(reload_cfg, default_profile);
}

absl::StatusOr<RtlControlFlowGraph> InvertReload(const RtlControlFlowGraph& reload_cfg,
                                                 const CompilationProfile& profile) {
  if (reload_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertReload: input CFG has no basic blocks");
  }

  const size_t num_blocks = reload_cfg.rpo_blocks.size();
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = reload_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: block %zu has out-of-bounds successor %zu (num_blocks=%zu)", b, succ,
            num_blocks));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: block %zu has out-of-bounds addr_vec target %zu (num_blocks=%zu)", b,
            target, num_blocks));
      }
    }
  }

  for (const auto& [regno, hard_reg] : reload_cfg.sideband.reg_renumber) {
    if (regno < kFirstPseudoRegister || hard_reg < -1 || hard_reg >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertReload: invalid sideband reg_renumber entry (%d -> %d)", regno, hard_reg));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_refs) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_refs", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_sets) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_sets", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_deaths) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_n_deaths", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_live_length) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertReload: negative regno %d in sideband reg_live_length", regno));
    }
  }
  for (const auto& [regno, _] : reload_cfg.sideband.reg_n_calls_crossed) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertReload: negative regno %d in sideband reg_n_calls_crossed", regno));
    }
  }
  if (reload_cfg.sideband.frame_size_bytes < 0) {
    return absl::InvalidArgumentError(absl::StrFormat("InvertReload: negative frame_size_bytes %d",
                                                      reload_cfg.sideband.frame_size_bytes));
  }

  RtlControlFlowGraph out = reload_cfg;
  out.pass = RtlPassKind::kLreg;

  std::vector<std::vector<RtlPatternAndNotes>> all_parsed_blocks(out.rpo_blocks.size());
  absl::flat_hash_set<int> referenced_pseudos;
  absl::flat_hash_map<int, RtlMachineMode> pseudo_widest_mode;
  int max_existing_regno = std::max(kFirstPseudoRegister - 1, out.sideband.max_regno - 1);
  for (const auto& [r, _] : out.sideband.reg_renumber) {
    max_existing_regno = std::max(max_existing_regno, r);
  }

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    const RtlBasicBlock& blk = out.rpo_blocks[b];
    std::vector<RtlPatternAndNotes>& parsed_insns = all_parsed_blocks[b];
    parsed_insns.reserve(blk.instructions.size());

    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("InvertReload: failed to parse instruction %zu in block %zu: %s", i, b,
                            parsed.status().message()));
      }
      if (parsed->pattern.op == RtlOp::kNil || parsed->pattern.op == RtlOp::kUnknown) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertReload: invalid pattern opcode at instruction %zu in block %zu", i, b));
      }
      if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
          !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = blk.rtl_insns[i].call_fusage;
      }
      if (absl::Status st = ValidateRtlExprRegisters(parsed->pattern); !st.ok()) {
        return st;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (absl::Status st = ValidateRtlExprRegisters(note.datum); !st.ok()) {
          return st;
        }
      }
      for (const RtlExpr& fu : parsed->call_fusage) {
        if (absl::Status st = ValidateRtlExprRegisters(fu); !st.ok()) {
          return st;
        }
      }

      auto record_reg = [&](const RtlExpr& node) {
        if (node.IsPseudoReg()) {
          const int p = node.RegNo();
          referenced_pseudos.insert(p);
          max_existing_regno = std::max(max_existing_regno, p);
          auto it = pseudo_widest_mode.find(p);
          if (it == pseudo_widest_mode.end() ||
              MipsModeByteSize(node.mode) > MipsModeByteSize(it->second)) {
            pseudo_widest_mode[p] = node.mode;
          }
        }
      };
      parsed->pattern.Visit(record_reg);
      for (const RtlRegNote& note : parsed->reg_notes) {
        note.datum.Visit(record_reg);
      }
      parsed_insns.push_back(*std::move(parsed));
    }
  }

  const ParsedCompilerFlags parsed_flags = ParseCompilationProfileFlags(profile);
  MipsReloadFrameLayout layout = ComputeMipsReloadFrameLayout(
      reload_cfg, profile, parsed_flags, all_parsed_blocks, pseudo_widest_mode);

  absl::flat_hash_set<int> reversed_reload_regs;
  absl::flat_hash_set<int> proven_spill_regs;
  absl::flat_hash_set<int> folded_scratch_pseudos;
  absl::flat_hash_set<int> target_note_pseudos;

  // Step 1: Reverse large-frame (`|offset| >= 32768`) stack-offset address and
  // `addsi3` constant reloads so all stack offsets are directly visible as
  // `(plus:SI (reg:SI to_reg) (const_int off))`.
  {
    const absl::flat_hash_map<int, PseudoUseSummary> pre_summaries =
        BuildPseudoUseSummaries(all_parsed_blocks);
    for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
      ReverseLargeStackOffsetReloadsInBlock(b, layout.elim_to_reg, layout.gp64, layout.fp64,
                                            out.sideband.reg_renumber, pre_summaries,
                                            &all_parsed_blocks[b], &reversed_reload_regs,
                                            &proven_spill_regs, &folded_scratch_pseudos);
    }
  }

  // Record whether any `MEM` in the CFG accesses incoming stack arguments
  // (`offset >= post_reload_tsize`) after folding large-offset reloads.
  for (const auto& blk_insns : all_parsed_blocks) {
    for (const RtlPatternAndNotes& insn : blk_insns) {
      insn.pattern.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kMem && node.children.size() == 1) {
          std::optional<int64_t> off = ExtractElimToRegOffset(node.children[0], layout.elim_to_reg);
          if (off.has_value() && *off >= layout.post_reload_tsize) {
            layout.has_incoming_arg_mem_access = true;
          }
        }
      });
    }
  }

  // Step 2: Reverse `caller-save` save/restore pairs and unify call-split
  // `InvertAlterReg` pseudos across calls, using pure RTL cross-call liveness
  // of existing allocated pseudos.
  absl::flat_hash_set<int> rtl_cross_call_hrs;
  if (!referenced_pseudos.empty() && parsed_flags.opt_level > 0 && parsed_flags.flag_caller_saves) {
    RtlControlFlowGraph pre_cs_probe = out;
    ReconstructRtlLiveness(&pre_cs_probe, profile,
                           RtlLivenessOptions{
                               .reload_completed = true,
                               .update_reg_notes = false,
                           });
    for (const auto& [regno, hard_reg] : out.sideband.reg_renumber) {
      if (regno >= kFirstPseudoRegister && hard_reg >= 0 && hard_reg < kFirstPseudoRegister) {
        auto calls_it = pre_cs_probe.sideband.reg_n_calls_crossed.find(regno);
        if (calls_it != pre_cs_probe.sideband.reg_n_calls_crossed.end() && calls_it->second > 0) {
          auto mode_it = pseudo_widest_mode.find(regno);
          const RtlMachineMode mode =
              (mode_it != pseudo_widest_mode.end()) ? mode_it->second : RtlMachineMode::kSI;
          const int nregs = std::max(1, MipsHardRegNregs(hard_reg, mode, layout.gp64, layout.fp64));
          for (int k = 0; k < nregs; ++k) {
            if (IsMipsCallerSavableHardReg(hard_reg + k)) {
              rtl_cross_call_hrs.insert(hard_reg + k);
            }
          }
        }
      }
    }
  }

  const CallerSaveReversalOutcome cs_outcome = ReverseCallerSaveAcrossCfg(
      parsed_flags, layout, out.sideband.frame_pointer_needed, out.sideband.frame_size_bytes,
      rtl_cross_call_hrs, &max_existing_regno, &pseudo_widest_mode, &all_parsed_blocks,
      &out.sideband.reg_renumber);
  target_note_pseudos.insert(cs_outcome.unified_pseudos.begin(), cs_outcome.unified_pseudos.end());
  for (int u_p : cs_outcome.unified_pseudos) {
    max_existing_regno = std::max(max_existing_regno, u_p);
  }

  // Step 3: Reverse `global=1` `alter_reg` spilled-pseudo stack slots and
  // fold input/output/inherited reloads.
  const SpillReversalOutcome spill_outcome = ReconstructSpilledPseudosAcrossCfg(
      parsed_flags, layout, out.sideband.frame_pointer_needed, cs_outcome.pre_caller_save_top,
      out.sideband.reg_renumber, max_existing_regno, &all_parsed_blocks, &reversed_reload_regs,
      &proven_spill_regs, &folded_scratch_pseudos, &target_note_pseudos);
  target_note_pseudos.insert(spill_outcome.reconstructed_pseudos.begin(),
                             spill_outcome.reconstructed_pseudos.end());

  if (cs_outcome.stripped_any || !spill_outcome.reconstructed_pseudos.empty() ||
      !spill_outcome.phantom_pseudos.empty()) {
    out.sideband.frame_size_bytes = static_cast<int>(
        std::max<int64_t>(0, spill_outcome.pre_reload_frame_top - layout.starting_frame_offset));
  }

  // Step 3b: Reverse register-class constraint reloads (`=x` `HI:LO` output
  // reloads on `mulsidi3`/`umulsidi3` and `"f"` FPR reloads for GPR/spilled
  // `SFmode`/`DFmode` operands).
  absl::flat_hash_set<int> combine_priority_dead_regs;
  ReverseRegisterConstraintReloadsAcrossCfg(
      out.rpo_blocks, parsed_flags.opt_level, layout.gp64, layout.fp64, out.sideband.reg_renumber,
      &all_parsed_blocks, &reversed_reload_regs, &proven_spill_regs, &folded_scratch_pseudos,
      &target_note_pseudos, &combine_priority_dead_regs);

  // Step 4: Reverse `PARALLEL` earlyclobber `CLOBBER` output reloads.
  {
    const absl::flat_hash_map<int, PseudoUseSummary> par_summaries =
        BuildPseudoUseSummaries(all_parsed_blocks);
    absl::flat_hash_map<int, int> par_unify_map;
    for (size_t b = 0; b < all_parsed_blocks.size(); ++b) {
      ReverseParallelClobberOutputReloads(
          all_parsed_blocks, out.rpo_blocks, b, &all_parsed_blocks[b], parsed_flags.opt_level,
          layout.gp64, layout.fp64, out.sideband.reg_renumber, par_summaries, &reversed_reload_regs,
          &proven_spill_regs, &folded_scratch_pseudos, &par_unify_map, &target_note_pseudos);
    }
    if (!par_unify_map.empty()) {
      auto resolve_par = [&](int p) {
        int cur = p;
        while (par_unify_map.contains(cur) && par_unify_map[cur] != cur) {
          cur = par_unify_map[cur];
        }
        return cur;
      };
      for (auto& blk_insns : all_parsed_blocks) {
        for (RtlPatternAndNotes& insn : blk_insns) {
          insn.RewriteRegs([&](RtlExpr& reg) {
            if (reg.IsPseudoReg() && par_unify_map.contains(reg.RegNo())) {
              reg.SetRegNo(resolve_par(reg.RegNo()));
            }
          });
        }
      }
      for (const auto& [dead_p, _] : par_unify_map) {
        out.sideband.reg_renumber.erase(dead_p);
      }
    }
  }

  CleanupReversedReloadRegsExplicitUses(layout, proven_spill_regs, &out, &all_parsed_blocks,
                                        &pseudo_widest_mode, &target_note_pseudos);

  // At `-O0` (`local-alloc` skipped, `toplev.c:3293`), `movstrsi_internal*`,
  // `fix_trunc*`, `ffs*`, and `mul*`/`div*`/`mod*` `PARALLEL` patterns in
  // `mips.md` enter `reload` with `(clobber (scratch:...))` operands that
  // `subst_reloads` (`reload1.c:6257`) replaces in-place with allocated hard
  // registers. Restore `(clobber (scratch:...))` at `-O0`.
  if (parsed_flags.opt_level == 0) {
    RestoreO0ParallelScratchClobbers(layout, &all_parsed_blocks, &out, &reversed_reload_regs,
                                     &folded_scratch_pseudos);
  }

  // Step 5: Reverse 3-region stack address elimination (`$sp` outgoing args,
  // `$fp` local frame, and `(reg:SI 0 argp)` incoming/homed caller args).
  bool restored_any_fp = false;
  absl::flat_hash_set<int> remaining_hard_regs;
  absl::flat_hash_set<int> remaining_pseudos;

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    std::vector<RtlPatternAndNotes>& parsed_insns = all_parsed_blocks[b];

    for (RtlPatternAndNotes& insn : parsed_insns) {
      if (!ShouldSkipStackAddressUneliminationForInsn(insn.pattern,
                                                      out.sideband.frame_pointer_needed)) {
        if (ReverseEliminateRegsInExpr(&insn.pattern, /*inside_mem=*/false,
                                       out.sideband.frame_pointer_needed, out.sideband.is_variadic,
                                       layout, &restored_any_fp)) {
          // In `reload1.c:reload_as_needed` (lines 3836-3847), `(use (mem ...))`
          // and `(clobber (mem ...))` have `XEXP(MEM, 0)` eliminated in-place
          // before `eliminate_regs_in_insn(insn, 1)` is called, so `val == 0`
          // and `REG_NOTES(insn)` are not rewritten by `eliminate_regs_in_insn`.
          if (insn.pattern.op != RtlOp::kUse && insn.pattern.op != RtlOp::kClobber) {
            for (RtlRegNote& note : insn.reg_notes) {
              ReverseEliminateRegsInExpr(&note.datum, /*inside_mem=*/false,
                                         out.sideband.frame_pointer_needed,
                                         out.sideband.is_variadic, layout, &restored_any_fp);
            }
          }
        }
      }
    }
  }

  if (parsed_flags.opt_level == 0) {
    FoldO0Mulsidi3TopStackSpills(layout, &max_existing_regno, &all_parsed_blocks, &out,
                                 &reversed_reload_regs, &folded_scratch_pseudos);
    if (!layout.gp64) {
      const absl::flat_hash_map<int, PseudoUseSummary> di_zero_summaries =
          BuildPseudoUseSummaries(all_parsed_blocks);
      absl::flat_hash_set<int> use_insn_pseudos;
      for (const auto& blk : all_parsed_blocks) {
        for (const RtlPatternAndNotes& pn : blk) {
          if (pn.pattern.op == RtlOp::kUse && !pn.pattern.children.empty() &&
              pn.pattern.children[0].IsPseudoReg()) {
            use_insn_pseudos.insert(pn.pattern.children[0].RegNo());
          }
        }
      }
      for (auto& blk : all_parsed_blocks) {
        for (size_t i = 0; i + 1 < blk.size(); ++i) {
          if (!blk[i].pattern.IsSet() || !blk[i].pattern.SetDest().IsReg() ||
              blk[i].pattern.SetDest().IsArgPointerReg() ||
              blk[i].pattern.SetDest().flags.volatil || blk[i].pattern.SetDest().flags.integrated ||
              blk[i].pattern.SetDest().mode != RtlMachineMode::kDi ||
              !blk[i].pattern.SetSrc().IsConstInt() || blk[i].pattern.SetSrc().int_val != 0 ||
              !blk[i + 1].pattern.IsSet() || !blk[i + 1].pattern.SetDest().IsMem() ||
              blk[i + 1].pattern.SetDest().mode != RtlMachineMode::kDi ||
              !blk[i + 1].pattern.SetSrc().IsReg() ||
              blk[i + 1].pattern.SetSrc().mode != RtlMachineMode::kDi ||
              blk[i + 1].pattern.SetSrc().RegNo() != blk[i].pattern.SetDest().RegNo() ||
              blk[i + 1].pattern.SetDest().ContainsReg(blk[i].pattern.SetDest().RegNo())) {
            continue;
          }
          const int zr = blk[i].pattern.SetDest().RegNo();
          const int zhr = EffectiveHardReg(zr, out.sideband.reg_renumber);
          bool single_pair = false;
          if (zr >= kFirstPseudoRegister) {
            auto sit = di_zero_summaries.find(zr);
            single_pair = sit != di_zero_summaries.end() && sit->second.def_count == 1 &&
                          sit->second.use_count == 1 && !sit->second.multi_block &&
                          !use_insn_pseudos.contains(zr);
          } else if (zr >= 2 && zr <= 24 && (zr % 2) == 0 && blk[i].reg_notes.empty()) {
            bool has_dead_note = false;
            for (const RtlRegNote& n : blk[i + 1].reg_notes) {
              if (n.kind == RtlMachineMode::kRegDead && n.datum.IsHardReg() &&
                  (n.datum.RegNo() == zr || n.datum.RegNo() == zr + 1)) {
                has_dead_note = true;
                break;
              }
            }
            bool pair_referenced_elsewhere = has_dead_note;
            auto expr_mentions_pair = [&](const RtlExpr& e) {
              bool hit = false;
              e.Visit([&](const RtlExpr& n) {
                if (hit || !n.IsHardReg() || n.IsArgPointerReg()) {
                  return;
                }
                const int r = n.RegNo();
                const int nw = std::max(1, MipsHardRegNregs(r, n.mode, layout.gp64, layout.fp64));
                if (r < zr + 2 && zr < r + nw) {
                  hit = true;
                }
              });
              return hit;
            };
            for (size_t k = i + 2; !pair_referenced_elsewhere && k < blk.size(); ++k) {
              if (expr_mentions_pair(blk[k].pattern)) {
                pair_referenced_elsewhere = true;
              }
            }
            for (const auto& other_blk : all_parsed_blocks) {
              if (&other_blk == &blk || pair_referenced_elsewhere) {
                continue;
              }
              for (const RtlPatternAndNotes& opn : other_blk) {
                if (expr_mentions_pair(opn.pattern)) {
                  pair_referenced_elsewhere = true;
                  break;
                }
              }
            }
            single_pair = !pair_referenced_elsewhere;
          }
          if (single_pair && zhr >= 2 && zhr <= 24) {
            blk[i + 1].pattern.children[1] = RtlExpr::MakeConstInt(0);
            RemoveDeadOrUnusedNotesForReg(&blk[i + 1].reg_notes, zr);
            reversed_reload_regs.insert(zhr);
            reversed_reload_regs.insert(zhr + 1);
            if (zr >= kFirstPseudoRegister) {
              folded_scratch_pseudos.insert(zr);
            }
            blk.erase(blk.begin() + static_cast<ptrdiff_t>(i));
            if (i > 0) {
              --i;
            }
          }
        }
      }
    }
  }

  RestoreCrossClassCallArgEquivReads(parsed_flags.flag_expensive_optimizations,
                                     out.sideband.reg_renumber, layout.gp64, layout.fp64,
                                     &all_parsed_blocks);

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    const std::vector<RtlPatternAndNotes>& parsed_insns = all_parsed_blocks[b];

    blk.instructions.clear();
    blk.instructions.reserve(parsed_insns.size());
    for (const RtlPatternAndNotes& insn : parsed_insns) {
      auto collect_remaining = [&](const RtlExpr& node) {
        if (node.IsHardReg()) {
          const int hr = node.RegNo();
          const int nw = std::max(1, MipsHardRegNregs(hr, node.mode, layout.gp64, layout.fp64));
          for (int k = 0; k < nw && hr + k < kFirstPseudoRegister; ++k) {
            remaining_hard_regs.insert(hr + k);
          }
        } else if (node.IsPseudoReg()) {
          remaining_pseudos.insert(node.RegNo());
        }
      };
      insn.pattern.Visit(collect_remaining);
      for (const RtlRegNote& note : insn.reg_notes) {
        note.datum.Visit(collect_remaining);
      }
      const bool is_top_call = insn.pattern.op != RtlOp::kSequence && IsCallPattern(insn.pattern);
      blk.instructions.push_back(FormatGccPrintRtlPatternAndNotes(
          insn.pattern, is_top_call ? absl::Span<const RtlRegNote>{} : insn.reg_notes,
          insn.bracketed_notes, /*call_fusage=*/{}));
    }
    blk.SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < blk.rtl_insns.size() && i < parsed_insns.size(); ++i) {
      if (parsed_insns[i].has_explicit_call_fusage && !blk.rtl_insns[i].has_explicit_call_fusage) {
        blk.rtl_insns[i].has_explicit_call_fusage = true;
        blk.rtl_insns[i].call_fusage = parsed_insns[i].call_fusage;
      }
    }
  }

  // Prune any folded reload scratch pseudos that have zero remaining references.
  for (int scratch_p : folded_scratch_pseudos) {
    if (!remaining_pseudos.contains(scratch_p)) {
      out.sideband.reg_renumber.erase(scratch_p);
    }
  }

  // Prune callee-saved GPRs (`16..23`, and `30` when `!frame_pointer_needed`)
  // and FPRs (`52..63`) that were introduced solely as reload scratch registers
  // and are now completely unreferenced in both instructions and `reg_renumber`.
  if (!reversed_reload_regs.empty()) {
    for (const auto& [p, hard_reg] : out.sideband.reg_renumber) {
      if (hard_reg >= 0 && hard_reg < kFirstPseudoRegister) {
        auto mode_it = pseudo_widest_mode.find(p);
        const RtlMachineMode mode =
            (mode_it != pseudo_widest_mode.end()) ? mode_it->second : RtlMachineMode::kSI;
        const int nw = std::max(1, MipsHardRegNregs(hard_reg, mode, layout.gp64, layout.fp64));
        for (int k = 0; k < nw && hard_reg + k < kFirstPseudoRegister; ++k) {
          remaining_hard_regs.insert(hard_reg + k);
        }
      }
    }
    auto& saved_gprs = out.sideband.saved_gpr_regnos;
    saved_gprs.erase(std::remove_if(saved_gprs.begin(), saved_gprs.end(),
                                    [&](int gpr) {
                                      if (gpr == kFramePointerRegnum &&
                                          (out.sideband.frame_pointer_needed || restored_any_fp)) {
                                        return false;
                                      }
                                      return reversed_reload_regs.contains(gpr) &&
                                             !remaining_hard_regs.contains(gpr);
                                    }),
                     saved_gprs.end());
    auto& saved_fprs = out.sideband.saved_fpr_regnos;
    saved_fprs.erase(std::remove_if(saved_fprs.begin(), saved_fprs.end(),
                                    [&](int fpr) {
                                      return fpr >= 52 && fpr < 64 &&
                                             reversed_reload_regs.contains(fpr) &&
                                             !remaining_hard_regs.contains(fpr);
                                    }),
                     saved_fprs.end());
  }

  // Step 6: Recompute pre-reload RTL liveness, refresh managed `REG_NOTES`,
  // and finalize sideband statistics.
  const int min_final_max_regno =
      (parsed_flags.opt_level == 0) ? kFirstPseudoRegister : reload_cfg.sideband.max_regno;
  FinalizePreReloadLivenessAndSideband(profile, min_final_max_regno, restored_any_fp,
                                       target_note_pseudos, combine_priority_dead_regs,
                                       spill_outcome.phantom_pseudos, &all_parsed_blocks, &out);

  return out;
}

}  // namespace rom_nom_nom::lifter
