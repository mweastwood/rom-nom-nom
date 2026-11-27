#include "lifter/mips_multi_insn_idioms.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"
#include "lifter/mips_idiom_utils.h"

namespace rom_nom_nom::lifter {
namespace {

std::vector<RtlPatternAndNotes> ParseBlockInsns(const std::vector<std::string_view>& texts) {
  std::vector<RtlPatternAndNotes> out;
  out.reserve(texts.size());
  for (std::string_view text : texts) {
    auto parsed = ParsePatternAndNotes(text);
    EXPECT_TRUE(parsed.ok()) << text << ": " << parsed.status();
    if (parsed.ok()) {
      out.push_back(std::move(*parsed));
    }
  }
  return out;
}

TEST(MipsMultiInsnIdiomsTest, TryFoldGp32DiMultiInsnIdiomsPositiveAndLiveScratchNegative) {
  auto make_cfg = [](bool scratch_live_after) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(2);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].successors = {1};
    cfg.rpo_blocks[1].id = 1;
    cfg.rpo_blocks[1].predecessors = {0};
    if (scratch_live_after) {
      cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 6, 7}}, {1, {2, 3, 8}}};
    } else {
      cfg.sideband.basic_block_live_at_start = {{0, {4, 5, 6, 7}}, {1, {2, 3}}};
    }
    return cfg;
  };

  // 4-insn `adddi3_internal_1` (`dL = aL + bL; tmp = dL <u bL; dH = aH + bH; dH = dH + tmp`).
  const std::vector<std::string_view> add_di_insns = {
      "(set (reg:SI 3 v1) (plus:SI (reg:SI 5 a1) (reg:SI 7 a3)))",
      "(set (reg:SI 8 t0) (ltu:SI (reg:SI 3 v1) (reg:SI 7 a3)))",
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 6 a2)))",
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 8 t0)))",
  };

  // Positive: dead carry scratch `$8 t0` folds cleanly into `adddi3_internal_1` `PARALLEL`.
  {
    const RtlControlFlowGraph cfg = make_cfg(/*scratch_live_after=*/false);
    std::vector<RtlPatternAndNotes> insns = ParseBlockInsns(add_di_insns);
    EXPECT_TRUE(TryFoldGp32DiMultiInsnIdiomsInBlock(cfg, /*b=*/0,
                                                    /*mark_newly_folded_sentinel=*/false, &insns));
    ASSERT_EQ(insns.size(), 1u);
    const std::string s = insns[0].ToGccPrintRtlString();
    EXPECT_TRUE(absl::StrContains(s, "(set (reg:DI 2 v0) (plus:DI (reg:DI 4 a0) (reg:DI 6 a2)))"))
        << s;
    EXPECT_TRUE(absl::StrContains(s, "(clobber (reg:SI 8 t0))")) << s;
  }

  // Negative: when carry scratch `$8 t0` is live into successor `B1`, folding is forbidden.
  {
    const RtlControlFlowGraph cfg = make_cfg(/*scratch_live_after=*/true);
    std::vector<RtlPatternAndNotes> insns = ParseBlockInsns(add_di_insns);
    EXPECT_FALSE(TryFoldGp32DiMultiInsnIdiomsInBlock(cfg, /*b=*/0,
                                                     /*mark_newly_folded_sentinel=*/false, &insns));
    EXPECT_EQ(insns.size(), 4u);
  }
}

