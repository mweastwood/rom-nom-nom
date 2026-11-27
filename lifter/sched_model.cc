#include "lifter/sched_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter::sched_model {
using detail::AnalyzeWithoutPendingListFlushCap;
using detail::ClassifyInsnKind;
using detail::ForcedReplayResult;
using detail::IsBlock0PinnedParamCopy;
using detail::IsPureSched1704CallArtifactEdge;
using detail::kMipsArgPointerRegno;
using detail::kMipsArgRegFirst;
using detail::kMipsArgRegLast;
using detail::kMipsAtRegno;
using detail::kMipsCalleeSavedFprFirst;
using detail::kMipsCalleeSavedGprFirst;
using detail::kMipsCalleeSavedGprLast;
using detail::kMipsFpArgRegFirst;
using detail::kMipsFpArgRegLast;
using detail::kMipsFpRegFirst;
using detail::kMipsFpRegLast;
using detail::kMipsFpRetRegLast;
using detail::kMipsFramePointerRegno;
using detail::kMipsKernelRegFirst;
using detail::kMipsMdRegFirst;
using detail::kMipsMdRegLast;
using detail::kMipsRetRegFirst;
using detail::kMipsRetRegLast;
using detail::kMipsReturnAddrRegno;
using detail::kMipsStackPointerRegno;
using detail::kMipsTemp1Regno;
using detail::ModeByteSize;
using detail::RunForcedReplay;
using detail::SingleSetOf;
using detail::SolveSupernodeKahn;
using detail::TryExpandUnpairedNoConflictRetvalForSched1;

namespace {

constexpr int kUnitNone = -1;
constexpr int kUnitMemory = 0;
constexpr int kUnitImuldiv = 1;
constexpr int kUnitAdder = 2;
constexpr int kUnitMult = 3;
constexpr int kUnitDivide = 4;
constexpr int kNumFunctionUnits = 5;

constexpr int kInsnQueueSize = 128;  // `insn-attr.h:88`
constexpr int kTailPriority = 0x7ffffffe;
constexpr int kLaunchPriority = 0x7f000001;
constexpr int kDonePriority = -1;

}  // namespace

namespace detail {

// Mirrors `GET_MODE_SIZE` (`machmode.def`) for MIPS scheduling modes.
int ModeByteSize(RtlMachineMode mode) {
  switch (mode) {
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
    case RtlMachineMode::kTi:
    case RtlMachineMode::kTf:
      return 16;
    case RtlMachineMode::kBlk:
    default:
      return 0;
  }
}

}  // namespace detail

namespace {

bool IsVectorFormattedOp(RtlOp op) {
  return op == RtlOp::kParallel || op == RtlOp::kSequence || op == RtlOp::kAddrVec ||
         op == RtlOp::kAddrDiffVec || op == RtlOp::kAsmOperands || op == RtlOp::kUnspec ||
         op == RtlOp::kUnspecVolatile;
}

bool IsConstantP(const RtlExpr& x) {
  return x.op == RtlOp::kConstInt || x.op == RtlOp::kConstDouble || x.op == RtlOp::kSymbolRef ||
         x.op == RtlOp::kLabelRef || x.op == RtlOp::kConst;
}

bool IsConstDoubleZero(const RtlExpr& x) {
  if (x.op != RtlOp::kConstDouble) {
    return false;
  }
  if (x.extra_ints.empty()) {
    return x.int_val == 0 && x.int_val_high == 0;
  }
  for (int64_t w : x.extra_ints) {
    if (w != 0) {
      return false;
    }
  }
  return true;
}

// Mirrors `config/mips/mips.c:416` (`large_int`).
bool IsMipsLargeInt(int64_t val) {
  const uint32_t u32 = static_cast<uint32_t>(val);
  const bool fits_signed16 = (val >= -32768 && val <= 32767);
  const bool fits_unsigned16 = (val >= 0 && val <= 65535);
  const bool fits_lui = ((u32 & 0x0000ffffu) == 0u);
  return !fits_signed16 && !fits_unsigned16 && !fits_lui;
}

// Pointer-identity checks mirroring `emit-rtl.c:335-350` and `gcc_context.cc:1915-1936`.
bool IsStackPointerRtx(const RtlExpr& x) {
  return x.op == RtlOp::kReg && x.RegNo() == kMipsStackPointerRegno &&
         x.mode == RtlMachineMode::kSi && !x.flags.volatil && !x.flags.integrated &&
         !x.flags.unchanging && !x.flags.in_struct;
}

bool IsFramePointerRtx(const RtlExpr& x) {
  return x.op == RtlOp::kReg && x.RegNo() == kMipsFramePointerRegno &&
         x.mode == RtlMachineMode::kSi && !x.flags.volatil && !x.flags.integrated &&
         !x.flags.unchanging && !x.flags.in_struct;
}

bool IsArgPointerRtx(const RtlExpr& x) {
  return x.IsArgPointerReg() && x.mode == RtlMachineMode::kSi && !x.flags.volatil &&
         !x.flags.integrated && !x.flags.unchanging && !x.flags.in_struct;
}

bool IsStackOrFramePointerRtx(const RtlExpr& x) {
  return IsFramePointerRtx(x) || IsStackPointerRtx(x);
}

bool IsCommutativeOp(RtlOp op) {
  // Mirrors `GET_RTX_CLASS(code) == 'c'` plus `EQ`/`NE` (`sched.c:487`).
  switch (op) {
    case RtlOp::kEq:
    case RtlOp::kNe:
    case RtlOp::kPlus:
    case RtlOp::kMult:
    case RtlOp::kAnd:
    case RtlOp::kIor:
    case RtlOp::kXor:
    case RtlOp::kSmin:
    case RtlOp::kSmax:
    case RtlOp::kUmin:
    case RtlOp::kUmax:
      return true;
    default:
      return false;
  }
}

bool IsComparisonOp(RtlOp op) {
  switch (op) {
    case RtlOp::kEq:
    case RtlOp::kNe:
    case RtlOp::kLt:
    case RtlOp::kLe:
    case RtlOp::kGt:
    case RtlOp::kGe:
    case RtlOp::kLtu:
    case RtlOp::kLeu:
    case RtlOp::kGtu:
    case RtlOp::kGeu:
      return true;
    default:
      return false;
  }
}

}  // namespace

namespace detail {

// Mirrors `rtlanal.c:single_set` on an instruction pattern.
const RtlExpr* SingleSetOf(const RtlExpr& pat) {
  if (pat.op == RtlOp::kSet && pat.children.size() >= 2) {
    return &pat;
  }
  if (pat.op == RtlOp::kParallel) {
    const RtlExpr* found = nullptr;
    for (const RtlExpr& ch : pat.children) {
      if (ch.op == RtlOp::kSet && ch.children.size() >= 2) {
        if (found != nullptr) {
          return nullptr;
        }
        found = &ch;
      }
    }
    return found;
  }
  return nullptr;
}

}  // namespace detail

namespace {

bool IsJumpPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      if (IsJumpPattern(ch)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

namespace detail {

Insn::Kind ClassifyInsnKind(const RtlExpr& pat) {
  if (IsCallPattern(pat)) {
    return Insn::Kind::kCall;
  }
  if (IsJumpPattern(pat)) {
    return Insn::Kind::kJump;
  }
  return Insn::Kind::kInsn;
}

}  // namespace detail

namespace {

bool IsTrapIfExpr(const RtlExpr& x) {
  return x.op == RtlOp::kUnknown && x.raw_op == "trap_if";
}

// Mirrors `explow.c:plus_constant_for_output` for `canon_rtx` (`sched.c:387, 389`).
RtlExpr PlusConstantForOutput(const RtlExpr& x, int64_t c) {
  if (c == 0) {
    return x;
  }
  if (x.op == RtlOp::kConstInt) {
    return RtlExpr::MakeConstInt(x.int_val + c);
  }
  if (x.op == RtlOp::kConst && !x.children.empty()) {
    RtlExpr inner = PlusConstantForOutput(x.children[0], c);
    RtlExpr res = x;
    res.children[0] = std::move(inner);
    return res;
  }
  if (x.op == RtlOp::kPlus && x.children.size() == 2 && x.children[1].op == RtlOp::kConstInt) {
    const int64_t sum = x.children[1].int_val + c;
    if (sum == 0) {
      return x.children[0];
    }
    RtlExpr res = x;
    res.children[1] = RtlExpr::MakeConstInt(sum);
    return res;
  }
  if (x.op == RtlOp::kLoSum && x.children.size() == 2) {
    RtlExpr res = x;
    res.children[1] = PlusConstantForOutput(x.children[1], c);
    return res;
  }
  if (x.op == RtlOp::kSymbolRef || x.op == RtlOp::kLabelRef) {
    RtlExpr plus;
    plus.op = RtlOp::kPlus;
    plus.mode = x.mode != RtlMachineMode::kNone ? x.mode : RtlMachineMode::kSi;
    plus.children = {x, RtlExpr::MakeConstInt(c)};
    RtlExpr wrapped;
    wrapped.op = RtlOp::kConst;
    wrapped.mode = plus.mode;
    wrapped.children = {std::move(plus)};
    return wrapped;
  }
  RtlExpr plus;
  plus.op = RtlOp::kPlus;
  plus.mode = x.mode != RtlMachineMode::kNone ? x.mode : RtlMachineMode::kSi;
  plus.children = {x, RtlExpr::MakeConstInt(c)};
  return plus;
}

// Mirrors `sched.c:560-589` (`find_symbolic_term`).
std::optional<RtlExpr> FindSymbolicTerm(const RtlExpr& x) {
  if (x.op == RtlOp::kSymbolRef || x.op == RtlOp::kLabelRef) {
    return x;
  }
  if (x.op == RtlOp::kReg || x.op == RtlOp::kSubreg || x.op == RtlOp::kMem ||
      x.op == RtlOp::kScratch || x.op == RtlOp::kConstInt || x.op == RtlOp::kConstDouble ||
      x.op == RtlOp::kPc || x.op == RtlOp::kCc0) {
    return std::nullopt;
  }
  if (IsVectorFormattedOp(x.op)) {
    return std::nullopt;
  }
  for (size_t i = x.children.size(); i > 0; --i) {
    if (auto found = FindSymbolicTerm(x.children[i - 1]); found.has_value()) {
      return found;
    }
  }
  return std::nullopt;
}

// Helper for `MachineModel::TypeOf`: unwraps `(subreg (reg ...))` to inspect hard-reg class.
enum class RegClassKind { kNonReg, kGpr, kFpr, kMd, kSt, kPseudo };

RegClassKind ClassifyRegOperand(const RtlExpr& x) {
  const RtlExpr* cur = &x;
  if (cur->op == RtlOp::kSubreg && !cur->children.empty()) {
    cur = &cur->children[0];
  }
  if (cur->op != RtlOp::kReg) {
    return RegClassKind::kNonReg;
  }
  const int r = cur->RegNo();
  if (r >= kMipsFirstPseudoRegister) {
    return RegClassKind::kPseudo;
  }
  if (r >= 0 && r <= 31) {
    return RegClassKind::kGpr;
  }
  if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
    return RegClassKind::kFpr;
  }
  if (r >= kMipsMdRegFirst && r <= kMipsMdRegLast) {
    return RegClassKind::kMd;
  }
  if (r == kMipsStatusRegnum) {
    return RegClassKind::kSt;
  }
  return RegClassKind::kNonReg;
}

bool ContainsAsmExpr(const RtlExpr& x) {
  bool found = false;
  x.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kAsmInput || n.op == RtlOp::kAsmOperands) {
      found = true;
    }
  });
  return found;
}

// Classifies a single `(set dst src)` pattern into `mips.md` `"type"`.
InsnType ClassifySetPattern(const RtlExpr& set_expr, Phase /*phase*/) {
  const RtlExpr& dst = set_expr.SetDest();
  const RtlExpr& src = set_expr.SetSrc();

  if (dst.op == RtlOp::kPc) {
    return src.op == RtlOp::kIfThenElse ? InsnType::kBranch : InsnType::kJump;
  }
  if (src.op == RtlOp::kCall) {
    return InsnType::kCall;
  }

  // Floating-point comparisons writing `$fcr31` (`mips.md:5588-5786`).
  if (dst.op == RtlOp::kReg &&
      (dst.RegNo() == kMipsStatusRegnum || dst.mode == RtlMachineMode::kCc)) {
    return InsnType::kFcmp;
  }

  const bool is_fp_mode = (dst.mode == RtlMachineMode::kSf || dst.mode == RtlMachineMode::kDf ||
                           src.mode == RtlMachineMode::kSf || src.mode == RtlMachineMode::kDf);

  switch (src.op) {
    case RtlOp::kPlus:
    case RtlOp::kMinus:
      if (is_fp_mode) {
        if ((!src.children.empty() && src.children[0].op == RtlOp::kMult) ||
            (src.children.size() >= 2 && src.children[1].op == RtlOp::kMult)) {
          return InsnType::kFmadd;
        }
        return InsnType::kFadd;
      }
      if ((!src.children.empty() && src.children[0].op == RtlOp::kMult) ||
          (src.children.size() >= 2 && src.children[1].op == RtlOp::kMult)) {
        return InsnType::kImul;  // `madsi`/`maddi`/`umaddi` (`mips.md:1071-1180`)
      }
      return dst.mode == RtlMachineMode::kDi ? InsnType::kDarith : InsnType::kArith;
    case RtlOp::kMult:
      return is_fp_mode ? InsnType::kFmul : InsnType::kImul;
    case RtlOp::kTruncate:
      if (!src.children.empty() && src.children[0].op == RtlOp::kLshiftrt &&
          !src.children[0].children.empty() && src.children[0].children[0].op == RtlOp::kMult) {
        // `smulsi3_highpart`/`umulsi3_highpart`/`smuldi3_highpart`/`umuldi3_highpart`
        // (`mips.md:986-1070`).
        return InsnType::kImul;
      }
      return InsnType::kDarith;  // `truncdisi2`/`truncdihi2`/`truncdiqi2` (`mips.md:2084-2177`)
    case RtlOp::kDiv:
    case RtlOp::kUdiv:
    case RtlOp::kMod:
    case RtlOp::kUmod:
      return is_fp_mode ? InsnType::kFdiv : InsnType::kIdiv;
    case RtlOp::kSqrt:
      return InsnType::kFsqrt;
    case RtlOp::kAbs:
      return is_fp_mode ? InsnType::kFabs : InsnType::kArith;
    case RtlOp::kNeg:
      return is_fp_mode ? InsnType::kFneg : InsnType::kArith;
    case RtlOp::kFloat:
    case RtlOp::kUnsignedFloat:
    case RtlOp::kFix:
    case RtlOp::kUnsignedFix:
    case RtlOp::kFloatExtend:
    case RtlOp::kFloatTruncate:
      return InsnType::kFcvt;
    case RtlOp::kZeroExtend:
    case RtlOp::kSignExtend:
      if (!src.children.empty() && src.children[0].op == RtlOp::kMem) {
        return InsnType::kLoad;  // `mips.md:2186-2458`
      }
      return InsnType::kArith;
    case RtlOp::kUnalignedLoad:
      return InsnType::kLoad;
    case RtlOp::kUnalignedStore:
      return InsnType::kStore;
    case RtlOp::kUnspec:
      if (src.int_val == 0) {
        return InsnType::kLoad;  // `movsi_ulw`, `mips.md:2952`
      }
      if (src.int_val == 1 && dst.op == RtlOp::kMem) {
        return InsnType::kStore;  // `movsi_usw`, `mips.md:2978`
      }
      return InsnType::kUnknown;
    default:
      break;
  }

  // Move instructions (`movqi`/`movhi`/`movsi`/`movdi`/`movsf`/`movdf`).
  if (dst.op == RtlOp::kMem) {
    return InsnType::kStore;
  }
  if (src.op == RtlOp::kMem) {
    return InsnType::kLoad;
  }
  const RegClassKind dst_cls = ClassifyRegOperand(dst);
  const RegClassKind src_cls = ClassifyRegOperand(src);

  if (src.op == RtlOp::kConstDouble) {
    if (dst.mode == RtlMachineMode::kSf) {
      // `movsf_internal1` (`mips.md:3449-3460`):
      // alt 1: `=f <- G` (`xfer`), alt 3: `=f <- Fm` (`load`),
      // alt 8: `*d <- *G*d` (`move`), alt 10: `*d <- *F*m` (`load`).
      if (IsConstDoubleZero(src)) {
        return dst_cls == RegClassKind::kGpr ? InsnType::kMove : InsnType::kXfer;
      }
      return InsnType::kLoad;
    }
    if (dst.mode == RtlMachineMode::kDf) {
      // `movdf_internal1` (`mips.md:3497-3509`):
      // alt 5: `=f <- F` (`load`), alt 8: `*d <- *d*G` (`move`), alt 10: `*d <- *o*F` (`load`).
      if (IsConstDoubleZero(src) && dst_cls == RegClassKind::kGpr) {
        return InsnType::kMove;
      }
      return InsnType::kLoad;
    }
    return InsnType::kArith;  // `movdi_internal` alt 1 (`=d <- iF`)
  }

  if (IsConstantP(src) || src.op == RtlOp::kHigh || src.op == RtlOp::kLoSum) {
    if (dst_cls == RegClassKind::kMd) {
      return InsnType::kHilo;
    }
    if (dst.mode == RtlMachineMode::kSf && src.op == RtlOp::kConstInt && src.int_val == 0) {
      return dst_cls == RegClassKind::kGpr ? InsnType::kMove : InsnType::kXfer;
    }
    return InsnType::kArith;
  }

  if (dst_cls != RegClassKind::kNonReg && src_cls != RegClassKind::kNonReg) {
    if (dst_cls == RegClassKind::kMd || src_cls == RegClassKind::kMd) {
      return InsnType::kHilo;
    }
    if (dst_cls == RegClassKind::kSt || src_cls == RegClassKind::kSt) {
      return InsnType::kXfer;  // `movsi_internal2` / `movhi_internal2` `*z`
    }
    if (is_fp_mode) {
      // `movsf_internal1` / `movdf_internal1` (`mips.md:3450-3508`):
      // In `constrain_operands`, a pseudo matches `=f`/`f` first; a hard GPR forces
      // alt 6 (`*f <- *d`) or alt 7 (`*d <- *f`), both of which have `type = xfer`.
      const bool dst_is_gpr = (dst_cls == RegClassKind::kGpr);
      const bool src_is_gpr = (src_cls == RegClassKind::kGpr);
      if (dst_is_gpr != src_is_gpr) {
        return InsnType::kXfer;
      }
      return InsnType::kMove;
    }
    const bool dst_is_fpr = (dst_cls == RegClassKind::kFpr);
    const bool src_is_fpr = (src_cls == RegClassKind::kFpr);
    if (dst_is_fpr != src_is_fpr) {
      return InsnType::kXfer;
    }
    return InsnType::kMove;
  }

  if (IsComparisonOp(src.op)) {
    return InsnType::kArith;
  }
  return InsnType::kArith;
}

// Extracts `REG_DEAD` / `REG_UNUSED` presence on a parsed instruction.
bool HasRegNoteFor(const RtlPatternAndNotes& pn, RtlMachineMode kind, int regno) {
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind == kind && note.datum.op == RtlOp::kReg && note.datum.RegNo() == regno) {
      return true;
    }
  }
  return false;
}

// Mirrors `rtlanal.c:dead_or_set_regno_p` for `attach_deaths` (`sched.c:2872, 2882, 2901`).
bool DeadOrSetRegnoInInsn(const RtlPatternAndNotes& pn, int regno,
                          const absl::btree_set<int>& newly_attached_dead, const MachineModel& mm) {
  if (newly_attached_dead.contains(regno)) {
    return true;
  }
  auto sets_regno = [&](const RtlExpr& elem) -> bool {
    if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
      return false;
    }
    const RtlExpr& dest = elem.children[0];
    if (dest.op == RtlOp::kReg) {
      const int d = dest.RegNo();
      if (d < kMipsFirstPseudoRegister) {
        const int n = mm.HardRegNRegs(d, dest.mode);
        return regno >= d && regno < d + n;
      }
      return d == regno;
    }
    if (dest.op == RtlOp::kSubreg && !dest.children.empty() && dest.children[0].op == RtlOp::kReg) {
      const RtlExpr& inner = dest.children[0];
      if (ModeByteSize(inner.mode) <= ModeByteSize(dest.mode)) {
        const int d = inner.RegNo();
        if (d < kMipsFirstPseudoRegister) {
          const int n = mm.HardRegNRegs(d, inner.mode);
          return regno >= d && regno < d + n;
        }
        return d == regno;
      }
    }
    return false;
  };
  if (pn.pattern.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pn.pattern.children) {
      if (sets_regno(ch)) {
        return true;
      }
    }
  } else if (sets_regno(pn.pattern)) {
    return true;
  }
  for (const RtlExpr& fu : pn.call_fusage) {
    if (fu.op == RtlOp::kClobber && sets_regno(fu)) {
      return true;
    }
  }
  return false;
}

bool DeadOrSetExprInInsn(const RtlPatternAndNotes& pn, const RtlExpr& reg_expr,
                         const absl::btree_set<int>& newly_attached_dead, const MachineModel& mm) {
  const int regno = reg_expr.RegNo();
  if (regno >= kMipsFirstPseudoRegister) {
    return DeadOrSetRegnoInInsn(pn, regno, newly_attached_dead, mm);
  }
  const int n = mm.HardRegNRegs(regno, reg_expr.mode);
  for (int i = 0; i < n; ++i) {
    if (!DeadOrSetRegnoInInsn(pn, regno + i, newly_attached_dead, mm)) {
      return false;
    }
  }
  return true;
}

// Mirrors `sched.c:2280-2368` (`sched_note_set`).
void SchedNoteSet(const RtlExpr& x, bool death, const MachineModel& mm,
                  absl::btree_set<int>* bb_live_regs) {
  if (x.children.empty()) {
    return;
  }
  const RtlExpr* reg = &x.children[0];
  bool subreg_p = false;
  while (reg->op == RtlOp::kSubreg || reg->op == RtlOp::kStrictLowPart ||
         reg->op == RtlOp::kSignExtract || reg->op == RtlOp::kZeroExtract) {
    if (reg->children.empty()) {
      return;
    }
    if (reg->op != RtlOp::kSubreg ||
        ModeByteSize(reg->children[0].mode) > ModeByteSize(reg->mode)) {
      subreg_p = true;
    }
    reg = &reg->children[0];
  }
  if (reg->op != RtlOp::kReg) {
    return;
  }
  const int regno = reg->RegNo();
  if (death && subreg_p) {
    return;
  }
  if (regno < kMipsFirstPseudoRegister) {
    const int n = mm.HardRegNRegs(regno, reg->mode);
    for (int j = 0; j < n; ++j) {
      if (death) {
        bb_live_regs->erase(regno + j);
      } else {
        bb_live_regs->insert(regno + j);
      }
    }
  } else {
    if (death) {
      bb_live_regs->erase(regno);
    } else {
      bb_live_regs->insert(regno);
    }
  }
}

void ApplyForwardInsnLivenessStep(const Insn& insn, const MachineModel& mm,
                                  absl::btree_set<int>* bb_live_regs) {
  // Mirrors `sched.c:3440-3491, 3541-3609`: birth from SET/CLOBBER, then kill REG_DEAD/REG_UNUSED.
  const RtlExpr& pat = insn.pn.pattern;
  if (pat.op == RtlOp::kSet || pat.op == RtlOp::kClobber) {
    SchedNoteSet(pat, /*death=*/false, mm, bb_live_regs);
  } else if (pat.op == RtlOp::kParallel) {
    for (size_t j = pat.children.size(); j > 0; --j) {
      const RtlExpr& ch = pat.children[j - 1];
      if (ch.op == RtlOp::kSet || ch.op == RtlOp::kClobber) {
        SchedNoteSet(ch, /*death=*/false, mm, bb_live_regs);
      }
    }
    for (size_t j = pat.children.size(); j > 0; --j) {
      const RtlExpr& ch = pat.children[j - 1];
      if (ch.op == RtlOp::kUse) {
        SchedNoteSet(ch, /*death=*/false, mm, bb_live_regs);
      }
    }
  }
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if ((note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) &&
        note.datum.op == RtlOp::kReg) {
      const int regno = note.datum.RegNo();
      if (regno < kMipsFirstPseudoRegister) {
        const int n = mm.HardRegNRegs(regno, note.datum.mode);
        for (int j = 0; j < n; ++j) {
          bb_live_regs->erase(regno + j);
        }
      } else {
        bb_live_regs->erase(regno);
      }
    }
  }
}

RtlMachineMode RawModeForMipsHardReg(int regno, bool gp64, bool fp64) {
  if (regno >= kMipsFpRegFirst && regno <= kMipsFpRegLast) {
    return fp64 ? RtlMachineMode::kDf : RtlMachineMode::kSf;
  }
  if (regno == kMipsStatusRegnum) {
    return RtlMachineMode::kCc;
  }
  return gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi;
}

// Mirrors `sched.c:2779-2965` (`attach_deaths`).
void AttachDeathsExpr(const RtlExpr& x, const RtlPatternAndNotes& pn, bool set_p,
                      const MachineModel& mm, const absl::btree_set<int>& old_live_regs,
                      absl::btree_set<int>* bb_live_regs, absl::btree_set<int>* newly_attached_dead,
                      std::vector<RtlRegNote>* prepended_dead) {
  switch (x.op) {
    case RtlOp::kConstInt:
    case RtlOp::kConstDouble:
    case RtlOp::kLabelRef:
    case RtlOp::kSymbolRef:
    case RtlOp::kConst:
    case RtlOp::kCodeLabel:
    case RtlOp::kPc:
    case RtlOp::kCc0:
    case RtlOp::kScratch:
    case RtlOp::kAsmInput:
    case RtlOp::kAddrVec:
    case RtlOp::kAddrDiffVec:
    case RtlOp::kNote:
      return;
    case RtlOp::kReg: {
      if (set_p) {
        return;
      }
      const int regno = x.RegNo();
      bool all_needed = old_live_regs.contains(regno);
      bool some_needed = all_needed;
      int n_regs = 1;
      if (regno < kMipsFirstPseudoRegister) {
        n_regs = mm.HardRegNRegs(regno, x.mode);
        for (int n = n_regs - 1; n > 0; --n) {
          const bool live_n = old_live_regs.contains(regno + n);
          some_needed = some_needed || live_n;
          all_needed = all_needed && live_n;
        }
      }
      // `sched.c:2859-2866`: never attach `REG_DEAD` for `$fp`, `$sp`, or fixed `argp` (`$0`).
      const bool never_dead = (regno == kMipsFramePointerRegno || regno == kMipsStackPointerRegno ||
                               regno == kMipsArgPointerRegno);
      if (!never_dead && !all_needed && !DeadOrSetExprInInsn(pn, x, *newly_attached_dead, mm) &&
          !HasRegNoteFor(pn, RtlMachineMode::kRegUnused, regno)) {
        if (regno < kMipsFirstPseudoRegister && n_regs > 1) {
          for (int n = n_regs - 1; n >= 0; --n) {
            if (DeadOrSetRegnoInInsn(pn, regno + n, *newly_attached_dead, mm)) {
              some_needed = true;
            }
          }
        }
        if (!some_needed) {
          prepended_dead->insert(
              prepended_dead->begin(),
              RtlRegNote{
                  .kind = RtlMachineMode::kRegDead, .list_op = RtlOp::kExprList, .datum = x});
          for (int n = 0; n < n_regs; ++n) {
            newly_attached_dead->insert(regno + n);
          }
        } else {
          for (int i = n_regs - 1; i >= 0; --i) {
            if (!old_live_regs.contains(regno + i) &&
                !DeadOrSetRegnoInInsn(pn, regno + i, *newly_attached_dead, mm)) {
              const RtlMachineMode raw_m =
                  RawModeForMipsHardReg(regno + i, mm.Target64Bit(), mm.TargetFloat64());
              prepended_dead->insert(prepended_dead->begin(),
                                     RtlRegNote{.kind = RtlMachineMode::kRegDead,
                                                .list_op = RtlOp::kExprList,
                                                .datum = RtlExpr::MakeReg(raw_m, regno + i)});
              newly_attached_dead->insert(regno + i);
            }
          }
        }
      }
      if (regno < kMipsFirstPseudoRegister) {
        for (int j = 0; j < n_regs; ++j) {
          bb_live_regs->insert(regno + j);
        }
      } else {
        bb_live_regs->insert(regno);
      }
      return;
    }
    case RtlOp::kMem:
      if (!x.children.empty()) {
        AttachDeathsExpr(x.children[0], pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                         newly_attached_dead, prepended_dead);
      }
      return;
    case RtlOp::kSubreg:
    case RtlOp::kStrictLowPart:
      if (!x.children.empty()) {
        AttachDeathsExpr(x.children[0], pn, set_p, mm, old_live_regs, bb_live_regs,
                         newly_attached_dead, prepended_dead);
      }
      return;
    case RtlOp::kZeroExtract:
    case RtlOp::kSignExtract:
      if (x.children.size() >= 1) {
        AttachDeathsExpr(x.children[0], pn, set_p, mm, old_live_regs, bb_live_regs,
                         newly_attached_dead, prepended_dead);
      }
      if (x.children.size() >= 2) {
        AttachDeathsExpr(x.children[1], pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                         newly_attached_dead, prepended_dead);
      }
      if (x.children.size() >= 3) {
        AttachDeathsExpr(x.children[2], pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                         newly_attached_dead, prepended_dead);
      }
      return;
    default:
      if (IsVectorFormattedOp(x.op)) {
        for (const RtlExpr& ch : x.children) {
          AttachDeathsExpr(ch, pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                           newly_attached_dead, prepended_dead);
        }
      } else {
        for (size_t i = x.children.size(); i > 0; --i) {
          AttachDeathsExpr(x.children[i - 1], pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                           newly_attached_dead, prepended_dead);
        }
      }
      return;
  }
}

