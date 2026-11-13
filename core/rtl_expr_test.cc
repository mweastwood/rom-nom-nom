#include "core/rtl_expr.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(RtlExprTest, LosslessRoundTripAcrossGcc272Constructs) {
  const std::vector<std::string> constructs = {
      "(nil)",
      "(pc)",
      "(cc0)",
      "(return)",
      "(scratch:SI)",
      "(const_int 0)",
      "(const_int -32768)",
      "(const_double:DF (mem/u:DF (symbol_ref:SI (\"$LC0\"))) 0 1072693248 0)",
      "(const_string \"hello\\nworld\")",
      "(reg:SI 0 $0)",
      "(reg:SI 2 v0)",
      "(reg:SI 4 a0)",
      "(reg:SI 29 sp)",
      "(reg:SI 31 ra)",
      "(reg:DF 32 $f0)",
      "(reg:SI 64 hi)",
      "(reg:SI 65 lo)",
      "(reg:SI 66 accum)",
      "(reg:SI 72)",
      "(reg/v:SI 73)",
      "(reg/v/u:DI 74)",
      "(subreg:SI (reg:DI 72) 1)",
      "(subreg/s:HI (reg/v:SI 75) 0)",
      "(strict_low_part (subreg:HI (reg:SI 72) 0))",
      "(mem:SI (plus:SI (reg:SI 29 sp) (const_int 16)))",
      "(mem/s:HI (reg:SI 4 a0))",
      "(mem/v:SI (const_int -1538785280))",
      "(mem/s/u:QI (plus:SI (reg:SI 28 gp) (const_int -32752)))",
      "(symbol_ref:SI (\"g_counter\"))",
      "(symbol_ref:SI \"bare_sym\")",
      "(symbol_ref/v:SI (\"func_ptr\"))",
      "(label_ref 42)",
      "(label_ref:SI 42)",
      "(label_ref L0)",
      "(set (pc) (label_ref L2))",
      "(sequence [(jump_insn (return)) (insn (set (reg/i:SI 2 v0) (reg:SI 4 a0)))])",
      "(note NOTE_INSN_LOOP_BEG)",
      "(const:SI (plus:SI (symbol_ref:SI (\"arr\")) (const_int 12)))",
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
      "(set (reg:SI 72) (minus:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 73) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 74) (div:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 75) (udiv:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 76) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 77) (umod:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 78) (smin:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 79) (smax:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 80) (umin:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 81) (umax:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 82) (and:SI (reg:SI 4 a0) (const_int 255)))",
      "(set (reg:SI 83) (ior:SI (reg:SI 4 a0) (const_int 1)))",
      "(set (reg:SI 84) (xor:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 85) (not:SI (reg:SI 4 a0)))",
      "(set (reg:SI 86) (neg:SI (reg:SI 4 a0)))",
      "(set (reg:SF 32 $f0) (abs:SF (reg:SF 44 $f12)))",
      "(set (reg:SF 32 $f0) (sqrt:SF (reg:SF 44 $f12)))",
      "(set (reg:SI 87) (ffs:SI (reg:SI 4 a0)))",
      "(set (reg:SI 88) (ashift:SI (reg:SI 4 a0) (const_int 4)))",
      "(set (reg:SI 89) (ashiftrt:SI (reg:SI 4 a0) (const_int 2)))",
      "(set (reg:SI 90) (lshiftrt:SI (reg:SI 4 a0) (const_int 2)))",
      "(set (reg:SI 91) (rotate:SI (reg:SI 4 a0) (const_int 8)))",
      "(set (reg:SI 92) (rotatert:SI (reg:SI 4 a0) (const_int 8)))",
      "(set (reg:SI 93) (high:SI (symbol_ref:SI (\"g_state\"))))",
      "(set (reg:SI 94) (lo_sum:SI (reg:SI 93) (symbol_ref:SI (\"g_state\"))))",
      "(set (reg:SI 95) (sign_extend:SI (mem:HI (reg:SI 4 a0))))",
      "(set (reg:SI 96) (zero_extend:SI (mem:QI (reg:SI 4 a0))))",
      "(set (reg:HI 97) (truncate:HI (reg:SI 4 a0)))",
      "(set (reg:DF 32 $f0) (float_extend:DF (reg:SF 44 $f12)))",
      "(set (reg:SF 32 $f0) (float_truncate:SF (reg:DF 44 $f12)))",
      "(set (reg:SF 32 $f0) (float:SF (reg:SI 4 a0)))",
      "(set (reg:DF 32 $f0) (unsigned_float:DF (reg:SI 4 a0)))",
      "(set (reg:SI 2 v0) (fix:SI (reg:SF 44 $f12)))",
      "(set (reg:SI 2 v0) (unsigned_fix:SI (reg:DF 44 $f12)))",
      "(set (reg:SI 98) (sign_extract:SI (reg:SI 4 a0) (const_int 8) "
      "(const_int 16)))",
      "(set (reg:SI 99) (zero_extract:SI (reg:SI 4 a0) (const_int 4) "
      "(const_int 0)))",
      "(set (pc) (if_then_else (eq (reg:SI 4 a0) (const_int 0)) (label_ref 25) "
      "(pc)))",
      "(set (reg:SI 100) (ltu:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
      "(set (reg:SI 101) (geu:SI (reg:SI 4 a0) (const_int 10)))",
      "(cond [(eq (reg:SI 4 a0) (const_int 0)) (const_int 1)] (const_int 2))",
      "(parallel [(set (reg:SI 2 v0) (div:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
      "(set (reg:SI 3 v1) (mod:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
      "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo))])",
      "(sequence [(insn 10 9 11 (set (reg:SI 2 v0) (const_int 0)) -1 (nil) "
      "(nil)) (jump_insn 11 10 12 (return) -1 (nil) (nil))])",
      "(use (reg:SI 31 ra))",
      "(clobber (scratch:SI))",
      "(call (mem:SI (symbol_ref:SI (\"helper\"))) (const_int 16))",
      "(addr_vec:SI [(label_ref 10) (label_ref 11) (label_ref 12)])",
      "(addr_diff_vec:HI (label_ref 10) [(label_ref 11) (label_ref 12)] "
      "(const_int 0) (const_int 0))",
      "(asm_input \"nop\")",
      "(asm_operands:SI \"mfc0 %0,$12\" \"=r\" 0 [] [])",
      "(unspec:SI [(reg:SI 4 a0)] 2)",
      "(unspec_volatile:SI [(const_int 0)] 1)",
      "(unaligned_load:SI (mem:BLK (reg:SI 4 a0)))",
      "(unaligned_store:BLK (mem:BLK (reg:SI 4 a0)) (reg:SI 5 a1))",
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_EQUAL (const_int 0) "
      "(nil)))",
      "(insn_list 12 (insn_list:REG_DEP_ANTI 8 (nil)))",
      "(code_label 15 14 16 2 \"\" [0 uses])",
      "(barrier 17 16 18)",
      "(note 5 4 6 \"\" NOTE_INSN_FUNCTION_BEG)",
  };

  for (const std::string& sexpr : constructs) {
    auto parsed = RtlExpr::Parse(sexpr);
    ASSERT_TRUE(parsed.ok()) << "Failed on: " << sexpr << " (" << parsed.status().message() << ")";
    EXPECT_EQ(parsed->ToSexprString(), sexpr);
  }
}

