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
#include "lifter/cop1_lifter.h"
#include "lifter/instruction_patterns.h"
#include "lifter/stack_frame_analyzer.h"
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

std::unique_ptr<LiftedExpression> LiftedExpression::Cast(
    std::string type, std::unique_ptr<LiftedExpression> operand) {
  auto e = std::make_unique<LiftedExpression>();
  e->kind = ExpressionKind::kCast;
  e->name = std::move(type);
  e->args.push_back(std::move(operand));
  return e;
}

std::unique_ptr<LiftedExpression> LiftedExpression::Unary(
    std::string op, std::unique_ptr<LiftedExpression> operand) {
  if (op == "(u32)" || op == "(s32)" || op == "(u16)" || op == "(s16)" || op == "(u8)" ||
      op == "(s8)" || op == "(f32)" || op == "(f64)" || op == "(s64)" || op == "(u64)") {
    return Cast(op.substr(1, op.size() - 2), std::move(operand));
  }
  if (op == "(u8*)" || op == "(u8 *)") {
    return Cast("u8*", std::move(operand));
  }
  if (op == "(u64)(u32)") {
    return Cast("u64", Cast("u32", std::move(operand)));
  }
  if (op == "(s32)&") {
    return Cast("s32", Unary("&", std::move(operand)));
  }
  if (op == "(f32)*(s32*)&") {
    return Cast("f32", Unary("*(s32*)&", std::move(operand)));
  }
  if (op == "(f64)*(s32*)&") {
    return Cast("f64", Unary("*(s32*)&", std::move(operand)));
  }
  if (op == "(f32)*(s64*)&") {
    return Cast("f32", Unary("*(s64*)&", std::move(operand)));
  }
  if (op == "(f64)*(s64*)&") {
    return Cast("f64", Unary("*(s64*)&", std::move(operand)));
  }
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
    case ExpressionKind::kCast: {
      std::string operand = args.empty() ? "" : args[0]->ToString();
      if (!args.empty() && args[0]->kind == ExpressionKind::kBinaryOp) {
        operand = "(" + operand + ")";
      }
      return "(" + name + ")" + operand;
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
  return StackFrameAnalyzer::StackVarName(offset);
}

namespace {

std::unique_ptr<LiftedExpression> LiftFpRegister(FpRegister reg, bool is_double = false) {
  return LiftedExpression::Variable(ExpressionBuilder::FpRegisterVarName(reg, is_double));
}

struct PendingMult {
  bool is_unsigned = false;
  Register rs = Register::kZero;
  Register rt = Register::kZero;
};

struct PendingUnalignedAccess {
  Opcode opcode = Opcode::kUnknown;
  Register rt = Register::kZero;
  Register rs = Register::kZero;
  int16_t offset = 0;
};

bool IsUnalignedLoadOpcode(Opcode op) {
  return op == Opcode::kLwl || op == Opcode::kLwr || op == Opcode::kLdl || op == Opcode::kLdr;
}

Opcode PartnerUnalignedOpcode(Opcode op) {
  switch (op) {
    case Opcode::kLwl:
      return Opcode::kLwr;
    case Opcode::kLwr:
      return Opcode::kLwl;
    case Opcode::kLdl:
      return Opcode::kLdr;
    case Opcode::kLdr:
      return Opcode::kLdl;
    case Opcode::kSwl:
      return Opcode::kSwr;
    case Opcode::kSwr:
      return Opcode::kSwl;
    case Opcode::kSdl:
      return Opcode::kSdr;
    case Opcode::kSdr:
      return Opcode::kSdl;
    default:
      return Opcode::kUnknown;
  }
}

bool IsLeftUnalignedOpcode(Opcode op) {
  return op == Opcode::kLwl || op == Opcode::kLdl || op == Opcode::kSwl || op == Opcode::kSdl;
}

bool Is64BitUnalignedOpcode(Opcode op) {
  return op == Opcode::kLdl || op == Opcode::kLdr || op == Opcode::kSdl || op == Opcode::kSdr;
}

struct BlockLiftRunner {
  const FunctionLiftContext& ctx;
  absl::Span<const Instruction> raw_instructions;
  std::vector<Instruction> reordered_storage;
  absl::Span<const Instruction> instructions_to_process;
  size_t loop_end = 0;
  bool has_frame_pointer = false;
  bool has_calls = false;
  bool sp_address_taken = false;
  uint32_t frame_size = 0;
  int32_t sp_base_offset = 0;
  int32_t sp_buffer_size = 0;
  int32_t caller_frame_start = 0x7FFFFFFF;
  std::optional<size_t> sp_alloc_idx;
  SymbolFolder folder;
  RegisterTracker tracker;
  std::vector<LiftedStatement> statements;
  bool pending_return = false;
  std::unique_ptr<PendingMult> pending_mult;
  std::unique_ptr<PendingUnalignedAccess> pending_lwl;
  std::unique_ptr<PendingUnalignedAccess> pending_swl;
  absl::flat_hash_set<int32_t> local_stack_stores;
  std::optional<FoldedComparison> folded_comp;
  absl::flat_hash_set<size_t> skipped_instruction_indices;
  absl::flat_hash_set<Register> gprs_defined_in_block;
  std::optional<uint32_t> fpr_const_bits[32];
  std::unique_ptr<LiftedExpression> pending_int_to_fp[32];
  std::unique_ptr<LiftedExpression> pending_fp_to_int[32];

  BlockLiftRunner(absl::Span<const Instruction> instructions, const FunctionLiftContext& lift_ctx,
                  bool omit_last_instruction)
      : ctx(lift_ctx),
        raw_instructions(instructions),
        instructions_to_process(instructions),
        has_frame_pointer(lift_ctx.has_frame_pointer),
        has_calls(lift_ctx.has_calls),
        sp_address_taken(lift_ctx.sp_address_taken),
        frame_size(lift_ctx.frame_size),
        sp_buffer_size(lift_ctx.sp_buffer_size) {
    Initialize(omit_last_instruction);
  }

  bool IsCallerHomedScalarAddr(int32_t imm) const {
    return StackFrameAnalyzer::IsCallerHomedScalarAddr(static_cast<int32_t>(frame_size), imm,
                                                       local_stack_stores);
  }

  std::unique_ptr<LiftedExpression> MakeSpAddr(int32_t offset) const {
    int32_t eff_offset = offset - sp_base_offset;
    if (eff_offset == 0) {
      return LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp"));
    }
    if (eff_offset > 0) {
      return LiftedExpression::Binary(
          "+", LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp")),
          LiftedExpression::Integer(eff_offset));
    }
    return LiftedExpression::Binary(
        "-", LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp")),
        LiftedExpression::Integer(-eff_offset));
  }

  bool IsSpOffset(int32_t offset) const {
    if (!sp_address_taken) return false;
    if (offset < sp_base_offset) return false;
    if (sp_buffer_size > 0 && (offset - sp_base_offset) >= sp_buffer_size) return false;
    return true;
  }

  std::unique_ptr<LiftedExpression> LiftReg(Register r) const {
    return ExpressionBuilder::LiftRegisterOrConstant(r, tracker, has_frame_pointer);
  }

  std::unique_ptr<LiftedExpression> MakeRegOffsetExpr(Register r, int16_t imm) const {
    if (imm == 0) {
      return LiftReg(r);
    }
    if (imm < 0) {
      return LiftedExpression::Binary("-", LiftReg(r), LiftedExpression::Integer(-imm));
    }
    return LiftedExpression::Binary("+", LiftReg(r), LiftedExpression::Integer(imm));
  }

  void EmitUnalignedLoad(Register rt, Register base_reg, int16_t offset, bool is_64bit) {
    const char* load_type = is_64bit ? "s64" : "s32";
    LiftedStatement statement;
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = ExpressionBuilder::RegisterVarName(rt);
    if (base_reg == Register::kSp) {
      if (IsSpOffset(offset)) {
        statement.expression = LiftedExpression::Load(load_type, MakeSpAddr(offset));
      } else {
        statement.expression = LiftedExpression::Variable(ExpressionBuilder::StackVarName(offset));
      }
    } else {
      statement.expression = LiftedExpression::Load(load_type, MakeRegOffsetExpr(base_reg, offset));
    }
    statements.push_back(std::move(statement));
  }

  void FlushPendingLwl() {
    if (pending_lwl == nullptr) return;
    EmitUnalignedLoad(pending_lwl->rt, pending_lwl->rs, pending_lwl->offset,
                      Is64BitUnalignedOpcode(pending_lwl->opcode));
    pending_lwl = nullptr;
  }

  void EmitUnalignedStore(Register rt, Register base_reg, int16_t offset, bool is_64bit) {
    if (base_reg == Register::kSp) {
      LiftedStatement statement;
      if (IsSpOffset(offset)) {
        statement.kind = StatementKind::kStore;
        statement.store_type = is_64bit ? "s64" : "s32";
        statement.destination_address = MakeSpAddr(offset);
        statement.expression = LiftReg(rt);
      } else {
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::StackVarName(offset);
        statement.expression = LiftReg(rt);
      }
      statements.push_back(std::move(statement));
    } else {
      int num_bytes = is_64bit ? 8 : 4;
      for (int b = 0; b < num_bytes; ++b) {
        LiftedStatement statement;
        statement.kind = StatementKind::kStore;
        statement.store_type = "u8";
        int32_t byte_offset = static_cast<int32_t>(offset) + b;
        if (byte_offset == 0) {
          statement.destination_address = LiftReg(base_reg);
        } else {
          statement.destination_address = LiftedExpression::Binary(
              "+", LiftReg(base_reg), LiftedExpression::Integer(byte_offset));
        }
        if (b == num_bytes - 1) {
          statement.expression = LiftReg(rt);
        } else {
          statement.expression = LiftedExpression::Binary(
              ">>", LiftReg(rt), LiftedExpression::Integer((num_bytes - 1 - b) * 8));
        }
        statements.push_back(std::move(statement));
      }
    }
  }

  void FlushPendingSwl() {
    if (pending_swl == nullptr) return;
    LiftedStatement statement;
    statement.kind = StatementKind::kStore;
    statement.store_type = "u8";
    Register base_reg = pending_swl->rs;
    if (pending_swl->offset == 0) {
      statement.destination_address = LiftReg(base_reg);
    } else {
      statement.destination_address = LiftedExpression::Binary(
          "+", LiftReg(base_reg), LiftedExpression::Integer(pending_swl->offset));
    }
    statement.expression = LiftReg(pending_swl->rt);
    statements.push_back(std::move(statement));
    pending_swl = nullptr;
  }

  void RecordGprDefs(const Instruction& ins) {
    RegisterUseDef ud = GetInstructionUseDef(ins);
    for (Register d : ud.gpr_defs) {
      gprs_defined_in_block.insert(d);
    }
  }

  bool IsFloatCarrierGpr(Register r) const {
    return ctx.float_parameters_in_general_registers != nullptr &&
           ctx.float_parameters_in_general_registers->contains(r) && r != Register::kA0 &&
           r != Register::kA1 && r != Register::kA2 && r != Register::kA3;
  }

  Cop1LiftState MakeCop1State() {
    return Cop1LiftState{
        .ctx = ctx,
        .instructions_to_process = instructions_to_process,
        .loop_end = loop_end,
        .has_frame_pointer = has_frame_pointer,
        .sp_address_taken = sp_address_taken,
        .frame_size = frame_size,
        .sp_base_offset = sp_base_offset,
        .sp_buffer_size = sp_buffer_size,
        .sp_alloc_idx = sp_alloc_idx,
        .tracker = tracker,
        .skipped_instruction_indices = skipped_instruction_indices,
        .gprs_defined_in_block = gprs_defined_in_block,
        .fpr_const_bits = fpr_const_bits,
        .pending_int_to_fp = pending_int_to_fp,
        .pending_fp_to_int = pending_fp_to_int,
        .statements = statements,
    };
  }

  void Initialize(bool omit_last_instruction);
  bool TryLiftMultiInstructionPatterns(size_t& i);
  void LiftIntegerAluInstruction(const Instruction& inst);
  void LiftMultiplyDivideInstruction(const Instruction& inst, size_t i);
  void LiftMemoryInstruction(const Instruction& inst, size_t i);
  void EmitPendingReturn();
  std::vector<LiftedStatement> Run();
};

void BlockLiftRunner::Initialize(bool omit_last_instruction) {
  if (raw_instructions.empty()) {
    return;
  }

  StackFrameAnalysis local_stack_analysis;
  local_stack_analysis.has_frame_pointer = has_frame_pointer;
  local_stack_analysis.has_calls = has_calls;
  local_stack_analysis.frame_size = static_cast<int32_t>(frame_size);
  StackFrameAnalyzer::ScanInstructionsPass1(raw_instructions, &local_stack_analysis);
  if (!has_frame_pointer) {
    has_frame_pointer = local_stack_analysis.has_frame_pointer;
  }
  if (frame_size == 0 && local_stack_analysis.frame_size > 0) {
    frame_size = static_cast<uint32_t>(local_stack_analysis.frame_size);
  }

  bool has_delayed_branch = false;
  for (const auto& instruction : raw_instructions) {
    if (instruction.IsCall() || instruction.IsReturn()) {
      has_delayed_branch = true;
      break;
    }
  }
  if (has_delayed_branch) {
    reordered_storage.reserve(raw_instructions.size());
    for (size_t instruction_index = 0; instruction_index < raw_instructions.size();
         ++instruction_index) {
      if ((raw_instructions[instruction_index].IsCall() ||
           raw_instructions[instruction_index].IsReturn()) &&
          instruction_index + 1 < raw_instructions.size()) {
        // In MIPS, delay slot instructions execute before the call or return takes effect.
        reordered_storage.push_back(raw_instructions[instruction_index + 1]);
        reordered_storage.push_back(raw_instructions[instruction_index]);
        ++instruction_index;
      } else {
        reordered_storage.push_back(raw_instructions[instruction_index]);
      }
    }
    instructions_to_process = reordered_storage;
  }

  if (!has_calls) {
    has_calls = local_stack_analysis.has_calls;
  }

  folder.Fold(instructions_to_process, ctx.symbol_index, ctx.game_config, ctx.initial_tracker);

  caller_frame_start = (frame_size > 0) ? static_cast<int32_t>(frame_size) : 0x7FFFFFFF;
  if (!sp_address_taken) {
    local_stack_analysis.frame_size = static_cast<int32_t>(frame_size);
    local_stack_analysis.has_frame_pointer = has_frame_pointer;
    if (ctx.function_stack_stores != nullptr) {
      local_stack_analysis.stack_store_offsets = *ctx.function_stack_stores;
    }
    StackFrameAnalyzer::ScanInstructionsAddressTaken(instructions_to_process,
                                                     &local_stack_analysis);
    sp_address_taken = local_stack_analysis.sp_address_taken;
  }

  sp_base_offset = ctx.outgoing_args_size > 0 ? static_cast<int32_t>(ctx.outgoing_args_size)
                                              : ((has_calls && frame_size > 16) ? 16 : 0);

  if (ctx.initial_tracker != nullptr) {
    tracker = *ctx.initial_tracker;
    tracker.PrepareForBlock();
  }

  if (ctx.function_stack_stores != nullptr) {
    local_stack_stores = *ctx.function_stack_stores;
  } else {
    for (const auto& inst : instructions_to_process) {
      if (inst.IsStore() && inst.rs.has_value() &&
          (*inst.rs == Register::kSp || (has_frame_pointer && *inst.rs == Register::kFp))) {
        local_stack_stores.insert(inst.immediate);
      }
    }
  }

  folded_comp = BranchConditionBuilder::FindFoldedComparison(instructions_to_process);

  loop_end = instructions_to_process.size();
  if (omit_last_instruction && loop_end > 0) {
    --loop_end;
  }

  for (size_t k = 0; k < loop_end; ++k) {
    const auto& k_inst = instructions_to_process[k];
    if ((k_inst.opcode == Opcode::kAddiu || k_inst.opcode == Opcode::kAddi) &&
        k_inst.rt == Register::kSp && k_inst.rs == Register::kSp && k_inst.immediate < 0) {
      sp_alloc_idx = k;
      break;
    }
  }

  if (ctx.initial_fpr_const_bits != nullptr) {
    for (int k = 0; k < 32; ++k) {
      fpr_const_bits[k] = (*ctx.initial_fpr_const_bits)[k];
    }
  }
}

bool BlockLiftRunner::TryLiftMultiInstructionPatterns(size_t& i) {
  const auto& inst = instructions_to_process[i];

  // Skip emitting assignment for folded comparison instructions that are directly
  // integrated into subsequent branch conditions.
  if (folded_comp.has_value() && folded_comp->can_suppress_statement &&
      i == folded_comp->instruction_index) {
    RecordGprDefs(inst);
    tracker.Step(inst);
    return true;
  }

  if (skipped_instruction_indices.contains(i)) {
    RecordGprDefs(inst);
    tracker.Step(inst);
    return true;
  }

  // Check for MIPS GCC integer division/modulo trap expansion
  auto div_pattern = MatchDivisionPattern(instructions_to_process, i);
  if (div_pattern.has_value()) {
    if (div_pattern->div_dest_reg.has_value()) {
      LiftedStatement statement;
      statement.kind = StatementKind::kAssignment;
      statement.destination_variable =
          ExpressionBuilder::RegisterVarName(*div_pattern->div_dest_reg);
      auto num = ExpressionBuilder::LiftRegisterOrConstant(div_pattern->num_reg, tracker);
      auto den = ExpressionBuilder::LiftRegisterOrConstant(div_pattern->den_reg, tracker);
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
      statement.destination_variable =
          ExpressionBuilder::RegisterVarName(*div_pattern->mod_dest_reg);
      auto num = ExpressionBuilder::LiftRegisterOrConstant(div_pattern->num_reg, tracker);
      auto den = ExpressionBuilder::LiftRegisterOrConstant(div_pattern->den_reg, tracker);
      if (div_pattern->is_unsigned) {
        num = LiftedExpression::Unary("(u32)", std::move(num));
        den = LiftedExpression::Unary("(u32)", std::move(den));
      }
      statement.expression = LiftedExpression::Binary("%", std::move(num), std::move(den));
      statements.push_back(std::move(statement));
    }
    for (size_t k = i; k <= div_pattern->end_index; ++k) {
      RecordGprDefs(instructions_to_process[k]);
      tracker.Step(instructions_to_process[k]);
    }
    i = div_pattern->end_index;
    return true;
  }

  // Check for MIPS GCC floating-point / integer conversion patterns
  auto conversion = MatchFloatingPointConversionPattern(instructions_to_process, i);
  if (conversion.has_value()) {
    if (conversion->is_rounding) {
      LiftedStatement statement;
      statement.kind = StatementKind::kAssignment;
      statement.destination_variable = ExpressionBuilder::FpRegisterVarName(
          conversion->dest_floating_point_reg, conversion->is_double);
      std::vector<std::unique_ptr<LiftedExpression>> args;
      args.push_back(LiftFpRegister(conversion->src_floating_point_reg, conversion->is_double));
      statement.expression = LiftedExpression::Call(conversion->round_func, std::move(args));
      statements.push_back(std::move(statement));
    } else if (conversion->is_int_to_float) {
      LiftedStatement statement;
      statement.kind = StatementKind::kAssignment;
      statement.destination_variable = ExpressionBuilder::FpRegisterVarName(
          conversion->dest_floating_point_reg, conversion->is_double);
      if (conversion->src_gpr == Register::kZero) {
        statement.expression = LiftedExpression::Variable(conversion->is_double ? "0.0" : "0.0f");
      } else {
        statement.expression = LiftedExpression::Unary(
            conversion->is_double ? "(f64)" : "(f32)",
            ExpressionBuilder::LiftRegisterOrConstant(conversion->src_gpr, tracker));
      }
      statements.push_back(std::move(statement));
    } else {
      LiftedStatement statement;
      statement.kind = StatementKind::kAssignment;
      statement.destination_variable = ExpressionBuilder::RegisterVarName(conversion->dest_gpr);
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
      RecordGprDefs(instructions_to_process[k]);
      tracker.Step(instructions_to_process[k]);
    }
    i = conversion->end_index;
    return true;
  }

  // If this instruction is the high or mid half of a folded pair, skip emitting it
  if (folder.IsFoldedHi(i) || folder.IsFoldedMid(i)) {
    RecordGprDefs(inst);
    tracker.Step(inst);
    return true;
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
              ExpressionBuilder::LiftRegisterOrConstant(lo->index_reg, tracker));
          pending_int_to_fp[ft_idx] = LiftedExpression::Load("s32", std::move(indexed_addr));
        }
        fpr_const_bits[ft_idx] = std::nullopt;
        RecordGprDefs(inst);
        tracker.Step(inst);
        return true;
      }
    }
    if (inst.opcode == Opcode::kSwc1 && lo->source_float_register.has_value() &&
        (lo->type == FoldedPatternType::kGlobalStore ||
         lo->type == FoldedPatternType::kGlobalIndexedStore)) {
      int ft_idx = static_cast<int>(*lo->source_float_register);
      if (ft_idx >= 0 && ft_idx < 32 && pending_fp_to_int[ft_idx] != nullptr) {
        LiftedStatement statement;
        if (lo->type == FoldedPatternType::kGlobalStore) {
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable =
              lo->symbol_name.empty() ? absl::StrFormat("*(0x%X)", lo->address) : lo->symbol_name;
          statement.expression = std::move(pending_fp_to_int[ft_idx]);
        } else {
          std::unique_ptr<LiftedExpression> base_addr_expr =
              lo->symbol_name.empty()
                  ? LiftedExpression::Integer(lo->address, true)
                  : LiftedExpression::Unary("&", LiftedExpression::GlobalRef(lo->symbol_name));
          auto indexed_addr = LiftedExpression::Binary(
              "+", LiftedExpression::Unary("(u8*)", std::move(base_addr_expr)),
              ExpressionBuilder::LiftRegisterOrConstant(lo->index_reg, tracker));
          statement.kind = StatementKind::kStore;
          statement.store_type = "s32";
          statement.destination_address = std::move(indexed_addr);
          statement.expression = std::move(pending_fp_to_int[ft_idx]);
        }
        statements.push_back(std::move(statement));
        RecordGprDefs(inst);
        tracker.Step(inst);
        return true;
      }
    }
    RecordGprDefs(inst);
    ExpressionBuilder::LiftFoldedAccess(*lo, inst, tracker, statements);
    return true;
  }

  return false;
}