// Mirrors `sched.c:2971-3018` (`attach_deaths_insn`).
void AttachDeathsInsn(const Insn& insn, const MachineModel& mm,
                      const absl::btree_set<int>& old_live_regs, absl::btree_set<int>* bb_live_regs,
                      std::vector<RtlRegNote>* out_notes) {
  absl::btree_set<int> newly_attached_dead;
  std::vector<RtlRegNote> prepended_dead;

  const RtlExpr& x = insn.pn.pattern;
  if (x.op == RtlOp::kSet && x.children.size() >= 2) {
    AttachDeathsExpr(x.SetSrc(), insn.pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                     &newly_attached_dead, &prepended_dead);
    AttachDeathsExpr(x.SetDest(), insn.pn, /*set_p=*/true, mm, old_live_regs, bb_live_regs,
                     &newly_attached_dead, &prepended_dead);
  } else if (x.op == RtlOp::kParallel) {
    for (size_t i = x.children.size(); i > 0; --i) {
      const RtlExpr& elem = x.children[i - 1];
      if (elem.op == RtlOp::kSet && elem.children.size() >= 2) {
        AttachDeathsExpr(elem.SetSrc(), insn.pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                         &newly_attached_dead, &prepended_dead);
        AttachDeathsExpr(elem.SetDest(), insn.pn, /*set_p=*/true, mm, old_live_regs, bb_live_regs,
                         &newly_attached_dead, &prepended_dead);
      } else if (elem.op != RtlOp::kClobber) {
        AttachDeathsExpr(elem, insn.pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                         &newly_attached_dead, &prepended_dead);
      }
    }
  } else if (x.op == RtlOp::kClobber && !x.children.empty() && x.children[0].op == RtlOp::kMem &&
             !x.children[0].children.empty()) {
    AttachDeathsExpr(x.children[0].children[0], insn.pn, /*set_p=*/false, mm, old_live_regs,
                     bb_live_regs, &newly_attached_dead, &prepended_dead);
  } else if (x.op != RtlOp::kClobber) {
    AttachDeathsExpr(x, insn.pn, /*set_p=*/false, mm, old_live_regs, bb_live_regs,
                     &newly_attached_dead, &prepended_dead);
  }

  if (insn.kind == Insn::Kind::kCall) {
    for (const RtlExpr& fu : insn.pn.call_fusage) {
      if (!fu.children.empty()) {
        AttachDeathsExpr(fu.children[0], insn.pn, /*set_p=*/fu.op == RtlOp::kClobber, mm,
                         old_live_regs, bb_live_regs, &newly_attached_dead, &prepended_dead);
      }
    }
  }

  // `create_reg_dead_note` (`sched.c:2770-2772`) prepends newly attached `REG_DEAD` notes
  // ahead of all surviving non-`REG_DEAD` notes (`REG_UNUSED`, `REG_EQUIV`, `REG_EQUAL`, etc.).
  if (out_notes != nullptr) {
    out_notes->clear();
    out_notes->reserve(prepended_dead.size() + insn.pn.reg_notes.size());
    for (RtlRegNote& n : prepended_dead) {
      out_notes->push_back(std::move(n));
    }
    for (const RtlRegNote& n : insn.pn.reg_notes) {
      if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg) {
        continue;
      }
      out_notes->push_back(n);
    }
  }
}