TEST(RtlExprTest, BuildersAndRegisterRewriting) {
  RtlExpr expr = RtlExpr::MakeSet(
      RtlExpr::MakeReg(RtlMachineMode::kSI, 72),
      RtlExpr::MakeBinary(
          RtlOp::kPlus, RtlMachineMode::kSI,
          RtlExpr::MakeMem(RtlMachineMode::kSI,
                           RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSI,
                                               RtlExpr::MakeReg(RtlMachineMode::kSI, 73),
                                               RtlExpr::MakeConstInt(16)),
                           RtlFlags{.in_struct = true}),
          RtlExpr::MakeReg(RtlMachineMode::kSI, 4)));

  EXPECT_EQ(expr.ToSexprString(),
            "(set (reg:SI 72) (plus:SI (mem/s:SI (plus:SI (reg:SI 73) "
            "(const_int 16))) (reg:SI 4 a0)))");
  EXPECT_TRUE(expr.IsSet());
  EXPECT_TRUE(expr.SetDest().IsPseudoReg());
  EXPECT_FALSE(expr.SetDest().IsHardReg());
  EXPECT_TRUE(expr.ContainsReg(72));
  EXPECT_TRUE(expr.ContainsReg(73));
  EXPECT_TRUE(expr.ContainsReg(4));
  EXPECT_FALSE(expr.ContainsReg(2));

  // Replace pseudo 72 -> hard reg 2 (v0) and hard reg 4 (a0) -> pseudo 80.
  expr.ReplaceRegNo(72, 2);
  expr.ReplaceRegNo(4, 80);

  EXPECT_TRUE(expr.SetDest().IsHardReg());
  EXPECT_FALSE(expr.SetDest().IsPseudoReg());
  EXPECT_EQ(expr.SetDest().RegNo(), 2);
  EXPECT_EQ(expr.ToSexprString(),
            "(set (reg:SI 2 v0) (plus:SI (mem/s:SI (plus:SI (reg:SI 73) "
            "(const_int 16))) (reg:SI 80)))");
}

