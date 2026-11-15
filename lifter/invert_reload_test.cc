#include "lifter/invert_reload.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_alter_reg.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/pass03_sched2_post_reload.h"

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ReloadOnlyInverseProvider : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg) const override {
    absl::StatusOr<RtlControlFlowGraph> res = ::rom_nom_nom::lifter::InvertReload(reload_cfg);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg,
                                   const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res =
        ::rom_nom_nom::lifter::InvertReload(reload_cfg, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
};

class CorruptedReloadInverseProvider : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg) const override {
    RtlControlFlowGraph corrupted = reload_cfg;
    if (!corrupted.rpo_blocks.empty()) {
      corrupted.rpo_blocks[0].instructions.clear();
      corrupted.rpo_blocks[0].instructions.push_back("(set (reg:SI 2 $2) (const_int 999999))");
      corrupted.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    }
    return corrupted;
  }
};

CompilationProfile EffectiveStage0Profile(const Stage0FunctionInput& input_x) {
  CompilationProfile effective_profile = input_x.profile;
  if (input_x.base_vram != 0 && input_x.relocations.empty()) {
    for (const auto& [vram, sym] : input_x.vram_to_symbol) {
      if (sym.empty()) {
        continue;
      }
      const auto sz_it = input_x.vram_to_symbol_size.find(vram);
      if (sz_it != input_x.vram_to_symbol_size.end() && sz_it->second > 0) {
        effective_profile.gp_symbol_sizes.try_emplace(sym, sz_it->second);
      }
    }
  }
  return effective_profile;
}

bool HasRawSpMemAboveOutgoingArgs(const RtlControlFlowGraph& cfg) {
  const int64_t out_args = std::max(0, cfg.sideband.outgoing_args_size);
  bool found_bad = false;
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& insn_s : blk.instructions) {
      absl::StatusOr<RtlPatternAndNotes> pn = ParsePatternAndNotes(insn_s);
      if (!pn.ok()) {
        continue;
      }
      pn->pattern.Visit([&](const RtlExpr& node) {
        if (!node.IsMem() || node.children.empty()) {
          return;
        }
        const RtlExpr& addr = node.children[0];
        if (addr.op == RtlOp::kReg && !addr.IsArgPointerReg() && addr.RegNo() == 29 &&
            out_args == 0 &&
            (cfg.sideband.frame_size_bytes > 0 || !cfg.sideband.saved_gpr_regnos.empty())) {
          found_bad = true;
        } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
                   addr.children[0].op == RtlOp::kReg && !addr.children[0].IsArgPointerReg() &&
                   addr.children[0].RegNo() == 29 && addr.children[1].IsConstInt() &&
                   addr.children[1].int_val >= out_args) {
          found_bad = true;
        }
      });
    }
  }
  return found_bad;
}

std::string CompareStage0ReplayResult(const Stage0FunctionInput& expected,
                                      const Stage0FunctionInput& actual,
                                      const std::string& combined_asm) {
  if (actual.machine_words != expected.machine_words) {
    std::string word_diffs;
    const size_t min_len = std::min(actual.machine_words.size(), expected.machine_words.size());
    for (size_t i = 0; i < min_len; ++i) {
      if (actual.machine_words[i] != expected.machine_words[i]) {
        absl::StrAppendFormat(&word_diffs, "  word[%zu]: got 0x%08x != expected 0x%08x\n", i,
                              actual.machine_words[i], expected.machine_words[i]);
      }
    }
    return absl::StrFormat(
        "Machine words mismatch: got %zu words != expected %zu words\n%sasm:\n%s",
        actual.machine_words.size(), expected.machine_words.size(), word_diffs, combined_asm);
  }
  if (expected.base_vram != 0 && expected.relocations.empty()) {
    return "";
  }
  if (actual.jump_tables.size() != expected.jump_tables.size()) {
    return absl::StrFormat("Jump-table count got %zu != expected %zu\nasm:\n%s",
                           actual.jump_tables.size(), expected.jump_tables.size(), combined_asm);
  }
  for (size_t i = 0; i < expected.jump_tables.size(); ++i) {
    if (actual.jump_tables[i].target_word_indices != expected.jump_tables[i].target_word_indices) {
      return absl::StrFormat("Jump table %zu target_word_indices mismatch\nasm:\n%s", i,
                             combined_asm);
    }
  }
  if (actual.constant_pool.size() != expected.constant_pool.size()) {
    return absl::StrFormat("Constant-pool count got %zu != expected %zu\nasm:\n%s",
                           actual.constant_pool.size(), expected.constant_pool.size(),
                           combined_asm);
  }
  if (actual.relocations.size() != expected.relocations.size()) {
    return absl::StrFormat("Relocation count got %zu != expected %zu\nasm:\n%s",
                           actual.relocations.size(), expected.relocations.size(), combined_asm);
  }
  absl::flat_hash_map<std::string, size_t> expected_jt_by_label;
  for (size_t i = 0; i < expected.jump_tables.size(); ++i) {
    expected_jt_by_label[expected.jump_tables[i].label_name] = i;
  }
  absl::flat_hash_map<std::string, size_t> actual_jt_by_label;
  for (size_t i = 0; i < actual.jump_tables.size(); ++i) {
    actual_jt_by_label[actual.jump_tables[i].label_name] = i;
  }
  absl::flat_hash_map<std::string, const Stage0ConstantPoolEntry*> expected_cp_by_label;
  for (const auto& cp : expected.constant_pool) {
    expected_cp_by_label[cp.label_name] = &cp;
  }
  absl::flat_hash_map<std::string, const Stage0ConstantPoolEntry*> actual_cp_by_label;
  for (const auto& cp : actual.constant_pool) {
    actual_cp_by_label[cp.label_name] = &cp;
  }
  for (size_t i = 0; i < expected.relocations.size(); ++i) {
    const Stage0Relocation& exp_r = expected.relocations[i];
    const Stage0Relocation& act_r = actual.relocations[i];
    if (act_r.word_index != exp_r.word_index || act_r.kind != exp_r.kind ||
        act_r.addend != exp_r.addend) {
      return absl::StrFormat(
          "Relocation %zu mismatch: actual(word=%zu, kind=%d, sym=%s, add=%d) != "
          "expected(word=%zu, kind=%d, sym=%s, add=%d)\nasm:\n%s",
          i, act_r.word_index, static_cast<int>(act_r.kind), act_r.symbol_name,
          static_cast<int>(act_r.addend), exp_r.word_index, static_cast<int>(exp_r.kind),
          exp_r.symbol_name, static_cast<int>(exp_r.addend), combined_asm);
    }
    if (auto exp_jt = expected_jt_by_label.find(exp_r.symbol_name);
        exp_jt != expected_jt_by_label.end()) {
      auto act_jt = actual_jt_by_label.find(act_r.symbol_name);
      if (act_jt == actual_jt_by_label.end() ||
          actual.jump_tables[act_jt->second].target_word_indices !=
              expected.jump_tables[exp_jt->second].target_word_indices) {
        return absl::StrFormat("Relocation %zu jump-table target mismatch\nasm:\n%s", i,
                               combined_asm);
      }
    } else if (auto exp_cp = expected_cp_by_label.find(exp_r.symbol_name);
               exp_cp != expected_cp_by_label.end()) {
      auto act_cp = actual_cp_by_label.find(act_r.symbol_name);
      if (act_cp == actual_cp_by_label.end() ||
          act_cp->second->byte_size != exp_cp->second->byte_size ||
          act_cp->second->words != exp_cp->second->words) {
        return absl::StrFormat("Relocation %zu constant-pool mismatch\nasm:\n%s", i, combined_asm);
      }
    } else if (act_r.symbol_name != exp_r.symbol_name) {
      return absl::StrFormat("Relocation %zu symbol mismatch: got '%s' != expected '%s'\nasm:\n%s",
                             i, act_r.symbol_name, exp_r.symbol_name, combined_asm);
    }
  }
  return "";
}

class InvertReloadTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ctx = new GccContext();
    hooks = new FunctionHooks(ctx);
  }

  static void TearDownTestSuite() {
    delete hooks;
    hooks = nullptr;
    delete ctx;
    ctx = nullptr;
  }

  static GccContext* ctx;
  static FunctionHooks* hooks;
};

GccContext* InvertReloadTest::ctx = nullptr;
FunctionHooks* InvertReloadTest::hooks = nullptr;

// Positive synthetic test 1: Frame-pointer elimination reversal (`$sp` -> `$fp`).
TEST_F(InvertReloadTest, ReversesFramePointerEliminationInStackAccesses) {
  RtlControlFlowGraph reload_cfg;
  reload_cfg.pass = RtlPassKind::kGreg;
  reload_cfg.sideband.frame_pointer_needed = false;
  reload_cfg.sideband.frame_size_bytes = 24;
  reload_cfg.sideband.max_regno = 74;
  reload_cfg.sideband.reg_renumber[72] = 4;
  reload_cfg.sideband.reg_renumber[73] = 3;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))",
      "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
      "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  reload_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_cfg);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  ASSERT_EQ(lifted->rpo_blocks.size(), 1u);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[1],
            "(set (mem:SI (plus:SI (reg:SI 30 fp) (const_int 16))) (reg:SI 72)) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[2],
            "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 30 fp) (const_int 16))))");

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyReloadRightInverse(reload_cfg);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 2: Preserves `$fp` when `frame_pointer_needed` is true.
TEST_F(InvertReloadTest, PreservesFramePointerWhenFramePointerNeeded) {
  RtlControlFlowGraph reload_cfg;
  reload_cfg.pass = RtlPassKind::kGreg;
  reload_cfg.sideband.frame_pointer_needed = true;
  reload_cfg.sideband.frame_size_bytes = 16;
  reload_cfg.sideband.max_regno = 74;
  reload_cfg.sideband.reg_renumber[72] = 4;
  reload_cfg.sideband.reg_renumber[73] = 3;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (reg:SI 72)))",
      "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 8))) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))",
      "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 8))))",
      "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  reload_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_cfg);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[2],
            "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 8))) (reg:SI 72)) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyReloadRightInverse(reload_cfg);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 3: Exact 3-Region MIPS Stack Address Un-Elimination