// Mirrors `sched.c:2469-2501` (`birthing_insn_p`).
bool BirthingInsnP(const RtlExpr& pat, const absl::btree_set<int>& bb_live_regs,
                   const RegFacts& facts, Phase phase) {
  if (phase == Phase::kSched2) {
    return false;
  }
  if (pat.op == RtlOp::kSet && pat.children.size() >= 2 && pat.SetDest().op == RtlOp::kReg) {
    const int regno = pat.SetDest().RegNo();
    if (bb_live_regs.contains(regno)) {
      const auto it = facts.reg_n_sets.find(regno);
      return it != facts.reg_n_sets.end() && it->second == 1;
    }
    return false;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      if (BirthingInsnP(ch, bb_live_regs, facts, phase)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

namespace detail {

// Mirrors `sched.c:3197-3214` (block-0 leading hard-reg-source copy pinning).
bool IsBlock0PinnedParamCopy(const Insn& insn) {
  if (insn.kind != Insn::Kind::kInsn || insn.pn.pattern.op != RtlOp::kSet ||
      insn.pn.pattern.children.size() < 2) {
    return false;
  }
  const RtlExpr* src = &insn.pn.pattern.SetSrc();
  while ((src->op == RtlOp::kSubreg || src->op == RtlOp::kSignExtend ||
          src->op == RtlOp::kZeroExtend || src->op == RtlOp::kSignExtract ||
          src->op == RtlOp::kZeroExtract) &&
         !src->children.empty()) {
    src = &src->children[0];
  }
  return src->op == RtlOp::kReg && src->RegNo() < kMipsFirstPseudoRegister;
}

}  // namespace detail

namespace {

bool IsUseOfRegInsn(const Insn& insn) {
  return insn.kind == Insn::Kind::kInsn && insn.pn.pattern.op == RtlOp::kUse &&
         !insn.pn.pattern.children.empty() && insn.pn.pattern.children[0].op == RtlOp::kReg;
}

bool IsTailPinnedInsn(const Insn& insn) {
  return insn.kind == Insn::Kind::kCall || insn.kind == Insn::Kind::kJump ||
         (insn.kind == Insn::Kind::kInsn && insn.pn.pattern.op == RtlOp::kUse);
}

// Mirrors `sched.c:4825-4848` + `mips.md:3208-3221`: splits `(set OP0 (const_int LARGE))`
// where `OP0` is an `SImode` `register_operand` (`REG` or `SUBREG` of `REG`).
bool TrySplitLargeIntInsn(const RtlPatternAndNotes& orig, RtlPatternAndNotes* out_first,
                          RtlPatternAndNotes* out_second, int* out_dest_regno) {
  if (orig.pattern.op != RtlOp::kSet || orig.pattern.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = orig.pattern.SetDest();
  const RtlExpr& src = orig.pattern.SetSrc();
  if (dst.mode != RtlMachineMode::kSi || src.op != RtlOp::kConstInt ||
      !IsMipsLargeInt(src.int_val)) {
    return false;
  }
  int regno = -1;
  if (dst.op == RtlOp::kReg) {
    regno = dst.RegNo();
  } else if (dst.op == RtlOp::kSubreg && !dst.children.empty() &&
             dst.children[0].op == RtlOp::kReg) {
    regno = dst.children[0].RegNo();
  } else {
    return false;
  }

  const uint32_t u32 = static_cast<uint32_t>(src.int_val);
  const int64_t hi = static_cast<int32_t>(u32 & 0xffff0000u);
  const int64_t lo = static_cast<int64_t>(u32 & 0x0000ffffu);

  out_first->bracketed_notes = orig.bracketed_notes;
  out_first->pattern = RtlExpr::MakeSet(dst, RtlExpr::MakeConstInt(hi));
  out_first->reg_notes.clear();

  RtlExpr ior_expr;
  ior_expr.op = RtlOp::kIor;
  ior_expr.mode = RtlMachineMode::kSi;
  ior_expr.children = {dst, RtlExpr::MakeConstInt(lo)};

  out_second->bracketed_notes = orig.bracketed_notes;
  out_second->pattern = RtlExpr::MakeSet(dst, std::move(ior_expr));
  out_second->reg_notes.clear();

  // Mirrors `sched.c:4289-4469` (`update_flow_info`):
  // `REG_WAS_0` and `REG_LIBCALL` move to `first`; `REG_DEAD`, `REG_UNUSED`,
  // `REG_EQUAL`, `REG_EQUIV`, `REG_NO_CONFLICT`, `REG_RETVAL` move to `last`.
  // Note: `update_flow_info` iterates forward and prepends each note onto target's `REG_NOTES`!
  for (const RtlRegNote& note : orig.reg_notes) {
    switch (note.kind) {
      case RtlMachineMode::kRegWas0:
      case RtlMachineMode::kRegLibcall:
        out_first->reg_notes.insert(out_first->reg_notes.begin(), note);
        break;
      case RtlMachineMode::kRegDead:
      case RtlMachineMode::kRegUnused:
      case RtlMachineMode::kRegEqual:
      case RtlMachineMode::kRegEquiv:
      case RtlMachineMode::kRegNoConflict:
      case RtlMachineMode::kRegRetval:
        out_second->reg_notes.insert(out_second->reg_notes.begin(), note);
        break;
      default:
        out_second->reg_notes.insert(out_second->reg_notes.begin(), note);
        break;
    }
  }
  *out_dest_regno = regno;
  return true;
}

}  // namespace

// =============================================================================
// 1. MachineModel
// =============================================================================

MachineModel MachineModel::FromProfile(const CompilationProfile& profile) {
  MachineModel mm;
  int cc1_isa = 0;
  bool has_msingle_float = false;
  bool has_m4650 = false;
  for (std::string_view flag : profile.c_flags) {
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

  // `config/mips/elf64.h:26` (`MIPS_ISA_DEFAULT 3`) + `CC1_SPEC` defaults.
  mm.mips_isa_ = (cc1_isa != 0) ? cc1_isa : 3;
  if (cc1_isa == 3 || cc1_isa == 4) {
    mm.target_64bit_ = true;
    mm.target_float64_ = (!has_msingle_float && !has_m4650);
  } else {
    mm.target_64bit_ = false;
    mm.target_float64_ = false;
  }

  std::string cpu_str = has_m4650 ? "r4650" : "";
  for (std::string_view flag : profile.c_flags) {
    if (flag == "-mgp64") {
      mm.target_64bit_ = true;
    } else if (flag == "-mgp32") {
      mm.target_64bit_ = false;
    } else if (flag == "-mfp64") {
      mm.target_float64_ = true;
    } else if (flag == "-mfp32") {
      mm.target_float64_ = false;
    } else if (flag.size() > 6 && flag.substr(0, 6) == "-mcpu=") {
      cpu_str = std::string(flag.substr(6));
    }
  }

  // `config/mips/mips.c:3195-3277`.
  if (cpu_str.empty() || cpu_str == "default" || cpu_str == "DEFAULT") {
    switch (mm.mips_isa_) {
      case 2:
        mm.cpu_ = MipsCpu::kR6000;
        break;
      case 3:
        mm.cpu_ = MipsCpu::kR4000;
        break;
      case 4:
        mm.cpu_ = MipsCpu::kR8000;
        break;
      default:
        mm.cpu_ = MipsCpu::kR3000;
        break;
    }
  } else {
    std::string_view p = cpu_str;
    if (!p.empty() && (p[0] == 'r' || p[0] == 'R')) {
      p.remove_prefix(1);
    }
    if (p == "2000" || p == "2k" || p == "2K" || p == "3000" || p == "3k" || p == "3K") {
      mm.cpu_ = MipsCpu::kR3000;
    } else if (p == "4000" || p == "4k" || p == "4K" || p == "4400" || p == "4300" ||
               p == "vr4300") {
      mm.cpu_ = MipsCpu::kR4000;
    } else if (p == "4600" || p == "orion") {
      mm.cpu_ = MipsCpu::kR4600;
    } else if (p == "4650") {
      mm.cpu_ = MipsCpu::kR4650;
    } else if (p == "6000" || p == "6k" || p == "6K") {
      mm.cpu_ = MipsCpu::kR6000;
    } else if (p == "8000") {
      mm.cpu_ = MipsCpu::kR8000;
    } else {
      mm.cpu_ = MipsCpu::kDefault;
    }
  }
  return mm;
}

bool MachineModel::IsRecognized(const RtlPatternAndNotes& insn) const {
  const RtlExpr& pat = insn.pattern;
  if (pat.op == RtlOp::kUse || pat.op == RtlOp::kClobber || pat.op == RtlOp::kAsmInput ||
      pat.op == RtlOp::kAsmOperands || pat.op == RtlOp::kAddrVec || pat.op == RtlOp::kAddrDiffVec ||
      pat.op == RtlOp::kNote || pat.op == RtlOp::kUnknown) {
    return false;
  }
  if (ContainsAsmExpr(pat)) {
    return false;
  }
  return true;
}

InsnType MachineModel::TypeOf(const RtlPatternAndNotes& insn, Phase phase) const {
  if (!IsRecognized(insn)) {
    return InsnType::kUnknown;
  }
  const RtlExpr& pat = insn.pattern;
  if (pat.op == RtlOp::kReturn) {
    return InsnType::kJump;
  }
  if (pat.op == RtlOp::kCall) {
    return InsnType::kCall;
  }
  if (pat.op == RtlOp::kSet && pat.children.size() >= 2) {
    return ClassifySetPattern(pat, phase);
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
    for (const RtlExpr& ch : pat.children) {
      if (ch.op == RtlOp::kReturn) {
        return InsnType::kJump;
      }
      if (IsCallPattern(ch)) {
        return InsnType::kCall;
      }
    }
    const RtlExpr& first = pat.children[0];
    if (first.op == RtlOp::kSet && first.children.size() >= 2) {
      if (first.SetDest().op == RtlOp::kMem && first.SetDest().mode == RtlMachineMode::kBlk &&
          first.SetSrc().op == RtlOp::kMem && first.SetSrc().mode == RtlMachineMode::kBlk) {
        // `movstrsi_internal3` (`mips.md:3662-3676`) has `(use (const_int 2))` and `type = store`.
        for (const RtlExpr& ch : pat.children) {
          if (ch.op == RtlOp::kUse && !ch.children.empty() &&
              ch.children[0].op == RtlOp::kConstInt && ch.children[0].int_val == 2) {
            return InsnType::kStore;
          }
        }
        return InsnType::kMulti;
      }
      return ClassifySetPattern(first, phase);
    }
  }
  return InsnType::kUnknown;
}

RtlMachineMode MachineModel::AttrModeOf(const RtlPatternAndNotes& insn) const {
  const RtlExpr* set = SingleSetOf(insn.pattern);
  if (set == nullptr && insn.pattern.op == RtlOp::kParallel && !insn.pattern.children.empty() &&
      insn.pattern.children[0].op == RtlOp::kSet) {
    set = &insn.pattern.children[0];
  }
  if (set != nullptr && set->children.size() >= 2) {
    if (set->SetDest().mode != RtlMachineMode::kNone &&
        set->SetDest().mode != RtlMachineMode::kVoid) {
      return set->SetDest().mode;
    }
    return set->SetSrc().mode;
  }
  return RtlMachineMode::kNone;
}

UnitUse MachineModel::UnitOf(InsnType type, RtlMachineMode mode) const {
  UnitUse u;
  const bool is_r3000 = (cpu_ == MipsCpu::kR3000);
  const bool is_r4000 = (cpu_ == MipsCpu::kR4000);
  const bool is_r4600 = (cpu_ == MipsCpu::kR4600);
  const bool is_r4650 = (cpu_ == MipsCpu::kR4650);
  const bool is_r6000 = (cpu_ == MipsCpu::kR6000);

  switch (type) {
    case InsnType::kLoad:
      u.unit = kUnitMemory;
      u.simultaneity = 0;
      u.issue = 0;
      u.ready = (is_r3000 || is_r4600 || is_r4650) ? 2 : 3;
      // `insn-attrtab.c:1831-1891`: `memory_unit_blockage_range`:
      // `ready==3` -> `[1,1]`, `ready==2` -> `[1,2]`, `ready==1` -> `[1,3]`.
      u.min_blockage = 1;
      u.max_blockage = (u.ready == 3) ? 1 : 2;
      return u;
    case InsnType::kStore:
      u.unit = kUnitMemory;
      u.simultaneity = 0;
      u.issue = 0;
      u.ready = 1;
      u.min_blockage = 1;
      u.max_blockage = 3;
      return u;
    case InsnType::kXfer:
      u.unit = kUnitMemory;
      u.simultaneity = 0;
      u.issue = 0;
      u.ready = 2;
      u.min_blockage = 1;
      u.max_blockage = 2;
      return u;
    case InsnType::kHilo:
      u.unit = kUnitImuldiv;
      u.simultaneity = 0;
      u.ready = 1;
      u.issue = 3;
      u.min_blockage = 3;
      u.max_blockage = 69;
      return u;
    case InsnType::kImul:
      u.unit = kUnitImuldiv;
      u.simultaneity = 0;
      u.ready = is_r3000 ? 12 : (is_r4000 || is_r4600) ? 10 : is_r4650 ? 4 : 17;
      u.issue = u.ready;
      u.min_blockage = 3;
      u.max_blockage = 69;
      return u;
    case InsnType::kIdiv:
      u.unit = kUnitImuldiv;
      u.simultaneity = 0;
      u.ready = is_r3000 ? 35 : is_r4600 ? 42 : is_r4650 ? 36 : is_r4000 ? 69 : 38;
      u.issue = u.ready;
      u.min_blockage = 3;
      u.max_blockage = 69;
      return u;
    case InsnType::kFcmp:
      u.unit = kUnitAdder;
      u.simultaneity = 1;
      u.issue = 0;
      u.ready = (is_r3000 || is_r6000) ? 2 : 3;
      u.min_blockage = 1;
      u.max_blockage = 4;
      return u;
    case InsnType::kFadd:
      u.unit = kUnitAdder;
      u.simultaneity = 1;
      u.issue = 0;
      u.ready = is_r3000 ? 2 : is_r6000 ? 3 : 4;
      u.min_blockage = 1;
      u.max_blockage = 4;
      return u;
    case InsnType::kFabs:
    case InsnType::kFneg:
      u.unit = kUnitAdder;
      u.simultaneity = 1;
      u.issue = 0;
      u.ready = (is_r3000 || is_r4600 || is_r4650) ? 1 : 2;
      u.min_blockage = 1;
      u.max_blockage = 4;
      return u;
    case InsnType::kFmul:
      u.unit = kUnitMult;
      u.simultaneity = 1;
      u.issue = 0;
      if (mode == RtlMachineMode::kDf) {
        u.ready = is_r3000 ? 5 : is_r6000 ? 6 : 8;
      } else {
        u.ready = is_r3000 ? 4 : is_r6000 ? 5 : (is_r4600 || is_r4650) ? 8 : 7;
      }
      u.min_blockage = 4;
      u.max_blockage = 8;
      return u;
    case InsnType::kFdiv:
      u.unit = kUnitDivide;
      u.simultaneity = 1;
      u.issue = 0;
      if (mode == RtlMachineMode::kDf) {
        u.ready = is_r3000 ? 19 : is_r6000 ? 16 : (is_r4600 || is_r4650) ? 61 : 36;
      } else {
        u.ready = is_r3000 ? 12 : is_r6000 ? 15 : (is_r4600 || is_r4650) ? 32 : 23;
      }
      u.min_blockage = 12;
      u.max_blockage = 112;
      return u;
    case InsnType::kFsqrt:
      u.unit = kUnitDivide;
      u.simultaneity = 1;
      u.issue = 0;
      if (mode == RtlMachineMode::kDf) {
        u.ready = (is_r4600 || is_r4650) ? 60 : 112;
      } else {
        u.ready = (is_r4600 || is_r4650) ? 31 : 54;
      }
      u.min_blockage = 12;
      u.max_blockage = 112;
      return u;
    default:
      return u;
  }
}

int MachineModel::ResultReadyCost(const RtlPatternAndNotes& insn, Phase phase) const {
  if (!IsRecognized(insn)) {
    return 1;
  }
  const UnitUse u = UnitOf(TypeOf(insn, phase), AttrModeOf(insn));
  return std::max(1, u.ready);
}

int MachineModel::Blockage(const UnitUse& executing, const UnitUse& candidate) const {
  // Mirrors `genattrtab.c:1986-1996`:
  // - `simultaneity == 0`: `max(issue(E, C), ready(E) - (ready(C) - 1))` (floored at 1).
  // - `simultaneity == 1`: `filltime == 0`, so `max(issue(E, C), ready(E))`.
  const int eff_issue = std::max(1, executing.issue);
  if (executing.simultaneity != 0) {
    return std::max(eff_issue, executing.ready);
  }
  const int pipe = executing.ready - std::max(0, candidate.ready - 1);
  return std::max(eff_issue, pipe);
}

int MachineModel::MaxBlockage(int unit) const {
  // `insn-attrtab.c:6298-6302`.
  switch (unit) {
    case kUnitMemory:
      return 3;
    case kUnitImuldiv:
      return 69;
    case kUnitAdder:
      return 4;
    case kUnitMult:
      return 8;
    case kUnitDivide:
      return 112;
    default:
      return 1;
  }
}

bool MachineModel::CallUsed(int hard_regno) const {
  // Exact `CALL_USED_REGISTERS` (`config/mips/mips.h:1203-1210`).
  if (hard_regno < 0 || hard_regno >= kMipsFirstPseudoRegister) {
    return false;
  }
  return (hard_regno <= 15) || (hard_regno >= 24 && hard_regno <= 29) || (hard_regno == 31) ||
         (hard_regno >= 32 && hard_regno <= 51) || (hard_regno >= 64 && hard_regno <= 67);
}

int MachineModel::HardRegNRegs(int hard_regno, RtlMachineMode mode) const {
  // `config/mips/mips.h:1255-1258`.
  const int size = ModeByteSize(mode);
  if (size <= 0) {
    return 1;
  }
  const int unit_bytes = (hard_regno >= kMipsFpRegFirst && hard_regno <= kMipsFpRegLast)
                             ? (target_float64_ ? 8 : 4)
                             : (target_64bit_ ? 8 : 4);
  return std::max(1, (size + unit_bytes - 1) / unit_bytes);
}

// =============================================================================
// 2. RegFacts::FromCfg (deterministic, zero B4 sideband-presence branching)
// =============================================================================

RegFacts RegFacts::FromCfg(const RtlControlFlowGraph& cfg, const CompilationProfile& profile,
                           Phase phase) {
  RegFacts facts;
  facts.frame_pointer_needed = cfg.sideband.frame_pointer_needed;
  facts.max_reg_num = std::max(kMipsFirstPseudoRegister, cfg.sideband.max_regno);

  // Derive uniform baseline liveness facts from the CFG without branching on whether
  // `cfg.sideband.basic_block_live_at_start` was pre-populated (resolving B4).
  RtlControlFlowGraph flow_cfg = cfg;
  ReconstructRtlLiveness(&flow_cfg, profile,
                         {.reload_completed = (phase == Phase::kSched2),
                          .update_reg_notes = false,
                          .preserve_phantom_pseudos = true});
  facts.max_reg_num = std::max(facts.max_reg_num, flow_cfg.sideband.max_regno);

  for (const auto& [r, v] : flow_cfg.sideband.reg_n_sets) {
    facts.reg_n_sets[r] = v;
  }
  for (const auto& [r, v] : flow_cfg.sideband.reg_n_calls_crossed) {
    facts.reg_n_calls_crossed[r] = v;
  }
  for (const auto& [r, v] : flow_cfg.sideband.reg_live_length) {
    facts.reg_live_length[r] = v;
  }

  // Honor explicit sideband stats (including negative `reg_live_length` sentinels
  // per `sched.c:4946` and preserved pre-sched `reg_n_calls_crossed` / `reg_n_sets`).
  for (const auto& [r, v] : cfg.sideband.reg_n_sets) {
    if (v > 0) {
      facts.reg_n_sets[r] = v;
    }
  }
  for (const auto& [r, v] : cfg.sideband.reg_n_calls_crossed) {
    facts.reg_n_calls_crossed[r] = v;
  }
  for (const auto& [r, v] : cfg.sideband.reg_live_length) {
    if (v != 0) {
      facts.reg_live_length[r] = v;
    }
  }

  // Track `reg_basic_block[r] == REG_BLOCK_GLOBAL` (used in multiple blocks or live-in).
  absl::btree_map<int, int> seen_block;
  for (size_t b = 0; b < cfg.rpo_blocks.size(); ++b) {
    const auto live_it = flow_cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
    if (live_it != flow_cfg.sideband.basic_block_live_at_start.end()) {
      for (int r : live_it->second) {
        if (b > 0) {
          facts.reg_block_global.insert(r);
        }
      }
    }
    for (const std::string& pat_str : cfg.rpo_blocks[b].instructions) {
      auto parsed = ParsePatternAndNotes(pat_str);
      if (!parsed.ok()) {
        continue;
      }
      auto record_reg = [&](const RtlExpr& e) {
        if (e.op == RtlOp::kReg) {
          const int r = e.RegNo();
          if (r >= facts.max_reg_num) {
            facts.max_reg_num = r + 1;
          }
          auto [it, inserted] = seen_block.emplace(r, static_cast<int>(b));
          if (!inserted && it->second != static_cast<int>(b)) {
            facts.reg_block_global.insert(r);
          }
        }
      };
      parsed->pattern.Visit(record_reg);
      for (const RtlRegNote& note : parsed->reg_notes) {
        note.datum.Visit(record_reg);
      }

      // `init_alias_analysis` (`sched.c:421-433`).
      const RtlExpr* set = SingleSetOf(parsed->pattern);
      if (set != nullptr && set->SetDest().op == RtlOp::kReg &&
          set->SetDest().RegNo() >= kMipsFirstPseudoRegister) {
        const int regno = set->SetDest().RegNo();
        const int n_sets = facts.reg_n_sets.contains(regno) ? facts.reg_n_sets.at(regno) : 0;
        const RtlRegNote* chosen = nullptr;
        for (const RtlRegNote& note : parsed->reg_notes) {
          if (note.kind == RtlMachineMode::kRegEqual && n_sets == 1) {
            chosen = &note;
            break;
          }
        }
        if (chosen == nullptr) {
          for (const RtlRegNote& note : parsed->reg_notes) {
            if (note.kind == RtlMachineMode::kRegEquiv) {
              chosen = &note;
              break;
            }
          }
        }
        if (chosen != nullptr && chosen->datum.op != RtlOp::kExprList) {
          facts.reg_known_value[regno] = chosen->datum;
          if (chosen->kind == RtlMachineMode::kRegEquiv) {
            facts.reg_known_equiv_p.insert(regno);
          } else {
            facts.reg_known_equiv_p.erase(regno);
          }
        }
      }
    }
  }

  return facts;
}

// =============================================================================
// 3. Alias Analysis (`sched.c:370-882`, `rtlanal.c:76-183`)
// =============================================================================

RtlExpr CanonRtx(const RtlExpr& x, const RegFacts& facts) {
  // Mirrors `sched.c:370-394`.
  if (x.op == RtlOp::kReg && x.RegNo() >= kMipsFirstPseudoRegister &&
      x.RegNo() <= facts.max_reg_num) {
    const auto it = facts.reg_known_value.find(x.RegNo());
    if (it != facts.reg_known_value.end()) {
      return it->second;
    }
    return x;
  }
  if (x.op == RtlOp::kPlus && x.children.size() == 2) {
    RtlExpr x0 = CanonRtx(x.children[0], facts);
    RtlExpr x1 = CanonRtx(x.children[1], facts);
    if (x0 != x.children[0] || x1 != x.children[1]) {
      if (x0.op == RtlOp::kConstInt) {
        return PlusConstantForOutput(x1, x0.int_val);
      }
      if (x1.op == RtlOp::kConstInt) {
        return PlusConstantForOutput(x0, x1.int_val);
      }
      RtlExpr res = x;
      res.children[0] = std::move(x0);
      res.children[1] = std::move(x1);
      return res;
    }
  }
  return x;
}

bool RtxEqualForMemrefP(const RtlExpr& x_in, const RtlExpr& y_in, const RegFacts& facts) {
  // Mirrors `sched.c:446-555`.
  const RtlExpr x = CanonRtx(x_in, facts);
  const RtlExpr y = CanonRtx(y_in, facts);

  if (x.op != y.op || x.mode != y.mode) {
    return false;
  }
  if (x.op == RtlOp::kReg) {
    return x.RegNo() == y.RegNo() && x.IsArgPointerReg() == y.IsArgPointerReg();
  }
  if (x.op == RtlOp::kLabelRef) {
    return x.str_val == y.str_val && x.int_val == y.int_val;
  }
  if (x.op == RtlOp::kSymbolRef) {
    return x.str_val == y.str_val;
  }
  if (x.op == RtlOp::kConstInt) {
    return x.int_val == y.int_val;
  }
  if (x.op == RtlOp::kConstDouble) {
    return x.int_val == y.int_val && x.int_val_high == y.int_val_high &&
           x.extra_ints == y.extra_ints;
  }
  if (IsCommutativeOp(x.op) && x.children.size() == 2 && y.children.size() == 2) {
    return (RtxEqualForMemrefP(x.children[0], y.children[0], facts) &&
            RtxEqualForMemrefP(x.children[1], y.children[1], facts)) ||
           (RtxEqualForMemrefP(x.children[0], y.children[1], facts) &&
            RtxEqualForMemrefP(x.children[1], y.children[0], facts));
  }
  if (x.int_val != y.int_val || x.int_val_high != y.int_val_high || x.str_val != y.str_val ||
      x.children.size() != y.children.size()) {
    return false;
  }
  for (size_t i = 0; i < x.children.size(); ++i) {
    if (!RtxEqualForMemrefP(x.children[i], y.children[i], facts)) {
      return false;
    }
  }
  return true;
}

bool RtxVariesP(const RtlExpr& x, const RegFacts& facts, Phase phase) {
  // Mirrors `rtlanal.c:76-116`:
  // - `MEM` always varies.
  // - `CONST`/`CONST_INT`/`CONST_DOUBLE`/`SYMBOL_REF`/`LABEL_REF` never vary.
  // - `REG` exempts ONLY `frame_pointer_rtx`, `hard_frame_pointer_rtx`, `arg_pointer_rtx`
  //   (`stack_pointer_rtx` `$sp` IS varying!).
  // - `LO_SUM` checks operand 1 ONLY (`rtlanal.c:104-107`).
  switch (x.op) {
    case RtlOp::kMem:
      return true;
    case RtlOp::kConst:
    case RtlOp::kConstInt:
    case RtlOp::kConstDouble:
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
      return false;
    case RtlOp::kReg:
      return !(IsFramePointerRtx(x) || IsArgPointerRtx(x));
    case RtlOp::kLoSum:
      return x.children.size() >= 2 ? RtxVariesP(x.children[1], facts, phase) : false;
    default:
      for (size_t i = x.children.size(); i > 0; --i) {
        if (RtxVariesP(x.children[i - 1], facts, phase)) {
          return true;
        }
      }
      return false;
  }
}

bool RtxAddrVariesP(const RtlExpr& x, const RegFacts& facts, Phase phase) {
  // Mirrors `rtlanal.c:163-183`.
  if (x.op == RtlOp::kMem) {
    if (x.mode == RtlMachineMode::kBlk) {
      return true;
    }
    return !x.children.empty() && RtxVariesP(x.children[0], facts, phase);
  }
  for (size_t i = x.children.size(); i > 0; --i) {
    if (RtxAddrVariesP(x.children[i - 1], facts, phase)) {
      return true;
    }
  }
  return false;
}

bool MemrefsConflictP(int xsize, const RtlExpr& x_in, int ysize, const RtlExpr& y_in, int64_t c,
                      const RegFacts& facts, Phase phase) {
  // Mirrors `sched.c:614-783` verbatim (including exact swap-without-negating-`c` at
  // `sched.c:636-643`, `MULT` division at `sched.c:727-750`, and `CONST` unwrapping gated on
  // `CONSTANT_P(x)` at `sched.c:753-774`).
  RtlExpr x = (x_in.op == RtlOp::kHigh && !x_in.children.empty())       ? x_in.children[0]
              : (x_in.op == RtlOp::kLoSum && x_in.children.size() >= 2) ? x_in.children[1]
                                                                        : CanonRtx(x_in, facts);
  RtlExpr y = (y_in.op == RtlOp::kHigh && !y_in.children.empty())       ? y_in.children[0]
              : (y_in.op == RtlOp::kLoSum && y_in.children.size() >= 2) ? y_in.children[1]
                                                                        : CanonRtx(y_in, facts);

  auto overlap = [&](int xs, int ys, int64_t off) -> bool {
    return xs == 0 || ys == 0 || (off >= 0 && xs > off) || (off < 0 && ys + off > 0);
  };

  if (RtxEqualForMemrefP(x, y, facts)) {
    return overlap(xsize, ysize, c);
  }

  // `sched.c:636-643`: intentionally leaves `c` unchanged when swapping `x` and `y`!
  if (IsStackOrFramePointerRtx(y)) {
    std::swap(x, y);
    std::swap(xsize, ysize);
  }

  if (IsStackOrFramePointerRtx(x)) {
    if (IsConstantP(y)) {
      return false;
    }
    if (y.op == RtlOp::kPlus && y.children.size() == 2) {
      const RtlExpr y0 = CanonRtx(y.children[0], facts);
      const RtlExpr y1 = CanonRtx(y.children[1], facts);
      if (RtxEqualForMemrefP(y0, x, facts) && y1.op == RtlOp::kConstInt) {
        return overlap(xsize, ysize, c + y1.int_val);
      }
      if (IsConstantP(y0)) {
        return false;
      }
    }
    return true;
  }

  if (x.op == RtlOp::kPlus && x.children.size() == 2) {
    const RtlExpr& x0 = x.children[0];
    const RtlExpr& x1 = x.children[1];
    if (y.op == RtlOp::kPlus && y.children.size() == 2) {
      const RtlExpr& y0 = y.children[0];
      const RtlExpr& y1 = y.children[1];
      if (RtxEqualForMemrefP(x1, y1, facts)) {
        return MemrefsConflictP(xsize, x0, ysize, y0, c, facts, phase);
      }
      if (RtxEqualForMemrefP(x0, y0, facts)) {
        return MemrefsConflictP(xsize, x1, ysize, y1, c, facts, phase);
      }
      if (x1.op == RtlOp::kConstInt) {
        if (y1.op == RtlOp::kConstInt) {
          return MemrefsConflictP(xsize, x0, ysize, y0, c - x1.int_val + y1.int_val, facts, phase);
        }
        return MemrefsConflictP(xsize, x0, ysize, y, c - x1.int_val, facts, phase);
      }
      if (y1.op == RtlOp::kConstInt) {
        return MemrefsConflictP(xsize, x, ysize, y0, c + y1.int_val, facts, phase);
      }
      const auto sx = FindSymbolicTerm(x);
      if (!sx.has_value()) {
        return true;
      }
      const auto sy = FindSymbolicTerm(y);
      if (!sy.has_value()) {
        return true;
      }
      return RtxEqualForMemrefP(*sx, *sy, facts);
    }
    if (x1.op == RtlOp::kConstInt) {
      return MemrefsConflictP(xsize, x0, ysize, y, c - x1.int_val, facts, phase);
    }
  } else if (y.op == RtlOp::kPlus && y.children.size() == 2) {
    const RtlExpr& y0 = y.children[0];
    const RtlExpr& y1 = y.children[1];
    if (y1.op == RtlOp::kConstInt) {
      return MemrefsConflictP(xsize, x, ysize, y0, c + y1.int_val, facts, phase);
    }
    return true;
  }

  // `sched.c:724-751`: `MULT` case.
  if (x.op == RtlOp::kMult && y.op == RtlOp::kMult && x.children.size() == 2 &&
      y.children.size() == 2) {
    const RtlExpr x1 = CanonRtx(x.children[1], facts);
    const RtlExpr y1 = CanonRtx(y.children[1], facts);
    if (!RtxEqualForMemrefP(x1, y1, facts)) {
      return true;
    }
    const RtlExpr x0 = CanonRtx(x.children[0], facts);
    const RtlExpr y0 = CanonRtx(y.children[0], facts);
    if (RtxEqualForMemrefP(x0, y0, facts)) {
      return overlap(xsize, ysize, c);
    }
    if (x1.op != RtlOp::kConstInt || x1.int_val == 0) {
      return true;
    }
    return MemrefsConflictP(static_cast<int>(xsize / x1.int_val), x0,
                            static_cast<int>(ysize / x1.int_val), y0, c / x1.int_val, facts, phase);
  }

  // `sched.c:753-781`: entered strictly when `CONSTANT_P(x)`.
  if (IsConstantP(x)) {
    if (x.op == RtlOp::kConstInt && y.op == RtlOp::kConstInt) {
      return overlap(xsize, ysize, c + (y.int_val - x.int_val));
    }
    if (x.op == RtlOp::kConst && !x.children.empty()) {
      const RtlExpr cx = CanonRtx(x.children[0], facts);
      if (y.op == RtlOp::kConst && !y.children.empty()) {
        return MemrefsConflictP(xsize, cx, ysize, CanonRtx(y.children[0], facts), c, facts, phase);
      }
      return MemrefsConflictP(xsize, cx, ysize, y, c, facts, phase);
    }
    if (y.op == RtlOp::kConst && !y.children.empty()) {
      return MemrefsConflictP(xsize, x, ysize, CanonRtx(y.children[0], facts), c, facts, phase);
    }
    if (IsConstantP(y)) {
      return RtxEqualForMemrefP(x, y, facts) && overlap(xsize, ysize, c);
    }
    return true;
  }
  return true;
}

bool ReadDependence(const RtlExpr& r1_mem, const RtlExpr& r2_mem) {
  return r1_mem.flags.volatil && r2_mem.flags.volatil;  // `sched.c:807-812`
}

namespace {

bool BaseMemDependence(const RtlExpr& mem, const RtlExpr& x, const RegFacts& facts, Phase phase) {
  if (x.flags.volatil && mem.flags.volatil) {
    return true;
  }
  if (mem.children.empty() || x.children.empty()) {
    return true;
  }
  if (!MemrefsConflictP(ModeByteSize(mem.mode), mem.children[0], ModeByteSize(x.mode),
                        x.children[0], 0, facts, phase)) {
    return false;
  }
  const bool mem_varies = RtxAddrVariesP(mem, facts, phase);
  const bool x_varies = RtxAddrVariesP(x, facts, phase);
  if (mem.flags.in_struct && mem_varies && mem.mode != RtlMachineMode::kQi && !x.flags.in_struct &&
      !x_varies) {
    return false;
  }
  if (x.flags.in_struct && x_varies && x.mode != RtlMachineMode::kQi && !mem.flags.in_struct &&
      !mem_varies) {
    return false;
  }
  return true;
}

}  // namespace

bool TrueDependence(const RtlExpr& write_mem, const RtlExpr& read_mem, const RegFacts& facts,
                    Phase phase) {
  // `sched.c:817-840`
  if (read_mem.flags.unchanging && !write_mem.flags.unchanging) {
    return false;
  }
  return BaseMemDependence(write_mem, read_mem, facts, phase);
}

bool AntiDependence(const RtlExpr& read_mem, const RtlExpr& write_mem, const RegFacts& facts,
                    Phase phase) {
  // `sched.c:845-864`
  if (read_mem.flags.unchanging) {
    return false;
  }
  return BaseMemDependence(read_mem, write_mem, facts, phase);
}

bool OutputDependence(const RtlExpr& w1_mem, const RtlExpr& w2_mem, const RegFacts& facts,
                      Phase phase) {
  // `sched.c:869-882`
  return BaseMemDependence(w1_mem, w2_mem, facts, phase);
}

// =============================================================================
// 4. Analyze (`sched.c:891-954, 1614-2273`)
// =============================================================================

namespace {

class DepAnalyzer {
 public:
  DepAnalyzer(absl::Span<const Insn> block, const RegFacts& facts, const MachineModel& mm,
              Phase phase, bool enable_pending_list_flush_cap = true)
      : block_(block),
        facts_(facts),
        mm_(mm),
        phase_(phase),
        enable_pending_list_flush_cap_(enable_pending_list_flush_cap),
        max_reg_(std::max(kMipsFirstPseudoRegister, facts.max_reg_num)),
        reg_last_uses_(static_cast<size_t>(max_reg_)),
        reg_last_sets_(static_cast<size_t>(max_reg_), -1) {
    graph_.log_links.resize(block.size());
    graph_.sched_group_p.assign(block.size(), false);
  }

  DepGraph Run() {
    const int n = static_cast<int>(block_.size());
    for (int idx = 0; idx < n; ++idx) {
      const Insn& insn = block_[static_cast<size_t>(idx)];
      const bool has_loop_notes = !insn.leading_loop_notes.empty();
      if (insn.kind == Insn::Kind::kInsn || insn.kind == Insn::Kind::kJump) {
        AnalyzeInsn(insn.pn.pattern, idx, has_loop_notes);
      } else {
        // `CALL_INSN` (`sched.c:2183-2263`):
        // 1. `SETJMP` or `call_used_regs` clobbers (`sched.c:2201-2234`).
        if (insn.setjmp_follows) {
          for (int r = 0; r < max_reg_; ++r) {
            for (int u : reg_last_uses_[static_cast<size_t>(r)]) {
              AddDependence(idx, u, DepKind::kAnti);
            }
            reg_last_uses_[static_cast<size_t>(r)].clear();
            if (reg_last_sets_[static_cast<size_t>(r)] >= 0) {
              // `sched.c:2211`: TRUE dependence `0`!
              AddDependence(idx, reg_last_sets_[static_cast<size_t>(r)], DepKind::kTrue);
            }
          }
          reg_pending_sets_all_ = true;
        } else {
          for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
            if (mm_.CallUsed(r)) {
              for (int u : reg_last_uses_[static_cast<size_t>(r)]) {
                AddDependence(idx, u, DepKind::kAnti);
              }
              reg_last_uses_[static_cast<size_t>(r)].clear();
              if (reg_last_sets_[static_cast<size_t>(r)] >= 0) {
                AddDependence(idx, reg_last_sets_[static_cast<size_t>(r)], DepKind::kAnti);
              }
              reg_pending_sets_.insert(r);
            }
          }
        }

        // 2. Flush `sched_before_next_call` (`sched.c:2238-2244`).
        for (int u : sched_before_next_call_) {
          AddDependence(idx, u, DepKind::kAnti);
        }
        sched_before_next_call_.clear();

        // 3. `sched_analyze_insn` BEFORE `last_function_call = insn` (`sched.c:2246`).
        AnalyzeInsn(insn.pn.pattern, idx, has_loop_notes);

        // 4. Flush pending memory if `!CONST_CALL_P(insn)` (`sched.c:2251-2257`).
        if (!insn.is_const_call) {
          FlushPendingLists(idx);
        }

        // 5. Update `last_function_call` (`sched.c:2261`).
        last_function_call_ = idx;
      }
    }
    return std::move(graph_);
  }

 private:
  void EnsureRegCapacity(int regno) {
    if (regno >= max_reg_) {
      max_reg_ = regno + 1;
      reg_last_uses_.resize(static_cast<size_t>(max_reg_));
      reg_last_sets_.resize(static_cast<size_t>(max_reg_), -1);
    }
  }

  bool HasLink(int insn_idx, int elem_idx) const {
    for (const Link& l : graph_.log_links[static_cast<size_t>(insn_idx)]) {
      if (l.target == elem_idx) {
        return true;
      }
    }
    return false;
  }

  void AddDependence(int insn_idx, int elem_idx, DepKind dep_type) {
    // Mirrors `sched.c:891-954`.
    if (insn_idx == elem_idx || elem_idx < 0) {
      return;
    }
    const int n = static_cast<int>(block_.size());
    int next = elem_idx + 1;
    if (next < n && graph_.sched_group_p[static_cast<size_t>(next)]) {
      while (next + 1 < n && graph_.sched_group_p[static_cast<size_t>(next + 1)]) {
        ++next;
      }
      if (insn_idx == next) {
        return;
      }
      elem_idx = next;
    }
    auto& links = graph_.log_links[static_cast<size_t>(insn_idx)];
    for (Link& l : links) {
      if (l.target == elem_idx) {
        if (static_cast<int>(dep_type) < static_cast<int>(l.kind)) {
          l.kind = dep_type;
        }
        return;
      }
    }
    // Exact GCC prepend order (`sched.c:948-953`).
    links.insert(links.begin(), Link{.target = elem_idx, .kind = dep_type});
  }

  void AddSchedBeforeNextCall(int elem_idx) {
    // Mirrors `add_dependence(sched_before_next_call, insn, REG_DEP_ANTI)` (prepend, dedupe).
    for (int u : sched_before_next_call_) {
      if (u == elem_idx) {
        return;
      }
    }
    sched_before_next_call_.insert(sched_before_next_call_.begin(), elem_idx);
  }

  void FlushPendingLists(int insn_idx) {
    // Mirrors `sched.c:1614-1653`.
    for (const auto& [u, _] : pending_reads_) {
      AddDependence(insn_idx, u, DepKind::kAnti);
    }
    pending_reads_.clear();
    for (const auto& [u, _] : pending_writes_) {
      AddDependence(insn_idx, u, DepKind::kAnti);
    }
    pending_writes_.clear();
    pending_lists_length_ = 0;
    if (last_pending_memory_flush_ >= 0) {
      AddDependence(insn_idx, last_pending_memory_flush_, DepKind::kAnti);
    }
    last_pending_memory_flush_ = insn_idx;
  }

  void Analyze1(const RtlExpr& x, int insn_idx) {
    // Mirrors `sched.c:1659-1792` (`sched_analyze_1`).
    if (x.children.empty()) {
      return;
    }
    const RtlExpr* dest = &x.children[0];
    while (dest->op == RtlOp::kStrictLowPart || dest->op == RtlOp::kSubreg ||
           dest->op == RtlOp::kZeroExtract || dest->op == RtlOp::kSignExtract) {
      if (dest->op == RtlOp::kZeroExtract || dest->op == RtlOp::kSignExtract) {
        if (dest->children.size() >= 2) {
          Analyze2(dest->children[1], insn_idx);
        }
        if (dest->children.size() >= 3) {
          Analyze2(dest->children[2], insn_idx);
        }
      }
      if (dest->children.empty()) {
        return;
      }
      dest = &dest->children[0];
    }

    if (dest->op == RtlOp::kReg) {
      const int regno = dest->RegNo();
      if (regno < 0) {
        return;
      }
      if (regno < kMipsFirstPseudoRegister) {
        int i = mm_.HardRegNRegs(regno, dest->mode);
        while (--i >= 0) {
          const int r = regno + i;
          EnsureRegCapacity(r);
          for (int u : reg_last_uses_[static_cast<size_t>(r)]) {
            AddDependence(insn_idx, u, DepKind::kAnti);
          }
          reg_last_uses_[static_cast<size_t>(r)].clear();
          if (reg_last_sets_[static_cast<size_t>(r)] >= 0) {
            AddDependence(insn_idx, reg_last_sets_[static_cast<size_t>(r)], DepKind::kOutput);
          }
          reg_pending_sets_.insert(r);
          // Reproduces exact GCC 2.7.2 bug at `sched.c:1704`: indexes `call_used_regs[i]`
          // by word offset `i` instead of `regno + i` (hitting `call_used_regs[0] == 1` on MIPS)!
          if (mm_.CallUsed(i) && last_function_call_ >= 0) {
            AddDependence(insn_idx, last_function_call_, DepKind::kAnti);
          }
        }
      } else {
        EnsureRegCapacity(regno);
        for (int u : reg_last_uses_[static_cast<size_t>(regno)]) {
          AddDependence(insn_idx, u, DepKind::kAnti);
        }
        reg_last_uses_[static_cast<size_t>(regno)].clear();
        if (reg_last_sets_[static_cast<size_t>(regno)] >= 0) {
          AddDependence(insn_idx, reg_last_sets_[static_cast<size_t>(regno)], DepKind::kOutput);
        }
        reg_pending_sets_.insert(regno);
        if (phase_ == Phase::kSched1 && facts_.reg_known_equiv_p.contains(regno)) {
          const auto kv = facts_.reg_known_value.find(regno);
          if (kv != facts_.reg_known_value.end() && kv->second.op == RtlOp::kMem &&
              !kv->second.children.empty()) {
            Analyze2(kv->second.children[0], insn_idx);
          }
        }
        const auto cc_it = facts_.reg_n_calls_crossed.find(regno);
        const int calls_crossed = (cc_it != facts_.reg_n_calls_crossed.end()) ? cc_it->second : 0;
        if (calls_crossed == 0 && last_function_call_ >= 0) {
          AddDependence(insn_idx, last_function_call_, DepKind::kAnti);
        }
      }
    } else if (dest->op == RtlOp::kMem) {
      if (enable_pending_list_flush_cap_ && pending_lists_length_ > 32) {
        FlushPendingLists(insn_idx);
      } else {
        for (const auto& [u, pmem] : pending_reads_) {
          if (!HasLink(insn_idx, u) && AntiDependence(pmem, *dest, facts_, phase_)) {
            AddDependence(insn_idx, u, DepKind::kAnti);
          }
        }
        for (const auto& [u, pmem] : pending_writes_) {
          if (!HasLink(insn_idx, u) && OutputDependence(pmem, *dest, facts_, phase_)) {
            AddDependence(insn_idx, u, DepKind::kOutput);
          }
        }
        if (last_pending_memory_flush_ >= 0) {
          AddDependence(insn_idx, last_pending_memory_flush_, DepKind::kAnti);
        }
        pending_writes_.insert(pending_writes_.begin(), {insn_idx, *dest});
        ++pending_lists_length_;
      }
      if (!dest->children.empty()) {
        Analyze2(dest->children[0], insn_idx);
      }
    }

    if (x.op == RtlOp::kSet && x.children.size() >= 2) {
      Analyze2(x.SetSrc(), insn_idx);
    }
  }

  void Analyze2(const RtlExpr& x, int insn_idx) {
    // Mirrors `sched.c:1797-2012` (`sched_analyze_2`).
    switch (x.op) {
      case RtlOp::kConstInt:
      case RtlOp::kConstDouble:
      case RtlOp::kSymbolRef:
      case RtlOp::kConst:
      case RtlOp::kLabelRef:
        return;
      case RtlOp::kReg: {
        const int regno = x.RegNo();
        if (regno < 0) {
          return;
        }
        if (regno < kMipsFirstPseudoRegister) {
          int i = mm_.HardRegNRegs(regno, x.mode);
          while (--i >= 0) {
            const int r = regno + i;
            EnsureRegCapacity(r);
            reg_last_uses_[static_cast<size_t>(r)].insert(
                reg_last_uses_[static_cast<size_t>(r)].begin(), insn_idx);
            if (reg_last_sets_[static_cast<size_t>(r)] >= 0) {
              AddDependence(insn_idx, reg_last_sets_[static_cast<size_t>(r)], DepKind::kTrue);
            }
            if (mm_.CallUsed(r) && last_function_call_ >= 0) {
              AddDependence(insn_idx, last_function_call_, DepKind::kAnti);
            }
          }
        } else {
          EnsureRegCapacity(regno);
          reg_last_uses_[static_cast<size_t>(regno)].insert(
              reg_last_uses_[static_cast<size_t>(regno)].begin(), insn_idx);
          if (reg_last_sets_[static_cast<size_t>(regno)] >= 0) {
            AddDependence(insn_idx, reg_last_sets_[static_cast<size_t>(regno)], DepKind::kTrue);
          }
          if (phase_ == Phase::kSched1 && facts_.reg_known_equiv_p.contains(regno)) {
            const auto kv = facts_.reg_known_value.find(regno);
            if (kv != facts_.reg_known_value.end() && kv->second.op == RtlOp::kMem &&
                !kv->second.children.empty()) {
              Analyze2(kv->second.children[0], insn_idx);
            }
          }
          const auto cc_it = facts_.reg_n_calls_crossed.find(regno);
          const int calls_crossed = (cc_it != facts_.reg_n_calls_crossed.end()) ? cc_it->second : 0;
          if (calls_crossed == 0) {
            AddSchedBeforeNextCall(insn_idx);
          }
        }
        return;
      }
      case RtlOp::kMem: {
        for (const auto& [u, pmem] : pending_reads_) {
          if (!HasLink(insn_idx, u) && ReadDependence(pmem, x)) {
            AddDependence(insn_idx, u, DepKind::kAnti);
          }
        }
        for (const auto& [u, pmem] : pending_writes_) {
          if (!HasLink(insn_idx, u) && TrueDependence(pmem, x, facts_, phase_)) {
            AddDependence(insn_idx, u, DepKind::kTrue);
          }
        }
        if (last_pending_memory_flush_ >= 0) {
          AddDependence(insn_idx, last_pending_memory_flush_, DepKind::kAnti);
        }
        pending_reads_.insert(pending_reads_.begin(), {insn_idx, x});
        ++pending_lists_length_;
        if (!x.children.empty()) {
          Analyze2(x.children[0], insn_idx);
        }
        return;
      }
      case RtlOp::kAsmOperands:
      case RtlOp::kAsmInput:
      case RtlOp::kUnspecVolatile:
        if (x.op != RtlOp::kAsmOperands || x.flags.volatil) {
          ApplyFullBarrier(insn_idx);
        }
        if (x.op == RtlOp::kAsmOperands) {
          // `sched.c:1978-1983`: traverse input operands for ALL `ASM_OPERANDS` (including
          // non-volatile ones), skipping `ASM_INPUT` constraints vector!
          for (const RtlExpr& ch : x.children) {
            if (ch.op != RtlOp::kAsmInput) {
              Analyze2(ch, insn_idx);
            }
          }
          return;
        }
        break;
      default:
        if (IsTrapIfExpr(x)) {
          ApplyFullBarrier(insn_idx);
        }
        break;
    }

    if (IsVectorFormattedOp(x.op)) {
      for (const RtlExpr& ch : x.children) {
        Analyze2(ch, insn_idx);
      }
    } else {
      for (size_t i = x.children.size(); i > 0; --i) {
        Analyze2(x.children[i - 1], insn_idx);
      }
    }
  }

  void ApplyFullBarrier(int insn_idx) {
    // Mirrors `sched.c:1959-1970, 2060-2074`.
    for (int r = 0; r < max_reg_; ++r) {
      for (int u : reg_last_uses_[static_cast<size_t>(r)]) {
        AddDependence(insn_idx, u, DepKind::kAnti);
      }
      reg_last_uses_[static_cast<size_t>(r)].clear();
      if (reg_last_sets_[static_cast<size_t>(r)] >= 0) {
        AddDependence(insn_idx, reg_last_sets_[static_cast<size_t>(r)], DepKind::kTrue);
      }
    }
    reg_pending_sets_all_ = true;
    FlushPendingLists(insn_idx);
  }

  void AnalyzeInsn(const RtlExpr& pat, int insn_idx, bool has_loop_notes) {
    // Mirrors `sched.c:2017-2158` (`sched_analyze_insn`).
    if (pat.op == RtlOp::kSet || pat.op == RtlOp::kClobber) {
      Analyze1(pat, insn_idx);
    } else if (pat.op == RtlOp::kParallel) {
      for (size_t i = pat.children.size(); i > 0; --i) {
        const RtlExpr& ch = pat.children[i - 1];
        if (ch.op == RtlOp::kSet || ch.op == RtlOp::kClobber) {
          Analyze1(ch, insn_idx);
        } else {
          Analyze2(ch, insn_idx);
        }
      }
    } else {
      Analyze2(pat, insn_idx);
    }

    const Insn& insn = block_[static_cast<size_t>(insn_idx)];
    // `CALL_INSN_FUNCTION_USAGE` (`sched.c:2044-2051`) runs BEFORE committing `reg_pending_sets`!
    if (insn.kind == Insn::Kind::kCall) {
      for (const RtlExpr& fu : insn.pn.call_fusage) {
        if (fu.op == RtlOp::kClobber) {
          Analyze1(fu, insn_idx);
        } else {
          Analyze2(fu, insn_idx);
        }
      }
    }

    if (has_loop_notes) {
      ApplyFullBarrier(insn_idx);
    }

    // Post-reload `REG_DEAD` analysis (`sched.c:2095-2102`).
    if (phase_ == Phase::kSched2) {
      for (const RtlRegNote& note : insn.pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead) {
          Analyze2(note.datum, insn_idx);
        }
      }
    }

    // Commit `reg_pending_sets` (`sched.c:2104-2121`).
    for (int r : reg_pending_sets_) {
      EnsureRegCapacity(r);
      reg_last_sets_[static_cast<size_t>(r)] = insn_idx;
    }
    reg_pending_sets_.clear();
    if (reg_pending_sets_all_) {
      for (int r = 0; r < max_reg_; ++r) {
        reg_last_sets_[static_cast<size_t>(r)] = insn_idx;
      }
      reg_pending_sets_all_ = false;
    }

    // `SCHED_GROUP_P` for `(use (reg ...))` preceding `CALL_INSN`/`JUMP_INSN`
    // (`sched.c:2125-2157`).
    if (insn.kind == Insn::Kind::kCall || insn.kind == Insn::Kind::kJump) {
      int prev_dep = insn_idx;
      int dep = insn_idx - 1;
      while (dep >= 0 && IsUseOfRegInsn(block_[static_cast<size_t>(dep)])) {
        graph_.sched_group_p[static_cast<size_t>(prev_dep)] = true;
        // Copy links in forward traversal order of `LOG_LINKS(dep)`.
        const std::vector<Link> dep_links = graph_.log_links[static_cast<size_t>(dep)];
        for (const Link& l : dep_links) {
          AddDependence(insn_idx, l.target, l.kind);
        }
        prev_dep = dep;
        --dep;
      }
    }
  }

  absl::Span<const Insn> block_;
  const RegFacts& facts_;
  const MachineModel& mm_;
  Phase phase_;
  bool enable_pending_list_flush_cap_ = true;
  int max_reg_ = kMipsFirstPseudoRegister;
  std::vector<std::vector<int>> reg_last_uses_;
  std::vector<int> reg_last_sets_;
  absl::btree_set<int> reg_pending_sets_;
  bool reg_pending_sets_all_ = false;
  std::vector<std::pair<int, RtlExpr>> pending_reads_;
  std::vector<std::pair<int, RtlExpr>> pending_writes_;
  int pending_lists_length_ = 0;
  int last_pending_memory_flush_ = -1;
  int last_function_call_ = -1;
  std::vector<int> sched_before_next_call_;
  DepGraph graph_;
};

}  // namespace

namespace detail {

// Computes the semantic dependency graph without GCC 2.7.2's position-dependent
// `pending_lists_length > 32` cap (`sched.c:1740`), preserving all register,
// true/anti/output memory, call-barrier, and volatile-barrier dependencies.
// Used solely during inverse search to distinguish genuine dependencies from
// `sched2`-order-dependent pending-list-flush artifacts; forward verification
// always uses exact `Analyze()`.
DepGraph AnalyzeWithoutPendingListFlushCap(absl::Span<const Insn> block, const RegFacts& facts,
                                           const MachineModel& mm, Phase phase) {
  return DepAnalyzer(block, facts, mm, phase, /*enable_pending_list_flush_cap=*/false).Run();
}

}  // namespace detail

DepGraph Analyze(absl::Span<const Insn> block, const RegFacts& facts, const MachineModel& mm,
                 Phase phase) {
  return DepAnalyzer(block, facts, mm, phase).Run();
}

// =============================================================================
// 5. Schedule (`sched.c:1100-1500, 2373-2704, 3144-3940`)
// =============================================================================

namespace {

class ListScheduler {
 public:
  ListScheduler(absl::Span<const Insn> block, const DepGraph& graph, const RegFacts& facts,
                const MachineModel& mm, Phase phase, const absl::btree_set<int>& live_at_end)
      : block_(block),
        log_links_(graph.log_links),
        sched_group_p_(graph.sched_group_p),
        facts_(facts),
        mm_(mm),
        phase_(phase),
        n_(static_cast<int>(block.size())),
        insn_priority_(static_cast<size_t>(n_), 0),
        insn_ref_count_(static_cast<size_t>(n_), 0),
        insn_tick_(static_cast<size_t>(n_), 0),
        insn_cost_cache_(static_cast<size_t>(n_), 0),
        insn_unit_use_(static_cast<size_t>(n_)),
        insn_recognized_(static_cast<size_t>(n_), false),
        bb_live_regs_(live_at_end) {
    for (int i = 0; i < n_; ++i) {
      const auto& pn = block_[static_cast<size_t>(i)].pn;
      insn_recognized_[static_cast<size_t>(i)] = mm_.IsRecognized(pn);
      if (insn_recognized_[static_cast<size_t>(i)]) {
        insn_unit_use_[static_cast<size_t>(i)] =
            mm_.UnitOf(mm_.TypeOf(pn, phase_), mm_.AttrModeOf(pn));
      }
    }
  }

  // Tail pinning & priority computation (`sched.c:3293-3377`). Returns `last_tail`.
  int InitTailPinningAndPriorities(std::vector<int>* ready) {
    ready->reserve(static_cast<size_t>(n_));
    int last_tail = -1;
    int idx = n_ - 1;
    while (idx >= 0 && IsTailPinnedInsn(block_[static_cast<size_t>(idx)])) {
      ComputePriority(idx);
      if (last_tail < 0) {
        ready->push_back(idx);
        insn_priority_[static_cast<size_t>(idx)] = kTailPriority - facts_.max_reg_num;
        insn_ref_count_[static_cast<size_t>(idx)] = 0;
      } else if (!HasLink(last_tail, idx)) {
        AddLink(last_tail, idx, DepKind::kAnti);
        insn_ref_count_[static_cast<size_t>(idx)] += 1;
      }
      last_tail = idx;
      while (idx > 0 && sched_group_p_[static_cast<size_t>(idx)]) {
        --idx;
        ComputePriority(idx);
      }
      --idx;
    }
    for (; idx >= 0; --idx) {
      ComputePriority(idx);
      if (insn_ref_count_[static_cast<size_t>(idx)] == 0) {
        if (last_tail < 0) {
          ready->push_back(idx);
        } else {
          AddLink(last_tail, idx, DepKind::kAnti);
          insn_ref_count_[static_cast<size_t>(idx)] = 1;
        }
      }
      if (sched_group_p_[static_cast<size_t>(idx)]) {
        while (idx > 0 && sched_group_p_[static_cast<size_t>(idx)]) {
          --idx;
          ComputePriority(idx);
        }
      }
    }
    return last_tail;
  }

  // Advances `clock` and `q_ptr`, popping LIFO `insn_queue` buckets (`sched.c:3678-3686`).
  bool AdvanceClockAndPopQueue(std::vector<int>* ready, int* clock, int* q_ptr) {
    *q_ptr = (*q_ptr + 1) & (kInsnQueueSize - 1);
    ++(*clock);

    for (int q_insn : insn_queue_[static_cast<size_t>(*q_ptr)]) {
      ready->push_back(q_insn);
    }
    insn_queue_[static_cast<size_t>(*q_ptr)].clear();

    if (ready->empty()) {
      int stalls = 1;
      for (; stalls < kInsnQueueSize; ++stalls) {
        const int bucket = (*q_ptr + stalls) & (kInsnQueueSize - 1);
        if (!insn_queue_[static_cast<size_t>(bucket)].empty()) {
          for (int q_insn : insn_queue_[static_cast<size_t>(bucket)]) {
            ready->push_back(q_insn);
          }
          insn_queue_[static_cast<size_t>(bucket)].clear();
          break;
        }
      }
      *q_ptr = (*q_ptr + stalls) & (kInsnQueueSize - 1);
      *clock += stalls;
    }
    return !ready->empty();
  }

  // Updates backward `bb_live_regs` for `birthing_insn_p` across `chosen` and its
  // `SCHED_GROUP_P` chain (`sched.c:3764-3882`).
  void UpdateBbLiveRegsForGroup(int chosen) {
    if (phase_ != Phase::kSched1) {
      return;
    }
    int cur = chosen;
    while (true) {
      UpdateBbLiveRegsBackward(cur);
      if (!sched_group_p_[static_cast<size_t>(cur)] || cur == 0) {
        break;
      }
      --cur;
    }
  }

  ScheduleResult Run() {
    ScheduleResult res;
    if (n_ == 0) {
      return res;
    }

    std::vector<int> ready;
    InitTailPinningAndPriorities(&ready);

    // Initial `SCHED_SORT(ready, n_ready, 1)` (`sched.c:3637`).
    SchedSort(&ready, /*old_ready=*/1);

    // Main reverse list-scheduling loop (`sched.c:3658-3940`).
    int q_ptr = 0;
    int clock = 0;
    int sched_n_insns = 0;
    int n_ready = static_cast<int>(ready.size());
    std::vector<int> reverse_order;
    reverse_order.reserve(static_cast<size_t>(n_));

    while (sched_n_insns < n_) {
      if (!AdvanceClockAndPopQueue(&ready, &clock, &q_ptr)) {
        break;
      }

      SchedSort(&ready, n_ready);
      StepTrace step;
      step.clock = clock;
      step.ready_before_select = ready;
      step.priorities.reserve(ready.size());
      for (int r : ready) {
        step.priorities.push_back(insn_priority_[static_cast<size_t>(r)]);
      }

      if (!ScheduleSelect(&ready, clock, q_ptr)) {
        n_ready = 0;
        continue;
      }

      n_ready = static_cast<int>(ready.size());
      const int chosen = ready[0];
      last_scheduled_insn_ = chosen;
      step.chosen = chosen;
      res.trace.push_back(std::move(step));

      UpdateBbLiveRegsForGroup(chosen);

      // Pop `ready[0]` (`sched.c:3885-3891`).
      ready.erase(ready.begin());
      --n_ready;
      ++sched_n_insns;
      reverse_order.push_back(chosen);

      // Launch predecessors (`sched.c:3902-3904`).
      insn_priority_[static_cast<size_t>(chosen)] = kLaunchPriority;
      ScheduleInsn(chosen, &ready, clock, q_ptr);
      insn_priority_[static_cast<size_t>(chosen)] = kDonePriority;

      // Schedule `SCHED_GROUP_P` chain atomically (`sched.c:3907-3936`).
      if (sched_group_p_[static_cast<size_t>(chosen)]) {
        int link = chosen;
        while (sched_group_p_[static_cast<size_t>(link)] && link > 0) {
          --link;
          insn_ref_count_[static_cast<size_t>(link)] = 0;
        }
        int g = chosen;
        while (sched_group_p_[static_cast<size_t>(g)] && g > 0) {
          --g;
          ++sched_n_insns;
          reverse_order.push_back(g);
          // `sched.c:3932`: intentionally does NOT set `LAUNCH_PRIORITY`!
          ScheduleInsn(g, &ready, clock, q_ptr);
          insn_priority_[static_cast<size_t>(g)] = kDonePriority;
        }
      }
    }

    res.order.assign(reverse_order.rbegin(), reverse_order.rend());
    return res;
  }

  int Size() const { return n_; }
  int Priority(int idx) const { return insn_priority_[static_cast<size_t>(idx)]; }
  void SetPriority(int idx, int pri) { insn_priority_[static_cast<size_t>(idx)] = pri; }
  void SetRefCount(int idx, int ref_count) {
    insn_ref_count_[static_cast<size_t>(idx)] = ref_count;
  }
  const std::vector<Link>& LogLinks(int idx) const { return log_links_[static_cast<size_t>(idx)]; }
  bool SchedGroupP(int idx) const { return sched_group_p_[static_cast<size_t>(idx)]; }
  int LastScheduledInsn() const { return last_scheduled_insn_; }
  void SetLastScheduledInsn(int idx) { last_scheduled_insn_ = idx; }

  int InsnCost(int prod, DepKind kind, int cons) {
    // Mirrors `sched.c:1363-1420` + `mips.h:2944-2946` (`ADJUST_COST`).
    int& cached = insn_cost_cache_[static_cast<size_t>(prod)];
    if (cached == 0) {
      if (!insn_recognized_[static_cast<size_t>(prod)]) {
        cached = 1;
      } else {
        cached = std::max(1, insn_unit_use_[static_cast<size_t>(prod)].ready);
      }
    }
    if (!insn_recognized_[static_cast<size_t>(cons)]) {
      return 1;
    }
    if (kind != DepKind::kTrue) {
      return 1;  // `ADJUST_COST` sets anti/output cost to 0 -> clamped to `LINK_COST_FREE` (1).
    }
    return cached;
  }

  void QueueInsn(int insn_idx, int n_cycles, int q_ptr) {
    // LIFO prepend (`sched.c:2455-2463`).
    const int next_q = (q_ptr + n_cycles) & (kInsnQueueSize - 1);
    auto& bucket = insn_queue_[static_cast<size_t>(next_q)];
    bucket.insert(bucket.begin(), insn_idx);
  }

  int ActualHazard(int insn_idx, int clock) const {
    // Mirrors `sched.c:1200-1231, 1269-1308`.
    const UnitUse& u = insn_unit_use_[static_cast<size_t>(insn_idx)];
    if (u.unit < 0) {
      return 0;
    }
    int tick = unit_tick_[static_cast<size_t>(u.unit)];
    if (tick - clock > 0) {
      const int last_insn = unit_last_insn_[static_cast<size_t>(u.unit)];
      const int blk = mm_.Blockage(u, insn_unit_use_[static_cast<size_t>(last_insn)]);
      tick += blk - mm_.MaxBlockage(u.unit);
      if (tick - clock > 0) {
        return tick - clock;
      }
    }
    return 0;
  }

  int PotentialHazard(int insn_idx) const {
    // Mirrors `sched.c:1318-1356`.
    const UnitUse& u = insn_unit_use_[static_cast<size_t>(insn_idx)];
    if (u.unit < 0 || mm_.MaxBlockage(u.unit) <= 1 || u.max_blockage <= 1) {
      return 0;
    }
    const int ncost = (u.min_blockage * 0x40 + u.max_blockage) *
                      ((unit_n_insns_[static_cast<size_t>(u.unit)] - 1) * 0x1000 + u.unit);
    return std::max(0, ncost);
  }

  void ScheduleInsn(int insn_idx, std::vector<int>* ready, int clock, int q_ptr) {
    // Mirrors `sched.c:2557-2609`.
    const UnitUse& u = insn_unit_use_[static_cast<size_t>(insn_idx)];
    if (u.unit >= 0) {
      unit_last_insn_[static_cast<size_t>(u.unit)] = insn_idx;
      unit_tick_[static_cast<size_t>(u.unit)] = clock + mm_.MaxBlockage(u.unit);
    }
    const auto& links = log_links_[static_cast<size_t>(insn_idx)];
    if (links.empty()) {
      return;
    }
    int max_pri = insn_priority_[static_cast<size_t>(insn_idx)];
    for (int r : *ready) {
      max_pri = std::max(max_pri, insn_priority_[static_cast<size_t>(r)]);
    }

    for (const Link& l : links) {
      const int prev = l.target;
      int cost = InsnCost(prev, l.kind, insn_idx);
      insn_ref_count_[static_cast<size_t>(prev)] -= 1;
      if (insn_ref_count_[static_cast<size_t>(prev)] != 0) {
        if (cost > 1) {
          insn_tick_[static_cast<size_t>(prev)] =
              std::max(insn_tick_[static_cast<size_t>(prev)], clock + cost);
        }
      } else {
        if (insn_tick_[static_cast<size_t>(prev)] - clock > cost) {
          cost = insn_tick_[static_cast<size_t>(prev)] - clock;
        }
        // `adjust_priority` (`sched.c:2507-2549`): `REG_DEAD` notes were stripped before
        // scheduling, so only the `birthing_insn_p` promotion applies.
        if (phase_ == Phase::kSched1 && BirthingInsnP(block_[static_cast<size_t>(prev)].pn.pattern,
                                                      bb_live_regs_, facts_, phase_)) {
          if (max_pri > insn_priority_[static_cast<size_t>(prev)]) {
            insn_priority_[static_cast<size_t>(prev)] = max_pri;
          }
        }
        if (cost <= 1) {
          ready->push_back(prev);
        } else {
          QueueInsn(prev, cost, q_ptr);
        }
      }
    }
  }

 private:
  bool HasLink(int insn_idx, int elem_idx) const {
    for (const Link& l : log_links_[static_cast<size_t>(insn_idx)]) {
      if (l.target == elem_idx) {
        return true;
      }
    }
    return false;
  }

  void AddLink(int insn_idx, int elem_idx, DepKind kind) {
    log_links_[static_cast<size_t>(insn_idx)].insert(
        log_links_[static_cast<size_t>(insn_idx)].begin(), Link{.target = elem_idx, .kind = kind});
  }

  int ComputePriority(int insn_idx) {
    // Mirrors `sched.c:1424-1500`.
    if (insn_priority_[static_cast<size_t>(insn_idx)] > 0) {
      return insn_priority_[static_cast<size_t>(insn_idx)];
    }
    int max_pri = 1;
    if (sched_group_p_[static_cast<size_t>(insn_idx)]) {
      int prev = insn_idx;
      while (sched_group_p_[static_cast<size_t>(prev)] && prev > 0) {
        --prev;
        insn_ref_count_[static_cast<size_t>(prev)] += 1;
      }
    }
    for (const Link& l : log_links_[static_cast<size_t>(insn_idx)]) {
      const int prev_pri = ComputePriority(l.target) + InsnCost(l.target, l.kind, insn_idx) - 1;
      if (prev_pri > max_pri) {
        max_pri = prev_pri;
      }
      insn_ref_count_[static_cast<size_t>(l.target)] += 1;
    }
    const int unit = insn_unit_use_[static_cast<size_t>(insn_idx)].unit;
    if (unit >= 0 && unit < kNumFunctionUnits) {
      unit_n_insns_[static_cast<size_t>(unit)] += 1;
    }
    insn_priority_[static_cast<size_t>(insn_idx)] = max_pri;
    return max_pri;
  }

  // Mirrors `sched.c:2385-2429`: > 0 means `y` preferred over `x`; < 0 means `x` preferred.
  int RankForSchedule(int x, int y) {
    const int pri_diff =
        insn_priority_[static_cast<size_t>(y)] - insn_priority_[static_cast<size_t>(x)];
    if (pri_diff != 0) {
      return pri_diff;
    }
    if (last_scheduled_insn_ >= 0) {
      auto dep_class = [&](int cand) -> int {
        for (const Link& l : log_links_[static_cast<size_t>(last_scheduled_insn_)]) {
          if (l.target == cand) {
            if (InsnCost(cand, l.kind, last_scheduled_insn_) == 1) {
              return 3;
            }
            return (l.kind == DepKind::kTrue) ? 1 : 2;
          }
        }
        return 3;
      };
      const int cls_diff = dep_class(y) - dep_class(x);
      if (cls_diff != 0) {
        return cls_diff;
      }
    }
    return block_[static_cast<size_t>(y)].luid - block_[static_cast<size_t>(x)].luid;
  }

  void SchedSort(std::vector<int>* ready, int old_ready) {
    // Exact `SCHED_SORT` (`sched.c:2373-2378, 2434-2447`).
    const int new_ready = static_cast<int>(ready->size());
    if (new_ready - old_ready == 1 && new_ready >= 2) {
      const int insn = (*ready)[static_cast<size_t>(new_ready - 1)];
      int i = new_ready - 2;
      while (i >= 0 && RankForSchedule((*ready)[static_cast<size_t>(i)], insn) >= 0) {
        (*ready)[static_cast<size_t>(i + 1)] = (*ready)[static_cast<size_t>(i)];
        --i;
      }
      (*ready)[static_cast<size_t>(i + 1)] = insn;
    } else if (new_ready - old_ready > 1) {
      std::sort(ready->begin(), ready->end(),
                [this](int a, int b) { return RankForSchedule(a, b) < 0; });
    }
  }

  bool ScheduleSelect(std::vector<int>* ready, int clock, int q_ptr) {
    // Mirrors `sched.c:2616-2704`.
    const int n_ready = static_cast<int>(ready->size());
    if (n_ready == 0) {
      return false;
    }
    int pri = insn_priority_[static_cast<size_t>((*ready)[0])];
    int best_insn = 0;
    int new_ready = n_ready;

    for (int i = 0; i < n_ready;) {
      const int opri = pri;
      int j = i + 1;
      for (; j < n_ready; ++j) {
        pri = insn_priority_[static_cast<size_t>((*ready)[static_cast<size_t>(j)])];
        if (pri != opri) {
          break;
        }
      }
      int q = 0;
      for (int k = i; k < j; ++k) {
        const int insn_idx = (*ready)[static_cast<size_t>(k)];
        const int cost = ActualHazard(insn_idx, clock);
        if (cost != 0) {
          ++q;
          (*ready)[static_cast<size_t>(k)] = -1;
          QueueInsn(insn_idx, cost, q_ptr);
        }
      }
      new_ready -= q;
      if (j - i - q == 0) {
        i = j;
        continue;
      }
      if (j - i - q > 1) {
        int best_cost = -1;
        for (int k = i; k < j; ++k) {
          const int insn_idx = (*ready)[static_cast<size_t>(k)];
          if (insn_idx < 0) {
            continue;
          }
          const int cost = PotentialHazard(insn_idx);
          if (cost > best_cost) {
            best_cost = cost;
            best_insn = k;
          }
        }
      }
      break;
    }

    if (best_insn != 0) {
      for (int i = best_insn; i > 0; --i) {
        std::swap((*ready)[static_cast<size_t>(i - 1)], (*ready)[static_cast<size_t>(i)]);
      }
    }
    if (new_ready < n_ready) {
      std::vector<int> compacted;
      compacted.reserve(static_cast<size_t>(new_ready));
      for (int insn_idx : *ready) {
        if (insn_idx >= 0) {
          compacted.push_back(insn_idx);
        }
      }
      *ready = std::move(compacted);
    }
    return !ready->empty();
  }

  void UpdateBbLiveRegsBackward(int insn_idx) {
    const Insn& insn = block_[static_cast<size_t>(insn_idx)];
    const RtlExpr& pat = insn.pn.pattern;
    if (pat.op == RtlOp::kSet || pat.op == RtlOp::kClobber) {
      SchedNoteSet(pat, /*death=*/true, mm_, &bb_live_regs_);
    } else if (pat.op == RtlOp::kParallel) {
      for (size_t j = pat.children.size(); j > 0; --j) {
        const RtlExpr& ch = pat.children[j - 1];
        if (ch.op == RtlOp::kSet || ch.op == RtlOp::kClobber) {
          SchedNoteSet(ch, /*death=*/true, mm_, &bb_live_regs_);
        }
      }
    }
    if (insn.kind == Insn::Kind::kCall) {
      for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
        if (mm_.CallUsed(r)) {
          bb_live_regs_.erase(r);
        }
      }
    }
    const absl::btree_set<int> old_live = bb_live_regs_;
    AttachDeathsInsn(insn, mm_, old_live, &bb_live_regs_, /*out_notes=*/nullptr);
  }

  absl::Span<const Insn> block_;
  std::vector<std::vector<Link>> log_links_;
  std::vector<bool> sched_group_p_;
  const RegFacts& facts_;
  const MachineModel& mm_;
  Phase phase_;
  int n_ = 0;
  std::vector<int> insn_priority_;
  std::vector<int> insn_ref_count_;
  std::vector<int> insn_tick_;
  std::vector<int> insn_cost_cache_;
  std::vector<UnitUse> insn_unit_use_;
  std::vector<bool> insn_recognized_;
  std::array<int, kNumFunctionUnits> unit_last_insn_ = {-1, -1, -1, -1, -1};
  std::array<int, kNumFunctionUnits> unit_tick_ = {0, 0, 0, 0, 0};
  std::array<int, kNumFunctionUnits> unit_n_insns_ = {0, 0, 0, 0, 0};
  std::array<std::vector<int>, kInsnQueueSize> insn_queue_;
  int last_scheduled_insn_ = -1;
  absl::btree_set<int> bb_live_regs_;
};

// Phase 2 of `InvertBlockOrder`: forced reverse replay tracking `last_cmp[u][v]`
// across `qsort` (>1), `swap_sort` (==1), and no-sort (==0) steps (`sched.c:2373-2378,
// 2425-2447, 2616-2704`).
class ForcedReplayTracker {
 public:
  ForcedReplayTracker(absl::Span<const Insn> block, const DepGraph& graph, const RegFacts& facts,
                      const MachineModel& mm, Phase phase, const absl::btree_set<int>& live_at_end)
      : sched_(block, graph, facts, mm, phase, live_at_end),
        n_(sched_.Size()),
        last_cmp_(static_cast<size_t>(n_) * static_cast<size_t>(n_)) {}

  ForcedReplayResult Run(bool continue_on_conflict) {
    ForcedReplayResult res;
    if (n_ == 0) {
      return res;
    }

    std::vector<int> ready;
    res.last_tail = sched_.InitTailPinningAndPriorities(&ready);

    // Initial `SCHED_SORT(ready, n_ready, 1)` (`sched.c:3637`).
    RecordSchedSort(ready, /*old_ready=*/1, /*last_sched=*/-1);

    int q_ptr = 0;
    int clock = 0;
    int n_ready = static_cast<int>(ready.size());
    int cursor = n_ - 1;
    std::vector<bool> scheduled(static_cast<size_t>(n_), false);

    while (cursor >= 0) {
      while (cursor >= 0 && scheduled[static_cast<size_t>(cursor)]) {
        --cursor;
      }
      if (cursor < 0) {
        break;
      }
      if (!sched_.AdvanceClockAndPopQueue(&ready, &clock, &q_ptr)) {
        res.had_conflict = true;
        break;
      }

      RecordSchedSort(ready, n_ready, sched_.LastScheduledInsn());

      // Simulate `ScheduleSelect` priority groups & `ActualHazard` queueing (`sched.c:2629-2703`).
      std::vector<int> winning_group;
      if (!SelectWinningGroup(&ready, clock, q_ptr, &winning_group)) {
        n_ready = 0;
        continue;
      }

      const int target = cursor;
      const int chosen = ChooseReplayInsn(winning_group, target, continue_on_conflict, &res);
      if (chosen < 0) {
        break;
      }

      sched_.SetLastScheduledInsn(chosen);
      sched_.UpdateBbLiveRegsForGroup(chosen);

      ready.erase(std::remove(ready.begin(), ready.end(), chosen), ready.end());
      n_ready = static_cast<int>(ready.size());
      scheduled[static_cast<size_t>(chosen)] = true;
      if (chosen == cursor) {
        --cursor;
      }

      sched_.SetPriority(chosen, kLaunchPriority);
      sched_.ScheduleInsn(chosen, &ready, clock, q_ptr);
      sched_.SetPriority(chosen, kDonePriority);

      if (sched_.SchedGroupP(chosen)) {
        int link = chosen;
        while (sched_.SchedGroupP(link) && link > 0) {
          --link;
          sched_.SetRefCount(link, 0);
        }
        int g = chosen;
        while (sched_.SchedGroupP(g) && g > 0) {
          --g;
          if (!continue_on_conflict && cursor != g) {
            res.had_conflict = true;
            return res;
          }
          scheduled[static_cast<size_t>(g)] = true;
          if (cursor == g) {
            --cursor;
          }
          sched_.ScheduleInsn(g, &ready, clock, q_ptr);
          sched_.SetPriority(g, kDonePriority);
        }
      }
    }
    return res;
  }

 private:
  struct CmpRecord {
    bool valid = false;
    int pri_u = 0;
    int pri_v = 0;
    int cls_u = 3;
    int cls_v = 3;
  };

  CmpRecord& CmpAt(int u, int v) {
    return last_cmp_[static_cast<size_t>(u) * static_cast<size_t>(n_) + static_cast<size_t>(v)];
  }

  int DepClass(int cand, int last_sched) {
    if (last_sched < 0) {
      return 3;
    }
    for (const Link& l : sched_.LogLinks(last_sched)) {
      if (l.target == cand) {
        if (sched_.InsnCost(cand, l.kind, last_sched) == 1) {
          return 3;
        }
        return (l.kind == DepKind::kTrue) ? 1 : 2;
      }
    }
    return 3;
  }

  void RecordPair(int u, int v, int last_sched) {
    const int pu = sched_.Priority(u);
    const int pv = sched_.Priority(v);
    const int cu = DepClass(u, last_sched);
    const int cv = DepClass(v, last_sched);
    CmpAt(u, v) = CmpRecord{.valid = true, .pri_u = pu, .pri_v = pv, .cls_u = cu, .cls_v = cv};
    CmpAt(v, u) = CmpRecord{.valid = true, .pri_u = pv, .pri_v = pu, .cls_u = cv, .cls_v = cu};
  }

  void RecordSchedSort(const std::vector<int>& r, int old_ready, int last_sched) {
    const int new_ready = static_cast<int>(r.size());
    if (new_ready - old_ready == 1 && new_ready >= 2) {
      // `swap_sort` (`sched.c:2434-2447`): only the newly appended element is compared
      // against existing ready elements; prior relative comparisons remain intact!
      const int w = r[static_cast<size_t>(new_ready - 1)];
      for (int i = 0; i < new_ready - 1; ++i) {
        RecordPair(w, r[static_cast<size_t>(i)], last_sched);
      }
    } else if (new_ready - old_ready > 1) {
      // `qsort` (`sched.c:2377`): all pairs in `ready` are re-compared under `last_sched`.
      for (int i = 0; i < new_ready; ++i) {
        for (int j = i + 1; j < new_ready; ++j) {
          RecordPair(r[static_cast<size_t>(i)], r[static_cast<size_t>(j)], last_sched);
        }
      }
    }
  }

  bool SelectWinningGroup(std::vector<int>* ready, int clock, int q_ptr,
                          std::vector<int>* winning_group) {
    absl::btree_set<int> distinct_pris;
    for (int r : *ready) {
      distinct_pris.insert(sched_.Priority(r));
    }
    for (auto it = distinct_pris.rbegin(); it != distinct_pris.rend(); ++it) {
      const int grp_pri = *it;
      std::vector<int> unblocked;
      std::vector<int> next_ready;
      next_ready.reserve(ready->size());
      for (int r : *ready) {
        if (sched_.Priority(r) == grp_pri) {
          const int cost = sched_.ActualHazard(r, clock);
          if (cost != 0) {
            sched_.QueueInsn(r, cost, q_ptr);
            continue;
          }
          unblocked.push_back(r);
        }
        next_ready.push_back(r);
      }
      *ready = std::move(next_ready);
      if (!unblocked.empty()) {
        *winning_group = std::move(unblocked);
        return true;
      }
    }
    return false;
  }

  int ChooseReplayInsn(const std::vector<int>& winning_group, int target, bool continue_on_conflict,
                       ForcedReplayResult* res) {
    bool target_in_group = false;
    for (int r : winning_group) {
      if (r == target) {
        target_in_group = true;
        break;
      }
    }
    if (!target_in_group) {
      res->had_conflict = true;
      if (!continue_on_conflict) {
        return -1;
      }
      int chosen = winning_group[0];
      int best_ph = sched_.PotentialHazard(chosen);
      for (size_t wi = 1; wi < winning_group.size(); ++wi) {
        const int c = winning_group[wi];
        const int c_ph = sched_.PotentialHazard(c);
        if (c_ph > best_ph || (c_ph == best_ph && c > chosen)) {
          best_ph = c_ph;
          chosen = c;
        }
      }
      return chosen;
    }
    const int target_ph = sched_.PotentialHazard(target);
    for (int c : winning_group) {
      if (c == target) {
        continue;
      }
      const int c_ph = sched_.PotentialHazard(c);
      if (c_ph > target_ph) {
        res->had_conflict = true;
      } else if (c_ph == target_ph) {
        const CmpRecord& rec = CmpAt(target, c);
        if (!rec.valid || (rec.pri_u == rec.pri_v && rec.cls_u == rec.cls_v)) {
          res->constraints.push_back(Constraint{.must_have_greater_luid = target, .than = c});
        } else if (rec.pri_u < rec.pri_v || (rec.pri_u == rec.pri_v && rec.cls_u < rec.cls_v)) {
          res->had_conflict = true;
        }
      }
    }
    return target;
  }

  ListScheduler sched_;
  int n_ = 0;
  std::vector<CmpRecord> last_cmp_;
};

}  // namespace

namespace detail {

ForcedReplayResult RunForcedReplay(absl::Span<const Insn> block, const DepGraph& graph,
                                   const RegFacts& facts, const MachineModel& mm, Phase phase,
                                   const absl::btree_set<int>& live_at_end,
                                   bool continue_on_conflict) {
  return ForcedReplayTracker(block, graph, facts, mm, phase, live_at_end).Run(continue_on_conflict);
}

}  // namespace detail

ScheduleResult Schedule(absl::Span<const Insn> block, const DepGraph& graph, const RegFacts& facts,
                        const MachineModel& mm, Phase phase,
                        const absl::btree_set<int>& live_at_end) {
  return ListScheduler(block, graph, facts, mm, phase, live_at_end).Run();
}

// =============================================================================
// 6. UpdateLife (`sched.c:3415-3942`)
// =============================================================================

namespace {

LifeResult UpdateLifeFromLiveAtEnd(absl::Span<const Insn> scheduled,
                                   const absl::btree_set<int>& live_at_end, const MachineModel& mm,
                                   Phase phase) {
  LifeResult res;
  res.notes.resize(scheduled.size());
  if (phase == Phase::kSched2) {
    for (size_t i = 0; i < scheduled.size(); ++i) {
      res.notes[i] = scheduled[i].pn.reg_notes;
    }
    return res;
  }

  absl::btree_set<int> bb_live = live_at_end;
  struct SometimesEntry {
    int regno = 0;
    int live_length = 0;
    int calls_crossed = 0;
  };
  absl::btree_set<int> old_live = bb_live;
  std::vector<SometimesEntry> sometimes;
  for (int r : bb_live) {
    sometimes.push_back(SometimesEntry{.regno = r});
  }

  // Backward scan mirroring `sched.c:3764-3882`.
  for (size_t idx = scheduled.size(); idx > 0; --idx) {
    const Insn& insn = scheduled[idx - 1];
    const RtlExpr& pat = insn.pn.pattern;
    if (pat.op == RtlOp::kSet || pat.op == RtlOp::kClobber) {
      SchedNoteSet(pat, /*death=*/true, mm, &bb_live);
    } else if (pat.op == RtlOp::kParallel) {
      for (size_t j = pat.children.size(); j > 0; --j) {
        const RtlExpr& ch = pat.children[j - 1];
        if (ch.op == RtlOp::kSet || ch.op == RtlOp::kClobber) {
          SchedNoteSet(ch, /*death=*/true, mm, &bb_live);
        }
      }
    }

    if (insn.kind == Insn::Kind::kCall) {
      for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
        if (mm.CallUsed(r)) {
          bb_live.erase(r);
        }
      }
      for (SometimesEntry& p : sometimes) {
        if (bb_live.contains(p.regno)) {
          p.calls_crossed += 1;
        }
      }
    }

    AttachDeathsInsn(insn, mm, old_live, &bb_live, &res.notes[idx - 1]);

    for (int r : bb_live) {
      if (!old_live.contains(r)) {
        old_live.insert(r);
        sometimes.push_back(SometimesEntry{.regno = r});
      }
    }

    for (size_t i = 0; i < sometimes.size();) {
      SometimesEntry& p = sometimes[i];
      p.live_length += 1;
      if (!bb_live.contains(p.regno)) {
        res.sched_live_length[p.regno] += p.live_length;
        res.sched_calls_crossed[p.regno] += p.calls_crossed;
        old_live.erase(p.regno);
        sometimes[i] = sometimes.back();
        sometimes.pop_back();
      } else {
        ++i;
      }
    }
  }

  // `finish_sometimes_live` (`sched.c:3941-3942`).
  for (const SometimesEntry& p : sometimes) {
    res.sched_live_length[p.regno] += p.live_length;
    res.sched_calls_crossed[p.regno] += p.calls_crossed;
  }
  return res;
}

}  // namespace

LifeResult UpdateLife(absl::Span<const Insn> scheduled, const absl::btree_set<int>& live_at_start,
                      absl::Span<const Insn> block0_pinned_prefix, const RegFacts& /*facts*/,
                      const MachineModel& mm, Phase phase) {
  if (phase == Phase::kSched2) {
    return UpdateLifeFromLiveAtEnd(scheduled, {}, mm, phase);
  }
  // Forward pass to reconstruct `bb_live_regs` at end of block (`sched.c:3417-3611`).
  absl::btree_set<int> bb_live = live_at_start;
  for (const Insn& pref : block0_pinned_prefix) {
    ApplyForwardInsnLivenessStep(pref, mm, &bb_live);
  }
  for (const Insn& insn : scheduled) {
    ApplyForwardInsnLivenessStep(insn, mm, &bb_live);
  }
  return UpdateLifeFromLiveAtEnd(scheduled, bb_live, mm, phase);
}

// =============================================================================
// 7. RunScheduleInsns (full forward CFG pass, `sched.c:4665-4967`)
// =============================================================================

namespace {

// Mirrors `rtlanal.c:rtx_equal_p`: compares RTL expressions ignoring per-rtx
// metadata flags (`/v`, `/u`, `/i`, `/s`, `/f`) so `(reg/v:SI 4 a0)` equals
// `(reg:SI 4 a0)` for post-reload no-op move deletion (`sched.c:4804`).
bool RtxEqualP(const RtlExpr& a, const RtlExpr& b) {
  if (a.op != b.op || a.mode != b.mode) {
    return false;
  }
  switch (a.op) {
    case RtlOp::kReg:
      return a.RegNo() == b.RegNo();
    case RtlOp::kConstInt:
      return a.int_val == b.int_val;
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
    case RtlOp::kCodeLabel:
      return a.str_val == b.str_val;
    case RtlOp::kScratch:
      return false;
    default:
      break;
  }
  if (a.int_val != b.int_val || a.str_val != b.str_val || a.children.size() != b.children.size()) {
    return false;
  }
  for (size_t i = 0; i < a.children.size(); ++i) {
    if (!RtxEqualP(a.children[i], b.children[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

RtlControlFlowGraph RunScheduleInsns(const RtlControlFlowGraph& in,
                                     const CompilationProfile& profile, Phase phase) {
  if (in.rpo_blocks.empty()) {
    return in;
  }
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);

  RtlControlFlowGraph out = in;
  out.pass = (phase == Phase::kSched1) ? RtlPassKind::kSched : RtlPassKind::kSched2;
  const bool active = (phase == Phase::kSched1) ? flags.RunsSched1() : flags.RunsSched2();
  if (!active) {
    return out;
  }

  const MachineModel mm = MachineModel::FromProfile(profile);
  // Pre-compute baseline `basic_block_live_at_start` and fresh flow stats uniformly
  // (zero B4 branching).
  RtlControlFlowGraph live_cfg = in;
  ReconstructRtlLiveness(&live_cfg, profile,
                         {.reload_completed = (phase == Phase::kSched2),
                          .update_reg_notes = false,
                          .preserve_phantom_pseudos = false});
  RegFacts facts = RegFacts::FromCfg(in, profile, phase);

  if (phase == Phase::kSched1) {
    // Mirrors `FunctionHooks::RunScheduleInsns` (`flow_analysis` + `RestoreExistingFlowState`):
    // rebuild `reg_n_sets`, `reg_live_length`, and `reg_n_calls_crossed` from active RTL
    // flow stats while excluding pure dead-set pseudos that `flow.c:1670` (`insn_dead_p`)
    // omits from `mark_set_1`, and preserving positive `saved_live_length` on phantom
    // pseudos that still carry `saved_n_refs > 0` (`function_hooks.cc:3277-3348`).
    absl::btree_set<int> read_pseudos;
    absl::btree_set<int> side_effect_set_pseudos;
    for (const RtlBasicBlock& blk : in.rpo_blocks) {
      for (const std::string& raw : blk.instructions) {
        auto parsed = ParsePatternAndNotes(raw);
        if (!parsed.ok()) {
          continue;
        }
        const RtlExpr& pat = parsed->pattern;
        bool has_se = false;
        pat.Visit([&](const RtlExpr& node) {
          if (node.op == RtlOp::kCall || node.op == RtlOp::kAsmInput ||
              node.op == RtlOp::kAsmOperands || node.op == RtlOp::kUnspecVolatile ||
              IsTrapIfExpr(node) || (node.op == RtlOp::kMem && node.flags.volatil)) {
            has_se = true;
          }
        });
        const bool is_live_parallel = (pat.op == RtlOp::kParallel && pat.children.size() > 1);
        auto scan_reads = [&](auto& self, const RtlExpr& node) -> void {
          if (node.op == RtlOp::kReg) {
            if (node.RegNo() >= kMipsFirstPseudoRegister) {
              read_pseudos.insert(node.RegNo());
            }
            return;
          }
          if (node.op == RtlOp::kSet && node.children.size() == 2) {
            const RtlExpr& dst = node.SetDest();
            if (dst.op == RtlOp::kReg && dst.RegNo() >= kMipsFirstPseudoRegister &&
                (has_se || is_live_parallel)) {
              side_effect_set_pseudos.insert(dst.RegNo());
            } else if (dst.op != RtlOp::kReg) {
              self(self, dst);
            }
            self(self, node.SetSrc());
            return;
          }
          if (node.op == RtlOp::kClobber) {
            // `flow.c:1792-1795` (`insn_dead_p`): an insn consisting solely of a
            // `CLOBBER` is never dead, so `mark_set_1` always counts its target
            // pseudo in `reg_n_sets` / `reg_live_length`.
            if ((is_live_parallel || pat.op == RtlOp::kClobber) && !node.children.empty() &&
                node.children[0].op == RtlOp::kReg &&
                node.children[0].RegNo() >= kMipsFirstPseudoRegister) {
              side_effect_set_pseudos.insert(node.children[0].RegNo());
            }
            return;
          }
          for (const RtlExpr& ch : node.children) {
            self(self, ch);
          }
        };
        scan_reads(scan_reads, pat);
      }
    }
    auto active_in_gcc_flow = [&](int r) -> bool {
      const auto active_it = live_cfg.sideband.reg_live_length.find(r);
      if (active_it == live_cfg.sideband.reg_live_length.end() || active_it->second <= 0) {
        return false;
      }
      if (r >= kMipsFirstPseudoRegister && !read_pseudos.contains(r) &&
          !side_effect_set_pseudos.contains(r)) {
        return false;
      }
      return true;
    };

    out.sideband.reg_n_sets.clear();
    for (const auto& [r, sets] : live_cfg.sideband.reg_n_sets) {
      if (active_in_gcc_flow(r)) {
        out.sideband.reg_n_sets[r] = sets;
      }
    }
    for (const auto& [r, sets] : in.sideband.reg_n_sets) {
      if (sets > 0 && active_in_gcc_flow(r)) {
        out.sideband.reg_n_sets[r] = sets;
      }
    }
    out.sideband.reg_live_length.clear();
    for (const auto& [r, len] : live_cfg.sideband.reg_live_length) {
      if (active_in_gcc_flow(r)) {
        out.sideband.reg_live_length[r] = len;
      }
    }
    out.sideband.reg_n_calls_crossed.clear();
    for (const auto& [r, cc] : live_cfg.sideband.reg_n_calls_crossed) {
      if (active_in_gcc_flow(r)) {
        out.sideband.reg_n_calls_crossed[r] = cc;
      }
    }
    for (const auto& [r, len] : in.sideband.reg_live_length) {
      if (len == 0) {
        continue;
      }
      const auto ref_it = in.sideband.reg_n_refs.find(r);
      const bool has_saved_refs = (ref_it != in.sideband.reg_n_refs.end() && ref_it->second > 0);
      if (active_in_gcc_flow(r) || has_saved_refs) {
        out.sideband.reg_live_length[r] = len;
      }
    }
    for (const auto& [r, cc] : in.sideband.reg_n_calls_crossed) {
      if (active_in_gcc_flow(r)) {
        out.sideband.reg_n_calls_crossed[r] = cc;
      }
    }
  }

  absl::btree_map<int, int> total_sched_live_length;
  absl::btree_map<int, int> total_sched_calls_crossed;

  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = out.rpo_blocks[b];
    if (blk.rtl_insns.size() != blk.instructions.size()) {
      blk.SyncRtlInsnsFromInstructions();
    }
    if (blk.instructions.empty()) {
      continue;
    }

    // Parse raw block items, preserving loop notes and `call_fusage`.
    struct RawItem {
      bool is_note = false;
      RtlLoopNoteKind note_kind = RtlLoopNoteKind::kLoopBeg;
      Insn insn;
      std::string raw_text;
      bool valid = false;
    };
    std::vector<RawItem> items;
    items.reserve(blk.instructions.size());
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      RawItem item;
      item.raw_text = blk.instructions[i];
      const auto loop_k = (i < blk.rtl_insns.size() && blk.rtl_insns[i].loop_note_kind.has_value())
                              ? blk.rtl_insns[i].loop_note_kind
                              : ParseRtlLoopNoteKind(blk.instructions[i]);
      if (loop_k.has_value()) {
        item.is_note = true;
        item.note_kind = *loop_k;
        item.valid = true;
        items.push_back(std::move(item));
        continue;
      }
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        items.push_back(std::move(item));
        continue;
      }
      if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
          !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = blk.rtl_insns[i].call_fusage;
      }
      item.valid = true;
      item.insn.pn = std::move(*parsed);
      item.insn.kind = ClassifyInsnKind(item.insn.pn.pattern);
      items.push_back(std::move(item));
    }

    // Per-block pre-pass (`sched.c:4784-4852`):
    // - `kSched2`: delete non-block-ending no-op self-moves (`sched.c:4803-4821`).
    // - `kSched1`: `try_split` + `update_flow_info` for `large_int` (`sched.c:4825-4848`).
    size_t last_real_insn_idx = items.size();
    for (size_t i = items.size(); i > 0; --i) {
      if (!items[i - 1].is_note) {
        last_real_insn_idx = i - 1;
        break;
      }
    }
    std::vector<RawItem> expanded;
    expanded.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].is_note || !items[i].valid || items[i].insn.kind != Insn::Kind::kInsn) {
        expanded.push_back(std::move(items[i]));
        continue;
      }
      const RtlExpr* set = SingleSetOf(items[i].insn.pn.pattern);
      if (set != nullptr && RtxEqualP(set->SetSrc(), set->SetDest())) {
        const bool is_block_end = (i == last_real_insn_idx);
        if (phase == Phase::kSched2 && !is_block_end) {
          continue;  // Deleted no-op move (`sched.c:4813-4818`).
        }
        expanded.push_back(std::move(items[i]));
        continue;
      }
      if (phase == Phase::kSched1) {
        RtlPatternAndNotes s1;
        RtlPatternAndNotes s2;
        int dest_regno = -1;
        if (TrySplitLargeIntInsn(items[i].insn.pn, &s1, &s2, &dest_regno)) {
          facts.reg_n_sets[dest_regno] += 1;
          out.sideband.reg_n_sets[dest_regno] = facts.reg_n_sets[dest_regno];
          RawItem r1;
          r1.valid = true;
          r1.insn.pn = std::move(s1);
          r1.insn.kind = Insn::Kind::kInsn;
          RawItem r2;
          r2.valid = true;
          r2.insn.pn = std::move(s2);
          r2.insn.kind = Insn::Kind::kInsn;
          expanded.push_back(std::move(r1));
          expanded.push_back(std::move(r2));
          continue;
        }
      }
      expanded.push_back(std::move(items[i]));
    }

    // Separate leading/trailing notes & block-0 pinned param copies (`sched.c:3191-3233`).
    // Strip trailing notes BEFORE `b0_pinned_insns` so `tail` matches `basic_block_end[b]`
    // (`flow.c:468-472`), preventing a sole block-0 param copy followed by `LOOP_BEG` from
    // being skipped (`sched.c:3197`).
    std::vector<RawItem> prefix_items;
    std::vector<Insn> b0_pinned_insns;
    size_t head = 0;
    size_t tail = expanded.size();
    std::vector<RawItem> suffix_notes;
    while (tail > head && expanded[tail - 1].is_note) {
      suffix_notes.push_back(std::move(expanded[tail - 1]));
      --tail;
    }
    std::reverse(suffix_notes.begin(), suffix_notes.end());

    if (phase == Phase::kSched1 && b == 0) {
      // Mirrors `sched.c:3197-3214` + `combine.c` `NOTE_INSN_DELETED` left before
      // a narrow `QI`/`HI` `$a0-$a3` parameter copy whose full-word copy was folded
      // away (`gcc_context.cc:3094-3119`): stop advancing `head` right before it.
      absl::btree_set<int> entry_word_copied_arg_regs;
      while (head + 1 < tail && !expanded[head].is_note && expanded[head].valid &&
             IsBlock0PinnedParamCopy(expanded[head].insn)) {
        const RtlExpr& pat = expanded[head].insn.pn.pattern;
        if (in.pass == RtlPassKind::kCombine && pat.op == RtlOp::kSet &&
            pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kReg) {
          const RtlExpr& dst = pat.SetDest();
          const RtlExpr& src = pat.SetSrc();
          const int src_r = src.RegNo();
          if ((src.mode == RtlMachineMode::kSi || src.mode == RtlMachineMode::kDi) &&
              src_r >= kMipsArgRegFirst && src_r <= kMipsArgRegLast) {
            entry_word_copied_arg_regs.insert(src_r);
          } else if ((dst.mode == RtlMachineMode::kQi || dst.mode == RtlMachineMode::kHi) &&
                     (src.mode == RtlMachineMode::kQi || src.mode == RtlMachineMode::kHi) &&
                     src_r >= kMipsArgRegFirst && src_r <= kMipsArgRegLast &&
                     !entry_word_copied_arg_regs.contains(src_r)) {
            break;
          }
        }
        b0_pinned_insns.push_back(expanded[head].insn);
        prefix_items.push_back(std::move(expanded[head]));
        ++head;
      }
    }
    while (head < tail && expanded[head].is_note) {
      prefix_items.push_back(std::move(expanded[head]));
      ++head;
    }

    // Build schedulable insn list, capturing `LOOP_BEG`/`LOOP_END` barriers vs
    // `LOOP_CONT`/`LOOP_VTOP` non-barrier `note_list` (`sched.c:3047-3056, 3951-3960`).
    std::vector<Insn> sched_block;
    std::vector<RtlLoopNoteKind> region_note_list;
    std::vector<RtlLoopNoteKind> pending_barriers;
    int luid = 0;
    for (size_t i = head; i < tail; ++i) {
      if (expanded[i].is_note) {
        const RtlLoopNoteKind k = expanded[i].note_kind;
        if (k == RtlLoopNoteKind::kLoopBeg || k == RtlLoopNoteKind::kLoopEnd) {
          pending_barriers.push_back(k);
        } else {
          region_note_list.push_back(k);
        }
        ++luid;
        continue;
      }
      Insn cur = std::move(expanded[i].insn);
      cur.luid = luid++;
      cur.leading_loop_notes = std::move(pending_barriers);
      pending_barriers.clear();
      sched_block.push_back(std::move(cur));
    }

    if (sched_block.empty()) {
      continue;
    }

    absl::btree_set<int> live_at_start;
    if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
        it != live_cfg.sideband.basic_block_live_at_start.end()) {
      for (int r : it->second) {
        live_at_start.insert(r);
      }
    }
    absl::btree_set<int> succ_live_union;
    if (phase == Phase::kSched1) {
      for (int succ : blk.successors) {
        if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
            it != live_cfg.sideband.basic_block_live_at_start.end()) {
          succ_live_union.insert(it->second.begin(), it->second.end());
        }
      }
    }
    absl::btree_set<int> live_at_end = live_at_start;
    if (phase == Phase::kSched1) {
      for (const Insn& pref : b0_pinned_insns) {
        ApplyForwardInsnLivenessStep(pref, mm, &live_at_end);
      }
      for (size_t i = 0; i < sched_block.size(); ++i) {
        const Insn& insn = sched_block[i];
        ApplyForwardInsnLivenessStep(insn, mm, &live_at_end);
        // `GccContext::DematerializeToCfg` (`gcc_context.cc:3877`) omits `REG_NOTES` on
        // `CALL_INSN` strings, so reconstruct any `REG_DEAD` deaths on `CALL_INSN` operands
        // (e.g. indirect call target registers) that are neither used later in the block nor
        // live into any successor.
        if (insn.kind == Insn::Kind::kCall) {
          absl::btree_set<int> call_reads;
          insn.pn.pattern.Visit([&](const RtlExpr& node) {
            if (node.op == RtlOp::kReg) {
              call_reads.insert(node.RegNo());
            }
          });
          for (const RtlExpr& fu : insn.pn.call_fusage) {
            fu.Visit([&](const RtlExpr& node) {
              if (node.op == RtlOp::kReg) {
                call_reads.insert(node.RegNo());
              }
            });
          }
          for (int r : call_reads) {
            if (r == kMipsArgPointerRegno || r == kMipsStackPointerRegno ||
                r == kMipsFramePointerRegno || !live_at_end.contains(r)) {
              continue;
            }
            if (r < kMipsFirstPseudoRegister && mm.CallUsed(r)) {
              live_at_end.erase(r);
              continue;
            }
            bool needed_later = succ_live_union.contains(r);
            for (size_t k = i + 1; !needed_later && k < sched_block.size(); ++k) {
              sched_block[k].pn.pattern.Visit([&](const RtlExpr& node) {
                if (node.op == RtlOp::kReg && node.RegNo() == r) {
                  needed_later = true;
                }
              });
            }
            if (!needed_later) {
              live_at_end.erase(r);
            }
          }
        }
      }
    }

    const DepGraph graph = Analyze(sched_block, facts, mm, phase);
    const ScheduleResult sched_res = Schedule(sched_block, graph, facts, mm, phase, live_at_end);

    std::vector<Insn> reordered;
    reordered.reserve(sched_res.order.size());
    for (int idx : sched_res.order) {
      reordered.push_back(sched_block[static_cast<size_t>(idx)]);
    }

    const LifeResult life = UpdateLifeFromLiveAtEnd(reordered, live_at_end, mm, phase);
    if (phase == Phase::kSched1) {
      for (const auto& [r, len] : life.sched_live_length) {
        total_sched_live_length[r] += len;
      }
      for (const auto& [r, cc] : life.sched_calls_crossed) {
        total_sched_calls_crossed[r] += cc;
      }
      // Tablejump blocks are followed in GCC RTL by a 1-insn `(jump_insn (addr_vec ...))`
      // basic block (`flow.c:447-480`, `sched.c:3830`) that contributes +1 live_length to
      // every register live across the switch dispatch.
      if (!reordered.empty() && reordered.back().kind == Insn::Kind::kJump &&
          reordered.back().pn.pattern.op == RtlOp::kParallel &&
          reordered.back().pn.pattern.children.size() >= 2 &&
          reordered.back().pn.pattern.children[0].op == RtlOp::kSet &&
          reordered.back().pn.pattern.children[0].SetDest().op == RtlOp::kPc &&
          reordered.back().pn.pattern.children[0].SetSrc().op == RtlOp::kReg) {
        for (int r : live_at_end) {
          total_sched_live_length[r] += 1;
        }
      }
    }

    // Re-emit block instructions, preserving `note_list` at region head (`sched.c:3951-3960`),
    // re-emitted `LOOP_BEG`/`LOOP_END` notes (`sched.c:3132`), and `call_fusage`.
    std::vector<std::string> new_insns;
    std::vector<std::optional<std::vector<RtlExpr>>> new_fusage;
    const std::vector<RtlRegNote> empty_reg_notes;
    auto push_raw_item = [&](const RawItem& item) {
      if (item.is_note) {
        new_insns.push_back(std::string(RtlLoopNoteKindToSexpr(item.note_kind)));
        new_fusage.push_back(std::nullopt);
      } else if (item.valid) {
        // `GccContext::DematerializeToCfg` (`gcc_context.cc:3877`) omits `REG_NOTES` on
        // `CALL_INSN` instruction strings.
        const auto& emitted_notes =
            (item.insn.kind == Insn::Kind::kCall) ? empty_reg_notes : item.insn.pn.reg_notes;
        new_insns.push_back(FormatGccPrintRtlPatternAndNotes(item.insn.pn.pattern, emitted_notes,
                                                             item.insn.pn.bracketed_notes,
                                                             /*call_fusage=*/{}));
        if (item.insn.pn.has_explicit_call_fusage) {
          new_fusage.push_back(item.insn.pn.call_fusage);
        } else {
          new_fusage.push_back(std::nullopt);
        }
      } else {
        new_insns.push_back(item.raw_text);
        new_fusage.push_back(std::nullopt);
      }
    };

    for (const RawItem& pref : prefix_items) {
      push_raw_item(pref);
    }
    for (RtlLoopNoteKind nk : region_note_list) {
      new_insns.push_back(std::string(RtlLoopNoteKindToSexpr(nk)));
      new_fusage.push_back(std::nullopt);
    }
    for (size_t i = 0; i < reordered.size(); ++i) {
      // `reemit_notes` (`sched.c:3132`) emits attached `LOOP_BEG`/`LOOP_END` before `insn`
      // in reverse of link prepend order (which restores original relative order).
      for (RtlLoopNoteKind nk : reordered[i].leading_loop_notes) {
        new_insns.push_back(std::string(RtlLoopNoteKindToSexpr(nk)));
        new_fusage.push_back(std::nullopt);
      }
      const auto& emitted_notes =
          (reordered[i].kind == Insn::Kind::kCall) ? empty_reg_notes : life.notes[i];
      new_insns.push_back(FormatGccPrintRtlPatternAndNotes(reordered[i].pn.pattern, emitted_notes,
                                                           reordered[i].pn.bracketed_notes,
                                                           /*call_fusage=*/{}));
      if (reordered[i].pn.has_explicit_call_fusage) {
        new_fusage.push_back(reordered[i].pn.call_fusage);
      } else {
        new_fusage.push_back(std::nullopt);
      }
    }
    for (const RawItem& suf : suffix_notes) {
      push_raw_item(suf);
    }

    blk.instructions = std::move(new_insns);
    blk.SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < blk.rtl_insns.size() && i < new_fusage.size(); ++i) {
      if (new_fusage[i].has_value() && !blk.rtl_insns[i].has_explicit_call_fusage) {
        blk.rtl_insns[i].has_explicit_call_fusage = true;
        blk.rtl_insns[i].call_fusage = std::move(*new_fusage[i]);
      }
    }
  }

  // Final `sched_reg_live_length` / `sched_reg_n_calls_crossed` write-back (`sched.c:4911-4966`).
  if (phase == Phase::kSched1) {
    for (const auto& [regno, sched_len] : total_sched_live_length) {
      if (sched_len == 0) {
        continue;
      }
      const auto cur_len_it = out.sideband.reg_live_length.find(regno);
      const int cur_len =
          (cur_len_it != out.sideband.reg_live_length.end()) ? cur_len_it->second : 0;
      if (cur_len >= 0) {
        out.sideband.reg_live_length[regno] = sched_len;
      }
      const auto sched_cc_it = total_sched_calls_crossed.find(regno);
      const int sched_cc =
          (sched_cc_it != total_sched_calls_crossed.end()) ? sched_cc_it->second : 0;
      if (sched_cc != 0 || !facts.reg_block_global.contains(regno)) {
        if (sched_cc != 0) {
          out.sideband.reg_n_calls_crossed[regno] = sched_cc;
        } else {
          out.sideband.reg_n_calls_crossed.erase(regno);
        }
      }
    }
  }

  return out;
}

