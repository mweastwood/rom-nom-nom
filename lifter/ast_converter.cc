#include "lifter/ast_converter.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/mips.h"
#include "lifter/control_flow_graph.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/expression_builder.h"
#include "lifter/register_tracker.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

namespace {

std::unique_ptr<CExpression> RegExpr(Register reg) {
  if (reg == Register::kZero) {
    return CExpression::Integer(0);
  }
  return CExpression::Identifier(ExpressionBuilder::RegisterVarName(reg));
}

// Recursively collects all variable identifiers referenced in a CExpression.
void CollectVariablesInExpression(const CExpression& expr, std::vector<std::string>* vars,
                                  absl::flat_hash_set<std::string>* seen) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier: {
      const auto& id = static_cast<const IdentifierExpression&>(expr);
      const std::string& name = id.Name();
      if (!name.empty() && seen->insert(name).second) {
        vars->push_back(name);
      }
      break;
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      CollectVariablesInExpression(un.Operand(), vars, seen);
      break;
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      CollectVariablesInExpression(bin.Lhs(), vars, seen);
      CollectVariablesInExpression(bin.Rhs(), vars, seen);
      break;
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      CollectVariablesInExpression(assign.Lhs(), vars, seen);
      CollectVariablesInExpression(assign.Rhs(), vars, seen);
      break;
    }
    case CExpressionKind::kCastExpression: {
      const auto& cast = static_cast<const CastExpression&>(expr);
      CollectVariablesInExpression(cast.Operand(), vars, seen);
      break;
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      for (const auto& arg : call.Arguments()) {
        CollectVariablesInExpression(*arg, vars, seen);
      }
      break;
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& member = static_cast<const MemberAccessExpression&>(expr);
      CollectVariablesInExpression(member.Object(), vars, seen);
      break;
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      CollectVariablesInExpression(arr.Array(), vars, seen);
      CollectVariablesInExpression(arr.Index(), vars, seen);
      break;
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      CollectVariablesInExpression(tern.Condition(), vars, seen);
      CollectVariablesInExpression(tern.TrueExpression(), vars, seen);
      CollectVariablesInExpression(tern.FalseExpression(), vars, seen);
      break;
    }
    case CExpressionKind::kIntegerLiteral:
    case CExpressionKind::kFloatLiteral:
    case CExpressionKind::kStringLiteral:
      break;
  }
}

// Recursively collects all variable names referenced in a CStatement.
void CollectUsedVariables(const CStatement& stmt, std::vector<std::string>* vars,
                          absl::flat_hash_set<std::string>* seen) {
  switch (stmt.Kind()) {
    case CStatementKind::kExpressionStatement: {
      const auto& expr_stmt = static_cast<const ExpressionStatement&>(stmt);
      CollectVariablesInExpression(expr_stmt.Expression(), vars, seen);
      break;
    }
    case CStatementKind::kCompoundStatement: {
      const auto& comp = static_cast<const CompoundStatement&>(stmt);
      for (const auto& child : comp.Statements()) {
        CollectUsedVariables(*child, vars, seen);
      }
      break;
    }
    case CStatementKind::kIfStatement: {
      const auto& if_stmt = static_cast<const IfStatement&>(stmt);
      CollectVariablesInExpression(if_stmt.Condition(), vars, seen);
      CollectUsedVariables(if_stmt.ThenBranch(), vars, seen);
      if (if_stmt.ElseBranch() != nullptr) {
        CollectUsedVariables(*if_stmt.ElseBranch(), vars, seen);
      }
      break;
    }
    case CStatementKind::kWhileStatement: {
      const auto& while_stmt = static_cast<const WhileStatement&>(stmt);
      CollectVariablesInExpression(while_stmt.Condition(), vars, seen);
      CollectUsedVariables(while_stmt.Body(), vars, seen);
      break;
    }
    case CStatementKind::kDoWhileStatement: {
      const auto& dowhile_stmt = static_cast<const DoWhileStatement&>(stmt);
      CollectUsedVariables(dowhile_stmt.Body(), vars, seen);
      CollectVariablesInExpression(dowhile_stmt.Condition(), vars, seen);
      break;
    }
    case CStatementKind::kReturnStatement: {
      const auto& ret = static_cast<const ReturnStatement&>(stmt);
      if (ret.ReturnValue() != nullptr) {
        CollectVariablesInExpression(*ret.ReturnValue(), vars, seen);
      }
      break;
    }
    case CStatementKind::kSwitchStatement: {
      const auto& switch_stmt = static_cast<const SwitchStatement&>(stmt);
      CollectVariablesInExpression(switch_stmt.Condition(), vars, seen);
      for (const auto& sc : switch_stmt.Cases()) {
        if (sc.body != nullptr) {
          CollectUsedVariables(*sc.body, vars, seen);
        }
      }
      break;
    }
    default:
      break;
  }
}

