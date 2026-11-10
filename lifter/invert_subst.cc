#include "lifter/invert_subst.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kFirstPseudoRegister = 68;

bool IsIntegralMode(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kQI:
    case RtlMachineMode::kHI:
    case RtlMachineMode::kSI:
    case RtlMachineMode::kDI:
      return true;
    default:
      return false;
  }
}

bool IsFloatMode(RtlMachineMode mode) {
  return mode == RtlMachineMode::kSF || mode == RtlMachineMode::kDF || mode == RtlMachineMode::kTF;
}

bool IsValidConditionCodeMode(const RtlExpr& expr) {
  return expr.mode == RtlMachineMode::kCC ||
         (expr.mode == RtlMachineMode::kUnknown &&
          (expr.raw_mode == "CC_FP" || expr.raw_mode == "CC_REV_FP"));
}

bool ModesMatch(const RtlExpr& a, const RtlExpr& b) {
  return a.mode == b.mode && (a.mode != RtlMachineMode::kUnknown || a.raw_mode == b.raw_mode);
}

int ModeByteSize(RtlMachineMode mode) {
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

int ModeBitSize(RtlMachineMode mode) {
  return ModeByteSize(mode) * 8;
}

RtlOp ReverseComparisonOp(RtlOp op) {
  switch (op) {
    case RtlOp::kEq:
      return RtlOp::kNe;
    case RtlOp::kNe:
      return RtlOp::kEq;
    case RtlOp::kLt:
      return RtlOp::kGe;
    case RtlOp::kGe:
      return RtlOp::kLt;
    case RtlOp::kLe:
      return RtlOp::kGt;
    case RtlOp::kGt:
      return RtlOp::kLe;
    case RtlOp::kLtu:
      return RtlOp::kGeu;
    case RtlOp::kGeu:
      return RtlOp::kLtu;
    case RtlOp::kLeu:
      return RtlOp::kGtu;
    case RtlOp::kGtu:
      return RtlOp::kLeu;
    default:
      return RtlOp::kUnknown;
  }
}

bool IsUnaryOp(RtlOp op) {
  switch (op) {
    case RtlOp::kNeg:
    case RtlOp::kNot:
    case RtlOp::kAbs:
    case RtlOp::kSqrt:
    case RtlOp::kFfs:
    case RtlOp::kSignExtend:
    case RtlOp::kZeroExtend:
    case RtlOp::kFloatExtend:
    case RtlOp::kFloatTruncate:
    case RtlOp::kTruncate:
    case RtlOp::kFix:
    case RtlOp::kFloat:
    case RtlOp::kUnsignedFix:
    case RtlOp::kUnsignedFloat:
      return true;
    default:
      return false;
  }
}

bool IsBinaryOp(RtlOp op) {
  switch (op) {
    case RtlOp::kPlus:
    case RtlOp::kMinus:
    case RtlOp::kMult:
    case RtlOp::kDiv:
    case RtlOp::kUdiv:
    case RtlOp::kMod:
    case RtlOp::kUmod:
    case RtlOp::kAnd:
    case RtlOp::kIor:
    case RtlOp::kXor:
    case RtlOp::kAshift:
    case RtlOp::kAshiftrt:
    case RtlOp::kLshiftrt:
    case RtlOp::kRotate:
    case RtlOp::kRotatert:
    case RtlOp::kSmin:
    case RtlOp::kSmax:
    case RtlOp::kUmin:
    case RtlOp::kUmax:
    case RtlOp::kLoSum:
      return true;
    default:
      return false;
  }
}

bool IsConstantExpr(const RtlExpr& expr) {
  switch (expr.op) {
    case RtlOp::kConstInt:
    case RtlOp::kConstDouble:
    case RtlOp::kConst:
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
    case RtlOp::kHigh:
      return true;
    default:
      return false;
  }
}

bool IsObjectOrSubregOfObject(const RtlExpr& expr) {
  switch (expr.op) {
    case RtlOp::kReg:
    case RtlOp::kMem:
    case RtlOp::kPc:
    case RtlOp::kCc0:
    case RtlOp::kScratch:
    case RtlOp::kConstInt:
    case RtlOp::kConstDouble:
    case RtlOp::kConst:
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
    case RtlOp::kLoSum:
      return true;
    case RtlOp::kSubreg:
      return !expr.children.empty() && IsObjectOrSubregOfObject(expr.children[0]);
    default:
      return false;
  }
}

bool ContainsOp(const RtlExpr& expr, RtlOp target) {
  if (expr.op == target) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (ContainsOp(child, target)) {
      return true;
    }
  }
  return false;
}

