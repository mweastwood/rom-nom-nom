#include "lifter/ast_parameter_coalescer.h"

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

TEST(AstParameterCoalescerTest, SplitsReusedFloatGprParameterIntoIntegerTempAndTypesParamAsF32) {
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

TEST(AstParameterCoalescerTest, CoalescesCallerHomedParameterSlotWhenAddressTaken) {
  std::string textproto = R"pb(
    entries { name: "TakePtr" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // 00: addiu $sp, $sp, -24
  // 04: sw    $ra, 20($sp)
  // 08: sw    $s0, 16($sp)
  // 0C: sw    $a0, 24($sp)
  // 10: addiu $a0, $sp, 24
  // 14: jal   0x80001000
  // 18: addu  $s0, $a1, $zero
  // 1C: addu  $v0, $v0, $s0
  // 20: lw    $ra, 20($sp)
  // 24: lw    $s0, 16($sp)
  // 28: jr    $ra
  // 2C: addiu $sp, $sp, 24
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0xAFB00010,  // 08: sw    $s0, 16($sp)
      0xAFA40018,  // 0C: sw    $a0, 24($sp)
      0x27A40018,  // 10: addiu $a0, $sp, 24
      0x0C000400,  // 14: jal   0x80001000
      0x00A08021,  // 18: addu  $s0, $a1, $zero
      0x00501021,  // 1C: addu  $v0, $v0, $s0
      0x8FBF0014,  // 20: lw    $ra, 20($sp)
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
  options.function_name = "TestCallerHomedAddressTaken";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("TakePtr((s32)&arg0)"));
  EXPECT_THAT(code, Not(HasSubstr("var_sp_24")));
  EXPECT_THAT(code, Not(HasSubstr("u8 sp[")));
}

TEST(AstParameterCoalescerTest, CoalescesCallerHomedParameterSlotWithoutAddressTaken) {
  // Function that homes $a0 to 24($sp) (its caller home slot in a 24-byte frame)
  // and reloads it after a call without taking its address.
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0xAFA40018,  // 08: sw    $a0, 24($sp)
      0x0C000400,  // 0C: jal   0x80001000
      0x00000000,  // 10: nop
      0x8FA30018,  // 14: lw    $v1, 24($sp)
      0x00431021,  // 18: addu  $v0, $v0, $v1
      0x8FBF0014,  // 1C: lw    $ra, 20($sp)
      0x03E00008,  // 20: jr    $ra
      0x27BD0018,  // 24: addiu $sp, $sp, 24
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestHomedParamCoalesce";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, Not(HasSubstr("var_sp_24")));
  EXPECT_THAT(code, HasSubstr("arg0"));
}

TEST(AstParameterCoalescerTest, SplitsParameterRegisterReusedAsScratchAfterPrologueCopy) {
  // Function with 2 parameters ($a0, $a1) that copies $a0 -> $s0 and $a1 -> $s1
  // before a call, then reuses $a0 as a scratch register after the call.
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFB00010,  // 04: sw    $s0, 16($sp)
      0x00808021,  // 08: addu  $s0, $a0, $zero
      0xAFBF0018,  // 0C: sw    $ra, 24($sp)
      0xAFB10014,  // 10: sw    $s1, 20($sp)
      0x8E040000,  // 14: lw    $a0, 0($s0)
      0x00A08821,  // 18: addu  $s1, $a1, $zero
      0x0C000400,  // 1C: jal   0x80001000
      0x26250001,  // 20: addiu $a1, $s1, 1
      0x00402021,  // 24: addu  $a0, $v0, $zero
      0x8E030008,  // 28: lw    $v1, 8($s0)
      0x00831821,  // 2C: addu  $v1, $a0, $v1
      0x00712026,  // 30: xor   $a0, $v1, $s1
      0xAE030004,  // 34: sw    $v1, 4($s0)
      0xAE04000C,  // 38: sw    $a0, 12($s0)
      0x00801021,  // 3C: addu  $v0, $a0, $zero
      0x8FBF0018,  // 40: lw    $ra, 24($sp)
      0x8FB10014,  // 44: lw    $s1, 20($sp)
      0x8FB00010,  // 48: lw    $s0, 16($sp)
      0x03E00008,  // 4C: jr    $ra
      0x27BD0020,  // 50: addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestSplitParamScratch";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("register s32 temp_a0 asm(\"$4\");"));
  EXPECT_THAT(code, HasSubstr("temp_s0 = arg0;"));
}

TEST(AstParameterCoalescerTest, SplitsParameterReusedInsideConditionalBranchAfterCall) {
  // Single parameter ($a0) copied to $s0, $s1 used as callee-saved register,
  // and $a0 overwritten and used multiple times inside a conditional branch after a call.
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xAFB10018,  // 08: sw    $s1, 24($sp)
      0xAFB00014,  // 0C: sw    $s0, 20($sp)
      0x00808021,  // 10: addu  $s0, $a0, $zero
      0x8E110000,  // 14: lw    $s1, 0($s0)
      0x12200006,  // 18: beq   $s1, $zero, 0x34
      0x00000000,  // 1C: nop
      0x0C000400,  // 20: jal   0x80001000
      0x8E040004,  // 24: lw    $a0, 4($s0)
      0x8E040008,  // 28: lw    $a0, 8($s0)
      0x02248821,  // 2C: addu  $s1, $s1, $a0
      0xAE04000C,  // 30: sw    $a0, 12($s0)
      0x02201021,  // 34: addu  $v0, $s1, $zero
      0x8FBF001C,  // 38: lw    $ra, 28($sp)
      0x8FB10018,  // 3C: lw    $s1, 24($sp)
      0x8FB00014,  // 40: lw    $s0, 20($sp)
      0x03E00008,  // 44: jr    $ra
      0x27BD0020,  // 48: addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestSplitParamInIf";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("register s32 temp_s1 asm(\"$17\");"));
  EXPECT_THAT(code, HasSubstr("temp_a0"));
}

TEST(AstParameterCoalescerTest, PreservesCalleeSavedFprCopyFromGprFloatParameterAcrossCalls) {
  std::string textproto = R"pb(
    entries { name: "InitMatrix" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xAFB00018,  // 08: sw    $s0, 24($sp)
      0xE7B40014,  // 0C: swc1  $f20, 20($sp)
      0x00808021,  // 10: addu  $s0, $a0, $zero
      0x4485A000,  // 14: mtc1  $a1, $f20
      0x0C000400,  // 18: jal   0x80001000 (InitMatrix)
      0x00000000,  // 1C: nop
      0xE6140000,  // 20: swc1  $f20, 0($s0)
      0x8FBF001C,  // 24: lw    $ra, 28($sp)
      0x8FB00018,  // 28: lw    $s0, 24($sp)
      0xC7B40014,  // 2C: lwc1  $f20, 20($sp)
      0x03E00008,  // 30: jr    $ra
      0x27BD0020,  // 34: addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestGprFloatParamSavedToF20";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &*index_or, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("register f32 f20 asm(\"$f20\");"));
  EXPECT_THAT(code, HasSubstr("f20 = arg1;"));
}

TEST(AstParameterCoalescerTest, SplitsScratchReusedParametersAcrossMultiParamGotoFunction) {
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xAFB10018,  // 08: sw    $s1, 24($sp)
      0xAFB00014,  // 0C: sw    $s0, 20($sp)
      0x00808821,  // 10: move  $s1, $a0
      0x00A08021,  // 14: move  $s0, $a1
      0x0C000400,  // 18: jal   0x80001000
      0x2405012C,  // 1C: addiu $a1, $zero, 300
      0x8E240000,  // 20: lw    $a0, 0($s1)
      0x10800003,  // 24: beq   $a0, $zero, +3 (to 0x34)
      0x00000000,  // 28: nop
      0x24840001,  // 2C: addiu $a0, $a0, 1
      0xAE040000,  // 30: sw    $a0, 0($s0)
      0x00801021,  // 34: move  $v0, $a0
      0x8FB10018,  // 38: lw    $s1, 24($sp)
      0x8FB00014,  // 3C: lw    $s0, 20($sp)
      0x8FBF001C,  // 40: lw    $ra, 28($sp)
      0x03E00008,  // 44: jr    $ra
      0x27BD0020,  // 48: addiu $sp, $sp, 32
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
  options.function_name = "TestSplitMultiParamGoto";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *seq, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("temp_s1 = arg0;"));
  EXPECT_THAT(code, HasSubstr("register s32 temp_a0 asm(\"$4\");"));
  EXPECT_THAT(code, HasSubstr("temp_a0 = *(s32*)temp_s1;"));
  EXPECT_THAT(code, HasSubstr("*(s32*)arg1 = temp_a0;"));
}

TEST(AstParameterCoalescerTest, CoalescesPrologueV0CopyUsedInConditionalCallArguments) {
  // Prologue copies $a1 to $v0 in the branch delay slot before an if/else where
  // both branches overwrite $a1 with an outgoing call argument while reading
  // float stack arguments from $v0:
  //   00: addiu $sp, $sp, -32
  //   04: sw    $ra, 24($sp)
  //   08: blez  $a0, .Lelse (0x28)
  //   0C: addu  $v0, $a1, $zero
  //   10: lwc1  $f0, 0($v0)
  //   14: addiu $a1, $zero, 1
  //   18: jal   0x80001000
  //   1C: swc1  $f0, 16($sp)
  //   20: j     .Lend (0x38)
  //   24: nop
  // .Lelse:
  //   28: lwc1  $f2, 4($v0)
  //   2C: addiu $a1, $zero, 2
  //   30: jal   0x80002000
  //   34: swc1  $f2, 16($sp)
  // .Lend:
  //   38: lw    $ra, 24($sp)
  //   3C: jr    $ra
  //   40: addiu $sp, $sp, 32
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF0018,  // 04: sw    $ra, 24($sp)
      0x18800006,  // 08: blez  $a0, +6 (to 0x28)
      0x00A01021,  // 0C: addu  $v0, $a1, $zero
      0xC4400000,  // 10: lwc1  $f0, 0($v0)
      0x24050001,  // 14: addiu $a1, $zero, 1
      0x0C000400,  // 18: jal   0x80001000
      0xE7A00010,  // 1C: swc1  $f0, 16($sp)
      0x0800000E,  // 20: j     0x80000038
      0x00000000,  // 24: nop
      0xC4420004,  // 28: lwc1  $f2, 4($v0)
      0x24050002,  // 2C: addiu $a1, $zero, 2
      0x0C000800,  // 30: jal   0x80002000
      0xE7A20010,  // 34: swc1  $f2, 16($sp)
      0x8FBF0018,  // 38: lw    $ra, 24($sp)
      0x03E00008,  // 3C: jr    $ra
      0x27BD0020,  // 40: addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestPrologueV0CopyCoalesced";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, Not(HasSubstr("v0 = arg1;")));
  EXPECT_THAT(code, HasSubstr("*(f32*)arg1"));
  EXPECT_THAT(code, HasSubstr("*(f32*)(arg1 + 4)"));
}

TEST(AstParameterCoalescerTest, CoalescesMutatedFirstParameterWhenSecondParameterLivesAcrossCall) {
  // Prologue saves $a0 -> $s0 and $a1 -> $s1, then loops advancing $s0 = *($s0)
  // while passing $s1 to a call on each iteration:
  //   00: addiu $sp, $sp, -32
  //   04: sw    $s0, 16($sp)
  //   08: addu  $s0, $a0, $zero
  //   0C: sw    $s1, 20($sp)
  //   10: addu  $s1, $a1, $zero
  //   14: beq   $s0, $zero, .Lend (0x34)
  //   18: sw    $ra, 24($sp)
  // .Lloop (0x1C):
  //   1C: lw    $a0, 4($s0)
  //   20: jal   0x80001000
  //   24: addu  $a1, $s1, $zero
  //   28: lw    $s0, 0($s0)
  //   2C: bne   $s0, $zero, .Lloop (0x1C)
  //   30: nop
  // .Lend (0x34):
  //   34: lw    $ra, 24($sp)
  //   38: lw    $s1, 20($sp)
  //   3C: lw    $s0, 16($sp)
  //   40: addiu $sp, $sp, 32
  //   44: jr    $ra
  //   48: nop
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFB00010,  // 04: sw    $s0, 16($sp)
      0x00808021,  // 08: addu  $s0, $a0, $zero
      0xAFB10014,  // 0C: sw    $s1, 20($sp)
      0x00A08821,  // 10: addu  $s1, $a1, $zero
      0x12000007,  // 14: beq   $s0, $zero, +7 (to 0x34)
      0xAFBF0018,  // 18: sw    $ra, 24($sp)
      0x8E040004,  // 1C: lw    $a0, 4($s0)
      0x0C000400,  // 20: jal   0x80001000
      0x02202821,  // 24: addu  $a1, $s1, $zero
      0x8E100000,  // 28: lw    $s0, 0($s0)
      0x1600FFFB,  // 2C: bne   $s0, $zero, -5 (to 0x1C)
      0x00000000,  // 30: nop
      0x8FBF0018,  // 34: lw    $ra, 24($sp)
      0x8FB10014,  // 38: lw    $s1, 20($sp)
      0x8FB00010,  // 3C: lw    $s0, 16($sp)
      0x27BD0020,  // 40: addiu $sp, $sp, 32
      0x03E00008,  // 44: jr    $ra
      0x00000000,  // 48: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestMutatedParam0Coalesced";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, Not(HasSubstr("temp_s0")));
  EXPECT_THAT(code, Not(HasSubstr("temp_s1")));
  EXPECT_THAT(code, HasSubstr("arg0 = *(s32*)arg0;"));
}

TEST(AstParameterCoalescerTest, SplitsAndCoalescesMultiplePrologueSavedParametersReusedAsScratch) {
  // Prologue saves $a0 -> $s0 and $a1 -> $s1, calls a function using $s1, and
  // then reuses $a0 and $a1 as scratch registers while reading from $s0 and $s1:
  //   00: addiu $sp, $sp, -32
  //   04: sw    $s0, 16($sp)
  //   08: addu  $s0, $a0, $zero
  //   0C: sw    $s1, 20($sp)
  //   10: addu  $s1, $a1, $zero
  //   14: addu  $a0, $s1, $zero
  //   18: sw    $ra, 24($sp)
  //   1C: jal   0x80001000
  //   20: addiu $a1, $zero, 300
  //   24: lw    $a0, 8($s0)
  //   28: lw    $a1, 4($s1)
  //   2C: addu  $v0, $a0, $a1
  //   30: lw    $ra, 24($sp)
  //   34: lw    $s1, 20($sp)
  //   38: lw    $s0, 16($sp)
  //   3C: addiu $sp, $sp, 32
  //   40: jr    $ra
  //   44: nop
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFB00010,  // 04: sw    $s0, 16($sp)
      0x00808021,  // 08: addu  $s0, $a0, $zero
      0xAFB10014,  // 0C: sw    $s1, 20($sp)
      0x00A08821,  // 10: addu  $s1, $a1, $zero
      0x02202021,  // 14: addu  $a0, $s1, $zero
      0xAFBF0018,  // 18: sw    $ra, 24($sp)
      0x0C000400,  // 1C: jal   0x80001000
      0x2405012C,  // 20: addiu $a1, $zero, 300
      0x8E040008,  // 24: lw    $a0, 8($s0)
      0x8E250004,  // 28: lw    $a1, 4($s1)
      0x00851021,  // 2C: addu  $v0, $a0, $a1
      0x8FBF0018,  // 30: lw    $ra, 24($sp)
      0x8FB10014,  // 34: lw    $s1, 20($sp)
      0x8FB00010,  // 38: lw    $s0, 16($sp)
      0x27BD0020,  // 3C: addiu $sp, $sp, 32
      0x03E00008,  // 40: jr    $ra
      0x00000000,  // 44: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestMultiParamSavedAndSplit";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, Not(HasSubstr("temp_s0")));
  EXPECT_THAT(code, Not(HasSubstr("temp_s1")));
  EXPECT_THAT(code, HasSubstr("*(s32*)(arg0 + 8)"));
  EXPECT_THAT(code, HasSubstr("*(s32*)(arg1 + 4)"));
}

TEST(AstParameterCoalescerTest, CoalescesHomedFloatParameterStoreIntoIntegerBitcastRead) {
  // Zero-frame leaf function homes $f12 to 0($sp) and loads it into $v0 as an
  // integer to extract IEEE-754 exponent bits:
  //   00: swc1  $f12, 0($sp)
  //   04: lw    $v0, 0($sp)
  //   08: sra   $v0, $v0, 22
  //   0C: andi  $v0, $v0, 0x1FF
  //   10: jr    $ra
  //   14: nop
  std::vector<uint32_t> words = {
      0xE7AC0000,  // 00: swc1  $f12, 0($sp)
      0x8FA20000,  // 04: lw    $v0, 0($sp)
      0x00021583,  // 08: sra   $v0, $v0, 22
      0x304201FF,  // 0C: andi  $v0, $v0, 0x1FF
      0x03E00008,  // 10: jr    $ra
      0x00000000,  // 14: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestHomedFloatBitcastRead";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, Not(HasSubstr("var_sp_0")));
  EXPECT_THAT(code, HasSubstr("*(s32*)&f12"));
}

TEST(AstParameterCoalescerTest, SplitReusedFloatGprParametersRewritesSubsequentIntUseToTemp) {
  CompoundStatement body;
  // f2 = arg1 + 1.0f;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("f2"),
      CExpression::Binary("+", CExpression::Identifier("arg1"), CExpression::Float(1.0)))));
  // arg1 = 42;
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("arg1"), CExpression::Integer(42))));
  // return arg1;
  body.AddStatement(CStatement::Return(CExpression::Identifier("arg1")));

  std::vector<CParameter> params = {
      {.type = CType::F32(), .name = "f12"},
      {.type = CType::F32(), .name = "arg1"},
  };
  SplitReusedFloatGprParameters(&body, params);

  std::string code = body.ToString();
  EXPECT_THAT(code, HasSubstr("f2 = (arg1 + 1.0f);"));
  EXPECT_THAT(code, HasSubstr("temp_a1 = 42;"));
  EXPECT_THAT(code, HasSubstr("return temp_a1;"));
}

