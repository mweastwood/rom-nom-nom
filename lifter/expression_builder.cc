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

std::string ExpressionBuilder::FpRegisterVarName(FpRegister reg, bool is_double) {
  std::string_view name = FpRegisterName(reg);
  std::string base;
  if (!name.empty() && name[0] == '$') {
    base = std::string(name.substr(1));
  } else {
    base = std::string(name);
  }
  if (is_double) {
    return absl::StrCat(base, "_d");
  }
  return base;
}

namespace {

std::unique_ptr<LiftedExpression> LiftFpRegister(FpRegister reg, bool is_double = false) {
  return LiftedExpression::Variable(ExpressionBuilder::FpRegisterVarName(reg, is_double));
}

std::string StackVarName(int32_t offset) {
  if (offset < 0) {
    return absl::StrFormat("var_sp_neg_%d", -offset);
  }
  return absl::StrFormat("var_sp_%d", offset);
}

struct FloatingPointConversionPattern {
  size_t end_index = 0;
  bool is_int_to_float = false;
  Register src_gpr = Register::kZero;
  FpRegister dest_floating_point_reg = FpRegister::kF0;
  bool is_double = false;
  FpRegister src_floating_point_reg = FpRegister::kF0;
  Register dest_gpr = Register::kZero;
  bool is_rounding = false;
  std::string round_func;
};

std::optional<FloatingPointConversionPattern> MatchFloatingPointConversionPattern(
    absl::Span<const Instruction> instructions, size_t i) {
  const auto& inst = instructions[i];

  // 1. Integer to Float: mtc1 $rt, $fs [-> nop]* -> cvt.s.w/cvt.d.w $fd, $fs
  if (inst.opcode == Opcode::kMtc1 && inst.fs.has_value() && inst.rt.has_value()) {
    size_t j = i + 1;
    while (j < instructions.size() && instructions[j].IsNop()) {
      ++j;
    }
    if (j < instructions.size()) {
      const auto& next_inst = instructions[j];
      if ((next_inst.opcode == Opcode::kCvtSW || next_inst.opcode == Opcode::kCvtDW) &&
          next_inst.fs.has_value() && *next_inst.fs == *inst.fs && next_inst.fd.has_value()) {
        FloatingPointConversionPattern pattern;
        pattern.end_index = j;
        pattern.is_int_to_float = true;
        pattern.src_gpr = *inst.rt;
        pattern.dest_floating_point_reg = *next_inst.fd;
        pattern.is_double = (next_inst.opcode == Opcode::kCvtDW);
        return pattern;
      }
    }
  }

  // 2. Float to Integer: (trunc/ceil/floor/round).w.(s/d) $fd, $fs [-> nop]* -> mfc1 $rt, $fd
  if ((inst.opcode == Opcode::kTruncWS || inst.opcode == Opcode::kTruncWD ||
       inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD ||
       inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD ||
       inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD) &&
      inst.fd.has_value() && inst.fs.has_value()) {
    size_t j = i + 1;
    while (j < instructions.size() && instructions[j].IsNop()) {
      ++j;
    }
    if (j < instructions.size()) {
      const auto& next_inst = instructions[j];
      if (next_inst.opcode == Opcode::kMfc1 && next_inst.fs.has_value() &&
          *next_inst.fs == *inst.fd && next_inst.rt.has_value() &&
          *next_inst.rt != Register::kZero) {
        FloatingPointConversionPattern pattern;
        pattern.end_index = j;
        pattern.is_int_to_float = false;
        pattern.src_floating_point_reg = *inst.fs;
        pattern.dest_gpr = *next_inst.rt;
        pattern.is_double = (inst.opcode == Opcode::kTruncWD || inst.opcode == Opcode::kCeilWD ||
                             inst.opcode == Opcode::kFloorWD || inst.opcode == Opcode::kRoundWD);
        if (inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD) {
          pattern.round_func = pattern.is_double ? "ceil" : "ceilf";
        } else if (inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD) {
          pattern.round_func = pattern.is_double ? "floor" : "floorf";
        } else if (inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD) {
          pattern.round_func = pattern.is_double ? "round" : "roundf";
        }
        return pattern;
      }
    }
  }

  // 3. Float rounding: (ceil/floor/round/trunc).w.(s/d) $fd, $fs [-> nop]* -> cvt.(s/d).w $dest,
  // $fd
  if ((inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD ||
       inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD ||
       inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD ||
       inst.opcode == Opcode::kTruncWS || inst.opcode == Opcode::kTruncWD) &&
      inst.fd.has_value() && inst.fs.has_value()) {
    size_t j = i + 1;
    while (j < instructions.size() && instructions[j].IsNop()) {
      ++j;
    }
    if (j < instructions.size()) {
      const auto& next_inst = instructions[j];
      if ((next_inst.opcode == Opcode::kCvtSW || next_inst.opcode == Opcode::kCvtDW) &&
          next_inst.fs.has_value() && *next_inst.fs == *inst.fd && next_inst.fd.has_value()) {
        bool is_double = (inst.opcode == Opcode::kCeilWD || inst.opcode == Opcode::kFloorWD ||
                          inst.opcode == Opcode::kRoundWD || inst.opcode == Opcode::kTruncWD ||
                          next_inst.opcode == Opcode::kCvtDW);
        std::string func_name;
        if (inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD) {
          func_name = is_double ? "ceil" : "ceilf";
        } else if (inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD) {
          func_name = is_double ? "floor" : "floorf";
        } else if (inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD) {
          func_name = is_double ? "round" : "roundf";
        } else {
          func_name = is_double ? "trunc" : "truncf";
        }
        FloatingPointConversionPattern pattern;
        pattern.end_index = j;
        pattern.is_rounding = true;
        pattern.round_func = func_name;
        pattern.dest_floating_point_reg = *next_inst.fd;
        pattern.src_floating_point_reg = *inst.fs;
        pattern.is_double = is_double;
        return pattern;
      }
    }
  }

  // 3. Float rounding: (ceil/floor/round/trunc).w.(s/d) $fd, $fs [-> nop]* -> cvt.(s/d).w $dest,
  // $fd
  if ((inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD ||
       inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD ||
       inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD ||
       inst.opcode == Opcode::kTruncWS || inst.opcode == Opcode::kTruncWD) &&
      inst.fd.has_value() && inst.fs.has_value()) {
    size_t j = i + 1;
    while (j < instructions.size() && instructions[j].IsNop()) {
      ++j;
    }
    if (j < instructions.size()) {
      const auto& next_inst = instructions[j];
      if ((next_inst.opcode == Opcode::kCvtSW || next_inst.opcode == Opcode::kCvtDW) &&
          next_inst.fs.has_value() && *next_inst.fs == *inst.fd && next_inst.fd.has_value()) {
        bool is_double = (inst.opcode == Opcode::kCeilWD || inst.opcode == Opcode::kFloorWD ||
                          inst.opcode == Opcode::kRoundWD || inst.opcode == Opcode::kTruncWD ||
                          next_inst.opcode == Opcode::kCvtDW);
        std::string func_name;
        if (inst.opcode == Opcode::kCeilWS || inst.opcode == Opcode::kCeilWD) {
          func_name = is_double ? "ceil" : "ceilf";
        } else if (inst.opcode == Opcode::kFloorWS || inst.opcode == Opcode::kFloorWD) {
          func_name = is_double ? "floor" : "floorf";
        } else if (inst.opcode == Opcode::kRoundWS || inst.opcode == Opcode::kRoundWD) {
          func_name = is_double ? "round" : "roundf";
        } else {
          func_name = is_double ? "trunc" : "truncf";
        }
        FloatingPointConversionPattern pattern;
        pattern.end_index = j;
        pattern.is_rounding = true;
        pattern.round_func = func_name;
        pattern.dest_floating_point_reg = *next_inst.fd;
        pattern.src_floating_point_reg = *inst.fs;
        pattern.is_double = is_double;
        return pattern;
      }
    }
  }

  return std::nullopt;
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
    if (lo.src_fp_reg.has_value()) {
      src_expr = LiftFpRegister(*lo.src_fp_reg, lo.access_type == "f64");
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
    if (lo.src_fp_reg.has_value()) {
      src_expr = LiftFpRegister(*lo.src_fp_reg);
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
}

void ExpressionBuilder::LiftCallInstruction(
    const Instruction& inst, size_t i, absl::Span<const Instruction> instructions_to_process,
    const SymbolIndex* symbol_index,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
    const absl::flat_hash_set<std::string>* functions_returning_v0,
    const absl::flat_hash_set<std::string>* functions_returning_f0, RegisterTracker& tracker,
    std::vector<LiftedStatement>& statements, bool sp_address_taken,
    const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info) {
  bool v0_used_after_call = false;
  bool v1_used_after_call = false;
  bool f0_used_after_call = false;
  bool f0_is_double = false;

  if (call_return_info != nullptr) {
    auto it = call_return_info->find(inst.vram);
    if (it != call_return_info->end()) {
      if (it->second.returns_v0) v0_used_after_call = true;
      if (it->second.returns_s64) {
        v0_used_after_call = true;
        v1_used_after_call = true;
      }
      if (it->second.returns_f0) f0_used_after_call = true;
      if (it->second.returns_f0_double) {
        f0_used_after_call = true;
        f0_is_double = true;
      }
    }
  }

  bool v0_active = !v0_used_after_call;
  bool v1_active = !v1_used_after_call;
  bool f0_active = !f0_used_after_call;
  for (size_t next_i = i + 1; next_i < instructions_to_process.size(); ++next_i) {
    const auto& next_inst = instructions_to_process[next_i];
    if (next_inst.opcode == Opcode::kJal || next_inst.opcode == Opcode::kJalr) {
      break;
    }
    RegisterUseDef next_ud = GetInstructionUseDef(next_inst);
    if (v0_active) {
      for (Register r : next_ud.gpr_uses) {
        if (r == Register::kV0) {
          v0_used_after_call = true;
          v0_active = false;
          break;
        }
      }
      for (Register r : next_ud.gpr_defs) {
        if (r == Register::kV0) {
          v0_active = false;
          break;
        }
      }
    }

    if (v1_active) {
      for (Register r : next_ud.gpr_uses) {
        if (r == Register::kV1) {
          v1_used_after_call = true;
          v1_active = false;
          break;
        }
      }
      for (Register r : next_ud.gpr_defs) {
        if (r == Register::kV1) {
          v1_active = false;
          break;
        }
      }
    }

    if (f0_active) {
      for (FpRegister r : next_ud.fpr_uses) {
        if (r == FpRegister::kF0) {
          f0_used_after_call = true;
          f0_active = false;
          Opcode next_op = next_inst.opcode;
          if (next_op == Opcode::kAddD || next_op == Opcode::kSubD || next_op == Opcode::kMulD ||
              next_op == Opcode::kDivD || next_op == Opcode::kSqrtD || next_op == Opcode::kAbsD ||
              next_op == Opcode::kMovD || next_op == Opcode::kNegD || next_op == Opcode::kCvtSD ||
              next_op == Opcode::kTruncWD || next_op == Opcode::kRoundWD ||
              next_op == Opcode::kCeilWD || next_op == Opcode::kFloorWD ||
              next_op == Opcode::kCEqD || next_op == Opcode::kCLtD || next_op == Opcode::kCLeD ||
              next_op == Opcode::kSdc1 || next_op == Opcode::kDmfc1) {
            f0_is_double = true;
          }
          break;
        }
      }
      for (FpRegister r : next_ud.fpr_defs) {
        if (r == FpRegister::kF0) {
          f0_active = false;
          break;
        }
      }
    }

    if (!v0_active && !v1_active && !f0_active) {
      break;
    }
  }

  std::string function_name;
  if (inst.opcode == Opcode::kJalr) {
    if (v0_used_after_call && v1_used_after_call && !f0_used_after_call) {
      function_name = absl::StrFormat(
          "((s64 (*)())%s)", ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    } else if (f0_used_after_call && f0_is_double) {
      function_name = absl::StrFormat(
          "((f64 (*)())%s)", ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    } else if (f0_used_after_call) {
      function_name = absl::StrFormat(
          "((f32 (*)())%s)", ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    } else if (v0_used_after_call) {
      function_name = absl::StrFormat(
          "((s32 (*)())%s)", ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    } else {
      function_name = absl::StrFormat(
          "((void (*)())%s)", ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    }
  } else {
    uint32_t target_vram = (inst.opcode == Opcode::kJal) ? inst.JumpTarget() : inst.BranchTarget();
    function_name = (symbol_index != nullptr)
                        ? symbol_index->LookupOrSynthesizeName(target_vram, SYMBOL_FUNC)
                        : absl::StrFormat("func_%08X", target_vram);
  }

  Register argument_registers[] = {Register::kA0, Register::kA1, Register::kA2, Register::kA3};
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

  int fp_arg_count = 0;
  if (function_fp_parameter_counts != nullptr) {
    auto it = function_fp_parameter_counts->find(function_name);
    if (it != function_fp_parameter_counts->end()) {
      fp_arg_count = it->second;
    }
  }

  std::vector<std::unique_ptr<LiftedExpression>> call_arguments;
  if (fp_arg_count > 0) {
    bool f12_double = false;
    bool f14_double = false;
    if (call_return_info != nullptr) {
      auto it = call_return_info->find(inst.vram);
      if (it != call_return_info->end()) {
        f12_double = it->second.arg_f12_double;
        f14_double = it->second.arg_f14_double;
      }
    }
    for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
      if (instructions_to_process[j].opcode == Opcode::kJal ||
          instructions_to_process[j].opcode == Opcode::kJalr) {
        break;
      }
      const auto& inst = instructions_to_process[j];
      if (inst.opcode == Opcode::kCvtDW || inst.opcode == Opcode::kCvtDS ||
          inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kMulD ||
          inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
          inst.opcode == Opcode::kDivD || inst.opcode == Opcode::kLdc1) {
        if (inst.fd == FpRegister::kF12 || inst.fs == FpRegister::kF12 ||
            inst.ft == FpRegister::kF12) {
          f12_double = true;
        }
        if (inst.fd == FpRegister::kF14 || inst.fs == FpRegister::kF14 ||
            inst.ft == FpRegister::kF14) {
          f14_double = true;
        }
      }
    }
    int slot = 0;
    int fp_idx = 0;
    while (slot < argument_count || fp_idx < fp_arg_count) {
      if (fp_idx < fp_arg_count) {
        bool is_double = (fp_idx == 0) ? f12_double : f14_double;
        if (is_double && (slot % 2 != 0)) {
          slot++;
        }
        std::string fp_name =
            (fp_idx == 0) ? (is_double ? "f12_d" : "f12") : (is_double ? "f14_d" : "f14");
        call_arguments.push_back(LiftedExpression::Variable(fp_name));
        slot += (is_double ? 2 : 1);
        fp_idx++;
      } else {
        if (slot < 4 && tracker.GetReachingDefinition(argument_registers[slot]).has_value()) {
          call_arguments.push_back(
              ExpressionBuilder::LiftRegisterOrConstant(argument_registers[slot], tracker));
        } else if (slot < 4) {
          call_arguments.push_back(LiftedExpression::Variable(absl::StrFormat("arg%d", slot)));
        } else {
          int32_t stack_offset = 16 + (slot - 4) * 4;
          if (sp_address_taken) {
            std::unique_ptr<LiftedExpression> addr_expr;
            if (stack_offset == 0) {
              addr_expr = LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp"));
            } else {
              addr_expr = LiftedExpression::Binary(
                  "+", LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp")),
                  LiftedExpression::Integer(stack_offset));
            }
            call_arguments.push_back(LiftedExpression::Load("s32", std::move(addr_expr)));
          } else {
            call_arguments.push_back(LiftedExpression::Variable(StackVarName(stack_offset)));
          }
        }
        slot++;
      }
    }
  } else {
    call_arguments.reserve(argument_count);
    for (int arg_index = 0; arg_index < argument_count; ++arg_index) {
      if (arg_index < 4 &&
          tracker.GetReachingDefinition(argument_registers[arg_index]).has_value()) {
        call_arguments.push_back(
            ExpressionBuilder::LiftRegisterOrConstant(argument_registers[arg_index], tracker));
      } else if (arg_index < 4) {
        call_arguments.push_back(LiftedExpression::Variable(absl::StrFormat("arg%d", arg_index)));
      } else {
        int32_t stack_offset = 16 + (arg_index - 4) * 4;
        if (sp_address_taken) {
          std::unique_ptr<LiftedExpression> addr_expr;
          if (stack_offset == 0) {
            addr_expr = LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp"));
          } else {
            addr_expr = LiftedExpression::Binary(
                "+", LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp")),
                LiftedExpression::Integer(stack_offset));
          }
          call_arguments.push_back(LiftedExpression::Load("s32", std::move(addr_expr)));
        } else {
          call_arguments.push_back(LiftedExpression::Variable(StackVarName(stack_offset)));
        }
      }
    }

    if (call_arguments.empty()) {
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
      int fp_param_count = 0;
      if (function_fp_parameter_counts != nullptr) {
        auto it = function_fp_parameter_counts->find(function_name);
        if (it != function_fp_parameter_counts->end()) {
          fp_param_count = it->second;
        }
      }
      if (defines_f12 || fp_param_count >= 1) {
        call_arguments.push_back(LiftedExpression::Variable("f12"));
        if (defines_f14 || fp_param_count >= 2) {
          call_arguments.push_back(LiftedExpression::Variable("f14"));
        }
      }
    }
  }

  bool returns_f0 = false;
  bool returns_v0 = false;
  if (inst.opcode == Opcode::kJal && functions_returning_v0 != nullptr &&
      functions_returning_f0 != nullptr) {
    if (functions_returning_f0->contains(function_name)) {
      returns_f0 = true;
    } else if (functions_returning_v0->contains(function_name)) {
      returns_v0 = true;
    }
  } else if (v0_used_after_call && !f0_used_after_call) {
    returns_v0 = true;
  } else if (f0_used_after_call && !v0_used_after_call) {
    returns_f0 = true;
  } else if (functions_returning_v0 != nullptr && functions_returning_v0->contains(function_name)) {
    returns_v0 = true;
  } else if (functions_returning_f0 != nullptr && functions_returning_f0->contains(function_name)) {
    returns_f0 = true;
  } else if (v0_used_after_call) {
    returns_v0 = true;
  } else if (f0_used_after_call) {
    returns_f0 = true;
  }

  if (v0_used_after_call && v1_used_after_call && !returns_f0) {
    LiftedStatement statement;
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = "call_res_64";
    std::string cast_func = function_name;
    if (!absl::StartsWith(cast_func, "((s64 (*)())")) {
      cast_func = absl::StrFormat("((s64 (*)())%s)", function_name);
    }
    statement.expression = LiftedExpression::Call(cast_func, std::move(call_arguments));
    statements.push_back(std::move(statement));

    LiftedStatement stmt_v0;
    stmt_v0.kind = StatementKind::kAssignment;
    stmt_v0.destination_variable = "v0";
    stmt_v0.expression = LiftedExpression::Unary(
        "(s32)", LiftedExpression::Binary(">>", LiftedExpression::Variable("call_res_64"),
                                          LiftedExpression::Integer(32)));
    statements.push_back(std::move(stmt_v0));

    LiftedStatement stmt_v1;
    stmt_v1.kind = StatementKind::kAssignment;
    stmt_v1.destination_variable = "v1";
    stmt_v1.expression =
        LiftedExpression::Unary("(s32)", LiftedExpression::Variable("call_res_64"));
    statements.push_back(std::move(stmt_v1));
    return;
  }

  LiftedStatement statement;
  if (returns_f0) {
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = f0_is_double ? "f0_d" : "f0";
  } else if (returns_v0) {
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = "v0";
  } else {
    statement.kind = StatementKind::kCall;
  }
  statement.expression = LiftedExpression::Call(function_name, std::move(call_arguments));
  statements.push_back(std::move(statement));
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
  return LiftInstructions(block.instructions, symbol_index, game_config, function_parameter_counts,
                          function_fp_parameter_counts, omit_last_instruction, frame_size,
                          functions_returning_v0, functions_returning_f0, has_frame_pointer,
                          has_calls, sp_address_taken, saved_reg_offsets, saved_fpr_offsets,
                          initial_fpr_const_bits, initial_tracker, call_return_info,
                          function_stack_stores);
}

// NOLINTNEXTLINE(google-readability-function-size)
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
  folder.Fold(instructions_to_process, symbol_index, game_config);

  if (!sp_address_taken) {
    for (const auto& inst : instructions_to_process) {
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
          (inst.rs == Register::kSp || (has_frame_pointer && inst.rs == Register::kFp)) &&
          inst.rt.has_value() && *inst.rt != Register::kSp && *inst.rt != Register::kFp &&
          inst.immediate >= 0) {
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

  int32_t sp_base_offset = (has_calls && frame_size > 16) ? 16 : 0;
  auto make_sp_addr = [sp_base_offset](int32_t offset) -> std::unique_ptr<LiftedExpression> {
    int32_t eff_offset = (offset >= sp_base_offset) ? (offset - sp_base_offset) : offset;
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

  RegisterTracker tracker;
  if (initial_tracker != nullptr) {
    tracker = *initial_tracker;
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
      if (sp_address_taken) {
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

  absl::flat_hash_set<size_t> skipped_instruction_indices;
  std::optional<uint32_t> fpr_const_bits[32];
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
    }

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
        tracker.Step(instructions_to_process[k]);
      }
      i = conversion->end_index;
      continue;
    }

    if (skipped_instruction_indices.contains(i)) {
      tracker.Step(inst);
      continue;
    }

    // If this instruction is the high or mid half of a folded pair, skip emitting it
    if (folder.IsFoldedHi(i) || folder.IsFoldedMid(i)) {
      tracker.Step(inst);
      continue;
    }

    // If this instruction is the low half of a folded pair, emit high-level folded access
    if (const auto* lo = folder.GetFoldedLo(i)) {
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
            statement.expression =
                LiftedExpression::Binary("+", lift_reg(*inst.rs), lift_reg(*inst.rt));
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
          } else if (sp_address_taken && (inst.rs == Register::kSp ||
                                          (has_frame_pointer && inst.rs == Register::kFp))) {
            statement.expression = make_sp_addr(inst.immediate);
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
        if (inst.rs.has_value() && inst.rt.has_value()) {
          pending_mult = std::make_unique<PendingMult>();
          pending_mult->is_unsigned = (inst.opcode == Opcode::kMultu);
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
                next_inst.opcode == Opcode::kDiv || next_inst.opcode == Opcode::kDivu ||
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
                  next_inst.opcode == Opcode::kDiv || next_inst.opcode == Opcode::kDivu ||
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
                  next_inst.opcode == Opcode::kDiv || next_inst.opcode == Opcode::kDivu ||
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
            int arg_idx = -1;
            if (offset >= static_cast<int32_t>(frame_size + 16)) {
              arg_idx = 4 + (offset - static_cast<int32_t>(frame_size + 16)) / 4;
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
            } else if (sp_address_taken) {
              statement.expression = LiftedExpression::Load(load_type, make_sp_addr(offset));
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
                    statement.expression =
                        LiftedExpression::Binary(">>", std::move(base_var),
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
          } else {
            std::string effective_load_type = load_type;
            if (tracker.IsConstant(*inst.rs)) {
              uint32_t const_addr = *tracker.GetConstant(*inst.rs) + inst.immediate;
              if (const_addr >= 0xA0000000 && const_addr <= 0xBFFFFFFF) {
                effective_load_type = "volatile " + std::string(load_type);
              }
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
            if (sp_address_taken) {
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
          }

          LiftedStatement statement;
          if (is_stack_base) {
            int32_t offset = inst.immediate;
            int arg_idx = -1;
            if (offset >= static_cast<int32_t>(frame_size + 16)) {
              arg_idx = 4 + (offset - static_cast<int32_t>(frame_size + 16)) / 4;
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
            } else if (sp_address_taken) {
              statement.kind = StatementKind::kStore;
              statement.store_type = store_type;
              statement.destination_address = make_sp_addr(offset);
              statement.expression = lift_reg(*inst.rt);
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
            if (sp_address_taken) {
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
                            sp_address_taken, call_return_info);
        break;

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
          bool is_stack_base =
              (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
          if (is_stack_base) {
            if (saved_fpr_offsets != nullptr) {
              auto it = saved_fpr_offsets->find(*inst.ft);
              if (it != saved_fpr_offsets->end() && it->second == inst.immediate) {
                // Callee-saved float register epilogue restore, omit from C body
                break;
              }
            }
            int32_t offset = inst.immediate;
            uint32_t base_offset = frame_size + 16;
            if (offset >= static_cast<int32_t>(base_offset)) {
              int arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
              statement.expression = LiftedExpression::Unary(
                  "*(f32*)&", LiftedExpression::Variable(absl::StrFormat("arg%d", arg_idx)));
            } else if (sp_address_taken) {
              statement.expression = LiftedExpression::Load("f32", make_sp_addr(offset));
            } else {
              statement.expression = LiftedExpression::Load(
                  "f32",
                  LiftedExpression::Unary("&", LiftedExpression::Variable(StackVarName(offset))));
            }
          } else {
            statement.expression =
                LiftedExpression::Load("f32", make_reg_offset_expr(*inst.rs, inst.immediate));
          }
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kSwc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "f32";
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
            if (sp_address_taken) {
              statement.destination_address = make_sp_addr(inst.immediate);
            } else {
              statement.destination_address = LiftedExpression::Unary(
                  "&", LiftedExpression::Variable(StackVarName(inst.immediate)));
            }
          } else {
            statement.destination_address = make_reg_offset_expr(*inst.rs, inst.immediate);
          }
          statement.expression = LiftFpRegister(*inst.ft);
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
            uint32_t base_offset = frame_size + 16;
            if (offset >= static_cast<int32_t>(base_offset)) {
              int arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
              statement.expression = LiftedExpression::Unary(
                  "*(f64*)&", LiftedExpression::Variable(absl::StrFormat("arg%d", arg_idx)));
            } else {
              int reg_num = static_cast<int>(*inst.ft);
              if (frame_size > 0 && offset < static_cast<int32_t>(frame_size) &&
                  reg_num >= static_cast<int>(FpRegister::kF20) &&
                  reg_num <= static_cast<int>(FpRegister::kF31)) {
                // Function epilogue restoring callee-saved float register, omit from C body
                break;
              }
              if (sp_address_taken) {
                statement.expression = LiftedExpression::Load("f64", make_sp_addr(offset));
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
            int reg_num = static_cast<int>(*inst.ft);
            if (frame_size > 0 && inst.immediate < static_cast<int32_t>(frame_size) &&
                reg_num >= static_cast<int>(FpRegister::kF20) &&
                reg_num <= static_cast<int>(FpRegister::kF31)) {
              // Function prologue saving callee-saved float register, omit from C body
              break;
            }
            if (sp_address_taken) {
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
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = RegisterVarName(*inst.rt);
          statement.expression =
              LiftedExpression::Unary("*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
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
          uint32_t val_bits = 0;
          bool has_val_bits = false;
          if (*inst.rt == Register::kZero) {
            val_bits = 0;
            has_val_bits = true;
          } else {
            TrackedValue tv = tracker.GetRegisterValue(*inst.rt);
            if (tv.kind == ValueKind::kConstant) {
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
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/false);
          statement.expression = LiftedExpression::Unary(
              "(f32)*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
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
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = FpRegisterVarName(*inst.fd, /*is_double=*/true);
          statement.expression = LiftedExpression::Unary(
              "(f64)*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
          statements.push_back(std::move(statement));
        }
        break;

      case Opcode::kCvtWS:
      case Opcode::kCvtWD:
        if (inst.fd.has_value() && inst.fs.has_value()) {
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "s32";
          statement.destination_address = LiftedExpression::Unary(
              "&", LiftedExpression::Variable(FpRegisterVarName(*inst.fd, /*is_double=*/false)));
          bool src_is_double = (inst.opcode == Opcode::kCvtWD);
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
          LiftedStatement statement;
          statement.kind = StatementKind::kStore;
          statement.store_type = "s32";
          statement.destination_address = LiftedExpression::Unary(
              "&", LiftedExpression::Variable(FpRegisterVarName(*inst.fd, /*is_double=*/false)));
          bool src_is_double = (inst.opcode == Opcode::kTruncWD);
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
      case Opcode::kCLtS:
      case Opcode::kCOltS:
      case Opcode::kCNgeS:
      case Opcode::kCLeS:
      case Opcode::kCOleS:
      case Opcode::kCNgtS:
        if (inst.fs.has_value() && inst.ft.has_value()) {
          const char* op = "==";
          if (inst.opcode == Opcode::kCLtS || inst.opcode == Opcode::kCOltS ||
              inst.opcode == Opcode::kCNgeS) {
            op = "<";
          } else if (inst.opcode == Opcode::kCLeS || inst.opcode == Opcode::kCOleS ||
                     inst.opcode == Opcode::kCNgtS) {
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
      case Opcode::kCLtD:
      case Opcode::kCOltD:
      case Opcode::kCNgeD:
      case Opcode::kCLeD:
      case Opcode::kCOleD:
      case Opcode::kCNgtD:
        if (inst.fs.has_value() && inst.ft.has_value()) {
          const char* op = "==";
          if (inst.opcode == Opcode::kCLtD || inst.opcode == Opcode::kCOltD ||
              inst.opcode == Opcode::kCNgeD) {
            op = "<";
          } else if (inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kCOleD ||
                     inst.opcode == Opcode::kCNgtD) {
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

namespace {

bool IsRegisterLiveIn(const ControlFlowGraph& cfg, Register reg) {
  if (cfg.Blocks().empty()) return false;

  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  std::vector<bool> uses_before_def(cfg.Blocks().size(), false);
  std::vector<bool> defines(cfg.Blocks().size(), false);

  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    const auto& block = cfg.Blocks()[i];
    for (size_t j = 0; j < block.instructions.size(); ++j) {
      const auto& inst = block.instructions[j];
      bool is_call = (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr);

      if (is_call && j + 1 < block.instructions.size()) {
        RegisterUseDef delay_ud = GetInstructionUseDef(block.instructions[j + 1]);
        for (Register u : delay_ud.gpr_uses) {
          if (u == reg && !defines[i]) {
            uses_before_def[i] = true;
          }
        }
        for (Register d : delay_ud.gpr_defs) {
          if (d == reg) {
            defines[i] = true;
          }
        }
      }

      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register u : ud.gpr_uses) {
        if (u == reg && !defines[i]) {
          uses_before_def[i] = true;
        }
      }
      for (Register d : ud.gpr_defs) {
        if (d == reg) {
          defines[i] = true;
        }
      }
      if (is_call) {
        defines[i] = true;
        if (j + 1 < block.instructions.size()) {
          ++j;
        }
      }
    }
  }

  std::vector<size_t> queue = {0};
  absl::flat_hash_set<size_t> visited = {0};

  while (!queue.empty()) {
    size_t curr = queue.back();
    queue.pop_back();

    if (uses_before_def[curr]) {
      return true;
    }
    if (defines[curr]) {
      continue;
    }

    const auto& block = cfg.Blocks()[curr];
    for (uint32_t succ_id : block.successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end() && visited.insert(it->second).second) {
        queue.push_back(it->second);
      }
    }
  }

  return false;
}

bool IsFpRegisterLiveIn(const ControlFlowGraph& cfg, FpRegister reg) {
  if (cfg.Blocks().empty()) return false;

  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  std::vector<bool> uses_before_def(cfg.Blocks().size(), false);
  std::vector<bool> defines(cfg.Blocks().size(), false);

  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    const auto& block = cfg.Blocks()[i];
    for (size_t j = 0; j < block.instructions.size(); ++j) {
      const auto& inst = block.instructions[j];
      bool is_call = (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr);

      if (is_call && j + 1 < block.instructions.size()) {
        RegisterUseDef delay_ud = GetInstructionUseDef(block.instructions[j + 1]);
        for (FpRegister u : delay_ud.fpr_uses) {
          if (u == reg && !defines[i]) {
            uses_before_def[i] = true;
          }
        }
        for (FpRegister d : delay_ud.fpr_defs) {
          if (d == reg) {
            defines[i] = true;
          }
        }
      }

      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (FpRegister u : ud.fpr_uses) {
        if (u == reg && !defines[i]) {
          uses_before_def[i] = true;
        }
      }
      for (FpRegister d : ud.fpr_defs) {
        if (d == reg) {
          defines[i] = true;
        }
      }
      if (is_call) {
        defines[i] = true;
        if (j + 1 < block.instructions.size()) {
          ++j;
        }
      }
    }
  }

  std::vector<size_t> queue = {0};
  absl::flat_hash_set<size_t> visited = {0};

  while (!queue.empty()) {
    size_t curr = queue.back();
    queue.pop_back();

    if (uses_before_def[curr]) {
      return true;
    }
    if (defines[curr]) {
      continue;
    }

    const auto& block = cfg.Blocks()[curr];
    for (uint32_t succ_id : block.successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end() && visited.insert(it->second).second) {
        queue.push_back(it->second);
      }
    }
  }

  return false;
}

}  // namespace

int ExpressionBuilder::DetermineParameterCount(absl::Span<const Instruction> instructions) {
  absl::flat_hash_set<Register> defined_registers;
  absl::flat_hash_set<Register> used_before_definition;

  int frame_size = 0;
  for (const auto& inst : instructions) {
    if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) && inst.rt.has_value() &&
        *inst.rt == Register::kSp && inst.rs == Register::kSp && inst.immediate < 0) {
      frame_size = std::max(frame_size, static_cast<int>(-inst.immediate));
    }
  }

  int max_stack_arg = -1;
  absl::flat_hash_set<int32_t> stored_stack_offsets;

  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& instruction = instructions[i];
    bool is_call = (instruction.opcode == Opcode::kJal || instruction.opcode == Opcode::kJalr);

    if (instruction.rs == Register::kSp || instruction.rs == Register::kFp) {
      if (instruction.opcode == Opcode::kSw || instruction.opcode == Opcode::kSh ||
          instruction.opcode == Opcode::kSb || instruction.opcode == Opcode::kSwc1 ||
          instruction.opcode == Opcode::kSdc1) {
        stored_stack_offsets.insert(instruction.immediate);
      } else if (instruction.opcode == Opcode::kLw || instruction.opcode == Opcode::kLh ||
                 instruction.opcode == Opcode::kLhu || instruction.opcode == Opcode::kLb ||
                 instruction.opcode == Opcode::kLbu || instruction.opcode == Opcode::kLwc1 ||
                 instruction.opcode == Opcode::kLdc1) {
        int32_t offset = instruction.immediate;
        int arg_idx = -1;
        if (offset >= static_cast<int32_t>(frame_size + 16)) {
          arg_idx = 4 + (offset - static_cast<int32_t>(frame_size + 16)) / 4;
        }
        if (arg_idx >= 0 && !stored_stack_offsets.contains(offset)) {
          if (arg_idx < 4) {
            Register arg_reg = static_cast<Register>(static_cast<int>(Register::kA0) + arg_idx);
            used_before_definition.insert(arg_reg);
          } else {
            max_stack_arg = std::max(max_stack_arg, arg_idx);
          }
        }
      }
    }

    if (is_call && i + 1 < instructions.size()) {
      const auto& delay_inst = instructions[i + 1];
      RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
      for (Register used_register : delay_ud.gpr_uses) {
        if (!defined_registers.contains(used_register)) {
          used_before_definition.insert(used_register);
        }
      }
      for (Register defined_register : delay_ud.gpr_defs) {
        defined_registers.insert(defined_register);
      }
    }

    RegisterUseDef use_def = GetInstructionUseDef(instruction);
    for (Register used_register : use_def.gpr_uses) {
      if (!defined_registers.contains(used_register)) {
        used_before_definition.insert(used_register);
      }
    }
    for (Register defined_register : use_def.gpr_defs) {
      defined_registers.insert(defined_register);
    }
    if (is_call) {
      // Subroutine calls may clobber argument registers; from caller perspective they are redefined
      defined_registers.insert(Register::kA0);
      defined_registers.insert(Register::kA1);
      defined_registers.insert(Register::kA2);
      defined_registers.insert(Register::kA3);
      if (i + 1 < instructions.size()) {
        ++i;  // Delay slot was already processed
      }
    }
  }

  int count = 0;
  auto cfg = ControlFlowGraph::Build(instructions);
  auto check_reg = [&](Register reg) {
    if (cfg.ok() && IsRegisterLiveIn(*cfg, reg)) return true;
    return used_before_definition.contains(reg);
  };

  if (check_reg(Register::kA3)) {
    count = 4;
  } else if (check_reg(Register::kA2)) {
    count = 3;
  } else if (check_reg(Register::kA1)) {
    count = 2;
  } else if (check_reg(Register::kA0)) {
    count = 1;
  }

  if (max_stack_arg >= 4) {
    count = std::max(count, max_stack_arg + 1);
  }
  return count;
}

int ExpressionBuilder::DetermineFpParameterCount(absl::Span<const Instruction> instructions) {
  auto cfg = ControlFlowGraph::Build(instructions);
  if (cfg.ok()) {
    if (IsFpRegisterLiveIn(*cfg, FpRegister::kF12)) {
      if (IsFpRegisterLiveIn(*cfg, FpRegister::kF14)) {
        return 2;
      }
      return 1;
    }
    return 0;
  }

  absl::flat_hash_set<FpRegister> defined_fprs;
  absl::flat_hash_set<FpRegister> used_fprs_before_def;

  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& instruction = instructions[i];
    bool is_call = (instruction.opcode == Opcode::kJal || instruction.opcode == Opcode::kJalr);

    if (is_call && i + 1 < instructions.size()) {
      const auto& delay_inst = instructions[i + 1];
      RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
      for (FpRegister used_reg : delay_ud.fpr_uses) {
        if (!defined_fprs.contains(used_reg)) {
          used_fprs_before_def.insert(used_reg);
        }
      }
      for (FpRegister def_reg : delay_ud.fpr_defs) {
        defined_fprs.insert(def_reg);
      }
    }

    RegisterUseDef use_def = GetInstructionUseDef(instruction);
    for (FpRegister used_reg : use_def.fpr_uses) {
      if (!defined_fprs.contains(used_reg)) {
        used_fprs_before_def.insert(used_reg);
      }
    }
    for (FpRegister def_reg : use_def.fpr_defs) {
      defined_fprs.insert(def_reg);
    }
    if (is_call) {
      defined_fprs.insert(FpRegister::kF12);
      defined_fprs.insert(FpRegister::kF14);
      if (i + 1 < instructions.size()) {
        ++i;  // Delay slot was already processed
      }
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

bool ExpressionBuilder::DetermineReturnsV0(
    const ControlFlowGraph& cfg, const SymbolIndex* symbol_index,
    const absl::flat_hash_set<std::string>* functions_returning_v0) {
  return ::rom_nom_nom::DetermineReturnsV0(cfg, symbol_index, functions_returning_v0);
}

bool ExpressionBuilder::DetermineReturnsF0(const ControlFlowGraph& cfg) {
  return ::rom_nom_nom::DetermineReturnsF0(cfg);
}

bool ExpressionBuilder::DetermineReturnsF0Double(const ControlFlowGraph& cfg) {
  return ::rom_nom_nom::DetermineReturnsF0Double(cfg);
}

absl::flat_hash_set<Register> ExpressionBuilder::DetectUninitializedCalleeSavedRegisters(
    const ControlFlowGraph& cfg) {
  return ::rom_nom_nom::DetectUninitializedCalleeSavedRegisters(cfg);
}

absl::flat_hash_map<uint32_t, CallReturnInfo> ExpressionBuilder::ComputeCallReturnInfo(
    const ControlFlowGraph& cfg, bool function_returns_v0, bool function_returns_f0) {
  absl::flat_hash_map<uint32_t, CallReturnInfo> result;

  auto is_double_op = [](Opcode op) {
    return (op == Opcode::kAddD || op == Opcode::kSubD || op == Opcode::kMulD ||
            op == Opcode::kDivD || op == Opcode::kSqrtD || op == Opcode::kAbsD ||
            op == Opcode::kMovD || op == Opcode::kNegD || op == Opcode::kCvtSD ||
            op == Opcode::kTruncWD || op == Opcode::kRoundWD || op == Opcode::kCeilWD ||
            op == Opcode::kFloorWD || op == Opcode::kCEqD || op == Opcode::kCLtD ||
            op == Opcode::kCLeD || op == Opcode::kSdc1 || op == Opcode::kDmfc1);
  };

  for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
    const auto& block = cfg.Blocks()[b];
    for (size_t i = 0; i < block.instructions.size(); ++i) {
      const auto& call_inst = block.instructions[i];
      if (call_inst.opcode != Opcode::kJal && call_inst.opcode != Opcode::kJalr) {
        continue;
      }

      CallReturnInfo info;

      auto check_fp_arg_double = [&](FpRegister reg) -> bool {
        // Check delay slot instruction of this call if within current block
        if (i + 1 < block.instructions.size()) {
          const auto& inst = block.instructions[i + 1];
          if (inst.opcode == Opcode::kLdc1 && inst.ft == reg) return true;
          if (inst.opcode == Opcode::kDmtc1 && inst.fs == reg) return true;
          if ((inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
               inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
               inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
               inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
               inst.opcode == Opcode::kCvtDW || inst.opcode == Opcode::kCvtDS) &&
              inst.fd == reg) {
            return true;
          }
          RegisterUseDef ud = GetInstructionUseDef(inst);
          for (FpRegister d : ud.fpr_defs) {
            if (d == reg) return false;
          }
        }
        // Check instructions in the current block before the call
        for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
          const auto& inst = block.instructions[j];
          if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr) {
            break;
          }
          if (inst.opcode == Opcode::kLdc1 && inst.ft == reg) return true;
          if (inst.opcode == Opcode::kDmtc1 && inst.fs == reg) return true;
          if ((inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
               inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
               inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
               inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
               inst.opcode == Opcode::kCvtDW || inst.opcode == Opcode::kCvtDS) &&
              inst.fd == reg) {
            return true;
          }
          RegisterUseDef ud = GetInstructionUseDef(inst);
          for (FpRegister d : ud.fpr_defs) {
            if (d == reg) return false;
          }
        }
        // If not found in current block, search predecessor blocks in CFG
        absl::flat_hash_set<uint32_t> visited;
        visited.insert(block.id);
        std::vector<uint32_t> pred_queue = block.predecessors;
        while (!pred_queue.empty()) {
          uint32_t pid = pred_queue.back();
          pred_queue.pop_back();
          if (visited.contains(pid)) continue;
          visited.insert(pid);
          const auto* pblock = cfg.GetBlock(pid);
          if (pblock == nullptr) continue;
          bool def_found = false;
          for (int j = static_cast<int>(pblock->instructions.size()) - 1; j >= 0; --j) {
            const auto& inst = pblock->instructions[j];
            if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr) {
              break;
            }
            if (inst.opcode == Opcode::kLdc1 && inst.ft == reg) return true;
            if (inst.opcode == Opcode::kDmtc1 && inst.fs == reg) return true;
            if ((inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
                 inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
                 inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
                 inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
                 inst.opcode == Opcode::kCvtDW || inst.opcode == Opcode::kCvtDS) &&
                inst.fd == reg) {
              return true;
            }
            RegisterUseDef ud = GetInstructionUseDef(inst);
            for (FpRegister d : ud.fpr_defs) {
              if (d == reg) {
                def_found = true;
                break;
              }
            }
            if (def_found) break;
          }
          if (!def_found) {
            for (uint32_t next_pid : pblock->predecessors) {
              pred_queue.push_back(next_pid);
            }
          }
        }
        return false;
      };

      info.arg_f12_double = check_fp_arg_double(FpRegister::kF12);
      info.arg_f14_double = check_fp_arg_double(FpRegister::kF14);

      struct WorkItem {
        uint32_t block_id;
        size_t start_idx;
        bool v0_live;
        bool v1_live;
        bool f0_live;
        int depth;
      };

      std::vector<WorkItem> queue;
      if (i + 2 < block.instructions.size()) {
        queue.push_back({block.id, i + 2, true, true, true, 32});
      } else {
        for (uint32_t succ_id : block.successors) {
          queue.push_back({succ_id, 0, true, true, true, 32});
        }
        if (block.successors.empty()) {
          const auto* next_block = cfg.FindBlockByVram(call_inst.vram + 8);
          if (next_block != nullptr) {
            queue.push_back({next_block->id, 0, true, true, true, 32});
          }
        }
      }

      absl::flat_hash_set<uint32_t> visited_blocks;

      while (!queue.empty()) {
        auto item = queue.back();
        queue.pop_back();

        if (!item.v0_live && !item.v1_live && !item.f0_live) {
          continue;
        }

        const auto* curr_block = cfg.GetBlock(item.block_id);
        if (curr_block == nullptr) continue;

        if (item.start_idx == 0) {
          if (!visited_blocks.insert(item.block_id).second) {
            continue;
          }
        }

        bool stop_path = false;
        for (size_t k = item.start_idx; k < curr_block->instructions.size(); ++k) {
          if (item.depth <= 0) {
            stop_path = true;
            break;
          }
          --item.depth;

          const auto& inst = curr_block->instructions[k];

          if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr) {
            RegisterUseDef call_ud = GetInstructionUseDef(inst);
            if (item.v0_live) {
              for (Register u : call_ud.gpr_uses) {
                if (u == Register::kV0) {
                  info.returns_v0 = true;
                  item.v0_live = false;
                  break;
                }
              }
            }
            if (item.v1_live) {
              for (Register u : call_ud.gpr_uses) {
                if (u == Register::kV1) {
                  info.returns_s64 = true;
                  item.v1_live = false;
                  break;
                }
              }
            }
            if (item.f0_live) {
              for (FpRegister u : call_ud.fpr_uses) {
                if (u == FpRegister::kF0) {
                  info.returns_f0 = true;
                  if (is_double_op(inst.opcode)) info.returns_f0_double = true;
                  item.f0_live = false;
                  break;
                }
              }
            }
            if (k + 1 < curr_block->instructions.size()) {
              const auto& delay_inst = curr_block->instructions[k + 1];
              RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
              if (item.v0_live) {
                for (Register u : delay_ud.gpr_uses) {
                  if (u == Register::kV0) {
                    info.returns_v0 = true;
                    item.v0_live = false;
                    break;
                  }
                }
              }
              if (item.v1_live) {
                for (Register u : delay_ud.gpr_uses) {
                  if (u == Register::kV1) {
                    info.returns_s64 = true;
                    item.v1_live = false;
                    break;
                  }
                }
              }
              if (item.f0_live) {
                for (FpRegister u : delay_ud.fpr_uses) {
                  if (u == FpRegister::kF0) {
                    info.returns_f0 = true;
                    if (is_double_op(delay_inst.opcode)) info.returns_f0_double = true;
                    item.f0_live = false;
                    break;
                  }
                }
              }
            }
            stop_path = true;
            break;
          }

          RegisterUseDef ud = GetInstructionUseDef(inst);

          // Check uses first
          if (item.v0_live) {
            for (Register u : ud.gpr_uses) {
              if (u == Register::kV0) {
                info.returns_v0 = true;
                item.v0_live = false;
                break;
              }
            }
          }
          if (item.v1_live) {
            for (Register u : ud.gpr_uses) {
              if (u == Register::kV1) {
                info.returns_s64 = true;
                item.v1_live = false;
                break;
              }
            }
          }
          if (item.f0_live) {
            for (FpRegister u : ud.fpr_uses) {
              if (u == FpRegister::kF0) {
                info.returns_f0 = true;
                if (is_double_op(inst.opcode)) {
                  info.returns_f0_double = true;
                }
                item.f0_live = false;
                break;
              }
            }
          }

          // Check defs
          if (item.v0_live) {
            for (Register d : ud.gpr_defs) {
              if (d == Register::kV0) {
                item.v0_live = false;
                break;
              }
            }
          }
          if (item.v1_live) {
            for (Register d : ud.gpr_defs) {
              if (d == Register::kV1) {
                item.v1_live = false;
                break;
              }
            }
          }
          if (item.f0_live) {
            for (FpRegister d : ud.fpr_defs) {
              if (d == FpRegister::kF0) {
                item.f0_live = false;
                break;
              }
            }
          }

          if (inst.IsReturn()) {
            stop_path = true;
            break;
          }

          if (!item.v0_live && !item.v1_live && !item.f0_live) {
            stop_path = true;
            break;
          }
        }

        if (!stop_path && (item.v0_live || item.v1_live || item.f0_live)) {
          for (uint32_t succ_id : curr_block->successors) {
            queue.push_back({succ_id, 0, item.v0_live, item.v1_live, item.f0_live, item.depth});
          }
        }
      }

      if (info.returns_s64) {
        info.returns_v0 = true;
      }
      if (info.returns_f0_double) {
        info.returns_f0 = true;
      }

      result[call_inst.vram] = info;
    }
  }

  return result;
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

  // 5. Reject folding if a non-likely branch delay slot defines any input of def_inst (other than
  // cond_reg)
  if (!branch.IsBranchLikely() && branch_idx + 1 < static_cast<int>(instructions.size())) {
    RegisterUseDef delay_ud = GetInstructionUseDef(instructions[branch_idx + 1]);
    for (Register input_reg : def_ud.gpr_uses) {
      if (input_reg == *cond_reg) continue;
      for (Register delay_def : delay_ud.gpr_defs) {
        if (delay_def == input_reg) {
          return std::nullopt;
        }
      }
    }
  }

  // 6. Check if cond_reg is an input to def_inst (in-place modification / self-clobbering)
  bool cond_is_input = false;
  for (Register r : def_ud.gpr_uses) {
    if (r == *cond_reg) {
      cond_is_input = true;
      break;
    }
  }

  bool delay_clobbers_cond = false;
  if (branch_idx + 1 < static_cast<int>(instructions.size())) {
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
      delay_clobbers_cond = true;
    }
  }

  // If def_inst mutates cond_reg in place (cond_is_input) and cond_reg is not $at,
  // we can only fold and suppress def_inst if the delay slot immediately clobbers cond_reg.
  // Otherwise, def_inst is an in-place variable mutation that must be preserved as a
  // statement and cannot be folded into the branch condition.
  if (cond_is_input && *cond_reg != Register::kAt && !delay_clobbers_cond) {
    return std::nullopt;
  }

  // Determine whether emitting def_inst as a statement can be safely suppressed
  bool can_suppress = false;
  if (*cond_reg == Register::kAt || delay_clobbers_cond) {
    can_suppress = true;
  }

  return FoldedComparison{static_cast<size_t>(def_idx), can_suppress};
}

}  // namespace rom_nom_nom
