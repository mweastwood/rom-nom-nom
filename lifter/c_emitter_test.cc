#include "lifter/c_emitter.h"

#include <memory>
#include <string>
#include <vector>

#include "core/c_ast.h"
#include "core/symbol_registry.h"
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

  SymbolIndex symbol_index = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "AddressTakenFunction" address: 0x80010000 type: SYMBOL_FUNC }
    entries { name: "g_audio_state" address: 0x80020000 type: SYMBOL_DATA }
  )")
                                 .value();
  translation_unit.symbol_index = &symbol_index;

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32().MakePointer(), "function_pointer"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32().MakePointer(), "stack_pointer_reference"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "var_sp_16"));

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

  // Test address-taken external function and address-taken stack variable
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("function_pointer"),
      CExpression::Unary("&", CExpression::Identifier("AddressTakenFunction")))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("stack_pointer_reference"),
                              CExpression::Unary("&", CExpression::Identifier("var_sp_16")))));

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
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 AddressTakenFunction();\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 ExternalHelper();\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 func_80012345();\n"));

  // Stack and local variables should not be emitted as extern
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 var_sp_16();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 var_sp_16;")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 function_pointer();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 function_pointer;")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 stack_pointer_reference();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 stack_pointer_reference;")));

  // Check external data declarations
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 D_80100000;\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 g_audio_state;\n"));

  // Check function prototype
  EXPECT_THAT(emitted_code, HasSubstr("s32 LocalFunction(s32 arg0);\n"));

  // Defined function should not have an extern declaration
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 LocalFunction();")));

  // Keywords should not have extern declarations
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 if();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 return();")));
}

TEST(CEmitterTest, EmitTranslationUnitWithSemanticSymbolClassification) {
  CTranslationUnit translation_unit;
  translation_unit.includes = {"types.h"};

  SymbolIndex symbol_index = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "kAudioMinPitch" address: 0x80040000 type: SYMBOL_DATA }
    entries { name: "kAudioMaxPitch" address: 0x80040008 type: SYMBOL_DATA }
    entries { name: "AudioProcess" address: 0x80050000 type: SYMBOL_FUNC }
  )")
                                 .value();
  translation_unit.symbol_index = &symbol_index;

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32().MakePointer(), "pitch_ptr"));
  body->AddStatement(CStatement::VariableDeclaration(CType::Void().MakePointer(), "proc_ptr"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "max_pitch"));

  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("pitch_ptr"),
                              CExpression::Unary("&", CExpression::Identifier("kAudioMinPitch")))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("proc_ptr"),
                              CExpression::Unary("&", CExpression::Identifier("AudioProcess")))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("max_pitch"), CExpression::Identifier("kAudioMaxPitch"))));
  body->AddStatement(CStatement::Return());

  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "TestAudio", /*parameters=*/{}, std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;

  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  // Address-taken function is correctly recognized as SYMBOL_FUNC
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 AudioProcess();\n"));
  // Constants and data symbols (even when address is taken) are correctly recognized as SYMBOL_DATA
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 kAudioMaxPitch;\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 kAudioMinPitch;\n"));
  // Constants must NOT be declared as functions
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 kAudioMinPitch();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 kAudioMaxPitch();")));
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
      "extern s32 AlphaHelper();\n"
      "extern s32 BetaHelper();\n\n";
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
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 LocalFirst();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 LocalSecond();")));

  // Verify keywords are not declared extern
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 while();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 sizeof();")));

  // Verify layout order: extern funcs before extern data, extern data before prototypes,
  // prototypes before function implementations
  size_t extern_func_pos = emitted_code.find("extern s32 AlphaHelper();");
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

TEST(CEmitterTest, EmitTranslationUnitSkipsN64Intrinsics) {
  CTranslationUnit translation_unit;
  auto body = std::make_unique<CompoundStatement>();

  std::vector<std::unique_ptr<CExpression>> args;
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("__n64_hazard_barrier"), std::move(args))));
  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  translation_unit.functions.push_back(
      FunctionDeclaration(CType::S32(), "IntrinsicUser", {}, std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 __n64_hazard_barrier();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern void __n64_hazard_barrier();")));
}

TEST(CEmitterTest, EmitTranslationUnitDetectsFloatReturnTypes) {
  CTranslationUnit translation_unit;
  auto body = std::make_unique<CompoundStatement>();

  // f0 = GetFloat();
  std::vector<std::unique_ptr<CExpression>> float_args;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("f0"),
      CExpression::Call(CExpression::Identifier("GetFloat"), std::move(float_args)))));

  // f0_d = GetDouble();
  std::vector<std::unique_ptr<CExpression>> double_args;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("f0_d"),
      CExpression::Call(CExpression::Identifier("GetDouble"), std::move(double_args)))));

  // NormalIntFunc();
  std::vector<std::unique_ptr<CExpression>> int_args;
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("NormalIntFunc"), std::move(int_args))));

  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  translation_unit.functions.push_back(
      FunctionDeclaration(CType::S32(), "CallerFunc", {}, std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  EXPECT_THAT(emitted_code, HasSubstr("extern f32 GetFloat();\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern f64 GetDouble();\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 NormalIntFunc();\n"));
}

TEST(CEmitterTest, EmitTranslationUnitSkipsHeaderProvidedFunctions) {
  CTranslationUnit translation_unit;
  translation_unit.header_provided_functions = {"sqrtf", "fabsf"};

  auto body = std::make_unique<CompoundStatement>();

  std::vector<std::unique_ptr<CExpression>> args1;
  args1.push_back(CExpression::Identifier("f0"));
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("sqrtf"), std::move(args1))));

  std::vector<std::unique_ptr<CExpression>> args2;
  args2.push_back(CExpression::Identifier("f0"));
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("fabsf"), std::move(args2))));

  std::vector<std::unique_ptr<CExpression>> args3;
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("NormalExternalFunc"), std::move(args3))));

  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  translation_unit.functions.push_back(
      FunctionDeclaration(CType::S32(), "MathUser", {}, std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;

  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  // Functions in header_provided_functions must NOT be emitted as extern declarations
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 sqrtf();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern f32 sqrtf(")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern s32 fabsf();")));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("extern f32 fabsf(")));

  // External functions not in header_provided_functions must be declared extern
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 NormalExternalFunc();\n"));
}

TEST(CEmitterTest, EmitTranslationUnitEmitsTypedAndArrayExternDataDeclarations) {
  CTranslationUnit translation_unit;
  translation_unit.includes = {"types.h"};

  auto body = std::make_unique<CompoundStatement>();
  auto short_sym = std::make_unique<IdentifierExpression>("g_halfword", CType::S16());
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", std::move(short_sym), CExpression::Integer(12))));

  auto arr_sym = std::make_unique<IdentifierExpression>("g_word_table", CType::S32(),
                                                        /*is_array_symbol=*/true);
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::ArrayIndex(std::move(arr_sym), CExpression::Integer(3)),
      CExpression::Integer(99))));

  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "WriteGlobals", {}, std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  EXPECT_THAT(emitted_code, HasSubstr("extern s16 g_halfword;\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 g_word_table[];\n"));
}

}  // namespace
}  // namespace rom_nom_nom
