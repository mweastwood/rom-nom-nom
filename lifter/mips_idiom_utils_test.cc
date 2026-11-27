#include "lifter/mips_idiom_utils.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::lifter {
namespace {

RtlPatternAndNotes MustParsePn(std::string_view sexpr) {
  auto parsed = ParsePatternAndNotes(sexpr);
  EXPECT_TRUE(parsed.ok()) << sexpr << ": " << parsed.status();
  return *std::move(parsed);
}

std::string MakeMovstrsiSexpr(int dst_base, int dst_off, int src_base, int src_off, int bytes,
                              int align, int split_kind) {
  auto fmt_addr = [](int base, int off) {
    const std::string reg = "(reg:SI " + std::to_string(base) + ")";
    if (off == 0) {
      return reg;
    }
    return "(plus:SI " + reg + " (const_int " + std::to_string(off) + "))";
  };
  return "(parallel[ (set (mem:BLK " + fmt_addr(dst_base, dst_off) + ") (mem:BLK " +
         fmt_addr(src_base, src_off) +
         ")) (clobber (reg:SI 8 t0)) (clobber (reg:SI 9 t1)) (clobber (reg:SI 10 t2)) "
         "(clobber (reg:SI 11 t3)) (use (const_int " +
         std::to_string(bytes) + ")) (use (const_int " + std::to_string(align) +
         ")) (use (const_int " + std::to_string(split_kind) + ")) ] )";
}

TEST(MipsIdiomUtilsTest, TryRecombineAdjacentSplitMovstrsiPositiveAndNegative) {
  // Positive 1: `movstrsi_internal2` (`split_kind = 1`) + non-conflicting ALU +
  // `movstrsi_internal3` (`split_kind = 2`) recombines into `split_kind = 0`
  // (`config/mips/mips.md:3587-3675`), and adjacent `16 + 8` byte batches
  // coalesce into a single 24-byte `movstrsi_internal` (`mips.c:2361`).
  std::vector<RtlPatternAndNotes> insns = {
      MustParsePn(MakeMovstrsiSexpr(/*dst_base=*/4, 0, /*src_base=*/5, 0, 16, 4, 0)),
      MustParsePn(MakeMovstrsiSexpr(/*dst_base=*/4, 16, /*src_base=*/5, 16, 8, 4, 1)),
      MustParsePn("(set (reg:SI 12 t4) (plus:SI (reg:SI 6 a2) (const_int 1)))"),
      MustParsePn(MakeMovstrsiSexpr(/*dst_base=*/4, 16, /*src_base=*/5, 16, 8, 4, 2)),
  };
  EXPECT_TRUE(TryRecombineAdjacentSplitMovstrsiInBlock(&insns,
                                                       /*mark_newly_folded_sentinel=*/false,
                                                       /*gp64=*/false));
  ASSERT_EQ(insns.size(), 2u);
  int dst_reg = -1;
  int src_reg = -1;
  int64_t dst_off = -1;
  int64_t src_off = -1;
  int total_bytes = 0;
  ASSERT_TRUE(ExtractMovstrsiAddrsAndBytes(insns[0].pattern, &dst_reg, &dst_off, &src_reg, &src_off,
                                           &total_bytes));
  EXPECT_EQ(dst_reg, 4);
  EXPECT_EQ(dst_off, 0);
  EXPECT_EQ(src_reg, 5);
  EXPECT_EQ(src_off, 0);
  EXPECT_EQ(total_bytes, 24);
  EXPECT_TRUE(IsMovstrsiSplitPart(insns[0].pattern, 0));

  // Negative 1: an intervening write to `dst_base` ($a0 = 4) blocks recombination.
  std::vector<RtlPatternAndNotes> blocked_base = {
      MustParsePn(MakeMovstrsiSexpr(/*dst_base=*/4, 0, /*src_base=*/5, 0, 8, 4, 1)),
      MustParsePn("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 8)))"),
      MustParsePn(MakeMovstrsiSexpr(/*dst_base=*/4, 0, /*src_base=*/5, 0, 8, 4, 2)),
  };
  EXPECT_FALSE(TryRecombineAdjacentSplitMovstrsiInBlock(&blocked_base,
                                                        /*mark_newly_folded_sentinel=*/false,
                                                        /*gp64=*/false));
  EXPECT_EQ(blocked_base.size(), 3u);

  // Negative 2: an intervening memory access blocks recombination.
  std::vector<RtlPatternAndNotes> blocked_mem = {
      MustParsePn(MakeMovstrsiSexpr(/*dst_base=*/4, 0, /*src_base=*/5, 0, 8, 4, 1)),
      MustParsePn("(set (reg:SI 12 t4) (mem:SI (reg:SI 6 a2)))"),
      MustParsePn(MakeMovstrsiSexpr(/*dst_base=*/4, 0, /*src_base=*/5, 0, 8, 4, 2)),
  };
  EXPECT_FALSE(TryRecombineAdjacentSplitMovstrsiInBlock(&blocked_mem,
                                                        /*mark_newly_folded_sentinel=*/false,
                                                        /*gp64=*/false));
  EXPECT_EQ(blocked_mem.size(), 3u);
}

TEST(MipsIdiomUtilsTest, RestoreNoConflictExtendClobbersPositiveNegativeAndIdempotent) {
  // Positive 1: Disjoint `extendsidi2` (`lo = src; hi = ashiftrt(src, 31)`)
  // restores `(clobber (reg:DI 18 s2)) (insn_list:REG_LIBCALL (nil))` and
  // `(subreg:SI (reg:DI 18 s2) 1/0)`, and re-running is strictly idempotent.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].instructions = {
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
      "(set (reg:SI 19 s3) (reg:SI 2 v0))",
      "(set (reg:SI 18 s2) (ashiftrt:SI (reg:SI 2 v0) (const_int 31)))",
      "(set (reg/i:DI 2 v0) (xor:DI (reg:DI 18 s2) (reg:DI 6 a2)))",
      "(use (reg/i:DI 2 v0))",
  };
  cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();

  RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
  ASSERT_EQ(cfg.rpo_blocks[0].instructions.size(), 6u);
  EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[1],
                                "(clobber (reg:DI 18 s2)) (insn_list:REG_LIBCALL (nil))"))
      << cfg.FormatSummary();
  EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[2],
                                "(set (subreg:SI (reg:DI 18 s2) 1) (reg:SI 2 v0))"))
      << cfg.FormatSummary();
  EXPECT_TRUE(absl::StrContains(cfg.rpo_blocks[0].instructions[3],
                                "(set (subreg:SI (reg:DI 18 s2) 0) (ashiftrt:SI (reg:SI 2 v0)"))
      << cfg.FormatSummary();

  // Idempotency: running a second time must not insert a duplicate clobber.
  const std::vector<std::string> after_first = cfg.rpo_blocks[0].instructions;
  RestoreNoConflictExtendClobbers(&cfg, /*gp64=*/false, /*fp64=*/false, /*runs_sched2=*/true);
  EXPECT_EQ(cfg.rpo_blocks[0].instructions, after_first);

  // Negative 1: Without a `DImode` consumer of `(reg:DI 18 s2)`, no clobber is added.
  RtlControlFlowGraph unconsumed;
  unconsumed.pass = RtlPassKind::kSched2;
  unconsumed.rpo_blocks.resize(1);
  unconsumed.rpo_blocks[0].id = 0;
  unconsumed.rpo_blocks[0].instructions = {
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
      "(set (reg:SI 19 s3) (reg:SI 2 v0))",
      "(set (reg:SI 18 s2) (ashiftrt:SI (reg:SI 2 v0) (const_int 31)))",
      "(set (reg/i:SI 2 v0) (reg:SI 19 s3))",
  };
  unconsumed.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  RestoreNoConflictExtendClobbers(&unconsumed, /*gp64=*/false, /*fp64=*/false,
                                  /*runs_sched2=*/true);
  EXPECT_EQ(unconsumed.rpo_blocks[0].instructions.size(), 4u);

  // Negative 2: Under `gp64 = true`, `!TARGET_64BIT` no-conflict restoration is a no-op.
  RtlControlFlowGraph gp64_cfg;
  gp64_cfg.pass = RtlPassKind::kSched2;
  gp64_cfg.rpo_blocks.resize(1);
  gp64_cfg.rpo_blocks[0].id = 0;
  gp64_cfg.rpo_blocks[0].instructions = {
      "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
      "(set (reg:SI 19 s3) (reg:SI 2 v0))",
      "(set (reg:SI 18 s2) (ashiftrt:SI (reg:SI 2 v0) (const_int 31)))",
      "(set (reg/i:DI 2 v0) (xor:DI (reg:DI 18 s2) (reg:DI 6 a2)))",
  };
  gp64_cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
  RestoreNoConflictExtendClobbers(&gp64_cfg, /*gp64=*/true, /*fp64=*/false, /*runs_sched2=*/true);
  EXPECT_EQ(gp64_cfg.rpo_blocks[0].instructions.size(), 4u);
}

TEST(MipsIdiomUtilsTest, ComputeHardRegLiveInTracksCalleeSavedAcrossCalls) {
  // B0: calls `foo` (clobbers `$t0` = 8, preserves `$s0` = 16) -> B1
  // B1: reads `$s0` (16) and `$t0` (8), sets `$v0` (2), returns.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kSched2;
  cfg.rpo_blocks.resize(2);
  cfg.rpo_blocks[0].id = 0;
  cfg.rpo_blocks[0].instructions = {
      "(parallel[ (call (mem:SI (symbol_ref:SI (\"foo\"))) (const_int 16)) "
      "(clobber (reg:SI 31 ra)) ] )",
  };
  cfg.rpo_blocks[0].successors = {1};
  cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();

  cfg.rpo_blocks[1].id = 1;
  cfg.rpo_blocks[1].instructions = {
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 16 s0) (reg:SI 8 t0)))",
      "(use (reg/i:SI 2 v0))",
  };
  cfg.rpo_blocks[1].SyncRtlInsnsFromInstructions();

  const std::vector<MipsHardRegSet> live_in =
      ComputeHardRegLiveIn(cfg, /*gp64=*/false, /*fp64=*/false);
  ASSERT_EQ(live_in.size(), 2u);
  // Both `$s0` and `$t0` are live into B1.
  EXPECT_TRUE(live_in[1].Test(16));
  EXPECT_TRUE(live_in[1].Test(8));
  // Callee-saved `$s0` propagates across the call into B0; caller-saved `$t0` is killed by call.
  EXPECT_TRUE(live_in[0].Test(16));
  EXPECT_FALSE(live_in[0].Test(8));
}

}  // namespace
}  // namespace rom_nom_nom::lifter
