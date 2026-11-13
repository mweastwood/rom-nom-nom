#include "core/rtl_sexpr_utils.h"

#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(RtlSexprUtilsTest, LexingAndBalancedParenHelpersHandleQuotesAndBrackets) {
  std::string_view text =
      "  (insn/s:SI 12 11 13 (symbol_ref:SI (\"foo(bar)\")) [ (use (reg:SI 4)) (clobber (reg:SI "
      "1)) ])";
  size_t pos = 0;
  EXPECT_EQ(ReadRtlToken(text, &pos), "");
  ASSERT_LT(pos, text.size());
  EXPECT_EQ(text[pos], '(');

  const size_t close_pos = FindMatchingCloseParen(text, pos);
  EXPECT_EQ(close_pos, text.size() - 1);

  ++pos;
  EXPECT_EQ(ReadRtlToken(text, &pos), "insn");
  EXPECT_EQ(ExtractInsnTagWithFlags(text), "insn/s:SI");

  const size_t bracket_pos = text.find('[');
  ASSERT_NE(bracket_pos, std::string_view::npos);
  const std::vector<std::string_view> elems = ExtractBracketedSexprElements(text, bracket_pos);
  ASSERT_EQ(elems.size(), 2u);
  EXPECT_EQ(elems[0], "(use (reg:SI 4))");
  EXPECT_EQ(elems[1], "(clobber (reg:SI 1))");
}

TEST(RtlSexprUtilsTest, PatternAndRegNoteExtractionAndSplitting) {
  constexpr std::string_view kInsnSexpr =
      "(insn 14 13 15 (set (reg:SI 2 v0)\n"
      "        (plus:SI (reg:SI 72)\n"
      "            (const_int 1))) 12 {addsi3_internal} (insn_list 13 (nil))\n"
      "    (expr_list:REG_DEAD (reg:SI 72)\n"
      "        (nil)))";

  EXPECT_EQ(CanonicalizeRtlPattern(ExtractTopLevelPatternFromInsnSexpr(kInsnSexpr)),
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 72) (const_int 1)))");
  EXPECT_EQ(CanonicalizeRtlPattern(ExtractRegNotesFromInsnSexpr(kInsnSexpr)),
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");

  const absl::flat_hash_map<int, std::string> empty_labels;
  const std::string combined = ExtractPatternAndNotesFromInsnSexpr(kInsnSexpr, empty_labels);
  EXPECT_EQ(combined,
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 72) (const_int 1))) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");

  const auto [main_pat, notes_pat] = SplitPatternAndTrailingRegNotes(combined);
  EXPECT_EQ(main_pat, "(set (reg:SI 2 v0) (plus:SI (reg:SI 72) (const_int 1)))");
  EXPECT_EQ(notes_pat, "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  EXPECT_EQ(StripTrailingRegNotesFromPattern(combined),
            "(set (reg:SI 2 v0) (plus:SI (reg:SI 72) (const_int 1)))");
}

TEST(RtlSexprUtilsTest, SequenceExtractionAndBranchInversion) {
  constexpr std::string_view kSeqPat =
      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
      "(label_ref L1) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 7)))])";

  std::string jump_pat;
  std::string delay_tag;
  std::string delay_pat;
  ASSERT_TRUE(ExtractOuterSequenceParts(kSeqPat, &jump_pat, &delay_tag, &delay_pat));
  EXPECT_EQ(jump_pat,
            "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L1) (pc)))");
  EXPECT_EQ(delay_tag, "insn/s");
  EXPECT_EQ(delay_pat, "(set (reg:SI 2 v0) (const_int 7))");

  EXPECT_EQ(InvertBranchConditionInRtlPattern(jump_pat),
            "(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 0)) (label_ref L1) (pc)))");
  EXPECT_TRUE(IsJumpPatternText(jump_pat));
  EXPECT_FALSE(IsUnconditionalJumpOrReturnPatternText(jump_pat));
  EXPECT_TRUE(IsUnconditionalJumpOrReturnPatternText("(set (pc) (label_ref L1))"));
  EXPECT_TRUE(IsUnconditionalJumpOrReturnPatternText("(return)"));
  EXPECT_TRUE(
      IsCallPatternText("(parallel [(call (mem:SI (symbol_ref:SI (\"f\"))) (const_int 0)) "
                        "(clobber (reg:SI 31 ra))])"));
}

