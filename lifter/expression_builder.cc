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
#include "lifter/division_pattern.h"
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

std::string ExpressionBuilder::FpRegisterVarName(FpRegister reg) {
  std::string_view name = FpRegisterName(reg);
  if (!name.empty() && name[0] == '$') {
    return std::string(name.substr(1));
  }
  return std::string(name);
}

namespace {

std::unique_ptr<LiftedExpression> LiftFpRegister(FpRegister reg) {
  return LiftedExpression::Variable(ExpressionBuilder::FpRegisterVarName(reg));
}

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
  if (reg == Register::kSp) {
    return LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp"));
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
    const absl::flat_hash_map<std::string, int>* function_parameter_counts,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts) {
  return LiftInstructions(block.instructions, symbol_index, split_config, function_parameter_counts,
                          function_fp_parameter_counts);
}

std::vector<LiftedStatement> ExpressionBuilder::LiftInstructions(
    absl::Span<const Instruction> instructions, const SymbolIndex* symbol_index,
    const SplitConfig* split_config,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts) {
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

  auto folded_comp = FindFoldedComparison(instructions_to_process);

  for (size_t i = 0; i < instructions_to_process.size(); ++i) {
    const auto& inst = instructions_to_process[i];

    // Skip emitting assignment for folded comparison instructions that are directly
    // integrated into subsequent branch conditions.
    if (folded_comp.has_value() && folded_comp->can_suppress_statement &&
        i == folded_comp->instruction_index) {
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
        statement.expression =
            LiftedExpression::Binary("/", LiftRegisterOrConstant(div_pattern->num_reg, tracker),
                                     LiftRegisterOrConstant(div_pattern->den_reg, tracker));
        statements.push_back(std::move(statement));
      }
      if (div_pattern->mod_dest_reg.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(*div_pattern->mod_dest_reg);
        statement.expression =
            LiftedExpression::Binary("%", LiftRegisterOrConstant(div_pattern->num_reg, tracker),
                                     LiftRegisterOrConstant(div_pattern->den_reg, tracker));
        statements.push_back(std::move(statement));
      }
      for (size_t k = i; k <= div_pattern->end_index; ++k) {
        tracker.Step(instructions_to_process[k]);
      }
      i = div_pattern->end_index;
      continue;
    }

    // If this instruction is the high half of a folded pair, skip emitting it
    if (folder.IsFoldedHi(i)) {
      tracker.Step(inst);
      continue;
    }

    // If this instruction is the low half of a folded pair, emit high-level folded access
    if (const auto* lo = folder.GetFoldedLo(i)) {
      LiftedStatement statement;
      if (lo->type == FoldedPatternType::kAddressLoad) {
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(lo->dest_reg);
        statement.expression =
            lo->symbol_name.empty()
                ? LiftedExpression::Integer(lo->address, true)
                : LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo->symbol_name));
        statements.push_back(std::move(statement));
      } else if (lo->type == FoldedPatternType::kConstantLiteral) {
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(lo->dest_reg);
        statement.expression = LiftedExpression::Integer(lo->address, true);
        statements.push_back(std::move(statement));
      } else if (lo->type == FoldedPatternType::kGlobalLoad) {
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = RegisterVarName(lo->dest_reg);
        if (lo->symbol_name.empty()) {
          statement.expression =
              LiftedExpression::Load(lo->access_type, LiftedExpression::Integer(lo->address, true));
        } else if (lo->access_type != "s32") {
          statement.expression = LiftedExpression::Load(
              lo->access_type,
              LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo->symbol_name)));
        } else {
          statement.expression = LiftedExpression::GlobalRef(lo->symbol_name);
        }
        statements.push_back(std::move(statement));
      } else if (lo->type == FoldedPatternType::kGlobalStore) {
        if (lo->access_type != "s32") {
          statement.kind = StatementKind::kStore;
          statement.store_type = lo->access_type;
          statement.destination_address =
              lo->symbol_name.empty()
                  ? LiftedExpression::Integer(lo->address, true)
                  : LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo->symbol_name));
          statement.expression = LiftRegisterOrConstant(lo->src_reg, tracker);
        } else {
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable =
              lo->symbol_name.empty() ? absl::StrFormat("*(0x%X)", lo->address) : lo->symbol_name;
          statement.expression = LiftRegisterOrConstant(lo->src_reg, tracker);
        }
        statements.push_back(std::move(statement));
      }
      tracker.Step(inst);
      continue;
    }

    // Skip NOPs
    if (inst.IsNop()) {
      tracker.Step(inst);
      continue;
    }

    switch (inst.opcode) {
      case Opcode::kAddu:
      case Opcode::kAdd:
        if (inst.rd.has_value() && *inst.rd != Register::kZero && *inst.rd != Register::kSp) {
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
        if (inst.rd.has_value() && *inst.rd != Register::kZero && *inst.rd != Register::kSp) {
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

      case Opcode::kLui:
        if (inst.rt.has_value() && *inst.rt != Register::kZero) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(inst.immediate)) << 16;
          statement.expression = LiftedExpression::Integer(val, /*hex=*/true);
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
      case Opcode::kOri:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (inst.opcode == Opcode::kOri) {
            statement.expression =
                LiftedExpression::Binary("|", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftedExpression::Integer(inst.UnsignedImmediate()));
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
                                         LiftedExpression::Integer(inst.UnsignedImmediate()));
          } else if (inst.rd.has_value() && *inst.rd != Register::kZero) {
            statement.destination_variable = RegisterVarName(*inst.rd);
            statement.expression =
                LiftedExpression::Binary("^", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftRegisterOrConstant(*inst.rt, tracker));
          }
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

      case Opcode::kSlt:
      case Opcode::kSltu:
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

      case Opcode::kSlti:
      case Opcode::kSltiu:
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

      case Opcode::kMult:
      case Opcode::kMultu:
        if (inst.rs.has_value() && inst.rt.has_value()) {
          pending_mult = std::make_unique<PendingMult>();
          pending_mult->is_unsigned = (inst.opcode == Opcode::kMultu);
          pending_mult->rs = *inst.rs;
          pending_mult->rt = *inst.rt;
        }
        break;

      case Opcode::kMflo:
        if (inst.rd.has_value() && *inst.rd != Register::kZero) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (pending_mult != nullptr) {
            statement.expression =
                LiftedExpression::Binary("*", LiftRegisterOrConstant(pending_mult->rs, tracker),
                                         LiftRegisterOrConstant(pending_mult->rt, tracker));
            pending_mult = nullptr;
          } else {
            statement.expression = LiftedExpression::Variable("lo");
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMfhi:
        if (inst.rd.has_value() && *inst.rd != Register::kZero) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rd);
          if (pending_mult != nullptr) {
            std::string cast_type = pending_mult->is_unsigned ? "u64" : "s64";
            auto lhs = LiftRegisterOrConstant(pending_mult->rs, tracker);
            auto rhs = LiftRegisterOrConstant(pending_mult->rt, tracker);
            statement.expression = LiftedExpression::Binary(
                ">>",
                LiftedExpression::Binary(
                    "*", LiftedExpression::Unary("(" + cast_type + ")", std::move(lhs)),
                    LiftedExpression::Unary("(" + cast_type + ")", std::move(rhs))),
                LiftedExpression::Integer(32));
            pending_mult = nullptr;
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

      case Opcode::kLw:
      case Opcode::kLh:
      case Opcode::kLb:
      case Opcode::kLhu:
      case Opcode::kLbu:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && *inst.rt != Register::kSp &&
            inst.rs.has_value()) {
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

      case Opcode::kLwl:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && *inst.rt != Register::kSp &&
            inst.rs.has_value()) {
          pending_lwl = std::make_unique<PendingUnalignedAccess>();
          pending_lwl->rt = *inst.rt;
          pending_lwl->rs = *inst.rs;
          pending_lwl->offset = inst.immediate;
        }
        break;

      case Opcode::kLwr:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && *inst.rt != Register::kSp &&
            inst.rs.has_value()) {
          int16_t offset = inst.immediate;
          Register base_reg = *inst.rs;
          if (pending_lwl != nullptr && pending_lwl->rt == *inst.rt &&
              pending_lwl->rs == *inst.rs) {
            offset = pending_lwl->offset;
            pending_lwl = nullptr;
          }
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          if (base_reg == Register::kSp) {
            statement.expression = LiftedExpression::Variable(StackVarName(offset));
          } else if (offset == 0) {
            statement.expression =
                LiftedExpression::Load("s32", LiftRegisterOrConstant(base_reg, tracker));
          } else {
            statement.expression = LiftedExpression::Load(
                "s32", LiftedExpression::Binary("+", LiftRegisterOrConstant(base_reg, tracker),
                                                LiftedExpression::Integer(offset)));
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

      case Opcode::kSwl:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          pending_swl = std::make_unique<PendingUnalignedAccess>();
          pending_swl->rt = *inst.rt;
          pending_swl->rs = *inst.rs;
          pending_swl->offset = inst.immediate;
        }
        break;

      case Opcode::kSwr:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          int16_t offset = inst.immediate;
          Register base_reg = *inst.rs;
          if (pending_swl != nullptr && pending_swl->rt == *inst.rt &&
              pending_swl->rs == *inst.rs) {
            offset = pending_swl->offset;
            pending_swl = nullptr;
          }
          LiftedStatement statement;
          if (base_reg == Register::kSp) {
            statement.kind = StatementKind::kAssignment;
            statement.destination_variable = StackVarName(offset);
            statement.expression = LiftRegisterOrConstant(*inst.rt, tracker);
          } else {
            statement.kind = StatementKind::kStore;
            statement.store_type = "s32";
            if (offset == 0) {
              statement.destination_address = LiftRegisterOrConstant(base_reg, tracker);
            } else {
              statement.destination_address =
                  LiftedExpression::Binary("+", LiftRegisterOrConstant(base_reg, tracker),
                                           LiftedExpression::Integer(offset));
            }
            statement.expression = LiftRegisterOrConstant(*inst.rt, tracker);
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kJal:
      case Opcode::kJalr: {
        std::string function_name;
        if (inst.opcode == Opcode::kJalr) {
          function_name =
              absl::StrFormat("((void (*)())%s)", RegisterVarName(inst.rs.value_or(Register::kT9)));
        } else {
          uint32_t target_vram = inst.JumpTarget();
          function_name = (symbol_index != nullptr)
                              ? symbol_index->LookupOrSynthesizeName(target_vram, SYMBOL_FUNC)
                              : absl::StrFormat("func_%08X", target_vram);
        }

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

        if (call_arguments.empty()) {
          bool known_fp_count = false;
          int fp_arg_count = 0;
          if (function_fp_parameter_counts != nullptr) {
            auto it = function_fp_parameter_counts->find(function_name);
            if (it != function_fp_parameter_counts->end()) {
              known_fp_count = true;
              fp_arg_count = it->second;
            }
          }
          if (known_fp_count) {
            if (fp_arg_count >= 1) {
              call_arguments.push_back(LiftedExpression::Variable("f12"));
            }
            if (fp_arg_count >= 2) {
              call_arguments.push_back(LiftedExpression::Variable("f14"));
            }
          } else {
            bool defines_f12 = false;
            bool defines_f14 = false;
            for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
              RegisterUseDef ud = GetInstructionUseDef(instructions_to_process[j]);
              for (FpRegister r : ud.fpr_defs) {
                if (r == FpRegister::kF12) defines_f12 = true;
                if (r == FpRegister::kF14) defines_f14 = true;
              }
              if (instructions_to_process[j].opcode == Opcode::kJal ||
                  instructions_to_process[j].opcode == Opcode::kJalr) {
                break;
              }
            }
            if (defines_f12 || function_name == "sqrtf" || function_name == "fabsf" ||
                function_name == "sqrt" || function_name == "fabs") {
              call_arguments.push_back(LiftedExpression::Variable("f12"));
              if (defines_f14) {
                call_arguments.push_back(LiftedExpression::Variable("f14"));
              }
            }
          }
        }

        LiftedStatement statement;
        if (function_name == "sqrtf" || function_name == "fabsf" || function_name == "sqrt" ||
            function_name == "fabs") {
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = "f0";
        } else {
          statement.kind = StatementKind::kCall;
        }
        statement.expression = LiftedExpression::Call(function_name, std::move(call_arguments));
        statements.push_back(std::move(statement));
        break;
      }

      case Opcode::kJr:
        if (inst.rs == Register::kRa) {
          pending_return = true;
        }
        break;

      case Opcode::kLwc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.ft);
          if (*inst.rs == Register::kSp) {
            statement.expression = LiftedExpression::Load(
                "f32", LiftedExpression::Unary(
                           "&", LiftedExpression::Variable(StackVarName(inst.immediate))));
          } else if (inst.immediate == 0) {
            statement.expression =
                LiftedExpression::Load("f32", LiftRegisterOrConstant(*inst.rs, tracker));
          } else {
            statement.expression = LiftedExpression::Load(
                "f32", LiftedExpression::Binary("+", LiftRegisterOrConstant(*inst.rs, tracker),
                                                LiftedExpression::Integer(inst.immediate)));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSwc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "f32";
          if (*inst.rs == Register::kSp) {
            statement.destination_address = LiftedExpression::Unary(
                "&", LiftedExpression::Variable(StackVarName(inst.immediate)));
          } else if (inst.immediate == 0) {
            statement.destination_address = LiftRegisterOrConstant(*inst.rs, tracker);
          } else {
            statement.destination_address =
                LiftedExpression::Binary("+", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftedExpression::Integer(inst.immediate));
          }
          statement.expression = LiftFpRegister(*inst.ft);
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kLdc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.ft);
          if (*inst.rs == Register::kSp) {
            statement.expression = LiftedExpression::Load(
                "f64", LiftedExpression::Unary(
                           "&", LiftedExpression::Variable(StackVarName(inst.immediate))));
          } else if (inst.immediate == 0) {
            statement.expression =
                LiftedExpression::Load("f64", LiftRegisterOrConstant(*inst.rs, tracker));
          } else {
            statement.expression = LiftedExpression::Load(
                "f64", LiftedExpression::Binary("+", LiftRegisterOrConstant(*inst.rs, tracker),
                                                LiftedExpression::Integer(inst.immediate)));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSdc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "f64";
          if (*inst.rs == Register::kSp) {
            statement.destination_address = LiftedExpression::Unary(
                "&", LiftedExpression::Variable(StackVarName(inst.immediate)));
          } else if (inst.immediate == 0) {
            statement.destination_address = LiftRegisterOrConstant(*inst.rs, tracker);
          } else {
            statement.destination_address =
                LiftedExpression::Binary("+", LiftRegisterOrConstant(*inst.rs, tracker),
                                         LiftedExpression::Integer(inst.immediate));
          }
          statement.expression = LiftFpRegister(*inst.ft);
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMfc1:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression = LiftedExpression::Unary("*(s32*)&", LiftFpRegister(*inst.fs));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDmfc1:
        if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression = LiftedExpression::Unary("*(s64*)&", LiftFpRegister(*inst.fs));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMtc1:
        if (inst.fs.has_value() && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fs);
          if (*inst.rt == Register::kZero) {
            statement.expression = LiftedExpression::Variable("0.0f");
          } else {
            statement.expression = LiftedExpression::Unary(
                "*(f32*)&", LiftedExpression::Variable(RegisterVarName(*inst.rt)));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kDmtc1:
        if (inst.fs.has_value() && inst.rt.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fs);
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

      case Opcode::kAddS:
      case Opcode::kSubS:
      case Opcode::kMulS:
      case Opcode::kDivS:
      case Opcode::kAddD:
      case Opcode::kSubD:
      case Opcode::kMulD:
      case Opcode::kDivD:
        if (inst.fd.has_value() && inst.fs.has_value() && inst.ft.has_value()) {
          const char* op = "+";
          if (inst.opcode == Opcode::kSubS || inst.opcode == Opcode::kSubD) op = "-";
          if (inst.opcode == Opcode::kMulS || inst.opcode == Opcode::kMulD) op = "*";
          if (inst.opcode == Opcode::kDivS || inst.opcode == Opcode::kDivD) op = "/";
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd);
          statement.expression =
              LiftedExpression::Binary(op, LiftFpRegister(*inst.fs), LiftFpRegister(*inst.ft));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSqrtS:
      case Opcode::kSqrtD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd);
          std::string fn = (inst.opcode == Opcode::kSqrtS) ? "sqrtf" : "sqrt";
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs));
          statement.expression = LiftedExpression::Call(fn, std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kAbsS:
      case Opcode::kAbsD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd);
          std::string fn = (inst.opcode == Opcode::kAbsS) ? "fabsf" : "fabs";
          std::vector<std::unique_ptr<LiftedExpression>> args;
          args.push_back(LiftFpRegister(*inst.fs));
          statement.expression = LiftedExpression::Call(fn, std::move(args));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kMovS:
      case Opcode::kMovD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd);
          statement.expression = LiftFpRegister(*inst.fs);
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kNegS:
      case Opcode::kNegD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd);
          statement.expression = LiftedExpression::Unary("-", LiftFpRegister(*inst.fs));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtSD:
      case Opcode::kCvtSW:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd);
          statement.expression = LiftedExpression::Unary("(f32)", LiftFpRegister(*inst.fs));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtDS:
      case Opcode::kCvtDW:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd);
          statement.expression = LiftedExpression::Unary("(f64)", LiftFpRegister(*inst.fs));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kTruncWS:
      case Opcode::kTruncWD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd);
          statement.expression = LiftedExpression::Unary("(s32)", LiftFpRegister(*inst.fs));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCEqS:
      case Opcode::kCEqD:
      case Opcode::kCLtS:
      case Opcode::kCLtD:
      case Opcode::kCLeS:
      case Opcode::kCLeD:
        if (inst.fs.has_value() && inst.ft.has_value()) {
          const char* op = "==";
          if (inst.opcode == Opcode::kCLtS || inst.opcode == Opcode::kCLtD) op = "<";
          if (inst.opcode == Opcode::kCLeS || inst.opcode == Opcode::kCLeD) op = "<=";
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = "fcond";
          statement.expression =
              LiftedExpression::Binary(op, LiftFpRegister(*inst.fs), LiftFpRegister(*inst.ft));
          statements.push_back(std::move(statement));
        }
        break;

      default:
        break;
    }

    tracker.Step(inst);
  }

  if (pending_return) {
    LiftedStatement ret;
    ret.kind = StatementKind::kReturn;
    auto v0_def = tracker.GetReachingDefinition(Register::kV0);
    if (v0_def.has_value()) {
      ret.expression = LiftedExpression::Variable("v0");
    } else {
      bool defines_f0 = false;
      for (int i = static_cast<int>(instructions.size()) - 1; i >= 0; --i) {
        RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
        for (FpRegister r : ud.fpr_defs) {
          if (r == FpRegister::kF0) {
            defines_f0 = true;
            break;
          }
        }
        if (defines_f0) break;
      }
      if (defines_f0) {
        ret.expression = LiftedExpression::Variable("f0");
      }
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
    if (instruction.opcode == Opcode::kJal || instruction.opcode == Opcode::kJalr) {
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

int ExpressionBuilder::DetermineFpParameterCount(absl::Span<const Instruction> instructions) {
  absl::flat_hash_set<FpRegister> defined_fprs;
  absl::flat_hash_set<FpRegister> used_fprs_before_def;

  for (const auto& instruction : instructions) {
    RegisterUseDef use_def = GetInstructionUseDef(instruction);
    for (FpRegister used_reg : use_def.fpr_uses) {
      if (!defined_fprs.contains(used_reg)) {
        used_fprs_before_def.insert(used_reg);
      }
    }
    for (FpRegister def_reg : use_def.fpr_defs) {
      defined_fprs.insert(def_reg);
    }
    if (instruction.opcode == Opcode::kJal || instruction.opcode == Opcode::kJalr) {
      defined_fprs.insert(FpRegister::kF12);
      defined_fprs.insert(FpRegister::kF14);
    }
  }

  if (used_fprs_before_def.contains(FpRegister::kF12)) {
    if (used_fprs_before_def.contains(FpRegister::kF14)) {
      return 2;
    }
    return 1;
  }
  return 0;
}

bool ExpressionBuilder::DetermineReturnsV0(const ControlFlowGraph& cfg) {
  if (cfg.Blocks().empty()) {
    return false;
  }

  // Build a lookup map from block ID to block index
  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  // Pre-calculate per-block properties for $v0
  struct BlockV0Props {
    bool has_return = false;
    bool defines_v0 = false;
    bool v0_used_after_last_def = false;
    bool uses_v0_before_def = false;
  };

  std::vector<BlockV0Props> block_props(cfg.Blocks().size());

  for (size_t b_idx = 0; b_idx < cfg.Blocks().size(); ++b_idx) {
    const auto& block = cfg.Blocks()[b_idx];
    auto& props = block_props[b_idx];
    props.has_return = block.HasReturn();

    int last_def_idx = -1;
    for (size_t i = 0; i < block.instructions.size(); ++i) {
      RegisterUseDef ud = GetInstructionUseDef(block.instructions[i]);
      bool uses_v0 = false;
      for (Register r : ud.gpr_uses) {
        if (r == Register::kV0) {
          uses_v0 = true;
          break;
        }
      }
      if (uses_v0) {
        if (last_def_idx == -1) {
          props.uses_v0_before_def = true;
        } else {
          props.v0_used_after_last_def = true;
        }
      }

      bool defs_v0 = false;
      for (Register r : ud.gpr_defs) {
        if (r == Register::kV0) {
          defs_v0 = true;
          break;
        }
      }
      if (defs_v0) {
        props.defines_v0 = true;
        last_def_idx = static_cast<int>(i);
        props.v0_used_after_last_def = false;
      }
    }

    // Direct check: if an exit block directly defines $v0 and does not use it after,
    // it returns $v0.
    if (props.has_return && props.defines_v0 && !props.v0_used_after_last_def) {
      return true;
    }
  }

  // Helper lambda: inspect a sequence of instructions to check if it produces an unconsumed def of
  // $v0
  auto produces_unconsumed_v0 = [](absl::Span<const Instruction> instructions) -> bool {
    int last_def = -1;
    bool used_after_last_def = false;
    for (size_t i = 0; i < instructions.size(); ++i) {
      RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
      for (Register r : ud.gpr_uses) {
        if (r == Register::kV0) {
          if (last_def != -1) {
            used_after_last_def = true;
          }
          break;
        }
      }
      for (Register r : ud.gpr_defs) {
        if (r == Register::kV0) {
          last_def = static_cast<int>(i);
          used_after_last_def = false;
          break;
        }
      }
    }
    return last_def != -1 && !used_after_last_def;
  };

  // Check each outgoing edge across all blocks
  for (size_t b_idx = 0; b_idx < cfg.Blocks().size(); ++b_idx) {
    const auto& block = cfg.Blocks()[b_idx];
    const auto* term = block.Terminator();

    for (const auto& edge : block.outgoing_edges) {
      absl::Span<const Instruction> executed_instructions = block.instructions;
      // If this is a fallthrough edge from a branch likely, the delay slot instruction
      // was nullified and did not execute on fallthrough.
      if (edge.type == EdgeType::kFallthrough && term != nullptr && term->IsBranchLikely() &&
          executed_instructions.size() > 1) {
        executed_instructions = executed_instructions.subspan(0, executed_instructions.size() - 1);
      }

      if (!produces_unconsumed_v0(executed_instructions)) {
        continue;
      }

      // This edge carries an unconsumed definition of $v0 into edge.to_block_id.
      // Search forward to see if it reaches any exit block without being consumed or redefined.
      auto target_it = id_to_index.find(edge.to_block_id);
      if (target_it == id_to_index.end()) {
        continue;
      }

      std::vector<size_t> worklist = {target_it->second};
      absl::flat_hash_set<size_t> visited = {target_it->second};

      while (!worklist.empty()) {
        size_t curr_idx = worklist.back();
        worklist.pop_back();

        const auto& curr_props = block_props[curr_idx];
        // If this block consumes $v0 before defining it, the reaching definition is consumed.
        if (curr_props.uses_v0_before_def) {
          continue;
        }

        // If this block is an exit block and does not redefine $v0,
        // the unconsumed definition reaches the return!
        if (curr_props.has_return && !curr_props.defines_v0) {
          return true;
        }

        // If this block defines $v0, it redefines $v0 (kills the incoming def).
        if (curr_props.defines_v0) {
          continue;
        }

        // Otherwise propagate through successors.
        const auto& curr_block = cfg.Blocks()[curr_idx];
        for (uint32_t succ_id : curr_block.successors) {
          auto it = id_to_index.find(succ_id);
          if (it != id_to_index.end() && visited.insert(it->second).second) {
            worklist.push_back(it->second);
          }
        }
      }
    }
  }

  return false;
}

bool ExpressionBuilder::DetermineReturnsF0(const ControlFlowGraph& cfg) {
  for (const auto& block : cfg.Blocks()) {
    if (block.HasReturn()) {
      for (const auto& inst : block.instructions) {
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (FpRegister r : ud.fpr_defs) {
          if (r == FpRegister::kF0) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

std::optional<ExpressionBuilder::FoldedComparison> ExpressionBuilder::FindFoldedComparison(
    absl::Span<const Instruction> instructions) {
  if (instructions.size() < 2) {
    return std::nullopt;
  }

  // 1. Find the terminating branch instruction (skipping trailing delay slot if present)
  int branch_idx = -1;
  for (int i = static_cast<int>(instructions.size()) - 1; i >= 0; --i) {
    if (instructions[i].IsBranch()) {
      branch_idx = i;
      break;
    }
  }

  if (branch_idx < 0) {
    return std::nullopt;
  }

  const auto& branch = instructions[branch_idx];
  if (branch.opcode == Opcode::kBc1t || branch.opcode == Opcode::kBc1tl ||
      branch.opcode == Opcode::kBc1f || branch.opcode == Opcode::kBc1fl) {
    int def_idx = -1;
    for (int i = branch_idx - 1; i >= 0; --i) {
      const auto& op = instructions[i].opcode;
      if (op == Opcode::kCEqS || op == Opcode::kCEqD || op == Opcode::kCLtS ||
          op == Opcode::kCLtD || op == Opcode::kCLeS || op == Opcode::kCLeD) {
        def_idx = i;
        break;
      }
      if (op == Opcode::kCtc1) {
        break;
      }
    }
    if (def_idx >= 0) {
      const auto& def_inst = instructions[def_idx];
      RegisterUseDef def_ud = GetInstructionUseDef(def_inst);
      bool hazard = false;
      for (int i = def_idx + 1; i < branch_idx; ++i) {
        RegisterUseDef mid_ud = GetInstructionUseDef(instructions[i]);
        for (FpRegister input_reg : def_ud.fpr_uses) {
          for (FpRegister mid_def : mid_ud.fpr_defs) {
            if (mid_def == input_reg) {
              hazard = true;
              break;
            }
          }
          if (hazard) break;
        }
        if (hazard) break;
      }
      if (!hazard) {
        FoldedComparison folded;
        folded.instruction_index = static_cast<size_t>(def_idx);
        folded.can_suppress_statement = true;
        return folded;
      }
    }
    return std::nullopt;
  }

  Register rs = branch.rs.value_or(Register::kZero);
  Register rt = branch.rt.value_or(Register::kZero);

  // 2. Identify condition register tested against zero
  std::optional<Register> cond_reg;
  switch (branch.opcode) {
    case Opcode::kBne:
    case Opcode::kBnel:
    case Opcode::kBeq:
    case Opcode::kBeql:
      if (rt == Register::kZero && rs != Register::kZero) {
        cond_reg = rs;
      } else if (rs == Register::kZero && rt != Register::kZero) {
        cond_reg = rt;
      }
      break;

    case Opcode::kBgtz:
    case Opcode::kBgtzl:
    case Opcode::kBgez:
    case Opcode::kBgezl:
    case Opcode::kBltz:
    case Opcode::kBltzl:
    case Opcode::kBlez:
    case Opcode::kBlezl:
      if (rs != Register::kZero) {
        cond_reg = rs;
      }
      break;

    default:
      break;
  }

  if (!cond_reg.has_value()) {
    return std::nullopt;
  }

  // 3. Scan backward from branch_idx - 1 for reaching definition of cond_reg
  int def_idx = -1;
  for (int i = branch_idx - 1; i >= 0; --i) {
    RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
    bool defs_cond = false;
    for (Register r : ud.gpr_defs) {
      if (r == *cond_reg) {
        defs_cond = true;
        break;
      }
    }
    if (defs_cond) {
      def_idx = i;
      break;
    }
  }

  if (def_idx < 0) {
    return std::nullopt;
  }

  const auto& def_inst = instructions[def_idx];

  // Verify that def_inst is a comparison/test opcode that defines cond_reg
  switch (def_inst.opcode) {
    case Opcode::kSlt:
    case Opcode::kSltu:
    case Opcode::kXor:
    case Opcode::kAnd:
      if (!def_inst.rd.has_value() || *def_inst.rd != *cond_reg) {
        return std::nullopt;
      }
      break;

    case Opcode::kSlti:
    case Opcode::kSltiu:
    case Opcode::kXori:
    case Opcode::kAndi:
      if (!def_inst.rt.has_value() || *def_inst.rt != *cond_reg) {
        return std::nullopt;
      }
      break;

    default:
      return std::nullopt;
  }

  // 4. Hazard check: no instruction between def_idx and branch_idx clobbers inputs of def_inst,
  // or uses or clobbers cond_reg
  RegisterUseDef def_ud = GetInstructionUseDef(def_inst);
  for (int i = def_idx + 1; i < branch_idx; ++i) {
    RegisterUseDef mid_ud = GetInstructionUseDef(instructions[i]);
    for (Register input_reg : def_ud.gpr_uses) {
      for (Register mid_def : mid_ud.gpr_defs) {
        if (mid_def == input_reg) {
          return std::nullopt;
        }
      }
    }
    for (Register mid_use : mid_ud.gpr_uses) {
      if (mid_use == *cond_reg) {
        return std::nullopt;
      }
    }
    for (Register mid_def : mid_ud.gpr_defs) {
      if (mid_def == *cond_reg) {
        return std::nullopt;
      }
    }
  }

  // 5. Determine whether emitting def_inst as a statement can be safely suppressed
  bool can_suppress = false;
  if (*cond_reg == Register::kAt) {
    can_suppress = true;
  } else {
    // If cond_reg is an input to def_inst, emitting def_inst as an assignment clobbers
    // its own input, corrupting the folded branch expression. It must be suppressed.
    bool cond_is_input = false;
    for (Register r : def_ud.gpr_uses) {
      if (r == *cond_reg) {
        cond_is_input = true;
        break;
      }
    }
    if (cond_is_input) {
      can_suppress = true;
    } else if (branch_idx + 1 < static_cast<int>(instructions.size())) {
      const auto& delay = instructions[branch_idx + 1];
      RegisterUseDef delay_ud = GetInstructionUseDef(delay);
      bool delay_uses_cond = false;
      for (Register r : delay_ud.gpr_uses) {
        if (r == *cond_reg) {
          delay_uses_cond = true;
          break;
        }
      }
      bool delay_defs_cond = false;
      for (Register r : delay_ud.gpr_defs) {
        if (r == *cond_reg) {
          delay_defs_cond = true;
          break;
        }
      }
      if (!delay_uses_cond && delay_defs_cond) {
        can_suppress = true;
      }
    }
  }

  return FoldedComparison{static_cast<size_t>(def_idx), can_suppress};
}

}  // namespace rom_nom_nom
