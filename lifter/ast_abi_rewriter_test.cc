#include "lifter/ast_abi_rewriter.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/ast_converter.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;
using ::testing::UnorderedElementsAre;

TEST(AstAbiRewriterTest, SimplifyReturnStatementsTerminalAssignmentFoldsIntoDirectReturn) {
  // v0 = (a + b); return v0; -> return (a + b);
  CompoundStatement comp;
  comp.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("a"), CExpression::Identifier("b")))));
  comp.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  SimplifyReturnStatements(&comp, /*returns_v0=*/true, /*returns_f0=*/false);

  ASSERT_EQ(comp.Statements().size(), 1u);
  EXPECT_EQ(comp.Statements()[0]->Kind(), CStatementKind::kReturnStatement);
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("return (a + b);"));
}

TEST(AstAbiRewriterTest, SimplifyReturnStatementsEarlyReturnRestructuring) {
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

  SimplifyReturnStatements(&comp, /*returns_v0=*/true, /*returns_f0=*/false);

  ASSERT_EQ(comp.Statements().size(), 2u);
  EXPECT_EQ(comp.Statements()[0]->Kind(), CStatementKind::kIfStatement);
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("if ((default_val == 0))"));
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("return other_val;"));
  EXPECT_EQ(comp.Statements()[1]->Kind(), CStatementKind::kReturnStatement);
  EXPECT_THAT(comp.Statements()[1]->ToString(), HasSubstr("return default_val;"));
}

TEST(AstAbiRewriterTest, SimplifyReturnStatementsBranchLikelyReturnInversion) {
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

  SimplifyReturnStatements(&comp, /*returns_v0=*/true, /*returns_f0=*/false);

  ASSERT_EQ(comp.Statements().size(), 2u);
  EXPECT_EQ(comp.Statements()[0]->Kind(), CStatementKind::kIfStatement);
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("if ((arg0 > arg1))"));
  EXPECT_THAT(comp.Statements()[0]->ToString(), HasSubstr("return arg0;"));
  EXPECT_EQ(comp.Statements()[1]->Kind(), CStatementKind::kReturnStatement);
  EXPECT_THAT(comp.Statements()[1]->ToString(), HasSubstr("return arg1;"));
}

