#include "lifter/mips_reg_alloc_common.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

enum class MipsMatchScratchRuleKind {
  kSrcOp,
  kSrcOpContainingMult,
  kBlkMemCopy,
};

struct MipsMatchScratchRule {
  MipsMatchScratchRuleKind kind;
  RtlOp src_op;
  const char* mips_md_citation;
};

// Exhaustive table of GCC 2.7.2 `config/mips/mips.md` `define_insn` `PARALLEL`
// families carrying `(clobber (match_scratch ...))` operands.
// Note: `define_expand "casesi"` (`config/mips/mips.md:5946-5986`,
// `TARGET_EMBEDDED_PIC`) lists `(clobber (match_scratch:SI 6 ""))` at line 5963
// in its template, but unconditionally expands via
// `gen_casesi_internal(..., gen_reg_rtx(SImode))` (line 5982) into
// `define_insn "casesi_internal"` (`config/mips/mips.md:6001-6018`), which uses
// `(clobber (match_operand:SI 2 "register_operand" "d"))` (line 6006) rather
// than `match_scratch`. It is therefore excluded from this table.
constexpr std::array<MipsMatchScratchRule, 10> kMipsMatchScratchRules = {{
    // `mulsi3_internal` (848-853), `mulsi3_r4000` (860-866), `mulsi3_r4650`
    // (883-889), `muldi3_internal` (912-917), `muldi3_r4000` (924-930),
    // `mulsidi3_internal` (963-967), `mulsidi3_64bit` (974-979),
    // `umulsidi3_internal` (1014-1018), `umulsidi3_64bit` (1025-1030).
    {MipsMatchScratchRuleKind::kSrcOp, RtlOp::kMult, "config/mips/mips.md:848-1030"},
    // `smulsi3_highpart` (986-993), `umulsi3_highpart` (1037-1044),
    // `smuldi3_highpart` (1051-1058), `umuldi3_highpart` (1065-1072).
    {MipsMatchScratchRuleKind::kSrcOpContainingMult, RtlOp::kTruncate,
     "config/mips/mips.md:986-1072"},
    // `madsi` (1082-1088), `maddi` (1095-1102), `maddi_64bit` (1109-1117),
    // `umaddi` (1124-1131), `umaddi_64bit` (1138-1146).
    {MipsMatchScratchRuleKind::kSrcOpContainingMult, RtlOp::kPlus, "config/mips/mips.md:1082-1146"},
    // `divmodsi4` (1285-1294), `divmoddi4` (1310-1319), `divsi3` (1385-1391),
    // `divdi3` (1398-1404).
    {MipsMatchScratchRuleKind::kSrcOp, RtlOp::kDiv, "config/mips/mips.md:1285-1404"},
    // `modsi3` (1411-1417), `moddi3` (1424-1430).
    {MipsMatchScratchRuleKind::kSrcOp, RtlOp::kMod, "config/mips/mips.md:1411-1430"},
    // `udivmodsi4` (1335-1344), `udivmoddi4` (1360-1369), `udivsi3`
    // (1437-1443), `udivdi3` (1450-1456).
    {MipsMatchScratchRuleKind::kSrcOp, RtlOp::kUdiv, "config/mips/mips.md:1335-1456"},
    // `umodsi3` (1463-1469), `umoddi3` (1476-1482).
    {MipsMatchScratchRuleKind::kSrcOp, RtlOp::kUmod, "config/mips/mips.md:1463-1482"},
    // `ffssi2` (1596-1600), `ffsdi2` (1632-1636).
    {MipsMatchScratchRuleKind::kSrcOp, RtlOp::kFfs, "config/mips/mips.md:1596-1636"},
    // `fix_truncdfsi2` (2485-2489), `fix_truncsfsi2` (2510-2514),
    // `fix_truncdfdi2` (2544-2547), `fix_truncsfdi2` (2571-2574).
    {MipsMatchScratchRuleKind::kSrcOp, RtlOp::kFix, "config/mips/mips.md:2485-2574"},
    // `movstrsi_internal` (3587-3593), `movstrsi_internal2` (3646-3652),
    // `movstrsi_internal3` (3662-3668).
    {MipsMatchScratchRuleKind::kBlkMemCopy, RtlOp::kMem, "config/mips/mips.md:3587-3668"},
}};

bool SetMatchesScratchRule(const RtlExpr& dst, const RtlExpr& src,
                           const MipsMatchScratchRule& rule) {
  switch (rule.kind) {
    case MipsMatchScratchRuleKind::kSrcOp:
      return src.op == rule.src_op;
    case MipsMatchScratchRuleKind::kSrcOpContainingMult:
      return src.op == rule.src_op && ExprContainsOp(src, RtlOp::kMult);
    case MipsMatchScratchRuleKind::kBlkMemCopy:
      return dst.op == RtlOp::kMem && dst.mode == RtlMachineMode::kBlk && src.op == RtlOp::kMem &&
             src.mode == RtlMachineMode::kBlk;
  }
  return false;
}

}  // namespace

ParsedCompilerFlags ParseProfileFlags(const CompilationProfile& profile) {
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  return ParsedCompilerFlags::Parse(effective_flags);
}

