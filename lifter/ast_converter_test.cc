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

  auto switch_region = std::make_unique<StructuredRegion>();
  switch_region->type = RegionType::kSwitch;
  switch_region->jump_table = &jt;

  StructuredCase case0;
  case0.case_values = {0, 1};
  switch_region->cases.push_back(std::move(case0));

  StructuredCase default_case;
  default_case.is_default = true;
  switch_region->cases.push_back(std::move(default_case));

  AstConverterOptions options;
  options.function_name = "TestSwitch";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *switch_region, nullptr, options);
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

  auto switch_region = std::make_unique<StructuredRegion>();
  switch_region->type = RegionType::kSwitch;
  switch_region->jump_table = &jt;

  StructuredCase case0;
  case0.case_values = {0};
  auto case_body = std::make_unique<StructuredRegion>();
  case_body->type = RegionType::kBlock;
  case_body->block_id = 0;
  case0.body = std::move(case_body);
  switch_region->cases.push_back(std::move(case0));

  AstConverterOptions options;
  options.function_name = "TestSwitchReturn";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *switch_region, nullptr, options);
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

  auto switch_region = std::make_unique<StructuredRegion>();
  switch_region->type = RegionType::kSwitch;
  switch_region->jump_table = nullptr;  // No jump table pointer

  StructuredCase default_case;
  default_case.is_default = true;
  switch_region->cases.push_back(std::move(default_case));

  AstConverterOptions options;
  options.function_name = "TestSwitchFallback";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *switch_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("switch (cond) {"));
  EXPECT_THAT(code, HasSubstr("default:"));
}

