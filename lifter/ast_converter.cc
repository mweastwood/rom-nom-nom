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
#include "lifter/register_tracker.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

absl::flat_hash_set<std::string> AstConverter::IdentifyDoubleVariables(
    const ControlFlowGraph& cfg) {
  absl::flat_hash_set<std::string> double_vars;
  for (const auto& block : cfg.Blocks()) {
    for (const auto& inst : block.instructions) {
      switch (inst.opcode) {
        case Opcode::kAddD:
        case Opcode::kSubD:
        case Opcode::kMulD:
        case Opcode::kDivD:
        case Opcode::kSqrtD:
        case Opcode::kAbsD:
        case Opcode::kMovD:
        case Opcode::kNegD:
        case Opcode::kCEqD:
        case Opcode::kCLtD:
        case Opcode::kCLeD:
          if (inst.fd) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fd, true));
          if (inst.fs) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fs, true));
          if (inst.ft) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.ft, true));
          break;
        case Opcode::kLdc1:
        case Opcode::kSdc1:
          if (inst.ft) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.ft, true));
          if (inst.rs.has_value() && (*inst.rs == Register::kSp || *inst.rs == Register::kFp)) {
            if (inst.immediate < 0) {
              double_vars.insert(absl::StrFormat("var_sp_neg_%d", -inst.immediate));
            } else {
              double_vars.insert(absl::StrFormat("var_sp_%d", inst.immediate));
            }
          }
          break;
        case Opcode::kDmfc1:
        case Opcode::kDmtc1:
          if (inst.fs) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fs, true));
          break;
        case Opcode::kCvtDS:
        case Opcode::kCvtDW:
          if (inst.fd) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fd, true));
          break;
        case Opcode::kCvtSD:
        case Opcode::kTruncWD:
          if (inst.fs) double_vars.insert(ExpressionBuilder::FpRegisterVarName(*inst.fs, true));
          break;
        default:
          break;
      }
    }
  }
  return double_vars;
}

