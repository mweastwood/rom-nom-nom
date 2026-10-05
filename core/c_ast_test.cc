#include "core/c_ast.h"

#include <memory>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

TEST(CAstTest, CTypeBasic) {
  CType void_type = CType::Void();
  EXPECT_EQ(void_type.ToString(), "void");

  CType s32_type = CType::S32();
  EXPECT_EQ(s32_type.ToString(), "s32");

  CType s32_ptr = s32_type.MakePointer();
  EXPECT_EQ(s32_ptr.ToString(), "s32*");

  CType s32_ptr_ptr = s32_ptr.MakePointer();
  EXPECT_EQ(s32_ptr_ptr.ToString(), "s32**");

  CType custom = CType::Named("Gfx");
  EXPECT_EQ(custom.ToString(), "Gfx");
  EXPECT_EQ(custom.MakePointer().ToString(), "Gfx*");
}

TEST(CAstTest, IntegerLiteralExpressions) {
  auto lit1 = CExpression::Integer(42);
  EXPECT_EQ(lit1->Kind(), CExpressionKind::kIntegerLiteral);
  EXPECT_EQ(lit1->ToString(), "42");

  auto lit_hex = CExpression::Integer(0x801F0000, /*is_hex=*/true);
  EXPECT_EQ(lit_hex->ToString(), "0x801F0000");

  auto lit_unsigned = CExpression::Integer(100, /*is_hex=*/false, /*is_unsigned=*/true);
  EXPECT_EQ(lit_unsigned->ToString(), "100U");

  auto clone = lit_hex->Clone();
  ASSERT_THAT(clone, NotNull());
  EXPECT_EQ(clone->ToString(), "0x801F0000");
}

TEST(CAstTest, FloatLiteralExpressions) {
  auto f1 = CExpression::Float(3.14, /*is_float=*/true);
  EXPECT_EQ(f1->Kind(), CExpressionKind::kFloatLiteral);
  EXPECT_EQ(f1->ToString(), "3.14f");

  auto f_int = CExpression::Float(5.0, /*is_float=*/true);
  EXPECT_EQ(f_int->ToString(), "5.0f");

  auto d1 = CExpression::Float(2.5, /*is_float=*/false);
  EXPECT_EQ(d1->ToString(), "2.5");
}

TEST(CAstTest, IdentifierAndStringExpressions) {
  auto id = CExpression::Identifier("g_counter");
  EXPECT_EQ(id->Kind(), CExpressionKind::kIdentifier);
  EXPECT_EQ(id->ToString(), "g_counter");

  auto str = CExpression::String("Hello, World!");
  EXPECT_EQ(str->Kind(), CExpressionKind::kStringLiteral);
  EXPECT_EQ(str->ToString(), "\"Hello, World!\"");
}

TEST(CAstTest, UnaryAndBinaryExpressions) {
  auto var_x = CExpression::Identifier("x");
  auto neg_x = CExpression::Unary("-", std::move(var_x));
  EXPECT_EQ(neg_x->Kind(), CExpressionKind::kUnaryExpression);
  EXPECT_EQ(neg_x->ToString(), "-x");

  auto add = CExpression::Binary("+", CExpression::Identifier("a"), CExpression::Identifier("b"));
  EXPECT_EQ(add->Kind(), CExpressionKind::kBinaryExpression);
  EXPECT_EQ(add->ToString(), "(a + b)");

  auto deref = CExpression::Unary("*", CExpression::Identifier("ptr"));
  EXPECT_EQ(deref->ToString(), "*ptr");

  auto address_of = CExpression::Unary("&", CExpression::Identifier("val"));
  EXPECT_EQ(address_of->ToString(), "&val");
}

TEST(CAstTest, AssignmentAndCallExpressions) {
  auto assign =
      CExpression::Assignment("=", CExpression::Identifier("result"), CExpression::Integer(10));
  EXPECT_EQ(assign->Kind(), CExpressionKind::kAssignmentExpression);
  EXPECT_EQ(assign->ToString(), "result = 10");

  std::vector<std::unique_ptr<CExpression>> args;
  args.push_back(CExpression::Identifier("a0"));
  args.push_back(CExpression::Integer(4));
  auto call = CExpression::Call(CExpression::Identifier("osWritebackDCache"), std::move(args));
  EXPECT_EQ(call->Kind(), CExpressionKind::kCallExpression);
  EXPECT_EQ(call->ToString(), "osWritebackDCache(a0, 4)");
}

