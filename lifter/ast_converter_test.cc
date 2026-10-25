#include "lifter/ast_converter.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/jump_table.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/branch_condition_builder.h"
#include "lifter/cfg_de_reorg.h"
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

  // Value casts lower to ExpressionKind::kCast and CastExpression rather than UnaryExpression
  for (const char* cast_op :
       {"(u32)", "(s32)", "(u16)", "(s16)", "(u8)", "(s8)", "(f32)", "(f64)", "(s64)", "(u64)"}) {
    auto cast_expr = LiftedExpression::Unary(cast_op, LiftedExpression::Variable("arg0"));
    EXPECT_EQ(cast_expr->kind, ExpressionKind::kCast) << "for " << cast_op;
    auto c_cast = AstConverter::ConvertExpression(*cast_expr);
    EXPECT_EQ(c_cast->Kind(), CExpressionKind::kCastExpression) << "for " << cast_op;
  }

  auto u64_u32_expr = LiftedExpression::Unary("(u64)(u32)", LiftedExpression::Variable("arg0"));
  auto c_u64_u32 = AstConverter::ConvertExpression(*u64_u32_expr);
  ASSERT_EQ(c_u64_u32->Kind(), CExpressionKind::kCastExpression);
  const auto& outer_cast = static_cast<const CastExpression&>(*c_u64_u32);
  EXPECT_EQ(outer_cast.TargetType().ToString(), "u64");
  ASSERT_EQ(outer_cast.Operand().Kind(), CExpressionKind::kCastExpression);
  EXPECT_EQ(static_cast<const CastExpression&>(outer_cast.Operand()).TargetType().ToString(),
            "u32");

  auto cvt_s_w_expr = LiftedExpression::Unary("(f32)*(s32*)&", LiftedExpression::Variable("f0"));
  auto c_cvt_s_w = AstConverter::ConvertExpression(*cvt_s_w_expr);
  ASSERT_EQ(c_cvt_s_w->Kind(), CExpressionKind::kCastExpression);
  EXPECT_EQ(static_cast<const CastExpression&>(*c_cvt_s_w).TargetType().ToString(), "f32");
  EXPECT_EQ(c_cvt_s_w->ToString(), "(f32)*(s32*)&f0");
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
    EXPECT_EQ(BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/false)
                  ->ToString(),
              "(f2 == f4)");
    EXPECT_EQ(BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/true)
                  ->ToString(),
              "(f2 != f4)");
  }

  // c.ult.s $f2, $f4 (0x46041035) + bc1f (0x45000002)
  {
    auto block = make_fpu_branch_block(0x46041035, 0x45000002);
    EXPECT_EQ(BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/false)
                  ->ToString(),
              "!(f2 < f4)");
    EXPECT_EQ(BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/true)
                  ->ToString(),
              "(f2 < f4)");
  }

  // c.ule.d $f12, $f14 (0x462E6037) + bc1t (0x45010002)
  {
    auto block = make_fpu_branch_block(0x462E6037, 0x45010002);
    EXPECT_EQ(BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/false)
                  ->ToString(),
              "(f12_d <= f14_d)");
    EXPECT_EQ(BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/true)
                  ->ToString(),
              "!(f12_d <= f14_d)");
  }

  // c.ngle.s $f2, $f4 (0x46041039) + bc1t (0x45010002)
  {
    auto block = make_fpu_branch_block(0x46041039, 0x45010002);
    EXPECT_EQ(BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/false)
                  ->ToString(),
              "(f2 <= f4)");
  }

  // c.ngl.d $f12, $f14 (0x462E603B) + bc1f (0x45000002)
  {
    auto block = make_fpu_branch_block(0x462E603B, 0x45000002);
    EXPECT_EQ(BranchConditionBuilder::ExtractBranchCondition(block, /*invert_condition=*/false)
                  ->ToString(),
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

  // Short-circuit && condition should be synthesized into if (...)
  EXPECT_THAT(code, HasSubstr("if (((arg0 < 10) && (arg1 < 20)))"));
}

TEST(AstConverterTest, ShortCircuitOrConditionSynthesis) {
  // Cascading branches for (arg0 < 10 || arg1 < 20):
  // 00: slti  $at, $a0, 10       (a0 < 10)
  // 04: bne   $at, $zero, +4 -> 0x18 (true -> then)
  // 08: nop
  // 0C: slti  $at, $a1, 20       (a1 < 20)
  // 10: beq   $at, $zero, +2 -> 0x1C (false -> exit)
  // 14: nop
  // 18: addiu $v0, $zero, 1      (then: v0 = 1)
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> words = {
      0x2881000A,  // 00: slti $at, $a0, 10
      0x14200004,  // 04: bne  $at, $zero, +4 -> 0x18
      0x00000000,  // 08: nop
      0x28A10014,  // 0C: slti $at, $a1, 20
      0x10200002,  // 10: beq  $at, $zero, +2 -> 0x1C
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
  options.function_name = "CascadingBranchOr";

  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("if (((arg0 < 10) || (arg1 < 20)))"));
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

TEST(AstConverterTest, LoopBreakTargetingEpilogueThatModifiesV0EmitsBreak) {
  // Loop where an early exit jumps to the epilogue block (0x1C), and the epilogue
  // block sets $v0 = $s0 before returning (rather than returning the loop's $v0):
  //   00: addiu $sp, $sp, -24
  //   04: sw    $s0, 16($sp)
  //   08: move  $s0, $a1
  // .Lloop:
  //   0C: addiu $a0, $a0, -1
  //   10: beqz  $a0, .Lexit (0x20)
  //   14: addiu $v0, $zero, 99
  //   18: bnez  $a0, .Lloop (0x0C)
  //   1C: nop
  // .Lexit:
  //   20: move  $v0, $s0
  //   24: lw    $s0, 16($sp)
  //   28: jr    $ra
  //   2C: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFB00010,  // 04: sw    $s0, 16($sp)
      0x00A08021,  // 08: addu  $s0, $a1, $zero
      0x2484FFFF,  // 0C: addiu $a0, $a0, -1
      0x10800003,  // 10: beqz  $a0, +3 (to 0x20)
      0x24020063,  // 14: addiu $v0, $zero, 99
      0x1480FFFC,  // 18: bnez  $a0, -4 (to 0x0C)
      0x00000000,  // 1C: nop
      0x02001021,  // 20: addu  $v0, $s0, $zero
      0x8FB00010,  // 24: lw    $s0, 16($sp)
      0x03E00008,  // 28: jr    $ra
      0x27BD0018,  // 2C: addiu $sp, $sp, 24
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestLoopBreakToNonTrivialEpilogue";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("break;"));
  EXPECT_THAT(code, Not(HasSubstr("return 99;")));
}

TEST(AstConverterTest, GeneralizedForLoopReconstructionCountdownAndNonUnitStride) {
  // 1. Countdown loop: for (temp_t0 = arg0; (temp_t0 > 0); temp_t0--)
  // 00: addu  $t0, $a0, $zero    (temp_t0 = arg0)
  // 04: blez  $t0, +5 -> 0x1C    (guard: temp_t0 > 0)
  // 08: nop
  // 0C: addu  $v0, $v0, $t0      (v0 = v0 + temp_t0)
  // 10: addiu $t0, $t0, -1       (temp_t0 = temp_t0 - 1)
  // 14: bgtz  $t0, -3 -> 0x0C    (latch: temp_t0 > 0)
  // 18: nop
  // 1C: jr    $ra
  // 20: nop
  std::vector<uint32_t> countdown_words = {
      0x00804021,  // 00: addu  $t0, $a0, $zero
      0x19000005,  // 04: blez  $t0, +5 -> 0x1C
      0x00000000,  // 08: nop
      0x00481021,  // 0C: addu  $v0, $v0, $t0
      0x2508FFFF,  // 10: addiu $t0, $t0, -1
      0x1D00FFFD,  // 14: bgtz  $t0, -3 -> 0x0C
      0x00000000,  // 18: nop
      0x03E00008,  // 1C: jr    $ra
      0x00000000,  // 20: nop
  };
  auto countdown_cfg = *ControlFlowGraph::Build(*DecodeSequence(countdown_words, 0x80020000));
  DominatorTree countdown_dom = DominatorTree::Compute(countdown_cfg);
  DominatorTree countdown_pdom = DominatorTree::ComputePostDominators(countdown_cfg);
  LoopInfo countdown_loops = LoopInfo::Analyze(countdown_cfg, countdown_dom);
  auto countdown_region = ControlFlowStructurer::Structure(countdown_cfg, countdown_dom,
                                                           countdown_pdom, countdown_loops);
  ASSERT_THAT(countdown_region, NotNull());

  AstConverterOptions countdown_opts;
  countdown_opts.function_name = "CountdownForLoop";
  FunctionDeclaration countdown_func =
      *AstConverter::Convert(countdown_cfg, *countdown_region, nullptr, countdown_opts);
  std::string countdown_code = countdown_func.ToString();
  EXPECT_THAT(countdown_code, HasSubstr("for (temp_t0 = arg0; (temp_t0 > 0); temp_t0--)"));
  EXPECT_THAT(countdown_code, Not(HasSubstr("temp_t0 = arg0;\n    for")));

  // 2. Non-zero init and stride += 2 with <= bound:
  // for (temp_t0 = 1; (temp_t0 <= arg0); temp_t0 += 2)
  // 00: addiu $t0, $zero, 1      (temp_t0 = 1)
  // 04: blez  $a0, +6 -> 0x20    (guard: arg0 > 0, equivalent to 1 <= arg0)
  // 08: nop
  // 0C: addu  $v0, $v0, $t0      (v0 = v0 + temp_t0)
  // 10: addiu $t0, $t0, 2        (temp_t0 = temp_t0 + 2)
  // 14: slt   $at, $a0, $t0      (at = arg0 < temp_t0)
  // 18: beqz  $at, -4 -> 0x0C    (latch: temp_t0 <= arg0)
  // 1C: nop
  // 20: jr    $ra
  // 24: nop
  std::vector<uint32_t> stride_words = {
      0x24080001,  // 00: addiu $t0, $zero, 1
      0x18800006,  // 04: blez  $a0, +6 -> 0x20
      0x00000000,  // 08: nop
      0x00481021,  // 0C: addu  $v0, $v0, $t0
      0x25080002,  // 10: addiu $t0, $t0, 2
      0x0088082A,  // 14: slt   $at, $a0, $t0
      0x1020FFFC,  // 18: beqz  $at, -4 -> 0x0C
      0x00000000,  // 1C: nop
      0x03E00008,  // 20: jr    $ra
      0x00000000,  // 24: nop
  };
  auto stride_cfg = *ControlFlowGraph::Build(*DecodeSequence(stride_words, 0x80020000));
  DominatorTree stride_dom = DominatorTree::Compute(stride_cfg);
  DominatorTree stride_pdom = DominatorTree::ComputePostDominators(stride_cfg);
  LoopInfo stride_loops = LoopInfo::Analyze(stride_cfg, stride_dom);
  auto stride_region =
      ControlFlowStructurer::Structure(stride_cfg, stride_dom, stride_pdom, stride_loops);
  ASSERT_THAT(stride_region, NotNull());

  AstConverterOptions stride_opts;
  stride_opts.function_name = "StrideForLoop";
  FunctionDeclaration stride_func =
      *AstConverter::Convert(stride_cfg, *stride_region, nullptr, stride_opts);
  std::string stride_code = stride_func.ToString();
  EXPECT_THAT(stride_code, HasSubstr("for (temp_t0 = 1; (temp_t0 <= arg0); temp_t0 += 2)"));
  EXPECT_THAT(stride_code, Not(HasSubstr("temp_t0 = 1;\n    for")));
}

TEST(AstConverterTest, DaddiuStackFrameAllocationMapsIncomingStackArgsCorrectly) {
  // Function using 64-bit daddiu $sp, $sp, -16 prologue, storing a local at 8($sp) and reading
  // 5th argument (arg4) at 32($sp):
  std::vector<uint32_t> words = {
      0x67BDFFF0,  // 00: daddiu $sp, $sp, -16
      0xAFA40008,  // 04: sw     $a0, 8($sp)
      0x8FA20020,  // 08: lw     $v0, 32($sp)
      0x8FA30008,  // 0C: lw     $v1, 8($sp)
      0x00431021,  // 10: addu   $v0, $v0, $v1
      0x03E00008,  // 14: jr     $ra
      0x67BD0010,  // 18: daddiu $sp, $sp, 16
  };
  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80001000));
  CfgDeReorg::UnscheduleDelaySlots(&cfg);
  DominatorTree dom = DominatorTree::Compute(cfg);
  DominatorTree pdom = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dom);
  auto region = ControlFlowStructurer::Structure(cfg, dom, pdom, loops);
  ASSERT_THAT(region, NotNull());

  AstConverterOptions opts;
  opts.function_name = "DaddiuFrameFunc";
  auto func_or = AstConverter::Convert(cfg, *region, nullptr, opts);
  ASSERT_TRUE(func_or.ok()) << func_or.status();
  std::string code = func_or->ToString();
  EXPECT_THAT(code,
              HasSubstr("s32 DaddiuFrameFunc(s32 arg0, s32 arg1, s32 arg2, s32 arg3, s32 arg4)"));
  EXPECT_THAT(code, HasSubstr("arg4"));
  EXPECT_THAT(code, Not(HasSubstr("arg8")));
}

