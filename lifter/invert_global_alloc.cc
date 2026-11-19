#include "lifter/invert_global_alloc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kArgPointerRegnum = 0;
constexpr int kFramePointerRegnum = 30;
constexpr int kFirstFprRegnum = 32;
constexpr int kLastFprRegnum = 63;
constexpr int kHiRegnum = 64;
constexpr int kLoRegnum = 65;
constexpr int kHiloRegnum = 66;
constexpr int kFpswRegnum = 67;
constexpr int kFirstPseudoRegister = kMipsFirstPseudoRegister;

struct MipsTargetConfig {
  bool gp_64bit = false;
  bool float_64bit = false;
  bool soft_float = false;
  bool single_float = false;
  bool omit_frame_pointer = true;
  bool caller_saves = true;
  bool mem_move_cost_6 = false;
};

MipsTargetConfig ExtractMipsTargetConfig(const ParsedCompilerFlags& flags) {
  MipsTargetConfig cfg;
  cfg.single_float = flags.flag_single_float;
  cfg.omit_frame_pointer = flags.flag_omit_frame_pointer;
  cfg.caller_saves = flags.flag_caller_saves;
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
  // Mirror `mips.c:override_options` (`3195-3270`) and `mips.h:MEMORY_MOVE_COST`
  // (`2928`): `MEMORY_MOVE_COST` is `6` for `PROCESSOR_R4000` / `PROCESSOR_R6000`
  // and `4` otherwise.
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

enum class MipsRegClass : int {
  kNoRegs = 0,
  kGrRegs = 1,
  kFpRegs = 2,
  kHiReg = 3,
  kLoReg = 4,
  kHiloReg = 5,
  kMdRegs = 6,
  kStRegs = 7,
  kAllRegs = 8,
};

struct HardRegSet {
  uint64_t lo = 0;
  uint64_t hi = 0;

  void Clear() {
    lo = 0;
    hi = 0;
  }
  void SetBit(int r) {
    if (r >= 0 && r < 64) {
      lo |= (uint64_t{1} << r);
    } else if (r >= 64 && r < kFirstPseudoRegister) {
      hi |= (uint64_t{1} << (r - 64));
    }
  }
  void ClearBit(int r) {
    if (r >= 0 && r < 64) {
      lo &= ~(uint64_t{1} << r);
    } else if (r >= 64 && r < kFirstPseudoRegister) {
      hi &= ~(uint64_t{1} << (r - 64));
    }
  }
  bool TestBit(int r) const {
    if (r >= 0 && r < 64) {
      return ((lo >> r) & 1) != 0;
    }
    if (r >= 64 && r < kFirstPseudoRegister) {
      return ((hi >> (r - 64)) & 1) != 0;
    }
    return false;
  }
  void Ior(const HardRegSet& other) {
    lo |= other.lo;
    hi |= other.hi;
  }
  void AndCompl(const HardRegSet& other) {
    lo &= ~other.lo;
    hi &= ~other.hi;
  }
  void IorCompl(const HardRegSet& other) {
    lo |= (~other.lo);
    hi |= ((~other.hi) & 0xfULL);
  }
  bool IsEmpty() const { return lo == 0 && (hi & 0xfULL) == 0; }
};

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

struct RegModeInfo {
  RtlMachineMode mode = RtlMachineMode::kSi;
  std::string raw_mode;
};

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

ParsedCompilerFlags ParseProfileFlags(const CompilationProfile& profile) {
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  return ParsedCompilerFlags::Parse(effective_flags);
}

// Recursively validates all `(reg ...)` nodes in `expr` and updates `max_regno`.
absl::Status ValidateRtlExprRegisters(const RtlExpr& expr, int* max_regno) {
  if (expr.op == RtlOp::kReg) {
    if (expr.int_val < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid negative register number %d", expr.int_val));
    }
    if (max_regno != nullptr && !expr.IsArgPointerReg()) {
      *max_regno = std::max(*max_regno, expr.RegNo() + 1);
    }
  }
  for (const RtlExpr& child : expr.children) {
    if (absl::Status st = ValidateRtlExprRegisters(child, max_regno); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

// Collects all pseudo-register numbers (`>= kFirstPseudoRegister`) read inside
// `expr`, along with total read counts and whether any read occurs inside a
// `MEM` address.
void CollectUsedPseudoRegsInExpr(const RtlExpr& expr, bool in_mem_addr,
                                 absl::flat_hash_set<int>* used_pseudos,
                                 absl::flat_hash_map<int, int>* read_counts,
                                 absl::flat_hash_set<int>* mem_addr_pseudos) {
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

// Collects pseudo-register numbers (`>= kFirstPseudoRegister`) read vs. written
// by a single top-level pattern or `PARALLEL` element, matching
// `flow.c:mark_set_regs` and `flow.c:mark_used_regs`.
void CollectInsnElementUsesAndDefs(const RtlExpr& elem, absl::flat_hash_set<int>* used_pseudos,
                                   absl::flat_hash_set<int>* defined_pseudos,
                                   absl::flat_hash_map<int, int>* read_counts,
                                   absl::flat_hash_set<int>* mem_addr_pseudos) {
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

struct PseudoLivenessSummary {
  absl::flat_hash_set<size_t> blocks_referenced;
  bool has_set_in_block = false;
  bool used_before_set = false;
  bool alive_after_redef = false;
  bool defined_by_di_mult = false;
  bool used_in_mem_address = false;
  MipsRegClass single_reg_md_class = MipsRegClass::kNoRegs;
  int def_count_in_block = 0;
  int total_def_count = 0;
  int total_read_count = 0;
  size_t def_insn_idx = 0;
  size_t death_insn_idx = 0;
  int note_deaths = 0;
};

// Identifies destinations of single-register MD output constraints (`"=h"`,
// `"=l"`, `"=a"`) where `CLASS_LIKELY_SPILLED_P(CLASS)` (`reg_class_size == 1`)
// holds in `local-alloc.c:472-474`.
void CollectSingleRegMdOutputClasses(const RtlExpr& pat, const MipsTargetConfig& mips_cfg,
                                     absl::flat_hash_map<int, MipsRegClass>* single_md_dests) {
  auto check_highpart_or_64bit_mulsidi = [&](const RtlExpr& elem) {
    if (elem.op != RtlOp::kSet || elem.children.size() != 2 || elem.children[0].op != RtlOp::kReg ||
        elem.children[0].IsArgPointerReg() || elem.children[0].RegNo() < kFirstPseudoRegister) {
      return;
    }
    const int dst_regno = elem.children[0].RegNo();
    const RtlExpr& src = elem.children[1];
    // `smulsi3_highpart` / `umulsi3_highpart`: `(truncate:SI (lshiftrt:DI (mult:DI ...) 32))`
    // has output constraint `"=h"` (`HI_REG`, size 1).
    if (src.op == RtlOp::kTruncate && src.children.size() == 1 &&
        src.children[0].op == RtlOp::kLshiftrt && src.children[0].children.size() == 2 &&
        src.children[0].children[0].op == RtlOp::kMult) {
      (*single_md_dests)[dst_regno] = MipsRegClass::kHiReg;
      return;
    }
    // `mulsidi3_64bit` / `umulsidi3_64bit` (`TARGET_64BIT`): output constraint
    // `"=a"` (`HILO_REG`, size 1).
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
    // has output constraint `"=l"` (`LO_REG`, size 1). By contrast,
    // `mulsi3_r4000` has 4 elements (3rd clobber of `scratch:SI`) and `"=d"`.
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

void CollectDiMultDestRegs(const RtlExpr& pat, absl::flat_hash_set<int>* mult_di_dests,
                           bool* has_hard_non_hi_dest = nullptr) {
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

// Records MD hard registers (`64 hi`, `65 lo`, `66 hilo`) needed by `reload`
// in `basic_block_needs` for hard non-MD `SET_DEST`s or unallocated
// `(clobber (scratch:...))` operands on `mips.md` multiply/divide `PARALLEL`s.
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
    // op0 `"=h"` (`64`), op3 `"=l"` (`65`), op4 `"=a"` (`66`).
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
    // `mulsidi3` / `umulsidi3`:
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
    // `mulsi3_internal` / `muldi3_internal` (3-element) vs `mulsi3_r4000` (4-element):
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
    // `divmodsi4` / `udivmodsi4` / `divmoddi4` / `udivmoddi4` (5-element):
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

// Mirrors `rtlanal.c:single_set`.
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

// Mirrors `rtlanal.c:note_stores` (including reverse iteration over `PARALLEL`
// elements).
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

struct BlockMdPseudoNeed {
  int regno = -1;
  HardRegSet required_md;
};

struct GlobalAllocModel {
  MipsTargetConfig mips_cfg;
  int max_regno = kFirstPseudoRegister;
  int max_allocno = 0;
  int num_blocks = 0;
  std::vector<int> reg_allocno;
  std::vector<int> local_reg_renumber;
  std::vector<RegModeInfo> pseudo_modes;
  std::vector<int> allocno_reg;
  std::vector<int> allocno_size;
  std::vector<int> allocno_calls_crossed;
  std::vector<int> allocno_n_refs;
  std::vector<int> allocno_live_length;
  std::vector<int> allocno_single_block;
  std::vector<MipsRegClass> pref_class;
  std::vector<MipsRegClass> alt_class;
  std::vector<int> target_hr;

  HardRegSet fixed_regs;
  HardRegSet call_used_regs;
  HardRegSet no_global_alloc_regs;
  HardRegSet bad_spill_regs;
  HardRegSet base_regs_used_so_far;
  std::vector<int> reg_n_refs;
  std::vector<int> base_local_reg_n_refs;
  std::vector<int> base_local_reg_live_length;

  std::vector<HardRegSet> base_hard_reg_conflicts;
  std::vector<HardRegSet> base_hard_reg_preferences;
  std::vector<HardRegSet> base_hard_reg_copy_preferences;
  std::vector<HardRegSet> base_hard_reg_full_preferences;
  std::vector<absl::flat_hash_set<int>> allocno_md_clobber_blocks;
  std::vector<std::vector<BlockMdPseudoNeed>> block_md_pseudos;
  std::vector<HardRegSet> block_hard_md_needs;
  std::vector<std::vector<int>> non_move_read_pseudo_groups;
  // Asymmetric conflict table `conflicts[i * max_allocno + j]` matching
  // `CONFLICTP(i, j)` in `global.c:123`.
  std::vector<uint8_t> conflicts;

  bool ConflictP(int i, int j) const { return conflicts[i * max_allocno + j] != 0; }
  void SetConflict(int i, int j) { conflicts[i * max_allocno + j] = 1; }
};

struct SimResult {
  std::vector<int> allocno_hr;
  int primary_mismatches = 0;
  int secondary_mismatches = 0;
};

SimResult SimulateGlobalAlloc(const GlobalAllocModel& m, const std::vector<int>& order) {
  SimResult res;
  res.allocno_hr.assign(m.max_allocno, -1);
  if (m.max_allocno == 0) {
    return res;
  }

  std::vector<int> reg_renumber = m.local_reg_renumber;
  HardRegSet regs_used_so_far = m.base_regs_used_so_far;
  std::vector<int> local_reg_n_refs = m.base_local_reg_n_refs;
  const std::vector<int>& local_reg_live_length = m.base_local_reg_live_length;
  std::vector<HardRegSet> hard_reg_conflicts = m.base_hard_reg_conflicts;
  std::vector<HardRegSet> hard_reg_preferences = m.base_hard_reg_preferences;
  std::vector<HardRegSet> hard_reg_copy_preferences = m.base_hard_reg_copy_preferences;
  std::vector<HardRegSet> hard_reg_full_preferences = m.base_hard_reg_full_preferences;
  std::vector<HardRegSet> regs_someone_prefers(m.max_allocno);

  // `prune_preferences()` (`global.c:834-883`).
  for (int i = m.max_allocno - 1; i >= 0; --i) {
    const int allocno = order[i];
    HardRegSet temp = hard_reg_conflicts[allocno];
    if (m.allocno_calls_crossed[allocno] == 0) {
      temp.Ior(m.fixed_regs);
    } else {
      temp.Ior(m.call_used_regs);
    }
    temp.IorCompl(RegClassContents(m.pref_class[allocno]));

    hard_reg_preferences[allocno].AndCompl(temp);
    hard_reg_copy_preferences[allocno].AndCompl(temp);
    hard_reg_full_preferences[allocno].AndCompl(temp);
    regs_someone_prefers[allocno].Clear();

    for (int j = i + 1; j < m.max_allocno; ++j) {
      const int other = order[j];
      if (m.ConflictP(allocno, other)) {
        HardRegSet other_pref = hard_reg_full_preferences[other];
        if (m.allocno_size[other] <= m.allocno_size[allocno]) {
          other_pref.AndCompl(hard_reg_full_preferences[allocno]);
        }
        regs_someone_prefers[allocno].Ior(other_pref);
      }
    }
  }

  auto find_reg = [&](auto& self, int allocno, HardRegSet losers, bool alt_regs_p,
                      bool accept_call_clobbered, bool retrying) -> void {
    const MipsRegClass cls = alt_regs_p ? m.alt_class[allocno] : m.pref_class[allocno];
    const RegModeInfo& mode = m.pseudo_modes[m.allocno_reg[allocno]];

    HardRegSet used1 = (accept_call_clobbered || m.allocno_calls_crossed[allocno] == 0)
                           ? m.fixed_regs
                           : m.call_used_regs;
    used1.Ior(m.no_global_alloc_regs);
    used1.Ior(losers);
    used1.IorCompl(RegClassContents(cls));
    const HardRegSet used2 = used1;
    used1.Ior(hard_reg_conflicts[allocno]);

    HardRegSet used = used1;
    used.IorCompl(regs_used_so_far);
    used.Ior(regs_someone_prefers[allocno]);

    int best_reg = -1;
    int i = kFirstPseudoRegister;
    for (int pass = 0; pass <= 1 && i >= kFirstPseudoRegister; ++pass) {
      if (pass == 1) {
        used = used1;
      }
      for (i = 0; i < kFirstPseudoRegister; ++i) {
        const int regno = i;
        if (!used.TestBit(regno) && HardRegnoModeOk(regno, mode, m.mips_cfg)) {
          const int lim = regno + HardRegnoNregs(regno, mode, m.mips_cfg);
          int j = regno + 1;
          while (j < lim && !used.TestBit(j)) {
            ++j;
          }
          if (j == lim) {
            best_reg = regno;
            break;
          }
          i = j;
        }
      }
    }

    hard_reg_copy_preferences[allocno].AndCompl(used);
    bool took_copy_pref = false;
    if (!hard_reg_copy_preferences[allocno].IsEmpty() && best_reg >= 0) {
      for (int r = 0; r < kFirstPseudoRegister; ++r) {
        if (hard_reg_copy_preferences[allocno].TestBit(r) && HardRegnoModeOk(r, mode, m.mips_cfg) &&
            RegClassRelatedP(RegNoRegClass(r), RegNoRegClass(best_reg))) {
          const int lim = r + HardRegnoNregs(r, mode, m.mips_cfg);
          int j = r + 1;
          while (j < lim && !used.TestBit(j) &&
                 RegClassRelatedP(RegNoRegClass(j), RegNoRegClass(best_reg + (j - r)))) {
            ++j;
          }
          if (j == lim) {
            best_reg = r;
            took_copy_pref = true;
            break;
          }
        }
      }
    }

    if (!took_copy_pref) {
      hard_reg_preferences[allocno].AndCompl(used);
      if (!hard_reg_preferences[allocno].IsEmpty() && best_reg >= 0) {
        for (int r = 0; r < kFirstPseudoRegister; ++r) {
          if (hard_reg_preferences[allocno].TestBit(r) && HardRegnoModeOk(r, mode, m.mips_cfg) &&
              RegClassRelatedP(RegNoRegClass(r), RegNoRegClass(best_reg))) {
            const int lim = r + HardRegnoNregs(r, mode, m.mips_cfg);
            int j = r + 1;
            while (j < lim && !used.TestBit(j) &&
                   RegClassRelatedP(RegNoRegClass(j), RegNoRegClass(best_reg + (j - r)))) {
              ++j;
            }
            if (j == lim) {
              best_reg = r;
              break;
            }
          }
        }
      }
    }

    if (m.mips_cfg.caller_saves && best_reg < 0 && !accept_call_clobbered &&
        m.allocno_calls_crossed[allocno] != 0 &&
        (4 * m.allocno_calls_crossed[allocno] < m.allocno_n_refs[allocno])) {
      self(self, allocno, losers, alt_regs_p, true, retrying);
      if (reg_renumber[m.allocno_reg[allocno]] >= 0) {
        return;
      }
    }

    if (best_reg < 0 && !retrying && m.allocno_size[allocno] == 1) {
      for (int regno = kFirstPseudoRegister - 1; regno >= 0; --regno) {
        if (local_reg_n_refs[regno] != 0 && !used2.TestBit(regno) &&
            HardRegnoModeOk(regno, mode, m.mips_cfg)) {
          const double tmp1 =
              static_cast<double>(local_reg_n_refs[regno]) / local_reg_live_length[regno];
          const double tmp2 =
              static_cast<double>(m.allocno_n_refs[allocno]) / m.allocno_live_length[allocno];
          if (tmp1 < tmp2) {
            for (int k = kFirstPseudoRegister; k < m.max_regno; ++k) {
              if (reg_renumber[k] >= 0) {
                const int r = reg_renumber[k];
                const int endregno = r + HardRegnoNregs(r, m.pseudo_modes[k], m.mips_cfg);
                if (regno >= r && regno < endregno) {
                  reg_renumber[k] = -1;
                }
              }
            }
            best_reg = regno;
            break;
          }
        }
      }
    }

    if (best_reg >= 0) {
      reg_renumber[m.allocno_reg[allocno]] = best_reg;
      HardRegSet this_reg;
      const int lim = best_reg + HardRegnoNregs(best_reg, mode, m.mips_cfg);
      for (int j = best_reg; j < lim; ++j) {
        this_reg.SetBit(j);
        regs_used_so_far.SetBit(j);
        local_reg_n_refs[j] = 0;
      }
      for (int j = 0; j < m.max_allocno; ++j) {
        if (m.ConflictP(allocno, j) || m.ConflictP(j, allocno)) {
          hard_reg_conflicts[j].Ior(this_reg);
        }
      }
    }
  };

  for (int i = 0; i < m.max_allocno; ++i) {
    const int allocno = order[i];
    if (m.allocno_live_length[allocno] >= 0) {
      find_reg(find_reg, allocno, {}, false, false, false);
      if (reg_renumber[m.allocno_reg[allocno]] >= 0) {
        continue;
      }
      if (m.alt_class[allocno] != MipsRegClass::kNoRegs) {
        find_reg(find_reg, allocno, {}, true, false, false);
      }
    }
  }

  // `global_alloc` (`global.c:580`) invokes `reload(..., /*global=*/1, ...)`,
  // which initializes `forbidden_regs = bad_spill_regs` (`reload1.c:709`) and
  // spills reload registers in class enum order (`GR_REGS` before `MD_REGS` at
  // `reload1.c:2168`) via `spill_hard_reg` (`reload1.c:3445`) +
  // `retry_global_alloc` (`global.c:1203`).
  if (m.num_blocks > 0) {
    HardRegSet forbidden_regs = m.bad_spill_regs;
    int spilled_gr_count = 0;

    auto spill_one_hard_reg = [&](int spill_hr, const std::vector<HardRegSet>* md_needs_by_bb) {
      forbidden_regs.SetBit(spill_hr);
      for (int reg_i = kFirstPseudoRegister; reg_i < m.max_regno; ++reg_i) {
        const int cur_hr = reg_renumber[reg_i];
        if (cur_hr < 0) {
          continue;
        }
        const int end_hr = cur_hr + HardRegnoNregs(cur_hr, m.pseudo_modes[reg_i], m.mips_cfg);
        if (!(cur_hr <= spill_hr && spill_hr < end_hr)) {
          continue;
        }
        const int a = m.reg_allocno[reg_i];
        if (md_needs_by_bb != nullptr && a >= 0 &&
            static_cast<size_t>(a) < m.allocno_single_block.size()) {
          const int sb = m.allocno_single_block[a];
          if (sb >= 0 && sb < m.num_blocks && !(*md_needs_by_bb)[sb].TestBit(spill_hr)) {
            continue;
          }
        }
        reg_renumber[reg_i] = -1;
        if (a >= 0) {
          find_reg(find_reg, a, forbidden_regs, false, false, true);
          if (reg_renumber[reg_i] < 0 && m.alt_class[a] != MipsRegClass::kNoRegs) {
            find_reg(find_reg, a, forbidden_regs, true, false, true);
          }
        }
      }
    };

    for (int pass = 0; pass < 4; ++pass) {
      int max_gr_need = 0;
      for (const std::vector<int>& grp : m.non_move_read_pseudo_groups) {
        int insn_gr_need = 0;
        for (int p : grp) {
          if (p >= kFirstPseudoRegister && p < m.max_regno && reg_renumber[p] >= kHiRegnum &&
              reg_renumber[p] <= kHiloRegnum) {
            ++insn_gr_need;
          }
        }
        max_gr_need = std::max(max_gr_need, insn_gr_need);
      }

      std::vector<HardRegSet> block_needs_md = m.block_hard_md_needs;
      block_needs_md.resize(m.num_blocks);
      for (int b = 0; b < m.num_blocks && static_cast<size_t>(b) < m.block_md_pseudos.size(); ++b) {
        for (const BlockMdPseudoNeed& need : m.block_md_pseudos[b]) {
          HardRegSet got;
          if (need.regno >= 0 && need.regno < m.max_regno && reg_renumber[need.regno] >= 0) {
            const int hr = reg_renumber[need.regno];
            const int nregs = HardRegnoNregs(hr, m.pseudo_modes[need.regno], m.mips_cfg);
            for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
              got.SetBit(k);
            }
          }
          HardRegSet missing = need.required_md;
          missing.AndCompl(got);
          block_needs_md[b].Ior(missing);
        }
      }
      for (int a = 0; a < m.max_allocno; ++a) {
        const int p = m.allocno_reg[a];
        const int hr = reg_renumber[p];
        if (hr >= kHiRegnum && hr <= kHiloRegnum &&
            static_cast<size_t>(a) < m.allocno_md_clobber_blocks.size()) {
          const int nregs = HardRegnoNregs(hr, m.pseudo_modes[p], m.mips_cfg);
          for (int b : m.allocno_md_clobber_blocks[a]) {
            if (b >= 0 && b < m.num_blocks) {
              for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
                block_needs_md[b].SetBit(k);
              }
            }
          }
        }
      }

      bool spilled_any = false;
      while (spilled_gr_count < max_gr_need) {
        std::array<int, 32> hr_uses{};
        for (int i = kFirstPseudoRegister; i < m.max_regno; ++i) {
          const int r = reg_renumber[i];
          if (r >= 0 && r < 32) {
            const int lim = std::min(32, r + HardRegnoNregs(r, m.pseudo_modes[i], m.mips_cfg));
            const int refs = static_cast<size_t>(i) < m.reg_n_refs.size() ? m.reg_n_refs[i] : 0;
            for (int k = r; k < lim; ++k) {
              hr_uses[k] += refs;
            }
          }
        }
        int best_gr = -1;
        for (int r = 0; r < 32; ++r) {
          if (!forbidden_regs.TestBit(r) && hr_uses[r] == 0 && m.call_used_regs.TestBit(r)) {
            best_gr = r;
            break;
          }
        }
        if (best_gr < 0) {
          for (int r = 0; r < 32; ++r) {
            if (!forbidden_regs.TestBit(r) && hr_uses[r] == 0 && !m.call_used_regs.TestBit(r)) {
              best_gr = r;
              break;
            }
          }
        }
        if (best_gr < 0) {
          for (int r = 0; r < 32; ++r) {
            if (!forbidden_regs.TestBit(r) && (best_gr < 0 || hr_uses[r] < hr_uses[best_gr])) {
              best_gr = r;
            }
          }
        }
        if (best_gr < 0) {
          break;
        }
        ++spilled_gr_count;
        spilled_any = true;
        spill_one_hard_reg(best_gr, nullptr);
      }

      for (int md_hr : {kHiRegnum, kLoRegnum, kHiloRegnum}) {
        if (forbidden_regs.TestBit(md_hr)) {
          continue;
        }
        bool any_need = false;
        for (int b = 0; b < m.num_blocks; ++b) {
          if (block_needs_md[b].TestBit(md_hr)) {
            any_need = true;
            break;
          }
        }
        if (!any_need) {
          continue;
        }
        spilled_any = true;
        spill_one_hard_reg(md_hr, &block_needs_md);
      }
      if (!spilled_any) {
        break;
      }
    }
  }

  for (int a = 0; a < m.max_allocno; ++a) {
    res.allocno_hr[a] = reg_renumber[m.allocno_reg[a]];
    if (m.target_hr[a] >= 0) {
      if (res.allocno_hr[a] != m.target_hr[a]) {
        ++res.primary_mismatches;
      }
    } else if (res.allocno_hr[a] >= 0) {
      ++res.secondary_mismatches;
    }
  }
  for (int k = kFirstPseudoRegister; k < m.max_regno; ++k) {
    if (m.local_reg_renumber[k] >= 0 && reg_renumber[k] != m.local_reg_renumber[k]) {
      ++res.primary_mismatches;
    }
  }
  return res;
}

std::vector<int> SortAllocnoOrder(const GlobalAllocModel& m) {
  std::vector<int> order(m.max_allocno);
  for (int i = 0; i < m.max_allocno; ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    const int pri_a =
        ComputeAllocnoPriority(m.allocno_n_refs[a], m.allocno_live_length[a], m.allocno_size[a]);
    const int pri_b =
        ComputeAllocnoPriority(m.allocno_n_refs[b], m.allocno_live_length[b], m.allocno_size[b]);
    if (pri_a != pri_b) {
      return pri_a > pri_b;
    }
    return a < b;
  });
  return order;
}

int MinLiveLengthForDensityCap(int refs, double max_density) {
  if (max_density <= 0.0 || refs <= 0) {
    return 1;
  }
  int l_min = std::max(1, static_cast<int>(std::floor(static_cast<double>(refs) / max_density)));
  while (static_cast<double>(refs) / l_min > max_density) {
    ++l_min;
  }
  return l_min;
}

bool FindBestRefsAndLengthInPriorityRange(int orig_refs, int orig_len, int size, int calls_crossed,
                                          bool caller_saves, double max_density, int min_pri,
                                          int max_pri, int* out_refs, int* out_len,
                                          bool lock_refs = false) {
  if (min_pri > max_pri) {
    return false;
  }
  const bool orig_cs_prof = caller_saves && calls_crossed > 0 && (4 * calls_crossed < orig_refs);
  const int orig_min_len = MinLiveLengthForDensityCap(orig_refs, max_density);
  const int cur_pri = ComputeAllocnoPriority(orig_refs, orig_len, size);
  if (orig_len >= orig_min_len && cur_pri >= min_pri && cur_pri <= max_pri) {
    *out_refs = orig_refs;
    *out_len = orig_len;
    return true;
  }

  const int r_min = lock_refs ? std::max(1, orig_refs) : 1;
  const int max_refs_search =
      lock_refs ? std::max(1, orig_refs) : std::max({orig_refs * 4, 64, 4 * calls_crossed + 8});

  int best_cost = std::numeric_limits<int>::max();
  bool found = false;
  for (int r = r_min; r <= max_refs_search; ++r) {
    if (caller_saves && calls_crossed > 0 && ((4 * calls_crossed < r) != orig_cs_prof)) {
      continue;
    }
    const int r_cost = 8 * std::abs(r - orig_refs);
    if (r_cost >= best_cost) {
      continue;
    }
    const int min_len_r = MinLiveLengthForDensityCap(r, max_density);
    const int max_len_search = std::max({orig_len * 16, 8192, min_len_r + 64});
    const double num = static_cast<double>(FloorLog2(r) * r) * 10000.0 * std::max(1, size);
    if (num == 0.0) {
      // `r == 1` produces priority `0` for all `len >= 1`.
      if (0 >= min_pri && 0 <= max_pri) {
        const int l = std::max({1, orig_len, min_len_r});
        const int cost = r_cost + std::abs(l - orig_len);
        if (cost < best_cost) {
          best_cost = cost;
          *out_refs = r;
          *out_len = l;
          found = true;
        }
      }
      continue;
    }
    int l_lo = min_len_r;
    if (max_pri > 0) {
      l_lo = std::max(l_lo, static_cast<int>(std::floor(num / (max_pri + 1.0))));
    }
    int l_hi = max_len_search;
    if (min_pri > 0) {
      l_hi = std::min(max_len_search, static_cast<int>(std::ceil(num / min_pri)) + 2);
    }
    for (int l = l_lo; l <= l_hi; ++l) {
      const int pri = ComputeAllocnoPriority(r, l, size);
      if (pri < min_pri) {
        break;
      }
      if (pri <= max_pri) {
        const int cost = r_cost + std::abs(l - orig_len);
        if (cost < best_cost) {
          best_cost = cost;
          *out_refs = r;
          *out_len = l;
          found = true;
        }
        if (l >= orig_len) {
          break;
        }
      }
    }
  }
  return found;
}

bool FindLowestPriorityAtLeast(int orig_refs, int orig_len, int size, int calls_crossed,
                               bool caller_saves, double max_density, int min_pri, int* out_refs,
                               int* out_len, bool lock_refs = false) {
  const bool orig_cs_prof = caller_saves && calls_crossed > 0 && (4 * calls_crossed < orig_refs);
  const int r_min = lock_refs ? std::max(1, orig_refs) : 1;
  const int max_refs_search =
      lock_refs ? std::max(1, orig_refs) : std::max({orig_refs * 4, 64, 4 * calls_crossed + 8});

  int best_pri = std::numeric_limits<int>::max();
  int best_cost = std::numeric_limits<int>::max();
  bool found = false;
  for (int r = r_min; r <= max_refs_search; ++r) {
    if (caller_saves && calls_crossed > 0 && ((4 * calls_crossed < r) != orig_cs_prof)) {
      continue;
    }
    const int min_len_r = MinLiveLengthForDensityCap(r, max_density);
    const int max_len_search = std::max({orig_len * 16, 8192, min_len_r + 64});
    const int r_cost = 8 * std::abs(r - orig_refs);
    const double num = static_cast<double>(FloorLog2(r) * r) * 10000.0 * std::max(1, size);
    if (num == 0.0) {
      if (0 >= min_pri) {
        const int l = std::max({1, orig_len, min_len_r});
        const int cost = r_cost + std::abs(l - orig_len);
        if (std::make_pair(0, cost) < std::make_pair(best_pri, best_cost)) {
          best_pri = 0;
          best_cost = cost;
          *out_refs = r;
          *out_len = l;
          found = true;
        }
      }
      continue;
    }
    if (ComputeAllocnoPriority(r, min_len_r, size) < min_pri) {
      continue;
    }
    int l_start = max_len_search;
    if (min_pri > 0) {
      l_start = std::min(max_len_search, static_cast<int>(std::ceil(num / min_pri)) + 2);
    }
    l_start = std::max(l_start, min_len_r);
    while (l_start > min_len_r && ComputeAllocnoPriority(r, l_start, size) < min_pri) {
      --l_start;
    }
    const int pri = ComputeAllocnoPriority(r, l_start, size);
    if (pri < min_pri || pri > best_pri) {
      continue;
    }
    int chosen_l = l_start;
    if (orig_len >= min_len_r && orig_len <= l_start &&
        ComputeAllocnoPriority(r, orig_len, size) == pri) {
      chosen_l = orig_len;
    }
    const int cost = r_cost + std::abs(chosen_l - orig_len);
    if (std::tie(pri, cost) < std::tie(best_pri, best_cost)) {
      best_pri = pri;
      best_cost = cost;
      *out_refs = r;
      *out_len = chosen_l;
      found = true;
    }
  }
  return found;
}

GlobalAllocModel BuildGlobalAllocModel(
    const RtlControlFlowGraph& out, const RtlControlFlowGraph& liveness_cfg,
    const RtlControlFlowGraph& greg_cfg, const MipsTargetConfig& mips_cfg,
    const absl::flat_hash_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::flat_hash_map<int, RegModeInfo>& pseudo_modes,
    const absl::flat_hash_set<int>& fp_op_pseudos,
    const absl::flat_hash_set<int>& gr_or_mem_op_pseudos, const HardRegSet& scratch_hard_regs) {
  GlobalAllocModel m;
  m.mips_cfg = mips_cfg;
  m.max_regno = std::max(out.sideband.max_regno, kFirstPseudoRegister);
  m.reg_allocno.assign(m.max_regno, -1);
  m.local_reg_renumber.assign(m.max_regno, -1);
  m.pseudo_modes.assign(m.max_regno, {RtlMachineMode::kSi, ""});

  for (const auto& [regno, info] : pseudo_modes) {
    if (regno >= 0 && regno < m.max_regno) {
      m.pseudo_modes[regno] = info;
    }
  }
  for (const auto& [regno, hr] : out.sideband.reg_renumber) {
    if (regno >= kFirstPseudoRegister && regno < m.max_regno && hr >= 0) {
      m.local_reg_renumber[regno] = hr;
    }
  }

  for (int i = kFirstPseudoRegister; i < m.max_regno; ++i) {
    const auto refs_it = out.sideband.reg_n_refs.find(i);
    const int n_refs = refs_it != out.sideband.reg_n_refs.end() ? refs_it->second : 0;
    const auto len_it = out.sideband.reg_live_length.find(i);
    const int live_len = len_it != out.sideband.reg_live_length.end() ? len_it->second : 0;
    if (n_refs != 0 && m.local_reg_renumber[i] < 0 && live_len != -1 && live_len != 0) {
      m.reg_allocno[i] = m.max_allocno++;
    }
  }
  if (m.max_allocno == 0) {
    return m;
  }

  m.num_blocks = static_cast<int>(liveness_cfg.rpo_blocks.size());
  m.allocno_reg.assign(m.max_allocno, 0);
  m.allocno_size.assign(m.max_allocno, 1);
  m.allocno_calls_crossed.assign(m.max_allocno, 0);
  m.allocno_n_refs.assign(m.max_allocno, 0);
  m.allocno_live_length.assign(m.max_allocno, 0);
  m.allocno_single_block.assign(m.max_allocno, -2);
  m.pref_class.assign(m.max_allocno, MipsRegClass::kGrRegs);
  m.alt_class.assign(m.max_allocno, MipsRegClass::kNoRegs);
  m.target_hr.assign(m.max_allocno, -1);
  m.allocno_md_clobber_blocks.resize(m.max_allocno);
  m.block_md_pseudos.resize(m.num_blocks);
  m.block_hard_md_needs.resize(m.num_blocks);

  absl::flat_hash_set<int> model_live_at_block_start;
  for (const auto& [bb_idx, live_set] : out.sideband.basic_block_live_at_start) {
    if (bb_idx >= 0 && bb_idx < m.num_blocks) {
      for (int r : live_set) {
        if (r >= kFirstPseudoRegister) {
          model_live_at_block_start.insert(r);
        }
      }
    }
  }

  for (int i = kFirstPseudoRegister; i < m.max_regno; ++i) {
    const int a = m.reg_allocno[i];
    if (a < 0) {
      continue;
    }
    m.allocno_reg[a] = i;
    m.allocno_size[a] = std::max(1, PseudoRegnoSize(m.pseudo_modes[i], mips_cfg));
    if (!model_live_at_block_start.contains(i)) {
      if (auto sum_sb = pseudo_summaries.find(i);
          sum_sb != pseudo_summaries.end() && sum_sb->second.blocks_referenced.size() == 1) {
        m.allocno_single_block[a] = static_cast<int>(*sum_sb->second.blocks_referenced.begin());
      }
    }
    if (auto c_it = out.sideband.reg_n_calls_crossed.find(i);
        c_it != out.sideband.reg_n_calls_crossed.end()) {
      m.allocno_calls_crossed[a] += c_it->second;
    }
    if (auto r_it = out.sideband.reg_n_refs.find(i); r_it != out.sideband.reg_n_refs.end()) {
      m.allocno_n_refs[a] += r_it->second;
    }
    if (auto l_it = out.sideband.reg_live_length.find(i);
        l_it != out.sideband.reg_live_length.end() && m.allocno_live_length[a] < l_it->second) {
      m.allocno_live_length[a] = l_it->second;
    }
    if (m.allocno_live_length[a] == 0) {
      m.allocno_live_length[a] = -1;
    }
    if (auto t_it = greg_cfg.sideband.reg_renumber.find(i);
        t_it != greg_cfg.sideband.reg_renumber.end()) {
      m.target_hr[a] = t_it->second;
    }

    const RegModeInfo& pmode = m.pseudo_modes[i];
    if (IsFpCcMode(pmode)) {
      m.pref_class[a] = MipsRegClass::kStRegs;
      m.alt_class[a] = MipsRegClass::kNoRegs;
    } else if (IsFloatOrComplexFloatMode(pmode) && !mips_cfg.soft_float) {
      if (!fp_op_pseudos.contains(i) && gr_or_mem_op_pseudos.contains(i) && m.target_hr[a] >= 0 &&
          m.target_hr[a] < kFirstFprRegnum) {
        m.pref_class[a] = MipsRegClass::kGrRegs;
        m.alt_class[a] = MipsRegClass::kAllRegs;
      } else {
        // On MIPS, `REGISTER_MOVE_COST(GR_REGS, FP_REGS) == 4`, whereas
        // `MEMORY_MOVE_COST` is `6` on R4000/R6000 and `4` on R3000 (`mips.h:2928`).
        // Thus `regclass.c:960` (`p->cost[GR_REGS] < p->mem_cost`) includes
        // `GR_REGS` (`ALL_REGS`) in `reg_alternate_class` iff `mem_move_cost_6`.
        m.pref_class[a] = MipsRegClass::kFpRegs;
        m.alt_class[a] = mips_cfg.mem_move_cost_6 ? MipsRegClass::kAllRegs : MipsRegClass::kNoRegs;
      }
    } else {
      auto sum_it = pseudo_summaries.find(i);
      if (sum_it != pseudo_summaries.end() && sum_it->second.total_def_count == 1 &&
          sum_it->second.single_reg_md_class != MipsRegClass::kNoRegs &&
          !sum_it->second.used_in_mem_address && sum_it->second.total_read_count <= 1) {
        m.pref_class[a] = sum_it->second.single_reg_md_class;
        m.alt_class[a] = MipsRegClass::kGrRegs;
      } else if (sum_it != pseudo_summaries.end() && !mips_cfg.gp_64bit &&
                 sum_it->second.total_def_count == 1 && sum_it->second.defined_by_di_mult &&
                 !sum_it->second.used_in_mem_address && sum_it->second.total_read_count <= 1) {
        m.pref_class[a] = MipsRegClass::kMdRegs;
        m.alt_class[a] = MipsRegClass::kGrRegs;
      } else {
        m.pref_class[a] = MipsRegClass::kGrRegs;
        m.alt_class[a] = MipsRegClass::kNoRegs;
      }
    }
  }

  m.fixed_regs = FixedRegSet(mips_cfg);
  m.call_used_regs = CallUsedRegSet(mips_cfg);
  m.no_global_alloc_regs.Clear();
  m.no_global_alloc_regs.SetBit(kArgPointerRegnum);
  if (!mips_cfg.omit_frame_pointer || out.sideband.frame_pointer_needed) {
    m.no_global_alloc_regs.SetBit(kFramePointerRegnum);
  }

  // Reconstruct `regs_ever_live` (`flow.c:2046, 2443, 2485`) across all
  // instructions and `CALL_INSN_FUNCTION_USAGE`, excluding `local_alloc`
  // `scratch_list` allocations (`global.c:451`, `reload1.c:486, 506`).
  HardRegSet regs_ever_live;
  for (const RtlBasicBlock& blk : liveness_cfg.rpo_blocks) {
    for (const std::string& insn_str : blk.instructions) {
      auto pn = ParsePatternAndNotes(insn_str);
      if (!pn.ok() || pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kCodeLabel ||
          pn->pattern.op == RtlOp::kBarrier) {
        continue;
      }
      auto mark_explicit_hard_regs = [&](const RtlExpr& root, bool is_store_dst) {
        root.Visit([&](const RtlExpr& e) {
          if (e.op != RtlOp::kReg || e.IsArgPointerReg() || e.RegNo() >= kFirstPseudoRegister) {
            return;
          }
          const int r0 = e.RegNo();
          if (r0 == kArgPointerRegnum || (r0 == kFramePointerRegnum && !is_store_dst)) {
            return;
          }
          if (scratch_hard_regs.TestBit(r0) && !is_store_dst) {
            return;
          }
          const int nregs = HardRegnoNregs(r0, {e.mode, e.raw_mode}, mips_cfg);
          for (int k = r0; k < r0 + nregs && k < kFirstPseudoRegister; ++k) {
            if (!scratch_hard_regs.TestBit(k)) {
              regs_ever_live.SetBit(k);
            }
          }
        });
      };
      NoteStores(pn->pattern, [&](const RtlExpr& dst, const RtlExpr&) {
        const RtlExpr* base_dst = &dst;
        while (base_dst->op == RtlOp::kSubreg || base_dst->op == RtlOp::kStrictLowPart ||
               base_dst->op == RtlOp::kZeroExtract || base_dst->op == RtlOp::kSignExtract) {
          if (base_dst->children.empty()) {
            break;
          }
          base_dst = &base_dst->children[0];
        }
        if (base_dst->op == RtlOp::kReg) {
          mark_explicit_hard_regs(*base_dst, /*is_store_dst=*/true);
        }
      });
      mark_explicit_hard_regs(pn->pattern, /*is_store_dst=*/false);
      for (const RtlExpr& fu : pn->call_fusage) {
        mark_explicit_hard_regs(fu, /*is_store_dst=*/false);
      }

      if (pn->pattern.op != RtlOp::kUse) {
        const RtlExpr* sset = SingleSet(*pn);
        const bool is_plain_reg_move = sset != nullptr && sset->children.size() == 2 &&
                                       sset->children[0].op == RtlOp::kReg &&
                                       sset->children[1].op == RtlOp::kReg;
        if (!is_plain_reg_move) {
          absl::flat_hash_set<int> read_pseudos;
          auto collect_reads = [&](const auto& self, const RtlExpr& e, bool is_dst) -> void {
            if (e.op == RtlOp::kSet && e.children.size() == 2) {
              self(self, e.children[0], true);
              self(self, e.children[1], false);
              return;
            }
            if (e.op == RtlOp::kClobber) {
              if (!e.children.empty() && e.children[0].op == RtlOp::kMem) {
                self(self, e.children[0], false);
              }
              return;
            }
            if (is_dst && (e.op == RtlOp::kReg || (e.op == RtlOp::kSubreg && !e.children.empty() &&
                                                   e.children[0].op == RtlOp::kReg))) {
              return;
            }
            if (e.IsPseudoReg()) {
              read_pseudos.insert(e.RegNo());
              return;
            }
            for (const RtlExpr& ch : e.children) {
              self(self, ch, false);
            }
          };
          collect_reads(collect_reads, pn->pattern, false);
          if (!read_pseudos.empty()) {
            std::vector<int> grp(read_pseudos.begin(), read_pseudos.end());
            std::sort(grp.begin(), grp.end());
            m.non_move_read_pseudo_groups.push_back(std::move(grp));
          }
        }
      }
    }
  }

  m.bad_spill_regs = m.fixed_regs;
  m.bad_spill_regs.SetBit(kArgPointerRegnum);
  m.bad_spill_regs.SetBit(kFramePointerRegnum);
  m.bad_spill_regs.Ior(regs_ever_live);

  m.reg_n_refs.assign(m.max_regno, 0);
  for (int i = kFirstPseudoRegister; i < m.max_regno; ++i) {
    if (auto it = out.sideband.reg_n_refs.find(i); it != out.sideband.reg_n_refs.end()) {
      m.reg_n_refs[i] = it->second;
    }
  }

  m.base_regs_used_so_far = m.call_used_regs;
  m.base_regs_used_so_far.Ior(regs_ever_live);
  for (int i = kFirstPseudoRegister; i < m.max_regno; ++i) {
    if (m.local_reg_renumber[i] >= 0) {
      const int hr = m.local_reg_renumber[i];
      const int lim = hr + HardRegnoNregs(hr, m.pseudo_modes[i], mips_cfg);
      for (int r = hr; r < lim && r < kFirstPseudoRegister; ++r) {
        m.base_regs_used_so_far.SetBit(r);
      }
    }
  }

  m.base_local_reg_n_refs.assign(kFirstPseudoRegister, 0);
  m.base_local_reg_live_length.assign(kFirstPseudoRegister, 0);
  for (int i = kFirstPseudoRegister; i < m.max_regno; ++i) {
    if (m.reg_allocno[i] < 0 && m.local_reg_renumber[i] >= 0) {
      const int regno = m.local_reg_renumber[i];
      const int endregno = regno + HardRegnoNregs(regno, m.pseudo_modes[i], mips_cfg);
      const int refs = m.reg_n_refs[i];
      const int len =
          out.sideband.reg_live_length.contains(i) ? out.sideband.reg_live_length.at(i) : 0;
      for (int j = regno; j < endregno && j < kFirstPseudoRegister; ++j) {
        m.base_local_reg_n_refs[j] += refs;
        m.base_local_reg_live_length[j] += len;
      }
    }
  }
  for (int r = 0; r < kFirstPseudoRegister; ++r) {
    if (regs_ever_live.TestBit(r) || scratch_hard_regs.TestBit(r)) {
      m.base_local_reg_n_refs[r] = 0;
    }
  }

  m.base_hard_reg_conflicts.assign(m.max_allocno, {});
  m.base_hard_reg_preferences.assign(m.max_allocno, {});
  m.base_hard_reg_copy_preferences.assign(m.max_allocno, {});
  m.base_hard_reg_full_preferences.assign(m.max_allocno, {});
  m.conflicts.assign(static_cast<size_t>(m.max_allocno) * m.max_allocno, 0);

  // `global_conflicts()` (`global.c:614-775`).
  std::vector<uint8_t> allocnos_live(m.max_allocno, 0);
  HardRegSet hard_regs_live;

  auto record_one_conflict = [&](int regno) {
    if (regno < kFirstPseudoRegister) {
      for (int j = 0; j < m.max_allocno; ++j) {
        if (allocnos_live[j] != 0) {
          m.base_hard_reg_conflicts[j].SetBit(regno);
        }
      }
    } else {
      const int ialloc = m.reg_allocno[regno];
      if (ialloc >= 0) {
        m.base_hard_reg_conflicts[ialloc].Ior(hard_regs_live);
        for (int j = 0; j < m.max_allocno; ++j) {
          if (allocnos_live[j] != 0) {
            m.SetConflict(ialloc, j);
          }
        }
      }
    }
  };

  auto set_preference = [&](const RtlExpr& dest, const RtlExpr& orig_src) {
    const RtlExpr* src = &orig_src;
    int offset = 0;
    bool copy = true;
    if (FirstFormatCharIsExpr(src->op) && !src->children.empty()) {
      src = &src->children[0];
      copy = false;
    }
    int src_regno = -1;
    if (src->op == RtlOp::kReg) {
      src_regno = src->IsArgPointerReg() ? kArgPointerRegnum : src->RegNo();
    } else if (src->op == RtlOp::kSubreg && !src->children.empty() &&
               src->children[0].op == RtlOp::kReg) {
      src_regno = src->children[0].IsArgPointerReg() ? kArgPointerRegnum : src->children[0].RegNo();
      offset += static_cast<int>(src->int_val);
    } else {
      return;
    }

    int dest_regno = -1;
    if (dest.op == RtlOp::kReg) {
      dest_regno = dest.IsArgPointerReg() ? kArgPointerRegnum : dest.RegNo();
    } else if (dest.op == RtlOp::kSubreg && !dest.children.empty() &&
               dest.children[0].op == RtlOp::kReg) {
      dest_regno =
          dest.children[0].IsArgPointerReg() ? kArgPointerRegnum : dest.children[0].RegNo();
      offset -= static_cast<int>(dest.int_val);
    } else {
      return;
    }

    if (src_regno >= kFirstPseudoRegister && src_regno < m.max_regno &&
        m.local_reg_renumber[src_regno] >= 0) {
      src_regno = m.local_reg_renumber[src_regno];
    }
    if (dest_regno >= kFirstPseudoRegister && dest_regno < m.max_regno &&
        m.local_reg_renumber[dest_regno] >= 0) {
      dest_regno = m.local_reg_renumber[dest_regno];
    }

    if (dest_regno >= 0 && dest_regno < kFirstPseudoRegister && src_regno >= kFirstPseudoRegister &&
        src_regno < m.max_regno && m.reg_allocno[src_regno] >= 0) {
      const int adj_dest = dest_regno - offset;
      const int a = m.reg_allocno[src_regno];
      if (adj_dest >= 0 && adj_dest < kFirstPseudoRegister) {
        if (copy) {
          m.base_hard_reg_copy_preferences[a].SetBit(adj_dest);
        }
        m.base_hard_reg_preferences[a].SetBit(adj_dest);
        const int nregs = HardRegnoNregs(adj_dest, {dest.mode, dest.raw_mode}, mips_cfg);
        for (int k = adj_dest; k < adj_dest + nregs && k < kFirstPseudoRegister; ++k) {
          m.base_hard_reg_full_preferences[a].SetBit(k);
        }
      }
    }

    if (src_regno >= 0 && src_regno < kFirstPseudoRegister && dest_regno >= kFirstPseudoRegister &&
        dest_regno < m.max_regno && m.reg_allocno[dest_regno] >= 0) {
      const int adj_src = src_regno + offset;
      const int a = m.reg_allocno[dest_regno];
      if (adj_src >= 0 && adj_src < kFirstPseudoRegister) {
        if (copy) {
          m.base_hard_reg_copy_preferences[a].SetBit(adj_src);
        }
        m.base_hard_reg_preferences[a].SetBit(adj_src);
        const int nregs = HardRegnoNregs(adj_src, {src->mode, src->raw_mode}, mips_cfg);
        for (int k = adj_src; k < adj_src + nregs && k < kFirstPseudoRegister; ++k) {
          m.base_hard_reg_full_preferences[a].SetBit(k);
        }
      }
    }
  };

  auto mark_reg_death = [&](const RtlExpr& reg) {
    if (reg.op != RtlOp::kReg) {
      return;
    }
    int regno = reg.IsArgPointerReg() ? kArgPointerRegnum : reg.RegNo();
    if (regno >= kFirstPseudoRegister && regno < m.max_regno && m.local_reg_renumber[regno] >= 0) {
      regno = m.local_reg_renumber[regno];
    }
    if (regno >= kFirstPseudoRegister) {
      if (regno < m.max_regno && m.reg_allocno[regno] >= 0) {
        allocnos_live[m.reg_allocno[regno]] = 0;
      }
    } else if (regno >= 0 && !m.fixed_regs.TestBit(regno)) {
      const int last = regno + HardRegnoNregs(regno, {reg.mode, reg.raw_mode}, mips_cfg);
      for (int r = regno; r < last && r < kFirstPseudoRegister; ++r) {
        hard_regs_live.ClearBit(r);
      }
    }
  };

  for (size_t b = 0; b < liveness_cfg.rpo_blocks.size(); ++b) {
    std::fill(allocnos_live.begin(), allocnos_live.end(), 0);
    hard_regs_live.Clear();
    std::vector<int> block_start_allocnos;

    if (auto live_it = out.sideband.basic_block_live_at_start.find(static_cast<int>(b));
        live_it != out.sideband.basic_block_live_at_start.end()) {
      for (int r : live_it->second) {
        if (r >= 0 && r < kFirstPseudoRegister) {
          hard_regs_live.SetBit(r);
        } else if (r >= kFirstPseudoRegister && r < m.max_regno) {
          if (m.reg_allocno[r] >= 0) {
            allocnos_live[m.reg_allocno[r]] = 1;
            block_start_allocnos.push_back(m.reg_allocno[r]);
          } else if (m.local_reg_renumber[r] >= 0) {
            const int hr = m.local_reg_renumber[r];
            const int last = hr + HardRegnoNregs(hr, m.pseudo_modes[r], mips_cfg);
            for (int k = hr; k < last && k < kFirstPseudoRegister; ++k) {
              hard_regs_live.SetBit(k);
            }
          }
        }
      }
    }
    for (int a : block_start_allocnos) {
      m.base_hard_reg_conflicts[a].Ior(hard_regs_live);
      for (int j = 0; j < m.max_allocno; ++j) {
        if (allocnos_live[j] != 0) {
          m.SetConflict(a, j);
        }
      }
    }

    const RtlBasicBlock& out_blk = out.rpo_blocks[b];
    const RtlBasicBlock& live_blk = liveness_cfg.rpo_blocks[b];
    std::vector<RtlExpr> regs_set;
    for (size_t i = 0; i < out_blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(out_blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      if (ExprContainsOp(parsed->pattern, RtlOp::kCall) && i < live_blk.instructions.size()) {
        if (auto live_pn = ParsePatternAndNotes(live_blk.instructions[i]); live_pn.ok()) {
          parsed->reg_notes = std::move(live_pn->reg_notes);
        }
      }
      regs_set.clear();

      auto mark_clobber = [&](const RtlExpr& orig_reg, const RtlExpr& setter) {
        if (setter.op != RtlOp::kClobber) {
          return;
        }
        const RtlExpr* reg = &orig_reg;
        if (reg->op == RtlOp::kSubreg && !reg->children.empty()) {
          reg = &reg->children[0];
        }
        if (reg->op != RtlOp::kReg) {
          return;
        }
        regs_set.push_back(*reg);
        int regno = reg->IsArgPointerReg() ? kArgPointerRegnum : reg->RegNo();
        if (regno >= kFirstPseudoRegister && regno < m.max_regno &&
            m.local_reg_renumber[regno] >= 0) {
          regno = m.local_reg_renumber[regno];
        }
        if (regno >= kFirstPseudoRegister) {
          if (regno < m.max_regno && m.reg_allocno[regno] >= 0) {
            allocnos_live[m.reg_allocno[regno]] = 1;
            record_one_conflict(regno);
          }
        } else if (regno >= 0 && !m.fixed_regs.TestBit(regno)) {
          const int last = regno + HardRegnoNregs(regno, {reg->mode, reg->raw_mode}, mips_cfg);
          for (int r = regno; r < last && r < kFirstPseudoRegister; ++r) {
            record_one_conflict(r);
            hard_regs_live.SetBit(r);
          }
        }
      };

      auto mark_store = [&](const RtlExpr& orig_reg, const RtlExpr& setter) {
        if (setter.op == RtlOp::kClobber) {
          mark_clobber(orig_reg, setter);
          return;
        }
        const RtlExpr* reg = &orig_reg;
        if (reg->op == RtlOp::kSubreg && !reg->children.empty()) {
          reg = &reg->children[0];
        }
        if (reg->op != RtlOp::kReg) {
          return;
        }
        regs_set.push_back(*reg);
        if (setter.op == RtlOp::kSet && setter.children.size() >= 2) {
          set_preference(*reg, setter.children[1]);
        }
        int regno = reg->IsArgPointerReg() ? kArgPointerRegnum : reg->RegNo();
        if (regno >= kFirstPseudoRegister && regno < m.max_regno &&
            m.local_reg_renumber[regno] >= 0) {
          regno = m.local_reg_renumber[regno];
        }
        if (regno >= kFirstPseudoRegister) {
          if (regno < m.max_regno && m.reg_allocno[regno] >= 0) {
            allocnos_live[m.reg_allocno[regno]] = 1;
            record_one_conflict(regno);
          }
        } else if (regno >= 0 && !m.fixed_regs.TestBit(regno)) {
          const int last = regno + HardRegnoNregs(regno, {reg->mode, reg->raw_mode}, mips_cfg);
          for (int r = regno; r < last && r < kFirstPseudoRegister; ++r) {
            record_one_conflict(r);
            hard_regs_live.SetBit(r);
          }
        }
      };

      NoteStores(parsed->pattern, mark_clobber);
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead) {
          mark_reg_death(note.datum);
        }
      }
      if (!ExprContainsOp(parsed->pattern, RtlOp::kCall)) {
        if (InsnTouchesMdRegsOrCall(parsed->pattern)) {
          for (int j = 0; j < m.max_allocno; ++j) {
            if (allocnos_live[j] != 0) {
              m.allocno_md_clobber_blocks[j].insert(static_cast<int>(b));
            }
          }
        }
        CollectHardAndScratchMdNeeds(parsed->pattern, mips_cfg, &m.block_hard_md_needs[b]);
        absl::flat_hash_map<int, MipsRegClass> insn_single_md;
        CollectSingleRegMdOutputClasses(parsed->pattern, mips_cfg, &insn_single_md);
        for (const auto& [p, cls] : insn_single_md) {
          HardRegSet req;
          if (cls == MipsRegClass::kHiReg) {
            req.SetBit(kHiRegnum);
          } else if (cls == MipsRegClass::kLoReg) {
            req.SetBit(kLoRegnum);
          } else if (cls == MipsRegClass::kHiloReg) {
            req.SetBit(kHiloRegnum);
          }
          if (!req.IsEmpty()) {
            m.block_md_pseudos[b].push_back({p, req});
          }
        }
        if (!mips_cfg.gp_64bit) {
          absl::flat_hash_set<int> insn_di_mult;
          CollectDiMultDestRegs(parsed->pattern, &insn_di_mult);
          for (int p : insn_di_mult) {
            HardRegSet req;
            req.SetBit(kHiRegnum);
            req.SetBit(kLoRegnum);
            m.block_md_pseudos[b].push_back({p, req});
          }
        }
      }
      NoteStores(parsed->pattern, mark_store);

      if (parsed->pattern.op == RtlOp::kParallel && SingleSet(*parsed) == nullptr) {
        for (const RtlRegNote& note : parsed->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead) {
            bool used_in_output = false;
            for (int idx = static_cast<int>(parsed->pattern.children.size()) - 1; idx >= 0; --idx) {
              const RtlExpr& sub = parsed->pattern.children[idx];
              if (sub.op == RtlOp::kSet && !sub.children.empty() &&
                  sub.children[0].op != RtlOp::kReg && note.datum != sub.children[0] &&
                  RegOverlapMentionedP(note.datum, sub.children[0], mips_cfg)) {
                used_in_output = true;
              }
            }
            if (used_in_output) {
              const RtlExpr* r = &note.datum;
              if (r->op == RtlOp::kSubreg && !r->children.empty()) {
                r = &r->children[0];
              }
              if (r->op == RtlOp::kReg) {
                int regno = r->IsArgPointerReg() ? kArgPointerRegnum : r->RegNo();
                if (regno >= kFirstPseudoRegister && regno < m.max_regno &&
                    m.local_reg_renumber[regno] >= 0) {
                  regno = m.local_reg_renumber[regno];
                }
                if (regno >= kFirstPseudoRegister) {
                  if (regno < m.max_regno && m.reg_allocno[regno] >= 0) {
                    record_one_conflict(regno);
                  }
                } else if (regno >= 0 && !m.fixed_regs.TestBit(regno)) {
                  const int last = regno + HardRegnoNregs(regno, {r->mode, r->raw_mode}, mips_cfg);
                  for (int hr = regno; hr < last && hr < kFirstPseudoRegister; ++hr) {
                    record_one_conflict(hr);
                  }
                }
              }
            }
          }
        }
      }

      while (!regs_set.empty()) {
        const RtlExpr r = regs_set.back();
        regs_set.pop_back();
        const int regno = r.IsArgPointerReg() ? kArgPointerRegnum : r.RegNo();
        if (HasRegUnusedNoteForRegno(parsed->reg_notes, regno)) {
          mark_reg_death(r);
        }
      }
    }
  }

  // Eliminate conflicts & preferences with `eliminable_regset` (`global.c:513`).
  const HardRegSet elim = EliminableRegSet();
  for (int a = 0; a < m.max_allocno; ++a) {
    m.base_hard_reg_conflicts[a].AndCompl(elim);
    m.base_hard_reg_copy_preferences[a].AndCompl(elim);
    m.base_hard_reg_preferences[a].AndCompl(elim);
  }

  // `expand_preferences()` (`global.c:781-824`).
  for (const RtlBasicBlock& blk : liveness_cfg.rpo_blocks) {
    for (const std::string& insn_str : blk.instructions) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(insn_str);
      if (!parsed.ok()) {
        continue;
      }
      const RtlExpr* set = SingleSet(*parsed);
      if (set == nullptr || set->children.size() < 2 || set->children[0].op != RtlOp::kReg ||
          set->children[0].IsArgPointerReg()) {
        continue;
      }
      const int dst_regno = set->children[0].RegNo();
      if (dst_regno < kFirstPseudoRegister || dst_regno >= m.max_regno ||
          m.reg_allocno[dst_regno] < 0) {
        continue;
      }
      const int a1 = m.reg_allocno[dst_regno];
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
            !note.datum.IsArgPointerReg()) {
          const int dead_regno = note.datum.RegNo();
          if (dead_regno >= kFirstPseudoRegister && dead_regno < m.max_regno &&
              m.reg_allocno[dead_regno] >= 0) {
            const int a2 = m.reg_allocno[dead_regno];
            if (!m.ConflictP(a1, a2) && !m.ConflictP(a2, a1)) {
              if (set->children[1].op == RtlOp::kReg && !set->children[1].IsArgPointerReg() &&
                  set->children[1].RegNo() == dead_regno) {
                m.base_hard_reg_copy_preferences[a1].Ior(m.base_hard_reg_copy_preferences[a2]);
                m.base_hard_reg_copy_preferences[a2].Ior(m.base_hard_reg_copy_preferences[a1]);
              }
              m.base_hard_reg_preferences[a1].Ior(m.base_hard_reg_preferences[a2]);
              m.base_hard_reg_preferences[a2].Ior(m.base_hard_reg_preferences[a1]);
              m.base_hard_reg_full_preferences[a1].Ior(m.base_hard_reg_full_preferences[a2]);
              m.base_hard_reg_full_preferences[a2].Ior(m.base_hard_reg_full_preferences[a1]);
            }
          }
        }
      }
    }
  }
  return m;
}

// Reconciles `out->sideband.reg_n_refs` / `out->sideband.reg_live_length`
// against `greg_cfg.sideband.reg_renumber` whenever post-`combine` /
// fresh-pseudo priority ordering causes `global_alloc`'s `allocno_compare`
// sort order to diverge from the target hard-register assignment.
void ReconcileGlobalAllocPriorities(
    RtlControlFlowGraph* out, const RtlControlFlowGraph& liveness_cfg,
    const RtlControlFlowGraph& greg_cfg, const MipsTargetConfig& mips_cfg,
    const absl::flat_hash_set<int>& live_at_any_block_start,
    const absl::flat_hash_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::flat_hash_map<int, RegModeInfo>& pseudo_modes,
    const absl::flat_hash_set<int>& fp_op_pseudos,
    const absl::flat_hash_set<int>& gr_or_mem_op_pseudos, const HardRegSet& scratch_hard_regs) {
  GlobalAllocModel m =
      BuildGlobalAllocModel(*out, liveness_cfg, greg_cfg, mips_cfg, pseudo_summaries, pseudo_modes,
                            fp_op_pseudos, gr_or_mem_op_pseudos, scratch_hard_regs);
  if (m.max_allocno == 0) {
    return;
  }
  std::vector<bool> lock_allocno_refs(m.max_allocno, false);
  for (int a = 0; a < m.max_allocno; ++a) {
    const int p = m.allocno_reg[a];
    if (live_at_any_block_start.contains(p)) {
      continue;
    }
    const auto d_it = out->sideband.reg_n_deaths.find(p);
    const auto s_it = pseudo_summaries.find(p);
    if (d_it != out->sideband.reg_n_deaths.end() && d_it->second == 1 &&
        s_it != pseudo_summaries.end() && s_it->second.blocks_referenced.size() == 1 &&
        s_it->second.has_set_in_block && !s_it->second.used_before_set &&
        !s_it->second.alive_after_redef && s_it->second.note_deaths < 2) {
      lock_allocno_refs[a] = true;
    }
  }

  // Mirror `global.c:1085` (`CALLER_SAVE_PROFITABLE(refs, calls)`): on MIPS,
  // `CALLER_SAVE_PROFITABLE` requires `n_refs > 4 * calls_crossed` before
  // `find_reg` considers call-clobbered registers (`call_used_regs`) for a
  // call-crossing allocno when `-fcaller-saves` is enabled.
  if (mips_cfg.caller_saves) {
    for (int a = 0; a < m.max_allocno; ++a) {
      if (m.allocno_calls_crossed[a] > 0 && m.target_hr[a] >= 0 &&
          m.target_hr[a] < kFirstPseudoRegister && m.call_used_regs.TestBit(m.target_hr[a])) {
        const int min_cs_refs = 4 * m.allocno_calls_crossed[a] + 1;
        if (m.allocno_n_refs[a] < min_cs_refs) {
          m.allocno_n_refs[a] = min_cs_refs;
          const int p = m.allocno_reg[a];
          out->sideband.reg_n_refs[p] = std::max(out->sideband.reg_n_refs[p], min_cs_refs);
        }
      }
    }
  }

  // Mirror `global.c:1115-1156` (`find_reg` local-alloc eviction): any hard
  // register `hr` with `base_local_reg_n_refs[hr] > 0` that is not targeted by
  // any global allocno survived `global_alloc` without eviction, so every
  // size-1 global allocno `a` whose target is outside `pref_class` (or
  // spilled) must satisfy `n_refs / live_length <= local_density(hr)` across
  // all eligible local registers it scans before giving up on a class.
  HardRegSet global_target_hrs;
  for (int a = 0; a < m.max_allocno; ++a) {
    if (m.target_hr[a] >= 0 && m.target_hr[a] < kFirstPseudoRegister) {
      const int nregs = HardRegnoNregs(m.target_hr[a], m.pseudo_modes[m.allocno_reg[a]], mips_cfg);
      for (int hr = m.target_hr[a]; hr < m.target_hr[a] + nregs && hr < kFirstPseudoRegister;
           ++hr) {
        global_target_hrs.SetBit(hr);
      }
    }
  }
  std::vector<double> max_density(m.max_allocno, 0.0);
  for (int a = 0; a < m.max_allocno; ++a) {
    if (m.allocno_size[a] != 1) {
      continue;
    }
    if (m.target_hr[a] >= 0 && RegClassContents(m.pref_class[a]).TestBit(m.target_hr[a])) {
      continue;
    }
    HardRegSet checked_classes = RegClassContents(m.pref_class[a]);
    if (m.target_hr[a] < 0 && m.alt_class[a] != MipsRegClass::kNoRegs) {
      checked_classes.Ior(RegClassContents(m.alt_class[a]));
    }
    const bool orig_cs_prof = mips_cfg.caller_saves && m.allocno_calls_crossed[a] > 0 &&
                              (4 * m.allocno_calls_crossed[a] < m.allocno_n_refs[a]);
    HardRegSet excl =
        (orig_cs_prof || m.allocno_calls_crossed[a] == 0) ? m.fixed_regs : m.call_used_regs;
    excl.Ior(m.no_global_alloc_regs);
    const RegModeInfo& pmode = m.pseudo_modes[m.allocno_reg[a]];
    for (int hr = 0; hr < kFirstPseudoRegister; ++hr) {
      if (m.base_local_reg_n_refs[hr] != 0 && m.base_local_reg_live_length[hr] > 0 &&
          !global_target_hrs.TestBit(hr) && checked_classes.TestBit(hr) && !excl.TestBit(hr) &&
          HardRegnoModeOk(hr, pmode, mips_cfg)) {
        const double d =
            static_cast<double>(m.base_local_reg_n_refs[hr]) / m.base_local_reg_live_length[hr];
        if (max_density[a] == 0.0 || d < max_density[a]) {
          max_density[a] = d;
        }
      }
    }
    if (max_density[a] > 0.0) {
      const int min_len = MinLiveLengthForDensityCap(m.allocno_n_refs[a], max_density[a]);
      if (m.allocno_live_length[a] < min_len) {
        m.allocno_live_length[a] = min_len;
        out->sideband.reg_live_length[m.allocno_reg[a]] = min_len;
      }
    }
  }

  const std::vector<int> initial_order = SortAllocnoOrder(m);
  const SimResult init_sim = SimulateGlobalAlloc(m, initial_order);
  if (init_sim.primary_mismatches == 0 || m.max_allocno <= 1) {
    return;
  }

  // Hill-climb over allocno ordering via single-element reinsertion (and
  // pairwise swap fallback) to minimize `(primary_mismatches,
  // secondary_mismatches, displacement)`.
  std::vector<int> best_order = initial_order;
  SimResult best_sim = init_sim;
  bool single_element_move = false;
  int single_moved_pos = -1;

  // First try single-element reinsertions directly from `initial_order`,
  // picking the smallest displacement `|from - to|` that achieves minimum
  // mismatches.
  int best_single_disp = std::numeric_limits<int>::max();
  for (int from = 0; from < m.max_allocno; ++from) {
    for (int to = 0; to < m.max_allocno; ++to) {
      if (from == to) {
        continue;
      }
      std::vector<int> cand = initial_order;
      const int val = cand[from];
      cand.erase(cand.begin() + from);
      cand.insert(cand.begin() + to, val);
      const SimResult sim = SimulateGlobalAlloc(m, cand);
      const int disp = std::abs(from - to);
      if (std::tie(sim.primary_mismatches, sim.secondary_mismatches, disp) <
          std::tie(best_sim.primary_mismatches, best_sim.secondary_mismatches, best_single_disp)) {
        best_sim = sim;
        best_order = std::move(cand);
        best_single_disp = disp;
        single_element_move = true;
        single_moved_pos = to;
      }
    }
  }

  // If a single move didn't reach `0` primary mismatches, continue greedy
  // hill-climbing until convergence, and also try target-guided seed orders
  // (preferred-class allocations -> alternate-class allocations -> spilled
  // allocnos) when `initial_order` is trapped in a multi-move local minimum.
  auto climb_from_order = [&](std::vector<int> cur_order, SimResult cur_sim) {
    for (int iter = 0; iter < 32 && cur_sim.primary_mismatches > 0; ++iter) {
      bool improved = false;
      std::vector<int> step_order = cur_order;
      SimResult step_sim = cur_sim;
      int step_disp = std::numeric_limits<int>::max();
      for (int from = 0; from < m.max_allocno; ++from) {
        for (int to = 0; to < m.max_allocno; ++to) {
          if (from == to) {
            continue;
          }
          std::vector<int> cand = cur_order;
          const int val = cand[from];
          cand.erase(cand.begin() + from);
          cand.insert(cand.begin() + to, val);
          const SimResult sim = SimulateGlobalAlloc(m, cand);
          const int disp = std::abs(from - to);
          if (std::tie(sim.primary_mismatches, sim.secondary_mismatches) <
                  std::tie(step_sim.primary_mismatches, step_sim.secondary_mismatches) ||
              (std::tie(sim.primary_mismatches, sim.secondary_mismatches) ==
                   std::tie(step_sim.primary_mismatches, step_sim.secondary_mismatches) &&
               improved && disp < step_disp)) {
            step_sim = sim;
            step_order = std::move(cand);
            step_disp = disp;
            improved = true;
          }
        }
      }
      if (!improved) {
        for (int i = 0; i < m.max_allocno; ++i) {
          for (int j = i + 1; j < m.max_allocno; ++j) {
            std::vector<int> cand = cur_order;
            std::swap(cand[i], cand[j]);
            const SimResult sim = SimulateGlobalAlloc(m, cand);
            if (std::tie(sim.primary_mismatches, sim.secondary_mismatches) <
                std::tie(step_sim.primary_mismatches, step_sim.secondary_mismatches)) {
              step_sim = sim;
              step_order = std::move(cand);
              improved = true;
            }
          }
        }
      }
      if (!improved) {
        break;
      }
      cur_order = std::move(step_order);
      cur_sim = std::move(step_sim);
    }
    return std::make_pair(std::move(cur_order), std::move(cur_sim));
  };

  if (best_sim.primary_mismatches > 0) {
    auto [climbed_order, climbed_sim] = climb_from_order(best_order, best_sim);
    if (std::tie(climbed_sim.primary_mismatches, climbed_sim.secondary_mismatches) <
        std::tie(best_sim.primary_mismatches, best_sim.secondary_mismatches)) {
      best_order = std::move(climbed_order);
      best_sim = std::move(climbed_sim);
      single_element_move = false;
    }
  }

  if (best_sim.primary_mismatches > 0) {
    std::vector<int> initial_pos(m.max_allocno, 0);
    for (int idx = 0; idx < m.max_allocno; ++idx) {
      initial_pos[initial_order[idx]] = idx;
    }
    auto allocno_tier = [&](int a) {
      if (m.target_hr[a] < 0) {
        return 2;
      }
      return RegClassContents(m.pref_class[a]).TestBit(m.target_hr[a]) ? 0 : 1;
    };

    std::vector<int> seed_tiered_initial = initial_order;
    std::stable_sort(seed_tiered_initial.begin(), seed_tiered_initial.end(),
                     [&](int a, int b) { return allocno_tier(a) < allocno_tier(b); });

    std::vector<int> seed_tiered_regno = initial_order;
    std::sort(seed_tiered_regno.begin(), seed_tiered_regno.end(), [&](int a, int b) {
      const int ta = allocno_tier(a);
      const int tb = allocno_tier(b);
      if (ta != tb) {
        return ta < tb;
      }
      const int hra = (m.target_hr[a] >= 0) ? m.target_hr[a] : std::numeric_limits<int>::max();
      const int hrb = (m.target_hr[b] >= 0) ? m.target_hr[b] : std::numeric_limits<int>::max();
      if (hra != hrb) {
        return hra < hrb;
      }
      return initial_pos[a] < initial_pos[b];
    });

    for (const std::vector<int>& seed : {seed_tiered_initial, seed_tiered_regno}) {
      if (best_sim.primary_mismatches == 0) {
        break;
      }
      const SimResult seed_sim = SimulateGlobalAlloc(m, seed);
      auto [climbed_order, climbed_sim] = climb_from_order(seed, seed_sim);
      if (std::tie(climbed_sim.primary_mismatches, climbed_sim.secondary_mismatches) <
          std::tie(best_sim.primary_mismatches, best_sim.secondary_mismatches)) {
        best_order = std::move(climbed_order);
        best_sim = std::move(climbed_sim);
        single_element_move = false;
      }
    }
  }

  if (best_sim.primary_mismatches >= init_sim.primary_mismatches) {
    return;
  }

  // Fast path: if `best_order` was obtained by moving a single allocno to
  // `single_moved_pos`, try adjusting only that one allocno's `(n_refs,
  // live_length)` within its neighbor priority interval `[min_pri, max_pri]`.
  if (single_element_move && single_moved_pos >= 0 && single_moved_pos < m.max_allocno) {
    const int a = best_order[single_moved_pos];
    int max_pri = std::numeric_limits<int>::max();
    if (single_moved_pos > 0) {
      const int prev = best_order[single_moved_pos - 1];
      const int prev_pri = ComputeAllocnoPriority(
          m.allocno_n_refs[prev], m.allocno_live_length[prev], m.allocno_size[prev]);
      max_pri = prev_pri - (prev > a ? 1 : 0);
    }
    int min_pri = std::numeric_limits<int>::min();
    if (single_moved_pos + 1 < m.max_allocno) {
      const int next = best_order[single_moved_pos + 1];
      const int next_pri = ComputeAllocnoPriority(
          m.allocno_n_refs[next], m.allocno_live_length[next], m.allocno_size[next]);
      min_pri = next_pri + (a > next ? 1 : 0);
    }
    int new_refs = m.allocno_n_refs[a];
    int new_len = m.allocno_live_length[a];
    if (FindBestRefsAndLengthInPriorityRange(m.allocno_n_refs[a], m.allocno_live_length[a],
                                             m.allocno_size[a], m.allocno_calls_crossed[a],
                                             mips_cfg.caller_saves, max_density[a], min_pri,
                                             max_pri, &new_refs, &new_len, lock_allocno_refs[a])) {
      GlobalAllocModel test_m = m;
      test_m.allocno_n_refs[a] = new_refs;
      test_m.allocno_live_length[a] = new_len;
      const SimResult test_sim = SimulateGlobalAlloc(test_m, best_order);
      if (SortAllocnoOrder(test_m) == best_order &&
          test_sim.primary_mismatches <= best_sim.primary_mismatches) {
        const int regno = m.allocno_reg[a];
        out->sideband.reg_n_refs[regno] = new_refs;
        out->sideband.reg_live_length[regno] = new_len;
        return;
      }
    }
  }

  // General path: realize `best_order` by minimally adjusting adjacent priority
  // inversions (raising prefixes leftward when a tight integer gap requires
  // room), with a right-to-left monotone fallback if discrete priority gaps
  // prevent local pairwise steps from converging.
  GlobalAllocModel work_m = m;
  const int max_guard = m.max_allocno * m.max_allocno * 2 + 64;
  for (int guard = 0; guard < max_guard; ++guard) {
    if (SortAllocnoOrder(work_m) == best_order) {
      break;
    }
    bool changed = false;
    for (int k = 0; k + 1 < m.max_allocno; ++k) {
      const int u = best_order[k];
      const int v = best_order[k + 1];
      const int pri_u = ComputeAllocnoPriority(
          work_m.allocno_n_refs[u], work_m.allocno_live_length[u], work_m.allocno_size[u]);
      const int pri_v = ComputeAllocnoPriority(
          work_m.allocno_n_refs[v], work_m.allocno_live_length[v], work_m.allocno_size[v]);
      const int needed_gap = (u > v) ? 1 : 0;
      if (pri_u >= pri_v + needed_gap) {
        continue;
      }

      // First try lowering `v` below `u` without crossing `best_order[k + 2]`.
      int v_min_pri = 0;
      if (k + 2 < m.max_allocno) {
        const int w = best_order[k + 2];
        const int pri_w = ComputeAllocnoPriority(
            work_m.allocno_n_refs[w], work_m.allocno_live_length[w], work_m.allocno_size[w]);
        v_min_pri = pri_w + (v > w ? 1 : 0);
      }
      const int v_max_pri = pri_u - needed_gap;
      int cand_refs = work_m.allocno_n_refs[v];
      int cand_len = work_m.allocno_live_length[v];
      if (FindBestRefsAndLengthInPriorityRange(
              m.allocno_n_refs[v], m.allocno_live_length[v], work_m.allocno_size[v],
              work_m.allocno_calls_crossed[v], mips_cfg.caller_saves, max_density[v], v_min_pri,
              v_max_pri, &cand_refs, &cand_len, lock_allocno_refs[v])) {
        work_m.allocno_n_refs[v] = cand_refs;
        work_m.allocno_live_length[v] = cand_len;
        changed = true;
        break;
      }

      // Otherwise raise `u` above `v` (cascading leftward on subsequent
      // iterations if needed, and widening to `std::numeric_limits<int>::max()`
      // if discrete integer priority gaps leave `[pri_v + needed_gap, u_max_pri]` empty).
      int u_max_pri = std::numeric_limits<int>::max();
      if (k > 0) {
        const int p = best_order[k - 1];
        const int pri_p = ComputeAllocnoPriority(
            work_m.allocno_n_refs[p], work_m.allocno_live_length[p], work_m.allocno_size[p]);
        u_max_pri = std::max(pri_v + needed_gap + 5000, pri_p - (p > u ? 1 : 0));
      }
      if (FindBestRefsAndLengthInPriorityRange(
              m.allocno_n_refs[u], m.allocno_live_length[u], work_m.allocno_size[u],
              work_m.allocno_calls_crossed[u], mips_cfg.caller_saves, max_density[u],
              pri_v + needed_gap, u_max_pri, &cand_refs, &cand_len, lock_allocno_refs[u]) ||
          (u_max_pri < std::numeric_limits<int>::max() &&
           FindBestRefsAndLengthInPriorityRange(
               m.allocno_n_refs[u], m.allocno_live_length[u], work_m.allocno_size[u],
               work_m.allocno_calls_crossed[u], mips_cfg.caller_saves, max_density[u],
               pri_v + needed_gap, std::numeric_limits<int>::max(), &cand_refs, &cand_len,
               lock_allocno_refs[u]))) {
        work_m.allocno_n_refs[u] = cand_refs;
        work_m.allocno_live_length[u] = cand_len;
        changed = true;
        break;
      }
    }
    if (!changed) {
      break;
    }
  }

  if (SortAllocnoOrder(work_m) != best_order) {
    GlobalAllocModel mono_m = m;
    bool mono_ok = true;
    int min_req = 0;
    for (int k = m.max_allocno - 1; k >= 0; --k) {
      const int u = best_order[k];
      if (k + 1 < m.max_allocno) {
        const int v = best_order[k + 1];
        const int pri_v = ComputeAllocnoPriority(
            mono_m.allocno_n_refs[v], mono_m.allocno_live_length[v], mono_m.allocno_size[v]);
        min_req = pri_v + ((u > v) ? 1 : 0);
      }
      int cand_refs = m.allocno_n_refs[u];
      int cand_len = m.allocno_live_length[u];
      if (!FindLowestPriorityAtLeast(m.allocno_n_refs[u], m.allocno_live_length[u],
                                     mono_m.allocno_size[u], mono_m.allocno_calls_crossed[u],
                                     mips_cfg.caller_saves, max_density[u], min_req, &cand_refs,
                                     &cand_len, lock_allocno_refs[u]) &&
          (!lock_allocno_refs[u] ||
           !FindLowestPriorityAtLeast(m.allocno_n_refs[u], m.allocno_live_length[u],
                                      mono_m.allocno_size[u], mono_m.allocno_calls_crossed[u],
                                      mips_cfg.caller_saves, max_density[u], min_req, &cand_refs,
                                      &cand_len, /*lock_refs=*/false))) {
        mono_ok = false;
        break;
      }
      mono_m.allocno_n_refs[u] = cand_refs;
      mono_m.allocno_live_length[u] = cand_len;
    }
    if (mono_ok && SortAllocnoOrder(mono_m) == best_order) {
      work_m = std::move(mono_m);
    }
  }

  if (SortAllocnoOrder(work_m) == best_order) {
    const SimResult work_sim = SimulateGlobalAlloc(work_m, best_order);
    if (work_sim.primary_mismatches <= best_sim.primary_mismatches) {
      for (int a = 0; a < m.max_allocno; ++a) {
        const int regno = m.allocno_reg[a];
        if (work_m.allocno_n_refs[a] != m.allocno_n_refs[a]) {
          out->sideband.reg_n_refs[regno] = work_m.allocno_n_refs[a];
        }
        if (work_m.allocno_live_length[a] != m.allocno_live_length[a]) {
          out->sideband.reg_live_length[regno] = work_m.allocno_live_length[a];
        }
      }
    }
  }
}

absl::Status ValidateInvertGlobalAllocInput(const RtlControlFlowGraph& greg_cfg,
                                            int* max_seen_regno_out) {
  if (greg_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertGlobalAlloc: input CFG has no basic blocks");
  }

  const size_t num_blocks = greg_cfg.rpo_blocks.size();
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = greg_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: block %zu has out-of-bounds successor %zu (num_blocks=%zu)", b,
            succ, num_blocks));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: block %zu has out-of-bounds addr_vec target %zu (num_blocks=%zu)",
            b, target, num_blocks));
      }
    }
  }

  int max_seen_regno = std::max(greg_cfg.sideband.max_regno, kFirstPseudoRegister);

  for (const auto& [regno, hard_reg] : greg_cfg.sideband.reg_renumber) {
    if (regno < kFirstPseudoRegister || hard_reg < -1 || hard_reg >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: invalid sideband reg_renumber entry (%d -> %d)", regno, hard_reg));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_refs) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_refs", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_sets) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_sets", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_deaths) {
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: negative regno %d in sideband reg_n_deaths", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_live_length) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative regno %d in sideband reg_live_length", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }
  for (const auto& [regno, _] : greg_cfg.sideband.reg_n_calls_crossed) {
    if (regno < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative regno %d in sideband reg_n_calls_crossed", regno));
    }
    max_seen_regno = std::max(max_seen_regno, regno + 1);
  }

  for (const auto& [bb_idx, live_set] : greg_cfg.sideband.basic_block_live_at_start) {
    if (bb_idx < 0) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "InvertGlobalAlloc: negative block index %d in sideband basic_block_live_at_start",
          bb_idx));
    }
    for (int regno : live_set) {
      if (regno < 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: negative regno %d in sideband basic_block_live_at_start", regno));
      }
      max_seen_regno = std::max(max_seen_regno, regno + 1);
    }
  }
  for (int gpr : greg_cfg.sideband.saved_gpr_regnos) {
    if (gpr < 0 || gpr >= kFirstFprRegnum) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid GPR %d in sideband saved_gpr_regnos", gpr));
    }
  }
  for (int fpr : greg_cfg.sideband.saved_fpr_regnos) {
    if (fpr < kFirstFprRegnum || fpr > kLastFprRegnum) {
      return absl::InvalidArgumentError(
          absl::StrFormat("InvertGlobalAlloc: invalid FPR %d in sideband saved_fpr_regnos", fpr));
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = greg_cfg.rpo_blocks[b];
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("InvertGlobalAlloc: failed to parse instruction %zu in block %zu: %s",
                            i, b, parsed.status().message()));
      }
      if (parsed->pattern.op == RtlOp::kNil || parsed->pattern.op == RtlOp::kUnknown) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "InvertGlobalAlloc: invalid pattern opcode at instruction %zu in block %zu", i, b));
      }
      if (absl::Status st = ValidateRtlExprRegisters(parsed->pattern, &max_seen_regno); !st.ok()) {
        return st;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (absl::Status st = ValidateRtlExprRegisters(note.datum, &max_seen_regno); !st.ok()) {
          return st;
        }
      }
      for (const RtlExpr& fu : parsed->call_fusage) {
        if (absl::Status st = ValidateRtlExprRegisters(fu, &max_seen_regno); !st.ok()) {
          return st;
        }
      }
    }
  }
  *max_seen_regno_out = max_seen_regno;
  return absl::OkStatus();
}