TEST(AstConverterTest, ConvertSwitchStatementDeclaresLocalVariables) {
  // Construct block 0 with: addiu $t0, $zero, 10; sw $t0, 0($a0); addiu $v0, $zero, 42; jr $ra; nop
  std::vector<uint32_t> words = {
      0x2408000A,  // addiu $t0, $zero, 10
      0xAC880000,  // sw    $t0, 0($a0)
      0x2402002A,  // addiu $v0, $zero, 42
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  JumpTable jt;
  jt.index_register = Register::kA0;

  auto switch_region = std::make_unique<StructuredRegion>();
  switch_region->type = RegionType::kSwitch;
  switch_region->jump_table = &jt;

  StructuredCase case0;
  case0.case_values = {0};
  auto case_body = std::make_unique<StructuredRegion>();
  case_body->type = RegionType::kBlock;
  case_body->block_id = 0;
  case0.body = std::move(case_body);
  switch_region->cases.push_back(std::move(case0));

  AstConverterOptions options;
  options.function_name = "TestSwitchVars";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *switch_region, nullptr, options);
  std::string code = func.ToString();

  // temp_t0 must be declared at the top of the function
  EXPECT_THAT(code, HasSubstr("register s32 temp_t0 asm(\"$8\");"));
  // Return type should be inferred as s32 because HasReturnValue detected return 42
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
  EXPECT_THAT(code, HasSubstr("register f32 f2 asm(\"$f2\");"));
  EXPECT_THAT(code, HasSubstr("register f32 f4 asm(\"$f4\");"));
  // Double precision variables are f64
  EXPECT_THAT(code, HasSubstr("f64 f12_d;"));
  EXPECT_THAT(code, HasSubstr("f64 f14_d;"));
  EXPECT_THAT(code, HasSubstr("register f64 f16_d asm(\"$f16\");"));
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
  EXPECT_THAT(code, HasSubstr("(f2 != f4)"));
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

TEST(AstConverterTest, VarsNeedingInitDataflowAcrossBlocks) {
  // Function:
  // 00: jal 0x80001000 (defines $v0, $ra)
  // 04: nop
  // 08: bgez $v0, +2 -> 0x14
  // 0C: nop
  // 10: addu $a0, $v0, $zero
  // 14: subu $a0, $v0, $zero
  // 18: jr $ra
  // 1C: nop
  std::vector<uint32_t> words = {
      0x0C000400,  // 00: jal 0x80001000
      0x00000000,  // 04: nop
      0x04410002,  // 08: bgez $v0, +2 -> 0x14
      0x00000000,  // 0C: nop
      0x00402021,  // 10: addu $a0, $v0, $zero
      0x00402023,  // 14: subu $a0, $v0, $zero
      0x03E00008,  // 18: jr $ra
      0x00000000,  // 1C: nop
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
  options.function_name = "DataflowReachingDefs";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // $v0 is defined by the jal in the entry block, so it reaches both blocks.
  // Therefore, v0 does NOT need "= 0" initialization.
  EXPECT_THAT(code, Not(HasSubstr("v0 = 0;")));
}

TEST(AstConverterTest, WhileLoopConditionInlining) {
  // 00: lb    $t0, 0($a0)        (load char: *(s8*)arg0)
  // 04: beq   $t0, $zero, +3 -> 0x14 (exit loop if *s == 0)
  // 08: nop
  // Loop body:
  // 0C: addiu $a0, $a0, 1        (arg0++)
  // 10: j     0x80020000         (loop back)
  // 14: nop
  // Exit:
  // 18: jr    $ra
  // 1C: nop
  std::vector<uint32_t> words = {
      0x80880000,  // 00: lb $t0, 0($a0)
      0x11000003,  // 04: beq $t0, $zero, +3 -> 0x14
      0x00000000,  // 08: nop
      0x24840001,  // 0C: addiu $a0, $a0, 1
      0x08008000,  // 10: j 0x80020000
      0x00000000,  // 14: nop
      0x03E00008,  // 18: jr $ra
      0x00000000,  // 1C: nop
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
  options.function_name = "WhileLoopInlining";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // The condition load should be inlined into the while condition without falling back to while
  // (1).
  EXPECT_THAT(code, Not(HasSubstr("while (1)")));
  EXPECT_THAT(code, HasSubstr("while ("));
}

TEST(AstConverterTest, ShortCircuitConditionSynthesis) {
  // Cascading branches:
  // 00: slti  $at, $a0, 10       (a0 < 10)
  // 04: beq   $at, $zero, +5 -> 0x1C (false -> exit)
  // 08: nop
  // 0C: slti  $at, $a1, 20       (a1 < 20)
  // 10: beq   $at, $zero, +2 -> 0x1C (false -> exit)
  // 14: nop
  // 18: addiu $v0, $zero, 1      (then: return 1)
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> words = {
      0x2881000A,  // 00: slti $at, $a0, 10
      0x10200005,  // 04: beq $at, $zero, +5 -> 0x1C
      0x00000000,  // 08: nop
      0x28A10014,  // 0C: slti $at, $a1, 20
      0x10200002,  // 10: beq $at, $zero, +2 -> 0x1C
      0x00000000,  // 14: nop
      0x24020001,  // 18: addiu $v0, $zero, 1
      0x03E00008,  // 1C: jr $ra
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
  options.function_name = "CascadingBranchAnd";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // Short-circuit || condition should be synthesized into if (...)
  EXPECT_THAT(code, HasSubstr("||"));
}

TEST(AstConverterTest, ChainedSelfAssignmentPreservesLvalueAndDefinitions) {
  // Pattern:
  // 00: or   $v0, $a0, $a1
  // 04: lui  $at, 0xA000
  // 08: or   $v0, $v0, $at
  // 0C: sw   $a2, 0($v0)
  // 10: jr   $ra
  // 14: nop
  std::vector<uint32_t> words = {
      0x00851025,  // 00: or  $v0, $a0, $a1
      0x3C01A000,  // 04: lui $at, 0xA000
      0x00411025,  // 08: or  $v0, $v0, $at
      0xAC460000,  // 0C: sw  $a2, 0($v0)
      0x03E00008,  // 10: jr  $ra
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
  options.function_name = "ChainedSelfAssignTest";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  // LHS of assignment must NEVER be replaced by an rvalue expression, while pure pointer
  // subexpressions inside a dereference *(s32*)ptr are folded cleanly.
  EXPECT_THAT(code, Not(HasSubstr(") = (")));
  EXPECT_THAT(code, HasSubstr("*(s32*)((arg0 | arg1) | 0xA0000000) = arg2;"));
}

TEST(AstConverterTest, FloatParametersInGeneralRegistersTyping) {
  // Function:
  // 00: mtc1  $a1, $f0
  // 04: jr    $ra
  // 08: nop
  std::vector<uint32_t> words = {
      0x44850000,  // mtc1 $a1, $f0
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
  options.function_name = "PassFloatInGpr";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  EXPECT_EQ(func.ReturnType().ToString(), "f32");
  ASSERT_GE(func.Parameters().size(), 2u);
  EXPECT_EQ(func.Parameters()[0].type.ToString(), "s32");
  EXPECT_EQ(func.Parameters()[1].type.ToString(), "f32");

  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("f32 PassFloatInGpr(s32 arg0, f32 arg1)"));
}

TEST(AstConverterTest, FloatToUintConversionAst) {
  std::vector<uint32_t> words = {
      0x3C014F00,  // 00: lui $at, 0x4F00
      0x00000000,  // 04: nop
      0x03E00008,  // 08: jr  $ra
      0x00000000,  // 0C: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  auto root = std::make_unique<StructuredRegion>();
  root->type = RegionType::kIfThenElse;
  root->condition_block_id = 0;
  root->float_to_uint_info = FloatToUintInfo{
      .entry_block_id = 0,
      .join_block_id = 0,
      .source_float_register = FpRegister::kF12,
      .destination_integer_register = Register::kV0,
      .is_double = false,
  };

  AstConverterOptions options;
  options.function_name = "ConvertFloat";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root, nullptr, options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("(u32)f12"));
}

TEST(AstConverterTest, ShiftAddMultiplicationSimplification) {
  std::vector<uint32_t> words = {
      // 00: sll  $v0, $a0, 2
      // 04: addu $v0, $v0, $a0  -> v0 = arg0 * 5
      // 08: sll  $v1, $v0, 2    -> v1 = arg0 * 20
      // 0C: sll  $t0, $a0, 4
      // 10: addu $t0, $t0, $a0
      // 14: sll  $t0, $t0, 2    -> t0 = arg0 * 68
      // 18: sll  $t1, $a0, 3
      // 1C: subu $t1, $t1, $a0  -> t1 = arg0 * 7
      // 20: jr   $ra
      // 24: nop
      0x00041080,  // sll  $v0, $a0, 2
      0x00441021,  // addu $v0, $v0, $a0
      0x00021880,  // sll  $v1, $v0, 2
      0x00044100,  // sll  $t0, $a0, 4
      0x01044021,  // addu $t0, $t0, $a0
      0x00084880,  // sll  $t0, $t0, 2
      0x000448C0,  // sll  $t1, $a0, 3
      0x01244823,  // subu $t1, $t1, $a0
      0x03E00008,  // jr   $ra
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
  options.function_name = "MulPatterns";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("(arg0 * 20)"));
  EXPECT_THAT(code, HasSubstr("(arg0 * 68)"));
  EXPECT_THAT(code, HasSubstr("(arg0 * 7)"));
}

TEST(AstConverterTest, FoldExpressionIntoIfCondition) {
  std::string textproto = R"pb(
    entries { name: "CheckItem" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // Function:
  // 00: jal  0x80001000  (CheckItem)
  // 04: nop
  // 08: beq  $v0, $zero, 0x18
  // 0C: nop
  // 10: addiu $v0, $zero, 1
  // 14: jr   $ra
  // 18: addiu $v0, $zero, 0
  // 1C: jr   $ra
  // 20: nop
  std::vector<uint32_t> words = {
      0x0C000400,  // 00: jal 0x80001000
      0x00000000,  // 04: nop
      0x10400003,  // 08: beq $v0, $zero, +3 (to 18)
      0x00000000,  // 0C: nop
      0x24020001,  // 10: addiu $v0, $zero, 1
      0x03E00008,  // 14: jr $ra
      0x24020000,  // 18: addiu $v0, $zero, 0
      0x03E00008,  // 1C: jr $ra
      0x00000000,  // 20: nop
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

  absl::flat_hash_set<std::string> v0_funcs = {"CheckItem"};
  AstConverterOptions options;
  options.function_name = "TestIfFold";
  options.functions_returning_v0 = &v0_funcs;

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("CheckItem()"));
}

TEST(AstConverterTest, FoldExpressionIntoReturn) {
  std::string textproto = R"pb(
    entries { name: "GetState" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // Function:
  // 00: jal  0x80001000  (GetState)
  // 04: nop
  // 08: jr   $ra
  // 0C: nop
  std::vector<uint32_t> words = {
      0x0C000400,  // 00: jal 0x80001000
      0x00000000,  // 04: nop
      0x03E00008,  // 08: jr $ra
      0x00000000,  // 0C: nop
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

  absl::flat_hash_set<std::string> v0_funcs = {"GetState"};
  AstConverterOptions options;
  options.function_name = "TestReturnFold";
  options.functions_returning_v0 = &v0_funcs;

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("return GetState();"));
}

TEST(AstConverterTest, FoldExpressionIntoSwitchCondition) {
  // Block 0:
  // 00: addiu $v0, $a0, 1
  // 04: beq   $zero, $zero, 0x10
  // 08: nop
  // Block 1:
  // 0C: nop
  // Block 2:
  // 10: jr    $ra
  // 14: nop
  std::vector<uint32_t> words = {
      0x24820001,  // 00: addiu $v0, $a0, 1
      0x10000002,  // 04: beq $zero, $zero, +2 (to 10)
      0x00000000,  // 08: nop
      0x00000000,  // 0C: nop
      0x03E00008,  // 10: jr  $ra
      0x00000000,  // 14: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  auto seq = std::make_unique<StructuredRegion>();
  seq->type = RegionType::kSequence;

  auto b0 = std::make_unique<StructuredRegion>();
  b0->type = RegionType::kBlock;
  b0->block_id = 0;
  seq->children.push_back(std::move(b0));

  auto sw = std::make_unique<StructuredRegion>();
  sw->type = RegionType::kSwitch;
  sw->switch_register = Register::kV0;
  StructuredCase c0;
  c0.case_values = {0};
  c0.body = std::make_unique<StructuredRegion>();
  c0.body->type = RegionType::kBlock;
  c0.body->block_id = 1;
  sw->cases.push_back(std::move(c0));
  seq->children.push_back(std::move(sw));

  AstConverterOptions options;
  options.function_name = "TestSwitchFold";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *seq, nullptr, options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("switch ((arg0 + 1))"));
}

TEST(AstConverterTest, ScopeAwareFoldingAcrossMultipleScratchUses) {
  std::string textproto = R"pb(
    entries { name: "StepOne" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
    entries { name: "StepTwo" address: 0x80002000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // Function calls StepOne(), branches on $v0, then calls StepTwo() and returns $v0:
  // 00: addiu $sp, $sp, -24
  // 04: sw    $ra, 20($sp)
  // 08: jal   0x80001000  (StepOne)
  // 0C: nop
  // 10: beq   $v0, $zero, +3 (to 20)
  // 14: nop
  // 18: jal   0x80002000  (StepTwo)
  // 1C: nop
  // 20: lw    $ra, 20($sp)
  // 24: jr    $ra
  // 28: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0x0C000400,  // 08: jal   0x80001000
      0x00000000,  // 0C: nop
      0x10400003,  // 10: beq   $v0, $zero, +3 (to 20)
      0x00000000,  // 14: nop
      0x0C000800,  // 18: jal   0x80002000
      0x00000000,  // 1C: nop
      0x8FBF0014,  // 20: lw    $ra, 20($sp)
      0x03E00008,  // 24: jr    $ra
      0x27BD0018,  // 28: addiu $sp, $sp, 24
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

  absl::flat_hash_set<std::string> v0_funcs = {"StepOne", "StepTwo"};
  AstConverterOptions options;
  options.function_name = "TestScopeAwareFold";
  options.functions_returning_v0 = &v0_funcs;

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("if ((StepOne() != 0))"));
}

TEST(AstConverterTest, GeneralInPlaceScratchRedefinitionFolding) {
  // v0 = *(u16*)arg0;
  // v0 = ((u32)v0 >> 3);
  // v0 = (v0 & 3);
  // return v0;
  std::vector<uint32_t> words = {
      0x94820000,  // 00: lhu  $v0, 0($a0)
      0x000210C2,  // 04: srl  $v0, $v0, 3
      0x30420003,  // 08: andi $v0, $v0, 3
      0x03E00008,  // 0C: jr   $ra
      0x00000000,  // 10: nop
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
  options.function_name = "ExtractBitfield";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("return (((u32)*(u16*)arg0 >> 3) & 3);"));
}

TEST(AstConverterTest, OutgoingStackArgumentFoldingIntoCall) {
  std::string textproto = R"pb(
    entries { name: "CallWithSixArgs" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // 00: addiu $sp, $sp, -32
  // 04: sw    $ra, 28($sp)
  // 08: addiu $v0, $zero, 10
  // 0C: sw    $v0, 16($sp)
  // 10: addiu $v1, $zero, 20
  // 14: sw    $v1, 20($sp)
  // 18: jal   0x80001000
  // 1C: nop
  // 20: lw    $ra, 28($sp)
  // 24: jr    $ra
  // 28: addiu $sp, $sp, 32
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0x2402000A,  // 08: addiu $v0, $zero, 10
      0xAF820010,  // 0C: sw    $v0, 16($sp) (wait: base $sp is 29 = 0x1D -> 0xAFA20010)
      0x24030014,  // 10: addiu $v1, $zero, 20
      0xAFA30014,  // 14: sw    $v1, 20($sp)
      0x0C000400,  // 18: jal   0x80001000
      0x00000000,  // 1C: nop
      0x8FBF001C,  // 20: lw    $ra, 28($sp)
      0x03E00008,  // 24: jr    $ra
      0x27BD0020,  // 28: addiu $sp, $sp, 32
  };
  words[3] = 0xAFA20010;  // sw $v0, 16($sp)

  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  absl::flat_hash_map<std::string, int> param_counts = {{"CallWithSixArgs", 6}};
  AstConverterOptions options;
  options.function_name = "TestStackArgsFold";
  options.function_parameter_counts = &param_counts;

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("CallWithSixArgs(arg0, arg1, arg2, arg3, 10, 20);"));
  EXPECT_THAT(code, Not(HasSubstr("var_sp_16")));
  EXPECT_THAT(code, Not(HasSubstr("var_sp_20")));
}

TEST(AstConverterTest, CalleeSavedRegistersOmitBogusZeroInitAndEmitHardwareRegisterBinding) {
  // Prologue saves $s0 and $s1 to stack BEFORE $s0 and $s1 are defined in the body:
  // 00: addiu $sp, $sp, -32
  // 04: sw    $ra, 24($sp)
  // 08: sw    $s1, 20($sp)
  // 0C: sw    $s0, 16($sp)
  // 10: addiu $s0, $a0, 5
  // 14: addiu $s1, $a1, 10
  // 18: jal   0x80001000
  // 1C: addu  $a0, $s0, $s1
  // 20: addu  $v0, $v0, $s0
  // 24: addu  $v0, $v0, $s1
  // 28: lw    $ra, 24($sp)
  // 2C: lw    $s1, 20($sp)
  // 30: lw    $s0, 16($sp)
  // 34: jr    $ra
  // 38: addiu $sp, $sp, 32
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF0018,  // 04: sw    $ra, 24($sp)
      0xAFB10014,  // 08: sw    $s1, 20($sp)
      0xAFB00010,  // 0C: sw    $s0, 16($sp)
      0x24840005,  // 10: addiu $s0, $a0, 5 (wait: $s0 is 16 -> 0x24900005)
      0x24B1000A,  // 14: addiu $s1, $a1, 10
      0x0C000400,  // 18: jal   0x80001000
      0x02112021,  // 1C: addu  $a0, $s0, $s1
      0x00501021,  // 20: addu  $v0, $v0, $s0
      0x00511021,  // 24: addu  $v0, $v0, $s1
      0x8FBF0018,  // 28: lw    $ra, 24($sp)
      0x8FB10014,  // 2C: lw    $s1, 20($sp)
      0x8FB00010,  // 30: lw    $s0, 16($sp)
      0x03E00008,  // 34: jr    $ra
      0x27BD0020,  // 38: addiu $sp, $sp, 32
  };
  words[4] = 0x24900005;  // addiu $s0, $a0, 5

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
  options.function_name = "TestCalleeSavedBindings";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("register s32 temp_s0 asm(\"$16\");"));
  EXPECT_THAT(code, HasSubstr("register s32 temp_s1 asm(\"$17\");"));
  EXPECT_THAT(code, Not(HasSubstr("temp_s0 = 0")));
  EXPECT_THAT(code, Not(HasSubstr("temp_s1 = 0")));
}

TEST(AstConverterTest, JumpTableBoundsCheckDoesNotClobberSwitchSelectorRegister) {
  // Mirrors GCC 2.7.2 jump table sequence where $v0 holds the switch selector (`lbu $v0, 3($a0)`),
  // $v1 holds the normalized index (`addiu $v1, $v0, -2`), and the bounds check `sltiu $v0, $v1, 6`
  // temporarily writes the boolean bounds result into $v0 before `sll $v0, $v1, 2` in the delay
  // slot of `beqz $v0, default`. The lifter must NOT emit `v0 = ((u32)v1 < 6);` before `switch`!
  std::vector<uint32_t> words = {
      0x90820003,  // 00: lbu   $v0, 3($a0)
      0x2443FFFE,  // 04: addiu $v1, $v0, -2
      0x2C620006,  // 08: sltiu $v0, $v1, 6
      0x10400008,  // 0C: beqz  $v0, +8 -> 0x30 (default/exit)
      0x00031080,  // 10: sll   $v0, $v1, 2
      0x3C018005,  // 14: lui   $at, 0x8005
      0x00220821,  // 18: addu  $at, $at, $v0
      0x8C220000,  // 1C: lw    $v0, 0($at)
      0x00400008,  // 20: jr    $v0
      0x00000000,  // 24: nop
      0x2402000A,  // 28: addiu $v0, $zero, 10 (case 2)
      0x03E00008,  // 2C: jr    $ra
      0x00000000,  // 30: nop                  (default/exit)
      0x03E00008,  // 34: jr    $ra
      0x00000000,  // 38: nop
  };

  auto insts = *DecodeSequence(words, 0x80000000);
  auto jts =
      JumpTableDetector::DetectJumpTables(insts, [](uint32_t vram) -> std::optional<uint32_t> {
        if (vram >= 0x80050000 && vram < 0x80050018) {
          return 0x80000028;
        }
        return std::nullopt;
      });
  ASSERT_EQ(jts.size(), 1u);

  auto cfg_or = ControlFlowGraph::Build(insts, jts);
  ASSERT_TRUE(cfg_or.ok());
  const auto& cfg = *cfg_or;

  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestSwitchSelectorNotClobbered";
  options.return_type = CType::Void();

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();
  EXPECT_THAT(code, Not(HasSubstr("< 6")));
  EXPECT_THAT(code, HasSubstr("switch (*(u8*)(arg0 + 3))"));
}

TEST(AstConverterTest, DeadPreCallArgAssignmentsEliminatedAndSignExtendedCallFolded) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"pb(
    entries { name: "QueryShortStatus" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb");
  ASSERT_TRUE(index_or.ok());

  // 00: addiu $sp, $sp, -24
  // 04: sw    $ra, 16($sp)
  // 08: lw    $v0, 4($a0)
  // 0C: addu  $a1, $v0, $zero
  // 10: jal   0x80001000 (QueryShortStatus)
  // 14: nop
  // 18: sll   $v0, $v0, 16
  // 1C: sra   $v0, $v0, 16
  // 20: lw    $ra, 16($sp)
  // 24: jr    $ra
  // 28: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0010,  // 04: sw    $ra, 16($sp)
      0x8C820004,  // 08: lw    $v0, 4($a0)
      0x00402821,  // 0C: addu  $a1, $v0, $zero
      0x0C000400,  // 10: jal   0x80001000
      0x00000000,  // 14: nop
      0x00021400,  // 18: sll   $v0, $v0, 16
      0x00021403,  // 1C: sra   $v0, $v0, 16
      0x8FBF0010,  // 20: lw    $ra, 16($sp)
      0x03E00008,  // 24: jr    $ra
      0x27BD0018,  // 28: addiu $sp, $sp, 24
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

  absl::flat_hash_map<std::string, int> param_counts = {{"QueryShortStatus", 2}};
  absl::flat_hash_set<std::string> returning_v0 = {"QueryShortStatus"};
  AstConverterOptions options;
  options.function_name = "TestSignExtendedShortCall";
  options.function_parameter_counts = &param_counts;
  options.functions_returning_v0 = &returning_v0;

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, Not(HasSubstr("arg1 = v0;")));
  EXPECT_THAT(code, HasSubstr("return (s16)QueryShortStatus(arg0, *(s32*)(arg0 + 4));"));
}

TEST(AstConverterTest, FloatInGprAndStackCallArgumentsRewrittenWithCalleeSavedFprBinding) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"pb(
    entries { name: "SpawnParticle" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb");
  ASSERT_TRUE(index_or.ok());

  // 00: addiu $sp, $sp, -32
  // 04: sw    $ra, 28($sp)
  // 08: swc1  $f20, 24($sp)
  // 0C: lwc1  $f20, 0($a0)
  // 10: mfc1  $a1, $f20
  // 14: mfc1  $a2, $f20
  // 18: mfc1  $a3, $f20
  // 1C: jal   0x80001000 (SpawnParticle)
  // 20: swc1  $f20, 16($sp)
  // 24: lwc1  $f20, 24($sp)
  // 28: lw    $ra, 28($sp)
  // 2C: jr    $ra
  // 30: addiu $sp, $sp, 32
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xE7B40018,  // 08: swc1  $f20, 24($sp)
      0xC4940000,  // 0C: lwc1  $f20, 0($a0)
      0x4405A000,  // 10: mfc1  $a1, $f20
      0x4406A000,  // 14: mfc1  $a2, $f20
      0x4407A000,  // 18: mfc1  $a3, $f20
      0x0C000400,  // 1C: jal   0x80001000
      0xE7B40010,  // 20: swc1  $f20, 16($sp)
      0xC7B40018,  // 24: lwc1  $f20, 24($sp)
      0x8FBF001C,  // 28: lw    $ra, 28($sp)
      0x03E00008,  // 2C: jr    $ra
      0x27BD0020,  // 30: addiu $sp, $sp, 32
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

  absl::flat_hash_map<std::string, int> param_counts = {{"SpawnParticle", 5}};
  AstConverterOptions options;
  options.function_name = "TestFloatGprAndStackCall";
  options.function_parameter_counts = &param_counts;

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("register f32 f20 asm(\"$f20\");"));
  EXPECT_THAT(code, Not(HasSubstr("*(s32*)&f20")));
  EXPECT_THAT(code, Not(HasSubstr("var_sp_16")));
  EXPECT_THAT(
      code,
      HasSubstr("((void (*)(s32, f32, f32, f32, f32))SpawnParticle)(arg0, f20, f20, f20, f20);"));
}

TEST(AstConverterTest, AnnotatesCallerSavedRegistersWhenNotLiveAcrossCalls) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "StepEntity" address: 0x80001000 type: SYMBOL_FUNC }
  )");
  ASSERT_TRUE(index_or.ok());

  // Non-leaf function where $t0 is only used after the call, while $t1 is defined before the call
  // and read after the call (so $t0 gets asm("$8") and $t1 must not get asm("$9")).
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0x24090005,  // 08: addiu $t1, $zero, 5
      0xAC890000,  // 0C: sw    $t1, 0($a0)
      0x0C000400,  // 10: jal   0x80001000 (StepEntity)
      0x00000000,  // 14: nop
      0x8C480000,  // 18: lw    $t0, 0($v0)
      0xAC480004,  // 1C: sw    $t0, 4($v0)
      0x01091021,  // 20: addu  $v0, $t0, $t1
      0x8FBF0014,  // 24: lw    $ra, 20($sp)
      0x03E00008,  // 28: jr    $ra
      0x27BD0018,  // 2C: addiu $sp, $sp, 24
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
  options.function_name = "TestCallerSavedLiveness";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("register s32 temp_t0 asm(\"$8\");"));
  EXPECT_THAT(code, HasSubstr("s32 temp_t1;"));
  EXPECT_THAT(code, Not(HasSubstr("register s32 temp_t1")));
}

