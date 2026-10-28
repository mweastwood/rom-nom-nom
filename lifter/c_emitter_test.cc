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

TEST(CEmitterTest, EmitTranslationUnitResolvesConflictingGlobalSymbolTypesAcrossFunctions) {
  CTranslationUnit translation_unit;
  translation_unit.includes = {"types.h"};

  // Function 1 uses g_shared_scalar as s16 and g_shared_array as s16[]
  auto body1 = std::make_unique<CompoundStatement>();
  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", std::make_unique<IdentifierExpression>("g_shared_scalar", CType::S16()),
      CExpression::Integer(10))));
  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::ArrayIndex(std::make_unique<IdentifierExpression>("g_shared_array", CType::S16(),
                                                                     /*is_array_symbol=*/true),
                              CExpression::Integer(1)),
      CExpression::Integer(20))));
  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "FuncOne", {}, std::move(body1)));

  // Function 2 uses g_shared_scalar as conflicting u8 and g_shared_array as conflicting s32[]
  auto body2 = std::make_unique<CompoundStatement>();
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", std::make_unique<IdentifierExpression>("g_shared_scalar", CType::U8()),
      CExpression::Integer(30))));
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::ArrayIndex(std::make_unique<IdentifierExpression>("g_shared_array", CType::S32(),
                                                                     /*is_array_symbol=*/true),
                              CExpression::Integer(2)),
      CExpression::Integer(40))));
  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "FuncTwo", {}, std::move(body2)));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  // The winning extern declarations use the resolved translation-unit-wide types
  EXPECT_THAT(emitted_code, HasSubstr("extern u8 g_shared_scalar;\n"));
  EXPECT_THAT(emitted_code, HasSubstr("extern s32 g_shared_array[];\n"));

  // Conflicting accesses in FuncOne are safely wrapped with pointer casts while FuncTwo accesses
  // directly
  EXPECT_THAT(emitted_code, HasSubstr("*(s16*)&g_shared_scalar = 10;"));
  EXPECT_THAT(emitted_code, HasSubstr("((s16*)g_shared_array)[1] = 20;"));
  EXPECT_THAT(emitted_code, HasSubstr("g_shared_scalar = 30;"));
  EXPECT_THAT(emitted_code, HasSubstr("g_shared_array[2] = 40;"));
}

TEST(CEmitterTest, EmitTranslationUnitEmitsVolatileGlobalDeclarations) {
  CTranslationUnit translation_unit;
  translation_unit.includes = {"types.h"};

  auto body = std::make_unique<CompoundStatement>();
  auto volatile_global =
      std::make_unique<IdentifierExpression>("D_800737A0", CType::Named("volatile s32"));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", std::move(volatile_global), CExpression::Integer(42))));
  body->AddStatement(CStatement::Return());

  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "WriteVolatileGlobal", {}, std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  EXPECT_THAT(emitted_code, HasSubstr("extern volatile s32 D_800737A0;\n"));
  EXPECT_THAT(emitted_code, HasSubstr("D_800737A0 = 42;"));
}

TEST(CEmitterTest, EmitTranslationUnitResolvesUntypedGlobalSymbolAccessWithDefaultS32Cast) {
  CTranslationUnit translation_unit;
  translation_unit.includes = {"types.h"};

  // FuncOne accesses g_byte_flag as u8
  auto body1 = std::make_unique<CompoundStatement>();
  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", std::make_unique<IdentifierExpression>("g_byte_flag", CType::U8()),
      CExpression::Integer(1))));
  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "FuncOne", {}, std::move(body1)));

  // FuncTwo accesses g_byte_flag without SymbolType (untyped, default access is s32)
  auto body2 = std::make_unique<CompoundStatement>();
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", std::make_unique<IdentifierExpression>("g_byte_flag"), CExpression::Integer(2))));
  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "FuncTwo", {}, std::move(body2)));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  // The extern declaration resolves to u8
  EXPECT_THAT(emitted_code, HasSubstr("extern u8 g_byte_flag;\n"));

  // FuncOne writes directly
  EXPECT_THAT(emitted_code, HasSubstr("g_byte_flag = 1;"));

  // FuncTwo wraps untyped access with *(s32*)&g_byte_flag
  EXPECT_THAT(emitted_code, HasSubstr("*(s32*)&g_byte_flag = 2;"));
}

