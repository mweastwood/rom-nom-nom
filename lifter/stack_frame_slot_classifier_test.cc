#include "lifter/stack_frame_slot_classifier.h"

#include <cstdint>
#include <string_view>
#include <vector>

#include "absl/container/btree_map.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

RtlPatternAndNotes MustParsePn(std::string_view sexpr) {
  auto parsed = ParsePatternAndNotes(sexpr);
  EXPECT_TRUE(parsed.ok()) << sexpr;
  return parsed.ok() ? *parsed : RtlPatternAndNotes{};
}

TEST(StackFrameSlotClassifierTest, SpillBucketAlignmentAndRegionClassification) {
  EXPECT_EQ(kMipsSpillSlotBucketBytes, 8);
  EXPECT_EQ(AlignDownToSpillBucket(16, 16), 16);
  EXPECT_EQ(AlignDownToSpillBucket(20, 16), 16);
  EXPECT_EQ(AlignDownToSpillBucket(24, 16), 24);
  EXPECT_EQ(AlignDownToSpillBucket(28, 16), 24);

  const absl::btree_map<int, StackSlotBound> cs_bounds = {
      {8, StackSlotBound{.offset = 24, .byte_size = 4}},
      {32, StackSlotBound{.offset = 32, .byte_size = 8}},
  };
  EXPECT_TRUE(VerifyMonotonicCallerSaveLayout(cs_bounds));

  EXPECT_EQ(ClassifyStackFrameOffset(0, 16, 32, cs_bounds), StackFrameSlotKind::kOutgoingArg);
  EXPECT_EQ(ClassifyStackFrameOffset(12, 16, 32, cs_bounds), StackFrameSlotKind::kOutgoingArg);
  EXPECT_EQ(ClassifyStackFrameOffset(16, 16, 32, cs_bounds), StackFrameSlotKind::kSpillOrLocal);
  EXPECT_EQ(ClassifyStackFrameOffset(24, 16, 32, cs_bounds), StackFrameSlotKind::kCallerSave);
  EXPECT_EQ(ClassifyStackFrameOffset(36, 16, 32, cs_bounds), StackFrameSlotKind::kCallerSave);
  EXPECT_EQ(ClassifyStackFrameOffset(48, 16, 32, cs_bounds), StackFrameSlotKind::kUnknown);
  EXPECT_EQ(ClassifyStackFrameOffset(-4, 16, 32, cs_bounds), StackFrameSlotKind::kUnknown);

  // Non-monotonic caller-save layout (higher regno at lower offset) fails.
  const absl::btree_map<int, StackSlotBound> bad_bounds = {
      {8, StackSlotBound{.offset = 32, .byte_size = 4}},
      {9, StackSlotBound{.offset = 24, .byte_size = 4}},
  };
  EXPECT_FALSE(VerifyMonotonicCallerSaveLayout(bad_bounds));
}

