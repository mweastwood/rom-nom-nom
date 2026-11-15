#include "lifter/reconstruct_hard_reg_liveness.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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
  std::array<RtlMachineMode, kMipsFirstPseudoRegister> def_modes{};
};

void CollectReadRegs(const RtlExpr& expr, bool gp64, bool fp64, ElemSummary* out) {
  if (expr.IsReg()) {
    const int regno = expr.RegNo();
    if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
      const RegMask m = RegBitMask(regno, expr.mode, gp64, fp64);
      out->uses |= m;
      out->use_modes[regno] = expr.mode;
      if (expr.flags.integrated) {
        out->integrated_ret_regs |= (m & kRetCandMask);
      }
    }
    return;
  }
  if (expr.IsSubreg() && !expr.children.empty() && expr.children[0].IsReg()) {
    const int base_regno = expr.children[0].RegNo();
    if (base_regno >= 0 && base_regno < kMipsFirstPseudoRegister) {
      const int regno = base_regno + static_cast<int>(expr.int_val);
      if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
        const RegMask m = RegBitMask(regno, expr.mode, gp64, fp64);
        out->uses |= m;
        out->use_modes[regno] = expr.mode;
        if (expr.children[0].flags.integrated) {
          out->integrated_ret_regs |= (m & kRetCandMask);
        }
      }
    }
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

