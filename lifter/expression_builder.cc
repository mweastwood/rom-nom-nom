#include "lifter/expression_builder.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/division_pattern.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/call_lifter.h"
#include "lifter/instruction_patterns.h"
#include "lifter/symbol_folder.h"

namespace rom_nom_nom {

std::unique_ptr<LiftedExpression> LiftedExpression::Integer(uint32_t val, bool hex) {
  auto e = std::make_unique<LiftedExpression>();
  e->kind = ExpressionKind::kIntegerLiteral;
  e->int_val = val;
  e->is_hex = hex;
  return e;
}

std::unique_ptr<LiftedExpression> LiftedExpression::Variable(std::string name) {
  auto e = std::make_unique<LiftedExpression>();
  e->kind = ExpressionKind::kVariable;
  e->name = std::move(name);
  return e;
}

std::unique_ptr<LiftedExpression> LiftedExpression::GlobalRef(std::string name) {
  auto e = std::make_unique<LiftedExpression>();
  e->kind = ExpressionKind::kGlobalRef;
  e->name = std::move(name);
  return e;
}

std::unique_ptr<LiftedExpression> LiftedExpression::Unary(
    std::string op, std::unique_ptr<LiftedExpression> operand) {
  auto e = std::make_unique<LiftedExpression>();
  e->kind = ExpressionKind::kUnaryOp;
  e->op = std::move(op);
  e->args.push_back(std::move(operand));
  return e;
}

std::unique_ptr<LiftedExpression> LiftedExpression::Binary(std::string op,
                                                           std::unique_ptr<LiftedExpression> lhs,
                                                           std::unique_ptr<LiftedExpression> rhs) {
  auto e = std::make_unique<LiftedExpression>();
  e->kind = ExpressionKind::kBinaryOp;
  e->op = std::move(op);
  e->args.push_back(std::move(lhs));
  e->args.push_back(std::move(rhs));
  return e;
}

std::unique_ptr<LiftedExpression> LiftedExpression::Load(std::string type,
                                                         std::unique_ptr<LiftedExpression> addr) {
  auto e = std::make_unique<LiftedExpression>();
  e->kind = ExpressionKind::kMemoryLoad;
  e->name = std::move(type);
  e->args.push_back(std::move(addr));
  return e;
}

std::unique_ptr<LiftedExpression> LiftedExpression::Call(
    std::string func_name, std::vector<std::unique_ptr<LiftedExpression>> arguments) {
  auto e = std::make_unique<LiftedExpression>();
  e->kind = ExpressionKind::kFunctionCall;
  e->name = std::move(func_name);
  e->args = std::move(arguments);
  return e;
}

std::string LiftedExpression::ToString() const {
  switch (kind) {
    case ExpressionKind::kIntegerLiteral:
      if (RequiresHex(int_val, is_hex)) {
        return absl::StrFormat("0x%X", int_val);
      }
      return std::to_string(int_val);
    case ExpressionKind::kVariable:
    case ExpressionKind::kGlobalRef:
      return name;
    case ExpressionKind::kUnaryOp: {
      std::string operand = args.empty() ? "" : args[0]->ToString();
      if (!args.empty() && args[0]->kind == ExpressionKind::kBinaryOp) {
        operand = "(" + operand + ")";
      }
      return op + operand;
    }
    case ExpressionKind::kBinaryOp:
      if (args.size() >= 2) {
        return args[0]->ToString() + " " + op + " " + args[1]->ToString();
      }
      return "";
    case ExpressionKind::kMemoryLoad:
      if (!args.empty()) {
        std::string addr = args[0]->ToString();
        if (args[0]->kind == ExpressionKind::kBinaryOp) {
          addr = "(" + addr + ")";
        }
        if (!name.empty()) {
          return "*(" + name + "*)" + addr;
        }
        return "*" + addr;
      }
      return "*ptr";
    case ExpressionKind::kFunctionCall: {
      std::string out = name + "(";
      for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out += ", ";
        out += args[i]->ToString();
      }
      out += ")";
      return out;
    }
  }
  return "";
}

std::unique_ptr<LiftedExpression> LiftedExpression::Clone() const {
  auto clone = std::make_unique<LiftedExpression>();
  clone->kind = kind;
  clone->int_val = int_val;
  clone->is_hex = is_hex;
  clone->name = name;
  clone->op = op;
  clone->args.reserve(args.size());
  for (const auto& arg : args) {
    clone->args.push_back(arg ? arg->Clone() : nullptr);
  }
  return clone;
}

std::string LiftedStatement::ToString() const {
  switch (kind) {
    case StatementKind::kAssignment:
      if (destination_variable.empty() || destination_variable == "0") {
        return (expression ? expression->ToString() : "") + ";\n";
      }
      return destination_variable + " = " + (expression ? expression->ToString() : "0") + ";\n";
    case StatementKind::kStore: {
      std::string dest = "dest";
      if (destination_address) {
        dest = destination_address->ToString();
        if (destination_address->kind == ExpressionKind::kBinaryOp) {
          dest = "(" + dest + ")";
        }
        if (!store_type.empty()) {
          dest = "*(" + store_type + "*)" + dest;
        } else {
          dest = "*" + dest;
        }
      }
      return dest + " = " + (expression ? expression->ToString() : "0") + ";\n";
    }
    case StatementKind::kCall:
      return (expression ? expression->ToString() : "call()") + ";\n";
    case StatementKind::kReturn:
      if (expression) {
        return "return " + expression->ToString() + ";\n";
      }
      return "return;\n";
    case StatementKind::kUnhandled:
      return "/* " + (expression ? expression->ToString() : "UNHANDLED_INSTRUCTION") + " */\n";
  }
  return "";
}

std::string ExpressionBuilder::RegisterVarName(Register reg) {
  return AbiAnalyzer::RegisterVarName(reg);
}

std::string ExpressionBuilder::FpRegisterVarName(FpRegister reg, bool is_double) {
  return AbiAnalyzer::FpRegisterVarName(reg, is_double);
}

std::string ExpressionBuilder::StackVarName(int32_t offset) {
  if (offset < 0) {
    return absl::StrFormat("var_sp_neg_%d", -offset);
  }
  return absl::StrFormat("var_sp_%d", offset);
}

namespace {

std::unique_ptr<LiftedExpression> LiftFpRegister(FpRegister reg, bool is_double = false) {
  return LiftedExpression::Variable(ExpressionBuilder::FpRegisterVarName(reg, is_double));
}

}  // namespace

std::unique_ptr<LiftedExpression> ExpressionBuilder::LiftRegisterOrConstant(
    Register reg, const RegisterTracker& tracker, bool has_frame_pointer) {
  if (reg == Register::kZero) {
    return LiftedExpression::Integer(0);
  }
  if (reg == Register::kSp || (has_frame_pointer && reg == Register::kFp)) {
    return LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp"));
  }
  if (tracker.IsConstant(reg)) {
    auto c = tracker.GetConstant(reg);
    if (c.has_value() && !IsDirectMappedAddress(*c)) {
      bool hex = (*c > 0xFFFF);
      return LiftedExpression::Integer(*c, hex);
    }
  }
  return LiftedExpression::Variable(RegisterVarName(reg));
}

void ExpressionBuilder::LiftFoldedAccess(const FoldedSymbolAccess& lo, const Instruction& inst,
                                         RegisterTracker& tracker,
                                         std::vector<LiftedStatement>& statements) {
  LiftedStatement statement;
  if (lo.type == FoldedPatternType::kAddressLoad) {
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = ExpressionBuilder::RegisterVarName(lo.dest_reg);
    statement.expression =
        lo.symbol_name.empty()
            ? LiftedExpression::Integer(lo.address, true)
            : LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo.symbol_name));
    statements.push_back(std::move(statement));
  } else if (lo.type == FoldedPatternType::kConstantLiteral) {
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = ExpressionBuilder::RegisterVarName(lo.dest_reg);
    statement.expression = LiftedExpression::Integer(lo.address, true);
    statements.push_back(std::move(statement));
  } else if (lo.type == FoldedPatternType::kGlobalLoad) {
    statement.kind = StatementKind::kAssignment;
    if (lo.dest_fp_reg.has_value()) {
      statement.destination_variable =
          ExpressionBuilder::FpRegisterVarName(*lo.dest_fp_reg, lo.access_type == "f64");
    } else {
      statement.destination_variable = ExpressionBuilder::RegisterVarName(lo.dest_reg);
    }
    if (lo.symbol_name.empty()) {
      statement.expression =
          LiftedExpression::Load(lo.access_type, LiftedExpression::Integer(lo.address, true));
    } else if (lo.access_type != "s32") {
      statement.expression = LiftedExpression::Load(
          lo.access_type,
          LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo.symbol_name)));
    } else {
      statement.expression = LiftedExpression::GlobalRef(lo.symbol_name);
    }
    statements.push_back(std::move(statement));
  } else if (lo.type == FoldedPatternType::kGlobalStore) {
    std::unique_ptr<LiftedExpression> src_expr;
    if (lo.source_float_register.has_value()) {
      src_expr = LiftFpRegister(*lo.source_float_register, lo.access_type == "f64");
    } else {
      src_expr = ExpressionBuilder::LiftRegisterOrConstant(lo.src_reg, tracker);
    }

    if (lo.access_type != "s32") {
      statement.kind = StatementKind::kStore;
      statement.store_type = lo.access_type;
      statement.destination_address =
          lo.symbol_name.empty()
              ? LiftedExpression::Integer(lo.address, true)
              : LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo.symbol_name));
      statement.expression = std::move(src_expr);
    } else {
      statement.kind = StatementKind::kAssignment;
      statement.destination_variable =
          lo.symbol_name.empty() ? absl::StrFormat("*(0x%X)", lo.address) : lo.symbol_name;
      statement.expression = std::move(src_expr);
    }
    statements.push_back(std::move(statement));
  } else if (lo.type == FoldedPatternType::kGlobalIndexedLoad) {
    if (lo.dest_fp_reg.has_value()) {
      statement.destination_variable =
          ExpressionBuilder::FpRegisterVarName(*lo.dest_fp_reg, lo.access_type == "f64");
    } else {
      statement.destination_variable = ExpressionBuilder::RegisterVarName(lo.dest_reg);
    }
    std::unique_ptr<LiftedExpression> base_addr_expr;
    if (lo.symbol_name.empty()) {
      base_addr_expr = LiftedExpression::Integer(lo.address, true);
    } else {
      base_addr_expr = LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo.symbol_name));
    }
    auto cast_u8_ptr = LiftedExpression::Unary("(u8*)", std::move(base_addr_expr));
    auto index_expr = ExpressionBuilder::LiftRegisterOrConstant(lo.index_reg, tracker);
    auto indexed_addr =
        LiftedExpression::Binary("+", std::move(cast_u8_ptr), std::move(index_expr));
    statement.kind = StatementKind::kAssignment;
    statement.expression = LiftedExpression::Load(lo.access_type, std::move(indexed_addr));
    statements.push_back(std::move(statement));
  } else if (lo.type == FoldedPatternType::kGlobalIndexedStore) {
    std::unique_ptr<LiftedExpression> src_expr;
    if (lo.source_float_register.has_value()) {
      src_expr = LiftFpRegister(*lo.source_float_register, lo.access_type == "f64");
    } else {
      src_expr = ExpressionBuilder::LiftRegisterOrConstant(lo.src_reg, tracker);
    }
    std::unique_ptr<LiftedExpression> base_addr_expr;
    if (lo.symbol_name.empty()) {
      base_addr_expr = LiftedExpression::Integer(lo.address, true);
    } else {
      base_addr_expr = LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo.symbol_name));
    }
    auto cast_u8_ptr = LiftedExpression::Unary("(u8*)", std::move(base_addr_expr));
    auto index_expr = ExpressionBuilder::LiftRegisterOrConstant(lo.index_reg, tracker);
    auto indexed_addr =
        LiftedExpression::Binary("+", std::move(cast_u8_ptr), std::move(index_expr));

    statement.kind = StatementKind::kStore;
    statement.store_type = lo.access_type;
    statement.destination_address = std::move(indexed_addr);
    statement.expression = std::move(src_expr);
    statements.push_back(std::move(statement));
  }
  tracker.Step(inst);
  if (lo.type == FoldedPatternType::kAddressLoad && !lo.symbol_name.empty() &&
      lo.dest_reg >= Register::kS0 && lo.dest_reg <= Register::kS7) {
    tracker.SetRegisterValue(lo.dest_reg, TrackedValue());
  }
}

void ExpressionBuilder::LiftCallInstruction(
    const Instruction& inst, size_t i, absl::Span<const Instruction> instructions_to_process,
    const SymbolIndex* symbol_index,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
    const absl::flat_hash_set<std::string>* functions_returning_v0,
    const absl::flat_hash_set<std::string>* functions_returning_f0, RegisterTracker& tracker,
    std::vector<LiftedStatement>& statements, bool sp_address_taken,
    const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info, int32_t sp_base_offset,
    const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets, const ControlFlowGraph* cfg,
    uint32_t current_block_id) {
  rom_nom_nom::LiftCallInstruction(inst, i, instructions_to_process, symbol_index,
                                   function_parameter_counts, function_fp_parameter_counts,
                                   functions_returning_v0, functions_returning_f0, tracker,
                                   statements, sp_address_taken, call_return_info, sp_base_offset,
                                   saved_reg_offsets, cfg, current_block_id);
}

std::vector<LiftedStatement> ExpressionBuilder::LiftBlock(const BasicBlock& block,
                                                          const FunctionLiftContext& ctx,
                                                          bool omit_last_instruction) {
  FunctionLiftContext block_ctx = ctx;
  block_ctx.current_block_id = block.id;
  return LiftInstructions(block.instructions, block_ctx, omit_last_instruction);
}

