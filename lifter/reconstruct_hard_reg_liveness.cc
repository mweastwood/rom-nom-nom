#include "lifter/reconstruct_hard_reg_liveness.h"

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
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

using RegMask = unsigned __int128;

constexpr RegMask kRetCandMask =
    (RegMask{1} << 2) | (RegMask{1} << 3) | (RegMask{1} << 32) | (RegMask{1} << 33);

constexpr RegMask BuildCallClobberedMask() {
  RegMask m = 0;
  for (int r = 1; r <= 15; ++r) m |= (RegMask{1} << r);
  for (int r = 24; r <= 27; ++r) m |= (RegMask{1} << r);
  m |= (RegMask{1} << 31);
  for (int r = 32; r <= 51; ++r) m |= (RegMask{1} << r);
  for (int r = 64; r <= 67; ++r) m |= (RegMask{1} << r);
  return m;
}

constexpr RegMask kCallClobberedMask = BuildCallClobberedMask();

constexpr RegMask BuildArgRegMask() {
  RegMask m = 0;
  for (int r = 4; r <= 7; ++r) m |= (RegMask{1} << r);
  for (int r = 44; r <= 47; ++r) m |= (RegMask{1} << r);
  return m;
}

constexpr RegMask kArgRegMask = BuildArgRegMask();

int ModeByteSize(RtlMachineMode mode) {
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

int HardRegNregs(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  const bool is_fp = (regno >= 32 && regno < 64);
  const int unit_bytes = is_fp ? (fp64 ? 8 : 4) : (gp64 ? 8 : 4);
  return std::max(1, (ModeByteSize(mode) + unit_bytes - 1) / unit_bytes);
}

RegMask RegBitMask(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
    return 0;
  }
  const int nregs = HardRegNregs(regno, mode, gp64, fp64);
  RegMask mask = 0;
  for (int r = regno; r < regno + nregs && r < kMipsFirstPseudoRegister; ++r) {
    mask |= (RegMask{1} << r);
  }
  return mask;
}

void DetectRegisterWidthFlags(const RtlControlFlowGraph& cfg, const CompilationProfile* profile,
                              bool* gp64, bool* fp64) {
  *gp64 = false;
  *fp64 = false;
  if (profile != nullptr && !profile->c_flags.empty()) {
    int cc1_isa = 0;
    bool has_msingle_float = false;
    bool has_m4650 = false;
    for (const std::string& flag : profile->c_flags) {
      if (flag == "-mips1") {
        cc1_isa = 1;
      } else if (flag == "-mips2") {
        cc1_isa = 2;
      } else if (flag == "-mips3") {
        cc1_isa = 3;
      } else if (flag == "-mips4") {
        cc1_isa = 4;
      } else if (flag == "-msingle-float") {
        has_msingle_float = true;
      } else if (flag == "-mdouble-float") {
        has_msingle_float = false;
      } else if (flag == "-m4650") {
        has_m4650 = true;
      } else if (flag == "-mno-4650") {
        has_m4650 = false;
      }
    }
    if (cc1_isa == 3 || cc1_isa == 4) {
      *gp64 = true;
      if (!has_msingle_float && !has_m4650) {
        *fp64 = true;
      }
    }
    for (const std::string& flag : profile->c_flags) {
      if (flag == "-mgp64") {
        *gp64 = true;
      } else if (flag == "-mgp32") {
        *gp64 = false;
      } else if (flag == "-mfp64") {
        *fp64 = true;
      } else if (flag == "-mfp32") {
        *fp64 = false;
      }
    }
    return;
  }
  auto check_di_alu_set = [&](const RtlExpr& set_expr, bool has_si_clobber) {
    if (has_si_clobber || !set_expr.IsSet() || !set_expr.SetDest().IsReg() ||
        set_expr.SetDest().RegNo() >= 32 || set_expr.SetSrc().mode != RtlMachineMode::kDi) {
      return;
    }
    const RtlOp op = set_expr.SetSrc().op;
    if (op == RtlOp::kPlus || op == RtlOp::kMinus || op == RtlOp::kAshift ||
        op == RtlOp::kAshiftrt || op == RtlOp::kLshiftrt || op == RtlOp::kSignExtend ||
        op == RtlOp::kZeroExtend) {
      *gp64 = true;
    }
  };
  auto check_top_expr = [&](const RtlExpr& top) {
    if (top.IsParallel()) {
      bool has_si_clobber = false;
      for (const RtlExpr& c : top.children) {
        if (c.op == RtlOp::kClobber && !c.children.empty() &&
            c.children[0].mode == RtlMachineMode::kSi) {
          has_si_clobber = true;
          break;
        }
      }
      for (const RtlExpr& c : top.children) {
        check_di_alu_set(c, has_si_clobber);
      }
    } else {
      check_di_alu_set(top, /*has_si_clobber=*/false);
    }
  };
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& s : blk.instructions) {
      auto parsed = ParsePatternAndNotes(s);
      if (!parsed.ok()) {
        continue;
      }
      if (parsed->pattern.op == RtlOp::kSequence) {
        for (const RtlExpr& sub : parsed->pattern.children) {
          auto sub_pn = RtlPatternAndNotes::FromSequenceSubInsn(sub);
          if (sub_pn.ok()) {
            check_top_expr(sub_pn->pattern);
          }
        }
      } else {
        check_top_expr(parsed->pattern);
      }
      parsed->pattern.Visit([&](const RtlExpr& node) {
        if (node.IsReg() && node.RegNo() >= 32 && node.RegNo() < 64 && (node.RegNo() % 2) == 1) {
          *fp64 = true;
        }
        if (node.IsReg() && node.RegNo() >= 1 && node.RegNo() < 32 && (node.RegNo() % 2) == 1 &&
            node.mode == RtlMachineMode::kDi) {
          *gp64 = true;
        }
      });
    }
  }
}

bool IsReturnJumpPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.IsParallel() && !pat.children.empty() && pat.children[0].op == RtlOp::kReturn) {
    return true;
  }
  if (pat.op == RtlOp::kSequence && !pat.children.empty() &&
      pat.children[0].op == RtlOp::kJumpInsn && !pat.children[0].children.empty()) {
    return IsReturnJumpPattern(pat.children[0].children[0]);
  }
  return false;
}

bool IsConditionalJumpPattern(const RtlExpr& pat) {
  if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
    return pat.SetSrc().op == RtlOp::kIfThenElse;
  }
  if (pat.IsParallel() && !pat.children.empty() && pat.children[0].IsSet() &&
      pat.children[0].SetDest().op == RtlOp::kPc) {
    return pat.children[0].SetSrc().op == RtlOp::kIfThenElse;
  }
  if (pat.op == RtlOp::kSequence && !pat.children.empty() &&
      pat.children[0].op == RtlOp::kJumpInsn && !pat.children[0].children.empty()) {
    return IsConditionalJumpPattern(pat.children[0].children[0]);
  }
  return false;
}

bool IsAnyControlTransferPattern(const RtlExpr& pat) {
  if (IsReturnJumpPattern(pat)) {
    return true;
  }
  if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
    return true;
  }
  if (pat.IsParallel() && !pat.children.empty() && pat.children[0].IsSet() &&
      pat.children[0].SetDest().op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kSequence && !pat.children.empty() &&
      pat.children[0].op == RtlOp::kJumpInsn && !pat.children[0].children.empty()) {
    return IsAnyControlTransferPattern(pat.children[0].children[0]);
  }
  return false;
}

bool HasExistingReturnUse(const RtlControlFlowGraph& cfg) {
  for (const auto& blk : cfg.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok() || parsed->pattern.op != RtlOp::kUse) {
        continue;
      }
      bool has_ret_reg = false;
      parsed->pattern.Visit([&has_ret_reg](const RtlExpr& node) {
        if (node.IsReg()) {
          const int r = node.RegNo();
          if (r == 2 || r == 3 || r == 32 || r == 33) {
            has_ret_reg = true;
          }
        }
      });
      if (has_ret_reg) {
        return true;
      }
    }
  }
  return false;
}

struct ElemSummary {
  RegMask uses = 0;
  RegMask arg_fusage_uses = 0;
  RegMask defs = 0;
  RegMask explicit_sets = 0;
  RegMask integrated_ret_regs = 0;
  bool is_call = false;
  bool is_note_or_pseudo = false;
  std::array<RtlMachineMode, kMipsFirstPseudoRegister> use_modes{};
  std::array<std::string, kMipsFirstPseudoRegister> use_raw_modes{};
  std::array<RtlMachineMode, kMipsFirstPseudoRegister> def_modes{};
  std::array<std::string, kMipsFirstPseudoRegister> def_raw_modes{};
};

void CollectReadRegs(const RtlExpr& expr, bool gp64, bool fp64, ElemSummary* out) {
  if (expr.IsReg()) {
    const int regno = expr.RegNo();
    if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
      const RegMask m = RegBitMask(regno, expr.mode, gp64, fp64);
      const bool had_reg = (out->uses & (RegMask{1} << regno)) != 0;
      const int new_nregs = HardRegNregs(regno, expr.mode, gp64, fp64);
      const int old_nregs = had_reg ? HardRegNregs(regno, out->use_modes[regno], gp64, fp64) : 0;
      out->uses |= m;
      if (!had_reg || new_nregs >= old_nregs) {
        out->use_modes[regno] = expr.mode;
        out->use_raw_modes[regno] = expr.raw_mode;
      }
      for (int k = 1; k < new_nregs && regno + k < kMipsFirstPseudoRegister; ++k) {
        if ((out->uses & (RegMask{1} << (regno + k))) == 0 ||
            out->use_modes[regno + k] == RtlMachineMode::kSi) {
          out->use_modes[regno + k] = (regno + k < 32)
                                          ? (gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi)
                                          : (fp64 ? RtlMachineMode::kDf : RtlMachineMode::kSf);
          out->use_raw_modes[regno + k].clear();
        }
      }
      if (expr.flags.integrated) {
        out->integrated_ret_regs |= (m & kRetCandMask);
      }
    }
    return;
  }
  if (expr.IsSubreg() && !expr.children.empty() && expr.children[0].IsReg()) {
    // Both `flow.c:2387-2405` (`mark_used_regs`, `case SUBREG:` -> `x = SUBREG_REG (x)`)
    // and `sched.c:2937-2942` (`attach_deaths`, `case SUBREG:` -> `attach_deaths (XEXP (x, 0),
    // insn, set_p)`) treat a read of `(subreg:M1 (reg:M2 base) word)` identically to a read
    // of the inner `(reg:M2 base)`.
    CollectReadRegs(expr.children[0], gp64, fp64, out);
    return;
  }
  for (const RtlExpr& child : expr.children) {
    CollectReadRegs(child, gp64, fp64, out);
  }
}

void AnalyzePatternAst(const RtlExpr& pat, bool gp64, bool fp64, ElemSummary* out) {
  if (pat.op == RtlOp::kAddrVec || pat.op == RtlOp::kAddrDiffVec || pat.op == RtlOp::kNote) {
    return;
  }
  if (pat.IsParallel()) {
    for (const RtlExpr& child : pat.children) {
      AnalyzePatternAst(child, gp64, fp64, out);
    }
    return;
  }
  if (pat.IsSet()) {
    const RtlExpr& dst = pat.SetDest();
    const RtlExpr& src = pat.SetSrc();
    CollectReadRegs(src, gp64, fp64, out);
    if (dst.IsReg()) {
      const int regno = dst.RegNo();
      if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
        const RegMask m = RegBitMask(regno, dst.mode, gp64, fp64);
        out->defs |= m;
        out->explicit_sets |= m;
        out->def_modes[regno] = dst.mode;
        out->def_raw_modes[regno] = dst.raw_mode;
        if (dst.flags.integrated) {
          out->integrated_ret_regs |= (m & kRetCandMask);
        }
      }
    } else if (dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg()) {
      const int base_regno = dst.children[0].RegNo();
      if (base_regno >= 0 && base_regno < kMipsFirstPseudoRegister) {
        const int regno = base_regno + static_cast<int>(dst.int_val);
        if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
          const RegMask m = RegBitMask(regno, dst.mode, gp64, fp64);
          out->defs |= m;
          out->explicit_sets |= m;
          out->def_modes[regno] = dst.mode;
          out->def_raw_modes[regno] = dst.raw_mode;
          if (dst.children[0].flags.integrated) {
            out->integrated_ret_regs |= (m & kRetCandMask);
          }
        }
      }
    } else {
      CollectReadRegs(dst, gp64, fp64, out);
    }
    return;
  }
  if (pat.op == RtlOp::kClobber) {
    if (!pat.children.empty()) {
      const RtlExpr* dst = &pat.children[0];
      RtlMachineMode mode = dst->mode;
      std::string raw_mode = dst->raw_mode;
      int word_off = 0;
      if (dst->IsSubreg() && !dst->children.empty()) {
        word_off = static_cast<int>(dst->int_val);
        dst = &dst->children[0];
      }
      if (dst->IsReg()) {
        const int base_regno = dst->RegNo();
        if (base_regno >= 0 && base_regno < kMipsFirstPseudoRegister) {
          const int regno = base_regno + word_off;
          if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
            out->defs |= RegBitMask(regno, mode, gp64, fp64);
            out->def_modes[regno] = mode;
            out->def_raw_modes[regno] = raw_mode;
          }
        }
      }
    }
    return;
  }
  if (pat.op == RtlOp::kUse || pat.op == RtlOp::kCall || pat.op == RtlOp::kReturn) {
    CollectReadRegs(pat, gp64, fp64, out);
  }
}

bool IsUnoptimizedProfile(const CompilationProfile* profile) {
  if (profile == nullptr) {
    return false;
  }
  if (!profile->c_flags.empty()) {
    return ParsedCompilerFlags::Parse(profile->c_flags).opt_level == 0;
  }
  return profile->optimize_level == 0;
}

absl::flat_hash_set<std::string> CollectJumpTableLabels(const RtlControlFlowGraph& cfg) {
  absl::flat_hash_set<std::string> labels;
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& insn_str : blk.instructions) {
      auto pn = ParsePatternAndNotes(insn_str);
      if (!pn.ok()) {
        continue;
      }
      pn->pattern.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kUse && !node.children.empty() &&
            node.children[0].op == RtlOp::kLabelRef && !node.children[0].str_val.empty()) {
          labels.insert(node.children[0].str_val);
        }
      });
    }
  }
  return labels;
}

bool PatternSrcContainsAnyLabelRef(const RtlExpr& pat,
                                   const absl::flat_hash_set<std::string>& labels) {
  if (labels.empty() || !pat.IsSet()) {
    return false;
  }
  bool found = false;
  pat.SetSrc().Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kLabelRef && labels.contains(node.str_val)) {
      found = true;
    }
  });
  return found;
}

ElemSummary SummarizeSingleElem(const RtlPatternAndNotes& pn, bool force_call, bool gp64, bool fp64,
                                bool is_unoptimized,
                                const absl::flat_hash_set<std::string>& jt_table_labels,
                                RegMask* pending_arg_defs) {
  ElemSummary s;
  s.use_modes.fill(RtlMachineMode::kSi);
  s.def_modes.fill(RtlMachineMode::kSi);
  const RtlExpr& pat = pn.pattern;
  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber) {
    s.is_note_or_pseudo = true;
  }
  s.is_call = force_call || IsCallPattern(pat);
  bool is_no_conflict_libcall_clobber = false;
  if (pat.op == RtlOp::kClobber) {
    for (const RtlRegNote& note : pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegLibcall) {
        is_no_conflict_libcall_clobber = true;
        break;
      }
    }
  }
  if (!is_no_conflict_libcall_clobber) {
    AnalyzePatternAst(pat, gp64, fp64, &s);
  }

  if (s.is_call) {
    if (pn.has_explicit_call_fusage) {
      for (const RtlExpr& item : pn.call_fusage) {
        if (item.op == RtlOp::kUse && !item.children.empty() && item.children[0].IsReg()) {
          const int r = item.children[0].RegNo();
          const RegMask m = RegBitMask(r, item.children[0].mode, gp64, fp64) & kArgRegMask;
          s.arg_fusage_uses |= m;
        }
      }
      s.uses |= s.arg_fusage_uses;
    } else {
      s.arg_fusage_uses |= *pending_arg_defs;
      for (const RtlRegNote& note : pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
          const int r = note.datum.RegNo();
          const RegMask m = RegBitMask(r, note.datum.mode, gp64, fp64) & kArgRegMask;
          s.arg_fusage_uses |= m;
        }
      }
      s.uses |= s.arg_fusage_uses;
    }
    s.defs |= kCallClobberedMask;
    *pending_arg_defs = 0;
  } else if (pat.op != RtlOp::kNote) {
    if (is_unoptimized && (s.defs & ((RegMask{1} << 29) | (RegMask{1} << 30))) != 0) {
      *pending_arg_defs = 0;
    } else {
      const bool is_jt_base_set = PatternSrcContainsAnyLabelRef(pat, jt_table_labels);
      const RegMask arg_explicit_sets = is_jt_base_set ? RegMask{0} : s.explicit_sets;
      *pending_arg_defs &= ~(s.defs & ~arg_explicit_sets);
      if (is_unoptimized) {
        *pending_arg_defs &= ~((s.uses & kArgRegMask) & ~arg_explicit_sets);
      }
      if (!fp64) {
        for (int even_fpr : {44, 46}) {
          if ((arg_explicit_sets & (RegMask{1} << even_fpr)) != 0 &&
              (arg_explicit_sets & (RegMask{1} << (even_fpr + 1))) == 0) {
            *pending_arg_defs &= ~(RegMask{1} << (even_fpr + 1));
          }
        }
      }
      *pending_arg_defs |= (arg_explicit_sets & kArgRegMask);
    }
  }
  return s;
}

struct InsnSummary {
  bool valid = false;
  bool is_sequence = false;
  bool is_call_seq = false;
  bool is_annulled_target_seq = false;
  RtlPatternAndNotes top_pn;
  RtlExpr seq_head_wrap;
  RtlExpr seq_slot_wrap;
  RtlPatternAndNotes head_pn;
  RtlPatternAndNotes slot_pn;
  ElemSummary single;
  ElemSummary head;
  ElemSummary slot;
};

struct DataflowResult {
  bool gp64 = false;
  bool fp64 = false;
  RegMask live_at_exit = 0;
  ElemSummary epilogue_delay;
  bool has_epilogue_delay = false;
  std::vector<bool> can_exit;
  std::vector<bool> uses_epilogue_delay_at_exit;
  std::vector<RegMask> live_in;
  std::vector<RegMask> live_out;
  std::vector<std::vector<InsnSummary>> block_summaries;
};

std::string ExtractCalleeSymbolRef(const RtlExpr& pat) {
  const RtlExpr* call_node = nullptr;
  if (pat.op == RtlOp::kCall) {
    call_node = &pat;
  } else if (pat.IsSet() && pat.SetSrc().op == RtlOp::kCall) {
    call_node = &pat.SetSrc();
  } else if (pat.IsParallel() && !pat.children.empty()) {
    const RtlExpr& c0 = pat.children[0];
    if (c0.op == RtlOp::kCall) {
      call_node = &c0;
    } else if (c0.IsSet() && c0.SetSrc().op == RtlOp::kCall) {
      call_node = &c0.SetSrc();
    }
  }
  if (call_node != nullptr && !call_node->children.empty() && call_node->children[0].IsMem() &&
      !call_node->children[0].children.empty() &&
      call_node->children[0].children[0].IsSymbolRef()) {
    return std::string(call_node->children[0].children[0].SymbolName());
  }
  return "";
}

// Returns the exact `CALL_INSN_FUNCTION_USAGE` argument register mask emitted
// by KMC GCC 2.7.2 (`optabs.c:1509-1525, 3945-4297` and `mips.c:function_arg`)
// for compiler-generated `libgcc` optab runtime calls, or `std::nullopt` for
// user/external function symbols.
std::optional<RegMask> LookupKnownGccLibcallArgMask(std::string_view sym, bool gp64, bool fp64,
                                                    const CompilationProfile* profile) {
  if (sym.empty()) {
    return std::nullopt;
  }
  bool soft_float = false;
  bool single_float = false;
  if (profile != nullptr) {
    std::vector<std::string> tokens;
    tokens.reserve(profile->c_flags.size() + 1);
    tokens.push_back(absl::StrCat("-O", profile->optimize_level));
    tokens.insert(tokens.end(), profile->c_flags.begin(), profile->c_flags.end());
    const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(tokens);
    single_float = flags.flag_single_float;
    for (const std::string& tok : profile->c_flags) {
      if (tok == "-msoft-float") {
        soft_float = true;
      } else if (tok == "-mhard-float") {
        soft_float = false;
      }
    }
  }

  // Standard unary C libm builtins (`expr.c:7239-7285` `BUILT_IN_FSQRT`/
  // `BUILT_IN_SIN`/`BUILT_IN_COS` falling back to library calls when the ISA
  // lacks hardware `sqrt.s`/`sqrt.d`, e.g. `-mips1`).
  if (sym == "sqrtf" || sym == "sinf" || sym == "cosf") {
    return RegBitMask(soft_float ? 4 : 44, RtlMachineMode::kSf, gp64, fp64);
  }
  if (sym == "sqrt" || sym == "sin" || sym == "cos") {
    return RegBitMask((soft_float || single_float) ? 4 : 44, RtlMachineMode::kDf, gp64, fp64);
  }

  if (sym.size() < 4 || !absl::StartsWith(sym, "__")) {
    return std::nullopt;
  }

  // Unary SFmode -> * (`optabs.c:4117, 4131, 4197, 4267-4268, 4283-4284`).
  if (sym == "__fixsfsi" || sym == "__fixsfdi" || sym == "__fixunssfsi" || sym == "__fixunssfdi" ||
      sym == "__extendsfdf2" || sym == "__ftruncsf2" || sym == "__negsf2") {
    return RegBitMask(soft_float ? 4 : 44, RtlMachineMode::kSf, gp64, fp64);
  }
  // Unary DFmode -> * (`optabs.c:4117, 4131, 4203, 4271-4272, 4287-4288`).
  if (sym == "__fixdfsi" || sym == "__fixdfdi" || sym == "__fixunsdfsi" || sym == "__fixunsdfdi" ||
      sym == "__truncdfsf2" || sym == "__ftruncdf2" || sym == "__negdf2") {
    return RegBitMask((soft_float || single_float) ? 4 : 44, RtlMachineMode::kDf, gp64, fp64);
  }
  // Unary SImode -> * (`optabs.c:4130, 4132-4133, 4251, 4255`).
  if (sym == "__floatsisf" || sym == "__floatsidf" || sym == "__negsi2" || sym == "__one_cmplsi2" ||
      sym == "__ffssi2") {
    return RegBitMask(4, RtlMachineMode::kSi, gp64, fp64);
  }
  // Unary DImode -> * (`optabs.c:4130, 4132-4133, 4252, 4256`).
  if (sym == "__floatdisf" || sym == "__floatdidf" || sym == "__negdi2" || sym == "__one_cmpldi2" ||
      sym == "__ffsdi2") {
    return RegBitMask(4, RtlMachineMode::kDi, gp64, fp64);
  }
  // Binary SImode (`optabs.c:4104-4137`).
  if (sym == "__addsi3" || sym == "__subsi3" || sym == "__mulsi3" || sym == "__divsi3" ||
      sym == "__udivsi3" || sym == "__modsi3" || sym == "__umodsi3" || sym == "__andsi3" ||
      sym == "__iorsi3" || sym == "__xorsi3" || sym == "__ashlsi3" || sym == "__ashrsi3" ||
      sym == "__lshrsi3" || sym == "__cmpsi2" || sym == "__ucmpsi2") {
    return RegBitMask(4, RtlMachineMode::kSi, gp64, fp64) |
           RegBitMask(5, RtlMachineMode::kSi, gp64, fp64);
  }
  // Binary DImode shifts take `(DImode, word_mode)` (`optabs.c:1509-1515`).
  if (sym == "__ashldi3" || sym == "__ashrdi3" || sym == "__lshrdi3") {
    return RegBitMask(4, RtlMachineMode::kDi, gp64, fp64) |
           RegBitMask(gp64 ? 5 : 6, gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi, gp64, fp64);
  }
  // Binary DImode non-shift (`optabs.c:4104-4137`).
  if (sym == "__adddi3" || sym == "__subdi3" || sym == "__muldi3" || sym == "__divdi3" ||
      sym == "__udivdi3" || sym == "__moddi3" || sym == "__umoddi3" || sym == "__anddi3" ||
      sym == "__iordi3" || sym == "__xordi3" || sym == "__cmpdi2" || sym == "__ucmpdi2" ||
      sym == "__mindi3" || sym == "__maxdi3" || sym == "__umindi3" || sym == "__umaxdi3") {
    return RegBitMask(4, RtlMachineMode::kDi, gp64, fp64) |
           RegBitMask(gp64 ? 5 : 6, RtlMachineMode::kDi, gp64, fp64);
  }
  // Binary SFmode (`optabs.c:4105-4138, 4223-4228`).
  if (sym == "__addsf3" || sym == "__subsf3" || sym == "__mulsf3" || sym == "__divsf3" ||
      sym == "__eqsf2" || sym == "__nesf2" || sym == "__gtsf2" || sym == "__gesf2" ||
      sym == "__ltsf2" || sym == "__lesf2" || sym == "__cmpsf2") {
    return soft_float ? (RegBitMask(4, RtlMachineMode::kSf, gp64, fp64) |
                         RegBitMask(5, RtlMachineMode::kSf, gp64, fp64))
                      : (RegBitMask(44, RtlMachineMode::kSf, gp64, fp64) |
                         RegBitMask(46, RtlMachineMode::kSf, gp64, fp64));
  }
  // Binary DFmode (`optabs.c:4105-4138, 4230-4235`).
  if (sym == "__adddf3" || sym == "__subdf3" || sym == "__muldf3" || sym == "__divdf3" ||
      sym == "__eqdf2" || sym == "__nedf2" || sym == "__gtdf2" || sym == "__gedf2" ||
      sym == "__ltdf2" || sym == "__ledf2" || sym == "__cmpdf2") {
    return (soft_float || single_float)
               ? (RegBitMask(4, RtlMachineMode::kDf, gp64, fp64) |
                  RegBitMask(gp64 ? 5 : 6, RtlMachineMode::kDf, gp64, fp64))
               : (RegBitMask(44, RtlMachineMode::kDf, gp64, fp64) |
                  RegBitMask(46, RtlMachineMode::kDf, gp64, fp64));
  }
  return std::nullopt;
}

// Accounts for `jump.c:434-465` (`jump_optimize` with `noop_moves = 1`), which
// deletes redundant `(set (reg:M arg_reg) (reg:M saved_reg))` moves before
// `B0`'s first `CALL_INSN` while keeping `(use (reg:M arg_reg))` in
// `CALL_INSN_FUNCTION_USAGE` and stripping `REG_DEAD(arg_reg)` via
// `remove_death`.
void InferEntryFirstCallDeletedNoopArgMoves(const CompilationProfile* profile,
                                            DataflowResult* res) {
  if (profile == nullptr || profile->optimize_level <= 0 || res->block_summaries.empty()) {
    return;
  }
  const size_t n_blocks = res->block_summaries.size();
  struct PreCallElem {
    const ElemSummary* esum = nullptr;
    const RtlPatternAndNotes* pn = nullptr;
  };
  std::vector<PreCallElem> pre_elems;
  ElemSummary* first_call_elem = nullptr;
  const RtlPatternAndNotes* first_call_pn = nullptr;
  size_t first_call_insn_idx = 0;
  bool b0_ok = true;
  for (size_t i = 0; i < res->block_summaries[0].size(); ++i) {
    InsnSummary& isum = res->block_summaries[0][i];
    if (!isum.valid) {
      b0_ok = false;
      break;
    }
    if (isum.is_sequence) {
      if (isum.is_call_seq) {
        pre_elems.push_back(PreCallElem{&isum.slot, &isum.slot_pn});
        first_call_elem = &isum.head;
        first_call_pn = &isum.head_pn;
        first_call_insn_idx = i;
      }
      break;
    }
    if (IsAnyControlTransferPattern(isum.top_pn.pattern)) {
      break;
    }
    if (isum.single.is_call) {
      first_call_elem = &isum.single;
      first_call_pn = &isum.top_pn;
      first_call_insn_idx = i;
      break;
    }
    pre_elems.push_back(PreCallElem{&isum.single, &isum.top_pn});
  }

  if (!b0_ok || first_call_elem == nullptr || first_call_pn->has_explicit_call_fusage ||
      LookupKnownGccLibcallArgMask(ExtractCalleeSymbolRef(first_call_pn->pattern), res->gp64,
                                   res->fp64, profile)
          .has_value()) {
    return;
  }

  // Callee-saved GPRs (`$s0..$s7`, `$fp`) and FPRs (`$f20..$f30` per
  // `config/mips/mips.h` `CALL_USED_REGISTERS`).
  auto is_callee_saved_reg = [](int r) -> bool {
    return (r >= 16 && r <= 23) || r == 30 || (r >= 52 && r < 64);
  };
  auto match_prologue_sp_save = [](const RtlExpr& pat, int* out_reg,
                                   RtlMachineMode* out_mode) -> bool {
    if (!pat.IsSet() || pat.SetDest().op != RtlOp::kMem || pat.SetSrc().op != RtlOp::kReg ||
        pat.SetDest().children.size() != 1) {
      return false;
    }
    const RtlExpr& addr = pat.SetDest().children[0];
    bool sp_addr = false;
    if (addr.op == RtlOp::kReg && addr.RegNo() == 29) {
      sp_addr = true;
    } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
               addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == 29 &&
               addr.children[1].op == RtlOp::kConstInt && addr.children[1].IntVal() >= 0) {
      sp_addr = true;
    }
    if (!sp_addr) {
      return false;
    }
    *out_reg = pat.SetSrc().RegNo();
    *out_mode = pat.SetSrc().mode;
    return true;
  };

  int ra_save_pos = -1;
  absl::flat_hash_map<int, size_t> callee_save_pos;
  for (size_t idx = 0; idx < pre_elems.size(); ++idx) {
    int saved_reg = -1;
    RtlMachineMode save_mode = RtlMachineMode::kSi;
    if (match_prologue_sp_save(pre_elems[idx].pn->pattern, &saved_reg, &save_mode)) {
      if (saved_reg == 31 && ra_save_pos < 0) {
        ra_save_pos = static_cast<int>(idx);
      } else if (is_callee_saved_reg(saved_reg)) {
        const int span = HardRegNregs(saved_reg, save_mode, res->gp64, res->fp64);
        for (int off = 0; off < span; ++off) {
          if (is_callee_saved_reg(saved_reg + off) && !callee_save_pos.contains(saved_reg + off)) {
            callee_save_pos[saved_reg + off] = idx;
          }
        }
      }
    }
  }
  if (ra_save_pos < 0) {
    return;
  }

  RegMask read_after_first_call = 0;
  for (size_t i = first_call_insn_idx + 1; i < res->block_summaries[0].size(); ++i) {
    const InsnSummary& s = res->block_summaries[0][i];
    if (!s.valid) continue;
    read_after_first_call |= s.is_sequence ? (s.head.uses | s.slot.uses) : s.single.uses;
  }
  for (size_t b = 1; b < n_blocks; ++b) {
    for (const InsnSummary& s : res->block_summaries[b]) {
      if (!s.valid) continue;
      read_after_first_call |= s.is_sequence ? (s.head.uses | s.slot.uses) : s.single.uses;
    }
  }

  constexpr RegMask kGprArgMask =
      (RegMask{1} << 4) | (RegMask{1} << 5) | (RegMask{1} << 6) | (RegMask{1} << 7);
  RegMask arg_gpr_live_into_call = first_call_elem->arg_fusage_uses & kGprArgMask;
  for (int r = 4; r <= 7; ++r) {
    int last_def = -1;
    for (size_t idx = 0; idx < pre_elems.size(); ++idx) {
      if ((pre_elems[idx].esum->defs & (RegMask{1} << r)) != 0) {
        last_def = ((pre_elems[idx].esum->explicit_sets & (RegMask{1} << r)) != 0)
                       ? static_cast<int>(idx)
                       : -1;
      }
    }
    if (last_def >= 0) {
      arg_gpr_live_into_call |= (RegMask{1} << r);
    }
  }

  auto fpr_live_into_call = [&](int regno, bool* out_is_df) -> bool {
    if ((first_call_elem->arg_fusage_uses & (RegMask{1} << regno)) != 0) {
      if (out_is_df != nullptr) {
        *out_is_df = ((first_call_elem->arg_fusage_uses & (RegMask{1} << (regno + 1))) != 0);
      }
      return true;
    }
    int last_def = -1;
    for (size_t idx = 0; idx < pre_elems.size(); ++idx) {
      if ((pre_elems[idx].esum->defs & (RegMask{1} << regno)) != 0) {
        last_def = ((pre_elems[idx].esum->explicit_sets & (RegMask{1} << regno)) != 0)
                       ? static_cast<int>(idx)
                       : -1;
      }
    }
    if (last_def < 0) {
      return false;
    }
    if (out_is_df != nullptr) {
      *out_is_df = res->fp64 ? (pre_elems[static_cast<size_t>(last_def)].esum->def_modes[regno] ==
                                RtlMachineMode::kDf)
                             : ((pre_elems[static_cast<size_t>(last_def)].esum->explicit_sets &
                                 (RegMask{1} << (regno + 1))) != 0);
    }
    return true;
  };

  RegMask arg_word_occupied_by_fpr = 0;
  bool f12_is_df = false;
  if (fpr_live_into_call(44, &f12_is_df)) {
    arg_word_occupied_by_fpr |= (RegMask{1} << 4);
    if (!res->gp64 && f12_is_df) {
      arg_word_occupied_by_fpr |= (RegMask{1} << 5);
    }
    bool f14_is_df = false;
    if (fpr_live_into_call(46, &f14_is_df)) {
      if (res->gp64) {
        arg_word_occupied_by_fpr |= (RegMask{1} << 5);
      } else if (f12_is_df) {
        arg_word_occupied_by_fpr |= (RegMask{1} << 6);
        if (f14_is_df) {
          arg_word_occupied_by_fpr |= (RegMask{1} << 7);
        }
      } else {
        arg_word_occupied_by_fpr |= (RegMask{1} << 5);
        if (f14_is_df) {
          arg_word_occupied_by_fpr |= (RegMask{1} << 6) | (RegMask{1} << 7);
        }
      }
    }
  }

  struct PreCallCopyCand {
    size_t copy_pos = 0;
    int saved_reg = -1;
    int arg_reg = -1;
    int saved_span = 1;
    int arg_span = 1;
    bool saved_reg_read_before_call = false;
  };
  std::vector<PreCallCopyCand> cands;
  RegMask arg_gpr_saved_in_callee_reg = 0;
  for (size_t idx = 0; idx < pre_elems.size(); ++idx) {
    const RtlExpr& pat = pre_elems[idx].pn->pattern;
    if (pre_elems[idx].esum->is_call || pre_elems[idx].esum->is_note_or_pseudo || !pat.IsSet() ||
        pat.SetDest().op != RtlOp::kReg || pat.SetSrc().op != RtlOp::kReg) {
      continue;
    }
    const RtlMachineMode mode = pat.SetDest().mode;
    if (mode != pat.SetSrc().mode || (mode != RtlMachineMode::kSi && mode != RtlMachineMode::kSf &&
                                      mode != RtlMachineMode::kDi && mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int saved_reg = pat.SetDest().RegNo();
    const int arg_reg = pat.SetSrc().RegNo();
    const int saved_span = HardRegNregs(saved_reg, mode, res->gp64, res->fp64);
    const int arg_span = HardRegNregs(arg_reg, mode, res->gp64, res->fp64);
    if (!is_callee_saved_reg(saved_reg) || arg_reg < 4 || arg_reg + arg_span - 1 > 7) {
      continue;
    }
    if (saved_span == 2 && !is_callee_saved_reg(saved_reg + 1)) {
      continue;
    }
    bool valid = true;
    bool saved_reg_read_before_call = false;
    for (int off = 0; off < saved_span; ++off) {
      const int sr = saved_reg + off;
      if ((read_after_first_call & (RegMask{1} << sr)) == 0) {
        valid = false;
      }
      // Per `jump.c:434-465` (`reload.c:find_equiv_reg`), reading `saved_reg`
      // before the call preserves equivalence `saved_reg == arg_reg`; only a
      // write to `saved_reg` invalidates the noop-move deletion.
      for (size_t k = idx + 1; k < pre_elems.size(); ++k) {
        if ((pre_elems[k].esum->defs & (RegMask{1} << sr)) != 0) {
          valid = false;
        }
        if ((pre_elems[k].esum->uses & (RegMask{1} << sr)) != 0) {
          saved_reg_read_before_call = true;
        }
      }
    }
    for (int off = 0; off < arg_span; ++off) {
      const int ar = arg_reg + off;
      if ((arg_word_occupied_by_fpr & (RegMask{1} << ar)) != 0) {
        valid = false;
      }
      for (size_t k = 0; k < idx; ++k) {
        if ((pre_elems[k].esum->defs & (RegMask{1} << ar)) != 0) {
          valid = false;
        }
      }
      for (size_t k = idx + 1; k < pre_elems.size(); ++k) {
        if ((pre_elems[k].esum->defs & (RegMask{1} << ar)) != 0 ||
            (pre_elems[k].esum->uses & (RegMask{1} << ar)) != 0) {
          valid = false;
        }
      }
    }
    if (!valid) {
      continue;
    }
    cands.push_back(
        PreCallCopyCand{idx, saved_reg, arg_reg, saved_span, arg_span, saved_reg_read_before_call});
    for (int off = 0; off < arg_span; ++off) {
      arg_gpr_saved_in_callee_reg |= (RegMask{1} << (arg_reg + off));
    }
  }

  auto has_cand = [&](int sr, int ar) -> bool {
    for (const PreCallCopyCand& c : cands) {
      if (c.saved_reg == sr && c.arg_reg == ar && c.saved_span == 1 && c.arg_span == 1) {
        return true;
      }
    }
    return false;
  };

  bool ra_sank_into_indirect_target_load_shadow = false;
  if (ParseCompilationProfileFlags(*profile).RunsSched2()) {
    int indirect_target_reg = -1;
    first_call_pn->pattern.Visit([&indirect_target_reg](const RtlExpr& n) {
      if (n.op == RtlOp::kCall && !n.children.empty() && n.children[0].op == RtlOp::kMem &&
          !n.children[0].children.empty() && n.children[0].children[0].op == RtlOp::kReg) {
        indirect_target_reg = n.children[0].children[0].RegNo();
      }
    });
    if (indirect_target_reg >= 1 && indirect_target_reg < 32 &&
        (indirect_target_reg < 4 || indirect_target_reg > 7)) {
      std::optional<size_t> target_def_pos;
      bool target_def_is_mem_load = false;
      for (size_t idx = 0; idx < pre_elems.size(); ++idx) {
        if ((pre_elems[idx].esum->defs & (RegMask{1} << indirect_target_reg)) != 0) {
          target_def_pos = idx;
          const RtlExpr& dpat = pre_elems[idx].pn->pattern;
          bool has_mem = false;
          if (dpat.IsSet()) {
            dpat.SetSrc().Visit([&has_mem](const RtlExpr& n) {
              if (n.op == RtlOp::kMem) {
                has_mem = true;
              }
            });
          }
          target_def_is_mem_load = has_mem;
        }
      }
      if (target_def_pos.has_value() && target_def_is_mem_load &&
          *target_def_pos < static_cast<size_t>(ra_save_pos)) {
        ra_sank_into_indirect_target_load_shadow = true;
      }
    }
  }

  int64_t call_arg_bytes = 0;
  first_call_pn->pattern.Visit([&call_arg_bytes](const RtlExpr& n) {
    if (n.op == RtlOp::kCall && n.children.size() >= 2 && n.children[1].op == RtlOp::kConstInt) {
      call_arg_bytes = std::max(call_arg_bytes, n.children[1].int_val);
    }
  });
  const bool runs_sched2 = ParseCompilationProfileFlags(*profile).RunsSched2();

  bool arg6_written_as_8byte = false;
  if (!res->gp64 && (arg_gpr_live_into_call & (RegMask{1} << 6)) != 0 &&
      (arg_gpr_live_into_call & (RegMask{1} << 7)) != 0) {
    for (size_t k = pre_elems.size(); k > 0; --k) {
      const RegMask d = pre_elems[k - 1].esum->defs;
      if ((d & ((RegMask{1} << 6) | (RegMask{1} << 7))) != 0) {
        const RtlExpr& kp = pre_elems[k - 1].pn->pattern;
        if (kp.IsSet() && kp.SetDest().IsHardReg() && kp.SetDest().RegNo() == 6 &&
            (kp.SetDest().mode == RtlMachineMode::kDf ||
             kp.SetDest().mode == RtlMachineMode::kDi)) {
          arg6_written_as_8byte = true;
        }
        break;
      }
    }
  }

  for (const PreCallCopyCand& cand : cands) {
    bool save_hoisted_before_ra = false;
    // When `saved_reg` is already consumed before the call, `sched2` already
    // has a pre-call dependency chain to hoist its prologue save ahead of
    // `sw $ra`; require an explicitly-written higher arg register instead.
    if (!ra_sank_into_indirect_target_load_shadow && !cand.saved_reg_read_before_call) {
      for (int off = 0; off < cand.saved_span; ++off) {
        auto it = callee_save_pos.find(cand.saved_reg + off);
        if (it != callee_save_pos.end() && it->second < static_cast<size_t>(ra_save_pos) &&
            it->second < cand.copy_pos) {
          bool preceded_by_non_copy_gpr_alu = false;
          if (runs_sched2 && cand.saved_reg >= 32 && call_arg_bytes <= 16) {
            for (size_t k = 0; k < it->second; ++k) {
              const RtlExpr& kp = pre_elems[k].pn->pattern;
              if (kp.IsSet() && kp.SetDest().IsHardReg() && kp.SetDest().RegNo() >= 2 &&
                  kp.SetDest().RegNo() < 29 && !kp.SetSrc().IsHardReg() && !kp.SetSrc().IsMem()) {
                preceded_by_non_copy_gpr_alu = true;
                break;
              }
            }
          }
          if (!preceded_by_non_copy_gpr_alu) {
            save_hoisted_before_ra = true;
          }
        }
      }
      if (!res->gp64 && cand.saved_span == 1 && cand.arg_span == 1 && (cand.saved_reg % 2) == 1 &&
          cand.arg_reg == 7 && has_cand(cand.saved_reg - 1, 6)) {
        auto it_hi = callee_save_pos.find(cand.saved_reg - 1);
        if (it_hi != callee_save_pos.end() && it_hi->second < static_cast<size_t>(ra_save_pos) &&
            it_hi->second < cand.copy_pos) {
          save_hoisted_before_ra = true;
        }
      }
    }

    bool higher_arg_written = false;
    for (int r = cand.arg_reg + cand.arg_span; r <= 7; ++r) {
      if (cand.arg_reg == 5 && cand.arg_span == 1 && arg6_written_as_8byte) {
        break;
      }
      if ((arg_gpr_live_into_call & (RegMask{1} << r)) != 0) {
        higher_arg_written = true;
      }
    }

    const bool is_o32_aligned_pair_at_6 =
        (cand.arg_span == 2 && cand.arg_reg == 6) ||
        (!res->gp64 && cand.saved_span == 1 && cand.arg_span == 1 &&
         ((cand.arg_reg == 6 && (cand.saved_reg % 2) == 0 && has_cand(cand.saved_reg + 1, 7)) ||
          (cand.arg_reg == 7 && (cand.saved_reg % 2) == 1 && has_cand(cand.saved_reg - 1, 6))));

    bool all_lower_args_supplied = true;
    for (int r = 4; r < cand.arg_reg; ++r) {
      if (is_o32_aligned_pair_at_6 && r == 5) {
        continue;
      }
      if (((arg_gpr_live_into_call | arg_gpr_saved_in_callee_reg | arg_word_occupied_by_fpr) &
           (RegMask{1} << r)) == 0) {
        all_lower_args_supplied = false;
      }
    }

    if (all_lower_args_supplied && (save_hoisted_before_ra || higher_arg_written)) {
      for (int off = 0; off < cand.arg_span; ++off) {
        const RegMask bit = RegMask{1} << (cand.arg_reg + off);
        first_call_elem->arg_fusage_uses |= bit;
        first_call_elem->uses |= bit;
      }
    }
  }
}

// Identifies argument hard registers (`$4..$7`, `$f12..$f15`) that are
// caller-saved and restored across an unannotated `CALL_INSN` (`caller-save.c`).
// By `local-alloc.c` and `global.c:global_conflicts`, an outgoing call argument
// register `r` is live into `CALL_INSN` and conflicts with every call-crossing
// pseudo, so `caller-save` can never save/restore an actual outgoing argument
// of that call. Excluding such caller-saved registers prevents pre-call writes
// to `r` (`pending_arg_defs`) from synthesizing spurious `call_fusage` uses.
std::vector<std::vector<RegMask>> ComputeCallerSavedArgMasks(const DataflowResult& res,
                                                             const CompilationProfile* profile) {
  const size_t n_blocks = res.block_summaries.size();
  std::vector<std::vector<RegMask>> out(n_blocks);
  for (size_t b = 0; b < n_blocks; ++b) {
    out[b].assign(res.block_summaries[b].size(), 0);
  }
  if (profile != nullptr) {
    std::vector<std::string> effective_flags;
    effective_flags.reserve(profile->c_flags.size() + 1);
    effective_flags.push_back(absl::StrCat("-O", profile->optimize_level));
    effective_flags.insert(effective_flags.end(), profile->c_flags.begin(), profile->c_flags.end());
    const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);
    if (flags.opt_level <= 0 || !flags.flag_caller_saves) {
      return out;
    }
  }

  auto extract_stack_mem = [](const RtlExpr& mem, int* out_base, int64_t* out_off,
                              int64_t* out_sz) -> bool {
    if (mem.op != RtlOp::kMem || mem.flags.volatil || mem.children.size() != 1) {
      return false;
    }
    const RtlExpr& addr = mem.children[0];
    int base = -1;
    int64_t off = 0;
    if (addr.op == RtlOp::kReg) {
      base = addr.RegNo();
    } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
               addr.children[0].op == RtlOp::kReg && addr.children[1].op == RtlOp::kConstInt) {
      base = addr.children[0].RegNo();
      off = addr.children[1].IntVal();
    } else {
      return false;
    }
    if ((base != 29 && base != 30) || off < 0) {
      return false;
    }
    *out_base = base;
    *out_off = off;
    *out_sz = ModeByteSize(mem.mode);
    return *out_sz > 0;
  };

  auto match_arg_save = [&](const RtlExpr& pat, int* out_base, int64_t* out_off, int64_t* out_sz,
                            int* out_reg, RtlMachineMode* out_mode) -> bool {
    if (!pat.IsSet()) return false;
    const RtlExpr& dst = pat.SetDest();
    const RtlExpr& src = pat.SetSrc();
    if (dst.op != RtlOp::kMem || dst.flags.Any() || src.op != RtlOp::kReg || src.flags.Any() ||
        dst.mode != src.mode) {
      return false;
    }
    const int r = src.RegNo();
    const RegMask bits = RegBitMask(r, src.mode, res.gp64, res.fp64);
    if (bits == 0 || (bits & ~kArgRegMask) != 0) {
      return false;
    }
    if (!extract_stack_mem(dst, out_base, out_off, out_sz) || *out_off < 16) {
      return false;
    }
    *out_reg = r;
    *out_mode = src.mode;
    return true;
  };

  auto match_arg_restore = [&](const RtlExpr& pat, int* out_base, int64_t* out_off, int64_t* out_sz,
                               int* out_reg, RtlMachineMode* out_mode) -> bool {
    if (!pat.IsSet()) return false;
    const RtlExpr& dst = pat.SetDest();
    const RtlExpr& src = pat.SetSrc();
    if (dst.op != RtlOp::kReg || dst.flags.Any() || src.op != RtlOp::kMem || src.flags.Any() ||
        dst.mode != src.mode) {
      return false;
    }
    const int r = dst.RegNo();
    const RegMask bits = RegBitMask(r, dst.mode, res.gp64, res.fp64);
    if (bits == 0 || (bits & ~kArgRegMask) != 0) {
      return false;
    }
    if (!extract_stack_mem(src, out_base, out_off, out_sz) || *out_off < 16) {
      return false;
    }
    *out_reg = r;
    *out_mode = dst.mode;
    return true;
  };

  struct SlotAccess {
    int base = -1;
    int64_t off = 0;
    int64_t sz = 0;
    int reg = -1;
    RtlMachineMode mode = RtlMachineMode::kNone;
    bool is_save = false;
    bool is_restore = false;
  };
  std::vector<SlotAccess> all_stack_accesses;
  absl::flat_hash_set<int64_t> addr_taken_offsets;
  bool has_volatile_stack_mem = false;

  auto scan_non_mem_addrs = [&](auto& self, const RtlExpr& expr, bool top_sp_fp_set) -> void {
    if (expr.op == RtlOp::kMem) {
      if (expr.flags.volatil && !expr.children.empty()) {
        const RtlExpr& a = expr.children[0];
        if ((a.op == RtlOp::kReg && (a.RegNo() == 29 || a.RegNo() == 30)) ||
            (a.op == RtlOp::kPlus && !a.children.empty() && a.children[0].op == RtlOp::kReg &&
             (a.children[0].RegNo() == 29 || a.children[0].RegNo() == 30))) {
          has_volatile_stack_mem = true;
        }
      }
      return;
    }
    if (!top_sp_fp_set && expr.op == RtlOp::kPlus && expr.children.size() == 2 &&
        expr.children[0].op == RtlOp::kReg &&
        (expr.children[0].RegNo() == 29 || expr.children[0].RegNo() == 30) &&
        expr.children[1].op == RtlOp::kConstInt) {
      addr_taken_offsets.insert(expr.children[1].IntVal());
      return;
    }
    for (const RtlExpr& ch : expr.children) {
      self(self, ch, false);
    }
  };

  auto inspect_elem_pat = [&](const RtlExpr& pat) {
    const bool is_sp_fp_set = pat.IsSet() && pat.SetDest().op == RtlOp::kReg &&
                              (pat.SetDest().RegNo() == 29 || pat.SetDest().RegNo() == 30);
    scan_non_mem_addrs(scan_non_mem_addrs, pat, is_sp_fp_set);

    int base = -1;
    int64_t off = 0;
    int64_t sz = 0;
    int reg = -1;
    RtlMachineMode mode = RtlMachineMode::kNone;
    if (match_arg_save(pat, &base, &off, &sz, &reg, &mode)) {
      all_stack_accesses.push_back({base, off, sz, reg, mode, true, false});
      return;
    }
    if (match_arg_restore(pat, &base, &off, &sz, &reg, &mode)) {
      all_stack_accesses.push_back({base, off, sz, reg, mode, false, true});
      return;
    }
    pat.Visit([&](const RtlExpr& n) {
      int mb = -1;
      int64_t mo = 0;
      int64_t ms = 0;
      if (extract_stack_mem(n, &mb, &mo, &ms)) {
        all_stack_accesses.push_back({mb, mo, ms, -1, RtlMachineMode::kNone, false, false});
      }
    });
  };

  for (size_t b = 0; b < n_blocks; ++b) {
    for (const InsnSummary& isum : res.block_summaries[b]) {
      if (!isum.valid) continue;
      if (isum.is_sequence) {
        inspect_elem_pat(isum.head_pn.pattern);
        inspect_elem_pat(isum.slot_pn.pattern);
      } else {
        inspect_elem_pat(isum.top_pn.pattern);
      }
    }
  }
  if (has_volatile_stack_mem) {
    return out;
  }

  struct PureCsSlot {
    int base = -1;
    int64_t off = 0;
    int64_t sz = 0;
    int reg = -1;
    RtlMachineMode mode = RtlMachineMode::kNone;
  };
  std::vector<PureCsSlot> pure_slots;
  for (const SlotAccess& cand : all_stack_accesses) {
    if (!cand.is_save || addr_taken_offsets.contains(cand.off)) {
      continue;
    }
    bool already_added = false;
    for (const PureCsSlot& ps : pure_slots) {
      if (ps.base == cand.base && ps.off == cand.off && ps.sz == cand.sz) {
        already_added = true;
        break;
      }
    }
    if (already_added) continue;

    bool pure = true;
    bool saw_restore = false;
    for (const SlotAccess& other : all_stack_accesses) {
      if (other.off < cand.off + cand.sz && cand.off < other.off + other.sz) {
        if ((!other.is_save && !other.is_restore) || other.base != cand.base ||
            other.off != cand.off || other.sz != cand.sz || other.reg != cand.reg ||
            other.mode != cand.mode) {
          pure = false;
          break;
        }
        if (other.is_restore) {
          saw_restore = true;
        }
      }
    }
    if (pure && saw_restore) {
      pure_slots.push_back({cand.base, cand.off, cand.sz, cand.reg, cand.mode});
    }
  }
  if (pure_slots.empty()) {
    return out;
  }

  struct FlatBlockElem {
    const ElemSummary* esum = nullptr;
    const RtlPatternAndNotes* pn = nullptr;
    size_t insn_idx = 0;
    bool is_target_call = false;
  };
  for (size_t b = 0; b < n_blocks; ++b) {
    std::vector<FlatBlockElem> flat;
    flat.reserve(res.block_summaries[b].size() * 2);
    for (size_t i = 0; i < res.block_summaries[b].size(); ++i) {
      const InsnSummary& isum = res.block_summaries[b][i];
      if (!isum.valid) continue;
      if (isum.is_sequence) {
        if (isum.is_call_seq) {
          flat.push_back({&isum.slot, &isum.slot_pn, i, false});
          flat.push_back({&isum.head, &isum.head_pn, i, !isum.head_pn.has_explicit_call_fusage});
        } else {
          flat.push_back({&isum.head, &isum.head_pn, i, false});
          flat.push_back({&isum.slot, &isum.slot_pn, i, false});
        }
      } else {
        flat.push_back({&isum.single, &isum.top_pn, i,
                        isum.single.is_call && !isum.top_pn.has_explicit_call_fusage});
      }
    }
    for (size_t pos = 0; pos < flat.size(); ++pos) {
      if (!flat[pos].is_target_call) continue;
      for (const PureCsSlot& slot : pure_slots) {
        const RegMask arg_bits = RegBitMask(slot.reg, slot.mode, res.gp64, res.fp64) & kArgRegMask;
        bool found_pre_save = false;
        for (size_t rev = pos; rev > 0; --rev) {
          const FlatBlockElem& prev = flat[rev - 1];
          int b_in = -1, r_in = -1;
          int64_t o_in = 0, s_in = 0;
          RtlMachineMode m_in = RtlMachineMode::kNone;
          if (match_arg_save(prev.pn->pattern, &b_in, &o_in, &s_in, &r_in, &m_in) &&
              b_in == slot.base && o_in == slot.off && r_in == slot.reg && m_in == slot.mode) {
            found_pre_save = true;
            break;
          }
          if (prev.esum->is_call || (prev.esum->defs & arg_bits) != 0) {
            break;
          }
        }
        if (!found_pre_save) continue;

        bool found_post_restore = false;
        for (size_t fwd = pos + 1; fwd < flat.size(); ++fwd) {
          const FlatBlockElem& next = flat[fwd];
          int b_in = -1, r_in = -1;
          int64_t o_in = 0, s_in = 0;
          RtlMachineMode m_in = RtlMachineMode::kNone;
          if (match_arg_restore(next.pn->pattern, &b_in, &o_in, &s_in, &r_in, &m_in) &&
              b_in == slot.base && o_in == slot.off && r_in == slot.reg && m_in == slot.mode) {
            found_post_restore = true;
            break;
          }
          if (next.esum->is_call || (next.esum->defs & arg_bits) != 0) {
            break;
          }
        }
        if (found_post_restore) {
          out[b][flat[pos].insn_idx] |= arg_bits;
        }
      }
    }
  }
  return out;
}

// Identifies `$sp + off` stack slots that are proven `reload1.c` GPR pseudo
// spill slots (`assign_stack_local` 8-byte aligned buckets with big-endian
// offset `bk + (8 - sz)`, accessed by at least two distinct registers when all
// callee-saved GPRs `16..23, 30` are already in use). By `reload1.c`, if a
// spilled pseudo were passed in an outgoing call argument register `$4..$7`,
// `alter_reg` would materialize `(set (reg dst_r) (mem (plus sp off)))`, which
// satisfies `movsi_internal2` (`"=d,m"`) with zero reloads and is never
// deleted. Hence an output-reload scratch `(set (reg dst_r) <op>); (set (mem
// (plus sp off)) (reg dst_r))` with no subsequent write or non-spill read of
// `dst_r` before `CALL_INSN` cannot be an outgoing call argument.
absl::flat_hash_set<int64_t> CollectProvenGprSpillOffsetsForCallFusage(
    const RtlControlFlowGraph& cfg, const DataflowResult& res, int64_t effective_sp_local_limit,
    const absl::flat_hash_set<int64_t>& addr_taken_sp_offsets) {
  absl::flat_hash_set<int64_t> out;
  if (cfg.sideband.frame_pointer_needed || effective_sp_local_limit <= 0 ||
      !addr_taken_sp_offsets.empty() || res.block_summaries.empty()) {
    return out;
  }
  uint32_t saved_gpr_mask = 0;
  for (int r : cfg.sideband.saved_gpr_regnos) {
    if ((r >= 16 && r <= 23) || r == 30) {
      saved_gpr_mask |= (uint32_t{1} << r);
    }
  }
  int64_t b0_tsize = 0;
  bool saw_b0_sp_dec = false;
  for (const InsnSummary& isum : res.block_summaries[0]) {
    if (!isum.valid) continue;
    auto inspect_b0 = [&](const RtlExpr& pat, const ElemSummary& esum) -> bool {
      if (esum.is_call || IsAnyControlTransferPattern(pat)) {
        return false;
      }
      if (!saw_b0_sp_dec) {
        if (pat.IsSet() && pat.SetDest().IsReg() && pat.SetDest().RegNo() == 29 &&
            pat.SetSrc().op == RtlOp::kPlus && pat.SetSrc().children.size() == 2 &&
            pat.SetSrc().children[0].IsReg() && pat.SetSrc().children[0].RegNo() == 29 &&
            pat.SetSrc().children[1].IsConstInt() && pat.SetSrc().children[1].int_val < 0) {
          b0_tsize = -pat.SetSrc().children[1].int_val;
          saw_b0_sp_dec = true;
        }
        return true;
      }
      if (pat.IsSet() && pat.SetDest().IsMem() && pat.SetDest().children.size() == 1 &&
          pat.SetSrc().IsReg()) {
        const int sr = pat.SetSrc().RegNo();
        if ((sr >= 16 && sr <= 23) || sr == 30) {
          const RtlExpr& a = pat.SetDest().children[0];
          int64_t off = -1;
          if (a.IsReg() && a.RegNo() == 29) {
            off = 0;
          } else if (a.op == RtlOp::kPlus && a.children.size() == 2 && a.children[0].IsReg() &&
                     a.children[0].RegNo() == 29 && a.children[1].IsConstInt()) {
            off = a.children[1].int_val;
          }
          if (off >= 0 && off < b0_tsize) {
            saved_gpr_mask |= (uint32_t{1} << sr);
          }
        }
      }
      return true;
    };
    if (isum.is_sequence) {
      if (!inspect_b0(isum.head_pn.pattern, isum.head) ||
          !inspect_b0(isum.slot_pn.pattern, isum.slot)) {
        break;
      }
    } else if (!inspect_b0(isum.top_pn.pattern, isum.single)) {
      break;
    }
  }
  constexpr uint32_t kAllCalleeSavedGprsMask = 0x40ff0000U;
  if ((saved_gpr_mask & kAllCalleeSavedGprsMask) != kAllCalleeSavedGprsMask) {
    return out;
  }

  struct SlotStat {
    int n_stores = 0;
    int n_loads = 0;
    int64_t sz = 0;
    RtlMachineMode mode = RtlMachineMode::kNone;
    int uniform_reg = -2;
  };
  absl::flat_hash_map<int64_t, SlotStat> slot_stats;
  absl::flat_hash_map<int64_t, int64_t> bucket_unique_off;
  absl::flat_hash_set<int64_t> bad_buckets;
  bool any_var_sp_mem = false;
  auto mode_sz = [](RtlMachineMode m) -> int64_t {
    switch (m) {
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
      default:
        return 0;
    }
  };
  for (const auto& blk_sums : res.block_summaries) {
    for (const InsnSummary& isum : blk_sums) {
      if (!isum.valid) continue;
      auto scan_pat = [&](const RtlExpr& pat) {
        pat.Visit([&](const RtlExpr& n) {
          if (any_var_sp_mem || n.op != RtlOp::kMem) return;
          if (n.children.size() != 1) {
            if (n.ContainsReg(29)) any_var_sp_mem = true;
            return;
          }
          const RtlExpr& a = n.children[0];
          int64_t off = -1;
          if (a.IsReg() && a.RegNo() == 29) {
            off = 0;
          } else if (a.op == RtlOp::kPlus && a.children.size() == 2 && a.children[0].IsReg() &&
                     a.children[0].RegNo() == 29 && a.children[1].IsConstInt()) {
            off = a.children[1].int_val;
          } else {
            if (a.ContainsReg(29)) any_var_sp_mem = true;
            return;
          }
          if (off < 0 || off >= effective_sp_local_limit) return;
          const int64_t sz = mode_sz(n.mode);
          const int64_t bk = (off / 8) * 8;
          if (n.flags.volatil || n.flags.in_struct || sz <= 0 || ((off + sz - 1) / 8) * 8 != bk) {
            bad_buckets.insert(bk);
            return;
          }
          const bool is_st = pat.IsSet() && &pat.SetDest() == &n && pat.SetSrc().IsReg() &&
                             !pat.SetSrc().flags.integrated;
          const bool is_ld = pat.IsSet() && &pat.SetSrc() == &n && pat.SetDest().IsReg() &&
                             !pat.SetDest().flags.integrated;
          if (!is_st && !is_ld) {
            bad_buckets.insert(bk);
            return;
          }
          if (const auto bit = bucket_unique_off.find(bk);
              bit != bucket_unique_off.end() && bit->second != off) {
            bad_buckets.insert(bk);
          } else {
            bucket_unique_off[bk] = off;
          }
          SlotStat& st = slot_stats[off];
          if (st.mode == RtlMachineMode::kNone) {
            st.mode = n.mode;
            st.sz = sz;
          } else if (st.mode != n.mode || st.sz != sz) {
            bad_buckets.insert(bk);
          }
          const int r = is_st ? pat.SetSrc().RegNo() : pat.SetDest().RegNo();
          st.uniform_reg = (st.uniform_reg == -2) ? r : (st.uniform_reg == r ? r : -1);
          if (is_st) {
            ++st.n_stores;
          } else {
            ++st.n_loads;
          }
        });
      };
      if (isum.is_sequence) {
        scan_pat(isum.head_pn.pattern);
        scan_pat(isum.slot_pn.pattern);
      } else {
        scan_pat(isum.top_pn.pattern);
      }
    }
  }
  if (any_var_sp_mem) {
    return out;
  }
  const int64_t min_spill_bk =
      ((std::max<int64_t>(res.gp64 ? 32 : 16, cfg.sideband.outgoing_args_size) + 7) / 8) * 8;
  for (const auto& [off, st] : slot_stats) {
    const int64_t bk = (off / 8) * 8;
    const bool valid_gpr_mode =
        (st.mode == RtlMachineMode::kSi && st.sz == 4 && off == bk + 4) ||
        (res.gp64 && st.mode == RtlMachineMode::kDi && st.sz == 8 && off == bk);
    if (bk >= min_spill_bk && !bad_buckets.contains(bk) && valid_gpr_mode && st.n_stores >= 1 &&
        st.n_loads >= 1 && st.uniform_reg == -1) {
      out.insert(off);
    }
  }
  return out;
}

void ComputeSpLocalLimitAndAddrTakenOffsets(const RtlControlFlowGraph& cfg,
                                            const CompilationProfile* profile, bool is_unoptimized,
                                            const DataflowResult& res,
                                            int64_t* out_effective_sp_local_limit,
                                            absl::flat_hash_set<int64_t>* out_addr_taken) {
  int64_t effective_sp_local_limit = cfg.sideband.frame_size_bytes;
  const size_t n_blocks = cfg.rpo_blocks.size();
  if (!cfg.sideband.frame_pointer_needed && n_blocks > 0) {
    int64_t b0_tsize = 0;
    int64_t min_prologue_save_off = std::numeric_limits<int64_t>::max();
    bool saw_b0_sp_dec = false;
    for (const InsnSummary& isum : res.block_summaries[0]) {
      if (!isum.valid) continue;
      auto inspect_prologue_pat = [&](const RtlExpr& pat, const ElemSummary& esum) -> bool {
        if (esum.is_call || IsAnyControlTransferPattern(pat)) {
          return false;
        }
        if (!saw_b0_sp_dec) {
          if (pat.IsSet() && pat.SetDest().IsReg() && pat.SetDest().RegNo() == 29 &&
              pat.SetSrc().op == RtlOp::kPlus && pat.SetSrc().children.size() == 2 &&
              pat.SetSrc().children[0].IsReg() && pat.SetSrc().children[0].RegNo() == 29 &&
              pat.SetSrc().children[1].IsConstInt() && pat.SetSrc().children[1].int_val < 0) {
            b0_tsize = -pat.SetSrc().children[1].int_val;
            saw_b0_sp_dec = true;
          }
          return true;
        }
        if (pat.IsSet() && pat.SetDest().IsMem() && !pat.SetDest().children.empty() &&
            pat.SetSrc().IsReg()) {
          const int sr = pat.SetSrc().RegNo();
          const bool is_callee_saved_reg =
              (sr >= 16 && sr <= 23) || sr == 30 || sr == 31 || (sr >= 52 && sr <= 63);
          if (is_callee_saved_reg) {
            const RtlExpr& a = pat.SetDest().children[0];
            int64_t off = -1;
            if (a.IsReg() && a.RegNo() == 29) {
              off = 0;
            } else if (a.op == RtlOp::kPlus && a.children.size() == 2 && a.children[0].IsReg() &&
                       a.children[0].RegNo() == 29 && a.children[1].IsConstInt()) {
              off = a.children[1].int_val;
            }
            if (off >= 0 && off < b0_tsize) {
              min_prologue_save_off = std::min(min_prologue_save_off, off);
            }
          }
        }
        return true;
      };
      if (isum.is_sequence) {
        if (!inspect_prologue_pat(isum.head_pn.pattern, isum.head) ||
            !inspect_prologue_pat(isum.slot_pn.pattern, isum.slot)) {
          break;
        }
      } else if (!inspect_prologue_pat(isum.top_pn.pattern, isum.single)) {
        break;
      }
    }
    if (b0_tsize > 0) {
      const int64_t derived_limit =
          (min_prologue_save_off < b0_tsize) ? min_prologue_save_off : b0_tsize;
      effective_sp_local_limit = std::max(effective_sp_local_limit, derived_limit);
    }
  }
  out_addr_taken->clear();
  const int64_t scan_limit = std::max<int64_t>(
      effective_sp_local_limit,
      static_cast<int64_t>(cfg.sideband.outgoing_args_size) + cfg.sideband.frame_size_bytes);
  if (profile != nullptr && !is_unoptimized && !cfg.sideband.frame_pointer_needed &&
      scan_limit > 0) {
    auto scan_sp_addr_taken = [&](const auto& self, const RtlExpr& expr) -> void {
      if (expr.op == RtlOp::kMem) {
        return;
      }
      if (expr.op == RtlOp::kPlus && expr.children.size() == 2 && expr.children[0].IsReg() &&
          expr.children[0].RegNo() == 29 && expr.children[1].IsConstInt()) {
        out_addr_taken->insert(expr.children[1].int_val);
        return;
      }
      for (const RtlExpr& ch : expr.children) {
        self(self, ch);
      }
    };
    for (size_t b = 0; b < n_blocks; ++b) {
      for (const InsnSummary& isum : res.block_summaries[b]) {
        if (!isum.valid) continue;
        auto scan_pat = [&](const RtlExpr& pat) {
          if (pat.IsSet() && pat.SetDest().IsReg() && pat.SetDest().RegNo() == 29) {
            return;
          }
          scan_sp_addr_taken(scan_sp_addr_taken, pat);
        };
        if (isum.is_sequence) {
          scan_pat(isum.head_pn.pattern);
          scan_pat(isum.slot_pn.pattern);
        } else {
          scan_pat(isum.top_pn.pattern);
        }
      }
    }
  }
  *out_effective_sp_local_limit = effective_sp_local_limit;
}

// Mirrors `calls.c:1850-1915`, `local-alloc.c:2158`: returns the mask of O32
// GPR argument registers (`$a0..$a3`, regnos 4..7) read inside `pat` ONLY as
// `MEM` address operands (never as non-address values such as store data or
// branch operands), and classifies computed pointer-address defs
// `(set (reg:SI a_r) (plus:SI ...))` that cannot be live-through scalar call
// arguments once dereferenced solely as a memory base pointer.
RegMask ClassifyAddrScratchArgDef(const RtlExpr& pat, bool gp64, bool fp64) {
  if (!pat.IsSet() || !pat.SetDest().IsReg() || IsCallPattern(pat)) {
    return 0;
  }
  const int dst_r = pat.SetDest().RegNo();
  if (dst_r < 4 || dst_r > 7 || pat.SetSrc().op != RtlOp::kPlus) {
    return 0;
  }
  return RegBitMask(dst_r, pat.SetDest().mode, gp64, fp64) & (RegMask{0xF} << 4);
}

RegMask CollectMemAddrOnlyGprArgReads(const RtlExpr& pat, bool gp64, bool fp64) {
  if (IsCallPattern(pat)) {
    return 0;
  }
  RegMask in_mem_addr = 0;
  RegMask outside_mem_addr = 0;
  auto walk = [&](const auto& self, const RtlExpr& e, bool in_mem, bool is_set_dest) -> void {
    if (e.op == RtlOp::kSet && e.children.size() >= 2) {
      if (e.children[0].op == RtlOp::kMem) {
        self(self, e.children[0], true, false);
      } else {
        self(self, e.children[0], in_mem, true);
      }
      for (size_t k = 1; k < e.children.size(); ++k) {
        self(self, e.children[k], in_mem, false);
      }
      return;
    }
    if (e.op == RtlOp::kMem) {
      for (const RtlExpr& ch : e.children) {
        self(self, ch, true, false);
      }
      return;
    }
    if (e.IsReg() && !is_set_dest) {
      const int r = e.RegNo();
      if (r >= 4 && r <= 7) {
        const RegMask bit = RegBitMask(r, e.mode, gp64, fp64) & (RegMask{0xF} << 4);
        if (in_mem) {
          in_mem_addr |= bit;
        } else {
          outside_mem_addr |= bit;
        }
      }
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch, in_mem, is_set_dest);
    }
  };
  walk(walk, pat, false, false);
  return in_mem_addr & ~outside_mem_addr;
}

void RefineCallArgumentFusageAcrossCfg(
    const RtlControlFlowGraph& cfg, const CompilationProfile* profile, bool is_unoptimized,
    const absl::flat_hash_set<std::string>& jt_table_labels,
    const std::vector<std::vector<RegMask>>& caller_saved_arg_masks, DataflowResult* res_ptr) {
  DataflowResult& res = *res_ptr;
  const size_t n_blocks = cfg.rpo_blocks.size();
  // Refine `CALL_INSN` argument register uses (`$4..$7`, `$f12..$f15`) across
  // CFG edges and incoming function parameters:
  // 1) Propagate explicit outgoing argument defs set in a predecessor block or
  //    branch delay slot (e.g., `(insn/s (set a0 ...))` on edge `B1 -> B3`) into
  //    the successor's first call.
  // 2) Propagate incoming parameter registers (`incoming_param_regs`) that reach
  //    a direct `symbol_ref` call unclobbered when another call to the same
  //    callee in the function explicitly passes those argument registers (or
  //    when MIPS O32 positional argument ordering requires the leading arg reg).
  if (n_blocks == 0) {
    return;
  }

  // Apply exact `optabs.c` argument register masks to unannotated compiler
  // `libgcc` runtime calls before CFG propagation so pre-call scratch GPR/FPR
  // writes never pollute `CALL_INSN_FUNCTION_USAGE`.
  for (size_t b = 0; b < n_blocks; ++b) {
    for (InsnSummary& isum : res.block_summaries[b]) {
      if (!isum.valid) continue;
      auto apply_known_libcall = [&](ElemSummary* call_elem, const RtlPatternAndNotes& call_pn) {
        if (call_pn.has_explicit_call_fusage) {
          return;
        }
        const std::optional<RegMask> known_mask = LookupKnownGccLibcallArgMask(
            ExtractCalleeSymbolRef(call_pn.pattern), res.gp64, res.fp64, profile);
        if (!known_mask.has_value()) {
          return;
        }
        ElemSummary direct_only;
        AnalyzePatternAst(call_pn.pattern, res.gp64, res.fp64, &direct_only);
        call_elem->arg_fusage_uses = *known_mask;
        call_elem->uses = direct_only.uses | *known_mask;
      };
      if (isum.is_sequence && isum.is_call_seq) {
        apply_known_libcall(&isum.head, isum.head_pn);
      } else if (!isum.is_sequence && isum.single.is_call) {
        apply_known_libcall(&isum.single, isum.top_pn);
      }
    }
  }

  auto close_o32_arg_prefix = [&](RegMask args, RegMask allowed_valid) -> RegMask {
    RegMask out_args = args;
    const bool has_fp_lead = ((out_args & ((RegMask{1} << 44) | (RegMask{1} << 46))) != 0);
    if ((out_args & (RegMask{1} << 46)) != 0) {
      out_args |= (RegMask{1} << 44) & allowed_valid;
    }
    if ((out_args & (RegMask{1} << 7)) != 0) {
      out_args |= (RegMask{1} << 6) & allowed_valid;
    }
    if (!has_fp_lead) {
      if ((out_args & (RegMask{1} << 6)) != 0) {
        out_args |= (RegMask{1} << 5) & allowed_valid;
      }
      if ((out_args & (RegMask{1} << 5)) != 0) {
        out_args |= (RegMask{1} << 4) & allowed_valid;
      }
    }
    return out_args;
  };

  auto dead_arg_mask_from_notes = [&](const RtlPatternAndNotes& pn) -> RegMask {
    RegMask dead = 0;
    for (const RtlRegNote& note : pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg()) {
        dead |= RegBitMask(note.datum.RegNo(), note.datum.mode, res.gp64, res.fp64) & kArgRegMask;
      }
    }
    return dead;
  };

  // Backward non-call read reachability to identify incoming parameter regs at B0.
  std::vector<RegMask> noncall_arg_in(n_blocks, 0);
  {
    auto step_noncall_bwd = [](const ElemSummary& e, RegMask cur) -> RegMask {
      if (e.is_call) {
        return 0;
      }
      return ((cur & ~e.defs) | (e.uses & kArgRegMask)) & kArgRegMask;
    };
    bool changed_nc = true;
    while (changed_nc) {
      changed_nc = false;
      for (size_t rev = n_blocks; rev > 0; --rev) {
        const size_t b = rev - 1;
        RegMask cur = 0;
        for (size_t sb : cfg.rpo_blocks[b].successors) {
          if (sb < n_blocks) {
            cur |= noncall_arg_in[sb];
          }
        }
        for (auto it = res.block_summaries[b].rbegin(); it != res.block_summaries[b].rend(); ++it) {
          if (!it->valid) continue;
          if (it->is_sequence) {
            if (it->is_call_seq) {
              cur = step_noncall_bwd(it->slot, step_noncall_bwd(it->head, cur));
            } else if (it->is_annulled_target_seq && b + 1 < n_blocks &&
                       std::find(cfg.rpo_blocks[b].successors.begin(),
                                 cfg.rpo_blocks[b].successors.end(),
                                 b + 1) != cfg.rpo_blocks[b].successors.end()) {
              RegMask taken_nc = 0;
              for (size_t sb : cfg.rpo_blocks[b].successors) {
                if (sb != b + 1 && sb < n_blocks) {
                  taken_nc |= noncall_arg_in[sb];
                }
              }
              const RegMask ft_nc = noncall_arg_in[b + 1];
              cur = step_noncall_bwd(it->head, step_noncall_bwd(it->slot, taken_nc) | ft_nc);
            } else {
              cur = step_noncall_bwd(it->head, step_noncall_bwd(it->slot, cur));
            }
          } else {
            cur = step_noncall_bwd(it->single, cur);
          }
        }
        if (cur != noncall_arg_in[b]) {
          noncall_arg_in[b] = cur;
          changed_nc = true;
        }
      }
    }
  }
  const RegMask incoming_param_regs = close_o32_arg_prefix(noncall_arg_in[0], kArgRegMask);

  std::vector<bool> arg_reachable(n_blocks, false);
  {
    std::vector<size_t> wl = {0};
    arg_reachable[0] = true;
    while (!wl.empty()) {
      const size_t u = wl.back();
      wl.pop_back();
      for (size_t v : cfg.rpo_blocks[u].successors) {
        if (v < n_blocks && !arg_reachable[v]) {
          arg_reachable[v] = true;
          wl.push_back(v);
        }
      }
    }
  }

  // Identify non-address-taken `$sp + off` stack slots (`0 <= off < local_limit`)
  // so `reload`-emitted input reloads from local spill slots (and `HI_REG` /
  // `LO_REG` moves from `64`/`65`) into `$4..$7` that are consumed by a
  // subsequent non-call instruction do not pollute `CALL_INSN_FUNCTION_USAGE`.
  int64_t effective_sp_local_limit = 0;
  absl::flat_hash_set<int64_t> addr_taken_sp_offsets;
  ComputeSpLocalLimitAndAddrTakenOffsets(cfg, profile, is_unoptimized, res,
                                         &effective_sp_local_limit, &addr_taken_sp_offsets);

  auto extract_local_sp_disp = [&](const RtlExpr& mem_expr, int64_t* out_off) -> bool {
    if (!mem_expr.IsMem() || mem_expr.flags.volatil || mem_expr.children.empty()) {
      return false;
    }
    const RtlExpr& addr = mem_expr.children[0];
    int64_t off = -1;
    if (addr.IsReg() && addr.RegNo() == 29) {
      off = 0;
    } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsReg() &&
               addr.children[0].RegNo() == 29 && addr.children[1].IsConstInt()) {
      off = addr.children[1].int_val;
    } else {
      return false;
    }
    if (off < 0 || off >= effective_sp_local_limit || addr_taken_sp_offsets.contains(off)) {
      return false;
    }
    *out_off = off;
    return true;
  };

  auto classify_reload_scratch_arg_def =
      [&](const RtlExpr& pat, bool saw_call_in_block,
          const absl::flat_hash_set<uint64_t>& saved_before_call) -> RegMask {
    if (profile == nullptr || is_unoptimized || !pat.IsSet() || !pat.SetDest().IsReg()) {
      return 0;
    }
    const int dst_r = pat.SetDest().RegNo();
    if (dst_r < 4 || dst_r > 7) {
      return 0;
    }
    const RegMask dst_bits =
        RegBitMask(dst_r, pat.SetDest().mode, res.gp64, res.fp64) & (RegMask{0xF} << 4);
    const RtlExpr& src = pat.SetSrc();
    if (src.IsReg() && (src.RegNo() == 64 || src.RegNo() == 65)) {
      return dst_bits;
    }
    if (!cfg.sideband.frame_pointer_needed && effective_sp_local_limit > 0) {
      int64_t off = 0;
      if (extract_local_sp_disp(src, &off)) {
        const uint64_t key = (static_cast<uint64_t>(off) << 8) | static_cast<uint64_t>(dst_r);
        if (!(saw_call_in_block && saved_before_call.contains(key))) {
          return dst_bits;
        }
      }
    }
    return 0;
  };
  const int64_t spill_sp_limit = std::max<int64_t>(
      effective_sp_local_limit,
      static_cast<int64_t>(cfg.sideband.outgoing_args_size) + cfg.sideband.frame_size_bytes);
  const absl::flat_hash_set<int64_t> proven_gpr_spill_offsets =
      (profile != nullptr && !is_unoptimized) ? CollectProvenGprSpillOffsetsForCallFusage(
                                                    cfg, res, spill_sp_limit, addr_taken_sp_offsets)
                                              : absl::flat_hash_set<int64_t>{};
  auto classify_out_spill_scratch_def = [&](const RtlExpr& pat) -> RegMask {
    if (proven_gpr_spill_offsets.empty() || !pat.IsSet() || !pat.SetDest().IsReg() ||
        IsCallPattern(pat)) {
      return 0;
    }
    const int dst_r = pat.SetDest().RegNo();
    const RtlExpr& src = pat.SetSrc();
    if (dst_r < 4 || dst_r > 7 || src.op == RtlOp::kReg || src.op == RtlOp::kMem) {
      return 0;
    }
    return RegBitMask(dst_r, pat.SetDest().mode, res.gp64, res.fp64) & (RegMask{0xF} << 4);
  };
  auto match_proven_gpr_spill_store = [&](const RtlExpr& pat) -> RegMask {
    if (proven_gpr_spill_offsets.empty() || !pat.IsSet() || !pat.SetSrc().IsReg()) {
      return 0;
    }
    const int s_r = pat.SetSrc().RegNo();
    const RtlExpr& dst = pat.SetDest();
    if (s_r < 4 || s_r > 7 || !dst.IsMem() || dst.flags.volatil || dst.children.size() != 1) {
      return 0;
    }
    const RtlExpr& a = dst.children[0];
    int64_t s_off = -1;
    if (a.IsReg() && a.RegNo() == 29) {
      s_off = 0;
    } else if (a.op == RtlOp::kPlus && a.children.size() == 2 && a.children[0].IsReg() &&
               a.children[0].RegNo() == 29 && a.children[1].IsConstInt()) {
      s_off = a.children[1].int_val;
    }
    if (s_off >= 0 && s_off < spill_sp_limit && proven_gpr_spill_offsets.contains(s_off)) {
      return RegBitMask(s_r, pat.SetSrc().mode, res.gp64, res.fp64) & (RegMask{0xF} << 4);
    }
    return 0;
  };
  struct ArgScratchState {
    RegMask in_scratch = 0;
    RegMask out_spill_cand = 0;
    RegMask addr_scratch = 0;
  };

  if (profile != nullptr && !is_unoptimized) {
    for (size_t b = 0; b < n_blocks; ++b) {
      bool saw_call = false;
      absl::flat_hash_set<uint64_t> pending_saves;
      absl::flat_hash_set<uint64_t> active_call_saves;
      RegMask active_scratch = 0;
      RegMask active_out_spill_cand = 0;
      RegMask consumed_scratch = 0;
      auto prune_intra_elem = [&](ElemSummary* e, const RtlPatternAndNotes& pn) {
        if (e->is_call) {
          if (!pn.has_explicit_call_fusage &&
              !LookupKnownGccLibcallArgMask(ExtractCalleeSymbolRef(pn.pattern), res.gp64, res.fp64,
                                            profile)
                   .has_value() &&
              consumed_scratch != 0) {
            const RegMask dead_on_call = dead_arg_mask_from_notes(pn);
            const RegMask drop = consumed_scratch & ~dead_on_call;
            if (drop != 0) {
              ElemSummary direct_only;
              AnalyzePatternAst(pn.pattern, res.gp64, res.fp64, &direct_only);
              e->arg_fusage_uses &= ~drop;
              e->uses = (e->uses & ~drop) | (direct_only.uses & drop) | e->arg_fusage_uses;
            }
          }
          saw_call = true;
          active_call_saves.insert(pending_saves.begin(), pending_saves.end());
          pending_saves.clear();
          active_scratch = 0;
          active_out_spill_cand = 0;
          consumed_scratch = 0;
          return;
        }
        if (pn.pattern.op == RtlOp::kNote) {
          return;
        }
        if (!cfg.sideband.frame_pointer_needed && cfg.sideband.frame_size_bytes > 0 &&
            pn.pattern.IsSet() && pn.pattern.SetSrc().IsReg()) {
          const int s_r = pn.pattern.SetSrc().RegNo();
          int64_t s_off = 0;
          if (s_r >= 4 && s_r <= 7 && extract_local_sp_disp(pn.pattern.SetDest(), &s_off)) {
            pending_saves.insert((static_cast<uint64_t>(s_off) << 8) | static_cast<uint64_t>(s_r));
          }
        }
        const bool is_jt_base_set = PatternSrcContainsAnyLabelRef(pn.pattern, jt_table_labels);
        const RegMask arg_explicit_sets = is_jt_base_set ? RegMask{0} : e->explicit_sets;
        const RegMask read_without_set = (e->uses & kArgRegMask) & ~arg_explicit_sets;
        const RegMask spill_st_reg = match_proven_gpr_spill_store(pn.pattern);
        consumed_scratch &= ~(read_without_set & ~spill_st_reg);
        consumed_scratch |=
            (active_scratch & read_without_set) | (active_out_spill_cand & spill_st_reg);
        active_scratch &= ~(read_without_set | e->defs | arg_explicit_sets);
        active_out_spill_cand &= ~(read_without_set | e->defs | arg_explicit_sets);
        consumed_scratch &= ~(e->defs | arg_explicit_sets);
        active_scratch |= classify_reload_scratch_arg_def(pn.pattern, saw_call, active_call_saves);
        active_out_spill_cand |= classify_out_spill_scratch_def(pn.pattern);
      };
      for (InsnSummary& isum : res.block_summaries[b]) {
        if (!isum.valid) continue;
        if (isum.is_sequence) {
          if (isum.is_call_seq) {
            prune_intra_elem(&isum.slot, isum.slot_pn);
            prune_intra_elem(&isum.head, isum.head_pn);
          } else {
            prune_intra_elem(&isum.head, isum.head_pn);
            prune_intra_elem(&isum.slot, isum.slot_pn);
          }
        } else {
          prune_intra_elem(&isum.single, isum.top_pn);
        }
      }
    }
  }

  auto step_arg_elem = [&](const ElemSummary& e, const RtlPatternAndNotes& pn,
                           bool* saw_call_in_block, absl::flat_hash_set<uint64_t>* pending_saves,
                           absl::flat_hash_set<uint64_t>* active_call_saves, RegMask* must_valid,
                           RegMask* may_explicit, RegMask* may_unread_explicit,
                           ArgScratchState* active_scratch) {
    if (e.is_call) {
      *saw_call_in_block = true;
      active_call_saves->insert(pending_saves->begin(), pending_saves->end());
      pending_saves->clear();
      *must_valid = 0;
      *may_explicit = 0;
      *may_unread_explicit = 0;
      *active_scratch = {};
      return;
    }
    if (pn.pattern.op == RtlOp::kNote) {
      return;
    }
    if (is_unoptimized && (e.defs & ((RegMask{1} << 29) | (RegMask{1} << 30))) != 0) {
      *must_valid = 0;
      *may_explicit = 0;
      *may_unread_explicit = 0;
      *active_scratch = {};
      return;
    }
    if (!is_unoptimized && !cfg.sideband.frame_pointer_needed &&
        cfg.sideband.frame_size_bytes > 0 && pn.pattern.IsSet() && pn.pattern.SetSrc().IsReg()) {
      const int s_r = pn.pattern.SetSrc().RegNo();
      int64_t s_off = 0;
      if (s_r >= 4 && s_r <= 7 && extract_local_sp_disp(pn.pattern.SetDest(), &s_off)) {
        pending_saves->insert((static_cast<uint64_t>(s_off) << 8) | static_cast<uint64_t>(s_r));
      }
    }
    const bool is_jt_base_set = PatternSrcContainsAnyLabelRef(pn.pattern, jt_table_labels);
    const RegMask arg_explicit_sets = is_jt_base_set ? RegMask{0} : e.explicit_sets;
    const RegMask dead_notes = dead_arg_mask_from_notes(pn) & ~arg_explicit_sets;
    RegMask clobbered = (e.defs & ~arg_explicit_sets) | dead_notes;
    if (!res.fp64) {
      for (int even_fpr : {44, 46}) {
        if ((arg_explicit_sets & (RegMask{1} << even_fpr)) != 0 &&
            (arg_explicit_sets & (RegMask{1} << (even_fpr + 1))) == 0) {
          clobbered |= (RegMask{1} << (even_fpr + 1));
        }
      }
    }
    *must_valid = ((*must_valid & ~clobbered) | (arg_explicit_sets & kArgRegMask)) & kArgRegMask;
    if (profile != nullptr) {
      const RegMask read_without_set = (e.uses & kArgRegMask) & ~arg_explicit_sets;
      *may_unread_explicit &= ~read_without_set;
      if (is_unoptimized) {
        *must_valid &= ~read_without_set;
        *may_explicit &= ~read_without_set;
      } else {
        const RegMask spill_st_reg = match_proven_gpr_spill_store(pn.pattern);
        const RegMask addr_only_read =
            CollectMemAddrOnlyGprArgReads(pn.pattern, res.gp64, res.fp64) & read_without_set;
        const RegMask consumed_scratch = (active_scratch->in_scratch & read_without_set) |
                                         (active_scratch->out_spill_cand & spill_st_reg);
        const RegMask consumed_addr_scratch = active_scratch->addr_scratch & addr_only_read;
        *must_valid &= ~consumed_scratch;
        *may_explicit &= ~(consumed_scratch | consumed_addr_scratch);
        active_scratch->in_scratch &= ~read_without_set;
        active_scratch->out_spill_cand &= ~read_without_set;
        active_scratch->addr_scratch &= ~read_without_set;
      }
    }
    active_scratch->in_scratch &= ~(clobbered | arg_explicit_sets);
    active_scratch->out_spill_cand &= ~(clobbered | arg_explicit_sets);
    active_scratch->addr_scratch &= ~(clobbered | arg_explicit_sets);
    active_scratch->in_scratch |=
        classify_reload_scratch_arg_def(pn.pattern, *saw_call_in_block, *active_call_saves);
    active_scratch->out_spill_cand |= classify_out_spill_scratch_def(pn.pattern);
    active_scratch->addr_scratch |= ClassifyAddrScratchArgDef(pn.pattern, res.gp64, res.fp64);
    *may_explicit =
        ((*may_explicit & ~clobbered) | (arg_explicit_sets & kArgRegMask)) & kArgRegMask;
    *may_unread_explicit =
        ((*may_unread_explicit & ~clobbered) | (arg_explicit_sets & kArgRegMask)) & kArgRegMask;
  };

  // At `-O0` (`function.c:assign_parms`, `calls.c:expand_call`, `stupid.c`),
  // incoming register parameters are homed in the entry block, `obey_regdecls`
  // never allocates call-crossing variables to caller-saved argument registers,
  // and outgoing register arguments are materialized intra-block after prologue
  // `$sp`/`$fp` setup and after any preceding `CALL_INSN`.
  std::vector<RegMask> must_valid_in(n_blocks, is_unoptimized ? RegMask{0} : kArgRegMask);
  std::vector<RegMask> may_explicit_in(n_blocks, 0);
  std::vector<RegMask> may_unread_in(n_blocks, 0);
  std::vector<bool> has_arg_init(n_blocks, is_unoptimized);
  RegMask b0_forwarded_incoming_explicit = 0;
  if (!is_unoptimized && profile != nullptr && n_blocks > 0 && incoming_param_regs != 0) {
    bool any_reachable_call = false;
    RegMask noncall_reads_outside_b0 = 0;
    RegMask all_reads_outside_b0 = 0;
    for (size_t b = 0; b < n_blocks; ++b) {
      if (!arg_reachable[b]) continue;
      for (const InsnSummary& isum : res.block_summaries[b]) {
        if (!isum.valid) continue;
        auto scan_e = [&](const ElemSummary& e) {
          if (e.is_call) {
            any_reachable_call = true;
          } else if (b > 0) {
            noncall_reads_outside_b0 |= (e.uses & kArgRegMask);
            all_reads_outside_b0 |= e.uses;
          }
        };
        if (isum.is_sequence) {
          scan_e(isum.head);
          scan_e(isum.slot);
        } else {
          scan_e(isum.single);
        }
      }
    }
    if (any_reachable_call) {
      struct B0Step {
        const ElemSummary* esum = nullptr;
        const RtlPatternAndNotes* pn = nullptr;
      };
      std::vector<B0Step> b0_steps;
      for (const InsnSummary& isum : res.block_summaries[0]) {
        if (!isum.valid) continue;
        if (isum.is_sequence) {
          if (isum.is_call_seq) {
            b0_steps.push_back({&isum.slot, &isum.slot_pn});
            b0_steps.push_back({&isum.head, &isum.head_pn});
          } else {
            b0_steps.push_back({&isum.head, &isum.head_pn});
            if (!isum.seq_slot_wrap.flags.in_struct) {
              b0_steps.push_back({&isum.slot, &isum.slot_pn});
            }
          }
        } else {
          b0_steps.push_back({&isum.single, &isum.top_pn});
        }
      }
      for (int arg_r = 4; arg_r <= 7; ++arg_r) {
        const RegMask arg_bit = (RegMask{1} << arg_r);
        if ((incoming_param_regs & arg_bit) == 0 || (noncall_reads_outside_b0 & arg_bit) != 0) {
          continue;
        }
        bool modified_in_b0 = false;
        size_t first_call_s = b0_steps.size();
        size_t last_read_s = b0_steps.size();
        for (size_t s = 0; s < b0_steps.size(); ++s) {
          if (b0_steps[s].esum->is_call) {
            if (first_call_s == b0_steps.size()) first_call_s = s;
            continue;
          }
          if ((b0_steps[s].esum->defs & arg_bit) != 0) {
            modified_in_b0 = true;
            break;
          }
          if ((b0_steps[s].esum->uses & arg_bit) != 0) {
            last_read_s = s;
          }
        }
        if (modified_in_b0 || last_read_s >= first_call_s) {
          continue;
        }
        const RtlExpr& pat = b0_steps[last_read_s].pn->pattern;
        if (!pat.IsSet() || !pat.SetDest().IsReg() || pat.SetDest().IsArgPointerReg() ||
            pat.SetDest().flags.integrated) {
          continue;
        }
        const int dst_r = pat.SetDest().RegNo();
        const RtlMachineMode dm = pat.SetDest().mode;
        const bool word_or_subword_int = dm == RtlMachineMode::kQi || dm == RtlMachineMode::kHi ||
                                         dm == RtlMachineMode::kSi ||
                                         (res.gp64 && dm == RtlMachineMode::kDi);
        if (!word_or_subword_int || dst_r == arg_r || dst_r < 3 ||
            !(dst_r <= 15 || dst_r == 24 || dst_r == 25)) {
          continue;
        }
        const RtlExpr& rhs = pat.SetSrc();
        if (rhs.op == RtlOp::kMult || rhs.children.empty() || !rhs.children[0].IsReg() ||
            rhs.children[0].RegNo() != arg_r || rhs.children[0].mode != dm) {
          continue;
        }
        const RegMask dst_bit = (RegMask{1} << dst_r);
        if ((incoming_param_regs & dst_bit) != 0) {
          continue;
        }
        bool dst_touched_before = false;
        for (size_t s = 0; s < last_read_s; ++s) {
          if (((b0_steps[s].esum->defs | b0_steps[s].esum->uses) & dst_bit) != 0) {
            dst_touched_before = true;
            break;
          }
        }
        bool dst_used_in_call_or_copy = false;
        bool dst_redefined_in_b0 = false;
        for (size_t s = last_read_s + 1; s < b0_steps.size(); ++s) {
          if (b0_steps[s].esum->is_call && (b0_steps[s].esum->uses & dst_bit) != 0) {
            dst_used_in_call_or_copy = true;
            break;
          }
          const RtlExpr& sp = b0_steps[s].pn->pattern;
          if (sp.IsSet() && sp.SetDest().IsReg() && sp.SetSrc().IsReg() &&
              sp.SetSrc().RegNo() == dst_r) {
            dst_used_in_call_or_copy = true;
            break;
          }
          if ((b0_steps[s].esum->defs & dst_bit) != 0) {
            dst_redefined_in_b0 = true;
            break;
          }
        }
        if (!dst_touched_before && !dst_used_in_call_or_copy &&
            (dst_redefined_in_b0 || (all_reads_outside_b0 & dst_bit) == 0)) {
          b0_forwarded_incoming_explicit |= arg_bit;
        }
      }
    }
    must_valid_in[0] = incoming_param_regs;
    may_explicit_in[0] = b0_forwarded_incoming_explicit;
    has_arg_init[0] = true;
  }

  bool changed_arg_fwd = !is_unoptimized;
  while (changed_arg_fwd) {
    changed_arg_fwd = false;
    std::vector<RegMask> next_valid_in(n_blocks, kArgRegMask);
    std::vector<RegMask> next_explicit_in(n_blocks, 0);
    std::vector<RegMask> next_unread_in(n_blocks, 0);
    std::vector<bool> next_init(n_blocks, false);
    next_valid_in[0] = incoming_param_regs;
    next_explicit_in[0] = b0_forwarded_incoming_explicit;
    next_init[0] = true;

    for (size_t b = 0; b < n_blocks; ++b) {
      if (!arg_reachable[b] || !has_arg_init[b]) {
        continue;
      }
      bool saw_call_in_block = false;
      absl::flat_hash_set<uint64_t> pending_saves;
      absl::flat_hash_set<uint64_t> active_call_saves;
      RegMask cur_valid = must_valid_in[b];
      RegMask cur_explicit = may_explicit_in[b];
      RegMask cur_unread = may_unread_in[b];
      ArgScratchState cur_scratch;
      RegMask ft_valid = 0;
      RegMask ft_explicit = 0;
      RegMask ft_unread = 0;
      RegMask taken_valid = 0;
      RegMask taken_explicit = 0;
      RegMask taken_unread = 0;
      bool split_edges = false;
      const size_t n_insns = res.block_summaries[b].size();
      const bool is_cond = (cfg.rpo_blocks[b].successors.size() >= 2);

      for (size_t i = 0; i < n_insns; ++i) {
        const InsnSummary& isum = res.block_summaries[b][i];
        if (!isum.valid) continue;
        if (isum.is_sequence && !isum.is_call_seq && is_cond && i + 1 == n_insns &&
            isum.is_annulled_target_seq) {
          step_arg_elem(isum.head, isum.head_pn, &saw_call_in_block, &pending_saves,
                        &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
          ft_valid = cur_valid;
          ft_explicit = cur_explicit;
          ft_unread = cur_unread;
          taken_valid = cur_valid;
          taken_explicit = cur_explicit;
          taken_unread = cur_unread;
          bool taken_saw_call = saw_call_in_block;
          absl::flat_hash_set<uint64_t> taken_pending_saves = pending_saves;
          absl::flat_hash_set<uint64_t> taken_active_saves = active_call_saves;
          ArgScratchState taken_scratch = cur_scratch;
          step_arg_elem(isum.slot, isum.slot_pn, &taken_saw_call, &taken_pending_saves,
                        &taken_active_saves, &taken_valid, &taken_explicit, &taken_unread,
                        &taken_scratch);
          split_edges = true;
          break;
        }
        if (isum.is_sequence) {
          if (isum.is_call_seq) {
            step_arg_elem(isum.slot, isum.slot_pn, &saw_call_in_block, &pending_saves,
                          &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
            step_arg_elem(isum.head, isum.head_pn, &saw_call_in_block, &pending_saves,
                          &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
          } else {
            step_arg_elem(isum.head, isum.head_pn, &saw_call_in_block, &pending_saves,
                          &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
            step_arg_elem(isum.slot, isum.slot_pn, &saw_call_in_block, &pending_saves,
                          &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
          }
        } else {
          step_arg_elem(isum.single, isum.top_pn, &saw_call_in_block, &pending_saves,
                        &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
        }
      }

      for (size_t sb : cfg.rpo_blocks[b].successors) {
        if (sb >= n_blocks || !arg_reachable[sb]) continue;
        const RegMask edge_valid =
            split_edges ? ((sb == b + 1) ? ft_valid : taken_valid) : cur_valid;
        const RegMask edge_explicit =
            split_edges ? ((sb == b + 1) ? ft_explicit : taken_explicit) : cur_explicit;
        const RegMask edge_unread =
            split_edges ? ((sb == b + 1) ? ft_unread : taken_unread) : cur_unread;
        next_valid_in[sb] &= edge_valid;
        next_explicit_in[sb] |= edge_explicit;
        next_unread_in[sb] |= edge_unread;
        next_init[sb] = true;
      }
    }

    for (size_t b = 0; b < n_blocks; ++b) {
      if (!arg_reachable[b]) continue;
      if (next_init[b] != has_arg_init[b] || next_valid_in[b] != must_valid_in[b] ||
          next_explicit_in[b] != may_explicit_in[b] || next_unread_in[b] != may_unread_in[b]) {
        has_arg_init[b] = next_init[b];
        must_valid_in[b] = next_valid_in[b];
        may_explicit_in[b] = next_explicit_in[b];
        may_unread_in[b] = next_unread_in[b];
        changed_arg_fwd = true;
      }
    }
  }

  absl::flat_hash_map<std::string, RegMask> callee_observed_args;
  absl::flat_hash_map<std::string, RegMask> callee_valid_intersection;
  absl::flat_hash_map<std::string, RegMask> callee_unread_args;
  absl::flat_hash_map<std::string, int> callee_call_count;
  for (int pass_idx = 0; pass_idx < 2; ++pass_idx) {
    for (size_t b = 0; b < n_blocks; ++b) {
      if (!arg_reachable[b] || !has_arg_init[b]) {
        continue;
      }
      bool saw_call_in_block = false;
      absl::flat_hash_set<uint64_t> pending_saves;
      absl::flat_hash_set<uint64_t> active_call_saves;
      RegMask cur_valid = must_valid_in[b];
      RegMask cur_explicit = may_explicit_in[b];
      RegMask cur_unread = may_unread_in[b];
      ArgScratchState cur_scratch;
      for (size_t i = 0; i < res.block_summaries[b].size(); ++i) {
        InsnSummary& isum = res.block_summaries[b][i];
        if (!isum.valid) continue;
        auto process_call = [&](ElemSummary* call_elem, const RtlPatternAndNotes& call_pn) {
          const std::string sym = ExtractCalleeSymbolRef(call_pn.pattern);
          if (call_pn.has_explicit_call_fusage ||
              LookupKnownGccLibcallArgMask(sym, res.gp64, res.fp64, profile).has_value()) {
            step_arg_elem(*call_elem, call_pn, &saw_call_in_block, &pending_saves,
                          &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
            return;
          }
          const RegMask cs_mask = caller_saved_arg_masks[b][i];
          const RegMask call_valid = cur_valid & ~cs_mask;
          const RegMask base_explicit =
              (call_elem->arg_fusage_uses | (call_valid & cur_explicit)) & ~cs_mask;
          const RegMask closed_explicit = close_o32_arg_prefix(base_explicit, call_valid);
          if (pass_idx == 0) {
            if (!is_unoptimized && !sym.empty()) {
              callee_observed_args[sym] |= closed_explicit;
              ++callee_call_count[sym];
              callee_unread_args[sym] |= (call_valid & cur_unread);
              auto [vit, inserted] = callee_valid_intersection.emplace(sym, call_valid);
              if (!inserted) {
                vit->second &= call_valid;
              }
            }
          } else {
            RegMask inferred = closed_explicit;
            if (!is_unoptimized && !sym.empty()) {
              if (auto it = callee_observed_args.find(sym); it != callee_observed_args.end()) {
                inferred |= (call_valid & it->second);
              }
            }
            RegMask candidate = (call_elem->arg_fusage_uses | inferred) & ~cs_mask;
            // In KMC GCC 2.7.2 (`calls.c:expand_call`, `config/mips/mips.c:function_arg`,
            // `local-alloc.c:2158`), an unused call-clobbered argument register (`$a0..$a3`
            // / `$f12..$f15`) may hold a block-local temporary that is already consumed by
            // a non-call instruction before the call (`(cur_unread & bit) == 0`). When the
            // same direct `symbol_ref` callee is invoked at multiple call sites, another
            // call site does not even define that register (`callee_valid_intersection`),
            // and no call site has an unread argument write to it (`callee_unread_args`),
            // drop the already-consumed local scratch unless required by O32 prefix closure.
            if (!is_unoptimized && !sym.empty() && callee_call_count[sym] >= 2) {
              const RegMask dead_on_call = dead_arg_mask_from_notes(call_pn);
              const RegMask drop_cand = candidate & ~cur_unread & ~dead_on_call &
                                        ~callee_valid_intersection[sym] & ~callee_unread_args[sym];
              if (drop_cand != 0) {
                const RegMask pruned = candidate & ~drop_cand;
                candidate = close_o32_arg_prefix(pruned, call_valid) & candidate;
              }
            }
            // In KMC GCC 2.7.2 (`mips.c:function_arg`), `$f12` (reg 44) and
            // `$a0` (reg 4) both correspond to `cum->arg_words == 0` and are
            // mutually exclusive on every call. If pre-call scratch writes put
            // both into `candidate`, keep the unread outgoing argument write
            // (`cur_unread`) and drop the already-consumed scratch register.
            if ((candidate & (RegMask{1} << 44)) != 0 && (candidate & (RegMask{1} << 4)) != 0) {
              const bool f12_unread = (cur_unread & (RegMask{1} << 44)) != 0;
              const bool r4_unread = (cur_unread & (RegMask{1} << 4)) != 0;
              if (f12_unread && !r4_unread) {
                candidate &= ~(RegMask{1} << 4);
                if (!res.gp64 && !res.fp64 && (candidate & (RegMask{1} << 45)) != 0 &&
                    (cur_unread & (RegMask{1} << 5)) == 0) {
                  candidate &= ~(RegMask{1} << 5);
                }
              } else if (r4_unread && !f12_unread) {
                candidate &= ~(RegMask{0xF} << 44);
              }
            }
            ElemSummary direct_only;
            AnalyzePatternAst(call_pn.pattern, res.gp64, res.fp64, &direct_only);
            call_elem->arg_fusage_uses = candidate;
            call_elem->uses = direct_only.uses | candidate;
          }
          step_arg_elem(*call_elem, call_pn, &saw_call_in_block, &pending_saves, &active_call_saves,
                        &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
        };

        if (isum.is_sequence) {
          if (isum.is_call_seq) {
            step_arg_elem(isum.slot, isum.slot_pn, &saw_call_in_block, &pending_saves,
                          &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
            process_call(&isum.head, isum.head_pn);
          } else {
            step_arg_elem(isum.head, isum.head_pn, &saw_call_in_block, &pending_saves,
                          &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
            step_arg_elem(isum.slot, isum.slot_pn, &saw_call_in_block, &pending_saves,
                          &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
          }
        } else if (isum.single.is_call) {
          process_call(&isum.single, isum.top_pn);
        } else {
          step_arg_elem(isum.single, isum.top_pn, &saw_call_in_block, &pending_saves,
                        &active_call_saves, &cur_valid, &cur_explicit, &cur_unread, &cur_scratch);
        }
      }
    }
  }

  if (!is_unoptimized) {
    InferEntryFirstCallDeletedNoopArgMoves(profile, &res);
  }
}

std::vector<RegMask> ComputeMayRetDefExit(const RtlControlFlowGraph& cfg, const DataflowResult& res,
                                          RegMask ret_cand_mask) {
  const size_t n_blocks = cfg.rpo_blocks.size();
  std::vector<RegMask> may_ret_def_in(n_blocks, 0);
  std::vector<RegMask> may_ret_def_exit(n_blocks, 0);
  if ((res.live_at_exit & ret_cand_mask) == 0 || n_blocks == 0) {
    return may_ret_def_exit;
  }
  bool changed_may = true;
  while (changed_may) {
    changed_may = false;
    for (size_t b = 0; b < n_blocks; ++b) {
      RegMask cur = may_ret_def_in[b];
      RegMask taken_may = 0;
      RegMask ft_may = 0;
      bool split_edges = false;
      const size_t n_insns = res.block_summaries[b].size();
      const bool is_cond = (cfg.rpo_blocks[b].successors.size() >= 2) ||
                           (res.can_exit[b] && !cfg.rpo_blocks[b].successors.empty()) ||
                           (!res.block_summaries[b].empty() &&
                            res.block_summaries[b].back().is_annulled_target_seq &&
                            !cfg.rpo_blocks[b].successors.empty());
      for (size_t i = 0; i < n_insns; ++i) {
        const InsnSummary& isum = res.block_summaries[b][i];
        if (!isum.valid) continue;
        if (isum.is_sequence && !isum.is_call_seq && is_cond && i + 1 == n_insns &&
            isum.is_annulled_target_seq) {
          ft_may = cur | (isum.head.explicit_sets & ret_cand_mask);
          taken_may = ft_may | (isum.slot.explicit_sets & ret_cand_mask);
          cur = ft_may;
          split_edges = true;
          break;
        }
        if (isum.is_sequence) {
          cur |= ((isum.head.explicit_sets | isum.slot.explicit_sets) & ret_cand_mask);
        } else {
          cur |= (isum.single.explicit_sets & ret_cand_mask);
        }
      }
      RegMask ex_may = cur;
      if (res.uses_epilogue_delay_at_exit[b]) {
        ex_may |= (res.epilogue_delay.explicit_sets & ret_cand_mask);
      }
      if (ex_may != may_ret_def_exit[b]) {
        may_ret_def_exit[b] = ex_may;
        changed_may = true;
      }
      const bool both_may_to_ft =
          split_edges && !cfg.rpo_blocks[b].successors.empty() &&
          std::all_of(cfg.rpo_blocks[b].successors.begin(), cfg.rpo_blocks[b].successors.end(),
                      [b](size_t s) { return s == b + 1; });
      for (size_t sb : cfg.rpo_blocks[b].successors) {
        if (sb >= n_blocks) continue;
        const RegMask edge_may = both_may_to_ft
                                     ? (ft_may | taken_may)
                                     : (split_edges ? ((sb == b + 1) ? ft_may : taken_may) : cur);
        if ((may_ret_def_in[sb] | edge_may) != may_ret_def_in[sb]) {
          may_ret_def_in[sb] |= edge_may;
          changed_may = true;
        }
      }
    }
  }
  return may_ret_def_exit;
}

bool InferExitV0LivenessFromBlockReads(const RtlControlFlowGraph& cfg, const DataflowResult& res,
                                       const std::vector<bool>& reachable) {
  const size_t n_blocks = cfg.rpo_blocks.size();
  for (size_t b = 0; b < n_blocks; ++b) {
    if (!reachable[b] || !res.can_exit[b]) {
      continue;
    }
    bool v0_read_in_succ = false;
    RegMask succ_reads_all = 0;
    std::vector<bool> vis(n_blocks, false);
    std::vector<size_t> q(cfg.rpo_blocks[b].successors.begin(), cfg.rpo_blocks[b].successors.end());
    while (!q.empty()) {
      const size_t sb = q.back();
      q.pop_back();
      if (sb >= n_blocks || vis[sb]) continue;
      vis[sb] = true;
      for (const InsnSummary& ss : res.block_summaries[sb]) {
        if (!ss.valid) continue;
        const RegMask u = ss.is_sequence ? (ss.head.uses | ss.slot.uses) : ss.single.uses;
        succ_reads_all |= u;
        if ((u & (RegMask{1} << 2)) != 0) {
          v0_read_in_succ = true;
        }
      }
      for (size_t nxt : cfg.rpo_blocks[sb].successors) {
        if (nxt < n_blocks && !vis[nxt]) q.push_back(nxt);
      }
    }
    if (v0_read_in_succ) {
      continue;
    }
    struct ExitStep {
      const ElemSummary* esum = nullptr;
      const RtlExpr* pat = nullptr;
    };
    std::vector<ExitStep> b_steps;
    for (const InsnSummary& isum : res.block_summaries[b]) {
      if (!isum.valid) continue;
      if (isum.is_sequence) {
        if (isum.is_call_seq) {
          b_steps.push_back({&isum.slot, &isum.slot_pn.pattern});
          b_steps.push_back({&isum.head, &isum.head_pn.pattern});
        } else {
          if (!isum.seq_slot_wrap.flags.in_struct) {
            b_steps.push_back({&isum.slot, &isum.slot_pn.pattern});
          }
          b_steps.push_back({&isum.head, &isum.head_pn.pattern});
        }
      } else {
        b_steps.push_back({&isum.single, &isum.top_pn.pattern});
      }
    }
    RtlExpr ep_pat;
    if (res.uses_epilogue_delay_at_exit[b] && cfg.sideband.epilogue_delay_pattern.has_value()) {
      if (auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern); ep.ok()) {
        ep_pat = std::move(ep->pattern);
        b_steps.push_back({&res.epilogue_delay, &ep_pat});
      }
    }
    size_t last_v0_s = b_steps.size();
    size_t def_v0_s = b_steps.size();
    int v0_defs_in_b = 0;
    for (size_t s = 0; s < b_steps.size(); ++s) {
      if ((b_steps[s].esum->defs & (RegMask{1} << 2)) != 0) {
        if (!b_steps[s].esum->is_call) {
          ++v0_defs_in_b;
          def_v0_s = s;
        }
        last_v0_s = s;
      } else if ((b_steps[s].esum->uses & (RegMask{1} << 2)) != 0) {
        last_v0_s = s;
      }
    }
    if (last_v0_s >= b_steps.size() || (b_steps[last_v0_s].esum->uses & (RegMask{1} << 2)) == 0 ||
        (b_steps[last_v0_s].esum->defs & (RegMask{1} << 2)) != 0) {
      continue;
    }
    bool call_after = false;
    for (size_t s = last_v0_s + 1; s < b_steps.size(); ++s) {
      if (b_steps[s].esum->is_call) {
        call_after = true;
        break;
      }
    }
    if (call_after) {
      continue;
    }
    const RtlExpr& pat = *b_steps[last_v0_s].pat;
    if (n_blocks == 1 && b == 0 && v0_defs_in_b == 1 && def_v0_s < last_v0_s) {
      const RtlExpr& def_pat = *b_steps[def_v0_s].pat;
      const RtlMachineMode ptr_m = res.gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi;
      if (def_pat.IsSet() && def_pat.SetDest().IsHardReg() && def_pat.SetDest().RegNo() == 2 &&
          def_pat.SetDest().mode == ptr_m && def_pat.SetSrc().IsHardReg() &&
          def_pat.SetSrc().RegNo() == 4 && def_pat.SetSrc().mode == ptr_m) {
        bool a0_read_before_redef = false;
        for (size_t s = def_v0_s + 1; s < b_steps.size(); ++s) {
          if ((b_steps[s].esum->uses & (RegMask{1} << 4)) != 0) {
            a0_read_before_redef = true;
            break;
          }
          if ((b_steps[s].esum->defs & (RegMask{1} << 4)) != 0) {
            break;
          }
        }
        const bool stores_via_v0 =
            (pat.IsSet() && pat.SetDest().op == RtlOp::kMem && pat.SetDest().ContainsReg(2)) ||
            (pat.op == RtlOp::kParallel && !pat.children.empty() && pat.children[0].IsSet() &&
             pat.children[0].SetDest().op == RtlOp::kMem &&
             pat.children[0].SetDest().ContainsReg(2));
        if (!a0_read_before_redef && stores_via_v0) {
          return true;
        }
      }
    }
    if (!pat.IsSet() || !pat.SetDest().IsReg() || pat.SetDest().IsArgPointerReg() ||
        pat.SetDest().flags.integrated) {
      continue;
    }
    const int dst_r = pat.SetDest().RegNo();
    const RtlMachineMode dm = pat.SetDest().mode;
    const bool word_or_subword_int = dm == RtlMachineMode::kQi || dm == RtlMachineMode::kHi ||
                                     dm == RtlMachineMode::kSi ||
                                     (res.gp64 && dm == RtlMachineMode::kDi);
    if (!word_or_subword_int || dst_r < 3 || !(dst_r <= 15 || dst_r == 24 || dst_r == 25) ||
        (succ_reads_all & (RegMask{1} << dst_r)) != 0) {
      continue;
    }
    bool dst_copied_to_reg = false;
    for (size_t s = last_v0_s + 1; s < b_steps.size(); ++s) {
      const RtlExpr& sp = *b_steps[s].pat;
      if (sp.IsSet() && sp.SetDest().IsReg() && sp.SetSrc().IsReg() &&
          sp.SetSrc().RegNo() == dst_r) {
        dst_copied_to_reg = true;
        break;
      }
      if ((b_steps[s].esum->defs & (RegMask{1} << dst_r)) != 0) {
        break;
      }
    }
    if (dst_copied_to_reg) {
      continue;
    }
    const bool witness_a = (dst_r == 3 && (b_steps[last_v0_s].esum->uses & (RegMask{1} << 3)) == 0);
    const RtlExpr& rhs = pat.SetSrc();
    bool v0_born_pure_in_b = false;
    if (def_v0_s < last_v0_s) {
      for (size_t s = def_v0_s + 1; s > 0; --s) {
        const size_t idx = s - 1;
        if ((b_steps[idx].esum->defs & (RegMask{1} << 2)) != 0) {
          if ((b_steps[idx].esum->uses & (RegMask{1} << 2)) == 0 && !b_steps[idx].esum->is_call) {
            v0_born_pure_in_b = true;
            break;
          }
        } else if ((b_steps[idx].esum->uses & (RegMask{1} << 2)) != 0) {
          break;
        }
      }
    }
    const bool witness_b =
        (v0_born_pure_in_b && rhs.op != RtlOp::kMult && !rhs.children.empty() &&
         rhs.children[0].IsReg() && rhs.children[0].RegNo() == 2 && rhs.children[0].mode == dm);
    if (witness_a || witness_b) {
      return true;
    }
  }
  return false;
}

DataflowResult RunHardRegDataflow(const RtlControlFlowGraph& cfg, const CompilationProfile* profile,
                                  bool dbr_post_simple_markers = false) {
  DataflowResult res;
  DetectRegisterWidthFlags(cfg, profile, &res.gp64, &res.fp64);
  const bool is_unoptimized = IsUnoptimizedProfile(profile);
  const absl::flat_hash_set<std::string> jt_table_labels = CollectJumpTableLabels(cfg);
  const size_t n_blocks = cfg.rpo_blocks.size();
  res.can_exit.assign(n_blocks, false);
  res.uses_epilogue_delay_at_exit.assign(n_blocks, false);
  res.live_in.assign(n_blocks, 0);
  res.live_out.assign(n_blocks, 0);
  res.block_summaries.resize(n_blocks);

  RegMask any_integrated_ret = 0;
  if (cfg.sideband.epilogue_delay_pattern.has_value() &&
      !cfg.sideband.epilogue_delay_pattern->empty()) {
    auto ep_pn = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
    if (ep_pn.ok()) {
      RegMask dummy_args = 0;
      res.epilogue_delay = SummarizeSingleElem(*ep_pn, /*force_call=*/false, res.gp64, res.fp64,
                                               is_unoptimized, jt_table_labels, &dummy_args);
      res.has_epilogue_delay = true;
      any_integrated_ret |= res.epilogue_delay.integrated_ret_regs;
    }
  }

  for (size_t b = 0; b < n_blocks; ++b) {
    const RtlBasicBlock& blk = cfg.rpo_blocks[b];
    res.can_exit[b] = blk.successors.empty();
    bool ends_with_ret_insn = false;
    if (!blk.instructions.empty()) {
      auto last_p = ParsePatternAndNotes(blk.instructions.back());
      if (last_p.ok()) {
        if (IsReturnJumpPattern(last_p->pattern)) {
          ends_with_ret_insn = true;
          res.can_exit[b] = true;
        } else if (b + 1 == n_blocks && blk.successors.size() == 1 &&
                   IsConditionalJumpPattern(last_p->pattern)) {
          // Final RPO block ending in a conditional branch whose fallthrough
          // block was empty after epilogue stripping (e.g., loop latch exiting
          // into the epilogue on fallthrough).
          res.can_exit[b] = true;
        }
      }
    }
    res.uses_epilogue_delay_at_exit[b] =
        res.can_exit[b] && !ends_with_ret_insn && res.has_epilogue_delay;

    RegMask pending_arg_defs = 0;
    res.block_summaries[b].resize(blk.instructions.size());
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      InsnSummary& isum = res.block_summaries[b][i];
      auto pn = ParsePatternAndNotes(blk.instructions[i]);
      if (!pn.ok()) {
        continue;
      }
      isum.valid = true;
      isum.top_pn = std::move(*pn);
      const RtlInsn* src_rinsn = (i < blk.rtl_insns.size()) ? &blk.rtl_insns[i] : nullptr;
      if (isum.top_pn.pattern.op == RtlOp::kSequence && isum.top_pn.pattern.children.size() == 2) {
        auto h_pn = RtlPatternAndNotes::FromSequenceSubInsn(isum.top_pn.pattern.children[0]);
        auto s_pn = RtlPatternAndNotes::FromSequenceSubInsn(isum.top_pn.pattern.children[1]);
        if (h_pn.ok() && s_pn.ok()) {
          isum.is_sequence = true;
          isum.seq_head_wrap = isum.top_pn.pattern.children[0];
          isum.seq_slot_wrap = isum.top_pn.pattern.children[1];
          isum.head_pn = std::move(*h_pn);
          isum.slot_pn = std::move(*s_pn);
          isum.is_call_seq = (isum.seq_head_wrap.op == RtlOp::kCallInsn);
          if (isum.is_call_seq && !isum.head_pn.has_explicit_call_fusage && src_rinsn != nullptr &&
              src_rinsn->has_explicit_call_fusage) {
            isum.head_pn.has_explicit_call_fusage = true;
            isum.head_pn.call_fusage = src_rinsn->call_fusage;
          }
          isum.is_annulled_target_seq =
              (isum.seq_head_wrap.op == RtlOp::kJumpInsn && isum.seq_head_wrap.flags.unchanging &&
               isum.seq_slot_wrap.flags.in_struct && i + 1 == blk.instructions.size());
          if (isum.is_call_seq) {
            isum.slot = SummarizeSingleElem(isum.slot_pn, /*force_call=*/false, res.gp64, res.fp64,
                                            is_unoptimized, jt_table_labels, &pending_arg_defs);
            isum.head = SummarizeSingleElem(isum.head_pn, /*force_call=*/true, res.gp64, res.fp64,
                                            is_unoptimized, jt_table_labels, &pending_arg_defs);
          } else {
            isum.head = SummarizeSingleElem(isum.head_pn, /*force_call=*/false, res.gp64, res.fp64,
                                            is_unoptimized, jt_table_labels, &pending_arg_defs);
            isum.slot = SummarizeSingleElem(isum.slot_pn, /*force_call=*/false, res.gp64, res.fp64,
                                            is_unoptimized, jt_table_labels, &pending_arg_defs);
          }
          any_integrated_ret |= isum.head.integrated_ret_regs | isum.slot.integrated_ret_regs;
          continue;
        }
      }
      if (!isum.top_pn.has_explicit_call_fusage && src_rinsn != nullptr &&
          src_rinsn->has_explicit_call_fusage) {
        isum.top_pn.has_explicit_call_fusage = true;
        isum.top_pn.call_fusage = src_rinsn->call_fusage;
      }
      isum.single = SummarizeSingleElem(isum.top_pn, /*force_call=*/false, res.gp64, res.fp64,
                                        is_unoptimized, jt_table_labels, &pending_arg_defs);
      any_integrated_ret |= isum.single.integrated_ret_regs;
    }
  }

  const std::vector<std::vector<RegMask>> caller_saved_arg_masks =
      ComputeCallerSavedArgMasks(res, profile);
  for (size_t b = 0; b < n_blocks; ++b) {
    for (size_t i = 0; i < res.block_summaries[b].size(); ++i) {
      const RegMask cs_mask = caller_saved_arg_masks[b][i];
      if (cs_mask == 0) continue;
      InsnSummary& isum = res.block_summaries[b][i];
      auto strip_cs_from_call = [&](ElemSummary* call_elem, const RtlPatternAndNotes& call_pn) {
        ElemSummary direct_only;
        AnalyzePatternAst(call_pn.pattern, res.gp64, res.fp64, &direct_only);
        call_elem->arg_fusage_uses &= ~cs_mask;
        call_elem->uses = (call_elem->uses & ~cs_mask) | (direct_only.uses & cs_mask);
      };
      if (isum.is_sequence && isum.is_call_seq) {
        strip_cs_from_call(&isum.head, isum.head_pn);
      } else if (!isum.is_sequence && isum.single.is_call) {
        strip_cs_from_call(&isum.single, isum.top_pn);
      }
    }
  }

  RefineCallArgumentFusageAcrossCfg(cfg, profile, is_unoptimized, jt_table_labels,
                                    caller_saved_arg_masks, &res);

  res.live_at_exit = (RegMask{1} << 29) | (RegMask{1} << 31) | any_integrated_ret;
  if (cfg.sideband.frame_pointer_needed) {
    res.live_at_exit |= (RegMask{1} << 30);
  }

  // Only infer return registers from dataflow in Stage 0 (`profile != nullptr`),
  // where raw assembly does not carry `REG_FUNCTION_VALUE_P` (`reg/i`). In
  // post-reload RTL stages (`profile == nullptr`), `any_integrated_ret` already
  // matches `GccContext::MaterializeFromCfg`'s `current_function_return_rtx`.
  if (profile != nullptr && any_integrated_ret == 0 && n_blocks > 0) {
    std::vector<bool> reachable(n_blocks, false);
    std::vector<size_t> worklist = {0};
    reachable[0] = true;
    while (!worklist.empty()) {
      const size_t u = worklist.back();
      worklist.pop_back();
      for (size_t v : cfg.rpo_blocks[u].successors) {
        if (v < n_blocks && !reachable[v]) {
          reachable[v] = true;
          worklist.push_back(v);
        }
      }
    }

    std::vector<RegMask> live_no_exit_in(n_blocks, 0);
    {
      auto has_ft_edge = [&](size_t b) -> bool {
        if (b + 1 >= n_blocks) return false;
        const auto& succs = cfg.rpo_blocks[b].successors;
        return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
      };
      bool changed_no_exit = true;
      while (changed_no_exit) {
        changed_no_exit = false;
        for (size_t rev = n_blocks; rev > 0; --rev) {
          const size_t b = rev - 1;
          RegMask up = 0;
          for (size_t sb : cfg.rpo_blocks[b].successors) {
            if (sb < n_blocks) {
              up |= live_no_exit_in[sb];
            }
          }
          for (auto it = res.block_summaries[b].rbegin(); it != res.block_summaries[b].rend();
               ++it) {
            if (!it->valid) continue;
            if (it->is_sequence) {
              if (it->is_call_seq) {
                up = (up & ~it->head.defs) | it->head.uses;
                up = (up & ~it->slot.defs) | it->slot.uses;
              } else if (it->is_annulled_target_seq && has_ft_edge(b)) {
                RegMask taken_up = 0;
                for (size_t sb : cfg.rpo_blocks[b].successors) {
                  if (sb != b + 1 && sb < n_blocks) {
                    taken_up |= live_no_exit_in[sb];
                  }
                }
                const RegMask before_slot =
                    ((taken_up & ~it->slot.defs) | it->slot.uses) | live_no_exit_in[b + 1];
                up = (before_slot & ~it->head.defs) | it->head.uses;
              } else {
                up = (up & ~it->slot.defs) | it->slot.uses;
                up = (up & ~it->head.defs) | it->head.uses;
              }
            } else {
              up = (up & ~it->single.defs) | it->single.uses;
            }
          }
          up &= ~RegMask{1};
          if (up != live_no_exit_in[b]) {
            live_no_exit_in[b] = up;
            changed_no_exit = true;
          }
        }
      }
    }

    auto step_def = [](const ElemSummary& e, RegMask state) -> RegMask {
      state &= ~(e.defs & ~e.explicit_sets);
      state |= (e.explicit_sets & kRetCandMask);
      return state;
    };
    auto step_unread_any = [](const ElemSummary& e, RegMask state) -> RegMask {
      state &= ~(e.uses | e.defs);
      state |= (e.explicit_sets & kRetCandMask);
      return state;
    };
    auto step_unread_noncall = [](const ElemSummary& e, RegMask state) -> RegMask {
      state &= ~(e.uses | e.defs);
      if (!e.is_call) {
        state |= (e.explicit_sets & kRetCandMask);
      }
      return state;
    };

    std::vector<RegMask> must_def_in(n_blocks, kRetCandMask);
    std::vector<RegMask> must_unread_in(n_blocks, kRetCandMask);
    std::vector<RegMask> may_noncall_in(n_blocks, 0);
    must_def_in[0] = 0;
    must_unread_in[0] = 0;

    std::vector<RegMask> must_def_exit(n_blocks, kRetCandMask);
    std::vector<RegMask> must_unread_exit(n_blocks, kRetCandMask);
    std::vector<RegMask> may_noncall_exit(n_blocks, 0);

    std::vector<bool> has_pred_init(n_blocks, false);
    has_pred_init[0] = true;

    bool changed_fwd = true;
    while (changed_fwd) {
      changed_fwd = false;
      std::vector<RegMask> next_def_in(n_blocks, kRetCandMask);
      std::vector<RegMask> next_unread_in(n_blocks, kRetCandMask);
      std::vector<RegMask> next_noncall_in = may_noncall_in;
      std::vector<bool> next_has_pred(n_blocks, false);
      next_def_in[0] = 0;
      next_unread_in[0] = 0;
      next_has_pred[0] = true;

      for (size_t b = 0; b < n_blocks; ++b) {
        if (!reachable[b] || !has_pred_init[b]) {
          continue;
        }
        RegMask cur_def = must_def_in[b];
        RegMask cur_unread = must_unread_in[b];
        RegMask cur_noncall = may_noncall_in[b];
        const size_t n_insns = res.block_summaries[b].size();
        const bool is_cond_branch_blk =
            (cfg.rpo_blocks[b].successors.size() >= 2) ||
            (res.can_exit[b] && !cfg.rpo_blocks[b].successors.empty()) ||
            (!res.block_summaries[b].empty() &&
             res.block_summaries[b].back().is_annulled_target_seq &&
             !cfg.rpo_blocks[b].successors.empty());

        RegMask taken_def = 0;
        RegMask taken_unread = 0;
        RegMask taken_noncall = 0;
        RegMask ft_def = 0;
        RegMask ft_unread = 0;
        RegMask ft_noncall = 0;
        bool split_branch_edges = false;

        for (size_t i = 0; i < n_insns; ++i) {
          const InsnSummary& isum = res.block_summaries[b][i];
          if (!isum.valid) continue;
          if (isum.is_sequence && !isum.is_call_seq && is_cond_branch_blk && i + 1 == n_insns) {
            const RegMask h_def = step_def(isum.head, cur_def);
            const RegMask h_unread = step_unread_any(isum.head, cur_unread);
            const RegMask h_noncall = step_unread_noncall(isum.head, cur_noncall);
            if (isum.is_annulled_target_seq) {
              ft_def = h_def;
              ft_unread = h_unread;
              ft_noncall = h_noncall;
              taken_def = step_def(isum.slot, h_def);
              taken_unread = step_unread_any(isum.slot, h_unread);
              taken_noncall = step_unread_noncall(isum.slot, h_noncall);
            } else {
              const RegMask raw_def = step_def(isum.slot, h_def);
              const RegMask raw_unread = step_unread_any(isum.slot, h_unread);
              const RegMask raw_noncall = step_unread_noncall(isum.slot, h_noncall);
              RegMask taken_live_no_exit = 0;
              for (size_t sb : cfg.rpo_blocks[b].successors) {
                if (sb != b + 1 && sb < n_blocks) {
                  taken_live_no_exit |= live_no_exit_in[sb];
                }
              }
              const RegMask ft_live_no_exit = (b + 1 < n_blocks) ? live_no_exit_in[b + 1] : 0;
              const RegMask stolen_ft =
                  isum.slot.explicit_sets & ft_live_no_exit & ~taken_live_no_exit;
              const RegMask stolen_tgt =
                  isum.slot.explicit_sets & isum.head.uses & taken_live_no_exit & ~ft_live_no_exit;
              ft_def = (raw_def & ~stolen_tgt) | (h_def & ~isum.slot.defs);
              ft_unread = raw_unread & ~stolen_tgt;
              ft_noncall = raw_noncall & ~stolen_tgt;
              // On a non-annulled conditional branch `SEQUENCE`, the delay-slot
              // instruction executes unconditionally on both the fallthrough
              // and taken edges (`reorg.c`), so `taken_def` includes any
              // register defined by `isum.slot` even if `ft_live_no_exit` also
              // reads it on the fallthrough path (e.g., a loop/goto countdown
              // setting `$v0` in the branch delay slot before branching to exit).
              // `taken_unread` and `taken_noncall` stay masked by `~stolen_ft`
              // so fallthrough-only writes do not false-positive on `void`
              // functions.
              taken_def = raw_def;
              taken_unread = raw_unread & ~stolen_ft;
              taken_noncall = raw_noncall & ~stolen_ft;
            }
            cur_def = ft_def;
            cur_unread = ft_unread;
            cur_noncall = ft_noncall;
            split_branch_edges = true;
            break;
          }
          if (isum.is_sequence) {
            if (isum.is_call_seq) {
              cur_def = step_def(isum.head, step_def(isum.slot, cur_def));
              cur_unread = step_unread_any(isum.head, step_unread_any(isum.slot, cur_unread));
              cur_noncall =
                  step_unread_noncall(isum.head, step_unread_noncall(isum.slot, cur_noncall));
            } else {
              cur_def = step_def(isum.slot, step_def(isum.head, cur_def));
              cur_unread = step_unread_any(isum.slot, step_unread_any(isum.head, cur_unread));
              cur_noncall =
                  step_unread_noncall(isum.slot, step_unread_noncall(isum.head, cur_noncall));
            }
          } else {
            cur_def = step_def(isum.single, cur_def);
            cur_unread = step_unread_any(isum.single, cur_unread);
            cur_noncall = step_unread_noncall(isum.single, cur_noncall);
          }
        }

        must_def_exit[b] = cur_def;
        must_unread_exit[b] = cur_unread;
        may_noncall_exit[b] = cur_noncall;

        const bool both_edges_to_ft =
            split_branch_edges && !cfg.rpo_blocks[b].successors.empty() &&
            std::all_of(cfg.rpo_blocks[b].successors.begin(), cfg.rpo_blocks[b].successors.end(),
                        [b](size_t s) { return s == b + 1; });
        for (size_t sb : cfg.rpo_blocks[b].successors) {
          if (sb >= n_blocks) continue;
          const bool is_ft = (sb == b + 1);
          RegMask e_def = both_edges_to_ft
                              ? (ft_def & taken_def)
                              : (split_branch_edges ? (is_ft ? ft_def : taken_def) : cur_def);
          const RegMask e_unread =
              both_edges_to_ft
                  ? (ft_unread & taken_unread)
                  : (split_branch_edges ? (is_ft ? ft_unread : taken_unread) : cur_unread);
          RegMask e_noncall =
              both_edges_to_ft
                  ? (ft_noncall | taken_noncall)
                  : (split_branch_edges ? (is_ft ? ft_noncall : taken_noncall) : cur_noncall);
          if (cfg.rpo_blocks[b].successors.size() >= 2 &&
              (live_no_exit_in[sb] & (RegMask{1} << 3)) == 0) {
            bool sibling_uses_r3_only = false;
            for (size_t other_sb : cfg.rpo_blocks[b].successors) {
              if (other_sb != sb && other_sb < n_blocks &&
                  (live_no_exit_in[other_sb] & (RegMask{1} << 3)) != 0 &&
                  (live_no_exit_in[other_sb] & (RegMask{1} << 2)) == 0) {
                sibling_uses_r3_only = true;
                break;
              }
            }
            if (sibling_uses_r3_only) {
              e_def &= ~(RegMask{1} << 3);
              e_noncall &= ~(RegMask{1} << 3);
            }
          }
          if (sb != 0) {
            next_def_in[sb] &= e_def;
            next_unread_in[sb] &= e_unread;
            next_has_pred[sb] = true;
          }
          next_noncall_in[sb] |= e_noncall;
        }
      }

      for (size_t b = 0; b < n_blocks; ++b) {
        if (!reachable[b]) continue;
        if (next_has_pred[b] != has_pred_init[b] || next_def_in[b] != must_def_in[b] ||
            next_unread_in[b] != must_unread_in[b] || next_noncall_in[b] != may_noncall_in[b]) {
          has_pred_init[b] = next_has_pred[b];
          must_def_in[b] = next_def_in[b];
          must_unread_in[b] = next_unread_in[b];
          may_noncall_in[b] = next_noncall_in[b];
          changed_fwd = true;
        }
      }
    }

    RegMask all_exits_def = kRetCandMask;
    RegMask all_exits_unread = kRetCandMask;
    RegMask any_exit_noncall = 0;
    bool saw_reachable_exit = false;
    for (size_t b = 0; b < n_blocks; ++b) {
      if (!reachable[b] || !res.can_exit[b]) {
        continue;
      }
      saw_reachable_exit = true;
      RegMask ex_def = must_def_exit[b];
      RegMask ex_unread = must_unread_exit[b];
      RegMask ex_noncall = may_noncall_exit[b];
      if (res.uses_epilogue_delay_at_exit[b]) {
        ex_def = step_def(res.epilogue_delay, ex_def);
        ex_unread = step_unread_any(res.epilogue_delay, ex_unread);
        ex_noncall = step_unread_noncall(res.epilogue_delay, ex_noncall);
      }
      all_exits_def &= ex_def;
      all_exits_unread &= ex_unread;
      any_exit_noncall |= ex_noncall;
    }

    if (saw_reachable_exit) {
      RegMask inferred = ((all_exits_def & any_exit_noncall) | all_exits_unread) & kRetCandMask;
      if ((inferred & (RegMask{1} << 32)) != 0) {
        const bool prefer_f0 =
            ((all_exits_unread & (RegMask{1} << 2)) == 0 ||
             (all_exits_unread & (RegMask{1} << 32)) != 0) &&
            (res.epilogue_delay.explicit_sets & ((RegMask{1} << 2) | (RegMask{1} << 3))) == 0;
        if (prefer_f0) {
          inferred &= ~((RegMask{1} << 2) | (RegMask{1} << 3));
        } else {
          inferred &= ~((RegMask{1} << 32) | (RegMask{1} << 33));
        }
      } else {
        inferred &= ~(RegMask{1} << 33);
      }
      if ((inferred & (RegMask{1} << 2)) == 0) {
        inferred &= ~(RegMask{1} << 3);
      }
      if (!res.gp64 && (inferred & (RegMask{1} << 3)) != 0) {
        bool has_di_v0 = false;
        bool loop_derefs_and_bumps_v1 = false;
        bool exit_sets_narrow_v0_without_v1 = false;
        auto check_di_v0 = [&](const RtlExpr& pat) {
          pat.Visit([&](const RtlExpr& n) {
            if (n.IsReg() && !n.IsArgPointerReg() && n.RegNo() == 2 &&
                n.mode == RtlMachineMode::kDi) {
              has_di_v0 = true;
            }
          });
        };
        auto expr_derefs_v1 = [](const RtlExpr& pat) {
          bool hit = false;
          pat.Visit([&](const RtlExpr& n) {
            if (n.op == RtlOp::kMem) {
              n.Visit([&](const RtlExpr& ch) {
                if (ch.IsReg() && !ch.IsArgPointerReg() && ch.RegNo() == 3) {
                  hit = true;
                }
              });
            }
          });
          return hit;
        };
        auto expr_narrow_v1_feeds_v0 = [](const RtlExpr& root) {
          bool hit = false;
          root.Visit([&](const RtlExpr& n) {
            if (n.IsSet() && n.SetDest().IsReg() && !n.SetDest().IsArgPointerReg() &&
                n.SetDest().RegNo() == 2 && n.SetDest().mode != RtlMachineMode::kDi) {
              n.SetSrc().Visit([&](const RtlExpr& ch) {
                if (ch.IsReg() && !ch.IsArgPointerReg() && ch.RegNo() == 3 &&
                    ch.mode != RtlMachineMode::kDi) {
                  hit = true;
                }
              });
            }
          });
          return hit;
        };
        bool narrow_v1_feeds_v0 = false;
        for (size_t b = 0; b < n_blocks; ++b) {
          if (!reachable[b]) continue;
          bool has_backedge = false;
          for (size_t succ : cfg.rpo_blocks[b].successors) {
            if (succ <= b) has_backedge = true;
          }
          bool b_defs_v1 = false;
          bool b_derefs_v1 = false;
          RegMask b_defs = 0;
          for (const InsnSummary& isum : res.block_summaries[b]) {
            if (!isum.valid) continue;
            if (isum.is_sequence) {
              check_di_v0(isum.head_pn.pattern);
              check_di_v0(isum.slot_pn.pattern);
              b_derefs_v1 = b_derefs_v1 || expr_derefs_v1(isum.head_pn.pattern) ||
                            expr_derefs_v1(isum.slot_pn.pattern);
              narrow_v1_feeds_v0 = narrow_v1_feeds_v0 ||
                                   expr_narrow_v1_feeds_v0(isum.head_pn.pattern) ||
                                   expr_narrow_v1_feeds_v0(isum.slot_pn.pattern);
              b_defs |= isum.head.defs | isum.slot.defs;
            } else {
              check_di_v0(isum.top_pn.pattern);
              b_derefs_v1 = b_derefs_v1 || expr_derefs_v1(isum.top_pn.pattern);
              narrow_v1_feeds_v0 =
                  narrow_v1_feeds_v0 || expr_narrow_v1_feeds_v0(isum.top_pn.pattern);
              b_defs |= isum.single.defs;
            }
          }
          b_defs_v1 = (b_defs & (RegMask{1} << 3)) != 0;
          if (has_backedge && b_defs_v1 && b_derefs_v1) {
            loop_derefs_and_bumps_v1 = true;
          }
          if (res.can_exit[b]) {
            RegMask ex_b_defs = b_defs;
            if (res.uses_epilogue_delay_at_exit[b]) {
              ex_b_defs |= res.epilogue_delay.defs;
            }
            if ((ex_b_defs & (RegMask{1} << 2)) != 0 && (ex_b_defs & (RegMask{1} << 3)) == 0) {
              exit_sets_narrow_v0_without_v1 = true;
            }
          }
        }
        if (cfg.sideband.epilogue_delay_pattern.has_value()) {
          if (auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern); ep.ok()) {
            check_di_v0(ep->pattern);
            narrow_v1_feeds_v0 = narrow_v1_feeds_v0 || expr_narrow_v1_feeds_v0(ep->pattern);
          }
        }
        if (!has_di_v0 && (loop_derefs_and_bumps_v1 || narrow_v1_feeds_v0) &&
            exit_sets_narrow_v0_without_v1) {
          inferred &= ~(RegMask{1} << 3);
        }
      }
      if (!is_unoptimized && (res.live_at_exit & kRetCandMask) == 0 &&
          (inferred & kRetCandMask) == 0 && (all_exits_def & (RegMask{1} << 2)) != 0 &&
          InferExitV0LivenessFromBlockReads(cfg, res, reachable)) {
        inferred |= (RegMask{1} << 2);
      }
      res.live_at_exit |= inferred;
    }
  }

  const std::vector<RegMask> may_ret_def_exit = ComputeMayRetDefExit(cfg, res, kRetCandMask);

  auto exit_live_for_block = [&](size_t b) -> RegMask {
    if (!res.can_exit[b]) {
      return 0;
    }
    const RegMask base_exit_live = (res.live_at_exit & ~kRetCandMask) |
                                   (res.live_at_exit & kRetCandMask & may_ret_def_exit[b]);
    if (res.uses_epilogue_delay_at_exit[b]) {
      return (base_exit_live & ~res.epilogue_delay.defs) | res.epilogue_delay.uses;
    }
    return base_exit_live;
  };

  auto has_fallthrough_edge = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg.rpo_blocks[b].successors;
    return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
  };

  auto taken_also_goes_to_ft = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg.rpo_blocks[b].successors;
    return !succs.empty() &&
           std::all_of(succs.begin(), succs.end(), [b](size_t s) { return s == b + 1; });
  };

  auto backward_transfer = [&](const InsnSummary& isum, size_t b, RegMask after_live) -> RegMask {
    if (!isum.valid) {
      return after_live;
    }
    if (isum.is_sequence) {
      if (isum.is_call_seq) {
        const RegMask before_call = (after_live & ~isum.head.defs) | isum.head.uses;
        return (before_call & ~isum.slot.defs) | isum.slot.uses;
      }
      if (isum.is_annulled_target_seq && has_fallthrough_edge(b)) {
        RegMask taken_live = exit_live_for_block(b);
        for (size_t sb : cfg.rpo_blocks[b].successors) {
          if ((sb != b + 1 || taken_also_goes_to_ft(b)) && sb < n_blocks) {
            taken_live |= res.live_in[sb];
          }
        }
        const RegMask ft_live = res.live_in[b + 1];
        const RegMask before_slot = ((taken_live & ~isum.slot.defs) | isum.slot.uses) | ft_live;
        return (before_slot & ~isum.head.defs) | isum.head.uses;
      }
      const RegMask before_slot = (after_live & ~isum.slot.defs) | isum.slot.uses;
      return (before_slot & ~isum.head.defs) | isum.head.uses;
    }
    return (after_live & ~isum.single.defs) | isum.single.uses;
  };

  bool changed_bwd = true;
  while (changed_bwd) {
    changed_bwd = false;
    for (size_t rev = n_blocks; rev > 0; --rev) {
      const size_t b = rev - 1;
      RegMask out_mask = exit_live_for_block(b);
      for (size_t sb : cfg.rpo_blocks[b].successors) {
        if (sb < n_blocks) {
          out_mask |= res.live_in[sb];
        }
      }
      res.live_out[b] = out_mask;
      RegMask cur = out_mask;
      for (size_t rev_i = res.block_summaries[b].size(); rev_i > 0; --rev_i) {
        const size_t i = rev_i - 1;
        const InsnSummary& isum = res.block_summaries[b][i];
        if (isum.valid && !isum.is_sequence && isum.top_pn.pattern.op == RtlOp::kUse &&
            i + 1 < res.block_summaries[b].size() && res.block_summaries[b][i + 1].valid &&
            res.block_summaries[b][i + 1].is_sequence &&
            IsReturnJumpPattern(res.block_summaries[b][i + 1].top_pn.pattern)) {
          cur = (cur & ~isum.single.defs) |
                (isum.single.uses & ~res.block_summaries[b][i + 1].slot.defs);
          continue;
        }
        cur = backward_transfer(isum, b, cur);
      }
      if (dbr_post_simple_markers && cfg.rpo_blocks[b].successors.size() >= 2 &&
          res.block_summaries[b].size() >= 2) {
        const InsnSummary* first_active = nullptr;
        const InsnSummary* last_active = nullptr;
        for (const InsnSummary& s : res.block_summaries[b]) {
          if (!s.valid) {
            continue;
          }
          const bool is_pseudo = !s.is_sequence && s.single.is_note_or_pseudo;
          if (!is_pseudo) {
            if (first_active == nullptr) {
              first_active = &s;
            }
            last_active = &s;
          }
        }
        if (first_active != nullptr && last_active != nullptr && first_active != last_active &&
            last_active->is_sequence && !last_active->is_call_seq &&
            !last_active->is_annulled_target_seq && !last_active->seq_slot_wrap.flags.in_struct &&
            !first_active->is_sequence &&
            (first_active->single.explicit_sets & last_active->head.uses) != 0 &&
            (first_active->single.defs & last_active->slot.explicit_sets) == 0) {
          cur |= last_active->slot.explicit_sets;
        }
      }
      cur &= ~RegMask{1};  // `$0` is hardwired zero.
      if (cur != res.live_in[b]) {
        res.live_in[b] = cur;
        changed_bwd = true;
      }
    }
  }

  return res;
}

void WriteLiveInToSideband(const DataflowResult& df, RtlControlFlowGraph* cfg) {
  cfg->sideband.basic_block_live_at_start.clear();
  for (size_t b = 0; b < df.live_in.size(); ++b) {
    const RegMask mask = df.live_in[b];
    if (mask == 0) {
      continue;
    }
    auto& dst_set = cfg->sideband.basic_block_live_at_start[static_cast<int>(b)];
    for (int r = 1; r < kMipsFirstPseudoRegister; ++r) {
      if ((mask & (RegMask{1} << r)) != 0) {
        dst_set.insert(r);
      }
    }
  }
}

bool MarkExitReachingReturnSetsInExpr(RtlExpr* pat, RegMask needed_ret, bool gp64, bool fp64) {
  if (needed_ret == 0) {
    return false;
  }
  bool changed = false;
  auto mark_one_set = [&](RtlExpr* elem) {
    if (!elem->IsSet()) {
      return;
    }
    RtlExpr* dst = &elem->MutableSetDest();
    RtlMachineMode dst_mode = dst->mode;
    int word_off = 0;
    if (dst->IsSubreg() && !dst->children.empty()) {
      word_off = static_cast<int>(dst->int_val);
      dst = &dst->children[0];
    }
    if (dst->IsReg()) {
      const int base_regno = dst->RegNo();
      const int regno = (base_regno >= 0 && base_regno < kMipsFirstPseudoRegister)
                            ? (base_regno + word_off)
                            : base_regno;
      const RegMask m = RegBitMask(regno, dst_mode, gp64, fp64);
      if ((m & needed_ret) != 0 && !dst->flags.integrated) {
        dst->flags.integrated = true;
        changed = true;
      }
    }
  };
  if (pat->IsParallel()) {
    for (RtlExpr& child : pat->children) {
      mark_one_set(&child);
    }
  } else {
    mark_one_set(pat);
  }
  return changed;
}

bool HasRegNote(const std::vector<RtlRegNote>& notes, RtlMachineMode kind, int regno) {
  for (const RtlRegNote& n : notes) {
    if (n.kind == kind && n.datum.IsReg() && n.datum.RegNo() == regno) {
      return true;
    }
  }
  return false;
}

RtlExpr MakeGccHardReg(RtlMachineMode mode, int regno, RtlFlags flags = {},
                       std::string_view raw_mode = "") {
  RtlExpr reg = RtlExpr::MakeReg(mode, regno, flags);
  if (!raw_mode.empty()) {
    reg.raw_mode = std::string(raw_mode);
  }
  if (regno == 30) {
    reg.str_val = "$fp";
  }
  return reg;
}

std::string FormatInsnWithoutCallFusage(const RtlPatternAndNotes& pn) {
  return FormatGccPrintRtlPatternAndNotes(pn.pattern, pn.reg_notes, pn.bracketed_notes,
                                          /*call_fusage=*/{});
}

size_t CountConstZeroRegDeadNotes(const std::vector<RtlRegNote>& notes) {
  size_t count = 0;
  for (const RtlRegNote& n : notes) {
    if (n.kind == RtlMachineMode::kRegDead && n.datum.IsConstInt() && n.datum.int_val == 0) {
      ++count;
    }
  }
  return count;
}

bool AttachNotesToElem(RtlPatternAndNotes* pn, const ElemSummary& esum, RegMask live_after,
                       RegMask fixed_mask, bool gp64, bool fp64, bool in_delay_slot = false,
                       RegMask call_arg_need_after = 0, bool force_reverse_clobbers = false) {
  if (esum.is_call || esum.is_note_or_pseudo) {
    return false;
  }
  bool changed = false;
  const RegMask dead_mask = esum.uses & ~esum.defs & ~live_after & ~fixed_mask;
  const RegMask unused_mask = esum.defs & ~live_after & ~fixed_mask;
  auto is_two_word_use_partially_overwritten_dead = [&](int r) -> bool {
    if ((esum.uses & ~fixed_mask & (RegMask{1} << r)) == 0) {
      return false;
    }
    const RtlMachineMode mode = esum.use_modes[r];
    const int nregs = HardRegNregs(r, mode, gp64, fp64);
    if (nregs != 2 || r + 1 >= kMipsFirstPseudoRegister) {
      return false;
    }
    return (esum.defs & (RegMask{1} << r)) != 0 && (dead_mask & (RegMask{1} << (r + 1))) != 0;
  };
  auto all_words_dead = [&](int r, RtlMachineMode mode) -> bool {
    const int nregs = HardRegNregs(r, mode, gp64, fp64);
    for (int k = 0; k < nregs && r + k < kMipsFirstPseudoRegister; ++k) {
      if ((dead_mask & (RegMask{1} << (r + k))) == 0) {
        return false;
      }
    }
    return true;
  };
  if (in_delay_slot) {
    // In `reorg.c:emit_delay_sequence` (`reorg.c:915-920`), every `REG_DEAD` note on a
    // `SEQUENCE` delay-slot instruction has its datum overwritten with `const0_rtx`
    // (`(const_int 0)`).
    size_t expected_dead_count = 0;
    for (int r = 1; r < kMipsFirstPseudoRegister;) {
      if ((dead_mask & (RegMask{1} << r)) != 0) {
        ++expected_dead_count;
        const RtlMachineMode mode = esum.use_modes[r];
        r += all_words_dead(r, mode) ? HardRegNregs(r, mode, gp64, fp64) : 1;
      } else if (is_two_word_use_partially_overwritten_dead(r)) {
        ++expected_dead_count;
        r += 2;
      } else {
        ++r;
      }
    }
    size_t existing_zero_dead = CountConstZeroRegDeadNotes(pn->reg_notes);
    while (existing_zero_dead < expected_dead_count) {
      pn->reg_notes.push_back(RtlRegNote{
          .kind = RtlMachineMode::kRegDead,
          .list_op = RtlOp::kExprList,
          .datum = RtlExpr::MakeConstInt(0),
      });
      ++existing_zero_dead;
      changed = true;
    }
  } else {
    for (int r = 1; r < kMipsFirstPseudoRegister;) {
      if ((dead_mask & (RegMask{1} << r)) != 0) {
        const RtlMachineMode mode = esum.use_modes[r];
        const bool full_dead = all_words_dead(r, mode);
        // Mirror `flow.c:2529-2553` / `sched.c:2888-2906`: emit the full wide-mode
        // `REG_DEAD` note only when every word of `x` is dead and unwritten; otherwise
        // emit single-word `reg_raw_mode` partial `REG_DEAD` notes for each dead word.
        const RtlMachineMode note_mode =
            full_dead ? mode
                      : ((r < 32) ? (gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi)
                                  : (fp64 ? RtlMachineMode::kDf : RtlMachineMode::kSf));
        const std::string& raw_mode = full_dead ? esum.use_raw_modes[r] : std::string();
        if (!HasRegNote(pn->reg_notes, RtlMachineMode::kRegDead, r)) {
          pn->reg_notes.push_back(RtlRegNote{
              .kind = RtlMachineMode::kRegDead,
              .list_op = RtlOp::kExprList,
              .datum = MakeGccHardReg(note_mode, r, {}, raw_mode),
          });
          changed = true;
        }
        r += full_dead ? HardRegNregs(r, mode, gp64, fp64) : 1;
      } else if (is_two_word_use_partially_overwritten_dead(r)) {
        const RtlMachineMode mode = esum.use_modes[r];
        if (!HasRegNote(pn->reg_notes, RtlMachineMode::kRegDead, r)) {
          pn->reg_notes.push_back(RtlRegNote{
              .kind = RtlMachineMode::kRegDead,
              .list_op = RtlOp::kExprList,
              .datum = MakeGccHardReg(mode, r, {}, esum.use_raw_modes[r]),
          });
          changed = true;
        }
        r += HardRegNregs(r, mode, gp64, fp64);
      } else {
        ++r;
      }
    }
  }
  RegMask emitted_unused = 0;
  auto emit_unused_for_reg = [&](int r) -> int {
    if (r <= 0 || r >= kMipsFirstPseudoRegister ||
        ((unused_mask & ~emitted_unused) & (RegMask{1} << r)) == 0) {
      return 1;
    }
    const RtlMachineMode mode = esum.def_modes[r];
    const int nregs = HardRegNregs(r, mode, gp64, fp64);
    bool all_unused = true;
    for (int k = 1; k < nregs && r + k < kMipsFirstPseudoRegister; ++k) {
      if ((unused_mask & (RegMask{1} << (r + k))) == 0) {
        all_unused = false;
        break;
      }
    }
    const RtlMachineMode note_mode =
        all_unused ? mode : ((gp64 && r < 32) ? RtlMachineMode::kDi : RtlMachineMode::kSi);
    const int span = all_unused ? nregs : 1;
    for (int k = 0; k < span && r + k < kMipsFirstPseudoRegister; ++k) {
      emitted_unused |= (RegMask{1} << (r + k));
    }
    if (!HasRegNote(pn->reg_notes, RtlMachineMode::kRegUnused, r)) {
      pn->reg_notes.push_back(RtlRegNote{
          .kind = RtlMachineMode::kRegUnused,
          .list_op = RtlOp::kExprList,
          .datum = MakeGccHardReg(note_mode, r, {}, all_unused ? esum.def_raw_modes[r] : ""),
      });
      changed = true;
    }
    return span;
  };

  if (pn->pattern.IsParallel()) {
    bool has_pinned_low_set_dest = false;
    bool has_truncate = false;
    pn->pattern.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kTruncate) {
        has_truncate = true;
      }
    });
    std::vector<int> clobber_regs;
    for (const RtlExpr& elem : pn->pattern.children) {
      if (elem.IsSet() && elem.SetDest().IsReg() && !elem.SetDest().IsArgPointerReg()) {
        const int sr = elem.SetDest().RegNo();
        emit_unused_for_reg(sr);
        if ((sr >= 0 && sr < 64 &&
             (elem.SetDest().flags.integrated || elem.SetDest().flags.frame_related || sr == 1 ||
              sr == 28 || sr == 29 || ((call_arg_need_after & (RegMask{1} << sr)) != 0))) ||
            (sr == 65 && ((call_arg_need_after & (RegMask{1} << 65)) != 0))) {
          has_pinned_low_set_dest = true;
        }
      } else if (elem.op == RtlOp::kClobber && elem.children.size() == 1 &&
                 elem.children[0].IsReg() && !elem.children[0].IsArgPointerReg()) {
        const int cr = elem.children[0].RegNo();
        if (cr > 0 && cr < kMipsFirstPseudoRegister && cr != 31) {
          clobber_regs.push_back(cr);
        }
      }
    }
    const bool reverse_clobbers =
        clobber_regs.size() >= 2 &&
        (has_pinned_low_set_dest || has_truncate || force_reverse_clobbers);
    if (reverse_clobbers) {
      for (auto it = clobber_regs.rbegin(); it != clobber_regs.rend(); ++it) {
        emit_unused_for_reg(*it);
      }
    } else {
      for (int cr : clobber_regs) {
        emit_unused_for_reg(cr);
      }
    }
  }

  for (int r = 1; r < kMipsFirstPseudoRegister;) {
    if (((unused_mask & ~emitted_unused) & (RegMask{1} << r)) != 0) {
      r += emit_unused_for_reg(r);
    } else {
      ++r;
    }
  }
  return changed;
}

void ApplyMissingCallFusageFromDataflow(const DataflowResult& df, RtlControlFlowGraph* cfg) {
  auto build_call_fusage_exprs = [&](RegMask arg_mask) -> std::vector<RtlExpr> {
    std::vector<RtlExpr> fusage;
    const RtlMachineMode gpr_mode = df.gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi;
    for (int r = 7; r >= 4; --r) {
      if ((arg_mask & (RegMask{1} << r)) != 0) {
        fusage.push_back(RtlExpr::MakeUse(RtlExpr::MakeReg(gpr_mode, r)));
      }
    }
    if (!df.fp64) {
      for (int r : {46, 44}) {
        const bool lo = (arg_mask & (RegMask{1} << r)) != 0;
        const bool hi = (arg_mask & (RegMask{1} << (r + 1))) != 0;
        if (lo || hi) {
          fusage.push_back(RtlExpr::MakeUse(
              RtlExpr::MakeReg(hi ? RtlMachineMode::kDf : RtlMachineMode::kSf, r)));
        }
      }
    } else {
      for (int r : {47, 46, 45, 44}) {
        if ((arg_mask & (RegMask{1} << r)) != 0) {
          fusage.push_back(RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kDf, r)));
        }
      }
    }
    return fusage;
  };

  const int min_reg_parm_bytes = df.gp64 ? 32 : 16;
  const RtlMachineMode ptr_mode = df.gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi;
  auto extract_call_arg_bytes = [](const RtlExpr& pat) -> int64_t {
    int64_t bytes = 0;
    pat.Visit([&bytes](const RtlExpr& n) {
      if (n.op == RtlOp::kCall && n.children.size() >= 2 && n.children[1].IsConstInt()) {
        bytes = std::max(bytes, n.children[1].int_val);
      }
    });
    return bytes;
  };
  auto record_sp_stack_arg_store = [&](const RtlExpr& stmt,
                                       std::vector<std::pair<int64_t, RtlMachineMode>>* stores) {
    if (!stmt.IsSet() || !stmt.SetDest().IsMem() || stmt.SetDest().flags.volatil ||
        stmt.SetDest().children.size() != 1) {
      return;
    }
    const RtlMachineMode m = stmt.SetDest().mode;
    const int sz = ModeByteSize(m);
    if (sz <= 0) {
      return;
    }
    const RtlExpr& addr = stmt.SetDest().children[0];
    int64_t off = -1;
    if (addr.IsReg() && addr.RegNo() == 29) {
      off = 0;
    } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsReg() &&
               addr.children[0].RegNo() == 29 && addr.children[1].IsConstInt()) {
      off = addr.children[1].int_val;
    }
    if (off >= min_reg_parm_bytes) {
      for (auto& kv : *stores) {
        if (kv.first == off) {
          kv.second = m;
          return;
        }
      }
      stores->emplace_back(off, m);
    }
  };

  const size_t n_blocks = cfg->rpo_blocks.size();
  for (size_t b = 0; b < n_blocks; ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
    std::vector<std::pair<int64_t, RtlMachineMode>> pending_stack_args;
    const size_t limit = std::min(blk.rtl_insns.size(), df.block_summaries[b].size());
    for (size_t i = 0; i < limit; ++i) {
      const InsnSummary& isum = df.block_summaries[b][i];
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!isum.valid || !(isum.is_call_seq || (!isum.is_sequence && isum.single.is_call))) {
        if (parsed.ok()) {
          if (parsed->pattern.op == RtlOp::kSequence && parsed->pattern.children.size() >= 2 &&
              !parsed->pattern.children[0].children.empty() &&
              !parsed->pattern.children[1].children.empty()) {
            record_sp_stack_arg_store(parsed->pattern.children[0].children[0], &pending_stack_args);
            record_sp_stack_arg_store(parsed->pattern.children[1].children[0], &pending_stack_args);
          } else {
            record_sp_stack_arg_store(parsed->pattern, &pending_stack_args);
          }
        }
        continue;
      }
      if (parsed.ok() && isum.is_call_seq && parsed->pattern.op == RtlOp::kSequence &&
          parsed->pattern.children.size() >= 2 && !parsed->pattern.children[1].children.empty()) {
        record_sp_stack_arg_store(parsed->pattern.children[1].children[0], &pending_stack_args);
      }
      const int64_t call_bytes = parsed.ok() ? extract_call_arg_bytes(parsed->pattern) : 0;
      RtlInsn& rinsn = blk.rtl_insns[i];
      if (!rinsn.has_explicit_call_fusage) {
        const RegMask fusage_mask =
            isum.is_call_seq ? isum.head.arg_fusage_uses : isum.single.arg_fusage_uses;
        rinsn.has_explicit_call_fusage = true;
        rinsn.call_fusage = build_call_fusage_exprs(fusage_mask);
        if (call_bytes > min_reg_parm_bytes) {
          for (const auto& [off, m] : pending_stack_args) {
            const int sz = ModeByteSize(m);
            if (off >= min_reg_parm_bytes && off + sz <= call_bytes) {
              RtlExpr addr = (off == 0) ? RtlExpr::MakeReg(ptr_mode, 29)
                                        : RtlExpr::MakeBinary(RtlOp::kPlus, ptr_mode,
                                                              RtlExpr::MakeReg(ptr_mode, 29),
                                                              RtlExpr::MakeConstInt(off));
              rinsn.call_fusage.push_back(RtlExpr::MakeUse(RtlExpr::MakeMem(m, std::move(addr))));
            }
          }
        }
      }
      pending_stack_args.clear();
    }
  }
}

}  // namespace

int MipsModeByteSize(RtlMachineMode mode) {
  return ModeByteSize(mode);
}

int MipsHardRegNregs(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  return HardRegNregs(regno, mode, gp64, fp64);
}

void DetectMipsRegisterWidthFlags(const RtlControlFlowGraph& cfg, const CompilationProfile* profile,
                                  bool* gp64, bool* fp64) {
  DetectRegisterWidthFlags(cfg, profile, gp64, fp64);
}

ParsedCompilerFlags ParseCompilationProfileFlags(const CompilationProfile& profile) {
  std::vector<std::string> tokens;
  tokens.reserve(profile.c_flags.size() + 1);
  tokens.push_back(absl::StrCat("-O", profile.optimize_level));
  tokens.insert(tokens.end(), profile.c_flags.begin(), profile.c_flags.end());
  return ParsedCompilerFlags::Parse(tokens);
}

void ReconstructMissingCallFusage(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  bool has_missing_call_fusage = false;
  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (blk.rtl_insns[i].has_explicit_call_fusage) {
        continue;
      }
      auto pn = ParsePatternAndNotes(blk.instructions[i]);
      if (!pn.ok()) {
        continue;
      }
      if (pn->has_explicit_call_fusage) {
        blk.rtl_insns[i].has_explicit_call_fusage = true;
        blk.rtl_insns[i].call_fusage = std::move(pn->call_fusage);
        continue;
      }
      if (IsCallPattern(pn->pattern) ||
          (pn->pattern.op == RtlOp::kSequence && !pn->pattern.children.empty() &&
           pn->pattern.children[0].op == RtlOp::kCallInsn)) {
        has_missing_call_fusage = true;
      }
    }
  }
  if (!has_missing_call_fusage) {
    return;
  }
  const DataflowResult df = RunHardRegDataflow(*cfg, &profile);
  ApplyMissingCallFusageFromDataflow(df, cfg);
}

class PortUnionFind {
 public:
  explicit PortUnionFind(size_t n) : parent_(n) {
    for (size_t i = 0; i < n; ++i) {
      parent_[i] = i;
    }
  }

  size_t Find(size_t x) {
    while (parent_[x] != x) {
      parent_[x] = parent_[parent_[x]];
      x = parent_[x];
    }
    return x;
  }

  void Union(size_t a, size_t b) {
    a = Find(a);
    b = Find(b);
    if (a != b) {
      if (a < b) {
        parent_[b] = a;
      } else {
        parent_[a] = b;
      }
    }
  }

 private:
  std::vector<size_t> parent_;
};

bool HasHigherSched2PriorityThanDeadUse(const RtlExpr& set_pat) {
  if (!set_pat.IsSet()) {
    return false;
  }
  const RtlExpr& src = set_pat.SetSrc();
  bool high_latency = false;
  src.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kMem || n.op == RtlOp::kMult || n.op == RtlOp::kDiv ||
        n.op == RtlOp::kUdiv || n.op == RtlOp::kMod || n.op == RtlOp::kUmod ||
        n.mode == RtlMachineMode::kSf || n.mode == RtlMachineMode::kDf) {
      high_latency = true;
    }
  });
  return high_latency;
}

bool MaybeInsertDeadLoopCombineUse(RtlControlFlowGraph* cfg, const DataflowResult& df,
                                   const CompilationProfile& profile) {
  const size_t n_blocks = cfg->rpo_blocks.size();
  if (n_blocks < 2) {
    return false;
  }
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);

  std::vector<bool> is_natural_loop_header(n_blocks, false);
  for (size_t u = 0; u < n_blocks; ++u) {
    for (size_t h : cfg->rpo_blocks[u].successors) {
      if (h >= 1 && h <= u && h < n_blocks && cfg->rpo_blocks[h].has_label) {
        is_natural_loop_header[h] = true;
      }
    }
  }

  RegMask global_multiblock_mask = 0;
  RegMask global_touched_mask = 0;
  std::vector<RegMask> block_touched(n_blocks, 0);
  for (size_t b = 0; b < n_blocks; ++b) {
    global_multiblock_mask |= df.live_in[b];
    for (const InsnSummary& isum : df.block_summaries[b]) {
      if (!isum.valid) {
        continue;
      }
      if (isum.is_sequence) {
        block_touched[b] |= isum.head.uses | isum.head.defs | isum.slot.uses | isum.slot.defs;
      } else {
        block_touched[b] |= isum.single.uses | isum.single.defs;
      }
    }
    global_touched_mask |= block_touched[b];
  }

  static constexpr std::array<int, 16> kCallerGprOrder = {2,  3,  4,  5,  6,  7,  8,  9,
                                                          10, 11, 12, 13, 14, 15, 24, 25};
  static constexpr std::array<int, 25> kAllGprAllocOrder = {
      2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 16, 17, 18, 19, 20, 21, 22, 23, 30};
  static constexpr std::array<int, 20> kCallerFprOrder = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                                                          42, 43, 44, 45, 46, 47, 48, 49, 50, 51};
  static constexpr std::array<int, 32> kAllFprAllocOrder = {
      32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
      48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};

  auto expr_has_call_or_volatile = [](const RtlExpr& expr) -> bool {
    bool bad = false;
    expr.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kCall || n.op == RtlOp::kCallInsn || n.flags.volatil) {
        bad = true;
      }
    });
    return bad;
  };

  bool any_inserted = false;
  for (size_t b = 1; b < n_blocks; ++b) {
    if (!is_natural_loop_header[b]) {
      continue;
    }
    const auto& sums = df.block_summaries[b];
    for (size_t i = 0; i < sums.size(); ++i) {
      const InsnSummary& isum = sums[i];
      if (!isum.valid || isum.is_sequence || isum.single.is_call || isum.single.is_note_or_pseudo ||
          !isum.top_pn.pattern.IsSet() || !isum.top_pn.pattern.SetDest().IsReg() ||
          isum.top_pn.pattern.SetDest().IsArgPointerReg() ||
          expr_has_call_or_volatile(isum.top_pn.pattern.SetSrc())) {
        continue;
      }
      const int r = isum.top_pn.pattern.SetDest().RegNo();
      const RtlMachineMode r_mode = isum.top_pn.pattern.SetDest().mode;
      const bool is_gpr = (r >= 2 && r <= 25);
      const bool is_fpr = (r >= 32 && r <= 51);
      if (!is_gpr && !is_fpr) {
        continue;
      }
      int caller_idx = -1;
      if (is_gpr) {
        auto it = std::find(kCallerGprOrder.begin(), kCallerGprOrder.end(), r);
        if (it != kCallerGprOrder.end()) {
          caller_idx = static_cast<int>(it - kCallerGprOrder.begin());
        }
      } else {
        auto it = std::find(kCallerFprOrder.begin(), kCallerFprOrder.end(), r);
        if (it != kCallerFprOrder.end()) {
          caller_idx = static_cast<int>(it - kCallerFprOrder.begin());
        }
      }
      if (caller_idx <= 0) {
        continue;
      }
      const int r_span = std::max(1, MipsHardRegNregs(r, r_mode, df.gp64, df.fp64));
      if (r_span > 1 && (r % 2) != 0) {
        continue;
      }
      RegMask r_mask = 0;
      for (int w = 0; w < r_span; ++w) {
        r_mask |= (RegMask{1} << (r + w));
      }
      if (((df.live_in[b] | df.live_out[b]) & r_mask) != 0 || (isum.single.uses & r_mask) != 0) {
        continue;
      }
      bool used_before_i = false;
      for (size_t k = 0; k < i; ++k) {
        if (!sums[k].valid) {
          continue;
        }
        const RegMask k_mask =
            sums[k].is_sequence
                ? (sums[k].head.uses | sums[k].head.defs | sums[k].slot.uses | sums[k].slot.defs)
                : (sums[k].single.uses | sums[k].single.defs);
        if ((k_mask & r_mask) != 0) {
          used_before_i = true;
          break;
        }
      }
      if (used_before_i) {
        continue;
      }
      bool read_after_i = false;
      bool crosses_call_in_b = false;
      size_t last_use_k = i;
      for (size_t k = i + 1; k < sums.size(); ++k) {
        if (!sums[k].valid) {
          continue;
        }
        const RegMask k_uses =
            sums[k].is_sequence ? (sums[k].head.uses | sums[k].slot.uses) : sums[k].single.uses;
        const RegMask k_defs =
            sums[k].is_sequence ? (sums[k].head.defs | sums[k].slot.defs) : sums[k].single.defs;
        if ((k_uses & r_mask) != 0) {
          read_after_i = true;
          last_use_k = k;
        }
        if ((k_defs & r_mask) != 0) {
          break;
        }
      }
      if (!read_after_i) {
        continue;
      }
      for (size_t k = i + 1; k < last_use_k; ++k) {
        if (sums[k].valid && ((sums[k].is_sequence && sums[k].is_call_seq) ||
                              (!sums[k].is_sequence && sums[k].single.is_call))) {
          crosses_call_in_b = true;
          break;
        }
      }
      if (crosses_call_in_b) {
        continue;
      }
      bool touched_outside_b = false;
      for (size_t ob = 0; ob < n_blocks; ++ob) {
        if (ob != b && (block_touched[ob] & r_mask) != 0) {
          touched_outside_b = true;
          break;
        }
      }
      if (touched_outside_b) {
        continue;
      }

      const RegMask b_busy = df.live_in[b] | df.live_out[b] | block_touched[b];
      bool has_earlier_free_reg_in_b = false;
      for (int idx = 0; idx < caller_idx; ++idx) {
        const int free_r = is_gpr ? kCallerGprOrder[idx] : kCallerFprOrder[idx];
        if (r_span > 1 && (free_r % 2) != 0) {
          continue;
        }
        RegMask free_mask = 0;
        for (int w = 0; w < r_span; ++w) {
          free_mask |= (RegMask{1} << (free_r + w));
        }
        if ((b_busy & free_mask) == 0 && (global_touched_mask & free_mask) != 0) {
          has_earlier_free_reg_in_b = true;
          break;
        }
      }
      if (!has_earlier_free_reg_in_b) {
        continue;
      }

      bool higher_reg_is_multiblock = false;
      if (is_gpr) {
        auto all_it = std::find(kAllGprAllocOrder.begin(), kAllGprAllocOrder.end(), r);
        for (auto it = (all_it != kAllGprAllocOrder.end()) ? (all_it + 1) : kAllGprAllocOrder.end();
             it != kAllGprAllocOrder.end(); ++it) {
          if (*it == 30 && cfg->sideband.frame_pointer_needed) {
            continue;
          }
          if ((global_multiblock_mask & (RegMask{1} << *it)) != 0) {
            higher_reg_is_multiblock = true;
            break;
          }
        }
      } else {
        auto all_it = std::find(kAllFprAllocOrder.begin(), kAllFprAllocOrder.end(), r);
        for (auto it = (all_it != kAllFprAllocOrder.end()) ? (all_it + 1) : kAllFprAllocOrder.end();
             it != kAllFprAllocOrder.end(); ++it) {
          if ((global_multiblock_mask & (RegMask{1} << *it)) != 0) {
            higher_reg_is_multiblock = true;
            break;
          }
        }
      }
      if (!higher_reg_is_multiblock) {
        continue;
      }

      size_t insert_idx = 0;
      if (flags.RunsSched2() && i >= 1 && !sums.empty() && sums[0].valid && !sums[0].is_sequence &&
          ((sums[0].single.uses | sums[0].single.defs) & r_mask) == 0 &&
          HasHigherSched2PriorityThanDeadUse(sums[0].top_pn.pattern)) {
        insert_idx = 1;
      }
      RtlPatternAndNotes dead_use_pn;
      dead_use_pn.pattern = RtlExpr::MakeUse(MakeGccHardReg(r_mode, r));
      // Mirror `flow.c:2507-2550` (`mark_used_regs`, `flow.c:2511`) and `sched.c:2855`
      // (`attach_deaths`):
      // a non-return `(use (reg:M r))` placed before `r`'s full redefinition (with
      // no intervening read) kills `r`, so it carries `(REG_DEAD (reg:M r))` when
      // `optimize > 0`.
      if (flags.opt_level > 0) {
        dead_use_pn.reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegDead,
            .list_op = RtlOp::kExprList,
            .datum = MakeGccHardReg(r_mode, r),
        });
      }
      const std::string use_str = FormatInsnWithoutCallFusage(dead_use_pn);
      cfg->rpo_blocks[b].instructions.insert(
          cfg->rpo_blocks[b].instructions.begin() + static_cast<ptrdiff_t>(insert_idx), use_str);
      cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
      any_inserted = true;
      break;
    }
  }
  return any_inserted;
}

bool HasTwoHopPreferenceBridgeReturnWitness(const RtlControlFlowGraph& cfg,
                                            const DataflowResult& df, int ret_hr,
                                            absl::Span<const int> candidates) {
  const size_t n_blocks = cfg.rpo_blocks.size();
  if (n_blocks < 4 || df.block_summaries.empty()) {
    return false;
  }
  const size_t exit_b = n_blocks - 1;
  std::vector<size_t> exit_preds;
  for (size_t b = 0; b + 1 < n_blocks; ++b) {
    for (size_t sb : cfg.rpo_blocks[b].successors) {
      if (sb == exit_b) {
        exit_preds.push_back(b);
        break;
      }
    }
  }
  if (exit_preds.size() < 2) {
    return false;
  }

  auto in_candidates = [&](int r) {
    return std::find(candidates.begin(), candidates.end(), r) != candidates.end();
  };
  std::vector<std::pair<int, int>> entry_moves;
  auto check_entry_pat = [&](const RtlExpr& pat) {
    if (!pat.IsSet() || !pat.SetDest().IsReg() || !pat.SetSrc().IsReg()) {
      return;
    }
    const int v_hr = pat.SetDest().RegNo();
    const int w_hr = pat.SetSrc().RegNo();
    if (v_hr < w_hr && in_candidates(v_hr) && in_candidates(w_hr) &&
        (df.live_in[0] & (RegMask{1} << w_hr)) != 0) {
      entry_moves.emplace_back(v_hr, w_hr);
    }
  };
  for (const InsnSummary& isum : df.block_summaries[0]) {
    if (!isum.valid) {
      continue;
    }
    if (isum.is_sequence) {
      check_entry_pat(isum.head_pn.pattern);
      check_entry_pat(isum.slot_pn.pattern);
    } else {
      check_entry_pat(isum.top_pn.pattern);
    }
  }

  for (const auto& [v_hr, w_hr] : entry_moves) {
    const RegMask v_bit = RegMask{1} << v_hr;
    const RegMask w_bit = RegMask{1} << w_hr;
    bool co_live = false;
    bool w_redefined = false;
    for (size_t b = 1; b < n_blocks; ++b) {
      if ((df.live_in[b] & (v_bit | w_bit)) == (v_bit | w_bit)) {
        co_live = true;
      }
      for (const InsnSummary& isum : df.block_summaries[b]) {
        if (!isum.valid) {
          continue;
        }
        const RegMask d = isum.is_sequence ? (isum.head.defs | isum.slot.defs) : isum.single.defs;
        if ((d & w_bit) != 0) {
          w_redefined = true;
        }
      }
    }
    if (!co_live || !w_redefined) {
      continue;
    }
    size_t first_v_pred = n_blocks;
    bool saw_later_w_pred = false;
    for (size_t pb : exit_preds) {
      RegMask pb_uses = 0;
      RegMask pb_defs = 0;
      for (const InsnSummary& isum : df.block_summaries[pb]) {
        if (!isum.valid) {
          continue;
        }
        pb_uses |= isum.is_sequence ? (isum.head.uses | isum.slot.uses) : isum.single.uses;
        pb_defs |= isum.is_sequence ? (isum.head.defs | isum.slot.defs) : isum.single.defs;
      }
      if ((pb_defs & (RegMask{1} << ret_hr)) == 0) {
        continue;
      }
      if (first_v_pred == n_blocks && (pb_uses & v_bit) != 0 && (df.live_out[pb] & v_bit) == 0) {
        first_v_pred = pb;
      } else if (pb > first_v_pred && (pb_uses & w_bit) != 0 && (df.live_out[pb] & w_bit) == 0) {
        saw_later_w_pred = true;
        break;
      }
    }
    if (saw_later_w_pred) {
      return true;
    }
  }
  return false;
}

bool HasDeletedSharedReturnMoveWitnessWithDataflow(const RtlControlFlowGraph& cfg,
                                                   const DataflowResult& df, int ret_hr,
                                                   RtlMachineMode ret_mode) {
  const size_t n_blocks = cfg.rpo_blocks.size();
  if (n_blocks < 3) {
    return false;
  }
  const bool valid_gpr_ret =
      (ret_hr == 2 && (ret_mode == RtlMachineMode::kSi || ret_mode == RtlMachineMode::kDi));
  const bool valid_fpr_ret =
      (ret_hr == 32 && (ret_mode == RtlMachineMode::kSf || ret_mode == RtlMachineMode::kDf));
  if (!valid_gpr_ret && !valid_fpr_ret) {
    return false;
  }
  const int ret_span = std::max(1, MipsHardRegNregs(ret_hr, ret_mode, df.gp64, df.fp64));

  struct FlatStep {
    RegMask uses = 0;
    RegMask defs = 0;
    bool is_call = false;
    const RtlExpr* pat = nullptr;
  };
  std::vector<std::vector<FlatStep>> blk_steps(n_blocks);
  for (size_t b = 0; b < n_blocks; ++b) {
    for (const InsnSummary& isum : df.block_summaries[b]) {
      if (!isum.valid) {
        continue;
      }
      if (isum.is_sequence) {
        blk_steps[b].push_back(FlatStep{
            .uses = isum.head.uses,
            .defs = isum.head.defs,
            .is_call = isum.is_call_seq,
            .pat = &isum.head_pn.pattern,
        });
        blk_steps[b].push_back(FlatStep{
            .uses = isum.slot.uses,
            .defs = isum.slot.defs,
            .is_call = false,
            .pat = &isum.slot_pn.pattern,
        });
      } else if (!isum.single.is_note_or_pseudo || isum.top_pn.pattern.op == RtlOp::kUse ||
                 isum.top_pn.pattern.op == RtlOp::kClobber) {
        blk_steps[b].push_back(FlatStep{
            .uses = isum.single.uses,
            .defs = isum.single.defs,
            .is_call = isum.single.is_call,
            .pat = &isum.top_pn.pattern,
        });
      }
    }
  }

  std::vector<std::vector<RegMask>> step_live_before(n_blocks);
  std::vector<std::vector<RegMask>> step_live_after(n_blocks);
  for (size_t b = 0; b < n_blocks; ++b) {
    const size_t m = blk_steps[b].size();
    step_live_before[b].resize(m, 0);
    step_live_after[b].resize(m, 0);
    RegMask cur = df.live_out[b];
    if (df.can_exit[b]) {
      cur |= df.uses_epilogue_delay_at_exit[b]
                 ? ((df.live_at_exit & ~df.epilogue_delay.defs) | df.epilogue_delay.uses)
                 : df.live_at_exit;
    }
    for (size_t rev = m; rev > 0; --rev) {
      const size_t s = rev - 1;
      step_live_after[b][s] = cur;
      cur = (cur & ~blk_steps[b][s].defs) | blk_steps[b][s].uses;
      step_live_before[b][s] = cur;
    }
  }

  std::vector<int> candidates;
  if (valid_gpr_ret) {
    static constexpr std::array<int, 16> kCallerGprs = {2,  3,  4,  5,  6,  7,  8,  9,
                                                        10, 11, 12, 13, 14, 15, 24, 25};
    for (int r : kCallerGprs) {
      if (r >= ret_hr + ret_span) {
        candidates.push_back(r);
      }
    }
  } else {
    for (int r = ret_hr + ret_span; r <= 51; ++r) {
      if (!df.fp64 && (r % 2) != 0) {
        continue;
      }
      candidates.push_back(r);
    }
  }

  for (int w_hr : candidates) {
    const RegMask w_bit = RegMask{1} << w_hr;
    bool any_multiblock = false;
    for (size_t b = 1; b < n_blocks; ++b) {
      if ((df.live_in[b] & w_bit) != 0) {
        any_multiblock = true;
        break;
      }
    }
    if (!any_multiblock) {
      continue;
    }

    PortUnionFind uf(2 * n_blocks);
    for (size_t b = 0; b < n_blocks; ++b) {
      bool entry_alive = (df.live_in[b] & w_bit) != 0;
      for (size_t s = 0; s < blk_steps[b].size() && entry_alive; ++s) {
        const bool defs_w = (blk_steps[b][s].defs & w_bit) != 0;
        const bool uses_w = (blk_steps[b][s].uses & w_bit) != 0;
        if (defs_w && !uses_w) {
          entry_alive = false;
        } else if (!defs_w && (step_live_after[b][s] & w_bit) == 0) {
          entry_alive = false;
        }
      }
      if (entry_alive && (df.live_out[b] & w_bit) != 0) {
        uf.Union(2 * b, 2 * b + 1);
      }
    }
    for (size_t u = 0; u < n_blocks; ++u) {
      if ((df.live_out[u] & w_bit) == 0) {
        continue;
      }
      for (size_t v : cfg.rpo_blocks[u].successors) {
        if (v < n_blocks && (df.live_in[v] & w_bit) != 0) {
          uf.Union(2 * u + 1, 2 * v);
        }
      }
    }

    absl::flat_hash_set<size_t> candidate_roots;
    for (size_t b = 1; b < n_blocks; ++b) {
      if ((df.live_in[b] & w_bit) != 0) {
        candidate_roots.insert(uf.Find(2 * b));
      }
    }

    for (size_t root : candidate_roots) {
      if ((df.live_in[0] & w_bit) != 0 && uf.Find(0) == root) {
        continue;
      }

      int c_span = 1;
      bool has_any_def = false;
      bool bad_call_or_conflict = false;

      // First pass over `C` to determine its widest register span (`1` vs `2` words).
      for (size_t b = 0; b < n_blocks; ++b) {
        const bool entry_in_c = (df.live_in[b] & w_bit) != 0 && uf.Find(2 * b) == root;
        const bool exit_in_c = (df.live_out[b] & w_bit) != 0 && uf.Find(2 * b + 1) == root;
        if (!entry_in_c && !exit_in_c) {
          continue;
        }
        const size_t m = blk_steps[b].size();
        size_t kill_step = m;
        if (entry_in_c) {
          for (size_t s = 0; s < m; ++s) {
            if ((blk_steps[b][s].defs & w_bit) != 0 && (blk_steps[b][s].uses & w_bit) == 0) {
              kill_step = s;
              break;
            }
          }
        }
        auto inspect_step_mode = [&](size_t s) {
          if (blk_steps[b][s].pat == nullptr) {
            return;
          }
          blk_steps[b][s].pat->Visit([&](const RtlExpr& n) {
            if (n.IsReg() && n.RegNo() == w_hr) {
              c_span = std::max(c_span, MipsHardRegNregs(w_hr, n.mode, df.gp64, df.fp64));
            }
          });
        };
        if (entry_in_c) {
          for (size_t s = 0; s < kill_step; ++s) {
            if (((blk_steps[b][s].uses | blk_steps[b][s].defs) & w_bit) != 0) {
              inspect_step_mode(s);
            }
          }
        }
        if (exit_in_c && (!entry_in_c || kill_step < m)) {
          for (size_t rev = m; rev > 0; --rev) {
            const size_t s = rev - 1;
            if (((blk_steps[b][s].uses | blk_steps[b][s].defs) & w_bit) != 0) {
              inspect_step_mode(s);
            }
            if ((blk_steps[b][s].defs & w_bit) != 0 && (blk_steps[b][s].uses & w_bit) == 0) {
              break;
            }
          }
        }
      }

      if (c_span > 1 && (w_hr % 2) != 0) {
        continue;
      }
      const int target_span = std::max(ret_span, c_span);
      RegMask target_ret_mask = 0;
      for (int k = 0; k < target_span; ++k) {
        target_ret_mask |= (RegMask{1} << (ret_hr + k));
      }

      // Second pass: verify zero call interaction and zero conflict with `target_ret_mask`.
      for (size_t b = 0; b < n_blocks && !bad_call_or_conflict; ++b) {
        const bool entry_in_c = (df.live_in[b] & w_bit) != 0 && uf.Find(2 * b) == root;
        const bool exit_in_c = (df.live_out[b] & w_bit) != 0 && uf.Find(2 * b + 1) == root;
        if (!entry_in_c && !exit_in_c) {
          continue;
        }
        const size_t m = blk_steps[b].size();
        std::vector<bool> c_live_before(m, false);
        std::vector<bool> c_live_after(m, false);
        std::vector<bool> c_defined_at(m, false);
        std::vector<bool> c_read_at(m, false);

        size_t kill_step = m;
        if (entry_in_c) {
          for (size_t s = 0; s < m; ++s) {
            if ((blk_steps[b][s].defs & w_bit) != 0 && (blk_steps[b][s].uses & w_bit) == 0) {
              kill_step = s;
              break;
            }
          }
          const bool unbroken_to_exit = exit_in_c && (kill_step == m);
          size_t last_prefix_step = 0;
          bool has_prefix_use = false;
          if (!unbroken_to_exit) {
            for (size_t s = 0; s < kill_step; ++s) {
              if ((blk_steps[b][s].uses & w_bit) != 0) {
                has_prefix_use = true;
                last_prefix_step = s;
              }
            }
          }
          const size_t prefix_end =
              unbroken_to_exit ? m : (has_prefix_use ? (last_prefix_step + 1) : 0);
          for (size_t s = 0; s < prefix_end; ++s) {
            c_live_before[s] = true;
            if (unbroken_to_exit || s < last_prefix_step) {
              c_live_after[s] = true;
            }
            if ((blk_steps[b][s].uses & w_bit) != 0) {
              c_read_at[s] = true;
            }
            if ((blk_steps[b][s].defs & w_bit) != 0) {
              c_defined_at[s] = true;
              if (s == last_prefix_step && !unbroken_to_exit) {
                c_live_after[s] = false;
              }
            }
          }
          if (!unbroken_to_exit && !has_prefix_use && (df.live_in[b] & target_ret_mask) != 0) {
            bad_call_or_conflict = true;
            break;
          }
        }

        if (exit_in_c && (!entry_in_c || kill_step < m)) {
          for (size_t rev = m; rev > 0; --rev) {
            const size_t s = rev - 1;
            c_live_after[s] = true;
            if ((blk_steps[b][s].uses & w_bit) != 0) {
              c_read_at[s] = true;
            }
            if ((blk_steps[b][s].defs & w_bit) != 0) {
              c_defined_at[s] = true;
              if ((blk_steps[b][s].uses & w_bit) == 0) {
                break;
              }
            }
            c_live_before[s] = true;
          }
        }

        if (entry_in_c && (df.live_in[b] & target_ret_mask) != 0) {
          bad_call_or_conflict = true;
          break;
        }
        const RegMask blk_exit_ret =
            (df.live_out[b] | (df.can_exit[b] ? df.live_at_exit : RegMask{0})) & target_ret_mask;
        if (exit_in_c && blk_exit_ret != 0) {
          bad_call_or_conflict = true;
          break;
        }

        for (size_t s = 0; s < m; ++s) {
          if (c_defined_at[s]) {
            has_any_def = true;
          }
          if (blk_steps[b][s].is_call &&
              ((c_live_before[s] && c_live_after[s]) || c_defined_at[s] || c_read_at[s])) {
            bad_call_or_conflict = true;
            break;
          }
          const RegMask ret_before = step_live_before[b][s] & target_ret_mask;
          const RegMask ret_after = step_live_after[b][s] & target_ret_mask;
          if ((c_live_before[s] && ret_before != 0) || (c_live_after[s] && ret_after != 0) ||
              ((blk_steps[b][s].defs & target_ret_mask) != 0 && c_live_after[s]) ||
              (c_defined_at[s] && ret_after != 0)) {
            bad_call_or_conflict = true;
            break;
          }
        }
      }

      if (!bad_call_or_conflict && has_any_def) {
        return true;
      }
    }
  }
  return HasTwoHopPreferenceBridgeReturnWitness(cfg, df, ret_hr, candidates);
}

bool IsSimpleUncondJumpSummaryPattern(const RtlExpr& pat) {
  return pat.IsSet() && pat.SetDest().op == RtlOp::kPc && pat.SetSrc().op == RtlOp::kLabelRef;
}

const RtlExpr* ExtractPredecessorExitTailReturnSet(const std::vector<InsnSummary>& p_sums,
                                                   bool is_fallthrough_pred) {
  if (p_sums.empty()) {
    return nullptr;
  }
  size_t last_active_idx = p_sums.size();
  for (size_t rev = p_sums.size(); rev > 0; --rev) {
    const InsnSummary& s = p_sums[rev - 1];
    if (s.valid && (s.is_sequence || !s.single.is_note_or_pseudo)) {
      last_active_idx = rev - 1;
      break;
    }
  }
  if (last_active_idx >= p_sums.size()) {
    return nullptr;
  }
  const InsnSummary& last_s = p_sums[last_active_idx];
  if (last_s.is_sequence) {
    if (last_s.is_call_seq || !IsSimpleUncondJumpSummaryPattern(last_s.head_pn.pattern)) {
      return nullptr;
    }
    if (last_s.slot_pn.pattern.IsSet() && last_s.slot_pn.pattern.SetDest().IsReg()) {
      const int sr = last_s.slot_pn.pattern.SetDest().RegNo();
      if (sr == 2 || sr == 32) {
        return &last_s.slot_pn.pattern;
      }
    }
    if ((last_s.slot.defs & kRetCandMask) != 0) {
      return nullptr;
    }
    for (size_t rev = last_active_idx; rev > 0; --rev) {
      const InsnSummary& prev = p_sums[rev - 1];
      if (!prev.valid || (!prev.is_sequence && prev.single.is_note_or_pseudo)) {
        continue;
      }
      return prev.is_sequence ? nullptr : &prev.top_pn.pattern;
    }
    return nullptr;
  }
  if (IsSimpleUncondJumpSummaryPattern(last_s.top_pn.pattern)) {
    for (size_t rev = last_active_idx; rev > 0; --rev) {
      const InsnSummary& prev = p_sums[rev - 1];
      if (!prev.valid || (!prev.is_sequence && prev.single.is_note_or_pseudo)) {
        continue;
      }
      return prev.is_sequence ? nullptr : &prev.top_pn.pattern;
    }
    return nullptr;
  }
  return is_fallthrough_pred ? &last_s.top_pn.pattern : nullptr;
}

RegMask GetSuppressedSharedReturnExitMaskInPostDbr(const RtlControlFlowGraph& cfg,
                                                   const DataflowResult& df) {
  const size_t n_blocks = cfg.rpo_blocks.size();
  if (n_blocks < 3) {
    return 0;
  }
  const size_t exit_b = n_blocks - 1;
  if (!df.can_exit[exit_b] || !cfg.rpo_blocks[exit_b].successors.empty() ||
      !cfg.rpo_blocks[exit_b].addr_vec_targets.empty() || df.uses_epilogue_delay_at_exit[exit_b]) {
    return 0;
  }
  std::vector<size_t> exit_preds;
  for (size_t b = 0; b + 1 < n_blocks; ++b) {
    for (size_t sb : cfg.rpo_blocks[b].successors) {
      if (sb == exit_b) {
        exit_preds.push_back(b);
      }
    }
  }
  if (exit_preds.size() < 2 || exit_preds.back() != exit_b - 1) {
    return 0;
  }

  int common_ret_hr = -1;
  RtlMachineMode common_ret_mode = RtlMachineMode::kNone;
  for (size_t pb : exit_preds) {
    if (cfg.rpo_blocks[pb].successors.size() != 1 || cfg.rpo_blocks[pb].successors[0] != exit_b ||
        !cfg.rpo_blocks[pb].addr_vec_targets.empty()) {
      return 0;
    }
    const RtlExpr* set_pat =
        ExtractPredecessorExitTailReturnSet(df.block_summaries[pb], pb + 1 == exit_b);
    if (set_pat == nullptr || !set_pat->IsSet() || !set_pat->SetDest().IsReg()) {
      return 0;
    }
    const int r = set_pat->SetDest().RegNo();
    const RtlMachineMode m = set_pat->SetDest().mode;
    const bool valid_scalar_ret =
        (r == 2 && (m == RtlMachineMode::kSi || m == RtlMachineMode::kDi)) ||
        (r == 32 && (m == RtlMachineMode::kSf || m == RtlMachineMode::kDf));
    if (!valid_scalar_ret) {
      return 0;
    }
    if (common_ret_hr < 0) {
      common_ret_hr = r;
      common_ret_mode = m;
    } else if (common_ret_hr != r || common_ret_mode != m) {
      return 0;
    }
  }

  const int ret_span =
      std::max(1, MipsHardRegNregs(common_ret_hr, common_ret_mode, df.gp64, df.fp64));
  RegMask ret_mask = 0;
  for (int k = 0; k < ret_span; ++k) {
    ret_mask |= (RegMask{1} << (common_ret_hr + k));
  }
  for (const InsnSummary& es : df.block_summaries[exit_b]) {
    if (!es.valid) {
      continue;
    }
    const RegMask edefs = es.is_sequence ? (es.head.defs | es.slot.defs) : es.single.defs;
    if ((edefs & ret_mask) != 0) {
      return 0;
    }
  }

  if (!HasDeletedSharedReturnMoveWitnessWithDataflow(cfg, df, common_ret_hr, common_ret_mode)) {
    return 0;
  }
  return ret_mask;
}

bool BlockHasCompetingSimpleDelayCandidate(const std::vector<InsnSummary>& b_sums) {
  if (b_sums.size() <= 1) {
    return false;
  }
  const InsnSummary& seq_sum = b_sums.back();
  auto expr_has_mem = [](const RtlExpr& expr) -> bool {
    bool found = false;
    expr.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kMem) {
        found = true;
      }
    });
    return found;
  };
  auto is_mem_write = [](const RtlExpr& expr) -> bool {
    if (expr.IsSet() && expr.SetDest().IsMem()) {
      return true;
    }
    if (expr.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : expr.children) {
        if (ch.IsSet() && ch.SetDest().IsMem()) {
          return true;
        }
      }
    }
    return false;
  };

  size_t window_start = 0;
  for (size_t i = 0; i + 1 < b_sums.size(); ++i) {
    const InsnSummary& s = b_sums[i];
    if (!s.valid || s.is_sequence || s.single.is_call) {
      window_start = i + 1;
    }
  }
  for (size_t i = window_start; i + 1 < b_sums.size(); ++i) {
    const InsnSummary& s = b_sums[i];
    if (!s.valid || s.single.is_note_or_pseudo) {
      continue;
    }
    const RtlExpr& pat = s.top_pn.pattern;
    if (pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber || pat.op == RtlOp::kParallel) {
      continue;
    }
    if (pat.IsSet() && !pat.SetDest().IsMem() && expr_has_mem(pat.SetSrc())) {
      continue;
    }
    if (((s.single.defs & (seq_sum.head.uses | seq_sum.head.defs)) != 0) ||
        ((seq_sum.head.defs & s.single.uses) != 0)) {
      continue;
    }
    const bool s_has_mem = expr_has_mem(pat);
    const bool s_writes_mem = is_mem_write(pat);
    bool conflicts_with_later = false;
    for (size_t j = i + 1; j + 1 < b_sums.size(); ++j) {
      const InsnSummary& ls = b_sums[j];
      if (!ls.valid || ls.single.is_note_or_pseudo) {
        continue;
      }
      const RtlExpr& lpat = ls.top_pn.pattern;
      if (((s.single.defs & (ls.single.uses | ls.single.defs)) != 0) ||
          ((ls.single.defs & s.single.uses) != 0) || (s_writes_mem && expr_has_mem(lpat)) ||
          (s_has_mem && is_mem_write(lpat))) {
        conflicts_with_later = true;
        break;
      }
    }
    if (!conflicts_with_later) {
      return true;
    }
  }
  return false;
}

namespace {

void InsertTrailingReturnValueUseIfNeeded(RtlControlFlowGraph* cfg, const DataflowResult& df,
                                          RegMask exit_ret_mask, bool attach_notes,
                                          const CompilationProfile& profile) {
  const size_t n_blocks = cfg->rpo_blocks.size();
  if (exit_ret_mask == 0 || !df.can_exit[n_blocks - 1] || HasExistingReturnUse(*cfg)) {
    return;
  }
  bool saw_r2_di = false;
  bool saw_f0_df = false;
  auto inspect_ret_modes = [&](const RtlExpr& expr) {
    expr.Visit([&](const RtlExpr& node) {
      if (node.IsReg() && node.flags.integrated) {
        if (node.RegNo() == 2 && node.mode == RtlMachineMode::kDi) {
          saw_r2_di = true;
        } else if (node.RegNo() == 32 && node.mode == RtlMachineMode::kDf) {
          saw_f0_df = true;
        }
      }
    });
  };
  if (cfg->sideband.epilogue_delay_pattern.has_value()) {
    auto ep_pn = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (ep_pn.ok()) {
      inspect_ret_modes(ep_pn->pattern);
    }
  }
  for (const auto& blk : cfg->rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (parsed.ok()) {
        inspect_ret_modes(parsed->pattern);
      }
    }
  }

  int ret_regno = -1;
  RtlMachineMode ret_mode = RtlMachineMode::kSi;
  if ((exit_ret_mask & ((RegMask{1} << 2) | (RegMask{1} << 3))) != 0) {
    ret_regno = 2;
    ret_mode = (((exit_ret_mask & (RegMask{1} << 3)) != 0) || saw_r2_di) ? RtlMachineMode::kDi
                                                                         : RtlMachineMode::kSi;
  } else if ((exit_ret_mask & ((RegMask{1} << 32) | (RegMask{1} << 33))) != 0) {
    ret_regno = 32;
    ret_mode = (((exit_ret_mask & (RegMask{1} << 33)) != 0) || saw_f0_df) ? RtlMachineMode::kDf
                                                                          : RtlMachineMode::kSf;
  }
  if (ret_regno < 0) {
    return;
  }

  std::string use_sexpr =
      RtlExpr::MakeUse(RtlExpr::MakeReg(ret_mode, ret_regno, RtlFlags{.integrated = true}))
          .ToGccPrintRtlString();
  RtlBasicBlock& last_blk = cfg->rpo_blocks[n_blocks - 1];
  bool ends_with_ret_jump = false;
  bool ends_with_ret_seq = false;
  if (!last_blk.instructions.empty()) {
    auto last_parsed = ParsePatternAndNotes(last_blk.instructions.back());
    ends_with_ret_jump = last_parsed.ok() && IsReturnJumpPattern(last_parsed->pattern);
    ends_with_ret_seq = ends_with_ret_jump && last_parsed->pattern.op == RtlOp::kSequence;
  }
  bool all_jumpers_to_last_are_annulled_non_t1_steals = false;
  if (ends_with_ret_jump && !ends_with_ret_seq && last_blk.has_label && n_blocks >= 2) {
    const RtlBasicBlock& prev_check = cfg->rpo_blocks[n_blocks - 2];
    RtlExpr prev_last_active_pat;
    bool has_prev_last_active = false;
    for (size_t k = prev_check.instructions.size(); k > 0;) {
      --k;
      auto pk = ParsePatternAndNotes(prev_check.instructions[k]);
      if (pk.ok() && pk->pattern.op != RtlOp::kNote && pk->pattern.op != RtlOp::kUse &&
          pk->pattern.op != RtlOp::kClobber) {
        prev_last_active_pat = pk->pattern;
        has_prev_last_active = true;
        break;
      }
    }
    bool saw_jumper = false;
    bool all_annulled = true;
    for (size_t pb = 0; pb + 1 < n_blocks; ++pb) {
      const auto& pblk = cfg->rpo_blocks[pb];
      if (std::find(pblk.addr_vec_targets.begin(), pblk.addr_vec_targets.end(), n_blocks - 1) !=
          pblk.addr_vec_targets.end()) {
        all_annulled = false;
        break;
      }
      if (std::find(pblk.successors.begin(), pblk.successors.end(), n_blocks - 1) ==
          pblk.successors.end()) {
        continue;
      }
      if (pb + 1 == n_blocks - 1 && pblk.successors.size() == 1) {
        bool p_ends_jump = false;
        if (!pblk.instructions.empty()) {
          auto plast = ParsePatternAndNotes(pblk.instructions.back());
          p_ends_jump = plast.ok() && IsAnyControlTransferPattern(plast->pattern);
        }
        if (!p_ends_jump) {
          continue;
        }
      }
      saw_jumper = true;
      bool is_non_t1_annulled_seq = false;
      if (!pblk.instructions.empty()) {
        auto plast = ParsePatternAndNotes(pblk.instructions.back());
        if (plast.ok() && plast->pattern.op == RtlOp::kSequence &&
            plast->pattern.children.size() == 2 &&
            plast->pattern.children[0].op == RtlOp::kJumpInsn &&
            plast->pattern.children[0].flags.unchanging &&
            plast->pattern.children[1].flags.in_struct) {
          auto pslot = RtlPatternAndNotes::FromSequenceSubInsn(plast->pattern.children[1]);
          if (pslot.ok() && (!has_prev_last_active || !(pslot->pattern == prev_last_active_pat))) {
            is_non_t1_annulled_seq = true;
          }
        }
      }
      if (!is_non_t1_annulled_seq) {
        all_annulled = false;
        break;
      }
    }
    all_jumpers_to_last_are_annulled_non_t1_steals = saw_jumper && all_annulled;
  }
  bool inserted_use = false;
  if (!attach_notes && last_blk.has_label && last_blk.instructions.empty() &&
      last_blk.successors.empty() && n_blocks >= 2) {
    // At `-O0` (`stmt.c:2530-2584`), every `expand_value_return` emits
    // `(set (reg/i:M ret_hr) val); (use (reg/i:M ret_hr)); (jump return_label)`
    // directly inside the returning block, leaving `return_label:` (`last_blk`)
    // empty so `stupid_life_analysis`'s backward linear scan does not extend
    // `regs_live[ret_hr]` across unrelated intermediate blocks.
    auto expr_sets_integrated_ret = [&](const RtlExpr& expr) -> bool {
      bool found = false;
      auto check_elem = [&](const RtlExpr& elem) {
        if (elem.IsSet() && elem.SetDest().IsReg() && elem.SetDest().flags.integrated &&
            elem.SetDest().RegNo() == ret_regno) {
          found = true;
        }
      };
      if (expr.op == RtlOp::kParallel) {
        for (const RtlExpr& c : expr.children) {
          check_elem(c);
        }
      } else {
        check_elem(expr);
      }
      return found;
    };
    for (size_t pb = 0; pb + 1 < n_blocks; ++pb) {
      RtlBasicBlock& pblk = cfg->rpo_blocks[pb];
      if (std::find(pblk.successors.begin(), pblk.successors.end(), n_blocks - 1) ==
          pblk.successors.end()) {
        continue;
      }
      bool has_ret_set = false;
      for (const std::string& s : pblk.instructions) {
        auto p = ParsePatternAndNotes(s);
        if (p.ok() && expr_sets_integrated_ret(p->pattern)) {
          has_ret_set = true;
          break;
        }
      }
      if (!has_ret_set) {
        continue;
      }
      bool p_ends_jump = false;
      if (!pblk.instructions.empty()) {
        auto plast = ParsePatternAndNotes(pblk.instructions.back());
        p_ends_jump = plast.ok() && IsAnyControlTransferPattern(plast->pattern);
      }
      if (p_ends_jump) {
        pblk.instructions.insert(pblk.instructions.end() - 1, use_sexpr);
      } else {
        pblk.instructions.push_back(use_sexpr);
      }
      pblk.SyncRtlInsnsFromInstructions();
      inserted_use = true;
    }
  }
  if (!inserted_use) {
    bool last_blk_is_bare_return = ends_with_ret_jump && !ends_with_ret_seq;
    if (last_blk_is_bare_return) {
      for (size_t k = 0; k + 1 < last_blk.instructions.size(); ++k) {
        auto pk = ParsePatternAndNotes(last_blk.instructions[k]);
        if (!pk.ok() || pk->pattern.op != RtlOp::kNote) {
          last_blk_is_bare_return = false;
          break;
        }
      }
    }
    if (last_blk_is_bare_return && last_blk.has_label &&
        !all_jumpers_to_last_are_annulled_non_t1_steals) {
      if (n_blocks >= 2) {
        RtlBasicBlock& prev_blk = cfg->rpo_blocks[n_blocks - 2];
        const bool has_edge_to_last =
            std::find(prev_blk.successors.begin(), prev_blk.successors.end(), n_blocks - 1) !=
            prev_blk.successors.end();
        bool prev_ends_with_jump = false;
        if (!prev_blk.instructions.empty()) {
          auto prev_last = ParsePatternAndNotes(prev_blk.instructions.back());
          prev_ends_with_jump = prev_last.ok() && IsAnyControlTransferPattern(prev_last->pattern);
        }
        if (has_edge_to_last && prev_blk.successors.size() == 1 && !prev_ends_with_jump) {
          prev_blk.instructions.push_back(std::move(use_sexpr));
          prev_blk.SyncRtlInsnsFromInstructions();
          inserted_use = true;
        } else if (has_edge_to_last && prev_blk.addr_vec_targets.empty() &&
                   prev_blk.successors.size() >= 2 &&
                   !(prev_blk.successors.size() == 2 &&
                     prev_blk.successors[0] == prev_blk.successors[1])) {
          const size_t insert_idx = n_blocks - 1;
          RtlBasicBlock use_blk;
          use_blk.id = static_cast<uint32_t>(insert_idx);
          use_blk.has_label = false;
          use_blk.instructions = {std::move(use_sexpr)};
          use_blk.successors = {insert_idx + 1};
          use_blk.SyncRtlInsnsFromInstructions();
          cfg->rpo_blocks.insert(cfg->rpo_blocks.begin() + static_cast<ptrdiff_t>(insert_idx),
                                 std::move(use_blk));
          cfg->rpo_blocks[insert_idx + 1].id = static_cast<uint32_t>(insert_idx + 1);
          for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
            if (b == insert_idx) {
              continue;
            }
            for (size_t& succ : cfg->rpo_blocks[b].successors) {
              if (succ >= insert_idx) {
                ++succ;
              }
            }
            for (size_t& t : cfg->rpo_blocks[b].addr_vec_targets) {
              if (t >= insert_idx) {
                ++t;
              }
            }
          }
          auto& cond_succs = cfg->rpo_blocks[insert_idx - 1].successors;
          if (!cond_succs.empty()) {
            if (cond_succs[0] == insert_idx + 1 && cond_succs.back() != insert_idx + 1) {
              cond_succs[0] = insert_idx;
            } else {
              cond_succs.back() = insert_idx;
            }
          }
          inserted_use = true;
        }
      }
    } else if (ends_with_ret_jump) {
      last_blk.instructions.insert(last_blk.instructions.end() - 1, std::move(use_sexpr));
      last_blk.SyncRtlInsnsFromInstructions();
      inserted_use = true;
    } else {
      bool last_ends_with_jump = false;
      if (!last_blk.instructions.empty()) {
        auto last_parsed = ParsePatternAndNotes(last_blk.instructions.back());
        last_ends_with_jump = last_parsed.ok() && IsAnyControlTransferPattern(last_parsed->pattern);
      }
      if (last_ends_with_jump) {
        const size_t end_idx = cfg->rpo_blocks.size();
        RtlBasicBlock use_blk;
        use_blk.id = static_cast<uint32_t>(end_idx);
        use_blk.has_label = false;
        use_blk.instructions = {std::move(use_sexpr)};
        use_blk.SyncRtlInsnsFromInstructions();
        last_blk.successors.push_back(end_idx);
        cfg->rpo_blocks.push_back(std::move(use_blk));
      } else {
        last_blk.instructions.push_back(std::move(use_sexpr));
        last_blk.SyncRtlInsnsFromInstructions();
      }
      inserted_use = true;
    }
  }

  if (inserted_use && attach_notes) {
    const DataflowResult updated_df = RunHardRegDataflow(*cfg, &profile);
    WriteLiveInToSideband(updated_df, cfg);
  }
}

struct Stage0StepRef {
  size_t insn_idx = 0;
  bool is_seq = false;
  bool in_slot = false;
  RtlPatternAndNotes* pn = nullptr;
};

struct Stage0BlockParsed {
  std::vector<RtlPatternAndNotes> tops;
  std::vector<RtlExpr> seq_head_wraps;
  std::vector<RtlExpr> seq_slot_wraps;
  std::vector<RtlPatternAndNotes> seq_heads;
  std::vector<RtlPatternAndNotes> seq_slots;
  std::vector<bool> is_seq;
  std::vector<bool> dirty;
  std::vector<Stage0StepRef> steps;
};

void FlushNormalizedStage0BlockInsns(size_t b, const Stage0BlockParsed& bp_b,
                                     RtlControlFlowGraph* cfg) {
  bool any_dirty = false;
  for (size_t i = 0; i < bp_b.tops.size(); ++i) {
    if (!bp_b.dirty[i]) {
      continue;
    }
    any_dirty = true;
    if (bp_b.is_seq[i]) {
      RtlPatternAndNotes h_fmt = bp_b.seq_heads[i];
      if (bp_b.seq_head_wraps[i].op == RtlOp::kCallInsn) {
        h_fmt.reg_notes.clear();
        h_fmt.has_explicit_call_fusage = false;
        h_fmt.call_fusage.clear();
      }
      cfg->rpo_blocks[b].instructions[i] =
          RtlExpr::MakeSequence(
              {
                  RtlExpr::MakeInsnWrapper(bp_b.seq_head_wraps[i].op, bp_b.seq_head_wraps[i].flags,
                                           h_fmt),
                  RtlExpr::MakeInsnWrapper(bp_b.seq_slot_wraps[i].op, bp_b.seq_slot_wraps[i].flags,
                                           bp_b.seq_slots[i]),
              })
              .ToGccPrintRtlString();
    } else {
      cfg->rpo_blocks[b].instructions[i] = FormatInsnWithoutCallFusage(bp_b.tops[i]);
    }
  }
  if (any_dirty) {
    std::vector<std::pair<bool, std::vector<RtlExpr>>> saved_fusage(
        cfg->rpo_blocks[b].rtl_insns.size());
    for (size_t i = 0; i < cfg->rpo_blocks[b].rtl_insns.size(); ++i) {
      saved_fusage[i] = {cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage,
                         cfg->rpo_blocks[b].rtl_insns[i].call_fusage};
    }
    cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < cfg->rpo_blocks[b].rtl_insns.size() && i < saved_fusage.size(); ++i) {
      if (saved_fusage[i].first && !cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage) {
        cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage = true;
        cfg->rpo_blocks[b].rtl_insns[i].call_fusage = std::move(saved_fusage[i].second);
      }
    }
  }
}

bool ExtractStage0SpMem(const RtlExpr& mem, int64_t* out_off, int64_t* out_sz) {
  if (mem.op != RtlOp::kMem || mem.flags.volatil || mem.children.size() != 1) {
    return false;
  }
  int64_t sz = 0;
  switch (mem.mode) {
    case RtlMachineMode::kQi:
      sz = 1;
      break;
    case RtlMachineMode::kHi:
      sz = 2;
      break;
    case RtlMachineMode::kSi:
    case RtlMachineMode::kSf:
      sz = 4;
      break;
    case RtlMachineMode::kDi:
    case RtlMachineMode::kDf:
      sz = 8;
      break;
    default:
      return false;
  }
  const RtlExpr& addr = mem.children[0];
  if (addr.op == RtlOp::kReg && addr.RegNo() == 29) {
    *out_off = 0;
    *out_sz = sz;
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      addr.children[0].RegNo() == 29 && addr.children[1].op == RtlOp::kConstInt &&
      addr.children[1].int_val >= 0) {
    *out_off = addr.children[1].int_val;
    *out_sz = sz;
    return true;
  }
  return false;
}

bool Stage0ExprHasCallOp(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kCall || n.op == RtlOp::kCallInsn) {
      found = true;
    }
  });
  return found;
}

ElemSummary SummarizeStage0Step(const Stage0StepRef& st, const DataflowResult& df) {
  RegMask dummy = 0;
  const absl::flat_hash_set<std::string> empty_jt;
  const bool force_call = st.is_seq && !st.in_slot && Stage0ExprHasCallOp(st.pn->pattern);
  return SummarizeSingleElem(*st.pn, force_call, df.gp64, df.fp64, /*is_unoptimized=*/false,
                             empty_jt, &dummy);
}

int64_t InferStage0EffectiveOutgoingArgsSize(const RtlControlFlowGraph& cfg,
                                             const DataflowResult& df,
                                             const std::vector<Stage0BlockParsed>& bp,
                                             bool found_b0_sp, int64_t frame_non_save_cap) {
  if (cfg.sideband.outgoing_args_size > 0) {
    return cfg.sideband.outgoing_args_size;
  }
  const int64_t min_reg_parm_bytes = df.gp64 ? 32 : 16;
  bool has_call = false;
  int64_t max_call_arg_bytes = 0;
  auto inspect_pat = [&](const RtlExpr& pat) {
    pat.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kCall) {
        has_call = true;
        if (n.children.size() >= 2 && n.children[1].IsConstInt()) {
          max_call_arg_bytes = std::max<int64_t>(max_call_arg_bytes, n.children[1].int_val);
        }
      }
    });
  };
  for (const Stage0BlockParsed& b : bp) {
    for (const Stage0StepRef& st : b.steps) {
      inspect_pat(st.pn->pattern);
    }
  }
  if (cfg.sideband.epilogue_delay_pattern.has_value() &&
      !cfg.sideband.epilogue_delay_pattern->empty()) {
    if (auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern); ep.ok()) {
      inspect_pat(ep->pattern);
    }
  }
  if (!has_call) {
    return 0;
  }
  if (max_call_arg_bytes > 0) {
    return std::max<int64_t>(min_reg_parm_bytes, (max_call_arg_bytes + 7) & ~int64_t{7});
  }
  return (!found_b0_sp || frame_non_save_cap >= min_reg_parm_bytes)
             ? min_reg_parm_bytes
             : std::max<int64_t>(0, frame_non_save_cap);
}

void IdentifyStage0CallerSaveAndSpillOffsets(const RtlControlFlowGraph& cfg,
                                             const DataflowResult& df,
                                             const std::vector<Stage0BlockParsed>& bp,
                                             bool* out_found_b0_sp, size_t* out_b0_sp_step,
                                             absl::flat_hash_set<size_t>* out_prologue_save_steps,
                                             absl::flat_hash_set<int64_t>* caller_save_offsets,
                                             absl::flat_hash_set<int64_t>* proven_spill_offsets) {
  const size_t nb = bp.size();
  int b0_tsize = 0;
  size_t b0_sp_step = 0;
  bool found_b0_sp = false;
  int min_save_off = 0;
  absl::flat_hash_set<size_t>& prologue_save_steps = *out_prologue_save_steps;
  absl::flat_hash_set<int> saved_gprs_set(cfg.sideband.saved_gpr_regnos.begin(),
                                          cfg.sideband.saved_gpr_regnos.end());
  absl::flat_hash_set<int> saved_fprs_set(cfg.sideband.saved_fpr_regnos.begin(),
                                          cfg.sideband.saved_fpr_regnos.end());
  if (!bp.empty()) {
    for (size_t si = 0; si < bp[0].steps.size(); ++si) {
      const RtlExpr& pat = bp[0].steps[si].pn->pattern;
      if (pat.IsSet() && pat.SetDest().IsReg() && pat.SetDest().RegNo() == 29 &&
          pat.SetSrc().op == RtlOp::kPlus && pat.SetSrc().children.size() == 2 &&
          pat.SetSrc().children[0].IsReg() && pat.SetSrc().children[0].RegNo() == 29 &&
          pat.SetSrc().children[1].IsConstInt() && pat.SetSrc().children[1].int_val < 0) {
        b0_tsize = static_cast<int>(-pat.SetSrc().children[1].int_val);
        b0_sp_step = si;
        found_b0_sp = true;
        break;
      }
    }
    min_save_off = b0_tsize;
    if (found_b0_sp) {
      absl::flat_hash_set<int> written_or_saved_regs;
      for (size_t si = b0_sp_step + 1; si < bp[0].steps.size(); ++si) {
        const RtlExpr& pat = bp[0].steps[si].pn->pattern;
        if (Stage0ExprHasCallOp(pat)) {
          break;
        }
        if (IsAnyControlTransferPattern(pat)) {
          if (si + 1 < bp[0].steps.size() && bp[0].steps[si + 1].in_slot &&
              !bp[0].seq_head_wraps[bp[0].steps[si].insn_idx].flags.unchanging) {
            continue;
          }
          break;
        }
        int64_t off = 0, sz = 0;
        if (pat.IsSet() && pat.SetSrc().IsReg() && ExtractStage0SpMem(pat.SetDest(), &off, &sz) &&
            off < b0_tsize) {
          const int sr = pat.SetSrc().RegNo();
          const int nw = HardRegNregs(sr, pat.SetSrc().mode, df.gp64, df.fp64);
          bool already_touched = false;
          for (int w = 0; w < nw; ++w) {
            if (written_or_saved_regs.contains(sr + w)) {
              already_touched = true;
              break;
            }
          }
          if (!already_touched &&
              ((sr >= 16 && sr <= 23) || sr == 30 || sr == 31 || (sr >= 52 && sr <= 63))) {
            prologue_save_steps.insert(si);
            min_save_off = std::min(min_save_off, static_cast<int>(off));
            for (int w = 0; w < nw; ++w) {
              written_or_saved_regs.insert(sr + w);
              if (sr + w < 32) {
                saved_gprs_set.insert(sr + w);
              } else if (sr + w < 64) {
                saved_fprs_set.insert(sr + w);
              }
            }
          }
        }
        const ElemSummary step_sum = SummarizeStage0Step(bp[0].steps[si], df);
        for (int r = 1; r < kMipsFirstPseudoRegister; ++r) {
          if ((step_sum.defs & (RegMask{1} << r)) != 0) {
            written_or_saved_regs.insert(r);
          }
        }
        if (bp[0].steps[si].in_slot) {
          break;
        }
      }
    } else if (cfg.sideband.frame_size_bytes > 0 || cfg.sideband.outgoing_args_size > 0) {
      min_save_off = cfg.sideband.outgoing_args_size + cfg.sideband.frame_size_bytes;
    }
  }
  *out_found_b0_sp = found_b0_sp;
  *out_b0_sp_step = b0_sp_step;

  bool all_saved_gprs_16_23 = true;
  for (int r = 16; r <= 23; ++r) {
    if (!saved_gprs_set.contains(r)) {
      all_saved_gprs_16_23 = false;
      break;
    }
  }
  const bool all_saved_gprs =
      all_saved_gprs_16_23 && (cfg.sideband.frame_pointer_needed || saved_gprs_set.contains(30));
  bool all_saved_fprs = true;
  for (int r = 52; r <= 62; r += 2) {
    if (!saved_fprs_set.contains(r)) {
      all_saved_fprs = false;
      break;
    }
  }

  if (cfg.sideband.frame_pointer_needed || (!found_b0_sp && cfg.sideband.frame_size_bytes <= 0) ||
      min_save_off <= 0) {
    return;
  }

  struct OffInfo {
    int n_stores = 0;
    int n_loads = 0;
    int64_t sz = 0;
    RtlMachineMode mode = RtlMachineMode::kNone;
    bool all_reg_moves = true;
    int uniform_reg = -2;
    std::vector<std::pair<size_t, size_t>> stores;
    std::vector<std::pair<size_t, size_t>> loads;
  };
  absl::flat_hash_map<int64_t, OffInfo> off_map;
  absl::flat_hash_set<int64_t> bad_buckets;
  for (size_t b = 0; b < nb; ++b) {
    for (size_t si = 0; si < bp[b].steps.size(); ++si) {
      if (b == 0 && ((found_b0_sp && si == b0_sp_step) || prologue_save_steps.contains(si))) {
        continue;
      }
      const RtlExpr& pat = bp[b].steps[si].pn->pattern;
      auto check_non_mem_sp = [&](const auto& self, const RtlExpr& e, bool in_mem) -> void {
        if (e.op == RtlOp::kMem) {
          for (const RtlExpr& ch : e.children) self(self, ch, true);
          return;
        }
        if (!in_mem && e.op == RtlOp::kPlus && e.children.size() == 2 && e.children[0].IsReg() &&
            e.children[0].RegNo() == 29 && e.children[1].IsConstInt() &&
            e.children[1].int_val >= 0 && e.children[1].int_val < min_save_off) {
          bad_buckets.insert((e.children[1].int_val / 8) * 8);
        }
        for (const RtlExpr& ch : e.children) self(self, ch, in_mem);
      };
      check_non_mem_sp(check_non_mem_sp, pat, false);
      pat.Visit([&](const RtlExpr& n) {
        if (n.op != RtlOp::kMem) return;
        int64_t off = 0, sz = 0;
        if (!ExtractStage0SpMem(n, &off, &sz)) {
          if (n.children.size() == 1 &&
              ((n.children[0].IsReg() && n.children[0].RegNo() == 29) ||
               (n.children[0].op == RtlOp::kPlus && n.children[0].children.size() == 2 &&
                n.children[0].children[0].IsReg() && n.children[0].children[0].RegNo() == 29))) {
            for (int64_t bk = 0; bk < min_save_off; bk += 8) bad_buckets.insert(bk);
          }
          return;
        }
        if (off >= min_save_off) return;
        const int64_t bk = (off / 8) * 8;
        if (((off + sz - 1) / 8) * 8 != bk) {
          bad_buckets.insert(bk);
          return;
        }
        const bool is_st = pat.IsSet() && &pat.SetDest() == &n && pat.SetSrc().IsReg() &&
                           !pat.SetSrc().flags.integrated;
        const bool is_ld = pat.IsSet() && &pat.SetSrc() == &n && pat.SetDest().IsReg() &&
                           !pat.SetDest().flags.integrated;
        OffInfo& inf = off_map[off];
        if (inf.mode == RtlMachineMode::kNone) {
          inf.mode = n.mode;
          inf.sz = sz;
        } else if (inf.mode != n.mode || inf.sz != sz) {
          bad_buckets.insert(bk);
        }
        if (is_st) {
          ++inf.n_stores;
          inf.stores.emplace_back(b, si);
          const int r = pat.SetSrc().RegNo();
          inf.uniform_reg = (inf.uniform_reg == -2) ? r : (inf.uniform_reg == r ? r : -1);
        } else if (is_ld) {
          ++inf.n_loads;
          inf.loads.emplace_back(b, si);
          const int r = pat.SetDest().RegNo();
          inf.uniform_reg = (inf.uniform_reg == -2) ? r : (inf.uniform_reg == r ? r : -1);
        } else {
          inf.all_reg_moves = false;
          bad_buckets.insert(bk);
        }
      });
    }
  }
  absl::flat_hash_map<int64_t, std::vector<int64_t>> bucket_offs;
  for (const auto& [off, _] : off_map) {
    bucket_offs[(off / 8) * 8].push_back(off);
  }
  for (auto& [bk, offs] : bucket_offs) {
    std::sort(offs.begin(), offs.end());
    for (size_t idx = 0; idx + 1 < offs.size(); ++idx) {
      if (offs[idx] + off_map.at(offs[idx]).sz > offs[idx + 1]) {
        bad_buckets.insert(bk);
        break;
      }
    }
  }
  absl::flat_hash_set<int64_t> valid_cs_offs;
  for (const auto& [off, inf] : off_map) {
    const int64_t bk = (off / 8) * 8;
    const int cs_r = inf.uniform_reg;
    const bool caller_savable =
        (cs_r >= 2 && cs_r <= 15) || cs_r == 24 || cs_r == 25 || (cs_r >= 32 && cs_r <= 51);
    if (bad_buckets.contains(bk) || !inf.all_reg_moves || inf.n_stores < 1 || inf.n_loads < 1 ||
        !caller_savable) {
      continue;
    }
    bool ok_cs = true;
    const bool single_slot_bk = (bucket_offs[bk].size() == 1);
    for (const auto& [b, s_si] : inf.stores) {
      bool saw_call = false;
      for (size_t k = s_si + 1; k < bp[b].steps.size(); ++k) {
        const ElemSummary ks = SummarizeStage0Step(bp[b].steps[k], df);
        if (ks.is_call) {
          if ((ks.uses & (RegMask{1} << cs_r)) != 0) ok_cs = false;
          saw_call = true;
          break;
        }
        if (((ks.uses | ks.defs) & (RegMask{1} << cs_r)) != 0) {
          ok_cs = false;
          break;
        }
      }
      if (!saw_call) {
        ok_cs = false;
        break;
      }
      const bool bank_full = (cs_r < 32) ? all_saved_gprs : all_saved_fprs;
      if (single_slot_bk && bank_full && !(inf.sz == 4 && (off % 8) == 0)) {
        bool read_earlier = false;
        for (size_t p = s_si; p > 0; --p) {
          const ElemSummary ps = SummarizeStage0Step(bp[b].steps[p - 1], df);
          if ((ps.uses & (RegMask{1} << cs_r)) != 0 && (ps.defs & (RegMask{1} << cs_r)) == 0) {
            read_earlier = true;
            break;
          }
          if ((ps.defs & (RegMask{1} << cs_r)) != 0) break;
        }
        if (!read_earlier && (df.live_in[b] & (RegMask{1} << cs_r)) == 0) {
          ok_cs = false;
          break;
        }
      }
    }
    for (const auto& [b, l_si] : inf.loads) {
      if (!ok_cs) break;
      bool saw_call = false;
      for (size_t p = l_si; p > 0; --p) {
        const ElemSummary ps = SummarizeStage0Step(bp[b].steps[p - 1], df);
        if (ps.is_call) {
          if ((ps.explicit_sets & (RegMask{1} << cs_r)) != 0) ok_cs = false;
          saw_call = true;
          break;
        }
        if (((ps.uses | ps.defs) & (RegMask{1} << cs_r)) != 0) {
          ok_cs = false;
          break;
        }
      }
      if (!saw_call) ok_cs = false;
    }
    if (ok_cs) {
      valid_cs_offs.insert(off);
    }
  }
  const int64_t prologue_bk = (min_save_off / 8) * 8;
  int64_t min_cs_bk = prologue_bk;
  int64_t max_cs_bk = -1;
  for (const auto& [bk, offs] : bucket_offs) {
    if (bad_buckets.contains(bk) || offs.empty()) {
      continue;
    }
    const bool all_in_bk =
        std::all_of(offs.begin(), offs.end(), [&](int64_t o) { return valid_cs_offs.contains(o); });
    if (all_in_bk) {
      for (int64_t o : offs) {
        caller_save_offsets->insert(o);
      }
      min_cs_bk = std::min(min_cs_bk, bk);
      max_cs_bk = std::max(max_cs_bk, bk);
    }
  }
  auto try_add_spill_bucket = [&](int64_t bk) -> bool {
    if (bad_buckets.contains(bk)) {
      return false;
    }
    const auto bit = bucket_offs.find(bk);
    if (bit == bucket_offs.end()) {
      return all_saved_gprs || all_saved_fprs || all_saved_gprs_16_23;
    }
    if (bit->second.size() != 1) {
      return false;
    }
    const int64_t off = bit->second[0];
    const OffInfo& inf = off_map.at(off);
    if (!inf.all_reg_moves || inf.n_stores < 1 || inf.n_loads < 1 || off != bk + (8 - inf.sz)) {
      return false;
    }
    const bool is_fp = (inf.mode == RtlMachineMode::kSf || inf.mode == RtlMachineMode::kDf);
    const bool is_di_gpr = (!df.gp64 && inf.mode == RtlMachineMode::kDi);
    if (is_fp ? !all_saved_fprs : (is_di_gpr ? !all_saved_gprs_16_23 : !all_saved_gprs)) {
      return false;
    }
    proven_spill_offsets->insert(off);
    return true;
  };
  // Zone 1 (`reload1.c:658, 879, 2352`): pre-`setup_save_areas` `alter_reg`
  // spill slots (`align = -1` -> 8-byte bucket, big-endian `bk + (8 - sz)`).
  const int64_t eff_outgoing_args =
      InferStage0EffectiveOutgoingArgsSize(cfg, df, bp, found_b0_sp, min_save_off);
  const int64_t min_spill_bk = ((std::max<int64_t>(0, eff_outgoing_args) + 7) / 8) * 8;
  for (int64_t bk = min_cs_bk - 8; bk >= min_spill_bk; bk -= 8) {
    if (!try_add_spill_bucket(bk)) {
      break;
    }
  }
  // Zone 3 (`reload1.c:1446, 3499, 2352`): post-`setup_save_areas` reload-loop
  // `spill_hard_reg` -> `alter_reg` spill slots above caller-save slots and
  // below the prologue register-save area.
  if (max_cs_bk >= 0) {
    for (int64_t bk = max_cs_bk + 8; bk < prologue_bk; bk += 8) {
      if (!try_add_spill_bucket(bk)) {
        break;
      }
    }
  }
}

const RtlExpr* ExtractStage0SingleSet(const RtlExpr& pat) {
  if (pat.IsSet()) {
    return &pat;
  }
  if (pat.op == RtlOp::kParallel) {
    const RtlExpr* found = nullptr;
    for (const RtlExpr& elem : pat.children) {
      if (elem.IsSet()) {
        if (found != nullptr) {
          return nullptr;
        }
        found = &elem;
      } else if (elem.op != RtlOp::kClobber && elem.op != RtlOp::kUse) {
        return nullptr;
      }
    }
    return found;
  }
  return nullptr;
}

bool HasStage0LibcallOrRetvalNote(const Stage0StepRef& st) {
  for (const RtlRegNote& n : st.pn->reg_notes) {
    if (n.kind == RtlMachineMode::kRegLibcall || n.kind == RtlMachineMode::kRegRetval) {
      return true;
    }
  }
  return false;
}

bool HasStage0OnlyUnpinnedReadsOfMask(const RtlExpr& pat, const DataflowResult& df, int hr,
                                      RtlMachineMode req_mode, RegMask hr_m) {
  if (pat.op == RtlOp::kUse) {
    return false;
  }
  bool ok = true;
  auto walk = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
    if (!ok) {
      return;
    }
    if (e.op == RtlOp::kAsmOperands) {
      ok = false;
      return;
    }
    if (e.op == RtlOp::kClobber) {
      return;
    }
    if (e.IsSet()) {
      if (!e.SetDest().IsReg()) {
        self(self, e.SetDest(), /*in_dst=*/true);
      }
      self(self, e.SetSrc(), /*in_dst=*/false);
      return;
    }
    if (in_dst && e.op == RtlOp::kMem) {
      for (const RtlExpr& ch : e.children) {
        self(self, ch, /*in_dst=*/false);
      }
      return;
    }
    if (e.IsHardReg() && !e.IsArgPointerReg() && e.RegNo() >= 0 && e.RegNo() < 64) {
      const RegMask em = RegBitMask(e.RegNo(), e.mode, df.gp64, df.fp64);
      if ((em & hr_m) != 0) {
        const bool compatible_subword_read =
            req_mode == RtlMachineMode::kSi &&
            (e.mode == RtlMachineMode::kQi || e.mode == RtlMachineMode::kHi);
        if (in_dst || e.RegNo() != hr || (e.mode != req_mode && !compatible_subword_read) ||
            e.flags.volatil || e.flags.integrated) {
          ok = false;
        }
      }
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch, in_dst);
    }
  };
  walk(walk, pat, /*in_dst=*/false);
  return ok;
}

template <typename Fn>
void ForEachStage0CfgSucc(const RtlBasicBlock& blk, size_t nb, Fn&& fn) {
  for (size_t succ : blk.successors) {
    if (succ < nb) {
      fn(succ);
    }
  }
  for (size_t target : blk.addr_vec_targets) {
    if (target < nb &&
        std::find(blk.successors.begin(), blk.successors.end(), target) == blk.successors.end()) {
      fn(target);
    }
  }
}

bool IsStage0AnnulledUntakenTailSlot(const RtlControlFlowGraph& cfg,
                                     const std::vector<Stage0BlockParsed>& all_bp, size_t blk_i,
                                     const Stage0StepRef& st) {
  return st.in_slot && st.insn_idx + 1 == cfg.rpo_blocks[blk_i].instructions.size() &&
         cfg.rpo_blocks[blk_i].successors.size() >= 2 &&
         all_bp[blk_i].seq_head_wraps[st.insn_idx].op == RtlOp::kJumpInsn &&
         all_bp[blk_i].seq_head_wraps[st.insn_idx].flags.unchanging &&
         all_bp[blk_i].seq_slot_wraps[st.insn_idx].flags.in_struct;
}

bool IsStage0ReorgOppositeReturnSlotComp(const RtlControlFlowGraph& cfg, size_t b,
                                         const RtlExpr& slot_pat) {
  const size_t nb = cfg.rpo_blocks.size();
  if (b == 0 || b + 1 != nb || cfg.rpo_blocks[b].has_label ||
      !cfg.rpo_blocks[b].successors.empty() || !slot_pat.IsSet() || !slot_pat.SetDest().IsReg()) {
    return false;
  }
  const RtlBasicBlock& prev_blk = cfg.rpo_blocks[b - 1];
  if (prev_blk.instructions.empty() ||
      std::find(prev_blk.successors.begin(), prev_blk.successors.end(), b) ==
          prev_blk.successors.end()) {
    return false;
  }
  auto prev_seq = ParsePatternAndNotes(prev_blk.instructions.back());
  if (!prev_seq.ok() || prev_seq->pattern.op != RtlOp::kSequence ||
      prev_seq->pattern.children.size() != 2 || prev_seq->pattern.children[0].flags.unchanging) {
    return false;
  }
  auto jump_pn = RtlPatternAndNotes::FromSequenceSubInsn(prev_seq->pattern.children[0]);
  auto eager_pn = RtlPatternAndNotes::FromSequenceSubInsn(prev_seq->pattern.children[1]);
  if (!jump_pn.ok() || !eager_pn.ok() || !jump_pn->pattern.IsSet() ||
      jump_pn->pattern.SetDest().op != RtlOp::kPc ||
      jump_pn->pattern.SetSrc().op != RtlOp::kIfThenElse || !eager_pn->pattern.IsSet() ||
      !eager_pn->pattern.SetDest().IsReg()) {
    return false;
  }
  const int hr = slot_pat.SetDest().RegNo();
  if (eager_pn->pattern.SetDest().RegNo() != hr ||
      eager_pn->pattern.SetDest().mode != slot_pat.SetDest().mode) {
    return false;
  }
  const RtlExpr& esrc = eager_pn->pattern.SetSrc();
  const RtlExpr& ssrc = slot_pat.SetSrc();
  if ((esrc.op != RtlOp::kPlus && esrc.op != RtlOp::kMinus) ||
      (ssrc.op != RtlOp::kPlus && ssrc.op != RtlOp::kMinus) || esrc.children.size() != 2 ||
      ssrc.children.size() != 2 || !esrc.children[0].IsReg() || !ssrc.children[0].IsReg() ||
      esrc.children[0].RegNo() != hr || ssrc.children[0].RegNo() != hr) {
    return false;
  }
  if (esrc.children[1].IsConstInt() && ssrc.children[1].IsConstInt()) {
    return (esrc.op == ssrc.op && esrc.children[1].int_val == -ssrc.children[1].int_val) ||
           (esrc.op != ssrc.op && esrc.children[1].int_val == ssrc.children[1].int_val);
  }
  if (esrc.op != ssrc.op) {
    RtlExpr erhs = esrc.children[1];
    RtlExpr srhs = ssrc.children[1];
    auto strip_flags = [](RtlExpr& n) { n.flags = RtlFlags{}; };
    erhs.Mutate(strip_flags);
    srhs.Mutate(strip_flags);
    return erhs == srhs;
  }
  return false;
}

void ComputeStage0EntryAbiArgUnmodifiedReach(const RtlControlFlowGraph& cfg,
                                             const DataflowResult& df,
                                             const std::vector<Stage0BlockParsed>& all_bp,
                                             const std::vector<std::vector<size_t>>& block_preds,
                                             RegMask hr_m, std::vector<bool>* any_unmod_in,
                                             std::vector<bool>* any_unmod_out,
                                             std::vector<bool>* all_unmod_in) {
  const size_t nb = cfg.rpo_blocks.size();
  std::vector<bool> block_no_def(nb, true);
  for (size_t x = 0; x < nb; ++x) {
    for (const Stage0StepRef& st : all_bp[x].steps) {
      const ElemSummary es = SummarizeStage0Step(st, df);
      if ((es.defs & hr_m) != 0 || HasStage0LibcallOrRetvalNote(st)) {
        block_no_def[x] = false;
        break;
      }
    }
  }
  any_unmod_in->assign(nb, false);
  any_unmod_out->assign(nb, false);
  if (nb == 0 || !block_preds[0].empty()) {
    all_unmod_in->assign(nb, false);
    return;
  }
  (*any_unmod_in)[0] = true;
  (*any_unmod_out)[0] = block_no_def[0];
  bool chg = true;
  while (chg) {
    chg = false;
    for (size_t x = 1; x < nb; ++x) {
      bool in_v = false;
      for (size_t pred : block_preds[x]) {
        if ((*any_unmod_out)[pred]) {
          in_v = true;
          break;
        }
      }
      const bool out_v = in_v && block_no_def[x];
      if (in_v != (*any_unmod_in)[x] || out_v != (*any_unmod_out)[x]) {
        (*any_unmod_in)[x] = in_v;
        (*any_unmod_out)[x] = out_v;
        chg = true;
      }
    }
  }
  all_unmod_in->assign(nb, true);
  std::vector<bool> all_unmod_out(nb, false);
  for (size_t x = 0; x < nb; ++x) {
    if (x > 0 && block_preds[x].empty()) {
      (*all_unmod_in)[x] = false;
    }
    all_unmod_out[x] = (*all_unmod_in)[x] && block_no_def[x];
  }
  chg = true;
  while (chg) {
    chg = false;
    for (size_t x = 1; x < nb; ++x) {
      bool in_v = !block_preds[x].empty();
      for (size_t pred : block_preds[x]) {
        if (!all_unmod_out[pred]) {
          in_v = false;
          break;
        }
      }
      const bool out_v = in_v && block_no_def[x];
      if (in_v != (*all_unmod_in)[x] || out_v != all_unmod_out[x]) {
        (*all_unmod_in)[x] = in_v;
        all_unmod_out[x] = out_v;
        chg = true;
      }
    }
  }
}

bool IsStage0ConstantReloadSrc(const RtlExpr& src) {
  return src.IsConstInt() || src.op == RtlOp::kConstDouble || src.op == RtlOp::kSymbolRef ||
         src.op == RtlOp::kLabelRef || src.op == RtlOp::kConst || src.op == RtlOp::kHigh;
}

// Mirrors `reload1.c:5641-6155` (`emit_reload_insns`): returns true when `r`
// (`SF`/`DF` FPR) at step `s` was loaded by an earlier cross-bank `"f"` input
// reload `(set (reg:SF/DF r) (reg:SF/DF gpr))` from a `gpr` that remains live
// after `s` (`(after_mask & gpr_m) != 0`) with no `REG_DEAD gpr` on the reload
// and no intervening touch of `gpr` or `r`. Such reload scratches never receive
// a `REG_DEAD r` note on the FP consumer.
bool IsLiveGprCrossBankFprReloadStep(const std::vector<Stage0StepRef>& steps,
                                     const std::vector<ElemSummary>& esums,
                                     const DataflowResult& df, size_t s, int r, RtlMachineMode m,
                                     RegMask rm, RegMask after_mask) {
  if (r < 32 || r >= 64 || (m != RtlMachineMode::kSf && m != RtlMachineMode::kDf) || s == 0) {
    return false;
  }
  for (size_t prev = s; prev > 0; --prev) {
    const size_t pi = prev - 1;
    const RtlExpr& ppat = steps[pi].pn->pattern;
    if (ppat.IsSet() && ppat.SetDest().IsReg() && !ppat.SetDest().flags.integrated &&
        ppat.SetDest().mode == m && ppat.SetDest().RegNo() == r && ppat.SetSrc().IsReg() &&
        ppat.SetSrc().mode == m && ppat.SetSrc().RegNo() >= 2 && ppat.SetSrc().RegNo() < 32) {
      const int gpr_r = ppat.SetSrc().RegNo();
      const RegMask gpr_m = RegBitMask(gpr_r, m, df.gp64, df.fp64);
      for (size_t k = pi + 1; k < s; ++k) {
        if (((esums[k].uses | esums[k].defs) & gpr_m) != 0) {
          return false;
        }
      }
      return (after_mask & gpr_m) != 0 &&
             !HasRegNote(steps[pi].pn->reg_notes, RtlMachineMode::kRegDead, gpr_r);
    }
    if (esums[pi].is_call || IsAnyControlTransferPattern(ppat) ||
        ((esums[pi].uses | esums[pi].defs) & rm) != 0) {
      return false;
    }
  }
  return false;
}

struct Stage0RmwCtx {
  const RtlControlFlowGraph& cfg;
  const DataflowResult& df;
  size_t b = 0;
  const std::vector<Stage0BlockParsed>& all_bp;
  const std::vector<ElemSummary>& esums;
  const std::vector<RegMask>& live_after_step;
  const absl::flat_hash_set<int64_t>& caller_save_offsets;
  const absl::flat_hash_set<int64_t>& proven_spill_offsets;
  RegMask fixed_mask = 0;
  std::vector<std::vector<size_t>> block_preds;
  Stage0BlockParsed* blk_parsed = nullptr;

  const std::vector<Stage0StepRef>& StepsOf(size_t blk) const {
    return (blk == b) ? blk_parsed->steps : all_bp[blk].steps;
  }

  ElemSummary SummaryOf(size_t blk, size_t idx) const {
    return (blk == b) ? esums[idx] : SummarizeStage0Step(all_bp[blk].steps[idx], df);
  }

  bool IsSpillOrCsLoadExpr(const RtlExpr& src) const {
    int64_t off = 0, sz = 0;
    return ExtractStage0SpMem(src, &off, &sz) &&
           (caller_save_offsets.contains(off) || proven_spill_offsets.contains(off));
  }

  bool IsSpillOutputReloadBeforeStep(size_t def_step, size_t use_step, int hr,
                                     RtlMachineMode mode) const {
    const auto& steps = blk_parsed->steps;
    for (size_t q = def_step + 1; q < use_step; ++q) {
      const RtlExpr& qpat = steps[q].pn->pattern;
      if (qpat.IsSet() && qpat.SetSrc().IsHardReg() && qpat.SetSrc().RegNo() == hr &&
          qpat.SetSrc().mode == mode) {
        int64_t off = 0, sz = 0;
        if (ExtractStage0SpMem(qpat.SetDest(), &off, &sz) && proven_spill_offsets.contains(off)) {
          return true;
        }
      }
    }
    return false;
  }

  bool IsUnpackedAnnulledFallthroughTailInDbr(size_t s) const {
    const auto& steps = blk_parsed->steps;
    const size_t ns = steps.size();
    const size_t nb = cfg.rpo_blocks.size();
    if (cfg.pass != RtlPassKind::kDbr || steps[s].in_slot || s + 1 != ns || b == 0 || b + 1 >= nb ||
        cfg.rpo_blocks[b].has_label || cfg.rpo_blocks[b].successors.size() != 1 ||
        cfg.rpo_blocks[b].successors[0] != b + 1) {
      return false;
    }
    const RtlExpr& pat = steps[s].pn->pattern;
    if (!pat.IsSet() || IsAnyControlTransferPattern(pat) || esums[s].is_call) {
      return false;
    }
    bool bad_mem_or_vol = false;
    pat.Visit([&](const RtlExpr& n) {
      if (n.flags.volatil || (n.op == RtlOp::kMem && &n != &pat.SetDest())) {
        bad_mem_or_vol = true;
      }
    });
    if (bad_mem_or_vol) {
      return false;
    }
    const auto& prev_succs = cfg.rpo_blocks[b - 1].successors;
    if (prev_succs.size() != 2 || !((prev_succs[0] == b + 1 && prev_succs[1] == b) ||
                                    (prev_succs[0] == b && prev_succs[1] == b + 1))) {
      return false;
    }
    const Stage0BlockParsed& prev_bp = all_bp[b - 1];
    if (prev_bp.is_seq.empty() || !prev_bp.is_seq.back()) {
      return false;
    }
    return prev_bp.seq_head_wraps.back().op == RtlOp::kJumpInsn &&
           prev_bp.seq_head_wraps.back().flags.unchanging &&
           prev_bp.seq_slot_wraps.back().flags.in_struct;
  }

  void AttachDeadNote(size_t s, int hr, const RtlExpr& datum) const {
    auto& steps = blk_parsed->steps;
    const size_t ns = steps.size();
    const size_t nb = cfg.rpo_blocks.size();
    const RtlExpr& pat = steps[s].pn->pattern;
    if (steps[s].in_slot) {
      const bool has_seq_head = s + 1 < ns && steps[s + 1].insn_idx == steps[s].insn_idx;
      const bool slot_in_struct = blk_parsed->seq_slot_wraps[steps[s].insn_idx].flags.in_struct;
      RegMask after_mask = live_after_step[s];
      if (has_seq_head && esums[s + 1].is_call) {
        const RegMask hr_m = RegBitMask(hr, datum.mode, df.gp64, df.fp64);
        after_mask = (live_after_step[s + 1] & ~esums[s + 1].defs) | (esums[s + 1].uses & ~hr_m);
      } else if (slot_in_struct && has_seq_head && cfg.rpo_blocks[b].successors.size() == 2 &&
                 b + 1 < nb) {
        RegMask taken_live = df.can_exit[b] ? df.live_at_exit : RegMask{0};
        for (size_t sb : cfg.rpo_blocks[b].successors) {
          if (sb != b + 1 && sb < nb) {
            taken_live |= df.live_in[sb];
          }
        }
        after_mask = taken_live;
      }
      const RegMask non_set_dying = esums[s].uses & ~esums[s].defs & ~after_mask & ~fixed_mask;
      size_t non_set_dying_notes = 0;
      RegMask rem = non_set_dying;
      while (rem != 0) {
        int r = 0;
        while (((rem >> r) & RegMask{1}) == 0) ++r;
        RtlMachineMode m = RtlMachineMode::kSi;
        pat.Visit([&](const RtlExpr& n) {
          if (n.IsHardReg() && n.RegNo() == r) m = n.mode;
        });
        const RegMask rm = RegBitMask(r, m, df.gp64, df.fp64);
        rem &= ~rm;
        if (!IsLiveGprCrossBankFprReloadStep(steps, esums, df, s, r, m, rm, after_mask)) {
          ++non_set_dying_notes;
        }
      }
      if (CountConstZeroRegDeadNotes(steps[s].pn->reg_notes) <= non_set_dying_notes) {
        steps[s].pn->reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegDead,
            .list_op = RtlOp::kExprList,
            .datum = RtlExpr::MakeConstInt(0),
        });
        blk_parsed->dirty[steps[s].insn_idx] = true;
      }
    } else if (IsUnpackedAnnulledFallthroughTailInDbr(s)) {
      if (CountConstZeroRegDeadNotes(steps[s].pn->reg_notes) == 0) {
        steps[s].pn->reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegDead,
            .list_op = RtlOp::kExprList,
            .datum = RtlExpr::MakeConstInt(0),
        });
        blk_parsed->dirty[steps[s].insn_idx] = true;
      }
    } else if (!HasRegNote(steps[s].pn->reg_notes, RtlMachineMode::kRegDead, hr)) {
      auto read_preorder_idx = [&](const RtlExpr& note_datum) -> int {
        int cur = 0;
        int hit = -1;
        auto walk_reads = [&](const auto& self, const RtlExpr& e, bool is_set_dst) -> void {
          if (hit >= 0) {
            return;
          }
          if (e.op == RtlOp::kSet && e.children.size() == 2) {
            if (e.children[0].op == RtlOp::kMem) {
              self(self, e.children[0], false);
            }
            self(self, e.children[1], false);
            return;
          }
          if (e.op == RtlOp::kClobber) {
            if (!e.children.empty() && e.children[0].op == RtlOp::kMem) {
              self(self, e.children[0], false);
            }
            return;
          }
          if (!is_set_dst && e.IsHardReg() && !e.IsArgPointerReg()) {
            if (note_datum.IsHardReg() && !note_datum.IsArgPointerReg() &&
                e.RegNo() == note_datum.RegNo()) {
              hit = cur;
              return;
            }
            ++cur;
            return;
          }
          for (const RtlExpr& ch : e.children) {
            self(self, ch, is_set_dst);
          }
        };
        walk_reads(walk_reads, pat, false);
        if (hit >= 0) {
          return hit;
        }
        for (size_t rev = s; rev > 0; --rev) {
          const RtlExpr& ppat = steps[rev - 1].pn->pattern;
          if (ppat.IsSet() && ppat.SetDest().IsHardReg() && !ppat.SetDest().IsArgPointerReg() &&
              ppat.SetSrc() == note_datum) {
            cur = 0;
            hit = -1;
            const RtlExpr proxy = ppat.SetDest();
            auto walk_proxy = [&](const auto& self, const RtlExpr& e) -> void {
              if (hit >= 0) return;
              if (e.op == RtlOp::kSet && e.children.size() == 2) {
                if (e.children[0].op == RtlOp::kMem) self(self, e.children[0]);
                self(self, e.children[1]);
                return;
              }
              if (e.op == RtlOp::kClobber) return;
              if (e.IsHardReg() && !e.IsArgPointerReg()) {
                if (e.RegNo() == proxy.RegNo()) {
                  hit = cur;
                  return;
                }
                ++cur;
                return;
              }
              for (const RtlExpr& ch : e.children) self(self, ch);
            };
            walk_proxy(walk_proxy, pat);
            if (hit >= 0) {
              return hit;
            }
            break;
          }
        }
        return 10000;
      };
      auto& notes = steps[s].pn->reg_notes;
      const int my_ord = read_preorder_idx(datum);
      auto ins_it = std::find_if(notes.begin(), notes.end(), [&](const RtlRegNote& ex) {
        if (ex.kind != RtlMachineMode::kRegDead) {
          return true;
        }
        return read_preorder_idx(ex.datum) > my_ord;
      });
      notes.insert(ins_it, RtlRegNote{
                               .kind = RtlMachineMode::kRegDead,
                               .list_op = RtlOp::kExprList,
                               .datum = datum,
                           });
      blk_parsed->dirty[steps[s].insn_idx] = true;
    }
  }
};

std::vector<bool> ComputeTransitiveForbidMergeOut(const Stage0RmwCtx& ctx, size_t start_b, int hr,
                                                  RtlMachineMode mode, RegMask hr_m,
                                                  const std::vector<bool>& base_forbid) {
  const size_t nb = ctx.cfg.rpo_blocks.size();
  std::vector<bool> forbid = base_forbid;
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t u = 0; u < nb; ++u) {
      if (u == start_b || forbid[u] || (ctx.df.live_in[u] & hr_m) == 0 ||
          (ctx.df.live_out[u] & hr_m) == 0) {
        continue;
      }
      bool has_forbid_pred = false;
      for (size_t pred : ctx.block_preds[u]) {
        if (pred != start_b && forbid[pred]) {
          has_forbid_pred = true;
          break;
        }
      }
      if (!has_forbid_pred) {
        continue;
      }
      const auto& u_steps = ctx.StepsOf(u);
      bool survives_in_place = true;
      for (size_t si = 0; si < u_steps.size(); ++si) {
        const Stage0StepRef& st = u_steps[si];
        if (IsStage0AnnulledUntakenTailSlot(ctx.cfg, ctx.all_bp, u, st)) {
          continue;
        }
        const ElemSummary ses = ctx.SummaryOf(u, si);
        if ((ses.defs & hr_m) == 0) {
          continue;
        }
        const RtlExpr* ss = ExtractStage0SingleSet(st.pn->pattern);
        const bool in_place = !ses.is_call && (ses.defs & hr_m) == hr_m && (ses.uses & hr_m) != 0 &&
                              ss != nullptr && ss->SetDest().IsHardReg() &&
                              ss->SetDest().RegNo() == hr && ss->SetDest().mode == mode &&
                              !ss->SetDest().flags.volatil && !ss->SetDest().flags.integrated &&
                              !HasRegNote(st.pn->reg_notes, RtlMachineMode::kRegDead, hr);
        if (!in_place) {
          survives_in_place = false;
          break;
        }
      }
      if (survives_in_place) {
        forbid[u] = true;
        changed = true;
      }
    }
  }
  return forbid;
}

bool IsStage0UnpinnedOutWebFromStep(const Stage0RmwCtx& ctx, size_t start_b, size_t s_after, int hr,
                                    RtlMachineMode mode, RegMask hr_m, bool require_global,
                                    const std::vector<bool>* forbid_merge_unmod_out) {
  const size_t nb = ctx.cfg.rpo_blocks.size();
  const auto& start_steps = ctx.StepsOf(start_b);
  bool killed_in_start = false;
  bool saw_read = false;
  for (size_t k = s_after; k < start_steps.size(); ++k) {
    const Stage0StepRef& kst = start_steps[k];
    const ElemSummary kes = ctx.SummaryOf(start_b, k);
    if (HasStage0LibcallOrRetvalNote(kst) || (kes.is_call && (kes.uses & hr_m) != 0)) {
      return false;
    }
    if ((kes.uses & hr_m) != 0) {
      if (!HasStage0OnlyUnpinnedReadsOfMask(kst.pn->pattern, ctx.df, hr, mode, hr_m)) {
        return false;
      }
      saw_read = true;
    }
    if ((kes.defs & hr_m) != 0) {
      if (require_global) {
        return false;
      }
      killed_in_start = true;
      break;
    }
  }
  if (killed_in_start) {
    return !require_global && saw_read;
  }
  std::vector<bool> trans_forbid;
  if (forbid_merge_unmod_out != nullptr) {
    trans_forbid =
        ComputeTransitiveForbidMergeOut(ctx, start_b, hr, mode, hr_m, *forbid_merge_unmod_out);
  }
  bool live_at_other_succ = false;
  std::vector<size_t> q;
  std::vector<bool> vis(nb, false);
  ForEachStage0CfgSucc(ctx.cfg.rpo_blocks[start_b], nb, [&](size_t succ) {
    if ((ctx.df.live_in[succ] & hr_m) == 0) {
      return;
    }
    if (succ != start_b) {
      live_at_other_succ = true;
    }
    if (!vis[succ]) {
      vis[succ] = true;
      q.push_back(succ);
    }
  });
  if (require_global && !live_at_other_succ) {
    return false;
  }
  for (size_t qi = 0; qi < q.size(); ++qi) {
    const size_t sb = q[qi];
    if (!trans_forbid.empty()) {
      for (size_t pred : ctx.block_preds[sb]) {
        if (pred != start_b && trans_forbid[pred]) {
          return false;
        }
      }
    }
    const auto& sb_steps = ctx.StepsOf(sb);
    bool killed = false;
    bool killed_taken_only = false;
    for (size_t si = 0; si < sb_steps.size(); ++si) {
      const Stage0StepRef& st = sb_steps[si];
      const ElemSummary ses = ctx.SummaryOf(sb, si);
      const bool annulled_slot = IsStage0AnnulledUntakenTailSlot(ctx.cfg, ctx.all_bp, sb, st);
      if (HasStage0LibcallOrRetvalNote(st) || (ses.is_call && (ses.uses & hr_m) != 0)) {
        return false;
      }
      if ((ses.uses & hr_m) != 0) {
        if (!HasStage0OnlyUnpinnedReadsOfMask(st.pn->pattern, ctx.df, hr, mode, hr_m)) {
          return false;
        }
        saw_read = true;
      }
      if ((ses.defs & hr_m) != 0) {
        if ((ses.defs & hr_m) != hr_m) {
          return false;
        }
        if (annulled_slot) {
          killed_taken_only = true;
          continue;
        }
        if (st.in_slot && ctx.all_bp[sb].seq_slot_wraps[st.insn_idx].flags.in_struct) {
          return false;
        }
        const RtlExpr* ss = ExtractStage0SingleSet(st.pn->pattern);
        const bool in_place_rmw = (ses.uses & hr_m) != 0 && ss != nullptr &&
                                  ss->SetDest().IsHardReg() && ss->SetDest().RegNo() == hr &&
                                  ss->SetDest().mode == mode && !ss->SetDest().flags.volatil &&
                                  !ss->SetDest().flags.integrated;
        if (!in_place_rmw) {
          killed = true;
          break;
        }
      }
    }
    if (!killed) {
      if (ctx.df.can_exit[sb] && (hr == 2 || hr == 32) &&
          HasDeletedSharedReturnMoveWitnessWithDataflow(ctx.cfg, ctx.df, hr, mode)) {
        saw_read = true;
      }
      ForEachStage0CfgSucc(ctx.cfg.rpo_blocks[sb], nb, [&](size_t nx) {
        if (killed_taken_only && nx != sb + 1) {
          return;
        }
        if ((ctx.df.live_in[nx] & hr_m) != 0 && !vis[nx]) {
          vis[nx] = true;
          q.push_back(nx);
        }
      });
    }
  }
  return saw_read;
}

// Returns true when the definition at `(def_b, def_step)` can reach any outgoing
// `call_insn` `CALL_INSN_FUNCTION_USAGE` read of `hr_m` along a def-clear path.
// When true, the defining web is ABI-pinned to hard register `hr` (never split
// into an independent pseudo), so a downstream in-place RMW of `hr` reads and
// writes the same hard register and must not carry `REG_DEAD hr` (`flow.c:2506-2515`
// `!dead_or_set_p (insn, x)`, `sched.c:2872`).
bool Stage0DefReachesPinnedCallArgUse(const Stage0RmwCtx& ctx, size_t def_b, size_t def_step,
                                      RegMask hr_m) {
  const size_t nb = ctx.cfg.rpo_blocks.size();
  const auto& def_steps = ctx.StepsOf(def_b);
  const bool def_is_annulled_slot =
      def_step < def_steps.size() &&
      IsStage0AnnulledUntakenTailSlot(ctx.cfg, ctx.all_bp, def_b, def_steps[def_step]);
  bool killed_in_def_b = false;
  bool killed_taken_only = false;
  for (size_t k = def_step + 1; k < def_steps.size(); ++k) {
    const ElemSummary kes = ctx.SummaryOf(def_b, k);
    if (kes.is_call && (kes.uses & hr_m) != 0) {
      return true;
    }
    if ((kes.defs & hr_m) != 0) {
      if (IsStage0AnnulledUntakenTailSlot(ctx.cfg, ctx.all_bp, def_b, def_steps[k])) {
        killed_taken_only = true;
        continue;
      }
      killed_in_def_b = true;
      break;
    }
  }
  if (killed_in_def_b) {
    return false;
  }
  std::vector<size_t> q;
  std::vector<bool> vis(nb, false);
  ForEachStage0CfgSucc(ctx.cfg.rpo_blocks[def_b], nb, [&](size_t succ) {
    if (def_is_annulled_slot && succ == def_b + 1) {
      return;
    }
    if (killed_taken_only && succ != def_b + 1) {
      return;
    }
    if ((ctx.df.live_in[succ] & hr_m) != 0 && !vis[succ]) {
      vis[succ] = true;
      q.push_back(succ);
    }
  });
  for (size_t qi = 0; qi < q.size(); ++qi) {
    const size_t sb = q[qi];
    const auto& sb_steps = ctx.StepsOf(sb);
    bool killed = false;
    bool sb_killed_taken_only = false;
    for (size_t si = 0; si < sb_steps.size(); ++si) {
      const ElemSummary ses = ctx.SummaryOf(sb, si);
      if (ses.is_call && (ses.uses & hr_m) != 0) {
        return true;
      }
      if ((ses.defs & hr_m) != 0) {
        if (IsStage0AnnulledUntakenTailSlot(ctx.cfg, ctx.all_bp, sb, sb_steps[si])) {
          sb_killed_taken_only = true;
          continue;
        }
        killed = true;
        break;
      }
    }
    if (!killed) {
      ForEachStage0CfgSucc(ctx.cfg.rpo_blocks[sb], nb, [&](size_t nx) {
        if (sb_killed_taken_only && nx != sb + 1) {
          return;
        }
        if ((ctx.df.live_in[nx] & hr_m) != 0 && !vis[nx]) {
          vis[nx] = true;
          q.push_back(nx);
        }
      });
    }
  }
  return false;
}

bool HasStage0UnpinnedGlobalInWeb(const Stage0RmwCtx& ctx, size_t target_b, int hr,
                                  RtlMachineMode req_mode, RegMask hr_m,
                                  bool* out_all_narrow_mem_loads = nullptr) {
  const size_t nb = ctx.cfg.rpo_blocks.size();
  if (out_all_narrow_mem_loads != nullptr) {
    *out_all_narrow_mem_loads = true;
  }
  if (target_b == 0 || ctx.block_preds[target_b].empty() ||
      (ctx.df.live_in[target_b] & hr_m) == 0) {
    return false;
  }
  std::vector<std::pair<size_t, bool>> q;
  std::vector<bool> vis_ft(nb, false);
  std::vector<bool> vis_non_ft(nb, false);
  auto enqueue_pred_edge = [&](size_t pred, size_t succ) {
    if (pred >= nb) {
      return;
    }
    const bool is_ft_only_edge =
        (succ == pred + 1 && ctx.cfg.rpo_blocks[pred].successors.size() >= 2);
    if (is_ft_only_edge) {
      if (!vis_ft[pred] && !vis_non_ft[pred]) {
        vis_ft[pred] = true;
        q.emplace_back(pred, true);
      }
    } else if (!vis_non_ft[pred]) {
      vis_non_ft[pred] = true;
      q.emplace_back(pred, false);
    }
  };
  for (size_t pred : ctx.block_preds[target_b]) {
    enqueue_pred_edge(pred, target_b);
  }
  bool saw_def = false;
  for (size_t qi = 0; qi < q.size(); ++qi) {
    const auto [pb, ft_only] = q[qi];
    const auto& pb_steps = ctx.StepsOf(pb);
    bool stopped = false;
    for (size_t rev = pb_steps.size(); rev > 0; --rev) {
      const Stage0StepRef& pst = pb_steps[rev - 1];
      if (ft_only && IsStage0AnnulledUntakenTailSlot(ctx.cfg, ctx.all_bp, pb, pst)) {
        continue;
      }
      const ElemSummary pes = ctx.SummaryOf(pb, rev - 1);
      const RtlExpr& ppat = pst.pn->pattern;
      if (pes.is_call && (pes.uses & hr_m) != 0) {
        return false;
      }
      if ((pes.defs & hr_m) != 0) {
        if (pes.is_call || (ft_only && pst.in_slot &&
                            ctx.all_bp[pb].seq_slot_wraps[pst.insn_idx].flags.in_struct)) {
          return false;
        }
        const RtlExpr* ss = ExtractStage0SingleSet(ppat);
        if (ss == nullptr || IsAnyControlTransferPattern(ppat) ||
            HasStage0LibcallOrRetvalNote(pst) || !ss->SetDest().IsHardReg() ||
            ss->SetDest().RegNo() != hr || ss->SetDest().mode != req_mode ||
            ss->SetDest().flags.volatil || ss->SetDest().flags.integrated ||
            ctx.IsSpillOrCsLoadExpr(ss->SetSrc())) {
          return false;
        }
        if (out_all_narrow_mem_loads != nullptr) {
          const RtlExpr& ssrc = ss->SetSrc();
          const bool is_narrow_mem =
              (ssrc.op == RtlOp::kZeroExtend || ssrc.op == RtlOp::kSignExtend) &&
              ssrc.children.size() == 1 && ssrc.children[0].op == RtlOp::kMem &&
              (ssrc.children[0].mode == RtlMachineMode::kQi ||
               ssrc.children[0].mode == RtlMachineMode::kHi);
          if (!is_narrow_mem) {
            *out_all_narrow_mem_loads = false;
          }
        }
        if (Stage0DefReachesPinnedCallArgUse(ctx, pb, rev - 1, hr_m)) {
          return false;
        }
        saw_def = true;
        if ((pes.uses & hr_m) == 0) {
          stopped = true;
          break;
        }
        if (!HasStage0OnlyUnpinnedReadsOfMask(ppat, ctx.df, hr, req_mode, hr_m)) {
          return false;
        }
        continue;
      }
      if ((pes.uses & hr_m) != 0 &&
          !HasStage0OnlyUnpinnedReadsOfMask(ppat, ctx.df, hr, req_mode, hr_m)) {
        return false;
      }
    }
    if (!stopped) {
      if (ctx.block_preds[pb].empty()) {
        return false;
      }
      for (size_t nx : ctx.block_preds[pb]) {
        enqueue_pred_edge(nx, pb);
      }
    }
  }
  return saw_def;
}

bool CheckStage0LocalOutTail(const Stage0RmwCtx& ctx, size_t blk_idx, size_t start_k, int hr,
                             RtlMachineMode mode, RtlFlags dst_flags, RegMask hr_m,
                             bool is_callee_saved_hr, const RtlExpr* join_slot_pat) {
  const size_t nb = ctx.cfg.rpo_blocks.size();
  const auto& blk_steps = ctx.StepsOf(blk_idx);
  if (start_k == blk_steps.size() && join_slot_pat != nullptr &&
      ctx.cfg.rpo_blocks[blk_idx].successors.size() == 1) {
    const size_t succ = ctx.cfg.rpo_blocks[blk_idx].successors[0];
    if (succ != blk_idx && succ < nb && (ctx.df.live_in[succ] & hr_m) != 0 &&
        ctx.block_preds[succ].size() >= 2) {
      bool all_other_preds_stole_same = true;
      for (size_t pred : ctx.block_preds[succ]) {
        if (pred == blk_idx) {
          continue;
        }
        const auto& psteps = ctx.StepsOf(pred);
        const bool has_stolen_slot =
            psteps.size() >= 2 && psteps[psteps.size() - 2].in_slot &&
            psteps[psteps.size() - 2].insn_idx == psteps.back().insn_idx &&
            ctx.all_bp[pred].seq_slot_wraps[psteps.back().insn_idx].flags.in_struct &&
            psteps[psteps.size() - 2].pn->pattern == *join_slot_pat;
        if (!has_stolen_slot) {
          all_other_preds_stole_same = false;
          break;
        }
      }
      if (all_other_preds_stole_same) {
        return CheckStage0LocalOutTail(ctx, succ, 0, hr, mode, dst_flags, hr_m, is_callee_saved_hr,
                                       /*join_slot_pat=*/nullptr);
      }
    }
  }
  bool saw_local_use = false;
  bool saw_pinned_ret_use = false;
  bool killed_in_blk = false;
  bool w_crosses_call = false;
  bool call_since_start = false;
  for (size_t k = start_k; k < blk_steps.size(); ++k) {
    const Stage0StepRef& kst = blk_steps[k];
    const ElemSummary kes = ctx.SummaryOf(blk_idx, k);
    const RtlExpr& kpat = kst.pn->pattern;
    if (HasStage0LibcallOrRetvalNote(kst) ||
        (kst.in_slot && ctx.all_bp[blk_idx].seq_slot_wraps[kst.insn_idx].flags.in_struct) ||
        (kes.is_call && (kes.uses & hr_m) != 0)) {
      return false;
    }
    if ((kes.uses & hr_m) != 0) {
      const RtlExpr* kss = ExtractStage0SingleSet(kpat);
      const bool is_ret_copy =
          kss != nullptr && kss->SetDest().IsHardReg() && kss->SetDest().flags.integrated &&
          (kss->SetDest().RegNo() == 2 || kss->SetDest().RegNo() == 32) &&
          HasStage0OnlyUnpinnedReadsOfMask(kss->SetSrc(), ctx.df, hr, mode, hr_m);
      if (!is_ret_copy && !HasStage0OnlyUnpinnedReadsOfMask(kpat, ctx.df, hr, mode, hr_m)) {
        return false;
      }
      saw_local_use = true;
      if (is_ret_copy) {
        saw_pinned_ret_use = true;
      }
      if (call_since_start) {
        w_crosses_call = true;
      }
    }
    if (kes.is_call) {
      call_since_start = true;
    }
    if ((kes.defs & hr_m) != 0) {
      const RtlExpr* kss = ExtractStage0SingleSet(kpat);
      const bool in_place_rmw = !kes.is_call && (kes.uses & hr_m) != 0 && kss != nullptr &&
                                kss->SetDest().IsHardReg() && kss->SetDest().RegNo() == hr &&
                                kss->SetDest().mode == mode && kss->SetDest().flags == dst_flags &&
                                HasStage0OnlyUnpinnedReadsOfMask(kpat, ctx.df, hr, mode, hr_m);
      if (!in_place_rmw) {
        killed_in_blk = true;
        break;
      }
    }
  }
  bool live_at_any_succ = false;
  ForEachStage0CfgSucc(ctx.cfg.rpo_blocks[blk_idx], nb, [&](size_t succ) {
    if ((ctx.df.live_in[succ] & hr_m) != 0) {
      live_at_any_succ = true;
    }
  });
  if (!saw_local_use || (!killed_in_blk && live_at_any_succ)) {
    return false;
  }
  if (is_callee_saved_hr && !w_crosses_call && !saw_pinned_ret_use) {
    return false;
  }
  {
    size_t use_cnt = 0;
    size_t only_use_k = blk_steps.size();
    for (size_t k = start_k; k < blk_steps.size(); ++k) {
      const ElemSummary& kes = ctx.SummaryOf(blk_idx, k);
      if ((kes.uses & hr_m) != 0) {
        ++use_cnt;
        only_use_k = k;
      }
      if ((kes.defs & hr_m) != 0) {
        break;
      }
    }
    if (use_cnt == 1 && !blk_steps[only_use_k].in_slot) {
      const RtlExpr& tail_pat = blk_steps[only_use_k].pn->pattern;
      auto pred_stole_tail_pat = [&](size_t pred) {
        const auto& psteps = ctx.StepsOf(pred);
        return psteps.size() >= 2 && psteps[psteps.size() - 2].in_slot &&
               psteps[psteps.size() - 2].insn_idx == psteps.back().insn_idx &&
               ctx.all_bp[pred].seq_slot_wraps[psteps.back().insn_idx].flags.in_struct &&
               psteps[psteps.size() - 2].pn->pattern == tail_pat;
      };
      if (ctx.cfg.rpo_blocks[blk_idx].successors.size() == 1) {
        const size_t succ = ctx.cfg.rpo_blocks[blk_idx].successors[0];
        if (succ != blk_idx && succ < nb && ctx.block_preds[succ].size() >= 2) {
          for (size_t pred : ctx.block_preds[succ]) {
            if (pred != blk_idx && pred_stole_tail_pat(pred)) {
              return false;
            }
          }
        }
      }
      if (ctx.block_preds[blk_idx].size() == 1 && ctx.block_preds[blk_idx][0] != blk_idx &&
          pred_stole_tail_pat(ctx.block_preds[blk_idx][0])) {
        return false;
      }
    }
  }
  return true;
}

// Per `local-alloc.c:1762-1765` (`combine_regs`: `reg_qty[ureg] < 0` skips
// global operands without recording `qty_phys_sugg`), `local-alloc.c:2106-2182`
// (`find_free_reg`, `#ifndef REG_ALLOC_ORDER` at `2160-2180`, scanning
// `regno = 0..FIRST_PSEUDO_REGISTER-1` against `regs_live_at[born_index..dead_index-1]`),
// and `config/mips/mips.h:1192, 1203` (`FIXED_REGISTERS`/`CALL_USED_REGISTERS`):
// a block-local 1-word non-call-crossing pseudo born at a self-RMW reading an
// unpinned global pseudo in caller-saved GPR `hr > 2` with no hard-reg
// copy/return suggestion cannot be allocated to `hr` when ANY lower-numbered
// allocatable caller-saved GPR `cand_r < hr` is completely free across its
// lifespan `[born_index, dead_index)`.
bool HasLowerRegAllocOrderGprFreeAcrossLocalOutTail(const Stage0RmwCtx& ctx, size_t blk_idx,
                                                    size_t def_s, size_t start_k, int hr,
                                                    RtlMachineMode mode, RegMask hr_m,
                                                    bool is_callee_saved_hr) {
  const bool is_later_caller_saved_gpr =
      !is_callee_saved_hr && ((hr >= 3 && hr <= 15) || hr == 24 || hr == 25);
  const bool is_single_word_int =
      (mode == RtlMachineMode::kQi || mode == RtlMachineMode::kHi || mode == RtlMachineMode::kSi);
  const auto& blk_steps = ctx.StepsOf(blk_idx);
  if (!is_later_caller_saved_gpr || !is_single_word_int || def_s >= blk_steps.size() ||
      start_k >= blk_steps.size() || blk_idx != ctx.b || def_s >= ctx.live_after_step.size()) {
    return false;
  }
  size_t last_use_k = blk_steps.size();
  for (size_t k = start_k; k < blk_steps.size(); ++k) {
    const ElemSummary kes = ctx.SummaryOf(blk_idx, k);
    if (kes.is_call) {
      return false;
    }
    if ((kes.uses & hr_m) != 0) {
      const RtlExpr* kss = ExtractStage0SingleSet(blk_steps[k].pn->pattern);
      if (kss != nullptr && kss->SetDest().IsHardReg() &&
          (kss->SetSrc().IsHardReg() || kss->SetDest().flags.integrated)) {
        return false;
      }
      last_use_k = k;
    }
    if ((kes.defs & hr_m) != 0) {
      if ((kes.uses & hr_m) != 0) {
        return false;
      }
      break;
    }
  }
  if (last_use_k >= blk_steps.size() || last_use_k >= ctx.live_after_step.size()) {
    return false;
  }
  const bool last_use_is_simple_set = blk_steps[last_use_k].pn->pattern.IsSet();
  auto is_cand_free = [&](int cand_r) {
    const RegMask cand_m = RegMask{1} << cand_r;
    if ((ctx.live_after_step[def_s] & cand_m) != 0) {
      return false;
    }
    for (size_t k = start_k; k <= last_use_k; ++k) {
      const ElemSummary kes = ctx.SummaryOf(blk_idx, k);
      if ((kes.uses & cand_m) != 0) {
        return false;
      }
      if (k < last_use_k) {
        if ((kes.defs & cand_m) != 0 || (ctx.live_after_step[k] & cand_m) != 0) {
          return false;
        }
      } else if ((kes.defs & cand_m) != 0 && !last_use_is_simple_set) {
        // Non-simple-SET (e.g. `PARALLEL` earlyclobber/clobber) can mark
        // `cand_r` live at `2 * last_use_k - 1 < dead_index`.
        return false;
      }
    }
    return true;
  };
  for (int cand_r = 2; cand_r < hr; ++cand_r) {
    const bool is_caller_gpr = (cand_r >= 2 && cand_r <= 15) || cand_r == 24;
    if (is_caller_gpr && is_cand_free(cand_r)) {
      return true;
    }
  }
  return false;
}

bool Stage0ConstantAddrBaseP(const RtlExpr& e);
const RtlExpr& Stage0UnwrapMemBaseOffset(const RtlExpr& raw_addr, int64_t* offset_accum);

bool HasValidStage0PreStepInWeb(const Stage0RmwCtx& ctx, size_t s, int hr, RtlMachineMode mode,
                                RegMask hr_m, bool allow_pure_entry_abi_arg) {
  const auto& steps = ctx.blk_parsed->steps;
  for (size_t rev = s; rev > 0; --rev) {
    const size_t p = rev - 1;
    if ((ctx.esums[p].defs & hr_m) != 0) {
      if (ctx.esums[p].is_call) {
        return !steps[p].in_slot && (ctx.esums[p].explicit_sets & hr_m) == hr_m;
      }
      const RtlExpr* pss = ExtractStage0SingleSet(steps[p].pn->pattern);
      return !steps[p].in_slot && pss != nullptr && !HasStage0LibcallOrRetvalNote(steps[p]) &&
             pss->SetDest().IsHardReg() && pss->SetDest().RegNo() == hr &&
             pss->SetDest().mode == mode && !pss->SetDest().flags.volatil &&
             !pss->SetDest().flags.integrated && !ctx.IsSpillOrCsLoadExpr(pss->SetSrc()) &&
             !ctx.IsSpillOutputReloadBeforeStep(p, s, hr, mode);
    }
    if ((ctx.esums[p].uses & hr_m) != 0 &&
        !HasStage0OnlyUnpinnedReadsOfMask(steps[p].pn->pattern, ctx.df, hr, mode, hr_m)) {
      return false;
    }
  }
  if (HasStage0UnpinnedGlobalInWeb(ctx, ctx.b, hr, mode, hr_m)) {
    return true;
  }
  if (!allow_pure_entry_abi_arg) {
    return false;
  }
  std::vector<bool> any_unmod_in;
  std::vector<bool> any_unmod_out;
  std::vector<bool> all_unmod_in;
  ComputeStage0EntryAbiArgUnmodifiedReach(ctx.cfg, ctx.df, ctx.all_bp, ctx.block_preds, hr_m,
                                          &any_unmod_in, &any_unmod_out, &all_unmod_in);
  return ctx.b < ctx.cfg.rpo_blocks.size() && all_unmod_in[ctx.b];
}

bool TryAttachStage0LoadOrLocalScratchDeadNote(const Stage0RmwCtx& ctx, size_t s, int hr,
                                               const RtlExpr& dst, const RtlExpr& rhs,
                                               const RtlExpr& read_reg_expr, RegMask hr_m,
                                               bool direct_non_mem_read, bool mem_addr_read,
                                               bool is_callee_saved_hr, bool pure_entry_abi_arg) {
  if (dst.flags.integrated || (is_callee_saved_hr && !direct_non_mem_read)) {
    return false;
  }
  const auto& steps = ctx.blk_parsed->steps;
  const size_t ns = steps.size();
  const size_t nb = ctx.cfg.rpo_blocks.size();

  if (mem_addr_read && !direct_non_mem_read) {
    const bool is_ext_load = (rhs.op == RtlOp::kZeroExtend || rhs.op == RtlOp::kSignExtend ||
                              dst.mode != read_reg_expr.mode);
    bool computed_by_addr_arith = false;
    for (size_t rev = s; !is_ext_load && rev > 0; --rev) {
      const size_t p = rev - 1;
      if (ctx.esums[p].is_call || IsAnyControlTransferPattern(steps[p].pn->pattern)) {
        break;
      }
      if ((ctx.esums[p].defs & hr_m) != 0) {
        const RtlExpr* pss = ExtractStage0SingleSet(steps[p].pn->pattern);
        if (pss != nullptr && pss->SetDest().IsHardReg() && pss->SetDest().RegNo() == hr &&
            (pss->SetSrc().op == RtlOp::kAshift || pss->SetSrc().op == RtlOp::kPlus)) {
          computed_by_addr_arith = true;
        }
        break;
      }
    }
    bool feeds_indirect_xfer = false;
    auto pat_has_indirect_xfer_on_hr = [&](const RtlExpr& e) {
      bool hit = false;
      e.Visit([&](const RtlExpr& n) {
        if ((n.IsSet() && n.SetDest().op == RtlOp::kPc && n.SetSrc().IsReg() &&
             n.SetSrc().RegNo() == hr) ||
            (n.op == RtlOp::kCall && !n.children.empty() && n.children[0].op == RtlOp::kMem &&
             !n.children[0].children.empty() && n.children[0].children[0].IsReg() &&
             n.children[0].children[0].RegNo() == hr)) {
          hit = true;
        }
      });
      return hit;
    };
    const size_t after_s =
        (steps[s].in_slot && s + 1 < ns && steps[s + 1].insn_idx == steps[s].insn_idx) ? (s + 2)
                                                                                       : (s + 1);
    for (size_t k = after_s; k < ns; ++k) {
      if (pat_has_indirect_xfer_on_hr(steps[k].pn->pattern)) {
        feeds_indirect_xfer = true;
        break;
      }
      if ((ctx.esums[k].defs & hr_m) != 0) {
        break;
      }
    }
    bool entry_arg_never_deref_after = false;
    if (pure_entry_abi_arg && !is_ext_load && !computed_by_addr_arith && !feeds_indirect_xfer) {
      bool saw_mem_base_after = false;
      auto check_mem_base = [&](const auto& self, const RtlExpr& e, bool in_mem) -> void {
        if (e.op == RtlOp::kMem) {
          for (const RtlExpr& ch : e.children) self(self, ch, true);
          return;
        }
        if (in_mem && e.IsReg() && e.RegNo() == hr) {
          saw_mem_base_after = true;
          return;
        }
        for (const RtlExpr& ch : e.children) self(self, ch, in_mem);
      };
      for (size_t k = after_s; k < ns; ++k) {
        check_mem_base(check_mem_base, steps[k].pn->pattern, false);
        if ((ctx.esums[k].defs & hr_m) != 0) {
          break;
        }
      }
      if (!saw_mem_base_after && (ctx.df.live_out[ctx.b] & hr_m) == 0) {
        entry_arg_never_deref_after = true;
      }
    }
    if (is_ext_load || computed_by_addr_arith || feeds_indirect_xfer ||
        entry_arg_never_deref_after) {
      // Mirror `flow.c:2507-2550` (`flow.c:2511`) / `sched.c:2872`: when an unmodified incoming ABI
      // argument (`pure_entry_abi_arg`) is overwritten in place and feeds an
      // immediately following `call_insn`'s outgoing ABI argument use in the
      // same hard register `hr`, both source and destination are ABI-pinned to
      // `hr` (never distinct pseudos), so the insn cannot carry `REG_DEAD hr`.
      bool pure_entry_arg_feeds_call_arg = false;
      if (pure_entry_abi_arg && ((hr >= 4 && hr <= 7) || (hr >= 44 && hr <= 47))) {
        size_t call_s = ns;
        if (steps[s].in_slot && s + 1 < ns && steps[s + 1].insn_idx == steps[s].insn_idx &&
            ctx.esums[s + 1].is_call) {
          call_s = s + 1;
        } else if (!steps[s].in_slot) {
          for (size_t k = after_s; k < ns; ++k) {
            if (ctx.esums[k].is_call) {
              call_s = k;
              break;
            }
            if (((ctx.esums[k].defs | ctx.esums[k].uses) & hr_m) != 0 ||
                IsAnyControlTransferPattern(steps[k].pn->pattern)) {
              break;
            }
          }
        }
        if (call_s < ns && (ctx.esums[call_s].uses & hr_m) != 0 &&
            (ctx.live_after_step[call_s] & hr_m) == 0) {
          pure_entry_arg_feeds_call_arg = true;
        }
      }
      if (!pure_entry_arg_feeds_call_arg) {
        ctx.AttachDeadNote(s, hr, read_reg_expr);
        return true;
      }
    }
    return false;
  }

  const bool is_call_delay_slot = steps[s].in_slot && s + 1 < ns &&
                                  steps[s + 1].insn_idx == steps[s].insn_idx &&
                                  ctx.esums[s + 1].is_call;
  if (!direct_non_mem_read || (steps[s].in_slot && !is_call_delay_slot)) {
    return false;
  }
  size_t def_s = ns;
  // `reorg.c:2889-3025` (`fill_simple_delay_slots`) fills a `call_insn` delay slot from the same
  // basic block (never from a successor branch target). A callee-saved insn that reads
  // no call-clobbered register may have been moved from after the call, so treat it
  // conservatively as separated by the call (`saw_call_between = true`); one that reads
  // a call-clobbered register is guaranteed to have preceded the call.
  bool saw_call_between = is_call_delay_slot && is_callee_saved_hr &&
                          ((ctx.esums[s].uses & ctx.esums[s + 1].defs) == 0);
  for (size_t rev = s; rev > 0; --rev) {
    const size_t p = rev - 1;
    const RtlExpr& ppat = steps[p].pn->pattern;
    if (IsAnyControlTransferPattern(ppat) && !ctx.esums[p].is_call) {
      return false;
    }
    if (ctx.esums[p].is_call) {
      if ((ctx.esums[p].defs & hr_m) != 0) {
        if (!is_callee_saved_hr && !steps[p].in_slot &&
            (ctx.esums[p].explicit_sets & hr_m) == hr_m) {
          def_s = p;
          break;
        }
        return false;
      }
      if (!is_callee_saved_hr || (ctx.esums[p].uses & hr_m) != 0) {
        return false;
      }
      saw_call_between = true;
      continue;
    }
    if (HasStage0LibcallOrRetvalNote(steps[p])) {
      if (((ctx.esums[p].defs | ctx.esums[p].uses) & hr_m) == 0) {
        continue;
      }
      if (!is_callee_saved_hr && !steps[p].in_slot && (ctx.esums[p].defs & hr_m) != 0 &&
          std::any_of(steps[p].pn->reg_notes.begin(), steps[p].pn->reg_notes.end(),
                      [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegRetval; })) {
        def_s = p;
        break;
      }
      return false;
    }
    if ((ctx.esums[p].defs & hr_m) != 0) {
      const RtlExpr* pss = ExtractStage0SingleSet(ppat);
      bool is_dead_md_output_reload = false;
      if (pss != nullptr && pss->SetSrc().IsHardReg() &&
          (pss->SetSrc().RegNo() == 64 || pss->SetSrc().RegNo() == 65) &&
          (ctx.live_after_step[p] & (RegMask{1} << pss->SetSrc().RegNo())) == 0 && p > 0) {
        const int md_src_r = pss->SetSrc().RegNo();
        const RegMask md_src_m = RegMask{1} << md_src_r;
        size_t def_md_s = ns;
        for (size_t rev_q = p; rev_q > 0; --rev_q) {
          const size_t q = rev_q - 1;
          if (ctx.esums[q].is_call || IsAnyControlTransferPattern(steps[q].pn->pattern)) {
            break;
          }
          if ((ctx.esums[q].defs & md_src_m) != 0) {
            def_md_s = q;
            break;
          }
          if ((ctx.esums[q].uses & md_src_m) != 0) {
            break;
          }
        }
        if (def_md_s < p) {
          if (p + 1 < s) {
            is_dead_md_output_reload = true;
          } else {
            const RegMask md_pair = (RegMask{1} << 64) | (RegMask{1} << 65);
            for (size_t m_idx = 0; m_idx < ns; ++m_idx) {
              if (m_idx != def_md_s && m_idx != p && !ctx.esums[m_idx].is_call &&
                  ((ctx.esums[m_idx].defs | ctx.esums[m_idx].uses) & md_pair) != 0) {
                is_dead_md_output_reload = true;
                break;
              }
            }
          }
        }
      }
      if (!ctx.df.gp64 && !is_callee_saved_hr && dst.mode == RtlMachineMode::kDi &&
          read_reg_expr.mode == RtlMachineMode::kDi && (hr % 2) == 0 && p >= 2 &&
          !steps[p].in_slot && !steps[p - 1].in_slot && !steps[p - 2].in_slot &&
          !ctx.esums[p].is_call && !ctx.esums[p - 1].is_call) {
        const RtlExpr& cpat = steps[p - 2].pn->pattern;
        const RtlExpr* pss1 = ExtractStage0SingleSet(steps[p - 1].pn->pattern);
        const bool has_libcall_clobber =
            cpat.op == RtlOp::kClobber && !cpat.children.empty() && cpat.children[0].IsHardReg() &&
            cpat.children[0].mode == RtlMachineMode::kDi && cpat.children[0].RegNo() == hr &&
            std::any_of(steps[p - 2].pn->reg_notes.begin(), steps[p - 2].pn->reg_notes.end(),
                        [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegLibcall; });
        if (has_libcall_clobber && pss != nullptr && pss1 != nullptr &&
            pss->SetDest().IsHardReg() && pss->SetDest().mode == RtlMachineMode::kSi &&
            !pss->SetDest().flags.integrated && pss1->SetDest().IsHardReg() &&
            pss1->SetDest().mode == RtlMachineMode::kSi && !pss1->SetDest().flags.integrated &&
            ((pss->SetDest().RegNo() == hr && pss1->SetDest().RegNo() == hr + 1) ||
             (pss->SetDest().RegNo() == hr + 1 && pss1->SetDest().RegNo() == hr))) {
          def_s = p;
          break;
        }
      }
      if (steps[p].in_slot || pss == nullptr || !pss->SetDest().IsHardReg() ||
          pss->SetDest().RegNo() != hr || pss->SetDest().mode != read_reg_expr.mode ||
          pss->SetDest().flags.volatil || pss->SetDest().flags.integrated ||
          (pss->SetSrc().IsReg() && !is_dead_md_output_reload) ||
          ctx.IsSpillOrCsLoadExpr(pss->SetSrc()) ||
          ctx.IsSpillOutputReloadBeforeStep(p, s, hr, read_reg_expr.mode)) {
        return false;
      }
      if (is_callee_saved_hr) {
        int64_t dummy_off = 0;
        const bool is_invariant_mem =
            pss->SetSrc().op == RtlOp::kMem && !pss->SetSrc().flags.volatil &&
            (pss->SetSrc().flags.unchanging ||
             (!pss->SetSrc().children.empty() && Stage0ConstantAddrBaseP(Stage0UnwrapMemBaseOffset(
                                                     pss->SetSrc().children[0], &dummy_off))));
        if (saw_call_between) {
          // Across a call, a callee-saved register modified in place is a user variable
          // (`reg/v`, e.g. `dx *= inv_len`) unless its prior def was an invariant memory load.
          if (!is_invariant_mem || (ctx.esums[p].uses & hr_m) != 0) {
            return false;
          }
          def_s = p;
          break;
        }
        // Without an intervening call, `local-alloc.c:find_free_reg` / `global.c:find_reg`
        // allocates callee-saved registers (`REG_ALLOC_ORDER`: `$s0..$s7`, `$fp`, `$f20..$f30`)
        // to ordinary single-set dying-input pseudos under high register pressure or prior to
        // a later call (`flow.c:2506-2515` + `local-alloc.c:1395-1450`).
      }
      if ((ctx.esums[p].uses & hr_m) == 0 ||
          HasRegNote(steps[p].pn->reg_notes, RtlMachineMode::kRegDead, hr)) {
        // `expmed.c:store_bit_field` emits an in-place same-pseudo `and; ior` pair only for
        // word-sized `SImode` containers whose mask preserves upper bits (negative `const_int`
        // or register mask); `DImode` bitwise ops and narrow `QI`/`HI` positive 16-bit masks
        // `(0, 65535]` allocate a fresh `SImode` pseudo with `REG_DEAD` on `ior`.
        const bool pss_is_narrow_const_and =
            pss->SetSrc().op == RtlOp::kAnd && pss->SetSrc().children.size() == 2 &&
            pss->SetSrc().children[1].IsConstInt() && pss->SetSrc().children[1].int_val > 0 &&
            pss->SetSrc().children[1].int_val <= 65535;
        bool is_bitfield_rmw_step =
            (dst.mode == RtlMachineMode::kSi && pss->SetSrc().op == RtlOp::kAnd &&
             !pss_is_narrow_const_and && rhs.op == RtlOp::kIor);
        if (!is_bitfield_rmw_step && dst.mode == RtlMachineMode::kSi && rhs.op == RtlOp::kAnd &&
            (pss->SetSrc().IsMem() || pss->SetSrc().op == RtlOp::kAnd)) {
          for (size_t k = s + 1; k < ns; ++k) {
            if (ctx.esums[k].is_call || IsAnyControlTransferPattern(steps[k].pn->pattern)) {
              break;
            }
            if ((ctx.esums[k].defs & hr_m) != 0) {
              const RtlExpr* kss = ExtractStage0SingleSet(steps[k].pn->pattern);
              if ((ctx.esums[k].uses & hr_m) == 0 || kss == nullptr ||
                  !kss->SetDest().IsHardReg() || kss->SetDest().RegNo() != hr ||
                  kss->SetDest().mode != dst.mode) {
                break;
              }
              if (kss->SetSrc().op == RtlOp::kIor) {
                is_bitfield_rmw_step = true;
                break;
              }
            }
          }
        }
        const bool is_fp_const_pool_load =
            pss->SetSrc().op == RtlOp::kConstDouble && hr >= 32 && hr < 64 &&
            (dst.mode == RtlMachineMode::kSf || dst.mode == RtlMachineMode::kDf);
        const bool is_non_zero_const_mask_or_shift =
            pss->SetSrc().IsConstInt() && pss->SetSrc().int_val != 0 &&
            (rhs.op == RtlOp::kAnd || rhs.op == RtlOp::kAshift);
        if ((IsStage0ConstantReloadSrc(pss->SetSrc()) && !is_fp_const_pool_load &&
             !is_non_zero_const_mask_or_shift) ||
            is_bitfield_rmw_step) {
          return false;
        }
        def_s = p;
        break;
      }
      // A legitimate 2-insn same-pseudo `lui; ori` (`sched.c:try_split`) or `SImode`
      // `and; ior` (`expmed.c:store_bit_field`) has no `REG_DEAD` on its second insn `p`,
      // yet a subsequent consumer `s` (e.g. `and`, `ashift`, `lshiftrt`) that overwrites
      // `hr` starts a new single-set pseudo and kills that finished value (`REG_DEAD hr`).
      const bool p_is_lui_ori_second =
          !is_callee_saved_hr && pss->SetSrc().op == RtlOp::kIor &&
          pss->SetSrc().children.size() == 2 && pss->SetSrc().children[1].IsConstInt() &&
          pss->SetSrc().children[1].int_val > 0 && pss->SetSrc().children[1].int_val <= 65535;
      const bool p_is_bitfield_ior_second =
          !is_callee_saved_hr && dst.mode == RtlMachineMode::kSi &&
          pss->SetSrc().op == RtlOp::kIor && rhs.op != RtlOp::kIor;
      if (p_is_lui_ori_second || p_is_bitfield_ior_second) {
        bool matched_pair_head = false;
        for (size_t rev_q = p; rev_q > 0; --rev_q) {
          const size_t q = rev_q - 1;
          if (ctx.esums[q].is_call || IsAnyControlTransferPattern(steps[q].pn->pattern) ||
              HasStage0LibcallOrRetvalNote(steps[q])) {
            break;
          }
          if ((ctx.esums[q].defs & hr_m) != 0) {
            const RtlExpr* qss = ExtractStage0SingleSet(steps[q].pn->pattern);
            if (!steps[q].in_slot && qss != nullptr && qss->SetDest().IsHardReg() &&
                qss->SetDest().RegNo() == hr && qss->SetDest().mode == read_reg_expr.mode &&
                !qss->SetDest().flags.volatil && !qss->SetDest().flags.integrated) {
              if (p_is_lui_ori_second && qss->SetSrc().IsConstInt() &&
                  (qss->SetSrc().int_val & 0xffff) == 0 && (ctx.esums[q].uses & hr_m) == 0) {
                matched_pair_head = true;
              } else if (p_is_bitfield_ior_second && qss->SetSrc().op == RtlOp::kAnd) {
                matched_pair_head = true;
              }
            }
            break;
          }
        }
        if (matched_pair_head) {
          def_s = p;
          break;
        }
      }
      return false;
    }
  }
  if (def_s >= s) {
    return false;
  }
  bool saw_read = false;
  bool killed_in_b = false;
  RegMask pinned_ret_overwrite_mask = 0;
  RegMask annulled_taken_kill_mask = 0;
  auto is_integrated_reg_or_subreg = [](const RtlExpr& d) {
    return (d.IsHardReg() && d.flags.integrated) ||
           (d.IsSubreg() && !d.children.empty() && d.children[0].IsHardReg() &&
            d.children[0].flags.integrated);
  };
  auto is_reorg_bypassed_return_use = [&](size_t use_k) -> bool {
    if (steps[use_k].pn->pattern.op != RtlOp::kUse) {
      return false;
    }
    RegMask slot_ret_mask = pinned_ret_overwrite_mask;
    for (size_t k2 = use_k + 1; k2 < ns; ++k2) {
      if ((ctx.esums[k2].defs & hr_m) == 0) {
        continue;
      }
      if (!steps[k2].in_slot) {
        return false;
      }
      const RtlExpr* k2ss = ExtractStage0SingleSet(steps[k2].pn->pattern);
      if (k2ss != nullptr && is_integrated_reg_or_subreg(k2ss->SetDest())) {
        slot_ret_mask |= (ctx.esums[k2].defs & hr_m);
      }
    }
    return (slot_ret_mask & hr_m) == hr_m;
  };
  auto has_pinned_read_of_mask = [&](const auto& self, const RtlExpr& e, bool in_dst) -> bool {
    if (e.op == RtlOp::kClobber) {
      return false;
    }
    if (e.IsSet()) {
      const bool dst_is_reg_or_subreg =
          e.SetDest().IsReg() || (e.SetDest().IsSubreg() && !e.SetDest().children.empty() &&
                                  e.SetDest().children[0].IsReg());
      return (!dst_is_reg_or_subreg && self(self, e.SetDest(), true)) ||
             self(self, e.SetSrc(), false);
    }
    if (in_dst && e.op == RtlOp::kMem) {
      for (const RtlExpr& ch : e.children) {
        if (self(self, ch, false)) {
          return true;
        }
      }
      return false;
    }
    if (!in_dst && e.IsHardReg() && (e.flags.integrated || e.flags.volatil)) {
      const RegMask rm = RegBitMask(e.RegNo(), e.mode, ctx.df.gp64, ctx.df.fp64);
      if ((rm & hr_m) != 0) {
        return true;
      }
    }
    for (const RtlExpr& ch : e.children) {
      if (self(self, ch, in_dst)) {
        return true;
      }
    }
    return false;
  };
  for (size_t k = s + 1; k < ns; ++k) {
    const ElemSummary& kes = ctx.esums[k];
    const RtlExpr& kpat = steps[k].pn->pattern;
    if (HasStage0LibcallOrRetvalNote(steps[k])) {
      return false;
    }
    if (kes.is_call) {
      // Callee-saved registers (`$s0..$s7`, `$fp`, `$f20..$f30`) are preserved across calls
      // unless explicitly read/clobbered by the call.
      if (is_callee_saved_hr && ((kes.uses | kes.defs) & hr_m) == 0) {
        continue;
      }
      return false;
    }
    if ((kes.uses & hr_m) != 0) {
      if (has_pinned_read_of_mask(has_pinned_read_of_mask, kpat, false)) {
        // `reorg.c:fill_simple_delay_slots` moves `(set (reg/i:M hr) ...)` past
        // `(use (reg/i:M hr))` into `(return)`'s delay slot; skip the bypassed `use`
        // marker so the scan reaches the delay-slot consumer and `/i` overwrite.
        if (is_reorg_bypassed_return_use(k)) {
          continue;
        }
        return false;
      }
      saw_read = true;
    }
    if ((kes.defs & hr_m) != 0) {
      if (steps[k].in_slot && ctx.blk_parsed->seq_slot_wraps[steps[k].insn_idx].flags.in_struct) {
        // `reorg.c:fill_slots_from_thread` annulled taken-branch delay slot unconditionally
        // overwrites `hr_m` on the taken edge without reading pre-slot `hr_m`.
        if ((kes.uses & hr_m) == 0 && k + 1 < ns && steps[k + 1].insn_idx == steps[k].insn_idx &&
            IsConditionalJumpPattern(steps[k + 1].pn->pattern)) {
          annulled_taken_kill_mask |= (kes.defs & hr_m);
        }
        continue;
      }
      const RtlExpr* kss = ExtractStage0SingleSet(kpat);
      const bool is_mem_load_overwrite =
          kss != nullptr &&
          (kss->SetSrc().op == RtlOp::kMem ||
           ((kss->SetSrc().op == RtlOp::kZeroExtend || kss->SetSrc().op == RtlOp::kSignExtend) &&
            !kss->SetSrc().children.empty() && kss->SetSrc().children[0].op == RtlOp::kMem));
      const bool is_pinned_ret_overwrite =
          kss != nullptr && is_integrated_reg_or_subreg(kss->SetDest());
      if (is_pinned_ret_overwrite) {
        pinned_ret_overwrite_mask |= (kes.defs & hr_m);
      }
      const bool is_dead_rmw_overwrite =
          HasRegNote(steps[k].pn->reg_notes, RtlMachineMode::kRegDead, hr);
      if ((kes.uses & hr_m) == 0 || is_mem_load_overwrite || is_dead_rmw_overwrite ||
          (pinned_ret_overwrite_mask & hr_m) == hr_m) {
        // `steps` orders a `SEQUENCE`'s delay slot (`k`) before its branch head (`k + 1`),
        // yet a non-annulled conditional branch evaluates its condition before the delay
        // slot commits (`reorg.c:3258-3306`, `fill_slots_from_thread` from fallthrough
        // when `hr` is dead in `opposite_needed`). Account for the branch's read of `hr_m`
        // before stopping on the slot's overwrite.
        if (steps[k].in_slot && k + 1 < ns && steps[k + 1].insn_idx == steps[k].insn_idx &&
            IsConditionalJumpPattern(steps[k + 1].pn->pattern) &&
            (ctx.esums[k + 1].uses & hr_m) != 0) {
          if (has_pinned_read_of_mask(has_pinned_read_of_mask, steps[k + 1].pn->pattern, false)) {
            return false;
          }
          saw_read = true;
        }
        killed_in_b = true;
        break;
      } else if (is_call_delay_slot && is_callee_saved_hr) {
        // A subsequent non-dead in-place RMW of callee-saved `hr` (e.g.
        // `l.d $f22 -> add.d $f22 -> mul.d $f22`) indicates a multi-set `reg/v`
        // accumulator rather than a single-set load dying in `s`.
        return false;
      }
    }
  }
  const RegMask exit_live = ctx.b < nb && ctx.df.can_exit[ctx.b] ? ctx.df.live_at_exit : RegMask{0};
  bool dead_at_blk_end = ((ctx.df.live_out[ctx.b] | exit_live) & hr_m) == 0;
  if (!dead_at_blk_end && (annulled_taken_kill_mask & hr_m) == hr_m && (exit_live & hr_m) == 0 &&
      ctx.b + 1 < nb && ctx.cfg.rpo_blocks[ctx.b].successors.size() == 2 &&
      (ctx.df.live_in[ctx.b + 1] & hr_m) == 0) {
    dead_at_blk_end = true;
  }
  if (!saw_read || (!killed_in_b && !dead_at_blk_end)) {
    return false;
  }
  ctx.AttachDeadNote(s, hr, dst);
  return true;
}

bool TryAttachStage0CaseCDeadNote(const Stage0RmwCtx& ctx, size_t s, int hr, const RtlExpr& dst,
                                  const RtlExpr& rhs, const RtlExpr& read_reg_expr, RegMask hr_m,
                                  bool direct_non_mem_read, bool is_callee_saved_hr) {
  const auto& steps = ctx.blk_parsed->steps;
  const size_t ns = steps.size();
  const size_t nb = ctx.cfg.rpo_blocks.size();
  for (size_t k = s + 1; k < ns; ++k) {
    if ((ctx.esums[k].defs & hr_m) != 0 ||
        (ctx.esums[k].is_call && (ctx.esums[k].uses & hr_m) != 0)) {
      return false;
    }
  }
  bool live_at_other_succ = false;
  ForEachStage0CfgSucc(ctx.cfg.rpo_blocks[ctx.b], nb, [&](size_t succ) {
    if (succ != ctx.b && (ctx.df.live_in[succ] & hr_m) != 0) {
      live_at_other_succ = true;
    }
  });
  if (!live_at_other_succ) {
    return false;
  }
  size_t def_s = ns;
  size_t in_place_rmw_steps = 0;
  bool saw_intermediate_read = false;
  bool crosses_call_between_def_and_s = false;
  bool valid_def = false;
  for (size_t rev = s; rev > 0; --rev) {
    const size_t p = rev - 1;
    const RtlExpr& ppat = steps[p].pn->pattern;
    if ((ctx.esums[p].defs & hr_m) != 0) {
      if (!steps[p].in_slot && !ctx.esums[p].is_call && !IsAnyControlTransferPattern(ppat) &&
          !HasStage0LibcallOrRetvalNote(steps[p])) {
        const RtlExpr* pss = ExtractStage0SingleSet(ppat);
        if (pss != nullptr && pss->SetDest().IsHardReg() && pss->SetDest().RegNo() == hr &&
            pss->SetDest().mode == read_reg_expr.mode &&
            pss->SetDest().raw_mode == read_reg_expr.raw_mode &&
            pss->SetDest().flags == read_reg_expr.flags && !pss->SetSrc().IsReg()) {
          if (((ctx.esums[p].uses & hr_m) == 0 ||
               HasRegNote(steps[p].pn->reg_notes, RtlMachineMode::kRegDead, hr)) &&
              ppat.IsSet()) {
            if (!ctx.IsSpillOrCsLoadExpr(pss->SetSrc()) &&
                !ctx.IsSpillOutputReloadBeforeStep(p, s, hr, read_reg_expr.mode)) {
              def_s = p;
              valid_def = true;
            }
            break;
          }
          bool p_direct_read = false;
          auto check_p_rhs = [&](const auto& self, const RtlExpr& e) -> void {
            if (e.op == RtlOp::kMem) return;
            if (e.IsHardReg() && e.RegNo() == hr && e.mode == read_reg_expr.mode &&
                e.raw_mode == read_reg_expr.raw_mode && e.flags == read_reg_expr.flags) {
              p_direct_read = true;
              return;
            }
            for (const RtlExpr& ch : e.children) self(self, ch);
          };
          check_p_rhs(check_p_rhs, pss->SetSrc());
          if ((ctx.esums[p].uses & hr_m) != 0 && p_direct_read &&
              !HasRegNote(steps[p].pn->reg_notes, RtlMachineMode::kRegDead, hr) &&
              HasStage0OnlyUnpinnedReadsOfMask(ppat, ctx.df, hr, read_reg_expr.mode, hr_m)) {
            ++in_place_rmw_steps;
            continue;
          }
        }
      }
      break;
    }
    if (ctx.esums[p].is_call) {
      if ((ctx.esums[p].uses & hr_m) != 0) {
        break;
      }
      crosses_call_between_def_and_s = true;
    }
    if (IsAnyControlTransferPattern(ppat)) {
      break;
    }
    if ((ctx.esums[p].uses & hr_m) != 0) {
      if (!HasStage0OnlyUnpinnedReadsOfMask(ppat, ctx.df, hr, read_reg_expr.mode, hr_m)) {
        break;
      }
      saw_intermediate_read = true;
    }
  }
  if (!valid_def || def_s >= s) {
    return false;
  }
  if ((ctx.df.live_in[ctx.b] & hr_m) != 0) {
    if (is_callee_saved_hr && !crosses_call_between_def_and_s) {
      return false;
    }
    for (int tmp_r = 2; tmp_r < kMipsFirstPseudoRegister; ++tmp_r) {
      const RegMask tmp_m = RegMask{1} << tmp_r;
      if (tmp_r == hr || (ctx.esums[def_s].uses & tmp_m) == 0 ||
          (ctx.live_after_step[def_s] & tmp_m) != 0) {
        continue;
      }
      for (size_t rev_q = def_s; rev_q > 0; --rev_q) {
        const size_t q = rev_q - 1;
        if ((ctx.esums[q].defs & tmp_m) != 0) {
          if ((ctx.esums[q].uses & hr_m) != 0 && (ctx.live_after_step[q] & hr_m) == 0) {
            return false;
          }
          break;
        }
      }
    }
  } else if (def_s == 0 && ctx.b > 0 && ctx.block_preds[ctx.b].size() == 1 &&
             ctx.block_preds[ctx.b][0] + 1 == ctx.b && (ctx.df.live_in[ctx.b - 1] & hr_m) != 0) {
    const size_t pred = ctx.b - 1;
    const auto& psteps = ctx.StepsOf(pred);
    for (int tmp_r = 2; tmp_r < kMipsFirstPseudoRegister; ++tmp_r) {
      const RegMask tmp_m = RegMask{1} << tmp_r;
      if (tmp_r == hr || (ctx.esums[def_s].uses & tmp_m) == 0 ||
          (ctx.live_after_step[def_s] & tmp_m) != 0) {
        continue;
      }
      for (size_t rev_q = psteps.size(); rev_q > 0; --rev_q) {
        const size_t q = rev_q - 1;
        const ElemSummary qes = ctx.SummaryOf(pred, q);
        if ((qes.defs & hr_m) != 0) {
          break;
        }
        if ((qes.defs & tmp_m) != 0) {
          if (psteps[q].in_slot && (qes.uses & hr_m) != 0) {
            return false;
          }
          break;
        }
      }
    }
  }
  if ((!direct_non_mem_read || in_place_rmw_steps > 0 || saw_intermediate_read ||
       steps[s].in_slot) &&
      !IsStage0UnpinnedOutWebFromStep(ctx, ctx.b, s + 1, hr, dst.mode, hr_m,
                                      /*require_global=*/true,
                                      /*forbid_merge_unmod_out=*/nullptr)) {
    return false;
  }
  const RtlExpr& def_rhs = steps[def_s].pn->pattern.SetSrc();
  if (in_place_rmw_steps >= 1 && def_rhs.op == RtlOp::kMem) {
    for (size_t k = s + 1; k < ns; ++k) {
      if (steps[k].pn->pattern.IsSet() && steps[k].pn->pattern.SetDest() == def_rhs &&
          (ctx.esums[k].uses & hr_m) != 0) {
        return false;
      }
      if (ctx.esums[k].is_call || IsAnyControlTransferPattern(steps[k].pn->pattern)) {
        break;
      }
    }
  }
  bool earlier_def_in_b = false;
  for (size_t e = 0; e < def_s; ++e) {
    if ((ctx.esums[e].defs & hr_m) != 0) {
      earlier_def_in_b = true;
      break;
    }
  }
  const bool is_const_init = ctx.b == 0 && !earlier_def_in_b && !saw_intermediate_read &&
                             (def_rhs.IsConstInt() || def_rhs.op == RtlOp::kConstDouble ||
                              def_rhs.op == RtlOp::kSymbolRef || def_rhs.op == RtlOp::kLabelRef ||
                              def_rhs.op == RtlOp::kConst);
  const bool is_lui_ori_const_init = def_rhs.IsConstInt() && (def_rhs.int_val & 0xffff) == 0 &&
                                     rhs.op == RtlOp::kIor && rhs.children.size() == 2 &&
                                     rhs.children[1].IsConstInt() && rhs.children[1].int_val > 0 &&
                                     rhs.children[1].int_val <= 65535;
  const bool is_high_lo_init =
      !earlier_def_in_b && def_rhs.op == RtlOp::kHigh &&
      (rhs.op == RtlOp::kIor || rhs.op == RtlOp::kPlus || rhs.op == RtlOp::kLoSum);
  const bool is_and_ior_init = def_rhs.op == RtlOp::kAnd && rhs.op == RtlOp::kIor;
  if (direct_non_mem_read && in_place_rmw_steps == 0 &&
      (is_const_init || is_lui_ori_const_init || is_high_lo_init || is_and_ior_init)) {
    return false;
  }
  ctx.AttachDeadNote(s, hr, direct_non_mem_read ? dst : read_reg_expr);
  return true;
}

void AttachStage0SplitLocalToGlobalRmwRegDeadNotes(
    const RtlControlFlowGraph& cfg, const DataflowResult& df, size_t b,
    const std::vector<Stage0BlockParsed>& all_bp, const std::vector<ElemSummary>& esums,
    const std::vector<RegMask>& live_after_step,
    const absl::flat_hash_set<int64_t>& caller_save_offsets,
    const absl::flat_hash_set<int64_t>& proven_spill_offsets, Stage0BlockParsed* blk_parsed) {
  auto& steps = blk_parsed->steps;
  const size_t ns = steps.size();
  const size_t nb = cfg.rpo_blocks.size();
  const RegMask fixed_mask = (RegMask{1} << 0) | (RegMask{1} << 28) | (RegMask{1} << 29) |
                             (cfg.sideband.frame_pointer_needed ? (RegMask{1} << 30) : RegMask{0});

  std::vector<std::vector<size_t>> block_preds(nb);
  for (size_t src_b = 0; src_b < nb; ++src_b) {
    ForEachStage0CfgSucc(cfg.rpo_blocks[src_b], nb, [&](size_t succ) {
      if (std::find(block_preds[succ].begin(), block_preds[succ].end(), src_b) ==
          block_preds[succ].end()) {
        block_preds[succ].push_back(src_b);
      }
    });
  }

  const Stage0RmwCtx ctx{
      .cfg = cfg,
      .df = df,
      .b = b,
      .all_bp = all_bp,
      .esums = esums,
      .live_after_step = live_after_step,
      .caller_save_offsets = caller_save_offsets,
      .proven_spill_offsets = proven_spill_offsets,
      .fixed_mask = fixed_mask,
      .block_preds = std::move(block_preds),
      .blk_parsed = blk_parsed,
  };

  struct DeferredLocalCand {
    size_t s = 0;
    int hr = -1;
    const RtlExpr* dst = nullptr;
    const RtlExpr* rhs = nullptr;
    RtlExpr read_reg_expr;
    RegMask hr_m = 0;
    bool direct_non_mem_read = false;
    bool mem_addr_read = false;
    bool is_callee_saved_hr = false;
    bool is_pure_entry_arg = false;
  };
  std::vector<DeferredLocalCand> deferred_local_cands;
  bool any_case_c_attached = false;

  const RtlMachineMode ptr_mode = df.gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi;
  for (size_t s = 0; s < ns; ++s) {
    const RtlExpr& pat = steps[s].pn->pattern;
    if (esums[s].is_call || IsAnyControlTransferPattern(pat) ||
        HasStage0LibcallOrRetvalNote(steps[s])) {
      continue;
    }
    const RtlExpr* single_set = ExtractStage0SingleSet(pat);
    if (single_set == nullptr) {
      continue;
    }
    const RtlExpr& dst = single_set->SetDest();
    const RtlExpr& rhs = single_set->SetSrc();
    // `optabs.c:2485-2525` + `flow.c:2507-2550`: when a `REG_NO_CONFLICT`
    // sign-extension high-word step `(set (subreg:SI (reg:DI r) 0) (ashiftrt:SI
    // (reg:SI r) 31))` overwrites `r`, `r` (the source pseudo) dies on this step
    // even when liveness is rebuilt after `RestoreNoConflictExtendClobbers`.
    if (dst.op == RtlOp::kSubreg && dst.int_val == 0 && dst.children.size() == 1 &&
        dst.children[0].IsHardReg() && dst.children[0].mode == RtlMachineMode::kDi &&
        rhs.op == RtlOp::kAshiftrt && rhs.children.size() == 2 && rhs.children[0].IsHardReg() &&
        rhs.children[0].mode == RtlMachineMode::kSi &&
        rhs.children[0].RegNo() == dst.children[0].RegNo() &&
        HasRegNote(steps[s].pn->reg_notes, RtlMachineMode::kRegNoConflict,
                   dst.children[0].RegNo())) {
      const int sr = dst.children[0].RegNo();
      if (!HasRegNote(steps[s].pn->reg_notes, RtlMachineMode::kRegDead, sr)) {
        auto& notes = steps[s].pn->reg_notes;
        auto nc_it = std::find_if(notes.begin(), notes.end(), [&](const RtlRegNote& n) {
          return n.kind == RtlMachineMode::kRegNoConflict && n.datum.IsReg() &&
                 n.datum.RegNo() == sr;
        });
        notes.insert(nc_it, RtlRegNote{.kind = RtlMachineMode::kRegDead,
                                       .list_op = RtlOp::kExprList,
                                       .datum = rhs.children[0]});
        blk_parsed->dirty[steps[s].insn_idx] = true;
      }
      continue;
    }
    if (!dst.IsHardReg() || dst.flags.volatil || rhs.IsReg()) {
      continue;
    }
    const int hr = dst.RegNo();
    if (hr < 2 || hr > 63 || hr == 28 || hr == 29 || hr == 31 ||
        (cfg.sideband.frame_pointer_needed && hr == 30)) {
      continue;
    }
    const bool is_callee_saved_hr = (hr >= 16 && hr <= 23) || hr == 30 || (hr >= 52 && hr <= 63);
    const int dst_nregs = HardRegNregs(hr, dst.mode, df.gp64, df.fp64);
    const RegMask hr_m = RegBitMask(hr, dst.mode, df.gp64, df.fp64);

    // `flow.c:2507-2550`: a non-integrated multi-word load `(set (reg:DI hr)
    // (mem:DI ... (reg:SI ptr_r) ...))` whose `ptr_mode` base register `ptr_r`
    // overlaps `[hr, hr + dst_nregs)` overwrites `ptr_r` with a distinct wider
    // mode, so `ptr_r` cannot share a pseudo with `dst` and always dies on `s`.
    if (!dst.flags.integrated && dst_nregs == 2 && rhs.op == RtlOp::kMem && dst.mode != ptr_mode &&
        !HasRegNote(steps[s].pn->reg_notes, RtlMachineMode::kRegDead, hr) &&
        !HasRegNote(steps[s].pn->reg_notes, RtlMachineMode::kRegDead, hr + 1)) {
      RtlExpr ptr_expr;
      rhs.Visit([&](const RtlExpr& n) {
        if (!ptr_expr.IsHardReg() && n.IsHardReg() && n.mode == ptr_mode && !n.flags.volatil &&
            !n.flags.integrated && (n.RegNo() == hr || n.RegNo() == hr + 1)) {
          ptr_expr = n;
        }
      });
      if (ptr_expr.IsHardReg()) {
        const int pr = ptr_expr.RegNo();
        if (pr >= 2 && pr < 32 && pr != 28 && pr != 29 && pr != 31 &&
            !(cfg.sideband.frame_pointer_needed && pr == 30)) {
          ctx.AttachDeadNote(s, pr, ptr_expr);
          continue;
        }
      }
    }

    if (!dst.flags.integrated && dst.mode == RtlMachineMode::kSi && rhs.op == RtlOp::kAnd &&
        rhs.children.size() == 2 && rhs.children[0].op == RtlOp::kSubreg &&
        !rhs.children[0].children.empty() && rhs.children[0].children[0].IsHardReg() &&
        rhs.children[0].children[0].RegNo() == hr &&
        (rhs.children[0].children[0].mode == RtlMachineMode::kQi ||
         rhs.children[0].children[0].mode == RtlMachineMode::kHi) &&
        rhs.children[1].IsConstInt()) {
      ctx.AttachDeadNote(s, hr, rhs.children[0].children[0]);
      continue;
    }

    bool direct_non_mem_read = false;
    bool mem_addr_read = false;
    RtlExpr read_reg_expr;
    auto check_rhs = [&](const auto& self, const RtlExpr& e, bool in_mem, bool in_subreg) -> void {
      if (e.op == RtlOp::kMem) {
        for (const RtlExpr& ch : e.children) {
          self(self, ch, /*in_mem=*/true, /*in_subreg=*/false);
        }
        return;
      }
      if (e.op == RtlOp::kSubreg) {
        for (const RtlExpr& ch : e.children) {
          self(self, ch, in_mem, /*in_subreg=*/true);
        }
        return;
      }
      if (!in_mem && e.IsHardReg() && e.RegNo() == hr && !e.flags.volatil && !e.flags.integrated &&
          ((e.mode == dst.mode && e.raw_mode == dst.raw_mode) ||
           (!in_subreg && e.mode != dst.mode))) {
        if (!direct_non_mem_read) {
          read_reg_expr = e;
        }
        direct_non_mem_read = true;
        return;
      }
      if (in_mem &&
          ((!dst.flags.integrated && dst_nregs == 1) || (dst.flags.integrated && hr == 2)) &&
          e.IsHardReg() && e.RegNo() == hr && e.mode == ptr_mode && !e.flags.volatil &&
          !e.flags.integrated) {
        if (!direct_non_mem_read && !mem_addr_read) {
          read_reg_expr = e;
        }
        mem_addr_read = true;
        return;
      }
      for (const RtlExpr& ch : e.children) {
        self(self, ch, in_mem, in_subreg);
      }
    };
    check_rhs(check_rhs, rhs, /*in_mem=*/false, /*in_subreg=*/false);
    if (!direct_non_mem_read && !mem_addr_read) {
      continue;
    }
    const RegMask read_m = RegBitMask(hr, read_reg_expr.mode, df.gp64, df.fp64);
    const RegMask mask_union = hr_m | read_m;
    if (!HasStage0OnlyUnpinnedReadsOfMask(rhs, df, hr, read_reg_expr.mode, mask_union)) {
      continue;
    }

    const bool is_abi_arg_hr =
        (hr >= 4 && hr <= (df.gp64 ? 11 : 7)) || (hr >= 44 && hr <= (df.gp64 ? 51 : 47));
    auto check_pure_entry_abi_arg_before_s = [&](RegMask arg_m,
                                                 std::vector<bool>* out_any_unmod_out) {
      if (!is_abi_arg_hr) {
        return false;
      }
      for (size_t p = 0; p < s; ++p) {
        if ((esums[p].defs & arg_m) != 0 || HasStage0LibcallOrRetvalNote(steps[p])) {
          return false;
        }
      }
      std::vector<bool> any_unmod_in;
      std::vector<bool> all_unmod_in;
      ComputeStage0EntryAbiArgUnmodifiedReach(cfg, df, all_bp, ctx.block_preds, arg_m,
                                              &any_unmod_in, out_any_unmod_out, &all_unmod_in);
      return b < nb && all_unmod_in[b];
    };

    const bool has_seq_head =
        steps[s].in_slot && s + 1 < ns && steps[s + 1].insn_idx == steps[s].insn_idx;
    const bool slot_in_struct =
        steps[s].in_slot && blk_parsed->seq_slot_wraps[steps[s].insn_idx].flags.in_struct;

    // Handle eager-stolen conditional-branch delay slots (`in_struct` or branch
    // condition itself reads `read_m` before the slot executes).
    if (has_seq_head && !esums[s + 1].is_call && !dst.flags.integrated &&
        (slot_in_struct || ((esums[s + 1].uses | esums[s + 1].defs) & mask_union) != 0)) {
      if ((esums[s + 1].defs & mask_union) != 0) {
        continue;
      }
      const RtlExpr& hpat = steps[s + 1].pn->pattern;
      if (!hpat.IsSet() || hpat.SetDest().op != RtlOp::kPc ||
          hpat.SetSrc().op != RtlOp::kIfThenElse || cfg.rpo_blocks[b].successors.size() != 2) {
        continue;
      }
      size_t succ_owner = nb;
      if (slot_in_struct) {
        for (size_t succ : cfg.rpo_blocks[b].successors) {
          if (succ != b + 1 && succ < nb) {
            succ_owner = succ;
            break;
          }
        }
      } else {
        size_t live_succ_cnt = 0;
        for (size_t succ : cfg.rpo_blocks[b].successors) {
          if (succ < nb && (df.live_in[succ] & hr_m) != 0) {
            succ_owner = succ;
            ++live_succ_cnt;
          }
        }
        if (live_succ_cnt != 1) {
          succ_owner = nb;
        }
      }
      if (succ_owner >= nb || succ_owner == b) {
        continue;
      }
      std::vector<bool> any_unmod_out;
      if (check_pure_entry_abi_arg_before_s(read_m, &any_unmod_out)) {
        if (IsStage0UnpinnedOutWebFromStep(ctx, succ_owner, 0, hr, dst.mode, hr_m,
                                           /*require_global=*/false, &any_unmod_out)) {
          ctx.AttachDeadNote(s, hr, read_reg_expr);
        }
        continue;
      }
      if (HasValidStage0PreStepInWeb(ctx, s, hr, read_reg_expr.mode, read_m,
                                     /*allow_pure_entry_abi_arg=*/false) &&
          (read_reg_expr.mode != dst.mode ||
           CheckStage0LocalOutTail(ctx, succ_owner, 0, hr, dst.mode, dst.flags, hr_m,
                                   is_callee_saved_hr, /*join_slot_pat=*/nullptr))) {
        ctx.AttachDeadNote(s, hr, read_reg_expr);
      }
      continue;
    }

    if (slot_in_struct && !dst.flags.integrated) {
      continue;
    }

    // Outgoing ABI call-argument RMW feeding an immediately following `call_insn`
    // (either in the call's delay slot or right before the call in `b`).
    if (!dst.flags.integrated && (direct_non_mem_read || mem_addr_read) && is_abi_arg_hr) {
      size_t call_s = ns;
      if (has_seq_head && esums[s + 1].is_call) {
        call_s = s + 1;
      } else if (!steps[s].in_slot) {
        for (size_t k = s + 1; k < ns; ++k) {
          if (esums[k].is_call) {
            call_s = k;
            break;
          }
          if (((esums[k].defs | esums[k].uses) & mask_union) != 0 ||
              IsAnyControlTransferPattern(steps[k].pn->pattern)) {
            break;
          }
        }
      }
      // Mirror `flow.c:2507-2550` (`flow.c:2511`) / `sched.c:2872`: pass `allow_pure_entry_abi_arg
      // = false` because an unmodified incoming ABI argument directly rewritten into the same
      // outgoing call argument register (`a0 = a0 + 5 ; call(a0)` or `a0 = *(a0 + 4) ; call(a0)`)
      // has both source and destination ABI-pinned to `hr` (never distinct pseudos), and must not
      // fall through to Case B. Also follow an unconditional single-successor jump/fallthrough
      // when `jump2` cross-jumped the trailing `call_insn` into the successor block.
      bool feeds_call_arg = call_s < ns && (esums[call_s].uses & hr_m) != 0 &&
                            (live_after_step[call_s] & mask_union) == 0;
      if (!feeds_call_arg && call_s >= ns && cfg.rpo_blocks[b].successors.size() == 1) {
        bool reaches_end_untouched = true;
        for (size_t k = has_seq_head ? (s + 2) : (s + 1); k < ns; ++k) {
          if (esums[k].is_call || ((esums[k].defs | esums[k].uses) & mask_union) != 0) {
            reaches_end_untouched = false;
            break;
          }
        }
        const size_t succ_b = cfg.rpo_blocks[b].successors[0];
        if (reaches_end_untouched && succ_b < nb && succ_b != b) {
          const auto& succ_steps = ctx.StepsOf(succ_b);
          for (size_t sk = 0; sk < succ_steps.size(); ++sk) {
            const ElemSummary ses = ctx.SummaryOf(succ_b, sk);
            if (ses.is_call) {
              if ((ses.uses & hr_m) != 0) {
                RegMask live_after_sk = df.live_out[succ_b];
                for (size_t rev = succ_steps.size(); rev > sk + 1; --rev) {
                  const ElemSummary tail = ctx.SummaryOf(succ_b, rev - 1);
                  live_after_sk = (live_after_sk & ~tail.defs) | tail.uses;
                }
                if ((live_after_sk & mask_union) == 0) {
                  feeds_call_arg = true;
                }
              }
              break;
            }
            if (((ses.defs | ses.uses) & mask_union) != 0 ||
                IsAnyControlTransferPattern(succ_steps[sk].pn->pattern)) {
              break;
            }
          }
        }
      }
      if (feeds_call_arg) {
        if (HasValidStage0PreStepInWeb(ctx, s, hr, read_reg_expr.mode, read_m,
                                       /*allow_pure_entry_abi_arg=*/false)) {
          ctx.AttachDeadNote(s, hr, read_reg_expr);
        }
        continue;
      }
    }

    if (has_seq_head && esums[s + 1].is_call &&
        ((esums[s + 1].defs | esums[s + 1].uses) & mask_union) != 0) {
      continue;
    }
    if (has_seq_head && !esums[s + 1].is_call && !dst.flags.integrated &&
        ((esums[s + 1].uses | esums[s + 1].defs) & mask_union) != 0) {
      continue;
    }

    // Case A: Return-register RMW `(set (reg/i:M1 hr) <op ... (reg:M2 hr) ...>)`
    // or return-value load from unpinned pointer in `hr == 2`.
    if (dst.flags.integrated) {
      if ((!direct_non_mem_read && !mem_addr_read) ||
          !(hr == 2 || (direct_non_mem_read && hr == 32))) {
        continue;
      }
      if (steps[s].in_slot && IsStage0ReorgOppositeReturnSlotComp(cfg, b, pat)) {
        continue;
      }
      const RtlMachineMode req_mode = read_reg_expr.mode;
      const std::string& req_raw_mode = read_reg_expr.raw_mode;
      const RegMask req_m = read_m;
      auto expr_has_mem = [](const RtlExpr& e) {
        bool found = false;
        e.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kMem) {
            found = true;
          }
        });
        return found;
      };
      bool valid_ret_def = false;
      bool saw_prior_write_or_bad = false;
      bool saw_unpinned_rmw = false;
      bool chain_reads_external = ((esums[s].uses & ~req_m) != 0) || expr_has_mem(rhs);
      for (size_t rev = s; rev > 0; --rev) {
        const size_t p = rev - 1;
        const RtlExpr& ppat = steps[p].pn->pattern;
        if (steps[s].in_slot && ppat.op == RtlOp::kUse) {
          continue;
        }
        if (steps[p].in_slot || (!esums[p].is_call && IsAnyControlTransferPattern(ppat))) {
          saw_prior_write_or_bad = true;
          break;
        }
        if (esums[p].is_call && (esums[p].defs & req_m) == 0 && (esums[p].uses & req_m) != 0) {
          saw_prior_write_or_bad = true;
          break;
        }
        if (HasStage0LibcallOrRetvalNote(steps[p])) {
          if (((esums[p].defs | esums[p].uses) & req_m) == 0) {
            continue;
          }
          if (saw_unpinned_rmw && (esums[p].defs & req_m) != 0 &&
              std::any_of(
                  steps[p].pn->reg_notes.begin(), steps[p].pn->reg_notes.end(),
                  [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegRetval; })) {
            valid_ret_def = true;
            break;
          }
          saw_prior_write_or_bad = true;
          break;
        }
        if ((esums[p].defs & req_m) != 0) {
          saw_prior_write_or_bad = true;
          if (saw_unpinned_rmw && esums[p].is_call && (esums[p].explicit_sets & req_m) == req_m) {
            valid_ret_def = true;
            break;
          }
          const RtlExpr* pss = ExtractStage0SingleSet(ppat);
          if (!esums[p].is_call && pss != nullptr && pss->SetDest().IsHardReg() &&
              pss->SetDest().RegNo() == hr && pss->SetDest().mode == req_mode &&
              pss->SetDest().raw_mode == req_raw_mode && !pss->SetDest().flags.volatil &&
              !pss->SetDest().flags.integrated && !ctx.IsSpillOrCsLoadExpr(pss->SetSrc()) &&
              (!pss->SetSrc().IsConstInt() || chain_reads_external)) {
            if ((esums[p].uses & req_m) == 0 ||
                HasRegNote(steps[p].pn->reg_notes, RtlMachineMode::kRegDead, hr)) {
              valid_ret_def = true;
              break;
            }
            if (!pss->SetSrc().IsReg() &&
                HasStage0OnlyUnpinnedReadsOfMask(ppat, df, hr, req_mode, req_m)) {
              saw_unpinned_rmw = true;
              chain_reads_external = chain_reads_external || ((esums[p].uses & ~req_m) != 0) ||
                                     expr_has_mem(pss->SetSrc());
              continue;
            }
          }
          if (!df.gp64 && saw_unpinned_rmw && hr == 2 && req_mode == RtlMachineMode::kDi &&
              !esums[p].is_call && p > 0 && !esums[p - 1].is_call && !steps[p - 1].in_slot) {
            const RtlExpr* pss0 = ExtractStage0SingleSet(ppat);
            const RtlExpr* pss1 = ExtractStage0SingleSet(steps[p - 1].pn->pattern);
            if (pss0 != nullptr && pss1 != nullptr && pss0->SetDest().IsHardReg() &&
                pss0->SetDest().mode == RtlMachineMode::kSi && !pss0->SetDest().flags.integrated &&
                pss1->SetDest().IsHardReg() && pss1->SetDest().mode == RtlMachineMode::kSi &&
                !pss1->SetDest().flags.integrated &&
                ((pss0->SetDest().RegNo() == 2 && pss1->SetDest().RegNo() == 3) ||
                 (pss0->SetDest().RegNo() == 3 && pss1->SetDest().RegNo() == 2))) {
              valid_ret_def = true;
              break;
            }
          }
          break;
        }
        if ((esums[p].uses & req_m) != 0 &&
            !HasStage0OnlyUnpinnedReadsOfMask(ppat, df, hr, req_mode, req_m)) {
          saw_prior_write_or_bad = true;
          break;
        }
      }
      if (!valid_ret_def && !saw_prior_write_or_bad) {
        valid_ret_def = HasStage0UnpinnedGlobalInWeb(ctx, b, hr, req_mode, req_m);
      }
      if (valid_ret_def) {
        // A loop-carried return-register accumulator `(set (reg/i:M hr) (op (reg:M hr) ...))`
        // (either inside the loop or peeled before a loop header by `reorg.c` delay-slot
        // stealing) cannot be a dying pseudo `p` feeding `(reg/i:M hr)` when `hr_m` flows
        // unkilled into a `hr_m`-live CFG cycle reading `hr_m` (`sched.c:2872`).
        bool is_loop_carried_ret_acc = false;
        const size_t after_s = has_seq_head ? (s + 2) : (s + 1);
        bool killed_after_in_b = false;
        for (size_t k = after_s; k < ns; ++k) {
          if ((esums[k].defs & hr_m) == hr_m) {
            killed_after_in_b = true;
            break;
          }
        }
        bool killed_before_in_b = false;
        for (size_t k = 0; k < s; ++k) {
          if ((esums[k].defs & hr_m) == hr_m) {
            killed_before_in_b = true;
            break;
          }
        }
        auto block_in_hr_live_cycle = [&](size_t start_b) -> bool {
          if ((df.live_out[start_b] & hr_m) == 0) {
            return false;
          }
          std::vector<bool> cvis(nb, false);
          std::vector<size_t> cq;
          ForEachStage0CfgSucc(cfg.rpo_blocks[start_b], nb, [&](size_t succ) {
            if (succ == start_b) {
              cvis[start_b] = true;
              return;
            }
            if (!cvis[succ] && (df.live_in[succ] & hr_m) != 0) {
              cvis[succ] = true;
              cq.push_back(succ);
            }
          });
          if (cvis[start_b]) {
            return true;
          }
          for (size_t ci = 0; ci < cq.size(); ++ci) {
            const size_t cb = cq[ci];
            if ((df.live_out[cb] & hr_m) == 0) {
              continue;
            }
            ForEachStage0CfgSucc(cfg.rpo_blocks[cb], nb, [&](size_t succ) {
              if (succ == start_b) {
                cvis[start_b] = true;
                return;
              }
              if (!cvis[succ] && (df.live_in[succ] & hr_m) != 0) {
                cvis[succ] = true;
                cq.push_back(succ);
              }
            });
            if (cvis[start_b]) {
              return true;
            }
          }
          return false;
        };
        if (!killed_after_in_b && (df.live_out[b] & hr_m) != 0) {
          std::vector<bool> vis(nb, false);
          std::vector<size_t> q;
          const auto& b_succs = cfg.rpo_blocks[b].successors;
          const bool slot_only_taken =
              slot_in_struct && has_seq_head && b + 1 < nb &&
              std::any_of(b_succs.begin(), b_succs.end(), [b](size_t sb) { return sb != b + 1; });
          ForEachStage0CfgSucc(cfg.rpo_blocks[b], nb, [&](size_t succ) {
            if (slot_only_taken && succ == b + 1) {
              return;
            }
            if (succ == b && !killed_before_in_b) {
              is_loop_carried_ret_acc = true;
              return;
            }
            if (!vis[succ] && (df.live_in[succ] & hr_m) != 0) {
              vis[succ] = true;
              q.push_back(succ);
            }
          });
          for (size_t qi = 0; qi < q.size() && !is_loop_carried_ret_acc; ++qi) {
            const size_t cur_b = q[qi];
            const auto& cur_steps = ctx.StepsOf(cur_b);
            bool reads_hr_here = false;
            bool killed_here = false;
            for (size_t k = 0; k < cur_steps.size(); ++k) {
              const ElemSummary kes = ctx.SummaryOf(cur_b, k);
              if ((kes.uses & hr_m) != 0 && cur_steps[k].pn->pattern.op != RtlOp::kUse) {
                reads_hr_here = true;
                break;
              }
              if ((kes.defs & hr_m) == hr_m) {
                killed_here = true;
                break;
              }
            }
            if (reads_hr_here && block_in_hr_live_cycle(cur_b)) {
              is_loop_carried_ret_acc = true;
              break;
            }
            if (!killed_here && (df.live_out[cur_b] & hr_m) != 0) {
              ForEachStage0CfgSucc(cfg.rpo_blocks[cur_b], nb, [&](size_t succ) {
                if (succ == b && !killed_before_in_b) {
                  is_loop_carried_ret_acc = true;
                  return;
                }
                if (!vis[succ] && (df.live_in[succ] & hr_m) != 0) {
                  vis[succ] = true;
                  q.push_back(succ);
                }
              });
            }
          }
        }
        if (!is_loop_carried_ret_acc) {
          ctx.AttachDeadNote(s, hr, read_reg_expr);
        }
      }
      continue;
    }

    // Mode-changing non-integrated RMW `(set (reg:M1 hr) (<op> ... (reg:M2 hr) ...))`
    // (`M1 != M2` without `subreg`): in `.08.lreg` two distinct modes cannot share a
    // pseudo, so `(reg:M2 hr)` died on `s` whenever `(reg:M2 hr)` was an unpinned
    // pseudo (or unmodified incoming ABI arg with unpinned `dst` pseudo) and not a
    // cross-class input reload scratch.
    if (direct_non_mem_read && read_reg_expr.mode != dst.mode) {
      const size_t after_s = has_seq_head ? (s + 2) : (s + 1);
      const size_t live_check_s = has_seq_head ? (s + 1) : s;
      if ((live_after_step[live_check_s] & (read_m & ~hr_m)) != 0) {
        continue;
      }
      bool is_cross_class_reload = false;
      bool saw_call_between = false;
      for (size_t rev = s; rev > 0; --rev) {
        const size_t p = rev - 1;
        if (esums[p].is_call) {
          saw_call_between = true;
        }
        if ((esums[p].defs & read_m) != 0) {
          const RtlExpr* pss = ExtractStage0SingleSet(steps[p].pn->pattern);
          if (!saw_call_between && !is_callee_saved_hr && pss != nullptr &&
              pss->SetDest().IsHardReg() && pss->SetDest().RegNo() == hr &&
              pss->SetSrc().IsHardReg() && !pss->SetSrc().IsArgPointerReg() &&
              ((hr < 32) != (pss->SetSrc().RegNo() < 32))) {
            is_cross_class_reload = true;
          }
          break;
        }
        if ((esums[p].uses & read_m) != 0) {
          break;
        }
      }
      if (!is_cross_class_reload) {
        if (HasValidStage0PreStepInWeb(ctx, s, hr, read_reg_expr.mode, read_m,
                                       /*allow_pure_entry_abi_arg=*/false)) {
          ctx.AttachDeadNote(s, hr, read_reg_expr);
          continue;
        }
        std::vector<bool> any_unmod_out;
        if (check_pure_entry_abi_arg_before_s(read_m, &any_unmod_out) &&
            (IsStage0UnpinnedOutWebFromStep(ctx, b, after_s, hr, dst.mode, hr_m,
                                            /*require_global=*/false, &any_unmod_out) ||
             CheckStage0LocalOutTail(ctx, b, after_s, hr, dst.mode, dst.flags, hr_m,
                                     is_callee_saved_hr, /*join_slot_pat=*/nullptr))) {
          ctx.AttachDeadNote(s, hr, read_reg_expr);
          continue;
        }
      }
      continue;
    }

    // Case B: Unmodified incoming ABI argument RMW in any block `b` whose
    // destination lifts to an unpinned pseudo web that never merges back with
    // unmodified entry `hr`.
    bool is_pure_entry_arg = false;
    {
      std::vector<bool> any_unmod_out;
      if (check_pure_entry_abi_arg_before_s(hr_m, &any_unmod_out)) {
        is_pure_entry_arg = true;
        const size_t after_s = has_seq_head ? (s + 2) : (s + 1);
        if (IsStage0UnpinnedOutWebFromStep(ctx, b, after_s, hr, dst.mode, hr_m,
                                           /*require_global=*/false, &any_unmod_out)) {
          ctx.AttachDeadNote(s, hr, read_reg_expr);
          continue;
        }
      }
    }

    // Case D: Global-in pseudo web overwritten by a purely local RMW in `b` (or
    // narrow byte/halfword load web widened by `zero_extend`/`and 0xff|0xffff`).
    if (!slot_in_struct) {
      bool prior_write_or_bad = false;
      for (size_t p = 0; p < s; ++p) {
        if ((esums[p].defs & hr_m) != 0 || (esums[p].is_call && (esums[p].uses & hr_m) != 0) ||
            IsAnyControlTransferPattern(steps[p].pn->pattern) ||
            HasStage0LibcallOrRetvalNote(steps[p])) {
          prior_write_or_bad = true;
          break;
        }
        if ((esums[p].uses & hr_m) != 0 &&
            !HasStage0OnlyUnpinnedReadsOfMask(steps[p].pn->pattern, df, hr, read_reg_expr.mode,
                                              hr_m)) {
          prior_write_or_bad = true;
          break;
        }
      }
      bool all_narrow_loads = false;
      if (!prior_write_or_bad &&
          HasStage0UnpinnedGlobalInWeb(ctx, b, hr, read_reg_expr.mode, hr_m, &all_narrow_loads)) {
        const size_t after_s = has_seq_head ? (s + 2) : (s + 1);
        const RtlExpr* join_pat = steps[s].in_slot ? nullptr : &pat;
        const bool narrow_widen_rmw =
            all_narrow_loads && direct_non_mem_read && rhs.op == RtlOp::kAnd &&
            rhs.children.size() == 2 && rhs.children[1].IsConstInt() &&
            (rhs.children[1].int_val == 255 || rhs.children[1].int_val == 65535);
        bool fallthrough_local_tail = false;
        if (!narrow_widen_rmw && after_s == ns && has_seq_head && !esums[s + 1].is_call &&
            cfg.rpo_blocks[b].successors.size() == 2 && b + 1 < nb &&
            (df.live_in[b + 1] & hr_m) != 0) {
          bool live_on_taken = false;
          for (size_t succ : cfg.rpo_blocks[b].successors) {
            if (succ != b + 1 && succ < nb && (df.live_in[succ] & hr_m) != 0) {
              live_on_taken = true;
            }
          }
          if (!live_on_taken) {
            fallthrough_local_tail =
                CheckStage0LocalOutTail(ctx, b + 1, 0, hr, dst.mode, dst.flags, hr_m,
                                        is_callee_saved_hr, /*join_slot_pat=*/nullptr);
          }
        }
        if (narrow_widen_rmw || fallthrough_local_tail ||
            (CheckStage0LocalOutTail(ctx, b, after_s, hr, dst.mode, dst.flags, hr_m,
                                     is_callee_saved_hr, join_pat) &&
             !HasLowerRegAllocOrderGprFreeAcrossLocalOutTail(ctx, b, s, after_s, hr, dst.mode, hr_m,
                                                             is_callee_saved_hr))) {
          ctx.AttachDeadNote(s, hr, read_reg_expr);
          continue;
        }
      }
    }

    if (TryAttachStage0LoadOrLocalScratchDeadNote(ctx, s, hr, dst, rhs, read_reg_expr, hr_m,
                                                  direct_non_mem_read, mem_addr_read,
                                                  is_callee_saved_hr, is_pure_entry_arg)) {
      continue;
    }

    if (TryAttachStage0CaseCDeadNote(ctx, s, hr, dst, rhs, read_reg_expr, hr_m, direct_non_mem_read,
                                     is_callee_saved_hr)) {
      any_case_c_attached = true;
    } else if (direct_non_mem_read) {
      deferred_local_cands.push_back(DeferredLocalCand{
          .s = s,
          .hr = hr,
          .dst = &dst,
          .rhs = &rhs,
          .read_reg_expr = read_reg_expr,
          .hr_m = hr_m,
          .direct_non_mem_read = direct_non_mem_read,
          .mem_addr_read = mem_addr_read,
          .is_callee_saved_hr = is_callee_saved_hr,
          .is_pure_entry_arg = is_pure_entry_arg,
      });
    }
  }
  // When `TryAttachStage0CaseCDeadNote` attached `REG_DEAD hr` to the final live-out
  // step in `b`, earlier local dying-input steps in the same chain now terminate at
  // that `REG_DEAD` kill (`is_dead_rmw_overwrite`) and can be split cleanly.
  if (any_case_c_attached && !deferred_local_cands.empty()) {
    for (const DeferredLocalCand& cand : deferred_local_cands) {
      if (HasRegNote(steps[cand.s].pn->reg_notes, RtlMachineMode::kRegDead, cand.hr)) {
        continue;
      }
      TryAttachStage0LoadOrLocalScratchDeadNote(ctx, cand.s, cand.hr, *cand.dst, *cand.rhs,
                                                cand.read_reg_expr, cand.hr_m,
                                                cand.direct_non_mem_read, cand.mem_addr_read,
                                                cand.is_callee_saved_hr, cand.is_pure_entry_arg);
    }
  }
}

bool IsStage0DyingPseudoOverwrittenBySpillOutputReload(
    const RtlControlFlowGraph& cfg, const DataflowResult& df, size_t b, size_t pi, int scr,
    RegMask scr_m, const std::vector<Stage0StepRef>& steps, const std::vector<ElemSummary>& esums,
    const absl::flat_hash_set<int64_t>& proven_spill_offsets) {
  if (steps[pi].in_slot || (esums[pi].uses & scr_m) == 0 || scr < 2 || scr > 63 || scr == 28 ||
      scr == 29 || (cfg.sideband.frame_pointer_needed && scr == 30)) {
    return false;
  }
  const bool caller_savable =
      (scr >= 2 && scr <= 15) || scr == 24 || scr == 25 || (scr >= 32 && scr <= 51);
  for (size_t rev = pi; rev > 0; --rev) {
    const size_t ei = rev - 1;
    const RtlExpr& epat = steps[ei].pn->pattern;
    int64_t moff = 0, msz = 0;
    if (epat.IsSet() && epat.SetSrc().IsReg() && epat.SetSrc().RegNo() == scr &&
        ExtractStage0SpMem(epat.SetDest(), &moff, &msz) && proven_spill_offsets.contains(moff)) {
      return false;
    }
    if ((esums[ei].defs & scr_m) != 0) {
      if ((esums[ei].explicit_sets & scr_m) == 0) {
        return false;
      }
      if (epat.IsSet()) {
        if (ExtractStage0SpMem(epat.SetSrc(), &moff, &msz) && proven_spill_offsets.contains(moff)) {
          return false;
        }
        if (IsStage0ConstantReloadSrc(epat.SetSrc()) ||
            (epat.SetSrc().IsHardReg() && ((scr >= 32) != (epat.SetSrc().RegNo() >= 32)))) {
          return false;
        }
      }
      return true;
    }
    if (esums[ei].is_call && caller_savable) {
      return false;
    }
  }
  return (df.live_in[b] & scr_m) != 0;
}

bool StripStage0RegDeadOnStep(Stage0BlockParsed* blk_parsed, Stage0StepRef* st, int regno) {
  auto& notes = st->pn->reg_notes;
  if (st->in_slot) {
    for (auto it = notes.begin(); it != notes.end(); ++it) {
      if (it->kind == RtlMachineMode::kRegDead && it->datum.IsConstInt() &&
          it->datum.int_val == 0) {
        notes.erase(it);
        blk_parsed->dirty[st->insn_idx] = true;
        return true;
      }
    }
    return false;
  }
  const size_t before = notes.size();
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [regno](const RtlRegNote& n) {
                               return n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                                      !n.datum.IsArgPointerReg() && n.datum.RegNo() == regno;
                             }),
              notes.end());
  if (notes.size() != before) {
    blk_parsed->dirty[st->insn_idx] = true;
    return true;
  }
  return false;
}

void NormalizeStage0MdFprAndDiReloadNotes(const RtlControlFlowGraph& cfg, const DataflowResult& df,
                                          bool is_optimized, bool runs_sched2, bool expensive_opts,
                                          size_t b, const std::vector<ElemSummary>& esums,
                                          const std::vector<RegMask>& live_after_step,
                                          const absl::flat_hash_set<size_t>& prologue_save_steps,
                                          const absl::flat_hash_set<int64_t>& proven_spill_offsets,
                                          std::vector<Stage0BlockParsed>* bp_ptr,
                                          RegMask* out_proven_fp_spill_regs = nullptr) {
  auto& bp = *bp_ptr;
  const size_t nb = cfg.rpo_blocks.size();
  auto& steps = bp[b].steps;
  const size_t ns = steps.size();

  absl::flat_hash_set<int> block_use_regs;
  for (const Stage0StepRef& st : steps) {
    if (st.pn->pattern.op == RtlOp::kUse && st.pn->pattern.children.size() == 1 &&
        st.pn->pattern.children[0].IsReg()) {
      const int ur = st.pn->pattern.children[0].RegNo();
      const int nw = HardRegNregs(ur, st.pn->pattern.children[0].mode, df.gp64, df.fp64);
      for (int w = 0; w < nw; ++w) block_use_regs.insert(ur + w);
    }
  }

  struct Stage0MdNotePromo {
    size_t use_s = 0;
    int gpr_r = -1;
    int md_r = -1;
  };
  std::vector<Stage0MdNotePromo> md_note_promos;
  absl::flat_hash_set<size_t> covered_md_steps;

  // 1. `MD_REGS` (`64`/`65`) output / input reload moves after single-`SET` `mult`/`truncate`.
  for (size_t s = 0; s + 1 < ns; ++s) {
    const RtlExpr& p0 = steps[s].pn->pattern;
    if (!df.gp64 && p0.op == RtlOp::kParallel && p0.children.size() == 2 &&
        p0.children[0].IsSet() && p0.children[1].op == RtlOp::kClobber &&
        p0.children[0].SetDest().IsReg() && p0.children[0].SetDest().mode == RtlMachineMode::kDi &&
        p0.children[0].SetDest().RegNo() == 64 && p0.children[0].SetSrc().op == RtlOp::kMult) {
      const RegMask md_pair = (RegMask{1} << 64) | (RegMask{1} << 65);
      size_t mv_s = ns;
      for (size_t k = s + 1; k < ns; ++k) {
        const RtlExpr& pk = steps[k].pn->pattern;
        if (pk.IsSet() && pk.SetDest().IsReg() && pk.SetDest().mode == RtlMachineMode::kDi &&
            pk.SetSrc().IsReg() && pk.SetSrc().mode == RtlMachineMode::kDi &&
            pk.SetSrc().RegNo() == 64 && pk.SetDest().RegNo() >= 2 && pk.SetDest().RegNo() <= 24) {
          mv_s = k;
          break;
        }
        if (esums[k].is_call || IsAnyControlTransferPattern(pk) ||
            ((esums[k].uses | esums[k].defs) & md_pair) != 0) {
          break;
        }
      }
      if (mv_s < ns && (live_after_step[mv_s] & md_pair) == 0) {
        StripStage0RegDeadOnStep(&bp[b], &steps[mv_s], 64);
        const int dst_r = steps[mv_s].pn->pattern.SetDest().RegNo();
        const RegMask dst_m = RegBitMask(dst_r, RtlMachineMode::kDi, df.gp64, df.fp64);
        int64_t sp_off = 0, sp_sz = 0;
        if (mv_s + 2 < ns && steps[mv_s + 1].pn->pattern.IsSet() &&
            steps[mv_s + 1].pn->pattern.SetSrc().IsReg() &&
            steps[mv_s + 1].pn->pattern.SetSrc().mode == RtlMachineMode::kDi &&
            steps[mv_s + 1].pn->pattern.SetSrc().RegNo() == dst_r &&
            ExtractStage0SpMem(steps[mv_s + 1].pn->pattern.SetDest(), &sp_off, &sp_sz) &&
            sp_sz == 8 && (esums[mv_s + 2].uses & dst_m) != 0 &&
            (live_after_step[mv_s + 2] & dst_m) == 0) {
          bool slot_ever_read = false;
          for (size_t qb = 0; qb < nb && !slot_ever_read; ++qb) {
            for (const Stage0StepRef& qst : bp[qb].steps) {
              if (Stage0ExprHasCallOp(qst.pn->pattern) &&
                  sp_off < cfg.sideband.outgoing_args_size) {
                slot_ever_read = true;
                break;
              }
              qst.pn->pattern.Visit([&](const RtlExpr& n) {
                int64_t qoff = 0, qsz = 0;
                if (n.op == RtlOp::kMem &&
                    !(qst.pn->pattern.IsSet() && &qst.pn->pattern.SetDest() == &n) &&
                    ExtractStage0SpMem(n, &qoff, &qsz) && qoff < sp_off + 8 &&
                    sp_off < qoff + qsz) {
                  slot_ever_read = true;
                }
              });
              if (slot_ever_read) {
                break;
              }
            }
          }
          if (!slot_ever_read) {
            StripStage0RegDeadOnStep(&bp[b], &steps[mv_s + 1], dst_r);
            for (RtlRegNote& n : steps[mv_s + 2].pn->reg_notes) {
              if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                  n.datum.RegNo() == dst_r) {
                n.datum = steps[mv_s + 1].pn->pattern.SetDest();
                bp[b].dirty[steps[mv_s + 2].insn_idx] = true;
                break;
              }
            }
          }
        }
      }
      continue;
    }
    if (p0.op != RtlOp::kParallel || p0.children.size() != 3 || !p0.children[0].IsSet() ||
        p0.children[1].op != RtlOp::kClobber || p0.children[2].op != RtlOp::kClobber ||
        !p0.children[0].SetDest().IsReg() || p0.children[0].SetDest().mode != RtlMachineMode::kSi) {
      continue;
    }
    const int md_r = p0.children[0].SetDest().RegNo();
    const RtlOp op0 = p0.children[0].SetSrc().op;
    if (!((md_r == 64 && op0 == RtlOp::kTruncate) || (md_r == 65 && op0 == RtlOp::kMult))) {
      continue;
    }
    size_t mv_s = ns;
    for (size_t k = s + 1; k < ns; ++k) {
      const RtlExpr& pk = steps[k].pn->pattern;
      if (pk.IsSet() && pk.SetDest().IsReg() && pk.SetDest().mode == RtlMachineMode::kSi &&
          pk.SetSrc().IsReg() && pk.SetSrc().mode == RtlMachineMode::kSi &&
          pk.SetSrc().RegNo() == md_r && pk.SetDest().RegNo() >= 2 && pk.SetDest().RegNo() <= 25) {
        mv_s = k;
        break;
      }
      if (esums[k].is_call || IsAnyControlTransferPattern(pk) ||
          ((esums[k].uses | esums[k].defs) & (RegMask{1} << md_r)) != 0) {
        break;
      }
    }
    if (mv_s >= ns || (live_after_step[mv_s] & (RegMask{1} << md_r)) != 0) {
      continue;
    }
    StripStage0RegDeadOnStep(&bp[b], &steps[mv_s], md_r);
    const RtlExpr& gpr_dst = steps[mv_s].pn->pattern.SetDest();
    const int gpr_r = gpr_dst.RegNo();
    bool reaches_block_use = false;
    const bool overwritten_in_epilogue_delay =
        b < df.uses_epilogue_delay_at_exit.size() && df.uses_epilogue_delay_at_exit[b] &&
        (df.epilogue_delay.defs & (RegMask{1} << gpr_r)) != 0;
    if (!gpr_dst.flags.integrated && !overwritten_in_epilogue_delay &&
        block_use_regs.contains(gpr_r)) {
      for (size_t k = mv_s + 1; k < ns; ++k) {
        const RtlExpr& pk = steps[k].pn->pattern;
        if (pk.op == RtlOp::kUse && !pk.children.empty() && pk.children[0].IsReg() &&
            pk.children[0].RegNo() == gpr_r) {
          reaches_block_use = true;
          break;
        }
        if (esums[k].is_call || (esums[k].defs & (RegMask{1} << gpr_r)) != 0) {
          break;
        }
      }
    }
    if (gpr_dst.flags.integrated || reaches_block_use) {
      continue;
    }
    size_t use_s = ns;
    for (size_t k = mv_s + 1; k < ns; ++k) {
      if ((esums[k].uses & (RegMask{1} << gpr_r)) != 0) {
        use_s = k;
        break;
      }
      if (esums[k].is_call || IsAnyControlTransferPattern(steps[k].pn->pattern) ||
          ((esums[k].defs | esums[k].uses) &
           ((RegMask{1} << gpr_r) | (RegMask{1} << 64) | (RegMask{1} << 65))) != 0) {
        break;
      }
    }
    if (use_s < ns && !steps[use_s].in_slot &&
        (live_after_step[use_s] & (RegMask{1} << gpr_r)) == 0 &&
        (live_after_step[use_s] & (RegMask{1} << md_r)) == 0) {
      const RtlExpr& up = steps[use_s].pn->pattern;
      if (up.IsSet() && !esums[use_s].is_call && !IsAnyControlTransferPattern(up)) {
        const RtlOp uop = up.SetSrc().op;
        const bool non_fp =
            up.SetDest().mode != RtlMachineMode::kSf && up.SetDest().mode != RtlMachineMode::kDf &&
            up.SetSrc().mode != RtlMachineMode::kSf && up.SetSrc().mode != RtlMachineMode::kDf;
        const bool elig =
            (uop == RtlOp::kFloat && up.SetDest().IsReg()) ||
            (uop == RtlOp::kReg && non_fp && up.SetDest().op == RtlOp::kMem) ||
            (non_fp && uop != RtlOp::kReg && uop != RtlOp::kMem && uop != RtlOp::kDiv &&
             uop != RtlOp::kUdiv && uop != RtlOp::kMod && uop != RtlOp::kUmod &&
             uop != RtlOp::kMult && uop != RtlOp::kFloat && uop != RtlOp::kUnsignedFloat);
        if (elig) {
          covered_md_steps.insert(s);
          covered_md_steps.insert(mv_s);
          md_note_promos.push_back(Stage0MdNotePromo{
              .use_s = use_s,
              .gpr_r = gpr_r,
              .md_r = md_r,
          });
        }
      }
    }
  }
  if (!md_note_promos.empty()) {
    bool other_md = false;
    if (is_optimized) {
      const RegMask md_pair = (RegMask{1} << 64) | (RegMask{1} << 65);
      for (size_t m = 0; m < ns; ++m) {
        if (!covered_md_steps.contains(m) && !esums[m].is_call &&
            ((esums[m].uses | esums[m].defs) & md_pair) != 0) {
          other_md = true;
          break;
        }
      }
    }
    if (!other_md) {
      for (const Stage0MdNotePromo& pr : md_note_promos) {
        auto& u_notes = steps[pr.use_s].pn->reg_notes;
        const bool is_mem_store = steps[pr.use_s].pn->pattern.IsSet() &&
                                  steps[pr.use_s].pn->pattern.SetDest().op == RtlOp::kMem &&
                                  steps[pr.use_s].pn->pattern.SetSrc().op == RtlOp::kReg;
        for (size_t ni = 0; ni < u_notes.size(); ++ni) {
          if (u_notes[ni].kind == RtlMachineMode::kRegDead && u_notes[ni].datum.IsReg() &&
              u_notes[ni].datum.RegNo() == pr.gpr_r) {
            if (is_mem_store) {
              RtlRegNote moved = std::move(u_notes[ni]);
              moved.datum = MakeGccHardReg(RtlMachineMode::kSi, pr.md_r);
              u_notes.erase(u_notes.begin() + static_cast<ptrdiff_t>(ni));
              u_notes.push_back(std::move(moved));
            } else {
              u_notes[ni].datum = MakeGccHardReg(RtlMachineMode::kSi, pr.md_r);
            }
            bp[b].dirty[steps[pr.use_s].insn_idx] = true;
            break;
          }
        }
      }
    }
  }

  // 2a. Single-use cross-bank `"f"` FPR input reload `(set fpr src_gpr) -> <fp_op(fpr)>`.
  // Track `(fp_s, gpr_r) -> fpr_ord` so when multiple GPR inputs reload into the same
  // FP instruction (`c.lt.s`, `mul.s`, etc.), subsequent insertions preserve exact
  // operand read order (`sched.c:2779-3018`) rather than falling back to `1000000`.
  std::vector<std::vector<std::pair<int, int>>> fp_replaced_gpr_ords(ns);
  for (size_t s = 0; s + 1 < ns; ++s) {
    const RtlExpr& pin = steps[s].pn->pattern;
    if (!pin.IsSet() || !pin.SetDest().IsReg() || pin.SetDest().flags.integrated ||
        (pin.SetDest().mode != RtlMachineMode::kSf && pin.SetDest().mode != RtlMachineMode::kDf) ||
        pin.SetDest().RegNo() < 32 || pin.SetDest().RegNo() >= 64 || !pin.SetSrc().IsReg() ||
        pin.SetSrc().mode != pin.SetDest().mode || pin.SetSrc().RegNo() < 2 ||
        pin.SetSrc().RegNo() >= 32) {
      continue;
    }
    const int fpr_r = pin.SetDest().RegNo();
    const int gpr_r = pin.SetSrc().RegNo();
    const RegMask fpr_m = RegBitMask(fpr_r, pin.SetDest().mode, df.gp64, df.fp64);
    const RegMask gpr_m = RegBitMask(gpr_r, pin.SetSrc().mode, df.gp64, df.fp64);
    size_t fp_s = ns;
    bool gpr_touched = false;
    for (size_t k = s + 1; k < ns; ++k) {
      if ((esums[k].uses & fpr_m) != 0) {
        fp_s = k;
        break;
      }
      if (esums[k].is_call || IsAnyControlTransferPattern(steps[k].pn->pattern) ||
          (esums[k].defs & fpr_m) != 0) {
        break;
      }
      if (((esums[k].uses | esums[k].defs) & gpr_m) != 0) {
        gpr_touched = true;
      }
    }
    if (fp_s >= ns) {
      continue;
    }
    const RtlExpr& pfp = steps[fp_s].pn->pattern;
    bool is_reload_other_out = false;
    int reload_out_dg = -1;
    if ((live_after_step[fp_s] & fpr_m) != 0) {
      if (!steps[fp_s].in_slot && pfp.IsSet() && pfp.SetDest().IsReg() &&
          !pfp.SetDest().flags.integrated && pfp.SetDest().RegNo() == fpr_r &&
          pfp.SetDest().mode == pin.SetDest().mode) {
        const RtlOp rop = pfp.SetSrc().op;
        if (rop != RtlOp::kReg && rop != RtlOp::kMem && rop != RtlOp::kConstDouble &&
            rop != RtlOp::kCall && rop != RtlOp::kSubreg) {
          for (size_t k = fp_s + 1; k < ns; ++k) {
            if ((esums[k].uses & fpr_m) != 0) {
              const RtlExpr& pout = steps[k].pn->pattern;
              const int dg = pout.IsSet() && pout.SetDest().IsReg() ? pout.SetDest().RegNo() : -1;
              const bool valid_dg =
                  (dg >= 2 && dg <= 25) || (dg == 30 && !cfg.sideband.frame_pointer_needed);
              if ((live_after_step[k] & fpr_m) == 0 && pout.IsSet() && pout.SetDest().IsReg() &&
                  !pout.SetDest().flags.integrated && valid_dg &&
                  pout.SetDest().mode == pin.SetDest().mode && pout.SetSrc().IsReg() &&
                  pout.SetSrc().RegNo() == fpr_r) {
                const RegMask dg_m = RegBitMask(dg, pout.SetDest().mode, df.gp64, df.fp64);
                bool dg_touched = false;
                for (size_t m = fp_s + 1; m < k; ++m) {
                  if (((esums[m].uses | esums[m].defs) & dg_m) != 0) {
                    dg_touched = true;
                    break;
                  }
                }
                if (!dg_touched) {
                  is_reload_other_out = true;
                  reload_out_dg = dg;
                }
              }
              break;
            }
            if (esums[k].is_call || IsAnyControlTransferPattern(steps[k].pn->pattern) ||
                (esums[k].defs & fpr_m) != 0) {
              break;
            }
          }
        }
      }
      if (!is_reload_other_out) {
        // `reload1.c:5186-5258` (`choose_reload_regs` inheritance): when a
        // cross-bank GPR->FPR input reload is inherited across multiple pure FP
        // consumers in the same block, `fpr_r` dies at its final pure FP
        // consumer without receiving `REG_DEAD fpr_r` (`reload1.c:5641-6640`).
        auto is_pure_fp_consumer = [&](size_t idx) {
          const RtlExpr& pat = steps[idx].pn->pattern;
          return !esums[idx].is_call && !IsAnyControlTransferPattern(pat) &&
                 pat.op != RtlOp::kUse && pat.op != RtlOp::kClobber &&
                 !(pat.IsSet() && (pat.SetSrc().IsReg() || pat.SetSrc().op == RtlOp::kMem));
        };
        size_t last_fp_s = ns;
        const bool gpr_is_callee_saved = (gpr_r >= 16 && gpr_r <= 23) || gpr_r == 30;
        if (gpr_is_callee_saved && is_pure_fp_consumer(fp_s) && (esums[fp_s].defs & fpr_m) == 0) {
          bool gpr_touched_ext = gpr_touched;
          for (size_t k = fp_s + 1; k < ns; ++k) {
            if (esums[k].is_call || IsAnyControlTransferPattern(steps[k].pn->pattern)) {
              break;
            }
            if ((esums[k].uses & fpr_m) != 0) {
              if (!is_pure_fp_consumer(k) || (esums[k].defs & fpr_m) != 0) {
                break;
              }
              if ((live_after_step[k] & fpr_m) == 0) {
                last_fp_s = k;
                gpr_touched = gpr_touched_ext;
                break;
              }
            }
            if ((esums[k].defs & fpr_m) != 0) {
              break;
            }
            if (((esums[k].uses | esums[k].defs) & gpr_m) != 0) {
              gpr_touched_ext = true;
            }
          }
        }
        if (last_fp_s >= ns) {
          continue;
        }
        fp_s = last_fp_s;
      }
    }
    const RtlExpr& final_pfp = steps[fp_s].pn->pattern;
    if (esums[fp_s].is_call || IsAnyControlTransferPattern(final_pfp) ||
        final_pfp.op == RtlOp::kUse || final_pfp.op == RtlOp::kClobber ||
        (final_pfp.IsSet() &&
         (final_pfp.SetSrc().IsReg() || final_pfp.SetSrc().op == RtlOp::kMem))) {
      continue;
    }
    if (out_proven_fp_spill_regs != nullptr && ((gpr_r >= 16 && gpr_r <= 23) || gpr_r == 30)) {
      *out_proven_fp_spill_regs |= fpr_m;
    }
    if (steps[fp_s].in_slot) {
      const bool moved_gpr_dead = !gpr_touched && (esums[fp_s].defs & gpr_m) == 0 &&
                                  StripStage0RegDeadOnStep(&bp[b], &steps[s], gpr_r);
      if (moved_gpr_dead) {
        if (CountConstZeroRegDeadNotes(steps[fp_s].pn->reg_notes) == 0) {
          steps[fp_s].pn->reg_notes.push_back(RtlRegNote{
              .kind = RtlMachineMode::kRegDead,
              .list_op = RtlOp::kExprList,
              .datum = RtlExpr::MakeConstInt(0),
          });
          bp[b].dirty[steps[fp_s].insn_idx] = true;
        }
      } else {
        StripStage0RegDeadOnStep(&bp[b], &steps[fp_s], fpr_r);
      }
      continue;
    }
    StripStage0RegDeadOnStep(&bp[b], &steps[fp_s], fpr_r);
    // `flow.c:2506-2515` (`mark_used_regs`): when the FP op's output reload
    // writes back to the same GPR (`reload_out_dg == gpr_r`), the pre-reload
    // insn is an in-place RMW on `gpr_r` and carries no `(REG_DEAD gpr_r)`.
    if (!gpr_touched && (esums[fp_s].defs & gpr_m) == 0 &&
        StripStage0RegDeadOnStep(&bp[b], &steps[s], gpr_r) &&
        !(is_reload_other_out && reload_out_dg == gpr_r)) {
      if (!HasRegNote(steps[fp_s].pn->reg_notes, RtlMachineMode::kRegDead, gpr_r)) {
        auto first_read_order = [](const RtlExpr& root, int target_reg) -> int {
          int ord = 0;
          int found = 1000000;
          auto walk = [&](auto& self, const RtlExpr& e, bool in_dst) -> void {
            if (found < 1000000 || e.op == RtlOp::kClobber) {
              return;
            }
            if (e.IsSet()) {
              if (e.SetDest().IsMem()) {
                self(self, e.SetDest(), false);
              }
              self(self, e.SetSrc(), false);
              return;
            }
            if (!in_dst && e.IsReg()) {
              if (e.RegNo() == target_reg) {
                found = ord;
                return;
              }
              ++ord;
              return;
            }
            for (const RtlExpr& ch : e.children) {
              self(self, ch, in_dst);
            }
          };
          walk(walk, root, false);
          return found;
        };
        const int fpr_ord = first_read_order(final_pfp, fpr_r);
        auto effective_read_order = [&](int regno) -> int {
          for (const auto& [mapped_gpr, mapped_ord] : fp_replaced_gpr_ords[fp_s]) {
            if (mapped_gpr == regno) {
              return mapped_ord;
            }
          }
          return first_read_order(final_pfp, regno);
        };
        auto& fp_notes = steps[fp_s].pn->reg_notes;
        auto ins_it = fp_notes.end();
        for (auto it = fp_notes.begin(); it != fp_notes.end(); ++it) {
          if (it->kind == RtlMachineMode::kRegDead && it->datum.IsReg() &&
              effective_read_order(it->datum.RegNo()) > fpr_ord) {
            ins_it = it;
            break;
          }
          if (it->kind == RtlMachineMode::kRegUnused) {
            ins_it = it;
            break;
          }
        }
        fp_notes.insert(ins_it, RtlRegNote{
                                    .kind = RtlMachineMode::kRegDead,
                                    .list_op = RtlOp::kExprList,
                                    .datum = pin.SetSrc(),
                                });
        fp_replaced_gpr_ords[fp_s].emplace_back(gpr_r, fpr_ord);
        bp[b].dirty[steps[fp_s].insn_idx] = true;
      }
    }
  }

  // 2b. Single-use cross-bank `"=f"` FPR output reload `(set fpr <fp_op>) ; (set dst_gpr fpr)`.
  for (size_t s = 0; s + 1 < ns; ++s) {
    if (steps[s].in_slot) {
      continue;
    }
    const RtlExpr& pprod = steps[s].pn->pattern;
    if (!pprod.IsSet() || !pprod.SetDest().IsReg() || pprod.SetDest().flags.integrated ||
        (pprod.SetDest().mode != RtlMachineMode::kSf &&
         pprod.SetDest().mode != RtlMachineMode::kDf) ||
        pprod.SetDest().RegNo() < 32 || pprod.SetDest().RegNo() >= 64) {
      continue;
    }
    const RtlOp sop = pprod.SetSrc().op;
    if (sop == RtlOp::kReg || sop == RtlOp::kMem || sop == RtlOp::kConstDouble ||
        sop == RtlOp::kCall || sop == RtlOp::kSubreg) {
      continue;
    }
    const int fpr_r = pprod.SetDest().RegNo();
    const RegMask fpr_m = RegBitMask(fpr_r, pprod.SetDest().mode, df.gp64, df.fp64);
    size_t out_s = ns;
    bool saw_prior_fp_reader = false;
    for (size_t k = s + 1; k < ns; ++k) {
      if ((esums[k].uses & fpr_m) != 0) {
        // `reload1.c:5186-5249, 5641` (`choose_reload_regs` + `emit_reload_insns`):
        // under `sched2`, an FP instruction inheriting `fpr_r` from the output
        // reload may be scheduled ahead of the cross-bank `mfc1` move while
        // `fpr_r` remains live into `mfc1`. Keep scanning past pure FP readers.
        if (runs_sched2 && (live_after_step[k] & fpr_m) != 0 && (esums[k].defs & fpr_m) == 0 &&
            !esums[k].is_call && !IsAnyControlTransferPattern(steps[k].pn->pattern)) {
          const RtlExpr& pk = steps[k].pn->pattern;
          const bool is_cross_bank_gpr_move = pk.IsSet() && pk.SetDest().IsReg() &&
                                              pk.SetDest().RegNo() < 32 && pk.SetSrc().IsReg() &&
                                              pk.SetSrc().RegNo() == fpr_r;
          if (!is_cross_bank_gpr_move) {
            saw_prior_fp_reader = true;
            continue;
          }
        }
        out_s = k;
        break;
      }
      if (esums[k].is_call || IsAnyControlTransferPattern(steps[k].pn->pattern) ||
          (esums[k].defs & fpr_m) != 0) {
        break;
      }
    }
    if (out_s >= ns || (!runs_sched2 && out_s != s + 1) || (live_after_step[out_s] & fpr_m) != 0) {
      continue;
    }
    const RtlExpr& pout = steps[out_s].pn->pattern;
    const int dst_gpr = pout.IsSet() && pout.SetDest().IsReg() ? pout.SetDest().RegNo() : -1;
    const bool valid_dst_gpr =
        (dst_gpr >= 2 && dst_gpr <= 25) || (dst_gpr == 30 && !cfg.sideband.frame_pointer_needed);
    if (!pout.IsSet() || !pout.SetDest().IsReg() || pout.SetDest().mode != pprod.SetDest().mode ||
        !valid_dst_gpr || !pout.SetSrc().IsReg() || pout.SetSrc().RegNo() != fpr_r) {
      continue;
    }
    const RegMask dst_gpr_m = RegBitMask(dst_gpr, pout.SetDest().mode, df.gp64, df.fp64);
    if (dst_gpr >= 4 && dst_gpr <= 7) {
      // `cse.c:481-486`: non-fixed hard arg regs `$a0..$a3` have `COST == 2 > COST(pseudo) == 1`,
      // so a multi-use FP value consumed by both a non-call FP reader and a call arg lives in an
      // `FP_REGS` pseudo copied to `$a0..$a3` rather than an `"=f"` output reload into `$a0..$a3`.
      bool read_by_non_call = saw_prior_fp_reader;
      bool read_by_call = false;
      for (size_t k = out_s + 1; k < ns; ++k) {
        if (esums[k].is_call) {
          if ((esums[k].uses & dst_gpr_m) != 0) {
            read_by_call = true;
          }
          break;
        }
        if ((esums[k].uses & dst_gpr_m) != 0) {
          read_by_non_call = true;
        }
        if (((esums[k].defs & dst_gpr_m) != 0 && !steps[k].in_slot) ||
            (!steps[k].in_slot && IsAnyControlTransferPattern(steps[k].pn->pattern))) {
          break;
        }
      }
      if (read_by_non_call && read_by_call) {
        continue;
      }
    }
    bool dst_touched_between = false;
    for (size_t k = s + 1; k < out_s; ++k) {
      // `toplev.c:3103` (`thread_prologue_and_epilogue_insns`): block-0 prologue
      // callee-save stores are emitted after `reload` and interleaved by `sched2`,
      // so they were not present during `reload1.c:5641` (`emit_reload_insns`).
      if (b == 0 && prologue_save_steps.contains(k)) {
        continue;
      }
      if (((esums[k].uses | esums[k].defs) & dst_gpr_m) != 0) {
        dst_touched_between = true;
        break;
      }
    }
    if (dst_touched_between) {
      bool s_has_mem = false;
      pprod.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kMem) {
          s_has_mem = true;
        }
      });
      bool can_sink_s = runs_sched2 && !s_has_mem && (esums[s].uses & fpr_m) == 0;
      for (size_t k = s + 1; k < out_s && can_sink_s; ++k) {
        if ((esums[k].defs & esums[s].uses) != 0) {
          can_sink_s = false;
        }
      }
      if (!can_sink_s) {
        continue;
      }
    }
    if (out_proven_fp_spill_regs != nullptr &&
        ((dst_gpr >= 16 && dst_gpr <= 23) || dst_gpr == 30)) {
      *out_proven_fp_spill_regs |= fpr_m;
    }
    StripStage0RegDeadOnStep(&bp[b], &steps[out_s], fpr_r);
    if ((esums[s].uses & fpr_m) != 0) {
      bool is_reload_other_scratch = false;
      for (size_t prev = s; prev > 0; --prev) {
        const size_t pi = prev - 1;
        if ((esums[pi].defs & fpr_m) != 0) {
          const RtlExpr& ppat = steps[pi].pn->pattern;
          int64_t poff = 0, psz = 0;
          if ((esums[pi].uses & fpr_m) == 0 && ppat.IsSet() && ppat.SetDest().IsHardReg() &&
              ppat.SetDest().RegNo() == fpr_r &&
              (ppat.SetSrc().IsHardReg() || (ExtractStage0SpMem(ppat.SetSrc(), &poff, &psz) &&
                                             proven_spill_offsets.contains(poff)))) {
            is_reload_other_scratch = true;
          }
          break;
        }
        if ((esums[pi].uses & fpr_m) != 0 || esums[pi].is_call ||
            IsAnyControlTransferPattern(steps[pi].pn->pattern)) {
          break;
        }
      }
      if (is_reload_other_scratch) {
        StripStage0RegDeadOnStep(&bp[b], &steps[s], fpr_r);
      } else if (!HasRegNote(steps[s].pn->reg_notes, RtlMachineMode::kRegDead, fpr_r)) {
        steps[s].pn->reg_notes.insert(steps[s].pn->reg_notes.begin(),
                                      RtlRegNote{
                                          .kind = RtlMachineMode::kRegDead,
                                          .list_op = RtlOp::kExprList,
                                          .datum = pprod.SetDest(),
                                      });
        bp[b].dirty[steps[s].insn_idx] = true;
      }
    }
  }

  // 2c. `!df.gp64` `"=&d"` `DImode` earlyclobber output reload scratches.
  if (!df.gp64) {
    for (size_t s = 0; s + 1 < ns; ++s) {
      if (steps[s].in_slot) {
        continue;
      }
      const RtlExpr& p0 = steps[s].pn->pattern;
      if (p0.op != RtlOp::kParallel || p0.children.size() < 2 || !p0.children[0].IsSet() ||
          p0.children[1].op != RtlOp::kClobber || !p0.children[0].SetDest().IsReg() ||
          p0.children[0].SetDest().flags.integrated ||
          p0.children[0].SetDest().mode != RtlMachineMode::kDi) {
        continue;
      }
      const RtlOp op0 = p0.children[0].SetSrc().op;
      if (op0 != RtlOp::kPlus && op0 != RtlOp::kMinus && op0 != RtlOp::kAshift &&
          op0 != RtlOp::kAshiftrt && op0 != RtlOp::kLshiftrt) {
        continue;
      }
      const int scr = p0.children[0].SetDest().RegNo();
      if (scr < 2 || scr > 24 || block_use_regs.contains(scr)) {
        continue;
      }
      const RegMask scr_m = RegBitMask(scr, RtlMachineMode::kDi, df.gp64, df.fp64);
      size_t out_s = ns;
      for (size_t k = s + 1; k < ns; ++k) {
        if ((esums[k].uses & scr_m) != 0) {
          out_s = k;
          break;
        }
        if (esums[k].is_call || IsAnyControlTransferPattern(steps[k].pn->pattern) ||
            (esums[k].defs & (scr_m | (esums[s].uses & ~scr_m))) != 0) {
          break;
        }
      }
      if (out_s >= ns || steps[out_s].in_slot || (live_after_step[out_s] & scr_m) != 0) {
        continue;
      }
      const RtlExpr& pout = steps[out_s].pn->pattern;
      if (pout.IsSet() && pout.SetDest().IsReg() && pout.SetDest().mode == RtlMachineMode::kDi &&
          pout.SetSrc().IsReg() && pout.SetSrc().mode == RtlMachineMode::kDi &&
          pout.SetSrc().RegNo() == scr) {
        const int dst = pout.SetDest().RegNo();
        const RegMask dst_m = RegBitMask(dst, RtlMachineMode::kDi, df.gp64, df.fp64);
        if (dst >= 2 && dst <= 24 && dst != scr && (dst_m & (esums[s].uses & ~scr_m)) != 0) {
          StripStage0RegDeadOnStep(&bp[b], &steps[out_s], scr);
        }
      }
    }
  }

  // 2d. `movstrsi` earlyclobber output reload moves.
  for (size_t s = 0; s + 1 < ns; ++s) {
    if (steps[s].in_slot) {
      continue;
    }
    const RtlExpr& p0 = steps[s].pn->pattern;
    if (p0.op != RtlOp::kParallel || p0.children.size() < 5 || !p0.children[0].IsSet() ||
        p0.children[0].SetDest().op != RtlOp::kMem ||
        p0.children[0].SetDest().mode != RtlMachineMode::kBlk ||
        p0.children[0].SetSrc().op != RtlOp::kMem ||
        p0.children[0].SetSrc().mode != RtlMachineMode::kBlk) {
      continue;
    }
    RegMask clob_m = 0;
    for (size_t c = 1; c < p0.children.size(); ++c) {
      if (p0.children[c].op == RtlOp::kClobber && !p0.children[c].children.empty() &&
          p0.children[c].children[0].IsReg()) {
        const int cr = p0.children[c].children[0].RegNo();
        if (cr >= 2 && cr <= 25) {
          clob_m |= (RegMask{1} << cr);
        }
      }
    }
    RegMask addr_m = 0;
    p0.children[0].Visit([&](const RtlExpr& n) {
      if (n.IsReg() && n.RegNo() >= 2 && n.RegNo() <= 25) {
        addr_m |= (RegMask{1} << n.RegNo());
      }
    });
    for (size_t k = s + 1; k < ns && k <= s + 2; ++k) {
      const RtlExpr& pk = steps[k].pn->pattern;
      if (!pk.IsSet() || !pk.SetDest().IsReg() || !pk.SetSrc().IsReg()) {
        break;
      }
      const int dst_r = pk.SetDest().RegNo();
      const int src_r = pk.SetSrc().RegNo();
      if (dst_r < 2 || dst_r > 25 || src_r < 2 || src_r > 25 ||
          (clob_m & (RegMask{1} << src_r)) == 0 || (addr_m & (RegMask{1} << dst_r)) == 0 ||
          (live_after_step[k] & ((RegMask{1} << dst_r) | (RegMask{1} << src_r))) != 0) {
        break;
      }
      StripStage0RegDeadOnStep(&bp[b], &steps[k], src_r);
      auto& notes = steps[k].pn->reg_notes;
      const size_t before_sz = notes.size();
      notes.erase(std::remove_if(notes.begin(), notes.end(),
                                 [dst_r](const RtlRegNote& n) {
                                   return n.kind == RtlMachineMode::kRegUnused && n.datum.IsReg() &&
                                          n.datum.RegNo() == dst_r;
                                 }),
                  notes.end());
      if (notes.size() != before_sz) {
        bp[b].dirty[steps[k].insn_idx] = true;
      }
    }
  }

  // 2e. `cse` cross-class call-argument FPR->GPR copy `(set (reg:SF/DF a0..a3) (reg:SF/DF fpr))`
  // immediately followed by `<fp_arith(..., fpr)>` whose `a0..a3` operand was inherited back to
  // `fpr` by `reload` (`reload_as_needed` leaves `REG_DEAD fpr` on the `a0..a3` copy).
  if (expensive_opts) {
    auto is_fp_arith_op = [](RtlOp op) {
      return op == RtlOp::kPlus || op == RtlOp::kMinus || op == RtlOp::kMult || op == RtlOp::kDiv ||
             op == RtlOp::kNeg || op == RtlOp::kAbs;
    };
    for (size_t s = 0; s + 1 < ns; ++s) {
      if (steps[s].in_slot) {
        continue;
      }
      const RtlExpr& p0 = steps[s].pn->pattern;
      if (!p0.IsSet() || !p0.SetDest().IsHardReg() || p0.SetDest().IsArgPointerReg() ||
          !p0.SetSrc().IsHardReg() || p0.SetSrc().IsArgPointerReg()) {
        continue;
      }
      const int d_hr = p0.SetDest().RegNo();
      const int s_fpr = p0.SetSrc().RegNo();
      const RtlMachineMode mode = p0.SetDest().mode;
      if (d_hr < 4 || d_hr > 7 || s_fpr < 32 || s_fpr > 63 || p0.SetSrc().mode != mode ||
          (mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDf)) {
        continue;
      }
      const RegMask d_m = RegBitMask(d_hr, mode, df.gp64, df.fp64);
      const RegMask s_m = RegBitMask(s_fpr, mode, df.gp64, df.fp64);
      if ((live_after_step[s] & d_m) == 0) {
        continue;
      }
      size_t fp_s = ns;
      for (size_t k = s + 1; k < ns; ++k) {
        const bool is_call_delay_slot = steps[k].in_slot && k + 1 < ns && esums[k + 1].is_call;
        if ((steps[k].in_slot && !is_call_delay_slot) || esums[k].is_call ||
            IsAnyControlTransferPattern(steps[k].pn->pattern) ||
            (esums[k].defs & (d_m | s_m)) != 0) {
          break;
        }
        if ((esums[k].uses & s_m) != 0) {
          fp_s = k;
          break;
        }
        if ((esums[k].uses & d_m) != 0) {
          break;
        }
      }
      if (fp_s >= ns) {
        continue;
      }
      const RtlExpr& p1 = steps[fp_s].pn->pattern;
      if (!p1.IsSet() || !is_fp_arith_op(p1.SetSrc().op) || (live_after_step[fp_s] & s_m) != 0 ||
          (steps[fp_s].in_slot &&
           ((esums[fp_s].uses & ~esums[fp_s].defs & ~live_after_step[fp_s]) & ~s_m) != 0)) {
        continue;
      }
      bool feeds_call_in_b = false;
      for (size_t k = fp_s + 1; k < ns; ++k) {
        if (esums[k].is_call && (esums[k].uses & d_m) != 0) {
          feeds_call_in_b = true;
          break;
        }
        if ((esums[k].defs & d_m) != 0 || IsAnyControlTransferPattern(steps[k].pn->pattern)) {
          break;
        }
      }
      if (feeds_call_in_b && StripStage0RegDeadOnStep(&bp[b], &steps[fp_s], s_fpr) &&
          !HasRegNote(steps[s].pn->reg_notes, RtlMachineMode::kRegDead, s_fpr)) {
        steps[s].pn->reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegDead,
            .list_op = RtlOp::kExprList,
            .datum = p0.SetSrc(),
        });
        bp[b].dirty[steps[s].insn_idx] = true;
      }
    }
  }
}

struct Stage0MemEquivLayout {
  int elim_to_reg = 29;
  bool frame_pointer_needed = false;
  bool float_store = false;
  int64_t starting_frame_offset = 0;
  int64_t min_spill_base = 0;
  int64_t post_reload_tsize = 0;
  absl::flat_hash_set<int64_t> written_incoming_offsets;
  absl::flat_hash_map<int64_t, int> incoming_load_counts;
};

bool ExtractStage0ElimBaseMemOffset(const RtlExpr& mem, int base_reg, int64_t* out_off,
                                    int* out_sz) {
  if (mem.op != RtlOp::kMem || mem.children.size() != 1) {
    return false;
  }
  int sz = 0;
  switch (mem.mode) {
    case RtlMachineMode::kQi:
      sz = 1;
      break;
    case RtlMachineMode::kHi:
      sz = 2;
      break;
    case RtlMachineMode::kSi:
    case RtlMachineMode::kSf:
      sz = 4;
      break;
    case RtlMachineMode::kDi:
    case RtlMachineMode::kDf:
      sz = 8;
      break;
    case RtlMachineMode::kBlk:
      sz = 16;
      break;
    default:
      sz = 4;
      break;
  }
  const RtlExpr& addr = mem.children[0];
  if (addr.op == RtlOp::kReg && !addr.IsArgPointerReg() && addr.RegNo() == base_reg) {
    *out_off = 0;
    *out_sz = sz;
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      !addr.children[0].IsArgPointerReg() && addr.children[0].RegNo() == base_reg &&
      addr.children[1].op == RtlOp::kConstInt) {
    *out_off = addr.children[1].int_val;
    *out_sz = sz;
    return true;
  }
  return false;
}

Stage0MemEquivLayout ComputeStage0MemEquivLayout(
    const RtlControlFlowGraph& cfg, const DataflowResult& df, const ParsedCompilerFlags& flags,
    const std::vector<Stage0BlockParsed>& bp, bool found_b0_sp, size_t b0_sp_step,
    const absl::flat_hash_set<size_t>& prologue_save_steps,
    const absl::flat_hash_set<int64_t>& caller_save_offsets,
    const absl::flat_hash_set<int64_t>& proven_spill_offsets) {
  Stage0MemEquivLayout layout;
  layout.frame_pointer_needed = cfg.sideband.frame_pointer_needed;
  layout.elim_to_reg = layout.frame_pointer_needed ? 30 : 29;
  layout.float_store = flags.flag_float_store;
  int64_t local_top = 0;
  if (found_b0_sp && !bp.empty() && b0_sp_step < bp[0].steps.size()) {
    const RtlExpr& sp_pat = bp[0].steps[b0_sp_step].pn->pattern;
    if (sp_pat.IsSet() && sp_pat.SetSrc().op == RtlOp::kPlus &&
        sp_pat.SetSrc().children.size() == 2 && sp_pat.SetSrc().children[1].IsConstInt() &&
        sp_pat.SetSrc().children[1].int_val < 0) {
      layout.post_reload_tsize = -sp_pat.SetSrc().children[1].int_val;
    } else if (sp_pat.IsSet() && sp_pat.SetSrc().op == RtlOp::kMinus &&
               sp_pat.SetSrc().children.size() == 2 && sp_pat.SetSrc().children[1].IsConstInt() &&
               sp_pat.SetSrc().children[1].int_val > 0) {
      layout.post_reload_tsize = sp_pat.SetSrc().children[1].int_val;
    }
    local_top = layout.post_reload_tsize;
    for (size_t s : prologue_save_steps) {
      if (s < bp[0].steps.size()) {
        const RtlExpr& p = bp[0].steps[s].pn->pattern;
        int64_t off = 0;
        int sz = 0;
        if (p.IsSet() &&
            (ExtractStage0ElimBaseMemOffset(p.SetDest(), 29, &off, &sz) ||
             ExtractStage0ElimBaseMemOffset(p.SetDest(), 30, &off, &sz)) &&
            off >= 0 && off < local_top) {
          local_top = off;
        }
      }
    }
    layout.starting_frame_offset =
        InferStage0EffectiveOutgoingArgsSize(cfg, df, bp, found_b0_sp, local_top);
    local_top = std::max(local_top, layout.starting_frame_offset);
  } else {
    layout.starting_frame_offset = InferStage0EffectiveOutgoingArgsSize(
        cfg, df, bp, found_b0_sp, cfg.sideband.outgoing_args_size + cfg.sideband.frame_size_bytes);
    const int64_t units_per_word = df.gp64 ? 8 : 4;
    const int64_t units_per_fpreg = df.fp64 ? 8 : 4;
    const int64_t var_size =
        (std::max<int64_t>(0, cfg.sideband.frame_size_bytes) + 7) & ~int64_t{7};
    bool calls_alloca = layout.frame_pointer_needed && flags.flag_omit_frame_pointer;
    std::array<bool, kMipsFirstPseudoRegister> ever_live{};
    for (int gpr : cfg.sideband.saved_gpr_regnos) {
      if (gpr >= 0 && gpr < 32) {
        ever_live[gpr] = true;
      }
    }
    for (int fpr : cfg.sideband.saved_fpr_regnos) {
      if (fpr >= 32 && fpr < 64) {
        ever_live[fpr] = true;
      }
    }
    auto inspect_pat = [&](const RtlExpr& pat) {
      if (Stage0ExprHasCallOp(pat)) {
        ever_live[31] = true;
      }
      auto mark_elem = [&](const RtlExpr& elem) {
        if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
          const RtlExpr* dst = &elem.children[0];
          while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
            dst = &dst->children[0];
          }
          if (dst->IsHardReg() && !dst->IsArgPointerReg()) {
            const int hr = dst->RegNo();
            const int nw = HardRegNregs(hr, dst->mode, df.gp64, df.fp64);
            for (int k = 0; k < nw && hr + k < kMipsFirstPseudoRegister; ++k) {
              ever_live[hr + k] = true;
            }
          }
        }
      };
      if (pat.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : pat.children) {
          mark_elem(elem);
        }
      } else {
        mark_elem(pat);
      }
    };
    for (const Stage0BlockParsed& b : bp) {
      for (const Stage0StepRef& st : b.steps) {
        inspect_pat(st.pn->pattern);
      }
    }
    if (cfg.sideband.epilogue_delay_pattern.has_value() &&
        !cfg.sideband.epilogue_delay_pattern->empty()) {
      if (auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern); ep.ok()) {
        inspect_pat(ep->pattern);
      }
    }
    int64_t args_size = (std::max<int64_t>(0, layout.starting_frame_offset) + 7) & ~int64_t{7};
    if (args_size == 0 && calls_alloca) {
      args_size = 4 * units_per_word;
    }
    int64_t gp_reg_size = 0;
    for (int r = 0; r < 32; ++r) {
      const bool callee_saved_gpr = (r >= 16 && r <= 23) || r == 30;
      const bool must_save = (ever_live[r] && callee_saved_gpr) ||
                             (r == 30 && layout.frame_pointer_needed) || (r == 31 && ever_live[31]);
      if (must_save) {
        gp_reg_size += units_per_word;
      }
    }
    const int fp_inc = (df.fp64 || flags.flag_single_float) ? 1 : 2;
    int64_t fp_reg_size = 0;
    for (int r = 32; r < 64; r += fp_inc) {
      if (r >= 52 && ever_live[r]) {
        fp_reg_size += int64_t{fp_inc} * units_per_fpreg;
      }
    }
    const int64_t gp_reg_rounded = (gp_reg_size + 7) & ~int64_t{7};
    const int64_t fp_reg_rounded = (fp_reg_size + 7) & ~int64_t{7};
    local_top = var_size + args_size;
    layout.post_reload_tsize = local_top + gp_reg_rounded + fp_reg_rounded;
  }
  for (int64_t cs_off : caller_save_offsets) {
    if (cs_off >= layout.starting_frame_offset && cs_off < local_top) {
      local_top = (cs_off / 8) * 8;
    }
  }
  for (int64_t sp_off : proven_spill_offsets) {
    if (sp_off >= layout.starting_frame_offset && sp_off < local_top) {
      local_top = std::min(local_top, layout.post_reload_tsize);
    }
  }

  const int num_slots = (local_top > layout.starting_frame_offset)
                            ? static_cast<int>((local_top - layout.starting_frame_offset) / 8)
                            : 0;
  struct SlotInfo {
    bool invalidated = false;
    bool has_access = false;
    bool has_store = false;
    bool has_load = false;
    RtlMachineMode mode = RtlMachineMode::kNone;
    int64_t exact_offset = 0;
  };
  std::vector<SlotInfo> slots(num_slots);
  int min_escaped_slot = num_slots;

  for (size_t b = 0; b < bp.size(); ++b) {
    for (size_t s = 0; s < bp[b].steps.size(); ++s) {
      if (b == 0 && ((found_b0_sp && s == b0_sp_step) || prologue_save_steps.contains(s))) {
        continue;
      }
      const RtlExpr& root = bp[b].steps[s].pn->pattern;
      if (root.IsSet() && root.SetDest().op == RtlOp::kReg && !root.SetDest().IsArgPointerReg() &&
          (root.SetDest().RegNo() == 29 ||
           (layout.frame_pointer_needed && root.SetDest().RegNo() == 30))) {
        continue;
      }
      auto walk = [&](const auto& self, const RtlExpr& expr, bool in_mem_addr,
                      bool is_set_dest) -> void {
        if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
          self(self, expr.children[0], false, true);
          self(self, expr.children[1], false, false);
          return;
        }
        if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
          int64_t off = 0;
          int sz = 0;
          if (ExtractStage0ElimBaseMemOffset(expr, layout.elim_to_reg, &off, &sz)) {
            if (off >= layout.post_reload_tsize) {
              if (is_set_dest || expr.flags.volatil) {
                layout.written_incoming_offsets.insert(off);
              } else {
                ++layout.incoming_load_counts[off];
              }
            } else if (num_slots > 0 && off >= layout.starting_frame_offset && off < local_top) {
              const int64_t rel_lo = off - layout.starting_frame_offset;
              const int64_t rel_hi = rel_lo + sz;
              const int first_s = static_cast<int>(rel_lo / 8);
              const int last_s =
                  static_cast<int>(std::min<int64_t>(num_slots - 1, (rel_hi - 1) / 8));
              const bool is_fp_mode =
                  expr.mode == RtlMachineMode::kSf || expr.mode == RtlMachineMode::kDf;
              if (expr.flags.volatil || expr.flags.in_struct || expr.mode == RtlMachineMode::kBlk ||
                  sz > 8 || first_s != last_s || (layout.float_store && is_fp_mode)) {
                for (int k = std::max(0, first_s); k <= last_s; ++k) {
                  slots[k].invalidated = true;
                }
                return;
              }
              const int64_t slot_base = layout.starting_frame_offset + 8 * int64_t{first_s};
              if (off != slot_base + (8 - sz) || proven_spill_offsets.contains(off)) {
                if (!proven_spill_offsets.contains(off)) {
                  slots[first_s].invalidated = true;
                }
                slots[first_s].has_access = true;
                return;
              }
              SlotInfo& info = slots[first_s];
              if (!info.has_access) {
                info.has_access = true;
                info.mode = expr.mode;
                info.exact_offset = off;
              } else if (info.mode != expr.mode || info.exact_offset != off) {
                info.invalidated = true;
              }
              if (is_set_dest) {
                info.has_store = true;
              } else {
                info.has_load = true;
              }
              return;
            }
          }
          self(self, expr.children[0], true, false);
          return;
        }
        if (!in_mem_addr && expr.op == RtlOp::kPlus && expr.children.size() == 2 &&
            expr.children[0].op == RtlOp::kReg && !expr.children[0].IsArgPointerReg() &&
            expr.children[0].RegNo() == layout.elim_to_reg &&
            expr.children[1].op == RtlOp::kConstInt) {
          const int64_t aoff = expr.children[1].int_val;
          if (aoff >= layout.post_reload_tsize) {
            layout.written_incoming_offsets.insert(aoff);
          } else if (num_slots > 0 && aoff >= layout.starting_frame_offset && aoff < local_top) {
            const int si = static_cast<int>((aoff - layout.starting_frame_offset) / 8);
            if (si >= 0 && si < num_slots) {
              slots[si].invalidated = true;
              min_escaped_slot = std::min(min_escaped_slot, si);
            }
          }
          return;
        }
        for (const RtlExpr& ch : expr.children) {
          self(self, ch, in_mem_addr, false);
        }
      };
      walk(walk, root, false, false);
    }
  }

  for (int si = min_escaped_slot; si < num_slots; ++si) {
    if (slots[si].has_store && !slots[si].has_load) {
      slots[si].invalidated = true;
    }
  }
  int non_inv_begin = num_slots;
  while (non_inv_begin > 0 && !slots[non_inv_begin - 1].invalidated) {
    --non_inv_begin;
  }
  int first_spill_slot = non_inv_begin;
  if (non_inv_begin > 0) {
    while (first_spill_slot < num_slots && !slots[first_spill_slot].has_access) {
      ++first_spill_slot;
    }
  }
  layout.min_spill_base = layout.starting_frame_offset + 8 * int64_t{first_spill_slot};
  return layout;
}

bool Stage0ConstantAddrBaseP(const RtlExpr& e) {
  return e.op == RtlOp::kConstInt || e.op == RtlOp::kConstDouble || e.op == RtlOp::kSymbolRef ||
         e.op == RtlOp::kLabelRef || e.op == RtlOp::kConst ||
         (e.op == RtlOp::kLoSum && e.children.size() >= 2 && e.children[0].IsReg() &&
          e.children[0].RegNo() == 28);
}

const RtlExpr& Stage0UnwrapMemBaseOffset(const RtlExpr& raw_addr, int64_t* offset_accum) {
  const RtlExpr* cur = &raw_addr;
  while (true) {
    if (cur->op == RtlOp::kLoSum && cur->children.size() >= 2 && cur->children[0].IsReg() &&
        cur->children[0].RegNo() == 28) {
      cur = &cur->children[1];
      continue;
    }
    if ((cur->op == RtlOp::kConst || cur->op == RtlOp::kHigh) && !cur->children.empty()) {
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

bool Stage0MemAddrVariesP(const RtlExpr& mem, const Stage0MemEquivLayout& layout) {
  int64_t off = 0;
  int sz = 0;
  if (ExtractStage0ElimBaseMemOffset(mem, layout.elim_to_reg, &off, &sz)) {
    return !layout.frame_pointer_needed && off >= 0 && off < layout.starting_frame_offset;
  }
  if (mem.children.empty()) {
    return true;
  }
  bool varies = false;
  auto scan = [&](const auto& self, const RtlExpr& e) -> void {
    if (varies) return;
    if (e.op == RtlOp::kLoSum && e.children.size() >= 2 && e.children[0].IsReg() &&
        e.children[0].RegNo() == 28) {
      return;
    }
    if (e.op == RtlOp::kReg) {
      if (!e.flags.unchanging && !e.IsArgPointerReg() &&
          !(layout.frame_pointer_needed && e.RegNo() == 30)) {
        varies = true;
      }
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch);
    }
  };
  scan(scan, mem.children[0]);
  return varies;
}

bool Stage0TrueDependence(const RtlExpr& store_mem, const RtlExpr& equiv_mem,
                          const Stage0MemEquivLayout& layout) {
  if (equiv_mem.flags.unchanging && !store_mem.flags.unchanging) {
    return false;
  }
  if (equiv_mem.flags.volatil && store_mem.flags.volatil) {
    return true;
  }
  if (store_mem.children.empty() || equiv_mem.children.empty()) {
    return true;
  }
  auto mem_sz = [](RtlMachineMode m) -> int {
    switch (m) {
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
      default:
        return 0;
    }
  };
  const int xsize = mem_sz(store_mem.mode);
  const int ysize = mem_sz(equiv_mem.mode);
  int64_t xoff = 0;
  int64_t yoff = 0;
  RtlExpr xbase = Stage0UnwrapMemBaseOffset(store_mem.children[0], &xoff);
  RtlExpr ybase = Stage0UnwrapMemBaseOffset(equiv_mem.children[0], &yoff);
  auto unelim_stack_base = [&](RtlExpr* b, int64_t* off) {
    if (!b->IsReg() || b->IsArgPointerReg() || b->RegNo() != layout.elim_to_reg) {
      return;
    }
    if (*off >= layout.post_reload_tsize) {
      *b = RtlExpr::MakeArgPointerReg();
      *off -= layout.post_reload_tsize;
    } else if (!layout.frame_pointer_needed &&
               ((*off >= layout.starting_frame_offset && *off < layout.min_spill_base) ||
                *off < 0)) {
      *b = MakeGccHardReg(RtlMachineMode::kSi, 30);
    }
  };
  unelim_stack_base(&xbase, &xoff);
  unelim_stack_base(&ybase, &yoff);
  const int64_t c = yoff - xoff;
  auto ranges_overlap = [&]() {
    return xsize == 0 || ysize == 0 || (c >= 0 && xsize > c) || (c < 0 && ysize + c > 0);
  };
  auto is_stack_base = [](const RtlExpr& b) {
    return b.IsReg() && (b.IsArgPointerReg() || b.RegNo() == 29 || b.RegNo() == 30);
  };

  bool conflict = true;
  if (xbase == ybase) {
    conflict = ranges_overlap();
  } else if ((is_stack_base(xbase) && Stage0ConstantAddrBaseP(ybase)) ||
             (is_stack_base(ybase) && Stage0ConstantAddrBaseP(xbase))) {
    conflict = false;
  } else if (Stage0ConstantAddrBaseP(xbase) && Stage0ConstantAddrBaseP(ybase)) {
    if (xbase.op == RtlOp::kSymbolRef && ybase.op == RtlOp::kSymbolRef) {
      conflict = (xbase.str_val == ybase.str_val) && ranges_overlap();
    } else if (xbase.op == RtlOp::kConstInt && ybase.op == RtlOp::kConstInt) {
      conflict = false;
    } else {
      conflict = false;
    }
  }
  if (!conflict) {
    return false;
  }
  const bool store_varies = Stage0MemAddrVariesP(store_mem, layout);
  const bool equiv_varies = Stage0MemAddrVariesP(equiv_mem, layout);
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

bool Stage0PatternTriggersReloadElimination(const RtlExpr& pat,
                                            const Stage0MemEquivLayout& layout) {
  if (pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber) {
    return false;
  }
  if (pat.IsSet() && pat.SetDest().IsReg() && !pat.SetDest().IsArgPointerReg() &&
      (pat.SetDest().RegNo() == 29 ||
       (layout.frame_pointer_needed && pat.SetDest().RegNo() == 30))) {
    return false;
  }
  bool triggers = false;
  auto walk = [&](const auto& self, const RtlExpr& expr, bool in_mem) -> void {
    if (triggers) return;
    if (expr.op == RtlOp::kMem && expr.children.size() == 1) {
      self(self, expr.children[0], true);
      return;
    }
    if (expr.op == RtlOp::kPlus && expr.children.size() == 2 && expr.children[0].IsReg() &&
        !expr.children[0].IsArgPointerReg() && expr.children[0].RegNo() == layout.elim_to_reg &&
        expr.children[1].IsConstInt()) {
      const int64_t off = expr.children[1].int_val;
      if (off >= layout.post_reload_tsize) {
        triggers = true;
      } else if (!layout.frame_pointer_needed &&
                 ((off >= layout.starting_frame_offset && off < layout.min_spill_base) ||
                  off < 0)) {
        triggers = true;
      }
      return;
    }
    if (expr.IsReg() && !expr.IsArgPointerReg() && expr.RegNo() == layout.elim_to_reg) {
      if (in_mem && layout.post_reload_tsize == 0) {
        triggers = true;
      } else if (!layout.frame_pointer_needed && layout.starting_frame_offset == 0 &&
                 (!in_mem || layout.min_spill_base > 0)) {
        triggers = true;
      }
      return;
    }
    for (const RtlExpr& ch : expr.children) {
      self(self, ch, in_mem);
    }
  };
  walk(walk, pat, false);
  return triggers;
}

RtlExpr UneliminateStage0MemForNoteIfNeeded(const RtlExpr& mem, const RtlExpr& def_pat,
                                            const Stage0MemEquivLayout& layout) {
  if (Stage0PatternTriggersReloadElimination(def_pat, layout)) {
    return mem;
  }
  int64_t off = 0;
  int sz = 0;
  if (!ExtractStage0ElimBaseMemOffset(mem, layout.elim_to_reg, &off, &sz)) {
    return mem;
  }
  RtlExpr out = mem;
  if (off >= layout.post_reload_tsize) {
    const int64_t arg_off = off - layout.post_reload_tsize;
    out.children[0] = (arg_off == 0) ? RtlExpr::MakeArgPointerReg()
                                     : RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi,
                                                           RtlExpr::MakeArgPointerReg(),
                                                           RtlExpr::MakeConstInt(arg_off));
    return out;
  }
  if (!layout.frame_pointer_needed && (off >= layout.starting_frame_offset || off < 0)) {
    const int64_t fp_off = off;
    out.children[0] = (fp_off == 0) ? MakeGccHardReg(RtlMachineMode::kSi, 30)
                                    : RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi,
                                                          MakeGccHardReg(RtlMachineMode::kSi, 30),
                                                          RtlExpr::MakeConstInt(fp_off));
    return out;
  }
  return out;
}

bool IsStage0CompatibleEquivStore(const RtlExpr* dead_set, int r, const RtlExpr& dst,
                                  const RtlExpr& init_src) {
  if (dead_set == nullptr || dead_set->SetDest().op != RtlOp::kMem || !dead_set->SetSrc().IsReg() ||
      dead_set->SetSrc().IsArgPointerReg() || dead_set->SetSrc().RegNo() != r) {
    return false;
  }
  const RtlMachineMode sm = dead_set->SetDest().mode;
  if (dead_set->SetSrc().mode != sm) {
    return false;
  }
  if (sm == dst.mode) {
    return true;
  }
  return dst.mode == RtlMachineMode::kSi && init_src.IsConstInt() &&
         (sm == RtlMachineMode::kQi || sm == RtlMachineMode::kHi);
}

bool IsStage0IndirectCallTargetReg(const RtlExpr& pat, int r) {
  if ((r >= 4 && r <= 11) || (r >= 44 && r <= 51)) {
    return false;
  }
  bool hit = false;
  pat.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kCall && !n.children.empty() && n.children[0].op == RtlOp::kMem &&
        !n.children[0].children.empty() && n.children[0].children[0].IsReg() &&
        !n.children[0].children[0].IsArgPointerReg() && n.children[0].children[0].RegNo() == r) {
      hit = true;
    }
  });
  return hit;
}

bool IsStage0Sched2HoistedIndependentLoadKillingAddr(const Stage0BlockParsed& blk_bp,
                                                     const std::vector<ElemSummary>& esums,
                                                     const DataflowResult& df, bool runs_sched2,
                                                     size_t start_s, size_t k, size_t dead_s,
                                                     RegMask dying_addr_m) {
  if (!runs_sched2 || k <= start_s || k >= dead_s || esums[k].is_call) {
    return false;
  }
  const RtlExpr* kset = ExtractStage0SingleSet(blk_bp.steps[k].pn->pattern);
  if (kset == nullptr || !kset->SetDest().IsReg() || kset->SetDest().IsArgPointerReg() ||
      kset->SetSrc().op != RtlOp::kMem || kset->SetSrc().flags.volatil) {
    return false;
  }
  const int rk = kset->SetDest().RegNo();
  const RegMask rk_m = RegBitMask(rk, kset->SetDest().mode, df.gp64, df.fp64);
  if ((rk_m & dying_addr_m) != 0) {
    if (blk_bp.steps[k].in_slot ||
        !HasRegNote(blk_bp.steps[k].pn->reg_notes, RtlMachineMode::kRegDead, rk)) {
      return false;
    }
  }
  for (size_t q = start_s; q <= dead_s; ++q) {
    if (q != k && (esums[q].defs & dying_addr_m) != 0) {
      return false;
    }
  }
  // Compute the forward register-dependence closure `in_closure` of `k` within
  // `[k, dead_s]` (`sched.c:1685-1703, 1854-1874`): `sched2` can hoist `k` together with pure
  // register consumers of `k` ahead of `dead_s` as long as `dead_s` itself is
  // not in the closure and no call, control transfer, or memory store enters it.
  std::vector<bool> in_closure(dead_s - k + 1, false);
  in_closure[0] = true;
  auto has_mem_store = [](const RtlExpr& pat) {
    bool found = false;
    pat.Visit([&](const RtlExpr& n) {
      if (n.IsSet() && n.SetDest().op == RtlOp::kMem) {
        found = true;
      }
    });
    return found;
  };
  for (size_t q = k + 1; q <= dead_s; ++q) {
    if (esums[q].is_call) {
      return false;
    }
    bool depends_on_closure = false;
    for (size_t p = k; p < q; ++p) {
      if (!in_closure[p - k]) {
        continue;
      }
      if ((esums[p].defs & (esums[q].uses | esums[q].defs)) != 0 ||
          (esums[p].uses & esums[q].defs) != 0) {
        depends_on_closure = true;
        break;
      }
    }
    if (!depends_on_closure) {
      continue;
    }
    if (q == dead_s || IsAnyControlTransferPattern(blk_bp.steps[q].pn->pattern) ||
        has_mem_store(blk_bp.steps[q].pn->pattern)) {
      return false;
    }
    in_closure[q - k] = true;
  }
  return true;
}

bool IsMultiBlockIncomingParamLoadEquivCandidate(const RtlControlFlowGraph& cfg,
                                                 const DataflowResult& df,
                                                 const Stage0MemEquivLayout& layout,
                                                 const std::vector<ElemSummary>& esums,
                                                 size_t s_def, size_t ns, int r, RegMask reg_m,
                                                 const RtlExpr& dst, const RtlExpr& src) {
  if (src.op != RtlOp::kMem || src.mode != dst.mode || (df.live_out[0] & reg_m) != reg_m) {
    return false;
  }
  int64_t ioff = 0;
  int isz = 0;
  if (!ExtractStage0ElimBaseMemOffset(src, layout.elim_to_reg, &ioff, &isz) ||
      ioff < layout.post_reload_tsize + 16 || layout.written_incoming_offsets.contains(ioff) ||
      !layout.incoming_load_counts.contains(ioff) || layout.incoming_load_counts.at(ioff) != 1) {
    return false;
  }
  for (size_t k = s_def + 1; k < ns; ++k) {
    if ((esums[k].defs & reg_m) != 0 || (esums[k].is_call && (esums[k].uses & reg_m) != 0)) {
      return false;
    }
  }
  bool redefined_elsewhere = false;
  for (size_t ob = 1; ob < cfg.rpo_blocks.size() && !redefined_elsewhere; ++ob) {
    if ((df.live_in[ob] & reg_m) == 0) {
      continue;
    }
    RegMask cur = df.live_in[ob];
    for (const InsnSummary& ois : df.block_summaries[ob]) {
      if (!ois.valid) continue;
      auto chk = [&](const ElemSummary& oe) {
        if ((cur & reg_m) != 0 && (oe.defs & reg_m) != 0 && !oe.is_call) {
          redefined_elsewhere = true;
        }
        if (oe.is_call && (cur & reg_m) != 0 &&
            !((r >= 16 && r <= 23) || r == 30 || (r >= 52 && r <= 63))) {
          redefined_elsewhere = true;
        }
      };
      if (ois.is_sequence) {
        if (ois.is_call_seq) {
          chk(ois.slot);
          chk(ois.head);
        } else {
          chk(ois.head);
          chk(ois.slot);
        }
      } else {
        chk(ois.single);
      }
    }
  }
  return !redefined_elsewhere;
}

// Mirrors `reorg.c:fill_slots_from_thread` (`reorg.c:3280`) and
// `local-alloc.c:992`: a non-annulled fallthrough-stolen conditional-branch
// delay slot is only returned to single-predecessor `b + 1` (making `r`
// block-local for `update_equiv_regs`) when the first active instruction in
// `b + 1` reads `r`.
bool IsStage0FirstFtStepReadsMask(const Stage0BlockParsed& ft_bp,
                                  const std::vector<ElemSummary>& ft_esums, RegMask reg_m) {
  for (size_t k = 0; k < ft_bp.steps.size() && k < ft_esums.size(); ++k) {
    const RtlExpr& kp = ft_bp.steps[k].pn->pattern;
    if (kp.op == RtlOp::kNote || kp.op == RtlOp::kCodeLabel || kp.op == RtlOp::kBarrier ||
        kp.op == RtlOp::kUse || kp.op == RtlOp::kClobber) {
      continue;
    }
    return (ft_esums[k].uses & reg_m) != 0;
  }
  return false;
}

bool HasStage0NonShareableEqualOrEquivNote(const RtlPatternAndNotes& pn) {
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEqual || note.kind == RtlMachineMode::kRegEquiv) {
      if (note.datum.op != RtlOp::kConstInt || note.datum.int_val < -64 ||
          note.datum.int_val > 64) {
        return true;
      }
    }
  }
  return false;
}

bool IsStage0ActiveCrossJumpBodyStep(const Stage0StepRef& st) {
  if (st.is_seq || st.in_slot) {
    return false;
  }
  const RtlExpr& p = st.pn->pattern;
  if (IsAnyControlTransferPattern(p) || p.op == RtlOp::kNote || p.op == RtlOp::kCodeLabel ||
      p.op == RtlOp::kBarrier || p.op == RtlOp::kUse || p.op == RtlOp::kClobber ||
      p.op == RtlOp::kAddrVec || p.op == RtlOp::kAddrDiffVec || p.op == RtlOp::kConstInt ||
      p.op == RtlOp::kAsmInput || p.op == RtlOp::kAsmOperands || p.op == RtlOp::kSequence) {
    return false;
  }
  if (p.IsSet() && p.SetDest().IsHardReg() && p.SetSrc().IsHardReg() &&
      p.SetDest().RegNo() == p.SetSrc().RegNo() && p.SetDest().mode == p.SetSrc().mode) {
    return false;
  }
  bool has_label_ref = false;
  p.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kLabelRef) {
      has_label_ref = true;
    }
  });
  for (const RtlRegNote& note : st.pn->reg_notes) {
    note.datum.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kLabelRef) {
        has_label_ref = true;
      }
    });
  }
  return !has_label_ref;
}

bool IsStage0UnitLatencyIntAluOrStore(const RtlExpr& expr, const ElemSummary& esum) {
  if (esum.is_call || IsAnyControlTransferPattern(expr) || expr.op == RtlOp::kNote ||
      expr.op == RtlOp::kCodeLabel || expr.op == RtlOp::kBarrier || expr.op == RtlOp::kAddrVec ||
      expr.op == RtlOp::kAddrDiffVec || expr.op == RtlOp::kSequence) {
    return false;
  }
  bool touches_mem = false;
  bool has_non_single_cycle_op = false;
  bool has_non_gpr_reg = false;
  expr.Visit([&](const RtlExpr& node) {
    switch (node.op) {
      case RtlOp::kMem:
      case RtlOp::kUnalignedLoad:
      case RtlOp::kUnalignedStore:
        touches_mem = true;
        break;
      case RtlOp::kMult:
      case RtlOp::kDiv:
      case RtlOp::kUdiv:
      case RtlOp::kMod:
      case RtlOp::kUmod:
      case RtlOp::kSqrt:
      case RtlOp::kFloat:
      case RtlOp::kUnsignedFloat:
      case RtlOp::kFix:
      case RtlOp::kUnsignedFix:
      case RtlOp::kFloatExtend:
      case RtlOp::kFloatTruncate:
      case RtlOp::kUnspecVolatile:
      case RtlOp::kAsmInput:
      case RtlOp::kAsmOperands:
        has_non_single_cycle_op = true;
        break;
      case RtlOp::kReg:
        if (node.RegNo() < 0 || node.RegNo() > 31) {
          has_non_gpr_reg = true;
        }
        break;
      default:
        break;
    }
  });
  if (touches_mem) {
    return esum.defs == 0;
  }
  return !has_non_single_cycle_op && !has_non_gpr_reg;
}

struct Stage0MemEquivContext {
  const RtlControlFlowGraph& cfg;
  const DataflowResult& df;
  const ParsedCompilerFlags& flags;
  const Stage0MemEquivLayout& layout;
  size_t b;
  bool found_b0_sp;
  size_t b0_sp_step;
  const absl::flat_hash_set<size_t>& prologue_save_steps;
  const absl::flat_hash_set<int64_t>& caller_save_offsets;
  const absl::flat_hash_set<int64_t>& proven_spill_offsets;
  const std::vector<Stage0BlockParsed>& all_bp;
  const std::vector<std::vector<ElemSummary>>& all_esums;
  const std::vector<std::vector<RegMask>>& all_live_after;
  const std::vector<std::vector<size_t>>& block_preds;
  Stage0BlockParsed* blk_bp;
  RegMask fixed_addr_mask;

  const Stage0BlockParsed& BlockParsed(size_t blk_i) const {
    return (blk_i == b) ? *blk_bp : all_bp[blk_i];
  }

  bool IsStolenSlotIn(size_t blk_i, size_t s) const {
    const Stage0BlockParsed& bpi = BlockParsed(blk_i);
    return bpi.steps[s].is_seq && bpi.steps[s].in_slot &&
           bpi.seq_slot_wraps[bpi.steps[s].insn_idx].flags.in_struct;
  }

  bool IsPrologueStep(size_t s) const {
    return b == 0 && ((found_b0_sp && s == b0_sp_step) || prologue_save_steps.contains(s));
  }

  bool IsSpillOrCsMem(const RtlExpr& mem) const {
    if (mem.op != RtlOp::kMem || mem.flags.volatil || mem.children.size() != 1) {
      return true;
    }
    bool bad_op = false;
    mem.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kUnknown || n.flags.volatil) {
        bad_op = true;
      }
    });
    if (bad_op) {
      return true;
    }
    int64_t off = 0;
    int sz = 0;
    if (ExtractStage0ElimBaseMemOffset(mem, layout.elim_to_reg, &off, &sz)) {
      if (caller_save_offsets.contains(off) || proven_spill_offsets.contains(off)) {
        return true;
      }
      if (off >= layout.min_spill_base && off < layout.post_reload_tsize) {
        return true;
      }
    }
    return false;
  }

  RegMask CollectMemAddrMask(const RtlExpr& mem) const {
    RegMask mask = 0;
    if (mem.children.empty()) {
      return mask;
    }
    mem.children[0].Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kReg && !n.IsArgPointerReg() && n.RegNo() > 0 &&
          n.RegNo() < kMipsFirstPseudoRegister) {
        mask |= RegBitMask(n.RegNo(), n.mode, df.gp64, df.fp64);
      }
    });
    return mask;
  }

  bool IsPostUeqFrameStep(size_t blk_i, size_t k) const {
    if (blk_i == 0 && ((found_b0_sp && k == b0_sp_step) || prologue_save_steps.contains(k))) {
      return true;
    }
    const Stage0BlockParsed& bpi = BlockParsed(blk_i);
    const RtlExpr& kp = bpi.steps[k].pn->pattern;
    if (!kp.IsSet()) {
      return false;
    }
    auto is_frame_spill_mem = [&](const RtlExpr& m) {
      int64_t off = 0;
      int sz = 0;
      if (!ExtractStage0ElimBaseMemOffset(m, layout.elim_to_reg, &off, &sz)) {
        return false;
      }
      return caller_save_offsets.contains(off) || proven_spill_offsets.contains(off) ||
             (off >= layout.min_spill_base && off < layout.post_reload_tsize);
    };
    return (kp.SetDest().op == RtlOp::kMem && is_frame_spill_mem(kp.SetDest())) ||
           (kp.SetSrc().op == RtlOp::kMem && is_frame_spill_mem(kp.SetSrc()));
  }

  bool ValidateEquivMemInBlock(size_t blk_i, size_t start_s, size_t dead_s, RegMask target_reg_m,
                               const RtlExpr& memref,
                               bool allow_single_step_at_start = false) const {
    if ((!allow_single_step_at_start && start_s >= dead_s) || IsSpillOrCsMem(memref)) {
      return false;
    }
    const Stage0BlockParsed& bpi = BlockParsed(blk_i);
    const auto& bes = all_esums[blk_i];
    const auto& bla = all_live_after[blk_i];
    const size_t bns = bpi.steps.size();
    const RegMask raw_addr_mask = CollectMemAddrMask(memref);
    if ((raw_addr_mask & target_reg_m) != 0) {
      return false;
    }
    const RegMask var_addr_mask = raw_addr_mask & ~fixed_addr_mask;
    const bool dead_is_noncall_seq_head =
        dead_s < bns && !bpi.steps[dead_s].in_slot && dead_s > 0 && bpi.steps[dead_s - 1].in_slot &&
        bpi.steps[dead_s - 1].insn_idx == bpi.steps[dead_s].insn_idx && !bes[dead_s].is_call;
    for (size_t k = start_s; k < dead_s && k < bns; ++k) {
      if (IsPostUeqFrameStep(blk_i, k) ||
          (!(allow_single_step_at_start && k == start_s) && IsStolenSlotIn(blk_i, k)) ||
          (dead_is_noncall_seq_head && k + 1 == dead_s)) {
        continue;
      }
      const RtlPatternAndNotes& kpn = *bpi.steps[k].pn;
      if (kpn.pattern.op == RtlOp::kNote || kpn.pattern.op == RtlOp::kCodeLabel ||
          kpn.pattern.op == RtlOp::kBarrier) {
        continue;
      }
      if (bes[k].is_call && !memref.flags.unchanging) {
        const bool is_const_call = bpi.steps[k].is_seq
                                       ? bpi.seq_head_wraps[bpi.steps[k].insn_idx].flags.unchanging
                                       : kpn.pattern.flags.unchanging;
        if (!is_const_call) {
          return false;
        }
      }
      if ((bes[k].defs & raw_addr_mask) != 0) {
        const RegMask def_addr_m = bes[k].defs & raw_addr_mask;
        if ((def_addr_m & fixed_addr_mask) != 0 || dead_s >= bns ||
            !IsStage0Sched2HoistedIndependentLoadKillingAddr(bpi, bes, df, flags.RunsSched2(),
                                                             start_s, k, dead_s, def_addr_m)) {
          return false;
        }
      }
      if (bpi.steps[k].in_slot && var_addr_mask != 0 && (bla[k] & var_addr_mask) != var_addr_mask) {
        const RegMask dying_m = var_addr_mask & ~bla[k];
        if (dead_s >= bns || !IsStage0Sched2HoistedIndependentLoadKillingAddr(
                                 bpi, bes, df, flags.RunsSched2(), start_s, k, dead_s, dying_m)) {
          return false;
        }
      }
      for (const RtlRegNote& n : kpn.reg_notes) {
        if ((n.kind == RtlMachineMode::kRegInc || n.kind == RtlMachineMode::kRegDead) &&
            n.datum.IsReg() && !n.datum.IsArgPointerReg() &&
            n.datum.RegNo() < kMipsFirstPseudoRegister) {
          const RegMask nm = RegBitMask(n.datum.RegNo(), n.datum.mode, df.gp64, df.fp64);
          if ((nm & raw_addr_mask) != 0) {
            if (n.kind == RtlMachineMode::kRegDead && dead_s < bns &&
                IsStage0Sched2HoistedIndependentLoadKillingAddr(
                    bpi, bes, df, flags.RunsSched2(), start_s, k, dead_s, nm & raw_addr_mask)) {
              continue;
            }
            return false;
          }
        }
      }
      bool store_invalidates = false;
      auto check_elem = [&](const RtlExpr& el) {
        if ((el.op == RtlOp::kSet || el.op == RtlOp::kClobber) && !el.children.empty() &&
            el.children[0].op == RtlOp::kMem &&
            Stage0TrueDependence(el.children[0], memref, layout)) {
          store_invalidates = true;
        }
      };
      if (kpn.pattern.IsParallel()) {
        for (const RtlExpr& el : kpn.pattern.children) check_elem(el);
      } else {
        check_elem(kpn.pattern);
      }
      if (store_invalidates) {
        return false;
      }
    }
    return true;
  }

  bool ValidateEquivMem(size_t start_s, size_t dead_s, RegMask target_reg_m,
                        const RtlExpr& memref) const {
    return ValidateEquivMemInBlock(b, start_s, dead_s, target_reg_m, memref);
  }

  bool ExprReadsMemref(const RtlExpr& memref, const RtlExpr& root) const {
    auto ref_in_expr = [&](const auto& self, const RtlExpr& x) -> bool {
      if (x.op == RtlOp::kSet && x.children.size() == 2) {
        if (x.children[0].op == RtlOp::kMem) {
          if (!x.children[0].children.empty() && self(self, x.children[0].children[0])) {
            return true;
          }
        } else if (self(self, x.children[0])) {
          return true;
        }
        return self(self, x.children[1]);
      }
      if (x.op == RtlOp::kMem && Stage0TrueDependence(memref, x, layout)) {
        return true;
      }
      for (const RtlExpr& ch : x.children) {
        if (self(self, ch)) return true;
      }
      return false;
    };
    return ref_in_expr(ref_in_expr, root);
  }

  bool MemrefUsedInBlockRange(size_t blk_i, const RtlExpr& memref, size_t first_k,
                              size_t end_s) const {
    const Stage0BlockParsed& bpi = BlockParsed(blk_i);
    const auto& bes = all_esums[blk_i];
    const size_t bns = bpi.steps.size();
    const bool end_is_noncall_seq_head =
        end_s < bns && !bpi.steps[end_s].in_slot && end_s > 0 && bpi.steps[end_s - 1].in_slot &&
        bpi.steps[end_s - 1].insn_idx == bpi.steps[end_s].insn_idx && !bes[end_s].is_call;
    for (size_t k = first_k; k <= end_s && k < bns; ++k) {
      if (IsPostUeqFrameStep(blk_i, k) || IsStolenSlotIn(blk_i, k) ||
          (end_is_noncall_seq_head && k + 1 == end_s)) {
        continue;
      }
      const RtlExpr& kp = bpi.steps[k].pn->pattern;
      if (kp.op == RtlOp::kNote || kp.op == RtlOp::kCodeLabel || kp.op == RtlOp::kBarrier) {
        continue;
      }
      if (ExprReadsMemref(memref, kp)) {
        return true;
      }
    }
    return false;
  }

  bool MemrefUsedBetween(const RtlExpr& memref, size_t start_s, size_t end_s) const {
    return MemrefUsedInBlockRange(b, memref, start_s + 1, end_s);
  }

  void PrependRegEquiv(size_t target_s, RtlExpr datum) const {
    auto& steps = blk_bp->steps;
    for (const RtlRegNote& ex : steps[target_s].pn->reg_notes) {
      if ((ex.kind == RtlMachineMode::kRegEquiv || ex.kind == RtlMachineMode::kRegEqual) &&
          ex.datum == datum) {
        return;
      }
    }
    steps[target_s].pn->reg_notes.insert(steps[target_s].pn->reg_notes.begin(),
                                         RtlRegNote{
                                             .kind = RtlMachineMode::kRegEquiv,
                                             .list_op = RtlOp::kExprList,
                                             .datum = std::move(datum),
                                         });
    blk_bp->dirty[steps[target_s].insn_idx] = true;
  }

  bool StepHasExplicitRmwDeadIn(size_t blk_i, size_t idx, int hr, RegMask m) const {
    const Stage0BlockParsed& bpi = BlockParsed(blk_i);
    const auto& bes = all_esums[blk_i];
    const auto& bla = all_live_after[blk_i];
    if ((bes[idx].uses & m) == 0) {
      return false;
    }
    if (!bpi.steps[idx].in_slot) {
      return HasRegNote(bpi.steps[idx].pn->reg_notes, RtlMachineMode::kRegDead, hr);
    }
    const RegMask fixed_m = (RegMask{1} << 0) | fixed_addr_mask;
    RegMask non_set_dying = bes[idx].uses & ~bes[idx].defs & ~bla[idx] & ~fixed_m;
    size_t other_notes = 0;
    while (non_set_dying != 0) {
      int dr = 0;
      while (((non_set_dying >> dr) & RegMask{1}) == 0) {
        ++dr;
      }
      RtlMachineMode dm = RtlMachineMode::kSi;
      bpi.steps[idx].pn->pattern.Visit([&](const RtlExpr& n) {
        if (n.IsHardReg() && n.RegNo() == dr) {
          dm = n.mode;
        }
      });
      const RegMask drm = RegBitMask(dr, dm, df.gp64, df.fp64);
      non_set_dying &= ~drm;
      if (!IsLiveGprCrossBankFprReloadStep(bpi.steps, bes, df, idx, dr, dm, drm, bla[idx])) {
        ++other_notes;
      }
    }
    return CountConstZeroRegDeadNotes(bpi.steps[idx].pn->reg_notes) > other_notes;
  }

  bool StepHasExplicitRmwDead(size_t idx, int hr, RegMask m) const {
    return StepHasExplicitRmwDeadIn(b, idx, hr, m);
  }

  void FindUniqueDeathInBlock(size_t blk_i, size_t scan_start, int r, RegMask reg_m,
                              size_t* out_dead, bool* out_killed_or_arg) const {
    const Stage0BlockParsed& bpi = BlockParsed(blk_i);
    const auto& bes = all_esums[blk_i];
    const auto& bla = all_live_after[blk_i];
    const size_t bns = bpi.steps.size();
    *out_dead = bns;
    *out_killed_or_arg = false;
    for (size_t ord = scan_start; ord < bns; ++ord) {
      size_t k = ord;
      if (ord + 1 < bns && bpi.steps[ord].in_slot && !bpi.steps[ord + 1].in_slot &&
          bpi.steps[ord + 1].insn_idx == bpi.steps[ord].insn_idx && !bes[ord + 1].is_call) {
        k = ord + 1;
      } else if (ord > scan_start && !bpi.steps[ord].in_slot && bpi.steps[ord - 1].in_slot &&
                 bpi.steps[ord - 1].insn_idx == bpi.steps[ord].insn_idx && !bes[ord].is_call) {
        k = ord - 1;
      }
      if (IsStolenSlotIn(blk_i, k)) {
        continue;
      }
      if (bes[k].is_call && (bes[k].uses & reg_m) != 0) {
        if (((bla[k] & reg_m) == 0 || (bes[k].defs & reg_m) != 0) &&
            IsStage0IndirectCallTargetReg(bpi.steps[k].pn->pattern, r)) {
          *out_dead = k;
        } else {
          *out_killed_or_arg = true;
        }
        break;
      }
      if ((bes[k].uses & reg_m) != 0 && (bla[k] & reg_m) == 0 && (bes[k].defs & reg_m) == 0) {
        const bool has_dead =
            bpi.steps[k].in_slot
                ? (CountConstZeroRegDeadNotes(bpi.steps[k].pn->reg_notes) > 0)
                : HasRegNote(bpi.steps[k].pn->reg_notes, RtlMachineMode::kRegDead, r);
        if (has_dead) {
          *out_dead = k;
        } else {
          *out_killed_or_arg = true;
        }
        break;
      }
      if ((bes[k].defs & reg_m) != 0) {
        if (StepHasExplicitRmwDeadIn(blk_i, k, r, reg_m)) {
          *out_dead = k;
        } else {
          *out_killed_or_arg = true;
        }
        break;
      }
    }
  }

  // Mirrors `jump.c:1978, 2371-2560` (`find_cross_jump` skipping `CODE_LABEL`s
  // in `i2` at `jump.c:2392` and comparing `PATTERN`s regardless of `REG_EQUIV`
  // at `jump.c:2469-2470`) and Stage 2 `UnCrossJumpChainedPureFallthroughStoreTails`:
  // returns true iff `succ_b` is a pure fallthrough store tail (every step is a
  // cross-jumpable unit-latency int ALU/const op or store, falling through to a
  // labeled non-`(return)`/non-jump block `succ_b + 1`, with all predecessors
  // compatibly defining `r` and unit-latency under `sched2`).
  bool IsChainedPureFallthroughStoreTail(size_t succ_b, size_t j_dead, int r, const RtlExpr& dst,
                                         RegMask reg_m, const RtlExpr& store_mem) const {
    const size_t nb = cfg.rpo_blocks.size();
    if (j_dead != 0 || succ_b == 0 || succ_b + 1 >= nb) {
      return false;
    }
    const auto& succ_blk = cfg.rpo_blocks[succ_b];
    if (!succ_blk.has_label || !succ_blk.addr_vec_targets.empty() ||
        succ_blk.successors.size() != 1 || succ_blk.successors[0] != succ_b + 1) {
      return false;
    }
    for (size_t blk_i = 0; blk_i < nb; ++blk_i) {
      for (size_t t : cfg.rpo_blocks[blk_i].addr_vec_targets) {
        if (t == succ_b) {
          return false;
        }
      }
    }
    const size_t next_b = succ_b + 1;
    const auto& next_steps = all_bp[next_b].steps;
    if (!cfg.rpo_blocks[next_b].has_label || next_steps.empty()) {
      return false;
    }
    const RtlExpr& next_head = next_steps.front().pn->pattern;
    if (next_head.op == RtlOp::kReturn || IsSimpleUncondJumpSummaryPattern(next_head)) {
      return false;
    }
    const auto& succ_steps = all_bp[succ_b].steps;
    const auto& succ_esums = all_esums[succ_b];
    if (succ_steps.empty()) {
      return false;
    }
    for (size_t k = 0; k < succ_steps.size(); ++k) {
      if (!IsStage0ActiveCrossJumpBodyStep(succ_steps[k]) ||
          !IsStage0UnitLatencyIntAluOrStore(succ_steps[k].pn->pattern, succ_esums[k])) {
        return false;
      }
    }
    if (block_preds[succ_b].size() < 2) {
      return false;
    }
    for (size_t pb : block_preds[succ_b]) {
      const Stage0BlockParsed& pbp = BlockParsed(pb);
      const auto& pes = all_esums[pb];
      const size_t pns = pbp.steps.size();
      if (pns == 0) {
        return false;
      }
      if (pb + 1 == succ_b) {
        if (IsAnyControlTransferPattern(pbp.steps.back().pn->pattern)) {
          return false;
        }
      } else if (!IsSimpleUncondJumpSummaryPattern(pbp.steps.back().pn->pattern)) {
        return false;
      }
      if (flags.RunsSched2()) {
        for (size_t pi = 0; pi < pns; ++pi) {
          const RtlExpr& ppat = pbp.steps[pi].pn->pattern;
          if (ppat.op == RtlOp::kNote || ppat.op == RtlOp::kCodeLabel ||
              ppat.op == RtlOp::kBarrier || IsAnyControlTransferPattern(ppat)) {
            continue;
          }
          if (!IsStage0UnitLatencyIntAluOrStore(ppat, pes[pi])) {
            return false;
          }
        }
      }
      if (pb == b) {
        continue;
      }
      size_t p_def = pns;
      for (size_t rev = pns; rev > 0; --rev) {
        const size_t pi = rev - 1;
        if (pes[pi].is_call) {
          break;
        }
        if ((pes[pi].defs & reg_m) != 0) {
          p_def = pi;
          break;
        }
      }
      if (p_def >= pns || pes[p_def].is_call) {
        return false;
      }
      const RtlExpr* p_set = ExtractStage0SingleSet(pbp.steps[p_def].pn->pattern);
      if (p_set == nullptr || !p_set->SetDest().IsHardReg() || p_set->SetDest().RegNo() != r ||
          p_set->SetDest().mode != dst.mode ||
          !IsStage0CompatibleEquivStore(ExtractStage0SingleSet(succ_steps[j_dead].pn->pattern), r,
                                        p_set->SetDest(), p_set->SetSrc()) ||
          !ValidateEquivMemInBlock(pb, p_def, pns, reg_m, store_mem)) {
        return false;
      }
    }
    return true;
  }

  // Returns true iff `succ_b` is a forward cross-jump join block whose
  // predecessors are single-successor arms (plus any `reorg.c:3718`
  // `fill_eager_delay_slots` target-stolen twin branch or `reorg.c:3280`
  // `fill_slots_from_thread` fallthrough-stolen branch at `succ_b - 1`).
  bool IsCrossJumpJoinPredSet(size_t succ_b, RegMask reg_m) const {
    const size_t nb = cfg.rpo_blocks.size();
    if (succ_b == 0 || succ_b >= nb || block_preds[succ_b].size() < 2) {
      return false;
    }
    if (std::find(block_preds[succ_b].begin(), block_preds[succ_b].end(), succ_b - 1) ==
        block_preds[succ_b].end()) {
      return false;
    }
    return std::all_of(block_preds[succ_b].begin(), block_preds[succ_b].end(), [&](size_t pb) {
      if (pb >= succ_b || !cfg.rpo_blocks[pb].addr_vec_targets.empty()) {
        return false;
      }
      if (cfg.rpo_blocks[pb].successors.size() == 1) {
        return true;
      }
      if (cfg.rpo_blocks[pb].successors.size() != 2) {
        return false;
      }
      const auto& pb_steps = all_bp[pb].steps;
      if (pb_steps.size() < 2 || !pb_steps[pb_steps.size() - 2].in_slot ||
          pb_steps[pb_steps.size() - 2].insn_idx != pb_steps[pb_steps.size() - 1].insn_idx ||
          all_esums[pb][pb_steps.size() - 1].is_call) {
        return false;
      }
      const auto& pb_succs = cfg.rpo_blocks[pb].successors;
      const size_t pb_other = (pb_succs[0] == succ_b) ? pb_succs[1] : pb_succs[0];
      if (IsStolenSlotIn(pb, pb_steps.size() - 2) ||
          (pb_other < nb && pb_other != succ_b && (df.live_in[pb_other] & reg_m) == 0)) {
        // `reorg.c:3718` (`fill_eager_delay_slots`): target-stolen slot matches
        // another single-successor predecessor's trailing def of `r`.
        const RtlExpr& stolen_pat = pb_steps[pb_steps.size() - 2].pn->pattern;
        const bool matches_sibling_def = std::any_of(
            block_preds[succ_b].begin(), block_preds[succ_b].end(), [&](size_t other_pb) {
              if (other_pb == pb || cfg.rpo_blocks[other_pb].successors.size() != 1) {
                return false;
              }
              const auto& os = all_bp[other_pb].steps;
              if (os.empty()) {
                return false;
              }
              if (!IsAnyControlTransferPattern(os.back().pn->pattern)) {
                return os.back().pn->pattern == stolen_pat;
              }
              return os.size() >= 2 && os[os.size() - 2].in_slot &&
                     os[os.size() - 2].pn->pattern == stolen_pat;
            });
        if (matches_sibling_def) {
          return true;
        }
      }
      // `reorg.c:3280` (`fill_slots_from_thread`): fallthrough predecessor
      // `succ_b - 1` defining `r` in its non-stolen delay slot while `r` is
      // dead on the non-fallthrough target.
      if (pb + 1 != succ_b || IsStolenSlotIn(pb, pb_steps.size() - 2)) {
        return false;
      }
      return pb_other < nb && pb_other != succ_b && (df.live_in[pb_other] & reg_m) == 0 &&
             (all_esums[pb][pb_steps.size() - 2].defs & reg_m) == reg_m;
    });
  }

  // Mirrors `jump.c:find_cross_jump` (`jump.c:1978, 1993, 2371-2534`) and Stage 2
  // `InvertDoCrossJump` (`epilogue_has_active_head`, `minimum = 1` vs `2`, compatible
  // predecessor definitions, and `sched2` prefix safety): returns true iff step `j_dead`
  // (`(set store_mem (reg r))`) in join block `succ_b` lies in the cross-jumped prefix
  // that `jump2` merged from all predecessors of `succ_b`, so `r` was single-block in
  // each predecessor during `local-alloc.c:update_equiv_regs` (`local-alloc.c:990-1000`).
  bool CanJoinStoreBeCrossJumpedIntoPreds(size_t succ_b, size_t j_dead, int r, const RtlExpr& dst,
                                          RegMask reg_m, const RtlExpr& store_mem,
                                          size_t skip_pred = std::string::npos) const {
    if (IsChainedPureFallthroughStoreTail(succ_b, j_dead, r, dst, reg_m, store_mem)) {
      return true;
    }
    const auto& succ_steps = all_bp[succ_b].steps;
    const auto& succ_esums = all_esums[succ_b];
    const size_t j_ns = succ_steps.size();
    if (j_dead >= j_ns) {
      return false;
    }
    size_t shared_end = 0;
    while (shared_end < j_ns && IsStage0ActiveCrossJumpBodyStep(succ_steps[shared_end]) &&
           !HasStage0NonShareableEqualOrEquivNote(*succ_steps[shared_end].pn)) {
      ++shared_end;
    }
    // `jump.c:1978` passes `minimum = 1` only when `preds.size() == 2` (1 jump +
    // 1 fallthrough); if `succ_b` already has an active non-control epilogue head
    // stopped solely by `HasStage0NonShareableEqualOrEquivNote`, duplicating even 1
    // instruction preserves an active head at `succ_b`. Otherwise `minimum = 2`
    // (`jump.c:1993`) or trailing control instructions require keeping the last
    // shareable instruction in `succ_b`.
    const bool epilogue_has_active_head =
        block_preds[succ_b].size() == 2 && shared_end < j_ns &&
        IsStage0ActiveCrossJumpBodyStep(succ_steps[shared_end]) &&
        HasStage0NonShareableEqualOrEquivNote(*succ_steps[shared_end].pn);
    const size_t effective_shared_len =
        epilogue_has_active_head ? shared_end : (shared_end >= 2 ? shared_end - 1 : 0);
    if (j_dead >= effective_shared_len || effective_shared_len >= j_ns) {
      return false;
    }
    if (j_dead == 0) {
      // Every predecessor of `succ_b` must compatibly define `r` with a valid
      // `store_mem` so `update_equiv_regs` (`local-alloc.c:990-1000`) and
      // `find_cross_jump` (`jump.c:2371-2499`) agree across all merged arms.
      for (size_t pb : block_preds[succ_b]) {
        if (pb == b || pb == skip_pred) {
          continue;
        }
        const Stage0BlockParsed& pbp = BlockParsed(pb);
        const auto& pes = all_esums[pb];
        const size_t pns = pbp.steps.size();
        size_t p_def = pns;
        for (size_t rev = pns; rev > 0; --rev) {
          const size_t pi = rev - 1;
          // In a non-call `SEQUENCE`, the delay slot `pi - 1` executes AFTER
          // the branch head `pi`, so a read of `r` in the branch condition
          // precedes any def of `r` in the delay slot.
          if (!pbp.steps[pi].in_slot && pi > 0 && pbp.steps[pi - 1].in_slot &&
              pbp.steps[pi - 1].insn_idx == pbp.steps[pi].insn_idx && !pes[pi].is_call) {
            continue;
          }
          if ((pes[pi].defs & reg_m) != 0) {
            p_def = pi;
            break;
          }
          if ((pes[pi].uses & reg_m) != 0 || pes[pi].is_call) {
            break;
          }
        }
        if (p_def >= pns) {
          // `reorg.c:3280` (`fill_slots_from_thread`): `pb`'s def of `r` may
          // have been hoisted into its unique predecessor `pb - 1`'s non-stolen
          // fallthrough delay slot when `r` is dead on `pb - 1`'s taken edge.
          if (pb == 0 || block_preds[pb].size() != 1 || block_preds[pb][0] != pb - 1) {
            return false;
          }
          const size_t pp = pb - 1;
          const auto& pp_succs = cfg.rpo_blocks[pp].successors;
          if (pp_succs.size() != 2) {
            return false;
          }
          const size_t pp_target = (pp_succs[0] == pb) ? pp_succs[1] : pp_succs[0];
          const Stage0BlockParsed& pp_bp = BlockParsed(pp);
          const auto& pp_es = all_esums[pp];
          const size_t pp_ns = pp_bp.steps.size();
          if (pp_ns < 2 || !pp_bp.steps[pp_ns - 2].in_slot || IsStolenSlotIn(pp, pp_ns - 2) ||
              pp_bp.steps[pp_ns - 2].insn_idx != pp_bp.steps[pp_ns - 1].insn_idx ||
              pp_es[pp_ns - 1].is_call || (df.live_in[pp_target] & reg_m) != 0 ||
              (pp_es[pp_ns - 2].defs & reg_m) != reg_m) {
            return false;
          }
          const RtlExpr* pp_set = ExtractStage0SingleSet(pp_bp.steps[pp_ns - 2].pn->pattern);
          if (pp_set == nullptr || !pp_set->SetDest().IsHardReg() ||
              pp_set->SetDest().RegNo() != r || pp_set->SetDest().mode != dst.mode ||
              !IsStage0CompatibleEquivStore(ExtractStage0SingleSet(succ_steps[j_dead].pn->pattern),
                                            r, pp_set->SetDest(), pp_set->SetSrc()) ||
              !ValidateEquivMemInBlock(pp, pp_ns - 2, pp_ns - 1, reg_m, store_mem,
                                       /*allow_single_step_at_start=*/true) ||
              !ValidateEquivMemInBlock(pb, 0, pns, reg_m, store_mem,
                                       /*allow_single_step_at_start=*/true)) {
            return false;
          }
          continue;
        }
        if (pes[p_def].is_call) {
          return false;
        }
        const RtlExpr* p_set = ExtractStage0SingleSet(pbp.steps[p_def].pn->pattern);
        if (p_set == nullptr || !p_set->SetDest().IsHardReg() || p_set->SetDest().RegNo() != r ||
            p_set->SetDest().mode != dst.mode ||
            !IsStage0CompatibleEquivStore(ExtractStage0SingleSet(succ_steps[j_dead].pn->pattern), r,
                                          p_set->SetDest(), p_set->SetSrc()) ||
            !ValidateEquivMemInBlock(pb, p_def, pns, reg_m, store_mem,
                                     /*allow_single_step_at_start=*/pbp.steps[p_def].in_slot)) {
          return false;
        }
      }
      if (flags.RunsSched2()) {
        // Under `flag_schedule_insns_after_reload`, a leading memory store in
        // `succ_b` is only split by `InvertDoCrossJump` when followed within
        // `effective_shared_len` by either a `CALL_INSN` or a store-prefix +
        // 1-cycle ALU chain (never a length-1 store alone).
        if (effective_shared_len < 2) {
          return false;
        }
        bool has_call_in_prefix = false;
        for (size_t k = 1; k < effective_shared_len; ++k) {
          if (succ_esums[k].is_call) {
            has_call_in_prefix = true;
            break;
          }
        }
        if (!has_call_in_prefix) {
          size_t alu_k = 0;
          while (alu_k < effective_shared_len) {
            const RtlExpr* ks = ExtractStage0SingleSet(succ_steps[alu_k].pn->pattern);
            if (ks == nullptr || ks->SetDest().op != RtlOp::kMem) {
              break;
            }
            ++alu_k;
          }
          if (alu_k == 0 || alu_k + 1 != effective_shared_len) {
            return false;
          }
          const RtlExpr* alu_set = ExtractStage0SingleSet(succ_steps[alu_k].pn->pattern);
          if (alu_set == nullptr || !alu_set->SetDest().IsHardReg() ||
              alu_set->SetSrc().op == RtlOp::kMem) {
            return false;
          }
        }
      }
    }
    return true;
  }
};

void TrySynthesizeStage0CrossBlockMemRegEquiv(const Stage0MemEquivContext& ctx, size_t s_def,
                                              size_t effective_init_s, RegMask effective_init_m,
                                              int r, RegMask reg_m, const RtlExpr& dst,
                                              const RtlExpr& src, bool def_in_stolen_slot) {
  const auto& steps = ctx.blk_bp->steps;
  const auto& esums = ctx.all_esums[ctx.b];
  const size_t ns = steps.size();
  const size_t nb = ctx.cfg.rpo_blocks.size();
  const RtlPatternAndNotes& def_pn = *steps[s_def].pn;

  // Case 1a: `r` dies in `cfg.sideband.epilogue_delay_pattern` at exit of `b`.
  if (ctx.cfg.rpo_blocks[ctx.b].successors.empty() && ctx.df.can_exit[ctx.b] &&
      ctx.df.uses_epilogue_delay_at_exit[ctx.b] &&
      ctx.cfg.sideband.epilogue_delay_pattern.has_value() &&
      (ctx.df.epilogue_delay.uses & reg_m) != 0 &&
      ((ctx.df.live_at_exit & ~ctx.df.epilogue_delay.defs) & reg_m) == 0) {
    auto ep_pn = ParsePatternAndNotes(*ctx.cfg.sideband.epilogue_delay_pattern);
    if (ep_pn.ok()) {
      const RtlExpr* ep_set = ExtractStage0SingleSet(ep_pn->pattern);
      const bool ep_kills_r =
          (ctx.df.epilogue_delay.defs & reg_m) == 0 ||
          (ep_set != nullptr && ep_set->SetDest().IsHardReg() &&
           ep_set->SetDest().flags.integrated && ep_set->SetDest().RegNo() == r &&
           HasStage0OnlyUnpinnedReadsOfMask(ep_pn->pattern, ctx.df, r, dst.mode, reg_m));
      if (ep_kills_r) {
        if (effective_init_s == s_def && src.op == RtlOp::kMem && src.mode == dst.mode &&
            ctx.ValidateEquivMem(s_def, ns, reg_m, src)) {
          ctx.PrependRegEquiv(s_def, src);
        }
        const RtlExpr& init_src =
            ExtractStage0SingleSet(steps[effective_init_s].pn->pattern)->SetSrc();
        if (IsStage0CompatibleEquivStore(ep_set, r, dst, init_src) &&
            ctx.ValidateEquivMem(effective_init_s, ns, effective_init_m, ep_set->SetDest()) &&
            (effective_init_s + 1 >= ns ||
             !ctx.MemrefUsedBetween(ep_set->SetDest(), effective_init_s, ns - 1)) &&
            !ctx.ExprReadsMemref(ep_set->SetDest(), ep_pn->pattern)) {
          ctx.PrependRegEquiv(
              effective_init_s,
              UneliminateStage0MemForNoteIfNeeded(ep_set->SetDest(),
                                                  steps[effective_init_s].pn->pattern, ctx.layout));
        }
        return;
      }
    }
  }

  // Case 1b: `do_cross_jump` merged store `(set store_mem (reg r))` at the
  // head of single-successor join block `succ_b`, or `fill_eager_delay_slots`
  // split fallthrough `b -> b + 1` where another predecessor target-stole `def_pn`.
  if (ctx.cfg.rpo_blocks[ctx.b].successors.size() == 1) {
    const size_t succ_b = ctx.cfg.rpo_blocks[ctx.b].successors[0];
    bool def_can_sink_to_block_end = true;
    for (size_t k = s_def + 1; k < ns; ++k) {
      if (esums[k].is_call || (esums[k].defs & esums[s_def].uses) != 0) {
        def_can_sink_to_block_end = false;
        break;
      }
    }
    const bool is_eager_split_ft =
        def_can_sink_to_block_end && succ_b == ctx.b + 1 && succ_b < nb &&
        ctx.block_preds[succ_b].size() >= 2 &&
        !IsAnyControlTransferPattern(steps.back().pn->pattern) &&
        std::any_of(ctx.block_preds[succ_b].begin(), ctx.block_preds[succ_b].end(), [&](size_t pb) {
          const size_t pns = ctx.all_bp[pb].steps.size();
          return pb != ctx.b && pns >= 2 && ctx.all_bp[pb].steps[pns - 2].in_slot &&
                 ctx.IsStolenSlotIn(pb, pns - 2) &&
                 ctx.all_bp[pb].steps[pns - 2].pn->pattern == def_pn.pattern;
        });
    const bool is_cross_jump_join = succ_b > ctx.b && ctx.IsCrossJumpJoinPredSet(succ_b, reg_m);
    if (is_eager_split_ft || is_cross_jump_join) {
      size_t j_dead = 0;
      bool j_bad = false;
      ctx.FindUniqueDeathInBlock(succ_b, 0, r, reg_m, &j_dead, &j_bad);
      const size_t j_ns = ctx.all_bp[succ_b].steps.size();
      if (!j_bad && j_dead < j_ns) {
        const RtlExpr* j_set = ExtractStage0SingleSet(ctx.all_bp[succ_b].steps[j_dead].pn->pattern);
        const RtlExpr& init_src =
            ExtractStage0SingleSet(steps[effective_init_s].pn->pattern)->SetSrc();
        if (IsStage0CompatibleEquivStore(j_set, r, dst, init_src) &&
            (is_eager_split_ft || ctx.CanJoinStoreBeCrossJumpedIntoPreds(
                                      succ_b, j_dead, r, dst, reg_m, j_set->SetDest())) &&
            ctx.ValidateEquivMem(effective_init_s, ns, effective_init_m, j_set->SetDest()) &&
            ctx.ValidateEquivMemInBlock(succ_b, 0, j_dead, effective_init_m, j_set->SetDest(),
                                        /*allow_single_step_at_start=*/true) &&
            (effective_init_s + 1 >= ns ||
             !ctx.MemrefUsedBetween(j_set->SetDest(), effective_init_s, ns - 1)) &&
            !ctx.MemrefUsedInBlockRange(succ_b, j_set->SetDest(), 0, j_dead)) {
          ctx.PrependRegEquiv(
              effective_init_s,
              UneliminateStage0MemForNoteIfNeeded(j_set->SetDest(),
                                                  steps[effective_init_s].pn->pattern, ctx.layout));
          return;
        }
      }
    }
  }

  // Case 1c: Multi-block `B0` `assign_parms` incoming caller stack-parameter load.
  if (ctx.b == 0 && effective_init_s == s_def && !ctx.IsSpillOrCsMem(src) &&
      IsMultiBlockIncomingParamLoadEquivCandidate(ctx.cfg, ctx.df, ctx.layout, esums, s_def, ns, r,
                                                  reg_m, dst, src)) {
    ctx.PrependRegEquiv(s_def, src);
  }

  // Case 1d: `relax_delay_slots`-shared branch delay slot (`s_def` in `b`'s
  // non-call delay slot where fallthrough conditional branch(es) `b+1..`
  // also branch to the same `target_b` and `r` dies on a store in `target_b`).
  if (effective_init_s == s_def && steps[s_def].in_slot && !def_in_stolen_slot && s_def + 1 < ns &&
      steps[s_def + 1].insn_idx == steps[s_def].insn_idx && !esums[s_def + 1].is_call &&
      ctx.cfg.rpo_blocks[ctx.b].successors.size() == 2) {
    const auto& b_succs = ctx.cfg.rpo_blocks[ctx.b].successors;
    const bool has_ft =
        ctx.b + 1 < nb && std::find(b_succs.begin(), b_succs.end(), ctx.b + 1) != b_succs.end();
    const size_t target_b = has_ft ? ((b_succs[0] == ctx.b + 1) ? b_succs[1] : b_succs[0]) : nb;
    if (has_ft && target_b < nb && target_b != ctx.b && target_b != ctx.b + 1 &&
        (ctx.df.live_in[target_b] & reg_m) == reg_m) {
      std::vector<size_t> chain_blocks;
      size_t cur_b = ctx.b + 1;
      bool chain_ok = true;
      while (cur_b < nb && cur_b != target_b && (ctx.df.live_in[cur_b] & reg_m) == reg_m) {
        const auto& cs = ctx.cfg.rpo_blocks[cur_b].successors;
        if (cs.size() != 2 || cur_b + 1 >= nb ||
            std::find(cs.begin(), cs.end(), target_b) == cs.end() ||
            std::find(cs.begin(), cs.end(), cur_b + 1) == cs.end()) {
          chain_ok = false;
          break;
        }
        for (size_t q = 0; q < ctx.all_esums[cur_b].size(); ++q) {
          if (((ctx.all_esums[cur_b][q].uses | ctx.all_esums[cur_b][q].defs) & reg_m) != 0 ||
              ctx.all_esums[cur_b][q].is_call) {
            chain_ok = false;
            break;
          }
        }
        if (!chain_ok) {
          break;
        }
        chain_blocks.push_back(cur_b);
        ++cur_b;
      }
      if (chain_ok && !chain_blocks.empty() && cur_b < nb && cur_b != target_b &&
          (ctx.df.live_in[cur_b] & reg_m) == 0) {
        size_t t_dead = 0;
        bool t_bad = false;
        ctx.FindUniqueDeathInBlock(target_b, 0, r, reg_m, &t_dead, &t_bad);
        if (!t_bad && t_dead < ctx.all_bp[target_b].steps.size()) {
          auto chain_validates = [&](const RtlExpr& memref, bool disallow_reads) {
            for (size_t cb : chain_blocks) {
              const size_t cns = ctx.all_bp[cb].steps.size();
              if (!ctx.ValidateEquivMemInBlock(cb, 0, cns, reg_m, memref,
                                               /*allow_single_step_at_start=*/true)) {
                return false;
              }
              if (disallow_reads && cns > 0 && ctx.MemrefUsedInBlockRange(cb, memref, 0, cns - 1)) {
                return false;
              }
            }
            return true;
          };
          if (src.op == RtlOp::kMem && src.mode == dst.mode &&
              ctx.ValidateEquivMemInBlock(ctx.b, s_def, s_def + 1, reg_m, src,
                                          /*allow_single_step_at_start=*/true) &&
              chain_validates(src, /*disallow_reads=*/false) &&
              ctx.ValidateEquivMemInBlock(target_b, 0, t_dead, reg_m, src,
                                          /*allow_single_step_at_start=*/true)) {
            ctx.PrependRegEquiv(s_def, src);
          }
          const RtlExpr* t_set =
              ExtractStage0SingleSet(ctx.all_bp[target_b].steps[t_dead].pn->pattern);
          if (IsStage0CompatibleEquivStore(t_set, r, dst, src) &&
              ctx.ValidateEquivMemInBlock(ctx.b, s_def, s_def + 1, reg_m, t_set->SetDest(),
                                          /*allow_single_step_at_start=*/true) &&
              chain_validates(t_set->SetDest(), /*disallow_reads=*/true) &&
              ctx.ValidateEquivMemInBlock(target_b, 0, t_dead, reg_m, t_set->SetDest(),
                                          /*allow_single_step_at_start=*/true) &&
              !ctx.MemrefUsedInBlockRange(target_b, t_set->SetDest(), 0, t_dead)) {
            ctx.PrependRegEquiv(s_def, UneliminateStage0MemForNoteIfNeeded(
                                           t_set->SetDest(), def_pn.pattern, ctx.layout));
          }
        }
      }
    }
  }
}

void SynthesizeStage0MemRegEquivNotes(const RtlControlFlowGraph& cfg, const DataflowResult& df,
                                      const ParsedCompilerFlags& flags,
                                      const Stage0MemEquivLayout& layout, size_t b,
                                      bool found_b0_sp, size_t b0_sp_step,
                                      const absl::flat_hash_set<size_t>& prologue_save_steps,
                                      const absl::flat_hash_set<int64_t>& caller_save_offsets,
                                      const absl::flat_hash_set<int64_t>& proven_spill_offsets,
                                      const std::vector<Stage0BlockParsed>& all_bp,
                                      const std::vector<std::vector<ElemSummary>>& all_esums,
                                      const std::vector<std::vector<RegMask>>& all_live_after,
                                      const std::vector<std::vector<size_t>>& block_preds,
                                      Stage0BlockParsed* blk_bp) {
  const auto& esums = all_esums[b];
  const auto& live_after_step = all_live_after[b];
  auto& steps = blk_bp->steps;
  const size_t ns = steps.size();
  const size_t nb = cfg.rpo_blocks.size();
  if (ns == 0) {
    return;
  }
  std::vector<RegMask> live_before_step(ns, 0);
  for (size_t s = 0; s < ns; ++s) {
    live_before_step[s] = (live_after_step[s] & ~esums[s].defs) | esums[s].uses;
  }
  const Stage0MemEquivContext ctx{
      .cfg = cfg,
      .df = df,
      .flags = flags,
      .layout = layout,
      .b = b,
      .found_b0_sp = found_b0_sp,
      .b0_sp_step = b0_sp_step,
      .prologue_save_steps = prologue_save_steps,
      .caller_save_offsets = caller_save_offsets,
      .proven_spill_offsets = proven_spill_offsets,
      .all_bp = all_bp,
      .all_esums = all_esums,
      .all_live_after = all_live_after,
      .block_preds = block_preds,
      .blk_bp = blk_bp,
      .fixed_addr_mask = (RegMask{1} << 28) | (RegMask{1} << 29) |
                         (layout.frame_pointer_needed ? (RegMask{1} << 30) : RegMask{0}),
  };

  for (size_t s_def = 0; s_def < ns; ++s_def) {
    if (ctx.IsPrologueStep(s_def) || esums[s_def].is_call) {
      continue;
    }
    const bool def_in_stolen_slot = ctx.IsStolenSlotIn(b, s_def);
    const RtlPatternAndNotes& def_pn = *steps[s_def].pn;
    const RtlExpr* def_set = ExtractStage0SingleSet(def_pn.pattern);
    if (def_set == nullptr || !def_set->SetDest().IsReg() || def_set->SetDest().IsArgPointerReg() ||
        def_set->SetDest().flags.integrated || def_set->SetDest().flags.frame_related) {
      continue;
    }
    const RtlExpr& dst = def_set->SetDest();
    const RtlExpr& src = def_set->SetSrc();
    const int r = dst.RegNo();
    const bool allocatable_r =
        (r >= 2 && r <= 25) || (r == 30 && !layout.frame_pointer_needed) || (r >= 32 && r <= 63);
    if (!allocatable_r) {
      continue;
    }
    const RegMask reg_m = RegBitMask(r, dst.mode, df.gp64, df.fp64);
    if ((live_after_step[s_def] & reg_m) != reg_m) {
      continue;
    }
    if ((live_before_step[s_def] & reg_m) != 0 && (esums[s_def].uses & reg_m) != 0 &&
        !ctx.StepHasExplicitRmwDead(s_def, r, reg_m)) {
      continue;
    }
    bool already_has_equiv = false;
    for (const RtlRegNote& n : def_pn.reg_notes) {
      if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual ||
          n.kind == RtlMachineMode::kRegLibcall || n.kind == RtlMachineMode::kRegRetval) {
        already_has_equiv = true;
        break;
      }
    }
    if (already_has_equiv) {
      continue;
    }

    // Target-stolen `in_struct` branch delay slot (`s_def` stolen from `target_b`).
    if (def_in_stolen_slot) {
      size_t target_b = nb;
      const auto& succs = cfg.rpo_blocks[b].successors;
      if (succs.size() == 1) {
        target_b = succs[0];
      } else if (succs.size() == 2) {
        target_b = (succs[0] == b + 1) ? succs[1] : succs[0];
      }
      if (target_b >= nb || target_b == b || (df.live_in[target_b] & reg_m) != reg_m) {
        continue;
      }
      size_t t_dead = 0;
      bool t_bad = false;
      ctx.FindUniqueDeathInBlock(target_b, 0, r, reg_m, &t_dead, &t_bad);
      if (t_bad) {
        continue;
      }
      const size_t t_ns = all_bp[target_b].steps.size();
      if (t_dead < t_ns) {
        if (src.op == RtlOp::kMem && src.mode == dst.mode &&
            ctx.ValidateEquivMemInBlock(b, s_def, s_def + 1, reg_m, src,
                                        /*allow_single_step_at_start=*/true) &&
            ctx.ValidateEquivMemInBlock(target_b, 0, t_dead, reg_m, src,
                                        /*allow_single_step_at_start=*/true)) {
          ctx.PrependRegEquiv(s_def, src);
        }
        const RtlExpr* t_set = ExtractStage0SingleSet(all_bp[target_b].steps[t_dead].pn->pattern);
        if (IsStage0CompatibleEquivStore(t_set, r, dst, src) &&
            ctx.ValidateEquivMemInBlock(b, s_def, s_def + 1, reg_m, t_set->SetDest(),
                                        /*allow_single_step_at_start=*/true) &&
            ctx.ValidateEquivMemInBlock(target_b, 0, t_dead, reg_m, t_set->SetDest(),
                                        /*allow_single_step_at_start=*/true) &&
            !ctx.MemrefUsedInBlockRange(target_b, t_set->SetDest(), 0, t_dead)) {
          ctx.PrependRegEquiv(
              s_def, UneliminateStage0MemForNoteIfNeeded(t_set->SetDest(), def_pn.pattern, layout));
        }
      }
      continue;
    }

    // Fallthrough-stolen branch delay slot (`s_def` stolen from single-pred `b + 1`
    // by `reorg.c:fill_slots_from_thread` when `r` is dead on `target_b`).
    if (!def_in_stolen_slot && steps[s_def].in_slot && s_def + 1 < ns &&
        steps[s_def + 1].insn_idx == steps[s_def].insn_idx && !esums[s_def + 1].is_call &&
        cfg.rpo_blocks[b].successors.size() == 2) {
      const auto& succs = cfg.rpo_blocks[b].successors;
      const bool has_ft = b + 1 < nb && std::find(succs.begin(), succs.end(), b + 1) != succs.end();
      const size_t target_b = has_ft ? ((succs[0] == b + 1) ? succs[1] : succs[0]) : nb;
      if (has_ft && target_b < nb && target_b != b + 1 && (df.live_in[target_b] & reg_m) == 0 &&
          (df.live_in[b + 1] & reg_m) == reg_m) {
        const size_t ft_b = b + 1;
        if (block_preds[ft_b].size() == 1 && block_preds[ft_b][0] == b) {
          if (IsStage0FirstFtStepReadsMask(all_bp[ft_b], all_esums[ft_b], reg_m)) {
            size_t ft_dead = 0;
            bool ft_bad = false;
            ctx.FindUniqueDeathInBlock(ft_b, 0, r, reg_m, &ft_dead, &ft_bad);
            if (!ft_bad && ft_dead < all_bp[ft_b].steps.size()) {
              if (src.op == RtlOp::kMem && src.mode == dst.mode &&
                  ctx.ValidateEquivMemInBlock(b, s_def, s_def + 1, reg_m, src,
                                              /*allow_single_step_at_start=*/true) &&
                  ctx.ValidateEquivMemInBlock(ft_b, 0, ft_dead, reg_m, src,
                                              /*allow_single_step_at_start=*/true)) {
                ctx.PrependRegEquiv(s_def, src);
              }
              const RtlExpr* ft_set =
                  ExtractStage0SingleSet(all_bp[ft_b].steps[ft_dead].pn->pattern);
              if (IsStage0CompatibleEquivStore(ft_set, r, dst, src) &&
                  ctx.ValidateEquivMemInBlock(b, s_def, s_def + 1, reg_m, ft_set->SetDest(),
                                              /*allow_single_step_at_start=*/true) &&
                  ctx.ValidateEquivMemInBlock(ft_b, 0, ft_dead, reg_m, ft_set->SetDest(),
                                              /*allow_single_step_at_start=*/true) &&
                  !ctx.MemrefUsedInBlockRange(ft_b, ft_set->SetDest(), 0, ft_dead)) {
                ctx.PrependRegEquiv(s_def, UneliminateStage0MemForNoteIfNeeded(
                                               ft_set->SetDest(), def_pn.pattern, layout));
              }
            }
            continue;
          }
          // `reorg.c:3280` + `jump.c:1978, 2371-2534`: `s_def` was hoisted from
          // `ft_b` into `b`'s fallthrough delay slot while `ft_b` itself falls
          // into cross-jumped join block `succ_b` whose head store kills `r`.
          if (cfg.rpo_blocks[ft_b].successors.size() == 1 &&
              cfg.rpo_blocks[ft_b].addr_vec_targets.empty()) {
            const size_t succ_b = cfg.rpo_blocks[ft_b].successors[0];
            const size_t ft_ns = all_bp[ft_b].steps.size();
            size_t ft_dead = 0;
            bool ft_bad = false;
            ctx.FindUniqueDeathInBlock(ft_b, 0, r, reg_m, &ft_dead, &ft_bad);
            if (!ft_bad && ft_dead == ft_ns && succ_b == ft_b + 1 &&
                ctx.IsCrossJumpJoinPredSet(succ_b, reg_m)) {
              size_t j_dead = 0;
              bool j_bad = false;
              ctx.FindUniqueDeathInBlock(succ_b, 0, r, reg_m, &j_dead, &j_bad);
              if (!j_bad && j_dead == 0 && j_dead < all_bp[succ_b].steps.size()) {
                const RtlExpr* j_set =
                    ExtractStage0SingleSet(all_bp[succ_b].steps[j_dead].pn->pattern);
                if (IsStage0CompatibleEquivStore(j_set, r, dst, src) &&
                    ctx.CanJoinStoreBeCrossJumpedIntoPreds(succ_b, j_dead, r, dst, reg_m,
                                                           j_set->SetDest(), /*skip_pred=*/ft_b) &&
                    ctx.ValidateEquivMemInBlock(b, s_def, s_def + 1, reg_m, j_set->SetDest(),
                                                /*allow_single_step_at_start=*/true) &&
                    ctx.ValidateEquivMemInBlock(ft_b, 0, ft_ns, reg_m, j_set->SetDest(),
                                                /*allow_single_step_at_start=*/true) &&
                    (ft_ns == 0 ||
                     !ctx.MemrefUsedInBlockRange(ft_b, j_set->SetDest(), 0, ft_ns - 1)) &&
                    !ctx.MemrefUsedInBlockRange(succ_b, j_set->SetDest(), 0, j_dead)) {
                  ctx.PrependRegEquiv(s_def, UneliminateStage0MemForNoteIfNeeded(
                                                 j_set->SetDest(), def_pn.pattern, layout));
                  continue;
                }
              }
            }
          }
        } else if (ctx.IsCrossJumpJoinPredSet(ft_b, reg_m)) {
          // `reorg.c:3280` + `jump.c:1978, 2371-2534`: `s_def` was hoisted from
          // an otherwise-empty fallthrough arm directly into `b`'s delay slot,
          // falling directly into cross-jumped join block `ft_b` whose head store kills `r`.
          size_t j_dead = 0;
          bool j_bad = false;
          ctx.FindUniqueDeathInBlock(ft_b, 0, r, reg_m, &j_dead, &j_bad);
          if (!j_bad && j_dead == 0 && j_dead < all_bp[ft_b].steps.size()) {
            const RtlExpr* j_set = ExtractStage0SingleSet(all_bp[ft_b].steps[j_dead].pn->pattern);
            if (IsStage0CompatibleEquivStore(j_set, r, dst, src) &&
                ctx.CanJoinStoreBeCrossJumpedIntoPreds(ft_b, j_dead, r, dst, reg_m,
                                                       j_set->SetDest()) &&
                ctx.ValidateEquivMemInBlock(b, s_def, s_def + 1, reg_m, j_set->SetDest(),
                                            /*allow_single_step_at_start=*/true) &&
                !ctx.MemrefUsedInBlockRange(ft_b, j_set->SetDest(), 0, j_dead)) {
              ctx.PrependRegEquiv(s_def, UneliminateStage0MemForNoteIfNeeded(
                                             j_set->SetDest(), def_pn.pattern, layout));
              continue;
            }
          }
        }
      }
    }

    size_t effective_init_s = s_def;
    RegMask effective_init_m = reg_m;
    if (src.IsReg() && !src.IsArgPointerReg() && src.mode == dst.mode &&
        src.RegNo() < kMipsFirstPseudoRegister && !steps[s_def].in_slot) {
      const int r_src = src.RegNo();
      const RegMask src_m = RegBitMask(r_src, src.mode, df.gp64, df.fp64);
      if ((live_after_step[s_def] & src_m) == 0 &&
          !HasRegNote(def_pn.reg_notes, RtlMachineMode::kRegDead, r_src)) {
        size_t prod_s = ns;
        for (size_t prev = s_def; prev > 0; --prev) {
          const size_t pi = prev - 1;
          if ((esums[pi].defs & src_m) != 0) {
            prod_s = pi;
            break;
          }
          if ((esums[pi].uses & src_m) != 0 || esums[pi].is_call ||
              IsAnyControlTransferPattern(steps[pi].pn->pattern)) {
            break;
          }
        }
        if (prod_s < ns && !esums[prod_s].is_call) {
          const RtlExpr* pset = ExtractStage0SingleSet(steps[prod_s].pn->pattern);
          if (pset != nullptr && pset->SetDest().IsReg() && pset->SetDest().RegNo() == r_src &&
              pset->SetDest().mode == dst.mode &&
              !((live_before_step[prod_s] & src_m) != 0 && (esums[prod_s].uses & src_m) != 0 &&
                !ctx.StepHasExplicitRmwDead(prod_s, r_src, src_m))) {
            effective_init_s = prod_s;
            effective_init_m = src_m | reg_m;
          } else {
            continue;
          }
        } else {
          continue;
        }
      }
    }

    size_t s_dead = ns;
    bool killed_or_call_arg = false;
    const size_t scan_start =
        (steps[s_def].in_slot && s_def + 1 < ns &&
         steps[s_def + 1].insn_idx == steps[s_def].insn_idx && !esums[s_def + 1].is_call)
            ? (s_def + 2)
            : (s_def + 1);
    ctx.FindUniqueDeathInBlock(b, scan_start, r, reg_m, &s_dead, &killed_or_call_arg);
    if (killed_or_call_arg) {
      continue;
    }

    if (s_dead == ns) {
      TrySynthesizeStage0CrossBlockMemRegEquiv(ctx, s_def, effective_init_s, effective_init_m, r,
                                               reg_m, dst, src, def_in_stolen_slot);
      continue;
    }

    size_t validate_end_s = s_dead;
    const RtlExpr& dead_pat = steps[s_dead].pn->pattern;
    if (dead_pat.IsSet() && dead_pat.SetDest().IsReg() && !dead_pat.SetDest().IsArgPointerReg() &&
        dead_pat.SetSrc().IsReg() && !dead_pat.SetSrc().IsArgPointerReg() &&
        dead_pat.SetSrc().RegNo() == r && dead_pat.SetDest().mode == dst.mode &&
        dead_pat.SetDest().RegNo() < 64 && ((r < 32) != (dead_pat.SetDest().RegNo() < 32))) {
      const int r2 = dead_pat.SetDest().RegNo();
      const RegMask r2_m = RegBitMask(r2, dst.mode, df.gp64, df.fp64);
      size_t r2_dead = ns;
      for (size_t k = s_dead + 1; k < ns; ++k) {
        if (esums[k].is_call && (esums[k].uses & r2_m) != 0) break;
        if ((esums[k].uses & r2_m) != 0 && (live_after_step[k] & r2_m) == 0 &&
            (esums[k].defs & r2_m) == 0) {
          r2_dead = k;
          break;
        }
        if ((esums[k].defs & r2_m) != 0) break;
      }
      if (r2_dead < ns) {
        validate_end_s = r2_dead;
      }
    }

    // Single-block `B0` `assign_parms` incoming caller stack-parameter load
    // (`function.c:3640` attaches `REG_EQUIV` at expansion time).
    bool added_incoming_param_equiv = false;
    if (b == 0 && effective_init_s == s_def && src.op == RtlOp::kMem && src.mode == dst.mode &&
        !ctx.IsSpillOrCsMem(src)) {
      int64_t ioff = 0;
      int isz = 0;
      if (ExtractStage0ElimBaseMemOffset(src, layout.elim_to_reg, &ioff, &isz) &&
          ioff >= layout.post_reload_tsize + 16 &&
          !layout.written_incoming_offsets.contains(ioff) &&
          layout.incoming_load_counts.contains(ioff) && layout.incoming_load_counts.at(ioff) == 1) {
        ctx.PrependRegEquiv(s_def, src);
        added_incoming_param_equiv = true;
      }
    }

    // Rule B (`local-alloc.c:1051-1055`).
    if (!added_incoming_param_equiv && effective_init_s == s_def && src.op == RtlOp::kMem &&
        src.mode == dst.mode && ctx.ValidateEquivMem(s_def, validate_end_s, reg_m, src)) {
      ctx.PrependRegEquiv(s_def, src);
    }

    // Rule A (`local-alloc.c:990-1000`).
    const RtlExpr* dead_set = ExtractStage0SingleSet(dead_pat);
    const RtlExpr& init_src = ExtractStage0SingleSet(steps[effective_init_s].pn->pattern)->SetSrc();
    if (IsStage0CompatibleEquivStore(dead_set, r, dst, init_src) &&
        ctx.ValidateEquivMem(effective_init_s, s_dead, effective_init_m, dead_set->SetDest()) &&
        !ctx.MemrefUsedBetween(dead_set->SetDest(), effective_init_s, s_dead)) {
      RtlExpr note_mem = UneliminateStage0MemForNoteIfNeeded(
          dead_set->SetDest(), steps[effective_init_s].pn->pattern, layout);
      ctx.PrependRegEquiv(effective_init_s, std::move(note_mem));
    } else if (steps[s_dead].in_slot && !ctx.IsStolenSlotIn(b, s_dead) && s_dead + 1 < ns &&
               steps[s_dead + 1].insn_idx == steps[s_dead].insn_idx && !esums[s_dead + 1].is_call &&
               (esums[s_dead].defs & reg_m) == 0) {
      // `reorg.c:fill_simple_delay_slots` skipped a multi-word symbolic store
      // `s_store` (`(set (mem (symbol_ref ...)) (reg r))`) and pulled the
      // preceding single-word `s_dead` past `s_store` into the delay slot.
      for (size_t s_store = effective_init_s + 1; s_store < s_dead; ++s_store) {
        if (steps[s_store].in_slot) {
          continue;
        }
        const RtlExpr* st_set = ExtractStage0SingleSet(steps[s_store].pn->pattern);
        if (!IsStage0CompatibleEquivStore(st_set, r, dst, init_src) ||
            st_set->SetDest().children.empty()) {
          continue;
        }
        int64_t sym_off = 0;
        const RtlExpr& st_base = Stage0UnwrapMemBaseOffset(st_set->SetDest().children[0], &sym_off);
        const bool is_gp_lo_sum = st_set->SetDest().children[0].op == RtlOp::kLoSum;
        if (is_gp_lo_sum || !Stage0ConstantAddrBaseP(st_base)) {
          continue;
        }
        if (ctx.ValidateEquivMem(effective_init_s, s_store, effective_init_m, st_set->SetDest()) &&
            ctx.ValidateEquivMem(s_store + 1, s_dead, effective_init_m, st_set->SetDest()) &&
            !ctx.MemrefUsedBetween(st_set->SetDest(), effective_init_s, s_store) &&
            !ctx.MemrefUsedBetween(st_set->SetDest(), s_store, s_dead)) {
          RtlExpr note_mem = UneliminateStage0MemForNoteIfNeeded(
              st_set->SetDest(), steps[effective_init_s].pn->pattern, layout);
          ctx.PrependRegEquiv(effective_init_s, std::move(note_mem));
          break;
        }
      }
    }
  }
}

void ParseStage0Blocks(const RtlControlFlowGraph& cfg, std::vector<Stage0BlockParsed>* out_bp) {
  const size_t nb = cfg.rpo_blocks.size();
  out_bp->assign(nb, Stage0BlockParsed{});
  for (size_t b = 0; b < nb; ++b) {
    const auto& blk = cfg.rpo_blocks[b];
    const size_t ni = blk.instructions.size();
    (*out_bp)[b].tops.resize(ni);
    (*out_bp)[b].seq_head_wraps.resize(ni);
    (*out_bp)[b].seq_slot_wraps.resize(ni);
    (*out_bp)[b].seq_heads.resize(ni);
    (*out_bp)[b].seq_slots.resize(ni);
    (*out_bp)[b].is_seq.assign(ni, false);
    (*out_bp)[b].dirty.assign(ni, false);
    for (size_t i = 0; i < ni; ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      (*out_bp)[b].tops[i] = std::move(*parsed);
      if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
          !(*out_bp)[b].tops[i].has_explicit_call_fusage) {
        (*out_bp)[b].tops[i].has_explicit_call_fusage = true;
        (*out_bp)[b].tops[i].call_fusage = blk.rtl_insns[i].call_fusage;
      }
      if ((*out_bp)[b].tops[i].pattern.op == RtlOp::kSequence &&
          (*out_bp)[b].tops[i].pattern.children.size() == 2) {
        auto h = RtlPatternAndNotes::FromSequenceSubInsn((*out_bp)[b].tops[i].pattern.children[0]);
        auto s = RtlPatternAndNotes::FromSequenceSubInsn((*out_bp)[b].tops[i].pattern.children[1]);
        if (h.ok() && s.ok()) {
          (*out_bp)[b].is_seq[i] = true;
          (*out_bp)[b].seq_head_wraps[i] = (*out_bp)[b].tops[i].pattern.children[0];
          (*out_bp)[b].seq_slot_wraps[i] = (*out_bp)[b].tops[i].pattern.children[1];
          (*out_bp)[b].seq_heads[i] = std::move(*h);
          (*out_bp)[b].seq_slots[i] = std::move(*s);
          if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
              (*out_bp)[b].seq_head_wraps[i].op == RtlOp::kCallInsn &&
              !(*out_bp)[b].seq_heads[i].has_explicit_call_fusage) {
            (*out_bp)[b].seq_heads[i].has_explicit_call_fusage = true;
            (*out_bp)[b].seq_heads[i].call_fusage = blk.rtl_insns[i].call_fusage;
          }
          (*out_bp)[b].steps.push_back(Stage0StepRef{
              .insn_idx = i, .is_seq = true, .in_slot = true, .pn = &(*out_bp)[b].seq_slots[i]});
          (*out_bp)[b].steps.push_back(Stage0StepRef{
              .insn_idx = i, .is_seq = true, .in_slot = false, .pn = &(*out_bp)[b].seq_heads[i]});
          continue;
        }
      }
      (*out_bp)[b].steps.push_back(Stage0StepRef{
          .insn_idx = i, .is_seq = false, .in_slot = false, .pn = &(*out_bp)[b].tops[i]});
    }
  }
}

void ComputeStage0BlockLiveAfterSteps(const RtlControlFlowGraph& cfg, const DataflowResult& df,
                                      size_t b, const Stage0BlockParsed& blk_bp,
                                      std::vector<ElemSummary>* out_esums,
                                      std::vector<RegMask>* out_live_after) {
  const size_t nb = cfg.rpo_blocks.size();
  const auto& steps = blk_bp.steps;
  const size_t ns = steps.size();
  out_esums->assign(ns, ElemSummary{});
  out_live_after->assign(ns, 0);
  RegMask cur_l = df.live_out[b];
  for (size_t rev = ns; rev > 0; --rev) {
    const size_t s = rev - 1;
    (*out_esums)[s] = SummarizeStage0Step(steps[s], df);
    if (!steps[s].in_slot && rev >= 2 && steps[s - 1].in_slot &&
        steps[s - 1].insn_idx == steps[s].insn_idx && !(*out_esums)[s].is_call) {
      const size_t slot_s = s - 1;
      (*out_esums)[slot_s] = SummarizeStage0Step(steps[slot_s], df);
      RegMask slot_after = cur_l;
      const bool slot_in_struct = blk_bp.seq_slot_wraps[steps[slot_s].insn_idx].flags.in_struct;
      const auto& succs = cfg.rpo_blocks[b].successors;
      const bool has_ft = b + 1 < nb && std::find(succs.begin(), succs.end(), b + 1) != succs.end();
      const bool taken_also_ft =
          has_ft && !succs.empty() &&
          std::all_of(succs.begin(), succs.end(), [b](size_t sb) { return sb == b + 1; });
      if (slot_in_struct && has_ft) {
        RegMask taken_l =
            df.can_exit[b]
                ? (df.uses_epilogue_delay_at_exit[b]
                       ? ((df.live_at_exit & ~df.epilogue_delay.defs) | df.epilogue_delay.uses)
                       : df.live_at_exit)
                : RegMask{0};
        for (size_t sb : succs) {
          if ((sb != b + 1 || taken_also_ft) && sb < nb) {
            taken_l |= df.live_in[sb];
          }
        }
        slot_after = taken_l;
      }
      RegMask before_slot = (cur_l & ~(*out_esums)[slot_s].defs) | (*out_esums)[slot_s].uses;
      if (slot_in_struct && has_ft && blk_bp.seq_head_wraps[steps[s].insn_idx].flags.unchanging) {
        before_slot = ((slot_after & ~(*out_esums)[slot_s].defs) | (*out_esums)[slot_s].uses) |
                      df.live_in[b + 1];
      }
      (*out_live_after)[slot_s] = slot_after;
      (*out_live_after)[s] = before_slot;
      cur_l = (before_slot & ~(*out_esums)[s].defs) | (*out_esums)[s].uses;
      --rev;
      continue;
    }
    (*out_live_after)[s] = cur_l;
    RegMask step_uses = (*out_esums)[s].uses;
    if (!steps[s].in_slot && steps[s].pn->pattern.op == RtlOp::kUse && s + 2 < ns &&
        steps[s + 1].in_slot && !steps[s + 2].in_slot &&
        steps[s + 1].insn_idx == steps[s + 2].insn_idx &&
        IsReturnJumpPattern(steps[s + 2].pn->pattern)) {
      step_uses &= ~(*out_esums)[s + 1].defs;
    }
    cur_l = (cur_l & ~(*out_esums)[s].defs) | step_uses;
  }
}

// Mirrors `toplev.c:3140-3143` -> `jump.c:434-465` (`jump_optimize` with
// `noop_moves = 1`): every normal register-to-register move `(set dreg sreg)`
// (`sreg != dreg`) walks backward via `prev_nonnote_insn` up to the nearest
// `CODE_LABEL` and calls `remove_death(dreg, trial)` (`jump.c:457`) on the
// first instruction carrying `REG_DEAD dreg`. When `dreg` is live across an
// intervening conditional jump so the jump carried no `REG_DEAD dreg`, this
// strips `REG_DEAD dreg` from an earlier pointer-load self-RMW
// `(set (reg:M dreg) (mem:M <addr reading dreg>))` before `.11.jump2`/`.11.dbr`.
void StripStage0Jump2NoopMoveConsumedDeaths(const RtlControlFlowGraph& cfg,
                                            const DataflowResult& df,
                                            std::vector<Stage0BlockParsed>* bp_ptr) {
  auto& bp = *bp_ptr;
  const size_t nb = cfg.rpo_blocks.size();
  struct FlatStep {
    size_t b = 0;
    Stage0StepRef* ref = nullptr;
  };
  size_t run_start = 0;
  while (run_start < nb) {
    size_t run_end = run_start + 1;
    while (run_end < nb && !cfg.rpo_blocks[run_end].has_label) {
      ++run_end;
    }
    std::vector<FlatStep> run_steps;
    for (size_t b = run_start; b < run_end; ++b) {
      for (Stage0StepRef& st : bp[b].steps) {
        if (st.pn->pattern.op == RtlOp::kNote || st.pn->pattern.op == RtlOp::kAddrVec ||
            st.pn->pattern.op == RtlOp::kAddrDiffVec) {
          continue;
        }
        run_steps.push_back(FlatStep{.b = b, .ref = &st});
      }
    }
    for (size_t idx = 0; idx < run_steps.size(); ++idx) {
      const Stage0StepRef& cur = *run_steps[idx].ref;
      if (cur.in_slot) {
        const size_t cb = run_steps[idx].b;
        auto is_uncond_head = [&](size_t h_idx) {
          if (h_idx >= run_steps.size() || run_steps[h_idx].b != cb ||
              run_steps[h_idx].ref->insn_idx != cur.insn_idx || run_steps[h_idx].ref->in_slot) {
            return false;
          }
          const RtlExpr& hpat = run_steps[h_idx].ref->pn->pattern;
          return IsSimpleUncondJumpSummaryPattern(hpat) || IsReturnJumpPattern(hpat);
        };
        if (!is_uncond_head(idx + 1) && (idx == 0 || !is_uncond_head(idx - 1))) {
          continue;
        }
      }
      const RtlExpr& pat = cur.pn->pattern;
      if (!pat.IsSet() || pat.children.size() != 2 || !pat.SetDest().IsHardReg() ||
          pat.SetDest().IsArgPointerReg() || !pat.SetSrc().IsHardReg() ||
          pat.SetSrc().IsArgPointerReg() || IsAnyControlTransferPattern(pat)) {
        continue;
      }
      const int dreg = pat.SetDest().RegNo();
      const int sreg = pat.SetSrc().RegNo();
      if (dreg < 2 || dreg >= 64 || sreg < 2 || sreg >= 64 || dreg == sreg) {
        continue;
      }
      bool saw_non_branch_read_or_def = false;
      bool saw_live_branch_read = false;
      bool saw_any_read_or_call_arg = false;
      bool saw_call_clobber_of_dreg = false;
      const bool is_o32_call_arg = (dreg >= 4 && dreg <= 7) || (dreg >= 44 && dreg <= 47);
      const bool is_call_clobbered_non_arg_fpr =
          !pat.SetDest().flags.integrated && !is_o32_call_arg && dreg >= 36 && dreg < 52;
      for (size_t prev = idx; prev > 0;) {
        --prev;
        Stage0StepRef& pst = *run_steps[prev].ref;
        if (pst.in_slot) {
          const size_t pb = run_steps[prev].b;
          auto is_call_head = [&](size_t h_idx) {
            return h_idx < run_steps.size() && run_steps[h_idx].b == pb &&
                   run_steps[h_idx].ref->insn_idx == pst.insn_idx &&
                   !run_steps[h_idx].ref->in_slot &&
                   IsCallPattern(run_steps[h_idx].ref->pn->pattern);
          };
          if (!is_call_head(prev + 1) && (prev == 0 || !is_call_head(prev - 1))) {
            break;
          }
        }
        const RtlExpr& ppat = pst.pn->pattern;
        auto dead_it = std::find_if(pst.pn->reg_notes.begin(), pst.pn->reg_notes.end(),
                                    [&](const RtlRegNote& n) {
                                      return n.kind == RtlMachineMode::kRegDead &&
                                             n.datum.IsHardReg() && n.datum.RegNo() == dreg;
                                    });
        if (dead_it != pst.pn->reg_notes.end()) {
          const bool is_self_rmw_across_branch =
              saw_live_branch_read && !saw_non_branch_read_or_def && ppat.IsSet() &&
              ppat.children.size() == 2 && ppat.SetDest().IsHardReg() &&
              ppat.SetDest().RegNo() == dreg && !ppat.SetDest().flags.integrated &&
              ppat.SetSrc().op == RtlOp::kMem && ppat.SetSrc().ContainsReg(dreg);
          // `jump.c:452-459` walks backward via `prev_nonnote_insn` past calls
          // and non-label insns, unconditionally calling `remove_death(dreg,
          // trial)` on the first insn carrying `REG_DEAD dreg`. When no
          // intervening read of `dreg` (or call-arg use) exists in the same
          // basic block and either (a) a `CALL_INSN` clobbering non-arg `dreg`
          // separates `trial` from `idx`, or (b) non-arg FPR `dreg` was loaded
          // locally from a `CONST_DOUBLE` right before `trial`, `trial`'s
          // `REG_DEAD dreg` cannot survive `.11.jump2` into `.11.dbr`.
          bool defined_locally_from_const_double = false;
          if (!saw_call_clobber_of_dreg && !saw_any_read_or_call_arg && dreg >= 32 && dreg < 64 &&
              !is_o32_call_arg && run_steps[prev].b == run_steps[idx].b) {
            for (size_t def_k = prev; def_k-- > 0;) {
              if (run_steps[def_k].b != run_steps[prev].b) {
                break;
              }
              const RtlExpr& dpat = run_steps[def_k].ref->pn->pattern;
              if (dpat.IsSet() && dpat.children.size() == 2 && dpat.SetDest().IsHardReg() &&
                  dpat.SetDest().RegNo() == dreg) {
                defined_locally_from_const_double = (dpat.SetSrc().op == RtlOp::kConstDouble);
                break;
              }
              ElemSummary des;
              AnalyzePatternAst(dpat, df.gp64, df.fp64, &des);
              if ((des.defs & (RegMask{1} << dreg)) != 0) {
                break;
              }
            }
          }
          const bool consumed_in_same_block =
              (saw_call_clobber_of_dreg || defined_locally_from_const_double) &&
              !saw_any_read_or_call_arg && run_steps[prev].b == run_steps[idx].b;
          if (is_self_rmw_across_branch || consumed_in_same_block) {
            pst.pn->reg_notes.erase(dead_it);
            bp[run_steps[prev].b].dirty[pst.insn_idx] = true;
          }
          break;
        }
        ElemSummary pes;
        AnalyzePatternAst(ppat, df.gp64, df.fp64, &pes);
        const RegMask dm = RegMask{1} << dreg;
        const bool is_call = IsCallPattern(ppat);
        if (is_call) {
          if (is_o32_call_arg) {
            saw_any_read_or_call_arg = true;
          } else if (is_call_clobbered_non_arg_fpr) {
            saw_call_clobber_of_dreg = true;
          }
        }
        if ((pes.uses & dm) != 0) {
          saw_any_read_or_call_arg = true;
          if (ppat.IsSet() && ppat.SetDest().op == RtlOp::kPc &&
              ppat.SetSrc().op == RtlOp::kIfThenElse) {
            saw_live_branch_read = true;
          } else {
            saw_non_branch_read_or_def = true;
          }
        }
        if ((pes.defs & dm) != 0) {
          saw_non_branch_read_or_def = true;
        }
      }
    }
    run_start = run_end;
  }
}

// Mirrors `reload1.c:3445-3508` (`spill_hard_reg`) and `reload1.c:5641-6640`
// (`emit_reload_insns`): once an `FP_REGS` hard register `fpr_r` is proven to
// be a reload spill register in basic block `b` (via a `proven_spill_offsets`
// FP load/store or a cross-bank GPR<->FPR reload in block `b`),
// `spill_hard_reg` evicts every pseudo overlapping `fpr_r` in block `b`. Any
// local `(set (reg:SF|DF fpr_r) (const_double ...))` feeding pure FP
// computation in block `b` (not live-in/out of `b`) was therefore emitted by
// `reload` after `flow`, so its final consumer does not carry `REG_DEAD fpr_r`.
void StripProvenFpSpillConstDoubleDeadNotes(const DataflowResult& df, size_t b,
                                            RegMask block_fp_spill_regs,
                                            const std::vector<ElemSummary>& esums,
                                            const std::vector<RegMask>& live_after_step,
                                            Stage0BlockParsed* blk_parsed) {
  if (block_fp_spill_regs == 0) {
    return;
  }
  auto& steps = blk_parsed->steps;
  const size_t ns = steps.size();
  const RegMask live_boundary = df.live_in[b] | df.live_out[b];
  for (size_t s = 0; s + 1 < ns; ++s) {
    const RtlExpr& pat = steps[s].pn->pattern;
    if (!pat.IsSet() || !pat.SetDest().IsHardReg() || pat.SetDest().flags.integrated ||
        pat.SetSrc().op != RtlOp::kConstDouble) {
      continue;
    }
    const RtlMachineMode mode = pat.SetDest().mode;
    if (mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDf) {
      continue;
    }
    const int fpr_r = pat.SetDest().RegNo();
    if (fpr_r < 32 || fpr_r >= 64) {
      continue;
    }
    const RegMask fpr_m = RegBitMask(fpr_r, mode, df.gp64, df.fp64);
    if ((block_fp_spill_regs & fpr_m) == 0 || (live_boundary & fpr_m) != 0) {
      continue;
    }
    size_t last_u = ns;
    bool all_pure_fp_consumers = true;
    for (size_t k = s + 1; k < ns; ++k) {
      const RtlExpr& kpat = steps[k].pn->pattern;
      if (esums[k].is_call || IsAnyControlTransferPattern(kpat)) {
        break;
      }
      if ((esums[k].uses & fpr_m) != 0) {
        if (kpat.op == RtlOp::kUse || kpat.op == RtlOp::kClobber ||
            (kpat.IsSet() && (kpat.SetSrc().IsReg() || kpat.SetSrc().op == RtlOp::kMem))) {
          all_pure_fp_consumers = false;
          break;
        }
        last_u = k;
        if ((esums[k].defs & fpr_m) != 0 || (live_after_step[k] & fpr_m) == 0) {
          break;
        }
      } else if ((esums[k].defs & fpr_m) != 0) {
        break;
      }
    }
    if (all_pure_fp_consumers && last_u < ns &&
        ((esums[last_u].defs & fpr_m) != 0 || (live_after_step[last_u] & fpr_m) == 0)) {
      StripStage0RegDeadOnStep(blk_parsed, &steps[last_u], fpr_r);
    }
  }
}

// Mirrors `sched.c:2956-2986` (`attach_deaths` / `attach_deaths_insn`):
// `attach_deaths` traverses `SET_SRC` operands right-to-left and prepends each
// `REG_DEAD` note to `REG_NOTES(insn)`, so dying operands appear in left-to-right
// operand order. Ordering hard-register `REG_DEAD` notes on `pn` before
// upgrading `(REG_DEAD (reg scr))` to `(REG_DEAD (mem [sp+off]))` ensures
// `CanonicalizeCfgLivenessNoteOrder` preserves the spilled operand's exact
// relative slot against co-dying hard-register operands.
void OrderRegDeadNotesInAttachDeathsOrderBeforeSpillUpgrade(RtlPatternAndNotes* pn, bool gp64,
                                                            bool fp64) {
  if (pn == nullptr || pn->reg_notes.size() < 2) {
    return;
  }
  std::vector<int> read_order;
  auto collect_reads = [&](const auto& self, const RtlExpr& x, bool set_p) -> void {
    switch (x.op) {
      case RtlOp::kReg:
        if (!set_p && x.RegNo() >= 0 && x.RegNo() < kMipsFirstPseudoRegister) {
          const int nr = HardRegNregs(x.RegNo(), x.mode, gp64, fp64);
          for (int k = 0; k < nr; ++k) {
            if (std::find(read_order.begin(), read_order.end(), x.RegNo() + k) ==
                read_order.end()) {
              read_order.push_back(x.RegNo() + k);
            }
          }
        }
        return;
      case RtlOp::kMem:
        if (!x.children.empty()) {
          self(self, x.children[0], false);
        }
        return;
      case RtlOp::kSubreg:
      case RtlOp::kStrictLowPart:
        if (!x.children.empty()) {
          self(self, x.children[0], set_p);
        }
        return;
      case RtlOp::kZeroExtract:
      case RtlOp::kSignExtract:
        if (x.children.size() >= 1) self(self, x.children[0], set_p);
        if (x.children.size() >= 2) self(self, x.children[1], false);
        if (x.children.size() >= 3) self(self, x.children[2], false);
        return;
      default:
        for (const RtlExpr& ch : x.children) {
          self(self, ch, false);
        }
        return;
    }
  };
  const RtlExpr& pat = pn->pattern;
  // Mirrors `sched.c:2978-2986` (`attach_deaths_insn`): `attach_deaths` visits
  // `SET_SRC` then `SET_DEST` and prepends each `REG_DEAD` note, so `SET_DEST`
  // `MEM` address deaths appear ahead of `SET_SRC` deaths.
  if (pat.IsSet()) {
    collect_reads(collect_reads, pat.SetDest(), true);
    collect_reads(collect_reads, pat.SetSrc(), false);
  } else if (pat.IsParallel()) {
    for (const RtlExpr& ch : pat.children) {
      if (ch.IsSet()) {
        collect_reads(collect_reads, ch.SetDest(), true);
        collect_reads(collect_reads, ch.SetSrc(), false);
      } else if (ch.op != RtlOp::kClobber) {
        collect_reads(collect_reads, ch, false);
      }
    }
  } else {
    collect_reads(collect_reads, pat, false);
  }
  auto rank_note = [&](const RtlRegNote& n) -> size_t {
    if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg()) {
      const auto it = std::find(read_order.begin(), read_order.end(), n.datum.RegNo());
      if (it != read_order.end()) {
        return static_cast<size_t>(it - read_order.begin());
      }
    }
    return read_order.size();
  };
  std::vector<size_t> dead_slots;
  std::vector<RtlRegNote> dead_notes;
  for (size_t i = 0; i < pn->reg_notes.size(); ++i) {
    if (pn->reg_notes[i].kind == RtlMachineMode::kRegDead && pn->reg_notes[i].datum.IsReg()) {
      dead_slots.push_back(i);
      dead_notes.push_back(pn->reg_notes[i]);
    }
  }
  if (dead_notes.size() < 2) {
    return;
  }
  std::stable_sort(
      dead_notes.begin(), dead_notes.end(),
      [&](const RtlRegNote& a, const RtlRegNote& b) { return rank_note(a) < rank_note(b); });
  for (size_t k = 0; k < dead_slots.size(); ++k) {
    pn->reg_notes[dead_slots[k]] = std::move(dead_notes[k]);
  }
}

void NormalizeStage0PostReloadRegNotes(RtlControlFlowGraph* cfg, const DataflowResult& df,
                                       const CompilationProfile& profile) {
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  const size_t nb = cfg->rpo_blocks.size();
  using StepRef = Stage0StepRef;
  using BlockParsed = Stage0BlockParsed;
  std::vector<BlockParsed> bp;
  ParseStage0Blocks(*cfg, &bp);

  auto extract_sp_mem = [](const RtlExpr& mem, int64_t* out_off, int64_t* out_sz) -> bool {
    return ExtractStage0SpMem(mem, out_off, out_sz);
  };
  auto strip_reg_dead_on_step = [&](size_t b, StepRef& st, int regno) -> bool {
    return StripStage0RegDeadOnStep(&bp[b], &st, regno);
  };

  bool found_b0_sp = false;
  size_t b0_sp_step = 0;
  absl::flat_hash_set<size_t> prologue_save_steps;
  absl::flat_hash_set<int64_t> caller_save_offsets;
  absl::flat_hash_set<int64_t> proven_spill_offsets;
  IdentifyStage0CallerSaveAndSpillOffsets(*cfg, df, bp, &found_b0_sp, &b0_sp_step,
                                          &prologue_save_steps, &caller_save_offsets,
                                          &proven_spill_offsets);
  const Stage0MemEquivLayout mem_equiv_layout =
      (flags.opt_level > 0) ? ComputeStage0MemEquivLayout(*cfg, df, flags, bp, found_b0_sp,
                                                          b0_sp_step, prologue_save_steps,
                                                          caller_save_offsets, proven_spill_offsets)
                            : Stage0MemEquivLayout{};

  absl::flat_hash_map<int64_t, std::vector<bool>> spill_live_in;
  absl::flat_hash_map<int64_t, std::vector<bool>> spill_live_out;
  for (int64_t off : proven_spill_offsets) {
    std::vector<bool> lin(nb, false), lout(nb, false);
    bool chg = true;
    while (chg) {
      chg = false;
      for (size_t rev_b = nb; rev_b > 0; --rev_b) {
        const size_t b = rev_b - 1;
        bool out_val = false;
        for (size_t succ : cfg->rpo_blocks[b].successors) {
          if (succ < nb && lin[succ]) out_val = true;
        }
        lout[b] = out_val;
        bool cur = out_val;
        for (size_t rev_s = bp[b].steps.size(); rev_s > 0; --rev_s) {
          const RtlExpr& pat = bp[b].steps[rev_s - 1].pn->pattern;
          int64_t moff = 0, msz = 0;
          if (pat.IsSet() && extract_sp_mem(pat.SetDest(), &moff, &msz) && moff == off) {
            cur = false;
          } else if (pat.IsSet() && extract_sp_mem(pat.SetSrc(), &moff, &msz) && moff == off) {
            cur = true;
          }
        }
        if (cur != lin[b]) {
          lin[b] = cur;
          chg = true;
        }
      }
    }
    spill_live_in[off] = std::move(lin);
    spill_live_out[off] = std::move(lout);
  }

  std::vector<std::vector<ElemSummary>> all_esums(nb);
  std::vector<std::vector<RegMask>> all_live_after(nb);
  for (size_t b = 0; b < nb; ++b) {
    ComputeStage0BlockLiveAfterSteps(*cfg, df, b, bp[b], &all_esums[b], &all_live_after[b]);
  }

  for (size_t b = 0; b < nb; ++b) {
    auto& steps = bp[b].steps;
    const size_t ns = steps.size();
    const auto& esums = all_esums[b];
    const auto& live_after_step = all_live_after[b];

    RegMask block_fp_spill_regs = 0;
    NormalizeStage0MdFprAndDiReloadNotes(*cfg, df, flags.opt_level > 0, flags.RunsSched2(),
                                         flags.flag_expensive_optimizations, b, esums,
                                         live_after_step, prologue_save_steps, proven_spill_offsets,
                                         &bp, &block_fp_spill_regs);

    // 3. `caller-save` stores and proven `reload` stack-spill loads/stores.
    if (!caller_save_offsets.empty() || !proven_spill_offsets.empty()) {
      absl::flat_hash_map<int64_t, std::vector<bool>> slot_live_after(proven_spill_offsets.size());
      for (int64_t off : proven_spill_offsets) {
        std::vector<bool> la(ns, false);
        bool cur = spill_live_out[off][b];
        for (size_t rev = ns; rev > 0; --rev) {
          const size_t idx = rev - 1;
          la[idx] = cur;
          const RtlExpr& pat = steps[idx].pn->pattern;
          int64_t moff = 0, msz = 0;
          if (pat.IsSet() && extract_sp_mem(pat.SetDest(), &moff, &msz) && moff == off) {
            cur = false;
          } else if (pat.IsSet() && extract_sp_mem(pat.SetSrc(), &moff, &msz) && moff == off) {
            cur = true;
          }
        }
        slot_live_after[off] = std::move(la);
      }

      for (size_t s = 0; s < ns; ++s) {
        if (b == 0 && ((found_b0_sp && s <= b0_sp_step) || prologue_save_steps.contains(s))) {
          continue;
        }
        const RtlExpr& pat = steps[s].pn->pattern;
        int64_t off = 0, sz = 0;
        if (pat.IsSet() && pat.SetSrc().IsReg() && extract_sp_mem(pat.SetDest(), &off, &sz)) {
          if (caller_save_offsets.contains(off)) {
            strip_reg_dead_on_step(b, steps[s], pat.SetSrc().RegNo());
          } else if (proven_spill_offsets.contains(off)) {
            const int scr = pat.SetSrc().RegNo();
            const RegMask scr_m = RegBitMask(scr, pat.SetSrc().mode, df.gp64, df.fp64);
            // Note: do NOT mark `scr` in `block_fp_spill_regs` from an output-reload
            // store alone (`reload1.c:4712` can reuse a dying allocated pseudo's hard
            // register for an output reload without calling `spill_hard_reg(scr)`).
            bool pure_output_reload_scratch = false;
            bool saw_read_between = false;
            size_t dying_arg_prod_step = ns;
            for (size_t prev = s; prev > 0; --prev) {
              const size_t pi = prev - 1;
              if ((esums[pi].defs & scr_m) != 0) {
                const RtlExpr& pdef = steps[pi].pn->pattern;
                if (pdef.IsSet() && !pdef.SetSrc().IsReg()) {
                  pure_output_reload_scratch = true;
                  if (!saw_read_between &&
                      IsStage0DyingPseudoOverwrittenBySpillOutputReload(
                          *cfg, df, b, pi, scr, scr_m, steps, esums, proven_spill_offsets)) {
                    dying_arg_prod_step = pi;
                  }
                }
                break;
              }
              if ((esums[pi].uses & scr_m) != 0) {
                const RtlExpr& ppi = steps[pi].pn->pattern;
                const bool is_reg_copy_from_scr =
                    flags.RunsSched2() && ppi.IsSet() && ppi.SetDest().IsReg() &&
                    !ppi.SetDest().flags.integrated && ppi.SetSrc().IsReg();
                if (!is_reg_copy_from_scr) {
                  saw_read_between = true;
                }
              }
              if (esums[pi].is_call || IsAnyControlTransferPattern(steps[pi].pn->pattern)) {
                break;
              }
            }
            if (pure_output_reload_scratch) {
              strip_reg_dead_on_step(b, steps[s], scr);
              if ((live_after_step[s] & scr_m) != 0) {
                size_t last_u = ns;
                bool slot_reloaded_between = false;
                for (size_t k = s + 1; k < ns; ++k) {
                  int64_t koff = 0, ksz = 0;
                  if (steps[k].pn->pattern.IsSet() &&
                      extract_sp_mem(steps[k].pn->pattern.SetSrc(), &koff, &ksz) && koff == off) {
                    slot_reloaded_between = true;
                  }
                  if ((esums[k].uses & scr_m) != 0) {
                    last_u = k;
                    if ((live_after_step[k] & scr_m) == 0) {
                      break;
                    }
                  }
                  if ((esums[k].defs & scr_m) != 0 || esums[k].is_call ||
                      IsAnyControlTransferPattern(steps[k].pn->pattern)) {
                    break;
                  }
                }
                if (last_u < ns && (live_after_step[last_u] & scr_m) == 0) {
                  const bool slot_dies = !slot_reloaded_between && !slot_live_after[off][s];
                  if (steps[last_u].in_slot) {
                    if (!slot_dies) {
                      strip_reg_dead_on_step(b, steps[last_u], scr);
                    }
                  } else if (slot_dies) {
                    bool replaced_in_place = false;
                    for (RtlRegNote& n : steps[last_u].pn->reg_notes) {
                      if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                          n.datum.RegNo() == scr) {
                        n.datum = pat.SetDest();
                        replaced_in_place = true;
                        bp[b].dirty[steps[last_u].insn_idx] = true;
                        break;
                      }
                    }
                    if (!replaced_in_place) {
                      steps[last_u].pn->reg_notes.insert(steps[last_u].pn->reg_notes.begin(),
                                                         RtlRegNote{
                                                             .kind = RtlMachineMode::kRegDead,
                                                             .list_op = RtlOp::kExprList,
                                                             .datum = pat.SetDest(),
                                                         });
                      bp[b].dirty[steps[last_u].insn_idx] = true;
                    }
                  } else {
                    strip_reg_dead_on_step(b, steps[last_u], scr);
                  }
                }
              }
            }
            if (dying_arg_prod_step < ns && !HasRegNote(steps[dying_arg_prod_step].pn->reg_notes,
                                                        RtlMachineMode::kRegDead, scr)) {
              steps[dying_arg_prod_step].pn->reg_notes.insert(
                  steps[dying_arg_prod_step].pn->reg_notes.begin(),
                  RtlRegNote{
                      .kind = RtlMachineMode::kRegDead,
                      .list_op = RtlOp::kExprList,
                      .datum = MakeGccHardReg(pat.SetSrc().mode, scr),
                  });
              bp[b].dirty[steps[dying_arg_prod_step].insn_idx] = true;
            }
          }
        } else if (pat.IsSet() && pat.SetDest().IsReg() &&
                   extract_sp_mem(pat.SetSrc(), &off, &sz) && proven_spill_offsets.contains(off)) {
          const int scr = pat.SetDest().RegNo();
          const RegMask scr_m = RegBitMask(scr, pat.SetDest().mode, df.gp64, df.fp64);
          if (scr >= 32 && scr < 64) {
            block_fp_spill_regs |= scr_m;
          }
          size_t last_u = ns;
          bool slot_reloaded_between = false;
          for (size_t k = s + 1; k < ns; ++k) {
            int64_t koff = 0, ksz = 0;
            if (steps[k].pn->pattern.IsSet() &&
                extract_sp_mem(steps[k].pn->pattern.SetSrc(), &koff, &ksz) && koff == off) {
              slot_reloaded_between = true;
            }
            if ((esums[k].uses & scr_m) != 0) {
              last_u = k;
              if ((live_after_step[k] & scr_m) == 0) {
                break;
              }
            }
            if ((esums[k].defs & scr_m) != 0 || esums[k].is_call ||
                IsAnyControlTransferPattern(steps[k].pn->pattern)) {
              break;
            }
          }
          if (last_u < ns && (live_after_step[last_u] & scr_m) == 0) {
            const bool slot_dies = !slot_reloaded_between && !slot_live_after[off][s];
            if (steps[last_u].in_slot) {
              if (!slot_dies) {
                strip_reg_dead_on_step(b, steps[last_u], scr);
              }
            } else if (slot_dies) {
              if (flags.RunsSched1()) {
                OrderRegDeadNotesInAttachDeathsOrderBeforeSpillUpgrade(steps[last_u].pn, df.gp64,
                                                                       df.fp64);
              }
              bool replaced_in_place = false;
              for (RtlRegNote& n : steps[last_u].pn->reg_notes) {
                if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() &&
                    n.datum.RegNo() == scr) {
                  n.datum = pat.SetSrc();
                  replaced_in_place = true;
                  bp[b].dirty[steps[last_u].insn_idx] = true;
                  break;
                }
              }
              if (!replaced_in_place) {
                steps[last_u].pn->reg_notes.insert(steps[last_u].pn->reg_notes.begin(),
                                                   RtlRegNote{
                                                       .kind = RtlMachineMode::kRegDead,
                                                       .list_op = RtlOp::kExprList,
                                                       .datum = pat.SetSrc(),
                                                   });
                bp[b].dirty[steps[last_u].insn_idx] = true;
              }
            } else {
              strip_reg_dead_on_step(b, steps[last_u], scr);
            }
          }
        }
      }
    }

    // 4. `!gp64` `DImode` `"=&d"` earlyclobber output reload copies
    // `(parallel [(set (reg:DI scr) (op:DI ...)) (clobber ...)])` ->
    // `(set (reg:DI dst) (reg:DI scr))`: when `dst` overlaps the colliding RHS
    // input of the `"=&d"` pattern, `reload` allocated `scr` after `flow` and
    // emitted the copy without `REG_DEAD (reg:DI scr)`.
    if (!df.gp64) {
      for (size_t s = 1; s < ns; ++s) {
        if (steps[s].in_slot) {
          continue;
        }
        const RtlExpr& mv = steps[s].pn->pattern;
        if (!mv.IsSet() || !mv.SetDest().IsHardReg() || mv.SetDest().mode != RtlMachineMode::kDi ||
            !mv.SetSrc().IsHardReg() || mv.SetSrc().mode != RtlMachineMode::kDi) {
          continue;
        }
        const int dst_hr = mv.SetDest().RegNo();
        const int scr_hr = mv.SetSrc().RegNo();
        if (dst_hr < 2 || dst_hr > 24 || scr_hr < 2 || scr_hr > 24 || dst_hr == scr_hr) {
          continue;
        }
        const RegMask scr_m = RegBitMask(scr_hr, RtlMachineMode::kDi, df.gp64, df.fp64);
        if ((live_after_step[s] & scr_m) != 0) {
          continue;
        }
        for (size_t rev = s; rev > 0; --rev) {
          const size_t p = rev - 1;
          if ((esums[p].defs & scr_m) != 0) {
            const RtlExpr& pp = steps[p].pn->pattern;
            if (!steps[p].in_slot && pp.op == RtlOp::kParallel && pp.children.size() >= 2 &&
                pp.children[0].IsSet() && pp.children[1].op == RtlOp::kClobber &&
                pp.children[0].SetDest().IsHardReg() &&
                pp.children[0].SetDest().mode == RtlMachineMode::kDi &&
                pp.children[0].SetDest().RegNo() == scr_hr && (esums[p].uses & scr_m) == 0) {
              bool clobber_overlaps_dst = false;
              for (size_t c = 1; c < pp.children.size(); ++c) {
                if (pp.children[c].op == RtlOp::kClobber && !pp.children[c].children.empty() &&
                    pp.children[c].children[0].IsHardReg()) {
                  const int chr = pp.children[c].children[0].RegNo();
                  const int cnw =
                      HardRegNregs(chr, pp.children[c].children[0].mode, df.gp64, df.fp64);
                  if (std::max(dst_hr, chr) < std::min(dst_hr + 2, chr + cnw)) {
                    clobber_overlaps_dst = true;
                  }
                }
              }
              bool earlyclobber_forced = false;
              const RtlExpr& rhs = pp.children[0].SetSrc();
              if (!clobber_overlaps_dst) {
                const bool plus_imm_same_dst = rhs.op == RtlOp::kPlus && rhs.children.size() == 2 &&
                                               rhs.children[0].IsHardReg() &&
                                               rhs.children[0].RegNo() == dst_hr &&
                                               rhs.children[1].IsConstInt();
                if (((rhs.op == RtlOp::kPlus && !plus_imm_same_dst) || rhs.op == RtlOp::kMinus ||
                     rhs.op == RtlOp::kAshift || rhs.op == RtlOp::kAshiftrt ||
                     rhs.op == RtlOp::kLshiftrt)) {
                  const RegMask dst_m = RegBitMask(dst_hr, RtlMachineMode::kDi, df.gp64, df.fp64);
                  earlyclobber_forced = (esums[p].uses & dst_m) != 0;
                }
              }
              if (earlyclobber_forced) {
                strip_reg_dead_on_step(b, steps[s], scr_hr);
              }
            }
            break;
          }
          if ((esums[p].uses & scr_m) != 0 || esums[p].is_call ||
              IsAnyControlTransferPattern(steps[p].pn->pattern)) {
            break;
          }
        }
      }
    }

    if (flags.opt_level > 0) {
      AttachStage0SplitLocalToGlobalRmwRegDeadNotes(*cfg, df, b, bp, esums, live_after_step,
                                                    caller_save_offsets, proven_spill_offsets,
                                                    &bp[b]);
      StripProvenFpSpillConstDoubleDeadNotes(df, b, block_fp_spill_regs, esums, live_after_step,
                                             &bp[b]);
    }
  }

  if (flags.opt_level > 0 && (cfg->pass == RtlPassKind::kDbr || cfg->pass == RtlPassKind::kJump2)) {
    StripStage0Jump2NoopMoveConsumedDeaths(*cfg, df, &bp);
  }

  if (flags.opt_level > 0) {
    std::vector<std::vector<size_t>> block_preds(nb);
    for (size_t src_b = 0; src_b < nb; ++src_b) {
      ForEachStage0CfgSucc(cfg->rpo_blocks[src_b], nb, [&](size_t succ) {
        if (std::find(block_preds[succ].begin(), block_preds[succ].end(), src_b) ==
            block_preds[succ].end()) {
          block_preds[succ].push_back(src_b);
        }
      });
    }
    // Process blocks in reverse RPO order so `succ_b > b` already carries its
    // synthesized `REG_EQUIV` notes when `CanJoinStoreBeCrossJumpedIntoPreds`
    // checks `HasStage0NonShareableEqualOrEquivNote` on `succ_b`.
    for (size_t b = nb; b-- > 0;) {
      SynthesizeStage0MemRegEquivNotes(*cfg, df, flags, mem_equiv_layout, b, found_b0_sp,
                                       b0_sp_step, prologue_save_steps, caller_save_offsets,
                                       proven_spill_offsets, bp, all_esums, all_live_after,
                                       block_preds, &bp[b]);
    }
  }

  for (size_t b = 0; b < nb; ++b) {
    FlushNormalizedStage0BlockInsns(b, bp[b], cfg);
  }
}

RegMask ComputeGlobalExplicitRegMaskForAtFix(const RtlControlFlowGraph& cfg,
                                             const DataflowResult& df, RegMask exit_ret_mask,
                                             bool has_at_fix_clobber) {
  RegMask global_explicit_reg_mask =
      exit_ret_mask | df.epilogue_delay.defs | df.epilogue_delay.uses;
  if (!has_at_fix_clobber) {
    return global_explicit_reg_mask;
  }
  const size_t n_blocks = cfg.rpo_blocks.size();
  auto is_sp_based_mem = [](const RtlExpr& m) {
    if (!m.IsMem() || m.children.empty()) return false;
    const RtlExpr& a = m.children[0];
    if (a.IsReg() && a.RegNo() == 29) return true;
    return a.op == RtlOp::kPlus && a.children.size() == 2 && a.children[0].IsReg() &&
           a.children[0].RegNo() == 29 && a.children[1].IsConstInt();
  };
  auto is_sp_decrement = [](const RtlExpr& pat) {
    return pat.IsSet() && pat.SetDest().IsReg() && pat.SetDest().RegNo() == 29 &&
           pat.SetSrc().op == RtlOp::kPlus && pat.SetSrc().children.size() == 2 &&
           pat.SetSrc().children[0].IsReg() && pat.SetSrc().children[0].RegNo() == 29 &&
           pat.SetSrc().children[1].IsConstInt() && pat.SetSrc().children[1].int_val < 0;
  };
  bool b0_has_sp_dec = false;
  if (n_blocks > 0) {
    for (const InsnSummary& isum : df.block_summaries[0]) {
      if (!isum.valid) continue;
      if (isum.is_sequence) {
        if (is_sp_decrement(isum.head_pn.pattern) || is_sp_decrement(isum.slot_pn.pattern)) {
          b0_has_sp_dec = true;
          break;
        }
      } else if (is_sp_decrement(isum.top_pn.pattern)) {
        b0_has_sp_dec = true;
        break;
      }
    }
  }
  for (size_t b = 0; b < n_blocks; ++b) {
    bool saw_b0_sp_dec = !b0_has_sp_dec;
    bool in_b0_prologue = (b == 0 && b0_has_sp_dec);
    for (const InsnSummary& isum : df.block_summaries[b]) {
      if (!isum.valid) continue;
      auto add_elem_explicit = [&](const ElemSummary& e, const RtlExpr& pat) {
        if (b == 0 && !saw_b0_sp_dec) {
          if (is_sp_decrement(pat)) {
            saw_b0_sp_dec = true;
            return;
          }
          if (pat.IsSet() && is_sp_based_mem(pat.SetDest()) && pat.SetSrc().IsReg()) {
            return;
          }
        }
        if (in_b0_prologue && (e.is_call || IsAnyControlTransferPattern(pat))) {
          in_b0_prologue = false;
        }
        if (in_b0_prologue && pat.IsSet() && is_sp_based_mem(pat.SetDest()) &&
            pat.SetSrc().IsReg()) {
          const int sr = pat.SetSrc().RegNo();
          if ((sr >= 16 && sr <= 23) || sr == 30 || sr == 31 || (sr >= 52 && sr <= 63)) {
            return;
          }
        }
        if (e.is_call) {
          global_explicit_reg_mask |= (e.uses | e.explicit_sets);
        } else {
          global_explicit_reg_mask |= (e.uses | e.defs);
        }
      };
      if (isum.is_sequence) {
        add_elem_explicit(isum.head, isum.head_pn.pattern);
        add_elem_explicit(isum.slot, isum.slot_pn.pattern);
      } else {
        add_elem_explicit(isum.single, isum.top_pn.pattern);
      }
    }
  }
  return global_explicit_reg_mask;
}

bool IsAtClobberFixParallel(const RtlExpr& pat) {
  return pat.op == RtlOp::kParallel && pat.children.size() == 3 &&
         pat.children[0].op == RtlOp::kSet && pat.children[0].children.size() == 2 &&
         pat.children[0].children[1].op == RtlOp::kFix && pat.children[1].op == RtlOp::kClobber &&
         pat.children[1].children.size() == 1 && pat.children[1].children[0].IsReg() &&
         pat.children[1].children[0].RegNo() == 1;
}

bool IsDisassembledMips1HighpartMul(const RtlExpr& pat, bool* is_unsigned = nullptr) {
  if (pat.op != RtlOp::kParallel || pat.children.size() != 3 || !pat.children[0].IsSet()) {
    return false;
  }
  const RtlExpr& dst = pat.children[0].SetDest();
  const RtlExpr& src = pat.children[0].SetSrc();
  if (!dst.IsReg() || dst.RegNo() != 64 || dst.mode != RtlMachineMode::kSi || dst.flags.Any() ||
      src.op != RtlOp::kTruncate || src.children.size() != 1 ||
      pat.children[1].op != RtlOp::kClobber || pat.children[1].children.size() != 1 ||
      !pat.children[1].children[0].IsReg() || pat.children[1].children[0].RegNo() != 65 ||
      pat.children[2].op != RtlOp::kClobber || pat.children[2].children.size() != 1 ||
      !pat.children[2].children[0].IsReg() || pat.children[2].children[0].RegNo() != 66) {
    return false;
  }
  const RtlExpr& lsh = src.children[0];
  if (lsh.op != RtlOp::kLshiftrt || lsh.children.size() != 2 || !lsh.children[1].IsConstInt() ||
      lsh.children[1].int_val != 32 || lsh.children[0].op != RtlOp::kMult ||
      lsh.children[0].children.size() != 2) {
    return false;
  }
  const RtlExpr& ext0 = lsh.children[0].children[0];
  const RtlExpr& ext1 = lsh.children[0].children[1];
  const bool both_sign = ext0.op == RtlOp::kSignExtend && ext1.op == RtlOp::kSignExtend;
  const bool both_zero = ext0.op == RtlOp::kZeroExtend && ext1.op == RtlOp::kZeroExtend;
  if ((!both_sign && !both_zero) || ext0.children.size() != 1 || ext1.children.size() != 1 ||
      ext0.children[0].mode != RtlMachineMode::kSi ||
      ext1.children[0].mode != RtlMachineMode::kSi) {
    return false;
  }
  if (is_unsigned != nullptr) {
    *is_unsigned = both_zero;
  }
  return true;
}

bool IsDemotedParallelHardSetDest(const DataflowResult& df, const CompilationProfile& profile,
                                  size_t b, size_t i, const RtlPatternAndNotes& pn,
                                  const ElemSummary& esum, RegMask live_after) {
  if (IsUnoptimizedProfile(&profile) || !pn.pattern.IsParallel() ||
      pn.pattern.children.size() < 3 || !pn.pattern.children[0].IsSet()) {
    return false;
  }
  const RtlExpr& dst = pn.pattern.children[0].SetDest();
  if (!dst.IsHardReg() || dst.IsArgPointerReg() || dst.flags.Any() ||
      dst.mode != RtlMachineMode::kSi) {
    return false;
  }
  const int sr = dst.RegNo();
  const RegMask sr_bit = RegMask{1} << sr;
  if (sr < 3 || sr > 25 || (esum.uses & sr_bit) != 0 || (df.live_in[b] & sr_bit) != 0 ||
      (live_after & sr_bit) == 0) {
    return false;
  }
  int clobber_count = 0;
  for (size_t ci = 1; ci < pn.pattern.children.size(); ++ci) {
    const RtlExpr& ch = pn.pattern.children[ci];
    if (ch.op == RtlOp::kClobber && ch.children.size() == 1 && ch.children[0].IsReg()) {
      ++clobber_count;
    }
  }
  if (clobber_count < 2) {
    return false;
  }
  for (size_t ob = 0; ob < df.block_summaries.size(); ++ob) {
    for (size_t oi = 0; oi < df.block_summaries[ob].size(); ++oi) {
      if (ob == b && oi == i) {
        continue;
      }
      const InsnSummary& os = df.block_summaries[ob][oi];
      if (!os.valid) {
        continue;
      }
      const RegMask defs = os.is_sequence ? (os.head.defs | os.slot.defs) : os.single.defs;
      const RegMask uses = os.is_sequence ? (os.head.uses | os.slot.uses) : os.single.uses;
      if ((defs & sr_bit) != 0 || (ob != b && (uses & sr_bit) != 0) ||
          (ob == b && oi < i && (uses & sr_bit) != 0)) {
        return false;
      }
    }
  }
  size_t use_j = df.block_summaries[b].size();
  RegMask touched_between = 0;
  for (size_t k = i + 1; k < df.block_summaries[b].size(); ++k) {
    const InsnSummary& ks = df.block_summaries[b][k];
    if (!ks.valid || ks.single.is_note_or_pseudo) {
      continue;
    }
    if (ks.is_sequence || ks.single.is_call) {
      if (use_j >= df.block_summaries[b].size()) {
        return false;
      }
      break;
    }
    if ((ks.single.uses & sr_bit) != 0) {
      if (use_j < df.block_summaries[b].size()) {
        return false;
      }
      if (!ks.top_pn.pattern.IsSet() || ks.top_pn.pattern.SetSrc().op == RtlOp::kReg ||
          !ks.top_pn.pattern.SetDest().IsHardReg() || ks.top_pn.pattern.SetDest().RegNo() == sr) {
        return false;
      }
      use_j = k;
      touched_between |= (ks.single.defs | ks.single.uses);
    } else if (use_j >= df.block_summaries[b].size()) {
      touched_between |= (ks.single.defs | ks.single.uses);
    }
  }
  if (use_j >= df.block_summaries[b].size()) {
    return false;
  }
  const int limit_h = std::min(sr, 16);
  for (int h = 2; h < limit_h; ++h) {
    const RegMask h_bit = RegMask{1} << h;
    if (((live_after | touched_between) & h_bit) == 0) {
      return true;
    }
  }
  return false;
}

// Detects a non-annulled conditional-branch `SEQUENCE` delay slot stolen from
// its single-predecessor fallthrough block `b + 1` by
// `reorg.c:fill_slots_from_thread` (`reorg.c:3375-3436`, `own_thread = 1`),
// whose `REG_DEAD` notes (`reorg.c:918-920` `(const_int 0)`) were computed
// against `df.live_in[b + 1]` rather than the union of taken and fallthrough
// live-in sets.
bool IsFallthroughStolenCondBranchDelaySlot(const RtlControlFlowGraph& cfg,
                                            const DataflowResult& df,
                                            const std::vector<std::vector<size_t>>& preds, size_t b,
                                            const InsnSummary& isum, RegMask taken_live) {
  const size_t n_blocks = cfg.rpo_blocks.size();
  const size_t ft_b = b + 1;
  if (ft_b >= n_blocks || isum.seq_slot_wrap.flags.in_struct ||
      isum.seq_head_wrap.flags.unchanging || cfg.rpo_blocks[b].successors.size() != 2 ||
      cfg.rpo_blocks[ft_b].has_label || preds[ft_b].size() != 1 || preds[ft_b][0] != b) {
    return false;
  }
  const RegMask slot_defs = isum.slot.explicit_sets;
  if (slot_defs == 0 || (taken_live & slot_defs) != 0 || (df.live_in[ft_b] & slot_defs) == 0) {
    return false;
  }
  if ((isum.head.uses & slot_defs) != 0) {
    return true;
  }
  // Mirror `reorg.c:mostly_true_jump` (`invert_fill_eager_delay_slots.cc:1627-1666`):
  // forward mostly-false branch whose first fallthrough active insn reads `slot_defs`.
  const RtlExpr& hpat = isum.head_pn.pattern;
  if (!hpat.IsSet() || hpat.SetDest().op != RtlOp::kPc || hpat.SetSrc().op != RtlOp::kIfThenElse ||
      hpat.SetSrc().children.size() != 3) {
    return false;
  }
  const RtlExpr& ite = hpat.SetSrc();
  const RtlExpr& cond = ite.children[0];
  const bool lbl_first =
      (ite.children[1].op == RtlOp::kLabelRef && ite.children[2].op == RtlOp::kPc);
  const bool pc_first =
      (ite.children[1].op == RtlOp::kPc && ite.children[2].op == RtlOp::kLabelRef);
  bool mostly_false_fwd = false;
  if ((lbl_first && cond.op == RtlOp::kEq) || (pc_first && cond.op == RtlOp::kNe)) {
    mostly_false_fwd = true;
  } else if (lbl_first && cond.children.size() == 2 && cond.children[1].IsConstInt() &&
             cond.children[1].int_val == 0 &&
             (cond.op == RtlOp::kLt || cond.op == RtlOp::kLe || cond.op == RtlOp::kLtu ||
              cond.op == RtlOp::kLeu)) {
    mostly_false_fwd = true;
  }
  if (!mostly_false_fwd) {
    return false;
  }
  const auto& succs = cfg.rpo_blocks[b].successors;
  const size_t target_b = (succs[0] == ft_b) ? succs[1] : succs[0];
  if (target_b <= b || target_b >= n_blocks) {
    return false;
  }
  bool slot_has_mem_or_vol = false;
  isum.slot_pn.pattern.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kMem || n.flags.volatil) {
      slot_has_mem_or_vol = true;
    }
  });
  if (slot_has_mem_or_vol) {
    return false;
  }
  for (const InsnSummary& ft_sum : df.block_summaries[ft_b]) {
    if (!ft_sum.valid || (!ft_sum.is_sequence && ft_sum.single.is_note_or_pseudo)) {
      continue;
    }
    const RegMask ft_uses =
        ft_sum.is_sequence ? (ft_sum.head.uses | ft_sum.slot.uses) : ft_sum.single.uses;
    return (ft_uses & slot_defs) != 0;
  }
  return false;
}

void AttachPostDbrBlockRegNotesAndFixupScratches(RtlControlFlowGraph* cfg, DataflowResult* df,
                                                 const CompilationProfile& profile,
                                                 RegMask exit_ret_mask, RegMask fixed_mask,
                                                 RegMask global_explicit_reg_mask,
                                                 bool attach_notes, bool has_smulsi3_highpart,
                                                 const std::vector<RegMask>& ret_need_in,
                                                 const std::vector<RegMask>& ret_need_out) {
  const size_t n_blocks = cfg->rpo_blocks.size();
  auto has_fallthrough_edge = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg->rpo_blocks[b].successors;
    return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
  };
  auto taken_also_goes_to_ft = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg->rpo_blocks[b].successors;
    return !succs.empty() &&
           std::all_of(succs.begin(), succs.end(), [b](size_t s) { return s == b + 1; });
  };
  auto exit_live_for_block = [&](size_t b) -> RegMask {
    if (!df->can_exit[b]) {
      return 0;
    }
    if (df->uses_epilogue_delay_at_exit[b]) {
      return (df->live_at_exit & ~df->epilogue_delay.defs) | df->epilogue_delay.uses;
    }
    return df->live_at_exit;
  };
  auto maybe_rewrite_mulsi3_internal = [&](RtlPatternAndNotes* pn, ElemSummary* elem,
                                           RegMask live_after) -> bool {
    bool is_unsigned = false;
    if (!has_smulsi3_highpart || !IsDisassembledMips1HighpartMul(pn->pattern, &is_unsigned) ||
        (is_unsigned && df->gp64) || (live_after & (RegMask{1} << 64)) != 0 ||
        (live_after & (RegMask{1} << 65)) == 0) {
      return false;
    }
    const RtlExpr& lsh = pn->pattern.children[0].SetSrc().children[0];
    RtlExpr op0 = lsh.children[0].children[0].children[0];
    RtlExpr op1 = lsh.children[0].children[1].children[0];
    if (is_unsigned) {
      pn->pattern = RtlExpr::MakeParallel({
          RtlExpr::MakeSet(
              MakeGccHardReg(RtlMachineMode::kDi, 64),
              RtlExpr::MakeBinary(
                  RtlOp::kMult, RtlMachineMode::kDi,
                  RtlExpr::MakeUnary(RtlOp::kZeroExtend, RtlMachineMode::kDi, std::move(op0)),
                  RtlExpr::MakeUnary(RtlOp::kZeroExtend, RtlMachineMode::kDi, std::move(op1)))),
          RtlExpr::MakeClobber(MakeGccHardReg(RtlMachineMode::kSi, 66)),
      });
    } else {
      pn->pattern = RtlExpr::MakeParallel({
          RtlExpr::MakeSet(MakeGccHardReg(RtlMachineMode::kSi, 65),
                           RtlExpr::MakeBinary(RtlOp::kMult, RtlMachineMode::kSi, std::move(op0),
                                               std::move(op1))),
          RtlExpr::MakeClobber(MakeGccHardReg(RtlMachineMode::kSi, 64)),
          RtlExpr::MakeClobber(MakeGccHardReg(RtlMachineMode::kSi, 66)),
      });
    }
    pn->reg_notes.erase(std::remove_if(pn->reg_notes.begin(), pn->reg_notes.end(),
                                       [](const RtlRegNote& n) {
                                         return n.kind == RtlMachineMode::kRegUnused &&
                                                n.datum.IsReg() &&
                                                (n.datum.RegNo() == 64 || n.datum.RegNo() == 65 ||
                                                 n.datum.RegNo() == 66);
                                       }),
                        pn->reg_notes.end());
    RegMask dummy_args = 0;
    const absl::flat_hash_set<std::string> empty_jt_labels;
    *elem = SummarizeSingleElem(*pn, /*force_call=*/false, df->gp64, df->fp64,
                                IsUnoptimizedProfile(&profile), empty_jt_labels, &dummy_args);
    return true;
  };

  constexpr RegMask kOutgoingCallArgMask = RegMask{0xF0} | (RegMask{0xF} << 44);
  auto step_pinned_lo_copy = [](const RtlPatternAndNotes& pn, const ElemSummary& es,
                                RegMask* pinned_need) {
    bool copies_lo_to_pinned = false;
    if (pn.pattern.IsSet() && pn.pattern.SetDest().IsHardReg() &&
        !pn.pattern.SetDest().IsArgPointerReg() && pn.pattern.SetSrc().IsHardReg() &&
        pn.pattern.SetSrc().RegNo() == 65) {
      const int dr = pn.pattern.SetDest().RegNo();
      if (dr >= 0 && dr < 64 &&
          (pn.pattern.SetDest().flags.integrated || ((*pinned_need & (RegMask{1} << dr)) != 0))) {
        copies_lo_to_pinned = true;
      }
    }
    *pinned_need &= ~es.defs;
    if (copies_lo_to_pinned) {
      *pinned_need |= (RegMask{1} << 65);
    }
  };
  std::vector<std::vector<size_t>> preds(n_blocks);
  for (size_t b = 0; b < n_blocks; ++b) {
    ForEachStage0CfgSucc(cfg->rpo_blocks[b], n_blocks, [&](size_t succ) {
      if (std::find(preds[succ].begin(), preds[succ].end(), b) == preds[succ].end()) {
        preds[succ].push_back(b);
      }
    });
  }
  for (size_t b = 0; b < n_blocks; ++b) {
    bool block_changed = false;
    RegMask cur_live = df->live_out[b];
    RegMask cur_ret_need = ret_need_out[b];
    RegMask cur_call_arg_need = 0;
    // In a conditional-branch early-return block (`b -> {exit_b, other_b}` where
    // `cur_ret_need` is dead on `other_b` and not read by the branch itself),
    // any non-`use` read of `ret_hr` inside `b` (e.g. `c.eq.s $f6, $f0`) proves
    // GCC computed the value into a non-`/i` pseudo and emitted a separate
    // `(set (reg/i:M ret_hr) P)` copy that `sched2` (`sched.c:4813-4818`) deleted.
    bool is_early_return_cond_block = false;
    if (attach_notes && cur_ret_need != 0 && b + 1 < n_blocks &&
        cfg->rpo_blocks[b].successors.size() == 2) {
      const size_t exit_b = n_blocks - 1;
      if (df->can_exit[exit_b] && cfg->rpo_blocks[exit_b].successors.empty() &&
          cfg->rpo_blocks[exit_b].addr_vec_targets.empty()) {
        const size_t s0 = static_cast<size_t>(cfg->rpo_blocks[b].successors[0]);
        const size_t s1 = static_cast<size_t>(cfg->rpo_blocks[b].successors[1]);
        const size_t other_b = (s0 == exit_b) ? s1 : (s1 == exit_b ? s0 : n_blocks);
        if (other_b < n_blocks && other_b != exit_b && (df->live_in[other_b] & cur_ret_need) == 0) {
          bool jump_reads_ret = false;
          for (auto it = df->block_summaries[b].rbegin(); it != df->block_summaries[b].rend();
               ++it) {
            if (!it->valid) {
              continue;
            }
            const RtlExpr& jpat = it->is_sequence ? it->head_pn.pattern : it->top_pn.pattern;
            if (IsConditionalJumpPattern(jpat)) {
              const RegMask juses = it->is_sequence ? it->head.uses : it->single.uses;
              jump_reads_ret = (juses & cur_ret_need) != 0;
            }
            break;
          }
          is_early_return_cond_block = !jump_reads_ret;
        }
      }
    }

    for (size_t rev_i = df->block_summaries[b].size(); rev_i > 0; --rev_i) {
      const size_t i = rev_i - 1;
      InsnSummary& isum = df->block_summaries[b][i];
      if (!isum.valid) {
        continue;
      }
      bool insn_changed = false;
      if (isum.is_sequence) {
        if (isum.is_call_seq) {
          if (MarkExitReachingReturnSetsInExpr(&isum.head_pn.pattern, cur_ret_need, df->gp64,
                                               df->fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.head.defs;
          cur_call_arg_need = isum.head.uses & kOutgoingCallArgMask;
          if (MarkExitReachingReturnSetsInExpr(&isum.slot_pn.pattern, cur_ret_need, df->gp64,
                                               df->fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.slot.defs;

          const RegMask before_call = (cur_live & ~isum.head.defs) | isum.head.uses;
          if (maybe_rewrite_mulsi3_internal(&isum.slot_pn, &isum.slot, before_call)) {
            insn_changed = true;
          }
          if (attach_notes &&
              AttachNotesToElem(&isum.slot_pn, isum.slot, before_call, fixed_mask, df->gp64,
                                df->fp64, /*in_delay_slot=*/true, cur_call_arg_need)) {
            insn_changed = true;
          }
          step_pinned_lo_copy(isum.slot_pn, isum.slot, &cur_call_arg_need);
          cur_live = (before_call & ~isum.slot.defs) | isum.slot.uses;
        } else {
          cur_call_arg_need = 0;
          RegMask slot_after_live = cur_live;
          RegMask before_slot = (cur_live & ~isum.slot.defs) | isum.slot.uses;
          RegMask slot_ret_need = cur_ret_need;
          bool is_ft_stolen_slot = false;
          if (isum.seq_slot_wrap.flags.in_struct && has_fallthrough_edge(b)) {
            RegMask taken_live = exit_live_for_block(b);
            RegMask taken_ret_need = df->can_exit[b] ? exit_ret_mask : 0;
            for (size_t sb : cfg->rpo_blocks[b].successors) {
              if ((sb != b + 1 || taken_also_goes_to_ft(b)) && sb < n_blocks) {
                taken_live |= df->live_in[sb];
                taken_ret_need |= ret_need_in[sb];
              }
            }
            slot_after_live = taken_live;
            slot_ret_need = taken_ret_need;
            if (isum.is_annulled_target_seq) {
              before_slot = ((taken_live & ~isum.slot.defs) | isum.slot.uses) | df->live_in[b + 1];
              cur_ret_need = (taken_ret_need & ~isum.slot.defs) | ret_need_in[b + 1];
            } else {
              cur_ret_need &= ~isum.slot.defs;
            }
          } else {
            if (!isum.seq_slot_wrap.flags.in_struct && has_fallthrough_edge(b)) {
              RegMask taken_live = exit_live_for_block(b);
              for (size_t sb : cfg->rpo_blocks[b].successors) {
                if ((sb != b + 1 || taken_also_goes_to_ft(b)) && sb < n_blocks) {
                  taken_live |= df->live_in[sb];
                }
              }
              if (IsFallthroughStolenCondBranchDelaySlot(*cfg, *df, preds, b, isum, taken_live)) {
                is_ft_stolen_slot = true;
                slot_after_live = df->live_in[b + 1];
              }
            }
            cur_ret_need &= ~isum.slot.defs;
          }

          if (maybe_rewrite_mulsi3_internal(&isum.slot_pn, &isum.slot, slot_after_live)) {
            insn_changed = true;
          }
          if (MarkExitReachingReturnSetsInExpr(&isum.slot_pn.pattern, slot_ret_need, df->gp64,
                                               df->fp64)) {
            insn_changed = true;
          }
          if (MarkExitReachingReturnSetsInExpr(&isum.head_pn.pattern, cur_ret_need, df->gp64,
                                               df->fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.head.defs;

          if (attach_notes) {
            const RegMask shared_head_slot_dead =
                (!isum.seq_slot_wrap.flags.in_struct && !is_ft_stolen_slot)
                    ? (isum.head.uses & isum.slot.uses & ~isum.head.defs & ~isum.slot.defs &
                       ~slot_after_live & ~fixed_mask)
                    : RegMask{0};
            if (AttachNotesToElem(&isum.slot_pn, isum.slot, slot_after_live | shared_head_slot_dead,
                                  fixed_mask, df->gp64, df->fp64, /*in_delay_slot=*/true,
                                  cur_call_arg_need)) {
              insn_changed = true;
            }
            step_pinned_lo_copy(isum.slot_pn, isum.slot, &cur_call_arg_need);
            if (AttachNotesToElem(&isum.head_pn, isum.head, before_slot & ~shared_head_slot_dead,
                                  fixed_mask, df->gp64, df->fp64, /*in_delay_slot=*/false,
                                  cur_call_arg_need)) {
              insn_changed = true;
            }
            step_pinned_lo_copy(isum.head_pn, isum.head, &cur_call_arg_need);
          } else {
            step_pinned_lo_copy(isum.slot_pn, isum.slot, &cur_call_arg_need);
            step_pinned_lo_copy(isum.head_pn, isum.head, &cur_call_arg_need);
          }
          cur_live = (before_slot & ~isum.head.defs) | isum.head.uses;
        }
        if (insn_changed) {
          RtlPatternAndNotes seq_head_pn = isum.head_pn;
          if (isum.seq_head_wrap.op == RtlOp::kCallInsn) {
            seq_head_pn.reg_notes.clear();
            seq_head_pn.has_explicit_call_fusage = false;
            seq_head_pn.call_fusage.clear();
          }
          cfg->rpo_blocks[b].instructions[i] =
              RtlExpr::MakeSequence(
                  {
                      RtlExpr::MakeInsnWrapper(isum.seq_head_wrap.op, isum.seq_head_wrap.flags,
                                               seq_head_pn),
                      RtlExpr::MakeInsnWrapper(isum.seq_slot_wrap.op, isum.seq_slot_wrap.flags,
                                               isum.slot_pn),
                  })
                  .ToGccPrintRtlString();
          block_changed = true;
        }
      } else {
        if (maybe_rewrite_mulsi3_internal(&isum.top_pn, &isum.single, cur_live)) {
          insn_changed = true;
        }
        if (MarkExitReachingReturnSetsInExpr(&isum.top_pn.pattern, cur_ret_need, df->gp64,
                                             df->fp64)) {
          insn_changed = true;
        }
        cur_ret_need &= ~isum.single.defs;
        if (is_early_return_cond_block && isum.top_pn.pattern.op != RtlOp::kUse) {
          cur_ret_need &= ~isum.single.uses;
        }

        if (IsAtClobberFixParallel(isum.top_pn.pattern)) {
          static constexpr int kCallUsedGprOrder[] = {2,  3,  4,  5,  6,  7,  8,  9,
                                                      10, 11, 12, 13, 14, 15, 24, 25};
          static constexpr int kSavedGprOrder[] = {16, 17, 18, 19, 20, 21, 22, 23, 30};
          const RegMask busy = cur_live | isum.single.defs | isum.single.uses | fixed_mask;
          int chosen_gpr = -1;
          if (IsUnoptimizedProfile(&profile)) {
            for (int r : kCallUsedGprOrder) {
              if (((busy | global_explicit_reg_mask) & (RegMask{1} << r)) == 0) {
                chosen_gpr = r;
                break;
              }
            }
          }
          if (chosen_gpr < 0) {
            for (int r : kCallUsedGprOrder) {
              if ((busy & (RegMask{1} << r)) == 0) {
                chosen_gpr = r;
                break;
              }
            }
          }
          if (chosen_gpr < 0) {
            for (int r : kSavedGprOrder) {
              if ((busy & (RegMask{1} << r)) == 0 &&
                  std::find(cfg->sideband.saved_gpr_regnos.begin(),
                            cfg->sideband.saved_gpr_regnos.end(),
                            r) != cfg->sideband.saved_gpr_regnos.end()) {
                chosen_gpr = r;
                break;
              }
            }
          }
          if (chosen_gpr >= 0) {
            const RtlMachineMode scratch_mode = isum.top_pn.pattern.children[1].children[0].mode;
            isum.top_pn.pattern.children[1].children[0] = MakeGccHardReg(scratch_mode, chosen_gpr);
            isum.top_pn.reg_notes.erase(
                std::remove_if(isum.top_pn.reg_notes.begin(), isum.top_pn.reg_notes.end(),
                               [](const RtlRegNote& n) {
                                 return n.kind == RtlMachineMode::kRegUnused && n.datum.IsReg() &&
                                        n.datum.RegNo() == 1;
                               }),
                isum.top_pn.reg_notes.end());
            RegMask dummy_args = 0;
            const absl::flat_hash_set<std::string> empty_jt_labels;
            isum.single =
                SummarizeSingleElem(isum.top_pn, /*force_call=*/false, df->gp64, df->fp64,
                                    IsUnoptimizedProfile(&profile), empty_jt_labels, &dummy_args);
            insn_changed = true;
          }
        }

        const bool force_rev =
            attach_notes &&
            IsDemotedParallelHardSetDest(*df, profile, b, i, isum.top_pn, isum.single, cur_live);
        if (attach_notes &&
            AttachNotesToElem(&isum.top_pn, isum.single, cur_live, fixed_mask, df->gp64, df->fp64,
                              /*in_delay_slot=*/false, cur_call_arg_need, force_rev)) {
          insn_changed = true;
        }
        if (isum.single.is_call) {
          cur_call_arg_need = isum.single.uses & kOutgoingCallArgMask;
        } else {
          step_pinned_lo_copy(isum.top_pn, isum.single, &cur_call_arg_need);
        }
        cur_live = (cur_live & ~isum.single.defs) | isum.single.uses;
        if (insn_changed) {
          cfg->rpo_blocks[b].instructions[i] = FormatInsnWithoutCallFusage(isum.top_pn);
          block_changed = true;
        }
      }
    }
    if (block_changed) {
      cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
      for (size_t k = 0;
           k < df->block_summaries[b].size() && k < cfg->rpo_blocks[b].rtl_insns.size(); ++k) {
        const InsnSummary& ks = df->block_summaries[b][k];
        const RtlPatternAndNotes& src_pn = ks.is_sequence ? ks.head_pn : ks.top_pn;
        if (src_pn.has_explicit_call_fusage &&
            !cfg->rpo_blocks[b].rtl_insns[k].has_explicit_call_fusage) {
          cfg->rpo_blocks[b].rtl_insns[k].has_explicit_call_fusage = true;
          cfg->rpo_blocks[b].rtl_insns[k].call_fusage = src_pn.call_fusage;
        }
      }
    }
  }
}

void RecoverUnconditionalTargetStealInStructFlags(RtlControlFlowGraph* cfg, DataflowResult* df_ptr,
                                                  const std::vector<std::vector<size_t>>& preds) {
  DataflowResult& df = *df_ptr;
  const size_t n_blocks = cfg->rpo_blocks.size();
  auto has_fallthrough_edge = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg->rpo_blocks[b].successors;
    return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
  };
  auto expr_has_mem_or_volatile = [](const RtlExpr& expr) {
    bool bad = false;
    expr.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kMem || n.op == RtlOp::kUnknown || n.flags.volatil) {
        bad = true;
      }
    });
    return bad;
  };
  auto is_simple_uncond_jump_pat = [](const RtlExpr& pat) -> bool {
    return pat.IsSet() && pat.SetDest().op == RtlOp::kPc && pat.SetSrc().op == RtlOp::kLabelRef;
  };
  auto is_mostly_false_forward_cond_jump = [](const RtlExpr& pat) -> bool {
    if (!pat.IsSet() || pat.SetDest().op != RtlOp::kPc || pat.SetSrc().op != RtlOp::kIfThenElse ||
        pat.SetSrc().children.size() != 3) {
      return false;
    }
    const RtlExpr& cond = pat.SetSrc().children[0];
    const RtlExpr& arm1 = pat.SetSrc().children[1];
    const RtlExpr& arm2 = pat.SetSrc().children[2];
    const bool pc_first = (arm1.op == RtlOp::kPc && arm2.op == RtlOp::kLabelRef);
    const bool lbl_first = (arm1.op == RtlOp::kLabelRef && arm2.op == RtlOp::kPc);
    if (!pc_first && !lbl_first) {
      return false;
    }
    if ((lbl_first && cond.op == RtlOp::kEq) || (pc_first && cond.op == RtlOp::kNe)) {
      return true;
    }
    if (lbl_first && cond.children.size() == 2 && cond.children[1].IsConstInt() &&
        cond.children[1].IntVal() == 0 &&
        (cond.op == RtlOp::kLt || cond.op == RtlOp::kLe || cond.op == RtlOp::kLtu ||
         cond.op == RtlOp::kLeu)) {
      return true;
    }
    return false;
  };

  struct UncondTargetStealWitness {
    size_t target_b;
    RtlExpr slot_pat;
    RegMask slot_uses;
  };
  std::vector<UncondTargetStealWitness> uncond_witnesses;
  auto mark_uncond_seq_in_struct = [&](size_t u_idx) {
    InsnSummary& u_sum = df.block_summaries[u_idx].back();
    if (u_sum.seq_slot_wrap.flags.in_struct) {
      return;
    }
    u_sum.seq_slot_wrap.flags.in_struct = true;
    cfg->rpo_blocks[u_idx].instructions.back() =
        RtlExpr::MakeSequence(
            {
                RtlExpr::MakeInsnWrapper(u_sum.seq_head_wrap.op, u_sum.seq_head_wrap.flags,
                                         u_sum.head_pn),
                RtlExpr::MakeInsnWrapper(u_sum.seq_slot_wrap.op, u_sum.seq_slot_wrap.flags,
                                         u_sum.slot_pn),
            })
            .ToGccPrintRtlString();
    cfg->rpo_blocks[u_idx].SyncRtlInsnsFromInstructions();
  };

  for (size_t u = 1; u < n_blocks; ++u) {
    if (cfg->rpo_blocks[u].has_label || preds[u].size() != 1 || preds[u][0] != u - 1 ||
        cfg->rpo_blocks[u].successors.size() != 1 || !cfg->rpo_blocks[u].addr_vec_targets.empty() ||
        df.block_summaries[u].size() != 1) {
      continue;
    }
    InsnSummary& u_last = df.block_summaries[u].back();
    if (!u_last.valid || !u_last.is_sequence || u_last.is_call_seq ||
        u_last.seq_slot_wrap.flags.in_struct ||
        !is_simple_uncond_jump_pat(u_last.head_pn.pattern)) {
      continue;
    }
    const RegMask u_slot_defs = u_last.slot.explicit_sets;
    if (u_slot_defs == 0 || expr_has_mem_or_volatile(u_last.slot_pn.pattern)) {
      continue;
    }
    const size_t b = u - 1;
    const auto& b_succs = cfg->rpo_blocks[b].successors;
    if (b_succs.size() != 2 || !has_fallthrough_edge(b) || df.block_summaries[b].empty()) {
      continue;
    }
    const size_t b_target = (b_succs[0] == u) ? b_succs[1] : b_succs[0];
    if (b_target <= u + 1 || b_target >= n_blocks) {
      continue;
    }
    const InsnSummary& b_last = df.block_summaries[b].back();
    if (!b_last.valid || !b_last.is_sequence || b_last.is_call_seq ||
        b_last.seq_head_wrap.flags.unchanging || !b_last.seq_slot_wrap.flags.in_struct ||
        b_last.slot_pn.pattern == u_last.slot_pn.pattern ||
        !is_mostly_false_forward_cond_jump(b_last.head_pn.pattern)) {
      continue;
    }
    const RegMask live_at_b_orig_target =
        (df.live_in[b_target] & ~b_last.slot.explicit_sets) | b_last.slot.uses;
    if ((live_at_b_orig_target & u_slot_defs) != 0) {
      continue;
    }
    mark_uncond_seq_in_struct(u);
    uncond_witnesses.push_back(UncondTargetStealWitness{
        .target_b = cfg->rpo_blocks[u].successors[0],
        .slot_pat = u_last.slot_pn.pattern,
        .slot_uses = u_last.slot.uses,
    });
  }
  if (!uncond_witnesses.empty()) {
    for (size_t u2 = 0; u2 < n_blocks; ++u2) {
      if (cfg->rpo_blocks[u2].successors.size() != 1 ||
          !cfg->rpo_blocks[u2].addr_vec_targets.empty() || df.block_summaries[u2].empty()) {
        continue;
      }
      InsnSummary& u2_last = df.block_summaries[u2].back();
      if (!u2_last.valid || !u2_last.is_sequence || u2_last.is_call_seq ||
          u2_last.seq_slot_wrap.flags.in_struct ||
          !is_simple_uncond_jump_pat(u2_last.head_pn.pattern)) {
        continue;
      }
      const size_t u2_target = cfg->rpo_blocks[u2].successors[0];
      for (const auto& w : uncond_witnesses) {
        if (w.target_b != u2_target || !(w.slot_pat == u2_last.slot_pn.pattern)) {
          continue;
        }
        bool prior_defs_slot_use = false;
        for (size_t k = 0; k + 1 < df.block_summaries[u2].size(); ++k) {
          const InsnSummary& ks = df.block_summaries[u2][k];
          if (!ks.valid) {
            continue;
          }
          const RegMask kdefs = ks.is_sequence ? (ks.head.defs | ks.slot.defs) : ks.single.defs;
          if ((kdefs & w.slot_uses) != 0) {
            prior_defs_slot_use = true;
            break;
          }
        }
        if (!prior_defs_slot_use) {
          mark_uncond_seq_in_struct(u2);
        }
        break;
      }
    }
  }

  for (size_t target_b = 0; target_b < n_blocks; ++target_b) {
    if (preds[target_b].size() < 2) {
      continue;
    }
    const RtlExpr* shared_slot = nullptr;
    std::vector<size_t> uncond_cands;
    bool all_preds_match = true;
    for (size_t q : preds[target_b]) {
      if (df.block_summaries[q].empty()) {
        all_preds_match = false;
        break;
      }
      const InsnSummary* q_last_active = nullptr;
      for (size_t rev = df.block_summaries[q].size(); rev > 0; --rev) {
        const InsnSummary& qs = df.block_summaries[q][rev - 1];
        if (qs.valid && (qs.is_sequence || !qs.single.is_note_or_pseudo)) {
          q_last_active = &qs;
          break;
        }
      }
      if (q_last_active == nullptr) {
        all_preds_match = false;
        break;
      }
      if (q_last_active->is_sequence && !q_last_active->is_call_seq &&
          q_last_active->seq_slot_wrap.flags.in_struct && q_last_active->slot.explicit_sets != 0 &&
          !expr_has_mem_or_volatile(q_last_active->slot_pn.pattern)) {
        if (shared_slot == nullptr) {
          shared_slot = &q_last_active->slot_pn.pattern;
        } else if (!(*shared_slot == q_last_active->slot_pn.pattern)) {
          all_preds_match = false;
          break;
        }
      } else if (q + 1 == target_b && has_fallthrough_edge(q) && !q_last_active->is_sequence) {
        if (shared_slot != nullptr && !(*shared_slot == q_last_active->top_pn.pattern)) {
          all_preds_match = false;
          break;
        }
      } else if (q_last_active->is_sequence && !q_last_active->is_call_seq &&
                 !q_last_active->seq_slot_wrap.flags.in_struct &&
                 cfg->rpo_blocks[q].successors.size() == 1 &&
                 cfg->rpo_blocks[q].successors[0] == target_b &&
                 is_simple_uncond_jump_pat(q_last_active->head_pn.pattern) &&
                 q_last_active->slot.explicit_sets != 0 &&
                 !expr_has_mem_or_volatile(q_last_active->slot_pn.pattern)) {
        uncond_cands.push_back(q);
      } else {
        all_preds_match = false;
        break;
      }
    }
    if (!all_preds_match || shared_slot == nullptr || uncond_cands.empty()) {
      continue;
    }
    for (size_t q : preds[target_b]) {
      const InsnSummary* q_last_active = nullptr;
      for (size_t rev = df.block_summaries[q].size(); rev > 0; --rev) {
        const InsnSummary& qs = df.block_summaries[q][rev - 1];
        if (qs.valid && (qs.is_sequence || !qs.single.is_note_or_pseudo)) {
          q_last_active = &qs;
          break;
        }
      }
      const RtlExpr& q_pat = q_last_active->is_sequence ? q_last_active->slot_pn.pattern
                                                        : q_last_active->top_pn.pattern;
      if (!(*shared_slot == q_pat)) {
        all_preds_match = false;
        break;
      }
    }
    if (all_preds_match) {
      for (size_t u_q : uncond_cands) {
        mark_uncond_seq_in_struct(u_q);
      }
    }
  }
}

}  // namespace

void ReconstructPostDbrHardRegLivenessAndReturnUse(RtlControlFlowGraph* cfg,
                                                   const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }

  DataflowResult df = RunHardRegDataflow(*cfg, &profile);
  const bool attach_notes = (profile.optimize_level > 0);
  if (attach_notes && MaybeInsertDeadLoopCombineUse(cfg, df, profile)) {
    df = RunHardRegDataflow(*cfg, &profile);
  }
  if (attach_notes) {
    WriteLiveInToSideband(df, cfg);
  } else {
    cfg->sideband.basic_block_live_at_start.clear();
  }

  const RegMask exit_ret_mask = df.live_at_exit & kRetCandMask;
  const size_t n_blocks = cfg->rpo_blocks.size();

  auto apply_call_fusage_to_blocks = [&]() { ApplyMissingCallFusageFromDataflow(df, cfg); };

  auto has_fallthrough_edge = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg->rpo_blocks[b].successors;
    return std::find(succs.begin(), succs.end(), b + 1) != succs.end();
  };
  auto taken_also_goes_to_ft = [&](size_t b) -> bool {
    if (b + 1 >= n_blocks) {
      return false;
    }
    const auto& succs = cfg->rpo_blocks[b].successors;
    return !succs.empty() &&
           std::all_of(succs.begin(), succs.end(), [b](size_t s) { return s == b + 1; });
  };

  // Backward propagation of exit-reaching return register values (`exit_ret_mask`)
  // so only genuine return-value `SET`s receive `REG_FUNCTION_VALUE_P` (`reg/i`).
  std::vector<RegMask> ret_need_in(n_blocks, 0);
  std::vector<RegMask> ret_need_out(n_blocks, 0);
  if (exit_ret_mask != 0) {
    if (df.has_epilogue_delay && cfg->sideband.epilogue_delay_pattern.has_value()) {
      auto ep_pn = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
      if (ep_pn.ok() &&
          MarkExitReachingReturnSetsInExpr(&ep_pn->pattern, exit_ret_mask, df.gp64, df.fp64)) {
        cfg->sideband.epilogue_delay_pattern = ep_pn->ToGccPrintRtlString();
      }
    }
    const RegMask suppressed_exit_ret_mask =
        attach_notes ? GetSuppressedSharedReturnExitMaskInPostDbr(*cfg, df) : RegMask{0};
    bool changed_ret = true;
    while (changed_ret) {
      changed_ret = false;
      for (size_t rev = n_blocks; rev > 0; --rev) {
        const size_t b = rev - 1;
        RegMask out_need = 0;
        if (df.can_exit[b]) {
          const RegMask blk_exit_ret =
              (b + 1 == n_blocks) ? (exit_ret_mask & ~suppressed_exit_ret_mask) : exit_ret_mask;
          out_need |= df.uses_epilogue_delay_at_exit[b] ? (blk_exit_ret & ~df.epilogue_delay.defs)
                                                        : blk_exit_ret;
        }
        for (size_t sb : cfg->rpo_blocks[b].successors) {
          if (sb < n_blocks) {
            out_need |= ret_need_in[sb];
          }
        }
        ret_need_out[b] = out_need;
        RegMask cur_need = out_need;
        for (auto it = df.block_summaries[b].rbegin(); it != df.block_summaries[b].rend(); ++it) {
          if (!it->valid) continue;
          if (it->is_sequence) {
            if (it == df.block_summaries[b].rbegin() && it->is_annulled_target_seq &&
                has_fallthrough_edge(b)) {
              RegMask taken_need = df.can_exit[b] ? exit_ret_mask : 0;
              for (size_t sb : cfg->rpo_blocks[b].successors) {
                if ((sb != b + 1 || taken_also_goes_to_ft(b)) && sb < n_blocks) {
                  taken_need |= ret_need_in[sb];
                }
              }
              cur_need = ((taken_need & ~it->slot.defs) | ret_need_in[b + 1]) & ~it->head.defs;
            } else {
              cur_need &= ~(it->slot.defs | it->head.defs);
            }
          } else {
            cur_need &= ~it->single.defs;
          }
        }
        if (cur_need != ret_need_in[b]) {
          ret_need_in[b] = cur_need;
          changed_ret = true;
        }
      }
    }
  }

  bool has_at_fix_clobber = false;
  bool has_smulsi3_highpart = false;
  for (size_t b = 0; b < n_blocks && !(has_at_fix_clobber && has_smulsi3_highpart); ++b) {
    for (const InsnSummary& isum : df.block_summaries[b]) {
      if (!isum.valid) continue;
      if (!isum.is_sequence) {
        if (IsAtClobberFixParallel(isum.top_pn.pattern)) {
          has_at_fix_clobber = true;
        }
        if (IsDisassembledMips1HighpartMul(isum.top_pn.pattern)) {
          has_smulsi3_highpart = true;
        }
      } else if (IsDisassembledMips1HighpartMul(isum.slot_pn.pattern)) {
        has_smulsi3_highpart = true;
      }
    }
  }

  if (exit_ret_mask == 0 && !attach_notes && !has_at_fix_clobber && !has_smulsi3_highpart) {
    apply_call_fusage_to_blocks();
    return;
  }

  const RegMask fixed_mask = (RegMask{1} << 0) | (RegMask{1} << 28) | (RegMask{1} << 29) |
                             (cfg->sideband.frame_pointer_needed ? (RegMask{1} << 30) : RegMask{0});

  auto exit_live_for_block = [&](size_t b) -> RegMask {
    if (!df.can_exit[b]) {
      return 0;
    }
    if (df.uses_epilogue_delay_at_exit[b]) {
      return (df.live_at_exit & ~df.epilogue_delay.defs) | df.epilogue_delay.uses;
    }
    return df.live_at_exit;
  };

  // Recover `INSN_FROM_TARGET_P` (`insn/s`, `flags.in_struct = true`) on
  // non-annulled conditional branch `SEQUENCE` delay slots whose `/s` flag is
  // not explicitly encoded in MIPS assembly (`invert_final_scan_insn` only sets
  // `in_struct` on annulled branch-likely `/u` delay slots).
  if (attach_notes) {
    std::vector<std::vector<size_t>> preds(n_blocks);
    for (size_t b = 0; b < n_blocks; ++b) {
      for (size_t sb : cfg->rpo_blocks[b].successors) {
        if (sb < n_blocks) {
          preds[sb].push_back(b);
        }
      }
    }
    auto expr_has_mem_or_volatile = [](const RtlExpr& expr) {
      bool bad = false;
      expr.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kMem || n.op == RtlOp::kUnknown || n.flags.volatil) {
          bad = true;
        }
      });
      return bad;
    };
    auto is_opposite_arith_self_pair = [](const RtlExpr& slot_pat, const RtlExpr& ft_pat) -> bool {
      if (!slot_pat.IsSet() || !slot_pat.SetDest().IsReg() || !ft_pat.IsSet() ||
          !ft_pat.SetDest().IsReg()) {
        return false;
      }
      const int dst_r = slot_pat.SetDest().RegNo();
      if (dst_r <= 0 || ft_pat.SetDest().RegNo() != dst_r) {
        return false;
      }
      const RtlExpr& s_src = slot_pat.SetSrc();
      const RtlExpr& f_src = ft_pat.SetSrc();
      if ((s_src.op != RtlOp::kPlus && s_src.op != RtlOp::kMinus) ||
          (f_src.op != RtlOp::kPlus && f_src.op != RtlOp::kMinus) || s_src.children.size() != 2 ||
          f_src.children.size() != 2 || !s_src.children[0].IsReg() ||
          s_src.children[0].RegNo() != dst_r || !f_src.children[0].IsReg() ||
          f_src.children[0].RegNo() != dst_r) {
        return false;
      }
      if (s_src.children[1].IsConstInt() && f_src.children[1].IsConstInt()) {
        const int64_t s_delta =
            (s_src.op == RtlOp::kMinus) ? -s_src.children[1].int_val : s_src.children[1].int_val;
        const int64_t f_delta =
            (f_src.op == RtlOp::kMinus) ? -f_src.children[1].int_val : f_src.children[1].int_val;
        return s_delta != 0 && f_delta == -s_delta;
      }
      if (s_src.children[1].IsReg() && f_src.children[1].IsReg()) {
        const int rhs_r = s_src.children[1].RegNo();
        return rhs_r > 0 && rhs_r != dst_r && f_src.children[1].RegNo() == rhs_r &&
               s_src.op != f_src.op;
      }
      return false;
    };
    auto is_simple_uncond_jump_pat = [](const RtlExpr& pat) -> bool {
      return pat.IsSet() && pat.SetDest().op == RtlOp::kPc && pat.SetSrc().op == RtlOp::kLabelRef;
    };
    auto is_mostly_false_forward_cond_jump = [](const RtlExpr& pat) -> bool {
      if (!pat.IsSet() || pat.SetDest().op != RtlOp::kPc || pat.SetSrc().op != RtlOp::kIfThenElse ||
          pat.SetSrc().children.size() != 3) {
        return false;
      }
      const RtlExpr& cond = pat.SetSrc().children[0];
      const RtlExpr& arm1 = pat.SetSrc().children[1];
      const RtlExpr& arm2 = pat.SetSrc().children[2];
      const bool pc_first = (arm1.op == RtlOp::kPc && arm2.op == RtlOp::kLabelRef);
      const bool lbl_first = (arm1.op == RtlOp::kLabelRef && arm2.op == RtlOp::kPc);
      if (!pc_first && !lbl_first) {
        return false;
      }
      if ((lbl_first && cond.op == RtlOp::kEq) || (pc_first && cond.op == RtlOp::kNe)) {
        return true;
      }
      if (lbl_first && cond.children.size() == 2 && cond.children[1].IsConstInt() &&
          cond.children[1].IntVal() == 0 &&
          (cond.op == RtlOp::kLt || cond.op == RtlOp::kLe || cond.op == RtlOp::kLtu ||
           cond.op == RtlOp::kLeu)) {
        return true;
      }
      return false;
    };
    auto find_last_real_active_summary = [&](size_t blk_idx) -> const InsnSummary* {
      const auto& sums = df.block_summaries[blk_idx];
      for (size_t rev = sums.size(); rev > 0; --rev) {
        const InsnSummary& s = sums[rev - 1];
        if (!s.valid || (!s.is_sequence && s.single.is_note_or_pseudo)) {
          continue;
        }
        return &s;
      }
      return nullptr;
    };
    auto inline_slot_blocked_pred_ft_steal = [&](size_t cur_b, RegMask s_defs) -> bool {
      if (cur_b < 2 || !cfg->rpo_blocks[cur_b].has_label) {
        return false;
      }
      const size_t q = cur_b - 2;
      const size_t u = cur_b - 1;
      const auto& q_succs = cfg->rpo_blocks[q].successors;
      if (q_succs.size() != 2 || !has_fallthrough_edge(q) || df.block_summaries[q].empty() ||
          ((q_succs[0] == u) ? q_succs[1] : q_succs[0]) != cur_b) {
        return false;
      }
      if (cfg->rpo_blocks[u].has_label || preds[u].size() != 1 || preds[u][0] != q ||
          cfg->rpo_blocks[u].successors.size() != 1 || df.block_summaries[u].size() != 1) {
        return false;
      }
      const InsnSummary& q_last = df.block_summaries[q].back();
      const InsnSummary& u_last = df.block_summaries[u].back();
      return q_last.valid && q_last.is_sequence && !q_last.is_call_seq &&
             !q_last.seq_head_wrap.flags.unchanging && q_last.seq_slot_wrap.flags.in_struct &&
             is_mostly_false_forward_cond_jump(q_last.head_pn.pattern) &&
             (q_last.slot.explicit_sets & s_defs) == 0 && u_last.valid && u_last.is_sequence &&
             !u_last.is_call_seq && is_simple_uncond_jump_pat(u_last.head_pn.pattern) &&
             (u_last.slot.explicit_sets & s_defs) == s_defs;
    };
    for (size_t b = 0; b + 1 < n_blocks; ++b) {
      const auto& succs = cfg->rpo_blocks[b].successors;
      if (succs.size() != 2 || !has_fallthrough_edge(b) || df.block_summaries[b].empty()) {
        continue;
      }
      const size_t ft_b = b + 1;
      const size_t target_b = (succs[0] == ft_b) ? succs[1] : succs[0];
      if (target_b == ft_b) {
        continue;
      }
      InsnSummary& isum = df.block_summaries[b].back();
      if (!isum.valid || !isum.is_sequence || isum.is_call_seq ||
          isum.seq_head_wrap.flags.unchanging || isum.seq_slot_wrap.flags.in_struct) {
        continue;
      }
      const RegMask slot_defs = isum.slot.explicit_sets;
      if (slot_defs == 0 || expr_has_mem_or_volatile(isum.slot_pn.pattern)) {
        continue;
      }
      const RegMask taken_live =
          (target_b < n_blocks) ? df.live_in[target_b] : exit_live_for_block(b);
      const RegMask ft_live = df.live_in[ft_b];
      bool mark_in_struct = false;

      // (I1) Branch condition reads a register defined by the delay slot, while
      // that register is dead on fallthrough and live on the taken target.
      if ((isum.head.uses & slot_defs) != 0 && (ft_live & slot_defs) == 0 &&
          (taken_live & slot_defs) != 0) {
        mark_in_struct = true;
      }

      // (I2) Opposite-arithmetic fallthrough compensation (`reorg.c:3526-3586`):
      // delay slot adjusts `D += X` / `D -= X` and unlabeled single-pred `ft_b`
      // immediately applies the opposite adjustment (either as its first standalone
      // instruction or in the non-annulled delay slot of a return/jump `SEQUENCE`).
      if (!mark_in_struct && !cfg->rpo_blocks[ft_b].has_label && preds[ft_b].size() == 1 &&
          preds[ft_b][0] == b) {
        for (const InsnSummary& ft_sum : df.block_summaries[ft_b]) {
          if (!ft_sum.valid || (!ft_sum.is_sequence && ft_sum.single.is_note_or_pseudo)) {
            continue;
          }
          if ((!ft_sum.is_sequence &&
               is_opposite_arith_self_pair(isum.slot_pn.pattern, ft_sum.top_pn.pattern)) ||
              (ft_sum.is_sequence && !ft_sum.is_call_seq &&
               !ft_sum.seq_head_wrap.flags.unchanging && (ft_sum.head.uses & slot_defs) == 0 &&
               is_opposite_arith_self_pair(isum.slot_pn.pattern, ft_sum.slot_pn.pattern))) {
            mark_in_struct = true;
          }
          break;
        }
      }

      // (I3) Delay slot is dead on fallthrough, live on taken target, and
      // structurally corroborated by another predecessor of `target_b`, a
      // merged first instruction in `ft_b`, or Category 3 conditional sequence
      // overwrite on `ft_b`.
      if (!mark_in_struct && (isum.head.uses & slot_defs) == 0 && (ft_live & slot_defs) == 0 &&
          (taken_live & slot_defs) != 0) {
        if (target_b < n_blocks) {
          const bool blocked_by_pred_ft = inline_slot_blocked_pred_ft_steal(b, slot_defs);
          for (size_t p : preds[target_b]) {
            if (p == b || df.block_summaries[p].empty()) {
              continue;
            }
            const InsnSummary& p_last = df.block_summaries[p].back();
            if (!p_last.valid) {
              continue;
            }
            if (p_last.is_sequence && !p_last.is_call_seq &&
                p_last.slot_pn.pattern == isum.slot_pn.pattern) {
              mark_in_struct = true;
              break;
            }
            if (p + 1 == target_b && !blocked_by_pred_ft) {
              const InsnSummary* cand_last = find_last_real_active_summary(p);
              if (cand_last == nullptr && p > 0 && p - 1 != b && preds[p].size() == 1 &&
                  preds[p][0] == p - 1 && has_fallthrough_edge(p - 1)) {
                cand_last = find_last_real_active_summary(p - 1);
              }
              if (cand_last != nullptr && !cand_last->is_sequence &&
                  cand_last->top_pn.pattern == isum.slot_pn.pattern) {
                mark_in_struct = true;
                break;
              }
            }
          }
        }
        if (!mark_in_struct) {
          for (const InsnSummary& ft_sum : df.block_summaries[ft_b]) {
            if (!ft_sum.valid || (!ft_sum.is_sequence && ft_sum.single.is_note_or_pseudo)) {
              continue;
            }
            if ((!ft_sum.is_sequence && ft_sum.top_pn.pattern == isum.slot_pn.pattern) ||
                (ft_sum.is_sequence && ft_sum.slot_pn.pattern == isum.slot_pn.pattern)) {
              mark_in_struct = true;
            }
            break;
          }
        }
        if (!mark_in_struct && target_b == b + 2 && !cfg->rpo_blocks[ft_b].has_label &&
            !df.block_summaries[ft_b].empty()) {
          bool prefix_clean = true;
          for (size_t fi = 0; fi + 1 < df.block_summaries[ft_b].size(); ++fi) {
            const InsnSummary& fs = df.block_summaries[ft_b][fi];
            if (!fs.valid || fs.is_sequence || fs.single.is_call ||
                ((fs.single.uses | fs.single.defs) & slot_defs) != 0) {
              prefix_clean = false;
              break;
            }
          }
          const InsnSummary& ft_last = df.block_summaries[ft_b].back();
          if (prefix_clean && ft_last.valid && ft_last.is_sequence && !ft_last.is_call_seq &&
              !ft_last.seq_head_wrap.flags.unchanging &&
              ((ft_last.head.uses | ft_last.slot.uses) & slot_defs) == 0 &&
              (ft_last.slot.explicit_sets & slot_defs) == slot_defs) {
            mark_in_struct = true;
          }
        }
        // Diamond join `target_b == b + 3` where unlabeled single-pred `ft_b`
        // (`b + 1`) falls through to `b + 2`, and `b + 2` starts with the
        // identical pattern before falling through to `target_b`.
        if (!mark_in_struct && target_b == b + 3 && target_b < n_blocks &&
            !cfg->rpo_blocks[ft_b].has_label && preds[ft_b].size() == 1 && preds[ft_b][0] == b &&
            cfg->rpo_blocks[ft_b].successors.size() == 1 &&
            cfg->rpo_blocks[ft_b].successors[0] == b + 2 && has_fallthrough_edge(b + 2)) {
          bool ft_reads_slot_defs = false;
          for (const InsnSummary& fs : df.block_summaries[ft_b]) {
            if (!fs.valid) {
              continue;
            }
            const RegMask fuses = fs.is_sequence ? (fs.head.uses | fs.slot.uses) : fs.single.uses;
            if ((fuses & slot_defs) != 0) {
              ft_reads_slot_defs = true;
              break;
            }
          }
          if (!ft_reads_slot_defs) {
            for (const InsnSummary& jsum : df.block_summaries[b + 2]) {
              if (!jsum.valid || (!jsum.is_sequence && jsum.single.is_note_or_pseudo)) {
                continue;
              }
              if ((!jsum.is_sequence && jsum.top_pn.pattern == isum.slot_pn.pattern) ||
                  (jsum.is_sequence && !jsum.is_call_seq &&
                   jsum.slot_pn.pattern == isum.slot_pn.pattern)) {
                mark_in_struct = true;
              }
              break;
            }
          }
        }
        // (I3c) Single-predecessor non-exit `target_b` (`preds[target_b] == {b}`):
        // when `slot_pn` is a non-copy, non-constant register ALU `SET`, `slot_defs`
        // is neither defined earlier in `b` nor in `ft_b` (or `slot_pn`'s first
        // source operand is a dying caller-saved GPR `src_r != dst_r`, which
        // `global_alloc` would have preferred if `slot_pn` had originated in `b`),
        // and `b` had no competing `fill_simple_delay_slots` candidate, `slot_pn`
        // was stolen from `target_b` via `steal_delay_list_from_target`.
        if (!mark_in_struct && target_b < n_blocks && preds[target_b].size() == 1 &&
            preds[target_b][0] == b && !cfg->rpo_blocks[target_b].successors.empty() &&
            isum.slot_pn.pattern.IsSet() && isum.slot_pn.pattern.SetDest().IsReg() &&
            !isum.slot_pn.pattern.SetSrc().IsConstInt() && !isum.slot_pn.pattern.SetSrc().IsReg() &&
            !BlockHasCompetingSimpleDelayCandidate(df.block_summaries[b])) {
          bool prior_or_ft_defs = false;
          for (size_t bi = 0; bi + 1 < df.block_summaries[b].size(); ++bi) {
            const InsnSummary& bs = df.block_summaries[b][bi];
            if (!bs.valid) {
              continue;
            }
            const RegMask bdefs = bs.is_sequence ? (bs.head.defs | bs.slot.defs) : bs.single.defs;
            if ((bdefs & slot_defs) != 0) {
              prior_or_ft_defs = true;
              break;
            }
            if (!bs.is_sequence && bs.top_pn.pattern.IsSet() &&
                bs.top_pn.pattern.SetDest().IsReg() && bs.top_pn.pattern.SetSrc().IsReg() &&
                (bs.single.defs & isum.head.uses) != 0 && (bs.single.uses & slot_defs) != 0) {
              bool overwritten_later = false;
              for (size_t bj = bi + 1; bj + 1 < df.block_summaries[b].size(); ++bj) {
                const InsnSummary& lsum = df.block_summaries[b][bj];
                if (!lsum.valid) {
                  continue;
                }
                const RegMask ldefs =
                    lsum.is_sequence ? (lsum.head.defs | lsum.slot.defs) : lsum.single.defs;
                if ((ldefs & bs.single.defs) != 0) {
                  overwritten_later = true;
                  break;
                }
              }
              if (!overwritten_later) {
                prior_or_ft_defs = true;
                break;
              }
            }
          }
          for (size_t fi = 0; !prior_or_ft_defs && fi < df.block_summaries[ft_b].size(); ++fi) {
            const InsnSummary& fs = df.block_summaries[ft_b][fi];
            if (!fs.valid) {
              continue;
            }
            const RegMask fdefs = fs.is_sequence ? (fs.head.defs | fs.slot.defs) : fs.single.defs;
            if ((fdefs & slot_defs) != 0) {
              prior_or_ft_defs = true;
              break;
            }
          }
          bool dying_mismatched_src_gpr = false;
          if (prior_or_ft_defs && (isum.slot.uses & slot_defs) == 0) {
            const int dst_r = isum.slot_pn.pattern.SetDest().RegNo();
            const RtlExpr& src = isum.slot_pn.pattern.SetSrc();
            if (!src.children.empty() && src.children[0].IsReg()) {
              const int src_r = src.children[0].RegNo();
              const RegMask src_bit = (src_r >= 2 && src_r <= 15) ? (RegMask{1} << src_r) : 0;
              if (src_bit != 0 && src_r != dst_r &&
                  ((isum.head.uses | taken_live | ft_live) & src_bit) == 0) {
                dying_mismatched_src_gpr = true;
              }
            }
          }
          if (!prior_or_ft_defs || dying_mismatched_src_gpr) {
            mark_in_struct = true;
          }
        }
      }

      // (I3b) Dominated `ft_b` `redundant_insn` skip / shared-target steal
      // (`reorg.c:1802, 3366`): `b` stole `target_b`'s first instruction
      // (`slot_defs`) without annulling while `slot_defs` is dead on `ft_b + 1`
      // and untouched across `b` (prior to `b`'s `SEQUENCE`) and unlabeled
      // single-predecessor `ft_b` (`b + 1`):
      //   - Case 1 (`ANNUL_IFFALSE_SLOTS`): `ft_b` skipped `slot_pn` via
      //     `redundant_insn` and annul-stole the next instruction `target_b[0]`
      //     (reading `slot_defs`) into an `in_struct` conditional `SEQUENCE`
      //     targeting `target_b + 1` (`preds[target_b] == {b}`).
      //   - Case 2 (`!ANNUL_IFFALSE_SLOTS`, e.g. `-mips1`): `ft_b` could not
      //     annul-steal `target_b[0]`, so `ft_b` still branches directly to
      //     `target_b` (`preds[target_b]` consists solely of `{b, ft_b}`) while
      //     `slot_pn` reads a dying caller-saved GPR `src_r != dst_r`.
      if (!mark_in_struct && (isum.head.uses & slot_defs) == 0 && (taken_live & slot_defs) != 0 &&
          target_b < n_blocks && ft_b + 1 < n_blocks && !cfg->rpo_blocks[ft_b].has_label &&
          preds[ft_b].size() == 1 && preds[ft_b][0] == b && has_fallthrough_edge(ft_b) &&
          (df.live_in[ft_b + 1] & slot_defs) == 0 && !df.block_summaries[ft_b].empty()) {
        bool ft_prefix_clean = true;
        for (size_t fi = 0; fi + 1 < df.block_summaries[ft_b].size(); ++fi) {
          const InsnSummary& fs = df.block_summaries[ft_b][fi];
          if (!fs.valid || fs.is_sequence || fs.single.is_call ||
              ((fs.single.uses | fs.single.defs) & slot_defs) != 0) {
            ft_prefix_clean = false;
            break;
          }
        }
        const InsnSummary& ft_last = df.block_summaries[ft_b].back();
        const auto& ft_succs = cfg->rpo_blocks[ft_b].successors;
        const size_t ft_target = (ft_succs.size() == 2)
                                     ? ((ft_succs[0] == ft_b + 1) ? ft_succs[1] : ft_succs[0])
                                     : n_blocks;
        if (ft_prefix_clean && target_b + 1 < n_blocks && preds[target_b].size() == 1 &&
            preds[target_b][0] == b && cfg->rpo_blocks[target_b].successors.size() == 1 &&
            cfg->rpo_blocks[target_b].successors[0] == target_b + 1 && ft_target == target_b + 1 &&
            ft_last.valid && ft_last.is_sequence && !ft_last.is_call_seq &&
            (ft_last.slot.uses & slot_defs) != 0 && (ft_last.head.uses & slot_defs) == 0) {
          RtlExpr tgt_sole_pat;
          int tgt_active = 0;
          for (const InsnSummary& ts : df.block_summaries[target_b]) {
            if (!ts.valid || (!ts.is_sequence && ts.single.is_note_or_pseudo)) {
              continue;
            }
            ++tgt_active;
            if (!ts.is_sequence) {
              tgt_sole_pat = ts.top_pn.pattern;
            }
          }
          if (tgt_active == 1 && tgt_sole_pat == ft_last.slot_pn.pattern) {
            mark_in_struct = true;
          }
        } else if (ft_prefix_clean && ft_target == target_b && preds[target_b].size() == 2 &&
                   ((preds[target_b][0] == b && preds[target_b][1] == ft_b) ||
                    (preds[target_b][0] == ft_b && preds[target_b][1] == b)) &&
                   ft_last.valid && !ft_last.is_call_seq && (isum.slot.uses & slot_defs) == 0 &&
                   isum.slot_pn.pattern.IsSet() && isum.slot_pn.pattern.SetDest().IsReg()) {
          const RegMask ft_last_touches =
              ft_last.is_sequence
                  ? (ft_last.head.uses | ft_last.head.defs | ft_last.slot.uses | ft_last.slot.defs)
                  : (ft_last.single.uses | ft_last.single.defs);
          bool b_prefix_touches = false;
          for (size_t bi = 0; bi + 1 < df.block_summaries[b].size(); ++bi) {
            const InsnSummary& bs = df.block_summaries[b][bi];
            if (bs.valid && !bs.is_sequence &&
                ((bs.single.uses | bs.single.defs) & slot_defs) != 0) {
              b_prefix_touches = true;
              break;
            }
          }
          if (!b_prefix_touches && (ft_last_touches & slot_defs) == 0) {
            const int dst_r = isum.slot_pn.pattern.SetDest().RegNo();
            const RtlExpr& src = isum.slot_pn.pattern.SetSrc();
            if (!src.children.empty() && src.children[0].IsReg()) {
              const int src_r = src.children[0].RegNo();
              const RegMask src_bit = (src_r >= 2 && src_r <= 15) ? (RegMask{1} << src_r) : 0;
              bool ft_touches_src = (ft_last_touches & src_bit) != 0;
              for (size_t fi = 0; !ft_touches_src && fi + 1 < df.block_summaries[ft_b].size();
                   ++fi) {
                const InsnSummary& fs = df.block_summaries[ft_b][fi];
                if (fs.valid && ((fs.single.uses | fs.single.defs) & src_bit) != 0) {
                  ft_touches_src = true;
                }
              }
              if (src_bit != 0 && src_r != dst_r && !ft_touches_src &&
                  ((isum.head.uses | taken_live | df.live_in[ft_b + 1]) & src_bit) == 0) {
                mark_in_struct = true;
              }
            }
          }
        }
      }

      if (mark_in_struct) {
        isum.seq_slot_wrap.flags.in_struct = true;
        RtlPatternAndNotes seq_head_pn = isum.head_pn;
        if (isum.seq_head_wrap.op == RtlOp::kCallInsn) {
          seq_head_pn.reg_notes.clear();
          seq_head_pn.has_explicit_call_fusage = false;
          seq_head_pn.call_fusage.clear();
        }
        cfg->rpo_blocks[b].instructions.back() =
            RtlExpr::MakeSequence(
                {
                    RtlExpr::MakeInsnWrapper(isum.seq_head_wrap.op, isum.seq_head_wrap.flags,
                                             seq_head_pn),
                    RtlExpr::MakeInsnWrapper(isum.seq_slot_wrap.op, isum.seq_slot_wrap.flags,
                                             isum.slot_pn),
                })
                .ToGccPrintRtlString();
        cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
        if (isum.head_pn.has_explicit_call_fusage && !cfg->rpo_blocks[b].rtl_insns.empty()) {
          cfg->rpo_blocks[b].rtl_insns.back().has_explicit_call_fusage = true;
          cfg->rpo_blocks[b].rtl_insns.back().call_fusage = isum.head_pn.call_fusage;
        }
      }
    }

    // (I4) / (I4b) Unconditional jump `SEQUENCE` target-steal recovery.
    RecoverUnconditionalTargetStealInStructFlags(cfg, &df, preds);
  }

  const RegMask global_explicit_reg_mask =
      ComputeGlobalExplicitRegMaskForAtFix(*cfg, df, exit_ret_mask, has_at_fix_clobber);
  AttachPostDbrBlockRegNotesAndFixupScratches(cfg, &df, profile, exit_ret_mask, fixed_mask,
                                              global_explicit_reg_mask, attach_notes,
                                              has_smulsi3_highpart, ret_need_in, ret_need_out);
  apply_call_fusage_to_blocks();
  InsertTrailingReturnValueUseIfNeeded(cfg, df, exit_ret_mask, attach_notes, profile);
  if (attach_notes && !IsUnoptimizedProfile(&profile)) {
    NormalizeStage0PostReloadRegNotes(cfg, df, profile);
  }
}

void RecomputePostReloadLiveAtStart(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty() ||
      static_cast<int>(cfg->pass) <= static_cast<int>(RtlPassKind::kGreg)) {
    return;
  }
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (flags.opt_level <= 0) {
    cfg->sideband.basic_block_live_at_start.clear();
    return;
  }
  bool has_pseudo = false;
  auto check_pseudo = [&has_pseudo](const RtlExpr& node) {
    if (node.IsReg() && node.RegNo() >= kMipsFirstPseudoRegister) {
      has_pseudo = true;
    }
  };
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    auto ep_parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (ep_parsed.ok()) {
      ep_parsed->pattern.Visit(check_pseudo);
    }
    if (has_pseudo) {
      return;
    }
  }
  for (const auto& blk : cfg->rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok()) {
        continue;
      }
      parsed->pattern.Visit(check_pseudo);
      if (has_pseudo) {
        return;
      }
    }
  }
  const DataflowResult df =
      RunHardRegDataflow(*cfg, &profile,
                         /*dbr_post_simple_markers=*/(cfg->pass == RtlPassKind::kDbr));
  WriteLiveInToSideband(df, cfg);
}

void RecomputePostReloadLiveAtStart(RtlControlFlowGraph* cfg) {
  const CompilationProfile default_profile{.optimize_level = 2};
  RecomputePostReloadLiveAtStart(cfg, default_profile);
}

void ResolvePostReloadConstZeroRegDeadNotes(RtlControlFlowGraph* cfg,
                                            const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (flags.opt_level <= 0) {
    return;
  }
  bool has_const_zero_dead = false;
  for (const auto& blk : cfg->rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (parsed.ok() && CountConstZeroRegDeadNotes(parsed->reg_notes) > 0) {
        has_const_zero_dead = true;
        break;
      }
    }
    if (has_const_zero_dead) {
      break;
    }
  }
  if (!has_const_zero_dead) {
    return;
  }

  const DataflowResult df = RunHardRegDataflow(*cfg, &profile);
  const RegMask fixed_mask = (RegMask{1} << 0) | (RegMask{1} << 28) | (RegMask{1} << 29) |
                             (cfg->sideband.frame_pointer_needed ? (RegMask{1} << 30) : RegMask{0});
  auto find_used_reg_expr = [](const RtlExpr& root, int target_regno,
                               RtlMachineMode fallback_mode) -> RtlExpr {
    const RtlExpr* found = nullptr;
    auto visit = [&](auto& self, const RtlExpr& e, bool is_set_lhs) -> void {
      if (found != nullptr) {
        return;
      }
      if (e.IsSet()) {
        self(self, e.SetSrc(), false);
        self(self, e.SetDest(), true);
        return;
      }
      if (e.op == RtlOp::kClobber) {
        return;
      }
      if (e.IsReg()) {
        if (!is_set_lhs && e.RegNo() == target_regno) {
          found = &e;
        }
        return;
      }
      for (const RtlExpr& child : e.children) {
        self(self, child, false);
      }
    };
    visit(visit, root, false);
    if (found != nullptr) {
      return *found;
    }
    return MakeGccHardReg(fallback_mode, target_regno);
  };

  absl::flat_hash_set<uint64_t> resolved_locs;
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    std::vector<InsnSummary> block_sums = df.block_summaries[b];
    std::vector<RegMask> live_after(block_sums.size(), 0);
    RegMask cur_live = df.live_out[b];
    bool block_changed = false;
    for (size_t rev_i = block_sums.size(); rev_i > 0; --rev_i) {
      const size_t i = rev_i - 1;
      live_after[i] = cur_live;
      InsnSummary& isum = block_sums[i];
      if (!isum.valid) {
        continue;
      }
      if (isum.is_sequence) {
        if (isum.is_call_seq) {
          const RegMask before_call = (cur_live & ~isum.head.defs) | isum.head.uses;
          cur_live = (before_call & ~isum.slot.defs) | isum.slot.uses;
        } else {
          const RegMask before_slot = (cur_live & ~isum.slot.defs) | isum.slot.uses;
          cur_live = (before_slot & ~isum.head.defs) | isum.head.uses;
        }
        continue;
      }
      const ElemSummary& esum = isum.single;
      const size_t num_const_zero = CountConstZeroRegDeadNotes(isum.top_pn.reg_notes);
      if (num_const_zero > 0) {
        std::vector<RtlExpr> candidates;
        std::vector<std::pair<int, int>> cand_proxy_regs;
        RegMask picked = 0;
        auto collect_tier = [&](RegMask cand_mask) {
          for (int r = 1; r < kMipsFirstPseudoRegister;) {
            if ((cand_mask & ~picked & (RegMask{1} << r)) != 0) {
              const RtlMachineMode mode = esum.use_modes[r];
              if (!HasRegNote(isum.top_pn.reg_notes, RtlMachineMode::kRegDead, r)) {
                candidates.push_back(find_used_reg_expr(isum.top_pn.pattern, r, mode));
              }
              const int nregs = HardRegNregs(r, mode, df.gp64, df.fp64);
              for (int k = 0; k < nregs && r + k < kMipsFirstPseudoRegister; ++k) {
                picked |= (RegMask{1} << (r + k));
              }
              r += nregs;
            } else {
              ++r;
            }
          }
        };

        // First resolve single-use cross-bank `"f"` FPR input reloads unpacked
        // from a branch delay slot (`(set fpr src_gpr) -> <fp_op(fpr)>`), where
        // `reload` left `REG_DEAD src_gpr` on `<fp_op(fpr)>` rather than `fpr`.
        const RtlExpr& ipat = isum.top_pn.pattern;
        const bool eligible_fp_cons =
            !esum.is_call && !IsAnyControlTransferPattern(ipat) && ipat.op != RtlOp::kUse &&
            ipat.op != RtlOp::kClobber &&
            !(ipat.IsSet() && (ipat.SetSrc().IsReg() || ipat.SetSrc().op == RtlOp::kMem));
        if (eligible_fp_cons && i > 0) {
          for (int fpr_r = 32; fpr_r < 64; ++fpr_r) {
            if ((esum.uses & ~esum.defs & ~cur_live & (RegMask{1} << fpr_r)) == 0) {
              continue;
            }
            const RtlMachineMode fmode = esum.use_modes[fpr_r];
            if (fmode != RtlMachineMode::kSf && fmode != RtlMachineMode::kDf) {
              continue;
            }
            const RegMask fpr_m = RegBitMask(fpr_r, fmode, df.gp64, df.fp64);
            if ((cur_live & fpr_m) != 0) {
              continue;
            }
            for (size_t prev = i; prev > 0; --prev) {
              const size_t pi = prev - 1;
              const InsnSummary& psum = block_sums[pi];
              if (!psum.valid || psum.is_sequence) {
                break;
              }
              const RtlExpr& ppat = psum.top_pn.pattern;
              if (ppat.IsSet() && ppat.SetDest().IsReg() && !ppat.SetDest().flags.integrated &&
                  ppat.SetDest().mode == fmode && ppat.SetDest().RegNo() == fpr_r &&
                  ppat.SetSrc().IsReg() && ppat.SetSrc().mode == fmode &&
                  ppat.SetSrc().RegNo() >= 2 && ppat.SetSrc().RegNo() < 32) {
                const int gpr_r = ppat.SetSrc().RegNo();
                const RegMask gpr_m = RegBitMask(gpr_r, fmode, df.gp64, df.fp64);
                bool gpr_intervening = false;
                for (size_t k = pi + 1; k < i; ++k) {
                  if (((block_sums[k].single.uses | block_sums[k].single.defs) & gpr_m) != 0) {
                    gpr_intervening = true;
                    break;
                  }
                }
                if (!gpr_intervening && (esum.defs & gpr_m) == 0 && (cur_live & gpr_m) == 0 &&
                    !HasRegNote(psum.top_pn.reg_notes, RtlMachineMode::kRegDead, gpr_r) &&
                    !HasRegNote(isum.top_pn.reg_notes, RtlMachineMode::kRegDead, gpr_r)) {
                  candidates.push_back(ppat.SetSrc());
                  cand_proxy_regs.emplace_back(gpr_r, fpr_r);
                  picked |= fpr_m | gpr_m;
                } else if (!gpr_intervening &&
                           !HasRegNote(psum.top_pn.reg_notes, RtlMachineMode::kRegDead, gpr_r)) {
                  // Cross-bank `"f"` input reload `(set (reg:SF/DF fpr_r) (reg:SF/DF gpr_r))`
                  // (`reload1.c:emit_reload_insns`) from a still-live `gpr_r`: `reload` never
                  // attaches `REG_DEAD fpr_r` on the FP consumer, so `fpr_r` must not steal a
                  // `(REG_DEAD (const_int 0))` slot from another operand (e.g. a self-RMW).
                  picked |= fpr_m;
                }
                break;
              }
              if (psum.single.is_call || IsAnyControlTransferPattern(ppat) ||
                  ((psum.single.uses | psum.single.defs) & fpr_m) != 0) {
                break;
              }
            }
          }
        }

        std::optional<RtlExpr> deferred_store_md_dead;
        if (i > 0 && ipat.IsSet() && !esum.is_call && !IsAnyControlTransferPattern(ipat)) {
          const RtlOp uop = ipat.SetSrc().op;
          const bool non_fp = ipat.SetDest().mode != RtlMachineMode::kSf &&
                              ipat.SetDest().mode != RtlMachineMode::kDf &&
                              ipat.SetSrc().mode != RtlMachineMode::kSf &&
                              ipat.SetSrc().mode != RtlMachineMode::kDf;
          const bool is_mem_store_md =
              (uop == RtlOp::kReg && non_fp && ipat.SetDest().op == RtlOp::kMem);
          const bool elig_md_cons =
              (uop == RtlOp::kFloat && ipat.SetDest().IsReg()) || is_mem_store_md ||
              (non_fp && uop != RtlOp::kReg && uop != RtlOp::kMem && uop != RtlOp::kDiv &&
               uop != RtlOp::kUdiv && uop != RtlOp::kMod && uop != RtlOp::kUmod &&
               uop != RtlOp::kMult && uop != RtlOp::kFloat && uop != RtlOp::kUnsignedFloat);
          if (elig_md_cons) {
            for (int gpr_r = 2; gpr_r <= 25; ++gpr_r) {
              const RegMask gpr_m = RegMask{1} << gpr_r;
              if ((esum.uses & ~esum.defs & ~cur_live & ~picked & gpr_m) == 0 ||
                  esum.use_modes[gpr_r] != RtlMachineMode::kSi) {
                continue;
              }
              for (size_t prev = i; prev > 0; --prev) {
                const size_t pi = prev - 1;
                const InsnSummary& psum = block_sums[pi];
                if (!psum.valid || psum.is_sequence) {
                  break;
                }
                const RtlExpr& ppat = psum.top_pn.pattern;
                if (ppat.IsSet() && ppat.SetDest().IsReg() && !ppat.SetDest().flags.integrated &&
                    ppat.SetDest().mode == RtlMachineMode::kSi && ppat.SetDest().RegNo() == gpr_r &&
                    ppat.SetSrc().IsReg() && ppat.SetSrc().mode == RtlMachineMode::kSi &&
                    (ppat.SetSrc().RegNo() == 64 || ppat.SetSrc().RegNo() == 65)) {
                  const int md_r = ppat.SetSrc().RegNo();
                  const RegMask md_m = RegMask{1} << md_r;
                  bool md_intervening = false;
                  for (size_t k = pi + 1; k < i; ++k) {
                    if (((block_sums[k].single.uses | block_sums[k].single.defs) &
                         (gpr_m | (RegMask{1} << 64) | (RegMask{1} << 65))) != 0) {
                      md_intervening = true;
                      break;
                    }
                  }
                  bool saw_md_prod = false;
                  size_t md_prod_idx = i;
                  for (size_t pre2 = pi; !md_intervening && pre2 > 0; --pre2) {
                    const size_t qi = pre2 - 1;
                    const InsnSummary& qsum = block_sums[qi];
                    if (!qsum.valid || qsum.is_sequence) {
                      break;
                    }
                    const RtlExpr& qpat = qsum.top_pn.pattern;
                    if (qpat.op == RtlOp::kParallel && qpat.children.size() == 3 &&
                        qpat.children[0].IsSet() && qpat.children[1].op == RtlOp::kClobber &&
                        qpat.children[2].op == RtlOp::kClobber &&
                        qpat.children[0].SetDest().IsReg() &&
                        qpat.children[0].SetDest().mode == RtlMachineMode::kSi &&
                        qpat.children[0].SetDest().RegNo() == md_r &&
                        ((md_r == 64 && qpat.children[0].SetSrc().op == RtlOp::kTruncate) ||
                         (md_r == 65 && qpat.children[0].SetSrc().op == RtlOp::kMult))) {
                      saw_md_prod = true;
                      md_prod_idx = qi;
                      break;
                    }
                    if (qsum.single.is_call || IsAnyControlTransferPattern(qpat) ||
                        ((qsum.single.uses | qsum.single.defs) & md_m) != 0) {
                      break;
                    }
                  }
                  bool other_md = false;
                  for (size_t m = 0; saw_md_prod && m < block_sums.size(); ++m) {
                    if (m == md_prod_idx || m == pi || !block_sums[m].valid) {
                      continue;
                    }
                    const RegMask m_touch =
                        block_sums[m].is_sequence
                            ? ((block_sums[m].is_call_seq
                                    ? RegMask{0}
                                    : (block_sums[m].head.uses | block_sums[m].head.defs)) |
                               block_sums[m].slot.uses | block_sums[m].slot.defs)
                            : (block_sums[m].single.is_call
                                   ? RegMask{0}
                                   : (block_sums[m].single.uses | block_sums[m].single.defs));
                    if ((m_touch & ((RegMask{1} << 64) | (RegMask{1} << 65))) != 0) {
                      other_md = true;
                      break;
                    }
                  }
                  if (saw_md_prod && !other_md && (esum.defs & md_m) == 0 &&
                      (cur_live & md_m) == 0 &&
                      !HasRegNote(psum.top_pn.reg_notes, RtlMachineMode::kRegDead, md_r) &&
                      !HasRegNote(isum.top_pn.reg_notes, RtlMachineMode::kRegDead, md_r)) {
                    if (is_mem_store_md) {
                      deferred_store_md_dead = MakeGccHardReg(RtlMachineMode::kSi, md_r);
                    } else {
                      candidates.push_back(MakeGccHardReg(RtlMachineMode::kSi, md_r));
                    }
                    cand_proxy_regs.emplace_back(md_r, gpr_r);
                    picked |= gpr_m | md_m;
                  }
                  break;
                }
                if (psum.single.is_call || IsAnyControlTransferPattern(ppat) ||
                    ((psum.single.uses | psum.single.defs) & gpr_m) != 0) {
                  break;
                }
              }
            }
          }
        }

        // Resolve 2-word uses (`DFmode`/`DImode`) whose low word is overwritten
        // by a 1-word destination and whose high word dies at instruction `i`.
        for (int r = 1; r + 1 < kMipsFirstPseudoRegister; ++r) {
          if ((esum.uses & ~picked & ~fixed_mask & (RegMask{1} << r)) == 0) {
            continue;
          }
          const RtlMachineMode mode = esum.use_modes[r];
          if (HardRegNregs(r, mode, df.gp64, df.fp64) != 2) {
            continue;
          }
          if ((esum.defs & (RegMask{1} << r)) != 0 &&
              ((esum.uses & ~esum.defs & ~cur_live & ~fixed_mask) & (RegMask{1} << (r + 1))) != 0) {
            if (!HasRegNote(isum.top_pn.reg_notes, RtlMachineMode::kRegDead, r)) {
              candidates.push_back(find_used_reg_expr(isum.top_pn.pattern, r, mode));
            }
            picked |= (RegMask{1} << r) | (RegMask{1} << (r + 1));
          }
        }

        // Resolve deaths that genuinely occur on instruction `i` itself.
        // Exclude registers whose death in this same live range is already
        // attributed to an earlier pure reader (`flow.c:2485-2515`: a live range
        // cannot die twice without an intervening definition, e.g. when `sched2`
        // or `reorg.c:fill_simple_delay_slots` moved a pure reader into a delay
        // slot past the pre-`sched2`/pre-`reorg` last reader).
        RegMask already_dead_earlier = 0;
        const RegMask raw_pure_dying = esum.uses & ~esum.defs & ~cur_live & ~fixed_mask;
        for (int r = 1; r < kMipsFirstPseudoRegister; ++r) {
          const RegMask rm = RegMask{1} << r;
          if ((raw_pure_dying & rm) == 0) {
            continue;
          }
          for (size_t prev = i; prev > 0; --prev) {
            const InsnSummary& psum = block_sums[prev - 1];
            if (!psum.valid || psum.is_sequence || psum.single.is_call ||
                IsAnyControlTransferPattern(psum.top_pn.pattern)) {
              break;
            }
            if ((psum.single.defs & rm) != 0) {
              break;
            }
            const bool killed_in_prev = std::any_of(
                psum.top_pn.reg_notes.begin(), psum.top_pn.reg_notes.end(),
                [&](const RtlRegNote& n) {
                  if (n.kind != RtlMachineMode::kRegDead || !n.datum.IsHardReg() ||
                      n.datum.IsArgPointerReg() || n.datum.RegNo() >= kMipsFirstPseudoRegister) {
                    return false;
                  }
                  return (RegBitMask(n.datum.RegNo(), n.datum.mode, df.gp64, df.fp64) & rm) != 0;
                });
            if (killed_in_prev) {
              already_dead_earlier |= rm;
              break;
            }
          }
        }
        collect_tier(raw_pure_dying & ~already_dead_earlier);
        if (deferred_store_md_dead.has_value()) {
          candidates.push_back(*std::move(deferred_store_md_dead));
        }
        collect_tier(esum.uses & esum.defs & ~fixed_mask);

        // Invert `reorg.c:2283-2306` (`update_reg_dead_notes`): any remaining
        // `(REG_DEAD (const_int 0))` notes were moved onto `trial` from an
        // intervening instruction `j > i` between `trial` and `delayed_insn`.
        size_t excess =
            (num_const_zero > candidates.size()) ? (num_const_zero - candidates.size()) : 0;
        for (size_t j = i + 1; excess > 0 && j + 1 < block_sums.size(); ++j) {
          InsnSummary& jsum = block_sums[j];
          if (!jsum.valid || jsum.is_sequence) {
            continue;
          }
          const bool is_use_insn = (jsum.top_pn.pattern.op == RtlOp::kUse);
          const RegMask shared_uses = jsum.single.uses & esum.uses & ~fixed_mask;
          if (shared_uses == 0) {
            continue;
          }
          for (int r = 1; excess > 0 && r < kMipsFirstPseudoRegister;) {
            if ((shared_uses & (RegMask{1} << r)) != 0) {
              const RtlMachineMode jmode = jsum.single.use_modes[r];
              const int nregs = HardRegNregs(r, jmode, df.gp64, df.fp64);
              const bool dies_at_j = is_use_insn || ((live_after[j] & (RegMask{1} << r)) == 0) ||
                                     ((jsum.single.defs & (RegMask{1} << r)) != 0);
              if (dies_at_j && !HasRegNote(jsum.top_pn.reg_notes, RtlMachineMode::kRegDead, r)) {
                RtlRegNote restored_note;
                restored_note.kind = RtlMachineMode::kRegDead;
                restored_note.datum = find_used_reg_expr(jsum.top_pn.pattern, r, jmode);
                jsum.top_pn.reg_notes.push_back(std::move(restored_note));
                blk.instructions[j] = FormatInsnWithoutCallFusage(jsum.top_pn);
                if (blk.rtl_insns.size() == blk.instructions.size()) {
                  blk.rtl_insns[j].canonical_pattern = blk.instructions[j];
                } else {
                  block_changed = true;
                }
                resolved_locs.insert((static_cast<uint64_t>(b) << 32) | static_cast<uint64_t>(j));
                --excess;
              }
              r += nregs;
            } else {
              ++r;
            }
          }
        }

        if (excess > 0) {
          collect_tier(esum.uses & ~fixed_mask);
        }

        const size_t sort_count = std::min(num_const_zero, candidates.size());
        if (sort_count >= 2 && ParseCompilationProfileFlags(profile).RunsSched1()) {
          // Order the selected delay-slot `REG_DEAD` candidates to match `sched1`
          // `attach_deaths_insn` (`sched.c:2779-3018`), mapping any cross-bank
          // input-reloaded GPR (or `hi`/`lo` proxy) back to its operand in `ipat`.
          auto sched1_read_rank = [&](const RtlExpr& cand) -> int {
            if (!cand.IsReg() || cand.IsArgPointerReg()) {
              return 1000000;
            }
            int target_reg = cand.RegNo();
            for (const auto& [src_r, dst_r] : cand_proxy_regs) {
              if (src_r == target_reg) {
                target_reg = dst_r;
                break;
              }
            }
            int ord = 0;
            int found = 1000000;
            auto walk = [&](const auto& self, const RtlExpr& e, bool set_p) -> void {
              if (found < 1000000 || e.op == RtlOp::kClobber) {
                return;
              }
              if (e.IsSet() && e.children.size() >= 2) {
                // `attach_deaths_insn` visits `SET_SRC` then `SET_DEST` and prepends,
                // so `SET_DEST` (`MEM` address) appears BEFORE `SET_SRC` in `REG_NOTES`.
                self(self, e.SetDest(), true);
                self(self, e.SetSrc(), false);
                return;
              }
              if (e.IsMem() && !e.children.empty()) {
                self(self, e.children[0], false);
                return;
              }
              if (e.IsReg() && !e.IsArgPointerReg()) {
                if (!set_p) {
                  if (e.RegNo() == target_reg) {
                    found = ord;
                    return;
                  }
                  ++ord;
                }
                return;
              }
              for (const RtlExpr& ch : e.children) {
                self(self, ch, set_p);
              }
            };
            walk(walk, ipat, false);
            return found;
          };
          std::stable_sort(candidates.begin(),
                           candidates.begin() + static_cast<ptrdiff_t>(sort_count),
                           [&](const RtlExpr& a, const RtlExpr& b_expr) {
                             return sched1_read_rank(a) < sched1_read_rank(b_expr);
                           });
        }

        size_t cand_idx = 0;
        std::vector<RtlRegNote> updated_notes;
        updated_notes.reserve(isum.top_pn.reg_notes.size());
        auto already_has_dead_reg = [&](int regno) {
          return std::any_of(isum.top_pn.reg_notes.begin(), isum.top_pn.reg_notes.end(),
                             [&](const RtlRegNote& ex) {
                               return ex.kind == RtlMachineMode::kRegDead && ex.datum.IsReg() &&
                                      !ex.datum.IsArgPointerReg() && ex.datum.RegNo() == regno;
                             }) ||
                 std::any_of(updated_notes.begin(), updated_notes.end(), [&](const RtlRegNote& ex) {
                   return ex.kind == RtlMachineMode::kRegDead && ex.datum.IsReg() &&
                          !ex.datum.IsArgPointerReg() && ex.datum.RegNo() == regno;
                 });
        };
        for (RtlRegNote& note : isum.top_pn.reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.IsConstInt() &&
              note.datum.int_val == 0) {
            while (cand_idx < candidates.size() && candidates[cand_idx].IsReg() &&
                   !candidates[cand_idx].IsArgPointerReg() &&
                   already_has_dead_reg(candidates[cand_idx].RegNo())) {
              ++cand_idx;
            }
            if (cand_idx < candidates.size()) {
              note.datum = candidates[cand_idx++];
              updated_notes.push_back(std::move(note));
            }
          } else {
            updated_notes.push_back(std::move(note));
          }
        }
        isum.top_pn.reg_notes = std::move(updated_notes);
        blk.instructions[i] = FormatInsnWithoutCallFusage(isum.top_pn);
        if (blk.rtl_insns.size() == blk.instructions.size()) {
          blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
        } else {
          block_changed = true;
        }
        resolved_locs.insert((static_cast<uint64_t>(b) << 32) | static_cast<uint64_t>(i));
      }
      cur_live = (cur_live & ~esum.defs) | esum.uses;
    }
    if (block_changed) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }

  // When a delay-slot instruction unpacked by `InvertFillSimpleDelaySlots` is
  // the single consumer `last_u` of a proven `reload` stack-spill load/store,
  // `reorg.c` had overwritten its `(REG_DEAD (mem [sp+off]))` datum with
  // `(const_int 0)`. Upgrade the resolved `(REG_DEAD (reg scr))` note on
  // `resolved_locs` to `(REG_DEAD (mem [sp+off]))` (or strip if the slot stays
  // live).
  if (!resolved_locs.empty() && !IsUnoptimizedProfile(&profile)) {
    const DataflowResult post_df = RunHardRegDataflow(*cfg, &profile);
    const size_t nb = cfg->rpo_blocks.size();
    std::vector<Stage0BlockParsed> bp(nb);
    for (size_t b = 0; b < nb; ++b) {
      const auto& blk = cfg->rpo_blocks[b];
      const size_t ni = blk.instructions.size();
      bp[b].tops.resize(ni);
      bp[b].seq_head_wraps.resize(ni);
      bp[b].seq_slot_wraps.resize(ni);
      bp[b].seq_heads.resize(ni);
      bp[b].seq_slots.resize(ni);
      bp[b].is_seq.assign(ni, false);
      bp[b].dirty.assign(ni, false);
      for (size_t i = 0; i < ni; ++i) {
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok()) {
          continue;
        }
        bp[b].tops[i] = std::move(*parsed);
        if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
            !bp[b].tops[i].has_explicit_call_fusage) {
          bp[b].tops[i].has_explicit_call_fusage = true;
          bp[b].tops[i].call_fusage = blk.rtl_insns[i].call_fusage;
        }
        bp[b].steps.push_back(
            Stage0StepRef{.insn_idx = i, .is_seq = false, .in_slot = false, .pn = &bp[b].tops[i]});
      }
    }
    bool found_b0_sp = false;
    size_t b0_sp_step = 0;
    absl::flat_hash_set<size_t> prologue_save_steps;
    absl::flat_hash_set<int64_t> caller_save_offsets;
    absl::flat_hash_set<int64_t> proven_spill_offsets;
    IdentifyStage0CallerSaveAndSpillOffsets(*cfg, post_df, bp, &found_b0_sp, &b0_sp_step,
                                            &prologue_save_steps, &caller_save_offsets,
                                            &proven_spill_offsets);
    if (!proven_spill_offsets.empty()) {
      absl::flat_hash_map<int64_t, std::vector<bool>> spill_live_out;
      for (int64_t off : proven_spill_offsets) {
        std::vector<bool> lin(nb, false), lout(nb, false);
        bool chg = true;
        while (chg) {
          chg = false;
          for (size_t rev_b = nb; rev_b > 0; --rev_b) {
            const size_t b = rev_b - 1;
            bool out_val = false;
            for (size_t succ : cfg->rpo_blocks[b].successors) {
              if (succ < nb && lin[succ]) out_val = true;
            }
            lout[b] = out_val;
            bool cur = out_val;
            for (size_t rev_s = bp[b].steps.size(); rev_s > 0; --rev_s) {
              const RtlExpr& pat = bp[b].steps[rev_s - 1].pn->pattern;
              int64_t moff = 0, msz = 0;
              if (pat.IsSet() && ExtractStage0SpMem(pat.SetDest(), &moff, &msz) && moff == off) {
                cur = false;
              } else if (pat.IsSet() && ExtractStage0SpMem(pat.SetSrc(), &moff, &msz) &&
                         moff == off) {
                cur = true;
              }
            }
            if (cur != lin[b]) {
              lin[b] = cur;
              chg = true;
            }
          }
        }
        spill_live_out[off] = std::move(lout);
      }

      for (size_t b = 0; b < nb; ++b) {
        auto& steps = bp[b].steps;
        const size_t ns = steps.size();
        std::vector<ElemSummary> esums(ns);
        std::vector<RegMask> live_after_step(ns, 0);
        RegMask cur_l = post_df.live_out[b];
        for (size_t rev = ns; rev > 0; --rev) {
          const size_t s = rev - 1;
          esums[s] = SummarizeStage0Step(steps[s], post_df);
          live_after_step[s] = cur_l;
          cur_l = (cur_l & ~esums[s].defs) | esums[s].uses;
        }
        absl::flat_hash_map<int64_t, std::vector<bool>> slot_live_after(
            proven_spill_offsets.size());
        for (int64_t off : proven_spill_offsets) {
          std::vector<bool> la(ns, false);
          bool cur = spill_live_out[off][b];
          for (size_t rev = ns; rev > 0; --rev) {
            const size_t idx = rev - 1;
            la[idx] = cur;
            const RtlExpr& pat = steps[idx].pn->pattern;
            int64_t moff = 0, msz = 0;
            if (pat.IsSet() && ExtractStage0SpMem(pat.SetDest(), &moff, &msz) && moff == off) {
              cur = false;
            } else if (pat.IsSet() && ExtractStage0SpMem(pat.SetSrc(), &moff, &msz) &&
                       moff == off) {
              cur = true;
            }
          }
          slot_live_after[off] = std::move(la);
        }

        auto apply_to_resolved_last_u = [&](size_t s, int64_t off, int scr, RegMask scr_m,
                                            const RtlExpr& slot_mem) {
          size_t last_u = ns;
          bool slot_reloaded_between = false;
          for (size_t k = s + 1; k < ns; ++k) {
            int64_t koff = 0, ksz = 0;
            if (steps[k].pn->pattern.IsSet() &&
                ExtractStage0SpMem(steps[k].pn->pattern.SetSrc(), &koff, &ksz) && koff == off) {
              slot_reloaded_between = true;
            }
            if ((esums[k].uses & scr_m) != 0) {
              last_u = k;
              if ((live_after_step[k] & scr_m) == 0) {
                break;
              }
            }
            if ((esums[k].defs & scr_m) != 0 || esums[k].is_call ||
                IsAnyControlTransferPattern(steps[k].pn->pattern)) {
              break;
            }
          }
          if (last_u >= ns || (live_after_step[last_u] & scr_m) != 0) {
            return;
          }
          const uint64_t loc_key =
              (static_cast<uint64_t>(b) << 32) | static_cast<uint64_t>(steps[last_u].insn_idx);
          if (!resolved_locs.contains(loc_key)) {
            return;
          }
          const bool slot_dies = !slot_reloaded_between && !slot_live_after[off][s];
          if (slot_dies) {
            for (RtlRegNote& n : steps[last_u].pn->reg_notes) {
              if (n.kind == RtlMachineMode::kRegDead && n.datum.IsReg() && n.datum.RegNo() == scr) {
                n.datum = slot_mem;
                bp[b].dirty[steps[last_u].insn_idx] = true;
                break;
              }
            }
          } else {
            StripStage0RegDeadOnStep(&bp[b], &steps[last_u], scr);
          }
        };

        for (size_t s = 0; s < ns; ++s) {
          if (b == 0 && ((found_b0_sp && s <= b0_sp_step) || prologue_save_steps.contains(s))) {
            continue;
          }
          const RtlExpr& pat = steps[s].pn->pattern;
          int64_t off = 0, sz = 0;
          if (pat.IsSet() && pat.SetSrc().IsReg() && ExtractStage0SpMem(pat.SetDest(), &off, &sz) &&
              proven_spill_offsets.contains(off)) {
            const int scr = pat.SetSrc().RegNo();
            const RegMask scr_m = RegBitMask(scr, pat.SetSrc().mode, post_df.gp64, post_df.fp64);
            bool pure_output_reload_scratch = false;
            for (size_t prev = s; prev > 0; --prev) {
              const size_t pi = prev - 1;
              if ((esums[pi].defs & scr_m) != 0) {
                const RtlExpr& pdef = steps[pi].pn->pattern;
                if (pdef.IsSet() && !pdef.SetSrc().IsReg()) {
                  pure_output_reload_scratch = true;
                }
                break;
              }
              if (esums[pi].is_call || IsAnyControlTransferPattern(steps[pi].pn->pattern)) {
                break;
              }
            }
            if (pure_output_reload_scratch && (live_after_step[s] & scr_m) != 0) {
              apply_to_resolved_last_u(s, off, scr, scr_m, pat.SetDest());
            }
          } else if (pat.IsSet() && pat.SetDest().IsReg() &&
                     ExtractStage0SpMem(pat.SetSrc(), &off, &sz) &&
                     proven_spill_offsets.contains(off)) {
            const int scr = pat.SetDest().RegNo();
            const RegMask scr_m = RegBitMask(scr, pat.SetDest().mode, post_df.gp64, post_df.fp64);
            apply_to_resolved_last_u(s, off, scr, scr_m, pat.SetSrc());
          }
        }
        FlushNormalizedStage0BlockInsns(b, bp[b], cfg);
      }
    }
  }
}

void ResolvePostReloadConstZeroRegDeadNotes(RtlControlFlowGraph* cfg) {
  const CompilationProfile default_profile{.optimize_level = 2};
  ResolvePostReloadConstZeroRegDeadNotes(cfg, default_profile);
}

void NormalizePostReloadHardRegNotes(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty() || IsUnoptimizedProfile(&profile)) {
    return;
  }
  const DataflowResult df = RunHardRegDataflow(*cfg, &profile);
  NormalizeStage0PostReloadRegNotes(cfg, df, profile);
}

void SynthesizePostReloadMemRegEquivNotes(RtlControlFlowGraph* cfg,
                                          const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty() || IsUnoptimizedProfile(&profile)) {
    return;
  }
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  if (flags.opt_level <= 0) {
    return;
  }
  const DataflowResult df = RunHardRegDataflow(*cfg, &profile);
  const size_t nb = cfg->rpo_blocks.size();
  std::vector<Stage0BlockParsed> bp;
  ParseStage0Blocks(*cfg, &bp);
  for (size_t b = 0; b < nb; ++b) {
    for (Stage0StepRef& st : bp[b].steps) {
      auto& notes = st.pn->reg_notes;
      const size_t before = notes.size();
      notes.erase(std::remove_if(notes.begin(), notes.end(),
                                 [](const RtlRegNote& n) {
                                   return n.kind == RtlMachineMode::kRegEquiv &&
                                          n.datum.op == RtlOp::kMem;
                                 }),
                  notes.end());
      if (notes.size() != before) {
        bp[b].dirty[st.insn_idx] = true;
      }
    }
  }
  bool found_b0_sp = false;
  size_t b0_sp_step = 0;
  absl::flat_hash_set<size_t> prologue_save_steps;
  absl::flat_hash_set<int64_t> caller_save_offsets;
  absl::flat_hash_set<int64_t> proven_spill_offsets;
  IdentifyStage0CallerSaveAndSpillOffsets(*cfg, df, bp, &found_b0_sp, &b0_sp_step,
                                          &prologue_save_steps, &caller_save_offsets,
                                          &proven_spill_offsets);
  const Stage0MemEquivLayout layout =
      ComputeStage0MemEquivLayout(*cfg, df, flags, bp, found_b0_sp, b0_sp_step, prologue_save_steps,
                                  caller_save_offsets, proven_spill_offsets);
  std::vector<std::vector<ElemSummary>> all_esums(nb);
  std::vector<std::vector<RegMask>> all_live_after(nb);
  std::vector<std::vector<size_t>> block_preds(nb);
  for (size_t b = 0; b < nb; ++b) {
    ComputeStage0BlockLiveAfterSteps(*cfg, df, b, bp[b], &all_esums[b], &all_live_after[b]);
    ForEachStage0CfgSucc(cfg->rpo_blocks[b], nb, [&](size_t succ) {
      if (std::find(block_preds[succ].begin(), block_preds[succ].end(), b) ==
          block_preds[succ].end()) {
        block_preds[succ].push_back(b);
      }
    });
  }
  for (size_t b = nb; b-- > 0;) {
    SynthesizeStage0MemRegEquivNotes(*cfg, df, flags, layout, b, found_b0_sp, b0_sp_step,
                                     prologue_save_steps, caller_save_offsets, proven_spill_offsets,
                                     bp, all_esums, all_live_after, block_preds, &bp[b]);
  }
  for (size_t b = 0; b < nb; ++b) {
    FlushNormalizedStage0BlockInsns(b, bp[b], cfg);
  }
}

bool IsAnyDefinedHardRegLiveAtBlockEntry(const RtlControlFlowGraph& cfg, const RtlExpr& def_pat,
                                         size_t block_idx) {
  if (block_idx >= cfg.rpo_blocks.size()) {
    return false;
  }
  const DataflowResult df =
      RunHardRegDataflow(cfg, /*profile=*/nullptr,
                         /*dbr_post_simple_markers=*/(cfg.pass == RtlPassKind::kDbr));
  ElemSummary sum;
  AnalyzePatternAst(def_pat, df.gp64, df.fp64, &sum);
  return (df.live_in[block_idx] & sum.defs) != 0;
}

std::vector<std::vector<int>> ComputePostReloadHardRegLiveIn(const RtlControlFlowGraph& cfg) {
  std::vector<std::vector<int>> out(cfg.rpo_blocks.size());
  if (cfg.rpo_blocks.empty()) {
    return out;
  }
  const DataflowResult df =
      RunHardRegDataflow(cfg, /*profile=*/nullptr,
                         /*dbr_post_simple_markers=*/(cfg.pass == RtlPassKind::kDbr));
  for (size_t b = 0; b < cfg.rpo_blocks.size(); ++b) {
    const RegMask mask = df.live_in[b];
    for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
      if ((mask & (RegMask{1} << r)) != 0) {
        out[b].push_back(r);
      }
    }
  }
  return out;
}

bool HasDeletedSharedReturnMoveWitness(const RtlControlFlowGraph& cfg,
                                       const CompilationProfile& profile, int ret_hr,
                                       RtlMachineMode ret_mode) {
  if (cfg.rpo_blocks.size() < 3) {
    return false;
  }
  const DataflowResult df =
      RunHardRegDataflow(cfg, &profile,
                         /*dbr_post_simple_markers=*/(cfg.pass == RtlPassKind::kDbr));
  return HasDeletedSharedReturnMoveWitnessWithDataflow(cfg, df, ret_hr, ret_mode);
}

}  // namespace rom_nom_nom::lifter
