#include "lifter/pass06_sched1_pre_alloc.h"

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
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"
#include "lifter/invert_alter_reg.h"
#include "lifter/invert_block_alloc.h"
#include "lifter/invert_do_cross_jump.h"
#include "lifter/invert_fill_eager_delay_slots.h"
#include "lifter/invert_fill_simple_delay_slots.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"
#include "lifter/invert_gas_mips_ip.h"
#include "lifter/invert_global_alloc.h"
#include "lifter/invert_make_return_insns.h"
#include "lifter/invert_relax_delay_slots.h"
#include "lifter/invert_reload.h"
#include "lifter/invert_schedule_block.h"
#include "lifter/invert_update_equiv_regs.h"
#include "lifter/pass00_final_and_gas.h"
#include "lifter/pass01_dbr_schedule.h"
#include "lifter/pass02_jump2_cross_jump.h"
#include "lifter/pass03_sched2_post_reload.h"
#include "lifter/pass04_greg_reload.h"
#include "lifter/pass05_lreg_local_alloc.h"

extern "C" void regclass_init();  // NOLINT(readability-identifier-naming)

namespace rom_nom_nom::lifter {
namespace {

using ::rom_nom_nom::gcc_harness::FunctionCompilationTrace;
using ::rom_nom_nom::gcc_harness::FunctionHooks;
using ::rom_nom_nom::gcc_harness::GccContext;
using ::rom_nom_nom::gcc_harness::PrologueEpilogueFrameSpec;
using ::rom_nom_nom::gcc_harness::RightInverseResult;
using ::rom_nom_nom::gcc_harness::RightInverseVerifier;
using ::rom_nom_nom::gcc_harness::SubfunctionBoundarySnapshot;
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ProductionPass06InverseProvider final : public SubfunctionInverseProvider {
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

  std::vector<std::string> InvertGasAppendInsn(
      absl::Span<const uint32_t> words_x, absl::Span<const std::string> c_flags) const override {
    return rom_nom_nom::InvertGasAppendInsn(words_x, c_flags);
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
    if (scheduled_cfg_x.pass == RtlPassKind::kSched) {
      return lifter::InvertPass06Sched1PreAlloc(scheduled_cfg_x);
    }
    return lifter::InvertScheduleBlock(scheduled_cfg_x);
  }

  RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg_x,
                                          const CompilationProfile& profile) const override {
    if (scheduled_cfg_x.pass == RtlPassKind::kSched) {
      return lifter::InvertPass06Sched1PreAlloc(scheduled_cfg_x, profile);
    }
    return lifter::InvertScheduleBlock(scheduled_cfg_x, profile);
  }

  RtlControlFlowGraph InvertSched2PostReload(const RtlControlFlowGraph& scheduled_cfg_x,
                                             const CompilationProfile& profile,
                                             bool /*sched2_enabled*/) const override {
    return lifter::InvertPass03Sched2PostReload(scheduled_cfg_x, profile);
  }

  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     int pseudo_regno) const override {
    return lifter::InvertAlterReg(allocated_cfg_x, pseudo_regno);
  }

  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& allocated_cfg_x,
                                     const CompilationProfile& profile,
                                     int pseudo_regno) const override {
    return lifter::InvertAlterReg(allocated_cfg_x, profile, pseudo_regno);
  }

  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg_x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertReload(reload_cfg_x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& reload_cfg_x,
                                   const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertReload(reload_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg_x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertGlobalAlloc(greg_cfg_x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& greg_cfg_x,
                                        const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertGlobalAlloc(greg_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg_x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertBlockAlloc(lreg_cfg_x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& lreg_cfg_x,
                                       const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertBlockAlloc(lreg_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg_x) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertUpdateEquivRegs(lreg_cfg_x);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }

  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& lreg_cfg_x,
                                            const CompilationProfile& profile) const override {
    absl::StatusOr<RtlControlFlowGraph> res = lifter::InvertUpdateEquivRegs(lreg_cfg_x, profile);
    return res.ok() ? *res : RtlControlFlowGraph{};
  }
};

class CorruptedPass06InverseProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg_x,
                                          const CompilationProfile& profile) const override {
    RtlControlFlowGraph out = lifter::InvertPass06Sched1PreAlloc(scheduled_cfg_x, profile);
    if (!out.rpo_blocks.empty() && !out.rpo_blocks[0].instructions.empty()) {
      out.rpo_blocks[0].instructions[0] = "(set (reg:SI 72) (const_int 999))";
      out.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    }
    return out;
  }
};

