#include "lifter/pass03_sched2_post_reload.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_do_cross_jump.h"
#include "lifter/invert_fill_eager_delay_slots.h"
#include "lifter/invert_fill_simple_delay_slots.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"
#include "lifter/invert_gas_mips_ip.h"
#include "lifter/invert_make_return_insns.h"
#include "lifter/invert_relax_delay_slots.h"
#include "lifter/invert_schedule_block.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/stage0_test_utils.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::PrologueEpilogueFrameSpec;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ProductionPass03InverseProvider final : public SubfunctionInverseProvider {
 public:
  std::string InvertGasMipsIp(uint32_t word_x) const override {
    return rom_nom_nom::InvertGasMipsIp(word_x);
  }

  std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x) const override {
    return rom_nom_nom::InvertGasMacro(macro_words_x);
  }

  std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) const override {
    return rom_nom_nom::InvertGasAppendInsn(words_x);
  }

  std::string InvertFinalScanInsn(std::string_view asm_line_x) const override {
    return lifter::InvertFinalScanInsn(asm_line_x);
  }

  PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(
      std::string_view asm_text_x) const override {
    const auto spec = lifter::InvertFunctionPrologueEpilogue(asm_text_x);
    return {
        .var_size = spec.var_size,
        .saved_gpr_mask = spec.saved_gpr_mask,
        .saved_fpr_mask = spec.saved_fpr_mask,
    };
  }

  RtlControlFlowGraph InvertFinalAndGasFunction(
      absl::Span<const uint32_t> machine_words_x) const override {
    return lifter::InvertFinalAndGasFunction(machine_words_x);
  }

  RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x) const override {
    return lifter::InvertFinalAndGasFunction(input_x);
  }

  RtlControlFlowGraph InvertMakeReturnInsns(
      const RtlControlFlowGraph& dbr_return_cfg_x) const override {
    return lifter::InvertMakeReturnInsns(dbr_return_cfg_x);
  }

  RtlControlFlowGraph InvertRelaxDelaySlots(
      const RtlControlFlowGraph& dbr_relaxed_cfg_x) const override {
    return lifter::InvertRelaxDelaySlots(dbr_relaxed_cfg_x);
  }

  RtlControlFlowGraph InvertFillEagerDelaySlots(
      const RtlControlFlowGraph& dbr_eager_cfg_x) const override {
    return lifter::InvertFillEagerDelaySlots(dbr_eager_cfg_x);
  }

  RtlControlFlowGraph InvertFillSimpleDelaySlots(
      const RtlControlFlowGraph& dbr_simple_cfg_x) const override {
    return lifter::InvertFillSimpleDelaySlots(dbr_simple_cfg_x);
  }

  RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& dbr_simple_cfg_x,
                                                 const CompilationProfile& profile) const override {
    return lifter::InvertFillSimpleDelaySlots(dbr_simple_cfg_x, profile);
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) const override {
    return lifter::InvertPass01DbrSchedule(dbr_cfg_x);
  }

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x,
                                        const CompilationProfile& profile) const override {
    return lifter::InvertPass01DbrSchedule(dbr_cfg_x, profile);
  }

  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) const override {
    return lifter::InvertPass02Jump2CrossJump(jump2_cfg_x);
  }

  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                        const CompilationProfile& profile) const override {
    return lifter::InvertPass02Jump2CrossJump(jump2_cfg_x, profile);
  }

  RtlControlFlowGraph InvertScheduleBlock(
      const RtlControlFlowGraph& scheduled_cfg_x) const override {
    return lifter::InvertScheduleBlock(scheduled_cfg_x);
  }

  RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg_x,
                                          const CompilationProfile& profile) const override {
    return lifter::InvertScheduleBlock(scheduled_cfg_x, profile);
  }

  RtlControlFlowGraph InvertSched2PostReload(const RtlControlFlowGraph& scheduled_cfg_x,
                                             const CompilationProfile& profile,
                                             bool /*sched2_enabled*/) const override {
    return lifter::InvertPass03Sched2PostReload(scheduled_cfg_x, profile);
  }
};

RtlBasicBlock MakeBlock(uint32_t id, bool has_label, std::vector<std::string> instructions,
                        std::vector<size_t> successors) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.has_label = has_label;
  blk.instructions = std::move(instructions);
  blk.successors = std::move(successors);
  blk.SyncRtlInsnsFromInstructions();
  return blk;
}

class Pass03Sched2PostReloadTest : public ::testing::Test {
 protected:
  Pass03Sched2PostReloadTest() : hooks_(&ctx_), verifier_(&ctx_, &hooks_, &provider_) {}

  GccContext ctx_;
  FunctionHooks hooks_;
  ProductionPass03InverseProvider provider_;
  RightInverseVerifier verifier_;
};

