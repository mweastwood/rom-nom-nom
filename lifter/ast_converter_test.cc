#include "lifter/ast_converter.h"

#include <memory>
#include <string>
#include <vector>

#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/dominator_tree.h"
#include "lifter/expression_builder.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;
using ::testing::UnorderedElementsAre;

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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  EXPECT_EQ(func.Name(), "AddTwo");
  EXPECT_EQ(func.ReturnType().ToString(), "s32");
  EXPECT_EQ(func.Parameters().size(), 2u);

  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("s32 AddTwo(s32 arg0, s32 arg1) {"));
  EXPECT_THAT(code, HasSubstr("return (arg0 + arg1);"));
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  EXPECT_EQ(func.Name(), "CheckPositive");
  EXPECT_EQ(func.ReturnType().ToString(), "s32");

  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("CheckPositive(s32 arg0) {"));
  EXPECT_THAT(code, HasSubstr("if ("));
  EXPECT_THAT(code, HasSubstr("return 2;"));
  EXPECT_THAT(code, HasSubstr("return 1;"));
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
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
      0x00000000,  // 18: nop
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root, nullptr, options);
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *switch_reg, nullptr, options);
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *switch_reg, nullptr, options);
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *switch_reg, nullptr, options);
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *switch_reg, nullptr, options);
  std::string code = func.ToString();

  // v0 must be declared at the top of the function
  EXPECT_THAT(code, HasSubstr("s32 v0;"));
  // Return type should be inferred as s32 because HasReturnValue detected return v0
  EXPECT_EQ(func.ReturnType().ToString(), "s32");
}

TEST(AstConverterTest, ConvertMultiBlockFunctionWithInterBlockReturnLiveness) {
  // Multi-block function where $v0 is computed across conditional branches,
  // and the exit block contains only jr $ra; nop.
  // The lifter must infer return type s32, declare s32 v0, and emit return v0.
  std::vector<uint32_t> words = {
      0x00041400,  // 00: sll   $v0, $a0, 16
      0x00021403,  // 04: sra   $v0, $v0, 16
      0x04410004,  // 08: bgez  $v0, +4 -> 0x1C (.L_exit)
      0x00000000,  // 0C: nop
      0x00041023,  // 10: subu  $v0, $zero, $a0
      0x00021400,  // 14: sll   $v0, $v0, 16
      0x00021403,  // 18: sra   $v0, $v0, 16
      0x03E00008,  // 1C: jr    $ra
      0x00000000,  // 20: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "Abs16";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  EXPECT_EQ(func.Name(), "Abs16");
  EXPECT_EQ(func.ReturnType().ToString(), "s32");

  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("s32 Abs16(s32 arg0) {"));
  EXPECT_THAT(code, HasSubstr("s32 v0;"));
  EXPECT_THAT(code, HasSubstr("return v0;"));
}