TEST(MipsMultiInsnIdiomsTest, TryFoldGp32DiWitnessedIdiomsPositiveAndUnwitnessedNegative) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;

  const std::vector<std::string_view> xor_and_load_insns = {
      "(set (reg:SI 2 v0) (xor:SI (reg:SI 4 a0) (reg:SI 6 a2)))",
      "(set (reg:SI 3 v1) (xor:SI (reg:SI 5 a1) (reg:SI 7 a3)))",
      "(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))",
      "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))",
  };

  // Positive: bitwise `xordi3` folds directly, and witnessed `$8:$9` load folds to `movdi`.
  {
    std::vector<std::vector<RtlPatternAndNotes>> blocks = {ParseBlockInsns(xor_and_load_insns)};
    const absl::flat_hash_set<int> compare_pairs;
    const absl::flat_hash_set<int> existing_di = {8};
    MipsWitnessedDiFoldOptions opts;
    EXPECT_TRUE(TryFoldGp32DiWitnessedIdioms(cfg, /*frame_size=*/0, compare_pairs, existing_di,
                                             opts, &blocks));
    ASSERT_EQ(blocks[0].size(), 2u);
    EXPECT_TRUE(absl::StrContains(blocks[0][0].ToGccPrintRtlString(),
                                  "(set (reg:DI 2 v0) (xor:DI (reg:DI 4 a0) (reg:DI 6 a2)))"));
    EXPECT_TRUE(absl::StrContains(blocks[0][1].ToGccPrintRtlString(),
                                  "(set (reg:DI 8 t0) (mem:DI (reg:SI 4 a0)))"));
  }

  // Negative 1: epilogue callee-saved restores (`$s0/$s1` from `16($sp), 20($sp)` within
  // `frame_size = 32`) and unaligned stack loads (`4($sp), 8($sp)`) must NOT fold to `movdi`.
  {
    std::vector<std::vector<RtlPatternAndNotes>> blocks = {
        ParseBlockInsns({
            "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
            "(set (reg:SI 17 s1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
            "(set (reg:SI 8 t0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 4))))",
            "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 8))))",
        }),
    };
    const absl::flat_hash_set<int> empty_set;
    MipsWitnessedDiFoldOptions opts;
    EXPECT_FALSE(
        TryFoldGp32DiWitnessedIdioms(cfg, /*frame_size=*/32, empty_set, empty_set, opts, &blocks));
    EXPECT_EQ(blocks[0].size(), 4u);
  }

  // Negative 2: unwitnessed 2-word constant load into `$8:$9` (absent from `existing_di_regs`)
  // must NOT fold into `const_double:DI`.
  {
    std::vector<std::vector<RtlPatternAndNotes>> blocks = {
        ParseBlockInsns({
            "(set (reg:SI 8 t0) (const_int 1))",
            "(set (reg:SI 9 t1) (const_int 2))",
        }),
    };
    const absl::flat_hash_set<int> empty_set;
    MipsWitnessedDiFoldOptions opts;
    EXPECT_FALSE(
        TryFoldGp32DiWitnessedIdioms(cfg, /*frame_size=*/0, empty_set, empty_set, opts, &blocks));
    EXPECT_EQ(blocks[0].size(), 2u);
  }
}