bool AstConverter::IsFpParameterDouble(const ControlFlowGraph& cfg, FpRegister reg) {
  if (cfg.Blocks().empty()) return false;
  std::vector<size_t> queue = {0};
  absl::flat_hash_set<size_t> visited = {0};

  while (!queue.empty()) {
    size_t curr = queue.front();
    queue.erase(queue.begin());
    const auto& block = cfg.Blocks()[curr];
    for (const auto& inst : block.instructions) {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (FpRegister u : ud.fpr_uses) {
        if (u == reg) {
          Opcode op = inst.opcode;
          return (op == Opcode::kAddD || op == Opcode::kSubD || op == Opcode::kMulD ||
                  op == Opcode::kDivD || op == Opcode::kSqrtD || op == Opcode::kAbsD ||
                  op == Opcode::kMovD || op == Opcode::kNegD || op == Opcode::kCvtSD ||
                  op == Opcode::kTruncWD || op == Opcode::kRoundWD || op == Opcode::kCeilWD ||
                  op == Opcode::kFloorWD || op == Opcode::kCEqD || op == Opcode::kCLtD ||
                  op == Opcode::kCLeD || op == Opcode::kSdc1);
        }
      }
      for (FpRegister d : ud.fpr_defs) {
        if (d == reg) {
          goto next_block;
        }
      }
    }
    for (uint32_t succ_id : block.successors) {
      auto it = std::find_if(cfg.Blocks().begin(), cfg.Blocks().end(),
                             [succ_id](const BasicBlock& b) { return b.id == succ_id; });
      if (it != cfg.Blocks().end()) {
        size_t succ_idx = std::distance(cfg.Blocks().begin(), it);
        if (visited.insert(succ_idx).second) {
          queue.push_back(succ_idx);
        }
      }
    }
  next_block:;
  }
  return false;
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
      case Opcode::kBc1t:
      case Opcode::kBc1tl:
        branch_taken_on_nonzero = true;
        break;
      case Opcode::kBc1f:
      case Opcode::kBc1fl:
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

      case Opcode::kCEqS:
      case Opcode::kCEqD: {
        bool is_double = (def_inst.opcode == Opcode::kCEqD);
        std::string op = want_true ? "==" : "!=";
        return CExpression::Binary(op,
                                   CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                       def_inst.fs.value_or(FpRegister::kF0), is_double)),
                                   CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                       def_inst.ft.value_or(FpRegister::kF0), is_double)));
      }

      case Opcode::kCLtS:
      case Opcode::kCLtD: {
        bool is_double = (def_inst.opcode == Opcode::kCLtD);
        auto expr =
            CExpression::Binary("<",
                                CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                    def_inst.fs.value_or(FpRegister::kF0), is_double)),
                                CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                    def_inst.ft.value_or(FpRegister::kF0), is_double)));
        if (!want_true) {
          return CExpression::Unary("!", std::move(expr));
        }
        return expr;
      }

      case Opcode::kCLeS:
      case Opcode::kCLeD: {
        bool is_double = (def_inst.opcode == Opcode::kCLeD);
        auto expr =
            CExpression::Binary("<=",
                                CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                    def_inst.fs.value_or(FpRegister::kF0), is_double)),
                                CExpression::Identifier(ExpressionBuilder::FpRegisterVarName(
                                    def_inst.ft.value_or(FpRegister::kF0), is_double)));
        if (!want_true) {
          return CExpression::Unary("!", std::move(expr));
        }
        return expr;
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
                   const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
                   bool returns_v0, bool returns_f0,
                   const absl::flat_hash_set<std::string>* functions_returning_v0 = nullptr,
                   const absl::flat_hash_set<std::string>* functions_returning_f0 = nullptr,
                   std::string function_name = "")
      : cfg_(cfg),
        symbol_index_(symbol_index),
        split_config_(split_config),
        function_parameter_counts_(function_parameter_counts),
        function_fp_parameter_counts_(function_fp_parameter_counts),
        returns_v0_(returns_v0),
        returns_f0_(returns_f0),
        functions_returning_v0_(functions_returning_v0),
        functions_returning_f0_(functions_returning_f0),
        function_name_(std::move(function_name)),
        double_vars_(AstConverter::IdentifyDoubleVariables(cfg_)) {
    absl::flat_hash_map<int32_t, int> stack_access_sizes;
    for (const auto& block : cfg_.Blocks()) {
      for (const auto& inst : block.instructions) {
        if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
            inst.rt.has_value() && *inst.rt == Register::kSp && inst.rs == Register::kSp &&
            inst.immediate < 0) {
          frame_size_ = std::max(frame_size_, static_cast<uint32_t>(-inst.immediate));
        }
        if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd) && inst.rd.has_value() &&
            *inst.rd == Register::kFp &&
            ((inst.rs == Register::kSp && inst.rt == Register::kZero) ||
             (inst.rs == Register::kZero && inst.rt == Register::kSp))) {
          has_frame_pointer_ = true;
        }
        if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
            inst.rt.has_value() && *inst.rt == Register::kFp && inst.rs == Register::kSp &&
            inst.immediate == 0) {
          has_frame_pointer_ = true;
        }
        if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr) {
          has_calls_ = true;
        }
        if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
            (inst.rs == Register::kSp || (has_frame_pointer_ && inst.rs == Register::kFp)) &&
            inst.rt.has_value() && *inst.rt != Register::kSp && *inst.rt != Register::kFp &&
            inst.immediate >= 0) {
          sp_address_taken_ = true;
        }
        if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd ||
             inst.opcode == Opcode::kOr) &&
            (inst.rs == Register::kSp || inst.rt == Register::kSp ||
             (has_frame_pointer_ && (inst.rs == Register::kFp || inst.rt == Register::kFp))) &&
            inst.rd.has_value() && *inst.rd != Register::kSp &&
            (!has_frame_pointer_ || *inst.rd != Register::kFp)) {
          sp_address_taken_ = true;
        }
        if ((inst.opcode == Opcode::kLwl || inst.opcode == Opcode::kLwr ||
             inst.opcode == Opcode::kSwl || inst.opcode == Opcode::kSwr) &&
            (inst.rs == Register::kSp || (has_frame_pointer_ && inst.rs == Register::kFp))) {
          sp_address_taken_ = true;
        }
        if (inst.rs == Register::kSp || (has_frame_pointer_ && inst.rs == Register::kFp)) {
          if (inst.IsStore()) {
            stack_stores_.insert(inst.immediate);
          }
          int size = 0;
          if (inst.opcode == Opcode::kLw || inst.opcode == Opcode::kSw) {
            size = 4;
          } else if (inst.opcode == Opcode::kLh || inst.opcode == Opcode::kLhu ||
                     inst.opcode == Opcode::kSh) {
            size = 2;
          } else if (inst.opcode == Opcode::kLb || inst.opcode == Opcode::kLbu ||
                     inst.opcode == Opcode::kSb) {
            size = 1;
          }
          if (size > 0) {
            auto [it, inserted] = stack_access_sizes.try_emplace(inst.immediate, size);
            if (!inserted && it->second != size) {
              sp_address_taken_ = true;
            }
          }
        }
      }
    }
    if (!cfg_.Blocks().empty()) {
      const auto& entry_block = cfg_.Blocks()[0];
      absl::flat_hash_set<Register> defined_gprs;
      absl::flat_hash_set<FpRegister> defined_fprs;
      for (const auto& inst : entry_block.instructions) {
        if (inst.opcode == Opcode::kSw &&
            (inst.rs == Register::kSp || (has_frame_pointer_ && inst.rs == Register::kFp)) &&
            inst.rt.has_value() && inst.immediate > 0) {
          Register r = *inst.rt;
          if (((r >= Register::kS0 && r <= Register::kS7) || r == Register::kFp ||
               r == Register::kRa) &&
              !defined_gprs.contains(r)) {
            saved_reg_offsets_[r] = inst.immediate;
          }
        }
        if ((inst.opcode == Opcode::kSwc1 || inst.opcode == Opcode::kSdc1) &&
            (inst.rs == Register::kSp || (has_frame_pointer_ && inst.rs == Register::kFp)) &&
            inst.ft.has_value() && inst.immediate > 0) {
          FpRegister fr = *inst.ft;
          if (fr >= FpRegister::kF20 && fr <= FpRegister::kF31 && !defined_fprs.contains(fr)) {
            saved_fpr_offsets_[fr] = inst.immediate;
          }
        }
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register d : ud.gpr_defs) {
          if (d != Register::kZero) defined_gprs.insert(d);
        }
        for (FpRegister fd : ud.fpr_defs) {
          defined_fprs.insert(fd);
        }
      }
    }
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
    call_return_info_ = ExpressionBuilder::ComputeCallReturnInfo(cfg_, returns_v0_, returns_f0_);
  }

  const absl::flat_hash_set<uint32_t>& GotoTargets() const { return goto_targets_; }
  const absl::flat_hash_set<uint32_t>& EmittedLabels() const { return emitted_labels_; }

  std::vector<LiftedStatement> LiftInstructionsHelper(
      absl::Span<const Instruction> instructions) const {
    return ExpressionBuilder::LiftInstructions(
        instructions, symbol_index_, split_config_, function_parameter_counts_,
        function_fp_parameter_counts_, /*omit_last_instruction=*/false, frame_size_,
        functions_returning_v0_, functions_returning_f0_, has_frame_pointer_, has_calls_,
        sp_address_taken_, &saved_reg_offsets_, &saved_fpr_offsets_, nullptr, nullptr,
        &call_return_info_, &stack_stores_);
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

        if (!region.children.empty() && region.children[0]) {
          ConvertRegion(*region.children[0], then_body.get());
        }

        if (!else_body->Statements().empty()) {
          target_block->AddStatement(
              CStatement::If(std::move(cond), std::move(then_body), std::move(else_body)));
        } else {
          target_block->AddStatement(CStatement::If(std::move(cond), std::move(then_body)));
        }
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
        bool decouple_delay = ShouldDecoupleBranchConditionDelaySlot(region.condition_block_id);

        EmitBlockStatements(region.condition_block_id, target_block,
                            /*omit_last_instruction=*/has_branch_likely || decouple_delay);
        auto cond = cond_block != nullptr
                        ? AstConverter::ExtractBranchCondition(*cond_block, region.invert_condition)
                        : CExpression::Integer(1);

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

          if ((has_branch_likely || delay_clobbers_cond) && cond_block != nullptr &&
              cond_block->instructions.size() >= 2) {
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
                auto cs = AstConverter::ConvertStatement(s);
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
                auto cs = AstConverter::ConvertStatement(s);
                if (cs != nullptr) {
                  loop_body->AddStatement(std::move(cs));
                }
              }
            }
            target_block->AddStatement(
                CStatement::While(CExpression::Integer(1), std::move(loop_body)));
          } else {
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

          if (!pre_cond_stmts.empty()) {
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

          if (sc.is_default && jt != nullptr && jt->bounds_branch_vram != 0) {
            const auto* bounds_block = cfg_.FindBlockByVram(jt->bounds_branch_vram);
            if (bounds_block != nullptr) {
              const auto* term = bounds_block->Terminator();
              if (term != nullptr && term->IsBranchLikely() &&
                  bounds_block->instructions.size() >= 2) {
                const auto& delay_inst = bounds_block->instructions.back();
                if (!delay_inst.IsNop()) {
                  auto delay_stmts = LiftInstructionsHelper(absl::MakeSpan(&delay_inst, 1));
                  for (auto& s : delay_stmts) {
                    auto cs = AstConverter::ConvertStatement(s);
                    if (cs != nullptr) {
                      c_case.body->AddStatement(std::move(cs));
                    }
                  }
                }
              }
            }
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

  bool ShouldDecoupleBranchConditionDelaySlot(uint32_t cond_block_id) const {
    const auto* cond_block = cfg_.GetBlock(cond_block_id);
    if (cond_block == nullptr || cond_block->instructions.size() < 2) {
      return false;
    }
    const auto* term = cond_block->Terminator();
    if (term == nullptr || !term->IsBranch() || term->IsBranchLikely()) {
      return false;
    }
    const auto& delay_inst = cond_block->instructions.back();
    if (delay_inst.IsNop()) {
      return false;
    }

    RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
    if (delay_ud.gpr_defs.empty() && delay_ud.fpr_defs.empty()) {
      return false;
    }

    absl::flat_hash_set<Register> cond_gprs;
    absl::flat_hash_set<FpRegister> cond_fprs;
    if (term->rs.has_value() && *term->rs != Register::kZero) {
      cond_gprs.insert(*term->rs);
    }
    if (term->rt.has_value() && *term->rt != Register::kZero) {
      cond_gprs.insert(*term->rt);
    }
    auto folded_comp = ExpressionBuilder::FindFoldedComparison(cond_block->instructions);
    if (folded_comp.has_value() &&
        folded_comp->instruction_index < cond_block->instructions.size()) {
      const auto& def_inst = cond_block->instructions[folded_comp->instruction_index];
      RegisterUseDef def_ud = GetInstructionUseDef(def_inst);
      for (Register r : def_ud.gpr_uses) {
        if (r != Register::kZero) {
          cond_gprs.insert(r);
        }
      }
      for (FpRegister f : def_ud.fpr_uses) {
        cond_fprs.insert(f);
      }
    }
    for (Register r : delay_ud.gpr_defs) {
      if (cond_gprs.contains(r)) {
        return true;
      }
    }
    for (FpRegister f : delay_ud.fpr_defs) {
      if (cond_fprs.contains(f)) {
        return true;
      }
    }
    return false;
  }

  bool ShouldDecoupleLoopConditionDelaySlot(uint32_t cond_block_id,
                                            uint32_t header_block_id) const {
    const auto* cond_block = cfg_.GetBlock(cond_block_id);
    if (cond_block == nullptr || cond_block->instructions.size() < 2) {
      return false;
    }
    const auto* term = cond_block->Terminator();
    if (term == nullptr || !term->IsBranch() || term->IsBranchLikely()) {
      return false;
    }
    const auto& delay_inst = cond_block->instructions.back();
    if (delay_inst.IsNop()) {
      return false;
    }

    RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
    if (delay_ud.gpr_defs.empty()) {
      return false;
    }

    // Check 1: Does delay_inst define a register used by the branch condition or its folded
    // comparison?
    absl::flat_hash_set<Register> cond_regs;
    if (term->rs.has_value() && *term->rs != Register::kZero) {
      cond_regs.insert(*term->rs);
    }
    if (term->rt.has_value() && *term->rt != Register::kZero) {
      cond_regs.insert(*term->rt);
    }
    auto folded_comp = ExpressionBuilder::FindFoldedComparison(cond_block->instructions);
    if (folded_comp.has_value() &&
        folded_comp->instruction_index < cond_block->instructions.size()) {
      const auto& def_inst = cond_block->instructions[folded_comp->instruction_index];
      RegisterUseDef def_ud = GetInstructionUseDef(def_inst);
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
    // Check: Are all registers defined by delay_inst killed at header_block before being used?
    const auto* header_block = cfg_.GetBlock(header_block_id);
    if (header_block != nullptr) {
      bool all_killed = true;
      for (Register def_reg : delay_ud.gpr_defs) {
        if (def_reg == Register::kZero) continue;
        bool killed = false;
        for (const auto& inst : header_block->instructions) {
          RegisterUseDef inst_ud = GetInstructionUseDef(inst);
          bool used = false;
          for (Register u : inst_ud.gpr_uses) {
            if (u == def_reg) {
              used = true;
              break;
            }
          }
          if (used) {
            break;
          }
          bool defined = false;
          for (Register d : inst_ud.gpr_defs) {
            if (d == def_reg) {
              defined = true;
              break;
            }
          }
          if (defined) {
            killed = true;
            break;
          }
        }
        if (!killed) {
          all_killed = false;
          break;
        }
      }
      if (all_killed) {
        return true;
      }
    }

    return false;
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

    bool should_omit =
        (omit_last_instruction || omit_last_instruction_blocks_.contains(block_id)) &&
        filtered_block.instructions.size() > 1;
    const std::array<std::optional<uint32_t>, 32>* initial_fpr = nullptr;
    const RegisterTracker* initial_tracker = nullptr;
    auto it_state = block_entry_states_.find(block_id);
    if (it_state != block_entry_states_.end()) {
      initial_fpr = &it_state->second.fpr_const_bits;
      initial_tracker = &it_state->second.tracker;
    }

    auto lifted_stmts = ExpressionBuilder::LiftBlock(
        filtered_block, symbol_index_, split_config_, function_parameter_counts_,
        function_fp_parameter_counts_, should_omit, frame_size_, functions_returning_v0_,
        functions_returning_f0_, has_frame_pointer_, has_calls_, sp_address_taken_,
        &saved_reg_offsets_, &saved_fpr_offsets_, initial_fpr, initial_tracker, &call_return_info_,
        &stack_stores_);
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
                for (int r = 0; r < 32; ++r) {
                  Register reg = static_cast<Register>(r);
                  if (entry_state.tracker.GetRegisterValue(reg) !=
                      pred_state->tracker.GetRegisterValue(reg)) {
                    entry_state.tracker.Invalidate(reg);
                  }
                }
              }
            }
          }
        }

        // Simulate block instructions to compute exit state
        BlockState exit_state = entry_state;
        std::optional<BlockState> exit_fallthrough_state;
        const auto* term = block->Terminator();
        bool is_likely = (term != nullptr && term->IsBranchLikely());

        for (size_t idx = 0; idx < block->instructions.size(); ++idx) {
          const auto& inst = block->instructions[idx];
          if (is_likely && idx + 1 == block->instructions.size()) {
            exit_fallthrough_state = exit_state;
          }
          if (inst.opcode != Opcode::kMtc1) {
            RegisterUseDef ud = GetInstructionUseDef(inst);
            for (FpRegister d : ud.fpr_defs) {
              int f_idx = static_cast<int>(d);
              exit_state.fpr_const_bits[f_idx] = std::nullopt;
              if ((f_idx % 2) == 0 && f_idx + 1 < 32) {
                exit_state.fpr_const_bits[f_idx + 1] = std::nullopt;
              }
            }
          } else if (inst.fs.has_value() && inst.rt.has_value()) {
            int fs_idx = static_cast<int>(*inst.fs);
            if (*inst.rt == Register::kZero) {
              exit_state.fpr_const_bits[fs_idx] = 0;
            } else {
              TrackedValue tv = exit_state.tracker.GetRegisterValue(*inst.rt);
              if (tv.kind == ValueKind::kConstant) {
                exit_state.fpr_const_bits[fs_idx] = tv.constant_value;
              } else if (tv.kind == ValueKind::kSymbolHi) {
                exit_state.fpr_const_bits[fs_idx] = tv.symbol_hi;
              } else {
                exit_state.fpr_const_bits[fs_idx] = std::nullopt;
              }
            }
          }
          exit_state.tracker.Step(inst);
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
  const SplitConfig* split_config_;
  const absl::flat_hash_map<std::string, int>* function_parameter_counts_;
  const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts_;
  bool returns_v0_ = false;
  bool returns_f0_ = false;
  const absl::flat_hash_set<std::string>* functions_returning_v0_ = nullptr;
  const absl::flat_hash_set<std::string>* functions_returning_f0_ = nullptr;
  std::string function_name_;
  absl::flat_hash_set<std::string> double_vars_;
  uint32_t frame_size_ = 0;
  bool has_frame_pointer_ = false;
  bool has_calls_ = false;
  bool sp_address_taken_ = false;
  absl::flat_hash_map<Register, int32_t> saved_reg_offsets_;
  absl::flat_hash_map<FpRegister, int32_t> saved_fpr_offsets_;
  absl::flat_hash_set<int32_t> stack_stores_;
  absl::flat_hash_set<uint32_t> goto_targets_;
  absl::flat_hash_set<uint32_t> emitted_labels_;
  absl::flat_hash_set<uint32_t> emitted_blocks_;
  absl::flat_hash_set<uint32_t> omit_last_instruction_blocks_;
  absl::flat_hash_set<uint32_t> setup_instruction_vrams_;
  absl::flat_hash_map<uint32_t, BlockState> block_entry_states_;
  absl::flat_hash_map<uint32_t, CallReturnInfo> call_return_info_;
};

}  // namespace

