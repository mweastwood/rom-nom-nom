#include "lifter/ast_converter.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
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

// Recursively collects all assigned variable names in a CStatement.
void CollectAssignedVariables(const CStatement& stmt, std::vector<std::string>* assigned_vars,
                              absl::flat_hash_set<std::string>* seen) {
  if (stmt.Kind() == CStatementKind::kExpressionStatement) {
    const auto& expr_stmt = static_cast<const ExpressionStatement&>(stmt);
    if (expr_stmt.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& assign = static_cast<const AssignmentExpression&>(expr_stmt.Expression());
      if (assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
        const auto& id = static_cast<const IdentifierExpression&>(assign.Lhs());
        if (seen->insert(id.Name()).second) {
          assigned_vars->push_back(id.Name());
        }
      }
    }
  } else if (stmt.Kind() == CStatementKind::kCompoundStatement) {
    const auto& comp = static_cast<const CompoundStatement&>(stmt);
    for (const auto& child : comp.Statements()) {
      CollectAssignedVariables(*child, assigned_vars, seen);
    }
  } else if (stmt.Kind() == CStatementKind::kIfStatement) {
    const auto& if_stmt = static_cast<const IfStatement&>(stmt);
    CollectAssignedVariables(if_stmt.ThenBranch(), assigned_vars, seen);
    if (if_stmt.ElseBranch() != nullptr) {
      CollectAssignedVariables(*if_stmt.ElseBranch(), assigned_vars, seen);
    }
  } else if (stmt.Kind() == CStatementKind::kWhileStatement) {
    const auto& while_stmt = static_cast<const WhileStatement&>(stmt);
    CollectAssignedVariables(while_stmt.Body(), assigned_vars, seen);
  } else if (stmt.Kind() == CStatementKind::kDoWhileStatement) {
    const auto& dowhile_stmt = static_cast<const DoWhileStatement&>(stmt);
    CollectAssignedVariables(dowhile_stmt.Body(), assigned_vars, seen);
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
      auto lhs = CExpression::Identifier(stmt.destination_variable);
      auto rhs = stmt.expression ? ConvertExpression(*stmt.expression) : CExpression::Integer(0);
      return CStatement::Expression(CExpression::Assignment("=", std::move(lhs), std::move(rhs)));
    }

    case StatementKind::kStore: {
      auto target = stmt.destination_address ? ConvertExpression(*stmt.destination_address)
                                             : CExpression::Identifier("addr");
      auto deref = CExpression::Unary("*", std::move(target));
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
  ConverterContext(const ControlFlowGraph& cfg, const SymbolIndex* symbol_index)
      : cfg_(cfg), symbol_index_(symbol_index) {}

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
        EmitBlockStatements(region.condition_block_id, target_block);
        const auto* cond_block = cfg_.GetBlock(region.condition_block_id);
        auto cond = cond_block != nullptr
                        ? AstConverter::ExtractBranchCondition(*cond_block, region.invert_condition)
                        : CExpression::Integer(1);

        auto then_body = std::make_unique<CompoundStatement>();
        if (!region.children.empty() && region.children[0]) {
          ConvertRegion(*region.children[0], then_body.get());
        }
        target_block->AddStatement(CStatement::If(std::move(cond), std::move(then_body)));
        break;
      }

      case RegionType::kIfThenElse: {
        EmitBlockStatements(region.condition_block_id, target_block);
        const auto* cond_block = cfg_.GetBlock(region.condition_block_id);
        auto cond = cond_block != nullptr
                        ? AstConverter::ExtractBranchCondition(*cond_block, region.invert_condition)
                        : CExpression::Integer(1);

        auto then_body = std::make_unique<CompoundStatement>();
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
          const auto* header_block = cfg_.GetBlock(region.loop_header);
          auto cond = header_block != nullptr
                          ? AstConverter::ExtractBranchCondition(*header_block, false)
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
    }
  }

 private:
  void EmitBlockStatements(uint32_t block_id, CompoundStatement* target_block) {
    if (emitted_blocks_.contains(block_id)) {
      return;
    }
    emitted_blocks_.insert(block_id);

    const auto* block = cfg_.GetBlock(block_id);
    if (block == nullptr) {
      return;
    }

    auto lifted_stmts = ExpressionBuilder::LiftBlock(*block, symbol_index_);
    for (const auto& lifted_stmt : lifted_stmts) {
      auto c_stmt = AstConverter::ConvertStatement(lifted_stmt);
      if (c_stmt != nullptr) {
        target_block->AddStatement(std::move(c_stmt));
      }
    }
  }

  const ControlFlowGraph& cfg_;
  const SymbolIndex* symbol_index_;
  absl::flat_hash_set<uint32_t> emitted_blocks_;
};

}  // namespace

FunctionDeclaration AstConverter::Convert(const ControlFlowGraph& cfg,
                                          const StructuredRegion& root_region,
                                          const SymbolIndex* symbol_index,
                                          const AstConverterOptions& options) {
  auto body = std::make_unique<CompoundStatement>();

  ConverterContext ctx(cfg, symbol_index);
  ctx.ConvertRegion(root_region, body.get());

  // Determine parameters
  std::vector<CParameter> parameters = options.parameters;
  if (parameters.empty()) {
    // Check if arg0, arg1, arg2, arg3 are referenced in the function body
    std::string body_text = body->ToString(0);
    if (body_text.find("arg0") != std::string::npos) {
      parameters.push_back(CParameter{.type = CType::S32(), .name = "arg0"});
    }
    if (body_text.find("arg1") != std::string::npos) {
      parameters.push_back(CParameter{.type = CType::S32(), .name = "arg1"});
    }
    if (body_text.find("arg2") != std::string::npos) {
      parameters.push_back(CParameter{.type = CType::S32(), .name = "arg2"});
    }
    if (body_text.find("arg3") != std::string::npos) {
      parameters.push_back(CParameter{.type = CType::S32(), .name = "arg3"});
    }
  }

  // Determine return type
  CType return_type = CType::Void();
  if (options.return_type.has_value()) {
    return_type = *options.return_type;
  } else if (HasReturnValue(*body)) {
    return_type = CType::S32();
  }

  // Collect assigned variables and declare them at the top of the function
  std::vector<std::string> assigned_vars;
  absl::flat_hash_set<std::string> seen;
  for (const auto& param : parameters) {
    seen.insert(param.name);
  }
  CollectAssignedVariables(*body, &assigned_vars, &seen);

  if (!assigned_vars.empty()) {
    auto new_body = std::make_unique<CompoundStatement>();
    for (const auto& var_name : assigned_vars) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::S32(), var_name));
    }
    // Move existing statements
    std::vector<std::unique_ptr<CStatement>> existing_stmts;
    // Swap statements into new_body
    for (const auto& stmt : body->Statements()) {
      new_body->AddStatement(stmt->Clone());
    }
    body = std::move(new_body);
  }

  return FunctionDeclaration(return_type, options.function_name, std::move(parameters),
                             std::move(body), options.is_static);
}

}  // namespace rom_nom_nom