// (`[0, outgoing_args_size)` -> `$sp`, `[outgoing_args_size, post_reload_tsize)` -> `$fp`,
// `[post_reload_tsize, ...)` -> `(reg:SI 0 argp)`) for BOTH `!frame_pointer_needed`
// and `frame_pointer_needed`, plus large-frame `>= 32768` stack-offset address reloads.
TEST_F(InvertReloadTest, UneliminatesThreeStackRegionsAndLargeFrameOffsets) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // Case A: `!frame_pointer_needed`:
  // - `outgoing_args_size = 24` -> `[0, 24)` stays `$sp`
  // - `frame_size_bytes = 16` -> local `[24, 40)` + saved `ra` (`8` bytes at `[40, 48)`) =>
  //   `post_reload_tsize = 48` -> `[24, 48)` rewrites to `$fp`
  // - `[48, ...)` incoming caller stack args rewrite to `(reg:SI 0 argp)` (`+0` and `+16`)
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_pointer_needed = false;
    cfg.sideband.outgoing_args_size = 24;
    cfg.sideband.frame_size_bytes = 16;
    cfg.sideband.saved_gpr_regnos = {31};
    cfg.sideband.max_regno = 76;
    cfg.sideband.reg_renumber[72] = 8;
    cfg.sideband.reg_renumber[73] = 9;
    cfg.sideband.reg_renumber[74] = 10;

    RtlBasicBlock bb0;
    bb0.id = 0;
    // Incoming caller stack args at `post_reload_tsize + 0` (48) and `+ 16` (64).
    bb0.AddPatternOnly("(set (reg:SI 72) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 48))))");
    bb0.AddPatternOnly("(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 64))))");
    // Local frame variable at `starting_frame_offset` (24).
    bb0.AddPatternOnly(
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 72)) "
        "(expr_list:REG_DEAD (reg:SI 72) (nil))");
    bb0.AddPatternOnly("(set (reg:SI 74) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))");
    // Outgoing 5th call argument at `sp + 16` (`< outgoing_args_size = 24`).
    bb0.AddPatternOnly(
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 73)) "
        "(expr_list:REG_DEAD (reg:SI 73) (nil))");
    RtlInsn call_insn;
    call_insn.canonical_pattern =
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Callee6\"))) "
        "(const_int 24))) (clobber (reg:SI 31 ra)) ] )";
    call_insn.has_explicit_call_fusage = true;
    bb0.AddInsn(std::move(call_insn));
    bb0.AddPatternOnly(
        "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 74))) "
        "(expr_list:REG_DEAD (reg:SI 74) (nil))");
    bb0.AddPatternOnly("(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
    cfg.rpo_blocks.push_back(std::move(bb0));

    absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(cfg, prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    EXPECT_EQ(lifted->rpo_blocks[0].instructions[0], "(set (reg:SI 72) (mem:SI (reg:SI 0 argp)))");
    EXPECT_EQ(lifted->rpo_blocks[0].instructions[1],
              "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 0 argp) (const_int 16))))");
    EXPECT_EQ(lifted->rpo_blocks[0].instructions[2],
              "(set (mem:SI (plus:SI (reg:SI 30 fp) (const_int 24))) (reg:SI 72)) "
              "(expr_list:REG_DEAD (reg:SI 72) (nil))");
    EXPECT_EQ(lifted->rpo_blocks[0].instructions[4],
              "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 73)) "
              "(expr_list:REG_DEAD (reg:SI 73) (nil))");

    // Note: pseudo `74` crosses `Callee6` in callee-saved `$s0` (`16`) so `reload` won't spill it.
    cfg.sideband.saved_gpr_regnos = {16, 31};
    cfg.sideband.reg_renumber[74] = 16;
    // With `$s0` + `$ra` (`8` bytes, rounded to `8`), `post_reload_tsize` is still `24 + 16 + 8 =
    // 48`!
    cfg.sideband.basic_block_live_at_start[0] = {29};
    cfg.sideband.reg_n_deaths[72] = 1;
    cfg.sideband.reg_n_deaths[73] = 1;
    cfg.sideband.reg_n_deaths[74] = 1;
    cfg.sideband.reg_n_calls_crossed[74] = 1;
    ReloadOnlyInverseProvider provider;
    RightInverseVerifier verifier(ctx, hooks, &provider);
    RightInverseResult vres = verifier.VerifyReloadRightInverse(cfg);
    EXPECT_TRUE(vres.matches) << vres.diff_detail;
  }

  // Case B: `frame_pointer_needed = true` (`alloca`): `$fp`-based incoming args
  // at `>= post_reload_tsize` un-eliminate to `(reg:SI 0 argp)`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_pointer_needed = true;
    cfg.sideband.outgoing_args_size = 16;
    cfg.sideband.frame_size_bytes = 8;
    cfg.sideband.saved_gpr_regnos = {30};
    cfg.sideband.max_regno = 75;
    cfg.sideband.reg_renumber[72] = 8;
    cfg.sideband.reg_renumber[73] = 9;

    // `var_size = 8`, `reg_save = 8` (`$fp`), `args_size = 16` => `tsize = 32`.
    cfg.sideband.basic_block_live_at_start[0] = {29, 30};
    cfg.sideband.reg_n_deaths[72] = 1;
    cfg.sideband.reg_n_deaths[73] = 1;
    RtlBasicBlock bb0;
    bb0.id = 0;
    bb0.instructions = {
        "(set (reg:SI 72) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 48))))",
        "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 16))) (reg:SI 72)) "
        "(expr_list:REG_DEAD (reg:SI 72) (nil))",
        "(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 16))))",
        "(set (reg/i:SI 2 v0) (reg:SI 73)) (expr_list:REG_DEAD (reg:SI 73) (nil))",
        "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
    };
    bb0.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(bb0);

    absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(cfg, prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    EXPECT_EQ(lifted->rpo_blocks[0].instructions[0],
              "(set (reg:SI 72) (mem:SI (plus:SI (reg:SI 0 argp) (const_int 16))))");
    EXPECT_EQ(lifted->rpo_blocks[0].instructions[1],
              "(set (mem:SI (plus:SI (reg:SI 30 $fp) (const_int 16))) (reg:SI 72)) "
              "(expr_list:REG_DEAD (reg:SI 72) (nil))");

    hooks->ApplyCompilationProfile(prof);
    ReloadOnlyInverseProvider provider;
    RightInverseVerifier verifier(ctx, hooks, &provider);
    RightInverseResult vres = verifier.VerifyReloadRightInverse(cfg);
    EXPECT_TRUE(vres.matches) << vres.diff_detail;
  }

  // Case C: Large stack frame (`>= 32768`) `gen_reload` address reload
  // (`(set r_const (const_int 70000))` + `(set r_addr (plus r_const $sp))`)
  // folds back into `(mem:SI (plus:SI (reg:SI 30 fp) (const_int 70000)))`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kGreg;
    cfg.sideband.frame_pointer_needed = false;
    cfg.sideband.outgoing_args_size = 0;
    cfg.sideband.frame_size_bytes = 70032;
    cfg.sideband.max_regno = 76;
    cfg.sideband.reg_renumber[72] = 4;
    cfg.sideband.reg_renumber[73] = 2;
    cfg.sideband.reg_renumber[74] = 8;  // `InvertAlterReg` scratch pseudo for `const_int 70000`
    cfg.sideband.reg_renumber[75] = 8;  // `InvertAlterReg` scratch pseudo for `r_const + $sp`

    RtlBasicBlock bb0;
    bb0.id = 0;
    bb0.instructions = {
        "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
        "(set (reg:SI 74) (const_int 70000))",
        "(set (reg:SI 75) (plus:SI (reg:SI 74) (reg:SI 29 sp))) "
        "(expr_list:REG_DEAD (reg:SI 74) (nil))",
        "(set (mem:SI (reg:SI 75)) (reg:SI 72)) "
        "(expr_list:REG_DEAD (reg:SI 75) (expr_list:REG_DEAD (reg:SI 72) (nil)))",
        "(set (reg/i:SI 2 v0) (const_int 0))",
        "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
    };
    bb0.SyncRtlInsnsFromInstructions();
    cfg.rpo_blocks.push_back(bb0);

    absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(cfg, prof);
    ASSERT_TRUE(lifted.ok()) << lifted.status();
    ASSERT_EQ(lifted->rpo_blocks[0].instructions.size(), 4u);
    EXPECT_EQ(lifted->rpo_blocks[0].instructions[1],
              "(set (mem:SI (plus:SI (reg:SI 30 fp) (const_int 70000))) (reg:SI 72)) "
              "(expr_list:REG_DEAD (reg:SI 72) (nil))");
    EXPECT_FALSE(lifted->sideband.reg_renumber.contains(74));
    EXPECT_FALSE(lifted->sideband.reg_renumber.contains(75));

    // Also add a volatile read at `[fp + 70024]` so `[0, 70032)` has no phantom
    // unreferenced tail slots, materialize the real `kReload` snapshot via GCC,
    // and verify `VerifySubfunctionBoundaryRightInverse` + `VerifyReloadRightInverse`.
    RtlControlFlowGraph large_pre = *lifted;
    large_pre.rpo_blocks[0].instructions.insert(
        large_pre.rpo_blocks[0].instructions.begin() + 2,
        "(set (mem/v:SI (plus:SI (reg:SI 30 fp) (const_int 70024))) (const_int 0))");
    large_pre.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    hooks->ApplyCompilationProfile(prof);
    const SubfunctionBoundarySnapshot large_snap = hooks->ExecuteSubfunctionBoundary(
        SubfunctionId::kReload, large_pre, {}, prof.optimize_level, prof.c_flags);
    ReloadOnlyInverseProvider provider;
    RightInverseVerifier verifier(ctx, hooks, &provider);
    EXPECT_TRUE(verifier.VerifySubfunctionBoundaryRightInverse(large_snap).matches)
        << verifier.VerifySubfunctionBoundaryRightInverse(large_snap).diff_detail;
    EXPECT_TRUE(verifier.VerifyReloadRightInverse(large_snap.post_state).matches)
        << verifier.VerifyReloadRightInverse(large_snap.post_state).diff_detail;
  }
}

