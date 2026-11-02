#include "lifter/ast_converter.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/c_ast.h"
#include "lifter/expression_builder.h"

namespace rom_nom_nom {

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

}  // namespace rom_nom_nom