TEST(MipsMultiInsnIdiomsTest, TryFoldMovstrsiBlockMovesNormalDelaySplitAndAliasingNegative) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;

  // 1. Positive (normal 16-byte 4-word `movstrsi_internal`, `split_kind = 0`).
  {
    std::vector<RtlPatternAndNotes> insns = ParseBlockInsns({
        "(set (reg:SI 8 t0) (mem:SI (reg:SI 5 a1)))",
        "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 5 a1) (const_int 4))))",
        "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 5 a1) (const_int 8))))",
        "(set (reg:SI 11 t3) (mem:SI (plus:SI (reg:SI 5 a1) (const_int 12))))",
        "(set (mem:SI (reg:SI 4 a0)) (reg:SI 8 t0))",
        "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SI 9 t1))",
        "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 8))) (reg:SI 10 t2))",
        "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 12))) (reg:SI 11 t3))",
    });
    MipsMovstrsiFoldOptions opts;
    EXPECT_TRUE(TryFoldMovstrsiBlockMovesInBlock(cfg, /*b=*/0, opts, &insns));
    ASSERT_EQ(insns.size(), 1u);
    const std::string s = insns[0].ToGccPrintRtlString();
    EXPECT_TRUE(absl::StrContains(s, "(mem:BLK (reg:SI 4 a0))")) << s;
    EXPECT_TRUE(absl::StrContains(s, "(mem:BLK (reg:SI 5 a1))")) << s;
    EXPECT_TRUE(absl::StrContains(s, "(use (const_int 16))")) << s;
    EXPECT_TRUE(absl::StrContains(s, "(use (const_int 0))")) << s;
  }

  // 2. Positive (delay-slot split: 3 loads + 2 stores + 3rd store in `(sequence ...)` slot
  //    folds to `movstrsi_internal2` (`split_kind = 1`) + `movstrsi_internal3` (`split_kind = 2`)).
  {
    std::vector<RtlPatternAndNotes> insns = ParseBlockInsns({
        "(set (reg:SI 8 t0) (mem:SI (reg:SI 5 a1)))",
        "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 5 a1) (const_int 4))))",
        "(set (reg:SI 10 t2) (mem:SI (plus:SI (reg:SI 5 a1) (const_int 8))))",
        "(set (mem:SI (reg:SI 4 a0)) (reg:SI 8 t0))",
        "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SI 9 t1))",
        "(sequence[ (jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
        "(insn (set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 8))) (reg:SI 10 t2))) ] )",
    });
    MipsMovstrsiFoldOptions opts;
    opts.allow_delay_slot_split_movstrsi = true;
    EXPECT_TRUE(TryFoldMovstrsiBlockMovesInBlock(cfg, /*b=*/0, opts, &insns));
    ASSERT_EQ(insns.size(), 2u);
    EXPECT_TRUE(IsMovstrsiSplitPart(insns[0].pattern, /*expected_split_kind=*/1))
        << insns[0].ToGccPrintRtlString();
    ASSERT_EQ(insns[1].pattern.op, RtlOp::kSequence);
    ASSERT_GE(insns[1].pattern.children.size(), 2u);
    EXPECT_TRUE(IsMovstrsiSplitPart(insns[1].pattern.children[1].children[0],
                                    /*expected_split_kind=*/2))
        << insns[1].ToGccPrintRtlString();
  }

  // 3. Negative (aliasing overlap: same base `$a0` with overlapping `[0..8)` vs `[4..12)`
  //    must NOT fold).
  {
    std::vector<RtlPatternAndNotes> insns = ParseBlockInsns({
        "(set (reg:SI 8 t0) (mem:SI (reg:SI 4 a0)))",
        "(set (reg:SI 9 t1) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))",
        "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SI 8 t0))",
        "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 8))) (reg:SI 9 t1))",
    });
    MipsMovstrsiFoldOptions opts;
    EXPECT_FALSE(TryFoldMovstrsiBlockMovesInBlock(cfg, /*b=*/0, opts, &insns));
    EXPECT_EQ(insns.size(), 4u);
  }
}

TEST(MipsMultiInsnIdiomsTest, TryFoldGp32DfInGprPairMovesPositiveAndDiWitnessedNegative) {
  RtlControlFlowGraph cfg;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;

  const std::vector<std::string_view> df_pair_insns = {
      "(set (reg:SI 6 a2) (mem:SI (reg:SI 4 a0)))",
      "(set (reg:SI 7 a3) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))",
      "(set (reg:DF 32 $f0) (reg:DF 6 a2))",
  };

  // Positive: `(reg:DF 6 a2)` witnesses `$a2:$a3` as `DFmode` when not in `existing_di_regs`.
  {
    std::vector<std::vector<RtlPatternAndNotes>> blocks = {ParseBlockInsns(df_pair_insns)};
    const absl::flat_hash_set<int> empty_set;
    EXPECT_TRUE(TryFoldGp32DfInGprPairMoves(cfg, /*frame_size=*/0, empty_set, empty_set,
                                            /*mark_newly_folded_sentinel=*/false, &blocks));
    ASSERT_EQ(blocks[0].size(), 2u);
    EXPECT_TRUE(absl::StrContains(blocks[0][0].ToGccPrintRtlString(),
                                  "(set (reg:DF 6 a2) (mem:DF (reg:SI 4 a0)))"));
  }

  // Negative: when `6` is in `existing_di_regs`, `DFmode` folding must skip it.
  {
    std::vector<std::vector<RtlPatternAndNotes>> blocks = {ParseBlockInsns(df_pair_insns)};
    const absl::flat_hash_set<int> empty_set;
    const absl::flat_hash_set<int> di_regs = {6};
    EXPECT_FALSE(TryFoldGp32DfInGprPairMoves(cfg, /*frame_size=*/0, empty_set, di_regs,
                                             /*mark_newly_folded_sentinel=*/false, &blocks));
    EXPECT_EQ(blocks[0].size(), 3u);
  }
}