TEST(AstConverterTest,
     PeelsNonBranchLikelyDoWhileLoopRotationAndSkipsDuplicateExitDelayOnEarlyBreak) {
  // Block 0 (preheader):
  //   0x00: addiu $v1, $zero, 0
  //   0x04: andi  $v0, $a0, 0xFF
  // Block 1 (loop header):
  //   0x08: bnel  $v0, $zero, +5 -> 0x20 (block 4, after loop_follow)
  //   0x0C: sh    $v1, 0($a1)            (delay slot matches loop_follow block 3)
  // Block 2 (loop latch):
  //   0x10: addiu $v1, $v1, 1
  //   0x14: srl   $a0, $a0, 8
  //   0x18: bne   $v1, $a2, -5 -> 0x08 (block 1)
  //   0x1C: andi  $v0, $a0, 0xFF        (peeled rotation instruction clobbering $v0, dead on exit)
  // Block 3 (loop_follow):
  //   0x20: sh    $v1, 0($a1)
  // Block 4 (return):
  //   0x24: jr    $ra
  //   0x28: nop
  std::vector<uint32_t> words = {
      0x24030000,  // 0x00: addiu $v1, $zero, 0
      0x308200FF,  // 0x04: andi  $v0, $a0, 0xFF
      0x54400005,  // 0x08: bnel  $v0, $zero, +5 -> 0x24 (Wait: 0x0C + 5*4 = 0x20, +6 = 0x24)
      0xA4A30000,  // 0x0C: sh    $v1, 0($a1)
      0x24630001,  // 0x10: addiu $v1, $v1, 1
      0x00042202,  // 0x14: srl   $a0, $a0, 8
      0x1466FFFB,  // 0x18: bne   $v1, $a2, -5 -> 0x08
      0x308200FF,  // 0x1C: andi  $v0, $a0, 0xFF
      0xA4A30000,  // 0x20: sh    $v1, 0($a1)
      0x03E00008,  // 0x24: jr    $ra
      0x00000000,  // 0x28: nop
  };
  words[2] = 0x54400006;  // 0x08: bnel $v0, $zero, +6 -> 0x24 (block 4)

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80001000));
  CfgDeReorg::UnscheduleDelaySlots(&cfg);
  DominatorTree dom = DominatorTree::Compute(cfg);
  DominatorTree pdom = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dom);
  auto region = ControlFlowStructurer::Structure(cfg, dom, pdom, loops);
  ASSERT_THAT(region, NotNull());

  AstConverterOptions opts;
  opts.function_name = "PeeledLoopWithEarlyBreak";
  opts.return_type = CType::Void();
  auto func_or = AstConverter::Convert(cfg, *region, nullptr, opts);
  ASSERT_TRUE(func_or.ok()) << func_or.status();
  std::string code = func_or->ToString();
  EXPECT_THAT(code, HasSubstr("do {"));
  EXPECT_THAT(code, HasSubstr("break;"));
  EXPECT_THAT(code, Not(HasSubstr("while (1)")));
  EXPECT_THAT(code, Not(HasSubstr("goto")));
}

