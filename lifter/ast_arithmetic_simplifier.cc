#include "lifter/ast_arithmetic_simplifier.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "core/c_ast_visitor.h"

namespace rom_nom_nom {
namespace {

std::unique_ptr<CExpression> SimplifyShiftAddExpression(const CExpression& expr) {
  std::unique_ptr<CExpression> rewritten;
  switch (expr.Kind()) {
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      auto new_lhs = SimplifyShiftAddExpression(bin.Lhs());
      auto new_rhs = SimplifyShiftAddExpression(bin.Rhs());
      rewritten = CExpression::Binary(bin.Op(), std::move(new_lhs), std::move(new_rhs));
      break;
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      rewritten =
          CExpression::Unary(un.Op(), SimplifyShiftAddExpression(un.Operand()), un.IsPrefix());
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      rewritten = CExpression::Assignment(assign.Op(), assign.Lhs().Clone(),
                                          SimplifyShiftAddExpression(assign.Rhs()));
      break;
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expr);
      rewritten = CExpression::Cast(cast.TargetType(), SimplifyShiftAddExpression(cast.Operand()));
      break;
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      rewritten = CExpression::ArrayIndex(SimplifyShiftAddExpression(arr.Array()),
                                          SimplifyShiftAddExpression(arr.Index()));
      break;
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& mem = static_cast<const MemberAccessExpression&>(expr);
      rewritten = CExpression::MemberAccess(SimplifyShiftAddExpression(mem.Object()),
                                            mem.MemberName(), mem.IsArrow());
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      rewritten = CExpression::Ternary(SimplifyShiftAddExpression(tern.Condition()),
                                       SimplifyShiftAddExpression(tern.TrueExpression()),
                                       SimplifyShiftAddExpression(tern.FalseExpression()));
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> new_args;
      for (const auto& arg : call.Arguments()) {
        new_args.push_back(SimplifyShiftAddExpression(*arg));
      }
      rewritten = CExpression::Call(call.Callee().Clone(), std::move(new_args));
      break;
    }
    default:
      return expr.Clone();
  }

  if (rewritten->Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(*rewritten);

    auto is_int_val = [](const CExpression& e, int64_t* out_val) -> bool {
      if (e.Kind() == CExpressionKind::kIntegerLiteral) {
        if (out_val != nullptr) *out_val = static_cast<const IntegerLiteralExpression&>(e).Value();
        return true;
      }
      return false;
    };

    auto exprs_match = [](const CExpression& a, const CExpression& b) -> bool {
      return a.ToString() == b.ToString();
    };

    // 1. (X << S) + X, X + (X << S), (X * C) + X, or X + (X * C)
    if (bin.Op() == "+") {
      int64_t shift_val = 0;
      if (bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& lhs_bin = static_cast<const BinaryExpression&>(bin.Lhs());
        if (lhs_bin.Op() == "<<" && is_int_val(lhs_bin.Rhs(), &shift_val) && shift_val > 0 &&
            shift_val < 31) {
          if (exprs_match(lhs_bin.Lhs(), bin.Rhs())) {
            int64_t mult = (1LL << shift_val) + 1;
            return CExpression::Binary("*", lhs_bin.Lhs().Clone(), CExpression::Integer(mult));
          }
        }
        int64_t c_val = 0;
        if (lhs_bin.Op() == "*" && is_int_val(lhs_bin.Rhs(), &c_val) && c_val > 1) {
          if (exprs_match(lhs_bin.Lhs(), bin.Rhs())) {
            return CExpression::Binary("*", lhs_bin.Lhs().Clone(), CExpression::Integer(c_val + 1));
          }
        }
      }
      if (bin.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& rhs_bin = static_cast<const BinaryExpression&>(bin.Rhs());
        if (rhs_bin.Op() == "<<" && is_int_val(rhs_bin.Rhs(), &shift_val) && shift_val > 0 &&
            shift_val < 31) {
          if (exprs_match(rhs_bin.Lhs(), bin.Lhs())) {
            int64_t mult = (1LL << shift_val) + 1;
            return CExpression::Binary("*", bin.Lhs().Clone(), CExpression::Integer(mult));
          }
        }
        int64_t c_val = 0;
        if (rhs_bin.Op() == "*" && is_int_val(rhs_bin.Rhs(), &c_val) && c_val > 1) {
          if (exprs_match(rhs_bin.Lhs(), bin.Lhs())) {
            return CExpression::Binary("*", bin.Lhs().Clone(), CExpression::Integer(c_val + 1));
          }
        }
      }
    }

    // 2. (X << S) - X -> X * ((1 << S) - 1), or (X * C) - X -> X * (C - 1)
    if (bin.Op() == "-") {
      int64_t shift_val = 0;
      if (bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& lhs_bin = static_cast<const BinaryExpression&>(bin.Lhs());
        if (lhs_bin.Op() == "<<" && is_int_val(lhs_bin.Rhs(), &shift_val) && shift_val >= 2 &&
            shift_val < 31) {
          if (exprs_match(lhs_bin.Lhs(), bin.Rhs())) {
            int64_t mult = (1LL << shift_val) - 1;
            return CExpression::Binary("*", lhs_bin.Lhs().Clone(), CExpression::Integer(mult));
          }
        }
        int64_t c_val = 0;
        if (lhs_bin.Op() == "*" && is_int_val(lhs_bin.Rhs(), &c_val) && c_val > 2) {
          if (exprs_match(lhs_bin.Lhs(), bin.Rhs())) {
            return CExpression::Binary("*", lhs_bin.Lhs().Clone(), CExpression::Integer(c_val - 1));
          }
        }
      }
    }

    // 3. (X * C) << S  ->  X * (C << S)
    if (bin.Op() == "<<") {
      int64_t shift_val = 0;
      if (is_int_val(bin.Rhs(), &shift_val) && shift_val > 0 && shift_val < 31) {
        if (bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& lhs_bin = static_cast<const BinaryExpression&>(bin.Lhs());
          int64_t c_val = 0;
          if (lhs_bin.Op() == "*" && is_int_val(lhs_bin.Rhs(), &c_val)) {
            int64_t new_mult = c_val << shift_val;
            return CExpression::Binary("*", lhs_bin.Lhs().Clone(), CExpression::Integer(new_mult));
          }
        }
      }
    }

    // 4. Direct ((X << S) + X) << S2
    if (bin.Op() == "<<") {
      int64_t s2 = 0;
      if (is_int_val(bin.Rhs(), &s2) && s2 > 0 && s2 < 31) {
        if (bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& inner = static_cast<const BinaryExpression&>(bin.Lhs());
          if (inner.Op() == "+") {
            int64_t s1 = 0;
            if (inner.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
              const auto& sll = static_cast<const BinaryExpression&>(inner.Lhs());
              if (sll.Op() == "<<" && is_int_val(sll.Rhs(), &s1) && s1 > 0 && s1 < 31) {
                if (exprs_match(sll.Lhs(), inner.Rhs())) {
                  int64_t mult = ((1LL << s1) + 1) << s2;
                  return CExpression::Binary("*", sll.Lhs().Clone(), CExpression::Integer(mult));
                }
              }
            }
          }
        }
      }
    }

    // 5. Sign-extension shift pair: ((X << 16) >> 16) -> (s16)(X), ((X << 24) >> 24) -> (s8)(X)
    if (bin.Op() == ">>") {
      int64_t s_right = 0;
      if (is_int_val(bin.Rhs(), &s_right) && (s_right == 16 || s_right == 24) &&
          bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& inner = static_cast<const BinaryExpression&>(bin.Lhs());
        int64_t s_left = 0;
        if (inner.Op() == "<<" && is_int_val(inner.Rhs(), &s_left) && s_left == s_right) {
          CType target_type = (s_right == 16) ? CType::S16() : CType::S8();
          return CExpression::Cast(target_type, inner.Lhs().Clone());
        }
      }
      // 6. Signed division by 2: ((X + ((u32)X >> 31)) >> 1) -> (X / 2)
      if (is_int_val(bin.Rhs(), &s_right) && s_right == 1 &&
          bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& inner = static_cast<const BinaryExpression&>(bin.Lhs());
        if (inner.Op() == "+") {
          auto is_unsigned_shr_31_of = [&](const CExpression& cand, const CExpression& x) {
            if (cand.Kind() != CExpressionKind::kBinaryExpression) return false;
            const auto& shr = static_cast<const BinaryExpression&>(cand);
            int64_t sh = 0;
            if (shr.Op() != ">>" || !is_int_val(shr.Rhs(), &sh) || sh != 31) return false;
            if (shr.Lhs().Kind() != CExpressionKind::kCastExpression) return false;
            const auto& cast = static_cast<const CastExpression&>(shr.Lhs());
            return cast.TargetType().name == "u32" && cast.TargetType().pointer_depth == 0 &&
                   exprs_match(cast.Operand(), x);
          };
          if (is_unsigned_shr_31_of(inner.Rhs(), inner.Lhs())) {
            return CExpression::Binary("/", inner.Lhs().Clone(), CExpression::Integer(2));
          }
          if (is_unsigned_shr_31_of(inner.Lhs(), inner.Rhs())) {
            return CExpression::Binary("/", inner.Rhs().Clone(), CExpression::Integer(2));
          }
        }
      }
    }
  }

  return rewritten;
}

int CountRhsVariableUsesInStatement(const CStatement& root_s, std::string_view var) {
  return CountVariableReadUsesInStatement(root_s, var);
}

bool IsLocalRegVarName(std::string_view name) {
  if (name == "v0" || name == "v1" || name == "arg0" || name == "arg1" || name == "arg2" ||
      name == "arg3" || name == "fp" || name == "at") {
    return true;
  }
  if (absl::StartsWith(name, "temp_")) return true;
  if (name.size() == 2 && (name[0] == 's' || name[0] == 't') &&
      std::isdigit(static_cast<unsigned char>(name[1]))) {
    return true;
  }
  if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))) {
    return true;
  }
  return false;
}

bool ExtractSimpleRegVarAssignment(const CStatement& s, std::string* lhs_out,
                                   const CExpression** rhs_out) {
  if (s.Kind() != CStatementKind::kExpressionStatement) return false;
  const auto& es = static_cast<const ExpressionStatement&>(s);
  if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
  const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
  if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
  const std::string& name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
  if (!IsLocalRegVarName(name)) return false;
  *lhs_out = name;
  *rhs_out = &a.Rhs();
  return true;
}

const CExpression* ExtractHighMultOperand(const CExpression& e, bool* is_unsigned_out) {
  if (e.Kind() == CExpressionKind::kCastExpression) {
    const auto& c1 = static_cast<const CastExpression&>(e);
    if (c1.TargetType().name == "s64" && c1.TargetType().pointer_depth == 0) {
      *is_unsigned_out = false;
      return &c1.Operand();
    }
    if (c1.TargetType().name == "u64" && c1.TargetType().pointer_depth == 0 &&
        c1.Operand().Kind() == CExpressionKind::kCastExpression) {
      const auto& c2 = static_cast<const CastExpression&>(c1.Operand());
      if (c2.TargetType().name == "u32" && c2.TargetType().pointer_depth == 0) {
        *is_unsigned_out = true;
        return &c2.Operand();
      }
    }
  }
  return nullptr;
}

uint32_t ComputeUnsignedReciprocalDivisor(uint32_t magic, int64_t shift, bool has_adjust) {
  if (magic == 0 || shift < 0 || shift > 31) return 0;
  unsigned __int128 num = static_cast<unsigned __int128>(1) << (32 + shift + (has_adjust ? 1 : 0));
  uint64_t den =
      has_adjust ? (static_cast<uint64_t>(magic) + (1ULL << 32)) : static_cast<uint64_t>(magic);
  uint64_t d_est = static_cast<uint64_t>(num / den);
  for (int delta = -1; delta <= 2; ++delta) {
    int64_t cand = static_cast<int64_t>(d_est) + delta;
    if (cand <= 1 || cand > 0x7FFFFFFFLL) continue;
    uint64_t d = static_cast<uint64_t>(cand);
    unsigned __int128 expected_m = (num + d - 1) / d;
    if (has_adjust) {
      if (expected_m < (static_cast<unsigned __int128>(1) << 32)) continue;
      expected_m -= (static_cast<unsigned __int128>(1) << 32);
    }
    if (static_cast<uint32_t>(expected_m) == magic ||
        static_cast<uint32_t>(expected_m - 1) == magic) {
      return static_cast<uint32_t>(d);
    }
  }
  return 0;
}

int64_t ComputeSignedReciprocalDivisor(uint32_t magic, int64_t shift, bool has_add_x) {
  if (magic == 0 || shift < 0 || shift > 31) return 0;
  if (!has_add_x && static_cast<int32_t>(magic) <= 0) return 0;
  if (has_add_x && static_cast<int32_t>(magic) >= 0) return 0;
  unsigned __int128 num = static_cast<unsigned __int128>(1) << (32 + shift);
  uint64_t d_est = static_cast<uint64_t>(num / magic);
  for (int delta = -1; delta <= 2; ++delta) {
    int64_t cand = static_cast<int64_t>(d_est) + delta;
    if (cand <= 1 || cand > 0x7FFFFFFFLL) continue;
    uint64_t d = static_cast<uint64_t>(cand);
    unsigned __int128 expected_m = (num + d - 1) / d;
    if (static_cast<uint32_t>(expected_m) == magic ||
        static_cast<uint32_t>(expected_m - 1) == magic) {
      return static_cast<int64_t>(d);
    }
  }
  return 0;
}

