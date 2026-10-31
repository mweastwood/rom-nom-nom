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

  // 4. Discover from void function with trailing `return v0;` (only param 0 `return;` is sound)
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
  EXPECT_FALSE(v0_found_p1);
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

TEST(AstMutatorTest, ZeroReturnVariableSubstitutionExcludesVariablesReassignedInBranches) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v1"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "arg0"));

  // v1 = 0;
  body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v1"), CExpression::Integer(0))));

  // if (arg0 != 0) { v1 = 5; }
  auto if_body = std::make_unique<CompoundStatement>();
  if_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v1"), CExpression::Integer(5))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(if_body)));

  // return 0;
  body->AddStatement(CStatement::Return(CExpression::Integer(0)));

  FunctionDeclaration func(CType::S32(), "ZeroVarReassignedInIfFunc", {}, std::move(body));
  auto mutations = AstMutator::DiscoverMutations(func, 64);
  for (const auto& step : mutations) {
    EXPECT_NE(step.kind(), AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR)
        << "v1 is reassigned inside an if-branch and must not be treated as a constant zero!";
  }
}

TEST(AstMutatorTest, DualCompilerInverseMutationsPinMergeSplitStoreFlagAndHoistLoopInvariant) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "v0", nullptr, std::nullopt, "$2"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t0_2"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_s0"));

  // 1. Generalized subexpression split target: v0 = ((arg0 * 4) + arg1);
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary(
          "+", CExpression::Binary("*", CExpression::Identifier("arg0"), CExpression::Integer(4)),
          CExpression::Identifier("arg1")))));

  // 2. Store-flag conditional toggle target: if (arg0 < 0) { temp_s0 = (temp_s0 + 1); }
  auto then_block = std::make_unique<CompoundStatement>();
  then_block->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::If(
      CExpression::Binary("<", CExpression::Identifier("arg0"), CExpression::Integer(0)),
      std::move(then_block)));

  // 3. Split web usage: temp_t0_2 = (temp_s0 + 1);
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t0_2"),
      CExpression::Binary("+", CExpression::Identifier("temp_s0"), CExpression::Integer(1)))));

  // 4. Loop with hoistable invariant condition constant 16:
  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::DoWhile(
      std::move(loop_body),
      CExpression::Binary("<", CExpression::Identifier("v0"), CExpression::Integer(16))));

  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration func(CType::S32(), "DualInverseFunc",
                           {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body));

  // A. Toggle register annotation on `v0` (remove `$2` with param=0, swap to `$3` with param=1)
  AstMutationStep unpin_step;
  unpin_step.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
  unpin_step.set_site_index(0);
  unpin_step.set_param(0);
  EXPECT_FALSE(
      absl::StrContains(AstMutator::ApplyMutation(func, unpin_step).ToString(), "asm(\"$2\")"));

  AstMutationStep swap_reg_step;
  swap_reg_step.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
  swap_reg_step.set_site_index(0);
  swap_reg_step.set_param(1);
  EXPECT_TRUE(
      absl::StrContains(AstMutator::ApplyMutation(func, swap_reg_step).ToString(), "asm(\"$3\")"));

  // B. Merge split scratch webs (`temp_t0_2` -> `temp_t0`)
  AstMutationStep merge_step;
  merge_step.set_kind(AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS);
  merge_step.set_site_index(0);
  merge_step.set_param(0);
  std::string merged_code = AstMutator::ApplyMutation(func, merge_step).ToString();
  EXPECT_FALSE(absl::StrContains(merged_code, "temp_t0_2")) << merged_code;
  EXPECT_TRUE(absl::StrContains(merged_code, "temp_t0 = (temp_s0 + 1);")) << merged_code;

  // C. Split general subexpression (`(arg0 * 4)` into dead variable `arg1` or `temp_t0`)
  AstMutationStep split_step;
  split_step.set_kind(AST_MUTATION_SPLIT_SINGLE_USE_TEMP);
  split_step.set_site_index(0);
  split_step.set_param(0);
  std::string split_code = AstMutator::ApplyMutation(func, split_step).ToString();
  EXPECT_TRUE(absl::StrContains(split_code, "temp_t0 = (arg0 * 4);")) << split_code;
  EXPECT_TRUE(absl::StrContains(split_code, "v0 = (temp_t0 + arg1);")) << split_code;

  // D. Toggle store-flag conditional (`if (arg0 < 0) { temp_s0 = (temp_s0 + 1); }` <->
  // `temp_s0 = (temp_s0 + (arg0 < 0));`)
  AstMutationStep sf_step;
  sf_step.set_kind(AST_MUTATION_TOGGLE_STORE_FLAG_CONDITIONAL);
  sf_step.set_site_index(0);
  sf_step.set_param(0);
  FunctionDeclaration sf_mutated = AstMutator::ApplyMutation(func, sf_step);
  EXPECT_TRUE(absl::StrContains(sf_mutated.ToString(), "temp_s0 = (temp_s0 + (arg0 < 0));"))
      << sf_mutated.ToString();
  FunctionDeclaration sf_restored = AstMutator::ApplyMutation(sf_mutated, sf_step);
  EXPECT_TRUE(absl::StrContains(sf_restored.ToString(), "if ((arg0 < 0)) {"))
      << sf_restored.ToString();

  // E. Hoist loop-invariant constant `16` out of `do { ... } while (v0 < 16);`
  AstMutationStep hoist_inv_step;
  hoist_inv_step.set_kind(AST_MUTATION_HOIST_LOOP_INVARIANT);
  hoist_inv_step.set_site_index(0);
  hoist_inv_step.set_param(0);
  std::string hoisted_code = AstMutator::ApplyMutation(func, hoist_inv_step).ToString();
  EXPECT_TRUE(absl::StrContains(hoisted_code, "= 16;")) << hoisted_code;
  EXPECT_FALSE(absl::StrContains(hoisted_code, "while ((v0 < 16));")) << hoisted_code;

  EXPECT_EQ(AstMutator::DescribeStep(unpin_step), "ToggleRegisterAnnotation(site=0, param=0)");
  EXPECT_EQ(AstMutator::DescribeStep(merge_step), "MergeSplitScratchWebs(site=0, param=0)");
  EXPECT_EQ(AstMutator::DescribeStep(sf_step), "ToggleStoreFlagConditional(site=0, param=0)");
  EXPECT_EQ(AstMutator::DescribeStep(hoist_inv_step), "HoistLoopInvariant(site=0, param=0)");
}

