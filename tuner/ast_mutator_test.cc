#include "tuner/ast_mutator.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "core/c_ast.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(AstMutatorTest, CommutesBinaryAndRelationalExpressions) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::Return(
      CExpression::Binary("+", CExpression::Identifier("a"), CExpression::Identifier("b"))));

  FunctionDeclaration func(CType::S32(), "Add", {{CType::S32(), "a"}, {CType::S32(), "b"}},
                           std::move(body));

  auto steps = AstMutator::DiscoverMutations(func);
  ASSERT_EQ(steps.size(), 1u);
  EXPECT_EQ(steps[0].kind(), AST_MUTATION_COMMUTE_BINARY_OP);
  EXPECT_EQ(steps[0].site_index(), 0u);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, steps[0]);
  EXPECT_TRUE(absl::StrContains(mutated.ToString(), "return (b + a);"));
}

TEST(AstMutatorTest, InvertsIfElseAndNegatesCondition) {
  auto then_block = std::make_unique<CompoundStatement>();
  then_block->AddStatement(CStatement::Return(CExpression::Integer(1)));
  auto else_block = std::make_unique<CompoundStatement>();
  else_block->AddStatement(CStatement::Return(CExpression::Integer(2)));

  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("x"), CExpression::Integer(0)),
      std::move(then_block), std::move(else_block)));

  FunctionDeclaration func(CType::S32(), "Check", {{CType::S32(), "x"}}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_INVERT_IF_ELSE);
  step.set_site_index(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(code, "if ((x != 0))")) << code;
  size_t pos_ret2 = code.find("return 2;");
  size_t pos_ret1 = code.find("return 1;");
  ASSERT_NE(pos_ret2, std::string::npos);
  ASSERT_NE(pos_ret1, std::string::npos);
  EXPECT_LT(pos_ret2, pos_ret1);
}

TEST(AstMutatorTest, ReordersIndependentLocalDeclarations) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1"));
  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  FunctionDeclaration func(CType::S32(), "Locals", {}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_REORDER_LOCAL_DECLS);
  step.set_site_index(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  size_t pos_v1 = code.find("s32 v1;");
  size_t pos_v0 = code.find("s32 v0;");
  ASSERT_NE(pos_v1, std::string::npos);
  ASSERT_NE(pos_v0, std::string::npos);
  EXPECT_LT(pos_v1, pos_v0);
}

TEST(AstMutatorTest, TogglesCompoundAssignmentAndInlinesSingleUseTemp) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("t0"),
      CExpression::Binary("+", CExpression::Identifier("a0"), CExpression::Integer(4)))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("t0")));

  FunctionDeclaration func(CType::S32(), "TempInline", {{CType::S32(), "a0"}}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  step.set_site_index(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  EXPECT_FALSE(absl::StrContains(code, "t0 ="));
  EXPECT_TRUE(absl::StrContains(code, "return (a0 + 4);")) << code;
}

TEST(AstMutatorTest, LivenessAwareInlineAcrossOverwrittenTempAndHoist) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  // v0 = a0 + 1;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("a0"), CExpression::Integer(1)))));
  // v0 = v0 * 2; (single use of previous v0, and overwrites v0)
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("*", CExpression::Identifier("v0"), CExpression::Integer(2)))));
  // temp_s0 = v0; (consumes first v0 chain)
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("v0"))));
  // v0 = a1; (overwrites v0 again later in block)
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Identifier("a1"))));
  body->AddStatement(CStatement::Return(
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Identifier("v0"))));

  FunctionDeclaration func(CType::S32(), "ChainInline",
                           {{CType::S32(), "a0"}, {CType::S32(), "a1"}}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  step.set_site_index(0);
  step.set_param(1);  // inline all

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  EXPECT_FALSE(absl::StrContains(code, "v0 =")) << code;
  EXPECT_FALSE(absl::StrContains(code, "temp_s0 =")) << code;
  EXPECT_TRUE(absl::StrContains(code, "return (((a0 + 1) * 2) + a1);")) << code;
}

TEST(AstMutatorTest, DirectTypedGlobalAndArrayPromotions) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32().MakePointer(), "temp_s1"));
  // temp_s1 = &D_800B0000;
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s1"),
                              CExpression::Unary("&", CExpression::Identifier("D_800B0000")))));
  // *(s32 *)(temp_s1 + 8) = *(s16 *)&D_800A0000;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(),
                                 CExpression::Binary("+", CExpression::Identifier("temp_s1"),
                                                     CExpression::Integer(8)))),
      CExpression::Unary("*", CExpression::Cast(CType::S16().MakePointer(),
                                                CExpression::Unary("&", CExpression::Identifier(
                                                                            "D_800A0000")))))));

  FunctionDeclaration func(CType::Void(), "Globals", {}, std::move(body));

  AstMutationStep direct_step;
  direct_step.set_kind(AST_MUTATION_DIRECT_TYPED_GLOBAL);
  direct_step.set_site_index(0);
  FunctionDeclaration after_direct = AstMutator::ApplyMutation(func, direct_step);
  EXPECT_TRUE(absl::StrContains(after_direct.ToString(), "= D_800A0000;"))
      << after_direct.ToString();

  AstMutationStep array_step;
  array_step.set_kind(AST_MUTATION_GLOBAL_ARRAY_ACCESS);
  array_step.set_site_index(0);
  FunctionDeclaration after_array = AstMutator::ApplyMutation(after_direct, array_step);
  EXPECT_TRUE(absl::StrContains(after_array.ToString(), "D_800B0000[2] = D_800A0000;"))
      << after_array.ToString();

  ReferencedSymbols refs = CollectReferencedSymbols(after_array);
  ASSERT_TRUE(refs.inferred_data_types.count("D_800A0000") > 0);
  EXPECT_EQ(refs.inferred_data_types.at("D_800A0000").ToString(), "s16");
  ASSERT_TRUE(refs.inferred_data_types.count("D_800B0000") > 0);
  EXPECT_EQ(refs.inferred_data_types.at("D_800B0000").ToString(), "s32");
  EXPECT_TRUE(refs.array_data_symbols.count("D_800B0000") > 0);
}

