#include "lifter/ast_arithmetic_simplifier.h"

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

TEST(AstArithmeticSimplifierTest, ShiftAddMultiplicationSimplification) {
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

TEST(AstArithmeticSimplifierTest, FoldsMultiStepShiftAddSubConstantMultiplications) {
  // 00: sll  $v0, $a0, 4       # v0 = a0 << 4
  // 04: subu $v0, $v0, $a0     # v0 = v0 - a0  (a0 * 15)
  // 08: sll  $v1, $a1, 1       # v1 = a1 << 1
  // 0C: addu $v1, $v1, $a1     # v1 = v1 + a1  (a1 * 3)
  // 10: sll  $v1, $v1, 3       # v1 = v1 << 3  (a1 * 24)
  // 14: addu $v1, $v1, $a1     # v1 = v1 + a1  (a1 * 25)
  // 18: sll  $v1, $v1, 2       # v1 = v1 << 2  (a1 * 100)
  // 1C: jr   $ra
  // 20: addu $v0, $v0, $v1
  std::vector<uint32_t> words = {
      0x00041100,  // 00: sll  $v0, $a0, 4
      0x00441023,  // 04: subu $v0, $v0, $a0
      0x00051840,  // 08: sll  $v1, $a1, 1
      0x00651821,  // 0C: addu $v1, $v1, $a1
      0x000318C0,  // 10: sll  $v1, $v1, 3
      0x00651821,  // 14: addu $v1, $v1, $a1
      0x00031880,  // 18: sll  $v1, $v1, 2
      0x03E00008,  // 1C: jr   $ra
      0x00431021,  // 20: addu $v0, $v0, $v1
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestShiftAddSubConstMul";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("(arg0 * 15)"));
  EXPECT_THAT(code, HasSubstr("(arg1 * 100)"));
}

TEST(AstArithmeticSimplifierTest, FoldsReciprocalConstantDivisionsAndUnsignedToFloatDiamond) {
  auto index_or = SymbolIndex::ParseFromTextproto(R"(
    entries { name: "D_8011DD50" address: 0x8011DD50 type: SYMBOL_DATA size: 8 }
  )");
  ASSERT_TRUE(index_or.ok());

  // 00: lui     $a1, 0x8080
  // 04: ori     $a1, $a1, 0x8081   # a1 = 0x80808081 (magic for / 255u)
  // 08: multu   $a0, $a1
  // 0C: mfhi    $v0
  // 10: srl     $v0, $v0, 7        # v0 = (u32)arg0 / 255u
  // 14: mtc1    $v0, $f2
  // 18: bgez    $v0, 0x8000002C
  // 1C: cvt.d.w $f2, $f2
  // 20: lui     $at, 0x8012
  // 24: ldc1    $f0, -8880($at)
  // 28: add.d   $f2, $f2, $f0
  // 2C: cvt.s.d $f0, $f2
  // 30: jr      $ra
  // 34: nop
  std::vector<uint32_t> words = {
      0x3C058080,  // 00: lui     $a1, 0x8080
      0x34A58081,  // 04: ori     $a1, $a1, 0x8081
      0x00850019,  // 08: multu   $a0, $a1
      0x00001010,  // 0C: mfhi    $v0
      0x000211C2,  // 10: srl     $v0, $v0, 7
      0x44821000,  // 14: mtc1    $v0, $f2
      0x04410004,  // 18: bgez    $v0, 0x8000002C
      0x468010A1,  // 1C: cvt.d.w $f2, $f2
      0x3C018012,  // 20: lui     $at, 0x8012
      0xD420DD50,  // 24: ldc1    $f0, -8880($at)
      0x46201080,  // 28: add.d   $f2, $f2, $f0
      0x46201020,  // 2C: cvt.s.d $f0, $f2
      0x03E00008,  // 30: jr      $ra
      0x00000000,  // 34: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestUnsignedDivAndFloatDiamond";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, &(*index_or), options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("/ 255U"));
  EXPECT_THAT(code, HasSubstr("(f32)(u32)"));
  EXPECT_THAT(code, Not(HasSubstr("0x80808081")));
  EXPECT_THAT(code, Not(HasSubstr("f2_d")));
}

TEST(AstArithmeticSimplifierTest,
     UnsignedFloatDiamondSupportsFoldedFloatLiteralCustomSymbolAndRawAddress) {
  // 1. Folded FloatLiteral(4294967296.0) bias and (f32)f2_d on the LHS of a binary expression.
  {
    CompoundStatement block;
    block.AddStatement(std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
        "=", std::make_unique<IdentifierExpression>("f2_d"),
        std::make_unique<CastExpression>(CType::F64(),
                                         std::make_unique<IdentifierExpression>("arg0")))));
    auto then_b = std::make_unique<CompoundStatement>();
    then_b->AddStatement(
        std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
            "=", std::make_unique<IdentifierExpression>("f2_d"),
            std::make_unique<BinaryExpression>(
                "+", std::make_unique<IdentifierExpression>("f2_d"),
                std::make_unique<FloatLiteralExpression>(4294967296.0, false)))));
    block.AddStatement(std::make_unique<IfStatement>(
        std::make_unique<BinaryExpression>("<", std::make_unique<IdentifierExpression>("arg0"),
                                           std::make_unique<IntegerLiteralExpression>(0)),
        std::move(then_b)));
    block.AddStatement(std::make_unique<ReturnStatement>(std::make_unique<BinaryExpression>(
        "*",
        std::make_unique<CastExpression>(CType::F32(),
                                         std::make_unique<IdentifierExpression>("f2_d")),
        std::make_unique<IdentifierExpression>("arg1_f"))));

    SimplifyReciprocalDivisionAndUnsignedFloatConversions(&block);
    std::string code = block.ToString(0);
    EXPECT_THAT(code, HasSubstr("return ((f32)(u32)arg0 * arg1_f);"));
    EXPECT_THAT(code, Not(HasSubstr("if (")));
    EXPECT_THAT(code, Not(HasSubstr("4294967296")));
  }

  // 2. Non-2^32 FloatLiteral(1.0) must NOT be folded as an unsigned float conversion diamond.
  {
    CompoundStatement block;
    block.AddStatement(std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
        "=", std::make_unique<IdentifierExpression>("f2_d"),
        std::make_unique<CastExpression>(CType::F64(),
                                         std::make_unique<IdentifierExpression>("arg0")))));
    auto then_b = std::make_unique<CompoundStatement>();
    then_b->AddStatement(
        std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
            "=", std::make_unique<IdentifierExpression>("f2_d"),
            std::make_unique<BinaryExpression>(
                "+", std::make_unique<IdentifierExpression>("f2_d"),
                std::make_unique<FloatLiteralExpression>(1.0, false)))));
    block.AddStatement(std::make_unique<IfStatement>(
        std::make_unique<BinaryExpression>("<", std::make_unique<IdentifierExpression>("arg0"),
                                           std::make_unique<IntegerLiteralExpression>(0)),
        std::move(then_b)));
    block.AddStatement(std::make_unique<ReturnStatement>(std::make_unique<CastExpression>(
        CType::F32(), std::make_unique<IdentifierExpression>("f2_d"))));

    SimplifyReciprocalDivisionAndUnsignedFloatConversions(&block);
    std::string code = block.ToString(0);
    EXPECT_THAT(code, HasSubstr("if ((arg0 < 0))"));
    EXPECT_THAT(code, Not(HasSubstr("(u32)arg0")));
  }

  // 3. Custom named symbol *(const f64*)&kUintBias and standalone f64 unsigned conversion in store.
  {
    CompoundStatement block;
    block.AddStatement(std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
        "=", std::make_unique<IdentifierExpression>("f2_d"),
        std::make_unique<CastExpression>(CType::F64(),
                                         std::make_unique<IdentifierExpression>("arg0")))));
    auto then_b = std::make_unique<CompoundStatement>();
    then_b->AddStatement(
        std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
            "=", std::make_unique<IdentifierExpression>("f2_d"),
            std::make_unique<BinaryExpression>(
                "+",
                std::make_unique<UnaryExpression>(
                    "*", std::make_unique<CastExpression>(
                             CType{"f64", true, 1},
                             std::make_unique<UnaryExpression>(
                                 "&", std::make_unique<IdentifierExpression>("kUintBias")))),
                std::make_unique<IdentifierExpression>("f2_d")))));
    block.AddStatement(std::make_unique<IfStatement>(
        std::make_unique<BinaryExpression>("<", std::make_unique<IdentifierExpression>("arg0"),
                                           std::make_unique<IntegerLiteralExpression>(0)),
        std::move(then_b)));
    block.AddStatement(std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
        "=",
        std::make_unique<UnaryExpression>(
            "*", std::make_unique<CastExpression>(CType::F64().MakePointer(),
                                                  std::make_unique<IdentifierExpression>("out_d"))),
        std::make_unique<IdentifierExpression>("f2_d"))));

    SimplifyReciprocalDivisionAndUnsignedFloatConversions(&block);
    std::string code = block.ToString(0);
    EXPECT_THAT(code, HasSubstr("*(f64*)out_d = (f64)(u32)arg0;"));
    EXPECT_THAT(code, Not(HasSubstr("if (")));
  }

  // 4. Shared 2^32 bias variable (f4_d) used across a block and nested conditional block.
  {
    CompoundStatement block;
    block.AddStatement(std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
        "=", std::make_unique<IdentifierExpression>("f4_d"),
        std::make_unique<UnaryExpression>(
            "*", std::make_unique<CastExpression>(
                     CType::F64().MakePointer(),
                     std::make_unique<IntegerLiteralExpression>(0x8011DD50, true))))));
    auto outer_then = std::make_unique<CompoundStatement>();
    outer_then->AddStatement(
        std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
            "=", std::make_unique<IdentifierExpression>("f2_d"),
            std::make_unique<CastExpression>(CType::F64(),
                                             std::make_unique<IdentifierExpression>("arg2")))));
    auto then_b = std::make_unique<CompoundStatement>();
    then_b->AddStatement(
        std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
            "=", std::make_unique<IdentifierExpression>("f2_d"),
            std::make_unique<BinaryExpression>("+", std::make_unique<IdentifierExpression>("f2_d"),
                                               std::make_unique<IdentifierExpression>("f4_d")))));
    outer_then->AddStatement(std::make_unique<IfStatement>(
        std::make_unique<BinaryExpression>("<", std::make_unique<IdentifierExpression>("arg2"),
                                           std::make_unique<IntegerLiteralExpression>(0)),
        std::move(then_b)));
    outer_then->AddStatement(
        std::make_unique<ExpressionStatement>(std::make_unique<AssignmentExpression>(
            "=", std::make_unique<IdentifierExpression>("f6_d"),
            std::make_unique<BinaryExpression>("+", std::make_unique<IdentifierExpression>("f6_d"),
                                               std::make_unique<IdentifierExpression>("f2_d")))));
    block.AddStatement(std::make_unique<IfStatement>(
        std::make_unique<BinaryExpression>("<", std::make_unique<IdentifierExpression>("v1"),
                                           std::make_unique<IdentifierExpression>("arg1")),
        std::move(outer_then)));

    SimplifyReciprocalDivisionAndUnsignedFloatConversions(&block);
    std::string code = block.ToString(0);
    EXPECT_THAT(code, HasSubstr("f6_d = (f6_d + (f64)(u32)arg2);"));
    EXPECT_THAT(code, Not(HasSubstr("f4_d")));
    EXPECT_THAT(code, Not(HasSubstr("arg2 < 0")));
  }
}