int MaxRegNoInExpr(const RtlExpr& expr) {
  int max_reg = -1;
  if (expr.IsReg()) {
    max_reg = std::max(max_reg, expr.RegNo());
  }
  for (const RtlExpr& child : expr.children) {
    max_reg = std::max(max_reg, MaxRegNoInExpr(child));
  }
  return max_reg;
}

RtlMachineMode EffectiveModeOrSi(const RtlExpr& expr,
                                 RtlMachineMode fallback = RtlMachineMode::kSI) {
  if (expr.mode != RtlMachineMode::kNone && expr.mode != RtlMachineMode::kVoid &&
      expr.mode != RtlMachineMode::kUnknown) {
    return expr.mode;
  }
  if (fallback != RtlMachineMode::kNone && fallback != RtlMachineMode::kVoid &&
      fallback != RtlMachineMode::kUnknown) {
    return fallback;
  }
  return RtlMachineMode::kSI;
}

RtlExpr MakeRegForSubst(const RtlExpr& expr, int regno,
                        RtlMachineMode fallback = RtlMachineMode::kSI) {
  if (expr.mode == RtlMachineMode::kUnknown && IsValidConditionCodeMode(expr)) {
    RtlExpr reg = RtlExpr::MakeReg(RtlMachineMode::kUnknown, regno);
    reg.raw_mode = expr.raw_mode;
    return reg;
  }
  return RtlExpr::MakeReg(EffectiveModeOrSi(expr, fallback), regno);
}

RtlExpr MakeSetWithMode(RtlExpr dst, RtlExpr rhs, RtlMachineMode set_mode) {
  RtlExpr expr = RtlExpr::MakeSet(std::move(dst), std::move(rhs));
  expr.mode = set_mode;
  return expr;
}

