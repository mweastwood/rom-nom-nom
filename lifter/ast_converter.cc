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
      std::string name = id.Name();
      if (absl::StartsWith(name, "((") && absl::EndsWith(name, ")") && name.size() > 4) {
        size_t inner_close = name.rfind(')', name.size() - 2);
        if (inner_close != std::string::npos && inner_close + 1 < name.size() - 1) {
          name = name.substr(inner_close + 1, name.size() - inner_close - 2);
        }
      }
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
      if (call.Callee().Kind() != CExpressionKind::kIdentifier ||
          absl::StartsWith(static_cast<const IdentifierExpression&>(call.Callee()).Name(), "((")) {
        CollectVariablesInExpression(call.Callee(), vars, seen);
      }
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
    const auto& id = static_cast<const IdentifierExpression&>(expr);
    if (id.Name() == target) {
      return replacement.Clone();
    }
    if (replacement.Kind() == CExpressionKind::kIdentifier && absl::StartsWith(id.Name(), "((") &&
        absl::EndsWith(id.Name(), ")")) {
      std::string suffix = absl::StrCat(")", target, ")");
      if (absl::EndsWith(id.Name(), suffix)) {
        std::string prefix = id.Name().substr(0, id.Name().size() - target.size() - 1);
        const auto& rep_id = static_cast<const IdentifierExpression&>(replacement);
        return CExpression::Identifier(absl::StrCat(prefix, rep_id.Name(), ")"));
      }
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
    bool is_fp_bitcast_op = (un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&") &&
                            target.size() >= 2 && target[0] == 'f' &&
                            std::isdigit(static_cast<unsigned char>(target[1])) &&
                            replacement.Kind() != CExpressionKind::kFloatLiteral;
    if (un.Op().find('&') != std::string::npos &&
        replacement.Kind() != CExpressionKind::kIdentifier && !is_fp_bitcast_op) {
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
            AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg_)),
        float_stack_param_slots_(AbiAnalyzer::DetectFloatStackParameterSlots(cfg_)),
        float_local_stack_offsets_(AbiAnalyzer::DetectFloatLocalStackOffsets(cfg_)) {
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
          int branch_index = -1;
          Register comparison_register = Register::kZero;
          for (int i = static_cast<int>(bounds_block->instructions.size()) - 1; i >= 0; --i) {
            const auto& inst = bounds_block->instructions[i];
            if (inst.vram == jt.bounds_branch_vram &&
                (inst.opcode == Opcode::kBeq || inst.opcode == Opcode::kBeql)) {
              Register r1 = inst.rs.value_or(Register::kZero);
              Register r2 = inst.rt.value_or(Register::kZero);
              if (r2 == Register::kZero && r1 != Register::kZero) {
                comparison_register = r1;
                branch_index = i;
              } else if (r1 == Register::kZero && r2 != Register::kZero) {
                comparison_register = r2;
                branch_index = i;
              }
              break;
            }
          }
          if (branch_index > 0 && comparison_register != Register::kZero) {
            int sltiu_index = -1;
            Register normalized_index_register = Register::kZero;
            for (int i = branch_index - 1; i >= 0; --i) {
              const auto& inst = bounds_block->instructions[i];
              if ((inst.opcode == Opcode::kSltiu || inst.opcode == Opcode::kSlti) &&
                  inst.rt == comparison_register &&
                  static_cast<uint32_t>(inst.immediate) == jt.num_cases) {
                setup_instruction_vrams_.insert(inst.vram);
                sltiu_index = i;
                normalized_index_register = inst.rs.value_or(Register::kZero);
                break;
              }
            }
            if (sltiu_index > 0 && jt.lower_bound > 0 &&
                normalized_index_register != Register::kZero &&
                normalized_index_register != jt.index_register) {
              for (int i = sltiu_index - 1; i >= 0; --i) {
                const auto& inst = bounds_block->instructions[i];
                if (inst.opcode == Opcode::kAddiu && inst.rt == normalized_index_register &&
                    inst.rs == jt.index_register &&
                    -static_cast<int64_t>(inst.immediate) == jt.lower_bound) {
                  bool used_in_other_blocks = false;
                  for (const auto& other_block : cfg_.Blocks()) {
                    if (other_block.id == bounds_block->id) continue;
                    for (const auto& other_inst : other_block.instructions) {
                      if (setup_instruction_vrams_.contains(other_inst.vram) ||
                          other_inst.vram == jt.switch_vram) {
                        continue;
                      }
                      RegisterUseDef ud = GetInstructionUseDef(other_inst);
                      bool reads_norm = false;
                      for (Register u : ud.gpr_uses) {
                        if (u == normalized_index_register) {
                          reads_norm = true;
                          break;
                        }
                      }
                      if (reads_norm) {
                        used_in_other_blocks = true;
                        break;
                      }
                      bool writes_norm = false;
                      for (Register d : ud.gpr_defs) {
                        if (d == normalized_index_register) {
                          writes_norm = true;
                          break;
                        }
                      }
                      if (writes_norm || other_inst.IsCall()) {
                        break;
                      }
                    }
                    if (used_in_other_blocks) break;
                  }
                  if (!used_in_other_blocks) {
                    setup_instruction_vrams_.insert(inst.vram);
                  }
                  break;
                }
              }
            }
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
  uint32_t OutgoingArgsSize() const { return frame_analysis_.outgoing_args_size; }
  uint32_t SpBufferSize() const { return frame_analysis_.sp_buffer_size; }
  const absl::flat_hash_set<int32_t>& StackStoreOffsets() const {
    return frame_analysis_.stack_store_offsets;
  }
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
    ctx.float_stack_param_slots = &float_stack_param_slots_;
    ctx.float_local_stack_offsets = &float_local_stack_offsets_;
    ctx.outgoing_args_size = frame_analysis_.outgoing_args_size;
    ctx.sp_buffer_size = frame_analysis_.sp_buffer_size;
    ctx.cfg = &cfg_;
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
          auto delay_stmts = LiftDelaySlotFromBlock(*cond_block);
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
          auto delay_stmts = LiftDelaySlotFromBlock(*cond_block);
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
          auto delay_stmts = LiftDelaySlotFromBlock(*cond_block);
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
          auto delay_stmts = LiftDelaySlotFromBlock(*cond_block);
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
                auto delay_stmts = LiftDelaySlotFromBlock(*cond_block);
                if (peeled_loop_delays_.contains(cond_block_id)) {
                  if (!peeled_loop_header_emitted_.contains(region.loop_header)) {
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
                  }
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
                  peeled_loop_header_insts_[region.loop_header] = delay_inst;
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
    auto it_peeled_hdr = peeled_loop_header_insts_.find(block_id);
    if (it_peeled_hdr != peeled_loop_header_insts_.end()) {
      filtered_block.instructions.insert(filtered_block.instructions.begin(),
                                         it_peeled_hdr->second);
      peeled_loop_header_emitted_.insert(block_id);
    }
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

  static void StepBlockState(BlockState& state, const Instruction& inst) {
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
        if (tv.kind == ValueKind::kConstant || tv.kind == ValueKind::kSymbolAddress) {
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
      for (int t = static_cast<int>(Register::kT0); t <= static_cast<int>(Register::kT7); ++t) {
        state.tracker.Invalidate(static_cast<Register>(t));
      }
      state.tracker.Invalidate(Register::kT8);
      state.tracker.Invalidate(Register::kT9);
    }
  }

  std::vector<LiftedStatement> LiftDelaySlotFromBlock(const BasicBlock& block) const {
    if (block.instructions.empty()) return {};
    BlockState state;
    auto it_state = block_entry_states_.find(block.id);
    if (it_state != block_entry_states_.end()) {
      state = it_state->second;
    }
    for (size_t i = 0; i + 1 < block.instructions.size(); ++i) {
      StepBlockState(state, block.instructions[i]);
    }
    const auto& delay_inst = block.instructions.back();
    return ExpressionBuilder::LiftInstructions(
        absl::MakeSpan(&delay_inst, 1), BuildLiftContext(&state.fpr_const_bits, &state.tracker),
        /*omit_last_instruction=*/false);
  }

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
        if (block_id == cfg_.EntryBlockId() && cfg_.Blocks().size() > 1) {
          bool entry_has_call = false;
          for (const auto& inst : block->instructions) {
            if (inst.IsCall()) {
              entry_has_call = true;
              break;
            }
          }
          if (!entry_has_call) {
            for (Register reg : {Register::kA0, Register::kA1, Register::kA2, Register::kA3}) {
              if (!AbiAnalyzer::IsRegisterLiveIn(cfg_, reg) ||
                  float_parameters_in_general_registers_.contains(reg)) {
                continue;
              }
              bool used_directly_in_entry = false;
              for (const auto& inst : block->instructions) {
                bool is_callee_saved_copy =
                    (inst.opcode == Opcode::kAddu && inst.rt == Register::kZero &&
                     inst.rd.has_value() && *inst.rd >= Register::kS0 && *inst.rd <= Register::kS7);
                if (is_callee_saved_copy) {
                  continue;
                }
                RegisterUseDef ud = GetInstructionUseDef(inst);
                for (Register u : ud.gpr_uses) {
                  if (u == reg) {
                    used_directly_in_entry = true;
                    break;
                  }
                }
                if (used_directly_in_entry) break;
                bool overwritten = false;
                for (Register d : ud.gpr_defs) {
                  if (d == reg) {
                    overwritten = true;
                    break;
                  }
                }
                if (overwritten) break;
              }
              if (used_directly_in_entry) {
                entry_state.tracker.SetEntryParameter(reg);
              }
            }
          }
        }
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
          StepBlockState(state, inst);
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
            auto prev_def = it_prev->second.tracker.GetReachingDefinition(reg);
            auto next_def = exit_state.tracker.GetReachingDefinition(reg);
            bool same_def = prev_def.has_value() == next_def.has_value() &&
                            (!prev_def.has_value() ||
                             (prev_def->vram == next_def->vram &&
                              prev_def->instruction_index == next_def->instruction_index &&
                              prev_def->reg == next_def->reg));
            if (!same_def) {
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
  absl::flat_hash_set<int> float_stack_param_slots_;
  absl::flat_hash_set<int32_t> float_local_stack_offsets_;
  std::optional<uint32_t> current_switch_break_block_;
  absl::flat_hash_map<uint32_t, CallReturnInfo> call_return_info_;
  absl::flat_hash_map<uint32_t, Instruction> peeled_loop_delays_;
  absl::flat_hash_map<uint32_t, Instruction> peeled_loop_header_insts_;
  absl::flat_hash_set<uint32_t> peeled_loop_header_emitted_;
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

void SimplifyDecoupledDelaySlotEarlyReturns(CompoundStatement* comp) {
  if (comp == nullptr) return;

  for (auto& s : comp->MutableStatements()) {
    ForEachSubCompoundStatement(
        s.get(), [](CompoundStatement* sub) { SimplifyDecoupledDelaySlotEarlyReturns(sub); });
  }

  auto get_id_name = [](const CExpression& expr) -> std::string {
    if (expr.Kind() == CExpressionKind::kIdentifier) {
      return static_cast<const IdentifierExpression&>(expr).Name();
    }
    return "";
  };

  std::function<std::unique_ptr<CStatement>(const IfStatement&, std::string_view,
                                            const CExpression&)>
      try_extract = [&](const IfStatement& if_s, std::string_view var_name,
                        const CExpression& default_val) -> std::unique_ptr<CStatement> {
    if (absl::StrContains(if_s.Condition().ToString(), var_name)) return nullptr;
    if (if_s.ThenBranch().Kind() != CStatementKind::kCompoundStatement ||
        if_s.ElseBranch() == nullptr ||
        if_s.ElseBranch()->Kind() != CStatementKind::kCompoundStatement) {
      return nullptr;
    }
    const auto& then_comp = static_cast<const CompoundStatement&>(if_s.ThenBranch());
    const auto& else_comp = static_cast<const CompoundStatement&>(*if_s.ElseBranch());
    if (then_comp.Statements().size() != 2 || else_comp.Statements().size() != 1) {
      return nullptr;
    }
    if (then_comp.Statements()[0]->Kind() != CStatementKind::kExpressionStatement ||
        else_comp.Statements()[0]->Kind() != CStatementKind::kExpressionStatement) {
      return nullptr;
    }
    const auto& then_es0 = static_cast<const ExpressionStatement&>(*then_comp.Statements()[0]);
    const auto& else_es0 = static_cast<const ExpressionStatement&>(*else_comp.Statements()[0]);
    if (then_es0.Expression().Kind() != CExpressionKind::kAssignmentExpression ||
        else_es0.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
      return nullptr;
    }
    const auto& then_a0 = static_cast<const AssignmentExpression&>(then_es0.Expression());
    const auto& else_a0 = static_cast<const AssignmentExpression&>(else_es0.Expression());
    if (then_a0.Op() != "=" || else_a0.Op() != "=" || get_id_name(then_a0.Lhs()) != var_name ||
        get_id_name(else_a0.Lhs()) != var_name ||
        then_a0.Rhs().ToString() != default_val.ToString() ||
        else_a0.Rhs().ToString() != default_val.ToString()) {
      return nullptr;
    }

    const CStatement& tail_stmt = *then_comp.Statements()[1];
    if (tail_stmt.Kind() == CStatementKind::kExpressionStatement) {
      const auto& tail_es = static_cast<const ExpressionStatement&>(tail_stmt);
      if (tail_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
        const auto& tail_a = static_cast<const AssignmentExpression&>(tail_es.Expression());
        if (tail_a.Op() == "=" && get_id_name(tail_a.Lhs()) == var_name &&
            !absl::StrContains(tail_a.Rhs().ToString(), var_name)) {
          auto new_then = std::make_unique<CompoundStatement>();
          new_then->AddStatement(CStatement::Return(tail_a.Rhs().Clone()));
          return CStatement::If(if_s.Condition().Clone(), std::move(new_then));
        }
      }
    }
    if (tail_stmt.Kind() == CStatementKind::kIfStatement) {
      auto extracted_nested =
          try_extract(static_cast<const IfStatement&>(tail_stmt), var_name, default_val);
      if (extracted_nested != nullptr) {
        auto new_then = std::make_unique<CompoundStatement>();
        new_then->AddStatement(std::move(extracted_nested));
        return CStatement::If(if_s.Condition().Clone(), std::move(new_then));
      }
    }
    if (!absl::StrContains(tail_stmt.ToString(), var_name)) {
      auto new_then = std::make_unique<CompoundStatement>();
      new_then->AddStatement(tail_stmt.Clone());
      return CStatement::If(if_s.Condition().Clone(), std::move(new_then));
    }
    return nullptr;
  };

  std::vector<std::unique_ptr<CStatement>> new_stmts;
  auto& old_stmts = comp->MutableStatements();
  for (size_t i = 0; i < old_stmts.size(); ++i) {
    if (i + 1 < old_stmts.size() && old_stmts[i]->Kind() == CStatementKind::kIfStatement &&
        old_stmts[i + 1]->Kind() == CStatementKind::kReturnStatement) {
      const auto& if_s = static_cast<const IfStatement&>(*old_stmts[i]);
      const auto& ret_s = static_cast<const ReturnStatement&>(*old_stmts[i + 1]);
      if (ret_s.ReturnValue() != nullptr) {
        std::string var_name = get_id_name(*ret_s.ReturnValue());
        if ((var_name == "v0" || var_name == "f0" || var_name == "f0_d") &&
            if_s.ElseBranch() != nullptr &&
            if_s.ElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          const auto& else_comp = static_cast<const CompoundStatement&>(*if_s.ElseBranch());
          if (else_comp.Statements().size() == 1 &&
              else_comp.Statements()[0]->Kind() == CStatementKind::kExpressionStatement) {
            const auto& else_es0 =
                static_cast<const ExpressionStatement&>(*else_comp.Statements()[0]);
            if (else_es0.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
              const auto& else_a0 = static_cast<const AssignmentExpression&>(else_es0.Expression());
              if (else_a0.Op() == "=" && get_id_name(else_a0.Lhs()) == var_name) {
                auto extracted = try_extract(if_s, var_name, else_a0.Rhs());
                if (extracted != nullptr) {
                  new_stmts.push_back(std::move(extracted));
                  new_stmts.push_back(CStatement::Return(else_a0.Rhs().Clone()));
                  i += 1;
                  continue;
                }
              }
            }
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
  absl::flat_hash_set<std::string> float_param_names;
  absl::flat_hash_set<std::string> float_stack_param_names;
  for (const auto& p : parameters) {
    param_names.insert(p.name);
    if (p.type.ToString() == "f32" && absl::StartsWith(p.name, "arg")) {
      float_param_names.insert(p.name);
      if (p.name != "arg0" && p.name != "arg1" && p.name != "arg2" && p.name != "arg3") {
        float_stack_param_names.insert(p.name);
      }
    }
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

  std::optional<size_t> modified_s0_arg0_stmt_idx;
  bool other_s_reg_coalesced = false;
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
          bool is_s_reg = (absl::StartsWith(lhs, "temp_s") || lhs == "fp" ||
                           (lhs.size() == 2 && lhs[0] == 's' && std::isdigit(lhs[1])));
          bool is_f_reg = (lhs.size() >= 2 && lhs[0] == 'f' && std::isdigit(lhs[1]));
          bool is_callee_saved_f_reg = (lhs == "f20" || lhs == "f22" || lhs == "f24" ||
                                        lhs == "f26" || lhs == "f28" || lhs == "f30");
          if (param_names.contains(rhs) && (is_s_reg || is_f_reg) &&
              !(float_param_names.contains(rhs) && is_s_reg) &&
              !(float_stack_param_names.contains(rhs) && is_callee_saved_f_reg)) {
            std::string body_text = comp->ToString();
            if (!is_assigned(rhs) && count_assignments(lhs) == 1 &&
                !absl::StrContains(body_text, absl::StrCat("&", lhs)) &&
                !absl::StrContains(body_text, absl::StrCat("(*)())", lhs))) {
              coalesced_aliases[lhs] = rhs;
              stmts_to_erase.push_back(i);
              if (is_s_reg) {
                other_s_reg_coalesced = true;
              }
            } else if (i == 0 && lhs == "temp_s0" && rhs == "arg0" && !is_assigned(rhs) &&
                       !absl::StrContains(body_text, absl::StrCat("&", lhs)) &&
                       !absl::StrContains(body_text, absl::StrCat("(*)())", lhs))) {
              modified_s0_arg0_stmt_idx = i;
            }
          }
        }
      }
    }
  }
  if (modified_s0_arg0_stmt_idx.has_value() && other_s_reg_coalesced) {
    coalesced_aliases["temp_s0"] = "arg0";
    stmts_to_erase.insert(stmts_to_erase.begin(), *modified_s0_arg0_stmt_idx);
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
}

// NOLINTNEXTLINE(google-readability-function-size)
void FoldSingleUseExpressions(CompoundStatement* comp, const CompoundStatement* root_body = nullptr,
                              const absl::flat_hash_set<std::string>* dead_at_end_vars = nullptr,
                              bool is_switch_case_body = false, int root_has_label_cached = -1) {
  if (comp == nullptr) return;
  if (root_body == nullptr) root_body = comp;

  auto matches_var_identifier = [](std::string_view id_name, std::string_view var) -> bool {
    if (id_name == var) return true;
    if (absl::StartsWith(id_name, "((") && absl::EndsWith(id_name, ")")) {
      std::string suffix = absl::StrCat(")", var, ")");
      if (absl::EndsWith(id_name, suffix)) return true;
    }
    return false;
  };

  auto count_uses = [&](const CStatement& s, std::string_view var) -> int {
    int count = 0;
    std::function<void(const CExpression&)> check_expr = [&](const CExpression& e) {
      if (e.Kind() == CExpressionKind::kIdentifier) {
        if (matches_var_identifier(static_cast<const IdentifierExpression&>(e).Name(), var)) {
          count++;
        }
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
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos) {
          return;
        }
        if (un.Op().find('&') != std::string::npos) {
          found = true;
          return;
        }
        check(un.Operand());
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

  auto count_expr_uses = [&](const CExpression& e, std::string_view var) -> int {
    int count = 0;
    std::function<void(const CExpression&)> check_expr = [&](const CExpression& expr) {
      if (expr.Kind() == CExpressionKind::kIdentifier) {
        if (matches_var_identifier(static_cast<const IdentifierExpression&>(expr).Name(), var)) {
          count++;
        }
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

  auto has_address_of_var_in_stmt = [&](const CStatement& root_s, std::string_view var) -> bool {
    bool found = false;
    std::function<void(const CStatement&)> walk = [&](const CStatement& s) {
      if (found) return;
      switch (s.Kind()) {
        case CStatementKind::kExpressionStatement:
          if (has_address_of_var(static_cast<const ExpressionStatement&>(s).Expression(), var)) {
            found = true;
          }
          break;
        case CStatementKind::kReturnStatement: {
          const auto& ret = static_cast<const ReturnStatement&>(s);
          if (ret.ReturnValue() != nullptr && has_address_of_var(*ret.ReturnValue(), var)) {
            found = true;
          }
          break;
        }
        case CStatementKind::kIfStatement: {
          const auto& ifs = static_cast<const IfStatement&>(s);
          if (has_address_of_var(ifs.Condition(), var)) found = true;
          walk(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) walk(*ifs.ElseBranch());
          break;
        }
        case CStatementKind::kWhileStatement: {
          const auto& ws = static_cast<const WhileStatement&>(s);
          if (has_address_of_var(ws.Condition(), var)) found = true;
          walk(ws.Body());
          break;
        }
        case CStatementKind::kDoWhileStatement: {
          const auto& dws = static_cast<const DoWhileStatement&>(s);
          if (has_address_of_var(dws.Condition(), var)) found = true;
          walk(dws.Body());
          break;
        }
        case CStatementKind::kForStatement: {
          const auto& fs = static_cast<const ForStatement&>(s);
          if (fs.Init() != nullptr) walk(*fs.Init());
          if (fs.Condition() != nullptr && has_address_of_var(*fs.Condition(), var)) found = true;
          if (fs.Step() != nullptr && has_address_of_var(*fs.Step(), var)) found = true;
          walk(fs.Body());
          break;
        }
        case CStatementKind::kCompoundStatement: {
          for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
            walk(*child);
          }
          break;
        }
        case CStatementKind::kSwitchStatement: {
          const auto& ss = static_cast<const SwitchStatement&>(s);
          if (has_address_of_var(ss.Condition(), var)) found = true;
          for (const auto& sc : ss.Cases()) {
            if (sc.body != nullptr) walk(*sc.body);
          }
          break;
        }
        default:
          break;
      }
    };
    walk(root_s);
    return found;
  };

  auto has_label_in_stmt = [](const CStatement& root_s) -> bool {
    bool found = false;
    std::function<void(const CStatement&)> walk = [&](const CStatement& s) {
      if (found) return;
      if (s.Kind() == CStatementKind::kLabelStatement) {
        found = true;
        return;
      }
      switch (s.Kind()) {
        case CStatementKind::kIfStatement: {
          const auto& ifs = static_cast<const IfStatement&>(s);
          walk(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) walk(*ifs.ElseBranch());
          break;
        }
        case CStatementKind::kWhileStatement:
          walk(static_cast<const WhileStatement&>(s).Body());
          break;
        case CStatementKind::kDoWhileStatement:
          walk(static_cast<const DoWhileStatement&>(s).Body());
          break;
        case CStatementKind::kForStatement: {
          const auto& fs = static_cast<const ForStatement&>(s);
          if (fs.Init() != nullptr) walk(*fs.Init());
          walk(fs.Body());
          break;
        }
        case CStatementKind::kCompoundStatement:
          for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
            walk(*child);
          }
          break;
        case CStatementKind::kSwitchStatement:
          for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
            if (sc.body != nullptr) walk(*sc.body);
          }
          break;
        default:
          break;
      }
    };
    walk(root_s);
    return found;
  };

  auto has_control_jump_in_stmt = [](const CStatement& root_s) -> bool {
    bool found = false;
    std::function<void(const CStatement&)> walk = [&](const CStatement& s) {
      if (found) return;
      if (s.Kind() == CStatementKind::kBreakStatement ||
          s.Kind() == CStatementKind::kContinueStatement ||
          s.Kind() == CStatementKind::kGotoStatement ||
          s.Kind() == CStatementKind::kLabelStatement) {
        found = true;
        return;
      }
      switch (s.Kind()) {
        case CStatementKind::kIfStatement: {
          const auto& ifs = static_cast<const IfStatement&>(s);
          walk(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) walk(*ifs.ElseBranch());
          break;
        }
        case CStatementKind::kWhileStatement:
          walk(static_cast<const WhileStatement&>(s).Body());
          break;
        case CStatementKind::kDoWhileStatement:
          walk(static_cast<const DoWhileStatement&>(s).Body());
          break;
        case CStatementKind::kForStatement: {
          const auto& fs = static_cast<const ForStatement&>(s);
          if (fs.Init() != nullptr) walk(*fs.Init());
          walk(fs.Body());
          break;
        }
        case CStatementKind::kCompoundStatement:
          for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
            walk(*child);
          }
          break;
        case CStatementKind::kSwitchStatement:
          for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
            if (sc.body != nullptr) walk(*sc.body);
          }
          break;
        default:
          break;
      }
    };
    walk(root_s);
    return found;
  };

  auto is_stack_param_or_local_name = [](std::string_view id_name) -> bool {
    if (absl::StartsWith(id_name, "var_sp_")) return true;
    if (id_name.size() >= 4 && absl::StartsWith(id_name, "arg") && id_name != "arg0" &&
        id_name != "arg1" && id_name != "arg2" && id_name != "arg3") {
      return true;
    }
    return false;
  };

  auto has_memory_deref = [&](const CExpression& expr) -> bool {
    bool found = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (found) return;
      if (e.Kind() == CExpressionKind::kIdentifier) {
        std::string_view id_name = static_cast<const IdentifierExpression&>(e).Name();
        if (is_stack_param_or_local_name(id_name)) {
          found = true;
        }
        return;
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos) {
          if ((un.Op() == "*(s32*)&" || un.Op() == "*(u32*)&" || un.Op() == "*(f32*)&" ||
               un.Op() == "*(s32 *)&" || un.Op() == "*(u32 *)&" || un.Op() == "*(f32 *)&") &&
              un.Operand().Kind() == CExpressionKind::kIdentifier) {
            std::string_view id_name =
                static_cast<const IdentifierExpression&>(un.Operand()).Name();
            if (!is_stack_param_or_local_name(id_name) && !absl::StartsWith(id_name, "D_") &&
                !absl::StartsWith(id_name, "g_") && !absl::StartsWith(id_name, "RO_") &&
                !absl::StartsWith(id_name, "B_")) {
              return;
            }
          }
          found = true;
          return;
        }
        check(un.Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        found = true;
        return;
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        if (static_cast<const MemberAccessExpression&>(e).IsArrow()) {
          found = true;
          return;
        }
        check(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        check(static_cast<const BinaryExpression&>(e).Lhs());
        check(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
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

  auto is_call_or_wrapped_call = [&](const CExpression& expr) -> bool {
    int call_count = 0;
    bool invalid = false;
    std::function<void(const CExpression&)> check = [&](const CExpression& e) {
      if (invalid) return;
      if (e.Kind() == CExpressionKind::kCallExpression) {
        call_count++;
        if (call_count > 1) {
          invalid = true;
        }
        return;
      }
      if (e.Kind() == CExpressionKind::kAssignmentExpression ||
          e.Kind() == CExpressionKind::kArrayIndexExpression) {
        invalid = true;
        return;
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos) {
          invalid = true;
          return;
        }
        check(un.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        if (bin.Op() == "&&" || bin.Op() == "||") {
          invalid = true;
          return;
        }
        check(bin.Lhs());
        check(bin.Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        check(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        invalid = true;
      }
    };
    check(expr);
    return !invalid && call_count == 1;
  };

  auto is_unconditionally_evaluated_first = [&](const CExpression& expr,
                                                std::string_view var) -> bool {
    bool seen_var = false;
    bool seen_side_effect_before_var = false;
    bool in_conditional_branch = false;
    std::function<void(const CExpression&, bool)> walk = [&](const CExpression& e,
                                                             bool cond_branch) {
      if (seen_var) return;
      if (e.Kind() == CExpressionKind::kIdentifier) {
        if (matches_var_identifier(static_cast<const IdentifierExpression&>(e).Name(), var)) {
          seen_var = true;
          in_conditional_branch = cond_branch;
        }
        return;
      }
      if (e.Kind() == CExpressionKind::kCallExpression ||
          e.Kind() == CExpressionKind::kAssignmentExpression) {
        seen_side_effect_before_var = true;
        return;
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op().find('*') != std::string::npos) {
          seen_side_effect_before_var = true;
        }
        walk(un.Operand(), cond_branch);
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        walk(bin.Lhs(), cond_branch);
        bool rhs_cond = cond_branch || (bin.Op() == "&&" || bin.Op() == "||");
        walk(bin.Rhs(), rhs_cond);
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        walk(static_cast<const CastExpression&>(e).Operand(), cond_branch);
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        seen_side_effect_before_var = true;
        walk(static_cast<const ArrayIndexExpression&>(e).Array(), cond_branch);
        walk(static_cast<const ArrayIndexExpression&>(e).Index(), cond_branch);
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        if (static_cast<const MemberAccessExpression&>(e).IsArrow()) {
          seen_side_effect_before_var = true;
        }
        walk(static_cast<const MemberAccessExpression&>(e).Object(), cond_branch);
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        const auto& tern = static_cast<const TernaryExpression&>(e);
        walk(tern.Condition(), cond_branch);
        walk(tern.TrueExpression(), true);
        walk(tern.FalseExpression(), true);
      }
    };
    walk(expr, false);
    return seen_var && !in_conditional_branch && !seen_side_effect_before_var;
  };

  auto is_scratch_var = [](std::string_view var) -> bool {
    return var == "v0" || var == "v1" || var == "at" || var == "temp_at" || var == "fcond" ||
           var == "f0" || var == "f0_d" || var == "f2" || var == "f2_d" || var == "f4" ||
           var == "f4_d" || var == "f6" || var == "f6_d" || var == "f8" || var == "f8_d" ||
           var == "f10" || var == "f10_d" || var == "f16" || var == "f16_d" || var == "f18" ||
           var == "f18_d" || absl::StartsWith(var, "temp_t") ||
           (var.size() >= 2 && var[0] == 't' && std::isdigit(var[1]));
  };

  auto is_outgoing_stack_arg_var = [](std::string_view var) -> bool {
    if (!absl::StartsWith(var, "var_sp_")) return false;
    int32_t offset = 0;
    if (!absl::SimpleAtoi(var.substr(7), &offset)) return false;
    return offset >= 16 && (offset % 4 == 0) && offset < 128;
  };

  auto is_call_arg_var = [](std::string_view var) -> bool {
    return var == "arg0" || var == "arg1" || var == "arg2" || var == "arg3";
  };

  auto extract_float_bitcast_lhs_var = [](const CExpression& lhs) -> std::string_view {
    if (lhs.Kind() != CExpressionKind::kUnaryExpression) return {};
    const auto& deref = static_cast<const UnaryExpression&>(lhs);
    if (deref.Op() != "*" || !deref.IsPrefix()) return {};
    if (deref.Operand().Kind() != CExpressionKind::kCastExpression) return {};
    const auto& cast = static_cast<const CastExpression&>(deref.Operand());
    if (cast.TargetType().name != "f32" || cast.TargetType().pointer_depth != 1) return {};
    if (cast.Operand().Kind() != CExpressionKind::kUnaryExpression) return {};
    const auto& addr = static_cast<const UnaryExpression&>(cast.Operand());
    if (addr.Op() != "&" || !addr.IsPrefix()) return {};
    if (addr.Operand().Kind() != CExpressionKind::kIdentifier) return {};
    return static_cast<const IdentifierExpression&>(addr.Operand()).Name();
  };

  auto extract_assigned_lhs_var = [&](const CExpression& lhs) -> std::string_view {
    if (lhs.Kind() == CExpressionKind::kIdentifier) {
      return static_cast<const IdentifierExpression&>(lhs).Name();
    }
    return extract_float_bitcast_lhs_var(lhs);
  };

  if (root_has_label_cached == -1) {
    root_has_label_cached = (root_body != nullptr && has_label_in_stmt(*root_body)) ? 1 : 0;
  }
  const bool root_has_label = (root_has_label_cached != 0);

  enum class VarLiveness {
    kTransparent,
    kReadBeforeWrite,
    kKilledBeforeRead,
  };

  const CStatement* active_ignored_def_stmt = nullptr;
  const CStatement* active_ignored_use_stmt = nullptr;

  std::function<VarLiveness(std::string_view, std::string_view, int)> analyze_from_label;
  std::function<VarLiveness(const CStatement&, std::string_view, bool, int)> analyze_var_liveness =
      [&](const CStatement& s, std::string_view var, bool in_switch_case_top,
          int goto_depth) -> VarLiveness {
    if (&s == active_ignored_def_stmt || &s == active_ignored_use_stmt) {
      return VarLiveness::kTransparent;
    }
    switch (s.Kind()) {
      case CStatementKind::kExpressionStatement: {
        const auto& es = static_cast<const ExpressionStatement&>(s);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          std::string_view lhs_var = (a.Op() == "=") ? extract_assigned_lhs_var(a.Lhs()) : "";
          if (!lhs_var.empty() && lhs_var == var) {
            return count_expr_uses(a.Rhs(), var) == 0 ? VarLiveness::kKilledBeforeRead
                                                      : VarLiveness::kReadBeforeWrite;
          }
        }
        return count_expr_uses(es.Expression(), var) > 0 ? VarLiveness::kReadBeforeWrite
                                                         : VarLiveness::kTransparent;
      }
      case CStatementKind::kReturnStatement:
        return count_uses(s, var) == 0 ? VarLiveness::kKilledBeforeRead
                                       : VarLiveness::kReadBeforeWrite;
      case CStatementKind::kBreakStatement:
        return in_switch_case_top ? VarLiveness::kTransparent : VarLiveness::kReadBeforeWrite;
      case CStatementKind::kContinueStatement:
        return VarLiveness::kReadBeforeWrite;
      case CStatementKind::kGotoStatement: {
        if (goto_depth > 4 || root_body == nullptr) {
          return VarLiveness::kReadBeforeWrite;
        }
        const auto& gs = static_cast<const GotoStatement&>(s);
        return analyze_from_label(gs.Label(), var, goto_depth + 1);
      }
      case CStatementKind::kLabelStatement:
        return VarLiveness::kTransparent;
      case CStatementKind::kCompoundStatement: {
        const auto& children = static_cast<const CompoundStatement&>(s).Statements();
        for (size_t c = 0; c < children.size(); ++c) {
          const auto& child = children[c];
          if (child->Kind() == CStatementKind::kBreakStatement && in_switch_case_top) {
            return VarLiveness::kTransparent;
          }
          VarLiveness res = analyze_var_liveness(*child, var, in_switch_case_top, goto_depth);
          if (res == VarLiveness::kReadBeforeWrite) return VarLiveness::kReadBeforeWrite;
          if (res == VarLiveness::kKilledBeforeRead) {
            bool label_after = false;
            if (root_has_label) {
              for (size_t c2 = c + 1; c2 < children.size(); ++c2) {
                if (has_label_in_stmt(*children[c2])) {
                  label_after = true;
                  break;
                }
              }
            }
            if (!label_after) return VarLiveness::kKilledBeforeRead;
          }
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        if (count_expr_uses(ifs.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        VarLiveness then_res =
            analyze_var_liveness(ifs.ThenBranch(), var, in_switch_case_top, goto_depth);
        VarLiveness else_res =
            (ifs.ElseBranch() != nullptr)
                ? analyze_var_liveness(*ifs.ElseBranch(), var, in_switch_case_top, goto_depth)
                : VarLiveness::kTransparent;
        if (then_res == VarLiveness::kReadBeforeWrite ||
            else_res == VarLiveness::kReadBeforeWrite) {
          return VarLiveness::kReadBeforeWrite;
        }
        if (then_res == VarLiveness::kKilledBeforeRead &&
            else_res == VarLiveness::kKilledBeforeRead) {
          return VarLiveness::kKilledBeforeRead;
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        if (count_expr_uses(ss.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        bool has_default = false;
        bool all_killed = !ss.Cases().empty();
        for (const auto& sc : ss.Cases()) {
          if (sc.is_default) has_default = true;
          VarLiveness case_res =
              (sc.body != nullptr)
                  ? analyze_var_liveness(*sc.body, var, /*in_switch_case_top=*/true, goto_depth)
                  : VarLiveness::kTransparent;
          if (case_res == VarLiveness::kReadBeforeWrite) {
            return VarLiveness::kReadBeforeWrite;
          }
          if (case_res != VarLiveness::kKilledBeforeRead) {
            all_killed = false;
          }
        }
        if (has_default && all_killed) {
          return VarLiveness::kKilledBeforeRead;
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(s);
        if (count_expr_uses(ws.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        VarLiveness body_res = analyze_var_liveness(ws.Body(), var, false, goto_depth);
        if (body_res == VarLiveness::kReadBeforeWrite) {
          return VarLiveness::kReadBeforeWrite;
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(s);
        VarLiveness body_res = analyze_var_liveness(dws.Body(), var, false, goto_depth);
        if (body_res == VarLiveness::kReadBeforeWrite) {
          return VarLiveness::kReadBeforeWrite;
        }
        if (body_res == VarLiveness::kKilledBeforeRead && !has_label_in_stmt(dws.Body())) {
          return VarLiveness::kKilledBeforeRead;
        }
        if (count_expr_uses(dws.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        return VarLiveness::kTransparent;
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr) {
          VarLiveness init_res = analyze_var_liveness(*fs.Init(), var, false, goto_depth);
          if (init_res == VarLiveness::kReadBeforeWrite) return VarLiveness::kReadBeforeWrite;
          if (init_res == VarLiveness::kKilledBeforeRead) return VarLiveness::kKilledBeforeRead;
        }
        if (fs.Condition() != nullptr && count_expr_uses(*fs.Condition(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        VarLiveness body_res = analyze_var_liveness(fs.Body(), var, false, goto_depth);
        if (body_res == VarLiveness::kReadBeforeWrite) {
          return VarLiveness::kReadBeforeWrite;
        }
        if (body_res != VarLiveness::kKilledBeforeRead && fs.Step() != nullptr &&
            count_expr_uses(*fs.Step(), var) > 0) {
          return VarLiveness::kReadBeforeWrite;
        }
        return VarLiveness::kTransparent;
      }
      default:
        return (count_uses(s, var) > 0 || (root_has_label && has_control_jump_in_stmt(s)))
                   ? VarLiveness::kReadBeforeWrite
                   : VarLiveness::kTransparent;
    }
  };

  analyze_from_label = [&](std::string_view target_label, std::string_view var,
                           int goto_depth) -> VarLiveness {
    if (goto_depth > 4 || root_body == nullptr) {
      return VarLiveness::kReadBeforeWrite;
    }
    const CompoundStatement* found_comp = nullptr;
    size_t found_idx = 0;
    std::function<void(const CStatement&)> find_label = [&](const CStatement& s) {
      if (found_comp != nullptr) return;
      if (s.Kind() == CStatementKind::kCompoundStatement) {
        const auto& cs = static_cast<const CompoundStatement&>(s);
        for (size_t idx = 0; idx < cs.Statements().size(); ++idx) {
          const CStatement* cur = cs.Statements()[idx].get();
          if (cur != nullptr && cur->Kind() == CStatementKind::kLabelStatement) {
            const auto& ls = static_cast<const LabelStatement&>(*cur);
            if (ls.Label() == target_label) {
              found_comp = &cs;
              found_idx = idx;
              return;
            }
          }
          find_label(*cs.Statements()[idx]);
          if (found_comp != nullptr) return;
        }
      } else if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        find_label(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) find_label(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
          if (sc.body != nullptr) find_label(*sc.body);
        }
      }
    };
    find_label(*root_body);
    if (found_comp == nullptr) {
      return VarLiveness::kReadBeforeWrite;
    }
    for (size_t m = found_idx; m < found_comp->Statements().size(); ++m) {
      VarLiveness res = analyze_var_liveness(*found_comp->Statements()[m], var, false, goto_depth);
      if (res == VarLiveness::kReadBeforeWrite) return VarLiveness::kReadBeforeWrite;
      if (res == VarLiveness::kKilledBeforeRead) return VarLiveness::kKilledBeforeRead;
    }
    if (found_comp == root_body) {
      return VarLiveness::kKilledBeforeRead;
    }
    return VarLiveness::kReadBeforeWrite;
  };

  auto is_comparison_expr = [](const CExpression& e) -> bool {
    if (e.Kind() != CExpressionKind::kBinaryExpression) return false;
    std::string_view op = static_cast<const BinaryExpression&>(e).Op();
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
  };

  auto has_call_in_stmt = [](const CStatement& root_s) -> bool {
    bool found = false;
    std::function<void(const CExpression&)> walk_expr = [&](const CExpression& e) {
      if (found) return;
      if (e.Kind() == CExpressionKind::kCallExpression) {
        found = true;
        return;
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        walk_expr(static_cast<const UnaryExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        walk_expr(static_cast<const BinaryExpression&>(e).Lhs());
        walk_expr(static_cast<const BinaryExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
        walk_expr(static_cast<const AssignmentExpression&>(e).Lhs());
        walk_expr(static_cast<const AssignmentExpression&>(e).Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        walk_expr(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kArrayIndexExpression) {
        walk_expr(static_cast<const ArrayIndexExpression&>(e).Array());
        walk_expr(static_cast<const ArrayIndexExpression&>(e).Index());
      } else if (e.Kind() == CExpressionKind::kMemberAccessExpression) {
        walk_expr(static_cast<const MemberAccessExpression&>(e).Object());
      } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
        walk_expr(static_cast<const TernaryExpression&>(e).Condition());
        walk_expr(static_cast<const TernaryExpression&>(e).TrueExpression());
        walk_expr(static_cast<const TernaryExpression&>(e).FalseExpression());
      }
    };
    std::function<void(const CStatement&)> walk_stmt = [&](const CStatement& s) {
      if (found) return;
      if (s.Kind() == CStatementKind::kExpressionStatement) {
        walk_expr(static_cast<const ExpressionStatement&>(s).Expression());
      } else if (s.Kind() == CStatementKind::kReturnStatement) {
        const auto& ret = static_cast<const ReturnStatement&>(s);
        if (ret.ReturnValue() != nullptr) walk_expr(*ret.ReturnValue());
      } else if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        walk_expr(ifs.Condition());
        walk_stmt(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) walk_stmt(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        walk_expr(ss.Condition());
        for (const auto& sc : ss.Cases()) {
          if (sc.body != nullptr) walk_stmt(*sc.body);
        }
      } else if (s.Kind() == CStatementKind::kCompoundStatement) {
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          walk_stmt(*child);
        }
      }
    };
    walk_stmt(root_s);
    return found;
  };

  auto is_dead_after_stmt = [&](size_t next_idx, std::string_view var,
                                int expected_uses_in_next = 1,
                                const CStatement* def_stmt = nullptr) -> bool {
    const auto& stmts = comp->Statements();
    if (next_idx >= stmts.size()) {
      return false;
    }
    if (stmts[next_idx]->Kind() == CStatementKind::kReturnStatement) {
      return !has_address_of_var_in_stmt(*stmts[next_idx], var);
    }
    if (stmts[next_idx]->Kind() != CStatementKind::kIfStatement &&
        stmts[next_idx]->Kind() != CStatementKind::kSwitchStatement &&
        has_control_jump_in_stmt(*stmts[next_idx])) {
      return false;
    }

    active_ignored_def_stmt = def_stmt;
    active_ignored_use_stmt = (expected_uses_in_next > 0) ? stmts[next_idx].get() : nullptr;
    for (size_t k = next_idx + 1; k < stmts.size(); ++k) {
      const auto& s = *stmts[k];
      if (s.Kind() == CStatementKind::kBreakStatement && is_switch_case_body) {
        break;
      }
      VarLiveness res = analyze_var_liveness(s, var, is_switch_case_body, 0);
      if (res == VarLiveness::kKilledBeforeRead) {
        active_ignored_def_stmt = nullptr;
        active_ignored_use_stmt = nullptr;
        return true;
      }
      if (res == VarLiveness::kReadBeforeWrite) {
        active_ignored_def_stmt = nullptr;
        active_ignored_use_stmt = nullptr;
        return false;
      }
    }
    active_ignored_def_stmt = nullptr;
    active_ignored_use_stmt = nullptr;
    if (comp == root_body || (dead_at_end_vars != nullptr && dead_at_end_vars->contains(var))) {
      return true;
    }
    if ((is_outgoing_stack_arg_var(var) || (is_call_arg_var(var) && expected_uses_in_next > 0 &&
                                            has_call_in_stmt(*stmts[next_idx]))) &&
        !root_has_label) {
      return true;
    }
    if (is_outgoing_stack_arg_var(var) && expected_uses_in_next > 0 &&
        has_call_in_stmt(*stmts[next_idx]) && root_body != nullptr) {
      int total_uses_in_root = count_uses(*root_body, var);
      int uses_in_calls = 0;
      std::function<void(const CExpression&)> count_call_uses_expr = [&](const CExpression& e) {
        if (e.Kind() == CExpressionKind::kCallExpression) {
          for (const auto& arg : static_cast<const CallExpression&>(e).Arguments()) {
            uses_in_calls += count_expr_uses(*arg, var);
          }
          return;
        }
        if (e.Kind() == CExpressionKind::kUnaryExpression) {
          count_call_uses_expr(static_cast<const UnaryExpression&>(e).Operand());
        } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
          count_call_uses_expr(static_cast<const BinaryExpression&>(e).Lhs());
          count_call_uses_expr(static_cast<const BinaryExpression&>(e).Rhs());
        } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
          count_call_uses_expr(static_cast<const AssignmentExpression&>(e).Lhs());
          count_call_uses_expr(static_cast<const AssignmentExpression&>(e).Rhs());
        } else if (e.Kind() == CExpressionKind::kCastExpression) {
          count_call_uses_expr(static_cast<const CastExpression&>(e).Operand());
        } else if (e.Kind() == CExpressionKind::kTernaryExpression) {
          const auto& tern = static_cast<const TernaryExpression&>(e);
          count_call_uses_expr(tern.Condition());
          count_call_uses_expr(tern.TrueExpression());
          count_call_uses_expr(tern.FalseExpression());
        }
      };
      std::function<void(const CStatement&)> count_call_uses_stmt = [&](const CStatement& s) {
        if (s.Kind() == CStatementKind::kExpressionStatement) {
          count_call_uses_expr(static_cast<const ExpressionStatement&>(s).Expression());
        } else if (s.Kind() == CStatementKind::kReturnStatement) {
          const auto& ret = static_cast<const ReturnStatement&>(s);
          if (ret.ReturnValue() != nullptr) count_call_uses_expr(*ret.ReturnValue());
        } else if (s.Kind() == CStatementKind::kCompoundStatement) {
          for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
            count_call_uses_stmt(*child);
          }
        } else if (s.Kind() == CStatementKind::kIfStatement) {
          const auto& ifs = static_cast<const IfStatement&>(s);
          count_call_uses_expr(ifs.Condition());
          count_call_uses_stmt(ifs.ThenBranch());
          if (ifs.ElseBranch() != nullptr) count_call_uses_stmt(*ifs.ElseBranch());
        } else if (s.Kind() == CStatementKind::kSwitchStatement) {
          const auto& ss = static_cast<const SwitchStatement&>(s);
          count_call_uses_expr(ss.Condition());
          for (const auto& sc : ss.Cases()) {
            if (sc.body != nullptr) count_call_uses_stmt(*sc.body);
          }
        } else if (s.Kind() == CStatementKind::kWhileStatement) {
          const auto& ws = static_cast<const WhileStatement&>(s);
          count_call_uses_expr(ws.Condition());
          count_call_uses_stmt(ws.Body());
        } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
          const auto& dws = static_cast<const DoWhileStatement&>(s);
          count_call_uses_stmt(dws.Body());
          count_call_uses_expr(dws.Condition());
        } else if (s.Kind() == CStatementKind::kForStatement) {
          const auto& fs = static_cast<const ForStatement&>(s);
          if (fs.Init() != nullptr) count_call_uses_stmt(*fs.Init());
          if (fs.Condition() != nullptr) count_call_uses_expr(*fs.Condition());
          if (fs.Step() != nullptr) count_call_uses_expr(*fs.Step());
          count_call_uses_stmt(fs.Body());
        }
      };
      count_call_uses_stmt(*root_body);
      if (total_uses_in_root == uses_in_calls && !has_address_of_var_in_stmt(*root_body, var)) {
        return true;
      }
    }
    return false;
  };

  auto is_duplicable_arg_rhs = [](const CExpression& rhs) -> bool {
    if (rhs.Kind() == CExpressionKind::kIdentifier ||
        rhs.Kind() == CExpressionKind::kIntegerLiteral ||
        rhs.Kind() == CExpressionKind::kFloatLiteral) {
      return true;
    }
    if (rhs.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(rhs);
      if ((un.Op() == "&" || un.Op() == "-" || un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&") &&
          (un.Operand().Kind() == CExpressionKind::kIdentifier ||
           un.Operand().Kind() == CExpressionKind::kIntegerLiteral)) {
        return true;
      }
    }
    return false;
  };

  auto extract_simple_deref_range = [](const CExpression& e, std::string_view* out_base,
                                       int64_t* out_offset, int64_t* out_size) -> bool {
    if (e.Kind() != CExpressionKind::kUnaryExpression) return false;
    const auto& un = static_cast<const UnaryExpression&>(e);
    if (un.Op() != "*" || un.Operand().Kind() != CExpressionKind::kCastExpression) return false;
    const auto& cast = static_cast<const CastExpression&>(un.Operand());
    std::string ts = cast.TargetType().ToString();
    int64_t size = 4;
    if (ts == "s8*" || ts == "u8*" || ts == "s8 *" || ts == "u8 *") {
      size = 1;
    } else if (ts == "s16*" || ts == "u16*" || ts == "s16 *" || ts == "u16 *") {
      size = 2;
    } else if (ts == "s64*" || ts == "u64*" || ts == "f64*" || ts == "s64 *" || ts == "u64 *" ||
               ts == "f64 *") {
      size = 8;
    }
    const CExpression& addr = cast.Operand();
    if (addr.Kind() == CExpressionKind::kIdentifier) {
      *out_base = static_cast<const IdentifierExpression&>(addr).Name();
      *out_offset = 0;
      *out_size = size;
      return true;
    }
    if (addr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(addr);
      if ((bin.Op() == "+" || bin.Op() == "-") &&
          bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
          bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        *out_base = static_cast<const IdentifierExpression&>(bin.Lhs()).Name();
        int64_t off = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        *out_offset = (bin.Op() == "-") ? -off : off;
        *out_size = size;
        return true;
      }
    }
    return false;
  };

  auto are_provably_disjoint_derefs = [&](const CExpression& a, const CExpression& b) -> bool {
    std::string_view base_a;
    std::string_view base_b;
    int64_t off_a = 0;
    int64_t off_b = 0;
    int64_t size_a = 0;
    int64_t size_b = 0;
    if (!extract_simple_deref_range(a, &base_a, &off_a, &size_a) ||
        !extract_simple_deref_range(b, &base_b, &off_b, &size_b)) {
      return false;
    }
    if (base_a != base_b) return false;
    return (off_a + size_a <= off_b) || (off_b + size_b <= off_a);
  };

  auto are_all_derefs_provably_disjoint = [&](const CExpression& expr,
                                              const CExpression& store_lhs) -> bool {
    bool found_deref = false;
    bool all_disjoint = true;
    std::function<void(const CExpression&)> walk = [&](const CExpression& e) {
      if (!all_disjoint) return;
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op() == "*") {
          found_deref = true;
          if (!are_provably_disjoint_derefs(e, store_lhs)) {
            all_disjoint = false;
          }
          return;
        }
        if (un.Op() == "*(s32*)&" || un.Op() == "*(u32*)&" || un.Op() == "*(f32*)&" ||
            un.Op() == "*(s32 *)&" || un.Op() == "*(u32 *)&" || un.Op() == "*(f32 *)&") {
          if (un.Operand().Kind() == CExpressionKind::kIdentifier) {
            std::string_view id_name =
                static_cast<const IdentifierExpression&>(un.Operand()).Name();
            if (!absl::StartsWith(id_name, "D_") && !absl::StartsWith(id_name, "g_") &&
                !absl::StartsWith(id_name, "RO_") && !absl::StartsWith(id_name, "B_")) {
              return;
            }
          }
          all_disjoint = false;
          return;
        }
        walk(un.Operand());
      } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        walk(bin.Lhs());
        walk(bin.Rhs());
      } else if (e.Kind() == CExpressionKind::kCastExpression) {
        walk(static_cast<const CastExpression&>(e).Operand());
      } else if (e.Kind() == CExpressionKind::kIdentifier ||
                 e.Kind() == CExpressionKind::kIntegerLiteral ||
                 e.Kind() == CExpressionKind::kFloatLiteral) {
        return;
      } else {
        all_disjoint = false;
      }
    };
    walk(expr);
    return found_deref && all_disjoint;
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
          std::string_view extracted_var =
              (assign.Op() == "=") ? extract_assigned_lhs_var(assign.Lhs()) : "";
          bool is_float_bitcast_lhs =
              (!extracted_var.empty() && assign.Lhs().Kind() != CExpressionKind::kIdentifier);
          if (!extracted_var.empty() &&
              (!is_float_bitcast_lhs || is_outgoing_stack_arg_var(extracted_var))) {
            std::string var(extracted_var);
            bool is_scratch = is_scratch_var(var);
            bool is_stack_arg = is_outgoing_stack_arg_var(var);
            bool is_call_arg = is_call_arg_var(var);
            bool is_fp_param_in_place_before_return =
                ((var == "f12" || var == "f14" || var == "f12_d" || var == "f14_d") &&
                 !is_float_bitcast_lhs && i + 1 < stmts.size() &&
                 stmts[i + 1]->Kind() == CStatementKind::kReturnStatement &&
                 is_pure_expr(assign.Rhs()) && !has_memory_deref(assign.Rhs()));
            if (is_fp_param_in_place_before_return) {
              const auto& ret_stmt = static_cast<const ReturnStatement&>(*stmts[i + 1]);
              if (ret_stmt.ReturnValue() != nullptr &&
                  count_expr_uses(*ret_stmt.ReturnValue(), var) == 1 &&
                  is_pure_expr(*ret_stmt.ReturnValue()) &&
                  !has_memory_deref(*ret_stmt.ReturnValue()) &&
                  !has_address_of_var(*ret_stmt.ReturnValue(), var)) {
                auto new_ret = SubstituteIdentifier(*ret_stmt.ReturnValue(), var, assign.Rhs());
                stmts[i + 1] = CStatement::Return(std::move(new_ret));
                stmts.erase(stmts.begin() + i);
                changed = true;
                break;
              }
            }
            if (var.size() >= 2 && var[0] == 'f' && std::isdigit(var[1]) && !is_float_bitcast_lhs &&
                is_pure_expr(assign.Rhs())) {
              // 1. Subword half of a 64-bit double constant load: `f2 = <lit>; f2_d = <lit>;`
              // or `f2_d = <lit>; f2_d = <lit>;`
              if (i + 1 < stmts.size() &&
                  stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement &&
                  (assign.Rhs().Kind() == CExpressionKind::kFloatLiteral ||
                   assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral)) {
                const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& next_a =
                      static_cast<const AssignmentExpression&>(next_es.Expression());
                  if (next_a.Op() == "=" && next_a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      (next_a.Rhs().Kind() == CExpressionKind::kFloatLiteral ||
                       next_a.Rhs().Kind() == CExpressionKind::kIntegerLiteral)) {
                    std::string_view next_lhs =
                        static_cast<const IdentifierExpression&>(next_a.Lhs()).Name();
                    if (next_lhs == var || next_lhs == absl::StrCat(var, "_d")) {
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                      break;
                    }
                  }
                }
              }
              // 2. Completely unread FPR literal assignment (e.g., dead odd/even half of a
              // synthesized double literal).
              bool is_literal_rhs = (assign.Rhs().Kind() == CExpressionKind::kFloatLiteral ||
                                     assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral);
              bool can_drop_unread_fpr =
                  is_literal_rhs &&
                  ((var != "f0" && var != "f0_d") ||
                   (dead_at_end_vars != nullptr && dead_at_end_vars->contains(var)));
              if (can_drop_unread_fpr && root_body != nullptr && count_uses(*root_body, var) == 0 &&
                  !has_address_of_var_in_stmt(*root_body, var)) {
                stmts.erase(stmts.begin() + i);
                changed = true;
                break;
              }
            }
            if (!is_scratch && !is_stack_arg && !is_call_arg) continue;

            auto is_c_var_identifier = [](const CExpression& e) -> bool {
              if (e.Kind() != CExpressionKind::kIdentifier) return false;
              std::string_view name = static_cast<const IdentifierExpression&>(e).Name();
              return !name.empty() &&
                     (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
            };

            std::unique_ptr<CExpression> effective_rhs_holder;
            if (is_float_bitcast_lhs) {
              if (!is_pure_expr(assign.Rhs())) continue;
              effective_rhs_holder = CExpression::Unary("*(s32*)&", assign.Rhs().Clone());
            }
            const CExpression& effective_rhs =
                is_float_bitcast_lhs ? *effective_rhs_holder : assign.Rhs();

            // 0. Dead pre-call argument/stack-arg store elimination:
            // When `arg0 = ...` or `var_sp_16 = ...` is pure and dead after stmt i (e.g., because
            // LiftCallInstruction already substituted its value into the call arguments), erase it
            // immediately so it does not artificially inflate use counts of `v0`/`v1`!
            if ((is_call_arg || is_stack_arg) && is_pure_expr(effective_rhs)) {
              bool followed_by_call = false;
              for (size_t k = i + 1; k < stmts.size() && k <= i + 24; ++k) {
                if (has_call_in_stmt(*stmts[k])) {
                  followed_by_call = true;
                  break;
                }
              }
              bool dead_at_end_arg =
                  is_call_arg && dead_at_end_vars != nullptr && dead_at_end_vars->contains(var);
              if ((followed_by_call || dead_at_end_arg) &&
                  is_dead_after_stmt(i, var, 0, stmts[i].get())) {
                stmts.erase(stmts.begin() + i);
                changed = true;
                break;
              }
            }

            if ((var == "temp_at" ||
                 (var == "v0" && !is_float_bitcast_lhs &&
                  ((dead_at_end_vars != nullptr && dead_at_end_vars->contains(var)) ||
                   comp == root_body) &&
                  (assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral ||
                   is_comparison_expr(effective_rhs)))) &&
                is_pure_expr(effective_rhs) && is_dead_after_stmt(i, var, 0, stmts[i].get())) {
              stmts.erase(stmts.begin() + i);
              changed = true;
              break;
            }

            // Dead scratch assignment overwritten later in the same CompoundStatement
            // across an intervening function call without any intervening read, or dead before a
            // call in the same block.
            if (is_scratch && !is_float_bitcast_lhs && is_pure_expr(effective_rhs)) {
              bool overwritten_before_use = false;
              bool saw_intervening_call = false;
              for (size_t k = i + 1; k < stmts.size(); ++k) {
                if (stmts[k]->Kind() != CStatementKind::kExpressionStatement) break;
                const auto& kes = static_cast<const ExpressionStatement&>(*stmts[k]);
                if (kes.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& ka = static_cast<const AssignmentExpression&>(kes.Expression());
                  if (ka.Op() == "=" && ka.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      static_cast<const IdentifierExpression&>(ka.Lhs()).Name() == var &&
                      count_expr_uses(ka.Rhs(), var) == 0) {
                    if (saw_intervening_call) {
                      overwritten_before_use = true;
                    } else {
                      for (size_t m = k + 1; m < stmts.size(); ++m) {
                        if (has_call_in_stmt(*stmts[m])) {
                          overwritten_before_use = true;
                          break;
                        }
                      }
                    }
                    break;
                  }
                }
                if (count_uses(*stmts[k], var) > 0) break;
                if (has_call_in_stmt(*stmts[k])) {
                  saw_intervening_call = true;
                  bool call_has_stack_args = false;
                  if (stmts[k]->Kind() == CStatementKind::kExpressionStatement) {
                    const auto& k_expr =
                        static_cast<const ExpressionStatement&>(*stmts[k]).Expression();
                    if (k_expr.Kind() == CExpressionKind::kCallExpression) {
                      call_has_stack_args =
                          static_cast<const CallExpression&>(k_expr).Arguments().size() >= 5;
                    } else if (k_expr.Kind() == CExpressionKind::kAssignmentExpression) {
                      const auto& k_rhs = static_cast<const AssignmentExpression&>(k_expr).Rhs();
                      if (k_rhs.Kind() == CExpressionKind::kCallExpression) {
                        call_has_stack_args =
                            static_cast<const CallExpression&>(k_rhs).Arguments().size() >= 5;
                      }
                    }
                  }
                  if (call_has_stack_args && is_dead_after_stmt(k, var, 0, stmts[i].get())) {
                    overwritten_before_use = true;
                    break;
                  }
                }
              }
              if (overwritten_before_use) {
                stmts.erase(stmts.begin() + i);
                changed = true;
                break;
              }
            }

            // Fold `argX = <pure_expr>; *(T*)<addr> = argX;` when `argX` is used as a dead
            // scratch register for a memory store right before return.
            if (is_call_arg && !is_float_bitcast_lhs && is_pure_expr(assign.Rhs()) &&
                i + 2 < stmts.size() &&
                stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement &&
                stmts[i + 2]->Kind() == CStatementKind::kReturnStatement) {
              const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
              if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                const auto& next_assign =
                    static_cast<const AssignmentExpression&>(next_es.Expression());
                if (next_assign.Op() == "=" &&
                    next_assign.Lhs().Kind() != CExpressionKind::kIdentifier &&
                    count_expr_uses(next_assign.Lhs(), var) == 0 &&
                    next_assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
                    static_cast<const IdentifierExpression&>(next_assign.Rhs()).Name() == var &&
                    is_dead_after_stmt(i + 1, var, 1, stmts[i].get())) {
                  stmts[i + 1] = CStatement::Expression(CExpression::Assignment(
                      "=", next_assign.Lhs().Clone(), assign.Rhs().Clone()));
                  stmts.erase(stmts.begin() + i);
                  changed = true;
                  break;
                }
              }
            }

            // Atomically fold `v0 = &sym; dest = ((idx << 2) + v0);` (or
            // `v0 = &sym; dest = (idx << 2); dest = (dest + v0);`) directly into the
            // immediately following statement when `dest` is dereferenced at least twice
            // (`*(T*)dest` read + write) and is dead afterwards.
            if (is_scratch && !is_float_bitcast_lhs &&
                assign.Rhs().Kind() == CExpressionKind::kUnaryExpression &&
                static_cast<const UnaryExpression&>(assign.Rhs()).Op() == "&") {
              auto extract_word_index = [](const CExpression& expr) -> const CExpression* {
                if (expr.Kind() == CExpressionKind::kBinaryExpression) {
                  const auto& b = static_cast<const BinaryExpression&>(expr);
                  if (b.Op() == "<<" && b.Rhs().Kind() == CExpressionKind::kIntegerLiteral &&
                      static_cast<const IntegerLiteralExpression&>(b.Rhs()).Value() == 2 &&
                      b.Lhs().Kind() == CExpressionKind::kIdentifier) {
                    return &b.Lhs();
                  }
                }
                return nullptr;
              };
              std::string dest_var;
              const CExpression* word_idx = nullptr;
              size_t use_stmt_idx = 0;
              // Pattern 1: `v0 = &sym; dest = ((idx << 2) + v0); <use_stmt>`
              if (i + 2 < stmts.size() &&
                  stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement) {
                const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& next_assign =
                      static_cast<const AssignmentExpression&>(next_es.Expression());
                  if (next_assign.Op() == "=" &&
                      next_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      next_assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                    const auto& bin = static_cast<const BinaryExpression&>(next_assign.Rhs());
                    const CExpression* candidate_idx = extract_word_index(bin.Lhs());
                    if (bin.Op() == "+" && candidate_idx != nullptr &&
                        bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
                        static_cast<const IdentifierExpression&>(bin.Rhs()).Name() == var &&
                        count_expr_uses(*candidate_idx, var) == 0 &&
                        is_dead_after_stmt(i + 1, var, 1, stmts[i].get())) {
                      dest_var = std::string(
                          static_cast<const IdentifierExpression&>(next_assign.Lhs()).Name());
                      word_idx = candidate_idx;
                      use_stmt_idx = i + 2;
                    }
                  }
                }
              }
              // Pattern 2: `v0 = &sym; dest = (idx << 2); dest = (dest + v0); <use_stmt>`
              if (word_idx == nullptr && i + 3 < stmts.size() &&
                  stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement &&
                  stmts[i + 2]->Kind() == CStatementKind::kExpressionStatement) {
                const auto& mid_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                const auto& add_es = static_cast<const ExpressionStatement&>(*stmts[i + 2]);
                if (mid_es.Expression().Kind() == CExpressionKind::kAssignmentExpression &&
                    add_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& mid_assign =
                      static_cast<const AssignmentExpression&>(mid_es.Expression());
                  const auto& add_assign =
                      static_cast<const AssignmentExpression&>(add_es.Expression());
                  const CExpression* candidate_idx = extract_word_index(mid_assign.Rhs());
                  if (mid_assign.Op() == "=" && add_assign.Op() == "=" &&
                      candidate_idx != nullptr &&
                      mid_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      add_assign.Lhs().Kind() == CExpressionKind::kIdentifier) {
                    std::string_view candidate_dest =
                        static_cast<const IdentifierExpression&>(mid_assign.Lhs()).Name();
                    if (candidate_dest ==
                            static_cast<const IdentifierExpression&>(add_assign.Lhs()).Name() &&
                        candidate_dest != var && count_expr_uses(*candidate_idx, var) == 0 &&
                        add_assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                      const auto& bin = static_cast<const BinaryExpression&>(add_assign.Rhs());
                      if (bin.Op() == "+" && bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
                          static_cast<const IdentifierExpression&>(bin.Lhs()).Name() ==
                              candidate_dest &&
                          bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
                          static_cast<const IdentifierExpression&>(bin.Rhs()).Name() == var &&
                          is_dead_after_stmt(i + 2, var, 1, stmts[i].get())) {
                        dest_var = std::string(candidate_dest);
                        word_idx = candidate_idx;
                        use_stmt_idx = i + 3;
                      }
                    }
                  }
                }
              }

              if (word_idx != nullptr && (is_scratch_var(dest_var) || is_call_arg_var(dest_var))) {
                int total_dest_uses = count_uses(*stmts[use_stmt_idx], dest_var);
                int deref_dest_uses = 0;
                std::function<void(const CExpression&)> count_deref_uses = [&](const CExpression&
                                                                                   e) {
                  if (e.Kind() == CExpressionKind::kUnaryExpression) {
                    const auto& u = static_cast<const UnaryExpression&>(e);
                    if (u.Op() == "*" && u.Operand().Kind() == CExpressionKind::kCastExpression) {
                      const auto& inner_cast = static_cast<const CastExpression&>(u.Operand());
                      if (inner_cast.Operand().Kind() == CExpressionKind::kIdentifier &&
                          static_cast<const IdentifierExpression&>(inner_cast.Operand()).Name() ==
                              dest_var) {
                        ++deref_dest_uses;
                        return;
                      }
                    }
                    count_deref_uses(u.Operand());
                  } else if (e.Kind() == CExpressionKind::kBinaryExpression) {
                    const auto& b = static_cast<const BinaryExpression&>(e);
                    count_deref_uses(b.Lhs());
                    count_deref_uses(b.Rhs());
                  } else if (e.Kind() == CExpressionKind::kAssignmentExpression) {
                    const auto& a = static_cast<const AssignmentExpression&>(e);
                    count_deref_uses(a.Lhs());
                    count_deref_uses(a.Rhs());
                  } else if (e.Kind() == CExpressionKind::kCastExpression) {
                    count_deref_uses(static_cast<const CastExpression&>(e).Operand());
                  }
                };
                bool safe_stmt_shape = false;
                if (stmts[use_stmt_idx]->Kind() == CStatementKind::kExpressionStatement) {
                  count_deref_uses(
                      static_cast<const ExpressionStatement&>(*stmts[use_stmt_idx]).Expression());
                  safe_stmt_shape = (total_dest_uses >= 2 && deref_dest_uses == total_dest_uses);
                } else if (stmts[use_stmt_idx]->Kind() == CStatementKind::kIfStatement) {
                  const auto& ifs = static_cast<const IfStatement&>(*stmts[use_stmt_idx]);
                  count_deref_uses(ifs.Condition());
                  if (ifs.ThenBranch().Kind() == CStatementKind::kCompoundStatement) {
                    for (const auto& cs :
                         static_cast<const CompoundStatement&>(ifs.ThenBranch()).Statements()) {
                      if (cs->Kind() == CStatementKind::kExpressionStatement) {
                        const auto& ces = static_cast<const ExpressionStatement&>(*cs);
                        if (has_call_in_stmt(*cs)) break;
                        count_deref_uses(ces.Expression());
                      } else {
                        break;
                      }
                    }
                  }
                  safe_stmt_shape = (total_dest_uses >= 2 && deref_dest_uses == total_dest_uses);
                }
                if (safe_stmt_shape && is_dead_after_stmt(use_stmt_idx, dest_var, total_dest_uses,
                                                          stmts[use_stmt_idx - 1].get())) {
                  auto sum_expr = CExpression::Binary("+", assign.Rhs().Clone(), word_idx->Clone());
                  if (stmts[use_stmt_idx]->Kind() == CStatementKind::kExpressionStatement) {
                    const auto& next_es =
                        static_cast<const ExpressionStatement&>(*stmts[use_stmt_idx]);
                    auto new_expr = SubstituteIdentifier(next_es.Expression(), dest_var, *sum_expr);
                    stmts[use_stmt_idx] = CStatement::Expression(std::move(new_expr));
                  } else if (stmts[use_stmt_idx]->Kind() == CStatementKind::kIfStatement) {
                    const auto& ifs = static_cast<const IfStatement&>(*stmts[use_stmt_idx]);
                    auto new_cond = SubstituteIdentifier(ifs.Condition(), dest_var, *sum_expr);
                    auto new_then = ifs.ThenBranch().Clone();
                    if (new_then->Kind() == CStatementKind::kCompoundStatement) {
                      auto& then_stmts =
                          static_cast<CompoundStatement*>(new_then.get())->MutableStatements();
                      for (auto& ts : then_stmts) {
                        if (ts->Kind() == CStatementKind::kExpressionStatement &&
                            count_uses(*ts, dest_var) > 0) {
                          const auto& tes = static_cast<const ExpressionStatement&>(*ts);
                          ts = CStatement::Expression(
                              SubstituteIdentifier(tes.Expression(), dest_var, *sum_expr));
                        }
                      }
                    }
                    std::unique_ptr<CStatement> new_else =
                        (ifs.ElseBranch() != nullptr) ? ifs.ElseBranch()->Clone() : nullptr;
                    stmts[use_stmt_idx] = CStatement::If(std::move(new_cond), std::move(new_then),
                                                         std::move(new_else));
                  }
                  stmts.erase(stmts.begin() + i, stmts.begin() + use_stmt_idx);
                  changed = true;
                  break;
                }
              }
            }

            // 1. Fold into ExpressionStatement (or ReturnStatement for call args)
            if (stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement ||
                ((is_stack_arg || is_call_arg) &&
                 stmts[i + 1]->Kind() == CStatementKind::kReturnStatement)) {
              bool is_fp_scratch = (var.size() >= 2 && var[0] == 'f' &&
                                    std::isdigit(static_cast<unsigned char>(var[1])));
              bool block_has_later_call = false;
              for (size_t k = i + 1; k < stmts.size() && k <= i + 20; ++k) {
                if (has_call_in_stmt(*stmts[k])) {
                  block_has_later_call = true;
                  break;
                }
              }
              bool block_has_earlier_call = false;
              for (size_t k = 0; k < i; ++k) {
                if (has_call_in_stmt(*stmts[k])) {
                  block_has_earlier_call = true;
                  break;
                }
              }
              bool block_has_any_call = block_has_later_call || block_has_earlier_call;

              // Case A: In-place redefinition (var = ...; var = ... var ...)
              size_t max_case_a_lookahead =
                  (is_call_arg || block_has_any_call) ? 10 : (is_fp_scratch ? 2 : 1);
              for (size_t j = i + 1; j < stmts.size() && j <= i + max_case_a_lookahead; ++j) {
                if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) break;
                const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) break;
                const auto& cand_assign =
                    static_cast<const AssignmentExpression&>(cand_es.Expression());
                if (cand_assign.Op() == "=" &&
                    cand_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                    static_cast<const IdentifierExpression&>(cand_assign.Lhs()).Name() == var) {
                  if (j > i + 1 && !is_call_arg && !block_has_any_call &&
                      has_memory_deref(assign.Rhs())) {
                    break;
                  }
                  int uses_in_rhs = count_expr_uses(cand_assign.Rhs(), var);
                  bool can_fold_in_place = false;
                  if (is_pure_expr(assign.Rhs()) && !is_comparison_expr(assign.Rhs())) {
                    can_fold_in_place = true;
                  } else if (is_call_or_wrapped_call(assign.Rhs()) && j == i + 1 &&
                             is_pure_expr(cand_assign.Rhs()) &&
                             !has_memory_deref(cand_assign.Rhs())) {
                    can_fold_in_place = true;
                  }
                  if (can_fold_in_place && uses_in_rhs == 1 &&
                      !has_address_of_var(cand_assign.Rhs(), var) &&
                      (!has_address_of(assign.Rhs()) ||
                       !used_in_addition_or_subtraction(cand_assign.Rhs(), var))) {
                    auto new_rhs = SubstituteIdentifier(cand_assign.Rhs(), var, assign.Rhs());
                    if (count_expr_uses(*new_rhs, var) == 0) {
                      stmts[j] = CStatement::Expression(CExpression::Assignment(
                          "=", cand_assign.Lhs().Clone(), std::move(new_rhs)));
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                      break;
                    }
                  }
                  break;
                }
                if (count_uses(*stmts[j], var) > 0) break;
                std::string_view mid_lhs_sv =
                    (cand_assign.Op() == "=") ? extract_assigned_lhs_var(cand_assign.Lhs()) : "";
                if (mid_lhs_sv.empty() || !is_pure_expr(cand_assign.Rhs())) break;
                std::string mid_lhs(mid_lhs_sv);
                if (mid_lhs == var || count_expr_uses(assign.Rhs(), mid_lhs) > 0) break;
              }
              if (changed) break;

              if (count_expr_uses(effective_rhs, var) > 0) continue;

              if (is_stack_arg || is_call_arg) {
                if (!is_pure_expr(effective_rhs)) continue;
                // Look ahead across pure stack/register argument setup statements to the call
                for (size_t j = i + 1; j < stmts.size() && j <= i + 24; ++j) {
                  const CallExpression* target_call = nullptr;
                  if (stmts[j]->Kind() == CStatementKind::kExpressionStatement) {
                    const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                    if (cand_es.Expression().Kind() == CExpressionKind::kCallExpression) {
                      target_call = static_cast<const CallExpression*>(&cand_es.Expression());
                    } else if (cand_es.Expression().Kind() ==
                               CExpressionKind::kAssignmentExpression) {
                      const auto& ca =
                          static_cast<const AssignmentExpression&>(cand_es.Expression());
                      if (ca.Op() == "=" && ca.Lhs().Kind() == CExpressionKind::kIdentifier &&
                          ca.Rhs().Kind() == CExpressionKind::kCallExpression) {
                        target_call = static_cast<const CallExpression*>(&ca.Rhs());
                      }
                    }
                  } else if (stmts[j]->Kind() == CStatementKind::kReturnStatement) {
                    const auto& ret_s = static_cast<const ReturnStatement&>(*stmts[j]);
                    if (ret_s.ReturnValue() != nullptr &&
                        ret_s.ReturnValue()->Kind() == CExpressionKind::kCallExpression) {
                      target_call = static_cast<const CallExpression*>(ret_s.ReturnValue());
                    }
                  } else {
                    break;
                  }
                  if (target_call != nullptr) {
                    int uses_in_call = count_expr_uses(*target_call, var);
                    int uses_in_stmt = count_uses(*stmts[j], var);
                    bool can_fold_arg_uses =
                        (uses_in_call == 1 && uses_in_stmt == 1) ||
                        (is_call_arg && uses_in_call > 1 && uses_in_stmt == uses_in_call &&
                         is_duplicable_arg_rhs(effective_rhs));
                    if (can_fold_arg_uses &&
                        (!has_address_of_var(*target_call, var) ||
                         is_c_var_identifier(effective_rhs)) &&
                        (!has_address_of(effective_rhs) ||
                         !used_in_addition_or_subtraction(*target_call, var)) &&
                        is_dead_after_stmt(j, var, uses_in_call, stmts[i].get())) {
                      if (stmts[j]->Kind() == CStatementKind::kReturnStatement) {
                        const auto& ret_s = static_cast<const ReturnStatement&>(*stmts[j]);
                        auto new_ret =
                            SubstituteIdentifier(*ret_s.ReturnValue(), var, effective_rhs);
                        if (count_expr_uses(*new_ret, var) == 0) {
                          stmts[j] = CStatement::Return(std::move(new_ret));
                          stmts.erase(stmts.begin() + i);
                          changed = true;
                        }
                      } else {
                        const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                        auto new_expr =
                            SubstituteIdentifier(cand_es.Expression(), var, effective_rhs);
                        if (count_expr_uses(*new_expr, var) == 0) {
                          stmts[j] = CStatement::Expression(std::move(new_expr));
                          stmts.erase(stmts.begin() + i);
                          changed = true;
                        }
                      }
                    }
                    break;
                  }
                  if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) break;
                  const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                  // Check if intermediate statement j is a safe argument setup that doesn't touch
                  // var or modify any variable referenced in effective_rhs
                  if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) break;
                  const auto& mid_assign =
                      static_cast<const AssignmentExpression&>(cand_es.Expression());
                  if (mid_assign.Op() != "=" || !is_pure_expr(mid_assign.Rhs()) ||
                      count_uses(*stmts[j], var) > 0) {
                    break;
                  }
                  std::string_view mid_lhs_sv = extract_assigned_lhs_var(mid_assign.Lhs());
                  if (!mid_lhs_sv.empty()) {
                    std::string mid_lhs(mid_lhs_sv);
                    if (mid_lhs == var || count_expr_uses(effective_rhs, mid_lhs) > 0) {
                      break;
                    }
                  } else if ((has_memory_deref(effective_rhs) &&
                              !are_all_derefs_provably_disjoint(effective_rhs, mid_assign.Lhs())) ||
                             !is_pure_expr(mid_assign.Lhs())) {
                    break;
                  }
                }
                if (changed) break;
                if (is_stack_arg) continue;
                if (!block_has_any_call) continue;
              }

              // Case B: Regular single-use folding into next (or across pure setup) expression
              // statement
              bool is_direct_call_store_rhs = (var == "v0" || var == "f0" || var == "f0_d") &&
                                              is_call_or_wrapped_call(assign.Rhs());
              bool f0_before_later_call =
                  (var == "f0" && !is_direct_call_store_rhs && block_has_any_call);
              size_t max_case_b_lookahead = ((var == "f0" && !f0_before_later_call) ||
                                             var == "f0_d" || is_direct_call_store_rhs)
                                                ? 1
                                                : (block_has_any_call ? 6 : 2);
              if (is_pure_expr(assign.Rhs()) || is_direct_call_store_rhs) {
                for (size_t j = i + 1; j < stmts.size() && j <= i + max_case_b_lookahead; ++j) {
                  size_t max_case_b_deref_lookahead = block_has_any_call ? 10 : 1;
                  if (j > i + max_case_b_deref_lookahead && has_memory_deref(assign.Rhs())) {
                    break;
                  }
                  if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) break;
                  const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
                  if (j > i + 1 && f0_before_later_call) {
                    if (cand_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                      const auto& ca =
                          static_cast<const AssignmentExpression&>(cand_es.Expression());
                      std::string_view ca_lhs = extract_assigned_lhs_var(ca.Lhs());
                      if (!is_outgoing_stack_arg_var(ca_lhs) && count_uses(*stmts[j], var) > 0) {
                        break;
                      }
                    }
                  }
                  if (is_direct_call_store_rhs) {
                    if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
                      break;
                    }
                    const auto& ca = static_cast<const AssignmentExpression&>(cand_es.Expression());
                    if (ca.Op() != "=" || ca.Lhs().Kind() == CExpressionKind::kIdentifier ||
                        !is_pure_expr(ca.Lhs()) || count_expr_uses(ca.Lhs(), var) > 0 ||
                        ca.Rhs().Kind() != CExpressionKind::kIdentifier ||
                        static_cast<const IdentifierExpression&>(ca.Rhs()).Name() != var) {
                      break;
                    }
                  }
                  if (j > i + 1 && is_fp_scratch &&
                      assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                    break;
                  }
                  if (j > i + 1 && is_fp_scratch && var != "f0" && var != "f0_d" &&
                      has_memory_deref(assign.Rhs())) {
                    if (cand_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                      const auto& ca =
                          static_cast<const AssignmentExpression&>(cand_es.Expression());
                      std::string_view ca_lhs = extract_assigned_lhs_var(ca.Lhs());
                      if (is_outgoing_stack_arg_var(ca_lhs)) {
                        break;
                      }
                    }
                  }
                  int uses_in_cand = count_uses(*stmts[j], var);
                  if (uses_in_cand == 1) {
                    if ((var == "f0" || var == "f0_d") && comp == root_body &&
                        j + 1 == stmts.size() && !has_call_in_stmt(*stmts[j])) {
                      break;
                    }
                    if (has_address_of(assign.Rhs()) &&
                        used_in_addition_or_subtraction(cand_es.Expression(), var)) {
                      break;
                    }
                    if (!is_fp_scratch &&
                        assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                      auto expr_has_u32_cast = [](const CExpression& root_e) -> bool {
                        auto impl = [&](auto& self, const CExpression& e) -> bool {
                          if (e.Kind() == CExpressionKind::kCastExpression) {
                            const auto& ce = static_cast<const CastExpression&>(e);
                            if (ce.TargetType().ToString() == "u32") return true;
                            return self(self, ce.Operand());
                          }
                          if (e.Kind() == CExpressionKind::kUnaryExpression) {
                            return self(self, static_cast<const UnaryExpression&>(e).Operand());
                          }
                          if (e.Kind() == CExpressionKind::kBinaryExpression) {
                            const auto& b = static_cast<const BinaryExpression&>(e);
                            return self(self, b.Lhs()) || self(self, b.Rhs());
                          }
                          return false;
                        };
                        return impl(impl, root_e);
                      };
                      auto expr_casts_var_to_float = [&](const CExpression& root_e) -> bool {
                        auto impl = [&](auto& self, const CExpression& e) -> bool {
                          if (e.Kind() == CExpressionKind::kCastExpression) {
                            const auto& ce = static_cast<const CastExpression&>(e);
                            std::string ts = ce.TargetType().ToString();
                            if ((ts == "f32" || ts == "f64") &&
                                ce.Operand().Kind() == CExpressionKind::kIdentifier &&
                                static_cast<const IdentifierExpression&>(ce.Operand()).Name() ==
                                    var) {
                              return true;
                            }
                            return self(self, ce.Operand());
                          }
                          if (e.Kind() == CExpressionKind::kUnaryExpression) {
                            return self(self, static_cast<const UnaryExpression&>(e).Operand());
                          }
                          if (e.Kind() == CExpressionKind::kBinaryExpression) {
                            const auto& b = static_cast<const BinaryExpression&>(e);
                            return self(self, b.Lhs()) || self(self, b.Rhs());
                          }
                          if (e.Kind() == CExpressionKind::kAssignmentExpression) {
                            const auto& a = static_cast<const AssignmentExpression&>(e);
                            return self(self, a.Lhs()) || self(self, a.Rhs());
                          }
                          if (e.Kind() == CExpressionKind::kCallExpression) {
                            const auto& c = static_cast<const CallExpression&>(e);
                            for (const auto& arg : c.Arguments()) {
                              if (arg != nullptr && self(self, *arg)) return true;
                            }
                            return false;
                          }
                          return false;
                        };
                        return impl(impl, root_e);
                      };
                      if (expr_has_u32_cast(assign.Rhs()) &&
                          expr_casts_var_to_float(cand_es.Expression())) {
                        break;
                      }
                    }
                    if (has_address_of_var(cand_es.Expression(), var) &&
                        !is_c_var_identifier(assign.Rhs())) {
                      bool is_float_arg_bitcast_assign = false;
                      if (is_fp_scratch && assign.Rhs().Kind() != CExpressionKind::kFloatLiteral) {
                        if (cand_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                          const auto& ca =
                              static_cast<const AssignmentExpression&>(cand_es.Expression());
                          if (ca.Op() == "=" && ca.Lhs().Kind() == CExpressionKind::kIdentifier &&
                              is_call_arg_var(
                                  static_cast<const IdentifierExpression&>(ca.Lhs()).Name()) &&
                              ca.Rhs().Kind() == CExpressionKind::kUnaryExpression &&
                              (static_cast<const UnaryExpression&>(ca.Rhs()).Op() == "*(s32*)&" ||
                               static_cast<const UnaryExpression&>(ca.Rhs()).Op() == "*(s32 *)&")) {
                            std::string arg_lhs_name(
                                static_cast<const IdentifierExpression&>(ca.Lhs()).Name());
                            if (count_expr_uses(assign.Rhs(), arg_lhs_name) == 0) {
                              for (size_t k = j + 1; k < stmts.size() && k <= j + 16; ++k) {
                                if (stmts[k]->Kind() != CStatementKind::kExpressionStatement) {
                                  break;
                                }
                                const auto& kes =
                                    static_cast<const ExpressionStatement&>(*stmts[k]);
                                const CallExpression* k_call = nullptr;
                                if (kes.Expression().Kind() == CExpressionKind::kCallExpression) {
                                  k_call = static_cast<const CallExpression*>(&kes.Expression());
                                } else if (kes.Expression().Kind() ==
                                           CExpressionKind::kAssignmentExpression) {
                                  const auto& ka =
                                      static_cast<const AssignmentExpression&>(kes.Expression());
                                  if (ka.Op() == "=" &&
                                      ka.Lhs().Kind() == CExpressionKind::kIdentifier &&
                                      ka.Rhs().Kind() == CExpressionKind::kCallExpression) {
                                    k_call = static_cast<const CallExpression*>(&ka.Rhs());
                                  }
                                }
                                if (k_call != nullptr) {
                                  bool arg0_is_fp =
                                      !k_call->Arguments().empty() &&
                                      k_call->Arguments()[0] != nullptr &&
                                      (k_call->Arguments()[0]->Kind() ==
                                           CExpressionKind::kFloatLiteral ||
                                       (k_call->Arguments()[0]->Kind() ==
                                            CExpressionKind::kIdentifier &&
                                        is_scratch_var(static_cast<const IdentifierExpression&>(
                                                           *k_call->Arguments()[0])
                                                           .Name()) &&
                                        static_cast<const IdentifierExpression&>(
                                            *k_call->Arguments()[0])
                                                .Name()[0] == 'f') ||
                                       (k_call->Arguments()[0]->Kind() ==
                                            CExpressionKind::kIdentifier &&
                                        absl::StartsWith(static_cast<const IdentifierExpression&>(
                                                             *k_call->Arguments()[0])
                                                             .Name(),
                                                         "f12")));
                                  bool valid_slot =
                                      !(arg0_is_fp && arg_lhs_name == "arg1") &&
                                      (!k_call->Arguments().empty() &&
                                       count_expr_uses(*k_call->Arguments()[0], arg_lhs_name) == 0);
                                  if (valid_slot && count_expr_uses(*k_call, arg_lhs_name) == 1 &&
                                      count_uses(*stmts[k], arg_lhs_name) == 1 &&
                                      is_dead_after_stmt(k, arg_lhs_name, 1, stmts[j].get())) {
                                    is_float_arg_bitcast_assign = true;
                                  }
                                  break;
                                }
                                if (has_call_in_stmt(*stmts[k]) ||
                                    count_uses(*stmts[k], arg_lhs_name) > 0 ||
                                    kes.Expression().Kind() !=
                                        CExpressionKind::kAssignmentExpression) {
                                  break;
                                }
                                const auto& mid_a =
                                    static_cast<const AssignmentExpression&>(kes.Expression());
                                std::string_view mid_lhs_sv =
                                    (mid_a.Op() == "=") ? extract_assigned_lhs_var(mid_a.Lhs())
                                                        : "";
                                if (mid_lhs_sv.empty() || !is_pure_expr(mid_a.Rhs())) {
                                  break;
                                }
                                std::string mid_lhs(mid_lhs_sv);
                                if (mid_lhs == arg_lhs_name ||
                                    count_expr_uses(assign.Rhs(), mid_lhs) > 0) {
                                  break;
                                }
                              }
                            }
                          } else if (ca.Op() == "=" &&
                                     ca.Lhs().Kind() == CExpressionKind::kIdentifier &&
                                     ca.Rhs().Kind() == CExpressionKind::kCallExpression) {
                            const auto& ce = static_cast<const CallExpression&>(ca.Rhs());
                            for (size_t ai = 1; ai < ce.Arguments().size(); ++ai) {
                              const auto& arg = ce.Arguments()[ai];
                              if (arg != nullptr &&
                                  arg->Kind() == CExpressionKind::kUnaryExpression) {
                                const auto& ue = static_cast<const UnaryExpression&>(*arg);
                                if ((ue.Op() == "*(s32*)&" || ue.Op() == "*(s32 *)&") &&
                                    ue.Operand().Kind() == CExpressionKind::kIdentifier &&
                                    static_cast<const IdentifierExpression&>(ue.Operand()).Name() ==
                                        var) {
                                  is_float_arg_bitcast_assign = true;
                                  break;
                                }
                              }
                            }
                          }
                        } else if (cand_es.Expression().Kind() ==
                                   CExpressionKind::kCallExpression) {
                          const auto& ce = static_cast<const CallExpression&>(cand_es.Expression());
                          for (size_t ai = 1; ai < ce.Arguments().size(); ++ai) {
                            const auto& arg = ce.Arguments()[ai];
                            if (arg != nullptr &&
                                arg->Kind() == CExpressionKind::kUnaryExpression) {
                              const auto& ue = static_cast<const UnaryExpression&>(*arg);
                              if ((ue.Op() == "*(s32*)&" || ue.Op() == "*(s32 *)&") &&
                                  ue.Operand().Kind() == CExpressionKind::kIdentifier &&
                                  static_cast<const IdentifierExpression&>(ue.Operand()).Name() ==
                                      var) {
                                is_float_arg_bitcast_assign = true;
                                break;
                              }
                            }
                          }
                        }
                      }
                      if (!is_float_arg_bitcast_assign) {
                        break;
                      }
                    }
                    if (cand_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                      const auto& cand_assign =
                          static_cast<const AssignmentExpression&>(cand_es.Expression());
                      auto is_stack_param_or_local_expr = [&](const CExpression& e) -> bool {
                        if (e.Kind() == CExpressionKind::kIdentifier) {
                          return is_stack_param_or_local_name(
                              static_cast<const IdentifierExpression&>(e).Name());
                        }
                        if (e.Kind() == CExpressionKind::kUnaryExpression) {
                          const auto& un = static_cast<const UnaryExpression&>(e);
                          if ((un.Op() == "*(f32*)&" || un.Op() == "*(f32 *)&") &&
                              un.Operand().Kind() == CExpressionKind::kIdentifier) {
                            return is_stack_param_or_local_name(
                                static_cast<const IdentifierExpression&>(un.Operand()).Name());
                          }
                        }
                        return false;
                      };
                      if (is_fp_scratch &&
                          cand_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                          is_stack_param_or_local_expr(assign.Rhs())) {
                        std::string_view ca_lhs =
                            static_cast<const IdentifierExpression&>(cand_assign.Lhs()).Name();
                        if (ca_lhs != var && count_expr_uses(cand_assign.Rhs(), ca_lhs) == 0) {
                          break;
                        }
                      }
                      if (cand_assign.Lhs().Kind() != CExpressionKind::kIdentifier) {
                        if (count_expr_uses(cand_assign.Lhs(), var) > 0 &&
                            !is_pure_expr(cand_assign.Rhs())) {
                          break;
                        }
                        if (absl::StartsWith(var, "temp_t") &&
                            assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
                          break;
                        }
                      }
                    }
                    if (is_dead_after_stmt(j, var, 1, stmts[i].get())) {
                      auto new_expr = SubstituteIdentifier(cand_es.Expression(), var, assign.Rhs());
                      if (count_expr_uses(*new_expr, var) == 0) {
                        stmts[j] = CStatement::Expression(std::move(new_expr));
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                      }
                    }
                    break;
                  }
                  if (uses_in_cand > 1) {
                    if (j == i + 1 && is_scratch && !is_fp_scratch &&
                        assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
                        is_call_arg_var(
                            static_cast<const IdentifierExpression&>(assign.Rhs()).Name()) &&
                        has_call_in_stmt(*stmts[j]) &&
                        !has_address_of_var(cand_es.Expression(), var) &&
                        is_dead_after_stmt(j, var, uses_in_cand, stmts[i].get())) {
                      auto new_expr = SubstituteIdentifier(cand_es.Expression(), var, assign.Rhs());
                      if (count_expr_uses(*new_expr, var) == 0) {
                        stmts[j] = CStatement::Expression(std::move(new_expr));
                        stmts.erase(stmts.begin() + i);
                        changed = true;
                      }
                    }
                    break;
                  }
                  if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) break;
                  const auto& mid_assign =
                      static_cast<const AssignmentExpression&>(cand_es.Expression());
                  if (mid_assign.Op() != "=" || !is_pure_expr(mid_assign.Rhs())) {
                    break;
                  }
                  std::string_view mid_lhs_sv = extract_assigned_lhs_var(mid_assign.Lhs());
                  if (!mid_lhs_sv.empty()) {
                    std::string mid_lhs(mid_lhs_sv);
                    if (mid_lhs == var || count_expr_uses(assign.Rhs(), mid_lhs) > 0) {
                      break;
                    }
                  } else if ((has_memory_deref(assign.Rhs()) &&
                              !are_all_derefs_provably_disjoint(assign.Rhs(), mid_assign.Lhs())) ||
                             !is_pure_expr(mid_assign.Lhs())) {
                    break;
                  }
                }
                if (changed) break;
              }
            }

            // 2, 3, 4. Fold into IfStatement, ReturnStatement, or SwitchStatement
            // (possibly across 1-3 pure register assignments like branch delay slots!)
            for (size_t j = i + 1; j < stmts.size() && j <= i + 4; ++j) {
              if (j > i + 1 && !is_pure_expr(assign.Rhs())) break;
              if (j > i + 1 && has_memory_deref(assign.Rhs())) break;

              if (stmts[j]->Kind() == CStatementKind::kIfStatement) {
                if (var == "f0" || var == "f0_d") {
                  bool is_f0_float_literal =
                      (var == "f0" && assign.Rhs().Kind() == CExpressionKind::kFloatLiteral);
                  if (!is_call_or_wrapped_call(assign.Rhs()) && !is_f0_float_literal) {
                    break;
                  }
                }
                const auto& if_stmt = static_cast<const IfStatement&>(*stmts[j]);
                // If `argX = <id_or_literal>;` (from a branch delay slot) precedes an `if`
                // without `else` whose `then` branch calls a function using `argX` once, and
                // `argX` is dead after the `if`, fold `argX` into the `then` branch call.
                if (j == i + 1 && is_call_arg && if_stmt.ElseBranch() == nullptr &&
                    count_expr_uses(if_stmt.Condition(), var) == 0 &&
                    (assign.Rhs().Kind() == CExpressionKind::kIdentifier ||
                     assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral) &&
                    if_stmt.ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
                    count_uses(if_stmt.ThenBranch(), var) == 1 &&
                    has_call_in_stmt(if_stmt.ThenBranch()) &&
                    !has_address_of_var_in_stmt(if_stmt.ThenBranch(), var) &&
                    is_dead_after_stmt(j, var, 1, stmts[i].get())) {
                  const auto& then_comp =
                      static_cast<const CompoundStatement&>(if_stmt.ThenBranch());
                  if (then_comp.Statements().size() == 1 &&
                      then_comp.Statements()[0]->Kind() == CStatementKind::kExpressionStatement) {
                    const auto& then_es =
                        static_cast<const ExpressionStatement&>(*then_comp.Statements()[0]);
                    auto new_then_expr =
                        SubstituteIdentifier(then_es.Expression(), var, assign.Rhs());
                    if (count_expr_uses(*new_then_expr, var) == 0) {
                      auto new_then = std::make_unique<CompoundStatement>();
                      new_then->AddStatement(CStatement::Expression(std::move(new_then_expr)));
                      stmts[j] =
                          CStatement::If(if_stmt.Condition().Clone(), std::move(new_then), nullptr);
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                      break;
                    }
                  }
                }
                if (has_address_of_var(if_stmt.Condition(), var) &&
                    assign.Rhs().Kind() != CExpressionKind::kIdentifier) {
                  break;
                }
                if (!is_pure_expr(assign.Rhs()) &&
                    !is_unconditionally_evaluated_first(if_stmt.Condition(), var)) {
                  break;
                }
                int uses_in_cond = count_expr_uses(if_stmt.Condition(), var);
                active_ignored_def_stmt = stmts[i].get();
                active_ignored_use_stmt = stmts[j].get();
                VarLiveness then_live =
                    analyze_var_liveness(if_stmt.ThenBranch(), var, is_switch_case_body, 0);
                VarLiveness else_live =
                    (if_stmt.ElseBranch() != nullptr)
                        ? analyze_var_liveness(*if_stmt.ElseBranch(), var, is_switch_case_body, 0)
                        : VarLiveness::kTransparent;
                active_ignored_def_stmt = nullptr;
                active_ignored_use_stmt = nullptr;
                if (uses_in_cond == 1 && then_live != VarLiveness::kReadBeforeWrite &&
                    else_live != VarLiveness::kReadBeforeWrite) {
                  bool killed_by_both = (then_live == VarLiveness::kKilledBeforeRead &&
                                         else_live == VarLiveness::kKilledBeforeRead);
                  if (killed_by_both || is_dead_after_stmt(j, var, 1, stmts[i].get())) {
                    auto new_cond = SubstituteIdentifier(if_stmt.Condition(), var, assign.Rhs());
                    if (count_expr_uses(*new_cond, var) == 0) {
                      auto new_then = if_stmt.ThenBranch().Clone();
                      std::unique_ptr<CStatement> new_else = (if_stmt.ElseBranch() != nullptr)
                                                                 ? if_stmt.ElseBranch()->Clone()
                                                                 : nullptr;
                      stmts[j] = CStatement::If(std::move(new_cond), std::move(new_then),
                                                std::move(new_else));
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                    }
                  }
                }
                break;
              }

              if (stmts[j]->Kind() == CStatementKind::kReturnStatement) {
                const auto& ret_stmt = static_cast<const ReturnStatement&>(*stmts[j]);
                if (ret_stmt.ReturnValue() != nullptr) {
                  int uses_in_ret = count_expr_uses(*ret_stmt.ReturnValue(), var);
                  bool is_direct_id =
                      (ret_stmt.ReturnValue()->Kind() == CExpressionKind::kIdentifier &&
                       static_cast<const IdentifierExpression&>(*ret_stmt.ReturnValue()).Name() ==
                           var);
                  bool can_fold_ret =
                      is_direct_id ||
                      (uses_in_ret == 1 && !is_comparison_expr(assign.Rhs()) &&
                       (is_pure_expr(assign.Rhs()) ||
                        (is_call_or_wrapped_call(assign.Rhs()) &&
                         is_pure_expr(*ret_stmt.ReturnValue()) &&
                         !has_memory_deref(*ret_stmt.ReturnValue()))) &&
                       !has_address_of_var(*ret_stmt.ReturnValue(), var) &&
                       (!has_address_of(assign.Rhs()) ||
                        !used_in_addition_or_subtraction(*ret_stmt.ReturnValue(), var)));
                  if (can_fold_ret && is_dead_after_stmt(j, var, 1, stmts[i].get())) {
                    auto new_ret = SubstituteIdentifier(*ret_stmt.ReturnValue(), var, assign.Rhs());
                    if (count_expr_uses(*new_ret, var) == 0) {
                      stmts[j] = CStatement::Return(std::move(new_ret));
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                    }
                  }
                }
                break;
              }

              if (stmts[j]->Kind() == CStatementKind::kSwitchStatement) {
                const auto& sw_stmt = static_cast<const SwitchStatement&>(*stmts[j]);
                if (has_address_of_var(sw_stmt.Condition(), var) &&
                    assign.Rhs().Kind() != CExpressionKind::kIdentifier) {
                  break;
                }
                if (!is_pure_expr(assign.Rhs()) &&
                    !is_unconditionally_evaluated_first(sw_stmt.Condition(), var)) {
                  break;
                }
                int uses_in_cond = count_expr_uses(sw_stmt.Condition(), var);
                bool read_in_cases = false;
                active_ignored_def_stmt = stmts[i].get();
                active_ignored_use_stmt = stmts[j].get();
                for (const auto& sc : sw_stmt.Cases()) {
                  if (sc.body != nullptr &&
                      analyze_var_liveness(*sc.body, var, /*in_switch_case_top=*/true, 0) ==
                          VarLiveness::kReadBeforeWrite) {
                    read_in_cases = true;
                    break;
                  }
                }
                active_ignored_def_stmt = nullptr;
                active_ignored_use_stmt = nullptr;
                if (uses_in_cond == 1 && !read_in_cases) {
                  if (is_dead_after_stmt(j, var, 1, stmts[i].get())) {
                    auto new_cond = SubstituteIdentifier(sw_stmt.Condition(), var, assign.Rhs());
                    if (count_expr_uses(*new_cond, var) == 0) {
                      std::vector<SwitchCase> new_cases;
                      for (const auto& sc : sw_stmt.Cases()) {
                        new_cases.push_back(sc.Clone());
                      }
                      stmts[j] = CStatement::Switch(std::move(new_cond), std::move(new_cases));
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                    }
                  }
                }
                break;
              }

              // Intermediate statement j must be a pure assignment to a different variable
              if (stmts[j]->Kind() != CStatementKind::kExpressionStatement) break;
              const auto& cand_es = static_cast<const ExpressionStatement&>(*stmts[j]);
              if (count_uses(*stmts[j], var) > 0) break;
              if (cand_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) break;
              const auto& mid_assign =
                  static_cast<const AssignmentExpression&>(cand_es.Expression());
              std::string_view mid_lhs_sv =
                  (mid_assign.Op() == "=") ? extract_assigned_lhs_var(mid_assign.Lhs()) : "";
              if (mid_lhs_sv.empty() || !is_pure_expr(mid_assign.Rhs())) {
                break;
              }
              std::string mid_lhs(mid_lhs_sv);
              if (mid_lhs == var || count_expr_uses(assign.Rhs(), mid_lhs) > 0) {
                break;
              }
            }
            if (changed) break;
          }
        }
      }
    }
  }

  // Recurse into direct child compound statements once (no exponential ForEachSubCompoundStatement
  // re-traversal!).
  {
    if (root_has_label_cached == -1) {
      root_has_label_cached = (root_body != nullptr && has_label_in_stmt(*root_body)) ? 1 : 0;
    }
    static constexpr std::string_view kTrackedVars[] = {
        "v0",        "v1",        "temp_t0",   "temp_t1",   "temp_t2",   "temp_t3",   "temp_t4",
        "temp_t5",   "temp_t6",   "temp_t7",   "temp_t8",   "temp_t9",   "temp_at",   "fcond",
        "f0",        "f0_d",      "f2",        "f2_d",      "f4",        "f4_d",      "f6",
        "f6_d",      "f8",        "f8_d",      "f10",       "f10_d",     "f12",       "f12_d",
        "f14",       "f14_d",     "f16",       "f16_d",     "f18",       "f18_d",     "arg0",
        "arg1",      "arg2",      "arg3",      "var_sp_16", "var_sp_20", "var_sp_24", "var_sp_28",
        "var_sp_32", "var_sp_36", "var_sp_40", "var_sp_44", "var_sp_48", "var_sp_52", "var_sp_56",
        "var_sp_60",
    };
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i]->Kind() == CStatementKind::kIfStatement ||
          stmts[i]->Kind() == CStatementKind::kSwitchStatement) {
        absl::flat_hash_set<std::string> child_dead_vars;
        for (std::string_view tv : kTrackedVars) {
          std::string tv_str(tv);
          if (is_dead_after_stmt(i, tv_str, 0)) {
            child_dead_vars.insert(std::move(tv_str));
          }
        }
        if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
          auto& ifs = static_cast<IfStatement&>(*stmts[i]);
          if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
            FoldSingleUseExpressions(static_cast<CompoundStatement*>(ifs.MutableThenBranch()),
                                     root_body, &child_dead_vars, false, root_has_label_cached);
          }
          if (ifs.MutableElseBranch() != nullptr) {
            if (ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
              FoldSingleUseExpressions(static_cast<CompoundStatement*>(ifs.MutableElseBranch()),
                                       root_body, &child_dead_vars, false, root_has_label_cached);
            } else if (ifs.MutableElseBranch()->Kind() == CStatementKind::kIfStatement) {
              CompoundStatement wrapper;
              wrapper.AddStatement(ifs.MutableElseBranch()->Clone());
              FoldSingleUseExpressions(&wrapper, root_body, &child_dead_vars, false,
                                       root_has_label_cached);
              if (wrapper.Statements().size() == 1) {
                stmts[i] = CStatement::If(ifs.Condition().Clone(), ifs.ThenBranch().Clone(),
                                          std::move(wrapper.MutableStatements()[0]));
              }
            }
          }
          // Hoist identical single-statement `else` branch duplicated by branch-likely (`beql`)
          // target-stealing when `then` contains a function call (`if (c) { ...; S; } else { S; }`
          // -> `if (c) { ... } S;`).
          auto& cur_ifs = static_cast<IfStatement&>(*stmts[i]);
          if (cur_ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement &&
              cur_ifs.MutableElseBranch() != nullptr &&
              cur_ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
              has_call_in_stmt(*cur_ifs.MutableThenBranch())) {
            auto& then_comp = static_cast<CompoundStatement&>(*cur_ifs.MutableThenBranch());
            auto& else_comp = static_cast<CompoundStatement&>(*cur_ifs.MutableElseBranch());
            if (then_comp.Statements().size() >= 2 && else_comp.Statements().size() == 1 &&
                else_comp.Statements()[0]->Kind() == CStatementKind::kExpressionStatement &&
                !has_call_in_stmt(*else_comp.Statements()[0]) &&
                then_comp.Statements().back()->ToString() ==
                    else_comp.Statements()[0]->ToString()) {
              auto hoisted_stmt = std::move(else_comp.MutableStatements()[0]);
              then_comp.MutableStatements().pop_back();
              stmts[i] = CStatement::If(cur_ifs.Condition().Clone(),
                                        cur_ifs.MutableThenBranch()->Clone(), nullptr);
              stmts.insert(stmts.begin() + i + 1, std::move(hoisted_stmt));
              changed = true;
              break;
            }
          }
        } else {
          auto& sw = static_cast<SwitchStatement&>(*stmts[i]);
          for (auto& sc : sw.MutableCases()) {
            if (sc.body != nullptr) {
              FoldSingleUseExpressions(sc.body.get(), root_body, &child_dead_vars, true,
                                       root_has_label_cached);
            }
          }
        }
      } else if (stmts[i]->Kind() == CStatementKind::kWhileStatement) {
        auto& ws = static_cast<WhileStatement&>(*stmts[i]);
        if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          auto* body_comp = static_cast<CompoundStatement*>(ws.MutableBody());
          absl::flat_hash_set<std::string> loop_dead_vars;
          if (has_call_in_stmt(*body_comp)) {
            for (std::string_view tv : kTrackedVars) {
              std::string tv_str(tv);
              if (count_expr_uses(ws.Condition(), tv_str) == 0 &&
                  analyze_var_liveness(*body_comp, tv_str, false, 0) ==
                      VarLiveness::kKilledBeforeRead &&
                  is_dead_after_stmt(i, tv_str, 0)) {
                loop_dead_vars.insert(std::move(tv_str));
              }
            }
          }
          FoldSingleUseExpressions(body_comp, root_body,
                                   loop_dead_vars.empty() ? nullptr : &loop_dead_vars, false,
                                   root_has_label_cached);
        }
      } else if (stmts[i]->Kind() == CStatementKind::kDoWhileStatement) {
        auto& dws = static_cast<DoWhileStatement&>(*stmts[i]);
        if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          auto* body_comp = static_cast<CompoundStatement*>(dws.MutableBody());
          absl::flat_hash_set<std::string> loop_dead_vars;
          if (has_call_in_stmt(*body_comp)) {
            for (std::string_view tv : kTrackedVars) {
              std::string tv_str(tv);
              if (count_expr_uses(dws.Condition(), tv_str) == 0 &&
                  analyze_var_liveness(*body_comp, tv_str, false, 0) ==
                      VarLiveness::kKilledBeforeRead &&
                  is_dead_after_stmt(i, tv_str, 0)) {
                loop_dead_vars.insert(std::move(tv_str));
              }
            }
          }
          FoldSingleUseExpressions(body_comp, root_body,
                                   loop_dead_vars.empty() ? nullptr : &loop_dead_vars, false,
                                   root_has_label_cached);
        }
      } else if (stmts[i]->Kind() == CStatementKind::kForStatement) {
        auto& fs = static_cast<ForStatement&>(*stmts[i]);
        if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
          FoldSingleUseExpressions(static_cast<CompoundStatement*>(fs.MutableBody()), root_body,
                                   nullptr, false, root_has_label_cached);
        }
      } else if (stmts[i]->Kind() == CStatementKind::kCompoundStatement) {
        FoldSingleUseExpressions(static_cast<CompoundStatement*>(stmts[i].get()), root_body,
                                 dead_at_end_vars, is_switch_case_body, root_has_label_cached);
      }
    }
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

void SinkCommonBranchTailArgAssignments(CompoundStatement* comp) {
  if (comp == nullptr) return;
  auto extract_arg_assign = [](const CStatement& s, std::string* lhs_out,
                               std::string* rhs_str_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
    std::string lhs = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
    if (lhs != "arg0" && lhs != "arg1" && lhs != "arg2" && lhs != "arg3" &&
        !absl::StartsWith(lhs, "var_sp_")) {
      return false;
    }
    if (a.Rhs().Kind() != CExpressionKind::kIdentifier &&
        a.Rhs().Kind() != CExpressionKind::kIntegerLiteral &&
        a.Rhs().Kind() != CExpressionKind::kFloatLiteral &&
        !(a.Rhs().Kind() == CExpressionKind::kUnaryExpression &&
          (static_cast<const UnaryExpression&>(a.Rhs()).Op() == "*(s32*)&" ||
           static_cast<const UnaryExpression&>(a.Rhs()).Op() == "*(s32 *)&"))) {
      return false;
    }
    *lhs_out = std::move(lhs);
    *rhs_str_out = a.Rhs().ToString();
    return true;
  };

  auto is_simple_reg_assign = [](const CStatement& s, std::string* lhs_out) -> bool {
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() != CExpressionKind::kAssignmentExpression) return false;
    const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
    if (a.Op() != "=" || a.Lhs().Kind() != CExpressionKind::kIdentifier) return false;
    *lhs_out = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
    return true;
  };

  auto& stmts = comp->MutableStatements();
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->Kind() == CStatementKind::kIfStatement) {
      auto& ifs = static_cast<IfStatement&>(*stmts[i]);
      if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(
            static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
      }
      if (ifs.MutableElseBranch() != nullptr &&
          ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(
            static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
      }
      if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement &&
          ifs.MutableElseBranch() != nullptr &&
          ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
        auto* then_comp = static_cast<CompoundStatement*>(ifs.MutableThenBranch());
        auto* else_comp = static_cast<CompoundStatement*>(ifs.MutableElseBranch());
        auto& then_stmts = then_comp->MutableStatements();
        auto& else_stmts = else_comp->MutableStatements();
        bool sunk_any = true;
        while (sunk_any && !then_stmts.empty() && !else_stmts.empty()) {
          sunk_any = false;
          size_t then_min = then_stmts.size() > 4 ? then_stmts.size() - 4 : 0;
          size_t else_min = else_stmts.size() > 4 ? else_stmts.size() - 4 : 0;
          for (size_t ti = then_stmts.size(); ti > then_min; --ti) {
            std::string t_lhs;
            std::string t_rhs;
            if (!extract_arg_assign(*then_stmts[ti - 1], &t_lhs, &t_rhs)) continue;
            bool t_blocked = false;
            for (size_t tk = ti; tk < then_stmts.size(); ++tk) {
              std::string after_lhs;
              if (!is_simple_reg_assign(*then_stmts[tk], &after_lhs) || after_lhs == t_lhs ||
                  absl::StrContains(then_stmts[tk]->ToString(), t_lhs) ||
                  absl::StrContains(t_rhs, after_lhs)) {
                t_blocked = true;
                break;
              }
            }
            if (t_blocked) continue;

            for (size_t ei = else_stmts.size(); ei > else_min; --ei) {
              std::string e_lhs;
              std::string e_rhs;
              if (!extract_arg_assign(*else_stmts[ei - 1], &e_lhs, &e_rhs)) continue;
              if (e_lhs != t_lhs || e_rhs != t_rhs) continue;
              bool e_blocked = false;
              for (size_t ek = ei; ek < else_stmts.size(); ++ek) {
                std::string after_lhs;
                if (!is_simple_reg_assign(*else_stmts[ek], &after_lhs) || after_lhs == e_lhs ||
                    absl::StrContains(else_stmts[ek]->ToString(), e_lhs) ||
                    absl::StrContains(e_rhs, after_lhs)) {
                  e_blocked = true;
                  break;
                }
              }
              if (e_blocked) continue;

              auto sunk_stmt = then_stmts[ti - 1]->Clone();
              then_stmts.erase(then_stmts.begin() + (ti - 1));
              else_stmts.erase(else_stmts.begin() + (ei - 1));
              stmts.insert(stmts.begin() + i + 1, std::move(sunk_stmt));
              sunk_any = true;
              break;
            }
            if (sunk_any) break;
          }
        }
      }
    } else if (stmts[i]->Kind() == CStatementKind::kSwitchStatement) {
      auto& sw = static_cast<SwitchStatement&>(*stmts[i]);
      for (auto& sc : sw.MutableCases()) {
        if (sc.body != nullptr) SinkCommonBranchTailArgAssignments(sc.body.get());
      }
    } else if (stmts[i]->Kind() == CStatementKind::kWhileStatement) {
      auto& ws = static_cast<WhileStatement&>(*stmts[i]);
      if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(static_cast<CompoundStatement*>(ws.MutableBody()));
      }
    } else if (stmts[i]->Kind() == CStatementKind::kDoWhileStatement) {
      auto& dws = static_cast<DoWhileStatement&>(*stmts[i]);
      if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(static_cast<CompoundStatement*>(dws.MutableBody()));
      }
    } else if (stmts[i]->Kind() == CStatementKind::kForStatement) {
      auto& fs = static_cast<ForStatement&>(*stmts[i]);
      if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        SinkCommonBranchTailArgAssignments(static_cast<CompoundStatement*>(fs.MutableBody()));
      }
    } else if (stmts[i]->Kind() == CStatementKind::kCompoundStatement) {
      SinkCommonBranchTailArgAssignments(static_cast<CompoundStatement*>(stmts[i].get()));
    }
  }
}