// Checks if a statement returns a non-void value.
bool HasReturnValue(const CStatement& stmt) {
  if (stmt.Kind() == CStatementKind::kReturnStatement) {
    const auto& ret = static_cast<const ReturnStatement&>(stmt);
    return ret.ReturnValue() != nullptr;
  }
  if (stmt.Kind() == CStatementKind::kCompoundStatement) {
    const auto& comp = static_cast<const CompoundStatement&>(stmt);
    for (const auto& child : comp.Statements()) {
      if (HasReturnValue(*child)) return true;
    }
  } else if (stmt.Kind() == CStatementKind::kIfStatement) {
    const auto& if_stmt = static_cast<const IfStatement&>(stmt);
    if (HasReturnValue(if_stmt.ThenBranch())) return true;
    if (if_stmt.ElseBranch() != nullptr && HasReturnValue(*if_stmt.ElseBranch())) return true;
  } else if (stmt.Kind() == CStatementKind::kSwitchStatement) {
    const auto& switch_stmt = static_cast<const SwitchStatement&>(stmt);
    for (const auto& sc : switch_stmt.Cases()) {
      if (sc.body != nullptr && HasReturnValue(*sc.body)) return true;
    }
  }
  return false;
}

}  // namespace

std::unique_ptr<CExpression> AstConverter::ConvertExpression(const LiftedExpression& expr) {
  switch (expr.kind) {
    case ExpressionKind::kIntegerLiteral:
      return CExpression::Integer(expr.int_val, expr.is_hex);

    case ExpressionKind::kVariable:
    case ExpressionKind::kGlobalRef:
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kUnaryOp:
      if (!expr.args.empty() && expr.args[0]) {
        return CExpression::Unary(expr.op, ConvertExpression(*expr.args[0]));
      }
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kBinaryOp:
      if (expr.args.size() >= 2 && expr.args[0] && expr.args[1]) {
        return CExpression::Binary(expr.op, ConvertExpression(*expr.args[0]),
                                   ConvertExpression(*expr.args[1]));
      }
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kMemoryLoad:
      if (!expr.args.empty() && expr.args[0]) {
        auto cast = CExpression::Cast(CType::Named(expr.name).MakePointer(),
                                      ConvertExpression(*expr.args[0]));
        return CExpression::Unary("*", std::move(cast));
      }
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kFunctionCall: {
      std::vector<std::unique_ptr<CExpression>> call_args;
      call_args.reserve(expr.args.size());
      for (const auto& arg : expr.args) {
        if (arg) {
          call_args.push_back(ConvertExpression(*arg));
        }
      }
      return CExpression::Call(CExpression::Identifier(expr.name), std::move(call_args));
    }
  }
  return CExpression::Identifier("unk");
}

std::unique_ptr<CStatement> AstConverter::ConvertStatement(const LiftedStatement& stmt) {
  switch (stmt.kind) {
    case StatementKind::kAssignment: {
      if (stmt.destination_variable.empty() || stmt.destination_variable == "0") {
        if (stmt.expression) {
          return CStatement::Expression(ConvertExpression(*stmt.expression));
        }
        return nullptr;
      }
      auto lhs = CExpression::Identifier(stmt.destination_variable);
      auto rhs = stmt.expression ? ConvertExpression(*stmt.expression) : CExpression::Integer(0);
      return CStatement::Expression(CExpression::Assignment("=", std::move(lhs), std::move(rhs)));
    }

    case StatementKind::kStore: {
      auto target = stmt.destination_address ? ConvertExpression(*stmt.destination_address)
                                             : CExpression::Identifier("addr");
      std::string type_name = stmt.store_type.empty() ? "s32" : stmt.store_type;
      auto cast = CExpression::Cast(CType::Named(type_name).MakePointer(), std::move(target));
      auto deref = CExpression::Unary("*", std::move(cast));
      auto val = stmt.expression ? ConvertExpression(*stmt.expression) : CExpression::Integer(0);
      return CStatement::Expression(CExpression::Assignment("=", std::move(deref), std::move(val)));
    }

    case StatementKind::kCall: {
      if (stmt.expression) {
        return CStatement::Expression(ConvertExpression(*stmt.expression));
      }
      return nullptr;
    }

    case StatementKind::kReturn: {
      if (stmt.expression) {
        return CStatement::Return(ConvertExpression(*stmt.expression));
      }
      return CStatement::Return();
    }
  }
  return nullptr;
}