TEST(MipsMultiInsnIdiomsTest, CollectCompareDiamondAndExistingDiAndDetectStackFrameSize) {
  // Ordered 64-bit compare diamond across `B0` (`lt:SI` hi), `B1` (`ne:SI` branch), `B2` (`ltu:SI`
  // lo).
  const std::vector<std::vector<RtlPatternAndNotes>> diamond_blocks = {
      ParseBlockInsns({"(set (reg:SI 8 t0) (lt:SI (reg:SI 4 a0) (reg:SI 6 a2)))"}),
      ParseBlockInsns(
          {"(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (reg:SI 6 a2)) (label_ref L1) (pc)))"}),
      ParseBlockInsns({"(set (reg:SI 8 t0) (ltu:SI (reg:SI 5 a1) (reg:SI 7 a3)))"}),
  };
  absl::flat_hash_set<int> cmp_pairs;
  CollectCompareDiamondDiPairs(diamond_blocks, &cmp_pairs);
  EXPECT_TRUE(cmp_pairs.contains(4));
  EXPECT_TRUE(cmp_pairs.contains(6));

  // Negative: without the preceding `B0` hi compare, plain `ne:SI` branch does NOT record DI pairs.
  const std::vector<std::vector<RtlPatternAndNotes>> plain_eq_blocks = {
      ParseBlockInsns({"(set (reg:SI 8 t0) (const_int 0))"}),
      ParseBlockInsns(
          {"(set (pc) (if_then_else (ne:SI (reg:SI 4 a0) (reg:SI 6 a2)) (label_ref L1) (pc)))"}),
      ParseBlockInsns({"(set (reg:SI 8 t0) (ltu:SI (reg:SI 5 a1) (reg:SI 7 a3)))"}),
  };
  absl::flat_hash_set<int> no_cmp_pairs;
  CollectCompareDiamondDiPairs(plain_eq_blocks, &no_cmp_pairs);
  EXPECT_TRUE(no_cmp_pairs.empty());

  // CollectExistingDiEvenRegs positive & negative.
  absl::flat_hash_set<int> di_regs;
  bool has_any_di = false;
  CollectExistingDiEvenRegs({ParseBlockInsns({"(set (reg:DI 4 a0) (reg:DI 6 a2))"})}, &di_regs,
                            &has_any_di);
  EXPECT_TRUE(has_any_di);
  EXPECT_TRUE(di_regs.contains(4));
  EXPECT_TRUE(di_regs.contains(6));

  absl::flat_hash_set<int> no_di_regs;
  bool has_no_di = false;
  CollectExistingDiEvenRegs({ParseBlockInsns({"(set (reg:SI 4 a0) (reg:SI 6 a2))"})}, &no_di_regs,
                            &has_no_di);
  EXPECT_FALSE(has_no_di);
  EXPECT_TRUE(no_di_regs.empty());

  // DetectStackFrameSize positive (`plus -48` and `minus 32`) & negative (leaf = 0).
  RtlControlFlowGraph frame_cfg;
  frame_cfg.rpo_blocks.resize(1);
  frame_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -48)))");
  EXPECT_EQ(DetectStackFrameSize(frame_cfg), 48);

  frame_cfg.rpo_blocks[0].instructions = {
      "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 32)))"};
  EXPECT_EQ(DetectStackFrameSize(frame_cfg), 32);

  frame_cfg.rpo_blocks[0].instructions = {"(set (reg:SI 2 v0) (const_int 0))"};
  EXPECT_EQ(DetectStackFrameSize(frame_cfg), 0);
}

