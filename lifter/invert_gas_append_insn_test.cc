#include "lifter/invert_gas_append_insn.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
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

  std::vector<std::string> InvertGasAppendInsn(
      absl::Span<const uint32_t> words_x, absl::Span<const std::string> c_flags) const override {
    return rom_nom_nom::InvertGasAppendInsn(words_x, c_flags);
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

TEST(InvertGasAppendInsnTest, Stage0InputJumpTablesExternalJumpRelocsAndLinkedVram) {
  gcc_harness::GccContext ctx;
  gcc_harness::FunctionHooks hooks(&ctx);
  LifterGasAppendInsnInverseProvider provider;
  gcc_harness::RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1. `.rodata` jump-table `target_word_indices` (`extra_label_indices`) +
  //    `out_word_to_label` mapping + trailing section-alignment padding strip:
  {
    Stage0FunctionInput jt_input;
    jt_input.machine_words = {
        0x00400008u,  // 0: jr $2
        0x00000000u,  // 1: nop
        0x03e00008u,  // 2: jr $31 (arm 0)
        0x2482000au,  // 3: addiu $2,$4,10 (delay slot)
        0x03e00008u,  // 4: jr $31 (arm 1)
        0x24820014u,  // 5: addiu $2,$4,20 (delay slot)
        0x00000000u,  // 6: alignment padding
        0x00000000u,  // 7: alignment padding
    };
    jt_input.jump_tables.push_back(Stage0JumpTable{
        .label_name = ".L10",
        .target_word_indices = {2u, 4u},
    });

    std::vector<AsmItem> items;
    absl::flat_hash_map<size_t, std::string> word_to_label;
    ASSERT_TRUE(InvertGasAppendInsnToItems(jt_input, &items, &word_to_label));
    ASSERT_EQ(word_to_label.size(), 2u);
    EXPECT_EQ(word_to_label[2u], "$L2");
    EXPECT_EQ(word_to_label[4u], "$L3");

    std::vector<std::string> item_lines;
    for (const AsmItem& it : items) {
      if (it.kind == AsmItemKind::kNoreorderBranchSeq) {
        item_lines.push_back(".set noreorder");
        item_lines.push_back(it.line);
        item_lines.push_back(it.delay_line);
        item_lines.push_back(".set reorder");
      } else {
        item_lines.push_back(it.line);
      }
    }
    const std::vector<uint32_t> unpadded_words(jt_input.machine_words.begin(),
                                               jt_input.machine_words.begin() + 6);
    EXPECT_EQ(hooks.RunGasMdAssemble(absl::StrJoin(item_lines, "\n")), unpadded_words);
    const auto res = verifier.VerifyGasAppendInsnRightInverse(unpadded_words);
    EXPECT_TRUE(res.matches) << res.diff_detail;
  }

  // 2. External `j` tail-call relocation (`external_j_indices` via `kMips26`):
  //    prevents an external `j` word (`op == 0x02`) from being misclassified as
  //    a local `.text` label jump, while un-relocated `j 0x8` IS resolved locally.
  {
    Stage0FunctionInput local_j_input;
    local_j_input.machine_words = {
        0x08000002u,  // 0: j 0x8 (local target word 2 when un-relocated)
        0x00000000u,  // 1: nop
        0x03e00008u,  // 2: jr $31
        0x00000000u,  // 3: nop
    };
    std::vector<AsmItem> local_items;
    absl::flat_hash_map<size_t, std::string> local_labels;
    ASSERT_TRUE(InvertGasAppendInsnToItems(local_j_input, &local_items, &local_labels));
    ASSERT_EQ(local_labels.size(), 1u);
    EXPECT_EQ(local_labels[2u], "$L2");
    EXPECT_EQ(local_items[0].line, "j $L2");
    const auto local_res = verifier.VerifyGasAppendInsnRightInverse(local_j_input.machine_words);
    EXPECT_TRUE(local_res.matches) << local_res.diff_detail;

    Stage0FunctionInput ext_j_input = local_j_input;
    ext_j_input.relocations.push_back(Stage0Relocation{
        .word_index = 0u,
        .kind = Stage0RelocKind::kMips26,
        .symbol_name = "ExternalTail",
        .addend = 0,
    });
    std::vector<AsmItem> ext_items;
    absl::flat_hash_map<size_t, std::string> ext_labels;
    ASSERT_TRUE(InvertGasAppendInsnToItems(ext_j_input, &ext_items, &ext_labels));
    EXPECT_TRUE(ext_labels.empty());
    ASSERT_FALSE(ext_items.empty());
    EXPECT_EQ(ext_items[0].word_index, 0u);
    EXPECT_NE(ext_items[0].line, "j $L2");
  }

  // 3. Linked-ROM `base_vram != 0` local `j` target resolution vs external `j`:
  {
    Stage0FunctionInput linked_input;
    linked_input.base_vram = 0x80020000u;
    linked_input.machine_words = {
        0x08008003u,  // 0 (0x80020000): j 0x8002000c (local word 3)
        0x00000000u,  // 1 (0x80020004): nop
        0x24820001u,  // 2 (0x80020008): addiu $2,$4,1
        0x03e00008u,  // 3 (0x8002000c): jr $31
        0x24820007u,  // 4 (0x80020010): addiu $2,$4,7
    };
    std::vector<AsmItem> linked_items;
    absl::flat_hash_map<size_t, std::string> linked_labels;
    ASSERT_TRUE(InvertGasAppendInsnToItems(linked_input, &linked_items, &linked_labels));
    ASSERT_EQ(linked_labels.size(), 1u);
    EXPECT_EQ(linked_labels[3u], "$L2");
    ASSERT_FALSE(linked_items.empty());
    EXPECT_EQ(linked_items[0].line, "j $L2");

    // Re-assembling `linked_items` at address 0 produces the exact local-offset
    // twin (`0x08000003u` at word 0), which passes `VerifyGasAppendInsnRightInverse`.
    std::vector<uint32_t> expected_zero_based = linked_input.machine_words;
    expected_zero_based[0] = 0x08000003u;
    const auto zero_res = verifier.VerifyGasAppendInsnRightInverse(expected_zero_based);
    EXPECT_TRUE(zero_res.matches) << zero_res.diff_detail;

    // Out-of-function `j 0x80024000` is preserved as external rather than local:
    Stage0FunctionInput ext_linked = linked_input;
    ext_linked.machine_words[0] = 0x08009000u;  // j 0x80024000
    std::vector<AsmItem> ext_linked_items;
    absl::flat_hash_map<size_t, std::string> ext_linked_labels;
    ASSERT_TRUE(InvertGasAppendInsnToItems(ext_linked, &ext_linked_items, &ext_linked_labels));
    EXPECT_TRUE(ext_linked_labels.empty());
    EXPECT_NE(ext_linked_items[0].line, "j $L2");
  }

  // 4. Negative checks on `Stage0FunctionInput` overload:
  {
    std::vector<AsmItem> items;
    absl::flat_hash_map<size_t, std::string> labels;
    EXPECT_FALSE(InvertGasAppendInsnToItems(Stage0FunctionInput{}, &items, &labels));
    EXPECT_TRUE(items.empty());

    Stage0FunctionInput truncated;
    truncated.machine_words = {0x03e00008u};
    EXPECT_FALSE(InvertGasAppendInsnToItems(truncated, &items, /*out_word_to_label=*/nullptr));

    Stage0FunctionInput invalid;
    invalid.machine_words = {0x00000001u, 0x00000000u};
    EXPECT_FALSE(InvertGasAppendInsnToItems(invalid, &items, &labels));
  }
}

TEST(InvertGasAppendInsnTest, Mips1LoadDelayHazardNopsAndPrevPrevLoadBranchNonSwap) {
  gcc_harness::GccContext ctx;
  gcc_harness::FunctionHooks hooks(&ctx);
  LifterGasAppendInsnInverseProvider provider;
  gcc_harness::RightInverseVerifier verifier(&ctx, &hooks, &provider);

  const std::vector<std::string> mips1_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"};

  // 1. `-mips1` load-delay `nop`s after `lw` (GPR) and `lwc1` (FPR), plus
  //    `-mips1` epilogue `lw $31,16($29); addiu $29,$29,24; jr $31; nop` where
  //    `prev_prev_insn` (`lw $31`) prevents `append_insn` from swapping `addiu`
  //    into `jr $31`'s delay slot (`tc-mips.c:1656-1664`).
  Stage0FunctionInput mips1_input;
  mips1_input.profile.optimize_level = 2;
  mips1_input.profile.c_flags = mips1_flags;
  mips1_input.machine_words = {
      0x8c820000u,  // 0: lw $2,0($4)
      0x00000000u,  // 1: -mips1 load-delay nop (lw $2 -> addu reading $2)
      0x00451021u,  // 2: addu $2,$2,$5
      0xc4800004u,  // 3: lwc1 $f0,4($4)
      0x00000000u,  // 4: -mips1 coproc-load-delay nop (lwc1 $f0 -> add.s reading $f0)
      0x46020000u,  // 5: add.s $f0,$f0,$f2
      0x8fbf0010u,  // 6: lw $31,16($29)
      0x27bd0018u,  // 7: addiu $29,$29,24
      0x03e00008u,  // 8: jr $31
      0x00000000u,  // 9: unfilled delay slot nop (prev_prev lw $31 blocked swap)
      0x00000000u,  // 10: section alignment padding
      0x00000000u,  // 11: section alignment padding
  };

  gcc_harness::SubfunctionBoundarySnapshot snap;
  snap.subfunction_id = gcc_harness::SubfunctionId::kGasAppendInsn;
  snap.optimize_level = 2;
  snap.c_flags = mips1_flags;
  snap.post_machine_words = mips1_input.machine_words;
  const auto boundary_res = verifier.VerifySubfunctionBoundaryRightInverse(snap);
  EXPECT_TRUE(boundary_res.matches) << boundary_res.diff_detail;

  std::vector<AsmItem> items;
  ASSERT_TRUE(InvertGasAppendInsnToItems(mips1_input, &items, /*out_word_to_label=*/nullptr));
  std::vector<std::string> item_lines;
  for (const AsmItem& item : items) {
    EXPECT_EQ(item.kind, AsmItemKind::kReorderInsn) << item.line;
    EXPECT_NE(item.line, "nop");
    item_lines.push_back(item.line);
  }
  const std::vector<std::string> expected_lines = {
      "lw $2,0($4)",    "addu $2,$2,$5",    "lwc1 $f0,4($4)", "add.s $f0,$f0,$f2",
      "lw $31,16($29)", "addiu $29,$29,24", "jr $31",
  };
  EXPECT_EQ(item_lines, expected_lines);
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