TEST(AstConverterTest, CrossBlockEntryParametersPassedToCallAndGlobalArrayElementFolded) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "NotifySlotChanged" address: 0x80001000 type: SYMBOL_FUNC }
    entries { name: "g_slot_status" address: 0x80050000 type: SYMBOL_DATA }
  )");
  ASSERT_TRUE(index_or.ok());

  // Matches GlobalArrayConditionalUpdateAroundCall:
  // Entry block computes $v1 = &g_slot_status + ($a0 << 2), branches on lw $v0, 0($v1) == $a1,
  // and in the fallthrough block stores $a1 to 0($v1) and calls NotifySlotChanged($a0, $a1)
  // using the entry parameters $a0 and $a1 without redefining them in the successor block.
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0x3C028005,  // 04: lui   $v0, 0x8005
      0x24420000,  // 08: addiu $v0, $v0, 0
      0x00041880,  // 0C: sll   $v1, $a0, 2
      0x00621821,  // 10: addu  $v1, $v1, $v0
      0xAFBF0010,  // 14: sw    $ra, 16($sp)
      0x8C620000,  // 18: lw    $v0, 0($v1)
      0x10450004,  // 1C: beq   $v0, $a1, 0x80000030
      0x00001021,  // 20: addu  $v0, $zero, $zero
      0x0C000400,  // 24: jal   0x80001000 (NotifySlotChanged)
      0xAC650000,  // 28: sw    $a1, 0($v1)
      0x24020001,  // 2C: addiu $v0, $zero, 1
      0x8FBF0010,  // 30: lw    $ra, 16($sp)
      0x27BD0018,  // 34: addiu $sp, $sp, 24
      0x03E00008,  // 38: jr    $ra
      0x00000000,  // 3C: nop
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
  options.function_name = "GlobalArrayConditionalUpdateAroundCall";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("NotifySlotChanged(arg0, arg1);"));
  EXPECT_THAT(code, HasSubstr("*(s32*)(&g_slot_status + arg0)"));
}