// Positive synthetic test 4: Zero-Oracle `caller-save` Unification on
// `InvertAlterReg` Output (split `P_pre` and `P_post` across `CALL_INSN`
// with empty incoming `reg_n_calls_crossed`).
TEST_F(InvertReloadTest, ZeroOracleCallerSaveUnifiesSplitPseudosAcrossCall) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // Build pre-reload CFG where pseudo `72` (`$t0 = 8`) crosses `Helper()` (`frame_size_bytes = 0`),
  // run real `kReload -> kAlterReg` to materialize `save_call_clobbered_regs`, then lift via
  // `InvertAlterReg` (which splits `72` into two pseudos with empty `reg_n_calls_crossed`)!
  RtlControlFlowGraph pre_reload;
  pre_reload.pass = RtlPassKind::kLreg;
  pre_reload.sideband.frame_pointer_needed = false;
  pre_reload.sideband.outgoing_args_size = 16;
  pre_reload.sideband.frame_size_bytes = 0;
  pre_reload.sideband.saved_gpr_regnos = {31};
  pre_reload.sideband.max_regno = 73;
  pre_reload.sideband.reg_renumber[72] = 8;
  pre_reload.sideband.reg_n_refs[72] = 2;
  pre_reload.sideband.reg_n_sets[72] = 1;
  pre_reload.sideband.reg_n_deaths[72] = 1;
  pre_reload.sideband.reg_n_calls_crossed[72] = 1;
  pre_reload.sideband.basic_block_live_at_start[0] = {4, 29};

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 5))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Helper\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  bb0.AddInsn(std::move(call_insn));
  bb0.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 72))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  bb0.AddPatternOnly("(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  pre_reload.rpo_blocks.push_back(std::move(bb0));

  hooks->ApplyCompilationProfile(prof);
  const SubfunctionBoundarySnapshot reload_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, pre_reload, {}, prof.optimize_level, prof.c_flags);
  const SubfunctionBoundarySnapshot expected_greg = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kAlterReg, reload_snap.post_state, {}, prof.optimize_level, prof.c_flags);
  ASSERT_EQ(expected_greg.post_state.rpo_blocks[0].instructions.size(), 6u);
  ASSERT_EQ(expected_greg.post_state.sideband.frame_size_bytes, 4);

  const absl::StatusOr<RtlControlFlowGraph> post_alter_lifted =
      InvertAlterReg(expected_greg.post_state, prof);
  ASSERT_TRUE(post_alter_lifted.ok()) << post_alter_lifted.status();
  ASSERT_TRUE(post_alter_lifted->sideband.reg_n_calls_crossed.empty());
  ASSERT_EQ(post_alter_lifted->sideband.reg_renumber.size(), 2u);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(*post_alter_lifted, prof);
  ASSERT_TRUE(lifted.ok()) << lifted.status();

  // Save & restore stripped (`6 -> 4` insns), split pseudos unified, `frame_size_bytes` shrunk `8
  // -> 0`, and `reg_n_calls_crossed` reconstructed to `1`!
  ASSERT_EQ(lifted->rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(lifted->sideband.frame_size_bytes, 0);
  ASSERT_EQ(lifted->sideband.reg_renumber.size(), 1u);
  const int unified_p = lifted->sideband.reg_renumber.begin()->first;
  ASSERT_TRUE(lifted->sideband.reg_n_calls_crossed.contains(unified_p));
  EXPECT_EQ(lifted->sideband.reg_n_calls_crossed.at(unified_p), 1);

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  EXPECT_TRUE(verifier.VerifySubfunctionBoundaryRightInverse(reload_snap).matches)
      << verifier.VerifySubfunctionBoundaryRightInverse(reload_snap).diff_detail;
  EXPECT_TRUE(verifier.VerifyReloadRightInverse(reload_snap.post_state).matches)
      << verifier.VerifyReloadRightInverse(reload_snap.post_state).diff_detail;

  // Also verify `VerifyRtlPassRightInverse` across the composed `InvertAlterReg -> InvertReload`
  // pipeline on `expected_greg.post_state`!
  const RightInverseResult alter_reload_res = verifier.VerifyRtlPassRightInverse(
      expected_greg.post_state,
      [&](const RtlControlFlowGraph& x) {
        const absl::StatusOr<RtlControlFlowGraph> pa = InvertAlterReg(x, prof);
        if (!pa.ok()) {
          return RtlControlFlowGraph{};
        }
        const absl::StatusOr<RtlControlFlowGraph> pr = InvertReload(*pa, prof);
        return pr.ok() ? *pr : RtlControlFlowGraph{};
      },
      [&](GccContext* gctx, FunctionHooks* ghooks) {
        ghooks->RunReload(gctx->GetInsns(), /*global=*/1);
        for (int regno = 68; regno < gctx->MaxRegNum(); ++regno) {
          ghooks->RunAlterReg(regno, /*from_reg=*/-1);
        }
      },
      RtlPassKind::kGreg);
  EXPECT_TRUE(alter_reload_res.matches) << alter_reload_res.diff_detail;
}

