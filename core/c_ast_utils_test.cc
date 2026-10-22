#include "core/c_ast_utils.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "core/c_ast.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;

TEST(CAstUtilsTest, ExtractsFunctionPointerCastCalleeIdentifier) {
  std::string_view prefix;
  std::string_view id;
  EXPECT_TRUE(ExtractFunctionPointerCastCalleeIdentifier("((void (*)())func_ptr)", &prefix, &id));
  EXPECT_EQ(prefix, "((void (*)())");
  EXPECT_EQ(id, "func_ptr");
  EXPECT_FALSE(ExtractFunctionPointerCastCalleeIdentifier("normal_func", &prefix, &id));
}

TEST(CAstUtilsTest, ChecksPurityAndCommutability) {
  auto pure_bin =
      CExpression::Binary("<", CExpression::Identifier("a"), CExpression::Identifier("b"));
  EXPECT_TRUE(IsPureExpression(*pure_bin));
  EXPECT_TRUE(IsCommutableBinary(static_cast<const BinaryExpression&>(*pure_bin)));
  EXPECT_EQ(CommutedBinaryOperator("<"), ">");

  auto negated = NegateCondition(*pure_bin);
  EXPECT_EQ(negated->ToString(), "(a >= b)");

  auto call_expr = CExpression::Call(CExpression::Identifier("foo"), {});
  EXPECT_FALSE(IsPureExpression(*call_expr));
}

TEST(CAstUtilsTest, DetectsCallsAndHandlesMathIntrinsicsFilter) {
  std::vector<std::unique_ptr<CExpression>> args;
  args.push_back(CExpression::Identifier("f12"));
  auto intrinsic_call = CExpression::Call(CExpression::Identifier("N64Sqrtf"), std::move(args));
  EXPECT_TRUE(ExpressionContainsCall(*intrinsic_call, /*ignore_math_intrinsics=*/false));
  EXPECT_FALSE(ExpressionContainsCall(*intrinsic_call, /*ignore_math_intrinsics=*/true));

  auto regular_call = CExpression::Call(CExpression::Identifier("func_80001000"), {});
  auto stmt = CStatement::Return(std::move(regular_call));
  EXPECT_TRUE(StatementContainsCall(*stmt, /*ignore_math_intrinsics=*/true));
  EXPECT_TRUE(HasReturnValue(*stmt));
}

TEST(CAstUtilsTest, CountsAndCollectsVariablesAndSubstitutesIdentifiers) {
  auto expr = CExpression::Binary(
      "+", CExpression::Identifier("temp_s0"),
      CExpression::Binary("*", CExpression::Identifier("temp_s0"), CExpression::Identifier("v0")));
  EXPECT_EQ(CountIdentifierOccurrencesInExpression(*expr, "temp_s0"), 2);
  EXPECT_EQ(CountIdentifierOccurrencesInExpression(*expr, "v0"), 1);
  EXPECT_TRUE(ExpressionUsesVariable(*expr, "temp_s0"));
  EXPECT_FALSE(ExpressionUsesVariable(*expr, "arg1"));

  std::vector<std::string> vars;
  absl::flat_hash_set<std::string> seen;
  auto stmt = CStatement::Return(std::move(expr));
  CollectUsedVariables(*stmt, &vars, &seen);
  EXPECT_THAT(vars, ElementsAre("temp_s0", "v0"));
  EXPECT_TRUE(StatementReferencesVariable(*stmt, "v0"));

  auto rewritten = SubstituteIdentifierInStatement(std::move(stmt), "temp_s0", "arg0");
  EXPECT_EQ(rewritten->ToString(0), "return (arg0 + (arg0 * v0));\n");
}

TEST(CAstUtilsTest, DetectsAddressOfAndVariableModifications) {
  auto addr_expr = CExpression::Cast(CType::S32().MakePointer(),
                                     CExpression::Unary("&", CExpression::Identifier("var_sp_16")));
  EXPECT_TRUE(ExpressionTakesAddressOf(*addr_expr, "var_sp_16"));
  EXPECT_FALSE(ExpressionTakesAddressOf(*addr_expr, "var_sp_20"));

  auto deref_store = CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Unary("*", std::move(addr_expr)), CExpression::Identifier("arg0")));
  EXPECT_TRUE(StatementTakesAddressOf(*deref_store, "var_sp_16"));
  EXPECT_FALSE(StatementModifiesVariable(*deref_store, "var_sp_16"));

  auto direct_assign = CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("arg0"), CExpression::Integer(42)));
  EXPECT_TRUE(StatementModifiesVariable(*direct_assign, "arg0"));
  EXPECT_FALSE(StatementModifiesVariable(*direct_assign, "arg1"));
}

TEST(CAstUtilsTest, PreservesFloatBitcastWrapperInCallArgumentsWhenSubstitutingF32Deref) {
  auto f32_load = CExpression::Unary(
      "*", CExpression::Cast(CType::F32().MakePointer(), CExpression::Identifier("arg1")));
  std::vector<std::unique_ptr<CExpression>> call_args;
  call_args.push_back(CExpression::Identifier("arg0"));
  call_args.push_back(CExpression::Unary("*(s32*)&", CExpression::Identifier("f2")));
  auto call = CExpression::Call(CExpression::Identifier("func_80001000"), std::move(call_args));

  auto substituted_call = SubstituteIdentifier(*call, "f2", *f32_load);
  EXPECT_EQ(substituted_call->ToString(), "func_80001000(arg0, *(s32*)&*(f32*)arg1)");

  // Outside a call argument, *(s32*)&f2 collapses directly to *(s32*)arg1.
  auto non_call = CExpression::Unary("*(s32*)&", CExpression::Identifier("f2"));
  auto substituted_non_call = SubstituteIdentifier(*non_call, "f2", *f32_load);
  EXPECT_EQ(substituted_non_call->ToString(), "*(s32*)arg1");
}

}  // namespace
}  // namespace rom_nom_nom