MipsTargetConfig ExtractMipsTargetConfig(const ParsedCompilerFlags& flags) {
  MipsTargetConfig cfg;
  cfg.single_float = flags.flag_single_float;
  cfg.omit_frame_pointer = flags.flag_omit_frame_pointer;
  cfg.caller_saves = flags.flag_caller_saves;
  // Mirrors `toplev.c:3028-3034` and `sched.c:4911-4966`: when `sched1` runs,
  // `schedule_insns` overwrites positive `reg_live_length[r]` with
  // `sched_reg_live_length[r]` while preserving `reg_n_refs[r]`.
  cfg.runs_sched1 = flags.RunsSched1();
  int mips_isa = 1;
  std::string cpu_str;
  for (const std::string& tok : flags.raw_tokens) {
    if (tok == "-mgp64") {
      cfg.gp_64bit = true;
    } else if (tok == "-mgp32") {
      cfg.gp_64bit = false;
    } else if (tok == "-mfp64") {
      cfg.float_64bit = true;
    } else if (tok == "-mfp32") {
      cfg.float_64bit = false;
    } else if (tok == "-msoft-float") {
      cfg.soft_float = true;
    } else if (tok == "-mhard-float") {
      cfg.soft_float = false;
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
    } else if (absl::StartsWith(tok, "-mcpu=")) {
      cpu_str = std::string(absl::StripPrefix(tok, "-mcpu="));
    }
  }
  // Mirror `config/mips/mips.c:3146-3270` (`override_options`) and
  // `config/mips/mips.h:2928-2929` (`MEMORY_MOVE_COST`): `MEMORY_MOVE_COST` is
  // `6` for `PROCESSOR_R4000` / `PROCESSOR_R6000` and `4` otherwise.
  if (cpu_str.empty() || cpu_str == "default" || cpu_str == "DEFAULT") {
    cfg.mem_move_cost_6 = (mips_isa == 2 || mips_isa == 3);
  } else {
    std::string p = cpu_str;
    if (!p.empty() && (p[0] == 'r' || p[0] == 'R')) {
      p.erase(0, 1);
    }
    cfg.mem_move_cost_6 = (p == "4000" || p == "4k" || p == "4K" || p == "4400" || p == "6000" ||
                           p == "6k" || p == "6K");
  }
  return cfg;
}

HardRegSet RegClassContents(MipsRegClass cls) {
  switch (cls) {
    case MipsRegClass::kNoRegs:
      return {0, 0};
    case MipsRegClass::kGrRegs:
      return {0xffffffffULL, 0};
    case MipsRegClass::kFpRegs:
      return {0xffffffff00000000ULL, 0};
    case MipsRegClass::kHiReg:
      return {0, 0x1ULL};
    case MipsRegClass::kLoReg:
      return {0, 0x2ULL};
    case MipsRegClass::kHiloReg:
      return {0, 0x4ULL};
    case MipsRegClass::kMdRegs:
      return {0, 0x3ULL};
    case MipsRegClass::kStRegs:
      return {0, 0x8ULL};
    case MipsRegClass::kAllRegs:
      return {0xffffffffffffffffULL, 0xfULL};
  }
  return {0, 0};
}

MipsRegClass RegNoRegClass(int regno) {
  if (regno >= 0 && regno < kFirstFprRegnum) {
    return MipsRegClass::kGrRegs;
  }
  if (regno >= kFirstFprRegnum && regno <= kLastFprRegnum) {
    return MipsRegClass::kFpRegs;
  }
  if (regno == kHiRegnum) {
    return MipsRegClass::kHiReg;
  }
  if (regno == kLoRegnum) {
    return MipsRegClass::kLoReg;
  }
  if (regno == kHiloRegnum) {
    return MipsRegClass::kHiloReg;
  }
  if (regno == kFpswRegnum) {
    return MipsRegClass::kStRegs;
  }
  return MipsRegClass::kNoRegs;
}

bool RegClassSubsetP(MipsRegClass c1, MipsRegClass c2) {
  const HardRegSet s1 = RegClassContents(c1);
  const HardRegSet s2 = RegClassContents(c2);
  return (s1.lo & ~s2.lo) == 0 && ((s1.hi & 0xfULL) & ~(s2.hi & 0xfULL)) == 0;
}

bool RegClassRelatedP(MipsRegClass c1, MipsRegClass c2) {
  return c1 == c2 || RegClassSubsetP(c1, c2) || RegClassSubsetP(c2, c1);
}

