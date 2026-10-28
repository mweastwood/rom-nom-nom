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
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "core/c_ast_visitor.h"

namespace rom_nom_nom {
namespace {

class GotoStatementFinder : public AstVisitor {
 public:
  bool Found() const { return found_; }
  void VisitGotoStatement(const GotoStatement& /*stmt*/) override { found_ = true; }

 private:
  bool found_ = false;
};

bool StatementContainsGoto(const CStatement& stmt) {
  GotoStatementFinder finder;
  finder.VisitStatement(stmt);
  return finder.Found();
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

  auto extract_scalar_tail_load = [&](const CStatement& s, std::string* lhs_var_out,
                                      int64_t* width_out, std::string* base_out,
                                      int64_t* offset_out) -> bool {
    const CExpression* dummy_expr = nullptr;
    if (extract_typed_load(s, "s16", lhs_var_out, base_out, offset_out, &dummy_expr) ||
        extract_typed_load(s, "u16", lhs_var_out, base_out, offset_out, &dummy_expr)) {
      *width_out = 2;
      return true;
    }
    if (extract_typed_load(s, "s8", lhs_var_out, base_out, offset_out, &dummy_expr) ||
        extract_typed_load(s, "u8", lhs_var_out, base_out, offset_out, &dummy_expr)) {
      *width_out = 1;
      return true;
    }
    return false;
  };

  auto match_scalar_tail_store = [&](const CStatement& s, std::string_view expected_src_var,
                                     int64_t expected_width, std::string_view expected_dst_base,
                                     int64_t expected_dst_offset) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(a.Lhs());
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    if (cast.TargetType().pointer_depth != 1) return false;
    if (expected_width == 2) {
      if (cast.TargetType().name != "s16" && cast.TargetType().name != "u16") return false;
    } else if (expected_width == 1) {
      if (cast.TargetType().name != "s8" && cast.TargetType().name != "u8") return false;
    } else {
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

  auto unaligned_block_type_name = [](int64_t num_bytes) -> std::string {
    if (num_bytes == 4) {
      return "UnalignedWord";
    }
    return absl::StrFormat("UnalignedBlock%d", num_bytes);
  };

  auto parse_unaligned_block_size = [](std::string_view type_name, int64_t* size_out) -> bool {
    if (type_name == "UnalignedWord") {
      *size_out = 4;
      return true;
    }
    if (absl::StartsWith(type_name, "UnalignedBlock")) {
      int64_t val = 0;
      if (absl::SimpleAtoi(type_name.substr(14), &val) && val > 0) {
        *size_out = val;
        return true;
      }
    }
    return false;
  };

  auto extract_unaligned_block_deref = [&](const CExpression& e, int64_t* size_out,
                                           std::string* b_out, int64_t* off_out,
                                           const CExpression** be_out) -> bool {
    if (e.Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& u = static_cast<const UnaryExpression&>(e);
    if (u.Op() != "*" || u.Operand().Kind() != CExpressionKind::kCastExpression) {
      return false;
    }
    const auto& c = static_cast<const CastExpression&>(u.Operand());
    if (c.TargetType().pointer_depth != 1 ||
        !parse_unaligned_block_size(c.TargetType().name, size_out)) {
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

  auto extract_unaligned_block_copy_stmt =
      [&](const CStatement& s, int64_t* size_out, std::string* dst_base_out, int64_t* dst_off_out,
          const CExpression** dst_expr_out, std::string* src_base_out, int64_t* src_off_out,
          const CExpression** src_expr_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=") return false;
    int64_t lhs_size = 0, rhs_size = 0;
    if (!extract_unaligned_block_deref(a.Lhs(), &lhs_size, dst_base_out, dst_off_out,
                                       dst_expr_out) ||
        !extract_unaligned_block_deref(a.Rhs(), &rhs_size, src_base_out, src_off_out,
                                       src_expr_out) ||
        lhs_size != rhs_size) {
      return false;
    }
    *size_out = lhs_size;
    return true;
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

  auto try_absorb_trailing_scalar_copies = [&](size_t idx) {
    while (idx + 2 < stmts.size()) {
      int64_t cur_size = 0, dst_off = 0, src_off = 0;
      std::string dst_base, src_base;
      const CExpression* dst_expr = nullptr;
      const CExpression* src_expr = nullptr;
      if (!extract_unaligned_block_copy_stmt(*stmts[idx], &cur_size, &dst_base, &dst_off, &dst_expr,
                                             &src_base, &src_off, &src_expr)) {
        break;
      }
      struct TailLoadInfo {
        std::string var;
        int64_t width = 0;
        int64_t rel_off = 0;
      };
      std::vector<TailLoadInfo> tails;
      int64_t tail_bytes = 0;
      while (idx + 1 + tails.size() < stmts.size() && tail_bytes < 3) {
        std::string tv, tb;
        int64_t tw = 0, toff = 0;
        if (!extract_scalar_tail_load(*stmts[idx + 1 + tails.size()], &tv, &tw, &tb, &toff) ||
            tb != src_base || toff != src_off + cur_size + tail_bytes || tail_bytes + tw > 3) {
          break;
        }
        tails.push_back(TailLoadInfo{tv, tw, cur_size + tail_bytes});
        tail_bytes += tw;
      }
      bool absorbed = false;
      for (size_t n_tail = tails.size(); n_tail >= 1; --n_tail) {
        if (idx + 1 + 2 * n_tail > stmts.size()) continue;
        bool stores_ok = true;
        int64_t matched_bytes = 0;
        for (size_t t = 0; t < n_tail; ++t) {
          if (!match_scalar_tail_store(*stmts[idx + 1 + n_tail + t], tails[t].var, tails[t].width,
                                       dst_base, dst_off + tails[t].rel_off)) {
            stores_ok = false;
            break;
          }
          matched_bytes += tails[t].width;
        }
        if (stores_ok) {
          std::string merged_type = unaligned_block_type_name(cur_size + matched_bytes);
          stmts[idx] = make_block_copy_stmt(merged_type, *dst_expr, dst_off, *src_expr, src_off);
          stmts.erase(stmts.begin() + idx + 1, stmts.begin() + idx + 1 + 2 * n_tail);
          absorbed = true;
          break;
        }
      }
      if (!absorbed) break;
    }
  };

  for (size_t i = 0; i < stmts.size(); ++i) {
    bool matched_block = false;
    for (std::pair<std::string_view, int64_t> word_spec :
         {std::pair<std::string_view, int64_t>{"s32", 4},
          std::pair<std::string_view, int64_t>{"s64", 8}}) {
      std::string_view word_type = word_spec.first;
      int64_t word_size = word_spec.second;

      std::vector<std::string> word_vars;
      std::string src_base;
      int64_t src_off0 = 0;
      const CExpression* src_base_expr = nullptr;
      while (i + word_vars.size() < stmts.size()) {
        std::string v, b;
        int64_t off = 0;
        const CExpression* be = nullptr;
        if (!extract_typed_load(*stmts[i + word_vars.size()], word_type, &v, &b, &off, &be)) {
          break;
        }
        if (word_vars.empty()) {
          src_base = b;
          src_off0 = off;
          src_base_expr = be;
        } else if (b != src_base ||
                   off != src_off0 + static_cast<int64_t>(word_vars.size()) * word_size) {
          break;
        }
        word_vars.push_back(std::move(v));
      }
      if (word_vars.empty()) continue;

      for (int n_words = static_cast<int>(word_vars.size()); n_words >= 1 && !matched_block;
           --n_words) {
        struct TailLoad {
          std::string var;
          int64_t width = 0;
          int64_t rel_off = 0;
        };
        std::vector<TailLoad> tail_loads;
        int64_t tail_bytes = 0;
        int64_t word_bytes = static_cast<int64_t>(n_words) * word_size;
        while (i + n_words + tail_loads.size() < stmts.size() && tail_bytes < word_size - 1) {
          std::string tv, tb;
          int64_t tw = 0, toff = 0;
          if (!extract_scalar_tail_load(*stmts[i + n_words + tail_loads.size()], &tv, &tw, &tb,
                                        &toff) ||
              tb != src_base || toff != src_off0 + word_bytes + tail_bytes ||
              tail_bytes + tw >= word_size) {
            break;
          }
          tail_loads.push_back(TailLoad{tv, tw, word_bytes + tail_bytes});
          tail_bytes += tw;
        }

        for (int n_tail = static_cast<int>(tail_loads.size()); n_tail >= 0; --n_tail) {
          size_t total_loads = static_cast<size_t>(n_words + n_tail);
          size_t total_word_stores = static_cast<size_t>(n_words * word_size);
          size_t total_stmts = total_loads + total_word_stores + static_cast<size_t>(n_tail);
          if (i + total_stmts > stmts.size()) continue;

          std::string dst_base;
          int64_t dst_off0 = 0;
          const CExpression* dst_base_expr = nullptr;
          bool stores_ok = true;
          for (int w = 0; w < n_words && stores_ok; ++w) {
            for (int b = 0; b < word_size; ++b) {
              std::string db;
              int64_t doff = 0;
              const CExpression* dbe = nullptr;
              size_t stmt_idx = i + total_loads + static_cast<size_t>(w * word_size + b);
              if (!match_u8_byte_store(*stmts[stmt_idx], word_vars[w], (word_size - 1 - b) * 8, &db,
                                       &doff, &dbe)) {
                stores_ok = false;
                break;
              }
              if (w == 0 && b == 0) {
                dst_base = db;
                dst_off0 = doff;
                dst_base_expr = dbe;
              } else if (db != dst_base || doff != dst_off0 + w * word_size + b) {
                stores_ok = false;
                break;
              }
            }
          }
          if (!stores_ok) continue;

          int64_t matched_tail_bytes = 0;
          for (int t = 0; t < n_tail; ++t) {
            size_t stmt_idx = i + total_loads + total_word_stores + static_cast<size_t>(t);
            if (!match_scalar_tail_store(*stmts[stmt_idx], tail_loads[t].var, tail_loads[t].width,
                                         dst_base, dst_off0 + tail_loads[t].rel_off)) {
              stores_ok = false;
              break;
            }
            matched_tail_bytes += tail_loads[t].width;
          }
          if (!stores_ok) continue;

          int64_t total_bytes = word_bytes + matched_tail_bytes;
          std::string block_type = unaligned_block_type_name(total_bytes);
          auto copy_stmt =
              make_block_copy_stmt(block_type, *dst_base_expr, dst_off0, *src_base_expr, src_off0);
          stmts[i] = std::move(copy_stmt);
          stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + total_stmts);
          matched_block = true;
          break;
        }
      }
      if (matched_block) break;
    }

    if (matched_block) {
      try_absorb_trailing_scalar_copies(i);
      if (i > 0) {
        int64_t prev_size = 0, cur_size = 0;
        std::string pd, ps, cd, cs;
        int64_t pdo = 0, pso = 0, cdo = 0, cso = 0;
        const CExpression* pde = nullptr;
        const CExpression* pse = nullptr;
        const CExpression* dummy1 = nullptr;
        const CExpression* dummy2 = nullptr;
        if (extract_unaligned_block_copy_stmt(*stmts[i - 1], &prev_size, &pd, &pdo, &pde, &ps, &pso,
                                              &pse) &&
            extract_unaligned_block_copy_stmt(*stmts[i], &cur_size, &cd, &cdo, &dummy1, &cs, &cso,
                                              &dummy2) &&
            pd == cd && ps == cs && cdo == pdo + prev_size && cso == pso + prev_size) {
          std::string merged_type = unaligned_block_type_name(prev_size + cur_size);
          stmts[i - 1] = make_block_copy_stmt(merged_type, *pde, pdo, *pse, pso);
          stmts.erase(stmts.begin() + i);
          --i;
          try_absorb_trailing_scalar_copies(i);
        }
      }
      continue;
    }
  }

  for (auto& s : stmts) {
    ForEachSubCompoundStatement(s.get(), CollapseUnalignedBlockCopies);
  }
}

namespace {

using O0LivenessFn = std::function<bool(std::string_view)>;
using O0DeadAfterFn =
    std::function<bool(const std::vector<std::unique_ptr<CStatement>>&, size_t, std::string_view,
                       const O0LivenessFn&, const O0LivenessFn&, const O0LivenessFn&, int)>;

bool IsO0ScratchGpr(std::string_view name) {
  return name == "v0" || name == "v1" || name == "at" || name == "temp_at" ||
         absl::StartsWith(name, "temp_t") || absl::StartsWith(name, "temp_a") ||
         (name.size() == 2 && name[0] == 't' && name[1] >= '0' && name[1] <= '9');
}

bool AliasesO0Reg(std::string_view var, std::string_view reg) {
  if (var == reg) return true;
  if (var.size() >= 2 && reg.size() >= 2 && var[0] == 'f' && reg[0] == 'f') {
    std::string_view b1 = absl::EndsWith(var, "_d") ? var.substr(0, var.size() - 2) : var;
    std::string_view b2 = absl::EndsWith(reg, "_d") ? reg.substr(0, reg.size() - 2) : reg;
    return b1 == b2;
  }
  return false;
}

bool ExprReadsO0Reg(const CExpression& expr, std::string_view reg) {
  if (ExpressionUsesVariable(expr, reg)) return true;
  if (reg.size() >= 2 && reg[0] == 'f') {
    std::string alt = absl::EndsWith(reg, "_d") ? std::string(reg.substr(0, reg.size() - 2))
                                                : absl::StrCat(reg, "_d");
    return ExpressionUsesVariable(expr, alt);
  }
  return false;
}

bool StmtReferencesO0Reg(const CStatement& stmt, std::string_view reg) {
  if (StatementReferencesVariable(stmt, reg)) return true;
  if (reg.size() >= 2 && reg[0] == 'f') {
    std::string alt = absl::EndsWith(reg, "_d") ? std::string(reg.substr(0, reg.size() - 2))
                                                : absl::StrCat(reg, "_d");
    return StatementReferencesVariable(stmt, alt);
  }
  return false;
}

bool CollapseO0GotoLabelDiamonds(CompoundStatement* body,
                                 const std::function<bool(std::string_view)>& is_foldable_scratch,
                                 const O0DeadAfterFn& is_scratch_dead_after) {
  const O0LivenessFn always_dead = [](std::string_view) { return true; };
  auto count_gotos = [](const CStatement& root) {
    absl::flat_hash_map<std::string, int> counts;
    class GotoCounter : public AstVisitor {
     public:
      explicit GotoCounter(absl::flat_hash_map<std::string, int>* c) : counts_(c) {}
      void VisitGotoStatement(const GotoStatement& stmt) override { ++(*counts_)[stmt.Label()]; }

     private:
      absl::flat_hash_map<std::string, int>* counts_;
    };
    GotoCounter counter(&counts);
    counter.VisitStatement(root);
    return counts;
  };
  auto starts_with_label = [](const CompoundStatement& c, std::string* label_out) -> bool {
    if (c.Statements().empty() || c.Statements()[0] == nullptr ||
        c.Statements()[0]->Kind() != CStatementKind::kLabelStatement) {
      return false;
    }
    *label_out = static_cast<const LabelStatement&>(*c.Statements()[0]).Label();
    return true;
  };
  auto is_single_goto = [](const CompoundStatement& c, std::string* label_out) -> bool {
    if (c.Statements().size() != 1 || c.Statements()[0] == nullptr ||
        c.Statements()[0]->Kind() != CStatementKind::kGotoStatement) {
      return false;
    }
    *label_out = static_cast<const GotoStatement&>(*c.Statements()[0]).Label();
    return true;
  };

  absl::flat_hash_map<std::string, int> goto_counts = count_gotos(*body);
  bool any_goto_collapsed = false;
  bool iter_changed = true;
  while (iter_changed && !goto_counts.empty()) {
    iter_changed = false;
    std::function<void(CompoundStatement*)> collapse_comp = [&](CompoundStatement* comp) {
      if (comp == nullptr) return;
      for (auto& s : comp->MutableStatements()) {
        ForEachSubCompoundStatement(s.get(), collapse_comp);
      }
      auto& stmts = comp->MutableStatements();
      for (size_t i = 0; i < stmts.size(); ++i) {
        if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kIfStatement) continue;
        auto& ifs = static_cast<IfStatement&>(*stmts[i]);
        if (ifs.ThenBranch().Kind() != CStatementKind::kCompoundStatement) continue;
        auto* tc = static_cast<CompoundStatement*>(ifs.MutableThenBranch());
        auto* ec = (ifs.MutableElseBranch() != nullptr &&
                    ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement)
                       ? static_cast<CompoundStatement*>(ifs.MutableElseBranch())
                       : nullptr;
        std::string lbl;

        if (tc->Statements().empty() && ec != nullptr && starts_with_label(*ec, &lbl)) {
          auto neg = NegateCondition(ifs.Condition());
          if (neg != nullptr) {
            stmts[i] = CStatement::If(std::move(neg), ec->Clone());
            iter_changed = true;
            any_goto_collapsed = true;
            continue;
          }
        }

        auto extract_inner_if = [&](const CompoundStatement& branch,
                                    std::unique_ptr<CExpression>* out_cond) -> const IfStatement* {
          const auto& b_stmts = branch.Statements();
          if (b_stmts.empty()) return nullptr;
          absl::flat_hash_map<std::string, std::unique_ptr<CExpression>> env;
          std::vector<std::string> env_order;
          size_t k = 0;
          for (; k + 1 < b_stmts.size() && k < 6; ++k) {
            if (b_stmts[k] == nullptr ||
                b_stmts[k]->Kind() != CStatementKind::kExpressionStatement) {
              return nullptr;
            }
            const auto& es = static_cast<const ExpressionStatement&>(*b_stmts[k]);
            if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return nullptr;
            const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
            if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier ||
                !IsPureExpression(a.Rhs())) {
              return nullptr;
            }
            std::string lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
            if (!is_foldable_scratch(lhs)) return nullptr;
            auto rhs_sub = a.Rhs().Clone();
            for (const auto& reg : env_order) {
              rhs_sub = SubstituteIdentifier(*rhs_sub, reg,
                                             *CExpression::Identifier(absl::StrCat("__o0_", reg)));
            }
            for (const auto& reg : env_order) {
              rhs_sub = SubstituteIdentifier(*rhs_sub, absl::StrCat("__o0_", reg), *env[reg]);
            }
            if (!env.contains(lhs)) env_order.push_back(lhs);
            env[lhs] = std::move(rhs_sub);
          }
          if (k + 1 != b_stmts.size() || b_stmts[k] == nullptr ||
              b_stmts[k]->Kind() != CStatementKind::kIfStatement) {
            return nullptr;
          }
          const auto& inner_if = static_cast<const IfStatement&>(*b_stmts[k]);
          auto sub_cond = inner_if.Condition().Clone();
          for (const auto& reg : env_order) {
            sub_cond = SubstituteIdentifier(*sub_cond, reg,
                                            *CExpression::Identifier(absl::StrCat("__o0_", reg)));
          }
          for (const auto& reg : env_order) {
            sub_cond = SubstituteIdentifier(*sub_cond, absl::StrCat("__o0_", reg), *env[reg]);
          }
          for (const auto& reg : env_order) {
            if (ExprReadsO0Reg(*sub_cond, reg) || StmtReferencesO0Reg(inner_if.ThenBranch(), reg) ||
                (inner_if.ElseBranch() != nullptr &&
                 StmtReferencesO0Reg(*inner_if.ElseBranch(), reg)) ||
                !is_scratch_dead_after(stmts, i + 1, reg, always_dead, always_dead, always_dead,
                                       0)) {
              return nullptr;
            }
          }
          *out_cond = std::move(sub_cond);
          return &inner_if;
        };

        std::unique_ptr<CExpression> inner_cond;
        if (ec == nullptr) {
          if (const IfStatement* inner_if = extract_inner_if(*tc, &inner_cond);
              inner_if != nullptr && inner_if->ElseBranch() == nullptr &&
              inner_if->ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
              starts_with_label(static_cast<const CompoundStatement&>(inner_if->ThenBranch()),
                                &lbl)) {
            auto comb = CExpression::Binary("&&", ifs.Condition().Clone(), std::move(inner_cond));
            stmts[i] = CStatement::If(std::move(comb), inner_if->ThenBranch().Clone());
            iter_changed = true;
            any_goto_collapsed = true;
            continue;
          }
        }

        if (ec != nullptr && is_single_goto(*ec, &lbl) && goto_counts[lbl] == 1) {
          if (const IfStatement* inner_if = extract_inner_if(*tc, &inner_cond);
              inner_if != nullptr) {
            std::string inner_lbl;
            if (inner_if->ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
                starts_with_label(static_cast<const CompoundStatement&>(inner_if->ThenBranch()),
                                  &inner_lbl) &&
                inner_lbl == lbl) {
              auto stripped_then = std::unique_ptr<CompoundStatement>(
                  static_cast<CompoundStatement*>(inner_if->ThenBranch().Clone().release()));
              stripped_then->MutableStatements().erase(stripped_then->MutableStatements().begin());
              goto_counts[lbl] = 0;
              if (inner_if->ElseBranch() != nullptr) {
                auto neg_b = NegateCondition(*inner_cond);
                if (neg_b != nullptr) {
                  auto comb = CExpression::Binary("&&", ifs.Condition().Clone(), std::move(neg_b));
                  stmts[i] = CStatement::If(std::move(comb), inner_if->ElseBranch()->Clone(),
                                            std::move(stripped_then));
                  iter_changed = true;
                  any_goto_collapsed = true;
                  continue;
                }
              } else {
                auto neg_a = NegateCondition(ifs.Condition());
                if (neg_a != nullptr) {
                  auto comb = CExpression::Binary("||", std::move(neg_a), std::move(inner_cond));
                  stmts[i] = CStatement::If(std::move(comb), std::move(stripped_then));
                  iter_changed = true;
                  any_goto_collapsed = true;
                  continue;
                }
              }
            }
          }
        }

        if (ec != nullptr && is_single_goto(*tc, &lbl) && goto_counts[lbl] == 1) {
          if (const IfStatement* inner_if = extract_inner_if(*ec, &inner_cond);
              inner_if != nullptr) {
            std::string inner_lbl;
            if (inner_if->ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
                starts_with_label(static_cast<const CompoundStatement&>(inner_if->ThenBranch()),
                                  &inner_lbl) &&
                inner_lbl == lbl) {
              auto stripped_then = std::unique_ptr<CompoundStatement>(
                  static_cast<CompoundStatement*>(inner_if->ThenBranch().Clone().release()));
              stripped_then->MutableStatements().erase(stripped_then->MutableStatements().begin());
              goto_counts[lbl] = 0;
              if (inner_if->ElseBranch() == nullptr) {
                auto comb =
                    CExpression::Binary("||", ifs.Condition().Clone(), std::move(inner_cond));
                stmts[i] = CStatement::If(std::move(comb), std::move(stripped_then));
                iter_changed = true;
                any_goto_collapsed = true;
                continue;
              } else {
                auto neg_x = NegateCondition(ifs.Condition());
                auto neg_b = NegateCondition(*inner_cond);
                if (neg_x != nullptr && neg_b != nullptr) {
                  auto comb = CExpression::Binary("&&", std::move(neg_x), std::move(neg_b));
                  stmts[i] = CStatement::If(std::move(comb), inner_if->ElseBranch()->Clone(),
                                            std::move(stripped_then));
                  iter_changed = true;
                  any_goto_collapsed = true;
                  continue;
                }
              }
            }
          }
        }
      }
    };
    collapse_comp(body);
  }
  return any_goto_collapsed;
}

void PushO0BranchReturnsAndStripDefaultBreak(CompoundStatement* body) {
  std::function<bool(const CompoundStatement&, std::string_view)> ends_with_assign_to =
      [&](const CompoundStatement& c, std::string_view r) -> bool {
    if (c.Statements().empty() || c.Statements().back() == nullptr) return false;
    const CStatement& last = *c.Statements().back();
    if (last.Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(last);
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        return a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
               static_cast<const IdentifierExpression&>(a.Lhs()).Name() == r;
      }
      return false;
    }
    if (last.Kind() == CStatementKind::kIfStatement) {
      const auto& ifs = static_cast<const IfStatement&>(last);
      if (ifs.ElseBranch() == nullptr ||
          ifs.ThenBranch().Kind() != CStatementKind::kCompoundStatement ||
          ifs.ElseBranch()->Kind() != CStatementKind::kCompoundStatement) {
        return false;
      }
      return ends_with_assign_to(static_cast<const CompoundStatement&>(ifs.ThenBranch()), r) &&
             ends_with_assign_to(static_cast<const CompoundStatement&>(*ifs.ElseBranch()), r);
    }
    return false;
  };

  std::function<void(CompoundStatement*, std::string_view, const CExpression&)> push_return_into =
      [&](CompoundStatement* c, std::string_view r, const CExpression& ret_expr) {
        auto& stmts = c->MutableStatements();
        CStatement& last = *stmts.back();
        if (last.Kind() == CStatementKind::kExpressionStatement) {
          const auto& es = static_cast<const ExpressionStatement&>(last);
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          stmts.back() = CStatement::Return(SubstituteIdentifier(ret_expr, r, a.Rhs()));
          return;
        }
        if (last.Kind() == CStatementKind::kIfStatement) {
          auto& ifs = static_cast<IfStatement&>(last);
          auto* tc = static_cast<CompoundStatement*>(ifs.MutableThenBranch());
          auto* ec = static_cast<CompoundStatement*>(ifs.MutableElseBranch());
          const bool unwrap_else = (tc->Statements().size() == 1 || ec->Statements().size() == 1);
          push_return_into(tc, r, ret_expr);
          push_return_into(ec, r, ret_expr);
          if (unwrap_else) {
            auto else_clone = std::unique_ptr<CompoundStatement>(
                static_cast<CompoundStatement*>(ec->Clone().release()));
            stmts.back() = CStatement::If(ifs.Condition().Clone(), tc->Clone());
            for (auto& es_stmt : else_clone->MutableStatements()) {
              stmts.push_back(std::move(es_stmt));
            }
          }
        }
      };

  auto& top_stmts = body->MutableStatements();
  if (top_stmts.size() >= 2 && top_stmts.back() != nullptr &&
      top_stmts[top_stmts.size() - 2] != nullptr &&
      top_stmts.back()->Kind() == CStatementKind::kReturnStatement &&
      top_stmts[top_stmts.size() - 2]->Kind() == CStatementKind::kIfStatement) {
    const auto& rs = static_cast<const ReturnStatement&>(*top_stmts.back());
    if (rs.ReturnValue() != nullptr) {
      for (std::string_view r : {"v0", "f0", "f0_d"}) {
        if (CountIdentifierOccurrencesInExpression(*rs.ReturnValue(), r) == 1) {
          const auto& ifs = static_cast<const IfStatement&>(*top_stmts[top_stmts.size() - 2]);
          if (ifs.ElseBranch() != nullptr &&
              ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
              ifs.ElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
              ends_with_assign_to(static_cast<const CompoundStatement&>(ifs.ThenBranch()), r) &&
              ends_with_assign_to(static_cast<const CompoundStatement&>(*ifs.ElseBranch()), r)) {
            auto ret_val_clone = rs.ReturnValue()->Clone();
            top_stmts.pop_back();
            push_return_into(body, r, *ret_val_clone);
            break;
          }
        }
      }
    }
  }

  std::function<void(CompoundStatement*)> strip_default_break = [&](CompoundStatement* comp) {
    if (comp == nullptr) return;
    for (auto& s : comp->MutableStatements()) {
      if (s == nullptr) continue;
      if (s->Kind() == CStatementKind::kSwitchStatement) {
        auto& ss = static_cast<SwitchStatement&>(*s);
        auto& cases = ss.MutableCases();
        if (!cases.empty() && cases.back().is_default && cases.back().case_values.empty()) {
          const auto* b = cases.back().body.get();
          if (b == nullptr || b->Statements().empty() ||
              (b->Statements().size() == 1 && b->Statements()[0] != nullptr &&
               b->Statements()[0]->Kind() == CStatementKind::kBreakStatement)) {
            cases.pop_back();
          }
        }
      }
      ForEachSubCompoundStatement(s.get(), strip_default_break);
    }
  };
  strip_default_break(body);
}

void RewritePositiveOffsetPointerDereferences(CompoundStatement* body) {
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

bool ExtractO0PureBreak(const IfStatement& ifs) {
  if (ifs.ElseBranch() != nullptr) return false;
  if (ifs.ThenBranch().Kind() == CStatementKind::kBreakStatement) return true;
  if (ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
    const auto& tc = static_cast<const CompoundStatement&>(ifs.ThenBranch());
    return tc.Statements().size() == 1 && tc.Statements()[0] != nullptr &&
           tc.Statements()[0]->Kind() == CStatementKind::kBreakStatement;
  }
  return false;
}

void FoldO0WhileHeaders(std::vector<std::unique_ptr<CStatement>>& stmts,
                        const std::function<bool(std::string_view)>& is_foldable_scratch,
                        const O0DeadAfterFn& is_scratch_dead_after,
                        const O0LivenessFn& dead_on_falloff, const O0LivenessFn& dead_on_break,
                        const O0LivenessFn& dead_on_continue) {
  const O0LivenessFn always_dead = [](std::string_view) { return true; };
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kWhileStatement) {
      continue;
    }
    O0LivenessFn after_loop = [&](std::string_view r) {
      return is_scratch_dead_after(stmts, i + 1, r, dead_on_falloff, dead_on_break,
                                   dead_on_continue, 0);
    };

    {
      const auto& ws = static_cast<const WhileStatement&>(*stmts[i]);
      if (ws.Condition().Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(ws.Condition()).Value() == 1 &&
          ws.Body().Kind() == CStatementKind::kCompoundStatement) {
        auto body_clone = std::unique_ptr<CompoundStatement>(
            static_cast<CompoundStatement*>(ws.Body().Clone().release()));
        auto& b_stmts = body_clone->MutableStatements();
        if (b_stmts.size() >= 3 && b_stmts[0] != nullptr && b_stmts[1] != nullptr &&
            b_stmts[2] != nullptr && b_stmts[0]->Kind() == CStatementKind::kExpressionStatement &&
            b_stmts[1]->Kind() == CStatementKind::kExpressionStatement &&
            b_stmts[2]->Kind() == CStatementKind::kIfStatement) {
          const auto& es0 = static_cast<const ExpressionStatement&>(*b_stmts[0]);
          const auto& es1 = static_cast<const ExpressionStatement&>(*b_stmts[1]);
          const auto& ifs = static_cast<const IfStatement&>(*b_stmts[2]);
          if (ExtractO0PureBreak(ifs) &&
              es0.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
              es1.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
              ifs.Condition().Kind() == CExpressionKind::kBinaryExpression) {
            const auto& a0 = static_cast<const AssignmentExpression&>(es0.Expression());
            const auto& a1 = static_cast<const AssignmentExpression&>(es1.Expression());
            const auto& cmp = static_cast<const BinaryExpression&>(ifs.Condition());
            if (a0.Op() == "=" && a1.Op() == "=" &&
                a0.Lhs().Kind() == CExpressionKind::kIdentifier &&
                a0.Rhs().Kind() == CExpressionKind::kBinaryExpression &&
                a1.Lhs().Kind() == CExpressionKind::kIdentifier &&
                a1.Rhs().Kind() == CExpressionKind::kIdentifier && cmp.Op() == "==" &&
                cmp.Lhs().Kind() == CExpressionKind::kIdentifier &&
                cmp.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
                static_cast<int32_t>(
                    static_cast<const IntegerLiteralExpression&>(cmp.Rhs()).Value()) == -1) {
              std::string vx = static_cast<const IdentifierExpression&>(a0.Lhs()).Name();
              const auto& b0 = static_cast<const BinaryExpression&>(a0.Rhs());
              std::string var_name = static_cast<const IdentifierExpression&>(a1.Lhs()).Name();
              if (is_foldable_scratch(vx) && !is_foldable_scratch(var_name) &&
                  static_cast<const IdentifierExpression&>(a1.Rhs()).Name() == vx &&
                  static_cast<const IdentifierExpression&>(cmp.Lhs()).Name() == vx &&
                  b0.Lhs().Kind() == CExpressionKind::kIdentifier &&
                  static_cast<const IdentifierExpression&>(b0.Lhs()).Name() == var_name &&
                  b0.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
                  ((b0.Op() == "-" &&
                    static_cast<const IntegerLiteralExpression&>(b0.Rhs()).Value() == 1) ||
                   (b0.Op() == "+" &&
                    static_cast<const IntegerLiteralExpression&>(b0.Rhs()).Value() == -1)) &&
                  after_loop(vx) &&
                  is_scratch_dead_after(b_stmts, 3, vx, always_dead, after_loop, always_dead, 0)) {
                b_stmts.erase(b_stmts.begin(), b_stmts.begin() + 3);
                auto dec_expr = CExpression::Unary("--", CExpression::Identifier(var_name),
                                                   /*is_prefix=*/false);
                auto cond_expr =
                    CExpression::Binary("!=", std::move(dec_expr), CExpression::Integer(0));
                stmts[i] = CStatement::While(std::move(cond_expr), std::move(body_clone));
              }
            }
          }
        }
      }
    }

    for (int pass = 0; pass < 4; ++pass) {
      if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kWhileStatement) break;
      const auto& ws = static_cast<const WhileStatement&>(*stmts[i]);
      if (ws.Body().Kind() != CStatementKind::kCompoundStatement) break;
      const bool is_while_true =
          ws.Condition().Kind() == CExpressionKind::kIntegerLiteral &&
          static_cast<const IntegerLiteralExpression&>(ws.Condition()).Value() == 1;
      auto body_clone = std::unique_ptr<CompoundStatement>(
          static_cast<CompoundStatement*>(ws.Body().Clone().release()));
      auto& b_stmts = body_clone->MutableStatements();
      absl::flat_hash_map<std::string, std::unique_ptr<CExpression>> env;
      std::vector<std::string> env_order;
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
        if (!is_foldable_scratch(lhs)) break;
        auto rhs_sub = a.Rhs().Clone();
        for (const auto& reg : env_order) {
          rhs_sub = SubstituteIdentifier(*rhs_sub, reg,
                                         *CExpression::Identifier(absl::StrCat("__o0_", reg)));
        }
        for (const auto& reg : env_order) {
          rhs_sub = SubstituteIdentifier(*rhs_sub, absl::StrCat("__o0_", reg), *env[reg]);
        }
        if (!env.contains(lhs)) env_order.push_back(lhs);
        env[lhs] = std::move(rhs_sub);
      }
      bool folded_break = false;
      if (k < b_stmts.size() && b_stmts[k] != nullptr &&
          b_stmts[k]->Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(*b_stmts[k]);
        if (ExtractO0PureBreak(ifs)) {
          auto sub_cond = ifs.Condition().Clone();
          for (const auto& reg : env_order) {
            sub_cond = SubstituteIdentifier(*sub_cond, reg,
                                            *CExpression::Identifier(absl::StrCat("__o0_", reg)));
          }
          for (const auto& reg : env_order) {
            sub_cond = SubstituteIdentifier(*sub_cond, absl::StrCat("__o0_", reg), *env[reg]);
          }
          bool any_scratch_left = false;
          for (const auto& reg : env_order) {
            if (ExprReadsO0Reg(*sub_cond, reg) ||
                (!is_while_true && ExprReadsO0Reg(ws.Condition(), reg))) {
              any_scratch_left = true;
              break;
            }
          }
          if (!any_scratch_left) {
            bool env_dead = true;
            for (const auto& reg : env_order) {
              if (!after_loop(reg) || !is_scratch_dead_after(b_stmts, k + 1, reg, always_dead,
                                                             after_loop, always_dead, 0)) {
                env_dead = false;
                break;
              }
            }
            if (env_dead) {
              auto loop_cond = NegateCondition(*sub_cond);
              if (loop_cond != nullptr) {
                b_stmts.erase(b_stmts.begin(), b_stmts.begin() + k + 1);
                std::unique_ptr<CExpression> combined_cond =
                    is_while_true
                        ? std::move(loop_cond)
                        : CExpression::Binary("&&", ws.Condition().Clone(), std::move(loop_cond));
                stmts[i] = CStatement::While(std::move(combined_cond), std::move(body_clone));
                folded_break = true;
              }
            }
          }
        }
      }
      if (!folded_break) break;
    }
  }
}

}  // namespace