TEST_F(Pass03Sched2PostReloadTest, PositiveOptimizationProfilesAndPrologueVariants) {
  constexpr std::string_view kSched2CalleeSavedSrc = R"(
extern double ext_step(int, double);
int sched2_callee_saved(const int* arr, int a, double d) {
  int x = arr[0] + arr[1];
  double y = ext_step(x + a, d * 2.0);
  return x + (int)y + a;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O1", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O3", "-mips2", "-mcpu=r4000", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips1", "-Wa,-O2"}}) {
    const auto trace = hooks_.TraceForwardCompilation(kSched2CalleeSavedSrc, "sched2_callee_saved",
                                                      flags, "pass03_opt_profiles");
    bool found_snap = false;
    for (const auto& snap : trace.subfunction_snapshots) {
      if (snap.subfunction_id != SubfunctionId::kScheduleBlockSched2) {
        continue;
      }
      found_snap = true;
      const RtlControlFlowGraph y3 =
          InvertPass03Sched2PostReload(snap.post_state, trace.stage0_input.profile);
      ASSERT_FALSE(y3.rpo_blocks.empty());
      EXPECT_EQ(y3.pass, RtlPassKind::kGreg);
      const SubfunctionBoundarySnapshot replayed = hooks_.ExecuteSubfunctionBoundary(snap, y3);
      const RtlCfgPassComparison cmp = replayed.post_state.CompareAgainst(snap.post_state);
      EXPECT_TRUE(cmp.MatchesExactly()) << cmp.mismatch_detail;
    }
    EXPECT_TRUE(found_snap);

    const RightInverseResult suffix_res =
        verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, trace.stage0_input);
    EXPECT_TRUE(suffix_res.matches) << suffix_res.diff_detail;
  }

  // `< 4`-byte struct arguments (`tsize == 0` leaf and `tsize > 0` non-leaf),
  // variadic home stores (`tsize == 0` and `tsize > 0`), and `> 32KB` stack frame.
  struct PrologueEdgeCase {
    std::string_view fn_name;
    std::string_view c_src;
  };
  constexpr PrologueEdgeCase kEdgeCases[] = {
      {
          "leaf_small_structs",
          R"(
struct Tiny1 { unsigned char a; };
struct Tiny2 { unsigned char a, b; };
struct Tiny3 { unsigned char a, b, c; };
int leaf_small_structs(struct Tiny1 s1, struct Tiny2 s2, struct Tiny3 s3) {
  return (int)s1.a + (int)s2.b + (int)s3.c;
}
)",
      },
      {
          "leaf_variadic",
          R"(
void* __builtin_next_arg();
int leaf_variadic(int tag, ...) {
  int* ap = (int*)__builtin_next_arg(tag);
  return tag + ap[0] + ap[1];
}
)",
      },
      {
          "huge_frame_fn",
          R"(
extern int sink_buf(int*, int);
int huge_frame_fn(int seed, int idx) {
  int buf[9000];
  buf[0] = seed + 3;
  buf[8999] = seed - 5;
  buf[idx] = seed ^ 0x5a;
  return sink_buf(buf, buf[0] + buf[8999]);
}
)",
      },
  };
  for (const auto& edge : kEdgeCases) {
    for (const std::vector<std::string>& flags :
         {std::vector<std::string>{"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"},
          std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"}}) {
      const auto trace =
          hooks_.TraceForwardCompilation(edge.c_src, edge.fn_name, flags, edge.fn_name);
      for (const auto& snap : trace.subfunction_snapshots) {
        if (snap.subfunction_id != SubfunctionId::kScheduleBlockSched2) {
          continue;
        }
        const RtlControlFlowGraph y3 =
            InvertPass03Sched2PostReload(snap.post_state, trace.stage0_input.profile);
        ASSERT_FALSE(y3.rpo_blocks.empty()) << edge.fn_name;
        const SubfunctionBoundarySnapshot replayed = hooks_.ExecuteSubfunctionBoundary(snap, y3);
        const RtlCfgPassComparison cmp = replayed.post_state.CompareAgainst(snap.post_state);
        EXPECT_TRUE(cmp.MatchesExactly()) << edge.fn_name << ": " << cmp.mismatch_detail;
      }
      const RightInverseResult suffix_res =
          verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, trace.stage0_input);
      EXPECT_TRUE(suffix_res.matches) << edge.fn_name << ": " << suffix_res.diff_detail;
    }
  }
}