int ModeByteSize(const RegModeInfo& info) {
  if (info.mode == RtlMachineMode::kUnknown) {
    if (info.raw_mode == "SC" || info.raw_mode == "CDI") {
      return 8;
    }
    if (info.raw_mode == "DC" || info.raw_mode == "CTI") {
      return 16;
    }
    if (info.raw_mode == "CC_FP" || info.raw_mode == "CC_REV_FP") {
      return 4;
    }
  }
  switch (info.mode) {
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

bool IsFpCcMode(const RegModeInfo& info) {
  return info.mode == RtlMachineMode::kCc || info.raw_mode == "CC_FP" ||
         info.raw_mode == "CC_REV_FP";
}

bool IsFloatOrComplexFloatMode(const RegModeInfo& info) {
  return info.mode == RtlMachineMode::kSf || info.mode == RtlMachineMode::kDf ||
         info.mode == RtlMachineMode::kTf || info.raw_mode == "SC" || info.raw_mode == "DC";
}

bool IsIntMode(const RegModeInfo& info) {
  return info.mode == RtlMachineMode::kQi || info.mode == RtlMachineMode::kHi ||
         info.mode == RtlMachineMode::kSi || info.mode == RtlMachineMode::kDi ||
         info.mode == RtlMachineMode::kTi;
}

int HardRegnoNregs(int regno, const RegModeInfo& info, const MipsTargetConfig& cfg) {
  const int size = ModeByteSize(info);
  if (regno >= kFirstFprRegnum && regno <= kLastFprRegnum) {
    const int units_per_fpreg = cfg.float_64bit ? 8 : 4;
    return std::max(1, (size + units_per_fpreg - 1) / units_per_fpreg);
  }
  const int units_per_word = cfg.gp_64bit ? 8 : 4;
  return std::max(1, (size + units_per_word - 1) / units_per_word);
}

int PseudoRegnoSize(const RegModeInfo& info, const MipsTargetConfig& cfg) {
  const int size = ModeByteSize(info);
  const int units_per_word = cfg.gp_64bit ? 8 : 4;
  return std::max(1, (size + units_per_word - 1) / units_per_word);
}

bool HardRegnoModeOk(int regno, const RegModeInfo& info, const MipsTargetConfig& cfg) {
  const int size = ModeByteSize(info);
  const int units_per_word = cfg.gp_64bit ? 8 : 4;
  if (IsFpCcMode(info)) {
    return regno == kFpswRegnum;
  }
  if (regno >= 0 && regno < kFirstFprRegnum) {
    return (regno & 1) == 0 || size <= units_per_word;
  }
  if (regno >= kFirstFprRegnum && regno <= kLastFprRegnum) {
    return !cfg.soft_float && (cfg.float_64bit || (regno & 1) == 0) &&
           IsFloatOrComplexFloatMode(info) && (!cfg.single_float || size <= 4);
  }
  if (regno >= kHiRegnum && regno <= kHiloRegnum) {
    return IsIntMode(info) &&
           (size <= units_per_word || (regno == kHiRegnum && size == 2 * units_per_word));
  }
  return false;
}

HardRegSet FixedRegSet(const MipsTargetConfig& cfg) {
  HardRegSet s;
  for (int r : {0, 1, 26, 27, 28, 29, 31, 67}) {
    s.SetBit(r);
  }
  if (cfg.soft_float) {
    for (int r = kFirstFprRegnum; r <= kLastFprRegnum; ++r) {
      s.SetBit(r);
    }
  }
  return s;
}

HardRegSet CallUsedRegSet(const MipsTargetConfig& cfg) {
  HardRegSet s = FixedRegSet(cfg);
  for (int r = 2; r <= 15; ++r) {
    s.SetBit(r);
  }
  s.SetBit(24);
  s.SetBit(25);
  for (int r = 32; r <= 51; ++r) {
    s.SetBit(r);
  }
  for (int r = 64; r <= 67; ++r) {
    s.SetBit(r);
  }
  return s;
}

HardRegSet EliminableRegSet() {
  HardRegSet s;
  s.SetBit(kArgPointerRegnum);
  s.SetBit(kFramePointerRegnum);
  return s;
}

int FloorLog2(int x) {
  if (x <= 0) {
    return -1;
  }
  return 31 - __builtin_clz(static_cast<unsigned int>(x));
}

int ComputeAllocnoPriority(int n_refs, int live_length, int size) {
  if (size == 0) {
    size = 1;
  }
  if (live_length == 0) {
    live_length = -1;
  }
  return static_cast<int>((static_cast<double>(FloorLog2(n_refs) * n_refs) / live_length) *
                          10000.0 * size);
}

bool IsMipsMatchScratchParallel(const RtlExpr& parallel_expr) {
  if (parallel_expr.op != RtlOp::kParallel || ExprContainsOp(parallel_expr, RtlOp::kCall) ||
      ExprContainsOp(parallel_expr, RtlOp::kUnspecVolatile) ||
      ExprContainsOp(parallel_expr, RtlOp::kAsmOperands)) {
    return false;
  }
  bool has_clobber = false;
  for (const RtlExpr& elem : parallel_expr.children) {
    if (elem.op == RtlOp::kClobber) {
      has_clobber = true;
      break;
    }
  }
  if (!has_clobber) {
    return false;
  }
  for (const RtlExpr& elem : parallel_expr.children) {
    if (elem.op != RtlOp::kSet || elem.children.size() != 2) {
      continue;
    }
    const RtlExpr& dst = elem.children[0];
    const RtlExpr& src = elem.children[1];
    for (const MipsMatchScratchRule& rule : kMipsMatchScratchRules) {
      if (SetMatchesScratchRule(dst, src, rule)) {
        return true;
      }
    }
  }
  return false;
}

void CollectUsedPseudoRegsInExpr(const RtlExpr& expr, bool in_mem_addr,
                                 absl::btree_set<int>* used_pseudos,
                                 absl::btree_map<int, int>* read_counts,
                                 absl::btree_set<int>* mem_addr_pseudos) {
  if (expr.op == RtlOp::kReg) {
    if (!expr.IsArgPointerReg() && expr.RegNo() >= kFirstPseudoRegister) {
      const int regno = expr.RegNo();
      used_pseudos->insert(regno);
      if (read_counts != nullptr) {
        ++(*read_counts)[regno];
      }
      if (in_mem_addr && mem_addr_pseudos != nullptr) {
        mem_addr_pseudos->insert(regno);
      }
    }
    return;
  }
  const bool child_in_mem_addr = in_mem_addr || expr.op == RtlOp::kMem;
  for (const RtlExpr& child : expr.children) {
    CollectUsedPseudoRegsInExpr(child, child_in_mem_addr, used_pseudos, read_counts,
                                mem_addr_pseudos);
  }
}

void CollectInsnElementUsesAndDefs(const RtlExpr& elem, absl::btree_set<int>* used_pseudos,
                                   absl::btree_set<int>* defined_pseudos,
                                   absl::btree_map<int, int>* read_counts,
                                   absl::btree_set<int>* mem_addr_pseudos) {
  if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
    const RtlExpr& dst = elem.children[0];
    const RtlExpr* inner_dst = &dst;
    bool is_bitfield_extract = false;
    while (inner_dst->op == RtlOp::kSubreg || inner_dst->op == RtlOp::kStrictLowPart ||
           inner_dst->op == RtlOp::kZeroExtract || inner_dst->op == RtlOp::kSignExtract) {
      if (inner_dst->op == RtlOp::kZeroExtract || inner_dst->op == RtlOp::kSignExtract) {
        is_bitfield_extract = true;
        for (size_t i = 1; i < inner_dst->children.size(); ++i) {
          CollectUsedPseudoRegsInExpr(inner_dst->children[i], false, used_pseudos, read_counts,
                                      mem_addr_pseudos);
        }
      }
      if (inner_dst->children.empty()) {
        break;
      }
      inner_dst = &inner_dst->children[0];
    }

    if (inner_dst->op == RtlOp::kReg) {
      if (!inner_dst->IsArgPointerReg() && inner_dst->RegNo() >= kFirstPseudoRegister) {
        defined_pseudos->insert(inner_dst->RegNo());
        if (is_bitfield_extract) {
          used_pseudos->insert(inner_dst->RegNo());
          if (read_counts != nullptr) {
            ++(*read_counts)[inner_dst->RegNo()];
          }
        }
      }
    } else {
      CollectUsedPseudoRegsInExpr(dst, false, used_pseudos, read_counts, mem_addr_pseudos);
    }

    for (size_t i = 1; i < elem.children.size(); ++i) {
      CollectUsedPseudoRegsInExpr(elem.children[i], false, used_pseudos, read_counts,
                                  mem_addr_pseudos);
    }
    return;
  }

  if (elem.op == RtlOp::kParallel) {
    for (const RtlExpr& sub : elem.children) {
      CollectInsnElementUsesAndDefs(sub, used_pseudos, defined_pseudos, read_counts,
                                    mem_addr_pseudos);
    }
    return;
  }

  CollectUsedPseudoRegsInExpr(elem, false, used_pseudos, read_counts, mem_addr_pseudos);
}

void CollectSingleRegMdOutputClasses(const RtlExpr& pat, const MipsTargetConfig& mips_cfg,
                                     absl::btree_map<int, MipsRegClass>* single_md_dests) {
  auto check_highpart_or_64bit_mulsidi = [&](const RtlExpr& elem) {
    if (elem.op != RtlOp::kSet || elem.children.size() != 2 || elem.children[0].op != RtlOp::kReg ||
        elem.children[0].IsArgPointerReg() || elem.children[0].RegNo() < kFirstPseudoRegister) {
      return;
    }
    const int dst_regno = elem.children[0].RegNo();
    const RtlExpr& src = elem.children[1];
    // `smulsi3_highpart` / `umulsi3_highpart`: `(truncate:SI (lshiftrt:DI (mult:DI ...) 32))`
    // has output constraint `"=h"` (`HI_REG`, size 1, `config/mips/mips.md:986-1072`).
    if (src.op == RtlOp::kTruncate && src.children.size() == 1 &&
        src.children[0].op == RtlOp::kLshiftrt && src.children[0].children.size() == 2 &&
        src.children[0].children[0].op == RtlOp::kMult) {
      (*single_md_dests)[dst_regno] = MipsRegClass::kHiReg;
      return;
    }
    // `mulsidi3_64bit` / `umulsidi3_64bit` (`TARGET_64BIT`): output constraint
    // `"=a"` (`HILO_REG`, size 1, `config/mips/mips.md:974-979, 1025-1030`).
    if (mips_cfg.gp_64bit && elem.children[0].mode == RtlMachineMode::kDi &&
        src.op == RtlOp::kMult && src.mode == RtlMachineMode::kDi) {
      (*single_md_dests)[dst_regno] = MipsRegClass::kHiloReg;
      return;
    }
  };

  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& sub : pat.children) {
      check_highpart_or_64bit_mulsidi(sub);
    }
    // `mulsi3_internal` / `muldi3_internal` (`mips_cpu != R4000`): 3-element
    // `parallel [(set (reg p) (mult:SI/DI ...)) (clobber hi) (clobber lo)]`
    // has output constraint `"=l"` (`LO_REG`, size 1,
    // `config/mips/mips.md:848-853, 912-917`). By contrast, `mulsi3_r4000`
    // (`config/mips/mips.md:860-866`) has 4 elements and `"=d"`.
    if (pat.children.size() == 3 && pat.children[0].op == RtlOp::kSet &&
        pat.children[0].children.size() == 2 && pat.children[0].children[0].op == RtlOp::kReg &&
        !pat.children[0].children[0].IsArgPointerReg() &&
        pat.children[0].children[0].RegNo() >= kFirstPseudoRegister &&
        pat.children[0].children[1].op == RtlOp::kMult && pat.children[1].op == RtlOp::kClobber &&
        pat.children[2].op == RtlOp::kClobber &&
        (pat.children[0].children[0].mode != RtlMachineMode::kDi || mips_cfg.gp_64bit)) {
      (*single_md_dests)[pat.children[0].children[0].RegNo()] = MipsRegClass::kLoReg;
    }
  } else {
    check_highpart_or_64bit_mulsidi(pat);
  }
}