bool ExtractReciprocalMagicConst(const CExpression& e,
                                 const absl::flat_hash_map<std::string, uint32_t>& const_map,
                                 uint32_t* magic_out, std::string* var_name_out = nullptr) {
  const CExpression* cur = &e;
  while (cur->Kind() == CExpressionKind::kCastExpression) {
    cur = &static_cast<const CastExpression*>(cur)->Operand();
  }
  if (cur->Kind() == CExpressionKind::kIntegerLiteral) {
    *magic_out = static_cast<uint32_t>(static_cast<const IntegerLiteralExpression*>(cur)->Value());
    if (var_name_out != nullptr) var_name_out->clear();
    return true;
  }
  if (cur->Kind() == CExpressionKind::kIdentifier) {
    const std::string& name = static_cast<const IdentifierExpression*>(cur)->Name();
    auto it = const_map.find(name);
    if (it != const_map.end()) {
      *magic_out = it->second;
      if (var_name_out != nullptr) *var_name_out = name;
      return true;
    }
  }
  return false;
}

bool MatchReciprocalHighMult(const CExpression& e,
                             const absl::flat_hash_map<std::string, uint32_t>& const_map,
                             bool* is_unsigned_out, const CExpression** x_expr_out,
                             uint32_t* magic_out, std::string* magic_var_out = nullptr) {
  const CExpression* cur = &e;
  if (cur->Kind() == CExpressionKind::kCastExpression &&
      static_cast<const CastExpression*>(cur)->TargetType().name == "u32") {
    cur = &static_cast<const CastExpression*>(cur)->Operand();
  }
  if (cur->Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& shr = static_cast<const BinaryExpression&>(*cur);
  if (shr.Op() != ">>" || shr.Rhs().Kind() != CExpressionKind::kIntegerLiteral ||
      static_cast<const IntegerLiteralExpression&>(shr.Rhs()).Value() != 32) {
    return false;
  }
  if (shr.Lhs().Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& mul = static_cast<const BinaryExpression&>(shr.Lhs());
  if (mul.Op() != "*") return false;
  bool u_lhs = false;
  bool u_rhs = false;
  const CExpression* op_lhs = ExtractHighMultOperand(mul.Lhs(), &u_lhs);
  const CExpression* op_rhs = ExtractHighMultOperand(mul.Rhs(), &u_rhs);
  if (op_lhs == nullptr || op_rhs == nullptr || u_lhs != u_rhs) return false;
  uint32_t m = 0;
  std::string mv;
  if (ExtractReciprocalMagicConst(mul.Rhs(), const_map, &m, &mv)) {
    *is_unsigned_out = u_lhs;
    *x_expr_out = op_lhs;
    *magic_out = m;
    if (magic_var_out != nullptr) *magic_var_out = mv;
    return true;
  }
  if (ExtractReciprocalMagicConst(mul.Lhs(), const_map, &m, &mv)) {
    *is_unsigned_out = u_lhs;
    *x_expr_out = op_rhs;
    *magic_out = m;
    if (magic_var_out != nullptr) *magic_var_out = mv;
    return true;
  }
  return false;
}

bool IsReciprocalSignBitShiftOf(const CExpression& e, const CExpression& x_expr,
                                const absl::flat_hash_map<std::string, std::string>& sign_bit_map,
                                const absl::flat_hash_map<std::string, std::string>& sign_equiv_map,
                                absl::flat_hash_set<std::string>* consumed_sign_vars) {
  if (e.Kind() == CExpressionKind::kIdentifier) {
    const std::string& vname = static_cast<const IdentifierExpression&>(e).Name();
    auto it = sign_bit_map.find(vname);
    if (it != sign_bit_map.end()) {
      if (it->second == x_expr.ToString()) {
        consumed_sign_vars->insert(vname);
        return true;
      }
      if (x_expr.Kind() == CExpressionKind::kIdentifier) {
        auto eq_it = sign_equiv_map.find(static_cast<const IdentifierExpression&>(x_expr).Name());
        if (eq_it != sign_equiv_map.end() && eq_it->second == it->second) {
          consumed_sign_vars->insert(vname);
          return true;
        }
      }
      if (x_expr.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& x_bin = static_cast<const BinaryExpression&>(x_expr);
        if (x_bin.Op() == ">>" && x_bin.Lhs().ToString() == it->second) {
          consumed_sign_vars->insert(vname);
          return true;
        }
      }
    }
    return false;
  }
  if (e.Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& bin = static_cast<const BinaryExpression&>(e);
  if (bin.Op() != ">>" || bin.Rhs().Kind() != CExpressionKind::kIntegerLiteral ||
      static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value() != 31) {
    return false;
  }
  if (bin.Lhs().ToString() == x_expr.ToString()) return true;
  if (x_expr.Kind() == CExpressionKind::kIdentifier) {
    auto eq_it = sign_equiv_map.find(static_cast<const IdentifierExpression&>(x_expr).Name());
    if (eq_it != sign_equiv_map.end() && eq_it->second == bin.Lhs().ToString()) {
      return true;
    }
  }
  if (x_expr.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& x_bin = static_cast<const BinaryExpression&>(x_expr);
    if (x_bin.Op() == ">>" && x_bin.Lhs().ToString() == bin.Lhs().ToString()) {
      return true;
    }
  }
  return false;
}

bool IsUnsignedFloatBiasExpr(const CExpression& expr,
                             const absl::flat_hash_set<std::string>* bias_vars = nullptr,
                             std::string* consumed_bias_var_out = nullptr) {
  if (consumed_bias_var_out != nullptr) {
    consumed_bias_var_out->clear();
  }
  if (expr.Kind() == CExpressionKind::kFloatLiteral) {
    return static_cast<const FloatLiteralExpression&>(expr).Value() == 4294967296.0;
  }
  if (expr.Kind() == CExpressionKind::kIdentifier) {
    const std::string& name = static_cast<const IdentifierExpression&>(expr).Name();
    double d_val = 0.0;
    if (absl::SimpleAtod(name, &d_val)) {
      return d_val == 4294967296.0;
    }
    if (bias_vars != nullptr && bias_vars->contains(name)) {
      if (consumed_bias_var_out != nullptr) {
        *consumed_bias_var_out = name;
      }
      return true;
    }
    return absl::StartsWith(name, "D_") || absl::StartsWith(name, "RO_");
  }
  if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& ue = static_cast<const UnaryExpression&>(expr);
    if (ue.Op() == "*(f64*)&" || ue.Op() == "*(f64 *)&" || ue.Op() == "*(const f64*)&" ||
        ue.Op() == "*(const f64 *)&") {
      return true;
    }
    if (ue.Op() == "*" && ue.Operand().Kind() == CExpressionKind::kCastExpression) {
      const auto& ce = static_cast<const CastExpression&>(ue.Operand());
      if (ce.TargetType().pointer_depth == 1 &&
          (ce.TargetType().name == "f64" || ce.TargetType().name == "const f64" ||
           ce.TargetType().name == "volatile f64")) {
        return true;
      }
    }
  }
  if (expr.Kind() == CExpressionKind::kCastExpression) {
    const auto& ce = static_cast<const CastExpression&>(expr);
    if (ce.TargetType().name == "f64" && ce.TargetType().pointer_depth == 0) {
      return IsUnsignedFloatBiasExpr(ce.Operand(), bias_vars, consumed_bias_var_out);
    }
  }
  std::string expr_str = expr.ToString();
  return absl::StrContains(expr_str, "D_") || absl::StrContains(expr_str, "RO_");
}

void SimplifyModuloAndUnsignedFloatDiamondsInBlockImpl(
    CompoundStatement* comp, const absl::flat_hash_set<std::string>* parent_bias_vars,
    absl::flat_hash_set<std::string>* consumed_bias_vars);

void SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
    CompoundStatement* comp, const absl::flat_hash_map<std::string, uint32_t>* parent_const_map,
    absl::flat_hash_set<uint32_t>* shared_consumed_magics,
    const absl::flat_hash_map<std::string, std::string>* parent_sign_equiv_map,
    const absl::flat_hash_set<std::string>* parent_bias_vars = nullptr,
    absl::flat_hash_set<std::string>* shared_consumed_bias_vars = nullptr) {
  if (comp == nullptr) return;

  absl::flat_hash_map<std::string, uint32_t> const_map;
  if (parent_const_map != nullptr) {
    const_map = *parent_const_map;
  }
  absl::flat_hash_map<std::string, std::string> sign_equiv_map;
  if (parent_sign_equiv_map != nullptr) {
    sign_equiv_map = *parent_sign_equiv_map;
  }
  absl::flat_hash_set<std::string> bias_vars;
  if (parent_bias_vars != nullptr) {
    bias_vars = *parent_bias_vars;
  }
  absl::flat_hash_set<uint32_t> root_consumed_magics;
  absl::flat_hash_set<uint32_t>* consumed_magics =
      (shared_consumed_magics != nullptr) ? shared_consumed_magics : &root_consumed_magics;
  absl::flat_hash_set<std::string> root_consumed_bias_vars;
  absl::flat_hash_set<std::string>* consumed_bias_vars =
      (shared_consumed_bias_vars != nullptr) ? shared_consumed_bias_vars : &root_consumed_bias_vars;

  auto is_int_literal = [](const CExpression& e, int64_t* val_out = nullptr) -> bool {
    if (e.Kind() == CExpressionKind::kIntegerLiteral) {
      if (val_out != nullptr) {
        *val_out = static_cast<const IntegerLiteralExpression&>(e).Value();
      }
      return true;
    }
    return false;
  };

  auto match_high_mult = [&](const CExpression& e, bool* is_unsigned_out,
                             const CExpression** x_expr_out, uint32_t* magic_out,
                             std::string* magic_var_out = nullptr) -> bool {
    return MatchReciprocalHighMult(e, const_map, is_unsigned_out, x_expr_out, magic_out,
                                   magic_var_out);
  };

  auto compute_unsigned_divisor = ComputeUnsignedReciprocalDivisor;
  auto compute_signed_divisor = ComputeSignedReciprocalDivisor;

  auto make_unsigned_div_expr = [](const CExpression& x_expr,
                                   uint32_t divisor) -> std::unique_ptr<CExpression> {
    return CExpression::Binary(
        "/", CExpression::Cast(CType::U32(), x_expr.Clone()),
        CExpression::Integer(divisor, /*is_hex=*/false, /*is_unsigned=*/true));
  };

  auto make_signed_div_expr = [](const CExpression& x_expr,
                                 int64_t divisor) -> std::unique_ptr<CExpression> {
    return CExpression::Binary("/", x_expr.Clone(), CExpression::Integer(divisor));
  };

  absl::flat_hash_map<std::string, std::string> sign_bit_map;
  absl::flat_hash_set<std::string> consumed_sign_vars;

  auto is_sign_bit_shift_of = [&](const CExpression& e, const CExpression& x_expr) -> bool {
    return IsReciprocalSignBitShiftOf(e, x_expr, sign_bit_map, sign_equiv_map, &consumed_sign_vars);
  };

  std::function<std::unique_ptr<CExpression>(const CExpression&)> rewrite_div_expr =
      [&](const CExpression& expr) -> std::unique_ptr<CExpression> {
    std::unique_ptr<CExpression> rewritten;
    switch (expr.Kind()) {
      case CExpressionKind::kBinaryExpression: {
        const auto& bin = static_cast<const BinaryExpression&>(expr);
        rewritten =
            CExpression::Binary(bin.Op(), rewrite_div_expr(bin.Lhs()), rewrite_div_expr(bin.Rhs()));
        break;
      }
      case CExpressionKind::kUnaryExpression: {
        const auto& un = static_cast<const UnaryExpression&>(expr);
        rewritten = CExpression::Unary(un.Op(), rewrite_div_expr(un.Operand()), un.IsPrefix());
        break;
      }
      case CExpressionKind::kAssignmentExpression: {
        const auto& a = static_cast<const AssignmentExpression&>(expr);
        rewritten = CExpression::Assignment(a.Op(), a.Lhs().Clone(), rewrite_div_expr(a.Rhs()));
        break;
      }
      case CExpressionKind::kCastExpression: {
        const auto& c = static_cast<const CastExpression&>(expr);
        rewritten = CExpression::Cast(c.TargetType(), rewrite_div_expr(c.Operand()));
        break;
      }
      case CExpressionKind::kCallExpression: {
        const auto& call = static_cast<const CallExpression&>(expr);
        std::vector<std::unique_ptr<CExpression>> args;
        for (const auto& arg : call.Arguments()) {
          args.push_back(rewrite_div_expr(*arg));
        }
        rewritten = CExpression::Call(call.Callee().Clone(), std::move(args));
        break;
      }
      default:
        return expr.Clone();
    }

    if (rewritten->Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(*rewritten);
      // 1. Unsigned reciprocal division: `((u32)(((u64)(u32)X * (u64)(u32)M) >> 32) >> s)`
      int64_t shift_s = 0;
      if (bin.Op() == ">>" && is_int_literal(bin.Rhs(), &shift_s) && shift_s >= 1 &&
          shift_s <= 30) {
        bool is_u = false;
        const CExpression* x_expr = nullptr;
        uint32_t magic = 0;
        if (match_high_mult(bin.Lhs(), &is_u, &x_expr, &magic) && is_u) {
          uint32_t div = compute_unsigned_divisor(magic, shift_s, /*has_adjust=*/false);
          if (div > 1) {
            consumed_magics->insert(magic);
            return make_unsigned_div_expr(*x_expr, div);
          }
        }
      }
      // 2. Signed reciprocal division: `((hi >> s) - (X >> 31))` or `(((hi + X) >> s) - (X >> 31))`
      // or `(hi - (X >> 31))`
      if (bin.Op() == "-") {
        const CExpression* lhs_inner = &bin.Lhs();
        int64_t s_val = 0;
        if (lhs_inner->Kind() == CExpressionKind::kBinaryExpression) {
          const auto& lhs_bin = static_cast<const BinaryExpression&>(*lhs_inner);
          if (lhs_bin.Op() == ">>" && is_int_literal(lhs_bin.Rhs(), &s_val) && s_val >= 1 &&
              s_val <= 30) {
            lhs_inner = &lhs_bin.Lhs();
          }
        }
        bool has_add_x = false;
        const CExpression* mult_cand = lhs_inner;
        const CExpression* added_x = nullptr;
        if (lhs_inner->Kind() == CExpressionKind::kBinaryExpression &&
            static_cast<const BinaryExpression*>(lhs_inner)->Op() == "+") {
          const auto& add_bin = static_cast<const BinaryExpression&>(*lhs_inner);
          mult_cand = &add_bin.Lhs();
          added_x = &add_bin.Rhs();
          has_add_x = true;
        }
        bool is_u = false;
        const CExpression* x_expr = nullptr;
        uint32_t magic = 0;
        if (match_high_mult(*mult_cand, &is_u, &x_expr, &magic) && !is_u &&
            (!has_add_x || added_x->ToString() == x_expr->ToString()) &&
            is_sign_bit_shift_of(bin.Rhs(), *x_expr)) {
          int64_t div = compute_signed_divisor(magic, s_val, has_add_x);
          if (div > 1) {
            consumed_magics->insert(magic);
            return make_signed_div_expr(*x_expr, div);
          }
        }
        // 3. Modulo subexpression: `X - ((X / D) * D)` or `X - ((X / D) << k)` where `D == 1 << k`
        if (bin.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& rhs_b = static_cast<const BinaryExpression&>(bin.Rhs());
          int64_t mult_d = 0;
          int64_t sh_k = 0;
          bool has_mult_d =
              (rhs_b.Op() == "*" && is_int_literal(rhs_b.Rhs(), &mult_d) && mult_d > 1);
          bool has_sh_k =
              (rhs_b.Op() == "<<" && is_int_literal(rhs_b.Rhs(), &sh_k) && sh_k >= 1 && sh_k <= 30);
          if ((has_mult_d || has_sh_k) &&
              rhs_b.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
            const auto& div_b = static_cast<const BinaryExpression&>(rhs_b.Lhs());
            int64_t div_d = 0;
            if (div_b.Op() == "/" && is_int_literal(div_b.Rhs(), &div_d) && div_d > 1) {
              int64_t expected_d = has_mult_d ? mult_d : (1LL << sh_k);
              if (div_d == expected_d) {
                std::string lhs_str = bin.Lhs().ToString();
                if (div_b.Lhs().ToString() == lhs_str ||
                    div_b.Lhs().ToString() == absl::StrCat("((u32)", lhs_str, ")")) {
                  return CExpression::Binary("%", div_b.Lhs().Clone(), div_b.Rhs().Clone());
                }
              }
            }
          }
        }
      }
    }
    return rewritten;
  };

  auto count_var_uses_in_stmt = CountRhsVariableUsesInStatement;
  auto is_local_reg_var = IsLocalRegVarName;
  auto extract_simple_var_assign = ExtractSimpleRegVarAssignment;

  // Matches `(((s64)X * (s64)M) >> 32)`, `((((s64)X * (s64)M) >> 32) + X)`,
  // `((((s64)X * (s64)M) >> 32) >> s)`, or `(((((s64)X * (s64)M) >> 32) + X) >> s)`.
  auto match_signed_div_prefix = [&](const CExpression& e, const CExpression** x_expr_out,
                                     uint32_t* magic_out, bool* has_add_x_out,
                                     int64_t* s_val_out) -> bool {
    const CExpression* cur = &e;
    int64_t s_val = 0;
    if (cur->Kind() == CExpressionKind::kBinaryExpression) {
      const auto& b = static_cast<const BinaryExpression&>(*cur);
      if (b.Op() == ">>" && is_int_literal(b.Rhs(), &s_val) && s_val >= 1 && s_val <= 30) {
        cur = &b.Lhs();
      } else {
        s_val = 0;
      }
    }
    bool has_add_x = false;
    const CExpression* added_x = nullptr;
    if (cur->Kind() == CExpressionKind::kBinaryExpression &&
        static_cast<const BinaryExpression*>(cur)->Op() == "+") {
      const auto& add_b = static_cast<const BinaryExpression&>(*cur);
      cur = &add_b.Lhs();
      added_x = &add_b.Rhs();
      has_add_x = true;
    }
    bool is_u = false;
    const CExpression* x_expr = nullptr;
    uint32_t magic = 0;
    if (!match_high_mult(*cur, &is_u, &x_expr, &magic) || is_u) {
      return false;
    }
    if (has_add_x && added_x->ToString() != x_expr->ToString()) {
      return false;
    }
    *x_expr_out = x_expr;
    *magic_out = magic;
    *has_add_x_out = has_add_x;
    *s_val_out = s_val;
    return true;
  };

  auto& stmts = comp->MutableStatements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    std::string lhs_var;
    const CExpression* rhs_expr = nullptr;
    if (extract_simple_var_assign(*stmts[i], &lhs_var, &rhs_expr)) {
      int64_t lit_val = 0;
      if (is_int_literal(*rhs_expr, &lit_val)) {
        const_map[lhs_var] = static_cast<uint32_t>(lit_val);
      } else {
        const_map.erase(lhs_var);
      }
      int64_t sh_val = 0;
      if (rhs_expr->Kind() == CExpressionKind::kBinaryExpression &&
          static_cast<const BinaryExpression*>(rhs_expr)->Op() == ">>" &&
          is_int_literal(static_cast<const BinaryExpression*>(rhs_expr)->Rhs(), &sh_val) &&
          sh_val == 31) {
        std::string sh_lhs = static_cast<const BinaryExpression*>(rhs_expr)->Lhs().ToString();
        auto eq_it = sign_equiv_map.find(sh_lhs);
        sign_bit_map[lhs_var] = (eq_it != sign_equiv_map.end()) ? eq_it->second : sh_lhs;
      } else {
        sign_bit_map.erase(lhs_var);
      }
      absl::erase_if(sign_equiv_map,
                     [&](const auto& kv) { return kv.first == lhs_var || kv.second == lhs_var; });
      if (rhs_expr->Kind() == CExpressionKind::kIdentifier) {
        sign_equiv_map[lhs_var] = static_cast<const IdentifierExpression*>(rhs_expr)->Name();
      } else if (rhs_expr->Kind() == CExpressionKind::kBinaryExpression &&
                 static_cast<const BinaryExpression*>(rhs_expr)->Op() == ">>" &&
                 is_int_literal(static_cast<const BinaryExpression*>(rhs_expr)->Rhs(), &sh_val) &&
                 (sh_val == 16 || sh_val == 24) &&
                 static_cast<const BinaryExpression*>(rhs_expr)->Lhs().Kind() ==
                     CExpressionKind::kIdentifier) {
        sign_equiv_map[lhs_var] = static_cast<const BinaryExpression*>(rhs_expr)->Lhs().ToString();
      }
      if (absl::EndsWith(lhs_var, "_d")) {
        if (IsUnsignedFloatBiasExpr(*rhs_expr, &bias_vars)) {
          bias_vars.insert(lhs_var);
        } else {
          bias_vars.erase(lhs_var);
        }
      }
    }

    // Multi-statement Pattern A:
    //   stmt i:     `hi_var = (((u64)(u32)X * (u64)(u32)M) >> 32);`
    //   (optional 1 unrelated statement, e.g. `v1 = ((u32)v1 < 100);` or `f0 = (f32)f2_d;`)
    //   stmt j:     `q_var = ((u32)hi_var >> s);` or `q_var = (((u32)hi_var >> s) & mask);`
    if (extract_simple_var_assign(*stmts[i], &lhs_var, &rhs_expr)) {
      bool is_u = false;
      const CExpression* x_expr = nullptr;
      uint32_t magic = 0;
      std::string magic_var;
      bool prefix_has_add_x = false;
      int64_t prefix_s_val = 0;
      if (match_high_mult(*rhs_expr, &is_u, &x_expr, &magic, &magic_var) && is_u) {
        // Check for overflow-adjusted unsigned division:
        // i:   `hi_var = (((u64)(u32)X * (u64)(u32)M) >> 32);`
        // i+1: `t1 = (X - hi_var);`
        // i+2: `t2 = ((u32)t1 >> 1);`
        // i+3: `t3 = (hi_var + t2);`
        // i+4: `q_var = ((u32)t3 >> s);`
        std::string v_sub, v_shr1, v_add, v_q;
        const CExpression* r_sub = nullptr;
        const CExpression* r_shr1 = nullptr;
        const CExpression* r_add = nullptr;
        const CExpression* r_q = nullptr;
        if (i + 4 < stmts.size() && extract_simple_var_assign(*stmts[i + 1], &v_sub, &r_sub) &&
            extract_simple_var_assign(*stmts[i + 2], &v_shr1, &r_shr1) &&
            extract_simple_var_assign(*stmts[i + 3], &v_add, &r_add) &&
            extract_simple_var_assign(*stmts[i + 4], &v_q, &r_q) &&
            r_sub->Kind() == CExpressionKind::kBinaryExpression &&
            static_cast<const BinaryExpression*>(r_sub)->Op() == "-" &&
            static_cast<const BinaryExpression*>(r_sub)->Lhs().ToString() == x_expr->ToString() &&
            static_cast<const BinaryExpression*>(r_sub)->Rhs().ToString() == lhs_var &&
            r_shr1->Kind() == CExpressionKind::kBinaryExpression &&
            static_cast<const BinaryExpression*>(r_shr1)->Op() == ">>" &&
            r_add->Kind() == CExpressionKind::kBinaryExpression &&
            static_cast<const BinaryExpression*>(r_add)->Op() == "+" &&
            r_q->Kind() == CExpressionKind::kBinaryExpression &&
            static_cast<const BinaryExpression*>(r_q)->Op() == ">>") {
          int64_t s_val = 0;
          if (is_int_literal(static_cast<const BinaryExpression*>(r_q)->Rhs(), &s_val)) {
            uint32_t div = compute_unsigned_divisor(magic, s_val, /*has_adjust=*/true);
            if (div > 1) {
              consumed_magics->insert(magic);
              auto div_expr = make_unsigned_div_expr(*x_expr, div);
              stmts[i + 4] = CStatement::Expression(
                  CExpression::Assignment("=", CExpression::Identifier(v_q), std::move(div_expr)));
              stmts.erase(stmts.begin() + i, stmts.begin() + i + 4);
              --i;
              continue;
            }
          }
        }

        // Standard multi-statement unsigned division:
        // `hi_var = (((u64)(u32)X * (u64)(u32)M) >> 32);` followed by
        // `q_var = ((u32)hi_var >> s);` or `*(s32*)... = ((u32)hi_var >> s);`
        std::string x_str = x_expr->ToString();
        bool folded_u = false;
        for (size_t j = i + 1; j < stmts.size() && j <= i + 12; ++j) {
          if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) continue;
          const auto& es_j = static_cast<const ExpressionStatement&>(*stmts[j]);
          if (es_j.Expression().Kind() != CExpressionKind::kAssignmentExpression) continue;
          const auto& assign_j = static_cast<const AssignmentExpression&>(es_j.Expression());
          if (assign_j.Op() != "=") continue;
          std::string q_var = (assign_j.Lhs().Kind() == CExpressionKind::kIdentifier)
                                  ? static_cast<const IdentifierExpression&>(assign_j.Lhs()).Name()
                                  : "";
          const CExpression* q_rhs = &assign_j.Rhs();
          const CExpression* shift_cand = q_rhs;
          int64_t mask_val = 0;
          bool has_mask = false;
          if (shift_cand->Kind() == CExpressionKind::kBinaryExpression) {
            const auto& b = static_cast<const BinaryExpression&>(*shift_cand);
            if (b.Op() == "&" && is_int_literal(b.Rhs(), &mask_val)) {
              shift_cand = &b.Lhs();
              has_mask = true;
            }
          }
          int64_t s_val = 0;
          if (shift_cand->Kind() == CExpressionKind::kBinaryExpression) {
            const auto& b = static_cast<const BinaryExpression&>(*shift_cand);
            if (b.Op() == ">>" && is_int_literal(b.Rhs(), &s_val) && s_val >= 1 && s_val <= 30) {
              const CExpression* shift_lhs = &b.Lhs();
              if (shift_lhs->Kind() == CExpressionKind::kCastExpression &&
                  static_cast<const CastExpression*>(shift_lhs)->TargetType().name == "u32") {
                shift_lhs = &static_cast<const CastExpression*>(shift_lhs)->Operand();
              }
              if (shift_lhs->Kind() == CExpressionKind::kIdentifier &&
                  static_cast<const IdentifierExpression*>(shift_lhs)->Name() == lhs_var) {
                uint32_t div = compute_unsigned_divisor(magic, s_val, /*has_adjust=*/false);
                if (div > 1) {
                  consumed_magics->insert(magic);
                  auto new_rhs = make_unsigned_div_expr(*x_expr, div);
                  if (has_mask) {
                    new_rhs = CExpression::Binary("&", std::move(new_rhs),
                                                  CExpression::Integer(mask_val));
                  }
                  // Check if any variable in x_expr was modified between i+1 and j-1; if so, we
                  // can only fold if lhs_var == q_var (so we can place the folded division at `i`
                  // before `X` is clobbered).
                  bool x_clobbered_between = false;
                  for (size_t p = i + 1; p < j; ++p) {
                    std::string plhs;
                    const CExpression* prhs = nullptr;
                    if (extract_simple_var_assign(*stmts[p], &plhs, &prhs) &&
                        count_var_uses_in_stmt(*stmts[i], plhs) > 0) {
                      x_clobbered_between = true;
                      break;
                    }
                  }
                  bool can_place_at_i = !q_var.empty() && q_var == lhs_var;
                  if (x_clobbered_between && !can_place_at_i && !q_var.empty()) {
                    bool q_or_lhs_used_between = false;
                    for (size_t p = i + 1; p < j; ++p) {
                      if (count_var_uses_in_stmt(*stmts[p], q_var) > 0 ||
                          count_var_uses_in_stmt(*stmts[p], lhs_var) > 0) {
                        q_or_lhs_used_between = true;
                        break;
                      }
                    }
                    bool lhs_used_after_j = false;
                    for (size_t p = j + 1; p < stmts.size(); ++p) {
                      if (count_var_uses_in_stmt(*stmts[p], lhs_var) > 0) {
                        lhs_used_after_j = true;
                        break;
                      }
                    }
                    if (!q_or_lhs_used_between && !lhs_used_after_j) {
                      can_place_at_i = true;
                    }
                  }
                  if (x_clobbered_between && !can_place_at_i) {
                    break;
                  }
                  if (x_clobbered_between && can_place_at_i) {
                    stmts[i] = CStatement::Expression(
                        CExpression::Assignment("=", assign_j.Lhs().Clone(), std::move(new_rhs)));
                    stmts.erase(stmts.begin() + j);
                  } else {
                    stmts[j] = CStatement::Expression(
                        CExpression::Assignment("=", assign_j.Lhs().Clone(), std::move(new_rhs)));
                    stmts.erase(stmts.begin() + i);
                    --i;
                  }
                  folded_u = true;
                  break;
                }
              }
            }
          }
          if (!q_var.empty() && q_var == lhs_var) break;
        }
        if (folded_u) continue;
      } else if (match_signed_div_prefix(*rhs_expr, &x_expr, &magic, &prefix_has_add_x,
                                         &prefix_s_val)) {
        // Signed multi-statement reciprocal division:
        // `hi_var = (((s64)X * (s64)M) >> 32);` or partially folded `((hi + X) >> s)`
        // Track a forward chain of `cur_var = (cur_var + X)` and `cur_var = (cur_var >> s)` up to
        // `<lhs> = (((cur_var [+ X]) [>> s]) - sign_expr) [- const]`.
        std::string x_str = x_expr->ToString();
        std::string cur_var = lhs_var;
        bool chain_has_add_x = prefix_has_add_x;
        int64_t chain_s_val = prefix_s_val;
        std::vector<size_t> chain_stmts = {i};
        bool folded_s = false;
        for (size_t j = i + 1; j < stmts.size() && j <= i + 12; ++j) {
          if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) continue;
          const auto& es_j = static_cast<const ExpressionStatement&>(*stmts[j]);
          if (es_j.Expression().Kind() != CExpressionKind::kAssignmentExpression) continue;
          const auto& assign_j = static_cast<const AssignmentExpression&>(es_j.Expression());
          if (assign_j.Op() != "=") continue;
          std::string q_var = (assign_j.Lhs().Kind() == CExpressionKind::kIdentifier)
                                  ? static_cast<const IdentifierExpression&>(assign_j.Lhs()).Name()
                                  : "";
          const CExpression* q_rhs = &assign_j.Rhs();

          // Check intermediate step 0: register/stack copy `q_var = cur_var`
          if (!q_var.empty() && (is_local_reg_var(q_var) || absl::StartsWith(q_var, "var_sp_")) &&
              q_rhs->Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(*q_rhs).Name() == cur_var) {
            cur_var = q_var;
            chain_stmts.push_back(j);
            continue;
          }

          // Check intermediate step 1: `next_var = (cur_var + X)`
          if (!q_var.empty() && is_local_reg_var(q_var) && !chain_has_add_x && chain_s_val == 0 &&
              q_rhs->Kind() == CExpressionKind::kBinaryExpression) {
            const auto& b = static_cast<const BinaryExpression&>(*q_rhs);
            if (b.Op() == "+" && b.Lhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(b.Lhs()).Name() == cur_var &&
                b.Rhs().ToString() == x_str) {
              cur_var = q_var;
              chain_has_add_x = true;
              chain_stmts.push_back(j);
              continue;
            }
          }

          // Check intermediate step 2: `next_var = (cur_var >> s)` or `((cur_var + X) >> s)`
          int64_t step_s = 0;
          if (!q_var.empty() && is_local_reg_var(q_var) && chain_s_val == 0 &&
              q_rhs->Kind() == CExpressionKind::kBinaryExpression) {
            const auto& b = static_cast<const BinaryExpression&>(*q_rhs);
            if (b.Op() == ">>" && is_int_literal(b.Rhs(), &step_s) && step_s >= 1 && step_s <= 30) {
              if (b.Lhs().Kind() == CExpressionKind::kIdentifier &&
                  static_cast<const IdentifierExpression&>(b.Lhs()).Name() == cur_var) {
                cur_var = q_var;
                chain_s_val = step_s;
                chain_stmts.push_back(j);
                continue;
              }
              if (!chain_has_add_x && b.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
                const auto& add_b = static_cast<const BinaryExpression&>(b.Lhs());
                if (add_b.Op() == "+" && add_b.Lhs().Kind() == CExpressionKind::kIdentifier &&
                    static_cast<const IdentifierExpression&>(add_b.Lhs()).Name() == cur_var &&
                    add_b.Rhs().ToString() == x_str) {
                  cur_var = q_var;
                  chain_has_add_x = true;
                  chain_s_val = step_s;
                  chain_stmts.push_back(j);
                  continue;
                }
              }
            }
          }

          // Check final subtraction: `((...) - sign)` or `(((...) - sign) - const)`
          const CExpression* sub_cand = q_rhs;
          const CExpression* outer_sub_rhs = nullptr;
          if (sub_cand->Kind() == CExpressionKind::kBinaryExpression) {
            const auto& b = static_cast<const BinaryExpression&>(*sub_cand);
            if (b.Op() == "-" && b.Lhs().Kind() == CExpressionKind::kBinaryExpression &&
                static_cast<const BinaryExpression&>(b.Lhs()).Op() == "-") {
              outer_sub_rhs = &b.Rhs();
              sub_cand = &b.Lhs();
            }
          }
          if (sub_cand->Kind() == CExpressionKind::kBinaryExpression &&
              static_cast<const BinaryExpression*>(sub_cand)->Op() == "-") {
            const auto& sub_bin = static_cast<const BinaryExpression&>(*sub_cand);

            // Check if sub_bin.Rhs() is `(X >> 31)` or a variable holding `(X >> 31)`
            int sign_stmt_idx = -1;
            std::string sign_var_name;
            bool sign_ok = false;
            if (sub_bin.Rhs().Kind() == CExpressionKind::kIdentifier) {
              sign_var_name = static_cast<const IdentifierExpression&>(sub_bin.Rhs()).Name();
              for (int k = static_cast<int>(j) - 1; k >= 0; --k) {
                std::string sv;
                const CExpression* sr = nullptr;
                if (extract_simple_var_assign(*stmts[k], &sv, &sr) && sv == sign_var_name) {
                  if (is_sign_bit_shift_of(*sr, *x_expr)) {
                    sign_ok = true;
                    sign_stmt_idx = k;
                  }
                  break;
                }
              }
            } else {
              sign_ok = is_sign_bit_shift_of(sub_bin.Rhs(), *x_expr);
            }
            if (sign_ok) {
              const CExpression* inner_part = &sub_bin.Lhs();
              int64_t s_val = chain_s_val;
              if (s_val == 0 && inner_part->Kind() == CExpressionKind::kBinaryExpression) {
                const auto& b = static_cast<const BinaryExpression&>(*inner_part);
                if (b.Op() == ">>" && is_int_literal(b.Rhs(), &s_val) && s_val >= 1 &&
                    s_val <= 30) {
                  inner_part = &b.Lhs();
                }
              }
              bool has_add_x = chain_has_add_x;
              if (!has_add_x && inner_part->Kind() == CExpressionKind::kBinaryExpression) {
                const auto& b = static_cast<const BinaryExpression&>(*inner_part);
                if (b.Op() == "+" && b.Lhs().Kind() == CExpressionKind::kIdentifier &&
                    static_cast<const IdentifierExpression&>(b.Lhs()).Name() == cur_var &&
                    b.Rhs().ToString() == x_str) {
                  has_add_x = true;
                  inner_part = &b.Lhs();
                }
              }
              if (inner_part->Kind() == CExpressionKind::kIdentifier &&
                  static_cast<const IdentifierExpression*>(inner_part)->Name() == cur_var) {
                int64_t div = compute_signed_divisor(magic, s_val, has_add_x);
                if (div > 1) {
                  consumed_magics->insert(magic);
                  auto div_expr = make_signed_div_expr(*x_expr, div);
                  if (outer_sub_rhs != nullptr) {
                    div_expr =
                        CExpression::Binary("-", std::move(div_expr), outer_sub_rhs->Clone());
                  }
                  stmts[j] = CStatement::Expression(
                      CExpression::Assignment("=", assign_j.Lhs().Clone(), std::move(div_expr)));
                  bool erase_sign_stmt = false;
                  if (sign_stmt_idx >= 0 && !sign_var_name.empty()) {
                    bool used_elsewhere = false;
                    for (size_t u = static_cast<size_t>(sign_stmt_idx) + 1; u < stmts.size(); ++u) {
                      if (u == j) {
                        if (count_var_uses_in_stmt(*stmts[u], sign_var_name) > 1) {
                          used_elsewhere = true;
                        }
                        if (q_var == sign_var_name) {
                          break;
                        }
                        continue;
                      }
                      if (count_var_uses_in_stmt(*stmts[u], sign_var_name) > 0) {
                        used_elsewhere = true;
                        break;
                      }
                      std::string ulhs;
                      const CExpression* urhs = nullptr;
                      if (extract_simple_var_assign(*stmts[u], &ulhs, &urhs) &&
                          ulhs == sign_var_name) {
                        break;
                      }
                    }
                    erase_sign_stmt = !used_elsewhere;
                  }
                  std::vector<size_t> to_erase = chain_stmts;
                  if (erase_sign_stmt) to_erase.push_back(static_cast<size_t>(sign_stmt_idx));
                  std::sort(to_erase.rbegin(), to_erase.rend());
                  for (size_t idx : to_erase) {
                    stmts.erase(stmts.begin() + idx);
                    if (idx <= i) --i;
                  }
                  folded_s = true;
                  break;
                }
              }
            }
          }
          if (!q_var.empty() && q_var == cur_var) break;
        }
        if (folded_s) continue;
      }
    }

    // Rewrite single-expression reciprocal divisions inside stmts[i]
    consumed_sign_vars.clear();
    if (stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*stmts[i]);
      stmts[i] = CStatement::Expression(rewrite_div_expr(es.Expression()));
    } else if (stmts[i]->Kind() == CStatementKind::kReturnStatement) {
      const auto& rs = static_cast<const ReturnStatement&>(*stmts[i]);
      if (rs.ReturnValue() != nullptr) {
        stmts[i] = CStatement::Return(rewrite_div_expr(*rs.ReturnValue()));
      }
    } else if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
      const auto& ifs = static_cast<const IfStatement&>(*stmts[i]);
      stmts[i] = CStatement::If(rewrite_div_expr(ifs.Condition()), ifs.ThenBranch().Clone(),
                                ifs.ElseBranch() != nullptr ? ifs.ElseBranch()->Clone() : nullptr);
    }
    if (!consumed_sign_vars.empty()) {
      for (const std::string& sv_name : consumed_sign_vars) {
        for (int k = static_cast<int>(i) - 1; k >= 0; --k) {
          std::string sv;
          const CExpression* sr = nullptr;
          if (extract_simple_var_assign(*stmts[k], &sv, &sr) && sv == sv_name) {
            bool used_elsewhere = false;
            for (size_t u = static_cast<size_t>(k) + 1; u < stmts.size(); ++u) {
              if (count_var_uses_in_stmt(*stmts[u], sv_name) > 0) {
                used_elsewhere = true;
                break;
              }
              std::string ulhs;
              const CExpression* urhs = nullptr;
              if (extract_simple_var_assign(*stmts[u], &ulhs, &urhs) && ulhs == sv_name) {
                break;
              }
            }
            if (!used_elsewhere) {
              stmts.erase(stmts.begin() + k);
              --i;
            }
            break;
          }
        }
      }
    }

    // Recurse into child blocks at statement `i` so `const_map` reflects constants live at `i`.
    if (stmts[i] != nullptr) {
      absl::flat_hash_set<std::string> child_bias_vars = bias_vars;
      absl::erase_if(child_bias_vars, [&](const std::string& bv) {
        return StatementModifiesVariable(*stmts[i], bv);
      });
      switch (stmts[i]->Kind()) {
        case CStatementKind::kCompoundStatement:
          SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
              static_cast<CompoundStatement*>(stmts[i].get()), &const_map, consumed_magics,
              &sign_equiv_map, &child_bias_vars, consumed_bias_vars);
          break;
        case CStatementKind::kIfStatement: {
          auto& ifs = static_cast<IfStatement&>(*stmts[i]);
          if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
            SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
                static_cast<CompoundStatement*>(ifs.MutableThenBranch()), &const_map,
                consumed_magics, &sign_equiv_map, &child_bias_vars, consumed_bias_vars);
          }
          if (ifs.MutableElseBranch() != nullptr &&
              ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
            SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
                static_cast<CompoundStatement*>(ifs.MutableElseBranch()), &const_map,
                consumed_magics, &sign_equiv_map, &child_bias_vars, consumed_bias_vars);
          }
          break;
        }
        case CStatementKind::kWhileStatement: {
          auto& ws = static_cast<WhileStatement&>(*stmts[i]);
          if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
                static_cast<CompoundStatement*>(ws.MutableBody()), &const_map, consumed_magics,
                &sign_equiv_map);
          }
          break;
        }
        case CStatementKind::kDoWhileStatement: {
          auto& dws = static_cast<DoWhileStatement&>(*stmts[i]);
          if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
                static_cast<CompoundStatement*>(dws.MutableBody()), &const_map, consumed_magics,
                &sign_equiv_map);
          }
          break;
        }
        case CStatementKind::kForStatement: {
          auto& fs = static_cast<ForStatement&>(*stmts[i]);
          if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
                static_cast<CompoundStatement*>(fs.MutableBody()), &const_map, consumed_magics,
                &sign_equiv_map);
          }
          break;
        }
        case CStatementKind::kSwitchStatement: {
          auto& sw = static_cast<SwitchStatement&>(*stmts[i]);
          for (auto& sc : sw.MutableCases()) {
            if (sc.body != nullptr) {
              SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
                  sc.body.get(), &const_map, consumed_magics, &sign_equiv_map, &child_bias_vars,
                  consumed_bias_vars);
            }
          }
          break;
        }
        default:
          break;
      }
      if (stmts[i]->Kind() != CStatementKind::kExpressionStatement) {
        for (auto it = bias_vars.begin(); it != bias_vars.end();) {
          if (StatementModifiesVariable(*stmts[i], *it)) {
            bias_vars.erase(it++);
          } else {
            ++it;
          }
        }
      }
    }
  }

  SimplifyModuloAndUnsignedFloatDiamondsInBlockImpl(comp, parent_bias_vars, consumed_bias_vars);

  // Remove dead magic constant assignments (`arg1 = 0x80808081;`, `fp = 0xB21642C9;`, etc.)
  // whose uses were completely eliminated by reciprocal division folding.
  if (!consumed_magics->empty()) {
    for (size_t i = 0; i < stmts.size(); ++i) {
      std::string lhs_var;
      const CExpression* rhs_expr = nullptr;
      int64_t lit_val = 0;
      if (extract_simple_var_assign(*stmts[i], &lhs_var, &rhs_expr) &&
          is_int_literal(*rhs_expr, &lit_val) &&
          consumed_magics->contains(static_cast<uint32_t>(lit_val))) {
        bool used_after = false;
        for (size_t k = i + 1; k < stmts.size(); ++k) {
          if (count_var_uses_in_stmt(*stmts[k], lhs_var) > 0) {
            used_after = true;
            break;
          }
          std::string klhs;
          const CExpression* krhs = nullptr;
          if (extract_simple_var_assign(*stmts[k], &klhs, &krhs) && klhs == lhs_var) {
            break;
          }
        }
        if (!used_after) {
          stmts.erase(stmts.begin() + i);
          --i;
        }
      }
    }
  }

  // Remove dead 2^32 bias double assignments (`f4_d = *(f64*)&D_...;`, `f0_d = 4294967296.0;`,
  // etc.) whose uses were completely eliminated by unsigned float conversion diamond recovery.
  if (!consumed_bias_vars->empty()) {
    for (size_t i = 0; i < stmts.size(); ++i) {
      std::string lhs_var;
      const CExpression* rhs_expr = nullptr;
      if (extract_simple_var_assign(*stmts[i], &lhs_var, &rhs_expr) &&
          consumed_bias_vars->contains(lhs_var) && IsUnsignedFloatBiasExpr(*rhs_expr)) {
        bool used_after = false;
        for (size_t k = i + 1; k < stmts.size(); ++k) {
          if (count_var_uses_in_stmt(*stmts[k], lhs_var) > 0) {
            used_after = true;
            break;
          }
          std::string klhs;
          const CExpression* krhs = nullptr;
          if (extract_simple_var_assign(*stmts[k], &klhs, &krhs) && klhs == lhs_var) {
            break;
          }
        }
        if (!used_after) {
          stmts.erase(stmts.begin() + i);
          --i;
        }
      }
    }
  }
}