TEST(AstMutatorTest, ToggleRegisterAnnotationNeverCreatesDuplicateHardRegisterBindings) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "v0", nullptr, std::nullopt, "$2"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "v1", nullptr, std::nullopt, "$3"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_t0", nullptr, std::nullopt, "$8"));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  FunctionDeclaration func(CType::S32(), "NoDuplicateRegBindings", {}, std::move(body));

  // Neither v0 ($2 -> $3), v1 ($3 -> $2), nor temp_t0 ($8 -> $2) should be allowed to swap to an
  // already-bound hard register ($2 or $3).
  auto discovered = AstMutator::DiscoverMutations(func, /*max_mutations=*/64);
  for (const auto& step : discovered) {
    if (step.kind() == AST_MUTATION_TOGGLE_REGISTER_ANNOTATION) {
      EXPECT_NE(step.param(), 1u) << "Unexpected swap to already-bound register: "
                                  << AstMutator::DescribeStep(step);
    }
  }

  for (uint32_t site = 0; site < 3; ++site) {
    AstMutationStep colliding_swap;
    colliding_swap.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
    colliding_swap.set_site_index(site);
    colliding_swap.set_param(1);
    EXPECT_EQ(AstMutator::ApplyMutation(func, colliding_swap).ToString(), func.ToString());
  }
}

