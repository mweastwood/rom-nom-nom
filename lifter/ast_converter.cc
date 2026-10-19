#include "lifter/ast_converter.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/branch_condition_builder.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/expression_builder.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

absl::flat_hash_set<std::string> AstConverter::IdentifyDoubleVariables(
    const ControlFlowGraph& cfg) {
  return AbiAnalyzer::IdentifyDoubleVariables(cfg);
}

bool AstConverter::IsFpParameterDouble(const ControlFlowGraph& cfg, FpRegister reg) {
  return AbiAnalyzer::IsFpParameterDouble(cfg, reg);
}

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
    case CStatementKind::kForStatement: {
      const auto& for_stmt = static_cast<const ForStatement&>(stmt);
      if (for_stmt.Init() != nullptr) {
        CollectUsedVariables(*for_stmt.Init(), vars, seen);
      }
      if (for_stmt.Condition() != nullptr) {
        CollectVariablesInExpression(*for_stmt.Condition(), vars, seen);
      }
      if (for_stmt.Step() != nullptr) {
        CollectVariablesInExpression(*for_stmt.Step(), vars, seen);
      }
      CollectUsedVariables(for_stmt.Body(), vars, seen);
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

std::unique_ptr<CExpression> SubstituteIdentifier(const CExpression& expr, std::string_view target,
                                                  const CExpression& replacement) {
  if (expr.Kind() == CExpressionKind::kIdentifier) {
    if (static_cast<const IdentifierExpression&>(expr).Name() == target) {
      return replacement.Clone();
    }
    return expr.Clone();
  }
  if (expr.Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(expr);
    return CExpression::Binary(bin.Op(), SubstituteIdentifier(bin.Lhs(), target, replacement),
                               SubstituteIdentifier(bin.Rhs(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& un = static_cast<const UnaryExpression&>(expr);
    if (un.Op().find('&') != std::string::npos &&
        replacement.Kind() != CExpressionKind::kIdentifier) {
      if (un.Operand().Kind() == CExpressionKind::kIdentifier &&
          static_cast<const IdentifierExpression&>(un.Operand()).Name() == target) {
        return expr.Clone();
      }
    }
    return CExpression::Unary(un.Op(), SubstituteIdentifier(un.Operand(), target, replacement),
                              un.IsPrefix());
  }
  if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
    const auto& assign = static_cast<const AssignmentExpression&>(expr);
    std::unique_ptr<CExpression> new_lhs;
    if (assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
      if (replacement.Kind() == CExpressionKind::kIdentifier) {
        new_lhs = SubstituteIdentifier(assign.Lhs(), target, replacement);
      } else {
        // Direct identifier on LHS is a definition (write), NOT a read.
        // Never replace an LHS identifier with an rvalue expression!
        new_lhs = assign.Lhs().Clone();
      }
    } else {
      new_lhs = SubstituteIdentifier(assign.Lhs(), target, replacement);
    }
    return CExpression::Assignment(assign.Op(), std::move(new_lhs),
                                   SubstituteIdentifier(assign.Rhs(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(expr);
    return CExpression::Cast(cast.TargetType(),
                             SubstituteIdentifier(cast.Operand(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kArrayIndexExpression) {
    const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
    return CExpression::ArrayIndex(SubstituteIdentifier(arr.Array(), target, replacement),
                                   SubstituteIdentifier(arr.Index(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kMemberAccessExpression) {
    const auto& mem = static_cast<const MemberAccessExpression&>(expr);
    return CExpression::MemberAccess(SubstituteIdentifier(mem.Object(), target, replacement),
                                     mem.MemberName(), mem.IsArrow());
  }
  if (expr.Kind() == CExpressionKind::kTernaryExpression) {
    const auto& tern = static_cast<const TernaryExpression&>(expr);
    return CExpression::Ternary(SubstituteIdentifier(tern.Condition(), target, replacement),
                                SubstituteIdentifier(tern.TrueExpression(), target, replacement),
                                SubstituteIdentifier(tern.FalseExpression(), target, replacement));
  }
  if (expr.Kind() == CExpressionKind::kCallExpression) {
    const auto& call = static_cast<const CallExpression&>(expr);
    std::vector<std::unique_ptr<CExpression>> new_args;
    for (const auto& arg : call.Arguments()) {
      new_args.push_back(SubstituteIdentifier(*arg, target, replacement));
    }
    return CExpression::Call(SubstituteIdentifier(call.Callee(), target, replacement),
                             std::move(new_args));
  }
  return expr.Clone();
}

}  // namespace

std::unique_ptr<CExpression> AstConverter::ConvertExpression(
    const LiftedExpression& expr, std::string_view current_function_name) {
  switch (expr.kind) {
    case ExpressionKind::kIntegerLiteral:
      return CExpression::Integer(expr.int_val, expr.is_hex);

    case ExpressionKind::kVariable:
    case ExpressionKind::kGlobalRef:
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kUnaryOp:
      if (!expr.args.empty() && expr.args[0]) {
        if (expr.op == "(u32)") {
          return CExpression::Cast(CType::U32(),
                                   ConvertExpression(*expr.args[0], current_function_name));
        }
        if (expr.op == "(s32)") {
          return CExpression::Cast(CType::S32(),
                                   ConvertExpression(*expr.args[0], current_function_name));
        }
        if (expr.op == "(u8*)" || expr.op == "(u8 *)") {
          return CExpression::Cast(CType::Named("u8").MakePointer(),
                                   ConvertExpression(*expr.args[0], current_function_name));
        }
        return CExpression::Unary(expr.op, ConvertExpression(*expr.args[0], current_function_name));
      }
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kBinaryOp:
      if (expr.args.size() >= 2 && expr.args[0] && expr.args[1]) {
        return CExpression::Binary(expr.op, ConvertExpression(*expr.args[0], current_function_name),
                                   ConvertExpression(*expr.args[1], current_function_name));
      }
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kMemoryLoad:
      if (!expr.args.empty() && expr.args[0]) {
        auto cast = CExpression::Cast(CType::Named(expr.name).MakePointer(),
                                      ConvertExpression(*expr.args[0], current_function_name));
        return CExpression::Unary("*", std::move(cast));
      }
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kFunctionCall: {
      std::string target_func = expr.name;
      std::vector<std::unique_ptr<CExpression>> call_args;
      call_args.reserve(expr.args.size());
      for (const auto& arg : expr.args) {
        if (arg) {
          call_args.push_back(ConvertExpression(*arg, current_function_name));
        }
      }
      return CExpression::Call(CExpression::Identifier(target_func), std::move(call_args));
    }
  }
  return CExpression::Identifier("unk");
}

std::unique_ptr<CStatement> AstConverter::ConvertStatement(const LiftedStatement& stmt,
                                                           std::string_view current_function_name) {
  switch (stmt.kind) {
    case StatementKind::kAssignment: {
      if (stmt.destination_variable.empty() || stmt.destination_variable == "0") {
        if (stmt.expression) {
          return CStatement::Expression(ConvertExpression(*stmt.expression, current_function_name));
        }
        return nullptr;
      }
      auto lhs = CExpression::Identifier(stmt.destination_variable);
      auto rhs = stmt.expression ? ConvertExpression(*stmt.expression, current_function_name)
                                 : CExpression::Integer(0);
      return CStatement::Expression(CExpression::Assignment("=", std::move(lhs), std::move(rhs)));
    }

    case StatementKind::kStore: {
      auto target = stmt.destination_address
                        ? ConvertExpression(*stmt.destination_address, current_function_name)
                        : CExpression::Identifier("addr");
      std::string type_name = stmt.store_type.empty() ? "s32" : stmt.store_type;
      auto cast = CExpression::Cast(CType::Named(type_name).MakePointer(), std::move(target));
      auto deref = CExpression::Unary("*", std::move(cast));
      auto val = stmt.expression ? ConvertExpression(*stmt.expression, current_function_name)
                                 : CExpression::Integer(0);
      return CStatement::Expression(CExpression::Assignment("=", std::move(deref), std::move(val)));
    }

    case StatementKind::kCall: {
      if (stmt.expression) {
        return CStatement::Expression(ConvertExpression(*stmt.expression, current_function_name));
      }
      return nullptr;
    }

    case StatementKind::kReturn: {
      if (stmt.expression) {
        return CStatement::Return(ConvertExpression(*stmt.expression, current_function_name));
      }
      return CStatement::Return();
    }

    case StatementKind::kUnhandled: {
      if (stmt.expression) {
        return CStatement::Comment(stmt.expression->ToString());
      }
      return CStatement::Comment("UNHANDLED_INSTRUCTION");
    }
  }
  return nullptr;
}

std::unique_ptr<CExpression> AstConverter::ExtractBranchCondition(const BasicBlock& block,
                                                                  bool invert_condition) {
  return BranchConditionBuilder::ExtractBranchCondition(block, invert_condition);
}

namespace {

class ConverterContext {
 public:
  ConverterContext(const ControlFlowGraph& cfg, const SymbolIndex* symbol_index,
                   const StructuredRegion& root_region, const GameConfig* game_config,
                   const absl::flat_hash_map<std::string, int>* function_parameter_counts,
                   const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
                   bool returns_v0, bool returns_f0,
                   const absl::flat_hash_set<std::string>* functions_returning_v0 = nullptr,
                   const absl::flat_hash_set<std::string>* functions_returning_f0 = nullptr,
                   std::string function_name = "")
      : cfg_(cfg),
        symbol_index_(symbol_index),
        game_config_(game_config),
        function_parameter_counts_(function_parameter_counts),
        function_fp_parameter_counts_(function_fp_parameter_counts),
        returns_v0_(returns_v0),
        returns_f0_(returns_f0),
        functions_returning_v0_(functions_returning_v0),
        functions_returning_f0_(functions_returning_f0),
        function_name_(std::move(function_name)),
        double_vars_(AstConverter::IdentifyDoubleVariables(cfg_)),
        frame_analysis_(StackFrameAnalyzer::Analyze(cfg_)),
        float_parameters_in_general_registers_(
            AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg_)) {
    CollectGotoTargets(root_region);
    for (const auto& jt : cfg_.JumpTables()) {
      for (uint32_t vram : jt.setup_instruction_vrams) {
        setup_instruction_vrams_.insert(vram);
      }
      if (jt.bounds_branch_vram != 0) {
        const auto* bounds_block = cfg_.FindBlockByVram(jt.bounds_branch_vram);
        if (bounds_block != nullptr) {
          const auto* term = bounds_block->Terminator();
          if (term != nullptr && term->IsBranchLikely()) {
            omit_last_instruction_blocks_.insert(bounds_block->id);
          }
        }
      }
    }
    ComputeBlockEntryStates();
    PreScanPeeledLoopRotations(root_region);
    call_return_info_ = ExpressionBuilder::ComputeCallReturnInfo(cfg_, returns_v0_, returns_f0_);
  }

  const absl::flat_hash_set<uint32_t>& GotoTargets() const { return goto_targets_; }
  const absl::flat_hash_set<uint32_t>& EmittedLabels() const { return emitted_labels_; }
  bool SpAddressTaken() const { return frame_analysis_.sp_address_taken; }
  uint32_t FrameSize() const { return frame_analysis_.frame_size; }
  bool HasFramePointer() const { return frame_analysis_.has_frame_pointer; }
  bool HasCalls() const { return frame_analysis_.has_calls; }
  uint32_t MinSavedRegOffset() const { return frame_analysis_.prologue.min_saved_reg_offset; }
  uint32_t SpBufferSize() const { return frame_analysis_.sp_buffer_size; }
  const absl::flat_hash_set<std::string>& DoubleVars() const { return double_vars_; }

  FunctionLiftContext BuildLiftContext(
      const std::array<std::optional<uint32_t>, 32>* initial_fpr_const_bits = nullptr,
      const RegisterTracker* initial_tracker = nullptr) const {
    FunctionLiftContext ctx;
    ctx.symbol_index = symbol_index_;
    ctx.game_config = game_config_;
    ctx.function_parameter_counts = function_parameter_counts_;
    ctx.function_fp_parameter_counts = function_fp_parameter_counts_;
    ctx.frame_size = frame_analysis_.frame_size;
    ctx.functions_returning_v0 = functions_returning_v0_;
    ctx.functions_returning_f0 = functions_returning_f0_;
    ctx.has_frame_pointer = frame_analysis_.has_frame_pointer;
    ctx.has_calls = frame_analysis_.has_calls;
    ctx.sp_address_taken = frame_analysis_.sp_address_taken;
    ctx.saved_reg_offsets = &frame_analysis_.prologue.saved_reg_offsets;
    ctx.saved_fpr_offsets = &frame_analysis_.prologue.saved_fpr_offsets;
    ctx.initial_fpr_const_bits = initial_fpr_const_bits;
    ctx.initial_tracker = initial_tracker;
    ctx.call_return_info = &call_return_info_;
    ctx.function_stack_stores = &frame_analysis_.stack_store_offsets;
    ctx.float_parameters_in_general_registers = &float_parameters_in_general_registers_;
    ctx.outgoing_args_size = frame_analysis_.outgoing_args_size;
    ctx.sp_buffer_size = frame_analysis_.sp_buffer_size;
    return ctx;
  }

  std::vector<LiftedStatement> LiftInstructionsHelper(
      absl::Span<const Instruction> instructions) const {
    return ExpressionBuilder::LiftInstructions(instructions, BuildLiftContext(),
                                               /*omit_last_instruction=*/false);
  }

  void EmitFloatToUintRegion(const StructuredRegion& region, CompoundStatement* target_block) {
    const auto& info = *region.float_to_uint_info;
    const auto* cond_block = cfg_.GetBlock(region.condition_block_id);
    if (cond_block != nullptr) {
      emitted_blocks_.insert(region.condition_block_id);
      if (goto_targets_.contains(region.condition_block_id)) {
        target_block->AddStatement(
            CStatement::Label(absl::StrFormat("block_%d", region.condition_block_id)));
        emitted_labels_.insert(region.condition_block_id);
      }

      absl::flat_hash_set<size_t> diamond_instruction_indices;
      int comparison_instruction_index = -1;
      for (size_t i = 0; i < cond_block->instructions.size(); ++i) {
        const auto& inst = cond_block->instructions[i];
        if (inst.opcode == Opcode::kCLeS || inst.opcode == Opcode::kCLtS ||
            inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kCLtD) {
          comparison_instruction_index = static_cast<int>(i);
          diamond_instruction_indices.insert(i);
          break;
        }
      }

      if (comparison_instruction_index >= 0) {
        for (int i = comparison_instruction_index - 1; i >= 0; --i) {
          const auto& inst = cond_block->instructions[i];
          if (inst.IsNop()) continue;
          if (inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kDmtc1) {
            diamond_instruction_indices.insert(static_cast<size_t>(i));
            Register reg = inst.rt.value_or(Register::kZero);
            for (int j = i - 1; j >= 0; --j) {
              const auto& prev = cond_block->instructions[j];
              if (prev.IsNop()) continue;
              if (prev.opcode == Opcode::kLui && prev.rt == reg) {
                uint32_t imm = prev.immediate & 0xFFFF;
                if (imm == 0x4F00 || imm == 0x41E0) {
                  diamond_instruction_indices.insert(static_cast<size_t>(j));
                }
                break;
              }
            }
            break;
          } else if ((inst.opcode == Opcode::kLdc1 || inst.opcode == Opcode::kLwc1) &&
                     inst.ft.has_value() && *inst.ft == FpRegister::kF0) {
            diamond_instruction_indices.insert(static_cast<size_t>(i));
            Register reg = inst.rs.value_or(Register::kZero);
            for (int j = i - 1; j >= 0; --j) {
              const auto& prev = cond_block->instructions[j];
              if (prev.IsNop()) continue;
              if (prev.opcode == Opcode::kLui && prev.rt == reg) {
                diamond_instruction_indices.insert(static_cast<size_t>(j));
                break;
              }
            }
            break;
          }
        }
      }

      std::vector<Instruction> non_diamond_instructions;
      for (size_t i = 0; i < cond_block->instructions.size(); ++i) {
        const auto& inst = cond_block->instructions[i];
        if (inst.IsNop()) continue;
        if (inst.IsBranch() || inst.IsJump()) break;
        if (i + 1 == cond_block->instructions.size() && cond_block->instructions.size() >= 2 &&
            (cond_block->instructions[i - 1].IsBranch() ||
             cond_block->instructions[i - 1].IsJump())) {
          break;
        }

        if (diamond_instruction_indices.contains(i)) {
          continue;
        }

        non_diamond_instructions.push_back(inst);
      }

      if (!non_diamond_instructions.empty()) {
        auto pre_stmts = LiftInstructionsHelper(absl::MakeSpan(non_diamond_instructions));
        for (auto& s : pre_stmts) {
          auto cs = AstConverter::ConvertStatement(s, function_name_);
          if (cs != nullptr) {
            target_block->AddStatement(std::move(cs));
          }
        }
      }
    }

    std::function<void(const StructuredRegion&)> mark_blocks_emitted =
        [&](const StructuredRegion& r) {
          if (r.type == RegionType::kBlock) {
            emitted_blocks_.insert(r.block_id);
          }
          for (const auto& child : r.children) {
            if (child) mark_blocks_emitted(*child);
          }
          for (const auto& c : r.cases) {
            if (c.body) mark_blocks_emitted(*c.body);
          }
        };
    for (const auto& child : region.children) {
      if (child) mark_blocks_emitted(*child);
    }

    if (cond_block != nullptr && cond_block->instructions.size() >= 2 &&
        cond_block->instructions[cond_block->instructions.size() - 2].IsBranch()) {
      const auto& delay_inst = cond_block->instructions.back();
      if (!delay_inst.IsNop() && delay_inst.opcode != Opcode::kMtc1 &&
          delay_inst.opcode != Opcode::kDmtc1 && delay_inst.opcode != Opcode::kCLeS &&
          delay_inst.opcode != Opcode::kCLtS && delay_inst.opcode != Opcode::kCLeD &&
          delay_inst.opcode != Opcode::kCLtD && delay_inst.opcode != Opcode::kSubS &&
          delay_inst.opcode != Opcode::kSubD && delay_inst.opcode != Opcode::kLdc1 &&
          delay_inst.opcode != Opcode::kLwc1) {
        auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
        for (auto& s : delay_stmts) {
          auto cs = AstConverter::ConvertStatement(s, function_name_);
          if (cs != nullptr) {
            target_block->AddStatement(std::move(cs));
          }
        }
      }
    }

    std::string dest_var = ExpressionBuilder::RegisterVarName(info.destination_integer_register);
    std::string src_fp_var =
        ExpressionBuilder::FpRegisterVarName(info.source_float_register, info.is_double);

    auto cast_expr = CExpression::Cast(CType::U32(), CExpression::Identifier(src_fp_var));
    auto assign_stmt = CStatement::Expression(
        CExpression::Assignment("=", CExpression::Identifier(dest_var), std::move(cast_expr)));
    target_block->AddStatement(std::move(assign_stmt));
  }

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
        bool decouple_delay = ShouldDecoupleBranchConditionDelaySlot(region.condition_block_id);

        EmitBlockStatements(region.condition_block_id, target_block,
                            /*omit_last_instruction=*/has_branch_likely || decouple_delay);
        auto cond = cond_block != nullptr
                        ? AstConverter::ExtractBranchCondition(*cond_block, region.invert_condition)
                        : CExpression::Integer(1);

        if (!region.condition_op.empty()) {
          for (size_t ci = 0; ci < region.additional_condition_blocks.size(); ++ci) {
            uint32_t add_blk_id = region.additional_condition_blocks[ci];
            bool add_inv = (ci < region.additional_invert_conditions.size())
                               ? region.additional_invert_conditions[ci]
                               : false;
            const auto* add_block = cfg_.GetBlock(add_blk_id);
            if (add_block != nullptr) {
              omit_last_instruction_blocks_.insert(add_blk_id);
              EmitBlockStatements(add_blk_id, target_block, /*omit_last_instruction=*/true);
              auto next_cond = AstConverter::ExtractBranchCondition(*add_block, add_inv);
              cond =
                  CExpression::Binary(region.condition_op, std::move(cond), std::move(next_cond));
            }
          }
        }

        auto then_body = std::make_unique<CompoundStatement>();
        auto else_body = std::make_unique<CompoundStatement>();
        if (has_branch_likely && cond_block != nullptr) {
          const auto& delay_inst = cond_block->instructions.back();
          auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
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
        } else if (decouple_delay && cond_block != nullptr) {
          const auto& delay_inst = cond_block->instructions.back();
          auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
          for (const auto& s : delay_stmts) {
            bool is_arg_assignment = (s.kind == StatementKind::kAssignment &&
                                      absl::StartsWith(s.destination_variable, "arg"));
            auto cs1 = AstConverter::ConvertStatement(s);
            if (cs1 != nullptr) {
              then_body->AddStatement(std::move(cs1));
            }
            if (!is_arg_assignment) {
              auto cs2 = AstConverter::ConvertStatement(s);
              if (cs2 != nullptr) {
                else_body->AddStatement(std::move(cs2));
              }
            }
          }
        }

        if (!region.children.empty() && region.children[0]) {
          ConvertRegion(*region.children[0], then_body.get());
        }

        bool emitted_for = false;
        if (else_body->Statements().empty() && then_body->Statements().size() == 1 &&
            then_body->Statements()[0]->Kind() == CStatementKind::kDoWhileStatement) {
          const auto* do_while =
              static_cast<const DoWhileStatement*>(then_body->Statements()[0].get());
          const auto& loop_cond = do_while->Condition();
          if (loop_cond.Kind() == CExpressionKind::kBinaryExpression) {
            const auto& bin_cond = static_cast<const BinaryExpression&>(loop_cond);
            if (bin_cond.Op() == "<" && bin_cond.Lhs().Kind() == CExpressionKind::kIdentifier) {
              std::string loop_var =
                  static_cast<const IdentifierExpression&>(bin_cond.Lhs()).Name();
              const auto& loop_body_stmt = do_while->Body();
              if (loop_body_stmt.Kind() == CStatementKind::kCompoundStatement) {
                const auto& loop_compound = static_cast<const CompoundStatement&>(loop_body_stmt);
                int inc_idx = -1;
                for (int i = static_cast<int>(loop_compound.Statements().size()) - 1; i >= 0; --i) {
                  std::string stmt_str = loop_compound.Statements()[i]->ToString();
                  if (absl::StrContains(stmt_str, loop_var) &&
                      (absl::StrContains(stmt_str, "+ 1") || absl::StrContains(stmt_str, "+= 1") ||
                       absl::StrContains(stmt_str, "++"))) {
                    inc_idx = i;
                    break;
                  }
                }
                bool has_init_zero = false;
                for (const auto& s : target_block->Statements()) {
                  std::string s_str = s->ToString();
                  if (absl::StartsWith(s_str, loop_var + " = 0;") ||
                      absl::StrContains(s_str, loop_var + " = 0;")) {
                    has_init_zero = true;
                    break;
                  }
                }

                if (inc_idx >= 0 && has_init_zero) {
                  target_block->RemoveStatementIf([&](const CStatement& s) {
                    std::string s_str = s.ToString();
                    return absl::StartsWith(s_str, loop_var + " = 0;") ||
                           absl::StrContains(s_str, loop_var + " = 0;");
                  });
                  auto for_body = std::make_unique<CompoundStatement>();
                  std::string loop_cond_str = loop_cond.ToString();
                  for (size_t i = 0; i < loop_compound.Statements().size(); ++i) {
                    if (static_cast<int>(i) == inc_idx) continue;
                    std::string stmt_str = loop_compound.Statements()[i]->ToString();
                    if (absl::StrContains(stmt_str, loop_cond_str) &&
                        (absl::StartsWith(stmt_str, "v0 = ") ||
                         absl::StartsWith(stmt_str, "v1 = ") ||
                         absl::StartsWith(stmt_str, "at = "))) {
                      continue;
                    }
                    for_body->AddStatement(loop_compound.Statements()[i]->Clone());
                  }
                  auto for_init = CStatement::Expression(CExpression::Assignment(
                      "=", CExpression::Identifier(loop_var), CExpression::Integer(0)));
                  auto for_step = CExpression::Unary("++", CExpression::Identifier(loop_var),
                                                     /*is_prefix=*/false);
                  target_block->AddStatement(CStatement::For(std::move(for_init), loop_cond.Clone(),
                                                             std::move(for_step),
                                                             std::move(for_body)));
                  emitted_for = true;
                }
              }
            }
          }
        }

        if (!emitted_for) {
          if (!else_body->Statements().empty()) {
            target_block->AddStatement(
                CStatement::If(std::move(cond), std::move(then_body), std::move(else_body)));
          } else {
            target_block->AddStatement(CStatement::If(std::move(cond), std::move(then_body)));
          }
        }
        break;
      }

      case RegionType::kIfThenElse: {
        if (region.float_to_uint_info.has_value()) {
          EmitFloatToUintRegion(region, target_block);
          break;
        }

        const auto* cond_block = cfg_.GetBlock(region.condition_block_id);
        bool has_branch_likely = false;
        if (cond_block != nullptr) {
          const auto* term = cond_block->Terminator();
          if (term != nullptr && term->IsBranchLikely() && cond_block->instructions.size() >= 2) {
            has_branch_likely = true;
          }
        }
        bool decouple_delay = ShouldDecoupleBranchConditionDelaySlot(region.condition_block_id);

        EmitBlockStatements(region.condition_block_id, target_block,
                            /*omit_last_instruction=*/has_branch_likely || decouple_delay);
        auto cond = cond_block != nullptr
                        ? AstConverter::ExtractBranchCondition(*cond_block, region.invert_condition)
                        : CExpression::Integer(1);

        if (!region.condition_op.empty()) {
          for (size_t ci = 0; ci < region.additional_condition_blocks.size(); ++ci) {
            uint32_t add_blk_id = region.additional_condition_blocks[ci];
            bool add_inv = (ci < region.additional_invert_conditions.size())
                               ? region.additional_invert_conditions[ci]
                               : false;
            const auto* add_block = cfg_.GetBlock(add_blk_id);
            if (add_block != nullptr) {
              omit_last_instruction_blocks_.insert(add_blk_id);
              EmitBlockStatements(add_blk_id, target_block, /*omit_last_instruction=*/true);
              auto next_cond = AstConverter::ExtractBranchCondition(*add_block, add_inv);
              cond =
                  CExpression::Binary(region.condition_op, std::move(cond), std::move(next_cond));
            }
          }
        }

        auto then_body = std::make_unique<CompoundStatement>();
        auto else_body = std::make_unique<CompoundStatement>();
        if (has_branch_likely && cond_block != nullptr) {
          const auto& delay_inst = cond_block->instructions.back();
          auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
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
        } else if (decouple_delay && cond_block != nullptr) {
          const auto& delay_inst = cond_block->instructions.back();
          auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
          for (const auto& s : delay_stmts) {
            auto cs1 = AstConverter::ConvertStatement(s);
            if (cs1 != nullptr) {
              then_body->AddStatement(std::move(cs1));
            }
            auto cs2 = AstConverter::ConvertStatement(s);
            if (cs2 != nullptr) {
              else_body->AddStatement(std::move(cs2));
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
          const auto* cond_block = cfg_.GetBlock(cond_block_id);
          bool has_branch_likely = false;
          if (cond_block != nullptr) {
            const auto* term = cond_block->Terminator();
            if (term != nullptr && term->IsBranchLikely() && cond_block->instructions.size() >= 2) {
              has_branch_likely = true;
            }
          }

          bool decouple_delay = !has_branch_likely && ShouldDecoupleLoopConditionDelaySlot(
                                                          cond_block_id, region.loop_header);

          bool delay_clobbers_cond = false;
          if (!decouple_delay && cond_block != nullptr && cond_block->instructions.size() >= 2) {
            const auto* term = cond_block->Terminator();
            if (term != nullptr && term->IsBranch() && !term->IsBranchLikely()) {
              const auto& delay_inst = cond_block->instructions.back();
              if (!delay_inst.IsNop()) {
                RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
                absl::flat_hash_set<Register> cond_regs;
                if (term->rs.has_value() && *term->rs != Register::kZero) {
                  cond_regs.insert(*term->rs);
                }
                if (term->rt.has_value() && *term->rt != Register::kZero) {
                  cond_regs.insert(*term->rt);
                }
                auto folded_comp =
                    ExpressionBuilder::FindFoldedComparison(cond_block->instructions);
                if (folded_comp.has_value() &&
                    folded_comp->instruction_index < cond_block->instructions.size()) {
                  RegisterUseDef def_ud = GetInstructionUseDef(
                      cond_block->instructions[folded_comp->instruction_index]);
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
                    delay_clobbers_cond = true;
                    break;
                  }
                }
              }
            }
          }

          if (has_branch_likely || delay_clobbers_cond || decouple_delay) {
            omit_last_instruction_blocks_.insert(cond_block_id);
          }

          auto loop_body = std::make_unique<CompoundStatement>();
          if (!region.children.empty() && region.children[0]) {
            ConvertRegion(*region.children[0], loop_body.get());
          }

          if (delay_clobbers_cond && cond_block != nullptr &&
              cond_block->instructions.size() >= 2) {
            bool condition_block_was_emitted_in_body =
                emitted_blocks_.contains(region.condition_block_id);
            emitted_blocks_.insert(region.condition_block_id);
            if (!condition_block_was_emitted_in_body) {
              size_t condition_instruction_index = cond_block->instructions.size() - 2;
              auto folded_comparison =
                  ExpressionBuilder::FindFoldedComparison(cond_block->instructions);
              if (folded_comparison.has_value() &&
                  folded_comparison->instruction_index < cond_block->instructions.size()) {
                condition_instruction_index = folded_comparison->instruction_index;
              }
              if (condition_instruction_index > 0) {
                auto pre_condition_statements = LiftInstructionsHelper(
                    absl::MakeSpan(cond_block->instructions.data(), condition_instruction_index));
                for (auto& statement : pre_condition_statements) {
                  auto converted_statement =
                      AstConverter::ConvertStatement(statement, function_name_);
                  if (converted_statement != nullptr) {
                    loop_body->AddStatement(std::move(converted_statement));
                  }
                }
              }
            }
            auto break_cond =
                AstConverter::ExtractBranchCondition(*cond_block, !region.invert_condition);
            const auto& delay_inst = cond_block->instructions.back();
            bool branch_taken_is_break = region.invert_condition;
            bool emit_in_break =
                !delay_inst.IsNop() && (!has_branch_likely || branch_taken_is_break);
            bool emit_in_loop =
                !delay_inst.IsNop() && (!has_branch_likely || !branch_taken_is_break);

            auto break_body = std::make_unique<CompoundStatement>();
            if (emit_in_break) {
              auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
              for (auto& s : delay_stmts) {
                auto cs = AstConverter::ConvertStatement(s, function_name_);
                if (cs != nullptr) {
                  break_body->AddStatement(std::move(cs));
                }
              }
            }
            break_body->AddStatement(CStatement::Break());
            loop_body->AddStatement(CStatement::If(std::move(break_cond), std::move(break_body)));

            if (emit_in_loop) {
              auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
              for (auto& s : delay_stmts) {
                auto cs = AstConverter::ConvertStatement(s, function_name_);
                if (cs != nullptr) {
                  loop_body->AddStatement(std::move(cs));
                }
              }
            }
            target_block->AddStatement(
                CStatement::While(CExpression::Integer(1), std::move(loop_body)));
          } else {
            if (has_branch_likely && cond_block != nullptr &&
                cond_block->instructions.size() >= 2) {
              const auto& delay_inst = cond_block->instructions.back();
              if (!delay_inst.IsNop()) {
                auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
                if (peeled_loop_delays_.contains(cond_block_id)) {
                  auto new_loop_body = std::make_unique<CompoundStatement>();
                  for (auto& s : delay_stmts) {
                    auto cs = AstConverter::ConvertStatement(s);
                    if (cs != nullptr) {
                      new_loop_body->AddStatement(std::move(cs));
                    }
                  }
                  for (const auto& s : loop_body->Statements()) {
                    new_loop_body->AddStatement(s->Clone());
                  }
                  loop_body = std::move(new_loop_body);
                } else {
                  for (auto& s : delay_stmts) {
                    auto cs = AstConverter::ConvertStatement(s);
                    if (cs != nullptr) {
                      loop_body->AddStatement(std::move(cs));
                    }
                  }
                }
              }
            }
            auto cond = cond_block != nullptr ? AstConverter::ExtractBranchCondition(
                                                    *cond_block, region.invert_condition)
                                              : CExpression::Integer(1);
            target_block->AddStatement(CStatement::DoWhile(std::move(loop_body), std::move(cond)));

            if (decouple_delay && cond_block != nullptr && cond_block->instructions.size() >= 2) {
              const auto& delay_inst = cond_block->instructions.back();
              auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
              for (auto& s : delay_stmts) {
                auto cs = AstConverter::ConvertStatement(s);
                if (cs != nullptr) {
                  target_block->AddStatement(std::move(cs));
                }
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
          size_t cond_inst_idx = (header_block != nullptr) ? header_block->instructions.size() : 0;
          if (header_block != nullptr) {
            for (size_t k = 0; k < header_block->instructions.size(); ++k) {
              if (header_block->instructions[k].IsBranch()) {
                cond_inst_idx = k;
                break;
              }
            }
            auto folded_comp = ExpressionBuilder::FindFoldedComparison(header_block->instructions);
            if (folded_comp.has_value() && folded_comp->instruction_index < cond_inst_idx) {
              cond_inst_idx = folded_comp->instruction_index;
            }
          }

          std::vector<std::unique_ptr<CStatement>> pre_cond_stmts;
          if (header_block != nullptr && cond_inst_idx > 0) {
            auto lifted = LiftInstructionsHelper(
                absl::MakeSpan(header_block->instructions.data(), cond_inst_idx));
            for (auto& s : lifted) {
              auto cs = AstConverter::ConvertStatement(s);
              if (cs != nullptr) {
                pre_cond_stmts.push_back(std::move(cs));
              }
            }
          }

          bool has_branch_likely = false;
          if (header_block != nullptr) {
            const auto* term = header_block->Terminator();
            if (term != nullptr && term->IsBranchLikely() &&
                header_block->instructions.size() >= 2) {
              has_branch_likely = true;
            }
          }

          bool can_inline_pre_cond = false;
          std::string inlined_reg;
          std::unique_ptr<CExpression> inlined_rhs;

          if (pre_cond_stmts.size() == 1 &&
              pre_cond_stmts[0]->Kind() == CStatementKind::kExpressionStatement) {
            const auto& expr_stmt = static_cast<const ExpressionStatement&>(*pre_cond_stmts[0]);
            if (expr_stmt.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
              const auto& assign = static_cast<const AssignmentExpression&>(expr_stmt.Expression());
              if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
                std::string lhs_name =
                    static_cast<const IdentifierExpression&>(assign.Lhs()).Name();

                auto is_pure_expr = [](const CExpression& expr) {
                  bool has_call = false;
                  std::function<void(const CExpression&)> check = [&](const CExpression& e) {
                    if (e.Kind() == CExpressionKind::kCallExpression ||
                        e.Kind() == CExpressionKind::kAssignmentExpression) {
                      has_call = true;
                      return;
                    }
                    if (e.Kind() == CExpressionKind::kUnaryExpression) {
                      check(static_cast<const UnaryExpression&>(e).Operand());
                    } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
                      const auto& b = static_cast<const BinaryExpression&>(e);
                      check(b.Lhs());
                      check(b.Rhs());
                    } else if (e.Kind() == CExpressionKind::kCastExpression) {
                      check(static_cast<const CastExpression&>(e).Operand());
                    } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
                      const auto& a = static_cast<const ArrayIndexExpression&>(e);
                      check(a.Array());
                      check(a.Index());
                    } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
                      check(static_cast<const MemberAccessExpression&>(e).Object());
                    }
                  };
                  check(expr);
                  return !has_call;
                };

                if (is_pure_expr(assign.Rhs())) {
                  auto raw_cond = header_block != nullptr
                                      ? AstConverter::ExtractBranchCondition(
                                            *header_block, region.invert_condition)
                                      : CExpression::Integer(1);
                  std::vector<std::string> cond_vars;
                  absl::flat_hash_set<std::string> cond_seen;
                  CollectVariablesInExpression(*raw_cond, &cond_vars, &cond_seen);

                  if (cond_seen.contains(lhs_name)) {
                    bool variable_used_elsewhere = false;
                    for (size_t k = cond_inst_idx + 1; k < header_block->instructions.size(); ++k) {
                      RegisterUseDef instruction_use_def =
                          GetInstructionUseDef(header_block->instructions[k]);
                      for (Register use_register : instruction_use_def.gpr_uses) {
                        if (ExpressionBuilder::RegisterVarName(use_register) == lhs_name) {
                          variable_used_elsewhere = true;
                          break;
                        }
                      }
                      for (Register definition_register : instruction_use_def.gpr_defs) {
                        if (ExpressionBuilder::RegisterVarName(definition_register) == lhs_name) {
                          variable_used_elsewhere = true;
                          break;
                        }
                      }
                      if (variable_used_elsewhere) break;
                    }

                    if (!variable_used_elsewhere) {
                      for (const auto& block : cfg_.Blocks()) {
                        if (block.id == header_block->id) continue;
                        for (const auto& inst : block.instructions) {
                          RegisterUseDef instruction_use_def = GetInstructionUseDef(inst);
                          for (Register use_register : instruction_use_def.gpr_uses) {
                            if (ExpressionBuilder::RegisterVarName(use_register) == lhs_name) {
                              variable_used_elsewhere = true;
                              break;
                            }
                          }
                          if (variable_used_elsewhere) break;
                        }
                        if (variable_used_elsewhere) break;
                      }
                    }

                    if (!variable_used_elsewhere) {
                      can_inline_pre_cond = true;
                      inlined_reg = lhs_name;
                      inlined_rhs = assign.Rhs().Clone();
                    }
                  }
                }
              }
            }
          }

          if (can_inline_pre_cond) {
            auto raw_cond = header_block != nullptr ? AstConverter::ExtractBranchCondition(
                                                          *header_block, region.invert_condition)
                                                    : CExpression::Integer(1);
            auto cond = SubstituteIdentifier(*raw_cond, inlined_reg, *inlined_rhs);
            auto loop_body = std::make_unique<CompoundStatement>();
            bool branch_taken_is_break = region.invert_condition;
            const Instruction* delay_inst =
                (header_block != nullptr && header_block->instructions.size() >= 2)
                    ? &header_block->instructions.back()
                    : nullptr;
            bool has_delay = (delay_inst != nullptr && !delay_inst->IsNop());
            bool emit_in_loop = has_delay && (!has_branch_likely || !branch_taken_is_break);

            if (emit_in_loop) {
              auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(delay_inst, 1));
              for (auto& s : delay_stmts) {
                auto cs = AstConverter::ConvertStatement(s);
                if (cs != nullptr) {
                  loop_body->AddStatement(std::move(cs));
                }
              }
            }
            if (!region.children.empty() && region.children[0]) {
              ConvertRegion(*region.children[0], loop_body.get());
            }
            target_block->AddStatement(CStatement::While(std::move(cond), std::move(loop_body)));
          } else if (!pre_cond_stmts.empty()) {
            auto loop_body = std::make_unique<CompoundStatement>();
            for (auto& s : pre_cond_stmts) {
              loop_body->AddStatement(std::move(s));
            }
            auto break_cond = header_block != nullptr ? AstConverter::ExtractBranchCondition(
                                                            *header_block, !region.invert_condition)
                                                      : CExpression::Integer(0);
            auto break_body = std::make_unique<CompoundStatement>();

            bool branch_taken_is_break = region.invert_condition;
            const Instruction* delay_inst =
                (header_block != nullptr && header_block->instructions.size() >= 2)
                    ? &header_block->instructions.back()
                    : nullptr;
            bool has_delay = (delay_inst != nullptr && !delay_inst->IsNop());
            bool emit_in_break = has_delay && (!has_branch_likely || branch_taken_is_break);
            bool emit_in_loop = has_delay && (!has_branch_likely || !branch_taken_is_break);

            if (emit_in_break) {
              auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(delay_inst, 1));
              for (auto& s : delay_stmts) {
                auto cs = AstConverter::ConvertStatement(s);
                if (cs != nullptr) {
                  break_body->AddStatement(std::move(cs));
                }
              }
            }

            break_body->AddStatement(CStatement::Break());
            loop_body->AddStatement(
                CStatement::If(std::move(break_cond), std::move(break_body), nullptr));

            if (emit_in_loop) {
              auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(delay_inst, 1));
              for (auto& s : delay_stmts) {
                auto cs = AstConverter::ConvertStatement(s);
                if (cs != nullptr) {
                  loop_body->AddStatement(std::move(cs));
                }
              }
            }
            if (!region.children.empty() && region.children[0]) {
              ConvertRegion(*region.children[0], loop_body.get());
            }
            target_block->AddStatement(
                CStatement::While(CExpression::Integer(1), std::move(loop_body)));
          } else {
            auto cond = header_block != nullptr ? AstConverter::ExtractBranchCondition(
                                                      *header_block, region.invert_condition)
                                                : CExpression::Integer(1);
            auto loop_body = std::make_unique<CompoundStatement>();
            bool branch_taken_is_break = region.invert_condition;
            const Instruction* delay_inst =
                (header_block != nullptr && header_block->instructions.size() >= 2)
                    ? &header_block->instructions.back()
                    : nullptr;
            bool has_delay = (delay_inst != nullptr && !delay_inst->IsNop());
            bool emit_in_loop = has_delay && (!has_branch_likely || !branch_taken_is_break);
            bool emit_after_loop = has_delay && (has_branch_likely && branch_taken_is_break);

            if (emit_in_loop) {
              auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(delay_inst, 1));
              for (auto& s : delay_stmts) {
                auto cs = AstConverter::ConvertStatement(s);
                if (cs != nullptr) {
                  loop_body->AddStatement(std::move(cs));
                }
              }
            }
            if (!region.children.empty() && region.children[0]) {
              ConvertRegion(*region.children[0], loop_body.get());
            }
            target_block->AddStatement(CStatement::While(std::move(cond), std::move(loop_body)));
            if (emit_after_loop) {
              auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(delay_inst, 1));
              for (auto& s : delay_stmts) {
                auto cs = AstConverter::ConvertStatement(s);
                if (cs != nullptr) {
                  target_block->AddStatement(std::move(cs));
                }
              }
            }
          }
        }
        break;
      }

      case RegionType::kBreak: {
        if (current_switch_break_block_.has_value() &&
            region.block_id == *current_switch_break_block_) {
          target_block->AddStatement(CStatement::Break());
          break;
        }
        bool is_return = false;
        const auto* target_blk = cfg_.GetBlock(region.block_id);
        if (target_blk != nullptr) {
          if (target_blk->successors.empty()) {
            is_return = true;
          } else if (target_blk->successors.size() == 1) {
            const auto* succ_blk = cfg_.GetBlock(target_blk->successors[0]);
            if (succ_blk != nullptr && succ_blk->successors.empty()) {
              is_return = true;
            }
          }
        }
        if (is_return) {
          if (returns_v0_) {
            target_block->AddStatement(CStatement::Return(CExpression::Identifier("v0")));
          } else if (returns_f0_) {
            bool is_double = ExpressionBuilder::DetermineReturnsF0Double(cfg_);
            target_block->AddStatement(
                CStatement::Return(CExpression::Identifier(is_double ? "f0_d" : "f0")));
          } else {
            target_block->AddStatement(CStatement::Return());
          }
        } else {
          target_block->AddStatement(CStatement::Break());
        }
        break;
      }

      case RegionType::kContinue:
        target_block->AddStatement(CStatement::Continue());
        break;

      case RegionType::kGoto:
        target_block->AddStatement(CStatement::Goto(absl::StrFormat("block_%d", region.block_id)));
        break;

      case RegionType::kSwitch: {
        auto prev_switch_break = current_switch_break_block_;
        current_switch_break_block_ = region.switch_join_block;
        bool is_case_block = false;
        for (const auto& sc : region.cases) {
          if (sc.body && sc.body->type == RegionType::kBlock &&
              sc.body->block_id == region.switch_block_id) {
            is_case_block = true;
            break;
          }
        }
        Register switch_selector_register = Register::kZero;
        if (region.jump_table == nullptr || region.owned_jump_table != nullptr) {
          switch_selector_register = region.switch_register;
        }
        if (!is_case_block &&
            (region.jump_table == nullptr || region.owned_jump_table != nullptr)) {
          EmitPreSwitchStatements(region.switch_block_id, target_block, switch_selector_register);
        }

        const auto* jt = region.jump_table;
        std::unique_ptr<CExpression> cond_expr;
        if (jt != nullptr && jt->index_register != Register::kZero) {
          cond_expr = RegExpr(jt->index_register);
        } else if (region.switch_register != Register::kZero) {
          cond_expr = RegExpr(region.switch_register);
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

          if (sc.is_default && jt != nullptr && jt->bounds_branch_vram != 0) {
            const auto* bounds_block = cfg_.FindBlockByVram(jt->bounds_branch_vram);
            if (bounds_block != nullptr) {
              const auto* term = bounds_block->Terminator();
              if (term != nullptr && term->IsBranchLikely() &&
                  bounds_block->instructions.size() >= 2) {
                const auto& delay_instruction = bounds_block->instructions.back();
                bool join_block_executes_delay = false;
                if (region.switch_join_block.has_value()) {
                  const auto* join_block = cfg_.GetBlock(*region.switch_join_block);
                  if (join_block != nullptr && !join_block->instructions.empty()) {
                    const auto& join_instruction = join_block->instructions.front();
                    if (delay_instruction.opcode == join_instruction.opcode &&
                        delay_instruction.rs == join_instruction.rs &&
                        delay_instruction.rt == join_instruction.rt &&
                        delay_instruction.immediate == join_instruction.immediate) {
                      join_block_executes_delay = true;
                    }
                  }
                }
                if (!join_block_executes_delay && !delay_instruction.IsNop()) {
                  auto delay_statements =
                      LiftInstructionsHelper(absl::MakeSpan(&delay_instruction, 1));
                  for (auto& statement : delay_statements) {
                    auto converted_statement =
                        AstConverter::ConvertStatement(statement, function_name_);
                    if (converted_statement != nullptr) {
                      c_case.body->AddStatement(std::move(converted_statement));
                    }
                  }
                }
              }
            }
          }

          if (!sc.delay_instructions.empty()) {
            auto delay_statements = LiftInstructionsHelper(sc.delay_instructions);
            for (auto& statement : delay_statements) {
              auto converted_statement = AstConverter::ConvertStatement(statement, function_name_);
              if (converted_statement != nullptr) {
                c_case.body->AddStatement(std::move(converted_statement));
              }
            }
          } else if (sc.delay_slot_const_reg.has_value() && sc.delay_slot_const_val.has_value()) {
            std::string variable_name =
                ExpressionBuilder::RegisterVarName(*sc.delay_slot_const_reg);
            c_case.body->AddStatement(CStatement::Expression(
                CExpression::Assignment("=", CExpression::Identifier(variable_name),
                                        CExpression::Integer(*sc.delay_slot_const_val))));
          }

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

        current_switch_break_block_ = prev_switch_break;
        target_block->AddStatement(CStatement::Switch(std::move(cond_expr), std::move(c_cases)));
        break;
      }
    }
  }

 private:
  void PreScanPeeledLoopRotations(const StructuredRegion& region) {
    if (region.type == RegionType::kLoop && region.loop_type == LoopType::kDoWhile) {
      uint32_t cond_block_id =
          region.condition_block_id != 0 ? region.condition_block_id : region.loop_header;
      const auto* cond_block = cfg_.GetBlock(cond_block_id);
      if (cond_block != nullptr && cond_block->instructions.size() >= 2) {
        const auto* term = cond_block->Terminator();
        if (term != nullptr && term->IsBranchLikely()) {
          const auto& delay_inst = cond_block->instructions.back();
          // Find loop entry predecessor (from outside the loop)
          for (const auto& edge : cfg_.Edges()) {
            if (edge.to_block_id == region.loop_header && edge.from_block_id != cond_block_id) {
              const auto* pred_block = cfg_.GetBlock(edge.from_block_id);
              if (pred_block != nullptr && !pred_block->instructions.empty()) {
                const auto& pred_inst = pred_block->instructions.back();
                if (pred_inst.opcode == delay_inst.opcode && pred_inst.rs == delay_inst.rs &&
                    pred_inst.rt == delay_inst.rt && pred_inst.rd == delay_inst.rd &&
                    pred_inst.immediate == delay_inst.immediate && !delay_inst.IsNop()) {
                  omit_last_instruction_blocks_.insert(pred_block->id);
                  peeled_loop_delays_[cond_block_id] = delay_inst;
                  break;
                }
              }
            }
          }
        }
      }
    }
    for (const auto& child : region.children) {
      if (child) PreScanPeeledLoopRotations(*child);
    }
  }

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

  bool ShouldDecoupleBranchConditionDelaySlot(uint32_t cond_block_id) const {
    return BranchConditionBuilder::ShouldDecoupleBranchConditionDelaySlot(cfg_, cond_block_id);
  }

  bool ShouldDecoupleLoopConditionDelaySlot(uint32_t cond_block_id,
                                            uint32_t header_block_id) const {
    return BranchConditionBuilder::ShouldDecoupleLoopConditionDelaySlot(cfg_, cond_block_id,
                                                                        header_block_id);
  }

  void EmitPreSwitchStatements(uint32_t block_id, CompoundStatement* target_block,
                               Register switch_selector_register = Register::kZero) {
    if (emitted_blocks_.contains(block_id)) return;
    emitted_blocks_.insert(block_id);

    if (goto_targets_.contains(block_id)) {
      target_block->AddStatement(CStatement::Label(absl::StrFormat("block_%d", block_id)));
      emitted_labels_.insert(block_id);
    }

    const auto* block = cfg_.GetBlock(block_id);
    if (block == nullptr || block->instructions.empty()) return;

    size_t pre_switch_count = 0;
    int branch_index = -1;
    for (int i = static_cast<int>(block->instructions.size()) - 1; i >= 0; --i) {
      if (block->instructions[i].IsBranch()) {
        branch_index = i;
        break;
      }
    }

    if (branch_index >= 0) {
      const auto& branch = block->instructions[branch_index];
      absl::flat_hash_set<Register> branch_used_registers;
      if (branch.rs.has_value() && *branch.rs != Register::kZero) {
        branch_used_registers.insert(*branch.rs);
      }
      if (branch.rt.has_value() && *branch.rt != Register::kZero) {
        branch_used_registers.insert(*branch.rt);
      }

      int switch_setup_start = branch_index;
      for (int i = branch_index - 1; i >= 0; --i) {
        const auto& inst = block->instructions[i];
        RegisterUseDef instruction_use_def = GetInstructionUseDef(inst);

        bool defines_switch_selector = false;
        if (switch_selector_register != Register::kZero) {
          for (Register destination_register : instruction_use_def.gpr_defs) {
            if (destination_register == switch_selector_register) {
              defines_switch_selector = true;
              break;
            }
          }
        }
        if (defines_switch_selector) {
          break;
        }

        bool defines_branch_register = false;
        for (Register destination_register : instruction_use_def.gpr_defs) {
          if (branch_used_registers.contains(destination_register)) {
            defines_branch_register = true;
            break;
          }
        }
        if (defines_branch_register) {
          switch_setup_start = i;
          for (Register use_register : instruction_use_def.gpr_uses) {
            if (use_register != Register::kZero) branch_used_registers.insert(use_register);
          }
        } else {
          break;
        }
      }
      pre_switch_count = static_cast<size_t>(switch_setup_start);
    } else if (!setup_instruction_vrams_.empty()) {
      std::vector<Instruction> pre_insts;
      for (const auto& inst : block->instructions) {
        if (!setup_instruction_vrams_.contains(inst.vram) && inst.opcode != Opcode::kJr &&
            inst.opcode != Opcode::kJ && !inst.IsBranch()) {
          pre_insts.push_back(inst);
        }
      }
      if (!pre_insts.empty()) {
        auto lifted = LiftInstructionsHelper(pre_insts);
        for (auto& s : lifted) {
          auto cs = AstConverter::ConvertStatement(s, function_name_);
          if (cs != nullptr) {
            target_block->AddStatement(std::move(cs));
          }
        }
      }
      return;
    }

    if (pre_switch_count > 0) {
      auto lifted =
          LiftInstructionsHelper(absl::MakeSpan(block->instructions.data(), pre_switch_count));
      for (auto& s : lifted) {
        auto cs = AstConverter::ConvertStatement(s, function_name_);
        if (cs != nullptr) {
          target_block->AddStatement(std::move(cs));
        }
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

    BasicBlock filtered_block = *block;
    if (!setup_instruction_vrams_.empty()) {
      filtered_block.instructions.erase(
          std::remove_if(filtered_block.instructions.begin(), filtered_block.instructions.end(),
                         [this](const Instruction& inst) {
                           return setup_instruction_vrams_.contains(inst.vram);
                         }),
          filtered_block.instructions.end());
    }

    bool should_omit = omit_last_instruction_blocks_.contains(block_id) ||
                       (omit_last_instruction && filtered_block.instructions.size() > 1);
    const std::array<std::optional<uint32_t>, 32>* initial_fpr = nullptr;
    const RegisterTracker* initial_tracker = nullptr;
    auto it_state = block_entry_states_.find(block_id);
    if (it_state != block_entry_states_.end()) {
      initial_fpr = &it_state->second.fpr_const_bits;
      initial_tracker = &it_state->second.tracker;
    }

    auto lifted_stmts = ExpressionBuilder::LiftBlock(
        filtered_block, BuildLiftContext(initial_fpr, initial_tracker), should_omit);
    for (auto& lifted_stmt : lifted_stmts) {
      if (lifted_stmt.kind == StatementKind::kReturn) {
        if (returns_v0_) {
          if (!lifted_stmt.expression) {
            lifted_stmt.expression = LiftedExpression::Variable("v0");
          }
        } else if (returns_f0_) {
          if (!lifted_stmt.expression) {
            bool is_double = ExpressionBuilder::DetermineReturnsF0Double(cfg_);
            lifted_stmt.expression = LiftedExpression::Variable(is_double ? "f0_d" : "f0");
          }
        } else {
          lifted_stmt.expression = nullptr;
        }
      }
      auto c_stmt = AstConverter::ConvertStatement(lifted_stmt, function_name_);
      if (c_stmt != nullptr) {
        target_block->AddStatement(std::move(c_stmt));
      }
    }
  }

  struct BlockState {
    std::array<std::optional<uint32_t>, 32> fpr_const_bits{};
    RegisterTracker tracker;
  };

  void ComputeBlockEntryStates() {
    auto rpo = cfg_.ReversePostOrder();
    if (rpo.empty()) return;

    absl::flat_hash_map<uint32_t, BlockState> exit_states;
    absl::flat_hash_map<uint32_t, BlockState> exit_fallthrough_states;

    absl::flat_hash_map<uint32_t, std::vector<CfgEdge>> incoming_edges;
    for (const auto& edge : cfg_.Edges()) {
      incoming_edges[edge.to_block_id].push_back(edge);
    }

    bool changed = true;
    int iterations = 0;
    constexpr int kMaxIterations = 16;
    while (changed && iterations < kMaxIterations) {
      changed = false;
      ++iterations;
      for (uint32_t block_id : rpo) {
        const auto* block = cfg_.GetBlock(block_id);
        if (block == nullptr) continue;

        BlockState entry_state;
        bool has_entry = false;
        auto it_edges = incoming_edges.find(block_id);
        if (it_edges != incoming_edges.end()) {
          for (const auto& edge : it_edges->second) {
            const BlockState* pred_state = nullptr;
            if (edge.type == EdgeType::kFallthrough) {
              auto it_ft = exit_fallthrough_states.find(edge.from_block_id);
              if (it_ft != exit_fallthrough_states.end()) {
                pred_state = &it_ft->second;
              }
            }
            if (pred_state == nullptr) {
              auto it = exit_states.find(edge.from_block_id);
              if (it != exit_states.end()) {
                pred_state = &it->second;
              }
            }
            if (pred_state != nullptr) {
              if (!has_entry) {
                entry_state = *pred_state;
                has_entry = true;
              } else {
                // Merge with existing entry state
                for (int k = 0; k < 32; ++k) {
                  if (entry_state.fpr_const_bits[k] != pred_state->fpr_const_bits[k]) {
                    entry_state.fpr_const_bits[k] = std::nullopt;
                  }
                }
                entry_state.tracker.Merge(pred_state->tracker);
              }
            }
          }
        }

        // Simulate block instructions to compute exit state
        BlockState exit_state = entry_state;
        std::optional<BlockState> exit_fallthrough_state;
        const auto* term = block->Terminator();
        bool is_likely = (term != nullptr && term->IsBranchLikely());

        auto step_inst = [](BlockState& state, const Instruction& inst) {
          if (inst.IsCall()) {
            for (int f = 0; f < 20; ++f) {
              state.fpr_const_bits[f] = std::nullopt;
            }
          } else if (inst.opcode != Opcode::kMtc1) {
            RegisterUseDef ud = GetInstructionUseDef(inst);
            for (FpRegister d : ud.fpr_defs) {
              int f_idx = static_cast<int>(d);
              state.fpr_const_bits[f_idx] = std::nullopt;
              if ((f_idx % 2) == 0 && f_idx + 1 < 32) {
                state.fpr_const_bits[f_idx + 1] = std::nullopt;
              }
            }
          } else if (inst.fs.has_value() && inst.rt.has_value()) {
            int fs_idx = static_cast<int>(*inst.fs);
            if (*inst.rt == Register::kZero) {
              state.fpr_const_bits[fs_idx] = 0;
            } else {
              TrackedValue tv = state.tracker.GetRegisterValue(*inst.rt);
              if (tv.kind == ValueKind::kConstant) {
                state.fpr_const_bits[fs_idx] = tv.constant_value;
              } else if (tv.kind == ValueKind::kSymbolHi) {
                state.fpr_const_bits[fs_idx] = tv.symbol_hi;
              } else {
                state.fpr_const_bits[fs_idx] = std::nullopt;
              }
            }
          }
          state.tracker.Step(inst);
          if (inst.IsCall()) {
            state.tracker.Invalidate(Register::kV0);
            state.tracker.Invalidate(Register::kV1);
            state.tracker.Invalidate(Register::kA0);
            state.tracker.Invalidate(Register::kA1);
            state.tracker.Invalidate(Register::kA2);
            state.tracker.Invalidate(Register::kA3);
            for (int t = static_cast<int>(Register::kT0); t <= static_cast<int>(Register::kT7);
                 ++t) {
              state.tracker.Invalidate(static_cast<Register>(t));
            }
            state.tracker.Invalidate(Register::kT8);
            state.tracker.Invalidate(Register::kT9);
          }
        };

        for (size_t idx = 0; idx < block->instructions.size(); ++idx) {
          const auto& inst = block->instructions[idx];
          if ((inst.IsCall() || inst.IsBranch() || inst.IsJump()) &&
              idx + 1 < block->instructions.size()) {
            if (is_likely && idx + 2 == block->instructions.size()) {
              exit_fallthrough_state = exit_state;
              step_inst(*exit_fallthrough_state, inst);
            }
            step_inst(exit_state, block->instructions[idx + 1]);
            step_inst(exit_state, inst);
            ++idx;
            continue;
          }
          if (is_likely && idx + 1 == block->instructions.size()) {
            exit_fallthrough_state = exit_state;
          }
          step_inst(exit_state, inst);
        }

        if (exit_fallthrough_state.has_value()) {
          exit_fallthrough_states[block_id] = std::move(*exit_fallthrough_state);
        }

        auto it_prev = exit_states.find(block_id);
        if (it_prev == exit_states.end() ||
            it_prev->second.fpr_const_bits != exit_state.fpr_const_bits) {
          changed = true;
        } else {
          for (int r = 0; r < 32; ++r) {
            Register reg = static_cast<Register>(r);
            if (it_prev->second.tracker.GetRegisterValue(reg) !=
                exit_state.tracker.GetRegisterValue(reg)) {
              changed = true;
              break;
            }
          }
        }
        exit_states[block_id] = std::move(exit_state);
        block_entry_states_[block_id] = std::move(entry_state);
      }
    }
  }

  const ControlFlowGraph& cfg_;
  const SymbolIndex* symbol_index_;
  const GameConfig* game_config_;
  const absl::flat_hash_map<std::string, int>* function_parameter_counts_;
  const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts_;
  bool returns_v0_ = false;
  bool returns_f0_ = false;
  const absl::flat_hash_set<std::string>* functions_returning_v0_ = nullptr;
  const absl::flat_hash_set<std::string>* functions_returning_f0_ = nullptr;
  std::string function_name_;
  absl::flat_hash_set<std::string> double_vars_;
  StackFrameAnalysis frame_analysis_;
  absl::flat_hash_set<uint32_t> goto_targets_;
  absl::flat_hash_set<uint32_t> emitted_labels_;
  absl::flat_hash_set<uint32_t> emitted_blocks_;
  absl::flat_hash_set<uint32_t> omit_last_instruction_blocks_;
  absl::flat_hash_set<uint32_t> setup_instruction_vrams_;
  absl::flat_hash_map<uint32_t, BlockState> block_entry_states_;
  absl::flat_hash_set<Register> float_parameters_in_general_registers_;
  std::optional<uint32_t> current_switch_break_block_;
  absl::flat_hash_map<uint32_t, CallReturnInfo> call_return_info_;
  absl::flat_hash_map<uint32_t, Instruction> peeled_loop_delays_;
};

void ForEachSubCompoundStatement(CStatement* stmt,
                                 const std::function<void(CompoundStatement*)>& visitor) {
  if (stmt == nullptr) return;
  if (stmt->Kind() == CStatementKind::kCompoundStatement) {
    visitor(static_cast<CompoundStatement*>(stmt));
  } else if (stmt->Kind() == CStatementKind::kIfStatement) {
    auto* if_stmt = static_cast<IfStatement*>(stmt);
    if (if_stmt->MutableThenBranch() != nullptr &&
        if_stmt->MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(if_stmt->MutableThenBranch()));
    }
    if (if_stmt->MutableElseBranch() != nullptr &&
        if_stmt->MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(if_stmt->MutableElseBranch()));
    }
  } else if (stmt->Kind() == CStatementKind::kWhileStatement) {
    auto* while_stmt = static_cast<WhileStatement*>(stmt);
    if (while_stmt->MutableBody() != nullptr &&
        while_stmt->MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(while_stmt->MutableBody()));
    }
  } else if (stmt->Kind() == CStatementKind::kDoWhileStatement) {
    auto* do_while_stmt = static_cast<DoWhileStatement*>(stmt);
    if (do_while_stmt->MutableBody() != nullptr &&
        do_while_stmt->MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(do_while_stmt->MutableBody()));
    }
  } else if (stmt->Kind() == CStatementKind::kForStatement) {
    auto* for_stmt = static_cast<ForStatement*>(stmt);
    if (for_stmt->MutableBody() != nullptr &&
        for_stmt->MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
      visitor(static_cast<CompoundStatement*>(for_stmt->MutableBody()));
    }
  } else if (stmt->Kind() == CStatementKind::kSwitchStatement) {
    auto* sw_stmt = static_cast<SwitchStatement*>(stmt);
    for (auto& sc : sw_stmt->MutableCases()) {
      if (sc.body != nullptr) {
        visitor(sc.body.get());
      }
    }
  }
}

void SimplifyReturnStatements(CompoundStatement* comp, bool returns_v0, bool returns_f0) {
  if (comp == nullptr) return;

  for (auto& s : comp->MutableStatements()) {
    ForEachSubCompoundStatement(s.get(), [&](CompoundStatement* sub) {
      SimplifyReturnStatements(sub, returns_v0, returns_f0);
    });
  }

  auto get_id_name = [](const CExpression& expr) -> std::string {
    if (expr.Kind() == CExpressionKind::kIdentifier) {
      return static_cast<const IdentifierExpression&>(expr).Name();
    }
    return "";
  };

  std::function<std::unique_ptr<CExpression>(const CExpression&, std::string_view,
                                             const CExpression&)>
      subst_id = [&](const CExpression& expr, std::string_view target,
                     const CExpression& replacement) -> std::unique_ptr<CExpression> {
    if (expr.Kind() == CExpressionKind::kIdentifier) {
      if (static_cast<const IdentifierExpression&>(expr).Name() == target) {
        return replacement.Clone();
      }
      return expr.Clone();
    }
    if (expr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return CExpression::Binary(bin.Op(), subst_id(bin.Lhs(), target, replacement),
                                 subst_id(bin.Rhs(), target, replacement));
    }
    if (expr.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(expr);
      return CExpression::Unary(un.Op(), subst_id(un.Operand(), target, replacement),
                                un.IsPrefix());
    }
    if (expr.Kind() == CExpressionKind::kCastExpression) {
      const auto& cast = static_cast<const CastExpression&>(expr);
      return CExpression::Cast(cast.TargetType(), subst_id(cast.Operand(), target, replacement));
    }
    if (expr.Kind() == CExpressionKind::kArrayIndexExpression) {
      const auto& arr = static_cast<const ArrayIndexExpression&>(expr);
      return CExpression::ArrayIndex(subst_id(arr.Array(), target, replacement),
                                     subst_id(arr.Index(), target, replacement));
    }
    if (expr.Kind() == CExpressionKind::kMemberAccessExpression) {
      const auto& mem = static_cast<const MemberAccessExpression&>(expr);
      return CExpression::MemberAccess(subst_id(mem.Object(), target, replacement),
                                       mem.MemberName(), mem.IsArrow());
    }
    return expr.Clone();
  };

  std::vector<std::unique_ptr<CStatement>> new_stmts;
  auto& old_stmts = comp->MutableStatements();
  for (size_t i = 0; i < old_stmts.size(); ++i) {
    if (i + 2 < old_stmts.size() && old_stmts[i]->Kind() == CStatementKind::kExpressionStatement &&
        old_stmts[i + 1]->Kind() == CStatementKind::kIfStatement &&
        old_stmts[i + 2]->Kind() == CStatementKind::kReturnStatement) {
      const auto& expr_s0 = static_cast<const ExpressionStatement&>(*old_stmts[i]);
      const auto& if_s = static_cast<const IfStatement&>(*old_stmts[i + 1]);
      const auto& ret_s = static_cast<const ReturnStatement&>(*old_stmts[i + 2]);

      if (expr_s0.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
          (if_s.ElseBranch() == nullptr ||
           (if_s.ElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
            static_cast<const CompoundStatement*>(if_s.ElseBranch())->IsEmpty()))) {
        const auto& assign0 = static_cast<const AssignmentExpression&>(expr_s0.Expression());
        std::string var_name = get_id_name(assign0.Lhs());
        if ((var_name == "v0" || var_name == "f0" || var_name == "f0_d") && assign0.Op() == "=") {
          bool ret_matches =
              (ret_s.ReturnValue() == nullptr || get_id_name(*ret_s.ReturnValue()) == var_name);
          if (ret_matches && if_s.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
            const auto& then_comp = static_cast<const CompoundStatement&>(if_s.ThenBranch());
            if (then_comp.Statements().size() == 1 &&
                then_comp.Statements()[0]->Kind() == CStatementKind::kExpressionStatement) {
              const auto& then_expr_s =
                  static_cast<const ExpressionStatement&>(*then_comp.Statements()[0]);
              if (then_expr_s.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                const auto& then_assign =
                    static_cast<const AssignmentExpression&>(then_expr_s.Expression());
                if (get_id_name(then_assign.Lhs()) == var_name && then_assign.Op() == "=") {
                  const auto& default_expr = assign0.Rhs();
                  const auto& raw_then_expr = then_assign.Rhs();
                  const auto& raw_cond = if_s.Condition();

                  bool uses_var = absl::StrContains(raw_then_expr.ToString(), var_name) ||
                                  absl::StrContains(raw_cond.ToString(), var_name);
                  if (!uses_var) {
                    new_stmts.push_back(std::move(old_stmts[i]));
                    continue;
                  }

                  auto final_then_expr = subst_id(raw_then_expr, var_name, default_expr);
                  auto final_cond = subst_id(raw_cond, var_name, default_expr);

                  if (final_cond->Kind() == CExpressionKind::kBinaryExpression) {
                    const auto& bin_cond = static_cast<const BinaryExpression&>(*final_cond);
                    if (bin_cond.Op() == ">=") {
                      std::string lhs_id = get_id_name(bin_cond.Lhs());
                      std::string rhs_id = get_id_name(bin_cond.Rhs());
                      std::string then_id = get_id_name(*final_then_expr);
                      std::string def_id = get_id_name(default_expr);
                      if (!lhs_id.empty() && !rhs_id.empty() && !then_id.empty() &&
                          !def_id.empty()) {
                        if (lhs_id == then_id && rhs_id == def_id) {
                          // MaxTwo pattern: if (arg0 > arg1) return arg0; return arg1;
                          auto inverted_cond = CExpression::Binary(">", default_expr.Clone(),
                                                                   final_then_expr->Clone());
                          auto then_return = std::make_unique<CompoundStatement>();
                          then_return->AddStatement(CStatement::Return(default_expr.Clone()));
                          new_stmts.push_back(
                              CStatement::If(std::move(inverted_cond), std::move(then_return)));
                          new_stmts.push_back(CStatement::Return(std::move(final_then_expr)));
                          i += 2;
                          continue;
                        } else if (lhs_id == def_id && rhs_id == then_id) {
                          // MinTwo pattern: if (arg0 < arg1) return arg0; return arg1;
                          auto inverted_cond = CExpression::Binary("<", default_expr.Clone(),
                                                                   final_then_expr->Clone());
                          auto then_return = std::make_unique<CompoundStatement>();
                          then_return->AddStatement(CStatement::Return(default_expr.Clone()));
                          new_stmts.push_back(
                              CStatement::If(std::move(inverted_cond), std::move(then_return)));
                          new_stmts.push_back(CStatement::Return(std::move(final_then_expr)));
                          i += 2;
                          continue;
                        }
                      }
                    }
                  }

                  auto then_return = std::make_unique<CompoundStatement>();
                  then_return->AddStatement(CStatement::Return(std::move(final_then_expr)));
                  new_stmts.push_back(
                      CStatement::If(std::move(final_cond), std::move(then_return)));
                  new_stmts.push_back(CStatement::Return(default_expr.Clone()));
                  i += 2;
                  continue;
                }
              }
            }
          }
        }
      }
    }

    if (i + 1 < old_stmts.size() && old_stmts[i]->Kind() == CStatementKind::kExpressionStatement &&
        old_stmts[i + 1]->Kind() == CStatementKind::kReturnStatement) {
      const auto& expr_s = static_cast<const ExpressionStatement&>(*old_stmts[i]);
      const auto& ret_s = static_cast<const ReturnStatement&>(*old_stmts[i + 1]);
      if (expr_s.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(expr_s.Expression());
        std::string var_name = get_id_name(assign.Lhs());
        if ((var_name == "v0" || var_name == "f0" || var_name == "f0_d") && assign.Op() == "=") {
          bool ret_matches =
              (ret_s.ReturnValue() == nullptr || get_id_name(*ret_s.ReturnValue()) == var_name);
          if (ret_matches) {
            new_stmts.push_back(CStatement::Return(assign.Rhs().Clone()));
            i += 1;
            continue;
          }
        }
      }
    }

    new_stmts.push_back(std::move(old_stmts[i]));
  }
  old_stmts = std::move(new_stmts);
}

void CoalescePrologueParameterCopies(CompoundStatement* comp,
                                     const std::vector<CParameter>& parameters) {
  if (comp == nullptr) return;
  absl::flat_hash_set<std::string> param_names;
  for (const auto& p : parameters) {
    param_names.insert(p.name);
  }

  auto is_assigned = [&](std::string_view name) {
    bool assigned = false;
    std::function<void(const CExpression&)> check_expr = [&](const CExpression& e) {
      if (assigned) return;
      if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(e);
        if (a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          if (static_cast<const IdentifierExpression&>(a.Lhs()).Name() == name) {
            assigned = true;
            return;
          }
        }
        check_expr(a.Lhs());
        check_expr(a.Rhs());
      } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& u = static_cast<const UnaryExpression&>(e);
        if ((u.Op() == "++" || u.Op() == "--") &&
            u.Operand().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(u.Operand()).Name() == name) {
          assigned = true;
          return;
        }
        check_expr(u.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check_expr(static_cast<const BinaryExpression&>(e).Lhs());
        check_expr(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check_expr(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check_expr(static_cast<const TernaryExpression&>(e).Condition());
        check_expr(static_cast<const TernaryExpression&>(e).TrueExpression());
        check_expr(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check_expr(*arg);
        }
      }
    };

    std::function<void(const CStatement&)> check_stmt = [&](const CStatement& s) {
      if (assigned) return;
      if (s.Kind() == CStatementKind::kExpressionStatement) {
        check_expr(static_cast<const ExpressionStatement&>(s).Expression());
      } else if (s.Kind() == CStatementKind::kCompoundStatement) {
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          check_stmt(*child);
        }
      } else if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        check_expr(ifs.Condition());
        check_stmt(ifs.ThenBranch());
        if (ifs.ElseBranch()) check_stmt(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kWhileStatement) {
        check_expr(static_cast<const WhileStatement&>(s).Condition());
        check_stmt(static_cast<const WhileStatement&>(s).Body());
      } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
        check_stmt(static_cast<const DoWhileStatement&>(s).Body());
        check_expr(static_cast<const DoWhileStatement&>(s).Condition());
      } else if (s.Kind() == CStatementKind::kForStatement) {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) check_stmt(*fs.Init());
        if (fs.Condition() != nullptr) check_expr(*fs.Condition());
        if (fs.Step() != nullptr) check_expr(*fs.Step());
        check_stmt(fs.Body());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        check_expr(ss.Condition());
        for (const auto& sc : ss.Cases()) {
          if (sc.body != nullptr) check_stmt(*sc.body);
        }
      }
    };
    check_stmt(*comp);
    return assigned;
  };

  absl::flat_hash_map<std::string, std::string> coalesced_aliases;
  std::vector<size_t> stmts_to_erase;

  auto count_assignments = [&](std::string_view name) {
    int count = 0;
    std::function<void(const CExpression&)> check_expr = [&](const CExpression& e) {
      if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(e);
        if (a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(a.Lhs()).Name() == name) {
          count++;
        }
        check_expr(a.Lhs());
        check_expr(a.Rhs());
      } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& u = static_cast<const UnaryExpression&>(e);
        if ((u.Op() == "++" || u.Op() == "--") &&
            u.Operand().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(u.Operand()).Name() == name) {
          count++;
        }
        check_expr(u.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check_expr(static_cast<const BinaryExpression&>(e).Lhs());
        check_expr(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check_expr(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check_expr(static_cast<const TernaryExpression&>(e).Condition());
        check_expr(static_cast<const TernaryExpression&>(e).TrueExpression());
        check_expr(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check_expr(*arg);
        }
      }
    };

    std::function<void(const CStatement&)> check_stmt = [&](const CStatement& s) {
      if (s.Kind() == CStatementKind::kExpressionStatement) {
        check_expr(static_cast<const ExpressionStatement&>(s).Expression());
      } else if (s.Kind() == CStatementKind::kCompoundStatement) {
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          check_stmt(*child);
        }
      } else if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        check_expr(ifs.Condition());
        check_stmt(ifs.ThenBranch());
        if (ifs.ElseBranch()) check_stmt(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kWhileStatement) {
        check_expr(static_cast<const WhileStatement&>(s).Condition());
        check_stmt(static_cast<const WhileStatement&>(s).Body());
      } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
        check_stmt(static_cast<const DoWhileStatement&>(s).Body());
        check_expr(static_cast<const DoWhileStatement&>(s).Condition());
      } else if (s.Kind() == CStatementKind::kForStatement) {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) check_stmt(*fs.Init());
        if (fs.Condition() != nullptr) check_expr(*fs.Condition());
        if (fs.Step() != nullptr) check_expr(*fs.Step());
        check_stmt(fs.Body());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        check_expr(ss.Condition());
        for (const auto& sc : ss.Cases()) {
          if (sc.body != nullptr) check_stmt(*sc.body);
        }
      }
    };
    check_stmt(*comp);
    return count;
  };

  for (size_t i = 0; i < comp->Statements().size(); ++i) {
    const auto& s = comp->Statements()[i];
    if (s->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*s);
      if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
            a.Rhs().Kind() == CExpressionKind::kIdentifier) {
          std::string lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          std::string rhs = static_cast<const IdentifierExpression&>(a.Rhs()).Name();
          bool is_s_reg = (absl::StartsWith(lhs, "temp_s") ||
                           (lhs.size() == 2 && lhs[0] == 's' && std::isdigit(lhs[1])));
          bool is_f_reg = (lhs.size() >= 2 && lhs[0] == 'f' && std::isdigit(lhs[1]));
          if (param_names.contains(rhs) && (is_s_reg || is_f_reg)) {
            if (!is_assigned(rhs) && count_assignments(lhs) == 1) {
              coalesced_aliases[lhs] = rhs;
              stmts_to_erase.push_back(i);
            }
          }
        }
      }
    }
  }

  if (coalesced_aliases.empty()) return;

  std::function<std::unique_ptr<CStatement>(std::unique_ptr<CStatement>)> rewrite_stmt =
      [&](std::unique_ptr<CStatement> s) -> std::unique_ptr<CStatement> {
    if (s == nullptr) return nullptr;
    switch (s->Kind()) {
      case CStatementKind::kCompoundStatement: {
        auto comp_stmt = static_cast<CompoundStatement*>(s.get());
        for (auto& child : comp_stmt->MutableStatements()) {
          child = rewrite_stmt(std::move(child));
        }
        return s;
      }
      case CStatementKind::kExpressionStatement: {
        const auto& es = static_cast<const ExpressionStatement&>(*s);
        auto new_expr = es.Expression().Clone();
        for (const auto& [from, to] : coalesced_aliases) {
          new_expr = SubstituteIdentifier(*new_expr, from, *CExpression::Identifier(to));
        }
        return CStatement::Expression(std::move(new_expr));
      }
      case CStatementKind::kReturnStatement: {
        const auto& ret = static_cast<const ReturnStatement&>(*s);
        if (ret.ReturnValue() != nullptr) {
          auto new_val = ret.ReturnValue()->Clone();
          for (const auto& [from, to] : coalesced_aliases) {
            new_val = SubstituteIdentifier(*new_val, from, *CExpression::Identifier(to));
          }
          return CStatement::Return(std::move(new_val));
        }
        return CStatement::Return(nullptr);
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(*s);
        auto new_cond = ifs.Condition().Clone();
        for (const auto& [from, to] : coalesced_aliases) {
          new_cond = SubstituteIdentifier(*new_cond, from, *CExpression::Identifier(to));
        }
        auto new_then = rewrite_stmt(ifs.ThenBranch().Clone());
        std::unique_ptr<CStatement> new_else = nullptr;
        if (ifs.ElseBranch() != nullptr) {
          new_else = rewrite_stmt(ifs.ElseBranch()->Clone());
        }
        return CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(*s);
        auto new_cond = ws.Condition().Clone();
        for (const auto& [from, to] : coalesced_aliases) {
          new_cond = SubstituteIdentifier(*new_cond, from, *CExpression::Identifier(to));
        }
        auto new_body = rewrite_stmt(ws.Body().Clone());
        return CStatement::While(std::move(new_cond), std::move(new_body));
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(*s);
        auto new_cond = dws.Condition().Clone();
        for (const auto& [from, to] : coalesced_aliases) {
          new_cond = SubstituteIdentifier(*new_cond, from, *CExpression::Identifier(to));
        }
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
          new_cond = fs.Condition()->Clone();
          for (const auto& [from, to] : coalesced_aliases) {
            new_cond = SubstituteIdentifier(*new_cond, from, *CExpression::Identifier(to));
          }
        }
        std::unique_ptr<CExpression> new_step = nullptr;
        if (fs.Step() != nullptr) {
          new_step = fs.Step()->Clone();
          for (const auto& [from, to] : coalesced_aliases) {
            new_step = SubstituteIdentifier(*new_step, from, *CExpression::Identifier(to));
          }
        }
        auto new_body = rewrite_stmt(fs.Body().Clone());
        return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                               std::move(new_body));
      }
      case CStatementKind::kVariableDeclarationStatement: {
        const auto& vds = static_cast<const VariableDeclarationStatement&>(*s);
        std::unique_ptr<CExpression> new_init = nullptr;
        if (vds.Initializer() != nullptr) {
          new_init = vds.Initializer()->Clone();
          for (const auto& [from, to] : coalesced_aliases) {
            new_init = SubstituteIdentifier(*new_init, from, *CExpression::Identifier(to));
          }
        }
        return CStatement::VariableDeclaration(vds.Type(), vds.Name(), std::move(new_init),
                                               vds.ArraySize());
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(*s);
        auto new_cond = ss.Condition().Clone();
        for (const auto& [from, to] : coalesced_aliases) {
          new_cond = SubstituteIdentifier(*new_cond, from, *CExpression::Identifier(to));
        }
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

  std::vector<std::unique_ptr<CStatement>> new_stmts;
  absl::flat_hash_set<size_t> erase_set(stmts_to_erase.begin(), stmts_to_erase.end());
  for (size_t i = 0; i < comp->Statements().size(); ++i) {
    if (!erase_set.contains(i)) {
      new_stmts.push_back(rewrite_stmt(std::move(comp->MutableStatements()[i])));
    }
  }
  comp->MutableStatements() = std::move(new_stmts);
}