TEST(RtlExprTest, ParseAndFormatPatternWithRegNotes) {
  const std::string insn_pattern =
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
      "[REG_NOTES: (expr_list:REG_DEAD (reg:SI 4 a0) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) "
      "(expr_list:REG_EQUAL (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)) (nil))))]";

  auto parsed = ParsePatternAndNotes(insn_pattern);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  ASSERT_EQ(parsed->reg_notes.size(), 3u);
  EXPECT_EQ(parsed->reg_notes[0].kind, RtlMachineMode::kRegDead);
  EXPECT_EQ(parsed->reg_notes[0].datum.RegNo(), 4);
  EXPECT_EQ(parsed->reg_notes[1].kind, RtlMachineMode::kRegDead);
  EXPECT_EQ(parsed->reg_notes[1].datum.RegNo(), 5);
  EXPECT_EQ(parsed->reg_notes[2].kind, RtlMachineMode::kRegEqual);
  EXPECT_EQ(parsed->ToSexprString(), insn_pattern);

  // Rewriting reg 4 -> pseudo 72 updates both the pattern and the REG_NOTES.
  parsed->ReplaceRegNo(4, 72);
  EXPECT_EQ(parsed->ToSexprString(),
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 72) (reg:SI 5 a1))) "
            "[REG_NOTES: (expr_list:REG_DEAD (reg:SI 72) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) "
            "(expr_list:REG_EQUAL (plus:SI (reg:SI 72) (reg:SI 5 a1)) (nil))))]");

  // Also test space-separated trailing REG_NOTES (used in RtlBasicBlock::instructions).
  const std::string space_notes_pattern =
      "(set (reg:SI 2 v0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))";
  auto space_parsed = ParsePatternAndNotes(space_notes_pattern);
  ASSERT_TRUE(space_parsed.ok()) << space_parsed.status().message();
  ASSERT_EQ(space_parsed->reg_notes.size(), 1u);
  EXPECT_FALSE(space_parsed->bracketed_notes);
  space_parsed->ReplaceRegNo(2, 72);
  EXPECT_EQ(space_parsed->ToSexprString(),
            "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
}

TEST(RtlExprTest, RejectsMalformedSexpr) {
  EXPECT_FALSE(RtlExpr::Parse("").ok());
  EXPECT_FALSE(RtlExpr::Parse("(set (reg:SI 2 v0)").ok());
  EXPECT_FALSE(RtlExpr::Parse("(const_int not_a_number)").ok());
  EXPECT_FALSE(RtlExpr::Parse("(reg:SI 2 v0) trailing").ok());
  EXPECT_FALSE(ParsePatternAndNotes("(set (reg:SI 2 v0) (const_int 1)) [REG_NOTES: (nil)").ok());
}

