#include "lifter/ast_memory_idiom_rewriter.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"

namespace rom_nom_nom {
namespace {

bool StatementContainsGoto(const CStatement& stmt) {
  if (stmt.Kind() == CStatementKind::kGotoStatement) {
    return true;
  }
  switch (stmt.Kind()) {
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return StatementContainsGoto(ifs.ThenBranch()) ||
             (ifs.ElseBranch() != nullptr && StatementContainsGoto(*ifs.ElseBranch()));
    }
    case CStatementKind::kWhileStatement:
      return StatementContainsGoto(static_cast<const WhileStatement&>(stmt).Body());
    case CStatementKind::kDoWhileStatement:
      return StatementContainsGoto(static_cast<const DoWhileStatement&>(stmt).Body());
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return (fs.Init() != nullptr && StatementContainsGoto(*fs.Init())) ||
             StatementContainsGoto(fs.Body());
    }
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr && StatementContainsGoto(*child)) {
          return true;
        }
      }
      return false;
    case CStatementKind::kSwitchStatement:
      for (const auto& sc : static_cast<const SwitchStatement&>(stmt).Cases()) {
        if (sc.body != nullptr && StatementContainsGoto(*sc.body)) {
          return true;
        }
      }
      return false;
    default:
      return false;
  }
}

}  // namespace