TEST_F(Pass03Sched2PostReloadTest, NegativeRejectsEmptyAndMalformedCfgsAcrossProfiles) {
  for (const CompilationProfile& prof :
       {CompilationProfile{.optimize_level = 0, .c_flags = {"-O0"}},
        CompilationProfile{.optimize_level = 1, .c_flags = {"-O1"}},
        CompilationProfile{.optimize_level = 2, .c_flags = {"-O2"}}}) {
    RtlControlFlowGraph empty;
    EXPECT_TRUE(InvertPass03Sched2PostReload(empty, prof).rpo_blocks.empty());

    RtlControlFlowGraph bad_succ;
    bad_succ.pass = RtlPassKind::kSched2;
    bad_succ.rpo_blocks.push_back(
        MakeBlock(0, false, {"(set (reg:SI 2) (const_int 0))"}, /*successors=*/{5}));
    EXPECT_TRUE(InvertPass03Sched2PostReload(bad_succ, prof).rpo_blocks.empty());

    RtlControlFlowGraph unknown_op;
    unknown_op.pass = RtlPassKind::kSched2;
    unknown_op.rpo_blocks.push_back(MakeBlock(0, false, {"(bogus_rtl_op (reg:SI 2))"}, {}));
    EXPECT_TRUE(InvertPass03Sched2PostReload(unknown_op, prof).rpo_blocks.empty());

    RtlControlFlowGraph bare_ret;
    bare_ret.pass = RtlPassKind::kSched2;
    bare_ret.rpo_blocks.push_back(MakeBlock(0, false, {"(return)"}, {}));
    EXPECT_TRUE(InvertPass03Sched2PostReload(bare_ret, prof).rpo_blocks.empty());
  }
}

TEST_F(Pass03Sched2PostReloadTest, LinkedRomAndNegativeSuffixRightInverse) {
  constexpr std::string_view kLinkedMultiSwitchSched2Src = R"(
struct StateBox {
  int a;
  int b;
  int c;
};
extern struct StateBox g_box;
int g_small_val;
extern int mix_fn(int, double);

int dual_switch_sched2(int n, const int* s1, const int* s2, double v) {
  int res = g_box.c + g_small_val;
  int i;
  for (i = 0; i < n; ++i) {
    switch (s1[i]) {
      case 0: res = mix_fn(res + 1, v * 3.141592653589793) + 9; break;
      case 1: res = mix_fn(res - 2, v + 2.718281828459045) + 9; break;
      case 2: res += g_box.b; break;
      case 3: res -= g_small_val; break;
      case 4: res ^= 0x12345678; break;
      default: res += 3; break;
    }
    switch (s2[i]) {
      case 0: res += 11; break;
      case 1: res -= 22; break;
      case 2: res ^= 33; break;
      case 3: res += g_box.a; break;
      case 4: res -= 44; break;
      default: break;
    }
  }
  g_small_val = res;
  return res;
}
)";
  for (const std::vector<std::string>& flags :
       {std::vector<std::string>{"-O2", "-mips1", "-G", "8", "-Wa,-g"},
        std::vector<std::string>{"-O2", "-mips2", "-mcpu=r4000", "-G", "8", "-Wa,-g"}}) {
    const auto ms_trace = hooks_.TraceForwardCompilation(
        kLinkedMultiSwitchSched2Src, "dual_switch_sched2", flags, "pass03_linked_multi_switch");
    ASSERT_GE(ms_trace.stage0_input.jump_tables.size(), 2u);

    const RightInverseResult reloc_res =
        verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, ms_trace.stage0_input);
    EXPECT_TRUE(reloc_res.matches) << reloc_res.diff_detail;

    // Negative check: corrupting a machine word must fail `VerifySuffixRightInverse(kGreg, ...)`.
    Stage0FunctionInput corrupted = ms_trace.stage0_input;
    corrupted.machine_words.back() ^= 0x00000004u;
    EXPECT_FALSE(verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, corrupted).matches);

    // Simulate linked N64 ROM (`base_vram != 0`, empty relocations, reversed `.rodata` JTs).
    const Stage0FunctionInput linked = BakeLinkedRomStage0Input(ms_trace.stage0_input);

    const RightInverseResult linked_greg_res =
        verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, linked);
    EXPECT_TRUE(linked_greg_res.matches) << linked_greg_res.diff_detail;

    if (!linked.constant_pool.empty()) {
      Stage0FunctionInput bad_cp = linked;
      bad_cp.constant_pool[0].entry_vram += 32u;
      EXPECT_FALSE(verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, bad_cp).matches);
    }

    Stage0FunctionInput bad_jt = linked;
    bad_jt.jump_tables[0].table_vram += 128u;
    EXPECT_FALSE(verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, bad_jt).matches);
  }
}