TEST(MipsMultiInsnIdiomsTest, RestoreStage0DbrNoConflictExtendAndMuldi3AndGp64Negative) {
  // 1. Positive Stage-0 `.12.dbr` tail delay-slot folding: `extendsidi2` into `$v0/$v1`
  //    whose `DImode` store consumer has its second half sitting inside a trailing
  //    `(sequence [(jump_insn ...) (insn ...)])` delay slot.
  auto make_dbr_cfg = []() {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks.resize(1);
    cfg.rpo_blocks[0].id = 0;
    cfg.rpo_blocks[0].instructions = {
        "(set (reg:SI 3 v1) (reg:SI 4 a0))",
        "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))",
        "(set (mem:SI (reg:SI 6 a2)) (reg:SI 2 v0))",
        "(sequence[ (jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
        "(insn (set (mem:SI (plus:SI (reg:SI 6 a2) (const_int 4))) (reg:SI 3 v1))) ] )",
    };
    cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    return cfg;
  };
  {
    RtlControlFlowGraph cfg = make_dbr_cfg();
    RestoreStage0DbrNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false,
                                             /*runs_sched2=*/false);
    EXPECT_TRUE(absl::StrContains(cfg.FormatSummary(), "(clobber (reg:DI 2 v0))"))
        << cfg.FormatSummary();
  }
  // Negative (`-mgp64`): `RestoreStage0DbrNoConflictExtendClobbers` is a no-op under `gp64 = true`.
  {
    RtlControlFlowGraph cfg = make_dbr_cfg();
    const std::vector<std::string> orig = cfg.rpo_blocks[0].instructions;
    RestoreStage0DbrNoConflictExtendClobbers(&cfg, /*gp64=*/true, /*fp64=*/false,
                                             /*runs_sched2=*/false);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions, orig);
  }

  // 2. Positive + negative `muldi3` 3-multiply `SUBREG` operand restoration via
  //    `RestoreStage0DbrNoConflictExtendClobbers`.
  RtlControlFlowGraph mul_cfg;
  mul_cfg.pass = RtlPassKind::kDbr;
  mul_cfg.rpo_blocks.resize(1);
  mul_cfg.rpo_blocks[0].id = 0;
  mul_cfg.rpo_blocks[0].instructions = {
      "(parallel[ (set (reg:DI 64 hi) (mult:DI (zero_extend:DI (reg:SI 23 s7)) "
      "(zero_extend:DI (reg:SI 3 v1)))) (clobber (reg:SI 66 accum)) ] )",
      "(parallel[ (set (reg:SI 4 a0) (mult:SI (reg:SI 23 s7) (reg:SI 2 v0))) "
      "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] )",
      "(parallel[ (set (reg:SI 2 v0) (mult:SI (reg:SI 3 v1) (reg:SI 22 s6))) "
      "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
      "(use (reg/i:SI 2 v0))",
  };
  mul_cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  RestoreStage0DbrNoConflictExtendClobbers(&mul_cfg, /*gp64=*/false, /*fp64=*/false,
                                           /*runs_sched2=*/false);
  EXPECT_TRUE(
      absl::StrContains(mul_cfg.rpo_blocks[0].instructions[1], "(subreg:SI (reg:DI 2 v0) 0)"))
      << mul_cfg.FormatSummary();
  EXPECT_TRUE(absl::StrContains(mul_cfg.rpo_blocks[0].instructions[2], "REG_DEAD (reg:DI 2 v0)"))
      << mul_cfg.FormatSummary();
}

}  // namespace
}  // namespace rom_nom_nom::lifter