RtlControlFlowGraph InvertStupidGlobalAlloc(const RtlControlFlowGraph& greg_cfg,
                                            const CompilationProfile& profile, int max_seen_regno) {
  RtlControlFlowGraph out = greg_cfg;
  out.pass = RtlPassKind::kLreg;
  out.sideband.max_regno = max_seen_regno;
  absl::erase_if(out.sideband.reg_renumber, [](const auto& kv) {
    return kv.first < kFirstPseudoRegister || kv.second < 0 || kv.second >= kFirstPseudoRegister;
  });
  ReconstructRtlLiveness(
      &out, profile,
      {.reload_completed = false, .update_reg_notes = false, .preserve_phantom_pseudos = true});
  return out;
}

int ComputeLocalQtyPriority(int n_refs, int span_len, int qty_size) {
  if (span_len <= 0) {
    span_len = 1;
  }
  return static_cast<int>(
      ((static_cast<double>(FloorLog2(n_refs) * n_refs * qty_size)) / span_len) * 10000.0);
}

bool ModesTieableForLocalAlloc(const RegModeInfo& m1, const RegModeInfo& m2) {
  return IsFloatOrComplexFloatMode(m1) == IsFloatOrComplexFloatMode(m2);
}

struct LocalBlockQty {
  int qty_id = 0;
  int first_reg = -1;
  std::vector<int> member_regs;
  std::vector<int> member_offsets;
  RegModeInfo mode;
  int size = 1;
  int birth = -1;
  int death = -1;
  int calls_crossed = 0;
  MipsRegClass min_class = MipsRegClass::kGrRegs;
  MipsRegClass alt_class = MipsRegClass::kNoRegs;
  int n_refs = 1;
  HardRegSet phys_copy_sugg;
  int num_copy_sugg = 0;
  HardRegSet phys_sugg;
  int num_sugg = 0;
};

