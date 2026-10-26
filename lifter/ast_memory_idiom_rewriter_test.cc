#include "lifter/ast_memory_idiom_rewriter.h"

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

TEST(AstMemoryIdiomRewriterTest, CollapsesUnalignedBlock8CopyFromLwlLwrSwlSwr) {
  // 00: lwl $v0, 0($a1)
  // 04: lwr $v0, 3($a1)
  // 08: lwl $v1, 4($a1)
  // 0C: lwr $v1, 7($a1)
  // 10: swl $v0, 0($a0)
  // 14: swr $v0, 3($a0)
  // 18: swl $v1, 4($a0)
  // 1C: jr  $ra
  // 20: swr $v1, 7($a0)
  std::vector<uint32_t> words = {
      0x88A20000,  // 00: lwl $v0, 0($a1)
      0x98A20003,  // 04: lwr $v0, 3($a1)
      0x88A30004,  // 08: lwl $v1, 4($a1)
      0x98A30007,  // 0C: lwr $v1, 7($a1)
      0xA8820000,  // 10: swl $v0, 0($a0)
      0xB8820003,  // 14: swr $v0, 3($a0)
      0xA8830004,  // 18: swl $v1, 4($a0)
      0x03E00008,  // 1C: jr  $ra
      0xB8830007,  // 20: swr $v1, 7($a0)
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestUnalignedBlock8Copy";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("*(UnalignedBlock8*)arg0 = *(UnalignedBlock8*)arg1;"));
}

TEST(AstMemoryIdiomRewriterTest, RewritesFramePointerConstantOffsetDereferencesToStructMembers) {
  // -O0 frame-pointer function ($fp = $sp) that stores an unsigned 16-bit
  // immediate (0x8000) to offset +4 of arg0 and passes a 32-bit field at
  // offset +20 of arg0 to a call:
  //   00: addiu $sp, $sp, -24
  //   04: sw    $ra, 20($sp)
  //   08: sw    $fp, 16($sp)
  //   0C: addu  $fp, $sp, $zero
  //   10: sw    $a0, 24($fp)
  //   14: lw    $v0, 24($fp)
  //   18: ori   $v1, $zero, 0x8000
  //   1C: sh    $v1, 4($v0)
  //   20: lw    $v0, 24($fp)
  //   24: lw    $a0, 20($v0)
  //   28: jal   0x80001000
  //   2C: nop
  //   30: addu  $sp, $fp, $zero
  //   34: lw    $ra, 20($sp)
  //   38: lw    $fp, 16($sp)
  //   3C: addiu $sp, $sp, 24
  //   40: jr    $ra
  //   44: nop
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBF0014,  // 04: sw    $ra, 20($sp)
      0xAFBE0010,  // 08: sw    $fp, 16($sp)
      0x03A0F021,  // 0C: addu  $fp, $sp, $zero
      0xAFC40018,  // 10: sw    $a0, 24($fp)
      0x8FC20018,  // 14: lw    $v0, 24($fp)
      0x34038000,  // 18: ori   $v1, $zero, 0x8000
      0xA4430004,  // 1C: sh    $v1, 4($v0)
      0x8FC20018,  // 20: lw    $v0, 24($fp)
      0x8C440014,  // 24: lw    $a0, 20($v0)
      0x0C000400,  // 28: jal   0x80001000
      0x00000000,  // 2C: nop
      0x03C0E821,  // 30: addu  $sp, $fp, $zero
      0x8FBF0014,  // 34: lw    $ra, 20($sp)
      0x8FBE0010,  // 38: lw    $fp, 16($sp)
      0x27BD0018,  // 3C: addiu $sp, $sp, 24
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
  options.function_name = "TestO0StructOffsetAndCall";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, HasSubstr("((struct { u8 _p[4]; u16 _v; }*)arg0)->_v = 32768;"));
  EXPECT_THAT(code, HasSubstr("((struct { u8 _p[20]; s32 _v; }*)arg0)->_v"));
  EXPECT_THAT(code, Not(HasSubstr("return;")));
}

