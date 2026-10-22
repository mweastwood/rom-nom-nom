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
#include "absl/strings/str_cat.h"
#include "core/c_ast.h"

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
    }
  }

  return rewritten;
}

int CountRhsVariableUsesInStatement(const CStatement& root_s, std::string_view var) {
  int count = 0;
  std::function<void(const CExpression&)> walk_e = [&](const CExpression& e) {
    if (e.Kind() == CExpressionKind::kIdentifier) {
      if (static_cast<const IdentifierExpression&>(e).Name() == var) ++count;
      return;
    }
    if (e.Kind() == CExpressionKind::kUnaryExpression) {
      walk_e(static_cast<const UnaryExpression&>(e).Operand());
    } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& b = static_cast<const BinaryExpression&>(e);
      walk_e(b.Lhs());
      walk_e(b.Rhs());
    } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& a = static_cast<const AssignmentExpression&>(e);
      if (a.Lhs().Kind() != CExpressionKind::kIdentifier) walk_e(a.Lhs());
      walk_e(a.Rhs());
    } else if (e.Kind() == CExpressionKind::kCastExpression) {
      walk_e(static_cast<const CastExpression&>(e).Operand());
    } else if (e.Kind() == CExpressionKind::kCallExpression) {
      const auto& c = static_cast<const CallExpression&>(e);
      walk_e(c.Callee());
      for (const auto& arg : c.Arguments()) {
        if (arg != nullptr) walk_e(*arg);
      }
    }
  };
  std::function<void(const CStatement&)> walk_s = [&](const CStatement& s) {
    switch (s.Kind()) {
      case CStatementKind::kExpressionStatement:
        walk_e(static_cast<const ExpressionStatement&>(s).Expression());
        break;
      case CStatementKind::kReturnStatement: {
        const auto& r = static_cast<const ReturnStatement&>(s);
        if (r.ReturnValue() != nullptr) walk_e(*r.ReturnValue());
        break;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        walk_e(ifs.Condition());
        walk_s(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) walk_s(*ifs.ElseBranch());
        break;
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(s);
        walk_e(ws.Condition());
        walk_s(ws.Body());
        break;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(s);
        walk_s(dws.Body());
        walk_e(dws.Condition());
        break;
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) walk_s(*fs.Init());
        if (fs.Condition() != nullptr) walk_e(*fs.Condition());
        if (fs.Step() != nullptr) walk_e(*fs.Step());
        walk_s(fs.Body());
        break;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& sw = static_cast<const SwitchStatement&>(s);
        walk_e(sw.Condition());
        for (const auto& sc : sw.Cases()) {
          if (sc.body != nullptr) walk_s(*sc.body);
        }
        break;
      }
      case CStatementKind::kCompoundStatement:
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          if (child != nullptr) walk_s(*child);
        }
        break;
      default:
        break;
    }
  };
  walk_s(root_s);
  return count;
}