std::unique_ptr<CExpression> AstConverter::ExtractBranchCondition(const BasicBlock& block,
                                                                  bool invert_condition) {
  // Find the branch instruction (skipping trailing delay slot if present)
  const Instruction* branch_inst = nullptr;
  for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
    if (it->IsBranch()) {
      branch_inst = &(*it);
      break;
    }
  }

  if (branch_inst == nullptr) {
    return CExpression::Integer(invert_condition ? 0 : 1);
  }

  // Check if this block's branch folds a reaching comparison instruction
  auto folded_comp = ExpressionBuilder::FindFoldedComparison(block.instructions);
  if (folded_comp.has_value()) {
    const auto& def_inst = block.instructions[folded_comp->instruction_index];

    bool branch_taken_on_nonzero = false;
    switch (branch_inst->opcode) {
      case Opcode::kBne:
      case Opcode::kBnel:
      case Opcode::kBgtz:
      case Opcode::kBgtzl:
        branch_taken_on_nonzero = true;
        break;
      case Opcode::kBeq:
      case Opcode::kBeql:
      case Opcode::kBlez:
      case Opcode::kBlezl:
        branch_taken_on_nonzero = false;
        break;
      case Opcode::kBc1t:
      case Opcode::kBc1tl:
        branch_taken_on_nonzero = true;
        break;
      case Opcode::kBc1f:
      case Opcode::kBc1fl:
        branch_taken_on_nonzero = false;
        break;
      default:
        break;
    }

    bool want_true = branch_taken_on_nonzero ^ invert_condition;

    switch (def_inst.opcode) {
      case Opcode::kSlt: {
        std::string op = want_true ? "<" : ">=";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        Register def_rt = def_inst.rt.value_or(Register::kZero);
        return CExpression::Binary(op, RegExpr(def_rs), RegExpr(def_rt));
      }

      case Opcode::kSlti: {
        std::string op = want_true ? "<" : ">=";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        return CExpression::Binary(op, RegExpr(def_rs), CExpression::Integer(def_inst.immediate));
      }

      case Opcode::kSltu: {
        std::string op = want_true ? "<" : ">=";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        Register def_rt = def_inst.rt.value_or(Register::kZero);
        return CExpression::Binary(op, CExpression::Cast(CType::U32(), RegExpr(def_rs)),
                                   CExpression::Cast(CType::U32(), RegExpr(def_rt)));
      }

      case Opcode::kSltiu: {
        std::string op = want_true ? "<" : ">=";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        uint32_t uimm = static_cast<uint32_t>(def_inst.immediate);
        return CExpression::Binary(
            op, CExpression::Cast(CType::U32(), RegExpr(def_rs)),
            CExpression::Integer(uimm, /*is_hex=*/uimm > 0xFFFF, /*is_unsigned=*/true));
      }

      case Opcode::kXor: {
        std::string op = want_true ? "!=" : "==";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        Register def_rt = def_inst.rt.value_or(Register::kZero);
        return CExpression::Binary(op, RegExpr(def_rs), RegExpr(def_rt));
      }

      case Opcode::kXori: {
        std::string op = want_true ? "!=" : "==";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        uint32_t uimm = def_inst.UnsignedImmediate();
        return CExpression::Binary(op, RegExpr(def_rs),
                                   CExpression::Integer(uimm, /*is_hex=*/uimm > 0xFFFF));
      }

      case Opcode::kAndi: {
        std::string op = want_true ? "!=" : "==";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        uint32_t uimm = def_inst.UnsignedImmediate();
        auto bit_and = CExpression::Binary("&", RegExpr(def_rs),
                                           CExpression::Integer(uimm, /*is_hex=*/uimm > 0xFFFF));
        return CExpression::Binary(op, std::move(bit_and), CExpression::Integer(0));
      }

      case Opcode::kAnd: {
        std::string op = want_true ? "!=" : "==";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        Register def_rt = def_inst.rt.value_or(Register::kZero);
        auto bit_and = CExpression::Binary("&", RegExpr(def_rs), RegExpr(def_rt));
        return CExpression::Binary(op, std::move(bit_and), CExpression::Integer(0));
      }

      case Opcode::kCEqS:
      case Opcode::kCEqD: {
        std::string op = want_true ? "==" : "!=";
        return CExpression::Binary(op,
                                   CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                       def_inst.fs.value_or(FpRegister::kF0))),
                                   CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                       def_inst.ft.value_or(FpRegister::kF0))));
      }

      case Opcode::kCLtS:
      case Opcode::kCLtD: {
        std::string op = want_true ? "<" : ">=";
        return CExpression::Binary(op,
                                   CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                       def_inst.fs.value_or(FpRegister::kF0))),
                                   CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                       def_inst.ft.value_or(FpRegister::kF0))));
      }

      case Opcode::kCLeS:
      case Opcode::kCLeD: {
        std::string op = want_true ? "<=" : ">";
        return CExpression::Binary(op,
                                   CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                       def_inst.fs.value_or(FpRegister::kF0))),
                                   CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                       def_inst.ft.value_or(FpRegister::kF0))));
      }

      default:
        break;
    }
  }

  Register rs = branch_inst->rs.value_or(Register::kZero);
  Register rt = branch_inst->rt.value_or(Register::kZero);

  switch (branch_inst->opcode) {
    case Opcode::kBeq:
    case Opcode::kBeql: {
      std::string op = invert_condition ? "!=" : "==";
      return CExpression::Binary(op, RegExpr(rs), RegExpr(rt));
    }

    case Opcode::kBne:
    case Opcode::kBnel: {
      std::string op = invert_condition ? "==" : "!=";
      return CExpression::Binary(op, RegExpr(rs), RegExpr(rt));
    }

    case Opcode::kBgtz:
    case Opcode::kBgtzl: {
      std::string op = invert_condition ? "<=" : ">";
      return CExpression::Binary(op, RegExpr(rs), CExpression::Integer(0));
    }

    case Opcode::kBgez:
    case Opcode::kBgezl: {
      std::string op = invert_condition ? "<" : ">=";
      return CExpression::Binary(op, RegExpr(rs), CExpression::Integer(0));
    }

    case Opcode::kBltz:
    case Opcode::kBltzl: {
      std::string op = invert_condition ? ">=" : "<";
      return CExpression::Binary(op, RegExpr(rs), CExpression::Integer(0));
    }

    case Opcode::kBlez:
    case Opcode::kBlezl: {
      std::string op = invert_condition ? ">" : "<=";
      return CExpression::Binary(op, RegExpr(rs), CExpression::Integer(0));
    }

    case Opcode::kBc1t:
    case Opcode::kBc1tl:
      if (invert_condition) {
        return CExpression::Unary("!", CExpression::Identifier("fcond"));
      }
      return CExpression::Identifier("fcond");

    case Opcode::kBc1f:
    case Opcode::kBc1fl:
      if (invert_condition) {
        return CExpression::Identifier("fcond");
      }
      return CExpression::Unary("!", CExpression::Identifier("fcond"));

    default:
      break;
  }

  return CExpression::Integer(invert_condition ? 0 : 1);
}

