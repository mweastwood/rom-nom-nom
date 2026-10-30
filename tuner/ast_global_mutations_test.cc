#include "tuner/ast_global_mutations.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "core/c_ast.h"
#include "gtest/gtest.h"
#include "tuner/ast_mutator.h"

namespace rom_nom_nom {
namespace {

TEST(AstGlobalMutationsTest, DirectTypedGlobalAndArrayPromotions) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32().MakePointer(), "temp_s1"));
  // temp_s1 = &D_800B0000;
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s1"),
                              CExpression::Unary("&", CExpression::Identifier("D_800B0000")))));
  // *(s32 *)(temp_s1 + 8) = *(s16 *)&D_800A0000;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(),
                                 CExpression::Binary("+", CExpression::Identifier("temp_s1"),
                                                     CExpression::Integer(8)))),
      CExpression::Unary("*", CExpression::Cast(CType::S16().MakePointer(),
                                                CExpression::Unary("&", CExpression::Identifier(
                                                                            "D_800A0000")))))));

  FunctionDeclaration func(CType::Void(), "Globals", {}, std::move(body));

  AstMutationStep direct_step;
  direct_step.set_kind(AST_MUTATION_DIRECT_TYPED_GLOBAL);
  direct_step.set_site_index(0);
  FunctionDeclaration after_direct = AstMutator::ApplyMutation(func, direct_step);
  EXPECT_TRUE(absl::StrContains(after_direct.ToString(), "= D_800A0000;"))
      << after_direct.ToString();

  AstMutationStep array_step;
  array_step.set_kind(AST_MUTATION_GLOBAL_ARRAY_ACCESS);
  array_step.set_site_index(0);
  FunctionDeclaration after_array = AstMutator::ApplyMutation(after_direct, array_step);
  EXPECT_TRUE(absl::StrContains(after_array.ToString(), "D_800B0000[2] = D_800A0000;"))
      << after_array.ToString();

  ReferencedSymbols refs = CollectReferencedSymbols(after_array);
  ASSERT_TRUE(refs.inferred_data_types.count("D_800A0000") > 0);
  EXPECT_EQ(refs.inferred_data_types.at("D_800A0000").ToString(), "s16");
  ASSERT_TRUE(refs.inferred_data_types.count("D_800B0000") > 0);
  EXPECT_EQ(refs.inferred_data_types.at("D_800B0000").ToString(), "s32");
  EXPECT_TRUE(refs.array_data_symbols.count("D_800B0000") > 0);
}