bool IsLocalRegVarName(std::string_view name) {
  if (name == "v0" || name == "v1" || name == "arg0" || name == "arg1" || name == "arg2" ||
      name == "arg3" || name == "fp") {
    return true;
  }
  if (absl::StartsWith(name, "temp_")) return true;
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
  if (e.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& u = static_cast<const UnaryExpression&>(e);
    if (u.Op() == "(s64)") {
      *is_unsigned_out = false;
      return &u.Operand();
    }
    if (u.Op() == "(u64)(u32)") {
      *is_unsigned_out = true;
      return &u.Operand();
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
  while (cur->Kind() == CExpressionKind::kCastExpression ||
         (cur->Kind() == CExpressionKind::kUnaryExpression &&
          (static_cast<const UnaryExpression*>(cur)->Op() == "(u64)(u32)" ||
           static_cast<const UnaryExpression*>(cur)->Op() == "(s64)" ||
           static_cast<const UnaryExpression*>(cur)->Op() == "(u32)" ||
           static_cast<const UnaryExpression*>(cur)->Op() == "(s32)"))) {
    if (cur->Kind() == CExpressionKind::kCastExpression) {
      cur = &static_cast<const CastExpression*>(cur)->Operand();
    } else {
      cur = &static_cast<const UnaryExpression*>(cur)->Operand();
    }
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

void SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
    CompoundStatement* comp, const absl::flat_hash_map<std::string, uint32_t>* parent_const_map,
    absl::flat_hash_set<uint32_t>* shared_consumed_magics,
    const absl::flat_hash_map<std::string, std::string>* parent_sign_equiv_map) {
  if (comp == nullptr) return;

  absl::flat_hash_map<std::string, uint32_t> const_map;
  if (parent_const_map != nullptr) {
    const_map = *parent_const_map;
  }
  absl::flat_hash_map<std::string, std::string> sign_equiv_map;
  if (parent_sign_equiv_map != nullptr) {
    sign_equiv_map = *parent_sign_equiv_map;
  }
  absl::flat_hash_set<uint32_t> root_consumed_magics;
  absl::flat_hash_set<uint32_t>* consumed_magics =
      (shared_consumed_magics != nullptr) ? shared_consumed_magics : &root_consumed_magics;

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
              } else if (shift_lhs->Kind() == CExpressionKind::kUnaryExpression &&
                         static_cast<const UnaryExpression*>(shift_lhs)->Op() == "(u32)") {
                shift_lhs = &static_cast<const UnaryExpression*>(shift_lhs)->Operand();
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
      switch (stmts[i]->Kind()) {
        case CStatementKind::kCompoundStatement:
          SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
              static_cast<CompoundStatement*>(stmts[i].get()), &const_map, consumed_magics,
              &sign_equiv_map);
          break;
        case CStatementKind::kIfStatement: {
          auto& ifs = static_cast<IfStatement&>(*stmts[i]);
          if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
            SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
                static_cast<CompoundStatement*>(ifs.MutableThenBranch()), &const_map,
                consumed_magics, &sign_equiv_map);
          }
          if (ifs.MutableElseBranch() != nullptr &&
              ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
            SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(
                static_cast<CompoundStatement*>(ifs.MutableElseBranch()), &const_map,
                consumed_magics, &sign_equiv_map);
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
                  sc.body.get(), &const_map, consumed_magics, &sign_equiv_map);
            }
          }
          break;
        }
        default:
          break;
      }
    }
  }

  SimplifyModuloAndUnsignedFloatDiamondsInBlock(comp);

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
}

}  // namespace