TEST(AstConverterTest,
     FloatToUintRegionEmitsTrailingMaskAndPreservesNonDiamondBranchDelayInstruction) {
  // Single-precision float-to-uint conversion diamond where:
  // 1. The non-likely `bc1t` branch has an unrelated `sub.s $f8, $f14, $f12` in its delay slot.
  // 2. Both branches have a stolen trailing `andi $v0, $v0, 0xFF` instruction.
  std::vector<uint32_t> words = {
      0x3C014F00,  // 0x00: lui       $at, 0x4F00
      0x44811000,  // 0x04: mtc1      $at, $f2
      0x460C103E,  // 0x08: c.le.s    $f2, $f12
      0x45010004,  // 0x0C: bc1t      +4 -> 0x20 (high path)
      0x460C7201,  // 0x10: sub.s     $f8, $f14, $f12 (unrelated delay slot instruction!)
      0x4600600D,  // 0x14: trunc.w.s $f0, $f12 (low path)
      0x44020000,  // 0x18: mfc1      $v0, $f0
      0x0800000E,  // 0x1C: j         0x80000038 (join)
      0x304200FF,  // 0x20: andi      $v0, $v0, 0xFF -> wait: 0x20 is delay slot of j, so high path
                   // is 0x24!
  };
  words = {
      0x3C014F00,  // 0x00: lui       $at, 0x4F00
      0x44811000,  // 0x04: mtc1      $at, $f2
      0x460C103E,  // 0x08: c.le.s    $f2, $f12
      0x45010005,  // 0x0C: bc1t      +5 -> 0x24 (high path)
      0x460C7201,  // 0x10: sub.s     $f8, $f14, $f12 (unrelated delay slot instruction)
      0x4600600D,  // 0x14: trunc.w.s $f0, $f12 (low path)
      0x44020000,  // 0x18: mfc1      $v0, $f0
      0x0800000F,  // 0x1C: j         0x8000003C (join)
      0x304200FF,  // 0x20: andi      $v0, $v0, 0xFF (delay slot of j)
      0x46026101,  // 0x24: sub.s     $f4, $f12, $f2 (high path)
      0x4600200D,  // 0x28: trunc.w.s $f0, $f4
      0x44020000,  // 0x2C: mfc1      $v0, $f0
      0x3C018000,  // 0x30: lui       $at, 0x8000
      0x00411025,  // 0x34: or        $v0, $v0, $at
      0x304200FF,  // 0x38: andi      $v0, $v0, 0xFF (trailing instruction at end of high path)
      0xE7A80000,  // 0x3C: swc1      $f8, 0($sp) (join block uses $f8 and returns $v0)
      0x03E00008,  // 0x40: jr        $ra
      0x00000000,  // 0x44: nop
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dom = DominatorTree::Compute(cfg);
  DominatorTree pdom = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dom);
  auto region = ControlFlowStructurer::Structure(cfg, dom, pdom, loops);
  ASSERT_THAT(region, NotNull());

  AstConverterOptions opts;
  opts.function_name = "FloatToUintWithDelaySubAndTrailingMask";
  auto func_or = AstConverter::Convert(cfg, *region, nullptr, opts);
  ASSERT_TRUE(func_or.ok()) << func_or.status();
  std::string code = func_or->ToString();
  EXPECT_THAT(code, HasSubstr("(f14 - f12)"));
  EXPECT_THAT(code, HasSubstr("((u32)f12 & 255)"));
}

