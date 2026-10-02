#include "lifter/ast_converter.h"

#include <memory>
#include <string>
#include <vector>

#include "core/c_ast.h"
#include "core/mips.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/control_flow_graph.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/dominator_tree.h"
#include "lifter/expression_builder.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

TEST(AstConverterTest, ExpressionConversion) {
  // Integer literal
  auto int_expr = LiftedExpression::Integer(42);
  auto c_int = AstConverter::ConvertExpression(*int_expr);
  EXPECT_EQ(c_int->Kind(), CExpressionKind::kIntegerLiteral);
  EXPECT_EQ(c_int->ToString(), "42");

  // Variable identifier
  auto var_expr = LiftedExpression::Variable("temp_t0");
  auto c_var = AstConverter::ConvertExpression(*var_expr);
  EXPECT_EQ(c_var->Kind(), CExpressionKind::kIdentifier);
  EXPECT_EQ(c_var->ToString(), "temp_t0");

  // Binary operation
  auto bin_expr = LiftedExpression::Binary("+", LiftedExpression::Variable("arg0"),
                                           LiftedExpression::Integer(16));
  auto c_bin = AstConverter::ConvertExpression(*bin_expr);
  EXPECT_EQ(c_bin->Kind(), CExpressionKind::kBinaryExpression);
  EXPECT_EQ(c_bin->ToString(), "(arg0 + 16)");

  // Memory load
  auto load_expr = LiftedExpression::Load("s32", LiftedExpression::Variable("arg0"));
  auto c_load = AstConverter::ConvertExpression(*load_expr);
  EXPECT_EQ(c_load->Kind(), CExpressionKind::kUnaryExpression);
  EXPECT_EQ(c_load->ToString(), "*(s32*)arg0");
}

TEST(AstConverterTest, StatementConversion) {
  // Assignment statement: v0 = arg0 + arg1
  LiftedStatement assign_stmt;
  assign_stmt.kind = StatementKind::kAssignment;
  assign_stmt.destination_variable = "v0";
  assign_stmt.expression = LiftedExpression::Binary("+", LiftedExpression::Variable("arg0"),
                                                    LiftedExpression::Variable("arg1"));

  auto c_assign = AstConverter::ConvertStatement(assign_stmt);
  ASSERT_THAT(c_assign, NotNull());
  EXPECT_EQ(c_assign->Kind(), CStatementKind::kExpressionStatement);
  EXPECT_EQ(c_assign->ToString(0), "v0 = (arg0 + arg1);\n");

  // Return statement: return v0;
  LiftedStatement ret_stmt;
  ret_stmt.kind = StatementKind::kReturn;
  ret_stmt.expression = LiftedExpression::Variable("v0");

  auto c_ret = AstConverter::ConvertStatement(ret_stmt);
  ASSERT_THAT(c_ret, NotNull());
  EXPECT_EQ(c_ret->Kind(), CStatementKind::kReturnStatement);
  EXPECT_EQ(c_ret->ToString(0), "return v0;\n");
}

TEST(AstConverterTest, LinearFunctionConversion) {
  // Simple leaf function:
  // 0x00: addu $v0, $a0, $a1
  // 0x04: jr $ra
  // 0x08: nop
  std::vector<uint32_t> words = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "AddTwo";

  FunctionDeclaration func = AstConverter::Convert(cfg, *root_region, nullptr, options);
  EXPECT_EQ(func.Name(), "AddTwo");
  EXPECT_EQ(func.ReturnType().ToString(), "s32");
  EXPECT_EQ(func.Parameters().size(), 2u);

  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("s32 AddTwo(s32 arg0, s32 arg1) {"));
  EXPECT_THAT(code, HasSubstr("s32 v0;"));
  EXPECT_THAT(code, HasSubstr("v0 = (arg0 + arg1);"));
  EXPECT_THAT(code, HasSubstr("return v0;"));
}

TEST(AstConverterTest, IfElseFunctionConversion) {
  // 0x00: bne $a0, $zero, .L1 (0x10)
  // 0x04: nop
  // 0x08: addiu $v0, $zero, 1
  // 0x0C: jr $ra
  // 0x10: nop (.L1 delay slot / target)
  // 0x14: addiu $v0, $zero, 2
  // 0x18: jr $ra
  // 0x1C: nop
  std::vector<uint32_t> words = {
      0x14800003,  // 0x00: bne $a0, $zero, 0x10 (.L1)
      0x00000000,  // 0x04: nop
      0x24020001,  // 0x08: addiu $v0, $zero, 1
      0x03E00008,  // 0x0C: jr $ra
      0x00000000,  // 0x10: nop
      0x24020002,  // 0x14: addiu $v0, $zero, 2
      0x03E00008,  // 0x18: jr $ra
      0x00000000,  // 0x1C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "CheckPositive";

  FunctionDeclaration func = AstConverter::Convert(cfg, *root_region, nullptr, options);
  EXPECT_EQ(func.Name(), "CheckPositive");
  EXPECT_EQ(func.ReturnType().ToString(), "s32");

  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("CheckPositive(s32 arg0) {"));
  EXPECT_THAT(code, HasSubstr("if ("));
  EXPECT_THAT(code, HasSubstr("return v0;"));
}

TEST(AstConverterTest, LoopFunctionConversion) {
  // Loop counting down:
  // .L_loop (0x00):
  // addiu $a0, $a0, -1
  // bgtz $a0, .L_loop
  // nop
  // jr $ra
  // nop
  std::vector<uint32_t> words = {
      0x2484FFFF,  // 0x00: addiu $a0, $a0, -1
      0x1C80FFFE,  // 0x04: bgtz $a0, 0x00
      0x00000000,  // 0x08: nop
      0x03E00008,  // 0x0C: jr $ra
      0x00000000,  // 0x10: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "CountDown";

  FunctionDeclaration func = AstConverter::Convert(cfg, *root_region, nullptr, options);
  EXPECT_EQ(func.Name(), "CountDown");

  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("CountDown(s32 arg0) {"));
  EXPECT_THAT(code, HasSubstr("while ("));
}

}  // namespace
}  // namespace rom_nom_nom