TEST(AstMutatorTest, PointerOffsetToArrayIndexAndCoalesceParamCopyAndDeadCallReturn) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  // temp_s0 = arg0;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));
  // v0 = Helper(temp_s0); (dead return value)
  std::vector<std::unique_ptr<CExpression>> args;
  args.push_back(CExpression::Identifier("temp_s0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Call(CExpression::Identifier("Helper"), std::move(args)))));
  // *(s32 *)(temp_s0 + 8) = 1;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(),
                                 CExpression::Binary("+", CExpression::Identifier("temp_s0"),
                                                     CExpression::Integer(8)))),
      CExpression::Integer(1))));

  FunctionDeclaration func(CType::Void(), "Mutations", {{CType::S32(), "arg0"}}, std::move(body));

  AstMutationStep coalesce_step;
  coalesce_step.set_kind(AST_MUTATION_COALESCE_PARAM_COPY);
  coalesce_step.set_site_index(0);
  FunctionDeclaration m1 = AstMutator::ApplyMutation(func, coalesce_step);
  EXPECT_FALSE(absl::StrContains(m1.ToString(), "temp_s0")) << m1.ToString();

  AstMutationStep dead_call_step;
  dead_call_step.set_kind(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN);
  dead_call_step.set_site_index(0);
  dead_call_step.set_param(1);
  FunctionDeclaration m2 = AstMutator::ApplyMutation(m1, dead_call_step);
  EXPECT_FALSE(absl::StrContains(m2.ToString(), "v0 = Helper")) << m2.ToString();
  EXPECT_TRUE(absl::StrContains(m2.ToString(), "Helper(arg0);")) << m2.ToString();

  AstMutationStep ptr_idx_step;
  ptr_idx_step.set_kind(AST_MUTATION_POINTER_OFFSET_TO_ARRAY_INDEX);
  ptr_idx_step.set_site_index(0);
  FunctionDeclaration m3 = AstMutator::ApplyMutation(m2, ptr_idx_step);
  EXPECT_TRUE(absl::StrContains(m3.ToString(), "((s32*)arg0)[2] = 1;")) << m3.ToString();
}

TEST(AstMutatorTest, InlineInsideInfiniteLoopAndFuncPtrCastCallee) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", CExpression::Integer(0)));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  // v0 = 10; (overwritten at top of while(1) loop)
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(10))));

  auto if_body = std::make_unique<CompoundStatement>();
  if_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("g_idle_callback"))));
  if_body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("((void (*)())temp_s0)"), {})));

  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"), CExpression::Identifier("g_idle_callback"))));
  loop_body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("v0"), CExpression::Integer(0)),
      std::move(if_body)));

  body->AddStatement(CStatement::While(CExpression::Integer(1), std::move(loop_body)));

  FunctionDeclaration func(CType::Void(), "idle", {}, std::move(body));
  auto presets = AstMutator::GenerateCanonicalPresets(func);
  ASSERT_FALSE(presets.empty());
  FunctionDeclaration tuned = AstMutator::ApplyMutations(func, presets[0]);
  std::string code = tuned.ToString();
  EXPECT_FALSE(absl::StrContains(code, "v0")) << code;
  EXPECT_FALSE(absl::StrContains(code, "temp_s0")) << code;
  EXPECT_TRUE(absl::StrContains(code, "if ((g_idle_callback != 0))")) << code;
  EXPECT_TRUE(absl::StrContains(code, "((void (*)())g_idle_callback)();")) << code;
}

TEST(AstMutatorTest, MultiStatementInlineTempSinkAndHoistAndHazardRejection) {
  // temp_t6 = arg0 + 1;
  // g_other = 99;        (independent intervening statement)
  // g_dest = temp_t6;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t6"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t6"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("g_other"), CExpression::Integer(99))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_dest"), CExpression::Identifier("temp_t6"))));

  FunctionDeclaration func(CType::Void(), "CrossInline", {{CType::S32(), "arg0"}}, std::move(body));

  // param = 0: sink definition down to use site (`g_other = 99;` precedes `g_dest = (arg0 + 1);`)
  AstMutationStep sink_step;
  sink_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  sink_step.set_site_index(0);
  sink_step.set_param(0);
  std::string sunk = AstMutator::ApplyMutation(func, sink_step).ToString();
  size_t sunk_other_pos = sunk.find("g_other = 99;");
  size_t sunk_dest_pos = sunk.find("g_dest = (arg0 + 1);");
  ASSERT_NE(sunk_other_pos, std::string::npos) << sunk;
  ASSERT_NE(sunk_dest_pos, std::string::npos) << sunk;
  EXPECT_LT(sunk_other_pos, sunk_dest_pos) << sunk;

  // param = 2: hoist use statement up to definition site (`g_dest = (arg0 + 1);` precedes `g_other
  // = 99;`)
  AstMutationStep hoist_step;
  hoist_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  hoist_step.set_site_index(0);
  hoist_step.set_param(2);
  std::string hoisted = AstMutator::ApplyMutation(func, hoist_step).ToString();
  size_t hoisted_other_pos = hoisted.find("g_other = 99;");
  size_t hoisted_dest_pos = hoisted.find("g_dest = (arg0 + 1);");
  ASSERT_NE(hoisted_other_pos, std::string::npos) << hoisted;
  ASSERT_NE(hoisted_dest_pos, std::string::npos) << hoisted;
  EXPECT_LT(hoisted_dest_pos, hoisted_other_pos) << hoisted;

  // If the intervening statement modifies `arg0` AND reads `g_dest`, neither sink nor hoist is
  // valid.
  auto hazard_body = std::make_unique<CompoundStatement>();
  hazard_body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t6"));
  hazard_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t6"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  hazard_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"), CExpression::Identifier("g_dest"))));
  hazard_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_dest"), CExpression::Identifier("temp_t6"))));
  FunctionDeclaration hazard_func(CType::Void(), "HazardInline", {{CType::S32(), "arg0"}},
                                  std::move(hazard_body));
  std::string hazard_out = AstMutator::ApplyMutation(hazard_func, sink_step).ToString();
  EXPECT_TRUE(absl::StrContains(hazard_out, "temp_t6 = (arg0 + 1);")) << hazard_out;
}

TEST(AstMutatorTest, SwapAdjacentStatementsAndGlobalPromotionsAndDescribeStep) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32().MakePointer(), "temp_s1"));
  // Statement 0: D_80010000 = 0; (zero-only assigned global)
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_80010000"), CExpression::Integer(0))));
  // Statement 1: temp_s1 = &D_80020000;
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s1"),
                              CExpression::Unary("&", CExpression::Identifier("D_80020000")))));
  // Statement 2: *(s32 *)(temp_s1 + 8) = 7;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(),
                                 CExpression::Binary("+", CExpression::Identifier("temp_s1"),
                                                     CExpression::Integer(8)))),
      CExpression::Integer(7))));
  // Conflicting widths on D_80030000 (`s16` and `s8`) should NOT be promoted by DirectTypedGlobal
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S16().MakePointer(),
                                 CExpression::Unary("&", CExpression::Identifier("D_80030000")))),
      CExpression::Integer(1))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S8().MakePointer(),
                                 CExpression::Unary("&", CExpression::Identifier("D_80030000")))),
      CExpression::Integer(2))));

  FunctionDeclaration func(CType::Void(), "SwapAndGlobalTest", {}, std::move(body));

  // Swap statement 0 and statement 1 (independent)
  AstMutationStep swap_step;
  swap_step.set_kind(AST_MUTATION_SWAP_ADJACENT_STATEMENTS);
  swap_step.set_site_index(0);
  std::string swapped = AstMutator::ApplyMutation(func, swap_step).ToString();
  EXPECT_LT(swapped.find("temp_s1 = &D_80020000;"), swapped.find("D_80010000 = 0;")) << swapped;

  // Conflicting widths on D_80030000 are rejected by AST_MUTATION_DIRECT_TYPED_GLOBAL
  AstMutationStep direct_step;
  direct_step.set_kind(AST_MUTATION_DIRECT_TYPED_GLOBAL);
  direct_step.set_param(1);
  std::string direct_out = AstMutator::ApplyMutation(func, direct_step).ToString();
  EXPECT_TRUE(absl::StrContains(direct_out, "*(s16*)&D_80030000 = 1;")) << direct_out;

  // Smart global array access (param = 2) skips zero-only D_80010000 and folds temp_s1 into
  // D_80020000[2]
  AstMutationStep smart_arr_step;
  smart_arr_step.set_kind(AST_MUTATION_GLOBAL_ARRAY_ACCESS);
  smart_arr_step.set_param(2);
  std::string smart_arr_out = AstMutator::ApplyMutation(func, smart_arr_step).ToString();
  EXPECT_TRUE(absl::StrContains(smart_arr_out, "D_80010000 = 0;")) << smart_arr_out;
  EXPECT_TRUE(absl::StrContains(smart_arr_out, "D_80020000[2] = 7;")) << smart_arr_out;
  EXPECT_FALSE(absl::StrContains(smart_arr_out, "temp_s1")) << smart_arr_out;

  EXPECT_EQ(AstMutator::DescribeStep(swap_step), "SwapAdjacentStatements(site=0)");
  EXPECT_EQ(AstMutator::DescribeStep(direct_step), "DirectTypedGlobal(site=0, param=1)");
  EXPECT_EQ(AstMutator::DescribeStep(smart_arr_step), "GlobalArrayAccess(site=0, param=2)");
}