TEST(CAstTest, CastMemberAndArrayIndexExpressions) {
  auto cast = CExpression::Cast(CType::U32(), CExpression::Identifier("reg"));
  EXPECT_EQ(cast->Kind(), CExpressionKind::kCastExpression);
  EXPECT_EQ(cast->ToString(), "(u32)reg");

  auto member_dot = CExpression::MemberAccess(CExpression::Identifier("obj"), "field");
  EXPECT_EQ(member_dot->ToString(), "obj.field");

  auto member_arrow = CExpression::MemberAccess(CExpression::Identifier("ptr"), "field",
                                                /*is_arrow=*/true);
  EXPECT_EQ(member_arrow->ToString(), "ptr->field");

  auto arr_idx = CExpression::ArrayIndex(CExpression::Identifier("table"), CExpression::Integer(3));
  EXPECT_EQ(arr_idx->ToString(), "table[3]");

  auto ternary = CExpression::Ternary(CExpression::Identifier("cond"), CExpression::Integer(1),
                                      CExpression::Integer(0));
  EXPECT_EQ(ternary->ToString(), "(cond ? 1 : 0)");
}

TEST(CAstTest, VariableDeclarationAndExpressionStatements) {
  auto var_decl = CStatement::VariableDeclaration(CType::S32(), "counter", CExpression::Integer(0));
  EXPECT_EQ(var_decl->Kind(), CStatementKind::kVariableDeclarationStatement);
  EXPECT_EQ(var_decl->ToString(0), "s32 counter = 0;\n");

  auto expr_stmt = CStatement::Expression(
      CExpression::Assignment("+=", CExpression::Identifier("counter"), CExpression::Integer(1)));
  EXPECT_EQ(expr_stmt->Kind(), CStatementKind::kExpressionStatement);
  EXPECT_EQ(expr_stmt->ToString(1), "    counter += 1;\n");
}

TEST(CAstTest, VariableDeclarationArray) {
  auto arr_decl = CStatement::VariableDeclaration(CType::U8(), "buffer", nullptr, 128);
  EXPECT_EQ(arr_decl->Kind(), CStatementKind::kVariableDeclarationStatement);
  EXPECT_EQ(arr_decl->ToString(0), "u8 buffer[128];\n");

  const auto* var_stmt = static_cast<const VariableDeclarationStatement*>(arr_decl.get());
  ASSERT_TRUE(var_stmt->ArraySize().has_value());
  EXPECT_EQ(*var_stmt->ArraySize(), 128u);

  auto cloned = arr_decl->Clone();
  EXPECT_EQ(cloned->ToString(0), "u8 buffer[128];\n");
}

TEST(CAstTest, ControlFlowStatements) {
  // if (x < 10) { x = 10; } else { x = 0; }
  auto cond = CExpression::Binary("<", CExpression::Identifier("x"), CExpression::Integer(10));
  auto then_body = std::make_unique<CompoundStatement>();
  then_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("x"), CExpression::Integer(10))));
  auto else_body = std::make_unique<CompoundStatement>();
  else_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("x"), CExpression::Integer(0))));

  auto if_stmt = CStatement::If(std::move(cond), std::move(then_body), std::move(else_body));
  EXPECT_EQ(if_stmt->Kind(), CStatementKind::kIfStatement);
  std::string if_str = if_stmt->ToString(0);
  EXPECT_THAT(if_str, HasSubstr("if ((x < 10)) {"));
  EXPECT_THAT(if_str, HasSubstr("else {"));

  // while (i < 5) { i = i + 1; }
  auto while_body = std::make_unique<CompoundStatement>();
  while_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("i"),
      CExpression::Binary("+", CExpression::Identifier("i"), CExpression::Integer(1)))));
  auto while_stmt = CStatement::While(
      CExpression::Binary("<", CExpression::Identifier("i"), CExpression::Integer(5)),
      std::move(while_body));
  EXPECT_EQ(while_stmt->Kind(), CStatementKind::kWhileStatement);
  EXPECT_THAT(while_stmt->ToString(0), HasSubstr("while ((i < 5)) {"));

  // return x;
  auto ret = CStatement::Return(CExpression::Identifier("x"));
  EXPECT_EQ(ret->Kind(), CStatementKind::kReturnStatement);
  EXPECT_EQ(ret->ToString(0), "return x;\n");

  // break; continue;
  EXPECT_EQ(CStatement::Break()->ToString(0), "break;\n");
  EXPECT_EQ(CStatement::Continue()->ToString(0), "continue;\n");
}