TEST(AstParameterCoalescerTest, SplitScratchReusedParameterRegistersSplitsDeadEntryArgAfterCopy) {
  CompoundStatement body;
  // temp_t0 = arg0;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t0"), CExpression::Identifier("arg0"))));
  // arg0 = temp_t0 + 4;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("arg0"),
      CExpression::Binary("+", CExpression::Identifier("temp_t0"), CExpression::Integer(4)))));
  // return arg0;
  body.AddStatement(CStatement::Return(CExpression::Identifier("arg0")));

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"}};
  SplitScratchReusedParameterRegisters(&body, params, /*frame_size=*/24);

  std::string code = body.ToString();
  EXPECT_THAT(code, HasSubstr("temp_a0 = (temp_t0 + 4);"));
  EXPECT_THAT(code, HasSubstr("return temp_a0;"));
}

TEST(AstParameterCoalescerTest, CoalescePrologueParameterCopiesGeneralizesMutatedParamAndV0Copy) {
  // 1. Mutated second parameter (temp_s1 = arg1) coalesced when arg0 is also saved/used and no
  // other uncoalesced $s registers remain:
  {
    CompoundStatement body;
    // temp_s0 = arg0;
    body.AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("temp_s0"), CExpression::Identifier("arg0"))));
    // temp_s1 = arg1;
    body.AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("temp_s1"), CExpression::Identifier("arg1"))));
    // while (temp_s1 != 0) { func_80001000(temp_s0, temp_s1); temp_s1 = *(s32*)temp_s1; }
    auto loop_body = std::make_unique<CompoundStatement>();
    std::vector<std::unique_ptr<CExpression>> call_args;
    call_args.push_back(CExpression::Identifier("temp_s0"));
    call_args.push_back(CExpression::Identifier("temp_s1"));
    loop_body->AddStatement(CStatement::Expression(
        CExpression::Call(CExpression::Identifier("func_80001000"), std::move(call_args))));
    loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("temp_s1"),
        CExpression::Unary(
            "*", CExpression::Cast(CType{"s32", false, 1}, CExpression::Identifier("temp_s1"))))));
    body.AddStatement(CStatement::While(
        CExpression::Binary("!=", CExpression::Identifier("temp_s1"), CExpression::Integer(0)),
        std::move(loop_body)));

    std::vector<CParameter> params = {
        {.type = CType::S32(), .name = "arg0"},
        {.type = CType::S32(), .name = "arg1"},
    };
    CoalescePrologueParameterCopies(&body, params, /*frame_size=*/32);

    std::string code = body.ToString();
    EXPECT_THAT(code, Not(HasSubstr("temp_s0")));
    EXPECT_THAT(code, Not(HasSubstr("temp_s1")));
    EXPECT_THAT(code, HasSubstr("func_80001000(arg0, arg1);"));
    EXPECT_THAT(code, HasSubstr("arg1 = *(s32*)arg1;"));
  }

  // 2. Prologue v0 = arg0 copy at statement index i >= 3 before an if-statement that overwrites
  // arg0 while reading v0 in a call argument:
  {
    CompoundStatement body;
    // Four call-free prologue statements before v0 = arg0 (so v0 = arg0 is at index 3 >= 3):
    for (int idx = 0; idx < 3; ++idx) {
      body.AddStatement(CStatement::Expression(CExpression::Assignment(
          "=",
          CExpression::Unary(
              "*", CExpression::Cast(CType{"s32", false, 1},
                                     CExpression::Binary("+", CExpression::Identifier("arg1"),
                                                         CExpression::Integer(idx * 4)))),
          CExpression::Integer(idx))));
    }
    // v0 = arg0;
    body.AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("v0"), CExpression::Identifier("arg0"))));
    // if (arg2 > 0) { func_80001000(1, *(s32*)v0); }
    auto then_branch = std::make_unique<CompoundStatement>();
    std::vector<std::unique_ptr<CExpression>> call_args;
    call_args.push_back(CExpression::Integer(1));
    call_args.push_back(CExpression::Unary(
        "*", CExpression::Cast(CType{"s32", false, 1}, CExpression::Identifier("v0"))));
    then_branch->AddStatement(CStatement::Expression(
        CExpression::Call(CExpression::Identifier("func_80001000"), std::move(call_args))));
    body.AddStatement(CStatement::If(
        CExpression::Binary(">", CExpression::Identifier("arg2"), CExpression::Integer(0)),
        std::move(then_branch), nullptr));

    std::vector<CParameter> params = {
        {.type = CType::S32(), .name = "arg0"},
        {.type = CType::S32(), .name = "arg1"},
        {.type = CType::S32(), .name = "arg2"},
    };
    CoalescePrologueParameterCopies(&body, params, /*frame_size=*/24);

    std::string code = body.ToString();
    EXPECT_THAT(code, Not(HasSubstr("v0 = arg0;")));
    EXPECT_THAT(code, HasSubstr("func_80001000(1, *(s32*)arg0);"));
  }
}