TEST(AstMutatorTest, CoalesceParameterCopyPreservesPreCopyUsesAndPrunesDeadInitializer) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", CExpression::Integer(0)));
  // Pre-copy use of temp_s0 as a temporary before `temp_s0 = arg1;`
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Unary(
          "*", CExpression::Cast(CType::U8().MakePointer(), CExpression::Identifier("arg1"))))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_val"), CExpression::Identifier("temp_s0"))));
  // Now copy parameter `arg1` into `temp_s0` and increment it
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("temp_s0")));

  FunctionDeclaration function(CType::S32(), "PreCopyTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  AstMutationStep coalesce_step;
  coalesce_step.set_kind(AST_MUTATION_COALESCE_PARAM_COPY);
  coalesce_step.set_param(1);
  FunctionDeclaration coalesced = AstMutator::ApplyMutation(function, coalesce_step);
  std::string code = coalesced.ToString();
  // Pre-copy assignment to temp_s0 must NOT be overwritten with `arg1 = *(u8*)arg1;`
  EXPECT_TRUE(absl::StrContains(code, "temp_s0 = *(u8*)arg1;")) << code;
  EXPECT_TRUE(absl::StrContains(code, "arg1 = (arg1 + 1);")) << code;
  EXPECT_TRUE(absl::StrContains(code, "return arg1;")) << code;

  // Bulk inline (param = 1) should inline the pre-copy temp_s0 and strip `s32 temp_s0 = 0;`
  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string inlined = AstMutator::ApplyMutation(coalesced, inline_step).ToString();
  EXPECT_TRUE(absl::StrContains(inlined, "g_val = *(u8*)arg1;")) << inlined;
  EXPECT_FALSE(absl::StrContains(inlined, "temp_s0")) << inlined;
}

TEST(AstMutatorTest, ReadModifyWriteCompoundAssignmentAndScaledGlobalArrayIndexing) {
  // v0 = ((((u32)arg0 >> 5) << 2) + &D_8016FAFC);
  // *(s32 *)v0 = (*(s32 *)v0 | (1 << (arg0 & 31)));
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  auto index_expression =
      CExpression::Binary(">>", CExpression::Cast(CType::U32(), CExpression::Identifier("arg0")),
                          CExpression::Integer(5));
  auto scaled_address = CExpression::Binary(
      "+", CExpression::Binary("<<", std::move(index_expression), CExpression::Integer(2)),
      CExpression::Unary("&", CExpression::Identifier("D_8016FAFC")));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), std::move(scaled_address))));
  auto dereference_v0 = CExpression::Unary(
      "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("v0")));
  auto dereference_v0_lhs = dereference_v0->Clone();
  auto mask_expression = CExpression::Binary(
      "<<", CExpression::Integer(1),
      CExpression::Binary("&", CExpression::Identifier("arg0"), CExpression::Integer(31)));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", std::move(dereference_v0_lhs),
      CExpression::Binary("|", std::move(dereference_v0), std::move(mask_expression)))));

  FunctionDeclaration function(CType::Void(), "SetBit", {{CType::S32(), "arg0"}}, std::move(body));

  auto presets = AstMutator::GenerateCanonicalPresets(function);
  ASSERT_GE(presets.size(), 3u);
  FunctionDeclaration mutated = AstMutator::ApplyMutations(function, presets[2]);
  std::string code = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(code, "D_8016FAFC[((u32)arg0 >> 5)] |= (1 << (arg0 & 31));"))
      << code;
  EXPECT_FALSE(absl::StrContains(code, "v0")) << code;
}

TEST(AstMutatorTest, ShiftAddMultiplyAndLoopPreheaderBackedgeNormalization) {
  // temp_s0 = 0;
  // arg0 = (0 & 255);
  // do {
  //   v0 = (arg0 << 1);
  //   v0 = (v0 + arg0);
  //   v0 = (v0 << 2);
  //   v0 = (v0 - arg0);
  //   v1 = (v0 << 2);
  //   temp_s0 = (temp_s0 + 1);
  //   if ((v1 != 0)) {
  //     Foo(arg0);
  //   }
  //   v0 = (temp_s0 & 255);
  //   v0 = ((u32)v0 < 10);
  //   arg0 = (temp_s0 & 255);
  // } while ((v0 != 0));
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1", CExpression::Integer(0)));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s0"), CExpression::Integer(0))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary("&", CExpression::Integer(0), CExpression::Integer(255)))));

  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("<<", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Identifier("arg0")))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("<<", CExpression::Identifier("v0"), CExpression::Integer(2)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("-", CExpression::Identifier("v0"), CExpression::Identifier("arg0")))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("<<", CExpression::Identifier("v0"), CExpression::Integer(2)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));

  auto then_body = std::make_unique<CompoundStatement>();
  std::vector<std::unique_ptr<CExpression>> call_arguments;
  call_arguments.push_back(CExpression::Identifier("arg0"));
  then_body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("Foo"), std::move(call_arguments))));
  loop_body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("v1"), CExpression::Integer(0)),
      std::move(then_body)));

  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("&", CExpression::Identifier("temp_s0"), CExpression::Integer(255)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("<", CExpression::Cast(CType::U32(), CExpression::Identifier("v0")),
                          CExpression::Integer(10)))));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary("&", CExpression::Identifier("temp_s0"), CExpression::Integer(255)))));

  body->AddStatement(CStatement::DoWhile(
      std::move(loop_body),
      CExpression::Binary("!=", CExpression::Identifier("v0"), CExpression::Integer(0))));

  FunctionDeclaration function(CType::Void(), "LoopMultiplyTest", {}, std::move(body));

  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string output = AstMutator::ApplyMutation(function, inline_step).ToString();
  EXPECT_FALSE(absl::StrContains(output, "arg0 = (0 & 255);")) << output;
  EXPECT_TRUE(absl::StrContains(output, "arg0 * 44")) << output;
  EXPECT_LT(output.find("Foo(arg0);"), output.find("temp_s0 = (temp_s0 + 1);")) << output;
}