void InlineCrossJumpedCallTails(CompoundStatement* root_body) {
  if (root_body == nullptr) return;

  struct LabelSite {
    CompoundStatement* comp = nullptr;
    std::string label_name;
    std::vector<std::unique_ptr<CStatement>> tail_stmts;
  };

  auto is_simple_call_or_assign_stmt = [](const CStatement& s, bool* out_has_call) -> bool {
    if (s.Kind() == CStatementKind::kReturnStatement) {
      const auto& ret = static_cast<const ReturnStatement&>(s);
      if (ret.ReturnValue() == nullptr) return true;
      return ret.ReturnValue()->Kind() == CExpressionKind::kIdentifier ||
             ret.ReturnValue()->Kind() == CExpressionKind::kIntegerLiteral;
    }
    if (s.Kind() != CStatementKind::kExpressionStatement) return false;
    const auto& es = static_cast<const ExpressionStatement&>(s);
    if (es.Expression().Kind() == CExpressionKind::kCallExpression) {
      *out_has_call = true;
      return true;
    }
    if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
        if (a.Rhs().Kind() == CExpressionKind::kCallExpression) {
          *out_has_call = true;
          return true;
        }
        if (a.Rhs().Kind() == CExpressionKind::kIdentifier ||
            a.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
          return true;
        }
      }
    }
    return false;
  };

  std::vector<LabelSite> candidates;
  std::function<void(CompoundStatement*, bool)> collect_labels = [&](CompoundStatement* comp,
                                                                     bool is_root) {
    if (comp == nullptr) return;
    auto& stmts = comp->MutableStatements();
    for (size_t i = 0; i < stmts.size(); ++i) {
      CStatement* s = stmts[i].get();
      if (s == nullptr) continue;
      if (!is_root && s->Kind() == CStatementKind::kLabelStatement) {
        std::string lbl(static_cast<const LabelStatement&>(*s).Label());
        size_t end_idx = stmts.size();
        if (end_idx > i + 1 && stmts[end_idx - 1]->Kind() == CStatementKind::kBreakStatement) {
          --end_idx;
        }
        size_t tail_count = end_idx - (i + 1);
        if (tail_count >= 1 && tail_count <= 3) {
          bool has_call = false;
          bool all_valid = true;
          std::vector<std::unique_ptr<CStatement>> tail;
          for (size_t k = i + 1; k < end_idx; ++k) {
            if (!is_simple_call_or_assign_stmt(*stmts[k], &has_call)) {
              all_valid = false;
              break;
            }
            tail.push_back(stmts[k]->Clone());
          }
          if (all_valid && has_call) {
            candidates.push_back(LabelSite{
                .comp = comp, .label_name = std::move(lbl), .tail_stmts = std::move(tail)});
          }
        }
      } else if (s->Kind() == CStatementKind::kIfStatement) {
        auto& ifs = static_cast<IfStatement&>(*s);
        if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
          collect_labels(static_cast<CompoundStatement*>(ifs.MutableThenBranch()), false);
        }
        if (ifs.MutableElseBranch() != nullptr &&
            ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          collect_labels(static_cast<CompoundStatement*>(ifs.MutableElseBranch()), false);
        }
      } else if (s->Kind() == CStatementKind::kSwitchStatement) {
        for (auto& sc : static_cast<SwitchStatement&>(*s).MutableCases()) {
          if (sc.body != nullptr) collect_labels(sc.body.get(), false);
        }
      } else if (s->Kind() == CStatementKind::kCompoundStatement) {
        collect_labels(static_cast<CompoundStatement*>(s), false);
      }
    }
  };
  collect_labels(root_body, true);

  for (const auto& site : candidates) {
    int total_gotos = 0;
    int valid_tail_gotos = 0;
    std::function<void(const CStatement&)> check_gotos = [&](const CStatement& s) {
      if (s.Kind() == CStatementKind::kGotoStatement) {
        if (static_cast<const GotoStatement&>(s).Label() == site.label_name) {
          ++total_gotos;
        }
      } else if (s.Kind() == CStatementKind::kCompoundStatement) {
        const auto& cs = static_cast<const CompoundStatement&>(s).Statements();
        for (size_t i = 0; i < cs.size(); ++i) {
          if (cs[i]->Kind() == CStatementKind::kGotoStatement &&
              static_cast<const GotoStatement&>(*cs[i]).Label() == site.label_name) {
            ++total_gotos;
            if (i + 1 == cs.size() ||
                (i + 2 == cs.size() && cs[i + 1]->Kind() == CStatementKind::kBreakStatement)) {
              ++valid_tail_gotos;
            }
          } else {
            check_gotos(*cs[i]);
          }
        }
      } else if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        check_gotos(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) check_gotos(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
          if (sc.body != nullptr) check_gotos(*sc.body);
        }
      } else if (s.Kind() == CStatementKind::kWhileStatement) {
        check_gotos(static_cast<const WhileStatement&>(s).Body());
      } else if (s.Kind() == CStatementKind::kDoWhileStatement) {
        check_gotos(static_cast<const DoWhileStatement&>(s).Body());
      } else if (s.Kind() == CStatementKind::kForStatement) {
        check_gotos(static_cast<const ForStatement&>(s).Body());
      }
    };
    check_gotos(*root_body);

    if (total_gotos == 0 || total_gotos != valid_tail_gotos) continue;

    int replaced_gotos = 0;
    std::function<void(CompoundStatement*)> replace_gotos = [&](CompoundStatement* comp) {
      if (comp == nullptr) return;
      auto& stmts = comp->MutableStatements();
      for (size_t i = 0; i < stmts.size(); ++i) {
        CStatement* s = stmts[i].get();
        if (s == nullptr) continue;
        if (s->Kind() == CStatementKind::kGotoStatement &&
            static_cast<const GotoStatement&>(*s).Label() == site.label_name) {
          stmts.erase(stmts.begin() + i);
          for (size_t k = 0; k < site.tail_stmts.size(); ++k) {
            stmts.insert(stmts.begin() + i + k, site.tail_stmts[k]->Clone());
          }
          ++replaced_gotos;
          return;
        }
        if (s->Kind() == CStatementKind::kIfStatement) {
          auto& ifs = static_cast<IfStatement&>(*s);
          if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
          }
          if (ifs.MutableElseBranch() != nullptr &&
              ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
          }
        } else if (s->Kind() == CStatementKind::kSwitchStatement) {
          for (auto& sc : static_cast<SwitchStatement&>(*s).MutableCases()) {
            if (sc.body != nullptr) replace_gotos(sc.body.get());
          }
        } else if (s->Kind() == CStatementKind::kWhileStatement) {
          auto& ws = static_cast<WhileStatement&>(*s);
          if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(ws.MutableBody()));
          }
        } else if (s->Kind() == CStatementKind::kDoWhileStatement) {
          auto& dws = static_cast<DoWhileStatement&>(*s);
          if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(dws.MutableBody()));
          }
        } else if (s->Kind() == CStatementKind::kForStatement) {
          auto& fs = static_cast<ForStatement&>(*s);
          if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
            replace_gotos(static_cast<CompoundStatement*>(fs.MutableBody()));
          }
        } else if (s->Kind() == CStatementKind::kCompoundStatement) {
          replace_gotos(static_cast<CompoundStatement*>(s));
        }
      }
    };
    replace_gotos(root_body);

    total_gotos = 0;
    check_gotos(*root_body);
    if (total_gotos == 0 && replaced_gotos > 0) {
      auto& label_stmts = site.comp->MutableStatements();
      for (size_t i = 0; i < label_stmts.size(); ++i) {
        if (label_stmts[i]->Kind() == CStatementKind::kLabelStatement &&
            static_cast<const LabelStatement&>(*label_stmts[i]).Label() == site.label_name) {
          label_stmts.erase(label_stmts.begin() + i);
          break;
        }
      }
    }
  }
}