namespace {

class ConverterContext {
 public:
  ConverterContext(const ControlFlowGraph& cfg, const SymbolIndex* symbol_index,
                   const StructuredRegion& root_region, const SplitConfig* split_config,
                   const absl::flat_hash_map<std::string, int>* function_parameter_counts,
                   const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
                   bool returns_v0, bool returns_f0)
      : cfg_(cfg),
        symbol_index_(symbol_index),
        split_config_(split_config),
        function_parameter_counts_(function_parameter_counts),
        function_fp_parameter_counts_(function_fp_parameter_counts),
        returns_v0_(returns_v0),
        returns_f0_(returns_f0) {
    CollectGotoTargets(root_region);
  }

  const absl::flat_hash_set<uint32_t>& GotoTargets() const { return goto_targets_; }
  const absl::flat_hash_set<uint32_t>& EmittedLabels() const { return emitted_labels_; }

  void ConvertRegion(const StructuredRegion& region, CompoundStatement* target_block) {
    switch (region.type) {
      case RegionType::kBlock:
        EmitBlockStatements(region.block_id, target_block);
        break;

      case RegionType::kSequence:
        for (const auto& child : region.children) {
          if (child) {
            ConvertRegion(*child, target_block);
          }
        }
        break;

      case RegionType::kIfThen: {
        const auto* cond_block = cfg_.GetBlock(region.condition_block_id);
        bool has_branch_likely = false;
        if (cond_block != nullptr) {
          const auto* term = cond_block->Terminator();
          if (term != nullptr && term->IsBranchLikely() && cond_block->instructions.size() >= 2) {
            has_branch_likely = true;
          }
        }

        EmitBlockStatements(region.condition_block_id, target_block,
                            /*omit_last_instruction=*/has_branch_likely);
        auto cond = cond_block != nullptr
                        ? AstConverter::ExtractBranchCondition(*cond_block, region.invert_condition)
                        : CExpression::Integer(1);

        auto then_body = std::make_unique<CompoundStatement>();
        auto else_body = std::make_unique<CompoundStatement>();
        if (has_branch_likely) {
          const auto& delay_inst = cond_block->instructions.back();
          auto delay_stmts =
              ExpressionBuilder::LiftInstructions(absl::MakeSpan(&delay_inst, 1), symbol_index_,
                                                  split_config_, function_parameter_counts_);
          for (auto& s : delay_stmts) {
            auto cs = AstConverter::ConvertStatement(s);
            if (cs != nullptr) {
              if (region.invert_condition) {
                else_body->AddStatement(std::move(cs));
              } else {
                then_body->AddStatement(std::move(cs));
              }
            }
          }
        }

        if (!region.children.empty() && region.children[0]) {
          ConvertRegion(*region.children[0], then_body.get());
        }

        if (!else_body->Statements().empty()) {
          target_block->AddStatement(
              CStatement::If(std::move(cond), std::move(then_body), std::move(else_body)));
        } else {
          target_block->AddStatement(CStatement::If(std::move(cond), std::move(then_body)));
        }
        break;
      }

      case RegionType::kIfThenElse: {
        const auto* cond_block = cfg_.GetBlock(region.condition_block_id);
        bool has_branch_likely = false;
        if (cond_block != nullptr) {
          const auto* term = cond_block->Terminator();
          if (term != nullptr && term->IsBranchLikely() && cond_block->instructions.size() >= 2) {
            has_branch_likely = true;
          }
        }

        EmitBlockStatements(region.condition_block_id, target_block,
                            /*omit_last_instruction=*/has_branch_likely);
        auto cond = cond_block != nullptr
                        ? AstConverter::ExtractBranchCondition(*cond_block, region.invert_condition)
                        : CExpression::Integer(1);

        auto then_body = std::make_unique<CompoundStatement>();
        auto else_body = std::make_unique<CompoundStatement>();
        if (has_branch_likely) {
          const auto& delay_inst = cond_block->instructions.back();
          auto delay_stmts =
              ExpressionBuilder::LiftInstructions(absl::MakeSpan(&delay_inst, 1), symbol_index_,
                                                  split_config_, function_parameter_counts_);
          for (auto& s : delay_stmts) {
            auto cs = AstConverter::ConvertStatement(s);
            if (cs != nullptr) {
              if (region.invert_condition) {
                else_body->AddStatement(std::move(cs));
              } else {
                then_body->AddStatement(std::move(cs));
              }
            }
          }
        }

        if (region.children.size() > 0 && region.children[0]) {
          ConvertRegion(*region.children[0], then_body.get());
        }
        if (region.children.size() > 1 && region.children[1]) {
          ConvertRegion(*region.children[1], else_body.get());
        }
        target_block->AddStatement(
            CStatement::If(std::move(cond), std::move(then_body), std::move(else_body)));
        break;
      }

      case RegionType::kLoop: {
        if (region.loop_type == LoopType::kDoWhile) {
          uint32_t cond_block_id =
              region.condition_block_id != 0 ? region.condition_block_id : region.loop_header;
          bool decouple_delay =
              ShouldDecoupleLoopConditionDelaySlot(cond_block_id, region.loop_header);
          if (decouple_delay) {
            omit_last_instruction_blocks_.insert(cond_block_id);
          }

          auto loop_body = std::make_unique<CompoundStatement>();
          if (!region.children.empty() && region.children[0]) {
            ConvertRegion(*region.children[0], loop_body.get());
          }

          const auto* cond_block = cfg_.GetBlock(cond_block_id);
          auto cond = cond_block != nullptr
                          ? AstConverter::ExtractBranchCondition(*cond_block, false)
                          : CExpression::Integer(1);
          target_block->AddStatement(CStatement::DoWhile(std::move(loop_body), std::move(cond)));

          if (decouple_delay && cond_block != nullptr && cond_block->instructions.size() >= 2) {
            const auto& delay_inst = cond_block->instructions.back();
            auto delay_stmts =
                ExpressionBuilder::LiftInstructions(absl::MakeSpan(&delay_inst, 1), symbol_index_,
                                                    split_config_, function_parameter_counts_);
            for (auto& s : delay_stmts) {
              auto cs = AstConverter::ConvertStatement(s);
              if (cs != nullptr) {
                target_block->AddStatement(std::move(cs));
              }
            }
          }
        } else if (region.loop_type == LoopType::kInfinite) {
          auto loop_body = std::make_unique<CompoundStatement>();
          if (!region.children.empty() && region.children[0]) {
            ConvertRegion(*region.children[0], loop_body.get());
          }
          target_block->AddStatement(
              CStatement::While(CExpression::Integer(1), std::move(loop_body)));
        } else {
          // Pre-tested while loop
          const auto* header_block = cfg_.GetBlock(region.loop_header);
          auto cond = header_block != nullptr
                          ? AstConverter::ExtractBranchCondition(*header_block, false)
                          : CExpression::Integer(1);
          auto loop_body = std::make_unique<CompoundStatement>();
          if (!region.children.empty() && region.children[0]) {
            ConvertRegion(*region.children[0], loop_body.get());
          }
          target_block->AddStatement(CStatement::While(std::move(cond), std::move(loop_body)));
        }
        break;
      }

      case RegionType::kBreak:
        target_block->AddStatement(CStatement::Break());
        break;

      case RegionType::kContinue:
        target_block->AddStatement(CStatement::Continue());
        break;

      case RegionType::kGoto:
        target_block->AddStatement(CStatement::Goto(absl::StrFormat("block_%d", region.block_id)));
        break;

      case RegionType::kSwitch: {
        const auto* jt = region.jump_table;
        std::unique_ptr<CExpression> cond_expr;
        if (jt != nullptr && jt->index_register != Register::kZero) {
          cond_expr = RegExpr(jt->index_register);
        } else {
          cond_expr = CExpression::Identifier("cond");
        }

        std::vector<SwitchCase> c_cases;
        c_cases.reserve(region.cases.size());

        for (const auto& sc : region.cases) {
          SwitchCase c_case;
          c_case.case_values = sc.case_values;
          c_case.is_default = sc.is_default;
          c_case.body = std::make_unique<CompoundStatement>();

          if (sc.body) {
            ConvertRegion(*sc.body, c_case.body.get());
          }

          if (c_case.body->IsEmpty() || !HasReturnValue(*c_case.body)) {
            const auto& stmts = c_case.body->Statements();
            if (stmts.empty() || stmts.back()->Kind() != CStatementKind::kBreakStatement) {
              c_case.body->AddStatement(CStatement::Break());
            }
          }

          c_cases.push_back(std::move(c_case));
        }

        target_block->AddStatement(CStatement::Switch(std::move(cond_expr), std::move(c_cases)));
        break;
      }
    }
  }