TEST(CEmitterTest, EmitTranslationUnitPromotesConsistentScalarCastDerefGlobals) {
  CTranslationUnit translation_unit;
  translation_unit.includes = {"types.h"};

  auto body = std::make_unique<CompoundStatement>();
  // *(u16*)&D_800AF3C6 = 7;
  auto u16_deref = CExpression::Unary(
      "*", CExpression::Cast(CType::U16().MakePointer(),
                             CExpression::Unary("&", CExpression::Identifier("D_800AF3C6"))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", std::move(u16_deref), CExpression::Integer(7))));

  // Mixed symbol: *(u8*)&D_800AF3C8 = 1; and *(u16*)&D_800AF3C8 = 2;
  auto u8_mixed = CExpression::Unary(
      "*", CExpression::Cast(CType::U8().MakePointer(),
                             CExpression::Unary("&", CExpression::Identifier("D_800AF3C8"))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", std::move(u8_mixed), CExpression::Integer(1))));
  auto u16_mixed = CExpression::Unary(
      "*", CExpression::Cast(CType::U16().MakePointer(),
                             CExpression::Unary("&", CExpression::Identifier("D_800AF3C8"))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", std::move(u16_mixed), CExpression::Integer(2))));

  body->AddStatement(CStatement::Return());
  translation_unit.functions.push_back(
      FunctionDeclaration(CType::Void(), "UpdateScalarGlobals", {}, std::move(body)));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted_code = CEmitter::EmitTranslationUnit(translation_unit, options);

  EXPECT_THAT(emitted_code, HasSubstr("extern u16 D_800AF3C6;\n"));
  EXPECT_THAT(emitted_code, HasSubstr("D_800AF3C6 = 7;"));
  EXPECT_THAT(emitted_code, ::testing::Not(HasSubstr("*(u16*)&D_800AF3C6")));

  EXPECT_THAT(emitted_code, HasSubstr("extern s32 D_800AF3C8;\n"));
  EXPECT_THAT(emitted_code, HasSubstr("*(u8*)&D_800AF3C8 = 1;"));
  EXPECT_THAT(emitted_code, HasSubstr("*(u16*)&D_800AF3C8 = 2;"));
}

TEST(CEmitterTest, EmitTypesHeaderRespectsIntrinsicFilterAndExtraDeclarations) {
  std::set<std::string> emitted_intrinsics;
  TypesHeaderOptions options;
  options.should_skip_math_intrinsic = [](std::string_view name) { return name == "sqrtf"; };
  options.emitted_math_intrinsics = &emitted_intrinsics;
  options.extra_declarations = "typedef struct { s32 x; } CustomVec;";

  std::string header = CEmitter::EmitTypesHeader(options);

  EXPECT_THAT(header, HasSubstr("typedef signed int s32;"));
  EXPECT_THAT(header, HasSubstr("UnalignedBlock16;"));
  EXPECT_THAT(header, HasSubstr("static inline f32 N64Sqrtf(f32 x)"));
  EXPECT_THAT(header, ::testing::Not(HasSubstr("static inline f32 sqrtf(f32 x)")));
  EXPECT_THAT(header, HasSubstr("typedef struct { s32 x; } CustomVec;"));
  EXPECT_TRUE(emitted_intrinsics.count("N64Sqrtf") > 0);
  EXPECT_EQ(emitted_intrinsics.count("sqrtf"), 0u);
}

TEST(CEmitterTest, EmitTranslationUnitEmitsCustomUnalignedBlockTypedefs) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  auto body = std::make_unique<CompoundStatement>();
  CType ptr10 = CType::Named("UnalignedBlock10").MakePointer();
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Unary("*", CExpression::Cast(ptr10, CExpression::Identifier("arg0"))),
      CExpression::Unary("*", CExpression::Cast(ptr10, CExpression::Identifier("arg1"))))));
  CType ptr20 = CType::Named("UnalignedBlock20").MakePointer();
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Unary("*", CExpression::Cast(ptr20, CExpression::Identifier("arg2"))),
      CExpression::Unary("*", CExpression::Cast(ptr20, CExpression::Identifier("arg3"))))));
  body->AddStatement(CStatement::Return());
  tu.functions.emplace_back(
      CType::Void(), "CopyCustomBlocks",
      std::vector<CParameter>{CParameter{CType::S32(), "arg0"}, CParameter{CType::S32(), "arg1"},
                              CParameter{CType::S32(), "arg2"}, CParameter{CType::S32(), "arg3"}},
      std::move(body));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted, HasSubstr("typedef struct { u8 b[10]; } UnalignedBlock10;"));
  EXPECT_THAT(emitted, HasSubstr("typedef struct { u8 b[20]; } UnalignedBlock20;"));
}

