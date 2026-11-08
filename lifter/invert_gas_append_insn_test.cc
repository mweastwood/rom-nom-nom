#include "lifter/invert_gas_append_insn.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_gas_mips_ip.h"

namespace rom_nom_nom {
namespace {

class LifterGasAppendInsnInverseProvider : public gcc_harness::SubfunctionInverseProvider {
 public:
  std::string InvertGasMipsIp(uint32_t word_x) const override {
    return rom_nom_nom::InvertGasMipsIp(word_x);
  }

  std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) const override {
    return rom_nom_nom::InvertGasAppendInsn(words_x);
  }
};

TEST(InvertGasAppendInsnTest, SyntheticReorderSwapAndNopDelaySlotTests) {
  gcc_harness::GccContext ctx;
  gcc_harness::FunctionHooks hooks(&ctx);
  LifterGasAppendInsnInverseProvider provider;
  gcc_harness::RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1. Swappable delay slot before `jr $31`: `InvertGasAppendInsn` emits
  //    `["addiu $2,$4,5", "jr $31"]` in `.set reorder` mode so `append_insn`
  //    swaps them back to `[jr $31, addiu $2,$4,5]`.
  {
    const std::vector<uint32_t> words = {
        0x03e00008u,  // jr $31
        0x24820005u,  // addiu $2,$4,5
    };
    const std::vector<std::string> lines = InvertGasAppendInsn(words);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "addiu $2,$4,5");
    EXPECT_EQ(lines[1], "jr $31");
    const auto res = verifier.VerifyGasAppendInsnRightInverse(words);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_EQ(hooks.RunGasMdAssemble(absl::StrJoin(lines, "\n")), words);
  }

  // 2. Unfilled (`nop`) delay slot at start of stream: `InvertGasAppendInsn`
  //    emits bare `["jr $31"]` and `mips_emit_delays()` supplies the trailing
  //    `nop`.
  {
    const std::vector<uint32_t> words = {
        0x03e00008u,  // jr $31
        0x00000000u,  // nop
    };
    const std::vector<std::string> lines = InvertGasAppendInsn(words);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "jr $31");
    const auto res = verifier.VerifyGasAppendInsnRightInverse(words);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_EQ(hooks.RunGasMdAssemble(absl::StrJoin(lines, "\n")), words);
  }

  // 3. Unfilled (`nop`) delay slot immediately after a swappable instruction
  //    (e.g., `-O0` or `-Wa,-g` output): must wrap `[jr $31, nop]` in `.set
  //    noreorder` so `append_insn` does not swap `addiu $2,$4,5` into `jr
  //    $31`'s delay slot.
  {
    const std::vector<uint32_t> words = {
        0x24820005u,  // addiu $2,$4,5
        0x03e00008u,  // jr $31
        0x00000000u,  // nop
    };
    const std::vector<std::string> lines = InvertGasAppendInsn(words);
    const auto res = verifier.VerifyGasAppendInsnRightInverse(words);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_EQ(hooks.RunGasMdAssemble(absl::StrJoin(lines, "\n")), words);
  }

  // 4. GCC `dbr_schedule` delay slot that writes a register read by the branch
  //    (`beq $2,$0,$L2` with `addiu $2,$4,1` in delay slot): `append_insn`
  //    refuses to swap when `delay_insn` modifies a register used by the
  //    branch, so `InvertGasAppendInsn` preserves it via `.set noreorder`.
  {
    const std::vector<uint32_t> words = {
        0x10400002u,  // beq $2,$0,$L2 (targets word 3)
        0x24820001u,  // addiu $2,$4,1 (modifies $2 read by beq)
        0x24020000u,  // addiu $2,$0,0
        0x03e00008u,  // $L2: jr $31
        0x00000000u,  // nop
    };
    const std::vector<std::string> lines = InvertGasAppendInsn(words);
    const auto res = verifier.VerifyGasAppendInsnRightInverse(words);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_EQ(hooks.RunGasMdAssemble(absl::StrJoin(lines, "\n")), words);
  }

  // 5. Branch-likely (`bnel`) and coprocessor condition branch (`bc1f`) with
  //    non-`nop` delay slots (`INSN_COND_BRANCH_LIKELY` and
  //    `INSN_READ_COND_CODE` are never swapped by `append_insn`).
  {
    const std::vector<uint32_t> words = {
        0x54800003u,  // bnel $4,$0,$L2 (targets word 4)
        0x24820001u,  // addiu $2,$4,1
        0x45000001u,  // bc1f $L2 (targets word 4)
        0x24820002u,  // addiu $2,$4,2
        0x03e00008u,  // $L2: jr $31
        0x00000000u,  // nop
    };
    const std::vector<std::string> lines = InvertGasAppendInsn(words);
    const auto res = verifier.VerifyGasAppendInsnRightInverse(words);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_EQ(hooks.RunGasMdAssemble(absl::StrJoin(lines, "\n")), words);
  }

  // 6. Local `j $L<N>` jump and label targeting a branch instruction itself
  //    (`insn_label != NULL` prevents `append_insn` from swapping across the
  //    label).
  {
    const std::vector<uint32_t> words = {
        0x08000003u,  // j $L2 (word 3)
        0x00000000u,  // nop
        0x24840001u,  // addiu $4,$4,1
        0x03e00008u,  // $L2: jr $31
        0x24820009u,  // addiu $2,$4,9 (delay slot at labeled branch)
    };
    const std::vector<std::string> lines = InvertGasAppendInsn(words);
    const auto res = verifier.VerifyGasAppendInsnRightInverse(words);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_EQ(hooks.RunGasMdAssemble(absl::StrJoin(lines, "\n")), words);
  }

  // 7. Hazard instructions (`mflo`, `mfhi`, `mfc1`, `mtc1`, `c.lt.s`) without
  //    and with explicit `nop` words: verifies `InvertGasAppendInsn` never
  //    triggers extra automatic hazard `nop` insertion in `append_insn` or
  //    `mips_emit_delays()`.
  {
    const std::vector<uint32_t> words = {
        0x00850018u,  // mult $4,$5
        0x00001012u,  // mflo $2
        0x00001810u,  // mfhi $3
        0x44840000u,  // mtc1 $4,$f0
        0x44050000u,  // mfc1 $5,$f0
        0x4600003cu,  // c.lt.s $f0,$f0
        0x03e00008u,  // jr $31
        0x00431021u,  // addu $2,$2,$3
    };
    const std::vector<std::string> lines = InvertGasAppendInsn(words);
    const auto res = verifier.VerifyGasAppendInsnRightInverse(words);
    EXPECT_TRUE(res.matches) << res.diff_detail;
    EXPECT_EQ(hooks.RunGasMdAssemble(absl::StrJoin(lines, "\n")), words);
  }
}

