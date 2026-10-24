#include "lifter/call_lifter.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lifter/abi_analyzer.h"
#include "lifter/expression_builder.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

TEST(CallLifterTest, FunctionCallAndReturn) {
  std::string textproto = R"pb(
    entries { name: "AudioUpdate" address: 0x80001000 type: SYMBOL_FUNC size: 128 }
  )pb";

  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  std::vector<uint32_t> words = {
      0x24040005,  // 0: addiu $a0, $zero, 5
      0x0C000400,  // 4: jal   0x80001000
      0x00000000,  // 8: nop
      0x03E00008,  // C: jr    $ra
  };

  auto insts = *DecodeSequence(words);
  auto statements =
      ExpressionBuilder::LiftInstructions(insts, FunctionLiftContext{.symbol_index = &(*index_or)});

  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "arg0 = 5;\n");
  EXPECT_EQ(statements[1].ToString(), "AudioUpdate(5);\n");
  EXPECT_EQ(statements[2].ToString(), "return;\n");
}

TEST(CallLifterTest, JalDelaySlotReorderingPassesArgumentToCall) {
  // Call target: 0x80001000
  // 0: jal   0x80001000
  // 4: addiu $a0, $zero, 42  (delay-slot executes BEFORE call!)
  // 8: jr    $ra
  // C: nop
  std::vector<uint32_t> words = {
      0x0C000400,  // 0: jal   0x80001000
      0x2404002A,  // 4: addiu $a0, $zero, 42
      0x03E00008,  // 8: jr    $ra
      0x00000000,  // C: nop
  };

  std::string textproto = R"pb(
    entries { name: "TargetFunc" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  auto instructions = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(
      instructions, FunctionLiftContext{.symbol_index = &(*index_or)});

  // The delay-slot assignment arg0 = 42 must execute before TargetFunc, so the call passes 42
  ASSERT_THAT(statements, SizeIs(3));
  EXPECT_EQ(statements[0].ToString(), "arg0 = 42;\n");
  EXPECT_EQ(statements[1].ToString(), "TargetFunc(42);\n");
  EXPECT_EQ(statements[2].ToString(), "return;\n");
}

TEST(CallLifterTest, JalrDelaySlotReorderingPassesArgumentToCall) {
  // Indirect call via $t9 with delay slot setting $a0:
  // 0: jalr  $t9
  // 4: addiu $a0, $zero, 99
  // 8: jr    $ra
  // C: nop
  std::vector<uint32_t> words = {
      0x0320F809,  // 0: jalr  $ra, $t9
      0x24040063,  // 4: addiu $a0, $zero, 99
      0x03E00008,  // 8: jr    $ra
      0x00000000,  // C: nop
  };

  auto instructions = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(instructions);

  // The delay-slot assignment arg0 = 99 must execute before the indirect call
  ASSERT_GE(statements.size(), 3u);
  EXPECT_EQ(statements[0].ToString(), "arg0 = 99;\n");
  EXPECT_EQ(statements[1].ToString(), "((void (*)())temp_t9)(99);\n");
  EXPECT_EQ(statements.back().kind, StatementKind::kReturn);
}

TEST(CallLifterTest, JalParameterArityUsesKnownCount) {
  // Setup a call with reaching definitions for $a0, $a1, $a2
  std::vector<uint32_t> words = {
      0x24040001,  // 0: addiu $a0, $zero, 1
      0x24050002,  // 4: addiu $a1, $zero, 2
      0x24060003,  // 8: addiu $a2, $zero, 3
      0x0C000400,  // C: jal   0x80001000
      0x00000000,  // 10: nop
  };

  std::string textproto = R"pb(
    entries { name: "BinaryFunc" address: 0x80001000 type: SYMBOL_FUNC size: 32 }
  )pb";
  auto index_or = SymbolIndex::ParseFromTextproto(textproto);
  ASSERT_TRUE(index_or.ok());

  // Tell expression builder that BinaryFunc takes only 2 parameters
  absl::flat_hash_map<std::string, int> parameter_counts;
  parameter_counts["BinaryFunc"] = 2;

  auto instructions = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(
      instructions, FunctionLiftContext{.symbol_index = &(*index_or),
                                        .function_parameter_counts = &parameter_counts});

  ASSERT_THAT(statements, SizeIs(4));
  EXPECT_EQ(statements[3].ToString(), "BinaryFunc(1, 2);\n");
}