void SimplifyShiftAddMultiplication(CompoundStatement* comp) {
  if (comp == nullptr) return;

  std::function<std::unique_ptr<CStatement>(std::unique_ptr<CStatement>)> rewrite_stmt =
      [&](std::unique_ptr<CStatement> s) -> std::unique_ptr<CStatement> {
    if (s == nullptr) return nullptr;
    switch (s->Kind()) {
      case CStatementKind::kCompoundStatement: {
        auto* c = static_cast<CompoundStatement*>(s.get());
        for (auto& child : c->MutableStatements()) {
          child = rewrite_stmt(std::move(child));
        }
        return s;
      }
      case CStatementKind::kExpressionStatement: {
        const auto& es = static_cast<const ExpressionStatement&>(*s);
        return CStatement::Expression(SimplifyShiftAddExpression(es.Expression()));
      }
      case CStatementKind::kReturnStatement: {
        const auto& ret = static_cast<const ReturnStatement&>(*s);
        if (ret.ReturnValue() != nullptr) {
          return CStatement::Return(SimplifyShiftAddExpression(*ret.ReturnValue()));
        }
        return CStatement::Return(nullptr);
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(*s);
        auto new_cond = SimplifyShiftAddExpression(ifs.Condition());
        auto new_then = rewrite_stmt(ifs.ThenBranch().Clone());
        std::unique_ptr<CStatement> new_else = nullptr;
        if (ifs.ElseBranch() != nullptr) {
          new_else = rewrite_stmt(ifs.ElseBranch()->Clone());
        }
        return CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(*s);
        auto new_cond = SimplifyShiftAddExpression(ws.Condition());
        auto new_body = rewrite_stmt(ws.Body().Clone());
        return CStatement::While(std::move(new_cond), std::move(new_body));
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(*s);
        auto new_cond = SimplifyShiftAddExpression(dws.Condition());
        auto new_body = rewrite_stmt(dws.Body().Clone());
        return CStatement::DoWhile(std::move(new_body), std::move(new_cond));
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(*s);
        std::unique_ptr<CStatement> new_init = nullptr;
        if (fs.Init() != nullptr) {
          new_init = rewrite_stmt(fs.Init()->Clone());
        }
        std::unique_ptr<CExpression> new_cond = nullptr;
        if (fs.Condition() != nullptr) {
          new_cond = SimplifyShiftAddExpression(*fs.Condition());
        }
        std::unique_ptr<CExpression> new_step = nullptr;
        if (fs.Step() != nullptr) {
          new_step = SimplifyShiftAddExpression(*fs.Step());
        }
        auto new_body = rewrite_stmt(fs.Body().Clone());
        return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                               std::move(new_body));
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(*s);
        auto new_cond = SimplifyShiftAddExpression(ss.Condition());
        std::vector<SwitchCase> new_cases;
        for (const auto& c : ss.Cases()) {
          SwitchCase sc = c.Clone();
          if (c.body != nullptr) {
            auto rewritten_body = rewrite_stmt(c.body->Clone());
            sc.body = std::unique_ptr<CompoundStatement>(
                static_cast<CompoundStatement*>(rewritten_body.release()));
          }
          new_cases.push_back(std::move(sc));
        }
        return CStatement::Switch(std::move(new_cond), std::move(new_cases));
      }
      default:
        return s;
    }
  };

  for (auto& s : comp->MutableStatements()) {
    s = rewrite_stmt(std::move(s));
  }
}