TEST(AstAbiRewriterTest, FloatInGprAndStackCallArgumentsRewrittenWithCalleeSavedFprBinding) {
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

TEST(AstAbiRewriterTest, InlinesCrossJumpedCallTailsAndSimplifiesSignExtensionShiftPairs) {
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

TEST(AstAbiRewriterTest, TrimsTrailingUnassignedCallArgScratchRegistersAfterDivisionFolding) {
  std::string textproto = R"pb(
    entries { name: "ProcessPair" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // 00: addiu $sp, $sp, -24
  // 04: sw    $ra, 20($sp)
  // 08: lui   $v0, 0xB216
  // 0C: ori   $v0, $v0, 0x42C9
  // 10: mult  $a0, $v0
  // 14: mfhi  $a3
  // 18: addiu $a1, $a1, 1
  // 1C: addu  $a0, $a3, $a0
  // 20: sra   $a0, $a0, 7
  // 24: sra   $v1, $a0, 31
  // 28: jal   0x80001000
  // 2C: subu  $a0, $a0, $v1
  // 30: lw    $ra, 20($sp)
  // 34: jr    $ra
  // 38: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0x3C02B216,  // 08: lui   $v0, 0xB216
      0x344242C9,  // 0C: ori   $v0, $v0, 0x42C9
      0x00820018,  // 10: mult  $a0, $v0
      0x00003810,  // 14: mfhi  $a3
      0x24A50001,  // 18: addiu $a1, $a1, 1
      0x00E42021,  // 1C: addu  $a0, $a3, $a0
      0x000421C3,  // 20: sra   $a0, $a0, 7
      0x00041FC3,  // 24: sra   $v1, $a0, 31
      0x0C000400,  // 28: jal   0x80001000
      0x00832023,  // 2C: subu  $a0, $a0, $v1
      0x8FBF0014,  // 30: lw    $ra, 20($sp)
      0x03E00008,  // 34: jr    $ra
      0x27BD0018,  // 38: addiu $sp, $sp, 24
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestTrimTrailingScratchCallArgs";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("arg0 = (arg0 / 184);"));
  EXPECT_THAT(code, HasSubstr("ProcessPair(arg0, arg1);"));
  EXPECT_THAT(code, Not(HasSubstr("arg2")));
  EXPECT_THAT(code, Not(HasSubstr("arg3")));
}

TEST(AstAbiRewriterTest, TrimsUnassignedTrailingArgRegistersForKnownCalleeWithPrototypeCast) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "VarargsHelper" address: 0x80001000 type: SYMBOL_FUNC }
  )");
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0x0C000400,  // 08: jal   0x80001000 (VarargsHelper)
      0x24040007,  // 0C: addiu $a0, $zero, 7
      0x8FBF0014,  // 10: lw    $ra, 20($sp)
      0x03E00008,  // 14: jr    $ra
      0x27BD0018,  // 18: addiu $sp, $sp, 24
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  absl::flat_hash_map<std::string, int> param_counts = {{"VarargsHelper", 4}};
  AstConverterOptions options;
  options.function_name = "CallVarargsWithOneArg";
  options.function_parameter_counts = &param_counts;
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("((s32 (*)(s32))VarargsHelper)(7)"));
  EXPECT_THAT(code, ::testing::Not(HasSubstr("arg1")));
  EXPECT_THAT(code, ::testing::Not(HasSubstr("arg2")));
  EXPECT_THAT(code, ::testing::Not(HasSubstr("arg3")));
}

