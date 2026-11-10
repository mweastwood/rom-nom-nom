#include "lifter/invert_simplify_rtx.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kMipsZeroRegnum = 0;
constexpr int kStatusRegnum = 67;
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
  switch (mode) {
    case RtlMachineMode::kSF:
    case RtlMachineMode::kDF:
    case RtlMachineMode::kTF:
      return true;
    default:
      return false;
  }
}

int ModeBitSize(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kQI:
      return 8;
    case RtlMachineMode::kHI:
      return 16;
    case RtlMachineMode::kSI:
    case RtlMachineMode::kSF:
      return 32;
    case RtlMachineMode::kDI:
    case RtlMachineMode::kDF:
      return 64;
    default:
      return 0;
  }
}

uint64_t ModeMask(RtlMachineMode mode) {
  switch (mode) {
    case RtlMachineMode::kQI:
      return 0xffULL;
    case RtlMachineMode::kHI:
      return 0xffffULL;
    case RtlMachineMode::kSI:
      return 0xffffffffULL;
    case RtlMachineMode::kDI:
      return 0xffffffffffffffffULL;
    default:
      return 0ULL;
  }
}

RtlMachineMode EffectiveMode(const RtlExpr& expr) {
  if (expr.mode != RtlMachineMode::kNone && expr.mode != RtlMachineMode::kVoid) {
    return expr.mode;
  }
  return RtlMachineMode::kSI;
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

bool ContainsOp(const RtlExpr& expr, RtlOp target_op) {
  if (expr.op == target_op) {
    return true;
  }
  for (const RtlExpr& child : expr.children) {
    if (ContainsOp(child, target_op)) {
      return true;
    }
  }
  return false;
}

bool IsSimpleRegSubregOrMem(const RtlExpr& expr) {
  if (expr.IsReg() || expr.IsMem()) {
    return true;
  }
  if (expr.IsSubreg() && !expr.children.empty() && expr.children[0].IsReg()) {
    return true;
  }
  return false;
}

bool IsUnaryValueOp(RtlOp op) {
  switch (op) {
    case RtlOp::kNot:
    case RtlOp::kNeg:
    case RtlOp::kAbs:
    case RtlOp::kFfs:
    case RtlOp::kSqrt:
    case RtlOp::kHigh:
    case RtlOp::kSignExtend:
    case RtlOp::kZeroExtend:
    case RtlOp::kTruncate:
    case RtlOp::kFloatExtend:
    case RtlOp::kFloatTruncate:
    case RtlOp::kFloat:
    case RtlOp::kUnsignedFloat:
    case RtlOp::kFix:
    case RtlOp::kUnsignedFix:
    case RtlOp::kConst:
      return true;
    default:
      return false;
  }
}

bool IsBinaryValueOp(RtlOp op) {
  switch (op) {
    case RtlOp::kPlus:
    case RtlOp::kMinus:
    case RtlOp::kMult:
    case RtlOp::kDiv:
    case RtlOp::kUdiv:
    case RtlOp::kMod:
    case RtlOp::kUmod:
    case RtlOp::kSmin:
    case RtlOp::kSmax:
    case RtlOp::kUmin:
    case RtlOp::kUmax:
    case RtlOp::kAnd:
    case RtlOp::kIor:
    case RtlOp::kXor:
    case RtlOp::kAshift:
    case RtlOp::kAshiftrt:
    case RtlOp::kLshiftrt:
    case RtlOp::kRotate:
    case RtlOp::kRotatert:
    case RtlOp::kLoSum:
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

bool IsTernaryValueOp(RtlOp op) {
  switch (op) {
    case RtlOp::kIfThenElse:
    case RtlOp::kSignExtract:
    case RtlOp::kZeroExtract:
      return true;
    default:
      return false;
  }
}

bool IsTopLevelStatementOrContainerOp(RtlOp op) {
  switch (op) {
    case RtlOp::kNil:
    case RtlOp::kReturn:
    case RtlOp::kSet:
    case RtlOp::kParallel:
    case RtlOp::kSequence:
    case RtlOp::kUse:
    case RtlOp::kClobber:
    case RtlOp::kInsn:
    case RtlOp::kJumpInsn:
    case RtlOp::kCallInsn:
    case RtlOp::kCodeLabel:
    case RtlOp::kBarrier:
    case RtlOp::kNote:
    case RtlOp::kExprList:
    case RtlOp::kInsnList:
    case RtlOp::kAddrVec:
    case RtlOp::kAddrDiffVec:
      return true;
    default:
      return false;
  }
}

bool IsValidConditionCodeMode(const RtlExpr& expr) {
  return expr.mode == RtlMachineMode::kCC ||
         (expr.mode == RtlMachineMode::kUnknown &&
          (expr.raw_mode == "CC_FP" || expr.raw_mode == "CC_REV_FP"));
}

absl::Status ValidateExprNodeRecursively(const RtlExpr& expr) {
  if (expr.op == RtlOp::kUnknown) {
    return absl::InvalidArgumentError(
        absl::StrCat("InvertSimplifyRtxExpr: unknown RTL opcode '", expr.raw_op, "'"));
  }
  if (expr.mode == RtlMachineMode::kUnknown && !IsValidConditionCodeMode(expr)) {
    return absl::InvalidArgumentError(
        absl::StrCat("InvertSimplifyRtxExpr: unknown RTL mode '", expr.raw_mode, "'"));
  }
  if (expr.IsReg()) {
    if (expr.RegNo() < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertSimplifyRtxExpr: negative register number ", expr.RegNo()));
    }
    if (expr.mode == RtlMachineMode::kNone || expr.mode == RtlMachineMode::kVoid) {
      return absl::InvalidArgumentError("InvertSimplifyRtxExpr: reg expression missing mode");
    }
    if (!expr.children.empty()) {
      return absl::InvalidArgumentError("InvertSimplifyRtxExpr: reg expression has child nodes");
    }
  } else if (expr.IsConstInt()) {
    if (!expr.children.empty()) {
      return absl::InvalidArgumentError(
          "InvertSimplifyRtxExpr: const_int expression has child nodes");
    }
  } else if (expr.IsSubreg()) {
    if (expr.children.size() != 1 || expr.int_val < 0 || expr.mode == RtlMachineMode::kNone) {
      return absl::InvalidArgumentError("InvertSimplifyRtxExpr: malformed subreg expression");
    }
  } else if (expr.op == RtlOp::kMem) {
    if (expr.children.size() != 1 || expr.mode == RtlMachineMode::kNone) {
      return absl::InvalidArgumentError("InvertSimplifyRtxExpr: malformed mem expression");
    }
  } else if (expr.IsSymbolRef()) {
    if (expr.SymbolName().empty()) {
      return absl::InvalidArgumentError("InvertSimplifyRtxExpr: empty symbol_ref name");
    }
  } else if (IsUnaryValueOp(expr.op)) {
    if (expr.children.size() != 1 || expr.mode == RtlMachineMode::kNone) {
      return absl::InvalidArgumentError("InvertSimplifyRtxExpr: malformed unary expression");
    }
  } else if (IsBinaryValueOp(expr.op)) {
    if (expr.children.size() != 2 || expr.mode == RtlMachineMode::kNone) {
      return absl::InvalidArgumentError("InvertSimplifyRtxExpr: malformed binary expression");
    }
  } else if (IsTernaryValueOp(expr.op)) {
    if (expr.children.size() != 3) {
      return absl::InvalidArgumentError("InvertSimplifyRtxExpr: malformed ternary expression");
    }
  }
  for (const RtlExpr& child : expr.children) {
    absl::Status child_status = ValidateExprNodeRecursively(child);
    if (!child_status.ok()) {
      return child_status;
    }
  }
  return absl::OkStatus();
}

absl::Status ValidateSideband(const RtlSidebandState& sideband) {
  for (const auto& [pseudo, hard] : sideband.reg_renumber) {
    if (pseudo < kStatusRegnum) {
      return absl::InvalidArgumentError(absl::StrCat(
          "InvertSimplifyRtxCfg: invalid reg_renumber key ", pseudo, " (< ", kStatusRegnum, ")"));
    }
    if (hard < -1 || hard >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(absl::StrCat(
          "InvertSimplifyRtxCfg: invalid reg_renumber target ", hard, " for pseudo ", pseudo));
    }
  }
  for (const auto& [regno, refs] : sideband.reg_n_refs) {
    (void)refs;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertSimplifyRtxCfg: negative regno in reg_n_refs: ", regno));
    }
  }
  for (const auto& [regno, sets] : sideband.reg_n_sets) {
    (void)sets;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertSimplifyRtxCfg: negative regno in reg_n_sets: ", regno));
    }
  }
  for (const auto& [regno, deaths] : sideband.reg_n_deaths) {
    (void)deaths;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertSimplifyRtxCfg: negative regno in reg_n_deaths: ", regno));
    }
  }
  for (const auto& [regno, len] : sideband.reg_live_length) {
    (void)len;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertSimplifyRtxCfg: negative regno in reg_live_length: ", regno));
    }
  }
  for (const auto& [regno, calls] : sideband.reg_n_calls_crossed) {
    (void)calls;
    if (regno < 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertSimplifyRtxCfg: negative regno in reg_n_calls_crossed: ", regno));
    }
  }
  for (const auto& [bb_idx, live_regs] : sideband.basic_block_live_at_start) {
    if (bb_idx < 0) {
      return absl::InvalidArgumentError(absl::StrCat(
          "InvertSimplifyRtxCfg: negative basic_block_live_at_start block index ", bb_idx));
    }
    for (int regno : live_regs) {
      if (regno < 0) {
        return absl::InvalidArgumentError(absl::StrCat(
            "InvertSimplifyRtxCfg: negative regno in basic_block_live_at_start: ", regno));
      }
    }
  }
  for (int regno : sideband.saved_gpr_regnos) {
    if (regno < 0 || regno >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertSimplifyRtxCfg: invalid saved_gpr_regno ", regno));
    }
  }
  for (int regno : sideband.saved_fpr_regnos) {
    if (regno < 0 || regno >= kFirstPseudoRegister) {
      return absl::InvalidArgumentError(
          absl::StrCat("InvertSimplifyRtxCfg: invalid saved_fpr_regno ", regno));
    }
  }
  return absl::OkStatus();
}

