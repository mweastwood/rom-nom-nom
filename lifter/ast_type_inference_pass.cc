#include "lifter/ast_type_inference_pass.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "core/c_ast_visitor.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/ast_converter.h"
#include "lifter/ast_pass_pipeline.h"
#include "lifter/stack_frame_analyzer.h"
#include "lifter/variable_declaration_builder.h"

namespace rom_nom_nom {
namespace {

bool StatementHasCall(const CStatement& s) {
  return StatementContainsCall(s, /*ignore_math_intrinsics=*/true);
}

bool IsNarrowPointerStoreLhs(const CExpression& lhs, int64_t* out_mask) {
  if (lhs.Kind() != CExpressionKind::kUnaryExpression) return false;
  const auto& un = static_cast<const UnaryExpression&>(lhs);
  if (!un.IsPrefix() || un.Op() != "*") return false;
  if (un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
  const auto& cast = static_cast<const CastExpression&>(un.Operand());
  if (cast.TargetType().pointer_depth != 1) return false;
  if (cast.TargetType().name == "u8" || cast.TargetType().name == "s8") {
    *out_mask = 255;
    return true;
  }
  if (cast.TargetType().name == "u16" || cast.TargetType().name == "s16") {
    *out_mask = 65535;
    return true;
  }
  return false;
}

bool IsIdentifierStepByConst(const CExpression& rhs, std::string_view expected_var,
                             bool require_unit_step) {
  if (rhs.Kind() != CExpressionKind::kBinaryExpression) return false;
  const auto& bin = static_cast<const BinaryExpression&>(rhs);
  if (bin.Op() != "+" && bin.Op() != "-") return false;
  if (bin.Lhs().Kind() != CExpressionKind::kIdentifier ||
      bin.Rhs().Kind() != CExpressionKind::kIntegerLiteral) {
    return false;
  }
  const auto& id = static_cast<const IdentifierExpression&>(bin.Lhs());
  if (!expected_var.empty() && id.Name() != expected_var) {
    return false;
  }
  const int64_t step = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
  return require_unit_step ? (step == 1 || step == -1) : true;
}

class ConditionallySteppedVarsVisitor : public AstVisitor {
 public:
  ConditionallySteppedVarsVisitor(absl::flat_hash_set<std::string>* out_vars,
                                  absl::flat_hash_set<std::string>* out_do_while_post_stepped)
      : out_vars_(out_vars), out_do_while_post_stepped_(out_do_while_post_stepped) {}

  void VisitIfStatement(const IfStatement& ifs) override {
    auto try_extract_cond_step = [&](const CStatement* branch, const CStatement* other_branch) {
      const CStatement* single = branch;
      if (single != nullptr && single->Kind() == CStatementKind::kCompoundStatement) {
        const auto& comp = static_cast<const CompoundStatement&>(*single);
        single = (comp.Statements().size() == 1) ? comp.Statements()[0].get() : nullptr;
      }
      if (single != nullptr && single->Kind() == CStatementKind::kExpressionStatement) {
        const auto& expr = static_cast<const ExpressionStatement&>(*single).Expression();
        if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& assign = static_cast<const AssignmentExpression&>(expr);
          if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
              IsIdentifierStepByConst(assign.Rhs(), /*expected_var=*/"",
                                      /*require_unit_step=*/false)) {
            const std::string& lhs_name =
                static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
            if (other_branch == nullptr || !StatementModifiesVariable(*other_branch, lhs_name)) {
              out_vars_->insert(lhs_name);
            }
          }
        }
      }
    };
    try_extract_cond_step(&ifs.ThenBranch(), ifs.ElseBranch());
    if (ifs.ElseBranch() != nullptr) {
      try_extract_cond_step(ifs.ElseBranch(), &ifs.ThenBranch());
    }
    AstVisitor::VisitIfStatement(ifs);
  }

  void VisitDoWhileStatement(const DoWhileStatement& dws) override {
    if (out_do_while_post_stepped_ != nullptr &&
        dws.Body().Kind() == CStatementKind::kCompoundStatement && StatementHasCall(dws.Body())) {
      const auto& comp = static_cast<const CompoundStatement&>(dws.Body());
      for (size_t idx = 1; idx < comp.Statements().size(); ++idx) {
        const auto* prev_s = comp.Statements()[idx - 1].get();
        const auto* last_s = comp.Statements()[idx].get();
        if (prev_s != nullptr && last_s != nullptr &&
            prev_s->Kind() == CStatementKind::kExpressionStatement &&
            last_s->Kind() == CStatementKind::kExpressionStatement) {
          const auto& prev_e = static_cast<const ExpressionStatement&>(*prev_s).Expression();
          const auto& last_e = static_cast<const ExpressionStatement&>(*last_s).Expression();
          if (prev_e.Kind() == CExpressionKind::kAssignmentExpression &&
              last_e.Kind() == CExpressionKind::kAssignmentExpression) {
            const auto& prev_a = static_cast<const AssignmentExpression&>(prev_e);
            const auto& last_a = static_cast<const AssignmentExpression&>(last_e);
            if (prev_a.Op() == "=" && last_a.Op() == "=" &&
                prev_a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                last_a.Lhs().Kind() == CExpressionKind::kIdentifier) {
              const std::string& step_var =
                  static_cast<const IdentifierExpression&>(last_a.Lhs()).Name();
              if (IsIdentifierStepByConst(last_a.Rhs(), step_var, /*require_unit_step=*/false) &&
                  prev_a.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                const auto& mask_bin = static_cast<const BinaryExpression&>(prev_a.Rhs());
                if (mask_bin.Op() == "&" && mask_bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
                    static_cast<const IdentifierExpression&>(mask_bin.Lhs()).Name() == step_var &&
                    mask_bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
                  int64_t m = static_cast<const IntegerLiteralExpression&>(mask_bin.Rhs()).Value();
                  if (m == 255 || m == 65535) {
                    out_vars_->insert(step_var);
                    out_do_while_post_stepped_->insert(step_var);
                  }
                }
              }
            }
          }
        }
      }
    }
    AstVisitor::VisitDoWhileStatement(dws);
  }