TEST(CEmitterTest, PromotesScalarCastDerefGlobalEvenWhenIndexedWithPointerCast) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  auto body = std::make_unique<CompoundStatement>();
  // *(u16*)&D_80205622 = 0;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::U16().MakePointer(),
                                 CExpression::Unary("&", CExpression::Identifier("D_80205622")))),
      CExpression::Integer(0))));
  // *(u16*)((u8*)&D_80205622 + arg0) = 1;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*",
          CExpression::Cast(
              CType::U16().MakePointer(),
              CExpression::Binary(
                  "+",
                  CExpression::Cast(CType::U8().MakePointer(),
                                    CExpression::Unary("&", CExpression::Identifier("D_80205622"))),
                  CExpression::Identifier("arg0")))),
      CExpression::Integer(1))));
  body->AddStatement(CStatement::Return());
  tu.functions.emplace_back(CType::Void(), "ClearIndexedAndBaseHalfword",
                            std::vector<CParameter>{CParameter{CType::S32(), "arg0"}},
                            std::move(body));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted, HasSubstr("extern u16 D_80205622;\n"));
  EXPECT_THAT(emitted, HasSubstr("D_80205622 = 0;"));
  EXPECT_THAT(emitted, ::testing::Not(HasSubstr("*(u16*)&D_80205622")));
  EXPECT_THAT(emitted, HasSubstr("*(u16*)((u8*)&D_80205622 + arg0) = 1;"));
}

TEST(CEmitterTest, PromotesMixedSignedAndUnsignedByteAndHalfwordGlobals) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  auto body = std::make_unique<CompoundStatement>();
  // *(u8*)&D_80180000 = 0;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::U8().MakePointer(),
                                 CExpression::Unary("&", CExpression::Identifier("D_80180000")))),
      CExpression::Integer(0))));
  // arg0 = *(s8*)&D_80180000;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Unary("*", CExpression::Cast(CType::S8().MakePointer(),
                                                CExpression::Unary("&", CExpression::Identifier(
                                                                            "D_80180000")))))));
  // *(u16*)&D_80180004 = 0;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::U16().MakePointer(),
                                 CExpression::Unary("&", CExpression::Identifier("D_80180004")))),
      CExpression::Integer(0))));
  // arg0 = *(s16*)&D_80180004;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Unary("*", CExpression::Cast(CType::S16().MakePointer(),
                                                CExpression::Unary("&", CExpression::Identifier(
                                                                            "D_80180004")))))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("arg0")));
  tu.functions.emplace_back(CType::S32(), "TestGlobalScalarPromotion",
                            std::vector<CParameter>{CParameter{CType::S32(), "arg0"}},
                            std::move(body));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted, HasSubstr("extern s8 D_80180000;\n"));
  EXPECT_THAT(emitted, HasSubstr("extern s16 D_80180004;\n"));
  EXPECT_THAT(emitted, HasSubstr("D_80180000 = 0;"));
  EXPECT_THAT(emitted, HasSubstr("arg0 = D_80180000;"));
  EXPECT_THAT(emitted, HasSubstr("D_80180004 = 0;"));
  EXPECT_THAT(emitted, HasSubstr("arg0 = D_80180004;"));
  EXPECT_THAT(emitted, ::testing::Not(HasSubstr("*(u8*)&D_80180000")));
  EXPECT_THAT(emitted, ::testing::Not(HasSubstr("*(u16*)&D_80180004")));
}