TEST(RtlExprTest, DisambiguatesArgPointerRegFromHardwareZeroReg) {
  const RtlExpr zero_reg = RtlExpr::MakeReg(RtlMachineMode::kSI, 0);
  const RtlExpr argp_reg = RtlExpr::MakeArgPointerReg(RtlMachineMode::kSI);

  EXPECT_EQ(zero_reg.ToSexprString(), "(reg:SI 0 $0)");
  EXPECT_EQ(argp_reg.ToSexprString(), "(reg:SI 0 argp)");
  EXPECT_NE(zero_reg, argp_reg);

  EXPECT_TRUE(zero_reg.IsHardReg());
  EXPECT_FALSE(zero_reg.IsArgPointerReg());
  EXPECT_TRUE(zero_reg.ContainsReg(0));

  EXPECT_FALSE(argp_reg.IsHardReg());
  EXPECT_TRUE(argp_reg.IsArgPointerReg());
  EXPECT_FALSE(argp_reg.ContainsReg(0));

  auto parsed_argp = RtlExpr::Parse("(reg:SI 0 argp)");
  ASSERT_TRUE(parsed_argp.ok());
  EXPECT_TRUE(parsed_argp->IsArgPointerReg());
  EXPECT_FALSE(parsed_argp->IsHardReg());
  EXPECT_EQ(*parsed_argp, argp_reg);

  auto parsed_dollar_arg = RtlExpr::Parse("(reg:SI 0 $arg)");
  ASSERT_TRUE(parsed_dollar_arg.ok());
  EXPECT_TRUE(parsed_dollar_arg->IsArgPointerReg());
  EXPECT_FALSE(parsed_dollar_arg->IsHardReg());

  // ReplaceRegNo(0, 72) rewrites hardware $0 without touching argp.
  RtlExpr sum =
      RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kSI, 2),
                       RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSI, argp_reg, zero_reg));
  sum.ReplaceRegNo(0, 72);
  EXPECT_EQ(sum.ToSexprString(), "(set (reg:SI 2 v0) (plus:SI (reg:SI 0 argp) (reg:SI 72)))");
}

