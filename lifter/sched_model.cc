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
namespace {

constexpr int kMipsArgPointerRegno = 0;
constexpr int kMipsAtRegno = 1;
constexpr int kMipsRetRegFirst = 2;
constexpr int kMipsRetRegLast = 3;
constexpr int kMipsArgRegFirst = 4;
constexpr int kMipsArgRegLast = 7;
constexpr int kMipsTemp1Regno = 12;
constexpr int kMipsCalleeSavedGprFirst = 16;
constexpr int kMipsCalleeSavedGprLast = 23;
constexpr int kMipsKernelRegFirst = 26;
constexpr int kMipsStackPointerRegno = 29;
constexpr int kMipsFramePointerRegno = 30;
constexpr int kMipsReturnAddrRegno = 31;
constexpr int kMipsFpRegFirst = 32;
constexpr int kMipsFpRetRegLast = 33;
constexpr int kMipsFpArgRegFirst = 44;
constexpr int kMipsFpArgRegLast = 47;
constexpr int kMipsCalleeSavedFprFirst = 52;
constexpr int kMipsFpRegLast = 63;
constexpr int kMipsMdRegFirst = 64;
constexpr int kMipsMdRegLast = 66;

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

Insn::Kind ClassifyInsnKind(const RtlExpr& pat) {
  if (IsCallPattern(pat)) {
    return Insn::Kind::kCall;
  }
  if (IsJumpPattern(pat)) {
    return Insn::Kind::kJump;
  }
  return Insn::Kind::kInsn;
}

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

}  // namespace

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

  ScheduleResult Run() {
    ScheduleResult res;
    if (n_ == 0) {
      return res;
    }

    // 1. Tail pinning & priority computation (`sched.c:3293-3377`).
    std::vector<int> ready;
    ready.reserve(static_cast<size_t>(n_));
    int last_tail = -1;
    int idx = n_ - 1;
    while (idx >= 0 && IsTailPinnedInsn(block_[static_cast<size_t>(idx)])) {
      ComputePriority(idx);
      if (last_tail < 0) {
        ready.push_back(idx);
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
          ready.push_back(idx);
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

    // Initial `SCHED_SORT(ready, n_ready, 1)` (`sched.c:3637`).
    SchedSort(&ready, /*old_ready=*/1);

    // 2. Main reverse list-scheduling loop (`sched.c:3658-3940`).
    int q_ptr = 0;
    int clock = 0;
    int sched_n_insns = 0;
    int n_ready = static_cast<int>(ready.size());
    std::vector<int> reverse_order;
    reverse_order.reserve(static_cast<size_t>(n_));

    while (sched_n_insns < n_) {
      q_ptr = (q_ptr + 1) & (kInsnQueueSize - 1);
      ++clock;

      // Pop LIFO bucket `insn_queue[q_ptr]` (`sched.c:3678-3686`).
      for (int q_insn : insn_queue_[static_cast<size_t>(q_ptr)]) {
        ready.push_back(q_insn);
      }
      insn_queue_[static_cast<size_t>(q_ptr)].clear();

      if (ready.empty()) {
        int stalls = 1;
        for (; stalls < kInsnQueueSize; ++stalls) {
          const int bucket = (q_ptr + stalls) & (kInsnQueueSize - 1);
          if (!insn_queue_[static_cast<size_t>(bucket)].empty()) {
            for (int q_insn : insn_queue_[static_cast<size_t>(bucket)]) {
              ready.push_back(q_insn);
            }
            insn_queue_[static_cast<size_t>(bucket)].clear();
            break;
          }
        }
        q_ptr = (q_ptr + stalls) & (kInsnQueueSize - 1);
        clock += stalls;
      }
      if (ready.empty()) {
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

      // Update backward `bb_live_regs` for `birthing_insn_p` (`sched.c:3764-3882`).
      if (phase_ == Phase::kSched1) {
        int cur = chosen;
        while (true) {
          UpdateBbLiveRegsBackward(cur);
          if (!sched_group_p_[static_cast<size_t>(cur)] || cur == 0) {
            break;
          }
          --cur;
        }
      }

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

  struct ForcedReplayResult {
    std::vector<Constraint> constraints;
    int last_tail = -1;
    bool had_conflict = false;
  };

  // Phase 2 of `InvertBlockOrder`: forced reverse replay tracking `last_cmp[u][v]`
  // across `qsort` (>1), `swap_sort` (==1), and no-sort (==0) steps (`sched.c:2373-2378`).
  ForcedReplayResult RunForcedReplay(bool continue_on_conflict = false) {
    ForcedReplayResult res;
    if (n_ == 0) {
      return res;
    }

    // 1. Tail pinning & priority computation (`sched.c:3293-3377`).
    std::vector<int> ready;
    ready.reserve(static_cast<size_t>(n_));
    int last_tail = -1;
    int idx = n_ - 1;
    while (idx >= 0 && IsTailPinnedInsn(block_[static_cast<size_t>(idx)])) {
      ComputePriority(idx);
      if (last_tail < 0) {
        ready.push_back(idx);
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
    res.last_tail = last_tail;
    for (; idx >= 0; --idx) {
      ComputePriority(idx);
      if (insn_ref_count_[static_cast<size_t>(idx)] == 0) {
        if (last_tail < 0) {
          ready.push_back(idx);
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

    struct CmpRecord {
      bool valid = false;
      int pri_u = 0;
      int pri_v = 0;
      int cls_u = 3;
      int cls_v = 3;
    };
    std::vector<CmpRecord> last_cmp(static_cast<size_t>(n_) * static_cast<size_t>(n_));
    auto cmp_at = [&](int u, int v) -> CmpRecord& {
      return last_cmp[static_cast<size_t>(u) * static_cast<size_t>(n_) + static_cast<size_t>(v)];
    };
    auto dep_class = [&](int cand, int last_sched) -> int {
      if (last_sched < 0) {
        return 3;
      }
      for (const Link& l : log_links_[static_cast<size_t>(last_sched)]) {
        if (l.target == cand) {
          if (InsnCost(cand, l.kind, last_sched) == 1) {
            return 3;
          }
          return (l.kind == DepKind::kTrue) ? 1 : 2;
        }
      }
      return 3;
    };
    auto record_pair = [&](int u, int v, int last_sched) {
      const int pu = insn_priority_[static_cast<size_t>(u)];
      const int pv = insn_priority_[static_cast<size_t>(v)];
      const int cu = dep_class(u, last_sched);
      const int cv = dep_class(v, last_sched);
      cmp_at(u, v) = CmpRecord{.valid = true, .pri_u = pu, .pri_v = pv, .cls_u = cu, .cls_v = cv};
      cmp_at(v, u) = CmpRecord{.valid = true, .pri_u = pv, .pri_v = pu, .cls_u = cv, .cls_v = cu};
    };
    auto record_sched_sort = [&](const std::vector<int>& r, int old_ready, int last_sched) {
      const int new_ready = static_cast<int>(r.size());
      if (new_ready - old_ready == 1 && new_ready >= 2) {
        // `swap_sort` (`sched.c:2434-2447`): only the newly appended element is compared
        // against existing ready elements; prior relative comparisons remain intact!
        const int w = r[static_cast<size_t>(new_ready - 1)];
        for (int i = 0; i < new_ready - 1; ++i) {
          record_pair(w, r[static_cast<size_t>(i)], last_sched);
        }
      } else if (new_ready - old_ready > 1) {
        // `qsort` (`sched.c:2377`): all pairs in `ready` are re-compared under `last_sched`.
        for (int i = 0; i < new_ready; ++i) {
          for (int j = i + 1; j < new_ready; ++j) {
            record_pair(r[static_cast<size_t>(i)], r[static_cast<size_t>(j)], last_sched);
          }
        }
      }
    };

    // Initial `SCHED_SORT(ready, n_ready, 1)` (`sched.c:3637`).
    record_sched_sort(ready, /*old_ready=*/1, /*last_sched=*/-1);

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
      q_ptr = (q_ptr + 1) & (kInsnQueueSize - 1);
      ++clock;

      for (int q_insn : insn_queue_[static_cast<size_t>(q_ptr)]) {
        ready.push_back(q_insn);
      }
      insn_queue_[static_cast<size_t>(q_ptr)].clear();

      if (ready.empty()) {
        int stalls = 1;
        for (; stalls < kInsnQueueSize; ++stalls) {
          const int bucket = (q_ptr + stalls) & (kInsnQueueSize - 1);
          if (!insn_queue_[static_cast<size_t>(bucket)].empty()) {
            for (int q_insn : insn_queue_[static_cast<size_t>(bucket)]) {
              ready.push_back(q_insn);
            }
            insn_queue_[static_cast<size_t>(bucket)].clear();
            break;
          }
        }
        q_ptr = (q_ptr + stalls) & (kInsnQueueSize - 1);
        clock += stalls;
      }
      if (ready.empty()) {
        res.had_conflict = true;
        break;
      }

      record_sched_sort(ready, n_ready, last_scheduled_insn_);

      // Simulate `ScheduleSelect` priority groups & `ActualHazard` queueing (`sched.c:2629-2703`).
      absl::btree_set<int> distinct_pris;
      for (int r : ready) {
        distinct_pris.insert(insn_priority_[static_cast<size_t>(r)]);
      }
      bool found_group = false;
      std::vector<int> winning_group;
      for (auto it = distinct_pris.rbegin(); it != distinct_pris.rend(); ++it) {
        const int grp_pri = *it;
        std::vector<int> unblocked;
        std::vector<int> next_ready;
        next_ready.reserve(ready.size());
        for (int r : ready) {
          if (insn_priority_[static_cast<size_t>(r)] == grp_pri) {
            const int cost = ActualHazard(r, clock);
            if (cost != 0) {
              QueueInsn(r, cost, q_ptr);
              continue;
            }
            unblocked.push_back(r);
          }
          next_ready.push_back(r);
        }
        ready = std::move(next_ready);
        if (!unblocked.empty()) {
          winning_group = std::move(unblocked);
          found_group = true;
          break;
        }
      }
      if (!found_group) {
        n_ready = 0;
        continue;
      }

      const int target = cursor;
      bool target_in_group = false;
      for (int r : winning_group) {
        if (r == target) {
          target_in_group = true;
          break;
        }
      }
      int chosen = target;
      if (!target_in_group) {
        res.had_conflict = true;
        if (!continue_on_conflict) {
          break;
        }
        chosen = winning_group[0];
        int best_ph = PotentialHazard(chosen);
        for (size_t wi = 1; wi < winning_group.size(); ++wi) {
          const int c = winning_group[wi];
          const int c_ph = PotentialHazard(c);
          if (c_ph > best_ph || (c_ph == best_ph && c > chosen)) {
            best_ph = c_ph;
            chosen = c;
          }
        }
      } else {
        const int target_ph = PotentialHazard(target);
        for (int c : winning_group) {
          if (c == target) {
            continue;
          }
          const int c_ph = PotentialHazard(c);
          if (c_ph > target_ph) {
            res.had_conflict = true;
          } else if (c_ph == target_ph) {
            const CmpRecord& rec = cmp_at(target, c);
            if (!rec.valid || (rec.pri_u == rec.pri_v && rec.cls_u == rec.cls_v)) {
              res.constraints.push_back(Constraint{.must_have_greater_luid = target, .than = c});
            } else if (rec.pri_u < rec.pri_v || (rec.pri_u == rec.pri_v && rec.cls_u < rec.cls_v)) {
              res.had_conflict = true;
            }
          }
        }
      }

      last_scheduled_insn_ = chosen;
      if (phase_ == Phase::kSched1) {
        int cur = chosen;
        while (true) {
          UpdateBbLiveRegsBackward(cur);
          if (!sched_group_p_[static_cast<size_t>(cur)] || cur == 0) {
            break;
          }
          --cur;
        }
      }

      ready.erase(std::remove(ready.begin(), ready.end(), chosen), ready.end());
      n_ready = static_cast<int>(ready.size());
      scheduled[static_cast<size_t>(chosen)] = true;
      if (chosen == cursor) {
        --cursor;
      }

      insn_priority_[static_cast<size_t>(chosen)] = kLaunchPriority;
      ScheduleInsn(chosen, &ready, clock, q_ptr);
      insn_priority_[static_cast<size_t>(chosen)] = kDonePriority;

      if (sched_group_p_[static_cast<size_t>(chosen)]) {
        int link = chosen;
        while (sched_group_p_[static_cast<size_t>(link)] && link > 0) {
          --link;
          insn_ref_count_[static_cast<size_t>(link)] = 0;
        }
        int g = chosen;
        while (sched_group_p_[static_cast<size_t>(g)] && g > 0) {
          --g;
          if (!continue_on_conflict && cursor != g) {
            res.had_conflict = true;
            return res;
          }
          scheduled[static_cast<size_t>(g)] = true;
          if (cursor == g) {
            --cursor;
          }
          ScheduleInsn(g, &ready, clock, q_ptr);
          insn_priority_[static_cast<size_t>(g)] = kDonePriority;
        }
      }
    }
    return res;
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

}  // namespace

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
  // Never short-circuit to identity `P` before checking `preferred_order` or `preferred_adjacent`.
  std::optional<std::vector<int>> fallback_verified_pref;
  if (has_valid_pref) {
    const VerifyOutcome pref_check = VerifyCandidateOrder(
        scheduled_block, preferred_order, target_graph, facts, mm, phase, live_at_end);
    if (pref_check.matches) {
      std::vector<int> pref_vec(preferred_order.begin(), preferred_order.end());
      if (SatisfiesAdjacentPairs(pref_vec, preferred_adjacent)) {
        return pref_vec;
      }
      fallback_verified_pref = std::move(pref_vec);
    }
  } else if (preferred_adjacent.empty()) {
    std::vector<int> identity(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      identity[static_cast<size_t>(i)] = i;
    }
    if (VerifyCandidateOrder(scheduled_block, identity, target_graph, facts, mm, phase, live_at_end)
            .matches) {
      return identity;
    }
  }

  // Phase 2: Forced reverse replay tracking `last_cmp[u][v]` across `qsort`, `swap_sort`,
  // and no-sort steps.
  ListScheduler replay_sched(scheduled_block, target_graph, facts, mm, phase, live_at_end);
  const ListScheduler::ForcedReplayResult replay = replay_sched.RunForcedReplay();

  // Build hard DAG edges `E_dag` (including tail-pinning suffix ordering).
  absl::btree_set<std::pair<int, int>> dag_edges;
  for (int cons = 0; cons < n; ++cons) {
    for (const Link& l : target_graph.log_links[static_cast<size_t>(cons)]) {
      dag_edges.insert({l.target, cons});
    }
  }
  if (replay.last_tail >= 0) {
    for (int u = 0; u < replay.last_tail; ++u) {
      dag_edges.insert({u, replay.last_tail});
    }
    for (int k = replay.last_tail; k + 1 < n; ++k) {
      dag_edges.insert({k, k + 1});
    }
  }
  if (phase == Phase::kSched2) {
    AddSched2NoopEndPinEdges(scheduled_block, &dag_edges);
  }

  std::vector<int> pref_rank(static_cast<size_t>(n));
  if (has_valid_pref) {
    for (int pos = 0; pos < n; ++pos) {
      pref_rank[static_cast<size_t>(preferred_order[static_cast<size_t>(pos)])] = pos;
    }
  } else {
    for (int i = 0; i < n; ++i) {
      pref_rank[static_cast<size_t>(i)] = i;
    }
  }
  std::vector<int> identity_rank(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    identity_rank[static_cast<size_t>(i)] = i;
  }

  // Build hard `SCHED_GROUP_P` chains.
  std::vector<int> hard_next(static_cast<size_t>(n), -1);
  std::vector<int> hard_prev(static_cast<size_t>(n), -1);
  for (int i = 1; i < n; ++i) {
    if (target_graph.sched_group_p[static_cast<size_t>(i)]) {
      hard_next[static_cast<size_t>(i - 1)] = i;
      hard_prev[static_cast<size_t>(i)] = i - 1;
    }
  }

  // Combine `E_dag` with acyclic Phase-2 LUID constraints (`c -> target`).
  absl::btree_set<std::pair<int, int>> constrained_edges = dag_edges;
  for (const Constraint& c : replay.constraints) {
    if (c.than < 0 || c.than >= n || c.must_have_greater_luid < 0 ||
        c.must_have_greater_luid >= n || c.than == c.must_have_greater_luid) {
      continue;
    }
    const std::pair<int, int> edge = {c.than, c.must_have_greater_luid};
    if (constrained_edges.contains(edge)) {
      continue;
    }
    constrained_edges.insert(edge);
    if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, pref_rank).has_value()) {
      constrained_edges.erase(edge);
    }
  }

  auto build_chains = [&](const absl::btree_set<std::pair<int, int>>& edges, bool use_soft_adj,
                          const std::vector<int>& ranks,
                          bool unpin_sched_group =
                              false) -> std::pair<std::vector<int>, std::vector<int>> {
    std::vector<int> nxt =
        unpin_sched_group ? std::vector<int>(static_cast<size_t>(n), -1) : hard_next;
    std::vector<int> prv =
        unpin_sched_group ? std::vector<int>(static_cast<size_t>(n), -1) : hard_prev;
    if (use_soft_adj) {
      for (const AdjacentPair& p : preferred_adjacent) {
        TryMergeChainPair(p.first, p.second, n, edges, &nxt, &prv, ranks);
      }
    }
    return {std::move(nxt), std::move(prv)};
  };

  // Phase 3: Preference-biased supernode Kahn + exact forward verification.
  auto try_solve_and_verify = [&](const absl::btree_set<std::pair<int, int>>& edges,
                                  bool use_soft_adj, const std::vector<int>& ranks,
                                  VerifyOutcome* out_outcome, std::vector<int>* out_cand = nullptr,
                                  bool unpin_sched_group =
                                      false) -> std::optional<std::vector<int>> {
    auto [nxt, prv] = build_chains(edges, use_soft_adj, ranks, unpin_sched_group);
    std::optional<std::vector<int>> cand = SolveSupernodeKahn(n, edges, nxt, prv, ranks);
    if (!cand.has_value()) {
      return std::nullopt;
    }
    if (out_cand != nullptr) {
      *out_cand = *cand;
    }
    VerifyOutcome outcome =
        VerifyCandidateOrder(scheduled_block, *cand, target_graph, facts, mm, phase, live_at_end);
    if (out_outcome != nullptr) {
      *out_outcome = outcome;
    }
    if (outcome.matches) {
      return cand;
    }
    return std::nullopt;
  };

  // Precompute memory-order indices and semantic (no-cap) memory edges for
  // `pending_lists_length > 32` (`sched.c:1740`) DAG-shift detection/relaxation.
  // Track `total_pending_mem_ops` (not just `mem_insns.size()`) because a
  // single `kSched2` instruction with `REG_DEAD (mem ...)` (`sched.c:2095-2102`)
  // or multi-`MEM` pattern contributes multiple entries to `pending_lists_length`.
  std::vector<int> mem_insns;
  int total_pending_mem_ops = 0;
  for (int i = 0; i < n; ++i) {
    const int ops = CountPendingListMemOpsForInsn(scheduled_block[static_cast<size_t>(i)], phase);
    if (ops > 0) {
      mem_insns.push_back(i);
      total_pending_mem_ops += ops;
    }
  }
  absl::btree_set<std::pair<int, int>> semantic_no_flush_cap_edges;
  std::optional<DepGraph> no_cap_graph;
  const bool check_pending_flush_artifacts = total_pending_mem_ops > 32;
  if (check_pending_flush_artifacts) {
    no_cap_graph = AnalyzeWithoutPendingListFlushCap(scheduled_block, facts, mm, phase);
    for (int cons = 0; cons < n; ++cons) {
      for (const Link& l : no_cap_graph->log_links[static_cast<size_t>(cons)]) {
        semantic_no_flush_cap_edges.insert({l.target, cons});
      }
    }
  }
  auto is_pure_pending_flush_artifact = [&](int u, int v) -> bool {
    return check_pending_flush_artifacts && !semantic_no_flush_cap_edges.contains({u, v}) &&
           (replay.last_tail < 0 || v < replay.last_tail);
  };

  VerifyOutcome phase3_outcome;
  std::vector<int> phase3_cand;
  absl::btree_set<std::pair<int, int>> pref_relaxed_call_edges;
  absl::btree_set<std::pair<int, int>> pref_relaxed_flush_edges;
  VerifyOutcome relaxed_outcome;
  absl::btree_set<std::pair<int, int>> no_cap_replay_extra;
  VerifyOutcome no_cap_relaxed_outcome;
  std::vector<int> no_cap_relaxed_cand;
  absl::btree_set<std::pair<int, int>> flush_only_replay_extra;
  VerifyOutcome flush_only_outcome;
  std::vector<int> flush_only_cand;
  bool has_flush_only_relaxed = false;
  std::vector<int> relaxed_cand;
  bool has_no_cap_relaxed = false;
  if (has_valid_pref) {
    bool wants_pending_flush_relax = false;
    for (const auto& [u, v] : dag_edges) {
      if (pref_rank[static_cast<size_t>(v)] < pref_rank[static_cast<size_t>(u)]) {
        if (phase == Phase::kSched2 &&
            IsPureSched1704CallArtifactEdge(scheduled_block, target_graph, u, v, mm)) {
          pref_relaxed_call_edges.insert({u, v});
        } else if (is_pure_pending_flush_artifact(u, v)) {
          pref_relaxed_flush_edges.insert({u, v});
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
      for (const auto& [u, v] : dag_edges) {
        if (is_pure_pending_flush_artifact(u, v)) {
          pref_relaxed_flush_edges.insert({u, v});
        }
      }
      if (no_cap_graph.has_value()) {
        ListScheduler no_cap_sched(scheduled_block, *no_cap_graph, facts, mm, phase, live_at_end);
        const ListScheduler::ForcedReplayResult no_cap_replay = no_cap_sched.RunForcedReplay();
        absl::btree_set<std::pair<int, int>> pref_relaxed_union = pref_relaxed_call_edges;
        pref_relaxed_union.insert(pref_relaxed_flush_edges.begin(), pref_relaxed_flush_edges.end());
        absl::btree_set<std::pair<int, int>> no_cap_edges;
        for (const auto& e : dag_edges) {
          if (!pref_relaxed_union.contains(e)) {
            no_cap_edges.insert(e);
          }
        }
        for (const Constraint& c : no_cap_replay.constraints) {
          if (c.than < 0 || c.than >= n || c.must_have_greater_luid < 0 ||
              c.must_have_greater_luid >= n || c.than == c.must_have_greater_luid) {
            continue;
          }
          const std::pair<int, int> edge = {c.than, c.must_have_greater_luid};
          if (no_cap_edges.contains(edge)) {
            continue;
          }
          no_cap_edges.insert(edge);
          if (!SolveSupernodeKahn(n, no_cap_edges, hard_next, hard_prev, pref_rank).has_value()) {
            no_cap_edges.erase(edge);
          } else {
            no_cap_replay_extra.insert(edge);
          }
        }
        has_no_cap_relaxed = true;
        if (auto verified = try_solve_and_verify(no_cap_edges, /*use_soft_adj=*/true, pref_rank,
                                                 &no_cap_relaxed_outcome, &no_cap_relaxed_cand);
            verified.has_value()) {
          if (SatisfiesAdjacentPairs(*verified, preferred_adjacent)) {
            return verified;
          }
          if (!fallback_verified_pref.has_value()) {
            fallback_verified_pref = std::move(verified);
          }
        }
        // When `preferred_order` also requested relaxing `sched.c:1704` call edges,
        // `no_cap_graph` still contains those call edges; also test dropping ONLY
        // `pref_relaxed_flush_edges` so an unsolvable call hoist cannot mask flush dissolution.
        if (!pref_relaxed_call_edges.empty()) {
          absl::btree_set<std::pair<int, int>> flush_only_edges;
          for (const auto& e : dag_edges) {
            if (!pref_relaxed_flush_edges.contains(e)) {
              flush_only_edges.insert(e);
            }
          }
          for (const auto& e : no_cap_replay_extra) {
            if (!flush_only_edges.contains(e)) {
              flush_only_edges.insert(e);
              if (!SolveSupernodeKahn(n, flush_only_edges, hard_next, hard_prev, pref_rank)
                       .has_value()) {
                flush_only_edges.erase(e);
              } else {
                flush_only_replay_extra.insert(e);
              }
            }
          }
          has_flush_only_relaxed = true;
          if (auto verified =
                  try_solve_and_verify(flush_only_edges, /*use_soft_adj=*/true, pref_rank,
                                       &flush_only_outcome, &flush_only_cand);
              verified.has_value()) {
            if (SatisfiesAdjacentPairs(*verified, preferred_adjacent)) {
              return verified;
            }
            if (!fallback_verified_pref.has_value()) {
              fallback_verified_pref = std::move(verified);
            }
          }
        }
      }
    }
    absl::btree_set<std::pair<int, int>> pref_relaxed_all_edges = pref_relaxed_call_edges;
    pref_relaxed_all_edges.insert(pref_relaxed_flush_edges.begin(), pref_relaxed_flush_edges.end());
    if (!pref_relaxed_all_edges.empty()) {
      absl::btree_set<std::pair<int, int>> relaxed_edges = constrained_edges;
      for (const auto& edge : pref_relaxed_all_edges) {
        relaxed_edges.erase(edge);
      }
      if (auto verified = try_solve_and_verify(relaxed_edges, /*use_soft_adj=*/true, pref_rank,
                                               &relaxed_outcome, &relaxed_cand);
          verified.has_value()) {
        if (SatisfiesAdjacentPairs(*verified, preferred_adjacent)) {
          return verified;
        }
        if (!fallback_verified_pref.has_value()) {
          fallback_verified_pref = std::move(verified);
        }
      }
    }
  }
  absl::btree_set<std::pair<int, int>> pref_relaxed_all_edges = pref_relaxed_call_edges;
  pref_relaxed_all_edges.insert(pref_relaxed_flush_edges.begin(), pref_relaxed_flush_edges.end());
  bool only_refine_pref_relaxed = false;
  if (auto verified = try_solve_and_verify(constrained_edges, /*use_soft_adj=*/true, pref_rank,
                                           &phase3_outcome, &phase3_cand);
      verified.has_value()) {
    if (pref_relaxed_all_edges.empty() && SatisfiesAdjacentPairs(*verified, preferred_adjacent)) {
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
    if (!pref_relaxed_all_edges.empty()) {
      only_refine_pref_relaxed = true;
    }
  }

  // Phase 4: Bounded deterministic conflict-driven refinement (`<= 32` refinements, `<= 64`
  // states). Note: `pending_lists_length > 32` (`sched.c:1740, 1748`) is
  // all-or-nothing and handled as a group via `all_pending_flush_edges` /
  // `ComputePendingFlushDissolutionExtraEdges` rather than single-edge drops
  // (which would exhaust `kMaxSearchStates` on a 35-edge flush hub).
  auto is_pure_call_used_bug_artifact = [&](int call_idx, int insn_idx) -> bool {
    return IsPureSched1704CallArtifactEdge(scheduled_block, target_graph, call_idx, insn_idx, mm);
  };

  struct SearchState {
    absl::btree_set<std::pair<int, int>> extra_edges;
    absl::btree_set<std::pair<int, int>> dropped_dag_edges;
    bool use_soft_adj = true;
    bool use_pref_rank = true;
    bool unpin_sched_group = false;
  };
  using StateKey = std::tuple<absl::btree_set<std::pair<int, int>>,
                              absl::btree_set<std::pair<int, int>>, bool, bool, bool>;
  absl::btree_set<StateKey> visited;
  // Keep preference-guided (`use_pref_rank == true`) search states strictly
  // ahead of `identity_rank` (`use_pref_rank == false`) fallback states so an
  // early `identity_rank` match never preempts multi-conflict refinement of
  // `preferred_order` (`sched.c:schedule_block`).
  std::deque<SearchState> pref_worklist;
  std::deque<SearchState> fallback_worklist;

  auto enqueue = [&](SearchState st) {
    const bool is_primary_pref = st.use_pref_rank && !st.unpin_sched_group &&
                                 (preferred_adjacent.empty() || st.use_soft_adj);
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
  };

  auto enqueue_call_bug_relaxations = [&](const SearchState& base, int focus_insn) {
    if (focus_insn < 0 || focus_insn >= n) {
      return;
    }
    for (const Link& l : target_graph.log_links[static_cast<size_t>(focus_insn)]) {
      if (is_pure_call_used_bug_artifact(l.target, focus_insn)) {
        const std::pair<int, int> art_edge = {l.target, focus_insn};
        if (!base.dropped_dag_edges.contains(art_edge)) {
          SearchState nxt = base;
          nxt.dropped_dag_edges.insert(art_edge);
          nxt.extra_edges.erase(art_edge);
          nxt.extra_edges.insert({focus_insn, l.target});
          enqueue(std::move(nxt));
        }
      }
    }
  };

  auto enqueue_pending_flush_dissolution =
      [&](const SearchState& base, const absl::btree_set<std::pair<int, int>>& edges,
          absl::Span<const int> cand, const std::vector<int>& ranks) {
        if (!check_pending_flush_artifacts || base.dropped_dag_edges.empty() ||
            static_cast<int>(cand.size()) != n) {
          return;
        }
        const bool gate_by_pref = has_valid_pref && base.use_pref_rank;
        for (auto& extra : ComputePendingFlushDissolutionExtraEdges(
                 scheduled_block, phase, base.extra_edges, edges, cand, ranks, preferred_adjacent,
                 base.use_soft_adj, gate_by_pref)) {
          SearchState nxt = base;
          nxt.extra_edges = std::move(extra);
          enqueue(std::move(nxt));
        }
      };

  // Compute extra edges relative to `dag_edges` for the Phase-2 constrained root.
  absl::btree_set<std::pair<int, int>> replay_extra;
  for (const auto& e : constrained_edges) {
    if (!dag_edges.contains(e)) {
      replay_extra.insert(e);
    }
  }

  // Seed worklist: (1) Phase-2 root + first conflict edge / call-bug relaxation,
  // (2) clean `E_dag` root, (3) uncontracted soft-adj root if `preferred_adjacent` was non-empty.
  if (!pref_relaxed_all_edges.empty()) {
    if (has_no_cap_relaxed) {
      if (no_cap_relaxed_outcome.dag_differs && !no_cap_relaxed_cand.empty()) {
        SearchState st;
        st.extra_edges = no_cap_replay_extra;
        st.dropped_dag_edges = pref_relaxed_all_edges;
        st.use_soft_adj = true;
        st.use_pref_rank = true;
        absl::btree_set<std::pair<int, int>> st_edges;
        for (const auto& e : dag_edges) {
          if (!st.dropped_dag_edges.contains(e)) {
            st_edges.insert(e);
          }
        }
        for (const auto& e : st.extra_edges) {
          st_edges.insert(e);
        }
        enqueue_pending_flush_dissolution(st, st_edges, no_cap_relaxed_cand, pref_rank);
      }
      if (!only_refine_pref_relaxed && !no_cap_relaxed_cand.empty()) {
        // When no verified preimage exists yet, seed `fallback_worklist`
        // (`use_pref_rank = false`) with un-gated flush dissolution from the
        // clean dropped-flush DAG so multi-store cascades dissolve without
        // disturbing `pref_worklist` preference alignment.
        SearchState st_clean_id;
        st_clean_id.dropped_dag_edges = pref_relaxed_all_edges;
        st_clean_id.use_soft_adj = true;
        st_clean_id.use_pref_rank = false;
        absl::btree_set<std::pair<int, int>> clean_st_edges;
        for (const auto& e : dag_edges) {
          if (!st_clean_id.dropped_dag_edges.contains(e)) {
            clean_st_edges.insert(e);
          }
        }
        enqueue_pending_flush_dissolution(st_clean_id, clean_st_edges, no_cap_relaxed_cand,
                                          identity_rank);
        enqueue(st_clean_id);
      }
      if (no_cap_relaxed_outcome.target >= 0 && no_cap_relaxed_outcome.competitor >= 0) {
        SearchState st;
        st.extra_edges = no_cap_replay_extra;
        st.dropped_dag_edges = pref_relaxed_all_edges;
        st.extra_edges.insert({no_cap_relaxed_outcome.competitor, no_cap_relaxed_outcome.target});
        st.use_soft_adj = true;
        st.use_pref_rank = true;
        enqueue(std::move(st));
      }
      if (has_flush_only_relaxed && !pref_relaxed_flush_edges.empty()) {
        if (flush_only_outcome.dag_differs && !flush_only_cand.empty()) {
          SearchState st;
          st.extra_edges = flush_only_replay_extra;
          st.dropped_dag_edges = pref_relaxed_flush_edges;
          st.use_soft_adj = true;
          st.use_pref_rank = true;
          absl::btree_set<std::pair<int, int>> st_edges;
          for (const auto& e : dag_edges) {
            if (!st.dropped_dag_edges.contains(e)) {
              st_edges.insert(e);
            }
          }
          for (const auto& e : st.extra_edges) {
            st_edges.insert(e);
          }
          enqueue_pending_flush_dissolution(st, st_edges, flush_only_cand, pref_rank);
        }
        if (flush_only_outcome.target >= 0 && flush_only_outcome.competitor >= 0) {
          SearchState st;
          st.extra_edges = flush_only_replay_extra;
          st.dropped_dag_edges = pref_relaxed_flush_edges;
          st.extra_edges.insert({flush_only_outcome.competitor, flush_only_outcome.target});
          st.use_soft_adj = true;
          st.use_pref_rank = true;
          enqueue(std::move(st));
        }
        SearchState clean_flush_relaxed;
        clean_flush_relaxed.dropped_dag_edges = pref_relaxed_flush_edges;
        clean_flush_relaxed.use_soft_adj = true;
        clean_flush_relaxed.use_pref_rank = true;
        enqueue(std::move(clean_flush_relaxed));
      }
    }
    if (relaxed_outcome.target >= 0 && relaxed_outcome.competitor >= 0) {
      SearchState st;
      st.extra_edges = replay_extra;
      for (const auto& edge : pref_relaxed_all_edges) {
        st.extra_edges.erase(edge);
      }
      st.dropped_dag_edges = pref_relaxed_all_edges;
      st.extra_edges.insert({relaxed_outcome.competitor, relaxed_outcome.target});
      st.use_soft_adj = true;
      st.use_pref_rank = true;
      enqueue(std::move(st));
    }
    SearchState clean_relaxed;
    clean_relaxed.dropped_dag_edges = pref_relaxed_all_edges;
    clean_relaxed.use_soft_adj = true;
    clean_relaxed.use_pref_rank = true;
    enqueue(std::move(clean_relaxed));
    if (!preferred_adjacent.empty()) {
      SearchState no_adj_relaxed;
      no_adj_relaxed.dropped_dag_edges = pref_relaxed_all_edges;
      no_adj_relaxed.use_soft_adj = false;
      no_adj_relaxed.use_pref_rank = true;
      enqueue(std::move(no_adj_relaxed));
    }
  }
  if (!only_refine_pref_relaxed) {
    if (phase3_outcome.target >= 0 && phase3_outcome.competitor >= 0) {
      SearchState st;
      st.extra_edges = replay_extra;
      st.extra_edges.insert({phase3_outcome.competitor, phase3_outcome.target});
      st.use_soft_adj = true;
      st.use_pref_rank = true;
      enqueue(std::move(st));
      SearchState base_replay{
          .extra_edges = replay_extra, .use_soft_adj = true, .use_pref_rank = true};
      enqueue_call_bug_relaxations(base_replay, phase3_outcome.competitor);
      enqueue_call_bug_relaxations(base_replay, phase3_outcome.target);
      SearchState base_clean{.extra_edges = {}, .use_soft_adj = true, .use_pref_rank = true};
      enqueue_call_bug_relaxations(base_clean, phase3_outcome.competitor);
      enqueue_call_bug_relaxations(base_clean, phase3_outcome.target);
    }
    if (phase3_outcome.dag_differs && total_pending_mem_ops > 32) {
      SearchState st;
      st.extra_edges = replay_extra;
      for (size_t j = 0; j + 1 < mem_insns.size(); ++j) {
        st.extra_edges.insert({mem_insns[j], mem_insns[j + 1]});
      }
      st.use_soft_adj = true;
      st.use_pref_rank = true;
      enqueue(std::move(st));
    }
    // Mirrors `sched.c:1740, 1748` (`pending_lists_length > 32`) for BOTH
    // `kSched1` and `kSched2` even without a valid `preferred_order`: when
    // Phase 3 failed on a `> 32`-memory block containing pure flush-cap
    // artifact edges, seed Phase 4 with `no_cap` states (dropping all pure
    // flush edges) and flush-dissolution / store-hub transfer candidates.
    if (check_pending_flush_artifacts && pref_relaxed_flush_edges.empty() &&
        no_cap_graph.has_value()) {
      absl::btree_set<std::pair<int, int>> all_pending_flush_edges;
      for (const auto& [u, v] : dag_edges) {
        if (is_pure_pending_flush_artifact(u, v)) {
          all_pending_flush_edges.insert({u, v});
        }
      }
      if (!all_pending_flush_edges.empty()) {
        ListScheduler nc_sched(scheduled_block, *no_cap_graph, facts, mm, phase, live_at_end);
        const ListScheduler::ForcedReplayResult nc_replay = nc_sched.RunForcedReplay();
        absl::btree_set<std::pair<int, int>> nc_edges;
        for (const auto& e : dag_edges) {
          if (!all_pending_flush_edges.contains(e)) {
            nc_edges.insert(e);
          }
        }
        absl::btree_set<std::pair<int, int>> nc_extra;
        for (const Constraint& c : nc_replay.constraints) {
          if (c.than < 0 || c.than >= n || c.must_have_greater_luid < 0 ||
              c.must_have_greater_luid >= n || c.than == c.must_have_greater_luid) {
            continue;
          }
          const std::pair<int, int> edge = {c.than, c.must_have_greater_luid};
          if (nc_edges.contains(edge)) {
            continue;
          }
          nc_edges.insert(edge);
          if (!SolveSupernodeKahn(n, nc_edges, hard_next, hard_prev, identity_rank).has_value()) {
            nc_edges.erase(edge);
          } else {
            nc_extra.insert(edge);
          }
        }
        absl::btree_set<std::pair<int, int>> clean_edges;
        for (const auto& e : dag_edges) {
          if (!all_pending_flush_edges.contains(e)) {
            clean_edges.insert(e);
          }
        }
        SearchState nc_clean;
        nc_clean.dropped_dag_edges = all_pending_flush_edges;
        nc_clean.use_soft_adj = true;
        // When `preferred_order` did not request inverting any flush edge,
        // keep no-cap flush-dissolution states in `fallback_worklist`
        // (`use_pref_rank = false`) so they never preempt preference-guided
        // conflict refinement when a preference-preserving preimage exists.
        nc_clean.use_pref_rank = !has_valid_pref;
        const std::vector<int>& nc_ranks = nc_clean.use_pref_rank ? pref_rank : identity_rank;
        if (!phase3_cand.empty()) {
          enqueue_pending_flush_dissolution(nc_clean, clean_edges, phase3_cand, nc_ranks);
        }
        enqueue(nc_clean);
        SearchState nc_base;
        nc_base.extra_edges = nc_extra;
        nc_base.dropped_dag_edges = all_pending_flush_edges;
        nc_base.use_soft_adj = true;
        nc_base.use_pref_rank = !has_valid_pref;
        if (!phase3_cand.empty()) {
          enqueue_pending_flush_dissolution(nc_base, nc_edges, phase3_cand, nc_ranks);
        }
        enqueue(nc_base);
      }
    }
    for (auto& unpin_extra : CollectUnpinSchedGroupEdgeSets(n, target_graph, dag_edges)) {
      SearchState st;
      st.extra_edges = std::move(unpin_extra);
      st.use_soft_adj = true;
      st.use_pref_rank = false;
      st.unpin_sched_group = true;
      enqueue(std::move(st));
    }
    enqueue(SearchState{.extra_edges = {}, .use_soft_adj = true, .use_pref_rank = true});
    if (!preferred_adjacent.empty()) {
      enqueue(
          SearchState{.extra_edges = replay_extra, .use_soft_adj = false, .use_pref_rank = true});
      enqueue(SearchState{.extra_edges = {}, .use_soft_adj = false, .use_pref_rank = true});
    }
    if (has_valid_pref) {
      enqueue(
          SearchState{.extra_edges = replay_extra, .use_soft_adj = true, .use_pref_rank = false});
      enqueue(SearchState{.extra_edges = {}, .use_soft_adj = false, .use_pref_rank = false});
    }
  }

  int refinements = 0;
  int pref_refinements = 0;
  constexpr int kMaxPrefRefinements = kMaxConflictRefinements - 6;
  while (refinements < kMaxConflictRefinements) {
    SearchState cur;
    if (!pref_worklist.empty() &&
        (fallback_worklist.empty() || pref_refinements < kMaxPrefRefinements)) {
      cur = std::move(pref_worklist.front());
      pref_worklist.pop_front();
      ++pref_refinements;
    } else if (!fallback_worklist.empty()) {
      cur = std::move(fallback_worklist.front());
      fallback_worklist.pop_front();
    } else {
      break;
    }
    ++refinements;

    absl::btree_set<std::pair<int, int>> edges;
    for (const auto& e : dag_edges) {
      if (!cur.dropped_dag_edges.contains(e)) {
        edges.insert(e);
      }
    }
    for (const auto& e : cur.extra_edges) {
      edges.insert(e);
    }
    const std::vector<int>& ranks = cur.use_pref_rank ? pref_rank : identity_rank;
    VerifyOutcome outcome;
    std::vector<int> cur_cand;
    if (auto verified = try_solve_and_verify(edges, cur.use_soft_adj, ranks, &outcome, &cur_cand,
                                             cur.unpin_sched_group);
        verified.has_value()) {
      if (SatisfiesAdjacentPairs(*verified, preferred_adjacent) ||
          (pref_worklist.empty() && !cur.use_soft_adj)) {
        return verified;
      }
      if (!fallback_verified_pref.has_value()) {
        fallback_verified_pref = std::move(verified);
      }
    }

    if (total_pending_mem_ops > 32) {
      if (cur.dropped_dag_edges.empty() && outcome.dag_differs) {
        SearchState nxt = cur;
        for (size_t j = 0; j + 1 < mem_insns.size(); ++j) {
          nxt.extra_edges.insert({mem_insns[j], mem_insns[j + 1]});
        }
        enqueue(std::move(nxt));
      } else if (!cur.dropped_dag_edges.empty()) {
        enqueue_pending_flush_dissolution(cur, edges, cur_cand, ranks);
      }
    }
    if (outcome.target >= 0 && outcome.competitor >= 0) {
      const std::pair<int, int> conflict_edge = {outcome.competitor, outcome.target};
      const std::pair<int, int> reverse_edge = {outcome.target, outcome.competitor};
      if (cur.extra_edges.contains(reverse_edge)) {
        SearchState nxt = cur;
        nxt.extra_edges.erase(reverse_edge);
        nxt.extra_edges.insert(conflict_edge);
        enqueue(std::move(nxt));
      } else if (!edges.contains(conflict_edge)) {
        SearchState nxt = cur;
        nxt.extra_edges.insert(conflict_edge);
        enqueue(std::move(nxt));
      }
      enqueue_call_bug_relaxations(cur, outcome.competitor);
      enqueue_call_bug_relaxations(cur, outcome.target);
    }
  }

  if (fallback_verified_pref.has_value()) {
    return fallback_verified_pref;
  }
  return std::nullopt;
}

namespace {

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
                                                std::vector<int>* expanded_to_orig_out = nullptr) {
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

}  // namespace

std::vector<int> ScheduleSched1ViewCoreOrder(const Sched1ProjectionView& view,
                                             const MachineModel& mm) {
  std::vector<Insn> expanded;
  RegFacts expanded_facts;
  std::vector<int> exp_to_orig;
  if (TryExpandUnpairedNoConflictRetvalForSched1(view.core_insns, view.facts_s1, &expanded,
                                                 &expanded_facts, &exp_to_orig)) {
    const DepGraph g_s1 = Analyze(expanded, expanded_facts, mm, Phase::kSched1);
    const ScheduleResult s1_res =
        Schedule(expanded, g_s1, expanded_facts, mm, Phase::kSched1, view.live_at_end_s1);
    std::vector<int> mapped;
    mapped.reserve(view.core_insns.size());
    for (int exp_idx : s1_res.order) {
      if (exp_idx >= 0 && static_cast<size_t>(exp_idx) < exp_to_orig.size() &&
          exp_to_orig[static_cast<size_t>(exp_idx)] >= 0) {
        mapped.push_back(exp_to_orig[static_cast<size_t>(exp_idx)]);
      }
    }
    return mapped;
  }
  const DepGraph g_s1 = Analyze(view.core_insns, view.facts_s1, mm, Phase::kSched1);
  return Schedule(view.core_insns, g_s1, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1)
      .order;
}

std::vector<int> ScheduleClosestSched1CoreOrder(const Sched1ProjectionView& view,
                                                const MachineModel& mm) {
  auto solve_closest = [&](absl::Span<const Insn> insns,
                           const RegFacts& facts) -> std::vector<int> {
    const int n = static_cast<int>(insns.size());
    if (n <= 1) {
      return n == 1 ? std::vector<int>{0} : std::vector<int>{};
    }
    const DepGraph target_graph = Analyze(insns, facts, mm, Phase::kSched1);
    ListScheduler replay_sched(insns, target_graph, facts, mm, Phase::kSched1, view.live_at_end_s1);
    const ListScheduler::ForcedReplayResult replay =
        replay_sched.RunForcedReplay(/*continue_on_conflict=*/true);

    absl::btree_set<std::pair<int, int>> dag_edges;
    for (int cons = 0; cons < n; ++cons) {
      for (const Link& l : target_graph.log_links[static_cast<size_t>(cons)]) {
        dag_edges.insert({l.target, cons});
      }
    }
    if (replay.last_tail >= 0) {
      for (int u = 0; u < replay.last_tail; ++u) {
        dag_edges.insert({u, replay.last_tail});
      }
      for (int k = replay.last_tail; k + 1 < n; ++k) {
        dag_edges.insert({k, k + 1});
      }
    }
    std::vector<int> hard_next(static_cast<size_t>(n), -1);
    std::vector<int> hard_prev(static_cast<size_t>(n), -1);
    for (int i = 1; i < n; ++i) {
      if (target_graph.sched_group_p[static_cast<size_t>(i)]) {
        hard_next[static_cast<size_t>(i - 1)] = i;
        hard_prev[static_cast<size_t>(i)] = i - 1;
      }
    }
    std::vector<int> identity_rank(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      identity_rank[static_cast<size_t>(i)] = i;
    }

    absl::btree_set<std::pair<int, int>> constrained_edges = dag_edges;
    for (const Constraint& c : replay.constraints) {
      if (c.than < 0 || c.than >= n || c.must_have_greater_luid < 0 ||
          c.must_have_greater_luid >= n || c.than == c.must_have_greater_luid) {
        continue;
      }
      const std::pair<int, int> edge = {c.than, c.must_have_greater_luid};
      if (constrained_edges.contains(edge)) {
        continue;
      }
      constrained_edges.insert(edge);
      if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
               .has_value()) {
        constrained_edges.erase(edge);
      }
    }
    const std::optional<std::vector<int>> best_pre =
        SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank);
    if (!best_pre.has_value()) {
      return Schedule(insns, target_graph, facts, mm, Phase::kSched1, view.live_at_end_s1).order;
    }
    std::vector<Insn> permuted;
    permuted.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      Insn copy = insns[static_cast<size_t>((*best_pre)[static_cast<size_t>(pos)])];
      copy.luid = pos;
      permuted.push_back(std::move(copy));
    }
    const DepGraph g_pre = Analyze(permuted, facts, mm, Phase::kSched1);
    const ScheduleResult res =
        Schedule(permuted, g_pre, facts, mm, Phase::kSched1, view.live_at_end_s1);
    std::vector<int> mapped;
    mapped.reserve(static_cast<size_t>(n));
    for (int idx : res.order) {
      mapped.push_back((*best_pre)[static_cast<size_t>(idx)]);
    }
    return mapped;
  };

  std::vector<Insn> expanded;
  RegFacts expanded_facts;
  std::vector<int> exp_to_orig;
  if (TryExpandUnpairedNoConflictRetvalForSched1(view.core_insns, view.facts_s1, &expanded,
                                                 &expanded_facts, &exp_to_orig)) {
    const std::vector<int> exp_order = solve_closest(expanded, expanded_facts);
    std::vector<int> mapped;
    mapped.reserve(view.core_insns.size());
    for (int exp_idx : exp_order) {
      if (exp_idx >= 0 && static_cast<size_t>(exp_idx) < exp_to_orig.size() &&
          exp_to_orig[static_cast<size_t>(exp_idx)] >= 0) {
        mapped.push_back(exp_to_orig[static_cast<size_t>(exp_idx)]);
      }
    }
    return mapped;
  }
  return solve_closest(view.core_insns, view.facts_s1);
}

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

// =============================================================================
// 9. ProjectPostReloadBlockToSched1View (Read-Only Stage 3 Helper)
// =============================================================================

namespace {

struct StackSlotKey {
  int base_regno = -1;
  int64_t offset = 0;
  RtlMachineMode mode = RtlMachineMode::kVoid;

  bool operator<(const StackSlotKey& o) const {
    if (base_regno != o.base_regno) return base_regno < o.base_regno;
    if (offset != o.offset) return offset < o.offset;
    return static_cast<int>(mode) < static_cast<int>(o.mode);
  }
};

struct StackSlotAccess {
  bool is_store = false;
  int regno = -1;
  StackSlotKey key;
};

bool ParseStackBaseOffset(const RtlExpr& addr, int* base_regno, int64_t* offset,
                          bool allow_fp_base = true) {
  auto is_stack_base = [allow_fp_base](int r) {
    return r == kMipsStackPointerRegno || (allow_fp_base && r == kMipsFramePointerRegno);
  };
  if (addr.op == RtlOp::kReg) {
    const int r = addr.RegNo();
    if (is_stack_base(r)) {
      *base_regno = r;
      *offset = 0;
      return true;
    }
    return false;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      addr.children[1].op == RtlOp::kConstInt) {
    const int r = addr.children[0].RegNo();
    if (is_stack_base(r)) {
      *base_regno = r;
      *offset = addr.children[1].int_val;
      return true;
    }
  }
  return false;
}

std::optional<StackSlotAccess> TryParsePlainStackSlotMove(const Insn& insn,
                                                          bool allow_fp_base = true) {
  if (insn.kind != Insn::Kind::kInsn || insn.pn.pattern.op != RtlOp::kSet) {
    return std::nullopt;
  }
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual) {
      return std::nullopt;
    }
  }
  const RtlExpr& dst = insn.pn.pattern.SetDest();
  const RtlExpr& src = insn.pn.pattern.SetSrc();
  if (dst.op == RtlOp::kMem && src.op == RtlOp::kReg && !dst.flags.volatil &&
      !dst.flags.in_struct && !dst.flags.unchanging && !dst.children.empty()) {
    int base = -1;
    int64_t off = 0;
    if (ParseStackBaseOffset(dst.children[0], &base, &off, allow_fp_base)) {
      return StackSlotAccess{.is_store = true, .regno = src.RegNo(), .key = {base, off, dst.mode}};
    }
  }
  if (src.op == RtlOp::kMem && dst.op == RtlOp::kReg && !src.flags.volatil &&
      !src.flags.in_struct && !src.flags.unchanging && !src.children.empty()) {
    int base = -1;
    int64_t off = 0;
    if (ParseStackBaseOffset(src.children[0], &base, &off, allow_fp_base)) {
      return StackSlotAccess{.is_store = false, .regno = dst.RegNo(), .key = {base, off, src.mode}};
    }
  }
  return std::nullopt;
}

// Mirrors `config/mips/mips.h:1186-1209, 1734-1749` and `global.c:308-325`:
// `$fp` (reg 30, `FRAME_POINTER_REGNUM`) has `FIXED_REGISTERS[30] == 0` and
// `CALL_USED_REGISTERS[30] == 0`, so when `!frame_pointer_needed`, `global_alloc`
// allocates `$fp` as an ordinary callee-saved GPR.
bool IsFixedOrSpecialHardReg(int r, bool frame_pointer_needed = true) {
  if (r == kMipsFramePointerRegno) {
    return frame_pointer_needed;
  }
  return r == kMipsArgPointerRegno || r == kMipsAtRegno ||
         (r >= kMipsKernelRegFirst && r <= kMipsReturnAddrRegno) || r >= kMipsMdRegFirst;
}

bool IsAbiArgHardReg(int r) {
  return (r >= kMipsArgRegFirst && r <= kMipsArgRegLast) ||
         (r >= kMipsFpArgRegFirst && r <= kMipsFpArgRegLast);
}

bool IsAbiCallInterfaceHardReg(int r) {
  return (r >= kMipsRetRegFirst && r <= kMipsRetRegLast) ||
         (r >= kMipsFpRegFirst && r <= kMipsFpRetRegLast) || IsAbiArgHardReg(r);
}

void CollectRegReads(const RtlExpr& x, absl::btree_set<int>* reads) {
  if (x.op == RtlOp::kReg) {
    reads->insert(x.RegNo());
    return;
  }
  if (x.op == RtlOp::kSet) {
    const RtlExpr& dst = x.SetDest();
    if (dst.op == RtlOp::kMem && !dst.children.empty()) {
      CollectRegReads(dst.children[0], reads);
    } else if (dst.op == RtlOp::kStrictLowPart || dst.op == RtlOp::kSubreg ||
               dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract) {
      CollectRegReads(dst, reads);
    }
    CollectRegReads(x.SetSrc(), reads);
    return;
  }
  if (x.op == RtlOp::kClobber) {
    return;
  }
  for (const RtlExpr& ch : x.children) {
    CollectRegReads(ch, reads);
  }
}

void CollectFullRegDefs(const RtlExpr& x, absl::btree_set<int>* full_defs,
                        absl::btree_set<int>* partial_defs) {
  if (x.op == RtlOp::kSet || x.op == RtlOp::kClobber) {
    if (x.children.empty()) {
      return;
    }
    const RtlExpr& dst = x.children[0];
    if (dst.op == RtlOp::kReg) {
      full_defs->insert(dst.RegNo());
    } else if (dst.op == RtlOp::kSubreg || dst.op == RtlOp::kStrictLowPart ||
               dst.op == RtlOp::kZeroExtract || dst.op == RtlOp::kSignExtract) {
      const RtlExpr* inner = &dst;
      while ((inner->op == RtlOp::kSubreg || inner->op == RtlOp::kStrictLowPart ||
              inner->op == RtlOp::kZeroExtract || inner->op == RtlOp::kSignExtract) &&
             !inner->children.empty()) {
        inner = &inner->children[0];
      }
      if (inner->op == RtlOp::kReg) {
        partial_defs->insert(inner->RegNo());
      }
    }
    return;
  }
  if (x.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : x.children) {
      CollectFullRegDefs(ch, full_defs, partial_defs);
    }
  }
}

void RenameExprRegs(RtlExpr* x, const absl::btree_map<int, int>& mapping) {
  if (x->op == RtlOp::kReg) {
    if (!x->flags.integrated) {
      if (const auto it = mapping.find(x->RegNo()); it != mapping.end()) {
        x->int_val = it->second;
      }
    }
    return;
  }
  for (RtlExpr& ch : x->children) {
    RenameExprRegs(&ch, mapping);
  }
}

// Matches a `reload1.c:emit_reload_insns` adjacent `hi`/`lo` output-reload pair
// (`mips.md:830-1010` `mulsi3_internal`, `smulsi3_highpart`, `umulsi3_highpart`,
// `mulsidi3`/`umulsidi3`): `(parallel [(set (reg:M hi/lo) <mult/truncate>) ...])`
// followed immediately by `(set (reg:M gpr) (reg:M hi/lo))`.
bool TryMatchAdjacentMdOutputReload(const Insn& prod, const Insn& mv, RtlExpr* out_gpr_dest) {
  if (prod.kind != Insn::Kind::kInsn || mv.kind != Insn::Kind::kInsn) {
    return false;
  }
  if (prod.pn.pattern.op != RtlOp::kParallel || prod.pn.pattern.children.size() < 2) {
    return false;
  }
  const RtlExpr& elem0 = prod.pn.pattern.children[0];
  if (elem0.op != RtlOp::kSet || elem0.children.size() != 2 || elem0.SetDest().op != RtlOp::kReg ||
      elem0.SetDest().flags.Any()) {
    return false;
  }
  const int md_reg = elem0.SetDest().RegNo();
  const RtlMachineMode md_mode = elem0.SetDest().mode;
  const RtlOp rhs_op = elem0.SetSrc().op;
  const bool valid_md_prod =
      (md_reg == kMipsMdRegFirst && md_mode == RtlMachineMode::kDi && rhs_op == RtlOp::kMult) ||
      (md_reg == kMipsMdRegFirst + 1 && md_mode == RtlMachineMode::kSi && rhs_op == RtlOp::kMult) ||
      (md_reg == kMipsMdRegFirst && md_mode == RtlMachineMode::kSi && rhs_op == RtlOp::kTruncate);
  if (!valid_md_prod) {
    return false;
  }
  if (mv.pn.pattern.op != RtlOp::kSet || mv.pn.pattern.children.size() != 2) {
    return false;
  }
  for (const RtlRegNote& note : mv.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual) {
      return false;
    }
  }
  const RtlExpr& mv_dst = mv.pn.pattern.SetDest();
  const RtlExpr& mv_src = mv.pn.pattern.SetSrc();
  if (mv_dst.op != RtlOp::kReg || mv_src.op != RtlOp::kReg) {
    return false;
  }
  if (mv_dst.RegNo() < 0 || mv_dst.RegNo() >= kMipsFpRegFirst || mv_dst.mode != md_mode) {
    return false;
  }
  if (mv_src.RegNo() != md_reg || mv_src.mode != md_mode) {
    return false;
  }
  if (out_gpr_dest != nullptr) {
    *out_gpr_dest = mv_dst;
  }
  return true;
}

// True iff `notes` carries a `REG_DEAD` note overlapping register `r`
// (`flow.c:2506-2515`: `flow.c` never emits `REG_DEAD P` on an insn that both
// reads and writes the same pseudo `P`, so a post-reload `(set r (op r ...))`
// with `REG_DEAD r` proves two distinct single-set pre-alloc pseudos shared `r`).
bool HasRegDeadNoteCovering(absl::Span<const RtlRegNote> notes, int r, const MachineModel& mm) {
  for (const RtlRegNote& note : notes) {
    if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
      continue;
    }
    const int d = note.datum.RegNo();
    if (d == r) {
      return true;
    }
    if (d >= 0 && d < kMipsFirstPseudoRegister && r >= 0 && r < kMipsFirstPseudoRegister) {
      const int nr = mm.HardRegNRegs(d, note.datum.mode);
      if (r >= d && r < d + nr) {
        return true;
      }
    }
  }
  return false;
}

// Matches a single-use cross-bank `"=f"` FPR->GPR (`mfc1`) output reload
// (`reload1.c:5641, 6239-6285` `emit_reload_insns`) immediately following its
// producing FP arithmetic instruction:
// `(set (reg:SF|DF fpr) <fp_op>) ; (set (reg:SF|DF gpr) (reg:SF|DF fpr))`
// where the move carries no `REG_DEAD fpr` or `REG_EQUIV`/`REG_EQUAL` notes.
bool TryMatchAdjacentFprOutputReload(const Insn& prod_insn, const Insn& mv_insn,
                                     RtlExpr* gpr_dest_out) {
  if (prod_insn.kind != Insn::Kind::kInsn || mv_insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  const RtlExpr& p1 = prod_insn.pn.pattern;
  const RtlExpr& p2 = mv_insn.pn.pattern;
  if (p1.op != RtlOp::kSet || p1.children.size() != 2 || p1.SetDest().op != RtlOp::kReg ||
      p1.SetDest().flags.integrated || p2.op != RtlOp::kSet || p2.children.size() != 2 ||
      p2.SetDest().op != RtlOp::kReg || p2.SetDest().flags.integrated ||
      p2.SetSrc().op != RtlOp::kReg) {
    return false;
  }
  const int fpr = p1.SetDest().RegNo();
  const int gpr = p2.SetDest().RegNo();
  if (fpr < kMipsFpRegFirst || fpr > kMipsFpRegLast || gpr < 2 || gpr >= kMipsFpRegFirst ||
      p2.SetSrc().RegNo() != fpr || p1.SetDest().mode != p2.SetSrc().mode ||
      p2.SetDest().mode != p2.SetSrc().mode ||
      (p1.SetDest().mode != RtlMachineMode::kSf && p1.SetDest().mode != RtlMachineMode::kDf)) {
    return false;
  }
  const RtlOp sop = p1.SetSrc().op;
  if (sop == RtlOp::kReg || sop == RtlOp::kMem || sop == RtlOp::kConstDouble ||
      sop == RtlOp::kCall || sop == RtlOp::kSubreg) {
    return false;
  }
  for (const RtlRegNote& n : mv_insn.pn.reg_notes) {
    if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual ||
        (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
         n.datum.RegNo() == fpr)) {
      return false;
    }
  }
  if (gpr_dest_out != nullptr) {
    *gpr_dest_out = p2.SetDest();
  }
  return true;
}

// Returns true iff hard register `[regno, regno + HardRegNregs(regno, mode))`
// holds no live value after `block[cons_idx]` (either `cons_idx` itself fully
// overwrites/clobbers every sub-register, or no subsequent insn reads any
// sub-register before full overwrite/call-clobber and none is in `live_at_end_s2`).
bool IsRegDeadAfterInsn(absl::Span<const Insn> block, int cons_idx, int regno, RtlMachineMode mode,
                        const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2) {
  const int nregs = std::max(1, mm.HardRegNRegs(regno, mode));
  absl::btree_set<int> remaining;
  for (int o = 0; o < nregs; ++o) {
    remaining.insert(regno + o);
  }
  {
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(block[static_cast<size_t>(cons_idx)].pn.pattern, &fdefs, &pdefs);
    for (int r : fdefs) {
      remaining.erase(r);
    }
    if (remaining.empty()) {
      return true;
    }
  }
  for (size_t k = static_cast<size_t>(cons_idx) + 1; k < block.size(); ++k) {
    const Insn& insn = block[k];
    absl::btree_set<int> reads;
    CollectRegReads(insn.pn.pattern, &reads);
    if (insn.kind == Insn::Kind::kCall) {
      for (const RtlExpr& fu : insn.pn.call_fusage) {
        CollectRegReads(fu, &reads);
      }
    }
    for (int r : remaining) {
      if (reads.contains(r)) {
        return false;
      }
    }
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
    for (int r : fdefs) {
      remaining.erase(r);
    }
    if (insn.kind == Insn::Kind::kCall) {
      for (auto it = remaining.begin(); it != remaining.end();) {
        if (mm.CallUsed(*it)) {
          it = remaining.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (remaining.empty()) {
      return true;
    }
  }
  for (int r : remaining) {
    if (live_at_end_s2.contains(r)) {
      return false;
    }
  }
  return true;
}

// Matches a single-use `"f"` input reload (`reload1.c:5186-5257, 5641,
// 5849-5871, 6118-6125` `choose_reload_regs` + `emit_reload_insns`) preceding
// its consuming FP instruction:
//   - cross-bank GPR->FPR (`mtc1`): `(set (reg:SF|DF fpr) (reg:SF|DF gpr))`
//   - reload-inherited FPR->FPR (`mov.s`/`mov.d`) feeding an output-reloaded
//     FP arithmetic consumer (`md_output_reload_dest.contains(cons_idx)` whose
//     original dest is `fpr`): `(set (reg:SF|DF fpr) (reg:SF|DF src_fpr))`
// where `mv_insn` carries no `REG_EQUIV`/`REG_EQUAL`/`REG_DEAD src` notes (since
// `reload1.c` leaves any original `REG_DEAD` on `cons_insn`, whereas
// `function.c:assign_parms` entry-arg copies carry `REG_DEAD` from `flow.c`)
// and `fpr` is dead immediately after `cons_insn`.
bool TryMatchAdjacentFprInputReload(absl::Span<const Insn> block, int mv_idx, int cons_idx,
                                    const MachineModel& mm,
                                    const absl::btree_set<int>& live_at_end_s2,
                                    const absl::btree_map<int, RtlExpr>& md_output_reload_dest,
                                    int* fpr_out, RtlExpr* src_out) {
  const Insn& mv_insn = block[static_cast<size_t>(mv_idx)];
  const Insn& cons_insn = block[static_cast<size_t>(cons_idx)];
  if (mv_insn.kind != Insn::Kind::kInsn || cons_insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  const RtlExpr& mp = mv_insn.pn.pattern;
  if (mp.op != RtlOp::kSet || mp.children.size() != 2 || mp.SetDest().op != RtlOp::kReg ||
      mp.SetDest().flags.integrated || mp.SetSrc().op != RtlOp::kReg ||
      mp.SetSrc().flags.integrated) {
    return false;
  }
  const int fpr = mp.SetDest().RegNo();
  const int src_reg = mp.SetSrc().RegNo();
  const RtlMachineMode mode = mp.SetDest().mode;
  if (fpr < kMipsFpRegFirst || fpr > kMipsFpRegLast || mp.SetSrc().mode != mode ||
      (mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDf)) {
    return false;
  }
  const bool is_gpr_to_fpr = (src_reg >= 2 && src_reg < kMipsFpRegFirst);
  const bool is_fpr_to_fpr =
      (src_reg >= kMipsFpRegFirst && src_reg <= kMipsFpRegLast && src_reg != fpr);
  if (!is_gpr_to_fpr && !is_fpr_to_fpr) {
    return false;
  }
  const int src_nregs = std::max(1, mm.HardRegNRegs(src_reg, mode));
  for (const RtlRegNote& n : mv_insn.pn.reg_notes) {
    if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual) {
      return false;
    }
    if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg) {
      const int nr = n.datum.RegNo();
      if (nr >= src_reg && nr < src_reg + src_nregs) {
        return false;
      }
    }
  }
  const RtlExpr& cp = cons_insn.pn.pattern;
  if (cp.op == RtlOp::kUse || cp.op == RtlOp::kClobber || cp.op == RtlOp::kNote) {
    return false;
  }
  if (cp.op == RtlOp::kSet && cp.children.size() == 2 && cp.SetDest().op == RtlOp::kReg &&
      (cp.SetSrc().op == RtlOp::kReg || cp.SetSrc().op == RtlOp::kMem)) {
    return false;
  }
  if (is_fpr_to_fpr) {
    // Only fold an FPR->FPR copy when `cons_idx` is an output-reloaded FP
    // arithmetic insn (`md_output_reload_dest`) sharing `fpr` as both its
    // reload-inherited input reload reg and its `"=f"` output reload reg
    // (`reload1.c:5186-5257, 5641`).
    const RtlExpr* cs = SingleSetOf(cp);
    if (!md_output_reload_dest.contains(cons_idx) || cs == nullptr ||
        cs->SetDest().op != RtlOp::kReg || cs->SetDest().RegNo() != fpr) {
      return false;
    }
  }
  absl::btree_set<int> cons_reads;
  CollectRegReads(cp, &cons_reads);
  if (!cons_reads.contains(fpr)) {
    return false;
  }
  if (!IsRegDeadAfterInsn(block, cons_idx, fpr, mode, mm, live_at_end_s2)) {
    return false;
  }
  if (fpr_out != nullptr) {
    *fpr_out = fpr;
  }
  if (src_out != nullptr) {
    *src_out = mp.SetSrc();
  }
  return true;
}

void ReplaceRegInExpr(RtlExpr* expr, int old_reg, const RtlExpr& replacement) {
  if (expr->op == RtlOp::kReg && expr->RegNo() == old_reg && !expr->flags.integrated) {
    *expr = replacement;
    return;
  }
  for (RtlExpr& ch : expr->children) {
    ReplaceRegInExpr(&ch, old_reg, replacement);
  }
}

void ReplaceRegInReadPositions(RtlExpr* pattern, int old_reg, const RtlExpr& replacement) {
  auto handle_node = [&](RtlExpr* node) {
    if (node->op == RtlOp::kSet && node->children.size() == 2) {
      ReplaceRegInExpr(&node->children[1], old_reg, replacement);
      if (node->children[0].op == RtlOp::kMem) {
        ReplaceRegInExpr(&node->children[0], old_reg, replacement);
      }
    } else if (node->op != RtlOp::kClobber) {
      ReplaceRegInExpr(node, old_reg, replacement);
    }
  };
  if (pattern->op == RtlOp::kParallel) {
    for (RtlExpr& ch : pattern->children) {
      handle_node(&ch);
    }
  } else {
    handle_node(pattern);
  }
}

// Returns true when post-call read-modify-write `(set r (op r ...))` at core index `ci`
// on a block-local callee-saved register `r` that crossed an earlier call matches a
// `local-alloc.c:1266-1300, 1851-1886` (`combine_regs`) dying-input register reuse of a
// fresh `reg_n_calls_crossed == 0` pseudo (`sched.c:1732`), which locks the insn after the call.
bool IsPostCallCombineRegsSplitCandidate(absl::Span<const Insn> post_reload_block,
                                         absl::Span<const int> core_to_orig_index, int ci, int r,
                                         int birth_ci, int last_call_ci,
                                         const absl::btree_set<int>& live_at_end_s2,
                                         const MachineModel& mm) {
  if (r < 0 || r >= kMipsFirstPseudoRegister || mm.CallUsed(r) || birth_ci < 0 ||
      last_call_ci <= birth_ci || ci <= last_call_ci) {
    return false;
  }
  const Insn& insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
  const RtlExpr* s = SingleSetOf(insn.pn.pattern);
  if (s == nullptr || s->SetDest().op != RtlOp::kReg || s->SetDest().RegNo() != r ||
      s->SetDest().flags.integrated) {
    return false;
  }
  for (const RtlRegNote& n : insn.pn.reg_notes) {
    if ((n.kind == RtlMachineMode::kRegEquiv && n.datum.op != RtlOp::kMem) ||
        n.kind == RtlMachineMode::kRegEqual || n.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const RtlExpr& rhs = s->SetSrc();
  if (rhs.op == RtlOp::kReg || rhs.op == RtlOp::kMem || rhs.children.empty()) {
    return false;
  }
  // `sched.c:1732` (`reg_n_calls_crossed == 0`) anti-dependence on the preceding
  // call is only needed when `rhs` does not already read a call-used hard reg
  // (e.g. a call return value `$v0`/`$f0`), which already locks the insn after
  // the call via a hard `REG_DEP_TRUE` edge.
  absl::btree_set<int> rhs_reads;
  CollectRegReads(rhs, &rhs_reads);
  for (int rr : rhs_reads) {
    if (rr >= 0 && rr < kMipsFirstPseudoRegister && mm.CallUsed(rr)) {
      return false;
    }
  }
  const RtlMachineMode dst_mode = s->SetDest().mode;
  // Match `local-alloc.c:1266-1300`: `combine_regs` tries operands in order
  // and stops on the first winning dying local operand of the same mode.
  bool direct_combinable_operand = false;
  for (const RtlExpr& ch : rhs.children) {
    if (ch.op == RtlOp::kReg && ch.RegNo() == r && ch.mode == dst_mode) {
      direct_combinable_operand = true;
      break;
    }
    if (ch.op == RtlOp::kReg && ch.mode == dst_mode && ch.RegNo() >= 0 &&
        ch.RegNo() < kMipsFirstPseudoRegister &&
        HasRegDeadNoteCovering(insn.pn.reg_notes, ch.RegNo(), mm)) {
      break;
    }
  }
  if (!direct_combinable_operand) {
    return false;
  }
  const int m = static_cast<int>(core_to_orig_index.size());
  bool saw_call_after = false;
  for (int k = ci + 1; k < m; ++k) {
    const Insn& later = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
    if (later.kind == Insn::Kind::kCall) {
      saw_call_after = true;
      continue;
    }
    absl::btree_set<int> later_reads;
    CollectRegReads(later.pn.pattern, &later_reads);
    if (later_reads.contains(r) && saw_call_after) {
      return false;
    }
    absl::btree_set<int> later_fdefs;
    absl::btree_set<int> later_pdefs;
    CollectFullRegDefs(later.pn.pattern, &later_fdefs, &later_pdefs);
    // `flow.c:2506-2515` + `local-alloc.c:1266-1300`: a non-reading full def of
    // `r` (or an explicit `REG_DEAD r` on a non-defining reader) ends `ci`'s
    // live range cleanly before any later unrelated reuse of callee-saved `r`.
    // A subsequent call-free full RMW (`later_fdefs && later_reads && !saw_call_after`)
    // is the next step of the same post-call `combine_regs` chain (`sched.c:1732`)
    // and keeps scanning forward to verify `r` dies without crossing a later call.
    if (later_fdefs.contains(r) && !later_reads.contains(r)) {
      return true;
    }
    if (later_pdefs.contains(r)) {
      return false;
    }
    if (later_fdefs.contains(r)) {
      if (HasRegDeadNoteCovering(later.pn.reg_notes, r, mm)) {
        return true;
      }
      continue;
    }
    if (later_reads.contains(r) && HasRegDeadNoteCovering(later.pn.reg_notes, r, mm)) {
      return true;
    }
  }
  return !live_at_end_s2.contains(r);
}

// Returns true when read-modify-write `(set r (op r ...))` (or single-`SET`
// `PARALLEL`) at core index `ci` matches a `local-alloc.c:1266-1300, 1851-1886`
// (`combine_regs`) dying-input register reuse of a fresh single-set pseudo
// (`sched.c:2490` `birthing_insn_p`, `sched.c:1732` `reg_n_calls_crossed == 0`).
// Excludes MIPS `lui + ori` 32-bit constant materialization (`config/mips/mips.c`),
// which GCC emits on a single multi-set pseudo without `REG_DEAD`.
bool IsGeneralCombineRegsRmwSplitCandidate(absl::Span<const Insn> post_reload_block,
                                           absl::Span<const int> core_to_orig_index, int ci, int r,
                                           int birth_ci, const absl::btree_set<int>& live_at_end_s2,
                                           const MachineModel& mm) {
  if (birth_ci < 0 || ci <= birth_ci || r < 2 || r >= kMipsFirstPseudoRegister || r == 28 ||
      r == kMipsStackPointerRegno || r == kMipsFramePointerRegno) {
    return false;
  }
  const Insn& insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
  const RtlExpr* s = SingleSetOf(insn.pn.pattern);
  if (s == nullptr || s->SetDest().op != RtlOp::kReg || s->SetDest().RegNo() != r ||
      s->SetDest().flags.integrated || s->SetDest().flags.frame_related) {
    return false;
  }
  for (const RtlRegNote& n : insn.pn.reg_notes) {
    if ((n.kind == RtlMachineMode::kRegEquiv && n.datum.op != RtlOp::kMem) ||
        n.kind == RtlMachineMode::kRegEqual || n.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const RtlExpr& rhs = s->SetSrc();
  if (rhs.op == RtlOp::kReg || rhs.op == RtlOp::kMem || rhs.children.empty()) {
    return false;
  }
  // Exclude MIPS `lui + ori` constant materialization (`(set r high16)` followed
  // by `(set r (ior r low16))`), which stays a single multi-set pseudo in GCC,
  // and exclude subword RMWs onto a wider multi-word def (e.g. `muldi3`
  // high-word `SImode` accumulation into a `DImode` pair): Stage 4 cannot
  // split mixed-mode hard-register live ranges into separate pseudos, so a
  // self-set `REG_DEAD` would survive illegally (`flow.c:2506-2515`,
  // `sched.c:2872-2906`).
  const Insn& birth_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[birth_ci])];
  const RtlExpr* birth_s = SingleSetOf(birth_insn.pn.pattern);
  if (birth_s != nullptr && birth_s->SetDest().op == RtlOp::kReg &&
      birth_s->SetDest().mode != s->SetDest().mode) {
    return false;
  }
  if (rhs.op == RtlOp::kIor && rhs.children.size() == 2 && rhs.children[1].op == RtlOp::kConstInt) {
    if (birth_s != nullptr &&
        ((birth_s->SetSrc().op == RtlOp::kConstInt && (birth_s->SetSrc().int_val & 0xffff) == 0) ||
         birth_s->SetSrc().op == RtlOp::kHigh)) {
      return false;
    }
  }
  // `local-alloc.c:892-925, 1004-1007` (`optimize_reg_copy_1` called from
  // `update_equiv_regs`): if `birth_insn` is a same-mode register-to-register
  // copy `(set r src)` where `src` remains live after `birth_insn`, splitting
  // `r` turns `r` into a `reg_n_sets == 1` copy target and triggers forward
  // copy propagation. Note that `reload1.c` output-reload moves omit
  // `REG_DEAD src` even when `src` is dead after `birth_insn`, so verify `src`
  // actually has a later use or is live-out.
  if (birth_s != nullptr && birth_s->SetDest().op == RtlOp::kReg &&
      birth_s->SetSrc().op == RtlOp::kReg && birth_s->SetSrc().mode == birth_s->SetDest().mode &&
      !birth_s->SetSrc().IsArgPointerReg() &&
      !HasRegDeadNoteCovering(birth_insn.pn.reg_notes, birth_s->SetSrc().RegNo(), mm)) {
    const int copy_src = birth_s->SetSrc().RegNo();
    const int m_body = static_cast<int>(core_to_orig_index.size());
    bool src_live_after_birth = live_at_end_s2.contains(copy_src);
    for (int k = birth_ci + 1; k < m_body; ++k) {
      const Insn& after = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
      if (after.kind == Insn::Kind::kCall) {
        if (mm.CallUsed(copy_src)) {
          src_live_after_birth = false;
          break;
        }
        continue;
      }
      absl::btree_set<int> after_reads;
      CollectRegReads(after.pn.pattern, &after_reads);
      if (after_reads.contains(copy_src)) {
        src_live_after_birth = true;
        break;
      }
      absl::btree_set<int> after_fdefs;
      absl::btree_set<int> after_pdefs;
      CollectFullRegDefs(after.pn.pattern, &after_fdefs, &after_pdefs);
      if (after_fdefs.contains(copy_src)) {
        src_live_after_birth = false;
        break;
      }
    }
    if (src_live_after_birth) {
      return false;
    }
  }
  const RtlMachineMode dst_mode = s->SetDest().mode;
  // Match `local-alloc.c:1266-1300`: `combine_regs` tries operands in order
  // and stops on the first winning dying local operand of the same mode.
  bool direct_combinable_operand = false;
  for (const RtlExpr& ch : rhs.children) {
    if (ch.op == RtlOp::kReg && ch.RegNo() == r && ch.mode == dst_mode) {
      direct_combinable_operand = true;
      break;
    }
    if (ch.op == RtlOp::kReg && ch.mode == dst_mode && ch.RegNo() >= 0 &&
        ch.RegNo() < kMipsFirstPseudoRegister &&
        HasRegDeadNoteCovering(insn.pn.reg_notes, ch.RegNo(), mm)) {
      break;
    }
  }
  if (!direct_combinable_operand) {
    return false;
  }
  // `expmed.c:693-707` (`store_fixed_bit_field`): bitfield read-modify-write
  // sequences pass `subtarget = temp` from `and_optab` into `ior_optab`, so
  // multi-step intra-block `and`/`ior` RMW chains on `r` originate as a single
  // multi-set pseudo rather than distinct `combine_regs` pseudos.
  const bool is_bitwise_rmw = (rhs.op == RtlOp::kAnd || rhs.op == RtlOp::kIor);
  if (is_bitwise_rmw) {
    for (int k = birth_ci + 1; k < ci; ++k) {
      const Insn& mid = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
      if (mid.kind == Insn::Kind::kCall) {
        continue;
      }
      const RtlExpr* ms = SingleSetOf(mid.pn.pattern);
      if (ms != nullptr && ms->SetDest().op == RtlOp::kReg && ms->SetDest().RegNo() == r &&
          (ms->SetSrc().op == RtlOp::kAnd || ms->SetSrc().op == RtlOp::kIor)) {
        return false;
      }
    }
  }
  // Verify the live range of `r` terminates within this block (so both `P_old`
  // and `P_new` are block-local and deterministically tied by `local-alloc.c:1855`).
  const int m = static_cast<int>(core_to_orig_index.size());
  for (int k = ci + 1; k < m; ++k) {
    const Insn& later = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
    if (later.kind == Insn::Kind::kCall) {
      if (mm.CallUsed(r)) {
        return true;
      }
      continue;
    }
    absl::btree_set<int> later_reads;
    CollectRegReads(later.pn.pattern, &later_reads);
    absl::btree_set<int> later_fdefs;
    absl::btree_set<int> later_pdefs;
    CollectFullRegDefs(later.pn.pattern, &later_fdefs, &later_pdefs);
    if (later_pdefs.contains(r)) {
      return false;
    }
    if (later_fdefs.contains(r)) {
      if (!later_reads.contains(r)) {
        return true;
      }
      // `flow.c:2505-2520`, `local-alloc.c:1266-1300`: a later RMW carrying
      // `REG_DEAD r` (e.g. a return-value `reg/i` RMW killing the last local
      // pseudo in `$v0` before redefining `$v0` as live-at-exit) ends the local
      // live range of `r` within this block.
      if (HasRegDeadNoteCovering(later.pn.reg_notes, r, mm)) {
        return true;
      }
      if (is_bitwise_rmw) {
        const RtlExpr* ls = SingleSetOf(later.pn.pattern);
        if (ls != nullptr && (ls->SetSrc().op == RtlOp::kAnd || ls->SetSrc().op == RtlOp::kIor)) {
          return false;
        }
      }
    }
  }
  return !live_at_end_s2.contains(r);
}

// Returns true when register `regno` (spanning `mode`) written/read at `idx`
// does NOT stay live out of `block` without an intervening redefinition or
// clobbering call (i.e. it is a scratch reload register rather than a hard reg
// live across the block exit).
bool RegValueDoesNotReachBlockEnd(absl::Span<const Insn> block, int idx, int regno,
                                  RtlMachineMode mode, const MachineModel& mm,
                                  const absl::btree_set<int>& live_at_end_s2) {
  const int nregs = std::max(1, mm.HardRegNRegs(regno, mode));
  absl::btree_set<int> remaining;
  for (int k = 0; k < nregs; ++k) {
    if (live_at_end_s2.contains(regno + k)) {
      remaining.insert(regno + k);
    }
  }
  if (remaining.empty()) {
    return true;
  }
  const int n = static_cast<int>(block.size());
  for (int i = idx + 1; i < n; ++i) {
    const Insn& insn = block[static_cast<size_t>(i)];
    if (insn.kind == Insn::Kind::kCall) {
      for (auto it = remaining.begin(); it != remaining.end();) {
        if (mm.CallUsed(*it)) {
          it = remaining.erase(it);
        } else {
          ++it;
        }
      }
      if (remaining.empty()) {
        return true;
      }
      continue;
    }
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
    for (int d : fdefs) {
      remaining.erase(d);
    }
    if (remaining.empty()) {
      return true;
    }
  }
  return false;
}

// Identifies a distinct pre-reload spilled-pseudo live range on a stack slot:
// `(slot_offset, version_id)` where `version_id == -1` denotes a cross-block
// live-in/shared slot and `version_id >= 0` is the defining store's `orig_idx`
// for non-overlapping intra-block pseudos sharing `spill_stack_slot[from_reg]`
// (`reload1.c:2305-2306, 2362-2367`).
using SpillPseudoKey = std::pair<int64_t, int>;

struct SpillSlotFoldInfo {
  // Maps producer orig_idx -> spill pseudo key for folded output-reload spill stores.
  absl::btree_map<int, SpillPseudoKey> store_slot_by_prod;
  // Maps consumer orig_idx -> (hard_reg -> (spill pseudo key, mode)) for folded single-use
  // input-reload spill loads.
  absl::btree_map<int, absl::btree_map<int, std::pair<SpillPseudoKey, RtlMachineMode>>>
      load_subs_by_cons;
  // Maps folded input-reload orig_idx -> consuming orig_idx.
  absl::btree_map<int, int> input_reload_orig_consumer;
  // Maps excluded intra-block spill-load orig_idx (not folded into a single consumer) to
  // `(hard_reg, spill pseudo key)` so multi-use reloads still map `cur_pseudo[hard_reg]` to
  // `P_slot`.
  absl::btree_map<int, std::pair<int, SpillPseudoKey>> multi_use_load_slot_by_orig;
  // Maps core in-place `reload1.c:1968-1992` spill-slot load orig_idx
  // (`(set (reg arg/ret) (mem (sp + spill_off)))` feeding a call `call_fusage` or
  // `(use (reg/i ...))`) to `(spill pseudo key, mode)` so `ProjectPostReloadBlockToSched1View`
  // keeps it as a core `(set (reg arg/ret) (reg P_slot))` copy.
  absl::btree_map<int, std::pair<SpillPseudoKey, RtlMachineMode>> in_place_spill_load_slot_by_orig;
  // Maps block-0 direct spilled entry-parameter copy store orig_idx
  // (`function.c:assign_parms` + `reload1.c:2309-2400` `alter_reg`:
  // `(set (mem:M [sp/fp+off]) (reg:M arg_reg))` with no prior producer in B0)
  // to `(spill pseudo key, mode)` so `ProjectPostReloadBlockToSched1View` keeps it as
  // a core `(set (reg:M P_slot) (reg:M arg_reg))` entry-parameter copy.
  absl::btree_map<int, std::pair<SpillPseudoKey, RtlMachineMode>>
      direct_entry_arg_spill_slot_by_store;
  // Spill pseudo keys whose spilled pseudo is live-out to another basic block.
  absl::btree_set<SpillPseudoKey> live_out_slot_keys;
};

// Returns true when stack store `s_idx` of `(set (mem:M [sp/fp+off]) (reg:M s_reg))`
// is an intra-block output-reload spill store whose reload register `s_reg` is
// inherited directly (`reload1.c:5186-5257`, `reload1.c:3973-3981`) by a later
// consumer carrying the spilled pseudo's in-place `MEM`-rewritten `REG_DEAD`
// note (`reload1.c:1968-1992` `PUT_CODE (reg, MEM)`), so no intra-block
// input-reload load from `key` was emitted.
bool HasInheritedSpillSlotDeadConsumer(absl::Span<const Insn> post_reload_block, int s_idx,
                                       int s_reg, const StackSlotKey& key, const MachineModel& mm,
                                       bool fp_needed) {
  const int n = static_cast<int>(post_reload_block.size());
  const int s_nregs = std::max(1, mm.HardRegNRegs(s_reg, key.mode));
  for (int c = s_idx + 1; c < n; ++c) {
    const Insn& cand = post_reload_block[static_cast<size_t>(c)];
    bool has_matching_mem_dead = false;
    for (const RtlRegNote& note : cand.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem &&
          note.datum.mode == key.mode && !note.datum.children.empty()) {
        int nb = -1;
        int64_t noff = 0;
        if (ParseStackBaseOffset(note.datum.children[0], &nb, &noff, fp_needed) &&
            nb == key.base_regno && noff == key.offset) {
          has_matching_mem_dead = true;
          break;
        }
      }
    }
    if (has_matching_mem_dead) {
      absl::btree_set<int> c_reads;
      CollectRegReads(cand.pn.pattern, &c_reads);
      for (int k = 0; k < s_nregs; ++k) {
        if (c_reads.contains(s_reg + k)) {
          return true;
        }
      }
      return false;
    }
    if (cand.kind == Insn::Kind::kJump) {
      break;
    }
    if (cand.kind == Insn::Kind::kCall) {
      for (int k = 0; k < s_nregs; ++k) {
        if (mm.CallUsed(s_reg + k)) {
          return false;
        }
      }
      continue;
    }
    if (auto m_acc = TryParsePlainStackSlotMove(cand, fp_needed);
        m_acc.has_value() && m_acc->key.offset == key.offset) {
      break;
    }
    absl::btree_set<int> c_fdefs;
    absl::btree_set<int> c_pdefs;
    CollectFullRegDefs(cand.pn.pattern, &c_fdefs, &c_pdefs);
    for (int k = 0; k < s_nregs; ++k) {
      if (c_fdefs.contains(s_reg + k) || c_pdefs.contains(s_reg + k)) {
        return false;
      }
    }
  }
  return false;
}

// Identifies intra-block (non-caller-save), inherited intra-block, and cross-block
// `reload1.c:1968-1992, 2309-2419, 5186-5257, 5641-6160` (`alter_reg` /
// `choose_reload_regs` / `emit_reload_insns`) output-reload spill stores,
// input-reload spill loads, and in-place spill-slot loads into call-arg/return
// registers so `ProjectPostReloadBlockToSched1View` reconstructs the true
// pre-reload spilled pseudos `P_slot` and data-dependence edges across calls
// and scratch reuses.
SpillSlotFoldInfo FoldReloadSpillsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context,
    const absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>>& slot_stores,
    const absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>>& slot_loads,
    const absl::btree_set<int>& intra_reload_spill_store_indices,
    const absl::btree_set<int>& intra_reload_spill_load_indices,
    const absl::btree_map<int, RtlExpr>& md_output_reload_dest,
    std::vector<bool>* is_spill_overhead, absl::btree_map<int, int>* output_reload_orig_owner) {
  SpillSlotFoldInfo info;
  const int n = static_cast<int>(post_reload_block.size());
  const bool fp_needed = !context.stack_layout.valid || context.stack_layout.frame_pointer_needed;
  const bool can_fold_cross_block =
      context.has_cfg_context && context.stack_layout.valid && !context.has_stack_addr_escape &&
      context.stack_layout.frame_size > context.stack_layout.starting_frame_offset;
  const int frame_base =
      context.stack_layout.frame_pointer_needed ? kMipsFramePointerRegno : kMipsStackPointerRegno;
  auto is_cross_spill_slot_key = [&](const StackSlotKey& key) {
    return can_fold_cross_block &&
           (key.base_regno == frame_base || key.base_regno == kMipsStackPointerRegno) &&
           key.offset >= context.stack_layout.starting_frame_offset &&
           key.offset < context.stack_layout.frame_size;
  };

  absl::btree_set<int64_t> folded_intra_store_offsets;
  struct FoldedStoreRecord {
    int s_idx = -1;
    int prod_idx = -1;
    bool is_direct_entry_arg = false;
    RtlMachineMode mode = RtlMachineMode::kSi;
  };
  struct FoldedLoadRecord {
    enum class Kind { kFoldedSingleUse, kInPlace, kMultiUse };
    int64_t slot_off = 0;
    int l_idx = -1;
    int cons_idx = -1;
    int l_reg = -1;
    RtlMachineMode mode = RtlMachineMode::kSi;
    Kind kind = Kind::kFoldedSingleUse;
  };
  absl::btree_map<int64_t, std::vector<FoldedStoreRecord>> folded_stores_by_off;
  absl::btree_set<int64_t> cross_live_out_offsets;
  absl::btree_set<int64_t> cross_from_other_offsets;
  std::vector<FoldedLoadRecord> folded_loads;
  absl::btree_map<std::pair<int, int64_t>, int> cons_slot_load_idx;
  absl::btree_set<int> claimed_store_prods;
  absl::btree_set<std::pair<int, int>> claimed_cons_lregs;

  // (a) Output-reload spill stores (`reload1.c:5641-6155`): intra-block
  //     non-caller-save spill stores, inherited intra-block spill stores
  //     (`reload1.c:1968-1992, 5186-5257`), and cross-block spill stores.
  for (const auto& [key, stores] : slot_stores) {
    const bool is_cross_live_out =
        is_cross_spill_slot_key(key) && context.other_block_spill_load_offsets.contains(key.offset);
    if (is_cross_spill_slot_key(key) &&
        context.other_block_spill_store_counts.contains(key.offset)) {
      cross_from_other_offsets.insert(key.offset);
    }
    const bool is_local_spill_slot =
        !context.stack_layout.valid ||
        ((key.base_regno == frame_base || key.base_regno == kMipsStackPointerRegno) &&
         key.offset >= context.stack_layout.starting_frame_offset &&
         key.offset < context.stack_layout.frame_size);
    for (const auto& [s_idx, s_reg] : stores) {
      const bool is_intra_non_cs = intra_reload_spill_store_indices.contains(s_idx);
      const bool is_cross = !(*is_spill_overhead)[static_cast<size_t>(s_idx)] && is_cross_live_out;
      const bool is_entry_arg_spill_cand =
          context.is_entry_block && !context.has_stack_addr_escape && is_local_spill_slot &&
          (is_intra_non_cs || is_cross_live_out) &&
          ((s_reg >= kMipsArgRegFirst && s_reg <= kMipsArgRegLast) ||
           (s_reg >= kMipsFpArgRegFirst && s_reg <= kMipsFpArgRegFirst + 3));
      const bool is_inherited_intra =
          !is_intra_non_cs && !is_cross && !(*is_spill_overhead)[static_cast<size_t>(s_idx)] &&
          is_local_spill_slot &&
          HasInheritedSpillSlotDeadConsumer(post_reload_block, s_idx, s_reg, key, mm, fp_needed);
      if (!is_intra_non_cs && !is_cross && !is_entry_arg_spill_cand && !is_inherited_intra) {
        continue;
      }
      if (s_reg < 1 || s_reg >= kMipsFirstPseudoRegister ||
          IsFixedOrSpecialHardReg(s_reg, fp_needed) ||
          !RegValueDoesNotReachBlockEnd(post_reload_block, s_idx, s_reg, key.mode, mm,
                                        live_at_end_s2)) {
        continue;
      }
      const int s_nregs = std::max(1, mm.HardRegNRegs(s_reg, key.mode));
      int prod_idx = -1;
      for (int p = s_idx - 1; p >= 0; --p) {
        if ((*is_spill_overhead)[static_cast<size_t>(p)]) {
          continue;
        }
        const Insn& cand = post_reload_block[static_cast<size_t>(p)];
        if (cand.kind == Insn::Kind::kCall || cand.kind == Insn::Kind::kJump) {
          break;
        }
        if (auto m_acc = TryParsePlainStackSlotMove(cand, fp_needed);
            m_acc.has_value() && m_acc->key.offset == key.offset) {
          break;
        }
        const auto md_it = md_output_reload_dest.find(p);
        if (md_it != md_output_reload_dest.end() && md_it->second.op == RtlOp::kReg &&
            md_it->second.RegNo() == s_reg && md_it->second.mode == key.mode &&
            !md_it->second.flags.integrated) {
          prod_idx = p;
          break;
        }
        const RtlExpr* ps = SingleSetOf(cand.pn.pattern);
        if (ps != nullptr && ps->SetDest().op == RtlOp::kReg && ps->SetDest().RegNo() == s_reg &&
            ps->SetDest().mode == key.mode && !ps->SetDest().flags.integrated) {
          if (!TryParsePlainStackSlotMove(cand, fp_needed).has_value()) {
            prod_idx = p;
          }
          break;
        }
        absl::btree_set<int> p_fdefs;
        absl::btree_set<int> p_pdefs;
        CollectFullRegDefs(cand.pn.pattern, &p_fdefs, &p_pdefs);
        bool clobbers_sreg = false;
        for (int k = 0; k < s_nregs; ++k) {
          if (p_fdefs.contains(s_reg + k) || p_pdefs.contains(s_reg + k)) {
            clobbers_sreg = true;
            break;
          }
        }
        if (clobbers_sreg) {
          break;
        }
      }
      if (prod_idx >= 0 && !claimed_store_prods.contains(prod_idx)) {
        (*is_spill_overhead)[static_cast<size_t>(s_idx)] = true;
        (*output_reload_orig_owner)[s_idx] = prod_idx;
        claimed_store_prods.insert(prod_idx);
        folded_stores_by_off[key.offset].push_back({s_idx, prod_idx, false, key.mode});
        if (is_intra_non_cs) {
          folded_intra_store_offsets.insert(key.offset);
        }
        if (is_cross_live_out) {
          cross_live_out_offsets.insert(key.offset);
        }
      } else if (prod_idx < 0 && is_entry_arg_spill_cand) {
        // `function.c:assign_parms` + `reload1.c:2309-2400` (`alter_reg`):
        // a block-0 entry-parameter copy whose destination pseudo was spilled
        // to a stack slot lowers directly to `(set (mem:M [sp/fp+off]) (reg:M arg))`
        // with no prior producer in B0. Preserve it as a core `(set P_slot arg)`
        // copy rather than dropping or leaving it as an opaque stack store.
        bool clean_from_entry = true;
        for (int p = s_idx - 1; p >= 0; --p) {
          const Insn& cand = post_reload_block[static_cast<size_t>(p)];
          if (cand.kind == Insn::Kind::kCall || cand.kind == Insn::Kind::kJump) {
            clean_from_entry = false;
            break;
          }
          if (auto m_acc = TryParsePlainStackSlotMove(cand, fp_needed);
              m_acc.has_value() && m_acc->key.offset == key.offset) {
            clean_from_entry = false;
            break;
          }
          absl::btree_set<int> p_fdefs;
          absl::btree_set<int> p_pdefs;
          CollectFullRegDefs(cand.pn.pattern, &p_fdefs, &p_pdefs);
          for (int k = 0; k < s_nregs; ++k) {
            if (p_fdefs.contains(s_reg + k) || p_pdefs.contains(s_reg + k)) {
              clean_from_entry = false;
              break;
            }
          }
          if (!clean_from_entry) {
            break;
          }
        }
        if (clean_from_entry) {
          (*is_spill_overhead)[static_cast<size_t>(s_idx)] = false;
          folded_stores_by_off[key.offset].push_back({s_idx, s_idx, true, key.mode});
          if (is_intra_non_cs || slot_loads.contains(key)) {
            folded_intra_store_offsets.insert(key.offset);
          }
          if (is_cross_live_out) {
            cross_live_out_offsets.insert(key.offset);
          }
        }
      }
    }
  }

  // (b) Input-reload spill loads (`reload1.c:5641-6135`) and in-place spill-slot
  //     loads (`reload1.c:1968-1992` `PUT_CODE(reg, MEM)`): both intra-block
  //     non-caller-save spill loads and cross-block spill loads.
  for (const auto& [key, loads] : slot_loads) {
    const bool is_cross_from_other =
        is_cross_spill_slot_key(key) && context.other_block_spill_store_counts.contains(key.offset);
    if (is_cross_from_other) {
      cross_from_other_offsets.insert(key.offset);
    }
    const bool is_folded_intra = folded_intra_store_offsets.contains(key.offset);
    for (const auto& [l_idx, l_reg] : loads) {
      const bool is_intra_non_cs =
          intra_reload_spill_load_indices.contains(l_idx) && is_folded_intra;
      const bool is_cross =
          !(*is_spill_overhead)[static_cast<size_t>(l_idx)] && is_cross_from_other;
      if (!is_folded_intra && !is_cross) {
        continue;
      }
      if (l_reg < 1 || l_reg >= kMipsFirstPseudoRegister ||
          IsFixedOrSpecialHardReg(l_reg, fp_needed)) {
        continue;
      }
      const int l_nregs = std::max(1, mm.HardRegNRegs(l_reg, key.mode));
      int cons_idx = -1;
      bool feeds_call_or_use = false;
      for (int c = l_idx + 1; c < n; ++c) {
        if ((*is_spill_overhead)[static_cast<size_t>(c)]) {
          continue;
        }
        const Insn& cand = post_reload_block[static_cast<size_t>(c)];
        if (cand.kind == Insn::Kind::kCall) {
          absl::btree_set<int> fu_reads;
          for (const RtlExpr& fu : cand.pn.call_fusage) {
            CollectRegReads(fu, &fu_reads);
          }
          for (int k = 0; k < l_nregs; ++k) {
            if (fu_reads.contains(l_reg + k)) {
              feeds_call_or_use = true;
              break;
            }
          }
          break;
        }
        if (cand.pn.pattern.op == RtlOp::kUse) {
          absl::btree_set<int> use_reads;
          CollectRegReads(cand.pn.pattern, &use_reads);
          for (int k = 0; k < l_nregs; ++k) {
            if (use_reads.contains(l_reg + k)) {
              feeds_call_or_use = true;
              break;
            }
          }
          break;
        }
        if (auto m_acc = TryParsePlainStackSlotMove(cand, fp_needed);
            m_acc.has_value() && m_acc->is_store && m_acc->key.offset == key.offset) {
          break;
        }
        absl::btree_set<int> c_reads;
        CollectRegReads(cand.pn.pattern, &c_reads);
        bool reads_lreg = false;
        for (int k = 0; k < l_nregs; ++k) {
          if (c_reads.contains(l_reg + k)) {
            reads_lreg = true;
            break;
          }
        }
        if (reads_lreg) {
          if (c_reads.contains(l_reg) && !TryParsePlainStackSlotMove(cand, fp_needed).has_value() &&
              IsRegDeadAfterInsn(post_reload_block, c, l_reg, key.mode, mm, live_at_end_s2) &&
              !claimed_cons_lregs.contains({c, l_reg})) {
            cons_idx = c;
          }
          break;
        }
        absl::btree_set<int> c_fdefs;
        absl::btree_set<int> c_pdefs;
        CollectFullRegDefs(cand.pn.pattern, &c_fdefs, &c_pdefs);
        bool clobbers_lreg = false;
        for (int k = 0; k < l_nregs; ++k) {
          if (c_fdefs.contains(l_reg + k) || c_pdefs.contains(l_reg + k)) {
            clobbers_lreg = true;
            break;
          }
        }
        if (clobbers_lreg) {
          break;
        }
      }
      if (feeds_call_or_use) {
        (*is_spill_overhead)[static_cast<size_t>(l_idx)] = false;
        folded_loads.push_back(
            {key.offset, l_idx, -1, l_reg, key.mode, FoldedLoadRecord::Kind::kInPlace});
      } else if (cons_idx >= 0 && (is_folded_intra || is_cross)) {
        (*is_spill_overhead)[static_cast<size_t>(l_idx)] = true;
        claimed_cons_lregs.insert({cons_idx, l_reg});
        info.input_reload_orig_consumer[l_idx] = cons_idx;
        cons_slot_load_idx[{cons_idx, key.offset}] = l_idx;
        folded_loads.push_back({key.offset, l_idx, cons_idx, l_reg, key.mode,
                                FoldedLoadRecord::Kind::kFoldedSingleUse});
      } else if (is_intra_non_cs) {
        folded_loads.push_back(
            {key.offset, l_idx, -1, l_reg, key.mode, FoldedLoadRecord::Kind::kMultiUse});
      }
    }
  }

  // (c) Assign disjoint `SpillPseudoKey` versions per stack slot, mirroring
  //     `reload1.c:2305-2306, 2362-2367` (`alter_reg` reusing `spill_stack_slot[from_reg]`
  //     for all non-overlapping pseudos spilled from the same hard register).
  absl::btree_map<int64_t, absl::btree_map<int, int>> store_ver_by_off;
  auto prev_store_version = [&](int64_t slot_off, int before_idx) -> int {
    const auto oit = store_ver_by_off.find(slot_off);
    if (oit == store_ver_by_off.end()) {
      return -1;
    }
    auto it = oit->second.lower_bound(before_idx);
    if (it == oit->second.begin()) {
      return -1;
    }
    --it;
    return it->second;
  };
  for (auto& [slot_off, recs] : folded_stores_by_off) {
    std::sort(recs.begin(), recs.end(), [](const FoldedStoreRecord& a, const FoldedStoreRecord& b) {
      return a.s_idx < b.s_idx;
    });
    const bool is_cross_from_other = cross_from_other_offsets.contains(slot_off);
    const bool is_cross_live_out = cross_live_out_offsets.contains(slot_off);
    for (const FoldedStoreRecord& rec : recs) {
      int ver = rec.s_idx;
      if (!rec.is_direct_entry_arg) {
        if (const auto lit = cons_slot_load_idx.find({rec.prod_idx, slot_off});
            lit != cons_slot_load_idx.end() && lit->second < rec.s_idx) {
          ver = prev_store_version(slot_off, lit->second);
        } else if (is_cross_from_other && is_cross_live_out && recs.size() == 1) {
          ver = -1;
        }
      }
      store_ver_by_off[slot_off][rec.s_idx] = ver;
      const SpillPseudoKey sp_key{slot_off, ver};
      if (rec.is_direct_entry_arg) {
        info.direct_entry_arg_spill_slot_by_store[rec.s_idx] = {sp_key, rec.mode};
      } else {
        info.store_slot_by_prod[rec.prod_idx] = sp_key;
      }
    }
    if (is_cross_live_out && !recs.empty()) {
      const int last_ver = store_ver_by_off[slot_off][recs.back().s_idx];
      info.live_out_slot_keys.insert({slot_off, last_ver});
    }
  }
  for (const FoldedLoadRecord& lrec : folded_loads) {
    const SpillPseudoKey sp_key{lrec.slot_off, prev_store_version(lrec.slot_off, lrec.l_idx)};
    if (lrec.kind == FoldedLoadRecord::Kind::kInPlace) {
      info.in_place_spill_load_slot_by_orig[lrec.l_idx] = {sp_key, lrec.mode};
    } else if (lrec.kind == FoldedLoadRecord::Kind::kFoldedSingleUse) {
      info.load_subs_by_cons[lrec.cons_idx][lrec.l_reg] = {sp_key, lrec.mode};
    } else {
      info.multi_use_load_slot_by_orig[lrec.l_idx] = {lrec.l_reg, sp_key};
    }
  }
  return info;
}

struct IntraBlockSpillSlotClassification {
  absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>> slot_stores;
  absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>> slot_loads;
  absl::btree_map<int, int> spill_store_reg;
  std::vector<std::tuple<int, int, int>> preserved_across_call_ranges;
  absl::btree_set<int> caller_save_store_indices;
  absl::btree_set<int> intra_reload_spill_store_indices;
  absl::btree_set<int> intra_reload_spill_load_indices;
};

// Scans plain stack-slot moves in `post_reload_block`, separating
// `caller-save.c:480-580` (`insert_save`/`insert_restore`) same-reg across-call
// pairs from `reload1.c:2309-2400, 5641-6160` intra-block spill store/load
// pairs (crossing a call, or leaf register-file saturation where
// `local-alloc.c:2140-2181` / `global.c:345-360` allocated a callee-saved reg
// of the same class before spilling any pseudo to the stack, `mips.h:1203`).
// Mirrors `reload1.c:657-658, 2352-2358` (`alter_reg` -> `assign_stack_local(..., -1)`
// with MIPS `BIGGEST_ALIGNMENT == 8` and big-endian offset `slot_base + (8 - sz)`)
// and `reload1.c:1445-1446` + `caller-save.c:315-316` (`setup_save_areas` ->
// `assign_stack_local(..., 0)` at 4-byte alignment AFTER initial `alter_reg`):
// returns the lowest stack offset in `[layout.starting_frame_offset, layout.frame_size)`
// that can hold a `reload` spill slot (the contiguous upper 8-byte suffix above
// any pre-reload local aggregate, `REG_EQUIV` stack slot, or non-big-endian-aligned
// access such as `SImode` at `slot_base + 0` inside a local `s32[]` array),
// excluding `caller-save.c:315-316` call-used save/restore slots.
int64_t ComputeMinValidReloadSpillOffset(absl::Span<const Insn> post_reload_block,
                                         const PostReloadStackLayout& layout,
                                         const MachineModel& mm, bool frame_pointer_needed) {
  if (!layout.valid || layout.frame_size <= layout.starting_frame_offset) {
    return layout.valid ? layout.starting_frame_offset : 0;
  }
  const int64_t min_off = layout.starting_frame_offset;
  const int64_t raw_max_off = (layout.callee_saved_area_offset >= 0)
                                  ? std::min(layout.frame_size, layout.callee_saved_area_offset)
                                  : layout.frame_size;
  const int64_t max_off =
      (raw_max_off > min_off) ? (min_off + 8 * ((raw_max_off - min_off) / 8)) : min_off;
  const int num_slots = static_cast<int>((max_off - min_off) / 8);
  if (num_slots <= 0) {
    return max_off;
  }
  // Exclude `caller-save.c:315-316` save/restore slots (allocated after
  // `reload1.c:657-658` `alter_reg` at natural 4-byte alignment) so consecutive
  // 4-byte caller-save slots above an `alter_reg` spill slot do not look like a
  // pre-reload local array.
  absl::btree_set<int64_t> caller_save_offsets;
  const bool has_call =
      std::any_of(post_reload_block.begin(), post_reload_block.end(),
                  [](const Insn& insn) { return insn.kind == Insn::Kind::kCall; });
  if (has_call) {
    struct CallerSaveSlotInfo {
      bool ok = true;
      int regno = -1;
      bool saw_store = false;
      bool saw_load = false;
      bool saw_store_before_call = false;
      bool saw_load_after_call = false;
    };
    absl::btree_map<int64_t, CallerSaveSlotInfo> cs_info;
    bool seen_any_call = false;
    for (const Insn& insn : post_reload_block) {
      if (insn.kind == Insn::Kind::kCall) {
        seen_any_call = true;
        for (auto& [off, info] : cs_info) {
          if (info.saw_store) {
            info.saw_store_before_call = true;
          }
        }
        continue;
      }
      for (const RtlRegNote& note : insn.pn.reg_notes) {
        if ((note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegDead) &&
            note.datum.op == RtlOp::kMem && !note.datum.children.empty()) {
          int base = -1;
          int64_t off = 0;
          if (ParseStackBaseOffset(note.datum.children[0], &base, &off, frame_pointer_needed) &&
              off >= min_off && off < max_off) {
            cs_info[off].ok = false;
          }
        }
      }
      const RtlExpr& pat = insn.pn.pattern;
      if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
        int base = -1;
        int64_t off = 0;
        if (pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
            !pat.SetDest().flags.volatil && !pat.SetDest().flags.in_struct &&
            !pat.SetDest().flags.unchanging && pat.SetSrc().op == RtlOp::kReg &&
            ParseStackBaseOffset(pat.SetDest().children[0], &base, &off, frame_pointer_needed) &&
            off >= min_off && off < max_off) {
          const int r = pat.SetSrc().RegNo();
          CallerSaveSlotInfo& info = cs_info[off];
          if (r >= 0 && r < kMipsFirstPseudoRegister && mm.CallUsed(r) &&
              (info.regno < 0 || info.regno == r)) {
            info.regno = r;
            info.saw_store = true;
          } else {
            info.ok = false;
          }
          continue;
        }
        if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kMem &&
            !pat.SetSrc().children.empty() && !pat.SetSrc().flags.volatil &&
            !pat.SetSrc().flags.in_struct && !pat.SetSrc().flags.unchanging &&
            ParseStackBaseOffset(pat.SetSrc().children[0], &base, &off, frame_pointer_needed) &&
            off >= min_off && off < max_off) {
          const int r = pat.SetDest().RegNo();
          CallerSaveSlotInfo& info = cs_info[off];
          if (r >= 0 && r < kMipsFirstPseudoRegister && mm.CallUsed(r) &&
              (info.regno < 0 || info.regno == r)) {
            info.regno = r;
            info.saw_load = true;
            if (seen_any_call) {
              info.saw_load_after_call = true;
            }
          } else {
            info.ok = false;
          }
          continue;
        }
      }
      pat.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kMem && !node.children.empty()) {
          int base = -1;
          int64_t off = 0;
          if (ParseStackBaseOffset(node.children[0], &base, &off, frame_pointer_needed) &&
              off >= min_off && off < max_off) {
            cs_info[off].ok = false;
          }
        }
      });
    }
    for (const auto& [off, info] : cs_info) {
      if (info.ok && info.saw_store && info.saw_load && info.saw_store_before_call &&
          info.saw_load_after_call) {
        caller_save_offsets.insert(off);
      }
    }
  }
  struct SlotCheck {
    bool hard_invalidated = false;
    bool non_bigend_off = false;
    bool has_access = false;
    RtlMachineMode mode = RtlMachineMode::kVoid;
    int64_t exact_offset = -1;
  };
  std::vector<SlotCheck> slots(static_cast<size_t>(num_slots));
  auto record_mem = [&](const RtlExpr& mem, bool force_invalidate) {
    if (mem.op != RtlOp::kMem || mem.children.empty()) {
      return;
    }
    int base = -1;
    int64_t off = 0;
    if (!ParseStackBaseOffset(mem.children[0], &base, &off, frame_pointer_needed) ||
        off < min_off || off >= max_off) {
      return;
    }
    if (!force_invalidate && caller_save_offsets.contains(off)) {
      return;
    }
    const int sz = std::max(1, ModeByteSize(mem.mode));
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
    const int64_t expected_off = min_off + 8 * int64_t{first_s} + (8 - sz);
    SlotCheck& sc = slots[static_cast<size_t>(first_s)];
    if (off != expected_off) {
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
  for (const Insn& insn : post_reload_block) {
    insn.pn.pattern.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kMem) {
        record_mem(node, /*force_invalidate=*/false);
      }
    });
    for (const RtlRegNote& note : insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegEquiv && note.datum.op == RtlOp::kMem) {
        record_mem(note.datum, /*force_invalidate=*/true);
      }
    }
  }
  bool any_hard_invalidated = false;
  for (int s = 0; s < num_slots; ++s) {
    if (slots[static_cast<size_t>(s)].hard_invalidated) {
      any_hard_invalidated = true;
      break;
    }
  }
  int non_inv_begin = num_slots;
  while (non_inv_begin > 0) {
    const SlotCheck& sc = slots[static_cast<size_t>(non_inv_begin - 1)];
    if (sc.hard_invalidated || (any_hard_invalidated && sc.non_bigend_off)) {
      break;
    }
    --non_inv_begin;
  }
  return min_off + 8 * int64_t{non_inv_begin};
}

IntraBlockSpillSlotClassification ClassifyIntraBlockSpillAndCallerSaveSlots(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm, bool has_stack_addr_escape,
    std::vector<bool>* is_spill_overhead, bool frame_pointer_needed = true,
    const PostReloadStackLayout& stack_layout = {}) {
  IntraBlockSpillSlotClassification out;
  const int n = static_cast<int>(post_reload_block.size());
  const int64_t min_valid_reload_spill_off = std::max(
      ComputeMinValidReloadSpillOffset(post_reload_block, stack_layout, mm, frame_pointer_needed),
      stack_layout.valid ? stack_layout.min_valid_reload_spill_offset : int64_t{0});
  absl::btree_set<int64_t> mem_reg_dead_slot_offsets;
  for (int i = 0; i < n; ++i) {
    for (const RtlRegNote& note : post_reload_block[static_cast<size_t>(i)].pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem &&
          !note.datum.children.empty()) {
        int base = -1;
        int64_t off = 0;
        if (ParseStackBaseOffset(note.datum.children[0], &base, &off, frame_pointer_needed)) {
          mem_reg_dead_slot_offsets.insert(off);
        }
      }
    }
    if ((*is_spill_overhead)[static_cast<size_t>(i)]) {
      continue;
    }
    if (auto acc = TryParsePlainStackSlotMove(post_reload_block[static_cast<size_t>(i)],
                                              frame_pointer_needed);
        acc.has_value()) {
      if (acc->is_store) {
        out.slot_stores[acc->key].push_back({i, acc->regno});
        out.spill_store_reg[i] = acc->regno;
      } else {
        out.slot_loads[acc->key].push_back({i, acc->regno});
      }
    }
  }
  for (const auto& [key, stores] : out.slot_stores) {
    const auto lit = out.slot_loads.find(key);
    if (lit == out.slot_loads.end() || stores.empty()) {
      continue;
    }
    const bool valid_reload_spill_key =
        !stack_layout.valid || mem_reg_dead_slot_offsets.contains(key.offset) ||
        (key.offset >= min_valid_reload_spill_off && key.offset < stack_layout.frame_size);
    const int first_store_idx = stores.front().first;
    bool has_following_load = false;
    for (const auto& [l_idx, l_reg] : lit->second) {
      if (l_idx > first_store_idx) {
        has_following_load = true;
        break;
      }
    }
    if (!has_following_load) {
      continue;
    }
    for (const auto& [s_idx, s_reg] : stores) {
      for (const auto& [l_idx, l_reg] : lit->second) {
        if (s_idx >= l_idx) {
          continue;
        }
        bool saw_call = false;
        bool redef_in_span = false;
        bool s_reg_clobbered_in_span = false;
        bool saw_callee_saved_in_class = false;
        const bool is_fp_slot = (s_reg >= kMipsFpRegFirst && s_reg <= kMipsFpRegLast);
        for (int k = s_idx + 1; k < l_idx; ++k) {
          const Insn& mid = post_reload_block[static_cast<size_t>(k)];
          if (mid.kind == Insn::Kind::kCall) {
            saw_call = true;
            continue;
          }
          absl::btree_set<int> mf;
          absl::btree_set<int> mp;
          CollectFullRegDefs(mid.pn.pattern, &mf, &mp);
          if (mf.contains(s_reg) || mp.contains(s_reg)) {
            s_reg_clobbered_in_span = true;
            if (s_reg == l_reg) {
              redef_in_span = true;
            }
          }
          for (int d : mf) {
            if (!IsFixedOrSpecialHardReg(d, frame_pointer_needed) && d > 0 &&
                d < kMipsFirstPseudoRegister &&
                ((d >= kMipsFpRegFirst && d <= kMipsFpRegLast) == is_fp_slot) && !mm.CallUsed(d)) {
              saw_callee_saved_in_class = true;
            }
          }
        }
        // `caller-save.c:410-462` (`save_call_clobbered_regs` /
        // `restore_referenced_regs`) only saves call-used hard regs that stay
        // live (unredefined) until restored and never carry a `reload1.c:2309`
        // (`alter_reg`) `(REG_DEAD (mem [sp/fp+off]))` spilled-pseudo note.
        if (s_reg == l_reg && mm.CallUsed(s_reg) && saw_call && !redef_in_span &&
            !mem_reg_dead_slot_offsets.contains(key.offset)) {
          (*is_spill_overhead)[static_cast<size_t>(s_idx)] = true;
          (*is_spill_overhead)[static_cast<size_t>(l_idx)] = true;
          out.preserved_across_call_ranges.push_back({s_idx, l_idx, s_reg});
          out.caller_save_store_indices.insert(s_idx);
        } else if (valid_reload_spill_key) {
          (*is_spill_overhead)[static_cast<size_t>(s_idx)] = true;
          (*is_spill_overhead)[static_cast<size_t>(l_idx)] = true;
          // Per `reload1.c:2309-2400, 5641-6160` (`alter_reg` /
          // `emit_reload_insns`), whenever an intra-block reload spill store/load
          // pair is folded out of the `sched1` view and carries a proven spill
          // death note or clobbers `s_reg` before reloading into `l_reg`,
          // `FoldReloadSpillsForSched1View` must remap the store producer and
          // load consumer onto a common spilled pseudo `P_slot` so pre-reload
          // data-dependence chains are preserved (excluding trivial adjacent
          // leaf stack-struct copies with no intervening clobber).
          const bool proven_intra_spill =
              mem_reg_dead_slot_offsets.contains(key.offset) || s_reg_clobbered_in_span;
          if (saw_call || (proven_intra_spill && (s_reg != l_reg || redef_in_span)) ||
              (!has_stack_addr_escape && saw_callee_saved_in_class)) {
            out.intra_reload_spill_store_indices.insert(s_idx);
            out.intra_reload_spill_load_indices.insert(l_idx);
          }
        }
      }
    }
  }
  return out;
}

bool ExtractBaseMemOffset(const RtlExpr& mem, int base_regno, bool is_argp, int64_t* off_out) {
  if (mem.op != RtlOp::kMem || mem.children.empty()) {
    return false;
  }
  const RtlExpr& addr = mem.children[0];
  auto matches_base = [&](const RtlExpr& e) {
    if (e.op != RtlOp::kReg) {
      return false;
    }
    if (is_argp) {
      return e.IsArgPointerReg();
    }
    return !e.IsArgPointerReg() && e.RegNo() == base_regno;
  };
  if (matches_base(addr)) {
    *off_out = 0;
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && matches_base(addr.children[0]) &&
      addr.children[1].op == RtlOp::kConstInt) {
    *off_out = addr.children[1].int_val;
    return true;
  }
  return false;
}

// Normalizes a `MEM` expression's stack base (`$sp`/`$fp`/`argp`) to a common
// pre-reload representation (`mips.h:1649-1651, 1756-1764`, `reload1.c:2641`
// `eliminate_regs`) so post-reload `$sp`-eliminated `MEM` operands preserve
// distinct pre-reload `$fp`/`argp`/`$sp` base-register aliasing in `sched1` and
// compare equal to un-eliminated `REG_EQUIV` note datums.
RtlExpr NormalizeMemForRegEquivMatch(const RtlExpr& mem, const PostReloadStackLayout& layout) {
  if (!layout.valid || mem.op != RtlOp::kMem || mem.children.empty()) {
    return mem;
  }
  const int elim_to_reg =
      layout.frame_pointer_needed ? kMipsFramePointerRegno : kMipsStackPointerRegno;
  int64_t off = 0;
  if (!ExtractBaseMemOffset(mem, elim_to_reg, /*is_argp=*/false, &off)) {
    return mem;
  }
  RtlExpr out = mem;
  if (off >= layout.frame_size) {
    const int64_t arg_off = off - layout.frame_size;
    out.children[0] = (arg_off == 0) ? RtlExpr::MakeArgPointerReg()
                                     : RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi,
                                                           RtlExpr::MakeArgPointerReg(),
                                                           RtlExpr::MakeConstInt(arg_off));
    return out;
  }
  if (!layout.frame_pointer_needed && (off >= layout.starting_frame_offset || off < 0)) {
    out.children[0] =
        (off == 0)
            ? RtlExpr::MakeReg(RtlMachineMode::kSi, kMipsFramePointerRegno)
            : RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSi,
                                  RtlExpr::MakeReg(RtlMachineMode::kSi, kMipsFramePointerRegno),
                                  RtlExpr::MakeConstInt(off));
    return out;
  }
  return out;
}

void UneliminateStackMemsForSched1(RtlExpr* expr, const PostReloadStackLayout& layout) {
  if (!layout.valid || expr == nullptr) {
    return;
  }
  if (expr->op == RtlOp::kMem) {
    *expr = NormalizeMemForRegEquivMatch(*expr, layout);
    return;
  }
  for (RtlExpr& ch : expr->children) {
    UneliminateStackMemsForSched1(&ch, layout);
  }
}

// Derives `live_at_end_s1` and `facts_s1` (`reg_n_sets`, `reg_n_calls_crossed`,
// `max_reg_num`) for a projected pre-reload `Sched1ProjectionView`
// (`flow.c:2065`, `sched.c:1732, 2469-2501`).
// Mirrors `flow.c:1916-1958, 2035-2049, 2065` (`mark_set_regs` / `mark_set_1`):
// increments `reg_n_sets` for every `SET`/`CLOBBER` destination in `pat`,
// expanding multi-register hard-register destinations across
// `[regno, regno + HARD_REGNO_NREGS(regno, mode))` so `sched.c:2469-2501`
// (`birthing_insn_p`) observes exact `reg_n_sets[i]` for sub-registers of
// wide hard-register writes (e.g., `DFmode`/`DImode` argument registers).
void CountFlowMarkSet1RegNSets(const RtlExpr& pat, const MachineModel& mm,
                               absl::btree_map<int, int>* reg_n_sets) {
  auto count_one = [&](const RtlExpr& elem) {
    if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
      return;
    }
    const RtlExpr* reg = &elem.children[0];
    while ((reg->op == RtlOp::kSubreg || reg->op == RtlOp::kZeroExtract ||
            reg->op == RtlOp::kSignExtract || reg->op == RtlOp::kStrictLowPart) &&
           !reg->children.empty()) {
      reg = &reg->children[0];
    }
    if (reg->op != RtlOp::kReg) {
      return;
    }
    const int regno = reg->RegNo();
    if (regno < 0) {
      return;
    }
    if (regno < kMipsFirstPseudoRegister) {
      const int nregs = std::max(1, mm.HardRegNRegs(regno, reg->mode));
      for (int k = 0; k < nregs && regno + k < kMipsFirstPseudoRegister; ++k) {
        (*reg_n_sets)[regno + k] += 1;
      }
    } else {
      (*reg_n_sets)[regno] += 1;
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      count_one(ch);
    }
  } else {
    count_one(pat);
  }
}

void DeriveSched1ViewFacts(const Sched1ProjectionContext& context, const MachineModel& mm,
                           const absl::btree_set<int>& live_at_end_s2,
                           const absl::btree_map<int, int>& cur_pseudo,
                           const absl::btree_set<int>& live_in_pseudos,
                           const absl::btree_map<int, int>& live_in_reg_for_hard_reg,
                           const absl::btree_map<SpillPseudoKey, int>& spill_slot_pseudo,
                           const absl::btree_set<int>& spill_live_out_pseudos, int next_pseudo,
                           Sched1ProjectionView* view) {
  const int m = static_cast<int>(view->core_insns.size());
  for (int r : live_at_end_s2) {
    if (const auto it = cur_pseudo.find(r); it != cur_pseudo.end()) {
      view->live_at_end_s1.insert(it->second);
    } else {
      view->live_at_end_s1.insert(r);
    }
  }
  // Mirrors `sched.c:2325-2367, 2469-2501`: trailing `(use (reg/i ...))`
  // return-value markers at `[bounds.epilogue_begin .. n)` make the return
  // register live at the end of the schedulable body for `birthing_insn_p`.
  for (int r : context.epilogue_use_regs) {
    if (const auto it = cur_pseudo.find(r); it != cur_pseudo.end()) {
      view->live_at_end_s1.insert(it->second);
    } else {
      view->live_at_end_s1.insert(r);
    }
  }
  for (int sp_p : spill_live_out_pseudos) {
    view->live_at_end_s1.insert(sp_p);
  }
  view->facts_s1.max_reg_num = next_pseudo;

  absl::btree_map<int, int> first_touch;
  absl::btree_map<int, int> last_touch;
  absl::btree_map<int, RtlMachineMode> view_reg_mode;
  std::vector<int> call_indices;
  for (int ci = 0; ci < m; ++ci) {
    const Insn& insn = view->core_insns[static_cast<size_t>(ci)];
    if (insn.kind == Insn::Kind::kCall) {
      call_indices.push_back(ci);
    }
    insn.pn.pattern.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kReg && node.RegNo() >= kMipsFirstPseudoRegister &&
          node.mode != RtlMachineMode::kVoid) {
        view_reg_mode.try_emplace(node.RegNo(), node.mode);
      }
    });
    CountFlowMarkSet1RegNSets(insn.pn.pattern, mm, &view->facts_s1.reg_n_sets);
    absl::btree_set<int> full_defs;
    absl::btree_set<int> partial_defs;
    CollectFullRegDefs(insn.pn.pattern, &full_defs, &partial_defs);
    for (int r : full_defs) {
      first_touch.try_emplace(r, ci);
      last_touch[r] = ci;
    }
    for (int r : partial_defs) {
      first_touch.try_emplace(r, ci);
      last_touch[r] = ci;
    }
    absl::btree_set<int> reads;
    CollectRegReads(insn.pn.pattern, &reads);
    for (int r : reads) {
      first_touch.try_emplace(r, ci);
      last_touch[r] = ci;
    }
  }
  for (int r : live_in_pseudos) {
    first_touch[r] = -1;
  }
  if (!context.has_cfg_context) {
    for (int r : live_in_pseudos) {
      // Standalone single-block fallback: assume live-in pseudos were defined in a predecessor.
      view->facts_s1.reg_n_sets[r] += 1;
    }
  } else {
    // Whole-function `flow.c` / `sched.c:2469-2501` `reg_n_sets` accounting:
    // add cross-block reaching defs once per cross-block web (so incoming params with
    // 0 prior sets keep `reg_n_sets == 1`, while multi-block loop/join regs get `>= 2`).
    auto lookup_mode_map = [&](const absl::btree_map<std::pair<int, RtlMachineMode>, int>& m_map,
                               int hr, int view_reg) -> int {
      if (view_reg >= kMipsFirstPseudoRegister) {
        if (const auto mit = view_reg_mode.find(view_reg); mit != view_reg_mode.end()) {
          int sum = 0;
          if (const auto it = m_map.find({hr, mit->second}); it != m_map.end()) {
            sum += it->second;
          }
          if (const auto wit = m_map.find({hr, RtlMachineMode::kVoid}); wit != m_map.end()) {
            sum += wit->second;
          }
          return sum;
        }
      }
      int best = 0;
      for (const auto& [k, cnt] : m_map) {
        if (k.first == hr) {
          best = std::max(best, cnt);
        }
      }
      return best;
    };
    auto lookup_cross_block_reaching = [&](int hr, int view_reg, bool is_entry_endpoint) -> int {
      if (context.has_per_endpoint_web_defs) {
        return lookup_mode_map(
            is_entry_endpoint ? context.entry_web_other_defs : context.exit_web_other_defs, hr,
            view_reg);
      }
      if (context.has_reaching_defs_by_mode && view_reg >= kMipsFirstPseudoRegister) {
        return lookup_mode_map(context.cross_block_reaching_defs_by_mode, hr, view_reg);
      }
      const auto it = context.cross_block_reaching_defs.find(hr);
      return (it != context.cross_block_reaching_defs.end()) ? it->second : 0;
    };
    absl::btree_set<int> added_cross_block;
    for (const auto& [hr, view_reg] : live_in_reg_for_hard_reg) {
      if (const int reach = lookup_cross_block_reaching(hr, view_reg, /*is_entry_endpoint=*/true);
          reach > 0) {
        view->facts_s1.reg_n_sets[view_reg] += reach;
        added_cross_block.insert(view_reg);
      }
    }
    for (int hr : live_at_end_s2) {
      const auto pit = cur_pseudo.find(hr);
      const int view_reg = (pit != cur_pseudo.end()) ? pit->second : hr;
      if (!added_cross_block.contains(view_reg) && view->facts_s1.reg_n_sets.contains(view_reg)) {
        if (const int reach =
                lookup_cross_block_reaching(hr, view_reg, /*is_entry_endpoint=*/false);
            reach > 0) {
          view->facts_s1.reg_n_sets[view_reg] += reach;
          added_cross_block.insert(view_reg);
        }
      }
    }
    // `flow.c:2065` (`mark_set_1`) counts every explicit `SET` function-wide for
    // unrenamed hard regs (both `/i` return-value regs and ABI call-arg regs in
    // `abi_pinned_defs`), so `sched.c:2469-2501` (`birthing_insn_p`) sees the
    // true whole-function `reg_n_sets[hr]` for every unrenamed hard reg.
    std::vector<int> unrenamed_hard_defs;
    const bool fp_needed = !context.stack_layout.valid || context.stack_layout.frame_pointer_needed;
    for (const auto& [r, cnt] : view->facts_s1.reg_n_sets) {
      if (r >= 0 && r < kMipsFirstPseudoRegister && !IsFixedOrSpecialHardReg(r, fp_needed) &&
          !added_cross_block.contains(r)) {
        unrenamed_hard_defs.push_back(r);
      }
    }
    for (int hr : unrenamed_hard_defs) {
      if (const auto it = context.other_block_total_defs.find(hr);
          it != context.other_block_total_defs.end() && it->second > 0) {
        view->facts_s1.reg_n_sets[hr] += it->second;
        added_cross_block.insert(hr);
      }
    }
    for (const auto& [slot_key, sp_p] : spill_slot_pseudo) {
      if (slot_key.second == -1 || spill_live_out_pseudos.contains(sp_p)) {
        if (const auto it = context.other_block_spill_store_counts.find(slot_key.first);
            it != context.other_block_spill_store_counts.end() && it->second > 0) {
          view->facts_s1.reg_n_sets[sp_p] += it->second;
        }
      }
    }
  }
  for (int r : view->live_at_end_s1) {
    last_touch[r] = m;
  }
  for (const auto& [r, first_ci] : first_touch) {
    if (r < kMipsFirstPseudoRegister) {
      continue;
    }
    const int last_ci = last_touch[r];
    int crossed = 0;
    for (int call_ci : call_indices) {
      if (first_ci < call_ci && call_ci < last_ci) {
        ++crossed;
      }
    }
    if (crossed > 0) {
      view->facts_s1.reg_n_calls_crossed[r] = crossed;
    }
  }
}

// Block-0 leading entry-parameter copy pinning (`sched.c:3191-3215`):
// excludes leading `(set dst src)` insns whose unwrapped `src` is a hard reg
// from `view->core_insns` (after recording their `facts_s1` defs) so
// `Schedule`/`IsAchievable` matches `schedule_block`'s advanced `head`.
void PinLeadingEntryParamCopiesForSched1View(Sched1ProjectionView* view) {
  if (view->core_insns.size() <= 1) {
    return;
  }
  absl::btree_set<int> word_copied_args;
  size_t pinned_count = 0;
  while (pinned_count + 1 < view->core_insns.size()) {
    const Insn& ci_insn = view->core_insns[pinned_count];
    if (ci_insn.kind != Insn::Kind::kInsn || ci_insn.pn.pattern.op != RtlOp::kSet ||
        ci_insn.pn.pattern.children.size() < 2) {
      break;
    }
    const RtlExpr& pat = ci_insn.pn.pattern;
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
    ++pinned_count;
  }
  if (pinned_count == 0) {
    return;
  }
  view->pinned_prefix_orig_indices.assign(
      view->core_to_orig_index.begin(),
      view->core_to_orig_index.begin() + static_cast<ptrdiff_t>(pinned_count));
  view->core_insns.erase(view->core_insns.begin(),
                         view->core_insns.begin() + static_cast<ptrdiff_t>(pinned_count));
  view->core_to_orig_index.erase(
      view->core_to_orig_index.begin(),
      view->core_to_orig_index.begin() + static_cast<ptrdiff_t>(pinned_count));
  for (size_t j = 0; j < view->core_insns.size(); ++j) {
    view->core_insns[j].luid = static_cast<int>(j);
  }
  const int pcount_i = static_cast<int>(pinned_count);
  absl::btree_map<int, int> shifted_output_reload;
  for (const auto& [orig_i, core_i] : view->output_reload_after_core) {
    if (core_i >= pcount_i) {
      shifted_output_reload[orig_i] = core_i - pcount_i;
    } else {
      view->pinned_prefix_orig_indices.push_back(orig_i);
    }
  }
  view->output_reload_after_core = std::move(shifted_output_reload);
  absl::btree_map<int, int> shifted_input_reload;
  for (const auto& [orig_i, core_i] : view->input_reload_before_core) {
    if (core_i >= pcount_i) {
      shifted_input_reload[orig_i] = core_i - pcount_i;
    } else {
      view->pinned_prefix_orig_indices.push_back(orig_i);
    }
  }
  view->input_reload_before_core = std::move(shifted_input_reload);
}

// Identifies non-entry-block hard registers that represent a single
// loop-carried / cross-block multi-def user variable (`flow.c:2065`
// `mark_set_1`, `sched.c:2469-2501` `birthing_insn_p`, `global.c:find_reg`),
// mirroring Stage 4's `CoalesceSameBlockDisjointGlobalChainPseudos`: live-in
// (read before first write), live-out (`live_at_end_s2`), and redefined in the
// block with uniform mode and without `REG_EQUIV`/`REG_EQUAL`, self-`REG_DEAD`,
// ABI pinning, or intervening call clobbers.
absl::btree_set<int> FindCoalescedGlobalCarrierHardRegs(
    absl::Span<const Insn> post_reload_block, absl::Span<const int> core_to_orig_index,
    const std::vector<absl::btree_set<int>>& abi_pinned_defs,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context,
    const MachineModel& mm) {
  absl::btree_set<int> result;
  if (!context.has_cfg_context || context.is_entry_block) {
    return result;
  }
  const int m = static_cast<int>(core_to_orig_index.size());
  bool has_call = false;
  for (int ci = 0; ci < m; ++ci) {
    if (post_reload_block[static_cast<size_t>(core_to_orig_index[ci])].kind == Insn::Kind::kCall) {
      has_call = true;
      break;
    }
  }
  const bool fp_needed = !context.stack_layout.valid || context.stack_layout.frame_pointer_needed;
  for (int r : live_at_end_s2) {
    if (r < 2 || r >= kMipsFirstPseudoRegister || IsFixedOrSpecialHardReg(r, fp_needed) ||
        (has_call && mm.CallUsed(r))) {
      continue;
    }
    if (context.has_per_endpoint_web_defs && !context.entry_exit_same_web_regs.contains(r)) {
      continue;
    }
    bool saw_read_before_def = false;
    bool saw_reg_dead = false;
    bool saw_def_after_reg_dead = false;
    bool saw_def = false;
    bool disqualified = false;
    RtlMachineMode expected_mode = RtlMachineMode::kVoid;
    for (int ci = 0; ci < m && !disqualified; ++ci) {
      const Insn& insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
      if (insn.kind == Insn::Kind::kCall || insn.pn.pattern.op == RtlOp::kUse) {
        continue;
      }
      absl::btree_set<int> reads;
      CollectRegReads(insn.pn.pattern, &reads);
      if (reads.contains(r) && !saw_def) {
        saw_read_before_def = true;
      }
      insn.pn.pattern.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kReg && node.RegNo() == r && node.mode != RtlMachineMode::kVoid) {
          if (expected_mode == RtlMachineMode::kVoid) {
            expected_mode = node.mode;
          } else if (expected_mode != node.mode) {
            disqualified = true;
          }
        }
      });
      absl::btree_set<int> fdefs;
      absl::btree_set<int> pdefs;
      CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
      if (pdefs.contains(r)) {
        disqualified = true;
        break;
      }
      if (fdefs.contains(r)) {
        if (!saw_read_before_def || abi_pinned_defs[static_cast<size_t>(ci)].contains(r) ||
            HasRegDeadNoteCovering(insn.pn.reg_notes, r, mm)) {
          disqualified = true;
          break;
        }
        const RtlExpr* s = SingleSetOf(insn.pn.pattern);
        if (s == nullptr || s->SetDest().op != RtlOp::kReg || s->SetDest().RegNo() != r ||
            s->SetDest().flags.integrated) {
          disqualified = true;
          break;
        }
        for (const RtlRegNote& n : insn.pn.reg_notes) {
          if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual ||
              n.kind == RtlMachineMode::kRegNoConflict) {
            disqualified = true;
            break;
          }
        }
        if (saw_reg_dead) {
          saw_def_after_reg_dead = true;
        }
        saw_def = true;
      } else if (reads.contains(r) && HasRegDeadNoteCovering(insn.pn.reg_notes, r, mm)) {
        saw_reg_dead = true;
      }
    }
    if (!disqualified && saw_read_before_def && saw_def && saw_def_after_reg_dead) {
      result.insert(r);
    }
  }
  return result;
}

void CollectDirectNonMemRegReads(const RtlExpr& x,
                                 std::vector<std::pair<int, RtlMachineMode>>* regs) {
  if (x.op == RtlOp::kMem) {
    return;
  }
  if (x.op == RtlOp::kReg) {
    regs->push_back({x.RegNo(), x.mode});
    return;
  }
  for (const RtlExpr& child : x.children) {
    CollectDirectNonMemRegReads(child, regs);
  }
}

// Mirrors `local-alloc.c:1266-1297, 1369-1375, 1765-1820, 2145-2182` and
// `config/mips/mips.h:1280` (`MODES_TIEABLE_P`): when a local single-block
// `SET` of hard register `r` reads a different genuine unrenamed physical hard
// register `s != r` (either an entry-block incoming ABI argument or a prior
// unrenamed hard-reg def in this block, never a cross-block global pseudo per
// `local-alloc.c:1765`) of the same bank/width that dies at `ci` (`REG_DEAD s`),
// `combine_regs` records `s` in `qty_phys_sugg` / `qty_phys_copy_sugg` and
// `wipe_dead_reg` clears `s` from `regs_live_at[2 * ci]`. If `s` remains free
// across `r`'s local live range `(ci, death_ci)` and `death_ci` does not itself
// suggest `r`, `find_free_reg` (`just_try_suggested = 1`) would allocate `s`
// instead of `r`; Stage 4 (`InvertAlterReg`) therefore keeps `r` unrenamed on
// physical hard register `r`.
bool HasLocalDyingHardRegSuggestionConflict(
    absl::Span<const Insn> post_reload_block, absl::Span<const int> core_to_orig_index, int ci,
    int r, const absl::btree_set<int>& reads, const absl::btree_map<int, int>& read_map,
    const absl::btree_set<int>& unrenamed_hard_def_regs, bool is_entry_block,
    const absl::btree_set<int>& live_at_end_s2, const MachineModel& mm,
    bool frame_pointer_needed = true) {
  if (r < 0 || r >= kMipsFirstPseudoRegister || IsFixedOrSpecialHardReg(r, frame_pointer_needed) ||
      reads.contains(r)) {
    return false;
  }
  const Insn& def_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
  if (def_insn.kind == Insn::Kind::kCall) {
    return false;
  }
  const RtlExpr* sset = SingleSetOf(def_insn.pn.pattern);
  if (sset == nullptr || sset->SetDest().op != RtlOp::kReg || sset->SetDest().RegNo() != r) {
    return false;
  }
  for (const RtlRegNote& note : def_insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const RtlMachineMode r_mode = sset->SetDest().mode;
  const int r_nregs = std::max(1, mm.HardRegNRegs(r, r_mode));
  const bool r_is_fp = (r >= kMipsFpRegFirst && r < kMipsMdRegFirst);
  if (r >= kMipsMdRegFirst) {
    return false;
  }
  std::vector<std::pair<int, RtlMachineMode>> direct_srcs;
  CollectDirectNonMemRegReads(sset->SetSrc(), &direct_srcs);
  std::vector<std::pair<int, int>> cand_suggs;  // (s_reg, s_nregs)
  for (const auto& [s, s_mode] : direct_srcs) {
    if (s < 0 || s >= kMipsMdRegFirst || s == r ||
        IsFixedOrSpecialHardReg(s, frame_pointer_needed)) {
      continue;
    }
    if (read_map.contains(s)) {
      continue;
    }
    const bool is_genuine_hard_reg =
        unrenamed_hard_def_regs.contains(s) || (is_entry_block && IsAbiCallInterfaceHardReg(s));
    if (!is_genuine_hard_reg) {
      continue;
    }
    const bool s_is_fp = (s >= kMipsFpRegFirst);
    const int s_nregs = std::max(1, mm.HardRegNRegs(s, s_mode));
    if (s_is_fp != r_is_fp || s_nregs != r_nregs) {
      continue;
    }
    if (!HasRegDeadNoteCovering(def_insn.pn.reg_notes, s, mm)) {
      continue;
    }
    cand_suggs.push_back({s, s_nregs});
  }
  if (cand_suggs.empty()) {
    return false;
  }
  const int m = static_cast<int>(core_to_orig_index.size());
  int death_ci = -1;
  bool terminated_in_block = false;
  for (int k = ci + 1; k < m; ++k) {
    const Insn& k_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
    if (k_insn.kind == Insn::Kind::kCall) {
      absl::btree_set<int> fu_reads;
      for (const RtlExpr& fu : k_insn.pn.call_fusage) {
        CollectRegReads(fu, &fu_reads);
      }
      for (int w = 0; w < r_nregs; ++w) {
        if (fu_reads.contains(r + w)) {
          death_ci = k;
        }
      }
      if (mm.CallUsed(r)) {
        terminated_in_block = true;
        break;
      }
      continue;
    }
    absl::btree_set<int> k_reads;
    CollectRegReads(k_insn.pn.pattern, &k_reads);
    bool reads_r = false;
    for (int w = 0; w < r_nregs; ++w) {
      if (k_reads.contains(r + w)) {
        reads_r = true;
        break;
      }
    }
    absl::btree_set<int> k_fdefs;
    absl::btree_set<int> k_pdefs;
    CollectFullRegDefs(k_insn.pn.pattern, &k_fdefs, &k_pdefs);
    bool defs_r = false;
    for (int w = 0; w < r_nregs; ++w) {
      if (k_fdefs.contains(r + w) || k_pdefs.contains(r + w)) {
        defs_r = true;
        break;
      }
    }
    // `optabs.c:2485-2525` (`emit_no_conflict_block`) + `local-alloc.c:1317-1352`:
    // a `REG_LIBCALL` `(clobber (reg:DI dh))` overlapping `r` does not end `r`'s
    // live range when subsequent subword insns read `r` under `REG_NO_CONFLICT`.
    if (!reads_r && defs_r && k_insn.pn.pattern.op == RtlOp::kClobber &&
        k_insn.pn.pattern.children.size() == 1 && k_insn.pn.pattern.children[0].op == RtlOp::kReg &&
        k_insn.pn.pattern.children[0].mode == RtlMachineMode::kDi &&
        std::any_of(k_insn.pn.reg_notes.begin(), k_insn.pn.reg_notes.end(),
                    [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegLibcall; })) {
      const int dh = k_insn.pn.pattern.children[0].RegNo();
      bool has_nc_reader = false;
      for (int m_k = k + 1; m_k < m; ++m_k) {
        const Insn& m_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[m_k])];
        for (const RtlRegNote& mn : m_insn.pn.reg_notes) {
          if (mn.kind == RtlMachineMode::kRegNoConflict && mn.datum.op == RtlOp::kReg &&
              mn.datum.RegNo() == r && (dh == r || dh + 1 == r)) {
            has_nc_reader = true;
            break;
          }
        }
        if (has_nc_reader) {
          break;
        }
      }
      if (has_nc_reader) {
        continue;
      }
    }
    if (reads_r) {
      death_ci = k;
      if (HasRegDeadNoteCovering(k_insn.pn.reg_notes, r, mm)) {
        terminated_in_block = true;
        break;
      }
      const bool is_nc_subreg_write = std::any_of(
          k_insn.pn.reg_notes.begin(), k_insn.pn.reg_notes.end(), [r](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegNoConflict && n.datum.op == RtlOp::kReg &&
                   n.datum.RegNo() == r;
          });
      if (defs_r && !is_nc_subreg_write) {
        // Non-dying RMW extends the same pseudo; do not treat as a single-def conflict.
        return false;
      }
    } else if (defs_r) {
      terminated_in_block = true;
      break;
    }
  }
  if (death_ci < 0 || (!terminated_in_block && live_at_end_s2.contains(r))) {
    return false;
  }
  // If `death_ci` itself sets `r` (e.g. return `(set (reg/i r) ...)`), `r` is also
  // suggested at `death_ci` (`local-alloc.c:1824`) and may beat `s`.
  const Insn& death_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[death_ci])];
  if (const RtlExpr* dset = SingleSetOf(death_insn.pn.pattern);
      dset != nullptr && dset->SetDest().op == RtlOp::kReg && dset->SetDest().RegNo() == r) {
    return false;
  }
  for (const auto& [s, s_nregs] : cand_suggs) {
    bool s_busy = false;
    for (int k = ci + 1; k <= death_ci && !s_busy; ++k) {
      const Insn& k_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[k])];
      if (k_insn.kind == Insn::Kind::kCall) {
        if (k < death_ci && mm.CallUsed(s)) {
          s_busy = true;
          break;
        }
        absl::btree_set<int> fu_reads;
        for (const RtlExpr& fu : k_insn.pn.call_fusage) {
          CollectRegReads(fu, &fu_reads);
        }
        for (int w = 0; w < s_nregs; ++w) {
          if (fu_reads.contains(s + w)) {
            s_busy = true;
            break;
          }
        }
        continue;
      }
      absl::btree_set<int> k_reads;
      CollectRegReads(k_insn.pn.pattern, &k_reads);
      for (int w = 0; w < s_nregs; ++w) {
        if (k_reads.contains(s + w)) {
          s_busy = true;
          break;
        }
      }
      if (!s_busy && k < death_ci) {
        absl::btree_set<int> k_fdefs;
        absl::btree_set<int> k_pdefs;
        CollectFullRegDefs(k_insn.pn.pattern, &k_fdefs, &k_pdefs);
        for (int w = 0; w < s_nregs; ++w) {
          if (k_fdefs.contains(s + w) || k_pdefs.contains(s + w)) {
            s_busy = true;
            break;
          }
        }
      }
    }
    if (!s_busy) {
      return true;
    }
  }
  return false;
}

// Computes exact backward hard-register (`0..63`) liveness masks immediately
// AFTER each core instruction in `core_to_orig_index` from `live_at_end_s2`,
// excluding reload-scratch hard registers folded back onto spilled pseudos
// (`reload1.c:2309-2400, 5641-6160`) and preserving call-used hard registers
// saved/restored across `CALL_INSN` (`caller-save.c:349-620`).
std::vector<uint64_t> ComputeCoreHardRegLiveAfterMasks(
    absl::Span<const Insn> post_reload_block, absl::Span<const int> core_to_orig_index,
    const absl::btree_set<int>& live_at_end_s2, const SpillSlotFoldInfo& cross_spill,
    const std::vector<std::tuple<int, int, int>>& preserved_across_call_ranges,
    const MachineModel& mm) {
  const int m = static_cast<int>(core_to_orig_index.size());
  std::vector<uint64_t> live_after(static_cast<size_t>(m), 0);
  uint64_t live = 0;
  for (int r : live_at_end_s2) {
    if (r >= 0 && r < 64) {
      live |= (uint64_t{1} << r);
    }
  }
  for (int k = m - 1; k >= 0; --k) {
    live_after[static_cast<size_t>(k)] = live;
    const int orig_k = core_to_orig_index[static_cast<size_t>(k)];
    const Insn& insn = post_reload_block[static_cast<size_t>(orig_k)];
    if (insn.kind == Insn::Kind::kCall) {
      uint64_t preserved_mask = 0;
      for (const auto& [s_idx, l_idx, s_reg] : preserved_across_call_ranges) {
        if (s_idx < orig_k && orig_k < l_idx && s_reg >= 0 && s_reg < 64) {
          int nr = 1;
          if (s_idx >= 0 && s_idx < static_cast<int>(post_reload_block.size())) {
            const RtlExpr* ss =
                SingleSetOf(post_reload_block[static_cast<size_t>(s_idx)].pn.pattern);
            if (ss != nullptr) {
              nr = std::max(1, mm.HardRegNRegs(s_reg, ss->SetSrc().mode));
            }
          }
          for (int w = 0; w < nr && s_reg + w < 64; ++w) {
            preserved_mask |= (uint64_t{1} << (s_reg + w));
          }
        }
      }
      for (int r = 0; r < 64; ++r) {
        if (mm.CallUsed(r) && (preserved_mask & (uint64_t{1} << r)) == 0) {
          live &= ~(uint64_t{1} << r);
        }
      }
      absl::btree_set<int> fu_reads;
      for (const RtlExpr& fu : insn.pn.call_fusage) {
        CollectRegReads(fu, &fu_reads);
      }
      for (int r : fu_reads) {
        if (r >= 0 && r < 64) {
          live |= (uint64_t{1} << r);
        }
      }
      continue;
    }
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
    if (!cross_spill.store_slot_by_prod.contains(orig_k)) {
      for (int r : fdefs) {
        if (r >= 0 && r < 64) {
          live &= ~(uint64_t{1} << r);
        }
      }
    }
    absl::btree_set<int> k_reads;
    CollectRegReads(insn.pn.pattern, &k_reads);
    if (const auto lit = cross_spill.load_subs_by_cons.find(orig_k);
        lit != cross_spill.load_subs_by_cons.end()) {
      for (const auto& [hr, slot_mode] : lit->second) {
        const int nr = std::max(1, mm.HardRegNRegs(hr, slot_mode.second));
        for (int w = 0; w < nr; ++w) {
          k_reads.erase(hr + w);
        }
      }
    }
    for (int r : k_reads) {
      if (r >= 0 && r < 64) {
        live |= (uint64_t{1} << r);
      }
    }
  }
  return live_after;
}

int LocalAllocQtyPriority(int n_refs, int size_nregs, int span) {
  if (n_refs < 2 || span <= 0) {
    return 0;
  }
  int log2_refs = 0;
  for (int v = n_refs; v > 1; v >>= 1) {
    ++log2_refs;
  }
  return static_cast<int>(
      (static_cast<double>(log2_refs * n_refs * size_nregs) / static_cast<double>(span)) * 10000.0);
}

// Mirrors `local-alloc.c:1512-1539, 1597-1623` (`block_alloc` / `qty_compare_1`):
// checks whether `post_reload_block[core_to_orig_index[k]]` starts a block-local,
// non-call-crossing, non-suggested pseudo on `low_r` whose allocation priority is
// `<= r_pri` (so `q_r`, born earlier at `ci < k`, is allocated before `q_low` and
// sees `low_r` as free during `find_free_reg`). If so, sets `*low_death_out` to
// `q_low`'s death core index and returns true.
bool IsDeferredLowerPriorityLocalDef(absl::Span<const Insn> post_reload_block,
                                     absl::Span<const int> core_to_orig_index,
                                     const SpillSlotFoldInfo& cross_spill, int k, int low_r,
                                     int r_nregs, int r_pri,
                                     const absl::btree_set<int>& live_at_end_s2,
                                     const MachineModel& mm, int* low_death_out) {
  const int orig_k = core_to_orig_index[static_cast<size_t>(k)];
  if (cross_spill.store_slot_by_prod.contains(orig_k)) {
    return false;
  }
  const Insn& def_insn = post_reload_block[static_cast<size_t>(orig_k)];
  if (def_insn.kind == Insn::Kind::kCall) {
    return false;
  }
  const RtlExpr* sset = SingleSetOf(def_insn.pn.pattern);
  if (sset == nullptr || sset->SetDest().op != RtlOp::kReg || sset->SetDest().RegNo() != low_r ||
      sset->SetDest().flags.integrated || sset->SetSrc().op == RtlOp::kReg) {
    return false;
  }
  const int low_nregs = std::max(1, mm.HardRegNRegs(low_r, sset->SetDest().mode));
  if (low_nregs != r_nregs) {
    return false;
  }
  for (const RtlRegNote& note : def_insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const bool low_is_fp = (low_r >= kMipsFpRegFirst && low_r < kMipsMdRegFirst);
  std::vector<std::pair<int, RtlMachineMode>> direct_srcs;
  CollectDirectNonMemRegReads(sset->SetSrc(), &direct_srcs);
  for (const auto& [s, s_mode] : direct_srcs) {
    if (s < 0 || s >= kMipsMdRegFirst) {
      continue;
    }
    const bool s_is_fp = (s >= kMipsFpRegFirst);
    if (s_is_fp == low_is_fp && HasRegDeadNoteCovering(def_insn.pn.reg_notes, s, mm)) {
      return false;
    }
  }
  const int m = static_cast<int>(core_to_orig_index.size());
  int death_j = -1;
  int n_refs = 1;
  bool terminated = false;
  for (int j = k + 1; j < m; ++j) {
    const int orig_j = core_to_orig_index[static_cast<size_t>(j)];
    const Insn& j_insn = post_reload_block[static_cast<size_t>(orig_j)];
    if (j_insn.kind == Insn::Kind::kCall) {
      return false;
    }
    absl::btree_set<int> j_reads;
    CollectRegReads(j_insn.pn.pattern, &j_reads);
    if (const auto lit = cross_spill.load_subs_by_cons.find(orig_j);
        lit != cross_spill.load_subs_by_cons.end()) {
      for (const auto& [hr, slot_mode] : lit->second) {
        const int nr = std::max(1, mm.HardRegNRegs(hr, slot_mode.second));
        for (int w = 0; w < nr; ++w) {
          j_reads.erase(hr + w);
        }
      }
    }
    bool reads_low = false;
    for (int w = 0; w < low_nregs; ++w) {
      if (j_reads.contains(low_r + w)) {
        reads_low = true;
        break;
      }
    }
    absl::btree_set<int> j_fdefs;
    absl::btree_set<int> j_pdefs;
    CollectFullRegDefs(j_insn.pn.pattern, &j_fdefs, &j_pdefs);
    bool defs_low = false;
    for (int w = 0; w < low_nregs; ++w) {
      if (j_fdefs.contains(low_r + w) || j_pdefs.contains(low_r + w)) {
        defs_low = true;
        break;
      }
    }
    const bool spills_dest = cross_spill.store_slot_by_prod.contains(orig_j);
    if (reads_low) {
      if (j_insn.pn.pattern.op == RtlOp::kUse) {
        return false;
      }
      // If `j` copies `low_r` directly to a hard reg (e.g. call arg / return),
      // `q_low` could receive a hard-reg suggestion (`qty_sugg_order`).
      const RtlExpr* j_set = SingleSetOf(j_insn.pn.pattern);
      if (j_set != nullptr && j_set->SetDest().op == RtlOp::kReg &&
          j_set->SetDest().flags.integrated) {
        return false;
      }
      ++n_refs;
      death_j = j;
      if (defs_low && spills_dest) {
        // `local-alloc.c:1851-1886` (`combine_regs`): a dying read feeding a
        // spilled-output def is combined into the spilled pseudo's quantity.
        return false;
      }
      if (HasRegDeadNoteCovering(j_insn.pn.reg_notes, low_r, mm)) {
        if (defs_low) {
          return false;
        }
        terminated = true;
        break;
      }
      if (defs_low) {
        return false;
      }
    } else if (defs_low) {
      terminated = true;
      break;
    }
  }
  if (death_j <= k) {
    return false;
  }
  if (!terminated) {
    for (int w = 0; w < low_nregs; ++w) {
      if (live_at_end_s2.contains(low_r + w)) {
        return false;
      }
    }
  }
  const int low_pri = LocalAllocQtyPriority(n_refs, low_nregs, death_j - k);
  if (low_pri > r_pri) {
    return false;
  }
  *low_death_out = death_j;
  return true;
}

// Mirrors `local-alloc.c:2158-2181, 2243-2263` (`find_free_reg` under
// `#ifndef REG_ALLOC_ORDER` + `post_mark_life`, `config/mips/mips.h:1186-1209, 1255-1274`):
// if a block-local single-set non-call, non-copy definition of hard register `r`
// at `ci` has no dying same-bank operand at `ci` (so `combine_regs` cannot tie
// it to an earlier quantity), dies strictly within the block at `death_ci`
// without crossing a call and without overwriting `r` at `death_ci`, and a
// strictly lower same-bank call-used allocatable register `low_r < r` is free
// across `[ci, death_ci)`, then `find_free_reg` would allocate `low_r` instead
// of `r`; Stage 4/5 therefore keeps `r` unrenamed on physical hard register `r`.
bool HasLocalLowerAllocOrderFreeRegConflict(absl::Span<const Insn> post_reload_block,
                                            absl::Span<const int> core_to_orig_index,
                                            absl::Span<const uint64_t> live_after_core, int ci,
                                            int r, const absl::btree_set<int>& reads,
                                            const absl::btree_set<int>& live_at_end_s2,
                                            const SpillSlotFoldInfo& cross_spill,
                                            const MachineModel& mm) {
  if (r < 2 || r >= kMipsMdRegFirst || IsFixedOrSpecialHardReg(r) || reads.contains(r)) {
    return false;
  }
  const Insn& def_insn = post_reload_block[static_cast<size_t>(core_to_orig_index[ci])];
  if (def_insn.kind == Insn::Kind::kCall) {
    return false;
  }
  const RtlExpr* sset = SingleSetOf(def_insn.pn.pattern);
  if (sset == nullptr || sset->SetDest().op != RtlOp::kReg || sset->SetDest().RegNo() != r ||
      sset->SetDest().flags.integrated || sset->SetSrc().op == RtlOp::kReg) {
    return false;
  }
  for (const RtlRegNote& note : def_insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegNoConflict) {
      return false;
    }
  }
  const RtlMachineMode r_mode = sset->SetDest().mode;
  const int r_nregs = std::max(1, mm.HardRegNRegs(r, r_mode));
  const bool r_is_fp = (r >= kMipsFpRegFirst && r < kMipsMdRegFirst);
  if (!mm.CallUsed(r)) {
    return false;
  }
  // If `def_insn` consumes any dying same-bank direct operand (or folded
  // spilled-pseudo operand), `combine_regs` (`local-alloc.c:1266-1300, 1851-1886`)
  // could merge `r` into an earlier quantity.
  const int orig_ci = core_to_orig_index[static_cast<size_t>(ci)];
  const auto ci_spill_it = cross_spill.load_subs_by_cons.find(orig_ci);
  std::vector<std::pair<int, RtlMachineMode>> direct_srcs;
  CollectDirectNonMemRegReads(sset->SetSrc(), &direct_srcs);
  for (const auto& [s, s_mode] : direct_srcs) {
    if (s < 0 || s >= kMipsMdRegFirst) {
      continue;
    }
    const bool s_is_fp = (s >= kMipsFpRegFirst);
    if (s_is_fp == r_is_fp &&
        (HasRegDeadNoteCovering(def_insn.pn.reg_notes, s, mm) ||
         (ci_spill_it != cross_spill.load_subs_by_cons.end() && ci_spill_it->second.contains(s)))) {
      return false;
    }
  }

  const int m = static_cast<int>(core_to_orig_index.size());
  int death_ci = -1;
  int r_n_refs = 1;
  bool terminated_in_block = false;
  for (int k = ci + 1; k < m; ++k) {
    const int orig_k = core_to_orig_index[static_cast<size_t>(k)];
    const Insn& k_insn = post_reload_block[static_cast<size_t>(orig_k)];
    if (k_insn.kind == Insn::Kind::kCall) {
      return false;
    }
    absl::btree_set<int> k_reads;
    CollectRegReads(k_insn.pn.pattern, &k_reads);
    if (const auto lit = cross_spill.load_subs_by_cons.find(orig_k);
        lit != cross_spill.load_subs_by_cons.end()) {
      for (const auto& [hr, slot_mode] : lit->second) {
        const int nr = std::max(1, mm.HardRegNRegs(hr, slot_mode.second));
        for (int w = 0; w < nr; ++w) {
          k_reads.erase(hr + w);
        }
      }
    }
    bool reads_r = false;
    for (int w = 0; w < r_nregs; ++w) {
      if (k_reads.contains(r + w)) {
        reads_r = true;
        break;
      }
    }
    absl::btree_set<int> k_fdefs;
    absl::btree_set<int> k_pdefs;
    CollectFullRegDefs(k_insn.pn.pattern, &k_fdefs, &k_pdefs);
    bool defs_r = false;
    for (int w = 0; w < r_nregs; ++w) {
      if (k_fdefs.contains(r + w) || k_pdefs.contains(r + w)) {
        defs_r = true;
        break;
      }
    }
    const bool spills_dest = cross_spill.store_slot_by_prod.contains(orig_k);
    if (reads_r) {
      if (k_insn.pn.pattern.op == RtlOp::kUse) {
        return false;
      }
      ++r_n_refs;
      death_ci = k;
      if (defs_r && spills_dest) {
        // `reload1.c:5641-6155` + `local-alloc.c:2158-2181`: when `k`'s output
        // pseudo was spilled (`reg_renumber == -1`), post-reload `k` only writes
        // `r` as a scratch output-reload register for the spill store; if `k`
        // carries `(REG_DEAD r)`, `r`'s local live range terminates at `k`.
        if (HasRegDeadNoteCovering(k_insn.pn.reg_notes, r, mm)) {
          terminated_in_block = true;
          break;
        }
        return false;
      }
      if (HasRegDeadNoteCovering(k_insn.pn.reg_notes, r, mm)) {
        if (defs_r) {
          return false;
        }
        terminated_in_block = true;
        break;
      }
      if (defs_r) {
        return false;
      }
    } else if (defs_r) {
      terminated_in_block = true;
      break;
    }
  }
  if (death_ci <= ci) {
    return false;
  }
  if (!terminated_in_block) {
    for (int w = 0; w < r_nregs; ++w) {
      if (live_at_end_s2.contains(r + w)) {
        return false;
      }
    }
  }

  const int r_pri = LocalAllocQtyPriority(r_n_refs, r_nregs, death_ci - ci);
  const int bank_lo = r_is_fp ? kMipsFpRegFirst : 2;
  const int step = r_is_fp ? (mm.TargetFloat64() ? r_nregs : 2) : (mm.Target64Bit() ? 1 : r_nregs);
  const uint64_t live_after_ci = live_after_core[static_cast<size_t>(ci)];
  for (int low_r = bank_lo; low_r < r; low_r += step) {
    bool candidate_ok = true;
    for (int w = 0; w < r_nregs; ++w) {
      const int sub = low_r + w;
      if (IsFixedOrSpecialHardReg(sub) || !mm.CallUsed(sub) ||
          (live_after_ci & (uint64_t{1} << sub)) != 0) {
        candidate_ok = false;
        break;
      }
    }
    if (!candidate_ok) {
      continue;
    }
    bool low_busy = false;
    for (int k = ci + 1; k <= death_ci && !low_busy; ++k) {
      const int orig_k = core_to_orig_index[static_cast<size_t>(k)];
      const Insn& k_insn = post_reload_block[static_cast<size_t>(orig_k)];
      absl::btree_set<int> k_reads;
      CollectRegReads(k_insn.pn.pattern, &k_reads);
      if (const auto lit = cross_spill.load_subs_by_cons.find(orig_k);
          lit != cross_spill.load_subs_by_cons.end()) {
        for (const auto& [hr, slot_mode] : lit->second) {
          const int nr = std::max(1, mm.HardRegNRegs(hr, slot_mode.second));
          for (int w = 0; w < nr; ++w) {
            k_reads.erase(hr + w);
          }
        }
      }
      for (int w = 0; w < r_nregs; ++w) {
        if (k_reads.contains(low_r + w)) {
          low_busy = true;
          break;
        }
      }
      if (!low_busy && k < death_ci && !cross_spill.store_slot_by_prod.contains(orig_k)) {
        absl::btree_set<int> k_fdefs;
        absl::btree_set<int> k_pdefs;
        CollectFullRegDefs(k_insn.pn.pattern, &k_fdefs, &k_pdefs);
        bool defs_low = false;
        for (int w = 0; w < r_nregs; ++w) {
          if (k_fdefs.contains(low_r + w) || k_pdefs.contains(low_r + w)) {
            defs_low = true;
            break;
          }
        }
        if (defs_low) {
          int low_death = -1;
          if (IsDeferredLowerPriorityLocalDef(post_reload_block, core_to_orig_index, cross_spill, k,
                                              low_r, r_nregs, r_pri, live_at_end_s2, mm,
                                              &low_death)) {
            k = low_death;
          } else {
            low_busy = true;
          }
        }
      }
    }
    if (!low_busy) {
      return true;
    }
  }
  return false;
}

// Folds `reload1.c:5186-5249, 5641` cross-bank `"=f"` FPR->GPR (`mfc1`) output
// reloads `(set (reg:SF|DF gpr) (reg:SF|DF fpr))` (including across
// `sched2`-interleaved insns, excluded block-0 prologue saves, and pure FP
// readers inheriting `fpr`) back onto their producing FP arithmetic insn.
void FoldFprOutputReloadsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, std::vector<bool>* is_spill_overhead,
    absl::btree_map<int, RtlExpr>* md_output_reload_dest,
    absl::btree_map<int, int>* output_reload_orig_owner,
    absl::btree_map<int, absl::btree_map<int, RtlExpr>>* fpr_input_reload_subs) {
  const int n = static_cast<int>(post_reload_block.size());
  for (int mv_idx = 1; mv_idx < n; ++mv_idx) {
    if ((*is_spill_overhead)[static_cast<size_t>(mv_idx)]) {
      continue;
    }
    const Insn& mv = post_reload_block[static_cast<size_t>(mv_idx)];
    const RtlExpr& mv_pat = mv.pn.pattern;
    if (mv.kind != Insn::Kind::kInsn || mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
        mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetDest().flags.integrated ||
        mv_pat.SetSrc().op != RtlOp::kReg || mv_pat.SetDest().mode != mv_pat.SetSrc().mode ||
        (mv_pat.SetDest().mode != RtlMachineMode::kSf &&
         mv_pat.SetDest().mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int dst_gpr = mv_pat.SetDest().RegNo();
    const int src_fpr = mv_pat.SetSrc().RegNo();
    if (dst_gpr < 2 || dst_gpr >= kMipsFpRegFirst || src_fpr < kMipsFpRegFirst ||
        src_fpr > kMipsFpRegLast) {
      continue;
    }
    const int dst_nregs = std::max(1, mm.HardRegNRegs(dst_gpr, mv_pat.SetDest().mode));
    const int fpr_nregs = std::max(1, mm.HardRegNRegs(src_fpr, mv_pat.SetSrc().mode));
    // Scan forward for post-`mv` pure FP readers inheriting `src_fpr`
    // (`reload1.c:5186-5249, 5641` `choose_reload_regs` + `emit_reload_insns`).
    std::vector<int> post_inherited_readers;
    int last_fpr_use_idx = mv_idx;
    for (int k = mv_idx + 1; k < n; ++k) {
      if ((*is_spill_overhead)[static_cast<size_t>(k)]) {
        continue;
      }
      const Insn& after = post_reload_block[static_cast<size_t>(k)];
      if (after.kind == Insn::Kind::kCall || after.kind == Insn::Kind::kJump) {
        break;
      }
      absl::btree_set<int> a_fdefs;
      absl::btree_set<int> a_pdefs;
      absl::btree_set<int> a_reads;
      CollectFullRegDefs(after.pn.pattern, &a_fdefs, &a_pdefs);
      CollectRegReads(after.pn.pattern, &a_reads);
      bool reads_fpr = false;
      bool defs_fpr = false;
      for (int w = 0; w < fpr_nregs; ++w) {
        if (a_reads.contains(src_fpr + w)) {
          reads_fpr = true;
        }
        if (a_fdefs.contains(src_fpr + w) || a_pdefs.contains(src_fpr + w)) {
          defs_fpr = true;
        }
      }
      if (reads_fpr) {
        const RtlExpr& ap = after.pn.pattern;
        const bool is_pure_fp_reader = after.kind == Insn::Kind::kInsn && ap.op == RtlOp::kSet &&
                                       ap.children.size() == 2 && ap.SetSrc().op != RtlOp::kReg &&
                                       ap.SetSrc().op != RtlOp::kMem &&
                                       ap.SetSrc().op != RtlOp::kConstDouble && !defs_fpr;
        if (!is_pure_fp_reader) {
          break;
        }
        post_inherited_readers.push_back(k);
        last_fpr_use_idx = k;
      }
      if (defs_fpr) {
        break;
      }
      bool defs_dst = false;
      for (int w = 0; w < dst_nregs; ++w) {
        if (a_fdefs.contains(dst_gpr + w) || a_pdefs.contains(dst_gpr + w)) {
          defs_dst = true;
          break;
        }
      }
      if (defs_dst) {
        break;
      }
    }
    if (!IsRegDeadAfterInsn(post_reload_block, last_fpr_use_idx, src_fpr, mv_pat.SetSrc().mode, mm,
                            live_at_end_s2)) {
      continue;
    }
    std::vector<int> inherited_readers;
    for (int prod_idx = mv_idx - 1; prod_idx >= 0; --prod_idx) {
      if ((*is_spill_overhead)[static_cast<size_t>(prod_idx)]) {
        continue;
      }
      const Insn& prod = post_reload_block[static_cast<size_t>(prod_idx)];
      if (prod.kind == Insn::Kind::kCall) {
        break;
      }
      RtlExpr gpr_dest;
      // Do not fold a call-used GPR copy `(set (reg:SF/DF a0..t9) (reg:SF/DF $fN))`
      // when other FP insns also read `$fN`: in that case the copy is a genuine
      // pre-reload ABI/call-argument move rather than a `reload1.c:5641`
      // `emit_reload_insns` output reload of a pseudo allocated to a GPR.
      if (!md_output_reload_dest->contains(prod_idx) &&
          !(mm.CallUsed(dst_gpr) &&
            (!inherited_readers.empty() || !post_inherited_readers.empty())) &&
          TryMatchAdjacentFprOutputReload(prod, mv, &gpr_dest)) {
        (*is_spill_overhead)[static_cast<size_t>(mv_idx)] = true;
        (*md_output_reload_dest)[prod_idx] = gpr_dest;
        (*output_reload_orig_owner)[mv_idx] = prod_idx;
        for (int mid_idx : inherited_readers) {
          (*fpr_input_reload_subs)[mid_idx][src_fpr] = gpr_dest;
        }
        for (int after_idx : post_inherited_readers) {
          (*fpr_input_reload_subs)[after_idx][src_fpr] = gpr_dest;
        }
        break;
      }
      absl::btree_set<int> p_fdefs;
      absl::btree_set<int> p_pdefs;
      absl::btree_set<int> p_reads;
      CollectFullRegDefs(prod.pn.pattern, &p_fdefs, &p_pdefs);
      CollectRegReads(prod.pn.pattern, &p_reads);
      bool defines_fpr = false;
      bool reads_fpr = false;
      for (int k = 0; k < fpr_nregs; ++k) {
        if (p_fdefs.contains(src_fpr + k) || p_pdefs.contains(src_fpr + k)) {
          defines_fpr = true;
        }
        if (p_reads.contains(src_fpr + k)) {
          reads_fpr = true;
        }
      }
      if (defines_fpr) {
        break;
      }
      bool touches_dst = false;
      for (int k = 0; k < dst_nregs; ++k) {
        if (p_fdefs.contains(dst_gpr + k) || p_pdefs.contains(dst_gpr + k) ||
            p_reads.contains(dst_gpr + k)) {
          touches_dst = true;
          break;
        }
      }
      if (touches_dst) {
        break;
      }
      if (reads_fpr) {
        if (prod.kind != Insn::Kind::kInsn) {
          break;
        }
        inherited_readers.push_back(prod_idx);
      }
    }
  }
}

// Folds `reload1.c:emit_reload_insns` cross-bank `"f"` GPR->FPR (`mtc1`) input
// reloads `(set (reg:SF|DF fpr) (reg:SF|DF gpr))` (including across
// `sched2`-interleaved independent insns and excluded block-0 prologue saves)
// back onto their single FP consumer `cons_idx`.
void FoldFprInputReloadsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2,
    const absl::btree_map<int, RtlExpr>& md_output_reload_dest,
    std::vector<bool>* is_spill_overhead,
    absl::btree_map<int, absl::btree_map<int, RtlExpr>>* fpr_input_reload_subs,
    absl::btree_map<int, int>* fpr_input_reload_orig_consumer) {
  const int n = static_cast<int>(post_reload_block.size());
  for (int cons_idx = 1; cons_idx < n; ++cons_idx) {
    if ((*is_spill_overhead)[static_cast<size_t>(cons_idx)]) {
      continue;
    }
    const Insn& cons = post_reload_block[static_cast<size_t>(cons_idx)];
    if (cons.kind != Insn::Kind::kInsn) {
      continue;
    }
    absl::btree_set<int> cons_reads;
    CollectRegReads(cons.pn.pattern, &cons_reads);
    bool reads_any_fpr = false;
    for (int r : cons_reads) {
      if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
        reads_any_fpr = true;
        break;
      }
    }
    if (!reads_any_fpr) {
      continue;
    }
    int matched = 0;
    absl::btree_set<int> blocked_fprs;
    absl::btree_set<int> clobbered_fprs;
    absl::btree_set<int> clobbered_gprs;
    for (int mv_idx = cons_idx - 1; mv_idx >= 0 && matched < 2; --mv_idx) {
      const Insn& mid = post_reload_block[static_cast<size_t>(mv_idx)];
      if (mid.kind == Insn::Kind::kCall || mid.kind == Insn::Kind::kJump) {
        break;
      }
      if (!(*is_spill_overhead)[static_cast<size_t>(mv_idx)]) {
        int fpr = -1;
        RtlExpr src_reg_expr;
        if (TryMatchAdjacentFprInputReload(post_reload_block, mv_idx, cons_idx, mm, live_at_end_s2,
                                           md_output_reload_dest, &fpr, &src_reg_expr) &&
            !(*fpr_input_reload_subs)[cons_idx].contains(fpr)) {
          const int fpr_nregs = std::max(1, mm.HardRegNRegs(fpr, src_reg_expr.mode));
          const int src_reg = src_reg_expr.RegNo();
          const int src_nregs = std::max(1, mm.HardRegNRegs(src_reg, src_reg_expr.mode));
          const bool src_is_fpr = (src_reg >= kMipsFpRegFirst && src_reg <= kMipsFpRegLast);
          bool conflict = false;
          for (int k = 0; k < fpr_nregs && !conflict; ++k) {
            if (blocked_fprs.contains(fpr + k)) {
              conflict = true;
            }
          }
          for (int k = 0; k < src_nregs && !conflict; ++k) {
            if (src_is_fpr ? clobbered_fprs.contains(src_reg + k)
                           : clobbered_gprs.contains(src_reg + k)) {
              conflict = true;
            }
          }
          if (!conflict) {
            (*is_spill_overhead)[static_cast<size_t>(mv_idx)] = true;
            (*fpr_input_reload_subs)[cons_idx][fpr] = std::move(src_reg_expr);
            (*fpr_input_reload_orig_consumer)[mv_idx] = cons_idx;
            ++matched;
            continue;
          }
        }
      }
      absl::btree_set<int> mid_fdefs;
      absl::btree_set<int> mid_pdefs;
      absl::btree_set<int> mid_reads;
      CollectFullRegDefs(mid.pn.pattern, &mid_fdefs, &mid_pdefs);
      CollectRegReads(mid.pn.pattern, &mid_reads);
      for (int r : mid_fdefs) {
        if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
          blocked_fprs.insert(r);
          clobbered_fprs.insert(r);
        } else if (r >= 0 && r < kMipsFpRegFirst) {
          clobbered_gprs.insert(r);
        }
      }
      for (int r : mid_pdefs) {
        if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
          blocked_fprs.insert(r);
          clobbered_fprs.insert(r);
        } else if (r >= 0 && r < kMipsFpRegFirst) {
          clobbered_gprs.insert(r);
        }
      }
      for (int r : mid_reads) {
        if (r >= kMipsFpRegFirst && r <= kMipsFpRegLast) {
          blocked_fprs.insert(r);
        }
      }
    }
    if (matched == 0 && fpr_input_reload_subs->contains(cons_idx) &&
        (*fpr_input_reload_subs)[cons_idx].empty()) {
      fpr_input_reload_subs->erase(cons_idx);
    }
  }

  // `reload1.c:5186-5258` (`choose_reload_regs` inheritance): a cross-bank
  // callee-saved-GPR -> call-used-FPR input reload `(set (reg:SF/DF fpr) (reg:SF/DF gpr))`
  // can be inherited across 2+ pure FP consumers until `fpr` is overwritten or
  // dies at its final pure FP consumer.
  auto is_pure_fp_consumer_insn = [](const Insn& insn) -> bool {
    if (insn.kind != Insn::Kind::kInsn) {
      return false;
    }
    const RtlExpr& p = insn.pn.pattern;
    if (p.op != RtlOp::kSet || p.children.size() != 2) {
      return false;
    }
    const RtlOp rop = p.SetSrc().op;
    return rop != RtlOp::kReg && rop != RtlOp::kMem && rop != RtlOp::kConstDouble &&
           rop != RtlOp::kCall;
  };
  for (int mv_idx = 0; mv_idx + 1 < n; ++mv_idx) {
    if ((*is_spill_overhead)[static_cast<size_t>(mv_idx)]) {
      continue;
    }
    const Insn& mv = post_reload_block[static_cast<size_t>(mv_idx)];
    const RtlExpr& mv_pat = mv.pn.pattern;
    if (mv.kind != Insn::Kind::kInsn || mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
        mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetSrc().op != RtlOp::kReg ||
        mv_pat.SetDest().flags.integrated || mv_pat.SetSrc().flags.integrated ||
        mv_pat.SetDest().mode != mv_pat.SetSrc().mode ||
        (mv_pat.SetDest().mode != RtlMachineMode::kSf &&
         mv_pat.SetDest().mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int fpr = mv_pat.SetDest().RegNo();
    const int gpr = mv_pat.SetSrc().RegNo();
    const bool gpr_callee_saved = (gpr >= 16 && gpr <= 23) || gpr == 30;
    if (fpr < kMipsFpRegFirst || fpr >= 52 || !gpr_callee_saved) {
      continue;
    }
    const int fpr_nw = std::max(1, mm.HardRegNRegs(fpr, mv_pat.SetDest().mode));
    const int gpr_nw = std::max(1, mm.HardRegNRegs(gpr, mv_pat.SetSrc().mode));
    std::vector<int> consumers;
    bool closed = false;
    for (int k = mv_idx + 1; k < n; ++k) {
      if ((*is_spill_overhead)[static_cast<size_t>(k)]) {
        continue;
      }
      const Insn& insn = post_reload_block[static_cast<size_t>(k)];
      if (insn.kind == Insn::Kind::kCall || insn.kind == Insn::Kind::kJump) {
        break;
      }
      absl::btree_set<int> k_fdefs;
      absl::btree_set<int> k_pdefs;
      absl::btree_set<int> k_reads;
      CollectFullRegDefs(insn.pn.pattern, &k_fdefs, &k_pdefs);
      CollectRegReads(insn.pn.pattern, &k_reads);
      bool reads_fpr = false;
      bool defs_fpr = false;
      bool defs_gpr = false;
      for (int w = 0; w < fpr_nw; ++w) {
        if (k_reads.contains(fpr + w)) {
          reads_fpr = true;
        }
        if (k_fdefs.contains(fpr + w) || k_pdefs.contains(fpr + w)) {
          defs_fpr = true;
        }
      }
      for (int w = 0; w < gpr_nw; ++w) {
        if (k_fdefs.contains(gpr + w) || k_pdefs.contains(gpr + w)) {
          defs_gpr = true;
        }
      }
      if (reads_fpr) {
        if (!is_pure_fp_consumer_insn(insn) ||
            (fpr_input_reload_subs->contains(k) && (*fpr_input_reload_subs)[k].contains(fpr))) {
          break;
        }
        consumers.push_back(k);
        if (defs_fpr || IsRegDeadAfterInsn(post_reload_block, k, fpr, mv_pat.SetDest().mode, mm,
                                           live_at_end_s2)) {
          closed = true;
          break;
        }
        if (defs_gpr) {
          break;
        }
        continue;
      }
      if (defs_fpr || defs_gpr) {
        break;
      }
    }
    if (closed && consumers.size() >= 2) {
      (*is_spill_overhead)[static_cast<size_t>(mv_idx)] = true;
      for (int c_idx : consumers) {
        (*fpr_input_reload_subs)[c_idx][fpr] = mv_pat.SetSrc();
      }
      (*fpr_input_reload_orig_consumer)[mv_idx] = consumers.front();
    }
  }
}

struct ConstDoubleReloadFoldInfo {
  // Maps consumer orig_idx -> (fpr_reg -> (const_double_key, mode)).
  absl::btree_map<int, absl::btree_map<int, std::pair<std::string, RtlMachineMode>>>
      load_subs_by_cons;
  // Maps folded `const_double` reload orig_idx -> first consuming orig_idx.
  absl::btree_map<int, int> input_reload_orig_consumer;
};

// Mirrors `reload1.c:517-524, 566-573, 6583` (`reg_equiv_constant`) and
// `FoldSpilledConstDoubleReloads` in `invert_reload.cc`: in a self-loop block
// with a unique preheader, an unallocated loop-invariant `const_double:SF/DF`
// pseudo is rematerialized as `(set (reg:SF/DF spill_fpr) (const_double ...))`
// (with empty `reg_notes`) into a proven call-used FP spill register before
// pure FP consumers. Fold those rematerialized input reloads onto a shared
// live-in preheader pseudo per distinct constant key.
ConstDoubleReloadFoldInfo FoldSpilledConstDoubleReloadsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, const Sched1ProjectionContext& context,
    const absl::btree_map<int, absl::btree_map<int, RtlExpr>>& fpr_input_reload_subs,
    const absl::btree_map<int, int>& output_reload_orig_owner, const SpillSlotFoldInfo& cross_spill,
    std::vector<bool>* is_spill_overhead) {
  ConstDoubleReloadFoldInfo info;
  if (!context.has_self_loop_preheader) {
    return info;
  }
  const int n = static_cast<int>(post_reload_block.size());
  absl::btree_set<int> proven_fp_spill_regs;
  auto add_fpr_spill = [&](int r, RtlMachineMode mode) {
    if (r >= kMipsFpRegFirst && r < 52) {
      const int nw = std::max(1, mm.HardRegNRegs(r, mode));
      for (int w = 0; w < nw; ++w) {
        if (r + w < 52) {
          proven_fp_spill_regs.insert(r + w);
        }
      }
    }
  };
  for (const auto& [cons_idx, subs] : fpr_input_reload_subs) {
    for (const auto& [fpr, expr] : subs) {
      add_fpr_spill(fpr, expr.mode);
    }
  }
  for (const auto& [mv_orig, prod_orig] : output_reload_orig_owner) {
    if (mv_orig >= 0 && mv_orig < n) {
      const RtlExpr& p = post_reload_block[static_cast<size_t>(mv_orig)].pn.pattern;
      if (p.op == RtlOp::kSet && p.children.size() == 2 && p.SetSrc().op == RtlOp::kReg) {
        add_fpr_spill(p.SetSrc().RegNo(), p.SetSrc().mode);
      }
    }
  }
  for (const auto& [cons_idx, subs] : cross_spill.load_subs_by_cons) {
    for (const auto& [l_reg, slot_and_mode] : subs) {
      add_fpr_spill(l_reg, slot_and_mode.second);
    }
  }
  for (const auto& [prod_orig, slot_off] : cross_spill.store_slot_by_prod) {
    if (prod_orig >= 0 && prod_orig < n) {
      const RtlExpr* s = SingleSetOf(post_reload_block[static_cast<size_t>(prod_orig)].pn.pattern);
      if (s != nullptr && s->SetDest().op == RtlOp::kReg) {
        add_fpr_spill(s->SetDest().RegNo(), s->SetDest().mode);
      }
    }
  }
  if (proven_fp_spill_regs.empty()) {
    return info;
  }

  auto is_pure_fp_consumer = [](const Insn& insn) -> bool {
    if (insn.kind != Insn::Kind::kInsn) {
      return false;
    }
    const RtlExpr& p = insn.pn.pattern;
    if (p.op != RtlOp::kSet || p.children.size() != 2) {
      return false;
    }
    const RtlOp rop = p.SetSrc().op;
    return rop != RtlOp::kReg && rop != RtlOp::kMem && rop != RtlOp::kConstDouble &&
           rop != RtlOp::kCall;
  };
  for (int i = 0; i + 1 < n; ++i) {
    if ((*is_spill_overhead)[static_cast<size_t>(i)]) {
      continue;
    }
    const Insn& insn = post_reload_block[static_cast<size_t>(i)];
    if (insn.kind != Insn::Kind::kInsn || !insn.pn.reg_notes.empty()) {
      continue;
    }
    const RtlExpr& pat = insn.pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kReg ||
        pat.SetDest().flags.Any() || pat.SetSrc().op != RtlOp::kConstDouble ||
        pat.SetDest().mode != pat.SetSrc().mode ||
        (pat.SetDest().mode != RtlMachineMode::kSf && pat.SetDest().mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int fpr = pat.SetDest().RegNo();
    if (fpr < kMipsFpRegFirst || fpr >= 52 || !proven_fp_spill_regs.contains(fpr)) {
      continue;
    }
    const int nw = std::max(1, mm.HardRegNRegs(fpr, pat.SetDest().mode));
    std::vector<int> consumers;
    bool closed = false;
    for (int k = i + 1; k < n; ++k) {
      if ((*is_spill_overhead)[static_cast<size_t>(k)]) {
        continue;
      }
      const Insn& c_insn = post_reload_block[static_cast<size_t>(k)];
      if (c_insn.kind == Insn::Kind::kCall || c_insn.kind == Insn::Kind::kJump) {
        break;
      }
      absl::btree_set<int> k_fdefs;
      absl::btree_set<int> k_pdefs;
      absl::btree_set<int> k_reads;
      CollectFullRegDefs(c_insn.pn.pattern, &k_fdefs, &k_pdefs);
      CollectRegReads(c_insn.pn.pattern, &k_reads);
      bool reads_fpr = false;
      bool defs_fpr = false;
      for (int w = 0; w < nw; ++w) {
        if (k_reads.contains(fpr + w)) {
          reads_fpr = true;
        }
        if (k_fdefs.contains(fpr + w) || k_pdefs.contains(fpr + w)) {
          defs_fpr = true;
        }
      }
      if (reads_fpr) {
        if (!is_pure_fp_consumer(c_insn) ||
            (fpr_input_reload_subs.contains(k) && fpr_input_reload_subs.at(k).contains(fpr)) ||
            (cross_spill.load_subs_by_cons.contains(k) &&
             cross_spill.load_subs_by_cons.at(k).contains(fpr))) {
          break;
        }
        consumers.push_back(k);
        if (defs_fpr ||
            IsRegDeadAfterInsn(post_reload_block, k, fpr, pat.SetDest().mode, mm, live_at_end_s2)) {
          closed = true;
          break;
        }
        continue;
      }
      if (defs_fpr) {
        break;
      }
    }
    if (!closed || consumers.empty()) {
      continue;
    }
    std::string cd_key = std::to_string(static_cast<int>(pat.SetDest().mode)) + ":" +
                         std::to_string(pat.SetSrc().int_val) + ":" +
                         std::to_string(pat.SetSrc().int_val_high);
    for (int64_t ev : pat.SetSrc().extra_ints) {
      cd_key += ":" + std::to_string(ev);
    }
    (*is_spill_overhead)[static_cast<size_t>(i)] = true;
    info.input_reload_orig_consumer[i] = consumers.front();
    for (int c_idx : consumers) {
      info.load_subs_by_cons[c_idx][fpr] = {cd_key, pat.SetDest().mode};
    }
  }
  return info;
}

// Matches a `reload1.c:5599-5665` (`emit_reload_insns`) earlyclobber `DImode`
// GPR output-reload pair: `prod` is a `DImode` `PARALLEL` (`mips.md:407-411`
// `adddi3_internal_1` `"=d,&d"` / `"0,d"`, `mips.md:3722-3726`
// `ashldi3_internal` `"=&d"`, `mips.md:3949-3953` `ashrdi3_internal` `"=&d"`,
// `mips.md:4174-4178` `lshrdi3_internal` `"=&d"`) writing `(reg:DI reload_hr)`
// with an input overlapping `orig_hr` (triggering earlyclobber), and `mv` is
// `(set (reg:DI orig_hr) (reg:DI reload_hr))`.
bool TryMatchEarlyclobberDiOutputReload(const Insn& prod, const Insn& mv, const MachineModel& mm,
                                        int* out_orig_hr, int* out_reload_hr) {
  const RtlExpr& mv_pat = mv.pn.pattern;
  if (mv.kind != Insn::Kind::kInsn || mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
      mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetSrc().op != RtlOp::kReg ||
      mv_pat.SetDest().mode != RtlMachineMode::kDi || mv_pat.SetSrc().mode != RtlMachineMode::kDi) {
    return false;
  }
  const int orig_hr = mv_pat.SetDest().RegNo();
  const int reload_hr = mv_pat.SetSrc().RegNo();
  if (orig_hr < 2 || orig_hr >= kMipsFpRegFirst - 1 || reload_hr < 2 ||
      reload_hr >= kMipsFpRegFirst - 1 || orig_hr == reload_hr || (orig_hr % 2) != 0 ||
      (reload_hr % 2) != 0) {
    return false;
  }
  const int nw = mm.HardRegNRegs(orig_hr, RtlMachineMode::kDi);
  if (nw != 2) {
    return false;
  }
  const RtlExpr& ppat = prod.pn.pattern;
  if (prod.kind != Insn::Kind::kInsn || ppat.op != RtlOp::kParallel || ppat.children.size() != 2 ||
      ppat.children[0].op != RtlOp::kSet || ppat.children[0].children.size() != 2 ||
      ppat.children[1].op != RtlOp::kClobber || ppat.children[1].children.size() != 1 ||
      ppat.children[0].SetDest().op != RtlOp::kReg || ppat.children[0].SetDest().flags.integrated ||
      ppat.children[0].SetDest().mode != RtlMachineMode::kDi ||
      ppat.children[0].SetDest().RegNo() != reload_hr ||
      ppat.children[1].children[0].op != RtlOp::kReg) {
    return false;
  }
  const int clob_hr = ppat.children[1].children[0].RegNo();
  if (clob_hr >= orig_hr && clob_hr < orig_hr + nw) {
    return false;
  }
  const RtlExpr& rhs = ppat.children[0].SetSrc();
  if (rhs.children.size() != 2) {
    return false;
  }
  auto expr_overlaps_orig = [&](const RtlExpr& e) {
    bool hit = false;
    e.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kReg && !node.IsArgPointerReg()) {
        const int nr = node.RegNo();
        if (nr >= 0 && nr < kMipsFirstPseudoRegister) {
          const int nnw = std::max(1, mm.HardRegNRegs(nr, node.mode));
          if (std::max(orig_hr, nr) < std::min(orig_hr + nw, nr + nnw)) {
            hit = true;
          }
        }
      }
    });
    return hit;
  };
  bool triggered = false;
  if (rhs.op == RtlOp::kPlus) {
    const bool lhs_is_orig = rhs.children[0].op == RtlOp::kReg &&
                             rhs.children[0].mode == RtlMachineMode::kDi &&
                             rhs.children[0].RegNo() == orig_hr;
    triggered = !lhs_is_orig && expr_overlaps_orig(rhs.children[1]);
  } else if ((rhs.op == RtlOp::kAshift || rhs.op == RtlOp::kAshiftrt ||
              rhs.op == RtlOp::kLshiftrt) &&
             rhs.children[1].op == RtlOp::kReg) {
    triggered = expr_overlaps_orig(rhs);
  }
  if (!triggered) {
    return false;
  }
  if (out_orig_hr != nullptr) {
    *out_orig_hr = orig_hr;
  }
  if (out_reload_hr != nullptr) {
    *out_reload_hr = reload_hr;
  }
  return true;
}

// Folds `reload1.c:5599-5665` (`emit_reload_insns`) earlyclobber `DImode` GPR
// output-reload moves `(set (reg:DI orig) (reg:DI reload))` back onto their
// owning `PARALLEL` when `reload` dies at the move and no intervening insn
// reads/writes `orig` or `reload`, so `sched1` sees the single pre-reload insn.
void FoldEarlyclobberDiOutputReloadsForSched1View(
    absl::Span<const Insn> post_reload_block, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, std::vector<bool>* is_spill_overhead,
    absl::btree_map<int, RtlExpr>* md_output_reload_dest,
    absl::btree_map<int, int>* output_reload_orig_owner) {
  const int n = static_cast<int>(post_reload_block.size());
  for (int mv_idx = 1; mv_idx < n; ++mv_idx) {
    if ((*is_spill_overhead)[static_cast<size_t>(mv_idx)]) {
      continue;
    }
    const Insn& mv = post_reload_block[static_cast<size_t>(mv_idx)];
    const RtlExpr& mv_pat = mv.pn.pattern;
    if (mv.kind != Insn::Kind::kInsn || mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
        mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetSrc().op != RtlOp::kReg ||
        mv_pat.SetDest().mode != RtlMachineMode::kDi ||
        mv_pat.SetSrc().mode != RtlMachineMode::kDi) {
      continue;
    }
    const int orig_hr = mv_pat.SetDest().RegNo();
    const int reload_hr = mv_pat.SetSrc().RegNo();
    if (orig_hr < 2 || orig_hr >= kMipsFpRegFirst - 1 || reload_hr < 2 ||
        reload_hr >= kMipsFpRegFirst - 1 || orig_hr == reload_hr || (orig_hr % 2) != 0 ||
        (reload_hr % 2) != 0 || mm.HardRegNRegs(orig_hr, RtlMachineMode::kDi) != 2) {
      continue;
    }
    if (!IsRegDeadAfterInsn(post_reload_block, mv_idx, reload_hr, RtlMachineMode::kDi, mm,
                            live_at_end_s2)) {
      continue;
    }
    for (int prod_idx = mv_idx - 1; prod_idx >= 0; --prod_idx) {
      if ((*is_spill_overhead)[static_cast<size_t>(prod_idx)]) {
        continue;
      }
      const Insn& prod = post_reload_block[static_cast<size_t>(prod_idx)];
      if (prod.kind == Insn::Kind::kCall) {
        break;
      }
      if (!md_output_reload_dest->contains(prod_idx) &&
          TryMatchEarlyclobberDiOutputReload(prod, mv, mm, nullptr, nullptr)) {
        (*is_spill_overhead)[static_cast<size_t>(mv_idx)] = true;
        (*md_output_reload_dest)[prod_idx] = mv_pat.SetDest();
        (*output_reload_orig_owner)[mv_idx] = prod_idx;
        break;
      }
      absl::btree_set<int> p_fdefs;
      absl::btree_set<int> p_pdefs;
      absl::btree_set<int> p_reads;
      CollectFullRegDefs(prod.pn.pattern, &p_fdefs, &p_pdefs);
      CollectRegReads(prod.pn.pattern, &p_reads);
      bool blocked = false;
      for (int w = 0; w < 2; ++w) {
        if (p_fdefs.contains(reload_hr + w) || p_pdefs.contains(reload_hr + w) ||
            p_reads.contains(reload_hr + w) || p_fdefs.contains(orig_hr + w) ||
            p_pdefs.contains(orig_hr + w) || p_reads.contains(orig_hr + w)) {
          blocked = true;
          break;
        }
      }
      if (blocked) {
        break;
      }
    }
  }
}

}  // namespace

Sched1ProjectionView ProjectPostReloadBlockToSched1View(absl::Span<const Insn> post_reload_block,
                                                        const MachineModel& mm,
                                                        const absl::btree_set<int>& live_at_end_s2,
                                                        const Sched1ProjectionContext& context) {
  Sched1ProjectionView view;
  const int n = static_cast<int>(post_reload_block.size());
  if (n == 0) {
    return view;
  }

  // 1. Identify matched intra-block plain stack-spill / caller-save store+load pairs
  //    and adjacent `reload1.c:emit_reload_insns` `hi`/`lo` output-reload moves.
  //    Exclude block-0 interleaved prologue callee-save stores emitted after `sched1`
  //    (`toplev.c:3103`, `function.c:5516`) before matching stack-spill pairs.
  std::vector<bool> is_spill_overhead(static_cast<size_t>(n), false);
  for (int idx : context.excluded_prologue_body_indices) {
    if (idx >= 0 && idx < n) {
      is_spill_overhead[static_cast<size_t>(idx)] = true;
    }
  }
  const bool fp_needed = !context.stack_layout.valid || context.stack_layout.frame_pointer_needed;
  const IntraBlockSpillSlotClassification intra_slots = ClassifyIntraBlockSpillAndCallerSaveSlots(
      post_reload_block, mm, context.has_stack_addr_escape, &is_spill_overhead, fp_needed,
      context.stack_layout);
  const auto& slot_stores = intra_slots.slot_stores;
  const auto& slot_loads = intra_slots.slot_loads;
  const auto& spill_store_reg = intra_slots.spill_store_reg;
  const auto& preserved_across_call_ranges = intra_slots.preserved_across_call_ranges;
  const auto& caller_save_store_indices = intra_slots.caller_save_store_indices;
  const auto& intra_reload_spill_store_indices = intra_slots.intra_reload_spill_store_indices;
  const auto& intra_reload_spill_load_indices = intra_slots.intra_reload_spill_load_indices;

  // Fold `reload1.c:emit_reload_insns` `hi`/`lo` (`mfhi`/`mflo`) output-reload
  // and `MD_REGS` input-reload moves (including across `sched2`-interleaved
  // insns) and adjacent cross-bank `"=f"` FPR->GPR (`mfc1`) output-reload moves
  // back onto their producing insn so `sched1` sees the single pre-reload insn.
  absl::btree_map<int, RtlExpr> md_output_reload_dest;
  absl::btree_map<int, int> output_reload_orig_owner;
  absl::btree_map<int, int> md_input_reload_orig_consumer;
  for (int mv_idx = 1; mv_idx < n; ++mv_idx) {
    if (is_spill_overhead[static_cast<size_t>(mv_idx)]) {
      continue;
    }
    const Insn& mv = post_reload_block[static_cast<size_t>(mv_idx)];
    const RtlExpr& mv_pat = mv.pn.pattern;
    if (mv.kind == Insn::Kind::kInsn && mv_pat.op == RtlOp::kSet && mv_pat.children.size() == 2 &&
        mv_pat.SetSrc().op == RtlOp::kReg &&
        (mv_pat.SetSrc().RegNo() == kMipsMdRegFirst ||
         mv_pat.SetSrc().RegNo() == kMipsMdRegFirst + 1) &&
        mv_pat.SetDest().op == RtlOp::kReg && mv_pat.SetDest().RegNo() >= 0 &&
        mv_pat.SetDest().RegNo() < kMipsFpRegFirst) {
      const int md_reg = mv_pat.SetSrc().RegNo();
      const int dst_reg = mv_pat.SetDest().RegNo();
      const int dst_nregs = mm.HardRegNRegs(dst_reg, mv_pat.SetDest().mode);
      // Per `reload1.c:5641` (`emit_reload_insns`): when a pseudo is allocated
      // to `hi`/`lo` (`MD_REGS`), reload emits an input reload `(set gpr lo/hi)`
      // immediately before the consuming insn, which retains `(REG_DEAD lo/hi)`.
      // Anchor such input reloads before their consumer (`input_reload_before_core`)
      // rather than immediately after the `mult`/`div` producer.
      int md_input_cons_idx = -1;
      for (int c_idx = mv_idx + 1; c_idx < n; ++c_idx) {
        if (is_spill_overhead[static_cast<size_t>(c_idx)]) {
          continue;
        }
        const Insn& cons = post_reload_block[static_cast<size_t>(c_idx)];
        if (cons.kind == Insn::Kind::kCall || cons.kind == Insn::Kind::kJump) {
          break;
        }
        absl::btree_set<int> c_fdefs;
        absl::btree_set<int> c_pdefs;
        absl::btree_set<int> c_reads;
        CollectFullRegDefs(cons.pn.pattern, &c_fdefs, &c_pdefs);
        CollectRegReads(cons.pn.pattern, &c_reads);
        bool reads_dst = false;
        bool defs_dst = false;
        for (int k = 0; k < dst_nregs; ++k) {
          if (c_reads.contains(dst_reg + k)) {
            reads_dst = true;
          }
          if (c_fdefs.contains(dst_reg + k) || c_pdefs.contains(dst_reg + k)) {
            defs_dst = true;
          }
        }
        if (reads_dst && HasRegDeadNoteCovering(cons.pn.reg_notes, md_reg, mm)) {
          md_input_cons_idx = c_idx;
          break;
        }
        if (defs_dst || c_fdefs.contains(md_reg) || c_pdefs.contains(md_reg)) {
          break;
        }
      }
      for (int prod_idx = mv_idx - 1; prod_idx >= 0; --prod_idx) {
        if (is_spill_overhead[static_cast<size_t>(prod_idx)]) {
          continue;
        }
        const Insn& prod = post_reload_block[static_cast<size_t>(prod_idx)];
        if (prod.kind == Insn::Kind::kCall) {
          break;
        }
        RtlExpr gpr_dest;
        if (!md_output_reload_dest.contains(prod_idx) &&
            TryMatchAdjacentMdOutputReload(prod, mv, &gpr_dest)) {
          is_spill_overhead[static_cast<size_t>(mv_idx)] = true;
          md_output_reload_dest[prod_idx] = std::move(gpr_dest);
          if (md_input_cons_idx >= 0) {
            md_input_reload_orig_consumer[mv_idx] = md_input_cons_idx;
          } else {
            output_reload_orig_owner[mv_idx] = prod_idx;
          }
          break;
        }
        absl::btree_set<int> p_fdefs;
        absl::btree_set<int> p_pdefs;
        absl::btree_set<int> p_reads;
        CollectFullRegDefs(prod.pn.pattern, &p_fdefs, &p_pdefs);
        CollectRegReads(prod.pn.pattern, &p_reads);
        if (p_fdefs.contains(md_reg) || p_pdefs.contains(md_reg)) {
          break;
        }
        bool touches_dst = false;
        for (int k = 0; k < dst_nregs; ++k) {
          if (p_fdefs.contains(dst_reg + k) || p_pdefs.contains(dst_reg + k) ||
              p_reads.contains(dst_reg + k)) {
            touches_dst = true;
            break;
          }
        }
        if (touches_dst) {
          break;
        }
      }
    }
  }

  // Fold `reload1.c:5186-5249, 5641` cross-bank `"=f"` FPR->GPR (`mfc1`) output
  // reloads and `"f"` GPR->FPR (`mtc1`) input reloads (including across
  // `sched2`-interleaved independent insns and excluded block-0 prologue saves)
  // back onto their owning FP arithmetic insns.
  absl::btree_map<int, absl::btree_map<int, RtlExpr>> fpr_input_reload_subs;
  absl::btree_map<int, int> fpr_input_reload_orig_consumer;
  FoldFprOutputReloadsForSched1View(post_reload_block, mm, live_at_end_s2, &is_spill_overhead,
                                    &md_output_reload_dest, &output_reload_orig_owner,
                                    &fpr_input_reload_subs);
  FoldFprInputReloadsForSched1View(post_reload_block, mm, live_at_end_s2, md_output_reload_dest,
                                   &is_spill_overhead, &fpr_input_reload_subs,
                                   &fpr_input_reload_orig_consumer);
  FoldEarlyclobberDiOutputReloadsForSched1View(post_reload_block, mm, live_at_end_s2,
                                               &is_spill_overhead, &md_output_reload_dest,
                                               &output_reload_orig_owner);

  // Fold intra-block (non-caller-save) and cross-block `reload1.c:2309-2400, 5641-6160`
  // output-reload spill stores and input-reload spill loads back onto their spilled pseudos.
  const SpillSlotFoldInfo cross_spill = FoldReloadSpillsForSched1View(
      post_reload_block, mm, live_at_end_s2, context, slot_stores, slot_loads,
      intra_reload_spill_store_indices, intra_reload_spill_load_indices, md_output_reload_dest,
      &is_spill_overhead, &output_reload_orig_owner);
  const ConstDoubleReloadFoldInfo const_double_reloads = FoldSpilledConstDoubleReloadsForSched1View(
      post_reload_block, mm, live_at_end_s2, context, fpr_input_reload_subs,
      output_reload_orig_owner, cross_spill, &is_spill_overhead);

  // Select core instructions (preserving exact relative order) and anchor output-reload moves
  // and non-caller-save output-spill stores immediately after their owning core insn.
  std::vector<int> orig_to_core(static_cast<size_t>(n), -1);
  for (int i = 0; i < n; ++i) {
    if (!is_spill_overhead[static_cast<size_t>(i)]) {
      orig_to_core[static_cast<size_t>(i)] = static_cast<int>(view.core_to_orig_index.size());
      view.core_to_orig_index.push_back(i);
    }
  }
  const int m = static_cast<int>(view.core_to_orig_index.size());
  if (m == 0) {
    return view;
  }
  for (const auto& [mv_orig, prod_orig] : output_reload_orig_owner) {
    if (orig_to_core[static_cast<size_t>(prod_orig)] >= 0) {
      view.output_reload_after_core[mv_orig] = orig_to_core[static_cast<size_t>(prod_orig)];
    }
  }
  for (const auto& [mv_orig, cons_orig] : md_input_reload_orig_consumer) {
    if (orig_to_core[static_cast<size_t>(cons_orig)] >= 0) {
      view.input_reload_before_core[mv_orig] = orig_to_core[static_cast<size_t>(cons_orig)];
    }
  }
  for (const auto& [load_orig, cons_orig] : cross_spill.input_reload_orig_consumer) {
    if (orig_to_core[static_cast<size_t>(cons_orig)] >= 0) {
      view.input_reload_before_core[load_orig] = orig_to_core[static_cast<size_t>(cons_orig)];
    }
  }
  for (const auto& [mv_orig, cons_orig] : fpr_input_reload_orig_consumer) {
    if (orig_to_core[static_cast<size_t>(cons_orig)] >= 0) {
      view.input_reload_before_core[mv_orig] = orig_to_core[static_cast<size_t>(cons_orig)];
    }
  }
  for (const auto& [cd_orig, cons_orig] : const_double_reloads.input_reload_orig_consumer) {
    if (orig_to_core[static_cast<size_t>(cons_orig)] >= 0) {
      view.input_reload_before_core[cd_orig] = orig_to_core[static_cast<size_t>(cons_orig)];
    }
  }
  for (const auto& [s_idx, s_reg] : spill_store_reg) {
    if (!is_spill_overhead[static_cast<size_t>(s_idx)]) {
      continue;
    }
    if (caller_save_store_indices.contains(s_idx)) {
      // `caller-save.c:455-462` (`save_call_clobbered_regs`): emit caller-save
      // stores immediately before the `CALL_INSN` they save across.
      for (int next = s_idx + 1; next < n; ++next) {
        if (post_reload_block[static_cast<size_t>(next)].kind == Insn::Kind::kCall &&
            orig_to_core[static_cast<size_t>(next)] >= 0) {
          view.input_reload_before_core[s_idx] = orig_to_core[static_cast<size_t>(next)];
          break;
        }
      }
      continue;
    }
    for (int prev = s_idx - 1; prev >= 0; --prev) {
      if (orig_to_core[static_cast<size_t>(prev)] < 0) {
        continue;
      }
      absl::btree_set<int> fdefs;
      absl::btree_set<int> pdefs;
      CollectFullRegDefs(post_reload_block[static_cast<size_t>(prev)].pn.pattern, &fdefs, &pdefs);
      if (fdefs.contains(s_reg) || pdefs.contains(s_reg)) {
        view.output_reload_after_core[s_idx] = orig_to_core[static_cast<size_t>(prev)];
      }
      break;
    }
  }

  // 2. Determine ABI-pinned hard-reg defs directly feeding `CALL_INSN` `call_fusage`
  // or return-value `(use (reg/i ...))` (`function.c:5595`, `calls.c:emit_call_1`).
  std::vector<absl::btree_set<int>> abi_pinned_defs(static_cast<size_t>(m));
  for (int ci = 0; ci < m; ++ci) {
    const Insn& insn = post_reload_block[static_cast<size_t>(view.core_to_orig_index[ci])];
    absl::btree_set<int> needed_arg_regs;
    if (insn.kind == Insn::Kind::kCall) {
      for (const RtlExpr& fu : insn.pn.call_fusage) {
        CollectRegReads(fu, &needed_arg_regs);
      }
    } else if (insn.pn.pattern.op == RtlOp::kUse && !insn.pn.pattern.children.empty() &&
               insn.pn.pattern.children[0].op == RtlOp::kReg &&
               insn.pn.pattern.children[0].flags.integrated) {
      const RtlExpr& ur = insn.pn.pattern.children[0];
      const int base_r = ur.RegNo();
      if (base_r >= 0 && base_r < kMipsFirstPseudoRegister) {
        const int nr = mm.HardRegNRegs(base_r, ur.mode);
        for (int k = 0; k < nr; ++k) {
          needed_arg_regs.insert(base_r + k);
        }
      }
    } else {
      continue;
    }
    for (int prev = ci - 1; prev >= 0 && !needed_arg_regs.empty(); --prev) {
      const Insn& pinsn = post_reload_block[static_cast<size_t>(view.core_to_orig_index[prev])];
      if (pinsn.kind == Insn::Kind::kCall) {
        break;
      }
      absl::btree_set<int> fdefs;
      absl::btree_set<int> pdefs;
      const int prev_orig = view.core_to_orig_index[static_cast<size_t>(prev)];
      CollectFullRegDefs(pinsn.pn.pattern, &fdefs, &pdefs);
      if (const auto md_it = md_output_reload_dest.find(prev_orig);
          md_it != md_output_reload_dest.end()) {
        fdefs.insert(md_it->second.RegNo());
      }
      for (int r : fdefs) {
        if (needed_arg_regs.contains(r)) {
          abi_pinned_defs[static_cast<size_t>(prev)].insert(r);
          needed_arg_regs.erase(r);
        }
      }
      for (int r : pdefs) {
        if (needed_arg_regs.contains(r)) {
          abi_pinned_defs[static_cast<size_t>(prev)].insert(r);
        }
      }
    }
  }

  // 3. Forward intra-block SSA live-range splitting.
  const absl::btree_set<int> global_carrier_regs = FindCoalescedGlobalCarrierHardRegs(
      post_reload_block, view.core_to_orig_index, abi_pinned_defs, live_at_end_s2, context, mm);
  const std::vector<uint64_t> live_after_core =
      ComputeCoreHardRegLiveAfterMasks(post_reload_block, view.core_to_orig_index, live_at_end_s2,
                                       cross_spill, preserved_across_call_ranges, mm);
  absl::btree_map<int, int> global_carrier_pseudo;
  int next_pseudo = kMipsFirstPseudoRegister;
  absl::btree_map<int, int> cur_pseudo;
  absl::btree_map<int, int> cur_pseudo_birth_ci;
  // Physical hard registers explicitly defined earlier in this block as
  // unrenamed hard registers (ABI call returns/args, `/i` returns, or
  // `local-alloc.c:1798` dying hard-reg suggestion conflicts) so subsequent
  // reads stay bound to that physical hard register rather than inventing a
  // disconnected live-in pseudo.
  absl::btree_set<int> unrenamed_hard_def_regs;
  // Active `optabs.c:2485-2525` (`emit_no_conflict_block`) destination pseudos
  // whose `DImode` hard home `dh` overlaps a still-live `REG_NO_CONFLICT` input
  // register (`local-alloc.c:1317-1352`); keeps `cur_pseudo[src]` intact until
  // the final subword write terminates the input's live range.
  absl::btree_map<int, std::pair<int, int>> active_no_conflict_di;  // dh -> (pseudo, src_reg)
  int last_call_ci = -1;
  absl::btree_set<int> live_in_pseudos;
  // Maps hard reg `hr` read before first write in this block to its view reg
  // (either its fresh live-in pseudo or `hr` itself for unrenamed ABI regs).
  absl::btree_map<int, int> live_in_reg_for_hard_reg;
  // Spilled-slot pseudos keyed by `(stack_offset, version_id)` (`reload1.c:2309, 2362-2367`).
  absl::btree_map<SpillPseudoKey, int> spill_slot_pseudo;
  absl::btree_map<std::string, int> const_double_pseudo;
  absl::btree_set<int> spill_live_out_pseudos;
  auto get_or_create_const_double_pseudo = [&](const std::string& cd_key) -> int {
    const auto it = const_double_pseudo.find(cd_key);
    if (it != const_double_pseudo.end()) {
      return it->second;
    }
    const int p = next_pseudo++;
    const_double_pseudo[cd_key] = p;
    live_in_pseudos.insert(p);
    return p;
  };
  auto get_or_create_spill_pseudo = [&](const SpillPseudoKey& slot_key,
                                        bool mark_live_in_if_new) -> int {
    const auto it = spill_slot_pseudo.find(slot_key);
    if (it != spill_slot_pseudo.end()) {
      return it->second;
    }
    const int p = next_pseudo++;
    spill_slot_pseudo[slot_key] = p;
    if (mark_live_in_if_new) {
      live_in_pseudos.insert(p);
    }
    return p;
  };

  view.core_insns.reserve(static_cast<size_t>(m));
  bool in_entry_param_prefix = context.is_entry_block;
  int prev_orig_idx = -1;
  for (int ci = 0; ci < m; ++ci) {
    const int orig_idx = view.core_to_orig_index[static_cast<size_t>(ci)];
    for (int skipped = prev_orig_idx + 1; skipped < orig_idx; ++skipped) {
      if (const auto mit = cross_spill.multi_use_load_slot_by_orig.find(skipped);
          mit != cross_spill.multi_use_load_slot_by_orig.end()) {
        const int sp_pseudo =
            get_or_create_spill_pseudo(mit->second.second, /*mark_live_in_if_new=*/false);
        cur_pseudo[mit->second.first] = sp_pseudo;
        cur_pseudo_birth_ci[mit->second.first] = -1;
        unrenamed_hard_def_regs.erase(mit->second.first);
      }
    }
    prev_orig_idx = orig_idx;
    Insn insn = post_reload_block[static_cast<size_t>(orig_idx)];
    insn.luid = ci;
    int direct_entry_arg_spill_pseudo = -1;
    int direct_entry_arg_spill_src_reg = -1;
    if (const auto de_it = cross_spill.direct_entry_arg_spill_slot_by_store.find(orig_idx);
        de_it != cross_spill.direct_entry_arg_spill_slot_by_store.end() &&
        insn.pn.pattern.op == RtlOp::kSet && insn.pn.pattern.children.size() == 2 &&
        insn.pn.pattern.children[1].op == RtlOp::kReg) {
      const int sp_pseudo =
          get_or_create_spill_pseudo(de_it->second.first, /*mark_live_in_if_new=*/false);
      if (cross_spill.live_out_slot_keys.contains(de_it->second.first)) {
        spill_live_out_pseudos.insert(sp_pseudo);
      }
      direct_entry_arg_spill_pseudo = sp_pseudo;
      direct_entry_arg_spill_src_reg = insn.pn.pattern.children[1].RegNo();
      RtlExpr dst_reg;
      dst_reg.op = RtlOp::kReg;
      dst_reg.mode = de_it->second.second;
      dst_reg.int_val = sp_pseudo;
      insn.pn.pattern.children[0] = std::move(dst_reg);
    }
    const bool is_leading_entry_param_copy = in_entry_param_prefix && IsBlock0PinnedParamCopy(insn);
    if (!is_leading_entry_param_copy) {
      in_entry_param_prefix = false;
    }
    if (const auto md_it = md_output_reload_dest.find(orig_idx);
        md_it != md_output_reload_dest.end()) {
      if (insn.pn.pattern.op == RtlOp::kParallel && !insn.pn.pattern.children.empty() &&
          insn.pn.pattern.children[0].op == RtlOp::kSet &&
          insn.pn.pattern.children[0].children.size() == 2) {
        insn.pn.pattern.children[0].children[0] = md_it->second;
      } else if (insn.pn.pattern.op == RtlOp::kSet && insn.pn.pattern.children.size() == 2) {
        insn.pn.pattern.children[0] = md_it->second;
      }
    }
    if (const auto in_it = fpr_input_reload_subs.find(orig_idx);
        in_it != fpr_input_reload_subs.end()) {
      for (const auto& [fpr, gpr_src] : in_it->second) {
        ReplaceRegInReadPositions(&insn.pn.pattern, fpr, gpr_src);
      }
    }
    if (const auto ip_it = cross_spill.in_place_spill_load_slot_by_orig.find(orig_idx);
        ip_it != cross_spill.in_place_spill_load_slot_by_orig.end() &&
        insn.pn.pattern.op == RtlOp::kSet && insn.pn.pattern.children.size() == 2) {
      const int sp_pseudo =
          get_or_create_spill_pseudo(ip_it->second.first, /*mark_live_in_if_new=*/true);
      RtlExpr repl;
      repl.op = RtlOp::kReg;
      repl.mode = ip_it->second.second;
      repl.int_val = sp_pseudo;
      insn.pn.pattern.children[1] = std::move(repl);
    }
    absl::btree_set<int> spill_clobbered_hard_regs;
    if (const auto sp_in_it = cross_spill.load_subs_by_cons.find(orig_idx);
        sp_in_it != cross_spill.load_subs_by_cons.end()) {
      for (const auto& [l_reg, slot_and_mode] : sp_in_it->second) {
        const int sp_pseudo =
            get_or_create_spill_pseudo(slot_and_mode.first, /*mark_live_in_if_new=*/true);
        RtlExpr repl;
        repl.op = RtlOp::kReg;
        repl.mode = slot_and_mode.second;
        repl.int_val = sp_pseudo;
        ReplaceRegInReadPositions(&insn.pn.pattern, l_reg, repl);
        spill_clobbered_hard_regs.insert(l_reg);
      }
    }
    if (const auto cd_in_it = const_double_reloads.load_subs_by_cons.find(orig_idx);
        cd_in_it != const_double_reloads.load_subs_by_cons.end()) {
      for (const auto& [fpr_reg, key_and_mode] : cd_in_it->second) {
        const int cd_pseudo = get_or_create_const_double_pseudo(key_and_mode.first);
        RtlExpr repl;
        repl.op = RtlOp::kReg;
        repl.mode = key_and_mode.second;
        repl.int_val = cd_pseudo;
        ReplaceRegInReadPositions(&insn.pn.pattern, fpr_reg, repl);
        spill_clobbered_hard_regs.insert(fpr_reg);
      }
    }

    const bool is_use_or_call =
        (insn.kind == Insn::Kind::kCall || insn.pn.pattern.op == RtlOp::kUse);
    if (is_use_or_call) {
      // Preserve ABI call/return semantics; rename only non-ABI address operands inside calls.
      if (insn.kind == Insn::Kind::kCall) {
        last_call_ci = ci;
        // Per `calls.c:emit_call_1`: the `CALL_INSN` return destination
        // (`$v0`/`$f0`), `(clobber $ra)`, and `call_fusage` are physical ABI
        // hard regs, whereas an indirect call's callee address `(call (mem
        // <addr>) ...)` is an ordinary pseudo in `.07.sched` that `lreg`/`greg`
        // may allocate to any caller-saved GPR (commonly `$v0`/`$v1`).
        absl::btree_map<int, int> call_addr_map;
        for (const auto& [hr, pr] : cur_pseudo) {
          if (!IsFixedOrSpecialHardReg(hr, fp_needed)) {
            call_addr_map[hr] = pr;
          }
        }
        auto rename_call_addrs = [&](auto&& self, RtlExpr* node) -> void {
          if (node->op == RtlOp::kCall && !node->children.empty()) {
            RenameExprRegs(&node->children[0], call_addr_map);
            return;
          }
          for (RtlExpr& ch : node->children) {
            self(self, &ch);
          }
        };
        rename_call_addrs(rename_call_addrs, &insn.pn.pattern);
        // Clobber call-used hard regs not protected by an excluded caller-save spill range.
        for (auto it = cur_pseudo.begin(); it != cur_pseudo.end();) {
          const int hr = it->first;
          bool preserved = false;
          for (const auto& [s_idx, l_idx, s_reg] : preserved_across_call_ranges) {
            if (s_reg == hr && s_idx < orig_idx && orig_idx < l_idx) {
              preserved = true;
              break;
            }
          }
          if (mm.CallUsed(hr) && !preserved) {
            cur_pseudo_birth_ci.erase(hr);
            it = cur_pseudo.erase(it);
          } else {
            ++it;
          }
        }
        for (auto it = unrenamed_hard_def_regs.begin(); it != unrenamed_hard_def_regs.end();) {
          if (mm.CallUsed(*it)) {
            it = unrenamed_hard_def_regs.erase(it);
          } else {
            ++it;
          }
        }
        absl::btree_set<int> call_fdefs;
        absl::btree_set<int> call_pdefs;
        CollectFullRegDefs(insn.pn.pattern, &call_fdefs, &call_pdefs);
        for (int r : call_fdefs) {
          if (r >= 0 && r < kMipsFirstPseudoRegister && !IsFixedOrSpecialHardReg(r, fp_needed)) {
            cur_pseudo.erase(r);
            cur_pseudo_birth_ci.erase(r);
            unrenamed_hard_def_regs.insert(r);
          }
        }
      }
      insn.pn.reg_notes.clear();
      view.core_insns.push_back(std::move(insn));
      continue;
    }

    absl::btree_set<int> reads;
    CollectRegReads(insn.pn.pattern, &reads);
    absl::btree_map<int, int> read_map;
    for (int r : reads) {
      if (r >= kMipsFirstPseudoRegister || IsFixedOrSpecialHardReg(r, fp_needed)) {
        continue;
      }
      if (const auto it = cur_pseudo.find(r); it != cur_pseudo.end()) {
        read_map[r] = it->second;
      } else if (unrenamed_hard_def_regs.contains(r)) {
        // Keep reads of an earlier unrenamed hard-reg def in this block bound
        // to the physical hard register so `def -> use` edges stay intact.
      } else if (!IsAbiCallInterfaceHardReg(r) || global_carrier_regs.contains(r)) {
        const int fresh = next_pseudo++;
        cur_pseudo[r] = fresh;
        cur_pseudo_birth_ci[r] = -1;
        live_in_pseudos.insert(fresh);
        live_in_reg_for_hard_reg.try_emplace(r, fresh);
        if (global_carrier_regs.contains(r)) {
          global_carrier_pseudo[r] = fresh;
        }
        read_map[r] = fresh;
      } else {
        live_in_reg_for_hard_reg.try_emplace(r, r);
      }
    }

    absl::btree_set<int> full_defs;
    absl::btree_set<int> partial_defs;
    CollectFullRegDefs(insn.pn.pattern, &full_defs, &partial_defs);

    absl::btree_map<int, int> def_map;
    // Handle `optabs.c:2485-2525` (`emit_no_conflict_block`) `REG_LIBCALL`
    // `(clobber (reg:DI dh))` overlapping a still-live `REG_NO_CONFLICT` input
    // without clobbering `cur_pseudo[src_reg]` before the subword writes.
    bool handled_no_conflict_clobber = false;
    if (insn.pn.pattern.op == RtlOp::kClobber && insn.pn.pattern.children.size() == 1 &&
        insn.pn.pattern.children[0].op == RtlOp::kReg &&
        insn.pn.pattern.children[0].mode == RtlMachineMode::kDi &&
        insn.pn.pattern.children[0].RegNo() >= 0 &&
        insn.pn.pattern.children[0].RegNo() < kMipsFirstPseudoRegister &&
        std::any_of(insn.pn.reg_notes.begin(), insn.pn.reg_notes.end(),
                    [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegLibcall; })) {
      const int dh = insn.pn.pattern.children[0].RegNo();
      int nc_src = -1;
      for (int k = ci + 1; k < m; ++k) {
        const RtlPatternAndNotes& kpn =
            post_reload_block[static_cast<size_t>(view.core_to_orig_index[k])].pn;
        if (kpn.pattern.op == RtlOp::kSet && kpn.pattern.children.size() == 2 &&
            kpn.pattern.SetDest().op == RtlOp::kSubreg && !kpn.pattern.SetDest().children.empty() &&
            kpn.pattern.SetDest().children[0].op == RtlOp::kReg &&
            kpn.pattern.SetDest().children[0].mode == RtlMachineMode::kDi &&
            kpn.pattern.SetDest().children[0].RegNo() == dh) {
          for (const RtlRegNote& n : kpn.reg_notes) {
            if (n.kind == RtlMachineMode::kRegNoConflict && n.datum.op == RtlOp::kReg &&
                (n.datum.RegNo() == dh || n.datum.RegNo() == dh + 1)) {
              nc_src = n.datum.RegNo();
              break;
            }
          }
        }
        if (nc_src >= 0) {
          break;
        }
      }
      if (nc_src >= 0) {
        const int fresh = next_pseudo++;
        def_map[dh] = fresh;
        active_no_conflict_di[dh] = {fresh, nc_src};
        handled_no_conflict_clobber = true;
      }
    }
    for (int r : partial_defs) {
      if (handled_no_conflict_clobber || r >= kMipsFirstPseudoRegister ||
          IsFixedOrSpecialHardReg(r, fp_needed) ||
          abi_pinned_defs[static_cast<size_t>(ci)].contains(r)) {
        continue;
      }
      if (const auto nc_it = active_no_conflict_di.find(r); nc_it != active_no_conflict_di.end()) {
        const int nc_pseudo = nc_it->second.first;
        const int nc_src = nc_it->second.second;
        def_map[r] = nc_pseudo;
        if (HasRegDeadNoteCovering(insn.pn.reg_notes, nc_src, mm)) {
          cur_pseudo[r] = nc_pseudo;
          cur_pseudo_birth_ci[r] = ci;
          active_no_conflict_di.erase(nc_it);
        }
        continue;
      }
      if (const auto it = cur_pseudo.find(r); it != cur_pseudo.end()) {
        def_map[r] = it->second;
      } else if (!IsAbiCallInterfaceHardReg(r)) {
        const int fresh = next_pseudo++;
        cur_pseudo[r] = fresh;
        cur_pseudo_birth_ci[r] = ci;
        def_map[r] = fresh;
      }
    }
    for (int r : full_defs) {
      if (handled_no_conflict_clobber || r >= kMipsFirstPseudoRegister ||
          IsFixedOrSpecialHardReg(r, fp_needed)) {
        continue;
      }
      if (abi_pinned_defs[static_cast<size_t>(ci)].contains(r)) {
        cur_pseudo.erase(r);
        cur_pseudo_birth_ci.erase(r);
        unrenamed_hard_def_regs.insert(r);
        continue;
      }
      // Keep `/i` return-value sets unrenamed (`RenameExprRegs` also skips `/i`).
      const RtlExpr* s = SingleSetOf(insn.pn.pattern);
      if (s != nullptr && s->SetDest().op == RtlOp::kReg && s->SetDest().RegNo() == r &&
          s->SetDest().flags.integrated) {
        cur_pseudo.erase(r);
        cur_pseudo_birth_ci.erase(r);
        unrenamed_hard_def_regs.insert(r);
        continue;
      }
      // `flow.c:2506-2515` never attaches `REG_DEAD P` when an insn both reads
      // and writes the same pseudo `P`. Therefore:
      //  - `(set r (op r ...))` WITHOUT `REG_DEAD r` => same multi-set pseudo
      //    (induction variable / accumulator, `reg_n_sets >= 2`, `birthing_insn_p == false`),
      //    UNLESS it is a post-call dying-input reuse tied by `local-alloc.c:1266-1300, 1855`
      //    (`combine_regs`) whose `reg_n_calls_crossed == 0` locks it after the call
      //    (`sched.c:1732`);
      //  - `(set r (op r ...))` WITH `REG_DEAD r` => dying-input register reuse
      //    (`local-alloc.c:1395-1450`), so LHS is a distinct fresh single-set pseudo!
      const auto birth_it = cur_pseudo_birth_ci.find(r);
      const int birth_ci = (birth_it != cur_pseudo_birth_ci.end()) ? birth_it->second : -1;
      const bool has_reg_dead = HasRegDeadNoteCovering(insn.pn.reg_notes, r, mm);
      const bool forced_combine_split =
          !has_reg_dead && context.forced_combine_regs_split_body_indices.contains(orig_idx);
      const bool post_call_combine_split =
          context.allow_post_call_combine_regs_split && !has_reg_dead &&
          IsPostCallCombineRegsSplitCandidate(post_reload_block, view.core_to_orig_index, ci, r,
                                              birth_ci, last_call_ci, live_at_end_s2, mm);
      const bool general_combine_split =
          (context.allow_general_combine_regs_split ||
           (context.allow_call_used_general_combine_regs_split && mm.CallUsed(r))) &&
          !has_reg_dead && !post_call_combine_split &&
          IsGeneralCombineRegsRmwSplitCandidate(post_reload_block, view.core_to_orig_index, ci, r,
                                                birth_ci, live_at_end_s2, mm);
      const bool implicit_combine_split =
          forced_combine_split || post_call_combine_split || general_combine_split;
      // `function.c:assign_parms` + `flow.c:2506-2515`: in block 0, an incoming
      // ABI parameter hard register (`$a0-$a3`, `$f12`, `$f14`) read before any
      // prior def in the block is a physical argument register, never the same
      // pseudo as a computed result written back into that hard register.
      const bool incoming_abi_arg_rmw =
          context.is_entry_block && IsAbiCallInterfaceHardReg(r) && !cur_pseudo.contains(r);
      // `emit-rtl.c:515-517` (`gen_reg_rtx`): a pre-allocation pseudo register
      // has one fixed machine mode. An in-place mode-changing conversion/overwrite
      // `(set (reg:M2 r) (... (reg:M1 r) ...))` where every read of `r` in the
      // RHS has a distinct mode `M1 != M2` (e.g. `float_extend:DF (reg:SF $f24)`
      // -> `(reg:DF $f24)`) cannot be the same pre-allocation pseudo even when
      // `REG_DEAD (reg:M1 r)` was omitted (such as delay-slot `cvt.d.s`).
      bool mode_changing_rmw = false;
      if (reads.contains(r) && s != nullptr && s->SetDest().op == RtlOp::kReg &&
          s->SetDest().RegNo() == r && s->SetDest().mode != RtlMachineMode::kVoid &&
          s->SetDest().mode != RtlMachineMode::kNone) {
        const RtlMachineMode dst_mode = s->SetDest().mode;
        bool saw_r_read = false;
        bool all_diff_mode = true;
        s->SetSrc().Visit([&](const RtlExpr& node) {
          if (node.op == RtlOp::kReg && node.RegNo() == r) {
            saw_r_read = true;
            if (node.mode == dst_mode || node.mode == RtlMachineMode::kVoid ||
                node.mode == RtlMachineMode::kNone) {
              all_diff_mode = false;
            }
          }
        });
        mode_changing_rmw = saw_r_read && all_diff_mode;
      }
      const auto sp_out_it = cross_spill.store_slot_by_prod.find(orig_idx);
      const bool feeds_cross_spill = sp_out_it != cross_spill.store_slot_by_prod.end() &&
                                     s != nullptr && s->SetDest().op == RtlOp::kReg &&
                                     s->SetDest().RegNo() == r;
      if (reads.contains(r) && !has_reg_dead && !implicit_combine_split && !incoming_abi_arg_rmw &&
          !mode_changing_rmw && !feeds_cross_spill) {
        if (const auto it = read_map.find(r); it != read_map.end()) {
          def_map[r] = it->second;
          cur_pseudo[r] = it->second;
        }
        continue;
      }
      if (!has_reg_dead && !implicit_combine_split && !mode_changing_rmw && !feeds_cross_spill) {
        if (const auto git = global_carrier_pseudo.find(r); git != global_carrier_pseudo.end()) {
          def_map[r] = git->second;
          cur_pseudo[r] = git->second;
          continue;
        }
      }
      if (reads.contains(r) && implicit_combine_split) {
        view.implicit_combine_regs_split_orig_indices.push_back(orig_idx);
      }
      if (!feeds_cross_spill && !is_leading_entry_param_copy &&
          (HasLocalDyingHardRegSuggestionConflict(
               post_reload_block, view.core_to_orig_index, ci, r, reads, read_map,
               unrenamed_hard_def_regs, context.is_entry_block, live_at_end_s2, mm, fp_needed) ||
           HasLocalLowerAllocOrderFreeRegConflict(post_reload_block, view.core_to_orig_index,
                                                  live_after_core, ci, r, reads, live_at_end_s2,
                                                  cross_spill, mm))) {
        cur_pseudo.erase(r);
        cur_pseudo_birth_ci.erase(r);
        unrenamed_hard_def_regs.insert(r);
        continue;
      }
      int fresh = -1;
      if (feeds_cross_spill) {
        fresh = get_or_create_spill_pseudo(sp_out_it->second, /*mark_live_in_if_new=*/false);
        if (cross_spill.live_out_slot_keys.contains(sp_out_it->second)) {
          spill_live_out_pseudos.insert(fresh);
        }
      } else {
        fresh = next_pseudo++;
      }
      cur_pseudo[r] = fresh;
      cur_pseudo_birth_ci[r] = ci;
      unrenamed_hard_def_regs.erase(r);
      def_map[r] = fresh;
    }
    for (int clob_r : spill_clobbered_hard_regs) {
      if (!full_defs.contains(clob_r) && !partial_defs.contains(clob_r)) {
        cur_pseudo.erase(clob_r);
        cur_pseudo_birth_ci.erase(clob_r);
        unrenamed_hard_def_regs.erase(clob_r);
      }
    }
    if (direct_entry_arg_spill_pseudo >= 0 && direct_entry_arg_spill_src_reg >= 0 &&
        !HasRegDeadNoteCovering(insn.pn.reg_notes, direct_entry_arg_spill_src_reg, mm)) {
      cur_pseudo[direct_entry_arg_spill_src_reg] = direct_entry_arg_spill_pseudo;
      cur_pseudo_birth_ci[direct_entry_arg_spill_src_reg] = ci;
      unrenamed_hard_def_regs.erase(direct_entry_arg_spill_src_reg);
    }

    // Apply `read_map` to uses (RHS / MEM dest address) and `def_map` to defined dests.
    auto rename_set_or_clobber = [&](RtlExpr* node) {
      if (node->op == RtlOp::kSet && node->children.size() == 2) {
        RenameExprRegs(&node->children[1], read_map);
        if (node->children[0].op == RtlOp::kMem) {
          RenameExprRegs(&node->children[0], read_map);
        } else {
          RenameExprRegs(&node->children[0], def_map);
        }
      } else if (node->op == RtlOp::kClobber && !node->children.empty()) {
        RenameExprRegs(&node->children[0], def_map);
      } else {
        RenameExprRegs(node, read_map);
      }
    };
    if (insn.pn.pattern.op == RtlOp::kParallel) {
      for (RtlExpr& ch : insn.pn.pattern.children) {
        rename_set_or_clobber(&ch);
      }
    } else {
      rename_set_or_clobber(&insn.pn.pattern);
    }
    // Mirror `reload1.c:2641` (`eliminate_regs`, `mips.h:1649-1651`):
    // un-eliminate `$sp`-relative local stack slots (`$fp`) and incoming stack
    // arguments (`argp`) so `sched1` sees the exact pre-reload distinct stack
    // base registers for `sched.c:true_dependence` memory aliasing.
    UneliminateStackMemsForSched1(&insn.pn.pattern, context.stack_layout);

    // Keep non-liveness notes (`REG_EQUIV`/`REG_EQUAL`) with renamed regs; drop stale `REG_DEAD`.
    std::vector<RtlRegNote> kept_notes;
    for (RtlRegNote note : insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) {
        continue;
      }
      RenameExprRegs(&note.datum, read_map);
      kept_notes.push_back(std::move(note));
    }
    insn.pn.reg_notes = std::move(kept_notes);
    view.core_insns.push_back(std::move(insn));
  }

  // 4. Derive `live_at_end_s1` and `facts_s1` (`reg_n_sets`, `reg_n_calls_crossed`, `max_reg_num`).
  DeriveSched1ViewFacts(context, mm, live_at_end_s2, cur_pseudo, live_in_pseudos,
                        live_in_reg_for_hard_reg, spill_slot_pseudo, spill_live_out_pseudos,
                        next_pseudo, &view);

  // 5. Block-0 leading entry-parameter copy pinning (`sched.c:3191-3215`).
  if (context.is_entry_block) {
    PinLeadingEntryParamCopiesForSched1View(&view);
  }

  return view;
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

bool IsReturnInternalPattern(const RtlExpr& pat) {
  return pat.op == RtlOp::kParallel && !pat.children.empty() &&
         pat.children[0].op == RtlOp::kReturn;
}

bool IsPrologueSpDecrementPattern(const RtlExpr& pat) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || dst.RegNo() != kMipsStackPointerRegno || src.children.size() != 2 ||
      src.children[0].op != RtlOp::kReg || src.children[0].RegNo() != kMipsStackPointerRegno) {
    return false;
  }
  if (src.op == RtlOp::kMinus) {
    return (src.children[1].op == RtlOp::kConstInt && src.children[1].int_val > 0) ||
           (src.children[1].op == RtlOp::kReg && src.children[1].RegNo() == kMipsTemp1Regno);
  }
  return src.op == RtlOp::kPlus && src.children[1].op == RtlOp::kConstInt &&
         src.children[1].int_val < 0;
}

bool IsPrologueFpSetupPattern(const RtlExpr& pat) {
  return pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
         pat.children[0].RegNo() == kMipsFramePointerRegno && !pat.children[0].flags.volatil &&
         pat.children[1].op == RtlOp::kReg && pat.children[1].RegNo() == kMipsStackPointerRegno;
}

bool IsProloguePrefixPattern(const RtlExpr& pat) {
  if (IsPrologueSpDecrementPattern(pat) || IsPrologueFpSetupPattern(pat)) {
    return true;
  }
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (dst.op == RtlOp::kMem && !dst.children.empty() && src.op == RtlOp::kReg) {
    const int sr = src.RegNo();
    const bool is_saved = (sr >= kMipsCalleeSavedGprFirst && sr <= kMipsCalleeSavedGprLast) ||
                          sr == kMipsFramePointerRegno || sr == kMipsReturnAddrRegno ||
                          (sr >= kMipsCalleeSavedFprFirst && sr <= kMipsFpRegLast);
    if (is_saved) {
      const RtlExpr& addr = dst.children[0];
      const bool sp_based = (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
                            (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                             addr.children[0].op == RtlOp::kReg &&
                             addr.children[0].RegNo() == kMipsStackPointerRegno &&
                             addr.children[1].op == RtlOp::kConstInt);
      if (sp_based) {
        return true;
      }
    }
  }
  return false;
}

bool IsEpilogueSuffixPattern(const RtlExpr& pat) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (dst.op == RtlOp::kReg && dst.RegNo() == kMipsStackPointerRegno) {
    if (src.op == RtlOp::kReg && src.RegNo() == kMipsFramePointerRegno) {
      return true;
    }
    if (src.op == RtlOp::kPlus && src.children.size() == 2 && src.children[0].op == RtlOp::kReg &&
        src.children[0].RegNo() == kMipsStackPointerRegno &&
        src.children[1].op == RtlOp::kConstInt) {
      return true;
    }
  }
  if (dst.op == RtlOp::kReg && src.op == RtlOp::kMem && !src.children.empty()) {
    const int dr = dst.RegNo();
    const bool is_saved = (dr >= kMipsCalleeSavedGprFirst && dr <= kMipsCalleeSavedGprLast) ||
                          dr == kMipsFramePointerRegno || dr == kMipsReturnAddrRegno ||
                          (dr >= kMipsCalleeSavedFprFirst && dr <= kMipsFpRegLast);
    if (is_saved) {
      const RtlExpr& addr = src.children[0];
      const bool sp_based = (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
                            (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                             addr.children[0].op == RtlOp::kReg &&
                             addr.children[0].RegNo() == kMipsStackPointerRegno &&
                             addr.children[1].op == RtlOp::kConstInt);
      if (sp_based) {
        return true;
      }
    }
  }
  return false;
}

enum class PreReloadStackBase {
  kOther,
  kSp,
  kFp,
  kArgp,
};

PreReloadStackBase ClassifyPreReloadStackBase(const RtlExpr& mem,
                                              const PostReloadStackLayout& layout) {
  if (!layout.valid || mem.op != RtlOp::kMem || mem.children.empty()) {
    return PreReloadStackBase::kOther;
  }
  const RtlExpr& a = mem.children[0];
  int base_reg = -1;
  int64_t off = 0;
  if (a.op == RtlOp::kReg) {
    base_reg = a.RegNo();
  } else if (a.op == RtlOp::kPlus && a.children.size() == 2 && a.children[0].op == RtlOp::kReg &&
             a.children[1].op == RtlOp::kConstInt) {
    base_reg = a.children[0].RegNo();
    off = a.children[1].int_val;
  } else {
    return PreReloadStackBase::kOther;
  }
  if (!layout.frame_pointer_needed) {
    if (base_reg != kMipsStackPointerRegno) {
      return PreReloadStackBase::kOther;
    }
    if (layout.frame_size > 0) {
      if (off >= layout.frame_size) {
        return PreReloadStackBase::kArgp;
      }
      if (off >= layout.starting_frame_offset) {
        return PreReloadStackBase::kFp;
      }
      return PreReloadStackBase::kSp;
    }
    return (off >= 0) ? PreReloadStackBase::kArgp : PreReloadStackBase::kSp;
  }
  if (base_reg == kMipsFramePointerRegno) {
    return (off >= layout.frame_size) ? PreReloadStackBase::kArgp : PreReloadStackBase::kFp;
  }
  if (base_reg == kMipsStackPointerRegno) {
    return PreReloadStackBase::kSp;
  }
  return PreReloadStackBase::kOther;
}

// Mirrors `local-alloc.c:547` + `sched.c:636-669, 817-840` (`true_dependence`)
// on post-reload `$sp`/`$fp`-addressed `MEM`s whose pre-reload bases differed
// (`$sp` vs `$fp` vs `argp`) before `reload1.c:eliminate_regs` (`mips.h:1649, 1756`).
bool PreReloadDistinctStackBasesConflict(const RtlExpr& write_mem, const RtlExpr& read_mem,
                                         const PostReloadStackLayout& layout) {
  if (read_mem.flags.unchanging && !write_mem.flags.unchanging) {
    return false;
  }
  const PreReloadStackBase wb = ClassifyPreReloadStackBase(write_mem, layout);
  const PreReloadStackBase rb = ClassifyPreReloadStackBase(read_mem, layout);
  if (wb == PreReloadStackBase::kOther || rb == PreReloadStackBase::kOther || wb == rb) {
    return false;
  }
  const bool w_varies = (wb == PreReloadStackBase::kSp);
  const bool r_varies = (rb == PreReloadStackBase::kSp);
  if (write_mem.flags.in_struct && w_varies && write_mem.mode != RtlMachineMode::kQi &&
      !read_mem.flags.in_struct && !r_varies) {
    return false;
  }
  if (read_mem.flags.in_struct && r_varies && read_mem.mode != RtlMachineMode::kQi &&
      !write_mem.flags.in_struct && !w_varies) {
    return false;
  }
  return true;
}

// Exact `local-alloc.c:540-603` (`validate_equiv_mem`) predicate using
// `TrueDependence` (`local-alloc.c:547`) and `REG_DEAD` death cutoff
// (`local-alloc.c:580-581`).
bool ValidateEquivMemForPostReloadGate(absl::Span<const Insn> seq, absl::Span<const int> orig_ids,
                                       int start_idx, int target_reg, const RtlExpr& mem_expr,
                                       const RegFacts& facts_s2, const MachineModel& mm,
                                       const PostReloadStackLayout& stack_layout,
                                       const absl::btree_set<int>& excluded_prologue_orig_ids) {
  if (mem_expr.op != RtlOp::kMem || mem_expr.children.empty() || mem_expr.flags.volatil) {
    return false;
  }
  absl::btree_set<int> addr_regs;
  auto insert_span = [&](int r, RtlMachineMode mode, absl::btree_set<int>* s) {
    if (r < 0 || r >= kMipsFirstPseudoRegister) {
      return;
    }
    const int nr = mm.HardRegNRegs(r, mode);
    for (int k = 0; k < nr; ++k) {
      s->insert(r + k);
    }
  };
  auto collect_addr = [&](auto& self, const RtlExpr& node) -> void {
    if (node.op == RtlOp::kReg) {
      insert_span(node.RegNo(), node.mode, &addr_regs);
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch);
    }
  };
  collect_addr(collect_addr, mem_expr.children[0]);

  auto store_modifies_equiv = [&](const RtlExpr& pat) -> bool {
    bool modified = false;
    auto check_elem = [&](const RtlExpr& elem) {
      if (modified) {
        return;
      }
      if ((elem.op != RtlOp::kSet && elem.op != RtlOp::kClobber) || elem.children.empty()) {
        return;
      }
      const RtlExpr& dest = elem.children[0];
      if (dest.op == RtlOp::kMem) {
        if (TrueDependence(dest, mem_expr, facts_s2, Phase::kSched2) ||
            PreReloadDistinctStackBasesConflict(dest, mem_expr, stack_layout)) {
          modified = true;
        }
        return;
      }
      const RtlExpr* inner = &dest;
      while ((inner->op == RtlOp::kSubreg || inner->op == RtlOp::kStrictLowPart ||
              inner->op == RtlOp::kZeroExtract || inner->op == RtlOp::kSignExtract) &&
             !inner->children.empty()) {
        inner = &inner->children[0];
      }
      if (inner->op == RtlOp::kReg && inner->RegNo() >= 0 &&
          inner->RegNo() < kMipsFirstPseudoRegister) {
        const int nr = mm.HardRegNRegs(inner->RegNo(), inner->mode);
        for (int k = 0; k < nr; ++k) {
          if (addr_regs.contains(inner->RegNo() + k)) {
            modified = true;
            return;
          }
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        check_elem(ch);
      }
    } else {
      check_elem(pat);
    }
    return modified;
  };

  const int n = static_cast<int>(seq.size());
  for (int j = start_idx; j < n; ++j) {
    if (excluded_prologue_orig_ids.contains(orig_ids[static_cast<size_t>(j)])) {
      continue;
    }
    if (j > start_idx &&
        HasRegDeadNoteCovering(seq[static_cast<size_t>(j)].pn.reg_notes, target_reg, mm)) {
      return true;
    }
    if (seq[static_cast<size_t>(j)].kind == Insn::Kind::kCall && !mem_expr.flags.unchanging &&
        !seq[static_cast<size_t>(j)].is_const_call) {
      return false;
    }
    if (store_modifies_equiv(seq[static_cast<size_t>(j)].pn.pattern)) {
      return false;
    }
    for (const RtlRegNote& note : seq[static_cast<size_t>(j)].pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
          note.datum.RegNo() >= 0 && note.datum.RegNo() < kMipsFirstPseudoRegister) {
        const int nr = mm.HardRegNRegs(note.datum.RegNo(), note.datum.mode);
        for (int k = 0; k < nr; ++k) {
          if (addr_regs.contains(note.datum.RegNo() + k)) {
            return false;
          }
        }
      }
    }
  }
  return false;
}

// Mirrors `local-alloc.c:609-665, 996` (`memref_used_between_p`), accounting for
// `reload1.c:eliminate_regs` having rewritten pre-reload `$fp`/`argp` accesses
// onto `$sp` (`29`) so disjoint post-reload `$sp` offsets from distinct pre-reload
// stack bases still alias under `local-alloc.c`'s pre-reload `true_dependence`.
bool MemrefUsedBetweenForPostReloadGate(absl::Span<const Insn> seq, absl::Span<const int> orig_ids,
                                        int start_idx, int end_idx, const RtlExpr& mem_expr,
                                        const RegFacts& facts_s2,
                                        const PostReloadStackLayout& stack_layout,
                                        const absl::btree_set<int>& excluded_prologue_orig_ids) {
  auto is_sp_addressed = [](const RtlExpr& m) {
    if (m.op != RtlOp::kMem || m.children.empty()) {
      return false;
    }
    const RtlExpr& addr = m.children[0];
    return (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
           (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
            addr.children[0].op == RtlOp::kReg &&
            addr.children[0].RegNo() == kMipsStackPointerRegno);
  };
  const bool store_uses_sp = !stack_layout.valid && is_sp_addressed(mem_expr);
  bool used = false;
  auto walk = [&](auto& self, const RtlExpr& node) -> void {
    if (used) {
      return;
    }
    if (node.op == RtlOp::kMem) {
      if ((store_uses_sp && is_sp_addressed(node)) ||
          PreReloadDistinctStackBasesConflict(mem_expr, node, stack_layout) ||
          TrueDependence(mem_expr, node, facts_s2, Phase::kSched2) ||
          AntiDependence(node, mem_expr, facts_s2, Phase::kSched2)) {
        used = true;
        return;
      }
    }
    if (node.op == RtlOp::kSet && node.children.size() == 2 && node.SetDest().op == RtlOp::kMem) {
      if (!node.SetDest().children.empty()) {
        self(self, node.SetDest().children[0]);
      }
      self(self, node.SetSrc());
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch);
    }
  };
  for (int j = start_idx + 1; j < end_idx && !used; ++j) {
    if (excluded_prologue_orig_ids.contains(orig_ids[static_cast<size_t>(j)])) {
      continue;
    }
    walk(walk, seq[static_cast<size_t>(j)].pn.pattern);
  }
  return used;
}

bool TryExtractPlainStackSlotOffset(const RtlExpr& addr, int64_t* off_out) {
  if (addr.op == RtlOp::kReg &&
      (addr.RegNo() == kMipsStackPointerRegno || addr.RegNo() == kMipsFramePointerRegno)) {
    if (off_out != nullptr) {
      *off_out = 0;
    }
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      (addr.children[0].RegNo() == kMipsStackPointerRegno ||
       addr.children[0].RegNo() == kMipsFramePointerRegno) &&
      addr.children[1].op == RtlOp::kConstInt) {
    if (off_out != nullptr) {
      *off_out = addr.children[1].int_val;
    }
    return true;
  }
  return false;
}

bool HasNoEquivOrEqualNotes(const Insn& insn) {
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual) {
      return false;
    }
  }
  return true;
}

bool PatternReadsHardRegSpan(const RtlExpr& pattern, int r, int nr, const MachineModel& mm) {
  bool reads_r = false;
  auto check_reads = [&](auto& self, const RtlExpr& node) -> void {
    if (reads_r) {
      return;
    }
    if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kMipsFirstPseudoRegister) {
      const int cnr = mm.HardRegNRegs(node.RegNo(), node.mode);
      if (node.RegNo() < r + nr && r < node.RegNo() + cnr) {
        reads_r = true;
      }
      return;
    }
    if (node.op == RtlOp::kSet && node.children.size() == 2) {
      if (node.SetDest().op == RtlOp::kMem && !node.SetDest().children.empty()) {
        self(self, node.SetDest().children[0]);
      } else if (node.SetDest().op != RtlOp::kReg) {
        self(self, node.SetDest());
      }
      self(self, node.SetSrc());
      return;
    }
    if (node.op == RtlOp::kClobber) {
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch);
    }
  };
  check_reads(check_reads, pattern);
  return reads_r;
}

bool InsnReadsHardRegSpan(const Insn& insn, int r, int nr, const MachineModel& mm) {
  return PatternReadsHardRegSpan(insn.pn.pattern, r, nr, mm);
}

bool PatternDefinesHardRegSpan(const RtlExpr& pattern, int r, int nr, const MachineModel& mm) {
  bool defs_r = false;
  auto check_elem = [&](const RtlExpr& elem) {
    if (defs_r || elem.op != RtlOp::kSet || elem.children.size() != 2) {
      return;
    }
    const RtlExpr& d = elem.SetDest();
    if (d.op == RtlOp::kReg && d.RegNo() >= 0 && d.RegNo() < kMipsFirstPseudoRegister) {
      const int dnr = mm.HardRegNRegs(d.RegNo(), d.mode);
      if (d.RegNo() < r + nr && r < d.RegNo() + dnr) {
        defs_r = true;
      }
    }
  };
  if (pattern.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pattern.children) {
      check_elem(ch);
    }
  } else {
    check_elem(pattern);
  }
  return defs_r;
}

bool InsnDefinesHardRegSpan(const Insn& insn, int r, int nr, const MachineModel& mm) {
  return PatternDefinesHardRegSpan(insn.pn.pattern, r, nr, mm);
}

struct HardRegLivenessSummary {
  absl::btree_set<int> live_at_entry;
  absl::btree_set<std::pair<int, int>> interferences;
  absl::btree_set<int> call_crossing_regs;
  absl::btree_map<int, bool> mem_equiv_valid;
};

HardRegLivenessSummary ComputeHardRegLivenessSummary(
    absl::Span<const Insn> seq, absl::Span<const int> orig_ids,
    const absl::btree_set<int>& live_at_end, const RegFacts& facts_s2, const MachineModel& mm,
    const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_prologue_orig_ids) {
  HardRegLivenessSummary out;
  const int n = static_cast<int>(seq.size());
  std::vector<absl::btree_set<int>> reads(static_cast<size_t>(n));
  std::vector<absl::btree_set<int>> full_defs(static_cast<size_t>(n));
  std::vector<absl::btree_set<int>> partial_defs(static_cast<size_t>(n));
  std::vector<bool> is_call(static_cast<size_t>(n), false);

  auto insert_span = [&](int r, RtlMachineMode mode, absl::btree_set<int>* s) {
    if (r < 0 || r >= kMipsFirstPseudoRegister) {
      return;
    }
    const int nr = mm.HardRegNRegs(r, mode);
    for (int k = 0; k < nr; ++k) {
      s->insert(r + k);
    }
  };
  auto walk_reads = [&](auto& self, const RtlExpr& node, absl::btree_set<int>* dst) -> void {
    if (node.op == RtlOp::kReg) {
      insert_span(node.RegNo(), node.mode, dst);
      return;
    }
    if (node.op == RtlOp::kSet && node.children.size() == 2) {
      if (node.SetDest().op == RtlOp::kMem && !node.SetDest().children.empty()) {
        self(self, node.SetDest().children[0], dst);
      } else if (node.SetDest().op != RtlOp::kReg) {
        self(self, node.SetDest(), dst);
      }
      self(self, node.SetSrc(), dst);
      return;
    }
    if (node.op == RtlOp::kClobber) {
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch, dst);
    }
  };
  auto walk_defs = [&](const RtlExpr& pat, absl::btree_set<int>* fd, absl::btree_set<int>* pd) {
    auto handle = [&](const RtlExpr& elem) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        const RtlExpr& d = elem.SetDest();
        if (d.op == RtlOp::kReg) {
          insert_span(d.RegNo(), d.mode, fd);
        } else if (d.op != RtlOp::kMem) {
          const RtlExpr* cur = &d;
          while ((cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
                  cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) &&
                 !cur->children.empty()) {
            cur = &cur->children[0];
          }
          if (cur->op == RtlOp::kReg) {
            insert_span(cur->RegNo(), cur->mode, pd);
          }
        }
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty() &&
                 elem.children[0].op == RtlOp::kReg) {
        insert_span(elem.children[0].RegNo(), elem.children[0].mode, fd);
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        handle(ch);
      }
    } else {
      handle(pat);
    }
  };

  for (int i = 0; i < n; ++i) {
    is_call[static_cast<size_t>(i)] = (seq[static_cast<size_t>(i)].kind == Insn::Kind::kCall);
    walk_reads(walk_reads, seq[static_cast<size_t>(i)].pn.pattern, &reads[static_cast<size_t>(i)]);
    for (const RtlExpr& fu : seq[static_cast<size_t>(i)].pn.call_fusage) {
      walk_reads(walk_reads, fu, &reads[static_cast<size_t>(i)]);
    }
    walk_defs(seq[static_cast<size_t>(i)].pn.pattern, &full_defs[static_cast<size_t>(i)],
              &partial_defs[static_cast<size_t>(i)]);
  }

  // Track `full_live` over the entire sequence for `live_at_entry`, and
  // `body_live` excluding post-reload interleaved prologue instructions
  // (`excluded_prologue_orig_ids`, emitted after `reload` by
  // `mips.c:mips_expand_prologue` / `toplev.c:3098-3103`) for `interferences`
  // and `call_crossing_regs` so hoisting a prologue callee-save store back to
  // the prologue never looks like a dropped pre-reload interference.
  absl::btree_set<int> full_live;
  for (int r : live_at_end) {
    if (r >= 0 && r < kMipsFirstPseudoRegister) {
      full_live.insert(r);
    }
  }
  absl::btree_set<int> body_live = full_live;
  for (int i = n - 1; i >= 0; --i) {
    const bool is_excluded_prologue =
        excluded_prologue_orig_ids.contains(orig_ids[static_cast<size_t>(i)]);
    if (!is_excluded_prologue) {
      absl::btree_set<int> all_defs = full_defs[static_cast<size_t>(i)];
      all_defs.insert(partial_defs[static_cast<size_t>(i)].begin(),
                      partial_defs[static_cast<size_t>(i)].end());
      for (int d : all_defs) {
        for (int l : body_live) {
          if (d != l) {
            out.interferences.insert({std::min(d, l), std::max(d, l)});
          }
        }
        for (int d2 : all_defs) {
          if (d < d2) {
            out.interferences.insert({d, d2});
          }
        }
      }
      for (int d : full_defs[static_cast<size_t>(i)]) {
        body_live.erase(d);
      }
      if (is_call[static_cast<size_t>(i)]) {
        for (auto it = body_live.begin(); it != body_live.end();) {
          if (mm.CallUsed(*it)) {
            it = body_live.erase(it);
          } else {
            out.call_crossing_regs.insert(*it);
            ++it;
          }
        }
      }
      body_live.insert(reads[static_cast<size_t>(i)].begin(), reads[static_cast<size_t>(i)].end());
    }
    for (int d : full_defs[static_cast<size_t>(i)]) {
      full_live.erase(d);
    }
    if (is_call[static_cast<size_t>(i)]) {
      for (auto it = full_live.begin(); it != full_live.end();) {
        if (mm.CallUsed(*it)) {
          it = full_live.erase(it);
        } else {
          ++it;
        }
      }
    }
    full_live.insert(reads[static_cast<size_t>(i)].begin(), reads[static_cast<size_t>(i)].end());
  }
  out.live_at_entry = std::move(full_live);

  // Exact `local-alloc.c:540-603, 990-1055` (`validate_equiv_mem`) status for every
  // `MEM` load and store so post-reload reordering never flips `update_equiv_regs`.
  for (int i = 0; i < n; ++i) {
    if (excluded_prologue_orig_ids.contains(orig_ids[static_cast<size_t>(i)])) {
      continue;
    }
    const RtlExpr& pat = seq[static_cast<size_t>(i)].pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
      continue;
    }
    if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kMem &&
        !pat.SetSrc().children.empty()) {
      const int target_reg = pat.SetDest().RegNo();
      // `function.c:3832-3834` (`assign_parms`) attaches `REG_EQUIV` to incoming
      // stack-parameter loads before `local-alloc`; `local-alloc.c:1049-1055`
      // (`if (note == 0 && ... && validate_equiv_mem(...))`) never calls
      // `validate_equiv_mem` when `REG_EQUIV` is already present.
      int64_t arg_off = 0;
      const bool is_incoming_arg_with_equiv =
          stack_layout.valid &&
          TryExtractPlainStackSlotOffset(pat.SetSrc().children[0], &arg_off) &&
          arg_off >= stack_layout.frame_size &&
          std::any_of(seq[static_cast<size_t>(i)].pn.reg_notes.begin(),
                      seq[static_cast<size_t>(i)].pn.reg_notes.end(), [](const RtlRegNote& n) {
                        return n.kind == RtlMachineMode::kRegEquiv && n.datum.op == RtlOp::kMem;
                      });
      if (target_reg >= 0 && target_reg < kMipsFirstPseudoRegister && !is_incoming_arg_with_equiv) {
        out.mem_equiv_valid[orig_ids[static_cast<size_t>(i)]] =
            ValidateEquivMemForPostReloadGate(seq, orig_ids, i, target_reg, pat.SetSrc(), facts_s2,
                                              mm, stack_layout, excluded_prologue_orig_ids);
      }
    } else if (pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
               pat.SetSrc().op == RtlOp::kReg) {
      const int target_reg = pat.SetSrc().RegNo();
      if (target_reg >= 0 && target_reg < kMipsFirstPseudoRegister) {
        int init_idx = -1;
        for (int j = i - 1; j >= 0; --j) {
          if (is_call[static_cast<size_t>(j)]) {
            break;
          }
          if (full_defs[static_cast<size_t>(j)].contains(target_reg)) {
            const RtlExpr& jpat = seq[static_cast<size_t>(j)].pn.pattern;
            // `local-alloc.c:1021` skips `reg_n_sets[regno] != 1`; `flow.c:2506-2515`
            // never attaches `REG_DEAD` on a read-modify-write of the same pseudo,
            // so a self-reading def without `REG_DEAD` proves `reg_n_sets >= 2`.
            const bool is_multiset_rmw =
                reads[static_cast<size_t>(j)].contains(target_reg) &&
                !HasRegDeadNoteCovering(seq[static_cast<size_t>(j)].pn.reg_notes, target_reg, mm);
            if (!is_multiset_rmw && jpat.op == RtlOp::kSet && jpat.children.size() == 2 &&
                jpat.SetDest().op == RtlOp::kReg && jpat.SetDest().RegNo() == target_reg) {
              init_idx = j;
            }
            break;
          }
        }
        bool equiv_ok = false;
        if (init_idx >= 0 &&
            ValidateEquivMemForPostReloadGate(seq, orig_ids, init_idx, target_reg, pat.SetDest(),
                                              facts_s2, mm, stack_layout,
                                              excluded_prologue_orig_ids) &&
            !MemrefUsedBetweenForPostReloadGate(seq, orig_ids, init_idx, i, pat.SetDest(), facts_s2,
                                                stack_layout, excluded_prologue_orig_ids)) {
          equiv_ok = true;
        }
        out.mem_equiv_valid[orig_ids[static_cast<size_t>(i)]] = equiv_ok;
      }
    }
  }
  return out;
}

void CollectHardRegDefBaseRegnos(const RtlExpr& pat, absl::btree_map<int, int>* counts,
                                 absl::btree_map<int, RtlMachineMode>* last_def_mode = nullptr) {
  auto record = [&](const RtlExpr& elem) {
    if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
      const RtlExpr* dst = &elem.children[0];
      while ((dst->op == RtlOp::kSubreg || dst->op == RtlOp::kStrictLowPart ||
              dst->op == RtlOp::kZeroExtract || dst->op == RtlOp::kSignExtract) &&
             !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (dst->op == RtlOp::kReg && dst->RegNo() >= 0 && dst->RegNo() < kMipsFirstPseudoRegister) {
        (*counts)[dst->RegNo()] += 1;
        if (last_def_mode != nullptr && elem.op == RtlOp::kSet &&
            dst->mode != RtlMachineMode::kVoid) {
          (*last_def_mode)[dst->RegNo()] = dst->mode;
        }
      }
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      record(ch);
    }
  } else {
    record(pat);
  }
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

// Matches leading `< 4`-byte by-value struct/union argument right-shifts
// emitted by `mips_expand_prologue` (`config/mips/mips.c:4976-4992`) before
// any `$sp` adjustment:
//   `(set (reg:SI r) (ashiftrt:SI (reg:SI r) (const_int 8|16|24)))`
// for strictly increasing `r in [4, 7]` with only flow-liveness notes
// (matching `StripPrologueAndEpilogue`'s `HasOnlyFlowLivenessNotes` guard).
bool MatchPrologueStructArgShiftPrefixInsn(const Insn& insn, int* out_regno) {
  if (insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  for (const RtlRegNote& n : insn.pn.reg_notes) {
    if (n.kind != RtlMachineMode::kRegDead && n.kind != RtlMachineMode::kRegUnused) {
      return false;
    }
  }
  const RtlExpr& pat = insn.pn.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (dst.op != RtlOp::kReg || dst.mode != RtlMachineMode::kSi || dst.flags.volatil ||
      dst.flags.in_struct || dst.flags.integrated) {
    return false;
  }
  const int r = dst.RegNo();
  if (r < 4 || r > 7) {
    return false;
  }
  if (src.op != RtlOp::kAshiftrt || src.mode != RtlMachineMode::kSi || src.children.size() != 2 ||
      src.children[0].op != RtlOp::kReg || src.children[0].mode != RtlMachineMode::kSi ||
      src.children[0].RegNo() != r || src.children[0].flags.volatil ||
      src.children[0].flags.in_struct || src.children[0].flags.integrated ||
      src.children[1].op != RtlOp::kConstInt) {
    return false;
  }
  const int64_t sh = src.children[1].int_val;
  if (sh != 8 && sh != 16 && sh != 24) {
    return false;
  }
  *out_regno = r;
  return true;
}

// Matches a pre-`$sp`-decrement variadic GP argument home store emitted by
// `mips_expand_prologue` (`config/mips/mips.c:4996-5020`):
//   `(set (mem:<SI|DI> <$sp or $sp + (r - 4) * word_bytes>) (reg:<SI|DI> r))`
// for `r in [4, 7]` carrying only flow-liveness notes.
bool MatchPrologueVariadicArgStorePrefixInsn(const Insn& insn, int* out_regno) {
  if (insn.kind != Insn::Kind::kInsn) {
    return false;
  }
  for (const RtlRegNote& n : insn.pn.reg_notes) {
    if (n.kind != RtlMachineMode::kRegDead && n.kind != RtlMachineMode::kRegUnused) {
      return false;
    }
  }
  const RtlExpr& pat = insn.pn.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (dst.op != RtlOp::kMem || dst.children.size() != 1 || dst.flags.volatil ||
      dst.flags.in_struct || src.op != RtlOp::kReg || src.flags.volatil || src.flags.in_struct ||
      dst.mode != src.mode ||
      (dst.mode != RtlMachineMode::kSi && dst.mode != RtlMachineMode::kDi)) {
    return false;
  }
  const int r = src.RegNo();
  if (r < kMipsArgRegFirst || r > kMipsArgRegLast) {
    return false;
  }
  const int word_bytes = (dst.mode == RtlMachineMode::kDi) ? 8 : 4;
  const int64_t expected_offset = static_cast<int64_t>(r - kMipsArgRegFirst) * word_bytes;
  const RtlExpr& addr = dst.children[0];
  if (expected_offset == 0) {
    if (addr.op != RtlOp::kReg || addr.RegNo() != kMipsStackPointerRegno) {
      return false;
    }
  } else {
    if (addr.op != RtlOp::kPlus || addr.children.size() != 2 ||
        addr.children[0].op != RtlOp::kReg || addr.children[0].RegNo() != kMipsStackPointerRegno ||
        addr.children[1].op != RtlOp::kConstInt || addr.children[1].int_val != expected_offset) {
      return false;
    }
  }
  *out_regno = r;
  return true;
}

PostReloadBodyBounds FindPostReloadBodyBounds(absl::Span<const Insn> sched_block,
                                              size_t block_idx) {
  const int n = static_cast<int>(sched_block.size());
  int prologue_end = 0;
  if (block_idx == 0) {
    int last_shift_reg = 3;
    while (prologue_end < n) {
      int shift_reg = -1;
      if (MatchPrologueStructArgShiftPrefixInsn(sched_block[static_cast<size_t>(prologue_end)],
                                                &shift_reg) &&
          shift_reg > last_shift_reg) {
        last_shift_reg = shift_reg;
        ++prologue_end;
        continue;
      }
      break;
    }
    // Skip contiguous leading variadic GP argument home stores emitted by
    // `mips_expand_prologue` (`config/mips/mips.c:4996-5020`) prior to `$sp`
    // decrement: they form a contiguous suffix `[min_reg .. $a3]` and precede a
    // prologue `$sp` decrement before any call, jump, or other `$sp` write.
    {
      int scan = prologue_end;
      absl::btree_set<int> vararg_regs;
      while (scan < n) {
        int vararg_reg = -1;
        if (!MatchPrologueVariadicArgStorePrefixInsn(sched_block[static_cast<size_t>(scan)],
                                                     &vararg_reg) ||
            !vararg_regs.insert(vararg_reg).second) {
          break;
        }
        ++scan;
      }
      if (!vararg_regs.empty()) {
        const int min_reg = *vararg_regs.begin();
        const bool is_contiguous_suffix =
            *vararg_regs.rbegin() == kMipsArgRegLast &&
            static_cast<int>(vararg_regs.size()) == (kMipsArgRegLast - min_reg + 1);
        bool has_sp_dec_ahead = false;
        if (is_contiguous_suffix) {
          for (int k = scan; k < n; ++k) {
            const Insn& ki = sched_block[static_cast<size_t>(k)];
            if (ki.kind == Insn::Kind::kCall || ki.kind == Insn::Kind::kJump) {
              break;
            }
            if (IsPrologueSpDecrementPattern(ki.pn.pattern)) {
              has_sp_dec_ahead = true;
              break;
            }
            if (ki.pn.pattern.op == RtlOp::kSet && ki.pn.pattern.children.size() == 2 &&
                ki.pn.pattern.SetDest().op == RtlOp::kReg &&
                ki.pn.pattern.SetDest().RegNo() == kMipsStackPointerRegno) {
              break;
            }
          }
        }
        if (has_sp_dec_ahead) {
          prologue_end = scan;
        }
      }
    }
    while (prologue_end < n &&
           IsProloguePrefixPattern(sched_block[static_cast<size_t>(prologue_end)].pn.pattern)) {
      ++prologue_end;
    }
  }
  int epilogue_begin = n;
  int scan = n;
  while (scan > prologue_end) {
    const RtlExpr& pat = sched_block[static_cast<size_t>(scan - 1)].pn.pattern;
    if (pat.op == RtlOp::kUse || pat.op == RtlOp::kReturn || IsReturnInternalPattern(pat)) {
      --scan;
    } else {
      break;
    }
  }
  int ep = scan;
  while (ep > prologue_end &&
         IsEpilogueSuffixPattern(sched_block[static_cast<size_t>(ep - 1)].pn.pattern)) {
    --ep;
  }
  if (ep < scan) {
    epilogue_begin = ep;
  } else {
    // Even in frameless leaf functions with no stack-pop epilogue, trailing
    // `(return)` / `return_internal` insns are emitted by
    // `thread_prologue_and_epilogue_insns` (`toplev.c:3103`, `function.c:5516`,
    // `mips.c:mips_expand_epilogue`) after `reload` and do not exist in `sched1`.
    // Trailing `(use (reg/i ...))` markers DO exist in `sched1` and remain inside
    // `[prologue_end, epilogue_begin)`.
    while (epilogue_begin > prologue_end) {
      const RtlExpr& pat = sched_block[static_cast<size_t>(epilogue_begin - 1)].pn.pattern;
      if (pat.op == RtlOp::kReturn || IsReturnInternalPattern(pat)) {
        --epilogue_begin;
      } else {
        break;
      }
    }
  }
  return PostReloadBodyBounds{.prologue_end = prologue_end, .epilogue_begin = epilogue_begin};
}

absl::btree_set<int> FindInterleavedPrologueSaveBodyIndices(absl::Span<const Insn> sched_block,
                                                            const PostReloadBodyBounds& bounds,
                                                            size_t block_idx,
                                                            const MachineModel& mm) {
  absl::btree_set<int> excluded;
  if (block_idx != 0 || bounds.epilogue_begin <= bounds.prologue_end) {
    return excluded;
  }
  auto match_callee_save_store = [](const RtlExpr& pat, int* out_reg,
                                    RtlMachineMode* out_mode) -> bool {
    if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
      return false;
    }
    const RtlExpr& dst = pat.SetDest();
    const RtlExpr& src = pat.SetSrc();
    if (dst.op != RtlOp::kMem || dst.children.empty() || dst.flags.volatil ||
        src.op != RtlOp::kReg) {
      return false;
    }
    const int sr = src.RegNo();
    const bool is_saved = (sr >= kMipsCalleeSavedGprFirst && sr <= kMipsCalleeSavedGprLast) ||
                          sr == kMipsFramePointerRegno || sr == kMipsReturnAddrRegno ||
                          (sr >= kMipsCalleeSavedFprFirst && sr <= kMipsFpRegLast);
    if (!is_saved) {
      return false;
    }
    const RtlExpr& addr = dst.children[0];
    const bool sp_based =
        (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
        (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
         addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == kMipsStackPointerRegno &&
         addr.children[1].op == RtlOp::kConstInt);
    if (!sp_based) {
      return false;
    }
    *out_reg = sr;
    *out_mode = src.mode;
    return true;
  };

  bool saw_sp_dec = false;
  absl::btree_set<int> saved_regs;
  for (int i = 0; i < bounds.prologue_end; ++i) {
    const RtlExpr& pat = sched_block[static_cast<size_t>(i)].pn.pattern;
    if (IsPrologueSpDecrementPattern(pat)) {
      saw_sp_dec = true;
    }
    if (IsPrologueFpSetupPattern(pat)) {
      saved_regs.insert(kMipsFramePointerRegno);
    }
    int sr = -1;
    RtlMachineMode smode = RtlMachineMode::kVoid;
    if (match_callee_save_store(pat, &sr, &smode)) {
      const int nr = mm.HardRegNRegs(sr, smode);
      for (int k = 0; k < nr; ++k) {
        saved_regs.insert(sr + k);
      }
    }
  }
  auto collect_written_hard_regs = [&](const RtlExpr& pat, absl::btree_set<int>* out_defs) {
    auto handle = [&](const RtlExpr& elem) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
        const RtlExpr* cur = &elem.children[0];
        if (cur->op == RtlOp::kMem) {
          return;
        }
        while ((cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
                cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) &&
               !cur->children.empty()) {
          cur = &cur->children[0];
        }
        if (cur->op == RtlOp::kReg && cur->RegNo() >= 0 &&
            cur->RegNo() < kMipsFirstPseudoRegister) {
          const int nr = mm.HardRegNRegs(cur->RegNo(), cur->mode);
          for (int k = 0; k < nr; ++k) {
            out_defs->insert(cur->RegNo() + k);
          }
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        handle(ch);
      }
    } else {
      handle(pat);
    }
  };

  absl::btree_set<int> body_writes;
  bool body_touched_sp = false;
  for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
    const Insn& insn = sched_block[static_cast<size_t>(i)];
    if (insn.kind == Insn::Kind::kCall || insn.kind == Insn::Kind::kJump) {
      break;
    }
    const RtlExpr& pat = insn.pn.pattern;
    if (!saw_sp_dec) {
      if (insn.kind == Insn::Kind::kInsn && HasNoEquivOrEqualNotes(insn) &&
          IsPrologueSpDecrementPattern(pat) && !body_touched_sp &&
          !body_writes.contains(kMipsStackPointerRegno)) {
        saw_sp_dec = true;
        excluded.insert(i - bounds.prologue_end);
        continue;
      }
      if (pat.ContainsReg(kMipsStackPointerRegno)) {
        body_touched_sp = true;
      }
      collect_written_hard_regs(pat, &body_writes);
      continue;
    }
    if (insn.kind == Insn::Kind::kInsn && HasNoEquivOrEqualNotes(insn)) {
      if (IsPrologueFpSetupPattern(pat) && !body_writes.contains(kMipsFramePointerRegno) &&
          !saved_regs.contains(kMipsFramePointerRegno)) {
        excluded.insert(i - bounds.prologue_end);
        saved_regs.insert(kMipsFramePointerRegno);
        continue;
      }
      int sr = -1;
      RtlMachineMode smode = RtlMachineMode::kVoid;
      if (match_callee_save_store(pat, &sr, &smode)) {
        const int nr = mm.HardRegNRegs(sr, smode);
        bool conflict = false;
        for (int k = 0; k < nr; ++k) {
          if (saved_regs.contains(sr + k) || body_writes.contains(sr + k)) {
            conflict = true;
            break;
          }
        }
        if (!conflict) {
          excluded.insert(i - bounds.prologue_end);
          for (int k = 0; k < nr; ++k) {
            saved_regs.insert(sr + k);
          }
          continue;
        }
      }
    }
    absl::btree_set<int> defs;
    collect_written_hard_regs(pat, &defs);
    if (defs.contains(kMipsStackPointerRegno)) {
      break;
    }
    body_writes.insert(defs.begin(), defs.end());
  }
  return excluded;
}

bool IsAdjacentReloadContiguityPair(const Insn& first_insn, const Insn& second_insn,
                                    const MachineModel& mm,
                                    const PostReloadStackLayout& stack_layout) {
  auto is_plausible_spill_slot_offset = [&](int64_t off, bool need_store, bool need_load) {
    // `caller-save.c:455-462` emits caller-save stores before `CALL_INSN`
    // (not adjacent to the defining insn), whereas `caller-save.c:410-411,
    // 568-615` (`restore_referenced_regs`) emits caller-save restores
    // immediately before the first post-call consumer.
    if (need_load && stack_layout.caller_save_slot_offsets.contains(off)) {
      return false;
    }
    if (stack_layout.valid && stack_layout.frame_size > 0) {
      const int64_t min_spill =
          std::max(stack_layout.starting_frame_offset, stack_layout.min_valid_reload_spill_offset);
      if (off < min_spill || off >= stack_layout.frame_size) {
        return false;
      }
    }
    if (stack_layout.has_stack_slot_access_info) {
      if (need_store && !stack_layout.stack_slot_store_offsets.contains(off)) {
        return false;
      }
      if (need_load && !stack_layout.stack_slot_load_offsets.contains(off)) {
        return false;
      }
    }
    return true;
  };
  // (a) Stack-slot input reload or (b) cross-bank GPR->FPR (`mtc1`) input reload.
  if (first_insn.kind == Insn::Kind::kInsn && second_insn.kind != Insn::Kind::kCall &&
      HasNoEquivOrEqualNotes(first_insn)) {
    const RtlExpr& pat = first_insn.pn.pattern;
    if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg &&
        !pat.SetDest().flags.integrated) {
      const int r = pat.SetDest().RegNo();
      const int nr = mm.HardRegNRegs(r, pat.SetDest().mode);
      if (r > 0 && r < kMipsFirstPseudoRegister && r != kMipsStackPointerRegno &&
          r != kMipsFramePointerRegno) {
        const RtlExpr& src = pat.SetSrc();
        int64_t load_off = 0;
        const bool is_stack_load =
            (src.op == RtlOp::kMem && !src.children.empty() && !src.flags.volatil &&
             !src.flags.in_struct && !src.flags.unchanging &&
             TryExtractPlainStackSlotOffset(src.children[0], &load_off) &&
             is_plausible_spill_slot_offset(load_off, /*need_store=*/true, /*need_load=*/false));
        bool is_gpr_to_fpr_reload =
            (r >= kMipsFpRegFirst && r <= kMipsFpRegLast && src.op == RtlOp::kReg &&
             src.RegNo() >= 2 && src.RegNo() < kMipsFpRegFirst &&
             (pat.SetDest().mode == RtlMachineMode::kSf ||
              pat.SetDest().mode == RtlMachineMode::kDf));
        if (is_gpr_to_fpr_reload) {
          // `reload1.c:5849-5871, 6118-6125` never attaches `REG_DEAD gpr` onto
          // an emitted `mtc1` input reload (whereas `function.c:assign_parms`
          // entry-parameter copies carry `REG_DEAD gpr` from `flow.c`).
          const int gpr = src.RegNo();
          const int gnr = std::max(1, mm.HardRegNRegs(gpr, src.mode));
          for (const RtlRegNote& n : first_insn.pn.reg_notes) {
            if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                n.datum.RegNo() >= gpr && n.datum.RegNo() < gpr + gnr) {
              is_gpr_to_fpr_reload = false;
              break;
            }
          }
        }
        if ((is_stack_load || is_gpr_to_fpr_reload) &&
            InsnReadsHardRegSpan(second_insn, r, nr, mm)) {
          return true;
        }
      }
    }
  }
  // (c) Stack-slot output spill immediately after its defining insn.
  if (first_insn.kind == Insn::Kind::kInsn && second_insn.kind == Insn::Kind::kInsn &&
      HasNoEquivOrEqualNotes(second_insn)) {
    const RtlExpr& pat2 = second_insn.pn.pattern;
    int64_t store_off = 0;
    if (pat2.op == RtlOp::kSet && pat2.children.size() == 2 && pat2.SetDest().op == RtlOp::kMem &&
        !pat2.SetDest().children.empty() && !pat2.SetDest().flags.volatil &&
        !pat2.SetDest().flags.in_struct && !pat2.SetDest().flags.unchanging &&
        pat2.SetSrc().op == RtlOp::kReg &&
        TryExtractPlainStackSlotOffset(pat2.SetDest().children[0], &store_off) &&
        is_plausible_spill_slot_offset(store_off, /*need_store=*/false, /*need_load=*/true)) {
      const int r = pat2.SetSrc().RegNo();
      if (r > 0 && r < kMipsFirstPseudoRegister && r != kMipsStackPointerRegno &&
          r != kMipsFramePointerRegno) {
        const int nr = mm.HardRegNRegs(r, pat2.SetSrc().mode);
        if (InsnDefinesHardRegSpan(first_insn, r, nr, mm)) {
          return true;
        }
      }
    }
  }
  // (d) Cross-bank `"=f"` FPR->GPR (`mfc1`) output reload immediately after its FP op.
  return TryMatchAdjacentFprOutputReload(first_insn, second_insn, /*gpr_dest_out=*/nullptr);
}

namespace {

struct BlockWebEndpointSummary {
  absl::btree_map<int, RtlMachineMode> gen_def_mode;
  absl::btree_set<int> kill_regs;
  absl::btree_set<int> upward_use_regs;
  absl::btree_set<int> upward_call_fusage_use_regs;
  absl::btree_set<int> upward_return_use_regs;
  absl::btree_set<int> upward_use_rmw_regs;
  absl::btree_set<int> live_in_regs;
  // Exit-reaching defs of an ABI call-arg register not yet counted as
  // intra-block unrenamed hard-reg defs; promoted to `block_unrenamed_hard_defs`
  // only when the def actually reaches a downstream `CALL_INSN_FUNCTION_USAGE`.
  absl::btree_set<int> uncounted_exit_def_regs;
};

// Computes exact CFG reaching-definition webs (`flow.c:reg_n_sets`,
// `sched.c:2469-2501` `birthing_insn_p`) per hard register so disjoint webs on
// the same hard register in unrelated blocks never inflate `reg_n_sets`.
void ComputeExactCfgWebDefCounts(const RtlControlFlowGraph& cfg,
                                 const std::vector<BlockWebEndpointSummary>& blk_info,
                                 CfgSched1Context* out) {
  const size_t num_blocks = cfg.rpo_blocks.size();
  out->block_entry_web_other_defs.assign(num_blocks, {});
  out->block_exit_web_other_defs.assign(num_blocks, {});
  out->block_entry_exit_same_web_regs.assign(num_blocks, {});
  if (num_blocks == 0) {
    return;
  }

  std::vector<std::vector<size_t>> preds(num_blocks);
  for (size_t b = 0; b < num_blocks; ++b) {
    for (int succ : cfg.rpo_blocks[b].successors) {
      if (succ >= 0 && static_cast<size_t>(succ) < num_blocks) {
        preds[static_cast<size_t>(succ)].push_back(b);
      }
    }
  }

  absl::btree_set<int> active_hard_regs;
  for (size_t b = 0; b < num_blocks; ++b) {
    for (const auto& [r, _] : blk_info[b].gen_def_mode) {
      active_hard_regs.insert(r);
    }
  }

  auto modes_compatible = [](RtlMachineMode a, RtlMachineMode b) {
    return a == RtlMachineMode::kVoid || b == RtlMachineMode::kVoid || a == b;
  };

  for (int r : active_hard_regs) {
    std::vector<absl::btree_set<size_t>> reach_in(num_blocks);
    std::vector<absl::btree_set<size_t>> reach_out(num_blocks);
    bool changed = true;
    for (size_t iter = 0; iter <= num_blocks + 2 && changed; ++iter) {
      changed = false;
      for (size_t b = 0; b < num_blocks; ++b) {
        absl::btree_set<size_t> new_in;
        for (size_t p : preds[b]) {
          new_in.insert(reach_out[p].begin(), reach_out[p].end());
        }
        absl::btree_set<size_t> new_out;
        if (blk_info[b].gen_def_mode.contains(r)) {
          new_out.insert(b);
        } else if (!blk_info[b].kill_regs.contains(r)) {
          new_out = new_in;
        }
        if (new_in != reach_in[b] || new_out != reach_out[b]) {
          reach_in[b] = std::move(new_in);
          reach_out[b] = std::move(new_out);
          changed = true;
        }
      }
    }

    // Union defining blocks that jointly reach a common upward-exposed use,
    // live-in join point, or non-dying RMW in the same machine mode (`flow.c`).
    std::vector<size_t> parent(num_blocks);
    for (size_t b = 0; b < num_blocks; ++b) {
      parent[b] = b;
    }
    auto find_root = [&](size_t x) {
      while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
      }
      return x;
    };
    auto unite = [&](size_t a, size_t b) {
      const size_t ra = find_root(a);
      const size_t rb = find_root(b);
      if (ra != rb) {
        if (ra < rb) {
          parent[rb] = ra;
        } else {
          parent[ra] = rb;
        }
      }
    };

    for (size_t u = 0; u < num_blocks; ++u) {
      if (reach_in[u].empty()) {
        continue;
      }
      const bool needs_join = blk_info[u].upward_use_regs.contains(r) ||
                              blk_info[u].live_in_regs.contains(r) ||
                              blk_info[u].upward_use_rmw_regs.contains(r);
      if (!needs_join) {
        continue;
      }
      std::vector<size_t> defs(reach_in[u].begin(), reach_in[u].end());
      for (size_t i = 0; i < defs.size(); ++i) {
        const RtlMachineMode mi = blk_info[defs[i]].gen_def_mode.at(r);
        for (size_t j = i + 1; j < defs.size(); ++j) {
          const RtlMachineMode mj = blk_info[defs[j]].gen_def_mode.at(r);
          if (modes_compatible(mi, mj)) {
            unite(defs[i], defs[j]);
          }
        }
        if (blk_info[u].upward_use_rmw_regs.contains(r) && blk_info[u].gen_def_mode.contains(r) &&
            modes_compatible(mi, blk_info[u].gen_def_mode.at(r))) {
          unite(defs[i], u);
        }
      }
    }

    absl::btree_map<size_t, absl::btree_set<size_t>> web_members;
    for (size_t b = 0; b < num_blocks; ++b) {
      if (blk_info[b].gen_def_mode.contains(r)) {
        web_members[find_root(b)].insert(b);
      }
    }

    absl::btree_set<size_t> call_fusage_reaching_defs;
    absl::btree_set<size_t> return_use_reaching_defs;
    if (IsAbiCallInterfaceHardReg(r)) {
      for (size_t u = 0; u < num_blocks; ++u) {
        if (blk_info[u].upward_call_fusage_use_regs.contains(r)) {
          call_fusage_reaching_defs.insert(reach_in[u].begin(), reach_in[u].end());
        }
        if (blk_info[u].upward_return_use_regs.contains(r)) {
          return_use_reaching_defs.insert(reach_in[u].begin(), reach_in[u].end());
        }
      }
      // `calls.c:emit_call_1` + `flow.c:2065`: count an exit-reaching def of
      // `r` as an unrenamed hard-reg def only when it actually reaches a
      // downstream `CALL_INSN_FUNCTION_USAGE` or `(use (reg/i ...))` return
      // read (not when killed earlier by an intervening block's def, where the
      // upstream def was a pseudo colored onto `$v0`/`$f0`).
      for (size_t b : call_fusage_reaching_defs) {
        if (blk_info[b].uncounted_exit_def_regs.contains(r)) {
          out->block_unrenamed_hard_defs[b][r] += 1;
          out->cfg_unrenamed_hard_defs[r] += 1;
        }
      }
      for (size_t b : return_use_reaching_defs) {
        if (!call_fusage_reaching_defs.contains(b) &&
            blk_info[b].uncounted_exit_def_regs.contains(r)) {
          out->block_unrenamed_hard_defs[b][r] += 1;
          out->cfg_unrenamed_hard_defs[r] += 1;
        }
      }
    }

    for (size_t b = 0; b < num_blocks; ++b) {
      if (const auto git = blk_info[b].gen_def_mode.find(r);
          git != blk_info[b].gen_def_mode.end()) {
        const auto& members = web_members.at(find_root(b));
        int other_cnt = static_cast<int>(members.size()) - (members.contains(b) ? 1 : 0);
        if (call_fusage_reaching_defs.contains(b)) {
          // `calls.c:emit_call_1` + `flow.c:2065`: an ABI call-arg hard reg
          // feeding a downstream `CALL_INSN_FUNCTION_USAGE` remains an unrenamed
          // hard register in `sched1`, so `reg_n_sets[r]` counts every explicit
          // pre-reload unrenamed hard-reg `SET` of `r` across other blocks as
          // well as any other defs joined into the same CFG reaching web.
          int own = 0;
          if (const auto bit = out->block_unrenamed_hard_defs[b].find(r);
              bit != out->block_unrenamed_hard_defs[b].end()) {
            own = bit->second;
          }
          const auto tit = out->cfg_unrenamed_hard_defs.find(r);
          const int total = (tit != out->cfg_unrenamed_hard_defs.end()) ? tit->second : 0;
          other_cnt = std::max(other_cnt, std::max(0, total - own));
        }
        if (other_cnt > 0) {
          out->block_exit_web_other_defs[b][{r, git->second}] = other_cnt;
        }
      }
      if (!reach_in[b].empty() &&
          (blk_info[b].upward_use_regs.contains(r) || blk_info[b].live_in_regs.contains(r))) {
        absl::btree_map<RtlMachineMode, absl::btree_set<size_t>> by_mode;
        const bool b_has_gen_def = blk_info[b].gen_def_mode.contains(r);
        const size_t b_root = b_has_gen_def ? find_root(b) : num_blocks;
        bool entry_exit_connected = false;
        for (size_t d : reach_in[b]) {
          const RtlMachineMode md = blk_info[d].gen_def_mode.at(r);
          const size_t d_root = find_root(d);
          if (b_has_gen_def && d_root == b_root) {
            entry_exit_connected = true;
          }
          for (size_t m_blk : web_members.at(d_root)) {
            if (m_blk != b) {
              by_mode[md].insert(m_blk);
            }
          }
        }
        if (entry_exit_connected) {
          out->block_entry_exit_same_web_regs[b].insert(r);
        }
        for (const auto& [md, defs_set] : by_mode) {
          if (!defs_set.empty()) {
            out->block_entry_web_other_defs[b][{r, md}] = static_cast<int>(defs_set.size());
          }
        }
      }
    }
  }
}

// Returns true when `root` references `$sp` (or `$fp` when used as the frame
// pointer) outside any `(mem ...)` address expression (e.g. taking the address
// of a local stack object `(set r (plus sp off))` or adjusting `$sp`).
bool PatternHasStackPointerEscape(const RtlExpr& root, bool frame_pointer_needed) {
  bool escaped = false;
  auto walk = [&](auto& self, const RtlExpr& node, bool inside_mem_addr) -> void {
    if (escaped) {
      return;
    }
    if (node.op == RtlOp::kUse || node.op == RtlOp::kClobber) {
      return;
    }
    if (node.op == RtlOp::kMem) {
      for (const RtlExpr& ch : node.children) {
        self(self, ch, /*inside_mem_addr=*/true);
      }
      return;
    }
    if (node.op == RtlOp::kReg) {
      if (!inside_mem_addr && !node.IsArgPointerReg()) {
        const int r = node.RegNo();
        if (r == kMipsStackPointerRegno || (frame_pointer_needed && r == kMipsFramePointerRegno)) {
          escaped = true;
        }
      }
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch, inside_mem_addr);
    }
  };
  walk(walk, root, /*inside_mem_addr=*/false);
  return escaped;
}

}  // namespace

CfgSched1Context ComputeCfgSched1Context(const RtlControlFlowGraph& cfg,
                                         const RtlControlFlowGraph& live_cfg,
                                         const MachineModel& mm,
                                         absl::Span<const int> extra_b0_defs) {
  const size_t num_blocks = cfg.rpo_blocks.size();
  CfgSched1Context out;
  out.block_total_defs.resize(num_blocks);
  out.block_epilogue_use_regs.resize(num_blocks);
  out.block_unrenamed_hard_defs.resize(num_blocks);
  out.block_reaching_defs.resize(num_blocks);
  out.block_reaching_defs_by_mode.resize(num_blocks);
  out.block_spill_store_counts.resize(num_blocks);
  out.block_spill_load_offsets.resize(num_blocks);
  std::vector<BlockWebEndpointSummary> web_blk_info(num_blocks);
  out.stack_layout.valid = true;
  out.stack_layout.has_stack_slot_access_info = true;
  bool any_call_in_cfg = false;
  int64_t max_call_arg_bytes = 0;
  std::vector<Insn> all_body_insns_for_spill_bounds;
  absl::btree_set<int> cfg_return_use_regs;
  absl::btree_set<int> cfg_call_fusage_regs;
  for (size_t b = 0; b < num_blocks; ++b) {
    const ParsedSchedRegion pre_reg = ParseBlockForSchedModel(cfg.rpo_blocks[b], b, Phase::kSched2);
    if (!pre_reg.valid) {
      continue;
    }
    for (const Insn& insn : pre_reg.sched_block) {
      if (insn.pn.pattern.op == RtlOp::kUse && !insn.pn.pattern.children.empty() &&
          insn.pn.pattern.children[0].op == RtlOp::kReg &&
          insn.pn.pattern.children[0].flags.integrated) {
        const int r = insn.pn.pattern.children[0].RegNo();
        const int nr = std::max(1, mm.HardRegNRegs(r, insn.pn.pattern.children[0].mode));
        for (int k = 0; k < nr && r + k < kMipsFirstPseudoRegister; ++k) {
          cfg_return_use_regs.insert(r + k);
        }
      }
      if (insn.kind == Insn::Kind::kCall) {
        absl::btree_set<int> fu_reads;
        for (const RtlExpr& fu : insn.pn.call_fusage) {
          CollectRegReads(fu, &fu_reads);
        }
        for (int r : fu_reads) {
          if (r >= 0 && r < kMipsFirstPseudoRegister) {
            cfg_call_fusage_regs.insert(r);
          }
        }
      }
    }
  }
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = cfg.rpo_blocks[b];
    const ParsedSchedRegion region = ParseBlockForSchedModel(blk, b, Phase::kSched2);
    if (!region.valid) {
      continue;
    }
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, b);
    const absl::btree_set<int> excluded_prologue =
        FindInterleavedPrologueSaveBodyIndices(region.sched_block, bounds, b, mm);
    for (size_t i = 0; i < region.sched_block.size(); ++i) {
      const Insn& insn = region.sched_block[i];
      const RtlExpr& pat = insn.pn.pattern;
      if (b == 0) {
        if (IsPrologueSpDecrementPattern(pat) && out.stack_layout.frame_size == 0 &&
            pat.SetSrc().children[1].op == RtlOp::kConstInt) {
          out.stack_layout.frame_size = (pat.SetSrc().op == RtlOp::kMinus)
                                            ? pat.SetSrc().children[1].int_val
                                            : -pat.SetSrc().children[1].int_val;
        }
        if (IsPrologueFpSetupPattern(pat)) {
          out.stack_layout.frame_pointer_needed = true;
        }
        const int ii = static_cast<int>(i);
        const bool is_prologue_region =
            (ii < bounds.prologue_end) ||
            (ii >= bounds.prologue_end && excluded_prologue.contains(ii - bounds.prologue_end));
        if (is_prologue_region && pat.op == RtlOp::kSet && pat.children.size() == 2 &&
            pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
            pat.SetSrc().op == RtlOp::kReg) {
          const int sr = pat.SetSrc().RegNo();
          const bool is_cs_save_reg =
              (sr == 31 || sr == 30 ||
               (sr >= 0 && sr < kMipsFirstPseudoRegister && !mm.CallUsed(sr)));
          int64_t save_off = 0;
          if (is_cs_save_reg &&
              TryExtractPlainStackSlotOffset(pat.SetDest().children[0], &save_off) &&
              save_off >= 0) {
            if (out.stack_layout.callee_saved_area_offset < 0 ||
                save_off < out.stack_layout.callee_saved_area_offset) {
              out.stack_layout.callee_saved_area_offset = save_off;
            }
          }
        }
      }
      if (insn.kind == Insn::Kind::kCall) {
        any_call_in_cfg = true;
        auto scan_call = [&](const RtlExpr& e) {
          if (e.op == RtlOp::kCall && e.children.size() >= 2 &&
              e.children[1].op == RtlOp::kConstInt) {
            max_call_arg_bytes = std::max(max_call_arg_bytes, e.children[1].int_val);
          } else if (e.op == RtlOp::kSet && e.children.size() == 2 &&
                     e.SetSrc().op == RtlOp::kCall && e.SetSrc().children.size() >= 2 &&
                     e.SetSrc().children[1].op == RtlOp::kConstInt) {
            max_call_arg_bytes = std::max(max_call_arg_bytes, e.SetSrc().children[1].int_val);
          }
        };
        if (pat.op == RtlOp::kParallel) {
          for (const RtlExpr& ch : pat.children) {
            scan_call(ch);
          }
        } else {
          scan_call(pat);
        }
      }
    }

    absl::btree_set<int> live_at_end;
    for (int succ : blk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end.insert(it->second.begin(), it->second.end());
      }
    }
    // Identify matched intra-block `caller-save.c:480-580` (`insert_save` /
    // `insert_restore`) restore loads `(set r (mem sp+off))` paired with a
    // pre-call store `(set (mem sp+off) r)` in block `b`. These post-reload
    // caller-save restores do not exist in `flow.c` / `sched1` (`sched.c:2490`)
    // and must not inflate `reg_n_sets` for loop-invariant pseudos live into `b`.
    absl::btree_set<int> caller_save_restore_indices;
    {
      absl::btree_map<StackSlotKey, std::vector<std::pair<int, int>>> blk_stores;
      std::vector<int> blk_calls;
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
        if (excluded_prologue.contains(i - bounds.prologue_end)) {
          continue;
        }
        const Insn& body_insn = region.sched_block[static_cast<size_t>(i)];
        if (body_insn.kind == Insn::Kind::kCall) {
          blk_calls.push_back(i);
          continue;
        }
        if (auto acc = TryParsePlainStackSlotMove(body_insn); acc.has_value()) {
          if (acc->is_store) {
            blk_stores[acc->key].push_back({i, acc->regno});
          } else if (const auto sit = blk_stores.find(acc->key); sit != blk_stores.end()) {
            for (const auto& [s_idx, s_reg] : sit->second) {
              if (s_idx < i && s_reg == acc->regno) {
                for (int c_idx : blk_calls) {
                  if (s_idx < c_idx && c_idx < i) {
                    caller_save_restore_indices.insert(i);
                    break;
                  }
                }
                if (caller_save_restore_indices.contains(i)) {
                  break;
                }
              }
            }
          }
        }
      }
    }

    absl::btree_map<int, RtlMachineMode> last_def_mode;
    BlockWebEndpointSummary& winfo = web_blk_info[b];
    if (const auto lit = live_cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
        lit != live_cfg.sideband.basic_block_live_at_start.end()) {
      for (int r : lit->second) {
        if (r >= 0 && r < kMipsFirstPseudoRegister) {
          winfo.live_in_regs.insert(r);
        }
      }
    }
    absl::btree_set<int> killed_so_far;
    // Identify ABI `call_fusage` argument defs inside block `b` (matching
    // `ProjectPostReloadBlockToSched1View`'s `abi_pinned_defs`).
    std::vector<absl::btree_set<int>> blk_call_arg_defs(region.sched_block.size());
    for (int ci = bounds.prologue_end; ci < bounds.epilogue_begin; ++ci) {
      if (region.sched_block[static_cast<size_t>(ci)].kind != Insn::Kind::kCall) {
        continue;
      }
      absl::btree_set<int> needed;
      for (const RtlExpr& fu : region.sched_block[static_cast<size_t>(ci)].pn.call_fusage) {
        CollectRegReads(fu, &needed);
      }
      for (int prev = ci - 1; prev >= bounds.prologue_end && !needed.empty(); --prev) {
        if (excluded_prologue.contains(prev - bounds.prologue_end)) {
          continue;
        }
        if (region.sched_block[static_cast<size_t>(prev)].kind == Insn::Kind::kCall) {
          break;
        }
        absl::btree_map<int, int> pdefs;
        absl::btree_map<int, RtlMachineMode> pmodes;
        CollectHardRegDefBaseRegnos(region.sched_block[static_cast<size_t>(prev)].pn.pattern,
                                    &pdefs, &pmodes);
        for (const auto& [r, cnt] : pdefs) {
          const int nr = std::max(1, mm.HardRegNRegs(r, pmodes[r]));
          bool hit = false;
          for (int k = 0; k < nr; ++k) {
            if (needed.erase(r + k) > 0) {
              hit = true;
            }
          }
          if (hit) {
            blk_call_arg_defs[static_cast<size_t>(prev)].insert(r);
          }
        }
      }
    }
    auto has_integrated_def = [](const RtlExpr& pat, int regno) {
      auto check = [&](const RtlExpr& e) {
        return e.op == RtlOp::kSet && e.children.size() == 2 && e.SetDest().op == RtlOp::kReg &&
               e.SetDest().RegNo() == regno && e.SetDest().flags.integrated;
      };
      if (pat.op == RtlOp::kParallel) {
        for (const RtlExpr& ch : pat.children) {
          if (check(ch)) {
            return true;
          }
        }
        return false;
      }
      return check(pat);
    };
    absl::btree_map<int, RtlMachineMode> active_def_mode;
    absl::btree_map<int, int> last_body_def_idx;
    absl::btree_set<std::pair<int, int>> counted_unrenamed_insn_reg;
    // Hard registers whose incoming live-in value already died via `REG_DEAD`
    // on an earlier instruction in this block (`flow.c:2337, 2505-2520`,
    // `mark_used_regs`); any later `SET` starts a new disjoint live range rather
    // than an RMW of the incoming web.
    absl::btree_set<int> incoming_dead_so_far;
    for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
      if (excluded_prologue.contains(i - bounds.prologue_end)) {
        continue;
      }
      const Insn& body_insn = region.sched_block[static_cast<size_t>(i)];
      const RtlExpr& pat = body_insn.pn.pattern;
      absl::btree_set<int> insn_reads;
      CollectRegReads(pat, &insn_reads);
      absl::btree_set<int> fusage_reads;
      for (const RtlExpr& fu : body_insn.pn.call_fusage) {
        CollectRegReads(fu, &fusage_reads);
      }
      for (int r : fusage_reads) {
        if (r >= 0 && r < kMipsFirstPseudoRegister && !killed_so_far.contains(r)) {
          winfo.upward_call_fusage_use_regs.insert(r);
          winfo.upward_use_regs.insert(r);
        }
      }
      for (int r : insn_reads) {
        if (r >= 0 && r < kMipsFirstPseudoRegister && !killed_so_far.contains(r)) {
          winfo.upward_use_regs.insert(r);
        }
      }
      if (body_insn.kind == Insn::Kind::kCall) {
        for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
          if (mm.CallUsed(r)) {
            killed_so_far.insert(r);
            active_def_mode.erase(r);
          }
        }
      }
      if (!caller_save_restore_indices.contains(i)) {
        absl::btree_map<int, int> insn_def_counts;
        absl::btree_map<int, RtlMachineMode> insn_def_modes;
        CollectHardRegDefBaseRegnos(pat, &insn_def_counts, &insn_def_modes);
        for (const auto& [r, cnt] : insn_def_counts) {
          out.block_total_defs[b][r] += cnt;
          last_body_def_idx[r] = i;
          bool read_later_in_block = false;
          for (int k = i + 1; k < bounds.epilogue_begin; ++k) {
            if (excluded_prologue.contains(k - bounds.prologue_end)) {
              continue;
            }
            const Insn& later = region.sched_block[static_cast<size_t>(k)];
            absl::btree_set<int> later_reads;
            CollectRegReads(later.pn.pattern, &later_reads);
            for (const RtlExpr& fu : later.pn.call_fusage) {
              CollectRegReads(fu, &later_reads);
            }
            if (later_reads.contains(r)) {
              read_later_in_block = true;
              break;
            }
            absl::btree_map<int, int> later_defs;
            absl::btree_map<int, RtlMachineMode> later_modes;
            CollectHardRegDefBaseRegnos(later.pn.pattern, &later_defs, &later_modes);
            if (later_defs.contains(r) || (later.kind == Insn::Kind::kCall && mm.CallUsed(r))) {
              break;
            }
          }
          const bool is_dead_unused_def = !read_later_in_block && !live_at_end.contains(r);
          const bool is_unrenamed_hard =
              (body_insn.kind == Insn::Kind::kCall) || has_integrated_def(pat, r) ||
              blk_call_arg_defs[static_cast<size_t>(i)].contains(r) || is_dead_unused_def ||
              (r >= kMipsMdRegFirst && r <= kMipsMdRegLast);
          const RtlMachineMode dmode = insn_def_modes[r];
          const int nw = std::max(1, mm.HardRegNRegs(r, dmode));
          if (is_unrenamed_hard) {
            for (int w = 0; w < nw && r + w < kMipsFirstPseudoRegister; ++w) {
              out.block_unrenamed_hard_defs[b][r + w] += cnt;
              counted_unrenamed_insn_reg.insert({i, r + w});
            }
          }
          last_def_mode[r] = dmode;
          if (winfo.upward_use_regs.contains(r) && !killed_so_far.contains(r) &&
              !incoming_dead_so_far.contains(r) &&
              !HasRegDeadNoteCovering(body_insn.pn.reg_notes, r, mm)) {
            winfo.upward_use_rmw_regs.insert(r);
          }
          for (int w = 0; w < nw && r + w < kMipsFirstPseudoRegister; ++w) {
            killed_so_far.insert(r + w);
            active_def_mode.erase(r + w);
          }
          active_def_mode[r] = dmode;
        }
      }
      for (const RtlRegNote& note : body_insn.pn.reg_notes) {
        if (note.kind != RtlMachineMode::kRegDead || !note.datum.IsReg()) {
          continue;
        }
        const int nr = note.datum.RegNo();
        if (nr < 0 || nr >= kMipsFirstPseudoRegister) {
          continue;
        }
        const int nw = std::max(1, mm.HardRegNRegs(nr, note.datum.mode));
        for (int w = 0; w < nw && nr + w < kMipsFirstPseudoRegister; ++w) {
          if (!killed_so_far.contains(nr + w)) {
            incoming_dead_so_far.insert(nr + w);
          }
        }
      }
      if (body_insn.kind != Insn::Kind::kCall &&
          PatternHasStackPointerEscape(pat, out.stack_layout.frame_pointer_needed)) {
        out.has_stack_addr_escape = true;
      }
      if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
        int64_t slot_off = 0;
        if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kMem &&
            !pat.SetSrc().children.empty() && !pat.SetSrc().flags.volatil &&
            !pat.SetSrc().flags.in_struct && !pat.SetSrc().flags.unchanging &&
            TryExtractPlainStackSlotOffset(pat.SetSrc().children[0], &slot_off)) {
          out.stack_layout.stack_slot_load_offsets.insert(slot_off);
          out.block_spill_load_offsets[b].insert(slot_off);
        } else if (pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
                   !pat.SetDest().flags.volatil && !pat.SetDest().flags.in_struct &&
                   !pat.SetDest().flags.unchanging && pat.SetSrc().op == RtlOp::kReg &&
                   TryExtractPlainStackSlotOffset(pat.SetDest().children[0], &slot_off)) {
          out.stack_layout.stack_slot_store_offsets.insert(slot_off);
          out.block_spill_store_counts[b][slot_off] += 1;
          out.cfg_spill_store_counts[slot_off] += 1;
        }
      }
      // `reload1.c` inherited spill slots may have no explicit reload `(set reg (mem sp+off))`
      // when every reader inherits the reload register directly, leaving only
      // `(REG_DEAD (mem sp+off))` on the final consumer (`reload1.c:6134-6140`).
      for (const RtlRegNote& note : body_insn.pn.reg_notes) {
        int64_t dead_off = 0;
        if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem &&
            !note.datum.children.empty() && !note.datum.flags.volatil &&
            !note.datum.flags.in_struct && !note.datum.flags.unchanging &&
            TryExtractPlainStackSlotOffset(note.datum.children[0], &dead_off)) {
          out.stack_layout.stack_slot_load_offsets.insert(dead_off);
          out.block_spill_load_offsets[b].insert(dead_off);
        }
      }
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg &&
          pat.SetSrc().op == RtlOp::kReg) {
        const int d = pat.SetDest().RegNo();
        const int s = pat.SetSrc().RegNo();
        // `global.c:1010-1014` and `local-alloc.c:2148` only apply hard-reg copy
        // preferences within compatible register classes (`GR_REGS` vs `FP_REGS`);
        // cross-bank GPR<->FPR moves (including `reload1.c` `mtc1`/`mfc1` reloads)
        // never create copy-tie coalescing between disjoint register files.
        const bool same_bank = (d < kMipsFpRegFirst && s < kMipsFpRegFirst) ||
                               (d >= kMipsFpRegFirst && d <= kMipsFpRegLast &&
                                s >= kMipsFpRegFirst && s <= kMipsFpRegLast);
        if (same_bank && d >= 0 && d < kMipsFirstPseudoRegister && s >= 0 &&
            s < kMipsFirstPseudoRegister && d != s) {
          const int dn = mm.HardRegNRegs(d, pat.SetDest().mode);
          const int sn = mm.HardRegNRegs(s, pat.SetSrc().mode);
          for (int kd = 0; kd < dn; ++kd) {
            for (int ks = 0; ks < sn; ++ks) {
              const int rd = d + kd;
              const int rs = s + ks;
              if (rd != rs) {
                out.cfg_copy_pairs.insert({std::min(rd, rs), std::max(rd, rs)});
              }
            }
          }
        }
      }
    }
    // Identify dedicated `caller-save.c:248-340, 575-630` save-area slots
    // (`s_reg == l_reg`, `CallUsed(s_reg)`, `saw_call`, `!redef_before_call`):
    // `insert_save`/`insert_restore` emit around the `CALL_INSN`, never
    // contiguously with the defining/consuming insn (`reload1.c:emit_reload_insns`).
    if (bounds.epilogue_begin > bounds.prologue_end) {
      const size_t blen = static_cast<size_t>(bounds.epilogue_begin - bounds.prologue_end);
      std::vector<bool> cs_overhead(blen, false);
      for (int rel : excluded_prologue) {
        if (rel >= 0 && static_cast<size_t>(rel) < blen) {
          cs_overhead[static_cast<size_t>(rel)] = true;
        }
      }
      const absl::Span<const Insn> body_span =
          absl::MakeConstSpan(region.sched_block).subspan(bounds.prologue_end, blen);
      const IntraBlockSpillSlotClassification cs_slots = ClassifyIntraBlockSpillAndCallerSaveSlots(
          body_span, mm, /*has_stack_addr_escape=*/true, &cs_overhead);
      for (int s_rel : cs_slots.caller_save_store_indices) {
        if (auto acc = TryParsePlainStackSlotMove(body_span[static_cast<size_t>(s_rel)]);
            acc.has_value() && mm.CallUsed(acc->regno)) {
          out.stack_layout.caller_save_slot_offsets.insert(acc->key.offset);
        }
      }
    }
    // Record upward-exposed reads from trailing `(use ...)` return markers and
    // any un-unpacked `epilogue_delay_pattern` on exit blocks.
    for (int i = bounds.epilogue_begin; i < static_cast<int>(region.sched_block.size()); ++i) {
      const RtlExpr& pat = region.sched_block[static_cast<size_t>(i)].pn.pattern;
      if (pat.op == RtlOp::kUse) {
        absl::btree_set<int> use_reads;
        CollectRegReads(pat, &use_reads);
        for (int r : use_reads) {
          if (r >= 0 && r < kMipsFirstPseudoRegister) {
            out.block_epilogue_use_regs[b].insert(r);
            if (!killed_so_far.contains(r)) {
              winfo.upward_return_use_regs.insert(r);
              winfo.upward_use_regs.insert(r);
            }
          }
        }
      }
    }
    if (blk.successors.empty() && cfg.sideband.epilogue_delay_pattern.has_value() &&
        !cfg.sideband.epilogue_delay_pattern->empty()) {
      if (auto ep = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern); ep.ok()) {
        absl::btree_set<int> ep_reads;
        CollectRegReads(ep->pattern, &ep_reads);
        for (int r : ep_reads) {
          if (r >= 0 && r < kMipsFirstPseudoRegister && !killed_so_far.contains(r)) {
            winfo.upward_use_regs.insert(r);
          }
        }
      }
    }
    if (b == 0) {
      for (int hr : extra_b0_defs) {
        if (hr >= 0 && hr < kMipsFirstPseudoRegister && !out.block_total_defs[0].contains(hr)) {
          out.block_total_defs[0][hr] += 1;
          out.block_unrenamed_hard_defs[0][hr] += 1;
          if (!active_def_mode.contains(hr)) {
            active_def_mode[hr] = RtlMachineMode::kVoid;
            killed_so_far.insert(hr);
          }
        }
      }
    }
    winfo.kill_regs = std::move(killed_so_far);
    for (const auto& [r, cnt] : out.block_total_defs[b]) {
      out.cfg_total_defs[r] += cnt;
      if (live_at_end.contains(r)) {
        out.block_reaching_defs[b].insert(r);
        out.cfg_reaching_defs[r] += 1;
        if (last_body_def_idx.contains(r) &&
            !counted_unrenamed_insn_reg.contains({last_body_def_idx[r], r})) {
          if (cfg_return_use_regs.contains(r) || cfg_call_fusage_regs.contains(r)) {
            winfo.uncounted_exit_def_regs.insert(r);
          }
        }
        const auto mit = last_def_mode.find(r);
        const RtlMachineMode def_mode =
            (mit != last_def_mode.end()) ? mit->second : RtlMachineMode::kVoid;
        out.block_reaching_defs_by_mode[b].insert({r, def_mode});
        out.cfg_reaching_defs_by_mode[{r, def_mode}] += 1;
      }
    }
    for (const auto& [r, cnt] : out.block_unrenamed_hard_defs[b]) {
      out.cfg_unrenamed_hard_defs[r] += cnt;
    }
    for (const auto& [r, md] : active_def_mode) {
      if (live_at_end.contains(r)) {
        winfo.gen_def_mode[r] = md;
      }
    }
    for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
      if (!excluded_prologue.contains(i - bounds.prologue_end)) {
        all_body_insns_for_spill_bounds.push_back(region.sched_block[static_cast<size_t>(i)]);
      }
    }
    // Track non-call-used hard registers live across any `CALL_INSN` in `b`
    // (`global.c:927`, `local-alloc.c:2106`).
    {
      absl::btree_set<int> cur_live = live_at_end;
      for (int i = bounds.epilogue_begin - 1; i >= bounds.prologue_end; --i) {
        if (excluded_prologue.contains(i - bounds.prologue_end)) {
          continue;
        }
        const Insn& insn = region.sched_block[static_cast<size_t>(i)];
        absl::btree_set<int> fdefs;
        absl::btree_set<int> pdefs;
        CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
        for (int d : fdefs) {
          cur_live.erase(d);
        }
        if (insn.kind == Insn::Kind::kCall) {
          for (auto it = cur_live.begin(); it != cur_live.end();) {
            if (mm.CallUsed(*it)) {
              it = cur_live.erase(it);
            } else {
              out.stack_layout.cfg_call_crossing_regs.insert(*it);
              ++it;
            }
          }
        }
        absl::btree_set<int> rds;
        CollectRegReads(insn.pn.pattern, &rds);
        for (const RtlExpr& fu : insn.pn.call_fusage) {
          CollectRegReads(fu, &rds);
        }
        for (int r : rds) {
          if (r >= 0 && r < kMipsFirstPseudoRegister) {
            cur_live.insert(r);
          }
        }
      }
    }
    for (int64_t off : out.block_spill_load_offsets[b]) {
      out.cfg_spill_load_block_counts[off] += 1;
    }
  }
  ComputeExactCfgWebDefCounts(cfg, web_blk_info, &out);
  if (any_call_in_cfg) {
    // `config/mips/mips.c:4465`, `mips.h:1649`: outgoing arg area is at least
    // `4 * UNITS_PER_WORD` (16 bytes for `$a0-$a3`) and 8-byte aligned.
    const int64_t min_args = std::max<int64_t>(16, max_call_arg_bytes);
    out.stack_layout.starting_frame_offset = (min_args + 7) & ~int64_t{7};
  }
  out.stack_layout.min_valid_reload_spill_offset = ComputeMinValidReloadSpillOffset(
      all_body_insns_for_spill_bounds, out.stack_layout, mm, out.stack_layout.frame_pointer_needed);
  // Mirror `FoldSpilledConstDoubleReloads` (`invert_reload.cc`): mark self-loop
  // blocks with a unique non-self predecessor (`reload1.c:517-524, 566-573`).
  out.block_has_self_loop_preheader.assign(cfg.rpo_blocks.size(), false);
  for (size_t b = 0; b < cfg.rpo_blocks.size(); ++b) {
    const auto& succs = cfg.rpo_blocks[b].successors;
    if (std::find(succs.begin(), succs.end(), static_cast<int>(b)) == succs.end()) {
      continue;
    }
    int pre_count = 0;
    for (size_t p = 0; p < cfg.rpo_blocks.size(); ++p) {
      if (p == b) {
        continue;
      }
      const auto& psuccs = cfg.rpo_blocks[p].successors;
      if (std::find(psuccs.begin(), psuccs.end(), static_cast<int>(b)) != psuccs.end()) {
        ++pre_count;
      }
    }
    if (pre_count == 1) {
      out.block_has_self_loop_preheader[b] = true;
    }
  }
  return out;
}

Sched1ProjectionContext BuildSched1ProjectionContextForBlock(size_t block_idx,
                                                             const CfgSched1Context& ctx) {
  Sched1ProjectionContext proj;
  proj.has_cfg_context = true;
  proj.is_entry_block = (block_idx == 0);
  for (const auto& [r, total] : ctx.cfg_unrenamed_hard_defs) {
    int own = 0;
    if (const auto it = ctx.block_unrenamed_hard_defs[block_idx].find(r);
        it != ctx.block_unrenamed_hard_defs[block_idx].end()) {
      own = it->second;
    }
    if (total > own) {
      proj.other_block_total_defs[r] = total - own;
    }
  }
  for (const auto& [r, reach] : ctx.cfg_reaching_defs) {
    const int own = ctx.block_reaching_defs[block_idx].contains(r) ? 1 : 0;
    if (reach > own) {
      proj.cross_block_reaching_defs[r] = reach - own;
    }
  }
  if (block_idx < ctx.block_reaching_defs_by_mode.size()) {
    proj.has_reaching_defs_by_mode = true;
    for (const auto& [key, reach] : ctx.cfg_reaching_defs_by_mode) {
      const int own = ctx.block_reaching_defs_by_mode[block_idx].contains(key) ? 1 : 0;
      if (reach > own) {
        proj.cross_block_reaching_defs_by_mode[key] = reach - own;
      }
    }
  }
  if (block_idx < ctx.block_entry_web_other_defs.size() &&
      block_idx < ctx.block_exit_web_other_defs.size()) {
    proj.has_per_endpoint_web_defs = true;
    proj.entry_web_other_defs = ctx.block_entry_web_other_defs[block_idx];
    proj.exit_web_other_defs = ctx.block_exit_web_other_defs[block_idx];
    if (block_idx < ctx.block_entry_exit_same_web_regs.size()) {
      proj.entry_exit_same_web_regs = ctx.block_entry_exit_same_web_regs[block_idx];
    }
  }
  if (block_idx < ctx.block_epilogue_use_regs.size()) {
    proj.epilogue_use_regs = ctx.block_epilogue_use_regs[block_idx];
  }
  proj.stack_layout = ctx.stack_layout;
  proj.has_stack_addr_escape = ctx.has_stack_addr_escape;
  if (block_idx < ctx.block_has_self_loop_preheader.size()) {
    proj.has_self_loop_preheader = ctx.block_has_self_loop_preheader[block_idx];
  }
  if (block_idx < ctx.block_spill_store_counts.size()) {
    for (const auto& [off, total] : ctx.cfg_spill_store_counts) {
      int own = 0;
      if (const auto it = ctx.block_spill_store_counts[block_idx].find(off);
          it != ctx.block_spill_store_counts[block_idx].end()) {
        own = it->second;
      }
      if (total > own) {
        proj.other_block_spill_store_counts[off] = total - own;
      }
    }
  }
  if (block_idx < ctx.block_spill_load_offsets.size()) {
    for (const auto& [off, blk_cnt] : ctx.cfg_spill_load_block_counts) {
      const int own = ctx.block_spill_load_offsets[block_idx].contains(off) ? 1 : 0;
      if (blk_cnt > own) {
        proj.other_block_spill_load_offsets.insert(off);
      }
    }
  }
  return proj;
}

namespace {

struct InsnRegReadDefDeaths {
  absl::btree_set<int> reads;
  absl::btree_set<int> defs;
  absl::btree_set<int> deaths;
  bool is_call_or_use = false;
};

InsnRegReadDefDeaths CollectInsnRegReadDefDeaths(const Insn& insn, const MachineModel& mm) {
  InsnRegReadDefDeaths out;
  out.is_call_or_use = (insn.kind == Insn::Kind::kCall || insn.pn.pattern.op == RtlOp::kUse);
  auto insert_reg = [&](int r, RtlMachineMode mode, absl::btree_set<int>* dst) {
    if (r < 0) {
      return;
    }
    const int nr = (r < kMipsFirstPseudoRegister) ? mm.HardRegNRegs(r, mode) : 1;
    for (int k = 0; k < nr; ++k) {
      dst->insert(r + k);
    }
  };
  auto walk_reads = [&](auto& self, const RtlExpr& node) -> void {
    if (node.op == RtlOp::kReg) {
      insert_reg(node.RegNo(), node.mode, &out.reads);
      return;
    }
    if (node.op == RtlOp::kSet && node.children.size() == 2) {
      if (node.SetDest().op == RtlOp::kMem && !node.SetDest().children.empty()) {
        self(self, node.SetDest().children[0]);
      } else if (node.SetDest().op != RtlOp::kReg) {
        self(self, node.SetDest());
      }
      self(self, node.SetSrc());
      return;
    }
    if (node.op == RtlOp::kClobber) {
      return;
    }
    for (const RtlExpr& ch : node.children) {
      self(self, ch);
    }
  };
  auto walk_defs = [&](const RtlExpr& pat) {
    auto handle = [&](const RtlExpr& elem) {
      if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
        const RtlExpr& d = elem.SetDest();
        if (d.op == RtlOp::kReg) {
          insert_reg(d.RegNo(), d.mode, &out.defs);
        } else if (d.op != RtlOp::kMem) {
          const RtlExpr* cur = &d;
          while ((cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
                  cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) &&
                 !cur->children.empty()) {
            cur = &cur->children[0];
          }
          if (cur->op == RtlOp::kReg) {
            insert_reg(cur->RegNo(), cur->mode, &out.defs);
          }
        }
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty()) {
        const RtlExpr* cur = &elem.children[0];
        while (cur->op == RtlOp::kSubreg && !cur->children.empty()) {
          cur = &cur->children[0];
        }
        if (cur->op == RtlOp::kReg) {
          insert_reg(cur->RegNo(), cur->mode, &out.defs);
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : pat.children) {
        handle(ch);
      }
    } else {
      handle(pat);
    }
  };
  walk_reads(walk_reads, insn.pn.pattern);
  for (const RtlExpr& fu : insn.pn.call_fusage) {
    walk_reads(walk_reads, fu);
    walk_defs(fu);
  }
  walk_defs(insn.pn.pattern);
  if (insn.kind == Insn::Kind::kCall) {
    for (int hr = 0; hr < kMipsFirstPseudoRegister; ++hr) {
      if (mm.CallUsed(hr)) {
        out.defs.insert(hr);
      }
    }
  }
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
      insert_reg(note.datum.RegNo(), note.datum.mode, &out.deaths);
    }
  }
  return out;
}

bool IsReturnJumpPatternForAttribution(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  return pat.op == RtlOp::kParallel && !pat.children.empty() &&
         pat.children[0].op == RtlOp::kReturn;
}

bool IsSimpleNonControlDelayInsn(const RtlExpr& pat) {
  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kCodeLabel || pat.op == RtlOp::kBarrier ||
      pat.op == RtlOp::kReturn || pat.op == RtlOp::kSequence) {
    return false;
  }
  if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kPc) {
    return false;
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
    const RtlExpr& c0 = pat.children[0];
    if (c0.op == RtlOp::kReturn ||
        (c0.op == RtlOp::kSet && c0.children.size() == 2 && c0.SetDest().op == RtlOp::kPc)) {
      return false;
    }
  }
  return true;
}

bool IsMdOutputReloadInsn(const Insn& insn) {
  const RtlExpr& pat = insn.pn.pattern;
  return pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetSrc().op == RtlOp::kReg &&
         (pat.SetSrc().RegNo() == kMipsMdRegFirst || pat.SetSrc().RegNo() == kMipsMdRegFirst + 1);
}

}  // namespace

bool IsAdjacentMdOutputReloadPair(const Insn& first_insn, const Insn& second_insn) {
  // Preserve `reload1.c:emit_reload_insns` `mfhi`/`mflo` followed immediately by
  // its consumer carrying `(REG_DEAD hi|lo)` so `InvertReload` never loses the
  // `hi`/`lo` death attribution on the consumer.
  if (IsMdOutputReloadInsn(first_insn)) {
    const int md_reg = first_insn.pn.pattern.SetSrc().RegNo();
    for (const RtlRegNote& note : second_insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
          note.datum.RegNo() == md_reg) {
        return true;
      }
    }
  }
  if (!IsMdOutputReloadInsn(second_insn)) {
    return false;
  }
  const int md_reg = second_insn.pn.pattern.SetSrc().RegNo();
  if (IsMdOutputReloadInsn(first_insn) && first_insn.pn.pattern.SetSrc().RegNo() != md_reg) {
    return true;
  }
  auto sets_md_reg = [md_reg](const RtlExpr& elem) -> bool {
    if (elem.op != RtlOp::kSet || elem.children.size() != 2 || elem.SetDest().op != RtlOp::kReg) {
      return false;
    }
    const int d = elem.SetDest().RegNo();
    if (d == md_reg) {
      return true;
    }
    return d == kMipsMdRegFirst && elem.SetDest().mode == RtlMachineMode::kDi &&
           md_reg == kMipsMdRegFirst + 1;
  };
  const RtlExpr& p1 = first_insn.pn.pattern;
  if (p1.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : p1.children) {
      if (sets_md_reg(ch)) {
        return true;
      }
    }
    return false;
  }
  return sets_md_reg(p1);
}

namespace {

bool BuildPreferredSched1OrderInBase(absl::Span<const Insn> base_block, int prologue_end,
                                     int epilogue_begin, const Sched1ProjectionView& base_view,
                                     const absl::btree_set<int>& excluded_prologue_body_indices,
                                     const MachineModel& mm, std::vector<int>* preferred_out,
                                     absl::Span<const int> extra_pinned_body_indices = {},
                                     absl::Span<const int> explicit_s1_order = {}) {
  const int n = static_cast<int>(base_block.size());
  const int body_len = epilogue_begin - prologue_end;
  const int num_core = static_cast<int>(base_view.core_insns.size());
  std::vector<int> s1_order;
  if (!explicit_s1_order.empty()) {
    if (static_cast<int>(explicit_s1_order.size()) != num_core) {
      return false;
    }
    s1_order.assign(explicit_s1_order.begin(), explicit_s1_order.end());
  } else {
    s1_order = ScheduleSched1ViewCoreOrder(base_view, mm);
    if (static_cast<int>(s1_order.size()) != num_core) {
      return false;
    }
  }
  std::vector<int> body_to_core(static_cast<size_t>(body_len), -1);
  for (int ci = 0; ci < num_core; ++ci) {
    body_to_core[static_cast<size_t>(base_view.core_to_orig_index[static_cast<size_t>(ci)])] = ci;
  }
  absl::btree_set<int> pinned_prefix_set(base_view.pinned_prefix_orig_indices.begin(),
                                         base_view.pinned_prefix_orig_indices.end());
  absl::btree_set<int> extra_pinned_core_set;
  std::vector<int> extra_pinned_cores;
  for (int rel : extra_pinned_body_indices) {
    if (rel >= 0 && rel < body_len) {
      const int ci = body_to_core[static_cast<size_t>(rel)];
      if (ci >= 0 && extra_pinned_core_set.insert(ci).second) {
        extra_pinned_cores.push_back(ci);
      }
    }
  }
  std::vector<int> interleaved_prologue_bis;
  std::vector<int> pinned_prefix_bis;
  std::vector<std::vector<int>> before_core(static_cast<size_t>(num_core));
  std::vector<std::vector<int>> after_core(static_cast<size_t>(num_core));
  std::vector<int> after_last_core;
  for (int rel = 0; rel < body_len; ++rel) {
    if (body_to_core[static_cast<size_t>(rel)] >= 0) {
      continue;
    }
    const int bi = prologue_end + rel;
    if (excluded_prologue_body_indices.contains(rel)) {
      interleaved_prologue_bis.push_back(bi);
      continue;
    }
    if (pinned_prefix_set.contains(rel)) {
      pinned_prefix_bis.push_back(bi);
      continue;
    }
    if (const auto ait = base_view.output_reload_after_core.find(rel);
        ait != base_view.output_reload_after_core.end() && ait->second >= 0 &&
        ait->second < num_core) {
      after_core[static_cast<size_t>(ait->second)].push_back(bi);
      continue;
    }
    if (const auto bit = base_view.input_reload_before_core.find(rel);
        bit != base_view.input_reload_before_core.end() && bit->second >= 0 &&
        bit->second < num_core) {
      before_core[static_cast<size_t>(bit->second)].push_back(bi);
      continue;
    }
    int next_core = -1;
    for (int k = rel + 1; k < body_len; ++k) {
      if (body_to_core[static_cast<size_t>(k)] >= 0) {
        next_core = body_to_core[static_cast<size_t>(k)];
        break;
      }
    }
    if (next_core >= 0) {
      before_core[static_cast<size_t>(next_core)].push_back(bi);
    } else {
      after_last_core.push_back(bi);
    }
  }
  preferred_out->clear();
  preferred_out->reserve(static_cast<size_t>(n));
  for (int i = 0; i < prologue_end; ++i) {
    preferred_out->push_back(i);
  }
  for (int pbi : interleaved_prologue_bis) {
    preferred_out->push_back(pbi);
  }
  for (int ppbi : pinned_prefix_bis) {
    preferred_out->push_back(ppbi);
  }
  for (int ci : extra_pinned_cores) {
    for (int aux_bi : before_core[static_cast<size_t>(ci)]) {
      preferred_out->push_back(aux_bi);
    }
    preferred_out->push_back(prologue_end + base_view.core_to_orig_index[static_cast<size_t>(ci)]);
    for (int aux_bi : after_core[static_cast<size_t>(ci)]) {
      preferred_out->push_back(aux_bi);
    }
  }
  for (int ci : s1_order) {
    if (extra_pinned_core_set.contains(ci)) {
      continue;
    }
    for (int aux_bi : before_core[static_cast<size_t>(ci)]) {
      preferred_out->push_back(aux_bi);
    }
    preferred_out->push_back(prologue_end + base_view.core_to_orig_index[static_cast<size_t>(ci)]);
    for (int aux_bi : after_core[static_cast<size_t>(ci)]) {
      preferred_out->push_back(aux_bi);
    }
  }
  for (int aux_bi : after_last_core) {
    preferred_out->push_back(aux_bi);
  }
  for (int i = epilogue_begin; i < n; ++i) {
    preferred_out->push_back(i);
  }
  return static_cast<int>(preferred_out->size()) == n;
}

// Mirrors `reload1.c:2185-2250, 2675-2720`: `eliminate_regs` only rewrites
// `REG_NOTES` on insns whose pattern triggers register elimination. When a
// store-equiv initializer's pattern has no stack-pointer/frame-pointer operand,
// its `REG_EQUIV (mem ...)` note retains its pre-elimination `$fp`/`argp` base.
RtlExpr UneliminateMemForRegEquivNoteIfNeeded(const RtlExpr& mem, const RtlExpr& init_pat,
                                              const PostReloadStackLayout& layout) {
  if (!layout.valid) {
    return mem;
  }
  const int elim_to_reg =
      layout.frame_pointer_needed ? kMipsFramePointerRegno : kMipsStackPointerRegno;
  bool pat_triggers_elim = false;
  init_pat.Visit([&](const RtlExpr& node) {
    if (pat_triggers_elim) {
      return;
    }
    if (node.op == RtlOp::kReg &&
        (node.IsArgPointerReg() ||
         (!node.IsArgPointerReg() &&
          (node.RegNo() == elim_to_reg || node.RegNo() == kMipsFramePointerRegno)))) {
      pat_triggers_elim = true;
    }
  });
  if (pat_triggers_elim) {
    return mem;
  }
  return NormalizeMemForRegEquivMatch(mem, layout);
}

bool SameAddrIgnoringRegNames(const RtlExpr& a, const RtlExpr& b) {
  if (a.op != b.op || a.mode != b.mode || a.children.size() != b.children.size()) {
    return false;
  }
  if (a.op == RtlOp::kReg) {
    return a.IsArgPointerReg() == b.IsArgPointerReg() &&
           (a.IsArgPointerReg() || a.RegNo() == b.RegNo());
  }
  if (a.int_val != b.int_val || a.int_val_high != b.int_val_high || a.str_val != b.str_val) {
    return false;
  }
  for (size_t i = 0; i < a.children.size(); ++i) {
    if (!SameAddrIgnoringRegNames(a.children[i], b.children[i])) {
      return false;
    }
  }
  return true;
}

bool SameMemSlotIgnoringRegNames(const RtlExpr& a, const RtlExpr& b,
                                 const PostReloadStackLayout& layout) {
  if (a.op != RtlOp::kMem || b.op != RtlOp::kMem || a.mode != b.mode || a.children.empty() ||
      b.children.empty()) {
    return false;
  }
  if (SameAddrIgnoringRegNames(a.children[0], b.children[0])) {
    return true;
  }
  const RtlExpr na = NormalizeMemForRegEquivMatch(a, layout);
  const RtlExpr nb = NormalizeMemForRegEquivMatch(b, layout);
  return !na.children.empty() && !nb.children.empty() &&
         SameAddrIgnoringRegNames(na.children[0], nb.children[0]);
}

bool HasMatchingMemRegEquivNote(const Insn& insn, const RtlExpr& target_mem,
                                const PostReloadStackLayout& layout) {
  for (const RtlRegNote& note : insn.pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegEquiv &&
        SameMemSlotIgnoringRegNames(note.datum, target_mem, layout)) {
      return true;
    }
  }
  return false;
}

// Locates the unique preceding single-set initializer of `target_reg` before
// `store_pos` in `seq`, mirroring `local-alloc.c:990-1027` and
// `ComputeHardRegLivenessSummary`.
int FindUniqueInitPosForStoreInSeq(absl::Span<const Insn> seq, int store_pos, int target_reg,
                                   const MachineModel& mm) {
  for (int j = store_pos - 1; j >= 0; --j) {
    const Insn& jinsn = seq[static_cast<size_t>(j)];
    if (jinsn.kind == Insn::Kind::kCall) {
      return -1;
    }
    const InsnRegReadDefDeaths jinfo = CollectInsnRegReadDefDeaths(jinsn, mm);
    if (!jinfo.defs.contains(target_reg)) {
      continue;
    }
    const bool is_multiset_rmw = jinfo.reads.contains(target_reg) &&
                                 !HasRegDeadNoteCovering(jinsn.pn.reg_notes, target_reg, mm);
    const RtlExpr& jpat = jinsn.pn.pattern;
    if (!is_multiset_rmw && jpat.op == RtlOp::kSet && jpat.children.size() == 2 &&
        jpat.SetDest().op == RtlOp::kReg && !jpat.SetDest().IsArgPointerReg() &&
        jpat.SetDest().RegNo() == target_reg) {
      return j;
    }
    return -1;
  }
  return -1;
}

// Mirrors `local-alloc.c:540-603, 1049-1055` (`validate_equiv_mem`,
// `update_equiv_regs`): returns true when reordering from post-`sched2`
// `base_block` to pre-`sched2` `cand_in_base` moves an invalidator `d` of
// `MEM` load `oid` (`dst_r = mem_src`) from outside `[oid, base_dst_death_pos)`
// into `[load_pos, cand_dst_death_pos)`:
//   - `(REG_DEAD base_r)` for a register mentioned in `mem_src` (`sched2` can
//     hoist `d` ahead of `oid` or hoist `REG_DEAD dst_r` ahead of `d`), or
//   - a base-register write, non-const `CALL_INSN` (when `!mem_src.flags.unchanging`),
//     or aliasing `SET`/`CLOBBER` `MEM` store `d` that lay after `dst_r`'s death
//     in `base_block` (`base_dst_death_pos >= 0 && d >= base_dst_death_pos`, i.e.
//     `sched2` hoisted `REG_DEAD dst_r` ahead of `d` without crossing `oid`).
bool HasLoadEquivInvalidatorMovedIntoWindow(
    int oid, const RtlExpr& mem_src, int dst_r, int prologue_end, int epilogue_begin,
    absl::Span<const Insn> base_block, absl::Span<const int> cand_in_base,
    absl::Span<const int> pos_in_cand, absl::Span<const InsnRegReadDefDeaths> insn_info,
    const RegFacts& facts_s2, const MachineModel& mm, const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_orig_ids, bool allow_moved_store_invalidator = false) {
  if (mem_src.op != RtlOp::kMem || mem_src.children.empty() || mem_src.flags.volatil) {
    return false;
  }
  const int load_pos = pos_in_cand[static_cast<size_t>(oid)];
  int cand_dst_death_pos = -1;
  for (int p = load_pos + 1; p < epilogue_begin; ++p) {
    const int oi = cand_in_base[static_cast<size_t>(p)];
    if (insn_info[static_cast<size_t>(oi)].deaths.contains(dst_r) ||
        insn_info[static_cast<size_t>(oi)].defs.contains(dst_r)) {
      cand_dst_death_pos = p;
      break;
    }
  }
  int base_dst_death_pos = -1;
  for (int b = oid + 1; b < epilogue_begin; ++b) {
    if (insn_info[static_cast<size_t>(b)].deaths.contains(dst_r) ||
        insn_info[static_cast<size_t>(b)].defs.contains(dst_r)) {
      base_dst_death_pos = b;
      break;
    }
  }
  absl::btree_set<int> addr_regs;
  mem_src.children[0].Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kMipsFirstPseudoRegister) {
      const int nr = std::max(1, mm.HardRegNRegs(node.RegNo(), node.mode));
      for (int k = 0; k < nr; ++k) {
        addr_regs.insert(node.RegNo() + k);
      }
    }
  });
  auto store_modifies_mem = [&](const RtlExpr& pat) -> bool {
    bool modified = false;
    auto check_part = [&](const RtlExpr& p) {
      if (modified) {
        return;
      }
      if ((p.op == RtlOp::kSet || p.op == RtlOp::kClobber) && !p.children.empty() &&
          p.children[0].op == RtlOp::kMem) {
        if (TrueDependence(p.children[0], mem_src, facts_s2, Phase::kSched2) ||
            PreReloadDistinctStackBasesConflict(p.children[0], mem_src, stack_layout)) {
          modified = true;
        }
      }
    };
    if (pat.op == RtlOp::kParallel) {
      for (const RtlExpr& sub : pat.children) {
        check_part(sub);
      }
    } else {
      check_part(pat);
    }
    return modified;
  };
  for (int d = prologue_end; d < epilogue_begin; ++d) {
    if (excluded_orig_ids.contains(d)) {
      continue;
    }
    const bool was_in_base_window =
        (d >= oid) && (base_dst_death_pos < 0 || d < base_dst_death_pos);
    if (was_in_base_window) {
      continue;
    }
    const int d_pos = pos_in_cand[static_cast<size_t>(d)];
    if (d_pos < load_pos || (cand_dst_death_pos >= 0 && d_pos >= cand_dst_death_pos)) {
      continue;
    }
    for (int dead_r : insn_info[static_cast<size_t>(d)].deaths) {
      if (addr_regs.contains(dead_r)) {
        return true;
      }
    }
    if (base_dst_death_pos >= 0 && d >= base_dst_death_pos) {
      for (int def_r : insn_info[static_cast<size_t>(d)].defs) {
        if (addr_regs.contains(def_r)) {
          return true;
        }
      }
      const Insn& d_insn = base_block[static_cast<size_t>(d)];
      if (!mem_src.flags.unchanging) {
        if (d_insn.kind == Insn::Kind::kCall && !d_insn.pn.pattern.flags.unchanging) {
          return true;
        }
        if (allow_moved_store_invalidator && store_modifies_mem(d_insn.pn.pattern)) {
          return true;
        }
      }
    }
  }
  return false;
}

// Mirrors `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`,
// `memref_used_between_p`, `update_equiv_regs`): returns true when reordering
// from post-`sched2` `base_block` to pre-`sched2` `cand_in_base` moves an
// instruction `d` that invalidates `mem_dst` (base-reg write/death, non-const
// call, or overlapping `MEM` access) from outside `[init_oid, store_oid)` into
// `[init_pos, store_pos)`.
bool HasStoreEquivInvalidatorMovedIntoWindow(
    int store_oid, int init_pos, const RtlExpr& mem_dst, int prologue_end, int epilogue_begin,
    absl::Span<const Insn> base_block, absl::Span<const int> cand_in_base,
    absl::Span<const int> pos_in_cand, absl::Span<const InsnRegReadDefDeaths> insn_info,
    const RegFacts& facts_s2, const MachineModel& mm, const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_orig_ids) {
  if (init_pos < 0 || mem_dst.op != RtlOp::kMem || mem_dst.children.empty() ||
      mem_dst.flags.volatil) {
    return false;
  }
  const int store_pos = pos_in_cand[static_cast<size_t>(store_oid)];
  const int init_oid = cand_in_base[static_cast<size_t>(init_pos)];
  if (store_pos <= init_pos) {
    return false;
  }
  absl::btree_set<int> addr_regs;
  mem_dst.children[0].Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kReg && node.RegNo() >= 0 && node.RegNo() < kMipsFirstPseudoRegister) {
      const int nr = std::max(1, mm.HardRegNRegs(node.RegNo(), node.mode));
      for (int k = 0; k < nr; ++k) {
        addr_regs.insert(node.RegNo() + k);
      }
    }
  });
  for (int d = prologue_end; d < epilogue_begin; ++d) {
    if (excluded_orig_ids.contains(d)) {
      continue;
    }
    const bool was_in_base_window = (init_oid < store_oid) && (d >= init_oid && d < store_oid);
    if (was_in_base_window) {
      continue;
    }
    const int d_pos = pos_in_cand[static_cast<size_t>(d)];
    if (d_pos < init_pos || d_pos >= store_pos) {
      continue;
    }
    for (int r : insn_info[static_cast<size_t>(d)].defs) {
      if (addr_regs.contains(r)) {
        return true;
      }
    }
    for (int r : insn_info[static_cast<size_t>(d)].deaths) {
      if (addr_regs.contains(r)) {
        return true;
      }
    }
    const Insn& d_insn = base_block[static_cast<size_t>(d)];
    if (d_insn.kind == Insn::Kind::kCall && !mem_dst.flags.unchanging) {
      return true;
    }
    bool mem_conflict = false;
    d_insn.pn.pattern.Visit([&](const RtlExpr& node) {
      if (!mem_conflict && node.op == RtlOp::kMem &&
          (TrueDependence(node, mem_dst, facts_s2, Phase::kSched2) ||
           TrueDependence(mem_dst, node, facts_s2, Phase::kSched2) ||
           AntiDependence(node, mem_dst, facts_s2, Phase::kSched2) ||
           PreReloadDistinctStackBasesConflict(node, mem_dst, stack_layout))) {
        mem_conflict = true;
      }
    });
    if (mem_conflict) {
      return true;
    }
  }
  return false;
}

// Mirrors `local-alloc.c:700-839, 1004-1007` (`optimize_reg_copy_1`) +
// `reload1.c:5186-5257` (`choose_reload_regs` reload inheritance): when
// `copy_insn` is a cross-class FP->GPR copy `(set (reg:SF/DF r_gpr)
// (reg:SF/DF r_fpr))` carrying `(REG_DEAD r_fpr)` and `read_insn` is a later
// FPR-only consumer reading `r_fpr` with both `r_gpr` and `r_fpr` unmodified
// (and no call) in between, `reload()` inherited `r_fpr` for `r_gpr` while
// leaving `(REG_DEAD r_fpr)` on `copy_insn` (matching `invert_reload.cc`
// Pass D).
bool IsFprOnlySchedRhsExpr(const RtlExpr& rhs) {
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

bool IsCrossClassFprToGprCopyReloadInheritedRead(absl::Span<const Insn> block,
                                                 absl::Span<const int> order_in_block, int copy_pos,
                                                 int read_pos, int r, const MachineModel& mm) {
  if (copy_pos < 0 || read_pos <= copy_pos || read_pos >= static_cast<int>(order_in_block.size())) {
    return false;
  }
  const Insn& copy_insn = block[static_cast<size_t>(order_in_block[static_cast<size_t>(copy_pos)])];
  const Insn& read_insn = block[static_cast<size_t>(order_in_block[static_cast<size_t>(read_pos)])];
  const RtlExpr* cs = SingleSetOf(copy_insn.pn.pattern);
  if (cs == nullptr || cs->SetDest().op != RtlOp::kReg || cs->SetSrc().op != RtlOp::kReg ||
      cs->SetDest().IsArgPointerReg() || cs->SetSrc().IsArgPointerReg() ||
      cs->SetDest().mode != cs->SetSrc().mode ||
      (cs->SetDest().mode != RtlMachineMode::kSF && cs->SetDest().mode != RtlMachineMode::kDF)) {
    return false;
  }
  const int r_gpr = cs->SetDest().RegNo();
  const int r_fpr = cs->SetSrc().RegNo();
  if (r_gpr < 0 || r_gpr >= kMipsFpRegFirst || r_fpr < kMipsFpRegFirst ||
      r_fpr >= kMipsFirstPseudoRegister) {
    return false;
  }
  const int fpr_nr = std::max(1, mm.HardRegNRegs(r_fpr, cs->SetSrc().mode));
  if (r < r_fpr || r >= r_fpr + fpr_nr) {
    return false;
  }
  const RtlExpr* rs = SingleSetOf(read_insn.pn.pattern);
  if (rs == nullptr || !IsFprOnlySchedRhsExpr(rs->SetSrc())) {
    return false;
  }
  const int gpr_nr = std::max(1, mm.HardRegNRegs(r_gpr, cs->SetDest().mode));
  for (int p = copy_pos + 1; p < read_pos; ++p) {
    const Insn& mid = block[static_cast<size_t>(order_in_block[static_cast<size_t>(p)])];
    if (mid.kind == Insn::Kind::kCall || InsnDefinesHardRegSpan(mid, r_gpr, gpr_nr, mm) ||
        InsnDefinesHardRegSpan(mid, r_fpr, fpr_nr, mm)) {
      return false;
    }
  }
  return true;
}

// Returns true when `cand_in_base` drops a per-live-range same-class
// hard-register interference `(lo, hi)` (`lo < hi`) on a block-local,
// non-call-crossing, call-used ordinary def of `hi` whose overlapping `lo`
// in `base_block` was also an ordinary non-reload def. Per
// `local-alloc.c:2158-2182` (`find_free_reg` ascending regno scan when
// `REG_ALLOC_ORDER` is undefined in `config/mips/mips.h`), exposing `lo` as
// free across `hi`'s entire live range forces `local-alloc` to assign `lo`
// instead of `hi` and Stage 4 to demote both to hard registers.
bool DropsPerLiveRangeLocalFindFreeRegInterference(
    absl::Span<const Insn> base_block, absl::Span<const int> cand_in_base, int prologue_end,
    int epilogue_begin, const absl::btree_set<int>& excluded_prologue_orig_ids,
    const absl::btree_set<int>& live_at_end_s2, const HardRegLivenessSummary& base_sum,
    const absl::btree_set<std::pair<int, int>>& cfg_copy_pairs,
    absl::Span<const InsnRegReadDefDeaths> insn_info, const PostReloadStackLayout& stack_layout,
    const MachineModel& mm) {
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return false;
  }
  auto same_reg_file = [](int a, int b) {
    return (a < kMipsFpRegFirst && b < kMipsFpRegFirst) ||
           (a >= kMipsFpRegFirst && a <= kMipsFpRegLast && b >= kMipsFpRegFirst &&
            b <= kMipsFpRegLast);
  };
  auto is_ordinary_local_def = [&](const Insn& insn, int rel, int r, RtlMachineMode* mode_out,
                                   const std::vector<absl::btree_set<int>>& live_after) {
    if (insn.kind != Insn::Kind::kInsn || IsMdOutputReloadInsn(insn) ||
        TryParsePlainStackSlotMove(insn, stack_layout.frame_pointer_needed).has_value()) {
      return false;
    }
    const RtlExpr* s = SingleSetOf(insn.pn.pattern);
    if (s == nullptr || s->SetDest().op != RtlOp::kReg || s->SetDest().RegNo() != r ||
        s->SetDest().flags.integrated || s->SetDest().flags.frame_related ||
        s->SetSrc().op == RtlOp::kReg) {
      return false;
    }
    // Exclude `reload1.c:517-524, 6583` rematerialized `const_double` reloads
    // (inserted post-`local-alloc` with empty `reg_notes` into FP spill regs
    // and NO `(REG_DEAD r)` on their last use), as well as `reload1.c:5641-6155`
    // spilled-output and cross-bank FPR->GPR (`mfc1`) output-reload producers
    // whose reload register `r` dies at the reload store/copy (`local-alloc`
    // never assigned `r` to those reload scratches).
    const bool is_unnoted_const_double =
        (s->SetSrc().op == RtlOp::kConstDouble && insn.pn.reg_notes.empty());
    const int nw = std::max(1, mm.HardRegNRegs(r, s->SetDest().mode));
    for (int j = rel + 1; j < body_len; ++j) {
      const int j_oid = prologue_end + j;
      if (excluded_prologue_orig_ids.contains(j_oid)) {
        continue;
      }
      const Insn& next = base_block[static_cast<size_t>(j_oid)];
      if (next.kind == Insn::Kind::kCall || next.kind == Insn::Kind::kJump) {
        break;
      }
      bool dead_after_j = true;
      for (int w = 0; w < nw; ++w) {
        if (live_after[static_cast<size_t>(j)].contains(r + w)) {
          dead_after_j = false;
          break;
        }
      }
      if (is_unnoted_const_double && dead_after_j) {
        bool reads_r = false;
        bool has_reg_dead_r = false;
        for (int w = 0; w < nw; ++w) {
          if (insn_info[static_cast<size_t>(j_oid)].reads.contains(r + w)) {
            reads_r = true;
          }
          if (insn_info[static_cast<size_t>(j_oid)].deaths.contains(r + w)) {
            has_reg_dead_r = true;
          }
        }
        if (reads_r && !has_reg_dead_r) {
          return false;
        }
      }
      if (auto sm = TryParsePlainStackSlotMove(next, stack_layout.frame_pointer_needed);
          sm.has_value() && sm->is_store && sm->regno == r && dead_after_j) {
        return false;
      }
      const RtlExpr* ns = SingleSetOf(next.pn.pattern);
      if (r >= kMipsFpRegFirst && ns != nullptr && ns->SetDest().op == RtlOp::kReg &&
          ns->SetSrc().op == RtlOp::kReg && ns->SetSrc().RegNo() == r &&
          ns->SetDest().RegNo() >= 2 && ns->SetDest().RegNo() < kMipsFpRegFirst && dead_after_j) {
        return false;
      }
      bool defs_r = false;
      for (int w = 0; w < nw; ++w) {
        if (insn_info[static_cast<size_t>(j_oid)].defs.contains(r + w)) {
          defs_r = true;
          break;
        }
      }
      if (defs_r || dead_after_j) {
        break;
      }
    }
    if (mode_out != nullptr) {
      *mode_out = s->SetDest().mode;
    }
    return true;
  };
  absl::btree_set<int> copy_tied_regs;
  for (const auto& [a, b] : cfg_copy_pairs) {
    copy_tied_regs.insert(a);
    copy_tied_regs.insert(b);
  }
  for (int rel = 0; rel < body_len; ++rel) {
    const int oid = prologue_end + rel;
    if (excluded_prologue_orig_ids.contains(oid)) {
      continue;
    }
    const RtlExpr* cs = SingleSetOf(base_block[static_cast<size_t>(oid)].pn.pattern);
    if (cs != nullptr && cs->SetDest().op == RtlOp::kReg && cs->SetSrc().op == RtlOp::kReg) {
      copy_tied_regs.insert(cs->SetDest().RegNo());
      copy_tied_regs.insert(cs->SetSrc().RegNo());
    }
  }
  auto compute_live_after = [&](absl::Span<const int> order) {
    std::vector<absl::btree_set<int>> live_after(static_cast<size_t>(body_len));
    absl::btree_set<int> live = live_at_end_s2;
    for (int rel = body_len - 1; rel >= 0; --rel) {
      const int oid = order[static_cast<size_t>(prologue_end + rel)];
      if (excluded_prologue_orig_ids.contains(oid)) {
        live_after[static_cast<size_t>(rel)] = live;
        continue;
      }
      live_after[static_cast<size_t>(rel)] = live;
      const Insn& insn = base_block[static_cast<size_t>(oid)];
      const auto& info = insn_info[static_cast<size_t>(oid)];
      if (insn.kind == Insn::Kind::kCall) {
        for (auto it = live.begin(); it != live.end();) {
          if (*it >= 0 && *it < kMipsFirstPseudoRegister && mm.CallUsed(*it)) {
            it = live.erase(it);
          } else {
            ++it;
          }
        }
      } else {
        for (int d : info.defs) {
          live.erase(d);
        }
      }
      for (int r : info.reads) {
        if (r >= 0 && r < kMipsFirstPseudoRegister) {
          live.insert(r);
        }
      }
    }
    return live_after;
  };
  std::vector<int> base_order(base_block.size());
  for (size_t i = 0; i < base_block.size(); ++i) {
    base_order[i] = static_cast<int>(i);
  }
  const std::vector<absl::btree_set<int>> base_live_after = compute_live_after(base_order);
  const std::vector<absl::btree_set<int>> cand_live_after = compute_live_after(cand_in_base);
  std::vector<int> orig_to_cand_rel(base_block.size(), -1);
  for (int rel = 0; rel < body_len; ++rel) {
    orig_to_cand_rel[static_cast<size_t>(cand_in_base[static_cast<size_t>(prologue_end + rel)])] =
        rel;
  }
  for (int base_rel = 0; base_rel < body_len; ++base_rel) {
    const int oid = prologue_end + base_rel;
    if (excluded_prologue_orig_ids.contains(oid)) {
      continue;
    }
    const Insn& def_insn = base_block[static_cast<size_t>(oid)];
    const RtlExpr* s = SingleSetOf(def_insn.pn.pattern);
    if (s == nullptr || s->SetDest().op != RtlOp::kReg) {
      continue;
    }
    const int hi = s->SetDest().RegNo();
    RtlMachineMode hi_mode = RtlMachineMode::kSi;
    // Exclude RMWs (`local-alloc.c:1266-1300` `combine_regs` continuations) so
    // `base_rel` is the true birth of a standalone local qty.
    if (hi < 2 || hi >= kMipsFirstPseudoRegister ||
        IsFixedOrSpecialHardReg(hi, stack_layout.frame_pointer_needed) || !mm.CallUsed(hi) ||
        IsAbiCallInterfaceHardReg(hi) || copy_tied_regs.contains(hi) ||
        insn_info[static_cast<size_t>(oid)].reads.contains(hi) ||
        !is_ordinary_local_def(def_insn, base_rel, hi, &hi_mode, base_live_after)) {
      continue;
    }
    if (hi >= kMipsFpRegFirst && (hi % 2) != 0) {
      continue;
    }
    const bool hi_needs_even = (mm.HardRegNRegs(hi, hi_mode) >= 2) || (hi >= kMipsFpRegFirst);
    if (!base_live_after[static_cast<size_t>(base_rel)].contains(hi)) {
      continue;
    }
    // Find the end of this standalone local live range of `hi` in `base_block`.
    int base_end_rel = -1;
    bool crosses_call = false;
    for (int k = base_rel + 1; k < body_len; ++k) {
      const int k_oid = prologue_end + k;
      if (base_block[static_cast<size_t>(k_oid)].kind == Insn::Kind::kCall) {
        crosses_call = true;
        break;
      }
      if (!base_live_after[static_cast<size_t>(k)].contains(hi)) {
        base_end_rel = k;
        break;
      }
    }
    if (base_end_rel <= base_rel + 1 || crosses_call) {
      continue;
    }
    // Exclude live ranges ending at an RMW of `hi` (where `combine_regs` ties
    // `hi` into a continuing qty past `base_end_rel`).
    if (insn_info[static_cast<size_t>(prologue_end + base_end_rel)].defs.contains(hi)) {
      continue;
    }
    // Collect straddling same-class `lo < hi` born strictly inside
    // `(base_rel, base_end_rel)` at an independent ordinary def that stays live
    // past `base_end_rel` (`local-alloc.c:2158-2182` `find_free_reg`).
    absl::btree_set<int> straddling_lo_regs;
    for (int k = base_rel + 1; k < base_end_rel; ++k) {
      const int k_oid = prologue_end + k;
      if (excluded_prologue_orig_ids.contains(k_oid)) {
        continue;
      }
      const Insn& lo_insn = base_block[static_cast<size_t>(k_oid)];
      const RtlExpr* ls = SingleSetOf(lo_insn.pn.pattern);
      if (ls == nullptr || ls->SetDest().op != RtlOp::kReg) {
        continue;
      }
      const int lo = ls->SetDest().RegNo();
      if (lo < 2 || lo >= hi || !same_reg_file(lo, hi) || !mm.CallUsed(lo) ||
          IsFixedOrSpecialHardReg(lo, stack_layout.frame_pointer_needed) ||
          base_sum.live_at_entry.contains(lo) || (hi_needs_even && (lo % 2) != 0) ||
          base_live_after[static_cast<size_t>(k - 1)].contains(lo) ||
          insn_info[static_cast<size_t>(k_oid)].reads.contains(lo) ||
          insn_info[static_cast<size_t>(k_oid)].reads.contains(hi) ||
          !is_ordinary_local_def(lo_insn, k, lo, nullptr, base_live_after)) {
        continue;
      }
      // Require the SAME live range of `lo` born at `k` to remain continuously
      // live past `base_end_rel` without dying or being redefined in `(k, base_end_rel]`.
      bool continuously_live = true;
      for (int j = k; j <= base_end_rel; ++j) {
        if (!base_live_after[static_cast<size_t>(j)].contains(lo) ||
            (j > k && insn_info[static_cast<size_t>(prologue_end + j)].defs.contains(lo))) {
          continuously_live = false;
          break;
        }
      }
      if (continuously_live) {
        straddling_lo_regs.insert(lo);
      }
    }
    // Symmetric leading-straddler case (`local-alloc.c:2158-2182` `find_free_reg`):
    // same-class call-used `lo < hi` already live entering `base_rel` (born before
    // `base_rel` at an ordinary non-reload local def) that dies strictly inside
    // `(base_rel, base_end_rel)` without being redefined later inside
    // `[base_rel, base_end_rel)`. Hoisting `lo`'s death ahead of `base_rel`
    // exposes `lo` as free across `hi`'s entire live range. Skip zero-input
    // `const_double` `hi` producers because forward `Schedule(kSched1)` freely
    // sinks zero-input constants next to their consumer past `lo`'s death.
    if (base_rel > 0 && s->SetSrc().op != RtlOp::kConstDouble) {
      for (int lo : base_live_after[static_cast<size_t>(base_rel - 1)]) {
        if (lo < 2 || lo >= hi || !same_reg_file(lo, hi) || !mm.CallUsed(lo) ||
            IsFixedOrSpecialHardReg(lo, stack_layout.frame_pointer_needed) ||
            base_sum.live_at_entry.contains(lo) || (hi_needs_even && (lo % 2) != 0) ||
            straddling_lo_regs.contains(lo) ||
            insn_info[static_cast<size_t>(oid)].reads.contains(lo) ||
            insn_info[static_cast<size_t>(oid)].defs.contains(lo)) {
          continue;
        }
        int lo_death = -1;
        bool redefined_in_window = false;
        for (int j = base_rel; j < base_end_rel; ++j) {
          if (insn_info[static_cast<size_t>(prologue_end + j)].defs.contains(lo)) {
            redefined_in_window = true;
            break;
          }
          if (lo_death < 0 && !base_live_after[static_cast<size_t>(j)].contains(lo)) {
            lo_death = j;
          }
        }
        if (redefined_in_window || lo_death <= base_rel) {
          continue;
        }
        int lo_birth = -1;
        bool lo_crosses_call = false;
        for (int j = base_rel - 1; j >= 0; --j) {
          const int j_oid = prologue_end + j;
          if (base_block[static_cast<size_t>(j_oid)].kind == Insn::Kind::kCall) {
            lo_crosses_call = true;
            break;
          }
          if (insn_info[static_cast<size_t>(j_oid)].defs.contains(lo)) {
            lo_birth = j;
            break;
          }
        }
        if (lo_crosses_call || lo_birth < 0) {
          continue;
        }
        const Insn& lo_birth_insn = base_block[static_cast<size_t>(prologue_end + lo_birth)];
        if (is_ordinary_local_def(lo_birth_insn, lo_birth, lo, nullptr, base_live_after)) {
          straddling_lo_regs.insert(lo);
        }
      }
    }
    if (straddling_lo_regs.empty()) {
      continue;
    }
    // `local-alloc.c:2158-2182` (`find_free_reg` ascending regno scan): dropping
    // a straddling interference with `lo < hi` only causes `find_free_reg` to
    // re-assign `hi`'s qty to `lo` when every lower allocatable call-used
    // same-class register `r < hi` was actually occupied during `hi`'s live
    // range `[base_rel, base_end_rel)` in `base_block`.
    absl::btree_set<int> base_range_occupied;
    for (int k = base_rel; k < base_end_rel; ++k) {
      base_range_occupied.insert(base_live_after[static_cast<size_t>(k)].begin(),
                                 base_live_after[static_cast<size_t>(k)].end());
      if (k > base_rel) {
        const auto& kdefs = insn_info[static_cast<size_t>(prologue_end + k)].defs;
        base_range_occupied.insert(kdefs.begin(), kdefs.end());
      }
    }
    const int class_first = (hi >= kMipsFpRegFirst) ? kMipsFpRegFirst : 2;
    const int class_step = hi_needs_even ? 2 : 1;
    bool all_lower_occupied = true;
    for (int r = class_first; r < hi; r += class_step) {
      if (!mm.CallUsed(r) || IsFixedOrSpecialHardReg(r, stack_layout.frame_pointer_needed)) {
        continue;
      }
      bool covered = base_range_occupied.contains(r);
      if (hi_needs_even && r + 1 < kMipsFirstPseudoRegister) {
        covered = covered || base_range_occupied.contains(r + 1);
      }
      if (!covered) {
        all_lower_occupied = false;
        break;
      }
    }
    if (!all_lower_occupied) {
      continue;
    }
    const int cand_rel = orig_to_cand_rel[static_cast<size_t>(oid)];
    if (cand_rel < 0 || !cand_live_after[static_cast<size_t>(cand_rel)].contains(hi)) {
      continue;
    }
    absl::btree_set<int> cand_overlaps;
    for (int k = cand_rel; k < body_len; ++k) {
      if (!cand_live_after[static_cast<size_t>(k)].contains(hi)) {
        break;
      }
      for (int lo : cand_live_after[static_cast<size_t>(k)]) {
        if (straddling_lo_regs.contains(lo)) {
          cand_overlaps.insert(lo);
        }
      }
    }
    if (cand_overlaps.size() < straddling_lo_regs.size()) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool PreservesPostReloadOrderSafety(absl::Span<const Insn> base_block,
                                    absl::Span<const int> cand_in_base, int prologue_end,
                                    int epilogue_begin, const absl::btree_set<int>& live_at_end_s2,
                                    const absl::btree_set<std::pair<int, int>>& cfg_copy_pairs,
                                    const RegFacts& facts_s2, const MachineModel& mm,
                                    bool allow_pure_reader_reg_dead_transfer,
                                    const PostReloadStackLayout& stack_layout,
                                    const absl::btree_set<int>& excluded_prologue_body_indices,
                                    bool allow_moved_store_load_equiv_invalidation) {
  const int n = static_cast<int>(base_block.size());
  if (static_cast<int>(cand_in_base.size()) != n) {
    return false;
  }
  std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
  for (int pos = 0; pos < n; ++pos) {
    pos_in_cand[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])] = pos;
  }
  for (int i = 0; i < prologue_end; ++i) {
    if (pos_in_cand[static_cast<size_t>(i)] != i) {
      return false;
    }
  }
  for (int i = epilogue_begin; i < n; ++i) {
    if (pos_in_cand[static_cast<size_t>(i)] != i) {
      return false;
    }
  }

  // 1. Preserve every semantic hard-reg / memory / call dependence edge of `base_block`
  //    (excluding asymmetric `sched.c:1704` post-call `call_used_regs[0]` artifact edges
  //    and position-dependent `sched.c:1740` `pending_lists_length > 32` flush-cap
  //    artifacts; forward `kSched2` replay verification in Gate (a) independently
  //    enforces exact `pending_lists_length > 32` scheduling equivalence).
  const DepGraph base_graph =
      AnalyzeWithoutPendingListFlushCap(base_block, facts_s2, mm, Phase::kSched2);
  for (int cons = 0; cons < n; ++cons) {
    for (const Link& l : base_graph.log_links[static_cast<size_t>(cons)]) {
      if (pos_in_cand[static_cast<size_t>(l.target)] >= pos_in_cand[static_cast<size_t>(cons)] &&
          !IsPureSched1704CallArtifactEdge(base_block, base_graph, l.target, cons, mm)) {
        return false;
      }
    }
  }

  // 2. Preserve adjacent `hi`/`lo` (`64`/`65`) output-reload and stack/mtc1 reload
  //    contiguity (`reload1.c:emit_reload_insns`).
  absl::btree_set<int> md_output_regs;
  for (int i = 0; i + 1 < n; ++i) {
    const RtlExpr& p2 = base_block[static_cast<size_t>(i + 1)].pn.pattern;
    if (IsAdjacentMdOutputReloadPair(base_block[static_cast<size_t>(i)],
                                     base_block[static_cast<size_t>(i + 1)]) &&
        pos_in_cand[static_cast<size_t>(i + 1)] != pos_in_cand[static_cast<size_t>(i)] + 1) {
      return false;
    }
    if (IsMdOutputReloadInsn(base_block[static_cast<size_t>(i + 1)])) {
      if (p2.SetDest().op == RtlOp::kReg && p2.SetDest().RegNo() >= 0 &&
          p2.SetDest().RegNo() < kMipsFirstPseudoRegister) {
        const int nr = mm.HardRegNRegs(p2.SetDest().RegNo(), p2.SetDest().mode);
        for (int k = 0; k < nr; ++k) {
          md_output_regs.insert(p2.SetDest().RegNo() + k);
        }
      }
    }
    if (IsAdjacentReloadContiguityPair(base_block[static_cast<size_t>(i)],
                                       base_block[static_cast<size_t>(i + 1)], mm, stack_layout) &&
        pos_in_cand[static_cast<size_t>(i + 1)] != pos_in_cand[static_cast<size_t>(i)] + 1) {
      return false;
    }
  }

  // 3. Preserve `REG_DEAD` reader order (`sched.c:3415-3611, 3764-3830`).
  //    Post-reload `sched2` (`reload_completed == 1`) does not recompute `REG_DEAD`
  //    notes, so a pure reader `i` that held `REG_DEAD r` in `.09.greg` can be
  //    hoisted ahead of pure reader `j` in `.10.sched2` (`i < j` in `base_block`,
  //    `pos_in_cand[i] > pos_in_cand[j]` in `cand_in_base`). Allow restoring `i`
  //    back to the last reader of `r` in `cand_in_base` when neither `i` nor `j`
  //    is a call/`USE` or writes `r`, and no intervening instruction writes `r`.
  std::vector<InsnRegReadDefDeaths> insn_info(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    insn_info[static_cast<size_t>(i)] =
        CollectInsnRegReadDefDeaths(base_block[static_cast<size_t>(i)], mm);
  }
  for (int i = 0; i < n; ++i) {
    if (insn_info[static_cast<size_t>(i)].deaths.empty()) {
      continue;
    }
    for (int j = 0; j < n; ++j) {
      if (i == j) {
        continue;
      }
      const bool flip_j_was_before =
          (j < i && pos_in_cand[static_cast<size_t>(j)] > pos_in_cand[static_cast<size_t>(i)]);
      const bool flip_i_was_before =
          (i < j && pos_in_cand[static_cast<size_t>(i)] > pos_in_cand[static_cast<size_t>(j)]);
      if (!flip_j_was_before && !flip_i_was_before) {
        continue;
      }
      for (int r : insn_info[static_cast<size_t>(i)].deaths) {
        if (!insn_info[static_cast<size_t>(j)].reads.contains(r)) {
          continue;
        }
        if (insn_info[static_cast<size_t>(i)].is_call_or_use ||
            insn_info[static_cast<size_t>(j)].is_call_or_use ||
            insn_info[static_cast<size_t>(i)].defs.contains(r) ||
            insn_info[static_cast<size_t>(j)].defs.contains(r)) {
          return false;
        }
        const int base_lo = std::min(i, j);
        const int base_hi = std::max(i, j);
        for (int k = base_lo + 1; k < base_hi; ++k) {
          if (insn_info[static_cast<size_t>(k)].defs.contains(r)) {
            return false;
          }
        }
        const int cand_lo =
            std::min(pos_in_cand[static_cast<size_t>(i)], pos_in_cand[static_cast<size_t>(j)]);
        const int cand_hi =
            std::max(pos_in_cand[static_cast<size_t>(i)], pos_in_cand[static_cast<size_t>(j)]);
        for (int p = cand_lo + 1; p < cand_hi; ++p) {
          if (insn_info[static_cast<size_t>(cand_in_base[static_cast<size_t>(p)])].defs.contains(
                  r)) {
            return false;
          }
        }
        if (flip_j_was_before) {
          if (!allow_pure_reader_reg_dead_transfer &&
              !IsCrossClassFprToGprCopyReloadInheritedRead(
                  base_block, cand_in_base, pos_in_cand[static_cast<size_t>(i)],
                  pos_in_cand[static_cast<size_t>(j)], r, mm)) {
            return false;
          }
        } else if (!allow_pure_reader_reg_dead_transfer) {
          for (int p = pos_in_cand[static_cast<size_t>(i)] + 1; p < n; ++p) {
            const int m = cand_in_base[static_cast<size_t>(p)];
            if (insn_info[static_cast<size_t>(m)].defs.contains(r)) {
              break;
            }
            if (insn_info[static_cast<size_t>(m)].reads.contains(r)) {
              return false;
            }
          }
        }
      }
    }
  }

  // 4. Preserve hard-register live-at-entry, call crossings (`caller-save.c`), exact
  //    `local-alloc.c:540-603, 990-1055` (`validate_equiv_mem`) status, and monotonic
  //    copy-tie / `hi`-`lo` allocation-order interferences (`global.c:962-985`,
  //    `local-alloc.c:1399, 2158-2181`).
  std::vector<int> base_ids(static_cast<size_t>(n));
  std::vector<Insn> cand_seq;
  cand_seq.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    base_ids[static_cast<size_t>(i)] = i;
    cand_seq.push_back(base_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(i)])]);
  }
  absl::btree_set<int> excluded_prologue_orig_ids;
  for (int i = 0; i < prologue_end; ++i) {
    excluded_prologue_orig_ids.insert(i);
  }
  for (int rel : excluded_prologue_body_indices) {
    excluded_prologue_orig_ids.insert(prologue_end + rel);
  }
  const HardRegLivenessSummary base_sum = ComputeHardRegLivenessSummary(
      base_block, base_ids, live_at_end_s2, facts_s2, mm, stack_layout, excluded_prologue_orig_ids);
  const HardRegLivenessSummary cand_sum =
      ComputeHardRegLivenessSummary(cand_seq, cand_in_base, live_at_end_s2, facts_s2, mm,
                                    stack_layout, excluded_prologue_orig_ids);
  if (base_sum.live_at_entry != cand_sum.live_at_entry) {
    return false;
  }
  if (base_sum.call_crossing_regs != cand_sum.call_crossing_regs) {
    return false;
  }
  // When `allow_pure_reader_reg_dead_transfer` is true (Stage 0 discovery probe),
  // defer `mem_equiv_valid` verification until after the pure-reader `REG_DEAD`
  // transfers are applied (where `PreservesPostReloadOrderSafety(..., false)`
  // enforces strict `mem_equiv_valid` consistency on the transferred notes).
  if (!allow_pure_reader_reg_dead_transfer &&
      base_sum.mem_equiv_valid != cand_sum.mem_equiv_valid) {
    for (const auto& [oid, bv] : base_sum.mem_equiv_valid) {
      const auto cit = cand_sum.mem_equiv_valid.find(oid);
      const bool cv = (cit != cand_sum.mem_equiv_valid.end()) && cit->second;
      if (bv == cv) {
        continue;
      }
      // `local-alloc.c:990-1000, 1049-1055`: if a `MEM` load (or the initializing
      // insn of a dying `MEM` store) already carries a matching `REG_EQUIV` note
      // from pre-`sched2` `update_equiv_regs`, transitioning `validate_equiv_mem`
      // from `false` (broken by `sched2` reordering) back to `true` restores the
      // exact pre-`sched2` equivalence condition.
      if (!bv && cv) {
        const Insn& base_insn = base_block[static_cast<size_t>(oid)];
        const RtlExpr& pat = base_insn.pn.pattern;
        if (pat.op == RtlOp::kSet && pat.children.size() == 2) {
          if (pat.SetDest().op == RtlOp::kReg && pat.SetSrc().op == RtlOp::kMem &&
              HasMatchingMemRegEquivNote(base_insn, pat.SetSrc(), stack_layout)) {
            continue;
          }
          if (pat.SetDest().op == RtlOp::kMem && pat.SetSrc().op == RtlOp::kReg &&
              !pat.SetSrc().IsArgPointerReg()) {
            const int store_pos = pos_in_cand[static_cast<size_t>(oid)];
            const int init_pos =
                FindUniqueInitPosForStoreInSeq(cand_seq, store_pos, pat.SetSrc().RegNo(), mm);
            if (init_pos >= 0 && HasMatchingMemRegEquivNote(cand_seq[static_cast<size_t>(init_pos)],
                                                            pat.SetDest(), stack_layout)) {
              continue;
            }
          }
        }
      }
      // Symmetric `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`) case:
      // when `sched2` moved a non-dying store `oid` `(set (mem M) (reg r))` AFTER
      // `r`'s true death instruction `d < oid`, `validate_equiv_mem` on `base_block`
      // stopped scanning at `d` and spuriously reported `bv = true`. Moving `oid`
      // back ahead of `d` (`pos_in_cand[oid] < pos_in_cand[d]`) restores `cv = false`,
      // matching the absence of a `REG_EQUIV (mem M)` note on `r`'s initializer.
      if (bv && !cv) {
        const Insn& base_insn = base_block[static_cast<size_t>(oid)];
        const RtlExpr& pat = base_insn.pn.pattern;
        if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kMem &&
            pat.SetSrc().op == RtlOp::kReg && !pat.SetSrc().IsArgPointerReg()) {
          const int r = pat.SetSrc().RegNo();
          const int store_pos = pos_in_cand[static_cast<size_t>(oid)];
          const int init_pos = FindUniqueInitPosForStoreInSeq(cand_seq, store_pos, r, mm);
          if (init_pos >= 0 && !HasMatchingMemRegEquivNote(cand_seq[static_cast<size_t>(init_pos)],
                                                           pat.SetDest(), stack_layout)) {
            if (!HasRegDeadNoteCovering(base_insn.pn.reg_notes, r, mm)) {
              bool moved_ahead_of_death = false;
              for (int d = oid - 1; d >= prologue_end; --d) {
                if (insn_info[static_cast<size_t>(d)].deaths.contains(r) &&
                    pos_in_cand[static_cast<size_t>(d)] > store_pos) {
                  moved_ahead_of_death = true;
                  break;
                }
                if (insn_info[static_cast<size_t>(d)].defs.contains(r)) {
                  break;
                }
              }
              if (moved_ahead_of_death) {
                continue;
              }
            }
            // Symmetric `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`,
            // `memref_used_between_p`, `update_equiv_regs`) store case: when
            // `sched2` moved an address-base write/death, call, or overlapping
            // `MEM` access out of `[init, store)`, `base_block` spuriously saw
            // `bv = true`. Moving it back into `[init_pos, store_pos)` restores
            // `cv = false`, matching the absence of `REG_EQUIV` on `init_pos`.
            if (HasStoreEquivInvalidatorMovedIntoWindow(oid, init_pos, pat.SetDest(), prologue_end,
                                                        epilogue_begin, base_block, cand_in_base,
                                                        pos_in_cand, insn_info, facts_s2, mm,
                                                        stack_layout, excluded_prologue_orig_ids)) {
              continue;
            }
          }
        }
        // Symmetric `local-alloc.c:540-603, 1049-1055` (`validate_equiv_mem`,
        // `update_equiv_regs`) load case: when `sched2` hoisted a `(REG_DEAD base_r)`
        // insn `d` ahead of `oid` or hoisted `REG_DEAD dst_r` ahead of an invalidator
        // `d` (base-reg write/death, non-const call, or aliasing `MEM` store),
        // `validate_equiv_mem` on `base_block` missed `d` and spuriously reported
        // `bv = true`. Restoring `d` into `[load_pos, cand_dst_death_pos)` restores
        // `cv = false`, matching the absence of a `REG_EQUIV (mem ...)` note on `oid`.
        if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg &&
            pat.SetSrc().op == RtlOp::kMem &&
            !HasMatchingMemRegEquivNote(base_insn, pat.SetSrc(), stack_layout) &&
            HasLoadEquivInvalidatorMovedIntoWindow(
                oid, pat.SetSrc(), pat.SetDest().RegNo(), prologue_end, epilogue_begin, base_block,
                cand_in_base, pos_in_cand, insn_info, facts_s2, mm, stack_layout,
                excluded_prologue_orig_ids, allow_moved_store_load_equiv_invalidation)) {
          continue;
        }
      }
      return false;
    }
    for (const auto& [oid, cv] : cand_sum.mem_equiv_valid) {
      if (!base_sum.mem_equiv_valid.contains(oid) && cv) {
        return false;
      }
    }
  }
  // Adding an interference between distinct hard registers `a != b` preserves coloring
  // validity; only dropping an interference `(a, b)` can trigger copy-tie coalescing
  // (`global.c:find_reg` / `local-alloc.c:1399`) or lower-numbered `REG_ALLOC_ORDER`
  // selection (`reload1.c:3619-3663` `order_regs_for_reload`) on `hi`/`lo` output reloads.
  absl::btree_set<int> non_md_body_defined_regs;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    if (excluded_prologue_orig_ids.contains(i)) {
      continue;
    }
    const Insn& insn = base_block[static_cast<size_t>(i)];
    if (IsMdOutputReloadInsn(insn) || insn.kind == Insn::Kind::kCall) {
      continue;
    }
    for (int d : insn_info[static_cast<size_t>(i)].defs) {
      if (d > 0 && d < kMipsFirstPseudoRegister) {
        non_md_body_defined_regs.insert(d);
      }
    }
  }
  auto same_reg_file = [](int a, int b) {
    return (a < kMipsFpRegFirst && b < kMipsFpRegFirst) ||
           (a >= kMipsFpRegFirst && a <= kMipsFpRegLast && b >= kMipsFpRegFirst &&
            b <= kMipsFpRegLast);
  };
  std::optional<HardRegLivenessSummary> base_sum_without_reload_spill_stores;
  auto get_base_interferences_without_reload_spill_stores =
      [&]() -> const absl::btree_set<std::pair<int, int>>& {
    if (!base_sum_without_reload_spill_stores.has_value()) {
      absl::btree_set<int> excluded_with_spills = excluded_prologue_orig_ids;
      absl::btree_set<int> body_defined_so_far;
      for (int i = prologue_end; i < epilogue_begin; ++i) {
        if (excluded_prologue_orig_ids.contains(i)) {
          continue;
        }
        // `reload1.c:emit_reload_insns` emits output reload spill stores
        // `(set (mem [sp+off]) (reg r))` immediately after the defining insn of `r`
        // with `REG_NOTES = NULL_RTX` (no `REG_DEAD r`). When post-reload `sched2`
        // defers such a spill store past a reload-inherited copy `(set (reg d) (reg r))`
        // (`reload1.c:5186-5257`), it creates a spurious post-`sched2` interference
        // `(r, d)` that never existed in `.09.greg` / `.08.lreg`.
        if (auto acc = TryParsePlainStackSlotMove(base_block[static_cast<size_t>(i)],
                                                  stack_layout.frame_pointer_needed);
            acc.has_value() && acc->is_store &&
            stack_layout.stack_slot_load_offsets.contains(acc->key.offset) &&
            !stack_layout.caller_save_slot_offsets.contains(acc->key.offset) &&
            !HasRegDeadNoteCovering(base_block[static_cast<size_t>(i)].pn.reg_notes, acc->regno,
                                    mm) &&
            body_defined_so_far.contains(acc->regno)) {
          excluded_with_spills.insert(i);
        }
        for (int d : insn_info[static_cast<size_t>(i)].defs) {
          if (d >= 0 && d < kMipsFirstPseudoRegister) {
            body_defined_so_far.insert(d);
          }
        }
      }
      base_sum_without_reload_spill_stores = ComputeHardRegLivenessSummary(
          base_block, base_ids, live_at_end_s2, facts_s2, mm, stack_layout, excluded_with_spills);
    }
    return base_sum_without_reload_spill_stores->interferences;
  };
  auto reg_crosses_call = [&](int r) {
    if (cand_sum.call_crossing_regs.contains(r)) {
      return true;
    }
    return stack_layout.cfg_call_crossing_regs.contains(r) &&
           (base_sum.live_at_entry.contains(r) || live_at_end_s2.contains(r));
  };
  // `reload1.c:5599-5665` (`emit_reload_insns`): an earlyclobber `DImode` GPR
  // output-reload copy `(set (reg:DI orig) (reg:DI reload))` reunited adjacent
  // to its owning `PARALLEL` is folded away by `InvertReload` before
  // `local-alloc`/`global-alloc` ever runs, so a spurious post-`sched2`
  // interference between `orig` and the pure reload scratch `reload` never
  // causes copy-tie coalescing.
  absl::btree_set<std::pair<int, int>> earlyclobber_reload_copy_pairs;
  for (int pos = prologue_end + 1; pos < epilogue_begin; ++pos) {
    int orig_hr = -1;
    int reload_hr = -1;
    if (!TryMatchEarlyclobberDiOutputReload(cand_seq[static_cast<size_t>(pos - 1)],
                                            cand_seq[static_cast<size_t>(pos)], mm, &orig_hr,
                                            &reload_hr) ||
        !IsRegDeadAfterInsn(cand_seq, pos, reload_hr, RtlMachineMode::kDi, mm, live_at_end_s2) ||
        base_sum.live_at_entry.contains(reload_hr) ||
        base_sum.live_at_entry.contains(reload_hr + 1)) {
      continue;
    }
    bool pure_reload_scratch = true;
    for (int k = prologue_end; k < epilogue_begin; ++k) {
      if (k == pos - 1 || k == pos) {
        continue;
      }
      const auto& ki = insn_info[static_cast<size_t>(cand_in_base[static_cast<size_t>(k)])];
      if (ki.reads.contains(reload_hr) || ki.reads.contains(reload_hr + 1) ||
          ki.defs.contains(reload_hr) || ki.defs.contains(reload_hr + 1)) {
        pure_reload_scratch = false;
        break;
      }
    }
    if (pure_reload_scratch) {
      for (int w1 = 0; w1 < 2; ++w1) {
        for (int w2 = 0; w2 < 2; ++w2) {
          const int a = orig_hr + w1;
          const int b = reload_hr + w2;
          earlyclobber_reload_copy_pairs.insert({std::min(a, b), std::max(a, b)});
        }
      }
    }
  }
  for (const std::pair<int, int>& edge : base_sum.interferences) {
    if (cand_sum.interferences.contains(edge)) {
      continue;
    }
    // `local-alloc.c:2106` / `global.c:927` (`COPY_HARD_REG_SET (..., call_used_reg_set)`):
    // a call-crossing register excludes `call_used_reg_set`, so dropping an
    // interference between a call-used register and a call-crossing register
    // can never coalesce them via copy-tie preference (`global.c:find_reg`).
    const bool call_crossing_vs_call_used =
        (mm.CallUsed(edge.first) && reg_crosses_call(edge.second)) ||
        (mm.CallUsed(edge.second) && reg_crosses_call(edge.first));
    if (cfg_copy_pairs.contains(edge) && same_reg_file(edge.first, edge.second) &&
        !call_crossing_vs_call_used) {
      if (earlyclobber_reload_copy_pairs.contains(edge) ||
          !get_base_interferences_without_reload_spill_stores().contains(edge)) {
        continue;
      }
      return false;
    }
    const int lo = edge.first;
    const int hi = edge.second;
    // `reload1.c:3619-3663` (`order_regs_for_reload`): an unused hard reg `hi`
    // that appears solely as a reload-inserted `mfhi`/`mflo` output register
    // (`hard_reg_n_uses[hi] == 0`) is strictly preferred ahead of any register
    // `lo` already live or defined in the body (`hard_reg_n_uses[lo] > 0`).
    const bool hi_is_pure_md_reload =
        md_output_regs.contains(hi) && !non_md_body_defined_regs.contains(hi) &&
        !base_sum.live_at_entry.contains(hi) && !live_at_end_s2.contains(hi);
    const bool lo_is_used_reg = non_md_body_defined_regs.contains(lo) ||
                                base_sum.live_at_entry.contains(lo) || live_at_end_s2.contains(lo);
    const bool md_blocked =
        md_output_regs.contains(hi) && !(hi_is_pure_md_reload && lo_is_used_reg);
    // `local-alloc.c:2106` / `global.c:927` (`COPY_HARD_REG_SET (..., call_used_reg_set)`):
    // a call-crossing register `hi` excludes `call_used_reg_set`, so dropping an
    // interference with a call-used register `lo` cannot change `hi`'s allocation.
    if (md_blocked && !(mm.CallUsed(lo) && reg_crosses_call(hi)) && same_reg_file(lo, hi)) {
      return false;
    }
  }
  return !DropsPerLiveRangeLocalFindFreeRegInterference(
      base_block, cand_in_base, prologue_end, epilogue_begin, excluded_prologue_orig_ids,
      live_at_end_s2, base_sum, cfg_copy_pairs, insn_info, stack_layout, mm);
}

std::optional<RtlExpr> FindUsedHardRegExprInPattern(const RtlExpr& root, int target_regno) {
  std::optional<RtlExpr> found;
  auto walk = [&](auto& self, const RtlExpr& e, bool is_set_lhs) -> void {
    if (found.has_value()) {
      return;
    }
    if (e.op == RtlOp::kSet && e.children.size() == 2) {
      self(self, e.SetSrc(), false);
      if (e.SetDest().op == RtlOp::kMem && !e.SetDest().children.empty()) {
        self(self, e.SetDest().children[0], false);
      } else {
        self(self, e.SetDest(), true);
      }
      return;
    }
    if (e.op == RtlOp::kClobber) {
      return;
    }
    if (e.op == RtlOp::kReg) {
      if (!is_set_lhs && e.RegNo() == target_regno) {
        found = e;
      }
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch, false);
    }
  };
  walk(walk, root, false);
  return found;
}

std::vector<int> CollectDisplacedBlock0EntryArgCopyBodyIndices(
    absl::Span<const Insn> post_reload_body, const Sched1ProjectionView& view,
    const absl::btree_set<int>& excluded_prologue_body_indices, const MachineModel& mm,
    bool frame_pointer_needed) {
  std::vector<int> displaced;
  const int body_len = static_cast<int>(post_reload_body.size());
  if (body_len <= 1 || view.core_insns.empty()) {
    return displaced;
  }

  // Find the first call/jump boundary in `post_reload_body`: entry-parameter copies
  // (`function.c:3668-3742, 5129-5144`, `sched.c:3191-3215`) cannot cross a call/jump.
  int limit = body_len;
  int first_call_idx = -1;
  for (int i = 0; i < body_len; ++i) {
    if (post_reload_body[static_cast<size_t>(i)].kind == Insn::Kind::kCall) {
      first_call_idx = i;
      limit = i;
      break;
    }
    if (post_reload_body[static_cast<size_t>(i)].kind == Insn::Kind::kJump) {
      limit = i;
      break;
    }
  }
  if (limit <= 1) {
    return displaced;
  }

  // Identify hard regs directly feeding the first call's `call_fusage` so we never
  // mistake an outgoing call-argument move `(set a0 a1)` for an entry-param copy.
  std::vector<absl::btree_set<int>> call_arg_defs(static_cast<size_t>(limit));
  if (first_call_idx >= 0) {
    absl::btree_set<int> needed_args;
    for (const RtlExpr& fu : post_reload_body[static_cast<size_t>(first_call_idx)].pn.call_fusage) {
      CollectRegReads(fu, &needed_args);
    }
    for (int prev = first_call_idx - 1; prev >= 0 && !needed_args.empty(); --prev) {
      if (excluded_prologue_body_indices.contains(prev)) {
        continue;
      }
      const InsnRegReadDefDeaths info =
          CollectInsnRegReadDefDeaths(post_reload_body[static_cast<size_t>(prev)], mm);
      for (int d : info.defs) {
        if (needed_args.contains(d)) {
          call_arg_defs[static_cast<size_t>(prev)].insert(d);
          needed_args.erase(d);
        }
      }
    }
  }

  absl::btree_map<int, int> orig_to_core_idx;
  for (int ci = 0; ci < static_cast<int>(view.core_to_orig_index.size()); ++ci) {
    orig_to_core_idx[view.core_to_orig_index[static_cast<size_t>(ci)]] = ci;
  }
  absl::btree_set<int> pinned_set(view.pinned_prefix_orig_indices.begin(),
                                  view.pinned_prefix_orig_indices.end());
  absl::btree_set<int> written_hard_regs;
  absl::btree_set<int> word_copied_args;

  auto is_incoming_arg_reg = [](int r) {
    return (r >= kMipsArgRegFirst && r <= kMipsArgRegLast) ||
           (r >= kMipsFpArgRegFirst && r <= kMipsFpArgRegFirst + 3);
  };

  for (int rel = 0; rel < limit; ++rel) {
    if (excluded_prologue_body_indices.contains(rel)) {
      continue;
    }
    const Insn& insn = post_reload_body[static_cast<size_t>(rel)];
    const RtlExpr& pat = insn.pn.pattern;

    if (pinned_set.contains(rel)) {
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg &&
          pat.SetSrc().op == RtlOp::kReg) {
        const RtlExpr& s = pat.SetSrc();
        if ((s.mode == RtlMachineMode::kSi || s.mode == RtlMachineMode::kDi) &&
            s.RegNo() >= kMipsArgRegFirst && s.RegNo() <= kMipsArgRegLast) {
          word_copied_args.insert(s.RegNo());
        }
      }
      const InsnRegReadDefDeaths info = CollectInsnRegReadDefDeaths(insn, mm);
      written_hard_regs.insert(info.defs.begin(), info.defs.end());
      continue;
    }

    // Include `/i` (`flags.integrated`) return-reg entry-parameter copies (e.g.,
    // sret hidden pointer `(set (reg/i:SI 2 v0) (reg:SI 4 a0))` colored directly
    // to `$v0` in `local-alloc`, `function.c:assign_parms, 5129-5144`,
    // `sched.c:3191-3215`) provided `dr` has not already been written earlier,
    // and block-0 direct spilled entry-parameter copies lowered to
    // `(set (mem:M [sp/fp+off]) (reg:M arg))` (`reload1.c:2309-2400`) whose
    // projected `sched1` view destination is a pseudo register.
    bool matched_displaced = false;
    const auto core_it = orig_to_core_idx.find(rel);
    const bool is_direct_spilled_entry_copy =
        core_it != orig_to_core_idx.end() && insn.kind == Insn::Kind::kInsn &&
        pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kMem &&
        pat.SetSrc().op == RtlOp::kReg &&
        view.core_insns[static_cast<size_t>(core_it->second)].pn.pattern.op == RtlOp::kSet &&
        view.core_insns[static_cast<size_t>(core_it->second)].pn.pattern.children.size() == 2 &&
        view.core_insns[static_cast<size_t>(core_it->second)].pn.pattern.SetDest().op ==
            RtlOp::kReg &&
        view.core_insns[static_cast<size_t>(core_it->second)].pn.pattern.SetDest().RegNo() >=
            kMipsFirstPseudoRegister;
    if (is_direct_spilled_entry_copy) {
      const RtlExpr& src_r = pat.SetSrc();
      const int sr = src_r.RegNo();
      if (is_incoming_arg_reg(sr)) {
        const int snr = mm.HardRegNRegs(sr, src_r.mode);
        bool src_clobbered = false;
        for (int k = 0; k < snr; ++k) {
          if (written_hard_regs.contains(sr + k)) {
            src_clobbered = true;
            break;
          }
        }
        bool narrow_unpaired = false;
        if ((pat.SetDest().mode == RtlMachineMode::kQi ||
             pat.SetDest().mode == RtlMachineMode::kHi) &&
            (src_r.mode == RtlMachineMode::kQi || src_r.mode == RtlMachineMode::kHi) &&
            sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast && !word_copied_args.contains(sr)) {
          narrow_unpaired = true;
        }
        if (!src_clobbered && !narrow_unpaired) {
          matched_displaced = true;
          displaced.push_back(rel);
          if ((src_r.mode == RtlMachineMode::kSi || src_r.mode == RtlMachineMode::kDi) &&
              sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast) {
            word_copied_args.insert(sr);
          }
        }
      }
    } else if (core_it != orig_to_core_idx.end() && insn.kind == Insn::Kind::kInsn &&
               pat.op == RtlOp::kSet && pat.children.size() == 2 &&
               pat.SetDest().op == RtlOp::kReg && !pat.SetDest().IsArgPointerReg() &&
               !pat.SetDest().flags.frame_related &&
               (!pat.SetDest().flags.integrated ||
                !written_hard_regs.contains(pat.SetDest().RegNo()))) {
      const RtlExpr& dst = pat.SetDest();
      const int dr = dst.RegNo();
      const bool non_fixed_dst = dr > 0 && dr < kMipsFirstPseudoRegister &&
                                 !IsFixedOrSpecialHardReg(dr, frame_pointer_needed);
      const RtlExpr* inner_src = &pat.SetSrc();
      while ((inner_src->op == RtlOp::kSubreg || inner_src->op == RtlOp::kSignExtend ||
              inner_src->op == RtlOp::kZeroExtend || inner_src->op == RtlOp::kSignExtract ||
              inner_src->op == RtlOp::kZeroExtract) &&
             !inner_src->children.empty()) {
        inner_src = &inner_src->children[0];
      }
      if (non_fixed_dst && inner_src->op == RtlOp::kReg && !inner_src->IsArgPointerReg() &&
          is_incoming_arg_reg(inner_src->RegNo()) && inner_src->RegNo() != dr &&
          !call_arg_defs[static_cast<size_t>(rel)].contains(dr)) {
        const int sr = inner_src->RegNo();
        const int snr = mm.HardRegNRegs(sr, inner_src->mode);
        bool src_clobbered = false;
        for (int k = 0; k < snr; ++k) {
          if (written_hard_regs.contains(sr + k)) {
            src_clobbered = true;
            break;
          }
        }
        // Mirror `sched.c:3197-3214` + `ParseBlockForSchedModel:5654-5667`: do not
        // treat an un-word-copied narrow `QI`/`HI` copy from `$a0-$a3` as pinned.
        bool narrow_unpaired = false;
        if (pat.SetSrc().op == RtlOp::kReg &&
            (dst.mode == RtlMachineMode::kQi || dst.mode == RtlMachineMode::kHi) &&
            (pat.SetSrc().mode == RtlMachineMode::kQi ||
             pat.SetSrc().mode == RtlMachineMode::kHi) &&
            sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast && !word_copied_args.contains(sr)) {
          narrow_unpaired = true;
        }
        // Do not hoist a caller-saved scratch `mtc1` input reload (`reload1.c`)
        // that sits contiguously before its FP consumer; callee-saved FPRs
        // (`$f20-$f30`) are `assign_parms` copies, never scratch input reloads.
        bool is_scratch_mtc1_reload = false;
        if (dr >= kMipsFpRegFirst && dr < kMipsCalleeSavedFprFirst && sr < kMipsFpRegFirst &&
            rel + 1 < body_len &&
            IsAdjacentReloadContiguityPair(insn, post_reload_body[static_cast<size_t>(rel + 1)],
                                           mm)) {
          is_scratch_mtc1_reload = true;
        }
        if (!src_clobbered && !narrow_unpaired && !is_scratch_mtc1_reload) {
          matched_displaced = true;
          displaced.push_back(rel);
          if (pat.SetSrc().op == RtlOp::kReg &&
              (pat.SetSrc().mode == RtlMachineMode::kSi ||
               pat.SetSrc().mode == RtlMachineMode::kDi) &&
              sr >= kMipsArgRegFirst && sr <= kMipsArgRegLast) {
            word_copied_args.insert(sr);
          }
        }
      }
    }
    const InsnRegReadDefDeaths info = CollectInsnRegReadDefDeaths(insn, mm);
    written_hard_regs.insert(info.defs.begin(), info.defs.end());
    (void)matched_displaced;
  }
  return displaced;
}

std::optional<std::vector<int>> BuildInvertedRegDeadSeedOrderInBase(
    absl::Span<const Insn> base_block, int prologue_end, int epilogue_begin,
    const RegFacts& facts_s2, const MachineModel& mm, InvertedRegDeadSeedMode mode) {
  const int n = static_cast<int>(base_block.size());
  if (epilogue_begin - prologue_end <= 1) {
    return std::nullopt;
  }
  std::vector<int> seed_in_base(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    seed_in_base[static_cast<size_t>(i)] = i;
  }
  const std::vector<int> identity_order = seed_in_base;
  const DepGraph base_graph =
      AnalyzeWithoutPendingListFlushCap(base_block, facts_s2, mm, Phase::kSched2);
  bool any_inverted = false;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    const Insn& i_insn = base_block[static_cast<size_t>(i)];
    if (i_insn.kind == Insn::Kind::kCall || i_insn.pn.pattern.op == RtlOp::kUse) {
      continue;
    }
    for (const RtlRegNote& note : i_insn.pn.reg_notes) {
      if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
        continue;
      }
      const int r = note.datum.RegNo();
      if (r <= 0 || r >= kMipsFirstPseudoRegister) {
        continue;
      }
      const int nr = std::max(1, mm.HardRegNRegs(r, note.datum.mode));
      if (InsnDefinesHardRegSpan(i_insn, r, nr, mm)) {
        continue;
      }
      std::vector<int> later_readers;
      for (int j = i + 1; j < epilogue_begin; ++j) {
        const Insn& j_insn = base_block[static_cast<size_t>(j)];
        if (InsnDefinesHardRegSpan(j_insn, r, nr, mm)) {
          break;
        }
        if (j_insn.kind != Insn::Kind::kCall && j_insn.pn.pattern.op != RtlOp::kUse &&
            InsnReadsHardRegSpan(j_insn, r, nr, mm)) {
          if (IsCrossClassFprToGprCopyReloadInheritedRead(base_block, identity_order, i, j, r,
                                                          mm)) {
            continue;
          }
          later_readers.push_back(j);
          // Per `local-alloc.c:580-599` (`validate_equiv_mem`), if `j` is a
          // `MEM` load carrying `(REG_EQUIV (mem ... r ...))`, `i`'s
          // `(REG_DEAD r)` cannot lie inside `[j, death_of_dst)` without
          // invalidating the `REG_EQUIV` note; include `dst`'s death insn so
          // `kPullBeforeDead` / `kDeferAfterReaders` keeps `[j, death_of_dst]`
          // ahead of `i`.
          const RtlExpr* js = SingleSetOf(j_insn.pn.pattern);
          if (js != nullptr && js->SetDest().op == RtlOp::kReg &&
              !js->SetDest().IsArgPointerReg() && js->SetSrc().op == RtlOp::kMem) {
            bool has_equiv_on_r = false;
            for (const RtlRegNote& jn : j_insn.pn.reg_notes) {
              if (jn.kind == RtlMachineMode::kRegEquiv && jn.datum.op == RtlOp::kMem) {
                for (int w = 0; w < nr; ++w) {
                  if (jn.datum.ContainsReg(r + w)) {
                    has_equiv_on_r = true;
                    break;
                  }
                }
              }
              if (has_equiv_on_r) {
                break;
              }
            }
            if (has_equiv_on_r) {
              const int dst_r = js->SetDest().RegNo();
              const int dst_nr = std::max(1, mm.HardRegNRegs(dst_r, js->SetDest().mode));
              for (int d = j + 1; d < epilogue_begin; ++d) {
                const Insn& d_insn = base_block[static_cast<size_t>(d)];
                if (InsnDefinesHardRegSpan(d_insn, r, nr, mm)) {
                  break;
                }
                bool kills_dst = InsnDefinesHardRegSpan(d_insn, dst_r, dst_nr, mm);
                for (int w = 0; w < dst_nr && !kills_dst; ++w) {
                  if (HasRegDeadNoteCovering(d_insn.pn.reg_notes, dst_r + w, mm)) {
                    kills_dst = true;
                  }
                }
                if (kills_dst) {
                  if (d_insn.kind != Insn::Kind::kCall && d_insn.pn.pattern.op != RtlOp::kUse) {
                    later_readers.push_back(d);
                    // Keep any `reload1.c:5641-6155` (`emit_reload_insns`)
                    // adjacent output-reload / spill-store insns attached to
                    // `d` atomic with `d` so neither `kPullBeforeDead` nor
                    // `kDeferAfterReaders` splits a `reload_adjacent` pair.
                    int adj = d;
                    while (
                        adj + 1 < epilogue_begin &&
                        (IsAdjacentMdOutputReloadPair(base_block[static_cast<size_t>(adj)],
                                                      base_block[static_cast<size_t>(adj + 1)]) ||
                         IsAdjacentReloadContiguityPair(base_block[static_cast<size_t>(adj)],
                                                        base_block[static_cast<size_t>(adj + 1)],
                                                        mm))) {
                      ++adj;
                      later_readers.push_back(adj);
                    }
                  }
                  break;
                }
              }
            }
          }
        }
      }
      if (mode == InvertedRegDeadSeedMode::kPullBeforeDead && !later_readers.empty()) {
        auto it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
        if (it_i != seed_in_base.end()) {
          const absl::btree_set<int> later_set(later_readers.begin(), later_readers.end());
          absl::btree_set<int> after_i_set;
          std::vector<int> stack;
          absl::btree_set<int> to_pull;
          for (auto it = it_i + 1; it != seed_in_base.end(); ++it) {
            after_i_set.insert(*it);
            if (later_set.contains(*it) && to_pull.insert(*it).second) {
              stack.push_back(*it);
            }
          }
          // Include transitive in-window `kSched2` ancestors of `later_readers`
          // positioned after `i` so pulling them ahead of `i` preserves
          // `base_graph` topological order (e.g., anti-dep ancestors reusing
          // the destination reg of a later reader).
          bool depends_on_i = false;
          while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            if (cur + 1 < epilogue_begin && after_i_set.contains(cur + 1) &&
                (IsAdjacentMdOutputReloadPair(base_block[static_cast<size_t>(cur)],
                                              base_block[static_cast<size_t>(cur + 1)]) ||
                 IsAdjacentReloadContiguityPair(base_block[static_cast<size_t>(cur)],
                                                base_block[static_cast<size_t>(cur + 1)], mm)) &&
                to_pull.insert(cur + 1).second) {
              stack.push_back(cur + 1);
            }
            for (const Link& l : base_graph.log_links[static_cast<size_t>(cur)]) {
              if (l.target == i) {
                depends_on_i = true;
                break;
              }
              if (after_i_set.contains(l.target) && to_pull.insert(l.target).second) {
                stack.push_back(l.target);
              }
            }
            if (depends_on_i) {
              break;
            }
          }
          if (!depends_on_i && !to_pull.empty()) {
            std::vector<int> trailing;
            trailing.reserve(to_pull.size());
            for (auto it = it_i + 1; it != seed_in_base.end(); ++it) {
              if (to_pull.contains(*it)) {
                trailing.push_back(*it);
              }
            }
            seed_in_base.erase(std::remove_if(seed_in_base.begin(), seed_in_base.end(),
                                              [&](int node) { return to_pull.contains(node); }),
                               seed_in_base.end());
            it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
            seed_in_base.insert(it_i, trailing.begin(), trailing.end());
            any_inverted = true;
            continue;
          }
        }
      }
      if (mode == InvertedRegDeadSeedMode::kDeferAfterReaders && !later_readers.empty()) {
        auto it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
        auto it_last = it_i;
        for (int j : later_readers) {
          auto it_j = std::find(seed_in_base.begin(), seed_in_base.end(), j);
          if (it_j != seed_in_base.end() && it_j > it_last) {
            it_last = it_j;
          }
        }
        if (it_i != seed_in_base.end() && it_last > it_i) {
          // Prefer deferring `i` together with its in-window forward `base_graph`
          // descendants immediately after the last later reader whenever no
          // later reader transitively depends on `i` (exact inverse of `sched2`
          // hoisting `i` early).
          const absl::btree_set<int> later_set(later_readers.begin(), later_readers.end());
          absl::btree_set<int> fwd_closure = {i};
          bool later_depends_on_i = false;
          for (auto it = it_i + 1; it <= it_last; ++it) {
            bool dep = false;
            for (const Link& l : base_graph.log_links[static_cast<size_t>(*it)]) {
              if (fwd_closure.contains(l.target)) {
                dep = true;
                break;
              }
            }
            if (dep) {
              if (later_set.contains(*it)) {
                later_depends_on_i = true;
                break;
              }
              fwd_closure.insert(*it);
            }
          }
          if (!later_depends_on_i) {
            const int last_node = *it_last;
            std::vector<int> deferred;
            deferred.reserve(fwd_closure.size());
            for (auto it = it_i; it <= it_last; ++it) {
              if (fwd_closure.contains(*it)) {
                deferred.push_back(*it);
              }
            }
            seed_in_base.erase(std::remove_if(seed_in_base.begin(), seed_in_base.end(),
                                              [&](int node) { return fwd_closure.contains(node); }),
                               seed_in_base.end());
            auto ins_it = std::find(seed_in_base.begin(), seed_in_base.end(), last_node);
            seed_in_base.insert(ins_it + 1, deferred.begin(), deferred.end());
            any_inverted = true;
            continue;
          }
        }
      }
      for (int j : later_readers) {
        auto it_j = std::find(seed_in_base.begin(), seed_in_base.end(), j);
        auto it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
        if (it_j == seed_in_base.end() || it_i == seed_in_base.end() || it_j <= it_i) {
          continue;
        }
        // Collect `j` together with all transitive `kSched2` ancestors currently
        // positioned after `i` in `seed_in_base` so hoisting `j` ahead of `i`
        // preserves topological order of `base_graph` (e.g., an address-offset
        // shift between `i` and `j` feeding `j`).
        absl::btree_set<int> after_i_set;
        for (auto it = it_i + 1; it != seed_in_base.end(); ++it) {
          after_i_set.insert(*it);
        }
        absl::btree_set<int> to_hoist = {j};
        std::vector<int> stack = {j};
        bool depends_on_i = false;
        while (!stack.empty()) {
          const int cur = stack.back();
          stack.pop_back();
          for (const Link& l : base_graph.log_links[static_cast<size_t>(cur)]) {
            if (l.target == i) {
              depends_on_i = true;
              break;
            }
            if (after_i_set.contains(l.target) && to_hoist.insert(l.target).second) {
              stack.push_back(l.target);
            }
          }
          if (depends_on_i) {
            break;
          }
        }
        if (depends_on_i) {
          continue;
        }
        std::vector<int> hoist_seq;
        absl::btree_set<int> external_deps;
        for (int node : seed_in_base) {
          if (to_hoist.contains(node)) {
            hoist_seq.push_back(node);
            for (const Link& l : base_graph.log_links[static_cast<size_t>(node)]) {
              if (!to_hoist.contains(l.target)) {
                external_deps.insert(l.target);
              }
            }
          }
        }
        seed_in_base.erase(std::remove_if(seed_in_base.begin(), seed_in_base.end(),
                                          [&](int node) { return to_hoist.contains(node); }),
                           seed_in_base.end());
        it_i = std::find(seed_in_base.begin(), seed_in_base.end(), i);
        auto insert_it = seed_in_base.begin() + prologue_end;
        for (auto it = seed_in_base.begin(); it != it_i; ++it) {
          if (external_deps.contains(*it)) {
            insert_it = it + 1;
          }
        }
        if (insert_it > it_i) {
          insert_it = it_i;
        }
        seed_in_base.insert(insert_it, hoist_seq.begin(), hoist_seq.end());
        any_inverted = true;
      }
    }
  }
  if (!any_inverted) {
    return std::nullopt;
  }
  return seed_in_base;
}

std::vector<std::pair<int, int>> ExtractCoreSched2Edges(absl::Span<const Insn> cur_seq,
                                                        int prologue_end, int epilogue_begin,
                                                        const Sched1ProjectionView& view,
                                                        const RegFacts& facts_s2,
                                                        const MachineModel& mm) {
  std::vector<std::pair<int, int>> edges;
  const int n = static_cast<int>(cur_seq.size());
  const int m = static_cast<int>(view.core_insns.size());
  if (m <= 1 || prologue_end < 0 || epilogue_begin > n || epilogue_begin <= prologue_end) {
    return edges;
  }
  const DepGraph g_s2 = AnalyzeWithoutPendingListFlushCap(cur_seq, facts_s2, mm, Phase::kSched2);
  std::vector<int> seq_to_core(static_cast<size_t>(n), -1);
  for (int ci = 0; ci < m; ++ci) {
    const int si = prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)];
    if (si >= prologue_end && si < epilogue_begin) {
      seq_to_core[static_cast<size_t>(si)] = ci;
    }
  }
  // Compute transitive reachability in `g_s2` over `[prologue_end, epilogue_begin)`
  // (excluding pure `sched.c:1704` post-call artifact edges) and restrict to core
  // instructions. Because `cur_seq` is already topologically ordered in `g_s2`,
  // every edge has `src < dst`.
  const int words = (m + 63) / 64;
  std::vector<std::vector<uint64_t>> reach_core(
      static_cast<size_t>(n), std::vector<uint64_t>(static_cast<size_t>(words), 0));
  for (int si = epilogue_begin - 1; si >= prologue_end; --si) {
    const int ci = seq_to_core[static_cast<size_t>(si)];
    if (ci >= 0) {
      reach_core[static_cast<size_t>(si)][static_cast<size_t>(ci / 64)] |=
          (uint64_t{1} << (ci % 64));
    }
  }
  for (int cons = epilogue_begin - 1; cons >= prologue_end; --cons) {
    for (const Link& l : g_s2.log_links[static_cast<size_t>(cons)]) {
      if (l.target < prologue_end || l.target >= cons) {
        continue;
      }
      if (IsPureSched1704CallArtifactEdge(cur_seq, g_s2, l.target, cons, mm)) {
        continue;
      }
      for (int w = 0; w < words; ++w) {
        reach_core[static_cast<size_t>(l.target)][static_cast<size_t>(w)] |=
            reach_core[static_cast<size_t>(cons)][static_cast<size_t>(w)];
      }
    }
  }
  for (int u = 0; u < m; ++u) {
    const int si = prologue_end + view.core_to_orig_index[static_cast<size_t>(u)];
    if (si < prologue_end || si >= epilogue_begin) {
      continue;
    }
    const auto& bits = reach_core[static_cast<size_t>(si)];
    for (int v = u + 1; v < m; ++v) {
      if ((bits[static_cast<size_t>(v / 64)] & (uint64_t{1} << (v % 64))) != 0) {
        edges.emplace_back(u, v);
      }
    }
  }
  return edges;
}

std::vector<std::pair<int, int>> ExtractCoreSched2ReplayEdges(
    absl::Span<const Insn> target_sched_block, absl::Span<const int> cur_to_target,
    int prologue_end, int epilogue_begin, const Sched1ProjectionView& view,
    const RegFacts& facts_s2, const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2,
    bool preserve_reg_dead_reader_order) {
  std::vector<std::pair<int, int>> edges;
  const int n = static_cast<int>(target_sched_block.size());
  const int m = static_cast<int>(view.core_insns.size());
  if (m <= 1 || n <= 1 || static_cast<int>(cur_to_target.size()) != n || prologue_end < 0 ||
      epilogue_begin > n || epilogue_begin <= prologue_end) {
    return edges;
  }
  std::vector<int> target_to_cur(static_cast<size_t>(n), -1);
  for (int pos = 0; pos < n; ++pos) {
    const int t = cur_to_target[static_cast<size_t>(pos)];
    if (t < 0 || t >= n || target_to_cur[static_cast<size_t>(t)] != -1) {
      return edges;
    }
    target_to_cur[static_cast<size_t>(t)] = pos;
  }
  std::vector<int> target_to_core(static_cast<size_t>(n), -1);
  for (int ci = 0; ci < m; ++ci) {
    const int cur_pos = prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)];
    if (cur_pos >= prologue_end && cur_pos < epilogue_begin) {
      target_to_core[static_cast<size_t>(cur_to_target[static_cast<size_t>(cur_pos)])] = ci;
    }
  }

  const DepGraph target_graph =
      AnalyzeWithoutPendingListFlushCap(target_sched_block, facts_s2, mm, Phase::kSched2);
  ListScheduler replay_sched(target_sched_block, target_graph, facts_s2, mm, Phase::kSched2,
                             live_at_end_s2);
  const ListScheduler::ForcedReplayResult replay = replay_sched.RunForcedReplay();

  absl::btree_set<std::pair<int, int>> constrained_edges;
  for (int cons = 0; cons < n; ++cons) {
    for (const Link& l : target_graph.log_links[static_cast<size_t>(cons)]) {
      if (l.target < 0 || l.target >= n || l.target == cons) {
        continue;
      }
      if (IsPureSched1704CallArtifactEdge(target_sched_block, target_graph, l.target, cons, mm)) {
        continue;
      }
      constrained_edges.insert({l.target, cons});
    }
  }
  if (replay.last_tail >= 0) {
    for (int u = 0; u < replay.last_tail; ++u) {
      constrained_edges.insert({u, replay.last_tail});
    }
    for (int k = replay.last_tail; k + 1 < n; ++k) {
      constrained_edges.insert({k, k + 1});
    }
  }
  std::vector<int> hard_next(static_cast<size_t>(n), -1);
  std::vector<int> hard_prev(static_cast<size_t>(n), -1);
  for (int i = 1; i < n; ++i) {
    if (target_graph.sched_group_p[static_cast<size_t>(i)]) {
      hard_next[static_cast<size_t>(i - 1)] = i;
      hard_prev[static_cast<size_t>(i)] = i - 1;
    }
  }
  std::vector<int> identity_rank(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    identity_rank[static_cast<size_t>(i)] = i;
  }
  for (const Constraint& c : replay.constraints) {
    if (c.than < 0 || c.than >= n || c.must_have_greater_luid < 0 ||
        c.must_have_greater_luid >= n || c.than == c.must_have_greater_luid) {
      continue;
    }
    const std::pair<int, int> edge = {c.than, c.must_have_greater_luid};
    if (constrained_edges.contains(edge)) {
      continue;
    }
    constrained_edges.insert(edge);
    if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
             .has_value()) {
      constrained_edges.erase(edge);
    }
  }

  // When strict `PreservesPostReloadOrderSafety(..., allow_transfer=false)` is
  // enforced (Stage 3), every pure reader `j < i` of a register killed at `i`
  // (`REG_DEAD r`, with no intervening def of `r`) must stay ahead of `i`
  // (`sched.c:3415-3611`). Projecting `(j -> i)` keeps `CollectSched1TieFlipCoreOrders`
  // from spending search budget on Gate-(b)-violating `REG_DEAD` reader flips.
  if (preserve_reg_dead_reader_order) {
    std::vector<InsnRegReadDefDeaths> insn_info(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      insn_info[static_cast<size_t>(i)] =
          CollectInsnRegReadDefDeaths(target_sched_block[static_cast<size_t>(i)], mm);
    }
    for (int i = 0; i < n; ++i) {
      for (int r : insn_info[static_cast<size_t>(i)].deaths) {
        for (int j = i - 1; j >= 0; --j) {
          if (insn_info[static_cast<size_t>(j)].reads.contains(r)) {
            const std::pair<int, int> edge = {j, i};
            if (!constrained_edges.contains(edge)) {
              constrained_edges.insert(edge);
              if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
                       .has_value()) {
                constrained_edges.erase(edge);
              }
            }
          }
          if (insn_info[static_cast<size_t>(j)].defs.contains(r)) {
            break;
          }
        }
        // Symmetric inverted-`REG_DEAD` case (`sched.c:3415-3611`): when `sched2`
        // hoisted the pure-reader `(REG_DEAD r)` holder `i` ahead of a later pure
        // reader `j > i` of the same live range of `r`, every valid pre-`sched2`
        // preimage must also place `j` before `i` (`edge = {j, i}`).
        if (!insn_info[static_cast<size_t>(i)].is_call_or_use &&
            !insn_info[static_cast<size_t>(i)].defs.contains(r)) {
          for (int j = i + 1; j < n; ++j) {
            if (insn_info[static_cast<size_t>(j)].defs.contains(r)) {
              break;
            }
            if (!insn_info[static_cast<size_t>(j)].is_call_or_use &&
                insn_info[static_cast<size_t>(j)].reads.contains(r)) {
              const std::pair<int, int> edge = {j, i};
              if (!constrained_edges.contains(edge)) {
                constrained_edges.insert(edge);
                if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
                         .has_value()) {
                  constrained_edges.erase(edge);
                }
              }
            }
          }
        }
      }
    }
  }

  // Project constraints on excluded input/output reload instructions onto their
  // owning core instruction (`reload1.c:emit_reload_insns` places input reloads
  // immediately before and output reloads immediately after their owner, with no
  // other core instruction intervening).
  std::vector<int> target_reload_owner(static_cast<size_t>(n), -1);
  for (const auto& [body_idx, ci] : view.input_reload_before_core) {
    const int cur_pos = prologue_end + body_idx;
    if (cur_pos >= prologue_end && cur_pos < epilogue_begin && ci >= 0 && ci < m) {
      target_reload_owner[static_cast<size_t>(cur_to_target[static_cast<size_t>(cur_pos)])] = ci;
    }
  }
  for (const auto& [body_idx, ci] : view.output_reload_after_core) {
    const int cur_pos = prologue_end + body_idx;
    if (cur_pos >= prologue_end && cur_pos < epilogue_begin && ci >= 0 && ci < m) {
      target_reload_owner[static_cast<size_t>(cur_to_target[static_cast<size_t>(cur_pos)])] = ci;
    }
  }
  auto add_reload_projected_edges = [&](int from_t, int to_t) {
    if (from_t < 0 || from_t >= n || to_t < 0 || to_t >= n || from_t == to_t) {
      return;
    }
    const std::pair<int, int> edge = {from_t, to_t};
    if (constrained_edges.contains(edge)) {
      return;
    }
    constrained_edges.insert(edge);
    if (!SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank)
             .has_value()) {
      constrained_edges.erase(edge);
    }
  };
  const std::vector<std::pair<int, int>> base_constrained(constrained_edges.begin(),
                                                          constrained_edges.end());
  for (const auto& [body_idx, ci] : view.input_reload_before_core) {
    const int cur_pos = prologue_end + body_idx;
    const int owner_cur = (ci >= 0 && ci < m)
                              ? (prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)])
                              : -1;
    if (cur_pos < prologue_end || cur_pos >= epilogue_begin || owner_cur < prologue_end ||
        owner_cur >= epilogue_begin) {
      continue;
    }
    const int t_r = cur_to_target[static_cast<size_t>(cur_pos)];
    const int t_u = cur_to_target[static_cast<size_t>(owner_cur)];
    for (const auto& [u, v] : base_constrained) {
      if (u == t_r && v != t_u && target_reload_owner[static_cast<size_t>(v)] != ci) {
        add_reload_projected_edges(t_u, v);
      }
    }
  }
  for (const auto& [body_idx, ci] : view.output_reload_after_core) {
    const int cur_pos = prologue_end + body_idx;
    const int owner_cur = (ci >= 0 && ci < m)
                              ? (prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)])
                              : -1;
    if (cur_pos < prologue_end || cur_pos >= epilogue_begin || owner_cur < prologue_end ||
        owner_cur >= epilogue_begin) {
      continue;
    }
    const int t_s = cur_to_target[static_cast<size_t>(cur_pos)];
    const int t_u = cur_to_target[static_cast<size_t>(owner_cur)];
    for (const auto& [u, v] : base_constrained) {
      if (v == t_s && u != t_u && target_reload_owner[static_cast<size_t>(u)] != ci) {
        add_reload_projected_edges(u, t_u);
      }
    }
  }

  const std::optional<std::vector<int>> topo =
      SolveSupernodeKahn(n, constrained_edges, hard_next, hard_prev, identity_rank);
  if (!topo.has_value()) {
    return edges;
  }
  std::vector<std::vector<int>> succs(static_cast<size_t>(n));
  for (const auto& [u, v] : constrained_edges) {
    succs[static_cast<size_t>(u)].push_back(v);
  }
  const int words = (m + 63) / 64;
  std::vector<std::vector<uint64_t>> reach_core(
      static_cast<size_t>(n), std::vector<uint64_t>(static_cast<size_t>(words), 0));
  for (int idx = n - 1; idx >= 0; --idx) {
    const int u = (*topo)[static_cast<size_t>(idx)];
    const int ci = target_to_core[static_cast<size_t>(u)];
    if (ci >= 0) {
      reach_core[static_cast<size_t>(u)][static_cast<size_t>(ci / 64)] |=
          (uint64_t{1} << (ci % 64));
    }
    for (int v : succs[static_cast<size_t>(u)]) {
      for (int w = 0; w < words; ++w) {
        reach_core[static_cast<size_t>(u)][static_cast<size_t>(w)] |=
            reach_core[static_cast<size_t>(v)][static_cast<size_t>(w)];
      }
    }
  }
  for (int u = 0; u < m; ++u) {
    const int cur_pos = prologue_end + view.core_to_orig_index[static_cast<size_t>(u)];
    if (cur_pos < prologue_end || cur_pos >= epilogue_begin) {
      continue;
    }
    const int t_u = cur_to_target[static_cast<size_t>(cur_pos)];
    const auto& bits = reach_core[static_cast<size_t>(t_u)];
    for (int v = 0; v < m; ++v) {
      if (v != u && (bits[static_cast<size_t>(v / 64)] & (uint64_t{1} << (v % 64))) != 0) {
        edges.emplace_back(u, v);
      }
    }
  }
  return edges;
}

namespace {

// Conflict-driven `sched1` tie-flip solver (`sched.c:2428` `rank_for_schedule`,
// `sched.c:2557-2609` `schedule_insn`, `toplev.c:3028-3087`): given a `kSched1`
// schedule `base_res` that violates one or more post-reload `core_s2_edges`
// `(u -> v)` (meaning backward `sched1` scheduled `v` AFTER `u` instead of
// BEFORE `u`), promotes `v` or its blocking `g_s1` consumer chain earlier in
// the backward schedule via cumulative equal-priority LUID tie-flips until
// `core_s2_edges` violations drop to 0.
void CollectConflictDrivenSched2Orders(
    const Sched1ProjectionView& view, const DepGraph& g_s1, const ScheduleResult& base_res,
    const absl::btree_set<std::pair<int, int>>& dag_edges, const std::vector<int>& hard_next,
    const std::vector<int>& hard_prev, const std::vector<int>& base_order,
    absl::Span<const std::pair<int, int>> core_s2_edges, const MachineModel& mm, int max_orders,
    int non_zero_cap, absl::btree_set<std::vector<int>>* seen_orders,
    std::vector<std::vector<int>>* out) {
  const int m = static_cast<int>(view.core_insns.size());
  if (core_s2_edges.empty() || m <= 1 || static_cast<int>(out->size()) >= max_orders) {
    return;
  }
  std::vector<int> base_rank(static_cast<size_t>(m), 0);
  for (int pos = 0; pos < m; ++pos) {
    base_rank[static_cast<size_t>(base_order[static_cast<size_t>(pos)])] = pos;
  }
  // Build reverse `g_s1` consumer adjacency (`prod -> consumers`).
  std::vector<std::vector<int>> consumers_of(static_cast<size_t>(m));
  for (int cons = 0; cons < m; ++cons) {
    for (const Link& l : g_s1.log_links[static_cast<size_t>(cons)]) {
      if (l.target >= 0 && l.target < m && l.target != cons) {
        consumers_of[static_cast<size_t>(l.target)].push_back(cons);
      }
    }
  }

  struct EvalState {
    absl::btree_set<std::pair<int, int>> extra_edges;
    std::vector<int> input_to_orig;
    ScheduleResult res;
    std::vector<int> mapped;
    int num_s2_viol = 0;
    int first_viol_step = 0;
    int s1_diffs = 0;
  };

  auto score_schedule = [&](const ScheduleResult& res, absl::Span<const int> input_to_orig,
                            std::vector<int>* mapped_out, int* num_viol_out, int* first_viol_out,
                            int* s1_diffs_out) {
    mapped_out->resize(static_cast<size_t>(m));
    std::vector<int> pos_in_mapped(static_cast<size_t>(m), -1);
    int diffs = 0;
    for (int pos = 0; pos < m; ++pos) {
      const int orig = input_to_orig[static_cast<size_t>(res.order[static_cast<size_t>(pos)])];
      (*mapped_out)[static_cast<size_t>(pos)] = orig;
      pos_in_mapped[static_cast<size_t>(orig)] = pos;
      if (orig != pos) {
        ++diffs;
      }
    }
    int viol = 0;
    int first_v = m;
    for (const auto& [u, v] : core_s2_edges) {
      if (pos_in_mapped[static_cast<size_t>(v)] < pos_in_mapped[static_cast<size_t>(u)]) {
        ++viol;
        const int u_step = m - 1 - pos_in_mapped[static_cast<size_t>(u)];
        first_v = std::min(first_v, u_step);
      }
    }
    *num_viol_out = viol;
    *first_viol_out = first_v;
    *s1_diffs_out = diffs;
  };

  auto extract_promotion_sets =
      [&](const EvalState& cur,
          bool include_u_demote) -> std::vector<absl::btree_set<std::pair<int, int>>> {
    std::vector<int> step_of_orig(static_cast<size_t>(m), -1);
    for (int s = 0; s < static_cast<int>(cur.res.trace.size()); ++s) {
      const int orig =
          cur.input_to_orig[static_cast<size_t>(cur.res.trace[static_cast<size_t>(s)].chosen)];
      step_of_orig[static_cast<size_t>(orig)] = s;
    }
    // Collect violated `core_s2_edges` `(u, v)` ordered by earliest backward step
    // `step_of_orig[u]`.
    std::vector<std::tuple<int, int, int, int>> viols;
    for (const auto& [u, v] : core_s2_edges) {
      const int su = step_of_orig[static_cast<size_t>(u)];
      const int sv = step_of_orig[static_cast<size_t>(v)];
      if (su >= 0 && sv >= 0 && su < sv) {
        viols.emplace_back(su, sv, u, v);
      }
    }
    std::sort(viols.begin(), viols.end());

    std::vector<absl::btree_set<std::pair<int, int>>> edge_sets;
    absl::btree_set<absl::btree_set<std::pair<int, int>>> dedup;
    auto push_set = [&](absl::btree_set<std::pair<int, int>> es) {
      if (!es.empty() && dedup.insert(es).second) {
        edge_sets.push_back(std::move(es));
      }
    };

    auto emit_promote_node = [&](int target_orig, int max_shift) {
      const int st_target = step_of_orig[static_cast<size_t>(target_orig)];
      if (st_target <= 0) {
        return;
      }
      absl::btree_set<std::pair<int, int>> accum;
      const int low_step = std::max(0, st_target - max_shift);
      for (int t = st_target - 1; t >= low_step; --t) {
        const StepTrace& st = cur.res.trace[static_cast<size_t>(t)];
        if (st.ready_before_select.size() != st.priorities.size()) {
          continue;
        }
        int chosen_pri = 0;
        bool found_chosen = false;
        int target_pri = 0;
        bool found_target = false;
        for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
          const int r_in = st.ready_before_select[k];
          if (r_in == st.chosen) {
            chosen_pri = st.priorities[k];
            found_chosen = true;
          }
          if (cur.input_to_orig[static_cast<size_t>(r_in)] == target_orig) {
            target_pri = st.priorities[k];
            found_target = true;
          }
        }
        if (found_chosen && found_target && target_pri == chosen_pri) {
          const int chosen_orig = cur.input_to_orig[static_cast<size_t>(st.chosen)];
          if (chosen_orig != target_orig) {
            accum.insert({chosen_orig, target_orig});
            push_set(accum);
          }
        }
      }
    };

    auto emit_demote_node = [&](int target_orig) {
      const int st_target = step_of_orig[static_cast<size_t>(target_orig)];
      if (st_target < 0 || st_target >= static_cast<int>(cur.res.trace.size())) {
        return;
      }
      const StepTrace& st = cur.res.trace[static_cast<size_t>(st_target)];
      if (st.ready_before_select.size() != st.priorities.size()) {
        return;
      }
      int target_pri = 0;
      bool found_target = false;
      for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
        if (st.ready_before_select[k] == st.chosen) {
          target_pri = st.priorities[k];
          found_target = true;
          break;
        }
      }
      if (!found_target) {
        return;
      }
      absl::btree_set<std::pair<int, int>> accum;
      for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
        const int other_orig = cur.input_to_orig[static_cast<size_t>(st.ready_before_select[k])];
        if (other_orig != target_orig && st.priorities[k] == target_pri) {
          push_set({{target_orig, other_orig}});
          accum.insert({target_orig, other_orig});
        }
      }
      if (accum.size() > 1) {
        push_set(std::move(accum));
      }
    };

    const int max_viols = std::min<int>(3, static_cast<int>(viols.size()));
    for (int vi = 0; vi < max_viols; ++vi) {
      const auto [su, sv, u, v] = viols[static_cast<size_t>(vi)];
      const int shift = std::max(1, sv - su) + 3;
      // (a) Promote `v` directly if `v` was already ready before `sv`.
      emit_promote_node(v, shift);
      // (b) Promote `v`'s latest-scheduled blocking `g_s1` consumers (depth 1 and 2).
      std::vector<std::pair<int, int>> d1_cons;
      for (int c : consumers_of[static_cast<size_t>(v)]) {
        const int sc = step_of_orig[static_cast<size_t>(c)];
        if (sc >= 0 && sc < sv) {
          d1_cons.emplace_back(-sc, c);
        }
      }
      std::sort(d1_cons.begin(), d1_cons.end());
      for (size_t ci = 0; ci < std::min<size_t>(2, d1_cons.size()); ++ci) {
        const int c = d1_cons[ci].second;
        emit_promote_node(c, shift);
        std::vector<std::pair<int, int>> d2_cons;
        for (int c2 : consumers_of[static_cast<size_t>(c)]) {
          const int sc2 = step_of_orig[static_cast<size_t>(c2)];
          if (sc2 >= 0 && sc2 < step_of_orig[static_cast<size_t>(c)]) {
            d2_cons.emplace_back(-sc2, c2);
          }
        }
        std::sort(d2_cons.begin(), d2_cons.end());
        for (size_t ci2 = 0; ci2 < std::min<size_t>(2, d2_cons.size()); ++ci2) {
          emit_promote_node(d2_cons[ci2].second, shift);
        }
      }
      // (c) Demote `u` at `su` (`sched.c:2428` `rank_for_schedule`). Pass 1
      //     keeps single-edge demotes of `u` itself; Pass 2 (when Pass 1 finds
      //     no zero-violation order) adds multi-edge `u` demotes and walks
      //     `u`'s consumer chain (up to depth 4) so pushing a sunk `u`-consumer
      //     later in backward time pulls `u` ahead of `v`.
      if (!include_u_demote) {
        const StepTrace& st_u = cur.res.trace[static_cast<size_t>(su)];
        if (st_u.ready_before_select.size() == st_u.priorities.size()) {
          int u_pri = 0;
          bool found_u = false;
          for (size_t k = 0; k < st_u.ready_before_select.size(); ++k) {
            if (st_u.ready_before_select[k] == st_u.chosen) {
              u_pri = st_u.priorities[k];
              found_u = true;
              break;
            }
          }
          if (found_u) {
            for (size_t k = 0; k < st_u.ready_before_select.size(); ++k) {
              const int other_orig =
                  cur.input_to_orig[static_cast<size_t>(st_u.ready_before_select[k])];
              if (other_orig != u && st_u.priorities[k] == u_pri) {
                push_set({{u, other_orig}});
              }
            }
          }
        }
      } else {
        emit_demote_node(u);
        std::vector<int> u_frontier = {u};
        absl::btree_set<int> u_seen = {u};
        for (int depth = 0; depth < 4 && !u_frontier.empty(); ++depth) {
          std::vector<std::pair<int, int>> next_cons;
          for (int cur_u : u_frontier) {
            const int scur = step_of_orig[static_cast<size_t>(cur_u)];
            for (int c : consumers_of[static_cast<size_t>(cur_u)]) {
              const int sc = step_of_orig[static_cast<size_t>(c)];
              if (sc >= 0 && sc < scur && !u_seen.contains(c)) {
                next_cons.emplace_back(-sc, c);
              }
            }
          }
          std::sort(next_cons.begin(), next_cons.end());
          u_frontier.clear();
          for (size_t ci = 0; ci < next_cons.size() && u_frontier.size() < 2; ++ci) {
            const int c = next_cons[ci].second;
            if (u_seen.insert(c).second) {
              emit_demote_node(c);
              u_frontier.push_back(c);
            }
          }
        }
      }
    }
    return edge_sets;
  };

  EvalState base_state;
  base_state.input_to_orig = base_order;
  base_state.res = base_res;
  score_schedule(base_state.res, base_state.input_to_orig, &base_state.mapped,
                 &base_state.num_s2_viol, &base_state.first_viol_step, &base_state.s1_diffs);
  if (base_state.num_s2_viol == 0) {
    return;
  }

  std::vector<std::vector<int>> zero_viol_orders;
  int non_zero_added = 0;
  constexpr int kMaxConflictSteps = 16;
  // Pass 1: pure `v`-promotion hill climb (monotonic preservation of all
  // promote-solvable blocks). Pass 2: if Pass 1 finds no zero-violation order,
  // retry with `u`-consumer-chain demotion enabled (`sched.c:2428`).
  for (const bool include_u_demote : {false, true}) {
    if (!zero_viol_orders.empty() || static_cast<int>(out->size()) >= max_orders) {
      break;
    }
    EvalState cur = base_state;
    absl::btree_set<absl::btree_set<std::pair<int, int>>> visited_sets;
    visited_sets.insert({});
    for (int step = 0; step < kMaxConflictSteps && static_cast<int>(out->size()) < max_orders;
         ++step) {
      const std::vector<absl::btree_set<std::pair<int, int>>> promos =
          extract_promotion_sets(cur, include_u_demote);
      if (promos.empty()) {
        break;
      }
      bool has_best = false;
      EvalState best;
      for (const auto& add_edges : promos) {
        absl::btree_set<std::pair<int, int>> next_extra = cur.extra_edges;
        bool conflict_with_existing = false;
        for (const auto& [a, b] : add_edges) {
          if (next_extra.contains({b, a})) {
            conflict_with_existing = true;
            break;
          }
          next_extra.insert({a, b});
        }
        if (conflict_with_existing || !visited_sets.insert(next_extra).second) {
          continue;
        }
        absl::btree_set<std::pair<int, int>> edges = dag_edges;
        edges.insert(next_extra.begin(), next_extra.end());
        const std::optional<std::vector<int>> flipped_input =
            SolveSupernodeKahn(m, edges, hard_next, hard_prev, base_rank);
        if (!flipped_input.has_value()) {
          continue;
        }
        std::vector<Insn> flipped_core;
        flipped_core.reserve(static_cast<size_t>(m));
        for (int pos = 0; pos < m; ++pos) {
          Insn insn =
              view.core_insns[static_cast<size_t>((*flipped_input)[static_cast<size_t>(pos)])];
          insn.luid = pos;
          flipped_core.push_back(std::move(insn));
        }
        const DepGraph g_flip = Analyze(flipped_core, view.facts_s1, mm, Phase::kSched1);
        ScheduleResult res_flip =
            Schedule(flipped_core, g_flip, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
        if (static_cast<int>(res_flip.order.size()) != m) {
          continue;
        }
        EvalState cand;
        cand.extra_edges = std::move(next_extra);
        cand.input_to_orig = *flipped_input;
        cand.res = std::move(res_flip);
        score_schedule(cand.res, cand.input_to_orig, &cand.mapped, &cand.num_s2_viol,
                       &cand.first_viol_step, &cand.s1_diffs);
        if (cand.num_s2_viol == 0) {
          if (seen_orders->insert(cand.mapped).second) {
            zero_viol_orders.push_back(cand.mapped);
          }
        } else if (non_zero_added < non_zero_cap && static_cast<int>(out->size()) < max_orders &&
                   seen_orders->insert(cand.mapped).second) {
          out->push_back(cand.mapped);
          ++non_zero_added;
        }
        auto better = [](const EvalState& a, const EvalState& b) {
          return std::tie(a.num_s2_viol, b.first_viol_step, a.s1_diffs) <
                 std::tie(b.num_s2_viol, a.first_viol_step, b.s1_diffs);
        };
        if (better(cand, cur) && (!has_best || better(cand, best))) {
          best = std::move(cand);
          has_best = true;
        }
      }
      if (!zero_viol_orders.empty() || !has_best) {
        break;
      }
      cur = std::move(best);
    }
  }
  if (!zero_viol_orders.empty()) {
    out->insert(out->begin(), zero_viol_orders.begin(), zero_viol_orders.end());
    if (static_cast<int>(out->size()) > max_orders) {
      out->resize(static_cast<size_t>(max_orders));
    }
  }
}

}  // namespace

std::vector<std::vector<int>> CollectSched1TieFlipCoreOrders(
    const Sched1ProjectionView& view, const MachineModel& mm, int max_orders,
    absl::Span<const std::pair<int, int>> core_s2_edges,
    absl::Span<const std::pair<int, int>> core_s2_replay_edges) {
  std::vector<std::vector<int>> out;
  const int m = static_cast<int>(view.core_insns.size());
  if (m <= 1 || max_orders <= 0) {
    return out;
  }
  const DepGraph g_s1 = Analyze(view.core_insns, view.facts_s1, mm, Phase::kSched1);
  const ScheduleResult base_res =
      Schedule(view.core_insns, g_s1, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
  if (static_cast<int>(base_res.order.size()) != m) {
    return out;
  }

  absl::btree_set<std::pair<int, int>> dag_edges;
  for (int cons = 0; cons < m; ++cons) {
    for (const Link& l : g_s1.log_links[static_cast<size_t>(cons)]) {
      dag_edges.insert({l.target, cons});
    }
  }
  std::vector<int> hard_next(static_cast<size_t>(m), -1);
  std::vector<int> hard_prev(static_cast<size_t>(m), -1);
  for (int i = 1; i < m; ++i) {
    if (g_s1.sched_group_p[static_cast<size_t>(i)]) {
      hard_next[static_cast<size_t>(i - 1)] = i;
      hard_prev[static_cast<size_t>(i)] = i - 1;
    }
  }
  std::vector<int> identity_rank(static_cast<size_t>(m));
  for (int i = 0; i < m; ++i) {
    identity_rank[static_cast<size_t>(i)] = i;
  }

  auto extract_ties = [&](const ScheduleResult& res, absl::Span<const int> input_to_orig,
                          int branch_limit) -> std::vector<std::pair<int, int>> {
    int first_div_step = static_cast<int>(res.trace.size());
    for (int step_idx = 0; step_idx < static_cast<int>(res.trace.size()); ++step_idx) {
      const int chosen_orig =
          input_to_orig[static_cast<size_t>(res.trace[static_cast<size_t>(step_idx)].chosen)];
      const int expected_orig = m - 1 - step_idx;
      if (chosen_orig != expected_orig) {
        first_div_step = step_idx;
        break;
      }
    }
    struct TieCandidate {
      int after_div = 0;
      int dist = 0;
      int step_idx = 0;
      int chosen_orig = -1;
      int other_orig = -1;
      bool operator<(const TieCandidate& rhs) const {
        return std::tie(after_div, dist, step_idx, chosen_orig, other_orig) <
               std::tie(rhs.after_div, rhs.dist, rhs.step_idx, rhs.chosen_orig, rhs.other_orig);
      }
    };
    std::vector<TieCandidate> ties;
    absl::btree_set<std::pair<int, int>> seen_pairs;
    for (int step_idx = 0; step_idx < static_cast<int>(res.trace.size()); ++step_idx) {
      const StepTrace& st = res.trace[static_cast<size_t>(step_idx)];
      if (st.ready_before_select.size() < 2 ||
          st.ready_before_select.size() != st.priorities.size()) {
        continue;
      }
      int chosen_pri = 0;
      bool found_chosen = false;
      for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
        if (st.ready_before_select[k] == st.chosen) {
          chosen_pri = st.priorities[k];
          found_chosen = true;
          break;
        }
      }
      if (!found_chosen) {
        continue;
      }
      const int chosen_orig = input_to_orig[static_cast<size_t>(st.chosen)];
      for (size_t k = 0; k < st.ready_before_select.size(); ++k) {
        const int other = st.ready_before_select[k];
        if (other == st.chosen || st.priorities[k] != chosen_pri) {
          continue;
        }
        const int other_orig = input_to_orig[static_cast<size_t>(other)];
        if (!seen_pairs.insert({chosen_orig, other_orig}).second) {
          continue;
        }
        ties.push_back(TieCandidate{
            .after_div = (step_idx > first_div_step) ? 1 : 0,
            .dist = std::abs(step_idx - first_div_step),
            .step_idx = step_idx,
            .chosen_orig = chosen_orig,
            .other_orig = other_orig,
        });
      }
    }
    std::sort(ties.begin(), ties.end());
    std::vector<std::pair<int, int>> result;
    for (const TieCandidate& tc : ties) {
      if (static_cast<int>(result.size()) >= branch_limit) {
        break;
      }
      result.push_back({tc.chosen_orig, tc.other_orig});
    }
    return result;
  };

  absl::btree_set<std::vector<int>> seen_orders;
  seen_orders.insert(base_res.order);
  seen_orders.insert(identity_rank);

  absl::btree_set<std::pair<int, int>> joint_edges = dag_edges;
  for (const auto& e : core_s2_edges) {
    joint_edges.insert(e);
  }
  absl::btree_set<std::pair<int, int>> joint_replay_edges = joint_edges;
  for (const auto& e : core_s2_replay_edges) {
    joint_replay_edges.insert(e);
  }
  const bool has_replay_edges =
      !core_s2_replay_edges.empty() && joint_replay_edges.size() > joint_edges.size();
  // Combined `kSched2` scoring edge set (DAG + forced-replay LUID tie-break
  // constraints when available) so seeds and conflict-driven promotion respect
  // exact `sched2` LUID tie-breaks (`sched.c:2428`).
  std::vector<std::pair<int, int>> combined_s2_edges(core_s2_edges.begin(), core_s2_edges.end());
  if (has_replay_edges) {
    absl::btree_set<std::pair<int, int>> merged(core_s2_edges.begin(), core_s2_edges.end());
    for (const auto& e : core_s2_replay_edges) {
      merged.insert(e);
    }
    combined_s2_edges.assign(merged.begin(), merged.end());
  }

  // First run conflict-driven resolution of violated `core_s2_edges` so orders
  // in `Im(F_sched1) ∩ Topo(g_s2)` are placed at the front of `out`, while
  // capping non-zero-violation intermediates so canonical `.combine` seeds and
  // BFS tie-flips are never starved.
  const int conflict_non_zero_cap = std::max(1, max_orders / 3);
  CollectConflictDrivenSched2Orders(view, g_s1, base_res, dag_edges, hard_next, hard_prev,
                                    identity_rank, combined_s2_edges, mm, max_orders,
                                    conflict_non_zero_cap, &seen_orders, &out);

  // Canonical pre-`sched1` (`.06.combine`) topological seeds (`expr.c:4027`
  // `expand_expr`, `sched.c:2428` `rank_for_schedule` LUID tie-breaking):
  // post-reload `sched2` frequently sinks multi-cycle FP/ALU consumers past
  // independent instructions or hoists independent loads above their consuming
  // ALU ops, trapping default-LUID `sched1` replay in stale post-`sched2` LUID
  // tie-breaks. Evaluating `Schedule(Q_seed, kSched1)` on ASAP, critical-path,
  // and sink-rooted / consumer-clustered expression-tree DFS post-order seeds
  // (under `g_s1 ∪ core_s2_replay_edges`, `g_s1 ∪ core_s2_edges`, and `g_s1`)
  // recovers un-sunk `Im(F_sched1)` candidates; zero-violation seeds are
  // prioritized ahead of partial states.
  auto count_s2_violations = [&](absl::Span<const int> mapped) -> int {
    std::vector<int> pos_in_mapped(static_cast<size_t>(m), -1);
    for (int pos = 0; pos < m; ++pos) {
      pos_in_mapped[static_cast<size_t>(mapped[static_cast<size_t>(pos)])] = pos;
    }
    int viol = 0;
    for (const auto& [u, v] : combined_s2_edges) {
      if (pos_in_mapped[static_cast<size_t>(v)] < pos_in_mapped[static_cast<size_t>(u)]) {
        ++viol;
      }
    }
    return viol;
  };
  std::vector<std::vector<int>> zero_viol_seeds;
  std::vector<std::pair<int, std::vector<int>>> non_zero_seeds;
  struct SeedConflictCandidate {
    int viol = 0;
    std::vector<int> seed_input;
    ScheduleResult res_seed;
  };
  std::optional<SeedConflictCandidate> best_seed_for_conflict;
  auto try_add_seed_rank_with_edges = [&](const std::vector<int>& rank_vec,
                                          const absl::btree_set<std::pair<int, int>>& seed_edges) {
    const std::optional<std::vector<int>> seed_input =
        SolveSupernodeKahn(m, seed_edges, hard_next, hard_prev, rank_vec);
    if (!seed_input.has_value()) {
      return;
    }
    std::vector<Insn> seed_core;
    seed_core.reserve(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      Insn insn = view.core_insns[static_cast<size_t>((*seed_input)[static_cast<size_t>(pos)])];
      insn.luid = pos;
      seed_core.push_back(std::move(insn));
    }
    const DepGraph g_seed = Analyze(seed_core, view.facts_s1, mm, Phase::kSched1);
    ScheduleResult res_seed =
        Schedule(seed_core, g_seed, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
    if (static_cast<int>(res_seed.order.size()) != m) {
      return;
    }
    std::vector<int> mapped(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      mapped[static_cast<size_t>(pos)] =
          (*seed_input)[static_cast<size_t>(res_seed.order[static_cast<size_t>(pos)])];
    }
    const int viol = count_s2_violations(mapped);
    if (viol == 0) {
      if (seen_orders.insert(mapped).second) {
        zero_viol_seeds.push_back(std::move(mapped));
      }
    } else {
      if (seen_orders.insert(mapped).second) {
        non_zero_seeds.emplace_back(viol, std::move(mapped));
      }
      if (!best_seed_for_conflict.has_value() || viol < best_seed_for_conflict->viol) {
        best_seed_for_conflict = SeedConflictCandidate{
            .viol = viol,
            .seed_input = *seed_input,
            .res_seed = std::move(res_seed),
        };
      }
    }
  };
  auto try_add_seed_rank = [&](const std::vector<int>& rank_vec) {
    if (has_replay_edges) {
      try_add_seed_rank_with_edges(rank_vec, joint_replay_edges);
    }
    if (!core_s2_edges.empty()) {
      try_add_seed_rank_with_edges(rank_vec, joint_edges);
    }
    try_add_seed_rank_with_edges(rank_vec, dag_edges);
  };

  {
    std::vector<bool> has_consumer(static_cast<size_t>(m), false);
    std::vector<bool> has_true_consumer(static_cast<size_t>(m), false);
    std::vector<int> true_indeg(static_cast<size_t>(m), 0);
    for (int u = 0; u < m; ++u) {
      for (const Link& l : g_s1.log_links[static_cast<size_t>(u)]) {
        if (l.target < 0 || l.target >= m || l.target == u) {
          continue;
        }
        has_consumer[static_cast<size_t>(l.target)] = true;
        if (l.kind == DepKind::kTrue) {
          has_true_consumer[static_cast<size_t>(l.target)] = true;
          ++true_indeg[static_cast<size_t>(u)];
        }
      }
    }

    auto build_dfs_postorder_rank = [&](bool reverse_roots, bool true_deps_first,
                                        bool consumer_clustered) {
      std::vector<int> finish_rank(static_cast<size_t>(m), m);
      std::vector<bool> visited(static_cast<size_t>(m), false);
      int next_rank = 0;
      auto dfs = [&](auto& self, int u) -> void {
        if (visited[static_cast<size_t>(u)]) {
          return;
        }
        visited[static_cast<size_t>(u)] = true;
        std::vector<int> true_preds;
        std::vector<int> other_preds;
        for (const Link& l : g_s1.log_links[static_cast<size_t>(u)]) {
          if (l.target < 0 || l.target >= m || l.target == u) {
            continue;
          }
          if (true_deps_first && l.kind == DepKind::kTrue) {
            true_preds.push_back(l.target);
          } else {
            other_preds.push_back(l.target);
          }
        }
        std::sort(true_preds.begin(), true_preds.end());
        std::sort(other_preds.begin(), other_preds.end());
        for (int p : true_preds) {
          self(self, p);
        }
        for (int p : other_preds) {
          self(self, p);
        }
        finish_rank[static_cast<size_t>(u)] = next_rank++;
      };
      if (consumer_clustered) {
        // Cluster hoisted leaf loads/constants directly before their first
        // consuming ALU/store/call instruction (`expr.c:4027` `expand_expr`)
        // while preserving the relative order of consumers.
        for (int u = 0; u < m; ++u) {
          if (true_indeg[static_cast<size_t>(u)] > 0 ||
              !has_true_consumer[static_cast<size_t>(u)]) {
            dfs(dfs, u);
          }
        }
        for (int u = 0; u < m; ++u) {
          dfs(dfs, u);
        }
      } else if (reverse_roots) {
        for (int u = m - 1; u >= 0; --u) {
          if (!has_consumer[static_cast<size_t>(u)]) {
            dfs(dfs, u);
          }
        }
        for (int u = m - 1; u >= 0; --u) {
          dfs(dfs, u);
        }
      } else {
        for (int u = 0; u < m; ++u) {
          if (!has_consumer[static_cast<size_t>(u)]) {
            dfs(dfs, u);
          }
        }
        for (int u = 0; u < m; ++u) {
          dfs(dfs, u);
        }
      }
      return finish_rank;
    };
    try_add_seed_rank(build_dfs_postorder_rank(/*reverse_roots=*/false, /*true_deps_first=*/true,
                                               /*consumer_clustered=*/true));
    try_add_seed_rank(build_dfs_postorder_rank(/*reverse_roots=*/false, /*true_deps_first=*/true,
                                               /*consumer_clustered=*/false));
    try_add_seed_rank(build_dfs_postorder_rank(/*reverse_roots=*/true, /*true_deps_first=*/true,
                                               /*consumer_clustered=*/false));

    std::vector<int> asap_depth(static_cast<size_t>(m), 0);
    for (int u = 0; u < m; ++u) {
      for (const Link& l : g_s1.log_links[static_cast<size_t>(u)]) {
        if (l.target >= 0 && l.target < u) {
          asap_depth[static_cast<size_t>(u)] = std::max(
              asap_depth[static_cast<size_t>(u)], asap_depth[static_cast<size_t>(l.target)] + 1);
        }
      }
    }
    std::vector<int> asap_order(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
      asap_order[static_cast<size_t>(i)] = i;
    }
    std::sort(asap_order.begin(), asap_order.end(), [&](int a, int b) {
      if (asap_depth[static_cast<size_t>(a)] != asap_depth[static_cast<size_t>(b)]) {
        return asap_depth[static_cast<size_t>(a)] < asap_depth[static_cast<size_t>(b)];
      }
      return a < b;
    });
    std::vector<int> asap_rank(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      asap_rank[static_cast<size_t>(asap_order[static_cast<size_t>(pos)])] = pos;
    }
    try_add_seed_rank(asap_rank);

    std::vector<int> dist_to_sink(static_cast<size_t>(m), 0);
    for (int u = m - 1; u >= 0; --u) {
      for (const Link& l : g_s1.log_links[static_cast<size_t>(u)]) {
        if (l.target >= 0 && l.target < u) {
          dist_to_sink[static_cast<size_t>(l.target)] =
              std::max(dist_to_sink[static_cast<size_t>(l.target)],
                       dist_to_sink[static_cast<size_t>(u)] + 1);
        }
      }
    }
    std::vector<int> crit_order(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
      crit_order[static_cast<size_t>(i)] = i;
    }
    std::sort(crit_order.begin(), crit_order.end(), [&](int a, int b) {
      if (dist_to_sink[static_cast<size_t>(a)] != dist_to_sink[static_cast<size_t>(b)]) {
        return dist_to_sink[static_cast<size_t>(a)] > dist_to_sink[static_cast<size_t>(b)];
      }
      return a < b;
    });
    std::vector<int> crit_rank(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      crit_rank[static_cast<size_t>(crit_order[static_cast<size_t>(pos)])] = pos;
    }
    try_add_seed_rank(crit_rank);
  }
  std::vector<std::vector<int>> seed_conflict_out;
  if (zero_viol_seeds.empty() && best_seed_for_conflict.has_value()) {
    CollectConflictDrivenSched2Orders(
        view, g_s1, best_seed_for_conflict->res_seed, dag_edges, hard_next, hard_prev,
        best_seed_for_conflict->seed_input, combined_s2_edges, mm, max_orders,
        /*non_zero_cap=*/std::max(1, max_orders / 3), &seen_orders, &seed_conflict_out);
  }
  // Assemble `out` in strict `combined_s2_edges` quality order:
  //   (1) zero-violation orders (from conflict-driven runs and canonical seeds),
  //   (2) lowest-violation partial states (leaving room for BFS tie-flips if needed).
  std::vector<std::pair<int, std::vector<int>>> ranked_candidates;
  for (std::vector<int>& ord : zero_viol_seeds) {
    ranked_candidates.emplace_back(0, std::move(ord));
  }
  for (std::vector<int>& ord : seed_conflict_out) {
    const int v = count_s2_violations(ord);
    ranked_candidates.emplace_back(v, std::move(ord));
  }
  for (std::vector<int>& ord : out) {
    const int v = count_s2_violations(ord);
    ranked_candidates.emplace_back(v, std::move(ord));
  }
  for (auto& [v, ord] : non_zero_seeds) {
    ranked_candidates.emplace_back(v, std::move(ord));
  }
  std::stable_sort(ranked_candidates.begin(), ranked_candidates.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  out.clear();
  // Always reserve room for local `extract_ties` BFS flips around
  // `first_div_step` (`sched.c:2428` `rank_for_schedule`) so global canonical
  // seeds never starve single-step tie resolution on near-converged blocks.
  const int bfs_reserve = std::min(max_orders / 2, std::max(4, max_orders / 3));
  const int seed_budget = std::max(1, max_orders - bfs_reserve);
  for (auto& [v, ord] : ranked_candidates) {
    if (static_cast<int>(out.size()) >= seed_budget) {
      break;
    }
    out.push_back(std::move(ord));
  }

  constexpr int kMaxTieFlipDepth = 3;
  constexpr int kMaxBfsVisits = 24;
  struct BfsState {
    absl::btree_set<std::pair<int, int>> extra_edges;
    int depth = 0;
  };
  std::deque<BfsState> worklist;
  absl::btree_set<absl::btree_set<std::pair<int, int>>> visited_edge_sets;
  visited_edge_sets.insert({});
  for (const auto& [u, v] : extract_ties(base_res, identity_rank, /*branch_limit=*/8)) {
    absl::btree_set<std::pair<int, int>> e = {{u, v}};
    if (visited_edge_sets.insert(e).second) {
      worklist.push_back(BfsState{.extra_edges = std::move(e), .depth = 1});
    }
  }

  int visits = 0;
  while (!worklist.empty() && static_cast<int>(out.size()) < max_orders && visits < kMaxBfsVisits) {
    BfsState st = std::move(worklist.front());
    worklist.pop_front();
    ++visits;

    absl::btree_set<std::pair<int, int>> edges = dag_edges;
    edges.insert(st.extra_edges.begin(), st.extra_edges.end());
    const std::optional<std::vector<int>> flipped_input =
        SolveSupernodeKahn(m, edges, hard_next, hard_prev, identity_rank);
    if (!flipped_input.has_value()) {
      continue;
    }
    std::vector<Insn> flipped_core;
    flipped_core.reserve(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      Insn insn = view.core_insns[static_cast<size_t>((*flipped_input)[static_cast<size_t>(pos)])];
      insn.luid = pos;
      flipped_core.push_back(std::move(insn));
    }
    const DepGraph g_flip = Analyze(flipped_core, view.facts_s1, mm, Phase::kSched1);
    const ScheduleResult res_flip =
        Schedule(flipped_core, g_flip, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
    if (static_cast<int>(res_flip.order.size()) != m) {
      continue;
    }
    std::vector<int> mapped(static_cast<size_t>(m));
    for (int pos = 0; pos < m; ++pos) {
      mapped[static_cast<size_t>(pos)] =
          (*flipped_input)[static_cast<size_t>(res_flip.order[static_cast<size_t>(pos)])];
    }
    if (seen_orders.insert(mapped).second) {
      out.push_back(std::move(mapped));
    }
    if (st.depth < kMaxTieFlipDepth && static_cast<int>(out.size()) < max_orders) {
      for (const auto& [u, v] : extract_ties(res_flip, *flipped_input, /*branch_limit=*/4)) {
        if (st.extra_edges.contains({u, v}) || st.extra_edges.contains({v, u})) {
          continue;
        }
        absl::btree_set<std::pair<int, int>> nxt_edges = st.extra_edges;
        nxt_edges.insert({u, v});
        if (visited_edge_sets.insert(nxt_edges).second) {
          worklist.push_back(BfsState{.extra_edges = std::move(nxt_edges), .depth = st.depth + 1});
        }
      }
    }
  }
  return out;
}

std::vector<std::vector<int>> CollectSched1SunkConsumerHoistCoreOrders(
    const Sched1ProjectionView& view, const MachineModel& mm, int max_orders) {
  std::vector<std::vector<int>> out;
  const int m = static_cast<int>(view.core_insns.size());
  if (m <= 2 || max_orders <= 0) {
    return out;
  }
  const DepGraph dg = Analyze(view.core_insns, view.facts_s1, mm, Phase::kSched1);
  const ScheduleResult sr =
      Schedule(view.core_insns, dg, view.facts_s1, mm, Phase::kSched1, view.live_at_end_s1);
  if (static_cast<int>(sr.order.size()) != m) {
    return out;
  }

  std::vector<int> sched_pos(static_cast<size_t>(m), -1);
  for (int pos = 0; pos < m; ++pos) {
    const int u = sr.order[static_cast<size_t>(pos)];
    if (u >= 0 && u < m) {
      sched_pos[static_cast<size_t>(u)] = pos;
    }
  }

  std::vector<int> min_true_cons(static_cast<size_t>(m), -1);
  std::vector<int> min_any_cons(static_cast<size_t>(m), -1);
  for (int c = 0; c < m; ++c) {
    for (const Link& lk : dg.log_links[static_cast<size_t>(c)]) {
      if (lk.target < 0 || lk.target >= c) {
        continue;
      }
      if (min_any_cons[static_cast<size_t>(lk.target)] < 0) {
        min_any_cons[static_cast<size_t>(lk.target)] = c;
      }
      if (lk.kind == DepKind::kTrue && min_true_cons[static_cast<size_t>(lk.target)] < 0) {
        min_true_cons[static_cast<size_t>(lk.target)] = c;
      }
    }
  }

  auto make_hoist_order = [&](int consumer, int target_pos) -> std::optional<std::vector<int>> {
    if (target_pos < 0 || target_pos >= consumer || consumer >= m) {
      return std::nullopt;
    }
    std::vector<bool> in_closure(static_cast<size_t>(m), false);
    in_closure[static_cast<size_t>(consumer)] = true;
    for (int u = consumer; u > target_pos; --u) {
      if (!in_closure[static_cast<size_t>(u)]) {
        continue;
      }
      for (const Link& lk : dg.log_links[static_cast<size_t>(u)]) {
        if (lk.target >= target_pos && lk.target < u) {
          in_closure[static_cast<size_t>(lk.target)] = true;
        }
      }
    }
    if (in_closure[static_cast<size_t>(target_pos)]) {
      return std::nullopt;
    }
    std::vector<int> ord;
    ord.reserve(static_cast<size_t>(m));
    for (int u = 0; u < target_pos; ++u) {
      ord.push_back(u);
    }
    for (int u = target_pos; u <= consumer; ++u) {
      if (in_closure[static_cast<size_t>(u)]) {
        ord.push_back(u);
      }
    }
    for (int u = target_pos; u <= consumer; ++u) {
      if (!in_closure[static_cast<size_t>(u)]) {
        ord.push_back(u);
      }
    }
    for (int u = consumer + 1; u < m; ++u) {
      ord.push_back(u);
    }
    return ord;
  };

  absl::btree_set<std::vector<int>> seen;
  auto emit = [&](int consumer, int target_pos) {
    if (static_cast<int>(out.size()) >= max_orders) {
      return;
    }
    std::optional<std::vector<int>> ord = make_hoist_order(consumer, target_pos);
    if (ord.has_value() && seen.insert(*ord).second) {
      out.push_back(std::move(*ord));
    }
  };

  for (int p = 0; p < m && static_cast<int>(out.size()) < max_orders; ++p) {
    const int sp = sched_pos[static_cast<size_t>(p)];
    if (sp < 0) {
      continue;
    }
    const int c_p = min_true_cons[static_cast<size_t>(p)] >= 0
                        ? min_true_cons[static_cast<size_t>(p)]
                        : min_any_cons[static_cast<size_t>(p)];
    if (c_p < 0) {
      continue;
    }
    int g_min = -1;
    int min_c_g = -1;
    for (int g = p + 1; g < m; ++g) {
      if (sched_pos[static_cast<size_t>(g)] >= 0 && sched_pos[static_cast<size_t>(g)] < sp) {
        if (g_min < 0) {
          g_min = g;
        }
        const int c_g = min_true_cons[static_cast<size_t>(g)] >= 0
                            ? min_true_cons[static_cast<size_t>(g)]
                            : min_any_cons[static_cast<size_t>(g)];
        if (c_g >= 0 && (min_c_g < 0 || c_g < min_c_g)) {
          min_c_g = c_g;
        }
      }
    }
    if (g_min < 0) {
      continue;
    }
    if (min_c_g >= 0 && c_p > min_c_g) {
      emit(c_p, min_c_g);
      if (g_min > p && g_min < min_c_g) {
        emit(c_p, g_min);
      }
    } else if (c_p > g_min) {
      emit(c_p, g_min);
    }
  }
  return out;
}

std::vector<std::vector<int>> CollectSched1704CallHoistCandidatePrefs(
    absl::Span<const Insn> cur_seq, int prologue_end, int epilogue_begin,
    const Sched1ProjectionView& cur_view, const RegFacts& facts_s2, const MachineModel& mm) {
  std::vector<std::vector<int>> prefs;
  const int n = static_cast<int>(cur_seq.size());
  const int m = static_cast<int>(cur_view.core_insns.size());
  if (n <= 2 || m <= 1 || prologue_end < 0 || epilogue_begin > n ||
      epilogue_begin <= prologue_end) {
    return prefs;
  }
  const std::vector<int> s1_order = ScheduleSched1ViewCoreOrder(cur_view, mm);
  if (static_cast<int>(s1_order.size()) != m) {
    return prefs;
  }
  std::vector<int> s1_pos(static_cast<size_t>(m), -1);
  for (int pos = 0; pos < m; ++pos) {
    s1_pos[static_cast<size_t>(s1_order[static_cast<size_t>(pos)])] = pos;
  }

  const DepGraph g_s2 = AnalyzeWithoutPendingListFlushCap(cur_seq, facts_s2, mm, Phase::kSched2);
  std::vector<int> call_seq_indices;
  for (int ci = 0; ci < m; ++ci) {
    if (cur_view.core_insns[static_cast<size_t>(ci)].kind == Insn::Kind::kCall) {
      call_seq_indices.push_back(prologue_end +
                                 cur_view.core_to_orig_index[static_cast<size_t>(ci)]);
    }
  }
  if (call_seq_indices.empty()) {
    return prefs;
  }

  // Maps call_seq_idx -> list of callee-saved insn seq indices to hoist immediately before it.
  absl::btree_map<int, std::vector<int>> displaced_by_call;
  absl::btree_map<int, std::vector<int>> all_by_call;
  for (int ci = 0; ci < m; ++ci) {
    const int seq_i = prologue_end + cur_view.core_to_orig_index[static_cast<size_t>(ci)];
    if (seq_i <= call_seq_indices.front() ||
        cur_view.core_insns[static_cast<size_t>(ci)].kind == Insn::Kind::kCall) {
      continue;
    }
    int prev_call = -1;
    for (int c_seq : call_seq_indices) {
      if (c_seq < seq_i) {
        prev_call = c_seq;
      } else {
        break;
      }
    }
    if (prev_call < 0 || !IsPureSched1704CallArtifactEdge(cur_seq, g_s2, prev_call, seq_i, mm)) {
      continue;
    }
    // Verify `seq_i` has no semantic `kSched2` dependency on `prev_call` or any
    // instruction in `(prev_call, seq_i)` (excluding `sched.c:1704` artifacts).
    bool has_post_call_dep = false;
    for (const Link& l : g_s2.log_links[static_cast<size_t>(seq_i)]) {
      if (l.target < prev_call || l.target >= seq_i) {
        continue;
      }
      if (l.target == prev_call &&
          IsPureSched1704CallArtifactEdge(cur_seq, g_s2, prev_call, seq_i, mm)) {
        continue;
      }
      has_post_call_dep = true;
      break;
    }
    if (has_post_call_dep) {
      continue;
    }
    all_by_call[prev_call].push_back(seq_i);
    if (s1_pos[static_cast<size_t>(ci)] != ci) {
      displaced_by_call[prev_call].push_back(seq_i);
    }
  }

  auto build_hoisted_pref = [&](const absl::btree_map<int, std::vector<int>>& by_call) {
    if (by_call.empty()) {
      return;
    }
    absl::btree_set<int> hoisted_set;
    for (const auto& [c_seq, items] : by_call) {
      for (int s : items) {
        hoisted_set.insert(s);
      }
    }
    std::vector<int> pref;
    pref.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      if (hoisted_set.contains(pos)) {
        continue;
      }
      if (const auto it = by_call.find(pos); it != by_call.end()) {
        for (int s : it->second) {
          pref.push_back(s);
        }
      }
      pref.push_back(pos);
    }
    if (static_cast<int>(pref.size()) == n &&
        std::find(prefs.begin(), prefs.end(), pref) == prefs.end()) {
      prefs.push_back(std::move(pref));
    }
  };
  constexpr int kMaxIndividualHoists = 6;
  int individual_emitted = 0;
  for (const auto& [c_seq, items] : displaced_by_call) {
    for (int s : items) {
      if (individual_emitted >= kMaxIndividualHoists) {
        break;
      }
      absl::btree_map<int, std::vector<int>> single;
      single[c_seq] = {s};
      build_hoisted_pref(single);
      ++individual_emitted;
    }
    if (individual_emitted >= kMaxIndividualHoists) {
      break;
    }
  }
  build_hoisted_pref(displaced_by_call);
  build_hoisted_pref(all_by_call);
  return prefs;
}

namespace {

struct Stage0MemEquivAddition {
  int target_sched_idx = -1;
  RtlRegNote note;
};

// Collects `REG_EQUIV (mem ...)` notes (`local-alloc.c:540-665, 990-1000, 1049-1055`)
// that were valid in the pre-`sched2` candidate order `cand_seq` (`mem_equiv_valid == 1`)
// but obstructed in the post-`sched2` order `base_block` (`mem_equiv_valid == 0`) by
// `sched2` reordering an independent store, load, or call across the live range.
std::vector<Stage0MemEquivAddition> CollectSched2ObstructedMemRegEquivAdditions(
    absl::Span<const Insn> base_block, absl::Span<const Insn> cand_seq,
    absl::Span<const int> cand_in_base, int prologue_end, int epilogue_begin,
    const absl::btree_set<int>& live_at_end_s2, const RegFacts& facts_s2, const MachineModel& mm,
    const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_prologue_body_indices,
    const absl::btree_set<int>& split_rmw_orig_ids = {}) {
  std::vector<Stage0MemEquivAddition> additions;
  const int n = static_cast<int>(base_block.size());
  std::vector<int> base_ids(static_cast<size_t>(n));
  std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
  for (int i = 0; i < n; ++i) {
    base_ids[static_cast<size_t>(i)] = i;
    pos_in_cand[static_cast<size_t>(cand_in_base[static_cast<size_t>(i)])] = i;
  }
  absl::btree_set<int> excluded_prologue_orig_ids;
  for (int i = 0; i < prologue_end; ++i) {
    excluded_prologue_orig_ids.insert(i);
  }
  for (int rel : excluded_prologue_body_indices) {
    excluded_prologue_orig_ids.insert(prologue_end + rel);
  }

  // Identify matched intra-block local-frame spill/caller-save stack slots so we
  // never synthesize `REG_EQUIV` notes for `reload1.c`/`caller-save.c` spills.
  // Mirrors `reload1.c:2352-2358` + `function.c:682-704`: exclude local stack
  // arrays/aggregates below `min_valid_reload_spill_offset`.
  const int64_t min_spill_off =
      stack_layout.valid
          ? std::max(stack_layout.starting_frame_offset, stack_layout.min_valid_reload_spill_offset)
          : int64_t{0};
  absl::btree_map<StackSlotKey, int> first_store_pos;
  absl::btree_set<StackSlotKey> matched_spill_slots;
  for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
    const int oid = cand_in_base[static_cast<size_t>(pos)];
    if (excluded_prologue_orig_ids.contains(oid)) {
      continue;
    }
    if (auto acc = TryParsePlainStackSlotMove(cand_seq[static_cast<size_t>(pos)]);
        acc.has_value()) {
      const bool in_spill_frame =
          !stack_layout.valid ||
          (acc->key.offset >= min_spill_off && acc->key.offset < stack_layout.frame_size);
      if (!in_spill_frame) {
        continue;
      }
      if (acc->is_store) {
        first_store_pos.try_emplace(acc->key, pos);
      } else if (const auto it = first_store_pos.find(acc->key);
                 it != first_store_pos.end() && it->second < pos) {
        matched_spill_slots.insert(acc->key);
      }
    }
  }

  // Identify defs in `cand_seq` directly feeding a `CALL_INSN`'s `call_fusage`
  // (hard regs `< FIRST_PSEUDO_REGISTER` in `.08.lreg`, skipped by `local-alloc.c:1020`).
  std::vector<absl::btree_set<int>> call_arg_defs_in_cand(static_cast<size_t>(n));
  for (int ci = prologue_end; ci < epilogue_begin; ++ci) {
    if (cand_seq[static_cast<size_t>(ci)].kind != Insn::Kind::kCall) {
      continue;
    }
    absl::btree_set<int> needed;
    for (const RtlExpr& fu : cand_seq[static_cast<size_t>(ci)].pn.call_fusage) {
      CollectRegReads(fu, &needed);
    }
    for (int prev = ci - 1; prev >= prologue_end && !needed.empty(); --prev) {
      if (cand_seq[static_cast<size_t>(prev)].kind == Insn::Kind::kCall) {
        break;
      }
      const InsnRegReadDefDeaths info =
          CollectInsnRegReadDefDeaths(cand_seq[static_cast<size_t>(prev)], mm);
      for (int d : info.defs) {
        if (needed.contains(d)) {
          call_arg_defs_in_cand[static_cast<size_t>(prev)].insert(d);
          needed.erase(d);
        }
      }
    }
  }

  const HardRegLivenessSummary base_sum = ComputeHardRegLivenessSummary(
      base_block, base_ids, live_at_end_s2, facts_s2, mm, stack_layout, excluded_prologue_orig_ids);
  const HardRegLivenessSummary cand_sum =
      ComputeHardRegLivenessSummary(cand_seq, cand_in_base, live_at_end_s2, facts_s2, mm,
                                    stack_layout, excluded_prologue_orig_ids);

  auto is_allocatable_hard_reg = [&](int r) {
    return (r >= 2 && r <= 25) ||
           (r == kMipsFramePointerRegno && !stack_layout.frame_pointer_needed) ||
           (r >= kMipsFpRegFirst && r <= kMipsFpRegLast);
  };
  auto is_matched_spill_mem = [&](const Insn& insn) {
    if (auto acc = TryParsePlainStackSlotMove(insn); acc.has_value()) {
      return matched_spill_slots.contains(acc->key);
    }
    return false;
  };

  for (const auto& [oid, cv] : cand_sum.mem_equiv_valid) {
    if (!cv || oid < prologue_end || oid >= epilogue_begin ||
        excluded_prologue_orig_ids.contains(oid)) {
      continue;
    }
    const auto bit = base_sum.mem_equiv_valid.find(oid);
    const bool bv = (bit != base_sum.mem_equiv_valid.end()) && bit->second;
    const Insn& base_insn = base_block[static_cast<size_t>(oid)];
    const RtlExpr& pat = base_insn.pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || is_matched_spill_mem(base_insn)) {
      continue;
    }
    const int cpos = pos_in_cand[static_cast<size_t>(oid)];
    if (cpos < prologue_end || cpos >= epilogue_begin) {
      continue;
    }

    // (a) Load-equiv (`local-alloc.c:1049-1055`): `(set (reg R) (mem M))`.
    // `ValidateEquivMemForPostReloadGate` already requires `REG_DEAD R` inside
    // block `b`, proving this live range ends within the single basic block even
    // if hard reg `R` (e.g. `$v0`) is later redefined for a live-out value.
    if (pat.SetDest().op == RtlOp::kReg && !pat.SetDest().IsArgPointerReg() &&
        !pat.SetDest().flags.integrated && !pat.SetDest().flags.frame_related &&
        pat.SetSrc().op == RtlOp::kMem && !pat.SetSrc().children.empty() &&
        !pat.SetSrc().flags.volatil) {
      const int r = pat.SetDest().RegNo();
      bool death_is_split_rmw = false;
      if (bv && !split_rmw_orig_ids.empty()) {
        // When `SelectMinimalCombineRegsRmwSplits` splits a self-RMW terminating
        // `r`'s live range, `SynthesizePostReloadMemRegEquivNotes` previously
        // skipped `oid` as a multi-set pseudo even if `base_sum` scanned past
        // the unsplit RMW (`local-alloc.c:1021, 1049-1055`).
        for (int q = cpos + 1; q < epilogue_begin; ++q) {
          const int q_orig = cand_in_base[static_cast<size_t>(q)];
          if (excluded_prologue_orig_ids.contains(q_orig)) {
            continue;
          }
          if (HasRegDeadNoteCovering(cand_seq[static_cast<size_t>(q)].pn.reg_notes, r, mm)) {
            death_is_split_rmw = split_rmw_orig_ids.contains(q_orig);
            break;
          }
          if (CollectInsnRegReadDefDeaths(cand_seq[static_cast<size_t>(q)], mm).defs.contains(r)) {
            break;
          }
        }
      }
      if (bv && !death_is_split_rmw) {
        continue;
      }
      if (!is_allocatable_hard_reg(r) ||
          call_arg_defs_in_cand[static_cast<size_t>(cpos)].contains(r)) {
        continue;
      }
      bool has_existing = false;
      for (const RtlRegNote& note : base_insn.pn.reg_notes) {
        if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual ||
            note.kind == RtlMachineMode::kRegLibcall || note.kind == RtlMachineMode::kRegRetval) {
          has_existing = true;
          break;
        }
      }
      if (!has_existing) {
        additions.push_back(Stage0MemEquivAddition{
            .target_sched_idx = oid,
            .note =
                RtlRegNote{
                    .kind = RtlMachineMode::kRegEquiv,
                    .list_op = RtlOp::kExprList,
                    .datum = pat.SetSrc(),
                },
        });
      }
      continue;
    }

    // (b) Store-equiv (`local-alloc.c:990-1000`): `(set (mem M) (reg R))`.
    if (pat.SetDest().op == RtlOp::kMem && !pat.SetDest().children.empty() &&
        !pat.SetDest().flags.volatil && pat.SetSrc().op == RtlOp::kReg &&
        !pat.SetSrc().IsArgPointerReg() && !pat.SetSrc().flags.integrated) {
      const int r = pat.SetSrc().RegNo();
      if (!is_allocatable_hard_reg(r)) {
        continue;
      }
      const int init_pos = FindUniqueInitPosForStoreInSeq(cand_seq, cpos, r, mm);
      if (init_pos < prologue_end || init_pos >= cpos) {
        continue;
      }
      const int init_orig = cand_in_base[static_cast<size_t>(init_pos)];
      if (bv && !split_rmw_orig_ids.contains(init_orig)) {
        continue;
      }
      if (excluded_prologue_orig_ids.contains(init_orig) ||
          call_arg_defs_in_cand[static_cast<size_t>(init_pos)].contains(r)) {
        continue;
      }
      const Insn& init_insn = base_block[static_cast<size_t>(init_orig)];
      const RtlExpr& ipat = init_insn.pn.pattern;
      // `local-alloc.c:990` (`update_equiv_regs`) requires `GET_CODE (SET_SRC (set)) == REG`
      // on the store; when the store's source mode differs from the initializing `SET_DEST`
      // mode (e.g. `(reg:HI r)` stored from `(set (reg:SI r) ...)`), pre-reload RTL used a
      // `(subreg:HI (reg:SI P) 0)` (`alter_subreg`), so `local-alloc.c:990` never attaches
      // a store-`REG_EQUIV` note.
      if (ipat.op != RtlOp::kSet || ipat.children.size() != 2 || ipat.SetDest().op != RtlOp::kReg ||
          ipat.SetDest().mode != pat.SetSrc().mode || ipat.SetDest().flags.integrated ||
          ipat.SetDest().flags.frame_related) {
        continue;
      }
      if (HasMatchingMemRegEquivNote(init_insn, pat.SetDest(), stack_layout)) {
        continue;
      }
      additions.push_back(Stage0MemEquivAddition{
          .target_sched_idx = init_orig,
          .note =
              RtlRegNote{
                  .kind = RtlMachineMode::kRegEquiv,
                  .list_op = RtlOp::kExprList,
                  .datum = UneliminateMemForRegEquivNoteIfNeeded(pat.SetDest(), ipat, stack_layout),
              },
      });
    }
  }
  return additions;
}

// Reorders `base_block` by `cand_in_base`, and when `transfer_pure_reader_dead`
// is true (Stage 0 discovery), mirrors `AttributeSched2PureReaderRegDeadNotes`
// (`flow.c:2504-2515`, `sched.c:3415-3611`) by transferring pure-reader
// `(REG_DEAD r)` notes from `i` to the pre-`sched2` last reader `last_reader`
// (`last_reader < i` in `base_block` and `pos_in_cand[i] < pos_in_cand[last_reader]`)
// so `ProjectPostReloadBlockToSched1View` never projects stale `sched2`-order deaths.
std::vector<Insn> BuildTransferredCandSeqForStage0Projection(absl::Span<const Insn> base_block,
                                                             absl::Span<const int> cand_in_base,
                                                             int prologue_end, int epilogue_begin,
                                                             const MachineModel& mm,
                                                             bool transfer_pure_reader_dead) {
  const int n = static_cast<int>(base_block.size());
  std::vector<Insn> cand_seq;
  cand_seq.reserve(static_cast<size_t>(n));
  for (int pos = 0; pos < n; ++pos) {
    Insn insn = base_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
    insn.luid = pos;
    cand_seq.push_back(std::move(insn));
  }
  if (!transfer_pure_reader_dead || epilogue_begin <= prologue_end) {
    return cand_seq;
  }
  std::vector<InsnRegReadDefDeaths> insn_info(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    insn_info[static_cast<size_t>(i)] =
        CollectInsnRegReadDefDeaths(base_block[static_cast<size_t>(i)], mm);
  }
  std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
  for (int pos = 0; pos < n; ++pos) {
    pos_in_cand[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])] = pos;
  }
  struct PureTransfer {
    int from_pos = -1;
    size_t note_idx = 0;
    int to_pos = -1;
    RtlRegNote note;
  };
  std::vector<PureTransfer> transfers;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    if (insn_info[static_cast<size_t>(i)].is_call_or_use) {
      continue;
    }
    const auto& notes = base_block[static_cast<size_t>(i)].pn.reg_notes;
    for (size_t ni = 0; ni < notes.size(); ++ni) {
      const RtlRegNote& note = notes[ni];
      if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
        continue;
      }
      const int r = note.datum.RegNo();
      if (r < 0 || r >= kMipsFirstPseudoRegister) {
        continue;
      }
      const int nr = mm.HardRegNRegs(r, note.datum.mode);
      bool i_defs_r = false;
      for (int k = 0; k < nr; ++k) {
        if (insn_info[static_cast<size_t>(i)].defs.contains(r + k)) {
          i_defs_r = true;
          break;
        }
      }
      if (i_defs_r) {
        continue;
      }
      int last_reader = -1;
      for (int p = pos_in_cand[static_cast<size_t>(i)] + 1; p < epilogue_begin; ++p) {
        const int m = cand_in_base[static_cast<size_t>(p)];
        bool m_defs_r = false;
        for (int k = 0; k < nr; ++k) {
          if (insn_info[static_cast<size_t>(m)].defs.contains(r + k)) {
            m_defs_r = true;
            break;
          }
        }
        if (m_defs_r) {
          break;
        }
        bool m_reads_all = true;
        for (int k = 0; k < nr; ++k) {
          if (!insn_info[static_cast<size_t>(m)].reads.contains(r + k)) {
            m_reads_all = false;
            break;
          }
        }
        if (m_reads_all) {
          last_reader = m;
        }
      }
      if (last_reader < 0 || last_reader >= i ||
          insn_info[static_cast<size_t>(last_reader)].is_call_or_use) {
        continue;
      }
      bool already_dead = false;
      for (int k = 0; k < nr; ++k) {
        if (insn_info[static_cast<size_t>(last_reader)].deaths.contains(r + k)) {
          already_dead = true;
          break;
        }
      }
      if (already_dead) {
        continue;
      }
      bool intervening_def = false;
      const int lo = std::min(i, last_reader);
      const int hi = std::max(i, last_reader);
      for (int k = lo + 1; k < hi && !intervening_def; ++k) {
        for (int w = 0; w < nr; ++w) {
          if (insn_info[static_cast<size_t>(k)].defs.contains(r + w)) {
            intervening_def = true;
            break;
          }
        }
      }
      if (!intervening_def) {
        transfers.push_back(PureTransfer{
            .from_pos = pos_in_cand[static_cast<size_t>(i)],
            .note_idx = ni,
            .to_pos = pos_in_cand[static_cast<size_t>(last_reader)],
            .note = note,
        });
      }
    }
  }
  if (transfers.empty()) {
    return cand_seq;
  }
  absl::btree_map<int, absl::btree_set<size_t>> erase_by_pos;
  for (const PureTransfer& tr : transfers) {
    erase_by_pos[tr.from_pos].insert(tr.note_idx);
  }
  for (const auto& [from_pos, idxs] : erase_by_pos) {
    auto& notes = cand_seq[static_cast<size_t>(from_pos)].pn.reg_notes;
    std::vector<RtlRegNote> kept;
    kept.reserve(notes.size());
    for (size_t ni = 0; ni < notes.size(); ++ni) {
      if (!idxs.contains(ni)) {
        kept.push_back(std::move(notes[ni]));
      }
    }
    notes = std::move(kept);
  }
  for (const PureTransfer& tr : transfers) {
    cand_seq[static_cast<size_t>(tr.to_pos)].pn.reg_notes.push_back(tr.note);
  }
  return cand_seq;
}

// Runs bounded fixed-point discovery (`<= 4` rounds) for Stage 0 `sched2` preimage
// candidates under `allow_pure_reader_reg_dead_transfer=true`, optionally hoisting
// displaced block-0 entry-parameter copies (`sched.c:3191-3215`) on a second pass.
std::vector<std::vector<int>> FindStage0Sched2PreimageCandidates(
    absl::Span<const Insn> base_block, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionView& base_view, const Sched1ProjectionContext& proj_ctx,
    const CfgSched1Context& cfg_ctx, const RegFacts& facts_s2, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, absl::Span<const AdjacentPair> reload_adjacent,
    bool allow_pure_reader_reg_dead_transfer = true, bool allow_tie_flips = true,
    absl::Span<const int> initial_seed_in_base = {},
    bool allow_moved_store_load_equiv_invalidation = false) {
  std::vector<std::vector<int>> primary_candidates;
  std::vector<std::vector<int>> tie_candidates;
  const int n = static_cast<int>(base_block.size());
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  const bool fp_needed = !proj_ctx.stack_layout.valid || proj_ctx.stack_layout.frame_pointer_needed;
  constexpr int kMaxRounds = 4;

  const bool has_custom_seed = (static_cast<int>(initial_seed_in_base.size()) == n);
  Sched1ProjectionView seeded_view;
  absl::btree_set<int> seeded_excluded;
  if (has_custom_seed) {
    const std::vector<Insn> seed_seq = BuildTransferredCandSeqForStage0Projection(
        base_block, initial_seed_in_base, prologue_end, epilogue_begin, mm,
        allow_pure_reader_reg_dead_transfer);
    std::vector<Insn> seed_body;
    seed_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      Insn insn = seed_seq[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      seed_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext seed_ctx = proj_ctx;
    seeded_excluded = FindInterleavedPrologueSaveBodyIndices(seed_seq, bounds, block_idx, mm);
    seed_ctx.excluded_prologue_body_indices = seeded_excluded;
    seeded_view = ProjectPostReloadBlockToSched1View(seed_body, mm, live_at_end_s2, seed_ctx);
  }

  auto run_pass = [&](bool hoist_b0_args, bool run_tie_flips, bool use_closest_s1 = false) -> bool {
    std::vector<int> cur_to_base(static_cast<size_t>(n));
    if (has_custom_seed) {
      cur_to_base.assign(initial_seed_in_base.begin(), initial_seed_in_base.end());
    } else {
      for (int i = 0; i < n; ++i) {
        cur_to_base[static_cast<size_t>(i)] = i;
      }
    }
    Sched1ProjectionView cur_view = has_custom_seed ? seeded_view : base_view;
    absl::btree_set<int> cur_excluded =
        has_custom_seed ? seeded_excluded : proj_ctx.excluded_prologue_body_indices;
    absl::btree_set<std::vector<int>> tried_prefs;

    for (int round = 0; round < kMaxRounds; ++round) {
      std::vector<Insn> cur_seq = BuildTransferredCandSeqForStage0Projection(
          base_block, cur_to_base, prologue_end, epilogue_begin, mm,
          allow_pure_reader_reg_dead_transfer);
      std::vector<int> extra_pinned;
      if (hoist_b0_args && block_idx == 0) {
        std::vector<Insn> cur_body;
        cur_body.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          Insn insn = cur_seq[static_cast<size_t>(pos)];
          insn.luid = pos - prologue_end;
          cur_body.push_back(std::move(insn));
        }
        extra_pinned = CollectDisplacedBlock0EntryArgCopyBodyIndices(cur_body, cur_view,
                                                                     cur_excluded, mm, fp_needed);
        if (round == 0 && extra_pinned.empty()) {
          return false;
        }
      }
      std::vector<int> closest_s1;
      if (use_closest_s1) {
        closest_s1 = ScheduleClosestSched1CoreOrder(cur_view, mm);
      }
      std::vector<int> pref_in_cur;
      if (!BuildPreferredSched1OrderInBase(cur_seq, prologue_end, epilogue_begin, cur_view,
                                           cur_excluded, mm, &pref_in_cur, extra_pinned,
                                           closest_s1)) {
        break;
      }
      std::vector<int> pref_in_base(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        pref_in_base[static_cast<size_t>(pos)] =
            cur_to_base[static_cast<size_t>(pref_in_cur[static_cast<size_t>(pos)])];
      }
      if (!tried_prefs.insert(pref_in_base).second) {
        break;
      }
      const std::optional<std::vector<int>> cand_in_base = InvertBlockOrder(
          base_block, facts_s2, mm, Phase::kSched2, live_at_end_s2, pref_in_base, reload_adjacent);
      if (!cand_in_base.has_value()) {
        break;
      }
      bool differs = false;
      for (int pos = 0; pos < n; ++pos) {
        if ((*cand_in_base)[static_cast<size_t>(pos)] != pos) {
          differs = true;
          break;
        }
      }
      if (!differs) {
        break;
      }
      if (!PreservesPostReloadOrderSafety(base_block, *cand_in_base, prologue_end, epilogue_begin,
                                          live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                                          allow_pure_reader_reg_dead_transfer, cfg_ctx.stack_layout,
                                          proj_ctx.excluded_prologue_body_indices,
                                          allow_moved_store_load_equiv_invalidation)) {
        break;
      }
      if (!run_tie_flips) {
        primary_candidates.push_back(*cand_in_base);
      }

      std::vector<Insn> cand_seq = BuildTransferredCandSeqForStage0Projection(
          base_block, *cand_in_base, prologue_end, epilogue_begin, mm,
          allow_pure_reader_reg_dead_transfer);
      std::vector<Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        Insn insn = cand_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        cand_body.push_back(std::move(insn));
      }
      Sched1ProjectionContext cand_ctx = proj_ctx;
      cand_ctx.excluded_prologue_body_indices =
          FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, block_idx, mm);
      Sched1ProjectionView cand_view =
          ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
      if (cand_view.core_insns.size() <= 1 ||
          IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm, Phase::kSched1,
                       cand_view.live_at_end_s1)) {
        return true;
      }
      cur_to_base = *cand_in_base;
      cur_excluded = std::move(cand_ctx.excluded_prologue_body_indices);
      cur_view = std::move(cand_view);
    }

    if (!run_tie_flips) {
      return false;
    }

    // Bounded `sched.c:1704` callee-saved call-hoist and equal-priority `kSched1`
    // LUID tie-flip search (`sched.c:2428` `rank_for_schedule`) when the default
    // LUID tie-breaks did not reach `Im(F_sched1)`.
    std::vector<Insn> cur_seq = BuildTransferredCandSeqForStage0Projection(
        base_block, cur_to_base, prologue_end, epilogue_begin, mm,
        allow_pure_reader_reg_dead_transfer);
    std::vector<int> extra_pinned;
    if (hoist_b0_args && block_idx == 0) {
      std::vector<Insn> cur_body;
      cur_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        Insn insn = cur_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        cur_body.push_back(std::move(insn));
      }
      extra_pinned = CollectDisplacedBlock0EntryArgCopyBodyIndices(cur_body, cur_view, cur_excluded,
                                                                   mm, fp_needed);
    }

    auto count_s1_diffs = [&](const Sched1ProjectionView& v) -> int {
      const std::vector<int> ord = ScheduleSched1ViewCoreOrder(v, mm);
      if (ord.size() != v.core_insns.size()) {
        return static_cast<int>(v.core_insns.size());
      }
      int diffs = 0;
      for (int i = 0; i < static_cast<int>(ord.size()); ++i) {
        if (ord[static_cast<size_t>(i)] != i) {
          ++diffs;
        }
      }
      return diffs;
    };

    struct BestResidualState {
      int s1_diffs = std::numeric_limits<int>::max();
      std::vector<int> to_base;
      std::vector<Insn> seq;
      Sched1ProjectionView view;
      absl::btree_set<int> excluded;
      std::vector<int> extra_pinned;
    };
    const int cur_s1_diffs = count_s1_diffs(cur_view);
    BestResidualState best_residual;

    auto refine_from_initial_pref = [&](const std::vector<int>& init_pref_in_base,
                                        bool record_best) -> bool {
      std::vector<int> seed_to_base;
      std::vector<Insn> seed_seq;
      Sched1ProjectionView seed_view;
      absl::btree_set<int> seed_excluded;
      std::vector<int> seed_extra_pinned;
      std::vector<int> pref_in_base = init_pref_in_base;
      constexpr int kMaxSeedRefineRounds = 4;
      for (int sround = 0; sround < kMaxSeedRefineRounds; ++sround) {
        if (sround > 0) {
          std::vector<int> pref_in_cur;
          if (!BuildPreferredSched1OrderInBase(seed_seq, prologue_end, epilogue_begin, seed_view,
                                               seed_excluded, mm, &pref_in_cur,
                                               seed_extra_pinned)) {
            break;
          }
          pref_in_base.resize(static_cast<size_t>(n));
          for (int pos = 0; pos < n; ++pos) {
            pref_in_base[static_cast<size_t>(pos)] =
                seed_to_base[static_cast<size_t>(pref_in_cur[static_cast<size_t>(pos)])];
          }
        }
        if (!tried_prefs.insert(pref_in_base).second) {
          break;
        }
        const std::optional<std::vector<int>> cand_in_base =
            InvertBlockOrder(base_block, facts_s2, mm, Phase::kSched2, live_at_end_s2, pref_in_base,
                             reload_adjacent);
        if (!cand_in_base.has_value()) {
          break;
        }
        bool differs = false;
        for (int pos = 0; pos < n; ++pos) {
          if ((*cand_in_base)[static_cast<size_t>(pos)] != pos) {
            differs = true;
            break;
          }
        }
        const bool ok_safe =
            differs &&
            PreservesPostReloadOrderSafety(
                base_block, *cand_in_base, prologue_end, epilogue_begin, live_at_end_s2,
                cfg_ctx.cfg_copy_pairs, facts_s2, mm, allow_pure_reader_reg_dead_transfer,
                cfg_ctx.stack_layout, proj_ctx.excluded_prologue_body_indices,
                allow_moved_store_load_equiv_invalidation);
        if (!ok_safe) {
          break;
        }
        tie_candidates.push_back(*cand_in_base);
        std::vector<Insn> cand_seq = BuildTransferredCandSeqForStage0Projection(
            base_block, *cand_in_base, prologue_end, epilogue_begin, mm,
            allow_pure_reader_reg_dead_transfer);
        std::vector<Insn> cand_body;
        cand_body.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          Insn insn = cand_seq[static_cast<size_t>(pos)];
          insn.luid = pos - prologue_end;
          cand_body.push_back(std::move(insn));
        }
        Sched1ProjectionContext cand_ctx = proj_ctx;
        cand_ctx.excluded_prologue_body_indices =
            FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, block_idx, mm);
        Sched1ProjectionView cand_view =
            ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
        const bool is_ach = cand_view.core_insns.size() <= 1 ||
                            IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                         Phase::kSched1, cand_view.live_at_end_s1);
        if (is_ach) {
          return true;
        }
        seed_to_base = *cand_in_base;
        seed_seq = std::move(cand_seq);
        seed_excluded = std::move(cand_ctx.excluded_prologue_body_indices);
        seed_view = std::move(cand_view);
        if (hoist_b0_args && block_idx == 0) {
          seed_extra_pinned = CollectDisplacedBlock0EntryArgCopyBodyIndices(
              cand_body, seed_view, seed_excluded, mm, fp_needed);
        }
        if (record_best) {
          const int nd = count_s1_diffs(seed_view);
          if (nd > 0 && nd < best_residual.s1_diffs) {
            best_residual.s1_diffs = nd;
            best_residual.to_base = seed_to_base;
            best_residual.seq = seed_seq;
            best_residual.view = seed_view;
            best_residual.excluded = seed_excluded;
            best_residual.extra_pinned = seed_extra_pinned;
          }
        }
      }
      return false;
    };

    // (a) Hoist callee-saved insns sunk across a call (`sched.c:1704, 2235`).
    for (const std::vector<int>& hoist_in_cur : CollectSched1704CallHoistCandidatePrefs(
             cur_seq, prologue_end, epilogue_begin, cur_view, facts_s2, mm)) {
      std::vector<int> hoist_in_base(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        hoist_in_base[static_cast<size_t>(pos)] =
            cur_to_base[static_cast<size_t>(hoist_in_cur[static_cast<size_t>(pos)])];
      }
      if (refine_from_initial_pref(hoist_in_base, /*record_best=*/true)) {
        return true;
      }
    }

    auto run_tie_flips_on_state =
        [&](const std::vector<int>& st_to_base, const std::vector<Insn>& st_seq,
            const Sched1ProjectionView& st_view, const absl::btree_set<int>& st_excluded,
            const std::vector<int>& st_extra_pinned, int max_orders, bool record_best) -> bool {
      const std::vector<std::pair<int, int>> core_s2_edges =
          ExtractCoreSched2Edges(st_seq, prologue_end, epilogue_begin, st_view, facts_s2, mm);
      const std::vector<std::pair<int, int>> core_s2_replay_edges = ExtractCoreSched2ReplayEdges(
          base_block, st_to_base, prologue_end, epilogue_begin, st_view, facts_s2, mm,
          live_at_end_s2, /*preserve_reg_dead_reader_order=*/!allow_pure_reader_reg_dead_transfer);
      const std::vector<std::vector<int>> tie_flip_orders = CollectSched1TieFlipCoreOrders(
          st_view, mm, max_orders, core_s2_edges, core_s2_replay_edges);
      for (const std::vector<int>& alt_s1_order : tie_flip_orders) {
        std::vector<int> pref_in_cur;
        if (!BuildPreferredSched1OrderInBase(st_seq, prologue_end, epilogue_begin, st_view,
                                             st_excluded, mm, &pref_in_cur, st_extra_pinned,
                                             alt_s1_order)) {
          continue;
        }
        std::vector<int> pref_in_base(static_cast<size_t>(n));
        for (int pos = 0; pos < n; ++pos) {
          pref_in_base[static_cast<size_t>(pos)] =
              st_to_base[static_cast<size_t>(pref_in_cur[static_cast<size_t>(pos)])];
        }
        if (refine_from_initial_pref(pref_in_base, record_best)) {
          return true;
        }
      }
      return false;
    };

    // (b) Tie-flip search on the default-LUID fixed point `cur_view`.
    if (run_tie_flips_on_state(cur_to_base, cur_seq, cur_view, cur_excluded, extra_pinned,
                               /*max_orders=*/24, /*record_best=*/true)) {
      return true;
    }

    // (c) Also try tie-flips starting from original `base_view` when default-LUID
    //     fixed-point rounds altered consumer ordering (`expr.c:4027`, `sched.c:2428`).
    bool cur_differs_from_base = false;
    for (int pos = 0; pos < n; ++pos) {
      if (cur_to_base[static_cast<size_t>(pos)] != pos) {
        cur_differs_from_base = true;
        break;
      }
    }
    if (cur_differs_from_base) {
      std::vector<int> id_to_base(static_cast<size_t>(n));
      std::vector<Insn> id_seq;
      id_seq.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        id_to_base[static_cast<size_t>(pos)] = pos;
        Insn insn = base_block[static_cast<size_t>(pos)];
        insn.luid = pos;
        id_seq.push_back(std::move(insn));
      }
      std::vector<int> base_extra_pinned;
      if (hoist_b0_args && block_idx == 0) {
        std::vector<Insn> base_body_only;
        base_body_only.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          Insn insn = id_seq[static_cast<size_t>(pos)];
          insn.luid = pos - prologue_end;
          base_body_only.push_back(std::move(insn));
        }
        base_extra_pinned = CollectDisplacedBlock0EntryArgCopyBodyIndices(
            base_body_only, base_view, proj_ctx.excluded_prologue_body_indices, mm, fp_needed);
      }
      if (run_tie_flips_on_state(id_to_base, id_seq, base_view,
                                 proj_ctx.excluded_prologue_body_indices, base_extra_pinned,
                                 /*max_orders=*/24, /*record_best=*/true)) {
        return true;
      }
    }

    auto run_sunk_consumer_hoists_on_state =
        [&](const std::vector<int>& st_to_base, const std::vector<Insn>& st_seq,
            const Sched1ProjectionView& st_view, const absl::btree_set<int>& st_excluded,
            const std::vector<int>& st_extra_pinned, int max_orders, bool record_best) -> bool {
      const std::vector<std::vector<int>> hoist_orders =
          CollectSched1SunkConsumerHoistCoreOrders(st_view, mm, max_orders);
      for (const std::vector<int>& alt_s1_order : hoist_orders) {
        std::vector<int> pref_in_cur;
        if (!BuildPreferredSched1OrderInBase(st_seq, prologue_end, epilogue_begin, st_view,
                                             st_excluded, mm, &pref_in_cur, st_extra_pinned,
                                             alt_s1_order)) {
          continue;
        }
        std::vector<int> pref_in_base(static_cast<size_t>(n));
        for (int pos = 0; pos < n; ++pos) {
          pref_in_base[static_cast<size_t>(pos)] =
              st_to_base[static_cast<size_t>(pref_in_cur[static_cast<size_t>(pos)])];
        }
        if (refine_from_initial_pref(pref_in_base, record_best)) {
          return true;
        }
      }
      return false;
    };

    // (d) Undo `sched2` consumer-sinking and run one bounded nested tie-flip
    //     pass on the single strictly-best residual state.
    if (best_residual.s1_diffs < cur_s1_diffs && !best_residual.to_base.empty()) {
      const BestResidualState snap1 = best_residual;
      if (run_sunk_consumer_hoists_on_state(snap1.to_base, snap1.seq, snap1.view, snap1.excluded,
                                            snap1.extra_pinned, /*max_orders=*/12,
                                            /*record_best=*/true)) {
        return true;
      }
      const BestResidualState snap2 = best_residual;
      if (run_tie_flips_on_state(snap2.to_base, snap2.seq, snap2.view, snap2.excluded,
                                 snap2.extra_pinned, /*max_orders=*/12, /*record_best=*/true)) {
        return true;
      }
      if (best_residual.s1_diffs < snap2.s1_diffs && !best_residual.to_base.empty()) {
        const BestResidualState snap3 = best_residual;
        if (run_sunk_consumer_hoists_on_state(snap3.to_base, snap3.seq, snap3.view, snap3.excluded,
                                              snap3.extra_pinned, /*max_orders=*/12,
                                              /*record_best=*/false)) {
          return true;
        }
      }
    } else if (cur_s1_diffs > 0) {
      if (run_sunk_consumer_hoists_on_state(cur_to_base, cur_seq, cur_view, cur_excluded,
                                            extra_pinned, /*max_orders=*/12,
                                            /*record_best=*/false)) {
        return true;
      }
    }
    return false;
  };

  // Tier 1: default-LUID fixed-point search (`hoist=false`, then block-0 `hoist=true`).
  bool primary_solved = run_pass(/*hoist_b0_args=*/false, /*run_tie_flips=*/false);
  if (block_idx == 0) {
    primary_solved = run_pass(/*hoist_b0_args=*/true, /*run_tie_flips=*/false) || primary_solved;
  }
  // Tier 1b: minimal-edit `ScheduleClosestSched1CoreOrder` fixed-point search
  // preserving already-achievable equal-priority prefixes/suffixes.
  if (!primary_solved) {
    primary_solved =
        run_pass(/*hoist_b0_args=*/false, /*run_tie_flips=*/false, /*use_closest_s1=*/true);
    if (!primary_solved && block_idx == 0) {
      primary_solved =
          run_pass(/*hoist_b0_args=*/true, /*run_tie_flips=*/false, /*use_closest_s1=*/true);
    }
  }
  // Tier 2: bounded equal-priority LUID tie-flip fallback, ordered ahead of
  // `primary_candidates` so reverse iteration (`rbegin()`) always tries every
  // primary default-LUID candidate before any tie-flip candidate.
  if (!primary_solved && allow_tie_flips) {
    if (!run_pass(/*hoist_b0_args=*/false, /*run_tie_flips=*/true) && block_idx == 0) {
      (void)run_pass(/*hoist_b0_args=*/true, /*run_tie_flips=*/true);
    }
  }
  std::vector<std::vector<int>> candidates;
  candidates.reserve(tie_candidates.size() + primary_candidates.size());
  for (std::vector<int>& tc : tie_candidates) {
    candidates.push_back(std::move(tc));
  }
  for (std::vector<int>& pc : primary_candidates) {
    candidates.push_back(std::move(pc));
  }
  return candidates;
}

}  // namespace

bool HasSched1AchievablePreimageForSched2Block(
    absl::Span<const Insn> cand_sched, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionView& cand_view, const Sched1ProjectionContext& proj_ctx,
    const CfgSched1Context& cfg_ctx, const RegFacts& facts_s2, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, bool allow_pure_reader_reg_dead_transfer,
    bool allow_tie_flips) {
  if (cand_view.core_insns.size() <= 1 || IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                                       Phase::kSched1, cand_view.live_at_end_s1)) {
    return true;
  }
  const int n = static_cast<int>(cand_sched.size());
  const int body_len = bounds.epilogue_begin - bounds.prologue_end;
  if (n <= 2 || body_len <= 1) {
    return false;
  }
  std::vector<AdjacentPair> reload_adjacent;
  for (int i = 0; i + 1 < n; ++i) {
    if (IsAdjacentMdOutputReloadPair(cand_sched[static_cast<size_t>(i)],
                                     cand_sched[static_cast<size_t>(i + 1)]) ||
        IsAdjacentReloadContiguityPair(cand_sched[static_cast<size_t>(i)],
                                       cand_sched[static_cast<size_t>(i + 1)], mm,
                                       cfg_ctx.stack_layout)) {
      reload_adjacent.push_back(AdjacentPair{.first = i, .second = i + 1});
    }
  }
  const std::vector<std::vector<int>> candidates = FindStage0Sched2PreimageCandidates(
      cand_sched, block_idx, bounds, cand_view, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2,
      reload_adjacent, allow_pure_reader_reg_dead_transfer, allow_tie_flips);
  for (const std::vector<int>& cand_in_base : candidates) {
    std::vector<Insn> pre_seq = BuildTransferredCandSeqForStage0Projection(
        cand_sched, cand_in_base, bounds.prologue_end, bounds.epilogue_begin, mm,
        allow_pure_reader_reg_dead_transfer);
    std::vector<Insn> pre_body;
    pre_body.reserve(static_cast<size_t>(body_len));
    for (int pos = bounds.prologue_end; pos < bounds.epilogue_begin; ++pos) {
      Insn insn = pre_seq[static_cast<size_t>(pos)];
      insn.luid = pos - bounds.prologue_end;
      pre_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext pre_ctx = proj_ctx;
    pre_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(pre_seq, bounds, block_idx, mm);
    const Sched1ProjectionView pre_view =
        ProjectPostReloadBlockToSched1View(pre_body, mm, live_at_end_s2, pre_ctx);
    if (pre_view.core_insns.size() <= 1 || IsAchievable(pre_view.core_insns, pre_view.facts_s1, mm,
                                                        Phase::kSched1, pre_view.live_at_end_s1)) {
      return true;
    }
  }
  return false;
}

namespace {

struct Stage0OriginLoc {
  size_t orig_insn_idx = 0;
  bool is_delay_slot = false;
  bool had_placeholder_dead = false;
  bool is_epilogue_delay = false;
  bool is_synthetic = false;
  // Temporary `(REG_DEAD arg_reg)` notes attached to this entry-param copy
  // inside `view_cfg` only to mirror `jump.c:434-465` deleted pre-call arg
  // moves; stored per-origin-loc so view-index shifts never leak them back.
  absl::btree_set<int> synthetic_added_dead_regs;
};

bool IsSpRaSavePatternForDelayView(const RtlExpr& pat) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kMem ||
      pat.SetDest().children.empty() || pat.SetSrc().op != RtlOp::kReg ||
      pat.SetSrc().RegNo() != kMipsReturnAddrRegno) {
    return false;
  }
  const RtlExpr& addr = pat.SetDest().children[0];
  return (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
         (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
          addr.children[0].op == RtlOp::kReg &&
          addr.children[0].RegNo() == kMipsStackPointerRegno &&
          addr.children[1].op == RtlOp::kConstInt);
}

bool SlotCanPrecedeRaSaveForDelayView(const RtlExpr& pat) {
  bool writes_mem = false;
  auto check_set = [&](const RtlExpr& elem) {
    if (elem.op == RtlOp::kSet && elem.children.size() == 2 && elem.SetDest().op == RtlOp::kMem) {
      writes_mem = true;
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      check_set(ch);
    }
  } else {
    check_set(pat);
  }
  if (writes_mem) {
    return false;
  }
  absl::btree_set<int> fdefs;
  absl::btree_set<int> pdefs;
  absl::btree_set<int> reads;
  CollectFullRegDefs(pat, &fdefs, &pdefs);
  CollectRegReads(pat, &reads);
  return !fdefs.contains(kMipsStackPointerRegno) && !pdefs.contains(kMipsStackPointerRegno) &&
         !fdefs.contains(kMipsReturnAddrRegno) && !pdefs.contains(kMipsReturnAddrRegno) &&
         !reads.contains(kMipsReturnAddrRegno);
}

struct Stage0SeqInfo {
  RtlExpr head_wrap;
  RtlExpr slot_wrap;
  RtlPatternAndNotes slot_pn;
};

struct Stage0BlockDelayView {
  bool eligible = true;
  std::vector<Stage0OriginLoc> view_to_orig;
  absl::btree_map<size_t, Stage0SeqInfo> seqs;
  absl::btree_set<int> annulled_slot_live_at_end;
  std::optional<size_t> unpacked_non_return_jump_slot_view_idx;
  absl::btree_set<size_t> alignment_touched_view_idxs;
};

void CommitStage0AttributedNotesToBlock(
    const ParsedSchedRegion& base_region, const std::vector<Insn>& updated_base,
    const absl::btree_set<int>& touched_sched_idxs, Stage0BlockDelayView* block_view,
    RtlBasicBlock* orig_blk, std::optional<std::string>* epilogue_delay_pattern = nullptr) {
  const size_t orig_len = orig_blk->instructions.size();
  std::vector<bool> saved_has_fusage(orig_len, false);
  std::vector<std::vector<RtlExpr>> saved_fusage(orig_len);
  for (size_t i = 0; i < orig_len && i < orig_blk->rtl_insns.size(); ++i) {
    if (orig_blk->rtl_insns[i].has_explicit_call_fusage) {
      saved_has_fusage[i] = true;
      saved_fusage[i] = orig_blk->rtl_insns[i].call_fusage;
    }
  }
  auto filter_synthetic_dead_notes = [&](const Stage0OriginLoc& loc,
                                         const std::vector<RtlRegNote>& notes) {
    if (loc.synthetic_added_dead_regs.empty()) {
      return notes;
    }
    std::vector<RtlRegNote> filtered;
    filtered.reserve(notes.size());
    for (const RtlRegNote& n : notes) {
      if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
          loc.synthetic_added_dead_regs.contains(n.datum.RegNo())) {
        continue;
      }
      filtered.push_back(n);
    }
    return filtered;
  };
  for (int si : touched_sched_idxs) {
    const size_t view_idx = base_region.sched_orig_indices[static_cast<size_t>(si)];
    const Stage0OriginLoc& loc = block_view->view_to_orig[view_idx];
    if (loc.is_synthetic) {
      continue;
    }
    if (loc.is_epilogue_delay) {
      if (epilogue_delay_pattern != nullptr && epilogue_delay_pattern->has_value()) {
        const RtlPatternAndNotes& upd = updated_base[static_cast<size_t>(si)].pn;
        *epilogue_delay_pattern = FormatGccPrintRtlPatternAndNotes(
            upd.pattern, filter_synthetic_dead_notes(loc, upd.reg_notes), upd.bracketed_notes,
            /*call_fusage=*/{});
      }
    } else if (loc.is_delay_slot) {
      auto seq_it = block_view->seqs.find(loc.orig_insn_idx);
      if (seq_it != block_view->seqs.end()) {
        seq_it->second.slot_pn.reg_notes =
            filter_synthetic_dead_notes(loc, updated_base[static_cast<size_t>(si)].pn.reg_notes);
        orig_blk->instructions[loc.orig_insn_idx] =
            RtlExpr::MakeSequence({
                                      seq_it->second.head_wrap,
                                      RtlExpr::MakeInsnWrapper(seq_it->second.slot_wrap.op,
                                                               seq_it->second.slot_wrap.flags,
                                                               seq_it->second.slot_pn),
                                  })
                .ToGccPrintRtlString();
      }
    } else {
      const RtlPatternAndNotes& upd = updated_base[static_cast<size_t>(si)].pn;
      orig_blk->instructions[loc.orig_insn_idx] = FormatGccPrintRtlPatternAndNotes(
          upd.pattern, filter_synthetic_dead_notes(loc, upd.reg_notes), upd.bracketed_notes,
          /*call_fusage=*/{});
    }
  }
  orig_blk->SyncRtlInsnsFromInstructions();
  for (size_t i = 0; i < orig_len && i < orig_blk->rtl_insns.size(); ++i) {
    if (saved_has_fusage[i] && !orig_blk->rtl_insns[i].has_explicit_call_fusage) {
      orig_blk->rtl_insns[i].has_explicit_call_fusage = true;
      orig_blk->rtl_insns[i].call_fusage = std::move(saved_fusage[i]);
    }
  }
}

// Returns the subset of general `local-alloc.c:1266-1300, 1851-1886`
// (`combine_regs`) RMW split candidates in `body` whose destination is a
// call-used hard register (`mm.CallUsed(r)`). Call-used hard registers never
// hold call-crossing user accumulators (`local-alloc.c:2106`, `global.c:927`),
// so separating them from callee-saved multi-set accumulators prevents spurious
// splits from distorting `sched.c:2490` (`birthing_insn_p`) priorities.
absl::btree_set<int> CollectCallUsedGeneralCombineRegsRmwSplits(
    absl::Span<const Insn> body, const Sched1ProjectionContext& base_ctx,
    const absl::btree_set<int>& live_at_end_s2, const MachineModel& mm) {
  Sched1ProjectionContext all_ctx = base_ctx;
  all_ctx.allow_post_call_combine_regs_split = true;
  all_ctx.allow_general_combine_regs_split = true;
  all_ctx.forced_combine_regs_split_body_indices.clear();
  const Sched1ProjectionView all_view =
      ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, all_ctx);
  absl::btree_set<int> call_used_splits;
  for (int body_idx : all_view.implicit_combine_regs_split_orig_indices) {
    if (body_idx < 0 || body_idx >= static_cast<int>(body.size())) {
      continue;
    }
    const RtlExpr* s = SingleSetOf(body[static_cast<size_t>(body_idx)].pn.pattern);
    if (s != nullptr && s->SetDest().op == RtlOp::kReg && s->SetDest().RegNo() >= 0 &&
        s->SetDest().RegNo() < kMipsFirstPseudoRegister && mm.CallUsed(s->SetDest().RegNo())) {
      call_used_splits.insert(body_idx);
    }
  }
  return call_used_splits;
}

// Finds a minimal subset of block-local `local-alloc.c:1266-1300, 1851-1886`
// (`combine_regs`) RMW body indices whose implicit `(REG_DEAD r)` splits
// restore `Phase::kSched1` achievability (`sched.c:1732`, `sched.c:2490`) on a
// body that is otherwise non-achievable without those splits. Returns empty if
// `body` is already achievable without splitting or cannot be made achievable.
absl::btree_set<int> SelectMinimalCombineRegsRmwSplits(absl::Span<const Insn> body,
                                                       const Sched1ProjectionContext& base_ctx,
                                                       const absl::btree_set<int>& live_at_end_s2,
                                                       const MachineModel& mm,
                                                       bool allow_general = true) {
  Sched1ProjectionContext unsplit_ctx = base_ctx;
  unsplit_ctx.allow_post_call_combine_regs_split = false;
  unsplit_ctx.allow_call_used_general_combine_regs_split = false;
  unsplit_ctx.allow_general_combine_regs_split = false;
  unsplit_ctx.forced_combine_regs_split_body_indices.clear();
  const Sched1ProjectionView unsplit_view =
      ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, unsplit_ctx);
  if (unsplit_view.core_insns.size() <= 1 ||
      IsAchievable(unsplit_view.core_insns, unsplit_view.facts_s1, mm, Phase::kSched1,
                   unsplit_view.live_at_end_s1)) {
    return {};
  }

  auto shrink_minimal = [&](absl::btree_set<int> kept) -> absl::btree_set<int> {
    const std::vector<int> cands(kept.rbegin(), kept.rend());
    for (int idx : cands) {
      if (kept.size() <= 1) {
        break;
      }
      Sched1ProjectionContext trial_ctx = unsplit_ctx;
      trial_ctx.forced_combine_regs_split_body_indices = kept;
      trial_ctx.forced_combine_regs_split_body_indices.erase(idx);
      const Sched1ProjectionView trial_view =
          ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, trial_ctx);
      if (trial_view.core_insns.size() > 1 &&
          IsAchievable(trial_view.core_insns, trial_view.facts_s1, mm, Phase::kSched1,
                       trial_view.live_at_end_s1)) {
        kept.erase(idx);
      }
    }
    return kept;
  };

  // Prefer the narrower post-call `combine_regs` split set first
  // (`sched.c:1732`, `local-alloc.c:1266-1300`), then call-used general RMWs
  // (excluding callee-saved accumulators), then all general RMWs, before
  // falling back to bounded forward greedy addition when splitting an unrelated
  // callee-saved accumulator breaks `IsAchievable` (`sched.c:2490`).
  absl::btree_set<int> all_general_cands;
  for (bool try_general : {false, true}) {
    if (try_general && !allow_general) {
      break;
    }
    Sched1ProjectionContext all_ctx = base_ctx;
    all_ctx.allow_post_call_combine_regs_split = true;
    all_ctx.allow_call_used_general_combine_regs_split = false;
    all_ctx.allow_general_combine_regs_split = try_general;
    all_ctx.forced_combine_regs_split_body_indices.clear();
    const Sched1ProjectionView candidate_view =
        ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, all_ctx);
    if (try_general) {
      all_general_cands.insert(candidate_view.implicit_combine_regs_split_orig_indices.begin(),
                               candidate_view.implicit_combine_regs_split_orig_indices.end());
    }
    if (candidate_view.core_insns.size() > 1 &&
        !candidate_view.implicit_combine_regs_split_orig_indices.empty() &&
        IsAchievable(candidate_view.core_insns, candidate_view.facts_s1, mm, Phase::kSched1,
                     candidate_view.live_at_end_s1)) {
      return shrink_minimal(
          absl::btree_set<int>(candidate_view.implicit_combine_regs_split_orig_indices.begin(),
                               candidate_view.implicit_combine_regs_split_orig_indices.end()));
    }
  }
  if (!allow_general || all_general_cands.empty()) {
    return {};
  }

  const absl::btree_set<int> call_used_cands =
      CollectCallUsedGeneralCombineRegsRmwSplits(body, base_ctx, live_at_end_s2, mm);
  if (!call_used_cands.empty() && call_used_cands.size() < all_general_cands.size()) {
    Sched1ProjectionContext cu_ctx = unsplit_ctx;
    cu_ctx.forced_combine_regs_split_body_indices = call_used_cands;
    const Sched1ProjectionView cu_view =
        ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, cu_ctx);
    if (cu_view.core_insns.size() > 1 && IsAchievable(cu_view.core_insns, cu_view.facts_s1, mm,
                                                      Phase::kSched1, cu_view.live_at_end_s1)) {
      return shrink_minimal(call_used_cands);
    }
  }

  if (all_general_cands.size() > 1 && all_general_cands.size() <= 16) {
    auto count_view_diffs = [&](const Sched1ProjectionView& v) -> int {
      const std::vector<int> ord = ScheduleSched1ViewCoreOrder(v, mm);
      if (ord.size() != v.core_insns.size()) {
        return static_cast<int>(v.core_insns.size());
      }
      int d = 0;
      for (int i = 0; i < static_cast<int>(ord.size()); ++i) {
        if (ord[static_cast<size_t>(i)] != i) {
          ++d;
        }
      }
      return d;
    };
    absl::btree_set<int> chosen;
    int cur_diffs = count_view_diffs(unsplit_view);
    constexpr int kMaxGreedySteps = 4;
    for (int step = 0; step < kMaxGreedySteps; ++step) {
      int best_idx = -1;
      int best_diffs = cur_diffs;
      for (int idx : all_general_cands) {
        if (chosen.contains(idx)) {
          continue;
        }
        Sched1ProjectionContext trial_ctx = unsplit_ctx;
        trial_ctx.forced_combine_regs_split_body_indices = chosen;
        trial_ctx.forced_combine_regs_split_body_indices.insert(idx);
        const Sched1ProjectionView trial_view =
            ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, trial_ctx);
        if (trial_view.core_insns.size() <= 1) {
          continue;
        }
        if (IsAchievable(trial_view.core_insns, trial_view.facts_s1, mm, Phase::kSched1,
                         trial_view.live_at_end_s1)) {
          chosen.insert(idx);
          return shrink_minimal(std::move(chosen));
        }
        const int nd = count_view_diffs(trial_view);
        if (nd < best_diffs) {
          best_diffs = nd;
          best_idx = idx;
        }
      }
      if (best_idx < 0) {
        break;
      }
      chosen.insert(best_idx);
      cur_diffs = best_diffs;
    }
  }
  return {};
}

// Mirrors `reload1.c:5641, 6102-6130` (`emit_reload_insns`; MIPS does not
// define `PRESERVE_DEATH_INFO_REGNO_P`): returns true when `SF`/`DF` FPR `r`
// read by FP consumer at `pos` was loaded by an earlier cross-bank `"f"`
// GPR->FPR (`mtc1`) input reload `(set (reg:SF/DF r) (reg:SF/DF gpr))`
// (without `REG_EQUIV`/`REG_EQUAL`/`REG_DEAD gpr`) with no intervening touch
// of `r` or `gpr`. Such reload scratch FPRs never receive `(REG_DEAD r)` in
// `.09.greg`.
template <typename GetPnFn, typename GetKindFn>
bool IsCrossBankFprInputReloadOperandImpl(int pos, int r, RtlMachineMode mode,
                                          const MachineModel& mm, const GetPnFn& get_pn,
                                          const GetKindFn& get_kind) {
  if (r < kMipsFpRegFirst || r > kMipsFpRegLast ||
      (mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDf)) {
    return false;
  }
  const RtlPatternAndNotes& cons_pn = get_pn(pos);
  const RtlExpr& cp = cons_pn.pattern;
  if (get_kind(pos) != Insn::Kind::kInsn || cp.op == RtlOp::kUse || cp.op == RtlOp::kClobber ||
      (cp.op == RtlOp::kSet && cp.children.size() == 2 &&
       (cp.SetSrc().op == RtlOp::kReg || cp.SetSrc().op == RtlOp::kMem))) {
    return false;
  }
  const int fpr_nregs = std::max(1, mm.HardRegNRegs(r, mode));
  for (int p = pos - 1; p >= 0; --p) {
    const Insn::Kind pkind = get_kind(p);
    if (pkind == Insn::Kind::kCall || pkind == Insn::Kind::kJump) {
      return false;
    }
    const RtlPatternAndNotes& prev_pn = get_pn(p);
    const RtlExpr& pp = prev_pn.pattern;
    if (pp.op == RtlOp::kSet && pp.children.size() == 2 && pp.SetDest().op == RtlOp::kReg &&
        !pp.SetDest().flags.integrated && pp.SetDest().RegNo() == r && pp.SetDest().mode == mode &&
        pp.SetSrc().op == RtlOp::kReg && !pp.SetSrc().flags.integrated &&
        pp.SetSrc().mode == mode && pp.SetSrc().RegNo() >= 2 &&
        pp.SetSrc().RegNo() < kMipsFpRegFirst) {
      const int gpr = pp.SetSrc().RegNo();
      const int gpr_nregs = std::max(1, mm.HardRegNRegs(gpr, mode));
      for (const RtlRegNote& n : prev_pn.reg_notes) {
        if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual) {
          return false;
        }
        if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg) {
          const int nr = n.datum.RegNo();
          if (nr >= gpr && nr < gpr + gpr_nregs) {
            return false;
          }
        }
      }
      for (int k = p + 1; k < pos; ++k) {
        const RtlExpr& mp = get_pn(k).pattern;
        if (PatternDefinesHardRegSpan(mp, gpr, gpr_nregs, mm) ||
            PatternReadsHardRegSpan(mp, gpr, gpr_nregs, mm)) {
          return false;
        }
      }
      return true;
    }
    if (PatternDefinesHardRegSpan(pp, r, fpr_nregs, mm) ||
        PatternReadsHardRegSpan(pp, r, fpr_nregs, mm)) {
      return false;
    }
  }
  return false;
}

bool IsCrossBankFprInputReloadOperandInView(absl::Span<const std::string> instructions, int pos,
                                            int r, RtlMachineMode mode, const MachineModel& mm) {
  if (r < kMipsFpRegFirst || r > kMipsFpRegLast ||
      (mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDf) || pos < 0 ||
      static_cast<size_t>(pos) >= instructions.size()) {
    return false;
  }
  std::vector<RtlPatternAndNotes> parsed(static_cast<size_t>(pos) + 1);
  for (int i = 0; i <= pos; ++i) {
    auto pn = ParsePatternAndNotes(instructions[static_cast<size_t>(i)]);
    if (!pn.ok()) {
      return false;
    }
    parsed[static_cast<size_t>(i)] = std::move(*pn);
  }
  return IsCrossBankFprInputReloadOperandImpl(
      pos, r, mode, mm,
      [&](int idx) -> const RtlPatternAndNotes& { return parsed[static_cast<size_t>(idx)]; },
      [&](int idx) { return ClassifyInsnKind(parsed[static_cast<size_t>(idx)].pattern); });
}

bool IsCrossBankFprInputReloadOperandInOrder(absl::Span<const Insn> base_block,
                                             absl::Span<const int> order_in_base, int pos, int r,
                                             RtlMachineMode mode, const MachineModel& mm) {
  return IsCrossBankFprInputReloadOperandImpl(
      pos, r, mode, mm,
      [&](int idx) -> const RtlPatternAndNotes& {
        return base_block[static_cast<size_t>(order_in_base[static_cast<size_t>(idx)])].pn;
      },
      [&](int idx) {
        return base_block[static_cast<size_t>(order_in_base[static_cast<size_t>(idx)])].kind;
      });
}

// Unpacks non-annulled `(sequence [(call_insn|jump_insn) (insn)])` delay slots
// (`reorg.c:2901-2955`) into a read-only pre-`reorg` view (`view_cfg`) plus
// reconstructed liveness (`live_cfg`), recording exact origin locations so
// verified Stage 0 notes can be committed back to `orig_cfg`.
void BuildStage0UnpackedDelayViewCfg(const RtlControlFlowGraph& orig_cfg,
                                     const CompilationProfile& profile,
                                     RtlControlFlowGraph* view_cfg, RtlControlFlowGraph* live_cfg,
                                     std::vector<Stage0BlockDelayView>* block_views,
                                     bool allow_annulled_tail_branch = false) {
  const size_t num_blocks = orig_cfg.rpo_blocks.size();
  *view_cfg = orig_cfg;
  block_views->assign(num_blocks, Stage0BlockDelayView{});

  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& orig_blk = orig_cfg.rpo_blocks[b];
    const size_t m = orig_blk.instructions.size();
    Stage0BlockDelayView& bv = (*block_views)[b];
    size_t effective_end = m;
    while (effective_end > 0) {
      auto tail_pn = ParsePatternAndNotes(orig_blk.instructions[effective_end - 1]);
      if (!tail_pn.ok() || tail_pn->pattern.op != RtlOp::kBarrier) {
        break;
      }
      --effective_end;
    }

    RtlBasicBlock& vblk = view_cfg->rpo_blocks[b];
    vblk.instructions.clear();
    vblk.rtl_insns.clear();
    bv.view_to_orig.clear();
    std::vector<std::optional<std::vector<RtlExpr>>> unpacked_call_fusage;

    for (size_t i = 0; i < effective_end; ++i) {
      auto pn = ParsePatternAndNotes(orig_blk.instructions[i]);
      if (!pn.ok()) {
        bv.eligible = false;
        break;
      }
      if (pn->pattern.op != RtlOp::kSequence) {
        if (i + 1 == effective_end && IsReturnJumpPatternForAttribution(pn->pattern)) {
          continue;
        }
        vblk.instructions.push_back(orig_blk.instructions[i]);
        bv.view_to_orig.push_back(Stage0OriginLoc{.orig_insn_idx = i, .is_delay_slot = false});
        unpacked_call_fusage.push_back(std::nullopt);
        continue;
      }
      if (pn->pattern.children.size() != 2 || pn->pattern.children[1].op != RtlOp::kInsn) {
        bv.eligible = false;
        break;
      }
      const RtlOp head_op = pn->pattern.children[0].op;
      if (pn->pattern.children[0].flags.unchanging || pn->pattern.children[1].flags.unchanging) {
        // `reorg.c:fill_eager_delay_slots`: an annulled branch (`jump_insn/u`)
        // at the end of a block holds an instruction stolen from a successor
        // block, not from this block's `sched1`/`sched2` region.
        if (allow_annulled_tail_branch && head_op == RtlOp::kJumpInsn && i + 1 == effective_end) {
          auto head_pn = RtlPatternAndNotes::FromSequenceSubInsn(pn->pattern.children[0]);
          auto slot_pn = RtlPatternAndNotes::FromSequenceSubInsn(pn->pattern.children[1]);
          if (head_pn.ok() && slot_pn.ok()) {
            CollectRegReads(slot_pn->pattern, &bv.annulled_slot_live_at_end);
            vblk.instructions.push_back(
                FormatGccPrintRtlPatternAndNotes(head_pn->pattern, head_pn->reg_notes,
                                                 head_pn->bracketed_notes, /*call_fusage=*/{}));
            bv.view_to_orig.push_back(Stage0OriginLoc{.orig_insn_idx = i, .is_delay_slot = false});
            unpacked_call_fusage.push_back(std::nullopt);
            continue;
          }
        }
        bv.eligible = false;
        break;
      }
      if (head_op != RtlOp::kJumpInsn && head_op != RtlOp::kCallInsn) {
        bv.eligible = false;
        break;
      }
      if (head_op == RtlOp::kJumpInsn && i + 1 != effective_end) {
        bv.eligible = false;
        break;
      }
      auto head_pn = RtlPatternAndNotes::FromSequenceSubInsn(pn->pattern.children[0]);
      auto slot_pn = RtlPatternAndNotes::FromSequenceSubInsn(pn->pattern.children[1]);
      if (!head_pn.ok() || !slot_pn.ok() || !IsSimpleNonControlDelayInsn(slot_pn->pattern)) {
        bv.eligible = false;
        break;
      }
      bv.seqs[i] = Stage0SeqInfo{
          .head_wrap = pn->pattern.children[0],
          .slot_wrap = pn->pattern.children[1],
          .slot_pn = *slot_pn,
      };
      // Strip `reorg.c:2283-2306` placeholder `(REG_DEAD (const_int 0))` notes
      // on unpacked call delay slots inside `view_cfg` so liveness reconstruction
      // below restores exact post-`sched2` `REG_DEAD` notes for Stage 0 discovery.
      bool had_placeholder_dead = false;
      std::vector<RtlRegNote> clean_slot_notes;
      clean_slot_notes.reserve(slot_pn->reg_notes.size());
      for (const RtlRegNote& note : slot_pn->reg_notes) {
        if (head_op == RtlOp::kCallInsn && note.kind == RtlMachineMode::kRegDead &&
            note.datum.op != RtlOp::kReg && note.datum.op != RtlOp::kMem) {
          had_placeholder_dead = true;
          continue;
        }
        clean_slot_notes.push_back(note);
      }
      const std::string slot_str = FormatGccPrintRtlPatternAndNotes(
          slot_pn->pattern, clean_slot_notes, slot_pn->bracketed_notes, /*call_fusage=*/{});
      const bool is_return =
          (head_op == RtlOp::kJumpInsn && IsReturnJumpPatternForAttribution(head_pn->pattern));
      if (is_return) {
        size_t insert_pos = vblk.instructions.size();
        while (insert_pos > 0) {
          auto prev_pn = ParsePatternAndNotes(vblk.instructions[insert_pos - 1]);
          if (!prev_pn.ok() || prev_pn->pattern.op != RtlOp::kUse) {
            break;
          }
          --insert_pos;
        }
        vblk.instructions.insert(vblk.instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                                 slot_str);
        bv.view_to_orig.insert(bv.view_to_orig.begin() + static_cast<ptrdiff_t>(insert_pos),
                               Stage0OriginLoc{.orig_insn_idx = i,
                                               .is_delay_slot = true,
                                               .had_placeholder_dead = had_placeholder_dead});
        unpacked_call_fusage.insert(
            unpacked_call_fusage.begin() + static_cast<ptrdiff_t>(insert_pos), std::nullopt);
      } else {
        // Mirror `reorg.c:2901-2955` (`fill_simple_delay_slots`): a `CALL_INSN`
        // clobbers `$ra` (`31`), so `reorg.c` skips an immediately preceding
        // prologue `$ra` save `(set (mem sp+off) (reg 31))` when scanning
        // backward for a call delay slot.
        size_t insert_pos = vblk.instructions.size();
        if (head_op == RtlOp::kCallInsn && SlotCanPrecedeRaSaveForDelayView(slot_pn->pattern)) {
          // Only step past a prologue `$ra` save when `slot` does not write a
          // `call_fusage` argument register (which `sched2` keeps after `sw $ra`
          // due to its `REG_DEP_TRUE` edge into `call`).
          absl::btree_set<int> call_arg_reads;
          for (const RtlExpr& fu : head_pn->call_fusage) {
            CollectRegReads(fu, &call_arg_reads);
          }
          if (i < orig_blk.rtl_insns.size() && orig_blk.rtl_insns[i].has_explicit_call_fusage) {
            for (const RtlExpr& fu : orig_blk.rtl_insns[i].call_fusage) {
              CollectRegReads(fu, &call_arg_reads);
            }
          }
          absl::btree_set<int> slot_fdefs;
          absl::btree_set<int> slot_pdefs;
          CollectFullRegDefs(slot_pn->pattern, &slot_fdefs, &slot_pdefs);
          bool writes_call_arg = false;
          for (int r : call_arg_reads) {
            if (slot_fdefs.contains(r) || slot_pdefs.contains(r)) {
              writes_call_arg = true;
              break;
            }
          }
          while (!writes_call_arg && insert_pos > 0) {
            auto prev_pn = ParsePatternAndNotes(vblk.instructions[insert_pos - 1]);
            if (!prev_pn.ok() || !IsSpRaSavePatternForDelayView(prev_pn->pattern)) {
              break;
            }
            --insert_pos;
          }
        }
        if (head_op == RtlOp::kJumpInsn) {
          bv.unpacked_non_return_jump_slot_view_idx = insert_pos;
        }
        vblk.instructions.insert(vblk.instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                                 slot_str);
        bv.view_to_orig.insert(bv.view_to_orig.begin() + static_cast<ptrdiff_t>(insert_pos),
                               Stage0OriginLoc{.orig_insn_idx = i,
                                               .is_delay_slot = true,
                                               .had_placeholder_dead = had_placeholder_dead});
        unpacked_call_fusage.insert(
            unpacked_call_fusage.begin() + static_cast<ptrdiff_t>(insert_pos), std::nullopt);
        vblk.instructions.push_back(FormatGccPrintRtlPatternAndNotes(
            head_pn->pattern, head_pn->reg_notes, head_pn->bracketed_notes, head_pn->call_fusage));
        bv.view_to_orig.push_back(Stage0OriginLoc{.orig_insn_idx = i, .is_delay_slot = false});
        if (head_pn->has_explicit_call_fusage) {
          unpacked_call_fusage.push_back(head_pn->call_fusage);
        } else {
          unpacked_call_fusage.push_back(std::nullopt);
        }
      }
    }
    if (!bv.eligible) {
      vblk = orig_blk;
      continue;
    }
    // Mirrors `reorg.c:3151-3225`, `config/mips/mips.h:2041-2050`, and
    // `config/mips/mips.c:5080, 5191-5217`: unpack an eligible sideband
    // `epilogue_delay_pattern` before trailing `USE`/`CLOBBER`/`NOTE` insns in
    // the exit block so Stage 0 pure-reader `REG_DEAD` attribution sees the
    // complete post-`sched2` exit block.
    if (b + 1 == num_blocks && orig_blk.successors.empty() &&
        orig_cfg.sideband.epilogue_delay_pattern.has_value() &&
        !orig_cfg.sideband.epilogue_delay_pattern->empty()) {
      auto ep_pn = ParsePatternAndNotes(*orig_cfg.sideband.epilogue_delay_pattern);
      bool ep_ok = ep_pn.ok() && IsSimpleNonControlDelayInsn(ep_pn->pattern) &&
                   !(ep_pn->pattern.IsSet() && ep_pn->pattern.SetSrc().op == RtlOp::kMem);
      if (ep_ok) {
        const MachineModel mm_ep = MachineModel::FromProfile(profile);
        ep_pn->pattern.Visit([&](const RtlExpr& n) {
          if (!ep_ok || !n.IsHardReg() || n.IsArgPointerReg()) {
            return;
          }
          const int r = n.RegNo();
          const int span = std::max(1, mm_ep.HardRegNRegs(r, n.mode));
          for (int w = r; w < r + span && w < kMipsFirstPseudoRegister; ++w) {
            const bool is_saved = std::find(orig_cfg.sideband.saved_gpr_regnos.begin(),
                                            orig_cfg.sideband.saved_gpr_regnos.end(),
                                            w) != orig_cfg.sideband.saved_gpr_regnos.end() ||
                                  std::find(orig_cfg.sideband.saved_fpr_regnos.begin(),
                                            orig_cfg.sideband.saved_fpr_regnos.end(),
                                            w) != orig_cfg.sideband.saved_fpr_regnos.end();
            if (w == kMipsStackPointerRegno || w == kMipsReturnAddrRegno ||
                (w == kMipsFramePointerRegno && orig_cfg.sideband.frame_pointer_needed) ||
                is_saved) {
              ep_ok = false;
              return;
            }
          }
        });
      }
      if (ep_ok) {
        size_t insert_pos = vblk.instructions.size();
        while (insert_pos > 0) {
          auto tail_pn = ParsePatternAndNotes(vblk.instructions[insert_pos - 1]);
          if (!tail_pn.ok() ||
              (tail_pn->pattern.op != RtlOp::kUse && tail_pn->pattern.op != RtlOp::kClobber &&
               tail_pn->pattern.op != RtlOp::kNote)) {
            break;
          }
          --insert_pos;
        }
        vblk.instructions.insert(vblk.instructions.begin() + static_cast<ptrdiff_t>(insert_pos),
                                 *orig_cfg.sideband.epilogue_delay_pattern);
        bv.view_to_orig.insert(bv.view_to_orig.begin() + static_cast<ptrdiff_t>(insert_pos),
                               Stage0OriginLoc{.orig_insn_idx = 0,
                                               .is_delay_slot = false,
                                               .had_placeholder_dead = false,
                                               .is_epilogue_delay = true});
        unpacked_call_fusage.insert(
            unpacked_call_fusage.begin() + static_cast<ptrdiff_t>(insert_pos), std::nullopt);
        view_cfg->sideband.epilogue_delay_pattern.reset();
      }
    }
    vblk.SyncRtlInsnsFromInstructions();
    for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < bv.view_to_orig.size(); ++vi) {
      if (!bv.view_to_orig[vi].is_delay_slot && !bv.view_to_orig[vi].is_epilogue_delay) {
        if (unpacked_call_fusage[vi].has_value() && !vblk.rtl_insns[vi].has_explicit_call_fusage) {
          vblk.rtl_insns[vi].has_explicit_call_fusage = true;
          vblk.rtl_insns[vi].call_fusage = *unpacked_call_fusage[vi];
          continue;
        }
        const size_t oi = bv.view_to_orig[vi].orig_insn_idx;
        if (oi < orig_blk.rtl_insns.size() && orig_blk.rtl_insns[oi].has_explicit_call_fusage &&
            !vblk.rtl_insns[vi].has_explicit_call_fusage) {
          vblk.rtl_insns[vi].has_explicit_call_fusage = true;
          vblk.rtl_insns[vi].call_fusage = orig_blk.rtl_insns[oi].call_fusage;
        }
      }
    }
  }

  *live_cfg = *view_cfg;
  ReconstructRtlLiveness(
      live_cfg, profile,
      {.reload_completed = true, .update_reg_notes = true, .preserve_phantom_pseudos = false});
  const MachineModel mm = MachineModel::FromProfile(profile);
  // Restore reconstructed `REG_DEAD` notes in `view_cfg` for unpacked call delay
  // slots whose original `(REG_DEAD (const_int 0))` placeholder was cleared by `reorg.c`.
  for (size_t b = 0; b < num_blocks; ++b) {
    if (!(*block_views)[b].eligible) {
      continue;
    }
    RtlBasicBlock& vblk = view_cfg->rpo_blocks[b];
    const RtlBasicBlock& lblk = live_cfg->rpo_blocks[b];
    bool updated_block = false;
    for (size_t vi = 0; vi < vblk.instructions.size() &&
                        vi < (*block_views)[b].view_to_orig.size() && vi < lblk.instructions.size();
         ++vi) {
      if (!(*block_views)[b].view_to_orig[vi].had_placeholder_dead) {
        continue;
      }
      auto vpn = ParsePatternAndNotes(vblk.instructions[vi]);
      auto lpn = ParsePatternAndNotes(lblk.instructions[vi]);
      if (!vpn.ok() || !lpn.ok()) {
        continue;
      }
      std::vector<RtlRegNote> merged;
      for (const RtlRegNote& note : lpn->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead) {
          // `reload1.c:5641, 6102-6130` (`emit_reload_insns`; MIPS does not
          // define `PRESERVE_DEATH_INFO_REGNO_P`): omit `(REG_DEAD fpr)` for a
          // cross-bank `"f"` GPR->FPR (`mtc1`) input reload scratch register.
          if (note.datum.op == RtlOp::kReg &&
              IsCrossBankFprInputReloadOperandInView(vblk.instructions, static_cast<int>(vi),
                                                     note.datum.RegNo(), note.datum.mode, mm)) {
            continue;
          }
          merged.push_back(note);
        }
      }
      for (const RtlRegNote& note : vpn->reg_notes) {
        if (note.kind != RtlMachineMode::kRegDead) {
          merged.push_back(note);
        }
      }
      vblk.instructions[vi] = FormatGccPrintRtlPatternAndNotes(
          vpn->pattern, merged, vpn->bracketed_notes, /*call_fusage=*/{});
      updated_block = true;
    }
    if (updated_block) {
      std::vector<bool> saved_has_fusage(vblk.rtl_insns.size(), false);
      std::vector<std::vector<RtlExpr>> saved_fusage(vblk.rtl_insns.size());
      for (size_t vi = 0; vi < vblk.rtl_insns.size(); ++vi) {
        if (vblk.rtl_insns[vi].has_explicit_call_fusage) {
          saved_has_fusage[vi] = true;
          saved_fusage[vi] = vblk.rtl_insns[vi].call_fusage;
        }
      }
      vblk.SyncRtlInsnsFromInstructions();
      for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < saved_has_fusage.size(); ++vi) {
        if (saved_has_fusage[vi] && !vblk.rtl_insns[vi].has_explicit_call_fusage) {
          vblk.rtl_insns[vi].has_explicit_call_fusage = true;
          vblk.rtl_insns[vi].call_fusage = std::move(saved_fusage[vi]);
        }
      }
    }
  }
}

bool PatternHasVolatileOrAsm(const RtlExpr& pat) {
  bool bad = false;
  pat.Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kAsmInput || node.op == RtlOp::kAsmOperands ||
        (node.op == RtlOp::kMem && node.flags.volatil)) {
      bad = true;
    }
  });
  return bad;
}

void ClassifyMemAccesses(const RtlExpr& pat, bool* reads_mem, bool* writes_mem,
                         bool* all_reads_unchanging) {
  *reads_mem = false;
  *writes_mem = false;
  *all_reads_unchanging = true;
  auto visit_reads = [&](const RtlExpr& expr) {
    expr.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kMem) {
        *reads_mem = true;
        if (!node.flags.unchanging) {
          *all_reads_unchanging = false;
        }
      }
    });
  };
  auto handle = [&](const RtlExpr& elem) {
    if (elem.op == RtlOp::kSet && elem.children.size() == 2) {
      if (elem.SetDest().op == RtlOp::kMem) {
        *writes_mem = true;
        if (!elem.SetDest().children.empty()) {
          visit_reads(elem.SetDest().children[0]);
        }
      }
      visit_reads(elem.SetSrc());
    } else if (elem.op != RtlOp::kClobber && elem.op != RtlOp::kUse) {
      visit_reads(elem);
    }
  };
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      handle(ch);
    }
  } else {
    handle(pat);
  }
}

int64_t StackModeByteSize(RtlMachineMode mode) {
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
    default:
      return 0;
  }
}

bool Stage0DelaySlotCanCommutePast(const Insn& slot, const Insn& earlier, const MachineModel& mm) {
  if (earlier.kind != Insn::Kind::kInsn || earlier.pn.pattern.op == RtlOp::kUse ||
      earlier.pn.pattern.op == RtlOp::kClobber || PatternHasVolatileOrAsm(earlier.pn.pattern) ||
      PatternHasVolatileOrAsm(slot.pn.pattern)) {
    return false;
  }
  const InsnRegReadDefDeaths si = CollectInsnRegReadDefDeaths(slot, mm);
  const InsnRegReadDefDeaths ei = CollectInsnRegReadDefDeaths(earlier, mm);
  for (int r : si.defs) {
    if (ei.defs.contains(r) || ei.reads.contains(r)) {
      return false;
    }
  }
  for (int r : ei.defs) {
    if (si.reads.contains(r)) {
      return false;
    }
  }
  bool s_rmem = false;
  bool s_wmem = false;
  bool s_ro_u = true;
  bool e_rmem = false;
  bool e_wmem = false;
  bool e_ro_u = true;
  ClassifyMemAccesses(slot.pn.pattern, &s_rmem, &s_wmem, &s_ro_u);
  ClassifyMemAccesses(earlier.pn.pattern, &e_rmem, &e_wmem, &e_ro_u);
  if (!s_wmem && !e_wmem) {
    return true;
  }
  if (!s_rmem && !s_wmem) {
    return true;
  }
  if (!e_rmem && !e_wmem) {
    return true;
  }
  // Allow commuting disjoint plain stack-slot accesses (`sched.c:memrefs_conflict_p`)
  // or a plain stack-slot store against a read-only `MEM/u` load.
  const auto s_acc = TryParsePlainStackSlotMove(slot);
  const auto e_acc = TryParsePlainStackSlotMove(earlier);
  if (s_acc.has_value() && e_acc.has_value() && s_acc->key.base_regno == e_acc->key.base_regno) {
    const int64_t s_sz = StackModeByteSize(s_acc->key.mode);
    const int64_t e_sz = StackModeByteSize(e_acc->key.mode);
    if (s_sz > 0 && e_sz > 0 &&
        (s_acc->key.offset + s_sz <= e_acc->key.offset ||
         e_acc->key.offset + e_sz <= s_acc->key.offset)) {
      return true;
    }
  }
  if (s_acc.has_value() && s_acc->is_store && !e_wmem && e_ro_u) {
    return true;
  }
  if (e_acc.has_value() && e_acc->is_store && !s_wmem && s_ro_u) {
    return true;
  }
  return false;
}

// Re-aligns unpacked delay-slot instructions (`reorg.c:2800-2960`
// `fill_simple_delay_slots`) in `view_cfg` when naive placement immediately
// before the branch/call falls outside `Im(F_sched2)`, sliding up to 2 delay
// slots earlier across dependency-independent instructions until
// `InvertBlockOrder(Phase::kSched2)` succeeds.
void AlignStage0DelaySlotsToSched2View(const CompilationProfile& profile, const MachineModel& mm,
                                       RtlControlFlowGraph* view_cfg, RtlControlFlowGraph* live_cfg,
                                       std::vector<Stage0BlockDelayView>* block_views) {
  const RegFacts facts_s2 = RegFacts::FromCfg(*view_cfg, profile, Phase::kSched2);
  bool any_reordered = false;
  const size_t num_blocks = view_cfg->rpo_blocks.size();
  std::vector<absl::btree_set<size_t>> moved_slot_new_view_idxs(num_blocks);

  for (size_t b = 0; b < num_blocks; ++b) {
    Stage0BlockDelayView& bv = (*block_views)[b];
    if (!bv.eligible || bv.seqs.empty()) {
      continue;
    }
    RtlBasicBlock& vblk = view_cfg->rpo_blocks[b];
    const ParsedSchedRegion region = ParseBlockForSchedModel(vblk, b, Phase::kSched2);
    if (!region.valid || region.has_mid_block_loop_barrier || region.sched_block.size() <= 2) {
      continue;
    }
    absl::btree_set<int> live_at_end_s2;
    for (int succ : vblk.successors) {
      if (const auto it = live_cfg->sideband.basic_block_live_at_start.find(succ);
          it != live_cfg->sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    if (InvertBlockOrder(region.sched_block, facts_s2, mm, Phase::kSched2, live_at_end_s2)
            .has_value()) {
      continue;
    }

    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, b);
    const int n = static_cast<int>(region.sched_block.size());
    std::vector<int> slot_sis;
    for (int si = bounds.prologue_end; si < bounds.epilogue_begin; ++si) {
      const size_t vi = region.sched_orig_indices[static_cast<size_t>(si)];
      if (vi < bv.view_to_orig.size() && bv.view_to_orig[vi].is_delay_slot) {
        slot_sis.push_back(si);
      }
    }
    if (slot_sis.empty()) {
      continue;
    }

    // Collect nearest valid earlier positions for each unpacked delay slot.
    constexpr int kMaxSlidesPerSlot = 12;
    std::vector<std::vector<int>> valid_targets(slot_sis.size());
    for (size_t k = 0; k < slot_sis.size(); ++k) {
      const int si = slot_sis[k];
      valid_targets[k].push_back(si);
      const Insn& slot_insn = region.sched_block[static_cast<size_t>(si)];
      int slides = 0;
      for (int pos = si - 1; pos >= bounds.prologue_end && slides < kMaxSlidesPerSlot; --pos) {
        if (!Stage0DelaySlotCanCommutePast(slot_insn, region.sched_block[static_cast<size_t>(pos)],
                                           mm)) {
          break;
        }
        valid_targets[k].push_back(pos);
        ++slides;
      }
    }

    auto apply_slot_moves = [&](absl::Span<const std::pair<int, int>> moves) -> std::vector<int> {
      std::vector<int> perm(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        perm[static_cast<size_t>(i)] = i;
      }
      // Apply moves in descending `from` order so earlier indices stay valid.
      std::vector<std::pair<int, int>> sorted_moves(moves.begin(), moves.end());
      std::sort(sorted_moves.begin(), sorted_moves.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      for (const auto& [from, to] : sorted_moves) {
        if (to >= from || to < 0 || from >= n) {
          continue;
        }
        const int val = perm[static_cast<size_t>(from)];
        perm.erase(perm.begin() + from);
        perm.insert(perm.begin() + to, val);
      }
      return perm;
    };

    auto test_perm = [&](const std::vector<int>& perm) -> bool {
      std::vector<Insn> trial;
      trial.reserve(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        Insn insn = region.sched_block[static_cast<size_t>(perm[static_cast<size_t>(i)])];
        insn.luid = i;
        trial.push_back(std::move(insn));
      }
      return InvertBlockOrder(trial, facts_s2, mm, Phase::kSched2, live_at_end_s2).has_value();
    };

    std::optional<std::vector<int>> chosen_perm;
    // Pass 1: single-slot slide (preferring minimal displacement).
    for (size_t k = 0; k < slot_sis.size() && !chosen_perm.has_value(); ++k) {
      for (size_t t = 1; t < valid_targets[k].size(); ++t) {
        std::vector<int> perm = apply_slot_moves({{slot_sis[k], valid_targets[k][t]}});
        if (test_perm(perm)) {
          chosen_perm = std::move(perm);
          break;
        }
      }
    }
    // Pass 2: pairwise slides when two call/jump delay slots in the same block both moved.
    int pair_budget = 40;
    for (size_t k1 = 0; k1 < slot_sis.size() && !chosen_perm.has_value() && pair_budget > 0; ++k1) {
      for (size_t k2 = k1 + 1; k2 < slot_sis.size() && !chosen_perm.has_value() && pair_budget > 0;
           ++k2) {
        const size_t lim1 = std::min<size_t>(valid_targets[k1].size(), 7);
        const size_t lim2 = std::min<size_t>(valid_targets[k2].size(), 7);
        for (size_t t1 = 1; t1 < lim1 && !chosen_perm.has_value() && pair_budget > 0; ++t1) {
          for (size_t t2 = 1; t2 < lim2 && !chosen_perm.has_value() && pair_budget > 0; ++t2) {
            --pair_budget;
            std::vector<int> perm = apply_slot_moves(
                {{slot_sis[k1], valid_targets[k1][t1]}, {slot_sis[k2], valid_targets[k2][t2]}});
            if (test_perm(perm)) {
              chosen_perm = std::move(perm);
              break;
            }
          }
        }
      }
    }

    if (!chosen_perm.has_value()) {
      continue;
    }

    // Apply `chosen_perm` to `vblk.instructions`, `vblk.rtl_insns`, and `bv.view_to_orig`.
    std::vector<std::string> new_insns = vblk.instructions;
    std::vector<Stage0OriginLoc> new_v2o = bv.view_to_orig;
    std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage(vblk.instructions.size());
    for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < saved_fusage.size(); ++vi) {
      if (vblk.rtl_insns[vi].has_explicit_call_fusage) {
        saved_fusage[vi] = vblk.rtl_insns[vi].call_fusage;
      }
    }
    std::vector<std::optional<std::vector<RtlExpr>>> new_fusage = saved_fusage;
    for (int i = 0; i < n; ++i) {
      const size_t dst_vi = region.sched_orig_indices[static_cast<size_t>(i)];
      const size_t src_vi =
          region.sched_orig_indices[static_cast<size_t>((*chosen_perm)[static_cast<size_t>(i)])];
      new_insns[dst_vi] = vblk.instructions[src_vi];
      new_v2o[dst_vi] = bv.view_to_orig[src_vi];
      new_fusage[dst_vi] = saved_fusage[src_vi];
      if (bv.view_to_orig[src_vi].is_delay_slot && dst_vi < src_vi) {
        moved_slot_new_view_idxs[b].insert(dst_vi);
      }
    }
    vblk.instructions = std::move(new_insns);
    bv.view_to_orig = std::move(new_v2o);
    vblk.SyncRtlInsnsFromInstructions();
    for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < new_fusage.size(); ++vi) {
      if (new_fusage[vi].has_value() && !vblk.rtl_insns[vi].has_explicit_call_fusage) {
        vblk.rtl_insns[vi].has_explicit_call_fusage = true;
        vblk.rtl_insns[vi].call_fusage = *new_fusage[vi];
      }
    }
    any_reordered = true;
  }

  if (any_reordered) {
    *live_cfg = *view_cfg;
    ReconstructRtlLiveness(
        live_cfg, profile,
        {.reload_completed = true, .update_reg_notes = true, .preserve_phantom_pseudos = false});
    for (size_t b = 0; b < num_blocks; ++b) {
      Stage0BlockDelayView& bv = (*block_views)[b];
      if (!bv.eligible) {
        continue;
      }
      RtlBasicBlock& vblk = view_cfg->rpo_blocks[b];
      const RtlBasicBlock& lblk = live_cfg->rpo_blocks[b];
      bool updated = false;
      for (size_t vi = 0; vi < vblk.instructions.size() && vi < bv.view_to_orig.size() &&
                          vi < lblk.instructions.size();
           ++vi) {
        if (!bv.view_to_orig[vi].had_placeholder_dead) {
          continue;
        }
        auto vpn = ParsePatternAndNotes(vblk.instructions[vi]);
        auto lpn = ParsePatternAndNotes(lblk.instructions[vi]);
        if (!vpn.ok() || !lpn.ok()) {
          continue;
        }
        std::vector<RtlRegNote> merged;
        for (const RtlRegNote& note : lpn->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead) {
            merged.push_back(note);
          }
        }
        for (const RtlRegNote& note : vpn->reg_notes) {
          if (note.kind != RtlMachineMode::kRegDead) {
            merged.push_back(note);
          }
        }
        vblk.instructions[vi] = FormatGccPrintRtlPatternAndNotes(
            vpn->pattern, merged, vpn->bracketed_notes, /*call_fusage=*/{});
        updated = true;
      }
      // Transfer pure-reader `REG_DEAD` notes (or `reorg.c:918-920` `(REG_DEAD (const_int 0))`
      // placeholders) off moved delay slots onto the subsequent pure reader where `live_cfg`
      // shows the register actually dies in the pre-`reorg` (post-`sched2`) order.
      for (size_t slot_vi : moved_slot_new_view_idxs[b]) {
        if (slot_vi >= vblk.instructions.size() || slot_vi >= bv.view_to_orig.size() ||
            slot_vi >= lblk.instructions.size() || bv.view_to_orig[slot_vi].had_placeholder_dead) {
          continue;
        }
        auto svpn = ParsePatternAndNotes(vblk.instructions[slot_vi]);
        auto slpn = ParsePatternAndNotes(lblk.instructions[slot_vi]);
        if (!svpn.ok() || !slpn.ok()) {
          continue;
        }
        absl::btree_set<int> sv_fdefs;
        absl::btree_set<int> sv_pdefs;
        CollectFullRegDefs(svpn->pattern, &sv_fdefs, &sv_pdefs);
        absl::btree_set<int> sl_dead_regs;
        for (const RtlRegNote& note : slpn->reg_notes) {
          if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
            const int r = note.datum.RegNo();
            const int nr = std::max(1, mm.HardRegNRegs(r, note.datum.mode));
            for (int k = 0; k < nr; ++k) {
              sl_dead_regs.insert(r + k);
            }
          }
        }
        std::vector<size_t> erase_slot_notes;
        absl::btree_set<int> transferred_regs;
        auto try_transfer_reg = [&](int r, int nr, size_t slot_note_idx) -> bool {
          if (r <= 0 || r >= kMipsFirstPseudoRegister || r == 28 || r == kMipsStackPointerRegno ||
              transferred_regs.contains(r)) {
            return false;
          }
          for (int k = 0; k < nr; ++k) {
            if (sv_fdefs.contains(r + k) || sv_pdefs.contains(r + k) ||
                sl_dead_regs.contains(r + k)) {
              return false;
            }
          }
          for (size_t vi2 = slot_vi + 1;
               vi2 < vblk.instructions.size() && vi2 < lblk.instructions.size(); ++vi2) {
            auto v2 = ParsePatternAndNotes(vblk.instructions[vi2]);
            auto l2 = ParsePatternAndNotes(lblk.instructions[vi2]);
            if (!v2.ok() || !l2.ok()) {
              break;
            }
            if (v2->pattern.op == RtlOp::kCall || v2->pattern.op == RtlOp::kUse ||
                (v2->pattern.op == RtlOp::kParallel && !v2->pattern.children.empty() &&
                 (v2->pattern.children[0].op == RtlOp::kCall ||
                  (v2->pattern.children[0].op == RtlOp::kSet &&
                   v2->pattern.children[0].children.size() == 2 &&
                   v2->pattern.children[0].SetSrc().op == RtlOp::kCall)))) {
              break;
            }
            absl::btree_set<int> d2_f;
            absl::btree_set<int> d2_p;
            CollectFullRegDefs(v2->pattern, &d2_f, &d2_p);
            bool v2_defs_r = false;
            for (int k = 0; k < nr; ++k) {
              if (d2_f.contains(r + k) || d2_p.contains(r + k)) {
                v2_defs_r = true;
                break;
              }
            }
            if (v2_defs_r) {
              break;
            }
            std::optional<RtlRegNote> l2_dead_note;
            for (const RtlRegNote& note : l2->reg_notes) {
              if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
                  note.datum.RegNo() == r) {
                l2_dead_note = note;
                break;
              }
            }
            if (l2_dead_note.has_value()) {
              if (!HasRegDeadNoteCovering(v2->reg_notes, r, mm)) {
                v2->reg_notes.push_back(*l2_dead_note);
                vblk.instructions[vi2] = FormatGccPrintRtlPatternAndNotes(
                    v2->pattern, v2->reg_notes, v2->bracketed_notes, /*call_fusage=*/{});
                erase_slot_notes.push_back(slot_note_idx);
                for (int k = 0; k < nr; ++k) {
                  transferred_regs.insert(r + k);
                }
                bv.alignment_touched_view_idxs.insert(slot_vi);
                bv.alignment_touched_view_idxs.insert(vi2);
                updated = true;
                return true;
              }
              break;
            }
          }
          return false;
        };
        for (size_t ni = 0; ni < svpn->reg_notes.size(); ++ni) {
          const RtlRegNote& note = svpn->reg_notes[ni];
          if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg) {
            const int r = note.datum.RegNo();
            const int nr = std::max(1, mm.HardRegNRegs(r, note.datum.mode));
            try_transfer_reg(r, nr, ni);
          }
        }
        if (!erase_slot_notes.empty()) {
          absl::btree_set<size_t> erase_set(erase_slot_notes.begin(), erase_slot_notes.end());
          std::vector<RtlRegNote> kept;
          kept.reserve(svpn->reg_notes.size());
          for (size_t ni = 0; ni < svpn->reg_notes.size(); ++ni) {
            if (!erase_set.contains(ni)) {
              kept.push_back(svpn->reg_notes[ni]);
            }
          }
          vblk.instructions[slot_vi] = FormatGccPrintRtlPatternAndNotes(
              svpn->pattern, kept, svpn->bracketed_notes, /*call_fusage=*/{});
        }
      }
      if (updated) {
        std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage(vblk.rtl_insns.size());
        for (size_t vi = 0; vi < vblk.rtl_insns.size(); ++vi) {
          if (vblk.rtl_insns[vi].has_explicit_call_fusage) {
            saved_fusage[vi] = vblk.rtl_insns[vi].call_fusage;
          }
        }
        vblk.SyncRtlInsnsFromInstructions();
        for (size_t vi = 0; vi < vblk.rtl_insns.size() && vi < saved_fusage.size(); ++vi) {
          if (saved_fusage[vi].has_value() && !vblk.rtl_insns[vi].has_explicit_call_fusage) {
            vblk.rtl_insns[vi].has_explicit_call_fusage = true;
            vblk.rtl_insns[vi].call_fusage = *saved_fusage[vi];
          }
        }
      }
    }
  }
}

struct ConstDoubleLoadSpan {
  size_t orig_start = 0;
  size_t orig_len = 0;
  size_t view_start = 0;
  std::string replacement_rtl;
};

RtlExpr MakeVoidConstDoubleExpr(int64_t lo_word, int64_t hi_word) {
  RtlExpr cd;
  cd.op = RtlOp::kConstDouble;
  cd.mode = RtlMachineMode::kNone;
  cd.children = {RtlExpr::MakeCc0()};
  cd.int_val = lo_word;
  cd.int_val_high = hi_word;
  return cd;
}

bool HasRegUnusedNote(const std::vector<RtlRegNote>& notes) {
  for (const RtlRegNote& note : notes) {
    if (note.kind == RtlMachineMode::kRegUnused) {
      return true;
    }
  }
  return false;
}

bool IsPlainNonSpecialSiGprDest(const RtlExpr& expr, int* out_reg) {
  if (expr.op != RtlOp::kReg || expr.IsArgPointerReg() || expr.mode != RtlMachineMode::kSi ||
      expr.flags.volatil || expr.flags.integrated) {
    return false;
  }
  const int r = expr.RegNo();
  if (r < 2 || r > 25) {
    return false;
  }
  *out_reg = r;
  return true;
}

std::vector<ConstDoubleLoadSpan> CollectConstDoubleLoadSpans(const RtlBasicBlock& orig_blk,
                                                             const Stage0BlockDelayView& bv) {
  const size_t n = orig_blk.instructions.size();
  std::vector<RtlPatternAndNotes> parsed(n);
  std::vector<bool> ok(n, false);
  for (size_t i = 0; i < n; ++i) {
    if (bv.seqs.contains(i)) {
      continue;
    }
    auto pn = ParsePatternAndNotes(orig_blk.instructions[i]);
    if (pn.ok()) {
      parsed[i] = std::move(*pn);
      ok[i] = true;
    }
  }

  // Map each non-delay-slot original insn index to its index in `view_cfg.rpo_blocks[b]`.
  absl::btree_map<size_t, size_t> orig_to_view;
  for (size_t vi = 0; vi < bv.view_to_orig.size(); ++vi) {
    if (!bv.view_to_orig[vi].is_delay_slot && !bv.view_to_orig[vi].is_epilogue_delay &&
        !bv.view_to_orig[vi].is_synthetic) {
      orig_to_view[bv.view_to_orig[vi].orig_insn_idx] = vi;
    }
  }

  // `varasm.c:1857-1895` (`immed_double_const`), `config/mips/mips.md:3208-3221`
  // (`large_int`), `config/mips/mips.c:1047-1057` (`mips_move_1word`): unsigned
  // 32-bit immediates with bit 31 set (`v < 0`) are represented as
  // `(const_double:VOID (cc0) v 0)`, never split by `large_int`, and emitted as
  // `li $r, v` -> GAS `lui $r, HI>>16; ori $r, $r, LO`.
  std::vector<ConstDoubleLoadSpan> spans;
  for (size_t i = 0; i + 1 < n; ++i) {
    if (!ok[i] || !ok[i + 1]) {
      continue;
    }
    const RtlExpr& p0 = parsed[i].pattern;
    const RtlExpr& p1 = parsed[i + 1].pattern;
    int r0 = -1;
    int r1 = -1;
    if (!p0.IsSet() || !p1.IsSet() || !IsPlainNonSpecialSiGprDest(p0.SetDest(), &r0) ||
        !IsPlainNonSpecialSiGprDest(p1.SetDest(), &r1) || r1 != r0 ||
        p0.SetSrc().op != RtlOp::kConstInt || p1.SetSrc().op != RtlOp::kIor ||
        p1.SetSrc().mode != RtlMachineMode::kSi || p1.SetSrc().children.size() != 2 ||
        p1.SetSrc().children[0].op != RtlOp::kReg ||
        p1.SetSrc().children[0].mode != RtlMachineMode::kSi ||
        p1.SetSrc().children[0].RegNo() != r0 || p1.SetSrc().children[1].op != RtlOp::kConstInt ||
        HasRegUnusedNote(parsed[i + 1].reg_notes)) {
      continue;
    }
    const int64_t hi = p0.SetSrc().int_val;
    const int64_t lo = p1.SetSrc().children[1].int_val;
    if (hi >= 0 || (hi & 0xffff) != 0 || lo <= 0 || lo > 0xffff) {
      continue;
    }
    const int32_t combined =
        static_cast<int32_t>(static_cast<uint32_t>(hi) | static_cast<uint32_t>(lo));
    if (combined >= 0) {
      continue;
    }
    const auto vit = orig_to_view.find(i);
    if (vit == orig_to_view.end()) {
      continue;
    }
    const size_t vs = vit->second;
    if (vs + 2 > bv.view_to_orig.size() || bv.view_to_orig[vs + 1].is_delay_slot ||
        bv.view_to_orig[vs + 1].is_epilogue_delay ||
        bv.view_to_orig[vs + 1].orig_insn_idx != i + 1) {
      continue;
    }
    RtlExpr set_expr = RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kSi, r0),
                                        MakeVoidConstDoubleExpr(combined, 0));
    spans.push_back(ConstDoubleLoadSpan{
        .orig_start = i,
        .orig_len = 2,
        .view_start = vs,
        .replacement_rtl = FormatGccPrintRtlPatternAndNotes(set_expr, {}, {}, {}),
    });
    ++i;
  }
  return spans;
}

RtlBasicBlock ApplyConstDoubleSpansToBlock(const RtlBasicBlock& src_blk,
                                           absl::Span<const ConstDoubleLoadSpan> spans,
                                           const std::vector<bool>& mask, bool use_view_indices,
                                           std::optional<size_t> omit_view_idx = std::nullopt) {
  absl::btree_map<size_t, size_t> start_to_span;
  for (size_t k = 0; k < spans.size(); ++k) {
    if (mask[k]) {
      start_to_span[use_view_indices ? spans[k].view_start : spans[k].orig_start] = k;
    }
  }
  RtlBasicBlock out = src_blk;
  out.instructions.clear();
  out.instructions.reserve(src_blk.instructions.size());
  std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage;
  saved_fusage.reserve(src_blk.instructions.size());

  size_t pos = 0;
  while (pos < src_blk.instructions.size()) {
    if (use_view_indices && omit_view_idx.has_value() && pos == *omit_view_idx) {
      ++pos;
      continue;
    }
    if (const auto it = start_to_span.find(pos); it != start_to_span.end()) {
      const ConstDoubleLoadSpan& sp = spans[it->second];
      out.instructions.push_back(sp.replacement_rtl);
      saved_fusage.push_back(std::nullopt);
      pos += sp.orig_len;
      continue;
    }
    out.instructions.push_back(src_blk.instructions[pos]);
    if (pos < src_blk.rtl_insns.size() && src_blk.rtl_insns[pos].has_explicit_call_fusage) {
      saved_fusage.push_back(src_blk.rtl_insns[pos].call_fusage);
    } else {
      saved_fusage.push_back(std::nullopt);
    }
    ++pos;
  }
  out.SyncRtlInsnsFromInstructions();
  for (size_t i = 0; i < out.rtl_insns.size() && i < saved_fusage.size(); ++i) {
    if (saved_fusage[i].has_value() && !out.rtl_insns[i].has_explicit_call_fusage) {
      out.rtl_insns[i].has_explicit_call_fusage = true;
      out.rtl_insns[i].call_fusage = std::move(*saved_fusage[i]);
    }
  }
  return out;
}

}  // namespace

void CollapseSched1UnsplitConstDoubleLoads(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);
  const MachineModel mm = MachineModel::FromProfile(profile);
  if (flags.opt_level <= 0 || !flags.RunsSched1() || !flags.RunsSched2() || mm.Target64Bit()) {
    return;
  }

  const size_t num_blocks = cfg->rpo_blocks.size();
  RtlControlFlowGraph view_cfg;
  RtlControlFlowGraph live_cfg;
  std::vector<Stage0BlockDelayView> block_views;
  BuildStage0UnpackedDelayViewCfg(*cfg, profile, &view_cfg, &live_cfg, &block_views,
                                  /*allow_annulled_tail_branch=*/true);
  const RegFacts facts_s2 = RegFacts::FromCfg(view_cfg, profile, Phase::kSched2);
  const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(view_cfg, live_cfg, mm);

  for (size_t b = 0; b < num_blocks; ++b) {
    if (!block_views[b].eligible) {
      continue;
    }
    const RtlBasicBlock& vblk = view_cfg.rpo_blocks[b];
    const ParsedSchedRegion base_region = ParseBlockForSchedModel(vblk, b, Phase::kSched2);
    if (!base_region.valid || base_region.has_mid_block_loop_barrier ||
        base_region.sched_block.size() <= 2) {
      continue;
    }
    const std::vector<ConstDoubleLoadSpan> spans =
        CollectConstDoubleLoadSpans(cfg->rpo_blocks[b], block_views[b]);
    if (spans.empty()) {
      continue;
    }

    absl::btree_set<int> live_at_end_s2 = block_views[b].annulled_slot_live_at_end;
    for (int succ : vblk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    const Sched1ProjectionContext base_proj_ctx = BuildSched1ProjectionContextForBlock(b, cfg_ctx);

    auto check_single_view = [&](const RtlBasicBlock& candidate_vblk,
                                 const absl::btree_set<int>& live_end,
                                 bool require_sched2_invert) -> bool {
      const ParsedSchedRegion region = ParseBlockForSchedModel(candidate_vblk, b, Phase::kSched2);
      if (!region.valid || region.has_mid_block_loop_barrier || region.sched_block.size() <= 1) {
        return false;
      }
      if (require_sched2_invert &&
          !InvertBlockOrder(region.sched_block, facts_s2, mm, Phase::kSched2, live_end)
               .has_value()) {
        return false;
      }
      const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(region.sched_block, b);
      const int body_len = bounds.epilogue_begin - bounds.prologue_end;
      if (body_len <= 1) {
        return true;
      }
      std::vector<Insn> body;
      body.reserve(static_cast<size_t>(body_len));
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
        Insn insn = region.sched_block[static_cast<size_t>(i)];
        insn.luid = i - bounds.prologue_end;
        body.push_back(std::move(insn));
      }
      Sched1ProjectionContext proj_ctx = base_proj_ctx;
      proj_ctx.excluded_prologue_body_indices =
          FindInterleavedPrologueSaveBodyIndices(region.sched_block, bounds, b, mm);
      Sched1ProjectionContext unsplit_ctx = proj_ctx;
      unsplit_ctx.allow_post_call_combine_regs_split = false;
      unsplit_ctx.allow_general_combine_regs_split = false;
      const Sched1ProjectionView unsplit_view =
          ProjectPostReloadBlockToSched1View(body, mm, live_end, unsplit_ctx);
      if (unsplit_view.core_insns.size() <= 1 ||
          IsAchievable(unsplit_view.core_insns, unsplit_view.facts_s1, mm, Phase::kSched1,
                       unsplit_view.live_at_end_s1)) {
        return true;
      }
      if (HasSched1AchievablePreimageForSched2Block(region.sched_block, b, bounds, unsplit_view,
                                                    unsplit_ctx, cfg_ctx, facts_s2, mm, live_end,
                                                    /*allow_pure_reader_reg_dead_transfer=*/true)) {
        return true;
      }
      if (!SelectMinimalCombineRegsRmwSplits(body, proj_ctx, live_end, mm,
                                             /*allow_general=*/true)
               .empty()) {
        return true;
      }
      return false;
    };

    // If `reorg.c:fill_simple_delay_slots` sank an independent insn into a
    // non-return branch's delay slot, its pre-`reorg` position is not restored
    // until Stage 1; test with and without that unpacked slot consistently for
    // both the uncollapsed baseline and collapsed trials.
    absl::btree_set<int> live_end_without_jump_slot = live_at_end_s2;
    const std::optional<size_t> jump_slot_vi =
        block_views[b].unpacked_non_return_jump_slot_view_idx;
    if (jump_slot_vi.has_value() && *jump_slot_vi < vblk.instructions.size()) {
      if (auto spn = ParsePatternAndNotes(vblk.instructions[*jump_slot_vi]); spn.ok()) {
        CollectRegReads(spn->pattern, &live_end_without_jump_slot);
      }
    }

    auto check_mask = [&](const std::vector<bool>& m, bool require_sched2_invert) -> bool {
      const RtlBasicBlock trial_vblk =
          ApplyConstDoubleSpansToBlock(vblk, spans, m, /*use_view_indices=*/true);
      if (check_single_view(trial_vblk, live_at_end_s2, require_sched2_invert)) {
        return true;
      }
      if (jump_slot_vi.has_value()) {
        const RtlBasicBlock trimmed_vblk =
            ApplyConstDoubleSpansToBlock(vblk, spans, m, /*use_view_indices=*/true, jump_slot_vi);
        if (check_single_view(trimmed_vblk, live_end_without_jump_slot, require_sched2_invert)) {
          return true;
        }
      }
      return false;
    };

    const std::vector<bool> empty_mask(spans.size(), false);
    if (check_mask(empty_mask, /*require_sched2_invert=*/false)) {
      continue;
    }

    std::vector<bool> mask(spans.size(), true);
    if (!check_mask(mask, /*require_sched2_invert=*/true)) {
      continue;
    }

    // Greedily drop any span not needed for `kSched1` achievability.
    for (size_t k = 0; k < spans.size(); ++k) {
      mask[k] = false;
      if (!check_mask(mask, /*require_sched2_invert=*/true)) {
        mask[k] = true;
      }
    }
    bool any_selected = false;
    for (bool bit : mask) {
      if (bit) {
        any_selected = true;
        break;
      }
    }
    if (!any_selected) {
      continue;
    }
    cfg->rpo_blocks[b] =
        ApplyConstDoubleSpansToBlock(cfg->rpo_blocks[b], spans, mask, /*use_view_indices=*/false);
  }
}

namespace {

// If a later pure-reader store `j > i` of a resolved dead register `r`
// `(set (mem M) (reg r))` moves before `i` in the pre-`sched2` order, `r` no
// longer dies at `j`, so `local-alloc.c:540-603, 990-1000` (`validate_equiv_mem`)
// rejects any store-`REG_EQUIV (mem M)` note that Stage 0 speculatively attached
// to the initializer of `r`. Strips that spurious `REG_EQUIV (mem M)` note from
// `seq` and records every modified initializer index in `*touched_sched_idxs`.
void StripSpuriousStoreMemRegEquivForMovedReaders(
    absl::Span<const std::pair<int, int>> resolved_insn_regs, int prologue_end, int epilogue_begin,
    absl::Span<const InsnRegReadDefDeaths> insn_info, const PostReloadStackLayout& stack_layout,
    const MachineModel& mm, absl::Span<const int> pos_in_cand, std::vector<Insn>* seq,
    absl::btree_set<int>* touched_sched_idxs) {
  for (const auto& [i, r] : resolved_insn_regs) {
    for (int j = i + 1; j < epilogue_begin; ++j) {
      if (insn_info[static_cast<size_t>(j)].defs.contains(r)) {
        break;
      }
      if (!pos_in_cand.empty() &&
          pos_in_cand[static_cast<size_t>(j)] >= pos_in_cand[static_cast<size_t>(i)]) {
        continue;
      }
      const RtlExpr& jpat = (*seq)[static_cast<size_t>(j)].pn.pattern;
      if (jpat.op == RtlOp::kSet && jpat.children.size() == 2 && jpat.SetDest().op == RtlOp::kMem &&
          !jpat.SetDest().children.empty() && jpat.SetSrc().op == RtlOp::kReg &&
          !jpat.SetSrc().IsArgPointerReg() && jpat.SetSrc().RegNo() == r) {
        const int init_idx = FindUniqueInitPosForStoreInSeq(*seq, j, r, mm);
        if (init_idx >= prologue_end &&
            HasMatchingMemRegEquivNote((*seq)[static_cast<size_t>(init_idx)], jpat.SetDest(),
                                       stack_layout)) {
          auto& inotes = (*seq)[static_cast<size_t>(init_idx)].pn.reg_notes;
          inotes.erase(std::remove_if(inotes.begin(), inotes.end(),
                                      [&](const RtlRegNote& note) {
                                        return note.kind == RtlMachineMode::kRegEquiv &&
                                               SameMemSlotIgnoringRegNames(
                                                   note.datum, jpat.SetDest(), stack_layout);
                                      }),
                       inotes.end());
          touched_sched_idxs->insert(init_idx);
        }
      }
    }
  }
}

// `local-alloc.c:575-600, 1049-1055` (`validate_equiv_mem`): transferring
// `(REG_DEAD r)` onto `to_idx` invalidates any speculative Stage-0
// `(REG_EQUIV (mem M))` on `to_idx` itself (or on an earlier load `p` whose
// dest is still live at `to_idx` in `cand_in_base`) whenever `M` references a
// hard register covered by `r`.
void StripInvalidatedMemRegEquivAfterDeadTransfers(
    absl::Span<const std::pair<int, RtlRegNote>> dead_targets, int prologue_end,
    absl::Span<const int> cand_in_base, absl::Span<const int> pos_in_cand,
    absl::Span<const InsnRegReadDefDeaths> insn_info, const MachineModel& mm,
    std::vector<Insn>* seq, absl::btree_set<int>* touched_sched_idxs) {
  for (const auto& [to_idx, note] : dead_targets) {
    if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
      continue;
    }
    const int dead_r = note.datum.RegNo();
    if (dead_r < 0 || dead_r >= kMipsFirstPseudoRegister) {
      continue;
    }
    const int dead_nr = mm.HardRegNRegs(dead_r, note.datum.mode);
    auto mem_mentions_dead = [&](const RtlExpr& mem_expr) {
      for (int k = 0; k < dead_nr; ++k) {
        if (mem_expr.ContainsReg(dead_r + k)) {
          return true;
        }
      }
      return false;
    };
    const int to_pos = pos_in_cand[static_cast<size_t>(to_idx)];
    for (int p = to_pos; p >= prologue_end; --p) {
      const int si = cand_in_base[static_cast<size_t>(p)];
      auto& notes = (*seq)[static_cast<size_t>(si)].pn.reg_notes;
      const RtlExpr* s = SingleSetOf((*seq)[static_cast<size_t>(si)].pn.pattern);
      if (s == nullptr || s->SetDest().op != RtlOp::kReg ||
          s->SetDest().RegNo() >= kMipsFirstPseudoRegister) {
        continue;
      }
      const int dst_r = s->SetDest().RegNo();
      const int dst_nr = mm.HardRegNRegs(dst_r, s->SetDest().mode);
      bool dst_live_at_to = (p == to_pos);
      if (!dst_live_at_to) {
        dst_live_at_to = true;
        for (int q = p + 1; q <= to_pos; ++q) {
          const int qi = cand_in_base[static_cast<size_t>(q)];
          if (HasRegDeadNoteCovering((*seq)[static_cast<size_t>(qi)].pn.reg_notes, dst_r, mm)) {
            dst_live_at_to = false;
            break;
          }
          for (int k = 0; k < dst_nr; ++k) {
            if (insn_info[static_cast<size_t>(qi)].defs.contains(dst_r + k)) {
              dst_live_at_to = false;
              break;
            }
          }
          if (!dst_live_at_to) {
            break;
          }
        }
      }
      if (!dst_live_at_to) {
        continue;
      }
      std::vector<RtlRegNote> kept;
      kept.reserve(notes.size());
      bool removed = false;
      for (const RtlRegNote& n : notes) {
        if (n.kind == RtlMachineMode::kRegEquiv && n.datum.op == RtlOp::kMem &&
            mem_mentions_dead(n.datum)) {
          removed = true;
          continue;
        }
        kept.push_back(n);
      }
      if (removed) {
        notes = std::move(kept);
        touched_sched_idxs->insert(si);
      }
    }
  }
}

// Companion to `CollectSched2ObstructedMemRegEquivAdditions` (`local-alloc.c:575,
// 580, 589-599, 1049-1055`): strips speculative Stage-0 `REG_EQUIV (mem ...)`
// notes on non-incoming-arg `MEM` loads when `sched2` reordered `REG_DEAD base_r`
// or `REG_DEAD dst_r` so `validate_equiv_mem` spuriously passed in post-`sched2`
// order (`transferred_base`) while failing in the pre-`sched2` `kSched1`
// preimage (`cand_in_base`).
void StripSched2SpuriousLoadMemRegEquivNotes(
    int prologue_end, int epilogue_begin, absl::Span<const int> cand_in_base,
    absl::Span<const int> pos_in_cand, const RegFacts& facts_s2, const MachineModel& mm,
    const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_prologue_body_indices, std::vector<Insn>* transferred_base,
    absl::btree_set<int>* stripped_equiv_targets) {
  const int n = static_cast<int>(transferred_base->size());
  if (static_cast<int>(cand_in_base.size()) != n || static_cast<int>(pos_in_cand.size()) != n) {
    return;
  }
  std::vector<InsnRegReadDefDeaths> info(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    info[static_cast<size_t>(i)] =
        CollectInsnRegReadDefDeaths((*transferred_base)[static_cast<size_t>(i)], mm);
  }
  std::vector<Insn> cand_seq;
  cand_seq.reserve(static_cast<size_t>(n));
  for (int pos = 0; pos < n; ++pos) {
    cand_seq.push_back(
        (*transferred_base)[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])]);
  }
  absl::btree_set<int> excluded_orig_ids;
  for (int i = 0; i < prologue_end; ++i) {
    excluded_orig_ids.insert(i);
  }
  for (int rel : excluded_prologue_body_indices) {
    excluded_orig_ids.insert(prologue_end + rel);
  }
  for (int oid = prologue_end; oid < epilogue_begin; ++oid) {
    if (excluded_orig_ids.contains(oid)) {
      continue;
    }
    const RtlExpr& pat = (*transferred_base)[static_cast<size_t>(oid)].pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kReg ||
        pat.SetSrc().op != RtlOp::kMem || pat.SetSrc().children.empty()) {
      continue;
    }
    const int dst_r = pat.SetDest().RegNo();
    if (dst_r < 0 || dst_r >= kMipsFirstPseudoRegister) {
      continue;
    }
    int64_t arg_off = 0;
    if (stack_layout.valid && TryExtractPlainStackSlotOffset(pat.SetSrc().children[0], &arg_off) &&
        arg_off >= stack_layout.frame_size) {
      continue;
    }
    if (!HasMatchingMemRegEquivNote((*transferred_base)[static_cast<size_t>(oid)], pat.SetSrc(),
                                    stack_layout)) {
      continue;
    }
    if (!HasLoadEquivInvalidatorMovedIntoWindow(
            oid, pat.SetSrc(), dst_r, prologue_end, epilogue_begin, *transferred_base, cand_in_base,
            pos_in_cand, info, facts_s2, mm, stack_layout, excluded_orig_ids,
            /*allow_moved_store_invalidator=*/true)) {
      continue;
    }
    const int cpos = pos_in_cand[static_cast<size_t>(oid)];
    if (ValidateEquivMemForPostReloadGate(cand_seq, cand_in_base, cpos, dst_r, pat.SetSrc(),
                                          facts_s2, mm, stack_layout, excluded_orig_ids)) {
      continue;
    }
    auto& notes = (*transferred_base)[static_cast<size_t>(oid)].pn.reg_notes;
    const RtlExpr norm_mem = NormalizeMemForRegEquivMatch(pat.SetSrc(), stack_layout);
    std::vector<RtlRegNote> kept;
    kept.reserve(notes.size());
    bool removed = false;
    for (const RtlRegNote& note : notes) {
      if (note.kind == RtlMachineMode::kRegEquiv &&
          (note.datum == pat.SetSrc() ||
           NormalizeMemForRegEquivMatch(note.datum, stack_layout) == norm_mem)) {
        removed = true;
        continue;
      }
      kept.push_back(note);
    }
    if (removed) {
      notes = std::move(kept);
      stripped_equiv_targets->insert(oid);
    }
  }
}

// Companion to `StripSched2SpuriousLoadMemRegEquivNotes` (`local-alloc.c:540-603,
// 990-1000` `validate_equiv_mem`, `memref_used_between_p`, `update_equiv_regs`):
// strips speculative Stage-0 `REG_EQUIV (mem ...)` notes placed on the unique
// initializer of a dying `MEM` store when `sched2` moved an address-base
// write/death, call, or overlapping `MEM` access out of `[init, store)` so
// `validate_equiv_mem` spuriously passed in post-`sched2` order while failing
// in the pre-`sched2` `kSched1` preimage (`cand_in_base`).
void StripSched2SpuriousStoreMemRegEquivNotes(
    int prologue_end, int epilogue_begin, absl::Span<const int> cand_in_base,
    absl::Span<const int> pos_in_cand, const RegFacts& facts_s2, const MachineModel& mm,
    const PostReloadStackLayout& stack_layout,
    const absl::btree_set<int>& excluded_prologue_body_indices, std::vector<Insn>* transferred_base,
    absl::btree_set<int>* stripped_equiv_targets) {
  const int n = static_cast<int>(transferred_base->size());
  if (static_cast<int>(cand_in_base.size()) != n || static_cast<int>(pos_in_cand.size()) != n) {
    return;
  }
  std::vector<InsnRegReadDefDeaths> info(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    info[static_cast<size_t>(i)] =
        CollectInsnRegReadDefDeaths((*transferred_base)[static_cast<size_t>(i)], mm);
  }
  std::vector<Insn> cand_seq;
  cand_seq.reserve(static_cast<size_t>(n));
  for (int pos = 0; pos < n; ++pos) {
    cand_seq.push_back(
        (*transferred_base)[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])]);
  }
  absl::btree_set<int> excluded_orig_ids;
  for (int i = 0; i < prologue_end; ++i) {
    excluded_orig_ids.insert(i);
  }
  for (int rel : excluded_prologue_body_indices) {
    excluded_orig_ids.insert(prologue_end + rel);
  }
  for (int oid = prologue_end; oid < epilogue_begin; ++oid) {
    if (excluded_orig_ids.contains(oid)) {
      continue;
    }
    const Insn& store_insn = (*transferred_base)[static_cast<size_t>(oid)];
    const RtlExpr& pat = store_insn.pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kMem ||
        pat.SetDest().children.empty() || pat.SetSrc().op != RtlOp::kReg ||
        pat.SetSrc().IsArgPointerReg()) {
      continue;
    }
    const int src_r = pat.SetSrc().RegNo();
    if (src_r < 0 || src_r >= kMipsFirstPseudoRegister ||
        !HasRegDeadNoteCovering(store_insn.pn.reg_notes, src_r, mm)) {
      continue;
    }
    const int store_pos = pos_in_cand[static_cast<size_t>(oid)];
    const int init_pos = FindUniqueInitPosForStoreInSeq(cand_seq, store_pos, src_r, mm);
    if (init_pos < 0) {
      continue;
    }
    const int init_oid = cand_in_base[static_cast<size_t>(init_pos)];
    if (!HasMatchingMemRegEquivNote((*transferred_base)[static_cast<size_t>(init_oid)],
                                    pat.SetDest(), stack_layout)) {
      continue;
    }
    if (!HasStoreEquivInvalidatorMovedIntoWindow(
            oid, init_pos, pat.SetDest(), prologue_end, epilogue_begin, *transferred_base,
            cand_in_base, pos_in_cand, info, facts_s2, mm, stack_layout, excluded_orig_ids)) {
      continue;
    }
    const bool valid_in_cand =
        ValidateEquivMemForPostReloadGate(cand_seq, cand_in_base, init_pos, src_r, pat.SetDest(),
                                          facts_s2, mm, stack_layout, excluded_orig_ids) &&
        !MemrefUsedBetweenForPostReloadGate(cand_seq, cand_in_base, init_pos, store_pos,
                                            pat.SetDest(), facts_s2, stack_layout,
                                            excluded_orig_ids);
    if (valid_in_cand) {
      continue;
    }
    auto& notes = (*transferred_base)[static_cast<size_t>(init_oid)].pn.reg_notes;
    const RtlExpr norm_mem = NormalizeMemForRegEquivMatch(pat.SetDest(), stack_layout);
    std::vector<RtlRegNote> kept;
    kept.reserve(notes.size());
    bool removed = false;
    for (const RtlRegNote& note : notes) {
      if (note.kind == RtlMachineMode::kRegEquiv &&
          (note.datum == pat.SetDest() ||
           NormalizeMemForRegEquivMatch(note.datum, stack_layout) == norm_mem)) {
        removed = true;
        continue;
      }
      kept.push_back(note);
    }
    if (removed) {
      notes = std::move(kept);
      stripped_equiv_targets->insert(init_oid);
    }
  }
}

// Inverts `reorg.c:918-920` when `sched2` moved a pure reader `j` past a
// delay-slot insn `i` without updating `REG_DEAD` (`sched.c:3415-3611`), and
// `reorg.c:emit_delay_sequence` subsequently zeroed `i`'s `(REG_DEAD r)` to
// `(REG_DEAD (const_int 0))`. Resolves such live-after-`i` `(const_int 0)`
// placeholders in-place onto `i` ONLY when the resolved block admits a
// 3-gate-verified `kSched2` preimage (`IsAchievable(kSched1)` + exact `kSched2`
// replay + `PreservesPostReloadOrderSafety(..., allow_transfer=false)`).
bool TryResolveStage0DelaySlotConstZeroInPlace(
    const ParsedSchedRegion& base_region, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionContext& proj_ctx, const CfgSched1Context& cfg_ctx,
    const RegFacts& facts_s2, const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2,
    absl::Span<const AdjacentPair> reload_adjacent,
    absl::Span<const InsnRegReadDefDeaths> insn_info,
    absl::Span<const absl::btree_set<int>> live_after_base,
    const absl::btree_set<int>& alignment_touched_sched_idxs, Stage0BlockDelayView* block_view,
    RtlBasicBlock* orig_blk) {
  const int n = static_cast<int>(base_region.sched_block.size());
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return false;
  }

  std::vector<int> identity_order(static_cast<size_t>(n));
  for (int idx = 0; idx < n; ++idx) {
    identity_order[static_cast<size_t>(idx)] = idx;
  }
  std::vector<Insn> resolved_base = base_region.sched_block;
  absl::btree_set<int> resolved_sched_idxs;
  std::vector<std::pair<int, int>> resolved_insn_regs;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    if (insn_info[static_cast<size_t>(i)].is_call_or_use) {
      continue;
    }
    const size_t vi = base_region.sched_orig_indices[static_cast<size_t>(i)];
    if (vi >= block_view->view_to_orig.size() || !block_view->view_to_orig[vi].is_delay_slot ||
        block_view->view_to_orig[vi].had_placeholder_dead) {
      continue;
    }
    auto& notes = resolved_base[static_cast<size_t>(i)].pn.reg_notes;
    std::vector<size_t> zero_idxs;
    for (size_t ni = 0; ni < notes.size(); ++ni) {
      if (notes[ni].kind == RtlMachineMode::kRegDead && notes[ni].datum.op == RtlOp::kConstInt &&
          notes[ni].datum.int_val == 0) {
        zero_idxs.push_back(ni);
      }
    }
    if (zero_idxs.empty()) {
      continue;
    }
    size_t zero_cursor = 0;
    absl::btree_set<int> handled_regs;
    for (int r : insn_info[static_cast<size_t>(i)].reads) {
      if (zero_cursor >= zero_idxs.size()) {
        break;
      }
      if (r <= 0 || r >= kMipsFirstPseudoRegister || r == 28 || r == kMipsStackPointerRegno ||
          (cfg_ctx.stack_layout.frame_pointer_needed && r == kMipsFramePointerRegno) ||
          handled_regs.contains(r) || HasRegDeadNoteCovering(notes, r, mm)) {
        continue;
      }
      const std::optional<RtlExpr> reg_expr =
          FindUsedHardRegExprInPattern(resolved_base[static_cast<size_t>(i)].pn.pattern, r);
      if (!reg_expr.has_value()) {
        continue;
      }
      const int nr = mm.HardRegNRegs(r, reg_expr->mode);
      bool defs_r = false;
      bool live_after_in_base = false;
      for (int k = 0; k < nr; ++k) {
        if (insn_info[static_cast<size_t>(i)].defs.contains(r + k)) {
          defs_r = true;
          break;
        }
        if (live_after_base[static_cast<size_t>(i)].contains(r + k)) {
          live_after_in_base = true;
        }
      }
      if (defs_r || !live_after_in_base ||
          IsCrossBankFprInputReloadOperandInOrder(base_region.sched_block, identity_order, i, r,
                                                  reg_expr->mode, mm)) {
        continue;
      }
      notes[zero_idxs[zero_cursor++]].datum = *reg_expr;
      resolved_sched_idxs.insert(i);
      resolved_insn_regs.push_back({i, r});
      for (int k = 0; k < nr; ++k) {
        handled_regs.insert(r + k);
      }
    }
  }
  if (resolved_sched_idxs.empty()) {
    return false;
  }

  StripSpuriousStoreMemRegEquivForMovedReaders(
      resolved_insn_regs, prologue_end, epilogue_begin, insn_info, cfg_ctx.stack_layout, mm,
      /*pos_in_cand=*/{}, &resolved_base, &resolved_sched_idxs);

  // Build initial pre-`sched2` preferences placing every later pure reader `j > i`
  // of each resolved dead register `r` before `i`: first at `j`'s earliest
  // DAG-legal position (right after its last `kSched2` dependence target, so
  // equal-priority `kSched1` LUID ties match `08.sched`), and second immediately
  // before `i`.
  const DepGraph resolved_graph = Analyze(resolved_base, facts_s2, mm, Phase::kSched2);
  auto build_pref_with_readers_moved = [&](bool earliest_dag_pos) -> std::vector<int> {
    std::vector<int> pref(static_cast<size_t>(n));
    for (int idx = 0; idx < n; ++idx) {
      pref[static_cast<size_t>(idx)] = idx;
    }
    for (const auto& [i, r] : resolved_insn_regs) {
      std::vector<int> later_readers;
      for (int j = i + 1; j < epilogue_begin; ++j) {
        if (insn_info[static_cast<size_t>(j)].defs.contains(r)) {
          break;
        }
        if (insn_info[static_cast<size_t>(j)].reads.contains(r) &&
            !insn_info[static_cast<size_t>(j)].is_call_or_use) {
          later_readers.push_back(j);
        }
      }
      for (int j : later_readers) {
        auto it_j = std::find(pref.begin(), pref.end(), j);
        auto it_i = std::find(pref.begin(), pref.end(), i);
        if (it_j == pref.end() || it_i == pref.end() || it_j <= it_i) {
          continue;
        }
        pref.erase(it_j);
        it_i = std::find(pref.begin(), pref.end(), i);
        auto insert_it = it_i;
        if (earliest_dag_pos) {
          absl::btree_set<int> deps;
          for (const Link& l : resolved_graph.log_links[static_cast<size_t>(j)]) {
            deps.insert(l.target);
          }
          auto after_last_dep = pref.begin() + prologue_end;
          for (auto it = pref.begin(); it != it_i; ++it) {
            if (deps.contains(*it)) {
              after_last_dep = it + 1;
            }
          }
          if (after_last_dep <= it_i) {
            insert_it = after_last_dep;
          }
        }
        pref.insert(insert_it, j);
      }
    }
    return pref;
  };
  Sched1ProjectionContext unsplit_ctx = proj_ctx;
  unsplit_ctx.allow_post_call_combine_regs_split = false;
  unsplit_ctx.allow_general_combine_regs_split = false;
  std::vector<std::vector<int>> cands;
  for (bool earliest : {true, false}) {
    const std::vector<int> initial_pref = build_pref_with_readers_moved(earliest);
    if (std::optional<std::vector<int>> init_cand =
            InvertBlockOrder(resolved_base, facts_s2, mm, Phase::kSched2, live_at_end_s2,
                             initial_pref, reload_adjacent);
        init_cand.has_value()) {
      cands.push_back(*init_cand);
      // Also run one fixed-point `kSched1` refinement starting from `init_cand`.
      std::vector<Insn> init_seq;
      init_seq.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        Insn insn = resolved_base[static_cast<size_t>((*init_cand)[static_cast<size_t>(pos)])];
        insn.luid = pos;
        init_seq.push_back(std::move(insn));
      }
      std::vector<Insn> init_body;
      init_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        Insn insn = init_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        init_body.push_back(std::move(insn));
      }
      Sched1ProjectionContext init_ctx = unsplit_ctx;
      init_ctx.excluded_prologue_body_indices =
          FindInterleavedPrologueSaveBodyIndices(init_seq, bounds, block_idx, mm);
      const Sched1ProjectionView init_view =
          ProjectPostReloadBlockToSched1View(init_body, mm, live_at_end_s2, init_ctx);
      std::vector<int> pref_in_init;
      if (init_view.core_insns.size() > 1 &&
          BuildPreferredSched1OrderInBase(init_seq, prologue_end, epilogue_begin, init_view,
                                          init_ctx.excluded_prologue_body_indices, mm,
                                          &pref_in_init)) {
        std::vector<int> pref_in_base(static_cast<size_t>(n));
        for (int pos = 0; pos < n; ++pos) {
          pref_in_base[static_cast<size_t>(pos)] =
              (*init_cand)[static_cast<size_t>(pref_in_init[static_cast<size_t>(pos)])];
        }
        if (std::optional<std::vector<int>> refined =
                InvertBlockOrder(resolved_base, facts_s2, mm, Phase::kSched2, live_at_end_s2,
                                 pref_in_base, reload_adjacent);
            refined.has_value()) {
          cands.push_back(std::move(*refined));
        }
      }
    }
  }
  for (auto it = cands.rbegin(); it != cands.rend(); ++it) {
    const std::vector<int>& cand_in_base = *it;
    std::vector<Insn> cand_seq;
    cand_seq.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      Insn insn = resolved_base[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
      insn.luid = pos;
      cand_seq.push_back(std::move(insn));
    }
    std::vector<Insn> cand_body;
    cand_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      Insn insn = cand_seq[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      cand_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext cand_ctx = unsplit_ctx;
    cand_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, block_idx, mm);
    const Sched1ProjectionView cand_view =
        ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
    if (cand_view.core_insns.size() <= 1 ||
        !IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm, Phase::kSched1,
                      cand_view.live_at_end_s1)) {
      continue;
    }
    const DepGraph g_cand = Analyze(cand_seq, facts_s2, mm, Phase::kSched2);
    const ScheduleResult s2_replay =
        Schedule(cand_seq, g_cand, facts_s2, mm, Phase::kSched2, live_at_end_s2);
    if (static_cast<int>(s2_replay.order.size()) != n) {
      continue;
    }
    bool replay_matches = true;
    for (int pos = 0; pos < n; ++pos) {
      if (cand_in_base[static_cast<size_t>(s2_replay.order[static_cast<size_t>(pos)])] != pos) {
        replay_matches = false;
        break;
      }
    }
    if (!replay_matches) {
      continue;
    }
    if (!PreservesPostReloadOrderSafety(resolved_base, cand_in_base, prologue_end, epilogue_begin,
                                        live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                                        /*allow_pure_reader_reg_dead_transfer=*/false,
                                        cfg_ctx.stack_layout,
                                        proj_ctx.excluded_prologue_body_indices)) {
      continue;
    }
    absl::btree_set<int> touched = alignment_touched_sched_idxs;
    touched.insert(resolved_sched_idxs.begin(), resolved_sched_idxs.end());
    CommitStage0AttributedNotesToBlock(base_region, resolved_base, touched, block_view, orig_blk);
    return true;
  }
  return false;
}

// Symmetric inverse of `SelectMinimalCombineRegsRmwSplits`:
// In GCC 2.7.2, `flow.c:2506-2515` (`mark_used_regs`) never attaches `REG_DEAD`
// to a self-modifying RMW `(set r (op r ...))` on a single multi-set pseudo,
// whereas `local-alloc.c:1266-1300` (`combine_regs`) merges distinct single-set
// pseudos onto the same hard register `r`, leaving an erased `REG_DEAD` that
// Stage 0's local RMW heuristics may speculatively restore. When all preimage
// and split passes fail to make the block `Phase::kSched1`-achievable
// (`toplev.c:3028-3053`, `sched.c:2490` `birthing_insn_p`), retracts a minimal
// subset of speculative same-mode self-set `(REG_DEAD r)` notes ONLY when both
// `Phase::kSched2` replay and `Phase::kSched1` achievability succeed.
bool TryRetractSpeculativeSelfSetRmwRegDeadNotes(
    const ParsedSchedRegion& base_region, size_t block_idx, const PostReloadBodyBounds& bounds,
    const Sched1ProjectionContext& unsplit_base_ctx, const CfgSched1Context& cfg_ctx,
    const RegFacts& facts_s2, const MachineModel& mm, const absl::btree_set<int>& live_at_end_s2,
    const absl::btree_set<int>& alignment_touched_sched_idxs, Stage0BlockDelayView* block_view,
    RtlBasicBlock* orig_blk) {
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return false;
  }
  struct SelfDeadCand {
    int sched_idx = -1;
    int regno = -1;
    RtlMachineMode mode = RtlMachineMode::kVoid;
  };
  std::vector<SelfDeadCand> cands;
  absl::btree_set<int> defined_so_far;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    const Insn& insn = base_region.sched_block[static_cast<size_t>(i)];
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(insn.pn.pattern, &fdefs, &pdefs);
    const RtlExpr* s = SingleSetOf(insn.pn.pattern);
    if (insn.kind == Insn::Kind::kInsn && s != nullptr && s->SetDest().op == RtlOp::kReg &&
        !s->SetDest().flags.integrated && !s->SetDest().flags.frame_related &&
        s->SetSrc().op != RtlOp::kReg && s->SetSrc().op != RtlOp::kMem &&
        !s->SetSrc().children.empty()) {
      const int r = s->SetDest().RegNo();
      const RtlMachineMode mode = s->SetDest().mode;
      const bool has_blocking_note =
          std::any_of(insn.pn.reg_notes.begin(), insn.pn.reg_notes.end(), [](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual ||
                   n.kind == RtlMachineMode::kRegLibcall || n.kind == RtlMachineMode::kRegRetval ||
                   n.kind == RtlMachineMode::kRegNoConflict;
          });
      const bool has_exact_self_dead =
          std::any_of(insn.pn.reg_notes.begin(), insn.pn.reg_notes.end(), [&](const RtlRegNote& n) {
            return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                   n.datum.RegNo() == r && n.datum.mode == mode;
          });
      bool reads_same_mode_r = false;
      s->SetSrc().Visit([&](const RtlExpr& e) {
        if (e.op == RtlOp::kReg && e.RegNo() == r && e.mode == mode) {
          reads_same_mode_r = true;
        }
      });
      const bool incoming_entry_arg = unsplit_base_ctx.is_entry_block &&
                                      IsAbiCallInterfaceHardReg(r) && !defined_so_far.contains(r);
      if (r >= 2 && r < kMipsFirstPseudoRegister && r != 28 && r != kMipsStackPointerRegno &&
          r != kMipsFramePointerRegno && !has_blocking_note && has_exact_self_dead &&
          reads_same_mode_r && !incoming_entry_arg) {
        cands.push_back(SelfDeadCand{.sched_idx = i, .regno = r, .mode = mode});
      }
    }
    defined_so_far.insert(fdefs.begin(), fdefs.end());
    defined_so_far.insert(pdefs.begin(), pdefs.end());
  }
  if (cands.empty()) {
    return false;
  }

  auto try_retract_subset = [&](absl::Span<const SelfDeadCand> subset, bool allow_sched2_preimage,
                                bool allow_tie_flips) -> bool {
    std::vector<Insn> updated_base = base_region.sched_block;
    absl::btree_set<int> touched;
    for (const SelfDeadCand& c : subset) {
      auto& notes = updated_base[static_cast<size_t>(c.sched_idx)].pn.reg_notes;
      const size_t before = notes.size();
      notes.erase(std::remove_if(notes.begin(), notes.end(),
                                 [&](const RtlRegNote& n) {
                                   return n.kind == RtlMachineMode::kRegDead &&
                                          n.datum.op == RtlOp::kReg && n.datum.RegNo() == c.regno &&
                                          n.datum.mode == c.mode;
                                 }),
                  notes.end());
      if (notes.size() < before) {
        touched.insert(c.sched_idx);
      }
    }
    if (touched.empty() ||
        !InvertBlockOrder(updated_base, facts_s2, mm, Phase::kSched2, live_at_end_s2).has_value()) {
      return false;
    }
    std::vector<Insn> updated_body;
    updated_body.reserve(static_cast<size_t>(body_len));
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      Insn insn = updated_base[static_cast<size_t>(i)];
      insn.luid = i - prologue_end;
      updated_body.push_back(std::move(insn));
    }
    const Sched1ProjectionView verify_view =
        ProjectPostReloadBlockToSched1View(updated_body, mm, live_at_end_s2, unsplit_base_ctx);
    if (verify_view.core_insns.size() <= 1) {
      return false;
    }
    const bool ok = allow_sched2_preimage
                        ? HasSched1AchievablePreimageForSched2Block(
                              updated_base, block_idx, bounds, verify_view, unsplit_base_ctx,
                              cfg_ctx, facts_s2, mm, live_at_end_s2,
                              /*allow_pure_reader_reg_dead_transfer=*/false, allow_tie_flips)
                        : IsAchievable(verify_view.core_insns, verify_view.facts_s1, mm,
                                       Phase::kSched1, verify_view.live_at_end_s1);
    if (!ok) {
      return false;
    }
    touched.insert(alignment_touched_sched_idxs.begin(), alignment_touched_sched_idxs.end());
    CommitStage0AttributedNotesToBlock(base_region, updated_base, touched, block_view, orig_blk);
    return true;
  };

  // Prefer single-note retraction first (minimal change), checking direct
  // `kSched1` achievability before falling back to a primary `kSched2` preimage
  // and (on bounded-size blocks) a tie-flip `kSched2` preimage.
  const std::vector<std::pair<bool, bool>> modes =
      (body_len <= 48 && cands.size() <= 10)
          ? std::vector<std::pair<bool, bool>>{{false, false}, {true, false}, {true, true}}
          : std::vector<std::pair<bool, bool>>{{false, false}, {true, false}};
  for (const auto& [allow_sched2_preimage, allow_tie_flips] : modes) {
    for (const SelfDeadCand& c : cands) {
      if (try_retract_subset(absl::MakeConstSpan(&c, 1), allow_sched2_preimage, allow_tie_flips)) {
        return true;
      }
    }
    if (cands.size() > 1 && cands.size() <= 6) {
      if (try_retract_subset(cands, allow_sched2_preimage, allow_tie_flips)) {
        return true;
      }
    }
  }
  return false;
}

// Mirrors `local-alloc.c:1266-1300, 1722-1886` (`block_alloc` -> `combine_regs`)
// and `config/mips/mips.h:1280-1284` (`MODES_TIEABLE_P`): if a block-local
// single-set instruction `i` (`dst = H_dst`, not reading `H_dst`) carries a
// pure-reader `(REG_DEAD H_u)` on its first dying same-bank tieable operand
// `H_u != H_dst` (where `H_u` was defined earlier in the same block), `local-alloc`
// would unconditionally merge `dst` into `H_u`'s quantity (`local-alloc.c:1855-1860`),
// contradicting `H_dst != H_u`. When `sched2` (`sched.c:3764`) hoisted an earlier
// conflict-free pure reader `j < i` of `H_u` ahead of `i` and `final.c` erased
// `REG_DEAD`, transferring `(REG_DEAD H_u)` from `i` back to `j` (`pos(i) < pos(j)`)
// restores the true pre-`sched2` death site while strictly verifying `kSched2`
// replay, `PreservesPostReloadOrderSafety(..., allow_transfer=false)`, and
// `IsAchievable(kSched1)`.
bool TryRepairCombineRegsPureReaderRegDeadConflicts(
    size_t block_idx, const PostReloadBodyBounds& bounds, const Sched1ProjectionContext& proj_ctx,
    const CfgSched1Context& cfg_ctx, const RegFacts& facts_s2, const MachineModel& mm,
    const absl::btree_set<int>& live_at_end_s2, absl::Span<const AdjacentPair> reload_adjacent,
    const std::vector<int>& init_cand_in_base, bool allow_moved_store_load_equiv_invalidation,
    std::vector<Insn>* base_block, absl::btree_set<int>* touched_sched_idxs) {
  const int n = static_cast<int>(base_block->size());
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1 || static_cast<int>(init_cand_in_base.size()) != n) {
    return false;
  }

  const bool fp_needed = cfg_ctx.stack_layout.frame_pointer_needed;
  auto is_allocatable_bank_reg = [&](int r) {
    return (r >= 2 && r <= 25) || (r == kMipsFramePointerRegno && !fp_needed) ||
           (r >= 32 && r <= 63);
  };
  auto is_float_mode = [](RtlMachineMode m) {
    return m == RtlMachineMode::kSf || m == RtlMachineMode::kDf;
  };
  auto modes_tieable = [&](RtlMachineMode m1, RtlMachineMode m2) {
    return is_float_mode(m1) == is_float_mode(m2);
  };
  auto collect_direct_nonmem_hard_regs = [](const RtlExpr& expr,
                                            std::vector<std::pair<int, RtlMachineMode>>* out_regs) {
    auto walk = [&](const auto& self, const RtlExpr& e) -> void {
      if (e.op == RtlOp::kMem) {
        return;
      }
      if (e.op == RtlOp::kReg && !e.IsArgPointerReg() && e.RegNo() >= 0 &&
          e.RegNo() < kMipsFirstPseudoRegister) {
        out_regs->emplace_back(e.RegNo(), e.mode);
        return;
      }
      if (e.op == RtlOp::kSubreg && !e.children.empty() && e.children[0].op == RtlOp::kReg &&
          !e.children[0].IsArgPointerReg() && e.children[0].RegNo() >= 0 &&
          e.children[0].RegNo() < kMipsFirstPseudoRegister) {
        out_regs->emplace_back(e.children[0].RegNo(), e.children[0].mode);
        return;
      }
      for (const RtlExpr& ch : e.children) {
        self(self, ch);
      }
    };
    walk(walk, expr);
  };

  struct ConflictSite {
    int insn_idx = -1;
    int regno = -1;
    size_t note_idx = 0;
  };

  auto find_conflict_at = [&](const std::vector<Insn>& seq,
                              const std::vector<InsnRegReadDefDeaths>& info, int idx,
                              int extra_dead_reg) -> std::optional<ConflictSite> {
    if (idx < prologue_end || idx >= epilogue_begin ||
        info[static_cast<size_t>(idx)].is_call_or_use) {
      return std::nullopt;
    }
    const RtlExpr* st = SingleSetOf(seq[static_cast<size_t>(idx)].pn.pattern);
    if (st == nullptr || st->SetDest().op != RtlOp::kReg || st->SetDest().IsArgPointerReg() ||
        st->SetDest().flags.integrated) {
      return std::nullopt;
    }
    const int dst_r = st->SetDest().RegNo();
    const RtlMachineMode dst_mode = st->SetDest().mode;
    if (!is_allocatable_bank_reg(dst_r)) {
      return std::nullopt;
    }
    const int dst_nr = mm.HardRegNRegs(dst_r, dst_mode);
    // `flow.c:2506-2515` + `local-alloc.c:1021, 1266-1300, 1851`: an RMW reading
    // `dst_r` WITHOUT `(REG_DEAD dst_r)` keeps the same multi-set pseudo
    // (`reg_qty != -1`, skipping `combine_regs`), EXCEPT when `idx` carries
    // `REG_EQUIV` (`reg_n_sets == 1` proves `SET_DEST` is a fresh single-set
    // pseudo whose input `dst_r` was a distinct dying pseudo whose `REG_DEAD`
    // was erased after `alter_reg` gave both `dst_r`). In both fresh-dest
    // cases, `combine_regs` tries operands in order, so an earlier dying
    // `ur != dst_r` causes a Conflict Kind 1 false merge.
    const bool has_reg_equiv =
        std::any_of(seq[static_cast<size_t>(idx)].pn.reg_notes.begin(),
                    seq[static_cast<size_t>(idx)].pn.reg_notes.end(),
                    [](const RtlRegNote& n) { return n.kind == RtlMachineMode::kRegEquiv; });
    bool reads_dst = false;
    bool self_rmw_without_death = false;
    for (int k = 0; k < dst_nr; ++k) {
      if (info[static_cast<size_t>(idx)].reads.contains(dst_r + k)) {
        reads_dst = true;
        if (!info[static_cast<size_t>(idx)].deaths.contains(dst_r + k) && !has_reg_equiv) {
          self_rmw_without_death = true;
          break;
        }
      }
    }
    if (self_rmw_without_death) {
      return std::nullopt;
    }
    // Check that `dst_r` is block-local (not live at block exit without a later
    // redefinition inside `[idx + 1, epilogue_begin)`).
    bool dst_redefined = false;
    for (int k = idx + 1; k < epilogue_begin; ++k) {
      if (info[static_cast<size_t>(k)].defs.contains(dst_r)) {
        dst_redefined = true;
        break;
      }
    }
    if (!dst_redefined && live_at_end_s2.contains(dst_r)) {
      return std::nullopt;
    }

    const auto& notes = seq[static_cast<size_t>(idx)].pn.reg_notes;
    std::vector<std::pair<int, RtlMachineMode>> rhs_regs;
    collect_direct_nonmem_hard_regs(st->SetSrc(), &rhs_regs);
    for (const auto& [ur, um] : rhs_regs) {
      if (!is_allocatable_bank_reg(ur) || (ur < 32) != (dst_r < 32) ||
          !modes_tieable(um, dst_mode)) {
        continue;
      }
      size_t found_ni = notes.size();
      bool dies_here = (ur == extra_dead_reg) || (ur == dst_r && has_reg_equiv);
      if (!dies_here) {
        for (size_t ni = 0; ni < notes.size(); ++ni) {
          if (notes[ni].kind == RtlMachineMode::kRegDead && notes[ni].datum.op == RtlOp::kReg &&
              !notes[ni].datum.IsArgPointerReg() && notes[ni].datum.RegNo() == ur) {
            dies_here = true;
            found_ni = ni;
            break;
          }
        }
      }
      if (!dies_here) {
        continue;
      }
      if (ur == dst_r) {
        return std::nullopt;
      }
      const int u_nr = mm.HardRegNRegs(ur, um);
      bool u_def_at_idx = false;
      for (int k = 0; k < u_nr; ++k) {
        if (info[static_cast<size_t>(idx)].defs.contains(ur + k)) {
          u_def_at_idx = true;
          break;
        }
      }
      if (u_def_at_idx) {
        continue;
      }
      // Verify `ur` was defined earlier in the same block (block-local quantity).
      bool u_defined_locally = false;
      for (int rev = idx - 1; rev >= prologue_end; --rev) {
        if (info[static_cast<size_t>(rev)].defs.contains(ur)) {
          u_defined_locally = !info[static_cast<size_t>(rev)].is_call_or_use;
          break;
        }
      }
      if (!u_defined_locally) {
        continue;
      }
      return ConflictSite{.insn_idx = idx, .regno = ur, .note_idx = found_ni};
    }

    // Conflict Kind 2 (`local-alloc.c:2106, 2158-2177` `find_free_reg`,
    // `#ifndef REG_ALLOC_ORDER` at `local-alloc.c:2163`, `global.c:962-985`
    // `find_reg`, `config/mips/mips.h:1192, 1203`):
    // if a pure-reader `(REG_DEAD r)` at `idx` kills a same-bank register `r`
    // strictly earlier in MIPS's default allocation order than `dst_r`, and `r`
    // is valid for `dst_r`'s mode and completely free across `dst_r`'s live
    // interval `[idx, dst_death]` (with no call crossed when `r` is call-used),
    // then neither `local-alloc` nor `global-alloc` could assign `dst_r`
    // instead of `r`. Skip when `reads_dst` because `dst_r` inherits its dying
    // input's quantity via `combine_regs` rather than allocating independently.
    if (reads_dst) {
      return std::nullopt;
    }
    int dst_death = -1;
    bool saw_call = false;
    bool dst_crosses_call = false;
    for (int k = idx + 1; k < epilogue_begin; ++k) {
      if (seq[static_cast<size_t>(k)].kind == Insn::Kind::kCall) {
        saw_call = true;
        bool call_clobbers_dst = false;
        for (int w = 0; w < dst_nr; ++w) {
          if (mm.CallUsed(dst_r + w)) {
            call_clobbers_dst = true;
            break;
          }
        }
        if (call_clobbers_dst) {
          break;
        }
      }
      bool k_reads_dst = false;
      bool k_defs_dst = false;
      bool k_deaths_dst = false;
      for (int w = 0; w < dst_nr; ++w) {
        if (info[static_cast<size_t>(k)].reads.contains(dst_r + w)) {
          k_reads_dst = true;
        }
        if (info[static_cast<size_t>(k)].defs.contains(dst_r + w)) {
          k_defs_dst = true;
        }
        if (info[static_cast<size_t>(k)].deaths.contains(dst_r + w)) {
          k_deaths_dst = true;
        }
      }
      if (k_reads_dst) {
        dst_death = k;
        if (saw_call) {
          dst_crosses_call = true;
        }
        if (k_deaths_dst && !k_defs_dst) {
          break;
        }
      }
      if (k_defs_dst && !k_reads_dst) {
        break;
      }
    }
    if (dst_death > idx) {
      auto check_lower_order_dead_reg = [&](int r, RtlMachineMode rm, size_t ni) -> bool {
        if (!is_allocatable_bank_reg(r) || (r < 32) != (dst_r < 32) || r >= dst_r) {
          return false;
        }
        if (dst_crosses_call && mm.CallUsed(r)) {
          return false;
        }
        const int r_nr = mm.HardRegNRegs(r, dst_mode);
        if (r_nr > 1 && (r % 2) != 0) {
          return false;
        }
        if (r >= 32 && mm.HardRegNRegs(32, RtlMachineMode::kDf) > 1 && (r % 2) != 0) {
          return false;
        }
        if (r < 32 && r != kMipsFramePointerRegno && r + r_nr > 26) {
          return false;
        }
        if (r >= 32 && r + r_nr > 64) {
          return false;
        }
        for (int w = 0; w < r_nr; ++w) {
          if (info[static_cast<size_t>(idx)].defs.contains(r + w)) {
            return false;
          }
          if (dst_crosses_call && mm.CallUsed(r + w)) {
            return false;
          }
        }
        // Check `r` is completely unused over `(idx, dst_death]` (free for `dst_r`).
        // When `r == extra_dead_reg`, we are evaluating the hypothesis that `r`
        // dies at `idx` (so all remaining pure reads of `r` in the same live
        // range are placed before `idx`); only a redefinition of `r` in
        // `(idx, dst_death]` makes `r` busy.
        for (int k = idx + 1; k <= dst_death; ++k) {
          for (int w = 0; w < r_nr; ++w) {
            if (info[static_cast<size_t>(k)].defs.contains(r + w)) {
              return false;
            }
            if (r != extra_dead_reg && info[static_cast<size_t>(k)].reads.contains(r + w)) {
              return false;
            }
          }
        }
        // Verify `r` was defined earlier in `[prologue_end, idx)` (not an ABI arg).
        bool r_defined_locally = false;
        for (int rev = idx - 1; rev >= prologue_end; --rev) {
          if (info[static_cast<size_t>(rev)].defs.contains(r)) {
            r_defined_locally = !info[static_cast<size_t>(rev)].is_call_or_use;
            break;
          }
        }
        (void)rm;
        (void)ni;
        return r_defined_locally;
      };
      if (extra_dead_reg >= 0 &&
          check_lower_order_dead_reg(extra_dead_reg, dst_mode, notes.size())) {
        return ConflictSite{.insn_idx = idx, .regno = extra_dead_reg, .note_idx = notes.size()};
      }
      for (size_t ni = 0; ni < notes.size(); ++ni) {
        if (notes[ni].kind == RtlMachineMode::kRegDead && notes[ni].datum.op == RtlOp::kReg &&
            !notes[ni].datum.IsArgPointerReg() &&
            check_lower_order_dead_reg(notes[ni].datum.RegNo(), notes[ni].datum.mode, ni)) {
          return ConflictSite{.insn_idx = idx, .regno = notes[ni].datum.RegNo(), .note_idx = ni};
        }
      }
    }
    return std::nullopt;
  };

  bool any_committed = false;
  std::vector<int> cur_cand = init_cand_in_base;
  constexpr int kMaxRepairs = 4;

  for (int rep = 0; rep < kMaxRepairs; ++rep) {
    std::vector<InsnRegReadDefDeaths> info(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      info[static_cast<size_t>(i)] =
          CollectInsnRegReadDefDeaths((*base_block)[static_cast<size_t>(i)], mm);
    }

    struct RepairMove {
      int from_idx = -1;
      int to_idx = -1;
      int regno = -1;
      size_t note_idx = 0;
    };
    std::vector<RepairMove> repairs;
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      const std::optional<ConflictSite> conf =
          find_conflict_at(*base_block, info, i, /*extra_dead_reg=*/-1);
      if (!conf.has_value() ||
          conf->note_idx >= (*base_block)[static_cast<size_t>(i)].pn.reg_notes.size()) {
        continue;
      }
      const RtlRegNote& dead_note =
          (*base_block)[static_cast<size_t>(i)].pn.reg_notes[conf->note_idx];
      const int r = conf->regno;
      const int nr = mm.HardRegNRegs(r, dead_note.datum.mode);
      for (int j = i - 1; j >= prologue_end; --j) {
        bool j_defs_r = false;
        for (int k = 0; k < nr; ++k) {
          if (info[static_cast<size_t>(j)].defs.contains(r + k)) {
            j_defs_r = true;
            break;
          }
        }
        if (j_defs_r) {
          break;
        }
        if (info[static_cast<size_t>(j)].is_call_or_use) {
          continue;
        }
        bool j_reads_all = true;
        bool j_already_dead = false;
        for (int k = 0; k < nr; ++k) {
          if (!info[static_cast<size_t>(j)].reads.contains(r + k)) {
            j_reads_all = false;
          }
          if (info[static_cast<size_t>(j)].deaths.contains(r + k)) {
            j_already_dead = true;
          }
        }
        if (!j_reads_all || j_already_dead) {
          continue;
        }
        if (!find_conflict_at(*base_block, info, j, /*extra_dead_reg=*/r).has_value()) {
          repairs.push_back(RepairMove{
              .from_idx = i,
              .to_idx = j,
              .regno = r,
              .note_idx = conf->note_idx,
          });
        }
      }
    }
    if (repairs.empty()) {
      break;
    }

    bool repaired_one = false;
    for (const RepairMove& rm : repairs) {
      std::vector<Insn> trial_base = *base_block;
      auto& from_notes = trial_base[static_cast<size_t>(rm.from_idx)].pn.reg_notes;
      const RtlRegNote moved_note = from_notes[rm.note_idx];
      from_notes.erase(from_notes.begin() + static_cast<std::ptrdiff_t>(rm.note_idx));
      trial_base[static_cast<size_t>(rm.to_idx)].pn.reg_notes.push_back(moved_note);

      // Build candidate preference orders placing `rm.to_idx` after `rm.from_idx`
      // and all other same-live-range pure readers of `rm.regno`:
      //   1. `defer_pref`: defer `rm.to_idx` immediately after the last trailing
      //      pure reader (inverts `sched2` hoisting `rm.to_idx` early).
      //   2. `pull_pref`: pull trailing pure readers immediately before `rm.to_idx`
      //      (when `rm.to_idx` has a consumer between `rm.to_idx` and `rm.from_idx`).
      const int moved_nr = mm.HardRegNRegs(rm.regno, moved_note.datum.mode);
      absl::btree_set<int> readers_to_pull;
      readers_to_pull.insert(rm.from_idx);
      for (int m = rm.to_idx + 1; m < epilogue_begin; ++m) {
        bool m_defs_r = false;
        bool m_reads_r = false;
        for (int w = 0; w < moved_nr; ++w) {
          if (info[static_cast<size_t>(m)].defs.contains(rm.regno + w)) {
            m_defs_r = true;
          }
          if (info[static_cast<size_t>(m)].reads.contains(rm.regno + w)) {
            m_reads_r = true;
          }
        }
        if (m_defs_r) {
          break;
        }
        if (m_reads_r && !info[static_cast<size_t>(m)].is_call_or_use) {
          readers_to_pull.insert(m);
        }
      }
      std::vector<int> pos_in_cur(static_cast<size_t>(n), -1);
      for (int p = 0; p < n; ++p) {
        pos_in_cur[static_cast<size_t>(cur_cand[static_cast<size_t>(p)])] = p;
      }
      std::vector<int> trailing_readers;
      for (int p = pos_in_cur[static_cast<size_t>(rm.to_idx)] + 1; p < n; ++p) {
        const int id = cur_cand[static_cast<size_t>(p)];
        if (readers_to_pull.contains(id)) {
          trailing_readers.push_back(id);
        }
      }
      std::vector<std::vector<int>> pref_orders;
      if (!trailing_readers.empty()) {
        const int last_tr_id = trailing_readers.back();
        std::vector<int> defer_pref;
        defer_pref.reserve(static_cast<size_t>(n));
        for (int p = 0; p < n; ++p) {
          const int id = cur_cand[static_cast<size_t>(p)];
          if (id == rm.to_idx) {
            continue;
          }
          defer_pref.push_back(id);
          if (id == last_tr_id) {
            defer_pref.push_back(rm.to_idx);
          }
        }
        pref_orders.push_back(std::move(defer_pref));

        const absl::btree_set<int> trailing_set(trailing_readers.begin(), trailing_readers.end());
        std::vector<int> pull_pref;
        pull_pref.reserve(static_cast<size_t>(n));
        for (int p = 0; p < n; ++p) {
          const int id = cur_cand[static_cast<size_t>(p)];
          if (trailing_set.contains(id)) {
            continue;
          }
          if (id == rm.to_idx) {
            for (int tr_id : trailing_readers) {
              pull_pref.push_back(tr_id);
            }
          }
          pull_pref.push_back(id);
        }
        pref_orders.push_back(std::move(pull_pref));
      } else {
        pref_orders.push_back(cur_cand);
      }

      auto verify_and_accept = [&](const std::vector<int>& cand_perm) -> bool {
        if (static_cast<int>(cand_perm.size()) != n) {
          return false;
        }
        std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
        for (int p = 0; p < n; ++p) {
          pos_in_cand[static_cast<size_t>(cand_perm[static_cast<size_t>(p)])] = p;
        }
        if (pos_in_cand[static_cast<size_t>(rm.from_idx)] >=
            pos_in_cand[static_cast<size_t>(rm.to_idx)]) {
          return false;
        }
        std::vector<Insn> cand_trial_base = trial_base;
        absl::btree_set<int> stripped_equiv_targets;
        const std::vector<std::pair<int, RtlRegNote>> dead_targets = {{rm.to_idx, moved_note}};
        StripInvalidatedMemRegEquivAfterDeadTransfers(dead_targets, prologue_end, cand_perm,
                                                      pos_in_cand, info, mm, &cand_trial_base,
                                                      &stripped_equiv_targets);
        StripSched2SpuriousLoadMemRegEquivNotes(prologue_end, epilogue_begin, cand_perm,
                                                pos_in_cand, facts_s2, mm, cfg_ctx.stack_layout,
                                                proj_ctx.excluded_prologue_body_indices,
                                                &cand_trial_base, &stripped_equiv_targets);
        const bool cand_allow_store_load_inval =
            allow_moved_store_load_equiv_invalidation || !stripped_equiv_targets.empty();
        std::vector<Insn> trial_cand;
        trial_cand.reserve(static_cast<size_t>(n));
        for (int p = 0; p < n; ++p) {
          Insn insn = cand_trial_base[static_cast<size_t>(cand_perm[static_cast<size_t>(p)])];
          insn.luid = p;
          trial_cand.push_back(std::move(insn));
        }
        // Discover `REG_EQUIV (mem ...)` notes valid in `trial_cand` (`cv == 1`) that
        // became obstructed in `cand_trial_base` (`bv == 0`) once `REG_DEAD` moved to
        // the hoisted `rm.to_idx` (`local-alloc.c:540-665, 990-1000, 1049-1055`).
        const std::vector<Stage0MemEquivAddition> equiv_additions =
            CollectSched2ObstructedMemRegEquivAdditions(
                cand_trial_base, trial_cand, cand_perm, prologue_end, epilogue_begin,
                live_at_end_s2, facts_s2, mm, cfg_ctx.stack_layout,
                proj_ctx.excluded_prologue_body_indices);
        absl::btree_set<int> added_equiv_targets;
        for (const Stage0MemEquivAddition& add : equiv_additions) {
          added_equiv_targets.insert(add.target_sched_idx);
          auto& bnotes = cand_trial_base[static_cast<size_t>(add.target_sched_idx)].pn.reg_notes;
          bnotes.insert(bnotes.begin(), add.note);
          auto& cnotes = trial_cand[static_cast<size_t>(
                                        pos_in_cand[static_cast<size_t>(add.target_sched_idx)])]
                             .pn.reg_notes;
          cnotes.insert(cnotes.begin(), add.note);
        }
        // Gate (a): exact forward `kSched2` replay reproduces `cand_trial_base`.
        const DepGraph g_s2 = Analyze(trial_cand, facts_s2, mm, Phase::kSched2);
        const ScheduleResult s2_res =
            Schedule(trial_cand, g_s2, facts_s2, mm, Phase::kSched2, live_at_end_s2);
        if (static_cast<int>(s2_res.order.size()) != n) {
          return false;
        }
        for (int p = 0; p < n; ++p) {
          if (cand_perm[static_cast<size_t>(s2_res.order[static_cast<size_t>(p)])] != p) {
            return false;
          }
        }
        // Gate (b): strict post-reload safety (`allow_transfer=false`).
        if (!PreservesPostReloadOrderSafety(
                cand_trial_base, cand_perm, prologue_end, epilogue_begin, live_at_end_s2,
                cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
                proj_ctx.excluded_prologue_body_indices, cand_allow_store_load_inval)) {
          return false;
        }
        // Gate (c): `IsAchievable(kSched1)` on `trial_cand`'s body.
        std::vector<Insn> trial_body;
        trial_body.reserve(static_cast<size_t>(body_len));
        for (int p = prologue_end; p < epilogue_begin; ++p) {
          Insn insn = trial_cand[static_cast<size_t>(p)];
          insn.luid = p - prologue_end;
          trial_body.push_back(std::move(insn));
        }
        Sched1ProjectionContext verify_ctx = proj_ctx;
        verify_ctx.allow_post_call_combine_regs_split = false;
        verify_ctx.allow_general_combine_regs_split = false;
        verify_ctx.excluded_prologue_body_indices =
            FindInterleavedPrologueSaveBodyIndices(trial_cand, bounds, block_idx, mm);
        const Sched1ProjectionView verify_view =
            ProjectPostReloadBlockToSched1View(trial_body, mm, live_at_end_s2, verify_ctx);
        if (verify_view.core_insns.size() <= 1 ||
            !IsAchievable(verify_view.core_insns, verify_view.facts_s1, mm, Phase::kSched1,
                          verify_view.live_at_end_s1)) {
          return false;
        }
        // Stage 0 -> Stage 3 soundness gate: when moving `(REG_DEAD r)` to an
        // earlier post-`sched2` instruction (`rm.to_idx < rm.from_idx`), Stage 0
        // leaves `cand_trial_base` in post-`sched2` order and Stage 3 must be
        // able to invert the resulting premature `(REG_DEAD r)` without leaving
        // it inverted for Stage 4/5 (`flow.c:2506`, `sched.c:3415-3611`).
        if (rm.to_idx < rm.from_idx) {
          bool stage3_seed_safe = false;
          for (const InvertedRegDeadSeedMode seed_mode :
               {InvertedRegDeadSeedMode::kHoistEarliest, InvertedRegDeadSeedMode::kPullBeforeDead,
                InvertedRegDeadSeedMode::kDeferAfterReaders}) {
            const std::optional<std::vector<int>> seed_pref = BuildInvertedRegDeadSeedOrderInBase(
                cand_trial_base, prologue_end, epilogue_begin, facts_s2, mm, seed_mode);
            if (!seed_pref.has_value()) {
              continue;
            }
            const std::optional<std::vector<int>> seed_cand =
                InvertBlockOrder(cand_trial_base, facts_s2, mm, Phase::kSched2, live_at_end_s2,
                                 *seed_pref, reload_adjacent);
            if (!seed_cand.has_value()) {
              continue;
            }
            std::vector<Insn> seed_seq;
            seed_seq.reserve(static_cast<size_t>(n));
            for (int p = 0; p < n; ++p) {
              Insn insn =
                  cand_trial_base[static_cast<size_t>((*seed_cand)[static_cast<size_t>(p)])];
              insn.luid = p;
              seed_seq.push_back(std::move(insn));
            }
            if (!BuildInvertedRegDeadSeedOrderInBase(seed_seq, prologue_end, epilogue_begin,
                                                     facts_s2, mm)
                     .has_value() &&
                PreservesPostReloadOrderSafety(
                    cand_trial_base, *seed_cand, prologue_end, epilogue_begin, live_at_end_s2,
                    cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                    /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
                    proj_ctx.excluded_prologue_body_indices, cand_allow_store_load_inval)) {
              stage3_seed_safe = true;
              break;
            }
          }
          if (!stage3_seed_safe) {
            return false;
          }
        }
        *base_block = std::move(cand_trial_base);
        cur_cand = cand_perm;
        touched_sched_idxs->insert(rm.from_idx);
        touched_sched_idxs->insert(rm.to_idx);
        for (int si : stripped_equiv_targets) {
          touched_sched_idxs->insert(si);
        }
        for (int si : added_equiv_targets) {
          touched_sched_idxs->insert(si);
        }
        return true;
      };

      Sched1ProjectionContext t_ctx = proj_ctx;
      t_ctx.allow_post_call_combine_regs_split = false;
      t_ctx.allow_general_combine_regs_split = false;
      std::optional<Sched1ProjectionView> t_view;
      auto ensure_t_view = [&]() -> const Sched1ProjectionView& {
        if (!t_view.has_value()) {
          std::vector<Insn> t_body;
          t_body.reserve(static_cast<size_t>(body_len));
          for (int p = prologue_end; p < epilogue_begin; ++p) {
            Insn insn = trial_base[static_cast<size_t>(p)];
            insn.luid = p - prologue_end;
            t_body.push_back(std::move(insn));
          }
          t_view = ProjectPostReloadBlockToSched1View(t_body, mm, live_at_end_s2, t_ctx);
        }
        return *t_view;
      };
      std::vector<std::vector<int>> direct_cands;
      for (const std::vector<int>& pref_in_base : pref_orders) {
        const std::optional<std::vector<int>> direct_cand =
            InvertBlockOrder(trial_base, facts_s2, mm, Phase::kSched2, live_at_end_s2, pref_in_base,
                             reload_adjacent);
        if (!direct_cand.has_value()) {
          continue;
        }
        if (verify_and_accept(*direct_cand)) {
          any_committed = true;
          repaired_one = true;
          break;
        }
        direct_cands.push_back(*direct_cand);
      }
      if (repaired_one) {
        break;
      }

      // If no direct candidate succeeded, fall back to bounded fixed-point
      // refinement on `trial_base`, seeded first from each `direct_cand` (which
      // already places `rm.from_idx < rm.to_idx`) and then unseeded.
      if (!direct_cands.empty()) {
        std::vector<std::vector<int>> refine_seeds = direct_cands;
        refine_seeds.emplace_back();
        for (const std::vector<int>& seed : refine_seeds) {
          const std::vector<std::vector<int>> refined = FindStage0Sched2PreimageCandidates(
              trial_base, block_idx, bounds, ensure_t_view(), t_ctx, cfg_ctx, facts_s2, mm,
              live_at_end_s2, reload_adjacent, /*allow_pure_reader_reg_dead_transfer=*/false,
              /*allow_tie_flips=*/true, seed, allow_moved_store_load_equiv_invalidation);
          for (auto it = refined.rbegin(); it != refined.rend(); ++it) {
            if (verify_and_accept(*it)) {
              any_committed = true;
              repaired_one = true;
              break;
            }
          }
          if (repaired_one) {
            break;
          }
        }
        if (repaired_one) {
          break;
        }
      }
    }
    if (!repaired_one) {
      break;
    }
  }
  return any_committed;
}

}  // namespace

// Mirrors `jump.c:434-465` (`jump_optimize` with `noop_moves = 1`): when `jump2`
// deleted a redundant pre-call `(set (reg:M arg_reg) (reg:M saved_reg))` before
// `B0`'s first `CALL_INSN` (leaving `arg_reg` in `call_fusage` and stripping
// `(REG_DEAD arg_reg)` from the earlier `(set saved_reg arg_reg)`), temporarily
// restores that synthetic copy inside `view_cfg.rpo_blocks[0]` so Stage 0's
// `AttributeSched2PureReaderRegDeadNotes` sees the complete post-`sched2` block
// (matching Stage 2's `InvertDoCrossJump`) and can attribute erased
// `local-alloc.c:1266-1300` (`combine_regs`) `REG_DEAD` notes on post-call RMWs.
void SynthesizeB0Jump2DeletedPreCallArgCopiesInDelayView(
    const CompilationProfile& profile, const MachineModel& mm, RtlControlFlowGraph* view_cfg,
    RtlControlFlowGraph* live_cfg, std::vector<Stage0BlockDelayView>* block_views) {
  if (view_cfg == nullptr || live_cfg == nullptr || block_views == nullptr ||
      view_cfg->rpo_blocks.empty() || block_views->empty() || !(*block_views)[0].eligible) {
    return;
  }
  RtlBasicBlock& vblk = view_cfg->rpo_blocks[0];
  Stage0BlockDelayView& bv = (*block_views)[0];
  const size_t n_insns = vblk.instructions.size();
  if (n_insns < 3 || vblk.rtl_insns.size() != n_insns || bv.view_to_orig.size() != n_insns) {
    return;
  }

  std::vector<RtlPatternAndNotes> parsed(n_insns);
  std::optional<size_t> first_call_vi;
  for (size_t i = 0; i < n_insns; ++i) {
    auto pn = ParsePatternAndNotes(vblk.instructions[i]);
    if (!pn.ok()) {
      return;
    }
    parsed[i] = std::move(*pn);
    const RtlExpr& pat = parsed[i].pattern;
    if (pat.op == RtlOp::kJumpInsn || pat.op == RtlOp::kReturn ||
        (pat.IsSet() && pat.SetDest().op == RtlOp::kPc)) {
      return;
    }
    bool has_call = false;
    pat.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kCall) {
        has_call = true;
      }
    });
    if (has_call) {
      first_call_vi = i;
      break;
    }
  }
  if (!first_call_vi.has_value() || *first_call_vi < 2) {
    return;
  }

  const size_t call_vi = *first_call_vi;
  const bool has_explicit_fusage = vblk.rtl_insns[call_vi].has_explicit_call_fusage;
  absl::btree_set<int> fusage_regs;
  if (has_explicit_fusage) {
    for (const RtlExpr& fu : vblk.rtl_insns[call_vi].call_fusage) {
      CollectRegReads(fu, &fusage_regs);
    }
  }

  std::vector<absl::btree_set<int>> reads(call_vi);
  std::vector<absl::btree_set<int>> defs(call_vi);
  std::optional<size_t> ra_save_vi;
  absl::btree_map<int, size_t> prologue_save_vi;
  for (size_t i = 0; i < call_vi; ++i) {
    CollectRegReads(parsed[i].pattern, &reads[i]);
    absl::btree_set<int> fdefs;
    absl::btree_set<int> pdefs;
    CollectFullRegDefs(parsed[i].pattern, &fdefs, &pdefs);
    defs[i] = fdefs;
    defs[i].insert(pdefs.begin(), pdefs.end());
    if (!ra_save_vi.has_value() && IsSpRaSavePatternForDelayView(parsed[i].pattern)) {
      ra_save_vi = i;
    }
    const RtlExpr& pat = parsed[i].pattern;
    if (pat.IsSet() && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kMem &&
        !pat.SetDest().children.empty() && pat.SetSrc().op == RtlOp::kReg) {
      const RtlExpr& addr = pat.SetDest().children[0];
      const bool sp_addr = (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
                           (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                            addr.children[0].op == RtlOp::kReg &&
                            addr.children[0].RegNo() == kMipsStackPointerRegno &&
                            addr.children[1].op == RtlOp::kConstInt);
      if (sp_addr) {
        const int sr = pat.SetSrc().RegNo();
        const int span = std::max(1, mm.HardRegNRegs(sr, pat.SetSrc().mode));
        for (int off = 0; off < span; ++off) {
          prologue_save_vi.try_emplace(sr + off, i);
        }
      }
    }
  }

  const bool fp_needed = view_cfg->sideband.frame_pointer_needed;
  auto is_callee_saved = [&](int r) {
    return (r >= 16 && r <= 23) || (r == kMipsFramePointerRegno && !fp_needed) ||
           (r >= 52 && r <= 63);
  };
  auto is_o32_arg_reg = [](int r) {
    return (r >= kMipsArgRegFirst && r <= kMipsArgRegLast) ||
           (r >= kMipsFpArgRegFirst && r <= kMipsFpArgRegFirst + 3);
  };

  // Track O32 arg regs explicitly defined before `call_vi` and O32 arg words
  // occupied by `$f12`/`$f14` (`config/mips/mips.c:function_arg`, mirroring
  // `invert_do_cross_jump.cc:1759-2089`).
  absl::btree_set<int> live_at_end_s2;
  for (int succ : vblk.successors) {
    if (const auto it = live_cfg->sideband.basic_block_live_at_start.find(succ);
        it != live_cfg->sideband.basic_block_live_at_start.end()) {
      live_at_end_s2.insert(it->second.begin(), it->second.end());
    }
  }
  absl::btree_set<int> read_after_first_call = live_at_end_s2;
  auto collect_reg_read_spans = [&](const RtlExpr& root, absl::btree_set<int>* out) {
    root.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kReg) {
        const int r = node.RegNo();
        if (r >= 0 && r < kMipsFirstPseudoRegister) {
          const int span = std::max(1, mm.HardRegNRegs(r, node.mode));
          for (int off = 0; off < span; ++off) {
            out->insert(r + off);
          }
        }
      }
    });
  };
  for (size_t i = call_vi + 1; i < n_insns; ++i) {
    auto pn = ParsePatternAndNotes(vblk.instructions[i]);
    if (!pn.ok()) {
      continue;
    }
    collect_reg_read_spans(pn->pattern, &read_after_first_call);
    if (vblk.rtl_insns[i].has_explicit_call_fusage) {
      for (const RtlExpr& fu : vblk.rtl_insns[i].call_fusage) {
        collect_reg_read_spans(fu, &read_after_first_call);
      }
    }
  }

  absl::btree_set<int> arg_regs_live_into_call;
  absl::btree_set<int> arg_word_occupied_by_fpr;
  bool arg6_written_as_8byte = false;
  for (int ar = kMipsArgRegFirst; ar <= kMipsArgRegLast; ++ar) {
    for (size_t i = call_vi; i > 0; --i) {
      if (defs[i - 1].contains(ar)) {
        if (!HasRegDeadNoteCovering(parsed[i - 1].reg_notes, ar, mm)) {
          arg_regs_live_into_call.insert(ar);
          if (ar == 6 && parsed[i - 1].pattern.IsSet() &&
              parsed[i - 1].pattern.SetDest().op == RtlOp::kReg &&
              parsed[i - 1].pattern.SetDest().RegNo() == 6 &&
              (parsed[i - 1].pattern.SetDest().mode == RtlMachineMode::kDf ||
               parsed[i - 1].pattern.SetDest().mode == RtlMachineMode::kDi)) {
            arg6_written_as_8byte = true;
          }
        }
        break;
      }
    }
  }
  for (int fr : {kMipsFpArgRegFirst, kMipsFpArgRegFirst + 2}) {
    const int base_word = (fr == kMipsFpArgRegFirst) ? 4 : 6;
    for (size_t i = call_vi; i > 0; --i) {
      if (defs[i - 1].contains(fr) || defs[i - 1].contains(fr + 1)) {
        if (!HasRegDeadNoteCovering(parsed[i - 1].reg_notes, fr, mm)) {
          arg_word_occupied_by_fpr.insert(base_word);
          if (defs[i - 1].contains(fr + 1) ||
              (parsed[i - 1].pattern.IsSet() && parsed[i - 1].pattern.SetDest().op == RtlOp::kReg &&
               parsed[i - 1].pattern.SetDest().mode == RtlMachineMode::kDf)) {
            arg_word_occupied_by_fpr.insert(base_word + 1);
          }
        }
        break;
      }
    }
  }
  if (has_explicit_fusage) {
    if (fusage_regs.contains(kMipsFpArgRegFirst)) {
      arg_word_occupied_by_fpr.insert(4);
      if (fusage_regs.contains(kMipsFpArgRegFirst + 1)) {
        arg_word_occupied_by_fpr.insert(5);
      }
    }
    if (fusage_regs.contains(kMipsFpArgRegFirst + 2)) {
      arg_word_occupied_by_fpr.insert(6);
      if (fusage_regs.contains(kMipsFpArgRegFirst + 3)) {
        arg_word_occupied_by_fpr.insert(7);
      }
    }
  }

  struct DeletedArgCopy {
    size_t copy_vi = 0;
    int saved_reg = -1;
    int arg_reg = -1;
    int saved_span = 1;
    int arg_span = 1;
    RtlMachineMode mode = RtlMachineMode::kNone;
    bool saved_read_before_call = false;
  };
  std::vector<DeletedArgCopy> raw_cands;
  absl::btree_set<int> claimed_arg_regs;
  for (size_t i = 0; i < call_vi; ++i) {
    const RtlExpr& pat = parsed[i].pattern;
    if (!pat.IsSet() || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kReg ||
        pat.SetSrc().op != RtlOp::kReg || pat.SetDest().IsArgPointerReg() ||
        pat.SetSrc().IsArgPointerReg() || pat.SetDest().mode != pat.SetSrc().mode) {
      continue;
    }
    const RtlMachineMode mode = pat.SetDest().mode;
    if (mode != RtlMachineMode::kSi && mode != RtlMachineMode::kSf && mode != RtlMachineMode::kDi &&
        mode != RtlMachineMode::kDf) {
      continue;
    }
    const int saved_reg = pat.SetDest().RegNo();
    const int arg_reg = pat.SetSrc().RegNo();
    const int saved_span = std::max(1, mm.HardRegNRegs(saved_reg, mode));
    const int arg_span = std::max(1, mm.HardRegNRegs(arg_reg, mode));
    if (!is_callee_saved(saved_reg) || (saved_span == 2 && !is_callee_saved(saved_reg + 1))) {
      continue;
    }
    const bool is_gpr_arg =
        (arg_reg >= kMipsArgRegFirst && arg_reg + arg_span - 1 <= kMipsArgRegLast);
    if (!is_gpr_arg && !(has_explicit_fusage && is_o32_arg_reg(arg_reg))) {
      continue;
    }
    if (HasRegDeadNoteCovering(parsed[i].reg_notes, arg_reg, mm)) {
      continue;
    }
    bool valid = true;
    for (int off = 0; off < arg_span; ++off) {
      const int ar = arg_reg + off;
      if (claimed_arg_regs.contains(ar) || (is_gpr_arg && arg_word_occupied_by_fpr.contains(ar)) ||
          (has_explicit_fusage && !fusage_regs.contains(ar))) {
        valid = false;
        break;
      }
      for (size_t k = 0; k < i; ++k) {
        if (defs[k].contains(ar)) {
          valid = false;
          break;
        }
      }
      for (size_t k = i + 1; k < call_vi && valid; ++k) {
        if (defs[k].contains(ar) || reads[k].contains(ar)) {
          valid = false;
        }
      }
    }
    bool saved_read_before_call = false;
    for (int off = 0; off < saved_span && valid; ++off) {
      const int sr = saved_reg + off;
      if (!read_after_first_call.contains(sr)) {
        valid = false;
        break;
      }
      for (size_t k = i + 1; k < call_vi; ++k) {
        if (defs[k].contains(sr)) {
          valid = false;
          break;
        }
        if (reads[k].contains(sr)) {
          saved_read_before_call = true;
        }
      }
    }
    if (!valid) {
      continue;
    }
    raw_cands.push_back(
        DeletedArgCopy{i, saved_reg, arg_reg, saved_span, arg_span, mode, saved_read_before_call});
    for (int off = 0; off < arg_span; ++off) {
      claimed_arg_regs.insert(arg_reg + off);
    }
  }
  if (raw_cands.empty()) {
    return;
  }

  std::vector<DeletedArgCopy> cands;
  for (const DeletedArgCopy& c : raw_cands) {
    bool all_lower_args_supplied = true;
    if (c.arg_reg >= kMipsArgRegFirst && c.arg_reg <= kMipsArgRegLast) {
      for (int r = kMipsArgRegFirst; r < c.arg_reg; ++r) {
        if (c.arg_span == 2 && c.arg_reg == 6 && r == 5) {
          continue;
        }
        if (!arg_regs_live_into_call.contains(r) && !claimed_arg_regs.contains(r) &&
            !arg_word_occupied_by_fpr.contains(r)) {
          all_lower_args_supplied = false;
          break;
        }
      }
    }
    if (!all_lower_args_supplied) {
      continue;
    }
    if (!has_explicit_fusage) {
      bool save_before_ra = false;
      if (ra_save_vi.has_value() && !c.saved_read_before_call) {
        for (int off = 0; off < c.saved_span; ++off) {
          if (const auto pit = prologue_save_vi.find(c.saved_reg + off);
              pit != prologue_save_vi.end() && pit->second < *ra_save_vi &&
              pit->second < c.copy_vi) {
            save_before_ra = true;
            break;
          }
        }
      }
      bool higher_arg_written = false;
      for (int ar = c.arg_reg + c.arg_span; ar <= kMipsArgRegLast; ++ar) {
        if (c.arg_reg == 5 && c.arg_span == 1 && arg6_written_as_8byte) {
          break;
        }
        if (arg_regs_live_into_call.contains(ar)) {
          higher_arg_written = true;
          break;
        }
      }
      if (!save_before_ra && !higher_arg_written) {
        continue;
      }
    }
    cands.push_back(c);
  }
  if (cands.empty()) {
    return;
  }

  // Build a trial copy of `vblk` and `bv` with the synthetic copies inserted,
  // verifying that `InvertBlockOrder(Phase::kSched2)` succeeds before committing.

  auto make_reg_expr = [](RtlMachineMode m, int regno) { return RtlExpr::MakeReg(m, regno); };

  auto is_prologue_save = [](const RtlExpr& pat) {
    if (!pat.IsSet() || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kMem ||
        pat.SetSrc().op != RtlOp::kReg || pat.SetDest().children.empty()) {
      return false;
    }
    const int sr = pat.SetSrc().RegNo();
    const bool saved = (sr == kMipsReturnAddrRegno) || (sr >= 16 && sr <= 23) ||
                       (sr == kMipsFramePointerRegno) || (sr >= 52 && sr <= 63);
    if (!saved) {
      return false;
    }
    const RtlExpr& addr = pat.SetDest().children[0];
    return (addr.op == RtlOp::kReg && addr.RegNo() == kMipsStackPointerRegno) ||
           (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
            addr.children[0].op == RtlOp::kReg &&
            addr.children[0].RegNo() == kMipsStackPointerRegno &&
            addr.children[1].op == RtlOp::kConstInt);
  };

  auto build_trial = [&](const std::vector<size_t>& insert_positions, RtlBasicBlock* out_blk,
                         Stage0BlockDelayView* out_bv) {
    *out_blk = vblk;
    *out_bv = bv;
    std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage(out_blk->instructions.size());
    for (size_t i = 0; i < out_blk->rtl_insns.size(); ++i) {
      if (out_blk->rtl_insns[i].has_explicit_call_fusage) {
        saved_fusage[i] = out_blk->rtl_insns[i].call_fusage;
      }
    }
    // Attach temporary `(REG_DEAD arg_reg)` on each candidate's entry copy in `out_blk`.
    for (const DeletedArgCopy& c : cands) {
      RtlPatternAndNotes cpn = parsed[c.copy_vi];
      if (!HasRegDeadNoteCovering(cpn.reg_notes, c.arg_reg, mm)) {
        cpn.reg_notes.insert(cpn.reg_notes.begin(), RtlRegNote{
                                                        .kind = RtlMachineMode::kRegDead,
                                                        .list_op = RtlOp::kExprList,
                                                        .datum = make_reg_expr(c.mode, c.arg_reg),
                                                    });
        out_blk->instructions[c.copy_vi] = FormatGccPrintRtlPatternAndNotes(
            cpn.pattern, cpn.reg_notes, cpn.bracketed_notes, /*call_fusage=*/{});
        out_bv->view_to_orig[c.copy_vi].synthetic_added_dead_regs.insert(c.arg_reg);
      }
    }
    // Insert synthetic `(set arg_reg saved_reg)` moves in descending position order
    // so earlier indices stay valid.
    std::vector<size_t> order(cands.size());
    for (size_t idx = 0; idx < cands.size(); ++idx) {
      order[idx] = idx;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      if (insert_positions[a] != insert_positions[b]) {
        return insert_positions[a] > insert_positions[b];
      }
      return a > b;
    });
    for (size_t idx : order) {
      const DeletedArgCopy& c = cands[idx];
      const size_t pos = insert_positions[idx];
      const RtlExpr mv_pat =
          RtlExpr::MakeSet(make_reg_expr(c.mode, c.arg_reg), make_reg_expr(c.mode, c.saved_reg));
      const std::string mv_str = FormatGccPrintRtlPatternAndNotes(mv_pat, {}, {}, {});
      out_blk->instructions.insert(out_blk->instructions.begin() + static_cast<ptrdiff_t>(pos),
                                   mv_str);
      out_bv->view_to_orig.insert(out_bv->view_to_orig.begin() + static_cast<ptrdiff_t>(pos),
                                  Stage0OriginLoc{.is_synthetic = true});
      saved_fusage.insert(saved_fusage.begin() + static_cast<ptrdiff_t>(pos), std::nullopt);
      if (out_bv->unpacked_non_return_jump_slot_view_idx.has_value() &&
          *out_bv->unpacked_non_return_jump_slot_view_idx >= pos) {
        ++(*out_bv->unpacked_non_return_jump_slot_view_idx);
      }
      absl::btree_set<size_t> shifted_align;
      for (size_t ai : out_bv->alignment_touched_view_idxs) {
        shifted_align.insert(ai >= pos ? ai + 1 : ai);
      }
      out_bv->alignment_touched_view_idxs = std::move(shifted_align);
    }
    out_blk->SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < out_blk->rtl_insns.size() && i < saved_fusage.size(); ++i) {
      if (saved_fusage[i].has_value()) {
        out_blk->rtl_insns[i].has_explicit_call_fusage = true;
        out_blk->rtl_insns[i].call_fusage = *saved_fusage[i];
      }
    }
    const size_t new_call_vi = call_vi + cands.size();
    if (!has_explicit_fusage && new_call_vi < out_blk->rtl_insns.size()) {
      absl::btree_set<int> synth_fusage_regs = arg_regs_live_into_call;
      for (const DeletedArgCopy& c : cands) {
        const int span = std::max(1, mm.HardRegNRegs(c.arg_reg, c.mode));
        for (int off = 0; off < span; ++off) {
          synth_fusage_regs.insert(c.arg_reg + off);
        }
      }
      out_blk->rtl_insns[new_call_vi].has_explicit_call_fusage = true;
      out_blk->rtl_insns[new_call_vi].call_fusage.clear();
      for (auto rit = synth_fusage_regs.rbegin(); rit != synth_fusage_regs.rend(); ++rit) {
        out_blk->rtl_insns[new_call_vi].call_fusage.push_back(RtlExpr::MakeUnary(
            RtlOp::kUse, RtlMachineMode::kNone, make_reg_expr(RtlMachineMode::kSi, *rit)));
      }
    }
  };

  auto trial_inverts_s2 = [&](const RtlBasicBlock& cand_blk) -> bool {
    RtlControlFlowGraph tmp_cfg = *view_cfg;
    tmp_cfg.rpo_blocks[0] = cand_blk;
    const ParsedSchedRegion reg = ParseBlockForSchedModel(cand_blk, 0, Phase::kSched2);
    if (!reg.valid || reg.has_mid_block_loop_barrier) {
      return false;
    }
    const RegFacts tmp_facts = RegFacts::FromCfg(tmp_cfg, profile, Phase::kSched2);
    return InvertBlockOrder(reg.sched_block, tmp_facts, mm, Phase::kSched2, live_at_end_s2)
        .has_value();
  };

  // Compute default insertion slots matching `invert_do_cross_jump.cc:2124-2220`.
  std::vector<size_t> default_pos(cands.size(), call_vi);
  for (size_t idx = 0; idx < cands.size(); ++idx) {
    const DeletedArgCopy& c = cands[idx];
    if (ra_save_vi.has_value() && *ra_save_vi > c.copy_vi) {
      if (c.saved_reg >= kMipsFpRegFirst && *ra_save_vi <= c.copy_vi + 2) {
        size_t pos = *ra_save_vi + 1;
        while (pos < call_vi && is_prologue_save(parsed[pos].pattern)) {
          ++pos;
        }
        default_pos[idx] = pos;
      } else {
        size_t pos = *ra_save_vi;
        while (pos > c.copy_vi + 1 && c.saved_reg >= kMipsFpRegFirst &&
               parsed[pos - 1].pattern.IsSet() &&
               parsed[pos - 1].pattern.SetDest().op == RtlOp::kReg &&
               is_o32_arg_reg(parsed[pos - 1].pattern.SetDest().RegNo())) {
          --pos;
        }
        default_pos[idx] = pos;
      }
    }
    // Never place a restored single-cycle GPR->GPR arg copy ahead of an unpacked
    // delay-slot or post-`$ra`-save setup of a lower O32 arg register
    // (`calls.c:expand_call`); multi-cycle FPR->GPR (`xfer` latency 2) copies
    // outrank single-cycle GPR arg setups (`sched.c:priority`, matching
    // `invert_do_cross_jump.cc:2293-2305`).
    if (c.saved_reg < kMipsFpRegFirst && c.arg_reg >= kMipsArgRegFirst &&
        c.arg_reg <= kMipsArgRegLast) {
      for (size_t k = default_pos[idx]; k < call_vi; ++k) {
        bool defines_lower_arg = false;
        for (int ar = kMipsArgRegFirst; ar < c.arg_reg; ++ar) {
          if (defs[k].contains(ar)) {
            defines_lower_arg = true;
            break;
          }
        }
        if (defines_lower_arg) {
          default_pos[idx] = k + 1;
        }
      }
    }
  }

  RtlBasicBlock chosen_blk;
  Stage0BlockDelayView chosen_bv;
  build_trial(default_pos, &chosen_blk, &chosen_bv);
  if (!trial_inverts_s2(chosen_blk)) {
    bool found_inv = false;
    if (cands.size() == 1) {
      for (size_t pos = call_vi; pos > cands[0].copy_vi && !found_inv; --pos) {
        if (pos == default_pos[0]) {
          continue;
        }
        RtlBasicBlock probe_blk;
        Stage0BlockDelayView probe_bv;
        build_trial({pos}, &probe_blk, &probe_bv);
        if (trial_inverts_s2(probe_blk)) {
          chosen_blk = std::move(probe_blk);
          chosen_bv = std::move(probe_bv);
          found_inv = true;
        }
      }
    } else {
      for (size_t idx = 0; idx < cands.size() && !found_inv; ++idx) {
        for (size_t pos = call_vi; pos > cands[idx].copy_vi && !found_inv; --pos) {
          if (pos == default_pos[idx]) {
            continue;
          }
          std::vector<size_t> probe = default_pos;
          probe[idx] = pos;
          RtlBasicBlock probe_blk;
          Stage0BlockDelayView probe_bv;
          build_trial(probe, &probe_blk, &probe_bv);
          if (trial_inverts_s2(probe_blk)) {
            chosen_blk = std::move(probe_blk);
            chosen_bv = std::move(probe_bv);
            found_inv = true;
          }
        }
      }
    }
    // Keep `default_pos` even if `trial_inverts_s2` hasn't succeeded yet when
    // an unpacked call delay slot still needs `AlignStage0DelaySlotsToSched2View`.
  }
  vblk = std::move(chosen_blk);
  bv = std::move(chosen_bv);
  *live_cfg = *view_cfg;
  ReconstructRtlLiveness(
      live_cfg, profile,
      {.reload_completed = true, .update_reg_notes = true, .preserve_phantom_pseudos = false});
}

// Mirrors Stage 1 -> Stage 2 order (`reorg` delay-slot alignment before
// `jump2`-deleted pre-call arg-copy restoration) in Stage 0's delay view,
// re-aligning if needed and rolling back `B0` if the synthetic copies fail
// `kSched2` inversion.
void ApplyStage0B0Jump2DeletedArgCopiesWithRollback(
    const CompilationProfile& profile, const MachineModel& mm, RtlControlFlowGraph* view_cfg,
    RtlControlFlowGraph* live_cfg, std::vector<Stage0BlockDelayView>* block_views) {
  if (view_cfg == nullptr || live_cfg == nullptr || block_views == nullptr ||
      view_cfg->rpo_blocks.empty()) {
    return;
  }
  const size_t b0_pre_synth_len = view_cfg->rpo_blocks[0].instructions.size();
  const RtlControlFlowGraph pre_synth_view_cfg = *view_cfg;
  const RtlControlFlowGraph pre_synth_live_cfg = *live_cfg;
  const std::vector<Stage0BlockDelayView> pre_synth_block_views = *block_views;
  SynthesizeB0Jump2DeletedPreCallArgCopiesInDelayView(profile, mm, view_cfg, live_cfg, block_views);
  if (view_cfg->rpo_blocks[0].instructions.size() == b0_pre_synth_len) {
    return;
  }
  auto b0_inverts_s2 = [&]() -> bool {
    const ParsedSchedRegion b0_reg =
        ParseBlockForSchedModel(view_cfg->rpo_blocks[0], 0, Phase::kSched2);
    if (!b0_reg.valid || b0_reg.has_mid_block_loop_barrier) {
      return false;
    }
    const RegFacts b0_facts_s2 = RegFacts::FromCfg(*view_cfg, profile, Phase::kSched2);
    absl::btree_set<int> b0_live_end;
    for (int succ : view_cfg->rpo_blocks[0].successors) {
      if (const auto it = live_cfg->sideband.basic_block_live_at_start.find(succ);
          it != live_cfg->sideband.basic_block_live_at_start.end()) {
        b0_live_end.insert(it->second.begin(), it->second.end());
      }
    }
    return InvertBlockOrder(b0_reg.sched_block, b0_facts_s2, mm, Phase::kSched2, b0_live_end)
        .has_value();
  };
  if (b0_inverts_s2()) {
    return;
  }
  AlignStage0DelaySlotsToSched2View(profile, mm, view_cfg, live_cfg, block_views);
  if (!b0_inverts_s2()) {
    *view_cfg = pre_synth_view_cfg;
    *live_cfg = pre_synth_live_cfg;
    *block_views = pre_synth_block_views;
  }
}

// `reload1.c:5599-5665` (`emit_reload_insns`) + `jump.c:452-459` (`jump_optimize`):
// when `sched2` separates an earlyclobber `DImode` GPR `PARALLEL` `prod` from
// its output-reload move `mv: (set (reg:DI orig) (reg:DI reload))` with an
// intervening pure reader of `orig`, `AttributeSched2PureReaderRegDeadNotes`
// validates the `.10.sched2` preimage with `(REG_DEAD orig)` on `prod`; before
// committing back to `.12.dbr` (post-`jump2`), strip that `(REG_DEAD orig)` on
// `prod` because `jump2` unconditionally consumes it scanning backward from `mv`
// (and Stage 2 `RestoreJump2ConsumedRegDeadNotes` restores it onto `prod`).
void StripJump2ConsumedEarlyclobberOutputReloadDeaths(int prologue_end, int epilogue_begin,
                                                      const MachineModel& mm,
                                                      std::vector<Insn>* transferred_base) {
  if (transferred_base == nullptr) {
    return;
  }
  for (int mv_idx = prologue_end + 1; mv_idx < epilogue_begin; ++mv_idx) {
    const RtlExpr& mv_pat = (*transferred_base)[static_cast<size_t>(mv_idx)].pn.pattern;
    if ((*transferred_base)[static_cast<size_t>(mv_idx)].kind != Insn::Kind::kInsn ||
        mv_pat.op != RtlOp::kSet || mv_pat.children.size() != 2 ||
        mv_pat.SetDest().op != RtlOp::kReg || mv_pat.SetSrc().op != RtlOp::kReg ||
        mv_pat.SetDest().mode != RtlMachineMode::kDi ||
        mv_pat.SetSrc().mode != RtlMachineMode::kDi) {
      continue;
    }
    const int orig_hr = mv_pat.SetDest().RegNo();
    const int reload_hr = mv_pat.SetSrc().RegNo();
    if (orig_hr < 2 || orig_hr >= kMipsFpRegFirst - 1 || reload_hr < 2 ||
        reload_hr >= kMipsFpRegFirst - 1 || orig_hr == reload_hr || (orig_hr % 2) != 0 ||
        (reload_hr % 2) != 0 || mm.HardRegNRegs(orig_hr, RtlMachineMode::kDi) != 2) {
      continue;
    }
    bool saw_intervening_orig_read = false;
    for (int p = mv_idx - 1; p >= prologue_end; --p) {
      const Insn& pi = (*transferred_base)[static_cast<size_t>(p)];
      if (pi.kind == Insn::Kind::kCall || pi.kind == Insn::Kind::kJump) {
        break;
      }
      int matched_orig = -1;
      int matched_reload = -1;
      if (saw_intervening_orig_read &&
          TryMatchEarlyclobberDiOutputReload(pi, (*transferred_base)[static_cast<size_t>(mv_idx)],
                                             mm, &matched_orig, &matched_reload) &&
          matched_orig == orig_hr && matched_reload == reload_hr) {
        auto& notes = (*transferred_base)[static_cast<size_t>(p)].pn.reg_notes;
        auto it = std::find_if(notes.begin(), notes.end(), [&](const RtlRegNote& n) {
          return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
                 n.datum.RegNo() == orig_hr;
        });
        if (it != notes.end()) {
          notes.erase(it);
        }
        break;
      }
      if (HasRegDeadNoteCovering(pi.pn.reg_notes, orig_hr, mm)) {
        break;
      }
      const InsnRegReadDefDeaths info = CollectInsnRegReadDefDeaths(pi, mm);
      if (info.defs.contains(orig_hr) || info.defs.contains(orig_hr + 1) ||
          info.defs.contains(reload_hr) || info.defs.contains(reload_hr + 1)) {
        break;
      }
      if (info.reads.contains(orig_hr) || info.reads.contains(orig_hr + 1)) {
        saw_intervening_orig_read = true;
      }
    }
  }
}

void AttributeSched2PureReaderRegDeadNotes(RtlControlFlowGraph* cfg,
                                           const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);
  if (flags.opt_level <= 0 || !flags.RunsSched1() || !flags.RunsSched2()) {
    return;
  }
  const MachineModel mm = MachineModel::FromProfile(profile);
  const size_t num_blocks = cfg->rpo_blocks.size();

  RtlControlFlowGraph view_cfg;
  RtlControlFlowGraph live_cfg;
  std::vector<Stage0BlockDelayView> block_views;
  BuildStage0UnpackedDelayViewCfg(*cfg, profile, &view_cfg, &live_cfg, &block_views);
  AlignStage0DelaySlotsToSched2View(profile, mm, &view_cfg, &live_cfg, &block_views);
  ApplyStage0B0Jump2DeletedArgCopiesWithRollback(profile, mm, &view_cfg, &live_cfg, &block_views);
  const RegFacts facts_s2 = RegFacts::FromCfg(view_cfg, profile, Phase::kSched2);
  const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(view_cfg, live_cfg, mm);

  for (size_t b = 0; b < num_blocks; ++b) {
    if (!block_views[b].eligible) {
      continue;
    }
    const RtlBasicBlock& vblk = view_cfg.rpo_blocks[b];
    const ParsedSchedRegion base_region = ParseBlockForSchedModel(vblk, b, Phase::kSched2);
    if (!base_region.valid || base_region.has_mid_block_loop_barrier) {
      continue;
    }
    const int n = static_cast<int>(base_region.sched_block.size());
    if (n <= 2) {
      continue;
    }
    absl::btree_set<int> live_at_end_s2;
    for (int succ : vblk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(base_region.sched_block, b);
    const int prologue_end = bounds.prologue_end;
    const int epilogue_begin = bounds.epilogue_begin;
    const int body_len = epilogue_begin - prologue_end;
    if (body_len <= 1) {
      continue;
    }
    std::vector<Insn> base_body;
    base_body.reserve(static_cast<size_t>(body_len));
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      Insn insn = base_region.sched_block[static_cast<size_t>(i)];
      insn.luid = i - prologue_end;
      base_body.push_back(std::move(insn));
    }
    Sched1ProjectionContext proj_ctx = BuildSched1ProjectionContextForBlock(b, cfg_ctx);
    proj_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(base_region.sched_block, bounds, b, mm);

    absl::btree_set<int> alignment_touched_sched_idxs;
    if (!block_views[b].alignment_touched_view_idxs.empty()) {
      for (int si = 0; si < n; ++si) {
        if (block_views[b].alignment_touched_view_idxs.contains(
                base_region.sched_orig_indices[static_cast<size_t>(si)])) {
          alignment_touched_sched_idxs.insert(si);
        }
      }
    }

    Sched1ProjectionContext unsplit_base_ctx = proj_ctx;
    unsplit_base_ctx.allow_post_call_combine_regs_split = false;
    unsplit_base_ctx.allow_general_combine_regs_split = false;
    const Sched1ProjectionView unsplit_base_view =
        ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, unsplit_base_ctx);
    if (unsplit_base_view.core_insns.size() <= 1 ||
        IsAchievable(unsplit_base_view.core_insns, unsplit_base_view.facts_s1, mm, Phase::kSched1,
                     unsplit_base_view.live_at_end_s1)) {
      std::vector<Insn> repaired_base = base_region.sched_block;
      absl::btree_set<int> early_touched = alignment_touched_sched_idxs;
      if (unsplit_base_view.core_insns.size() > 1) {
        std::vector<int> id_cand(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
          id_cand[static_cast<size_t>(i)] = i;
        }
        std::vector<AdjacentPair> early_reload_adj;
        for (int i = 0; i + 1 < n; ++i) {
          if (IsAdjacentMdOutputReloadPair(base_region.sched_block[static_cast<size_t>(i)],
                                           base_region.sched_block[static_cast<size_t>(i + 1)]) ||
              IsAdjacentReloadContiguityPair(base_region.sched_block[static_cast<size_t>(i)],
                                             base_region.sched_block[static_cast<size_t>(i + 1)],
                                             mm, cfg_ctx.stack_layout)) {
            early_reload_adj.push_back(AdjacentPair{.first = i, .second = i + 1});
          }
        }
        TryRepairCombineRegsPureReaderRegDeadConflicts(
            b, bounds, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2, early_reload_adj, id_cand,
            /*allow_moved_store_load_equiv_invalidation=*/false, &repaired_base, &early_touched);
      }
      if (!early_touched.empty() && unsplit_base_view.core_insns.size() > 1) {
        CommitStage0AttributedNotesToBlock(base_region, repaired_base, early_touched,
                                           &block_views[b], &cfg->rpo_blocks[b],
                                           &cfg->sideband.epilogue_delay_pattern);
      }
      continue;
    }

    const Sched1ProjectionView base_view =
        ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, proj_ctx);
    std::vector<AdjacentPair> reload_adjacent;
    for (int i = 0; i + 1 < n; ++i) {
      if (IsAdjacentMdOutputReloadPair(base_region.sched_block[static_cast<size_t>(i)],
                                       base_region.sched_block[static_cast<size_t>(i + 1)]) ||
          IsAdjacentReloadContiguityPair(base_region.sched_block[static_cast<size_t>(i)],
                                         base_region.sched_block[static_cast<size_t>(i + 1)], mm,
                                         cfg_ctx.stack_layout)) {
        reload_adjacent.push_back(AdjacentPair{.first = i, .second = i + 1});
      }
    }
    const std::vector<std::vector<int>> base_preimages =
        FindStage0Sched2PreimageCandidates(base_region.sched_block, b, bounds, base_view, proj_ctx,
                                           cfg_ctx, facts_s2, mm, live_at_end_s2, reload_adjacent);

    std::vector<InsnRegReadDefDeaths> insn_info(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      insn_info[static_cast<size_t>(i)] =
          CollectInsnRegReadDefDeaths(base_region.sched_block[static_cast<size_t>(i)], mm);
    }
    std::vector<absl::btree_set<int>> live_after_base(static_cast<size_t>(n));
    {
      absl::btree_set<int> cur_live = live_at_end_s2;
      for (int i = n - 1; i >= 0; --i) {
        live_after_base[static_cast<size_t>(i)] = cur_live;
        for (int d : insn_info[static_cast<size_t>(i)].defs) {
          cur_live.erase(d);
        }
        for (int r : insn_info[static_cast<size_t>(i)].reads) {
          cur_live.insert(r);
        }
      }
    }

    struct NoteTransfer {
      int from_idx = -1;
      size_t note_idx = 0;
      int to_idx = -1;
      RtlRegNote note;
    };

    // Pass 1 tries pure `kSched2` preimages WITHOUT synthesizing `combine_regs`
    // RMW splits so coalesced multi-step RMW chains that already have a pure
    // `sched2` preimage are never split unnecessarily.
    enum class PreimagePassResult { kNotSolved, kSolvedWithoutMutation, kCommitted };
    auto run_preimage_pass = [&](absl::Span<const std::vector<int>> cands,
                                 bool allow_post_call_rmw_splits,
                                 bool allow_general_rmw_splits) -> PreimagePassResult {
      bool saw_zero_mutation_solution = false;
      absl::btree_set<std::vector<int>> tried_cands;
      for (auto cand_it = cands.rbegin(); cand_it != cands.rend(); ++cand_it) {
        const std::vector<int>& cand_in_base = *cand_it;
        if (!tried_cands.insert(cand_in_base).second) {
          continue;
        }
        std::vector<int> pos_in_cand(static_cast<size_t>(n), -1);
        for (int pos = 0; pos < n; ++pos) {
          pos_in_cand[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])] = pos;
        }

        std::vector<NoteTransfer> transfers;
        auto find_cand_last_reader = [&](int i, int r, int nr) -> int {
          int last_reader = -1;
          for (int p = pos_in_cand[static_cast<size_t>(i)] + 1; p < epilogue_begin; ++p) {
            const int m = cand_in_base[static_cast<size_t>(p)];
            bool m_defs_r = false;
            for (int k = 0; k < nr; ++k) {
              if (insn_info[static_cast<size_t>(m)].defs.contains(r + k)) {
                m_defs_r = true;
                break;
              }
            }
            if (m_defs_r) {
              break;
            }
            bool m_reads_all = true;
            for (int k = 0; k < nr; ++k) {
              if (!insn_info[static_cast<size_t>(m)].reads.contains(r + k)) {
                m_reads_all = false;
                break;
              }
            }
            if (m_reads_all) {
              last_reader = m;
            }
          }
          // A `sched2` pure-reader `REG_DEAD` inversion (`flow.c:2504-2515`,
          // `sched.c:3415-3611`) only occurs when `sched2` hoisted the
          // pre-`sched2` last reader `last_reader` ahead of `i` (`last_reader <
          // i` in post-`sched2` `base_region` order while `pos_in_cand[i] <
          // pos_in_cand[last_reader]`). Reject `last_reader >= i` so a
          // candidate that keeps `i` ahead of `last_reader` never undoes an
          // already-inverted prologue-hoisted `REG_DEAD` (`function.c:5516`).
          if (last_reader < 0 || last_reader >= i ||
              insn_info[static_cast<size_t>(last_reader)].is_call_or_use) {
            return -1;
          }
          for (int k = 0; k < nr; ++k) {
            if (insn_info[static_cast<size_t>(last_reader)].deaths.contains(r + k)) {
              return -1;
            }
          }
          const int lo = std::min(i, last_reader);
          const int hi = std::max(i, last_reader);
          for (int k = lo + 1; k < hi; ++k) {
            for (int w = 0; w < nr; ++w) {
              if (insn_info[static_cast<size_t>(k)].defs.contains(r + w)) {
                return -1;
              }
            }
          }
          return last_reader;
        };

        for (int i = prologue_end; i < epilogue_begin; ++i) {
          if (insn_info[static_cast<size_t>(i)].is_call_or_use) {
            continue;
          }
          const auto& notes = base_region.sched_block[static_cast<size_t>(i)].pn.reg_notes;
          std::vector<size_t> const_zero_note_idxs;
          for (size_t ni = 0; ni < notes.size(); ++ni) {
            const RtlRegNote& note = notes[ni];
            if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kConstInt &&
                note.datum.int_val == 0) {
              const_zero_note_idxs.push_back(ni);
              continue;
            }
            if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
              continue;
            }
            const int r = note.datum.RegNo();
            if (r < 0 || r >= kMipsFirstPseudoRegister) {
              continue;
            }
            const int nr = mm.HardRegNRegs(r, note.datum.mode);
            bool i_defs_r = false;
            for (int k = 0; k < nr; ++k) {
              if (insn_info[static_cast<size_t>(i)].defs.contains(r + k)) {
                i_defs_r = true;
                break;
              }
            }
            if (i_defs_r) {
              continue;
            }
            const int last_reader = find_cand_last_reader(i, r, nr);
            if (last_reader >= 0 &&
                !IsCrossClassFprToGprCopyReloadInheritedRead(
                    base_region.sched_block, cand_in_base, pos_in_cand[static_cast<size_t>(i)],
                    pos_in_cand[static_cast<size_t>(last_reader)], r, mm)) {
              transfers.push_back(NoteTransfer{
                  .from_idx = i,
                  .note_idx = ni,
                  .to_idx = last_reader,
                  .note = note,
              });
            }
          }
          // `reorg.c:918-920`: delay-slot instructions carry `(REG_DEAD (const_int 0))`
          // placeholders; if `i` was moved from before `last_reader` by `sched2` or
          // `reorg.c:fill_simple_delay_slots`, move one `(const_int 0)` death to
          // `last_reader` as a concrete `(REG_DEAD (reg ...))` note.
          const size_t vidx = base_region.sched_orig_indices[static_cast<size_t>(i)];
          if (!const_zero_note_idxs.empty() && block_views[b].view_to_orig[vidx].is_delay_slot) {
            size_t zero_cursor = 0;
            absl::btree_set<int> handled_regs;
            for (int r : insn_info[static_cast<size_t>(i)].reads) {
              if (zero_cursor >= const_zero_note_idxs.size()) {
                break;
              }
              if (r <= 0 || r >= kMipsFirstPseudoRegister || r == 28 ||
                  r == kMipsStackPointerRegno ||
                  (cfg_ctx.stack_layout.frame_pointer_needed && r == kMipsFramePointerRegno) ||
                  handled_regs.contains(r)) {
                continue;
              }
              const std::optional<RtlExpr> reg_expr = FindUsedHardRegExprInPattern(
                  base_region.sched_block[static_cast<size_t>(i)].pn.pattern, r);
              if (!reg_expr.has_value()) {
                continue;
              }
              const int nr = mm.HardRegNRegs(r, reg_expr->mode);
              bool defs_r = false;
              bool live_after_in_base = false;
              for (int k = 0; k < nr; ++k) {
                if (insn_info[static_cast<size_t>(i)].defs.contains(r + k)) {
                  defs_r = true;
                  break;
                }
                if (live_after_base[static_cast<size_t>(i)].contains(r + k)) {
                  live_after_in_base = true;
                }
              }
              if (defs_r) {
                // `flow.c:2506`, `stmt.c:expand_return`, `reorg.c:918-920`: when
                // a delay-slot return-value `SET` `(set (reg/i:M r) (... r ...))`
                // overwrites a dying pseudo allocated to hard reg `r`, `reorg`
                // zeroes `(REG_DEAD (reg:M r))` to `(REG_DEAD (const_int 0))`.
                // Resolve it in-place onto `i` so `CollectSched2ObstructedMemRegEquivAdditions`
                // observes the exact death boundary of `r` at `i`.
                const RtlExpr* s_dest =
                    SingleSetOf(base_region.sched_block[static_cast<size_t>(i)].pn.pattern);
                if (s_dest != nullptr && s_dest->SetDest().op == RtlOp::kReg &&
                    s_dest->SetDest().flags.integrated && s_dest->SetDest().RegNo() == r &&
                    !HasRegDeadNoteCovering(notes, r, mm)) {
                  transfers.push_back(NoteTransfer{
                      .from_idx = i,
                      .note_idx = const_zero_note_idxs[zero_cursor++],
                      .to_idx = i,
                      .note =
                          RtlRegNote{
                              .kind = RtlMachineMode::kRegDead,
                              .list_op = RtlOp::kExprList,
                              .datum = *reg_expr,
                          },
                  });
                  for (int k = 0; k < nr; ++k) {
                    handled_regs.insert(r + k);
                  }
                }
                continue;
              }
              if (IsCrossBankFprInputReloadOperandInOrder(base_region.sched_block, cand_in_base,
                                                          pos_in_cand[static_cast<size_t>(i)], r,
                                                          reg_expr->mode, mm)) {
                continue;
              }
              int target_idx = -1;
              if (!live_after_in_base) {
                target_idx = find_cand_last_reader(i, r, nr);
              } else if (!HasRegDeadNoteCovering(notes, r, mm)) {
                // `reorg.c:918-920` + `sched.c:schedule_block`: `sched2` moved a
                // pure reader of `r` after `i` (keeping `r` live after `i` in
                // `base_region`), while in `cand_in_base` (`kSched1` preimage)
                // `i` itself is the true last reader of `r`. Resolve one
                // `(const_int 0)` placeholder in-place onto `i` so `Pass01`'s
                // `ResolvePostReloadConstZeroRegDeadNotes` preserves `i`'s death
                // instead of attributing `(REG_DEAD r)` to the later `sched2` reader.
                bool live_after_in_cand = false;
                for (int k = 0; k < nr; ++k) {
                  if (live_at_end_s2.contains(r + k)) {
                    live_after_in_cand = true;
                    break;
                  }
                }
                for (int p = pos_in_cand[static_cast<size_t>(i)] + 1;
                     p < epilogue_begin && !live_after_in_cand; ++p) {
                  const int m = cand_in_base[static_cast<size_t>(p)];
                  bool m_defs_all = true;
                  for (int k = 0; k < nr; ++k) {
                    if (insn_info[static_cast<size_t>(m)].reads.contains(r + k)) {
                      live_after_in_cand = true;
                      break;
                    }
                    if (!insn_info[static_cast<size_t>(m)].defs.contains(r + k)) {
                      m_defs_all = false;
                    }
                  }
                  if (m_defs_all) {
                    break;
                  }
                }
                if (!live_after_in_cand) {
                  target_idx = i;
                }
              }
              if (target_idx >= 0) {
                transfers.push_back(NoteTransfer{
                    .from_idx = i,
                    .note_idx = const_zero_note_idxs[zero_cursor++],
                    .to_idx = target_idx,
                    .note =
                        RtlRegNote{
                            .kind = RtlMachineMode::kRegDead,
                            .list_op = RtlOp::kExprList,
                            .datum = *reg_expr,
                        },
                });
                for (int k = 0; k < nr; ++k) {
                  handled_regs.insert(r + k);
                }
              }
            }
          }
        }

        // Apply pure-reader `REG_DEAD` transfers, then discover any `sched2`-obstructed
        // `REG_EQUIV (mem ...)` notes (`local-alloc.c:540-665, 990-1000, 1049-1055`).
        std::vector<Insn> transferred_base = base_region.sched_block;
        absl::btree_map<int, absl::btree_set<size_t>> erase_indices;
        for (const NoteTransfer& tr : transfers) {
          erase_indices[tr.from_idx].insert(tr.note_idx);
        }
        for (const auto& [from_idx, idxs] : erase_indices) {
          auto& notes = transferred_base[static_cast<size_t>(from_idx)].pn.reg_notes;
          std::vector<RtlRegNote> kept;
          kept.reserve(notes.size());
          for (size_t ni = 0; ni < notes.size(); ++ni) {
            if (!idxs.contains(ni)) {
              kept.push_back(std::move(notes[ni]));
            }
          }
          notes = std::move(kept);
        }
        for (const NoteTransfer& tr : transfers) {
          transferred_base[static_cast<size_t>(tr.to_idx)].pn.reg_notes.push_back(tr.note);
        }
        std::vector<std::pair<int, int>> resolved_dead_regs;
        for (const NoteTransfer& tr : transfers) {
          if (tr.note.datum.op == RtlOp::kReg) {
            resolved_dead_regs.push_back({tr.to_idx, tr.note.datum.RegNo()});
          }
        }
        absl::btree_set<int> stripped_equiv_targets;
        if (!resolved_dead_regs.empty()) {
          StripSpuriousStoreMemRegEquivForMovedReaders(
              resolved_dead_regs, prologue_end, epilogue_begin, insn_info, cfg_ctx.stack_layout, mm,
              pos_in_cand, &transferred_base, &stripped_equiv_targets);
        }
        std::vector<std::pair<int, RtlRegNote>> dead_targets;
        dead_targets.reserve(transfers.size());
        for (const NoteTransfer& tr : transfers) {
          dead_targets.emplace_back(tr.to_idx, tr.note);
        }
        StripInvalidatedMemRegEquivAfterDeadTransfers(dead_targets, prologue_end, cand_in_base,
                                                      pos_in_cand, insn_info, mm, &transferred_base,
                                                      &stripped_equiv_targets);
        StripSched2SpuriousLoadMemRegEquivNotes(prologue_end, epilogue_begin, cand_in_base,
                                                pos_in_cand, facts_s2, mm, cfg_ctx.stack_layout,
                                                proj_ctx.excluded_prologue_body_indices,
                                                &transferred_base, &stripped_equiv_targets);
        StripSched2SpuriousStoreMemRegEquivNotes(prologue_end, epilogue_begin, cand_in_base,
                                                 pos_in_cand, facts_s2, mm, cfg_ctx.stack_layout,
                                                 proj_ctx.excluded_prologue_body_indices,
                                                 &transferred_base, &stripped_equiv_targets);

        std::vector<Insn> transferred_cand;
        transferred_cand.reserve(static_cast<size_t>(n));
        for (int pos = 0; pos < n; ++pos) {
          Insn insn = transferred_base[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
          insn.luid = pos;
          transferred_cand.push_back(std::move(insn));
        }

        const std::vector<Stage0MemEquivAddition> equiv_additions =
            CollectSched2ObstructedMemRegEquivAdditions(
                base_region.sched_block, transferred_cand, cand_in_base, prologue_end,
                epilogue_begin, live_at_end_s2, facts_s2, mm, cfg_ctx.stack_layout,
                proj_ctx.excluded_prologue_body_indices);
        absl::btree_set<int> added_equiv_targets;
        for (const Stage0MemEquivAddition& add : equiv_additions) {
          added_equiv_targets.insert(add.target_sched_idx);
          auto& bnotes = transferred_base[static_cast<size_t>(add.target_sched_idx)].pn.reg_notes;
          bnotes.insert(bnotes.begin(), add.note);
          auto& cnotes =
              transferred_cand[static_cast<size_t>(
                                   pos_in_cand[static_cast<size_t>(add.target_sched_idx)])]
                  .pn.reg_notes;
          cnotes.insert(cnotes.begin(), add.note);
        }

        std::vector<Insn> cand_body;
        cand_body.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          Insn insn = transferred_cand[static_cast<size_t>(pos)];
          insn.luid = pos - prologue_end;
          cand_body.push_back(std::move(insn));
        }
        Sched1ProjectionContext cand_proj_ctx = proj_ctx;
        cand_proj_ctx.excluded_prologue_body_indices =
            FindInterleavedPrologueSaveBodyIndices(transferred_cand, bounds, b, mm);
        Sched1ProjectionContext unsplit_cand_ctx = cand_proj_ctx;
        unsplit_cand_ctx.allow_post_call_combine_regs_split = false;
        unsplit_cand_ctx.allow_general_combine_regs_split = false;
        const Sched1ProjectionView unsplit_cand_view =
            ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, unsplit_cand_ctx);
        absl::btree_set<int> rmw_dead_sched_idxs;
        if (unsplit_cand_view.core_insns.size() <= 1) {
          continue;
        }
        const bool unsplit_ach =
            IsAchievable(unsplit_cand_view.core_insns, unsplit_cand_view.facts_s1, mm,
                         Phase::kSched1, unsplit_cand_view.live_at_end_s1);
        if (!unsplit_ach) {
          if (!allow_post_call_rmw_splits && !allow_general_rmw_splits) {
            continue;
          }
          // Restore minimal erased `(REG_DEAD r)` + store-equiv `(REG_EQUIV (mem ...))`
          // notes on block-local `combine_regs` RMWs whenever the preimage only
          // achieves `kSched1` with those splits (`local-alloc.c:1266-1300, 1851-1886,
          // 990-1000`, `sched.c:1732, 2490`).
          const absl::btree_set<int> cand_rmw_splits = SelectMinimalCombineRegsRmwSplits(
              cand_body, cand_proj_ctx, live_at_end_s2, mm, allow_general_rmw_splits);
          if (cand_rmw_splits.empty()) {
            continue;
          }
          for (int body_idx : cand_rmw_splits) {
            const int cand_pos = prologue_end + body_idx;
            const int orig_si = cand_in_base[static_cast<size_t>(cand_pos)];
            const RtlExpr* s =
                SingleSetOf(transferred_cand[static_cast<size_t>(cand_pos)].pn.pattern);
            if (s != nullptr && s->SetDest().op == RtlOp::kReg &&
                !HasRegDeadNoteCovering(
                    transferred_cand[static_cast<size_t>(cand_pos)].pn.reg_notes,
                    s->SetDest().RegNo(), mm)) {
              const RtlRegNote dead_note{
                  .kind = RtlMachineMode::kRegDead,
                  .list_op = RtlOp::kExprList,
                  .datum = s->SetDest(),
              };
              auto& bnotes = transferred_base[static_cast<size_t>(orig_si)].pn.reg_notes;
              bnotes.insert(bnotes.begin(), dead_note);
              auto& cnotes = transferred_cand[static_cast<size_t>(cand_pos)].pn.reg_notes;
              cnotes.insert(cnotes.begin(), dead_note);
              auto& body_notes = cand_body[static_cast<size_t>(body_idx)].pn.reg_notes;
              body_notes.insert(body_notes.begin(), dead_note);
              rmw_dead_sched_idxs.insert(orig_si);
            }
          }
          if (rmw_dead_sched_idxs.empty()) {
            continue;
          }
          const std::vector<Stage0MemEquivAddition> split_equivs =
              CollectSched2ObstructedMemRegEquivAdditions(
                  base_region.sched_block, transferred_cand, cand_in_base, prologue_end,
                  epilogue_begin, live_at_end_s2, facts_s2, mm, cfg_ctx.stack_layout,
                  proj_ctx.excluded_prologue_body_indices, rmw_dead_sched_idxs);
          for (const Stage0MemEquivAddition& add : split_equivs) {
            if (!added_equiv_targets.insert(add.target_sched_idx).second) {
              continue;
            }
            auto& bnotes = transferred_base[static_cast<size_t>(add.target_sched_idx)].pn.reg_notes;
            bnotes.insert(bnotes.begin(), add.note);
            const int cpos = pos_in_cand[static_cast<size_t>(add.target_sched_idx)];
            auto& cnotes = transferred_cand[static_cast<size_t>(cpos)].pn.reg_notes;
            cnotes.insert(cnotes.begin(), add.note);
            if (cpos >= prologue_end && cpos < epilogue_begin) {
              auto& body_notes = cand_body[static_cast<size_t>(cpos - prologue_end)].pn.reg_notes;
              body_notes.insert(body_notes.begin(), add.note);
            }
            rmw_dead_sched_idxs.insert(add.target_sched_idx);
          }
          const Sched1ProjectionView verify_cand_view =
              ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, unsplit_cand_ctx);
          if (verify_cand_view.core_insns.size() <= 1 ||
              !IsAchievable(verify_cand_view.core_insns, verify_cand_view.facts_s1, mm,
                            Phase::kSched1, verify_cand_view.live_at_end_s1)) {
            continue;
          }
        }

        // Strictly verify remaining gates:
        //   (a) forward `kSched2` replay on `transferred_cand` reproduces `transferred_base`,
        //   (b) `PreservesPostReloadOrderSafety(..., allow_transfer=false)` passes.
        const DepGraph g_cand_s2 = Analyze(transferred_cand, facts_s2, mm, Phase::kSched2);
        const ScheduleResult s2_replay =
            Schedule(transferred_cand, g_cand_s2, facts_s2, mm, Phase::kSched2, live_at_end_s2);
        if (static_cast<int>(s2_replay.order.size()) != n) {
          continue;
        }
        bool replay_matches = true;
        for (int pos = 0; pos < n; ++pos) {
          if (cand_in_base[static_cast<size_t>(s2_replay.order[static_cast<size_t>(pos)])] != pos) {
            replay_matches = false;
            break;
          }
        }
        const bool replay_ok = replay_matches;
        const bool safe_ok =
            replay_ok && PreservesPostReloadOrderSafety(
                             transferred_base, cand_in_base, prologue_end, epilogue_begin,
                             live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                             /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
                             proj_ctx.excluded_prologue_body_indices,
                             /*allow_moved_store_load_equiv_invalidation=*/
                             !stripped_equiv_targets.empty());
        if (!safe_ok) {
          continue;
        }

        absl::btree_set<int> combine_repair_touched;
        TryRepairCombineRegsPureReaderRegDeadConflicts(
            b, bounds, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2, reload_adjacent,
            cand_in_base,
            /*allow_moved_store_load_equiv_invalidation=*/!stripped_equiv_targets.empty(),
            &transferred_base, &combine_repair_touched);

        if (transfers.empty() && equiv_additions.empty() && stripped_equiv_targets.empty() &&
            rmw_dead_sched_idxs.empty() && alignment_touched_sched_idxs.empty() &&
            combine_repair_touched.empty()) {
          saw_zero_mutation_solution = true;
          continue;
        }

        // Commit the verified `REG_DEAD` transfers, `REG_EQUIV` recoveries, and
        // `combine_regs` RMW `REG_DEAD` attributions back to `cfg->rpo_blocks[b]`.
        absl::btree_set<int> touched_sched_idxs = alignment_touched_sched_idxs;
        for (const NoteTransfer& tr : transfers) {
          touched_sched_idxs.insert(tr.from_idx);
          touched_sched_idxs.insert(tr.to_idx);
        }
        for (const Stage0MemEquivAddition& add : equiv_additions) {
          touched_sched_idxs.insert(add.target_sched_idx);
        }
        for (int si : stripped_equiv_targets) {
          touched_sched_idxs.insert(si);
        }
        for (int si : rmw_dead_sched_idxs) {
          touched_sched_idxs.insert(si);
        }
        for (int si : combine_repair_touched) {
          touched_sched_idxs.insert(si);
        }
        StripJump2ConsumedEarlyclobberOutputReloadDeaths(prologue_end, epilogue_begin, mm,
                                                         &transferred_base);
        CommitStage0AttributedNotesToBlock(base_region, transferred_base, touched_sched_idxs,
                                           &block_views[b], &cfg->rpo_blocks[b],
                                           &cfg->sideband.epilogue_delay_pattern);
        return PreimagePassResult::kCommitted;
      }
      return saw_zero_mutation_solution ? PreimagePassResult::kSolvedWithoutMutation
                                        : PreimagePassResult::kNotSolved;
    };

    auto try_identity_rmw_splits = [&](bool allow_general) -> bool {
      const absl::btree_set<int> id_rmw_splits =
          SelectMinimalCombineRegsRmwSplits(base_body, proj_ctx, live_at_end_s2, mm, allow_general);
      if (id_rmw_splits.empty()) {
        return false;
      }
      std::vector<Insn> updated_base = base_region.sched_block;
      absl::btree_set<int> touched;
      for (int body_idx : id_rmw_splits) {
        const int si = prologue_end + body_idx;
        const RtlExpr* s = SingleSetOf(updated_base[static_cast<size_t>(si)].pn.pattern);
        if (s != nullptr && s->SetDest().op == RtlOp::kReg &&
            !HasRegDeadNoteCovering(updated_base[static_cast<size_t>(si)].pn.reg_notes,
                                    s->SetDest().RegNo(), mm)) {
          auto& notes = updated_base[static_cast<size_t>(si)].pn.reg_notes;
          notes.insert(notes.begin(), RtlRegNote{
                                          .kind = RtlMachineMode::kRegDead,
                                          .list_op = RtlOp::kExprList,
                                          .datum = s->SetDest(),
                                      });
          touched.insert(si);
        }
      }
      if (touched.empty() ||
          !InvertBlockOrder(updated_base, facts_s2, mm, Phase::kSched2, live_at_end_s2)
               .has_value()) {
        return false;
      }
      std::vector<int> id_perm(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        id_perm[static_cast<size_t>(i)] = i;
      }
      const std::vector<Stage0MemEquivAddition> split_equivs =
          CollectSched2ObstructedMemRegEquivAdditions(
              base_region.sched_block, updated_base, id_perm, prologue_end, epilogue_begin,
              live_at_end_s2, facts_s2, mm, cfg_ctx.stack_layout,
              proj_ctx.excluded_prologue_body_indices, touched);
      for (const Stage0MemEquivAddition& add : split_equivs) {
        auto& bnotes = updated_base[static_cast<size_t>(add.target_sched_idx)].pn.reg_notes;
        bnotes.insert(bnotes.begin(), add.note);
        touched.insert(add.target_sched_idx);
      }
      std::vector<Insn> updated_body;
      updated_body.reserve(static_cast<size_t>(body_len));
      for (int i = prologue_end; i < epilogue_begin; ++i) {
        Insn insn = updated_base[static_cast<size_t>(i)];
        insn.luid = i - prologue_end;
        updated_body.push_back(std::move(insn));
      }
      const Sched1ProjectionView verify_view =
          ProjectPostReloadBlockToSched1View(updated_body, mm, live_at_end_s2, unsplit_base_ctx);
      if (verify_view.core_insns.size() <= 1 ||
          !IsAchievable(verify_view.core_insns, verify_view.facts_s1, mm, Phase::kSched1,
                        verify_view.live_at_end_s1)) {
        return false;
      }
      TryRepairCombineRegsPureReaderRegDeadConflicts(
          b, bounds, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2, reload_adjacent, id_perm,
          /*allow_moved_store_load_equiv_invalidation=*/false, &updated_base, &touched);
      touched.insert(alignment_touched_sched_idxs.begin(), alignment_touched_sched_idxs.end());
      CommitStage0AttributedNotesToBlock(base_region, updated_base, touched, &block_views[b],
                                         &cfg->rpo_blocks[b],
                                         &cfg->sideband.epilogue_delay_pattern);
      return true;
    };

    // Pass 1a: post-call `combine_regs` identity splits (`sched.c:1732`,
    // `local-alloc.c:1266-1300`) match Stage 3's default projection and run
    // before pure-reorder exit so Stage 0 and Stage 3 never disagree.
    if (try_identity_rmw_splits(/*allow_general=*/false)) {
      continue;
    }

    // Pass 1b: try `kSched2` preimages with post-call `combine_regs` RMW splits
    // (without general intra-block RMW splits).
    if (run_preimage_pass(base_preimages, /*allow_post_call_rmw_splits=*/true,
                          /*allow_general_rmw_splits=*/false) != PreimagePassResult::kNotSolved) {
      continue;
    }

    // Pass 1c: invert `reorg.c:918-920` in-place when `sched2` moved a pure
    // reader past a delay-slot insn carrying `(REG_DEAD (const_int 0))`.
    if (TryResolveStage0DelaySlotConstZeroInPlace(
            base_region, b, bounds, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2,
            reload_adjacent, insn_info, live_after_base, alignment_touched_sched_idxs,
            &block_views[b], &cfg->rpo_blocks[b])) {
      continue;
    }

    // Pass 2: general intra-block `combine_regs` identity RMW splits when no
    // zero-split or post-call `kSched2` preimage exists.
    if (try_identity_rmw_splits(/*allow_general=*/true)) {
      continue;
    }

    // Pass 3: discover `kSched2` preimages with general `combine_regs` RMW splits
    // enabled so blocks needing BOTH a `sched2` reorder and an RMW `(REG_DEAD r)`
    // attribution are found cleanly (trying call-used-only RMW splits first so
    // callee-saved multi-set accumulators do not distort `birthing_insn_p`).
    std::vector<std::vector<int>> split_preimages = base_preimages;
    Sched1ProjectionContext cu_proj_ctx = proj_ctx;
    cu_proj_ctx.allow_call_used_general_combine_regs_split = true;
    const Sched1ProjectionView cu_base_view =
        ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, cu_proj_ctx);
    if (!cu_base_view.implicit_combine_regs_split_orig_indices.empty()) {
      for (std::vector<int>& gc : FindStage0Sched2PreimageCandidates(
               base_region.sched_block, b, bounds, cu_base_view, cu_proj_ctx, cfg_ctx, facts_s2, mm,
               live_at_end_s2, reload_adjacent)) {
        split_preimages.push_back(std::move(gc));
      }
    }
    Sched1ProjectionContext gen_proj_ctx = proj_ctx;
    gen_proj_ctx.allow_general_combine_regs_split = true;
    const Sched1ProjectionView gen_base_view =
        ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, gen_proj_ctx);
    if (!gen_base_view.implicit_combine_regs_split_orig_indices.empty()) {
      std::vector<std::vector<int>> gen_cands = FindStage0Sched2PreimageCandidates(
          base_region.sched_block, b, bounds, gen_base_view, gen_proj_ctx, cfg_ctx, facts_s2, mm,
          live_at_end_s2, reload_adjacent);
      for (std::vector<int>& gc : gen_cands) {
        split_preimages.push_back(std::move(gc));
      }
    }
    if (!split_preimages.empty()) {
      if (run_preimage_pass(split_preimages, /*allow_post_call_rmw_splits=*/true,
                            /*allow_general_rmw_splits=*/true) != PreimagePassResult::kNotSolved) {
        continue;
      }
    }

    // Pass 4: symmetric inverse of Pass 2 (`flow.c:2506-2515`, `sched.c:2490`)
    // — when all preimage and split passes fail to make the block
    // `kSched1`-achievable, retract a minimal speculative self-set `REG_DEAD`
    // if doing so restores both `kSched2` and `kSched1` achievability.
    (void)TryRetractSpeculativeSelfSetRmwRegDeadNotes(
        base_region, b, bounds, unsplit_base_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2,
        alignment_touched_sched_idxs, &block_views[b], &cfg->rpo_blocks[b]);
  }
}

namespace {

template <typename Fn>
void ForEachStmtInPattern(const RtlExpr& pat, Fn&& fn) {
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : pat.children) {
      fn(ch);
    }
  } else {
    fn(pat);
  }
}

template <typename Fn>
void ForEachMemInExpr(const RtlExpr& node, Fn&& fn) {
  if (node.op == RtlOp::kMem) {
    fn(node);
    return;
  }
  for (const RtlExpr& ch : node.children) {
    ForEachMemInExpr(ch, fn);
  }
}

template <typename Fn>
void ForEachMemInExprMut(RtlExpr* node, Fn&& fn) {
  if (node->op == RtlOp::kMem) {
    fn(node);
    return;
  }
  for (RtlExpr& ch : node->children) {
    ForEachMemInExprMut(&ch, fn);
  }
}

bool ExtractBasePlusConst(const RtlExpr& addr, int* reg_out, int64_t* off_out) {
  if (addr.op == RtlOp::kReg) {
    *reg_out = addr.RegNo();
    *off_out = 0;
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      addr.children[1].op == RtlOp::kConstInt) {
    *reg_out = addr.children[0].RegNo();
    *off_out = addr.children[1].int_val;
    return true;
  }
  return false;
}

// Extracts the base symbol name and whether an explicit non-zero offset is
// present from a static address expression (`varasm.c:588-604`, `mips.md`).
bool ExtractStaticSymbolFromAddr(const RtlExpr& addr, std::string* sym_out, bool* has_offset_out) {
  if (addr.op == RtlOp::kSymbolRef && !addr.str_val.empty()) {
    *sym_out = addr.str_val;
    *has_offset_out = false;
    return true;
  }
  if (addr.op == RtlOp::kConst && addr.children.size() == 1) {
    return ExtractStaticSymbolFromAddr(addr.children[0], sym_out, has_offset_out);
  }
  if (addr.op == RtlOp::kLoSum && addr.children.size() == 2) {
    return ExtractStaticSymbolFromAddr(addr.children[1], sym_out, has_offset_out);
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2) {
    for (int i = 0; i < 2; ++i) {
      const RtlExpr& a = addr.children[static_cast<size_t>(i)];
      const RtlExpr& b = addr.children[static_cast<size_t>(1 - i)];
      if (a.op == RtlOp::kSymbolRef && !a.str_val.empty() && b.op == RtlOp::kConstInt) {
        *sym_out = a.str_val;
        *has_offset_out = (b.int_val != 0);
        return true;
      }
    }
  }
  return false;
}

struct SymRegOrigin {
  std::string symbol;
  bool aggregate_access = false;
  bool operator<(const SymRegOrigin& o) const {
    if (symbol != o.symbol) return symbol < o.symbol;
    return aggregate_access < o.aggregate_access;
  }
  bool operator==(const SymRegOrigin& o) const {
    return symbol == o.symbol && aggregate_access == o.aggregate_access;
  }
};

using SymOriginMap = absl::btree_map<int, absl::btree_set<SymRegOrigin>>;

void ResolveStaticSymbolsFromAddr(const RtlExpr& addr, const SymOriginMap& origins,
                                  absl::btree_set<SymRegOrigin>* out) {
  std::string sym;
  bool has_off = false;
  if (ExtractStaticSymbolFromAddr(addr, &sym, &has_off)) {
    out->insert(SymRegOrigin{.symbol = std::move(sym), .aggregate_access = has_off});
    return;
  }
  int base = -1;
  int64_t off = 0;
  if (ExtractBasePlusConst(addr, &base, &off)) {
    if (const auto it = origins.find(base); it != origins.end()) {
      for (const SymRegOrigin& orig : it->second) {
        out->insert(SymRegOrigin{.symbol = orig.symbol,
                                 .aggregate_access = orig.aggregate_access || (off != 0)});
      }
    }
  }
}

bool ResolveUniqueStaticSymbolFromAddr(const RtlExpr& addr, const SymOriginMap& origins,
                                       std::string* sym_out, bool* agg_out) {
  absl::btree_set<SymRegOrigin> resolved;
  ResolveStaticSymbolsFromAddr(addr, origins, &resolved);
  if (resolved.empty()) return false;
  const std::string& first_sym = resolved.begin()->symbol;
  bool agg = false;
  for (const SymRegOrigin& o : resolved) {
    if (o.symbol != first_sym) return false;
    agg = agg || o.aggregate_access;
  }
  *sym_out = first_sym;
  *agg_out = agg;
  return true;
}

// Propagates static-symbol address provenance sets through a single
// instruction's `SET` destination (`varasm.c:588-604`).
void UpdateSymRegOriginsForInsn(const Insn& insn, const MachineModel& mm, SymOriginMap* origins) {
  if (insn.kind == Insn::Kind::kCall) {
    for (auto it = origins->begin(); it != origins->end();) {
      it = mm.CallUsed(it->first) ? origins->erase(it) : std::next(it);
    }
    return;
  }
  const RtlExpr& pat = insn.pn.pattern;
  if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg) {
    const int dst = pat.SetDest().RegNo();
    const RtlExpr& src = pat.SetSrc();
    std::string sym;
    bool has_off = false;
    if (ExtractStaticSymbolFromAddr(src, &sym, &has_off) ||
        (src.op == RtlOp::kHigh && src.children.size() == 1 &&
         ExtractStaticSymbolFromAddr(src.children[0], &sym, &has_off))) {
      (*origins)[dst] = {SymRegOrigin{.symbol = std::move(sym), .aggregate_access = has_off}};
      return;
    }
    int base = -1;
    int64_t off = 0;
    if (ExtractBasePlusConst(src, &base, &off)) {
      if (const auto it = origins->find(base); it != origins->end() && !it->second.empty()) {
        absl::btree_set<SymRegOrigin> next;
        for (const SymRegOrigin& o : it->second) {
          next.insert(SymRegOrigin{.symbol = o.symbol,
                                   .aggregate_access = o.aggregate_access || (off != 0)});
        }
        (*origins)[dst] = std::move(next);
        return;
      }
    } else if (src.op == RtlOp::kPlus && src.children.size() == 2 &&
               src.children[0].op == RtlOp::kReg && src.children[1].op == RtlOp::kReg) {
      absl::btree_set<SymRegOrigin> next;
      for (int r : {src.children[0].RegNo(), src.children[1].RegNo()}) {
        if (const auto it = origins->find(r); it != origins->end()) {
          for (const SymRegOrigin& o : it->second) {
            next.insert(SymRegOrigin{.symbol = o.symbol, .aggregate_access = true});
          }
        }
      }
      if (!next.empty()) {
        (*origins)[dst] = std::move(next);
        return;
      }
    } else if (src.op == RtlOp::kMinus && src.children.size() == 2 &&
               src.children[0].op == RtlOp::kReg) {
      if (const auto it = origins->find(src.children[0].RegNo());
          it != origins->end() && !it->second.empty()) {
        absl::btree_set<SymRegOrigin> next;
        for (const SymRegOrigin& o : it->second) {
          next.insert(SymRegOrigin{.symbol = o.symbol, .aggregate_access = true});
        }
        (*origins)[dst] = std::move(next);
        return;
      }
    }
    origins->erase(dst);
    return;
  }
  absl::btree_set<int> fdefs, pdefs;
  CollectFullRegDefs(pat, &fdefs, &pdefs);
  for (int r : fdefs) origins->erase(r);
  for (int r : pdefs) origins->erase(r);
}

struct CfgReadOnlySymbolInfo {
  absl::btree_set<std::string> read_only_symbols;
  absl::btree_set<std::string> aggregate_symbols;
  std::vector<SymOriginMap> entry_sym_origins;
};

// Identifies non-volatile static symbols that are read without ever being
// written or having their address escaped across `view_cfg` (`varasm.c:601-604`),
// propagating static-symbol register provenance across CFG edges via RPO fixpoint.
CfgReadOnlySymbolInfo CollectCfgReadOnlyStaticSymbols(const RtlControlFlowGraph& view_cfg,
                                                      const MachineModel& mm) {
  const size_t nb = view_cfg.rpo_blocks.size();
  std::vector<ParsedSchedRegion> regions(nb);
  for (size_t b = 0; b < nb; ++b) {
    regions[b] = ParseBlockForSchedModel(view_cfg.rpo_blocks[b], b, Phase::kSched2);
  }
  std::vector<SymOriginMap> in_origins(nb);
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < nb; ++b) {
      SymOriginMap cur = in_origins[b];
      if (regions[b].valid) {
        for (const Insn& insn : regions[b].sched_block) {
          UpdateSymRegOriginsForInsn(insn, mm, &cur);
        }
      }
      for (int succ : view_cfg.rpo_blocks[b].successors) {
        if (succ < 0 || static_cast<size_t>(succ) >= nb) continue;
        SymOriginMap& dst_map = in_origins[static_cast<size_t>(succ)];
        for (const auto& [reg, syms] : cur) {
          auto& dst_set = dst_map[reg];
          const size_t before = dst_set.size();
          dst_set.insert(syms.begin(), syms.end());
          if (dst_set.size() != before) changed = true;
        }
      }
    }
  }

  absl::btree_set<std::string> read_syms, written_or_escaped, aggregate_syms;
  for (size_t b = 0; b < nb; ++b) {
    if (!regions[b].valid) continue;
    SymOriginMap origins = in_origins[b];
    for (const Insn& insn : regions[b].sched_block) {
      if (insn.kind == Insn::Kind::kCall) {
        absl::btree_set<int> used_regs;
        for (const RtlExpr& fu : insn.pn.call_fusage) CollectRegReads(fu, &used_regs);
        for (int r : used_regs) {
          if (const auto it = origins.find(r); it != origins.end()) {
            for (const SymRegOrigin& o : it->second) written_or_escaped.insert(o.symbol);
          }
        }
        UpdateSymRegOriginsForInsn(insn, mm, &origins);
        continue;
      }
      auto handle_read_mem = [&](const RtlExpr& mem) {
        if (mem.children.empty()) return;
        absl::btree_set<SymRegOrigin> resolved;
        ResolveStaticSymbolsFromAddr(mem.children[0], origins, &resolved);
        for (const SymRegOrigin& o : resolved) {
          if (mem.flags.volatil) {
            written_or_escaped.insert(o.symbol);
          } else {
            read_syms.insert(o.symbol);
            if (o.aggregate_access) aggregate_syms.insert(o.symbol);
          }
        }
      };
      ForEachStmtInPattern(insn.pn.pattern, [&](const RtlExpr& stmt) {
        if (stmt.op == RtlOp::kUse && !stmt.children.empty() &&
            stmt.children[0].op == RtlOp::kReg) {
          if (const auto it = origins.find(stmt.children[0].RegNo()); it != origins.end()) {
            for (const SymRegOrigin& o : it->second) written_or_escaped.insert(o.symbol);
          }
          return;
        }
        if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
          if (stmt.SetDest().op == RtlOp::kMem && !stmt.SetDest().children.empty()) {
            absl::btree_set<SymRegOrigin> dst_syms;
            ResolveStaticSymbolsFromAddr(stmt.SetDest().children[0], origins, &dst_syms);
            for (const SymRegOrigin& o : dst_syms) written_or_escaped.insert(o.symbol);
            if (stmt.SetSrc().op == RtlOp::kReg) {
              if (const auto it = origins.find(stmt.SetSrc().RegNo()); it != origins.end()) {
                for (const SymRegOrigin& o : it->second) written_or_escaped.insert(o.symbol);
              }
            }
            std::string sym;
            bool agg = false;
            if (ExtractStaticSymbolFromAddr(stmt.SetSrc(), &sym, &agg)) {
              written_or_escaped.insert(sym);
            }
          }
          ForEachMemInExpr(stmt.SetSrc(), handle_read_mem);
        } else {
          ForEachMemInExpr(stmt, handle_read_mem);
        }
      });
      UpdateSymRegOriginsForInsn(insn, mm, &origins);
    }
  }
  CfgReadOnlySymbolInfo info;
  for (const std::string& s : read_syms) {
    if (!written_or_escaped.contains(s)) info.read_only_symbols.insert(s);
  }
  info.aggregate_symbols = std::move(aggregate_syms);
  info.entry_sym_origins = std::move(in_origins);
  return info;
}

struct MemAliasFlagEdit {
  int sched_idx = -1;
  bool is_write = false;
  bool set_in_struct = false;
  bool set_unchanging = false;
};

struct MemAliasGroup {
  enum class Kind { kReadOnlySymbol, kReadOnlyPtr, kAggregatePtrOffset, kLocalStackArray };
  Kind kind = Kind::kReadOnlySymbol;
  std::vector<MemAliasFlagEdit> edits;
};

// Collects candidate erased `MEM_UNCHANGING_P` (`/u`) and `MEM_IN_STRUCT_P`
// (`/s`) semantic groups in `sched_block` (`varasm.c:601-604`,
// `expr.c:4534, 4571-4586, 4889`).
std::vector<MemAliasGroup> CollectErasedMemAliasGroupsForBlock(
    absl::Span<const Insn> sched_block, const PostReloadBodyBounds& bounds,
    const absl::btree_set<int>& excluded_prologue_body_indices,
    const PostReloadStackLayout& stack_layout, const CfgReadOnlySymbolInfo& ro_syms,
    const SymOriginMap& entry_sym_origins, const MachineModel& mm) {
  const int n = static_cast<int>(sched_block.size());
  const bool fp_needed = stack_layout.valid && stack_layout.frame_pointer_needed;
  auto is_general_ptr_reg = [&](int r) {
    return (r >= 2 && r <= 25) || (r == kMipsFramePointerRegno && !fp_needed);
  };

  absl::btree_map<std::string, std::vector<MemAliasFlagEdit>> sym_edits;
  absl::btree_map<int, int> reg_ver;
  // Tracks transitive `(reg, version)` pointer origins so writing through a
  // derived address (`a2 = a1 + (idx << 2)`) marks `a1` as written and reading
  // through a pointer derived from a written base (`q = out + idx`) suppresses
  // `/u` on `q`.
  absl::btree_map<int, absl::btree_set<std::pair<int, int>>> ptr_bases;
  absl::btree_map<std::pair<int, int>, std::vector<MemAliasFlagEdit>> ptr_ro_reads;
  absl::btree_map<std::pair<int, int>, absl::btree_set<std::pair<int, int>>> ptr_key_origins;
  absl::btree_set<std::pair<int, int>> ptr_written_vers;
  absl::btree_map<std::pair<int, int>, std::vector<MemAliasFlagEdit>> ptr_agg_edits;

  struct StackAccessInfo {
    int sched_idx = -1;
    bool is_write = false;
    bool already_in_struct = false;
  };
  absl::btree_map<std::pair<RtlMachineMode, int64_t>, std::vector<StackAccessInfo>> stack_accesses;
  SymOriginMap origins = entry_sym_origins;

  auto get_ptr_bases = [&](int r) -> absl::btree_set<std::pair<int, int>> {
    if (const auto it = ptr_bases.find(r); it != ptr_bases.end()) return it->second;
    if (is_general_ptr_reg(r)) return {{r, reg_ver[r]}};
    return {};
  };

  auto record_mem = [&](int si, const RtlExpr& mem, bool is_write) {
    if (mem.op != RtlOp::kMem || mem.children.empty() || mem.flags.volatil) return;
    const RtlExpr& addr = mem.children[0];
    std::string sym;
    bool sym_agg = false;
    if (!is_write && ResolveUniqueStaticSymbolFromAddr(addr, origins, &sym, &sym_agg) &&
        ro_syms.read_only_symbols.contains(sym)) {
      const bool want_s =
          (sym_agg || ro_syms.aggregate_symbols.contains(sym)) && mem.mode != RtlMachineMode::kQi;
      if (!mem.flags.unchanging || (want_s && !mem.flags.in_struct)) {
        sym_edits[sym].push_back({si, false, want_s, true});
      }
      return;
    }

    int base = -1;
    int64_t off = 0;
    if (!ExtractBasePlusConst(addr, &base, &off)) return;

    const bool is_frame_base =
        (base == kMipsStackPointerRegno) || (fp_needed && base == kMipsFramePointerRegno);
    if (is_frame_base && stack_layout.valid && mem.mode != RtlMachineMode::kQi) {
      const bool in_local_area = off >= stack_layout.starting_frame_offset &&
                                 (stack_layout.frame_size <= 0 || off < stack_layout.frame_size) &&
                                 (stack_layout.callee_saved_area_offset < 0 ||
                                  off < stack_layout.callee_saved_area_offset) &&
                                 !stack_layout.caller_save_slot_offsets.contains(off);
      const bool is_body = si >= bounds.prologue_end && si < bounds.epilogue_begin &&
                           !excluded_prologue_body_indices.contains(si - bounds.prologue_end);
      if (in_local_area && is_body) {
        stack_accesses[{mem.mode, off}].push_back({si, is_write, mem.flags.in_struct});
      }
      return;
    }

    if (!is_general_ptr_reg(base) || origins.contains(base)) return;
    const std::pair<int, int> key{base, reg_ver[base]};
    if (is_write) {
      ptr_written_vers.insert(key);
      for (const auto& origin_key : get_ptr_bases(base)) ptr_written_vers.insert(origin_key);
    } else {
      const bool want_s = (off != 0 && mem.mode != RtlMachineMode::kQi);
      if (!mem.flags.unchanging || (want_s && !mem.flags.in_struct)) {
        ptr_ro_reads[key].push_back({si, false, want_s, true});
        const auto base_origins = get_ptr_bases(base);
        ptr_key_origins[key].insert(base_origins.begin(), base_origins.end());
      }
    }
    if (off != 0 && mem.mode != RtlMachineMode::kQi && !mem.flags.in_struct) {
      ptr_agg_edits[key].push_back({si, is_write, true, false});
    }
  };

  for (int si = 0; si < n; ++si) {
    const Insn& insn = sched_block[static_cast<size_t>(si)];
    ForEachStmtInPattern(insn.pn.pattern, [&](const RtlExpr& stmt) {
      if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
        if (stmt.SetDest().op == RtlOp::kMem) record_mem(si, stmt.SetDest(), /*is_write=*/true);
        ForEachMemInExpr(stmt.SetSrc(), [&](const RtlExpr& m) { record_mem(si, m, false); });
      } else {
        ForEachMemInExpr(stmt, [&](const RtlExpr& m) { record_mem(si, m, false); });
      }
    });
    // Propagate pointer-base provenance and advance register versions.
    if (insn.kind == Insn::Kind::kCall) {
      for (int r = 2; r <= 25; ++r) {
        if (mm.CallUsed(r)) {
          ++reg_ver[r];
          ptr_bases.erase(r);
        }
      }
    } else {
      absl::btree_set<std::pair<int, int>> derived;
      int single_dst = -1;
      const RtlExpr& pat = insn.pn.pattern;
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.SetDest().op == RtlOp::kReg) {
        single_dst = pat.SetDest().RegNo();
        const RtlExpr& src = pat.SetSrc();
        int b = -1;
        int64_t o = 0;
        if (ExtractBasePlusConst(src, &b, &o)) {
          derived = get_ptr_bases(b);
        } else if (src.op == RtlOp::kPlus && src.children.size() == 2 &&
                   src.children[0].op == RtlOp::kReg && src.children[1].op == RtlOp::kReg) {
          derived = get_ptr_bases(src.children[0].RegNo());
          for (const auto& k : get_ptr_bases(src.children[1].RegNo())) derived.insert(k);
        } else if (src.op == RtlOp::kMinus && src.children.size() == 2 &&
                   src.children[0].op == RtlOp::kReg) {
          derived = get_ptr_bases(src.children[0].RegNo());
        }
      }
      absl::btree_set<int> fdefs, pdefs;
      CollectFullRegDefs(pat, &fdefs, &pdefs);
      for (int r : fdefs) {
        ++reg_ver[r];
        ptr_bases.erase(r);
      }
      for (int r : pdefs) {
        ++reg_ver[r];
        ptr_bases.erase(r);
      }
      if (single_dst >= 0 && !derived.empty() && is_general_ptr_reg(single_dst)) {
        derived.insert({single_dst, reg_ver[single_dst]});
        ptr_bases[single_dst] = std::move(derived);
      }
    }
    UpdateSymRegOriginsForInsn(insn, mm, &origins);
  }

  std::vector<MemAliasGroup> groups;
  for (auto& [sym, edits] : sym_edits) {
    if (!edits.empty()) groups.push_back({MemAliasGroup::Kind::kReadOnlySymbol, std::move(edits)});
  }
  for (auto& [key, edits] : ptr_ro_reads) {
    bool tainted = ptr_written_vers.contains(key);
    if (!tainted) {
      if (const auto it = ptr_key_origins.find(key); it != ptr_key_origins.end()) {
        for (const auto& orig : it->second) {
          if (ptr_written_vers.contains(orig)) {
            tainted = true;
            break;
          }
        }
      }
    }
    if (!tainted && !edits.empty()) {
      groups.push_back({MemAliasGroup::Kind::kReadOnlyPtr, std::move(edits)});
    }
  }
  for (auto& [key, edits] : ptr_agg_edits) {
    if (!edits.empty()) {
      groups.push_back({MemAliasGroup::Kind::kAggregatePtrOffset, std::move(edits)});
    }
  }
  // Contiguous local stack-array runs per element mode (`expr.c:4534, 4889`).
  absl::btree_map<RtlMachineMode, std::vector<int64_t>> offsets_by_mode;
  for (const auto& [mo, _] : stack_accesses) offsets_by_mode[mo.first].push_back(mo.second);
  for (const auto& [mode, offs] : offsets_by_mode) {
    const int64_t sz = ModeByteSize(mode);
    if (sz <= 0 || offs.size() < 2) continue;
    for (size_t i = 0; i < offs.size();) {
      size_t j = i + 1;
      while (j < offs.size() && offs[j] == offs[j - 1] + sz) ++j;
      if (j - i >= 2) {
        bool has_write = false, has_read = false;
        std::vector<MemAliasFlagEdit> edits;
        for (size_t k = i; k < j; ++k) {
          for (const StackAccessInfo& acc : stack_accesses[{mode, offs[k]}]) {
            has_write = has_write || acc.is_write;
            has_read = has_read || !acc.is_write;
            if (!acc.already_in_struct) edits.push_back({acc.sched_idx, acc.is_write, true, false});
          }
        }
        if (has_write && has_read && !edits.empty()) {
          groups.push_back({MemAliasGroup::Kind::kLocalStackArray, std::move(edits)});
        }
      }
      i = j;
    }
  }
  return groups;
}

void ApplyMemAliasEditToBlock(const MemAliasFlagEdit& edit,
                              const PostReloadStackLayout& stack_layout, std::vector<Insn>* block,
                              absl::btree_set<int>* touched_out) {
  Insn* insn = &(*block)[static_cast<size_t>(edit.sched_idx)];
  auto mark_mem = [&](RtlExpr* mem) {
    if (mem->op != RtlOp::kMem || mem->flags.volatil) return;
    if (edit.set_in_struct) mem->flags.in_struct = true;
    if (edit.set_unchanging) mem->flags.unchanging = true;
  };
  auto handle_stmt = [&](RtlExpr* stmt) {
    if (stmt->op == RtlOp::kSet && stmt->children.size() == 2) {
      if (edit.is_write) {
        if (stmt->children[0].op == RtlOp::kMem) mark_mem(&stmt->children[0]);
      } else {
        ForEachMemInExprMut(&stmt->children[1], mark_mem);
      }
    } else if (!edit.is_write) {
      ForEachMemInExprMut(stmt, mark_mem);
    }
  };
  if (insn->pn.pattern.op == RtlOp::kParallel) {
    for (RtlExpr& ch : insn->pn.pattern.children) handle_stmt(&ch);
  } else {
    handle_stmt(&insn->pn.pattern);
  }
  if (touched_out != nullptr) touched_out->insert(edit.sched_idx);

  // Keep matching `REG_EQUIV (mem ...)` notes consistent with `SET_SRC`
  // (`local-alloc.c:1049-1055`, Rule B) or a subsequent store `SET_DEST`
  // (`local-alloc.c:990-1000`, Rule A, where `reload1.c:3375-3385`
  // `eliminate_regs_in_insn` leaves `$fp` in `REG_NOTES`).
  if (insn->pn.pattern.op != RtlOp::kSet || insn->pn.pattern.children.size() != 2) return;
  const RtlExpr& ref_mem = edit.is_write ? insn->pn.pattern.SetDest() : insn->pn.pattern.SetSrc();
  if (ref_mem.op != RtlOp::kMem || ref_mem.children.empty()) return;
  const int lo = edit.is_write ? 0 : edit.sched_idx;
  const int hi = edit.is_write ? edit.sched_idx : edit.sched_idx + 1;
  for (int k = lo; k < hi; ++k) {
    bool synced = false;
    for (RtlRegNote& note : (*block)[static_cast<size_t>(k)].pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegEquiv &&
          SameMemSlotIgnoringRegNames(note.datum, ref_mem, stack_layout) &&
          (note.datum.flags.in_struct != ref_mem.flags.in_struct ||
           note.datum.flags.unchanging != ref_mem.flags.unchanging)) {
        note.datum.flags.in_struct = ref_mem.flags.in_struct;
        note.datum.flags.unchanging = ref_mem.flags.unchanging;
        synced = true;
      }
    }
    if (synced && touched_out != nullptr) touched_out->insert(k);
  }
}

void SyncRegEquivMemFlagsFromUpdated(const std::vector<RtlRegNote>& upd_notes,
                                     std::vector<RtlRegNote>* target_notes) {
  for (RtlRegNote& tn : *target_notes) {
    if (tn.kind != RtlMachineMode::kRegEquiv || tn.datum.op != RtlOp::kMem ||
        tn.datum.children.empty()) {
      continue;
    }
    for (const RtlRegNote& un : upd_notes) {
      if (un.kind == RtlMachineMode::kRegEquiv && un.datum.op == RtlOp::kMem &&
          !un.datum.children.empty() && un.datum.mode == tn.datum.mode &&
          SameAddrIgnoringRegNames(un.datum.children[0], tn.datum.children[0])) {
        tn.datum.flags.in_struct = un.datum.flags.in_struct;
        tn.datum.flags.unchanging = un.datum.flags.unchanging;
        break;
      }
    }
  }
}

void CommitStage0AttributedMemAliasFlagsToBlock(
    const ParsedSchedRegion& base_region, const std::vector<Insn>& updated_base,
    const absl::btree_set<int>& touched_sched_idxs, Stage0BlockDelayView* block_view,
    RtlBasicBlock* orig_blk, std::optional<std::string>* epilogue_delay_pattern) {
  const size_t orig_len = orig_blk->instructions.size();
  std::vector<bool> saved_has_fusage(orig_len, false);
  std::vector<std::vector<RtlExpr>> saved_fusage(orig_len);
  for (size_t i = 0; i < orig_len && i < orig_blk->rtl_insns.size(); ++i) {
    if (orig_blk->rtl_insns[i].has_explicit_call_fusage) {
      saved_has_fusage[i] = true;
      saved_fusage[i] = orig_blk->rtl_insns[i].call_fusage;
    }
  }
  for (int si : touched_sched_idxs) {
    const Stage0OriginLoc& loc =
        block_view->view_to_orig[base_region.sched_orig_indices[static_cast<size_t>(si)]];
    if (loc.is_synthetic) continue;
    const RtlPatternAndNotes& upd = updated_base[static_cast<size_t>(si)].pn;
    if (loc.is_epilogue_delay) {
      if (epilogue_delay_pattern != nullptr && epilogue_delay_pattern->has_value()) {
        if (auto parsed = ParsePatternAndNotes(**epilogue_delay_pattern); parsed.ok()) {
          parsed->pattern = upd.pattern;
          SyncRegEquivMemFlagsFromUpdated(upd.reg_notes, &parsed->reg_notes);
          *epilogue_delay_pattern = parsed->ToGccPrintRtlString();
        }
      }
    } else if (loc.is_delay_slot) {
      if (auto it = block_view->seqs.find(loc.orig_insn_idx); it != block_view->seqs.end()) {
        it->second.slot_pn.pattern = upd.pattern;
        SyncRegEquivMemFlagsFromUpdated(upd.reg_notes, &it->second.slot_pn.reg_notes);
        orig_blk->instructions[loc.orig_insn_idx] =
            RtlExpr::MakeSequence(
                {
                    it->second.head_wrap,
                    RtlExpr::MakeInsnWrapper(it->second.slot_wrap.op, it->second.slot_wrap.flags,
                                             it->second.slot_pn),
                })
                .ToGccPrintRtlString();
      }
    } else if (auto parsed = ParsePatternAndNotes(orig_blk->instructions[loc.orig_insn_idx]);
               parsed.ok()) {
      parsed->pattern = upd.pattern;
      SyncRegEquivMemFlagsFromUpdated(upd.reg_notes, &parsed->reg_notes);
      orig_blk->instructions[loc.orig_insn_idx] = parsed->ToGccPrintRtlString();
    }
  }
  orig_blk->SyncRtlInsnsFromInstructions();
  for (size_t i = 0; i < orig_len && i < orig_blk->rtl_insns.size(); ++i) {
    if (saved_has_fusage[i] && !orig_blk->rtl_insns[i].has_explicit_call_fusage) {
      orig_blk->rtl_insns[i].has_explicit_call_fusage = true;
      orig_blk->rtl_insns[i].call_fusage = std::move(saved_fusage[i]);
    }
  }
}

}  // namespace

bool AttributeSched2ErasedMemAliasFlags(RtlControlFlowGraph* cfg,
                                        const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) return false;
  std::vector<std::string> effective_flags;
  effective_flags.reserve(profile.c_flags.size() + 1);
  effective_flags.push_back(absl::StrCat("-O", profile.optimize_level));
  effective_flags.insert(effective_flags.end(), profile.c_flags.begin(), profile.c_flags.end());
  const ParsedCompilerFlags flags = ParsedCompilerFlags::Parse(effective_flags);
  if (flags.opt_level <= 0 || !flags.RunsSched1() || !flags.RunsSched2()) return false;
  const MachineModel mm = MachineModel::FromProfile(profile);

  RtlControlFlowGraph view_cfg, live_cfg;
  std::vector<Stage0BlockDelayView> block_views;
  BuildStage0UnpackedDelayViewCfg(*cfg, profile, &view_cfg, &live_cfg, &block_views);
  AlignStage0DelaySlotsToSched2View(profile, mm, &view_cfg, &live_cfg, &block_views);
  ApplyStage0B0Jump2DeletedArgCopiesWithRollback(profile, mm, &view_cfg, &live_cfg, &block_views);
  const RegFacts facts_s2 = RegFacts::FromCfg(view_cfg, profile, Phase::kSched2);
  const CfgSched1Context cfg_ctx = ComputeCfgSched1Context(view_cfg, live_cfg, mm);
  const CfgReadOnlySymbolInfo ro_syms = CollectCfgReadOnlyStaticSymbols(view_cfg, mm);
  bool any_changed = false;

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    if (!block_views[b].eligible) continue;
    const RtlBasicBlock& vblk = view_cfg.rpo_blocks[b];
    const ParsedSchedRegion base_region = ParseBlockForSchedModel(vblk, b, Phase::kSched2);
    if (!base_region.valid || base_region.has_mid_block_loop_barrier ||
        base_region.sched_block.size() <= 2) {
      continue;
    }
    absl::btree_set<int> live_at_end_s2;
    for (int succ : vblk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    const PostReloadBodyBounds bounds = FindPostReloadBodyBounds(base_region.sched_block, b);
    const int body_len = bounds.epilogue_begin - bounds.prologue_end;
    if (body_len <= 1) continue;
    Sched1ProjectionContext proj_ctx = BuildSched1ProjectionContextForBlock(b, cfg_ctx);
    proj_ctx.excluded_prologue_body_indices =
        FindInterleavedPrologueSaveBodyIndices(base_region.sched_block, bounds, b, mm);

    const SymOriginMap empty_origins;
    const SymOriginMap& block_origins =
        (b < ro_syms.entry_sym_origins.size()) ? ro_syms.entry_sym_origins[b] : empty_origins;
    const std::vector<MemAliasGroup> raw_groups = CollectErasedMemAliasGroupsForBlock(
        base_region.sched_block, bounds, proj_ctx.excluded_prologue_body_indices,
        cfg_ctx.stack_layout, ro_syms, block_origins, mm);
    if (raw_groups.empty()) continue;

    auto compute_dep_sig = [&](const std::vector<Insn>& cand_block) -> std::vector<int> {
      std::vector<int> sig;
      const DepGraph s2 = Analyze(cand_block, facts_s2, mm, Phase::kSched2);
      for (const auto& links : s2.log_links) {
        for (const Link& l : links) {
          sig.push_back(l.target);
          sig.push_back(static_cast<int>(l.kind));
        }
        sig.push_back(-1);
      }
      std::vector<Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
        Insn insn = cand_block[static_cast<size_t>(i)];
        insn.luid = i - bounds.prologue_end;
        cand_body.push_back(std::move(insn));
      }
      const Sched1ProjectionView cand_view =
          ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, proj_ctx);
      const DepGraph s1 = Analyze(cand_view.core_insns, cand_view.facts_s1, mm, Phase::kSched1);
      sig.push_back(-2);
      for (const auto& links : s1.log_links) {
        for (const Link& l : links) {
          sig.push_back(l.target);
          sig.push_back(static_cast<int>(l.kind));
        }
        sig.push_back(-1);
      }
      sig.push_back(-3);
      for (size_t i = 0; i < cand_block.size(); ++i) {
        for (const RtlRegNote& note : cand_block[i].pn.reg_notes) {
          if (note.kind == RtlMachineMode::kRegEquiv && note.datum.op == RtlOp::kMem) {
            sig.push_back(static_cast<int>(i));
            sig.push_back((note.datum.flags.in_struct ? 1 : 0) |
                          (note.datum.flags.unchanging ? 2 : 0));
          }
        }
      }
      return sig;
    };

    auto apply_group_subset = [&](const std::vector<MemAliasGroup>& src_groups,
                                  const std::vector<bool>& active,
                                  absl::btree_set<int>* touched_out) -> std::vector<Insn> {
      std::vector<Insn> cand = base_region.sched_block;
      for (size_t g = 0; g < src_groups.size(); ++g) {
        if (!active[g]) continue;
        for (const MemAliasFlagEdit& edit : src_groups[g].edits) {
          ApplyMemAliasEditToBlock(edit, cfg_ctx.stack_layout, &cand, touched_out);
        }
      }
      return cand;
    };

    // Prune groups whose `/s`/`/u` flags do not change any `kSched2`/`kSched1`
    // dependence edge or `REG_EQUIV` note flag.
    const std::vector<int> base_sig = compute_dep_sig(base_region.sched_block);
    const std::vector<int> all_raw_sig =
        compute_dep_sig(apply_group_subset(raw_groups, std::vector<bool>(raw_groups.size(), true),
                                           /*touched_out=*/nullptr));
    std::vector<MemAliasGroup> groups;
    for (size_t g = 0; g < raw_groups.size(); ++g) {
      std::vector<bool> single(raw_groups.size(), false);
      single[g] = true;
      bool active = (compute_dep_sig(apply_group_subset(raw_groups, single, nullptr)) != base_sig);
      if (!active && raw_groups.size() > 1) {
        std::vector<bool> drop(raw_groups.size(), true);
        drop[g] = false;
        active = (compute_dep_sig(apply_group_subset(raw_groups, drop, nullptr)) != all_raw_sig);
      }
      if (active) groups.push_back(raw_groups[g]);
    }
    if (groups.empty()) continue;

    absl::btree_map<std::vector<int>, bool> verify_cache_fast;
    absl::btree_map<std::vector<int>, bool> verify_cache_tie;
    auto verify_candidate = [&](const std::vector<Insn>& cand_block, bool allow_tie_flips) -> bool {
      const std::vector<int> sig = compute_dep_sig(cand_block);
      auto& cache = allow_tie_flips ? verify_cache_tie : verify_cache_fast;
      if (const auto it = cache.find(sig); it != cache.end()) return it->second;
      if (allow_tie_flips) {
        if (const auto it = verify_cache_fast.find(sig);
            it != verify_cache_fast.end() && it->second) {
          return cache[sig] = true;
        }
      }
      if (!InvertBlockOrder(cand_block, facts_s2, mm, Phase::kSched2, live_at_end_s2).has_value()) {
        verify_cache_fast[sig] = false;
        verify_cache_tie[sig] = false;
        return false;
      }
      std::vector<Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int i = bounds.prologue_end; i < bounds.epilogue_begin; ++i) {
        Insn insn = cand_block[static_cast<size_t>(i)];
        insn.luid = i - bounds.prologue_end;
        cand_body.push_back(std::move(insn));
      }
      const Sched1ProjectionView cand_view =
          ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, proj_ctx);
      const bool ok = HasSched1AchievablePreimageForSched2Block(
          cand_block, b, bounds, cand_view, proj_ctx, cfg_ctx, facts_s2, mm, live_at_end_s2,
          /*allow_pure_reader_reg_dead_transfer=*/true, allow_tie_flips);
      cache[sig] = ok;
      if (ok && !allow_tie_flips) verify_cache_tie[sig] = true;
      return ok;
    };

    const bool base_s2_ok =
        InvertBlockOrder(base_region.sched_block, facts_s2, mm, Phase::kSched2, live_at_end_s2)
            .has_value();
    if (base_s2_ok && verify_candidate(base_region.sched_block, /*allow_tie_flips=*/false)) {
      continue;
    }
    // Blocks exceeding `sched.c:1740` (`pending_lists_length > 32`) can exceed
    // 48 body insns while still needing tie-flips (`sched.c:2428`) to verify
    // erased `/s`/`/u` alias flags once fast verification on the base block fails.
    const bool allow_tie_tier = !base_s2_ok || body_len <= 128;
    if (base_s2_ok && allow_tie_tier &&
        verify_candidate(base_region.sched_block, /*allow_tie_flips=*/true)) {
      continue;
    }

    auto build_with_mask = [&](const std::vector<bool>& active,
                               absl::btree_set<int>* touched_out) -> std::vector<Insn> {
      return apply_group_subset(groups, active, touched_out);
    };

    std::vector<std::vector<bool>> seed_masks;
    seed_masks.push_back(std::vector<bool>(groups.size(), true));
    std::vector<bool> no_ptr_ro(groups.size(), true), s_only(groups.size(), false),
        u_only(groups.size(), false);
    for (size_t g = 0; g < groups.size(); ++g) {
      const bool is_s = groups[g].kind == MemAliasGroup::Kind::kAggregatePtrOffset ||
                        groups[g].kind == MemAliasGroup::Kind::kLocalStackArray;
      s_only[g] = is_s;
      u_only[g] = !is_s;
      if (groups[g].kind == MemAliasGroup::Kind::kReadOnlyPtr) no_ptr_ro[g] = false;
    }
    seed_masks.push_back(no_ptr_ro);
    seed_masks.push_back(s_only);
    seed_masks.push_back(u_only);
    for (size_t g = 0; g < groups.size(); ++g) {
      if (groups[g].kind == MemAliasGroup::Kind::kReadOnlyPtr) {
        std::vector<bool> drop_one(groups.size(), true);
        drop_one[g] = false;
        seed_masks.push_back(std::move(drop_one));
        std::vector<bool> add_one = no_ptr_ro;
        add_one[g] = true;
        seed_masks.push_back(std::move(add_one));
      }
    }
    for (size_t g = 0; g < groups.size(); ++g) {
      std::vector<bool> single(groups.size(), false);
      single[g] = true;
      seed_masks.push_back(std::move(single));
    }

    std::vector<bool> chosen_mask;
    bool chosen_tie_flips = false;
    const std::vector<bool> tie_tiers =
        allow_tie_tier ? std::vector<bool>{false, true} : std::vector<bool>{false};
    for (bool tier : tie_tiers) {
      for (const std::vector<bool>& mask : seed_masks) {
        if (verify_candidate(build_with_mask(mask, /*touched_out=*/nullptr), tier)) {
          chosen_mask = mask;
          chosen_tie_flips = tier;
          break;
        }
      }
      if (!chosen_mask.empty()) break;
    }
    if (chosen_mask.empty()) continue;

    // Greedily minimize to the minimal subset of semantic alias groups needed.
    for (size_t g = 0; g < groups.size(); ++g) {
      if (!chosen_mask[g]) continue;
      chosen_mask[g] = false;
      if (!verify_candidate(build_with_mask(chosen_mask, /*touched_out=*/nullptr),
                            chosen_tie_flips)) {
        chosen_mask[g] = true;
      }
    }
    absl::btree_set<int> touched_sched_idxs;
    const std::vector<Insn> final_block = build_with_mask(chosen_mask, &touched_sched_idxs);
    if (!touched_sched_idxs.empty()) {
      CommitStage0AttributedMemAliasFlagsToBlock(base_region, final_block, touched_sched_idxs,
                                                 &block_views[b], &cfg->rpo_blocks[b],
                                                 &cfg->sideband.epilogue_delay_pattern);
      any_changed = true;
    }
  }
  return any_changed;
}

}  // namespace rom_nom_nom::lifter::sched_model
