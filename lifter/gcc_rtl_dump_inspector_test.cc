#include "lifter/gcc_rtl_dump_inspector.h"

#include <string_view>

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(GccRtlDumpInspectorTest, ParsesRawDbrDumpWithSequencesAndNotes) {
  constexpr std::string_view kSampleDbrDump = R"(;; Reorg pass #1:
;; Reorg function #0
;; 2 insns needing delay slots

;; Function AddBranch

(note 2 0 35 "" NOTE_INSN_DELETED)

(note 35 2 7 "" NOTE_INSN_PROLOGUE_END)

(note 7 35 43 "" NOTE_INSN_FUNCTION_BEG)

(insn 43 7 22 (sequence[ 
            (jump_insn 12 7 16 (set (pc)
                    (if_then_else (gt:SI (reg/v:SI 4 a0)
                            (const_int 0))
                        (label_ref 20)
                        (pc))) 221 {branch_zero} (nil)
                (nil))
            (insn/s 16 12 22 (set (reg/i:SI 2 v0)
                    (plus:SI (reg/v:SI 4 a0)
                        (reg/v:SI 5 a1))) 3 {addsi3_internal} (nil)
                (nil))
        ] ) -1 (nil)
    (nil))

(code_label 20 43 30 2 "")

(jump_insn 30 20 31 (return) 245 {return_internal} (nil)
    (nil))

(barrier 31 30 0)
)";

  RtlPassDump dump = GccRtlDumpInspector::ParsePassDump(RtlPassKind::kDbr, kSampleDbrDump);
  EXPECT_EQ(dump.Pass(), RtlPassKind::kDbr);
  EXPECT_NE(dump.PreambleText().find("Reorg pass #1"), std::string::npos);
  ASSERT_EQ(dump.Functions().size(), 1u);

  const RtlFunctionDump* fn = dump.FindFunction("AddBranch");
  ASSERT_NE(fn, nullptr);
  EXPECT_EQ(fn->insns.size(), 7u);
  EXPECT_EQ(fn->ActiveInsnCount(), 2u);
  EXPECT_EQ(fn->SequenceCount(), 1u);
  EXPECT_FALSE(fn->HasLoopNotes());

  auto prologue_notes = fn->FindNotes("NOTE_INSN_PROLOGUE_END");
  ASSERT_EQ(prologue_notes.size(), 1u);
  EXPECT_EQ(prologue_notes[0]->uid, 35);

  const RtlInsnEntry& seq_entry = fn->insns[3];
  EXPECT_EQ(seq_entry.kind, RtlInsnKind::kInsn);
  EXPECT_EQ(seq_entry.uid, 43);
  EXPECT_EQ(seq_entry.prev_uid, 7);
  EXPECT_EQ(seq_entry.next_uid, 22);
  EXPECT_TRUE(seq_entry.is_sequence);
  EXPECT_TRUE(seq_entry.in_annulled_branch_delay);
}

TEST(GccRtlDumpInspectorTest, CompilesAndInspectsAllRtlPassesWithKmcGcc272) {
  auto inspector_or = GccRtlDumpInspector::Create();
  ASSERT_TRUE(inspector_or.ok()) << inspector_or.status();

  constexpr std::string_view kLoopSnippet = R"(
s32 SumPositive(s32 n, s32 step) {
  s32 total = 0;
  s32 i;
  for (i = 0; i < n; i++) {
    if ((i & 1) != 0) {
      total += step;
    }
  }
  return total;
}
)";

  auto dump_or = inspector_or->InspectC(kLoopSnippet);
  ASSERT_TRUE(dump_or.ok()) << dump_or.status();

  EXPECT_FALSE(dump_or->AssemblyText().empty());
  for (RtlPassKind pass : GccRtlDumpInspector::AllPasses()) {
    EXPECT_TRUE(dump_or->HasPass(pass))
        << "Missing pass dump for extension " << GccRtlDumpInspector::PassExtension(pass);
    const RtlFunctionDump* fn = dump_or->GetFunctionPass("SumPositive", pass);
    ASSERT_NE(fn, nullptr) << "Missing SumPositive in pass "
                           << GccRtlDumpInspector::PassExtension(pass);
    EXPECT_GT(fn->ActiveInsnCount(), 0u);
  }

  const RtlFunctionDump* rtl_fn = dump_or->GetFunctionPass("SumPositive", RtlPassKind::kRtl);
  ASSERT_NE(rtl_fn, nullptr);
  EXPECT_TRUE(rtl_fn->HasLoopNotes());
  EXPECT_FALSE(rtl_fn->FindNotes("NOTE_INSN_LOOP_CONT").empty());

  const RtlFunctionDump* dbr_fn = dump_or->GetFunctionPass("SumPositive", RtlPassKind::kDbr);
  ASSERT_NE(dbr_fn, nullptr);
  EXPECT_GT(dbr_fn->SequenceCount(), 0u);
}

TEST(GccRtlDumpInspectorTest, PinpointsFirstDivergentPassBetweenCandidates) {
  auto inspector_or = GccRtlDumpInspector::Create();
  ASSERT_TRUE(inspector_or.ok()) << inspector_or.status();

  auto orig_or = inspector_or->InspectC(R"(
s32 Compute(s32 a, s32 b) {
  if (a > 0) {
    return a + b;
  }
  return a - b;
}
)");
  ASSERT_TRUE(orig_or.ok()) << orig_or.status();

  // Structurally identical C ASTs (with extra parentheses/formatting) have no divergence.
  auto identical_or = inspector_or->InspectC(R"(
s32 Compute(s32 a, s32 b) {
  if ((a > 0)) { return (a + b); }
  return (a - b);
}
)");
  ASSERT_TRUE(identical_or.ok()) << identical_or.status();
  EXPECT_EQ(orig_or->FindFirstDivergentPass(*identical_or, "Compute"), std::nullopt);

  // `if (...) return; else return;` differs in initial RTL generation (`.rtl`) because
  // `stmt.c:expand_start_else` emits a jump around the `else` body, which `jump.c` then
  // eliminates in `.jump`, converging for all subsequent passes.
  auto else_variant_or = inspector_or->InspectC(R"(
s32 Compute(s32 a, s32 b) {
  if (a > 0) {
    return a + b;
  } else {
    return a - b;
  }
}
)");
  ASSERT_TRUE(else_variant_or.ok()) << else_variant_or.status();
  EXPECT_EQ(orig_or->FindFirstDivergentPass(*else_variant_or, "Compute"), RtlPassKind::kRtl);
  EXPECT_TRUE(orig_or->PassMatches(*else_variant_or, "Compute", RtlPassKind::kJump));
  EXPECT_TRUE(orig_or->PassMatches(*else_variant_or, "Compute", RtlPassKind::kDbr));

  // Different arithmetic operation diverges immediately in `.rtl` and remains divergent in `.dbr`.
  auto diff_rtl_or = inspector_or->InspectC(R"(
s32 Compute(s32 a, s32 b) {
  if (a > 0) {
    return a ^ b;
  }
  return a - b;
}
)");
  ASSERT_TRUE(diff_rtl_or.ok()) << diff_rtl_or.status();
  EXPECT_EQ(orig_or->FindFirstDivergentPass(*diff_rtl_or, "Compute"), RtlPassKind::kRtl);
  EXPECT_FALSE(orig_or->PassMatches(*diff_rtl_or, "Compute", RtlPassKind::kDbr));
}

}  // namespace
}  // namespace rom_nom_nom