TEST(InvertGasAppendInsnTest, NegativeInvalidWordsAndTruncatedBranchFailVerification) {
  gcc_harness::GccContext ctx;
  gcc_harness::FunctionHooks hooks(&ctx);
  LifterGasAppendInsnInverseProvider provider;
  gcc_harness::RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // Empty input returns empty lines.
  EXPECT_TRUE(InvertGasAppendInsn({}).empty());

  // Sequences containing reserved/invalid MIPS opcodes must be rejected.
  const std::vector<std::vector<uint32_t>> invalid_word_sequences = {
      {0x00000001u},                           // SPECIAL reserved funct=0x01
      {0x24820001u, 0x70000000u, 0x03e00008u,  // Primary opcode 0x1c reserved
       0x00000000u},
      {0x46000020u, 0x03e00008u, 0x00000000u},  // Invalid cvt.s.s
  };
  for (const auto& seq : invalid_word_sequences) {
    EXPECT_TRUE(InvertGasAppendInsn(seq).empty());
    const auto res = verifier.VerifyGasAppendInsnRightInverse(seq);
    EXPECT_FALSE(res.matches);
    EXPECT_EQ(res.diff_detail, "InvertGasAppendInsn returned empty assembly lines");
  }

  // Sequences ending on a branch/jump word without a trailing delay-slot word
  // must be rejected.
  const std::vector<std::vector<uint32_t>> truncated_branch_sequences = {
      {0x03e00008u},               // jr $31 without delay slot
      {0x24820001u, 0x10800002u},  // beq $4,$0,2 without delay slot
      {0x08000001u},               // j 0x4 without delay slot
      {0x45010002u},               // bc1t 2 without delay slot
  };
  for (const auto& seq : truncated_branch_sequences) {
    EXPECT_TRUE(InvertGasAppendInsn(seq).empty());
    const auto res = verifier.VerifyGasAppendInsnRightInverse(seq);
    EXPECT_FALSE(res.matches);
    EXPECT_EQ(res.diff_detail, "InvertGasAppendInsn returned empty assembly lines");
  }
}