struct LocalBlockAllocModel {
  MipsTargetConfig mips_cfg;
  HardRegSet fixed_regs;
  HardRegSet call_used_regs;
  std::vector<HardRegSet> base_regs_live_at;
  std::vector<LocalBlockQty> qtys;
  std::vector<int> eligible_pseudos;
  std::vector<int> reg_offset;
  absl::flat_hash_map<int, int> target_hr_by_pseudo;
};

int SimulateLocalBlockAlloc(const LocalBlockAllocModel& m,
                            const std::vector<int>* custom_pass2_order,
                            absl::flat_hash_map<int, int>* sim_hr_out = nullptr) {
  const int next_qty = static_cast<int>(m.qtys.size());
  std::vector<HardRegSet> regs_live_at = m.base_regs_live_at;
  std::vector<int> qty_phys_reg(next_qty, -1);
  std::vector<int> num_copy_sugg(next_qty, 0);
  for (int q = 0; q < next_qty; ++q) {
    num_copy_sugg[q] = m.qtys[q].num_copy_sugg;
  }

  auto post_mark_life = [&](int regno, const RegModeInfo& mode, int birth, int death) {
    const int nregs = HardRegnoNregs(regno, mode, m.mips_cfg);
    const int hi_idx = std::min(death, static_cast<int>(regs_live_at.size()));
    for (int ins = std::max(0, birth); ins < hi_idx; ++ins) {
      for (int k = 0; k < nregs && regno + k < kFirstPseudoRegister; ++k) {
        regs_live_at[ins].SetBit(regno + k);
      }
    }
  };

  auto find_free_reg = [&](auto& self, MipsRegClass cls, const RegModeInfo& mode, int q,
                           bool accept_call_clobbered, bool just_try_suggested, int born_idx,
                           int dead_idx) -> int {
    if (born_idx < 0 || born_idx > dead_idx) {
      return -1;
    }
    HardRegSet used =
        (accept_call_clobbered || m.qtys[q].calls_crossed == 0) ? m.fixed_regs : m.call_used_regs;
    const int hi_idx = std::min(dead_idx, static_cast<int>(regs_live_at.size()));
    for (int ins = born_idx; ins < hi_idx; ++ins) {
      used.Ior(regs_live_at[ins]);
    }
    used.IorCompl(RegClassContents(cls));
    used.SetBit(kArgPointerRegnum);
    used.SetBit(kFramePointerRegnum);

    HardRegSet first_used = used;
    if (just_try_suggested) {
      if (num_copy_sugg[q] != 0) {
        first_used.IorCompl(m.qtys[q].phys_copy_sugg);
      } else {
        first_used.IorCompl(m.qtys[q].phys_sugg);
      }
    }

    const HardRegSet all_regs = RegClassContents(MipsRegClass::kAllRegs);
    const bool all_excluded =
        (all_regs.lo & ~first_used.lo) == 0 && ((all_regs.hi & 0xfULL) & ~first_used.hi) == 0;
    if (!all_excluded) {
      for (int i = 0; i < kFirstPseudoRegister; ++i) {
        const int regno = i;
        if (!first_used.TestBit(regno) && HardRegnoModeOk(regno, mode, m.mips_cfg)) {
          const int size1 = HardRegnoNregs(regno, mode, m.mips_cfg);
          int j = 1;
          while (j < size1 && regno + j < kFirstPseudoRegister && !used.TestBit(regno + j)) {
            ++j;
          }
          if (j == size1) {
            post_mark_life(regno, mode, born_idx, dead_idx);
            return regno;
          }
          i += j;
        }
      }
    }

    if (just_try_suggested && num_copy_sugg[q] != 0 && m.qtys[q].num_sugg != 0) {
      num_copy_sugg[q] = 0;
      return self(self, cls, mode, q, accept_call_clobbered, true, born_idx, dead_idx);
    }
    if (!accept_call_clobbered && m.mips_cfg.caller_saves && !just_try_suggested &&
        m.qtys[q].calls_crossed != 0 && (4 * m.qtys[q].calls_crossed < m.qtys[q].n_refs)) {
      return self(self, cls, mode, q, true, false, born_idx, dead_idx);
    }
    return -1;
  };

  auto qty_pri = [&](int q) {
    return ComputeLocalQtyPriority(m.qtys[q].n_refs, m.qtys[q].death - m.qtys[q].birth,
                                   m.qtys[q].size);
  };

  std::vector<int> custom_rank;
  if (custom_pass2_order != nullptr) {
    custom_rank.assign(next_qty, next_qty);
    for (int pos = 0; pos < static_cast<int>(custom_pass2_order->size()); ++pos) {
      const int q = (*custom_pass2_order)[pos];
      if (q >= 0 && q < next_qty) {
        custom_rank[q] = pos;
      }
    }
  }

  auto qty_sugg_cmp = [&](int q1, int q2) {
    const int s1 =
        (num_copy_sugg[q1] != 0) ? num_copy_sugg[q1] : m.qtys[q1].num_sugg * kFirstPseudoRegister;
    const int s2 =
        (num_copy_sugg[q2] != 0) ? num_copy_sugg[q2] : m.qtys[q2].num_sugg * kFirstPseudoRegister;
    if (s1 != s2) {
      return s1 - s2;
    }
    if (!custom_rank.empty()) {
      return custom_rank[q1] - custom_rank[q2];
    }
    return qty_pri(q2) - qty_pri(q1);
  };

  std::vector<int> sugg_order(next_qty);
  for (int i = 0; i < next_qty; ++i) {
    sugg_order[i] = i;
  }
  switch (next_qty) {
    case 3:
      if (qty_sugg_cmp(0, 1) > 0) {
        std::swap(sugg_order[0], sugg_order[1]);
      }
      if (qty_sugg_cmp(1, 2) > 0) {
        std::swap(sugg_order[2], sugg_order[1]);
      }
      [[fallthrough]];
    case 2:
      if (qty_sugg_cmp(0, 1) > 0) {
        std::swap(sugg_order[0], sugg_order[1]);
      }
      break;
    case 1:
    case 0:
      break;
    default:
      std::sort(sugg_order.begin(), sugg_order.end(), [&](int q1, int q2) {
        const int cmp = qty_sugg_cmp(q1, q2);
        return (cmp != 0) ? (cmp < 0) : (q1 < q2);
      });
      break;
  }

  for (int q : sugg_order) {
    if (m.qtys[q].num_sugg != 0 || num_copy_sugg[q] != 0) {
      qty_phys_reg[q] = find_free_reg(find_free_reg, m.qtys[q].min_class, m.qtys[q].mode, q, false,
                                      true, m.qtys[q].birth, m.qtys[q].death);
    }
  }

  auto qty_cmp = [&](int q1, int q2) {
    if (!custom_rank.empty()) {
      return custom_rank[q1] - custom_rank[q2];
    }
    const int p1 = qty_pri(q1);
    const int p2 = qty_pri(q2);
    return p2 - p1;
  };

  std::vector<int> pass2_order(next_qty);
  for (int i = 0; i < next_qty; ++i) {
    pass2_order[i] = i;
  }
  switch (next_qty) {
    case 3:
      if (qty_cmp(0, 1) > 0) {
        std::swap(pass2_order[0], pass2_order[1]);
      }
      if (qty_cmp(1, 2) > 0) {
        std::swap(pass2_order[2], pass2_order[1]);
      }
      [[fallthrough]];
    case 2:
      if (qty_cmp(0, 1) > 0) {
        std::swap(pass2_order[0], pass2_order[1]);
      }
      break;
    case 1:
    case 0:
      break;
    default:
      std::sort(pass2_order.begin(), pass2_order.end(), [&](int q1, int q2) {
        const int cmp = qty_cmp(q1, q2);
        return (cmp != 0) ? (cmp < 0) : (q1 < q2);
      });
      break;
  }

  for (int q : pass2_order) {
    if (qty_phys_reg[q] < 0) {
      qty_phys_reg[q] = find_free_reg(find_free_reg, m.qtys[q].min_class, m.qtys[q].mode, q, false,
                                      false, m.qtys[q].birth, m.qtys[q].death);
      if (qty_phys_reg[q] < 0 && m.qtys[q].alt_class != MipsRegClass::kNoRegs) {
        qty_phys_reg[q] = find_free_reg(find_free_reg, m.qtys[q].alt_class, m.qtys[q].mode, q,
                                        false, false, m.qtys[q].birth, m.qtys[q].death);
      }
    }
  }

  absl::flat_hash_map<int, int> sim_hr;
  for (int p : m.eligible_pseudos) {
    sim_hr[p] = -1;
  }
  for (int q = 0; q < next_qty; ++q) {
    if (qty_phys_reg[q] >= 0) {
      for (size_t k = 0; k < m.qtys[q].member_regs.size(); ++k) {
        sim_hr[m.qtys[q].member_regs[k]] = qty_phys_reg[q] + m.qtys[q].member_offsets[k];
      }
    }
  }
  int mismatches = 0;
  for (int p : m.eligible_pseudos) {
    const int target = m.target_hr_by_pseudo.at(p);
    if (sim_hr[p] != target) {
      ++mismatches;
    }
  }
  if (sim_hr_out != nullptr) {
    *sim_hr_out = std::move(sim_hr);
  }
  return mismatches;
}