// =============================================================================
// 8. InvertBlockOrder & IsAchievable (4-Phase Deterministic Search)
// =============================================================================

namespace {

constexpr int kMaxConflictRefinements = 32;
constexpr int kMaxSearchStates = 64;

bool IsValidPermutation(absl::Span<const int> perm, int n) {
  if (static_cast<int>(perm.size()) != n) {
    return false;
  }
  std::vector<bool> seen(static_cast<size_t>(n), false);
  for (int v : perm) {
    if (v < 0 || v >= n || seen[static_cast<size_t>(v)]) {
      return false;
    }
    seen[static_cast<size_t>(v)] = true;
  }
  return true;
}

bool HasMemOperand(const RtlExpr& x) {
  bool found = false;
  x.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kMem) {
      found = true;
    }
  });
  return found;
}

// Mirrors `sched.c:1735-1740` (`sched_analyze_1`): returns true iff `pat` writes
// to a `MEM` destination (which checks `pending_lists_length > 32`).
bool HasMemWriteDest(const RtlExpr& pat) {
  auto is_mem_set = [](const RtlExpr& s) {
    if (s.op != RtlOp::kSet || s.children.empty()) {
      return false;
    }
    const RtlExpr* dest = &s.children[0];
    while (dest->op == RtlOp::kStrictLowPart || dest->op == RtlOp::kSubreg ||
           dest->op == RtlOp::kZeroExtract || dest->op == RtlOp::kSignExtract) {
      if (dest->children.empty()) {
        break;
      }
      dest = &dest->children[0];
    }
    return dest->op == RtlOp::kMem;
  };
  if (is_mem_set(pat)) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      if (is_mem_set(ch)) {
        return true;
      }
    }
  }
  return false;
}