ElemSummary SummarizeSingleElem(const RtlPatternAndNotes& pn, bool force_call, bool gp64, bool fp64,
                                RegMask* pending_arg_defs) {
  ElemSummary s;
  s.use_modes.fill(RtlMachineMode::kSi);
  s.def_modes.fill(RtlMachineMode::kSi);
  const RtlExpr& pat = pn.pattern;
  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber) {
    s.is_note_or_pseudo = true;
  }
  s.is_call = force_call || IsCallPattern(pat);
  AnalyzePatternAst(pat, gp64, fp64, &s);

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
    *pending_arg_defs &= ~(s.defs & ~s.explicit_sets);
    if (!fp64) {
      for (int even_fpr : {44, 46}) {
        if ((s.explicit_sets & (RegMask{1} << even_fpr)) != 0 &&
            (s.explicit_sets & (RegMask{1} << (even_fpr + 1))) == 0) {
          *pending_arg_defs &= ~(RegMask{1} << (even_fpr + 1));
        }
      }
    }
    *pending_arg_defs |= (s.explicit_sets & kArgRegMask);
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
  if (sym.size() < 4 || sym.substr(0, 2) != "__") {
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

  auto is_callee_saved_gpr = [](int r) -> bool { return (r >= 16 && r <= 23) || r == 30; };
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
  absl::flat_hash_map<int, size_t> gpr_save_pos;
  for (size_t idx = 0; idx < pre_elems.size(); ++idx) {
    int saved_reg = -1;
    RtlMachineMode save_mode = RtlMachineMode::kSi;
    if (match_prologue_sp_save(pre_elems[idx].pn->pattern, &saved_reg, &save_mode)) {
      if (saved_reg == 31 && ra_save_pos < 0) {
        ra_save_pos = static_cast<int>(idx);
      } else if (is_callee_saved_gpr(saved_reg)) {
        const int span = HardRegNregs(saved_reg, save_mode, res->gp64, res->fp64);
        for (int off = 0; off < span; ++off) {
          if (is_callee_saved_gpr(saved_reg + off) && !gpr_save_pos.contains(saved_reg + off)) {
            gpr_save_pos[saved_reg + off] = idx;
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
    int span = 1;
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
    const int span = HardRegNregs(saved_reg, mode, res->gp64, res->fp64);
    if (!is_callee_saved_gpr(saved_reg) || arg_reg < 4 || arg_reg + span - 1 > 7) {
      continue;
    }
    if (span == 2 && !is_callee_saved_gpr(saved_reg + 1)) {
      continue;
    }
    bool valid = true;
    for (int off = 0; off < span; ++off) {
      const int sr = saved_reg + off;
      const int ar = arg_reg + off;
      if ((read_after_first_call & (RegMask{1} << sr)) == 0 ||
          (arg_word_occupied_by_fpr & (RegMask{1} << ar)) != 0) {
        valid = false;
      }
      for (size_t k = 0; k < idx; ++k) {
        if ((pre_elems[k].esum->defs & (RegMask{1} << ar)) != 0) {
          valid = false;
        }
      }
      for (size_t k = idx + 1; k < pre_elems.size(); ++k) {
        if ((pre_elems[k].esum->defs & ((RegMask{1} << sr) | (RegMask{1} << ar))) != 0 ||
            (pre_elems[k].esum->uses & ((RegMask{1} << sr) | (RegMask{1} << ar))) != 0) {
          valid = false;
        }
      }
    }
    if (!valid) {
      continue;
    }
    cands.push_back(PreCallCopyCand{idx, saved_reg, arg_reg, span});
    for (int off = 0; off < span; ++off) {
      arg_gpr_saved_in_callee_reg |= (RegMask{1} << (arg_reg + off));
    }
  }

  auto has_cand = [&](int sr, int ar) -> bool {
    for (const PreCallCopyCand& c : cands) {
      if (c.saved_reg == sr && c.arg_reg == ar && c.span == 1) {
        return true;
      }
    }
    return false;
  };

  for (const PreCallCopyCand& cand : cands) {
    bool save_hoisted_before_ra = false;
    for (int off = 0; off < cand.span; ++off) {
      auto it = gpr_save_pos.find(cand.saved_reg + off);
      if (it != gpr_save_pos.end() && it->second < static_cast<size_t>(ra_save_pos) &&
          it->second < cand.copy_pos) {
        save_hoisted_before_ra = true;
      }
    }
    if (!res->gp64 && cand.span == 1 && (cand.saved_reg % 2) == 1 && cand.arg_reg == 7 &&
        has_cand(cand.saved_reg - 1, 6)) {
      auto it_hi = gpr_save_pos.find(cand.saved_reg - 1);
      if (it_hi != gpr_save_pos.end() && it_hi->second < static_cast<size_t>(ra_save_pos) &&
          it_hi->second < cand.copy_pos) {
        save_hoisted_before_ra = true;
      }
    }

    bool higher_arg_written = false;
    for (int r = cand.arg_reg + cand.span; r <= 7; ++r) {
      if ((arg_gpr_live_into_call & (RegMask{1} << r)) != 0) {
        higher_arg_written = true;
      }
    }

    const bool is_o32_aligned_pair_at_6 =
        (cand.span == 2 && cand.arg_reg == 6) ||
        (!res->gp64 && cand.span == 1 &&
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
      for (int off = 0; off < cand.span; ++off) {
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

void RefineCallArgumentFusageAcrossCfg(
    const RtlControlFlowGraph& cfg, const CompilationProfile* profile,
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

  auto step_arg_elem = [&](const ElemSummary& e, const RtlPatternAndNotes& pn, RegMask* must_valid,
                           RegMask* may_explicit) {
    if (e.is_call) {
      *must_valid = 0;
      *may_explicit = 0;
      return;
    }
    if (pn.pattern.op == RtlOp::kNote) {
      return;
    }
    const RegMask dead_notes = dead_arg_mask_from_notes(pn) & ~e.explicit_sets;
    RegMask clobbered = (e.defs & ~e.explicit_sets) | dead_notes;
    if (!res.fp64) {
      for (int even_fpr : {44, 46}) {
        if ((e.explicit_sets & (RegMask{1} << even_fpr)) != 0 &&
            (e.explicit_sets & (RegMask{1} << (even_fpr + 1))) == 0) {
          clobbered |= (RegMask{1} << (even_fpr + 1));
        }
      }
    }
    *must_valid = ((*must_valid & ~clobbered) | (e.explicit_sets & kArgRegMask)) & kArgRegMask;
    if (profile != nullptr) {
      const RegMask read_without_set = (e.uses & kArgRegMask) & ~e.explicit_sets;
      *may_explicit &= ~read_without_set;
    }
    *may_explicit = ((*may_explicit & ~clobbered) | (e.explicit_sets & kArgRegMask)) & kArgRegMask;
  };

  std::vector<RegMask> must_valid_in(n_blocks, kArgRegMask);
  std::vector<RegMask> may_explicit_in(n_blocks, 0);
  std::vector<bool> has_arg_init(n_blocks, false);
  must_valid_in[0] = incoming_param_regs;
  has_arg_init[0] = true;

  bool changed_arg_fwd = true;
  while (changed_arg_fwd) {
    changed_arg_fwd = false;
    std::vector<RegMask> next_valid_in(n_blocks, kArgRegMask);
    std::vector<RegMask> next_explicit_in(n_blocks, 0);
    std::vector<bool> next_init(n_blocks, false);
    next_valid_in[0] = incoming_param_regs;
    next_init[0] = true;

    for (size_t b = 0; b < n_blocks; ++b) {
      if (!arg_reachable[b] || !has_arg_init[b]) {
        continue;
      }
      RegMask cur_valid = must_valid_in[b];
      RegMask cur_explicit = may_explicit_in[b];
      RegMask ft_valid = 0;
      RegMask ft_explicit = 0;
      RegMask taken_valid = 0;
      RegMask taken_explicit = 0;
      bool split_edges = false;
      const size_t n_insns = res.block_summaries[b].size();
      const bool is_cond = (cfg.rpo_blocks[b].successors.size() >= 2);

      for (size_t i = 0; i < n_insns; ++i) {
        const InsnSummary& isum = res.block_summaries[b][i];
        if (!isum.valid) continue;
        if (isum.is_sequence && !isum.is_call_seq && is_cond && i + 1 == n_insns &&
            isum.is_annulled_target_seq) {
          step_arg_elem(isum.head, isum.head_pn, &cur_valid, &cur_explicit);
          ft_valid = cur_valid;
          ft_explicit = cur_explicit;
          taken_valid = cur_valid;
          taken_explicit = cur_explicit;
          step_arg_elem(isum.slot, isum.slot_pn, &taken_valid, &taken_explicit);
          split_edges = true;
          break;
        }
        if (isum.is_sequence) {
          if (isum.is_call_seq) {
            step_arg_elem(isum.slot, isum.slot_pn, &cur_valid, &cur_explicit);
            step_arg_elem(isum.head, isum.head_pn, &cur_valid, &cur_explicit);
          } else {
            step_arg_elem(isum.head, isum.head_pn, &cur_valid, &cur_explicit);
            step_arg_elem(isum.slot, isum.slot_pn, &cur_valid, &cur_explicit);
          }
        } else {
          step_arg_elem(isum.single, isum.top_pn, &cur_valid, &cur_explicit);
        }
      }

      for (size_t sb : cfg.rpo_blocks[b].successors) {
        if (sb >= n_blocks || !arg_reachable[sb]) continue;
        const RegMask edge_valid =
            split_edges ? ((sb == b + 1) ? ft_valid : taken_valid) : cur_valid;
        const RegMask edge_explicit =
            split_edges ? ((sb == b + 1) ? ft_explicit : taken_explicit) : cur_explicit;
        next_valid_in[sb] &= edge_valid;
        next_explicit_in[sb] |= edge_explicit;
        next_init[sb] = true;
      }
    }

    for (size_t b = 0; b < n_blocks; ++b) {
      if (!arg_reachable[b]) continue;
      if (next_init[b] != has_arg_init[b] || next_valid_in[b] != must_valid_in[b] ||
          next_explicit_in[b] != may_explicit_in[b]) {
        has_arg_init[b] = next_init[b];
        must_valid_in[b] = next_valid_in[b];
        may_explicit_in[b] = next_explicit_in[b];
        changed_arg_fwd = true;
      }
    }
  }

  absl::flat_hash_map<std::string, RegMask> callee_observed_args;
  for (int pass_idx = 0; pass_idx < 2; ++pass_idx) {
    for (size_t b = 0; b < n_blocks; ++b) {
      if (!arg_reachable[b] || !has_arg_init[b]) {
        continue;
      }
      RegMask cur_valid = must_valid_in[b];
      RegMask cur_explicit = may_explicit_in[b];
      for (size_t i = 0; i < res.block_summaries[b].size(); ++i) {
        InsnSummary& isum = res.block_summaries[b][i];
        if (!isum.valid) continue;
        auto process_call = [&](ElemSummary* call_elem, const RtlPatternAndNotes& call_pn) {
          const std::string sym = ExtractCalleeSymbolRef(call_pn.pattern);
          if (call_pn.has_explicit_call_fusage ||
              LookupKnownGccLibcallArgMask(sym, res.gp64, res.fp64, profile).has_value()) {
            step_arg_elem(*call_elem, call_pn, &cur_valid, &cur_explicit);
            return;
          }
          const RegMask cs_mask = caller_saved_arg_masks[b][i];
          const RegMask call_valid = cur_valid & ~cs_mask;
          const RegMask base_explicit =
              (call_elem->arg_fusage_uses | (call_valid & cur_explicit)) & ~cs_mask;
          const RegMask closed_explicit = close_o32_arg_prefix(base_explicit, call_valid);
          if (pass_idx == 0) {
            if (!sym.empty()) {
              callee_observed_args[sym] |= closed_explicit;
            }
          } else {
            RegMask inferred = closed_explicit;
            if (!sym.empty()) {
              if (auto it = callee_observed_args.find(sym); it != callee_observed_args.end()) {
                inferred |= (call_valid & it->second);
              }
            }
            RegMask candidate = (call_elem->arg_fusage_uses | inferred) & ~cs_mask;
            // In KMC GCC 2.7.2 (`mips.c:function_arg`), `$f12` (reg 44) and
            // `$a0` (reg 4) both correspond to `cum->arg_words == 0` and are
            // mutually exclusive on every call. If pre-call scratch writes put
            // both into `candidate`, keep the unread outgoing argument write
            // (`cur_explicit`) and drop the already-consumed scratch register.
            if ((candidate & (RegMask{1} << 44)) != 0 && (candidate & (RegMask{1} << 4)) != 0) {
              const bool f12_unread = (cur_explicit & (RegMask{1} << 44)) != 0;
              const bool r4_unread = (cur_explicit & (RegMask{1} << 4)) != 0;
              if (f12_unread && !r4_unread) {
                candidate &= ~(RegMask{1} << 4);
                if (!res.gp64 && !res.fp64 && (candidate & (RegMask{1} << 45)) != 0 &&
                    (cur_explicit & (RegMask{1} << 5)) == 0) {
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
          step_arg_elem(*call_elem, call_pn, &cur_valid, &cur_explicit);
        };

        if (isum.is_sequence) {
          if (isum.is_call_seq) {
            step_arg_elem(isum.slot, isum.slot_pn, &cur_valid, &cur_explicit);
            process_call(&isum.head, isum.head_pn);
          } else {
            step_arg_elem(isum.head, isum.head_pn, &cur_valid, &cur_explicit);
            step_arg_elem(isum.slot, isum.slot_pn, &cur_valid, &cur_explicit);
          }
        } else if (isum.single.is_call) {
          process_call(&isum.single, isum.top_pn);
        } else {
          step_arg_elem(isum.single, isum.top_pn, &cur_valid, &cur_explicit);
        }
      }
    }
  }

  InferEntryFirstCallDeletedNoopArgMoves(profile, &res);
}

DataflowResult RunHardRegDataflow(const RtlControlFlowGraph& cfg, const CompilationProfile* profile,
                                  bool dbr_post_simple_markers = false) {
  DataflowResult res;
  DetectRegisterWidthFlags(cfg, profile, &res.gp64, &res.fp64);
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
      res.epilogue_delay =
          SummarizeSingleElem(*ep_pn, /*force_call=*/false, res.gp64, res.fp64, &dummy_args);
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
                                            &pending_arg_defs);
            isum.head = SummarizeSingleElem(isum.head_pn, /*force_call=*/true, res.gp64, res.fp64,
                                            &pending_arg_defs);
          } else {
            isum.head = SummarizeSingleElem(isum.head_pn, /*force_call=*/false, res.gp64, res.fp64,
                                            &pending_arg_defs);
            isum.slot = SummarizeSingleElem(isum.slot_pn, /*force_call=*/false, res.gp64, res.fp64,
                                            &pending_arg_defs);
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
                                        &pending_arg_defs);
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

  RefineCallArgumentFusageAcrossCfg(cfg, profile, caller_saved_arg_masks, &res);

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
      res.live_at_exit |= inferred;
    }
  }

  std::vector<RegMask> may_ret_def_in(n_blocks, 0);
  std::vector<RegMask> may_ret_def_exit(n_blocks, 0);
  if ((res.live_at_exit & kRetCandMask) != 0 && n_blocks > 0) {
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
            ft_may = cur | (isum.head.explicit_sets & kRetCandMask);
            taken_may = ft_may | (isum.slot.explicit_sets & kRetCandMask);
            cur = ft_may;
            split_edges = true;
            break;
          }
          if (isum.is_sequence) {
            cur |= ((isum.head.explicit_sets | isum.slot.explicit_sets) & kRetCandMask);
          } else {
            cur |= (isum.single.explicit_sets & kRetCandMask);
          }
        }
        RegMask ex_may = cur;
        if (res.uses_epilogue_delay_at_exit[b]) {
          ex_may |= (res.epilogue_delay.explicit_sets & kRetCandMask);
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
  }

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
      for (auto it = res.block_summaries[b].rbegin(); it != res.block_summaries[b].rend(); ++it) {
        cur = backward_transfer(*it, b, cur);
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
                       RegMask fixed_mask, bool gp64, bool fp64, bool in_delay_slot = false) {
  if (esum.is_call || esum.is_note_or_pseudo) {
    return false;
  }
  bool changed = false;
  const RegMask dead_mask = esum.uses & ~esum.defs & ~live_after & ~fixed_mask;
  const RegMask unused_mask = esum.defs & ~live_after & ~fixed_mask;
  if (in_delay_slot) {
    // In `reorg.c:emit_delay_sequence` (`reorg.c:915-920`), every `REG_DEAD` note on a
    // `SEQUENCE` delay-slot instruction has its datum overwritten with `const0_rtx`
    // (`(const_int 0)`).
    size_t expected_dead_count = 0;
    for (int r = 1; r < kMipsFirstPseudoRegister;) {
      if ((dead_mask & (RegMask{1} << r)) != 0) {
        ++expected_dead_count;
        r += HardRegNregs(r, esum.use_modes[r], gp64, fp64);
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
        if (!HasRegNote(pn->reg_notes, RtlMachineMode::kRegDead, r)) {
          pn->reg_notes.push_back(RtlRegNote{
              .kind = RtlMachineMode::kRegDead,
              .list_op = RtlOp::kExprList,
              .datum = RtlExpr::MakeReg(mode, r),
          });
          changed = true;
        }
        r += HardRegNregs(r, mode, gp64, fp64);
      } else {
        ++r;
      }
    }
  }
  for (int r = 1; r < kMipsFirstPseudoRegister;) {
    if ((unused_mask & (RegMask{1} << r)) != 0) {
      const RtlMachineMode mode = esum.def_modes[r];
      if (!HasRegNote(pn->reg_notes, RtlMachineMode::kRegUnused, r)) {
        pn->reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegUnused,
            .list_op = RtlOp::kExprList,
            .datum = RtlExpr::MakeReg(mode, r),
        });
        changed = true;
      }
      r += HardRegNregs(r, mode, gp64, fp64);
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

  const size_t n_blocks = cfg->rpo_blocks.size();
  for (size_t b = 0; b < n_blocks; ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
    const size_t limit = std::min(blk.rtl_insns.size(), df.block_summaries[b].size());
    for (size_t i = 0; i < limit; ++i) {
      const InsnSummary& isum = df.block_summaries[b][i];
      if (!isum.valid || !(isum.is_call_seq || (!isum.is_sequence && isum.single.is_call))) {
        continue;
      }
      RtlInsn& rinsn = blk.rtl_insns[i];
      if (!rinsn.has_explicit_call_fusage) {
        const RegMask fusage_mask =
            isum.is_call_seq ? isum.head.arg_fusage_uses : isum.single.arg_fusage_uses;
        rinsn.has_explicit_call_fusage = true;
        rinsn.call_fusage = build_call_fusage_exprs(fusage_mask);
      }
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

void ReconstructPostDbrHardRegLivenessAndReturnUse(RtlControlFlowGraph* cfg,
                                                   const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }

  DataflowResult df = RunHardRegDataflow(*cfg, &profile);
  const bool attach_notes = (profile.optimize_level > 0);
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
    bool changed_ret = true;
    while (changed_ret) {
      changed_ret = false;
      for (size_t rev = n_blocks; rev > 0; --rev) {
        const size_t b = rev - 1;
        RegMask out_need = 0;
        if (df.can_exit[b]) {
          out_need |= df.uses_epilogue_delay_at_exit[b] ? (exit_ret_mask & ~df.epilogue_delay.defs)
                                                        : exit_ret_mask;
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

  if (exit_ret_mask == 0 && !attach_notes) {
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
      // immediately applies the opposite adjustment.
      if (!mark_in_struct && !cfg->rpo_blocks[ft_b].has_label && preds[ft_b].size() == 1 &&
          preds[ft_b][0] == b) {
        for (const InsnSummary& ft_sum : df.block_summaries[ft_b]) {
          if (!ft_sum.valid || (!ft_sum.is_sequence && ft_sum.single.is_note_or_pseudo)) {
            continue;
          }
          if (!ft_sum.is_sequence &&
              is_opposite_arith_self_pair(isum.slot_pn.pattern, ft_sum.top_pn.pattern)) {
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
            if (!p_last.is_sequence && p + 1 == target_b &&
                p_last.top_pn.pattern == isum.slot_pn.pattern) {
              mark_in_struct = true;
              break;
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
      }

      if (mark_in_struct) {
        isum.seq_slot_wrap.flags.in_struct = true;
        cfg->rpo_blocks[b].instructions.back() =
            RtlExpr::MakeSequence(
                {
                    RtlExpr::MakeInsnWrapper(isum.seq_head_wrap.op, isum.seq_head_wrap.flags,
                                             isum.head_pn),
                    RtlExpr::MakeInsnWrapper(isum.seq_slot_wrap.op, isum.seq_slot_wrap.flags,
                                             isum.slot_pn),
                })
                .ToGccPrintRtlString();
        cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
      }
    }

    // (I4) Unconditional jump `SEQUENCE` target-steal recovery (`reorg.c:3684-3701`
    // `steal_delay_list_from_target`): when a forward `mostly_true_jump == 0`
    // conditional `SEQUENCE` in block `b` (`!unchanging`, `slot.in_struct`) falls
    // through to a single-predecessor unlabeled 1-instruction unconditional jump
    // `SEQUENCE` in `u = b + 1` whose `slot_defs` was dead at `b`'s pre-steal
    // target, `reorg.c:steal_delay_list_from_fallthrough` would have stolen `u`'s
    // delay slot into `b` if `u`'s delay slot had been inline (`!in_struct`) in
    // `.11.jump2`. Therefore `u` (and any sibling unconditional jump `SEQUENCE`
    // targeting `u_target` with the identical delay-slot pattern) stole its delay
    // slot from a shared target block and must carry `insn/s` (`in_struct = true`).
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
          cfg->rpo_blocks[u].successors.size() != 1 ||
          !cfg->rpo_blocks[u].addr_vec_targets.empty() || df.block_summaries[u].size() != 1) {
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
  }

  for (size_t b = 0; b < n_blocks; ++b) {
    bool block_changed = false;
    RegMask cur_live = df.live_out[b];
    RegMask cur_ret_need = ret_need_out[b];

    for (size_t rev_i = df.block_summaries[b].size(); rev_i > 0; --rev_i) {
      const size_t i = rev_i - 1;
      InsnSummary& isum = df.block_summaries[b][i];
      if (!isum.valid) {
        continue;
      }
      bool insn_changed = false;
      if (isum.is_sequence) {
        if (isum.is_call_seq) {
          if (MarkExitReachingReturnSetsInExpr(&isum.head_pn.pattern, cur_ret_need, df.gp64,
                                               df.fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.head.defs;
          if (MarkExitReachingReturnSetsInExpr(&isum.slot_pn.pattern, cur_ret_need, df.gp64,
                                               df.fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.slot.defs;

          const RegMask before_call = (cur_live & ~isum.head.defs) | isum.head.uses;
          if (attach_notes &&
              AttachNotesToElem(&isum.slot_pn, isum.slot, before_call, fixed_mask, df.gp64, df.fp64,
                                /*in_delay_slot=*/true)) {
            insn_changed = true;
          }
          cur_live = (before_call & ~isum.slot.defs) | isum.slot.uses;
        } else {
          RegMask slot_after_live = cur_live;
          RegMask before_slot = (cur_live & ~isum.slot.defs) | isum.slot.uses;
          RegMask slot_ret_need = cur_ret_need;
          if (isum.is_annulled_target_seq && has_fallthrough_edge(b)) {
            RegMask taken_live = exit_live_for_block(b);
            RegMask taken_ret_need = df.can_exit[b] ? exit_ret_mask : 0;
            for (size_t sb : cfg->rpo_blocks[b].successors) {
              if ((sb != b + 1 || taken_also_goes_to_ft(b)) && sb < n_blocks) {
                taken_live |= df.live_in[sb];
                taken_ret_need |= ret_need_in[sb];
              }
            }
            slot_after_live = taken_live;
            before_slot = ((taken_live & ~isum.slot.defs) | isum.slot.uses) | df.live_in[b + 1];
            slot_ret_need = taken_ret_need;
            cur_ret_need = (taken_ret_need & ~isum.slot.defs) | ret_need_in[b + 1];
          } else {
            cur_ret_need &= ~isum.slot.defs;
          }

          if (MarkExitReachingReturnSetsInExpr(&isum.slot_pn.pattern, slot_ret_need, df.gp64,
                                               df.fp64)) {
            insn_changed = true;
          }
          if (MarkExitReachingReturnSetsInExpr(&isum.head_pn.pattern, cur_ret_need, df.gp64,
                                               df.fp64)) {
            insn_changed = true;
          }
          cur_ret_need &= ~isum.head.defs;

          if (attach_notes) {
            if (AttachNotesToElem(&isum.slot_pn, isum.slot, slot_after_live, fixed_mask, df.gp64,
                                  df.fp64, /*in_delay_slot=*/true)) {
              insn_changed = true;
            }
            if (AttachNotesToElem(&isum.head_pn, isum.head, before_slot, fixed_mask, df.gp64,
                                  df.fp64, /*in_delay_slot=*/false)) {
              insn_changed = true;
            }
          }
          cur_live = (before_slot & ~isum.head.defs) | isum.head.uses;
        }
        if (insn_changed) {
          cfg->rpo_blocks[b].instructions[i] =
              RtlExpr::MakeSequence(
                  {
                      RtlExpr::MakeInsnWrapper(isum.seq_head_wrap.op, isum.seq_head_wrap.flags,
                                               isum.head_pn),
                      RtlExpr::MakeInsnWrapper(isum.seq_slot_wrap.op, isum.seq_slot_wrap.flags,
                                               isum.slot_pn),
                  })
                  .ToGccPrintRtlString();
          block_changed = true;
        }
      } else {
        if (MarkExitReachingReturnSetsInExpr(&isum.top_pn.pattern, cur_ret_need, df.gp64,
                                             df.fp64)) {
          insn_changed = true;
        }
        cur_ret_need &= ~isum.single.defs;

        if (attach_notes &&
            AttachNotesToElem(&isum.top_pn, isum.single, cur_live, fixed_mask, df.gp64, df.fp64)) {
          insn_changed = true;
        }
        cur_live = (cur_live & ~isum.single.defs) | isum.single.uses;
        if (insn_changed) {
          cfg->rpo_blocks[b].instructions[i] = isum.top_pn.ToGccPrintRtlString();
          block_changed = true;
        }
      }
    }
    if (block_changed) {
      cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
    }
  }
  apply_call_fusage_to_blocks();

  // Insert the trailing `(use (reg/i:<mode> <regno>))` pseudo-instruction when
  // the function returns a value and the final RPO block can reach exit.
  if (exit_ret_mask != 0 && df.can_exit[n_blocks - 1] && !HasExistingReturnUse(*cfg)) {
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

    if (ret_regno >= 0) {
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
              if (pslot.ok() &&
                  (!has_prev_last_active || !(pslot->pattern == prev_last_active_pat))) {
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
      if (ends_with_ret_jump && !ends_with_ret_seq && last_blk.has_label &&
          !all_jumpers_to_last_are_annulled_non_t1_steals) {
        // In `reorg.c:find_end_label` (`reorg.c:820-834`), a shared `end_of_function_label`
        // before a leaf `RETURN` is emitted AFTER the function's trailing `(use (reg/i:...))`,
        // so `last_blk` (`end_of_function_label:`) must never begin with `(use ...)` ahead of
        // `RETURN` (otherwise `reorg.c:get_label_before(next_active_insn(end_label))` splits
        // `end_of_function_label` during forward replay). Place `(use ...)` on the fallthrough
        // into `last_blk` when `n_blocks - 2` falls through to `last_blk`.
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
          last_ends_with_jump =
              last_parsed.ok() && IsAnyControlTransferPattern(last_parsed->pattern);
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

      if (inserted_use && attach_notes) {
        const DataflowResult updated_df = RunHardRegDataflow(*cfg, &profile);
        WriteLiveInToSideband(updated_df, cfg);
      }
    }
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
    return RtlExpr::MakeReg(fallback_mode, target_regno);
  };

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

        // First resolve deaths that genuinely occur on instruction `i` itself.
        collect_tier(esum.uses & ~esum.defs & ~cur_live & ~fixed_mask);
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
                blk.instructions[j] = jsum.top_pn.ToGccPrintRtlString();
                if (blk.rtl_insns.size() == blk.instructions.size()) {
                  blk.rtl_insns[j].canonical_pattern = blk.instructions[j];
                } else {
                  block_changed = true;
                }
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

        size_t cand_idx = 0;
        std::vector<RtlRegNote> updated_notes;
        updated_notes.reserve(isum.top_pn.reg_notes.size());
        for (RtlRegNote& note : isum.top_pn.reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.IsConstInt() &&
              note.datum.int_val == 0) {
            if (cand_idx < candidates.size()) {
              note.datum = candidates[cand_idx++];
              updated_notes.push_back(std::move(note));
            }
          } else {
            updated_notes.push_back(std::move(note));
          }
        }
        isum.top_pn.reg_notes = std::move(updated_notes);
        blk.instructions[i] = isum.top_pn.ToGccPrintRtlString();
        if (blk.rtl_insns.size() == blk.instructions.size()) {
          blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
        } else {
          block_changed = true;
        }
      }
      cur_live = (cur_live & ~esum.defs) | esum.uses;
    }
    if (block_changed) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

void ResolvePostReloadConstZeroRegDeadNotes(RtlControlFlowGraph* cfg) {
  const CompilationProfile default_profile{.optimize_level = 2};
  ResolvePostReloadConstZeroRegDeadNotes(cfg, default_profile);
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

}  // namespace rom_nom_nom::lifter