std::optional<LocalBlockAllocModel> BuildSingleBlockLocalAllocModel(
    size_t b, RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::flat_hash_set<int>& live_at_any_block_start,
    const absl::flat_hash_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::flat_hash_map<int, RegModeInfo>& pseudo_modes,
    const absl::flat_hash_set<int>& fp_op_pseudos,
    const absl::flat_hash_set<int>& gr_or_mem_op_pseudos, bool bump_caller_saves_refs) {
  const int max_regno = std::max(dst_out->sideband.max_regno, kFirstPseudoRegister);
  std::vector<int> eligible_pseudos;
  for (int p = kFirstPseudoRegister; p < max_regno; ++p) {
    if (live_at_any_block_start.contains(p)) {
      continue;
    }
    const auto refs_it = dst_out->sideband.reg_n_refs.find(p);
    const auto deaths_it = dst_out->sideband.reg_n_deaths.find(p);
    const auto len_it = dst_out->sideband.reg_live_length.find(p);
    if (refs_it == dst_out->sideband.reg_n_refs.end() || refs_it->second <= 0 ||
        deaths_it == dst_out->sideband.reg_n_deaths.end() || deaths_it->second != 1 ||
        (len_it != dst_out->sideband.reg_live_length.end() && len_it->second < 0)) {
      continue;
    }
    const auto sum_it = pseudo_summaries.find(p);
    if (sum_it == pseudo_summaries.end() || sum_it->second.blocks_referenced.size() != 1 ||
        *sum_it->second.blocks_referenced.begin() != b || !sum_it->second.has_set_in_block ||
        sum_it->second.used_before_set || sum_it->second.alive_after_redef ||
        sum_it->second.note_deaths >= 2) {
      continue;
    }
    if (sum_it->second.def_count_in_block == 1 &&
        sum_it->second.single_reg_md_class != MipsRegClass::kNoRegs &&
        !sum_it->second.used_in_mem_address && sum_it->second.total_read_count <= 1) {
      continue;
    }
    eligible_pseudos.push_back(p);
  }

  const HardRegSet call_used = CallUsedRegSet(mips_cfg);
  if (bump_caller_saves_refs && mips_cfg.caller_saves) {
    for (int p : eligible_pseudos) {
      const auto ren_it = dst_out->sideband.reg_renumber.find(p);
      const auto cc_it = dst_out->sideband.reg_n_calls_crossed.find(p);
      if (ren_it != dst_out->sideband.reg_renumber.end() && ren_it->second >= 0 &&
          call_used.TestBit(ren_it->second) &&
          cc_it != dst_out->sideband.reg_n_calls_crossed.end() && cc_it->second > 0) {
        const int min_refs = 4 * cc_it->second + 1;
        int& cur_refs = dst_out->sideband.reg_n_refs[p];
        cur_refs = std::max(cur_refs, min_refs);
      }
    }
  }
  if (eligible_pseudos.empty()) {
    return std::nullopt;
  }

  LocalBlockAllocModel m;
  m.mips_cfg = mips_cfg;
  m.fixed_regs = FixedRegSet(mips_cfg);
  m.call_used_regs = call_used;
  m.eligible_pseudos = eligible_pseudos;

  std::vector<int> reg_qty(max_regno, -1);
  m.reg_offset.assign(max_regno, 0);
  auto& reg_offset = m.reg_offset;
  std::vector<MipsRegClass> pref_class(max_regno, MipsRegClass::kGrRegs);
  std::vector<MipsRegClass> alt_class(max_regno, MipsRegClass::kNoRegs);
  for (int p : eligible_pseudos) {
    reg_qty[p] = -2;
    const auto ren_it = dst_out->sideband.reg_renumber.find(p);
    const int target_hr = (ren_it != dst_out->sideband.reg_renumber.end()) ? ren_it->second : -1;
    m.target_hr_by_pseudo[p] = target_hr;
    const auto m_it = pseudo_modes.find(p);
    const RegModeInfo pmode =
        (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
    if (IsFpCcMode(pmode)) {
      pref_class[p] = MipsRegClass::kStRegs;
    } else if (IsFloatOrComplexFloatMode(pmode) && !mips_cfg.soft_float) {
      if (!fp_op_pseudos.contains(p) &&
          ((target_hr >= 0 && target_hr < kFirstFprRegnum) || gr_or_mem_op_pseudos.contains(p))) {
        if (target_hr >= kFirstFprRegnum) {
          pref_class[p] = MipsRegClass::kFpRegs;
          alt_class[p] = mips_cfg.mem_move_cost_6 ? MipsRegClass::kAllRegs : MipsRegClass::kNoRegs;
        } else {
          pref_class[p] = MipsRegClass::kGrRegs;
          alt_class[p] = MipsRegClass::kAllRegs;
        }
      } else {
        pref_class[p] = MipsRegClass::kFpRegs;
        alt_class[p] = mips_cfg.mem_move_cost_6 ? MipsRegClass::kAllRegs : MipsRegClass::kNoRegs;
      }
    } else if (pseudo_summaries.at(p).total_def_count == 1 &&
               pseudo_summaries.at(p).single_reg_md_class != MipsRegClass::kNoRegs &&
               !pseudo_summaries.at(p).used_in_mem_address &&
               pseudo_summaries.at(p).total_read_count <= 1) {
      pref_class[p] = pseudo_summaries.at(p).single_reg_md_class;
      alt_class[p] = MipsRegClass::kGrRegs;
    } else if (!mips_cfg.gp_64bit && pseudo_summaries.at(p).total_def_count == 1 &&
               pseudo_summaries.at(p).defined_by_di_mult &&
               !pseudo_summaries.at(p).used_in_mem_address &&
               pseudo_summaries.at(p).total_read_count <= 1) {
      pref_class[p] = MipsRegClass::kMdRegs;
      alt_class[p] = MipsRegClass::kGrRegs;
    } else {
      pref_class[p] = MipsRegClass::kGrRegs;
      alt_class[p] = MipsRegClass::kNoRegs;
    }
  }

  const RtlBasicBlock& out_blk = dst_out->rpo_blocks[b];
  const RtlBasicBlock& live_blk = liveness_cfg.rpo_blocks[b];
  int insn_count = out_blk.has_label ? 1 : 0;
  std::vector<std::optional<RtlPatternAndNotes>> parsed_insns(out_blk.instructions.size());
  for (size_t i = 0; i < out_blk.instructions.size(); ++i) {
    auto pn = ParsePatternAndNotes(out_blk.instructions[i]);
    if (!pn.ok()) {
      return std::nullopt;
    }
    if (ExprContainsOp(pn->pattern, RtlOp::kCall) && i < live_blk.instructions.size()) {
      if (auto live_pn = ParsePatternAndNotes(live_blk.instructions[i]); live_pn.ok()) {
        pn->reg_notes = std::move(live_pn->reg_notes);
      }
    }
    if (pn->pattern.op != RtlOp::kNote) {
      ++insn_count;
    }
    parsed_insns[i] = std::move(*pn);
  }

  m.base_regs_live_at.assign(static_cast<size_t>(2 * insn_count + 2), {});
  HardRegSet regs_live;
  if (auto live_it = dst_out->sideband.basic_block_live_at_start.find(static_cast<int>(b));
      live_it != dst_out->sideband.basic_block_live_at_start.end()) {
    for (int r : live_it->second) {
      if (r >= 0 && r < kFirstPseudoRegister) {
        regs_live.SetBit(r);
      }
    }
  }

  auto mark_life = [&](int regno, const RegModeInfo& mode, bool life) {
    const int nregs = HardRegnoNregs(regno, mode, mips_cfg);
    for (int j = 0; j < nregs && regno + j < kFirstPseudoRegister; ++j) {
      if (life) {
        regs_live.SetBit(regno + j);
      } else {
        regs_live.ClearBit(regno + j);
      }
    }
  };
  auto post_mark_base = [&](int regno, const RegModeInfo& mode, int birth, int death) {
    const int nregs = HardRegnoNregs(regno, mode, mips_cfg);
    for (int ins = std::max(0, birth);
         ins < death && ins < static_cast<int>(m.base_regs_live_at.size()); ++ins) {
      for (int j = 0; j < nregs && regno + j < kFirstPseudoRegister; ++j) {
        m.base_regs_live_at[ins].SetBit(regno + j);
      }
    }
  };
  auto alloc_pseudo_qty = [&](int regno, const RegModeInfo& mode, int birth) {
    const int q = static_cast<int>(m.qtys.size());
    reg_qty[regno] = q;
    reg_offset[regno] = 0;
    LocalBlockQty qty;
    qty.qty_id = q;
    qty.first_reg = regno;
    qty.member_regs = {regno};
    qty.member_offsets = {0};
    qty.mode = mode;
    qty.size = PseudoRegnoSize(mode, mips_cfg);
    qty.birth = birth;
    qty.death = -1;
    if (auto cc_it = dst_out->sideband.reg_n_calls_crossed.find(regno);
        cc_it != dst_out->sideband.reg_n_calls_crossed.end()) {
      qty.calls_crossed = cc_it->second;
    }
    qty.min_class = pref_class[regno];
    qty.alt_class = alt_class[regno];
    qty.n_refs = dst_out->sideband.reg_n_refs.at(regno);
    m.qtys.push_back(std::move(qty));
  };
  auto reg_is_born = [&](const RtlExpr& reg, int birth, int cur_insn_num) {
    int regno = -1;
    RegModeInfo rmode{reg.mode, reg.raw_mode};
    if (reg.op == RtlOp::kSubreg && !reg.children.empty() && reg.children[0].op == RtlOp::kReg) {
      regno = (reg.children[0].IsArgPointerReg() ? kArgPointerRegnum : reg.children[0].RegNo()) +
              static_cast<int>(reg.int_val);
    } else if (reg.op == RtlOp::kReg) {
      regno = reg.IsArgPointerReg() ? kArgPointerRegnum : reg.RegNo();
    } else {
      return;
    }
    if (regno < kFirstPseudoRegister) {
      mark_life(regno, rmode, true);
      if (birth < 2 * cur_insn_num) {
        post_mark_base(regno, rmode, birth, 2 * cur_insn_num);
      }
    } else if (regno < max_regno) {
      if (reg_qty[regno] == -2) {
        const auto m_it = pseudo_modes.find(regno);
        const RegModeInfo pmode = (m_it != pseudo_modes.end()) ? m_it->second : rmode;
        alloc_pseudo_qty(regno, pmode, birth);
      }
      if (reg_qty[regno] >= 0) {
        m.qtys[reg_qty[regno]].death = -1;
      }
    }
  };

  int insn_number = 0;
  if (out_blk.has_label) {
    insn_number = 1;
    m.base_regs_live_at[2].Ior(regs_live);
    m.base_regs_live_at[3].Ior(regs_live);
  }

  const int units_per_word = mips_cfg.gp_64bit ? 8 : 4;
  for (size_t i = 0; i < parsed_insns.size(); ++i) {
    const RtlPatternAndNotes& pn = *parsed_insns[i];
    if (pn.pattern.op == RtlOp::kNote) {
      m.base_regs_live_at[2 * insn_number].Ior(regs_live);
      m.base_regs_live_at[2 * insn_number + 1].Ior(regs_live);
      continue;
    }
    ++insn_number;
    if (pn.pattern.op == RtlOp::kCodeLabel || pn.pattern.op == RtlOp::kBarrier ||
        pn.pattern.op == RtlOp::kAddrVec) {
      m.base_regs_live_at[2 * insn_number].Ior(regs_live);
      m.base_regs_live_at[2 * insn_number + 1].Ior(regs_live);
      continue;
    }

    // Identify `(clobber (reg:M hr))` scratch clobbers that `InvertBlockAlloc`
    // restores to `(clobber (scratch:M))` before `block_alloc` runs.
    std::vector<std::pair<int, RegModeInfo>> insn_scratches;
    auto is_match_scratch_parallel = [&](const RtlExpr& par) {
      if (par.op != RtlOp::kParallel || ExprContainsOp(par, RtlOp::kCall) ||
          ExprContainsOp(par, RtlOp::kUnspecVolatile) || ExprContainsOp(par, RtlOp::kAsmOperands)) {
        return false;
      }
      for (const RtlExpr& elem : par.children) {
        if (elem.op != RtlOp::kSet || elem.children.size() != 2) {
          continue;
        }
        const RtlExpr& dst = elem.children[0];
        const RtlExpr& src = elem.children[1];
        if (dst.op == RtlOp::kMem && dst.mode == RtlMachineMode::kBlk && src.op == RtlOp::kMem &&
            src.mode == RtlMachineMode::kBlk) {
          return true;
        }
        if (src.op == RtlOp::kMult || src.op == RtlOp::kDiv || src.op == RtlOp::kMod ||
            src.op == RtlOp::kUdiv || src.op == RtlOp::kUmod || src.op == RtlOp::kTruncate ||
            src.op == RtlOp::kFfs || src.op == RtlOp::kFix ||
            (src.op == RtlOp::kPlus && ExprContainsOp(src, RtlOp::kMult))) {
          return true;
        }
      }
      return false;
    };
    if (is_match_scratch_parallel(pn.pattern)) {
      for (const RtlExpr& elem : pn.pattern.children) {
        if (elem.op == RtlOp::kClobber && elem.children.size() == 1 &&
            elem.children[0].op == RtlOp::kReg && !elem.children[0].IsArgPointerReg()) {
          const int hr = elem.children[0].RegNo();
          const bool is_fixed = (hr == 0 || hr == 1 || hr == 26 || hr == 27 || hr == 28 ||
                                 hr == 29 || hr == 31 || hr == 67);
          if (hr >= 0 && hr < kFirstPseudoRegister && !is_fixed) {
            insn_scratches.push_back({hr, {elem.children[0].mode, elem.children[0].raw_mode}});
          }
        }
      }
    }

    int combined_regno = -1;
    const RtlExpr* primary_set = SingleSet(pn);
    if (primary_set != nullptr && primary_set->children.size() == 2 &&
        (primary_set->children[0].op == RtlOp::kReg ||
         primary_set->children[0].op == RtlOp::kSubreg) &&
        !ExprContainsOp(pn.pattern, RtlOp::kCall)) {
      const RtlExpr& r0 = primary_set->children[0];
      const RtlExpr& rhs = primary_set->children[1];
      const bool earlyclobber_di =
          !mips_cfg.gp_64bit && r0.mode == RtlMachineMode::kDi &&
          (rhs.op == RtlOp::kAshift || rhs.op == RtlOp::kAshiftrt || rhs.op == RtlOp::kLshiftrt) &&
          rhs.children.size() == 2 && !rhs.children[1].IsConstInt();
      const bool earlyclobber_mul =
          (rhs.op == RtlOp::kMult && r0.mode == RtlMachineMode::kSi && !insn_scratches.empty());
      if (!earlyclobber_di && !earlyclobber_mul) {
        std::vector<const RtlExpr*> cand_r1s;
        auto collect_cand_r1 = [&](const auto& self, const RtlExpr& e) -> void {
          if (e.op == RtlOp::kReg ||
              (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg)) {
            cand_r1s.push_back(&e);
            return;
          }
          if (e.op == RtlOp::kMem) {
            return;
          }
          for (const RtlExpr& ch : e.children) {
            self(self, ch);
          }
        };
        collect_cand_r1(collect_cand_r1, rhs);

        for (const RtlExpr* r1_ptr : cand_r1s) {
          bool may_save_copy = (pn.pattern.op == RtlOp::kSet && &rhs == r1_ptr);
          int offset = 0;
          const RtlExpr* u_expr = r1_ptr;
          while (u_expr->op == RtlOp::kSubreg && !u_expr->children.empty()) {
            if (ModeByteSize({u_expr->children[0].mode, u_expr->children[0].raw_mode}) >
                units_per_word) {
              may_save_copy = false;
            }
            offset += static_cast<int>(u_expr->int_val);
            u_expr = &u_expr->children[0];
          }
          const RtlExpr* s_expr = &r0;
          while (s_expr->op == RtlOp::kSubreg && !s_expr->children.empty()) {
            if (ModeByteSize({s_expr->children[0].mode, s_expr->children[0].raw_mode}) >
                units_per_word) {
              may_save_copy = false;
            }
            offset -= static_cast<int>(s_expr->int_val);
            s_expr = &s_expr->children[0];
          }
          if (u_expr->op != RtlOp::kReg || s_expr->op != RtlOp::kReg) {
            continue;
          }
          const int ureg = u_expr->IsArgPointerReg() ? kArgPointerRegnum : u_expr->RegNo();
          const int sreg = s_expr->IsArgPointerReg() ? kArgPointerRegnum : s_expr->RegNo();
          const RegModeInfo umode{u_expr->mode, u_expr->raw_mode};
          const RegModeInfo smode{s_expr->mode, s_expr->raw_mode};
          const int usize = (ureg < kFirstPseudoRegister) ? HardRegnoNregs(ureg, umode, mips_cfg)
                                                          : PseudoRegnoSize(umode, mips_cfg);
          const int ssize = (sreg < kFirstPseudoRegister) ? HardRegnoNregs(sreg, smode, mips_cfg)
                                                          : PseudoRegnoSize(smode, mips_cfg);
          if ((ureg >= kFirstPseudoRegister && (ureg >= max_regno || reg_qty[ureg] < 0)) ||
              (offset > 0 && usize + offset > ssize) || (offset < 0 && usize + offset < ssize) ||
              (ssize > usize && ureg >= kFirstPseudoRegister &&
               usize < m.qtys[reg_qty[ureg]].size) ||
              (sreg >= kFirstPseudoRegister && (sreg >= max_regno || reg_qty[sreg] == -1)) ||
              ureg == sreg || (ureg < kFirstPseudoRegister && sreg < kFirstPseudoRegister) ||
              !ModesTieableForLocalAlloc(umode, smode)) {
            continue;
          }
          if (ureg < kFirstPseudoRegister) {
            if (reg_qty[sreg] == -2) {
              reg_is_born(*s_expr, 2 * insn_number, insn_number);
            }
            if (reg_qty[sreg] >= 0) {
              LocalBlockQty& sq = m.qtys[reg_qty[sreg]];
              if (may_save_copy && !sq.phys_copy_sugg.TestBit(ureg)) {
                sq.phys_copy_sugg.SetBit(ureg);
                ++sq.num_copy_sugg;
              } else if (!may_save_copy && !sq.phys_sugg.TestBit(ureg)) {
                sq.phys_sugg.SetBit(ureg);
                ++sq.num_sugg;
              }
            }
            continue;
          }
          if (sreg < kFirstPseudoRegister) {
            LocalBlockQty& uq = m.qtys[reg_qty[ureg]];
            if (may_save_copy && !uq.phys_copy_sugg.TestBit(sreg)) {
              uq.phys_copy_sugg.SetBit(sreg);
              ++uq.num_copy_sugg;
            } else if (!may_save_copy && !uq.phys_sugg.TestBit(sreg)) {
              uq.phys_sugg.SetBit(sreg);
              ++uq.num_sugg;
            }
            continue;
          }
          if (reg_qty[sreg] >= -1) {
            continue;
          }
          bool has_dead = false;
          for (const RtlRegNote& n : pn.reg_notes) {
            if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                !n.datum.IsArgPointerReg() && n.datum.RegNo() == ureg) {
              has_dead = true;
              break;
            }
          }
          if (has_dead && RegClassRelatedP(pref_class[sreg], m.qtys[reg_qty[ureg]].min_class)) {
            const int sqty = reg_qty[ureg];
            LocalBlockQty& sq = m.qtys[sqty];
            reg_qty[sreg] = sqty;
            reg_offset[sreg] = reg_offset[ureg] + offset;
            sq.member_regs.push_back(sreg);
            sq.member_offsets.push_back(reg_offset[sreg]);
            if (RegClassSubsetP(pref_class[sreg], sq.min_class)) {
              sq.min_class = pref_class[sreg];
            }
            if (RegClassSubsetP(alt_class[sreg], sq.alt_class)) {
              sq.alt_class = alt_class[sreg];
            }
            if (auto cc_it = dst_out->sideband.reg_n_calls_crossed.find(sreg);
                cc_it != dst_out->sideband.reg_n_calls_crossed.end()) {
              sq.calls_crossed += cc_it->second;
            }
            sq.n_refs += dst_out->sideband.reg_n_refs.at(sreg);
            if (usize < ssize) {
              for (size_t k = 0; k < sq.member_regs.size(); ++k) {
                reg_offset[sq.member_regs[k]] -= offset;
                sq.member_offsets[k] -= offset;
              }
              sq.size = ssize;
              sq.mode = smode;
            }
            combined_regno = ureg;
            break;
          }
        }
      }
    }

    auto wipe_dead_reg = [&](const RtlExpr& reg, bool output_p) {
      if (reg.op != RtlOp::kReg) {
        return;
      }
      const int regno = reg.IsArgPointerReg() ? kArgPointerRegnum : reg.RegNo();
      if (pn.pattern.op == RtlOp::kParallel && primary_set == nullptr) {
        for (int idx = static_cast<int>(pn.pattern.children.size()) - 1; idx >= 0; --idx) {
          const RtlExpr& sub = pn.pattern.children[idx];
          if (sub.op == RtlOp::kSet && !sub.children.empty() && sub.children[0].op != RtlOp::kReg &&
              reg != sub.children[0] && RegOverlapMentionedP(reg, sub.children[0], mips_cfg)) {
            output_p = true;
          }
        }
      }
      if (regno < kFirstPseudoRegister) {
        mark_life(regno, {reg.mode, reg.raw_mode}, false);
        if (output_p) {
          post_mark_base(regno, {reg.mode, reg.raw_mode}, 2 * insn_number, 2 * insn_number + 1);
        }
      } else if (regno < max_regno && reg_qty[regno] >= 0) {
        m.qtys[reg_qty[regno]].death = 2 * insn_number + (output_p ? 1 : 0);
      }
    };

    for (const RtlRegNote& note : pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
        const int r = note.datum.IsArgPointerReg() ? kArgPointerRegnum : note.datum.RegNo();
        if (r != combined_regno) {
          wipe_dead_reg(note.datum, false);
        }
      }
    }

    std::vector<bool> scratch_store_consumed(insn_scratches.size(), false);
    NoteStores(pn.pattern, [&](const RtlExpr& dest, const RtlExpr& setter) {
      if (setter.op == RtlOp::kClobber && dest.op == RtlOp::kReg && !dest.IsArgPointerReg()) {
        for (size_t si = 0; si < insn_scratches.size(); ++si) {
          if (!scratch_store_consumed[si] && insn_scratches[si].first == dest.RegNo() &&
              insn_scratches[si].second.mode == dest.mode) {
            scratch_store_consumed[si] = true;
            return;
          }
        }
      }
      reg_is_born(dest, 2 * insn_number - (setter.op == RtlOp::kClobber ? 1 : 0), insn_number);
    });

    std::vector<bool> scratch_note_consumed(insn_scratches.size(), false);
    for (const RtlRegNote& note : pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegUnused && note.datum.op == RtlOp::kReg) {
        bool was_scratch = false;
        if (!note.datum.IsArgPointerReg()) {
          for (size_t si = 0; si < insn_scratches.size(); ++si) {
            if (!scratch_note_consumed[si] && insn_scratches[si].first == note.datum.RegNo() &&
                insn_scratches[si].second.mode == note.datum.mode) {
              scratch_note_consumed[si] = true;
              was_scratch = true;
              break;
            }
          }
        }
        if (!was_scratch) {
          wipe_dead_reg(note.datum, true);
        }
      }
    }

    for (const auto& [scr_hr, scr_mode] : insn_scratches) {
      LocalBlockQty sq;
      sq.qty_id = static_cast<int>(m.qtys.size());
      sq.first_reg = -1;
      sq.mode = scr_mode;
      sq.size = ModeByteSize(scr_mode);
      sq.birth = 2 * insn_number - 1;
      sq.death = 2 * insn_number + 1;
      sq.min_class = RegNoRegClass(scr_hr);
      sq.alt_class = MipsRegClass::kNoRegs;
      sq.n_refs = 1;
      m.qtys.push_back(std::move(sq));
    }

    m.base_regs_live_at[2 * insn_number].Ior(regs_live);
    m.base_regs_live_at[2 * insn_number + 1].Ior(regs_live);
  }

  for (const LocalBlockQty& q : m.qtys) {
    if (q.birth < 0 || q.death < q.birth) {
      return std::nullopt;
    }
  }
  return m;
}