Stage0FunctionInput BakeLinkedRomStage0Input(const Stage0FunctionInput& reloc_input) {
  Stage0FunctionInput linked;
  linked.machine_words = reloc_input.machine_words;
  linked.base_vram = 0x80020000u;
  linked.gp_vram = 0x80060000u;
  linked.profile = reloc_input.profile;
  linked.profile.gp_symbol_sizes.clear();
  linked.jump_tables = reloc_input.jump_tables;
  linked.constant_pool = reloc_input.constant_pool;

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
  for (const Stage0Relocation& rel : reloc_input.relocations) {
    uint32_t existing = 0;
    if (find_sym(rel.symbol_name, &existing)) {
      continue;
    }
    int64_t max_add = 0;
    bool any_gp = false;
    bool any_call = false;
    for (const Stage0Relocation& r2 : reloc_input.relocations) {
      if (r2.symbol_name == rel.symbol_name) {
        max_add = std::max(max_add, r2.addend);
        if (r2.kind == Stage0RelocKind::kMipsGpRel16 || r2.kind == Stage0RelocKind::kMipsLiteral) {
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
    const auto gp_sz_it = reloc_input.profile.gp_symbol_sizes.find(rel.symbol_name);
    if (gp_sz_it != reloc_input.profile.gp_symbol_sizes.end() && gp_sz_it->second > 0) {
      linked.vram_to_symbol_size[assigned] = gp_sz_it->second;
    } else if (max_add > 0) {
      linked.vram_to_symbol_size[assigned] = span_bytes;
    }
  }
  std::vector<bool> relocated_word(linked.machine_words.size(), false);
  for (const Stage0Relocation& rel : reloc_input.relocations) {
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
      linked.machine_words[rel.word_index] = (linked.machine_words[rel.word_index] & 0xffff0000u) |
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
  return linked;
}

class Pass06Sched1PreAllocTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    trace_ctx = new GccContext();
    trace_hooks = new FunctionHooks(trace_ctx);
    verify_ctx = new GccContext();
    verify_hooks = new FunctionHooks(verify_ctx);
    provider = new ProductionPass06InverseProvider();
    verifier = new RightInverseVerifier(verify_ctx, verify_hooks, provider);
  }

  static void TearDownTestSuite() {
    delete verifier;
    verifier = nullptr;
    delete provider;
    provider = nullptr;
    delete verify_hooks;
    verify_hooks = nullptr;
    delete verify_ctx;
    verify_ctx = nullptr;
    delete trace_hooks;
    trace_hooks = nullptr;
    delete trace_ctx;
    trace_ctx = nullptr;
  }

  static GccContext* trace_ctx;
  static FunctionHooks* trace_hooks;
  static GccContext* verify_ctx;
  static FunctionHooks* verify_hooks;
  static ProductionPass06InverseProvider* provider;
  static RightInverseVerifier* verifier;
};

GccContext* Pass06Sched1PreAllocTest::trace_ctx = nullptr;
FunctionHooks* Pass06Sched1PreAllocTest::trace_hooks = nullptr;
GccContext* Pass06Sched1PreAllocTest::verify_ctx = nullptr;
FunctionHooks* Pass06Sched1PreAllocTest::verify_hooks = nullptr;
ProductionPass06InverseProvider* Pass06Sched1PreAllocTest::provider = nullptr;
RightInverseVerifier* Pass06Sched1PreAllocTest::verifier = nullptr;

TEST_F(Pass06Sched1PreAllocTest, RejectsEmptyMalformedAndCorruptedInputs) {
  RtlControlFlowGraph empty_cfg;
  EXPECT_TRUE(InvertPass06Sched1PreAlloc(empty_cfg).rpo_blocks.empty());
  EXPECT_TRUE(InvertSched1PreAlloc(empty_cfg).rpo_blocks.empty());

  RtlControlFlowGraph malformed_cfg;
  malformed_cfg.pass = RtlPassKind::kSched;
  RtlBasicBlock bb0;
  bb0.id = 0;
  bb0.instructions = {"(set (reg:SI 72) (plus:SI (reg:SI 4 a0)"};
  malformed_cfg.rpo_blocks.push_back(bb0);
  EXPECT_TRUE(InvertPass06Sched1PreAlloc(malformed_cfg).rpo_blocks.empty());
  EXPECT_TRUE(InvertPass06Sched1PreAlloc(malformed_cfg, {.optimize_level = 0, .c_flags = {"-O0"}})
                  .rpo_blocks.empty());

  RtlControlFlowGraph bad_succ_cfg;
  bad_succ_cfg.pass = RtlPassKind::kSched;
  RtlBasicBlock bb_bad_edge;
  bb_bad_edge.id = 0;
  bb_bad_edge.instructions = {"(set (reg/i:SI 2 v0) (const_int 0))", "(use (reg/i:SI 2 v0))"};
  bb_bad_edge.successors = {5};
  bb_bad_edge.SyncRtlInsnsFromInstructions();
  bad_succ_cfg.rpo_blocks.push_back(bb_bad_edge);
  EXPECT_TRUE(InvertPass06Sched1PreAlloc(bad_succ_cfg).rpo_blocks.empty());

  for (const std::string& opt_flag : {std::string("-O2"), std::string("-O0")}) {
    trace_ctx->BeginFunction("harness_init");
    trace_hooks->ApplyCompilationProfile({opt_flag == "-O0" ? 0 : 2, {opt_flag}, {}});
    regclass_init();
    const FunctionCompilationTrace trace = trace_hooks->TraceForwardCompilation(
        "int simple_add(int a, int b) { return a + b + 3; }\n", "simple_add", {opt_flag});
    ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kSched));
    CorruptedPass06InverseProvider bad_provider;
    RightInverseVerifier bad_verifier(verify_ctx, verify_hooks, &bad_provider);
    verify_ctx->BeginFunction("harness_func");
    verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
    EXPECT_FALSE(
        bad_verifier.VerifySched1StageRightInverse(trace.pass_snapshots.at(RtlPassKind::kSched))
            .matches);
  }
}