TEST(AstGlobalMutationsTest, VolatileGlobalAccessDiscoveryAndRewriting) {
  // D_800E9BA0 = 1;
  // D_800E9BA0 = 2;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(1))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(2))));

  FunctionDeclaration func(CType::Void(), "VolatileStoreFunc", {}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(func);
  bool found_decl_single = false;
  bool found_decl_bulk = false;
  bool found_cast_single = false;
  bool found_cast_bulk = false;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_VOLATILE_GLOBAL_ACCESS) {
      if (step.param() == 0) found_decl_single = true;
      if (step.param() == 1) found_decl_bulk = true;
      if (step.param() == 2) found_cast_single = true;
      if (step.param() == 3) found_cast_bulk = true;
    }
  }
  EXPECT_TRUE(found_decl_single);
  EXPECT_TRUE(found_decl_bulk);
  EXPECT_TRUE(found_cast_single);
  EXPECT_TRUE(found_cast_bulk);

  // Test direct volatile declaration (param=0)
  AstMutationStep decl_step;
  decl_step.set_kind(AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  decl_step.set_site_index(0);
  decl_step.set_param(0);
  FunctionDeclaration decl_mutated = AstMutator::ApplyMutation(func, decl_step);
  const auto& first_stmt =
      static_cast<const ExpressionStatement&>(*decl_mutated.Body().Statements()[0]);
  const auto& assign = static_cast<const AssignmentExpression&>(first_stmt.Expression());
  const auto& id = static_cast<const IdentifierExpression&>(assign.Lhs());
  EXPECT_TRUE(id.SymbolType().has_value());
  EXPECT_EQ(id.SymbolType()->ToString(), "volatile s32");

  // Test inline cast (param=2)
  AstMutationStep cast_step;
  cast_step.set_kind(AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  cast_step.set_site_index(0);
  cast_step.set_param(2);
  FunctionDeclaration cast_mutated = AstMutator::ApplyMutation(func, cast_step);
  std::string code = cast_mutated.ToString();
  EXPECT_TRUE(absl::StrContains(code, "*(volatile s32*)&D_800E9BA0 = 1;")) << code;
  EXPECT_TRUE(absl::StrContains(code, "*(volatile s32*)&D_800E9BA0 = 2;")) << code;
}

TEST(AstGlobalMutationsTest, VolatileGlobalAccessRejectsInterveningCallOrRead) {
  // With call intervening:
  // D_800E9BA0 = 1;
  // Helper();
  // D_800E9BA0 = 2;
  auto body1 = std::make_unique<CompoundStatement>();
  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(1))));
  body1->AddStatement(
      CStatement::Expression(CExpression::Call(CExpression::Identifier("Helper"), {})));
  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(2))));

  FunctionDeclaration func1(CType::Void(), "InterveningCallFunc", {}, std::move(body1));
  auto mutations1 = AstMutator::DiscoverMutations(func1);
  for (const auto& step : mutations1) {
    EXPECT_NE(step.kind(), AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  }

  // With read intervening:
  // D_800E9BA0 = 1;
  // x = D_800E9BA0;
  // D_800E9BA0 = 2;
  auto body2 = std::make_unique<CompoundStatement>();
  body2->AddStatement(CStatement::VariableDeclaration(CType::S32(), "x"));
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(1))));
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("x"), CExpression::Identifier("D_800E9BA0"))));
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(2))));

  FunctionDeclaration func2(CType::Void(), "InterveningReadFunc", {}, std::move(body2));
  auto mutations2 = AstMutator::DiscoverMutations(func2);
  for (const auto& step : mutations2) {
    EXPECT_NE(step.kind(), AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  }
}

TEST(AstGlobalMutationsTest, VolatileGlobalAccessOgreBattleFunc800737A0) {
  // void func_800737A0(void)
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"), CExpression::Identifier("D_800E9BA0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"), CExpression::Integer(0xE7000000))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(8)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(16)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"), CExpression::Integer(0xDB060000))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(8))),
                         true),
      CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(24)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(32)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("v0")), true),
      CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(16))),
                         true),
      CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"), CExpression::Integer(0xDE000000))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Unary("&", CExpression::Identifier("D_80186E90"), true))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(28))),
                         true),
      CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(40)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Unary("&", CExpression::Identifier("D_801869E8"), true))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(4))),
                         true),
      CExpression::Integer(0))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(12))),
                         true),
      CExpression::Integer(0))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(20))),
                         true),
      CExpression::Integer(0))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(24))),
                         true),
      CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(32))),
                         true),
      CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(36))),
                         true),
      CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Return());

  FunctionDeclaration func(CType::Void(), "func_800737A0", {}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(func, 64);
  bool found_volatile = false;
  AstMutationStep volatile_step;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_VOLATILE_GLOBAL_ACCESS) {
      found_volatile = true;
      volatile_step = step;
      break;
    }
  }
  EXPECT_TRUE(found_volatile);
  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, volatile_step);
  std::string output = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(output, "D_800E9BA0 = v1;"));
}

TEST(AstGlobalMutationsTest, GlobalArrayAddressingPreservesUnscaledIntegerAddition) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg2"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg2"),
      CExpression::Unary("&", CExpression::Identifier("D_800ABC60"), /*is_prefix=*/true))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Identifier("arg2"))),
                         /*is_prefix=*/true))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration func(CType::S32(), "func_8009C670_style", {}, std::move(body));
  AstMutationStep step;
  step.set_kind(AST_MUTATION_GLOBAL_ARRAY_ACCESS);
  step.set_site_index(0);
  step.set_param(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string output = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(output, "(s32)&D_800ABC60[0]")) << output;
}

}  // namespace
}  // namespace rom_nom_nom