// Positive synthetic test 5: Zero-Oracle Spill Recovery & Callee-Saved Scratch
// Cleanup on `InvertAlterReg`-Style Input (preserving adjacent 4-byte C stack
// local, folding RMW + inherited reloads, and pruning reload-only `$s0`).
TEST_F(InvertReloadTest, ZeroOracleSpillRecoveryPreservesLocalAndPrunesReloadCalleeSavedReg) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // Frame layout (`starting_frame_offset = 16`):
  // - `[16, 24)`: genuine 4-byte C stack local at `[sp + 16]` (address taken & passed to call)
  // - `[24, 32)`: 8-byte big-endian `alter_reg` spill slot (`SImode` accessed at `24 + 4 = 28`)
  //   reloaded using reload-only callee-saved `$s0` (`16`)!
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.frame_pointer_needed = false;
  cfg.sideband.outgoing_args_size = 16;
  cfg.sideband.frame_size_bytes = 16;        // `[16, 24)` C local + `[24, 32)` spilled pseudo
  cfg.sideband.saved_gpr_regnos = {16, 31};  // `$s0` (16) used ONLY as reload scratch!
  cfg.sideband.max_regno = 78;
  cfg.sideband.reg_renumber[72] = 4;   // `$a0`
  cfg.sideband.reg_renumber[73] = 16;  // `$s0` scratch pseudo #1 (output + inherited reload)
  cfg.sideband.reg_renumber[74] = 16;  // `$s0` scratch pseudo #2a (RMW input reload)
  cfg.sideband.reg_renumber[75] = 4;   // `$a0` temp for C stack local `[16]`
  cfg.sideband.reg_renumber[76] = 16;  // `$s0` scratch pseudo #3 (input reload after call)
  cfg.sideband.reg_renumber[77] = 16;  // `$s0` scratch pseudo #2b (RMW output reload)

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.AddPatternOnly("(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  // Store to C stack local at `[sp + 16]`.
  bb0.AddPatternOnly("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 72))");
  // Output reload + reload inheritance on spill slot `[sp + 28]` via `73` (`$s0`):
  bb0.AddPatternOnly(
      "(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 9))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  bb0.AddPatternOnly("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 73))");
  // Inherited use of `73` before `73` dies:
  bb0.AddPatternOnly(
      "(set (reg:SI 75) (plus:SI (reg:SI 73) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  bb0.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 75)) "
      "(expr_list:REG_DEAD (reg:SI 75) (nil))");
  // RMW reload on spill slot `[sp + 28]` via `74` & `77` (`$s0`):
  bb0.AddPatternOnly("(set (reg:SI 74) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");
  bb0.AddPatternOnly(
      "(set (reg:SI 77) (plus:SI (reg:SI 74) (const_int 3))) "
      "(expr_list:REG_DEAD (reg:SI 74) (nil))");
  bb0.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 77)) "
      "(expr_list:REG_DEAD (reg:SI 77) (nil))");
  // Pass address of C stack local `[sp + 16]` to `TouchLocal(&local)`:
  bb0.AddPatternOnly("(set (reg:SI 4 a0) (plus:SI (reg:SI 29 sp) (const_int 16)))");
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"TouchLocal\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  bb0.AddInsn(std::move(call_insn));
  // Input reload of spill slot `[sp + 28]` after call via `76` (`$s0`):
  bb0.AddPatternOnly("(set (reg:SI 76) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");
  bb0.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 76))) "
      "(expr_list:REG_DEAD (reg:SI 76) (nil))");
  bb0.AddPatternOnly("(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  cfg.rpo_blocks.push_back(std::move(bb0));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(cfg, prof);
  ASSERT_TRUE(lifted.ok()) << lifted.status();

  // 1. C stack local `[fp + 16]` remains a `MEM`, while `[sp + 28]` is reconstructed as a
  //    fresh unallocated pseudo (`>= 78`, absent from `reg_renumber`).
  EXPECT_EQ(lifted->sideband.frame_size_bytes, 8);
  int unallocated_spill_pseudo = -1;
  for (const auto& [p, nrefs] : lifted->sideband.reg_n_refs) {
    if (p >= 68 && nrefs > 0 && !lifted->sideband.reg_renumber.contains(p)) {
      unallocated_spill_pseudo = p;
    }
  }
  EXPECT_GE(unallocated_spill_pseudo, 78);

  // 2. Scratch pseudos `73`, `74`, `76`, `77` (`$s0`) were folded away, and reload-only `$s0`
  // (`16`)
  //    was pruned from `saved_gpr_regnos`!
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(73));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(74));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(76));
  EXPECT_FALSE(lifted->sideband.reg_renumber.contains(77));
  EXPECT_EQ(lifted->sideband.saved_gpr_regnos, std::vector<int>({31}));

  // 3. C stack local `[fp + 16]` is still accessed via `(mem:SI (plus:SI (reg:SI 30 fp) (const_int
  // 16)))`,
  //    and spill offset `28` no longer appears in any `MEM` address.
  bool saw_local_fp_mem = false;
  for (const std::string& insn_s : lifted->rpo_blocks[0].instructions) {
    absl::StatusOr<RtlPatternAndNotes> pn = ParsePatternAndNotes(insn_s);
    ASSERT_TRUE(pn.ok()) << pn.status();
    pn->pattern.Visit([&](const RtlExpr& n) {
      if (n.IsMem() && !n.children.empty() && n.children[0].op == RtlOp::kPlus &&
          n.children[0].children.size() == 2 && n.children[0].children[0].op == RtlOp::kReg &&
          n.children[0].children[1].IsConstInt()) {
        if (n.children[0].children[0].RegNo() == 30 && n.children[0].children[1].int_val == 16) {
          saw_local_fp_mem = true;
        }
        EXPECT_NE(n.children[0].children[1].int_val, 28) << insn_s;
      }
    });
  }
  EXPECT_TRUE(saw_local_fp_mem);

  // 4. Materialize the `kReload` snapshot from `*lifted` and verify right-inverse replay via
  //    `RightInverseVerifier`.
  hooks->ApplyCompilationProfile(prof);
  const SubfunctionBoundarySnapshot spill_reload_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, *lifted, {}, prof.optimize_level, prof.c_flags);
  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  const RightInverseResult sub_res =
      verifier.VerifySubfunctionBoundaryRightInverse(spill_reload_snap);
  EXPECT_TRUE(sub_res.matches) << sub_res.diff_detail;
  const RightInverseResult stage_res =
      verifier.VerifyReloadRightInverse(spill_reload_snap.post_state);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

// Adversarial Negative Test A: A stack local written before a call AND also read
// outside the `caller-save` restore pattern (or aliased) MUST NOT be stripped as
// `caller-save`!
TEST_F(InvertReloadTest, AdversarialDoesNotStripAliasedStackLocalAsCallerSave) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.frame_pointer_needed = false;
  cfg.sideband.outgoing_args_size = 16;
  cfg.sideband.frame_size_bytes = 8;
  cfg.sideband.saved_gpr_regnos = {31};
  cfg.sideband.max_regno = 75;
  cfg.sideband.reg_renumber[72] = 8;  // `$t0`
  cfg.sideband.reg_renumber[73] = 8;  // `$t0`
  cfg.sideband.reg_renumber[74] = 9;  // `$t1` extra reader of `[sp + 16]`!

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.AddPatternOnly("(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  bb0.AddPatternOnly(
      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 72)) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  // Pass `&local` (`sp + 16`) to callee -> aliases `[sp + 16]`!
  bb0.AddPatternOnly("(set (reg:SI 4 a0) (plus:SI (reg:SI 29 sp) (const_int 16)))");
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"MutateSlot\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  bb0.AddInsn(std::move(call_insn));
  bb0.AddPatternOnly("(set (reg:SI 73) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))");
  bb0.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  bb0.AddPatternOnly("(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  cfg.rpo_blocks.push_back(std::move(bb0));

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(cfg, prof);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  // Store & load to `[fp + 16]` MUST remain intact (`7` insns, `frame_size_bytes == 8`)!
  EXPECT_EQ(lifted->rpo_blocks[0].instructions.size(), 7u);
  EXPECT_EQ(lifted->sideband.frame_size_bytes, 8);
  EXPECT_TRUE(lifted->sideband.reg_renumber.contains(72));
  EXPECT_TRUE(lifted->sideband.reg_renumber.contains(73));
}

// Adversarial Negative Test B: `mem/v` (volatile) and `mem/s` (struct) stack
// slots in `[starting_frame_offset, frame_top)` MUST NOT be lifted to spilled pseudos!
TEST_F(InvertReloadTest, AdversarialDoesNotLiftVolatileOrStructStackLocalsAsSpilledPseudos) {
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kGreg;
  cfg.sideband.frame_pointer_needed = false;
  cfg.sideband.outgoing_args_size = 0;
  cfg.sideband.frame_size_bytes = 16;  // `[0, 8)` volatile local, `[8, 16)` struct local
  cfg.sideband.max_regno = 75;
  cfg.sideband.reg_renumber[72] = 8;
  cfg.sideband.reg_renumber[73] = 9;
  cfg.sideband.reg_renumber[74] = 2;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
      "(set (mem/v:SI (reg:SI 29 sp)) (reg:SI 72)) (expr_list:REG_DEAD (reg:SI 72) (nil))",
      "(set (mem/s:SI (plus:SI (reg:SI 29 sp) (const_int 8))) (reg:SI 5 a1)) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))",
      "(set (reg:SI 73) (mem/v:SI (reg:SI 29 sp)))",
      "(set (reg:SI 74) (mem/s:SI (plus:SI (reg:SI 29 sp) (const_int 8))))",
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 73) (reg:SI 74))) "
      "(expr_list:REG_DEAD (reg:SI 73) (expr_list:REG_DEAD (reg:SI 74) (nil)))",
      "(use (reg/i:SI 2 v0))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(cfg, prof);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.frame_size_bytes, 16);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions.size(), 7u);
  for (const auto& [p, nrefs] : lifted->sideband.reg_n_refs) {
    if (p >= 68 && nrefs > 0) {
      EXPECT_TRUE(lifted->sideband.reg_renumber.contains(p))
          << "Unexpectedly synthesized unallocated spill pseudo " << p;
    }
  }
}

// Positive synthetic test 6: Reverses earlyclobber clobber output reload after PARALLEL.
TEST_F(InvertReloadTest, ReversesEarlyclobberClobberOutputReloadsOnParallel) {
  RtlControlFlowGraph reload_cfg;
  reload_cfg.pass = RtlPassKind::kGreg;
  reload_cfg.sideband.frame_pointer_needed = false;
  reload_cfg.sideband.max_regno = 68;
  reload_cfg.sideband.saved_gpr_regnos = {2, 3, 4, 5, 6, 7};
  reload_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29};
  reload_cfg.sideband.reg_n_deaths[4] = 1;
  reload_cfg.sideband.reg_n_deaths[5] = 1;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(parallel[ (set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
      "(clobber (reg:SI 2 v0)) (clobber (reg:SI 3 v1)) "
      "(clobber (reg:SI 6 a2)) (clobber (reg:SI 7 a3)) "
      "(use (const_int 8)) (use (const_int 1)) (use (const_int 0)) ] ) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_UNUSED (scratch:SI) "
      "(expr_list:REG_UNUSED (scratch:SI) (expr_list:REG_UNUSED (scratch:SI) "
      "(expr_list:REG_UNUSED (scratch:SI) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))))))",
      "(set (reg:SI 5 a1) (reg:SI 7 a3))",
      "(set (reg:SI 4 a0) (reg:SI 6 a2))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  reload_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_cfg);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  ASSERT_EQ(lifted->rpo_blocks[0].instructions.size(), 1u);
  EXPECT_EQ(lifted->rpo_blocks[0].instructions[0],
            "(parallel [(set (mem:BLK (reg:SI 4 a0)) (mem:BLK (reg:SI 5 a1))) "
            "(clobber (reg:SI 2 v0)) (clobber (reg:SI 3 v1)) "
            "(clobber (reg:SI 4 a0)) (clobber (reg:SI 5 a1)) "
            "(use (const_int 8)) (use (const_int 1)) (use (const_int 0))]) "
            "(expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_UNUSED (scratch:SI) "
            "(expr_list:REG_UNUSED (scratch:SI) (expr_list:REG_UNUSED (scratch:SI) "
            "(expr_list:REG_UNUSED (scratch:SI) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))))))");

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  RightInverseResult vres = verifier.VerifyReloadRightInverse(reload_cfg);
  EXPECT_TRUE(vres.matches) << vres.diff_detail;
}

// Positive synthetic test 7: Reverses frame-offset address calculation and REG_NOTES.
TEST_F(InvertReloadTest, ReversesFramePointerInAddressArithmeticAndNotes) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int take_ptr(int* p);\nint foo(int x) { int a = x + 1; return take_ptr(&a); }\n", "foo",
      {"-O2"});
  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  bool found_reload = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      found_reload = true;
      absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(snap.post_state);
      ASSERT_TRUE(lifted.ok()) << lifted.status();
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_reload);
}