TEST_F(Pass06Sched1PreAllocTest, HonorsOptimizationAndScheduleFlags) {
  RtlControlFlowGraph split_cfg;
  split_cfg.pass = RtlPassKind::kSched;
  split_cfg.sideband.reg_n_sets[72] = 2;
  split_cfg.rpo_blocks.resize(1);
  split_cfg.rpo_blocks[0].id = 0;
  split_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 72) (const_int 305397760))");
  split_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (ior:SI (reg:SI 72) (const_int 22136))) "
      "(expr_list:REG_EQUAL (const_int 305419896) (nil))");
  split_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 72) (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 72) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))");
  split_cfg.rpo_blocks[0].AddPatternOnly(
      "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))");

  // At `-O2`, `RunsSched1()` is true: `InvertUpdateFlowInfo` re-merges the split `large_int`.
  const RtlControlFlowGraph o2_out =
      InvertPass06Sched1PreAlloc(split_cfg, {.optimize_level = 2, .c_flags = {"-O2"}});
  ASSERT_EQ(o2_out.BlockCount(), 1u);
  EXPECT_EQ(o2_out.pass, RtlPassKind::kCombine);
  ASSERT_EQ(o2_out.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_EQ(o2_out.sideband.reg_n_sets.at(72), 1);

  // At `-O1` and `-O2 -fno-schedule-insns`, `RunsSched1()` is false: passthrough without merging.
  const RtlControlFlowGraph o1_out =
      InvertPass06Sched1PreAlloc(split_cfg, {.optimize_level = 1, .c_flags = {"-O1"}});
  ASSERT_EQ(o1_out.BlockCount(), 1u);
  EXPECT_EQ(o1_out.pass, RtlPassKind::kCombine);
  EXPECT_EQ(o1_out.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(o1_out.sideband.reg_n_sets.at(72), 2);

  const RtlControlFlowGraph nosched_out = InvertPass06Sched1PreAlloc(
      split_cfg, {.optimize_level = 2, .c_flags = {"-O2", "-fno-schedule-insns"}});
  ASSERT_EQ(nosched_out.BlockCount(), 1u);
  EXPECT_EQ(nosched_out.pass, RtlPassKind::kCombine);
  EXPECT_EQ(nosched_out.rpo_blocks[0].instructions.size(), 4u);
}

TEST_F(Pass06Sched1PreAllocTest, RestorePreSched1FlowLivenessPreventsSched1DeadNotesAborts) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2"}};

  // 1. Extra `REG_DEAD` on a self-updating `SET` (`dead_or_set_p == true`, which would leave
  //    `dead_notes != 0` and trigger `sched.c:3968` `abort()`): `InvertPass06Sched1PreAlloc`
  //    strips the illegal self-`REG_DEAD` while leaving `(use (reg/i:SI 2 v0))` untouched, and
  //    forward `RunScheduleInsns` completes cleanly without aborting.
  RtlControlFlowGraph self_dead_cfg;
  self_dead_cfg.pass = RtlPassKind::kSched;
  self_dead_cfg.rpo_blocks.resize(3);
  self_dead_cfg.rpo_blocks[0].id = 0;
  self_dead_cfg.rpo_blocks[0].AddPatternOnly("(set (reg:SI 2 v0) (const_int 0))");
  self_dead_cfg.rpo_blocks[0].successors = {1};

  self_dead_cfg.rpo_blocks[1].id = 1;
  self_dead_cfg.rpo_blocks[1].has_label = true;
  self_dead_cfg.rpo_blocks[1].predecessors = {0, 1};
  self_dead_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0))) "
      "(expr_list:REG_DEAD (reg:SI 2 v0) (nil))");
  self_dead_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int -1)))");
  self_dead_cfg.rpo_blocks[1].AddPatternOnly(
      "(set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
  self_dead_cfg.rpo_blocks[1].successors = {1, 2};

  self_dead_cfg.rpo_blocks[2].id = 2;
  self_dead_cfg.rpo_blocks[2].predecessors = {1};
  self_dead_cfg.rpo_blocks[2].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const RtlControlFlowGraph cleaned_self_dead = InvertPass06Sched1PreAlloc(self_dead_cfg, o2_prof);
  ASSERT_EQ(cleaned_self_dead.BlockCount(), 3u);
  EXPECT_EQ(cleaned_self_dead.rpo_blocks[1].instructions[0],
            "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))");
  EXPECT_EQ(cleaned_self_dead.rpo_blocks[2].instructions[0], "(use (reg/i:SI 2 v0))");

  verify_ctx->BeginFunction("self_dead_replay");
  verify_hooks->ApplyCompilationProfile(o2_prof);
  verify_ctx->MaterializeFromCfg(cleaned_self_dead);
  verify_hooks->RunScheduleInsns();
  const RtlControlFlowGraph replayed_self =
      verify_ctx->DematerializeToCfg(verify_ctx->GetInsns(), RtlPassKind::kSched);
  EXPECT_EQ(replayed_self.BlockCount(), 3u);

  // 2. Missing intra-block `REG_DEAD` immediately before a full redefinition of `r72` (which
  //    would exhaust `dead_notes == 0` and trigger `sched.c:2728` `abort()`):
  //    `InvertPass06Sched1PreAlloc` restores `(REG_DEAD (reg:SI 72))` and `RunScheduleInsns`
  //    completes cleanly without aborting.
  RtlControlFlowGraph missing_dead_cfg;
  missing_dead_cfg.pass = RtlPassKind::kSched;
  missing_dead_cfg.rpo_blocks.resize(1);
  missing_dead_cfg.rpo_blocks[0].id = 0;
  missing_dead_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 8))) "
      "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  missing_dead_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 7)))");
  missing_dead_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (and:SI (reg:SI 73) (const_int -8))) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  missing_dead_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:SI 2 v0) (reg:SI 72)) (expr_list:REG_DEAD (reg:SI 72) (nil))");
  missing_dead_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const RtlControlFlowGraph repaired_missing =
      InvertPass06Sched1PreAlloc(missing_dead_cfg, o2_prof);
  ASSERT_EQ(repaired_missing.BlockCount(), 1u);
  EXPECT_EQ(repaired_missing.rpo_blocks[0].instructions[1],
            "(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 7))) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");

  verify_ctx->BeginFunction("missing_dead_replay");
  verify_hooks->ApplyCompilationProfile(o2_prof);
  verify_ctx->MaterializeFromCfg(repaired_missing);
  verify_hooks->RunScheduleInsns();
  const RtlControlFlowGraph replayed_missing =
      verify_ctx->DematerializeToCfg(verify_ctx->GetInsns(), RtlPassKind::kSched);
  ASSERT_EQ(replayed_missing.BlockCount(), 1u);
  EXPECT_EQ(replayed_missing.rpo_blocks[0].instructions[1],
            "(set (reg:SI 73) (plus:SI (reg:SI 72) (const_int 7))) "
            "(expr_list:REG_DEAD (reg:SI 72) (nil))");

  // 3. `VOIDmode` hard-register `REG_DEAD` note (`(reg 67 $fcr31)` on a `CC_FP` branch, where
  //    `HARD_REGNO_NREGS(67, VOIDmode) == 0` at `mips.h:1257` causes `sched.c:3589` to skip
  //    clearing bit 67 and triggers `sched.c:3968` `abort()`): `InvertPass06Sched1PreAlloc`
  //    repairs the note mode to `CC_FP` and `RunScheduleInsns` completes cleanly.
  RtlControlFlowGraph void_ccfp_cfg;
  void_ccfp_cfg.pass = RtlPassKind::kSched;
  void_ccfp_cfg.rpo_blocks.resize(2);
  void_ccfp_cfg.rpo_blocks[0].id = 0;
  void_ccfp_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:CC_FP 67 $fcr31) (lt:CC_FP (reg:SF 44 $f12) (reg:SF 46 $f14))) "
      "(expr_list:REG_DEAD (reg:SF 44 $f12) (expr_list:REG_DEAD (reg:SF 46 $f14) (nil)))");
  void_ccfp_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (pc) (if_then_else (ne:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0)) (label_ref L0) "
      "(pc))) (expr_list:REG_DEAD (reg 67 $fcr31) (nil))");
  void_ccfp_cfg.rpo_blocks[0].successors = {1, 1};
  void_ccfp_cfg.rpo_blocks[1].id = 1;
  void_ccfp_cfg.rpo_blocks[1].has_label = true;
  void_ccfp_cfg.rpo_blocks[1].predecessors = {0};
  void_ccfp_cfg.rpo_blocks[1].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  void_ccfp_cfg.rpo_blocks[1].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const RtlControlFlowGraph repaired_void = InvertPass06Sched1PreAlloc(void_ccfp_cfg, o2_prof);
  ASSERT_EQ(repaired_void.BlockCount(), 2u);
  EXPECT_NE(repaired_void.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:CC_FP 67 $fcr31)"),
            std::string::npos);

  verify_ctx->BeginFunction("void_ccfp_replay");
  verify_hooks->ApplyCompilationProfile(o2_prof);
  verify_ctx->MaterializeFromCfg(repaired_void);
  verify_hooks->RunScheduleInsns();
  const RtlControlFlowGraph replayed_void =
      verify_ctx->DematerializeToCfg(verify_ctx->GetInsns(), RtlPassKind::kSched);
  EXPECT_EQ(replayed_void.BlockCount(), 2u);
}

