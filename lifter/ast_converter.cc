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
                   bool returns_v0)
      : cfg_(cfg),
        symbol_index_(symbol_index),
        split_config_(split_config),
        function_parameter_counts_(function_parameter_counts),
        returns_v0_(returns_v0) {
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
        if (has_branch_likely && !region.invert_condition) {
          const auto& delay_inst = cond_block->instructions.back();
          auto delay_stmts =
              ExpressionBuilder::LiftInstructions(absl::MakeSpan(&delay_inst, 1), symbol_index_,
                                                  split_config_, function_parameter_counts_);
          for (auto& s : delay_stmts) {
            auto cs = AstConverter::ConvertStatement(s);
            if (cs != nullptr) {
              then_body->AddStatement(std::move(cs));
            }
          }
        }

        if (!region.children.empty() && region.children[0]) {
          ConvertRegion(*region.children[0], then_body.get());
        }
        target_block->AddStatement(CStatement::If(std::move(cond), std::move(then_body)));
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
        if (has_branch_likely) {
          const auto& delay_inst = cond_block->instructions.back();
          auto delay_stmts =
              ExpressionBuilder::LiftInstructions(absl::MakeSpan(&delay_inst, 1), symbol_index_,
                                                  split_config_, function_parameter_counts_);
          for (auto& s : delay_stmts) {
            auto cs = AstConverter::ConvertStatement(s);
            if (cs != nullptr) {
              then_body->AddStatement(std::move(cs));
            }
          }
        }

        auto else_body = std::make_unique<CompoundStatement>();
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
          auto loop_body = std::make_unique<CompoundStatement>();
          if (!region.children.empty() && region.children[0]) {
            ConvertRegion(*region.children[0], loop_body.get());
          }
          uint32_t cond_block_id =
              region.condition_block_id != 0 ? region.condition_block_id : region.loop_header;
          const auto* cond_block = cfg_.GetBlock(cond_block_id);
          auto cond = cond_block != nullptr
                          ? AstConverter::ExtractBranchCondition(*cond_block, false)
                          : CExpression::Integer(1);
          target_block->AddStatement(CStatement::DoWhile(std::move(loop_body), std::move(cond)));
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

    std::vector<LiftedStatement> lifted_stmts;
    if (omit_last_instruction && block->instructions.size() > 1) {
      absl::Span<const Instruction> inst_span(block->instructions.data(),
                                              block->instructions.size() - 1);
      lifted_stmts = ExpressionBuilder::LiftInstructions(inst_span, symbol_index_, split_config_,
                                                         function_parameter_counts_);
    } else {
      lifted_stmts = ExpressionBuilder::LiftBlock(*block, symbol_index_, split_config_,
                                                  function_parameter_counts_);
    }
    for (auto& lifted_stmt : lifted_stmts) {
      if (lifted_stmt.kind == StatementKind::kReturn) {
        if (returns_v0_) {
          if (!lifted_stmt.expression) {
            lifted_stmt.expression = LiftedExpression::Variable("v0");
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
  bool returns_v0_ = false;
  absl::flat_hash_set<uint32_t> goto_targets_;
  absl::flat_hash_set<uint32_t> emitted_labels_;
  absl::flat_hash_set<uint32_t> emitted_blocks_;
};

}  // namespace

FunctionDeclaration AstConverter::Convert(const ControlFlowGraph& cfg,
                                          const StructuredRegion& root_region,
                                          const SymbolIndex* symbol_index,
                                          const AstConverterOptions& options) {
  auto body = std::make_unique<CompoundStatement>();

  bool returns_v0 = false;
  if (options.return_type.has_value()) {
    returns_v0 = (options.return_type->ToString() != "void");
  } else {
    returns_v0 = ExpressionBuilder::DetermineReturnsV0(cfg);
  }

  ConverterContext ctx(cfg, symbol_index, root_region, options.split_config,
                       options.function_parameter_counts, returns_v0);
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
    if (parameter_count < 0) {
      std::vector<Instruction> all_instructions;
      for (const auto& block : cfg.Blocks()) {
        all_instructions.insert(all_instructions.end(), block.instructions.begin(),
                                block.instructions.end());
      }
      if (!all_instructions.empty()) {
        parameter_count = ExpressionBuilder::DetermineParameterCount(all_instructions);
      }
    }
    if (parameter_count >= 0) {
      for (int param_index = 0; param_index < parameter_count; ++param_index) {
        parameters.push_back(
            CParameter{.type = CType::S32(), .name = absl::StrFormat("arg%d", param_index)});
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
  } else if (returns_v0 || HasReturnValue(*body)) {
    return_type = CType::S32();
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
        var_name == "u8") {
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

    auto new_body = std::make_unique<CompoundStatement>();
    for (const auto& var_name : local_vars_to_declare) {
      if (var_name == "sp") {
        new_body->AddStatement(
            CStatement::VariableDeclaration(CType::U8(), "sp", nullptr, detected_frame_size));
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