TEST(AstAbiRewriterTest, DoubleParameterInA2A3AndStackSlotsAndCallerRewriting) {
  // 1. Callee taking (s32 arg0, f64 arg2, f64 arg4) where slot 1 ($a1) is skipped O32 alignment
  // padding:
  //   00: mtc1  $a2, $f5
  //   04: mtc1  $a3, $f4
  //   08: ldc1  $f6, 16($sp)
  //   0C: add.d $f0, $f4, $f6
  //   10: jr    $ra
  //   14: nop
  std::vector<uint32_t> callee_words = {
      0x44862800,  // mtc1  $a2, $f5
      0x44872000,  // mtc1  $a3, $f4
      0xD7A60010,  // ldc1  $f6, 16($sp)
      0x46262000,  // add.d $f0, $f4, $f6
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  auto callee_cfg = *ControlFlowGraph::Build(*DecodeSequence(callee_words, 0x80001000));
  DominatorTree callee_dom = DominatorTree::Compute(callee_cfg);
  DominatorTree callee_pdom = DominatorTree::ComputePostDominators(callee_cfg);
  LoopInfo callee_loops = LoopInfo::Analyze(callee_cfg, callee_dom);
  auto callee_region =
      ControlFlowStructurer::Structure(callee_cfg, callee_dom, callee_pdom, callee_loops);
  ASSERT_THAT(callee_region, NotNull());

  AstConverterOptions callee_opts;
  callee_opts.function_name = "CalleeIntDoubleDouble";
  FunctionDeclaration callee_func =
      *AstConverter::Convert(callee_cfg, *callee_region, nullptr, callee_opts);
  std::string callee_code = callee_func.ToString();
  EXPECT_THAT(callee_code, HasSubstr("f64 CalleeIntDoubleDouble(s32 arg0, f64 arg2, f64 arg4)"));
  EXPECT_THAT(callee_code, HasSubstr("return (arg2 + arg4);"));

  // 2. Caller passing (s32, f64, f64) where f64 in ($a2, $a3) is passed via mfc1 $a2, $f5;
  // mfc1 $a3, $f4 and f64 at 16($sp) is passed via sdc1 $f6, 16($sp):
  //   00: addiu $sp, $sp, -32
  //   04: sw    $ra, 28($sp)
  //   08: ldc1  $f4, 0($a1)
  //   0C: ldc1  $f6, 8($a1)
  //   10: mfc1  $a2, $f5
  //   14: mfc1  $a3, $f4
  //   18: sdc1  $f6, 16($sp)
  //   1C: jal   0x80001000
  //   20: nop
  //   24: lw    $ra, 28($sp)
  //   28: jr    $ra
  //   2C: addiu $sp, $sp, 32
  std::vector<uint32_t> caller_words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xD4A40000,  // 08: ldc1  $f4, 0($a1)
      0xD4A60008,  // 0C: ldc1  $f6, 8($a1)
      0x44062800,  // 10: mfc1  $a2, $f5
      0x44072000,  // 14: mfc1  $a3, $f4
      0xF7A60010,  // 18: sdc1  $f6, 16($sp)
      0x0C000400,  // 1C: jal   0x80001000
      0x00000000,  // 20: nop
      0x8FBF001C,  // 24: lw    $ra, 28($sp)
      0x03E00008,  // 28: jr    $ra
      0x27BD0020,  // 2C: addiu $sp, $sp, 32
  };
  auto caller_cfg = *ControlFlowGraph::Build(*DecodeSequence(caller_words, 0x80002000));
  DominatorTree caller_dom = DominatorTree::Compute(caller_cfg);
  DominatorTree caller_pdom = DominatorTree::ComputePostDominators(caller_cfg);
  LoopInfo caller_loops = LoopInfo::Analyze(caller_cfg, caller_dom);
  auto caller_region =
      ControlFlowStructurer::Structure(caller_cfg, caller_dom, caller_pdom, caller_loops);
  ASSERT_THAT(caller_region, NotNull());

  AstConverterOptions caller_opts;
  caller_opts.function_name = "CallerIntDoubleDouble";
  FunctionDeclaration caller_func =
      *AstConverter::Convert(caller_cfg, *caller_region, nullptr, caller_opts);
  std::string caller_code = caller_func.ToString();
  EXPECT_THAT(caller_code, HasSubstr("*(f64*)arg1"));
  EXPECT_THAT(caller_code, HasSubstr("(s32, f64, f64))func_80001000)(arg0, "));
  EXPECT_THAT(caller_code, HasSubstr("*(f64*)(arg1 + 8))"));
}