std::vector<LiftedStatement> ExpressionBuilder::LiftBlock(
    const BasicBlock& block, const SymbolIndex* symbol_index, const GameConfig* game_config,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
    bool omit_last_instruction, uint32_t frame_size,
    const absl::flat_hash_set<std::string>* functions_returning_v0,
    const absl::flat_hash_set<std::string>* functions_returning_f0, bool has_frame_pointer,
    bool has_calls, bool sp_address_taken,
    const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets,
    const absl::flat_hash_map<FpRegister, int32_t>* saved_fpr_offsets,
    const std::array<std::optional<uint32_t>, 32>* initial_fpr_const_bits,
    const RegisterTracker* initial_tracker,
    const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info,
    const absl::flat_hash_set<int32_t>* function_stack_stores) {
  FunctionLiftContext ctx;
  ctx.symbol_index = symbol_index;
  ctx.game_config = game_config;
  ctx.function_parameter_counts = function_parameter_counts;
  ctx.function_fp_parameter_counts = function_fp_parameter_counts;
  ctx.frame_size = frame_size;
  ctx.functions_returning_v0 = functions_returning_v0;
  ctx.functions_returning_f0 = functions_returning_f0;
  ctx.has_frame_pointer = has_frame_pointer;
  ctx.has_calls = has_calls;
  ctx.sp_address_taken = sp_address_taken;
  ctx.saved_reg_offsets = saved_reg_offsets;
  ctx.saved_fpr_offsets = saved_fpr_offsets;
  ctx.initial_fpr_const_bits = initial_fpr_const_bits;
  ctx.initial_tracker = initial_tracker;
  ctx.call_return_info = call_return_info;
  ctx.function_stack_stores = function_stack_stores;
  return LiftBlock(block, ctx, omit_last_instruction);
}

std::vector<LiftedStatement> ExpressionBuilder::LiftInstructions(
    absl::Span<const Instruction> instructions, const SymbolIndex* symbol_index,
    const GameConfig* game_config,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
    bool omit_last_instruction, uint32_t frame_size,
    const absl::flat_hash_set<std::string>* functions_returning_v0,
    const absl::flat_hash_set<std::string>* functions_returning_f0, bool has_frame_pointer,
    bool has_calls, bool sp_address_taken,
    const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets,
    const absl::flat_hash_map<FpRegister, int32_t>* saved_fpr_offsets,
    const std::array<std::optional<uint32_t>, 32>* initial_fpr_const_bits,
    const RegisterTracker* initial_tracker,
    const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info,
    const absl::flat_hash_set<int32_t>* function_stack_stores) {
  FunctionLiftContext ctx;
  ctx.symbol_index = symbol_index;
  ctx.game_config = game_config;
  ctx.function_parameter_counts = function_parameter_counts;
  ctx.function_fp_parameter_counts = function_fp_parameter_counts;
  ctx.frame_size = frame_size;
  ctx.functions_returning_v0 = functions_returning_v0;
  ctx.functions_returning_f0 = functions_returning_f0;
  ctx.has_frame_pointer = has_frame_pointer;
  ctx.has_calls = has_calls;
  ctx.sp_address_taken = sp_address_taken;
  ctx.saved_reg_offsets = saved_reg_offsets;
  ctx.saved_fpr_offsets = saved_fpr_offsets;
  ctx.initial_fpr_const_bits = initial_fpr_const_bits;
  ctx.initial_tracker = initial_tracker;
  ctx.call_return_info = call_return_info;
  ctx.function_stack_stores = function_stack_stores;
  return LiftInstructions(instructions, ctx, omit_last_instruction);
}