TEST(AstMutatorTest, SplitScratchWebsAndAddressOfCastsAndNonVoidReturnInvariants) {
  auto body = std::make_unique<CompoundStatement>();
  // Split scratch web `v0_2` must not be auto-pinned to `$2`.
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0_2"));
  // `(s32)&D_8016FFEC` must never have its `(s32)` cast stripped (`param = 0`), though flipping
  // `(s32)` <-> `(u32)` (`param = 1`) is allowed.
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0_2"),
      CExpression::Binary(
          "+", CExpression::Identifier("arg0"),
          CExpression::Cast(CType::S32(),
                            CExpression::Unary("&", CExpression::Identifier("D_8016FFEC"),
                                               /*is_prefix=*/true))))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("v0_2")));

  FunctionDeclaration func(CType::S32(), "TestInvariants", {{CType::S32(), "arg0"}},
                           std::move(body));

  auto discovered = AstMutator::DiscoverMutations(func, /*max_mutations=*/64);
  for (const auto& step : discovered) {
    EXPECT_NE(step.kind(), AST_MUTATION_TOGGLE_REGISTER_ANNOTATION)
        << "v0_2 should not have an inferred register annotation";
    EXPECT_NE(step.kind(), AST_MUTATION_TOGGLE_TRAILING_RETURN)
        << "Non-void function must not toggle trailing return";
    if (step.kind() == AST_MUTATION_TOGGLE_UNSIGNED_CAST) {
      EXPECT_EQ(step.param(), 1u) << "Address-of cast must not offer param=0 (strip cast)";
    }
  }

  // Applying param=0 on `(s32)&D_8016FFEC` must be a safe no-op.
  AstMutationStep strip_cast;
  strip_cast.set_kind(AST_MUTATION_TOGGLE_UNSIGNED_CAST);
  strip_cast.set_site_index(0);
  strip_cast.set_param(0);
  EXPECT_EQ(AstMutator::ApplyMutation(func, strip_cast).ToString(), func.ToString());

  // Applying param=1 on `(s32)&D_8016FFEC` flips it to `(u32)&D_8016FFEC`.
  AstMutationStep flip_cast;
  flip_cast.set_kind(AST_MUTATION_TOGGLE_UNSIGNED_CAST);
  flip_cast.set_site_index(0);
  flip_cast.set_param(1);
  EXPECT_TRUE(
      absl::StrContains(AstMutator::ApplyMutation(func, flip_cast).ToString(), "(u32)&D_8016FFEC"));
}

TEST(AstMutatorTest, DiscoverMutationsNeverStarvesHighPriorityFunctionMutationsUnderCap) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", nullptr, std::nullopt, "$16"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s1", nullptr, std::nullopt, "$17"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t0"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "temp_t0_2"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "v0"));

  // Parameter copy: temp_s0 = arg0;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));

  // 40 commutable / castable statements (80+ fine-grained expression mutations)
  for (int i = 0; i < 40; ++i) {
    body->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("v0"),
        CExpression::Binary("+",
                            CExpression::Cast(CType::S32(), CExpression::Identifier("temp_s0")),
                            CExpression::Identifier("arg1")))));
  }

  // Split scratch web usage + global access at the very bottom of the function
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t0_2"),
      CExpression::Binary("+", CExpression::Identifier("D_80100000"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::Return(CExpression::Identifier("temp_t0_2")));

  FunctionDeclaration func(CType::S32(), "LargeFunc",
                           {{CType::S32(), "arg0"}, {CType::S32(), "arg1"}}, std::move(body),
                           /*is_static=*/false, FunctionOptimizationMode::kO2Standard);

  auto capped = AstMutator::DiscoverMutations(func, /*max_mutations=*/24);
  EXPECT_EQ(capped.size(), 24u);

  bool has_coalesce_param = false;
  bool has_global_array = false;
  bool has_merge_webs = false;
  bool has_bulk_unpin = false;
  bool has_swap_reg_decls = false;
  for (const auto& step : capped) {
    if (step.kind() == AST_MUTATION_COALESCE_PARAM_COPY) has_coalesce_param = true;
    if (step.kind() == AST_MUTATION_GLOBAL_ARRAY_ACCESS) has_global_array = true;
    if (step.kind() == AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS) has_merge_webs = true;
    if (step.kind() == AST_MUTATION_TOGGLE_REGISTER_ANNOTATION && step.param() >= 2) {
      has_bulk_unpin = true;
    }
    if (step.kind() == AST_MUTATION_REORDER_LOCAL_DECLS && step.param() == 1) {
      has_swap_reg_decls = true;
    }
  }
  EXPECT_TRUE(has_coalesce_param);
  EXPECT_TRUE(has_global_array);
  EXPECT_TRUE(has_merge_webs);
  EXPECT_TRUE(has_bulk_unpin);
  EXPECT_TRUE(has_swap_reg_decls);
}

