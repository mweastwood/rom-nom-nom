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
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
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

  // Store statement with typed pointer cast: *(s16*)(arg0 + 4) = arg1
  LiftedStatement store_stmt;
  store_stmt.kind = StatementKind::kStore;
  store_stmt.store_type = "s16";
  store_stmt.destination_address = LiftedExpression::Binary("+", LiftedExpression::Variable("arg0"),
                                                            LiftedExpression::Integer(4));
  store_stmt.expression = LiftedExpression::Variable("arg1");

  auto c_store = AstConverter::ConvertStatement(store_stmt);
  ASSERT_THAT(c_store, NotNull());
  EXPECT_EQ(c_store->Kind(), CStatementKind::kExpressionStatement);
  EXPECT_THAT(c_store->ToString(0), HasSubstr("*(s16*)(arg0 + 4) = arg1;\n"));
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

TEST(AstConverterTest, LocalVariableDeclarationFiltering) {
  // Verify that local variables are declared at the function header while
  // parameters, globals (g_*), and registered symbols are excluded.
  std::string textproto = R"pb(
    entries { name: "g_audio_status" address: 0x801F2340 type: SYMBOL_DATA size: 64 }
    entries { name: "AudioUpdate" address: 0x80001000 type: SYMBOL_FUNC size: 128 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // Function:
  // 0x00: lui   $at, 0x801F
  // 0x04: lw    $v0, 0x2340($at)  -> v0 = g_audio_status
  // 0x08: jal   0x80001000        -> AudioUpdate(v0)
  // 0x0C: nop
  // 0x10: jr    $ra
  // 0x14: nop
  std::vector<uint32_t> words = {
      0x3C01801F,  // 0x00: lui $at, 0x801F
      0x8C222340,  // 0x04: lw  $v0, 0x2340($at)
      0x0C000400,  // 0x08: jal 0x80001000
      0x00000000,  // 0x0C: nop
      0x03E00008,  // 0x10: jr  $ra
      0x00000000,  // 0x14: nop
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
  options.function_name = "ProcessAudio";

  FunctionDeclaration func = AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();

  // Local variable v0 must be declared at the top of the function
  EXPECT_THAT(code, HasSubstr("s32 v0;"));
  // Parameter arg0, global g_audio_status, and symbol AudioUpdate must NOT be declared
  EXPECT_THAT(code, Not(HasSubstr("s32 arg0;")));
  EXPECT_THAT(code, Not(HasSubstr("s32 g_audio_status;")));
  EXPECT_THAT(code, Not(HasSubstr("s32 AudioUpdate;")));
}

TEST(AstConverterTest, GotoBlockLabelConsistency) {
  // block 0: vram 0x80000000: beq $a0, $zero, 0x80000014
  // delay slot: nop
  // block 1: vram 0x80000008: addiu $v0, $zero, 1; jr $ra; nop
  // block 2: vram 0x80000014: addiu $v0, $zero, 2; jr $ra; nop
  std::vector<uint32_t> words = {
      0x10800004,  // 00: beq   $a0, $zero, +4 -> 0x80000014
      0x00000000,  // 04: nop
      0x24020001,  // 08: addiu $v0, $zero, 1
      0x03E00008,  // 0C: jr    $ra
      0x00000000,  // 10: nop
      0x24020002,  // 14: addiu $v0, $zero, 2
      0x03E00008,  // 18: jr    $ra
      0x00000000,  // 1C: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  // Construct a region with gotos to test consistency
  auto root = std::make_unique<StructuredRegion>();
  root->type = RegionType::kSequence;

  auto block0 = std::make_unique<StructuredRegion>();
  block0->type = RegionType::kBlock;
  block0->block_id = 0;
  root->children.push_back(std::move(block0));

  auto goto2 = std::make_unique<StructuredRegion>();
  goto2->type = RegionType::kGoto;
  goto2->block_id = 2;
  root->children.push_back(std::move(goto2));

  auto block2 = std::make_unique<StructuredRegion>();
  block2->type = RegionType::kBlock;
  block2->block_id = 2;
  root->children.push_back(std::move(block2));

  // Goto targeting non-emitted block 99
  auto goto99 = std::make_unique<StructuredRegion>();
  goto99->type = RegionType::kGoto;
  goto99->block_id = 99;
  root->children.push_back(std::move(goto99));

  AstConverterOptions options;
  options.function_name = "TestGotoConsistency";

  FunctionDeclaration func = AstConverter::Convert(cfg, *root, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("goto block_2;"));
  EXPECT_THAT(code, HasSubstr("block_2:"));
  EXPECT_THAT(code, HasSubstr("goto block_99;"));
  EXPECT_THAT(code, HasSubstr("block_99:"));
}

TEST(AstConverterTest, ConvertSwitchStatement) {
  std::vector<Instruction> instructions;
  Instruction nop;
  nop.opcode = Opcode::kSll;
  nop.rd = Register::kZero;
  nop.rt = Register::kZero;
  nop.vram = 0x80000000;
  instructions.push_back(nop);

  auto cfg_or = ControlFlowGraph::Build(instructions);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  JumpTable jt;
  jt.index_register = Register::kV1;

  auto switch_reg = std::make_unique<StructuredRegion>();
  switch_reg->type = RegionType::kSwitch;
  switch_reg->jump_table = &jt;

  StructuredCase case0;
  case0.case_values = {0, 1};
  switch_reg->cases.push_back(std::move(case0));

  StructuredCase default_case;
  default_case.is_default = true;
  switch_reg->cases.push_back(std::move(default_case));

  AstConverterOptions options;
  options.function_name = "TestSwitch";

  FunctionDeclaration func = AstConverter::Convert(cfg, *switch_reg, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("switch (v1) {"));
  EXPECT_THAT(code, HasSubstr("case 0:"));
  EXPECT_THAT(code, HasSubstr("case 1:"));
  EXPECT_THAT(code, HasSubstr("default:"));
  EXPECT_THAT(code, HasSubstr("break;"));
}

TEST(AstConverterTest, ConvertSwitchStatementWithReturnDoesNotAddRedundantBreak) {
  // Construct a block with a return statement (jr $ra)
  std::vector<uint32_t> words = {
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  JumpTable jt;
  jt.index_register = Register::kA0;

  auto switch_reg = std::make_unique<StructuredRegion>();
  switch_reg->type = RegionType::kSwitch;
  switch_reg->jump_table = &jt;

  StructuredCase case0;
  case0.case_values = {0};
  auto case_body = std::make_unique<StructuredRegion>();
  case_body->type = RegionType::kBlock;
  case_body->block_id = 0;
  case0.body = std::move(case_body);
  switch_reg->cases.push_back(std::move(case0));

  AstConverterOptions options;
  options.function_name = "TestSwitchReturn";

  FunctionDeclaration func = AstConverter::Convert(cfg, *switch_reg, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("switch (arg0) {"));
  EXPECT_THAT(code, HasSubstr("case 0:"));
  EXPECT_THAT(code, HasSubstr("return;"));
  // Must NOT contain "break;" right after "return;"
  EXPECT_THAT(code, Not(HasSubstr("return;\n  break;")));
}

TEST(AstConverterTest, ConvertSwitchStatementFallbackCondition) {
  std::vector<Instruction> instructions;
  Instruction nop;
  nop.opcode = Opcode::kSll;
  nop.rd = Register::kZero;
  nop.rt = Register::kZero;
  nop.vram = 0x80000000;
  instructions.push_back(nop);

  auto cfg_or = ControlFlowGraph::Build(instructions);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  auto switch_reg = std::make_unique<StructuredRegion>();
  switch_reg->type = RegionType::kSwitch;
  switch_reg->jump_table = nullptr;  // No jump table pointer

  StructuredCase default_case;
  default_case.is_default = true;
  switch_reg->cases.push_back(std::move(default_case));

  AstConverterOptions options;
  options.function_name = "TestSwitchFallback";

  FunctionDeclaration func = AstConverter::Convert(cfg, *switch_reg, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("switch (cond) {"));
  EXPECT_THAT(code, HasSubstr("default:"));
}

TEST(AstConverterTest, ConvertSwitchStatementDeclaresLocalVariables) {
  // Construct block 0 with: addiu $v0, $zero, 42; jr $ra; nop
  std::vector<uint32_t> words = {
      0x2402002A,  // addiu $v0, $zero, 42
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  JumpTable jt;
  jt.index_register = Register::kA0;

  auto switch_reg = std::make_unique<StructuredRegion>();
  switch_reg->type = RegionType::kSwitch;
  switch_reg->jump_table = &jt;

  StructuredCase case0;
  case0.case_values = {0};
  auto case_body = std::make_unique<StructuredRegion>();
  case_body->type = RegionType::kBlock;
  case_body->block_id = 0;
  case0.body = std::move(case_body);
  switch_reg->cases.push_back(std::move(case0));

  AstConverterOptions options;
  options.function_name = "TestSwitchVars";

  FunctionDeclaration func = AstConverter::Convert(cfg, *switch_reg, nullptr, options);
  std::string code = func.ToString();

  // v0 must be declared at the top of the function
  EXPECT_THAT(code, HasSubstr("s32 v0;"));
  // Return type should be inferred as s32 because HasReturnValue detected return v0
  EXPECT_EQ(func.ReturnType().ToString(), "s32");
}

}  // namespace
}  // namespace rom_nom_nom