void CollectDiMultDestRegs(const RtlExpr& pat, absl::btree_set<int>* mult_di_dests,
                           bool* has_hard_non_hi_dest) {
  auto check_elem = [&](const RtlExpr& elem) {
    if (elem.op == RtlOp::kSet && elem.children.size() == 2 && elem.children[0].op == RtlOp::kReg &&
        elem.children[0].mode == RtlMachineMode::kDi && elem.children[1].op == RtlOp::kMult &&
        elem.children[1].mode == RtlMachineMode::kDi && !elem.children[0].IsArgPointerReg()) {
      if (elem.children[0].RegNo() >= kFirstPseudoRegister) {
        mult_di_dests->insert(elem.children[0].RegNo());
      } else if (has_hard_non_hi_dest != nullptr && elem.children[0].RegNo() != kHiRegnum) {
        *has_hard_non_hi_dest = true;
      }
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& sub : pat.children) {
      check_elem(sub);
    }
  } else {
    check_elem(pat);
  }
}

bool ExprContainsOp(const RtlExpr& root, RtlOp target_op) {
  bool found = false;
  root.Visit([&](const RtlExpr& n) {
    if (n.op == target_op) {
      found = true;
    }
  });
  return found;
}

void CollectHardAndScratchMdNeeds(const RtlExpr& pat, const MipsTargetConfig& mips_cfg,
                                  HardRegSet* needs) {
  if (pat.op != RtlOp::kParallel || pat.children.size() < 2 ||
      ExprContainsOp(pat, RtlOp::kAsmOperands)) {
    return;
  }
  auto is_clobber_scratch = [](const RtlExpr& e) {
    return e.op == RtlOp::kClobber && e.children.size() == 1 && e.children[0].op == RtlOp::kScratch;
  };
  const RtlExpr& e0 = pat.children[0];
  if (e0.op == RtlOp::kSet && e0.children.size() == 2) {
    const RtlExpr& dst = e0.children[0];
    const RtlExpr& src = e0.children[1];
    const int dst_hr =
        (dst.op == RtlOp::kReg && !dst.IsArgPointerReg() && dst.RegNo() < kFirstPseudoRegister)
            ? dst.RegNo()
            : -1;
    // `smulsi3_highpart` / `umulsi3_highpart` / `smuldi3_highpart` / `umuldi3_highpart`:
    // op0 `"=h"` (`64`), op3 `"=l"` (`65`), op4 `"=a"` (`66`) (`config/mips/mips.md:986-1072`).
    if (src.op == RtlOp::kTruncate && src.children.size() == 1 &&
        src.children[0].op == RtlOp::kLshiftrt && src.children[0].children.size() == 2 &&
        src.children[0].children[0].op == RtlOp::kMult) {
      if (dst_hr >= 0 && dst_hr != kHiRegnum) {
        needs->SetBit(kHiRegnum);
      }
      if (pat.children.size() >= 2 && is_clobber_scratch(pat.children[1])) {
        needs->SetBit(kLoRegnum);
      }
      if (pat.children.size() >= 3 && is_clobber_scratch(pat.children[2])) {
        needs->SetBit(kHiloRegnum);
      }
      return;
    }
    // `mulsidi3` / `umulsidi3` (`config/mips/mips.md:963-979, 1014-1030`):
    if (dst.mode == RtlMachineMode::kDi && src.op == RtlOp::kMult &&
        src.mode == RtlMachineMode::kDi) {
      if (!mips_cfg.gp_64bit) {
        if (dst_hr >= 0 && dst_hr != kHiRegnum) {
          needs->SetBit(kHiRegnum);
          needs->SetBit(kLoRegnum);
        }
        if (pat.children.size() >= 2 && is_clobber_scratch(pat.children[1])) {
          needs->SetBit(kHiloRegnum);
        }
      } else {
        if (dst_hr >= 0 && dst_hr != kHiloRegnum) {
          needs->SetBit(kHiloRegnum);
        }
        if (pat.children.size() >= 2 && is_clobber_scratch(pat.children[1])) {
          needs->SetBit(kLoRegnum);
        }
        if (pat.children.size() >= 3 && is_clobber_scratch(pat.children[2])) {
          needs->SetBit(kHiRegnum);
        }
      }
      return;
    }
    // `mulsi3_internal` / `muldi3_internal` (3-element) vs `mulsi3_r4000` (4-element)
    // (`config/mips/mips.md:848-930`):
    if (src.op == RtlOp::kMult) {
      if (pat.children.size() == 3) {
        if (dst_hr >= 0 && dst_hr != kLoRegnum) {
          needs->SetBit(kLoRegnum);
        }
        if (is_clobber_scratch(pat.children[1])) {
          needs->SetBit(kHiRegnum);
        }
        if (is_clobber_scratch(pat.children[2])) {
          needs->SetBit(kHiloRegnum);
        }
      } else if (pat.children.size() >= 4) {
        if (is_clobber_scratch(pat.children[1])) {
          needs->SetBit(kHiRegnum);
        }
        if (is_clobber_scratch(pat.children[2])) {
          needs->SetBit(kLoRegnum);
        }
        if (is_clobber_scratch(pat.children[3])) {
          needs->SetBit(kHiloRegnum);
        }
      }
      return;
    }
    // `divmodsi4` / `udivmodsi4` / `divmoddi4` / `udivmoddi4` (5-element)
    // (`config/mips/mips.md:1285-1369`):
    if ((src.op == RtlOp::kDiv || src.op == RtlOp::kUdiv) && pat.children.size() >= 5) {
      if (is_clobber_scratch(pat.children[2])) {
        needs->SetBit(kLoRegnum);
      }
      if (is_clobber_scratch(pat.children[3])) {
        needs->SetBit(kHiRegnum);
      }
      if (is_clobber_scratch(pat.children[4])) {
        needs->SetBit(kHiloRegnum);
      }
      return;
    }
  }
}