TEST(AstMutatorTest, InlinesTemporaryInsideIfBranchWhenOverwrittenAfterOuterIfStatement) {
  // if (arg0 != 0) {
  //   v0 = arg0 + 5;
  //   g_result = v0;
  // }
  // if (arg1 != 0) {
  //   v0 = 10;
  // } else {
  //   v0 = 20;
  // }
  // return v0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0", CExpression::Integer(0)));

  auto first_then = std::make_unique<CompoundStatement>();
  first_then->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(5)))));
  first_then->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_result"), CExpression::Identifier("v0"))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(first_then)));

  auto second_then = std::make_unique<CompoundStatement>();
  second_then->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(10))));
  auto second_else = std::make_unique<CompoundStatement>();
  second_else->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(20))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(second_then), std::move(second_else)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration function(CType::S32(), "NestedIfOverwriteTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string output = AstMutator::ApplyMutation(function, inline_step).ToString();
  EXPECT_TRUE(absl::StrContains(output, "g_result = (arg0 + 5);")) << output;
  // The `= 0` initializer on `s32 v0` is also stripped since `v0` is overwritten in both branches
  // of the second `if` before `return v0;`
  EXPECT_TRUE(absl::StrContains(output, "s32 v0;")) << output;
  EXPECT_FALSE(absl::StrContains(output, "s32 v0 = 0;")) << output;
}

TEST(AstMutatorTest, InfiniteWhileLoopPreheaderAndBackedgeRollIntoDoWhileLoop) {
  // v1 = *(u8 *)arg0;
  // while (1) {
  //   g_last = v1;
  //   arg0 = (arg0 + 1);
  //   if ((v0 == 0)) {
  //     break;
  //   }
  //   v1 = *(u8 *)arg0;
  // }
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1"));
  auto load_byte = [&]() {
    return CExpression::Unary(
        "*", CExpression::Cast(CType::U8().MakePointer(), CExpression::Identifier("arg0")));
  };
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v1"), load_byte())));

  auto while_body = std::make_unique<CompoundStatement>();
  while_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_last"), CExpression::Identifier("v1"))));
  while_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(1)))));
  auto break_then = std::make_unique<CompoundStatement>();
  break_then->AddStatement(CStatement::Break());
  while_body->AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("v0"), CExpression::Integer(0)),
      std::move(break_then)));
  while_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v1"), load_byte())));

  body->AddStatement(CStatement::While(CExpression::Integer(1), std::move(while_body)));

  FunctionDeclaration function(CType::Void(), "StringScanTest", {{CType::S32(), "arg0"}},
                               std::move(body));

  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string output = AstMutator::ApplyMutation(function, inline_step).ToString();
  EXPECT_TRUE(absl::StrContains(output, "do {")) << output;
  EXPECT_TRUE(absl::StrContains(output, "} while ((v0 != 0));")) << output;
  EXPECT_FALSE(absl::StrContains(output, "while (1)")) << output;
}

TEST(AstMutatorTest, BranchDelaySlotSinksIntoAndAcrossIfStatement) {
  // Case A (sink into if):
  //   v0 = (arg0 + 4);
  //   if ((arg1 != 0)) {
  //     g_sink = v0;
  //   }
  // Case B (sink across if when !prefer_hoist):
  //   temp_s0 = (temp_s0 + 1);
  //   if ((arg1 == 0)) {
  //     g_other = 1;
  //   }
  //   return temp_s0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));

  // Case A
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(4)))));
  auto then_a = std::make_unique<CompoundStatement>();
  then_a->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_sink"), CExpression::Identifier("v0"))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(then_a)));

  // Case B
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));
  auto then_b = std::make_unique<CompoundStatement>();
  then_b->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("g_other"), CExpression::Integer(1))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(then_b)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("temp_s0")));

  FunctionDeclaration function(CType::S32(), "DelaySlotSinkTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  AstMutationStep inline_step;
  inline_step.set_kind(AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  inline_step.set_param(1);
  std::string output = AstMutator::ApplyMutation(function, inline_step).ToString();
  // Case A: v0 is sunk into `then_a` and inlined into `g_sink = (arg0 + 4);`
  EXPECT_TRUE(absl::StrContains(output, "g_sink = (arg0 + 4);")) << output;
  EXPECT_FALSE(absl::StrContains(output, "v0")) << output;
  // Case B: `temp_s0 = (temp_s0 + 1);` is sunk after `if ((arg1 == 0))` and inlined into `return`
  EXPECT_LT(output.find("g_other = 1;"), output.find("return")) << output;
}

TEST(AstMutatorTest, SwapAdjacentStatementsRejectsIndirectMemoryReadsInAssignment) {
  // Statement 0: *(s32*)arg0 = 42;
  // Statement 1: v0 = *(s32*)arg0;
  // Should NOT be swappable!
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("arg0"))),
      CExpression::Integer(42))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("arg0"))))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration function(CType::S32(), "StoreLoadSwapTest",
                               {{CType::S32().MakePointer(), "arg0"}}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_SWAP_ADJACENT_STATEMENTS);
  }
}

TEST(AstMutatorTest, DeadAssignmentPreservesLiveVariableAssignedInInnerBlock) {
  // if (arg0 != 0) {
  //   v0 = (arg1 + 7);
  // }
  // return v0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));

  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("arg1"), CExpression::Integer(7)))));

  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(then_branch)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration function(CType::S32(), "InnerBlockAssignTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN);
  }
}

TEST(AstMutatorTest, CoalesceParamCopyRejectsReassignedParameter) {
  // temp_s0 = arg0;
  // arg0 = 0;
  // return temp_s0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("arg0"), CExpression::Integer(0))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("temp_s0")));

  FunctionDeclaration function(CType::S32(), "ReassignedParamTest", {{CType::S32(), "arg0"}},
                               std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_COALESCE_PARAM_COPY);
  }
}

TEST(AstMutatorTest, CoalesceParamCopyReplacesInsideNestedBlocks) {
  // temp_s0 = arg0;
  // if (arg1 != 0) {
  //   return temp_s0;
  // }
  // return 0;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));

  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_dest"), CExpression::Identifier("temp_s0"))));

  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg1"), CExpression::Integer(0)),
      std::move(then_branch)));
  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  FunctionDeclaration function(CType::S32(), "NestedParamCoalesceTest",
                               {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  bool found_coalesce = false;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_COALESCE_PARAM_COPY) {
      found_coalesce = true;
      FunctionDeclaration mutated = AstMutator::ApplyMutation(function, step);
      std::string code = mutated.ToString();
      EXPECT_FALSE(absl::StrContains(code, "temp_s0"));
      EXPECT_TRUE(absl::StrContains(code, "g_dest = arg0;"));
    }
  }
  EXPECT_TRUE(found_coalesce);
}