TEST(AstConverterTest, InlinesCrossJumpedCallTailsAndSimplifiesSignExtensionShiftPairs) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "DispatchCmd" address: 0x80001000 type: SYMBOL_FUNC }
  )");
  ASSERT_TRUE(index_or.ok());

  // Outer `if (arg0 > 0)` sets `arg1 = (s16)arg1` and jumps to shared call block 0x2C;
  // `else if (arg1 > 0)` sets `arg1 = 7` and falls through to shared call block 0x2C;
  // `else` jumps to epilogue 0x34 (mirroring CallWithSevenConstantAndStackArgsInsideBranch):
  // 00: addiu $sp, $sp, -24
  // 04: sw    $ra, 16($sp)
  // 08: blez  $a0, +4 -> 0x1C (else branch)
  // 0C: nop
  // 10: sll   $v0, $a1, 16
  // 14: b     +5 -> 0x2C (shared call block)
  // 18: sra   $a1, $v0, 16
  // 1C: blez  $a1, +5 -> 0x34 (epilogue)
  // 20: nop
  // 24: addiu $a1, $zero, 7
  // 28: nop
  // 2C: jal   0x80001000 (DispatchCmd)
  // 30: nop
  // 34: lw    $ra, 16($sp)
  // 38: jr    $ra
  // 3C: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0010,  // 04: sw    $ra, 16($sp)
      0x18800004,  // 08: blez  $a0, +4 -> 0x1C
      0x00000000,  // 0C: nop
      0x00051400,  // 10: sll   $v0, $a1, 16
      0x10000005,  // 14: b     +5 -> 0x2C
      0x00022C03,  // 18: sra   $a1, $v0, 16
      0x18A00005,  // 1C: blez  $a1, +5 -> 0x34
      0x00000000,  // 20: nop
      0x24050007,  // 24: addiu $a1, $zero, 7
      0x00000000,  // 28: nop
      0x0C000400,  // 2C: jal   0x80001000 (DispatchCmd)
      0x00000000,  // 30: nop
      0x8FBF0010,  // 34: lw    $ra, 16($sp)
      0x03E00008,  // 38: jr    $ra
      0x27BD0018,  // 3C: addiu $sp, $sp, 24
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
  options.function_name = "TestCrossJumpedCallAndSignExtend";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("arg1 = (s16)arg1;"));
  EXPECT_THAT(code, HasSubstr("DispatchCmd(arg0, arg1);"));
  EXPECT_THAT(code, HasSubstr("DispatchCmd(arg0, 7);"));
  EXPECT_THAT(code, Not(HasSubstr("goto ")));
}