// NOLINTNEXTLINE(google-readability-function-size)
std::vector<LiftedStatement> ExpressionBuilder::LiftInstructions(
    absl::Span<const Instruction> instructions, const FunctionLiftContext& ctx,
    bool omit_last_instruction) {
  const SymbolIndex* symbol_index = ctx.symbol_index;
  const GameConfig* game_config = ctx.game_config;
  const absl::flat_hash_map<std::string, int>* function_parameter_counts =
      ctx.function_parameter_counts;
  const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts =
      ctx.function_fp_parameter_counts;
  uint32_t frame_size = ctx.frame_size;
  const absl::flat_hash_set<std::string>* functions_returning_v0 = ctx.functions_returning_v0;
  const absl::flat_hash_set<std::string>* functions_returning_f0 = ctx.functions_returning_f0;
  bool has_frame_pointer = ctx.has_frame_pointer;
  bool has_calls = ctx.has_calls;
  bool sp_address_taken = ctx.sp_address_taken;
  const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets = ctx.saved_reg_offsets;
  const absl::flat_hash_map<FpRegister, int32_t>* saved_fpr_offsets = ctx.saved_fpr_offsets;
  const std::array<std::optional<uint32_t>, 32>* initial_fpr_const_bits =
      ctx.initial_fpr_const_bits;
  const RegisterTracker* initial_tracker = ctx.initial_tracker;
  const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info = ctx.call_return_info;
  const absl::flat_hash_set<int32_t>* function_stack_stores = ctx.function_stack_stores;
  const absl::flat_hash_set<Register>* float_parameters_in_general_registers =
      ctx.float_parameters_in_general_registers;
  const absl::flat_hash_set<int>* float_stack_param_slots = ctx.float_stack_param_slots;
  const absl::flat_hash_set<int32_t>* float_local_stack_offsets = ctx.float_local_stack_offsets;

  std::vector<LiftedStatement> statements;
  if (instructions.empty()) {
    return statements;
  }

  if (!has_frame_pointer) {
    for (const auto& inst : instructions) {
      if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd) && inst.rd.has_value() &&
          *inst.rd == Register::kFp &&
          ((inst.rs == Register::kSp && inst.rt == Register::kZero) ||
           (inst.rs == Register::kZero && inst.rt == Register::kSp))) {
        has_frame_pointer = true;
        break;
      }
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) && inst.rt.has_value() &&
          *inst.rt == Register::kFp && inst.rs == Register::kSp && inst.immediate == 0) {
        has_frame_pointer = true;
        break;
      }
    }
  }

  if (frame_size == 0) {
    for (const auto& inst : instructions) {
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) && inst.rt.has_value() &&
          *inst.rt == Register::kSp && inst.rs == Register::kSp && inst.immediate < 0) {
        frame_size = std::max(frame_size, static_cast<uint32_t>(-inst.immediate));
      }
    }
  }

  std::vector<Instruction> reordered_storage;
  absl::Span<const Instruction> instructions_to_process = instructions;
  bool has_delayed_branch = false;
  for (const auto& instruction : instructions) {
    if (instruction.IsCall() || instruction.IsReturn()) {
      has_delayed_branch = true;
      break;
    }
  }
  if (has_delayed_branch) {
    reordered_storage.reserve(instructions.size());
    for (size_t instruction_index = 0; instruction_index < instructions.size();
         ++instruction_index) {
      if ((instructions[instruction_index].IsCall() ||
           instructions[instruction_index].IsReturn()) &&
          instruction_index + 1 < instructions.size()) {
        // In MIPS, delay slot instructions execute before the call or return takes effect.
        reordered_storage.push_back(instructions[instruction_index + 1]);
        reordered_storage.push_back(instructions[instruction_index]);
        ++instruction_index;
      } else {
        reordered_storage.push_back(instructions[instruction_index]);
      }
    }
    instructions_to_process = reordered_storage;
  }

  if (!has_calls) {
    for (const auto& instruction : instructions) {
      if (instruction.opcode == Opcode::kJal || instruction.opcode == Opcode::kJalr) {
        has_calls = true;
        break;
      }
    }
  }

  SymbolFolder folder;
  folder.Fold(instructions_to_process, symbol_index, game_config, initial_tracker);

  const int32_t caller_frame_start =
      (frame_size > 0) ? static_cast<int32_t>(frame_size) : 0x7FFFFFFF;
  auto is_caller_homed_scalar_addr = [&](int32_t imm) -> bool {
    if (frame_size == 0 || imm < caller_frame_start || imm >= caller_frame_start + 16 ||
        (imm % 4) != 0) {
      return false;
    }
    if (function_stack_stores != nullptr) {
      return function_stack_stores->contains(imm);
    }
    for (const auto& ins : instructions_to_process) {
      if (ins.IsStore() && ins.rs.has_value() &&
          (*ins.rs == Register::kSp || (has_frame_pointer && *ins.rs == Register::kFp)) &&
          ins.immediate == imm) {
        return true;
      }
    }
    return false;
  };
  if (!sp_address_taken) {
    for (const auto& inst : instructions_to_process) {
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
          (inst.rs == Register::kSp || (has_frame_pointer && inst.rs == Register::kFp)) &&
          inst.rt.has_value() && *inst.rt != Register::kSp &&
          (!has_frame_pointer || *inst.rt != Register::kFp) && inst.immediate >= 0 &&
          !is_caller_homed_scalar_addr(inst.immediate)) {
        sp_address_taken = true;
        break;
      }
      if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd ||
           inst.opcode == Opcode::kOr) &&
          (inst.rs == Register::kSp || inst.rt == Register::kSp ||
           (has_frame_pointer && (inst.rs == Register::kFp || inst.rt == Register::kFp))) &&
          inst.rd.has_value() && *inst.rd != Register::kSp &&
          (!has_frame_pointer || *inst.rd != Register::kFp)) {
        sp_address_taken = true;
        break;
      }
      if ((inst.opcode == Opcode::kLwl || inst.opcode == Opcode::kLwr ||
           inst.opcode == Opcode::kSwl || inst.opcode == Opcode::kSwr) &&
          (inst.rs == Register::kSp || (has_frame_pointer && inst.rs == Register::kFp))) {
        sp_address_taken = true;
        break;
      }
    }
  }

  int32_t sp_base_offset = ctx.outgoing_args_size > 0 ? static_cast<int32_t>(ctx.outgoing_args_size)
                                                      : ((has_calls && frame_size > 16) ? 16 : 0);
  auto make_sp_addr = [sp_base_offset](int32_t offset) -> std::unique_ptr<LiftedExpression> {
    int32_t eff_offset = offset - sp_base_offset;
    if (eff_offset == 0) {
      return LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp"));
    } else if (eff_offset > 0) {
      return LiftedExpression::Binary(
          "+", LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp")),
          LiftedExpression::Integer(eff_offset));
    } else {
      return LiftedExpression::Binary(
          "-", LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp")),
          LiftedExpression::Integer(-eff_offset));
    }
  };

  int32_t sp_buffer_size = ctx.sp_buffer_size;
  auto is_sp_offset = [sp_address_taken, sp_base_offset, sp_buffer_size](int32_t offset) -> bool {
    if (!sp_address_taken) return false;
    if (offset < sp_base_offset) return false;
    if (sp_buffer_size > 0 && (offset - sp_base_offset) >= sp_buffer_size) return false;
    return true;
  };

  RegisterTracker tracker;
  if (initial_tracker != nullptr) {
    tracker = *initial_tracker;
    tracker.PrepareForBlock();
  }
  auto lift_reg = [&](Register r) { return LiftRegisterOrConstant(r, tracker, has_frame_pointer); };
  auto make_reg_offset_expr = [&](Register r, int16_t imm) -> std::unique_ptr<LiftedExpression> {
    if (imm == 0) {
      return lift_reg(r);
    }
    if (imm < 0) {
      return LiftedExpression::Binary("-", lift_reg(r), LiftedExpression::Integer(-imm));
    }
    return LiftedExpression::Binary("+", lift_reg(r), LiftedExpression::Integer(imm));
  };
  bool pending_return = false;

  struct PendingMult {
    bool is_unsigned = false;
    Register rs = Register::kZero;
    Register rt = Register::kZero;
  };
  std::unique_ptr<PendingMult> pending_mult;

  struct PendingUnalignedAccess {
    Register rt = Register::kZero;
    Register rs = Register::kZero;
    int16_t offset = 0;
  };
  std::unique_ptr<PendingUnalignedAccess> pending_lwl;
  std::unique_ptr<PendingUnalignedAccess> pending_swl;

  auto flush_pending_lwl = [&]() {
    if (pending_lwl == nullptr) return;
    LiftedStatement statement;
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = RegisterVarName(pending_lwl->rt);
    if (pending_lwl->rs == Register::kSp) {
      if (is_sp_offset(pending_lwl->offset)) {
        statement.expression = LiftedExpression::Load("s32", make_sp_addr(pending_lwl->offset));
      } else {
        statement.expression = LiftedExpression::Variable(StackVarName(pending_lwl->offset));
      }
    } else {
      statement.expression =
          LiftedExpression::Load("s32", make_reg_offset_expr(pending_lwl->rs, pending_lwl->offset));
    }
    statements.push_back(std::move(statement));
    pending_lwl = nullptr;
  };

  absl::flat_hash_set<int32_t> local_stack_stores;
  if (function_stack_stores != nullptr) {
    local_stack_stores = *function_stack_stores;
  } else {
    for (const auto& inst : instructions_to_process) {
      if (inst.IsStore() && inst.rs.has_value() &&
          (*inst.rs == Register::kSp || (has_frame_pointer && *inst.rs == Register::kFp))) {
        local_stack_stores.insert(inst.immediate);
      }
    }
  }

  auto folded_comp = FindFoldedComparison(instructions_to_process);

  size_t loop_end = instructions_to_process.size();
  if (omit_last_instruction && loop_end > 0) {
    --loop_end;
  }

  std::optional<size_t> sp_alloc_idx;
  for (size_t k = 0; k < loop_end; ++k) {
    const auto& k_inst = instructions_to_process[k];
    if ((k_inst.opcode == Opcode::kAddiu || k_inst.opcode == Opcode::kAddi) &&
        k_inst.rt == Register::kSp && k_inst.rs == Register::kSp && k_inst.immediate < 0) {
      sp_alloc_idx = k;
      break;
    }
  }

  absl::flat_hash_set<size_t> skipped_instruction_indices;
  absl::flat_hash_set<Register> gprs_defined_in_block;
  auto record_gpr_defs = [&](const Instruction& ins) {
    RegisterUseDef ud = GetInstructionUseDef(ins);
    for (Register d : ud.gpr_defs) {
      gprs_defined_in_block.insert(d);
    }
  };
  auto is_float_carrier_gpr = [&](Register r) -> bool {
    return float_parameters_in_general_registers != nullptr &&
           float_parameters_in_general_registers->contains(r) && r != Register::kA0 &&
           r != Register::kA1 && r != Register::kA2 && r != Register::kA3;
  };
  auto is_active_float_gpr = [&](Register r) -> bool {
    if (float_parameters_in_general_registers == nullptr ||
        !float_parameters_in_general_registers->contains(r)) {
      return false;
    }
    if (r == Register::kA0 || r == Register::kA1 || r == Register::kA2 || r == Register::kA3) {
      return !gprs_defined_in_block.contains(r);
    }
    return true;
  };
  std::optional<uint32_t> fpr_const_bits[32];
  std::unique_ptr<LiftedExpression> pending_int_to_fp[32];
  std::unique_ptr<LiftedExpression> pending_fp_to_int[32];
  if (initial_fpr_const_bits != nullptr) {
    for (int k = 0; k < 32; ++k) {
      fpr_const_bits[k] = (*initial_fpr_const_bits)[k];
    }
  }

  for (size_t i = 0; i < loop_end; ++i) {
    const auto& inst = instructions_to_process[i];

    if (pending_lwl != nullptr && inst.opcode != Opcode::kLwr) {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      bool touches_rt = false;
      for (Register r : ud.gpr_uses) {
        if (r == pending_lwl->rt) touches_rt = true;
      }
      for (Register r : ud.gpr_defs) {
        if (r == pending_lwl->rt) touches_rt = true;
      }
      if (touches_rt || inst.opcode == Opcode::kLwl || inst.IsBranch() ||
          inst.opcode == Opcode::kJ || inst.opcode == Opcode::kJr || inst.opcode == Opcode::kJal ||
          inst.opcode == Opcode::kJalr) {
        flush_pending_lwl();
      }
    }

    if (inst.opcode != Opcode::kMtc1) {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (FpRegister d : ud.fpr_defs) {
        int idx = static_cast<int>(d);
        fpr_const_bits[idx] = std::nullopt;
        if ((idx % 2) == 0 && idx + 1 < 32) {
          fpr_const_bits[idx + 1] = std::nullopt;
        }
      }
      if (inst.IsCall()) {
        for (int k = 0; k < 20; ++k) {
          fpr_const_bits[k] = std::nullopt;
          pending_int_to_fp[k] = nullptr;
          pending_fp_to_int[k] = nullptr;
        }
      }
    }

    // Skip emitting assignment for folded comparison instructions that are directly
    // integrated into subsequent branch conditions.
    if (folded_comp.has_value() && folded_comp->can_suppress_statement &&
        i == folded_comp->instruction_index) {
      record_gpr_defs(inst);
      tracker.Step(inst);
      continue;
    }

    if (skipped_instruction_indices.contains(i)) {
      record_gpr_defs(inst);
      tracker.Step(inst);
      continue;
    }

    // Check for MIPS GCC integer division/modulo trap expansion
    auto div_pattern = MatchDivisionPattern(instructions_to_process, i);
    if (div_pattern.has_value()) {
      if (div_pattern->div_dest_reg.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(*div_pattern->div_dest_reg);
        auto num = LiftRegisterOrConstant(div_pattern->num_reg, tracker);
        auto den = LiftRegisterOrConstant(div_pattern->den_reg, tracker);
        if (div_pattern->is_unsigned) {
          num = LiftedExpression::Unary("(u32)", std::move(num));
          den = LiftedExpression::Unary("(u32)", std::move(den));
        }
        statement.expression = LiftedExpression::Binary("/", std::move(num), std::move(den));
        statements.push_back(std::move(statement));
      }
      if (div_pattern->mod_dest_reg.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(*div_pattern->mod_dest_reg);
        auto num = LiftRegisterOrConstant(div_pattern->num_reg, tracker);
        auto den = LiftRegisterOrConstant(div_pattern->den_reg, tracker);
        if (div_pattern->is_unsigned) {
          num = LiftedExpression::Unary("(u32)", std::move(num));
          den = LiftedExpression::Unary("(u32)", std::move(den));
        }
        statement.expression = LiftedExpression::Binary("%", std::move(num), std::move(den));
        statements.push_back(std::move(statement));
      }
      for (size_t k = i; k <= div_pattern->end_index; ++k) {
        record_gpr_defs(instructions_to_process[k]);
        tracker.Step(instructions_to_process[k]);
      }
      i = div_pattern->end_index;
      continue;
    }

    // Check for MIPS GCC floating-point / integer conversion patterns
    auto conversion = MatchFloatingPointConversionPattern(instructions_to_process, i);
    if (conversion.has_value()) {
      if (conversion->is_rounding) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            FpRegisterVarName(conversion->dest_floating_point_reg, conversion->is_double);
        std::vector<std::unique_ptr<LiftedExpression>> args;
        args.push_back(LiftFpRegister(conversion->src_floating_point_reg, conversion->is_double));
        statement.expression = LiftedExpression::Call(conversion->round_func, std::move(args));
        statements.push_back(std::move(statement));
      } else if (conversion->is_int_to_float) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            FpRegisterVarName(conversion->dest_floating_point_reg, conversion->is_double);
        if (conversion->src_gpr == Register::kZero) {
          statement.expression = LiftedExpression::Variable(conversion->is_double ? "0.0" : "0.0f");
        } else {
          statement.expression =
              LiftedExpression::Unary(conversion->is_double ? "(f64)" : "(f32)",
                                      LiftRegisterOrConstant(conversion->src_gpr, tracker));
        }
        statements.push_back(std::move(statement));
      } else {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(conversion->dest_gpr);
        auto src_expr = LiftFpRegister(conversion->src_floating_point_reg, conversion->is_double);
        if (!conversion->round_func.empty()) {
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(std::move(src_expr));
          src_expr = LiftedExpression::Call(conversion->round_func, std::move(args));
        }
        statement.expression = LiftedExpression::Unary("(s32)", std::move(src_expr));
        statements.push_back(std::move(statement));
      }
      for (size_t k = i; k <= conversion->end_index; ++k) {
        record_gpr_defs(instructions_to_process[k]);
        tracker.Step(instructions_to_process[k]);
      }
      i = conversion->end_index;
      continue;
    }

    // If this instruction is the high or mid half of a folded pair, skip emitting it
    if (folder.IsFoldedHi(i) || folder.IsFoldedMid(i)) {
      record_gpr_defs(inst);
      tracker.Step(inst);
      continue;
    }

    // If this instruction is the low half of a folded pair, emit high-level folded access
    if (const auto* lo = folder.GetFoldedLo(i)) {
      if (inst.opcode == Opcode::kLwc1 && lo->dest_fp_reg.has_value() &&
          (lo->type == FoldedPatternType::kGlobalLoad ||
           lo->type == FoldedPatternType::kGlobalIndexedLoad)) {
        int ft_idx = static_cast<int>(*lo->dest_fp_reg);
        bool only_used_by_int_to_fp_cvt = false;
        for (size_t look = i + 1; look < loop_end; ++look) {
          const Instruction& future_inst = instructions_to_process[look];
          if ((future_inst.opcode == Opcode::kCvtSW || future_inst.opcode == Opcode::kCvtDW) &&
              future_inst.fs == *lo->dest_fp_reg) {
            bool reused_after_cvt = false;
            if (future_inst.fd != *lo->dest_fp_reg) {
              for (size_t k = look + 1; k < loop_end; ++k) {
                RegisterUseDef kud = GetInstructionUseDef(instructions_to_process[k]);
                for (FpRegister u : kud.fpr_uses) {
                  if (u == *lo->dest_fp_reg || (static_cast<int>(u) & ~1) == (ft_idx & ~1)) {
                    reused_after_cvt = true;
                    break;
                  }
                }
                if (reused_after_cvt) break;
                bool redefined = false;
                for (FpRegister d : kud.fpr_defs) {
                  if (d == *lo->dest_fp_reg || (static_cast<int>(d) & ~1) == (ft_idx & ~1)) {
                    redefined = true;
                    break;
                  }
                }
                if (redefined || instructions_to_process[k].IsCall()) break;
              }
            }
            only_used_by_int_to_fp_cvt = !reused_after_cvt;
            break;
          }
          RegisterUseDef fud = GetInstructionUseDef(future_inst);
          bool touches_ft_or_rs = false;
          for (FpRegister u : fud.fpr_uses) {
            if (u == *lo->dest_fp_reg || (static_cast<int>(u) & ~1) == (ft_idx & ~1)) {
              touches_ft_or_rs = true;
            }
          }
          for (FpRegister d : fud.fpr_defs) {
            if (d == *lo->dest_fp_reg || (static_cast<int>(d) & ~1) == (ft_idx & ~1)) {
              touches_ft_or_rs = true;
            }
          }
          if (lo->type == FoldedPatternType::kGlobalIndexedLoad) {
            for (Register d : fud.gpr_defs) {
              if (d == lo->index_reg) {
                touches_ft_or_rs = true;
              }
            }
          }
          if (touches_ft_or_rs || future_inst.IsCall() || future_inst.IsStore()) {
            break;
          }
        }
        if (only_used_by_int_to_fp_cvt) {
          if (lo->type == FoldedPatternType::kGlobalLoad) {
            if (lo->symbol_name.empty()) {
              pending_int_to_fp[ft_idx] =
                  LiftedExpression::Load("s32", LiftedExpression::Integer(lo->address, true));
            } else {
              pending_int_to_fp[ft_idx] = LiftedExpression::GlobalRef(lo->symbol_name);
            }
          } else {
            std::unique_ptr<LiftedExpression> base_addr_expr =
                lo->symbol_name.empty()
                    ? LiftedExpression::Integer(lo->address, true)
                    : LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo->symbol_name));
            auto indexed_addr = LiftedExpression::Binary(
                "+", LiftedExpression::Unary("(u8*)", std::move(base_addr_expr)),
                LiftRegisterOrConstant(lo->index_reg, tracker));
            pending_int_to_fp[ft_idx] = LiftedExpression::Load("s32", std::move(indexed_addr));
          }
          fpr_const_bits[ft_idx] = std::nullopt;
          record_gpr_defs(inst);
          tracker.Step(inst);
          continue;
        }
      }
      record_gpr_defs(inst);
      LiftFoldedAccess(*lo, inst, tracker, statements);
      continue;
    }

    // Skip NOPs
    if (inst.IsNop()) {
      tracker.Step(inst);
      continue;
    }

    if (inst.opcode != Opcode::kSwr && pending_swl != nullptr) {
      LiftedStatement statement;
      statement.kind = StatementKind::kStore;
      statement.store_type = "u8";
      Register base_reg = pending_swl->rs;
      if (pending_swl->offset == 0) {
        statement.destination_address = lift_reg(base_reg);
      } else {
        statement.destination_address = LiftedExpression::Binary(
            "+", lift_reg(base_reg), LiftedExpression::Integer(pending_swl->offset));
      }
      statement.expression = lift_reg(pending_swl->rt);
      statements.push_back(std::move(statement));
      pending_swl = nullptr;
    }

    switch (inst.opcode) {
      case Opcode::kAddu:
      case Opcode::kAdd:
      case Opcode::kDaddu:
      case Opcode::kDadd:
        if (has_frame_pointer && inst.rd == Register::kFp &&
            ((inst.rs == Register::kSp && inst.rt == Register::kZero) ||
             (inst.rs == Register::kZero && inst.rt == Register::kSp))) {
          break;
        }
        if (has_frame_pointer && inst.rd == Register::kSp &&
            ((inst.rs == Register::kFp && inst.rt == Register::kZero) ||
             (inst.rs == Register::kZero && inst.rt == Register::kFp))) {
          break;
        }
        if (inst.rd.has_value() && *inst.rd != Register::kZero && *inst.rd != Register::kSp &&
            (!has_frame_pointer || *inst.rd != Register::kFp)) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (inst.rs == Register::kZero && inst.rt.has_value()) {
            statement.expression = lift_reg(*inst.rt);
          } else if (inst.rt == Register::kZero && inst.rs.has_value()) {
            statement.expression = lift_reg(*inst.rs);
          } else if (inst.rs.has_value() && inst.rt.has_value()) {
            auto lift_non_zero_add_op = [&](Register r) {
              if (r != Register::kZero && tracker.IsConstant(r) && tracker.GetConstant(r) == 0) {
                return LiftedExpression::Variable(RegisterVarName(r));
              }
              return lift_reg(r);
            };
            statement.expression = LiftedExpression::Binary("+", lift_non_zero_add_op(*inst.rs),
                                                            lift_non_zero_add_op(*inst.rt));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSubu:
      case Opcode::kSub:
      case Opcode::kDsubu:
      case Opcode::kDsub:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && *inst.rd != Register::kSp &&
            (!has_frame_pointer || *inst.rd != Register::kFp)) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (inst.rs == Register::kZero && inst.rt.has_value()) {
            statement.expression = LiftedExpression::Unary("-", lift_reg(*inst.rt));
          } else if (inst.rs.has_value() && inst.rt.has_value()) {
            statement.expression =
                LiftedExpression::Binary("-", lift_reg(*inst.rs), lift_reg(*inst.rt));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAddiu:
      case Opcode::kAddi:
      case Opcode::kDaddiu:
      case Opcode::kDaddi:
        if (has_frame_pointer && inst.rt == Register::kFp && inst.rs == Register::kSp &&
            inst.immediate == 0) {
          break;
        }
        if (inst.rt.has_value() && *inst.rt != Register::kZero) {
          if (*inst.rt == Register::kSp || (has_frame_pointer && *inst.rt == Register::kFp)) {
            // Stack adjustment; handled in stack frame analysis
            break;
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (inst.rs == Register::kZero) {
            statement.expression = LiftedExpression::Integer(inst.immediate);
          } else if ((sp_address_taken || is_caller_homed_scalar_addr(inst.immediate)) &&
                     (inst.rs == Register::kSp ||
                      (has_frame_pointer && inst.rs == Register::kFp)) &&
                     inst.immediate >= 0) {
            if (is_sp_offset(inst.immediate)) {
              statement.expression = make_sp_addr(inst.immediate);
            } else {
              statement.expression = LiftedExpression::Unary(
                  "(s32)&", LiftedExpression::Variable(StackVarName(inst.immediate)));
            }
          } else if (inst.immediate == 0 && inst.rs.has_value()) {
            statement.expression = lift_reg(*inst.rs);
          } else if (inst.immediate < 0 && inst.rs.has_value()) {
            statement.expression = LiftedExpression::Binary(
                "-", lift_reg(*inst.rs), LiftedExpression::Integer(-inst.immediate));
          } else if (inst.rs.has_value()) {
            statement.expression = LiftedExpression::Binary(
                "+", lift_reg(*inst.rs), LiftedExpression::Integer(inst.immediate));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kLui:
        if (inst.rt.has_value() && *inst.rt != Register::kZero) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(inst.immediate)) << 16;
          if (is_float_carrier_gpr(*inst.rt) && (val & 0x7F800000u) != 0 &&
              (val & 0x7F800000u) != 0x7F800000u) {
            float f_val = 0.0f;
            std::memcpy(&f_val, &val, sizeof(f_val));
            std::string f_str = absl::StrFormat("%.9g", f_val);
            if (f_str.find('.') == std::string::npos && f_str.find('e') == std::string::npos) {
              f_str += ".0f";
            } else {
              f_str += "f";
            }
            statement.expression = LiftedExpression::Variable(f_str);
          } else {
            statement.expression = LiftedExpression::Integer(val, /*hex=*/true);
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAnd:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
            inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary("&", LiftRegisterOrConstant(*inst.rs, tracker),
                                       LiftRegisterOrConstant(*inst.rt, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAndi:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression =
              LiftedExpression::Binary("&", LiftRegisterOrConstant(*inst.rs, tracker),
                                       LiftedExpression::Integer(inst.UnsignedImmediate()));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kOr:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
            inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary("|", LiftRegisterOrConstant(*inst.rs, tracker),
                                       LiftRegisterOrConstant(*inst.rt, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kOri:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression =
              LiftedExpression::Binary("|", LiftRegisterOrConstant(*inst.rs, tracker),
                                       LiftedExpression::Integer(inst.UnsignedImmediate()));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kXor:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
            inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary("^", LiftRegisterOrConstant(*inst.rs, tracker),
                                       LiftRegisterOrConstant(*inst.rt, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kXori:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression =
              LiftedExpression::Binary("^", LiftRegisterOrConstant(*inst.rs, tracker),
                                       LiftedExpression::Integer(inst.UnsignedImmediate()));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kNor:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
            inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (*inst.rs == Register::kZero) {
            statement.expression =
                LiftedExpression::Unary("~", LiftRegisterOrConstant(*inst.rt, tracker));
          } else if (*inst.rt == Register::kZero) {
            statement.expression =
                LiftedExpression::Unary("~", LiftRegisterOrConstant(*inst.rs, tracker));
          } else {
            statement.expression = LiftedExpression::Unary(
                "~", LiftedExpression::Binary("|", LiftRegisterOrConstant(*inst.rs, tracker),
                                              LiftRegisterOrConstant(*inst.rt, tracker)));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSll:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary("<<", LiftRegisterOrConstant(*inst.rt, tracker),
                                       LiftedExpression::Integer(inst.shift_amount));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSrl:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression = LiftedExpression::Binary(
              ">>", LiftedExpression::Unary("(u32)", LiftRegisterOrConstant(*inst.rt, tracker)),
              LiftedExpression::Integer(inst.shift_amount));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSra:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary(">>", LiftRegisterOrConstant(*inst.rt, tracker),
                                       LiftedExpression::Integer(inst.shift_amount));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSllv:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
            inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary("<<", LiftRegisterOrConstant(*inst.rt, tracker),
                                       LiftRegisterOrConstant(*inst.rs, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSrlv:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
            inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression = LiftedExpression::Binary(
              ">>", LiftedExpression::Unary("(u32)", LiftRegisterOrConstant(*inst.rt, tracker)),
              LiftRegisterOrConstant(*inst.rs, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSrav:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
            inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary(">>", LiftRegisterOrConstant(*inst.rt, tracker),
                                       LiftRegisterOrConstant(*inst.rs, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDsll:
      case Opcode::kDsll32:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          uint8_t sa = inst.shift_amount + (inst.opcode == Opcode::kDsll32 ? 32 : 0);
          statement.expression = LiftedExpression::Binary(
              "<<", LiftRegisterOrConstant(*inst.rt, tracker), LiftedExpression::Integer(sa));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDsrl:
      case Opcode::kDsrl32:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          uint8_t sa = inst.shift_amount + (inst.opcode == Opcode::kDsrl32 ? 32 : 0);
          statement.expression = LiftedExpression::Binary(
              ">>", LiftedExpression::Unary("(u64)", LiftRegisterOrConstant(*inst.rt, tracker)),
              LiftedExpression::Integer(sa));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDsra:
      case Opcode::kDsra32:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          uint8_t sa = inst.shift_amount + (inst.opcode == Opcode::kDsra32 ? 32 : 0);
          statement.expression = LiftedExpression::Binary(
              ">>", LiftedExpression::Unary("(s64)", LiftRegisterOrConstant(*inst.rt, tracker)),
              LiftedExpression::Integer(sa));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDsllv:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
            inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary("<<", LiftRegisterOrConstant(*inst.rt, tracker),
                                       LiftRegisterOrConstant(*inst.rs, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDsrlv:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
            inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression = LiftedExpression::Binary(
              ">>", LiftedExpression::Unary("(u64)", LiftRegisterOrConstant(*inst.rt, tracker)),
              LiftRegisterOrConstant(*inst.rs, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDsrav:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
            inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression = LiftedExpression::Binary(
              ">>", LiftedExpression::Unary("(s64)", LiftRegisterOrConstant(*inst.rt, tracker)),
              LiftRegisterOrConstant(*inst.rs, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSlt:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
            inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression =
              LiftedExpression::Binary("<", LiftRegisterOrConstant(*inst.rs, tracker),
                                       LiftRegisterOrConstant(*inst.rt, tracker));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSltu:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
            inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          statement.expression = LiftedExpression::Binary(
              "<", LiftedExpression::Unary("(u32)", LiftRegisterOrConstant(*inst.rs, tracker)),
              LiftedExpression::Unary("(u32)", LiftRegisterOrConstant(*inst.rt, tracker)));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSlti:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression =
              LiftedExpression::Binary("<", LiftRegisterOrConstant(*inst.rs, tracker),
                                       LiftedExpression::Integer(inst.immediate));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSltiu:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          uint32_t uimm = static_cast<uint32_t>(inst.immediate);
          statement.expression = LiftedExpression::Binary(
              "<", LiftedExpression::Unary("(u32)", LiftRegisterOrConstant(*inst.rs, tracker)),
              LiftedExpression::Integer(uimm, /*hex=*/uimm > 0xFFFF));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMult:
      case Opcode::kMultu:
      case Opcode::kDmult:
      case Opcode::kDmultu:
        if (inst.rs.has_value() && inst.rt.has_value()) {
          pending_mult = std::make_unique<PendingMult>();
          pending_mult->is_unsigned =
              (inst.opcode == Opcode::kMultu || inst.opcode == Opcode::kDmultu);
          pending_mult->rs = *inst.rs;
          pending_mult->rt = *inst.rt;

          // Look ahead to find mflo and/or mfhi before next mult/div/mthi/mtlo
          std::optional<size_t> mflo_idx;
          std::optional<size_t> mfhi_idx;
          for (size_t next_i = i + 1; next_i < loop_end; ++next_i) {
            const auto& next_inst = instructions_to_process[next_i];
            if (next_inst.opcode == Opcode::kMflo && !mflo_idx.has_value()) {
              mflo_idx = next_i;
            } else if (next_inst.opcode == Opcode::kMfhi && !mfhi_idx.has_value()) {
              mfhi_idx = next_i;
            }
            if (mflo_idx.has_value() && mfhi_idx.has_value()) break;
            if (next_inst.opcode == Opcode::kMult || next_inst.opcode == Opcode::kMultu ||
                next_inst.opcode == Opcode::kDmult || next_inst.opcode == Opcode::kDmultu ||
                next_inst.opcode == Opcode::kDiv || next_inst.opcode == Opcode::kDivu ||
                next_inst.opcode == Opcode::kDdiv || next_inst.opcode == Opcode::kDdivu ||
                next_inst.opcode == Opcode::kMthi || next_inst.opcode == Opcode::kMtlo) {
              break;
            }
          }

          // Check if rs or rt is clobbered by instructions before mflo or mfhi
          size_t max_read_idx = 0;
          if (mflo_idx.has_value()) max_read_idx = std::max(max_read_idx, *mflo_idx);
          if (mfhi_idx.has_value()) max_read_idx = std::max(max_read_idx, *mfhi_idx);

          bool operands_clobbered = false;
          if (max_read_idx > i + 1) {
            for (size_t check_i = i + 1; check_i < max_read_idx; ++check_i) {
              RegisterUseDef def_ud = GetInstructionUseDef(instructions_to_process[check_i]);
              for (Register r : def_ud.gpr_defs) {
                if (r == *inst.rs || r == *inst.rt) {
                  operands_clobbered = true;
                  break;
                }
              }
              if (operands_clobbered) break;
            }
          }

          if (operands_clobbered) {
            auto lhs = LiftRegisterOrConstant(*inst.rs, tracker);
            auto rhs = LiftRegisterOrConstant(*inst.rt, tracker);
            std::string cast_type = pending_mult->is_unsigned ? "u64)(u32" : "s64";

            auto emit_mflo = [&]() {
              if (mflo_idx.has_value()) {
                const auto& mflo_inst = instructions_to_process[*mflo_idx];
                if (mflo_inst.rd.has_value() && *mflo_inst.rd != Register::kZero) {
                  bool rd_conflict = false;
                  for (size_t check_i = i + 1; check_i < *mflo_idx; ++check_i) {
                    RegisterUseDef ud = GetInstructionUseDef(instructions_to_process[check_i]);
                    for (Register r : ud.gpr_uses) {
                      if (r == *mflo_inst.rd) {
                        rd_conflict = true;
                        break;
                      }
                    }
                    for (Register r : ud.gpr_defs) {
                      if (r == *mflo_inst.rd) {
                        rd_conflict = true;
                        break;
                      }
                    }
                    if (rd_conflict) break;
                  }
                  LiftedStatement lo_stmt;
                  lo_stmt.kind = StatementKind::kAssignment;
                  if (!rd_conflict) {
                    lo_stmt.destination_variable = RegisterVarName(*mflo_inst.rd);
                    lo_stmt.expression = LiftedExpression::Binary("*", lhs->Clone(), rhs->Clone());
                    statements.push_back(std::move(lo_stmt));
                    skipped_instruction_indices.insert(*mflo_idx);
                  } else {
                    lo_stmt.destination_variable = "lo";
                    lo_stmt.expression = LiftedExpression::Binary("*", lhs->Clone(), rhs->Clone());
                    statements.push_back(std::move(lo_stmt));
                  }
                }
              }
            };

            auto emit_mfhi = [&]() {
              if (mfhi_idx.has_value()) {
                const auto& mfhi_inst = instructions_to_process[*mfhi_idx];
                if (mfhi_inst.rd.has_value() && *mfhi_inst.rd != Register::kZero) {
                  bool rd_conflict = false;
                  for (size_t check_i = i + 1; check_i < *mfhi_idx; ++check_i) {
                    RegisterUseDef ud = GetInstructionUseDef(instructions_to_process[check_i]);
                    for (Register r : ud.gpr_uses) {
                      if (r == *mfhi_inst.rd) {
                        rd_conflict = true;
                        break;
                      }
                    }
                    for (Register r : ud.gpr_defs) {
                      if (r == *mfhi_inst.rd) {
                        rd_conflict = true;
                        break;
                      }
                    }
                    if (rd_conflict) break;
                  }
                  LiftedStatement hi_stmt;
                  hi_stmt.kind = StatementKind::kAssignment;
                  if (!rd_conflict) {
                    hi_stmt.destination_variable = RegisterVarName(*mfhi_inst.rd);
                    hi_stmt.expression = LiftedExpression::Binary(
                        ">>",
                        LiftedExpression::Binary(
                            "*", LiftedExpression::Unary("(" + cast_type + ")", lhs->Clone()),
                            LiftedExpression::Unary("(" + cast_type + ")", rhs->Clone())),
                        LiftedExpression::Integer(32));
                    statements.push_back(std::move(hi_stmt));
                    skipped_instruction_indices.insert(*mfhi_idx);
                  } else {
                    hi_stmt.destination_variable = "hi";
                    hi_stmt.expression = LiftedExpression::Binary(
                        ">>",
                        LiftedExpression::Binary(
                            "*", LiftedExpression::Unary("(" + cast_type + ")", lhs->Clone()),
                            LiftedExpression::Unary("(" + cast_type + ")", rhs->Clone())),
                        LiftedExpression::Integer(32));
                    statements.push_back(std::move(hi_stmt));
                  }
                }
              }
            };

            bool emit_mfhi_first = false;
            if (mflo_idx.has_value() && mfhi_idx.has_value()) {
              const auto& mflo_inst = instructions_to_process[*mflo_idx];
              if (mflo_inst.rd.has_value() &&
                  (*mflo_inst.rd == *inst.rs || *mflo_inst.rd == *inst.rt)) {
                emit_mfhi_first = true;
              }
            }

            if (emit_mfhi_first) {
              emit_mfhi();
              emit_mflo();
            } else {
              emit_mflo();
              emit_mfhi();
            }

            pending_mult = nullptr;
          }
        }
        break;

      case Opcode::kDiv:
      case Opcode::kDivu:
      case Opcode::kDdiv:
      case Opcode::kDdivu:
        pending_mult = nullptr;
        if (inst.rs.has_value() && inst.rt.has_value()) {
          bool is_unsigned = (inst.opcode == Opcode::kDivu || inst.opcode == Opcode::kDdivu);
          {
            LiftedStatement lo_stmt;
            lo_stmt.kind = StatementKind::kAssignment;
            lo_stmt.destination_variable = "lo";
            auto num = LiftRegisterOrConstant(*inst.rs, tracker);
            auto den = LiftRegisterOrConstant(*inst.rt, tracker);
            if (is_unsigned) {
              num = LiftedExpression::Unary("(u32)", std::move(num));
              den = LiftedExpression::Unary("(u32)", std::move(den));
            }
            lo_stmt.expression = LiftedExpression::Binary("/", std::move(num), std::move(den));
            statements.push_back(std::move(lo_stmt));
          }
          {
            LiftedStatement hi_stmt;
            hi_stmt.kind = StatementKind::kAssignment;
            hi_stmt.destination_variable = "hi";
            auto num = LiftRegisterOrConstant(*inst.rs, tracker);
            auto den = LiftRegisterOrConstant(*inst.rt, tracker);
            if (is_unsigned) {
              num = LiftedExpression::Unary("(u32)", std::move(num));
              den = LiftedExpression::Unary("(u32)", std::move(den));
            }
            hi_stmt.expression = LiftedExpression::Binary("%", std::move(num), std::move(den));
            statements.push_back(std::move(hi_stmt));
          }
        }
        break;

      case Opcode::kMflo:
        if (inst.rd.has_value() && *inst.rd != Register::kZero) {
          if (pending_mult != nullptr) {
            bool mfhi_follows = false;
            size_t mfhi_idx = 0;
            for (size_t next_i = i + 1; next_i < loop_end; ++next_i) {
              const auto& next_inst = instructions_to_process[next_i];
              if (next_inst.opcode == Opcode::kMfhi) {
                mfhi_follows = true;
                mfhi_idx = next_i;
                break;
              }
              if (next_inst.opcode == Opcode::kMult || next_inst.opcode == Opcode::kMultu ||
                  next_inst.opcode == Opcode::kDmult || next_inst.opcode == Opcode::kDmultu ||
                  next_inst.opcode == Opcode::kDiv || next_inst.opcode == Opcode::kDivu ||
                  next_inst.opcode == Opcode::kDdiv || next_inst.opcode == Opcode::kDdivu ||
                  next_inst.opcode == Opcode::kMthi || next_inst.opcode == Opcode::kMtlo) {
                break;
              }
            }
            if (mfhi_follows && (inst.rd == pending_mult->rs || inst.rd == pending_mult->rt)) {
              const auto& mfhi_inst = instructions_to_process[mfhi_idx];
              if (mfhi_inst.rd.has_value() && *mfhi_inst.rd != Register::kZero) {
                LiftedStatement hi_stmt;
                hi_stmt.kind = StatementKind::kAssignment;
                hi_stmt.destination_variable = RegisterVarName(*mfhi_inst.rd);
                std::string cast_type = pending_mult->is_unsigned ? "u64)(u32" : "s64";
                auto lhs = LiftRegisterOrConstant(pending_mult->rs, tracker);
                auto rhs = LiftRegisterOrConstant(pending_mult->rt, tracker);
                hi_stmt.expression = LiftedExpression::Binary(
                    ">>",
                    LiftedExpression::Binary(
                        "*", LiftedExpression::Unary("(" + cast_type + ")", std::move(lhs)),
                        LiftedExpression::Unary("(" + cast_type + ")", std::move(rhs))),
                    LiftedExpression::Integer(32));
                statements.push_back(std::move(hi_stmt));
              }
              skipped_instruction_indices.insert(mfhi_idx);
            }
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (pending_mult != nullptr) {
            statement.expression =
                LiftedExpression::Binary("*", LiftRegisterOrConstant(pending_mult->rs, tracker),
                                         LiftRegisterOrConstant(pending_mult->rt, tracker));
            if (inst.rd == pending_mult->rs || inst.rd == pending_mult->rt) {
              pending_mult = nullptr;
            }
          } else {
            statement.expression = LiftedExpression::Variable("lo");
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMfhi:
        if (inst.rd.has_value() && *inst.rd != Register::kZero) {
          if (pending_mult != nullptr) {
            bool mflo_follows = false;
            size_t mflo_idx = 0;
            for (size_t next_i = i + 1; next_i < loop_end; ++next_i) {
              const auto& next_inst = instructions_to_process[next_i];
              if (next_inst.opcode == Opcode::kMflo) {
                mflo_follows = true;
                mflo_idx = next_i;
                break;
              }
              if (next_inst.opcode == Opcode::kMult || next_inst.opcode == Opcode::kMultu ||
                  next_inst.opcode == Opcode::kDmult || next_inst.opcode == Opcode::kDmultu ||
                  next_inst.opcode == Opcode::kDiv || next_inst.opcode == Opcode::kDivu ||
                  next_inst.opcode == Opcode::kDdiv || next_inst.opcode == Opcode::kDdivu ||
                  next_inst.opcode == Opcode::kMthi || next_inst.opcode == Opcode::kMtlo) {
                break;
              }
            }
            if (mflo_follows && (inst.rd == pending_mult->rs || inst.rd == pending_mult->rt)) {
              const auto& mflo_inst = instructions_to_process[mflo_idx];
              if (mflo_inst.rd.has_value() && *mflo_inst.rd != Register::kZero) {
                LiftedStatement lo_stmt;
                lo_stmt.kind = StatementKind::kAssignment;
                lo_stmt.destination_variable = RegisterVarName(*mflo_inst.rd);
                lo_stmt.expression =
                    LiftedExpression::Binary("*", LiftRegisterOrConstant(pending_mult->rs, tracker),
                                             LiftRegisterOrConstant(pending_mult->rt, tracker));
                statements.push_back(std::move(lo_stmt));
              }
              skipped_instruction_indices.insert(mflo_idx);
            }
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (pending_mult != nullptr) {
            std::string cast_type = pending_mult->is_unsigned ? "u64)(u32" : "s64";
            auto lhs = LiftRegisterOrConstant(pending_mult->rs, tracker);
            auto rhs = LiftRegisterOrConstant(pending_mult->rt, tracker);
            statement.expression = LiftedExpression::Binary(
                ">>",
                LiftedExpression::Binary(
                    "*", LiftedExpression::Unary("(" + cast_type + ")", std::move(lhs)),
                    LiftedExpression::Unary("(" + cast_type + ")", std::move(rhs))),
                LiftedExpression::Integer(32));
            if (inst.rd == pending_mult->rs || inst.rd == pending_mult->rt) {
              pending_mult = nullptr;
            }
          } else {
            statement.expression = LiftedExpression::Variable("hi");
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMtlo:
      case Opcode::kMthi:
        pending_mult = nullptr;
        break;

      case Opcode::kLd:
      case Opcode::kLld:
      case Opcode::kLw:
      case Opcode::kLwu:
      case Opcode::kLh:
      case Opcode::kLb:
      case Opcode::kLhu:
      case Opcode::kLbu:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && *inst.rt != Register::kSp &&
            (!has_frame_pointer || *inst.rt != Register::kFp) && inst.rs.has_value()) {
          const char* load_type = "s32";
          if (inst.opcode == Opcode::kLd || inst.opcode == Opcode::kLld) {
            load_type = "s64";
          } else if (inst.opcode == Opcode::kLwu) {
            load_type = "u32";
          } else if (inst.opcode == Opcode::kLh) {
            load_type = "s16";
          } else if (inst.opcode == Opcode::kLhu) {
            load_type = "u16";
          } else if (inst.opcode == Opcode::kLb) {
            load_type = "s8";
          } else if (inst.opcode == Opcode::kLbu) {
            load_type = "u8";
          }

          LiftedStatement statement;
          bool is_stack_base =
              (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
          if (is_stack_base) {
            if (*inst.rt == Register::kRa || (has_frame_pointer && *inst.rt == Register::kFp)) {
              // Function epilogue restoring $ra or $fp, omit from C body
              break;
            }
            if (saved_reg_offsets != nullptr) {
              auto it = saved_reg_offsets->find(*inst.rt);
              if (it != saved_reg_offsets->end() && it->second == inst.immediate) {
                // Callee-saved register epilogue restore, omit from C body
                break;
              }
            }
          }
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (is_stack_base) {
            int32_t offset = inst.immediate;
            if (is_sp_offset(offset)) {
              statement.expression = LiftedExpression::Load(load_type, make_sp_addr(offset));
            } else {
              uint32_t base_offset = frame_size + 16;
              int arg_idx = -1;
              if (offset >= static_cast<int32_t>(base_offset)) {
                arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
              }
              if (arg_idx >= 0) {
                std::string arg_name = absl::StrFormat("arg%d", arg_idx);
                if (inst.opcode == Opcode::kLhu) {
                  statement.expression =
                      LiftedExpression::Unary("(u16)", LiftedExpression::Variable(arg_name));
                } else if (inst.opcode == Opcode::kLh) {
                  statement.expression =
                      LiftedExpression::Unary("(s16)", LiftedExpression::Variable(arg_name));
                } else if (inst.opcode == Opcode::kLbu) {
                  statement.expression =
                      LiftedExpression::Unary("(u8)", LiftedExpression::Variable(arg_name));
                } else if (inst.opcode == Opcode::kLb) {
                  statement.expression =
                      LiftedExpression::Unary("(s8)", LiftedExpression::Variable(arg_name));
                } else {
                  statement.expression = LiftedExpression::Variable(arg_name);
                }
              } else {
                int32_t base_word_offset = offset & ~3;
                int32_t byte_in_word = offset & 3;
                bool is_subword_from_word =
                    (byte_in_word != 0 && !local_stack_stores.contains(offset) &&
                     local_stack_stores.contains(base_word_offset));
                if (!is_subword_from_word) {
                  auto var_expr = LiftedExpression::Variable(StackVarName(offset));
                  if (inst.opcode == Opcode::kLhu) {
                    statement.expression = LiftedExpression::Unary("(u16)", std::move(var_expr));
                  } else if (inst.opcode == Opcode::kLh) {
                    statement.expression = LiftedExpression::Unary("(s16)", std::move(var_expr));
                  } else if (inst.opcode == Opcode::kLbu) {
                    statement.expression = LiftedExpression::Unary("(u8)", std::move(var_expr));
                  } else if (inst.opcode == Opcode::kLb) {
                    statement.expression = LiftedExpression::Unary("(s8)", std::move(var_expr));
                  } else {
                    statement.expression = std::move(var_expr);
                  }
                } else {
                  auto base_var = LiftedExpression::Variable(StackVarName(base_word_offset));
                  if (inst.opcode == Opcode::kLhu) {
                    if (byte_in_word == 2) {
                      statement.expression = LiftedExpression::Unary("(u16)", std::move(base_var));
                    } else {
                      statement.expression = LiftedExpression::Unary(
                          "(u16)", LiftedExpression::Binary(
                                       ">>", LiftedExpression::Unary("(u32)", std::move(base_var)),
                                       LiftedExpression::Integer((2 - byte_in_word) * 8)));
                    }
                  } else if (inst.opcode == Opcode::kLh) {
                    if (byte_in_word == 2) {
                      statement.expression = LiftedExpression::Unary("(s16)", std::move(base_var));
                    } else {
                      statement.expression = LiftedExpression::Binary(
                          ">>", std::move(base_var),
                          LiftedExpression::Integer((2 - byte_in_word) * 8));
                    }
                  } else if (inst.opcode == Opcode::kLbu) {
                    if (byte_in_word == 3) {
                      statement.expression = LiftedExpression::Unary("(u8)", std::move(base_var));
                    } else {
                      statement.expression = LiftedExpression::Unary(
                          "(u8)", LiftedExpression::Binary(
                                      ">>", LiftedExpression::Unary("(u32)", std::move(base_var)),
                                      LiftedExpression::Integer((3 - byte_in_word) * 8)));
                    }
                  } else if (inst.opcode == Opcode::kLb) {
                    if (byte_in_word == 3) {
                      statement.expression = LiftedExpression::Unary("(s8)", std::move(base_var));
                    } else {
                      statement.expression = LiftedExpression::Unary(
                          "(s8)", LiftedExpression::Binary(
                                      ">>", LiftedExpression::Unary("(u32)", std::move(base_var)),
                                      LiftedExpression::Integer((3 - byte_in_word) * 8)));
                    }
                  } else {
                    statement.expression = std::move(base_var);
                  }
                }
              }
            }
          } else {
            std::string effective_load_type = load_type;
            if (tracker.IsConstant(*inst.rs)) {
              uint32_t const_addr = *tracker.GetConstant(*inst.rs) + inst.immediate;
              if (const_addr >= 0xA0000000 && const_addr <= 0xBFFFFFFF) {
                effective_load_type = "volatile " + std::string(load_type);
              }
            } else if (sp_alloc_idx.has_value() && i < *sp_alloc_idx) {
              effective_load_type = "const " + std::string(load_type);
            }
            statement.expression = LiftedExpression::Load(
                effective_load_type, make_reg_offset_expr(*inst.rs, inst.immediate));
          }
          if (statement.kind == StatementKind::kAssignment && statement.expression &&
              statement.expression->kind == ExpressionKind::kVariable &&
              statement.destination_variable == statement.expression->name) {
            break;
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kLwl:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && *inst.rt != Register::kSp &&
            (!has_frame_pointer || *inst.rt != Register::kFp) && inst.rs.has_value()) {
          flush_pending_lwl();
          pending_lwl = std::make_unique<PendingUnalignedAccess>();
          pending_lwl->rt = *inst.rt;
          pending_lwl->rs =
              (has_frame_pointer && *inst.rs == Register::kFp) ? Register::kSp : *inst.rs;
          pending_lwl->offset = inst.immediate;
        }
        break;

      case Opcode::kLwr:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && *inst.rt != Register::kSp &&
            (!has_frame_pointer || *inst.rt != Register::kFp) && inst.rs.has_value()) {
          int16_t offset = inst.immediate;
          Register base_reg =
              (has_frame_pointer && *inst.rs == Register::kFp) ? Register::kSp : *inst.rs;
          if (pending_lwl != nullptr) {
            if (pending_lwl->rt == *inst.rt && pending_lwl->rs == base_reg) {
              offset = pending_lwl->offset;
              pending_lwl = nullptr;
            } else {
              flush_pending_lwl();
            }
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (base_reg == Register::kSp) {
            if (is_sp_offset(offset)) {
              statement.expression = LiftedExpression::Load("s32", make_sp_addr(offset));
            } else {
              statement.expression = LiftedExpression::Variable(StackVarName(offset));
            }
          } else {
            statement.expression =
                LiftedExpression::Load("s32", make_reg_offset_expr(base_reg, offset));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSd:
      case Opcode::kScd:
      case Opcode::kSw:
      case Opcode::kSh:
      case Opcode::kSb:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          bool is_stack_base =
              (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
          if (is_stack_base) {
            if (*inst.rt == Register::kRa || (has_frame_pointer && *inst.rt == Register::kFp)) {
              // Function prologue saving $ra or $fp, omit from C body
              break;
            }
            if (saved_reg_offsets != nullptr) {
              auto it = saved_reg_offsets->find(*inst.rt);
              if (it != saved_reg_offsets->end() && it->second == inst.immediate) {
                // Callee-saved register save in prologue, omit from C body
                break;
              }
            }
          }
          const char* store_type = "s32";
          if (inst.opcode == Opcode::kSd || inst.opcode == Opcode::kScd) {
            store_type = "s64";
          } else if (inst.opcode == Opcode::kSh) {
            store_type = "s16";
          } else if (inst.opcode == Opcode::kSb) {
            store_type = "s8";
          } else if (inst.opcode == Opcode::kSw && is_float_carrier_gpr(*inst.rt)) {
            store_type = "f32";
          }

          LiftedStatement statement;
          if (is_stack_base) {
            int32_t offset = inst.immediate;
            if (is_sp_offset(offset)) {
              statement.kind = StatementKind::kStore;
              statement.store_type = store_type;
              statement.destination_address = make_sp_addr(offset);
              statement.expression = lift_reg(*inst.rt);
            } else {
              uint32_t base_offset = frame_size + 16;
              int arg_idx = -1;
              if (offset >= static_cast<int32_t>(base_offset)) {
                arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
              }
              if (arg_idx >= 0) {
                std::string arg_name = absl::StrFormat("arg%d", arg_idx);
                statement.kind = StatementKind::kAssignment;
                statement.destination_variable = arg_name;
                auto val_expr = lift_reg(*inst.rt);
                if (inst.opcode == Opcode::kSh) {
                  statement.expression = LiftedExpression::Unary("(u16)", std::move(val_expr));
                } else if (inst.opcode == Opcode::kSb) {
                  statement.expression = LiftedExpression::Unary("(u8)", std::move(val_expr));
                } else {
                  statement.expression = std::move(val_expr);
                }
              } else {
                statement.kind = StatementKind::kAssignment;
                statement.destination_variable = StackVarName(offset);
                auto val_expr = lift_reg(*inst.rt);
                if (inst.opcode == Opcode::kSh) {
                  statement.expression = LiftedExpression::Unary("(u16)", std::move(val_expr));
                } else if (inst.opcode == Opcode::kSb) {
                  statement.expression = LiftedExpression::Unary("(u8)", std::move(val_expr));
                } else {
                  statement.expression = std::move(val_expr);
                }
              }
            }
          } else {
            statement.kind = StatementKind::kStore;
            std::string effective_store_type = store_type;
            if (tracker.IsConstant(*inst.rs)) {
              uint32_t const_addr = *tracker.GetConstant(*inst.rs) + inst.immediate;
              if (const_addr >= 0xA0000000 && const_addr <= 0xBFFFFFFF) {
                effective_store_type = "volatile " + std::string(store_type);
              }
            }
            statement.store_type = effective_store_type;
            statement.destination_address = make_reg_offset_expr(*inst.rs, inst.immediate);
            statement.expression = lift_reg(*inst.rt);
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSwl:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          pending_swl = std::make_unique<PendingUnalignedAccess>();
          pending_swl->rt = *inst.rt;
          pending_swl->rs =
              (has_frame_pointer && *inst.rs == Register::kFp) ? Register::kSp : *inst.rs;
          pending_swl->offset = inst.immediate;
        }
        break;

      case Opcode::kSwr:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          int16_t offset = inst.immediate;
          Register base_reg =
              (has_frame_pointer && *inst.rs == Register::kFp) ? Register::kSp : *inst.rs;
          if (pending_swl != nullptr && pending_swl->rt == *inst.rt &&
              pending_swl->rs == base_reg) {
            offset = pending_swl->offset;
            pending_swl = nullptr;
          }
          if (base_reg == Register::kSp) {
            LiftedStatement statement;
            if (is_sp_offset(offset)) {
              statement.kind = StatementKind::kStore;
              statement.store_type = "s32";
              statement.destination_address = make_sp_addr(offset);
              statement.expression = lift_reg(*inst.rt);
            } else {
              statement.kind = StatementKind::kAssignment;
              statement.destination_variable = StackVarName(offset);
              statement.expression = lift_reg(*inst.rt);
            }
            statements.push_back(std::move(statement));
          } else {
            for (int b = 0; b < 4; ++b) {
              LiftedStatement statement;
              statement.kind = StatementKind::kStore;
              statement.store_type = "u8";
              int32_t byte_offset = static_cast<int32_t>(offset) + b;
              if (byte_offset == 0) {
                statement.destination_address = lift_reg(base_reg);
              } else {
                statement.destination_address = LiftedExpression::Binary(
                    "+", lift_reg(base_reg), LiftedExpression::Integer(byte_offset));
              }
              if (b == 3) {
                statement.expression = lift_reg(*inst.rt);
              } else {
                statement.expression = LiftedExpression::Binary(
                    ">>", lift_reg(*inst.rt), LiftedExpression::Integer((3 - b) * 8));
              }
              statements.push_back(std::move(statement));
            }
          }
        }
        break;

      case Opcode::kJal:
      case Opcode::kJalr:
      case Opcode::kBltzal:
      case Opcode::kBgezal:
      case Opcode::kBltzall:
      case Opcode::kBgezall:
        LiftCallInstruction(inst, i, instructions_to_process, symbol_index,
                            function_parameter_counts, function_fp_parameter_counts,
                            functions_returning_v0, functions_returning_f0, tracker, statements,
                            sp_address_taken, call_return_info, sp_base_offset, saved_reg_offsets,
                            ctx.cfg, ctx.current_block_id);
        break;

      case Opcode::kJr:
        if (inst.rs == Register::kRa) {
          pending_return = true;
        }
        break;

      case Opcode::kLwc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          bool is_stack_base =
              (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
          if (is_stack_base && saved_fpr_offsets != nullptr) {
            auto it = saved_fpr_offsets->find(*inst.ft);
            if (it != saved_fpr_offsets->end() && it->second == inst.immediate) {
              // Callee-saved float register epilogue restore, omit from C body
              break;
            }
          }
          int ft_idx = static_cast<int>(*inst.ft);
          bool only_used_by_int_to_fp_cvt = false;
          for (size_t look = i + 1; look < loop_end; ++look) {
            const Instruction& future_inst = instructions_to_process[look];
            if ((future_inst.opcode == Opcode::kCvtSW || future_inst.opcode == Opcode::kCvtDW) &&
                future_inst.fs == *inst.ft) {
              bool reused_after_cvt = false;
              if (future_inst.fd != *inst.ft) {
                for (size_t k = look + 1; k < loop_end; ++k) {
                  RegisterUseDef kud = GetInstructionUseDef(instructions_to_process[k]);
                  for (FpRegister u : kud.fpr_uses) {
                    if (u == *inst.ft || (static_cast<int>(u) & ~1) == (ft_idx & ~1)) {
                      reused_after_cvt = true;
                      break;
                    }
                  }
                  if (reused_after_cvt) break;
                  bool redefined = false;
                  for (FpRegister d : kud.fpr_defs) {
                    if (d == *inst.ft || (static_cast<int>(d) & ~1) == (ft_idx & ~1)) {
                      redefined = true;
                      break;
                    }
                  }
                  if (redefined || instructions_to_process[k].IsCall()) break;
                }
              }
              only_used_by_int_to_fp_cvt = !reused_after_cvt;
              break;
            }
            RegisterUseDef fud = GetInstructionUseDef(future_inst);
            bool touches_ft_or_rs = false;
            for (FpRegister u : fud.fpr_uses) {
              if (u == *inst.ft || (static_cast<int>(u) & ~1) == (ft_idx & ~1)) {
                touches_ft_or_rs = true;
              }
            }
            for (FpRegister d : fud.fpr_defs) {
              if (d == *inst.ft || (static_cast<int>(d) & ~1) == (ft_idx & ~1)) {
                touches_ft_or_rs = true;
              }
            }
            for (Register d : fud.gpr_defs) {
              if (d == *inst.rs) {
                touches_ft_or_rs = true;
              }
            }
            if (touches_ft_or_rs || future_inst.IsCall() || future_inst.IsStore()) {
              break;
            }
          }
          if (only_used_by_int_to_fp_cvt) {
            if (is_stack_base) {
              int32_t offset = inst.immediate;
              if (is_sp_offset(offset)) {
                pending_int_to_fp[ft_idx] = LiftedExpression::Load("s32", make_sp_addr(offset));
              } else {
                uint32_t base_offset = frame_size + 16;
                int arg_idx = -1;
                if (offset >= static_cast<int32_t>(base_offset)) {
                  arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
                }
                if (arg_idx >= 0) {
                  pending_int_to_fp[ft_idx] =
                      LiftedExpression::Variable(absl::StrFormat("arg%d", arg_idx));
                } else {
                  pending_int_to_fp[ft_idx] = LiftedExpression::Variable(StackVarName(offset));
                }
              }
            } else {
              pending_int_to_fp[ft_idx] =
                  LiftedExpression::Load("s32", make_reg_offset_expr(*inst.rs, inst.immediate));
            }
            fpr_const_bits[ft_idx] = std::nullopt;
            break;
          }

          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.ft);
          if (is_stack_base) {
            int32_t offset = inst.immediate;
            if (is_sp_offset(offset)) {
              statement.expression = LiftedExpression::Load("f32", make_sp_addr(offset));
            } else {
              uint32_t base_offset = frame_size + 16;
              int arg_idx = -1;
              if (offset >= static_cast<int32_t>(base_offset)) {
                arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
              }
              if (arg_idx >= 0) {
                std::string arg_name = absl::StrFormat("arg%d", arg_idx);
                if (float_stack_param_slots != nullptr &&
                    float_stack_param_slots->contains(arg_idx)) {
                  statement.expression = LiftedExpression::Variable(arg_name);
                } else {
                  statement.expression =
                      LiftedExpression::Unary("*(f32*)&", LiftedExpression::Variable(arg_name));
                }
              } else {
                if (float_local_stack_offsets != nullptr &&
                    float_local_stack_offsets->contains(offset)) {
                  statement.expression = LiftedExpression::Variable(StackVarName(offset));
                } else {
                  statement.expression = LiftedExpression::Load(
                      "f32", LiftedExpression::Unary(
                                 "&", LiftedExpression::Variable(StackVarName(offset))));
                }
              }
            }
          } else {
            const char* fp_load_type =
                (sp_alloc_idx.has_value() && i < *sp_alloc_idx) ? "const f32" : "f32";
            statement.expression = LiftedExpression::Load(
                fp_load_type, make_reg_offset_expr(*inst.rs, inst.immediate));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSwc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          int ft_idx = static_cast<int>(*inst.ft);
          bool is_int_trunc_store =
              (ft_idx >= 0 && ft_idx < 32 && pending_fp_to_int[ft_idx] != nullptr);
          auto val_expr =
              is_int_trunc_store ? std::move(pending_fp_to_int[ft_idx]) : LiftFpRegister(*inst.ft);
          const char* store_type = is_int_trunc_store ? "s32" : "f32";
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = store_type;
          bool is_stack_base =
              (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
          if (is_stack_base) {
            if (saved_fpr_offsets != nullptr) {
              auto it = saved_fpr_offsets->find(*inst.ft);
              if (it != saved_fpr_offsets->end() && it->second == inst.immediate) {
                // Callee-saved float register prologue save, omit from C body
                break;
              }
            }
            if (is_sp_offset(inst.immediate)) {
              statement.destination_address = make_sp_addr(inst.immediate);
              statement.expression = std::move(val_expr);
            } else {
              uint32_t base_offset = frame_size + 16;
              int arg_idx = -1;
              if (inst.immediate >= static_cast<int32_t>(base_offset)) {
                int candidate_idx = 4 + (inst.immediate - static_cast<int32_t>(base_offset)) / 4;
                if (float_stack_param_slots != nullptr &&
                    float_stack_param_slots->contains(candidate_idx)) {
                  arg_idx = candidate_idx;
                }
              }
              if (arg_idx >= 0) {
                std::string arg_name = absl::StrFormat("arg%d", arg_idx);
                statement.kind = StatementKind::kAssignment;
                statement.destination_variable = arg_name;
                statement.expression = std::move(val_expr);
              } else if (is_int_trunc_store ||
                         (float_local_stack_offsets != nullptr &&
                          float_local_stack_offsets->contains(inst.immediate))) {
                statement.kind = StatementKind::kAssignment;
                statement.destination_variable = StackVarName(inst.immediate);
                statement.expression = std::move(val_expr);
              } else {
                statement.destination_address = LiftedExpression::Unary(
                    "&", LiftedExpression::Variable(StackVarName(inst.immediate)));
                statement.expression = std::move(val_expr);
              }
            }
          } else {
            statement.destination_address = make_reg_offset_expr(*inst.rs, inst.immediate);
            statement.expression = std::move(val_expr);
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kLdc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.ft, /*is_double=*/true);
          bool is_stack_base =
              (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
          if (is_stack_base) {
            int32_t offset = inst.immediate;
            if (saved_fpr_offsets != nullptr && saved_fpr_offsets->contains(*inst.ft) &&
                saved_fpr_offsets->at(*inst.ft) == offset) {
              // Function epilogue restoring callee-saved float register, omit from C body
              break;
            }
            if (is_sp_offset(offset)) {
              statement.expression = LiftedExpression::Load("f64", make_sp_addr(offset));
            } else {
              uint32_t base_offset = frame_size + 16;
              int arg_idx = -1;
              if (offset >= static_cast<int32_t>(base_offset)) {
                arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
              }
              if (arg_idx >= 0) {
                statement.expression = LiftedExpression::Unary(
                    "*(f64*)&", LiftedExpression::Variable(absl::StrFormat("arg%d", arg_idx)));
              } else {
                statement.expression = LiftedExpression::Load(
                    "f64",
                    LiftedExpression::Unary("&", LiftedExpression::Variable(StackVarName(offset))));
              }
            }
          } else {
            statement.expression =
                LiftedExpression::Load("f64", make_reg_offset_expr(*inst.rs, inst.immediate));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSdc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "f64";
          bool is_stack_base =
              (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
          if (is_stack_base) {
            int32_t offset = inst.immediate;
            if (saved_fpr_offsets != nullptr && saved_fpr_offsets->contains(*inst.ft) &&
                saved_fpr_offsets->at(*inst.ft) == offset) {
              // Function prologue saving callee-saved float register, omit from C body
              break;
            }
            if (is_sp_offset(inst.immediate)) {
              statement.destination_address = make_sp_addr(inst.immediate);
            } else {
              statement.destination_address = LiftedExpression::Unary(
                  "&", LiftedExpression::Variable(StackVarName(inst.immediate)));
            }
          } else {
            statement.destination_address = make_reg_offset_expr(*inst.rs, inst.immediate);
          }
          statement.expression = LiftFpRegister(*inst.ft, /*is_double=*/true);
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMfc1:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.fs.has_value()) {
          int fs_idx = static_cast<int>(*inst.fs);
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          bool is_arg_reg =
              (*inst.rt == Register::kA1 || *inst.rt == Register::kA2 || *inst.rt == Register::kA3);
          if (pending_fp_to_int[fs_idx] != nullptr) {
            statement.expression = std::move(pending_fp_to_int[fs_idx]);
          } else if (float_parameters_in_general_registers != nullptr &&
                     float_parameters_in_general_registers->contains(*inst.rt) && !is_arg_reg) {
            statement.expression = LiftFpRegister(*inst.fs, /*is_double=*/false);
          } else {
            statement.expression =
                LiftedExpression::Unary("*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDmfc1:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression =
              LiftedExpression::Unary("*(s64*)&", LiftFpRegister(*inst.fs, /*is_double=*/true));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMtc1:
        if (inst.fs.has_value() && inst.rt.has_value()) {
          int fs_idx = static_cast<int>(*inst.fs);
          bool only_used_by_int_to_fp_cvt = false;
          bool rt_modified_before_cvt = false;
          size_t cvt_look_idx = 0;
          if (*inst.rt != Register::kZero) {
            for (size_t look = i + 1; look < loop_end; ++look) {
              const Instruction& future_inst = instructions_to_process[look];
              if ((future_inst.opcode == Opcode::kCvtSW || future_inst.opcode == Opcode::kCvtDW) &&
                  future_inst.fs == *inst.fs && future_inst.fd.has_value()) {
                bool reused_after_cvt = false;
                if (future_inst.fd != *inst.fs) {
                  for (size_t k = look + 1; k < loop_end; ++k) {
                    RegisterUseDef kud = GetInstructionUseDef(instructions_to_process[k]);
                    for (FpRegister u : kud.fpr_uses) {
                      if (u == *inst.fs || (static_cast<int>(u) & ~1) == (fs_idx & ~1)) {
                        reused_after_cvt = true;
                        break;
                      }
                    }
                    if (reused_after_cvt) break;
                    bool redefined = false;
                    for (FpRegister d : kud.fpr_defs) {
                      if (d == *inst.fs || (static_cast<int>(d) & ~1) == (fs_idx & ~1)) {
                        redefined = true;
                        break;
                      }
                    }
                    if (redefined || instructions_to_process[k].IsCall()) break;
                  }
                }
                only_used_by_int_to_fp_cvt = !reused_after_cvt;
                cvt_look_idx = look;
                break;
              }
              RegisterUseDef fud = GetInstructionUseDef(future_inst);
              bool touches_fs = false;
              for (FpRegister u : fud.fpr_uses) {
                if (u == *inst.fs || (static_cast<int>(u) & ~1) == (fs_idx & ~1)) {
                  touches_fs = true;
                }
              }
              for (FpRegister d : fud.fpr_defs) {
                if (d == *inst.fs || (static_cast<int>(d) & ~1) == (fs_idx & ~1)) {
                  touches_fs = true;
                }
              }
              for (Register d : fud.gpr_defs) {
                if (d == *inst.rt) {
                  rt_modified_before_cvt = true;
                }
              }
              if (touches_fs || future_inst.IsCall()) {
                break;
              }
            }
          }
          if (only_used_by_int_to_fp_cvt && !rt_modified_before_cvt) {
            pending_int_to_fp[fs_idx] = LiftRegisterOrConstant(*inst.rt, tracker);
            fpr_const_bits[fs_idx] = std::nullopt;
            break;
          }
          if (only_used_by_int_to_fp_cvt && rt_modified_before_cvt) {
            const Instruction& cvt_inst = instructions_to_process[cvt_look_idx];
            FpRegister target_fd = *cvt_inst.fd;
            int target_fd_idx = static_cast<int>(target_fd);
            bool fd_clobbered_between = false;
            absl::flat_hash_set<size_t> folded_staging_indices;
            for (size_t m = i + 1; m < cvt_look_idx; ++m) {
              if (skipped_instruction_indices.contains(m) || folded_staging_indices.contains(m)) {
                continue;
              }
              const Instruction& mid_inst = instructions_to_process[m];
              if (mid_inst.opcode == Opcode::kMtc1 && mid_inst.fs == target_fd &&
                  mid_inst.rt.has_value() && *mid_inst.rt != Register::kZero) {
                bool staged_cvt_found = false;
                for (size_t m2 = m + 1; m2 < cvt_look_idx; ++m2) {
                  const Instruction& m2_inst = instructions_to_process[m2];
                  if ((m2_inst.opcode == Opcode::kCvtSW || m2_inst.opcode == Opcode::kCvtDW) &&
                      m2_inst.fs == target_fd && m2_inst.fd.has_value() &&
                      *m2_inst.fd != target_fd) {
                    folded_staging_indices.insert(m2);
                    staged_cvt_found = true;
                    break;
                  }
                  RegisterUseDef m2_ud = GetInstructionUseDef(m2_inst);
                  bool touches_target_or_rt = false;
                  for (FpRegister u : m2_ud.fpr_uses) {
                    if (u == target_fd || (static_cast<int>(u) & ~1) == (target_fd_idx & ~1)) {
                      touches_target_or_rt = true;
                    }
                  }
                  for (FpRegister d : m2_ud.fpr_defs) {
                    if (d == target_fd || (static_cast<int>(d) & ~1) == (target_fd_idx & ~1)) {
                      touches_target_or_rt = true;
                    }
                  }
                  for (Register d : m2_ud.gpr_defs) {
                    if (d == *mid_inst.rt) {
                      touches_target_or_rt = true;
                    }
                  }
                  if (touches_target_or_rt || m2_inst.IsCall()) break;
                }
                if (staged_cvt_found) {
                  continue;
                }
              }
              RegisterUseDef mud = GetInstructionUseDef(mid_inst);
              for (FpRegister u : mud.fpr_uses) {
                if (u == target_fd || (static_cast<int>(u) & ~1) == (target_fd_idx & ~1)) {
                  fd_clobbered_between = true;
                }
              }
              for (FpRegister d : mud.fpr_defs) {
                if (d == target_fd || (static_cast<int>(d) & ~1) == (target_fd_idx & ~1)) {
                  fd_clobbered_between = true;
                }
              }
              if (fd_clobbered_between) break;
            }
            if (!fd_clobbered_between) {
              bool is_double_cvt = (cvt_inst.opcode == Opcode::kCvtDW);
              LiftedStatement statement;
              statement.kind = StatementKind::kAssignment;
              statement.destination_variable = FpRegisterVarName(target_fd, is_double_cvt);
              statement.expression = LiftedExpression::Unary(
                  is_double_cvt ? "(f64)" : "(f32)", LiftRegisterOrConstant(*inst.rt, tracker));
              statements.push_back(std::move(statement));
              skipped_instruction_indices.insert(cvt_look_idx);
              fpr_const_bits[fs_idx] = std::nullopt;
              fpr_const_bits[target_fd_idx] = std::nullopt;
              break;
            }
          }

          uint32_t val_bits = 0;
          bool has_val_bits = false;
          if (*inst.rt == Register::kZero) {
            val_bits = 0;
            has_val_bits = true;
          } else {
            TrackedValue tv = tracker.GetRegisterValue(*inst.rt);
            if (tv.kind == ValueKind::kConstant || tv.kind == ValueKind::kSymbolAddress) {
              val_bits = tv.constant_value;
              has_val_bits = true;
            } else if (tv.kind == ValueKind::kSymbolHi) {
              val_bits = tv.symbol_hi;
              has_val_bits = true;
            }
          }

          if (has_val_bits) {
            fpr_const_bits[fs_idx] = val_bits;
          } else {
            fpr_const_bits[fs_idx] = std::nullopt;
          }

          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fs, /*is_double=*/false);
          if (*inst.rt == Register::kZero) {
            statement.expression = LiftedExpression::Variable("0.0f");
          } else if (is_active_float_gpr(*inst.rt)) {
            statement.expression = LiftedExpression::Variable(RegisterVarName(*inst.rt));
          } else if (*inst.rt == Register::kAt && has_val_bits && (val_bits & 0x7F800000u) != 0 &&
                     (val_bits & 0x7F800000u) != 0x7F800000u) {
            float f_val = 0.0f;
            std::memcpy(&f_val, &val_bits, sizeof(f_val));
            std::string f_str = absl::StrFormat("%.9g", f_val);
            if (f_str.find('.') == std::string::npos && f_str.find('e') == std::string::npos) {
              f_str += ".0f";
            } else {
              f_str += "f";
            }
            statement.expression = LiftedExpression::Variable(f_str);
          } else {
            statement.expression = LiftedExpression::Unary(
                "*(f32*)&", LiftedExpression::Variable(RegisterVarName(*inst.rt)));
          }
          statements.push_back(std::move(statement));

          int even_idx = fs_idx & ~1;
          int odd_idx = even_idx + 1;
          if (fpr_const_bits[even_idx].has_value() && fpr_const_bits[odd_idx].has_value()) {
            uint64_t d_bits =
                (static_cast<uint64_t>(*fpr_const_bits[odd_idx]) << 32) | *fpr_const_bits[even_idx];
            double d_val = 0.0;
            std::memcpy(&d_val, &d_bits, sizeof(d_val));
            if (std::isfinite(d_val)) {
              LiftedStatement d_stmt;
              d_stmt.kind = StatementKind::kAssignment;
              d_stmt.destination_variable =
                  FpRegisterVarName(static_cast<FpRegister>(even_idx), /*is_double=*/true);
              if (d_val == 0.0) {
                d_stmt.expression = LiftedExpression::Variable("0.0");
              } else {
                std::string d_str = absl::StrFormat("%.17g", d_val);
                if (d_str.find('.') == std::string::npos && d_str.find('e') == std::string::npos) {
                  d_str += ".0";
                }
                d_stmt.expression = LiftedExpression::Variable(d_str);
              }
              statements.push_back(std::move(d_stmt));
            }
          }
        }
        break;

      case Opcode::kDmtc1:
        if (inst.fs.has_value() && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fs, /*is_double=*/true);
          if (*inst.rt == Register::kZero) {
            statement.expression = LiftedExpression::Variable("0.0");
          } else {
            statement.expression = LiftedExpression::Unary(
                "*(f64*)&", LiftedExpression::Variable(RegisterVarName(*inst.rt)));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCfc1:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          int ctrl = static_cast<int>(*inst.fs);
          if (ctrl == 31) {
            statement.expression = LiftedExpression::Variable("fcr31");
          } else {
            statement.expression = LiftedExpression::Integer(0x00000B00, true);
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCtc1:
        if (inst.fs.has_value() && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = "fcr31";
          statement.expression = LiftRegisterOrConstant(*inst.rt, tracker);
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMfc0:
        if (inst.rt.has_value() && *inst.rt != Register::kZero) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression = LiftedExpression::Integer(0);
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMtc0:
        break;

      case Opcode::kAddS:
      case Opcode::kSubS:
      case Opcode::kMulS:
      case Opcode::kDivS:
        if (inst.fd.has_value() && inst.fs.has_value() && inst.ft.has_value()) {
          const char* op = "+";
          if (inst.opcode == Opcode::kSubS) op = "-";
          if (inst.opcode == Opcode::kMulS) op = "*";
          if (inst.opcode == Opcode::kDivS) op = "/";
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          statement.expression =
              LiftedExpression::Binary(op, LiftFpRegister(*inst.fs, /*is_double=*/false),
                                       LiftFpRegister(*inst.ft, /*is_double=*/false));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAddD:
      case Opcode::kSubD:
      case Opcode::kMulD:
      case Opcode::kDivD:
        if (inst.fd.has_value() && inst.fs.has_value() && inst.ft.has_value()) {
          const char* op = "+";
          if (inst.opcode == Opcode::kSubD) op = "-";
          if (inst.opcode == Opcode::kMulD) op = "*";
          if (inst.opcode == Opcode::kDivD) op = "/";
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          statement.expression =
              LiftedExpression::Binary(op, LiftFpRegister(*inst.fs, /*is_double=*/true),
                                       LiftFpRegister(*inst.ft, /*is_double=*/true));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSqrtS:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/false));
          statement.expression = LiftedExpression::Call("N64Sqrtf", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSqrtD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/true));
          statement.expression = LiftedExpression::Call("N64Sqrt", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAbsS:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/false));
          statement.expression = LiftedExpression::Call("fabsf", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAbsD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/true));
          statement.expression = LiftedExpression::Call("fabs", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMovS:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          statement.expression = LiftFpRegister(*inst.fs, /*is_double=*/false);
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMovD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          statement.expression = LiftFpRegister(*inst.fs, /*is_double=*/true);
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kNegS:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          statement.expression =
              LiftedExpression::Unary("-", LiftFpRegister(*inst.fs, /*is_double=*/false));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kNegD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          statement.expression =
              LiftedExpression::Unary("-", LiftFpRegister(*inst.fs, /*is_double=*/true));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtSD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          statement.expression =
              LiftedExpression::Unary("(f32)", LiftFpRegister(*inst.fs, /*is_double=*/true));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtSW:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          int fs_idx = static_cast<int>(*inst.fs);
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          if (pending_int_to_fp[fs_idx] != nullptr) {
            statement.expression =
                LiftedExpression::Unary("(f32)", std::move(pending_int_to_fp[fs_idx]));
          } else {
            statement.expression = LiftedExpression::Unary(
                "(f32)*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtDS:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          statement.expression =
              LiftedExpression::Unary("(f64)", LiftFpRegister(*inst.fs, /*is_double=*/false));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtDW:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          int fs_idx = static_cast<int>(*inst.fs);
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          if (pending_int_to_fp[fs_idx] != nullptr) {
            statement.expression =
                LiftedExpression::Unary("(f64)", std::move(pending_int_to_fp[fs_idx]));
          } else {
            statement.expression = LiftedExpression::Unary(
                "(f64)*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtWS:
      case Opcode::kCvtWD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          int fd_idx = static_cast<int>(*inst.fd);
          int fs_idx = static_cast<int>(*inst.fs);
          bool src_is_double = (inst.opcode == Opcode::kCvtWD);
          bool only_used_by_mfc1_or_swc1 = false;
          for (size_t look = i + 1; look < loop_end; ++look) {
            const Instruction& future_inst = instructions_to_process[look];
            if ((future_inst.opcode == Opcode::kMfc1 && future_inst.fs == *inst.fd &&
                 future_inst.rt.has_value() && *future_inst.rt != Register::kZero) ||
                (future_inst.opcode == Opcode::kSwc1 && future_inst.ft == *inst.fd)) {
              bool reused_after_consumer = false;
              for (size_t k = look + 1; k < loop_end; ++k) {
                RegisterUseDef kud = GetInstructionUseDef(instructions_to_process[k]);
                for (FpRegister u : kud.fpr_uses) {
                  if (u == *inst.fd || (static_cast<int>(u) & ~1) == (fd_idx & ~1)) {
                    reused_after_consumer = true;
                    break;
                  }
                }
                if (reused_after_consumer) break;
                bool redefined = false;
                for (FpRegister d : kud.fpr_defs) {
                  if (d == *inst.fd || (static_cast<int>(d) & ~1) == (fd_idx & ~1)) {
                    redefined = true;
                    break;
                  }
                }
                if (redefined || instructions_to_process[k].IsCall()) break;
              }
              only_used_by_mfc1_or_swc1 = !reused_after_consumer;
              break;
            }
            RegisterUseDef fud = GetInstructionUseDef(future_inst);
            bool touches_fd = false;
            for (FpRegister u : fud.fpr_uses) {
              if (u == *inst.fd || (static_cast<int>(u) & ~1) == (fd_idx & ~1)) {
                touches_fd = true;
              }
            }
            for (FpRegister d : fud.fpr_defs) {
              if (d == *inst.fd || (static_cast<int>(d) & ~1) == (fd_idx & ~1) || d == *inst.fs ||
                  (static_cast<int>(d) & ~1) == (fs_idx & ~1)) {
                touches_fd = true;
              }
            }
            if (touches_fd || future_inst.IsCall()) {
              break;
            }
          }
          if (only_used_by_mfc1_or_swc1) {
            pending_fp_to_int[fd_idx] =
                LiftedExpression::Unary("(s32)", LiftFpRegister(*inst.fs, src_is_double));
            break;
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "s32";
          statement.destination_address = LiftedExpression::Unary(
              "&", LiftedExpression::Variable(FpRegisterVarName(*inst.fd, /*is_double=*/false)));
          statement.expression =
              LiftedExpression::Unary("(s32)", LiftFpRegister(*inst.fs, src_is_double));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtLS:
      case Opcode::kCvtLD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "s64";
          statement.destination_address = LiftedExpression::Unary(
              "&", LiftedExpression::Variable(FpRegisterVarName(*inst.fd, /*is_double=*/true)));
          bool src_is_double = (inst.opcode == Opcode::kCvtLD);
          statement.expression =
              LiftedExpression::Unary("(s64)", LiftFpRegister(*inst.fs, src_is_double));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtSL:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          statement.expression = LiftedExpression::Unary(
              "(f32)*(s64*)&", LiftFpRegister(*inst.fs, /*is_double=*/true));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtDL:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          statement.expression = LiftedExpression::Unary(
              "(f64)*(s64*)&", LiftFpRegister(*inst.fs, /*is_double=*/true));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kTruncWS:
      case Opcode::kTruncWD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          int fd_idx = static_cast<int>(*inst.fd);
          int fs_idx = static_cast<int>(*inst.fs);
          bool src_is_double = (inst.opcode == Opcode::kTruncWD);
          bool only_used_by_mfc1_or_swc1 = false;
          for (size_t look = i + 1; look < loop_end; ++look) {
            const Instruction& future_inst = instructions_to_process[look];
            if ((future_inst.opcode == Opcode::kMfc1 && future_inst.fs == *inst.fd &&
                 future_inst.rt.has_value() && *future_inst.rt != Register::kZero) ||
                (future_inst.opcode == Opcode::kSwc1 && future_inst.ft == *inst.fd)) {
              bool reused_after_consumer = false;
              for (size_t k = look + 1; k < loop_end; ++k) {
                RegisterUseDef kud = GetInstructionUseDef(instructions_to_process[k]);
                for (FpRegister u : kud.fpr_uses) {
                  if (u == *inst.fd || (static_cast<int>(u) & ~1) == (fd_idx & ~1)) {
                    reused_after_consumer = true;
                    break;
                  }
                }
                if (reused_after_consumer) break;
                bool redefined = false;
                for (FpRegister d : kud.fpr_defs) {
                  if (d == *inst.fd || (static_cast<int>(d) & ~1) == (fd_idx & ~1)) {
                    redefined = true;
                    break;
                  }
                }
                if (redefined || instructions_to_process[k].IsCall()) break;
              }
              only_used_by_mfc1_or_swc1 = !reused_after_consumer;
              break;
            }
            RegisterUseDef fud = GetInstructionUseDef(future_inst);
            bool touches_fd = false;
            for (FpRegister u : fud.fpr_uses) {
              if (u == *inst.fd || (static_cast<int>(u) & ~1) == (fd_idx & ~1)) {
                touches_fd = true;
              }
            }
            for (FpRegister d : fud.fpr_defs) {
              if (d == *inst.fd || (static_cast<int>(d) & ~1) == (fd_idx & ~1) || d == *inst.fs ||
                  (static_cast<int>(d) & ~1) == (fs_idx & ~1)) {
                touches_fd = true;
              }
            }
            if (touches_fd || future_inst.IsCall()) {
              break;
            }
          }
          if (only_used_by_mfc1_or_swc1) {
            pending_fp_to_int[fd_idx] =
                LiftedExpression::Unary("(s32)", LiftFpRegister(*inst.fs, src_is_double));
            break;
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "s32";
          statement.destination_address = LiftedExpression::Unary(
              "&", LiftedExpression::Variable(FpRegisterVarName(*inst.fd, /*is_double=*/false)));
          statement.expression =
              LiftedExpression::Unary("(s32)", LiftFpRegister(*inst.fs, src_is_double));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kRoundWS:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/false));
          statement.expression = LiftedExpression::Call("roundf", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kRoundWD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/true));
          statement.expression = LiftedExpression::Call("round", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCeilWS:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/false));
          statement.expression = LiftedExpression::Call("ceilf", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCeilWD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/true));
          statement.expression = LiftedExpression::Call("ceil", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kFloorWS:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/false));
          statement.expression = LiftedExpression::Call("floorf", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kFloorWD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs, /*is_double=*/true));
          statement.expression = LiftedExpression::Call("floor", std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCFS:
      case Opcode::kCSfS:
      case Opcode::kCFD:
      case Opcode::kCSfD: {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = "fcond";
        statement.expression = LiftedExpression::Integer(0);
        statements.push_back(std::move(statement));
        break;
      }

      case Opcode::kCEqS:
      case Opcode::kCSeqS:
      case Opcode::kCUeqS:
      case Opcode::kCNglS:
      case Opcode::kCUnS:
      case Opcode::kCLtS:
      case Opcode::kCOltS:
      case Opcode::kCUltS:
      case Opcode::kCNgeS:
      case Opcode::kCLeS:
      case Opcode::kCOleS:
      case Opcode::kCUleS:
      case Opcode::kCNgtS:
      case Opcode::kCNgleS:
        if (inst.fs.has_value() && inst.ft.has_value()) {
          const char* op = "==";
          if (inst.opcode == Opcode::kCLtS || inst.opcode == Opcode::kCOltS ||
              inst.opcode == Opcode::kCUltS || inst.opcode == Opcode::kCNgeS) {
            op = "<";
          } else if (inst.opcode == Opcode::kCLeS || inst.opcode == Opcode::kCOleS ||
                     inst.opcode == Opcode::kCUleS || inst.opcode == Opcode::kCNgtS ||
                     inst.opcode == Opcode::kCNgleS) {
            op = "<=";
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = "fcond";
          statement.expression =
              LiftedExpression::Binary(op, LiftFpRegister(*inst.fs, /*is_double=*/false),
                                       LiftFpRegister(*inst.ft, /*is_double=*/false));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCEqD:
      case Opcode::kCSeqD:
      case Opcode::kCUeqD:
      case Opcode::kCNglD:
      case Opcode::kCUnD:
      case Opcode::kCLtD:
      case Opcode::kCOltD:
      case Opcode::kCUltD:
      case Opcode::kCNgeD:
      case Opcode::kCLeD:
      case Opcode::kCOleD:
      case Opcode::kCUleD:
      case Opcode::kCNgtD:
      case Opcode::kCNgleD:
        if (inst.fs.has_value() && inst.ft.has_value()) {
          const char* op = "==";
          if (inst.opcode == Opcode::kCLtD || inst.opcode == Opcode::kCOltD ||
              inst.opcode == Opcode::kCUltD || inst.opcode == Opcode::kCNgeD) {
            op = "<";
          } else if (inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kCOleD ||
                     inst.opcode == Opcode::kCUleD || inst.opcode == Opcode::kCNgtD ||
                     inst.opcode == Opcode::kCNgleD) {
            op = "<=";
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = "fcond";
          statement.expression =
              LiftedExpression::Binary(op, LiftFpRegister(*inst.fs, /*is_double=*/true),
                                       LiftFpRegister(*inst.ft, /*is_double=*/true));
          statements.push_back(std::move(statement));
        }
        break;

      default:
        if (inst.opcode != Opcode::kJ && !inst.IsBranch() && inst.opcode != Opcode::kBreak &&
            inst.opcode != Opcode::kSync) {
          LiftedStatement statement;
          statement.kind = StatementKind::kUnhandled;
          statement.expression = LiftedExpression::Variable(
              absl::StrFormat("UNHANDLED_INSTRUCTION: 0x%08X", inst.raw_word));
          statements.push_back(std::move(statement));
        }
        break;
    }

    {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      if (inst.opcode != Opcode::kMtc1 && inst.opcode != Opcode::kLwc1) {
        for (FpRegister d : ud.fpr_defs) {
          int idx = static_cast<int>(d);
          if (idx >= 0 && idx < 32) {
            pending_int_to_fp[idx] = nullptr;
          }
        }
      }
      if (inst.opcode != Opcode::kCvtWS && inst.opcode != Opcode::kCvtWD &&
          inst.opcode != Opcode::kTruncWS && inst.opcode != Opcode::kTruncWD) {
        for (FpRegister d : ud.fpr_defs) {
          int idx = static_cast<int>(d);
          if (idx >= 0 && idx < 32) {
            pending_fp_to_int[idx] = nullptr;
          }
        }
      }
    }

    record_gpr_defs(inst);
    tracker.Step(inst);
  }

  flush_pending_lwl();

  if (pending_swl != nullptr) {
    LiftedStatement statement;
    statement.kind = StatementKind::kStore;
    statement.store_type = "u8";
    Register base_reg = pending_swl->rs;
    if (pending_swl->offset == 0) {
      statement.destination_address = lift_reg(base_reg);
    } else {
      statement.destination_address = LiftedExpression::Binary(
          "+", lift_reg(base_reg), LiftedExpression::Integer(pending_swl->offset));
    }
    statement.expression = lift_reg(pending_swl->rt);
    statements.push_back(std::move(statement));
    pending_swl = nullptr;
  }

  if (pending_return) {
    LiftedStatement ret;
    ret.kind = StatementKind::kReturn;

    int last_def_v0 = -1;
    bool v0_used_after = false;
    for (size_t i = 0; i < instructions.size(); ++i) {
      if (instructions[i].opcode == Opcode::kJal || instructions[i].opcode == Opcode::kJalr) {
        last_def_v0 = -1;
        v0_used_after = false;
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
      for (Register r : ud.gpr_uses) {
        if (r == Register::kV0 && last_def_v0 != -1) v0_used_after = true;
      }
      for (Register r : ud.gpr_defs) {
        if (r == Register::kV0) {
          last_def_v0 = static_cast<int>(i);
          v0_used_after = false;
        }
      }
    }

    int last_def_f0 = -1;
    bool f0_used_after = false;
    for (size_t i = 0; i < instructions.size(); ++i) {
      if (instructions[i].opcode == Opcode::kJal || instructions[i].opcode == Opcode::kJalr) {
        last_def_f0 = -1;
        f0_used_after = false;
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
      for (FpRegister r : ud.fpr_uses) {
        if (r == FpRegister::kF0 && last_def_f0 != -1) f0_used_after = true;
      }
      for (FpRegister r : ud.fpr_defs) {
        if (r == FpRegister::kF0) {
          last_def_f0 = static_cast<int>(i);
          f0_used_after = false;
        }
      }
    }

    bool has_live_f0 = (last_def_f0 != -1 && !f0_used_after);
    bool has_live_v0 = (last_def_v0 != -1 && !v0_used_after);

    auto is_double_op = [](Opcode op) {
      return (op == Opcode::kAddD || op == Opcode::kSubD || op == Opcode::kMulD ||
              op == Opcode::kDivD || op == Opcode::kSqrtD || op == Opcode::kAbsD ||
              op == Opcode::kMovD || op == Opcode::kNegD || op == Opcode::kCvtDS ||
              op == Opcode::kCvtDW || op == Opcode::kLdc1 || op == Opcode::kDmfc1);
    };

    if (has_live_f0 && (!has_live_v0 || last_def_f0 > last_def_v0)) {
      bool is_double = is_double_op(instructions[last_def_f0].opcode);
      ret.expression = LiftedExpression::Variable(is_double ? "f0_d" : "f0");
    } else if (has_live_v0) {
      ret.expression = LiftedExpression::Variable("v0");
    } else if (has_live_f0) {
      bool is_double = is_double_op(instructions[last_def_f0].opcode);
      ret.expression = LiftedExpression::Variable(is_double ? "f0_d" : "f0");
    }
    statements.push_back(std::move(ret));
  }

  return statements;
}

int ExpressionBuilder::DetermineParameterCount(absl::Span<const Instruction> instructions) {
  return AbiAnalyzer::DetermineParameterCount(instructions);
}

int ExpressionBuilder::DetermineFpParameterCount(absl::Span<const Instruction> instructions) {
  return AbiAnalyzer::DetermineFpParameterCount(instructions);
}

bool ExpressionBuilder::DetermineReturnsV0(
    const ControlFlowGraph& cfg, const SymbolIndex* symbol_index,
    const absl::flat_hash_set<std::string>* functions_returning_v0) {
  return AbiAnalyzer::DetermineReturnsV0(cfg, symbol_index, functions_returning_v0);
}

bool ExpressionBuilder::DetermineReturnsF0(const ControlFlowGraph& cfg) {
  return AbiAnalyzer::DetermineReturnsF0(cfg);
}

bool ExpressionBuilder::DetermineReturnsF0Double(const ControlFlowGraph& cfg) {
  return AbiAnalyzer::DetermineReturnsF0Double(cfg);
}

absl::flat_hash_set<Register> ExpressionBuilder::DetectUninitializedCalleeSavedRegisters(
    const ControlFlowGraph& cfg) {
  return AbiAnalyzer::DetectUninitializedCalleeSavedRegisters(cfg);
}

absl::flat_hash_map<uint32_t, CallReturnInfo> ExpressionBuilder::ComputeCallReturnInfo(
    const ControlFlowGraph& cfg, bool function_returns_v0, bool function_returns_f0) {
  return AbiAnalyzer::ComputeCallReturnInfo(cfg, function_returns_v0, function_returns_f0);
}

std::optional<ExpressionBuilder::FoldedComparison> ExpressionBuilder::FindFoldedComparison(
    absl::Span<const Instruction> instructions) {
  return BranchConditionBuilder::FindFoldedComparison(instructions);
}

}  // namespace rom_nom_nom