void SimplifyModuloAndUnsignedFloatDiamondsInBlockImpl(
    CompoundStatement* comp, const absl::flat_hash_set<std::string>* parent_bias_vars,
    absl::flat_hash_set<std::string>* consumed_bias_vars) {
  if (comp == nullptr) return;

  auto is_int_literal = [](const CExpression& e, int64_t* val_out = nullptr) -> bool {
    if (e.Kind() == CExpressionKind::kIntegerLiteral) {
      if (val_out != nullptr) {
        *val_out = static_cast<const IntegerLiteralExpression&>(e).Value();
      }
      return true;
    }
    return false;
  };

  auto extract_simple_var_assign = ExtractSimpleRegVarAssignment;
  auto count_var_uses_in_stmt = CountRhsVariableUsesInStatement;

  auto& stmts = comp->MutableStatements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*stmts[i]);
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(es.Expression());
        if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& sub_bin = static_cast<const BinaryExpression&>(assign.Rhs());
          if (sub_bin.Op() == "-" && sub_bin.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
            const auto& mul_bin = static_cast<const BinaryExpression&>(sub_bin.Rhs());
            int64_t mul_d = 0;
            if (mul_bin.Op() == "*" && is_int_literal(mul_bin.Rhs(), &mul_d) && mul_d > 1 &&
                mul_bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
              const auto& div_bin = static_cast<const BinaryExpression&>(mul_bin.Lhs());
              int64_t div_d = 0;
              if (div_bin.Op() == "/" && is_int_literal(div_bin.Rhs(), &div_d) && div_d == mul_d) {
                bool is_u_div =
                    static_cast<const IntegerLiteralExpression&>(div_bin.Rhs()).IsUnsigned();
                const CExpression* dx = &div_bin.Lhs();
                if (is_u_div && dx->Kind() == CExpressionKind::kCastExpression &&
                    static_cast<const CastExpression*>(dx)->TargetType().name == "u32") {
                  dx = &static_cast<const CastExpression*>(dx)->Operand();
                }
                if (dx->ToString() == sub_bin.Lhs().ToString()) {
                  auto mod_expr =
                      is_u_div
                          ? CExpression::Binary("%", CExpression::Cast(CType::U32(), dx->Clone()),
                                                CExpression::Integer(div_d, false, true))
                          : CExpression::Binary("%", dx->Clone(), CExpression::Integer(div_d));
                  stmts[i] = CStatement::Expression(
                      CExpression::Assignment("=", assign.Lhs().Clone(), std::move(mod_expr)));
                }
              }
            }
          }
        }
      }
    }
    if (i + 1 >= stmts.size()) continue;
    std::string q_var;
    const CExpression* q_rhs = nullptr;
    if (!extract_simple_var_assign(*stmts[i], &q_var, &q_rhs)) continue;
    if (q_rhs->Kind() != CExpressionKind::kBinaryExpression) continue;
    const auto& div_bin = static_cast<const BinaryExpression&>(*q_rhs);
    if (div_bin.Op() != "/") continue;
    int64_t d_val = 0;
    if (!is_int_literal(div_bin.Rhs(), &d_val) || d_val <= 1) continue;
    bool is_u_div = static_cast<const IntegerLiteralExpression&>(div_bin.Rhs()).IsUnsigned();
    const CExpression* x_expr = &div_bin.Lhs();
    if (is_u_div && x_expr->Kind() == CExpressionKind::kCastExpression &&
        static_cast<const CastExpression*>(x_expr)->TargetType().name == "u32") {
      x_expr = &static_cast<const CastExpression*>(x_expr)->Operand();
    }
    std::string x_str = x_expr->ToString();

    bool folded_mod1 = false;
    for (size_t j = i + 1; j < stmts.size() && j <= i + 8; ++j) {
      if (stmts[j]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es_j = static_cast<const ExpressionStatement&>(*stmts[j]);
        if (es_j.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& assign_j = static_cast<const AssignmentExpression&>(es_j.Expression());
          if (assign_j.Op() == "=") {
            std::string r_var =
                (assign_j.Lhs().Kind() == CExpressionKind::kIdentifier)
                    ? static_cast<const IdentifierExpression&>(assign_j.Lhs()).Name()
                    : "";
            const CExpression* r_rhs = &assign_j.Rhs();
            const CExpression* sub_cand = r_rhs;
            int64_t mask_val = 0;
            bool has_mask = false;
            if (sub_cand->Kind() == CExpressionKind::kBinaryExpression) {
              const auto& b = static_cast<const BinaryExpression&>(*sub_cand);
              if (b.Op() == "&" && is_int_literal(b.Rhs(), &mask_val)) {
                sub_cand = &b.Lhs();
                has_mask = true;
              }
            }
            if (sub_cand->Kind() == CExpressionKind::kBinaryExpression) {
              const auto& sub_bin = static_cast<const BinaryExpression&>(*sub_cand);
              int64_t mul_d = 0;
              if (sub_bin.Op() == "-" && sub_bin.Lhs().ToString() == x_str &&
                  sub_bin.Rhs().Kind() == CExpressionKind::kBinaryExpression &&
                  static_cast<const BinaryExpression&>(sub_bin.Rhs()).Op() == "*" &&
                  static_cast<const BinaryExpression&>(sub_bin.Rhs()).Lhs().ToString() == q_var &&
                  is_int_literal(static_cast<const BinaryExpression&>(sub_bin.Rhs()).Rhs(),
                                 &mul_d) &&
                  mul_d == d_val) {
                bool q_used_elsewhere = false;
                for (size_t k = i + 1; k < stmts.size(); ++k) {
                  if (k == j) continue;
                  if (count_var_uses_in_stmt(*stmts[k], q_var) > 0) {
                    q_used_elsewhere = true;
                    break;
                  }
                  std::string klhs;
                  const CExpression* krhs = nullptr;
                  if (extract_simple_var_assign(*stmts[k], &klhs, &krhs) && klhs == q_var) {
                    break;
                  }
                }
                if (!q_used_elsewhere) {
                  auto mod_expr =
                      is_u_div
                          ? CExpression::Binary("%",
                                                CExpression::Cast(CType::U32(), x_expr->Clone()),
                                                CExpression::Integer(d_val, false, true))
                          : CExpression::Binary("%", x_expr->Clone(), CExpression::Integer(d_val));
                  if (has_mask) {
                    mod_expr = CExpression::Binary("&", std::move(mod_expr),
                                                   CExpression::Integer(mask_val));
                  }
                  stmts[j] = CStatement::Expression(
                      CExpression::Assignment("=", assign_j.Lhs().Clone(), std::move(mod_expr)));
                  stmts.erase(stmts.begin() + i);
                  --i;
                  folded_mod1 = true;
                  break;
                }
              }
            }
            if (!r_var.empty() && (r_var == q_var || r_var == x_str)) break;
            continue;
          }
        }
      }
      if (count_var_uses_in_stmt(*stmts[j], q_var) > 0) {
        break;
      }
    }
    if (folded_mod1) continue;

    std::string p_var, r_var;
    const CExpression* p_rhs = nullptr;
    const CExpression* r_rhs = nullptr;
    if (i + 2 < stmts.size() && extract_simple_var_assign(*stmts[i + 1], &p_var, &p_rhs) &&
        extract_simple_var_assign(*stmts[i + 2], &r_var, &r_rhs)) {
      int64_t mul_d = 0;
      if (p_rhs->Kind() == CExpressionKind::kBinaryExpression &&
          static_cast<const BinaryExpression*>(p_rhs)->Op() == "*" &&
          static_cast<const BinaryExpression*>(p_rhs)->Lhs().ToString() == q_var &&
          is_int_literal(static_cast<const BinaryExpression*>(p_rhs)->Rhs(), &mul_d) &&
          mul_d == d_val && r_rhs->Kind() == CExpressionKind::kBinaryExpression &&
          static_cast<const BinaryExpression*>(r_rhs)->Op() == "-" &&
          static_cast<const BinaryExpression*>(r_rhs)->Lhs().ToString() == x_str &&
          static_cast<const BinaryExpression*>(r_rhs)->Rhs().ToString() == p_var) {
        bool qp_used_later = false;
        for (size_t k = i + 3; k < stmts.size(); ++k) {
          if ((r_var != q_var && count_var_uses_in_stmt(*stmts[k], q_var) > 0) ||
              (r_var != p_var && count_var_uses_in_stmt(*stmts[k], p_var) > 0)) {
            qp_used_later = true;
            break;
          }
        }
        if (!qp_used_later) {
          auto mod_expr =
              is_u_div ? CExpression::Binary("%", CExpression::Cast(CType::U32(), x_expr->Clone()),
                                             CExpression::Integer(d_val, false, true))
                       : CExpression::Binary("%", x_expr->Clone(), CExpression::Integer(d_val));
          stmts[i + 2] = CStatement::Expression(
              CExpression::Assignment("=", CExpression::Identifier(r_var), std::move(mod_expr)));
          stmts.erase(stmts.begin() + i, stmts.begin() + i + 2);
          --i;
          continue;
        }
      }
    }
  }

  auto is_f32_cast_of = [](const CExpression& expr, std::string_view target_var) -> bool {
    if (expr.Kind() == CExpressionKind::kCastExpression) {
      const auto& ce = static_cast<const CastExpression&>(expr);
      return ce.TargetType().name == "f32" && ce.TargetType().pointer_depth == 0 &&
             ce.Operand().Kind() == CExpressionKind::kIdentifier &&
             static_cast<const IdentifierExpression&>(ce.Operand()).Name() == target_var;
    }
    return false;
  };

  auto is_f64_cast_of = [](const CExpression& expr, std::string_view target_var) -> bool {
    if (expr.Kind() == CExpressionKind::kCastExpression) {
      const auto& ce = static_cast<const CastExpression&>(expr);
      return ce.TargetType().name == "f64" && ce.TargetType().pointer_depth == 0 &&
             ce.Operand().Kind() == CExpressionKind::kIdentifier &&
             static_cast<const IdentifierExpression&>(ce.Operand()).Name() == target_var;
    }
    return false;
  };

  auto is_id_of = [](const CExpression& expr, std::string_view target_var) -> bool {
    return expr.Kind() == CExpressionKind::kIdentifier &&
           static_cast<const IdentifierExpression&>(expr).Name() == target_var;
  };

  auto rewrite_unsigned_float_var_in_expr =
      [&](const CExpression& root_expr, std::string_view target_var, std::string_view int_var,
          int* out_f32_uses, int* out_f64_uses) -> std::unique_ptr<CExpression> {
    std::function<std::unique_ptr<CExpression>(const CExpression&)> walk =
        [&](const CExpression& e) -> std::unique_ptr<CExpression> {
      if (e.Kind() == CExpressionKind::kCastExpression) {
        const auto& ce = static_cast<const CastExpression&>(e);
        if (ce.TargetType().pointer_depth == 0 && is_id_of(ce.Operand(), target_var)) {
          if (ce.TargetType().name == "f32") {
            if (out_f32_uses != nullptr) ++(*out_f32_uses);
            return CExpression::Cast(
                CType::F32(),
                CExpression::Cast(CType::U32(), CExpression::Identifier(std::string(int_var))));
          }
          if (ce.TargetType().name == "f64") {
            if (out_f64_uses != nullptr) ++(*out_f64_uses);
            return CExpression::Cast(
                CType::F64(),
                CExpression::Cast(CType::U32(), CExpression::Identifier(std::string(int_var))));
          }
        }
        return CExpression::Cast(ce.TargetType(), walk(ce.Operand()));
      }
      if (is_id_of(e, target_var)) {
        if (out_f64_uses != nullptr) ++(*out_f64_uses);
        return CExpression::Cast(
            CType::F64(),
            CExpression::Cast(CType::U32(), CExpression::Identifier(std::string(int_var))));
      }
      switch (e.Kind()) {
        case CExpressionKind::kBinaryExpression: {
          const auto& b = static_cast<const BinaryExpression&>(e);
          return CExpression::Binary(b.Op(), walk(b.Lhs()), walk(b.Rhs()));
        }
        case CExpressionKind::kUnaryExpression: {
          const auto& u = static_cast<const UnaryExpression&>(e);
          return CExpression::Unary(u.Op(), walk(u.Operand()), u.IsPrefix());
        }
        case CExpressionKind::kAssignmentExpression: {
          const auto& a = static_cast<const AssignmentExpression&>(e);
          auto new_lhs = is_id_of(a.Lhs(), target_var) ? a.Lhs().Clone() : walk(a.Lhs());
          return CExpression::Assignment(a.Op(), std::move(new_lhs), walk(a.Rhs()));
        }
        case CExpressionKind::kCallExpression: {
          const auto& c = static_cast<const CallExpression&>(e);
          std::vector<std::unique_ptr<CExpression>> args;
          args.reserve(c.Arguments().size());
          for (const auto& arg : c.Arguments()) {
            args.push_back(walk(*arg));
          }
          return CExpression::Call(c.Callee().Clone(), std::move(args));
        }
        case CExpressionKind::kTernaryExpression: {
          const auto& t = static_cast<const TernaryExpression&>(e);
          return CExpression::Ternary(walk(t.Condition()), walk(t.TrueExpression()),
                                      walk(t.FalseExpression()));
        }
        case CExpressionKind::kArrayIndexExpression: {
          const auto& ai = static_cast<const ArrayIndexExpression&>(e);
          return CExpression::ArrayIndex(walk(ai.Array()), walk(ai.Index()));
        }
        case CExpressionKind::kMemberAccessExpression: {
          const auto& ma = static_cast<const MemberAccessExpression&>(e);
          return CExpression::MemberAccess(walk(ma.Object()), ma.MemberName(), ma.IsArrow());
        }
        default:
          return e.Clone();
      }
    };
    return walk(root_expr);
  };

  auto is_var_dead_after_index = [&](size_t idx, std::string_view var,
                                     bool allow_end_of_block) -> bool {
    for (size_t u = idx + 1; u < stmts.size(); ++u) {
      std::string ulhs;
      const CExpression* urhs = nullptr;
      if (extract_simple_var_assign(*stmts[u], &ulhs, &urhs) && ulhs == var &&
          CountVariableReadUsesInExpression(*urhs, var) == 0) {
        return true;
      }
      if (stmts[u]->Kind() == CStatementKind::kReturnStatement) {
        return count_var_uses_in_stmt(*stmts[u], var) == 0;
      }
      if (count_var_uses_in_stmt(*stmts[u], var) > 0 || StatementContainsLabel(*stmts[u]) ||
          StatementContainsControlFlowJump(*stmts[u]) || StatementContainsLoop(*stmts[u])) {
        return false;
      }
    }
    return allow_end_of_block;
  };

  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->Kind() != CStatementKind::kIfStatement) continue;
    const auto& ifs = static_cast<const IfStatement&>(*stmts[i]);
    if (ifs.Condition().Kind() != CExpressionKind::kBinaryExpression) continue;
    const auto& cond_bin = static_cast<const BinaryExpression&>(ifs.Condition());
    int64_t zero_val = -1;
    std::string int_var;
    bool neg_in_then = false;
    if (cond_bin.Op() == "<" && cond_bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
        is_int_literal(cond_bin.Rhs(), &zero_val) && zero_val == 0) {
      int_var = static_cast<const IdentifierExpression&>(cond_bin.Lhs()).Name();
      neg_in_then = true;
    } else if (cond_bin.Op() == ">" && cond_bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
               is_int_literal(cond_bin.Lhs(), &zero_val) && zero_val == 0) {
      int_var = static_cast<const IdentifierExpression&>(cond_bin.Rhs()).Name();
      neg_in_then = true;
    } else if (cond_bin.Op() == ">=" && cond_bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
               is_int_literal(cond_bin.Rhs(), &zero_val) && zero_val == 0 &&
               ifs.ElseBranch() != nullptr) {
      int_var = static_cast<const IdentifierExpression&>(cond_bin.Lhs()).Name();
      neg_in_then = false;
    } else if (cond_bin.Op() == "<=" && cond_bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
               is_int_literal(cond_bin.Lhs(), &zero_val) && zero_val == 0 &&
               ifs.ElseBranch() != nullptr) {
      int_var = static_cast<const IdentifierExpression&>(cond_bin.Rhs()).Name();
      neg_in_then = false;
    } else {
      continue;
    }

    const CStatement* neg_branch = neg_in_then ? &ifs.ThenBranch() : ifs.ElseBranch();
    const CStatement* pos_branch = neg_in_then ? ifs.ElseBranch() : &ifs.ThenBranch();
    if (neg_branch == nullptr || neg_branch->Kind() != CStatementKind::kCompoundStatement) continue;
    if (pos_branch != nullptr && pos_branch->Kind() != CStatementKind::kCompoundStatement) continue;
    const auto& neg_comp = static_cast<const CompoundStatement&>(*neg_branch);
    const auto* pos_comp = static_cast<const CompoundStatement*>(pos_branch);
    const auto& neg_stmts = neg_comp.Statements();
    if (neg_stmts.empty() || neg_stmts.size() > 6) continue;

    absl::flat_hash_set<std::string> active_bias_vars;
    if (parent_bias_vars != nullptr) {
      active_bias_vars = *parent_bias_vars;
    }
    for (size_t b = 0; b < i; ++b) {
      std::string blhs;
      const CExpression* brhs = nullptr;
      if (extract_simple_var_assign(*stmts[b], &blhs, &brhs)) {
        if (absl::EndsWith(blhs, "_d")) {
          if (IsUnsignedFloatBiasExpr(*brhs, &active_bias_vars)) {
            active_bias_vars.insert(blhs);
          } else {
            active_bias_vars.erase(blhs);
          }
        }
      } else {
        for (auto it = active_bias_vars.begin(); it != active_bias_vars.end();) {
          if (StatementModifiesVariable(*stmts[b], *it)) {
            active_bias_vars.erase(it++);
          } else {
            ++it;
          }
        }
      }
    }

    absl::flat_hash_set<std::string> branch_bias_vars = active_bias_vars;
    std::vector<std::string> branch_setup_vars;
    int add_stmt_idx = -1;
    std::string then_lhs;
    std::string used_bias_var;
    bool valid_setup = true;
    for (size_t s_idx = 0; s_idx < neg_stmts.size(); ++s_idx) {
      std::string slhs;
      const CExpression* srhs = nullptr;
      if (!extract_simple_var_assign(*neg_stmts[s_idx], &slhs, &srhs)) {
        valid_setup = false;
        break;
      }
      if (absl::EndsWith(slhs, "_d") && srhs->Kind() == CExpressionKind::kBinaryExpression) {
        const auto& add_bin = static_cast<const BinaryExpression&>(*srhs);
        if (add_bin.Op() == "+") {
          std::string cand_bias_var;
          if (is_id_of(add_bin.Lhs(), slhs) &&
              IsUnsignedFloatBiasExpr(add_bin.Rhs(), &branch_bias_vars, &cand_bias_var)) {
            add_stmt_idx = static_cast<int>(s_idx);
            then_lhs = slhs;
            used_bias_var = cand_bias_var;
            break;
          }
          if (is_id_of(add_bin.Rhs(), slhs) &&
              IsUnsignedFloatBiasExpr(add_bin.Lhs(), &branch_bias_vars, &cand_bias_var)) {
            add_stmt_idx = static_cast<int>(s_idx);
            then_lhs = slhs;
            used_bias_var = cand_bias_var;
            break;
          }
        }
      }
      if (!IsPureExpression(*srhs) || slhs == int_var) {
        valid_setup = false;
        break;
      }
      branch_setup_vars.push_back(slhs);
      if (absl::EndsWith(slhs, "_d")) {
        if (IsUnsignedFloatBiasExpr(*srhs, &branch_bias_vars)) {
          branch_bias_vars.insert(slhs);
        } else {
          branch_bias_vars.erase(slhs);
        }
      }
    }
    if (!valid_setup || add_stmt_idx < 0 || then_lhs.empty()) continue;

    bool setup_clobbers_then_lhs = false;
    for (const std::string& sv : branch_setup_vars) {
      if (sv == then_lhs || absl::StrCat(sv, "_d") == then_lhs) {
        setup_clobbers_then_lhs = true;
        break;
      }
    }
    if (setup_clobbers_then_lhs) continue;

    size_t rem_neg = neg_stmts.size() - static_cast<size_t>(add_stmt_idx + 1);
    size_t rem_pos = (pos_comp != nullptr) ? pos_comp->Statements().size() : 0;
    std::string branch_narrow_dst;
    std::string branch_copy_f64_dst;
    bool then_lhs_restored_in_branch = false;
    if (rem_neg == 0) {
      if (rem_pos != 0) continue;
    } else if ((rem_neg == 1 || rem_neg == 2) && rem_pos == 1) {
      std::string tlhs2, elhs1;
      const CExpression* trhs2 = nullptr;
      const CExpression* erhs1 = nullptr;
      if (!extract_simple_var_assign(*neg_stmts[add_stmt_idx + 1], &tlhs2, &trhs2) ||
          !extract_simple_var_assign(*pos_comp->Statements()[0], &elhs1, &erhs1) ||
          tlhs2 != elhs1) {
        continue;
      }
      if (is_f32_cast_of(*trhs2, then_lhs) && is_f32_cast_of(*erhs1, then_lhs)) {
        branch_narrow_dst = tlhs2;
      } else if (absl::EndsWith(tlhs2, "_d") && is_id_of(*trhs2, then_lhs) &&
                 is_id_of(*erhs1, then_lhs)) {
        branch_copy_f64_dst = tlhs2;
      } else {
        continue;
      }
      if (rem_neg == 2) {
        std::string tlhs3;
        const CExpression* trhs3 = nullptr;
        if (!extract_simple_var_assign(*neg_stmts[add_stmt_idx + 2], &tlhs3, &trhs3) ||
            tlhs3 != then_lhs || tlhs2 == then_lhs || !is_f64_cast_of(*trhs3, int_var)) {
          continue;
        }
        then_lhs_restored_in_branch = true;
      }
    } else {
      continue;
    }

    bool setup_vars_dead = true;
    for (const std::string& sv : branch_setup_vars) {
      if (!branch_narrow_dst.empty() &&
          (sv == branch_narrow_dst || sv == absl::StrCat(branch_narrow_dst, "_d"))) {
        continue;
      }
      if (!branch_copy_f64_dst.empty() && sv == branch_copy_f64_dst) {
        continue;
      }
      if (!is_var_dead_after_index(
              i, sv, /*allow_end_of_block=*/sv != "f0" && sv != "f0_d" && sv != "v0")) {
        setup_vars_dead = false;
        break;
      }
    }
    if (!setup_vars_dead) continue;

    int cvt_idx = -1;
    for (int k = static_cast<int>(i) - 1; k >= 0 && k >= static_cast<int>(i) - 6; --k) {
      std::string clhs;
      const CExpression* crhs = nullptr;
      if (extract_simple_var_assign(*stmts[k], &clhs, &crhs) && clhs == then_lhs) {
        if (is_f64_cast_of(*crhs, int_var)) {
          cvt_idx = k;
        }
        break;
      }
      if (StatementModifiesVariable(*stmts[k], then_lhs) ||
          StatementModifiesVariable(*stmts[k], int_var) ||
          count_var_uses_in_stmt(*stmts[k], then_lhs) > 0 || StatementContainsLabel(*stmts[k]) ||
          StatementContainsControlFlowJump(*stmts[k]) || StatementContainsCall(*stmts[k]) ||
          StatementContainsLoop(*stmts[k])) {
        break;
      }
    }
    if (cvt_idx < 0) continue;

    if (!used_bias_var.empty() && consumed_bias_vars != nullptr &&
        active_bias_vars.contains(used_bias_var)) {
      consumed_bias_vars->insert(used_bias_var);
    }

    if (!branch_narrow_dst.empty()) {
      auto f32_u32_stmt = CStatement::Expression(CExpression::Assignment(
          "=", CExpression::Identifier(branch_narrow_dst),
          CExpression::Cast(CType::F32(),
                            CExpression::Cast(CType::U32(), CExpression::Identifier(int_var)))));
      if (then_lhs_restored_in_branch &&
          !is_var_dead_after_index(i, then_lhs, /*allow_end_of_block=*/false)) {
        stmts[i] = std::move(f32_u32_stmt);
        continue;
      }
      bool dst_touched_between = false;
      for (size_t p = static_cast<size_t>(cvt_idx) + 1; p < i; ++p) {
        if (count_var_uses_in_stmt(*stmts[p], branch_narrow_dst) > 0 ||
            StatementModifiesVariable(*stmts[p], branch_narrow_dst)) {
          dst_touched_between = true;
          break;
        }
      }
      if (!dst_touched_between) {
        stmts[cvt_idx] = std::move(f32_u32_stmt);
        stmts.erase(stmts.begin() + i);
        --i;
      } else {
        stmts[i] = std::move(f32_u32_stmt);
        stmts.erase(stmts.begin() + cvt_idx);
        --i;
      }
      continue;
    }

    std::string target_f64_var = branch_copy_f64_dst.empty() ? then_lhs : branch_copy_f64_dst;
    auto u32_cast = CExpression::Cast(CType::U32(), CExpression::Identifier(int_var));
    auto f64_u32_stmt = CStatement::Expression(
        CExpression::Assignment("=", CExpression::Identifier(target_f64_var),
                                CExpression::Cast(CType::F64(), std::move(u32_cast))));
    if (then_lhs_restored_in_branch &&
        !is_var_dead_after_index(i, then_lhs, /*allow_end_of_block=*/false)) {
      stmts[i] = std::move(f64_u32_stmt);
      cvt_idx = static_cast<int>(i);
    } else {
      bool dst_touched_between = false;
      if (target_f64_var != then_lhs) {
        for (size_t p = static_cast<size_t>(cvt_idx) + 1; p < i; ++p) {
          if (count_var_uses_in_stmt(*stmts[p], target_f64_var) > 0 ||
              StatementModifiesVariable(*stmts[p], target_f64_var)) {
            dst_touched_between = true;
            break;
          }
        }
      }
      if (!dst_touched_between) {
        stmts[cvt_idx] = std::move(f64_u32_stmt);
        stmts.erase(stmts.begin() + i);
        --i;
      } else {
        stmts[i] = std::move(f64_u32_stmt);
        stmts.erase(stmts.begin() + cvt_idx);
        --i;
        cvt_idx = static_cast<int>(i);
      }
    }

    for (size_t m = static_cast<size_t>(cvt_idx) + 1;
         m < stmts.size() && m <= static_cast<size_t>(cvt_idx) + 6; ++m) {
      int uses_in_m = count_var_uses_in_stmt(*stmts[m], target_f64_var);
      if (uses_in_m == 0) {
        if (StatementModifiesVariable(*stmts[m], target_f64_var) ||
            StatementModifiesVariable(*stmts[m], int_var) || StatementContainsLabel(*stmts[m]) ||
            StatementContainsControlFlowJump(*stmts[m]) || StatementContainsCall(*stmts[m]) ||
            StatementContainsLoop(*stmts[m])) {
          break;
        }
        continue;
      }
      if (uses_in_m != 1 || StatementTakesAddressOf(*stmts[m], target_f64_var)) {
        break;
      }

      if (stmts[m]->Kind() == CStatementKind::kReturnStatement) {
        const auto& rs = static_cast<const ReturnStatement&>(*stmts[m]);
        if (rs.ReturnValue() != nullptr) {
          int f32_uses = 0;
          int f64_uses = 0;
          auto new_ret = rewrite_unsigned_float_var_in_expr(*rs.ReturnValue(), target_f64_var,
                                                            int_var, &f32_uses, &f64_uses);
          if (f32_uses + f64_uses == 1) {
            stmts[m] = CStatement::Return(std::move(new_ret));
            stmts.erase(stmts.begin() + cvt_idx);
            if (m <= i) --i;
          }
        }
        break;
      }

      std::string nlhs;
      const CExpression* nrhs = nullptr;
      if (extract_simple_var_assign(*stmts[m], &nlhs, &nrhs) &&
          is_f32_cast_of(*nrhs, target_f64_var)) {
        if (nlhs == target_f64_var || absl::StrCat(nlhs, "_d") == target_f64_var ||
            is_var_dead_after_index(m, target_f64_var, /*allow_end_of_block=*/true)) {
          bool f_dst_touched_between = false;
          for (size_t p = static_cast<size_t>(cvt_idx) + 1; p < m; ++p) {
            if (count_var_uses_in_stmt(*stmts[p], nlhs) > 0 ||
                StatementModifiesVariable(*stmts[p], nlhs)) {
              f_dst_touched_between = true;
              break;
            }
          }
          auto f32_u32_stmt = CStatement::Expression(CExpression::Assignment(
              "=", CExpression::Identifier(nlhs),
              CExpression::Cast(
                  CType::F32(),
                  CExpression::Cast(CType::U32(), CExpression::Identifier(int_var)))));
          size_t narrow_idx = 0;
          if (!f_dst_touched_between) {
            stmts[cvt_idx] = std::move(f32_u32_stmt);
            stmts.erase(stmts.begin() + m);
            narrow_idx = static_cast<size_t>(cvt_idx);
          } else {
            stmts[m] = std::move(f32_u32_stmt);
            stmts.erase(stmts.begin() + cvt_idx);
            narrow_idx = m - 1;
          }
          if (m <= i) --i;
          if (narrow_idx + 1 < stmts.size() &&
              stmts[narrow_idx + 1]->Kind() == CStatementKind::kReturnStatement) {
            const auto& next_rs = static_cast<const ReturnStatement&>(*stmts[narrow_idx + 1]);
            if (next_rs.ReturnValue() != nullptr &&
                CountVariableReadUsesInExpression(*next_rs.ReturnValue(), nlhs) == 1 &&
                !ExpressionTakesAddressOf(*next_rs.ReturnValue(), nlhs)) {
              const auto& narrow_assign = static_cast<const AssignmentExpression&>(
                  static_cast<const ExpressionStatement&>(*stmts[narrow_idx]).Expression());
              stmts[narrow_idx + 1] = CStatement::Return(
                  SubstituteIdentifier(*next_rs.ReturnValue(), nlhs, narrow_assign.Rhs()));
              stmts.erase(stmts.begin() + narrow_idx);
              if (narrow_idx <= i && i > 0) --i;
            }
          }
        }
        break;
      }

      if (stmts[m]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es_m = static_cast<const ExpressionStatement&>(*stmts[m]);
        if (es_m.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a_m = static_cast<const AssignmentExpression&>(es_m.Expression());
          if (a_m.Op() == "=" &&
              CountVariableReadUsesInExpression(a_m.Lhs(), target_f64_var) == 0 &&
              CountVariableReadUsesInExpression(a_m.Rhs(), target_f64_var) == 1) {
            int f32_uses = 0;
            int f64_uses = 0;
            auto new_rhs = rewrite_unsigned_float_var_in_expr(a_m.Rhs(), target_f64_var, int_var,
                                                              &f32_uses, &f64_uses);
            bool overwrites_target = is_id_of(a_m.Lhs(), target_f64_var);
            bool allow_eof = (f32_uses == 1) || (target_f64_var != "f0_d");
            if (f32_uses + f64_uses == 1 &&
                (overwrites_target || is_var_dead_after_index(m, target_f64_var, allow_eof))) {
              if (!nlhs.empty() && is_id_of(a_m.Rhs(), target_f64_var)) {
                bool dst_touched = false;
                for (size_t p = static_cast<size_t>(cvt_idx) + 1; p < m; ++p) {
                  if (count_var_uses_in_stmt(*stmts[p], nlhs) > 0 ||
                      StatementModifiesVariable(*stmts[p], nlhs)) {
                    dst_touched = true;
                    break;
                  }
                }
                auto copy_stmt = CStatement::Expression(
                    CExpression::Assignment("=", a_m.Lhs().Clone(), std::move(new_rhs)));
                size_t copy_idx = 0;
                if (!dst_touched) {
                  stmts[cvt_idx] = std::move(copy_stmt);
                  stmts.erase(stmts.begin() + m);
                  copy_idx = static_cast<size_t>(cvt_idx);
                } else {
                  stmts[m] = std::move(copy_stmt);
                  stmts.erase(stmts.begin() + cvt_idx);
                  copy_idx = m - 1;
                }
                if (m <= i) --i;
                if (copy_idx + 1 < stmts.size() &&
                    stmts[copy_idx + 1]->Kind() == CStatementKind::kReturnStatement) {
                  const auto& next_rs = static_cast<const ReturnStatement&>(*stmts[copy_idx + 1]);
                  if (next_rs.ReturnValue() != nullptr &&
                      CountVariableReadUsesInExpression(*next_rs.ReturnValue(), nlhs) == 1 &&
                      !ExpressionTakesAddressOf(*next_rs.ReturnValue(), nlhs)) {
                    const auto& copy_assign = static_cast<const AssignmentExpression&>(
                        static_cast<const ExpressionStatement&>(*stmts[copy_idx]).Expression());
                    stmts[copy_idx + 1] = CStatement::Return(
                        SubstituteIdentifier(*next_rs.ReturnValue(), nlhs, copy_assign.Rhs()));
                    stmts.erase(stmts.begin() + copy_idx);
                    if (copy_idx <= i && i > 0) --i;
                  }
                }
              } else {
                stmts[m] = CStatement::Expression(
                    CExpression::Assignment("=", a_m.Lhs().Clone(), std::move(new_rhs)));
                stmts.erase(stmts.begin() + cvt_idx);
                if (m <= i) --i;
              }
            }
          }
        } else if (es_m.Expression().Kind() == CExpressionKind::kCallExpression) {
          int f32_uses = 0;
          int f64_uses = 0;
          auto new_call = rewrite_unsigned_float_var_in_expr(es_m.Expression(), target_f64_var,
                                                             int_var, &f32_uses, &f64_uses);
          if (f32_uses + f64_uses == 1 &&
              is_var_dead_after_index(m, target_f64_var,
                                      /*allow_end_of_block=*/target_f64_var != "f0_d")) {
            stmts[m] = CStatement::Expression(std::move(new_call));
            stmts.erase(stmts.begin() + cvt_idx);
            if (m <= i) --i;
          }
        }
        break;
      }

      if (stmts[m]->Kind() == CStatementKind::kIfStatement) {
        const auto& ifs_m = static_cast<const IfStatement&>(*stmts[m]);
        if (CountVariableReadUsesInExpression(ifs_m.Condition(), target_f64_var) == 1 &&
            count_var_uses_in_stmt(ifs_m.ThenBranch(), target_f64_var) == 0 &&
            (ifs_m.ElseBranch() == nullptr ||
             count_var_uses_in_stmt(*ifs_m.ElseBranch(), target_f64_var) == 0)) {
          int f32_uses = 0;
          int f64_uses = 0;
          auto new_cond = rewrite_unsigned_float_var_in_expr(ifs_m.Condition(), target_f64_var,
                                                             int_var, &f32_uses, &f64_uses);
          if (f32_uses + f64_uses == 1 &&
              is_var_dead_after_index(m, target_f64_var,
                                      /*allow_end_of_block=*/target_f64_var != "f0_d")) {
            stmts[m] = CStatement::If(
                std::move(new_cond), ifs_m.ThenBranch().Clone(),
                ifs_m.ElseBranch() != nullptr ? ifs_m.ElseBranch()->Clone() : nullptr);
            stmts.erase(stmts.begin() + cvt_idx);
            if (m <= i) --i;
          }
        }
        break;
      }
      break;
    }
  }
}