TEST(AstParameterCoalescerTest,
     SplitScratchReusedParameterRegistersHandlesDoWhileLoopsAndNonMatchingCalleeSavedCopies) {
  CompoundStatement body;
  // temp_s2 = arg0;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s2"), CExpression::Identifier("arg0"))));
  // do { arg0 = 0; func_80001000(arg0); } while (temp_s2 != 0);
  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("arg0"), CExpression::Integer(0))));
  std::vector<std::unique_ptr<CExpression>> call_args;
  call_args.push_back(CExpression::Identifier("arg0"));
  loop_body->AddStatement(CStatement::Expression(
      CExpression::Call(CExpression::Identifier("func_80001000"), std::move(call_args))));
  body.AddStatement(CStatement::DoWhile(
      std::move(loop_body),
      CExpression::Binary("!=", CExpression::Identifier("temp_s2"), CExpression::Integer(0))));
  body.AddStatement(CStatement::Return());

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"}};
  SplitScratchReusedParameterRegisters(&body, params, /*frame_size=*/32);

  std::string code = body.ToString();
  EXPECT_THAT(code, HasSubstr("temp_s2 = arg0;"));
  EXPECT_THAT(code, HasSubstr("temp_a0 = 0;"));
  EXPECT_THAT(code, HasSubstr("func_80001000(temp_a0);"));
}