TEST(AstMutatorTest, AddressOfInliningRejectsBinaryPointerArithmetic) {
  // temp = &D_80145450;
  // return (arg0 + temp);
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp"));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp"),
                              CExpression::Unary("&", CExpression::Identifier("D_80145450")))));
  body->AddStatement(CStatement::Return(
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Identifier("temp"))));

  FunctionDeclaration function(CType::S32(), "AddressOfArithmeticTest", {{CType::S32(), "arg0"}},
                               std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  }
}

TEST(AstMutatorTest, AddressTakenVariableRejectsInlining) {
  // temp_at = 0x43380000;
  // *(f32 *)(temp_s1 + 68) = *(f32 *)&temp_at;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_at"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_at"), CExpression::Integer(0x43380000))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::F32().MakePointer(),
                                 CExpression::Binary("+", CExpression::Identifier("temp_s1"),
                                                     CExpression::Integer(68)))),
      CExpression::Unary(
          "*", CExpression::Cast(CType::F32().MakePointer(),
                                 CExpression::Unary("&", CExpression::Identifier("temp_at")))))));

  FunctionDeclaration function(CType::Void(), "AddressTakenInlineTest", {{CType::S32(), "temp_s1"}},
                               std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  }
}

TEST(AstMutatorTest, CompoundUnaryOpAddressTakenVariableRejectsInlining) {
  // temp_at = 0x43380000;
  // f0 = *(f32*)&temp_at;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_at"));
  body->AddStatement(CStatement::VariableDeclaration(CType::F32(), "f0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_at"), CExpression::Integer(0x43380000))));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("f0"),
                              CExpression::Unary("*(f32*)&", CExpression::Identifier("temp_at")))));

  FunctionDeclaration function(CType::Void(), "CompoundUnaryOpInlineTest", {}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(function);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_INLINE_SINGLE_USE_TEMP);
  }
}

TEST(AstMutatorTest, PruneUnusedLocalDeclarationsPreservesTrailingReturn) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "unused_var"));
  body->AddStatement(CStatement::Return());

  // Default preserve_trailing_return = true retains trailing `return;`
  auto pruned_preserve =
      AstMutator::PruneUnusedLocalDeclarations(*body, /*preserve_trailing_return=*/true);
  EXPECT_TRUE(absl::StrContains(pruned_preserve->ToString(), "return;"));
  EXPECT_FALSE(absl::StrContains(pruned_preserve->ToString(), "unused_var"));

  // preserve_trailing_return = false removes trailing `return;`
  auto pruned_strip =
      AstMutator::PruneUnusedLocalDeclarations(*body, /*preserve_trailing_return=*/false);
  EXPECT_FALSE(absl::StrContains(pruned_strip->ToString(), "return;"));
}

TEST(AstMutatorTest, ToggleTrailingReturnDiscoveryAndApplication) {
  // 1. Function without trailing return statement (kNone)
  auto body1 = std::make_unique<CompoundStatement>();
  body1->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body1->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(42))));
  FunctionDeclaration func1(CType::Void(), "Func1", {}, std::move(body1));

  auto mutations1 = AstMutator::DiscoverMutations(func1);
  bool found_param0 = false;
  bool found_param1 = false;
  bool found_param2 = false;
  for (const auto& step : mutations1) {
    if (step.kind() == AST_MUTATION_TOGGLE_TRAILING_RETURN) {
      if (step.param() == 0) found_param0 = true;
      if (step.param() == 1) found_param1 = true;
      if (step.param() == 2) found_param2 = true;
    }
  }
  EXPECT_TRUE(found_param0);
  EXPECT_TRUE(found_param1);
  EXPECT_TRUE(found_param2);

  // Apply param 0: return;
  AstMutationStep step0;
  step0.set_kind(AST_MUTATION_TOGGLE_TRAILING_RETURN);
  step0.set_param(0);
  FunctionDeclaration mut0 = AstMutator::ApplyMutation(func1, step0);
  EXPECT_TRUE(absl::StrContains(mut0.ToString(), "return;\n}"));

  // Apply param 1: return 0;
  AstMutationStep step1;
  step1.set_kind(AST_MUTATION_TOGGLE_TRAILING_RETURN);
  step1.set_param(1);
  FunctionDeclaration mut1 = AstMutator::ApplyMutation(func1, step1);
  EXPECT_TRUE(absl::StrContains(mut1.ToString(), "return 0;\n}"));

  // Apply param 2: return v0;
  AstMutationStep step2;
  step2.set_kind(AST_MUTATION_TOGGLE_TRAILING_RETURN);
  step2.set_param(2);
  FunctionDeclaration mut2 = AstMutator::ApplyMutation(func1, step2);
  EXPECT_TRUE(absl::StrContains(mut2.ToString(), "return v0;\n}"));

  // 2. Discover from function with trailing `return;`
  auto mutations_from_void = AstMutator::DiscoverMutations(mut0);
  bool void_found_p0 = false;
  bool void_found_p1 = false;
  bool void_found_p2 = false;
  for (const auto& step : mutations_from_void) {
    if (step.kind() == AST_MUTATION_TOGGLE_TRAILING_RETURN) {
      if (step.param() == 0) void_found_p0 = true;
      if (step.param() == 1) void_found_p1 = true;
      if (step.param() == 2) void_found_p2 = true;
    }
  }
  EXPECT_FALSE(void_found_p0);
  EXPECT_TRUE(void_found_p1);
  EXPECT_TRUE(void_found_p2);

  // 3. Discover from function with trailing `return 0;`
  auto mutations_from_zero = AstMutator::DiscoverMutations(mut1);
  bool zero_found_p0 = false;
  bool zero_found_p1 = false;
  bool zero_found_p2 = false;
  for (const auto& step : mutations_from_zero) {
    if (step.kind() == AST_MUTATION_TOGGLE_TRAILING_RETURN) {
      if (step.param() == 0) zero_found_p0 = true;
      if (step.param() == 1) zero_found_p1 = true;
      if (step.param() == 2) zero_found_p2 = true;
    }
  }
  EXPECT_TRUE(zero_found_p0);
  EXPECT_FALSE(zero_found_p1);
  EXPECT_TRUE(zero_found_p2);

  // 4. Discover from function with trailing `return v0;`
  auto mutations_from_v0 = AstMutator::DiscoverMutations(mut2);
  bool v0_found_p0 = false;
  bool v0_found_p1 = false;
  bool v0_found_p2 = false;
  for (const auto& step : mutations_from_v0) {
    if (step.kind() == AST_MUTATION_TOGGLE_TRAILING_RETURN) {
      if (step.param() == 0) v0_found_p0 = true;
      if (step.param() == 1) v0_found_p1 = true;
      if (step.param() == 2) v0_found_p2 = true;
    }
  }
  EXPECT_TRUE(v0_found_p0);
  EXPECT_TRUE(v0_found_p1);
  EXPECT_FALSE(v0_found_p2);
}