TEST(RtlExprTest, GccPrintRtlFormattingAndSequenceSubInsnAstHelpers) {
  RtlExpr ret_par = RtlExpr::MakeParallel({
      RtlExpr::MakeReturn(),
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSI, 31)),
  });
  EXPECT_EQ(ret_par.ToSexprString(), "(parallel [(return) (use (reg:SI 31 ra))])");
  EXPECT_EQ(ret_par.ToGccPrintRtlString(), "(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  RtlPatternAndNotes slot_pn;
  slot_pn.pattern = RtlExpr::MakeSet(
      RtlExpr::MakeReg(RtlMachineMode::kSI, 2, RtlFlags{.integrated = true}),
      RtlExpr::MakeBinary(RtlOp::kPlus, RtlMachineMode::kSI,
                          RtlExpr::MakeReg(RtlMachineMode::kSI, 4), RtlExpr::MakeConstInt(1)));
  slot_pn.reg_notes.push_back(RtlRegNote{
      .kind = RtlMachineMode::kRegDead,
      .list_op = RtlOp::kExprList,
      .datum = RtlExpr::MakeReg(RtlMachineMode::kSI, 4),
  });

  RtlExpr seq = RtlExpr::MakeSequence({
      RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, RtlFlags{.unchanging = true}, ret_par),
      RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{.in_struct = true}, slot_pn),
  });
  EXPECT_EQ(seq.ToGccPrintRtlString(),
            "(sequence [(jump_insn/u (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
            "(insn/s (set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil)))])");

  auto reparsed_seq = RtlExpr::Parse(seq.ToGccPrintRtlString());
  ASSERT_TRUE(reparsed_seq.ok()) << reparsed_seq.status().message();
  ASSERT_EQ(reparsed_seq->children.size(), 2u);

  auto head_extracted = RtlPatternAndNotes::FromSequenceSubInsn(reparsed_seq->children[0]);
  ASSERT_TRUE(head_extracted.ok()) << head_extracted.status().message();
  EXPECT_EQ(head_extracted->pattern, ret_par);
  EXPECT_TRUE(head_extracted->reg_notes.empty());
  EXPECT_EQ(head_extracted->ToGccPrintRtlString(), "(parallel[ (return) (use (reg:SI 31 ra)) ] )");

  auto slot_extracted = RtlPatternAndNotes::FromSequenceSubInsn(reparsed_seq->children[1]);
  ASSERT_TRUE(slot_extracted.ok()) << slot_extracted.status().message();
  EXPECT_EQ(*slot_extracted, slot_pn);

  // Also test `ToGccPrintRtlString` on top-level `PARALLEL` with trailing `REG_NOTES`.
  RtlPatternAndNotes call_pn;
  call_pn.pattern = RtlExpr::MakeParallel({
      RtlExpr::MakeSet(
          RtlExpr::MakeReg(RtlMachineMode::kSI, 2),
          RtlExpr::MakeCall(RtlExpr::MakeMem(RtlMachineMode::kSI,
                                             RtlExpr::MakeSymbolRef(RtlMachineMode::kSI, "callee")),
                            RtlExpr::MakeConstInt(0))),
      RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSI, 31)),
  });
  call_pn.reg_notes.push_back(RtlRegNote{
      .kind = RtlMachineMode::kRegDead,
      .list_op = RtlOp::kExprList,
      .datum = RtlExpr::MakeReg(RtlMachineMode::kSI, 4),
  });
  EXPECT_EQ(call_pn.ToGccPrintRtlString(),
            "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
            "(const_int 0))) (clobber (reg:SI 31 ra)) ] ) "
            "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");

  // Full GCC dump sub-insn with UIDs and `REG_NOTES`.
  auto full_sub = RtlExpr::Parse(
      "(insn 10 9 11 (set (reg:SI 2 v0) (const_int 0)) -1 (nil) "
      "(expr_list:REG_UNUSED (reg:SI 2 v0) (nil)))");
  ASSERT_TRUE(full_sub.ok());
  auto full_extracted = RtlPatternAndNotes::FromSequenceSubInsn(*full_sub);
  ASSERT_TRUE(full_extracted.ok());
  ASSERT_EQ(full_extracted->reg_notes.size(), 1u);
  EXPECT_EQ(full_extracted->reg_notes[0].kind, RtlMachineMode::kRegUnused);

  // Negative cases for `FromSequenceSubInsn`.
  EXPECT_FALSE(RtlPatternAndNotes::FromSequenceSubInsn(ret_par).ok());
  RtlExpr empty_insn;
  empty_insn.op = RtlOp::kInsn;
  EXPECT_FALSE(RtlPatternAndNotes::FromSequenceSubInsn(empty_insn).ok());

  // `MakeNote` builder round-trip.
  const RtlExpr vtop_note = RtlExpr::MakeNote("NOTE_INSN_LOOP_VTOP");
  EXPECT_EQ(vtop_note.ToGccPrintRtlString(), "(note NOTE_INSN_LOOP_VTOP)");
  auto parsed_vtop_note = RtlExpr::Parse(vtop_note.ToGccPrintRtlString());
  ASSERT_TRUE(parsed_vtop_note.ok()) << parsed_vtop_note.status().message();
  EXPECT_EQ(*parsed_vtop_note, vtop_note);
}

TEST(RtlExprTest, ParsesAndFormatsUidElidedInsnListNotesLosslessly) {
  const std::string pat_text =
      "(set (reg:SI 2 v0) (const_int 0)) "
      "(insn_list:REG_WAS_0 (insn_list:REG_LIBCALL (expr_list:REG_EQUAL (const_int 0) (nil))))";
  auto parsed = ParsePatternAndNotes(pat_text);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  ASSERT_EQ(parsed->reg_notes.size(), 3u);
  EXPECT_EQ(parsed->reg_notes[0].list_op, RtlOp::kInsnList);
  EXPECT_EQ(parsed->reg_notes[0].insn_id, -1);
  EXPECT_TRUE(parsed->reg_notes[0].datum.IsNil());
  EXPECT_EQ(parsed->reg_notes[1].list_op, RtlOp::kInsnList);
  EXPECT_EQ(parsed->reg_notes[1].insn_id, -1);
  EXPECT_EQ(parsed->reg_notes[2].list_op, RtlOp::kExprList);
  EXPECT_EQ(parsed->ToSexprString(), pat_text);
  EXPECT_EQ(parsed->ToGccPrintRtlString(), pat_text);
}