TEST(AstMutatorTest, ReorderLocalDeclsSwapsRegisterAnnotationsAndPreservesOptimizationMode) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", nullptr, std::nullopt, "$16"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s1", nullptr, std::nullopt, "$17"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_t0", nullptr, std::nullopt, "$8"));
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "fp", nullptr, std::nullopt, "$30"));
  body->AddStatement(CStatement::Return(CExpression::Binary("+", CExpression::Identifier("temp_s0"),
                                                            CExpression::Identifier("temp_s1"))));

  FunctionDeclaration func(CType::S32(), "RegColorFunc", {}, std::move(body),
                           /*is_static=*/false, FunctionOptimizationMode::kO2Standard);

  // 1. REORDER_LOCAL_DECLS(site=0, param=1) swaps asm("$16") and asm("$17") between temp_s0 and
  // temp_s1.
  AstMutationStep swap_regs;
  swap_regs.set_kind(AST_MUTATION_REORDER_LOCAL_DECLS);
  swap_regs.set_site_index(0);
  swap_regs.set_param(1);
  FunctionDeclaration swapped = AstMutator::ApplyMutation(func, swap_regs);
  EXPECT_EQ(swapped.OptimizationMode(), FunctionOptimizationMode::kO2Standard);
  EXPECT_TRUE(absl::StrContains(swapped.ToString(), "register s32 temp_s0 asm(\"$17\");"))
      << swapped.ToString();
  EXPECT_TRUE(absl::StrContains(swapped.ToString(), "register s32 temp_s1 asm(\"$16\");"))
      << swapped.ToString();

  // 2. REORDER_LOCAL_DECLS(site=0, param=2) unpins both temp_s0 and temp_s1 and swaps their
  // declaration order.
  AstMutationStep unpin_and_reorder;
  unpin_and_reorder.set_kind(AST_MUTATION_REORDER_LOCAL_DECLS);
  unpin_and_reorder.set_site_index(0);
  unpin_and_reorder.set_param(2);
  FunctionDeclaration reordered = AstMutator::ApplyMutation(func, unpin_and_reorder);
  EXPECT_EQ(reordered.OptimizationMode(), FunctionOptimizationMode::kO2Standard);
  std::string reordered_code = reordered.ToString();
  EXPECT_FALSE(absl::StrContains(reordered_code, "asm(\"$16\")")) << reordered_code;
  EXPECT_FALSE(absl::StrContains(reordered_code, "asm(\"$17\")")) << reordered_code;
  EXPECT_LT(reordered_code.find("s32 temp_s1;"), reordered_code.find("s32 temp_s0;"))
      << reordered_code;

  // 3. TOGGLE_REGISTER_ANNOTATION(site=0, param=2) unpins caller-saved scratch registers ($8)
  // while preserving callee-saved ($16, $17) and frame-pointer ($30).
  AstMutationStep unpin_scratch;
  unpin_scratch.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
  unpin_scratch.set_site_index(0);
  unpin_scratch.set_param(2);
  FunctionDeclaration scratch_unpinned = AstMutator::ApplyMutation(func, unpin_scratch);
  EXPECT_EQ(scratch_unpinned.OptimizationMode(), FunctionOptimizationMode::kO2Standard);
  EXPECT_FALSE(absl::StrContains(scratch_unpinned.ToString(), "asm(\"$8\")"));
  EXPECT_TRUE(absl::StrContains(scratch_unpinned.ToString(), "asm(\"$16\")"));
  EXPECT_TRUE(absl::StrContains(scratch_unpinned.ToString(), "asm(\"$30\")"));

  // 4. TOGGLE_REGISTER_ANNOTATION(site=0, param=3) unpins all non-$30 registers.
  AstMutationStep unpin_all;
  unpin_all.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
  unpin_all.set_site_index(0);
  unpin_all.set_param(3);
  FunctionDeclaration all_unpinned = AstMutator::ApplyMutation(func, unpin_all);
  EXPECT_EQ(all_unpinned.OptimizationMode(), FunctionOptimizationMode::kO2Standard);
  EXPECT_FALSE(absl::StrContains(all_unpinned.ToString(), "asm(\"$8\")"));
  EXPECT_FALSE(absl::StrContains(all_unpinned.ToString(), "asm(\"$16\")"));
  EXPECT_FALSE(absl::StrContains(all_unpinned.ToString(), "asm(\"$17\")"));
  EXPECT_TRUE(absl::StrContains(all_unpinned.ToString(), "asm(\"$30\")"));
}

