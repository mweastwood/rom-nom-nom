#include "lifter/c_emitter.h"

#include <memory>
#include <string>
#include <vector>

#include "core/c_ast.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;

TEST(CEmitterTest, EmitSimpleFunctionWithoutClangFormat) {
  std::vector<CParameter> params;
  params.push_back(CParameter{.type = CType::S32(), .name = "arg0"});
  params.push_back(CParameter{.type = CType::S32(), .name = "arg1"});

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(
      CType::S32(), "result",
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Identifier("arg1"))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("result")));

  FunctionDeclaration func(CType::S32(), "AddNumbers", std::move(params), std::move(body));

  CEmitterOptions options;
  options.includes = {"types.h", "common.h"};
  options.format_with_clang = false;

  std::string emitted = CEmitter::EmitFunction(func, options);
  EXPECT_THAT(emitted, HasSubstr("#include \"types.h\""));
  EXPECT_THAT(emitted, HasSubstr("#include \"common.h\""));
  EXPECT_THAT(emitted, HasSubstr("s32 AddNumbers(s32 arg0, s32 arg1) {"));
  EXPECT_THAT(emitted, HasSubstr("s32 result = (arg0 + arg1);"));
  EXPECT_THAT(emitted, HasSubstr("return result;"));
}

TEST(CEmitterTest, EmitTranslationUnit) {
  CTranslationUnit tu;
  tu.includes = {"ultra64.h", "<string.h>"};

  std::vector<CParameter> params;
  params.push_back(CParameter{.type = CType::Void().MakePointer(), .name = "ptr"});

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::Return());

  tu.functions.push_back(
      FunctionDeclaration(CType::Void(), "DoNothing", std::move(params), std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;

  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted, HasSubstr("#include \"ultra64.h\""));
  EXPECT_THAT(emitted, HasSubstr("#include <string.h>"));
  EXPECT_THAT(emitted, HasSubstr("void DoNothing(void* ptr) {"));
  EXPECT_THAT(emitted, HasSubstr("return;"));
}