class ShiftAddMultiplicationMutator : public AstMutator {
 public:
  void MutateVariableDeclarationStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                          VariableDeclarationStatement* /*decl_stmt*/) override {}

  void MutateExpression(std::unique_ptr<CExpression>* expr) override {
    if (expr == nullptr || *expr == nullptr) return;
    *expr = SimplifyShiftAddExpression(**expr);
  }
};

const CExpression* ExtractRelationalConditionFromStoreFlag(
    const CExpression& expr, std::unique_ptr<CExpression>* out_normalized) {
  const CExpression* cur = &expr;
  while (cur->Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(*cur);
    if (cast.TargetType().pointer_depth != 0 ||
        (cast.TargetType().name != "s32" && cast.TargetType().name != "u32" &&
         cast.TargetType().name != "s64" && cast.TargetType().name != "u64")) {
      return nullptr;
    }
    cur = &cast.Operand();
  }
  if (cur->Kind() != CExpressionKind::kBinaryExpression) {
    return nullptr;
  }
  const auto& bin = static_cast<const BinaryExpression&>(*cur);
  std::string_view op = bin.Op();
  if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" && op != ">=") {
    return nullptr;
  }
  // Normalize `((u32)0 < (u32)A)` -> `(A != 0)` and `((u32)(A ^ B) < 1)` -> `(A == B)`.
  if (out_normalized != nullptr && op == "<") {
    const CExpression* lhs_u = &bin.Lhs();
    const CExpression* rhs_u = &bin.Rhs();
    if (lhs_u->Kind() == CExpressionKind::kCastExpression &&
        static_cast<const CastExpression&>(*lhs_u).TargetType().name == "u32") {
      lhs_u = &static_cast<const CastExpression&>(*lhs_u).Operand();
    }
    if (rhs_u->Kind() == CExpressionKind::kCastExpression &&
        static_cast<const CastExpression&>(*rhs_u).TargetType().name == "u32") {
      rhs_u = &static_cast<const CastExpression&>(*rhs_u).Operand();
    }
    if (lhs_u->Kind() == CExpressionKind::kIntegerLiteral &&
        static_cast<const IntegerLiteralExpression&>(*lhs_u).Value() == 0) {
      *out_normalized = CExpression::Binary("!=", rhs_u->Clone(), CExpression::Integer(0));
    } else if (rhs_u->Kind() == CExpressionKind::kIntegerLiteral &&
               static_cast<const IntegerLiteralExpression&>(*rhs_u).Value() == 1) {
      if (lhs_u->Kind() == CExpressionKind::kBinaryExpression &&
          static_cast<const BinaryExpression&>(*lhs_u).Op() == "^") {
        const auto& xor_bin = static_cast<const BinaryExpression&>(*lhs_u);
        *out_normalized = CExpression::Binary("==", xor_bin.Lhs().Clone(), xor_bin.Rhs().Clone());
      } else {
        *out_normalized = CExpression::Binary("==", lhs_u->Clone(), CExpression::Integer(0));
      }
    }
  }
  return cur;
}

