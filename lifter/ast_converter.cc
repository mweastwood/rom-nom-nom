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
#include "lifter/ast_expression_folder.h"
#include "lifter/ast_pass_pipeline.h"
#include "lifter/branch_condition_builder.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/cop1_lifter.h"
#include "lifter/expression_builder.h"
#include "lifter/stack_frame_analyzer.h"
#include "lifter/variable_declaration_builder.h"

namespace rom_nom_nom {

namespace {

std::unique_ptr<CExpression> RegExpr(Register reg) {
  if (reg == Register::kZero) {
    return CExpression::Integer(0);
  }
  return CExpression::Identifier(ExpressionBuilder::RegisterVarName(reg));
}

const CExpression& UnwrapScalarCast(const CExpression& expr) {
  if (expr.Kind() == CExpressionKind::kCastExpression) {
    const auto& cast = static_cast<const CastExpression&>(expr);
    if (cast.TargetType().pointer_depth == 0 &&
        (cast.TargetType().name == "s32" || cast.TargetType().name == "u32" ||
         cast.TargetType().name == "s64" || cast.TargetType().name == "u64")) {
      return cast.Operand();
    }
  }
  return expr;
}

bool ExpressionsStructurallyEqual(const CExpression& a, const CExpression& b) {
  if (a.Kind() != b.Kind()) {
    return false;
  }
  switch (a.Kind()) {
    case CExpressionKind::kIntegerLiteral: {
      const auto& ia = static_cast<const IntegerLiteralExpression&>(a);
      const auto& ib = static_cast<const IntegerLiteralExpression&>(b);
      return ia.Value() == ib.Value();
    }
    case CExpressionKind::kFloatLiteral: {
      const auto& fa = static_cast<const FloatLiteralExpression&>(a);
      const auto& fb = static_cast<const FloatLiteralExpression&>(b);
      return fa.Value() == fb.Value() && fa.IsFloat() == fb.IsFloat();
    }
    case CExpressionKind::kStringLiteral: {
      const auto& sa = static_cast<const StringLiteralExpression&>(a);
      const auto& sb = static_cast<const StringLiteralExpression&>(b);
      return sa.Value() == sb.Value();
    }
    case CExpressionKind::kIdentifier: {
      const auto& ida = static_cast<const IdentifierExpression&>(a);
      const auto& idb = static_cast<const IdentifierExpression&>(b);
      return ida.Name() == idb.Name();
    }
    case CExpressionKind::kUnaryExpression: {
      const auto& ua = static_cast<const UnaryExpression&>(a);
      const auto& ub = static_cast<const UnaryExpression&>(b);
      return ua.Op() == ub.Op() && ua.IsPrefix() == ub.IsPrefix() &&
             ExpressionsStructurallyEqual(ua.Operand(), ub.Operand());
    }
    case CExpressionKind::kBinaryExpression: {
      const auto& ba = static_cast<const BinaryExpression&>(a);
      const auto& bb = static_cast<const BinaryExpression&>(b);
      return ba.Op() == bb.Op() && ExpressionsStructurallyEqual(ba.Lhs(), bb.Lhs()) &&
             ExpressionsStructurallyEqual(ba.Rhs(), bb.Rhs());
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& aa = static_cast<const AssignmentExpression&>(a);
      const auto& ab = static_cast<const AssignmentExpression&>(b);
      return aa.Op() == ab.Op() && ExpressionsStructurallyEqual(aa.Lhs(), ab.Lhs()) &&
             ExpressionsStructurallyEqual(aa.Rhs(), ab.Rhs());
    }
    case CExpressionKind::kCallExpression: {
      const auto& ca = static_cast<const CallExpression&>(a);
      const auto& cb = static_cast<const CallExpression&>(b);
      if (!ExpressionsStructurallyEqual(ca.Callee(), cb.Callee()) ||
          ca.Arguments().size() != cb.Arguments().size()) {
        return false;
      }
      for (size_t i = 0; i < ca.Arguments().size(); ++i) {
        if (!ExpressionsStructurallyEqual(*ca.Arguments()[i], *cb.Arguments()[i])) {
          return false;
        }
      }
      return true;
    }
    case CExpressionKind::kCastExpression: {
      const auto& ca = static_cast<const CastExpression&>(a);
      const auto& cb = static_cast<const CastExpression&>(b);
      return ca.TargetType().name == cb.TargetType().name &&
             ca.TargetType().is_const == cb.TargetType().is_const &&
             ca.TargetType().pointer_depth == cb.TargetType().pointer_depth &&
             ExpressionsStructurallyEqual(ca.Operand(), cb.Operand());
    }
    case CExpressionKind::kMemberAccessExpression: {
      const auto& ma = static_cast<const MemberAccessExpression&>(a);
      const auto& mb = static_cast<const MemberAccessExpression&>(b);
      return ma.MemberName() == mb.MemberName() && ma.IsArrow() == mb.IsArrow() &&
             ExpressionsStructurallyEqual(ma.Object(), mb.Object());
    }
    case CExpressionKind::kArrayIndexExpression: {
      const auto& sa = static_cast<const ArrayIndexExpression&>(a);
      const auto& sb = static_cast<const ArrayIndexExpression&>(b);
      return ExpressionsStructurallyEqual(sa.Array(), sb.Array()) &&
             ExpressionsStructurallyEqual(sa.Index(), sb.Index());
    }
    case CExpressionKind::kTernaryExpression: {
      const auto& ca = static_cast<const TernaryExpression&>(a);
      const auto& cb = static_cast<const TernaryExpression&>(b);
      return ExpressionsStructurallyEqual(ca.Condition(), cb.Condition()) &&
             ExpressionsStructurallyEqual(ca.TrueExpression(), cb.TrueExpression()) &&
             ExpressionsStructurallyEqual(ca.FalseExpression(), cb.FalseExpression());
    }
  }
  return false;
}

bool IsSupportedForLoopComparisonOp(std::string_view op) {
  return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "!=";
}

std::string_view ExtractForLoopConditionVar(const BinaryExpression& bin_cond) {
  if (!IsSupportedForLoopComparisonOp(bin_cond.Op())) {
    return {};
  }
  const CExpression& lhs = UnwrapScalarCast(bin_cond.Lhs());
  if (lhs.Kind() != CExpressionKind::kIdentifier) {
    return {};
  }
  std::string_view loop_var = static_cast<const IdentifierExpression&>(lhs).Name();
  if (ExpressionUsesVariable(bin_cond.Rhs(), loop_var)) {
    return {};
  }
  return loop_var;
}

struct ForLoopStepMatch {
  std::unique_ptr<CExpression> step_expr;
  int direction = 0;  // +1 for positive, -1 for negative, 0 for symbolic stride
};

std::optional<ForLoopStepMatch> TryBuildForLoopStep(const CStatement& stmt,
                                                    std::string_view loop_var) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) {
    return std::nullopt;
  }
  const CExpression& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() == CExpressionKind::kUnaryExpression) {
    const auto& unary = static_cast<const UnaryExpression&>(expr);
    if ((unary.Op() == "++" || unary.Op() == "--") &&
        unary.Operand().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(unary.Operand()).Name() == loop_var) {
      return ForLoopStepMatch{expr.Clone(), unary.Op() == "++" ? 1 : -1};
    }
    return std::nullopt;
  }
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) {
    return std::nullopt;
  }
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
      static_cast<const IdentifierExpression&>(assign.Lhs()).Name() != loop_var) {
    return std::nullopt;
  }

  auto make_const_step = [&](int64_t signed_delta) -> std::optional<ForLoopStepMatch> {
    if (signed_delta == 0) {
      return std::nullopt;
    }
    if (signed_delta == 1) {
      return ForLoopStepMatch{
          CExpression::Unary("++", CExpression::Identifier(std::string(loop_var)),
                             /*is_prefix=*/false),
          1};
    }
    if (signed_delta == -1) {
      return ForLoopStepMatch{
          CExpression::Unary("--", CExpression::Identifier(std::string(loop_var)),
                             /*is_prefix=*/false),
          -1};
    }
    if (signed_delta > 1) {
      return ForLoopStepMatch{
          CExpression::Assignment("+=", CExpression::Identifier(std::string(loop_var)),
                                  CExpression::Integer(signed_delta)),
          1};
    }
    return ForLoopStepMatch{
        CExpression::Assignment("-=", CExpression::Identifier(std::string(loop_var)),
                                CExpression::Integer(-signed_delta)),
        -1};
  };

  if (assign.Op() == "+=" || assign.Op() == "-=") {
    if (!IsPureExpression(assign.Rhs()) || ExpressionUsesVariable(assign.Rhs(), loop_var)) {
      return std::nullopt;
    }
    if (assign.Rhs().Kind() == CExpressionKind::kIntegerLiteral) {
      int64_t val = static_cast<const IntegerLiteralExpression&>(assign.Rhs()).Value();
      return make_const_step(assign.Op() == "+=" ? val : -val);
    }
    return ForLoopStepMatch{expr.Clone(), 0};
  }

  if (assign.Op() == "=" && assign.Rhs().Kind() == CExpressionKind::kBinaryExpression) {
    const auto& bin = static_cast<const BinaryExpression&>(assign.Rhs());
    if (bin.Op() != "+" && bin.Op() != "-") {
      return std::nullopt;
    }
    const CExpression* stride = nullptr;
    if (bin.Lhs().Kind() == CExpressionKind::kIdentifier &&
        static_cast<const IdentifierExpression&>(bin.Lhs()).Name() == loop_var) {
      stride = &bin.Rhs();
    } else if (bin.Op() == "+" && bin.Rhs().Kind() == CExpressionKind::kIdentifier &&
               static_cast<const IdentifierExpression&>(bin.Rhs()).Name() == loop_var) {
      stride = &bin.Lhs();
    }
    if (stride == nullptr || !IsPureExpression(*stride) ||
        ExpressionUsesVariable(*stride, loop_var)) {
      return std::nullopt;
    }
    if (stride->Kind() == CExpressionKind::kIntegerLiteral) {
      int64_t val = static_cast<const IntegerLiteralExpression&>(*stride).Value();
      return make_const_step(bin.Op() == "+" ? val : -val);
    }
    return ForLoopStepMatch{
        CExpression::Assignment(bin.Op() == "+" ? "+=" : "-=",
                                CExpression::Identifier(std::string(loop_var)), stride->Clone()),
        0};
  }
  return std::nullopt;
}