TEST(AstConverterTest, BranchConditionFoldingSltAndDeadAtElimination) {
  // slt   $at, $a0, $a1
  // beq   $at, $zero, +2 (0x10)
  // nop
  // addiu $v0, $zero, 1
  // jr    $ra
  // nop
  // addiu $v0, $zero, 2
  // jr    $ra
  // nop
  std::vector<uint32_t> words = {
      0x0085082A,  // 00: slt   $at, $a0, $a1
      0x10200002,  // 04: beq   $at, $zero, +2 -> 0x10
      0x00000000,  // 08: nop
      0x24020001,  // 0C: addiu $v0, $zero, 1
      0x03E00008,  // 10: jr    $ra
      0x00000000,  // 14: nop
      0x24020002,  // 18: addiu $v0, $zero, 2
      0x03E00008,  // 1C: jr    $ra
      0x00000000,  // 20: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "MinBranch";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // The condition should be folded to (arg0 < arg1)
  EXPECT_THAT(code, HasSubstr("(arg0 < arg1)"));
  // temp_at must not be assigned or declared
  EXPECT_THAT(code, Not(HasSubstr("temp_at = ")));
  EXPECT_THAT(code, Not(HasSubstr("s32 temp_at;")));
}

TEST(AstConverterTest, BranchConditionFoldingSltiu) {
  // sltiu $v0, $a0, 10
  // bne   $v0, $zero, +2 (0x10)
  // nop
  // addiu $v0, $zero, 1
  // jr    $ra
  // nop
  // addiu $v0, $zero, 2
  // jr    $ra
  // nop
  std::vector<uint32_t> words = {
      0x2C82000A,  // 00: sltiu $v0, $a0, 10
      0x14400002,  // 04: bne   $v0, $zero, +2 -> 0x10
      0x00000000,  // 08: nop
      0x24020001,  // 0C: addiu $v0, $zero, 1
      0x03E00008,  // 10: jr    $ra
      0x00000000,  // 14: nop
      0x24020002,  // 18: addiu $v0, $zero, 2
      0x03E00008,  // 1C: jr    $ra
      0x00000000,  // 20: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "CheckBound";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // The condition should be folded to ((u32)arg0 >= 10U) due to fallthrough structuring
  EXPECT_THAT(code, HasSubstr("(u32)arg0 >= 10U"));
}

TEST(AstConverterTest, BranchLikelyDelaySlotNullification) {
  // Pattern matching AudioChannelSetPan:
  // 00: slti  $v0, $a1, 257
  // 04: beql  $v0, $zero, 0x0C
  // 08: addiu $a1, $zero, 256
  // 0C: addu  $v0, $a1, $zero
  // 10: jr    $ra
  // 14: nop
  std::vector<uint32_t> words = {
      0x28A20101,  // 00: slti  $v0, $a1, 257
      0x50400001,  // 04: beql  $v0, $zero, +1 -> 0x0C
      0x24050100,  // 08: addiu $a1, $zero, 256  (delay slot)
      0x00A01021,  // 0C: addu  $v0, $a1, $zero
      0x03E00008,  // 10: jr    $ra
      0x00000000,  // 14: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "ClampPan";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // Condition should be folded to (arg1 >= 257)
  EXPECT_THAT(code, HasSubstr("(arg1 >= 257)"));
  // Inside the if body, arg1 should be set to 256
  EXPECT_THAT(code, HasSubstr("arg1 = 256;"));
}

TEST(AstConverterTest, MultiBlockDoWhileLoopConversion) {
  // Multi-block do-while loop:
  // 0x00: j 0x80020008
  // 0x04: nop
  // Header:
  // 0x08: addiu $v0, $v0, 1
  // 0x0C: j 0x80020014
  // 0x10: nop
  // Latch:
  // 0x14: addiu $a0, $a0, -1
  // 0x18: bne $a0, $zero, -4 (0x80020008)
  // 0x1C: nop
  // Exit:
  // 0x20: jr $ra
  // 0x24: nop
  std::vector<uint32_t> words = {
      0x08008002,  // 0x00: j 0x80020008
      0x00000000,  // 0x04: nop
      0x24420001,  // 0x08: addiu $v0, $v0, 1
      0x08008005,  // 0x0C: j 0x80020014
      0x00000000,  // 0x10: nop
      0x2484FFFF,  // 0x14: addiu $a0, $a0, -1
      0x1480FFFB,  // 0x18: bne $a0, $zero, -5 (to 0x08)
      0x00000000,  // 0x1C: nop
      0x03E00008,  // 0x20: jr $ra
      0x00000000,  // 0x24: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
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

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("do {"));
  EXPECT_THAT(code, HasSubstr("v0 = (v0 + 1);"));
  EXPECT_THAT(code, HasSubstr("arg0 = (arg0 - 1);"));
  EXPECT_THAT(code, HasSubstr("while ((arg0 != 0));"));
  EXPECT_THAT(code, Not(HasSubstr("continue;")));
}

TEST(AstConverterTest, DeclaresStackBufferWithDetectedFrameSize) {
  // Function prologue allocates 128 bytes on stack, indexes into stack buffer:
  // 0: addiu $sp, $sp, -128
  // 4: addu  $v0, $v1, $sp
  // 8: jr    $ra
  // C: addiu $sp, $sp, 128
  std::vector<uint32_t> words = {
      0x27BDFF80,  // addiu $sp, $sp, -128
      0x007D1021,  // addu  $v0, $v1, $sp
      0x03E00008,  // jr    $ra
      0x27BD0080,  // addiu $sp, $sp, 128
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "StackFunc";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("u8 sp[128];"));
  EXPECT_THAT(code, HasSubstr("return (v1 + (s32)sp);"));
  EXPECT_THAT(code, Not(HasSubstr("s32 sp;")));
}

TEST(AstConverterTest, FailsWhenStackBufferAccessedWithoutPrologueAdjustment) {
  // Function indexes into stack buffer without an explicit addiu $sp, $sp, -frame_size.
  // Must fail instead of guessing or defaulting to 64 bytes.
  std::vector<uint32_t> words = {
      0x007D1021,  // addu  $v0, $v1, $sp
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "StackFuncDefault";

  auto func_or = AstConverter::Convert(cfg, *root_region, nullptr, options);
  EXPECT_FALSE(func_or.ok());
}

TEST(AstConverterTest, RobustPrologueAnalysisWithSdAndMultiBlock) {
  // Function allocates frame in block 0, falls through to block 1,
  // saves callee-saved registers using sd (64-bit).
  // 0: daddiu $sp, $sp, -32
  // 4: b      8
  // 8: sd     $ra, 24($sp)
  // C: sd     $s0, 16($sp)
  // 10: jr    $ra
  // 14: daddiu $sp, $sp, 32
  std::vector<uint32_t> words = {
      0x67BDFFE0,  // 0: daddiu $sp, $sp, -32
      0x10000001,  // 4: b      12 (pc+4+4 = 0xC)
      0x00000000,  // 8: nop
      0xFFBF0018,  // C: sd     $ra, 24($sp)
      0xFFB00010,  // 10: sd    $s0, 16($sp)
      0x03E00008,  // 14: jr    $ra
      0x67BD0020,  // 18: daddiu $sp, $sp, 32
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "MultiBlockPrologue";

  auto func_or = AstConverter::Convert(cfg, *root_region, nullptr, options);
  ASSERT_TRUE(func_or.ok());
  std::string code = func_or->ToString();

  // $ra and $s0 saves should be identified as prologue and omitted from the C body
  EXPECT_THAT(code, Not(HasSubstr("*(s64*)")));
}

TEST(AstConverterTest, DoWhileLoopDelaySlotDecoupling) {
  // Pattern matching WaitVsyncFrames inner busy-wait loop:
  // 00: lui   $v0, 0x8020
  // 04: lbu   $v0, 21000($v0)
  // 08: beqz  $v0, 0x00 (.L_loop)
  // 0C: addu  $v0, $v1, $zero  (delay slot defines $v0, but $v0 is killed at 0x00)
  // 10: jr    $ra
  // 14: nop
  std::vector<uint32_t> words = {
      0x3C028020,  // 00: lui   $v0, 0x8020
      0x90425208,  // 04: lbu   $v0, 21000($v0)
      0x1040FFFD,  // 08: beqz  $v0, -3 words -> 0x00
      0x00601021,  // 0C: addu  $v0, $v1, $zero  (delay slot)
      0x03E00008,  // 10: jr    $ra
      0x00000000,  // 14: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "WaitVsyncBusyWait";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // The loop body should contain the load, but NOT the delay slot assignment v0 = v1.
  EXPECT_THAT(code, HasSubstr("do {"));
  EXPECT_THAT(code, HasSubstr("while ((v0 == 0));"));
  // return v1 must be emitted AFTER the do-while loop!
  EXPECT_THAT(code, HasSubstr("} while ((v0 == 0));\n    return v1;\n"));
}

TEST(AstConverterTest, BranchLikelyInvertedIfElseDelaySlotPreserved) {
  // Pattern matching AudioVoiceSetVolume:
  // 00: andi  $v0, $a0, 1
  // 04: beql  $v0, $zero, 0x10 (.L_join)  (skips to join on zero, incrementing $a2 in delay slot)
  // 08: addiu $a2, $a2, 1                (delay slot)
  // 0C: sw    $a1, 0($a0)                (then body)
  // 10: addu  $v0, $a2, $zero            (.L_join)
  // 14: jr    $ra
  // 18: nop
  std::vector<uint32_t> words = {
      0x30820001,  // 00: andi  $v0, $a0, 1
      0x50400002,  // 04: beql  $v0, $zero, +2 -> 0x10
      0x24C60001,  // 08: addiu $a2, $a2, 1  (delay slot)
      0xAC850000,  // 0C: sw    $a1, 0($a0)
      0x00C01021,  // 10: addu  $v0, $a2, $zero
      0x03E00008,  // 14: jr    $ra
      0x00000000,  // 18: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "BranchLikelyInverted";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // Condition is inverted to ((arg0 & 1) != 0)
  EXPECT_THAT(code, HasSubstr("if (((arg0 & 1) != 0)) {"));
  // Inside the else block, arg2 should be incremented (the taken branch-likely delay slot)
  EXPECT_THAT(code, HasSubstr("} else {\n        arg2 = (arg2 + 1);"));
}

TEST(AstConverterTest, DeclaresFpRegistersWithProperFloatTypesAndFoldsCop1Branch) {
  std::vector<uint32_t> words = {
      0x46041000,  // 00: add.s $f0, $f2, $f4
      0x4602003C,  // 04: c.lt.s $f0, $f2
      0x45010004,  // 08: bc1t  +4 (to 1C)
      0x00000000,  // 0C: nop
      0x46307300,  // 10: add.d $f12, $f14, $f16
      0x03E00008,  // 14: jr    $ra
      0x00000000,  // 18: nop
      0x03E00008,  // 1C: jr    $ra
      0x00000000,  // 20: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "FloatTest";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // Single precision variables are f32
  EXPECT_THAT(code, HasSubstr("f32 f0;"));
  EXPECT_THAT(code, HasSubstr("f32 f2;"));
  EXPECT_THAT(code, HasSubstr("f32 f4;"));
  // Double precision variables are f64
  EXPECT_THAT(code, HasSubstr("f64 f12_d;"));
  EXPECT_THAT(code, HasSubstr("f64 f14_d;"));
  EXPECT_THAT(code, HasSubstr("f64 f16_d;"));
  // Branch comparison is folded directly
  EXPECT_THAT(code, HasSubstr("f0 < f2"));
}

TEST(AstConverterTest, SltiuAssignmentProducesUnsignedCast) {
  // 00: sltiu $v0, $a0, 1
  // 04: jr    $ra
  // 08: nop
  std::vector<uint32_t> words = {
      0x2C820001,  // sltiu $v0, $a0, 1
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestSltiuAssign";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("(u32)arg0 < 1"));
}

TEST(AstConverterTest, DoesNotDeclareSynthesizedDataGlobalsAsLocalVariables) {
  std::vector<uint32_t> words = {
      0x3C018017,  // 0: lui  $at, 0x8017
      0xC4220450,  // 4: lwc1 $f2, 1104($at)
      0x03E00008,  // 8: jr   $ra
      0x00000000,  // C: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestGlobalDataNotLocal";

  auto index_or = SymbolIndex::ParseFromTextproto("");
  ASSERT_TRUE(index_or.ok());

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();

  EXPECT_THAT(code, Not(HasSubstr("s32 D_80170450;")));
  EXPECT_THAT(code, HasSubstr("&D_80170450"));
}

TEST(AstConverterTest, CalleeSavedInCallDelaySlotDetected) {
  // Pattern where $s1 is saved in the delay slot of jal:
  // 00: addiu $sp, $sp, -48
  // 04: sw    $ra, 40($sp)
  // 08: jal   func_80030000
  // 0C: sw    $s1, 36($sp)      (delay slot saves callee-saved $s1)
  // 10: lw    $ra, 40($sp)
  // 14: lw    $s1, 36($sp)
  // 18: jr    $ra
  // 1C: addiu $sp, $sp, 48
  std::vector<uint32_t> words = {
      0x27BDFFD0,  // 00: addiu $sp, $sp, -48
      0xAFBF0028,  // 04: sw    $ra, 40($sp)
      0x0C00C000,  // 08: jal   0x80030000
      0xAFB10024,  // 0C: sw    $s1, 36($sp)
      0x8FBF0028,  // 10: lw    $ra, 40($sp)
      0x8FB10024,  // 14: lw    $s1, 36($sp)
      0x03E00008,  // 18: jr    $ra
      0x27BD0030,  // 1C: addiu $sp, $sp, 48
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "DelaySlotSave";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // $s1 should be recognized as a callee-saved register save/restore,
  // not emitted as a local stack variable or assignment in the body.
  EXPECT_THAT(code, Not(HasSubstr("var_sp_36")));
  EXPECT_THAT(code, Not(HasSubstr("s1 = ")));
}

TEST(AstConverterTest, FoldsNewFpuComparisonOpcodesInBranchConditions) {
  // 1. Test ExtractBranchCondition directly on blocks with c.ueq.s, c.ult.s, c.ule.d, c.ngle.s,
  // c.ngl.d, c.un.s followed by bc1t / bc1f.
  auto make_fpu_branch_block = [](uint32_t cmp_word, uint32_t branch_word) {
    std::vector<uint32_t> words = {cmp_word, branch_word, 0x00000000};
    BasicBlock block;
    block.id = 0;
    block.instructions = *DecodeSequence(words, 0x80020000);
    return block;
  };

  // c.ueq.s $f2, $f4 (0x46041033) + bc1t (0x45010002)
  {
    auto block = make_fpu_branch_block(0x46041033, 0x45010002);
    EXPECT_EQ(AstConverter::ExtractBranchCondition(block, /*invert_condition=*/false)->ToString(),
              "(f2 == f4)");
    EXPECT_EQ(AstConverter::ExtractBranchCondition(block, /*invert_condition=*/true)->ToString(),
              "(f2 != f4)");
  }

  // c.ult.s $f2, $f4 (0x46041035) + bc1f (0x45000002)
  {
    auto block = make_fpu_branch_block(0x46041035, 0x45000002);
    EXPECT_EQ(AstConverter::ExtractBranchCondition(block, /*invert_condition=*/false)->ToString(),
              "!(f2 < f4)");
    EXPECT_EQ(AstConverter::ExtractBranchCondition(block, /*invert_condition=*/true)->ToString(),
              "(f2 < f4)");
  }

  // c.ule.d $f12, $f14 (0x462E6037) + bc1t (0x45010002)
  {
    auto block = make_fpu_branch_block(0x462E6037, 0x45010002);
    EXPECT_EQ(AstConverter::ExtractBranchCondition(block, /*invert_condition=*/false)->ToString(),
              "(f12_d <= f14_d)");
    EXPECT_EQ(AstConverter::ExtractBranchCondition(block, /*invert_condition=*/true)->ToString(),
              "!(f12_d <= f14_d)");
  }

  // c.ngle.s $f2, $f4 (0x46041039) + bc1t (0x45010002)
  {
    auto block = make_fpu_branch_block(0x46041039, 0x45010002);
    EXPECT_EQ(AstConverter::ExtractBranchCondition(block, /*invert_condition=*/false)->ToString(),
              "(f2 <= f4)");
  }

  // c.ngl.d $f12, $f14 (0x462E603B) + bc1f (0x45000002)
  {
    auto block = make_fpu_branch_block(0x462E603B, 0x45000002);
    EXPECT_EQ(AstConverter::ExtractBranchCondition(block, /*invert_condition=*/false)->ToString(),
              "(f12_d != f14_d)");
  }

  // 2. Test full AstConverter::Convert with c.ule.d + bc1f and c.ueq.s + bc1t
  std::vector<uint32_t> words = {
      0x462E6037,  // 00: c.ule.d $f12, $f14
      0x45000003,  // 04: bc1f    +3 -> 0x14
      0x00000000,  // 08: nop
      0x24020001,  // 0C: addiu   $v0, $zero, 1
      0x03E00008,  // 10: jr      $ra
      0x00000000,  // 14: nop
      0x46041033,  // 18: c.ueq.s $f2, $f4
      0x45010003,  // 1C: bc1t    +3 -> 0x2C
      0x00000000,  // 20: nop
      0x24020002,  // 24: addiu   $v0, $zero, 2
      0x03E00008,  // 28: jr      $ra
      0x00000000,  // 2C: nop
      0x24020003,  // 30: addiu   $v0, $zero, 3
      0x03E00008,  // 34: jr      $ra
      0x00000000,  // 38: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "FpuBranchFold";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("(f12_d <= f14_d)"));
  EXPECT_THAT(code, HasSubstr("(f2 == f4)"));
  EXPECT_THAT(code, Not(HasSubstr("fcond")));
}

TEST(AstConverterTest, ComputeBlockEntryStatesStepsDelaySlotBeforeCallInstruction) {
  // Block 0:
  //   00: addiu $v0, $zero, 42
  //   04: jal   0x80030000
  //   08: addu  $s0, $v0, $zero   (delay slot copies $v0 (=42) into callee-saved $s0 BEFORE jal
  //                                clobbers $v0; then jal preserves $s0)
  //   0C: bnez  $a0, 0x18         (forces Block 1 at 0x18 to be a separate basic block)
  //   10: nop
  // Block 1 (0x14):
  //   14: nop
  // Block 2 (0x18):
  //   18: addu  $v0, $s0, $zero   (uses $s0 in successor block; should fold to 42!)
  //   1C: jr    $ra
  //   20: nop
  std::vector<uint32_t> words = {
      0x2402002A,  // 00: addiu $v0, $zero, 42
      0x0C00C000,  // 04: jal   0x80030000
      0x00408021,  // 08: addu  $s0, $v0, $zero  (delay slot of jal)
      0x14800002,  // 0C: bnez  $a0, +2 -> 0x18
      0x00000000,  // 10: nop
      0x00000000,  // 14: nop
      0x02001021,  // 18: addu  $v0, $s0, $zero
      0x03E00008,  // 1C: jr    $ra
      0x00000000,  // 20: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "DelaySlotCopyAcrossCall";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // Because the delay slot stepped before the jal in ComputeBlockEntryStates,
  // $s0 holds constant 42 upon entering Block 2, so `addu $v0, $s0, $zero` lifts to `v0 = 42;`.
  EXPECT_THAT(code, HasSubstr("temp_s0 = 42;"));
  EXPECT_THAT(code, Not(HasSubstr("v0 = temp_s0;")));
}

TEST(AstConverterTest, ComputeBlockEntryStatesInvalidatesCallerSavedArgumentsAcrossCallsIntoLoop) {
  // Entry block sets $a0=1, $a1=2 and calls jal 0x80030000, then enters an infinite loop
  // that loads a function pointer into $s0 and executes `jalr $s0; nop` without setting $a0/$a1.
  // The indirect call inside the loop must NOT inherit stale $a0/$a1 from the pre-loop call.
  std::vector<uint32_t> words = {
      0x24040001,  // 00: addiu $a0, $zero, 1
      0x24050002,  // 04: addiu $a1, $zero, 2
      0x0C00C000,  // 08: jal   0x80030000
      0x00000000,  // 0C: nop
      // .L_loop (0x80020010):
      0x3C108020,  // 10: lui   $s0, 0x8020
      0x8E100000,  // 14: lw    $s0, 0x0000($s0)
      0x0200F809,  // 18: jalr  $ra, $s0
      0x00000000,  // 1C: nop
      0x08008004,  // 20: j     0x80020010 (.L_loop)
      0x00000000,  // 24: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "IdleLoopIndirectCall";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("((void (*)())temp_s0)();"));
  EXPECT_THAT(code, Not(HasSubstr("((void (*)())temp_s0)(1")));
}

TEST(AstConverterTest, CollectUsedVariablesProperlyCollectsVariablesInForStatement) {
  // Construct for (i = 0; (i < limit); i++) { total = (total + item); }
  auto for_init = CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("i"), CExpression::Integer(0)));
  auto for_cond =
      CExpression::Binary("<", CExpression::Identifier("i"), CExpression::Identifier("limit"));
  auto for_step = CExpression::Unary("++", CExpression::Identifier("i"), /*is_prefix=*/false);
  auto for_body = std::make_unique<CompoundStatement>();
  for_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("total"),
                              CExpression::Binary("+", CExpression::Identifier("total"),
                                                  CExpression::Identifier("item")))));

  auto for_stmt = CStatement::For(std::move(for_init), std::move(for_cond), std::move(for_step),
                                  std::move(for_body));

  std::vector<std::string> vars;
  absl::flat_hash_set<std::string> seen;
  AstConverter::CollectUsedVariables(*for_stmt, &vars, &seen);

  EXPECT_THAT(vars, UnorderedElementsAre("i", "limit", "total", "item"));
}

TEST(AstConverterTest, SimplifyReturnStatementsTerminalAssignmentFoldsIntoDirectReturn) {
  // v0 = (a + b); return v0; -> return (a + b);
  CompoundStatement comp;
  comp.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("a"), CExpression::Identifier("b")))));
  comp.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  AstConverter::SimplifyReturnStatements(&comp, /*returns_v0=*/true, /*returns_f0=*/false);

  ASSERT_EQ(comp.Statements().size(), 1u);
  EXPECT_EQ(comp.Statements()[0]->Kind(), CStatementKind::kReturnStatement);
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("return (a + b);"));
}

TEST(AstConverterTest, SimplifyReturnStatementsEarlyReturnRestructuring) {
  // v0 = default_val;
  // if ((v0 == 0)) { v0 = other_val; }
  // return v0;
  // ->
  // if ((default_val == 0)) { return other_val; }
  // return default_val;
  CompoundStatement comp;
  comp.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"), CExpression::Identifier("default_val"))));
  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"), CExpression::Identifier("other_val"))));
  comp.AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("v0"), CExpression::Integer(0)),
      std::move(then_branch)));
  comp.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  AstConverter::SimplifyReturnStatements(&comp, /*returns_v0=*/true, /*returns_f0=*/false);

  ASSERT_EQ(comp.Statements().size(), 2u);
  EXPECT_EQ(comp.Statements()[0]->Kind(), CStatementKind::kIfStatement);
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("if ((default_val == 0))"));
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("return other_val;"));
  EXPECT_EQ(comp.Statements()[1]->Kind(), CStatementKind::kReturnStatement);
  EXPECT_THAT(comp.Statements()[1]->ToString(), HasSubstr("return default_val;"));
}