void SimplifyModuloAndUnsignedFloatDiamondsInBlock(CompoundStatement* comp) {
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
                } else if (is_u_div && dx->Kind() == CExpressionKind::kUnaryExpression &&
                           static_cast<const UnaryExpression*>(dx)->Op() == "(u32)") {
                  dx = &static_cast<const UnaryExpression*>(dx)->Operand();
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
    } else if (is_u_div && x_expr->Kind() == CExpressionKind::kUnaryExpression &&
               static_cast<const UnaryExpression*>(x_expr)->Op() == "(u32)") {
      x_expr = &static_cast<const UnaryExpression*>(x_expr)->Operand();
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
    if (expr.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& ue = static_cast<const UnaryExpression&>(expr);
      return ue.Op() == "(f32)" && ue.Operand().Kind() == CExpressionKind::kIdentifier &&
             static_cast<const IdentifierExpression&>(ue.Operand()).Name() == target_var;
    }
    return false;
  };

  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->Kind() != CStatementKind::kIfStatement) continue;
    const auto& ifs = static_cast<const IfStatement&>(*stmts[i]);
    if (ifs.Condition().Kind() != CExpressionKind::kBinaryExpression) continue;
    const auto& cond_bin = static_cast<const BinaryExpression&>(ifs.Condition());
    int64_t zero_val = -1;
    if (cond_bin.Op() != "<" || cond_bin.Lhs().Kind() != CExpressionKind::kIdentifier ||
        !is_int_literal(cond_bin.Rhs(), &zero_val) || zero_val != 0) {
      continue;
    }
    std::string int_var = static_cast<const IdentifierExpression&>(cond_bin.Lhs()).Name();
    if (ifs.ThenBranch().Kind() != CStatementKind::kCompoundStatement) continue;
    const auto& then_comp = static_cast<const CompoundStatement&>(ifs.ThenBranch());
    if (then_comp.Statements().empty() || then_comp.Statements().size() > 2) continue;
    std::string then_lhs;
    const CExpression* then_rhs = nullptr;
    if (!extract_simple_var_assign(*then_comp.Statements()[0], &then_lhs, &then_rhs) ||
        !absl::EndsWith(then_lhs, "_d")) {
      continue;
    }
    std::string branch_narrow_dst;
    if (then_comp.Statements().size() == 2) {
      if (ifs.ElseBranch() == nullptr ||
          ifs.ElseBranch()->Kind() != CStatementKind::kCompoundStatement) {
        continue;
      }
      const auto& else_comp = static_cast<const CompoundStatement&>(*ifs.ElseBranch());
      if (else_comp.Statements().size() != 1) continue;
      std::string tlhs2, elhs1;
      const CExpression* trhs2 = nullptr;
      const CExpression* erhs1 = nullptr;
      if (!extract_simple_var_assign(*then_comp.Statements()[1], &tlhs2, &trhs2) ||
          !extract_simple_var_assign(*else_comp.Statements()[0], &elhs1, &erhs1) ||
          tlhs2 != elhs1 || !is_f32_cast_of(*trhs2, then_lhs) ||
          !is_f32_cast_of(*erhs1, then_lhs)) {
        continue;
      }
      branch_narrow_dst = tlhs2;
    } else if (ifs.ElseBranch() != nullptr) {
      continue;
    }
    if (then_rhs->Kind() != CExpressionKind::kBinaryExpression) continue;
    const auto& add_bin = static_cast<const BinaryExpression&>(*then_rhs);
    if (add_bin.Op() != "+" || add_bin.Lhs().Kind() != CExpressionKind::kIdentifier ||
        static_cast<const IdentifierExpression&>(add_bin.Lhs()).Name() != then_lhs) {
      continue;
    }
    bool rhs_uses_rodata_symbol = absl::StrContains(add_bin.Rhs().ToString(), "D_") ||
                                  absl::StrContains(add_bin.Rhs().ToString(), "RO_");
    if (!rhs_uses_rodata_symbol) continue;

    int cvt_idx = -1;
    for (int k = static_cast<int>(i) - 1; k >= 0 && k >= static_cast<int>(i) - 2; --k) {
      std::string clhs;
      const CExpression* crhs = nullptr;
      if (extract_simple_var_assign(*stmts[k], &clhs, &crhs) && clhs == then_lhs) {
        if (crhs->Kind() == CExpressionKind::kCastExpression) {
          const auto& ce = static_cast<const CastExpression&>(*crhs);
          if (ce.TargetType().name == "f64" && ce.TargetType().pointer_depth == 0 &&
              ce.Operand().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(ce.Operand()).Name() == int_var) {
            cvt_idx = k;
            break;
          }
        } else if (crhs->Kind() == CExpressionKind::kUnaryExpression) {
          const auto& ue = static_cast<const UnaryExpression&>(*crhs);
          if (ue.Op() == "(f64)" && ue.Operand().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(ue.Operand()).Name() == int_var) {
            cvt_idx = k;
            break;
          }
        }
      }
    }
    if (cvt_idx < 0) continue;

    if (!branch_narrow_dst.empty()) {
      stmts[cvt_idx] = CStatement::Expression(CExpression::Assignment(
          "=", CExpression::Identifier(branch_narrow_dst),
          CExpression::Cast(CType::F32(),
                            CExpression::Cast(CType::U32(), CExpression::Identifier(int_var)))));
      stmts.erase(stmts.begin() + i);
      --i;
      continue;
    }

    auto u32_cast = CExpression::Cast(CType::U32(), CExpression::Identifier(int_var));
    stmts[cvt_idx] = CStatement::Expression(
        CExpression::Assignment("=", CExpression::Identifier(then_lhs),
                                CExpression::Cast(CType::F64(), std::move(u32_cast))));
    stmts.erase(stmts.begin() + i);
    --i;

    for (size_t m = static_cast<size_t>(cvt_idx) + 1;
         m < stmts.size() && m <= static_cast<size_t>(cvt_idx) + 3; ++m) {
      if (stmts[m]->Kind() == CStatementKind::kReturnStatement) {
        const auto& rs = static_cast<const ReturnStatement&>(*stmts[m]);
        if (rs.ReturnValue() != nullptr && is_f32_cast_of(*rs.ReturnValue(), then_lhs)) {
          bool int_var_clobbered = false;
          for (size_t p = static_cast<size_t>(cvt_idx) + 1; p < m; ++p) {
            std::string plhs;
            const CExpression* prhs = nullptr;
            if (extract_simple_var_assign(*stmts[p], &plhs, &prhs) && plhs == int_var) {
              int_var_clobbered = true;
              break;
            }
          }
          if (!int_var_clobbered) {
            stmts[m] = CStatement::Return(CExpression::Cast(
                CType::F32(), CExpression::Cast(CType::U32(), CExpression::Identifier(int_var))));
            stmts.erase(stmts.begin() + cvt_idx);
            if (m <= i) --i;
          }
          break;
        }
        if (rs.ReturnValue() != nullptr &&
            rs.ReturnValue()->Kind() == CExpressionKind::kBinaryExpression) {
          const auto& rbin = static_cast<const BinaryExpression&>(*rs.ReturnValue());
          if (is_f32_cast_of(rbin.Rhs(), then_lhs)) {
            bool int_var_clobbered = false;
            for (size_t p = static_cast<size_t>(cvt_idx) + 1; p < m; ++p) {
              std::string plhs;
              const CExpression* prhs = nullptr;
              if (extract_simple_var_assign(*stmts[p], &plhs, &prhs) && plhs == int_var) {
                int_var_clobbered = true;
                break;
              }
            }
            if (!int_var_clobbered) {
              stmts[m] = CStatement::Return(CExpression::Binary(
                  rbin.Op(), rbin.Lhs().Clone(),
                  CExpression::Cast(
                      CType::F32(),
                      CExpression::Cast(CType::U32(), CExpression::Identifier(int_var)))));
              stmts.erase(stmts.begin() + cvt_idx);
              if (m <= i) --i;
            }
            break;
          }
        }
      }
      std::string nlhs;
      const CExpression* nrhs = nullptr;
      if (extract_simple_var_assign(*stmts[m], &nlhs, &nrhs) && is_f32_cast_of(*nrhs, then_lhs)) {
        bool int_var_clobbered = false;
        for (size_t p = static_cast<size_t>(cvt_idx) + 1; p < m; ++p) {
          std::string plhs;
          const CExpression* prhs = nullptr;
          if (extract_simple_var_assign(*stmts[p], &plhs, &prhs) && plhs == int_var) {
            int_var_clobbered = true;
            break;
          }
        }
        if (!int_var_clobbered) {
          bool f_dst_touched_between = false;
          for (size_t p = static_cast<size_t>(cvt_idx) + 1; p < m; ++p) {
            if (count_var_uses_in_stmt(*stmts[p], nlhs) > 0) {
              f_dst_touched_between = true;
              break;
            }
          }
          auto f32_u32_stmt = CStatement::Expression(CExpression::Assignment(
              "=", CExpression::Identifier(nlhs),
              CExpression::Cast(
                  CType::F32(),
                  CExpression::Cast(CType::U32(), CExpression::Identifier(int_var)))));
          if (!f_dst_touched_between) {
            stmts[cvt_idx] = std::move(f32_u32_stmt);
            stmts.erase(stmts.begin() + m);
          } else {
            stmts[m] = std::move(f32_u32_stmt);
            stmts.erase(stmts.begin() + cvt_idx);
          }
          if (m <= i) --i;
        }
        break;
      }
    }
  }
}

void SimplifyReciprocalDivisionAndUnsignedFloatConversions(CompoundStatement* comp) {
  SimplifyReciprocalDivisionAndUnsignedFloatConversionsImpl(comp, /*parent_const_map=*/nullptr,
                                                            /*shared_consumed_magics=*/nullptr,
                                                            /*parent_sign_equiv_map=*/nullptr);
}

}  // namespace rom_nom_nom