 private:
  void CollectGotoTargets(const StructuredRegion& region) {
    if (region.type == RegionType::kGoto) {
      goto_targets_.insert(region.block_id);
    }
    for (const auto& child : region.children) {
      if (child) {
        CollectGotoTargets(*child);
      }
    }
    for (const auto& sc : region.cases) {
      if (sc.body) {
        CollectGotoTargets(*sc.body);
      }
    }
  }

  bool ShouldDecoupleLoopConditionDelaySlot(uint32_t cond_block_id,
                                            uint32_t header_block_id) const {
    const auto* cond_block = cfg_.GetBlock(cond_block_id);
    if (cond_block == nullptr || cond_block->instructions.size() < 2) {
      return false;
    }
    const auto* term = cond_block->Terminator();
    if (term == nullptr || !term->IsBranch() || term->IsBranchLikely()) {
      return false;
    }
    const auto& delay_inst = cond_block->instructions.back();
    if (delay_inst.IsNop()) {
      return false;
    }

    RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
    if (delay_ud.gpr_defs.empty()) {
      return false;
    }

    // Check 1: Does delay_inst define a register used by the branch condition or its folded
    // comparison?
    absl::flat_hash_set<Register> cond_regs;
    if (term->rs.has_value() && *term->rs != Register::kZero) {
      cond_regs.insert(*term->rs);
    }
    if (term->rt.has_value() && *term->rt != Register::kZero) {
      cond_regs.insert(*term->rt);
    }
    auto folded_comp = ExpressionBuilder::FindFoldedComparison(cond_block->instructions);
    if (folded_comp.has_value() &&
        folded_comp->instruction_index < cond_block->instructions.size()) {
      const auto& def_inst = cond_block->instructions[folded_comp->instruction_index];
      RegisterUseDef def_ud = GetInstructionUseDef(def_inst);
      for (Register r : def_ud.gpr_uses) {
        if (r != Register::kZero) {
          cond_regs.insert(r);
        }
      }
      for (Register r : def_ud.gpr_defs) {
        if (r != Register::kZero) {
          cond_regs.insert(r);
        }
      }
    }
    for (Register r : delay_ud.gpr_defs) {
      if (cond_regs.contains(r)) {
        return true;
      }
    }

    // Check 2: Are all registers defined by delay_inst killed at header_block before being used?
    const auto* header_block = cfg_.GetBlock(header_block_id);
    if (header_block != nullptr) {
      bool all_killed = true;
      for (Register def_reg : delay_ud.gpr_defs) {
        if (def_reg == Register::kZero) continue;
        bool killed = false;
        for (const auto& inst : header_block->instructions) {
          RegisterUseDef inst_ud = GetInstructionUseDef(inst);
          bool used = false;
          for (Register u : inst_ud.gpr_uses) {
            if (u == def_reg) {
              used = true;
              break;
            }
          }
          if (used) {
            break;
          }
          bool defined = false;
          for (Register d : inst_ud.gpr_defs) {
            if (d == def_reg) {
              defined = true;
              break;
            }
          }
          if (defined) {
            killed = true;
            break;
          }
        }
        if (!killed) {
          all_killed = false;
          break;
        }
      }
      if (all_killed) {
        return true;
      }
    }

    return false;
  }

