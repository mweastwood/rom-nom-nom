#include "lifter/expression_builder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "lifter/control_flow_graph.h"
#include "lifter/register_tracker.h"
#include "lifter/symbol_folder.h"
#include "splitter/symbol_registry.h"

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
      if (is_hex && int_val > 9) {
        return absl::StrFormat("0x%X", int_val);
      }
      return std::to_string(int_val);
    case ExpressionKind::kVariable:
    case ExpressionKind::kGlobalRef:
      return name;
    case ExpressionKind::kUnaryOp:
      return op + (args.empty() ? "" : args[0]->ToString());
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
  }
  return "";
}

std::string ExpressionBuilder::RegisterVarName(Register reg) {
  switch (reg) {
    case Register::kZero:
      return "0";
    case Register::kV0:
      return "v0";
    case Register::kV1:
      return "v1";
    case Register::kA0:
      return "arg0";
    case Register::kA1:
      return "arg1";
    case Register::kA2:
      return "arg2";
    case Register::kA3:
      return "arg3";
    case Register::kSp:
      return "sp";
    case Register::kFp:
      return "fp";
    case Register::kRa:
      return "ra";
    default: {
      std::string_view name = RegisterName(reg);
      if (!name.empty() && name[0] == '$') {
        return std::string("temp_") + std::string(name.substr(1));
      }
      return std::string(name);
    }
  }
}

namespace {

std::string StackVarName(int32_t offset) {
  if (offset < 0) {
    return absl::StrFormat("var_sp_neg_%d", -offset);
  }
  return absl::StrFormat("var_sp_%d", offset);
}

}  // namespace

std::unique_ptr<LiftedExpression> ExpressionBuilder::LiftRegisterOrConstant(
    Register reg, const RegisterTracker& tracker) {
  if (reg == Register::kZero) {
    return LiftedExpression::Integer(0);
  }
  if (tracker.IsConstant(reg)) {
    auto c = tracker.GetConstant(reg);
    if (c.has_value()) {
      bool hex = (*c > 0xFFFF);
      return LiftedExpression::Integer(*c, hex);
    }
  }
  return LiftedExpression::Variable(RegisterVarName(reg));
}

std::vector<LiftedStatement> ExpressionBuilder::LiftBlock(
    const BasicBlock& block, const SymbolIndex* symbol_index, const SplitConfig* split_config,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts) {
  return LiftInstructions(block.instructions, symbol_index, split_config,
                          function_parameter_counts);
}