void CollapseUnalignedBlockCopies(CompoundStatement* comp) {
  if (comp == nullptr) return;

  auto extract_base_and_offset = [](const CExpression& addr, std::string* base_out,
                                    int64_t* offset_out) -> bool {
    if (addr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(addr);
      if (bin.Op() == "+" && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        *base_out = bin.Lhs().ToString();
        *offset_out = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        return true;
      }
    }
    *base_out = addr.ToString();
    *offset_out = 0;
    return true;
  };

  auto extract_typed_load = [&](const CStatement& s, std::string_view expected_type,
                                std::string* lhs_var_out, std::string* base_out,
                                int64_t* offset_out, const CExpression** base_expr_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
    if (a.Rhs().Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(a.Rhs());
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    if (cast.TargetType().name != expected_type || cast.TargetType().pointer_depth != 1) {
      return false;
    }
    *lhs_var_out = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
    if (!extract_base_and_offset(cast.Operand(), base_out, offset_out)) return false;
    if (cast.Operand().Kind() == CExpressionKind::kBinaryExpression &&
        static_cast<const BinaryExpression&>(cast.Operand()).Op() == "+" &&
        static_cast<const BinaryExpression&>(cast.Operand()).Rhs().Kind() ==
            CExpressionKind::kIntegerLiteral) {
      *base_expr_out = &static_cast<const BinaryExpression&>(cast.Operand()).Lhs();
    } else {
      *base_expr_out = &cast.Operand();
    }
    return true;
  };

  auto match_u8_byte_store = [&](const CStatement& s, std::string_view expected_src_var,
                                 int64_t expected_shift, std::string* dst_base_out,
                                 int64_t* dst_offset_out,
                                 const CExpression** dst_base_expr_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(a.Lhs());
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    if (cast.TargetType().name != "u8" || cast.TargetType().pointer_depth != 1) return false;
    if (expected_shift == 0) {
      if (a.Rhs().Kind() != CExpressionKind::kIdentifier ||
          static_cast<const IdentifierExpression&>(a.Rhs()).Name() != expected_src_var) {
        return false;
      }
    } else {
      if (a.Rhs().Kind() != CExpressionKind::kBinaryExpression) return false;
      const auto& rhs_bin = static_cast<const BinaryExpression&>(a.Rhs());
      if (rhs_bin.Op() != ">>" || rhs_bin.Lhs().Kind() != CExpressionKind::kIdentifier ||
          static_cast<const IdentifierExpression&>(rhs_bin.Lhs()).Name() != expected_src_var ||
          rhs_bin.Rhs().Kind() != CExpressionKind::kIntegerLiteral ||
          static_cast<const IntegerLiteralExpression&>(rhs_bin.Rhs()).Value() != expected_shift) {
        return false;
      }
    }
    if (!extract_base_and_offset(cast.Operand(), dst_base_out, dst_offset_out)) return false;
    if (cast.Operand().Kind() == CExpressionKind::kBinaryExpression &&
        static_cast<const BinaryExpression&>(cast.Operand()).Op() == "+" &&
        static_cast<const BinaryExpression&>(cast.Operand()).Rhs().Kind() ==
            CExpressionKind::kIntegerLiteral) {
      *dst_base_expr_out = &static_cast<const BinaryExpression&>(cast.Operand()).Lhs();
    } else {
      *dst_base_expr_out = &cast.Operand();
    }
    return true;
  };

  auto match_s8_byte_store = [&](const CStatement& s, std::string_view expected_src_var,
                                 std::string_view expected_dst_base,
                                 int64_t expected_dst_offset) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(a.Lhs());
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    if ((cast.TargetType().name != "s8" && cast.TargetType().name != "u8") ||
        cast.TargetType().pointer_depth != 1) {
      return false;
    }
    if (a.Rhs().Kind() != CExpressionKind::kIdentifier ||
        static_cast<const IdentifierExpression&>(a.Rhs()).Name() != expected_src_var) {
      return false;
    }
    std::string b;
    int64_t off = 0;
    if (!extract_base_and_offset(cast.Operand(), &b, &off)) return false;
    return b == expected_dst_base && off == expected_dst_offset;
  };

  auto make_addr_with_offset = [](const CExpression& base_expr,
                                  int64_t offset) -> std::unique_ptr<CExpression> {
    if (offset == 0) return base_expr.Clone();
    return CExpression::Binary("+", base_expr.Clone(), CExpression::Integer(offset));
  };

  auto make_block_copy_stmt = [&](std::string_view struct_name, const CExpression& dst_base,
                                  int64_t dst_off, const CExpression& src_base,
                                  int64_t src_off) -> std::unique_ptr<CStatement> {
    CType ptr_type = CType::Named(std::string(struct_name)).MakePointer();
    auto lhs = CExpression::Unary(
        "*", CExpression::Cast(ptr_type, make_addr_with_offset(dst_base, dst_off)));
    auto rhs = CExpression::Unary(
        "*", CExpression::Cast(ptr_type, make_addr_with_offset(src_base, src_off)));
    return CStatement::Expression(CExpression::Assignment("=", std::move(lhs), std::move(rhs)));
  };

  auto& stmts = comp->MutableStatements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    // Try matching 6-byte copy: 1 word load + 2 s8 loads + 4 u8 byte stores + 2 s8 byte stores
    if (i + 9 <= stmts.size()) {
      std::string v0, v1, v2, src_b0, src_b1, src_b2, dst_b0;
      int64_t src_off0 = 0, src_off1 = 0, src_off2 = 0, dst_off0 = 0;
      const CExpression* src_expr0 = nullptr;
      const CExpression* dummy_expr = nullptr;
      const CExpression* dst_expr0 = nullptr;
      if (extract_typed_load(*stmts[i], "s32", &v0, &src_b0, &src_off0, &src_expr0) &&
          extract_typed_load(*stmts[i + 1], "s8", &v1, &src_b1, &src_off1, &dummy_expr) &&
          extract_typed_load(*stmts[i + 2], "s8", &v2, &src_b2, &src_off2, &dummy_expr) &&
          src_b1 == src_b0 && src_b2 == src_b0 && src_off1 == src_off0 + 4 &&
          src_off2 == src_off0 + 5 &&
          match_u8_byte_store(*stmts[i + 3], v0, 24, &dst_b0, &dst_off0, &dst_expr0)) {
        bool all_stores_ok = true;
        for (int b = 1; b < 4; ++b) {
          std::string db;
          int64_t doff = 0;
          if (!match_u8_byte_store(*stmts[i + 3 + b], v0, (3 - b) * 8, &db, &doff, &dummy_expr) ||
              db != dst_b0 || doff != dst_off0 + b) {
            all_stores_ok = false;
            break;
          }
        }
        if (all_stores_ok && match_s8_byte_store(*stmts[i + 7], v1, dst_b0, dst_off0 + 4) &&
            match_s8_byte_store(*stmts[i + 8], v2, dst_b0, dst_off0 + 5)) {
          auto copy_stmt =
              make_block_copy_stmt("UnalignedBlock6", *dst_expr0, dst_off0, *src_expr0, src_off0);
          stmts[i] = std::move(copy_stmt);
          stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 9);
          continue;
        }
      }
    }

    // Try matching N-word copy (N = 4, 3, 2, 1): N s32 loads + 4*N u8 byte stores
    bool matched_n = false;
    for (int n_words : {4, 3, 2, 1}) {
      size_t total_stmts = static_cast<size_t>(n_words * 5);
      if (i + total_stmts > stmts.size()) continue;
      std::vector<std::string> vars(n_words);
      std::string src_base;
      int64_t src_off0 = 0;
      const CExpression* src_base_expr = nullptr;
      bool loads_ok = true;
      for (int w = 0; w < n_words; ++w) {
        std::string b;
        int64_t off = 0;
        const CExpression* be = nullptr;
        if (!extract_typed_load(*stmts[i + w], "s32", &vars[w], &b, &off, &be)) {
          loads_ok = false;
          break;
        }
        if (w == 0) {
          src_base = b;
          src_off0 = off;
          src_base_expr = be;
        } else if (b != src_base || off != src_off0 + w * 4) {
          loads_ok = false;
          break;
        }
      }
      if (!loads_ok) continue;

      std::string dst_base;
      int64_t dst_off0 = 0;
      const CExpression* dst_base_expr = nullptr;
      bool stores_ok = true;
      for (int w = 0; w < n_words && stores_ok; ++w) {
        for (int b = 0; b < 4; ++b) {
          std::string db;
          int64_t doff = 0;
          const CExpression* dbe = nullptr;
          size_t stmt_idx = i + n_words + w * 4 + b;
          if (!match_u8_byte_store(*stmts[stmt_idx], vars[w], (3 - b) * 8, &db, &doff, &dbe)) {
            stores_ok = false;
            break;
          }
          if (w == 0 && b == 0) {
            dst_base = db;
            dst_off0 = doff;
            dst_base_expr = dbe;
          } else if (db != dst_base || doff != dst_off0 + w * 4 + b) {
            stores_ok = false;
            break;
          }
        }
      }
      if (!stores_ok) continue;

      std::string_view block_type = "UnalignedWord";
      if (n_words == 4) {
        block_type = "UnalignedBlock16";
      } else if (n_words == 3) {
        block_type = "UnalignedBlock12";
      } else if (n_words == 2) {
        block_type = "UnalignedBlock8";
      }
      auto copy_stmt =
          make_block_copy_stmt(block_type, *dst_base_expr, dst_off0, *src_base_expr, src_off0);
      stmts[i] = std::move(copy_stmt);
      stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + total_stmts);
      matched_n = true;
      break;
    }
    if (matched_n) {
      if (i > 0 && stmts[i - 1]->Kind() == CStatementKind::kExpressionStatement &&
          stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
        // Merge two consecutive UnalignedBlock8 copies at offset 0 and +8 into UnalignedBlock16!
        const auto& prev_es = static_cast<const ExpressionStatement&>(*stmts[i - 1]);
        const auto& cur_es = static_cast<const ExpressionStatement&>(*stmts[i]);
        if (prev_es.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
            cur_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& pa = static_cast<const AssignmentExpression&>(prev_es.Expression());
          const auto& ca = static_cast<const AssignmentExpression&>(cur_es.Expression());
          auto extract_block8 = [&](const CExpression& e, std::string* b_out, int64_t* off_out,
                                    const CExpression** be_out) -> bool {
            if (e.Kind() != CExpressionKind::kUnaryExpression) return false;
            const auto& u = static_cast<const UnaryExpression&>(e);
            if (u.Op() != "*" || u.Operand().Kind() != CExpressionKind::kCastExpression) {
              return false;
            }
            const auto& c = static_cast<const CastExpression&>(u.Operand());
            if (c.TargetType().name != "UnalignedBlock8" || c.TargetType().pointer_depth != 1) {
              return false;
            }
            if (!extract_base_and_offset(c.Operand(), b_out, off_out)) return false;
            if (c.Operand().Kind() == CExpressionKind::kBinaryExpression &&
                static_cast<const BinaryExpression&>(c.Operand()).Op() == "+" &&
                static_cast<const BinaryExpression&>(c.Operand()).Rhs().Kind() ==
                    CExpressionKind::kIntegerLiteral) {
              *be_out = &static_cast<const BinaryExpression&>(c.Operand()).Lhs();
            } else {
              *be_out = &c.Operand();
            }
            return true;
          };
          std::string pd, ps, cd, cs;
          int64_t pdo = 0, pso = 0, cdo = 0, cso = 0;
          const CExpression* pde = nullptr;
          const CExpression* pse = nullptr;
          const CExpression* dummy = nullptr;
          if (extract_block8(pa.Lhs(), &pd, &pdo, &pde) &&
              extract_block8(pa.Rhs(), &ps, &pso, &pse) &&
              extract_block8(ca.Lhs(), &cd, &cdo, &dummy) &&
              extract_block8(ca.Rhs(), &cs, &cso, &dummy) && pd == cd && ps == cs &&
              cdo == pdo + 8 && cso == pso + 8) {
            stmts[i - 1] = make_block_copy_stmt("UnalignedBlock16", *pde, pdo, *pse, pso);
            stmts.erase(stmts.begin() + i);
            --i;
          }
        }
      }
      continue;
    }
  }

  for (auto& s : stmts) {
    ForEachSubCompoundStatement(s.get(), CollapseUnalignedBlockCopies);
  }
}