TEST(AstMemoryIdiomRewriterTest, FoldsFramePointerWhileLoopConditionAndPostIncrementPointerStore) {
  // -O0 frame-pointer function ($fp = $sp) with a while loop that zeroes 16
  // bytes via post-increment on arg1 and then stores 0xFF to offset +4 of arg0:
  std::vector<uint32_t> words = {
      0x27BDFFE8,  // 00: addiu $sp, $sp, -24
      0xAFBE0010,  // 04: sw    $fp, 16($sp)
      0x03A0F021,  // 08: addu  $fp, $sp, $zero
      0xAFC40018,  // 0C: sw    $a0, 24($fp)
      0xAFC5001C,  // 10: sw    $a1, 28($fp)
      0xAFC00008,  // 14: sw    $zero, 8($fp)
      0x8FC20008,  // 18: lw    $v0, 8($fp)
      0x28430010,  // 1C: slti  $v1, $v0, 16
      0x14600003,  // 20: bne   $v1, $zero, 0x80000030
      0x00000000,  // 24: nop
      0x08000015,  // 28: j     0x80000054
      0x00000000,  // 2C: nop
      0x8FC2001C,  // 30: lw    $v0, 28($fp)
      0x24430001,  // 34: addiu $v1, $v0, 1
      0xAFC3001C,  // 38: sw    $v1, 28($fp)
      0xA0400000,  // 3C: sb    $zero, 0($v0)
      0x8FC20008,  // 40: lw    $v0, 8($fp)
      0x24430001,  // 44: addiu $v1, $v0, 1
      0xAFC30008,  // 48: sw    $v1, 8($fp)
      0x08000006,  // 4C: j     0x80000018
      0x00000000,  // 50: nop
      0x8FC20018,  // 54: lw    $v0, 24($fp)
      0x240300FF,  // 58: addiu $v1, $zero, 255
      0xA0430004,  // 5C: sb    $v1, 4($v0)
      0x03C0E821,  // 60: addu  $sp, $fp, $zero
      0x8FBE0010,  // 64: lw    $fp, 16($sp)
      0x27BD0018,  // 68: addiu $sp, $sp, 24
      0x03E00008,  // 6C: jr    $ra
      0x00000000,  // 70: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  ASSERT_THAT(root_region, NotNull());

  AstConverterOptions options;
  options.function_name = "TestO0WhileLoopAndUnsignedByteStore";
  FunctionDeclaration func = *AstConverter::Convert(cfg, *root_region, nullptr, options);
  std::string code = func.ToString();

  EXPECT_THAT(code, Not(HasSubstr("s32 v0")));
  EXPECT_THAT(code, Not(HasSubstr("s32 v1")));
  EXPECT_THAT(code, HasSubstr("while ((var_sp_8 < 16))"));
  EXPECT_THAT(code, HasSubstr("*(s8*)arg1++ = 0;"));
  EXPECT_THAT(code, HasSubstr("var_sp_8 = (var_sp_8 + 1);"));
  EXPECT_THAT(code, HasSubstr("((struct { u8 _p[4]; u8 _v; }*)arg0)->_v = 255;"));
}

TEST(AstMemoryIdiomRewriterTest, CollapseUnalignedBlockCopiesCollapsesWordStore) {
  CompoundStatement body;
  // v0 = *(s32*)arg1;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Unary(
          "*", CExpression::Cast(CType{"s32", false, 1}, CExpression::Identifier("arg1"))))));
  // *(u8*)arg0 = v0 >> 24;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType{"u8", false, 1}, CExpression::Identifier("arg0"))),
      CExpression::Binary(">>", CExpression::Identifier("v0"), CExpression::Integer(24)))));
  // *(u8*)(arg0 + 1) = v0 >> 16;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType{"u8", false, 1},
                                           CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                               CExpression::Integer(1)))),
      CExpression::Binary(">>", CExpression::Identifier("v0"), CExpression::Integer(16)))));
  // *(u8*)(arg0 + 2) = v0 >> 8;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType{"u8", false, 1},
                                           CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                               CExpression::Integer(2)))),
      CExpression::Binary(">>", CExpression::Identifier("v0"), CExpression::Integer(8)))));
  // *(u8*)(arg0 + 3) = v0;
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType{"u8", false, 1},
                                           CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                               CExpression::Integer(3)))),
      CExpression::Identifier("v0"))));

  CollapseUnalignedBlockCopies(&body);
  std::string code = body.ToString();
  EXPECT_THAT(code, HasSubstr("*(UnalignedWord*)arg0 = *(UnalignedWord*)arg1;"));
  EXPECT_THAT(code, Not(HasSubstr(">> 24")));
}