bool IsCompoundSetRhsForSimplifyRtx(const RtlExpr& dst, const RtlExpr& rhs) {
  if (dst.op == RtlOp::kPc) {
    return false;
  }
  switch (rhs.op) {
    case RtlOp::kNil:
    case RtlOp::kPc:
    case RtlOp::kCc0:
    case RtlOp::kReg:
    case RtlOp::kSubreg:
    case RtlOp::kConstInt:
    case RtlOp::kMem:
    case RtlOp::kClobber:
    case RtlOp::kCall:
    case RtlOp::kIfThenElse:
    case RtlOp::kLabelRef:
      return false;
    default:
      return true;
  }
}

}  // namespace

absl::StatusOr<RtlExpr> InvertSimplifyRtxExpr(const RtlExpr& simplified_expr) {
  if (IsTopLevelStatementOrContainerOp(simplified_expr.op)) {
    return absl::InvalidArgumentError(
        "InvertSimplifyRtxExpr: expected value expression rather than top-level statement");
  }
  absl::Status validation_status = ValidateExprNodeRecursively(simplified_expr);
  if (!validation_status.ok()) {
    return validation_status;
  }

  const RtlMachineMode mode = simplified_expr.mode;

  // 1. Relational comparisons `(comp:M lhs rhs)`:
  //    - For non-floating-point operands in integral result mode `M`, invert
  //      `combine.c:4731-4735` (`simplify_logical` on `XOR` with `const_true_rtx`):
  //      `(xor:M (rev_comp:M lhs rhs) (const_int 1)) -> (comp:M lhs rhs)`.
  //    - For floating-point operands in integral result mode `M` (where
  //      `reversible_comparison_p` is false under IEEE rules), invert
  //      `combine.c:3932-3933` (`simplify_if_then_else` truth-value store):
  //      `(if_then_else:M (comp:M lhs rhs) (const_int 1) (const_int 0)) -> (comp:M lhs rhs)`.
  if (simplified_expr.IsComparison() && simplified_expr.children.size() == 2 &&
      IsIntegralMode(mode)) {
    const RtlExpr& lhs = simplified_expr.children[0];
    const RtlExpr& rhs = simplified_expr.children[1];
    if (!IsFloatMode(lhs.mode) && !IsFloatMode(rhs.mode)) {
      const RtlOp rev_op = ReverseComparisonOp(simplified_expr.op);
      if (rev_op != RtlOp::kUnknown) {
        RtlExpr rev_comp = RtlExpr::MakeBinary(rev_op, mode, lhs, rhs);
        return RtlExpr::MakeBinary(RtlOp::kXor, mode, std::move(rev_comp),
                                   RtlExpr::MakeConstInt(1));
      }
    } else if (!lhs.IsConstInt() || !rhs.IsConstInt()) {
      return RtlExpr::MakeIfThenElse(simplified_expr, RtlExpr::MakeConstInt(1),
                                     RtlExpr::MakeConstInt(0), mode);
    }
  }

  // 2. Bitwise complement `(not:M inner)`:
  //    Invert `combine.c:3351-3353` (`case NEG:`):
  //    `(neg:M (plus:M inner (const_int 1))) -> (not:M inner)`.
  if (simplified_expr.op == RtlOp::kNot && simplified_expr.children.size() == 1 &&
      IsIntegralMode(mode)) {
    const RtlExpr& inner = simplified_expr.children[0];
    if (IsSimpleRegSubregOrMem(inner) && inner.mode == mode) {
      RtlExpr plus_one = RtlExpr::MakeBinary(RtlOp::kPlus, mode, inner, RtlExpr::MakeConstInt(1));
      return RtlExpr::MakeUnary(RtlOp::kNeg, mode, std::move(plus_one));
    }
  }

  // 3. Arithmetic negation `(neg:M inner)`:
  //    Invert `combine.c:3313-3315` (`case NOT:`):
  //    `(not:M (plus:M inner (const_int -1))) -> (neg:M inner)`.
  if (simplified_expr.op == RtlOp::kNeg && simplified_expr.children.size() == 1 &&
      IsIntegralMode(mode)) {
    const RtlExpr& inner = simplified_expr.children[0];
    if (IsSimpleRegSubregOrMem(inner) && inner.mode == mode) {
      RtlExpr plus_m1 = RtlExpr::MakeBinary(RtlOp::kPlus, mode, inner, RtlExpr::MakeConstInt(-1));
      return RtlExpr::MakeUnary(RtlOp::kNot, mode, std::move(plus_m1));
    }
  }

  // 4. Bitwise AND `(and:M lhs rhs)`:
  if (simplified_expr.op == RtlOp::kAnd && simplified_expr.children.size() == 2 &&
      IsIntegralMode(mode)) {
    const RtlExpr& lhs = simplified_expr.children[0];
    const RtlExpr& rhs = simplified_expr.children[1];
    // 4a. Associative constant mask (`combine.c:3143-3183`):
    //     `(and:M (and:M a (const_int c)) (const_int c)) -> (and:M a (const_int c))`.
    const uint64_t mask = ModeMask(mode);
    if (lhs.IsReg() && lhs.mode == mode && rhs.IsConstInt() && rhs.IntVal() != 0 &&
        (static_cast<uint64_t>(rhs.IntVal()) & mask) != mask) {
      return RtlExpr::MakeBinary(RtlOp::kAnd, mode, simplified_expr, rhs);
    }
    // 4b. Non-constant `AND` (including `(and:M a b)`, `(and:M (not:M a) b)`,
    //     and `(and:M (not:M a) (not:M b))`) via `IOR` identity
    //     (`cse.c:simplify_binary_operation`), preserving operand order:
    //     `(ior:M (and:M lhs rhs) (const_int 0)) -> (and:M lhs rhs)`.
    const bool lhs_ok = (lhs.IsReg() && lhs.mode == mode) ||
                        (lhs.op == RtlOp::kNot && lhs.children.size() == 1 &&
                         lhs.children[0].IsReg() && lhs.children[0].mode == mode);
    const bool rhs_ok = (rhs.IsReg() && rhs.mode == mode) ||
                        (rhs.op == RtlOp::kNot && rhs.children.size() == 1 &&
                         rhs.children[0].IsReg() && rhs.children[0].mode == mode);
    if (lhs_ok && rhs_ok && !(lhs.IsReg() && rhs.IsReg() && lhs.RegNo() == rhs.RegNo())) {
      return RtlExpr::MakeBinary(RtlOp::kIor, mode, simplified_expr, RtlExpr::MakeConstInt(0));
    }
  }

  // 5. Bitwise IOR `(ior:M lhs rhs)`:
  if (simplified_expr.op == RtlOp::kIor && simplified_expr.children.size() == 2 &&
      IsIntegralMode(mode)) {
    const RtlExpr& lhs = simplified_expr.children[0];
    const RtlExpr& rhs = simplified_expr.children[1];
    const uint64_t mask = ModeMask(mode);
    if (lhs.IsReg() && lhs.mode == mode &&
        ((rhs.IsReg() && rhs.mode == mode && lhs.RegNo() != rhs.RegNo()) ||
         (rhs.IsConstInt() && rhs.IntVal() != 0 &&
          (static_cast<uint64_t>(rhs.IntVal()) & mask) != mask))) {
      return RtlExpr::MakeBinary(RtlOp::kIor, mode, simplified_expr, RtlExpr::MakeConstInt(0));
    }
  }

  // 6. Bitwise XOR `(xor:M lhs rhs)`:
  if (simplified_expr.op == RtlOp::kXor && simplified_expr.children.size() == 2 &&
      IsIntegralMode(mode)) {
    const RtlExpr& lhs = simplified_expr.children[0];
    const RtlExpr& rhs = simplified_expr.children[1];
    // 6a. `simplify_logical` (`combine.c:4690-4693`):
    //     `(xor:M (not:M a) (not:M b)) -> (xor:M a b)`.
    if (lhs.IsReg() && rhs.IsReg() && lhs.mode == mode && rhs.mode == mode &&
        lhs.RegNo() != rhs.RegNo()) {
      RtlExpr not_lhs = RtlExpr::MakeUnary(RtlOp::kNot, mode, lhs);
      RtlExpr not_rhs = RtlExpr::MakeUnary(RtlOp::kNot, mode, rhs);
      return RtlExpr::MakeBinary(RtlOp::kXor, mode, std::move(not_lhs), std::move(not_rhs));
    }
    // 6b. Constant XOR via identity `XOR` (`cse.c:simplify_binary_operation`):
    const uint64_t mask = ModeMask(mode);
    if (lhs.IsReg() && lhs.mode == mode && rhs.IsConstInt() && rhs.IntVal() != 0 &&
        (static_cast<uint64_t>(rhs.IntVal()) & mask) != mask) {
      return RtlExpr::MakeBinary(RtlOp::kXor, mode, simplified_expr, RtlExpr::MakeConstInt(0));
    }
  }

  // 7. Addition `(plus:M lhs rhs)`:
  if (simplified_expr.op == RtlOp::kPlus && simplified_expr.children.size() == 2 &&
      IsIntegralMode(mode)) {
    const RtlExpr& lhs = simplified_expr.children[0];
    const RtlExpr& rhs = simplified_expr.children[1];
    // 7a. `combine.c:3318-3320` (`case NOT:`):
    //     `(not:M (neg:M a)) -> (plus:M a (const_int -1))`.
    if (IsSimpleRegSubregOrMem(lhs) && lhs.mode == mode && rhs.IsConstInt() && rhs.IntVal() == -1) {
      RtlExpr neg_lhs = RtlExpr::MakeUnary(RtlOp::kNeg, mode, lhs);
      return RtlExpr::MakeUnary(RtlOp::kNot, mode, std::move(neg_lhs));
    }
    // 7b. `combine.c:3355-3357` (`case NEG:`):
    //     `(neg:M (not:M a)) -> (plus:M a (const_int 1))`.
    if (IsSimpleRegSubregOrMem(lhs) && lhs.mode == mode && rhs.IsConstInt() && rhs.IntVal() == 1) {
      RtlExpr not_lhs = RtlExpr::MakeUnary(RtlOp::kNot, mode, lhs);
      return RtlExpr::MakeUnary(RtlOp::kNeg, mode, std::move(not_lhs));
    }
    // 7c. Reassociation of indexed address (`combine.c:3557-3562`):
    //     `(plus:M (plus:M a (const_int c)) b) -> (plus:M (plus:M a b) (const_int c))`.
    if (lhs.op == RtlOp::kPlus && lhs.children.size() == 2 && lhs.children[0].IsReg() &&
        lhs.children[1].IsReg() && lhs.children[0].mode == mode && lhs.children[1].mode == mode &&
        rhs.IsConstInt() && (rhs.IntVal() & ~int64_t{7}) != 0) {
      RtlExpr inner_plus = RtlExpr::MakeBinary(RtlOp::kPlus, mode, lhs.children[0], rhs);
      return RtlExpr::MakeBinary(RtlOp::kPlus, mode, std::move(inner_plus), lhs.children[1]);
    }
    // 7d. Register-register or register-immediate addition where `nonzero_bits`
    //     have a guaranteed overlapping bit (even when `lhs` is stack-aligned):
    if (lhs.IsReg() && lhs.mode == mode &&
        ((rhs.IsReg() && rhs.mode == mode) ||
         (rhs.IsConstInt() && (rhs.IntVal() & ~int64_t{7}) != 0))) {
      return RtlExpr::MakeBinary(RtlOp::kPlus, mode, simplified_expr, RtlExpr::MakeConstInt(0));
    }
  }

  // 8. Subtraction `(minus:M lhs rhs)`:
  //    Invert `cse.c:3649` (`simplify_binary_operation` additive identity
  //    `(plus:M (minus:M lhs rhs) (const_int 0)) -> (minus:M lhs rhs)`),
  //    preserving the operand order `(lhs, rhs)` so flow death-note ordering is
  //    invariant.
  if (simplified_expr.op == RtlOp::kMinus && simplified_expr.children.size() == 2 &&
      IsIntegralMode(mode)) {
    const RtlExpr& lhs = simplified_expr.children[0];
    const RtlExpr& rhs = simplified_expr.children[1];
    if (lhs.IsReg() && rhs.IsReg() && lhs.mode == mode && rhs.mode == mode &&
        lhs.RegNo() != rhs.RegNo()) {
      return RtlExpr::MakeBinary(RtlOp::kPlus, mode, simplified_expr, RtlExpr::MakeConstInt(0));
    }
  }

  // 9. Constant shifts (`combine.c:7960-7970` `simplify_shift_const`):
  //    `(shift:M (shift:M a (const_int 1)) (const_int c - 1)) -> (shift:M a (const_int c))`.
  if ((simplified_expr.op == RtlOp::kAshift || simplified_expr.op == RtlOp::kAshiftrt ||
       simplified_expr.op == RtlOp::kLshiftrt) &&
      simplified_expr.children.size() == 2 && IsIntegralMode(mode)) {
    const RtlExpr& lhs = simplified_expr.children[0];
    const RtlExpr& rhs = simplified_expr.children[1];
    const int bit_size = ModeBitSize(mode);
    if (lhs.IsReg() && lhs.mode == mode && rhs.IsConstInt() && rhs.IntVal() > 1 &&
        rhs.IntVal() < bit_size) {
      RtlExpr inner_shift =
          RtlExpr::MakeBinary(simplified_expr.op, mode, lhs, RtlExpr::MakeConstInt(1));
      return RtlExpr::MakeBinary(simplified_expr.op, mode, std::move(inner_shift),
                                 RtlExpr::MakeConstInt(rhs.IntVal() - 1));
    }
  }

  // 10. Condition-code comparisons (`CC`, `CC_FP`, `CC_REV_FP`), symbolic
  //     constants (`symbol_ref`, `const`, `high`, `lo_sum`), and single-word
  //     integer atomic / multiplicative expressions that simplify via
  //     `cse.c:3649` (`simplify_binary_operation`:
  //     `(plus:SI expr (const_int 0)) -> expr`):
  if (IsValidConditionCodeMode(simplified_expr) ||
      ((mode == RtlMachineMode::kSI || simplified_expr.IsConstInt()) &&
       (simplified_expr.IsReg() || simplified_expr.IsConstInt() || simplified_expr.IsSymbolRef() ||
        simplified_expr.op == RtlOp::kConst || simplified_expr.op == RtlOp::kHigh ||
        (simplified_expr.op == RtlOp::kLoSum && simplified_expr.children.size() == 2 &&
         simplified_expr.children[0].IsReg()) ||
        ((simplified_expr.op == RtlOp::kMult || simplified_expr.op == RtlOp::kDiv ||
          simplified_expr.op == RtlOp::kUdiv || simplified_expr.op == RtlOp::kMod ||
          simplified_expr.op == RtlOp::kUmod) &&
         simplified_expr.children.size() == 2 && simplified_expr.children[0].IsReg() &&
         simplified_expr.children[1].IsReg())))) {
    return RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSI, simplified_expr,
                               RtlExpr::MakeConstInt(0));
  }

  // 12. General fallback for extensions (`zero_extend`, `sign_extend`,
  //     `zero_extract`, `sign_extract`), `subreg`, floating-point modes, multi-word
  //     modes, and compound expressions:
  //     - When `simplified_expr` contains `const_double` or `scratch` (for which
  //       `rtlanal.c:rtx_equal_p` returns 0 on distinct allocations), invert
  //       `cse.c:simplify_ternary_operation` (`combine.c:3124`, `cse.c:4743`):
  //       `(if_then_else:M (const_int 1) expr (const_int 0)) -> expr`.
  //     - Otherwise, invert `combine.c:4025-4026` (`simplify_if_then_else`
  //       identical-arms reduction with a side-effect-free non-comparison condition):
  //       `(if_then_else:M (reg:SI 0 $0) expr expr) -> expr`.
  const RtlMachineMode out_mode = EffectiveMode(simplified_expr);
  if (ContainsOp(simplified_expr, RtlOp::kConstDouble) ||
      ContainsOp(simplified_expr, RtlOp::kScratch)) {
    return RtlExpr::MakeIfThenElse(RtlExpr::MakeConstInt(1), simplified_expr,
                                   RtlExpr::MakeConstInt(0), out_mode);
  }
  return RtlExpr::MakeIfThenElse(RtlExpr::MakeReg(RtlMachineMode::kSI, kMipsZeroRegnum),
                                 simplified_expr, simplified_expr, out_mode);
}