TEST(AstConverterTest, FoldsCallReturnAndLoadsAcrossProvablyDisjointStructStores) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "StepVal" address: 0x80001000 type: SYMBOL_FUNC }
  )");
  ASSERT_TRUE(index_or.ok());

  // 00: addiu $sp, $sp, -32
  // 04: sw    $s0, 16($sp)
  // 08: addu  $s0, $a0, $zero
  // 0C: sw    $ra, 28($sp)
  // 10: lw    $a0, 12($s0)
  // 14: jal   0x80001000 (StepVal)
  // 18: nop
  // 1C: lw    $a0, 16($s0)
  // 20: jal   0x80001000 (StepVal)
  // 24: sw    $v0, 12($s0)       # delay slot stores first StepVal result to +12
  // 28: sw    $v0, 16($s0)       # stores second StepVal result to +16
  // 2C: lw    $v0, 0($s0)
  // 30: lw    $a1, 12($s0)
  // 34: lw    $v1, 4($s0)
  // 38: lw    $a2, 16($s0)
  // 3C: addu  $v0, $v0, $a1
  // 40: addu  $v1, $v1, $a2
  // 44: sw    $v0, 0($s0)
  // 48: sw    $v1, 4($s0)
  // 4C: lw    $ra, 28($sp)
  // 50: lw    $s0, 16($sp)
  // 54: jr    $ra
  // 58: addiu $sp, $sp, 32
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFB00010,  // 04: sw    $s0, 16($sp)
      0x00808021,  // 08: addu  $s0, $a0, $zero
      0xAFBF001C,  // 0C: sw    $ra, 28($sp)
      0x8E04000C,  // 10: lw    $a0, 12($s0)
      0x0C000400,  // 14: jal   0x80001000 (StepVal)
      0x00000000,  // 18: nop
      0x8E040010,  // 1C: lw    $a0, 16($s0)
      0x0C000400,  // 20: jal   0x80001000 (StepVal)
      0xAE02000C,  // 24: sw    $v0, 12($s0)
      0xAE020010,  // 28: sw    $v0, 16($s0)
      0x8E020000,  // 2C: lw    $v0, 0($s0)
      0x8E05000C,  // 30: lw    $a1, 12($s0)
      0x8E030004,  // 34: lw    $v1, 4($s0)
      0x8E060010,  // 38: lw    $a2, 16($s0)
      0x00451021,  // 3C: addu  $v0, $v0, $a1
      0x00661821,  // 40: addu  $v1, $v1, $a2
      0xAE020000,  // 44: sw    $v0, 0($s0)
      0xAE030004,  // 48: sw    $v1, 4($s0)
      0x8FBF001C,  // 4C: lw    $ra, 28($sp)
      0x8FB00010,  // 50: lw    $s0, 16($sp)
      0x03E00008,  // 54: jr    $ra
      0x27BD0020,  // 58: addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestDisjointStructStores";
  absl::flat_hash_set<std::string> funcs_returning_v0 = {"StepVal"};
  options.functions_returning_v0 = &funcs_returning_v0;

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("*(s32*)(arg0 + 12) = StepVal(*(s32*)(arg0 + 12));"));
  EXPECT_THAT(code, HasSubstr("*(s32*)(arg0 + 16) = StepVal(*(s32*)(arg0 + 16));"));
  EXPECT_THAT(code, HasSubstr("*(s32*)arg0 = (*(s32*)arg0 + *(s32*)(arg0 + 12));"));
  EXPECT_THAT(code, HasSubstr("*(s32*)(arg0 + 4) = (*(s32*)(arg0 + 4) + *(s32*)(arg0 + 16));"));
}