size_t CountBlockZeroNegativeSpAdjustments(const RtlControlFlowGraph& cfg) {
  if (cfg.rpo_blocks.empty()) {
    return 0;
  }
  size_t count = 0;
  for (const std::string& pat_str : cfg.rpo_blocks[0].instructions) {
    const auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr& pat = parsed->pattern;
    if (pat.IsSet() && pat.SetDest().IsReg() && pat.SetDest().RegNo() == 29 &&
        (pat.SetSrc().op == RtlOp::kPlus || pat.SetSrc().op == RtlOp::kMinus) &&
        pat.SetSrc().children.size() == 2 && pat.SetSrc().children[0].IsReg() &&
        pat.SetSrc().children[0].RegNo() == 29) {
      const RtlExpr& rhs = pat.SetSrc().children[1];
      if ((pat.SetSrc().op == RtlOp::kMinus &&
           ((rhs.IsConstInt() && rhs.int_val > 0) || (rhs.IsReg() && rhs.RegNo() == 12))) ||
          (pat.SetSrc().op == RtlOp::kPlus && rhs.IsConstInt() && rhs.int_val < 0)) {
        ++count;
      }
    }
  }
  return count;
}

bool ContainsPrologueEndNote(const RtlControlFlowGraph& cfg) {
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& pat_str : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat_str);
      if (!parsed.ok() || parsed->pattern.op != RtlOp::kNote) {
        continue;
      }
      for (const auto& item : parsed->pattern.generic_items) {
        if (item.kind == RtlExpr::GenericItem::Kind::kBareAtom &&
            item.text == "NOTE_INSN_PROLOGUE_END") {
          return true;
        }
      }
    }
  }
  return false;
}

TEST_F(Pass03Sched2PostReloadTest, NegativeUnthreadedPrologueProviderRejectedBySuffixVerifier) {
  class UnthreadedPrologueStage3Provider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertFinalAndGasFunction(
        const Stage0FunctionInput& input_x) const override {
      return lifter::InvertFinalAndGasFunction(input_x);
    }
    RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x,
                                          const CompilationProfile& profile) const override {
      return lifter::InvertPass01DbrSchedule(dbr_cfg_x, profile);
    }
    RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                          const CompilationProfile& profile) const override {
      return lifter::InvertPass02Jump2CrossJump(jump2_cfg_x, profile);
    }
    RtlControlFlowGraph InvertSched2PostReload(const RtlControlFlowGraph& scheduled_cfg_x,
                                               const CompilationProfile& profile,
                                               bool sched2_enabled) const override {
      // Deliberately omit `InvertThreadPrologueAndEpilogueInsns`: must be rejected when `tsize >
      // 0`.
      RtlControlFlowGraph bad =
          sched2_enabled ? lifter::InvertScheduleBlock(scheduled_cfg_x, profile) : scheduled_cfg_x;
      bad.pass = RtlPassKind::kGreg;
      return bad;
    }
  };

  constexpr std::string_view kNonZeroFrameSrc = R"(
extern int ext_callee(int, int, int, int, int, int);
int non_zero_frame_caller(int a, int b) {
  int local_buf[4];
  local_buf[0] = a + 1;
  local_buf[3] = b - 2;
  return ext_callee(local_buf[0], local_buf[3], a, b, a ^ b, a + b) + local_buf[0];
}
)";
  const auto trace = hooks_.TraceForwardCompilation(kNonZeroFrameSrc, "non_zero_frame_caller",
                                                    {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"},
                                                    "pass03_unthreaded_neg");
  EXPECT_TRUE(verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, trace.stage0_input).matches);

  UnthreadedPrologueStage3Provider broken_provider;
  RightInverseVerifier broken_verifier(&ctx_, &hooks_, &broken_provider);
  const RightInverseResult broken_res =
      broken_verifier.VerifySuffixRightInverse(RtlPassKind::kGreg, trace.stage0_input);
  EXPECT_FALSE(broken_res.matches);
}