TEST(AstMutatorTest, SplitSingleUseTempDirectPointerStore) {
  // void StoreFunc(s32* arg0, s32 arg1) {
  //   s32 v0;
  //   s32 live_var;
  //   *(s32*)arg0 = 42;
  //   live_var = 100;
  //   return live_var;
  // }
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "live_var"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("live_var"), CExpression::Integer(100))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("arg0")),
          /*is_prefix=*/true),
      CExpression::Integer(42))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("live_var")));

  FunctionDeclaration func(CType::S32(), "StoreFunc",
                           {{CType::S32().MakePointer(), "arg0"}, {CType::S32(), "arg1"}},
                           std::move(body));

  auto mutations = AstMutator::DiscoverMutations(func);
  bool found_arg1 = false;
  bool found_v0 = false;
  AstMutationStep step_arg1;
  AstMutationStep step_v0;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_SPLIT_SINGLE_USE_TEMP) {
      if (step.param() == 0) {
        found_arg1 = true;
        step_arg1 = step;
      } else if (step.param() == 1) {
        found_v0 = true;
        step_v0 = step;
      }
    }
  }
  ASSERT_TRUE(found_arg1);
  ASSERT_TRUE(found_v0);

  // param 0 reuses dead argument arg1
  FunctionDeclaration mut_arg1 = AstMutator::ApplyMutation(func, step_arg1);
  std::string code_arg1 = mut_arg1.ToString();
  EXPECT_TRUE(absl::StrContains(code_arg1, "arg1 = arg0;")) << code_arg1;
  EXPECT_TRUE(absl::StrContains(code_arg1, "*(s32*)arg1 = 42;")) << code_arg1;

  // param 1 reuses dead local v0
  FunctionDeclaration mut_v0 = AstMutator::ApplyMutation(func, step_v0);
  std::string code_v0 = mut_v0.ToString();
  EXPECT_TRUE(absl::StrContains(code_v0, "v0 = arg0;")) << code_v0;
  EXPECT_TRUE(absl::StrContains(code_v0, "*(s32*)v0 = 42;")) << code_v0;

  // live_var was NOT chosen in either mutation because it is live after the store
  EXPECT_FALSE(absl::StrContains(code_arg1, "live_var = arg0;"));
  EXPECT_FALSE(absl::StrContains(code_v0, "live_var = arg0;"));
}

TEST(AstMutatorTest, SplitSingleUseTempComplexPtrExpressionAndWithoutCast) {
  // *(s32*)(arg0 + 4) = 99;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                               CExpression::Integer(4))),
                         /*is_prefix=*/true),
      CExpression::Integer(99))));

  FunctionDeclaration func(CType::Void(), "ComplexStoreFunc",
                           {{CType::S32().MakePointer(), "arg0"}}, std::move(body));

  AstMutationStep step;
  step.set_kind(AST_MUTATION_SPLIT_SINGLE_USE_TEMP);
  step.set_site_index(0);
  step.set_param(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, step);
  std::string code = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(code, "v0 = (arg0 + 4);")) << code;
  EXPECT_TRUE(absl::StrContains(code, "*(s32*)v0 = 99;")) << code;
}

TEST(AstMutatorTest, VolatileGlobalAccessDiscoveryAndRewriting) {
  // D_800E9BA0 = 1;
  // D_800E9BA0 = 2;
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(1))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(2))));

  FunctionDeclaration func(CType::Void(), "VolatileStoreFunc", {}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(func);
  bool found_decl_single = false;
  bool found_decl_bulk = false;
  bool found_cast_single = false;
  bool found_cast_bulk = false;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_VOLATILE_GLOBAL_ACCESS) {
      if (step.param() == 0) found_decl_single = true;
      if (step.param() == 1) found_decl_bulk = true;
      if (step.param() == 2) found_cast_single = true;
      if (step.param() == 3) found_cast_bulk = true;
    }
  }
  EXPECT_TRUE(found_decl_single);
  EXPECT_TRUE(found_decl_bulk);
  EXPECT_TRUE(found_cast_single);
  EXPECT_TRUE(found_cast_bulk);

  // Test direct volatile declaration (param=0)
  AstMutationStep decl_step;
  decl_step.set_kind(AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  decl_step.set_site_index(0);
  decl_step.set_param(0);
  FunctionDeclaration decl_mutated = AstMutator::ApplyMutation(func, decl_step);
  const auto& first_stmt =
      static_cast<const ExpressionStatement&>(*decl_mutated.Body().Statements()[0]);
  const auto& assign = static_cast<const AssignmentExpression&>(first_stmt.Expression());
  const auto& id = static_cast<const IdentifierExpression&>(assign.Lhs());
  EXPECT_TRUE(id.SymbolType().has_value());
  EXPECT_EQ(id.SymbolType()->ToString(), "volatile s32");

  // Test inline cast (param=2)
  AstMutationStep cast_step;
  cast_step.set_kind(AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  cast_step.set_site_index(0);
  cast_step.set_param(2);
  FunctionDeclaration cast_mutated = AstMutator::ApplyMutation(func, cast_step);
  std::string code = cast_mutated.ToString();
  EXPECT_TRUE(absl::StrContains(code, "*(volatile s32*)&D_800E9BA0 = 1;")) << code;
  EXPECT_TRUE(absl::StrContains(code, "*(volatile s32*)&D_800E9BA0 = 2;")) << code;
}

TEST(AstMutatorTest, VolatileGlobalAccessRejectsInterveningCallOrRead) {
  // With call intervening:
  // D_800E9BA0 = 1;
  // Helper();
  // D_800E9BA0 = 2;
  auto body1 = std::make_unique<CompoundStatement>();
  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(1))));
  body1->AddStatement(
      CStatement::Expression(CExpression::Call(CExpression::Identifier("Helper"), {})));
  body1->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(2))));

  FunctionDeclaration func1(CType::Void(), "InterveningCallFunc", {}, std::move(body1));
  auto mutations1 = AstMutator::DiscoverMutations(func1);
  for (const auto& step : mutations1) {
    EXPECT_NE(step.kind(), AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  }

  // With read intervening:
  // D_800E9BA0 = 1;
  // x = D_800E9BA0;
  // D_800E9BA0 = 2;
  auto body2 = std::make_unique<CompoundStatement>();
  body2->AddStatement(CStatement::VariableDeclaration(CType::S32(), "x"));
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(1))));
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("x"), CExpression::Identifier("D_800E9BA0"))));
  body2->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Integer(2))));

  FunctionDeclaration func2(CType::Void(), "InterveningReadFunc", {}, std::move(body2));
  auto mutations2 = AstMutator::DiscoverMutations(func2);
  for (const auto& step : mutations2) {
    EXPECT_NE(step.kind(), AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  }
}