TEST(AstParameterCoalescerTest,
     CoalescesReassignedHigherCalleeSavedParamCopyWithBareAndOffsetDerefsAcrossCall) {
  CompoundStatement body;
  // temp_s1 = arg0;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_s1"), CExpression::Identifier("arg0"))));
  // arg0 = 1;
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("arg0"), CExpression::Integer(1))));
  // func_80001000(arg0);
  {
    std::vector<std::unique_ptr<CExpression>> args;
    args.push_back(CExpression::Identifier("arg0"));
    body.AddStatement(CStatement::Expression(
        CExpression::Call(CExpression::Identifier("func_80001000"), std::move(args))));
  }
  // temp_s0 = func_80002000(*(s32*)temp_s1, *(s32*)(temp_s1 + 8));
  {
    std::vector<std::unique_ptr<CExpression>> args;
    args.push_back(CExpression::Unary(
        "*", CExpression::Cast(CType{"s32", false, 1}, CExpression::Identifier("temp_s1"))));
    args.push_back(CExpression::Unary(
        "*", CExpression::Cast(CType{"s32", false, 1},
                               CExpression::Binary("+", CExpression::Identifier("temp_s1"),
                                                   CExpression::Integer(8)))));
    body.AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier("temp_s0"),
        CExpression::Call(CExpression::Identifier("func_80002000"), std::move(args)))));
  }
  // temp_s1 = 0;
  body.AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s1"), CExpression::Integer(0))));
  // return temp_s1 + temp_s0;
  body.AddStatement(CStatement::Return(CExpression::Binary("+", CExpression::Identifier("temp_s1"),
                                                           CExpression::Identifier("temp_s0"))));

  std::vector<CParameter> params = {{.type = CType::S32(), .name = "arg0"}};
  EXPECT_TRUE(SplitScratchReusedParameterRegisters(&body, params, /*frame_size=*/32));

  std::string code = body.ToString();
  EXPECT_THAT(code, Not(HasSubstr("temp_s1")));
  EXPECT_THAT(code, HasSubstr("func_80002000(*(s32*)arg0, *(s32*)(arg0 + 8))"));
  EXPECT_THAT(code, HasSubstr("arg0 = 0;"));
}

}  // namespace
}  // namespace rom_nom_nom