TEST(AstConverterTest, SimplifyReturnStatementsBranchLikelyReturnInversion) {
  // MaxTwo pattern:
  // v0 = arg0;
  // if ((arg1 >= v0)) { v0 = arg1; }
  // return v0;
  // ->
  // if ((arg0 > arg1)) { return arg0; }
  // return arg1;
  CompoundStatement comp;
  comp.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"), CExpression::Identifier("arg0"))));
  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"), CExpression::Identifier("arg1"))));
  comp.AddStatement(CStatement::If(
      CExpression::Binary(">=", CExpression::Identifier("arg1"), CExpression::Identifier("v0")),
      std::move(then_branch)));
  comp.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  AstConverter::SimplifyReturnStatements(&comp, /*returns_v0=*/true, /*returns_f0=*/false);

  ASSERT_EQ(comp.Statements().size(), 2u);
  EXPECT_EQ(comp.Statements()[0]->Kind(), CStatementKind::kIfStatement);
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("if ((arg0 > arg1))"));
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("return arg0;"));
  EXPECT_EQ(comp.Statements()[1]->Kind(), CStatementKind::kReturnStatement);
  EXPECT_THAT(comp.Statements()[1]->ToString(), HasSubstr("return arg1;"));
}

TEST(AstConverterTest, LoopRotationPreScanPeeledLoopLatchDelay) {
  // Entry block (0x80020000) peels loop decrement before entering loop:
  // 00: addiu $a0, $a0, -1
  // Loop header/latch (0x80020004):
  // 04: addu  $v0, $v0, $a0
  // 08: bnel  $a0, $zero, -1 word -> 0x04
  // 0C: addiu $a0, $a0, -1  (delay slot matches predecessor peeled instruction)
  // Exit (0x80020010):
  // 10: jr    $ra
  // 14: nop
  std::vector<uint32_t> words = {
      0x2484FFFF,  // 00: addiu $a0, $a0, -1
      0x00441021,  // 04: addu  $v0, $v0, $a0
      0x5480FFFF,  // 08: bnel  $a0, $zero, -1 word -> 0x80020004
      0x2484FFFF,  // 0C: addiu $a0, $a0, -1
      0x03E00008,  // 10: jr    $ra
      0x00000000,  // 14: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "PeeledLoopRotation";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // The peeled instruction before the loop should be omitted, and emitted inside the loop body!
  EXPECT_THAT(code, Not(HasSubstr("arg0 = (arg0 - 1);\n    do {")));
  EXPECT_THAT(code, HasSubstr("do {\n        arg0 = (arg0 - 1);"));
}

TEST(AstConverterTest, PredecessorGuardPeelingStructuredForLoop) {
  // Predecessor (0x80020000):
  // 00: addiu $t0, $zero, 0      (loop counter init: temp_t0 = 0)
  // 04: slt   $at, $t0, $a0      (guard check: temp_t0 < arg0)
  // 08: beqz  $at, +6 -> 0x24    (skip loop if false)
  // 0C: nop
  // Loop body (0x80020010):
  // 10: addu  $v0, $v0, $t0      (accumulate)
  // 14: addiu $t0, $t0, 1        (increment loop counter: temp_t0 = temp_t0 + 1)
  // 18: slt   $at, $t0, $a0      (loop latch condition: temp_t0 < arg0)
  // 1C: bnez  $at, -4 -> 0x10    (latch branch back to loop body)
  // 20: nop                      (delay slot)
  // Exit (0x80020024):
  // 24: jr    $ra
  // 28: nop
  std::vector<uint32_t> words = {
      0x24080000,  // 00: addiu $t0, $zero, 0
      0x0104082A,  // 04: slt   $at, $t0, $a0
      0x10200006,  // 08: beq   $at, $zero, +6 -> 0x24
      0x00000000,  // 0C: nop
      0x00481021,  // 10: addu  $v0, $v0, $t0
      0x25080001,  // 14: addiu $t0, $t0, 1
      0x0104082A,  // 18: slt   $at, $t0, $a0
      0x1420FFFC,  // 1C: bne   $at, $zero, -4 -> 0x10
      0x00000000,  // 20: nop
      0x03E00008,  // 24: jr    $ra
      0x00000000,  // 28: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "GuardedLoopConversion";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // Should synthesize for (temp_t0 = 0; (temp_t0 < arg0); temp_t0++)
  EXPECT_THAT(code, HasSubstr("for (temp_t0 = 0; (temp_t0 < arg0); temp_t0++)"));
  // And temp_t0 = 0; should NOT appear duplicated outside the for loop
  EXPECT_THAT(code, Not(HasSubstr("temp_t0 = 0;\n    for")));
}

}  // namespace
}  // namespace rom_nom_nom