TEST(AstMutatorTest, DescribeStepCoversNewMutations) {
  AstMutationStep ret_step;
  ret_step.set_kind(AST_MUTATION_TOGGLE_TRAILING_RETURN);
  ret_step.set_site_index(0);
  ret_step.set_param(1);
  EXPECT_EQ(AstMutator::DescribeStep(ret_step), "ToggleTrailingReturn(site=0, param=1)");

  AstMutationStep split_step;
  split_step.set_kind(AST_MUTATION_SPLIT_SINGLE_USE_TEMP);
  split_step.set_site_index(2);
  split_step.set_param(0);
  EXPECT_EQ(AstMutator::DescribeStep(split_step), "SplitSingleUseTemp(site=2, param=0)");

  AstMutationStep vol_step;
  vol_step.set_kind(AST_MUTATION_VOLATILE_GLOBAL_ACCESS);
  vol_step.set_site_index(0);
  vol_step.set_param(1);
  EXPECT_EQ(AstMutator::DescribeStep(vol_step), "VolatileGlobalAccess(site=0, param=1)");
}

TEST(AstMutatorTest, VolatileGlobalAccessOgreBattleFunc800737A0) {
  // void func_800737A0(void)
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"), CExpression::Identifier("D_800E9BA0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"), CExpression::Integer(0xE7000000))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(8)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(16)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"), CExpression::Integer(0xDB060000))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(8))),
                         true),
      CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(24)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(32)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType::S32().MakePointer(), CExpression::Identifier("v0")), true),
      CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(16))),
                         true),
      CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"), CExpression::Integer(0xDE000000))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Unary("&", CExpression::Identifier("D_80186E90"), true))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(28))),
                         true),
      CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(40)))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("D_800E9BA0"), CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Unary("&", CExpression::Identifier("D_801869E8"), true))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(4))),
                         true),
      CExpression::Integer(0))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(12))),
                         true),
      CExpression::Integer(0))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(20))),
                         true),
      CExpression::Integer(0))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(24))),
                         true),
      CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(32))),
                         true),
      CExpression::Identifier("arg0"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::S32().MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("v0"),
                                                               CExpression::Integer(36))),
                         true),
      CExpression::Identifier("v1"))));
  body->AddStatement(CStatement::Return());

  FunctionDeclaration func(CType::Void(), "func_800737A0", {}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(func, 64);
  bool found_volatile = false;
  AstMutationStep volatile_step;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_VOLATILE_GLOBAL_ACCESS) {
      found_volatile = true;
      volatile_step = step;
      break;
    }
  }
  EXPECT_TRUE(found_volatile);
  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, volatile_step);
  std::string output = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(output, "D_800E9BA0 = v1;"));
}

TEST(AstMutatorTest, PropagatesRegisterCopyAndPrunesDeadInit) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1", CExpression::Integer(0)));
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v1"), CExpression::Identifier("v0"))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v1")));

  FunctionDeclaration func(CType::S32(), "CopyFunc", {}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(func, 64);
  EXPECT_FALSE(mutations.empty());

  // Apply discovered mutations (both dead init pruning and register copy propagation)
  FunctionDeclaration mutated = AstMutator::ApplyMutations(func, mutations);
  std::string output = mutated.ToString();
  EXPECT_FALSE(absl::StrContains(output, "v1")) << output;
  EXPECT_TRUE(absl::StrContains(output, "return v0;")) << output;
}

TEST(AstMutatorTest, SinksSelfMaskAndFoldsCast) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "x"));
  // arg0 = (arg0 & 65535);
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary("&", CExpression::Identifier("arg0"), CExpression::Integer(65535)))));
  // x = 1;
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("x"), CExpression::Integer(1))));
  // if (arg0 < 16) { return x; }
  auto if_body = std::make_unique<CompoundStatement>();
  if_body->AddStatement(CStatement::Return(CExpression::Identifier("x")));
  body->AddStatement(CStatement::If(
      CExpression::Binary("<", CExpression::Identifier("arg0"), CExpression::Integer(16)),
      std::move(if_body)));
  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  FunctionDeclaration func(CType::S32(), "MaskFunc", {{CType::S32(), "arg0"}}, std::move(body));

  AstMutationStep sink_step;
  sink_step.set_kind(AST_MUTATION_PROPAGATE_REGISTER_COPY);
  sink_step.set_site_index(0);
  sink_step.set_param(0);  // sink statement

  FunctionDeclaration mutated_sink = AstMutator::ApplyMutation(func, sink_step);
  std::string output_sink = mutated_sink.ToString();
  size_t pos_x = output_sink.find("x = 1;");
  size_t pos_mask = output_sink.find("arg0 = (arg0 & 65535);");
  ASSERT_NE(pos_x, std::string::npos);
  ASSERT_NE(pos_mask, std::string::npos);
  EXPECT_LT(pos_x, pos_mask);

  AstMutationStep fold_step;
  fold_step.set_kind(AST_MUTATION_PROPAGATE_REGISTER_COPY);
  fold_step.set_site_index(0);
  fold_step.set_param(1);  // fold cast into condition

  FunctionDeclaration mutated_fold = AstMutator::ApplyMutation(func, fold_step);
  std::string output_fold = mutated_fold.ToString();
  EXPECT_TRUE(absl::StrContains(output_fold, "if (((u16)arg0 < 16))")) << output_fold;
}

TEST(AstMutatorTest, HoistsWhileGuardAndInvertsLoop) {
  auto body = std::make_unique<CompoundStatement>();
  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("i"),
      CExpression::Binary("+", CExpression::Identifier("i"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::While(
      CExpression::Binary("<", CExpression::Identifier("i"), CExpression::Integer(10)),
      std::move(loop_body)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("i")));

  FunctionDeclaration func(CType::S32(), "WhileFunc", {{CType::S32(), "i"}}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(func, 64);
  bool found_hoist = false;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_HOIST_WHILE_GUARD) {
      found_hoist = true;
      break;
    }
  }
  EXPECT_TRUE(found_hoist);

  // param 0: guarded do-while
  AstMutationStep guard_step;
  guard_step.set_kind(AST_MUTATION_HOIST_WHILE_GUARD);
  guard_step.set_site_index(0);
  guard_step.set_param(0);

  FunctionDeclaration guarded = AstMutator::ApplyMutation(func, guard_step);
  std::string output_guarded = guarded.ToString();
  EXPECT_TRUE(absl::StrContains(output_guarded, "if ((i < 10)) {")) << output_guarded;
  EXPECT_TRUE(absl::StrContains(output_guarded, "do {")) << output_guarded;

  // and reverse: guarded do-while -> while
  auto reverse_mutations = AstMutator::DiscoverMutations(guarded, 64);
  bool found_reverse = false;
  AstMutationStep reverse_step;
  for (const auto& step : reverse_mutations) {
    if (step.kind() == AST_MUTATION_HOIST_WHILE_GUARD) {
      found_reverse = true;
      reverse_step = step;
      break;
    }
  }
  EXPECT_TRUE(found_reverse);
  FunctionDeclaration restored = AstMutator::ApplyMutation(guarded, reverse_step);
  std::string output_restored = restored.ToString();
  EXPECT_TRUE(absl::StrContains(output_restored, "while ((i < 10)) {")) << output_restored;
}