std::vector<LiftedStatement> ExpressionBuilder::LiftInstructions(
    absl::Span<const Instruction> instructions, const SymbolIndex* symbol_index,
    const SplitConfig* split_config,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts) {
  std::vector<LiftedStatement> statements;
  if (instructions.empty()) {
    return statements;
  }

  std::vector<Instruction> reordered_storage;
  absl::Span<const Instruction> instructions_to_process = instructions;
  bool has_jal_instruction = false;
  for (const auto& instruction : instructions) {
    if (instruction.opcode == Opcode::kJal) {
      has_jal_instruction = true;
      break;
    }
  }
  if (has_jal_instruction) {
    reordered_storage.reserve(instructions.size());
    for (size_t instruction_index = 0; instruction_index < instructions.size();
         ++instruction_index) {
      if (instructions[instruction_index].opcode == Opcode::kJal &&
          instruction_index + 1 < instructions.size()) {
        // In MIPS, the delay slot instruction executes before the function call branch takes
        // effect.
        reordered_storage.push_back(instructions[instruction_index + 1]);
        reordered_storage.push_back(instructions[instruction_index]);
        ++instruction_index;
      } else {
        reordered_storage.push_back(instructions[instruction_index]);
      }
    }
    instructions_to_process = reordered_storage;
  }

  SymbolFolder folder;
  folder.Fold(instructions_to_process, symbol_index, split_config);

  RegisterTracker tracker;
  bool pending_return = false;

  for (size_t i = 0; i < instructions_to_process.size(); ++i) {
    const auto& inst = instructions_to_process[i];

    // If this instruction is the high half of a folded pair, skip emitting it
    if (folder.IsFoldedHi(i)) {
      tracker.Analyze(instructions_to_process.subspan(i, 1));
      continue;
    }

    // If this instruction is the low half of a folded pair, emit high-level folded access
    if (const auto* lo = folder.GetFoldedLo(i)) {
      LiftedStatement statement;
      if (lo->type == FoldedPatternType::kAddressLoad) {
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(lo->dest_reg);
        statement.expression = lo->symbol_name.empty()
                                   ? LiftedExpression::Integer(lo->address, true)
                                   : LiftedExpression::GlobalRef("&" + lo->symbol_name);
        statements.push_back(std::move(statement));
      } else if (lo->type == FoldedPatternType::kConstantLiteral) {
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(lo->dest_reg);
        statement.expression = LiftedExpression::Integer(lo->address, true);
        statements.push_back(std::move(statement));
      } else if (lo->type == FoldedPatternType::kGlobalLoad) {
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(lo->dest_reg);
        statement.expression =
            lo->symbol_name.empty()
                ? LiftedExpression::Load("u32", LiftedExpression::Integer(lo->address, true))
                : LiftedExpression::GlobalRef(lo->symbol_name);
        statements.push_back(std::move(statement));
      } else if (lo->type == FoldedPatternType::kGlobalStore) {
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            lo->symbol_name.empty() ? absl::StrFormat("*(0x%X)", lo->address) : lo->symbol_name;
        statement.expression = LiftedExpression::Variable(RegisterVarName(lo->src_reg));
        statements.push_back(std::move(statement));
      }
      tracker.Analyze(instructions_to_process.subspan(i, 1));
      continue;
    }

    // Skip NOPs
    if (inst.IsNop()) {
      tracker.Analyze(instructions_to_process.subspan(i, 1));
      continue;
    }

    switch (inst.opcode) {
      case Opcode::kAddu:
      case Opcode::kAdd:
        if (inst.rd.has_value() && *inst.rd != Register::kZero) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (inst.rs == Register::kZero && inst.rt.has_value()) {
            statement.expression = LiftRegisterOrConstant(*inst.rt, tracker);
          } else if (inst.rt == Register::kZero && inst.rs.has_value()) {
            statement.expression = LiftRegisterOrConstant(*inst.rs, tracker);
          } else if (inst.rs.has_value() && inst.rt.has_value()) {
            statement.expression =
                LiftedExpression::Binary("+", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftRegisterOrConstant(*inst.rt, tracker));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSubu:
      case Opcode::kSub:
        if (inst.rd.has_value() && *inst.rd != Register::kZero) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (inst.rs == Register::kZero && inst.rt.has_value()) {
            statement.expression =
                LiftedExpression::Unary("-", LiftRegisterOrConstant(*inst.rt, tracker));
          } else if (inst.rs.has_value() && inst.rt.has_value()) {
            statement.expression =
                LiftedExpression::Binary("-", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftRegisterOrConstant(*inst.rt, tracker));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAddiu:
      case Opcode::kAddi:
        if (inst.rt.has_value() && *inst.rt != Register::kZero) {
          if (*inst.rt == Register::kSp) {
            // Stack adjustment; handled in stack frame analysis
            break;
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (inst.rs == Register::kZero) {
            statement.expression = LiftedExpression::Integer(inst.immediate);
          } else if (inst.rs == Register::kSp) {
            statement.expression = LiftedExpression::Unary(
                "&", LiftedExpression::Variable(StackVarName(inst.immediate)));
          } else if (inst.immediate == 0 && inst.rs.has_value()) {
            statement.expression = LiftRegisterOrConstant(*inst.rs, tracker);
          } else if (inst.immediate < 0 && inst.rs.has_value()) {
            statement.expression =
                LiftedExpression::Binary("-", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftedExpression::Integer(-inst.immediate));
          } else if (inst.rs.has_value()) {
            statement.expression =
                LiftedExpression::Binary("+", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftedExpression::Integer(inst.immediate));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAnd:
      case Opcode::kAndi:
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

      case Opcode::kOr:
      case Opcode::kOri:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (inst.opcode == Opcode::kOri) {
            statement.expression =
                LiftedExpression::Binary("|", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftedExpression::Integer(inst.immediate));
          } else if (inst.rd.has_value() && *inst.rd != Register::kZero) {
            statement.destination_variable = RegisterVarName(*inst.rd);
            statement.expression =
                LiftedExpression::Binary("|", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftRegisterOrConstant(*inst.rt, tracker));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kXor:
      case Opcode::kXori:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (inst.opcode == Opcode::kXori) {
            statement.expression =
                LiftedExpression::Binary("^", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftedExpression::Integer(inst.immediate));
          } else if (inst.rd.has_value() && *inst.rd != Register::kZero) {
            statement.destination_variable = RegisterVarName(*inst.rd);
            statement.expression =
                LiftedExpression::Binary("^", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftRegisterOrConstant(*inst.rt, tracker));
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

      case Opcode::kLw:
      case Opcode::kLh:
      case Opcode::kLb:
      case Opcode::kLhu:
      case Opcode::kLbu:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          const char* load_type = "s32";
          if (inst.opcode == Opcode::kLh) {
            load_type = "s16";
          } else if (inst.opcode == Opcode::kLhu) {
            load_type = "u16";
          } else if (inst.opcode == Opcode::kLb) {
            load_type = "s8";
          } else if (inst.opcode == Opcode::kLbu) {
            load_type = "u8";
          }

          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (*inst.rs == Register::kSp) {
            statement.expression = LiftedExpression::Variable(StackVarName(inst.immediate));
          } else if (inst.immediate == 0) {
            statement.expression =
                LiftedExpression::Load(load_type, LiftRegisterOrConstant(*inst.rs, tracker));
          } else {
            statement.expression = LiftedExpression::Load(
                load_type, LiftedExpression::Binary("+", LiftRegisterOrConstant(*inst.rs, tracker),
                                                    LiftedExpression::Integer(inst.immediate)));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSw:
      case Opcode::kSh:
      case Opcode::kSb:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          if (*inst.rs == Register::kSp && *inst.rt == Register::kRa) {
            // Function prologue saving $ra, omit from C body
            break;
          }
          const char* store_type = "s32";
          if (inst.opcode == Opcode::kSh) {
            store_type = "s16";
          } else if (inst.opcode == Opcode::kSb) {
            store_type = "s8";
          }

          LiftedStatement statement;
          if (*inst.rs == Register::kSp) {
            statement.kind = StatementKind::kAssignment;
            statement.destination_variable = StackVarName(inst.immediate);
            statement.expression = LiftRegisterOrConstant(*inst.rt, tracker);
          } else {
            statement.kind = StatementKind::kStore;
            statement.store_type = store_type;
            if (inst.immediate == 0) {
              statement.destination_address = LiftRegisterOrConstant(*inst.rs, tracker);
            } else {
              statement.destination_address =
                  LiftedExpression::Binary("+", LiftRegisterOrConstant(*inst.rs, tracker),
                                           LiftedExpression::Integer(inst.immediate));
            }
            statement.expression = LiftRegisterOrConstant(*inst.rt, tracker);
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kJal: {
        uint32_t target_vram = inst.JumpTarget();
        std::string function_name =
            (symbol_index != nullptr)
                ? symbol_index->LookupOrSynthesizeName(target_vram, SYMBOL_FUNC)
                : absl::StrFormat("func_%08X", target_vram);

        Register argument_registers[] = {Register::kA0, Register::kA1, Register::kA2,
                                         Register::kA3};
        int highest_defined_argument_index = -1;
        for (int arg_index = 3; arg_index >= 0; --arg_index) {
          if (tracker.GetReachingDefinition(argument_registers[arg_index]).has_value()) {
            highest_defined_argument_index = arg_index;
            break;
          }
        }

        int argument_count = 0;
        if (function_parameter_counts != nullptr) {
          auto it = function_parameter_counts->find(function_name);
          if (it != function_parameter_counts->end()) {
            argument_count = it->second;
          } else {
            argument_count = highest_defined_argument_index + 1;
          }
        } else {
          argument_count = highest_defined_argument_index + 1;
        }

        std::vector<std::unique_ptr<LiftedExpression>> call_arguments;
        call_arguments.reserve(argument_count);
        for (int arg_index = 0; arg_index < argument_count; ++arg_index) {
          if (arg_index < 4 &&
              tracker.GetReachingDefinition(argument_registers[arg_index]).has_value()) {
            call_arguments.push_back(
                LiftRegisterOrConstant(argument_registers[arg_index], tracker));
          } else {
            call_arguments.push_back(
                LiftedExpression::Variable(absl::StrFormat("arg%d", arg_index)));
          }
        }

        LiftedStatement statement;
        statement.kind = StatementKind::kCall;
        statement.expression = LiftedExpression::Call(function_name, std::move(call_arguments));
        statements.push_back(std::move(statement));
        break;
      }

      case Opcode::kJr:
        if (inst.rs == Register::kRa) {
          pending_return = true;
        }
        break;

      default:
        break;
    }

    tracker.Analyze(instructions_to_process.subspan(i, 1));
  }

  if (pending_return) {
    LiftedStatement ret;
    ret.kind = StatementKind::kReturn;
    auto v0_def = tracker.GetReachingDefinition(Register::kV0);
    if (v0_def.has_value()) {
      ret.expression = LiftedExpression::Variable("v0");
    }
    statements.push_back(std::move(ret));
  }

  return statements;
}

int ExpressionBuilder::DetermineParameterCount(absl::Span<const Instruction> instructions) {
  absl::flat_hash_set<Register> defined_registers;
  absl::flat_hash_set<Register> used_before_definition;

  for (const auto& instruction : instructions) {
    RegisterUseDef use_def = GetInstructionUseDef(instruction);
    for (Register used_register : use_def.gpr_uses) {
      if (!defined_registers.contains(used_register)) {
        used_before_definition.insert(used_register);
      }
    }
    for (Register defined_register : use_def.gpr_defs) {
      defined_registers.insert(defined_register);
    }
    if (instruction.opcode == Opcode::kJal) {
      // Subroutine calls may clobber argument registers; from caller perspective they are redefined
      defined_registers.insert(Register::kA0);
      defined_registers.insert(Register::kA1);
      defined_registers.insert(Register::kA2);
      defined_registers.insert(Register::kA3);
    }
  }

  if (used_before_definition.contains(Register::kA3)) {
    return 4;
  }
  if (used_before_definition.contains(Register::kA2)) {
    return 3;
  }
  if (used_before_definition.contains(Register::kA1)) {
    return 2;
  }
  if (used_before_definition.contains(Register::kA0)) {
    return 1;
  }
  return 0;
}

}  // namespace rom_nom_nom