TEST(AstConverterTest, SplitsReusedFloatGprParameterIntoIntegerTempAndTypesParamAsF32) {
  // Pattern from HM64 actor.c (func_8002BD0C):
  //   00: sll   $v0, $a0, 16
  //   04: srl   $a0, $v0, 16
  //   08: mtc1  $a1, $f0       # $a1 is f32 arg1
  //   0C: sltiu $v0, $a0, 64
  //   10: beq   $v0, $zero, 0x80000024
  //   14: addu  $a1, $zero, $zero  # $a1 reused as integer status result
  //   18: swc1  $f0, 0($a2)
  //   1C: addiu $a1, $zero, 1
  //   20: jr    $ra
  //   24: addu  $v0, $a1, $zero
  std::vector<uint32_t> words = {
      0x00041400,  // 00: sll   $v0, $a0, 16
      0x00022402,  // 04: srl   $a0, $v0, 16
      0x44850000,  // 08: mtc1  $a1, $f0
      0x2C820040,  // 0C: sltiu $v0, $a0, 64
      0x10400003,  // 10: beq   $v0, $zero, 0x80000020
      0x00002821,  // 14: addu  $a1, $zero, $zero
      0xE4C00000,  // 18: swc1  $f0, 0($a2)
      0x24050001,  // 1C: addiu $a1, $zero, 1
      0x03E00008,  // 20: jr    $ra
      0x00A01021,  // 24: addu  $v0, $a1, $zero
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestReusedFloatGprParam";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("f32 arg1"));
  EXPECT_THAT(code, HasSubstr("s32 temp_a1;"));
  EXPECT_THAT(code, HasSubstr("*(f32*)arg2 = arg1;"));
  EXPECT_THAT(code, HasSubstr("return temp_a1;"));
}

}  // namespace
}  // namespace rom_nom_nom