void SplitReusedFloatGprParameters(CompoundStatement* comp,
                                   const std::vector<CParameter>& parameters) {
  if (comp == nullptr) return;

  std::function<std::unique_ptr<CStatement>(std::unique_ptr<CStatement>, std::string_view,
                                            std::string_view)>
      rewrite_stmt = [&](std::unique_ptr<CStatement> s, std::string_view from,
                         std::string_view to) -> std::unique_ptr<CStatement> {
    if (s == nullptr) return nullptr;
    auto to_id = CExpression::Identifier(std::string(to));
    switch (s->Kind()) {
      case CStatementKind::kCompoundStatement: {
        auto* c = static_cast<CompoundStatement*>(s.get());
        for (auto& child : c->MutableStatements()) {
          child = rewrite_stmt(std::move(child), from, to);
        }
        return s;
      }
      case CStatementKind::kExpressionStatement: {
        const auto& es = static_cast<const ExpressionStatement&>(*s);
        return CStatement::Expression(SubstituteIdentifier(es.Expression(), from, *to_id));
      }
      case CStatementKind::kReturnStatement: {
        const auto& ret = static_cast<const ReturnStatement&>(*s);
        if (ret.ReturnValue() != nullptr) {
          return CStatement::Return(SubstituteIdentifier(*ret.ReturnValue(), from, *to_id));
        }
        return s;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(*s);
        auto new_cond = SubstituteIdentifier(ifs.Condition(), from, *to_id);
        auto new_then = rewrite_stmt(ifs.ThenBranch().Clone(), from, to);
        std::unique_ptr<CStatement> new_else =
            ifs.ElseBranch() ? rewrite_stmt(ifs.ElseBranch()->Clone(), from, to) : nullptr;
        return CStatement::If(std::move(new_cond), std::move(new_then), std::move(new_else));
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(*s);
        return CStatement::While(SubstituteIdentifier(ws.Condition(), from, *to_id),
                                 rewrite_stmt(ws.Body().Clone(), from, to));
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(*s);
        return CStatement::DoWhile(rewrite_stmt(dws.Body().Clone(), from, to),
                                   SubstituteIdentifier(dws.Condition(), from, *to_id));
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(*s);
        auto new_init = fs.Init() ? rewrite_stmt(fs.Init()->Clone(), from, to) : nullptr;
        auto new_cond =
            fs.Condition() ? SubstituteIdentifier(*fs.Condition(), from, *to_id) : nullptr;
        auto new_step = fs.Step() ? SubstituteIdentifier(*fs.Step(), from, *to_id) : nullptr;
        return CStatement::For(std::move(new_init), std::move(new_cond), std::move(new_step),
                               rewrite_stmt(fs.Body().Clone(), from, to));
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(*s);
        auto new_cond = SubstituteIdentifier(ss.Condition(), from, *to_id);
        std::vector<SwitchCase> new_cases;
        for (const auto& c : ss.Cases()) {
          SwitchCase sc = c.Clone();
          if (c.body != nullptr) {
            auto rb = rewrite_stmt(c.body->Clone(), from, to);
            sc.body =
                std::unique_ptr<CompoundStatement>(static_cast<CompoundStatement*>(rb.release()));
          }
          new_cases.push_back(std::move(sc));
        }
        return CStatement::Switch(std::move(new_cond), std::move(new_cases));
      }
      default:
        return s;
    }
  };

  auto& stmts = comp->MutableStatements();
  for (const auto& p : parameters) {
    if (p.type.ToString() != "f32" || (p.name != "arg1" && p.name != "arg2" && p.name != "arg3")) {
      continue;
    }
    bool seen_float_read = false;
    std::optional<size_t> split_start_idx;
    for (size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i]->Kind() == CStatementKind::kExpressionStatement) {
        const auto& es = static_cast<const ExpressionStatement&>(*stmts[i]);
        if (es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(es.Expression());
          if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier &&
              static_cast<const IdentifierExpression&>(a.Lhs()).Name() == p.name) {
            if (seen_float_read && !absl::StrContains(a.Rhs().ToString(), p.name)) {
              split_start_idx = i;
            }
            break;
          }
        }
      }
      if (absl::StrContains(stmts[i]->ToString(), p.name)) {
        seen_float_read = true;
      }
    }
    if (split_start_idx.has_value()) {
      std::string temp_name = absl::StrCat("temp_a", p.name.substr(3));
      for (size_t k = *split_start_idx; k < stmts.size(); ++k) {
        stmts[k] = rewrite_stmt(std::move(stmts[k]), p.name, temp_name);
      }
    }
  }
}