TEST(AstMemoryIdiomRewriterTest, CollapseUnalignedBlockCopiesHandlesHalfwordTailAndCustomSizes) {
  // Test 10-byte copy: 2 s32 loads + 1 s16 tail load + 8 u8 stores + 1 s16 tail store
  CompoundStatement body10;
  body10.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Unary(
          "*", CExpression::Cast(CType{"s32", false, 1}, CExpression::Identifier("arg1"))))));
  body10.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v1"),
      CExpression::Unary("*",
                         CExpression::Cast(CType{"s32", false, 1},
                                           CExpression::Binary("+", CExpression::Identifier("arg1"),
                                                               CExpression::Integer(4)))))));
  body10.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("t0"),
      CExpression::Unary("*",
                         CExpression::Cast(CType{"s16", false, 1},
                                           CExpression::Binary("+", CExpression::Identifier("arg1"),
                                                               CExpression::Integer(8)))))));
  for (int w = 0; w < 2; ++w) {
    std::string var = (w == 0) ? "v0" : "v1";
    for (int b = 0; b < 4; ++b) {
      int off = w * 4 + b;
      auto addr = (off == 0) ? CExpression::Identifier("arg0")
                             : CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                   CExpression::Integer(off));
      auto val = (b == 3) ? CExpression::Identifier(var)
                          : CExpression::Binary(">>", CExpression::Identifier(var),
                                                CExpression::Integer((3 - b) * 8));
      body10.AddStatement(CStatement::Expression(CExpression::Assignment(
          "=", CExpression::Unary("*", CExpression::Cast(CType{"u8", false, 1}, std::move(addr))),
          std::move(val))));
    }
  }
  body10.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary("*",
                         CExpression::Cast(CType{"s16", false, 1},
                                           CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                               CExpression::Integer(8)))),
      CExpression::Identifier("t0"))));

  CollapseUnalignedBlockCopies(&body10);
  EXPECT_THAT(body10.ToString(), HasSubstr("*(UnalignedBlock10*)arg0 = *(UnalignedBlock10*)arg1;"));

  // Test merging UnalignedBlock16 (+0) + UnalignedWord (+16) + 2 trailing s8 copies (+20, +21)
  // into UnalignedBlock22
  CompoundStatement body22;
  for (int chunk = 0; chunk < 2; ++chunk) {
    int n_words = (chunk == 0) ? 4 : 1;
    int base_off = (chunk == 0) ? 0 : 16;
    for (int w = 0; w < n_words; ++w) {
      int off = base_off + w * 4;
      auto addr = (off == 0) ? CExpression::Identifier("arg1")
                             : CExpression::Binary("+", CExpression::Identifier("arg1"),
                                                   CExpression::Integer(off));
      body22.AddStatement(CStatement::Expression(CExpression::Assignment(
          "=", CExpression::Identifier(absl::StrFormat("t%d", w)),
          CExpression::Unary("*", CExpression::Cast(CType{"s32", false, 1}, std::move(addr))))));
    }
    for (int w = 0; w < n_words; ++w) {
      std::string var = absl::StrFormat("t%d", w);
      for (int b = 0; b < 4; ++b) {
        int off = base_off + w * 4 + b;
        auto addr = (off == 0) ? CExpression::Identifier("arg0")
                               : CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                     CExpression::Integer(off));
        auto val = (b == 3) ? CExpression::Identifier(var)
                            : CExpression::Binary(">>", CExpression::Identifier(var),
                                                  CExpression::Integer((3 - b) * 8));
        body22.AddStatement(CStatement::Expression(CExpression::Assignment(
            "=", CExpression::Unary("*", CExpression::Cast(CType{"u8", false, 1}, std::move(addr))),
            std::move(val))));
      }
    }
  }
  // Trailing 2-byte s8 loads and stores at +20 and +21
  for (int t = 0; t < 2; ++t) {
    body22.AddStatement(CStatement::Expression(CExpression::Assignment(
        "=", CExpression::Identifier(absl::StrFormat("v%d", t)),
        CExpression::Unary(
            "*", CExpression::Cast(CType{"u8", false, 1},
                                   CExpression::Binary("+", CExpression::Identifier("arg1"),
                                                       CExpression::Integer(20 + t)))))));
  }
  for (int t = 0; t < 2; ++t) {
    body22.AddStatement(CStatement::Expression(CExpression::Assignment(
        "=",
        CExpression::Unary(
            "*", CExpression::Cast(CType{"s8", false, 1},
                                   CExpression::Binary("+", CExpression::Identifier("arg0"),
                                                       CExpression::Integer(20 + t)))),
        CExpression::Identifier(absl::StrFormat("v%d", t)))));
  }

  CollapseUnalignedBlockCopies(&body22);
  EXPECT_THAT(body22.ToString(), HasSubstr("*(UnalignedBlock22*)arg0 = *(UnalignedBlock22*)arg1;"));
}

TEST(AstMemoryIdiomRewriterTest, RewritesGeneralizedPostIncrementPointerAccessForHalfwordAndWord) {
  CompoundStatement body;

  // 1. Halfword post-increment store `temp_t0 = ptr16; ptr16 = temp_t0 + 2; *(s16*)temp_t0 = arg0;`
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t0"), CExpression::Identifier("ptr16"))));
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("ptr16"),
      CExpression::Binary("+", CExpression::Identifier("temp_t0"), CExpression::Integer(2)))));
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=",
      CExpression::Unary(
          "*", CExpression::Cast(CType{"s16", false, 1}, CExpression::Identifier("temp_t0"))),
      CExpression::Identifier("arg0"))));

  // 2. Word post-increment load `temp_t1 = ptr32; ptr32 = temp_t1 + 4; v0 = *(s32*)temp_t1;`
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("temp_t1"), CExpression::Identifier("ptr32"))));
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("ptr32"),
      CExpression::Binary("+", CExpression::Identifier("temp_t1"), CExpression::Integer(4)))));
  body.AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("v0"),
      CExpression::Unary(
          "*", CExpression::Cast(CType{"s32", false, 1}, CExpression::Identifier("temp_t1"))))));
  body.AddStatement(CStatement::Return(CExpression::Identifier("v0")));

  RewriteFramePointerStructOffsetAccesses(&body);
  std::string rendered = body.ToString();
  EXPECT_THAT(rendered, HasSubstr("*(s16*)ptr16++ = arg0;"));
  EXPECT_THAT(rendered, HasSubstr("*(s32*)ptr32++"));
}

}  // namespace
}  // namespace rom_nom_nom