 private:
  absl::flat_hash_set<std::string>* out_vars_;
  absl::flat_hash_set<std::string>* out_do_while_post_stepped_;
};

void CollectConditionallySteppedVars(
    const CStatement& stmt, absl::flat_hash_set<std::string>* out_vars,
    absl::flat_hash_set<std::string>* out_do_while_post_stepped = nullptr) {
  ConditionallySteppedVarsVisitor visitor(out_vars, out_do_while_post_stepped);
  visitor.VisitStatement(stmt);
}

void CheckNarrowMaskedReadsInExpr(const CExpression& expr, std::string_view var_name,
                                  int* mask_255_count, int* mask_65535_count,
                                  bool* has_unmasked_read) {
  if (*has_unmasked_read) return;
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier:
      if (static_cast<const IdentifierExpression&>(expr).Name() == var_name) {
        *has_unmasked_read = true;
      }
      return;
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      if (bin.Op() == "&" && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        int64_t mask = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        if ((mask == 255 || mask == 65535) && bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(bin.Lhs()).Name() == var_name) {
          if (mask == 255) {
            ++(*mask_255_count);
          } else {
            ++(*mask_65535_count);
          }
          return;
        }
        if ((mask == 255 || mask == 65535) &&
            bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& inner = static_cast<const BinaryExpression&>(bin.Lhs());
          if (inner.Op() == "+" || inner.Op() == "-") {
            bool lhs_is_var =
                inner.Lhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(inner.Lhs()).Name() == var_name;
            bool rhs_is_var =
                inner.Rhs().Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(inner.Rhs()).Name() == var_name;
            if (lhs_is_var || rhs_is_var) {
              if (mask == 255) {
                ++(*mask_255_count);
              } else {
                ++(*mask_65535_count);
              }
              if (!lhs_is_var) {
                CheckNarrowMaskedReadsInExpr(inner.Lhs(), var_name, mask_255_count,
                                             mask_65535_count, has_unmasked_read);
              }
              if (!rhs_is_var) {
                CheckNarrowMaskedReadsInExpr(inner.Rhs(), var_name, mask_255_count,
                                             mask_65535_count, has_unmasked_read);
              }
              return;
            }
          }
        }
      }
      CheckNarrowMaskedReadsInExpr(bin.Lhs(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(bin.Rhs(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == var_name) {
        if (IsIdentifierStepByConst(assign.Rhs(), var_name, /*require_unit_step=*/false)) {
          return;
        }
        CheckNarrowMaskedReadsInExpr(assign.Rhs(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
        return;
      }
      int64_t store_mask = 0;
      if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(assign.Rhs()).Name() == var_name &&
          IsNarrowPointerStoreLhs(assign.Lhs(), &store_mask)) {
        CheckNarrowMaskedReadsInExpr(assign.Lhs(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
        return;
      }
      CheckNarrowMaskedReadsInExpr(assign.Lhs(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(assign.Rhs(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CExpressionKind::kUnaryExpression:
      CheckNarrowMaskedReadsInExpr(static_cast<const UnaryExpression&>(expr).Operand(), var_name,
                                   mask_255_count, mask_65535_count, has_unmasked_read);
      return;
    case CExpressionKind::kCastExpression:
      CheckNarrowMaskedReadsInExpr(static_cast<const CastExpression&>(expr).Operand(), var_name,
                                   mask_255_count, mask_65535_count, has_unmasked_read);
      return;
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      CheckNarrowMaskedReadsInExpr(call.Callee(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr) {
          CheckNarrowMaskedReadsInExpr(*arg, var_name, mask_255_count, mask_65535_count,
                                       has_unmasked_read);
        }
      }
      return;
    }
    case CExpressionKind::kMemberAccessExpression:
      CheckNarrowMaskedReadsInExpr(static_cast<const MemberAccessExpression&>(expr).Object(),
                                   var_name, mask_255_count, mask_65535_count, has_unmasked_read);
      return;
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      CheckNarrowMaskedReadsInExpr(arr.Array(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(arr.Index(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      CheckNarrowMaskedReadsInExpr(tern.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(tern.TrueExpression(), var_name, mask_255_count,
                                   mask_65535_count, has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(tern.FalseExpression(), var_name, mask_255_count,
                                   mask_65535_count, has_unmasked_read);
      return;
    }
    default:
      return;
  }
}

void CheckNarrowMaskedReadsInStmt(const CStatement& stmt, std::string_view var_name,
                                  int* mask_255_count, int* mask_65535_count,
                                  bool* has_unmasked_read) {
  if (*has_unmasked_read) return;
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement:
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          CheckNarrowMaskedReadsInStmt(*child, var_name, mask_255_count, mask_65535_count,
                                       has_unmasked_read);
        }
      }
      return;
    case CStatementKind::kExpressionStatement:
      CheckNarrowMaskedReadsInExpr(static_cast<const ExpressionStatement&>(stmt).Expression(),
                                   var_name, mask_255_count, mask_65535_count, has_unmasked_read);
      return;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      if (vd.Initializer() != nullptr) {
        CheckNarrowMaskedReadsInExpr(*vd.Initializer(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      return;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      CheckNarrowMaskedReadsInExpr(ifs.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInStmt(ifs.ThenBranch(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      if (ifs.ElseBranch() != nullptr) {
        CheckNarrowMaskedReadsInStmt(*ifs.ElseBranch(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      return;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      CheckNarrowMaskedReadsInExpr(ws.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInStmt(ws.Body(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      CheckNarrowMaskedReadsInStmt(dws.Body(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      CheckNarrowMaskedReadsInExpr(dws.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr) {
        CheckNarrowMaskedReadsInStmt(*fs.Init(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      if (fs.Condition() != nullptr) {
        CheckNarrowMaskedReadsInExpr(*fs.Condition(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      if (fs.Step() != nullptr) {
        CheckNarrowMaskedReadsInExpr(*fs.Step(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      CheckNarrowMaskedReadsInStmt(fs.Body(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      return;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() != nullptr) {
        CheckNarrowMaskedReadsInExpr(*rs.ReturnValue(), var_name, mask_255_count, mask_65535_count,
                                     has_unmasked_read);
      }
      return;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      CheckNarrowMaskedReadsInExpr(ss.Condition(), var_name, mask_255_count, mask_65535_count,
                                   has_unmasked_read);
      for (const auto& c : ss.Cases()) {
        if (c.body != nullptr) {
          CheckNarrowMaskedReadsInStmt(*c.body, var_name, mask_255_count, mask_65535_count,
                                       has_unmasked_read);
        }
      }
      return;
    }
    default:
      return;
  }
}

std::unique_ptr<CExpression> RewriteNarrowCondIncExpr(
    const CExpression& expr, const absl::flat_hash_map<std::string, CType>& narrow_types,
    const absl::flat_hash_set<std::string>& do_while_post_stepped_vars, bool is_return_root) {
  if (is_return_root && expr.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& lt = static_cast<const BinaryExpression&>(expr);
    if (lt.Op() == "<" && lt.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
        static_cast<const IntegerLiteralExpression&>(lt.Rhs()).Value() == 1 &&
        lt.Lhs().Kind() == CExpressionKind::kCastExpression) {
      const auto& cast = static_cast<const CastExpression&>(lt.Lhs());
      if (cast.TargetType().name == "u32" && cast.TargetType().pointer_depth == 0 &&
          cast.Operand().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& xor_bin = static_cast<const BinaryExpression&>(cast.Operand());
        if (xor_bin.Op() == "^" && xor_bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
            xor_bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
          const auto& mask_bin = static_cast<const BinaryExpression&>(xor_bin.Lhs());
          if (mask_bin.Op() == "&" && mask_bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
              narrow_types.contains(
                  static_cast<const IdentifierExpression&>(mask_bin.Lhs()).Name())) {
            return CExpression::Binary("==", xor_bin.Lhs().Clone(), xor_bin.Rhs().Clone());
          }
        }
      }
    }
  }
  switch (expr.Kind()) {
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      if (bin.Op() == "&" && bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
          bin.Lhs().Kind() == CExpressionKind::kIdentifier) {
        const std::string& var_name = static_cast<const IdentifierExpression&>(bin.Lhs()).Name();
        int64_t mask = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        auto it = narrow_types.find(var_name);
        if (it != narrow_types.end() && do_while_post_stepped_vars.contains(var_name) &&
            ((mask == 255 && it->second.name == "u8") ||
             (mask == 65535 && it->second.name == "u16"))) {
          return bin.Lhs().Clone();
        }
      }
      auto rewritten_lhs = RewriteNarrowCondIncExpr(
          bin.Lhs(), narrow_types, do_while_post_stepped_vars, /*is_return_root=*/false);
      auto rewritten_rhs = RewriteNarrowCondIncExpr(
          bin.Rhs(), narrow_types, do_while_post_stepped_vars, /*is_return_root=*/false);
      if (bin.Op() == "&" && rewritten_rhs->Kind() == CExpressionKind::kIntegerLiteral &&
          (static_cast<const IntegerLiteralExpression&>(*rewritten_rhs).Value() == 65535 ||
           static_cast<const IntegerLiteralExpression&>(*rewritten_rhs).Value() == 255) &&
          rewritten_lhs->Kind() == CExpressionKind::kBinaryExpression) {
        const int64_t mask_val =
            static_cast<const IntegerLiteralExpression&>(*rewritten_rhs).Value();
        const std::string_view expected_type_name = (mask_val == 255) ? "u8" : "u16";
        const CType target_narrow_type = (mask_val == 255) ? CType::U8() : CType::U16();
        const auto& inner = static_cast<const BinaryExpression&>(*rewritten_lhs);
        if (inner.Op() == "+" || inner.Op() == "-") {
          bool lhs_narrow = false;
          bool rhs_narrow = false;
          bool lhs_narrow_post_step = false;
          bool rhs_narrow_post_step = false;
          if (inner.Lhs().Kind() == CExpressionKind::kIdentifier) {
            const std::string& lhs_id =
                static_cast<const IdentifierExpression&>(inner.Lhs()).Name();
            auto it = narrow_types.find(lhs_id);
            lhs_narrow = (it != narrow_types.end() && it->second.name == expected_type_name);
            lhs_narrow_post_step =
                (it != narrow_types.end() && do_while_post_stepped_vars.contains(lhs_id));
          }
          if (inner.Rhs().Kind() == CExpressionKind::kIdentifier) {
            const std::string& rhs_id =
                static_cast<const IdentifierExpression&>(inner.Rhs()).Name();
            auto it = narrow_types.find(rhs_id);
            rhs_narrow = (it != narrow_types.end() && it->second.name == expected_type_name);
            rhs_narrow_post_step =
                (it != narrow_types.end() && do_while_post_stepped_vars.contains(rhs_id));
          }
          if (lhs_narrow || rhs_narrow || lhs_narrow_post_step || rhs_narrow_post_step) {
            auto new_lhs = lhs_narrow ? inner.Lhs().Clone()
                                      : CExpression::Cast(target_narrow_type, inner.Lhs().Clone());
            auto new_rhs = rhs_narrow ? inner.Rhs().Clone()
                                      : CExpression::Cast(target_narrow_type, inner.Rhs().Clone());
            return CExpression::Binary(
                "&",
                CExpression::Cast(
                    target_narrow_type,
                    CExpression::Binary(inner.Op(), std::move(new_lhs), std::move(new_rhs))),
                std::move(rewritten_rhs));
          }
        }
      }
      return CExpression::Binary(bin.Op(), std::move(rewritten_lhs), std::move(rewritten_rhs));
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      return CExpression::Assignment(
          assign.Op(),
          RewriteNarrowCondIncExpr(assign.Lhs(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncExpr(assign.Rhs(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false));
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(
          un.Op(),
          RewriteNarrowCondIncExpr(un.Operand(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          un.IsPrefix());
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(
          cast.TargetType(),
          RewriteNarrowCondIncExpr(cast.Operand(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false));
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      std::vector<std::unique_ptr<CExpression>> args;
      args.reserve(call.Arguments().size());
      for (const auto& arg : call.Arguments()) {
        args.push_back(arg != nullptr ? RewriteNarrowCondIncExpr(*arg, narrow_types,
                                                                 do_while_post_stepped_vars,
                                                                 /*is_return_root=*/false)
                                      : nullptr);
      }
      return CExpression::Call(
          RewriteNarrowCondIncExpr(call.Callee(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          std::move(args));
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return CExpression::Ternary(
          RewriteNarrowCondIncExpr(tern.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncExpr(tern.TrueExpression(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncExpr(tern.FalseExpression(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false));
    }
    default:
      return expr.Clone();
  }
}

std::unique_ptr<CStatement> RewriteNarrowCondIncStmt(
    const CStatement& stmt, const absl::flat_hash_map<std::string, CType>& narrow_types,
    const absl::flat_hash_set<std::string>& do_while_post_stepped_vars) {
  switch (stmt.Kind()) {
    case CStatementKind::kCompoundStatement: {
      auto out = std::make_unique<CompoundStatement>();
      for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
        if (child != nullptr) {
          out->AddStatement(
              RewriteNarrowCondIncStmt(*child, narrow_types, do_while_post_stepped_vars));
        }
      }
      return out;
    }
    case CStatementKind::kExpressionStatement: {
      const auto& es = static_cast<const ExpressionStatement&>(stmt);
      return CStatement::Expression(RewriteNarrowCondIncExpr(
          es.Expression(), narrow_types, do_while_post_stepped_vars, /*is_return_root=*/false));
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      return CStatement::If(
          RewriteNarrowCondIncExpr(ifs.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncStmt(ifs.ThenBranch(), narrow_types, do_while_post_stepped_vars),
          ifs.ElseBranch() != nullptr ? RewriteNarrowCondIncStmt(*ifs.ElseBranch(), narrow_types,
                                                                 do_while_post_stepped_vars)
                                      : nullptr);
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      return CStatement::While(
          RewriteNarrowCondIncExpr(ws.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          RewriteNarrowCondIncStmt(ws.Body(), narrow_types, do_while_post_stepped_vars));
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      return CStatement::DoWhile(
          RewriteNarrowCondIncStmt(dws.Body(), narrow_types, do_while_post_stepped_vars),
          RewriteNarrowCondIncExpr(dws.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false));
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      return CStatement::For(
          fs.Init() != nullptr
              ? RewriteNarrowCondIncStmt(*fs.Init(), narrow_types, do_while_post_stepped_vars)
              : nullptr,
          fs.Condition() != nullptr
              ? RewriteNarrowCondIncExpr(*fs.Condition(), narrow_types, do_while_post_stepped_vars,
                                         /*is_return_root=*/false)
              : nullptr,
          fs.Step() != nullptr
              ? RewriteNarrowCondIncExpr(*fs.Step(), narrow_types, do_while_post_stepped_vars,
                                         /*is_return_root=*/false)
              : nullptr,
          RewriteNarrowCondIncStmt(fs.Body(), narrow_types, do_while_post_stepped_vars));
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      return CStatement::Return(rs.ReturnValue() != nullptr
                                    ? RewriteNarrowCondIncExpr(*rs.ReturnValue(), narrow_types,
                                                               do_while_post_stepped_vars,
                                                               /*is_return_root=*/true)
                                    : nullptr);
    }
    case CStatementKind::kSwitchStatement: {
      const auto& ss = static_cast<const SwitchStatement&>(stmt);
      std::vector<SwitchCase> cases;
      cases.reserve(ss.Cases().size());
      for (const auto& c : ss.Cases()) {
        SwitchCase sc = c.Clone();
        if (c.body != nullptr) {
          auto rewritten =
              RewriteNarrowCondIncStmt(*c.body, narrow_types, do_while_post_stepped_vars);
          sc.body.reset(static_cast<CompoundStatement*>(rewritten.release()));
        }
        cases.push_back(std::move(sc));
      }
      return CStatement::Switch(
          RewriteNarrowCondIncExpr(ss.Condition(), narrow_types, do_while_post_stepped_vars,
                                   /*is_return_root=*/false),
          std::move(cases));
    }
    default:
      return stmt.Clone();
  }
}

}  // namespace

void FunctionSignatureInference::DetermineReturnFlags(const ControlFlowGraph& cfg,
                                                      const SymbolIndex* symbol_index,
                                                      const AstConverterOptions& options,
                                                      bool* returns_v0, bool* returns_f0) {
  *returns_v0 = false;
  *returns_f0 = false;
  if (options.return_type.has_value()) {
    std::string rt = options.return_type->ToString();
    *returns_v0 = (rt != "void" && rt != "f32" && rt != "f64");
    *returns_f0 = (rt == "f32" || rt == "f64");
    return;
  }
  *returns_f0 = AbiAnalyzer::DetermineReturnsF0(cfg);
  if (!*returns_f0 && options.functions_returning_f0 != nullptr &&
      options.functions_returning_f0->contains(options.function_name)) {
    for (const auto& block : cfg.Blocks()) {
      for (const auto& inst : block.instructions) {
        if (inst.IsCall()) {
          *returns_f0 = true;
          break;
        }
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (FpRegister d : ud.fpr_defs) {
          if (d == FpRegister::kF0) {
            *returns_f0 = true;
            break;
          }
        }
        if (*returns_f0) break;
      }
      if (*returns_f0) break;
    }
  }
  if (!*returns_f0) {
    *returns_v0 =
        AbiAnalyzer::DetermineReturnsV0(cfg, symbol_index, options.functions_returning_v0);
    if (!*returns_v0 && options.functions_returning_v0 != nullptr &&
        options.functions_returning_v0->contains(options.function_name)) {
      for (const auto& block : cfg.Blocks()) {
        for (const auto& inst : block.instructions) {
          if (inst.IsCall()) {
            *returns_v0 = true;
            break;
          }
          RegisterUseDef ud = GetInstructionUseDef(inst);
          for (Register d : ud.gpr_defs) {
            if (d == Register::kV0) {
              *returns_v0 = true;
              break;
            }
          }
          if (*returns_v0) break;
        }
        if (*returns_v0) break;
      }
    }
  }
}

std::vector<CParameter> FunctionSignatureInference::BuildFunctionParameters(
    const ControlFlowGraph& cfg, const AstConverterOptions& options) {
  std::vector<CParameter> parameters = options.parameters;
  if (!parameters.empty()) {
    return parameters;
  }
  int parameter_count = -1;
  if (options.function_parameter_counts != nullptr) {
    auto it = options.function_parameter_counts->find(options.function_name);
    if (it != options.function_parameter_counts->end()) {
      parameter_count = it->second;
    }
  }
  std::vector<Instruction> all_instructions;
  for (const auto& block : cfg.Blocks()) {
    all_instructions.insert(all_instructions.end(), block.instructions.begin(),
                            block.instructions.end());
  }
  if (parameter_count < 0 && !all_instructions.empty()) {
    parameter_count = AbiAnalyzer::DetermineParameterCount(all_instructions);
  }

  int fp_param_count = 0;
  if (options.function_fp_parameter_counts != nullptr) {
    auto it = options.function_fp_parameter_counts->find(options.function_name);
    if (it != options.function_fp_parameter_counts->end()) {
      fp_param_count = it->second;
    }
  }
  if (fp_param_count == 0 && !all_instructions.empty()) {
    fp_param_count = AbiAnalyzer::DetermineFpParameterCount(all_instructions);
  }

  int total_param_count = std::max(parameter_count, fp_param_count);
  auto float_parameters_in_general_registers =
      AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  bool double_parameter_in_a2_a3 = AbiAnalyzer::DetectDoubleParameterInGeneralRegisters(cfg);
  auto float_stack_param_slots = AbiAnalyzer::DetectFloatStackParameterSlots(cfg);
  auto double_stack_param_slots = AbiAnalyzer::DetectDoubleStackParameterSlots(cfg);
  uint32_t frame_size = StackFrameAnalyzer::ComputeFrameSize(cfg);
  auto is_stack_slot_accessed = [&](int s) -> bool {
    int32_t target_off = static_cast<int32_t>(frame_size + 16) + (s - 4) * 4;
    for (const auto& inst : all_instructions) {
      if ((inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.immediate == target_off &&
          (inst.IsLoad() || inst.IsStore())) {
        return true;
      }
    }
    return false;
  };
  int slot = 0;
  int fp_idx = 0;
  while (slot < total_param_count || fp_idx < fp_param_count) {
    if (fp_idx < fp_param_count) {
      std::string fp_name = (fp_idx == 0) ? "f12" : "f14";
      FpRegister fpr = (fp_idx == 0) ? FpRegister::kF12 : FpRegister::kF14;
      bool is_double = AbiAnalyzer::IsFpParameterDouble(cfg, fpr);
      CType t = is_double ? CType::F64() : CType::F32();
      if (is_double && (slot % 2 != 0)) {
        slot++;
      }
      parameters.push_back(CParameter{.type = t, .name = is_double ? (fp_name + "_d") : fp_name});
      slot += (is_double ? 2 : 1);
      fp_idx++;
    } else if (slot == 1 && double_parameter_in_a2_a3 &&
               !AbiAnalyzer::IsRegisterLiveIn(cfg, Register::kA1)) {
      slot++;
    } else if (slot == 2 && double_parameter_in_a2_a3) {
      parameters.push_back(CParameter{.type = CType::F64(), .name = "arg2"});
      slot += 2;
    } else if (slot == 3 && double_stack_param_slots.contains(4) &&
               !AbiAnalyzer::IsRegisterLiveIn(cfg, Register::kA3)) {
      slot++;
    } else if (slot >= 5 && (slot % 2 == 1) && double_stack_param_slots.contains(slot + 1) &&
               !is_stack_slot_accessed(slot)) {
      slot++;
    } else if (slot >= 4 && double_stack_param_slots.contains(slot)) {
      parameters.push_back(
          CParameter{.type = CType::F64(), .name = absl::StrFormat("arg%d", slot)});
      slot += 2;
    } else {
      Register reg = static_cast<Register>(static_cast<int>(Register::kA0) + slot);
      bool is_float_param = (slot < 4 && float_parameters_in_general_registers.contains(reg)) ||
                            (slot >= 4 && float_stack_param_slots.contains(slot));
      CType param_type = is_float_param ? CType::F32() : CType::S32();
      parameters.push_back(CParameter{.type = param_type, .name = absl::StrFormat("arg%d", slot)});
      slot++;
    }
  }
  return parameters;
}

CType FunctionSignatureInference::InferReturnType(const ControlFlowGraph& cfg,
                                                  const AstConverterOptions& options,
                                                  bool returns_v0, bool returns_f0,
                                                  const CompoundStatement& body) {
  if (options.return_type.has_value()) {
    return *options.return_type;
  }
  if (returns_f0) {
    bool is_double = AbiAnalyzer::DetermineReturnsF0Double(cfg);
    return is_double ? CType::F64() : CType::F32();
  }
  if (returns_v0) {
    return CType::S32();
  }
  if (HasReturnValue(body)) {
    bool body_returns_f0 = false;
    for (const auto& stmt : body.Statements()) {
      if (stmt->Kind() == CStatementKind::kReturnStatement) {
        const auto& r = static_cast<const ReturnStatement&>(*stmt);
        if (r.ReturnValue() != nullptr &&
            (r.ReturnValue()->ToString() == "f0" || r.ReturnValue()->ToString() == "f0_d")) {
          body_returns_f0 = true;
          break;
        }
      }
    }
    if (body_returns_f0) {
      bool is_double = AbiAnalyzer::DetermineReturnsF0Double(cfg);
      return is_double ? CType::F64() : CType::F32();
    }
    return CType::S32();
  }
  return CType::Void();
}

absl::flat_hash_map<std::string, CType> InferAndPromoteNarrowCondSteppedVars(
    absl::Span<const std::string> candidate_vars, std::unique_ptr<CompoundStatement>* body) {
  absl::flat_hash_map<std::string, CType> narrow_cond_inc_var_types;
  if (body == nullptr || *body == nullptr || candidate_vars.empty()) {
    return narrow_cond_inc_var_types;
  }
  absl::flat_hash_set<std::string> cond_stepped_vars;
  absl::flat_hash_set<std::string> do_while_post_stepped_vars;
  CollectConditionallySteppedVars(**body, &cond_stepped_vars, &do_while_post_stepped_vars);
  if (cond_stepped_vars.empty()) {
    return narrow_cond_inc_var_types;
  }
  for (const auto& var_name : candidate_vars) {
    if (!cond_stepped_vars.contains(var_name) || StatementTakesAddressOf(**body, var_name)) {
      continue;
    }
    int mask_255_count = 0;
    int mask_65535_count = 0;
    bool has_unmasked_read = false;
    CheckNarrowMaskedReadsInStmt(**body, var_name, &mask_255_count, &mask_65535_count,
                                 &has_unmasked_read);
    if (!has_unmasked_read) {
      if (mask_255_count > 0 && mask_65535_count == 0) {
        narrow_cond_inc_var_types.emplace(var_name, CType::U8());
      } else if (mask_65535_count > 0 && mask_255_count == 0) {
        narrow_cond_inc_var_types.emplace(var_name, CType::U16());
      }
    }
  }
  if (!narrow_cond_inc_var_types.empty()) {
    auto rewritten_body =
        RewriteNarrowCondIncStmt(**body, narrow_cond_inc_var_types, do_while_post_stepped_vars);
    body->reset(static_cast<CompoundStatement*>(rewritten_body.release()));
  }
  return narrow_cond_inc_var_types;
}

bool ReturnAndGlobalTypePromotionPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return false;
  std::string before = body->ToString();

  std::vector<std::string> candidates = candidate_vars_;
  if (candidates.empty()) {
    absl::flat_hash_set<std::string> seen;
    for (const auto& param : ctx.parameters) {
      seen.insert(param.name);
    }
    CollectUsedVariables(*body, &candidates, &seen);
  }

  auto temp_body = std::make_unique<CompoundStatement>();
  for (auto& s : body->MutableStatements()) {
    temp_body->AddStatement(std::move(s));
  }
  auto promoted = InferAndPromoteNarrowCondSteppedVars(candidates, &temp_body);
  if (out_promoted_types_ != nullptr) {
    *out_promoted_types_ = std::move(promoted);
  }
  *body = std::move(*temp_body);
  return body->ToString() != before;
}

namespace {

bool IsCallerSavedScratchVarName(std::string_view name) {
  if (name == "v0" || name == "v1" || name == "temp_at") {
    return true;
  }
  if (name.size() == 7 && absl::StartsWith(name, "temp_t") && name[6] >= '0' && name[6] <= '9') {
    return true;
  }
  if (name.size() == 7 && absl::StartsWith(name, "temp_a") && name[6] >= '0' && name[6] <= '3') {
    return true;
  }
  return false;
}

class ShortCircuitOrTernaryVisitor : public AstVisitor {
 public:
  void VisitBinaryExpression(const BinaryExpression& expr) override {
    if (expr.Op() == "&&" || expr.Op() == "||") {
      found_ = true;
      Stop();
      return;
    }
    AstVisitor::VisitBinaryExpression(expr);
  }

  void VisitTernaryExpression(const TernaryExpression& expr) override {
    (void)expr;
    found_ = true;
    Stop();
  }

  void VisitCallExpression(const CallExpression& expr) override {
    if (!IsInlineAsmIntrinsicCall(expr)) {
      ++call_count_;
    }
    AstVisitor::VisitCallExpression(expr);
  }

  bool FoundShortCircuitOrTernary() const { return found_; }
  int CallCount() const { return call_count_; }

 private:
  bool found_ = false;
  int call_count_ = 0;
};

void InspectExpressionControlAndCalls(const CExpression& expr, bool* has_short_circuit_or_ternary,
                                      int* call_count) {
  ShortCircuitOrTernaryVisitor visitor;
  visitor.VisitExpression(expr);
  *has_short_circuit_or_ternary = visitor.FoundShortCircuitOrTernary();
  *call_count = visitor.CallCount();
}

bool IsStandaloneCallUsingVarOnlyInArgs(const CExpression& expr, std::string_view var) {
  const CExpression* cur = &expr;
  if (cur->Kind() == CExpressionKind::kAssignmentExpression) {
    const auto& assign = static_cast<const AssignmentExpression&>(*cur);
    if (assign.Op() != "=" || ExpressionUsesVariable(assign.Lhs(), var) ||
        ExpressionModifiesVariable(assign.Lhs(), var) ||
        StatementHasCall(ExpressionStatement(assign.Lhs().Clone()))) {
      return false;
    }
    cur = &assign.Rhs();
  }
  while (cur->Kind() == CExpressionKind::kCastExpression) {
    cur = &static_cast<const CastExpression&>(*cur).Operand();
  }
  if (cur->Kind() != CExpressionKind::kCallExpression) {
    return false;
  }
  const auto& call = static_cast<const CallExpression&>(*cur);
  for (const auto& arg : call.Arguments()) {
    if (arg != nullptr && ExpressionContainsCall(*arg, /*ignore_math_intrinsics=*/true)) {
      return false;
    }
  }
  return true;
}

bool ExprMentionsVar(const CExpression& expr, std::string_view var) {
  return CountIdentifierOccurrencesInExpression(expr, var) > 0;
}

enum class VarPathOutcome {
  kKilled,
  kFallthrough,
  kLiveRead,
};

VarPathOutcome CheckStmtVarOutcome(const CStatement& stmt, std::string_view var) {
  switch (stmt.Kind()) {
    case CStatementKind::kCommentStatement:
      return VarPathOutcome::kFallthrough;
    case CStatementKind::kVariableDeclarationStatement: {
      const auto& vd = static_cast<const VariableDeclarationStatement&>(stmt);
      if (vd.Initializer() != nullptr && (ExprMentionsVar(*vd.Initializer(), var) ||
                                          ExpressionModifiesVariable(*vd.Initializer(), var))) {
        return VarPathOutcome::kLiveRead;
      }
      return (vd.Name() == var) ? VarPathOutcome::kKilled : VarPathOutcome::kFallthrough;
    }
    case CStatementKind::kExpressionStatement: {
      const auto& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
      if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(expr);
        if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == var) {
          if (ExprMentionsVar(assign.Rhs(), var) || ExpressionModifiesVariable(assign.Rhs(), var)) {
            return VarPathOutcome::kLiveRead;
          }
          return VarPathOutcome::kKilled;
        }
      }
      if (ExprMentionsVar(expr, var) || ExpressionModifiesVariable(expr, var)) {
        return VarPathOutcome::kLiveRead;
      }
      return VarPathOutcome::kFallthrough;
    }
    case CStatementKind::kReturnStatement: {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (rs.ReturnValue() != nullptr && (ExprMentionsVar(*rs.ReturnValue(), var) ||
                                          ExpressionModifiesVariable(*rs.ReturnValue(), var))) {
        return VarPathOutcome::kLiveRead;
      }
      return VarPathOutcome::kKilled;
    }
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      for (const auto& child : comp.Statements()) {
        if (child == nullptr) continue;
        VarPathOutcome out = CheckStmtVarOutcome(*child, var);
        if (out != VarPathOutcome::kFallthrough) {
          return out;
        }
      }
      return VarPathOutcome::kFallthrough;
    }
    case CStatementKind::kIfStatement: {
      const auto& ifs = static_cast<const IfStatement&>(stmt);
      if (ExprMentionsVar(ifs.Condition(), var) ||
          ExpressionModifiesVariable(ifs.Condition(), var)) {
        return VarPathOutcome::kLiveRead;
      }
      VarPathOutcome then_out = CheckStmtVarOutcome(ifs.ThenBranch(), var);
      if (then_out == VarPathOutcome::kLiveRead) {
        return VarPathOutcome::kLiveRead;
      }
      VarPathOutcome else_out = (ifs.ElseBranch() != nullptr)
                                    ? CheckStmtVarOutcome(*ifs.ElseBranch(), var)
                                    : VarPathOutcome::kFallthrough;
      if (else_out == VarPathOutcome::kLiveRead) {
        return VarPathOutcome::kLiveRead;
      }
      if (then_out == VarPathOutcome::kKilled && else_out == VarPathOutcome::kKilled) {
        return VarPathOutcome::kKilled;
      }
      return VarPathOutcome::kFallthrough;
    }
    case CStatementKind::kWhileStatement: {
      const auto& ws = static_cast<const WhileStatement&>(stmt);
      if (ExprMentionsVar(ws.Condition(), var) || ExpressionModifiesVariable(ws.Condition(), var)) {
        return VarPathOutcome::kLiveRead;
      }
      if (CheckStmtVarOutcome(ws.Body(), var) == VarPathOutcome::kLiveRead) {
        return VarPathOutcome::kLiveRead;
      }
      return VarPathOutcome::kFallthrough;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dws = static_cast<const DoWhileStatement&>(stmt);
      if (CheckStmtVarOutcome(dws.Body(), var) == VarPathOutcome::kLiveRead) {
        return VarPathOutcome::kLiveRead;
      }
      if (ExprMentionsVar(dws.Condition(), var) ||
          ExpressionModifiesVariable(dws.Condition(), var)) {
        return VarPathOutcome::kLiveRead;
      }
      return VarPathOutcome::kFallthrough;
    }
    case CStatementKind::kForStatement: {
      const auto& fs = static_cast<const ForStatement&>(stmt);
      if (fs.Init() != nullptr &&
          CheckStmtVarOutcome(*fs.Init(), var) == VarPathOutcome::kLiveRead) {
        return VarPathOutcome::kLiveRead;
      }
      if (fs.Condition() != nullptr && (ExprMentionsVar(*fs.Condition(), var) ||
                                        ExpressionModifiesVariable(*fs.Condition(), var))) {
        return VarPathOutcome::kLiveRead;
      }
      if (CheckStmtVarOutcome(fs.Body(), var) == VarPathOutcome::kLiveRead) {
        return VarPathOutcome::kLiveRead;
      }
      if (fs.Step() != nullptr &&
          (ExprMentionsVar(*fs.Step(), var) || ExpressionModifiesVariable(*fs.Step(), var))) {
        return VarPathOutcome::kLiveRead;
      }
      return VarPathOutcome::kFallthrough;
    }
    default:
      return VarPathOutcome::kLiveRead;
  }
}

bool IsVarMustKilledFrom(const CompoundStatement& comp, size_t start_stmt_idx, std::string_view var,
                         bool dead_at_comp_exit) {
  const auto& stmts = comp.Statements();
  for (size_t m = start_stmt_idx; m < stmts.size(); ++m) {
    if (stmts[m] == nullptr) continue;
    VarPathOutcome out = CheckStmtVarOutcome(*stmts[m], var);
    if (out == VarPathOutcome::kKilled) {
      return true;
    }
    if (out == VarPathOutcome::kLiveRead) {
      return false;
    }
  }
  return dead_at_comp_exit;
}

struct ScratchDefUseWeb {
  CompoundStatement* block = nullptr;
  size_t start_idx = 0;
  size_t end_idx = 0;
  bool end_is_if_condition = false;
  bool crosses_call = false;
  int occurrence_count = 0;
};

int CountWebOccurrences(const ScratchDefUseWeb& web, std::string_view var) {
  int total = 0;
  const auto& stmts = web.block->Statements();
  size_t stmt_end = web.end_is_if_condition ? web.end_idx : (web.end_idx + 1);
  for (size_t k = web.start_idx; k < stmt_end; ++k) {
    if (stmts[k] != nullptr) {
      total += CountIdentifierOccurrencesInStatement(*stmts[k], var);
    }
  }
  if (web.end_is_if_condition && stmts[web.end_idx] != nullptr &&
      stmts[web.end_idx]->Kind() == CStatementKind::kIfStatement) {
    const auto& ifs = static_cast<const IfStatement&>(*stmts[web.end_idx]);
    total += CountIdentifierOccurrencesInExpression(ifs.Condition(), var);
  }
  return total;
}

void CollectBasicBlockWebsInCompound(CompoundStatement* comp, std::string_view var,
                                     bool dead_at_comp_exit,
                                     std::vector<ScratchDefUseWeb>* out_webs) {
  if (comp == nullptr) return;
  auto& stmts = comp->MutableStatements();
  const size_t n = stmts.size();

  bool in_web = false;
  size_t start_idx = 0;
  size_t last_use_idx = 0;
  bool has_read_use = false;
  bool web_aborted = false;
  bool seen_call_after_last_def = false;
  bool web_crosses_call = false;

  auto try_start_web = [&](size_t idx, const CExpression& expr) -> bool {
    if (expr.Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& assign = static_cast<const AssignmentExpression&>(expr);
    if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
        static_cast<const IdentifierExpression&>(assign.Lhs()).Name() != var) {
      return false;
    }
    if (ExprMentionsVar(assign.Rhs(), var) || ExpressionModifiesVariable(assign.Rhs(), var)) {
      return false;
    }
    bool has_sc = false;
    int calls = 0;
    InspectExpressionControlAndCalls(assign.Rhs(), &has_sc, &calls);
    if (has_sc) {
      return false;
    }
    in_web = true;
    start_idx = idx;
    last_use_idx = idx;
    has_read_use = false;
    web_aborted = false;
    seen_call_after_last_def = false;
    web_crosses_call = (calls > 1);
    return true;
  };

  auto finalize_web_at_boundary = [&]() {
    if (in_web && !web_aborted && has_read_use && last_use_idx > start_idx) {
      if (IsVarMustKilledFrom(*comp, last_use_idx + 1, var, dead_at_comp_exit)) {
        ScratchDefUseWeb web{
            .block = comp,
            .start_idx = start_idx,
            .end_idx = last_use_idx,
            .end_is_if_condition = false,
            .crosses_call = web_crosses_call,
        };
        web.occurrence_count = CountWebOccurrences(web, var);
        out_webs->push_back(web);
      }
    }
    in_web = false;
  };

  for (size_t k = 0; k < n; ++k) {
    if (stmts[k] == nullptr) continue;
    CStatement& stmt = *stmts[k];
    if (stmt.Kind() == CStatementKind::kCommentStatement) {
      continue;
    }
    if (stmt.Kind() == CStatementKind::kVariableDeclarationStatement) {
      continue;
    }
    if (stmt.Kind() == CStatementKind::kExpressionStatement) {
      const auto& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
      if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(expr);
        if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(assign.Lhs()).Name() == var &&
            !ExprMentionsVar(assign.Rhs(), var) && !ExpressionModifiesVariable(assign.Rhs(), var)) {
          if (in_web && !web_aborted && has_read_use && last_use_idx > start_idx) {
            ScratchDefUseWeb web{
                .block = comp,
                .start_idx = start_idx,
                .end_idx = last_use_idx,
                .end_is_if_condition = false,
                .crosses_call = web_crosses_call,
            };
            web.occurrence_count = CountWebOccurrences(web, var);
            out_webs->push_back(web);
          }
          in_web = false;
          try_start_web(k, expr);
          continue;
        }
      }

      if (!in_web) {
        continue;
      }

      bool has_sc = false;
      int calls = 0;
      InspectExpressionControlAndCalls(expr, &has_sc, &calls);
      const bool uses_var = ExprMentionsVar(expr, var);
      const bool mods_var = ExpressionModifiesVariable(expr, var);

      if (uses_var || mods_var) {
        if (has_sc) {
          web_aborted = true;
        } else {
          if (seen_call_after_last_def) {
            web_crosses_call = true;
          }
          if (calls > 0) {
            if (calls > 1 || mods_var || !IsStandaloneCallUsingVarOnlyInArgs(expr, var)) {
              web_crosses_call = true;
            }
            seen_call_after_last_def = true;
          }
        }
        last_use_idx = k;
        if (uses_var) {
          has_read_use = true;
        }
      } else {
        if (has_sc) {
          finalize_web_at_boundary();
        } else if (calls > 0) {
          seen_call_after_last_def = true;
        }
      }
      continue;
    }

    if (stmt.Kind() == CStatementKind::kReturnStatement) {
      const auto& rs = static_cast<const ReturnStatement&>(stmt);
      if (in_web && rs.ReturnValue() != nullptr) {
        const CExpression& ret_val = *rs.ReturnValue();
        const bool uses_var = ExprMentionsVar(ret_val, var);
        const bool mods_var = ExpressionModifiesVariable(ret_val, var);
        if (uses_var || mods_var) {
          bool has_sc = false;
          int calls = 0;
          InspectExpressionControlAndCalls(ret_val, &has_sc, &calls);
          if (has_sc || mods_var) {
            web_aborted = true;
          } else {
            if (seen_call_after_last_def || calls > 0) {
              web_crosses_call = true;
            }
            last_use_idx = k;
            has_read_use = true;
          }
        }
      }
      finalize_web_at_boundary();
      continue;
    }

    if (stmt.Kind() == CStatementKind::kIfStatement) {
      auto& ifs = static_cast<IfStatement&>(stmt);
      const bool cond_uses_var = ExprMentionsVar(ifs.Condition(), var);
      const bool cond_mods_var = ExpressionModifiesVariable(ifs.Condition(), var);
      if (in_web && (cond_uses_var || cond_mods_var)) {
        bool has_sc = false;
        int calls = 0;
        InspectExpressionControlAndCalls(ifs.Condition(), &has_sc, &calls);
        if (!web_aborted && !has_sc && !cond_mods_var) {
          VarPathOutcome then_out = CheckStmtVarOutcome(ifs.ThenBranch(), var);
          VarPathOutcome else_out = (ifs.ElseBranch() != nullptr)
                                        ? CheckStmtVarOutcome(*ifs.ElseBranch(), var)
                                        : VarPathOutcome::kFallthrough;
          const bool dead_after_if = IsVarMustKilledFrom(*comp, k + 1, var, dead_at_comp_exit);
          const bool then_dead = (then_out == VarPathOutcome::kKilled) ||
                                 (then_out == VarPathOutcome::kFallthrough && dead_after_if);
          const bool else_dead = (else_out == VarPathOutcome::kKilled) ||
                                 (else_out == VarPathOutcome::kFallthrough && dead_after_if);
          if (then_dead && else_dead) {
            ScratchDefUseWeb web{
                .block = comp,
                .start_idx = start_idx,
                .end_idx = k,
                .end_is_if_condition = true,
                .crosses_call = web_crosses_call || seen_call_after_last_def || (calls > 0),
            };
            web.occurrence_count = CountWebOccurrences(web, var);
            out_webs->push_back(web);
          }
        }
        in_web = false;
      } else {
        finalize_web_at_boundary();
      }

      const bool dead_after_if = IsVarMustKilledFrom(*comp, k + 1, var, dead_at_comp_exit);
      if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
        CollectBasicBlockWebsInCompound(static_cast<CompoundStatement*>(ifs.MutableThenBranch()),
                                        var, dead_after_if, out_webs);
      }
      if (ifs.MutableElseBranch() != nullptr &&
          ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
        CollectBasicBlockWebsInCompound(static_cast<CompoundStatement*>(ifs.MutableElseBranch()),
                                        var, dead_after_if, out_webs);
      }
      continue;
    }

    finalize_web_at_boundary();

    if (stmt.Kind() == CStatementKind::kWhileStatement) {
      auto& ws = static_cast<WhileStatement&>(stmt);
      const bool dead_after_loop = (CheckStmtVarOutcome(ws, var) != VarPathOutcome::kLiveRead) &&
                                   IsVarMustKilledFrom(*comp, k + 1, var, dead_at_comp_exit);
      if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        CollectBasicBlockWebsInCompound(static_cast<CompoundStatement*>(ws.MutableBody()), var,
                                        dead_after_loop, out_webs);
      }
    } else if (stmt.Kind() == CStatementKind::kDoWhileStatement) {
      auto& dws = static_cast<DoWhileStatement&>(stmt);
      const bool dead_after_loop = (CheckStmtVarOutcome(dws, var) != VarPathOutcome::kLiveRead) &&
                                   IsVarMustKilledFrom(*comp, k + 1, var, dead_at_comp_exit);
      if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        CollectBasicBlockWebsInCompound(static_cast<CompoundStatement*>(dws.MutableBody()), var,
                                        dead_after_loop, out_webs);
      }
    } else if (stmt.Kind() == CStatementKind::kForStatement) {
      auto& fs = static_cast<ForStatement&>(stmt);
      const bool dead_after_loop = (CheckStmtVarOutcome(fs, var) != VarPathOutcome::kLiveRead) &&
                                   IsVarMustKilledFrom(*comp, k + 1, var, dead_at_comp_exit);
      if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        CollectBasicBlockWebsInCompound(static_cast<CompoundStatement*>(fs.MutableBody()), var,
                                        dead_after_loop, out_webs);
      }
    }
  }

  finalize_web_at_boundary();
}

class IdentifierRenameMutator : public AstMutator {
 public:
  IdentifierRenameMutator(std::string_view from, std::string_view to) : from_(from), to_(to) {}

  void MutateIdentifierExpression(std::unique_ptr<CExpression>* expr,
                                  IdentifierExpression* id_expr) override {
    if (id_expr->Name() == from_) {
      *expr = CExpression::Identifier(std::string(to_));
      MarkChanged();
      return;
    }
    std::string_view prefix;
    std::string_view inner_id;
    if (ExtractFunctionPointerCastCalleeIdentifier(id_expr->Name(), &prefix, &inner_id) &&
        inner_id == from_) {
      *expr = CExpression::Identifier(absl::StrCat(prefix, to_, ")"));
      MarkChanged();
    }
  }

 private:
  std::string_view from_;
  std::string_view to_;
};

void RenameWebVariable(const ScratchDefUseWeb& web, std::string_view from, std::string_view to) {
  IdentifierRenameMutator mutator(from, to);
  auto& stmts = web.block->MutableStatements();
  size_t stmt_end = web.end_is_if_condition ? web.end_idx : (web.end_idx + 1);
  for (size_t k = web.start_idx; k < stmt_end; ++k) {
    if (stmts[k] != nullptr) {
      mutator.MutateStatement(&stmts[k]);
    }
  }
  if (web.end_is_if_condition && stmts[web.end_idx] != nullptr &&
      stmts[web.end_idx]->Kind() == CStatementKind::kIfStatement) {
    auto& ifs = static_cast<IfStatement&>(*stmts[web.end_idx]);
    mutator.MutateExpression(&ifs.MutableCondition());
  }
}

}  // namespace

bool ScratchRegisterWebSplitterPass::Run(CompoundStatement* body, const AstPassContext& ctx) {
  if (body == nullptr) return false;

  absl::flat_hash_set<std::string> existing_vars;
  std::vector<std::string> all_used_vars;
  for (const auto& param : ctx.parameters) {
    existing_vars.insert(param.name);
  }
  CollectUsedVariables(*body, &all_used_vars, &existing_vars);
  for (const auto& stmt : body->Statements()) {
    if (stmt != nullptr && stmt->Kind() == CStatementKind::kVariableDeclarationStatement) {
      existing_vars.insert(static_cast<const VariableDeclarationStatement&>(*stmt).Name());
    }
  }

  std::vector<std::string> candidates = candidate_vars_;
  if (candidates.empty()) {
    for (const auto& v : all_used_vars) {
      if (IsCallerSavedScratchVarName(v)) {
        candidates.push_back(v);
      }
    }
  }

  bool any_changed = false;
  for (const auto& var : candidates) {
    if (!IsCallerSavedScratchVarName(var) || StatementTakesAddressOf(*body, var)) {
      continue;
    }

    int decl_occurrences = 0;
    for (const auto& stmt : body->Statements()) {
      if (stmt != nullptr && stmt->Kind() == CStatementKind::kVariableDeclarationStatement &&
          static_cast<const VariableDeclarationStatement&>(*stmt).Name() == var) {
        decl_occurrences += CountIdentifierOccurrencesInStatement(*stmt, var);
      }
    }
    const int total_occurrences =
        CountIdentifierOccurrencesInStatement(*body, var) - decl_occurrences;
    if (total_occurrences <= 1) {
      continue;
    }

    std::vector<ScratchDefUseWeb> webs;
    CollectBasicBlockWebsInCompound(body, var, /*dead_at_comp_exit=*/true, &webs);
    if (webs.empty()) {
      continue;
    }

    int sum_web_occurrences = 0;
    size_t non_crossing_webs_count = 0;
    int sum_non_crossing_occurrences = 0;
    for (auto& w : webs) {
      sum_web_occurrences += w.occurrence_count;
      if (!w.crosses_call) {
        ++non_crossing_webs_count;
        sum_non_crossing_occurrences += w.occurrence_count;
      }
    }

    int next_suffix = 2;
    std::vector<std::string> created_vars;
    const bool has_occurrences_outside_non_crossing_webs =
        (sum_non_crossing_occurrences < total_occurrences);
    if ((has_occurrences_outside_non_crossing_webs && non_crossing_webs_count >= 1) ||
        (!has_occurrences_outside_non_crossing_webs && non_crossing_webs_count >= 2)) {
      bool keep_first_non_crossing = !has_occurrences_outside_non_crossing_webs;
      for (size_t wi = 0; wi < webs.size(); ++wi) {
        if (webs[wi].crosses_call) continue;
        if (keep_first_non_crossing) {
          keep_first_non_crossing = false;
          continue;
        }
        std::string new_var;
        do {
          new_var = absl::StrFormat("%s_%d", var, next_suffix++);
        } while (existing_vars.contains(new_var));
        existing_vars.insert(new_var);
        RenameWebVariable(webs[wi], var, new_var);
        created_vars.push_back(new_var);
        any_changed = true;
      }
    }

    if (!created_vars.empty()) {
      // If `body` already has a `VariableDeclarationStatement` for `var` (e.g., in standalone
      // tests), insert declarations for `created_vars` right after `var`.
      auto& stmts = body->MutableStatements();
      for (size_t si = 0; si < stmts.size(); ++si) {
        if (stmts[si] != nullptr &&
            stmts[si]->Kind() == CStatementKind::kVariableDeclarationStatement) {
          const auto& vd = static_cast<const VariableDeclarationStatement&>(*stmts[si]);
          if (vd.Name() == var) {
            CType decl_type = vd.Type();
            for (size_t ci = 0; ci < created_vars.size(); ++ci) {
              stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(si + 1 + ci),
                           CStatement::VariableDeclaration(decl_type, created_vars[ci]));
            }
            break;
          }
        }
      }
      if (out_split_vars_ != nullptr) {
        (*out_split_vars_)[var] = std::move(created_vars);
      }
    }
  }

  return any_changed;
}

}  // namespace rom_nom_nom