TEST_F(Pass06Sched1PreAllocTest, RestoresCallReturnValueAndPartialSubregLivenessInSched1) {
  const CompilationProfile o2_prof = {2, {"-O2", "-mips2", "-mcpu=r4000"}, {}};

  // 1. Step B (`sched.c:3532-3611`) must NOT kill `call_used_regs` on the forward scan: a
  //    `CALL_INSN` marks its explicit return-value `SET` `(reg:SI 2 v0)` live, so a subsequent
  //    store reading `v0` before `(use (reg/i:SI 2 v0))` must NOT receive a bogus `REG_DEAD v0`.
  RtlControlFlowGraph call_ret_cfg;
  call_ret_cfg.pass = RtlPassKind::kSched;
  call_ret_cfg.rpo_blocks.resize(1);
  call_ret_cfg.rpo_blocks[0].id = 0;
  call_ret_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  call_ret_cfg.rpo_blocks[0].AddPatternOnly(
      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref \"helper\")) (const_int 16))) "
      "(clobber (reg:SI 31 ra)) ] )");
  call_ret_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (reg:SI 72)) (reg:SI 2 v0)) (expr_list:REG_DEAD (reg:SI 72) (nil))");
  call_ret_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  const RtlControlFlowGraph lifted_call_ret = InvertPass06Sched1PreAlloc(call_ret_cfg, o2_prof);
  ASSERT_EQ(lifted_call_ret.BlockCount(), 1u);
  ASSERT_EQ(lifted_call_ret.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_EQ(lifted_call_ret.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 2 v0)"),
            std::string::npos)
      << lifted_call_ret.rpo_blocks[0].instructions[2];

  verify_ctx->BeginFunction("call_ret_replay");
  verify_hooks->ApplyCompilationProfile(o2_prof);
  verify_ctx->MaterializeFromCfg(lifted_call_ret);
  verify_hooks->RunScheduleInsns();
  const RtlControlFlowGraph replayed_call_ret =
      verify_ctx->DematerializeToCfg(verify_ctx->GetInsns(), RtlPassKind::kSched);
  ASSERT_EQ(replayed_call_ret.BlockCount(), 1u);
  EXPECT_EQ(replayed_call_ret.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 2 v0)"),
            std::string::npos)
      << replayed_call_ret.rpo_blocks[0].instructions[2];

  // 2. Partial-def `SUBREG` (`sched.c:2320-2366, 3543, 3783`): two word-sized `subreg:SI` writes
  //    into `(reg:DI 72)` mark `72` live on the forward pass without full-killing it on the
  //    backward pass, preserving `(REG_DEAD (reg:DI 72))` on the final copy.
  RtlControlFlowGraph subreg_cfg;
  subreg_cfg.pass = RtlPassKind::kSched;
  subreg_cfg.rpo_blocks.resize(1);
  subreg_cfg.rpo_blocks[0].id = 0;
  subreg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (subreg:SI (reg:DI 72) 0) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  subreg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (subreg:SI (reg:DI 72) 1) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
  subreg_cfg.rpo_blocks[0].AddPatternOnly(
      "(set (reg/i:DI 2 v0) (reg:DI 72)) (expr_list:REG_DEAD (reg:DI 72) (nil))");
  subreg_cfg.rpo_blocks[0].AddPatternOnly("(use (reg/i:DI 2 v0))");

  const RtlControlFlowGraph lifted_subreg = InvertPass06Sched1PreAlloc(subreg_cfg, o2_prof);
  ASSERT_EQ(lifted_subreg.BlockCount(), 1u);
  ASSERT_EQ(lifted_subreg.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_NE(lifted_subreg.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:DI 72)"),
            std::string::npos)
      << lifted_subreg.rpo_blocks[0].instructions[2];

  verify_ctx->BeginFunction("subreg_replay");
  verify_hooks->ApplyCompilationProfile(o2_prof);
  verify_ctx->MaterializeFromCfg(lifted_subreg);
  verify_hooks->RunScheduleInsns();
  const RtlControlFlowGraph replayed_subreg =
      verify_ctx->DematerializeToCfg(verify_ctx->GetInsns(), RtlPassKind::kSched);
  ASSERT_EQ(replayed_subreg.BlockCount(), 1u);
  EXPECT_NE(replayed_subreg.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:DI 72)"),
            std::string::npos)
      << replayed_subreg.rpo_blocks[0].instructions[2];
}