TEST(InvertGasAppendInsnTest, InvertGasAppendInsnToItemsPaddingBranchModesAndHazardNops) {
  // 1. Section-alignment padding stripping (4-word aligned trailing `0x00000000u`
  //    words after the final branch+delay slot).
  {
    const std::vector<uint32_t> padded_swapped = {
        0x03e00008u,  // jr $31
        0x24820005u,  // addiu $2,$4,5 (delay slot)
        0x00000000u,  // section alignment padding
        0x00000000u,  // section alignment padding
    };
    const absl::Span<const uint32_t> stripped = StripSectionAlignmentPadding(padded_swapped);
    ASSERT_EQ(stripped.size(), 2u);
    EXPECT_EQ(stripped[0], 0x03e00008u);
    EXPECT_EQ(stripped[1], 0x24820005u);

    std::vector<AsmItem> items;
    ASSERT_TRUE(InvertGasAppendInsnToItems(padded_swapped, &items));
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].kind, AsmItemKind::kReorderInsn);
    EXPECT_EQ(items[0].line, "addiu $2,$4,5");
    EXPECT_EQ(items[0].word, 0x24820005u);
    EXPECT_TRUE(items[0].unswapped_from_branch);
    EXPECT_EQ(items[1].kind, AsmItemKind::kReorderInsn);
    EXPECT_EQ(items[1].line, "jr $31");
    EXPECT_EQ(items[1].word, 0x03e00008u);
    EXPECT_TRUE(items[1].unswapped_from_branch);

    // When the branch's delay slot itself is `nop` (`0x00000000u`), only the
    // trailing alignment `0x00000000u` after the delay slot is stripped.
    const std::vector<uint32_t> padded_nop_delay = {
        0x24820005u,  // addiu $2,$4,5
        0x03e00008u,  // jr $31
        0x00000000u,  // delay slot nop
        0x00000000u,  // section alignment padding
    };
    const absl::Span<const uint32_t> stripped_nop = StripSectionAlignmentPadding(padded_nop_delay);
    ASSERT_EQ(stripped_nop.size(), 3u);
  }

  // 2. `.set reorder` branch un-swapping vs `.set noreorder` branch delay slots.
  {
    const std::vector<uint32_t> words = {
        0x10400002u,  // beq $2,$0,$L2 (targets word 3)
        0x24820001u,  // addiu $2,$4,1 (modifies $2 read by beq -> .set noreorder)
        0x24020000u,  // addiu $2,$0,0
        0x03e00008u,  // $L2: jr $31 (labeled branch -> .set noreorder with non-nop delay)
        0x24820009u,  // addiu $2,$4,9
    };
    std::vector<AsmItem> items;
    ASSERT_TRUE(InvertGasAppendInsnToItems(words, &items));
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0].kind, AsmItemKind::kNoreorderBranchSeq);
    EXPECT_EQ(items[0].line, "beq $2,$0,$L2");
    EXPECT_EQ(items[0].delay_line, "addiu $2,$4,1");
    EXPECT_EQ(items[1].kind, AsmItemKind::kReorderInsn);
    EXPECT_EQ(items[1].line, "addiu $2,$0,0");
    EXPECT_EQ(items[2].kind, AsmItemKind::kLabel);
    EXPECT_EQ(items[2].line, "$L2:");
    EXPECT_EQ(items[3].kind, AsmItemKind::kNoreorderBranchSeq);
    EXPECT_EQ(items[3].line, "jr $31");
    EXPECT_EQ(items[3].delay_line, "addiu $2,$4,9");
  }

  // 3. Hazard `nop` stripping after `mflo`, `mfhi`, `mfc1`, `mtc1`, and `c.<cond>.s/d`.
  {
    const std::vector<uint32_t> hazard_words = {
        0x00850018u,  // mult $4,$5
        0x00001012u,  // mflo $2
        0x00000000u,  // hazard nop 1 (mflo -> mult)
        0x00000000u,  // hazard nop 2 (mflo -> mult)
        0x00850018u,  // mult $4,$5
        0x00001810u,  // mfhi $3
        0x00000000u,  // hazard nop 1 (mfhi -> mthi)
        0x00000000u,  // hazard nop 2 (mfhi -> mthi)
        0x00800011u,  // mthi $4
        0x44840000u,  // mtc1 $4,$f0
        0x00000000u,  // hazard nop (mtc1 -> add.s)
        0x46000000u,  // add.s $f0,$f0,$f0
        0x44050000u,  // mfc1 $5,$f0
        0x00000000u,  // hazard nop (mfc1 -> addu reading $5)
        0x00a21021u,  // addu $2,$5,$2
        0x4600003cu,  // c.lt.s $f0,$f0
        0x00000000u,  // hazard nop (c.lt.s -> bc1t)
        0x45010001u,  // bc1t $L2 (targets jr $31)
        0x00000000u,  // delay slot nop
        0x03e00008u,  // $L2: jr $31
        0x00000000u,  // delay slot nop
    };
    std::vector<AsmItem> items;
    ASSERT_TRUE(InvertGasAppendInsnToItems(hazard_words, &items));

    std::vector<std::string> item_lines;
    for (const AsmItem& item : items) {
      item_lines.push_back(item.line);
      EXPECT_NE(item.line, "nop") << "Unexpected unstripped hazard nop";
    }
    const std::vector<std::string> expected_lines = {
        "mult $4,$5",    "mflo $2",        "mult $4,$5",        "mfhi $3",
        "mthi $4",       "mtc1 $4,$f0",    "add.s $f0,$f0,$f0", "mfc1 $5,$f0",
        "addu $2,$5,$2", "c.lt.s $f0,$f0", "bc1t $L2",          "$L2:",
        "jr $31",
    };
    EXPECT_EQ(item_lines, expected_lines);
  }

  // 4. Negative cases for `InvertGasAppendInsnToItems` (empty span, invalid
  //    word `0xffffffffu`, non-branch-terminated sequence).
  {
    std::vector<AsmItem> items;
    EXPECT_FALSE(InvertGasAppendInsnToItems({}, &items));
    EXPECT_TRUE(items.empty());

    const std::vector<uint32_t> single_word = {0x03e00008u};
    EXPECT_FALSE(InvertGasAppendInsnToItems(single_word, &items));
    EXPECT_TRUE(items.empty());

    const std::vector<uint32_t> invalid_word_seq = {
        0xffffffffu,  // invalid word (sdc1 $f31,-1($31) is valid, so also test 0x7c000000u)
        0x7c000000u,  // unallocated opcode 0x1f
        0x03e00008u,
        0x00000000u,
    };
    EXPECT_FALSE(InvertGasAppendInsnToItems(invalid_word_seq, &items));
    EXPECT_TRUE(items.empty());

    const std::vector<uint32_t> non_branch_terminated = {
        0x24820001u,  // addiu $2,$4,1
        0x24820002u,  // addiu $2,$4,2
    };
    EXPECT_FALSE(InvertGasAppendInsnToItems(non_branch_terminated, &items));
    EXPECT_TRUE(items.empty());

    const std::vector<uint32_t> only_ffffffff = {0xffffffffu};
    EXPECT_FALSE(InvertGasAppendInsnToItems(only_ffffffff, &items));
    EXPECT_TRUE(items.empty());
  }
}

class InvertGasAppendInsnRoundTripCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new gcc_harness::GccContext();
    hooks = new gcc_harness::FunctionHooks(ctx);
    provider = new LifterGasAppendInsnInverseProvider();
    verifier = new gcc_harness::RightInverseVerifier(ctx, hooks, provider);
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static gcc_harness::GccContext* ctx;
  static gcc_harness::FunctionHooks* hooks;
  static LifterGasAppendInsnInverseProvider* provider;
  static gcc_harness::RightInverseVerifier* verifier;
};

gcc_harness::GccContext* InvertGasAppendInsnRoundTripCorpusTest::ctx = nullptr;
gcc_harness::FunctionHooks* InvertGasAppendInsnRoundTripCorpusTest::hooks = nullptr;
LifterGasAppendInsnInverseProvider* InvertGasAppendInsnRoundTripCorpusTest::provider = nullptr;
gcc_harness::RightInverseVerifier* InvertGasAppendInsnRoundTripCorpusTest::verifier = nullptr;

TEST_P(InvertGasAppendInsnRoundTripCorpusTest, VerifiesKGasAppendInsnBoundaryAndFinalMachineWords) {
  const RoundTripTestCase& tc = GetParam();
  const gcc_harness::FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);

  bool found_append_insn_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != gcc_harness::SubfunctionId::kGasAppendInsn) {
      continue;
    }
    found_append_insn_snap = true;
    EXPECT_FALSE(snap.post_machine_words.empty()) << tc.test_name;
    const auto boundary_res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(boundary_res.matches) << tc.test_name << ": " << boundary_res.diff_detail;

    const auto direct_res = verifier->VerifyGasAppendInsnRightInverse(snap.post_machine_words);
    EXPECT_TRUE(direct_res.matches) << tc.test_name << ": " << direct_res.diff_detail;
  }
  EXPECT_TRUE(found_append_insn_snap) << tc.test_name;

  EXPECT_FALSE(trace.final_machine_words.empty()) << tc.test_name;
  const auto final_res = verifier->VerifyGasAppendInsnRightInverse(trace.final_machine_words);
  EXPECT_TRUE(final_res.matches) << tc.test_name << ": " << final_res.diff_detail;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, InvertGasAppendInsnRoundTripCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