// Positive synthetic test 8: Multi-register pressure / stack spill & reload sequence.
TEST_F(InvertReloadTest, ReversesSpillAndReloadUnderHighRegisterPressure) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int bar(int a, int b, int c, int d) {\n"
      "  volatile int s0 = a + b;\n"
      "  volatile int s1 = c + d;\n"
      "  return s0 * s1 + (a ^ c) + (b ^ d);\n"
      "}\n",
      "bar", {"-O2"});
  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  bool found_reload = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      found_reload = true;
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_reload);
}

// Negative test 1: Rejects empty CFG.
TEST_F(InvertReloadTest, RejectsEmptyCfg) {
  RtlControlFlowGraph empty_cfg;
  absl::StatusOr<RtlControlFlowGraph> res = InvertReload(empty_cfg);
  EXPECT_FALSE(res.ok());
}

// Negative test 2: Rejects malformed S-expression in instruction.
TEST_F(InvertReloadTest, RejectsMalformedSexprInstruction) {
  RtlControlFlowGraph bad_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 2 $2) (plus:SI (reg:SI 4 $4)"};
  bad_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> res = InvertReload(bad_cfg);
  EXPECT_FALSE(res.ok());
}

// Negative test 3: Rejects out-of-bounds CFG successor edge.
TEST_F(InvertReloadTest, RejectsOutOfBoundsSuccessorEdge) {
  RtlControlFlowGraph bad_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 2 $2) (const_int 0))"};
  bb0.successors = {5};
  bad_cfg.rpo_blocks.push_back(bb0);

  absl::StatusOr<RtlControlFlowGraph> res = InvertReload(bad_cfg);
  EXPECT_FALSE(res.ok());
}

// Negative test 4: Rejects invalid negative register number in instruction or sideband.
TEST_F(InvertReloadTest, RejectsInvalidNegativeRegisterNumber) {
  RtlControlFlowGraph bad_cfg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI -5) (const_int 0))"};
  bad_cfg.rpo_blocks.push_back(bb0);

  EXPECT_FALSE(InvertReload(bad_cfg).ok());

  RtlControlFlowGraph bad_sideband_cfg;
  RtlBasicBlock valid_bb;
  valid_bb.id = 0;
  valid_bb.instructions = {"(set (reg:SI 2 $2) (const_int 0))"};
  bad_sideband_cfg.rpo_blocks.push_back(valid_bb);
  bad_sideband_cfg.sideband.reg_renumber[10] = 2;  // Not a pseudo register (< 68).
  EXPECT_FALSE(InvertReload(bad_sideband_cfg).ok());

  RtlControlFlowGraph fcr31_sideband_cfg = bad_sideband_cfg;
  fcr31_sideband_cfg.sideband.reg_renumber.clear();
  fcr31_sideband_cfg.sideband.reg_renumber[67] = 2;  // $fcr31 (67) is a hard register (< 68).
  EXPECT_FALSE(InvertReload(fcr31_sideband_cfg).ok());
}

// Negative test 5: Verifies that `VerifyReloadRightInverse` rejects a corrupted inverse.
TEST_F(InvertReloadTest, VerifierRejectsCorruptedInverseProvider) {
  RtlControlFlowGraph reload_cfg;
  reload_cfg.pass = RtlPassKind::kGreg;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {
      "(set (reg:SI 2 $2) (const_int 42))",
      "(use (reg/i:SI 2 $2))",
  };
  bb0.SyncRtlInsnsFromInstructions();
  reload_cfg.rpo_blocks.push_back(bb0);

  CorruptedReloadInverseProvider bad_provider;
  RightInverseVerifier verifier(ctx, hooks, &bad_provider);
  RightInverseResult vres = verifier.VerifyReloadRightInverse(reload_cfg);
  EXPECT_FALSE(vres.matches);
}

TEST_F(InvertReloadTest, ReversesCallerSaveSaveRestorePairsAndRestoresFrameOffset) {
  RtlControlFlowGraph pre_reload_cfg;
  pre_reload_cfg.pass = RtlPassKind::kLreg;
  pre_reload_cfg.sideband.frame_pointer_needed = false;
  pre_reload_cfg.sideband.outgoing_args_size = 16;
  pre_reload_cfg.sideband.frame_size_bytes = 8;
  pre_reload_cfg.sideband.reg_renumber[72] = 8;  // $t0 (call-clobbered)
  pre_reload_cfg.sideband.reg_n_calls_crossed[72] = 1;
  pre_reload_cfg.sideband.reg_n_refs[72] = 4;
  pre_reload_cfg.sideband.reg_live_length[72] = 4;

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.AddPatternOnly("(set (mem/v:SI (plus:SI (reg:SI 30 fp) (const_int 16))) (const_int 0))");
  bb0.AddPatternOnly("(set (reg:SI 72) (const_int 11))");
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Helper\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  call_insn.call_fusage = {};
  bb0.AddInsn(std::move(call_insn));
  bb0.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 72))) "
      "(expr_list:REG_DEAD (reg:SI 72) (nil))");
  bb0.AddPatternOnly("(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  pre_reload_cfg.rpo_blocks.push_back(std::move(bb0));

  const SubfunctionBoundarySnapshot reload_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, pre_reload_cfg, {}, /*optimize_level=*/2, {"-O2", "-mips2"});
  ASSERT_GT(reload_snap.post_state.rpo_blocks[0].instructions.size(), 5u)
      << reload_snap.post_state.FormatSummary();
  EXPECT_GT(reload_snap.post_state.sideband.frame_size_bytes, 8);

  absl::StatusOr<RtlControlFlowGraph> pre_or = InvertReload(reload_snap.post_state);
  ASSERT_TRUE(pre_or.ok()) << pre_or.status();
  EXPECT_EQ(pre_or->rpo_blocks[0].instructions.size(), 5u);
  EXPECT_EQ(pre_or->sideband.frame_size_bytes, 8);

  ReloadOnlyInverseProvider good_provider;
  RightInverseVerifier good_verifier(ctx, hooks, &good_provider);
  const RightInverseResult pos_sub_res =
      good_verifier.VerifySubfunctionBoundaryRightInverse(reload_snap);
  EXPECT_TRUE(pos_sub_res.matches) << pos_sub_res.diff_detail;
  const RightInverseResult pos_stage_res =
      good_verifier.VerifyReloadRightInverse(reload_snap.post_state);
  EXPECT_TRUE(pos_stage_res.matches) << pos_stage_res.diff_detail;

  // Adversarial negative: failing to strip the caller-save save/restore pairs or corrupting the
  // pre-image must fail `RightInverseVerifier`.
  class NoOpCallerSaveReloadProvider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& x) const override { return x; }
  };
  NoOpCallerSaveReloadProvider noop_provider;
  RightInverseVerifier noop_verifier(ctx, hooks, &noop_provider);
  EXPECT_FALSE(noop_verifier.VerifySubfunctionBoundaryRightInverse(reload_snap).matches);
}

TEST_F(InvertReloadTest, ReconstructsSpilledPseudosAlongsideCallerSaveAcrossOutgoingArgCall) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int Dispatch8Args(int a, int b, int c, int d, int e, int f, int g, int h);\n"
      "int SpillAndCallerSave(int* p) {\n"
      "  int v0 = p[0], v1 = p[1], v2 = p[2], v3 = p[3], v4 = p[4], v5 = p[5];\n"
      "  int v6 = p[6], v7 = p[7], v8 = p[8], v9 = p[9], v10 = p[10], v11 = p[11];\n"
      "  int c0 = v0 ^ v1, c1 = v2 ^ v3, c2 = v4 ^ v5, c3 = v6 ^ v7;\n"
      "  int r = Dispatch8Args(c0, c1, c2, c3, v8 ^ v9, v10 ^ v11, v0 + v6, v1 + v7);\n"
      "  return r + v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11;\n"
      "}\n",
      "SpillAndCallerSave", {"-O2", "-mips2", "-mcpu=r4000"});

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  bool found_reload = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      found_reload = true;
      RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(vres.matches) << vres.diff_detail;
    }
  }
  EXPECT_TRUE(found_reload);
}