void BlockLiftRunner::LiftIntegerAluInstruction(const Instruction& inst) {
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
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        if (inst.rs == Register::kZero && inst.rt.has_value()) {
          statement.expression = LiftReg(*inst.rt);
        } else if (inst.rt == Register::kZero && inst.rs.has_value()) {
          statement.expression = LiftReg(*inst.rs);
        } else if (inst.rs.has_value() && inst.rt.has_value()) {
          auto lift_non_zero_add_op = [&](Register r) {
            if (r != Register::kZero && tracker.IsConstant(r) && tracker.GetConstant(r) == 0) {
              return LiftedExpression::Variable(ExpressionBuilder::RegisterVarName(r));
            }
            return LiftReg(r);
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
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        if (inst.rs == Register::kZero && inst.rt.has_value()) {
          statement.expression = LiftedExpression::Unary("-", LiftReg(*inst.rt));
        } else if (inst.rs.has_value() && inst.rt.has_value()) {
          statement.expression =
              LiftedExpression::Binary("-", LiftReg(*inst.rs), LiftReg(*inst.rt));
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
          break;
        }
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        if (inst.rs == Register::kZero) {
          statement.expression = LiftedExpression::Integer(inst.immediate);
        } else if ((sp_address_taken || IsCallerHomedScalarAddr(inst.immediate)) &&
                   (inst.rs == Register::kSp || (has_frame_pointer && inst.rs == Register::kFp)) &&
                   inst.immediate >= 0) {
          if (IsSpOffset(inst.immediate)) {
            statement.expression = MakeSpAddr(inst.immediate);
          } else {
            statement.expression = LiftedExpression::Unary(
                "(s32)&",
                LiftedExpression::Variable(ExpressionBuilder::StackVarName(inst.immediate)));
          }
        } else if (inst.immediate == 0 && inst.rs.has_value()) {
          statement.expression = LiftReg(*inst.rs);
        } else if (inst.immediate < 0 && inst.rs.has_value()) {
          statement.expression = LiftedExpression::Binary(
              "-", LiftReg(*inst.rs), LiftedExpression::Integer(-inst.immediate));
        } else if (inst.rs.has_value()) {
          statement.expression = LiftedExpression::Binary(
              "+", LiftReg(*inst.rs), LiftedExpression::Integer(inst.immediate));
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kLui:
      if (inst.rt.has_value() && *inst.rt != Register::kZero) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(inst.immediate)) << 16;
        if (IsFloatCarrierGpr(*inst.rt) && (val & 0x7F800000u) != 0 &&
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
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            "&", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kAndi:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        statement.expression = LiftedExpression::Binary(
            "&", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
            LiftedExpression::Integer(inst.UnsignedImmediate()));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kOr:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
          inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            "|", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kOri:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        statement.expression = LiftedExpression::Binary(
            "|", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
            LiftedExpression::Integer(inst.UnsignedImmediate()));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kXor:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
          inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            "^", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kXori:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        statement.expression = LiftedExpression::Binary(
            "^", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
            LiftedExpression::Integer(inst.UnsignedImmediate()));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kNor:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
          inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        if (*inst.rs == Register::kZero) {
          statement.expression = LiftedExpression::Unary(
              "~", ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker));
        } else if (*inst.rt == Register::kZero) {
          statement.expression = LiftedExpression::Unary(
              "~", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker));
        } else {
          statement.expression = LiftedExpression::Unary(
              "~", LiftedExpression::Binary(
                       "|", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
                       ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker)));
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSll:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            "<<", ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker),
            LiftedExpression::Integer(inst.shift_amount));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSrl:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            ">>",
            LiftedExpression::Unary("(u32)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker)),
            LiftedExpression::Integer(inst.shift_amount));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSra:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            ">>", ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker),
            LiftedExpression::Integer(inst.shift_amount));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSllv:
    case Opcode::kDsllv:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
          inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            "<<", ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSrlv:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
          inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            ">>",
            LiftedExpression::Unary("(u32)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker)),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSrav:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
          inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            ">>", ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kDsll:
    case Opcode::kDsll32:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        uint8_t sa = inst.shift_amount + (inst.opcode == Opcode::kDsll32 ? 32 : 0);
        statement.expression = LiftedExpression::Binary(
            "<<", ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker),
            LiftedExpression::Integer(sa));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kDsrl:
    case Opcode::kDsrl32:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        uint8_t sa = inst.shift_amount + (inst.opcode == Opcode::kDsrl32 ? 32 : 0);
        statement.expression = LiftedExpression::Binary(
            ">>",
            LiftedExpression::Unary("(u64)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker)),
            LiftedExpression::Integer(sa));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kDsra:
    case Opcode::kDsra32:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        uint8_t sa = inst.shift_amount + (inst.opcode == Opcode::kDsra32 ? 32 : 0);
        statement.expression = LiftedExpression::Binary(
            ">>",
            LiftedExpression::Unary("(s64)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker)),
            LiftedExpression::Integer(sa));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kDsrlv:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
          inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            ">>",
            LiftedExpression::Unary("(u64)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker)),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kDsrav:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rt.has_value() &&
          inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            ">>",
            LiftedExpression::Unary("(s64)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker)),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSlt:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
          inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            "<", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
            ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSltu:
      if (inst.rd.has_value() && *inst.rd != Register::kZero && inst.rs.has_value() &&
          inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        statement.expression = LiftedExpression::Binary(
            "<",
            LiftedExpression::Unary("(u32)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker)),
            LiftedExpression::Unary("(u32)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker)));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSlti:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        statement.expression = LiftedExpression::Binary(
            "<", ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker),
            LiftedExpression::Integer(inst.immediate));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSltiu:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        uint32_t uimm = static_cast<uint32_t>(inst.immediate);
        statement.expression = LiftedExpression::Binary(
            "<",
            LiftedExpression::Unary("(u32)",
                                    ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker)),
            LiftedExpression::Integer(uimm, /*hex=*/uimm > 0xFFFF));
        statements.push_back(std::move(statement));
      }
      break;

    default:
      break;
  }
}

void BlockLiftRunner::LiftMultiplyDivideInstruction(const Instruction& inst, size_t i) {
  switch (inst.opcode) {
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
          auto lhs = ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker);
          auto rhs = ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker);
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
                  lo_stmt.destination_variable = ExpressionBuilder::RegisterVarName(*mflo_inst.rd);
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
                  hi_stmt.destination_variable = ExpressionBuilder::RegisterVarName(*mfhi_inst.rd);
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
          auto num = ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker);
          auto den = ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker);
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
          auto num = ExpressionBuilder::LiftRegisterOrConstant(*inst.rs, tracker);
          auto den = ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker);
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
              hi_stmt.destination_variable = ExpressionBuilder::RegisterVarName(*mfhi_inst.rd);
              std::string cast_type = pending_mult->is_unsigned ? "u64)(u32" : "s64";
              auto lhs = ExpressionBuilder::LiftRegisterOrConstant(pending_mult->rs, tracker);
              auto rhs = ExpressionBuilder::LiftRegisterOrConstant(pending_mult->rt, tracker);
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
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        if (pending_mult != nullptr) {
          statement.expression = LiftedExpression::Binary(
              "*", ExpressionBuilder::LiftRegisterOrConstant(pending_mult->rs, tracker),
              ExpressionBuilder::LiftRegisterOrConstant(pending_mult->rt, tracker));
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
              lo_stmt.destination_variable = ExpressionBuilder::RegisterVarName(*mflo_inst.rd);
              lo_stmt.expression = LiftedExpression::Binary(
                  "*", ExpressionBuilder::LiftRegisterOrConstant(pending_mult->rs, tracker),
                  ExpressionBuilder::LiftRegisterOrConstant(pending_mult->rt, tracker));
              statements.push_back(std::move(lo_stmt));
            }
            skipped_instruction_indices.insert(mflo_idx);
          }
        }
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rd);
        if (pending_mult != nullptr) {
          std::string cast_type = pending_mult->is_unsigned ? "u64)(u32" : "s64";
          auto lhs = ExpressionBuilder::LiftRegisterOrConstant(pending_mult->rs, tracker);
          auto rhs = ExpressionBuilder::LiftRegisterOrConstant(pending_mult->rt, tracker);
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

    default:
      break;
  }
}