void RewriteFramePointerStructOffsetAccesses(CompoundStatement* body,
                                             const std::vector<CParameter>* parameters) {
  if (body == nullptr) return;

  auto is_param_name = [&](std::string_view name) -> bool {
    if (parameters == nullptr) return false;
    for (const auto& p : *parameters) {
      if (p.name == name) return true;
    }
    return false;
  };

  if (parameters != nullptr) {
    for (int k = 0; k < 4; ++k) {
      std::string arg_name = absl::StrFormat("arg%d", k);
      if (!is_param_name(arg_name) && StatementReferencesVariable(*body, arg_name)) {
        std::string temp_name = absl::StrFormat("temp_a%d", k);
        std::unique_ptr<CStatement> cloned = body->Clone();
        cloned = SubstituteIdentifierInStatement(std::move(cloned), arg_name, temp_name);
        *body = std::move(*static_cast<CompoundStatement*>(cloned.get()));
      }
    }
  }

  auto is_foldable_scratch = [&](std::string_view name) {
    if (IsO0ScratchGpr(name)) return true;
    if (name == "f0" || name == "f0_d" || name == "f2" || name == "f2_d" || name == "f4" ||
        name == "f4_d") {
      return true;
    }
    if (parameters != nullptr &&
        (name == "f12" || name == "f12_d" || name == "f14" || name == "f14_d")) {
      std::string_view base = absl::EndsWith(name, "_d") ? name.substr(0, name.size() - 2) : name;
      return !is_param_name(name) && !is_param_name(base);
    }
    return false;
  };

  auto is_scratch_gpr = IsO0ScratchGpr;
  auto aliases_reg = AliasesO0Reg;
  auto expr_reads_reg = ExprReadsO0Reg;
  auto stmt_references_reg = StmtReferencesO0Reg;

  using LivenessFn = O0LivenessFn;
  const LivenessFn always_dead = [](std::string_view) { return true; };
  const LivenessFn never_dead = [](std::string_view) { return false; };

  std::function<bool(const std::vector<std::unique_ptr<CStatement>>&, size_t, std::string_view,
                     const LivenessFn&, const LivenessFn&, const LivenessFn&, int)>
      is_scratch_dead_after = [&](const std::vector<std::unique_ptr<CStatement>>& stmts,
                                  size_t start_idx, std::string_view reg,
                                  const LivenessFn& dead_on_falloff,
                                  const LivenessFn& dead_on_break,
                                  const LivenessFn& dead_on_continue, int depth) -> bool {
    for (size_t idx = start_idx; idx < stmts.size(); ++idx) {
      if (stmts[idx] == nullptr || stmts[idx]->Kind() == CStatementKind::kLabelStatement) {
        continue;
      }
      if (stmts[idx]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es = static_cast<const ExpressionStatement&>(*stmts[idx]);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
              aliases_reg(static_cast<const IdentifierExpression&>(a.Lhs()).Name(), reg)) {
            return !expr_reads_reg(a.Rhs(), reg);
          }
          if (a.Op() == "=" && is_foldable_scratch(reg) && !expr_reads_reg(a.Lhs(), reg) &&
              !expr_reads_reg(a.Rhs(), reg) && a.Rhs().Kind() == CExpressionKind::kCallExpression) {
            return true;
          }
        } else if (es.Expression().Kind() == CExpressionKind::kCallExpression &&
                   is_foldable_scratch(reg) && !expr_reads_reg(es.Expression(), reg)) {
          return true;
        }
      }
      if (stmts[idx]->Kind() == CStatementKind::kReturnStatement) {
        const auto& rs = static_cast<const ReturnStatement&>(*stmts[idx]);
        return rs.ReturnValue() == nullptr || !expr_reads_reg(*rs.ReturnValue(), reg);
      }
      if (stmts[idx]->Kind() == CStatementKind::kBreakStatement) {
        return dead_on_break(reg);
      }
      if (stmts[idx]->Kind() == CStatementKind::kContinueStatement) {
        return dead_on_continue(reg);
      }
      if (stmts[idx]->Kind() == CStatementKind::kIfStatement && depth < 4) {
        const auto& ifs = static_cast<const IfStatement&>(*stmts[idx]);
        if (expr_reads_reg(ifs.Condition(), reg)) return false;
        if (!stmt_references_reg(ifs, reg) && !StatementContainsControlFlowJump(ifs) &&
            !HasReturnValue(ifs)) {
          continue;
        }
        if (ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
            (ifs.ElseBranch() == nullptr ||
             ifs.ElseBranch()->Kind() == CStatementKind::kCompoundStatement)) {
          LivenessFn after_if = [&](std::string_view r) {
            return is_scratch_dead_after(stmts, idx + 1, r, dead_on_falloff, dead_on_break,
                                         dead_on_continue, depth + 1);
          };
          const auto& then_c = static_cast<const CompoundStatement&>(ifs.ThenBranch());
          if (!is_scratch_dead_after(then_c.Statements(), 0, reg, after_if, dead_on_break,
                                     dead_on_continue, depth + 1)) {
            return false;
          }
          if (ifs.ElseBranch() != nullptr) {
            const auto& else_c = static_cast<const CompoundStatement&>(*ifs.ElseBranch());
            if (else_c.Statements().size() == 1 && else_c.Statements()[0] != nullptr &&
                else_c.Statements()[0]->Kind() == CStatementKind::kGotoStatement &&
                !stmt_references_reg(ifs.ThenBranch(), reg) &&
                StatementContainsLabel(ifs.ThenBranch())) {
              return after_if(reg);
            }
            return is_scratch_dead_after(else_c.Statements(), 0, reg, after_if, dead_on_break,
                                         dead_on_continue, depth + 1);
          }
          return after_if(reg);
        }
      }
      if (stmts[idx]->Kind() == CStatementKind::kWhileStatement) {
        const auto& ws = static_cast<const WhileStatement&>(*stmts[idx]);
        if (expr_reads_reg(ws.Condition(), reg)) return false;
        if (!stmt_references_reg(ws, reg) && !StatementContainsGoto(ws) &&
            !StatementContainsLabel(ws) && !HasReturnValue(ws)) {
          continue;
        }
        if (depth < 4 && ws.Body().Kind() == CStatementKind::kCompoundStatement) {
          LivenessFn after_while = [&](std::string_view r) {
            return is_scratch_dead_after(stmts, idx + 1, r, dead_on_falloff, dead_on_break,
                                         dead_on_continue, depth + 1);
          };
          const auto& ws_c = static_cast<const CompoundStatement&>(ws.Body());
          return after_while(reg) && is_scratch_dead_after(ws_c.Statements(), 0, reg, always_dead,
                                                           after_while, always_dead, depth + 1);
        }
      }
      if (!stmt_references_reg(*stmts[idx], reg) &&
          !StatementContainsControlFlowJump(*stmts[idx])) {
        continue;
      }
      return false;
    }
    return dead_on_falloff(reg);
  };

  std::function<bool(const CExpression&, std::string_view)> is_evaluated_first =
      [&](const CExpression& expr, std::string_view vx) -> bool {
    if (expr.Kind() == CExpressionKind::kIdentifier) {
      return static_cast<const IdentifierExpression&>(expr).Name() == vx;
    }
    if (expr.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& u = static_cast<const UnaryExpression&>(expr);
      return u.Op() != "++" && u.Op() != "--" && is_evaluated_first(u.Operand(), vx);
    }
    if (expr.Kind() == CExpressionKind::kCastExpression) {
      return is_evaluated_first(static_cast<const CastExpression&>(expr).Operand(), vx);
    }
    if (expr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& b = static_cast<const BinaryExpression&>(expr);
      if (b.Op() == "&&" || b.Op() == "||") return false;
      if (is_evaluated_first(b.Lhs(), vx)) return true;
      return b.Lhs().Kind() == CExpressionKind::kIntegerLiteral && is_evaluated_first(b.Rhs(), vx);
    }
    if (expr.Kind() == CExpressionKind::kCallExpression) {
      const auto& c = static_cast<const CallExpression&>(expr);
      if (c.Callee().Kind() != CExpressionKind::kIdentifier || c.Arguments().empty()) return false;
      for (size_t ai = 1; ai < c.Arguments().size(); ++ai) {
        if (c.Arguments()[ai]->Kind() != CExpressionKind::kIntegerLiteral) return false;
      }
      return is_evaluated_first(*c.Arguments()[0], vx);
    }
    return false;
  };

  std::function<void(CompoundStatement*, const LivenessFn&, const LivenessFn&, const LivenessFn&)>
      fold_o0_seq = [&](CompoundStatement* comp, const LivenessFn& dead_on_falloff,
                        const LivenessFn& dead_on_break, const LivenessFn& dead_on_continue) {
        if (comp == nullptr) return;
        auto& stmts = comp->MutableStatements();

        FoldO0WhileHeaders(stmts, is_foldable_scratch, is_scratch_dead_after, dead_on_falloff,
                           dead_on_break, dead_on_continue);

        // 2. Recurse into sub-compound statements with exact liveness callbacks.
        for (size_t i = 0; i < stmts.size(); ++i) {
          if (stmts[i] == nullptr) continue;
          LivenessFn after_stmt = [&](std::string_view r) {
            return is_scratch_dead_after(stmts, i + 1, r, dead_on_falloff, dead_on_break,
                                         dead_on_continue, 0);
          };
          if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
            ForEachSubCompoundStatement(stmts[i].get(), [&](CompoundStatement* sub) {
              fold_o0_seq(sub, after_stmt, dead_on_break, dead_on_continue);
            });
            continue;
          }
          if (stmts[i]->Kind() == CStatementKind::kWhileStatement) {
            auto& ws = static_cast<WhileStatement&>(*stmts[i]);
            if (ws.Body().Kind() == CStatementKind::kCompoundStatement) {
              auto* b_comp = static_cast<CompoundStatement*>(ws.MutableBody());
              LivenessFn while_backedge = [&](std::string_view r) {
                return !expr_reads_reg(ws.Condition(), r) && after_stmt(r) &&
                       is_scratch_dead_after(b_comp->Statements(), 0, r, always_dead, after_stmt,
                                             always_dead, 0);
              };
              fold_o0_seq(b_comp, while_backedge, after_stmt, while_backedge);
              continue;
            }
          }
          if (stmts[i]->Kind() == CStatementKind::kDoWhileStatement) {
            auto& dws = static_cast<DoWhileStatement&>(*stmts[i]);
            if (dws.Body().Kind() == CStatementKind::kCompoundStatement) {
              auto* b_comp = static_cast<CompoundStatement*>(dws.MutableBody());
              LivenessFn do_backedge = [&](std::string_view r) {
                return !expr_reads_reg(dws.Condition(), r) && after_stmt(r) &&
                       is_scratch_dead_after(b_comp->Statements(), 0, r, always_dead, after_stmt,
                                             always_dead, 0);
              };
              fold_o0_seq(b_comp, do_backedge, after_stmt, do_backedge);
              continue;
            }
          }
          if (stmts[i]->Kind() == CStatementKind::kForStatement) {
            auto& fs = static_cast<ForStatement&>(*stmts[i]);
            if (fs.Body().Kind() == CStatementKind::kCompoundStatement) {
              auto* b_comp = static_cast<CompoundStatement*>(fs.MutableBody());
              LivenessFn for_backedge = [&](std::string_view r) {
                if (fs.Step() != nullptr && expr_reads_reg(*fs.Step(), r)) return false;
                if (fs.Condition() != nullptr && expr_reads_reg(*fs.Condition(), r)) return false;
                return after_stmt(r) &&
                       is_scratch_dead_after(b_comp->Statements(), 0, r, always_dead, after_stmt,
                                             always_dead, 0);
              };
              fold_o0_seq(b_comp, for_backedge, after_stmt, for_backedge);
              continue;
            }
          }
          if (stmts[i]->Kind() == CStatementKind::kSwitchStatement) {
            auto& ss = static_cast<SwitchStatement&>(*stmts[i]);
            auto& cases = ss.MutableCases();
            for (size_t ci = 0; ci < cases.size(); ++ci) {
              if (cases[ci].body == nullptr) continue;
              const bool is_last_case = (ci + 1 == cases.size());
              fold_o0_seq(cases[ci].body.get(), is_last_case ? after_stmt : never_dead, after_stmt,
                          dead_on_continue);
            }
            continue;
          }
          ForEachSubCompoundStatement(stmts[i].get(), [&](CompoundStatement* sub) {
            fold_o0_seq(sub, never_dead, never_dead, never_dead);
          });
        }
        FoldO0WhileHeaders(stmts, is_foldable_scratch, is_scratch_dead_after, dead_on_falloff,
                           dead_on_break, dead_on_continue);

        // 3. Recover `-O0` ternaries from `if (cond) { vX = e1; } else { vX = e2; }` followed by
        //    an assignment statement using `vX`.
        for (size_t i = 0; i + 1 < stmts.size(); ++i) {
          if (stmts[i] == nullptr || stmts[i + 1] == nullptr ||
              stmts[i]->Kind() != CStatementKind::kIfStatement ||
              stmts[i + 1]->Kind() != CStatementKind::kExpressionStatement) {
            continue;
          }
          const auto& ifs = static_cast<const IfStatement&>(*stmts[i]);
          const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
          if (ifs.ElseBranch() == nullptr ||
              ifs.ThenBranch().Kind() != CStatementKind::kCompoundStatement ||
              ifs.ElseBranch()->Kind() != CStatementKind::kCompoundStatement ||
              next_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
            continue;
          }
          const auto& tc = static_cast<const CompoundStatement&>(ifs.ThenBranch());
          const auto& ec = static_cast<const CompoundStatement&>(*ifs.ElseBranch());
          if (tc.Statements().size() != 1 || ec.Statements().size() != 1 ||
              tc.Statements()[0] == nullptr || ec.Statements()[0] == nullptr ||
              tc.Statements()[0]->Kind() != CStatementKind::kExpressionStatement ||
              ec.Statements()[0]->Kind() != CStatementKind::kExpressionStatement) {
            continue;
          }
          const auto& tes = static_cast<const ExpressionStatement&>(*tc.Statements()[0]);
          const auto& ees = static_cast<const ExpressionStatement&>(*ec.Statements()[0]);
          if (tes.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
              ees.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
            continue;
          }
          const auto& ta = static_cast<const AssignmentExpression&>(tes.Expression());
          const auto& ea = static_cast<const AssignmentExpression&>(ees.Expression());
          if (ta.Op() == "=" && ea.Op() == "=" && ta.Lhs().Kind() == CExpressionKind::kIdentifier &&
              ea.Lhs().Kind() == CExpressionKind::kIdentifier && IsPureExpression(ta.Rhs()) &&
              IsPureExpression(ea.Rhs())) {
            std::string vx = static_cast<const IdentifierExpression&>(ta.Lhs()).Name();
            if (vx == static_cast<const IdentifierExpression&>(ea.Lhs()).Name() &&
                is_foldable_scratch(vx) &&
                CountIdentifierOccurrencesInExpression(next_es.Expression(), vx) == 1) {
              auto tern =
                  CExpression::Ternary(ifs.Condition().Clone(), ta.Rhs().Clone(), ea.Rhs().Clone());
              stmts[i] = CStatement::Expression(
                  CExpression::Assignment("=", CExpression::Identifier(vx), std::move(tern)));
            }
          }
        }

        // 4. Recover `-O0` constant chained assignments `vX = C; lhs1 = C; lhs2 = C;` before
        //    `vX = C;` is erased as a dead constant load.
        for (size_t i = 0; i + 2 < stmts.size(); ++i) {
          if (stmts[i] == nullptr || stmts[i + 1] == nullptr || stmts[i + 2] == nullptr ||
              stmts[i]->Kind() != CStatementKind::kExpressionStatement ||
              stmts[i + 1]->Kind() != CStatementKind::kExpressionStatement ||
              stmts[i + 2]->Kind() != CStatementKind::kExpressionStatement) {
            continue;
          }
          const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
          const auto& es1 = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
          const auto& es2 = static_cast<const ExpressionStatement&>(*stmts[i + 2]);
          if (es0.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
              es1.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
              es2.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
            continue;
          }
          const auto& a0 = static_cast<const AssignmentExpression&>(es0.Expression());
          const auto& a1 = static_cast<const AssignmentExpression&>(es1.Expression());
          const auto& a2 = static_cast<const AssignmentExpression&>(es2.Expression());
          if (a0.Op() != "=" || a1.Op() != "=" || a2.Op() != "=" ||
              a0.Lhs().Kind() != CExpressionKind::kIdentifier ||
              a0.Rhs().Kind() != CExpressionKind::kIntegerLiteral ||
              a1.Lhs().Kind() != CExpressionKind::kIdentifier) {
            continue;
          }
          std::string vx = static_cast<const IdentifierExpression&>(a0.Lhs()).Name();
          std::string lhs1 = static_cast<const IdentifierExpression&>(a1.Lhs()).Name();
          int64_t cval = static_cast<const IntegerLiteralExpression&>(a0.Rhs()).Value();
          if (!is_foldable_scratch(vx) || is_foldable_scratch(lhs1)) continue;
          auto matches_rhs = [&](const CExpression& rhs) {
            if (rhs.Kind() == CExpressionKind::kIntegerLiteral) {
              return static_cast<const IntegerLiteralExpression&>(rhs).Value() == cval;
            }
            return rhs.Kind() == CExpressionKind::kIdentifier &&
                   static_cast<const IdentifierExpression&>(rhs).Name() == vx;
          };
          if (matches_rhs(a1.Rhs()) && matches_rhs(a2.Rhs()) && !expr_reads_reg(a2.Lhs(), vx) &&
              !(a2.Lhs().Kind() == CExpressionKind::kIdentifier &&
                is_foldable_scratch(static_cast<const IdentifierExpression&>(a2.Lhs()).Name())) &&
              is_scratch_dead_after(stmts, i + 3, vx, dead_on_falloff, dead_on_break,
                                    dead_on_continue, 0)) {
            auto inner = CExpression::Assignment("=", a1.Lhs().Clone(), a0.Rhs().Clone());
            auto outer = CExpression::Assignment("=", a2.Lhs().Clone(), std::move(inner));
            stmts[i] = CStatement::Expression(std::move(outer));
            stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 3);
          }
        }

        // 5. Fixed-point folding of post-increment idioms, chained assignments, single-use
        //    scratch variables, and dead pure scratch assignments.
        bool changed = true;
        while (changed) {
          changed = false;

          // 5a. Fold post-increment pointer stores and loads (`stride == 1, 2, 4, 8`) and
          //     scalar/lvalue post-increments (`stride == 1` or `-1`).
          for (size_t i = 0; i + 2 < stmts.size(); ++i) {
            if (stmts[i] == nullptr || stmts[i + 1] == nullptr || stmts[i + 2] == nullptr ||
                stmts[i]->Kind() != CStatementKind::kExpressionStatement ||
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
            if (a0.Op() != "=" || a1.Op() != "=" ||
                a0.Lhs().Kind() != CExpressionKind::kIdentifier ||
                a1.Rhs().Kind() != CExpressionKind::kBinaryExpression) {
              continue;
            }
            std::string vx = static_cast<const IdentifierExpression&>(a0.Lhs()).Name();
            const auto& b1 = static_cast<const BinaryExpression&>(a1.Rhs());
            if (!is_scratch_gpr(vx) || b1.Lhs().Kind() != CExpressionKind::kIdentifier ||
                static_cast<const IdentifierExpression&>(b1.Lhs()).Name() != vx ||
                b1.Rhs().Kind() != CExpressionKind::kIntegerLiteral) {
              continue;
            }
            int64_t raw_imm = static_cast<const IntegerLiteralExpression&>(b1.Rhs()).Value();
            int64_t stride = (b1.Op() == "+") ? raw_imm : ((b1.Op() == "-") ? -raw_imm : 0);
            if (stride == 0) continue;

            // Pointer dereference post-increment (`*(T*)ptr++` or `*((T*)ptr)++`):
            if (stride > 0 && a0.Rhs().Kind() == CExpressionKind::kIdentifier &&
                a1.Lhs().Kind() == CExpressionKind::kIdentifier &&
                stmts[i + 2]->Kind() == CStatementKind::kExpressionStatement) {
              std::string ptr_var = static_cast<const IdentifierExpression&>(a0.Rhs()).Name();
              if (static_cast<const IdentifierExpression&>(a1.Lhs()).Name() == ptr_var) {
                const auto& es2 = static_cast<const ExpressionStatement&>(*stmts[i + 2]);
                if (es2.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& a2 = static_cast<const AssignmentExpression&>(es2.Expression());
                  auto match_deref_of_vx = [&](const CExpression& expr,
                                               CType* out_ptr_type) -> bool {
                    if (expr.Kind() != CExpressionKind::kUnaryExpression) return false;
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
                    if (!elem_sz.has_value() || *elem_sz != stride) return false;
                    *out_ptr_type = cast.TargetType();
                    return true;
                  };
                  auto make_ptr_post_inc_deref = [&](const CType& ptr_type) {
                    if (stride == 1) {
                      auto post_inc = CExpression::Unary("++", CExpression::Identifier(ptr_var),
                                                         /*is_prefix=*/false);
                      return CExpression::Unary("*",
                                                CExpression::Cast(ptr_type, std::move(post_inc)));
                    }
                    auto cast_ptr = CExpression::Cast(ptr_type, CExpression::Identifier(ptr_var));
                    auto post_inc =
                        CExpression::Unary("++", std::move(cast_ptr), /*is_prefix=*/false);
                    return CExpression::Unary("*", std::move(post_inc));
                  };
                  CType ptr_type = CType::Void();
                  if (a2.Op() == "=" && match_deref_of_vx(a2.Lhs(), &ptr_type) &&
                      !expr_reads_reg(a2.Rhs(), vx) &&
                      is_scratch_dead_after(stmts, i + 3, vx, dead_on_falloff, dead_on_break,
                                            dead_on_continue, 0)) {
                    stmts[i] = CStatement::Expression(CExpression::Assignment(
                        "=", make_ptr_post_inc_deref(ptr_type), a2.Rhs().Clone()));
                    stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 3);
                    changed = true;
                    break;
                  }
                  if (a2.Op() == "=" && a2.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      match_deref_of_vx(a2.Rhs(), &ptr_type)) {
                    std::string vy = static_cast<const IdentifierExpression&>(a2.Lhs()).Name();
                    if (vy == vx || is_scratch_dead_after(stmts, i + 3, vx, dead_on_falloff,
                                                          dead_on_break, dead_on_continue, 0)) {
                      stmts[i] = CStatement::Expression(CExpression::Assignment(
                          "=", a2.Lhs().Clone(), make_ptr_post_inc_deref(ptr_type)));
                      stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 3);
                      changed = true;
                      break;
                    }
                  }
                }
              }
            }

            // Scalar / lvalue post-increment or post-decrement (`lval++` / `lval--`):
            if ((stride == 1 || stride == -1) && IsPureExpression(a0.Rhs()) &&
                !expr_reads_reg(a0.Rhs(), vx) && a0.Rhs().ToString() == a1.Lhs().ToString()) {
              std::string lval_str = a0.Rhs().ToString();
              const char* inc_op = (stride == 1) ? "++" : "--";
              if (stmts[i + 2]->Kind() == CStatementKind::kExpressionStatement) {
                const auto& es2 = static_cast<const ExpressionStatement&>(*stmts[i + 2]);
                if (es2.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& a2 = static_cast<const AssignmentExpression&>(es2.Expression());
                  bool vx_dead_after_a2 =
                      (a2.Op() == "=" && a2.Lhs().Kind() == CExpressionKind::kIdentifier &&
                       static_cast<const IdentifierExpression&>(a2.Lhs()).Name() == vx) ||
                      is_scratch_dead_after(stmts, i + 3, vx, dead_on_falloff, dead_on_break,
                                            dead_on_continue, 0);
                  if (a2.Op() == "=" && vx_dead_after_a2 && !expr_reads_reg(a2.Lhs(), vx) &&
                      a2.Lhs().ToString() != lval_str &&
                      CountIdentifierOccurrencesInExpression(a2.Rhs(), vx) == 1) {
                    auto post_inc =
                        CExpression::Unary(inc_op, a0.Rhs().Clone(), /*is_prefix=*/false);
                    auto new_rhs = SubstituteIdentifier(a2.Rhs(), vx, *post_inc);
                    stmts[i] = CStatement::Expression(
                        CExpression::Assignment("=", a2.Lhs().Clone(), std::move(new_rhs)));
                    stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 3);
                    changed = true;
                    break;
                  }
                }
              } else if (stmts[i + 2]->Kind() == CStatementKind::kIfStatement) {
                const auto& ifs = static_cast<const IfStatement&>(*stmts[i + 2]);
                if (CountIdentifierOccurrencesInExpression(ifs.Condition(), vx) == 1 &&
                    ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
                    (ifs.ElseBranch() == nullptr ||
                     ifs.ElseBranch()->Kind() == CStatementKind::kCompoundStatement)) {
                  LivenessFn after_if = [&](std::string_view r) {
                    return is_scratch_dead_after(stmts, i + 3, r, dead_on_falloff, dead_on_break,
                                                 dead_on_continue, 0);
                  };
                  const auto& tc = static_cast<const CompoundStatement&>(ifs.ThenBranch());
                  bool dead_in_if =
                      is_scratch_dead_after(tc.Statements(), 0, vx, after_if, dead_on_break,
                                            dead_on_continue, 0) &&
                      (ifs.ElseBranch() == nullptr
                           ? after_if(vx)
                           : is_scratch_dead_after(
                                 static_cast<const CompoundStatement&>(*ifs.ElseBranch())
                                     .Statements(),
                                 0, vx, after_if, dead_on_break, dead_on_continue, 0));
                  if (dead_in_if) {
                    auto post_inc =
                        CExpression::Unary(inc_op, a0.Rhs().Clone(), /*is_prefix=*/false);
                    auto new_cond = SubstituteIdentifier(ifs.Condition(), vx, *post_inc);
                    stmts[i] = CStatement::If(
                        std::move(new_cond), ifs.ThenBranch().Clone(),
                        ifs.ElseBranch() != nullptr ? ifs.ElseBranch()->Clone() : nullptr);
                    stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 3);
                    changed = true;
                    break;
                  }
                }
              }
            }
          }
          if (changed) continue;

          // 5b. Fold non-constant chained assignments (`vX = rhs; lhs1 = vX; lhs2 = vX;`).
          for (size_t i = 0; i + 2 < stmts.size(); ++i) {
            if (stmts[i] == nullptr || stmts[i + 1] == nullptr || stmts[i + 2] == nullptr ||
                stmts[i]->Kind() != CStatementKind::kExpressionStatement ||
                stmts[i + 1]->Kind() != CStatementKind::kExpressionStatement ||
                stmts[i + 2]->Kind() != CStatementKind::kExpressionStatement) {
              continue;
            }
            const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
            const auto& es1 = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
            const auto& es2 = static_cast<const ExpressionStatement&>(*stmts[i + 2]);
            if (es0.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
                es1.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
                es2.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
              continue;
            }
            const auto& a0 = static_cast<const AssignmentExpression&>(es0.Expression());
            const auto& a1 = static_cast<const AssignmentExpression&>(es1.Expression());
            const auto& a2 = static_cast<const AssignmentExpression&>(es2.Expression());
            if (a0.Op() == "=" && a1.Op() == "=" && a2.Op() == "=" &&
                a0.Lhs().Kind() == CExpressionKind::kIdentifier && IsPureExpression(a0.Rhs()) &&
                a1.Lhs().Kind() == CExpressionKind::kIdentifier &&
                a1.Rhs().Kind() == CExpressionKind::kIdentifier &&
                a2.Rhs().Kind() == CExpressionKind::kIdentifier) {
              std::string vx = static_cast<const IdentifierExpression&>(a0.Lhs()).Name();
              std::string lhs1 = static_cast<const IdentifierExpression&>(a1.Lhs()).Name();
              if (is_foldable_scratch(vx) && !is_foldable_scratch(lhs1) &&
                  static_cast<const IdentifierExpression&>(a1.Rhs()).Name() == vx &&
                  static_cast<const IdentifierExpression&>(a2.Rhs()).Name() == vx &&
                  !expr_reads_reg(a2.Lhs(), vx) &&
                  is_scratch_dead_after(stmts, i + 3, vx, dead_on_falloff, dead_on_break,
                                        dead_on_continue, 0)) {
                auto inner = CExpression::Assignment("=", a1.Lhs().Clone(), a0.Rhs().Clone());
                auto outer = CExpression::Assignment("=", a2.Lhs().Clone(), std::move(inner));
                stmts[i] = CStatement::Expression(std::move(outer));
                stmts.erase(stmts.begin() + i + 1, stmts.begin() + i + 3);
                changed = true;
                break;
              }
            }
          }
          if (changed) continue;

          // 5c. Fold single-use scratch expressions and erase dead pure scratch assignments.
          for (size_t i = 0; i < stmts.size(); ++i) {
            if (stmts[i] == nullptr || stmts[i]->Kind() != CStatementKind::kExpressionStatement) {
              continue;
            }
            const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
            if (es0.Expression().Kind() != CExpressionKind::kAssignmentExpression) continue;
            const auto& a0 = static_cast<const AssignmentExpression&>(es0.Expression());
            if (a0.Op() != "=" || a0.Lhs().Kind() != CExpressionKind::kIdentifier) continue;
            std::string vx = static_cast<const IdentifierExpression&>(a0.Lhs()).Name();
            if (!is_foldable_scratch(vx) || expr_reads_reg(a0.Rhs(), vx)) continue;
            const bool rhs0_pure = IsPureExpression(a0.Rhs());
            std::string rhs0_post_inc_var;
            if (!rhs0_pure && !ExpressionContainsCall(a0.Rhs()) &&
                a0.Rhs().Kind() == CExpressionKind::kUnaryExpression) {
              const auto& u0 = static_cast<const UnaryExpression&>(a0.Rhs());
              if (u0.Op() == "*") {
                if (u0.Operand().Kind() == CExpressionKind::kCastExpression) {
                  const auto& c0 = static_cast<const CastExpression&>(u0.Operand());
                  if (c0.Operand().Kind() == CExpressionKind::kUnaryExpression) {
                    const auto& u1 = static_cast<const UnaryExpression&>(c0.Operand());
                    if (!u1.IsPrefix() && (u1.Op() == "++" || u1.Op() == "--") &&
                        u1.Operand().Kind() == CExpressionKind::kIdentifier) {
                      rhs0_post_inc_var =
                          static_cast<const IdentifierExpression&>(u1.Operand()).Name();
                    }
                  }
                } else if (u0.Operand().Kind() == CExpressionKind::kUnaryExpression) {
                  const auto& u1 = static_cast<const UnaryExpression&>(u0.Operand());
                  if (!u1.IsPrefix() && (u1.Op() == "++" || u1.Op() == "--") &&
                      u1.Operand().Kind() == CExpressionKind::kCastExpression) {
                    const auto& c0 = static_cast<const CastExpression&>(u1.Operand());
                    if (c0.Operand().Kind() == CExpressionKind::kIdentifier) {
                      rhs0_post_inc_var =
                          static_cast<const IdentifierExpression&>(c0.Operand()).Name();
                    }
                  }
                }
              }
            }

            if (i + 1 < stmts.size() && stmts[i + 1] != nullptr) {
              if (stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
                const auto& es1 = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                if (es1.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& a1 = static_cast<const AssignmentExpression&>(es1.Expression());
                  if (a1.Op() == "=") {
                    bool killed_by_lhs1 =
                        a1.Lhs().Kind() == CExpressionKind::kIdentifier &&
                        aliases_reg(static_cast<const IdentifierExpression&>(a1.Lhs()).Name(), vx);
                    bool dead_after_a1 =
                        killed_by_lhs1 || is_scratch_dead_after(stmts, i + 2, vx, dead_on_falloff,
                                                                dead_on_break, dead_on_continue, 0);
                    bool safe_post_inc_sub = !rhs0_post_inc_var.empty() &&
                                             !ExpressionContainsCall(a1.Lhs()) &&
                                             !ExpressionContainsCall(a1.Rhs()) &&
                                             !ExpressionUsesVariable(a1.Lhs(), rhs0_post_inc_var) &&
                                             !ExpressionUsesVariable(a1.Rhs(), rhs0_post_inc_var);
                    if (dead_after_a1 && (killed_by_lhs1 || !expr_reads_reg(a1.Lhs(), vx)) &&
                        CountIdentifierOccurrencesInExpression(a1.Rhs(), vx) == 1 &&
                        (rhs0_pure || safe_post_inc_sub ||
                         (!ExpressionContainsCall(a1.Lhs()) && is_evaluated_first(a1.Rhs(), vx)))) {
                      auto new_rhs = SubstituteIdentifier(a1.Rhs(), vx, a0.Rhs());
                      stmts[i + 1] = CStatement::Expression(
                          CExpression::Assignment("=", a1.Lhs().Clone(), std::move(new_rhs)));
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                      break;
                    }
                    if (dead_after_a1 && rhs0_pure && IsPureExpression(a1.Rhs()) &&
                        !expr_reads_reg(a1.Rhs(), vx) &&
                        a1.Lhs().Kind() == CExpressionKind::kUnaryExpression &&
                        CountIdentifierOccurrencesInExpression(a1.Lhs(), vx) == 1) {
                      auto new_lhs = SubstituteIdentifier(a1.Lhs(), vx, a0.Rhs());
                      stmts[i + 1] = CStatement::Expression(
                          CExpression::Assignment("=", std::move(new_lhs), a1.Rhs().Clone()));
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                      break;
                    }
                  }
                } else if (es1.Expression().Kind() == CExpressionKind::kCallExpression) {
                  const auto& call = static_cast<const CallExpression&>(es1.Expression());
                  if (!expr_reads_reg(call.Callee(), vx) &&
                      CountIdentifierOccurrencesInExpression(call, vx) == 1 &&
                      (rhs0_pure || is_evaluated_first(call, vx)) &&
                      is_scratch_dead_after(stmts, i + 2, vx, dead_on_falloff, dead_on_break,
                                            dead_on_continue, 0)) {
                    auto new_call = SubstituteIdentifier(call, vx, a0.Rhs());
                    stmts[i + 1] = CStatement::Expression(std::move(new_call));
                    stmts.erase(stmts.begin() + i);
                    changed = true;
                    break;
                  }
                }
              } else if (stmts[i + 1]->Kind() == CStatementKind::kIfStatement) {
                const auto& ifs = static_cast<const IfStatement&>(*stmts[i + 1]);
                if (CountIdentifierOccurrencesInExpression(ifs.Condition(), vx) == 1 &&
                    (rhs0_pure || is_evaluated_first(ifs.Condition(), vx)) &&
                    ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
                    (ifs.ElseBranch() == nullptr ||
                     ifs.ElseBranch()->Kind() == CStatementKind::kCompoundStatement)) {
                  LivenessFn after_if = [&](std::string_view r) {
                    return is_scratch_dead_after(stmts, i + 2, r, dead_on_falloff, dead_on_break,
                                                 dead_on_continue, 0);
                  };
                  const auto& tc = static_cast<const CompoundStatement&>(ifs.ThenBranch());
                  bool dead_in_if =
                      is_scratch_dead_after(tc.Statements(), 0, vx, after_if, dead_on_break,
                                            dead_on_continue, 0) &&
                      (ifs.ElseBranch() == nullptr
                           ? after_if(vx)
                           : is_scratch_dead_after(
                                 static_cast<const CompoundStatement&>(*ifs.ElseBranch())
                                     .Statements(),
                                 0, vx, after_if, dead_on_break, dead_on_continue, 0));
                  if (dead_in_if) {
                    auto new_cond = SubstituteIdentifier(ifs.Condition(), vx, a0.Rhs());
                    stmts[i + 1] = CStatement::If(
                        std::move(new_cond), ifs.ThenBranch().Clone(),
                        ifs.ElseBranch() != nullptr ? ifs.ElseBranch()->Clone() : nullptr);
                    stmts.erase(stmts.begin() + i);
                    changed = true;
                    break;
                  }
                }
              } else if (stmts[i + 1]->Kind() == CStatementKind::kSwitchStatement) {
                const auto& ss = static_cast<const SwitchStatement&>(*stmts[i + 1]);
                if (CountIdentifierOccurrencesInExpression(ss.Condition(), vx) == 1 &&
                    (rhs0_pure || is_evaluated_first(ss.Condition(), vx))) {
                  LivenessFn after_sw = [&](std::string_view r) {
                    return is_scratch_dead_after(stmts, i + 2, r, dead_on_falloff, dead_on_break,
                                                 dead_on_continue, 0);
                  };
                  bool dead_in_cases = after_sw(vx);
                  for (size_t ci = 0; dead_in_cases && ci < ss.Cases().size(); ++ci) {
                    if (ss.Cases()[ci].body == nullptr) continue;
                    const bool is_last_case = (ci + 1 == ss.Cases().size());
                    if (!is_scratch_dead_after(ss.Cases()[ci].body->Statements(), 0, vx,
                                               is_last_case ? after_sw : never_dead, after_sw,
                                               dead_on_continue, 0)) {
                      dead_in_cases = false;
                    }
                  }
                  if (dead_in_cases) {
                    auto new_cond = SubstituteIdentifier(ss.Condition(), vx, a0.Rhs());
                    std::vector<SwitchCase> cloned_cases;
                    cloned_cases.reserve(ss.Cases().size());
                    for (const auto& c : ss.Cases()) {
                      SwitchCase sc;
                      sc.case_values = c.case_values;
                      sc.is_default = c.is_default;
                      if (c.body != nullptr) {
                        sc.body = std::unique_ptr<CompoundStatement>(
                            static_cast<CompoundStatement*>(c.body->Clone().release()));
                      }
                      cloned_cases.push_back(std::move(sc));
                    }
                    stmts[i + 1] = CStatement::Switch(std::move(new_cond), std::move(cloned_cases));
                    stmts.erase(stmts.begin() + i);
                    changed = true;
                    break;
                  }
                }
              } else if (stmts[i + 1]->Kind() == CStatementKind::kReturnStatement) {
                const auto& rs = static_cast<const ReturnStatement&>(*stmts[i + 1]);
                if (rs.ReturnValue() != nullptr &&
                    CountIdentifierOccurrencesInExpression(*rs.ReturnValue(), vx) == 1 &&
                    (rhs0_pure || is_evaluated_first(*rs.ReturnValue(), vx))) {
                  stmts[i + 1] =
                      CStatement::Return(SubstituteIdentifier(*rs.ReturnValue(), vx, a0.Rhs()));
                  stmts.erase(stmts.begin() + i);
                  changed = true;
                  break;
                }
              }
            }

            if (rhs0_pure && is_scratch_dead_after(stmts, i + 1, vx, dead_on_falloff, dead_on_break,
                                                   dead_on_continue, 0)) {
              stmts.erase(stmts.begin() + i);
              changed = true;
              break;
            }
          }
        }
      };

  auto run_fold_o0 = [&]() {
    LivenessFn top_falloff = [&](std::string_view) {
      return !StatementContainsGoto(*body) && !StatementContainsLabel(*body);
    };
    fold_o0_seq(body, top_falloff, never_dead, never_dead);
  };
  run_fold_o0();

  if (CollapseO0GotoLabelDiamonds(body, is_foldable_scratch, is_scratch_dead_after)) {
    run_fold_o0();
  }
  PushO0BranchReturnsAndStripDefaultBreak(body);
  RewritePositiveOffsetPointerDereferences(body);
}

bool UnalignedBlockCopyCollapserPass::Run(CompoundStatement* body, const AstPassContext& /*ctx*/) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  CollapseUnalignedBlockCopies(body);
  return body->ToString() != before;
}

bool FramePointerStructOffsetRewriterPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return false;
  std::string before = body->ToString();
  RewriteFramePointerStructOffsetAccesses(body, &ctx.parameters);
  return body->ToString() != before;
}

}  // namespace rom_nom_nom
