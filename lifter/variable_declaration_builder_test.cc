#include "lifter/variable_declaration_builder.h"

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

TEST(VariableDeclarationBuilderTest, LocalVariableDeclarationFiltering) {
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

TEST(VariableDeclarationBuilderTest, ConvertSwitchStatementDeclaresLocalVariables) {
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

TEST(VariableDeclarationBuilderTest, DeclaresStackBufferWithDetectedFrameSize) {
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

TEST(VariableDeclarationBuilderTest, FailsWhenStackBufferAccessedWithoutPrologueAdjustment) {
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

TEST(VariableDeclarationBuilderTest, RobustPrologueAnalysisWithSdAndMultiBlock) {
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

TEST(VariableDeclarationBuilderTest, DeclaresFpRegistersWithProperFloatTypesAndFoldsCop1Branch) {
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

TEST(VariableDeclarationBuilderTest, DoesNotDeclareSynthesizedDataGlobalsAsLocalVariables) {
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

TEST(VariableDeclarationBuilderTest, CalleeSavedInCallDelaySlotDetected) {
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

TEST(VariableDeclarationBuilderTest, CollectUsedVariablesProperlyCollectsVariablesInForStatement) {
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
  CollectUsedVariables(*for_stmt, &vars, &seen);

  EXPECT_THAT(vars, UnorderedElementsAre("i", "limit", "total", "item"));
}

TEST(VariableDeclarationBuilderTest, VarsNeedingInitDataflowAcrossBlocks) {
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

TEST(VariableDeclarationBuilderTest,
     CalleeSavedRegistersOmitBogusZeroInitAndEmitHardwareRegisterBinding) {
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

TEST(VariableDeclarationBuilderTest, AnnotatesCallerSavedRegistersWhenNotLiveAcrossCalls) {
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

TEST(VariableDeclarationBuilderTest, PinsCallerSavedTemporariesUsedAfterCallFreeGotoLabel) {
  // Block 0 (ends at call):
  // 00: addiu $sp, $sp, -24
  // 04: sw    $ra, 20($sp)
  // 08: jal   0x80001000
  // 0C: nop
  // Block 1 (branch after call):
  // 10: beq   $a0, $zero, +2 (to Block 3 at 0x20)
  // 14: nop
  // Block 2 (fallthrough):
  // 18: addiu $a0, $a0, 1
  // 1C: nop
  // Block 3 (goto target label):
  // 20: addiu $t0, $a0, 5
  // 24: sw    $t0, 0($a1)
  // 28: sll   $v1, $t0, 2
  // 2C: sw    $v1, 4($a1)
  // 30: addu  $v0, $v1, $t0
  // 34: lw    $ra, 20($sp)
  // 38: jr    $ra
  // 3C: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0x0C000400,  // 08: jal   0x80001000
      0x00000000,  // 0C: nop
      0x10800002,  // 10: beq   $a0, $zero, +2 (to 0x1C)
      0x00000000,  // 14: nop
      0x24840001,  // 18: addiu $a0, $a0, 1
      0x24880005,  // 1C: addiu $t0, $a0, 5
      0xACA80000,  // 20: sw    $t0, 0($a1)
      0x00081880,  // 24: sll   $v1, $t0, 2
      0xACA30004,  // 28: sw    $v1, 4($a1)
      0x00681021,  // 2C: addu  $v0, $v1, $t0
      0x8FBF0014,  // 30: lw    $ra, 20($sp)
      0x03E00008,  // 34: jr    $ra
      0x27BD0018,  // 38: addiu $sp, $sp, 24
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto seq = std::make_unique<StructuredRegion>();
  seq->type = RegionType::kSequence;

  auto b0 = std::make_unique<StructuredRegion>();
  b0->type = RegionType::kBlock;
  b0->block_id = 0;
  seq->children.push_back(std::move(b0));

  auto if_reg = std::make_unique<StructuredRegion>();
  if_reg->type = RegionType::kIfThenElse;
  if_reg->condition_block_id = 1;
  auto goto_b3 = std::make_unique<StructuredRegion>();
  goto_b3->type = RegionType::kGoto;
  goto_b3->block_id = 3;
  if_reg->children.push_back(std::move(goto_b3));
  seq->children.push_back(std::move(if_reg));

  auto b2 = std::make_unique<StructuredRegion>();
  b2->type = RegionType::kBlock;
  b2->block_id = 2;
  seq->children.push_back(std::move(b2));

  auto b3 = std::make_unique<StructuredRegion>();
  b3->type = RegionType::kBlock;
  b3->block_id = 3;
  seq->children.push_back(std::move(b3));

  AstConverterOptions options;
  options.function_name = "TestPinAfterGotoLabel";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *seq, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("register s32 temp_t0 asm(\"$8\");"));
  EXPECT_THAT(code, HasSubstr("register s32 v1 asm(\"$3\");"));
}

TEST(VariableDeclarationBuilderTest, SortsStackLocalDeclarationsByNumericOffset) {
  // Function accessing stack locals at 20($sp) then 16($sp).
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xAFA50014,  // 08: sw    $a1, 20($sp)
      0xAFA60010,  // 0C: sw    $a2, 16($sp)
      0x8FA20014,  // 10: lw    $v0, 20($sp)
      0x8FA30010,  // 14: lw    $v1, 16($sp)
      0x00431021,  // 18: addu  $v0, $v0, $v1
      0x8FBF001C,  // 1C: lw    $ra, 28($sp)
      0x03E00008,  // 20: jr    $ra
      0x27BD0020,  // 24: addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestSortStackVars";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  size_t pos_16 = code.find("s32 var_sp_16;");
  size_t pos_20 = code.find("s32 var_sp_20;");
  ASSERT_NE(pos_16, std::string::npos);
  ASSERT_NE(pos_20, std::string::npos);
  EXPECT_LT(pos_16, pos_20);
}

TEST(VariableDeclarationBuilderTest, PinsReturnScratchWhenAssignedMaskedCallReturnAcrossCalls) {
  std::string textproto = R"pb(
    entries { name: "func_80001000" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
    entries { name: "func_80001100" address: 0x80001100 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0xAFB00010,  // 08: sw    $s0, 16($sp)
      0x00808025,  // 0C: move  $s0, $a0
      0x0C000400,  // 10: jal   0x80001000
      0x00000000,  // 14: nop
      0x304200FF,  // 18: andi  $v0, $v0, 0xFF
      0xAC820000,  // 1C: sw    $v0, 0($s0)
      0x0C000440,  // 20: jal   0x80001100
      0x00402025,  // 24: move  $a0, $v0
      0x304200FF,  // 28: andi  $v0, $v0, 0xFF
      0xAC820004,  // 2C: sw    $v0, 4($s0)
      0x0C000400,  // 30: jal   0x80001000
      0x00000000,  // 34: nop
      0x304200FF,  // 38: andi  $v0, $v0, 0xFF
      0xAC820008,  // 3C: sw    $v0, 8($s0)
      0x0C000440,  // 40: jal   0x80001100
      0x00402025,  // 44: move  $a0, $v0
      0x304200FF,  // 48: andi  $v0, $v0, 0xFF
      0xAC82000C,  // 4C: sw    $v0, 12($s0)
      0x0C000400,  // 50: jal   0x80001000
      0x00000000,  // 54: nop
      0x304200FF,  // 58: andi  $v0, $v0, 0xFF
      0xAC820010,  // 5C: sw    $v0, 16($s0)
      0x0C000440,  // 60: jal   0x80001100
      0x00402025,  // 64: move  $a0, $v0
      0x0C000400,  // 68: jal   0x80001000
      0x00000000,  // 6C: nop
      0x304200FF,  // 70: andi  $v0, $v0, 0xFF
      0xAC820014,  // 74: sw    $v0, 20($s0)
      0x0C000440,  // 78: jal   0x80001100
      0x00402025,  // 7C: move  $a0, $v0
      0x8FBF0014,  // 80: lw    $ra, 20($sp)
      0x8FB00010,  // 84: lw    $s0, 16($sp)
      0x03E00008,  // 88: jr    $ra
      0x27BD0018,  // 8C: addiu $sp, $sp, 24
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestMaskedCallReturnPinning";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &*index_or, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("register s32 v0 asm(\"$2\");"));
}

TEST(VariableDeclarationBuilderTest,
     PinsCallerSavedScratchUsedInWrappedCallConditionAndMaskedReturn) {
  std::string textproto = R"pb(
    entries { name: "CheckSlot" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
    entries { name: "StepSlot" address: 0x80001100 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0xAFB00010,  // 08: sw    $s0, 16($sp)
      0x00808021,  // 0C: addu  $s0, $a0, $zero
      0x8E030000,  // 10: lw    $v1, 0($s0)
      0x24630001,  // 14: addiu $v1, $v1, 1
      0xAE030000,  // 18: sw    $v1, 0($s0)
      0x0C000400,  // 1C: jal   0x80001000 (CheckSlot)
      0x00602021,  // 20: addu  $a0, $v1, $zero
      0x10400005,  // 24: beq   $v0, $zero, 0x3C
      0x00000000,  // 28: nop
      0x0C000440,  // 2C: jal   0x80001100 (StepSlot)
      0x8E040000,  // 30: lw    $a0, 0($s0)
      0x304300FF,  // 34: andi  $v1, $v0, 0xFF
      0xAE030004,  // 38: sw    $v1, 4($s0)
      0x8E020004,  // 3C: lw    $v0, 4($s0)
      0x8FBF0014,  // 40: lw    $ra, 20($sp)
      0x8FB00010,  // 44: lw    $s0, 16($sp)
      0x03E00008,  // 48: jr    $ra
      0x27BD0018,  // 4C: addiu $sp, $sp, 24
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestWrappedCallCondPinning";
  absl::flat_hash_set<std::string> funcs_returning_v0 = {"CheckSlot", "StepSlot"};
  options.functions_returning_v0 = &funcs_returning_v0;
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &*index_or, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("register s32 v1 asm(\"$3\");"));
}

TEST(VariableDeclarationBuilderTest, PinsScratchRegistersInLeafWithCalleeSavedRegs) {
  std::vector<uint32_t> words = {
      0x27BDFFF0,  // 00: addiu $sp, $sp, -16
      0xAFB00008,  // 04: sw    $s0, 8($sp)
      0xAFB1000C,  // 08: sw    $s1, 12($sp)
      0x8C820000,  // 0C: lw    $v0, 0($a0)
      0x8C830004,  // 10: lw    $v1, 4($a0)
      0x8C850008,  // 14: lw    $a1, 8($a0)
      0x00438021,  // 18: addu  $s0, $v0, $v1
      0x02058821,  // 1C: addu  $s1, $s0, $a1
      0x10400003,  // 20: beq   $v0, $zero, 0x30
      0x00000000,  // 24: nop
      0x02231021,  // 28: addu  $v0, $s1, $v1
      0xAC82000C,  // 2C: sw    $v0, 12($a0)
      0x02051021,  // 30: addu  $v0, $s0, $a1
      0x8FB00008,  // 34: lw    $s0, 8($sp)
      0x8FB1000C,  // 38: lw    $s1, 12($sp)
      0x03E00008,  // 3C: jr    $ra
      0x27BD0010,  // 40: addiu $sp, $sp, 16
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestLeafWithCalleeSavedScratchPinning";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("register s32 v0 asm(\"$2\");"));
  EXPECT_THAT(code, HasSubstr("register s32 v1 asm(\"$3\");"));
  EXPECT_THAT(code, HasSubstr("register s32 arg1 asm(\"$5\");"));
}

TEST(VariableDeclarationBuilderTest, ZeroFrameFunctionDoesNotPinCalleeSavedRegisters) {
  std::vector<uint32_t> words = {
      0x8C820000,  // 00: lw    $v0, 0($a0)
      0x8C500004,  // 04: lw    $s0, 4($v0)
      0xAC900008,  // 08: sw    $s0, 8($a0)
      0x03E00008,  // 0C: jr    $ra
      0x00000000,  // 10: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestZeroFrameUnpinnedS0";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, ::testing::Not(HasSubstr("asm(\"$16\")")));
}

TEST(VariableDeclarationBuilderTest, Declares64BitStackVariableAndExtractsSubwordsFromSdStore) {
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xFFA40008,  // 04: sd    $a0, 8($sp)
      0x8FA20008,  // 08: lw    $v0, 8($sp)    (high 32-bit word at base offset)
      0x87A3000A,  // 0C: lh    $v1, 10($sp)   (halfword 1)
      0x00431021,  // 10: addu  $v0, $v0, $v1
      0x03E00008,  // 14: jr    $ra
      0x27BD0018,  // 18: addiu $sp, $sp, 24
  };
  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80001000));
  DominatorTree dom = DominatorTree::Compute(cfg);
  DominatorTree pdom = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dom);
  auto region = ControlFlowStructurer::Structure(cfg, dom, pdom, loops);
  ASSERT_THAT(region, NotNull());

  AstConverterOptions opts;
  opts.function_name = "ExtractFromSdLocal";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *region, nullptr, opts);
  std::string code = func.ToString();
  EXPECT_THAT(code, HasSubstr("s64 var_sp_8;"));
  EXPECT_THAT(code, HasSubstr("var_sp_8 = arg0;"));
  EXPECT_THAT(code, HasSubstr("(s32)((u64)var_sp_8 >> 32)"));
  EXPECT_THAT(code, HasSubstr("(s16)((u64)var_sp_8 >> 32)"));
}

TEST(VariableDeclarationBuilderTest, ExtractsHighAndLowSignedHalfwordsFrom32BitStackStore) {
  // Leaf function homing a 4-byte struct { s16 hi; s16 lo; } passed in $a0 at 0($sp):
  std::vector<uint32_t> words = {
      0xAFA40000,  // 00: sw   $a0, 0($sp)
      0x87A20000,  // 04: lh   $v0, 0($sp)
      0x87A30002,  // 08: lh   $v1, 2($sp)
      0x00431021,  // 0C: addu $v0, $v0, $v1
      0x03E00008,  // 10: jr   $ra
      0x00000000,  // 14: nop
  };
  auto cfg = *ControlFlowGraph::Build(*DecodeSequence(words, 0x80001000));
  DominatorTree dom = DominatorTree::Compute(cfg);
  DominatorTree pdom = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loops = LoopInfo::Analyze(cfg, dom);
  auto region = ControlFlowStructurer::Structure(cfg, dom, pdom, loops);
  ASSERT_THAT(region, NotNull());

  AstConverterOptions opts;
  opts.function_name = "SumPairS16";
  auto func_or = AstConverter::Convert(cfg, *region, nullptr, opts);
  ASSERT_TRUE(func_or.ok()) << func_or.status();
  std::string code = func_or->ToString();
  EXPECT_THAT(code, HasSubstr("(s16)((u32)var_sp_0 >> 16)"));
  EXPECT_THAT(code, HasSubstr("(s16)var_sp_0"));
}

TEST(VariableDeclarationBuilderTest,
     StructuralRegisterPinningAndS64PointerDerefInterferenceChecks) {
  // 1. Integer-returning leaf function with callee-saved FPR ($f20) and caller-saved ABI FPR ($f0)
  // pins both $f20 and $f0 without requiring 8+ distinct FPR bases:
  std::vector<uint32_t> fp_leaf_words = {
      0x27BDFFF0,  // 00: addiu $sp, $sp, -16
      0xE7B40008,  // 04: swc1  $f20, 8($sp)
      0xC4800000,  // 08: lwc1  $f0, 0($a0)
      0xC4940004,  // 0C: lwc1  $f20, 4($a0)
      0x4600A000,  // 10: add.s $f0, $f20, $f0
      0xE4800008,  // 14: swc1  $f0, 8($a0)
      0x24020001,  // 18: addiu $v0, $zero, 1
      0xC7B40008,  // 1C: lwc1  $f20, 8($sp)
      0x03E00008,  // 20: jr    $ra
      0x27BD0010,  // 24: addiu $sp, $sp, 16
  };
  auto fp_leaf_cfg = *ControlFlowGraph::Build(*DecodeSequence(fp_leaf_words, 0x80001000));
  DominatorTree fp_leaf_dom = DominatorTree::Compute(fp_leaf_cfg);
  DominatorTree fp_leaf_pdom = DominatorTree::ComputePostDominators(fp_leaf_cfg);
  LoopInfo fp_leaf_loops = LoopInfo::Analyze(fp_leaf_cfg, fp_leaf_dom);
  auto fp_leaf_region =
      ControlFlowStructurer::Structure(fp_leaf_cfg, fp_leaf_dom, fp_leaf_pdom, fp_leaf_loops);
  ASSERT_THAT(fp_leaf_region, NotNull());

  AstConverterOptions fp_leaf_opts;
  fp_leaf_opts.function_name = "IntReturnLeafWithF20AndF0";
  auto fp_leaf_func = *AstConverter::Convert(fp_leaf_cfg, *fp_leaf_region, nullptr, fp_leaf_opts);
  std::string fp_leaf_code = fp_leaf_func.ToString();
  EXPECT_THAT(fp_leaf_code, HasSubstr("register f32 f20 asm(\"$f20\");"));
  EXPECT_THAT(fp_leaf_code, HasSubstr("register f32 f0 asm(\"$f0\");"));

  // 2. Structural *(s64*) pointer dereference check suppresses $26/$27 kernel register pinning:
  std::vector<uint32_t> s64_deref_words = {
      0xDC9A0000,  // 00: ld   $k0, 0($a0)
      0xFC9A0008,  // 04: sd   $k0, 8($a0)
      0x03401021,  // 08: addu $v0, $k0, $zero
      0x03E00008,  // 0C: jr   $ra
      0x00000000,  // 10: nop
  };
  auto s64_cfg = *ControlFlowGraph::Build(*DecodeSequence(s64_deref_words, 0x80001000));
  DominatorTree s64_dom = DominatorTree::Compute(s64_cfg);
  DominatorTree s64_pdom = DominatorTree::ComputePostDominators(s64_cfg);
  LoopInfo s64_loops = LoopInfo::Analyze(s64_cfg, s64_dom);
  auto s64_region = ControlFlowStructurer::Structure(s64_cfg, s64_dom, s64_pdom, s64_loops);
  ASSERT_THAT(s64_region, NotNull());

  AstConverterOptions s64_opts;
  s64_opts.function_name = "S64DerefWithK0";
  auto s64_func = *AstConverter::Convert(s64_cfg, *s64_region, nullptr, s64_opts);
  std::string s64_code = s64_func.ToString();
  EXPECT_THAT(s64_code, HasSubstr("*(s64*)arg0"));
  EXPECT_THAT(s64_code, Not(HasSubstr("asm(\"$26\")")));
}

}  // namespace
}  // namespace rom_nom_nom