std::string InvertSimplifyRtx(std::string_view simplified_rtx_x) {
  auto parsed_or = RtlExpr::Parse(simplified_rtx_x);
  if (!parsed_or.ok()) {
    return "";
  }
  auto unsimplified_or = InvertSimplifyRtxExpr(*parsed_or);
  if (!unsimplified_or.ok()) {
    return "";
  }
  return unsimplified_or->ToSexprString();
}

absl::StatusOr<RtlControlFlowGraph> InvertSimplifyRtxCfg(const RtlControlFlowGraph& combine_cfg) {
  if (combine_cfg.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertSimplifyRtxCfg: empty rpo_blocks in CFG");
  }
  absl::Status sideband_status = ValidateSideband(combine_cfg.sideband);
  if (!sideband_status.ok()) {
    return sideband_status;
  }

  const size_t num_blocks = combine_cfg.rpo_blocks.size();
  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = combine_cfg.rpo_blocks[b];
    for (size_t succ : blk.successors) {
      if (succ >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrCat("InvertSimplifyRtxCfg: block ", b,
                                                       " has out-of-bounds successor ", succ,
                                                       " (num_blocks=", num_blocks, ")"));
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= num_blocks) {
        return absl::InvalidArgumentError(absl::StrCat("InvertSimplifyRtxCfg: block ", b,
                                                       " has out-of-bounds addr_vec_target ",
                                                       target, " (num_blocks=", num_blocks, ")"));
      }
    }
  }

  RtlControlFlowGraph out = combine_cfg;
  out.pass = RtlPassKind::kFlow;

  for (RtlBasicBlock& blk : out.rpo_blocks) {
    for (std::string& pat_str : blk.instructions) {
      auto parsed_or = ParsePatternAndNotes(pat_str);
      if (!parsed_or.ok()) {
        return parsed_or.status();
      }
      if (ContainsOp(parsed_or->pattern, RtlOp::kUnknown)) {
        return absl::InvalidArgumentError(
            absl::StrCat("InvertSimplifyRtxCfg: unknown opcode in instruction '", pat_str, "'"));
      }
      bool has_negative_reg = false;
      parsed_or->pattern.Visit([&has_negative_reg](const RtlExpr& node) {
        if (node.IsReg() && node.RegNo() < 0) {
          has_negative_reg = true;
        }
      });
      if (has_negative_reg) {
        return absl::InvalidArgumentError(absl::StrCat(
            "InvertSimplifyRtxCfg: negative register number in instruction '", pat_str, "'"));
      }
      for (const RtlRegNote& note : parsed_or->reg_notes) {
        if (note.kind == RtlMachineMode::kUnknown || ContainsOp(note.datum, RtlOp::kUnknown)) {
          return absl::InvalidArgumentError(
              absl::StrCat("InvertSimplifyRtxCfg: unknown note in instruction '", pat_str, "'"));
        }
        bool note_neg_reg = false;
        note.datum.Visit([&note_neg_reg](const RtlExpr& node) {
          if (node.IsReg() && node.RegNo() < 0) {
            note_neg_reg = true;
          }
        });
        if (note_neg_reg) {
          return absl::InvalidArgumentError(absl::StrCat(
              "InvertSimplifyRtxCfg: negative register number in note of '", pat_str, "'"));
        }
      }

      if (parsed_or->pattern.IsSet() &&
          IsCompoundSetRhsForSimplifyRtx(parsed_or->pattern.SetDest(),
                                         parsed_or->pattern.SetSrc())) {
        auto unsimp_or = InvertSimplifyRtxExpr(parsed_or->pattern.SetSrc());
        if (!unsimp_or.ok()) {
          return unsimp_or.status();
        }
        parsed_or->pattern.MutableSetSrc() = std::move(*unsimp_or);
        pat_str = parsed_or->ToSexprString();
      }
    }
    blk.SyncRtlInsnsFromInstructions();
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