void RewriteFramePointerStructOffsetAccesses(CompoundStatement* body) {
  if (body == nullptr) return;

  auto is_v0_or_v1 = [](std::string_view name) { return name == "v0" || name == "v1"; };

  // Check if the function is a void function without any value-returning
  // `return expr;` or `goto`/`label` control flow.
  bool func_has_value_return_or_goto =
      HasReturnValue(*body) || StatementContainsGoto(*body) || StatementContainsLabel(*body);

  auto is_scratch_dead_after = [&](const std::vector<std::unique_ptr<CStatement>>& stmts,
                                   size_t start_idx, std::string_view reg,
                                   bool can_fall_off_end) -> bool {
    for (size_t idx = start_idx; idx < stmts.size(); ++idx) {
      if (stmts[idx] == nullptr) continue;
      if (stmts[idx]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es = static_cast<const ExpressionStatement&>(*stmts[idx]);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(a.Lhs()).Name() == reg) {
            return !ExpressionUsesVariable(a.Rhs(), reg);
          }
        }
      }
      if (stmts[idx]->Kind() == CStatementKind::kReturnStatement) {
        const auto& rs = static_cast<const ReturnStatement&>(*stmts[idx]);
        return rs.ReturnValue() == nullptr || !ExpressionUsesVariable(*rs.ReturnValue(), reg);
      }
      if (stmts[idx]->Kind() == CStatementKind::kLabelStatement ||
          stmts[idx]->Kind() == CStatementKind::kGotoStatement ||
          stmts[idx]->Kind() == CStatementKind::kBreakStatement ||
          stmts[idx]->Kind() == CStatementKind::kContinueStatement ||
          StatementReferencesVariable(*stmts[idx], reg)) {
        return false;
      }
    }
    return can_fall_off_end;
  };

  std::function<void(CompoundStatement*, bool)> fold_o0_seq = [&](CompoundStatement* comp,
                                                                  bool can_fall_off_end) {
    if (comp == nullptr) return;
    auto& stmts = comp->MutableStatements();

    // 1. Fold `while (1)` loops starting with pure `v0`/`v1` assignments + `if (cond) break;`.
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kWhileStatement) continue;
      const auto& ws = static_cast<const WhileStatement&>(*stmts[i]);
      if (ws.Condition().Kind() != CExpressionKind::kIntegerLiteral ||
          static_cast<const IntegerLiteralExpression&>(ws.Condition()).Value() != 1 ||
          ws.Body().Kind() != CStatementKind::kCompoundStatement) {
        continue;
      }
      auto body_clone = std::unique_ptr<CompoundStatement>(
          static_cast<CompoundStatement*>(ws.Body().Clone().release()));
      auto& b_stmts = body_clone->MutableStatements();
      absl::flat_hash_map<std::string, std::unique_ptr<CExpression>> env;
      size_t k = 0;
      for (; k < b_stmts.size() && k < 6; ++k) {
        if (b_stmts[k] == nullptr || b_stmts[k]->Kind() != CStatementKind::kExpressionStatement) {
          break;
        }
        const auto& es = static_cast<const ExpressionStatement&>(*b_stmts[k]);
        if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) break;
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier ||
            !IsPureExpression(a.Rhs())) {
          break;
        }
        std::string lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
        if (!is_v0_or_v1(lhs)) break;
        auto rhs_sub = a.Rhs().Clone();
        if (env.contains("v0")) {
          rhs_sub = SubstituteIdentifier(*rhs_sub, "v0", *CExpression::Identifier("__o0_v0"));
        }
        if (env.contains("v1")) {
          rhs_sub = SubstituteIdentifier(*rhs_sub, "v1", *CExpression::Identifier("__o0_v1"));
        }
        if (env.contains("v0")) {
          rhs_sub = SubstituteIdentifier(*rhs_sub, "__o0_v0", *env["v0"]);
        }
        if (env.contains("v1")) {
          rhs_sub = SubstituteIdentifier(*rhs_sub, "__o0_v1", *env["v1"]);
        }
        env[lhs] = std::move(rhs_sub);
      }
      if (k > 0 && k < b_stmts.size() && b_stmts[k] != nullptr &&
          b_stmts[k]->Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(*b_stmts[k]);
        bool is_pure_break = false;
        if (ifs.ElseBranch() == nullptr) {
          if (ifs.ThenBranch().Kind() == CStatementKind::kBreakStatement) {
            is_pure_break = true;
          } else if (ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
            const auto& tc = static_cast<const CompoundStatement&>(ifs.ThenBranch());
            if (tc.Statements().size() == 1 && tc.Statements()[0] != nullptr &&
                tc.Statements()[0]->Kind() == CStatementKind::kBreakStatement) {
              is_pure_break = true;
            }
          }
        }
        if (is_pure_break) {
          auto sub_cond = ifs.Condition().Clone();
          if (env.contains("v0")) {
            sub_cond = SubstituteIdentifier(*sub_cond, "v0", *CExpression::Identifier("__o0_v0"));
          }
          if (env.contains("v1")) {
            sub_cond = SubstituteIdentifier(*sub_cond, "v1", *CExpression::Identifier("__o0_v1"));
          }
          if (env.contains("v0")) {
            sub_cond = SubstituteIdentifier(*sub_cond, "__o0_v0", *env["v0"]);
          }
          if (env.contains("v1")) {
            sub_cond = SubstituteIdentifier(*sub_cond, "__o0_v1", *env["v1"]);
          }
          if (!ExpressionUsesVariable(*sub_cond, "v0") &&
              !ExpressionUsesVariable(*sub_cond, "v1")) {
            bool env_dead = true;
            for (const auto& [reg, _] : env) {
              bool dead_after_loop = is_scratch_dead_after(stmts, i + 1, reg, can_fall_off_end);
              if (!dead_after_loop || !is_scratch_dead_after(b_stmts, k + 1, reg, true)) {
                env_dead = false;
                break;
              }
            }
            if (env_dead) {
              auto loop_cond = NegateCondition(*sub_cond);
              if (loop_cond != nullptr) {
                b_stmts.erase(b_stmts.begin(), b_stmts.begin() + k + 1);
                stmts[i] = CStatement::While(std::move(loop_cond), std::move(body_clone));
              }
            }
          }
        }
      }
    }

    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) continue;
      if (stmts[i]->Kind() == CStatementKind::kWhileStatement) {
        auto& ws = static_cast<WhileStatement&>(*stmts[i]);
        if (ws.Body().Kind() == CStatementKind::kCompoundStatement) {
          bool while_can_fall_off = !ExpressionUsesVariable(ws.Condition(), "v0") &&
                                    !ExpressionUsesVariable(ws.Condition(), "v1") &&
                                    is_scratch_dead_after(stmts, i + 1, "v0", can_fall_off_end) &&
                                    is_scratch_dead_after(stmts, i + 1, "v1", can_fall_off_end);
          ForEachSubCompoundStatement(stmts[i].get(), [&](CompoundStatement* sub) {
            fold_o0_seq(sub, while_can_fall_off);
          });
          continue;
        }
      }
      ForEachSubCompoundStatement(stmts[i].get(), [&](CompoundStatement* sub) {
        fold_o0_seq(sub, /*can_fall_off_end=*/false);
      });
    }

    // 2. Fold post-increment byte pointer stores and loads (`stride == 1`):
    //    `vX = ptr; ptr = (vX + 1); *(u8/s8*)vX = val;` -> `*(u8/s8*)ptr++ = val;`
    //    `vX = ptr; ptr = (vX + 1); vY = *(u8/s8*)vX;` -> `vY = *(u8/s8*)ptr++;`
    for (size_t i = 0; i + 2 < stmts.size();) {
      if (stmts[i] != nullptr && stmts[i + 1] != nullptr && stmts[i + 2] != nullptr &&
          stmts[i]->Kind() == CStatementKind::kExpressionStatement &&
          stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement &&
          stmts[i + 2]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
        const auto& es1 = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
        const auto& es2 = static_cast<const ExpressionStatement&>(*stmts[i + 2]);
        if (es0.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
            es1.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
            es2.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a0 = static_cast<const AssignmentExpression&>(es0.Expression());
          const auto& a1 = static_cast<const AssignmentExpression&>(es1.Expression());
          const auto& a2 = static_cast<const AssignmentExpression&>(es2.Expression());
          if (a0.Op() == "=" && a1.Op() == "=" && a2.Op() == "=" &&
              a0.Lhs().Kind() == CExpressionKind::kIdentifier &&
              a0.Rhs().Kind() == CExpressionKind::kIdentifier &&
              a1.Lhs().Kind() == CExpressionKind::kIdentifier &&
              a1.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
            std::string vx = static_cast<const IdentifierExpression&>(a0.Lhs()).Name();
            std::string ptr_var = static_cast<const IdentifierExpression&>(a0.Rhs()).Name();
            std::string a1_lhs = static_cast<const IdentifierExpression&>(a1.Lhs()).Name();
            const auto& b1 = static_cast<const BinaryExpression&>(a1.Rhs());
            if (is_v0_or_v1(vx) && a1_lhs == ptr_var && b1.Op() == "+" &&
                b1.Lhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(b1.Lhs()).Name() == vx &&
                b1.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
              int64_t stride = static_cast<const IntegerLiteralExpression&>(b1.Rhs()).Value();
              auto match_deref_of_vx = [&](const CExpression& expr, CType* out_ptr_type) -> bool {
                if (stride != 1 || expr.Kind() != CExpressionKind::kUnaryExpression) return false;
                const auto& u = static_cast<const UnaryExpression&>(expr);
                if (u.Op() != "*" || u.Operand().Kind() != CExpressionKind::kCastExpression) {
                  return false;
                }
                const auto& cast = static_cast<const CastExpression&>(u.Operand());
                if (cast.TargetType().pointer_depth != 1 ||
                    cast.Operand().Kind() != CExpressionKind::kIdentifier ||
                    static_cast<const IdentifierExpression&>(cast.Operand()).Name() != vx) {
                  return false;
                }
                std::optional<int64_t> elem_sz = TypeByteSize(cast.TargetType().name);
                if (!elem_sz.has_value() || *elem_sz != 1) return false;
                *out_ptr_type = cast.TargetType();
                return true;
              };
              CType ptr_type = CType::Void();
              if (match_deref_of_vx(a2.Lhs(), &ptr_type) && !ExpressionUsesVariable(a2.Rhs(), vx) &&
                  is_scratch_dead_after(stmts, i + 3, vx, can_fall_off_end)) {
                auto post_inc = CExpression::Unary("++", CExpression::Identifier(ptr_var),
                                                   /*is_prefix=*/false);
                auto new_lhs =
                    CExpression::Unary("*", CExpression::Cast(ptr_type, std::move(post_inc)));
                stmts[i] = CStatement::Expression(
                    CExpression::Assignment("=", std::move(new_lhs), a2.Rhs().Clone()));
                stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 3);
                continue;
              }
              if (a2.Lhs().Kind() == CExpressionKind::kIdentifier &&
                  match_deref_of_vx(a2.Rhs(), &ptr_type)) {
                std::string vy = static_cast<const IdentifierExpression&>(a2.Lhs()).Name();
                if (vy == vx || is_scratch_dead_after(stmts, i + 3, vx, can_fall_off_end)) {
                  auto post_inc = CExpression::Unary("++", CExpression::Identifier(ptr_var),
                                                     /*is_prefix=*/false);
                  auto new_rhs =
                      CExpression::Unary("*", CExpression::Cast(ptr_type, std::move(post_inc)));
                  stmts[i] = CStatement::Expression(
                      CExpression::Assignment("=", a2.Lhs().Clone(), std::move(new_rhs)));
                  stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 3);
                  continue;
                }
              }
            }
          }
        }
      }
      ++i;
    }

    // 3. Fold single-use `v0`/`v1` scratch expression chains and erase dead scratch assignments.
    for (size_t i = 0; i < stmts.size();) {
      if (stmts[i] != nullptr && stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
        if (es0.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a0 = static_cast<const AssignmentExpression&>(es0.Expression());
          if (a0.Op() == "=" && a0.Lhs().Kind() == CExpressionKind::kIdentifier &&
              IsPureExpression(a0.Rhs())) {
            std::string vx = static_cast<const IdentifierExpression&>(a0.Lhs()).Name();
            if (is_v0_or_v1(vx)) {
              if (i + 1 < stmts.size() && stmts[i + 1] != nullptr &&
                  stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
                const auto& es1 = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                if (es1.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& a1 = static_cast<const AssignmentExpression&>(es1.Expression());
                  if (a1.Op() == "=" && a1.Lhs().Kind() == CExpressionKind::kIdentifier) {
                    std::string lhs1 = static_cast<const IdentifierExpression&>(a1.Lhs()).Name();
                    if ((is_v0_or_v1(lhs1) || absl::StartsWith(lhs1, "var_sp_")) &&
                        CountIdentifierOccurrencesInExpression(a1.Rhs(), vx) == 1 &&
                        (lhs1 == vx || is_scratch_dead_after(stmts, i + 2, vx, can_fall_off_end))) {
                      auto new_rhs = SubstituteIdentifier(a1.Rhs(), vx, a0.Rhs());
                      stmts[i + 1] = CStatement::Expression(
                          CExpression::Assignment("=", a1.Lhs().Clone(), std::move(new_rhs)));
                      stmts.erase(stmts.begin() + i);
                      continue;
                    }
                  }
                }
              }
              if (is_scratch_dead_after(stmts, i + 1, vx, can_fall_off_end)) {
                stmts.erase(stmts.begin() + i);
                continue;
              }
            }
          }
        }
      }
      ++i;
    }
  };
  fold_o0_seq(body, !func_has_value_return_or_goto);

  auto expr_has_address_of = [](const CExpression& root) -> bool {
    bool found = false;
    std::function<void(const CExpression&)> walk = [&](const CExpression& e) {
      if (found) return;
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& u = static_cast<const UnaryExpression&>(e);
        if (u.Op().find('&') != std::string::npos) {
          found = true;
          return;
        }
        walk(u.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& b = static_cast<const BinaryExpression&>(e);
        walk(b.Lhs());
        walk(b.Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        walk(static_cast<const CastExpression&>(e).Operand());
      }
    };
    walk(root);
    return found;
  };

  auto is_sp_base = [](const CExpression& base) -> bool {
    if (base.Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(base).Name() == "sp") {
      return true;
    }
    if (base.Kind() == CExpressionKind::kCastExpression) {
      const auto& op = static_cast<const CastExpression&>(base).Operand();
      if (op.Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(op).Name() == "sp") {
        return true;
      }
    }
    return false;
  };

  auto extract_positive_offset_deref = [&](const CExpression& expr, const CExpression** base_out,
                                           int64_t* off_out, std::string* elem_type_out) -> bool {
    if (expr.Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& u = static_cast<const UnaryExpression&>(expr);
    if (!u.IsPrefix() || u.Op() != "*" || u.Operand().Kind() != CExpressionKind::kCastExpression) {
      return false;
    }
    const auto& cast = static_cast<const CastExpression&>(u.Operand());
    const CType& ptr_type = cast.TargetType();
    if (ptr_type.pointer_depth != 1 || !TypeByteSize(ptr_type.name).has_value() ||
        cast.Operand().Kind() != CExpressionKind::kBinaryExpression) {
      return false;
    }
    const auto& bin = static_cast<const BinaryExpression&>(cast.Operand());
    if (bin.Op() != "+" || bin.Rhs().Kind() != CExpressionKind::kIntegerLiteral) {
      return false;
    }
    int64_t off = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
    if (off <= 0 || off >= 32768 || is_sp_base(bin.Lhs()) || expr_has_address_of(bin.Lhs())) {
      return false;
    }
    *base_out = &bin.Lhs();
    *off_out = off;
    *elem_type_out = ptr_type.name;
    return true;
  };

  std::function<std::unique_ptr<CExpression>(const CExpression&)> rewrite_expr =
      [&](const CExpression& expr) -> std::unique_ptr<CExpression> {
    const CExpression* base = nullptr;
    int64_t off = 0;
    std::string elem_type;
    if (extract_positive_offset_deref(expr, &base, &off, &elem_type)) {
      auto rewritten_base = rewrite_expr(*base);
      CType struct_ptr =
          CType::Named(absl::StrFormat("struct { u8 _p[%d]; %s _v; }", off, elem_type))
              .MakePointer();
      return CExpression::MemberAccess(
          CExpression::Cast(std::move(struct_ptr), std::move(rewritten_base)), "_v",
          /*is_arrow=*/true);
    }
    if (expr.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(u.Op(), rewrite_expr(u.Operand()), u.IsPrefix());
    }
    if (expr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(b.Op(), rewrite_expr(b.Lhs()), rewrite_expr(b.Rhs()));
    }
    if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& a = static_cast<const AssignmentExpression&>(expr);
      if (a.Op() == "=" && extract_positive_offset_deref(a.Lhs(), &base, &off, &elem_type)) {
        if (a.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
          int64_t cval = static_cast<const IntegerLiteralExpression&>(a.Rhs()).Value();
          if (elem_type == "s16" && cval >= 32768 && cval <= 65535) {
            elem_type = "u16";
          } else if (elem_type == "s8" && cval >= 128 && cval <= 255) {
            elem_type = "u8";
          }
        }
        auto rewritten_base = rewrite_expr(*base);
        CType struct_ptr =
            CType::Named(absl::StrFormat("struct { u8 _p[%d]; %s _v; }", off, elem_type))
                .MakePointer();
        auto new_lhs = CExpression::MemberAccess(
            CExpression::Cast(std::move(struct_ptr), std::move(rewritten_base)), "_v",
            /*is_arrow=*/true);
        return CExpression::Assignment(a.Op(), std::move(new_lhs), rewrite_expr(a.Rhs()));
      }
      return CExpression::Assignment(a.Op(), rewrite_expr(a.Lhs()), rewrite_expr(a.Rhs()));
    }
    if (expr.Kind() == CExpressionKind::kCallExpression) {
      const auto& c = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(c.Arguments().size());
      for (const auto& arg : c.Arguments()) {
        args.push_back(rewrite_expr(*arg));
      }
      return CExpression::Call(rewrite_expr(c.Callee()), std::move(args));
    }
    if (expr.Kind() == CExpressionKind::kCastExpression) {
      const auto& c = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(c.TargetType(), rewrite_expr(c.Operand()));
    }
    if (expr.Kind() == CExpressionKind::kMemberAccessExpression) {
      const auto& m = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(rewrite_expr(m.Object()), m.MemberName(), m.IsArrow());
    }
    if (expr.Kind() == CExpressionKind::kArrayIndexExpression) {
      const auto& a = static_cast<const ArrayIndexExpression&>(expr);
      return CExpression::ArrayIndex(rewrite_expr(a.Array()), rewrite_expr(a.Index()));
    }
    if (expr.Kind() == CExpressionKind::kTernaryExpression) {
      const auto& t = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(rewrite_expr(t.Condition()), rewrite_expr(t.TrueExpression()),
                                  rewrite_expr(t.FalseExpression()));
    }
    return expr.Clone();
  };

  std::function<std::unique_ptr<CStatement>(const CStatement&)> rewrite_stmt =
      [&](const CStatement& stmt) -> std::unique_ptr<CStatement> {
    switch (stmt.Kind()) {
      case CStatementKind::kCompoundStatement: {
        auto comp = std::make_unique<CompoundStatement>();
        for (const auto& s : static_cast<const CompoundStatement&>(stmt).Statements()) {
          if (s != nullptr) comp->AddStatement(rewrite_stmt(*s));
        }
        return comp;
      }
      case CStatementKind::kExpressionStatement:
        return CStatement::Expression(
            rewrite_expr(static_cast<const ExpressionStatement&>(stmt).Expression()));
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(stmt);
        return CStatement::If(
            rewrite_expr(ifs.Condition()), rewrite_stmt(ifs.ThenBranch()),
            ifs.ElseBranch() != nullptr ? rewrite_stmt(*ifs.ElseBranch()) : nullptr);
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(stmt);
        return CStatement::While(rewrite_expr(ws.Condition()), rewrite_stmt(ws.Body()));
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(stmt);
        return CStatement::DoWhile(rewrite_stmt(dws.Body()), rewrite_expr(dws.Condition()));
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(stmt);
        return CStatement::For(fs.Init() != nullptr ? rewrite_stmt(*fs.Init()) : nullptr,
                               fs.Condition() != nullptr ? rewrite_expr(*fs.Condition()) : nullptr,
                               fs.Step() != nullptr ? rewrite_expr(*fs.Step()) : nullptr,
                               rewrite_stmt(fs.Body()));
      }
      case CStatementKind::kReturnStatement: {
        const auto& rs = static_cast<const ReturnStatement&>(stmt);
        if (rs.ReturnValue() == nullptr) return CStatement::Return();
        return CStatement::Return(rewrite_expr(*rs.ReturnValue()));
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(stmt);
        std::vector<SwitchCase> cases;
        cases.reserve(ss.Cases().size());
        for (const auto& c : ss.Cases()) {
          SwitchCase sc;
          sc.case_values = c.case_values;
          sc.is_default = c.is_default;
          if (c.body != nullptr) {
            sc.body = std::unique_ptr<CompoundStatement>(
                static_cast<CompoundStatement*>(rewrite_stmt(*c.body).release()));
          }
          cases.push_back(std::move(sc));
        }
        return CStatement::Switch(rewrite_expr(ss.Condition()), std::move(cases));
      }
      default:
        return stmt.Clone();
    }
  };

  auto rewritten = std::unique_ptr<CompoundStatement>(
      static_cast<CompoundStatement*>(rewrite_stmt(*body).release()));
  *body = std::move(*rewritten);
}

}  // namespace rom_nom_nom