TEST(RtlExprTest, CallFusageAndFrameRelatedFlagsRoundTripInTopLevelAndSequence) {
  auto frame_reg = RtlExpr::Parse("(reg/f/v/u:SI 72)");
  ASSERT_TRUE(frame_reg.ok()) << frame_reg.status().message();
  EXPECT_TRUE(frame_reg->flags.frame_related);
  EXPECT_TRUE(frame_reg->flags.volatil);
  EXPECT_TRUE(frame_reg->flags.unchanging);
  EXPECT_EQ(frame_reg->ToGccPrintRtlString(), "(reg/v/u/f:SI 72)");

  auto empty_fusage = ParseCallFusageChainSexpr("(nil)");
  ASSERT_TRUE(empty_fusage.ok()) << empty_fusage.status().message();
  EXPECT_TRUE(empty_fusage->empty());
  EXPECT_EQ(BuildCallFusageChainExpr(*empty_fusage).ToGccPrintRtlString(), "(nil)");

  const std::string fusage_text =
      "(expr_list (use (reg:SI 5 a1)) (expr_list (use (reg:SI 4 a0)) (nil)))";
  auto parsed_fusage = ParseCallFusageChainSexpr(fusage_text);
  ASSERT_TRUE(parsed_fusage.ok()) << parsed_fusage.status().message();
  ASSERT_EQ(parsed_fusage->size(), 2u);
  EXPECT_EQ(BuildCallFusageChainExpr(*parsed_fusage).ToGccPrintRtlString(), fusage_text);

  const std::string call_with_notes_and_fusage =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))) " +
      fusage_text;
  auto parsed_call = ParsePatternAndNotes(call_with_notes_and_fusage);
  ASSERT_TRUE(parsed_call.ok()) << parsed_call.status().message();
  EXPECT_TRUE(parsed_call->has_explicit_call_fusage);
  ASSERT_EQ(parsed_call->reg_notes.size(), 2u);
  ASSERT_EQ(parsed_call->call_fusage.size(), 2u);

  parsed_call->ReplaceRegNo(4, 6);
  EXPECT_EQ(parsed_call->call_fusage[1].children[0].RegNo(), 6);

  RtlPatternAndNotes slot_pn;
  slot_pn.pattern =
      RtlExpr::MakeSet(RtlExpr::MakeReg(RtlMachineMode::kSI, 6), RtlExpr::MakeConstInt(9));
  RtlExpr seq = RtlExpr::MakeSequence({
      RtlExpr::MakeInsnWrapper(RtlOp::kCallInsn, RtlFlags{}, *parsed_call),
      RtlExpr::MakeInsnWrapper(RtlOp::kInsn, RtlFlags{}, slot_pn),
  });
  auto reparsed_seq = RtlExpr::Parse(seq.ToGccPrintRtlString());
  ASSERT_TRUE(reparsed_seq.ok()) << reparsed_seq.status().message();
  ASSERT_EQ(reparsed_seq->children.size(), 2u);
  auto extracted_call = RtlPatternAndNotes::FromSequenceSubInsn(reparsed_seq->children[0]);
  ASSERT_TRUE(extracted_call.ok()) << extracted_call.status().message();
  EXPECT_TRUE(extracted_call->has_explicit_call_fusage);
  EXPECT_EQ(extracted_call->call_fusage, parsed_call->call_fusage);
  EXPECT_EQ(extracted_call->reg_notes, parsed_call->reg_notes);
}

}  // namespace
}  // namespace rom_nom_nom