TEST(RtlSexprUtilsTest, ParseRtlPassDumpAndBuildCanonicalCfgWithTableJumpAndLoopNotes) {
  constexpr std::string_view kRawDump = R"(
;; Function test_fn

(note 1 0 2 "" NOTE_INSN_FUNCTION_BEG)

(note 2 1 3 "" NOTE_INSN_LOOP_BEG)

(code_label 10 2 11 2 "" [num_refs: 1])

(insn 11 10 12 (set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) -1 (nil) (nil))

(jump_insn 12 11 13 (set (pc) (if_then_else (ne:SI (reg:SI 72) (const_int 10)) (label_ref 10) (pc))) -1 (nil) (nil))

(jump_insn 13 12 14 (return) -1 (nil) (nil))

(barrier 14 13 0)
)";

  const RtlPassDump pass_dump = ParseRtlPassDump(RtlPassKind::kLoop, kRawDump);
  EXPECT_EQ(pass_dump.Pass(), RtlPassKind::kLoop);
  const RtlFunctionDump* fn_dump = pass_dump.FindFunction("test_fn");
  ASSERT_NE(fn_dump, nullptr);
  EXPECT_TRUE(fn_dump->HasLoopNotes());
  EXPECT_EQ(fn_dump->ActiveInsnCount(), 3u);

  const RtlControlFlowGraph cfg = BuildCanonicalCfgFromRtlDump(*fn_dump);
  EXPECT_EQ(cfg.pass, RtlPassKind::kLoop);
  ASSERT_EQ(cfg.rpo_blocks.size(), 3u);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions, std::vector<std::string>{"(note NOTE_INSN_LOOP_BEG)"});
  EXPECT_EQ(cfg.rpo_blocks[0].successors, std::vector<size_t>{1});
  EXPECT_EQ(cfg.rpo_blocks[1].successors, (std::vector<size_t>{1, 2}));
  EXPECT_EQ(cfg.rpo_blocks[2].instructions, std::vector<std::string>{"(return)"});
  EXPECT_TRUE(cfg.rpo_blocks[2].successors.empty());
  EXPECT_EQ(MaxPseudoRegInCfg(cfg), 72);
}

TEST(RtlSexprUtilsTest, MipsRegisterFormattingAndParsing) {
  EXPECT_EQ(MipsGprRtlName(0), "$0");
  EXPECT_EQ(MipsGprRtlName(2), "v0");
  EXPECT_EQ(MipsGprRtlName(29), "sp");
  EXPECT_EQ(MipsGprRtlName(31), "ra");
  EXPECT_EQ(MipsGprRtlName(72), "");

  EXPECT_EQ(FormatHardRegRtl(2), "(reg:SI 2 v0)");
  EXPECT_EQ(FormatHardRegRtl(2, /*integrated=*/true), "(reg/i:SI 2 v0)");
  EXPECT_EQ(FormatHardRegRtl(72), "(reg:SI 72)");

  EXPECT_EQ(ParseMipsAsmRegNumber("$0"), 0);
  EXPECT_EQ(ParseMipsAsmRegNumber("$zero"), 0);
  EXPECT_EQ(ParseMipsAsmRegNumber("$v0"), 2);
  EXPECT_EQ(ParseMipsAsmRegNumber("$4"), 4);
  EXPECT_EQ(ParseMipsAsmRegNumber("$sp"), 29);
  EXPECT_EQ(ParseMipsAsmRegNumber("$fp"), 30);
  EXPECT_EQ(ParseMipsAsmRegNumber("$s8"), 30);
  EXPECT_EQ(ParseMipsAsmRegNumber("$ra"), 31);
  EXPECT_EQ(ParseMipsAsmRegNumber("invalid"), -1);
}