// Validates that an `RtlExpr` AST has well-formed opcodes, modes, and operand
// arities.
absl::Status ValidateRtlExpr(const RtlExpr& expr, bool allow_void_comparison = false) {
  if (expr.op == RtlOp::kUnknown || expr.op == RtlOp::kNil) {
    return absl::InvalidArgumentError("InvertSubst encountered unknown or nil RTL opcode");
  }
  if (expr.mode == RtlMachineMode::kUnknown && !IsValidConditionCodeMode(expr)) {
    return absl::InvalidArgumentError("InvertSubst encountered unknown RTL machine mode");
  }

  switch (expr.op) {
    case RtlOp::kReg:
      if (expr.RegNo() < 0 || expr.mode == RtlMachineMode::kNone) {
        return absl::InvalidArgumentError("InvertSubst encountered invalid (reg ...)");
      }
      break;
    case RtlOp::kSubreg:
      if (expr.children.size() != 1 || expr.int_val < 0 || expr.mode == RtlMachineMode::kNone) {
        return absl::InvalidArgumentError("InvertSubst encountered malformed (subreg ...)");
      }
      break;
    case RtlOp::kSet:
      if (expr.children.size() != 2) {
        return absl::InvalidArgumentError("InvertSubst encountered (set ...) with arity != 2");
      }
      break;
    case RtlOp::kMem:
      if (expr.children.size() != 1 || expr.mode == RtlMachineMode::kNone) {
        return absl::InvalidArgumentError("InvertSubst encountered malformed (mem ...)");
      }
      break;
    case RtlOp::kIfThenElse:
      if (expr.children.size() != 3) {
        return absl::InvalidArgumentError("InvertSubst encountered malformed (if_then_else ...)");
      }
      for (size_t i = 0; i < expr.children.size(); ++i) {
        if (absl::Status st = ValidateRtlExpr(expr.children[i], /*allow_void_comparison=*/i == 0);
            !st.ok()) {
          return st;
        }
      }
      return absl::OkStatus();
    default:
      if (IsUnaryOp(expr.op)) {
        if (expr.children.size() != 1 || expr.mode == RtlMachineMode::kNone) {
          return absl::InvalidArgumentError("InvertSubst encountered malformed unary RTL node");
        }
      } else if (IsBinaryOp(expr.op)) {
        if (expr.children.size() != 2 || expr.mode == RtlMachineMode::kNone) {
          return absl::InvalidArgumentError("InvertSubst encountered malformed binary RTL node");
        }
      } else if (expr.IsComparison()) {
        if (expr.children.size() != 2 ||
            (!allow_void_comparison && expr.mode == RtlMachineMode::kNone)) {
          return absl::InvalidArgumentError(
              "InvertSubst encountered malformed comparison RTL node");
        }
      }
      break;
  }

  for (const RtlExpr& child : expr.children) {
    if (absl::Status st = ValidateRtlExpr(child, /*allow_void_comparison=*/false); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

// Returns true if `comp` can be inverted into `(xor:M (rev_comp:M lhs rhs)
// (const_int 1))` such that both `subst(target, from, to, 0, 0)` and
// `subst(combined, pc_rtx, pc_rtx, 0, 0)` reduce it back to `comp` without
// changing the comparison opcode or immediate constant.
bool CanInvertComparisonWithXorOne(const RtlExpr& comp) {
  if (!comp.IsComparison() || !IsIntegralMode(comp.mode) || comp.children.size() != 2) {
    return false;
  }
  const RtlExpr& lhs = comp.children[0];
  const RtlExpr& rhs = comp.children[1];
  if (!lhs.IsReg() || IsFloatMode(lhs.mode) || IsFloatMode(rhs.mode)) {
    return false;
  }
  if (rhs.IsReg()) {
    return ReverseComparisonOp(comp.op) != RtlOp::kUnknown;
  }
  if (rhs.IsConstInt() && rhs.IntVal() == 0) {
    return comp.op == RtlOp::kEq || comp.op == RtlOp::kNe || comp.op == RtlOp::kLt ||
           comp.op == RtlOp::kLe || comp.op == RtlOp::kGt || comp.op == RtlOp::kGe;
  }
  return false;
}

// Returns true if `comp` is a fixed point of `combine.c:simplify_comparison`
// when its left-hand operand `lhs` is substituted for `from_reg`.
bool IsComparisonFixedPointUnderSubst(const RtlExpr& comp) {
  if (!comp.IsComparison() || comp.children.size() != 2) {
    return false;
  }
  const RtlExpr& lhs = comp.children[0];
  const RtlExpr& rhs = comp.children[1];
  if (!lhs.IsReg()) {
    return false;
  }
  if (rhs.IsReg()) {
    return true;
  }
  if (rhs.IsConstInt()) {
    const int64_t c = rhs.IntVal();
    if (c == 0) {
      return comp.op == RtlOp::kEq || comp.op == RtlOp::kNe || comp.op == RtlOp::kLt ||
             comp.op == RtlOp::kLe || comp.op == RtlOp::kGt || comp.op == RtlOp::kGe;
    }
    if (c > 0) {
      const int bits = ModeBitSize(EffectiveModeOrSi(lhs, comp.mode));
      const int64_t max_pos =
          bits > 0 && bits < 63 ? ((int64_t{1} << (bits - 1)) - 1) : 0x7fffffffLL;
      if (c >= max_pos) {
        return comp.op == RtlOp::kEq || comp.op == RtlOp::kNe;
      }
      return comp.op == RtlOp::kEq || comp.op == RtlOp::kNe || comp.op == RtlOp::kLe ||
             comp.op == RtlOp::kGt || comp.op == RtlOp::kLeu || comp.op == RtlOp::kGtu;
    }
    if (c < -1) {
      return comp.op == RtlOp::kEq || comp.op == RtlOp::kNe || comp.op == RtlOp::kLt ||
             comp.op == RtlOp::kGe;
    }
  }
  return false;
}

bool IsPowerOfTwo(int64_t v) {
  return v > 0 && (v & (v - 1)) == 0;
}

// Returns true if `mask` in `(and:M reg (const_int mask))` is preserved by
// `simplify_set` -> `make_compound_operation` (`combine.c:5093,5174`) without
// being rewritten into `zero_extend` or `zero_extract`.
bool IsSafeAndConstantMask(int64_t mask, RtlMachineMode mode) {
  if (mask == 0 || mask == -1 || mask == 0xff || mask == 0xffff || mask == 0xffffffffLL) {
    return false;
  }
  const int bits = ModeBitSize(mode);
  if (bits > 0 && bits < 64) {
    const int64_t mode_mask = (int64_t{1} << bits) - 1;
    if ((mask & mode_mask) == 0 || (mask & mode_mask) == mode_mask) {
      return false;
    }
    if ((mask & ~mode_mask) != 0 && (mask | mode_mask) != -1) {
      return false;
    }
  }
  return true;
}

// Returns true if `expr` is a compound binary subexpression that can be
// extracted into `from_reg` as the left-hand child of an outer binary
// operation without `simplify_rtx` or `make_compound_operation` rewriting the
// outer binary operation upon re-substitution.
bool IsExtractableCompoundChild(const RtlExpr& expr, RtlOp outer_op) {
  if (IsObjectOrSubregOfObject(expr)) {
    return false;
  }
  if (outer_op == RtlOp::kPlus || outer_op == RtlOp::kMinus) {
    if (ContainsOp(expr, RtlOp::kAshift) || expr.op == RtlOp::kPlus || expr.op == RtlOp::kMinus) {
      return false;
    }
    return expr.op == RtlOp::kMult && expr.children.size() == 2 && expr.children[0].IsReg() &&
           ((expr.children[1].IsConstInt() && expr.children[1].IntVal() > 1) ||
            expr.children[1].IsReg());
  }
  if (outer_op == RtlOp::kIor || outer_op == RtlOp::kXor) {
    if (expr.op == RtlOp::kAshift && expr.children.size() == 2 && expr.children[0].IsReg() &&
        expr.children[1].IsConstInt() && expr.children[1].IntVal() > 0 &&
        expr.children[1].IntVal() < ModeBitSize(expr.mode)) {
      return true;
    }
    return expr.op == RtlOp::kAnd && expr.children.size() == 2 && expr.children[0].IsReg() &&
           expr.children[1].IsConstInt() &&
           IsSafeAndConstantMask(expr.children[1].IntVal(), expr.mode);
  }
  return false;
}

// Returns true if `(op:M op0 op1)` can be decomposed by extracting `op0` into
// `from_reg = (reg:M_op0 fresh_regno)` such that `subst((set dst (op:M from_reg
// op1)), from_reg, op0, 0, 0)` reproduces `(set dst (op:M op0 op1))`.
bool CanExtractBinaryFirstOperand(const RtlExpr& dst, const RtlExpr& rhs) {
  if (!IsBinaryOp(rhs.op) || rhs.children.size() != 2) {
    return false;
  }
  if (!dst.IsReg() || dst.mode != rhs.mode) {
    return false;
  }
  const RtlExpr& op0 = rhs.children[0];
  const RtlExpr& op1 = rhs.children[1];

  if (rhs.op == RtlOp::kLoSum) {
    return op0.IsReg();
  }

  // Ensure commutative canonicalization in `combine.c:2975` will not swap `op0`
  // and `op1`.
  if (rhs.IsCommutative()) {
    if ((IsConstantExpr(op0) && !op1.IsConstInt()) ||
        (IsObjectOrSubregOfObject(op0) && !IsObjectOrSubregOfObject(op1))) {
      return false;
    }
  }

  const bool op0_ok = op0.IsReg() || IsExtractableCompoundChild(op0, rhs.op);
  if (!op0_ok) {
    return false;
  }

  switch (rhs.op) {
    case RtlOp::kPlus:
      if (ContainsOp(rhs, RtlOp::kAshift)) {
        return false;
      }
      return op1.IsReg() || (op1.IsConstInt() && op1.IntVal() != 0);
    case RtlOp::kMinus:
      if (ContainsOp(rhs, RtlOp::kAshift)) {
        return false;
      }
      return op1.IsReg() && op0 != op1;
    case RtlOp::kIor:
    case RtlOp::kXor:
      return (op1.IsReg() && op0 != op1) ||
             (op1.IsConstInt() && op1.IntVal() != 0 && op1.IntVal() != -1);
    case RtlOp::kAnd:
      if (!op0.IsReg()) {
        return false;
      }
      return (op1.IsReg() && op0 != op1) ||
             (op1.IsConstInt() && IsSafeAndConstantMask(op1.IntVal(), rhs.mode));
    case RtlOp::kAshift:
    case RtlOp::kAshiftrt:
    case RtlOp::kLshiftrt:
      if (!op0.IsReg()) {
        return false;
      }
      return op1.IsReg() ||
             (op1.IsConstInt() && op1.IntVal() > 0 && op1.IntVal() < ModeBitSize(rhs.mode));
    case RtlOp::kMult:
      if (!op0.IsReg()) {
        return false;
      }
      return op1.IsReg() || (op1.IsConstInt() && op1.IntVal() > 1 && !IsPowerOfTwo(op1.IntVal()));
    case RtlOp::kDiv:
    case RtlOp::kUdiv:
    case RtlOp::kMod:
    case RtlOp::kUmod:
    case RtlOp::kSmin:
    case RtlOp::kSmax:
    case RtlOp::kUmin:
    case RtlOp::kUmax:
      return op0.IsReg() && op1.IsReg() && op0 != op1;
    default:
      return false;
  }
}

}  // namespace

absl::StatusOr<SubstExprPreImage> InvertSubstExpr(const RtlExpr& combined_expr) {
  if (absl::Status st = ValidateRtlExpr(combined_expr); !st.ok()) {
    return st;
  }

  const int fresh_regno = std::max(kFirstPseudoRegister, MaxRegNoInExpr(combined_expr) + 1);

  if (combined_expr.IsSet()) {
    const RtlExpr& dst = combined_expr.SetDest();
    const RtlExpr& rhs = combined_expr.SetSrc();
    const RtlMachineMode set_mode = combined_expr.mode;

    // 1. Memory load `(set dst (mem:M addr))`: extract the address expression
    // `addr` into `from_reg` when `dst` is a register of matching mode and
    // `addr` has no `ashift` that `make_compound_operation` would rewrite into
    // `mult`.
    if (dst.IsReg() && rhs.IsMem() && dst.mode == rhs.mode) {
      const RtlExpr& addr = rhs.MemAddr();
      if (!addr.IsConstInt() && !addr.IsSubreg() && !ContainsOp(addr, RtlOp::kAshift)) {
        RtlExpr from_reg = RtlExpr::MakeReg(EffectiveModeOrSi(addr), fresh_regno);
        RtlExpr new_mem = rhs;
        new_mem.MutableMemAddr() = from_reg;
        return SubstExprPreImage{
            .target_expr = MakeSetWithMode(dst, std::move(new_mem), set_mode),
            .from_reg = std::move(from_reg),
            .to_expr = addr,
        };
      }
    }

    // 2. Extension and unary operations `(set dst (<unary>:M inner))`:
    //    - `(extend:M (mem:N addr))`: extract `addr` into `from_reg`.
    //    - `(extend:M (subreg:N sub_inner 0))`: extract `sub_inner` into
    //      `from_reg`.
    //    - `(<unary>:M (reg:N r))`: extract `(reg:N r)` into `from_reg`.
    if (dst.IsReg() && IsUnaryOp(rhs.op) && dst.mode == rhs.mode && !rhs.children.empty()) {
      const RtlExpr& inner = rhs.children[0];
      if ((rhs.op == RtlOp::kZeroExtend || rhs.op == RtlOp::kSignExtend ||
           rhs.op == RtlOp::kFloatExtend) &&
          inner.IsMem() &&
          (rhs.op == RtlOp::kFloatExtend ||
           (ModeByteSize(inner.mode) <= 4 && ModeByteSize(rhs.mode) > ModeByteSize(inner.mode)))) {
        const RtlExpr& addr = inner.MemAddr();
        if (!addr.IsConstInt() && !addr.IsSubreg() && !ContainsOp(addr, RtlOp::kAshift)) {
          RtlExpr from_reg = RtlExpr::MakeReg(EffectiveModeOrSi(addr), fresh_regno);
          RtlExpr new_rhs = rhs;
          new_rhs.children[0].MutableMemAddr() = from_reg;
          return SubstExprPreImage{
              .target_expr = MakeSetWithMode(dst, std::move(new_rhs), set_mode),
              .from_reg = std::move(from_reg),
              .to_expr = addr,
          };
        }
      }
      if ((rhs.op == RtlOp::kZeroExtend || rhs.op == RtlOp::kSignExtend) && inner.IsSubreg() &&
          inner.children.size() == 1 && inner.children[0].IsReg() && inner.int_val == 0 &&
          ModeByteSize(inner.children[0].mode) == 4 &&
          ModeByteSize(inner.mode) < ModeByteSize(inner.children[0].mode) &&
          ModeByteSize(rhs.mode) == 4) {
        const RtlExpr& sub_inner = inner.children[0];
        RtlExpr from_reg = RtlExpr::MakeReg(EffectiveModeOrSi(sub_inner), fresh_regno);
        RtlExpr new_rhs = rhs;
        new_rhs.children[0].children[0] = from_reg;
        return SubstExprPreImage{
            .target_expr = MakeSetWithMode(dst, std::move(new_rhs), set_mode),
            .from_reg = std::move(from_reg),
            .to_expr = sub_inner,
        };
      }
      if (inner.IsReg() && rhs.op != RtlOp::kZeroExtend && rhs.op != RtlOp::kSignExtend &&
          rhs.op != RtlOp::kAbs) {
        RtlExpr from_reg = RtlExpr::MakeReg(EffectiveModeOrSi(inner), fresh_regno);
        RtlExpr new_rhs = rhs;
        new_rhs.children[0] = from_reg;
        return SubstExprPreImage{
            .target_expr = MakeSetWithMode(dst, std::move(new_rhs), set_mode),
            .from_reg = std::move(from_reg),
            .to_expr = inner,
        };
      }
    }

    // 3. Subreg expression `(set dst (subreg:M inner word))`: extract `inner`
    // into `from_reg` when `inner` is a pseudo-register so `simplify_set` will
    // not fold a hard-register subreg via `alter_subreg`.
    if (rhs.IsSubreg() && rhs.children.size() == 1 && rhs.children[0].IsReg()) {
      const RtlExpr& sub_inner = rhs.children[0];
      const bool safe_subreg_set =
          dst.IsReg() && dst.mode == rhs.mode && sub_inner.RegNo() >= kFirstPseudoRegister;
      if (safe_subreg_set) {
        RtlExpr from_reg = RtlExpr::MakeReg(EffectiveModeOrSi(sub_inner), fresh_regno);
        RtlExpr new_rhs = rhs;
        new_rhs.children[0] = from_reg;
        return SubstExprPreImage{
            .target_expr = MakeSetWithMode(dst, std::move(new_rhs), set_mode),
            .from_reg = std::move(from_reg),
            .to_expr = sub_inner,
        };
      }
    }

    // 4. Relational comparison `(set dst (comp:M lhs_cmp rhs_cmp))`:
    //    - When eligible for `combine.c:4731` `XOR 1` condition reversal,
    //      produce `target = (set dst (xor:M from_reg (const_int 1)))` and
    //      `to = (rev_comp:M lhs_cmp rhs_cmp)`.
    //    - Otherwise, when `comp` is a fixed point of `simplify_comparison`,
    //      extract `lhs_cmp` into `from_reg`.
    if (dst.IsReg() && rhs.IsComparison() && ModesMatch(dst, rhs)) {
      if (CanInvertComparisonWithXorOne(rhs)) {
        RtlExpr from_reg = RtlExpr::MakeReg(rhs.mode, fresh_regno);
        RtlExpr target_rhs =
            RtlExpr::MakeBinary(RtlOp::kXor, rhs.mode, from_reg, RtlExpr::MakeConstInt(1));
        RtlExpr to_expr = RtlExpr::MakeBinary(ReverseComparisonOp(rhs.op), rhs.mode,
                                              rhs.children[0], rhs.children[1]);
        return SubstExprPreImage{
            .target_expr = MakeSetWithMode(dst, std::move(target_rhs), set_mode),
            .from_reg = std::move(from_reg),
            .to_expr = std::move(to_expr),
        };
      }
      if (IsComparisonFixedPointUnderSubst(rhs)) {
        const RtlExpr& lhs_cmp = rhs.children[0];
        RtlExpr from_reg = MakeRegForSubst(lhs_cmp, fresh_regno);
        RtlExpr new_rhs = rhs;
        new_rhs.children[0] = from_reg;
        return SubstExprPreImage{
            .target_expr = MakeSetWithMode(dst, std::move(new_rhs), set_mode),
            .from_reg = std::move(from_reg),
            .to_expr = lhs_cmp,
        };
      }
    }

    // 5. Conditional branch `(set (pc) (if_then_else (comp lhs rhs)
    // (label_ref ...) (pc)))`: extract `lhs` into `from_reg` when `(comp lhs
    // rhs)` is a fixed point of `simplify_comparison` and the true arm is
    // `(label_ref ...)`.
    if (dst.op == RtlOp::kPc && rhs.op == RtlOp::kIfThenElse && rhs.children.size() == 3) {
      const RtlExpr& cond = rhs.children[0];
      const RtlExpr& true_arm = rhs.children[1];
      const RtlExpr& false_arm = rhs.children[2];
      if (true_arm.IsLabelRef() && false_arm.op == RtlOp::kPc &&
          IsComparisonFixedPointUnderSubst(cond)) {
        const RtlExpr& cmp_lhs = cond.children[0];
        RtlExpr from_reg = MakeRegForSubst(cmp_lhs, fresh_regno);
        RtlExpr new_rhs = rhs;
        new_rhs.children[0].children[0] = from_reg;
        return SubstExprPreImage{
            .target_expr = MakeSetWithMode(dst, std::move(new_rhs), set_mode),
            .from_reg = std::move(from_reg),
            .to_expr = cmp_lhs,
        };
      }
    }

    // 6. Binary arithmetic, shift, and logical operations `(set dst (op:M op0
    // op1))`: extract `op0` (either a compound child subexpression or the left
    // register operand) into `from_reg`.
    if (CanExtractBinaryFirstOperand(dst, rhs)) {
      const RtlExpr& op0 = rhs.children[0];
      RtlExpr from_reg = MakeRegForSubst(op0, fresh_regno, rhs.mode);
      RtlExpr new_rhs = rhs;
      new_rhs.children[0] = from_reg;
      return SubstExprPreImage{
          .target_expr = MakeSetWithMode(dst, std::move(new_rhs), set_mode),
          .from_reg = std::move(from_reg),
          .to_expr = op0,
      };
    }

    // 7. General `(set dst rhs)` fallback: if `rhs` is a `SUBREG`, extract its
    // inner operand when `rhs` is not a subreg of a hard register; otherwise
    // substitute `rhs` directly at `SET_SRC`.
    if (rhs.IsSubreg() && rhs.children.size() == 1 && !rhs.children[0].IsSubreg() &&
        (!rhs.children[0].IsReg() || rhs.children[0].RegNo() >= kFirstPseudoRegister)) {
      const RtlExpr& sub_inner = rhs.children[0];
      RtlExpr from_reg = MakeRegForSubst(sub_inner, fresh_regno);
      RtlExpr new_rhs = rhs;
      new_rhs.children[0] = from_reg;
      return SubstExprPreImage{
          .target_expr = MakeSetWithMode(dst, std::move(new_rhs), set_mode),
          .from_reg = std::move(from_reg),
          .to_expr = sub_inner,
      };
    }

    RtlExpr from_reg = IsValidConditionCodeMode(dst)
                           ? MakeRegForSubst(dst, fresh_regno)
                           : MakeRegForSubst(rhs, fresh_regno, EffectiveModeOrSi(dst));
    return SubstExprPreImage{
        .target_expr = MakeSetWithMode(dst, from_reg, set_mode),
        .from_reg = std::move(from_reg),
        .to_expr = rhs,
    };
  }

  // Standalone (non-`SET`) expressions:
  if (combined_expr.IsMem()) {
    const RtlExpr& addr = combined_expr.MemAddr();
    if (!addr.IsConstInt() && !addr.IsSubreg()) {
      RtlExpr from_reg = RtlExpr::MakeReg(EffectiveModeOrSi(addr), fresh_regno);
      RtlExpr target = combined_expr;
      target.MutableMemAddr() = from_reg;
      return SubstExprPreImage{
          .target_expr = std::move(target),
          .from_reg = std::move(from_reg),
          .to_expr = addr,
      };
    }
  }

  if (CanInvertComparisonWithXorOne(combined_expr)) {
    RtlExpr from_reg = RtlExpr::MakeReg(combined_expr.mode, fresh_regno);
    RtlExpr target =
        RtlExpr::MakeBinary(RtlOp::kXor, combined_expr.mode, from_reg, RtlExpr::MakeConstInt(1));
    RtlExpr to_expr = RtlExpr::MakeBinary(ReverseComparisonOp(combined_expr.op), combined_expr.mode,
                                          combined_expr.children[0], combined_expr.children[1]);
    return SubstExprPreImage{
        .target_expr = std::move(target),
        .from_reg = std::move(from_reg),
        .to_expr = std::move(to_expr),
    };
  }

  if (IsBinaryOp(combined_expr.op) && combined_expr.children.size() == 2) {
    RtlExpr dummy_dst = RtlExpr::MakeReg(combined_expr.mode, fresh_regno + 1);
    if (CanExtractBinaryFirstOperand(dummy_dst, combined_expr)) {
      const RtlExpr& op0 = combined_expr.children[0];
      RtlExpr from_reg = RtlExpr::MakeReg(EffectiveModeOrSi(op0, combined_expr.mode), fresh_regno);
      RtlExpr target = combined_expr;
      target.children[0] = from_reg;
      return SubstExprPreImage{
          .target_expr = std::move(target),
          .from_reg = std::move(from_reg),
          .to_expr = op0,
      };
    }
  }

  RtlExpr from_reg = RtlExpr::MakeReg(EffectiveModeOrSi(combined_expr), fresh_regno);
  return SubstExprPreImage{
      .target_expr = from_reg,
      .from_reg = from_reg,
      .to_expr = combined_expr,
  };
}

SubstPreImage InvertSubst(std::string_view combined_rtx_x) {
  absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(combined_rtx_x);
  if (!parsed.ok()) {
    return SubstPreImage{};
  }
  absl::StatusOr<SubstExprPreImage> pre = InvertSubstExpr(parsed->pattern);
  if (!pre.ok()) {
    return SubstPreImage{};
  }
  return SubstPreImage{
      .target_expr = pre->target_expr.ToSexprString(),
      .from_reg = pre->from_reg.ToSexprString(),
      .to_expr = pre->to_expr.ToSexprString(),
  };
}

absl::StatusOr<RtlControlFlowGraph> InvertSubstCfg(const RtlControlFlowGraph& combine_cfg) {
  if (combine_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertSubstCfg requires at least one basic block");
  }
  if (combine_cfg.sideband.max_regno < 0) {
    return absl::InvalidArgumentError("InvertSubstCfg requires non-negative max_regno");
  }

  const size_t num_blocks = combine_cfg.rpo_blocks.size();
  for (const RtlBasicBlock& blk : combine_cfg.rpo_blocks) {
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(
            "InvertSubstCfg encountered out-of-range successor block id");
      }
    }
    for (size_t pred : blk.predecessors) {
      if (pred >= num_blocks) {
        return absl::InvalidArgumentError(
            "InvertSubstCfg encountered out-of-range predecessor block id");
      }
    }
  }

  RtlControlFlowGraph pre_cfg = combine_cfg;
  pre_cfg.pass = RtlPassKind::kCombine;

  for (RtlBasicBlock& blk : pre_cfg.rpo_blocks) {
    for (std::string& pat_str : blk.instructions) {
      absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(pat_str);
      if (!parsed.ok()) {
        return parsed.status();
      }
      if (absl::Status st = ValidateRtlExpr(parsed->pattern); !st.ok()) {
        return st;
      }
      if (parsed->pattern.IsSet()) {
        absl::StatusOr<SubstExprPreImage> spec = InvertSubstExpr(parsed->pattern);
        if (!spec.ok()) {
          return spec.status();
        }
        const RtlExpr& dst = parsed->pattern.SetDest();
        const RtlExpr& rhs = parsed->pattern.SetSrc();
        if (dst.IsReg() && CanInvertComparisonWithXorOne(rhs)) {
          RtlExpr combined_rhs = RtlExpr::MakeBinary(
              RtlOp::kXor, rhs.mode, std::move(spec->to_expr), RtlExpr::MakeConstInt(1));
          parsed->pattern.MutableSetSrc() = std::move(combined_rhs);
          pat_str = parsed->ToSexprString();
        }
      }
    }
  }

  pre_cfg.SyncAllBlocksPreservingMachineInsns(combine_cfg.rpo_blocks);
  return pre_cfg;
}

RtlControlFlowGraph InvertSubst(const RtlControlFlowGraph& combine_cfg) {
  absl::StatusOr<RtlControlFlowGraph> res = InvertSubstCfg(combine_cfg);
  if (!res.ok()) {
    return RtlControlFlowGraph{};
  }
  return *std::move(res);
}

}  // namespace rom_nom_nom::lifter