void RewriteFloatInGprAndStackCallArguments(CompoundStatement* comp) {
  if (comp == nullptr) return;
  auto is_fp_var_name = [](std::string_view name) -> bool {
    if (name.size() < 2 || name[0] != 'f' || !std::isdigit(name[1])) return false;
    for (size_t i = 1; i < name.size(); ++i) {
      if (!std::isdigit(name[i])) {
        return i == name.size() - 2 && name[i] == '_' && name[i + 1] == 'd';
      }
    }
    return true;
  };

  std::function<bool(const CExpression&)> is_float_arg0 = [&](const CExpression& arg0) -> bool {
    if (arg0.Kind() == CExpressionKind::kFloatLiteral) return true;
    if (arg0.Kind() == CExpressionKind::kIdentifier) {
      return is_fp_var_name(static_cast<const IdentifierExpression&>(arg0).Name());
    }
    if (arg0.Kind() == CExpressionKind::kCastExpression) {
      const auto& ct = static_cast<const CastExpression&>(arg0).TargetType();
      return ct.pointer_depth == 0 && (ct.name == "f32" || ct.name == "f64");
    }
    if (arg0.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(arg0);
      if (un.Op() == "-" || un.Op() == "(f32)" || un.Op() == "(f64)" || un.Op() == "*(f32*)&" ||
          un.Op() == "*(f32 *)&" || un.Op() == "*(f64*)&" || un.Op() == "*(f64 *)&") {
        return true;
      }
      if (un.Op() == "*" && un.Operand().Kind() == CExpressionKind::kCastExpression) {
        const auto& ct = static_cast<const CastExpression&>(un.Operand()).TargetType();
        return ct.pointer_depth == 1 && (ct.name == "f32" || ct.name == "f64");
      }
    }
    if (arg0.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(arg0);
      if (bin.Op() == "+" || bin.Op() == "-" || bin.Op() == "*" || bin.Op() == "/") {
        return is_float_arg0(bin.Lhs()) || is_float_arg0(bin.Rhs());
      }
    }
    return false;
  };

  auto extract_s32_bitcast_of_f32 = [&](const CExpression& arg) -> std::unique_ptr<CExpression> {
    if (arg.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(arg);
      if (un.Op() == "*(s32*)&" || un.Op() == "*(s32 *)&") {
        const CExpression& op = un.Operand();
        if (op.Kind() == CExpressionKind::kIdentifier) {
          std::string_view op_name = static_cast<const IdentifierExpression&>(op).Name();
          if ((is_fp_var_name(op_name) && !absl::EndsWith(op_name, "_d")) ||
              (!op_name.empty() && !std::isalpha(static_cast<unsigned char>(op_name[0])) &&
               op_name[0] != '_')) {
            return op.Clone();
          }
        } else if (op.Kind() == CExpressionKind::kFloatLiteral) {
          return op.Clone();
        } else if (op.Kind() == CExpressionKind::kCastExpression) {
          const auto& ct = static_cast<const CastExpression&>(op).TargetType();
          if (ct.pointer_depth == 0 && ct.name == "f32") {
            return op.Clone();
          }
        } else if (op.Kind() == CExpressionKind::kUnaryExpression) {
          const auto& inner_u = static_cast<const UnaryExpression&>(op);
          if (inner_u.Op() == "(f32)" || inner_u.Op() == "*(f32*)&" ||
              inner_u.Op() == "*(f32 *)&") {
            return op.Clone();
          }
          if (inner_u.Op() == "*" && inner_u.Operand().Kind() == CExpressionKind::kCastExpression) {
            const auto& ct = static_cast<const CastExpression&>(inner_u.Operand()).TargetType();
            if (ct.pointer_depth == 1 &&
                (ct.name == "f32" || ct.name == "const f32" || ct.name == "volatile f32")) {
              return op.Clone();
            }
          }
        } else if (op.Kind() == CExpressionKind::kBinaryExpression) {
          return op.Clone();
        }
      }
    }
    return nullptr;
  };

  auto rewrite_call = [&](const CallExpression& call,
                          std::string_view ret_type) -> std::unique_ptr<CExpression> {
    if (call.Callee().Kind() != CExpressionKind::kIdentifier) return nullptr;
    std::string callee_name = static_cast<const IdentifierExpression&>(call.Callee()).Name();
    if (callee_name.empty() || callee_name == "N64Sqrtf" || callee_name == "N64Sqrt" ||
        callee_name == "fabsf" || callee_name == "fabs" || callee_name == "roundf" ||
        callee_name == "round" || callee_name == "ceilf" || callee_name == "ceil" ||
        callee_name == "floorf" || callee_name == "floor" || callee_name == "truncf" ||
        callee_name == "trunc") {
      return nullptr;
    }
    const auto& args = call.Arguments();
    if (args.empty()) return nullptr;
    const bool arg0_is_float = is_float_arg0(*args[0]);

    std::vector<std::string> existing_param_types;
    if (absl::StartsWith(callee_name, "((")) {
      size_t params_open = callee_name.find("(*)(");
      size_t params_close = callee_name.rfind("))");
      if (params_open == std::string::npos || params_close == std::string::npos ||
          params_close <= params_open + 4) {
        return nullptr;
      }
      std::string params_str =
          callee_name.substr(params_open + 4, params_close - (params_open + 4));
      size_t pos = 0;
      while (pos <= params_str.size()) {
        size_t comma = params_str.find(',', pos);
        size_t end = (comma == std::string::npos) ? params_str.size() : comma;
        while (pos < end && params_str[pos] == ' ') ++pos;
        size_t r = end;
        while (r > pos && params_str[r - 1] == ' ') --r;
        existing_param_types.push_back(params_str.substr(pos, r - pos));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      if (existing_param_types.size() != args.size()) {
        return nullptr;
      }
      callee_name = callee_name.substr(params_close + 2);
      if (!callee_name.empty() && callee_name.back() == ')') {
        callee_name.pop_back();
      }
    }

    std::function<std::string(const CExpression&)> infer_fp_arg_type =
        [&](const CExpression& e) -> std::string {
      if (e.Kind() == CExpressionKind::kIdentifier &&
          absl::EndsWith(static_cast<const IdentifierExpression&>(e).Name(), "_d")) {
        return "f64";
      }
      if (e.Kind() == CExpressionKind::kCastExpression &&
          static_cast<const CastExpression&>(e).TargetType().name == "f64") {
        return "f64";
      }
      if (e.Kind() == CExpressionKind::kUnaryExpression) {
        const auto& un = static_cast<const UnaryExpression&>(e);
        if (un.Op() == "(f64)" || un.Op() == "*(f64*)&" || un.Op() == "*(f64 *)&") return "f64";
        if (un.Op() == "*" && un.Operand().Kind() == CExpressionKind::kCastExpression &&
            static_cast<const CastExpression&>(un.Operand()).TargetType().name == "f64") {
          return "f64";
        }
        return infer_fp_arg_type(un.Operand());
      }
      if (e.Kind() == CExpressionKind::kBinaryExpression) {
        const auto& bin = static_cast<const BinaryExpression&>(e);
        if (infer_fp_arg_type(bin.Lhs()) == "f64" || infer_fp_arg_type(bin.Rhs()) == "f64") {
          return "f64";
        }
      }
      return "f32";
    };

    bool has_float_arg = false;
    std::vector<std::string> param_types;
    std::vector<std::unique_ptr<CExpression>> new_args;
    param_types.reserve(args.size());
    new_args.reserve(args.size());

    param_types.push_back(existing_param_types.empty()
                              ? (arg0_is_float ? infer_fp_arg_type(*args[0]) : "s32")
                              : existing_param_types[0]);
    if (arg0_is_float && existing_param_types.empty() && param_types[0] == "f32") {
      has_float_arg = true;
    }
    new_args.push_back(args[0]->Clone());

    for (size_t k = 1; k < args.size(); ++k) {
      if (k == 1 && arg0_is_float) {
        if (is_float_arg0(*args[1])) {
          std::string p1_type =
              existing_param_types.empty() ? infer_fp_arg_type(*args[1]) : existing_param_types[1];
          if (existing_param_types.empty() && p1_type == "f32") {
            has_float_arg = true;
          }
          param_types.push_back(std::move(p1_type));
        } else {
          param_types.push_back(existing_param_types.empty() ? "s32" : existing_param_types[1]);
        }
        new_args.push_back(args[1]->Clone());
        continue;
      }
      auto unwrapped = extract_s32_bitcast_of_f32(*args[k]);
      if (unwrapped != nullptr) {
        has_float_arg = true;
        param_types.push_back("f32");
        new_args.push_back(std::move(unwrapped));
      } else {
        param_types.push_back(existing_param_types.empty() ? "s32" : existing_param_types[k]);
        new_args.push_back(args[k]->Clone());
      }
    }
    if (!has_float_arg) return nullptr;

    std::string sig;
    for (size_t k = 0; k < param_types.size(); ++k) {
      if (k > 0) sig += ", ";
      sig += param_types[k];
    }
    std::string cast_callee = absl::StrFormat("((%s (*)(%s))%s)", ret_type, sig, callee_name);
    return CExpression::Call(CExpression::Identifier(std::move(cast_callee)), std::move(new_args));
  };

  std::function<std::unique_ptr<CExpression>(const CExpression&, std::string_view)> rewrite_expr =
      [&](const CExpression& expr,
          std::string_view default_ret_type) -> std::unique_ptr<CExpression> {
    switch (expr.Kind()) {
      case CExpressionKind::kCallExpression: {
        const auto& call = static_cast<const CallExpression&>(expr);
        auto rewritten = rewrite_call(call, default_ret_type);
        if (rewritten != nullptr) return rewritten;
        std::vector<std::unique_ptr<CExpression>> args;
        args.reserve(call.Arguments().size());
        for (const auto& arg : call.Arguments()) {
          args.push_back(rewrite_expr(*arg, "s32"));
        }
        return CExpression::Call(call.Callee().Clone(), std::move(args));
      }
      case CExpressionKind::kAssignmentExpression: {
        const auto& a = static_cast<const AssignmentExpression&>(expr);
        std::string_view rhs_ret_type = "s32";
        if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
          std::string_view lhs_name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
          if (lhs_name == "f0") {
            rhs_ret_type = "f32";
          } else if (lhs_name == "f0_d") {
            rhs_ret_type = "f64";
          }
        }
        return CExpression::Assignment(a.Op(), rewrite_expr(a.Lhs(), "s32"),
                                       rewrite_expr(a.Rhs(), rhs_ret_type));
      }
      case CExpressionKind::kBinaryExpression: {
        const auto& b = static_cast<const BinaryExpression&>(expr);
        return CExpression::Binary(b.Op(), rewrite_expr(b.Lhs(), "s32"),
                                   rewrite_expr(b.Rhs(), "s32"));
      }
      case CExpressionKind::kUnaryExpression: {
        const auto& u = static_cast<const UnaryExpression&>(expr);
        return CExpression::Unary(u.Op(), rewrite_expr(u.Operand(), "s32"), u.IsPrefix());
      }
      case CExpressionKind::kCastExpression: {
        const auto& c = static_cast<const CastExpression&>(expr);
        return CExpression::Cast(c.TargetType(), rewrite_expr(c.Operand(), "s32"));
      }
      case CExpressionKind::kArrayIndexExpression: {
        const auto& ai = static_cast<const ArrayIndexExpression&>(expr);
        return CExpression::ArrayIndex(rewrite_expr(ai.Array(), "s32"),
                                       rewrite_expr(ai.Index(), "s32"));
      }
      case CExpressionKind::kMemberAccessExpression: {
        const auto& ma = static_cast<const MemberAccessExpression&>(expr);
        return CExpression::MemberAccess(rewrite_expr(ma.Object(), "s32"), ma.MemberName(),
                                         ma.IsArrow());
      }
      case CExpressionKind::kTernaryExpression: {
        const auto& t = static_cast<const TernaryExpression&>(expr);
        return CExpression::Ternary(rewrite_expr(t.Condition(), "s32"),
                                    rewrite_expr(t.TrueExpression(), "s32"),
                                    rewrite_expr(t.FalseExpression(), "s32"));
      }
      default:
        return expr.Clone();
    }
  };

  for (auto& stmt : comp->MutableStatements()) {
    if (stmt->Kind() == CStatementKind::kExpressionStatement) {
      const auto& es = static_cast<const ExpressionStatement&>(*stmt);
      std::string_view top_ret =
          (es.Expression().Kind() == CExpressionKind::kCallExpression) ? "void" : "s32";
      stmt = CStatement::Expression(rewrite_expr(es.Expression(), top_ret));
    } else if (stmt->Kind() == CStatementKind::kReturnStatement) {
      const auto& rs = static_cast<const ReturnStatement&>(*stmt);
      if (rs.ReturnValue() != nullptr) {
        stmt = CStatement::Return(rewrite_expr(*rs.ReturnValue(), "s32"));
      }
    } else if (stmt->Kind() == CStatementKind::kIfStatement) {
      auto& ifs = static_cast<IfStatement&>(*stmt);
      if (ifs.MutableThenBranch()->Kind() == CStatementKind::kCompoundStatement) {
        RewriteFloatInGprAndStackCallArguments(
            static_cast<CompoundStatement*>(ifs.MutableThenBranch()));
      }
      if (ifs.MutableElseBranch() != nullptr) {
        if (ifs.MutableElseBranch()->Kind() == CStatementKind::kCompoundStatement) {
          RewriteFloatInGprAndStackCallArguments(
              static_cast<CompoundStatement*>(ifs.MutableElseBranch()));
        } else if (ifs.MutableElseBranch()->Kind() == CStatementKind::kIfStatement) {
          CompoundStatement wrapper;
          wrapper.AddStatement(ifs.MutableElseBranch()->Clone());
          RewriteFloatInGprAndStackCallArguments(&wrapper);
          if (wrapper.Statements().size() == 1) {
            stmt = CStatement::If(rewrite_expr(ifs.Condition(), "s32"), ifs.ThenBranch().Clone(),
                                  std::move(wrapper.MutableStatements()[0]));
            continue;
          }
        }
      }
      stmt = CStatement::If(rewrite_expr(ifs.Condition(), "s32"), ifs.ThenBranch().Clone(),
                            ifs.ElseBranch() != nullptr ? ifs.ElseBranch()->Clone() : nullptr);
    } else if (stmt->Kind() == CStatementKind::kSwitchStatement) {
      auto& sw = static_cast<SwitchStatement&>(*stmt);
      for (auto& sc : sw.MutableCases()) {
        if (sc.body != nullptr) RewriteFloatInGprAndStackCallArguments(sc.body.get());
      }
    } else if (stmt->Kind() == CStatementKind::kWhileStatement) {
      auto& ws = static_cast<WhileStatement&>(*stmt);
      if (ws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        RewriteFloatInGprAndStackCallArguments(static_cast<CompoundStatement*>(ws.MutableBody()));
      }
    } else if (stmt->Kind() == CStatementKind::kDoWhileStatement) {
      auto& dws = static_cast<DoWhileStatement&>(*stmt);
      if (dws.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        RewriteFloatInGprAndStackCallArguments(static_cast<CompoundStatement*>(dws.MutableBody()));
      }
    } else if (stmt->Kind() == CStatementKind::kForStatement) {
      auto& fs = static_cast<ForStatement&>(*stmt);
      if (fs.MutableBody()->Kind() == CStatementKind::kCompoundStatement) {
        RewriteFloatInGprAndStackCallArguments(static_cast<CompoundStatement*>(fs.MutableBody()));
      }
    } else if (stmt->Kind() == CStatementKind::kCompoundStatement) {
      RewriteFloatInGprAndStackCallArguments(static_cast<CompoundStatement*>(stmt.get()));
    }
  }
}