TEST(AstMutatorTest, NonAdjacentRegisterPairSwapsDirectRePinAndCseBarrierSuppression) {
  auto body = std::make_unique<CompoundStatement>();
  // site 0: temp_t0 ($8)
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_t0", nullptr, std::nullopt, "$8"));
  // site 1: temp_s0 ($16) - sits between temp_t0 and temp_t2!
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_s0", nullptr, std::nullopt, "$16"));
  // site 2: temp_t2 ($10)
  body->AddStatement(
      CStatement::VariableDeclaration(CType::S32(), "temp_t2", nullptr, std::nullopt, "$10"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("D_80100000"))));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t0"), CExpression::Identifier("D_80100000"))));
  body->AddStatement(CStatement::Return(
      CExpression::Binary("+", CExpression::Identifier("temp_t0"),
                          CExpression::Binary("+", CExpression::Identifier("temp_s0"),
                                              CExpression::Identifier("temp_t2")))));

  FunctionDeclaration func(CType::S32(), "NonAdjFunc", {}, std::move(body),
                           /*is_static=*/false, FunctionOptimizationMode::kAuto);

  // 1. Non-adjacent pair swap between site 0 (temp_t0, $8) and site 2 (temp_t2, $10) via param=12.
  AstMutationStep swap_non_adj;
  swap_non_adj.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
  swap_non_adj.set_site_index(0);
  swap_non_adj.set_param(12);  // 10 + 2
  FunctionDeclaration swapped = AstMutator::ApplyMutation(func, swap_non_adj);
  EXPECT_EQ(swapped.OptimizationMode(), FunctionOptimizationMode::kO2Standard);
  EXPECT_TRUE(absl::StrContains(swapped.ToString(), "register s32 temp_t0 asm(\"$10\");"))
      << swapped.ToString();
  EXPECT_TRUE(absl::StrContains(swapped.ToString(), "register s32 temp_t2 asm(\"$8\");"))
      << swapped.ToString();

  // 2. Direct hardware-register re-pinning of site 0 (temp_t0) to $9 (`param = 109`).
  AstMutationStep repin_to_t1;
  repin_to_t1.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
  repin_to_t1.set_site_index(0);
  repin_to_t1.set_param(109);
  FunctionDeclaration repinned = AstMutator::ApplyMutation(func, repin_to_t1);
  EXPECT_TRUE(absl::StrContains(repinned.ToString(), "register s32 temp_t0 asm(\"$9\");"))
      << repinned.ToString();

  // 3. CSE barrier suppression (`param = 4` for whole function, `param = 5` for site 1 `temp_s0`).
  AstMutationStep suppress_var;
  suppress_var.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
  suppress_var.set_site_index(1);
  suppress_var.set_param(5);
  FunctionDeclaration var_suppressed = AstMutator::ApplyMutation(func, suppress_var);
  EXPECT_EQ(var_suppressed.SuppressedCseBarrierVars().count("temp_s0"), 1u);

  AstMutationStep suppress_all;
  suppress_all.set_kind(AST_MUTATION_TOGGLE_REGISTER_ANNOTATION);
  suppress_all.set_site_index(0);
  suppress_all.set_param(4);
  FunctionDeclaration all_suppressed = AstMutator::ApplyMutation(func, suppress_all);
  EXPECT_TRUE(all_suppressed.SuppressAllCseBarriers());

  // 4. Diff-guided mutation discovery promotes exact transposed register pair ($8, $9) to the
  // front of discovered mutations, while rejecting cross-save-class pairs like ($16, $5).
  DiffDiagnostic diag;
  diag.transposed_register_pairs.push_back({"$8", "$9"});
  diag.transposed_register_pairs.push_back({"$16", "$5"});
  diag.has_unfilled_delay_slot = true;
  auto guided = AstMutator::DiscoverMutations(func, /*max_mutations=*/16, &diag);
  ASSERT_FALSE(guided.empty());
  bool found_repin_t1 = false;
  bool found_cse_suppress = false;
  for (const auto& step : guided) {
    if (step.kind() == AST_MUTATION_TOGGLE_REGISTER_ANNOTATION && step.site_index() == 0 &&
        step.param() == 109) {
      found_repin_t1 = true;
    }
    if (step.kind() == AST_MUTATION_TOGGLE_REGISTER_ANNOTATION &&
        (step.param() == 4 || step.param() == 5)) {
      found_cse_suppress = true;
    }
    // Site 1 (temp_s0, $16) must never be re-pinned to a caller-saved or argument register.
    if (step.kind() == AST_MUTATION_TOGGLE_REGISTER_ANNOTATION && step.site_index() == 1) {
      EXPECT_NE(step.param(), 105u);
      EXPECT_NE(step.param(), 108u);
    }
  }
  EXPECT_TRUE(found_repin_t1);
  EXPECT_TRUE(found_cse_suppress);
}

}  // namespace
}  // namespace rom_nom_nom