TEST(AstAbiRewriterTest, GeneralizedCallArgumentCountInferenceAcrossPrologueAndPredecessor) {
  // 1. Entry block saves $a1 (k == 1) to $s1 before mtc1 $a2, $f20, modifies $a0, and calls
  // func_80001000(arg0 + 1, arg1):
  std::vector<uint32_t> entry_words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xAFB10018,  // 08: sw    $s1, 24($sp)
      0xE7B40014,  // 0C: swc1  $f20, 20($sp)
      0x00A08821,  // 10: addu  $s1, $a1, $zero  (save $a1 -> $s1)
      0x4486A000,  // 14: mtc1  $a2, $f20        (move $a2 -> $f20)
      0x24840001,  // 18: addiu $a0, $a0, 1      (define $a0)
      0x0C000400,  // 1C: jal   0x80001000       (should infer 2 args: $a0, $a1!)
      0x00000000,  // 20: nop
      0x4600A006,  // 24: mov.s $f0, $f20
      0xC7B40014,  // 28: lwc1  $f20, 20($sp)
      0x8FB10018,  // 2C: lw    $s1, 24($sp)
      0x8FBF001C,  // 30: lw    $ra, 28($sp)
      0x03E00008,  // 34: jr    $ra
      0x27BD0020,  // 38: addiu $sp, $sp, 32
  };
  auto entry_cfg = *ControlFlowGraph::Build(*DecodeSequence(entry_words, 0x80002000));
  DominatorTree entry_dom = DominatorTree::Compute(entry_cfg);
  DominatorTree entry_pdom = DominatorTree::ComputePostDominators(entry_cfg);
  LoopInfo entry_loops = LoopInfo::Analyze(entry_cfg, entry_dom);
  auto entry_region =
      ControlFlowStructurer::Structure(entry_cfg, entry_dom, entry_pdom, entry_loops);
  ASSERT_THAT(entry_region, NotNull());

  AstConverterOptions entry_opts;
  entry_opts.function_name = "EntrySecondArgSavedBeforeMtc1";
  auto entry_func = *AstConverter::Convert(entry_cfg, *entry_region, nullptr, entry_opts);
  std::string entry_code = entry_func.ToString();
  EXPECT_THAT(entry_code, HasSubstr("func_80001000(arg0, arg1)"));

  // 2. Predecessor block defines $a0 and $a1, tests $a1 with slti/beq before calling
  // func_80001000($a0, $a1) in the branch block:
  std::vector<uint32_t> pred_words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0010,  // 04: sw    $ra, 16($sp)
      0x2484000A,  // 08: addiu $a0, $a0, 10
      0x24A50004,  // 0C: addiu $a1, $a1, 4
      0x28A10001,  // 10: slti  $at, $a1, 1
      0x14200003,  // 14: bne   $at, $zero, +3 -> 0x24
      0x00000000,  // 18: nop
      0x0C000400,  // 1C: jal   0x80001000
      0x00000000,  // 20: nop
      0x8FBF0010,  // 24: lw    $ra, 16($sp)
      0x03E00008,  // 28: jr    $ra
      0x27BD0018,  // 2C: addiu $sp, $sp, 24
  };
  auto pred_cfg = *ControlFlowGraph::Build(*DecodeSequence(pred_words, 0x80002000));
  DominatorTree pred_dom = DominatorTree::Compute(pred_cfg);
  DominatorTree pred_pdom = DominatorTree::ComputePostDominators(pred_cfg);
  LoopInfo pred_loops = LoopInfo::Analyze(pred_cfg, pred_dom);
  auto pred_region = ControlFlowStructurer::Structure(pred_cfg, pred_dom, pred_pdom, pred_loops);
  ASSERT_THAT(pred_region, NotNull());

  AstConverterOptions pred_opts;
  pred_opts.function_name = "PredSecondArgBranchGuard";
  auto pred_func = *AstConverter::Convert(pred_cfg, *pred_region, nullptr, pred_opts);
  std::string pred_code = pred_func.ToString();
  EXPECT_THAT(pred_code, HasSubstr("func_80001000(arg0, arg1)"));
}

TEST(AstAbiRewriterTest, InlineCrossJumpedCallTailsPreservesBreakInSwitchCases) {
  CompoundStatement root;
  std::vector<SwitchCase> cases;

  // case 0: block_1: Dispatch(80, 104); break;
  SwitchCase c0;
  c0.case_values = {0};
  c0.body = std::make_unique<CompoundStatement>();
  c0.body->AddStatement(CStatement::Label("block_1"));
  std::vector<std::unique_ptr<CExpression>> args0;
  args0.push_back(CExpression::Integer(80));
  args0.push_back(CExpression::Integer(104));
  c0.body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("Dispatch"), std::move(args0))));
  c0.body->AddStatement(CStatement::Break());
  cases.push_back(std::move(c0));

  // case 2: goto block_1;
  SwitchCase c2;
  c2.case_values = {2};
  c2.body = std::make_unique<CompoundStatement>();
  c2.body->AddStatement(CStatement::Goto("block_1"));
  cases.push_back(std::move(c2));

  root.AddStatement(CStatement::Switch(CExpression::Identifier("sel"), std::move(cases)));
  InlineCrossJumpedCallTails(&root);

  const auto& sw = static_cast<const SwitchStatement&>(*root.Statements()[0]);
  ASSERT_EQ(sw.Cases().size(), 2u);
  ASSERT_EQ(sw.Cases()[1].body->Statements().size(), 2u);
  EXPECT_EQ(sw.Cases()[1].body->Statements()[1]->Kind(), CStatementKind::kBreakStatement);
}