  void EmitBlockStatements(uint32_t block_id, CompoundStatement* target_block,
                           bool omit_last_instruction = false) {
    if (emitted_blocks_.contains(block_id)) {
      return;
    }
    emitted_blocks_.insert(block_id);

    if (goto_targets_.contains(block_id)) {
      target_block->AddStatement(CStatement::Label(absl::StrFormat("block_%d", block_id)));
      emitted_labels_.insert(block_id);
    }

    const auto* block = cfg_.GetBlock(block_id);
    if (block == nullptr) {
      return;
    }

    bool should_omit = omit_last_instruction || omit_last_instruction_blocks_.contains(block_id);
    std::vector<LiftedStatement> lifted_stmts;
    if (should_omit && block->instructions.size() > 1) {
      absl::Span<const Instruction> inst_span(block->instructions.data(),
                                              block->instructions.size() - 1);
      lifted_stmts = ExpressionBuilder::LiftInstructions(inst_span, symbol_index_, split_config_,
                                                         function_parameter_counts_,
                                                         function_fp_parameter_counts_);
    } else {
      lifted_stmts =
          ExpressionBuilder::LiftBlock(*block, symbol_index_, split_config_,
                                       function_parameter_counts_, function_fp_parameter_counts_);
    }
    for (auto& lifted_stmt : lifted_stmts) {
      if (lifted_stmt.kind == StatementKind::kReturn) {
        if (returns_v0_) {
          if (!lifted_stmt.expression) {
            lifted_stmt.expression = LiftedExpression::Variable("v0");
          }
        } else if (returns_f0_) {
          if (!lifted_stmt.expression) {
            lifted_stmt.expression = LiftedExpression::Variable("f0");
          }
        } else {
          lifted_stmt.expression = nullptr;
        }
      }
      auto c_stmt = AstConverter::ConvertStatement(lifted_stmt);
      if (c_stmt != nullptr) {
        target_block->AddStatement(std::move(c_stmt));
      }
    }
  }

  const ControlFlowGraph& cfg_;
  const SymbolIndex* symbol_index_;
  const SplitConfig* split_config_;
  const absl::flat_hash_map<std::string, int>* function_parameter_counts_;
  const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts_;
  bool returns_v0_ = false;
  bool returns_f0_ = false;
  absl::flat_hash_set<uint32_t> goto_targets_;
  absl::flat_hash_set<uint32_t> emitted_labels_;
  absl::flat_hash_set<uint32_t> emitted_blocks_;
  absl::flat_hash_set<uint32_t> omit_last_instruction_blocks_;
};

}  // namespace