TEST_F(InvertReloadTest, ReconstructsActiveSpilledPseudoAlongsidePhantomAlterRegPseudos) {
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int ProbeStep(int iter, int acc);\n"
      "int LoopSpillAndCallerSave(const int* base, int unused_arg, int steps) {\n"
      "  int k0 = base[0], k1 = base[1], k2 = base[2], k3 = base[3], k4 = base[4], k5 = base[5];\n"
      "  int a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6;\n"
      "  int b0 = 7, b1 = 8, b2 = 9, b3 = 10, b4 = 11, b5 = 12;\n"
      "  int i;\n"
      "  for (i = 0; i < steps; i++) {\n"
      "    int p = ProbeStep(i, a0 + b5);\n"
      "    a0 = (a0 + k0) ^ p; a1 = (a1 + k1) + a0; a2 = (a2 ^ k2) + a1;\n"
      "    a3 = (a3 + k3) ^ a2; a4 = (a4 - k4) + a3; a5 = (a5 + k5) ^ a4;\n"
      "    b0 = (b0 ^ a5) + k5; b1 = (b1 + b0) ^ k4; b2 = (b2 - b1) + k3;\n"
      "    b3 = (b3 ^ b2) + k2; b4 = (b4 + b3) ^ k1; b5 = (b5 - b4) + k0;\n"
      "  }\n"
      "  return a0 + a1 + a2 + a3 + a4 + a5 + b0 + b1 + b2 + b3 + b4 + b5;\n"
      "}\n",
      "LoopSpillAndCallerSave", {"-O2", "-mips2", "-mcpu=r4000"});

  const SubfunctionBoundarySnapshot* base_reload = nullptr;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      base_reload = &snap;
      break;
    }
  }
  ASSERT_NE(base_reload, nullptr);

  RtlControlFlowGraph pre_cfg = base_reload->pre_state;
  const int base_frame = pre_cfg.sideband.frame_size_bytes;
  constexpr int kLowPhantomPseudo = 73;
  ASSERT_FALSE(pre_cfg.sideband.reg_renumber.contains(kLowPhantomPseudo));
  const int high_phantom_pseudo = pre_cfg.sideband.max_regno + 2;
  pre_cfg.sideband.max_regno = high_phantom_pseudo + 1;
  pre_cfg.sideband.reg_n_refs[kLowPhantomPseudo] = 2;
  pre_cfg.sideband.reg_live_length[kLowPhantomPseudo] = 2;
  pre_cfg.sideband.reg_n_sets.erase(kLowPhantomPseudo);
  pre_cfg.sideband.reg_n_deaths.erase(kLowPhantomPseudo);

  pre_cfg.sideband.reg_n_refs[high_phantom_pseudo] = 2;
  pre_cfg.sideband.reg_live_length[high_phantom_pseudo] = 2;

  const SubfunctionBoundarySnapshot reload_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, pre_cfg, {}, /*optimize_level=*/2, {"-O2", "-mips2", "-mcpu=r4000"});
  EXPECT_EQ(reload_snap.post_state.sideband.frame_size_bytes,
            base_reload->post_state.sideband.frame_size_bytes + 16);

  absl::StatusOr<RtlControlFlowGraph> base_lifted = InvertReload(base_reload->post_state);
  ASSERT_TRUE(base_lifted.ok()) << base_lifted.status();
  auto count_phantom_pseudos = [](const RtlControlFlowGraph& g) {
    absl::flat_hash_set<int> used;
    for (const RtlBasicBlock& blk : g.rpo_blocks) {
      for (const std::string& s : blk.instructions) {
        if (auto pn = ParsePatternAndNotes(s); pn.ok()) {
          pn->pattern.Visit([&](const RtlExpr& e) {
            if (e.IsPseudoReg()) {
              used.insert(e.RegNo());
            }
          });
        }
      }
    }
    size_t count = 0;
    for (const auto& [p, nrefs] : g.sideband.reg_n_refs) {
      if (p >= 68 && nrefs > 0 && !g.sideband.reg_renumber.contains(p) && !used.contains(p)) {
        ++count;
      }
    }
    return count;
  };
  const size_t base_phantom_count = count_phantom_pseudos(*base_lifted);

  // Clear incoming `reg_n_refs`/`reg_live_length`/`reg_n_deaths` on a copy of `post_state` to prove
  // `InvertReload` infers phantom `alter_reg` slots purely from unreferenced 8-byte stack gaps in
  // `[starting_frame_offset, pre_caller_save_top)` with zero oracle sideband dependence.
  RtlControlFlowGraph stripped_post = reload_snap.post_state;
  stripped_post.sideband.reg_n_refs.clear();
  stripped_post.sideband.reg_n_sets.clear();
  stripped_post.sideband.reg_n_deaths.clear();
  stripped_post.sideband.reg_live_length.clear();
  stripped_post.sideband.reg_n_calls_crossed.clear();

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(stripped_post);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.frame_size_bytes, base_frame);

  // Collect RTL-referenced pseudos vs synthesized zero-occurrence phantom pseudos.
  absl::flat_hash_set<int> rtl_pseudos;
  for (const RtlBasicBlock& blk : lifted->rpo_blocks) {
    for (const std::string& insn_s : blk.instructions) {
      absl::StatusOr<RtlPatternAndNotes> pn = ParsePatternAndNotes(insn_s);
      ASSERT_TRUE(pn.ok()) << pn.status();
      pn->pattern.Visit([&](const RtlExpr& e) {
        if (e.IsPseudoReg()) {
          rtl_pseudos.insert(e.RegNo());
        }
      });
    }
  }
  std::vector<int> unalloc_pseudos;
  std::vector<int> phantom_pseudos;
  for (const auto& [p, nrefs] : lifted->sideband.reg_n_refs) {
    if (p >= 68 && nrefs > 0 && !lifted->sideband.reg_renumber.contains(p)) {
      unalloc_pseudos.push_back(p);
      if (!rtl_pseudos.contains(p)) {
        phantom_pseudos.push_back(p);
        EXPECT_EQ(nrefs, 2);
        EXPECT_FALSE(lifted->sideband.reg_n_sets.contains(p));
        ASSERT_TRUE(lifted->sideband.reg_n_deaths.contains(p));
        EXPECT_EQ(lifted->sideband.reg_n_deaths.at(p), 1);
        ASSERT_TRUE(lifted->sideband.reg_live_length.contains(p));
        EXPECT_EQ(lifted->sideband.reg_live_length.at(p), 2);
      }
    }
  }
  std::sort(unalloc_pseudos.begin(), unalloc_pseudos.end());
  std::sort(phantom_pseudos.begin(), phantom_pseudos.end());
  ASSERT_GE(unalloc_pseudos.size(), base_phantom_count + 3u);
  ASSERT_EQ(phantom_pseudos.size(), base_phantom_count + 2u);
  // The low phantom gap precedes the active spilled pseudo(s) and the high phantom gap follows them
  // in ascending pseudo order (`reload1.c:654-658`).
  EXPECT_EQ(phantom_pseudos.front(), unalloc_pseudos.front());
  EXPECT_EQ(phantom_pseudos.back(), unalloc_pseudos.back());

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  const RightInverseResult sub_res = verifier.VerifySubfunctionBoundaryRightInverse(reload_snap);
  EXPECT_TRUE(sub_res.matches) << sub_res.diff_detail;
  hooks->ApplyCompilationProfile({2, {"-O2", "-mips2", "-mcpu=r4000"}, {}});
  const RightInverseResult stage_res = verifier.VerifyReloadRightInverse(stripped_post);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