bool IsStepDirectionCompatibleWithCondition(std::string_view cond_op, int step_direction) {
  if (cond_op == "<" || cond_op == "<=") {
    return step_direction >= 0;
  }
  if (cond_op == ">" || cond_op == ">=") {
    return step_direction <= 0;
  }
  return cond_op == "!=";
}

const CExpression* MatchSimpleInitializerAssignment(const CStatement& stmt,
                                                    std::string_view loop_var) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) {
    return nullptr;
  }
  const CExpression& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) {
    return nullptr;
  }
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier ||
      static_cast<const IdentifierExpression&>(assign.Lhs()).Name() != loop_var) {
    return nullptr;
  }
  if (!IsPureExpression(assign.Rhs()) || ExpressionUsesVariable(assign.Rhs(), loop_var)) {
    return nullptr;
  }
  return &assign.Rhs();
}

bool IsRedundantLatchComparisonAssignment(const CStatement& stmt, const CExpression& loop_cond) {
  if (stmt.Kind() != CStatementKind::kExpressionStatement) {
    return false;
  }
  const CExpression& expr = static_cast<const ExpressionStatement&>(stmt).Expression();
  if (expr.Kind() != CExpressionKind::kAssignmentExpression) {
    return false;
  }
  const auto& assign = static_cast<const AssignmentExpression&>(expr);
  if (assign.Op() != "=" || assign.Lhs().Kind() != CExpressionKind::kIdentifier) {
    return false;
  }
  std::string_view lhs_name = static_cast<const IdentifierExpression&>(assign.Lhs()).Name();
  if (lhs_name != "v0" && lhs_name != "v1" && lhs_name != "at") {
    return false;
  }
  return ExpressionsStructurallyEqual(UnwrapScalarCast(assign.Rhs()), UnwrapScalarCast(loop_cond));
}