void EliminateDeadPreCallArgAssignments(CompoundStatement* comp) {
  if (comp == nullptr) return;

  auto& stmts = comp->MutableStatements();
  std::vector<size_t> to_remove;

  for (size_t i = 0; i + 1 < stmts.size(); ++i) {
    if (stmts[i]->Kind() == CStatementKind::kExpressionStatement &&
        stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
      const auto& es1 = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
      if (es0.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(es0.Expression());
        if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
          std::string var = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
          if (absl::StartsWith(var, "arg") && std::isdigit(var.back())) {
            bool es1_has_call = false;
            std::function<void(const CExpression&)> check_call = [&](const CExpression& e) {
              if (e.Kind() == CExpressionKind::kCallExpression) {
                es1_has_call = true;
              } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
                check_call(static_cast<const UnaryExpression&>(e).Operand());
              } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
                check_call(static_cast<const BinaryExpression&>(e).Lhs());
                check_call(static_cast<const BinaryExpression&>(e).Rhs());
              } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
                check_call(static_cast<const AssignmentExpression&>(e).Rhs());
              }
            };
            check_call(es1.Expression());

            if (es1_has_call) {
              bool var_read = false;
              for (size_t j = i + 1; j < stmts.size(); ++j) {
                std::vector<std::string> vars;
                absl::flat_hash_set<std::string> seen;
                CollectUsedVariables(*stmts[j], &vars, &seen);
                if (seen.contains(var)) {
                  var_read = true;
                  break;
                }
              }
              if (!var_read) {
                to_remove.push_back(i);
              }
            }
          }
        }
      }
    }
  }

  if (!to_remove.empty()) {
    std::vector<std::unique_ptr<CStatement>> new_stmts;
    absl::flat_hash_set<size_t> erase_set(to_remove.begin(), to_remove.end());
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (!erase_set.contains(i)) {
        new_stmts.push_back(std::move(stmts[i]));
      }
    }
    stmts = std::move(new_stmts);
  }

  for (auto& s : comp->MutableStatements()) {
    ForEachSubCompoundStatement(
        s.get(), [](CompoundStatement* sub) { EliminateDeadPreCallArgAssignments(sub); });
  }
}