TEST(AstAbiRewriterTest, SinkCommonBranchTailArgAssignmentsSinksDelaySlotDuplicatedSingleIf) {
  CompoundStatement body;
  // temp_s0 = (arg0 + 4);
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(4)))));
  // if (v0 != 0) { func_80001000(arg0); temp_s0 = (arg0 + 4); }
  auto then_branch = std::make_unique<CompoundStatement>();
  std::vector<std::unique_ptr<CExpression>> call_args;
  call_args.push_back(CExpression::Identifier("arg0"));
  then_branch->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("func_80001000"), std::move(call_args))));
  then_branch->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s0"),
      CExpression::Binary("+", CExpression::Identifier("arg0"), CExpression::Integer(4)))));
  body.AddStatement(CStatement::If(
      CExpression::Binary("!=", CExpression::Identifier("v0"), CExpression::Integer(0)),
      std::move(then_branch), nullptr));

  SinkCommonBranchTailArgAssignments(&body);
  ASSERT_EQ(body.Statements().size(), 2u);
  EXPECT_EQ(body.Statements()[0]->Kind(), CStatementKind::kIfStatement);
  EXPECT_EQ(body.Statements()[1]->ToString(), "temp_s0 = (arg0 + 4);\n");
}

TEST(AstAbiRewriterTest, SinkCommonBranchTailArgAssignmentsEliminatesCascadeMergeGotosAndLabels) {
  // Construct nested if cascade with trailing label `block_6:` inside innermost `then` and
  // `else { goto block_6; }` on outer `if`s:
  //   if (c1) {
  //     if (c2) {
  //       if (c3) {
  //         temp_a2 = 1;
  //       block_6:
  //       }
  //       v0 = (temp_a2 & 255);
  //     } else {
  //       goto block_6;
  //     }
  //   } else {
  //     goto block_6;
  //   }
  //   return v0;
  CompoundStatement body;

  auto inner_then = std::make_unique<CompoundStatement>();
  inner_then->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_a2"), CExpression::Integer(1))));
  inner_then->AddStatement(CStatement::Label("block_6"));

  auto mid_then = std::make_unique<CompoundStatement>();
  mid_then->AddStatement(
      CStatement::If(CExpression::Identifier("c3"), std::move(inner_then), nullptr));
  mid_then->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("&", CExpression::Identifier("temp_a2"), CExpression::Integer(255)))));

  auto mid_else = std::make_unique<CompoundStatement>();
  mid_else->AddStatement(CStatement::Goto("block_6"));

  auto outer_then = std::make_unique<CompoundStatement>();
  outer_then->AddStatement(
      CStatement::If(CExpression::Identifier("c2"), std::move(mid_then), std::move(mid_else)));

  auto outer_else = std::make_unique<CompoundStatement>();
  outer_else->AddStatement(CStatement::Goto("block_6"));

  body.AddStatement(
      CStatement::If(CExpression::Identifier("c1"), std::move(outer_then), std::move(outer_else)));
  body.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  SinkCommonBranchTailArgAssignments(&body);

  std::string emitted = body.ToString();
  EXPECT_THAT(emitted, Not(HasSubstr("goto block_6")));
  EXPECT_THAT(emitted, Not(HasSubstr("block_6:")));
  ASSERT_EQ(body.Statements().size(), 3u);
  EXPECT_EQ(body.Statements()[0]->Kind(), CStatementKind::kIfStatement);
  EXPECT_EQ(body.Statements()[1]->ToString(), "v0 = (temp_a2 & 255);\n");
  EXPECT_EQ(body.Statements()[2]->Kind(), CStatementKind::kReturnStatement);
}