TEST(StackFrameSlotClassifierTest, DeterministicMultiWordAndOverlappingCallerSaveSpans) {
  // Block with multiple saves (GPR `$t0` at idx 0, 2-word DF `$f2/$f3` at idx 1,
  // GPR `$t1` at idx 2) saved across TWO consecutive calls, where the 2-word DF
  // save is restored word-by-word (`$f2` at 4, `$f3` at 5) after call 1 and the
  // GPR `$t0` save is restored after BOTH call 1 and call 2.
  const std::vector<std::vector<RtlPatternAndNotes>> blocks = {
      {
          MustParsePn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 8 t0))"),
          MustParsePn("(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:DF 34 $f2))"),
          MustParsePn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 9 t1))"),
          MustParsePn("(call (mem:QI (symbol_ref:SI (\"callee1\"))) (const_int 16))"),
          MustParsePn("(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 24))))"),
          MustParsePn("(set (reg:SF 35 $f3) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))"),
          MustParsePn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"),
          MustParsePn("(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))"),
          MustParsePn("(call (mem:QI (symbol_ref:SI (\"callee2\"))) (const_int 16))"),
          MustParsePn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"),
      },
  };

  const CallerSaveMatchOptions opts{
      .frame_pointer_needed = false,
      .frame_size_bytes = 32,
      .outgoing_args_size = 16,
      .gp64 = false,
      .fp64 = false,
      .opt_caller_saves_enabled = true,
  };

  const CallerSaveCfgClassification cls = ClassifyCallerSavePairsAcrossCfg(blocks, opts);
  ASSERT_EQ(cls.block_matches.size(), 1u);
  ASSERT_EQ(cls.block_matches[0].size(), 3u);

  // Strictly ascending `save_idx` order: 0 (`$t0`), 1 (`$f2/$f3`), 2 (`$t1`).
  EXPECT_EQ(cls.block_matches[0][0].save_idx, 0u);
  EXPECT_EQ(cls.block_matches[0][0].save_span, 1);
  ASSERT_EQ(cls.block_matches[0][0].restores.size(), 2u);
  EXPECT_EQ(cls.block_matches[0][0].restores[0].restore_idx, 6u);
  EXPECT_EQ(cls.block_matches[0][0].restores[1].restore_idx, 9u);

  EXPECT_EQ(cls.block_matches[0][1].save_idx, 1u);
  EXPECT_EQ(cls.block_matches[0][1].save_span, 2);
  ASSERT_EQ(cls.block_matches[0][1].restores.size(), 2u);
  EXPECT_EQ(cls.block_matches[0][1].restores[0].restore_idx, 4u);
  EXPECT_EQ(cls.block_matches[0][1].restores[1].restore_idx, 5u);

  EXPECT_EQ(cls.block_matches[0][2].save_idx, 2u);
  EXPECT_EQ(cls.block_matches[0][2].save_span, 1);
  ASSERT_EQ(cls.block_matches[0][2].restores.size(), 1u);
  EXPECT_EQ(cls.block_matches[0][2].restores[0].restore_idx, 7u);

  EXPECT_EQ(cls.save_span[0], std::vector<int>({1, 2, 1, 0, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(cls.restore_span[0], std::vector<int>({0, 0, 0, 0, 1, 1, 1, 1, 0, 1}));
  EXPECT_EQ(cls.aligned_caller_save_base, 16);

  // Monotonic hard-register bounds: `$8 @ 16`, `$9 @ 20`, `$34 @ 24`, `$35 @ 28`.
  ASSERT_EQ(cls.hr_slot_bounds.size(), 4u);
  EXPECT_EQ(cls.hr_slot_bounds.at(8), (StackSlotBound{.offset = 16, .byte_size = 4}));
  EXPECT_EQ(cls.hr_slot_bounds.at(9), (StackSlotBound{.offset = 20, .byte_size = 4}));
  EXPECT_EQ(cls.hr_slot_bounds.at(34), (StackSlotBound{.offset = 24, .byte_size = 4}));
  EXPECT_EQ(cls.hr_slot_bounds.at(35), (StackSlotBound{.offset = 28, .byte_size = 4}));

  // Repeated invocations produce bit-identical ordering and spans.
  for (int iter = 0; iter < 5; ++iter) {
    const CallerSaveCfgClassification again = ClassifyCallerSavePairsAcrossCfg(blocks, opts);
    EXPECT_EQ(again.hr_slot_bounds, cls.hr_slot_bounds);
    EXPECT_EQ(again.save_span, cls.save_span);
    EXPECT_EQ(again.restore_span, cls.restore_span);
    ASSERT_EQ(again.block_matches[0].size(), 3u);
    EXPECT_EQ(again.block_matches[0][0].save_idx, 0u);
    EXPECT_EQ(again.block_matches[0][1].save_idx, 1u);
    EXPECT_EQ(again.block_matches[0][2].save_idx, 2u);
  }
}

TEST(StackFrameSlotClassifierTest, RejectsConflictingInvalidatedAndDisabledCases) {
  const CallerSaveMatchOptions opts{
      .frame_pointer_needed = false,
      .frame_size_bytes = 32,
      .outgoing_args_size = 16,
      .gp64 = false,
      .fp64 = false,
      .opt_caller_saves_enabled = true,
  };

  // Negative 1: `$t0`'s stack slot is touched by an unrelated non-caller-save
  // load in another block, so `$t0` is pruned while `$t1` remains matched.
  const std::vector<std::vector<RtlPatternAndNotes>> partial_conflict = {
      {
          MustParsePn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 8 t0))"),
          MustParsePn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 9 t1))"),
          MustParsePn("(call (mem:QI (symbol_ref:SI (\"foo\"))) (const_int 16))"),
          MustParsePn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"),
          MustParsePn("(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))"),
      },
      {
          MustParsePn("(set (reg:SI 2 v0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"),
      },
  };
  const CallerSaveCfgClassification filtered =
      ClassifyCallerSavePairsAcrossCfg(partial_conflict, opts);
  EXPECT_EQ(filtered.save_span[0], std::vector<int>({0, 1, 0, 0, 0}));
  EXPECT_EQ(filtered.restore_span[0], std::vector<int>({0, 0, 0, 0, 1}));
  EXPECT_FALSE(filtered.hr_slot_bounds.contains(8));
  EXPECT_TRUE(filtered.hr_slot_bounds.contains(9));

  // Negative 2: intervening write to `$t0` before the call invalidates the pending save.
  const std::vector<std::vector<RtlPatternAndNotes>> overwritten_before_call = {
      {
          MustParsePn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 8 t0))"),
          MustParsePn("(set (reg:SI 8 t0) (const_int 99))"),
          MustParsePn("(call (mem:QI (symbol_ref:SI (\"foo\"))) (const_int 16))"),
          MustParsePn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"),
      },
  };
  const CallerSaveCfgClassification invalidated =
      ClassifyCallerSavePairsAcrossCfg(overwritten_before_call, opts);
  EXPECT_TRUE(invalidated.hr_slot_bounds.empty());
  EXPECT_EQ(invalidated.save_span[0], std::vector<int>({0, 0, 0, 0}));

  // Negative 3: non-monotonic layout (`$t0` at offset 24, `$t1` at offset 16) is rejected.
  const std::vector<std::vector<RtlPatternAndNotes>> non_monotonic = {
      {
          MustParsePn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 8 t0))"),
          MustParsePn("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 9 t1))"),
          MustParsePn("(call (mem:QI (symbol_ref:SI (\"foo\"))) (const_int 16))"),
          MustParsePn("(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))"),
          MustParsePn("(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))"),
      },
  };
  const CallerSaveCfgClassification rejected_mono =
      ClassifyCallerSavePairsAcrossCfg(non_monotonic, opts);
  EXPECT_TRUE(rejected_mono.hr_slot_bounds.empty());

  // Negative 4: caller-saves disabled returns empty.
  CallerSaveMatchOptions disabled_opts = opts;
  disabled_opts.opt_caller_saves_enabled = false;
  const CallerSaveCfgClassification disabled =
      ClassifyCallerSavePairsAcrossCfg(partial_conflict, disabled_opts);
  EXPECT_TRUE(disabled.hr_slot_bounds.empty());
}

}  // namespace
}  // namespace rom_nom_nom::lifter