bool IsEligibleStoreFlagConditionalTarget(const CExpression& lhs) {
  if (!IsPureExpression(lhs)) {
    return false;
  }
  if (lhs.Kind() == CExpressionKind::kIdentifier) {
    std::string_view name = static_cast<const IdentifierExpression&>(lhs).Name();
    return !(
        name == "sp" || name == "ra" || name == "lo" || name == "hi" || name == "fcr31" ||
        name == "fcond" ||
        (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))));
  }
  return lhs.Kind() == CExpressionKind::kUnaryExpression ||
         lhs.Kind() == CExpressionKind::kArrayIndexExpression ||
         lhs.Kind() == CExpressionKind::kMemberAccessExpression;
}

class StoreFlagConditionalRecoveryMutator : public AstMutator {
 public:
  explicit StoreFlagConditionalRecoveryMutator(const CompoundStatement* root_body = nullptr)
      : root_body_(root_body) {}

  void MutateVariableDeclarationStatement(std::unique_ptr<CStatement>* /*stmt*/,
                                          VariableDeclarationStatement* /*decl_stmt*/) override {}

  void MutateCompoundStatement(CompoundStatement* comp) override {
    if (comp == nullptr) return;
    if (root_body_ == nullptr) {
      root_body_ = comp;
    }
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i + 1 < stmts.size(); ++i) {
      if (stmts[i] == nullptr || stmts[i + 1] == nullptr) continue;
      if (stmts[i]->Kind() != CStatementKind::kExpressionStatement ||
          stmts[i + 1]->Kind() != CStatementKind::kExpressionStatement) {
        continue;
      }
      const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
      const auto& es1 = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
      if (es0.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
          es1.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
        continue;
      }
      const auto& a0 = static_cast<const AssignmentExpression&>(es0.Expression());
      const auto& a1 = static_cast<const AssignmentExpression&>(es1.Expression());
      if (a0.Op() != "=" || a0.Lhs().Kind() != CExpressionKind::kIdentifier ||
          !IsEligibleStoreFlagConditionalTarget(a1.Lhs())) {
        continue;
      }
      std::string tmp_var(static_cast<const IdentifierExpression&>(a0.Lhs()).Name());
      std::unique_ptr<CExpression> normalized_cond;
      const CExpression* cond_expr =
          ExtractRelationalConditionFromStoreFlag(a0.Rhs(), &normalized_cond);
      if (cond_expr == nullptr || !IsPureExpression(*cond_expr) ||
          ExpressionUsesVariable(a1.Lhs(), tmp_var)) {
        continue;
      }
      if (a1.Lhs().Kind() == CExpressionKind::kIdentifier &&
          (static_cast<const IdentifierExpression&>(a1.Lhs()).Name() == tmp_var ||
           ExpressionUsesVariable(*cond_expr,
                                  static_cast<const IdentifierExpression&>(a1.Lhs()).Name()))) {
        continue;
      }
      std::string step_op;
      if ((a1.Op() == "+=" || a1.Op() == "-=") && a1.Rhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(a1.Rhs()).Name() == tmp_var) {
        step_op = (a1.Op() == "+=") ? "+" : "-";
      } else if (a1.Op() == "=" && a1.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& rhs_bin = static_cast<const BinaryExpression&>(a1.Rhs());
        if (rhs_bin.Op() == "+" || rhs_bin.Op() == "-") {
          if (rhs_bin.Lhs().ToString() == a1.Lhs().ToString() &&
              rhs_bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(rhs_bin.Rhs()).Name() == tmp_var) {
            step_op = rhs_bin.Op();
          } else if (rhs_bin.Op() == "+" && rhs_bin.Rhs().ToString() == a1.Lhs().ToString() &&
                     rhs_bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
                     static_cast<const IdentifierExpression&>(rhs_bin.Lhs()).Name() == tmp_var) {
            step_op = "+";
          }
        }
      }
      if (step_op.empty()) continue;

      bool dead_after = true;
      for (size_t k = i + 2; k < stmts.size(); ++k) {
        if (stmts[k] != nullptr && StatementReferencesVariable(*stmts[k], tmp_var)) {
          dead_after = false;
          break;
        }
      }
      if (!dead_after) continue;
      if (root_body_ != nullptr &&
          CountIdentifierOccurrencesInStatement(*root_body_, tmp_var) > 2) {
        continue;
      }

      std::unique_ptr<CExpression> final_cond =
          normalized_cond != nullptr ? std::move(normalized_cond) : cond_expr->Clone();
      auto then_block = std::make_unique<CompoundStatement>();
      then_block->AddStatement(CStatement::Expression(CExpression::Assignment(
          "=", a1.Lhs().Clone(),
          CExpression::Binary(step_op, a1.Lhs().Clone(), CExpression::Integer(1)))));
      stmts[i + 1] = CStatement::If(std::move(final_cond), std::move(then_block));
      stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
      --i;
      MarkChanged();
    }
    AstMutator::MutateCompoundStatement(comp);
  }

  void MutateExpressionStatement(std::unique_ptr<CStatement>* stmt,
                                 ExpressionStatement* expr_stmt) override {
    if (stmt == nullptr || expr_stmt == nullptr) return;
    const CExpression& expr = expr_stmt->Expression();
    if (expr.Kind() != CExpressionKind::kAssignmentExpression) return;
    const auto& assign = static_cast<const AssignmentExpression&>(expr);
    if (!IsEligibleStoreFlagConditionalTarget(assign.Lhs())) {
      return;
    }

    std::string step_op;
    std::unique_ptr<CExpression> normalized_cond;
    const CExpression* cond_expr = nullptr;

    if (assign.Op() == "+=" || assign.Op() == "-=") {
      cond_expr = ExtractRelationalConditionFromStoreFlag(assign.Rhs(), &normalized_cond);
      step_op = (assign.Op() == "+=") ? "+" : "-";
    } else if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
      const auto& rhs_bin = static_cast<const BinaryExpression&>(assign.Rhs());
      if (rhs_bin.Op() == "+" || rhs_bin.Op() == "-") {
        if (rhs_bin.Lhs().ToString() == assign.Lhs().ToString()) {
          cond_expr = ExtractRelationalConditionFromStoreFlag(rhs_bin.Rhs(), &normalized_cond);
          step_op = rhs_bin.Op();
        } else if (rhs_bin.Op() == "+" && rhs_bin.Rhs().ToString() == assign.Lhs().ToString()) {
          cond_expr = ExtractRelationalConditionFromStoreFlag(rhs_bin.Lhs(), &normalized_cond);
          step_op = "+";
        }
      }
    }

    if (cond_expr == nullptr || !IsPureExpression(*cond_expr)) {
      return;
    }
    if (assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
        ExpressionUsesVariable(*cond_expr,
                               static_cast<const IdentifierExpression&>(assign.Lhs()).Name())) {
      return;
    }

    std::unique_ptr<CExpression> final_cond =
        normalized_cond != nullptr ? std::move(normalized_cond) : cond_expr->Clone();
    auto then_block = std::make_unique<CompoundStatement>();
    then_block->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", assign.Lhs().Clone(),
        CExpression::Binary(step_op, assign.Lhs().Clone(), CExpression::Integer(1)))));
    *stmt = CStatement::If(std::move(final_cond), std::move(then_block));
    MarkChanged();
  }

 private:
  const CompoundStatement* root_body_ = nullptr;
};

}  // namespace