TEST(AstAbiRewriterTest, SinkCommonBranchTailArgAssignmentsRewritesLoopExitAndContinueGotos) {
  // Construct a do-while loop where an inner branch jumps to `block_cont` at the end of the
  // loop body (`continue;`) and another branch jumps to `block_exit` immediately after the
  // loop (`break;`):
  //   do {
  //     if (skip_cond) {
  //       goto block_cont;
  //     }
  //     if (stop_cond) {
  //       goto block_exit;
  //     }
  //     v0 = (v0 + 1);
  //   block_cont:
  //   } while (loop_cond);
  // block_exit:
  //   return v0;
  CompoundStatement body;
  auto loop_body = std::make_unique<CompoundStatement>();

  auto skip_then = std::make_unique<CompoundStatement>();
  skip_then->AddStatement(CStatement::Goto("block_cont"));
  loop_body->AddStatement(
      CStatement::If(CExpression::Identifier("skip_cond"), std::move(skip_then), nullptr));

  auto stop_then = std::make_unique<CompoundStatement>();
  stop_then->AddStatement(CStatement::Goto("block_exit"));
  loop_body->AddStatement(
      CStatement::If(CExpression::Identifier("stop_cond"), std::move(stop_then), nullptr));

  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("+", CExpression::Identifier("v0"), CExpression::Integer(1)))));
  loop_body->AddStatement(CStatement::Label("block_cont"));

  body.AddStatement(
      CStatement::DoWhile(std::move(loop_body), CExpression::Identifier("loop_cond")));
  body.AddStatement(CStatement::Label("block_exit"));
  body.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  SinkCommonBranchTailArgAssignments(&body);

  std::string emitted = body.ToString();
  EXPECT_THAT(emitted, HasSubstr("continue;"));
  EXPECT_THAT(emitted, HasSubstr("break;"));
  EXPECT_THAT(emitted, Not(HasSubstr("goto block_cont")));
  EXPECT_THAT(emitted, Not(HasSubstr("goto block_exit")));
  EXPECT_THAT(emitted, Not(HasSubstr("block_cont:")));
  EXPECT_THAT(emitted, Not(HasSubstr("block_exit:")));
}

TEST(AstAbiRewriterTest,
     InlineCrossJumpedCallTailsMergesAliasSwitchCasesAndAppendsBreakOnNestedIfCallTail) {
  CompoundStatement root;
  std::vector<SwitchCase> cases;

  SwitchCase case0;
  case0.case_values = {0};
  case0.body = std::make_unique<CompoundStatement>();
  auto then0 = std::make_unique<CompoundStatement>();
  std::vector<std::unique_ptr<CExpression>> args_then0;
  args_then0.push_back(CExpression::Integer(0));
  args_then0.push_back(CExpression::Integer(0));
  then0->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("func_8002F3F0"), std::move(args_then0))));
  auto else0 = std::make_unique<CompoundStatement>();
  else0->AddStatement(CStatement::Label("block_20"));
  std::vector<std::unique_ptr<CExpression>> args_else0;
  args_else0.push_back(CExpression::Integer(0));
  args_else0.push_back(CExpression::Integer(429));
  else0->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("func_8002F3F0"), std::move(args_else0))));
  case0.body->AddStatement(
      CStatement::If(CExpression::Identifier("c0"), std::move(then0), std::move(else0)));
  case0.body->AddStatement(CStatement::Break());
  cases.push_back(std::move(case0));

  SwitchCase case2;
  case2.case_values = {2};
  case2.body = std::make_unique<CompoundStatement>();
  case2.body->AddStatement(CStatement::Label("block_17"));
  auto then2 = std::make_unique<CompoundStatement>();
  then2->AddStatement(CStatement::Break());
  case2.body->AddStatement(
      CStatement::If(CExpression::Identifier("c2"), std::move(then2), nullptr));
  case2.body->AddStatement(CStatement::Goto("block_20"));
  cases.push_back(std::move(case2));

  SwitchCase case3;
  case3.case_values = {3};
  case3.body = std::make_unique<CompoundStatement>();
  case3.body->AddStatement(CStatement::Goto("block_17"));
  case3.body->AddStatement(CStatement::Break());
  cases.push_back(std::move(case3));

  root.AddStatement(CStatement::Switch(CExpression::Identifier("sel"), std::move(cases)));
  root.AddStatement(CStatement::Return());

  InlineCrossJumpedCallTails(&root);

  std::string emitted = root.ToString();
  EXPECT_THAT(emitted, Not(HasSubstr("goto block_17")));
  EXPECT_THAT(emitted, Not(HasSubstr("goto block_20")));
  EXPECT_THAT(emitted, Not(HasSubstr("block_17:")));
  EXPECT_THAT(emitted, Not(HasSubstr("block_20:")));
  const auto& sw = static_cast<const SwitchStatement&>(*root.Statements()[0]);
  ASSERT_EQ(sw.Cases().size(), 3u);
  EXPECT_EQ(sw.Cases()[1].body->Statements().back()->Kind(), CStatementKind::kBreakStatement);
  EXPECT_EQ(sw.Cases()[2].body->Statements().back()->Kind(), CStatementKind::kBreakStatement);
}