TEST(CEmitterTest, EmitTranslationUnitWithExternalDeclarationsAndPrototypes) {
  CTranslationUnit translation_unit;
  translation_unit.includes = {"types.h"};

  auto body = std::make_unique<CompoundStatement>();
  // Call ExternalHelper(), func_80012345(), and LocalFunction(arg0)
  std::vector<std::unique_ptr<CExpression>> helper_args;
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("ExternalHelper"), std::move(helper_args))));

  std::vector<std::unique_ptr<CExpression>> asm_func_args;
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("func_80012345"), std::move(asm_func_args))));

  // Reference data symbols D_80100000 and g_audio_state
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_audio_state"), CExpression::Identifier("D_80100000"))));

  // Control flow keywords should not be identified as external functions
  auto cond = CExpression::Binary(">", CExpression::Identifier("arg0"), CExpression::Integer(0));
  auto if_body = std::make_unique<CompoundStatement>();
  if_body->AddStatement(CStatement::Return(CExpression::Identifier("arg0")));
  body->AddStatement(CStatement::If(std::move(cond), std::move(if_body)));

  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  std::vector<CParameter> parameters;
  parameters.push_back(CParameter{.type = CType::S32(), .name = "arg0"});
  translation_unit.functions.push_back(
      FunctionDeclaration(CType::S32(), "LocalFunction", std::move(parameters), std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;

  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  // Check external function declarations
  EXPECT_THAT(emitted_code, HasSubstr("extern void ExternalHelper();\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern void func_80012345();\n"));

  // Check external data declarations
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 D_80100000;\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 g_audio_state;\n"));

  // Check function prototype
  EXPECT_THAT(emitted_code, HasSubstr("s32 LocalFunction(s32 arg0);\n"));

  // Defined function should not have an extern declaration
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern void LocalFunction();")));

  // Keywords should not have extern declarations
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern void if();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern void return();")));
}

TEST(CEmitterTest, EmitTranslationUnitDeduplicationAndOrdering) {
  CTranslationUnit translation_unit;
  translation_unit.includes = {"types.h"};

  // Function 1: Calls BetaHelper(), AlphaHelper(), BetaHelper() (duplicate),
  // references D_80200000 (duplicate), D_80100000, and calls LocalSecond().
  // Also contains while, for, switch, sizeof keywords.
  auto body1 = std::make_unique<CompoundStatement>();

  std::vector<std::unique_ptr<CExpression>> beta_args1;
  body1->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("BetaHelper"), std::move(beta_args1))));

  std::vector<std::unique_ptr<CExpression>> alpha_args;
  body1->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("AlphaHelper"), std::move(alpha_args))));

  std::vector<std::unique_ptr<CExpression>> beta_args2;
  body1->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("BetaHelper"), std::move(beta_args2))));

  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_80200000"), CExpression::Identifier("D_80100000"))));
  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "+=", CExpression::Identifier("D_80200000"), CExpression::Integer(1))));

  std::vector<std::unique_ptr<CExpression>> local2_args;
  body1->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("LocalSecond"), std::move(local2_args))));

  // while loop
  auto while_body = std::make_unique<CompoundStatement>();
  while_body->AddStatement(CStatement::Break());
  body1->AddStatement(CStatement::While(CExpression::Integer(1), std::move(while_body)));

  // sizeof expression call
  std::vector<std::unique_ptr<CExpression>> sizeof_args;
  sizeof_args.push_back(CExpression::Identifier("u32"));
  body1->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("sizeof"), std::move(sizeof_args))));

  body1->AddStatement(CStatement::Return());

  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "LocalFirst", /*parameters=*/{}, std::move(body1)));

  // Function 2: LocalSecond, calls LocalFirst()
  auto body2 = std::make_unique<CompoundStatement>();
  std::vector<std::unique_ptr<CExpression>> local1_args;
  body2->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("LocalFirst"), std::move(local1_args))));
  body2->AddStatement(CStatement::Return());

  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "LocalSecond", /*parameters=*/{}, std::move(body2)));

  CEmitterOptions options;
  options.format_with_clang = false;

  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  // External functions are emitted in alphabetical order and deduplicated
  std::string expected_extern_funcs =
      "extern void AlphaHelper();\n"
      "extern void BetaHelper();\n\n";
  EXPECT_THAT(emitted_code, HasSubstr(expected_extern_funcs));

  // External data symbols are emitted in alphabetical order and deduplicated
  std::string expected_extern_data =
      "extern s32 D_80100000;\n"
      "extern s32 D_80200000;\n\n";
  EXPECT_THAT(emitted_code, HasSubstr(expected_extern_data));

  // Forward prototypes are emitted for both local functions
  std::string expected_prototypes =
      "void LocalFirst(void);\n"
      "void LocalSecond(void);\n\n";
  EXPECT_THAT(emitted_code, HasSubstr(expected_prototypes));

  // Verify that neither LocalFirst nor LocalSecond is declared extern
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern void LocalFirst();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern void LocalSecond();")));

  // Verify keywords are not declared extern
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern void while();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern void sizeof();")));

  // Verify layout order: extern funcs before extern data, extern data before prototypes,
  // prototypes before function implementations
  size_t extern_func_pos = emitted_code.find("extern void AlphaHelper();");
  size_t extern_data_pos = emitted_code.find("extern s32 D_80100000;");
  size_t prototype_pos = emitted_code.find("void LocalFirst(void);");
  size_t body_pos = emitted_code.find("void LocalFirst(void) {");

  EXPECT_LT(extern_func_pos, extern_data_pos);
  EXPECT_LT(extern_data_pos, prototype_pos);
  EXPECT_LT(prototype_pos, body_pos);
}

TEST(CEmitterTest, FormatCodeWithClangFormatOrFallback) {
  std::string messy_code = "void   MessyFunc (  s32   x ){\nif(x>0){return    x;}\nreturn 0;\n}\n";
  std::string formatted = CEmitter::FormatCode(messy_code);

  EXPECT_THAT(formatted, HasSubstr("MessyFunc"));
  EXPECT_THAT(formatted, HasSubstr("return"));
}

}  // namespace
}  // namespace rom_nom_nom
