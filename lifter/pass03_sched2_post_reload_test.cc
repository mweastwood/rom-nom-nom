#include "lifter/pass03_sched2_post_reload.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
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
    Stage0FunctionInput linked;
    linked.machine_words = ms_trace.stage0_input.machine_words;
    linked.base_vram = 0x80020000u;
    linked.gp_vram = 0x80060000u;
    linked.profile = ms_trace.stage0_input.profile;
    linked.profile.gp_symbol_sizes.clear();
    linked.jump_tables = ms_trace.stage0_input.jump_tables;
    linked.constant_pool = ms_trace.stage0_input.constant_pool;

    std::vector<std::pair<std::string, uint32_t>> sym_vrams;
    auto find_sym = [&](const std::string& s, uint32_t* out) -> bool {
      for (const auto& [k, val] : sym_vrams) {
        if (k == s) {
          *out = val;
          return true;
        }
      }
      return false;
    };

    uint32_t next_rodata = 0x80030000u;
    uint32_t next_func = 0x80040000u;
    uint32_t next_gp_data = 0x80059000u;
    uint32_t next_data = 0x80031000u;
    for (Stage0JumpTable& jt : linked.jump_tables) {
      jt.table_vram = next_rodata;
      sym_vrams.emplace_back(jt.label_name, jt.table_vram);
      next_rodata += static_cast<uint32_t>(jt.target_word_indices.size() * 4u + 16u);
    }
    for (Stage0ConstantPoolEntry& cp : linked.constant_pool) {
      if (cp.section_name == ".lit4" || cp.section_name == ".lit8" || cp.section_name == ".sdata") {
        cp.entry_vram = next_gp_data;
        next_gp_data += 16u;
      } else {
        cp.entry_vram = next_rodata;
        next_rodata += 16u;
      }
      sym_vrams.emplace_back(cp.label_name, cp.entry_vram);
    }
    for (const Stage0Relocation& rel : ms_trace.stage0_input.relocations) {
      uint32_t existing = 0;
      if (find_sym(rel.symbol_name, &existing)) {
        continue;
      }
      int64_t max_add = 0;
      bool any_gp = false;
      bool any_call = false;
      for (const Stage0Relocation& r2 : ms_trace.stage0_input.relocations) {
        if (r2.symbol_name == rel.symbol_name) {
          if (r2.addend > max_add) {
            max_add = r2.addend;
          }
          if (r2.kind == Stage0RelocKind::kMipsGpRel16 ||
              r2.kind == Stage0RelocKind::kMipsLiteral) {
            any_gp = true;
          }
          if (r2.kind == Stage0RelocKind::kMips26) {
            any_call = true;
          }
        }
      }
      const uint32_t span_bytes = static_cast<uint32_t>((max_add > 0 ? max_add : 0) + 16);
      uint32_t assigned = 0;
      if (any_call) {
        assigned = next_func;
        next_func += 64u;
      } else if (any_gp) {
        assigned = next_gp_data;
        next_gp_data += (span_bytes + 15u) & ~15u;
      } else {
        assigned = next_data;
        next_data += (span_bytes + 63u) & ~63u;
      }
      sym_vrams.emplace_back(rel.symbol_name, assigned);
      linked.vram_to_symbol[assigned] = rel.symbol_name;
      const auto gp_sz_it = ms_trace.stage0_input.profile.gp_symbol_sizes.find(rel.symbol_name);
      if (gp_sz_it != ms_trace.stage0_input.profile.gp_symbol_sizes.end() && gp_sz_it->second > 0) {
        linked.vram_to_symbol_size[assigned] = gp_sz_it->second;
      } else if (max_add > 0) {
        linked.vram_to_symbol_size[assigned] = span_bytes;
      }
    }
    std::vector<bool> relocated_word(linked.machine_words.size(), false);
    for (const Stage0Relocation& rel : ms_trace.stage0_input.relocations) {
      uint32_t sym_vram = 0;
      if (rel.word_index >= linked.machine_words.size() || !find_sym(rel.symbol_name, &sym_vram)) {
        continue;
      }
      relocated_word[rel.word_index] = true;
      const uint32_t full_addr = static_cast<uint32_t>(static_cast<int64_t>(sym_vram) + rel.addend);
      if (rel.kind == Stage0RelocKind::kMips26) {
        linked.machine_words[rel.word_index] =
            (linked.machine_words[rel.word_index] & 0xfc000000u) | ((full_addr >> 2) & 0x03ffffffu);
      } else if (rel.kind == Stage0RelocKind::kMipsHi16) {
        const uint32_t hi = ((full_addr + 0x8000u) >> 16) & 0xffffu;
        linked.machine_words[rel.word_index] =
            (linked.machine_words[rel.word_index] & 0xffff0000u) | hi;
      } else if (rel.kind == Stage0RelocKind::kMipsLo16) {
        linked.machine_words[rel.word_index] =
            (linked.machine_words[rel.word_index] & 0xffff0000u) | (full_addr & 0xffffu);
      } else if (rel.kind == Stage0RelocKind::kMipsGpRel16 ||
                 rel.kind == Stage0RelocKind::kMipsLiteral) {
        const int32_t delta = static_cast<int32_t>(full_addr - linked.gp_vram);
        linked.machine_words[rel.word_index] =
            (linked.machine_words[rel.word_index] & 0xffff0000u) |
            (static_cast<uint32_t>(delta) & 0xffffu);
      }
    }
    for (size_t idx = 0; idx < linked.machine_words.size();) {
      const uint32_t w = linked.machine_words[idx];
      if (!relocated_word[idx] && ((w >> 26) & 0x3fu) == 0x02u) {
        const uint32_t local_idx = w & 0x03ffffffu;
        const uint32_t target_vram = linked.base_vram + local_idx * 4u;
        linked.machine_words[idx] = 0x08000000u | ((target_vram >> 2) & 0x03ffffffu);
      }
      const uint32_t op = (w >> 26) & 0x3fu;
      const uint32_t funct = w & 0x3fu;
      const bool has_delay =
          (op == 0x01u || op == 0x02u || op == 0x03u || (op >= 0x04u && op <= 0x07u) ||
           (op >= 0x14u && op <= 0x17u) || (op == 0x00u && (funct == 0x08u || funct == 0x09u)) ||
           (op == 0x11u && ((w >> 21) & 0x1fu) == 0x08u));
      idx += has_delay ? 2u : 1u;
    }

    std::reverse(linked.jump_tables.begin(), linked.jump_tables.end());
    for (auto& jt : linked.jump_tables) {
      jt.label_name.clear();
    }
    for (auto& cp : linked.constant_pool) {
      cp.label_name.clear();
    }

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
  const auto trace = hooks->TraceForwardCompilation(tc);

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

    const RtlControlFlowGraph lifted_y0 = InvertFinalAndGasFunction(trace.stage0_input);
    const RtlControlFlowGraph lifted_y1 =
        InvertPass01DbrSchedule(lifted_y0, trace.stage0_input.profile);
    const RtlControlFlowGraph lifted_y2 =
        InvertPass02Jump2CrossJump(lifted_y1, trace.stage0_input.profile);
    const RtlControlFlowGraph lifted_y3 =
        InvertPass03Sched2PostReload(lifted_y2, trace.stage0_input.profile);
    ASSERT_FALSE(lifted_y3.rpo_blocks.empty())
        << "Empty fresh Pass00..Pass03 lifted CFG for " << tc.test_name;
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
  }
  EXPECT_TRUE(found_snap) << "Missing kScheduleBlockSched2 snapshot for " << tc.test_name;

  // 2. Full fresh-context `VerifySuffixRightInverse` ($F_{0..3}(G_{3..0}(x_0)) = x_0$)
  //    across all 4,172 corpus cases.
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << "Test case: " << tc.test_name;

  const RightInverseResult suffix_res =
      verifier->VerifySuffixRightInverse(RtlPassKind::kGreg, trace.stage0_input);
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