void BlockLiftRunner::LiftMemoryInstruction(const Instruction& inst, size_t i) {
  switch (inst.opcode) {
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
            break;
          }
          if (ctx.saved_reg_offsets != nullptr) {
            auto it = ctx.saved_reg_offsets->find(*inst.rt);
            if (it != ctx.saved_reg_offsets->end() && it->second == inst.immediate) {
              break;
            }
          }
        }
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        if (is_stack_base) {
          int32_t offset = inst.immediate;
          if (IsSpOffset(offset)) {
            statement.expression = LiftedExpression::Load(load_type, MakeSpAddr(offset));
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
                auto var_expr = LiftedExpression::Variable(ExpressionBuilder::StackVarName(offset));
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
                auto base_var =
                    LiftedExpression::Variable(ExpressionBuilder::StackVarName(base_word_offset));
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
              effective_load_type, MakeRegOffsetExpr(*inst.rs, inst.immediate));
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
    case Opcode::kLwr:
    case Opcode::kLdl:
    case Opcode::kLdr:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && *inst.rt != Register::kSp &&
          (!has_frame_pointer || *inst.rt != Register::kFp) && inst.rs.has_value()) {
        Register base_reg =
            (has_frame_pointer && *inst.rs == Register::kFp) ? Register::kSp : *inst.rs;
        if (pending_lwl != nullptr && pending_lwl->opcode == PartnerUnalignedOpcode(inst.opcode) &&
            pending_lwl->rt == *inst.rt && pending_lwl->rs == base_reg) {
          int16_t left_offset =
              IsLeftUnalignedOpcode(inst.opcode) ? inst.immediate : pending_lwl->offset;
          pending_lwl = nullptr;
          EmitUnalignedLoad(*inst.rt, base_reg, left_offset, Is64BitUnalignedOpcode(inst.opcode));
        } else {
          FlushPendingLwl();
          pending_lwl = std::make_unique<PendingUnalignedAccess>();
          pending_lwl->opcode = inst.opcode;
          pending_lwl->rt = *inst.rt;
          pending_lwl->rs = base_reg;
          pending_lwl->offset = inst.immediate;
        }
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
            break;
          }
          if (ctx.saved_reg_offsets != nullptr) {
            auto it = ctx.saved_reg_offsets->find(*inst.rt);
            if (it != ctx.saved_reg_offsets->end() && it->second == inst.immediate) {
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
        } else if (inst.opcode == Opcode::kSw && IsFloatCarrierGpr(*inst.rt)) {
          store_type = "f32";
        }

        LiftedStatement statement;
        if (is_stack_base) {
          int32_t offset = inst.immediate;
          if (IsSpOffset(offset)) {
            statement.kind = StatementKind::kStore;
            statement.store_type = store_type;
            statement.destination_address = MakeSpAddr(offset);
            statement.expression = LiftReg(*inst.rt);
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
              auto val_expr = LiftReg(*inst.rt);
              if (inst.opcode == Opcode::kSh) {
                statement.expression = LiftedExpression::Unary("(u16)", std::move(val_expr));
              } else if (inst.opcode == Opcode::kSb) {
                statement.expression = LiftedExpression::Unary("(u8)", std::move(val_expr));
              } else {
                statement.expression = std::move(val_expr);
              }
            } else {
              if (ctx.double_parameter_in_a2_a3 && inst.opcode == Opcode::kSw &&
                  *inst.rt == Register::kA3 && !gprs_defined_in_block.contains(Register::kA3) &&
                  offset == static_cast<int32_t>(frame_size + 12)) {
                break;
              }
              statement.kind = StatementKind::kAssignment;
              statement.destination_variable = ExpressionBuilder::StackVarName(offset);
              auto val_expr = LiftReg(*inst.rt);
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
          statement.destination_address = MakeRegOffsetExpr(*inst.rs, inst.immediate);
          statement.expression = LiftReg(*inst.rt);
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSwl:
    case Opcode::kSwr:
    case Opcode::kSdl:
    case Opcode::kSdr:
      if (inst.rt.has_value() && inst.rs.has_value()) {
        Register base_reg =
            (has_frame_pointer && *inst.rs == Register::kFp) ? Register::kSp : *inst.rs;
        if (pending_swl != nullptr && pending_swl->opcode == PartnerUnalignedOpcode(inst.opcode) &&
            pending_swl->rt == *inst.rt && pending_swl->rs == base_reg) {
          int16_t left_offset =
              IsLeftUnalignedOpcode(inst.opcode) ? inst.immediate : pending_swl->offset;
          pending_swl = nullptr;
          EmitUnalignedStore(*inst.rt, base_reg, left_offset, Is64BitUnalignedOpcode(inst.opcode));
        } else {
          FlushPendingSwl();
          pending_swl = std::make_unique<PendingUnalignedAccess>();
          pending_swl->opcode = inst.opcode;
          pending_swl->rt = *inst.rt;
          pending_swl->rs = base_reg;
          pending_swl->offset = inst.immediate;
        }
      }
      break;

    default:
      break;
  }
}

void BlockLiftRunner::EmitPendingReturn() {
  if (!pending_return) {
    return;
  }
  LiftedStatement ret;
  ret.kind = StatementKind::kReturn;

  int last_def_v0 = -1;
  bool v0_used_after = false;
  for (size_t i = 0; i < raw_instructions.size(); ++i) {
    if (raw_instructions[i].opcode == Opcode::kJal || raw_instructions[i].opcode == Opcode::kJalr) {
      last_def_v0 = -1;
      v0_used_after = false;
      continue;
    }
    RegisterUseDef ud = GetInstructionUseDef(raw_instructions[i]);
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
  for (size_t i = 0; i < raw_instructions.size(); ++i) {
    if (raw_instructions[i].opcode == Opcode::kJal || raw_instructions[i].opcode == Opcode::kJalr) {
      last_def_f0 = -1;
      f0_used_after = false;
      continue;
    }
    RegisterUseDef ud = GetInstructionUseDef(raw_instructions[i]);
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

  if (has_live_f0 && (!has_live_v0 || last_def_f0 > last_def_v0)) {
    bool is_double = IsDoublePrecisionWriteOp(raw_instructions[last_def_f0].opcode);
    ret.expression = LiftedExpression::Variable(is_double ? "f0_d" : "f0");
  } else if (has_live_v0) {
    ret.expression = LiftedExpression::Variable("v0");
  } else if (has_live_f0) {
    bool is_double = IsDoublePrecisionWriteOp(raw_instructions[last_def_f0].opcode);
    ret.expression = LiftedExpression::Variable(is_double ? "f0_d" : "f0");
  }
  statements.push_back(std::move(ret));
}

std::vector<LiftedStatement> BlockLiftRunner::Run() {
  if (raw_instructions.empty()) {
    return std::move(statements);
  }

  for (size_t i = 0; i < loop_end; ++i) {
    const auto& inst = instructions_to_process[i];

    if (pending_lwl != nullptr && inst.opcode != PartnerUnalignedOpcode(pending_lwl->opcode)) {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      bool touches_rt = false;
      for (Register r : ud.gpr_uses) {
        if (r == pending_lwl->rt) touches_rt = true;
      }
      for (Register r : ud.gpr_defs) {
        if (r == pending_lwl->rt) touches_rt = true;
      }
      if (touches_rt || IsUnalignedLoadOpcode(inst.opcode) || inst.IsBranch() ||
          inst.opcode == Opcode::kJ || inst.opcode == Opcode::kJr || inst.opcode == Opcode::kJal ||
          inst.opcode == Opcode::kJalr) {
        FlushPendingLwl();
      }
    }

    if (inst.opcode != Opcode::kMtc1) {
      InvalidateNonMtc1FprConstantBits(inst, fpr_const_bits);
      if (inst.IsCall()) {
        for (int k = 0; k < 20; ++k) {
          pending_int_to_fp[k] = nullptr;
          pending_fp_to_int[k] = nullptr;
        }
      }
    }

    if (TryLiftMultiInstructionPatterns(i)) {
      continue;
    }

    if (inst.IsNop()) {
      tracker.Step(inst);
      continue;
    }

    if (pending_swl != nullptr && inst.opcode != PartnerUnalignedOpcode(pending_swl->opcode)) {
      FlushPendingSwl();
    }

    switch (inst.opcode) {
      case Opcode::kAddu:
      case Opcode::kAdd:
      case Opcode::kDaddu:
      case Opcode::kDadd:
      case Opcode::kSubu:
      case Opcode::kSub:
      case Opcode::kDsubu:
      case Opcode::kDsub:
      case Opcode::kAddiu:
      case Opcode::kAddi:
      case Opcode::kDaddiu:
      case Opcode::kDaddi:
      case Opcode::kLui:
      case Opcode::kAnd:
      case Opcode::kAndi:
      case Opcode::kOr:
      case Opcode::kOri:
      case Opcode::kXor:
      case Opcode::kXori:
      case Opcode::kNor:
      case Opcode::kSll:
      case Opcode::kSrl:
      case Opcode::kSra:
      case Opcode::kSllv:
      case Opcode::kSrlv:
      case Opcode::kSrav:
      case Opcode::kDsll:
      case Opcode::kDsll32:
      case Opcode::kDsrl:
      case Opcode::kDsrl32:
      case Opcode::kDsra:
      case Opcode::kDsra32:
      case Opcode::kDsllv:
      case Opcode::kDsrlv:
      case Opcode::kDsrav:
      case Opcode::kSlt:
      case Opcode::kSltu:
      case Opcode::kSlti:
      case Opcode::kSltiu:
        LiftIntegerAluInstruction(inst);
        break;

      case Opcode::kMult:
      case Opcode::kMultu:
      case Opcode::kDmult:
      case Opcode::kDmultu:
      case Opcode::kDiv:
      case Opcode::kDivu:
      case Opcode::kDdiv:
      case Opcode::kDdivu:
      case Opcode::kMflo:
      case Opcode::kMfhi:
      case Opcode::kMtlo:
      case Opcode::kMthi:
        LiftMultiplyDivideInstruction(inst, i);
        break;

      case Opcode::kLd:
      case Opcode::kLld:
      case Opcode::kLw:
      case Opcode::kLwu:
      case Opcode::kLh:
      case Opcode::kLb:
      case Opcode::kLhu:
      case Opcode::kLbu:
      case Opcode::kLwl:
      case Opcode::kLwr:
      case Opcode::kLdl:
      case Opcode::kLdr:
      case Opcode::kSd:
      case Opcode::kScd:
      case Opcode::kSw:
      case Opcode::kSh:
      case Opcode::kSb:
      case Opcode::kSwl:
      case Opcode::kSwr:
      case Opcode::kSdl:
      case Opcode::kSdr:
        LiftMemoryInstruction(inst, i);
        break;

      case Opcode::kJal:
      case Opcode::kJalr:
      case Opcode::kBltzal:
      case Opcode::kBgezal:
      case Opcode::kBltzall:
      case Opcode::kBgezall:
        rom_nom_nom::LiftCallInstruction(
            inst, i, instructions_to_process, ctx.symbol_index, ctx.function_parameter_counts,
            ctx.function_fp_parameter_counts, ctx.functions_returning_v0,
            ctx.functions_returning_f0, tracker, statements, sp_address_taken, ctx.call_return_info,
            sp_base_offset, ctx.saved_reg_offsets, ctx.cfg, ctx.current_block_id);
        break;

      case Opcode::kJr:
        if (inst.rs == Register::kRa) {
          pending_return = true;
        }
        break;

      case Opcode::kLwc1:
      case Opcode::kSwc1:
      case Opcode::kLdc1:
      case Opcode::kSdc1:
      case Opcode::kMfc1:
      case Opcode::kDmfc1:
      case Opcode::kMtc1:
      case Opcode::kDmtc1:
      case Opcode::kCfc1:
      case Opcode::kCtc1: {
        Cop1LiftState cop1_state = MakeCop1State();
        rom_nom_nom::LiftCop1MemoryAndMoveInstruction(inst, i, cop1_state);
        break;
      }

      case Opcode::kMfc0:
        if (inst.rt.has_value() && *inst.rt != Register::kZero) {
          LiftedStatement statement;
          statement.kind = StatementKind::kAssignment;
          statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
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
      case Opcode::kAddD:
      case Opcode::kSubD:
      case Opcode::kMulD:
      case Opcode::kDivD:
      case Opcode::kSqrtS:
      case Opcode::kSqrtD:
      case Opcode::kAbsS:
      case Opcode::kAbsD:
      case Opcode::kMovS:
      case Opcode::kMovD:
      case Opcode::kNegS:
      case Opcode::kNegD:
      case Opcode::kCvtSD:
      case Opcode::kCvtSW:
      case Opcode::kCvtDS:
      case Opcode::kCvtDW:
      case Opcode::kCvtWS:
      case Opcode::kCvtWD:
      case Opcode::kCvtLS:
      case Opcode::kCvtLD:
      case Opcode::kCvtSL:
      case Opcode::kCvtDL:
      case Opcode::kTruncWS:
      case Opcode::kTruncWD:
      case Opcode::kRoundWS:
      case Opcode::kRoundWD:
      case Opcode::kCeilWS:
      case Opcode::kCeilWD:
      case Opcode::kFloorWS:
      case Opcode::kFloorWD: {
        Cop1LiftState cop1_state = MakeCop1State();
        rom_nom_nom::LiftCop1ArithmeticAndConvertInstruction(inst, i, cop1_state);
        break;
      }

      case Opcode::kCFS:
      case Opcode::kCSfS:
      case Opcode::kCFD:
      case Opcode::kCSfD:
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
        rom_nom_nom::LiftCop1ComparisonInstruction(inst, statements);
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

    RecordGprDefs(inst);
    tracker.Step(inst);
  }

  FlushPendingLwl();
  FlushPendingSwl();
  EmitPendingReturn();
  return std::move(statements);
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
  if (lo.type == FoldedPatternType::kAddressLoad && !lo.symbol_name.empty()) {
    tracker.SetRegisterValue(lo.dest_reg, TrackedValue());
  }
}

std::vector<LiftedStatement> ExpressionBuilder::LiftBlock(const BasicBlock& block,
                                                          const FunctionLiftContext& ctx,
                                                          bool omit_last_instruction) {
  FunctionLiftContext block_ctx = ctx;
  block_ctx.current_block_id = block.id;
  return LiftInstructions(block.instructions, block_ctx, omit_last_instruction);
}

std::vector<LiftedStatement> ExpressionBuilder::LiftInstructions(
    absl::Span<const Instruction> instructions, const FunctionLiftContext& ctx,
    bool omit_last_instruction) {
  BlockLiftRunner runner(instructions, ctx, omit_last_instruction);
  return runner.Run();
}

}  // namespace rom_nom_nom