TEST_F(Pass03Sched2PostReloadTest,
       PositiveNoDelayedBranchStructCopyAndDiArithmeticSuffixRightInverse) {
  constexpr std::string_view kNoDbrStructAndDiSrc = R"(
struct Packet16 {
  int w0;
  int w1;
  int w2;
  int w3;
};
extern long long ext_di_sink(struct Packet16*, long long, int, int, int);
long long nodbr_struct_and_di(struct Packet16* dst, const struct Packet16* src,
                              long long x, long long y) {
  struct Packet16 tmp = *src;
  long long sum = (x + y) - (x << 5);
  long long masked = (sum & y) ^ (x >> 3);
  *dst = tmp;
  return ext_di_sink(&tmp, masked, tmp.w0, tmp.w1, tmp.w3) + sum;
}
)";
  const std::vector<std::string> flags = {"-O2", "-fno-delayed-branch", "-mips2", "-mcpu=r4000",
                                          "-Wa,-g"};
  const auto trace = hooks_.TraceForwardCompilation(kNoDbrStructAndDiSrc, "nodbr_struct_and_di",
                                                    flags, "pass03_no_delayed_branch");

  // Stage 0 alone (when `-fno-delayed-branch` skips `dbr_schedule`) must already fold
  // `movstrsi_internal` and `DImode` arithmetic templates.
  const RtlControlFlowGraph y0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(y0.rpo_blocks.empty());
  const RtlControlFlowGraph y1 = InvertPass01DbrSchedule(y0, trace.stage0_input.profile);
  const RtlControlFlowGraph y2 = InvertPass02Jump2CrossJump(y1, trace.stage0_input.profile);
  const RtlControlFlowGraph y3 = InvertPass03Sched2PostReload(y2, trace.stage0_input.profile);
  ASSERT_FALSE(y3.rpo_blocks.empty());

  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kScheduleBlockSched2) {
      EXPECT_EQ(y3.sideband.outgoing_args_size, snap.pre_state.sideband.outgoing_args_size);
      EXPECT_EQ(y3.sideband.frame_size_bytes, (snap.pre_state.sideband.frame_size_bytes + 7) & ~7);
    }
  }

  const RightInverseResult suffix_res =
      verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, trace.stage0_input);
  EXPECT_TRUE(suffix_res.matches) << suffix_res.diff_detail;
}

TEST_F(Pass03Sched2PostReloadTest, MdRegsDiExtendAndNarrowSignExtendSuffixRightInverse) {
  // Deterministic regression test for `-O2 -mips1` `MD_REGS` (`hi:lo`) `DImode`
  // `convert_move` allocation (`mtlo`/`mthi` -> `mfhi`/`mflo`) combined with
  // narrow `HImode -> DImode` signed extension (`sll $6, $6, 16; sra $5, $6, 16;
  // sra $4, $6, 31`) where `jump2` (`noop_moves = 1`) deleted reload's redundant
  // `(set (reg:DI 64) (reg:DI 10))` output reload after `sched2` scheduled the
  // two `SImode` shifts into its 3-cycle `imuldiv` stall window.
  Stage0FunctionInput input;
  input.profile = CompilationProfile{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"},
  };
  input.machine_words = {
      0x00a04021u, 0x00800013u, 0x000427c3u, 0x01004821u, 0x00004021u, 0x00800011u,
      0x00063400u, 0x00005010u, 0x00005812u, 0x00062c03u, 0x000627c3u, 0x000a1100u,
      0x000b3702u, 0x00461025u, 0x000b1900u, 0x00691821u, 0x0069302bu, 0x00481021u,
      0x00461021u, 0x0065302bu, 0x00651823u, 0x00441023u, 0x03e00008u, 0x00461023u,
  };

  const RightInverseResult sched2_res =
      verifier_.VerifySuffixRightInverse(RtlPassKind::kSched2, input);
  EXPECT_TRUE(sched2_res.matches) << sched2_res.diff_detail << "\nexpected:\n"
                                  << sched2_res.expected_summary << "\nactual:\n"
                                  << sched2_res.actual_summary;

  const RightInverseResult greg_res = verifier_.VerifySuffixRightInverse(RtlPassKind::kGreg, input);
  EXPECT_TRUE(greg_res.matches) << greg_res.diff_detail << "\nexpected:\n"
                                << greg_res.expected_summary << "\nactual:\n"
                                << greg_res.actual_summary;
}