TEST_F(InvertReloadTest, DoesNotSynthesizePhantomPseudosForTrailingCallerSaveSetupSaveAreasGap) {
  // Under `-mips3` (`64-bit` GPR/FPR save slots), `setup_save_areas` (`caller-save.c:263-331`)
  // conservatively allocates 8-byte save slots for BOTH `$t0 = 8` (`[16, 24)`) and `$f0 = 32`
  // (`[24, 32)`), whereas `save_call_clobbered_regs` (`caller-save.c:459`) skips saving `$f0` at
  // `DoubleCall` in `B1` because `DoubleCall` itself sets `(reg:DF 32 $f0)` (`this_call_sets`),
  // while `B0 -> B2` keeps pseudo `73` (`$f0`) reaching `B2` as a single web! Thus `[24, 32)` is a
  // trailing unreferenced 8-byte `setup_save_areas` slot ABOVE `pre_caller_save_top` (`16`), NOT a
  // pre-`caller-save` phantom `alter_reg` slot!
  const CompilationProfile prof{2, {"-O2", "-mips3"}, {}};
  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kLreg;
  pre_cfg.sideband.frame_pointer_needed = false;
  pre_cfg.sideband.outgoing_args_size = 16;
  pre_cfg.sideband.frame_size_bytes = 0;
  pre_cfg.sideband.saved_gpr_regnos = {31};
  pre_cfg.sideband.max_regno = 74;
  pre_cfg.sideband.reg_renumber[72] = 8;   // `$t0` saved & restored at `[16, 24)`
  pre_cfg.sideband.reg_renumber[73] = 32;  // `$f0` (`DFmode`) allocated `[24, 32)` by
                                           // `setup_save_areas`, skipped by `this_call_sets`!
  pre_cfg.sideband.reg_n_calls_crossed[72] = 1;
  pre_cfg.sideband.reg_n_calls_crossed[73] = 1;
  pre_cfg.sideband.reg_n_refs[72] = 2;
  pre_cfg.sideband.reg_n_refs[73] = 2;
  pre_cfg.sideband.reg_live_length[72] = 6;
  pre_cfg.sideband.reg_live_length[73] = 6;
  pre_cfg.sideband.basic_block_live_at_start[0] = {4, 5, 29, 44};
  pre_cfg.sideband.basic_block_live_at_start[1] = {29, 72, 73};
  pre_cfg.sideband.basic_block_live_at_start[2] = {29, 72, 73};

  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.AddPatternOnly("(set (reg:DI 72) (reg:DI 4 a0)) (expr_list:REG_DEAD (reg:DI 4 a0) (nil))");
  bb0.AddPatternOnly(
      "(set (reg:DF 73) (reg:DF 44 $f12)) (expr_list:REG_DEAD (reg:DF 44 $f12) (nil))");
  bb0.AddPatternOnly(
      "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) (label_ref L0) (pc))) "
      "(expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  bb0.successors = {2, 1};
  pre_cfg.rpo_blocks.push_back(std::move(bb0));

  RtlBasicBlock bb1;
  bb1.id = 1;
  bb1.predecessors = {0};
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (set (reg:DF 32 $f0) (call (mem:SI (symbol_ref:SI (\"DoubleCall\"))) "
      "(const_int 16))) (clobber (reg:SI 31 ra)) ] ) "
      "(expr_list:REG_UNUSED (reg:DF 32 $f0) (nil))";
  call_insn.has_explicit_call_fusage = true;
  bb1.AddInsn(std::move(call_insn));
  bb1.successors = {2};
  pre_cfg.rpo_blocks.push_back(std::move(bb1));

  RtlBasicBlock bb2;
  bb2.id = 2;
  bb2.has_label = true;
  bb2.predecessors = {0, 1};
  bb2.AddPatternOnly(
      "(set (mem/v:DF (reg:DI 72)) (reg:DF 73)) "
      "(expr_list:REG_DEAD (reg:DI 72) (expr_list:REG_DEAD (reg:DF 73) (nil)))");
  bb2.AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  bb2.AddPatternOnly("(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  pre_cfg.rpo_blocks.push_back(std::move(bb2));

  hooks->ApplyCompilationProfile(prof);
  const SubfunctionBoundarySnapshot reload_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, pre_cfg, {}, prof.optimize_level, prof.c_flags);
  // `setup_save_areas` allocated 16 bytes (`[16, 24)` for `$t0`, `[24, 32)` for `$f0`), even
  // though `[24, 32)` is never referenced in RTL!
  EXPECT_EQ(reload_snap.post_state.sideband.frame_size_bytes, 16);

  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_snap.post_state, prof);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.frame_size_bytes, 0);
  for (const auto& [p, nrefs] : lifted->sideband.reg_n_refs) {
    if (p >= 68 && nrefs > 0) {
      EXPECT_TRUE(lifted->sideband.reg_renumber.contains(p))
          << "Unexpected phantom pseudo synthesized for trailing caller-save gap: " << p;
    }
  }

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  const RightInverseResult sub_res = verifier.VerifySubfunctionBoundaryRightInverse(reload_snap);
  EXPECT_TRUE(sub_res.matches) << sub_res.diff_detail;
}

TEST_F(InvertReloadTest, RejectsNegativeFrameSizeAndDetectsMissingPhantomGapSynthesis) {
  // 1. Negative `frame_size_bytes` must be rejected with a non-OK `absl::Status`.
  RtlControlFlowGraph bad_frame_cfg;
  bad_frame_cfg.pass = RtlPassKind::kGreg;
  bad_frame_cfg.sideband.frame_size_bytes = -8;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 2 v0) (const_int 0))"};
  bb0.SyncRtlInsnsFromInstructions();
  bad_frame_cfg.rpo_blocks.push_back(bb0);
  EXPECT_FALSE(InvertReload(bad_frame_cfg).ok());

  // 2. Omitting phantom-gap synthesis when an unreferenced 8-byte slot precedes an active spilled
  //    pseudo shifts the spilled pseudo's stack offset on replay and fails `RightInverseVerifier`.
  const CompilationProfile prof{2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};
  RtlControlFlowGraph pre_cfg;
  pre_cfg.pass = RtlPassKind::kLreg;
  pre_cfg.sideband.frame_pointer_needed = false;
  pre_cfg.sideband.outgoing_args_size = 16;
  pre_cfg.sideband.frame_size_bytes = 0;
  pre_cfg.sideband.saved_gpr_regnos = {31};
  pre_cfg.sideband.max_regno = 74;
  // `72` is a phantom unallocated pseudo (`[16, 24)`), `73` is an active spilled pseudo (`[24,
  // 32)`).
  pre_cfg.sideband.reg_n_refs[72] = 2;
  pre_cfg.sideband.reg_live_length[72] = 2;
  pre_cfg.sideband.reg_n_refs[73] = 2;
  pre_cfg.sideband.reg_n_sets[73] = 1;
  pre_cfg.sideband.reg_n_deaths[73] = 1;
  pre_cfg.sideband.reg_live_length[73] = 4;
  pre_cfg.sideband.reg_n_calls_crossed[73] = 1;
  pre_cfg.sideband.basic_block_live_at_start[0] = {4, 29};

  RtlBasicBlock spill_bb;
  spill_bb.id = 0;
  spill_bb.AddPatternOnly(
      "(set (reg:SI 73) (plus:SI (reg:SI 4 a0) (const_int 1))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  RtlInsn call_insn;
  call_insn.canonical_pattern =
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"Helper\"))) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )";
  call_insn.has_explicit_call_fusage = true;
  spill_bb.AddInsn(std::move(call_insn));
  spill_bb.AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 73))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  spill_bb.AddPatternOnly("(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");
  pre_cfg.rpo_blocks.push_back(std::move(spill_bb));

  hooks->ApplyCompilationProfile(prof);
  const SubfunctionBoundarySnapshot reload_snap = hooks->ExecuteSubfunctionBoundary(
      SubfunctionId::kReload, pre_cfg, {}, prof.optimize_level, prof.c_flags);
  EXPECT_EQ(reload_snap.post_state.sideband.frame_size_bytes, 16);

  ReloadOnlyInverseProvider good_provider;
  RightInverseVerifier good_verifier(ctx, hooks, &good_provider);
  EXPECT_TRUE(good_verifier.VerifySubfunctionBoundaryRightInverse(reload_snap).matches)
      << good_verifier.VerifySubfunctionBoundaryRightInverse(reload_snap).diff_detail;

  class StripPhantomPseudosReloadProvider final : public SubfunctionInverseProvider {
   public:
    RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg,
                                     const CompilationProfile& profile) const override {
      absl::StatusOr<RtlControlFlowGraph> res =
          ::rom_nom_nom::lifter::InvertReload(reload_cfg, profile);
      if (!res.ok()) {
        return {};
      }
      absl::flat_hash_set<int> rtl_regs;
      for (const RtlBasicBlock& blk : res->rpo_blocks) {
        for (const std::string& insn_s : blk.instructions) {
          if (auto pn = ParsePatternAndNotes(insn_s); pn.ok()) {
            pn->pattern.Visit([&](const RtlExpr& e) {
              if (e.IsPseudoReg()) {
                rtl_regs.insert(e.RegNo());
              }
            });
          }
        }
      }
      for (auto it = res->sideband.reg_n_refs.begin(); it != res->sideband.reg_n_refs.end();) {
        if (it->first >= 68 && !res->sideband.reg_renumber.contains(it->first) &&
            !rtl_regs.contains(it->first)) {
          res->sideband.reg_n_deaths.erase(it->first);
          res->sideband.reg_live_length.erase(it->first);
          res->sideband.reg_n_refs.erase(it++);
        } else {
          ++it;
        }
      }
      return *res;
    }
  };
  StripPhantomPseudosReloadProvider bad_provider;
  RightInverseVerifier bad_verifier(ctx, hooks, &bad_provider);
  EXPECT_FALSE(bad_verifier.VerifySubfunctionBoundaryRightInverse(reload_snap).matches);
}

TEST_F(InvertReloadTest,
       UnreferencedLocalAggregateGapCanonicallyAttributedToPhantomPseudoAndMatchesReload) {
  // Residual many-to-one ambiguity #4 (`non_invalidated_begin == 0`):
  // `char unused_buf[8];` is allocated at `starting_frame_offset` by
  // `stmt.c:expand_decl` (`snap.pre_state.sideband.frame_size_bytes == 8`) with
  // zero RTL occurrences in `.08.lreg`. In `.09.greg`, that unaccessed 8-byte
  // gap before spilled pseudos is bit-for-bit identical to an unallocated
  // phantom `alter_reg` pseudo slot. Verify `InvertReload` canonically
  // attributes the unaccessed 8-byte gap to a phantom spilled pseudo
  // (`lifted->sideband.frame_size_bytes == 0`) and satisfies $f(g(x)) = x$
  // across `SubfunctionId::kReload`.
  FunctionCompilationTrace trace = hooks->TraceForwardCompilation(
      "int ExternalStep(int x);\n"
      "int UnusedBufWithSpills(const int* p) {\n"
      "  char unused_buf[8];\n"
      "  int v0 = p[0], v1 = p[1], v2 = p[2], v3 = p[3], v4 = p[4], v5 = p[5];\n"
      "  int v6 = p[6], v7 = p[7], v8 = p[8], v9 = p[9], v10 = p[10];\n"
      "  int r = ExternalStep(v0 + v1);\n"
      "  return r + v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10;\n"
      "}\n",
      "UnusedBufWithSpills", {"-O2", "-mips2", "-mcpu=r4000"});

  const SubfunctionBoundarySnapshot* reload_snap = nullptr;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      reload_snap = &snap;
      break;
    }
  }
  ASSERT_NE(reload_snap, nullptr);
  EXPECT_EQ(reload_snap->pre_state.sideband.frame_size_bytes, 8);
  EXPECT_GT(reload_snap->post_state.sideband.frame_size_bytes, 8);

  const CompilationProfile prof{reload_snap->optimize_level, reload_snap->c_flags,
                                reload_snap->gp_symbol_sizes};
  absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(reload_snap->post_state, prof);
  ASSERT_TRUE(lifted.ok()) << lifted.status();
  EXPECT_EQ(lifted->sideband.frame_size_bytes, 0);

  absl::flat_hash_set<int> rtl_pseudos;
  for (const RtlBasicBlock& blk : lifted->rpo_blocks) {
    for (const std::string& insn_s : blk.instructions) {
      if (auto pn = ParsePatternAndNotes(insn_s); pn.ok()) {
        pn->pattern.Visit([&](const RtlExpr& e) {
          if (e.IsPseudoReg()) {
            rtl_pseudos.insert(e.RegNo());
          }
        });
      }
    }
  }
  std::vector<int> unalloc_pseudos;
  std::vector<int> phantom_pseudos;
  for (const auto& [p, nrefs] : lifted->sideband.reg_n_refs) {
    if (p >= 68 && nrefs > 0 && !lifted->sideband.reg_renumber.contains(p)) {
      unalloc_pseudos.push_back(p);
      if (!rtl_pseudos.contains(p)) {
        phantom_pseudos.push_back(p);
      }
    }
  }
  std::sort(unalloc_pseudos.begin(), unalloc_pseudos.end());
  std::sort(phantom_pseudos.begin(), phantom_pseudos.end());
  ASSERT_FALSE(phantom_pseudos.empty());
  ASSERT_GT(unalloc_pseudos.size(), phantom_pseudos.size());
  EXPECT_EQ(phantom_pseudos.front(), unalloc_pseudos.front());

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);
  const RightInverseResult sub_res = verifier.VerifySubfunctionBoundaryRightInverse(*reload_snap);
  EXPECT_TRUE(sub_res.matches) << sub_res.diff_detail;
  hooks->ApplyCompilationProfile(prof);
  const RightInverseResult stage_res = verifier.VerifyReloadRightInverse(reload_snap->post_state);
  EXPECT_TRUE(stage_res.matches) << stage_res.diff_detail;
}