TEST(CallLifterTest, IndirectCallJalr) {
  // jalr $t9
  // nop
  std::vector<uint32_t> words = {
      0x0320F809,  // jalr $t9
      0x00000000,  // nop
  };

  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  ASSERT_THAT(statements, SizeIs(1));
  EXPECT_EQ(statements[0].kind, StatementKind::kCall);
  ASSERT_NE(statements[0].expression, nullptr);
  EXPECT_EQ(statements[0].expression->name, "((void (*)())temp_t9)");
  EXPECT_EQ(statements[0].ToString(), "((void (*)())temp_t9)();\n");
}

TEST(CallLifterTest, DetermineFpParameterCountAndPassesFpArgsToCalls) {
  // Function that takes f12 and f14 (uses both before definition)
  std::vector<uint32_t> callee_words = {
      0x462E6032,  // c.eq.d $f12, $f14
      0x45010002,  // bc1t +2
      0x00000000,  // nop
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto callee_insts = *DecodeSequence(callee_words);
  EXPECT_EQ(AbiAnalyzer::DetermineFpParameterCount(callee_insts), 2);

  // Function that defines f12 before use (takes 0 FP parameters)
  std::vector<uint32_t> non_callee_words = {
      0x44806000,  // mtc1 $zero, $f12
      0x46006005,  // abs.s $f0, $f12
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto non_callee_insts = *DecodeSequence(non_callee_words);
  EXPECT_EQ(AbiAnalyzer::DetermineFpParameterCount(non_callee_insts), 0);

  // Caller that calls func_fp_2 via jal
  std::vector<uint32_t> caller_words = {
      0x0C020000,  // jal func_80080000
      0x00000000,  // nop
  };
  auto caller_insts = *DecodeSequence(caller_words, 0x80010000);
  absl::flat_hash_map<std::string, int> gpr_counts = {{"func_80080000", 0}};
  absl::flat_hash_map<std::string, int> fp_counts = {{"func_80080000", 2}};

  auto caller_stmts = ExpressionBuilder::LiftInstructions(
      caller_insts, FunctionLiftContext{.function_parameter_counts = &gpr_counts,
                                        .function_fp_parameter_counts = &fp_counts});
  ASSERT_GE(caller_stmts.size(), 1u);
  EXPECT_EQ(caller_stmts[0].ToString(), "func_80080000(f12, f14);\n");
}

TEST(CallLifterTest, AssignsV0ReturnWhenUsedAfterSubroutineCall) {
  // jal  func_80110410
  // andi $s0, $s0, 0xFFFF (delay slot)
  // addu $v1, $s1, $zero
  // andi $v0, $v0, 0xFFFF (uses $v0 from call)
  std::vector<uint32_t> words = {
      0x0C044104,  // 0: jal  func_80110410
      0x3210FFFF,  // 4: andi $s0, $s0, 0xFFFF
      0x02201821,  // 8: addu $v1, $s1, $zero
      0x3042FFFF,  // C: andi $v0, $v0, 0xFFFF
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_GE(stmts.size(), 4u);
  EXPECT_EQ(stmts[0].destination_variable, "temp_s0");
  EXPECT_EQ(stmts[1].destination_variable, "v0");
  EXPECT_EQ(stmts[1].kind, StatementKind::kAssignment);
  EXPECT_EQ(stmts[1].ToString(), "v0 = func_80110410();\n");
}

TEST(CallLifterTest, AssignsF0ReturnWhenUsedAfterSubroutineCall) {
  // jal   sinf
  // nop (delay slot)
  // add.s $f2, $f0, $f4 (uses $f0 from call)
  std::vector<uint32_t> words = {
      0x0C000000,  // 0: jal  func_00000000
      0x00000000,  // 4: nop
      0x46040080,  // 8: add.s $f2, $f0, $f4
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_GE(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].destination_variable, "f0");
  EXPECT_EQ(stmts[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(stmts[0].ToString(), "f0 = func_80000000();\n");
}

TEST(CallLifterTest, AssignsIndirectCallReturnWhenUsed) {
  // jalr $t9
  // nop
  // addu $a0, $v0, $zero (uses $v0)
  std::vector<uint32_t> words = {
      0x0320F809,  // 0: jalr $t9
      0x00000000,  // 4: nop
      0x00402021,  // 8: addu $a0, $v0, $zero
  };

  auto insts = *DecodeSequence(words);
  auto stmts = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_GE(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].destination_variable, "v0");
  EXPECT_EQ(stmts[0].kind, StatementKind::kAssignment);
  EXPECT_EQ(stmts[0].ToString(), "v0 = ((s32 (*)())temp_t9)();\n");
}

TEST(CallLifterTest, LiftBranchAndLink) {
  // bal target (0x04110002 -> bgezal $zero, +2 instructions)
  std::vector<uint32_t> words = {
      0x04110002,  // 0: bal +2 (target = 0x8002000C)
      0x00000000,  // 4: nop
  };

  auto insts = *DecodeSequence(words, 0x80020000);
  auto statements = ExpressionBuilder::LiftInstructions(insts);
  ASSERT_FALSE(statements.empty());
  EXPECT_EQ(statements[0].kind, StatementKind::kCall);
  EXPECT_THAT(statements[0].ToString(), HasSubstr("func_8002000C"));
}

TEST(CallLifterTest, PreCallCallerSavedGprsNotPreservedAcrossCall) {
  // 0: addiu $t0, $zero, 42    # caller-saved $t0
  // 1: addiu $s0, $zero, 100   # callee-saved $s0
  // 2: jal func_80100000
  // 3: nop                     # delay slot
  // 4: addu $v0, $t0, $s0      # uses $t0 (clobbered by call) and $s0 (preserved)
  std::vector<uint32_t> words = {
      0x2408002A,  // addiu $t0, $zero, 42
      0x24100064,  // addiu $s0, $zero, 100
      0x0C040000,  // jal func_80100000
      0x00000000,  // nop
      0x01101021,  // addu $v0, $t0, $s0
  };
  auto insts = *DecodeSequence(words);
  auto statements = ExpressionBuilder::LiftInstructions(insts);

  // $s0 should be recognized as constant 100 across the call, but $t0 must NOT be constant 42
  // across the call since the call clobbered it!
  ASSERT_GE(statements.size(), 4u);
  // Last statement: addu $v0, $t0, $s0
  const auto& last_stmt = statements.back();
  EXPECT_EQ(last_stmt.destination_variable, "v0");
  EXPECT_EQ(last_stmt.ToString(), "v0 = temp_t0 + 100;\n");
}

TEST(CallLifterTest, InPlaceEntryArg1DetectedWhenArg0DefinedAndArg2SavedInPrologue) {
  // 00: addiu $sp, $sp, -32
  // 04: sw    $s0, 16($sp)
  // 08: addu  $s0, $a0, $zero
  // 0C: sw    $ra, 28($sp)
  // 10: lw    $a0, 12($s0)
  // 14: addu  $s1, $a2, $zero   # $a2 saved in $s1 before call; $a1 passed in-place
  // 18: jal   0x80001000
  // 1C: nop
  std::vector<uint32_t> words = {
      0x27BDFFE0,  // addiu $sp, $sp, -32
      0xAFB00010,  // sw    $s0, 16($sp)
      0x00808021,  // addu  $s0, $a0, $zero
      0xAFBF001C,  // sw    $ra, 28($sp)
      0x8E04000C,  // lw    $a0, 12($s0)
      0x00C08821,  // addu  $s1, $a2, $zero
      0x0C000400,  // jal   0x80001000
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  absl::flat_hash_map<Register, int32_t> saved_regs = {{Register::kS0, 16}};
  FunctionLiftContext ctx;
  ctx.frame_size = 32;
  ctx.has_calls = true;
  ctx.saved_reg_offsets = &saved_regs;
  ctx.cfg = &cfg;
  ctx.current_block_id = cfg.EntryBlockId();

  auto stmts = ExpressionBuilder::LiftInstructions(insts, ctx);
  ASSERT_FALSE(stmts.empty());
  EXPECT_EQ(stmts.back().ToString(), "func_80001000(arg0, arg1);\n");
}

TEST(CallLifterTest, PredecessorCallDelaySlotDoesNotLeakIntoSuccessorCallArguments) {
  // Block 0:
  //   00: beq   $a0, $zero, 0x80000018
  //   04: nop
  // Block 1 (fallthrough):
  //   08: jal   0x80001000
  //   0C: addiu $a1, $zero, 10   # delay slot of jal 0x80001000!
  //   10: j     0x80000018
  //   14: nop
  // Block 2 (join at 0x80000018):
  //   18: addiu $a0, $zero, 1
  //   1C: jal   0x80002000       # should take only 1 arg ($a0), NOT $a1 from Block 1's jal!
  //   20: nop
  //   24: jr    $ra
  //   28: nop
  std::vector<uint32_t> words = {
      0x10800005,  // 00: beq   $a0, $zero, 0x80000018
      0x00000000,  // 04: nop
      0x0C000400,  // 08: jal   0x80001000
      0x2405000A,  // 0C: addiu $a1, $zero, 10
      0x08000006,  // 10: j     0x80000018
      0x00000000,  // 14: nop
      0x24040001,  // 18: addiu $a0, $zero, 1
      0x0C000800,  // 1C: jal   0x80002000
      0x00000000,  // 20: nop
      0x03E00008,  // 24: jr    $ra
      0x00000000,  // 28: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg_or = ControlFlowGraph::Build(insts);
  ASSERT_TRUE(cfg_or.ok());

  const BasicBlock* join_block = nullptr;
  for (const auto& b : cfg_or->Blocks()) {
    if (!b.instructions.empty() && b.instructions.front().vram == 0x80000018) {
      join_block = &b;
      break;
    }
  }
  ASSERT_NE(join_block, nullptr);

  FunctionLiftContext ctx;
  ctx.cfg = &(*cfg_or);
  auto stmts = ExpressionBuilder::LiftBlock(*join_block, ctx, /*omit_last_instruction=*/false);
  ASSERT_GE(stmts.size(), 2u);
  EXPECT_EQ(stmts[1].ToString(), "func_80002000(1);\n");
}

TEST(CallLifterTest, EarlierDoublePrecisionUseOfF12DoesNotOverrideSinglePrecisionCallArgument) {
  // 00: cvt.d.s $f12, $f12
  // 04: mul.d   $f2, $f12, $f12
  // 08: cvt.s.d $f12, $f2       # redefines $f12 as single-precision before call!
  // 0C: jal     0x80001000
  // 10: nop
  std::vector<uint32_t> words = {
      0x46006321,  // 00: cvt.d.s $f12, $f12
      0x462C6082,  // 04: mul.d   $f2, $f12, $f12
      0x46201320,  // 08: cvt.s.d $f12, $f2
      0x0C000400,  // 0C: jal     0x80001000
      0x00000000,  // 10: nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  absl::flat_hash_map<std::string, int> fp_param_counts = {{"func_80001000", 1}};
  FunctionLiftContext ctx;
  ctx.function_fp_parameter_counts = &fp_param_counts;
  auto stmts = ExpressionBuilder::LiftInstructions(insts, ctx);
  ASSERT_FALSE(stmts.empty());
  EXPECT_EQ(stmts.back().ToString(), "func_80001000(f12);\n");
}

}  // namespace
}  // namespace rom_nom_nom