bool InsnHardClobbersHiLoForLocalAlloc(const RtlExpr& pat) {
  bool clobbers = false;
  pat.Visit([&](const RtlExpr& expr) {
    if (clobbers) {
      return;
    }
    if (expr.op == RtlOp::kCall || expr.op == RtlOp::kCallInsn) {
      clobbers = true;
      return;
    }
    if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg() &&
        (expr.RegNo() == kHiRegnum || expr.RegNo() == kLoRegnum)) {
      clobbers = true;
    }
  });
  return clobbers;
}

bool InsnTouchesMdRegsOrCall(const RtlExpr& pat) {
  bool touches = false;
  pat.Visit([&](const RtlExpr& expr) {
    if (touches) {
      return;
    }
    if (expr.op == RtlOp::kCall || expr.op == RtlOp::kMult || expr.op == RtlOp::kDiv ||
        expr.op == RtlOp::kMod || expr.op == RtlOp::kUdiv || expr.op == RtlOp::kUmod) {
      touches = true;
      return;
    }
    if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg() && expr.RegNo() >= kHiRegnum &&
        expr.RegNo() <= kHiRegnum + 2) {
      touches = true;
    }
  });
  return touches;
}

bool HasSideEffects(const RtlExpr& expr) {
  if (expr.flags.volatil || expr.op == RtlOp::kCall || expr.op == RtlOp::kUnspecVolatile ||
      expr.op == RtlOp::kAsmOperands || expr.op == RtlOp::kAsmInput) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (HasSideEffects(child)) {
      return true;
    }
  }
  return false;
}