void SimplifyShiftAddMultiplication(CompoundStatement* comp) {
  if (comp == nullptr) return;
  ShiftAddMultiplicationMutator mutator;
  mutator.MutateCompoundStatement(comp);
}

void SimplifyModuloAndUnsignedFloatDiamondsInBlock(CompoundStatement* comp) {
  absl::flat_hash_set<std::string> consumed_bias_vars;
  SimplifyModuloAndUnsignedFloatDiamondsInBlockImpl(comp, /*parent_bias_vars=*/nullptr,
                                                    &consumed_bias_vars);
  if (comp != nullptr && !consumed_bias_vars.empty()) {
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      std::string lhs_var;
      const CExpression* rhs_expr = nullptr;
      if (ExtractSimpleRegVarAssignment(*stmts[i], &lhs_var, &rhs_expr) &&
          consumed_bias_vars.contains(lhs_var) && IsUnsignedFloatBiasExpr(*rhs_expr)) {
        bool used_after = false;
        for (size_t k = i + 1; k < stmts.size(); ++k) {
          if (CountRhsVariableUsesInStatement(*stmts[k], lhs_var) > 0) {
            used_after = true;
            break;
          }
          std::string klhs;
          const CExpression* krhs = nullptr;
          if (ExtractSimpleRegVarAssignment(*stmts[k], &klhs, &krhs) && klhs == lhs_var) {
            break;
          }
        }
        if (!used_after) {
          stmts.erase(stmts.begin() + i);
          --i;
        }
      }
    }
  }
}