void EnforceBlockAllocQuantityAtomicity(
    RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::flat_hash_set<int>& live_at_any_block_start,
    const absl::flat_hash_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::flat_hash_map<int, RegModeInfo>& pseudo_modes,
    const absl::flat_hash_set<int>& fp_op_pseudos,
    const absl::flat_hash_set<int>& gr_or_mem_op_pseudos, HardRegSet* erased_global_hard_regs,
    absl::flat_hash_set<int>* atomicity_erased_pseudos) {
  for (size_t b = 0; b < dst_out->rpo_blocks.size(); ++b) {
    std::optional<LocalBlockAllocModel> model = BuildSingleBlockLocalAllocModel(
        b, dst_out, liveness_cfg, mips_cfg, live_at_any_block_start, pseudo_summaries, pseudo_modes,
        fp_op_pseudos, gr_or_mem_op_pseudos, /*bump_caller_saves_refs=*/false);
    if (!model.has_value()) {
      continue;
    }
    for (const LocalBlockQty& q : model->qtys) {
      if (q.first_reg < kFirstPseudoRegister || q.member_regs.empty()) {
        continue;
      }
      int base_hr = -2;
      bool invalid_local_qty = false;
      for (size_t mi = 0; mi < q.member_regs.size(); ++mi) {
        const int p = q.member_regs[mi];
        const int off = q.member_offsets[mi];
        const int thr = model->target_hr_by_pseudo.at(p);
        const int cand_base = (thr >= 0) ? (thr - off) : -1;
        if (mi == 0) {
          base_hr = cand_base;
        } else if (cand_base != base_hr) {
          invalid_local_qty = true;
          break;
        }
      }
      if (!invalid_local_qty && base_hr >= 0) {
        if (base_hr >= kFirstPseudoRegister || !HardRegnoModeOk(base_hr, q.mode, mips_cfg)) {
          invalid_local_qty = true;
        } else {
          HardRegSet allowed_cls = RegClassContents(q.min_class);
          allowed_cls.Ior(RegClassContents(q.alt_class));
          const int nregs = HardRegnoNregs(base_hr, q.mode, mips_cfg);
          for (int k = 0; k < nregs; ++k) {
            const int r = base_hr + k;
            if (r >= kFirstPseudoRegister || !allowed_cls.TestBit(r) ||
                model->fixed_regs.TestBit(r) || r == kArgPointerRegnum ||
                r == kFramePointerRegnum ||
                (q.calls_crossed > 0 && !mips_cfg.caller_saves &&
                 model->call_used_regs.TestBit(r))) {
              invalid_local_qty = true;
              break;
            }
          }
        }
      }
      if (invalid_local_qty) {
        for (int p : q.member_regs) {
          if (auto ren_it = dst_out->sideband.reg_renumber.find(p);
              ren_it != dst_out->sideband.reg_renumber.end() && ren_it->second >= 0 &&
              ren_it->second < kFirstPseudoRegister) {
            const int hr = ren_it->second;
            const auto m_it = pseudo_modes.find(p);
            const RegModeInfo pmode =
                (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
            const int nregs = HardRegnoNregs(hr, pmode, mips_cfg);
            for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
              erased_global_hard_regs->SetBit(k);
            }
            if (atomicity_erased_pseudos != nullptr) {
              atomicity_erased_pseudos->insert(p);
            }
          }
          dst_out->sideband.reg_renumber.erase(p);
        }
      }
    }
  }
}