class InvertReloadCorpusTest : public InvertReloadTest,
                               public ::testing::WithParamInterface<RoundTripTestCase> {};

TEST_P(InvertReloadCorpusTest, All4172CasesSatisfyBoundaryAndFreshPreReloadRightInverse) {
  const RoundTripTestCase& tc = GetParam();
  const FunctionCompilationTrace trace = hooks->TraceForwardCompilation(tc);
  ASSERT_FALSE(trace.subfunction_snapshots.empty())
      << "Failed to capture snapshots for " << tc.test_name;

  ReloadOnlyInverseProvider provider;
  RightInverseVerifier verifier(ctx, hooks, &provider);

  // 1. Isolated `SubfunctionId::kReload` boundary right-inverse (`f(g(x)) == x`).
  bool found_reload = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kReload) {
      found_reload = true;
      const RightInverseResult vres = verifier.VerifySubfunctionBoundaryRightInverse(snap);
      const CompilationProfile prof{snap.optimize_level, snap.c_flags, snap.gp_symbol_sizes};
      const absl::StatusOr<RtlControlFlowGraph> lifted = InvertReload(snap.post_state, prof);
      EXPECT_TRUE(vres.matches) << "Case " << tc.test_name
                                << " failed VerifySubfunctionBoundaryRightInverse(kReload): "
                                << vres.diff_detail << "\nsnap.pre_state:\n"
                                << snap.pre_state.FormatSummary() << "\nlifted:\n"
                                << (lifted.ok() ? lifted->FormatSummary()
                                                : std::string(lifted.status().message()));
    }
  }
  EXPECT_TRUE(found_reload) << "Missing kReload snapshot for " << tc.test_name;

  // 2. Fresh cumulative composition (`Pass00 -> Pass01 -> Pass02 -> Pass03 ->
  //    InvertAlterReg -> InvertReload`) and fresh-context forward replay to
  //    bit-exact `.text` machine words, relocations, jump tables, and constant pools.
  const CompilationProfile effective_profile = EffectiveStage0Profile(trace.stage0_input);
  const RtlControlFlowGraph y0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(y0.rpo_blocks.empty()) << "Empty Pass00 CFG on " << tc.test_name;
  const RtlControlFlowGraph y1 = InvertPass01DbrSchedule(y0, effective_profile);
  ASSERT_FALSE(y1.rpo_blocks.empty()) << "Empty Pass01 CFG on " << tc.test_name;
  const RtlControlFlowGraph y2 = InvertPass02Jump2CrossJump(y1, effective_profile);
  ASSERT_FALSE(y2.rpo_blocks.empty()) << "Empty Pass02 CFG on " << tc.test_name;
  const RtlControlFlowGraph fresh_greg = InvertPass03Sched2PostReload(y2, effective_profile);
  ASSERT_FALSE(fresh_greg.rpo_blocks.empty()) << "Empty Pass03 .09.greg CFG on " << tc.test_name;

  const RtlControlFlowGraph lifted_pre_alter = InvertAlterReg(fresh_greg, effective_profile, 72);
  ASSERT_FALSE(lifted_pre_alter.rpo_blocks.empty())
      << "Empty InvertAlterReg CFG on " << tc.test_name;

  const absl::StatusOr<RtlControlFlowGraph> lifted_pre_reload_or =
      InvertReload(lifted_pre_alter, effective_profile);
  ASSERT_TRUE(lifted_pre_reload_or.ok())
      << "Fresh InvertReload failed on " << tc.test_name << ": " << lifted_pre_reload_or.status();
  const RtlControlFlowGraph& lifted_pre_reload = *lifted_pre_reload_or;
  ASSERT_FALSE(lifted_pre_reload.rpo_blocks.empty()) << tc.test_name;
  EXPECT_EQ(lifted_pre_reload.pass, RtlPassKind::kLreg) << tc.test_name;

  for (const auto& [pseudo, hard] : lifted_pre_reload.sideband.reg_renumber) {
    EXPECT_GE(pseudo, 68) << tc.test_name;
    EXPECT_GE(hard, -1) << tc.test_name;
    EXPECT_LT(hard, 68) << tc.test_name;
  }
  if (!fresh_greg.sideband.frame_pointer_needed) {
    EXPECT_FALSE(HasRawSpMemAboveOutgoingArgs(lifted_pre_reload))
        << "Un-eliminated raw $sp local/incoming stack MEM left in fresh lifted_pre_reload on "
        << tc.test_name << ":\n"
        << lifted_pre_reload.FormatSummary();
  }

  // Reset `ctx` and replay forward from `SubfunctionId::kReload` through
  // `RunFinal` + `RunGasMdAssembleWithMetadata` with zero oracle leakage.
  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(effective_profile);
  ctx->MaterializeFromCfg(lifted_pre_reload);
  {
    std::string bad_insn;
    ASSERT_TRUE(hooks->ValidateMaterializedInsnsRecognizable(&bad_insn))
        << "Unrecognizable lifted_pre_reload insn on " << tc.test_name << ": " << bad_insn << "\n"
        << lifted_pre_reload.FormatSummary();
  }

  constexpr SubfunctionId kReplaySubs[] = {
      SubfunctionId::kReload,
      SubfunctionId::kAlterReg,
      SubfunctionId::kScheduleBlockSched2,
      SubfunctionId::kDoCrossJump,
      SubfunctionId::kFillSimpleDelaySlots,
      SubfunctionId::kFillEagerDelaySlots,
      SubfunctionId::kRelaxDelaySlots,
      SubfunctionId::kMakeReturnInsns,
  };
  RtlControlFlowGraph current_cfg = lifted_pre_reload;
  RtlControlFlowGraph replayed_post_alter;
  for (SubfunctionId sub : kReplaySubs) {
    current_cfg = hooks
                      ->ExecuteSubfunctionBoundary(
                          sub, current_cfg, {}, effective_profile.optimize_level,
                          effective_profile.c_flags, effective_profile.gp_symbol_sizes)
                      .post_state;
    if (sub == SubfunctionId::kAlterReg) {
      replayed_post_alter = current_cfg;
      EXPECT_EQ(current_cfg.sideband.frame_pointer_needed, fresh_greg.sideband.frame_pointer_needed)
          << "Post-kAlterReg frame_pointer_needed mismatch on " << tc.test_name;
      EXPECT_EQ(current_cfg.sideband.outgoing_args_size, fresh_greg.sideband.outgoing_args_size)
          << "Post-kAlterReg outgoing_args_size mismatch on " << tc.test_name;
    }
  }

  hooks->ApplyCompilationProfile(effective_profile);
  ctx->MaterializeFromCfg(current_cfg);
  {
    std::string bad_insn;
    ASSERT_TRUE(hooks->ValidateMaterializedInsnsRecognizable(&bad_insn))
        << "Unrecognizable post-replay RTL insn on " << tc.test_name << ": " << bad_insn;
  }
  const std::string combined_asm = hooks->RunFinal();
  const Stage0FunctionInput actual_stage0 = hooks->RunGasMdAssembleWithMetadata(combined_asm);
  const std::string stage0_diff =
      CompareStage0ReplayResult(trace.stage0_input, actual_stage0, combined_asm);
  EXPECT_TRUE(stage0_diff.empty())
      << "Fresh Pass00..InvertReload -> forward Stage0 replay mismatch on " << tc.test_name << ":\n"
      << stage0_diff << "\nlifted_pre_reload:\n"
      << lifted_pre_reload.FormatSummary() << "\nreplayed_post_alter:\n"
      << replayed_post_alter.FormatSummary();
}

INSTANTIATE_TEST_SUITE_P(All4172Cases, InvertReloadCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