int CountPendingListMemOps(const RtlExpr& pat) {
  int count = 0;
  pat.Visit([&](const RtlExpr& n) {
    if (n.op == RtlOp::kMem) {
      ++count;
    }
  });
  return count;
}

// Mirrors `sched.c:4803-4821`: returns true iff `insn` is an `INSN` whose
// `single_set` satisfies `rtx_equal_p(SET_SRC(set), SET_DEST(set))`.
bool IsNoopSelfMoveInsn(const Insn& insn) {
  if (insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  const RtlExpr* set = SingleSetOf(insn.pn.pattern);
  return set != nullptr && RtxEqualP(set->SetSrc(), set->SetDest());
}

// Mirrors `sched.c:4803-4818`: in `kSched2` (`reload_completed != 0`), GCC
// deletes every no-op self-move before `schedule_block` EXCEPT the instruction
// at `basic_block_end[b]`. Any surviving no-op self-move in `scheduled_block`
// therefore MUST occupy `cand[n - 1]` in any valid pre-`kSched2` permutation.
bool HasMidBlockSched2NoopSelfMove(absl::Span<const Insn> scheduled_block,
                                   absl::Span<const int> cand) {
  const int n = static_cast<int>(scheduled_block.size());
  for (int pos = 0; pos + 1 < n; ++pos) {
    if (IsNoopSelfMoveInsn(scheduled_block[static_cast<size_t>(cand[static_cast<size_t>(pos)])])) {
      return true;
    }
  }
  return false;
}

// Pins any surviving `kSched2` no-op self-move `m` to `basic_block_end[b]`
// (`sched.c:4806-4818`) by adding acyclic edges `{u, m}` for every `u != m`
// not already reachable from `m` in `*dag_edges`.
void AddSched2NoopEndPinEdges(absl::Span<const Insn> scheduled_block,
                              absl::btree_set<std::pair<int, int>>* dag_edges) {
  const int n = static_cast<int>(scheduled_block.size());
  std::vector<int> noop_indices;
  for (int i = 0; i < n; ++i) {
    if (IsNoopSelfMoveInsn(scheduled_block[static_cast<size_t>(i)])) {
      noop_indices.push_back(i);
    }
  }
  if (noop_indices.size() != 1) {
    return;
  }
  const int m = noop_indices[0];
  std::vector<std::vector<int>> adj(static_cast<size_t>(n));
  for (const auto& [u, v] : *dag_edges) {
    adj[static_cast<size_t>(u)].push_back(v);
  }
  std::vector<bool> reachable_from_m(static_cast<size_t>(n), false);
  std::vector<int> stack = {m};
  reachable_from_m[static_cast<size_t>(m)] = true;
  while (!stack.empty()) {
    const int cur = stack.back();
    stack.pop_back();
    for (int nxt : adj[static_cast<size_t>(cur)]) {
      if (!reachable_from_m[static_cast<size_t>(nxt)]) {
        reachable_from_m[static_cast<size_t>(nxt)] = true;
        stack.push_back(nxt);
      }
    }
  }
  for (int u = 0; u < n; ++u) {
    if (!reachable_from_m[static_cast<size_t>(u)]) {
      dag_edges->insert({u, m});
    }
  }
}

struct VerifyOutcome {
  bool matches = false;
  bool dag_differs = false;
  int target = -1;
  int competitor = -1;
};

// Exact forward verification: builds `pre_block[pos] = scheduled_block[cand[pos]]` with
// `luid = pos`, runs `Analyze` + `Schedule`, and checks bit-exact reproduction of `0..n-1`.
VerifyOutcome VerifyCandidateOrder(absl::Span<const Insn> scheduled_block,
                                   absl::Span<const int> cand, const DepGraph& target_graph,
                                   const RegFacts& facts, const MachineModel& mm, Phase phase,
                                   const absl::btree_set<int>& live_at_end) {
  const int n = static_cast<int>(scheduled_block.size());
  if (phase == Phase::kSched2 && HasMidBlockSched2NoopSelfMove(scheduled_block, cand)) {
    return {};
  }
  std::vector<Insn> pre_block;
  pre_block.reserve(static_cast<size_t>(n));
  for (int pos = 0; pos < n; ++pos) {
    Insn insn = scheduled_block[static_cast<size_t>(cand[static_cast<size_t>(pos)])];
    insn.luid = pos;
    pre_block.push_back(std::move(insn));
  }
  const DepGraph pre_graph = Analyze(pre_block, facts, mm, phase);
  const ScheduleResult sched_res = Schedule(pre_block, pre_graph, facts, mm, phase, live_at_end);

  VerifyOutcome out;
  if (static_cast<int>(sched_res.order.size()) == n) {
    bool all_match = true;
    for (int i = n - 1; i >= 0; --i) {
      const int actual_orig = cand[static_cast<size_t>(sched_res.order[static_cast<size_t>(i)])];
      if (actual_orig != i) {
        all_match = false;
        out.target = i;
        out.competitor = actual_orig;
        break;
      }
    }
    if (all_match) {
      out.matches = true;
      return out;
    }
  }

  // Compare directed edge sets in `scheduled_block` coordinates to detect DAG shifts
  // (e.g., `pending_lists_length > 32` flush boundary shifting under `cand`).
  absl::btree_set<std::pair<int, int>> target_edges;
  for (int cons = 0; cons < n; ++cons) {
    for (const Link& l : target_graph.log_links[static_cast<size_t>(cons)]) {
      target_edges.insert({l.target, cons});
    }
  }
  absl::btree_set<std::pair<int, int>> cand_edges;
  for (int cons_pos = 0; cons_pos < n; ++cons_pos) {
    const int cons_orig = cand[static_cast<size_t>(cons_pos)];
    for (const Link& l : pre_graph.log_links[static_cast<size_t>(cons_pos)]) {
      cand_edges.insert({cand[static_cast<size_t>(l.target)], cons_orig});
    }
  }
  out.dag_differs = (target_edges != cand_edges);
  return out;
}

}  // namespace