void ReconcileSingleBlockLocalPriorities(
    size_t b, RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::flat_hash_set<int>& live_at_any_block_start,
    const absl::flat_hash_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::flat_hash_map<int, RegModeInfo>& pseudo_modes,
    const absl::flat_hash_set<int>& fp_op_pseudos,
    const absl::flat_hash_set<int>& gr_or_mem_op_pseudos) {
  std::optional<LocalBlockAllocModel> model = BuildSingleBlockLocalAllocModel(
      b, dst_out, liveness_cfg, mips_cfg, live_at_any_block_start, pseudo_summaries, pseudo_modes,
      fp_op_pseudos, gr_or_mem_op_pseudos, /*bump_caller_saves_refs=*/true);
  if (!model.has_value() || model->eligible_pseudos.size() < 2) {
    return;
  }
  const LocalBlockAllocModel& m = *model;
  const auto& reg_offset = m.reg_offset;
  const HardRegSet& call_used = m.call_used_regs;

  const int init_mismatches = SimulateLocalBlockAlloc(m, nullptr);
  if (init_mismatches == 0 || m.qtys.size() <= 1) {
    return;
  }

  const int nq = static_cast<int>(m.qtys.size());
  auto qty_pri = [&](const LocalBlockAllocModel& mod, int q) {
    return ComputeLocalQtyPriority(mod.qtys[q].n_refs, mod.qtys[q].death - mod.qtys[q].birth,
                                   mod.qtys[q].size);
  };
  auto sort_pass2 = [&](const LocalBlockAllocModel& mod) {
    std::vector<int> ord(nq);
    for (int i = 0; i < nq; ++i) {
      ord[i] = i;
    }
    std::sort(ord.begin(), ord.end(), [&](int q1, int q2) {
      const int p1 = qty_pri(mod, q1);
      const int p2 = qty_pri(mod, q2);
      return (p1 != p2) ? (p1 > p2) : (q1 < q2);
    });
    return ord;
  };

  const std::vector<int> initial_order = sort_pass2(m);
  std::vector<int> initial_pos(nq, 0);
  for (int idx = 0; idx < nq; ++idx) {
    initial_pos[initial_order[idx]] = idx;
  }

  auto qty_target_hr = [&](int q) -> int {
    if (m.qtys[q].first_reg < kFirstPseudoRegister) {
      return -2;
    }
    const int p0 = m.qtys[q].first_reg;
    const int thr = m.target_hr_by_pseudo.at(p0);
    return (thr >= 0) ? (thr - reg_offset[p0]) : -1;
  };

  // Realize an ordering `ord` via monotone right-to-left `qty_n_refs`
  // assignment on pseudo quantities (`first_reg >= kFirstPseudoRegister`).
  auto realize_local_order =
      [&](const std::vector<int>& ord) -> std::pair<LocalBlockAllocModel, int> {
    LocalBlockAllocModel work_m = m;
    for (int k = nq - 1; k >= 0; --k) {
      const int q = ord[k];
      int min_req_pri = 0;
      for (int r = k + 1; r < nq; ++r) {
        const int tail_q = ord[r];
        min_req_pri = std::max(min_req_pri, qty_pri(work_m, tail_q) + ((q > tail_q) ? 1 : 0));
      }
      if (work_m.qtys[q].first_reg < kFirstPseudoRegister) {
        continue;
      }
      const int span = std::max(1, work_m.qtys[q].death - work_m.qtys[q].birth);
      const int min_refs = std::max(1, static_cast<int>(work_m.qtys[q].member_regs.size()));
      const int thr = qty_target_hr(q);
      int max_refs = 4096;
      if (thr == -1 && mips_cfg.caller_saves && work_m.qtys[q].calls_crossed > 0) {
        max_refs = std::max(min_refs, 4 * work_m.qtys[q].calls_crossed);
      }
      int lo_r = min_refs;
      if (thr >= 0 && mips_cfg.caller_saves && work_m.qtys[q].calls_crossed > 0 &&
          call_used.TestBit(thr)) {
        lo_r = std::max(lo_r, 4 * work_m.qtys[q].calls_crossed + 1);
      }
      const int hi_r = std::max(lo_r, max_refs);
      const int orig_r = m.qtys[q].n_refs;
      if (orig_r >= lo_r && orig_r <= hi_r &&
          ComputeLocalQtyPriority(orig_r, span, work_m.qtys[q].size) >= min_req_pri &&
          (k == 0 || ComputeLocalQtyPriority(orig_r, span, work_m.qtys[q].size) <=
                         qty_pri(m, ord[k - 1]) - ((ord[k - 1] > q) ? 1 : 0))) {
        work_m.qtys[q].n_refs = orig_r;
        continue;
      }
      int chosen_r = (thr == -1) ? lo_r : hi_r;
      if (ComputeLocalQtyPriority(hi_r, span, work_m.qtys[q].size) >= min_req_pri) {
        int l = lo_r;
        int h = hi_r;
        while (l < h) {
          const int mid = l + (h - l) / 2;
          if (ComputeLocalQtyPriority(mid, span, work_m.qtys[q].size) >= min_req_pri) {
            h = mid;
          } else {
            l = mid + 1;
          }
        }
        chosen_r = l;
      }
      work_m.qtys[q].n_refs = chosen_r;
    }
    const int realized_mis = SimulateLocalBlockAlloc(work_m, nullptr);
    return {std::move(work_m), realized_mis};
  };

  std::vector<int> target_seed = initial_order;
  std::sort(target_seed.begin(), target_seed.end(), [&](int a, int b) {
    const int ta = qty_target_hr(a);
    const int tb = qty_target_hr(b);
    const int tier_a = (ta >= 0) ? 0 : (ta == -1 ? 1 : 2);
    const int tier_b = (tb >= 0) ? 0 : (tb == -1 ? 1 : 2);
    if (tier_a != tier_b) {
      return tier_a < tier_b;
    }
    if (ta >= 0 && tb >= 0) {
      const bool cs_a = (m.qtys[a].calls_crossed > 0 && call_used.TestBit(ta));
      const bool cs_b = (m.qtys[b].calls_crossed > 0 && call_used.TestBit(tb));
      if (cs_a != cs_b) {
        return !cs_a && cs_b;
      }
      if (ta != tb) {
        return ta < tb;
      }
    }
    return initial_pos[a] < initial_pos[b];
  });

  LocalBlockAllocModel best_work_m = m;
  int best_mismatches = init_mismatches;
  {
    auto [seed_work_m, seed_mis] = realize_local_order(target_seed);
    if (seed_mis < best_mismatches) {
      best_mismatches = seed_mis;
      best_work_m = std::move(seed_work_m);
    }
  }

  auto build_conflict_qty_mask = [&](const LocalBlockAllocModel& mod) {
    if (nq <= 3) {
      return std::vector<bool>(nq, true);
    }
    absl::flat_hash_map<int, int> sim_hr;
    SimulateLocalBlockAlloc(mod, nullptr, &sim_hr);
    std::vector<bool> mismatched_q(nq, false);
    HardRegSet conflict_hrs;
    for (int q = 0; q < nq; ++q) {
      if (mod.qtys[q].first_reg < kFirstPseudoRegister) {
        continue;
      }
      for (int p : mod.qtys[q].member_regs) {
        const int got = sim_hr[p];
        const int exp = mod.target_hr_by_pseudo.at(p);
        if (got != exp) {
          mismatched_q[q] = true;
          const auto m_it = pseudo_modes.find(p);
          const RegModeInfo pm =
              (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
          if (got >= 0) {
            const int nregs = HardRegnoNregs(got, pm, mips_cfg);
            for (int r = got; r < got + nregs && r < kFirstPseudoRegister; ++r) {
              conflict_hrs.SetBit(r);
            }
          }
          if (exp >= 0) {
            const int nregs = HardRegnoNregs(exp, pm, mips_cfg);
            for (int r = exp; r < exp + nregs && r < kFirstPseudoRegister; ++r) {
              conflict_hrs.SetBit(r);
            }
          }
        }
      }
    }
    std::vector<bool> active_q = mismatched_q;
    for (int q = 0; q < nq; ++q) {
      if (active_q[q] || mod.qtys[q].first_reg < kFirstPseudoRegister) {
        continue;
      }
      for (int p : mod.qtys[q].member_regs) {
        const auto m_it = pseudo_modes.find(p);
        const RegModeInfo pm =
            (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
        for (int hr : {sim_hr[p], mod.target_hr_by_pseudo.at(p)}) {
          if (hr >= 0) {
            const int nregs = HardRegnoNregs(hr, pm, mips_cfg);
            for (int r = hr; r < hr + nregs && r < kFirstPseudoRegister; ++r) {
              if (conflict_hrs.TestBit(r)) {
                active_q[q] = true;
                break;
              }
            }
          }
        }
        if (active_q[q]) {
          break;
        }
      }
    }
    bool changed = true;
    while (changed) {
      changed = false;
      for (int q = 0; q < nq; ++q) {
        if (active_q[q] || mod.qtys[q].first_reg < kFirstPseudoRegister) {
          continue;
        }
        for (int a = 0; a < nq; ++a) {
          if (!active_q[a] || !ModesTieableForLocalAlloc(mod.qtys[q].mode, mod.qtys[a].mode)) {
            continue;
          }
          if (std::max(mod.qtys[q].birth, mod.qtys[a].birth) <
              std::min(mod.qtys[q].death, mod.qtys[a].death)) {
            active_q[q] = true;
            changed = true;
            break;
          }
        }
      }
    }
    return active_q;
  };

  auto climb_local = [&](std::vector<int> cur_order) {
    auto [cur_work_m, cur_mis] = realize_local_order(cur_order);
    int cur_ideal_mis = SimulateLocalBlockAlloc(m, &cur_order);
    for (int iter = 0; iter < 64 && cur_mis > 0; ++iter) {
      const std::vector<bool> active_q = build_conflict_qty_mask(cur_work_m);
      bool improved = false;
      std::vector<int> step_order = cur_order;
      LocalBlockAllocModel step_work_m = cur_work_m;
      int step_mis = cur_mis;
      int step_ideal_mis = cur_ideal_mis;
      for (int from = 0; from < nq; ++from) {
        const int q_from = cur_order[from];
        if (m.qtys[q_from].first_reg < kFirstPseudoRegister || !active_q[q_from]) {
          continue;
        }
        for (int to = 0; to < nq; ++to) {
          if (from == to) {
            continue;
          }
          std::vector<int> cand = cur_order;
          const int val = cand[from];
          cand.erase(cand.begin() + from);
          cand.insert(cand.begin() + to, val);
          const int ideal_mis = SimulateLocalBlockAlloc(m, &cand);
          if (ideal_mis > step_mis && ideal_mis >= step_ideal_mis) {
            continue;
          }
          auto [cand_work_m, real_mis] = realize_local_order(cand);
          if (real_mis < step_mis || (real_mis == step_mis && ideal_mis < step_ideal_mis)) {
            step_mis = real_mis;
            step_ideal_mis = ideal_mis;
            step_work_m = std::move(cand_work_m);
            step_order = std::move(cand);
            improved = true;
          }
        }
      }
      if (!improved) {
        break;
      }
      cur_order = std::move(step_order);
      cur_work_m = std::move(step_work_m);
      cur_mis = step_mis;
      cur_ideal_mis = step_ideal_mis;
    }
    if (cur_mis < best_mismatches) {
      best_mismatches = cur_mis;
      best_work_m = std::move(cur_work_m);
    }
  };

  if (best_mismatches > 0) {
    climb_local(initial_order);
  }
  if (best_mismatches > 0) {
    climb_local(target_seed);
  }
  if (best_mismatches < init_mismatches) {
    for (int q = 0; q < nq; ++q) {
      const int p0 = best_work_m.qtys[q].first_reg;
      if (p0 >= kFirstPseudoRegister && best_work_m.qtys[q].n_refs != m.qtys[q].n_refs &&
          !best_work_m.qtys[q].member_regs.empty()) {
        const auto& members = best_work_m.qtys[q].member_regs;
        if (members.size() == 1) {
          dst_out->sideband.reg_n_refs[members[0]] = std::max(1, best_work_m.qtys[q].n_refs);
        } else {
          int tail_sum = 0;
          for (size_t mi = 1; mi < members.size(); ++mi) {
            dst_out->sideband.reg_n_refs[members[mi]] = 1;
            ++tail_sum;
          }
          dst_out->sideband.reg_n_refs[members[0]] =
              std::max(1, best_work_m.qtys[q].n_refs - tail_sum);
        }
      }
    }
  }
}

void ReconcileLocalBlockAllocPriorities(
    RtlControlFlowGraph* dst_out, const RtlControlFlowGraph& liveness_cfg,
    const MipsTargetConfig& mips_cfg, const absl::flat_hash_set<int>& live_at_any_block_start,
    const absl::flat_hash_map<int, PseudoLivenessSummary>& pseudo_summaries,
    const absl::flat_hash_map<int, RegModeInfo>& pseudo_modes,
    const absl::flat_hash_set<int>& fp_op_pseudos,
    const absl::flat_hash_set<int>& gr_or_mem_op_pseudos) {
  for (size_t b = 0; b < dst_out->rpo_blocks.size(); ++b) {
    ReconcileSingleBlockLocalPriorities(b, dst_out, liveness_cfg, mips_cfg, live_at_any_block_start,
                                        pseudo_summaries, pseudo_modes, fp_op_pseudos,
                                        gr_or_mem_op_pseudos);
  }
}

void RunOptimizingGlobalAllocInverseOnce(const RtlControlFlowGraph& base_greg_cfg,
                                         const CompilationProfile& profile,
                                         const MipsTargetConfig& mips_cfg, size_t num_blocks,
                                         int base_max_regno, RtlControlFlowGraph* dst_out) {
  *dst_out = base_greg_cfg;
  dst_out->pass = RtlPassKind::kLreg;
  dst_out->sideband.max_regno = base_max_regno;

  // Deterministically recompute `flow.c` liveness sidebands and death notes on
  // a temporary copy (preserving `dst_out->rpo_blocks` verbatim and preserving
  // zero-occurrence phantom pseudos in `sideband`).
  RtlControlFlowGraph liveness_cfg = *dst_out;
  ReconstructRtlLiveness(&liveness_cfg, profile,
                         {.reload_completed = false,
                          .update_reg_notes = true,
                          .preserve_phantom_pseudos = true,
                          .apply_post_sched_and_equiv_live_length = true,
                          .include_call_reg_notes = true});
  dst_out->sideband.basic_block_live_at_start =
      std::move(liveness_cfg.sideband.basic_block_live_at_start);
  dst_out->sideband.reg_n_refs = std::move(liveness_cfg.sideband.reg_n_refs);
  dst_out->sideband.reg_n_sets = std::move(liveness_cfg.sideband.reg_n_sets);
  dst_out->sideband.reg_n_deaths = std::move(liveness_cfg.sideband.reg_n_deaths);
  dst_out->sideband.reg_live_length = std::move(liveness_cfg.sideband.reg_live_length);
  dst_out->sideband.reg_n_calls_crossed = std::move(liveness_cfg.sideband.reg_n_calls_crossed);
  dst_out->sideband.max_regno =
      std::max(dst_out->sideband.max_regno, liveness_cfg.sideband.max_regno);
  {
    bool any_pseudo_live_len = false;
    for (const auto& [r, len] : dst_out->sideband.reg_live_length) {
      if (r >= kFirstPseudoRegister && len != 0) {
        any_pseudo_live_len = true;
        break;
      }
    }
    if (!any_pseudo_live_len) {
      dst_out->sideband.reg_live_length.try_emplace(kFirstPseudoRegister, -1);
      dst_out->sideband.basic_block_live_at_start.try_emplace(0);
    }
  }

  // `combine.c:10839` (`move_deaths`) can emit a standalone `(use (reg:M p))`
  // anchor before `combine.c:2336` eliminates the last setter of `p` and
  // clears `reg_n_refs[p] = 0` (`reg_n_sets[p] == 0`, `p` not live at entry).
  // Clear `reg_n_refs` and block-start liveness for such orphaned `USE`-only
  // pseudos so neither `GlobalAllocModel` nor `global_alloc` allocates them.
  {
    absl::flat_hash_set<int> use_insn_pseudos;
    absl::flat_hash_set<int> non_use_insn_pseudos;
    for (const RtlBasicBlock& blk : dst_out->rpo_blocks) {
      for (const std::string& insn_str : blk.instructions) {
        auto pn = ParsePatternAndNotes(insn_str);
        if (!pn.ok()) {
          continue;
        }
        absl::flat_hash_set<int>* bucket =
            (pn->pattern.op == RtlOp::kUse) ? &use_insn_pseudos : &non_use_insn_pseudos;
        pn->pattern.Visit([&](const RtlExpr& expr) {
          if (expr.IsPseudoReg()) {
            bucket->insert(expr.RegNo());
          }
        });
        for (const RtlExpr& fu : pn->call_fusage) {
          fu.Visit([&](const RtlExpr& expr) {
            if (expr.IsPseudoReg()) {
              non_use_insn_pseudos.insert(expr.RegNo());
            }
          });
        }
      }
    }
    for (int p : use_insn_pseudos) {
      if (non_use_insn_pseudos.contains(p) || base_greg_cfg.sideband.reg_renumber.contains(p)) {
        continue;
      }
      if (auto s_it = dst_out->sideband.reg_n_sets.find(p);
          s_it != dst_out->sideband.reg_n_sets.end() && s_it->second != 0) {
        continue;
      }
      dst_out->sideband.reg_n_refs.erase(p);
      dst_out->sideband.reg_live_length.erase(p);
      dst_out->sideband.reg_n_calls_crossed.erase(p);
      std::vector<int> empty_bbs;
      for (auto& [bb_id, live_set] : dst_out->sideband.basic_block_live_at_start) {
        live_set.erase(p);
        if (live_set.empty()) {
          empty_bbs.push_back(bb_id);
        }
      }
      for (int bb_id : empty_bbs) {
        dst_out->sideband.basic_block_live_at_start.erase(bb_id);
      }
    }
  }

  absl::flat_hash_set<int> live_at_any_block_start;
  for (const auto& [bb_idx, live_set] : dst_out->sideband.basic_block_live_at_start) {
    if (bb_idx >= 0 && static_cast<size_t>(bb_idx) < num_blocks) {
      for (int regno : live_set) {
        if (regno >= kFirstPseudoRegister) {
          live_at_any_block_start.insert(regno);
        }
      }
    }
  }

  absl::flat_hash_map<int, PseudoLivenessSummary> pseudo_summaries;
  absl::flat_hash_map<int, RegModeInfo> pseudo_modes;
  absl::flat_hash_set<int> fp_op_pseudos;
  absl::flat_hash_set<int> gr_or_mem_op_pseudos;
  HardRegSet scratch_hard_regs;
  absl::flat_hash_set<int> all_di_mult_pseudos;
  int num_hard_non_hi_di_mults = 0;
  std::vector<std::vector<bool>> block_insn_clobbers_md(liveness_cfg.rpo_blocks.size());

  for (size_t b = 0; b < liveness_cfg.rpo_blocks.size(); ++b) {
    const RtlBasicBlock& blk = liveness_cfg.rpo_blocks[b];
    absl::flat_hash_set<int> defined_so_far_in_block;
    absl::flat_hash_set<int> died_in_block;
    block_insn_clobbers_md[b].resize(blk.instructions.size(), false);

    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }

      parsed->pattern.Visit([&](const RtlExpr& expr) {
        if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg() &&
            expr.RegNo() >= kFirstPseudoRegister) {
          pseudo_modes.try_emplace(expr.RegNo(), RegModeInfo{expr.mode, expr.raw_mode});
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
              fp_op_pseudos.insert(sub.RegNo());
            }
          });
        }
        if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
          const RtlExpr& d = expr.children[0];
          const RtlExpr& s = expr.children[1];
          if (d.op == RtlOp::kReg && !d.IsArgPointerReg() && d.RegNo() >= kFirstPseudoRegister &&
              ((s.op == RtlOp::kReg && !s.IsArgPointerReg() && s.RegNo() < kFirstFprRegnum) ||
               s.op == RtlOp::kMem)) {
            gr_or_mem_op_pseudos.insert(d.RegNo());
          }
          if (s.op == RtlOp::kReg && !s.IsArgPointerReg() && s.RegNo() >= kFirstPseudoRegister &&
              ((d.op == RtlOp::kReg && !d.IsArgPointerReg() && d.RegNo() < kFirstFprRegnum) ||
               d.op == RtlOp::kMem)) {
            gr_or_mem_op_pseudos.insert(s.RegNo());
          }
        }
        if (expr.op == RtlOp::kClobber && !expr.children.empty() &&
            expr.children[0].op == RtlOp::kReg && !expr.children[0].IsArgPointerReg() &&
            expr.children[0].RegNo() < kFirstPseudoRegister) {
          const int hr = expr.children[0].RegNo();
          const int nregs =
              HardRegnoNregs(hr, {expr.children[0].mode, expr.children[0].raw_mode}, mips_cfg);
          for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
            scratch_hard_regs.SetBit(k);
          }
        }
      });

      block_insn_clobbers_md[b][i] = InsnTouchesMdRegsOrCall(parsed->pattern);
      absl::flat_hash_set<int> insn_di_mult_dests;
      bool insn_has_hard_non_hi_di_mult = false;
      CollectDiMultDestRegs(parsed->pattern, &insn_di_mult_dests, &insn_has_hard_non_hi_di_mult);
      if (insn_has_hard_non_hi_di_mult) {
        ++num_hard_non_hi_di_mults;
      }
      for (int p : insn_di_mult_dests) {
        all_di_mult_pseudos.insert(p);
      }

      absl::flat_hash_map<int, MipsRegClass> insn_single_md_dests;
      CollectSingleRegMdOutputClasses(parsed->pattern, mips_cfg, &insn_single_md_dests);

      absl::flat_hash_set<int> insn_used_pseudos;
      absl::flat_hash_set<int> insn_defined_pseudos;
      absl::flat_hash_map<int, int> insn_read_counts;
      absl::flat_hash_set<int> insn_mem_addr_pseudos;
      CollectInsnElementUsesAndDefs(parsed->pattern, &insn_used_pseudos, &insn_defined_pseudos,
                                    &insn_read_counts, &insn_mem_addr_pseudos);

      for (int regno : insn_used_pseudos) {
        PseudoLivenessSummary& summary = pseudo_summaries[regno];
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
        PseudoLivenessSummary& summary = pseudo_summaries[regno];
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

      absl::flat_hash_set<int> insn_note_dead_pseudos;
      for (const RtlRegNote& note : parsed->reg_notes) {
        note.datum.Visit([&](const RtlExpr& expr) {
          if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg() &&
              expr.RegNo() >= kFirstPseudoRegister) {
            pseudo_summaries[expr.RegNo()].blocks_referenced.insert(b);
            pseudo_modes.try_emplace(expr.RegNo(), RegModeInfo{expr.mode, expr.raw_mode});
          }
        });
        if ((note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) &&
            note.datum.op == RtlOp::kReg && !note.datum.IsArgPointerReg() &&
            note.datum.RegNo() >= kFirstPseudoRegister) {
          const int dead_regno = note.datum.RegNo();
          PseudoLivenessSummary& summary = pseudo_summaries[dead_regno];
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

  // Determine which pseudos in `sideband.reg_renumber` were allocated by
  // `global.c:global_alloc` rather than `local-alloc.c:block_alloc`.
  std::vector<int> global_pseudos_to_erase;
  for (const auto& [regno, hard_reg] : dst_out->sideband.reg_renumber) {
    bool is_global_alloc_pseudo = false;
    if (regno < kFirstPseudoRegister || hard_reg < 0 || hard_reg == kFramePointerRegnum ||
        hard_reg >= kHiRegnum) {
      is_global_alloc_pseudo = true;
    } else if (live_at_any_block_start.contains(regno)) {
      is_global_alloc_pseudo = true;
    } else if (auto death_it = dst_out->sideband.reg_n_deaths.find(regno);
               death_it == dst_out->sideband.reg_n_deaths.end() || death_it->second != 1) {
      is_global_alloc_pseudo = true;
    } else {
      auto sum_it = pseudo_summaries.find(regno);
      if (sum_it == pseudo_summaries.end() || sum_it->second.blocks_referenced.size() != 1 ||
          !sum_it->second.has_set_in_block || sum_it->second.used_before_set ||
          sum_it->second.alive_after_redef || sum_it->second.note_deaths >= 2) {
        is_global_alloc_pseudo = true;
      } else if (sum_it->second.def_count_in_block == 1 &&
                 sum_it->second.single_reg_md_class != MipsRegClass::kNoRegs &&
                 !sum_it->second.used_in_mem_address && sum_it->second.total_read_count <= 1) {
        is_global_alloc_pseudo = true;
      }
    }

    if (is_global_alloc_pseudo) {
      global_pseudos_to_erase.push_back(regno);
    }
  }

  HardRegSet erased_global_hard_regs;
  for (int regno : global_pseudos_to_erase) {
    if (auto ren_it = dst_out->sideband.reg_renumber.find(regno);
        ren_it != dst_out->sideband.reg_renumber.end() && ren_it->second >= 0 &&
        ren_it->second < kFirstPseudoRegister) {
      const int hr = ren_it->second;
      const auto m_it = pseudo_modes.find(regno);
      const RegModeInfo pmode =
          (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
      const int nregs = HardRegnoNregs(hr, pmode, mips_cfg);
      for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
        erased_global_hard_regs.SetBit(k);
      }
    }
    dst_out->sideband.reg_renumber.erase(regno);
  }

  EnforceBlockAllocQuantityAtomicity(dst_out, liveness_cfg, mips_cfg, live_at_any_block_start,
                                     pseudo_summaries, pseudo_modes, fp_op_pseudos,
                                     gr_or_mem_op_pseudos, &erased_global_hard_regs,
                                     /*atomicity_erased_pseudos=*/nullptr);

  // Prune post-reload `mark_home_live` callee-saved registers (`16..23, 30`
  // and `52..63`) that belonged solely to lifted global-alloc pseudos so
  // pre-`global_alloc` `regs_ever_live` matches `flow` (`saved_gpr_regnos` /
  // `saved_fpr_regnos`).
  if (!erased_global_hard_regs.IsEmpty()) {
    HardRegSet explicit_or_local_hard_regs;
    if (dst_out->sideband.frame_pointer_needed) {
      explicit_or_local_hard_regs.SetBit(kFramePointerRegnum);
    }
    for (const auto& [regno, hr] : dst_out->sideband.reg_renumber) {
      if (regno >= kFirstPseudoRegister && hr >= 0 && hr < kFirstPseudoRegister) {
        const auto m_it = pseudo_modes.find(regno);
        const RegModeInfo pmode =
            (m_it != pseudo_modes.end()) ? m_it->second : RegModeInfo{RtlMachineMode::kSi, ""};
        const int nregs = HardRegnoNregs(hr, pmode, mips_cfg);
        for (int k = hr; k < hr + nregs && k < kFirstPseudoRegister; ++k) {
          explicit_or_local_hard_regs.SetBit(k);
        }
      }
    }
    auto scan_explicit_expr = [&](const RtlExpr& root, bool is_store_dst) {
      root.Visit([&](const RtlExpr& e) {
        if (e.op != RtlOp::kReg || e.IsArgPointerReg() || e.RegNo() >= kFirstPseudoRegister) {
          return;
        }
        const int r0 = e.RegNo();
        if (r0 == kArgPointerRegnum || (r0 == kFramePointerRegnum && !is_store_dst)) {
          return;
        }
        if (scratch_hard_regs.TestBit(r0) && !is_store_dst) {
          return;
        }
        const int nregs = HardRegnoNregs(r0, {e.mode, e.raw_mode}, mips_cfg);
        for (int k = r0; k < r0 + nregs && k < kFirstPseudoRegister; ++k) {
          if (!scratch_hard_regs.TestBit(k)) {
            explicit_or_local_hard_regs.SetBit(k);
          }
        }
      });
    };
    for (const RtlBasicBlock& blk : dst_out->rpo_blocks) {
      for (const std::string& insn_s : blk.instructions) {
        auto pn = ParsePatternAndNotes(insn_s);
        if (!pn.ok() || pn->pattern.op == RtlOp::kNote || pn->pattern.op == RtlOp::kCodeLabel ||
            pn->pattern.op == RtlOp::kBarrier) {
          continue;
        }
        NoteStores(pn->pattern, [&](const RtlExpr& dst, const RtlExpr&) {
          const RtlExpr* base_dst = &dst;
          while (base_dst->op == RtlOp::kSubreg || base_dst->op == RtlOp::kStrictLowPart ||
                 base_dst->op == RtlOp::kZeroExtract || base_dst->op == RtlOp::kSignExtract) {
            if (base_dst->children.empty()) {
              break;
            }
            base_dst = &base_dst->children[0];
          }
          if (base_dst->op == RtlOp::kReg) {
            scan_explicit_expr(*base_dst, /*is_store_dst=*/true);
          }
        });
        scan_explicit_expr(pn->pattern, /*is_store_dst=*/false);
        for (const RtlExpr& fu : pn->call_fusage) {
          scan_explicit_expr(fu, /*is_store_dst=*/false);
        }
      }
    }
    if (dst_out->sideband.epilogue_delay_pattern.has_value() &&
        !dst_out->sideband.epilogue_delay_pattern->empty()) {
      if (auto ep = ParsePatternAndNotes(*dst_out->sideband.epilogue_delay_pattern); ep.ok()) {
        NoteStores(ep->pattern, [&](const RtlExpr& dst, const RtlExpr&) {
          if (dst.op == RtlOp::kReg) {
            scan_explicit_expr(dst, /*is_store_dst=*/true);
          }
        });
        scan_explicit_expr(ep->pattern, /*is_store_dst=*/false);
      }
    }

    auto& saved_gprs = dst_out->sideband.saved_gpr_regnos;
    saved_gprs.erase(std::remove_if(saved_gprs.begin(), saved_gprs.end(),
                                    [&](int r) {
                                      const bool callee_saved =
                                          (r >= 16 && r <= 23) || r == kFramePointerRegnum;
                                      return callee_saved && erased_global_hard_regs.TestBit(r) &&
                                             !explicit_or_local_hard_regs.TestBit(r);
                                    }),
                     saved_gprs.end());
    auto& saved_fprs = dst_out->sideband.saved_fpr_regnos;
    saved_fprs.erase(std::remove_if(saved_fprs.begin(), saved_fprs.end(),
                                    [&](int r) {
                                      return r >= 52 && r <= 63 &&
                                             erased_global_hard_regs.TestBit(r) &&
                                             !explicit_or_local_hard_regs.TestBit(r);
                                    }),
                     saved_fprs.end());
  }

  if (!mips_cfg.gp_64bit &&
      static_cast<int>(all_di_mult_pseudos.size()) + num_hard_non_hi_di_mults >= 2) {
    for (int p : all_di_mult_pseudos) {
      if (base_greg_cfg.sideband.reg_renumber.contains(p) || live_at_any_block_start.contains(p)) {
        continue;
      }
      if (auto d_it = dst_out->sideband.reg_n_deaths.find(p);
          d_it == dst_out->sideband.reg_n_deaths.end() || d_it->second != 1) {
        continue;
      }
      if (auto s_it = dst_out->sideband.reg_n_sets.find(p);
          s_it == dst_out->sideband.reg_n_sets.end() || s_it->second != 1) {
        continue;
      }
      if (auto c_it = dst_out->sideband.reg_n_calls_crossed.find(p);
          c_it != dst_out->sideband.reg_n_calls_crossed.end() && c_it->second != 0) {
        continue;
      }
      auto sum_it = pseudo_summaries.find(p);
      if (sum_it == pseudo_summaries.end()) {
        continue;
      }
      const PseudoLivenessSummary& sum = sum_it->second;
      if (sum.blocks_referenced.size() != 1 || !sum.has_set_in_block ||
          sum.def_count_in_block != 1 || !sum.defined_by_di_mult || sum.used_before_set ||
          sum.alive_after_redef || sum.note_deaths != 1 || sum.death_insn_idx <= sum.def_insn_idx) {
        continue;
      }
      const size_t bb_idx = *sum.blocks_referenced.begin();
      bool md_conflict_in_range = false;
      for (size_t k = sum.def_insn_idx + 1; k < sum.death_insn_idx; ++k) {
        if (block_insn_clobbers_md[bb_idx][k]) {
          md_conflict_in_range = true;
          break;
        }
      }
      if (md_conflict_in_range) {
        continue;
      }
      bool other_mult_needs_md_reload = (num_hard_non_hi_di_mults > 0);
      for (int q : all_di_mult_pseudos) {
        if (q == p) {
          continue;
        }
        auto q_it = base_greg_cfg.sideband.reg_renumber.find(q);
        if (q_it == base_greg_cfg.sideband.reg_renumber.end() || q_it->second != kHiRegnum) {
          other_mult_needs_md_reload = true;
          break;
        }
      }
      if (other_mult_needs_md_reload) {
        dst_out->sideband.reg_renumber[p] = kHiRegnum;
      }
    }
  }

  ReconcileLocalBlockAllocPriorities(dst_out, liveness_cfg, mips_cfg, live_at_any_block_start,
                                     pseudo_summaries, pseudo_modes, fp_op_pseudos,
                                     gr_or_mem_op_pseudos);
  ReconcileGlobalAllocPriorities(dst_out, liveness_cfg, base_greg_cfg, mips_cfg,
                                 live_at_any_block_start, pseudo_summaries, pseudo_modes,
                                 fp_op_pseudos, gr_or_mem_op_pseudos, scratch_hard_regs);
}

}  // namespace

absl::StatusOr<RtlControlFlowGraph> InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg,
                                                      const CompilationProfile& profile) {
  int max_seen_regno = kFirstPseudoRegister;
  if (absl::Status st = ValidateInvertGlobalAllocInput(greg_cfg, &max_seen_regno); !st.ok()) {
    return st;
  }

  const size_t num_blocks = greg_cfg.rpo_blocks.size();
  const ParsedCompilerFlags flags = ParseProfileFlags(profile);
  const MipsTargetConfig mips_cfg = ExtractMipsTargetConfig(flags);

  if (flags.opt_level == 0) {
    return InvertStupidGlobalAlloc(greg_cfg, profile, max_seen_regno);
  }

  RtlControlFlowGraph out;
  RunOptimizingGlobalAllocInverseOnce(greg_cfg, profile, mips_cfg, num_blocks, max_seen_regno,
                                      &out);
  return out;
}

absl::StatusOr<RtlControlFlowGraph> InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg) {
  return InvertGlobalAlloc(greg_cfg, {.optimize_level = 2, .c_flags = {"-O2"}});
}

}  // namespace rom_nom_nom::lifter