bool IsCompatibleForLoopEntryGuard(const CExpression& guard_cond, const BinaryExpression& loop_cond,
                                   std::string_view loop_var, const CExpression& init_rhs) {
  if (ExpressionsStructurallyEqual(guard_cond, loop_cond)) {
    return true;
  }
  auto substituted = SubstituteIdentifier(loop_cond, loop_var, init_rhs);
  if (ExpressionsStructurallyEqual(guard_cond, *substituted)) {
    return true;
  }
  if (guard_cond.Kind() != CExpressionKind::kBinaryExpression ||
      substituted->Kind() != CExpressionKind::kBinaryExpression) {
    return false;
  }
  const auto& g_bin = static_cast<const BinaryExpression&>(guard_cond);
  const auto& s_bin = static_cast<const BinaryExpression&>(*substituted);
  const CExpression& g_lhs = UnwrapScalarCast(g_bin.Lhs());
  const CExpression& g_rhs = UnwrapScalarCast(g_bin.Rhs());
  const CExpression& s_lhs = UnwrapScalarCast(s_bin.Lhs());
  const CExpression& s_rhs = UnwrapScalarCast(s_bin.Rhs());

  if (g_bin.Op() == s_bin.Op() && ExpressionsStructurallyEqual(g_lhs, s_lhs) &&
      ExpressionsStructurallyEqual(g_rhs, s_rhs)) {
    return true;
  }
  auto comm_op = CommutedBinaryOperator(s_bin.Op());
  if (comm_op.has_value() && g_bin.Op() == *comm_op && ExpressionsStructurallyEqual(g_lhs, s_rhs) &&
      ExpressionsStructurallyEqual(g_rhs, s_lhs)) {
    return true;
  }

  if (ExpressionsStructurallyEqual(g_lhs, s_lhs) &&
      g_rhs.Kind() == CExpressionKind::kIntegerLiteral &&
      s_rhs.Kind() == CExpressionKind::kIntegerLiteral) {
    int64_t g_val = static_cast<const IntegerLiteralExpression&>(g_rhs).Value();
    int64_t s_val = static_cast<const IntegerLiteralExpression&>(s_rhs).Value();
    if ((s_bin.Op() == "<=" && g_bin.Op() == "<" && g_val == s_val + 1) ||
        (s_bin.Op() == "<" && g_bin.Op() == "<=" && g_val == s_val - 1) ||
        (s_bin.Op() == ">=" && g_bin.Op() == ">" && g_val == s_val - 1) ||
        (s_bin.Op() == ">" && g_bin.Op() == ">=" && g_val == s_val + 1)) {
      return true;
    }
  }

  if (ExpressionsStructurallyEqual(g_lhs, s_rhs) &&
      g_rhs.Kind() == CExpressionKind::kIntegerLiteral &&
      s_lhs.Kind() == CExpressionKind::kIntegerLiteral) {
    int64_t g_val = static_cast<const IntegerLiteralExpression&>(g_rhs).Value();
    int64_t s_val = static_cast<const IntegerLiteralExpression&>(s_lhs).Value();
    if ((s_bin.Op() == "<=" && g_bin.Op() == ">" && g_val == s_val - 1) ||
        (s_bin.Op() == "<" && g_bin.Op() == ">=" && g_val == s_val + 1) ||
        (s_bin.Op() == ">=" && g_bin.Op() == "<" && g_val == s_val + 1) ||
        (s_bin.Op() == ">" && g_bin.Op() == "<=" && g_val == s_val - 1)) {
      return true;
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
        return CExpression::Unary(expr.op, ConvertExpression(*expr.args[0], current_function_name));
      }
      return CExpression::Identifier(expr.name);

    case ExpressionKind::kCast:
      if (!expr.args.empty() && expr.args[0]) {
        auto operand = ConvertExpression(*expr.args[0], current_function_name);
        if (expr.name == "u8*" || expr.name == "u8 *") {
          return CExpression::Cast(CType::Named("u8").MakePointer(), std::move(operand));
        }
        return CExpression::Cast(CType::Named(expr.name), std::move(operand));
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
        double_vars_(AbiAnalyzer::IdentifyDoubleVariables(cfg_)),
        frame_analysis_(StackFrameAnalyzer::Analyze(cfg_)),
        float_parameters_in_general_registers_(
            AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg_)),
        double_parameter_in_a2_a3_(AbiAnalyzer::DetectDoubleParameterInGeneralRegisters(cfg_)),
        float_stack_param_slots_(AbiAnalyzer::DetectFloatStackParameterSlots(cfg_)),
        double_stack_param_slots_(AbiAnalyzer::DetectDoubleStackParameterSlots(cfg_)),
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
    call_return_info_ = AbiAnalyzer::ComputeCallReturnInfo(cfg_, returns_v0_, returns_f0_);
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
    ctx.double_parameter_in_a2_a3 = double_parameter_in_a2_a3_;
    ctx.float_stack_param_slots = &float_stack_param_slots_;
    ctx.double_stack_param_slots = &double_stack_param_slots_;
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
      for (int i = static_cast<int>(cond_block->instructions.size()) - 1; i >= 0; --i) {
        const auto& inst = cond_block->instructions[i];
        if (inst.opcode == Opcode::kCLeS || inst.opcode == Opcode::kCLtS ||
            inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kCLtD) {
          comparison_instruction_index = i;
          diamond_instruction_indices.insert(static_cast<size_t>(i));
          break;
        }
      }

      FpRegister const_fp = FpRegister::kF0;
      if (comparison_instruction_index >= 0) {
        const auto& cmp_inst = cond_block->instructions[comparison_instruction_index];
        if (cmp_inst.fs.has_value() && cmp_inst.ft.has_value()) {
          const_fp = (*cmp_inst.fs == info.source_float_register) ? *cmp_inst.ft : *cmp_inst.fs;
        }
        for (int i = comparison_instruction_index - 1; i >= 0; --i) {
          const auto& inst = cond_block->instructions[i];
          if (inst.IsNop()) continue;
          if ((inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kDmtc1) &&
              inst.fs == const_fp) {
            Register reg = inst.rt.value_or(Register::kZero);
            for (int j = i - 1; j >= 0; --j) {
              const auto& prev = cond_block->instructions[j];
              if (prev.IsNop()) continue;
              if (prev.opcode == Opcode::kLui && prev.rt == reg) {
                uint32_t imm = prev.immediate & 0xFFFF;
                if (imm == 0x4F00 || imm == 0x41E0) {
                  diamond_instruction_indices.insert(static_cast<size_t>(i));
                  diamond_instruction_indices.insert(static_cast<size_t>(j));
                }
                break;
              }
            }
            break;
          } else if ((inst.opcode == Opcode::kLdc1 || inst.opcode == Opcode::kLwc1) &&
                     inst.ft == const_fp) {
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
        if (setup_instruction_vrams_.contains(inst.vram)) {
          continue;
        }

        if (diamond_instruction_indices.contains(i)) {
          continue;
        }

        non_diamond_instructions.push_back(inst);
      }

      if (cond_block->instructions.size() >= 2 &&
          cond_block->instructions[cond_block->instructions.size() - 2].IsBranch()) {
        const auto& branch_inst = cond_block->instructions[cond_block->instructions.size() - 2];
        const auto& delay_inst = cond_block->instructions.back();
        bool is_diamond_delay =
            (delay_inst.opcode == Opcode::kSubS || delay_inst.opcode == Opcode::kSubD) &&
            (branch_inst.IsBranchLikely() ||
             (delay_inst.fs.has_value() && *delay_inst.fs == info.source_float_register));
        if (!delay_inst.IsNop() && !is_diamond_delay &&
            !setup_instruction_vrams_.contains(delay_inst.vram)) {
          non_diamond_instructions.push_back(delay_inst);
        }
      }

      if (!non_diamond_instructions.empty()) {
        const std::array<std::optional<uint32_t>, 32>* initial_fpr = nullptr;
        const RegisterTracker* initial_tracker = nullptr;
        auto it_state = block_entry_states_.find(region.condition_block_id);
        if (it_state != block_entry_states_.end()) {
          initial_fpr = &it_state->second.fpr_const_bits;
          initial_tracker = &it_state->second.tracker;
        }
        auto pre_stmts =
            ExpressionBuilder::LiftInstructions(absl::MakeSpan(non_diamond_instructions),
                                                BuildLiftContext(initial_fpr, initial_tracker),
                                                /*omit_last_instruction=*/false);
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

    std::string dest_var = ExpressionBuilder::RegisterVarName(info.destination_integer_register);
    std::string src_fp_var =
        ExpressionBuilder::FpRegisterVarName(info.source_float_register, info.is_double);

    auto cast_expr = CExpression::Cast(CType::U32(), CExpression::Identifier(src_fp_var));
    auto assign_stmt = CStatement::Expression(
        CExpression::Assignment("=", CExpression::Identifier(dest_var), std::move(cast_expr)));
    target_block->AddStatement(std::move(assign_stmt));

    if (info.trailing_instruction.has_value()) {
      auto trailing_stmts = LiftInstructionsHelper(absl::MakeSpan(&*info.trailing_instruction, 1));
      for (auto& s : trailing_stmts) {
        auto cs = AstConverter::ConvertStatement(s, function_name_);
        if (cs != nullptr) {
          target_block->AddStatement(std::move(cs));
        }
      }
    }
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
        auto cond = cond_block != nullptr ? BranchConditionBuilder::ExtractBranchCondition(
                                                *cond_block, region.invert_condition)
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
              auto next_cond = BranchConditionBuilder::ExtractBranchCondition(*add_block, add_inv);
              cond =
                  CExpression::Binary(region.condition_op, std::move(cond), std::move(next_cond));
            }
          }
        }

        auto then_body = std::make_unique<CompoundStatement>();
        auto else_body = std::make_unique<CompoundStatement>();
        bool skip_branch_likely_delay = false;
        if (has_branch_likely && !region.invert_condition && cond_block != nullptr &&
            cond_block->instructions.size() >= 2 && region.children.size() == 1 &&
            region.children[0] != nullptr && region.children[0]->type == RegionType::kBreak) {
          uint32_t break_id = region.children[0]->block_id;
          bool directly_targets_break = false;
          for (uint32_t succ : cond_block->successors) {
            if (succ == break_id) {
              directly_targets_break = true;
              break;
            }
          }
          if (!directly_targets_break) {
            const auto* break_blk = cfg_.GetBlock(break_id);
            if (break_blk != nullptr && break_blk->instructions.size() == 1 &&
                break_blk->successors.size() == 1) {
              const auto& delay_inst = cond_block->instructions.back();
              const auto& break_inst = break_blk->instructions.front();
              if (!delay_inst.IsNop() && delay_inst.opcode == break_inst.opcode &&
                  delay_inst.rs == break_inst.rs && delay_inst.rt == break_inst.rt &&
                  delay_inst.rd == break_inst.rd && delay_inst.immediate == break_inst.immediate &&
                  delay_inst.fs == break_inst.fs && delay_inst.ft == break_inst.ft &&
                  delay_inst.fd == break_inst.fd) {
                skip_branch_likely_delay = true;
              }
            }
          }
        }
        if (has_branch_likely && !skip_branch_likely_delay && cond_block != nullptr) {
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
            const auto& orig_bin_cond = static_cast<const BinaryExpression&>(loop_cond);
            const auto& loop_body_stmt = do_while->Body();
            if (loop_body_stmt.Kind() == CStatementKind::kCompoundStatement) {
              const auto& loop_compound = static_cast<const CompoundStatement&>(loop_body_stmt);
              std::vector<std::unique_ptr<BinaryExpression>> cond_candidates;
              cond_candidates.push_back(std::make_unique<BinaryExpression>(
                  orig_bin_cond.Op(), orig_bin_cond.Lhs().Clone(), orig_bin_cond.Rhs().Clone()));
              if (auto comm_op = CommutedBinaryOperator(orig_bin_cond.Op()); comm_op.has_value()) {
                cond_candidates.push_back(std::make_unique<BinaryExpression>(
                    *comm_op, orig_bin_cond.Rhs().Clone(), orig_bin_cond.Lhs().Clone()));
              }

              for (const auto& candidate_cond : cond_candidates) {
                if (emitted_for) break;
                const BinaryExpression& bin_cond = *candidate_cond;
                std::string_view loop_var_view = ExtractForLoopConditionVar(bin_cond);
                if (loop_var_view.empty()) continue;
                std::string loop_var(loop_var_view);

                int inc_idx = -1;
                std::optional<ForLoopStepMatch> step_match;
                for (int i = static_cast<int>(loop_compound.Statements().size()) - 1; i >= 0; --i) {
                  auto candidate = TryBuildForLoopStep(*loop_compound.Statements()[i], loop_var);
                  if (candidate.has_value() &&
                      IsStepDirectionCompatibleWithCondition(bin_cond.Op(), candidate->direction)) {
                    inc_idx = i;
                    step_match = std::move(candidate);
                    break;
                  }
                }

                bool step_placement_valid = (inc_idx >= 0);
                for (size_t i = 0; step_placement_valid && i < loop_compound.Statements().size();
                     ++i) {
                  if (static_cast<int>(i) == inc_idx) continue;
                  const CStatement& s = *loop_compound.Statements()[i];
                  if (IsRedundantLatchComparisonAssignment(s, loop_cond) ||
                      IsRedundantLatchComparisonAssignment(s, bin_cond)) {
                    continue;
                  }
                  if (StatementModifiesVariable(s, loop_var)) {
                    step_placement_valid = false;
                    break;
                  }
                  if (static_cast<int>(i) > inc_idx &&
                      (StatementReferencesVariable(s, loop_var) || StatementContainsCall(s))) {
                    step_placement_valid = false;
                    break;
                  }
                }

                int init_idx = -1;
                const CExpression* init_rhs = nullptr;
                for (int si = static_cast<int>(target_block->Statements().size()) - 1; si >= 0;
                     --si) {
                  if (static_cast<size_t>(si + 4) < target_block->Statements().size()) {
                    break;
                  }
                  const CExpression* rhs =
                      MatchSimpleInitializerAssignment(*target_block->Statements()[si], loop_var);
                  if (rhs != nullptr) {
                    init_idx = si;
                    init_rhs = rhs;
                    break;
                  }
                }

                bool init_placement_valid =
                    (init_idx >= 0 && init_rhs != nullptr &&
                     IsCompatibleForLoopEntryGuard(*cond, bin_cond, loop_var, *init_rhs));
                if (init_placement_valid) {
                  const bool is_callee_saved_loop_var =
                      absl::StartsWith(loop_var, "temp_s") || loop_var == "temp_fp";
                  std::vector<std::string> init_vars;
                  absl::flat_hash_set<std::string> init_seen;
                  CollectVariablesInExpression(*init_rhs, &init_vars, &init_seen);
                  for (size_t k = static_cast<size_t>(init_idx + 1);
                       k < target_block->Statements().size(); ++k) {
                    const CStatement& after_s = *target_block->Statements()[k];
                    if (IsRedundantLatchComparisonAssignment(after_s, *cond) ||
                        IsRedundantLatchComparisonAssignment(after_s, loop_cond) ||
                        IsRedundantLatchComparisonAssignment(after_s, bin_cond)) {
                      continue;
                    }
                    if (is_callee_saved_loop_var ||
                        StatementReferencesVariable(after_s, loop_var)) {
                      init_placement_valid = false;
                      break;
                    }
                    for (const auto& iv : init_vars) {
                      if (StatementModifiesVariable(after_s, iv)) {
                        init_placement_valid = false;
                        break;
                      }
                    }
                    if (!init_placement_valid) break;
                  }
                }

                if (step_placement_valid && init_placement_valid) {
                  auto init_rhs_clone = init_rhs->Clone();
                  auto& target_stmts = target_block->MutableStatements();
                  target_stmts.erase(target_stmts.begin() + init_idx);

                  auto for_body = std::make_unique<CompoundStatement>();
                  for (size_t i = 0; i < loop_compound.Statements().size(); ++i) {
                    if (static_cast<int>(i) == inc_idx) continue;
                    if (IsRedundantLatchComparisonAssignment(*loop_compound.Statements()[i],
                                                             loop_cond) ||
                        IsRedundantLatchComparisonAssignment(*loop_compound.Statements()[i],
                                                             bin_cond)) {
                      continue;
                    }
                    for_body->AddStatement(loop_compound.Statements()[i]->Clone());
                  }
                  auto for_init = CStatement::Expression(CExpression::Assignment(
                      "=", CExpression::Identifier(loop_var), std::move(init_rhs_clone)));
                  target_block->AddStatement(CStatement::For(std::move(for_init), bin_cond.Clone(),
                                                             std::move(step_match->step_expr),
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
        auto cond = cond_block != nullptr ? BranchConditionBuilder::ExtractBranchCondition(
                                                *cond_block, region.invert_condition)
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
              auto next_cond = BranchConditionBuilder::ExtractBranchCondition(*add_block, add_inv);
              cond =
                  CExpression::Binary(region.condition_op, std::move(cond), std::move(next_cond));
            }
          }
        }

        auto then_body = std::make_unique<CompoundStatement>();
        auto else_body = std::make_unique<CompoundStatement>();
        bool skip_branch_likely_delay = false;
        if (has_branch_likely && !region.invert_condition && cond_block != nullptr &&
            cond_block->instructions.size() >= 2 && region.children.size() == 1 &&
            region.children[0] != nullptr && region.children[0]->type == RegionType::kBreak) {
          uint32_t break_id = region.children[0]->block_id;
          bool directly_targets_break = false;
          for (uint32_t succ : cond_block->successors) {
            if (succ == break_id) {
              directly_targets_break = true;
              break;
            }
          }
          if (!directly_targets_break) {
            const auto* break_blk = cfg_.GetBlock(break_id);
            if (break_blk != nullptr && break_blk->instructions.size() == 1 &&
                break_blk->successors.size() == 1) {
              const auto& delay_inst = cond_block->instructions.back();
              const auto& break_inst = break_blk->instructions.front();
              if (!delay_inst.IsNop() && delay_inst.opcode == break_inst.opcode &&
                  delay_inst.rs == break_inst.rs && delay_inst.rt == break_inst.rt &&
                  delay_inst.rd == break_inst.rd && delay_inst.immediate == break_inst.immediate &&
                  delay_inst.fs == break_inst.fs && delay_inst.ft == break_inst.ft &&
                  delay_inst.fd == break_inst.fd) {
                skip_branch_likely_delay = true;
              }
            }
          }
        }
        if (has_branch_likely && !skip_branch_likely_delay && cond_block != nullptr) {
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

          bool decouple_delay =
              !has_branch_likely && !peeled_loop_delays_.contains(cond_block_id) &&
              ShouldDecoupleLoopConditionDelaySlot(cond_block_id, region.loop_header);

          bool delay_clobbers_cond = false;
          if (!decouple_delay && !peeled_loop_delays_.contains(cond_block_id) &&
              cond_block != nullptr && cond_block->instructions.size() >= 2) {
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
                    BranchConditionBuilder::FindFoldedComparison(cond_block->instructions);
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
                  BranchConditionBuilder::FindFoldedComparison(cond_block->instructions);
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
            auto break_cond = BranchConditionBuilder::ExtractBranchCondition(
                *cond_block, !region.invert_condition);
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
            auto cond = cond_block != nullptr ? BranchConditionBuilder::ExtractBranchCondition(
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
            auto folded_comp =
                BranchConditionBuilder::FindFoldedComparison(header_block->instructions);
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

                if (IsPureExpression(assign.Rhs())) {
                  auto raw_cond = header_block != nullptr
                                      ? BranchConditionBuilder::ExtractBranchCondition(
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
            auto raw_cond = header_block != nullptr
                                ? BranchConditionBuilder::ExtractBranchCondition(
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
            auto break_cond = header_block != nullptr
                                  ? BranchConditionBuilder::ExtractBranchCondition(
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
            auto cond = header_block != nullptr ? BranchConditionBuilder::ExtractBranchCondition(
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
        auto block_has_only_return = [&](const BasicBlock& blk) {
          auto lifted = ExpressionBuilder::LiftBlock(blk, BuildLiftContext(),
                                                     /*omit_last_instruction=*/false);
          for (const auto& s : lifted) {
            if (s.kind != StatementKind::kReturn) {
              return false;
            }
          }
          return true;
        };
        bool already_ended_with_return =
            !target_block->Statements().empty() &&
            target_block->Statements().back()->Kind() == CStatementKind::kReturnStatement;
        bool is_return = false;
        const auto* target_blk = cfg_.GetBlock(region.block_id);
        if (target_blk != nullptr) {
          bool target_already_emitted = emitted_blocks_.contains(target_blk->id);
          if (target_blk->successors.empty()) {
            is_return = already_ended_with_return || block_has_only_return(*target_blk);
          } else if (target_blk->successors.size() == 1) {
            const auto* succ_blk = cfg_.GetBlock(target_blk->successors[0]);
            if (succ_blk != nullptr && succ_blk->successors.empty()) {
              is_return = already_ended_with_return ||
                          ((target_already_emitted || block_has_only_return(*target_blk)) &&
                           block_has_only_return(*succ_blk));
            }
          }
        }
        if (is_return) {
          if (!already_ended_with_return) {
            if (returns_v0_) {
              target_block->AddStatement(CStatement::Return(CExpression::Identifier("v0")));
            } else if (returns_f0_) {
              bool is_double = AbiAnalyzer::DetermineReturnsF0Double(cfg_);
              target_block->AddStatement(
                  CStatement::Return(CExpression::Identifier(is_double ? "f0_d" : "f0")));
            } else {
              target_block->AddStatement(CStatement::Return());
            }
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

      case RegionType::kSwitch:
        ConvertSwitchRegion(region, target_block);
        break;
    }
  }

 private:
  void ConvertSwitchRegion(const StructuredRegion& region, CompoundStatement* target_block) {
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
    if (!is_case_block && (region.jump_table == nullptr || region.owned_jump_table != nullptr)) {
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
          if (term != nullptr && term->IsBranchLikely() && bounds_block->instructions.size() >= 2) {
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
              auto delay_statements = LiftInstructionsHelper(absl::MakeSpan(&delay_instruction, 1));
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
        std::string variable_name = ExpressionBuilder::RegisterVarName(*sc.delay_slot_const_reg);
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
  }
  bool IsRegisterDeadAtBlockEntry(uint32_t start_block_id, Register reg) const {
    if (reg == Register::kZero) return true;
    absl::flat_hash_set<uint32_t> visited;
    std::vector<uint32_t> worklist = {start_block_id};
    while (!worklist.empty()) {
      uint32_t blk_id = worklist.back();
      worklist.pop_back();
      if (!visited.insert(blk_id).second) continue;
      const auto* blk = cfg_.GetBlock(blk_id);
      if (blk == nullptr) return false;
      bool killed = false;
      for (size_t i = 0; i < blk->instructions.size(); ++i) {
        const auto& inst = blk->instructions[i];
        if ((inst.IsCall() || inst.IsReturn() || inst.IsBranch() || inst.IsJump()) &&
            i + 1 < blk->instructions.size()) {
          const auto& delay_inst = blk->instructions[i + 1];
          bool annuls_delay = inst.IsBranchLikely();
          RegisterUseDef inst_ud = GetInstructionUseDef(inst);
          for (Register u : inst_ud.gpr_uses) {
            if (u == reg) return false;
          }
          RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
          for (Register u : delay_ud.gpr_uses) {
            if (u == reg) return false;
          }
          if (!annuls_delay) {
            for (Register d : delay_ud.gpr_defs) {
              if (d == reg) {
                killed = true;
                break;
              }
            }
          }
          if (killed) break;
          if (inst.IsCall()) {
            if (reg >= Register::kA0 && reg <= Register::kA3) {
              return false;
            }
            if (reg == Register::kV0 || reg == Register::kV1 || reg == Register::kAt ||
                (reg >= Register::kT0 && reg <= Register::kT7) || reg == Register::kT8 ||
                reg == Register::kT9) {
              killed = true;
              break;
            }
          } else if (inst.IsReturn()) {
            if (reg == Register::kV0 && returns_v0_) {
              return false;
            }
            killed = true;
            break;
          }
          ++i;
          continue;
        }
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register u : ud.gpr_uses) {
          if (u == reg) return false;
        }
        for (Register d : ud.gpr_defs) {
          if (d == reg) {
            killed = true;
            break;
          }
        }
        if (killed) break;
      }
      if (!killed) {
        if (blk->successors.empty()) {
          if (reg == Register::kV0 && returns_v0_) return false;
          continue;
        }
        for (uint32_t succ : blk->successors) {
          worklist.push_back(succ);
        }
      }
    }
    return true;
  }

  void PreScanPeeledLoopRotations(const StructuredRegion& region) {
    if (region.type == RegionType::kLoop && region.loop_type == LoopType::kDoWhile) {
      uint32_t cond_block_id =
          region.condition_block_id != 0 ? region.condition_block_id : region.loop_header;
      const auto* cond_block = cfg_.GetBlock(cond_block_id);
      if (cond_block != nullptr && cond_block->instructions.size() >= 2) {
        const auto* term = cond_block->Terminator();
        if (term != nullptr && term->IsBranch()) {
          const auto& delay_inst = cond_block->instructions.back();
          bool can_peel = term->IsBranchLikely();
          if (!can_peel && !region.invert_condition && !delay_inst.IsNop() &&
              !delay_inst.IsStore() && !delay_inst.IsCall() && !delay_inst.IsBranch() &&
              !delay_inst.IsJump() && !delay_inst.IsReturn() &&
              delay_inst.opcode != Opcode::kMfc0 &&
              !ShouldDecoupleLoopConditionDelaySlot(cond_block_id, region.loop_header)) {
            RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
            absl::flat_hash_set<Register> cond_regs;
            if (term->rs.has_value() && *term->rs != Register::kZero) {
              cond_regs.insert(*term->rs);
            }
            if (term->rt.has_value() && *term->rt != Register::kZero) {
              cond_regs.insert(*term->rt);
            }
            auto folded_comp =
                BranchConditionBuilder::FindFoldedComparison(cond_block->instructions);
            if (folded_comp.has_value() &&
                folded_comp->instruction_index < cond_block->instructions.size()) {
              RegisterUseDef def_ud =
                  GetInstructionUseDef(cond_block->instructions[folded_comp->instruction_index]);
              for (Register r : def_ud.gpr_uses) {
                if (r != Register::kZero) cond_regs.insert(r);
              }
              for (Register r : def_ud.gpr_defs) {
                if (r != Register::kZero) cond_regs.insert(r);
              }
            }
            bool clobbers_cond = false;
            for (Register r : delay_ud.gpr_defs) {
              if (cond_regs.contains(r)) {
                clobbers_cond = true;
                break;
              }
            }
            if (clobbers_cond && !delay_ud.gpr_defs.empty() && delay_ud.fpr_defs.empty()) {
              std::optional<uint32_t> exit_block_id;
              for (uint32_t succ : cond_block->successors) {
                if (succ != region.loop_header) {
                  exit_block_id = succ;
                  break;
                }
              }
              if (exit_block_id.has_value()) {
                bool all_dead = true;
                for (Register def_reg : delay_ud.gpr_defs) {
                  if (!IsRegisterDeadAtBlockEntry(*exit_block_id, def_reg)) {
                    all_dead = false;
                    break;
                  }
                }
                can_peel = all_dead;
              }
            }
          }
          if (can_peel) {
            std::vector<uint32_t> external_preds;
            for (const auto& edge : cfg_.Edges()) {
              if (edge.to_block_id == region.loop_header && edge.from_block_id != cond_block_id) {
                external_preds.push_back(edge.from_block_id);
              }
            }
            if (external_preds.size() == 1 || term->IsBranchLikely()) {
              for (uint32_t pred_id : external_preds) {
                const auto* pred_block = cfg_.GetBlock(pred_id);
                if (pred_block != nullptr && !pred_block->instructions.empty() &&
                    (term->IsBranchLikely() || pred_block->successors.size() == 1)) {
                  const auto& pred_inst = pred_block->instructions.back();
                  bool pred_is_standalone = !pred_inst.IsBranch() && !pred_inst.IsJump() &&
                                            !pred_inst.IsCall() && !pred_inst.IsReturn();
                  if (pred_is_standalone && pred_block->instructions.size() >= 2 &&
                      !term->IsBranchLikely()) {
                    const auto& prev_inst =
                        pred_block->instructions[pred_block->instructions.size() - 2];
                    if (prev_inst.IsBranch() || prev_inst.IsJump() || prev_inst.IsCall() ||
                        prev_inst.IsReturn()) {
                      pred_is_standalone = false;
                    }
                  }
                  if (pred_is_standalone && pred_inst.opcode == delay_inst.opcode &&
                      pred_inst.rs == delay_inst.rs && pred_inst.rt == delay_inst.rt &&
                      pred_inst.rd == delay_inst.rd &&
                      pred_inst.immediate == delay_inst.immediate && !delay_inst.IsNop() &&
                      !peeled_loop_header_insts_.contains(region.loop_header) &&
                      !omit_last_instruction_blocks_.contains(pred_block->id)) {
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
    }
    for (const auto& child : region.children) {
      if (child) PreScanPeeledLoopRotations(*child);
    }
    for (const auto& sc : region.cases) {
      if (sc.body) PreScanPeeledLoopRotations(*sc.body);
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

    std::vector<Instruction> pre_switch_insts(
        block->instructions.begin(),
        block->instructions.begin() + static_cast<ptrdiff_t>(pre_switch_count));
    if (branch_index >= 0 && branch_index + 1 < static_cast<int>(block->instructions.size())) {
      const auto& branch = block->instructions[branch_index];
      const auto& ds = block->instructions[branch_index + 1];
      RegisterUseDef ds_ud = GetInstructionUseDef(ds);
      bool emit_delay_before_switch = false;
      for (Register def_reg : ds_ud.gpr_defs) {
        if ((def_reg >= Register::kS0 && def_reg <= Register::kS7) || def_reg == Register::kFp) {
          emit_delay_before_switch = true;
          break;
        }
      }
      if (!emit_delay_before_switch && !branch.IsBranchLikely() && !ds.IsNop() &&
          switch_selector_register != Register::kZero) {
        bool is_prologue_save =
            (ds.opcode == Opcode::kSw && ds.rs == Register::kSp && ds.rt.has_value() &&
             (*ds.rt == Register::kRa || *ds.rt == Register::kFp ||
              (*ds.rt >= Register::kS0 && *ds.rt <= Register::kS7)));
        bool is_switch_split = (ds.opcode == Opcode::kSlti || ds.opcode == Opcode::kSltiu ||
                                ds.opcode == Opcode::kSlt || ds.opcode == Opcode::kSltu);
        bool defines_selector_or_at = false;
        for (Register def_reg : ds_ud.gpr_defs) {
          if (def_reg == switch_selector_register || def_reg == Register::kAt) {
            defines_selector_or_at = true;
            break;
          }
        }
        if (!is_prologue_save && !is_switch_split && !defines_selector_or_at) {
          emit_delay_before_switch = true;
        }
      }
      if (emit_delay_before_switch) {
        pre_switch_insts.push_back(ds);
      }
    }
    if (!pre_switch_insts.empty()) {
      auto lifted = LiftInstructionsHelper(pre_switch_insts);
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
            bool is_double = AbiAnalyzer::DetermineReturnsF0Double(cfg_);
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

  void StepBlockState(BlockState& state, const Instruction& inst) const {
    StepFprConstantBits(inst, state.tracker, state.fpr_const_bits.data());
    state.tracker.Step(inst);
    if (symbol_index_ != nullptr &&
        (inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
         inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi)) {
      for (Register d : GetInstructionUseDef(inst).gpr_defs) {
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
                  float_parameters_in_general_registers_.contains(reg) ||
                  (double_parameter_in_a2_a3_ && (reg == Register::kA2 || reg == Register::kA3))) {
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

        auto step_inst = [this](BlockState& state, const Instruction& inst) {
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
  bool double_parameter_in_a2_a3_ = false;
  absl::flat_hash_set<int> float_stack_param_slots_;
  absl::flat_hash_set<int> double_stack_param_slots_;
  absl::flat_hash_set<int32_t> float_local_stack_offsets_;
  std::optional<uint32_t> current_switch_break_block_;
  absl::flat_hash_map<uint32_t, CallReturnInfo> call_return_info_;
  absl::flat_hash_map<uint32_t, Instruction> peeled_loop_delays_;
  absl::flat_hash_map<uint32_t, Instruction> peeled_loop_header_insts_;
  absl::flat_hash_set<uint32_t> peeled_loop_header_emitted_;
};

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
  *returns_f0 = AbiAnalyzer::DetermineReturnsF0(cfg);
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
        AbiAnalyzer::DetermineReturnsV0(cfg, symbol_index, options.functions_returning_v0);
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
    parameter_count = AbiAnalyzer::DetermineParameterCount(all_instructions);
  }

  int fp_param_count = 0;
  if (options.function_fp_parameter_counts != nullptr) {
    auto it = options.function_fp_parameter_counts->find(options.function_name);
    if (it != options.function_fp_parameter_counts->end()) {
      fp_param_count = it->second;
    }
  }
  if (fp_param_count == 0 && !all_instructions.empty()) {
    fp_param_count = AbiAnalyzer::DetermineFpParameterCount(all_instructions);
  }

  int total_param_count = std::max(parameter_count, fp_param_count);
  auto float_parameters_in_general_registers =
      AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg);
  bool double_parameter_in_a2_a3 = AbiAnalyzer::DetectDoubleParameterInGeneralRegisters(cfg);
  auto float_stack_param_slots = AbiAnalyzer::DetectFloatStackParameterSlots(cfg);
  auto double_stack_param_slots = AbiAnalyzer::DetectDoubleStackParameterSlots(cfg);
  uint32_t frame_size = StackFrameAnalyzer::ComputeFrameSize(cfg);
  auto is_stack_slot_accessed = [&](int s) -> bool {
    int32_t target_off = static_cast<int32_t>(frame_size + 16) + (s - 4) * 4;
    for (const auto& inst : all_instructions) {
      if ((inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.immediate == target_off &&
          (inst.IsLoad() || inst.IsStore())) {
        return true;
      }
    }
    return false;
  };
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
    } else if (slot == 1 && double_parameter_in_a2_a3 &&
               !AbiAnalyzer::IsRegisterLiveIn(cfg, Register::kA1)) {
      slot++;
    } else if (slot == 2 && double_parameter_in_a2_a3) {
      parameters.push_back(CParameter{.type = CType::F64(), .name = "arg2"});
      slot += 2;
    } else if (slot == 3 && double_stack_param_slots.contains(4) &&
               !AbiAnalyzer::IsRegisterLiveIn(cfg, Register::kA3)) {
      slot++;
    } else if (slot >= 5 && (slot % 2 == 1) && double_stack_param_slots.contains(slot + 1) &&
               !is_stack_slot_accessed(slot)) {
      slot++;
    } else if (slot >= 4 && double_stack_param_slots.contains(slot)) {
      parameters.push_back(
          CParameter{.type = CType::F64(), .name = absl::StrFormat("arg%d", slot)});
      slot += 2;
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
    bool is_double = AbiAnalyzer::DetermineReturnsF0Double(cfg);
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
      bool is_double = AbiAnalyzer::DetermineReturnsF0Double(cfg);
      return_type = is_double ? CType::F64() : CType::F32();
    } else {
      return_type = CType::S32();
    }
  }

  // Run AST post-processing pipeline
  AstPassContext pass_ctx{
      .parameters = parameters,
      .frame_size = ctx.FrameSize(),
      .has_frame_pointer = ctx.FrameAnalysis().has_frame_pointer,
      .return_type = return_type,
      .has_goto_targets = !ctx.GotoTargets().empty(),
      .function_parameter_counts = options.function_parameter_counts,
  };
  AstPassPipeline::Run(body.get(), pass_ctx);

  absl::Status decl_status =
      PrependSynthesizedLocalDeclarations(cfg, ctx.FrameAnalysis(), ctx.DoubleVars(), parameters,
                                          return_type, symbol_index, options, &body);
  if (!decl_status.ok()) {
    return decl_status;
  }

  return FunctionDeclaration(return_type, options.function_name, std::move(parameters),
                             std::move(body), options.is_static);
}

}  // namespace rom_nom_nom