TEST(CEmitterTest, DisambiguatesSameBlockDuplicateCalleeSavedSymbolLoadsWithAsmBarrier) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s1", nullptr, std::nullopt, "$17"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s2", nullptr, std::nullopt, "$18"));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s2"),
                              CExpression::Unary("&", CExpression::Identifier("D_00D49B80")))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s1"),
                              CExpression::Unary("&", CExpression::Identifier("D_00D49B80")))));
  std::vector<std::unique_ptr<CExpression>> args;
  args.push_back(CExpression::Identifier("temp_s2"));
  args.push_back(CExpression::Identifier("temp_s1"));
  body->AddStatement(CStatement::Return(
      CExpression::Call(CExpression::Identifier("UseAssetRange"), std::move(args))));
  tu.functions.emplace_back(CType::S32(), "TestDuplicateCalleeSavedSym", std::vector<CParameter>{},
                            std::move(body));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted, HasSubstr("extern s32 D_00D49B80;\n"));
  EXPECT_THAT(emitted, HasSubstr("temp_s2 = &D_00D49B80;\n"));
  EXPECT_THAT(emitted, HasSubstr("__asm__(\"\" : \"=r\"(temp_s2) : \"0\"(temp_s2));\n"));
  EXPECT_THAT(emitted, HasSubstr("temp_s1 = &D_00D49B80;\n"));
}

TEST(CEmitterTest, EmitsPostAssignmentCseBarrierForPinnedCalleeSavedCopyAcrossCall) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", nullptr, std::nullopt, "$16"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s1", nullptr, std::nullopt, "$17"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s1"),
                              CExpression::Cast(CType::S16(), CExpression::Identifier("arg1")))));
  std::vector<std::unique_ptr<CExpression>> call_args;
  call_args.push_back(CExpression::Identifier("arg0"));
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("SyncNodeState"), std::move(call_args))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S16().MakePointer(), CExpression::Identifier("temp_s0"))),
      CExpression::Identifier("temp_s1"))));
  body->AddStatement(CStatement::Return());
  tu.functions.emplace_back(
      CType::Void(), "TestCalleeSavedCseBarrier",
      std::vector<CParameter>{CParameter{CType::S32(), "arg0"}, CParameter{CType::S32(), "arg1"}},
      std::move(body));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted,
              HasSubstr("temp_s0 = arg0;\n    __asm__(\"\" : \"=r\"(temp_s0) : \"0\"(temp_s0));"));
  EXPECT_THAT(
      emitted,
      HasSubstr("temp_s1 = (s16)arg1;\n    __asm__(\"\" : \"=r\"(temp_s1) : \"0\"(temp_s1));"));
}

TEST(CEmitterTest, EmitsVolatileExternForScalarGlobalPolledInCallFreeSpinLoop) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  auto body = std::make_unique<CompoundStatement>();
  auto loop_body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::While(
      CExpression::Binary("!=", CExpression::Identifier("D_80123450"), CExpression::Integer(0)),
      std::move(loop_body)));
  body->AddStatement(CStatement::Return());
  tu.functions.emplace_back(CType::Void(), "TestSpinLoopGlobal", std::vector<CParameter>{},
                            std::move(body));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted, HasSubstr("extern volatile s32 D_80123450;\n"));
}