TEST_F(Pass06Sched1PreAllocTest, PreservesExactReplayAcrossSplitLargeIntAndPendingFlushChain) {
  const CompilationProfile o2_prof{.optimize_level = 2, .c_flags = {"-O2", "-mips1"}};

  // Construct a pre-`sched1` block containing a split-eligible `large_int`
  // (`0x55aa1234` = `1437204480 | 4660`) alongside a 35-load `mem/u` chain and
  // multiple independent stores. Forward `sched1` splits the `large_int` into
  // non-adjacent `lui` / `ori` and hoists `mem/u` loads ahead of stores.
  // `InvertPass06Sched1PreAlloc` must produce a `kCombine` preimage that
  // replays bit-for-bit under real GCC `schedule_insns` (`sched.c:4825-4848`).
  RtlControlFlowGraph pre;
  pre.pass = RtlPassKind::kCombine;
  pre.rpo_blocks.resize(1);
  pre.rpo_blocks[0].id = 0;
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg/v:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 73) (plus:SI (reg/v:SI 72) (const_int 1)))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (mem/v:SI (symbol_ref:SI (\"g_p06_s1\"))) (reg:SI 73)) "
      "(expr_list:REG_DEAD (reg:SI 73) (nil))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (reg:SI 74) (plus:SI (reg/v:SI 72) (const_int 2))) "
      "(expr_list:REG_DEAD (reg/v:SI 72) (nil))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (mem/v:SI (symbol_ref:SI (\"g_p06_s2\"))) (reg:SI 74)) "
      "(expr_list:REG_DEAD (reg:SI 74) (nil))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 80) (mem/u:SI (symbol_ref:SI (\"g_p06_arr\"))))");
  for (int k = 1; k < 35; ++k) {
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 81) (mem/u:SI (const:SI (plus:SI (symbol_ref:SI "
        "(\"g_p06_arr\")) (const_int " +
        std::to_string(k * 4) + ")))))");
    pre.rpo_blocks[0].AddPatternOnly(
        "(set (reg:SI 80) (plus:SI (reg:SI 80) (reg:SI 81))) "
        "(expr_list:REG_DEAD (reg:SI 81) (nil))");
  }
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (symbol_ref:SI (\"g_p06_sink\"))) (reg:SI 80)) "
      "(expr_list:REG_DEAD (reg:SI 80) (nil))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg:SI 85) (const_int 1437209140))");
  pre.rpo_blocks[0].AddPatternOnly(
      "(set (mem:SI (symbol_ref:SI (\"g_p06_c_sink\"))) (reg:SI 85)) "
      "(expr_list:REG_DEAD (reg:SI 85) (nil))");
  pre.rpo_blocks[0].AddPatternOnly("(set (reg/i:SI 2 v0) (const_int 0))");
  pre.rpo_blocks[0].AddPatternOnly("(use (reg/i:SI 2 v0))");

  verify_ctx->BeginFunction("p06_split_large_int_flush_fwd");
  verify_hooks->ApplyCompilationProfile(o2_prof);
  regclass_init();
  verify_ctx->MaterializeFromCfg(pre);
  verify_hooks->RunScheduleInsns();
  const RtlControlFlowGraph sched1_out =
      verify_ctx->DematerializeToCfg(verify_ctx->GetInsns(), RtlPassKind::kSched);
  ASSERT_EQ(sched1_out.BlockCount(), 1u);

  verify_ctx->BeginFunction("p06_split_large_int_flush_verify");
  verify_hooks->ApplyCompilationProfile(o2_prof);
  const RightInverseResult res = verifier->VerifySched1StageRightInverse(sched1_out);
  EXPECT_TRUE(res.matches) << res.diff_detail;
}

class Pass06Sched1PreAllocRoundTripTest : public Pass06Sched1PreAllocTest,
                                          public ::testing::WithParamInterface<RoundTripTestCase> {
};

TEST_P(Pass06Sched1PreAllocRoundTripTest, Stage06AndCumulativeSuffixAcrossSharedCorpus) {
  const RoundTripTestCase& tc = GetParam();
  trace_ctx->BeginFunction("harness_init");
  trace_hooks->ApplyCompilationProfile({2, tc.options.c_flags, {}});
  regclass_init();
  const FunctionCompilationTrace trace = trace_hooks->TraceForwardCompilation(tc);
  ASSERT_FALSE(trace.stage0_input.machine_words.empty()) << tc.test_name;
  ASSERT_TRUE(trace.pass_snapshots.contains(RtlPassKind::kSched)) << tc.test_name;

  // 1. Standalone Stage 6 right-inverse (`VerifySched1StageRightInverse` and
  //    `VerifySubfunctionBoundaryRightInverse` on `SubfunctionId::kScheduleBlockSched1`).
  const RtlControlFlowGraph& sched_snap = trace.pass_snapshots.at(RtlPassKind::kSched);
  const RtlControlFlowGraph stitched_combine =
      InvertPass06Sched1PreAlloc(sched_snap, trace.stage0_input.profile);
  ASSERT_FALSE(stitched_combine.rpo_blocks.empty()) << "Empty Pass06 output for " << tc.test_name;
  EXPECT_EQ(stitched_combine.pass, RtlPassKind::kCombine);

  verify_ctx->BeginFunction("harness_func");
  verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  const RightInverseResult stage_res = verifier->VerifySched1StageRightInverse(sched_snap);
  EXPECT_TRUE(stage_res.matches) << "Stage 6 standalone mismatch for " << tc.test_name << ":\n"
                                 << stage_res.diff_detail;

  bool found_sched1_boundary = false;
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id == SubfunctionId::kScheduleBlockSched1) {
      found_sched1_boundary = true;
      verify_ctx->BeginFunction("harness_boundary");
      const RightInverseResult b_res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
      EXPECT_TRUE(b_res.matches) << "kScheduleBlockSched1 boundary mismatch for " << tc.test_name
                                 << ":\n"
                                 << b_res.diff_detail;
    }
  }
  EXPECT_TRUE(found_sched1_boundary) << tc.test_name;

  // 2. Fresh Stage 0 -> 6 stage right-inverse on fresh Stage 5 output.
  const RtlControlFlowGraph p0 = InvertFinalAndGasFunction(trace.stage0_input);
  ASSERT_FALSE(p0.rpo_blocks.empty()) << "Empty Pass00 output for " << tc.test_name;
  const RtlControlFlowGraph p1 = InvertPass01DbrSchedule(p0, trace.stage0_input.profile);
  ASSERT_FALSE(p1.rpo_blocks.empty()) << "Empty Pass01 output for " << tc.test_name;
  const RtlControlFlowGraph p2 = InvertPass02Jump2CrossJump(p1, trace.stage0_input.profile);
  ASSERT_FALSE(p2.rpo_blocks.empty()) << "Empty Pass02 output for " << tc.test_name;
  const RtlControlFlowGraph p3 = InvertPass03Sched2PostReload(p2, trace.stage0_input.profile);
  ASSERT_FALSE(p3.rpo_blocks.empty()) << "Empty Pass03 output for " << tc.test_name;
  const RtlControlFlowGraph p4 = InvertPass04GregReload(p3, trace.stage0_input.profile);
  ASSERT_FALSE(p4.rpo_blocks.empty()) << "Empty Pass04 output for " << tc.test_name;
  const RtlControlFlowGraph p5 = InvertPass05LregLocalAlloc(p4, trace.stage0_input.profile);
  ASSERT_FALSE(p5.rpo_blocks.empty()) << "Empty Pass05 output for " << tc.test_name;
  const RtlControlFlowGraph p6 = InvertPass06Sched1PreAlloc(p5, trace.stage0_input.profile);
  ASSERT_FALSE(p6.rpo_blocks.empty()) << "Empty Pass06 fresh output for " << tc.test_name;
  EXPECT_EQ(p6.pass, RtlPassKind::kCombine);

  verify_ctx->BeginFunction("harness_fresh_stage6");
  verify_hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  const RightInverseResult fresh_stage_res = verifier->VerifySched1StageRightInverse(p5);
  EXPECT_TRUE(fresh_stage_res.matches)
      << "Fresh Stage 6 right-inverse mismatch for " << tc.test_name << ":\n"
      << fresh_stage_res.diff_detail << "\nfresh_sched(p5):\n"
      << p5.FormatSummary() << "\nactual:\n"
      << fresh_stage_res.actual_summary << "\nfresh_combine(p6):\n"
      << p6.FormatSummary();

  // 3. Cumulative fresh-GccContext `VerifySuffixRightInverse(RtlPassKind::kCombine, ...)` on
  //    BOTH relocatable `.o` AND linked-ROM inputs.
  const RightInverseResult suffix_res =
      verifier->VerifySuffixRightInverse(RtlPassKind::kCombine, trace.stage0_input);
  EXPECT_TRUE(suffix_res.matches) << "Stage 0..6 relocatable suffix mismatch for " << tc.test_name
                                  << ":\n"
                                  << suffix_res.diff_detail;

  const Stage0FunctionInput linked_stage0 = BakeLinkedRomStage0Input(trace.stage0_input);
  const RightInverseResult linked_suffix_res =
      verifier->VerifySuffixRightInverse(RtlPassKind::kCombine, linked_stage0);
  EXPECT_TRUE(linked_suffix_res.matches)
      << "Stage 0..6 linked-ROM suffix mismatch for " << tc.test_name << ":\n"
      << linked_suffix_res.diff_detail;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, Pass06Sched1PreAllocRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom::lifter
