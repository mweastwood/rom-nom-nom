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
    const CStatement* single_then = &ifs.ThenBranch();
    if (single_then->Kind() == CStatementKind::kCompoundStatement) {
      const auto& comp = static_cast<const CompoundStatement&>(*single_then);
      single_then = (comp.Statements().size() == 1) ? comp.Statements()[0].get() : nullptr;
    }
    if (single_then != nullptr && single_then->Kind() == CStatementKind::kExpressionStatement) {
      const auto& expr = static_cast<const ExpressionStatement&>(*single_then).Expression();
      if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(expr);
        if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
            IsIdentifierStepByConst(assign.Rhs(), /*expected_var=*/"",
                                    /*require_unit_step=*/true)) {
          const std::string& lhs_name =
              static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
          if (ifs.ElseBranch() == nullptr ||
              !StatementModifiesVariable(*ifs.ElseBranch(), lhs_name)) {
            out_vars_->insert(lhs_name);
          }
        }
      }
    }
    AstVisitor::VisitIfStatement(ifs);
  }

  void VisitDoWhileStatement(const DoWhileStatement& dws) override {
    if (out_do_while_post_stepped_ != nullptr &&
        dws.Body().Kind() == CStatementKind::kCompoundStatement && StatementHasCall(dws.Body())) {
      const auto& comp = static_cast<const CompoundStatement&>(dws.Body());
      if (comp.Statements().size() >= 2) {
        const auto* prev_s = comp.Statements()[comp.Statements().size() - 2].get();
        const auto* last_s = comp.Statements().back().get();
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
              if (IsIdentifierStepByConst(last_a.Rhs(), step_var, /*require_unit_step=*/true) &&
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
          static_cast<const IntegerLiteralExpression&>(*rewritten_rhs).Value() == 65535 &&
          rewritten_lhs->Kind() == CExpressionKind::kBinaryExpression) {
        const auto& inner = static_cast<const BinaryExpression&>(*rewritten_lhs);
        if (inner.Op() == "+" || inner.Op() == "-") {
          bool lhs_u16 = false;
          bool rhs_u16 = false;
          bool lhs_narrow_post_step = false;
          bool rhs_narrow_post_step = false;
          if (inner.Lhs().Kind() == CExpressionKind::kIdentifier) {
            const std::string& lhs_id =
                static_cast<const IdentifierExpression&>(inner.Lhs()).Name();
            auto it = narrow_types.find(lhs_id);
            lhs_u16 = (it != narrow_types.end() && it->second.name == "u16");
            lhs_narrow_post_step =
                (it != narrow_types.end() && do_while_post_stepped_vars.contains(lhs_id));
          }
          if (inner.Rhs().Kind() == CExpressionKind::kIdentifier) {
            const std::string& rhs_id =
                static_cast<const IdentifierExpression&>(inner.Rhs()).Name();
            auto it = narrow_types.find(rhs_id);
            rhs_u16 = (it != narrow_types.end() && it->second.name == "u16");
            rhs_narrow_post_step =
                (it != narrow_types.end() && do_while_post_stepped_vars.contains(rhs_id));
          }
          if (lhs_u16 || rhs_u16 || lhs_narrow_post_step || rhs_narrow_post_step) {
            auto new_lhs = lhs_u16 ? inner.Lhs().Clone()
                                   : CExpression::Cast(CType::U16(), inner.Lhs().Clone());
            auto new_rhs = rhs_u16 ? inner.Rhs().Clone()
                                   : CExpression::Cast(CType::U16(), inner.Rhs().Clone());
            return CExpression::Binary(
                "&",
                CExpression::Cast(CType::U16(), CExpression::Binary(inner.Op(), std::move(new_lhs),
                                                                    std::move(new_rhs))),
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

}  // namespace rom_nom_nom
