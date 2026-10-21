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
#include "core/c_ast_utils.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/ast_abi_rewriter.h"
#include "lifter/ast_arithmetic_simplifier.h"
#include "lifter/branch_condition_builder.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/expression_builder.h"
#include "lifter/stack_frame_analyzer.h"
#include "lifter/variable_declaration_builder.h"

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
  const StackFrameAnalysis& FrameAnalysis() const { return frame_analysis_; }
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
                int init_zero_idx = -1;
                for (int si = static_cast<int>(target_block->Statements().size()) - 1; si >= 0;
                     --si) {
                  std::string s_str = target_block->Statements()[si]->ToString();
                  if (absl::StartsWith(s_str, loop_var + " = 0;") ||
                      absl::StrContains(s_str, loop_var + " = 0;")) {
                    init_zero_idx = si;
                    break;
                  }
                }
                bool has_init_zero =
                    (init_zero_idx >= 0 &&
                     static_cast<size_t>(init_zero_idx + 4) >= target_block->Statements().size());

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

          if (has_branch_likely || peeled_loop_delays_.contains(cond_block_id) ||
              delay_clobbers_cond || decouple_delay) {
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
            if ((has_branch_likely || peeled_loop_delays_.contains(cond_block_id)) &&
                cond_block != nullptr && cond_block->instructions.size() >= 2) {
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
    for (Register d : GetInstructionUseDef(inst).gpr_defs) {
      if (d >= Register::kS0 && d <= Register::kS7) {
        TrackedValue tv = state.tracker.GetRegisterValue(d);
        if (tv.kind == ValueKind::kSymbolAddress && !IsDirectMappedAddress(tv.constant_value)) {
          state.tracker.SetRegisterValue(d, TrackedValue());
        }
      }
    }
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
        if (cfg_.Blocks().size() > 1) {
          for (int r = 1; r < 32; ++r) {
            Register reg = static_cast<Register>(r);
            if (reg == Register::kAt || reg == Register::kSp) continue;
            TrackedValue tv = entry_state.tracker.GetRegisterValue(reg);
            if (tv.kind == ValueKind::kConstant &&
                static_cast<int32_t>(tv.constant_value) >= -32768 &&
                static_cast<int32_t>(tv.constant_value) <= 65535) {
              entry_state.tracker.SetRegisterValue(reg, TrackedValue());
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

  auto has_address_of_var = [&](const CExpression& expr, std::string_view var) -> bool {
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
        if (assign.Op() != "=" || extract_assigned_lhs_var(assign.Lhs()).empty()) {
          check(assign.Lhs());
        }
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
        if (assign.Op() != "=" || extract_assigned_lhs_var(assign.Lhs()).empty()) {
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
            return VarLiveness::kKilledBeforeRead;
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

  auto has_loop_in_stmt = [](const CStatement& root_s) -> bool {
    bool found = false;
    std::function<void(const CStatement&)> walk = [&](const CStatement& s) {
      if (found) return;
      if (s.Kind() == CStatementKind::kWhileStatement ||
          s.Kind() == CStatementKind::kDoWhileStatement ||
          s.Kind() == CStatementKind::kForStatement) {
        found = true;
        return;
      }
      if (s.Kind() == CStatementKind::kIfStatement) {
        const auto& ifs = static_cast<const IfStatement&>(s);
        walk(ifs.ThenBranch());
        if (ifs.ElseBranch() != nullptr) walk(*ifs.ElseBranch());
      } else if (s.Kind() == CStatementKind::kSwitchStatement) {
        for (const auto& sc : static_cast<const SwitchStatement&>(s).Cases()) {
          if (sc.body != nullptr) walk(*sc.body);
        }
      } else if (s.Kind() == CStatementKind::kCompoundStatement) {
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          walk(*child);
        }
      }
    };
    walk(root_s);
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
    if (expected_uses_in_next == 0 && root_body != nullptr && var != "v0" && var != "f0" &&
        var != "f0_d" && count_uses(*root_body, var) == 0 &&
        !has_address_of_var_in_stmt(*root_body, var)) {
      return true;
    }
    if (expected_uses_in_next > 0 && def_stmt != nullptr && root_body != nullptr &&
        !root_has_label && var != "v0" && var != "f0" && var != "f0_d" &&
        !has_loop_in_stmt(*stmts[next_idx]) &&
        count_uses(*root_body, var) == expected_uses_in_next &&
        !has_address_of_var_in_stmt(*root_body, var)) {
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
                                       int64_t* out_offset, int64_t* out_size,
                                       bool* out_is_addr_of = nullptr) -> bool {
    if (out_is_addr_of != nullptr) *out_is_addr_of = false;
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
    if (addr.Kind() == CExpressionKind::kUnaryExpression) {
      const auto& addr_un = static_cast<const UnaryExpression&>(addr);
      if (addr_un.Op() == "&" && addr_un.Operand().Kind() == CExpressionKind::kIdentifier) {
        *out_base = static_cast<const IdentifierExpression&>(addr_un.Operand()).Name();
        *out_offset = 0;
        *out_size = size;
        if (out_is_addr_of != nullptr) *out_is_addr_of = true;
        return true;
      }
    }
    if (addr.Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(addr);
      if ((bin.Op() == "+" || bin.Op() == "-") &&
          bin.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
        int64_t off = static_cast<const IntegerLiteralExpression&>(bin.Rhs()).Value();
        if (bin.Lhs().Kind() == CExpressionKind::kIdentifier) {
          *out_base = static_cast<const IdentifierExpression&>(bin.Lhs()).Name();
          *out_offset = (bin.Op() == "-") ? -off : off;
          *out_size = size;
          return true;
        }
        if (bin.Lhs().Kind() == CExpressionKind::kUnaryExpression) {
          const auto& lhs_un = static_cast<const UnaryExpression&>(bin.Lhs());
          if (lhs_un.Op() == "&" && lhs_un.Operand().Kind() == CExpressionKind::kIdentifier) {
            *out_base = static_cast<const IdentifierExpression&>(lhs_un.Operand()).Name();
            *out_offset = (bin.Op() == "-") ? -off : off;
            *out_size = size;
            if (out_is_addr_of != nullptr) *out_is_addr_of = true;
            return true;
          }
        }
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
    bool addr_of_a = false;
    bool addr_of_b = false;
    if (!extract_simple_deref_range(a, &base_a, &off_a, &size_a, &addr_of_a) ||
        !extract_simple_deref_range(b, &base_b, &off_b, &size_b, &addr_of_b)) {
      return false;
    }
    if (base_a == base_b) {
      return (off_a + size_a <= off_b) || (off_b + size_b <= off_a);
    }
    if (addr_of_a && addr_of_b) {
      return true;
    }
    if ((addr_of_a != addr_of_b) && size_a != size_b) {
      return true;
    }
    return false;
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
            all_disjoint = false;
            return;
          }
          walk(un.Operand());
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
            auto is_float_literal_expr = [](const CExpression& e) -> bool {
              if (e.Kind() == CExpressionKind::kFloatLiteral) return true;
              if (e.Kind() == CExpressionKind::kIdentifier) {
                std::string_view name = static_cast<const IdentifierExpression&>(e).Name();
                return !name.empty() &&
                       (std::isdigit(static_cast<unsigned char>(name[0])) ||
                        (name.size() > 1 && name[0] == '-' &&
                         std::isdigit(static_cast<unsigned char>(name[1])))) &&
                       absl::StrContains(name, '.');
              }
              return false;
            };
            if (var.size() >= 2 && var[0] == 'f' && std::isdigit(var[1]) && !is_float_bitcast_lhs &&
                is_pure_expr(assign.Rhs())) {
              // 1. Subword half of a 64-bit double constant load: `f2 = <lit>; f2_d = <lit>;`
              // or `f2_d = <lit>; f2_d = <lit>;`
              if (i + 1 < stmts.size() &&
                  stmts[i + 1]->Kind() == CStatementKind::kExpressionStatement &&
                  (is_float_literal_expr(assign.Rhs()) ||
                   assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral)) {
                const auto& next_es = static_cast<const ExpressionStatement&>(*stmts[i + 1]);
                if (next_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                  const auto& next_a =
                      static_cast<const AssignmentExpression&>(next_es.Expression());
                  if (next_a.Op() == "=" && next_a.Lhs().Kind() == CExpressionKind::kIdentifier &&
                      (is_float_literal_expr(next_a.Rhs()) ||
                       next_a.Rhs().Kind() == CExpressionKind::kIntegerLiteral)) {
                    std::string_view next_lhs =
                        static_cast<const IdentifierExpression&>(next_a.Lhs()).Name();
                    bool is_odd_half_of_next_double = false;
                    int var_reg = -1;
                    if (absl::SimpleAtoi(var.substr(1), &var_reg) && (var_reg % 2 == 1) &&
                        var_reg >= 1) {
                      if (next_lhs == absl::StrFormat("f%d_d", var_reg - 1)) {
                        is_odd_half_of_next_double = true;
                      }
                    }
                    if (next_lhs == var || next_lhs == absl::StrCat(var, "_d") ||
                        is_odd_half_of_next_double) {
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
              for (size_t k = i + 1; k < stmts.size() && k <= i + 40; ++k) {
                if (has_call_in_stmt(*stmts[k])) {
                  followed_by_call = true;
                  break;
                }
              }
              bool dead_at_end_arg =
                  is_call_arg && dead_at_end_vars != nullptr && dead_at_end_vars->contains(var);
              if ((followed_by_call || dead_at_end_arg) &&
                  (is_dead_after_stmt(i, var, 0, stmts[i].get()) ||
                   (dead_at_end_arg && root_body != nullptr && count_uses(*root_body, var) == 0 &&
                    !has_address_of_var_in_stmt(*root_body, var)))) {
                stmts.erase(stmts.begin() + i);
                changed = true;
                break;
              }
            }

            bool killed_in_same_block = false;
            if (!is_switch_case_body) {
              for (size_t k = i + 1; k < stmts.size(); ++k) {
                if (stmts[k]->Kind() == CStatementKind::kSwitchStatement) {
                  killed_in_same_block = true;
                  break;
                }
                if (stmts[k]->Kind() == CStatementKind::kExpressionStatement) {
                  const auto& kes = static_cast<const ExpressionStatement&>(*stmts[k]);
                  if (kes.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
                    const auto& ka = static_cast<const AssignmentExpression&>(kes.Expression());
                    if (ka.Op() == "=" && ka.Lhs().Kind() == CExpressionKind::kIdentifier &&
                        static_cast<const IdentifierExpression&>(ka.Lhs()).Name() == var) {
                      killed_in_same_block = true;
                      break;
                    }
                  }
                }
              }
            }
            if ((var == "temp_at" ||
                 (var == "v0" && !is_float_bitcast_lhs &&
                  ((dead_at_end_vars != nullptr && dead_at_end_vars->contains(var)) ||
                   comp == root_body || killed_in_same_block) &&
                  (assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral ||
                   is_comparison_expr(effective_rhs) ||
                   (effective_rhs.Kind() == CExpressionKind::kBinaryExpression &&
                    static_cast<const BinaryExpression&>(effective_rhs).Op() == "&")))) &&
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
              for (size_t k = i + 1; k < stmts.size() && k <= i + 48; ++k) {
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
                if (cand_assign.Op() != "=" || !is_pure_expr(cand_assign.Rhs())) break;
                std::string_view mid_lhs_sv = extract_assigned_lhs_var(cand_assign.Lhs());
                if (!mid_lhs_sv.empty()) {
                  std::string mid_lhs(mid_lhs_sv);
                  if (mid_lhs == var || count_expr_uses(assign.Rhs(), mid_lhs) > 0) break;
                } else if (!is_call_arg ||
                           (has_memory_deref(assign.Rhs()) &&
                            !are_all_derefs_provably_disjoint(assign.Rhs(), cand_assign.Lhs())) ||
                           !is_pure_expr(cand_assign.Lhs())) {
                  break;
                }
              }
              if (changed) break;

              if (count_expr_uses(effective_rhs, var) > 0) continue;

              if (is_stack_arg || is_call_arg) {
                if (!is_pure_expr(effective_rhs)) continue;
                std::vector<size_t> intermediate_use_stmts;
                // Look ahead across pure stack/register argument setup statements to the call
                for (size_t j = i + 1; j < stmts.size() && j <= i + 40; ++j) {
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
                        (uses_in_call == 1 && uses_in_stmt == 1 &&
                         (intermediate_use_stmts.empty() ||
                          (is_call_arg && is_duplicable_arg_rhs(effective_rhs)))) ||
                        (is_call_arg && uses_in_call > 1 && uses_in_stmt == uses_in_call &&
                         is_duplicable_arg_rhs(effective_rhs));
                    if (can_fold_arg_uses &&
                        (!has_address_of_var(*target_call, var) ||
                         is_c_var_identifier(effective_rhs)) &&
                        (!has_address_of(effective_rhs) ||
                         !used_in_addition_or_subtraction(*target_call, var)) &&
                        is_dead_after_stmt(j, var, uses_in_call, stmts[i].get())) {
                      bool all_intermediate_updated = true;
                      for (size_t mid_idx : intermediate_use_stmts) {
                        const auto& mid_es =
                            static_cast<const ExpressionStatement&>(*stmts[mid_idx]);
                        auto new_mid_expr =
                            SubstituteIdentifier(mid_es.Expression(), var, effective_rhs);
                        if (count_expr_uses(*new_mid_expr, var) != 0) {
                          all_intermediate_updated = false;
                          break;
                        }
                      }
                      if (all_intermediate_updated) {
                        for (size_t mid_idx : intermediate_use_stmts) {
                          const auto& mid_es =
                              static_cast<const ExpressionStatement&>(*stmts[mid_idx]);
                          stmts[mid_idx] = CStatement::Expression(
                              SubstituteIdentifier(mid_es.Expression(), var, effective_rhs));
                        }
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
                  if (mid_assign.Op() != "=" || !is_pure_expr(mid_assign.Rhs())) {
                    break;
                  }
                  int mid_uses = count_uses(*stmts[j], var);
                  if (mid_uses > 0) {
                    if (!is_call_arg || !is_duplicable_arg_rhs(effective_rhs) ||
                        has_address_of(effective_rhs) ||
                        has_address_of_var(cand_es.Expression(), var)) {
                      break;
                    }
                    intermediate_use_stmts.push_back(j);
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
                    bool is_mem_store_rhs =
                        ca.Op() == "=" && ca.Lhs().Kind() != CExpressionKind::kIdentifier &&
                        is_pure_expr(ca.Lhs()) && count_expr_uses(ca.Lhs(), var) == 0 &&
                        ca.Rhs().Kind() == CExpressionKind::kIdentifier &&
                        static_cast<const IdentifierExpression&>(ca.Rhs()).Name() == var;
                    bool is_saved_reg_copy_before_call =
                        var == "v0" && ca.Op() == "=" &&
                        ca.Lhs().Kind() == CExpressionKind::kIdentifier &&
                        absl::StartsWith(static_cast<const IdentifierExpression&>(ca.Lhs()).Name(),
                                         "temp_s") &&
                        ca.Rhs().Kind() == CExpressionKind::kIdentifier &&
                        static_cast<const IdentifierExpression&>(ca.Rhs()).Name() == var &&
                        j + 1 < stmts.size() && has_call_in_stmt(*stmts[j + 1]);
                    if (!is_mem_store_rhs && !is_saved_reg_copy_before_call) {
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
                    if (var == "f0_d" && i > 0 &&
                        (count_uses(*stmts[i - 1], var) > 0 ||
                         (i > 1 && count_uses(*stmts[i - 2], var) > 0))) {
                      std::string rhs_str = assign.Rhs().ToString();
                      if (absl::StartsWith(rhs_str, "D_") ||
                          absl::StartsWith(rhs_str, "*(f64*)&D_") ||
                          absl::StartsWith(rhs_str, "*(f64 *)&D_")) {
                        break;
                      }
                    }
                    if (has_address_of(assign.Rhs()) &&
                        used_in_addition_or_subtraction(cand_es.Expression(), var)) {
                      break;
                    }
                    if (!is_fp_scratch &&
                        assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                      auto expr_has_u32_cast = [](const CExpression& root_e) -> bool {
                        auto impl = [&](auto& self, const CExpression& e) -> bool {
                          if (e.Kind() == CExpressionKind::kIdentifier) {
                            return absl::StartsWith(
                                static_cast<const IdentifierExpression&>(e).Name(), "((u32)");
                          }
                          if (e.Kind() == CExpressionKind::kCastExpression) {
                            const auto& ce = static_cast<const CastExpression&>(e);
                            if (ce.TargetType().ToString() == "u32") return true;
                            return self(self, ce.Operand());
                          }
                          if (e.Kind() == CExpressionKind::kUnaryExpression) {
                            const auto& un = static_cast<const UnaryExpression&>(e);
                            if (un.Op() == "(u32)") return true;
                            return self(self, un.Operand());
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
                                count_expr_uses(ce.Operand(), var) > 0) {
                              return true;
                            }
                            return self(self, ce.Operand());
                          }
                          if (e.Kind() == CExpressionKind::kUnaryExpression) {
                            const auto& un = static_cast<const UnaryExpression&>(e);
                            if ((un.Op() == "(f32)" || un.Op() == "(f64)") &&
                                count_expr_uses(un.Operand(), var) > 0) {
                              return true;
                            }
                            return self(self, un.Operand());
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
                      if (is_fp_scratch && !absl::EndsWith(var, "_d") &&
                          assign.Rhs().Kind() == CExpressionKind::kUnaryExpression) {
                        const auto& rhs_un = static_cast<const UnaryExpression&>(assign.Rhs());
                        if (rhs_un.Op() == "*" &&
                            rhs_un.Operand().Kind() == CExpressionKind::kCastExpression) {
                          const auto& rhs_cast =
                              static_cast<const CastExpression&>(rhs_un.Operand());
                          if (rhs_cast.TargetType().name == "f32" &&
                              rhs_cast.TargetType().pointer_depth == 1) {
                            is_float_arg_bitcast_assign = true;
                          }
                        }
                      }
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
                      auto is_magic_high_mult = [](const CExpression& e) -> bool {
                        if (e.Kind() != CExpressionKind::kBinaryExpression) return false;
                        const auto& b = static_cast<const BinaryExpression&>(e);
                        if (b.Op() != ">>" || b.Rhs().Kind() != CExpressionKind::kIntegerLiteral ||
                            static_cast<const IntegerLiteralExpression&>(b.Rhs()).Value() != 32 ||
                            b.Lhs().Kind() != CExpressionKind::kBinaryExpression) {
                          return false;
                        }
                        const auto& mul = static_cast<const BinaryExpression&>(b.Lhs());
                        if (mul.Op() != "*") return false;
                        return mul.Lhs().Kind() == CExpressionKind::kCastExpression ||
                               mul.Lhs().Kind() == CExpressionKind::kUnaryExpression;
                      };
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
                      if (is_fp_scratch && var != "f0" && var != "f0_d" &&
                          ((assign.Rhs().Kind() == CExpressionKind::kCastExpression &&
                            static_cast<const CastExpression&>(assign.Rhs()).TargetType().name ==
                                "f64") ||
                           (assign.Rhs().Kind() == CExpressionKind::kUnaryExpression &&
                            static_cast<const UnaryExpression&>(assign.Rhs()).Op() == "(f64)")) &&
                          ((cand_assign.Rhs().Kind() == CExpressionKind::kCastExpression &&
                            static_cast<const CastExpression&>(cand_assign.Rhs())
                                    .TargetType()
                                    .name == "f32") ||
                           (cand_assign.Rhs().Kind() == CExpressionKind::kUnaryExpression &&
                            static_cast<const UnaryExpression&>(cand_assign.Rhs()).Op() ==
                                "(f32)"))) {
                        break;
                      }
                      if (j > i + 1 && is_fp_scratch && var != "f0" && var != "f0_d" &&
                          cand_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                          cand_assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
                        std::string_view ca_lhs =
                            static_cast<const IdentifierExpression&>(cand_assign.Lhs()).Name();
                        if (ca_lhs != var && count_expr_uses(cand_assign.Rhs(), ca_lhs) > 0) {
                          bool crossed_magic_mult = false;
                          for (size_t m = i + 1; m < j; ++m) {
                            if (stmts[m]->Kind() == CStatementKind::kExpressionStatement) {
                              const auto& mes = static_cast<const ExpressionStatement&>(*stmts[m]);
                              if (mes.Expression().Kind() ==
                                  CExpressionKind::kAssignmentExpression) {
                                const auto& ma =
                                    static_cast<const AssignmentExpression&>(mes.Expression());
                                if (is_magic_high_mult(ma.Rhs()) ||
                                    (ma.Rhs().Kind() == CExpressionKind::kBinaryExpression &&
                                     static_cast<const BinaryExpression&>(ma.Rhs()).Op() == ">>" &&
                                     static_cast<const BinaryExpression&>(ma.Rhs()).Lhs().Kind() ==
                                         CExpressionKind::kCastExpression &&
                                     is_magic_high_mult(
                                         static_cast<const CastExpression&>(
                                             static_cast<const BinaryExpression&>(ma.Rhs()).Lhs())
                                             .Operand()))) {
                                  crossed_magic_mult = true;
                                  break;
                                }
                              }
                            }
                          }
                          if (crossed_magic_mult) {
                            break;
                          }
                        }
                      }
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
                // If `argX = <pure_rhs>;` (from a branch delay slot) precedes an `if`
                // whose `then` or `else` branch calls a function using `argX` once (and the
                // other branch does not touch `argX`), and `argX` is dead after the `if`,
                // fold `argX` into that branch's call statement.
                if (j == i + 1 && is_call_arg && count_expr_uses(if_stmt.Condition(), var) == 0 &&
                    is_pure_expr(assign.Rhs()) && !has_memory_deref(assign.Rhs()) &&
                    !has_address_of(assign.Rhs()) &&
                    is_dead_after_stmt(j, var, 1, stmts[i].get())) {
                  auto try_fold_into_branch_comp = [&](const CompoundStatement& branch_comp)
                      -> std::unique_ptr<CompoundStatement> {
                    if (branch_comp.Statements().size() > 1 &&
                        assign.Rhs().Kind() == CExpressionKind::kIdentifier &&
                        absl::StartsWith(
                            static_cast<const IdentifierExpression&>(assign.Rhs()).Name(),
                            "temp_s")) {
                      return nullptr;
                    }
                    for (size_t k = 0; k < branch_comp.Statements().size() && k < 4; ++k) {
                      const CStatement& bs = *branch_comp.Statements()[k];
                      int k_uses = count_uses(bs, var);
                      if (k_uses == 1) {
                        if (bs.Kind() != CStatementKind::kExpressionStatement ||
                            !has_call_in_stmt(bs)) {
                          return nullptr;
                        }
                        if (k > 0 && (assign.Rhs().Kind() != CExpressionKind::kIdentifier &&
                                      assign.Rhs().Kind() != CExpressionKind::kIntegerLiteral)) {
                          // Only fold non-trivial expressions into a multi-statement branch
                          // if the branch has no other call after k.
                          for (size_t after_k = k + 1; after_k < branch_comp.Statements().size();
                               ++after_k) {
                            if (has_call_in_stmt(*branch_comp.Statements()[after_k])) {
                              return nullptr;
                            }
                          }
                        }
                        const auto& call_es = static_cast<const ExpressionStatement&>(bs);
                        auto new_call_expr =
                            SubstituteIdentifier(call_es.Expression(), var, assign.Rhs());
                        if (count_expr_uses(*new_call_expr, var) != 0) return nullptr;
                        auto new_comp = std::make_unique<CompoundStatement>();
                        for (size_t m = 0; m < branch_comp.Statements().size(); ++m) {
                          if (m == k) {
                            new_comp->AddStatement(
                                CStatement::Expression(std::move(new_call_expr)));
                          } else {
                            new_comp->AddStatement(branch_comp.Statements()[m]->Clone());
                          }
                        }
                        return new_comp;
                      }
                      if (k_uses > 0 || has_call_in_stmt(bs) ||
                          bs.Kind() != CStatementKind::kExpressionStatement) {
                        return nullptr;
                      }
                      const auto& mid_es = static_cast<const ExpressionStatement&>(bs);
                      if (mid_es.Expression().Kind() != CExpressionKind::kAssignmentExpression) {
                        return nullptr;
                      }
                      const auto& mid_a =
                          static_cast<const AssignmentExpression&>(mid_es.Expression());
                      if (mid_a.Op() != "=" || !is_pure_expr(mid_a.Rhs()) ||
                          !is_pure_expr(mid_a.Lhs())) {
                        return nullptr;
                      }
                      std::string_view mid_lhs_sv = extract_assigned_lhs_var(mid_a.Lhs());
                      if (!mid_lhs_sv.empty()) {
                        std::string mid_lhs(mid_lhs_sv);
                        if (mid_lhs == var || count_expr_uses(assign.Rhs(), mid_lhs) > 0) {
                          return nullptr;
                        }
                      }
                    }
                    return nullptr;
                  };

                  int then_uses = count_uses(if_stmt.ThenBranch(), var);
                  int else_uses = (if_stmt.ElseBranch() != nullptr)
                                      ? count_uses(*if_stmt.ElseBranch(), var)
                                      : 0;
                  if (then_uses == 1 && else_uses == 0 &&
                      if_stmt.ThenBranch().Kind() == CStatementKind::kCompoundStatement &&
                      has_call_in_stmt(if_stmt.ThenBranch()) &&
                      !has_address_of_var_in_stmt(if_stmt.ThenBranch(), var)) {
                    const auto& then_comp =
                        static_cast<const CompoundStatement&>(if_stmt.ThenBranch());
                    if (auto new_then = try_fold_into_branch_comp(then_comp)) {
                      std::unique_ptr<CStatement> new_else = (if_stmt.ElseBranch() != nullptr)
                                                                 ? if_stmt.ElseBranch()->Clone()
                                                                 : nullptr;
                      stmts[j] = CStatement::If(if_stmt.Condition().Clone(), std::move(new_then),
                                                std::move(new_else));
                      stmts.erase(stmts.begin() + i);
                      changed = true;
                      break;
                    }
                  } else if (then_uses == 0 && else_uses == 1 && if_stmt.ElseBranch() != nullptr &&
                             if_stmt.ElseBranch()->Kind() == CStatementKind::kCompoundStatement &&
                             has_call_in_stmt(*if_stmt.ElseBranch()) &&
                             !has_address_of_var_in_stmt(*if_stmt.ElseBranch(), var)) {
                    const auto& else_comp =
                        static_cast<const CompoundStatement&>(*if_stmt.ElseBranch());
                    if (auto new_else = try_fold_into_branch_comp(else_comp)) {
                      stmts[j] = CStatement::If(if_stmt.Condition().Clone(),
                                                if_stmt.ThenBranch().Clone(), std::move(new_else));
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
          // Remove redundant trailing comparison assignment (`v0 = (i < n);` or `v1 = (i < n);`)
          // at the end of a `do { ... } while (i < n);` body when the variable is dead on both
          // the loop back-edge and loop exit.
          if (!body_comp->Statements().empty() &&
              body_comp->Statements().back()->Kind() == CStatementKind::kExpressionStatement) {
            const auto& last_es =
                static_cast<const ExpressionStatement&>(*body_comp->Statements().back());
            if (last_es.Expression().Kind() == CExpressionKind::kAssignmentExpression) {
              const auto& last_assign =
                  static_cast<const AssignmentExpression&>(last_es.Expression());
              if (last_assign.Op() == "=" &&
                  last_assign.Lhs().Kind() == CExpressionKind::kIdentifier &&
                  is_pure_expr(last_assign.Rhs())) {
                const std::string& last_var =
                    static_cast<const IdentifierExpression&>(last_assign.Lhs()).Name();
                if ((last_var == "v0" || last_var == "v1") &&
                    count_expr_uses(dws.Condition(), last_var) == 0 &&
                    is_dead_after_stmt(i, last_var, 0)) {
                  active_ignored_def_stmt = body_comp->Statements().back().get();
                  VarLiveness prefix_live = analyze_var_liveness(*body_comp, last_var, false, 0);
                  active_ignored_def_stmt = nullptr;
                  if (prefix_live != VarLiveness::kReadBeforeWrite &&
                      (root_has_label_cached == 0 || !has_label_in_stmt(*body_comp))) {
                    body_comp->MutableStatements().pop_back();
                  }
                }
              }
            }
          }
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
  CollapseUnalignedBlockCopies(body.get());
  CoalescePrologueParameterCopies(body.get(), parameters, ctx.FrameSize());
  InlineCrossJumpedCallTails(body.get());
  SinkCommonBranchTailArgAssignments(body.get());
  SimplifyShiftAddMultiplication(body.get());
  FoldSingleUseExpressions(body.get());
  SimplifyShiftAddMultiplication(body.get());
  SimplifyReciprocalDivisionAndUnsignedFloatConversions(body.get());
  FoldSingleUseExpressions(body.get());
  TrimUnassignedTrailingCallArgRegisters(body.get(), parameters, options.function_parameter_counts);
  RewriteFloatInGprAndStackCallArguments(body.get());
  SplitReusedFloatGprParameters(body.get(), parameters);
  CoalescePrologueParameterCopies(body.get(), parameters, ctx.FrameSize());
  FoldSingleUseExpressions(body.get());
  RewriteFloatInGprAndStackCallArguments(body.get());
  CoalescePrologueParameterCopies(body.get(), parameters, ctx.FrameSize());
  FoldSingleUseExpressions(body.get());
  RewriteFloatInGprAndStackCallArguments(body.get());
  SimplifyDecoupledDelaySlotEarlyReturns(body.get());
  SimplifyShiftAddMultiplication(body.get());
  SimplifyReciprocalDivisionAndUnsignedFloatConversions(body.get());
  FoldSingleUseExpressions(body.get());
  TrimUnassignedTrailingCallArgRegisters(body.get(), parameters, options.function_parameter_counts);
  if (SplitScratchReusedParameterRegisters(body.get(), parameters, ctx.FrameSize())) {
    FoldSingleUseExpressions(body.get());
  }

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

  absl::Status decl_status =
      PrependSynthesizedLocalDeclarations(cfg, ctx.FrameAnalysis(), ctx.DoubleVars(), parameters,
                                          return_type, symbol_index, options, &body);
  if (!decl_status.ok()) {
    return decl_status;
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