TEST(RtlSexprUtilsTest, ConsecutiveCodeLabelsCoalesceExceptAcrossLoopNotesOrJumpTables) {
  constexpr std::string_view kRawDump = R"(
;; Function label_coalesce_fn

(note 1 0 2 "" NOTE_INSN_FUNCTION_BEG)

(jump_insn 2 1 3 (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref 10) (pc))) -1 (nil) (nil))

(jump_insn 3 2 4 (set (pc) (label_ref 11)) -1 (nil) (nil))

(barrier 4 3 10)

(code_label 10 4 5 2 "" [num_refs: 1])

(note 5 10 11 "" NOTE_INSN_DELETED)

(code_label 11 5 12 3 "" [num_refs: 1])

(insn 12 11 20 (set (reg:SI 2 v0) (const_int 7)) -1 (nil) (nil))

(code_label 20 12 21 4 "" [num_refs: 1])

(note 21 20 22 "" NOTE_INSN_LOOP_BEG)

(code_label 22 21 23 5 "" [num_refs: 1])

(jump_insn 23 22 24 (set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 1)) (label_ref 22) (pc))) -1 (nil) (nil))

(note 24 23 30 "" NOTE_INSN_LOOP_END)

(jump_insn 30 24 31 (parallel[ (set (pc) (reg:SI 2 v0)) (use (label_ref 32)) ] ) -1 (nil) (nil))

(barrier 31 30 32)

(code_label 32 31 33 6 "" [num_refs: 1])

(jump_insn 33 32 34 (addr_vec:SI [(label_ref 10) (label_ref 35)]) -1 (nil) (nil))

(barrier 34 33 35)

(code_label 35 34 36 7 "" [num_refs: 1])

(code_label 36 35 37 8 "" [num_refs: 1])

(jump_insn 37 36 38 (addr_diff_vec:HI (label_ref 36) [(label_ref 10) (label_ref 20)] (const_int 0) (const_int 0)) -1 (nil) (nil))

(barrier 38 37 39)

(code_label 39 38 40 9 "" [num_refs: 1])

(jump_insn 40 39 41 (return) -1 (nil) (nil))

(barrier 41 40 0)
)";

  const RtlPassDump pass_dump = ParseRtlPassDump(RtlPassKind::kJump2, kRawDump);
  const RtlFunctionDump* fn_dump = pass_dump.FindFunction("label_coalesce_fn");
  ASSERT_NE(fn_dump, nullptr);

  const absl::flat_hash_map<int, std::string> label_map = BuildSequentialLabelMap(*fn_dump);
  // Adjacent `code_label`s 10 and 11 (separated only by non-loop `NOTE_INSN_DELETED`)
  // coalesce onto the same canonical label `L0`.
  ASSERT_TRUE(label_map.contains(10));
  ASSERT_TRUE(label_map.contains(11));
  EXPECT_EQ(label_map.at(10), "L0");
  EXPECT_EQ(label_map.at(11), "L0");

  // `code_label`s 20 and 22 are separated by `NOTE_INSN_LOOP_BEG`, so they must NOT coalesce.
  ASSERT_TRUE(label_map.contains(20));
  ASSERT_TRUE(label_map.contains(22));
  EXPECT_EQ(label_map.at(20), "L1");
  EXPECT_EQ(label_map.at(22), "L2");
  EXPECT_NE(label_map.at(20), label_map.at(22));

  // `code_label` 32 heads an `addr_vec` jump table, and `code_label` 36 heads an
  // `addr_diff_vec` jump table immediately after `code_label` 35; neither jump-table
  // label may coalesce onto neighboring `code_label`s (35 or 39).
  ASSERT_TRUE(label_map.contains(32));
  ASSERT_TRUE(label_map.contains(35));
  ASSERT_TRUE(label_map.contains(36));
  ASSERT_TRUE(label_map.contains(39));
  EXPECT_EQ(label_map.at(32), "L3");
  EXPECT_EQ(label_map.at(35), "L4");
  EXPECT_EQ(label_map.at(36), "L5");
  EXPECT_EQ(label_map.at(39), "L6");
  EXPECT_NE(label_map.at(35), label_map.at(36));
  EXPECT_NE(label_map.at(36), label_map.at(39));

  // Verify `BuildCanonicalCfgFromRtlDump` rewrites both `(label_ref 10)` and
  // `(label_ref 11)` to `(label_ref L0)`, merges block `L0` into a single RPO block
  // (`rpo_blocks[2]`), and keeps `L1` (`rpo_blocks[3]`, containing `NOTE_INSN_LOOP_BEG`)
  // separate from loop header `L2` (`rpo_blocks[4]`).
  const RtlControlFlowGraph cfg = BuildCanonicalCfgFromRtlDump(*fn_dump);
  ASSERT_GE(cfg.rpo_blocks.size(), 5u);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions.back(),
            "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  EXPECT_EQ(cfg.rpo_blocks[1].instructions.back(), "(set (pc) (label_ref L0))");
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{2, 1}));
  EXPECT_EQ(cfg.rpo_blocks[1].successors, (std::vector<size_t>{2}));
  EXPECT_EQ(cfg.rpo_blocks[2].predecessors, (std::vector<size_t>{0, 1, 5}));
  EXPECT_EQ(cfg.rpo_blocks[2].instructions,
            (std::vector<std::string>{"(set (reg:SI 2 v0) (const_int 7))"}));
  EXPECT_EQ(cfg.rpo_blocks[3].instructions,
            (std::vector<std::string>{"(note NOTE_INSN_LOOP_BEG)"}));
  EXPECT_EQ(cfg.rpo_blocks[4].instructions.front(),
            "(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (const_int 1)) (label_ref L2) (pc)))");
}