TEST(AstMutatorTest, HoistsWhileBreakIfToGoardedDoWhile) {
  auto body = std::make_unique<CompoundStatement>();
  auto loop_body = std::make_unique<CompoundStatement>();
  auto break_body = std::make_unique<CompoundStatement>();
  break_body->AddStatement(CStatement::Break());
  loop_body->AddStatement(CStatement::If(
      CExpression::Binary("==", CExpression::Identifier("i"), CExpression::Integer(10)),
      std::move(break_body)));
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("i"),
      CExpression::Binary("+", CExpression::Identifier("i"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::While(CExpression::Integer(1), std::move(loop_body)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("i")));

  FunctionDeclaration func(CType::S32(), "BreakFunc", {{CType::S32(), "i"}}, std::move(body));

  AstMutationStep hoist_step;
  hoist_step.set_kind(AST_MUTATION_HOIST_WHILE_GUARD);
  hoist_step.set_site_index(0);
  hoist_step.set_param(0);  // while (!cond) { body; }

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, hoist_step);
  std::string output = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(output, "while ((i != 10)) {")) << output;
  EXPECT_FALSE(absl::StrContains(output, "break;")) << output;
}

TEST(AstMutatorTest, ReloadOrEarlyReturnVarMemoryStoreAndZeroSubstitution) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1", CExpression::Integer(0)));
  // *(s8 *)(arg0 + 213) = v0;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType::Named("s8").MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                               CExpression::Integer(213)))),
      CExpression::Identifier("v0"))));
  // v0 = *(u8 *)(arg0 + 213);
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Unary("*",
                         CExpression::Cast(CType::Named("u8").MakePointer(),
                                           CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                               CExpression::Integer(213)))))));
  // return 0;
  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  FunctionDeclaration func(CType::S32(), "ReloadFunc", {{CType::S32(), "arg0"}}, std::move(body));

  auto mutations = AstMutator::DiscoverMutations(func, 64);
  bool found_reload = false;
  for (const auto& step : mutations) {
    if (step.kind() == AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR) {
      found_reload = true;
      break;
    }
  }
  EXPECT_TRUE(found_reload);

  // Apply volatile store & reload mutation (site 0)
  AstMutationStep reload_step;
  reload_step.set_kind(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR);
  reload_step.set_site_index(0);
  reload_step.set_param(0);

  FunctionDeclaration mutated = AstMutator::ApplyMutation(func, reload_step);
  std::string output = mutated.ToString();
  EXPECT_TRUE(absl::StrContains(output, "volatile s8*")) << output;
  EXPECT_TRUE(absl::StrContains(output, "volatile u8*")) << output;

  // Apply zero-return variable substitution (site 1)
  AstMutationStep ret_step;
  ret_step.set_kind(AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR);
  ret_step.set_site_index(1);
  ret_step.set_param(0);

  FunctionDeclaration ret_mutated = AstMutator::ApplyMutation(func, ret_step);
  std::string ret_output = ret_mutated.ToString();
  EXPECT_TRUE(absl::StrContains(ret_output, "return v1;")) << ret_output;
}

TEST(AstMutatorTest, PropagateRegisterCopySoundnessGuardsRejectLoopsAndMemoryAliasing) {
  // Case 1: func_8009A9C0 pattern:
  // arg0 = v0;
  // followed by a loop where arg0 and v0 are assigned!
  {
    auto body = std::make_unique<CompoundStatement>();
    body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
    body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1"));
    body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg0"));
    body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));

    // arg0 = v0;
    body->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("arg0"), CExpression::Identifier("v0"))));

    // do { v0 = 1; arg0 = v0; } while (v0 != v1);
    auto loop_body = std::make_unique<CompoundStatement>();
    loop_body->AddStatement(CStatement::Expression(
        CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(1))));
    loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("arg0"), CExpression::Identifier("v0"))));
    body->AddStatement(CStatement::DoWhile(
        std::move(loop_body),
        CExpression::Binary("!=", CExpression::Identifier("v0"), CExpression::Identifier("v1"))));

    FunctionDeclaration func(CType::Void(), "UnsoundLoopCopyFunc", {}, std::move(body));
    auto mutations = AstMutator::DiscoverMutations(func, 64);
    for (const auto& step : mutations) {
      EXPECT_NE(step.kind(), AST_MUTATION_PROPAGATE_REGISTER_COPY)
          << "PropagateRegisterCopy must NOT propagate into or across a loop where registers are "
             "assigned!";
    }
  }

  // Case 2: func_8006E840 pattern:
  // temp_s0 = arg0;
  // followed by if (cond) { arg0 = 0; }
  {
    auto body = std::make_unique<CompoundStatement>();
    body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg0"));
    body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));
    body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "cond"));

    // temp_s0 = arg0;
    body->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));

    // if (cond) { arg0 = 0; }
    auto if_body = std::make_unique<CompoundStatement>();
    if_body->AddStatement(CStatement::Expression(
        CExpression::Assignment("=", CExpression::Identifier("arg0"), CExpression::Integer(0))));
    body->AddStatement(CStatement::If(CExpression::Identifier("cond"), std::move(if_body)));

    FunctionDeclaration func(CType::Void(), "UnsoundIfOverwriteFunc", {}, std::move(body));
    auto mutations = AstMutator::DiscoverMutations(func, 64);
    for (const auto& step : mutations) {
      EXPECT_NE(step.kind(), AST_MUTATION_PROPAGATE_REGISTER_COPY)
          << "PropagateRegisterCopy must NOT propagate across an if statement where the source is "
             "overwritten!";
    }
  }

  // Case 3: Self-mask sinking across memory write or function call:
  // x = (x & 65535);
  // *ptr = 1; (memory write)
  // if (x < 16) ...
  {
    auto body = std::make_unique<CompoundStatement>();
    body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "x"));
    body->AddStatement(CStatement::VariableDeclaration(CType::S32().MakePointer(), "ptr"));

    // x = (x & 65535);
    body->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("x"),
        CExpression::Binary("&", CExpression::Identifier("x"), CExpression::Integer(65535)))));

    // *ptr = 1;
    body->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Unary("*", CExpression::Identifier("ptr")), CExpression::Integer(1))));

    // if (x < 16) { ... }
    auto if_body = std::make_unique<CompoundStatement>();
    if_body->AddStatement(CStatement::Return(CExpression::Integer(1)));
    body->AddStatement(CStatement::If(
        CExpression::Binary("<", CExpression::Identifier("x"), CExpression::Integer(16)),
        std::move(if_body)));

    FunctionDeclaration func(CType::Void(), "UnsoundSinkAcrossMemoryFunc", {}, std::move(body));
    auto mutations = AstMutator::DiscoverMutations(func, 64);
    for (const auto& step : mutations) {
      EXPECT_NE(step.kind(), AST_MUTATION_PROPAGATE_REGISTER_COPY)
          << "Self-mask sinking must NOT sink across an instruction that writes to memory!";
    }
  }
}

}  // namespace
}  // namespace rom_nom_nom