TEST(CAstTest, SwitchAndCaseStatements) {
  auto case_zero = CStatement::Case(0);
  EXPECT_EQ(case_zero->Kind(), CStatementKind::kCaseStatement);
  EXPECT_EQ(case_zero->ToString(1), "    case 0:\n");

  auto case_def = CStatement::Default();
  EXPECT_EQ(case_def->Kind(), CStatementKind::kCaseStatement);
  EXPECT_EQ(case_def->ToString(1), "    default:\n");

  std::vector<SwitchCase> cases;

  SwitchCase case_0_1;
  case_0_1.case_values = {0, 1};
  case_0_1.body = std::make_unique<CompoundStatement>();
  case_0_1.body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("y"), CExpression::Integer(10))));
  case_0_1.body->AddStatement(CStatement::Break());
  cases.push_back(std::move(case_0_1));

  SwitchCase default_case;
  default_case.is_default = true;
  default_case.body = std::make_unique<CompoundStatement>();
  default_case.body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("y"), CExpression::Integer(0))));
  default_case.body->AddStatement(CStatement::Break());
  cases.push_back(std::move(default_case));

  auto switch_stmt = CStatement::Switch(CExpression::Identifier("x"), std::move(cases));
  EXPECT_EQ(switch_stmt->Kind(), CStatementKind::kSwitchStatement);
  std::string switch_str = switch_stmt->ToString(0);
  EXPECT_THAT(switch_str, HasSubstr("switch (x) {"));
  EXPECT_THAT(switch_str, HasSubstr("case 0:\n"));
  EXPECT_THAT(switch_str, HasSubstr("case 1:\n"));
  EXPECT_THAT(switch_str, HasSubstr("y = 10;\n"));
  EXPECT_THAT(switch_str, HasSubstr("default:\n"));
  EXPECT_THAT(switch_str, HasSubstr("y = 0;\n"));

  auto cloned = switch_stmt->Clone();
  ASSERT_THAT(cloned, NotNull());
  EXPECT_EQ(cloned->Kind(), CStatementKind::kSwitchStatement);
  EXPECT_EQ(cloned->ToString(0), switch_str);
}

TEST(CAstTest, FunctionDeclarationToString) {
  std::vector<CParameter> params;
  params.push_back(CParameter{.type = CType::S32(), .name = "arg0"});
  params.push_back(CParameter{.type = CType::S32(), .name = "arg1"});

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(
      CType::S32(), "result",
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Identifier("arg1"))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("result")));

  FunctionDeclaration func(CType::S32(), "AddNumbers", std::move(params), std::move(body));

  EXPECT_EQ(func.Name(), "AddNumbers");
  EXPECT_EQ(func.ReturnType().ToString(), "s32");
  EXPECT_EQ(func.Parameters().size(), 2u);

  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("s32 AddNumbers(s32 arg0, s32 arg1) {"));
  EXPECT_THAT(code, HasSubstr("s32 result = (arg0 + arg1);"));
  EXPECT_THAT(code, HasSubstr("return result;"));
}

TEST(CAstTest, FunctionDeclarationPrototype) {
  std::vector<CParameter> parameters;
  parameters.push_back(CParameter{.type = CType::S32(), .name = "arg0"});
  parameters.push_back(CParameter{.type = CType::S32(), .name = "arg1"});

  FunctionDeclaration function_with_parameters(CType::S32(), "AddNumbers", std::move(parameters),
                                               std::make_unique<CompoundStatement>());
  EXPECT_EQ(function_with_parameters.Prototype(), "s32 AddNumbers(s32 arg0, s32 arg1);\n");

  FunctionDeclaration function_without_parameters(CType::Void(), "ClearCache", /*parameters=*/{},
                                                  std::make_unique<CompoundStatement>());
  EXPECT_EQ(function_without_parameters.Prototype(), "void ClearCache(void);\n");

  std::vector<CParameter> static_parameters;
  static_parameters.push_back(CParameter{.type = CType::S32(), .name = "size"});
  FunctionDeclaration static_function_with_parameters(
      CType::U8().MakePointer(), "AllocateBlock", std::move(static_parameters),
      std::make_unique<CompoundStatement>(), /*is_static=*/true);
  EXPECT_EQ(static_function_with_parameters.Prototype(), "static u8* AllocateBlock(s32 size);\n");

  FunctionDeclaration static_function_without_parameters(
      CType::Void(), "ResetState", /*parameters=*/{}, std::make_unique<CompoundStatement>(),
      /*is_static=*/true);
  EXPECT_EQ(static_function_without_parameters.Prototype(), "static void ResetState(void);\n");
}

TEST(CAstTest, CollectReferencedSymbolsLocalOnly) {
  std::vector<CParameter> parameters;
  parameters.push_back(CParameter{.type = CType::S32(), .name = "arg0"});
  parameters.push_back(CParameter{.type = CType::S32(), .name = "arg1"});

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(
      CType::S32(), "result",
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Identifier("arg1"))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("result")));

  FunctionDeclaration func(CType::S32(), "AddNumbers", std::move(parameters), std::move(body));

  ReferencedSymbols symbols = CollectReferencedSymbols(func);

  EXPECT_EQ(symbols.local_variables, (std::set<std::string>{"arg0", "arg1", "result"}));
  EXPECT_TRUE(symbols.external_functions.empty());
  EXPECT_TRUE(symbols.external_data.empty());
}

