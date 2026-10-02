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

TEST(CEmitterTest, FormatCodeWithClangFormatOrFallback) {
  std::string messy_code = "void   MessyFunc (  s32   x ){\nif(x>0){return    x;}\nreturn 0;\n}\n";
  std::string formatted = CEmitter::FormatCode(messy_code);

  EXPECT_THAT(formatted, HasSubstr("MessyFunc"));
  EXPECT_THAT(formatted, HasSubstr("return"));
}

}  // namespace
}  // namespace rom_nom_nom