class Pass03Sched2PostReloadRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
    provider = new ProductionPass03InverseProvider();
    verifier = new RightInverseVerifier(ctx, hooks, provider);

    RtlControlFlowGraph dummy;
    hooks->ExecuteSubfunctionBoundary(SubfunctionId::kScheduleBlockSched2, dummy, {}, 2,
                                      {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
  static ProductionPass03InverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* Pass03Sched2PostReloadRoundTripTest::ctx = nullptr;
FunctionHooks* Pass03Sched2PostReloadRoundTripTest::hooks = nullptr;
ProductionPass03InverseProvider* Pass03Sched2PostReloadRoundTripTest::provider = nullptr;
RightInverseVerifier* Pass03Sched2PostReloadRoundTripTest::verifier = nullptr;

TEST_P(Pass03Sched2PostReloadRoundTripTest, RoundTrip4172Cases) {
  const RoundTripTestCase& tc = GetParam();
  const auto cached_trace = gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const gcc_harness::FunctionCompilationTrace& trace = **cached_trace;

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();

  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << "Test case: " << tc.test_name;
  const RtlControlFlowGraph lifted_y0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(lifted_y0.rpo_blocks.empty()) << "Empty Pass00 output for " << tc.test_name;
  const RtlControlFlowGraph lifted_y1 =
      InvertPass01DbrSchedule(lifted_y0, trace.stage0_input.profile);
  ASSERT_FALSE(lifted_y1.rpo_blocks.empty()) << "Empty Pass01 output for " << tc.test_name;
  const RtlControlFlowGraph lifted_y2 =
      InvertPass02Jump2CrossJump(lifted_y1, trace.stage0_input.profile);
  ASSERT_FALSE(lifted_y2.rpo_blocks.empty()) << "Empty Pass02 output for " << tc.test_name;
  const RtlControlFlowGraph lifted_y3 =
      InvertPass03Sched2PostReload(lifted_y2, trace.stage0_input.profile);
  ASSERT_FALSE(lifted_y3.rpo_blocks.empty())
      << "Empty fresh Pass00..Pass03 lifted CFG for " << tc.test_name;

  // 1. Verify isolated Stage 3 (`SubfunctionId::kScheduleBlockSched2`) right-inverse
  //    ($F_3(G_3(x_3)) = x_3$) via `InvertPass03Sched2PostReload`.
  bool found_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != SubfunctionId::kScheduleBlockSched2) {
      continue;
    }
    found_snap = true;
    const RtlControlFlowGraph y3 =
        InvertPass03Sched2PostReload(snap.post_state, trace.stage0_input.profile);
    ASSERT_FALSE(y3.rpo_blocks.empty())
        << "Empty InvertPass03Sched2PostReload for " << tc.test_name;
    EXPECT_EQ(y3.pass, RtlPassKind::kGreg);
    EXPECT_EQ(ContainsPrologueEndNote(y3), ContainsPrologueEndNote(snap.pre_state))
        << "Unexpected NOTE_INSN_PROLOGUE_END in y3 for " << tc.test_name;
    EXPECT_EQ(CountBlockZeroNegativeSpAdjustments(y3),
              CountBlockZeroNegativeSpAdjustments(snap.pre_state))
        << "Unthreaded prologue $sp decrement left in block 0 of y3 for " << tc.test_name;
    EXPECT_EQ(y3.sideband.outgoing_args_size, snap.pre_state.sideband.outgoing_args_size)
        << "Stage 3 outgoing_args_size mismatch for " << tc.test_name;
    EXPECT_EQ(y3.sideband.frame_size_bytes, (snap.pre_state.sideband.frame_size_bytes + 7) & ~7)
        << "Stage 3 frame_size_bytes mismatch for " << tc.test_name;

    const SubfunctionBoundarySnapshot replayed = hooks->ExecuteSubfunctionBoundary(snap, y3);
    const RtlCfgPassComparison cmp = replayed.post_state.CompareAgainst(snap.post_state);
    EXPECT_TRUE(cmp.MatchesExactly())
        << "Stage 3 (.10.sched2) right-inverse mismatch for " << tc.test_name << ":\n"
        << cmp.mismatch_detail << "\nexpected (.10.sched2):\n"
        << snap.post_state.FormatSummary() << "\nactual (.10.sched2):\n"
        << replayed.post_state.FormatSummary() << "\nlifted (.09.greg):\n"
        << y3.FormatSummary();

    ctx->BeginFunction("harness_fresh_sched2");
    hooks->ApplyCompilationProfile(trace.stage0_input.profile);
    regclass_init();
    const RightInverseResult fresh_sched2_res = verifier->VerifySched2StageRightInverse(lifted_y2);
    EXPECT_TRUE(fresh_sched2_res.matches)
        << "Fresh Stage 0->2 (.10.sched2) VerifySched2StageRightInverse mismatch for "
        << tc.test_name << ":\n"
        << fresh_sched2_res.diff_detail << "\nexpected (fresh lifted_y2):\n"
        << fresh_sched2_res.expected_summary << "\nactual:\n"
        << fresh_sched2_res.actual_summary;
    EXPECT_EQ(ContainsPrologueEndNote(lifted_y3), ContainsPrologueEndNote(snap.pre_state))
        << "Unexpected NOTE_INSN_PROLOGUE_END in fresh lifted_y3 for " << tc.test_name;
    EXPECT_EQ(CountBlockZeroNegativeSpAdjustments(lifted_y3),
              CountBlockZeroNegativeSpAdjustments(snap.pre_state))
        << "Unthreaded prologue $sp decrement in fresh lifted_y3 for " << tc.test_name;
    EXPECT_EQ(lifted_y3.sideband.outgoing_args_size, snap.pre_state.sideband.outgoing_args_size)
        << "Fresh Pass00..Pass03 outgoing_args_size mismatch for " << tc.test_name;
    EXPECT_EQ(lifted_y3.sideband.frame_size_bytes,
              (snap.pre_state.sideband.frame_size_bytes + 7) & ~7)
        << "Fresh Pass00..Pass03 frame_size_bytes mismatch for " << tc.test_name;

    // Verify that `lifted_y3`:
    // 1. Never emits a spurious exit `(use (reg/i ...))` when `.09.greg` (`snap.pre_state`) has
    //    none, and always recovers `(use (reg/i ...))` whenever `.09.greg` has one and `lifted_y3`
    //    contains a binary-observable unread non-call write to the return register reaching exit
    //    (e.g., a non-annulled backward-branch delay-slot write to `$v0` on a fallthrough loop
    //    exit; pure tail-call-like returns `return Callee(...);` with bare `(call ...)` and returns
    //    whose `$v0` is also consumed by a trailing store `*out = v0; return v0;` are
    //    binary-indistinguishable from `void` functions).
    // 2. Never introduces spurious standalone note-only blocks after a single non-jump fallthrough
    //    predecessor (`NOTE_INSN_LOOP_BEG`).
    auto find_return_value_use_reg = [](const RtlControlFlowGraph& g) -> int {
      for (const RtlBasicBlock& blk : g.rpo_blocks) {
        for (const std::string& pat_s : blk.instructions) {
          auto pn = ParsePatternAndNotes(pat_s);
          if (pn.ok() && pn->pattern.op == RtlOp::kUse && pn->pattern.children.size() == 1 &&
              pn->pattern.children[0].IsReg() && pn->pattern.children[0].flags.integrated) {
            return pn->pattern.children[0].RegNo();
          }
        }
      }
      return -1;
    };
    auto has_unread_noncall_ret_write_to_exit = [](const RtlControlFlowGraph& g,
                                                   int ret_regno) -> bool {
      const size_t n_blocks = g.rpo_blocks.size();
      if (ret_regno < 0 || n_blocks == 0) {
        return false;
      }
      auto expr_mentions_reg = [ret_regno](const RtlExpr& e) -> bool {
        bool hit = false;
        e.Visit([&](const RtlExpr& n) {
          if (n.IsReg() && n.RegNo() == ret_regno) {
            hit = true;
          }
        });
        return hit;
      };
      auto step_insn = [&](const RtlExpr& pat, bool* is_call, bool* sets_ret,
                           bool* kills_or_reads) {
        *is_call = false;
        *sets_ret = false;
        *kills_or_reads = false;
        pat.Visit([&](const RtlExpr& n) {
          if (n.op == RtlOp::kCall) {
            *is_call = true;
          }
        });
        auto inspect_elem = [&](const RtlExpr& elem) {
          if (elem.op == RtlOp::kNote ||
              (elem.op == RtlOp::kUse && elem.children.size() == 1 && elem.children[0].IsReg() &&
               elem.children[0].flags.integrated)) {
            return;
          }
          if (elem.IsSet()) {
            const RtlExpr& dst = elem.SetDest();
            const RtlExpr& src = elem.SetSrc();
            const RtlExpr* dst_reg =
                dst.IsReg() ? &dst
                            : ((dst.IsSubreg() && !dst.children.empty() && dst.children[0].IsReg())
                                   ? &dst.children[0]
                                   : nullptr);
            if (dst_reg != nullptr && dst_reg->RegNo() == ret_regno) {
              *sets_ret = true;
              *kills_or_reads = true;
            } else if (expr_mentions_reg(dst)) {
              *kills_or_reads = true;
            }
            if (expr_mentions_reg(src)) {
              *kills_or_reads = true;
            }
            return;
          }
          if (expr_mentions_reg(elem)) {
            *kills_or_reads = true;
          }
        };
        if (pat.IsParallel()) {
          for (const RtlExpr& ch : pat.children) {
            inspect_elem(ch);
          }
        } else if (pat.op == RtlOp::kSequence) {
          for (const RtlExpr& wrapper : pat.children) {
            if (!wrapper.children.empty()) {
              inspect_elem(wrapper.children[0]);
            }
          }
        } else {
          inspect_elem(pat);
        }
        if (*is_call) {
          *kills_or_reads = true;
        }
      };

      std::vector<bool> in_unread(n_blocks, false);
      bool changed = true;
      while (changed) {
        changed = false;
        for (size_t rev = n_blocks; rev > 0; --rev) {
          const size_t b = rev - 1;
          const RtlBasicBlock& blk = g.rpo_blocks[b];
          bool cur = blk.successors.empty();
          for (size_t sb : blk.successors) {
            if (sb < n_blocks && in_unread[sb]) {
              cur = true;
            }
          }
          for (size_t i = blk.instructions.size(); i > 0; --i) {
            auto pn = ParsePatternAndNotes(blk.instructions[i - 1]);
            if (!pn.ok() || pn->pattern.op == RtlOp::kNote) {
              continue;
            }
            bool is_call = false;
            bool sets_ret = false;
            bool kills_or_reads = false;
            step_insn(pn->pattern, &is_call, &sets_ret, &kills_or_reads);
            if (kills_or_reads) {
              cur = false;
            }
          }
          if (cur != in_unread[b]) {
            in_unread[b] = cur;
            changed = true;
          }
        }
      }

      for (size_t b = 0; b < n_blocks; ++b) {
        const RtlBasicBlock& blk = g.rpo_blocks[b];
        bool cur = blk.successors.empty();
        for (size_t sb : blk.successors) {
          if (sb < n_blocks && in_unread[sb]) {
            cur = true;
          }
        }
        for (size_t i = blk.instructions.size(); i > 0; --i) {
          auto pn = ParsePatternAndNotes(blk.instructions[i - 1]);
          if (!pn.ok() || pn->pattern.op == RtlOp::kNote) {
            continue;
          }
          bool is_call = false;
          bool sets_ret = false;
          bool kills_or_reads = false;
          step_insn(pn->pattern, &is_call, &sets_ret, &kills_or_reads);
          if (sets_ret && !is_call && cur) {
            return true;
          }
          if (kills_or_reads) {
            cur = false;
          }
        }
      }
      return false;
    };
    const int lifted_ret_reg = find_return_value_use_reg(lifted_y3);
    const int expected_ret_reg = find_return_value_use_reg(snap.pre_state);
    if (lifted_ret_reg >= 0 && expected_ret_reg >= 0) {
      EXPECT_EQ(lifted_ret_reg, expected_ret_reg)
          << "Mismatched exit (use (reg/i ...)) register in fresh lifted_y3 for " << tc.test_name
          << ":\n"
          << lifted_y3.FormatSummary();
    } else if (lifted_ret_reg >= 0 && expected_ret_reg < 0) {
      // A `void` C function (e.g., hoisting a floating-point subtraction into `$f0` ahead of a
      // guarded call) is binary-identical to a value-returning function only when an unread
      // non-call write to `lifted_ret_reg` genuinely reaches exit.
      EXPECT_TRUE(has_unread_noncall_ret_write_to_exit(lifted_y3, lifted_ret_reg))
          << "Spurious exit (use (reg/i ...)) without unread return write in fresh lifted_y3 for "
          << tc.test_name << ":\n"
          << lifted_y3.FormatSummary();
    } else if (expected_ret_reg >= 0) {
      EXPECT_FALSE(has_unread_noncall_ret_write_to_exit(lifted_y3, expected_ret_reg))
          << "Missing observable exit (use (reg/i ...)) in fresh lifted_y3 for " << tc.test_name
          << ":\n"
          << lifted_y3.FormatSummary();
    }

    for (size_t b = 1; b < lifted_y3.rpo_blocks.size(); ++b) {
      const RtlBasicBlock& blk = lifted_y3.rpo_blocks[b];
      if (blk.has_label || blk.instructions.empty()) {
        continue;
      }
      bool all_notes = true;
      for (const std::string& pat_s : blk.instructions) {
        auto pn = ParsePatternAndNotes(pat_s);
        if (!pn.ok() || pn->pattern.op != RtlOp::kNote) {
          all_notes = false;
          break;
        }
      }
      if (!all_notes) {
        continue;
      }
      const RtlBasicBlock& prev = lifted_y3.rpo_blocks[b - 1];
      if (prev.successors.size() == 1 && prev.successors[0] == b && !prev.instructions.empty()) {
        auto prev_last = ParsePatternAndNotes(prev.instructions.back());
        const bool prev_ends_in_jump =
            prev_last.ok() &&
            ((prev_last->pattern.IsSet() && prev_last->pattern.SetDest().op == RtlOp::kPc) ||
             prev_last->pattern.op == RtlOp::kReturn);
        EXPECT_TRUE(prev_ends_in_jump) << "Spurious standalone note-only fallthrough block B" << b
                                       << " in fresh lifted_y3 on " << tc.test_name << ":\n"
                                       << lifted_y3.FormatSummary();
      }
    }
  }
  EXPECT_TRUE(found_snap) << "Missing kScheduleBlockSched2 snapshot for " << tc.test_name;

  // 2. Full fresh-context `VerifySuffixRightInverse` ($F_{0..3}(G_{3..0}(x_0)) = x_0$)
  //    reusing `lifted_y3` from step 1.
  const RightInverseResult suffix_res =
      verifier->VerifySuffixRightInverse(RtlPassKind::kGreg, trace.stage0_input, lifted_y3);
  EXPECT_TRUE(suffix_res.matches) << "Stage 0..3 suffix right-inverse mismatch for " << tc.test_name
                                  << ":\n"
                                  << suffix_res.diff_detail << "\nexpected:\n"
                                  << suffix_res.expected_summary << "\nactual:\n"
                                  << suffix_res.actual_summary;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, Pass03Sched2PostReloadRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