namespace detail {

// Checks whether a directed graph on `0..n-1` with edge set `edges` and optional
// contracted chains (`next_in_chain`) is acyclic, and if so runs preference-biased
// Kahn's topological sort keyed by `pref_rank[chain_head]`.
std::optional<std::vector<int>> SolveSupernodeKahn(
    int n, const absl::btree_set<std::pair<int, int>>& edges, const std::vector<int>& next_in_chain,
    const std::vector<int>& prev_in_chain, const std::vector<int>& pref_rank) {
  std::vector<int> node_to_super(static_cast<size_t>(n), -1);
  std::vector<int> pos_in_chain(static_cast<size_t>(n), 0);
  std::vector<std::vector<int>> members;
  for (int v = 0; v < n; ++v) {
    if (prev_in_chain[static_cast<size_t>(v)] != -1) {
      continue;
    }
    const int sid = static_cast<int>(members.size());
    std::vector<int> chain;
    int cur = v;
    int pos = 0;
    while (cur != -1) {
      if (node_to_super[static_cast<size_t>(cur)] != -1) {
        return std::nullopt;  // Chain cycle.
      }
      node_to_super[static_cast<size_t>(cur)] = sid;
      pos_in_chain[static_cast<size_t>(cur)] = pos++;
      chain.push_back(cur);
      cur = next_in_chain[static_cast<size_t>(cur)];
    }
    members.push_back(std::move(chain));
  }
  for (int v = 0; v < n; ++v) {
    if (node_to_super[static_cast<size_t>(v)] == -1) {
      return std::nullopt;
    }
  }

  const int num_super = static_cast<int>(members.size());
  std::vector<absl::btree_set<int>> adj(static_cast<size_t>(num_super));
  std::vector<int> indeg(static_cast<size_t>(num_super), 0);
  for (const auto& [u, v] : edges) {
    const int su = node_to_super[static_cast<size_t>(u)];
    const int sv = node_to_super[static_cast<size_t>(v)];
    if (su == sv) {
      if (pos_in_chain[static_cast<size_t>(u)] > pos_in_chain[static_cast<size_t>(v)]) {
        return std::nullopt;  // Edge goes backwards inside a contracted chain.
      }
      continue;
    }
    if (adj[static_cast<size_t>(su)].insert(sv).second) {
      indeg[static_cast<size_t>(sv)] += 1;
    }
  }

  using HeapEntry = std::pair<int, int>;  // (pref_rank[chain_head], supernode_id)
  std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> pq;
  for (int s = 0; s < num_super; ++s) {
    if (indeg[static_cast<size_t>(s)] == 0) {
      pq.push({pref_rank[static_cast<size_t>(members[static_cast<size_t>(s)][0])], s});
    }
  }

  std::vector<int> order;
  order.reserve(static_cast<size_t>(n));
  int popped_super = 0;
  while (!pq.empty()) {
    const int s = pq.top().second;
    pq.pop();
    ++popped_super;
    for (int v : members[static_cast<size_t>(s)]) {
      order.push_back(v);
    }
    for (int nxt : adj[static_cast<size_t>(s)]) {
      if (--indeg[static_cast<size_t>(nxt)] == 0) {
        pq.push({pref_rank[static_cast<size_t>(members[static_cast<size_t>(nxt)][0])], nxt});
      }
    }
  }
  if (popped_super != num_super || static_cast<int>(order.size()) != n) {
    return std::nullopt;
  }
  return order;
}

}  // namespace detail

namespace {

bool TryMergeChainPair(int first, int second, int n,
                       const absl::btree_set<std::pair<int, int>>& edges,
                       std::vector<int>* next_in_chain, std::vector<int>* prev_in_chain,
                       const std::vector<int>& pref_rank) {
  if (first < 0 || first >= n || second < 0 || second >= n || first == second) {
    return false;
  }
  if ((*next_in_chain)[static_cast<size_t>(first)] != -1 ||
      (*prev_in_chain)[static_cast<size_t>(second)] != -1) {
    return false;
  }
  // Reject if `second` is already an ancestor of `first` in the same chain.
  int cur = second;
  while (cur != -1) {
    if (cur == first) {
      return false;
    }
    cur = (*next_in_chain)[static_cast<size_t>(cur)];
  }
  (*next_in_chain)[static_cast<size_t>(first)] = second;
  (*prev_in_chain)[static_cast<size_t>(second)] = first;
  if (SolveSupernodeKahn(n, edges, *next_in_chain, *prev_in_chain, pref_rank).has_value()) {
    return true;
  }
  (*next_in_chain)[static_cast<size_t>(first)] = -1;
  (*prev_in_chain)[static_cast<size_t>(second)] = -1;
  return false;
}

bool SatisfiesAdjacentPairs(absl::Span<const int> order, absl::Span<const AdjacentPair> pairs) {
  if (pairs.empty()) {
    return true;
  }
  std::vector<int> pos(order.size(), -1);
  for (size_t i = 0; i < order.size(); ++i) {
    pos[static_cast<size_t>(order[i])] = static_cast<int>(i);
  }
  for (const AdjacentPair& p : pairs) {
    if (p.first < 0 || p.second < 0 || static_cast<size_t>(p.first) >= order.size() ||
        static_cast<size_t>(p.second) >= order.size()) {
      continue;
    }
    if (pos[static_cast<size_t>(p.second)] != pos[static_cast<size_t>(p.first)] + 1) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace detail {

// Identifies `sched.c:1704` (`call_used_regs[i]` indexed by word offset `i == 0`
// instead of `regno + i`, hitting `call_used_regs[0] == 1` on MIPS) post-call
// anti-dependence artifact edges: a callee-saved hard-reg `SET` that originally
// preceded a `CALL_INSN` in `Q` has no edge `(insn -> call)` in `Analyze(Q)` and
// can legally sink after `CALL_INSN` in `P`, where `Analyze(P)` adds a
// non-invertible `REG_DEP_ANTI` edge `(call -> insn)`.
bool IsPureSched1704CallArtifactEdge(absl::Span<const Insn> block, const DepGraph& graph,
                                     int call_idx, int insn_idx, const MachineModel& mm) {
  const int n = static_cast<int>(block.size());
  if (call_idx < 0 || call_idx >= n || insn_idx < 0 || insn_idx >= n || call_idx == insn_idx) {
    return false;
  }
  const Insn& call_insn = block[static_cast<size_t>(call_idx)];
  const Insn& insn = block[static_cast<size_t>(insn_idx)];
  if (call_insn.kind != Insn::Kind::kCall || insn.kind != Insn::Kind::kInsn ||
      !insn.leading_loop_notes.empty() || HasMemOperand(insn.pn.pattern) ||
      insn.pn.pattern.op == RtlOp::kUse || insn.pn.pattern.op == RtlOp::kClobber ||
      insn.pn.pattern.op == RtlOp::kNote) {
    return false;
  }
  bool touches_call_used = false;
  absl::btree_set<int> insn_regs;
  auto scan_expr = [&](const RtlExpr& root) {
    root.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kMipsFirstPseudoRegister) {
        const int r0 = node.RegNo();
        const int nr = std::max(1, mm.HardRegNRegs(r0, node.mode));
        for (int k = 0; k < nr; ++k) {
          const int r = r0 + k;
          if (mm.CallUsed(r) || r == 0 || r == 28 || r == kMipsStackPointerRegno ||
              r == kMipsReturnAddrRegno || r >= kMipsMdRegFirst) {
            touches_call_used = true;
          }
          insn_regs.insert(r);
        }
      }
    });
  };
  scan_expr(insn.pn.pattern);
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead) {
      scan_expr(note.datum);
    }
  }
  if (touches_call_used || insn_regs.empty()) {
    return false;
  }
  for (const RtlExpr& fu : call_insn.pn.call_fusage) {
    bool conflicts_fusage = false;
    fu.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kMipsFirstPseudoRegister) {
        const int r0 = node.RegNo();
        const int nr = std::max(1, mm.HardRegNRegs(r0, node.mode));
        for (int k = 0; k < nr; ++k) {
          if (insn_regs.contains(r0 + k)) {
            conflicts_fusage = true;
          }
        }
      }
    });
    if (conflicts_fusage) {
      return false;
    }
  }
  bool has_anti = false;
  for (const Link& l : graph.log_links[static_cast<size_t>(insn_idx)]) {
    if (l.target == call_idx) {
      if (l.kind == DepKind::kAnti) {
        has_anti = true;
      } else {
        return false;
      }
    }
  }
  return has_anti;
}

}  // namespace detail

namespace {

int CountPendingListMemOpsForInsn(const Insn& insn, Phase phase) {
  int count = CountPendingListMemOps(insn.pn.pattern);
  if (phase == Phase::kSched2) {
    for (const RtlRegNote& note : insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead) {
        count += CountPendingListMemOps(note.datum);
      }
    }
  }
  return count;
}

// Mirrors `sched.c:1957, 1970, 2058-2074` (`sched_analyze_2` / `sched_analyze_insn`):
// volatile `ASM_OPERANDS`, standalone `ASM_INPUT`, `UNSPEC_VOLATILE`, calls,
// and instructions with leading loop notes flush all pending memory lists.
// Non-volatile `ASM_OPERANDS` do NOT flush (their `ASM_INPUT` children are
// constraint vectors, skipped by `sched.c:1978-1983`).
bool ExprHasPendingFlushBarrier(const RtlExpr& x) {
  if (x.op == RtlOp::kAsmInput || x.op == RtlOp::kUnspecVolatile) {
    return true;
  }
  if (x.op == RtlOp::kAsmOperands) {
    if (x.flags.volatil) {
      return true;
    }
    for (const RtlExpr& ch : x.children) {
      if (ch.op != RtlOp::kAsmInput && ExprHasPendingFlushBarrier(ch)) {
        return true;
      }
    }
    return false;
  }
  for (const RtlExpr& ch : x.children) {
    if (ExprHasPendingFlushBarrier(ch)) {
      return true;
    }
  }
  return false;
}

bool IsPendingFlushBarrier(const Insn& insn) {
  return insn.kind == Insn::Kind::kCall || !insn.leading_loop_notes.empty() ||
         ExprHasPendingFlushBarrier(insn.pn.pattern);
}

// Mirrors `sched.c:1740, 1748` (`sched_analyze_1` / `flush_pending_lists`):
// given a candidate order `cand` whose first `MEM` write at `flush_pos` trips
// `pending_lists_length > 32`, computes refined `extra_edges` sets that either:
//  1a. Defer the minimum number of semantically independent pre-hub `MEM` loads
//      (highest `ranks` first) past `hub` (`{hub, m}`), dissolving the cap
//      overflow at `hub`;
//  1b. Move `hub` before the earliest semantically independent pre-hub `MEM`
//      load in the segment (`{hub, earliest_load}`), lifting `hub` ahead of a
//      serial `> 32`-load chain; or
//  2.  Move an independent post-hub `MEM` write `w` in the same call/asm-free
//      segment ahead of `hub` (`{w, hub}`) so `w` absorbs the `> 32` flush and
//      resets `pending_lists_length` before `hub`.
std::vector<absl::btree_set<std::pair<int, int>>> ComputePendingFlushDissolutionExtraEdges(
    absl::Span<const Insn> scheduled_block, Phase phase,
    const absl::btree_set<std::pair<int, int>>& base_extra_edges,
    const absl::btree_set<std::pair<int, int>>& edges, absl::Span<const int> cand,
    const std::vector<int>& ranks, absl::Span<const AdjacentPair> preferred_adjacent,
    bool use_soft_adj, bool gate_load_defer_by_pref) {
  const int n = static_cast<int>(scheduled_block.size());
  if (static_cast<int>(cand.size()) != n) {
    return {};
  }
  int pending_count = 0;
  int segment_start = 0;
  int flush_pos = -1;
  int flush_excess = 0;
  for (int pos = 0; pos < n; ++pos) {
    const Insn& insn = scheduled_block[static_cast<size_t>(cand[static_cast<size_t>(pos)])];
    if (IsPendingFlushBarrier(insn)) {
      pending_count = 0;
      segment_start = pos + 1;
      continue;
    }
    if (HasMemWriteDest(insn.pn.pattern) && pending_count > 32) {
      flush_pos = pos;
      flush_excess = pending_count - 32;
      break;
    }
    pending_count += CountPendingListMemOpsForInsn(insn, phase);
  }
  if (flush_pos < 0 || flush_excess <= 0) {
    return {};
  }
  const int hub = cand[static_cast<size_t>(flush_pos)];
  std::vector<std::vector<int>> rev_adj(static_cast<size_t>(n));
  std::vector<std::vector<int>> fwd_adj(static_cast<size_t>(n));
  for (const auto& [u, v] : edges) {
    rev_adj[static_cast<size_t>(v)].push_back(u);
    fwd_adj[static_cast<size_t>(u)].push_back(v);
  }
  if (use_soft_adj) {
    for (const AdjacentPair& p : preferred_adjacent) {
      if (p.first >= 0 && p.first < n && p.second >= 0 && p.second < n) {
        rev_adj[static_cast<size_t>(p.first)].push_back(p.second);
        rev_adj[static_cast<size_t>(p.second)].push_back(p.first);
        fwd_adj[static_cast<size_t>(p.first)].push_back(p.second);
        fwd_adj[static_cast<size_t>(p.second)].push_back(p.first);
      }
    }
  }
  auto bfs_reachable = [&](const std::vector<std::vector<int>>& graph) {
    std::vector<bool> vis(static_cast<size_t>(n), false);
    std::vector<int> stack = {hub};
    vis[static_cast<size_t>(hub)] = true;
    while (!stack.empty()) {
      const int cur = stack.back();
      stack.pop_back();
      for (int nxt : graph[static_cast<size_t>(cur)]) {
        if (!vis[static_cast<size_t>(nxt)]) {
          vis[static_cast<size_t>(nxt)] = true;
          stack.push_back(nxt);
        }
      }
    }
    return vis;
  };
  const std::vector<bool> is_ancestor = bfs_reachable(rev_adj);
  const std::vector<bool> is_descendant = bfs_reachable(fwd_adj);

  std::vector<absl::btree_set<std::pair<int, int>>> out;

  // Strategy 1a & 1b: defer independent pre-hub `MEM` loads past `hub` (`{hub, m}`).
  std::vector<int> movable_loads;
  int earliest_movable_load = -1;
  bool any_pref_after_hub = !gate_load_defer_by_pref;
  for (int q = segment_start; q < flush_pos; ++q) {
    const int m = cand[static_cast<size_t>(q)];
    if (is_ancestor[static_cast<size_t>(m)]) {
      continue;
    }
    const Insn& minsn = scheduled_block[static_cast<size_t>(m)];
    if (CountPendingListMemOpsForInsn(minsn, phase) > 0 && !HasMemWriteDest(minsn.pn.pattern)) {
      if (earliest_movable_load < 0) {
        earliest_movable_load = m;
      }
      movable_loads.push_back(m);
      if (ranks[static_cast<size_t>(m)] > ranks[static_cast<size_t>(hub)]) {
        any_pref_after_hub = true;
      }
    }
  }
  for (int q = flush_pos + 1; q < n && !any_pref_after_hub; ++q) {
    const int m = cand[static_cast<size_t>(q)];
    const Insn& minsn = scheduled_block[static_cast<size_t>(m)];
    if (IsPendingFlushBarrier(minsn)) {
      break;
    }
    if (CountPendingListMemOpsForInsn(minsn, phase) > 0 && !HasMemWriteDest(minsn.pn.pattern) &&
        ranks[static_cast<size_t>(m)] > ranks[static_cast<size_t>(hub)]) {
      any_pref_after_hub = true;
    }
  }
  if (!movable_loads.empty() && any_pref_after_hub) {
    std::sort(movable_loads.begin(), movable_loads.end(), [&](int a, int b) {
      if (ranks[static_cast<size_t>(a)] != ranks[static_cast<size_t>(b)]) {
        return ranks[static_cast<size_t>(a)] > ranks[static_cast<size_t>(b)];
      }
      return a > b;
    });
    absl::btree_set<std::pair<int, int>> next_extra = base_extra_edges;
    int deferred_ops = 0;
    for (int m : movable_loads) {
      next_extra.erase({m, hub});
      next_extra.insert({hub, m});
      deferred_ops += CountPendingListMemOpsForInsn(scheduled_block[static_cast<size_t>(m)], phase);
      if (deferred_ops >= flush_excess) {
        break;
      }
    }
    out.push_back(std::move(next_extra));
    if (earliest_movable_load >= 0 && !gate_load_defer_by_pref) {
      // Strategy 1c: batch-lift `MEM` writes in the same segment that do not
      // depend on `earliest_movable_load` ahead of `earliest_movable_load`.
      // When a `> 32`-load chain (e.g., `mem/u` loads) was hoisted ahead of
      // multiple independent stores, lifting only `hub` causes the next store
      // to immediately become a new `> 32` flush hub; batch-lifting dissolves
      // the entire flush cascade in one step. Emit the prefix batch (stopping
      // at the first `load_desc` store, which is the true flush sink) first so
      // stores originally following that sink stay after it, then emit the full
      // segment batch when additional independent stores appear after the sink.
      std::vector<bool> load_desc(static_cast<size_t>(n), false);
      {
        std::vector<int> stack = {earliest_movable_load};
        load_desc[static_cast<size_t>(earliest_movable_load)] = true;
        while (!stack.empty()) {
          const int cur = stack.back();
          stack.pop_back();
          for (int nxt : fwd_adj[static_cast<size_t>(cur)]) {
            if (!load_desc[static_cast<size_t>(nxt)]) {
              load_desc[static_cast<size_t>(nxt)] = true;
              stack.push_back(nxt);
            }
          }
        }
      }
      absl::btree_set<std::pair<int, int>> prefix_batch_hoist = base_extra_edges;
      absl::btree_set<std::pair<int, int>> full_batch_hoist = base_extra_edges;
      int lifted_prefix = 0;
      int lifted_full = 0;
      bool seen_load_desc_write = false;
      for (int q = segment_start; q < n; ++q) {
        const int w = cand[static_cast<size_t>(q)];
        const Insn& winsn = scheduled_block[static_cast<size_t>(w)];
        if (IsPendingFlushBarrier(winsn)) {
          break;
        }
        if (!HasMemWriteDest(winsn.pn.pattern)) {
          continue;
        }
        if (load_desc[static_cast<size_t>(w)]) {
          seen_load_desc_write = true;
          continue;
        }
        full_batch_hoist.erase({earliest_movable_load, w});
        full_batch_hoist.insert({w, earliest_movable_load});
        ++lifted_full;
        if (!seen_load_desc_write) {
          prefix_batch_hoist.erase({earliest_movable_load, w});
          prefix_batch_hoist.insert({w, earliest_movable_load});
          ++lifted_prefix;
        }
      }
      if (lifted_prefix > 1) {
        out.push_back(std::move(prefix_batch_hoist));
      }
      if (lifted_full > 1 && lifted_full != lifted_prefix) {
        out.push_back(std::move(full_batch_hoist));
      }
      // Strategy 1b: lift `hub` ahead of `earliest_movable_load`.
      absl::btree_set<std::pair<int, int>> hoist_extra = base_extra_edges;
      hoist_extra.erase({earliest_movable_load, hub});
      hoist_extra.insert({hub, earliest_movable_load});
      out.push_back(std::move(hoist_extra));
    }
  }

  // Strategy 2: transfer the `> 32` flush hub back to an independent post-hub
  // `MEM` write `w` in the same segment (`{w, hub}`), e.g., when `schedule_block`
  // hoisted an independent store `hub` ahead of the original flushing store `w`.
  std::vector<int> post_writes;
  for (int q = flush_pos + 1; q < n; ++q) {
    const int w = cand[static_cast<size_t>(q)];
    const Insn& winsn = scheduled_block[static_cast<size_t>(w)];
    if (IsPendingFlushBarrier(winsn)) {
      break;
    }
    if (!HasMemWriteDest(winsn.pn.pattern) || is_descendant[static_cast<size_t>(w)]) {
      continue;
    }
    post_writes.push_back(w);
  }
  std::vector<int> chosen_writes;
  for (size_t i = 0; i < post_writes.size() && i < 2; ++i) {
    chosen_writes.push_back(post_writes[i]);
  }
  if (post_writes.size() > 2) {
    chosen_writes.push_back(post_writes.back());
  }
  for (int w : chosen_writes) {
    absl::btree_set<std::pair<int, int>> next_extra = base_extra_edges;
    next_extra.erase({hub, w});
    next_extra.insert({w, hub});
    out.push_back(std::move(next_extra));
  }
  return out;
}