void DetermineReturnFlags(const ControlFlowGraph& cfg, const SymbolIndex* symbol_index,
                          const AstConverterOptions& options, bool* returns_v0, bool* returns_f0) {
  *returns_v0 = false;
  *returns_f0 = false;
  if (options.return_type.has_value()) {
    std::string rt = options.return_type->ToString();
    *returns_v0 = (rt != "void" && rt != "f32" && rt != "f64");
    *returns_f0 = (rt == "f32" || rt == "f64");
    return;
  }
  *returns_f0 = ExpressionBuilder::DetermineReturnsF0(cfg);
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
        ExpressionBuilder::DetermineReturnsV0(cfg, symbol_index, options.functions_returning_v0);
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

std::vector<CParameter> BuildFunctionParameters(const ControlFlowGraph& cfg,
                                                const AstConverterOptions& options) {
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
    parameter_count = ExpressionBuilder::DetermineParameterCount(all_instructions);
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
  auto float_stack_param_slots = AbiAnalyzer::DetectFloatStackParameterSlots(cfg);
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

}  // namespace

absl::StatusOr<FunctionDeclaration> AstConverter::Convert(const ControlFlowGraph& cfg,
                                                          const StructuredRegion& root_region,
                                                          const SymbolIndex* symbol_index,
                                                          const AstConverterOptions& options) {
  auto body = std::make_unique<CompoundStatement>();

  bool returns_v0 = false;
  bool returns_f0 = false;
  DetermineReturnFlags(cfg, symbol_index, options, &returns_v0, &returns_f0);

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
  std::vector<CParameter> parameters = BuildFunctionParameters(cfg, options);

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
  InlineCrossJumpedCallTails(body.get());
  EliminateDeadPreCallArgAssignments(body.get());
  SinkCommonBranchTailArgAssignments(body.get());
  SimplifyShiftAddMultiplication(body.get());
  FoldSingleUseExpressions(body.get());
  RewriteFloatInGprAndStackCallArguments(body.get());
  SplitReusedFloatGprParameters(body.get(), parameters);
  CoalescePrologueParameterCopies(body.get(), parameters);
  FoldSingleUseExpressions(body.get());
  RewriteFloatInGprAndStackCallArguments(body.get());
  CoalescePrologueParameterCopies(body.get(), parameters);
  FoldSingleUseExpressions(body.get());
  RewriteFloatInGprAndStackCallArguments(body.get());
  SimplifyDecoupledDelaySlotEarlyReturns(body.get());
  SimplifyShiftAddMultiplication(body.get());

  if (ctx.HasFramePointer() && return_type.ToString() == "void" && !body->Statements().empty()) {
    const auto& last_stmt = *body->Statements().back();
    if (last_stmt.Kind() == CStatementKind::kReturnStatement &&
        static_cast<const ReturnStatement&>(last_stmt).ReturnValue() == nullptr) {
      bool preceded_by_label = (body->Statements().size() >= 2 &&
                                body->Statements()[body->Statements().size() - 2]->Kind() ==
                                    CStatementKind::kLabelStatement);
      if (!preceded_by_label) {
        body->MutableStatements().pop_back();
      }
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

  uint32_t unreferenced_sp_gap_size = 0;
  if (ctx.HasCalls() && !ctx.HasFramePointer() && ctx.MinSavedRegOffset() != 0xFFFFFFFF &&
      ctx.MinSavedRegOffset() > ctx.OutgoingArgsSize() && ctx.SpBufferSize() == 0) {
    bool has_sp_local = false;
    for (const auto& v : local_vars_to_declare) {
      if (v == "sp" || absl::StartsWith(v, "var_sp_")) {
        has_sp_local = true;
        break;
      }
    }
    for (int32_t off : ctx.StackStoreOffsets()) {
      if (off >= static_cast<int32_t>(ctx.OutgoingArgsSize()) &&
          off < static_cast<int32_t>(ctx.MinSavedRegOffset())) {
        has_sp_local = true;
        break;
      }
    }
    if (!has_sp_local) {
      std::string check_str = body->ToString();
      bool any_local_addr_taken = false;
      for (const auto& v : local_vars_to_declare) {
        if (absl::StrContains(check_str, absl::StrCat("&", v))) {
          any_local_addr_taken = true;
          break;
        }
      }
      for (const auto& p : parameters) {
        if (absl::StrContains(check_str, absl::StrCat("&", p.name))) {
          any_local_addr_taken = true;
          break;
        }
      }
      if (!any_local_addr_taken) {
        unreferenced_sp_gap_size = ctx.MinSavedRegOffset() - ctx.OutgoingArgsSize();
        local_vars_to_declare.push_back("sp");
      }
    }
  }

  if (!local_vars_to_declare.empty()) {
    if (ctx.SpAddressTaken() && ctx.FrameSize() == 0) {
      return absl::InternalError(
          absl::StrFormat("Frame analysis failed for function '%s': sp address taken as buffer, "
                          "but no stack allocation detected.",
                          options.function_name));
    }
    uint32_t sp_buffer_size =
        (unreferenced_sp_gap_size > 0) ? unreferenced_sp_gap_size : ctx.SpBufferSize();

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
    uint32_t entry_defs = (1u << static_cast<size_t>(Register::kZero)) |
                          (1u << static_cast<size_t>(Register::kSp)) |
                          (1u << static_cast<size_t>(Register::kRa));
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

    auto is_callee_saved_stack_access = [&](const Instruction& inst) -> bool {
      if (inst.rs != Register::kSp || !inst.rt.has_value()) return false;
      if (inst.opcode != Opcode::kSw && inst.opcode != Opcode::kSd && inst.opcode != Opcode::kLw &&
          inst.opcode != Opcode::kLd) {
        return false;
      }
      if ((inst.opcode == Opcode::kLw || inst.opcode == Opcode::kLd) && ctx.FrameSize() > 0 &&
          inst.immediate >= static_cast<int32_t>(ctx.FrameSize())) {
        return false;
      }
      Register rt = *inst.rt;
      return rt == Register::kRa || rt == Register::kFp ||
             (rt >= Register::kS0 && rt <= Register::kS7);
    };

    auto transfer_block = [&](const BasicBlock& block, uint32_t in_defs) -> uint32_t {
      uint32_t cur = in_defs;
      for (const auto& inst : block.instructions) {
        if (is_callee_saved_stack_access(inst)) continue;
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
        if (is_callee_saved_stack_access(inst)) continue;
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

    std::string body_str = body->ToString();
    auto has_address_taken = [&](std::string_view target_var) -> bool {
      std::string pattern = absl::StrCat("&", target_var);
      return absl::StrContains(body_str, pattern);
    };

    absl::flat_hash_map<std::string, int> fp_base_counts;
    bool has_shared_fp_base = false;
    for (const auto& vn : local_vars_to_declare) {
      if (is_fp_reg(vn)) {
        std::string base = absl::EndsWith(vn, "_d") ? vn.substr(0, vn.size() - 2) : vn;
        if (++fp_base_counts[base] > 1) {
          has_shared_fp_base = true;
        }
      }
    }

    auto expr_uses_var = [](const CExpression& expr, std::string_view target_var) -> bool {
      auto impl = [&](auto& self, const CExpression& e) -> bool {
        switch (e.Kind()) {
          case CExpressionKind::kIdentifier:
            return static_cast<const IdentifierExpression&>(e).Name() == target_var;
          case CExpressionKind::kUnaryExpression:
            return self(self, static_cast<const UnaryExpression&>(e).Operand());
          case CExpressionKind::kBinaryExpression: {
            const auto& b = static_cast<const BinaryExpression&>(e);
            return self(self, b.Lhs()) || self(self, b.Rhs());
          }
          case CExpressionKind::kAssignmentExpression: {
            const auto& a = static_cast<const AssignmentExpression&>(e);
            return self(self, a.Lhs()) || self(self, a.Rhs());
          }
          case CExpressionKind::kCallExpression: {
            const auto& c = static_cast<const CallExpression&>(e);
            if (self(self, c.Callee())) return true;
            for (const auto& arg : c.Arguments()) {
              if (arg != nullptr && self(self, *arg)) return true;
            }
            return false;
          }
          case CExpressionKind::kCastExpression:
            return self(self, static_cast<const CastExpression&>(e).Operand());
          case CExpressionKind::kMemberAccessExpression:
            return self(self, static_cast<const MemberAccessExpression&>(e).Object());
          case CExpressionKind::kArrayIndexExpression: {
            const auto& a = static_cast<const ArrayIndexExpression&>(e);
            return self(self, a.Array()) || self(self, a.Index());
          }
          case CExpressionKind::kTernaryExpression: {
            const auto& t = static_cast<const TernaryExpression&>(e);
            return self(self, t.Condition()) || self(self, t.TrueExpression()) ||
                   self(self, t.FalseExpression());
          }
          default:
            return false;
        }
      };
      return impl(impl, expr);
    };

    auto is_inline_asm_intrinsic_call = [](const CallExpression& c) -> bool {
      if (c.Callee().Kind() != CExpressionKind::kIdentifier) return false;
      std::string_view name = static_cast<const IdentifierExpression&>(c.Callee()).Name();
      return name == "N64Sqrtf" || name == "N64Sqrt" || name == "sqrtf" || name == "sqrt" ||
             name == "fabsf" || name == "fabs" || name == "ceilf" || name == "floorf" ||
             name == "roundf" || name == "truncf" || name == "ceil" || name == "floor" ||
             name == "round" || name == "trunc";
    };

    auto expr_has_call = [&](const CExpression& expr) -> bool {
      auto impl = [&](auto& self, const CExpression& e) -> bool {
        switch (e.Kind()) {
          case CExpressionKind::kCallExpression: {
            const auto& c = static_cast<const CallExpression&>(e);
            if (!is_inline_asm_intrinsic_call(c)) return true;
            for (const auto& arg : c.Arguments()) {
              if (arg != nullptr && self(self, *arg)) return true;
            }
            return false;
          }
          case CExpressionKind::kUnaryExpression:
            return self(self, static_cast<const UnaryExpression&>(e).Operand());
          case CExpressionKind::kBinaryExpression: {
            const auto& b = static_cast<const BinaryExpression&>(e);
            return self(self, b.Lhs()) || self(self, b.Rhs());
          }
          case CExpressionKind::kAssignmentExpression: {
            const auto& a = static_cast<const AssignmentExpression&>(e);
            return self(self, a.Lhs()) || self(self, a.Rhs());
          }
          case CExpressionKind::kCastExpression:
            return self(self, static_cast<const CastExpression&>(e).Operand());
          case CExpressionKind::kMemberAccessExpression:
            return self(self, static_cast<const MemberAccessExpression&>(e).Object());
          case CExpressionKind::kArrayIndexExpression: {
            const auto& a = static_cast<const ArrayIndexExpression&>(e);
            return self(self, a.Array()) || self(self, a.Index());
          }
          case CExpressionKind::kTernaryExpression: {
            const auto& t = static_cast<const TernaryExpression&>(e);
            return self(self, t.Condition()) || self(self, t.TrueExpression()) ||
                   self(self, t.FalseExpression());
          }
          default:
            return false;
        }
      };
      return impl(impl, expr);
    };

    auto stmt_has_call = [&](const CStatement& root_stmt) -> bool {
      auto impl = [&](auto& self, const CStatement& s) -> bool {
        switch (s.Kind()) {
          case CStatementKind::kCompoundStatement:
            for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
              if (child != nullptr && self(self, *child)) return true;
            }
            return false;
          case CStatementKind::kExpressionStatement:
            return expr_has_call(static_cast<const ExpressionStatement&>(s).Expression());
          case CStatementKind::kVariableDeclarationStatement: {
            const auto& vd = static_cast<const VariableDeclarationStatement&>(s);
            return vd.Initializer() != nullptr && expr_has_call(*vd.Initializer());
          }
          case CStatementKind::kIfStatement: {
            const auto& ifs = static_cast<const IfStatement&>(s);
            return expr_has_call(ifs.Condition()) || self(self, ifs.ThenBranch()) ||
                   (ifs.ElseBranch() != nullptr && self(self, *ifs.ElseBranch()));
          }
          case CStatementKind::kWhileStatement: {
            const auto& ws = static_cast<const WhileStatement&>(s);
            return expr_has_call(ws.Condition()) || self(self, ws.Body());
          }
          case CStatementKind::kDoWhileStatement: {
            const auto& dws = static_cast<const DoWhileStatement&>(s);
            return self(self, dws.Body()) || expr_has_call(dws.Condition());
          }
          case CStatementKind::kForStatement: {
            const auto& fs = static_cast<const ForStatement&>(s);
            return (fs.Init() != nullptr && self(self, *fs.Init())) ||
                   (fs.Condition() != nullptr && expr_has_call(*fs.Condition())) ||
                   (fs.Step() != nullptr && expr_has_call(*fs.Step())) || self(self, fs.Body());
          }
          case CStatementKind::kReturnStatement: {
            const auto& rs = static_cast<const ReturnStatement&>(s);
            return rs.ReturnValue() != nullptr && expr_has_call(*rs.ReturnValue());
          }
          case CStatementKind::kSwitchStatement: {
            const auto& ss = static_cast<const SwitchStatement&>(s);
            if (expr_has_call(ss.Condition())) return true;
            for (const auto& c : ss.Cases()) {
              if (c.body != nullptr && self(self, *c.body)) return true;
            }
            return false;
          }
          default:
            return false;
        }
      };
      return impl(impl, root_stmt);
    };

    const bool is_leaf_func = !stmt_has_call(*body);

    auto unwrap_direct_call = [](const CExpression& expr) -> const CallExpression* {
      const CExpression* cur = &expr;
      while (cur->Kind() == CExpressionKind::kCastExpression) {
        cur = &static_cast<const CastExpression*>(cur)->Operand();
      }
      if (cur->Kind() == CExpressionKind::kCallExpression) {
        return static_cast<const CallExpression*>(cur);
      }
      return nullptr;
    };

    auto check_arg_reg_in_call = [&](std::string_view r, const CExpression& expr) -> bool {
      bool is_gpr_arg = (r == "arg0" || r == "arg1" || r == "arg2" || r == "arg3");
      bool is_fpr_arg = (r == "f12" || r == "f12_d" || r == "f14" || r == "f14_d");
      if (!is_gpr_arg && !is_fpr_arg) return true;
      if (!expr_uses_var(expr, r)) return true;
      const CallExpression* call = unwrap_direct_call(expr);
      if (call == nullptr) return false;
      if (expr_uses_var(call->Callee(), r)) return false;
      size_t slot =
          is_gpr_arg ? static_cast<size_t>(r[3] - '0') : ((r == "f12" || r == "f12_d") ? 0 : 1);
      if (slot >= call->Arguments().size() || call->Arguments()[slot] == nullptr) return false;
      if (call->Arguments()[slot]->Kind() != CExpressionKind::kIdentifier ||
          static_cast<const IdentifierExpression&>(*call->Arguments()[slot]).Name() != r) {
        return false;
      }
      for (size_t i = 0; i < call->Arguments().size(); ++i) {
        if (i != slot && call->Arguments()[i] != nullptr &&
            expr_uses_var(*call->Arguments()[i], r)) {
          return false;
        }
      }
      return true;
    };

    auto is_caller_saved_safe_across_calls = [&](std::string_view r) -> bool {
      if (is_leaf_func) return true;
      bool clobbered = false;
      auto check_expr = [&](const CExpression& e) -> bool {
        if (e.Kind() == CExpressionKind::kAssignmentExpression) {
          const auto& a = static_cast<const AssignmentExpression&>(e);
          if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
            std::string_view lhs_name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
            const CExpression& rhs = a.Rhs();
            bool rhs_has_call = expr_has_call(rhs);
            bool rhs_uses_r = expr_uses_var(rhs, r);
            if (rhs_uses_r && clobbered) return false;
            if (rhs_has_call && rhs_uses_r) {
              if (!check_arg_reg_in_call(r, rhs)) return false;
            }
            if (lhs_name == r) {
              if (!rhs_has_call) {
                clobbered = false;
                return true;
              }
              if (unwrap_direct_call(rhs) != nullptr) {
                clobbered = (r != "v0" && r != "f0" && r != "f0_d");
                return true;
              }
              return false;
            }
            if (rhs_has_call) {
              clobbered = true;
            }
            return true;
          }
        }
        bool has_c = expr_has_call(e);
        bool uses_r = expr_uses_var(e, r);
        if (uses_r && clobbered) return false;
        if (has_c && uses_r) {
          if (!check_arg_reg_in_call(r, e)) return false;
        }
        if (has_c) {
          clobbered = true;
        }
        return true;
      };

      auto check_stmt = [&](auto& self, const CStatement& s) -> bool {
        switch (s.Kind()) {
          case CStatementKind::kCompoundStatement:
            for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
              if (child != nullptr && !self(self, *child)) return false;
            }
            return true;
          case CStatementKind::kExpressionStatement:
            return check_expr(static_cast<const ExpressionStatement&>(s).Expression());
          case CStatementKind::kVariableDeclarationStatement: {
            const auto& vd = static_cast<const VariableDeclarationStatement&>(s);
            if (vd.Initializer() != nullptr) {
              return check_expr(*vd.Initializer());
            }
            return true;
          }
          case CStatementKind::kReturnStatement: {
            const auto& rs = static_cast<const ReturnStatement&>(s);
            if (rs.ReturnValue() != nullptr) {
              return check_expr(*rs.ReturnValue());
            }
            return true;
          }
          case CStatementKind::kIfStatement: {
            const auto& ifs = static_cast<const IfStatement&>(s);
            if (!check_expr(ifs.Condition())) return false;
            bool before = clobbered;
            bool then_clobbered = before;
            clobbered = before;
            if (!self(self, ifs.ThenBranch())) return false;
            then_clobbered = clobbered;
            bool else_clobbered = before;
            if (ifs.ElseBranch() != nullptr) {
              clobbered = before;
              if (!self(self, *ifs.ElseBranch())) return false;
              else_clobbered = clobbered;
            }
            clobbered = then_clobbered || else_clobbered;
            return true;
          }
          case CStatementKind::kWhileStatement: {
            const auto& ws = static_cast<const WhileStatement&>(s);
            for (int pass = 0; pass < 2; ++pass) {
              if (!check_expr(ws.Condition())) return false;
              if (!self(self, ws.Body())) return false;
            }
            return true;
          }
          case CStatementKind::kDoWhileStatement: {
            const auto& dws = static_cast<const DoWhileStatement&>(s);
            for (int pass = 0; pass < 2; ++pass) {
              if (!self(self, dws.Body())) return false;
              if (!check_expr(dws.Condition())) return false;
            }
            return true;
          }
          case CStatementKind::kForStatement: {
            const auto& fs = static_cast<const ForStatement&>(s);
            if (fs.Init() != nullptr && !self(self, *fs.Init())) return false;
            for (int pass = 0; pass < 2; ++pass) {
              if (fs.Condition() != nullptr && !check_expr(*fs.Condition())) return false;
              if (!self(self, fs.Body())) return false;
              if (fs.Step() != nullptr && !check_expr(*fs.Step())) return false;
            }
            return true;
          }
          case CStatementKind::kSwitchStatement: {
            const auto& ss = static_cast<const SwitchStatement&>(s);
            if (!check_expr(ss.Condition())) return false;
            bool before = clobbered;
            bool any_clobbered = before;
            for (const auto& c : ss.Cases()) {
              clobbered = before;
              if (c.body != nullptr && !self(self, *c.body)) return false;
              any_clobbered = any_clobbered || clobbered;
            }
            clobbered = any_clobbered;
            return true;
          }
          case CStatementKind::kGotoStatement:
          case CStatementKind::kLabelStatement:
            clobbered = true;
            return true;
          default:
            return true;
        }
      };

      return check_stmt(check_stmt, *body);
    };

    size_t gpr_locals_count = 0;
    for (const auto& v : local_vars_to_declare) {
      if (v != "sp" && !is_fp_reg(v) && !absl::EndsWith(v, "_64") && !double_vars.contains(v)) {
        ++gpr_locals_count;
      }
    }
    const bool allow_caller_saved_gpr_pins = (gpr_locals_count <= 14);
    bool any_param_modified = false;
    for (const auto& param : parameters) {
      if (absl::StrContains(body_str, absl::StrCat(param.name, " = "))) {
        any_param_modified = true;
        break;
      }
    }

    absl::flat_hash_set<std::string> float_gpr_var_names;
    for (Register r : AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg)) {
      float_gpr_var_names.insert(ExpressionBuilder::RegisterVarName(r));
    }
    absl::flat_hash_set<std::string> float_local_var_names;
    for (int32_t off : AbiAnalyzer::DetectFloatLocalStackOffsets(cfg)) {
      float_local_var_names.insert(ExpressionBuilder::StackVarName(off));
    }

    int callee_saved_fp_count = 0;
    for (const auto& [base_name, count] : fp_base_counts) {
      if (count == 1 && (base_name == "f20" || base_name == "f22" || base_name == "f24" ||
                         base_name == "f26" || base_name == "f28" || base_name == "f30")) {
        callee_saved_fp_count++;
      }
    }
    int pinned_caller_fp_count = 0;

    auto get_callee_saved_register_annotation =
        [&](std::string_view var_name) -> std::optional<std::string> {
      if (has_address_taken(var_name)) {
        return std::nullopt;
      }
      if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_s") && var_name[6] >= '0' &&
          var_name[6] <= '7') {
        int reg_num = 16 + (var_name[6] - '0');
        return absl::StrFormat("$%d", reg_num);
      }
      if (is_fp_reg(var_name)) {
        std::string_view base =
            absl::EndsWith(var_name, "_d") ? var_name.substr(0, var_name.size() - 2) : var_name;
        if (fp_base_counts[std::string(base)] == 1) {
          if (base == "f20" || base == "f22" || base == "f24" || base == "f26" || base == "f28" ||
              base == "f30") {
            return absl::StrCat("$", base);
          }
          if (callee_saved_fp_count + pinned_caller_fp_count < 12 &&
              (base == "f2" || base == "f4" || base == "f6" || base == "f8" || base == "f10" ||
               base == "f16" || base == "f18" ||
               ((base == "f0" || base == "f12" || base == "f14") &&
                return_type.ToString() == "void" &&
                ((is_leaf_func && has_shared_fp_base) ||
                 (!is_leaf_func && base != "f14" && fp_base_counts.size() <= 10))))) {
            if (is_caller_saved_safe_across_calls(var_name)) {
              pinned_caller_fp_count++;
              return absl::StrCat("$", base);
            }
          }
        }
        return std::nullopt;
      }
      if (!allow_caller_saved_gpr_pins) {
        return std::nullopt;
      }
      std::optional<std::string> caller_gpr_reg;
      bool is_float_return_leaf_with_v0_v1 =
          is_leaf_func && (return_type.ToString() == "f32" || return_type.ToString() == "f64") &&
          std::find(local_vars_to_declare.begin(), local_vars_to_declare.end(), "v0") !=
              local_vars_to_declare.end() &&
          std::find(local_vars_to_declare.begin(), local_vars_to_declare.end(), "v1") !=
              local_vars_to_declare.end();
      if (is_float_return_leaf_with_v0_v1 && var_name == "v0") {
        caller_gpr_reg = "$2";
      } else if ((is_float_return_leaf_with_v0_v1 || !is_leaf_func) && var_name == "v1") {
        caller_gpr_reg = "$3";
      } else if (!is_leaf_func && !any_param_modified && var_name.size() == 4 &&
                 absl::StartsWith(var_name, "arg") && var_name[3] >= '0' && var_name[3] <= '3') {
        int reg_num = 4 + (var_name[3] - '0');
        caller_gpr_reg = absl::StrFormat("$%d", reg_num);
      } else if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_t") &&
                 var_name[6] >= '0' && var_name[6] <= '9') {
        int t_idx = var_name[6] - '0';
        int reg_num = (t_idx <= 7) ? (8 + t_idx) : (24 + (t_idx - 8));
        caller_gpr_reg = absl::StrFormat("$%d", reg_num);
      }
      if (caller_gpr_reg.has_value() && is_caller_saved_safe_across_calls(var_name)) {
        return caller_gpr_reg;
      }
      return std::nullopt;
    };

    auto new_body = std::make_unique<CompoundStatement>();
    for (const auto& var_name : local_vars_to_declare) {
      if (var_name == "sp") {
        new_body->AddStatement(
            CStatement::VariableDeclaration(CType::U8(), "sp", nullptr, sp_buffer_size));
      } else if (is_fp_reg(var_name)) {
        std::optional<std::string> reg_annot = get_callee_saved_register_annotation(var_name);
        if (absl::EndsWith(var_name, "_d") || double_vars.contains(var_name)) {
          new_body->AddStatement(CStatement::VariableDeclaration(
              CType::F64(), var_name, nullptr, std::nullopt, std::move(reg_annot)));
        } else {
          new_body->AddStatement(CStatement::VariableDeclaration(
              CType::F32(), var_name, nullptr, std::nullopt, std::move(reg_annot)));
        }
      } else if (float_gpr_var_names.contains(var_name)) {
        std::optional<std::string> reg_annot = get_callee_saved_register_annotation(var_name);
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name, nullptr,
                                                               std::nullopt, std::move(reg_annot)));
      } else if (float_local_var_names.contains(var_name)) {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name));
      } else if (absl::EndsWith(var_name, "_64")) {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::Named("s64"), var_name));
      } else if (double_vars.contains(var_name)) {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name));
      } else {
        auto init_expr = vars_needing_init.contains(var_name) ? CExpression::Integer(0) : nullptr;
        std::optional<std::string> reg_annot =
            (init_expr == nullptr) ? get_callee_saved_register_annotation(var_name) : std::nullopt;
        new_body->AddStatement(CStatement::VariableDeclaration(
            CType::S32(), var_name, std::move(init_expr), std::nullopt, std::move(reg_annot)));
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