TEST(AstArithmeticSimplifierTest,
     FoldsSignedDivisionWithHoistedMagicInFpAndPartiallyFoldedHighMult) {
  // 00: lui   $fp, 0xB216
  // 04: ori   $fp, $fp, 0x42C9   # 0xB21642C9 (magic for / 184)
  // 08: mult  $a0, $fp
  // 0C: mfhi  $v0
  // 10: addu  $v0, $v0, $a0
  // 14: sra   $v0, $v0, 7
  // 18: sra   $v1, $a0, 31
  // 1C: subu  $v0, $v0, $v1
  // 20: jr    $ra
  // 24: nop
  std::vector<uint32_t> words = {
      0x3C1EB216,  // 00: lui   $fp, 0xB216
      0x37DE42C9,  // 04: ori   $fp, $fp, 0x42C9
      0x009E0018,  // 08: mult  $a0, $fp
      0x00001010,  // 0C: mfhi  $v0
      0x00441021,  // 10: addu  $v0, $v0, $a0
      0x000211C3,  // 14: sra   $v0, $v0, 7
      0x00041FC3,  // 18: sra   $v1, $a0, 31
      0x00431023,  // 1C: subu  $v0, $v0, $v1
      0x03E00008,  // 20: jr    $ra
      0x00000000,  // 24: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "DivBy184";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("(arg0 / 184)"));
  EXPECT_THAT(code, Not(HasSubstr("0xB21642C9")));
}

TEST(AstArithmeticSimplifierTest,
     FoldsUnsignedReciprocalDivisionWhenNumeratorClobberedBeforeShift) {
  // Unsigned division of $a0 by 10 (`magic = 0xCCCCCCCD`, shift = 3) where $a0 is
  // overwritten before the final `srl $v0, $t0, 3` into a distinct register:
  //   lui   $at, 0xCCCC
  //   ori   $at, $at, 0xCCCD
  //   multu $a0, $at
  //   mfhi  $t0
  //   addiu $a0, $a1, 1       <-- clobbers numerator $a0 before `srl`
  //   srl   $v0, $t0, 3
  //   sw    $v0, 0($a1)
  //   addu  $v0, $v0, $a0
  //   jr    $ra
  //   nop
  std::vector<uint32_t> words = {
      0x3C01CCCC,  // 00: lui   $at, 0xCCCC
      0x3421CCCD,  // 04: ori   $at, $at, 0xCCCD
      0x00810019,  // 08: multu $a0, $at
      0x00004010,  // 0C: mfhi  $t0
      0x24A40001,  // 10: addiu $a0, $a1, 1
      0x000810C2,  // 14: srl   $v0, $t0, 3
      0xACA20000,  // 18: sw    $v0, 0($a1)
      0x00441021,  // 1C: addu  $v0, $v0, $a0
      0x03E00008,  // 20: jr    $ra
      0x00000000,  // 24: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestUnsignedDivClobberedNumerator";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("/ 10"));
  EXPECT_THAT(code, Not(HasSubstr("0xCCCCCCCD")));
}

}  // namespace
}  // namespace rom_nom_nom