bool HasRegUnusedNote(const std::vector<RtlRegNote>& notes, const RtlExpr& dest) {
  for (const RtlRegNote& note : notes) {
    if (note.kind == RtlMachineMode::kRegUnused && note.datum == dest) {
      return true;
    }
  }
  return false;
}

bool HasRegUnusedNoteForRegno(const std::vector<RtlRegNote>& notes, int regno) {
  for (const RtlRegNote& note : notes) {
    if (note.kind == RtlMachineMode::kRegUnused && note.datum.op == RtlOp::kReg &&
        !note.datum.IsArgPointerReg() && note.datum.RegNo() == regno) {
      return true;
    }
  }
  return false;
}

const RtlExpr* SingleSet(const RtlPatternAndNotes& parsed) {
  if (parsed.pattern.op == RtlOp::kSet) {
    return &parsed.pattern;
  }
  if (parsed.pattern.op == RtlOp::kParallel) {
    const RtlExpr* found = nullptr;
    for (const RtlExpr& sub : parsed.pattern.children) {
      if (sub.op == RtlOp::kSet && !sub.children.empty() &&
          (!HasRegUnusedNote(parsed.reg_notes, sub.children[0]) || HasSideEffects(sub))) {
        if (found != nullptr) {
          return nullptr;
        }
        found = &sub;
      }
    }
    return found;
  }
  return nullptr;
}

const RtlExpr* UnwrapNoteStoresDest(const RtlExpr& orig_dest) {
  const RtlExpr* dest = &orig_dest;
  while ((dest->op == RtlOp::kSubreg && !dest->children.empty() &&
          (dest->children[0].op != RtlOp::kReg || dest->children[0].IsArgPointerReg() ||
           dest->children[0].RegNo() >= kFirstPseudoRegister)) ||
         dest->op == RtlOp::kZeroExtract || dest->op == RtlOp::kSignExtract ||
         dest->op == RtlOp::kStrictLowPart) {
    if (dest->children.empty()) {
      break;
    }
    dest = &dest->children[0];
  }
  return dest;
}

void NoteStores(const RtlExpr& pat,
                absl::FunctionRef<void(const RtlExpr& dest, const RtlExpr& setter)> fn) {
  if ((pat.op == RtlOp::kSet || pat.op == RtlOp::kClobber) && !pat.children.empty()) {
    fn(*UnwrapNoteStoresDest(pat.children[0]), pat);
    return;
  }
  if (pat.op == RtlOp::kParallel) {
    for (int i = static_cast<int>(pat.children.size()) - 1; i >= 0; --i) {
      const RtlExpr& y = pat.children[i];
      if ((y.op == RtlOp::kSet || y.op == RtlOp::kClobber) && !y.children.empty()) {
        fn(*UnwrapNoteStoresDest(y.children[0]), y);
      }
    }
  }
}