TEST(CEmitterTest, EmitsCseBarrierForPinnedCalleeSavedCallReturnReadByZeroOffsetDerefAfterIfCall) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", nullptr, std::nullopt, "$16"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Call(CExpression::Identifier("AcquireSlotRecord"), {}))));
  auto then_body = std::make_unique<CompoundStatement>();
  std::vector<std::unique_ptr<CExpression>> call_args;
  call_args.push_back(CExpression::Identifier("arg0"));
  then_body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("NotifySlotChange"), std::move(call_args))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(then_body)));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("temp_s0"))),
      CExpression::Integer(1))));
  body->AddStatement(CStatement::Return());
  tu.functions.emplace_back(CType::Void(), "TestCallReturnZeroOffsetDerefCseBarrier",
                            std::vector<CParameter>{CParameter{CType::S32(), "arg0"}},
                            std::move(body));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted, HasSubstr("temp_s0 = AcquireSlotRecord();\n    __asm__(\"\" : "
                                 "\"=r\"(temp_s0) : \"0\"(temp_s0));"));
}

TEST(CEmitterTest,
     EmitsBarriersForInPlaceCalleeSavedAddNarrowFloatGlobalLoadAndDuplicateSymbolCallArgs) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s4", nullptr, std::nullopt, "$20"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s2", nullptr, std::nullopt, "$18"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "v1", nullptr, std::nullopt, "$3"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::F32(), "f0", nullptr, std::nullopt, "$f0"));

  // 1. In-place constant increment `temp_s4 = (temp_s4 + 8)` followed across a call by split-prone
  //    copy `temp_s2 = temp_s4`.
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s4"),
      CExpression::Binary("+", CExpression::Identifier("temp_s4"), CExpression::Integer(8)))));
  std::vector<std::unique_ptr<CExpression>> call1_args;
  call1_args.push_back(CExpression::Unary("&", CExpression::Identifier("D_80237440")));
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("Notify"), std::move(call1_args))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s2"), CExpression::Identifier("temp_s4"))));

  // 2. Global load `v1 = D_8016FBEA` followed by `(f32)(s8)v1` and a second read `D_8016FBE8 = v1`.
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"), CExpression::Identifier("D_8016FBEA"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("f0"),
      CExpression::Cast(CType::F32(),
                        CExpression::Cast(CType::S8(), CExpression::Identifier("v1"))))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_8016FBE8"), CExpression::Identifier("v1"))));

  // 3. Second call passing `&D_80237440` triggers caller-saved register statement expression.
  std::vector<std::unique_ptr<CExpression>> call2_args;
  call2_args.push_back(CExpression::Unary("&", CExpression::Identifier("D_80237440")));
  body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("Notify"), std::move(call2_args))));
  body->AddStatement(CStatement::Return());

  tu.functions.emplace_back(CType::Void(), "TestInPlaceAddAndDupSymCallArg",
                            std::vector<CParameter>{}, std::move(body));

  CEmitterOptions options;
  options.format_with_clang = false;
  std::string emitted = CEmitter::EmitTranslationUnit(tu, options);
  EXPECT_THAT(emitted, HasSubstr("temp_s4 = (temp_s4 + 8);\n    __asm__(\"\" : \"=r\"(temp_s4) : "
                                 "\"0\"(temp_s4));"));
  EXPECT_THAT(emitted, HasSubstr("v1 = D_8016FBEA;\n    __asm__(\"\" : \"=r\"(v1) : \"0\"(v1));"));
  EXPECT_THAT(emitted,
              HasSubstr("Notify(({ register s32 _p asm(\"$4\") = (s32)&D_80237440; _p; }));"));
}

}  // namespace
}  // namespace rom_nom_nom