TEST(CAstTest, CollectReferencedSymbolsExternalCallsAndData) {
  std::vector<CParameter> parameters;
  parameters.push_back(CParameter{.type = CType::S32(), .name = "arg0"});

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "local_var"));

  // Call external function: AlphaHelper(arg0, g_audio_status)
  std::vector<std::unique_ptr<CExpression>> alpha_args;
  alpha_args.push_back(CExpression::Identifier("arg0"));
  alpha_args.push_back(CExpression::Identifier("g_audio_status"));
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("AlphaHelper"), std::move(alpha_args))));

  // Assignment: local_var = kAudioMinPitch
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("local_var"), CExpression::Identifier("kAudioMinPitch"))));

  // Assignment: D_80100000 = sizeof(u32)
  std::vector<std::unique_ptr<CExpression>> sizeof_args;
  sizeof_args.push_back(CExpression::Identifier("u32"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_80100000"),
      CExpression::Call(CExpression::Identifier("sizeof"), std::move(sizeof_args)))));

  // Member access: player->flags (member 'flags' should not be recorded as external symbol)
  body->AddStatement(CStatement::Expression(
      CExpression::MemberAccess(CExpression::Identifier("player"), "flags", /*is_arrow=*/true)));

  FunctionDeclaration func(CType::Void(), "ProcessAudio", std::move(parameters), std::move(body));

  ReferencedSymbols symbols = CollectReferencedSymbols(func);

  EXPECT_EQ(symbols.local_variables, (std::set<std::string>{"arg0", "local_var"}));
  EXPECT_EQ(symbols.external_functions, (std::set<std::string>{"AlphaHelper"}));
  EXPECT_EQ(symbols.external_data,
            (std::set<std::string>{"D_80100000", "g_audio_status", "kAudioMinPitch", "player"}));
  EXPECT_EQ(symbols.external_data.count("flags"), 0u);
  EXPECT_EQ(symbols.external_functions.count("sizeof"), 0u);
}

TEST(CAstTest, CollectReferencedSymbolsNestedStructures) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "i"));

  // If statement with external condition and branch
  auto then_branch = std::make_unique<CompoundStatement>();
  std::vector<std::unique_ptr<CExpression>> call_args;
  then_branch->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("ExternalAction"), std::move(call_args))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("<", CExpression::Identifier("i"), CExpression::Identifier("g_limit")),
      std::move(then_branch)));

  // While loop
  auto while_body = std::make_unique<CompoundStatement>();
  while_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "+=", CExpression::Identifier("i"), CExpression::Identifier("kStepSize"))));
  body->AddStatement(CStatement::While(
      CExpression::Binary("!=", CExpression::Identifier("g_status"), CExpression::Integer(0)),
      std::move(while_body)));

  FunctionDeclaration func(CType::Void(), "LoopHandler", /*parameters=*/{}, std::move(body));

  ReferencedSymbols symbols = CollectReferencedSymbols(func);

  EXPECT_EQ(symbols.local_variables, (std::set<std::string>{"i"}));
  EXPECT_EQ(symbols.external_functions, (std::set<std::string>{"ExternalAction"}));
  EXPECT_EQ(symbols.external_data, (std::set<std::string>{"g_limit", "g_status", "kStepSize"}));
}

TEST(CAstTest, CollectReferencedSymbolsFiltersNonIdentifiers) {
  auto body = std::make_unique<CompoundStatement>();

  // Expressions containing invalid identifiers (e.g. starting with digits or containing symbols)
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("123bad"), CExpression::Identifier("&D_80100000"))));

  // Call with non-identifier callee
  std::vector<std::unique_ptr<CExpression>> call_args;
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("0invalid_call"), std::move(call_args))));

  // Valid identifier to verify collection still works
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("valid_data"), CExpression::Integer(42))));

  FunctionDeclaration func(CType::Void(), "InvalidIdentTest", /*parameters=*/{}, std::move(body));

  ReferencedSymbols symbols = CollectReferencedSymbols(func);

  EXPECT_TRUE(symbols.external_functions.empty());
  EXPECT_EQ(symbols.external_data, (std::set<std::string>{"valid_data"}));
  EXPECT_EQ(symbols.external_data.count("123bad"), 0u);
  EXPECT_EQ(symbols.external_data.count("&D_80100000"), 0u);
}

}  // namespace
}  // namespace rom_nom_nom