FunctionDeclaration AstConverter::Convert(const ControlFlowGraph& cfg,
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

  ConverterContext ctx(cfg, symbol_index, root_region, options.split_config,
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
    absl::flat_hash_set<std::string> double_vars = IdentifyDoubleVariables(cfg);

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
        parameters.push_back(
            CParameter{.type = CType::S32(), .name = absl::StrFormat("arg%d", slot)});
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
    uint32_t detected_frame_size = 0;
    bool has_calls = false;
    bool has_frame_pointer = false;
    uint32_t min_saved_reg_offset = 0xFFFFFFFF;
    if (!cfg.Blocks().empty()) {
      const auto& entry_block = cfg.Blocks()[0];
      absl::flat_hash_set<Register> defined_gprs;
      absl::flat_hash_set<FpRegister> defined_fprs;
      for (const auto& inst : entry_block.instructions) {
        if (inst.opcode == Opcode::kSw && inst.rs == Register::kSp && inst.rt.has_value() &&
            inst.immediate > 0) {
          Register reg = *inst.rt;
          if ((reg == Register::kRa || reg == Register::kFp ||
               (reg >= Register::kS0 && reg <= Register::kS7)) &&
              !defined_gprs.contains(reg)) {
            min_saved_reg_offset =
                std::min(min_saved_reg_offset, static_cast<uint32_t>(inst.immediate));
          }
        }
        if ((inst.opcode == Opcode::kSwc1 || inst.opcode == Opcode::kSdc1) &&
            inst.rs == Register::kSp && inst.ft.has_value() && inst.immediate > 0) {
          FpRegister fr = *inst.ft;
          if (fr >= FpRegister::kF20 && fr <= FpRegister::kF31 && !defined_fprs.contains(fr)) {
            min_saved_reg_offset =
                std::min(min_saved_reg_offset, static_cast<uint32_t>(inst.immediate));
          }
        }
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register d : ud.gpr_defs) {
          if (d != Register::kZero) defined_gprs.insert(d);
        }
        for (FpRegister fd : ud.fpr_defs) {
          defined_fprs.insert(fd);
        }
      }
    }
    for (const auto& block : cfg.Blocks()) {
      for (const auto& inst : block.instructions) {
        if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
            inst.rt.has_value() && *inst.rt == Register::kSp && inst.rs == Register::kSp &&
            inst.immediate < 0) {
          detected_frame_size =
              std::max(detected_frame_size, static_cast<uint32_t>(-inst.immediate));
        }
        if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd) && inst.rd.has_value() &&
            *inst.rd == Register::kFp &&
            ((inst.rs == Register::kSp && inst.rt == Register::kZero) ||
             (inst.rs == Register::kZero && inst.rt == Register::kSp))) {
          has_frame_pointer = true;
        }
        if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr) {
          has_calls = true;
        }
      }
    }
    if (detected_frame_size == 0) {
      detected_frame_size = 64;
    }
    if (min_saved_reg_offset == 0xFFFFFFFF) {
      min_saved_reg_offset = detected_frame_size;
    }
    uint32_t sp_base_offset = (has_calls && detected_frame_size > 16) ? 16 : 0;
    uint32_t sp_buffer_size = (min_saved_reg_offset > sp_base_offset)
                                  ? (min_saved_reg_offset - sp_base_offset)
                                  : detected_frame_size;

    uint32_t max_sp_access = 0;
    for (const auto& block : cfg.Blocks()) {
      for (const auto& inst : block.instructions) {
        if (inst.rs == Register::kSp || (has_frame_pointer && inst.rs == Register::kFp)) {
          if (inst.rt.has_value() &&
              (*inst.rt == Register::kRa || (has_frame_pointer && *inst.rt == Register::kFp) ||
               (*inst.rt >= Register::kS0 && *inst.rt <= Register::kS7))) {
            continue;
          }
          if ((inst.IsLoad() || inst.IsStore()) && inst.immediate > 0) {
            int32_t adj =
                static_cast<int32_t>(inst.immediate) - static_cast<int32_t>(sp_base_offset);
            if (adj > 0) {
              uint32_t access_size =
                  (inst.opcode == Opcode::kSdc1 || inst.opcode == Opcode::kLdc1 ||
                   inst.opcode == Opcode::kSd || inst.opcode == Opcode::kLd)
                      ? 8
                      : 4;
              max_sp_access = std::max(max_sp_access, static_cast<uint32_t>(adj + access_size));
            }
          } else if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
                     inst.rt.has_value() && *inst.rt != Register::kSp &&
                     (!has_frame_pointer || *inst.rt != Register::kFp) && inst.immediate > 0) {
            int32_t adj =
                static_cast<int32_t>(inst.immediate) - static_cast<int32_t>(sp_base_offset);
            if (adj > 0) {
              max_sp_access = std::max(max_sp_access, static_cast<uint32_t>(adj + 4));
            }
          }
        }
      }
    }
    if (max_sp_access > sp_buffer_size) {
      sp_buffer_size = (max_sp_access + 15) & ~15;
    }

    absl::flat_hash_set<std::string> double_vars = IdentifyDoubleVariables(cfg);

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
    absl::flat_hash_set<Register> entry_defined_gprs;
    if (!cfg.Blocks().empty()) {
      for (const auto& inst : cfg.Blocks()[0].instructions) {
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register d : ud.gpr_defs) {
          if (d != Register::kZero) entry_defined_gprs.insert(d);
        }
      }
    }
    for (const auto& block : cfg.Blocks()) {
      absl::flat_hash_set<Register> block_defs;
      for (const auto& inst : block.instructions) {
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register u : ud.gpr_uses) {
          if (u != Register::kZero && !block_defs.contains(u) && !entry_defined_gprs.contains(u)) {
            vars_needing_init.insert(ExpressionBuilder::RegisterVarName(u));
          }
        }
        for (Register d : ud.gpr_defs) {
          if (d != Register::kZero) block_defs.insert(d);
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

}  // namespace rom_nom_nom