TEST(AstAbiRewriterTest, InlineCrossJumpedCallTailsDuplicatesChainedSwitchCaseStoreTails) {
  CompoundStatement root;
  std::vector<SwitchCase> cases;

  SwitchCase case1;
  case1.case_values = {1};
  case1.body = std::make_unique<CompoundStatement>();
  case1.body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_season_name_char_0"), CExpression::Integer(179))));
  case1.body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(193))));
  case1.body->AddStatement(CStatement::Goto("block_12"));
  cases.push_back(std::move(case1));

  SwitchCase case2;
  case2.case_values = {2};
  case2.body = std::make_unique<CompoundStatement>();
  case2.body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_season_name_char_0"), CExpression::Integer(179))));
  case2.body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(199))));
  case2.body->AddStatement(CStatement::Goto("block_11"));
  cases.push_back(std::move(case2));

  SwitchCase case4;
  case4.case_values = {4};
  case4.body = std::make_unique<CompoundStatement>();
  case4.body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_season_name_char_0"), CExpression::Integer(183))));
  case4.body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(206))));
  case4.body->AddStatement(CStatement::Label("block_11"));
  case4.body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_season_name_char_3"), CExpression::Identifier("v0"))));
  case4.body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_season_name_char_4"), CExpression::Integer(191))));
  case4.body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("v0"), CExpression::Integer(204))));
  case4.body->AddStatement(CStatement::Label("block_12"));
  case4.body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("g_season_name_char_5"), CExpression::Identifier("v0"))));
  case4.body->AddStatement(CStatement::Break());
  cases.push_back(std::move(case4));

  root.AddStatement(CStatement::Switch(CExpression::Identifier("arg0"), std::move(cases)));
  root.AddStatement(CStatement::Return());

  InlineCrossJumpedCallTails(&root);

  std::string emitted = root.ToString();
  EXPECT_THAT(emitted, Not(HasSubstr("goto block_11")));
  EXPECT_THAT(emitted, Not(HasSubstr("goto block_12")));
  EXPECT_THAT(emitted, Not(HasSubstr("block_11:")));
  EXPECT_THAT(emitted, Not(HasSubstr("block_12:")));
  const auto& sw = static_cast<const SwitchStatement&>(*root.Statements()[0]);
  ASSERT_EQ(sw.Cases().size(), 3u);
  EXPECT_EQ(sw.Cases()[0].body->Statements().back()->Kind(), CStatementKind::kBreakStatement);
  EXPECT_EQ(sw.Cases()[1].body->Statements().back()->Kind(), CStatementKind::kBreakStatement);
  EXPECT_EQ(sw.Cases()[2].body->Statements().back()->Kind(), CStatementKind::kBreakStatement);
}

}  // namespace
}  // namespace rom_nom_nom