TEST(AstConverterTest, HoistsSingleExitBranchLikelyDelaySlotAfterInfiniteLoop) {
  // Loop with a function call in header (splitting CFG blocks so LoopInfo classifies as kInfinite)
  // and a `beql $v0, $zero, exit` whose delay slot `sb $zero, 0($s0)` executes on loop exit:
  //   0x00: addiu $sp, $sp, -24
  //   0x04: sw    $ra, 20($sp)
  //   0x08: sw    $s0, 16($sp)
  //   0x0C: addu  $s0, $a0, $zero
  //   0x10: jal   0x80001000         (loop header)
  //   0x14: addu  $a0, $s0, $zero
  //   0x18: beql  $v0, $zero, +3 -> 0x28 (exit block)
  //   0x1C: sb    $zero, 0($s0)      (exit delay slot)
  //   0x20: j     0x80000010         (back-edge to loop header)
  //   0x24: addiu $s0, $s0, 4
  //   0x28: lw    $ra, 20($sp)       (exit block)
  //   0x2C: lw    $s0, 16($sp)
  //   0x30: jr    $ra
  //   0x34: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 0x00: addiu $sp, $sp, -24
      0xAFBF0014,  // 0x04: sw    $ra, 20($sp)
      0xAFB00010,  // 0x08: sw    $s0, 16($sp)
      0x00808021,  // 0x0C: addu  $s0, $a0, $zero
      0x0C000400,  // 0x10: jal   0x80001000
      0x02002021,  // 0x14: addu  $a0, $s0, $zero
      0x50400004,  // 0x18: beql  $v0, $zero, +4 -> 0x2C
      0xA2000000,  // 0x1C: sb    $zero, 0($s0)
      0x26100004,  // 0x20: addiu $s0, $s0, 4
      0x08000004,  // 0x24: j     0x80000010
      0xAE000004,  // 0x28: sw    $zero, 4($s0)
      0x8FBF0014,  // 0x2C: lw    $ra, 20($sp)
      0x8FB00010,  // 0x30: lw    $s0, 16($sp)
      0x03E00008,  // 0x34: jr    $ra
      0x27BD0018,  // 0x38: addiu $sp, $sp, 24
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dom = DominatorTree::Compute(cfg);
  DominatorTree pdom = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dom);
  auto region = ControlFlowStructurer::Structure(cfg, dom, pdom, loops);
  ASSERT_THAT(region, NotNull());

  AstConverterOptions opts;
  opts.function_name = "LoopWithBranchLikelyExitStore";
  opts.return_type = CType::Void();
  auto func_or = AstConverter::Convert(cfg, *region, nullptr, opts);
  ASSERT_TRUE(func_or.ok()) << func_or.status();
  std::string code = func_or->ToString();
  EXPECT_THAT(code, HasSubstr("if ((v0 == 0)) {\n            break;\n        }"));
  EXPECT_THAT(code, HasSubstr("}\n    *(s8*)temp_s0 = 0;"));
}