void FoldSingleUseExpressions(CompoundStatement* comp,
                              const CompoundStatement* root_body = nullptr) {
  if (comp == nullptr) return;
  if (root_body == nullptr) root_body = comp;

  auto count_uses = [](const CStatement& s, std::string_view var) -> int {
    int count = 0;
    std::function<void(const CExpression&)> check_expr = [&](const CExpression& e) {
      if (e.Kind() == CExpressionKind::kIdentifier) {
        if (static_cast<const IdentifierExpression&>(e).Name() == var) count++;
      } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
        check_expr(static_cast<const UnaryExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check_expr(static_cast<const BinaryExpression&>(e).Lhs());
        check_expr(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(e);
        // If LHS is not a direct identifier (e.g. *(s32*)v0 or arr[v0]), its subexpressions are
        // reads!
        if (assign.Lhs().Kind() != CExpressionKind::kIdentifier) {
          check_expr(assign.Lhs());
        }
        check_expr(assign.Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check_expr(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        check_expr(static_cast<const ArrayIndexExpression&>(e).Array());
        check_expr(static_cast<const ArrayIndexExpression&>(e).Index());
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        check_expr(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check_expr(static_cast<const TernaryExpression&>(e).Condition());
        check_expr(static_cast<const TernaryExpression&>(e).TrueExpression());
        check_expr(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        check_expr(static_cast<const CallExpression&>(e).Callee());
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check_expr(*arg);
        }
      }
    };

    std::function<void(const CStatement&)> check_stmt = [&](const CStatement& stmt) {
      switch (stmt.Kind()) {
        case CStatementKind::kExpressionStatement:
          check_expr(static_cast<const ExpressionStatement&>(stmt).Expression());
          break;
        case CStatementKind::kReturnStatement:
          if (static_cast<const ReturnStatement&>(stmt).ReturnValue()) {
            check_expr(*static_cast<const ReturnStatement&>(stmt).ReturnValue());
          }
          break;
        case CStatementKind::kIfStatement: {
          const auto& ifs = static_cast<const IfStatement&>(stmt);
          check_expr(ifs.Condition());
          check_stmt(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) check_stmt(*ifs.ElseBranch());
          break;
        }
        case CStatementKind::kWhileStatement: {
          const auto& ws = static_cast<const WhileStatement&>(stmt);
          check_expr(ws.Condition());
          check_stmt(ws.Body());
          break;
        }
        case CStatementKind::kDoWhileStatement: {
          const auto& dws = static_cast<const DoWhileStatement&>(stmt);
          check_expr(dws.Condition());
          check_stmt(dws.Body());
          break;
        }
        case CStatementKind::kForStatement: {
          const auto& fs = static_cast<const ForStatement&>(stmt);
          if (fs.Init() != nullptr) check_stmt(*fs.Init());
          if (fs.Condition() != nullptr) check_expr(*fs.Condition());
          if (fs.Step() != nullptr) check_expr(*fs.Step());
          check_stmt(fs.Body());
          break;
        }
        case CStatementKind::kCompoundStatement: {
          for (const auto& child : static_cast<const CompoundStatement&>(stmt).Statements()) {
            check_stmt(*child);
          }
          break;
        }
        case CStatementKind::kSwitchStatement: {
          const auto& ss = static_cast<const SwitchStatement&>(stmt);
          check_expr(ss.Condition());
          for (const auto& sc : ss.Cases()) {
            if (sc.body != nullptr) check_stmt(*sc.body);
          }
          break;
        }
        default:
          break;
      }
    };
    check_stmt(s);
    return count;
  };

  auto is_pure_expr = [](const CExpression& expr) {
    bool has_side_effects = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (e.Kind() == CExpressionKind::kCallExpression ||
          e.Kind() == CExpressionKind::kAssignmentExpression) {
        has_side_effects = true;
      } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
        check(static_cast<const UnaryExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check(static_cast<const BinaryExpression&>(e).Lhs());
        check(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        check(static_cast<const ArrayIndexExpression&>(e).Array());
        check(static_cast<const ArrayIndexExpression&>(e).Index());
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        check(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check(static_cast<const TernaryExpression&>(e).Condition());
        check(static_cast<const TernaryExpression&>(e).TrueExpression());
        check(static_cast<const TernaryExpression&>(e).FalseExpression());
      }
    };
    check(expr);
    return !has_side_effects;
  };

  auto has_address_of = [](const CExpression& expr) {
    bool found = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (found) return;
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        if (static_cast<const UnaryExpression&>(e).Op().find('&') != std::string::npos) {
          found = true;
          return;
        }
        check(static_cast<const UnaryExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check(static_cast<const BinaryExpression&>(e).Lhs());
        check(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        check(static_cast<const ArrayIndexExpression&>(e).Array());
        check(static_cast<const ArrayIndexExpression&>(e).Index());
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        check(static_cast<const MemberAccessExpression&>(e).Object());
      }
    };
    check(expr);
    return found;
  };

  auto has_address_of_var = [](const CExpression& expr, std::string_view var) -> bool {
    bool found = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (found) return;
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('&') != std::string::npos &&
            un.Operand().Kind() == CExpressionKind::kIdentifier &&
            static_cast<const IdentifierExpression&>(un.Operand()).Name() == var) {
          found = true;
          return;
        }
        check(un.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check(static_cast<const BinaryExpression&>(e).Lhs());
        check(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(e);
        check(assign.Lhs());
        check(assign.Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        check(static_cast<const ArrayIndexExpression&>(e).Array());
        check(static_cast<const ArrayIndexExpression&>(e).Index());
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        check(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check(static_cast<const TernaryExpression&>(e).Condition());
        check(static_cast<const TernaryExpression&>(e).TrueExpression());
        check(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        check(static_cast<const CallExpression&>(e).Callee());
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check(*arg);
        }
      }
    };
    check(expr);
    return found;
  };

  auto used_in_addition_or_subtraction = [](const CExpression& expr, std::string_view var) {
    bool in_add_sub = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (in_add_sub) return;
      if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        if (bin.Op() == "+" || bin.Op() == "-") {
          auto check_side = [&](const CExpression& side) {
            if (side.Kind() == CExpressionKind::kIdentifier &&
                static_cast<const IdentifierExpression&>(side).Name() == var) {
              in_add_sub = true;
            }
          };
          check_side(bin.Lhs());
          check_side(bin.Rhs());
        }
        check(bin.Lhs());
        check(bin.Rhs());
      } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(e);
        if (assign.Op() == "+=" || assign.Op() == "-=") {
          if (assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(assign.Rhs()).Name() == var) {
            in_add_sub = true;
          }
        }
        check(assign.Lhs());
        check(assign.Rhs());
      } else if (e.Kind() == CExpressionKind::kUnaryExpression) {
        check(static_cast<const UnaryExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        check(static_cast<const ArrayIndexExpression&>(e).Array());
        check(static_cast<const ArrayIndexExpression&>(e).Index());
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        check(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        check(static_cast<const TernaryExpression&>(e).Condition());
        check(static_cast<const TernaryExpression&>(e).TrueExpression());
        check(static_cast<const TernaryExpression&>(e).FalseExpression());
      } else if (e.Kind() == CExpressionKind::kCallExpression) {
        check(static_cast<const CallExpression&>(e).Callee());
        for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
          check(*arg);
        }
      }
    };
    check(expr);
    return in_add_sub;
  };

  auto count_expr_uses = [](const CExpression& e, std::string_view var) -> int {
    int count = 0;
    std::function<void(const CExpression&)> check_expr = [&](const CExpression& expr) {
      if (expr.Kind() == CExpressionKind::kIdentifier) {
        if (static_cast<const IdentifierExpression&>(expr).Name() == var) count++;
      } else if (expr.Kind() == CExpressionKind::kUnaryExpression) {
        check_expr(static_cast<const UnaryExpression&>(expr).Operand());
      } else if (expr.Kind() == CExpressionKind::kBinaryExpression) {
        check_expr(static_cast<const BinaryExpression&>(expr).Lhs());
        check_expr(static_cast<const BinaryExpression&>(expr).Rhs());
      } else if (expr.Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& assign = static_cast<const AssignmentExpression&>(expr);
        if (assign.Lhs().Kind() != CExpressionKind::kIdentifier) {
          check_expr(assign.Lhs());
        }
        check_expr(assign.Rhs());
      } else if (expr.Kind() == CExpressionKind::kCastExpression) {
        check_expr(static_cast<const CastExpression&>(expr).Operand());
      } else if (expr.Kind() == CExpressionKind::kArrayIndexExpression) {
        check_expr(static_cast<const ArrayIndexExpression&>(expr).Array());
        check_expr(static_cast<const ArrayIndexExpression&>(expr).Index());
      } else if (expr.Kind() == CExpressionKind::kMemberAccessExpression) {
        check_expr(static_cast<const MemberAccessExpression&>(expr).Object());
      } else if (expr.Kind() == CExpressionKind::kTernaryExpression) {
        check_expr(static_cast<const TernaryExpression&>(expr).Condition());
        check_expr(static_cast<const TernaryExpression&>(expr).TrueExpression());
        check_expr(static_cast<const TernaryExpression&>(expr).FalseExpression());
      } else if (expr.Kind() == CExpressionKind::kCallExpression) {
        check_expr(static_cast<const CallExpression&>(expr).Callee());
        for (const auto& arg : static_cast<const CallExpression&>(expr).Arguments()) {
          check_expr(*arg);
        }
      }
    };
    check_expr(e);
    return count;
  };

  auto is_scratch_var = [](std::string_view var) -> bool {
    return var == "v0" || var == "v1" || var == "at" || var == "temp_at" || var == "f0" ||
           var == "f0_d" || absl::StartsWith(var, "temp_t") ||
           (var.size() >= 2 && var[0] == 't' && std::isdigit(var[1]));
  };

  bool changed = true;
  while (changed) {
    changed = false;
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i + 1 < stmts.size(); ++i) {
      if (stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es0 = static_cast<const ExpressionStatement&>(*stmts[i]);
        if (es0.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& assign = static_cast<const AssignmentExpression&>(es0.Expression());
          if (assign.Op() == "=" && assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
            std::string var = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
            if (!is_scratch_var(var)) continue;

            // Ensure assign.Rhs() itself doesn't contain var
            if (count_expr_uses(assign.Rhs(), var) > 0) continue;

            // 1. Fold into ExpressionStatement
            if (stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
              const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);

              // Case A: In-place redefinition (var = ...; var = ... var ...)
              if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                const auto& next_assign =
                    static_cast<const AssignmentExpression&>(next_es.Expression());
                if (next_assign.Op() == "=" &&
                    next_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                    static_cast<const IdentifierExpression&>(next_assign.Lhs()).Name() == var) {
                  bool is_shift_add = false;
                  bool assign_has_shift =
                      (assign.Rhs().Kind() == CExpressionKind::kBinaryExpression &&
                       static_cast<const BinaryExpression&>(assign.Rhs()).Op() == "<<");
                  if (next_assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                    std::string op = static_cast<const BinaryExpression&>(next_assign.Rhs()).Op();
                    if (op == "<<" || (assign_has_shift && (op == "+" || op == "-"))) {
                      is_shift_add = true;
                    }
                  }
                  if (is_shift_add && count_expr_uses(next_assign.Rhs(), var) == 1 &&
                      (!has_address_of(assign.Rhs()) ||
                       !used_in_addition_or_subtraction(next_assign.Rhs(), var))) {
                    auto new_rhs = SubstituteIdentifier(next_assign.Rhs(), var, assign.Rhs());
                    stmts[i + 1] = CStatement::Expression(CExpression::Assignment(
                        "=", next_assign.Lhs().Clone(), std::move(new_rhs)));
                    stmts.erase(stmts.begin() + i);
                    changed = true;
                    break;
                  }
                  continue;
                }
              }

              // Case B: Regular single-use folding into next expression statement
              if (var != "f0" && var != "f0_d" && is_pure_expr(assign.Rhs())) {
                if (has_address_of(assign.Rhs()) &&
                    used_in_addition_or_subtraction(next_es.Expression(), var)) {
                  continue;
                }

                if (has_address_of_var(next_es.Expression(), var) &&
                    assign.Rhs().Kind() != CExpressionKind::kIdentifier) {
                  continue;
                }

                if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& next_assign =
                      static_cast<const AssignmentExpression&>(next_es.Expression());
                  if (next_assign.Lhs().Kind() != CExpressionKind::kIdentifier) {
                    if (count_expr_uses(next_assign.Lhs(), var) > 0) continue;
                  }
                }

                int uses_in_next = count_uses(*stmts[i + 1], var);
                if (uses_in_next == 1) {
                  if (count_uses(*root_body, var) == 1) {
                    stmts[i + 1] = CStatement::Expression(
                        SubstituteIdentifier(next_es.Expression(), var, assign.Rhs()));
                    stmts.erase(stmts.begin() + i);
                    changed = true;
                    break;
                  }
                }
              }
            }

            // 2. Fold into IfStatement
            if (stmts[i + 1]->Kind() == CStatementKind::kIfStatement) {
              if (var == "f0" || var == "f0_d") {
                if (assign.Rhs().Kind() != CExpressionKind::kCallExpression) {
                  continue;
                }
              }
              const auto& if_stmt = static_cast<const IfStatement&>(*stmts[i + 1]);
              if (has_address_of_var(if_stmt.Condition(), var) &&
                  assign.Rhs().Kind() != CExpressionKind::kIdentifier) {
                continue;
              }
              int uses_in_cond = count_expr_uses(if_stmt.Condition(), var);
              int uses_in_then = count_uses(if_stmt.ThenBranch(), var);
              int uses_in_else =
                  (if_stmt.ElseBranch() != nullptr) ? count_uses(*if_stmt.ElseBranch(), var) : 0;
              if (uses_in_cond == 1 && uses_in_then == 0 && uses_in_else == 0) {
                if (count_uses(*root_body, var) == 1) {
                  auto new_cond = SubstituteIdentifier(if_stmt.Condition(), var, assign.Rhs());
                  auto new_then = if_stmt.ThenBranch().Clone();
                  std::unique_ptr<CStatement> new_else =
                      (if_stmt.ElseBranch() != nullptr) ? if_stmt.ElseBranch()->Clone() : nullptr;
                  stmts[i + 1] =
                      CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
                  stmts.erase(stmts.begin() + i);
                  changed = true;
                  break;
                }
              }
            }

            // 3. Fold into ReturnStatement
            if (stmts[i + 1]->Kind() == CStatementKind::kReturnStatement) {
              const auto& ret_stmt = static_cast<const ReturnStatement&>(*stmts[i + 1]);
              if (ret_stmt.ReturnValue() != nullptr &&
                  ret_stmt.ReturnValue()->Kind() == CExpressionKind::kIdentifier &&
                  static_cast<const IdentifierExpression&>(*ret_stmt.ReturnValue()).Name() == var) {
                if (count_uses(*root_body, var) == 1) {
                  stmts[i + 1] = CStatement::Return(assign.Rhs().Clone());
                  stmts.erase(stmts.begin() + i);
                  changed = true;
                  break;
                }
              }
            }

            // 4. Fold into SwitchStatement
            if (stmts[i + 1]->Kind() == CStatementKind::kSwitchStatement) {
              const auto& sw_stmt = static_cast<const SwitchStatement&>(*stmts[i + 1]);
              if (has_address_of_var(sw_stmt.Condition(), var) &&
                  assign.Rhs().Kind() != CExpressionKind::kIdentifier) {
                continue;
              }
              int uses_in_cond = count_expr_uses(sw_stmt.Condition(), var);
              int uses_in_cases = 0;
              for (const auto& sc : sw_stmt.Cases()) {
                if (sc.body != nullptr) {
                  uses_in_cases += count_uses(*sc.body, var);
                }
              }
              if (uses_in_cond == 1 && uses_in_cases == 0) {
                if (count_uses(*root_body, var) == 1) {
                  auto new_cond = SubstituteIdentifier(sw_stmt.Condition(), var, assign.Rhs());
                  std::vector<SwitchCase> new_cases;
                  for (const auto& sc : sw_stmt.Cases()) {
                    new_cases.push_back(sc.Clone());
                  }
                  stmts[i + 1] = CStatement::Switch(std::move(new_cond), std::move(new_cases));
                  stmts.erase(stmts.begin() + i);
                  changed = true;
                  break;
                }
              }
            }
          }
        }
      }
    }
  }

  for (auto& s : comp->MutableStatements()) {
    ForEachSubCompoundStatement(
        s.get(), [root_body](CompoundStatement* sub) { FoldSingleUseExpressions(sub, root_body); });
  }
}

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
        if (out_val) *out_val = static_cast<const IntegerLiteralExpression&>(e).Value();
        return true;
      }
      return false;
    };

    auto exprs_match = [](const CExpression& a, const CExpression& b) -> bool {
      return a.ToString() == b.ToString();
    };

    // 1. (X << S) + X or X + (X << S)
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
      }
    }

    // 2. (X << 3) - X  ->  X * 7
    if (bin.Op() == "-") {
      int64_t shift_val = 0;
      if (bin.Lhs().Kind() == CExpressionKind::kBinaryExpression) {
        const auto& lhs_bin = static_cast<const BinaryExpression&>(bin.Lhs());
        if (lhs_bin.Op() == "<<" && is_int_val(lhs_bin.Rhs(), &shift_val)) {
          if (shift_val == 3 && exprs_match(lhs_bin.Lhs(), bin.Rhs())) {
            return CExpression::Binary("*", lhs_bin.Lhs().Clone(), CExpression::Integer(7));
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
  }

  return rewritten;
}

void SimplifyShiftAddMultiplication(CompoundStatement* comp) {
  if (comp == nullptr) return;

  std::function<std::unique_ptr<CStatement>(std::unique_ptr<CStatement>)> rewrite_stmt =
      [&](std::unique_ptr<CStatement> s) -> std::unique_ptr<CStatement> {
    if (s == nullptr) return nullptr;
    switch (s->Kind()) {
      case CStatementKind::kCompoundStatement: {
        auto c = static_cast<CompoundStatement*>(s.get());
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

}  // namespace

absl::StatusOr<FunctionDeclaration> AstConverter::Convert(const ControlFlowGraph& cfg,
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
    returns_f0 = ExpressionBuilder::DetermineReturnsF0(cfg);
    if (!returns_f0 && options.functions_returning_f0 != nullptr &&
        options.functions_returning_f0->contains(options.function_name)) {
      returns_f0 = true;
    }
    if (!returns_f0) {
      returns_v0 =
          ExpressionBuilder::DetermineReturnsV0(cfg, symbol_index, options.functions_returning_v0);
      if (!returns_v0 && options.functions_returning_v0 != nullptr &&
          options.functions_returning_v0->contains(options.function_name)) {
        returns_v0 = true;
      }
    }
  }

  ConverterContext ctx(cfg, symbol_index, root_region, options.game_config,
                       options.function_parameter_counts, options.function_fp_parameter_counts,
                       returns_v0, returns_f0, options.functions_returning_v0,
                       options.functions_returning_f0, options.function_name);
  ctx.ConvertRegion(root_region, body.get());

  for (uint32_t target_block_id : ctx.GotoTargets()) {
    if (!ctx.EmittedLabels().contains(target_block_id)) {
      body->AddStatement(CStatement::Label(absl::StrFormat("block_%d", target_block_id)));
      body->AddStatement(CStatement::Expression(CExpression::Integer(0)));
    }
  }

  SimplifyReturnStatements(body.get(), returns_v0, returns_f0);

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

    int fp_param_count = 0;
    if (options.function_fp_parameter_counts != nullptr) {
      auto it = options.function_fp_parameter_counts->find(options.function_name);
      if (it != options.function_fp_parameter_counts->end()) {
        fp_param_count = it->second;
      }
    }
    if (fp_param_count == 0 && !all_instructions.empty()) {
      fp_param_count = ExpressionBuilder::DetermineFpParameterCount(all_instructions);
    }

    int total_param_count = std::max(parameter_count, fp_param_count);

    auto float_parameters_in_general_registers =
        AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
    int slot = 0;
    int fp_idx = 0;
    while (slot < total_param_count || fp_idx < fp_param_count) {
      if (fp_idx < fp_param_count) {
        std::string fp_name = (fp_idx == 0) ? "f12" : "f14";
        FpRegister fpr = (fp_idx == 0) ? FpRegister::kF12 : FpRegister::kF14;
        bool is_double = IsFpParameterDouble(cfg, fpr);
        CType t = is_double ? CType::F64() : CType::F32();
        if (is_double && (slot % 2 != 0)) {
          slot++;
        }
        parameters.push_back(CParameter{.type = t, .name = is_double ? (fp_name + "_d") : fp_name});
        slot += (is_double ? 2 : 1);
        fp_idx++;
      } else {
        Register reg = static_cast<Register>(static_cast<int>(Register::kA0) + slot);
        CType param_type = (slot < 4 && float_parameters_in_general_registers.contains(reg))
                               ? CType::F32()
                               : CType::S32();
        parameters.push_back(
            CParameter{.type = param_type, .name = absl::StrFormat("arg%d", slot)});
        slot++;
      }
    }
  }

  // Determine return type
  CType return_type = CType::Void();
  if (options.return_type.has_value()) {
    return_type = *options.return_type;
  } else if (returns_f0) {
    bool is_double = ExpressionBuilder::DetermineReturnsF0Double(cfg);
    return_type = is_double ? CType::F64() : CType::F32();
  } else if (returns_v0) {
    return_type = CType::S32();
  } else if (HasReturnValue(*body)) {
    bool body_returns_f0 = false;
    for (const auto& stmt : body->Statements()) {
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
      bool is_double = ExpressionBuilder::DetermineReturnsF0Double(cfg);
      return_type = is_double ? CType::F64() : CType::F32();
    } else {
      return_type = CType::S32();
    }
  }

  // Run AST post-processing
  CoalescePrologueParameterCopies(body.get(), parameters);
  EliminateDeadPreCallArgAssignments(body.get());
  SimplifyShiftAddMultiplication(body.get());
  FoldSingleUseExpressions(body.get());
  SimplifyShiftAddMultiplication(body.get());

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

    // Do not declare globals (prefixed with g_, D_, RO_, B_)
    if (absl::StartsWith(var_name, "g_") || absl::StartsWith(var_name, "D_") ||
        absl::StartsWith(var_name, "RO_") || absl::StartsWith(var_name, "B_")) {
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
    // Do not declare synthesized or auto function names (func_XXXXXXXX)
    uint32_t auto_addr = 0;
    if (var_name.size() == 13 && absl::StartsWith(var_name, "func_") &&
        absl::SimpleHexAtoi(var_name.substr(5), &auto_addr)) {
      continue;
    }
    // Do not declare known functions from pipeline options
    if (options.function_parameter_counts != nullptr &&
        options.function_parameter_counts->contains(var_name)) {
      continue;
    }
    if (options.function_fp_parameter_counts != nullptr &&
        options.function_fp_parameter_counts->contains(var_name)) {
      continue;
    }
    if (options.functions_returning_v0 != nullptr &&
        options.functions_returning_v0->contains(var_name)) {
      continue;
    }
    if (options.functions_returning_f0 != nullptr &&
        options.functions_returning_f0->contains(var_name)) {
      continue;
    }

    local_vars_to_declare.push_back(var_name);
  }

  if (!local_vars_to_declare.empty()) {
    if (ctx.SpAddressTaken() && ctx.FrameSize() == 0) {
      return absl::InternalError(
          absl::StrFormat("Frame analysis failed for function '%s': sp address taken as buffer, "
                          "but no stack allocation detected.",
                          options.function_name));
    }
    uint32_t sp_buffer_size = ctx.SpBufferSize();

    const absl::flat_hash_set<std::string>& double_vars = ctx.DoubleVars();

    auto is_fp_reg = [](std::string_view name) -> bool {
      if (name.size() >= 2 && name[0] == 'f' && std::isdigit(name[1])) {
        for (size_t i = 1; i < name.size(); ++i) {
          if (!std::isdigit(name[i])) {
            return i == name.size() - 2 && name[i] == '_' && name[i + 1] == 'd';
          }
        }
        return true;
      }
      return false;
    };

    absl::flat_hash_set<std::string> vars_needing_init;
    constexpr uint32_t kAllGprs = 0xFFFFFFFFu;
    uint32_t entry_defs = (1u << static_cast<size_t>(Register::kZero));
    for (size_t pi = 0; pi < parameters.size() && pi < 4; ++pi) {
      if (parameters[pi].name == "arg0" || pi == 0) {
        entry_defs |= (1u << static_cast<size_t>(Register::kA0));
      }
      if (parameters[pi].name == "arg1" || pi == 1) {
        entry_defs |= (1u << static_cast<size_t>(Register::kA1));
      }
      if (parameters[pi].name == "arg2" || pi == 2) {
        entry_defs |= (1u << static_cast<size_t>(Register::kA2));
      }
      if (parameters[pi].name == "arg3" || pi == 3) {
        entry_defs |= (1u << static_cast<size_t>(Register::kA3));
      }
    }

    auto transfer_block = [](const BasicBlock& block, uint32_t in_defs) -> uint32_t {
      uint32_t cur = in_defs;
      for (const auto& inst : block.instructions) {
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register d : ud.gpr_defs) {
          if (d != Register::kZero) {
            cur |= (1u << static_cast<size_t>(d));
          }
        }
        if (inst.IsCall()) {
          cur |= (1u << static_cast<size_t>(Register::kV0));
          cur |= (1u << static_cast<size_t>(Register::kRa));
        }
      }
      return cur;
    };

    std::vector<uint32_t> rpo = cfg.ReversePostOrder();
    absl::flat_hash_set<uint32_t> reachable_blocks(rpo.begin(), rpo.end());
    absl::flat_hash_map<uint32_t, uint32_t> def_in;
    absl::flat_hash_map<uint32_t, uint32_t> def_out;

    for (uint32_t b_id : rpo) {
      if (b_id == cfg.EntryBlockId()) {
        def_in[b_id] = entry_defs;
        const auto* eb = cfg.GetBlock(b_id);
        def_out[b_id] = (eb != nullptr) ? transfer_block(*eb, entry_defs) : entry_defs;
      } else {
        def_in[b_id] = kAllGprs;
        def_out[b_id] = kAllGprs;
      }
    }

    bool changed = true;
    int iteration = 0;
    while (changed && iteration < 100) {
      changed = false;
      iteration++;
      for (uint32_t b_id : rpo) {
        if (b_id == cfg.EntryBlockId()) continue;
        const auto* block = cfg.GetBlock(b_id);
        if (block == nullptr) continue;

        uint32_t new_in = kAllGprs;
        int valid_preds = 0;
        for (uint32_t pred_id : block->predecessors) {
          if (!reachable_blocks.contains(pred_id)) continue;
          auto it = def_out.find(pred_id);
          if (it != def_out.end()) {
            new_in &= it->second;
            valid_preds++;
          }
        }
        if (valid_preds == 0) {
          new_in = entry_defs;
        }

        def_in[b_id] = new_in;
        uint32_t new_out = transfer_block(*block, new_in);
        if (def_out[b_id] != new_out) {
          def_out[b_id] = new_out;
          changed = true;
        }
      }
    }

    for (uint32_t b_id : rpo) {
      const auto* block = cfg.GetBlock(b_id);
      if (block == nullptr) continue;

      uint32_t cur = def_in[b_id];
      for (const auto& inst : block->instructions) {
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register u : ud.gpr_uses) {
          if (u != Register::kZero) {
            if ((cur & (1u << static_cast<size_t>(u))) == 0) {
              vars_needing_init.insert(ExpressionBuilder::RegisterVarName(u));
            }
          }
        }
        for (Register d : ud.gpr_defs) {
          if (d != Register::kZero) {
            cur |= (1u << static_cast<size_t>(d));
          }
        }
        if (inst.IsCall()) {
          cur |= (1u << static_cast<size_t>(Register::kV0));
          cur |= (1u << static_cast<size_t>(Register::kRa));
        }
      }
    }

    auto new_body = std::make_unique<CompoundStatement>();
    for (const auto& var_name : local_vars_to_declare) {
      if (var_name == "sp") {
        new_body->AddStatement(
            CStatement::VariableDeclaration(CType::U8(), "sp", nullptr, sp_buffer_size));
      } else if (is_fp_reg(var_name)) {
        if (absl::EndsWith(var_name, "_d") || double_vars.contains(var_name)) {
          new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name));
        } else {
          new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name));
        }
      } else if (absl::EndsWith(var_name, "_64")) {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::Named("s64"), var_name));
      } else if (double_vars.contains(var_name)) {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name));
      } else {
        auto init_expr = vars_needing_init.contains(var_name) ? CExpression::Integer(0) : nullptr;
        new_body->AddStatement(
            CStatement::VariableDeclaration(CType::S32(), var_name, std::move(init_expr)));
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

void AstConverter::CollectUsedVariables(const CStatement& stmt, std::vector<std::string>* vars,
                                        absl::flat_hash_set<std::string>* seen) {
  rom_nom_nom::CollectUsedVariables(stmt, vars, seen);
}

void AstConverter::SimplifyReturnStatements(CompoundStatement* comp, bool returns_v0,
                                            bool returns_f0) {
  rom_nom_nom::SimplifyReturnStatements(comp, returns_v0, returns_f0);
}

}  // namespace rom_nom_nom