bool FirstFormatCharIsExpr(RtlOp op) {
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

bool ExprRefersToRegRange(int regno, int endregno, const RtlExpr& expr,
                          const MipsTargetConfig& mips_cfg) {
  if (expr.op == RtlOp::kReg) {
    const int r = expr.IsArgPointerReg() ? kArgPointerRegnum : expr.RegNo();
    const int nregs =
        r < kFirstPseudoRegister ? HardRegnoNregs(r, {expr.mode, expr.raw_mode}, mips_cfg) : 1;
    return endregno > r && regno < r + nregs;
  }
  if (expr.op == RtlOp::kSubreg && !expr.children.empty() && expr.children[0].op == RtlOp::kReg &&
      !expr.children[0].IsArgPointerReg() && expr.children[0].RegNo() < kFirstPseudoRegister) {
    const int inner_regno = expr.children[0].RegNo() + static_cast<int>(expr.int_val);
    const int inner_endregno =
        inner_regno + (inner_regno < kFirstPseudoRegister
                           ? HardRegnoNregs(regno, {expr.mode, expr.raw_mode}, mips_cfg)
                           : 1);
    return endregno > inner_regno && regno < inner_endregno;
  }
  for (const RtlExpr& child : expr.children) {
    if (ExprRefersToRegRange(regno, endregno, child, mips_cfg)) {
      return true;
    }
  }
  return false;
}

bool RegOverlapMentionedP(const RtlExpr& x, const RtlExpr& in, const MipsTargetConfig& mips_cfg) {
  int regno = -1;
  if (x.op == RtlOp::kSubreg && !x.children.empty() && x.children[0].op == RtlOp::kReg) {
    regno = x.children[0].IsArgPointerReg() ? kArgPointerRegnum : x.children[0].RegNo();
    if (regno < kFirstPseudoRegister) {
      regno += static_cast<int>(x.int_val);
    }
  } else if (x.op == RtlOp::kReg) {
    regno = x.IsArgPointerReg() ? kArgPointerRegnum : x.RegNo();
  } else {
    return false;
  }
  const int endregno =
      regno +
      (regno < kFirstPseudoRegister ? HardRegnoNregs(regno, {x.mode, x.raw_mode}, mips_cfg) : 1);
  return ExprRefersToRegRange(regno, endregno, in, mips_cfg);
}

MipsCfgRegAllocSummary AnalyzeCfgForMipsRegAlloc(const RtlControlFlowGraph& out,
                                                 const RtlControlFlowGraph& liveness_cfg,
                                                 const MipsTargetConfig& mips_cfg) {
  MipsCfgRegAllocSummary res;
  const size_t num_blocks = out.rpo_blocks.size();
  for (const auto& [bb_idx, live_set] : out.sideband.basic_block_live_at_start) {
    if (bb_idx >= 0 && static_cast<size_t>(bb_idx) < num_blocks) {
      for (int regno : live_set) {
        if (regno >= kFirstPseudoRegister) {
          res.live_at_any_block_start.insert(regno);
        }
      }
    }
  }

  res.block_insn_clobbers_md.resize(liveness_cfg.rpo_blocks.size());
  for (size_t b = 0; b < liveness_cfg.rpo_blocks.size(); ++b) {
    const RtlBasicBlock& blk = liveness_cfg.rpo_blocks[b];
    absl::btree_set<int> defined_so_far_in_block;
    absl::btree_set<int> died_in_block;
    res.block_insn_clobbers_md[b].resize(blk.instructions.size(), false);

    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }

      parsed->pattern.Visit([&](const RtlExpr& expr) {
        if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg() &&
            expr.RegNo() >= kFirstPseudoRegister) {
          res.pseudo_modes.try_emplace(expr.RegNo(), RegModeInfo{expr.mode, expr.raw_mode});
        }
        if ((expr.op == RtlOp::kFloat || expr.op == RtlOp::kUnsignedFloat ||
             expr.op == RtlOp::kFix || expr.op == RtlOp::kUnsignedFix ||
             expr.op == RtlOp::kFloatExtend || expr.op == RtlOp::kFloatTruncate ||
             expr.op == RtlOp::kSqrt || expr.op == RtlOp::kAbs ||
             ((expr.op == RtlOp::kPlus || expr.op == RtlOp::kMinus || expr.op == RtlOp::kMult ||
               expr.op == RtlOp::kDiv || expr.op == RtlOp::kNeg) &&
              IsFloatOrComplexFloatMode({expr.mode, expr.raw_mode})) ||
             ((expr.op == RtlOp::kEq || expr.op == RtlOp::kNe || expr.op == RtlOp::kLt ||
               expr.op == RtlOp::kLe || expr.op == RtlOp::kGt || expr.op == RtlOp::kGe) &&
              (expr.raw_mode == "CC_FP" || expr.raw_mode == "CC_REV_FP" ||
               (!expr.children.empty() &&
                IsFloatOrComplexFloatMode({expr.children[0].mode, expr.children[0].raw_mode})))))) {
          expr.Visit([&](const RtlExpr& sub) {
            if (sub.op == RtlOp::kReg && !sub.IsArgPointerReg() &&
                sub.RegNo() >= kFirstPseudoRegister) {
              res.fp_op_pseudos.insert(sub.RegNo());
            }
          });
        }
        if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
          const RtlExpr& d = expr.children[0];
          const RtlExpr& s = expr.children[1];
          if (d.op == RtlOp::kReg && !d.IsArgPointerReg() && d.RegNo() >= kFirstPseudoRegister &&
              ((s.op == RtlOp::kReg && !s.IsArgPointerReg() && s.RegNo() < kFirstFprRegnum) ||
               s.op == RtlOp::kMem)) {
            res.gr_or_mem_op_pseudos.insert(d.RegNo());
          }
          if (s.op == RtlOp::kReg && !s.IsArgPointerReg() && s.RegNo() >= kFirstPseudoRegister &&
              ((d.op == RtlOp::kReg && !d.IsArgPointerReg() && d.RegNo() < kFirstFprRegnum) ||
               d.op == RtlOp::kMem)) {
            res.gr_or_mem_op_pseudos.insert(s.RegNo());
          }
        }
        if (expr.op == RtlOp::kClobber && !expr.children.empty() &&
            expr.children[0].op == RtlOp::kReg && !expr.children[0].IsArgPointerReg() &&
            expr.children[0].RegNo() < kFirstPseudoRegister) {
          const int hr = expr.children[0].RegNo();
          const int nregs =
              HardRegnoNregs(hr, {expr.children[0].mode, expr.children[0].raw_mode}, mips_cfg);
          for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
            res.scratch_hard_regs.SetBit(k);
          }
        }
      });

      res.block_insn_clobbers_md[b][i] = InsnHardClobbersHiLoForLocalAlloc(parsed->pattern);
      absl::btree_set<int> insn_di_mult_dests;
      bool insn_has_hard_non_hi_di_mult = false;
      CollectDiMultDestRegs(parsed->pattern, &insn_di_mult_dests, &insn_has_hard_non_hi_di_mult);
      if (insn_has_hard_non_hi_di_mult) {
        ++res.num_hard_non_hi_di_mults;
      }
      for (int p : insn_di_mult_dests) {
        res.all_di_mult_pseudos.insert(p);
      }

      absl::btree_map<int, MipsRegClass> insn_single_md_dests;
      CollectSingleRegMdOutputClasses(parsed->pattern, mips_cfg, &insn_single_md_dests);

      absl::btree_set<int> insn_used_pseudos;
      absl::btree_set<int> insn_defined_pseudos;
      absl::btree_map<int, int> insn_read_counts;
      absl::btree_set<int> insn_mem_addr_pseudos;
      CollectInsnElementUsesAndDefs(parsed->pattern, &insn_used_pseudos, &insn_defined_pseudos,
                                    &insn_read_counts, &insn_mem_addr_pseudos);

      for (int regno : insn_used_pseudos) {
        PseudoLivenessSummary& summary = res.pseudo_summaries[regno];
        summary.blocks_referenced.insert(b);
        summary.total_read_count += insn_read_counts[regno];
        if (insn_mem_addr_pseudos.contains(regno)) {
          summary.used_in_mem_address = true;
        }
        if (!defined_so_far_in_block.contains(regno)) {
          summary.used_before_set = true;
        }
      }

      for (int regno : insn_defined_pseudos) {
        PseudoLivenessSummary& summary = res.pseudo_summaries[regno];
        summary.blocks_referenced.insert(b);
        summary.has_set_in_block = true;
        ++summary.def_count_in_block;
        ++summary.total_def_count;
        summary.def_insn_idx = i;
        if (insn_di_mult_dests.contains(regno)) {
          summary.defined_by_di_mult = true;
        }
        if (auto md_it = insn_single_md_dests.find(regno); md_it != insn_single_md_dests.end()) {
          summary.single_reg_md_class = md_it->second;
        }
        defined_so_far_in_block.insert(regno);
        if (died_in_block.contains(regno)) {
          summary.alive_after_redef = true;
        }
      }

      absl::btree_set<int> insn_note_dead_pseudos;
      for (const RtlRegNote& note : parsed->reg_notes) {
        note.datum.Visit([&](const RtlExpr& expr) {
          if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg() &&
              expr.RegNo() >= kFirstPseudoRegister) {
            res.pseudo_summaries[expr.RegNo()].blocks_referenced.insert(b);
            res.pseudo_modes.try_emplace(expr.RegNo(), RegModeInfo{expr.mode, expr.raw_mode});
          }
        });
        if ((note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) &&
            note.datum.op == RtlOp::kReg && !note.datum.IsArgPointerReg() &&
            note.datum.RegNo() >= kFirstPseudoRegister) {
          const int dead_regno = note.datum.RegNo();
          PseudoLivenessSummary& summary = res.pseudo_summaries[dead_regno];
          if (insn_note_dead_pseudos.insert(dead_regno).second) {
            ++summary.note_deaths;
          }
          summary.death_insn_idx = i;
          died_in_block.insert(dead_regno);
          summary.alive_after_redef = false;
        }
      }
    }
  }
  return res;
}

}  // namespace rom_nom_nom::lifter