FunctionDeclaration AstConverter::Convert(const ControlFlowGraph& cfg,
                                          const StructuredRegion& root_region,
                                          const SymbolIndex* symbol_index,
                                          const AstConverterOptions& options) {
  auto body = std::make_unique<CompoundStatement>();

  bool returns_v0 = false;
  bool returns_f0 = false;
  if (options.return_type.has_value()) {
    std::string rt = options.return_type->ToString();
    returns_v0 = (rt != "void" && rt != "f32" && rt != "f64");
    returns_f0 = (rt == "f32" || rt == "f64");
  } else {
    returns_v0 = ExpressionBuilder::DetermineReturnsV0(cfg);
    if (!returns_v0) {
      returns_f0 = ExpressionBuilder::DetermineReturnsF0(cfg);
    }
  }

  ConverterContext ctx(cfg, symbol_index, root_region, options.split_config,
                       options.function_parameter_counts, options.function_fp_parameter_counts,
                       returns_v0, returns_f0);
  ctx.ConvertRegion(root_region, body.get());

  for (uint32_t target_block_id : ctx.GotoTargets()) {
    if (!ctx.EmittedLabels().contains(target_block_id)) {
      body->AddStatement(CStatement::Label(absl::StrFormat("block_%d", target_block_id)));
      body->AddStatement(CStatement::Expression(CExpression::Integer(0)));
    }
  }

  // Determine parameters
  std::vector<CParameter> parameters = options.parameters;
  if (parameters.empty()) {
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
    if (parameter_count < 0) {
      if (!all_instructions.empty()) {
        parameter_count = ExpressionBuilder::DetermineParameterCount(all_instructions);
      }
    }
    absl::flat_hash_set<std::string> double_vars;
    for (const auto& block : cfg.Blocks()) {
      for (const auto& inst : block.instructions) {
        if (inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
            inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
            inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
            inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
            inst.opcode == Opcode::kCvtSD || inst.opcode == Opcode::kTruncWD ||
            inst.opcode == Opcode::kCEqD || inst.opcode == Opcode::kCLtD ||
            inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kLdc1 ||
            inst.opcode == Opcode::kSdc1 || inst.opcode == Opcode::kDmfc1 ||
            inst.opcode == Opcode::kDmtc1) {
          if (inst.fd) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fd));
          if (inst.fs) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fs));
          if (inst.ft) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.ft));
        }
      }
    }

    if (parameter_count > 0) {
      for (int param_index = 0; param_index < parameter_count; ++param_index) {
        parameters.push_back(
            CParameter{.type = CType::S32(), .name = absl::StrFormat("arg%d", param_index)});
      }
    } else if (parameter_count == 0 && !all_instructions.empty()) {
      int fp_param_count = 0;
      if (options.function_fp_parameter_counts != nullptr) {
        auto it = options.function_fp_parameter_counts->find(options.function_name);
        if (it != options.function_fp_parameter_counts->end()) {
          fp_param_count = it->second;
        }
      }
      if (fp_param_count == 0) {
        fp_param_count = ExpressionBuilder::DetermineFpParameterCount(all_instructions);
      }
      if (fp_param_count >= 1) {
        CType t = double_vars.contains("f12") ? CType::F64() : CType::F32();
        parameters.push_back(CParameter{.type = t, .name = "f12"});
        if (fp_param_count >= 2) {
          CType t2 = double_vars.contains("f14") ? CType::F64() : CType::F32();
          parameters.push_back(CParameter{.type = t2, .name = "f14"});
        }
      }
    } else {
      std::string body_text = body->ToString(0);
      int max_argument_index = -1;
      for (int arg_index = 3; arg_index >= 0; --arg_index) {
        if (body_text.find(absl::StrFormat("arg%d", arg_index)) != std::string::npos) {
          max_argument_index = arg_index;
          break;
        }
      }
      for (int arg_index = 0; arg_index <= max_argument_index; ++arg_index) {
        parameters.push_back(
            CParameter{.type = CType::S32(), .name = absl::StrFormat("arg%d", arg_index)});
      }
    }
  }

  // Determine return type
  CType return_type = CType::Void();
  if (options.return_type.has_value()) {
    return_type = *options.return_type;
  } else if (returns_v0) {
    return_type = CType::S32();
  } else if (HasReturnValue(*body)) {
    bool returns_f0 = false;
    for (const auto& stmt : body->Statements()) {
      if (stmt->Kind() == CStatementKind::kReturnStatement) {
        const auto& r = static_cast<const ReturnStatement&>(*stmt);
        if (r.ReturnValue() != nullptr && r.ReturnValue()->ToString() == "f0") {
          returns_f0 = true;
          break;
        }
      }
    }
    if (returns_f0) {
      absl::flat_hash_set<std::string> double_vars;
      for (const auto& block : cfg.Blocks()) {
        for (const auto& inst : block.instructions) {
          if (inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
              inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
              inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
              inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
              inst.opcode == Opcode::kCvtSD || inst.opcode == Opcode::kTruncWD ||
              inst.opcode == Opcode::kCEqD || inst.opcode == Opcode::kCLtD ||
              inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kLdc1 ||
              inst.opcode == Opcode::kSdc1 || inst.opcode == Opcode::kDmfc1 ||
              inst.opcode == Opcode::kDmtc1) {
            if (inst.fd) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fd));
            if (inst.fs) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fs));
            if (inst.ft) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.ft));
          }
        }
      }
      return_type = double_vars.contains("f0") ? CType::F64() : CType::F32();
    } else {
      return_type = CType::S32();
    }
  }

  // Collect all variables referenced in the function body
  std::vector<std::string> raw_vars;
  absl::flat_hash_set<std::string> seen;
  for (const auto& param : parameters) {
    seen.insert(param.name);
  }
  CollectUsedVariables(*body, &raw_vars, &seen);

  auto is_valid_c_id = [](std::string_view name) -> bool {
    if (name.empty()) return false;
    if (!std::isalpha(name[0]) && name[0] != '_') return false;
    for (char c : name) {
      if (!std::isalnum(c) && c != '_') return false;
    }
    return true;
  };

  std::vector<std::string> local_vars_to_declare;
  for (const auto& var_name : raw_vars) {
    if (!is_valid_c_id(var_name)) continue;
    // Do not declare parameters
    bool is_param = false;
    for (const auto& param : parameters) {
      if (param.name == var_name) {
        is_param = true;
        break;
      }
    }
    if (is_param) {
      continue;
    }

    // Do not declare globals (prefixed with g_)
    if (absl::StartsWith(var_name, "g_")) {
      continue;
    }
    // Do not declare symbols present in symbol registry
    if (symbol_index != nullptr && symbol_index->FindByName(var_name) != nullptr) {
      continue;
    }
    // Do not declare standard C keywords, literals, or types
    if (var_name == "NULL" || var_name == "TRUE" || var_name == "FALSE" || var_name == "s32" ||
        var_name == "u32" || var_name == "s16" || var_name == "u16" || var_name == "s8" ||
        var_name == "u8" || var_name == "f32" || var_name == "f64" || var_name == "s64" ||
        var_name == "u64") {
      continue;
    }
    // Do not declare function names (func_*, Os*, Gu*, Leo*, main*, idle*)
    if (absl::StartsWith(var_name, "func_") || absl::StartsWith(var_name, "Os") ||
        absl::StartsWith(var_name, "Gu") || absl::StartsWith(var_name, "Leo")) {
      continue;
    }

    local_vars_to_declare.push_back(var_name);
  }

  if (!local_vars_to_declare.empty()) {
    uint32_t detected_frame_size = 0;
    for (const auto& block : cfg.Blocks()) {
      for (const auto& inst : block.instructions) {
        if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
            inst.rt.has_value() && *inst.rt == Register::kSp && inst.rs == Register::kSp &&
            inst.immediate < 0) {
          detected_frame_size =
              std::max(detected_frame_size, static_cast<uint32_t>(-inst.immediate));
        }
      }
    }
    if (detected_frame_size == 0) {
      detected_frame_size = 64;
    }

    absl::flat_hash_set<std::string> double_vars;
    for (const auto& block : cfg.Blocks()) {
      for (const auto& inst : block.instructions) {
        if (inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
            inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
            inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
            inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
            inst.opcode == Opcode::kCvtSD || inst.opcode == Opcode::kTruncWD ||
            inst.opcode == Opcode::kCEqD || inst.opcode == Opcode::kCLtD ||
            inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kLdc1 ||
            inst.opcode == Opcode::kSdc1 || inst.opcode == Opcode::kDmfc1 ||
            inst.opcode == Opcode::kDmtc1) {
          if (inst.fd) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fd));
          if (inst.fs) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fs));
          if (inst.ft) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.ft));
        }
      }
    }

    auto is_fp_reg = [](std::string_view name) -> bool {
      if (name.size() >= 2 && name[0] == 'f' && std::isdigit(name[1])) {
        for (size_t i = 1; i < name.size(); ++i) {
          if (!std::isdigit(name[i])) return false;
        }
        return true;
      }
      return false;
    };

    auto new_body = std::make_unique<CompoundStatement>();
    for (const auto& var_name : local_vars_to_declare) {
      if (var_name == "sp") {
        new_body->AddStatement(
            CStatement::VariableDeclaration(CType::U8(), "sp", nullptr, detected_frame_size));
      } else if (is_fp_reg(var_name)) {
        if (double_vars.contains(var_name)) {
          new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name));
        } else {
          new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name));
        }
      } else {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::S32(), var_name));
      }
    }
    // Move existing statements
    std::vector<std::unique_ptr<CStatement>> existing_stmts;
    for (const auto& stmt : body->Statements()) {
      new_body->AddStatement(stmt->Clone());
    }
    body = std::move(new_body);
  }

  return FunctionDeclaration(return_type, options.function_name, std::move(parameters),
                             std::move(body), options.is_static);
}

}  // namespace rom_nom_nom