TEST(AstConverterTest,
     FoldsShortCircuitOrWithMultiInstructionConditionBlockAndEliminatesDeadScratchSetup) {
  // Block 0 (0x00):
  //   lhu   $v0, 0($a0)
  //   andi  $v0, $v0, 0x0004
  //   bnez  $v0, 0x20 (then_body)
  //   nop
  // Block 1 (0x10 - multi-instruction condition block):
  //   lhu   $v0, 2($a0)
  //   andi  $v0, $v0, 0x0008
  //   beqz  $v0, 0x24 (join)
  //   nop
  // Block 2 (0x20 - then_body):
  //   sw    $zero, 4($a0)
  // Block 3 (0x24 - join):
  //   jr    $ra
  //   nop
  std::vector<uint32_t> words = {
      0x94820000,  // 0x00: lhu   $v0, 0($a0)
      0x30420004,  // 0x04: andi  $v0, $v0, 4
      0x14400005,  // 0x08: bnez  $v0, +5 -> 0x20
      0x00000000,  // 0x0C: nop
      0x94820002,  // 0x10: lhu   $v0, 2($a0)
      0x30420008,  // 0x14: andi  $v0, $v0, 8
      0x10400002,  // 0x18: beqz  $v0, +2 -> 0x24
      0x00000000,  // 0x1C: nop
      0xAC800004,  // 0x20: sw    $zero, 4($a0)
      0x03E00008,  // 0x24: jr    $ra
      0x00000000,  // 0x28: nop
  };

  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80000000));
  DominatorTree dom = DominatorTree::Compute(cfg);
  DominatorTree pdom = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dom);
  auto region = ControlFlowStructurer::Structure(cfg, dom, pdom, loops);
  ASSERT_THAT(region, NotNull());

  AstConverterOptions opts;
  opts.function_name = "ShortCircuitOrBitmask";
  opts.return_type = CType::Void();
  auto func_or = AstConverter::Convert(cfg, *region, nullptr, opts);
  ASSERT_TRUE(func_or.ok()) << func_or.status();
  std::string code = func_or->ToString();
  EXPECT_THAT(code, HasSubstr("(((*(u16*)arg0 & 4) != 0) || ((*(u16*)(arg0 + 2) & 8) != 0))"));
  EXPECT_THAT(code, Not(HasSubstr("v0 =")));
  EXPECT_THAT(code, Not(HasSubstr("goto")));
}

}  // namespace
}  // namespace rom_nom_nom