// Mirrors `sched.c:2125-2157` (`SCHED_GROUP_P` for `(use (reg ...))` preceding
// `CALL_INSN`/`JUMP_INSN`): if `schedule_block` accidentally landed an
// independent instruction `mid` ahead of `(use (reg ...))` so `(use ...)` became
// adjacent to `CALL`/`JUMP` in `scheduled_block`, `target_graph.sched_group_p`
// locks them together even though they were separated in the pre-schedule block.
// Returns candidate extra-edge sets placing an independent `mid < use_idx`
// between `use_idx` and `call_idx` (`{use_idx, mid}`, `{mid, call_idx}`).
std::vector<absl::btree_set<std::pair<int, int>>> CollectUnpinSchedGroupEdgeSets(
    int n, const DepGraph& target_graph, const absl::btree_set<std::pair<int, int>>& dag_edges) {
  std::vector<std::vector<int>> rev_adj(static_cast<size_t>(n));
  for (const auto& [u, v] : dag_edges) {
    rev_adj[static_cast<size_t>(v)].push_back(u);
  }
  std::vector<absl::btree_set<std::pair<int, int>>> out;
  for (int i = 1; i < n && out.size() < 4; ++i) {
    if (!target_graph.sched_group_p[static_cast<size_t>(i)]) {
      continue;
    }
    int use_idx = i - 1;
    while (use_idx > 0 && target_graph.sched_group_p[static_cast<size_t>(use_idx)]) {
      --use_idx;
    }
    std::vector<bool> is_ancestor(static_cast<size_t>(n), false);
    std::vector<int> stack = {use_idx};
    is_ancestor[static_cast<size_t>(use_idx)] = true;
    while (!stack.empty()) {
      const int cur = stack.back();
      stack.pop_back();
      for (int pred : rev_adj[static_cast<size_t>(cur)]) {
        if (!is_ancestor[static_cast<size_t>(pred)]) {
          is_ancestor[static_cast<size_t>(pred)] = true;
          stack.push_back(pred);
        }
      }
    }
    for (int mid = use_idx - 1; mid >= 0 && out.size() < 4; --mid) {
      if (is_ancestor[static_cast<size_t>(mid)]) {
        continue;
      }
      absl::btree_set<std::pair<int, int>> extra;
      extra.insert({use_idx, mid});
      extra.insert({mid, i});
      out.push_back(std::move(extra));
    }
  }
  return out;
}

// Phase 1 of `InvertBlockOrder`: preference-respecting fast check.
// Never short-circuit to identity `P` before checking `preferred_order` or `preferred_adjacent`.
std::optional<std::vector<int>> TryFastCheckPreferenceOrder(
    absl::Span<const Insn> scheduled_block, const DepGraph& target_graph, const RegFacts& facts,
    const MachineModel& mm, Phase phase, const absl::btree_set<int>& live_at_end,
    absl::Span<const int> preferred_order, absl::Span<const AdjacentPair> preferred_adjacent,
    bool has_valid_pref, std::optional<std::vector<int>>* fallback_verified_pref) {
  const int n = static_cast<int>(scheduled_block.size());
  if (has_valid_pref) {
    const VerifyOutcome pref_check = VerifyCandidateOrder(
        scheduled_block, preferred_order, target_graph, facts, mm, phase, live_at_end);
    if (pref_check.matches) {
      std::vector<int> pref_vec(preferred_order.begin(), preferred_order.end());
      if (SatisfiesAdjacentPairs(pref_vec, preferred_adjacent)) {
        return pref_vec;
      }
      *fallback_verified_pref = std::move(pref_vec);
    }
    return std::nullopt;
  }
  if (preferred_adjacent.empty()) {
    std::vector<int> identity(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      identity[static_cast<size_t>(i)] = i;
    }
    if (VerifyCandidateOrder(scheduled_block, identity, target_graph, facts, mm, phase, live_at_end)
            .matches) {
      return identity;
    }
  }
  return std::nullopt;
}

// Immutable per-block state shared across Phases 2–4 of `InvertBlockOrder`.
struct InvertBlockOrderContext {
  absl::Span<const Insn> scheduled_block;
  const DepGraph& target_graph;
  const RegFacts& facts;
  const MachineModel& mm;
  Phase phase = Phase::kSched1;
  const absl::btree_set<int>& live_at_end;
  absl::Span<const AdjacentPair> preferred_adjacent;
  int n = 0;
  bool has_valid_pref = false;
  int last_tail = -1;
  absl::btree_set<std::pair<int, int>> dag_edges;
  std::vector<int> hard_next;
  std::vector<int> hard_prev;
  std::vector<int> pref_rank;
  std::vector<int> identity_rank;
  std::vector<int> mem_insns;
  int total_pending_mem_ops = 0;
  bool check_pending_flush_artifacts = false;
  std::optional<DepGraph> no_cap_graph;
  absl::btree_set<std::pair<int, int>> semantic_no_flush_cap_edges;

  bool IsPurePendingFlushArtifact(int u, int v) const {
    return check_pending_flush_artifacts && !semantic_no_flush_cap_edges.contains({u, v}) &&
           (last_tail < 0 || v < last_tail);
  }
};

// Combines `base_edges` with the maximal acyclic prefix of Phase-2 forced-replay
// LUID tie-break constraints (`sched.c:2428` `rank_for_schedule`).
absl::btree_set<std::pair<int, int>> BuildForcedReplayEdgeSet(
    int n, const absl::btree_set<std::pair<int, int>>& base_edges,
    absl::Span<const Constraint> constraints, const std::vector<int>& hard_next,
    const std::vector<int>& hard_prev, const std::vector<int>& ranks,
    absl::btree_set<std::pair<int, int>>* added_extra_out = nullptr) {
  absl::btree_set<std::pair<int, int>> edges = base_edges;
  for (const Constraint& c : constraints) {
    if (c.than < 0 || c.than >= n || c.must_have_greater_luid < 0 ||
        c.must_have_greater_luid >= n || c.than == c.must_have_greater_luid) {
      continue;
    }
    const std::pair<int, int> edge = {c.than, c.must_have_greater_luid};
    if (edges.contains(edge)) {
      continue;
    }
    edges.insert(edge);
    if (!SolveSupernodeKahn(n, edges, hard_next, hard_prev, ranks).has_value()) {
      edges.erase(edge);
    } else if (added_extra_out != nullptr) {
      added_extra_out->insert(edge);
    }
  }
  return edges;
}

// Phase 3 primitive: preference-biased supernode Kahn + exact forward verification.
std::optional<std::vector<int>> TrySupernodeKahnAndVerify(
    const InvertBlockOrderContext& ctx, const absl::btree_set<std::pair<int, int>>& edges,
    bool use_soft_adj, const std::vector<int>& ranks, VerifyOutcome* out_outcome,
    std::vector<int>* out_cand = nullptr, bool unpin_sched_group = false) {
  std::vector<int> nxt =
      unpin_sched_group ? std::vector<int>(static_cast<size_t>(ctx.n), -1) : ctx.hard_next;
  std::vector<int> prv =
      unpin_sched_group ? std::vector<int>(static_cast<size_t>(ctx.n), -1) : ctx.hard_prev;
  if (use_soft_adj) {
    for (const AdjacentPair& p : ctx.preferred_adjacent) {
      TryMergeChainPair(p.first, p.second, ctx.n, edges, &nxt, &prv, ranks);
    }
  }
  std::optional<std::vector<int>> cand = SolveSupernodeKahn(ctx.n, edges, nxt, prv, ranks);
  if (!cand.has_value()) {
    return std::nullopt;
  }
  if (out_cand != nullptr) {
    *out_cand = *cand;
  }
  VerifyOutcome outcome = VerifyCandidateOrder(ctx.scheduled_block, *cand, ctx.target_graph,
                                               ctx.facts, ctx.mm, ctx.phase, ctx.live_at_end);
  if (out_outcome != nullptr) {
    *out_outcome = outcome;
  }
  if (outcome.matches) {
    return cand;
  }
  return std::nullopt;
}

struct PendingFlushRelaxState {
  absl::btree_set<std::pair<int, int>> pref_relaxed_call_edges;
  absl::btree_set<std::pair<int, int>> pref_relaxed_flush_edges;
  absl::btree_set<std::pair<int, int>> pref_relaxed_all_edges;
  VerifyOutcome relaxed_outcome;
  std::vector<int> relaxed_cand;
  absl::btree_set<std::pair<int, int>> no_cap_replay_extra;
  VerifyOutcome no_cap_relaxed_outcome;
  std::vector<int> no_cap_relaxed_cand;
  bool has_no_cap_relaxed = false;
  absl::btree_set<std::pair<int, int>> flush_only_replay_extra;
  VerifyOutcome flush_only_outcome;
  std::vector<int> flush_only_cand;
  bool has_flush_only_relaxed = false;
};

// Phase 3 relaxation for preference-requested `sched.c:1704` call-artifact and
// `sched.c:1740, 1748` (`pending_lists_length > 32`) flush-cap artifact edges.
std::optional<std::vector<int>> TryPendingFlushDissolutionFallback(
    const InvertBlockOrderContext& ctx,
    const absl::btree_set<std::pair<int, int>>& constrained_edges,
    std::optional<std::vector<int>>* fallback_verified_pref, PendingFlushRelaxState* state) {
  if (!ctx.has_valid_pref) {
    return std::nullopt;
  }
  bool wants_pending_flush_relax = false;
  for (const auto& [u, v] : ctx.dag_edges) {
    if (ctx.pref_rank[static_cast<size_t>(v)] < ctx.pref_rank[static_cast<size_t>(u)]) {
      if (ctx.phase == Phase::kSched2 &&
          IsPureSched1704CallArtifactEdge(ctx.scheduled_block, ctx.target_graph, u, v, ctx.mm)) {
        state->pref_relaxed_call_edges.insert({u, v});
      } else if (ctx.IsPurePendingFlushArtifact(u, v)) {
        state->pref_relaxed_flush_edges.insert({u, v});
        wants_pending_flush_relax = true;
      }
    }
  }
  if (wants_pending_flush_relax) {
    // `sched.c:1740, 1748` (`flush_pending_lists`): `pending_lists_length > 32`
    // is all-or-nothing. When `preferred_order` moves memory ops across a
    // post-`sched2` flush hub to dissolve the cap overflow, all pure flush-cap
    // artifact edges vanish together and `rank_for_schedule` (`sched.c:2428`)
    // LUID tie-break constraints follow the unflushed `no_cap_graph` priorities;
    // `VerifyCandidateOrder` still verifies exact `Analyze(cand)` WITH `> 32`.
    // Keep `pref_relaxed_flush_edges` separate from `sched.c:1704` call artifact
    // edges so `no_cap_edges` matches `*no_cap_graph` exactly.
    for (const auto& [u, v] : ctx.dag_edges) {
      if (ctx.IsPurePendingFlushArtifact(u, v)) {
        state->pref_relaxed_flush_edges.insert({u, v});
      }
    }
    if (ctx.no_cap_graph.has_value()) {
      const ForcedReplayResult no_cap_replay = RunForcedReplay(
          ctx.scheduled_block, *ctx.no_cap_graph, ctx.facts, ctx.mm, ctx.phase, ctx.live_at_end);
      absl::btree_set<std::pair<int, int>> pref_relaxed_union = state->pref_relaxed_call_edges;
      pref_relaxed_union.insert(state->pref_relaxed_flush_edges.begin(),
                                state->pref_relaxed_flush_edges.end());
      absl::btree_set<std::pair<int, int>> base_no_cap_edges;
      for (const auto& e : ctx.dag_edges) {
        if (!pref_relaxed_union.contains(e)) {
          base_no_cap_edges.insert(e);
        }
      }
      const absl::btree_set<std::pair<int, int>> no_cap_edges = BuildForcedReplayEdgeSet(
          ctx.n, base_no_cap_edges, no_cap_replay.constraints, ctx.hard_next, ctx.hard_prev,
          ctx.pref_rank, &state->no_cap_replay_extra);
      state->has_no_cap_relaxed = true;
      if (auto verified = TrySupernodeKahnAndVerify(ctx, no_cap_edges, /*use_soft_adj=*/true,
                                                    ctx.pref_rank, &state->no_cap_relaxed_outcome,
                                                    &state->no_cap_relaxed_cand);
          verified.has_value()) {
        if (SatisfiesAdjacentPairs(*verified, ctx.preferred_adjacent)) {
          return verified;
        }
        if (!fallback_verified_pref->has_value()) {
          *fallback_verified_pref = std::move(verified);
        }
      }
      // When `preferred_order` also requested relaxing `sched.c:1704` call edges,
      // `no_cap_graph` still contains those call edges; also test dropping ONLY
      // `pref_relaxed_flush_edges` so an unsolvable call hoist cannot mask flush dissolution.
      if (!state->pref_relaxed_call_edges.empty()) {
        absl::btree_set<std::pair<int, int>> flush_only_edges;
        for (const auto& e : ctx.dag_edges) {
          if (!state->pref_relaxed_flush_edges.contains(e)) {
            flush_only_edges.insert(e);
          }
        }
        for (const auto& e : state->no_cap_replay_extra) {
          if (!flush_only_edges.contains(e)) {
            flush_only_edges.insert(e);
            if (!SolveSupernodeKahn(ctx.n, flush_only_edges, ctx.hard_next, ctx.hard_prev,
                                    ctx.pref_rank)
                     .has_value()) {
              flush_only_edges.erase(e);
            } else {
              state->flush_only_replay_extra.insert(e);
            }
          }
        }
        state->has_flush_only_relaxed = true;
        if (auto verified = TrySupernodeKahnAndVerify(ctx, flush_only_edges, /*use_soft_adj=*/true,
                                                      ctx.pref_rank, &state->flush_only_outcome,
                                                      &state->flush_only_cand);
            verified.has_value()) {
          if (SatisfiesAdjacentPairs(*verified, ctx.preferred_adjacent)) {
            return verified;
          }
          if (!fallback_verified_pref->has_value()) {
            *fallback_verified_pref = std::move(verified);
          }
        }
      }
    }
  }
  state->pref_relaxed_all_edges = state->pref_relaxed_call_edges;
  state->pref_relaxed_all_edges.insert(state->pref_relaxed_flush_edges.begin(),
                                       state->pref_relaxed_flush_edges.end());
  if (!state->pref_relaxed_all_edges.empty()) {
    absl::btree_set<std::pair<int, int>> relaxed_edges = constrained_edges;
    for (const auto& edge : state->pref_relaxed_all_edges) {
      relaxed_edges.erase(edge);
    }
    if (auto verified =
            TrySupernodeKahnAndVerify(ctx, relaxed_edges, /*use_soft_adj=*/true, ctx.pref_rank,
                                      &state->relaxed_outcome, &state->relaxed_cand);
        verified.has_value()) {
      if (SatisfiesAdjacentPairs(*verified, ctx.preferred_adjacent)) {
        return verified;
      }
      if (!fallback_verified_pref->has_value()) {
        *fallback_verified_pref = std::move(verified);
      }
    }
  }
  return std::nullopt;
}

struct ConflictSearchState {
  absl::btree_set<std::pair<int, int>> extra_edges;
  absl::btree_set<std::pair<int, int>> dropped_dag_edges;
  bool use_soft_adj = true;
  bool use_pref_rank = true;
  bool unpin_sched_group = false;
};

struct ConflictRefinementQueues {
  using StateKey = std::tuple<absl::btree_set<std::pair<int, int>>,
                              absl::btree_set<std::pair<int, int>>, bool, bool, bool>;
  absl::btree_set<StateKey> visited;
  // Keep preference-guided (`use_pref_rank == true`) search states strictly
  // ahead of `identity_rank` (`use_pref_rank == false`) fallback states so an
  // early `identity_rank` match never preempts multi-conflict refinement of
  // `preferred_order` (`sched.c:schedule_block`).
  std::deque<ConflictSearchState> pref_worklist;
  std::deque<ConflictSearchState> fallback_worklist;

  void Enqueue(const InvertBlockOrderContext& ctx, ConflictSearchState st) {
    const bool is_primary_pref = st.use_pref_rank && !st.unpin_sched_group &&
                                 (ctx.preferred_adjacent.empty() || st.use_soft_adj);
    if (is_primary_pref && static_cast<int>(visited.size()) >= kMaxSearchStates) {
      return;
    }
    StateKey key{st.extra_edges, st.dropped_dag_edges, st.use_soft_adj, st.use_pref_rank,
                 st.unpin_sched_group};
    if (visited.insert(std::move(key)).second) {
      if (is_primary_pref) {
        pref_worklist.push_back(std::move(st));
      } else {
        fallback_worklist.push_back(std::move(st));
      }
    }
  }

  void EnqueueCallBugRelaxations(const InvertBlockOrderContext& ctx,
                                 const ConflictSearchState& base, int focus_insn) {
    if (focus_insn < 0 || focus_insn >= ctx.n) {
      return;
    }
    for (const Link& l : ctx.target_graph.log_links[static_cast<size_t>(focus_insn)]) {
      if (IsPureSched1704CallArtifactEdge(ctx.scheduled_block, ctx.target_graph, l.target,
                                          focus_insn, ctx.mm)) {
        const std::pair<int, int> art_edge = {l.target, focus_insn};
        if (!base.dropped_dag_edges.contains(art_edge)) {
          ConflictSearchState nxt = base;
          nxt.dropped_dag_edges.insert(art_edge);
          nxt.extra_edges.erase(art_edge);
          nxt.extra_edges.insert({focus_insn, l.target});
          Enqueue(ctx, std::move(nxt));
        }
      }
    }
  }

  void EnqueuePendingFlushDissolution(const InvertBlockOrderContext& ctx,
                                      const ConflictSearchState& base,
                                      const absl::btree_set<std::pair<int, int>>& edges,
                                      absl::Span<const int> cand, const std::vector<int>& ranks) {
    if (!ctx.check_pending_flush_artifacts || base.dropped_dag_edges.empty() ||
        static_cast<int>(cand.size()) != ctx.n) {
      return;
    }
    const bool gate_by_pref = ctx.has_valid_pref && base.use_pref_rank;
    for (auto& extra : ComputePendingFlushDissolutionExtraEdges(
             ctx.scheduled_block, ctx.phase, base.extra_edges, edges, cand, ranks,
             ctx.preferred_adjacent, base.use_soft_adj, gate_by_pref)) {
      ConflictSearchState nxt = base;
      nxt.extra_edges = std::move(extra);
      Enqueue(ctx, std::move(nxt));
    }
  }
};

absl::btree_set<std::pair<int, int>> BuildEffectiveEdges(
    const absl::btree_set<std::pair<int, int>>& dag_edges, const ConflictSearchState& st) {
  absl::btree_set<std::pair<int, int>> edges;
  for (const auto& e : dag_edges) {
    if (!st.dropped_dag_edges.contains(e)) {
      edges.insert(e);
    }
  }
  for (const auto& e : st.extra_edges) {
    edges.insert(e);
  }
  return edges;
}

// Seeds Phase 4 conflict-driven refinement worklists from Phase 2/3 outcomes.
void SeedConflictRefinementWorklists(const InvertBlockOrderContext& ctx,
                                     const PendingFlushRelaxState& relax,
                                     const absl::btree_set<std::pair<int, int>>& replay_extra,
                                     const VerifyOutcome& phase3_outcome,
                                     absl::Span<const int> phase3_cand,
                                     bool only_refine_pref_relaxed,
                                     ConflictRefinementQueues* queues) {
  // Seed worklist: (1) Phase-2 root + first conflict edge / call-bug relaxation,
  // (2) clean `E_dag` root, (3) uncontracted soft-adj root if `preferred_adjacent` was non-empty.
  if (!relax.pref_relaxed_all_edges.empty()) {
    if (relax.has_no_cap_relaxed) {
      if (relax.no_cap_relaxed_outcome.dag_differs && !relax.no_cap_relaxed_cand.empty()) {
        ConflictSearchState st;
        st.extra_edges = relax.no_cap_replay_extra;
        st.dropped_dag_edges = relax.pref_relaxed_all_edges;
        st.use_soft_adj = true;
        st.use_pref_rank = true;
        queues->EnqueuePendingFlushDissolution(ctx, st, BuildEffectiveEdges(ctx.dag_edges, st),
                                               relax.no_cap_relaxed_cand, ctx.pref_rank);
      }
      if (!only_refine_pref_relaxed && !relax.no_cap_relaxed_cand.empty()) {
        // When no verified preimage exists yet, seed `fallback_worklist`
        // (`use_pref_rank = false`) with un-gated flush dissolution from the
        // clean dropped-flush DAG so multi-store cascades dissolve without
        // disturbing `pref_worklist` preference alignment.
        ConflictSearchState st_clean_id;
        st_clean_id.dropped_dag_edges = relax.pref_relaxed_all_edges;
        st_clean_id.use_soft_adj = true;
        st_clean_id.use_pref_rank = false;
        queues->EnqueuePendingFlushDissolution(ctx, st_clean_id,
                                               BuildEffectiveEdges(ctx.dag_edges, st_clean_id),
                                               relax.no_cap_relaxed_cand, ctx.identity_rank);
        queues->Enqueue(ctx, st_clean_id);
      }
      if (relax.no_cap_relaxed_outcome.target >= 0 &&
          relax.no_cap_relaxed_outcome.competitor >= 0) {
        ConflictSearchState st;
        st.extra_edges = relax.no_cap_replay_extra;
        st.dropped_dag_edges = relax.pref_relaxed_all_edges;
        st.extra_edges.insert(
            {relax.no_cap_relaxed_outcome.competitor, relax.no_cap_relaxed_outcome.target});
        st.use_soft_adj = true;
        st.use_pref_rank = true;
        queues->Enqueue(ctx, std::move(st));
      }
      if (relax.has_flush_only_relaxed && !relax.pref_relaxed_flush_edges.empty()) {
        if (relax.flush_only_outcome.dag_differs && !relax.flush_only_cand.empty()) {
          ConflictSearchState st;
          st.extra_edges = relax.flush_only_replay_extra;
          st.dropped_dag_edges = relax.pref_relaxed_flush_edges;
          st.use_soft_adj = true;
          st.use_pref_rank = true;
          queues->EnqueuePendingFlushDissolution(ctx, st, BuildEffectiveEdges(ctx.dag_edges, st),
                                                 relax.flush_only_cand, ctx.pref_rank);
        }
        if (relax.flush_only_outcome.target >= 0 && relax.flush_only_outcome.competitor >= 0) {
          ConflictSearchState st;
          st.extra_edges = relax.flush_only_replay_extra;
          st.dropped_dag_edges = relax.pref_relaxed_flush_edges;
          st.extra_edges.insert(
              {relax.flush_only_outcome.competitor, relax.flush_only_outcome.target});
          st.use_soft_adj = true;
          st.use_pref_rank = true;
          queues->Enqueue(ctx, std::move(st));
        }
        ConflictSearchState clean_flush_relaxed;
        clean_flush_relaxed.dropped_dag_edges = relax.pref_relaxed_flush_edges;
        clean_flush_relaxed.use_soft_adj = true;
        clean_flush_relaxed.use_pref_rank = true;
        queues->Enqueue(ctx, std::move(clean_flush_relaxed));
      }
    }
    if (relax.relaxed_outcome.target >= 0 && relax.relaxed_outcome.competitor >= 0) {
      ConflictSearchState st;
      st.extra_edges = replay_extra;
      for (const auto& edge : relax.pref_relaxed_all_edges) {
        st.extra_edges.erase(edge);
      }
      st.dropped_dag_edges = relax.pref_relaxed_all_edges;
      st.extra_edges.insert({relax.relaxed_outcome.competitor, relax.relaxed_outcome.target});
      st.use_soft_adj = true;
      st.use_pref_rank = true;
      queues->Enqueue(ctx, std::move(st));
    }
    ConflictSearchState clean_relaxed;
    clean_relaxed.dropped_dag_edges = relax.pref_relaxed_all_edges;
    clean_relaxed.use_soft_adj = true;
    clean_relaxed.use_pref_rank = true;
    queues->Enqueue(ctx, std::move(clean_relaxed));
    if (!ctx.preferred_adjacent.empty()) {
      ConflictSearchState no_adj_relaxed;
      no_adj_relaxed.dropped_dag_edges = relax.pref_relaxed_all_edges;
      no_adj_relaxed.use_soft_adj = false;
      no_adj_relaxed.use_pref_rank = true;
      queues->Enqueue(ctx, std::move(no_adj_relaxed));
    }
  }
  if (only_refine_pref_relaxed) {
    return;
  }
  if (phase3_outcome.target >= 0 && phase3_outcome.competitor >= 0) {
    ConflictSearchState st;
    st.extra_edges = replay_extra;
    st.extra_edges.insert({phase3_outcome.competitor, phase3_outcome.target});
    st.use_soft_adj = true;
    st.use_pref_rank = true;
    queues->Enqueue(ctx, std::move(st));
    ConflictSearchState base_replay{
        .extra_edges = replay_extra, .use_soft_adj = true, .use_pref_rank = true};
    queues->EnqueueCallBugRelaxations(ctx, base_replay, phase3_outcome.competitor);
    queues->EnqueueCallBugRelaxations(ctx, base_replay, phase3_outcome.target);
    ConflictSearchState base_clean{.extra_edges = {}, .use_soft_adj = true, .use_pref_rank = true};
    queues->EnqueueCallBugRelaxations(ctx, base_clean, phase3_outcome.competitor);
    queues->EnqueueCallBugRelaxations(ctx, base_clean, phase3_outcome.target);
  }
  if (phase3_outcome.dag_differs && ctx.total_pending_mem_ops > 32) {
    ConflictSearchState st;
    st.extra_edges = replay_extra;
    for (size_t j = 0; j + 1 < ctx.mem_insns.size(); ++j) {
      st.extra_edges.insert({ctx.mem_insns[j], ctx.mem_insns[j + 1]});
    }
    st.use_soft_adj = true;
    st.use_pref_rank = true;
    queues->Enqueue(ctx, std::move(st));
  }
  // Mirrors `sched.c:1740, 1748` (`pending_lists_length > 32`) for BOTH
  // `kSched1` and `kSched2` even without a valid `preferred_order`: when
  // Phase 3 failed on a `> 32`-memory block containing pure flush-cap
  // artifact edges, seed Phase 4 with `no_cap` states (dropping all pure
  // flush edges) and flush-dissolution / store-hub transfer candidates.
  if (ctx.check_pending_flush_artifacts && relax.pref_relaxed_flush_edges.empty() &&
      ctx.no_cap_graph.has_value()) {
    absl::btree_set<std::pair<int, int>> all_pending_flush_edges;
    for (const auto& [u, v] : ctx.dag_edges) {
      if (ctx.IsPurePendingFlushArtifact(u, v)) {
        all_pending_flush_edges.insert({u, v});
      }
    }
    if (!all_pending_flush_edges.empty()) {
      const ForcedReplayResult nc_replay = RunForcedReplay(
          ctx.scheduled_block, *ctx.no_cap_graph, ctx.facts, ctx.mm, ctx.phase, ctx.live_at_end);
      absl::btree_set<std::pair<int, int>> clean_edges;
      for (const auto& e : ctx.dag_edges) {
        if (!all_pending_flush_edges.contains(e)) {
          clean_edges.insert(e);
        }
      }
      absl::btree_set<std::pair<int, int>> nc_extra;
      const absl::btree_set<std::pair<int, int>> nc_edges =
          BuildForcedReplayEdgeSet(ctx.n, clean_edges, nc_replay.constraints, ctx.hard_next,
                                   ctx.hard_prev, ctx.identity_rank, &nc_extra);
      ConflictSearchState nc_clean;
      nc_clean.dropped_dag_edges = all_pending_flush_edges;
      nc_clean.use_soft_adj = true;
      // When `preferred_order` did not request inverting any flush edge,
      // keep no-cap flush-dissolution states in `fallback_worklist`
      // (`use_pref_rank = false`) so they never preempt preference-guided
      // conflict refinement when a preference-preserving preimage exists.
      nc_clean.use_pref_rank = !ctx.has_valid_pref;
      const std::vector<int>& nc_ranks = nc_clean.use_pref_rank ? ctx.pref_rank : ctx.identity_rank;
      if (!phase3_cand.empty()) {
        queues->EnqueuePendingFlushDissolution(ctx, nc_clean, clean_edges, phase3_cand, nc_ranks);
      }
      queues->Enqueue(ctx, nc_clean);
      ConflictSearchState nc_base;
      nc_base.extra_edges = nc_extra;
      nc_base.dropped_dag_edges = all_pending_flush_edges;
      nc_base.use_soft_adj = true;
      nc_base.use_pref_rank = !ctx.has_valid_pref;
      if (!phase3_cand.empty()) {
        queues->EnqueuePendingFlushDissolution(ctx, nc_base, nc_edges, phase3_cand, nc_ranks);
      }
      queues->Enqueue(ctx, nc_base);
    }
  }
  for (auto& unpin_extra : CollectUnpinSchedGroupEdgeSets(ctx.n, ctx.target_graph, ctx.dag_edges)) {
    ConflictSearchState st;
    st.extra_edges = std::move(unpin_extra);
    st.use_soft_adj = true;
    st.use_pref_rank = false;
    st.unpin_sched_group = true;
    queues->Enqueue(ctx, std::move(st));
  }
  queues->Enqueue(
      ctx, ConflictSearchState{.extra_edges = {}, .use_soft_adj = true, .use_pref_rank = true});
  if (!ctx.preferred_adjacent.empty()) {
    queues->Enqueue(ctx, ConflictSearchState{
                             .extra_edges = replay_extra,
                             .use_soft_adj = false,
                             .use_pref_rank = true,
                         });
    queues->Enqueue(
        ctx, ConflictSearchState{.extra_edges = {}, .use_soft_adj = false, .use_pref_rank = true});
  }
  if (ctx.has_valid_pref) {
    queues->Enqueue(ctx, ConflictSearchState{
                             .extra_edges = replay_extra,
                             .use_soft_adj = true,
                             .use_pref_rank = false,
                         });
    queues->Enqueue(
        ctx, ConflictSearchState{.extra_edges = {}, .use_soft_adj = false, .use_pref_rank = false});
  }
}