TEST(RtlSexprUtilsTest, CanonicalizesInsnListUidsAndMaxPseudoIgnoresTrailingNotes) {
  constexpr std::string_view kRtlDump = R"(
;; Function libcall_uid_test

(insn 14 12 15 (clobber (reg:DI 73)) -1 (nil)
    (insn_list:REG_LIBCALL 16 (nil)))

(insn 16 15 17 (set (reg:DI 73) (reg:DI 2 v0)) -1 (nil)
    (insn_list:REG_RETVAL 14 (expr_list:REG_EQUAL (fix:DI (reg:DF 80)) (nil))))
)";
  const RtlPassDump dump = ParseRtlPassDump(RtlPassKind::kRtl, kRtlDump);
  ASSERT_EQ(dump.Functions().size(), 1u);
  const RtlControlFlowGraph cfg = BuildCanonicalCfgFromRtlDump(dump.Functions()[0]);
  ASSERT_EQ(cfg.rpo_blocks.size(), 1u);
  ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions[0],
            "(clobber (reg:DI 73)) (insn_list:REG_LIBCALL (nil))");
  EXPECT_EQ(cfg.rpo_blocks[0].instructions[1],
            "(set (reg:DI 73) (reg:DI 2 v0)) "
            "(insn_list:REG_RETVAL (expr_list:REG_EQUAL (fix:DI (reg:DF 80)) (nil)))");
  EXPECT_EQ(MaxPseudoRegInCfg(cfg), 73);
}

TEST(RtlSexprUtilsTest, ExtractsAllFourLoopNoteKindsIntoCfgAndRtlInsns) {
  constexpr std::string_view kLoopDump = R"(
;; Function full_loop_notes

(note 1 0 2 "" NOTE_INSN_FUNCTION_BEG)

(note 2 1 3 "" NOTE_INSN_LOOP_BEG)

(code_label 3 2 4 2 "" [num_refs: 1])

(insn 4 3 5 (set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1))) -1 (nil) (nil))

(note 5 4 6 "" NOTE_INSN_LOOP_CONT)

(note 6 5 7 "" NOTE_INSN_LOOP_VTOP)

(jump_insn 7 6 8 (set (pc) (if_then_else (lt:SI (reg:SI 72) (const_int 10)) (label_ref 3) (pc))) -1 (nil) (nil))

(note 8 7 9 "" NOTE_INSN_LOOP_END)