void SimplifyReciprocalDivisionAndUnsignedFloatConversions(CompoundStatement* comp) {
  SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(comp, /*parent_const_map=*/nullptr,
                                                            /*shared_consumed_magics=*/nullptr,
                                                            /*parent_sign_equiv_map=*/nullptr);
}

bool ShiftAddMultiplicationSimplifierPass::Run(CompoundStatement* body,
                                               const AstPassContext& /*ctx*/) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  SimplifyShiftAddMultiplication(body);
  return body->ToString() != before;
}

bool ReciprocalAndUnsignedFloatSimplifierPass::Run(CompoundStatement* body,
                                                   const AstPassContext& /*ctx*/) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  SimplifyReciprocalDivisionAndUnsignedFloatConversions(body);
  return body->ToString() != before;
}

bool RecoverStoreFlagConditionals(CompoundStatement* comp) {
  if (comp == nullptr) return false;
  StoreFlagConditionalRecoveryMutator mutator(comp);
  mutator.MutateCompoundStatement(comp);
  return mutator.Changed();
}

bool StoreFlagConditionalRecoveryPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  if (body == nullptr) return false;
  StoreFlagConditionalRecoveryMutator mutator(body);
  mutator.MutateCompoundStatement(body);
  return mutator.Changed();
}

}  // namespace rom_nom_nom