// Phase 4: Bounded deterministic conflict-driven refinement (`<= 32` refinements, `<= 64` states).
// Note: `pending_lists_length > 32` (`sched.c:1740, 1748`) is all-or-nothing and handled
// as a group via `all_pending_flush_edges` / `ComputePendingFlushDissolutionExtraEdges`
// rather than single-edge drops (which would exhaust `kMaxSearchStates` on a 35-edge flush hub).
std::optional<std::vector<int>> RunConflictDrivenRefinementBfs(
    const InvertBlockOrderContext& ctx, const PendingFlushRelaxState& relax,
    const absl::btree_set<std::pair<int, int>>& replay_extra, const VerifyOutcome& phase3_outcome,
    absl::Span<const int> phase3_cand, bool only_refine_pref_relaxed,
    std::optional<std::vector<int>>* fallback_verified_pref) {
  ConflictRefinementQueues queues;
  SeedConflictRefinementWorklists(ctx, relax, replay_extra, phase3_outcome, phase3_cand,
                                  only_refine_pref_relaxed, &queues);

  int refinements = 0;
  int pref_refinements = 0;
  constexpr int kMaxPrefRefinements = kMaxConflictRefinements - 6;
  while (refinements < kMaxConflictRefinements) {
    ConflictSearchState cur;
    if (!queues.pref_worklist.empty() &&
        (queues.fallback_worklist.empty() || pref_refinements < kMaxPrefRefinements)) {
      cur = std::move(queues.pref_worklist.front());
      queues.pref_worklist.pop_front();
      ++pref_refinements;
    } else if (!queues.fallback_worklist.empty()) {
      cur = std::move(queues.fallback_worklist.front());
      queues.fallback_worklist.pop_front();
    } else {
      break;
    }
    ++refinements;

    const absl::btree_set<std::pair<int, int>> edges = BuildEffectiveEdges(ctx.dag_edges, cur);
    const std::vector<int>& ranks = cur.use_pref_rank ? ctx.pref_rank : ctx.identity_rank;
    VerifyOutcome outcome;
    std::vector<int> cur_cand;
    if (auto verified = TrySupernodeKahnAndVerify(ctx, edges, cur.use_soft_adj, ranks, &outcome,
                                                  &cur_cand, cur.unpin_sched_group);
        verified.has_value()) {
      if (SatisfiesAdjacentPairs(*verified, ctx.preferred_adjacent) ||
          (queues.pref_worklist.empty() && !cur.use_soft_adj)) {
        return verified;
      }
      if (!fallback_verified_pref->has_value()) {
        *fallback_verified_pref = std::move(verified);
      }
    }

    if (ctx.total_pending_mem_ops > 32) {
      if (cur.dropped_dag_edges.empty() && outcome.dag_differs) {
        ConflictSearchState nxt = cur;
        for (size_t j = 0; j + 1 < ctx.mem_insns.size(); ++j) {
          nxt.extra_edges.insert({ctx.mem_insns[j], ctx.mem_insns[j + 1]});
        }
        queues.Enqueue(ctx, std::move(nxt));
      } else if (!cur.dropped_dag_edges.empty()) {
        queues.EnqueuePendingFlushDissolution(ctx, cur, edges, cur_cand, ranks);
      }
    }
    if (outcome.target >= 0 && outcome.competitor >= 0) {
      const std::pair<int, int> conflict_edge = {outcome.competitor, outcome.target};
      const std::pair<int, int> reverse_edge = {outcome.target, outcome.competitor};
      if (cur.extra_edges.contains(reverse_edge)) {
        ConflictSearchState nxt = cur;
        nxt.extra_edges.erase(reverse_edge);
        nxt.extra_edges.insert(conflict_edge);
        queues.Enqueue(ctx, std::move(nxt));
      } else if (!edges.contains(conflict_edge)) {
        ConflictSearchState nxt = cur;
        nxt.extra_edges.insert(conflict_edge);
        queues.Enqueue(ctx, std::move(nxt));
      }
      queues.EnqueueCallBugRelaxations(ctx, cur, outcome.competitor);
      queues.EnqueueCallBugRelaxations(ctx, cur, outcome.target);
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::vector<int>> InvertBlockOrder(
    absl::Span<const Insn> scheduled_block, const RegFacts& facts, const MachineModel& mm,
    Phase phase, const absl::btree_set<int>& live_at_end, absl::Span<const int> preferred_order,
    absl::Span<const AdjacentPair> preferred_adjacent) {
  const int n = static_cast<int>(scheduled_block.size());
  if (n == 0) {
    return std::vector<int>{};
  }
  if (n == 1) {
    return std::vector<int>{0};
  }

  const DepGraph target_graph = Analyze(scheduled_block, facts, mm, phase);
  const bool has_valid_pref = IsValidPermutation(preferred_order, n);

  // Phase 1: Preference-respecting fast check.
  std::optional<std::vector<int>> fallback_verified_pref;
  if (auto fast = TryFastCheckPreferenceOrder(scheduled_block, target_graph, facts, mm, phase,
                                              live_at_end, preferred_order, preferred_adjacent,
                                              has_valid_pref, &fallback_verified_pref);
      fast.has_value()) {
    return fast;
  }

  // Phase 2: Forced reverse replay tracking `last_cmp[u][v]` across `qsort`, `swap_sort`,
  // and no-sort steps (`sched.c:2373-2378, 2425-2447, 2616-2704`).
  const ForcedReplayResult replay =
      RunForcedReplay(scheduled_block, target_graph, facts, mm, phase, live_at_end);

  InvertBlockOrderContext ctx{
      .scheduled_block = scheduled_block,
      .target_graph = target_graph,
      .facts = facts,
      .mm = mm,
      .phase = phase,
      .live_at_end = live_at_end,
      .preferred_adjacent = preferred_adjacent,
      .n = n,
      .has_valid_pref = has_valid_pref,
      .last_tail = replay.last_tail,
      .hard_next = std::vector<int>(static_cast<size_t>(n), -1),
      .hard_prev = std::vector<int>(static_cast<size_t>(n), -1),
      .pref_rank = std::vector<int>(static_cast<size_t>(n)),
      .identity_rank = std::vector<int>(static_cast<size_t>(n)),
  };
  for (int cons = 0; cons < n; ++cons) {
    for (const Link& l : target_graph.log_links[static_cast<size_t>(cons)]) {
      ctx.dag_edges.insert({l.target, cons});
    }
  }
  if (replay.last_tail >= 0) {
    for (int u = 0; u < replay.last_tail; ++u) {
      ctx.dag_edges.insert({u, replay.last_tail});
    }
    for (int k = replay.last_tail; k + 1 < n; ++k) {
      ctx.dag_edges.insert({k, k + 1});
    }
  }
  if (phase == Phase::kSched2) {
    AddSched2NoopEndPinEdges(scheduled_block, &ctx.dag_edges);
  }
  for (int i = 0; i < n; ++i) {
    ctx.identity_rank[static_cast<size_t>(i)] = i;
  }
  if (has_valid_pref) {
    for (int pos = 0; pos < n; ++pos) {
      ctx.pref_rank[static_cast<size_t>(preferred_order[static_cast<size_t>(pos)])] = pos;
    }
  } else {
    ctx.pref_rank = ctx.identity_rank;
  }
  for (int i = 1; i < n; ++i) {
    if (target_graph.sched_group_p[static_cast<size_t>(i)]) {
      ctx.hard_next[static_cast<size_t>(i - 1)] = i;
      ctx.hard_prev[static_cast<size_t>(i)] = i - 1;
    }
  }

  // Precompute memory-order indices and semantic (no-cap) memory edges for
  // `pending_lists_length > 32` (`sched.c:1740`) DAG-shift detection/relaxation.
  // Track `total_pending_mem_ops` (not just `mem_insns.size()`) because a
  // single `kSched2` instruction with `REG_DEAD (mem ...)` (`sched.c:2095-2102`)
  // or multi-`MEM` pattern contributes multiple entries to `pending_lists_length`.
  for (int i = 0; i < n; ++i) {
    const int ops = CountPendingListMemOpsForInsn(scheduled_block[static_cast<size_t>(i)], phase);
    if (ops > 0) {
      ctx.mem_insns.push_back(i);
      ctx.total_pending_mem_ops += ops;
    }
  }
  ctx.check_pending_flush_artifacts = ctx.total_pending_mem_ops > 32;
  if (ctx.check_pending_flush_artifacts) {
    ctx.no_cap_graph = AnalyzeWithoutPendingListFlushCap(scheduled_block, facts, mm, phase);
    for (int cons = 0; cons < n; ++cons) {
      for (const Link& l : ctx.no_cap_graph->log_links[static_cast<size_t>(cons)]) {
        ctx.semantic_no_flush_cap_edges.insert({l.target, cons});
      }
    }
  }

  // Combine `E_dag` with acyclic Phase-2 LUID constraints (`c -> target`).
  absl::btree_set<std::pair<int, int>> replay_extra;
  const absl::btree_set<std::pair<int, int>> constrained_edges =
      BuildForcedReplayEdgeSet(n, ctx.dag_edges, replay.constraints, ctx.hard_next, ctx.hard_prev,
                               ctx.pref_rank, &replay_extra);

  // Phase 3: Preference-biased supernode Kahn + call/pending-flush relaxation.
  PendingFlushRelaxState relax;
  if (auto verified = TryPendingFlushDissolutionFallback(ctx, constrained_edges,
                                                         &fallback_verified_pref, &relax);
      verified.has_value()) {
    return verified;
  }

  VerifyOutcome phase3_outcome;
  std::vector<int> phase3_cand;
  bool only_refine_pref_relaxed = false;
  if (auto verified = TrySupernodeKahnAndVerify(ctx, constrained_edges, /*use_soft_adj=*/true,
                                                ctx.pref_rank, &phase3_outcome, &phase3_cand);
      verified.has_value()) {
    if (relax.pref_relaxed_all_edges.empty() &&
        SatisfiesAdjacentPairs(*verified, preferred_adjacent)) {
      return verified;
    }
    // `preferred_order` explicitly requested inverting one or more pure
    // `sched.c:1704` / `sched.c:1740` (`pending_lists_length > 32`) artifact
    // edges, or `constrained_edges` prevented merging a `preferred_adjacent`
    // pair (`reload1.c:emit_reload_insns`). Keep the verified preimage as a
    // guaranteed fallback and allow bounded Phase-4 refinement first.
    if (!fallback_verified_pref.has_value() ||
        SatisfiesAdjacentPairs(*verified, preferred_adjacent)) {
      fallback_verified_pref = std::move(verified);
    }
    if (!relax.pref_relaxed_all_edges.empty()) {
      only_refine_pref_relaxed = true;
    }
  }

  // Phase 4: Bounded deterministic conflict-driven refinement BFS.
  if (auto refined =
          RunConflictDrivenRefinementBfs(ctx, relax, replay_extra, phase3_outcome, phase3_cand,
                                         only_refine_pref_relaxed, &fallback_verified_pref);
      refined.has_value()) {
    return refined;
  }
  if (fallback_verified_pref.has_value()) {
    return fallback_verified_pref;
  }
  return std::nullopt;
}

namespace detail {

// Mirrors `optabs.c:2507-2523` (`emit_no_conflict_block`) + `sched.c:4803-4818`
// (`schedule_insns` deleting post-reload no-op self-moves): when a post-reload
// `kSched1` view carries an unpaired `(clobber (reg:DI r)) (REG_LIBCALL)` plus
// its two `(set (subreg:SI (reg:DI r) 1/0) ...) (REG_NO_CONFLICT ...)` writes
// (whose closing `(set (reg:DI r) (reg:DI r))` `REG_RETVAL` move was deleted
// before `sched2`), materialize that closing self-move on a local copy so
// `IsAchievable(..., kSched1)` computes exact pre-`sched2` `sched1` priorities.
bool TryExpandUnpairedNoConflictRetvalForSched1(absl::Span<const Insn> block, const RegFacts& facts,
                                                std::vector<Insn>* expanded_out,
                                                RegFacts* expanded_facts_out,
                                                std::vector<int>* expanded_to_orig_out) {
  const int n = static_cast<int>(block.size());
  if (n < 3) {
    return false;
  }
  std::vector<std::pair<int, int>> insertions;  // (insert_pos, target_regno)
  for (int c = 0; c < n; ++c) {
    const RtlPatternAndNotes& cpn = block[static_cast<size_t>(c)].pn;
    if (cpn.pattern.op != RtlOp::kClobber || cpn.pattern.children.size() != 1 ||
        cpn.pattern.children[0].op != RtlOp::kReg ||
        cpn.pattern.children[0].mode != RtlMachineMode::kDi) {
      continue;
    }
    const bool has_libcall = std::any_of(
        cpn.reg_notes.begin(), cpn.reg_notes.end(),
        [](const RtlRegNote& note) { return note.kind == RtlMachineMode::kRegLibcall; });
    if (!has_libcall) {
      continue;
    }
    const int target_r = cpn.pattern.children[0].RegNo();
    int word1_idx = -1;
    int word0_idx = -1;
    bool has_retval = false;
    for (int k = c + 1; k < n; ++k) {
      const RtlPatternAndNotes& kpn = block[static_cast<size_t>(k)].pn;
      if (std::any_of(kpn.reg_notes.begin(), kpn.reg_notes.end(), [](const RtlRegNote& note) {
            return note.kind == RtlMachineMode::kRegRetval;
          })) {
        has_retval = true;
        break;
      }
      if (kpn.pattern.op == RtlOp::kSet && kpn.pattern.children.size() == 2 &&
          kpn.pattern.SetDest().op == RtlOp::kSubreg &&
          kpn.pattern.SetDest().mode == RtlMachineMode::kSi &&
          !kpn.pattern.SetDest().children.empty() &&
          kpn.pattern.SetDest().children[0].op == RtlOp::kReg &&
          kpn.pattern.SetDest().children[0].mode == RtlMachineMode::kDi &&
          kpn.pattern.SetDest().children[0].RegNo() == target_r) {
        const bool has_nc = std::any_of(
            kpn.reg_notes.begin(), kpn.reg_notes.end(),
            [](const RtlRegNote& note) { return note.kind == RtlMachineMode::kRegNoConflict; });
        if (has_nc) {
          if (kpn.pattern.SetDest().int_val == 1 && word1_idx < 0) {
            word1_idx = k;
          } else if (kpn.pattern.SetDest().int_val == 0 && word0_idx < 0) {
            word0_idx = k;
          }
        }
      }
      if (word1_idx >= 0 && word0_idx >= 0) {
        break;
      }
    }
    if (!has_retval && word1_idx >= 0 && word0_idx >= 0) {
      insertions.emplace_back(std::max(word1_idx, word0_idx) + 1, target_r);
    }
  }
  if (insertions.empty()) {
    return false;
  }
  std::stable_sort(insertions.begin(), insertions.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  expanded_out->clear();
  expanded_out->reserve(static_cast<size_t>(n) + insertions.size());
  if (expanded_to_orig_out != nullptr) {
    expanded_to_orig_out->clear();
    expanded_to_orig_out->reserve(static_cast<size_t>(n) + insertions.size());
  }
  *expanded_facts_out = facts;
  size_t ins_cur = 0;
  for (int i = 0; i <= n; ++i) {
    while (ins_cur < insertions.size() && insertions[ins_cur].first == i) {
      const int target_r = insertions[ins_cur].second;
      Insn mv;
      mv.kind = Insn::Kind::kInsn;
      mv.luid = static_cast<int>(expanded_out->size());
      mv.pn.pattern = RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kDi, target_r),
                                       RtlExpr::MakeReg(RtlMachineMode::kDi, target_r));
      mv.pn.reg_notes.push_back(RtlRegNote{
          .kind = RtlMachineMode::kRegRetval,
          .list_op = RtlOp::kInsnList,
          .datum = RtlExpr::MakeNil(),
      });
      expanded_out->push_back(std::move(mv));
      if (expanded_to_orig_out != nullptr) {
        expanded_to_orig_out->push_back(-1);
      }
      expanded_facts_out->reg_n_sets[target_r] += 1;
      ++ins_cur;
    }
    if (i < n) {
      Insn copy = block[static_cast<size_t>(i)];
      copy.luid = static_cast<int>(expanded_out->size());
      expanded_out->push_back(std::move(copy));
      if (expanded_to_orig_out != nullptr) {
        expanded_to_orig_out->push_back(i);
      }
    }
  }
  return true;
}

}  // namespace detail

bool IsAchievable(absl::Span<const Insn> block, const RegFacts& facts, const MachineModel& mm,
                  Phase phase, const absl::btree_set<int>& live_at_end) {
  if (phase == Phase::kSched1) {
    std::vector<Insn> expanded;
    RegFacts expanded_facts;
    std::vector<int> exp_to_orig;
    if (TryExpandUnpairedNoConflictRetvalForSched1(block, facts, &expanded, &expanded_facts,
                                                   &exp_to_orig)) {
      if (InvertBlockOrder(expanded, expanded_facts, mm, phase, live_at_end).has_value()) {
        return true;
      }
      // `reload1.c` / `flow.c` deletes the no-op `(set (reg:DI r) (reg:DI r))`
      // `REG_RETVAL` insn, so its relative position among independent insns
      // scheduled after the last `REG_NO_CONFLICT` subword set and before `r`'s
      // first consumer is unobservable post-reload (`sched.c:schedule_block`).
      // If `Schedule(expanded, kSched1)` preserves every real insn in order and
      // only shifts synthetic `REG_RETVAL` insns, verify that exact placement.
      const DepGraph g_s1 = Analyze(expanded, expanded_facts, mm, phase);
      const ScheduleResult s1_res =
          Schedule(expanded, g_s1, expanded_facts, mm, phase, live_at_end);
      if (s1_res.order.size() == expanded.size()) {
        int next_real = 0;
        bool real_in_order = true;
        for (int idx : s1_res.order) {
          if (exp_to_orig[static_cast<size_t>(idx)] >= 0) {
            if (exp_to_orig[static_cast<size_t>(idx)] != next_real++) {
              real_in_order = false;
              break;
            }
          }
        }
        if (real_in_order && next_real == static_cast<int>(block.size())) {
          std::vector<Insn> repos;
          repos.reserve(expanded.size());
          for (int idx : s1_res.order) {
            Insn copy = expanded[static_cast<size_t>(idx)];
            copy.luid = static_cast<int>(repos.size());
            repos.push_back(std::move(copy));
          }
          return InvertBlockOrder(repos, expanded_facts, mm, phase, live_at_end).has_value();
        }
      }
      return false;
    }
  }
  return InvertBlockOrder(block, facts, mm, phase, live_at_end).has_value();
}

namespace {

Insn::Kind ClassifyModelInsnKind(const RtlExpr& pat) {
  auto is_call_expr = [](const RtlExpr& e) {
    return e.op == RtlOp::kCall || (e.op == RtlOp::kSet && e.SetSrc().op == RtlOp::kCall);
  };
  auto is_jump_expr = [](const RtlExpr& e) {
    return e.op == RtlOp::kSet && e.SetDest().op == RtlOp::kPc;
  };
  if (is_call_expr(pat)) {
    return Insn::Kind::kCall;
  }
  if (is_jump_expr(pat)) {
    return Insn::Kind::kJump;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      if (is_call_expr(ch)) {
        return Insn::Kind::kCall;
      }
      if (is_jump_expr(ch)) {
        return Insn::Kind::kJump;
      }
    }
  }
  return Insn::Kind::kInsn;
}

}  // namespace

ParsedSchedRegion ParseBlockForSchedModel(const RtlBasicBlock& blk, size_t block_idx, Phase phase) {
  ParsedSchedRegion region;
  struct RawEntry {
    bool is_note = false;
    RtlLoopNoteKind note_kind = RtlLoopNoteKind::kLoopBeg;
    Insn insn;
    size_t orig_idx = 0;
  };
  std::vector<RawEntry> entries;
  entries.reserve(blk.instructions.size());
  for (size_t i = 0; i < blk.instructions.size(); ++i) {
    RawEntry e;
    e.orig_idx = i;
    if (auto nk = ParseRtlLoopNoteKind(blk.instructions[i]); nk.has_value()) {
      e.is_note = true;
      e.note_kind = *nk;
      entries.push_back(std::move(e));
      continue;
    }
    auto parsed = ParsePatternAndNotes(blk.instructions[i]);
    if (!parsed.ok()) {
      region.valid = false;
      return region;
    }
    if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
        !parsed->has_explicit_call_fusage) {
      parsed->has_explicit_call_fusage = true;
      parsed->call_fusage = blk.rtl_insns[i].call_fusage;
    }
    e.insn.pn = std::move(*parsed);
    e.insn.kind = ClassifyModelInsnKind(e.insn.pn.pattern);
    entries.push_back(std::move(e));
  }

  size_t head = 0;
  size_t tail = entries.size();
  // Strip trailing notes BEFORE block-0 param-copy skip (`flow.c:468-472`, `sched.c:3197`).
  while (tail > head && entries[tail - 1].is_note) {
    region.suffix_orig_indices.push_back(entries[tail - 1].orig_idx);
    --tail;
  }
  std::reverse(region.suffix_orig_indices.begin(), region.suffix_orig_indices.end());

  if (phase == Phase::kSched1 && block_idx == 0) {
    // Mirrors `sched.c:3197-3214` + `combine.c` `NOTE_INSN_DELETED` stop before narrow
    // `QI`/`HI` `$a0-$a3` parameter copies (`gcc_context.cc:3094-3119`).
    absl::btree_set<int> word_copied_args;
    while (head + 1 < tail && !entries[head].is_note &&
           entries[head].insn.kind == Insn::Kind::kInsn &&
           entries[head].insn.pn.pattern.op == RtlOp::kSet &&
           entries[head].insn.pn.pattern.children.size() >= 2) {
      const RtlExpr& pat = entries[head].insn.pn.pattern;
      const RtlExpr* src = &pat.SetSrc();
      while ((src->op == RtlOp::kSubreg || src->op == RtlOp::kSignExtend ||
              src->op == RtlOp::kZeroExtend || src->op == RtlOp::kSignExtract ||
              src->op == RtlOp::kZeroExtract) &&
             !src->children.empty()) {
        src = &src->children[0];
      }
      if (src->op != RtlOp::kReg || src->RegNo() >= kMipsFirstPseudoRegister) {
        break;
      }
      if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kReg) {
        const RtlExpr& dst = pat.SetDest();
        const RtlExpr& s = pat.SetSrc();
        const int sr = s.RegNo();
        if ((s.mode == RtlMachineMode::kSi || s.mode == RtlMachineMode::kDi) &&
            sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast) {
          word_copied_args.insert(sr);
        } else if ((dst.mode == RtlMachineMode::kQi || dst.mode == RtlMachineMode::kHi) &&
                   (s.mode == RtlMachineMode::kQi || s.mode == RtlMachineMode::kHi) &&
                   sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast &&
                   !word_copied_args.contains(sr)) {
          break;
        }
      }
      region.prefix_orig_indices.push_back(entries[head].orig_idx);
      ++head;
    }
  }
  while (head < tail && entries[head].is_note) {
    region.prefix_orig_indices.push_back(entries[head].orig_idx);
    ++head;
  }

  std::vector<RtlLoopNoteKind> pending_barriers;
  int luid = 0;
  for (size_t i = head; i < tail; ++i) {
    if (entries[i].is_note) {
      const RtlLoopNoteKind nk = entries[i].note_kind;
      if (nk == RtlLoopNoteKind::kLoopBeg || nk == RtlLoopNoteKind::kLoopEnd) {
        pending_barriers.push_back(nk);
        region.has_mid_block_loop_barrier = true;
      } else {
        region.region_note_list.push_back(nk);
      }
      ++luid;
      continue;
    }
    Insn cur = std::move(entries[i].insn);
    cur.luid = luid++;
    cur.leading_loop_notes = std::move(pending_barriers);
    pending_barriers.clear();
    region.sched_block.push_back(std::move(cur));
    region.sched_orig_indices.push_back(entries[i].orig_idx);
  }
  return region;
}

}  // namespace rom_nom_nom::lifter::sched_model