(jump_insn 9 8 10 (return) -1 (nil) (nil))

(barrier 10 9 0)
)";

  const RtlPassDump dump = ParseRtlPassDump(RtlPassKind::kLoop, kLoopDump);
  ASSERT_EQ(dump.Functions().size(), 1u);
  const RtlControlFlowGraph cfg = BuildCanonicalCfgFromRtlDump(dump.Functions()[0]);
  ASSERT_EQ(cfg.rpo_blocks.size(), 3u);
  EXPECT_EQ(cfg.rpo_blocks[0].LoopNotes(),
            (std::vector<RtlLoopNoteKind>{RtlLoopNoteKind::kLoopBeg}));
  EXPECT_EQ(cfg.rpo_blocks[1].LoopNotes(),
            (std::vector<RtlLoopNoteKind>{RtlLoopNoteKind::kLoopCont, RtlLoopNoteKind::kLoopVtop}));
  EXPECT_EQ(cfg.rpo_blocks[2].LoopNotes(),
            (std::vector<RtlLoopNoteKind>{RtlLoopNoteKind::kLoopEnd}));
}

TEST(RtlSexprUtilsTest, SkipsTopLevelInsnDeletedEntriesWhilePreservingUserVarRegs) {
  constexpr std::string_view kO0DeletedDump = R"(
;; Function o0_deleted_entries

(note 1 0 2 "" NOTE_INSN_FUNCTION_BEG)

(insn 2 1 3 (set (reg/v:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1))) -1 (nil) (nil))

(insn/v 3 2 4 (set (reg:SI 73) (const_int 99)) -1 (nil) (nil))

(call_insn/v 4 3 5 (parallel[ (call (mem:SI (symbol_ref:SI ("dead_fn"))) (const_int 16)) (clobber (reg:SI 31 ra)) ] ) -1 (nil) (nil)
    (expr_list (use (reg:SI 4 a0)) (nil)))

(jump_insn 5 4 6 (set (pc) (label_ref 12)) -1 (nil) (nil))

(barrier 6 5 7)

(code_label/v 7 6 8 2 "" [num_refs: 0])

(jump_insn/v 8 7 9 (set (pc) (label_ref 12)) -1 (nil) (nil))

(barrier/v 9 8 12)

(code_label 12 9 13 3 "" [num_refs: 1])

(insn 13 12 14 (set (reg/i:SI 2 v0) (reg/v:SI 72)) -1 (nil) (nil))

(jump_insn 14 13 15 (return) -1 (nil) (nil))

(barrier 15 14 0)
)";

  const RtlPassDump dump = ParseRtlPassDump(RtlPassKind::kJump, kO0DeletedDump);
  const RtlFunctionDump* fn_dump = dump.FindFunction("o0_deleted_entries");
  ASSERT_NE(fn_dump, nullptr);
  EXPECT_EQ(fn_dump->ActiveInsnCount(), 4u);

  const absl::flat_hash_map<int, std::string> label_map = BuildSequentialLabelMap(*fn_dump);
  EXPECT_FALSE(label_map.contains(7));
  ASSERT_TRUE(label_map.contains(12));
  EXPECT_EQ(label_map.at(12), "L0");

  const RtlControlFlowGraph cfg = BuildCanonicalCfgFromRtlDump(*fn_dump);
  ASSERT_EQ(cfg.rpo_blocks.size(), 2u);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions,
            (std::vector<std::string>{
                "(set (reg/v:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                "(set (pc) (label_ref L0))",
            }));
  EXPECT_EQ(cfg.rpo_blocks[0].successors, (std::vector<size_t>{1}));
  EXPECT_EQ(cfg.rpo_blocks[1].instructions, (std::vector<std::string>{
                                                "(set (reg/i:SI 2 v0) (reg/v:SI 72))",
                                                "(return)",
                                            }));
  auto parsed_first = ParsePatternAndNotes(cfg.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(parsed_first.ok());
  EXPECT_TRUE(parsed_first->pattern.SetDest().flags.volatil);
}

}  // namespace
}  // namespace rom_nom_nom
