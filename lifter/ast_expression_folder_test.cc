#include "lifter/ast_expression_folder.h"

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

TEST(AstExpressionFolderTest, ChainedSelfAssignmentPreservesLvalueAndDefinitions) {
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

TEST(AstExpressionFolderTest, FoldExpressionIntoIfCondition) {
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

TEST(AstExpressionFolderTest, FoldExpressionIntoReturn) {
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

TEST(AstExpressionFolderTest, FoldExpressionIntoSwitchCondition) {
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

TEST(AstExpressionFolderTest, ScopeAwareFoldingAcrossMultipleScratchUses) {
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

TEST(AstExpressionFolderTest, GeneralInPlaceScratchRedefinitionFolding) {
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

TEST(AstExpressionFolderTest, OutgoingStackArgumentFoldingIntoCall) {
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

TEST(AstExpressionFolderTest, DeadPreCallArgAssignmentsEliminatedAndSignExtendedCallFolded) {
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

TEST(AstExpressionFolderTest, CrossBlockEntryParametersPassedToCallAndGlobalArrayElementFolded) {
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

TEST(AstExpressionFolderTest, FoldsCallReturnAndLoadsAcrossProvablyDisjointStructStores) {
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

TEST(AstExpressionFolderTest, FoldsFcondAndDoublePrecisionScratchTemporaries) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "D_800AED48" address: 0x800AED48 type: SYMBOL_DATA size: 8 }
  )");
  ASSERT_TRUE(index_or.ok());

  // 00: lui     $at, 0x800B
  // 04: ldc1    $f18, -4792($at)   # f18_d = D_800AED48
  // 08: cvt.d.s $f4, $f12          # f4_d = (f64)f12
  // 0C: sub.d   $f6, $f18, $f4     # f6_d = f18_d - f4_d
  // 10: cvt.s.d $f0, $f6           # f0 = (f32)f6_d
  // 14: jr      $ra
  // 18: nop
  std::vector<uint32_t> words = {
      0x3C01800B,  // 00: lui     $at, 0x800B
      0xD432ED48,  // 04: ldc1    $f18, -4792($at)
      0x46006121,  // 08: cvt.d.s $f4, $f12
      0x46249181,  // 0C: sub.d   $f6, $f18, $f4
      0x46203020,  // 10: cvt.s.d $f0, $f6
      0x03E00008,  // 14: jr      $ra
      0x00000000,  // 18: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestDoubleScratchFolding";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("return (f32)(*(f64*)&D_800AED48 - (f64)f12);"));
  EXPECT_THAT(code, Not(HasSubstr("f18_d")));
  EXPECT_THAT(code, Not(HasSubstr("f4_d")));
  EXPECT_THAT(code, Not(HasSubstr("f6_d")));
}

TEST(AstExpressionFolderTest, FoldsFloatBitcastOutgoingStackArgsAcrossDisjointGlobalStore) {
  std::string textproto = R"pb(
    entries { name: "SinkCall" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
    entries { name: "D_800E7A0E" address: 0x800E7A0E type: SYMBOL_DATA size: 2 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // 00: addiu $sp, $sp, -32
  // 04: sw    $ra, 28($sp)
  // 08: lwc1  $f0, 0($a1)
  // 0C: swc1  $f0, 16($sp)
  // 10: lwc1  $f2, 4($a1)
  // 14: swc1  $f2, 20($sp)
  // 18: lui   $v0, 0x800F
  // 1C: sh    $a0, 0x7A0E($v0)
  // 20: addiu $a1, $zero, 1
  // 24: addiu $a2, $zero, 2
  // 28: jal   0x80001000
  // 2C: addiu $a3, $zero, 3
  // 30: lw    $ra, 28($sp)
  // 34: jr    $ra
  // 38: addiu $sp, $sp, 32
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xC4A00000,  // 08: lwc1  $f0, 0($a1)
      0xE7A00010,  // 0C: swc1  $f0, 16($sp)
      0xC4A20004,  // 10: lwc1  $f2, 4($a1)
      0xE7A20014,  // 14: swc1  $f2, 20($sp)
      0x3C02800F,  // 18: lui   $v0, 0x800F
      0xA4447A0E,  // 1C: sh    $a0, 0x7A0E($v0)
      0x24050001,  // 20: addiu $a1, $zero, 1
      0x24060002,  // 24: addiu $a2, $zero, 2
      0x0C000400,  // 28: jal   0x80001000
      0x24070003,  // 2C: addiu $a3, $zero, 3
      0x8FBF001C,  // 30: lw    $ra, 28($sp)
      0x03E00008,  // 34: jr    $ra
      0x27BD0020,  // 38: addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestFloatStackArgsAcrossGlobalStore";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("((void (*)(s32, s32, s32, s32, f32, f32))SinkCall)(arg0, 1, 2, 3, "
                              "*(f32*)arg1, *(f32*)(arg1 + 4));"));
  EXPECT_THAT(code, Not(HasSubstr("var_sp_16")));
  EXPECT_THAT(code, Not(HasSubstr("var_sp_20")));
}

TEST(AstExpressionFolderTest, FoldsSingleUseScratchKilledBeforeSubsequentGotoLabel) {
  // Block 0:
  // 00: beq   $a0, $zero, +5 (to Block 2 at 0x18)
  // 04: nop
  // Block 1:
  // 08: addiu $v0, $a1, 10
  // 0C: sw    $v0, 0($a2)
  // 10: lw    $v0, 4($a2)
  // 14: sw    $v0, 8($a2)
  // Block 2 (shared label target):
  // 18: jr    $ra
  // 1C: nop
  std::vector<uint32_t> words = {
      0x10800005,  // 00: beq   $a0, $zero, +5 (to 0x18)
      0x00000000,  // 04: nop
      0x24A2000A,  // 08: addiu $v0, $a1, 10
      0xACC20000,  // 0C: sw    $v0, 0($a2)
      0x8CC20004,  // 10: lw    $v0, 4($a2)
      0xACC20008,  // 14: sw    $v0, 8($a2)
      0x03E00008,  // 18: jr    $ra
      0x00000000,  // 1C: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  auto seq = std::make_unique<StructuredRegion>();
  seq->type = RegionType::kSequence;

  auto if_reg = std::make_unique<StructuredRegion>();
  if_reg->type = RegionType::kIfThenElse;
  if_reg->condition_block_id = 0;
  auto goto_b2 = std::make_unique<StructuredRegion>();
  goto_b2->type = RegionType::kGoto;
  goto_b2->block_id = 2;
  if_reg->children.push_back(std::move(goto_b2));
  seq->children.push_back(std::move(if_reg));

  auto b1 = std::make_unique<StructuredRegion>();
  b1->type = RegionType::kBlock;
  b1->block_id = 1;
  seq->children.push_back(std::move(b1));

  auto b2 = std::make_unique<StructuredRegion>();
  b2->type = RegionType::kBlock;
  b2->block_id = 2;
  seq->children.push_back(std::move(b2));

  AstConverterOptions options;
  options.function_name = "TestKilledBeforeLabel";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *seq, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("*(s32*)arg2 = (arg1 + 10);"));
  EXPECT_THAT(code, Not(HasSubstr("v0 = (arg1 + 10);")));
}

TEST(AstExpressionFolderTest, PreservesLoopHoistedConstantRegisterInStoreInsideLoop) {
  // Block 0 (preheader):
  // 00: addiu $v1, $zero, 47
  // Block 1 (do-while loop body):
  // 04: sb    $v1, 0($a0)
  // 08: addiu $a1, $a1, -1
  // 0C: bne   $a1, $zero, -3 (to 0x04)
  // 10: addiu $a0, $a0, 1
  // Block 2 (exit):
  // 14: jr    $ra
  // 18: nop
  std::vector<uint32_t> words = {
      0x2403002F,  // 00: addiu $v1, $zero, 47
      0xA0830000,  // 04: sb    $v1, 0($a0)
      0x24A5FFFF,  // 08: addiu $a1, $a1, -1
      0x14A0FFFD,  // 0C: bne   $a1, $zero, -3 (to 0x04)
      0x24840001,  // 10: addiu $a0, $a0, 1
      0x03E00008,  // 14: jr    $ra
      0x00000000,  // 18: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestLoopHoistedStore";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("*(s8*)arg0 = v1;"));
}

TEST(AstExpressionFolderTest, FoldsOutgoingStackArgInsideDoWhileLoopWithGotoExit) {
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xAFB00018,  // 08: sw    $s0, 24($sp)
      0x00808021,  // 0C: move  $s0, $a0
      0x24020007,  // 10: addiu $v0, $zero, 7 (loop body block 1)
      0xAFA20010,  // 14: sw    $v0, 16($sp)
      0x00002021,  // 18: move  $a0, $zero
      0x00002821,  // 1C: move  $a1, $zero
      0x00003021,  // 20: move  $a2, $zero
      0x0C000400,  // 24: jal   0x80001000
      0x00003821,  // 28: move  $a3, $zero
      0x14400004,  // 2C: bne   $v0, $zero, +4 (to 0x40 early exit)
      0x00000000,  // 30: nop
      0x2610FFFF,  // 34: addiu $s0, $s0, -1 (block 2)
      0x1600FFF6,  // 38: bne   $s0, $zero, -10 (to 0x10)
      0x00000000,  // 3C: nop
      0x8FBF001C,  // 40: lw    $ra, 28($sp) (block 3 exit)
      0x8FB00018,  // 44: lw    $s0, 24($sp)
      0x03E00008,  // 48: jr    $ra
      0x27BD0020,  // 4C: addiu $sp, $sp, 32
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestOutgoingStackArgInLoop";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("func_80001000(0, 0, 0, 0, v0)"));
  EXPECT_THAT(code, ::testing::Not(HasSubstr("var_sp_16")));
}

TEST(AstExpressionFolderTest, FoldsSingleUseCallArgInsideLoopEvenWhenFunctionHasGotoLabel) {
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // 00: addiu $sp, $sp, -32
      0xAFBF001C,  // 04: sw    $ra, 28($sp)
      0xAFB00018,  // 08: sw    $s0, 24($sp)
      0xAFB10014,  // 0C: sw    $s1, 20($sp)
      0x00808021,  // 10: move  $s0, $a0
      0x26050020,  // 14: addiu $s1, $s0, 32 (Wait: 0x26110020 is addiu $s1, $s0, 32)
      0x12000009,  // 18: beq   $s0, $zero, +9 -> 0x40 (shared exit block)
      0x00000000,  // 1C: nop
      0x00002021,  // 20: move  $a0, $zero (loop header)
      0x0C000400,  // 24: jal   0x80001000
      0x02202821,  // 28: move  $a1, $s1 (delay slot sets arg1)
      0x14400004,  // 2C: bne   $v0, $zero, +4 -> 0x40 (early exit to shared label)
      0x00000000,  // 30: nop
      0x2610FFFF,  // 34: addiu $s0, $s0, -1
      0x1600FFF9,  // 38: bne   $s0, $zero, -7 -> 0x20
      0x00000000,  // 3C: nop
      0x8FBF001C,  // 40: lw    $ra, 28($sp)
      0x8FB00018,  // 44: lw    $s0, 24($sp)
      0x8FB10014,  // 48: lw    $s1, 20($sp)
      0x03E00008,  // 4C: jr    $ra
      0x27BD0020,  // 50: addiu $sp, $sp, 32
  };
  words[5] = 0x26110020;  // addiu $s1, $s0, 32
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  absl::flat_hash_map<std::string, int> param_counts = {{"func_80001000", 2}};
  AstConverterOptions options;
  options.function_name = "TestSingleUseCallArgInLoop";
  options.function_parameter_counts = &param_counts;
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("func_80001000(0, temp_s1)"));
  EXPECT_THAT(code, ::testing::Not(HasSubstr("arg1 = temp_s1;")));
}

TEST(AstExpressionFolderTest, PreservesTempHoldingMultiplicationBeforeGlobalAddressAddition) {
  auto body = std::make_unique<CompoundStatement>();
  // v0 = (temp_s0 * 40);
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary("*", CExpression::Identifier("temp_s0"), CExpression::Integer(40)))));
  // if ((*(u8*)((u8*)&D_80158512 + v0) != 0)) { temp_s1 = 1; }
  auto cond = CExpression::Binary(
      "!=",
      CExpression::Unary(
          "*",
          CExpression::Cast(
              CType::U8().MakePointer(),
              CExpression::Binary(
                  "+",
                  CExpression::Cast(CType::U8().MakePointer(),
                                    CExpression::Unary("&", CExpression::Identifier("D_80158512"))),
                  CExpression::Identifier("v0")))),
      CExpression::Integer(0));
  auto then_branch = std::make_unique<CompoundStatement>();
  then_branch->AddStatement(CStatement::Expression(
      CExpression::Assignment("=", CExpression::Identifier("temp_s1"), CExpression::Integer(1))));
  body->AddStatement(CStatement::If(std::move(cond), std::move(then_branch)));
  body->AddStatement(CStatement::Return());

  FoldSingleUseExpressions(body.get());
  std::string code = body->ToString();

  EXPECT_THAT(code, HasSubstr("v0 = (temp_s0 * 40);"));
  EXPECT_THAT(code, HasSubstr("((u8*)&D_80158512 + v0)"));
}

TEST(AstExpressionFolderTest, PreservesTempHoldingShiftAddChainBeforeGlobalAddressAddition) {
  auto body = std::make_unique<CompoundStatement>();
  // v0 = ((arg0 << 3) - arg0) << 2;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Binary(
          "<<",
          CExpression::Binary(
              "-",
              CExpression::Binary("<<", CExpression::Identifier("arg0"), CExpression::Integer(3)),
              CExpression::Identifier("arg0")),
          CExpression::Integer(2)))));
  // *(u8*)((u8*)&D_80158512 + v0) = 1;
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*",
          CExpression::Cast(
              CType::U8().MakePointer(),
              CExpression::Binary(
                  "+",
                  CExpression::Cast(CType::U8().MakePointer(),
                                    CExpression::Unary("&", CExpression::Identifier("D_80158512"))),
                  CExpression::Identifier("v0")))),
      CExpression::Integer(1))));
  body->AddStatement(CStatement::Return());

  FoldSingleUseExpressions(body.get());
  std::string code = body->ToString();

  EXPECT_THAT(code, HasSubstr("v0 = (((arg0 << 3) - arg0) << 2);"));
  EXPECT_THAT(code, HasSubstr("((u8*)&D_80158512 + v0)"));
}

}  // namespace
}  // namespace rom_nom_nom
