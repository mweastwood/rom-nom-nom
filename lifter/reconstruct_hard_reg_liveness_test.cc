#include "lifter/reconstruct_hard_reg_liveness.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
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
using ::rom_nom_nom::gcc_harness::SubfunctionId;
using ::rom_nom_nom::gcc_harness::SubfunctionInverseProvider;

class ProductionHardRegLivenessInverseProvider final : public SubfunctionInverseProvider {
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
    return lifter::InvertDoCrossJump(jump2_cfg_x);
  }
  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x,
                                        const CompilationProfile& profile) const override {
    return lifter::InvertDoCrossJump(jump2_cfg_x, profile);
  }
};

class CorruptedLiveAtStartEagerProvider final : public SubfunctionInverseProvider {
 public:
  RtlControlFlowGraph InvertFillEagerDelaySlots(
      const RtlControlFlowGraph& dbr_eager_cfg_x) const override {
    RtlControlFlowGraph out = lifter::InvertFillEagerDelaySlots(dbr_eager_cfg_x);
    // `reorg.c:find_basic_block` scans backwards to the start of the function (`b == 0`) when
    // querying fallthrough liveness at block 1; falsely marking `$3` (`v1`) live in
    // `basic_block_live_at_start[0]` makes `mark_target_live_regs` report `$3` live on the
    // untaken path so `fill_slots_from_thread` annuls (`jump_insn/u`) instead of `jump_insn`.
    out.sideband.basic_block_live_at_start[0].insert(3);
    return out;
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

CompilationProfile MakeProfile(int optimize_level, std::vector<std::string> c_flags) {
  CompilationProfile profile;
  profile.optimize_level = optimize_level;
  profile.c_flags = std::move(c_flags);
  return profile;
}

bool HasRegNoteOnReg(const RtlPatternAndNotes& pn, RtlMachineMode kind, int regno) {
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind == kind && note.datum.IsReg() && note.datum.RegNo() == regno) {
      return true;
    }
  }
  return false;
}

bool TopLevelInsnHasNote(std::string_view insn_str, RtlMachineMode kind, int regno) {
  auto parsed = ParsePatternAndNotes(insn_str);
  if (!parsed.ok()) {
    return false;
  }
  return HasRegNoteOnReg(*parsed, kind, regno);
}

bool SequenceSubInsnHasNote(std::string_view seq_str, size_t sub_idx, RtlMachineMode kind,
                            int regno) {
  auto parsed = ParsePatternAndNotes(seq_str);
  if (!parsed.ok() || parsed->pattern.op != RtlOp::kSequence ||
      sub_idx >= parsed->pattern.children.size()) {
    return false;
  }
  auto sub_pn = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[sub_idx]);
  if (!sub_pn.ok()) {
    return false;
  }
  return HasRegNoteOnReg(*sub_pn, kind, regno);
}

size_t SequenceSubInsnConstZeroDeadNoteCount(std::string_view seq_str, size_t sub_idx) {
  auto parsed = ParsePatternAndNotes(seq_str);
  if (!parsed.ok() || parsed->pattern.op != RtlOp::kSequence ||
      sub_idx >= parsed->pattern.children.size()) {
    return 0;
  }
  auto sub_pn = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[sub_idx]);
  if (!sub_pn.ok()) {
    return 0;
  }
  size_t count = 0;
  for (const RtlRegNote& note : sub_pn->reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsConstInt() &&
        note.datum.int_val == 0) {
      ++count;
    }
  }
  return count;
}

bool ExprHasReturnValueSet(const RtlExpr& expr, int regno, RtlMachineMode mode) {
  bool found = false;
  expr.Visit([&](const RtlExpr& node) {
    if (node.IsSet() && node.SetDest().IsReg() && node.SetDest().RegNo() == regno &&
        node.SetDest().mode == mode && node.SetDest().flags.integrated) {
      found = true;
    }
  });
  return found;
}

bool InsnHasReturnValueSet(std::string_view insn_str, int regno, RtlMachineMode mode) {
  auto parsed = ParsePatternAndNotes(insn_str);
  if (!parsed.ok()) {
    return false;
  }
  return ExprHasReturnValueSet(parsed->pattern, regno, mode);
}

bool IsReturnValueUseInsn(std::string_view insn_str, int regno, RtlMachineMode mode) {
  auto parsed = ParsePatternAndNotes(insn_str);
  if (!parsed.ok() || parsed->pattern.op != RtlOp::kUse || parsed->pattern.children.size() != 1) {
    return false;
  }
  const RtlExpr& inner = parsed->pattern.children[0];
  return inner.IsReg() && inner.RegNo() == regno && inner.mode == mode && inner.flags.integrated;
}

bool ContainsAnyReturnValueRegOrUse(const RtlControlFlowGraph& cfg) {
  bool found = false;
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& insn_str : blk.instructions) {
      auto parsed = ParsePatternAndNotes(insn_str);
      if (!parsed.ok()) {
        continue;
      }
      parsed->pattern.Visit([&](const RtlExpr& node) {
        if (node.IsReg() && node.flags.integrated) {
          found = true;
        }
      });
    }
  }
  return found;
}

bool ContainsAnyDeadOrUnusedNote(const RtlControlFlowGraph& cfg) {
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    for (const std::string& insn_str : blk.instructions) {
      auto parsed = ParsePatternAndNotes(insn_str);
      if (!parsed.ok()) {
        continue;
      }
      for (const RtlRegNote& note : parsed->reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) {
          return true;
        }
      }
      if (parsed->pattern.op == RtlOp::kSequence) {
        for (const RtlExpr& sub : parsed->pattern.children) {
          auto sub_pn = RtlPatternAndNotes::FromSequenceSubInsn(sub);
          if (!sub_pn.ok()) {
            continue;
          }
          for (const RtlRegNote& note : sub_pn->reg_notes) {
            if (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) {
              return true;
            }
          }
        }
      }
    }
  }
  return false;
}

bool BlockLiveInContains(const RtlControlFlowGraph& cfg, int block_idx, int regno) {
  const auto it = cfg.sideband.basic_block_live_at_start.find(block_idx);
  return it != cfg.sideband.basic_block_live_at_start.end() && it->second.contains(regno);
}

TEST(ReconstructHardRegLivenessTest, VoidFunctionScratchV0AndClobberedCallNotMarkedReturn) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // Null and empty CFGs are safe no-ops.
  ReconstructPostDbrHardRegLivenessAndReturnUse(nullptr, profile_o2);
  RtlControlFlowGraph empty_cfg;
  ReconstructPostDbrHardRegLivenessAndReturnUse(&empty_cfg, profile_o2);
  EXPECT_TRUE(empty_cfg.rpo_blocks.empty());

  // Case A: Void function computes temporary address in `$2` (`v0`) and reads it in a memory
  // store before `return_internal`. Because `$2` is read after its definition, it is scratch,
  // not a return value.
  RtlControlFlowGraph scratch_cfg;
  scratch_cfg.pass = RtlPassKind::kDbr;
  scratch_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 8)))",
                    "(set (mem:SI (reg:SI 2 v0)) (reg:SI 5 a1))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&scratch_cfg, profile_o2);
  ASSERT_EQ(scratch_cfg.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_FALSE(ContainsAnyReturnValueRegOrUse(scratch_cfg));
  EXPECT_TRUE(
      TopLevelInsnHasNote(scratch_cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 2));
  EXPECT_TRUE(
      TopLevelInsnHasNote(scratch_cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 5));
  EXPECT_EQ(scratch_cfg.sideband.basic_block_live_at_start.size(), 1u);
  EXPECT_FALSE(BlockLiveInContains(scratch_cfg, 0, 2));
  EXPECT_TRUE(BlockLiveInContains(scratch_cfg, 0, 4));
  EXPECT_TRUE(BlockLiveInContains(scratch_cfg, 0, 5));

  // Case B: `$2` is defined on only one arm of a diamond before `return_internal` (not
  // guaranteed initialized on all paths from entry), so it cannot be a scalar return value.
  RtlControlFlowGraph partial_def_cfg;
  partial_def_cfg.pass = RtlPassKind::kDbr;
  partial_def_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 2 v0) (const_int 1))",
                    "(set (mem:SI (reg:SI 5 a1)) (const_int 0))",
                },
                {2}),
      MakeBlock(2, true,
                {
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&partial_def_cfg, profile_o2);
  EXPECT_FALSE(ContainsAnyReturnValueRegOrUse(partial_def_cfg));
  EXPECT_EQ(partial_def_cfg.rpo_blocks[2].instructions.size(), 1u);
  EXPECT_TRUE(TopLevelInsnHasNote(partial_def_cfg.rpo_blocks[1].instructions[0],
                                  RtlMachineMode::kRegUnused, 2));

  // Case C: Trailing void `(call (mem:SI ...) ...)` clobbers `$2` before epilogue, so an earlier
  // `(set (reg:SI 2 v0) ...)` does not reach exit.
  RtlControlFlowGraph trailing_void_call_cfg;
  trailing_void_call_cfg.pass = RtlPassKind::kDbr;
  trailing_void_call_cfg.sideband.frame_size_bytes = 24;
  trailing_void_call_cfg.sideband.saved_gpr_regnos = {31};
  trailing_void_call_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (const_int 7))",
                    "(parallel[ (call (mem:SI (symbol_ref \"void_helper\")) (const_int 16)) "
                    "(clobber (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&trailing_void_call_cfg, profile_o2);
  EXPECT_FALSE(ContainsAnyReturnValueRegOrUse(trailing_void_call_cfg));
  EXPECT_EQ(trailing_void_call_cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_TRUE(TopLevelInsnHasNote(trailing_void_call_cfg.rpo_blocks[0].instructions[0],
                                  RtlMachineMode::kRegUnused, 2));
}

TEST(ReconstructHardRegLivenessTest, ScalarSiAndDiReturnTaggingAndUseInsertionMgp32VsMgp64) {
  const CompilationProfile mgp32_profile = MakeProfile(2, {"-O2", "-mips2"});
  const CompilationProfile mgp64_profile = MakeProfile(2, {"-O2", "-mips3", "-mgp64", "-mfp64"});

  // 1. SImode return: an overwritten early `$2` def stays plain `(reg:SI 2 v0)` with
  // `REG_UNUSED`, while the exit-reaching `$2` def is upgraded to `(reg/i:SI 2 v0)` and
  // `(use (reg/i:SI 2 v0))` is inserted right before `return_internal`.
  RtlControlFlowGraph si_cfg;
  si_cfg.pass = RtlPassKind::kDbr;
  si_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (const_int 99))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&si_cfg, mgp32_profile);
  ASSERT_EQ(si_cfg.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_FALSE(InsnHasReturnValueSet(si_cfg.rpo_blocks[0].instructions[0], 2, RtlMachineMode::kSi));
  EXPECT_TRUE(
      TopLevelInsnHasNote(si_cfg.rpo_blocks[0].instructions[0], RtlMachineMode::kRegUnused, 2));
  EXPECT_TRUE(InsnHasReturnValueSet(si_cfg.rpo_blocks[0].instructions[1], 2, RtlMachineMode::kSi));
  EXPECT_FALSE(
      TopLevelInsnHasNote(si_cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegUnused, 2));
  EXPECT_TRUE(IsReturnValueUseInsn(si_cfg.rpo_blocks[0].instructions[2], 2, RtlMachineMode::kSi));

  // Idempotence: running reconstruction again does not insert a second `(use (reg/i:SI 2 v0))`.
  ReconstructPostDbrHardRegLivenessAndReturnUse(&si_cfg, mgp32_profile);
  EXPECT_EQ(si_cfg.rpo_blocks[0].instructions.size(), 4u);

  // 2. DImode return under `-mgp32` (`-mips2`): `$2` and `$3` form the 64-bit return pair; both
  // exit-reaching `SET`s get `reg/i`, `(use (reg/i:DI 2 v0))` is inserted, and both `2` and `3`
  // are live at the start of the exit block.
  RtlControlFlowGraph di_mgp32_cfg;
  di_mgp32_cfg.pass = RtlPassKind::kDbr;
  di_mgp32_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (reg:SI 4 a0))",
                    "(set (reg:SI 3 v1) (reg:SI 5 a1))",
                },
                {1}),
      MakeBlock(1, true,
                {
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&di_mgp32_cfg, mgp32_profile);
  EXPECT_TRUE(
      InsnHasReturnValueSet(di_mgp32_cfg.rpo_blocks[0].instructions[0], 2, RtlMachineMode::kSi));
  EXPECT_TRUE(
      InsnHasReturnValueSet(di_mgp32_cfg.rpo_blocks[0].instructions[1], 3, RtlMachineMode::kSi));
  ASSERT_EQ(di_mgp32_cfg.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_TRUE(
      IsReturnValueUseInsn(di_mgp32_cfg.rpo_blocks[0].instructions[2], 2, RtlMachineMode::kDi));
  EXPECT_EQ(di_mgp32_cfg.rpo_blocks[1].instructions.size(), 1u);
  EXPECT_TRUE(BlockLiveInContains(di_mgp32_cfg, 1, 2));
  EXPECT_TRUE(BlockLiveInContains(di_mgp32_cfg, 1, 3));

  // 3. DImode return under `-mgp64`: `(reg:DI 2 v0)` occupies a single 64-bit GPR (`$2` only,
  // `HardRegNregs == 1`), still emits `(use (reg/i:DI 2 v0))`, and does NOT mark `$3` live.
  RtlControlFlowGraph di_mgp64_cfg;
  di_mgp64_cfg.pass = RtlPassKind::kDbr;
  di_mgp64_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:DI 2 v0) (plus:DI (reg:DI 4 a0) (reg:DI 5 a1)))",
                },
                {1}),
      MakeBlock(1, true,
                {
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&di_mgp64_cfg, mgp64_profile);
  EXPECT_TRUE(
      InsnHasReturnValueSet(di_mgp64_cfg.rpo_blocks[0].instructions[0], 2, RtlMachineMode::kDi));
  ASSERT_EQ(di_mgp64_cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_TRUE(
      IsReturnValueUseInsn(di_mgp64_cfg.rpo_blocks[0].instructions[1], 2, RtlMachineMode::kDi));
  EXPECT_EQ(di_mgp64_cfg.rpo_blocks[1].instructions.size(), 1u);
  EXPECT_TRUE(BlockLiveInContains(di_mgp64_cfg, 1, 2));
  EXPECT_FALSE(BlockLiveInContains(di_mgp64_cfg, 1, 3));
}

TEST(ReconstructHardRegLivenessTest, FloatingPointSfAndDfReturnTaggingAndUseInsertionMfp32VsMfp64) {
  const CompilationProfile mfp32_profile = MakeProfile(2, {"-O2", "-mips2", "-mfp32"});
  const CompilationProfile mfp64_profile = MakeProfile(2, {"-O2", "-mips3", "-mgp64", "-mfp64"});

  // 1. SFmode return in `$f0` (`32`), even when `$2` (`v0`) was used earlier as address scratch.
  RtlControlFlowGraph sf_cfg;
  sf_cfg.pass = RtlPassKind::kDbr;
  sf_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 4)))",
                    "(set (reg:SF 32 $f0) (mem:SF (reg:SI 2 v0)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&sf_cfg, mfp32_profile);
  ASSERT_EQ(sf_cfg.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_FALSE(InsnHasReturnValueSet(sf_cfg.rpo_blocks[0].instructions[0], 2, RtlMachineMode::kSi));
  EXPECT_TRUE(InsnHasReturnValueSet(sf_cfg.rpo_blocks[0].instructions[1], 32, RtlMachineMode::kSf));
  EXPECT_TRUE(IsReturnValueUseInsn(sf_cfg.rpo_blocks[0].instructions[2], 32, RtlMachineMode::kSf));

  // 2. DFmode return under `-mfp32`: `(reg:DF 32 $f0)` spans `$f0` (`32`) and `$f1` (`33`).
  RtlControlFlowGraph df_mfp32_cfg;
  df_mfp32_cfg.pass = RtlPassKind::kDbr;
  df_mfp32_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:DF 32 $f0) (plus:DF (reg:DF 44 $f12) (reg:DF 46 $f14)))",
                },
                {1}),
      MakeBlock(1, true,
                {
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&df_mfp32_cfg, mfp32_profile);
  EXPECT_TRUE(
      InsnHasReturnValueSet(df_mfp32_cfg.rpo_blocks[0].instructions[0], 32, RtlMachineMode::kDf));
  ASSERT_EQ(df_mfp32_cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_TRUE(
      IsReturnValueUseInsn(df_mfp32_cfg.rpo_blocks[0].instructions[1], 32, RtlMachineMode::kDf));
  EXPECT_EQ(df_mfp32_cfg.rpo_blocks[1].instructions.size(), 1u);
  EXPECT_TRUE(BlockLiveInContains(df_mfp32_cfg, 1, 32));
  EXPECT_TRUE(BlockLiveInContains(df_mfp32_cfg, 1, 33));

  // 3. DFmode return under `-mfp64`: `(reg:DF 32 $f0)` occupies only register `32` (not `33`).
  RtlControlFlowGraph df_mfp64_cfg;
  df_mfp64_cfg.pass = RtlPassKind::kDbr;
  df_mfp64_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:DF 32 $f0) (plus:DF (reg:DF 44 $f12) (reg:DF 45 $f13)))",
                },
                {1}),
      MakeBlock(1, true,
                {
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&df_mfp64_cfg, mfp64_profile);
  EXPECT_TRUE(
      InsnHasReturnValueSet(df_mfp64_cfg.rpo_blocks[0].instructions[0], 32, RtlMachineMode::kDf));
  ASSERT_EQ(df_mfp64_cfg.rpo_blocks[0].instructions.size(), 2u);
  EXPECT_TRUE(
      IsReturnValueUseInsn(df_mfp64_cfg.rpo_blocks[0].instructions[1], 32, RtlMachineMode::kDf));
  EXPECT_EQ(df_mfp64_cfg.rpo_blocks[1].instructions.size(), 1u);
  EXPECT_TRUE(BlockLiveInContains(df_mfp64_cfg, 1, 32));
  EXPECT_FALSE(BlockLiveInContains(df_mfp64_cfg, 1, 33));
}

TEST(ReconstructHardRegLivenessTest,
     AnnulledVsNonAnnulledBranchSequenceCallSequenceAndEpilogueDelayLiveness) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // 1a. Annulled branch (`jump_insn/u` + `insn/s`): the delay slot executes ONLY along the taken
  // edge (block 2), NOT along the fallthrough edge (block 1). Writing `$7` (`a3`) in `insn/s`
  // therefore does NOT kill `$7`'s fallthrough liveness into block 1 (`$7` stays live at block 0
  // entry), while `$5` (`a1`) read in `insn/s` and dead on block 2 gets `REG_DEAD` inside `insn/s`.
  RtlControlFlowGraph annulled_cfg;
  annulled_cfg.pass = RtlPassKind::kDbr;
  annulled_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(sequence [(jump_insn/u (set (pc) (if_then_else "
                    "(ne:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))) "
                    "(insn/s (set (reg:SI 7 a3) (reg:SI 5 a1)))])",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 2 v0) (reg:SI 7 a3))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 7 a3) (const_int 1)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&annulled_cfg, profile_o2);
  EXPECT_TRUE(BlockLiveInContains(annulled_cfg, 0, 7));
  EXPECT_TRUE(BlockLiveInContains(annulled_cfg, 0, 5));
  EXPECT_EQ(SequenceSubInsnConstZeroDeadNoteCount(annulled_cfg.rpo_blocks[0].instructions[0], 1),
            1u);
  EXPECT_FALSE(SequenceSubInsnHasNote(annulled_cfg.rpo_blocks[0].instructions[0], 1,
                                      RtlMachineMode::kRegDead, 5));

  // 1b. Contrast with a non-annulled branch (`jump_insn` + `insn`): the delay slot executes on
  // BOTH edges, killing `$7` before both block 1 and block 2 so `$7` is NOT live at block 0 entry!
  RtlControlFlowGraph non_annulled_cfg;
  non_annulled_cfg.pass = RtlPassKind::kDbr;
  non_annulled_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(sequence [(jump_insn (set (pc) (if_then_else "
                    "(ne:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))) "
                    "(insn (set (reg:SI 7 a3) (reg:SI 5 a1)))])",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 2 v0) (reg:SI 7 a3))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 7 a3) (const_int 1)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&non_annulled_cfg, profile_o2);
  EXPECT_FALSE(BlockLiveInContains(non_annulled_cfg, 0, 7));
  EXPECT_TRUE(BlockLiveInContains(non_annulled_cfg, 0, 5));

  // 2. `CALL_INSN` `SEQUENCE` + `epilogue_delay_pattern`:
  //   - In `(sequence [(call_insn ...) (insn (set (reg:SI 4 a0) (reg:SI 6 a2)))])`, the delay
  //     slot runs BEFORE the call: `$6` (`a2`) dies in the delay slot (`REG_DEAD`), `$4` (`a0`)
  //     is consumed as a call argument (no `REG_UNUSED`), caller-saved `$8` (`t0`) defined before
  //     the call and unused afterward gets `REG_UNUSED`, and callee-saved `$16` (`s0`) stays live
  //     across the call into `epilogue_delay_pattern` (`(set (reg:SI 2 v0) (reg:SI 16 s0))`).
  //   - Because `$16` is read in `epilogue_delay_pattern`, an earlier read of `$16` in the body
  //     must NOT receive a false `REG_DEAD` note, and `epilogue_delay_pattern` is upgraded to
  //     `(set (reg/i:SI 2 v0) (reg:SI 16 s0))` with trailing `(use (reg/i:SI 2 v0))` appended.
  RtlControlFlowGraph call_and_epilogue_cfg;
  call_and_epilogue_cfg.pass = RtlPassKind::kDbr;
  call_and_epilogue_cfg.sideband.frame_size_bytes = 24;
  call_and_epilogue_cfg.sideband.saved_gpr_regnos = {16, 31};
  call_and_epilogue_cfg.sideband.epilogue_delay_pattern = "(set (reg:SI 2 v0) (reg:SI 16 s0))";
  call_and_epilogue_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 16 s0) (reg:SI 5 a1))",
                    "(set (reg:SI 8 t0) (const_int 42))",
                    "(sequence [(call_insn (parallel[ (set (reg:SI 2 v0) "
                    "(call (mem:SI (symbol_ref \"callee\")) (const_int 16))) "
                    "(clobber (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg:SI 4 a0) (reg:SI 6 a2)))])",
                    "(set (mem:SI (reg:SI 16 s0)) (reg:SI 2 v0))",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&call_and_epilogue_cfg, profile_o2);
  ASSERT_EQ(call_and_epilogue_cfg.rpo_blocks[0].instructions.size(), 5u);
  EXPECT_TRUE(TopLevelInsnHasNote(call_and_epilogue_cfg.rpo_blocks[0].instructions[1],
                                  RtlMachineMode::kRegUnused, 8));
  EXPECT_EQ(
      SequenceSubInsnConstZeroDeadNoteCount(call_and_epilogue_cfg.rpo_blocks[0].instructions[2], 1),
      1u);
  EXPECT_FALSE(SequenceSubInsnHasNote(call_and_epilogue_cfg.rpo_blocks[0].instructions[2], 1,
                                      RtlMachineMode::kRegUnused, 4));
  EXPECT_FALSE(TopLevelInsnHasNote(call_and_epilogue_cfg.rpo_blocks[0].instructions[3],
                                   RtlMachineMode::kRegDead, 16));
  EXPECT_TRUE(TopLevelInsnHasNote(call_and_epilogue_cfg.rpo_blocks[0].instructions[3],
                                  RtlMachineMode::kRegDead, 2));
  EXPECT_TRUE(InsnHasReturnValueSet(*call_and_epilogue_cfg.sideband.epilogue_delay_pattern, 2,
                                    RtlMachineMode::kSi));
  EXPECT_TRUE(IsReturnValueUseInsn(call_and_epilogue_cfg.rpo_blocks[0].instructions[4], 2,
                                   RtlMachineMode::kSi));
}

TEST(ReconstructHardRegLivenessTest,
     OptimizeZeroSuppressesNotesAndLiveAtStartVsOptimizeTwoSynthesizesThem) {
  RtlControlFlowGraph base_cfg;
  base_cfg.pass = RtlPassKind::kDbr;
  base_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 8 t0) (const_int 123))",
                    "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1))))])",
                },
                {}),
  };

  // At `-O0`, `flow_analysis` never runs in `cc1`, so `basic_block_live_at_start` stays empty and
  // zero `REG_DEAD` / `REG_UNUSED` notes are synthesized, while `reg/i` and `(use (reg/i:...))`
  // ARE reconstructed.
  RtlControlFlowGraph o0_cfg = base_cfg;
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o0_cfg, MakeProfile(0, {"-O0", "-mips2"}));
  EXPECT_TRUE(o0_cfg.sideband.basic_block_live_at_start.empty());
  EXPECT_FALSE(ContainsAnyDeadOrUnusedNote(o0_cfg));
  ASSERT_EQ(o0_cfg.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_TRUE(IsReturnValueUseInsn(o0_cfg.rpo_blocks[0].instructions[1], 2, RtlMachineMode::kSi));
  EXPECT_TRUE(InsnHasReturnValueSet(o0_cfg.rpo_blocks[0].instructions[2], 2, RtlMachineMode::kSi));

  // At `-O2`, the exact same CFG receives `basic_block_live_at_start` and both top-level
  // `REG_UNUSED` and `SEQUENCE`-inner `REG_DEAD` notes in addition to `reg/i` + `(use ...)`.
  RtlControlFlowGraph o2_cfg = base_cfg;
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o2_cfg, MakeProfile(2, {"-O2", "-mips2"}));
  EXPECT_TRUE(BlockLiveInContains(o2_cfg, 0, 4));
  EXPECT_TRUE(BlockLiveInContains(o2_cfg, 0, 5));
  ASSERT_EQ(o2_cfg.rpo_blocks[0].instructions.size(), 3u);
  EXPECT_TRUE(
      TopLevelInsnHasNote(o2_cfg.rpo_blocks[0].instructions[0], RtlMachineMode::kRegUnused, 8));
  EXPECT_TRUE(IsReturnValueUseInsn(o2_cfg.rpo_blocks[0].instructions[1], 2, RtlMachineMode::kSi));
  EXPECT_TRUE(InsnHasReturnValueSet(o2_cfg.rpo_blocks[0].instructions[2], 2, RtlMachineMode::kSi));
  EXPECT_EQ(SequenceSubInsnConstZeroDeadNoteCount(o2_cfg.rpo_blocks[0].instructions[2], 1), 2u);
  // Idempotence on `SEQUENCE` `const0_rtx` `REG_DEAD` notes:
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o2_cfg, MakeProfile(2, {"-O2", "-mips2"}));
  EXPECT_EQ(SequenceSubInsnConstZeroDeadNoteCount(o2_cfg.rpo_blocks[0].instructions[2], 1), 2u);
}

TEST(ReconstructHardRegLivenessTest,
     RecomputePostReloadLiveAtStartUpdatesPostReloadAndSkipsO0AndPreReload) {
  // 1. Positive post-reload CFG (e.g., after `InvertDoCrossJump` splits tail blocks and clears
  // stale `basic_block_live_at_start` entries): `RecomputePostReloadLiveAtStart` recomputes exact
  // per-block live-in sets without touching instruction strings or `REG_NOTES`.
  RtlControlFlowGraph post_reload_cfg;
  post_reload_cfg.pass = RtlPassKind::kSched2;
  // GCC 2.7.2 retains `reg_renumber` across `.greg`, `.sched2`, `.jump2`, and `.dbr`;
  // `RecomputePostReloadLiveAtStart` must still recompute `basic_block_live_at_start`.
  post_reload_cfg.sideband.reg_renumber = {{72, 2}};
  post_reload_cfg.sideband.basic_block_live_at_start = {{0, {4}}, {1, {}}, {2, {}}, {3, {}}};
  post_reload_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 6 a2) (const_int 2)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  const auto orig_b1_insns = post_reload_cfg.rpo_blocks[1].instructions;
  RecomputePostReloadLiveAtStart(&post_reload_cfg);
  ASSERT_EQ(post_reload_cfg.sideband.basic_block_live_at_start.size(), 3u);
  EXPECT_TRUE(BlockLiveInContains(post_reload_cfg, 0, 4));
  EXPECT_TRUE(BlockLiveInContains(post_reload_cfg, 0, 5));
  EXPECT_TRUE(BlockLiveInContains(post_reload_cfg, 0, 6));
  EXPECT_TRUE(BlockLiveInContains(post_reload_cfg, 1, 5));
  EXPECT_FALSE(BlockLiveInContains(post_reload_cfg, 1, 6));
  EXPECT_TRUE(BlockLiveInContains(post_reload_cfg, 2, 6));
  EXPECT_FALSE(BlockLiveInContains(post_reload_cfg, 2, 5));
  EXPECT_EQ(post_reload_cfg.rpo_blocks[1].instructions, orig_b1_insns);

  // 2. Negative `-O0` profile: `RecomputePostReloadLiveAtStart` clears
  // `basic_block_live_at_start` based strictly on `profile.optimize_level == 0`.
  RtlControlFlowGraph o0_cfg;
  o0_cfg.pass = RtlPassKind::kSched2;
  o0_cfg.sideband.basic_block_live_at_start = {{0, {4}}};
  o0_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                    "(use (reg/i:SI 2 v0))",
                },
                {}),
  };
  RecomputePostReloadLiveAtStart(&o0_cfg, MakeProfile(0, {"-O0"}));
  EXPECT_TRUE(o0_cfg.sideband.basic_block_live_at_start.empty());

  // 3a. Negative pre-reload CFG with pseudo-register (`regno >= 68`) in pattern: sentinel
  // `basic_block_live_at_start` must remain completely untouched.
  RtlControlFlowGraph pseudo_cfg;
  pseudo_cfg.pass = RtlPassKind::kSched;
  const absl::flat_hash_map<int, absl::flat_hash_set<int>> sentinel_live = {{0, {4, 72}}};
  pseudo_cfg.sideband.basic_block_live_at_start = sentinel_live;
  pseudo_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 72) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                    "(set (reg/i:SI 2 v0) (reg:SI 72))",
                    "(use (reg/i:SI 2 v0))",
                },
                {}),
  };
  RecomputePostReloadLiveAtStart(&pseudo_cfg);
  EXPECT_EQ(pseudo_cfg.sideband.basic_block_live_at_start, sentinel_live);

  // 3b. Negative pre-reload CFG with non-empty `reg_renumber` (e.g., `.lreg` / `.greg` sideband):
  // `basic_block_live_at_start` must also remain untouched.
  RtlControlFlowGraph reg_renumber_cfg;
  reg_renumber_cfg.pass = RtlPassKind::kGreg;
  const absl::flat_hash_map<int, absl::flat_hash_set<int>> sentinel_greg_live = {{0, {99}}};
  reg_renumber_cfg.sideband.basic_block_live_at_start = sentinel_greg_live;
  reg_renumber_cfg.sideband.reg_renumber = {{72, 2}};
  reg_renumber_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg/i:SI 2 v0) (reg:SI 4 a0))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  RecomputePostReloadLiveAtStart(&reg_renumber_cfg);
  EXPECT_EQ(reg_renumber_cfg.sideband.basic_block_live_at_start, sentinel_greg_live);
}

TEST(ReconstructHardRegLivenessTest, Stage0Stage1AndStage2RightInverseVerification) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionHardRegLivenessInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);

  // 1. Multi-block scheduled Stage 0 -> Stage 2 suffix replay (`VerifyFinalAndGasStageRightInverse`
  // + `VerifySuffixRightInverse` up to `kSched2`) exercising
  // `ReconstructPostDbrHardRegLivenessAndReturnUse` on branch delay slots and return delay slots.
  Stage0FunctionInput input;
  input.profile = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});
  input.machine_words = {
      0x10800003u,  // beq $a0, $zero, $L2
      0x00851021u,  // addu $v0, $a0, $a1 (branch delay slot)
      0x03e00008u,  // jr $ra
      0x24420001u,  // addiu $v0, $v0, 1 (return delay slot)
      0x03e00008u,  // $L2: jr $ra
      0x00851023u,  // subu $v0, $a0, $a1 (return delay slot)
  };
  const RightInverseResult stage0_res = verifier.VerifyFinalAndGasStageRightInverse(input);
  EXPECT_TRUE(stage0_res.matches) << stage0_res.diff_detail;

  const RightInverseResult suffix_res =
      verifier.VerifySuffixRightInverse(RtlPassKind::kSched2, input);
  EXPECT_TRUE(suffix_res.matches) << suffix_res.diff_detail;

  // 2. Stage 1 (`dbr_schedule`) forward replay (`VerifyDbrStageRightInverse`) where
  // `RecomputePostReloadLiveAtStart` computes exact per-block live-in sets so `reorg.c` knows `$3`
  // (`v1`) is dead on the fallthrough edge of block 0 and steals `(set (reg:SI 3 v1) ...)` into a
  // non-annulled delay slot; corrupting `basic_block_live_at_start[1]` to mark `$3` live causes
  // `reorg.c` to annul or skip the slot and fail right-inverse verification.
  RtlControlFlowGraph dummy;
  hooks.ExecuteSubfunctionBoundary(SubfunctionId::kFillSimpleDelaySlots, dummy, {}, 2,
                                   {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});

  RtlControlFlowGraph dbr_cfg;
  dbr_cfg.pass = RtlPassKind::kDbr;
  dbr_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 3 v1) (const_int 7)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (set (pc) (label_ref L1))) "
                 "(insn (set (mem:SI (reg:SI 6 a2)) (const_int 0)))])"},
                {3}),
      MakeBlock(2, true, {"(set (mem:SI (reg:SI 6 a2)) (reg:SI 3 v1))"}, {3}),
      MakeBlock(3, true,
                {"(use (reg/i:SI 2 v0))",
                 "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                 "(insn (set (reg/i:SI 2 v0) (reg:SI 4 a0)))])"},
                {}),
  };

  const RightInverseResult stage1_res = verifier.VerifyDbrStageRightInverse(dbr_cfg);
  EXPECT_TRUE(stage1_res.matches) << stage1_res.diff_detail;

  RtlControlFlowGraph eager_target_cfg;
  eager_target_cfg.pass = RtlPassKind::kDbr;
  eager_target_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
                 "(const_int 0)) (label_ref L1) (pc)))) "
                 "(insn/s (set (reg:SI 3 v1) (const_int 9)))])"},
                {3, 1}),
      MakeBlock(1, false,
                {"(set (mem:SI (reg:SI 6 a2)) (const_int 0))",
                 "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 5 a1) "
                 "(const_int 0)) (label_ref L0) (pc)))) "
                 "(insn (set (reg/i:SI 2 v0) (reg:SI 4 a0)))])"},
                {2, 2}),
      MakeBlock(2, true,
                {
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
      MakeBlock(3, true,
                {
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  const RightInverseResult eager_res =
      verifier.VerifyFillEagerDelaySlotsRightInverse(eager_target_cfg);
  EXPECT_TRUE(eager_res.matches) << eager_res.diff_detail;

  CorruptedLiveAtStartEagerProvider bad_provider;
  RightInverseVerifier bad_verifier(&ctx, &hooks, &bad_provider);
  const RightInverseResult bad_res =
      bad_verifier.VerifyFillEagerDelaySlotsRightInverse(eager_target_cfg);
  EXPECT_FALSE(bad_res.matches)
      << "lifted:\n"
      << bad_provider.InvertFillEagerDelaySlots(eager_target_cfg).FormatSummary() << "\nactual:\n"
      << bad_res.actual_summary;

  // 3. Stage 2 (`do_cross_jump`) forward replay (`VerifyDoCrossJumpRightInverse`) on a post-reload
  // cross-jumped CFG where un-merging the shared tail recomputes `basic_block_live_at_start`.
  hooks.ExecuteSubfunctionBoundary(SubfunctionId::kDoCrossJump, dummy, {}, 2,
                                   {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});

  RtlControlFlowGraph jump2_cfg;
  jump2_cfg.pass = RtlPassKind::kJump2;
  jump2_cfg.sideband.basic_block_live_at_start = {
      {0, {4, 5}},
      {1, {5}},
      {2, {5}},
      {3, {3, 5}},
  };
  jump2_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 6 a2) (const_int 1))",
                    "(set (reg:SI 3 v1) (const_int 10))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 6 a2) (const_int 2))",
                    "(set (reg:SI 3 v1) (const_int 20))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                    "(set (reg/i:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const RightInverseResult cross_jump_res = verifier.VerifyDoCrossJumpRightInverse(jump2_cfg);
  EXPECT_TRUE(cross_jump_res.matches) << cross_jump_res.diff_detail << "\nexpected:\n"
                                      << cross_jump_res.expected_summary << "\nactual:\n"
                                      << cross_jump_res.actual_summary;
}

TEST(ReconstructHardRegLivenessTest,
     UncondJumpInStructRecoveryDiCallReturnAndSharedLeafReturnUsePlacement) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  // 1a. `(I4)`: When `B0` ends with a mostly-false forward conditional `SEQUENCE` (`eq:SI` +
  // `insn/s`) jumping past `u + 1` (`b_target = 3 > u + 1`) and falling through to bare `SEQUENCE`
  // block `B1` (`u = 1`) whose delay slot defines `$2` (`v0`) (dead at `B3` before `B0`'s slot),
  // `(I4)` marks `B1`'s delay slot `insn/s` (`in_struct = true`).
  RtlControlFlowGraph i4_steal_cfg;
  i4_steal_cfg.pass = RtlPassKind::kDbr;
  i4_steal_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L1) (pc)))) (insn/s (set (reg:SI 6 a2) (const_int 1)))])"},
                {3, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (set (pc) (label_ref L2))) "
                 "(insn (set (reg:SI 2 v0) (const_int 0)))])"},
                {4}),
      MakeBlock(2, true, {"(set (reg:SI 6 a2) (const_int 2))"}, {3}),
      MakeBlock(3, true, {"(set (reg:SI 2 v0) (plus:SI (reg:SI 6 a2) (const_int 5)))"}, {4}),
      MakeBlock(4, true,
                {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&i4_steal_cfg, profile_o2);
  {
    const auto parsed = ParsePatternAndNotes(i4_steal_cfg.rpo_blocks[1].instructions[0]);
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed->pattern.op, RtlOp::kSequence);
    EXPECT_TRUE(parsed->pattern.children[1].flags.in_struct)
        << i4_steal_cfg.rpo_blocks[1].instructions[0];
  }

  // 1b. Contrast: when `B0`'s forward branch targets `B2` (`b_target = 2 == u + 1`) immediately
  // after bare `SEQUENCE` block `B1` (`u = 1`), `B0` could not have survived `jump_optimize`
  // uninverted around `B1`, so `(I4)` MUST keep `B1`'s `in_struct` `false`!
  RtlControlFlowGraph i4_adj_cfg;
  i4_adj_cfg.pass = RtlPassKind::kDbr;
  i4_adj_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int "
                 "0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 6 a2) (const_int 1)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(sequence [(jump_insn (set (pc) (label_ref L1))) "
                 "(insn (set (reg:SI 2 v0) (const_int 0)))])"},
                {3}),
      MakeBlock(2, true, {"(set (reg:SI 2 v0) (plus:SI (reg:SI 6 a2) (const_int 5)))"}, {3}),
      MakeBlock(3, true,
                {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))",
                 "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&i4_adj_cfg, profile_o2);
  {
    const auto parsed = ParsePatternAndNotes(i4_adj_cfg.rpo_blocks[1].instructions[0]);
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed->pattern.op, RtlOp::kSequence);
    EXPECT_FALSE(parsed->pattern.children[1].flags.in_struct)
        << i4_adj_cfg.rpo_blocks[1].instructions[0];
  }

  // 2. `DImode` call return `(set (reg:DI 2 v0) (call ...))`: defines both `$2` (`v0`) and `$3`
  // (`v1`), so a post-call `SImode` read of `$3` receives `REG_DEAD` on `$3` while `reg/i:DI 2 v0`
  // return upgrades `(use (reg/i:DI 2 v0))`.
  RtlControlFlowGraph di_call_cfg;
  di_call_cfg.pass = RtlPassKind::kDbr;
  di_call_cfg.sideband.saved_gpr_regnos = {16, 31};
  di_call_cfg.sideband.frame_size_bytes = 24;
  di_call_cfg.rpo_blocks = {
      MakeBlock(
          0, false,
          {"(parallel[ (set (reg:DI 2 v0) (call (mem:SI (symbol_ref \"__divdi3\")) "
           "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
           "(set (mem:SI (reg:SI 16 s0)) (reg:SI 3 v1))", "(set (reg:DI 2 v0) (const_int 9))"},
          {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&di_call_cfg, profile_o2);
  ASSERT_EQ(di_call_cfg.rpo_blocks[0].instructions.size(), 4u);
  EXPECT_TRUE(
      TopLevelInsnHasNote(di_call_cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 3));
  EXPECT_TRUE(
      IsReturnValueUseInsn(di_call_cfg.rpo_blocks[0].instructions[3], 2, RtlMachineMode::kDi));

  // 3. Shared leaf return block (`B2`, `has_label = true`) whose jumping predecessor (`B0`) is a
  // non-T1 annulled conditional `SEQUENCE` (`v0 = 0`, where `v0` is live into `B1`'s `v0 + t0` so
  // `reorg.c` annuls `B0` as `jump_insn/u`): `(use (reg/i:SI 2 v0))` MUST be placed inside `B2`
  // before `(parallel [(return) (use (reg:SI 31 ra))])` so `VerifyDbrStageRightInverse` replays
  // `reorg.c` cleanly.
  RtlControlFlowGraph shared_ret_cfg;
  shared_ret_cfg.pass = RtlPassKind::kDbr;
  shared_ret_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg:SI 2 v0) (reg:SI 6 a2))",
                 "(sequence [(jump_insn/u (set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (reg:SI 4 "
                 "a0)) (label_ref L0) (pc)))) (insn/s (set (reg:SI 2 v0) (const_int 0)))])"},
                {2, 1}),
      MakeBlock(1, false,
                {"(set (mem:SI (reg:SI 29 sp)) (const_int 0))",
                 "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))"},
                {2}),
      MakeBlock(2, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&shared_ret_cfg, profile_o2);
  ASSERT_EQ(shared_ret_cfg.rpo_blocks[2].instructions.size(), 2u);
  EXPECT_TRUE(
      IsReturnValueUseInsn(shared_ret_cfg.rpo_blocks[2].instructions[0], 2, RtlMachineMode::kSi));

  GccContext ctx;
  FunctionHooks hooks(&ctx);
  RtlControlFlowGraph dummy;
  hooks.ExecuteSubfunctionBoundary(SubfunctionId::kFillSimpleDelaySlots, dummy, {}, 2,
                                   {"-O2", "-mips2", "-mcpu=r4000"});
  ProductionHardRegLivenessInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const RightInverseResult dbr_res = verifier.VerifyDbrStageRightInverse(shared_ret_cfg);
  EXPECT_TRUE(dbr_res.matches) << dbr_res.diff_detail;
}

TEST(ReconstructHardRegLivenessTest,
     IsAnyDefinedHardRegLiveAtBlockEntryIgnoresSidebandAndHandlesMultiWord) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg:DI 2 v0) (const_int 0))"}, {1}),
      // `B1` overwrites `$2` (`v0`) in `SImode` before reading `$3` (`v1`), so `$2` is dead at
      // entry to `B1` while `$3` (the second word of `reg:DI 2`) is live at entry to `B1`.
      MakeBlock(1, false,
                {"(set (reg:SI 2 v0) (reg:SI 4 a0))",
                 "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1)))",
                 "(use (reg/i:SI 2 v0))", "(parallel[ (return) (use (reg:SI 31 ra)) ] )"},
                {}),
  };
  // Deliberately poison `basic_block_live_at_start[1]` to mark `$2` live and omit `$3`:
  // `IsAnyDefinedHardRegLiveAtBlockEntry` must compute liveness purely from CFG dataflow.
  cfg.sideband.basic_block_live_at_start[1] = {2};

  const auto si_v0 = ParsePatternAndNotes("(set (reg:SI 2 v0) (const_int -1))");
  const auto di_v0 = ParsePatternAndNotes("(set (reg:DI 2 v0) (const_int -1))");
  const auto sp_set =
      ParsePatternAndNotes("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 8)))");
  ASSERT_TRUE(si_v0.ok() && di_v0.ok() && sp_set.ok());

  EXPECT_FALSE(IsAnyDefinedHardRegLiveAtBlockEntry(cfg, si_v0->pattern, 1));
  EXPECT_TRUE(IsAnyDefinedHardRegLiveAtBlockEntry(cfg, di_v0->pattern, 1));
  EXPECT_TRUE(IsAnyDefinedHardRegLiveAtBlockEntry(cfg, sp_set->pattern, 1));
}

TEST(ReconstructHardRegLivenessTest,
     PopulatesExplicitCallFusageAcrossDiamondZeroArgAndSequenceCalls) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // 1. Multi-block diamond reaching a call (`$5` set in `B0`, `$4` set in both `B1` and `B2`),
  // followed by a 0-arg call in `B3` (where `$4` and `$5` were clobbered by the first call),
  // followed by a `SEQUENCE`-wrapped call whose second argument `$5` is filled in the delay slot.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband.frame_size_bytes = 24;
  cfg.sideband.saved_gpr_regnos = {31};
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (reg:SI 5 a1) (const_int 10))",
                 "(set (pc) (if_then_else (eq:SI (reg:SI 6 a2) (const_int 0)) (label_ref L0) "
                 "(pc)))"},
                {2, 1}),
      MakeBlock(1, false, {"(set (reg:SI 4 a0) (const_int 1))", "(set (pc) (label_ref L1))"}, {3}),
      MakeBlock(2, true, {"(set (reg:SI 4 a0) (const_int 2))"}, {3}),
      MakeBlock(3, true,
                {
                    "(parallel[ (call (mem:SI (symbol_ref \"two_arg_fn\")) (const_int 16)) "
                    "(clobber (reg:SI 31 ra)) ] )",
                    "(parallel[ (call (mem:SI (symbol_ref \"zero_arg_fn\")) (const_int 16)) "
                    "(clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SI 4 a0) (const_int 7))",
                    "(sequence [(call_insn (parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref "
                    "\"seq_two_arg_fn\")) (const_int 16))) (clobber (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg:SI 5 a1) (const_int 42)))])",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);

  ASSERT_GE(cfg.rpo_blocks[3].rtl_insns.size(), 4u);
  const RtlInsn& two_arg_call = cfg.rpo_blocks[3].rtl_insns[0];
  EXPECT_TRUE(two_arg_call.has_explicit_call_fusage);
  ASSERT_EQ(two_arg_call.call_fusage.size(), 2u);
  EXPECT_EQ(two_arg_call.call_fusage[0].ToGccPrintRtlString(), "(use (reg:SI 5 a1))");
  EXPECT_EQ(two_arg_call.call_fusage[1].ToGccPrintRtlString(), "(use (reg:SI 4 a0))");

  const RtlInsn& zero_arg_call = cfg.rpo_blocks[3].rtl_insns[1];
  EXPECT_TRUE(zero_arg_call.has_explicit_call_fusage);
  EXPECT_TRUE(zero_arg_call.call_fusage.empty());

  const RtlInsn& seq_call = cfg.rpo_blocks[3].rtl_insns[3];
  EXPECT_TRUE(seq_call.has_explicit_call_fusage);
  ASSERT_EQ(seq_call.call_fusage.size(), 2u);
  EXPECT_EQ(seq_call.call_fusage[0].ToGccPrintRtlString(), "(use (reg:SI 5 a1))");
  EXPECT_EQ(seq_call.call_fusage[1].ToGccPrintRtlString(), "(use (reg:SI 4 a0))");

  // 2. Pre-populated `has_explicit_call_fusage = true` with empty `call_fusage` (e.g. a 0-arg
  // call preceded by a scratch write to `$4` that was used as a memory base) must be preserved
  // verbatim without synthesizing a bogus `(use (reg:SI 4 a0))`.
  RtlControlFlowGraph explicit_zero_arg_cfg;
  explicit_zero_arg_cfg.pass = RtlPassKind::kDbr;
  explicit_zero_arg_cfg.sideband.frame_size_bytes = 24;
  explicit_zero_arg_cfg.sideband.saved_gpr_regnos = {31};
  explicit_zero_arg_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (plus:SI (reg:SI 29 sp) (const_int 16)))",
                    "(set (mem:SI (reg:SI 4 a0)) (const_int 0))",
                    "(parallel[ (call (mem:SI (symbol_ref \"no_args_callee\")) (const_int 16)) "
                    "(clobber (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  explicit_zero_arg_cfg.rpo_blocks[0].rtl_insns[2].has_explicit_call_fusage = true;
  explicit_zero_arg_cfg.rpo_blocks[0].rtl_insns[2].call_fusage.clear();

  ReconstructPostDbrHardRegLivenessAndReturnUse(&explicit_zero_arg_cfg, profile_o2);
  EXPECT_TRUE(explicit_zero_arg_cfg.rpo_blocks[0].rtl_insns[2].has_explicit_call_fusage);
  EXPECT_TRUE(explicit_zero_arg_cfg.rpo_blocks[0].rtl_insns[2].call_fusage.empty());
  EXPECT_TRUE(TopLevelInsnHasNote(explicit_zero_arg_cfg.rpo_blocks[0].instructions[1],
                                  RtlMachineMode::kRegDead, 4));
}

TEST(ReconstructHardRegLivenessTest,
     InfersDeletedNoopCallArgFusageExceptFprSlotAndResolvesConstZeroDeadNotes) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // 1. Entry block saves `$16` before `$31` and copies `$16 <- $4` before calling `first_callee`,
  // then reads `$16` after the call (`jump.c:434-465` deleted `(set $4 $16)` and stripped
  // `REG_DEAD(4)`): `call_fusage` gets `(use (reg:SI 4 a0))` and `copy` omits `REG_DEAD(4)`.
  RtlControlFlowGraph gpr_arg_cfg;
  gpr_arg_cfg.pass = RtlPassKind::kDbr;
  gpr_arg_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 24)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0))",
                    "(set (reg:SI 16 s0) (reg:SI 4 a0))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 31 ra))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref \"first_callee\")) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0)))",
                    "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
                    "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 24)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&gpr_arg_cfg, profile_o2);
  ASSERT_TRUE(gpr_arg_cfg.rpo_blocks[0].rtl_insns[4].has_explicit_call_fusage);
  ASSERT_EQ(gpr_arg_cfg.rpo_blocks[0].rtl_insns[4].call_fusage.size(), 1u);
  EXPECT_EQ(gpr_arg_cfg.rpo_blocks[0].rtl_insns[4].call_fusage[0].ToGccPrintRtlString(),
            "(use (reg:SI 4 a0))");
  EXPECT_FALSE(
      TopLevelInsnHasNote(gpr_arg_cfg.rpo_blocks[0].instructions[2], RtlMachineMode::kRegDead, 4));

  // 2. When `$f12` (`44`) already occupies argument word 0 (`mips.c:function_arg`), `$4` cannot
  // also be passed as GPR argument 0, so `call_fusage` contains only `$f12` and `$16 <- $4`
  // retains `REG_DEAD(4)`.
  RtlControlFlowGraph fpr_lead_cfg;
  fpr_lead_cfg.pass = RtlPassKind::kDbr;
  fpr_lead_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 24)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0))",
                    "(set (reg:SI 16 s0) (reg:SI 4 a0))",
                    "(set (reg:SF 44 $f12) (mem:SF (reg:SI 5 a1)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 31 ra))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref \"fp_callee\")) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0)))",
                    "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
                    "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 24)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&fpr_lead_cfg, profile_o2);
  ASSERT_TRUE(fpr_lead_cfg.rpo_blocks[0].rtl_insns[5].has_explicit_call_fusage);
  ASSERT_EQ(fpr_lead_cfg.rpo_blocks[0].rtl_insns[5].call_fusage.size(), 1u);
  EXPECT_EQ(fpr_lead_cfg.rpo_blocks[0].rtl_insns[5].call_fusage[0].ToGccPrintRtlString(),
            "(use (reg:SF 44 $f12))");
  EXPECT_TRUE(
      TopLevelInsnHasNote(fpr_lead_cfg.rpo_blocks[0].instructions[2], RtlMachineMode::kRegDead, 4));

  // 3. Unpacked post-reload instruction carrying `(expr_list:REG_DEAD (const_int 0) (nil))` stays
  // untouched during `RecomputePostReloadLiveAtStart` and is resolved back into
  // `(expr_list:REG_DEAD (reg:SI 3 v1) (nil))` by `ResolvePostReloadConstZeroRegDeadNotes`
  // even when `sideband.reg_renumber` is populated.
  RtlControlFlowGraph unpacked_jump2;
  unpacked_jump2.pass = RtlPassKind::kJump2;
  unpacked_jump2.sideband.reg_renumber = {{72, 3}};
  unpacked_jump2.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg/i:SI 2 v0) (reg:SI 3 v1)) "
                    "(expr_list:REG_DEAD (const_int 0) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  RecomputePostReloadLiveAtStart(&unpacked_jump2);
  EXPECT_FALSE(TopLevelInsnHasNote(unpacked_jump2.rpo_blocks[0].instructions[1],
                                   RtlMachineMode::kRegDead, 3));
  ResolvePostReloadConstZeroRegDeadNotes(&unpacked_jump2);
  EXPECT_TRUE(TopLevelInsnHasNote(unpacked_jump2.rpo_blocks[0].instructions[1],
                                  RtlMachineMode::kRegDead, 3));
}

TEST(ReconstructHardRegLivenessTest,
     NonAnnulledBackwardLoopExitDelaySlotReturnAndCallerSavedArgExcludedFromFusage) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  // 1. Non-annulled backward branch `SEQUENCE` writing `$2` (`v0`) in its delay slot on
  //    fallthrough exit: because the branch is NOT annulled (`jump_insn`, not `jump_insn/u`), its
  //    delay-slot `SET` to `$2` executes on BOTH the backward loop edge (`B1`) and the fallthrough
  //    exit edge (`B2`), so `$2` reaches `return_internal` on all paths and
  //    `(use (reg/i:SI 2 v0))` MUST be emitted!
  RtlControlFlowGraph loop_exit_cfg;
  loop_exit_cfg.pass = RtlPassKind::kDbr;
  loop_exit_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg:SI 3 v1) (reg:SI 4 a0))"}, {1}),
      MakeBlock(1, true,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int -1)))",
                    "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 3 v1) "
                    "(const_int 0)) (label_ref L0) (pc)))) "
                    "(insn (set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 10))))])",
                },
                {1, 2}),
      MakeBlock(2, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&loop_exit_cfg, profile_o2);
  ASSERT_EQ(loop_exit_cfg.rpo_blocks[2].instructions.size(), 2u);
  EXPECT_TRUE(
      IsReturnValueUseInsn(loop_exit_cfg.rpo_blocks[2].instructions[0], 2, RtlMachineMode::kSi));
  EXPECT_TRUE(
      InsnHasReturnValueSet(loop_exit_cfg.rpo_blocks[1].instructions[1], 2, RtlMachineMode::kSi));

  // 2. Caller-save preserved `$6` (`a2`) written before a 2-arg `CALL_INSN`, saved to `[sp + 16]`
  //    before the call, and restored from `[sp + 16]` after the call: `$6` is a caller-saved local
  //    value across the call, NOT a 3rd call argument, so `call_fusage` contains only `$5` and `$4`
  //    and excludes `(use (reg:SI 6 a2))`.
  RtlControlFlowGraph caller_save_a2_cfg;
  caller_save_a2_cfg.pass = RtlPassKind::kDbr;
  caller_save_a2_cfg.sideband.frame_size_bytes = 24;
  caller_save_a2_cfg.sideband.saved_gpr_regnos = {31};
  caller_save_a2_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int 24)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 31 ra))",
                    "(set (reg:SI 6 a2) (plus:SI (reg:SI 4 a0) (const_int 9)))",
                    "(set (reg:SI 4 a0) (const_int 1))",
                    "(set (reg:SI 5 a1) (const_int 2))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 6 a2))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref \"two_arg_callee\")) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SI 6 a2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 6 a2)))",
                    "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 24)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&caller_save_a2_cfg, profile_o2);
  ASSERT_TRUE(caller_save_a2_cfg.rpo_blocks[0].rtl_insns[6].has_explicit_call_fusage);
  ASSERT_EQ(caller_save_a2_cfg.rpo_blocks[0].rtl_insns[6].call_fusage.size(), 2u);
  EXPECT_EQ(caller_save_a2_cfg.rpo_blocks[0].rtl_insns[6].call_fusage[0].ToGccPrintRtlString(),
            "(use (reg:SI 5 a1))");
  EXPECT_EQ(caller_save_a2_cfg.rpo_blocks[0].rtl_insns[6].call_fusage[1].ToGccPrintRtlString(),
            "(use (reg:SI 4 a0))");
}

TEST(ReconstructHardRegLivenessTest,
     OptimizeSkipAnnulledBranchAndTrailingLoopLatchFallthroughReturnUse) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips3", "-mcpu=r4000"});

  // 1. `reorg.c:optimize_skip` annulled branch (`jump_insn/u` + `insn/s`, such as
  //    `bltzl $2, L0; subu $2, $0, $2; L0: jr $31`) where both taken and fallthrough edges enter
  //    `b + 1` (`successors = {1, 1}`): `$2` is defined on fallthrough before the branch and
  //    redefined unread in the annulled delay slot on the taken edge, so `(use (reg/i:SI 2 v0))`
  //    must be emitted in `B1` (`L0:`) and both `SET`s marked `reg/i`.
  RtlControlFlowGraph opt_skip_cfg;
  opt_skip_cfg.pass = RtlPassKind::kDbr;
  opt_skip_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (reg:SI 4 a0))",
                    "(sequence [(jump_insn/u (set (pc) (if_then_else (lt:SI (reg:SI 2 v0) "
                    "(const_int 0)) (label_ref L0) (pc)))) "
                    "(insn/s (set (reg:SI 2 v0) (neg:SI (reg:SI 2 v0))))])",
                },
                {1, 1}),
      MakeBlock(1, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&opt_skip_cfg, profile_o2);
  ASSERT_EQ(opt_skip_cfg.rpo_blocks[1].instructions.size(), 2u);
  EXPECT_TRUE(
      IsReturnValueUseInsn(opt_skip_cfg.rpo_blocks[1].instructions[0], 2, RtlMachineMode::kSi));
  EXPECT_TRUE(
      InsnHasReturnValueSet(opt_skip_cfg.rpo_blocks[0].instructions[0], 2, RtlMachineMode::kSi));
  EXPECT_TRUE(
      InsnHasReturnValueSet(opt_skip_cfg.rpo_blocks[0].instructions[1], 2, RtlMachineMode::kSi));

  // 2. Trailing loop-latch conditional branch `SEQUENCE` (`last_blk` ends in a conditional jump
  //    whose fallthrough exits into the stripped epilogue):
  //    `(use (reg/i:SI 2 v0))` must be placed in a new unlabeled fallthrough basic block `B2`
  //    rather than appended after `JUMP_INSN` inside `B1`.
  RtlControlFlowGraph trailing_latch_cfg;
  trailing_latch_cfg.pass = RtlPassKind::kDbr;
  trailing_latch_cfg.sideband.frame_size_bytes = 16;
  trailing_latch_cfg.sideband.saved_gpr_regnos = {16};
  trailing_latch_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg:SI 16 s0) (reg:SI 4 a0))"}, {1}),
      MakeBlock(1, true,
                {
                    "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int -1)))",
                    "(sequence [(jump_insn (set (pc) (if_then_else (gt:SI (reg:SI 16 s0) "
                    "(const_int 0)) (label_ref L0) (pc)))) "
                    "(insn (set (reg:SI 2 v0) (reg:SI 16 s0)))])",
                },
                {1}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&trailing_latch_cfg, profile_o2);
  ASSERT_EQ(trailing_latch_cfg.rpo_blocks.size(), 3u);
  EXPECT_EQ(trailing_latch_cfg.rpo_blocks[1].successors, (std::vector<size_t>{1, 2}));
  ASSERT_EQ(trailing_latch_cfg.rpo_blocks[2].instructions.size(), 1u);
  EXPECT_TRUE(IsReturnValueUseInsn(trailing_latch_cfg.rpo_blocks[2].instructions[0], 2,
                                   RtlMachineMode::kSi));
}

TEST(ReconstructHardRegLivenessTest, KnownLibcallAndFprGprArgSlotExclusion) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // 1. `optabs.c` `__fixdfdi` takes only `(reg:DF 44 $f12)` under O32 hard-float:
  //    pre-call scratch writes to `$4` (`mthi $4`) and `$6/$7` (`adddi3`) must NOT
  //    enter `CALL_INSN_FUNCTION_USAGE`, so `$4` and `$6/$7` receive `REG_DEAD` on
  //    their last real non-call uses.
  RtlControlFlowGraph libcall_cfg;
  libcall_cfg.pass = RtlPassKind::kDbr;
  libcall_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (ashiftrt:SI (reg:SI 4 a0) (const_int 31)))",
                    "(set (reg:SI 64 hi) (reg:SI 4 a0))",
                    "(set (reg:DI 6 a2) (reg:DI 64 hi))",
                    "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 6 a2)))",
                    "(set (reg:SI 17 s1) (plus:SI (reg:SI 17 s1) (reg:SI 7 a3)))",
                    "(sequence [(call_insn (parallel [(set (reg:DI 2 v0) (call (mem:SI "
                    "(symbol_ref:SI (\"__fixdfdi\"))) (const_int 0))) (clobber (reg:SI 31 ra))])) "
                    "(insn (set (reg:DF 44 $f12) (mult:DF (reg:DF 44 $f12) (reg:DF 32 $f0))))])",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&libcall_cfg, profile_o2);
  EXPECT_TRUE(
      TopLevelInsnHasNote(libcall_cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 4));
  EXPECT_TRUE(
      TopLevelInsnHasNote(libcall_cfg.rpo_blocks[0].instructions[3], RtlMachineMode::kRegDead, 6));
  EXPECT_TRUE(
      TopLevelInsnHasNote(libcall_cfg.rpo_blocks[0].instructions[4], RtlMachineMode::kRegDead, 7));
  ASSERT_TRUE(libcall_cfg.rpo_blocks[0].rtl_insns[5].has_explicit_call_fusage);
  ASSERT_EQ(libcall_cfg.rpo_blocks[0].rtl_insns[5].call_fusage.size(), 1u);
  EXPECT_EQ(libcall_cfg.rpo_blocks[0].rtl_insns[5].call_fusage[0].ToGccPrintRtlString(),
            "(use (reg:DF 44 $f12))");

  // 2. General user call: `$f12` and `$4` both represent `cum->arg_words == 0`
  //    (`mips.c:function_arg`) and cannot coexist in `CALL_INSN_FUNCTION_USAGE`.
  //    When `$4` was consumed before the call and `$f12` was written unread for
  //    the call, `$4` gets `REG_DEAD` on its consuming instruction and is omitted
  //    from `call_fusage`.
  RtlControlFlowGraph user_fp_call_cfg;
  user_fp_call_cfg.pass = RtlPassKind::kDbr;
  user_fp_call_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                    "(set (mem:SI (reg:SI 16 s0)) (reg:SI 4 a0))",
                    "(set (reg:DF 44 $f12) (mem:DF (reg:SI 16 s0)))",
                    "(parallel [(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI "
                    "(\"user_df_callee\"))) (const_int 16))) (clobber (reg:SI 31 ra))])",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&user_fp_call_cfg, profile_o2);
  EXPECT_TRUE(TopLevelInsnHasNote(user_fp_call_cfg.rpo_blocks[0].instructions[1],
                                  RtlMachineMode::kRegDead, 4));
  ASSERT_TRUE(user_fp_call_cfg.rpo_blocks[0].rtl_insns[3].has_explicit_call_fusage);
  ASSERT_EQ(user_fp_call_cfg.rpo_blocks[0].rtl_insns[3].call_fusage.size(), 1u);
  EXPECT_EQ(user_fp_call_cfg.rpo_blocks[0].rtl_insns[3].call_fusage[0].ToGccPrintRtlString(),
            "(use (reg:DF 44 $f12))");
}

TEST(ReconstructHardRegLivenessTest,
     JumpTableLabelRefO0NonCallArgReadAndLabeledNonBareReturnUsePlacement) {
  const CompilationProfile profile_o0 = MakeProfile(0, {"-O0", "-mips2"});
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // 1. Jump-table `ADDR_VEC` `kLabelRef` base load into `$4` (`la $4, .Ljt`) followed by a
  //    0-argument `CALL_INSN` in a switch case must NEVER count `$4` as an outgoing call argument.
  RtlBasicBlock jt_entry =
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (label_ref L0))",
                    "(set (reg:SI 2 v0) (mem:SI (reg:SI 4 a0)))",
                    "(parallel [(set (pc) (reg:SI 2 v0)) (use (label_ref L0))])",
                },
                {1});
  jt_entry.addr_vec_targets = {1};
  RtlBasicBlock jt_case =
      MakeBlock(1, true,
                {
                    "(parallel [(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI "
                    "(\"zero_arg_case\"))) (const_int 0))) (clobber (reg:SI 31 ra))])",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {});
  RtlControlFlowGraph jt_cfg;
  jt_cfg.pass = RtlPassKind::kDbr;
  jt_cfg.rpo_blocks = {jt_entry, jt_case};
  ReconstructPostDbrHardRegLivenessAndReturnUse(&jt_cfg, profile_o2);
  EXPECT_TRUE(
      TopLevelInsnHasNote(jt_cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 4));
  ASSERT_TRUE(jt_cfg.rpo_blocks[1].rtl_insns[0].has_explicit_call_fusage);
  EXPECT_TRUE(jt_cfg.rpo_blocks[1].rtl_insns[0].call_fusage.empty());

  // 2. At `-O0`, `expand` never reuses an expression-temporary hard register as an outgoing call
  //    argument: a non-call read of `$4` clears `may_explicit` before a 0-argument `CALL_INSN`.
  RtlControlFlowGraph o0_non_call_cfg;
  o0_non_call_cfg.pass = RtlPassKind::kDbr;
  o0_non_call_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (mem:SI (plus:SI (reg:SI 30 fp) (const_int 16))))",
                    "(set (mem:SI (plus:SI (reg:SI 30 fp) (const_int 20))) (reg:SI 4 a0))",
                    "(parallel [(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI "
                    "(\"zero_arg_fn\"))) (const_int 0))) (clobber (reg:SI 31 ra))])",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o0_non_call_cfg, profile_o0);
  ASSERT_TRUE(o0_non_call_cfg.rpo_blocks[0].rtl_insns[2].has_explicit_call_fusage);
  EXPECT_TRUE(o0_non_call_cfg.rpo_blocks[0].rtl_insns[2].call_fusage.empty());

  // 3. When `last_blk` (`has_label == true`) contains active instructions before `RETURN`
  //    (e.g. a shared cross-jumped store+load tail computing `(set (reg:SI 2 v0) ...)`),
  //    `reorg.c:find_end_label` did NOT place `end_of_function_label` at `last_blk`'s label:
  //    `(use (reg/i:SI 2 v0))` must be placed inside `last_blk` immediately before `RETURN`,
  //    never inside `prev_blk` ahead of `$2`'s definition!
  RtlControlFlowGraph shared_tail_cfg;
  shared_tail_cfg.pass = RtlPassKind::kDbr;
  shared_tail_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
                {1, 2}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 1)))",
                },
                {2}),
      MakeBlock(2, true,
                {
                    "(set (mem:SI (reg:SI 6 a2)) (reg:SI 5 a1))",
                    "(set (reg:SI 2 v0) (reg:SI 5 a1))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&shared_tail_cfg, profile_o2);
  EXPECT_EQ(shared_tail_cfg.rpo_blocks[1].instructions.size(), 1u);
  ASSERT_EQ(shared_tail_cfg.rpo_blocks[2].instructions.size(), 4u);
  EXPECT_TRUE(
      IsReturnValueUseInsn(shared_tail_cfg.rpo_blocks[2].instructions[2], 2, RtlMachineMode::kSi));
  EXPECT_FALSE(shared_tail_cfg.sideband.basic_block_live_at_start[0].contains(2));
  EXPECT_FALSE(shared_tail_cfg.sideband.basic_block_live_at_start[1].contains(2));
  EXPECT_FALSE(shared_tail_cfg.sideband.basic_block_live_at_start[2].contains(2));
}

TEST(ReconstructHardRegLivenessTest, DeadLoopCombineUseHandlesMultiBlockLoopAndSched2Priority) {
  auto make_loop_cfg = [](bool first_insn_is_load) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    const std::string first_insn =
        first_insn_is_load ? "(set (reg:SI 6 a2) (mem:SI (reg:SI 4 a0)))"
                           : "(set (reg:SI 6 a2) (plus:SI (reg:SI 4 a0) (const_int 1)))";
    cfg.rpo_blocks = {
        MakeBlock(0, false, {"(set (reg:SI 25 t9) (const_int 0))"}, {1}),
        MakeBlock(1, true,
                  {
                      first_insn,
                      "(set (reg:SI 24 t8) (plus:SI (reg:SI 6 a2) (reg:SI 5 a1)))",
                      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 24 t8))",
                  },
                  {2}),
        MakeBlock(2, false,
                  {
                      "(set (reg:SI 25 t9) (plus:SI (reg:SI 25 t9) (reg:SI 6 a2)))",
                      "(set (pc) (if_then_else (lt:SI (reg:SI 25 t9) (reg:SI 5 a1)) "
                      "(label_ref L0) (pc)))",
                  },
                  {1, 3}),
        MakeBlock(3, false,
                  {
                      "(set (reg:SI 2 v0) (reg:SI 25 t9))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    return cfg;
  };

  const CompilationProfile profile_o1{.optimize_level = 1, .c_flags = {"-O1", "-mips2"}};
  const CompilationProfile profile_o2{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};

  RtlControlFlowGraph o1_load_cfg = make_loop_cfg(/*first_insn_is_load=*/true);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o1_load_cfg, profile_o1);
  ASSERT_EQ(o1_load_cfg.rpo_blocks[1].instructions.size(), 4u) << o1_load_cfg.FormatSummary();
  EXPECT_EQ(o1_load_cfg.rpo_blocks[1].instructions[0],
            "(use (reg:SI 24 t8)) (expr_list:REG_DEAD (reg:SI 24 t8) (nil))");

  RtlControlFlowGraph o2_load_cfg = make_loop_cfg(/*first_insn_is_load=*/true);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o2_load_cfg, profile_o2);
  ASSERT_EQ(o2_load_cfg.rpo_blocks[1].instructions.size(), 4u) << o2_load_cfg.FormatSummary();
  EXPECT_EQ(o2_load_cfg.rpo_blocks[1].instructions[1],
            "(use (reg:SI 24 t8)) (expr_list:REG_DEAD (reg:SI 24 t8) (nil))");

  RtlControlFlowGraph o2_alu_cfg = make_loop_cfg(/*first_insn_is_load=*/false);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o2_alu_cfg, profile_o2);
  ASSERT_EQ(o2_alu_cfg.rpo_blocks[1].instructions.size(), 4u) << o2_alu_cfg.FormatSummary();
  EXPECT_EQ(o2_alu_cfg.rpo_blocks[1].instructions[0],
            "(use (reg:SI 24 t8)) (expr_list:REG_DEAD (reg:SI 24 t8) (nil))");
}

TEST(ReconstructHardRegLivenessTest, SharedReturnMoveWitnessSupportsFourExitPredsAndDfReturn) {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:DF 34 $f2) (mem:DF (reg:SI 4 a0)))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:DF 32 $f0) (plus:DF (reg:DF 34 $f2) (reg:DF 34 $f2)))",
                    "(set (pc) (label_ref L3))",
                },
                {7}),
      MakeBlock(2, true,
                {
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 1)) "
                    "(label_ref L1) (pc)))",
                },
                {4, 3}),
      MakeBlock(3, false,
                {
                    "(set (reg:DF 32 $f0) (mult:DF (reg:DF 34 $f2) (reg:DF 34 $f2)))",
                    "(set (pc) (label_ref L3))",
                },
                {7}),
      MakeBlock(4, true,
                {
                    "(set (reg:SI 2 v0) (const_int 2))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (reg:SI 2 v0)) "
                    "(label_ref L2) (pc)))",
                },
                {6, 5}),
      MakeBlock(5, false,
                {
                    "(set (reg:DF 32 $f0) (minus:DF (reg:DF 34 $f2) (reg:DF 34 $f2)))",
                    "(set (pc) (label_ref L3))",
                },
                {7}),
      MakeBlock(6, true,
                {
                    "(set (reg:DF 32 $f0) (neg:DF (reg:DF 34 $f2)))",
                },
                {7}),
      MakeBlock(7, true,
                {
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };

  const CompilationProfile profile_o2{
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips2", "-mcpu=r4000"},
  };
  EXPECT_TRUE(HasDeletedSharedReturnMoveWitness(cfg, profile_o2, 32, RtlMachineMode::kDf));

  ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
  EXPECT_FALSE(InsnHasReturnValueSet(cfg.rpo_blocks[1].instructions[0], 32, RtlMachineMode::kDf));
  EXPECT_FALSE(InsnHasReturnValueSet(cfg.rpo_blocks[3].instructions[0], 32, RtlMachineMode::kDf));
  EXPECT_FALSE(InsnHasReturnValueSet(cfg.rpo_blocks[5].instructions[0], 32, RtlMachineMode::kDf));
  EXPECT_FALSE(InsnHasReturnValueSet(cfg.rpo_blocks[6].instructions[0], 32, RtlMachineMode::kDf));
  ASSERT_EQ(cfg.rpo_blocks[6].instructions.size(), 2u);
  EXPECT_TRUE(IsReturnValueUseInsn(cfg.rpo_blocks[6].instructions[1], 32, RtlMachineMode::kDf));
}

TEST(ReconstructHardRegLivenessTest,
     SplitLocalToGlobalRmwRecoversRegDeadAndSkipsCachedMemStoreAndLuiOri) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips1"});

  // 1. Multi-block loop (`B0 -> B2`, `B1 -> B2`, `B2 -> {B1, B3}`) with empty
  // `RtlBasicBlock::predecessors` vectors: `NormalizePostReloadHardRegNotes` derives
  // predecessors from `successors` and attaches `REG_DEAD (reg:SI 3 v1)` onto `B1[0]` where
  // incoming multi-block `$3 v1` is overwritten by a local web that dies at `B1[1]`.
  RtlControlFlowGraph loop_cfg;
  loop_cfg.pass = RtlPassKind::kSched2;
  loop_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 6 a2) (const_int 0))",
                    "(set (pc) (label_ref L1))",
                },
                {2}),
      MakeBlock(1, true,
                {
                    "(set (reg:SI 3 v1) (and:SI (reg:SI 3 v1) (const_int 255)))",
                    "(set (reg:SI 6 a2) (plus:SI (reg:SI 6 a2) (reg:SI 3 v1))) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                    "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                },
                {2}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 3 v1) (zero_extend:SI (mem:QI (reg:SI 4 a0))))",
                    "(set (pc) (if_then_else (ne:SI (reg:SI 3 v1) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
                {1, 3}),
      MakeBlock(3, false,
                {
                    "(set (reg/i:SI 2 v0) (reg:SI 6 a2)) (expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                    "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
                },
                {}),
  };
  ASSERT_TRUE(loop_cfg.rpo_blocks[1].predecessors.empty());
  NormalizePostReloadHardRegNotes(&loop_cfg, profile_o2);
  EXPECT_TRUE(
      TopLevelInsnHasNote(loop_cfg.rpo_blocks[1].instructions[0], RtlMachineMode::kRegDead, 3));

  // 2. Cached bitfield memory load-RMW-store (`v1 = [a0]; v1 &= -256; v1 |= a2; [a0] = v1`) where
  // `v1` stays live into `B1`, and 2-insn `lui + ori` constant split (`t0 = 65536; t0 |= 5`) in a
  // non-entry block: both define a single multi-block pseudo in place and must NOT receive a
  // bogus `REG_DEAD` note.
  RtlControlFlowGraph cached_cfg;
  cached_cfg.pass = RtlPassKind::kSched2;
  cached_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 3 v1) (mem/s:SI (reg:SI 4 a0)))",
                    "(set (reg:SI 3 v1) (and:SI (reg:SI 3 v1) (const_int -256)))",
                    "(set (reg:SI 3 v1) (ior:SI (reg:SI 3 v1) (reg:SI 6 a2))) "
                    "(expr_list:REG_DEAD (reg:SI 6 a2) (nil))",
                    "(set (mem/s:SI (reg:SI 4 a0)) (reg:SI 3 v1))",
                    "(set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int 0)) "
                    "(label_ref L1) (pc)))",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 8 t0) (const_int 65536))",
                    "(set (reg:SI 8 t0) (ior:SI (reg:SI 8 t0) (const_int 5)))",
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0)))",
                },
                {2}),
      MakeBlock(2, true,
                {
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0))) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (expr_list:REG_DEAD (reg:SI 8 t0) (nil)))",
                    "(use (reg/i:SI 2 v0)) (expr_list:REG_DEAD (reg/i:SI 2 v0) (nil))",
                },
                {}),
  };
  NormalizePostReloadHardRegNotes(&cached_cfg, profile_o2);
  EXPECT_FALSE(
      TopLevelInsnHasNote(cached_cfg.rpo_blocks[0].instructions[2], RtlMachineMode::kRegDead, 3));
  EXPECT_FALSE(
      TopLevelInsnHasNote(cached_cfg.rpo_blocks[1].instructions[1], RtlMachineMode::kRegDead, 8));
}

TEST(ReconstructHardRegLivenessTest,
     ResolveConstZeroRegDeadDistinguishesSingleMdProducerFromMultiMdBlock) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips1"});

  // 1. Single `mult -> mflo $3 -> plus` in block (`!other_md`): the delay-slot
  //    `(REG_DEAD (const_int 0))` on the consumer resolves to `(reg:SI 65 lo)`.
  RtlControlFlowGraph single_md_cfg;
  single_md_cfg.pass = RtlPassKind::kDbr;
  single_md_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                    "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 hilo)) ] )",
                    "(set (reg:SI 3 v1) (reg:SI 65 lo))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2))) "
                    "(expr_list:REG_DEAD (const_int 0) (nil))",
                    "(use (reg/i:SI 2 v0))",
                },
                {}),
  };
  ResolvePostReloadConstZeroRegDeadNotes(&single_md_cfg, profile_o2);
  EXPECT_TRUE(TopLevelInsnHasNote(single_md_cfg.rpo_blocks[0].instructions[2],
                                  RtlMachineMode::kRegDead, 65));
  EXPECT_FALSE(TopLevelInsnHasNote(single_md_cfg.rpo_blocks[0].instructions[2],
                                   RtlMachineMode::kRegDead, 3));

  // 2. When the same basic block contains a second MD operation (`other_md == true`),
  //    `65 lo` is not promoted as the pseudo home, so `(REG_DEAD (const_int 0))`
  //    resolves to the GPR operand `(reg:SI 3 v1)`.
  RtlControlFlowGraph multi_md_cfg;
  multi_md_cfg.pass = RtlPassKind::kDbr;
  multi_md_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 4 a0) (reg:SI 5 a1))) "
                    "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 hilo)) ] )",
                    "(set (reg:SI 3 v1) (reg:SI 65 lo))",
                    "(set (reg:SI 7 a3) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2))) "
                    "(expr_list:REG_DEAD (const_int 0) (nil))",
                    "(parallel[ (set (reg:SI 65 lo) (mult:SI (reg:SI 7 a3) (reg:SI 4 a0))) "
                    "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 hilo)) ] )",
                    "(set (reg/i:SI 2 v0) (reg:SI 65 lo))",
                    "(use (reg/i:SI 2 v0))",
                },
                {}),
  };
  ResolvePostReloadConstZeroRegDeadNotes(&multi_md_cfg, profile_o2);
  EXPECT_TRUE(
      TopLevelInsnHasNote(multi_md_cfg.rpo_blocks[0].instructions[2], RtlMachineMode::kRegDead, 3));
  EXPECT_FALSE(TopLevelInsnHasNote(multi_md_cfg.rpo_blocks[0].instructions[2],
                                   RtlMachineMode::kRegDead, 65));
}

TEST(ReconstructHardRegLivenessTest, SharedReturnWitnessRecognizesJumpDelaySlotReturnRegisterSet) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 7)))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(sequence [(jump_insn (set (pc) (label_ref L1))) "
                    "(insn (set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1))))])",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 2)))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  EXPECT_TRUE(HasDeletedSharedReturnMoveWitness(cfg, profile_o2, 2, RtlMachineMode::kSi));

  ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
  EXPECT_EQ(cfg.rpo_blocks[1].instructions[0].find("(reg/i:SI 2 v0)"), std::string::npos);
  EXPECT_EQ(cfg.rpo_blocks[2].instructions[0].find("(reg/i:SI 2 v0)"), std::string::npos);
  ASSERT_EQ(cfg.rpo_blocks[2].instructions.size(), 2u);
  EXPECT_TRUE(IsReturnValueUseInsn(cfg.rpo_blocks[2].instructions[1], 2, RtlMachineMode::kSi));
}

TEST(ReconstructHardRegLivenessTest,
     DisambiguatesDisassembledMips1MultAndMultuWhenHiIsDeadAndLoIsLive) {
  GccContext ctx;
  FunctionHooks hooks(&ctx);
  ProductionHardRegLivenessInverseProvider provider;
  RightInverseVerifier verifier(&ctx, &hooks, &provider);
  const std::vector<std::string> mips1_flags = {"-O2", "-mips1", "-Wa,-O2"};

  // 1. Signed `mult`: `64 hi` is dead while `65 lo` is live, so Stage 0 rewrites disassembled
  //    `smulsi3_highpart` into `mulsi3_internal` (`(set (reg:SI 65 lo) (mult:SI ...))`).
  constexpr std::string_view kSignedSrc = R"(
int mul_signed(int a, int b, int c) {
  int prod = a * b;
  if (c == 0) return prod;
  return prod + c;
}
)";
  const auto signed_trace =
      hooks.TraceForwardCompilation(kSignedSrc, "mul_signed", mips1_flags, "mul_signed");
  const RtlControlFlowGraph lifted_signed =
      lifter::InvertFinalAndGasFunction(signed_trace.stage0_input);
  ASSERT_FALSE(lifted_signed.rpo_blocks.empty());
  bool found_signed_mulsi3 = false;
  for (const std::string& insn : lifted_signed.rpo_blocks[0].instructions) {
    const auto pn = ParsePatternAndNotes(insn);
    if (pn.ok() && pn->pattern.op == RtlOp::kParallel && pn->pattern.children.size() == 3u &&
        pn->pattern.children[0].IsSet() && pn->pattern.children[0].SetDest().IsReg() &&
        pn->pattern.children[0].SetDest().RegNo() == 65 &&
        pn->pattern.children[0].SetSrc().op == RtlOp::kMult) {
      found_signed_mulsi3 = true;
    }
  }
  EXPECT_TRUE(found_signed_mulsi3);
  const auto signed_res = verifier.VerifyFinalAndGasStageRightInverse(signed_trace.stage0_input);
  EXPECT_TRUE(signed_res.matches) << signed_res.diff_detail;

  // 2. Unsigned `multu` (when `unsigned` multiply is emitted or synthetic `multu` + `mflo` words):
  //    `64 hi` is dead while `65 lo` is live, so Stage 0 rewrites disassembled `umulsi3_highpart`
  //    into `umulsidi3_internal` (`(set (reg:DI 64 hi) (mult:DI (zero_extend:DI ...) ...))`) with
  //    partial `REG_UNUSED (reg:SI 64 hi)`, preserving exact `multu` machine emission (`0x19`).
  Stage0FunctionInput unsigned_in = signed_trace.stage0_input;
  bool patched_mult_to_multu = false;
  for (uint32_t& word : unsigned_in.machine_words) {
    if ((word & 0xfc00ffffu) == 0x00000018u) {
      word |= 0x1u;  // `mult` (0x18) -> `multu` (0x19)
      patched_mult_to_multu = true;
    }
  }
  ASSERT_TRUE(patched_mult_to_multu);
  const RtlControlFlowGraph lifted_unsigned = lifter::InvertFinalAndGasFunction(unsigned_in);
  ASSERT_FALSE(lifted_unsigned.rpo_blocks.empty());
  bool found_unsigned_umulsidi3 = false;
  bool has_unused_hi_si = false;
  for (const std::string& insn : lifted_unsigned.rpo_blocks[0].instructions) {
    const auto pn = ParsePatternAndNotes(insn);
    if (pn.ok() && pn->pattern.op == RtlOp::kParallel && pn->pattern.children.size() == 2u &&
        pn->pattern.children[0].IsSet() && pn->pattern.children[0].SetDest().IsReg() &&
        pn->pattern.children[0].SetDest().RegNo() == 64 &&
        pn->pattern.children[0].SetDest().mode == RtlMachineMode::kDi &&
        pn->pattern.children[0].SetSrc().op == RtlOp::kMult) {
      found_unsigned_umulsidi3 = true;
      for (const RtlRegNote& note : pn->reg_notes) {
        if (note.kind == RtlMachineMode::kRegUnused && note.datum.IsReg() &&
            note.datum.RegNo() == 64 && note.datum.mode == RtlMachineMode::kSi) {
          has_unused_hi_si = true;
        }
      }
    }
  }
  EXPECT_TRUE(found_unsigned_umulsidi3);
  EXPECT_TRUE(has_unused_hi_si);
  const auto unsigned_res = verifier.VerifyFinalAndGasStageRightInverse(unsigned_in);
  EXPECT_TRUE(unsigned_res.matches) << unsigned_res.diff_detail;

  // 3. When `64 hi` IS read (`mfhi` from unsigned constant division), `umulsi3_highpart` is
  //    preserved unchanged.
  constexpr std::string_view kHighpartSrc = R"(
unsigned div_ten(unsigned a) {
  return a / 10u;
}
)";
  const auto highpart_trace =
      hooks.TraceForwardCompilation(kHighpartSrc, "div_ten", mips1_flags, "div_ten");
  const RtlControlFlowGraph lifted_highpart =
      lifter::InvertFinalAndGasFunction(highpart_trace.stage0_input);
  ASSERT_FALSE(lifted_highpart.rpo_blocks.empty());
  bool found_highpart_truncate = false;
  for (const std::string& insn : lifted_highpart.rpo_blocks[0].instructions) {
    const auto pn = ParsePatternAndNotes(insn);
    if (pn.ok() && pn->pattern.op == RtlOp::kParallel && pn->pattern.children.size() == 3u &&
        pn->pattern.children[0].IsSet() &&
        pn->pattern.children[0].SetSrc().op == RtlOp::kTruncate) {
      found_highpart_truncate = true;
    }
  }
  EXPECT_TRUE(found_highpart_truncate);
  const auto highpart_res =
      verifier.VerifyFinalAndGasStageRightInverse(highpart_trace.stage0_input);
  EXPECT_TRUE(highpart_res.matches) << highpart_res.diff_detail;
}

TEST(ReconstructHardRegLivenessTest,
     ReversesMultiClobberUnusedNoteOrderOnDemotedParallelHardSetDest) {
  const CompilationProfile profile = MakeProfile(2, {"-O2", "-mips1"});

  // 1. Positive case: 3-clobber `mult:SI` `PARALLEL` writing `(reg:SI 8 t0)` while lower GPR `$5`
  //    (`a1`) dies on the multiply and stays dead across `t0`'s `[0, 2]` live range (`8 t0` is
  //    demoted to a hard register in `.08.lreg`). `REG_UNUSED` notes on `(64, 65, 66)` must be
  //    emitted in reversed `(66, 65, 64)` order matching `GccContext::MaterializeFromCfg`.
  RtlControlFlowGraph demoted_cfg;
  demoted_cfg.pass = RtlPassKind::kDbr;
  demoted_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(parallel[ (set (reg:SI 8 t0) (mult:SI (reg:SI 5 a1) (reg:SI 6 a2))) "
                    "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) "
                    "(clobber (reg:SI 66 hilo)) ] )",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 3)))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 8 t0)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&demoted_cfg, profile);
  const auto demoted_pn = ParsePatternAndNotes(demoted_cfg.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(demoted_pn.ok());
  std::vector<int> demoted_unused_regs;
  for (const RtlRegNote& n : demoted_pn->reg_notes) {
    if (n.kind == RtlMachineMode::kRegUnused && n.datum.IsReg()) {
      demoted_unused_regs.push_back(n.datum.RegNo());
    }
  }
  const std::vector<int> expected_rev = {66, 65, 64};
  EXPECT_EQ(demoted_unused_regs, expected_rev) << demoted_cfg.rpo_blocks[0].instructions[0];

  // 2. Contrast case: when the same 3-clobber `PARALLEL` writes `(reg:SI 2 v0)` (lowest caller-save
  //    GPR, which lifts to a pseudo in `.08.lreg`), `REG_UNUSED` notes remain in forward
  //    `(64, 65, 66)` order.
  RtlControlFlowGraph pseudo_cfg;
  pseudo_cfg.pass = RtlPassKind::kDbr;
  pseudo_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(parallel[ (set (reg:SI 2 v0) (mult:SI (reg:SI 5 a1) (reg:SI 6 a2))) "
                    "(clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) "
                    "(clobber (reg:SI 66 hilo)) ] )",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&pseudo_cfg, profile);
  const auto pseudo_pn = ParsePatternAndNotes(pseudo_cfg.rpo_blocks[0].instructions[0]);
  ASSERT_TRUE(pseudo_pn.ok());
  std::vector<int> pseudo_unused_regs;
  for (const RtlRegNote& n : pseudo_pn->reg_notes) {
    if (n.kind == RtlMachineMode::kRegUnused && n.datum.IsReg()) {
      pseudo_unused_regs.push_back(n.datum.RegNo());
    }
  }
  const std::vector<int> expected_fwd = {64, 65, 66};
  EXPECT_EQ(pseudo_unused_regs, expected_fwd) << pseudo_cfg.rpo_blocks[0].instructions[0];
}

TEST(ReconstructHardRegLivenessTest,
     SynthesizePostReloadMemRegEquivNotesAndExcludeProvenGprSpillFromCallFusage) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  // 1. `SynthesizePostReloadMemRegEquivNotes`: in a leaf function (`frame_size_bytes == 0`), an
  //    unmodified incoming 5th stack parameter load `[sp + 16]` receives `REG_EQUIV (mem:SI ...)`,
  //    while any stale `REG_EQUIV (mem:SI ...)` on a slot overwritten later in the function is
  //    stripped cleanly.
  RtlControlFlowGraph equiv_cfg;
  equiv_cfg.pass = RtlPassKind::kDbr;
  equiv_cfg.sideband.frame_size_bytes = 0;
  equiv_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 6 a2) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 6 a2) (const_int 1)))",
                    "(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20)))) "
                    "(expr_list:REG_EQUIV (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (nil))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 4 a0))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&equiv_cfg, profile_o2);
  SynthesizePostReloadMemRegEquivNotes(&equiv_cfg, profile_o2);
  EXPECT_NE(equiv_cfg.rpo_blocks[0].instructions[0].find(
                "REG_EQUIV (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16)))"),
            std::string::npos)
      << equiv_cfg.rpo_blocks[0].instructions[0];
  EXPECT_EQ(equiv_cfg.rpo_blocks[0].instructions[2].find("REG_EQUIV"), std::string::npos)
      << equiv_cfg.rpo_blocks[0].instructions[2];

  // 2. `CollectProvenGprSpillOffsetsForCallFusage`: when all callee-saved GPRs (`16..23, 30, 31`)
  //    are saved in the prologue and `$5` (`a1`) is stored into proven GPR reload spill slot
  //    `[sp + 20]` (`8`-byte bucket `[16, 24)` below saved registers `[24, 64)`) before a 1-arg
  //    `CALL_INSN` and reloaded from `[sp + 20]` into `$2` (`v0`) after the call, `$5` is excluded
  //    from reconstructed `CALL_INSN_FUNCTION_USAGE` (which keeps only `(use (reg:SI 4 a0))`).
  RtlControlFlowGraph spill_call_cfg;
  spill_call_cfg.pass = RtlPassKind::kDbr;
  spill_call_cfg.sideband.frame_size_bytes = 64;
  spill_call_cfg.sideband.outgoing_args_size = 16;
  spill_call_cfg.sideband.saved_gpr_regnos = {16, 17, 18, 19, 20, 21, 22, 23, 30, 31};
  spill_call_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -64)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 60))) (reg:SI 31 ra))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 56))) (reg:SI 30 $fp))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 52))) (reg:SI 23 s7))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 48))) (reg:SI 22 s6))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 44))) (reg:SI 21 s5))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 40))) (reg:SI 20 s4))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 36))) (reg:SI 19 s3))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 32))) (reg:SI 18 s2))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 17 s1))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 16 s0))",
                    "(set (reg:SI 5 a1) (plus:SI (reg:SI 4 a0) (const_int 9)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 5 a1))",
                    "(set (reg:SI 4 a0) (const_int 1))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref \"one_arg_callee\")) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SI 3 v1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1)))",
                    "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 60))))",
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 64)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&spill_call_cfg, profile_o2);
  ASSERT_TRUE(spill_call_cfg.rpo_blocks[0].rtl_insns[14].has_explicit_call_fusage);
  ASSERT_EQ(spill_call_cfg.rpo_blocks[0].rtl_insns[14].call_fusage.size(), 1u);
  EXPECT_EQ(spill_call_cfg.rpo_blocks[0].rtl_insns[14].call_fusage[0].ToGccPrintRtlString(),
            "(use (reg:SI 4 a0))");
}

TEST(ReconstructHardRegLivenessTest,
     PreservesCcFpRawModeAndReturnVsAnnulledSlotAndCallArgDeadNotes) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});
  const CompilationProfile profile_o1 = MakeProfile(1, {"-O1", "-mips2", "-mcpu=r4000"});

  // 1. `raw_mode` preservation (`CC_FP` / `CC_REV_FP`): `(reg:CC_FP 67 $fcr31)` has `mode ==
  //    kUnknown` with `raw_mode == "CC_FP"`. Reconstructed `REG_DEAD` must preserve `:CC_FP`
  //    rather than degrading to `VOIDmode` `(reg 67 $fcr31)`.
  RtlControlFlowGraph ccfp_cfg;
  ccfp_cfg.pass = RtlPassKind::kDbr;
  ccfp_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:CC_FP 67 $fcr31) (lt:CC_FP (reg:SF 44 $f12) (reg:SF 46 $f14)))",
                    "(set (pc) (if_then_else (ne:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
                {1, 1}),
      MakeBlock(1, true,
                {
                    "(set (reg/i:SI 2 v0) (const_int 0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&ccfp_cfg, profile_o2);
  EXPECT_NE(ccfp_cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:CC_FP 67 $fcr31)"),
            std::string::npos)
      << ccfp_cfg.rpo_blocks[0].instructions[1];
  EXPECT_EQ(ccfp_cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg 67 $fcr31)"),
            std::string::npos)
      << ccfp_cfg.rpo_blocks[0].instructions[1];

  // 2. Loop-carried return accumulator `(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4
  //    a0)))` whose backedge reads `v0` again must NOT receive a bogus self-`REG_DEAD v0`
  //    (zero `REG_DEAD` placeholders on the slot since both `v0` and `a0` are live around the
  //    loop).
  RtlControlFlowGraph loop_acc_cfg;
  loop_acc_cfg.pass = RtlPassKind::kDbr;
  loop_acc_cfg.rpo_blocks = {
      MakeBlock(0, false, {"(set (reg:SI 2 v0) (const_int 0))"}, {1}),
      MakeBlock(
          1, true,
          {
              "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int -1)))",
              "(sequence [(jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int 0)) "
              "(label_ref L0) (pc)))) (insn (set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) "
              "(reg:SI 4 a0))))])",
          },
          {1, 2}),
      MakeBlock(2, false, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&loop_acc_cfg, profile_o2);
  EXPECT_EQ(loop_acc_cfg.rpo_blocks[1].instructions[1].find("REG_DEAD"), std::string::npos)
      << loop_acc_cfg.rpo_blocks[1].instructions[1];

  // 3. Contrast case: an annulled branch-likely (`in_struct`) delay slot writing `(reg/i:DF 32
  //    $f0)` executes ONLY on the taken edge (`B2`, which returns immediately), NOT on the
  //    fallthrough edge (`B1`, which reads `$f0`). Therefore the slot DOES receive two
  //    `REG_DEAD (const_int 0)` placeholders (one for `$f2`, one for `$f0`).
  RtlControlFlowGraph annulled_cfg;
  annulled_cfg.pass = RtlPassKind::kDbr;
  annulled_cfg.rpo_blocks = {
      MakeBlock(
          0, false,
          {
              "(set (reg:DF 32 $f0) (reg:DF 6 a2))",
              "(set (reg:DF 34 $f2) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 16))))",
              "(sequence [(jump_insn/u (set (pc) (if_then_else (gt:SI (reg:SI 4 a0) (const_int 0)) "
              "(label_ref L0) (pc)))) (insn/s (set (reg:DF 32 $f0) (plus:DF (reg:DF 32 $f0) "
              "(reg:DF 34 $f2))))])",
          },
          {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:DF 32 $f0) (minus:DF (reg:DF 32 $f0) (reg:DF 34 $f2)))",
                },
                {2}),
      MakeBlock(2, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&annulled_cfg, profile_o1);
  EXPECT_NE(annulled_cfg.rpo_blocks[0].instructions[2].find(
                "(expr_list:REG_DEAD (const_int 0) (expr_list:REG_DEAD (const_int 0) (nil)))"),
            std::string::npos)
      << annulled_cfg.rpo_blocks[0].instructions[2];
}

TEST(ReconstructHardRegLivenessTest,
     Stage0LocalScratchDeadNoteHandlesReorgReturnUseAndAnnulledDelaySlotKills) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // 1a. Positive case (`reorg.c:fill_simple_delay_slots`): `(set (reg/i:SI 2 v0) ...)` moved past
  //     `(use (reg/i:SI 2 v0))` into `(return)`'s delay slot must NOT block Stage 0 from seeing
  //     that `v0` dies on the earlier local RMW (`v0 = v0 + a0`).
  RtlControlFlowGraph bypassed_ret_cfg;
  bypassed_ret_cfg.pass = RtlPassKind::kDbr;
  bypassed_ret_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (ashift:SI (reg:SI 4 a0) (const_int 2)))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))",
                    "(use (reg/i:SI 2 v0))",
                    "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) "
                    "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1))))])",
                },
                {}),
  };
  NormalizePostReloadHardRegNotes(&bypassed_ret_cfg, profile_o2);
  EXPECT_TRUE(TopLevelInsnHasNote(bypassed_ret_cfg.rpo_blocks[0].instructions[1],
                                  RtlMachineMode::kRegDead, 2))
      << bypassed_ret_cfg.rpo_blocks[0].instructions[1];

  // 1b. Negative case: when `(use (reg/i:SI 2 v0))` is NOT followed by an `in_slot` `/i` overwrite
  //     of `v0`, the earlier unpinned RMW feeds `(use (reg/i:SI 2 v0))` directly and must NOT
  //     receive a spurious `REG_DEAD v0`.
  RtlControlFlowGraph unpinned_direct_ret_cfg;
  unpinned_direct_ret_cfg.pass = RtlPassKind::kDbr;
  unpinned_direct_ret_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (ashift:SI (reg:SI 4 a0) (const_int 2)))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  NormalizePostReloadHardRegNotes(&unpinned_direct_ret_cfg, profile_o2);
  EXPECT_FALSE(TopLevelInsnHasNote(unpinned_direct_ret_cfg.rpo_blocks[0].instructions[1],
                                   RtlMachineMode::kRegDead, 2))
      << unpinned_direct_ret_cfg.rpo_blocks[0].instructions[1];

  // 2a. Positive case (`reorg.c:fill_slots_from_thread`): annulled (`insn/s`) taken-branch delay
  //     slot overwrites `v0` without reading it while `v0` is dead on the fallthrough edge (`B1`).
  RtlControlFlowGraph annulled_kill_cfg;
  annulled_kill_cfg.pass = RtlPassKind::kDbr;
  annulled_kill_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (ashift:SI (reg:SI 4 a0) (const_int 1)))",
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))",
                    "(set (mem:SI (reg:SI 4 a0)) (reg:SI 2 v0))",
                    "(sequence [(jump_insn/u (set (pc) (if_then_else "
                    "(ne:SI (reg:SI 5 a1) (const_int 0)) (label_ref L0) (pc)))) "
                    "(insn/s (set (reg:SI 2 v0) (mem:SI (reg:SI 6 a2))))])",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg/i:SI 2 v0) (const_int 0))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
      MakeBlock(2, true,
                {
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  RtlControlFlowGraph annulled_live_ft_cfg = annulled_kill_cfg;
  NormalizePostReloadHardRegNotes(&annulled_kill_cfg, profile_o2);
  EXPECT_TRUE(TopLevelInsnHasNote(annulled_kill_cfg.rpo_blocks[0].instructions[1],
                                  RtlMachineMode::kRegDead, 2))
      << annulled_kill_cfg.rpo_blocks[0].instructions[1];

  // 2b. Negative case: when `v0` is STILL live on the fallthrough edge (`B1`) into a loop
  //     accumulator, the annulled taken-edge overwrite does NOT make `v0` block-local, so `B0[1]`
  //     must NOT receive `REG_DEAD v0`.
  annulled_live_ft_cfg.rpo_blocks[1] =
      MakeBlock(1, true,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 9)))",
                    "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int -1)))",
                    "(set (pc) (if_then_else (ne:SI (reg:SI 5 a1) (const_int 0)) "
                    "(label_ref L1) (pc)))",
                },
                {1, 2});
  NormalizePostReloadHardRegNotes(&annulled_live_ft_cfg, profile_o2);
  EXPECT_FALSE(TopLevelInsnHasNote(annulled_live_ft_cfg.rpo_blocks[0].instructions[1],
                                   RtlMachineMode::kRegDead, 2))
      << annulled_live_ft_cfg.rpo_blocks[0].instructions[1];
}

TEST(ReconstructHardRegLivenessTest,
     Stage0LocalScratchDeadNoteHandlesNarrowBitfieldCalleeSavedAndChainedRmw) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // 1. Narrow 16-bit bitfield mask `(and v1 255)` followed by `(ior v1 a2)` (`0 < mask <= 65535`)
  //    plus a subsequent consumer after a 2-insn `lui; ori` constant split: both allocate fresh
  //    pseudos and must receive `REG_DEAD`.
  RtlControlFlowGraph bitfield_cfg;
  bitfield_cfg.pass = RtlPassKind::kSched2;
  bitfield_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 3 v1) (zero_extend:SI (mem/s:QI (reg:SI 4 a0))))",
                    "(set (reg:SI 3 v1) (and:SI (reg:SI 3 v1) (const_int 240)))",
                    "(set (reg:SI 3 v1) (ior:SI (reg:SI 3 v1) (reg:SI 5 a1)))",
                    "(set (mem/s:QI (reg:SI 4 a0)) (subreg:QI (reg:SI 3 v1) 0))",
                    "(set (reg:SI 8 t0) (const_int 65536))",
                    "(set (reg:SI 8 t0) (ior:SI (reg:SI 8 t0) (const_int 5)))",
                    "(set (reg:SI 8 t0) (and:SI (reg:SI 8 t0) (reg:SI 6 a2)))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 8 t0)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  NormalizePostReloadHardRegNotes(&bitfield_cfg, profile_o2);
  EXPECT_TRUE(
      TopLevelInsnHasNote(bitfield_cfg.rpo_blocks[0].instructions[2], RtlMachineMode::kRegDead, 3))
      << bitfield_cfg.rpo_blocks[0].instructions[2];
  EXPECT_FALSE(
      TopLevelInsnHasNote(bitfield_cfg.rpo_blocks[0].instructions[5], RtlMachineMode::kRegDead, 8))
      << bitfield_cfg.rpo_blocks[0].instructions[5];
  EXPECT_TRUE(
      TopLevelInsnHasNote(bitfield_cfg.rpo_blocks[0].instructions[6], RtlMachineMode::kRegDead, 8))
      << bitfield_cfg.rpo_blocks[0].instructions[6];

  // 2a. Leaf callee-saved register `$s0` (`16`) dying-input reuse without an intervening call
  //     (`local-alloc.c:1395-1450`) attaches `REG_DEAD 16`.
  RtlControlFlowGraph leaf_cs_cfg;
  leaf_cs_cfg.pass = RtlPassKind::kSched2;
  leaf_cs_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 16 s0) (mem:SI (reg:SI 4 a0)))",
                    "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (const_int 7)))",
                    "(set (reg/i:SI 2 v0) (xor:SI (reg:SI 16 s0) (reg:SI 5 a1)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  NormalizePostReloadHardRegNotes(&leaf_cs_cfg, profile_o2);
  EXPECT_TRUE(
      TopLevelInsnHasNote(leaf_cs_cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 16))
      << leaf_cs_cfg.rpo_blocks[0].instructions[1];

  // 2b. Negative case: callee-saved `$s0` mutated in place AFTER an intervening `call_insn`
  //     represents a cross-call user variable and must NOT receive `REG_DEAD 16`.
  RtlControlFlowGraph across_call_cs_cfg;
  across_call_cs_cfg.pass = RtlPassKind::kSched2;
  across_call_cs_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 16 s0) (mem:SI (reg:SI 4 a0)))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"helper\"))) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SI 16 s0) (plus:SI (reg:SI 16 s0) (reg:SI 2 v0)))",
                    "(set (reg/i:SI 2 v0) (reg:SI 16 s0))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  NormalizePostReloadHardRegNotes(&across_call_cs_cfg, profile_o2);
  EXPECT_FALSE(TopLevelInsnHasNote(across_call_cs_cfg.rpo_blocks[0].instructions[2],
                                   RtlMachineMode::kRegDead, 16))
      << across_call_cs_cfg.rpo_blocks[0].instructions[2];

  // 3. Chained local-to-live-out RMW (`VariableShiftBitmaskCheckWithTableUpdate` pattern):
  //    `B0[1]` defines a local RMW killed by `B0[2]`, which itself is a live-out RMW into `B1`.
  //    Both `B0[1]` and `B0[2]` must receive `REG_DEAD 3`.
  RtlControlFlowGraph chained_cfg;
  chained_cfg.pass = RtlPassKind::kSched2;
  chained_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0)))",
                    "(set (reg:SI 3 v1) (and:SI (reg:SI 3 v1) (const_int 31)))",
                    "(set (reg:SI 3 v1) (ashift:SI (reg:SI 5 a1) (reg:SI 3 v1)))",
                    "(set (pc) (if_then_else (eq:SI (reg:SI 6 a2) (const_int 0)) "
                    "(label_ref L0) (pc)))",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (mem:SI (reg:SI 4 a0)) (reg:SI 3 v1))",
                },
                {2}),
      MakeBlock(2, true,
                {
                    "(set (reg/i:SI 2 v0) (reg:SI 3 v1))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  NormalizePostReloadHardRegNotes(&chained_cfg, profile_o2);
  EXPECT_TRUE(
      TopLevelInsnHasNote(chained_cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 3))
      << chained_cfg.rpo_blocks[0].instructions[1];
  EXPECT_TRUE(
      TopLevelInsnHasNote(chained_cfg.rpo_blocks[0].instructions[2], RtlMachineMode::kRegDead, 3))
      << chained_cfg.rpo_blocks[0].instructions[2];
}

TEST(ReconstructHardRegLivenessTest,
     PureEntryAbiArgRmwFeedingImmediateCallOmitsSelfDeadAndFallthroughDelaySlotTracksFtLive) {
  const CompilationProfile profile_o2{.optimize_level = 2, .c_flags = {"-O2", "-mips2"}};

  // 1a. Pure entry ABI arg `$a0` modified in place and immediately passed to a
  //     call (`flow.c:2507-2550` [`flow.c:2511`] / `sched.c:2872`): both source and destination are
  //     ABI-pinned to `$a0`, so neither direct ALU nor pointer dereference may
  //     carry `REG_DEAD (reg:SI 4 a0)`.
  RtlControlFlowGraph alu_call_cfg;
  alu_call_cfg.pass = RtlPassKind::kDbr;
  alu_call_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 5)))",
                    "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  alu_call_cfg.rpo_blocks[0].rtl_insns[1].has_explicit_call_fusage = true;
  alu_call_cfg.rpo_blocks[0].rtl_insns[1].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&alu_call_cfg, profile_o2);
  EXPECT_FALSE(
      TopLevelInsnHasNote(alu_call_cfg.rpo_blocks[0].instructions[0], RtlMachineMode::kRegDead, 4))
      << alu_call_cfg.rpo_blocks[0].instructions[0];

  RtlControlFlowGraph load_call_cfg;
  load_call_cfg.pass = RtlPassKind::kDbr;
  load_call_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (mem:SI (reg:SI 4 a0)))",
                    "(parallel[ (call (mem:SI (symbol_ref:SI (\"notify\"))) (const_int 16)) "
                    "(clobber (reg:SI 31 ra)) ] )",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  load_call_cfg.rpo_blocks[0].rtl_insns[1].has_explicit_call_fusage = true;
  load_call_cfg.rpo_blocks[0].rtl_insns[1].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&load_call_cfg, profile_o2);
  EXPECT_FALSE(
      TopLevelInsnHasNote(load_call_cfg.rpo_blocks[0].instructions[0], RtlMachineMode::kRegDead, 4))
      << load_call_cfg.rpo_blocks[0].instructions[0];

  // 1b. Contrast: when `$a0` was defined by a prior non-entry instruction in `B0`,
  //     that prior value lifts to a pseudo while the outgoing call arg stays `$a0`,
  //     so RMW `REG_DEAD 4` IS legitimately attached.
  RtlControlFlowGraph defined_call_cfg;
  defined_call_cfg.pass = RtlPassKind::kDbr;
  defined_call_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (mem:SI (reg:SI 5 a1)))",
                    "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 5)))",
                    "(parallel[ (call (mem:SI (symbol_ref:SI (\"notify\"))) (const_int 16)) "
                    "(clobber (reg:SI 31 ra)) ] )",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  defined_call_cfg.rpo_blocks[0].rtl_insns[2].has_explicit_call_fusage = true;
  defined_call_cfg.rpo_blocks[0].rtl_insns[2].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&defined_call_cfg, profile_o2);
  EXPECT_TRUE(TopLevelInsnHasNote(defined_call_cfg.rpo_blocks[0].instructions[1],
                                  RtlMachineMode::kRegDead, 4))
      << defined_call_cfg.rpo_blocks[0].instructions[1];

  // 2. Fallthrough-stolen non-annulled conditional-branch delay slot
  //    (`reorg.c:3375-3436`, `reorg.c:918-920`): `xor $v0, $v1, $a2` was stolen
  //    from single-pred fallthrough `B1` into `B0`'s branch delay slot (where
  //    `$v0` is read by the branch and dead on taken `B2`). Even though `$v1`
  //    and `$a2` remain live on `B2`, both die on `B1`, so the delay slot must
  //    carry two `(REG_DEAD (const_int 0))` notes matching `.11.dbr`.
  RtlControlFlowGraph ft_steal_cfg;
  ft_steal_cfg.pass = RtlPassKind::kDbr;
  ft_steal_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0)))",
                    "(set (reg:SI 5 a1) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))",
                    "(set (reg:SI 2 v0) (lt:SI (reg:SI 5 a1) (reg:SI 3 v1)))",
                    "(set (reg:SI 6 a2) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 8))))",
                    "(sequence[ (jump_insn (set (pc) (if_then_else (ne:SI (reg:SI 2 v0) "
                    "(const_int 0)) (label_ref L0) (pc)))) "
                    "(insn (set (reg:SI 2 v0) (xor:SI (reg:SI 3 v1) (reg:SI 6 a2)))) ] )",
                },
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SI 2 v0))",
                    "(set (reg/i:SI 2 v0) (reg:SI 5 a1))",
                    "(set (pc) (label_ref L1))",
                },
                {3}),
      MakeBlock(2, true,
                {
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2)))",
                },
                {3}),
      MakeBlock(3, true,
                {
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&ft_steal_cfg, profile_o2);
  const std::string& seq_str = ft_steal_cfg.rpo_blocks[0].instructions.back();
  EXPECT_NE(seq_str.find("(expr_list:REG_DEAD (const_int 0) "
                         "(expr_list:REG_DEAD (const_int 0) (nil)))"),
            std::string::npos)
      << seq_str;
}

TEST(ReconstructHardRegLivenessTest,
     SynthesizeStage0MemRegEquivNotesGuardsCrossBlockAndWarDelaySlots) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  // 1. Fallthrough-stolen branch delay-slot `REG_EQUIV` (`reorg.c:3280`,
  //    `local-alloc.c:992`): only synthesize `REG_EQUIV` when `B1`'s first
  //    active step reads the slot's defined reg (`IsStage0FirstFtStepReadsMask`).
  auto make_ft_slot_cfg = [](bool first_ft_step_reads_v1) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> b1_insns;
    if (first_ft_step_reads_v1) {
      b1_insns = {
          "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1)))",
          "(set (mem:SI (reg:SI 5 a1)) (reg:SI 2 v0))",
      };
    } else {
      b1_insns = {
          "(set (reg:SI 6 a2) (const_int 7))",
          "(set (reg:SI 2 v0) (plus:SI (reg:SI 3 v1) (reg:SI 6 a2)))",
      };
    }
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(sequence [(jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
                      "(const_int 0)) (label_ref L0) (pc)))) "
                      "(insn (set (reg:SI 3 v1) (mem:SI (symbol_ref:SI (\"g_counter\")))))])",
                  },
                  {2, 1}),
        MakeBlock(1, false, std::move(b1_insns), {2}),
        MakeBlock(2, true,
                  {
                      "(set (reg/i:SI 2 v0) (const_int 0))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    return cfg;
  };
  RtlControlFlowGraph ft_reads = make_ft_slot_cfg(/*first_ft_step_reads_v1=*/true);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&ft_reads, profile_o2);
  EXPECT_NE(ft_reads.rpo_blocks[0].instructions[0].find("REG_EQUIV (mem:SI (symbol_ref"),
            std::string::npos)
      << ft_reads.rpo_blocks[0].instructions[0];

  RtlControlFlowGraph ft_no_read = make_ft_slot_cfg(/*first_ft_step_reads_v1=*/false);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&ft_no_read, profile_o2);
  EXPECT_EQ(ft_no_read.rpo_blocks[0].instructions[0].find("REG_EQUIV"), std::string::npos)
      << ft_no_read.rpo_blocks[0].instructions[0];

  // 2. Bypassed symbolic-store Rule A WAR guard (`reorg.c:fill_simple_delay_slots`):
  //    when the delay-slot instruction `s_dead` overwrites `r` (`$v1`), it could
  //    never have preceded `(set (mem (symbol_ref ...)) (reg v1))`, so `REG_EQUIV`
  //    must be suppressed; contrast when `s_dead` only reads `$v1`.
  auto make_sym_store_cfg = [](bool slot_redefines_v1) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    const std::string slot_insn =
        slot_redefines_v1 ? "(insn (set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 5))))"
                          : "(insn (set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 5))))";
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                      "(set (mem:SI (symbol_ref:SI (\"g_seq\"))) (reg:SI 3 v1))",
                      "(sequence [(jump_insn (parallel[ (return) (use (reg:SI 31 ra)) ] )) " +
                          slot_insn + "])",
                  },
                  {}),
    };
    return cfg;
  };
  RtlControlFlowGraph war_slot = make_sym_store_cfg(/*slot_redefines_v1=*/true);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&war_slot, profile_o2);
  EXPECT_EQ(war_slot.rpo_blocks[0].instructions[0].find("REG_EQUIV"), std::string::npos)
      << war_slot.rpo_blocks[0].instructions[0];

  RtlControlFlowGraph read_only_slot = make_sym_store_cfg(/*slot_redefines_v1=*/false);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&read_only_slot, profile_o2);
  EXPECT_NE(read_only_slot.rpo_blocks[0].instructions[0].find("REG_EQUIV (mem:SI (symbol_ref"),
            std::string::npos)
      << read_only_slot.rpo_blocks[0].instructions[0];
}

TEST(ReconstructHardRegLivenessTest,
     SynthesizeStage0CrossJumpJoinStoreEquivMatchesJump2StructuralCriteria) {
  const CompilationProfile profile_o1 = MakeProfile(1, {"-O1", "-mips2", "-mcpu=r4000"});
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  auto make_two_pred_join_cfg = [](bool pred2_defines_v0, bool join_has_call_after_store) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> b2_insns;
    if (pred2_defines_v0) {
      b2_insns = {
          "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 1)))",
      };
    } else {
      b2_insns = {
          "(set (reg:SI 6 a2) (plus:SI (reg:SI 5 a1) (const_int 1)))",
      };
    }
    std::vector<std::string> b3_insns = {
        "(set (mem:SI (symbol_ref:SI (\"g_next\"))) (reg:SI 2 v0))",
    };
    if (join_has_call_after_store) {
      b3_insns.push_back(
          "(set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"helper_fn\"))) (const_int 0)))");
      b3_insns.push_back("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))");
    } else {
      b3_insns.push_back("(set (reg:SI 3 v1) (mem:SI (symbol_ref:SI (\"g_inv\"))))");
      b3_insns.push_back("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1)))");
    }
    b3_insns.push_back("(use (reg/i:SI 2 v0))");
    b3_insns.push_back("(parallel[ (return) (use (reg:SI 31 ra)) ] )");

    std::vector<std::string> b0_insns;
    if (!pred2_defines_v0) {
      b0_insns.push_back("(set (reg:SI 2 v0) (const_int 7))");
    }
    b0_insns.push_back(
        "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref L0) (pc)))");
    cfg.rpo_blocks = {
        MakeBlock(0, false, std::move(b0_insns), {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SI 2 v0) (const_int 1000))",
                      "(set (pc) (label_ref L1))",
                  },
                  {3}),
        MakeBlock(2, true, std::move(b2_insns), {3}),
        MakeBlock(3, true, std::move(b3_insns), {}),
    };
    return cfg;
  };

  // 1. Positive (`-O1`, `jump.c:1978` `minimum = 1`, 2 compatible predecessors,
  //    join store before `REG_EQUIV`-stopped load): both predecessors get `REG_EQUIV`.
  RtlControlFlowGraph o1_pos =
      make_two_pred_join_cfg(/*pred2_defines_v0=*/true, /*join_has_call_after_store=*/false);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o1_pos, profile_o1);
  EXPECT_NE(o1_pos.rpo_blocks[1].instructions[0].find("REG_EQUIV (mem:SI (symbol_ref"),
            std::string::npos)
      << o1_pos.rpo_blocks[1].instructions[0];
  EXPECT_NE(o1_pos.rpo_blocks[2].instructions[0].find("REG_EQUIV (mem:SI (symbol_ref"),
            std::string::npos)
      << o1_pos.rpo_blocks[2].instructions[0];

  // 2. Negative (`-O2`, `RunsSched2()`): length-1 store prefix before `REG_EQUIV`-stopped
  //    load is not cross-jump-split under `sched2`, so `$v0` stays multi-block.
  RtlControlFlowGraph o2_single_store =
      make_two_pred_join_cfg(/*pred2_defines_v0=*/true, /*join_has_call_after_store=*/false);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o2_single_store, profile_o2);
  EXPECT_EQ(o2_single_store.rpo_blocks[1].instructions[0].find("REG_EQUIV"), std::string::npos)
      << o2_single_store.rpo_blocks[1].instructions[0];
  EXPECT_EQ(o2_single_store.rpo_blocks[2].instructions[0].find("REG_EQUIV"), std::string::npos)
      << o2_single_store.rpo_blocks[2].instructions[0];

  // 3. Negative (`-O1`, incompatible predecessor): `B2` inherits `$v0` instead of defining it,
  //    so `find_cross_jump`/`InvertDoCrossJump` cannot merge matching `REG_EQUIV` across preds.
  RtlControlFlowGraph o1_incompat_pred =
      make_two_pred_join_cfg(/*pred2_defines_v0=*/false, /*join_has_call_after_store=*/false);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o1_incompat_pred, profile_o1);
  EXPECT_EQ(o1_incompat_pred.rpo_blocks[1].instructions[0].find("REG_EQUIV"), std::string::npos)
      << o1_incompat_pred.rpo_blocks[1].instructions[0];

  // 4. Positive (`-O2`, multi-insn store + `CALL_INSN` prefix): `effective_shared_len >= 2`
  //    with a `CALL_INSN` in the prefix is split even under `sched2`.
  RtlControlFlowGraph o2_store_call =
      make_two_pred_join_cfg(/*pred2_defines_v0=*/true, /*join_has_call_after_store=*/true);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o2_store_call, profile_o2);
  EXPECT_NE(o2_store_call.rpo_blocks[1].instructions[0].find("REG_EQUIV (mem:SI (symbol_ref"),
            std::string::npos)
      << o2_store_call.rpo_blocks[1].instructions[0];
  EXPECT_NE(o2_store_call.rpo_blocks[2].instructions[0].find("REG_EQUIV (mem:SI (symbol_ref"),
            std::string::npos)
      << o2_store_call.rpo_blocks[2].instructions[0];

  // 5. Negative (`-O1`, 3 predecessors, `jump.c:1993` `minimum = 2`): a length-1 store
  //    prefix before a `REG_EQUIV`-stopped load cannot satisfy `minimum = 2`.
  RtlControlFlowGraph o1_three_preds;
  o1_three_preds.pass = RtlPassKind::kDbr;
  o1_three_preds.rpo_blocks = {
      MakeBlock(0, false,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                 "(label_ref L0) (pc)))"},
                {2, 1}),
      MakeBlock(1, false,
                {
                    "(set (reg:SI 2 v0) (const_int 1000))",
                    "(set (pc) (label_ref L3))",
                },
                {5}),
      MakeBlock(2, true,
                {"(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                 "(label_ref L1) (pc)))"},
                {4, 3}),
      MakeBlock(3, false,
                {
                    "(set (reg:SI 2 v0) (const_int 2000))",
                    "(set (pc) (label_ref L3))",
                },
                {5}),
      MakeBlock(4, true,
                {
                    "(set (reg:SI 2 v0) (const_int 3000))",
                },
                {5}),
      MakeBlock(5, true,
                {
                    "(set (mem:SI (symbol_ref:SI (\"g_next\"))) (reg:SI 2 v0))",
                    "(set (reg:SI 3 v1) (mem:SI (symbol_ref:SI (\"g_inv\"))))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 3 v1) (const_int 1)))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o1_three_preds, profile_o1);
  EXPECT_EQ(o1_three_preds.rpo_blocks[1].instructions[0].find("REG_EQUIV"), std::string::npos)
      << o1_three_preds.rpo_blocks[1].instructions[0];
  EXPECT_EQ(o1_three_preds.rpo_blocks[3].instructions[0].find("REG_EQUIV"), std::string::npos)
      << o1_three_preds.rpo_blocks[3].instructions[0];
  EXPECT_EQ(o1_three_preds.rpo_blocks[4].instructions[0].find("REG_EQUIV"), std::string::npos)
      << o1_three_preds.rpo_blocks[4].instructions[0];
}

TEST(ReconstructHardRegLivenessTest,
     ResolvePostReloadConstZeroRegDeadNotesSkipsEarlierAttributedDeathInSameLiveRange) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  // Positive: `$a0`'s death in the current live range is already attributed to an
  // earlier pure reader (`flow.c:2485-2515`, `sched.c:3573` stale `REG_DEAD`), so
  // resolving `(REG_DEAD (const_int 0))` on the later store of `$a0` to `0($a1)`
  // must skip `$a0` and resolve strictly to `$a1`.
  RtlControlFlowGraph dedup_cfg;
  dedup_cfg.pass = RtlPassKind::kSched2;
  dedup_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (mem:SI (reg:SI 5 a1)) (reg:SI 4 a0)) "
                    "(expr_list:REG_DEAD (const_int 0) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ResolvePostReloadConstZeroRegDeadNotes(&dedup_cfg, profile_o2);
  EXPECT_NE(dedup_cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:SI 5 a1)"),
            std::string::npos)
      << dedup_cfg.rpo_blocks[0].instructions[1];
  EXPECT_EQ(dedup_cfg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:SI 4 a0)"),
            std::string::npos)
      << dedup_cfg.rpo_blocks[0].instructions[1];

  // Negative: when `$a0` is redefined after the earlier `REG_DEAD $a0`, the new
  // live range of `$a0` has no earlier attributed death and MUST resolve to `$a0`.
  RtlControlFlowGraph redef_cfg;
  redef_cfg.pass = RtlPassKind::kSched2;
  redef_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 1))) "
                    "(expr_list:REG_DEAD (reg:SI 4 a0) (nil))",
                    "(set (reg:SI 4 a0) (plus:SI (reg:SI 3 v1) (const_int 2))) "
                    "(expr_list:REG_DEAD (reg:SI 3 v1) (nil))",
                    "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 3))) "
                    "(expr_list:REG_DEAD (const_int 0) (nil))",
                    "(use (reg/i:SI 2 v0))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ResolvePostReloadConstZeroRegDeadNotes(&redef_cfg, profile_o2);
  EXPECT_NE(redef_cfg.rpo_blocks[0].instructions[2].find("REG_DEAD (reg:SI 4 a0)"),
            std::string::npos)
      << redef_cfg.rpo_blocks[0].instructions[2];
}

TEST(ReconstructHardRegLivenessTest, PureEntryAbiArgMemAddrRmwFeedingCrossJumpedCallOmitsSelfDead) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  // Positive: unmodified entry ABI arg `$a0` rewritten in place via a
  // `mem_addr_read` load `(set (reg:SI 4 a0) (mem:SI (plus:SI (reg:SI 4 a0) 4)))`
  // before an unconditional jump to a cross-jumped successor `call_insn` reading
  // `$a0` (`flow.c:2507-2550` [`flow.c:2511`], `sched.c:2872`): both source and
  // destination are ABI-pinned to `$a0`, so it must NOT fall through to Case B
  // and receive `REG_DEAD (reg:SI 4 a0)`.
  RtlControlFlowGraph entry_cross_jump_cfg;
  entry_cross_jump_cfg.pass = RtlPassKind::kDbr;
  entry_cross_jump_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))",
                    "(set (pc) (label_ref L0))",
                },
                {1}),
      MakeBlock(1, true,
                {
                    "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
                    "(clobber (reg:SI 31 ra)) ] )",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  entry_cross_jump_cfg.rpo_blocks[1].rtl_insns[0].has_explicit_call_fusage = true;
  entry_cross_jump_cfg.rpo_blocks[1].rtl_insns[0].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&entry_cross_jump_cfg, profile_o2);
  EXPECT_FALSE(TopLevelInsnHasNote(entry_cross_jump_cfg.rpo_blocks[0].instructions[0],
                                   RtlMachineMode::kRegDead, 4))
      << entry_cross_jump_cfg.rpo_blocks[0].instructions[0];

  // Negative contrast: when `$a0` was defined by a prior non-entry instruction
  // in `B0` before the `mem_addr_read` RMW feeding the cross-jumped call, the
  // incoming value is a local pseudo rather than an entry ABI arg, so `REG_DEAD 4`
  // IS legitimately attached.
  RtlControlFlowGraph defined_cross_jump_cfg;
  defined_cross_jump_cfg.pass = RtlPassKind::kDbr;
  defined_cross_jump_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 4 a0) (mem:SI (reg:SI 5 a1)))",
                    "(set (reg:SI 4 a0) (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))))",
                    "(set (pc) (label_ref L0))",
                },
                {1}),
      MakeBlock(1, true,
                {
                    "(parallel[ (call (mem:SI (symbol_ref:SI (\"callee\"))) (const_int 16)) "
                    "(clobber (reg:SI 31 ra)) ] )",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  defined_cross_jump_cfg.rpo_blocks[1].rtl_insns[0].has_explicit_call_fusage = true;
  defined_cross_jump_cfg.rpo_blocks[1].rtl_insns[0].call_fusage = {
      RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&defined_cross_jump_cfg, profile_o2);
  EXPECT_TRUE(TopLevelInsnHasNote(defined_cross_jump_cfg.rpo_blocks[0].instructions[1],
                                  RtlMachineMode::kRegDead, 4))
      << defined_cross_jump_cfg.rpo_blocks[0].instructions[1];
}

TEST(ReconstructHardRegLivenessTest, StripStage0Jump2NoopMoveConsumedDeathsOnPointerLoadRmw) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  auto make_cfg = [](RtlPassKind pass_kind) {
    RtlControlFlowGraph cfg;
    cfg.pass = pass_kind;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 3 v1) (const_int 0))",
                  },
                  {1}),
        MakeBlock(1, true,
                  {
                      "(set (reg:SI 3 v1) (plus:SI (reg:SI 3 v1) (const_int 1)))",
                      "(set (reg:SI 2 v0) (ashift:SI (reg:SI 3 v1) (const_int 2)))",
                      "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))",
                      "(set (reg:SI 2 v0) (mem:SI (reg:SI 2 v0)))",
                      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (reg:SI 5 a1)) "
                      "(label_ref L1) (pc)))",
                  },
                  {3, 2}),
        MakeBlock(
            2, false,
            {
                pass_kind == RtlPassKind::kDbr ? "(sequence [(jump_insn (set (pc) (label_ref L2))) "
                                                 "(insn (set (reg/i:SI 2 v0) (reg:SI 3 v1)))])"
                                               : "(set (reg/i:SI 2 v0) (reg:SI 3 v1))",
            },
            {5}),
        MakeBlock(3, true,
                  {
                      "(set (pc) (if_then_else (ge:SI (reg:SI 2 v0) (const_int 0)) "
                      "(label_ref L0) (pc)))",
                  },
                  {1, 4}),
        MakeBlock(4, false, {"(set (reg/i:SI 2 v0) (const_int -1))"}, {5}),
        MakeBlock(5, true,
                  {
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    if (pass_kind != RtlPassKind::kDbr) {
      cfg.rpo_blocks[2].AddPatternOnly("(set (pc) (label_ref L2))");
    }
    return cfg;
  };

  // Positive (`kDbr`, post-`jump2`): `jump.c:452-459` strips `REG_DEAD 2 v0`
  // from `B1[3]` `(set (reg:SI 2 v0) (mem:SI (reg:SI 2 v0)))` because `B1[4]`
  // is a live branch reading `$v0` before label-less fallthrough `B2` setting
  // `$v0 = $v1` (in an unconditional jump delay slot).
  RtlControlFlowGraph dbr_cfg = make_cfg(RtlPassKind::kDbr);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&dbr_cfg, profile_o2);
  EXPECT_FALSE(
      TopLevelInsnHasNote(dbr_cfg.rpo_blocks[1].instructions[3], RtlMachineMode::kRegDead, 2))
      << dbr_cfg.rpo_blocks[1].instructions[3];

  // Negative (`kSched2`, pre-`jump2`): before `jump2` runs (`toplev.c:3140-3143`),
  // `B1[3]` retains its `REG_DEAD 2 v0` note.
  RtlControlFlowGraph sched2_cfg = make_cfg(RtlPassKind::kSched2);
  NormalizePostReloadHardRegNotes(&sched2_cfg, profile_o2);
  EXPECT_TRUE(
      TopLevelInsnHasNote(sched2_cfg.rpo_blocks[1].instructions[3], RtlMachineMode::kRegDead, 2))
      << sched2_cfg.rpo_blocks[1].instructions[3];
}

TEST(ReconstructHardRegLivenessTest,
     SelfRmwGlobalInLocalOutSkipsSplitWhenLowerRegAllocOrderGprFree) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2"});

  // Positive (`local-alloc.c:1762-1765, 2106-2181`, `#ifndef REG_ALLOC_ORDER` at
  // `2178`, `config/mips/mips.h:1192, 1203`):
  // `$3 v1` enters `B1` as an unpinned global-in pseudo and undergoes a self-RMW
  // `(set (reg:SI 3 v1) (ior:SI (reg:SI 3 v1) (const_int 65536)))` followed by a
  // local store `(set (mem:SI (reg:SI 4 a0)) (reg:SI 3 v1))`. Because `$2 v0`
  // precedes `$3 v1` in MIPS's default GPR allocation order and is completely
  // free across the local-out tail, `local-alloc` would assign a split local
  // output pseudo to `$2 v0` (never `$3 v1`); Stage 0 must NOT attach a spurious
  // `(REG_DEAD (reg:SI 3 v1))` to the RMW.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0)))",
                      "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                      "(label_ref L1) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SI 3 v1) (ior:SI (reg:SI 3 v1) (const_int 65536)))",
                      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 3 v1))",
                  },
                  {2}),
        MakeBlock(2, true,
                  {
                      "(set (reg/i:SI 2 v0) (mem:SI (reg:SI 4 a0)))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    NormalizePostReloadHardRegNotes(&cfg, profile_o2);
    EXPECT_FALSE(
        TopLevelInsnHasNote(cfg.rpo_blocks[1].instructions[0], RtlMachineMode::kRegDead, 3))
        << cfg.rpo_blocks[1].instructions[0];
  }

  // Negative contrast: when `$2 v0` IS live across the local-out tail (used
  // after the store to compute the return value), `find_free_reg` skips `$2 v0`
  // and assigns `$3 v1`, so Stage 0 DOES attach `(REG_DEAD (reg:SI 3 v1))` to
  // the global-in -> local-out self-RMW.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 3 v1) (mem:SI (reg:SI 4 a0)))",
                      "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 7)))",
                      "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                      "(label_ref L1) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SI 3 v1) (ior:SI (reg:SI 3 v1) (const_int 65536)))",
                      "(set (mem:SI (reg:SI 4 a0)) (reg:SI 3 v1))",
                      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))",
                  },
                  {2}),
        MakeBlock(2, true,
                  {
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    NormalizePostReloadHardRegNotes(&cfg, profile_o2);
    EXPECT_TRUE(TopLevelInsnHasNote(cfg.rpo_blocks[1].instructions[0], RtlMachineMode::kRegDead, 3))
        << cfg.rpo_blocks[1].instructions[0];
  }
}

TEST(ReconstructHardRegLivenessTest,
     SynthesizesStoreRegEquivAcrossChainedPureFallthroughStoreTails) {
  const CompilationProfile profile_o2 = {
      .optimize_level = 2,
      .c_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-O2"},
  };
  auto build_cfg = [](bool bare_return_exit, bool load_in_b1) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) "
                   "(label_ref L0) (pc)))"},
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      load_in_b1 ? "(set (reg:QI 2 v0) (mem:QI (symbol_ref:SI (\"g_ext\"))))"
                                 : "(set (reg:QI 2 v0) (const_int 199))",
                      "(sequence [ (jump_insn 0 0 0 (set (pc) (label_ref L2))) "
                      "(insn 0 0 0 (set (mem:QI (symbol_ref:SI (\"g_c2\"))) (reg:QI 2 v0))) ])",
                  },
                  {5}),
        MakeBlock(2, true,
                  {"(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 1)) "
                   "(label_ref L1) (pc)))"},
                  {4, 3}),
        MakeBlock(3, false,
                  {
                      "(sequence [ (jump_insn 0 0 0 (set (pc) (label_ref L3))) "
                      "(insn 0 0 0 (set (reg:QI 2 v0) (const_int 193))) ])",
                  },
                  {6}),
        MakeBlock(4, true,
                  {
                      "(set (reg:QI 2 v0) (const_int 194))",
                  },
                  {5}),
        MakeBlock(5, true,
                  {
                      "(set (mem:QI (symbol_ref:SI (\"g_c3\"))) (reg:QI 2 v0))",
                      "(set (reg:QI 2 v0) (const_int 196))",
                      "(set (mem:QI (symbol_ref:SI (\"g_c4\"))) (reg:QI 2 v0))",
                      "(set (reg:QI 2 v0) (const_int 197))",
                  },
                  {6}),
        MakeBlock(6, true,
                  {
                      "(set (mem:QI (symbol_ref:SI (\"g_c5\"))) (reg:QI 2 v0))",
                  },
                  {7}),
        MakeBlock(
            7, true,
            {
                bare_return_exit ? "(return)" : "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
            },
            {}),
    };
    return cfg;
  };

  // 1. Positive (`local-alloc.c:990-1000`, `jump.c:1978, 2371-2560`, `sched.c:1488, 2428`):
  //    Stage 0 attaches `REG_EQUIV (mem:QI "g_c3")` onto `B1[0]` and `B4[0]` (even with
  //    intermediate non-aliasing store `g_c2` in `B1`'s delay slot) and `REG_EQUIV (mem:QI "g_c5")`
  //    onto `B3`'s delay slot and `B5[3]`.
  {
    RtlControlFlowGraph cfg = build_cfg(/*bare_return_exit=*/false, /*load_in_b1=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    EXPECT_NE(
        cfg.rpo_blocks[1].instructions[0].find("REG_EQUIV (mem:QI (symbol_ref:SI (\"g_c3\"))"),
        std::string::npos)
        << cfg.rpo_blocks[1].instructions[0];
    EXPECT_NE(
        cfg.rpo_blocks[4].instructions[0].find("REG_EQUIV (mem:QI (symbol_ref:SI (\"g_c3\"))"),
        std::string::npos)
        << cfg.rpo_blocks[4].instructions[0];
    EXPECT_NE(
        cfg.rpo_blocks[3].instructions[0].find("REG_EQUIV (mem:QI (symbol_ref:SI (\"g_c5\"))"),
        std::string::npos)
        << cfg.rpo_blocks[3].instructions[0];
    EXPECT_NE(
        cfg.rpo_blocks[5].instructions[3].find("REG_EQUIV (mem:QI (symbol_ref:SI (\"g_c5\"))"),
        std::string::npos)
        << cfg.rpo_blocks[5].instructions[3];
  }

  // 2. Negative contrast (bare `(return)` in `B7`): `B6` is not a chained cross-jumpable tail,
  //    so `B3` and `B5[3]` must NOT receive `REG_EQUIV (mem:QI "g_c5")`.
  {
    RtlControlFlowGraph cfg = build_cfg(/*bare_return_exit=*/true, /*load_in_b1=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    EXPECT_EQ(cfg.rpo_blocks[3].instructions[0].find("REG_EQUIV"), std::string::npos)
        << cfg.rpo_blocks[3].instructions[0];
    EXPECT_EQ(cfg.rpo_blocks[5].instructions[3].find("REG_EQUIV"), std::string::npos)
        << cfg.rpo_blocks[5].instructions[3];
  }

  // 3. Negative contrast (multi-cycle load in `B1` under `-O2` `sched2`): `B5` is not
  //    unit-latency-safe across predecessors, so `B4[0]` must NOT receive `REG_EQUIV "g_c3"`.
  {
    RtlControlFlowGraph cfg = build_cfg(/*bare_return_exit=*/false, /*load_in_b1=*/true);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    EXPECT_EQ(cfg.rpo_blocks[4].instructions[0].find("REG_EQUIV"), std::string::npos)
        << cfg.rpo_blocks[4].instructions[0];
  }
}

TEST(ReconstructHardRegLivenessTest,
     NormalizeStage0FprOutputReloadStripsDeadNoteAcrossPrologueSaveAndInheritedReader) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});
  const CompilationProfile profile_o1 = MakeProfile(1, {"-O1", "-mips2", "-mcpu=r4000"});

  auto make_cfg = [](bool clobber_fpr_between) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> insns = {
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -80)))",
        "(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 62 $f30)))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 17 s1))",
        clobber_fpr_between ? "(set (reg:SF 32 $f0) (mem/u:SF (reg:SI 4 a0)))"
                            : "(set (reg:SF 44 $f12) (plus:SF (reg:SF 54 $f22) (reg:SF 32 $f0)))",
        "(set (reg:SF 17 s1) (reg:SF 32 $f0))",
        "(set (reg/i:SF 32 $f0) (const_double:SF (cc0) 0 1071644672))",
        "(set (reg:SI 17 s1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))))",
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 80)))",
        "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
    };
    cfg.rpo_blocks = {MakeBlock(0, false, std::move(insns), {})};
    return cfg;
  };

  // Positive (`toplev.c:3103`, `reload1.c:5186-5249, 5641`): under `-O2`
  // (`RunsSched2()`), `mfc1 s1,$f0` separated from `add.s $f0,$f0,$f30` by a
  // block-0 prologue save `sw $s1,20($sp)` and pure FP reader `add.s $f12`
  // must have `(REG_DEAD $f0)` stripped.
  {
    RtlControlFlowGraph cfg = make_cfg(/*clobber_fpr_between=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[4].find("REG_DEAD"), std::string::npos)
        << cfg.rpo_blocks[0].instructions[4];
  }

  // Negative 1: when an intervening insn overwrites `$f0` with a load,
  // `mfc1 s1,$f0` is not an FP-op output reload and keeps `(REG_DEAD $f0)`.
  {
    RtlControlFlowGraph cfg = make_cfg(/*clobber_fpr_between=*/true);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[4].find("REG_DEAD (reg:SF 32 $f0)"), std::string::npos)
        << cfg.rpo_blocks[0].instructions[4];
  }

  // Negative 2: at `-O1` (`!RunsSched2()`), non-adjacent `mfc1 s1,$f0` across
  // an intervening pure FP reader keeps `(REG_DEAD $f0)`.
  {
    RtlControlFlowGraph cfg = make_cfg(/*clobber_fpr_between=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o1);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[4].find("REG_DEAD (reg:SF 32 $f0)"), std::string::npos)
        << cfg.rpo_blocks[0].instructions[4];
  }

  // Negative 3 (`cse.c:481-486`): when `dst_gpr` is an outgoing call-arg reg
  // `$a1` (`5`) read by a `CALL_INSN` and a pure FP reader consumed `$f0`
  // before `mfc1 $a1,$f0`, `$f0` is a real multi-use `FP_REGS` pseudo copied
  // to `$a1` (not an `"=f"` output reload into `$a1`), so `mfc1 $a1,$f0` must
  // preserve `(REG_DEAD $f0)`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> insns = {
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 31 ra))",
        "(set (reg:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 36 $f4)))",
        "(set (reg:SF 62 $f30) (plus:SF (reg:SF 62 $f30) (reg:SF 32 $f0)))",
        "(set (reg:SF 5 a1) (reg:SF 32 $f0))",
        "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"SinkF\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
        "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 62 $f30)))",
        "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))",
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 32)))",
        "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
    };
    cfg.rpo_blocks = {MakeBlock(0, false, std::move(insns), {})};
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[4].find("REG_DEAD (reg:SF 32 $f0)"), std::string::npos)
        << cfg.rpo_blocks[0].instructions[4];
  }
}

TEST(ReconstructHardRegLivenessTest,
     RefineCallArgumentFusagePrunesConsumedScratchArgAcrossMultiCallDirectCallee) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  auto build_cfg = [](bool consume_a2_before_call1, bool define_unread_a3_before_call1) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> insns = {
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -32)))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SI 31 ra))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))) (reg:SI 16 s0))",
        "(set (reg:SI 6 a2) (mem:SI (reg:SI 4 a0)))",
    };
    if (consume_a2_before_call1) {
      insns.push_back("(set (reg:SI 16 s0) (xor:SI (reg:SI 6 a2) (const_int 37)))");
    }
    if (define_unread_a3_before_call1) {
      insns.push_back("(set (reg:SI 7 a3) (const_int 99))");
    }
    insns.push_back("(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))");
    insns.push_back("(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 2)))");
    insns.push_back(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"TransformStage\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    insns.push_back("(set (reg:SI 4 a0) (xor:SI (reg:SI 2 v0) (reg:SI 16 s0)))");
    insns.push_back("(set (reg:SI 5 a1) (plus:SI (reg:SI 2 v0) (const_int 3)))");
    insns.push_back(
        "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"TransformStage\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    insns.push_back("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 16 s0)))");
    insns.push_back("(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 24))))");
    insns.push_back("(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 28))))");
    insns.push_back("(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 32)))");
    insns.push_back("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks = {MakeBlock(0, false, std::move(insns), {})};
    return cfg;
  };

  auto has_call_fusage_reg = [](const RtlInsn& insn, int regno) {
    for (const RtlExpr& u : insn.call_fusage) {
      if (u.op == RtlOp::kUse && u.children.size() == 1 && u.children[0].op == RtlOp::kReg &&
          u.children[0].RegNo() == regno) {
        return true;
      }
    }
    return false;
  };

  // Positive (`calls.c:479`, `config/mips/mips.c:2850`, `local-alloc.c:2158`):
  // `$a2` was consumed by `xor $s0, $a2, 37` before Call 1 to `TransformStage`
  // and is not even defined at Call 2 to `TransformStage`; Call 1 must drop `$a2`
  // while preserving `$a0` and `$a1`.
  {
    RtlControlFlowGraph cfg =
        build_cfg(/*consume_a2_before_call1=*/true, /*define_unread_a3_before_call1=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    const RtlInsn& call1 = cfg.rpo_blocks[0].rtl_insns[7];
    EXPECT_TRUE(has_call_fusage_reg(call1, 4));
    EXPECT_TRUE(has_call_fusage_reg(call1, 5));
    EXPECT_FALSE(has_call_fusage_reg(call1, 6));
  }

  // Negative 1: when `$a2` is NOT consumed before Call 1 (`cur_unread` set),
  // Call 1 keeps `$a2` as an outgoing argument.
  {
    RtlControlFlowGraph cfg =
        build_cfg(/*consume_a2_before_call1=*/false, /*define_unread_a3_before_call1=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    const RtlInsn& call1 = cfg.rpo_blocks[0].rtl_insns[6];
    EXPECT_TRUE(has_call_fusage_reg(call1, 4));
    EXPECT_TRUE(has_call_fusage_reg(call1, 5));
    EXPECT_TRUE(has_call_fusage_reg(call1, 6));
  }

  // Negative 2: when `$a3` is also live/unread at Call 1, O32 argument prefix
  // closure (`close_o32_arg_prefix`) preserves `$a2` between `$a1` and `$a3`.
  {
    RtlControlFlowGraph cfg =
        build_cfg(/*consume_a2_before_call1=*/true, /*define_unread_a3_before_call1=*/true);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    const RtlInsn& call1 = cfg.rpo_blocks[0].rtl_insns[8];
    EXPECT_TRUE(has_call_fusage_reg(call1, 4));
    EXPECT_TRUE(has_call_fusage_reg(call1, 5));
    EXPECT_TRUE(has_call_fusage_reg(call1, 6));
    EXPECT_TRUE(has_call_fusage_reg(call1, 7));
  }
}

TEST(ReconstructHardRegLivenessTest,
     Stage0LocalScratchDeadNoteCountsCondBranchHeadReadAfterDelaySlotOverwrite) {
  const CompilationProfile profile_mips1 = MakeProfile(2, {"-O2", "-mips1", "-Wa,-O2"});

  // Positive (`reorg.c:3258-3306` `fill_slots_from_thread`): in a non-annulled
  // `-mips1` conditional-branch `SEQUENCE`, the delay slot overwrites `$v0 (2)`
  // (from the fallthrough path) while the branch head reads `$v0` (`steps[k]`
  // precedes `steps[k+1]`). Stage 0 must still observe the branch head's read
  // of `$v0` before stopping at the delay-slot overwrite, and attach self-RMW
  // `(REG_DEAD (reg:SI 2 v0))` to the earlier `$v0` load-and-shift RMW.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 2 v0) (mem/s:SI (symbol_ref:SI (\"g_val\"))))",
                      "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))",
                      "(sequence[ (jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 2 v0) "
                      "(const_int 0)) (label_ref L1) (pc)))) "
                      "(insn (set (reg:SI 2 v0) (const_int 7))) ] )",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
        MakeBlock(2, true,
                  {
                      "(set (reg/i:SI 2 v0) (const_int 1))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    NormalizePostReloadHardRegNotes(&cfg, profile_mips1);
    EXPECT_TRUE(TopLevelInsnHasNote(cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 2))
        << cfg.rpo_blocks[0].instructions[1];
  }

  // Negative: when the delay slot overwrites `$v0` while the branch head tests
  // `$a0` (not `$v0`), `steps[k + 1]` does NOT read `$v0` (`saw_read == false`),
  // so the earlier `$v0` RMW must NOT receive a spurious self-`REG_DEAD $v0`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 2 v0) (mem/s:SI (symbol_ref:SI (\"g_val\"))))",
                      "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))",
                      "(sequence[ (jump_insn (set (pc) (if_then_else (eq:SI (reg:SI 4 a0) "
                      "(const_int 0)) (label_ref L1) (pc)))) "
                      "(insn (set (reg:SI 2 v0) (const_int 7))) ] )",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
        MakeBlock(2, true,
                  {
                      "(set (reg/i:SI 2 v0) (const_int 1))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    NormalizePostReloadHardRegNotes(&cfg, profile_mips1);
    EXPECT_FALSE(
        TopLevelInsnHasNote(cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 2))
        << cfg.rpo_blocks[0].instructions[1];
  }
}

TEST(ReconstructHardRegLivenessTest,
     EarlyReturnCondBranchInBlockReadSuppressesReturnValueIntegratedFlag) {
  const CompilationProfile profile_mips1 = MakeProfile(2, {"-O2", "-mips1", "-Wa,-O2"});

  // Positive (`stmt.c:2590`, `jump.c:308, 427-433`): in a conditional-branch
  // early-return block `B0 -> {B2(exit), B1(other)}` where `$f0 (32)` is dead on
  // `B1` and read inside `B0` by `c.eq.s` before the branch, the producer
  // `(set (reg:SF 32 $f0) ...)` computed a pseudo allocated to `$f0` (not the
  // `/i` return copy, which was deleted as a noop move) and must NOT carry `/i`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SF 32 $f0) (mem:SF (reg:SI 4 a0)))",
                      "(set (reg:CC_FP 67 $fcr31) (eq:CC_FP (reg:SF 32 $f0) (reg:SF 44 $f12)))",
                      "(set (pc) (if_then_else (eq:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0)) "
                      "(label_ref L1) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))",
                  },
                  {2}),
        MakeBlock(2, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
    };
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_mips1);
    EXPECT_FALSE(InsnHasReturnValueSet(cfg.rpo_blocks[0].instructions[0], 32, RtlMachineMode::kSf))
        << cfg.rpo_blocks[0].instructions[0];
  }

  // Negative: when `$f0` is NOT read inside `B0` before the branch (the branch
  // compares `$a0` instead), `(set (reg/i:SF 32 $f0) ...)` feeds the early return
  // directly and DOES carry `/i`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SF 32 $f0) (mem:SF (reg:SI 4 a0)))",
                      "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) "
                      "(label_ref L1) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SF 32 $f0) (plus:SF (reg:SF 44 $f12) (reg:SF 46 $f14)))",
                  },
                  {2}),
        MakeBlock(2, true, {"(parallel[ (return) (use (reg:SI 31 ra)) ] )"}, {}),
    };
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_mips1);
    EXPECT_TRUE(InsnHasReturnValueSet(cfg.rpo_blocks[0].instructions[0], 32, RtlMachineMode::kSf))
        << cfg.rpo_blocks[0].instructions[0];
  }
}

TEST(ReconstructHardRegLivenessTest,
     CallDelaySlotRmwDeadNoteAndLiveGprCrossBankFprReloadResolution) {
  const CompilationProfile profile_mips1 = MakeProfile(2, {"-O2", "-mips1", "-Wa,-O2"});

  // Part 1 (`reorg.c:2889-3025` + `reload1.c:5641-6155`):
  // In a `call_insn` delay slot, a self-RMW `(set (reg:SF 62 $f30) (plus:SF
  // (reg:SF 62 $f30) (reg:SF 40 $f8)))` where `$f8` was loaded by a cross-bank
  // `"f"` input reload `(set (reg:SF 40 $f8) (reg:SF 16 s0))` from a still-live
  // `$s0` (no `REG_DEAD $s0`) must receive `(REG_DEAD (const_int 0))` in Stage 0
  // (`IsLiveGprCrossBankFprReloadStep` + `TryAttachStage0LoadOrLocalScratchDeadNote`
  // allowing `is_call_delay_slot` reading call-clobbered `$f8`), and
  // `ResolvePostReloadConstZeroRegDeadNotes` must resolve `(const_int 0)` to
  // `(reg:SF 62 $f30)` rather than `$f8` (positive).
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks = {
        MakeBlock(
            0, false,
            {
                "(set (reg:SF 62 $f30) (mem/u:SF (reg:SI 4 a0)))",
                "(set (reg:SF 16 s0) (mem/u:SF (reg:SI 5 a1)))",
                "(set (reg:SF 40 $f8) (reg:SF 16 s0))",
                "(sequence[ (call_insn (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI "
                "(\"callee\"))) (const_int 16)))) "
                "(insn (set (reg:SF 62 $f30) (plus:SF (reg:SF 62 $f30) (reg:SF 40 $f8)))) ] )",
                "(set (mem:SF (reg:SI 4 a0)) (reg:SF 16 s0))",
                "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 62 $f30)))",
                "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
            },
            {}),
    };
    NormalizePostReloadHardRegNotes(&cfg, profile_mips1);
    const std::string& seq_str = cfg.rpo_blocks[0].instructions[3];
    // The delay slot must carry `(REG_DEAD (const_int 0))` for the `$f30` self-RMW
    // (NOT zero notes and NOT two notes from miscounting `$f8`).
    const size_t first_zero = seq_str.find("REG_DEAD (const_int 0)");
    EXPECT_NE(first_zero, std::string::npos) << seq_str;
    if (first_zero != std::string::npos) {
      EXPECT_EQ(seq_str.find("REG_DEAD (const_int 0)", first_zero + 1), std::string::npos)
          << seq_str;
    }

    // After unpacking the delay slot into `.10.sched2`, `ResolvePostReloadConstZeroRegDeadNotes`
    // must resolve `(const_int 0)` to `(reg:SF 62 $f30)` instead of reload scratch `$f8`.
    RtlControlFlowGraph unpacked;
    unpacked.pass = RtlPassKind::kSched2;
    unpacked.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SF 62 $f30) (mem/u:SF (reg:SI 4 a0)))",
                      "(set (reg:SF 16 s0) (mem/u:SF (reg:SI 5 a1)))",
                      "(set (reg:SF 40 $f8) (reg:SF 16 s0))",
                      "(set (reg:SF 62 $f30) (plus:SF (reg:SF 62 $f30) (reg:SF 40 $f8))) "
                      "(expr_list:REG_DEAD (const_int 0) (nil))",
                      "(set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                      "(const_int 16)))",
                      "(set (mem:SF (reg:SI 4 a0)) (reg:SF 16 s0))",
                      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 62 $f30)))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    ResolvePostReloadConstZeroRegDeadNotes(&unpacked, profile_mips1);
    EXPECT_TRUE(
        TopLevelInsnHasNote(unpacked.rpo_blocks[0].instructions[3], RtlMachineMode::kRegDead, 62))
        << unpacked.rpo_blocks[0].instructions[3];
    EXPECT_FALSE(
        TopLevelInsnHasNote(unpacked.rpo_blocks[0].instructions[3], RtlMachineMode::kRegDead, 40))
        << unpacked.rpo_blocks[0].instructions[3];
  }

  // Negative contrast: when `$s0` IS dead at the FP consumer (no post-call read
  // of `$s0`, and `mtc1` has no `REG_DEAD $s0`), `ResolvePostReloadConstZeroRegDeadNotes`
  // resolves `(const_int 0)` to proxy GPR `(reg:SF 16 s0)`.
  {
    RtlControlFlowGraph unpacked;
    unpacked.pass = RtlPassKind::kSched2;
    unpacked.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SF 62 $f30) (mem/u:SF (reg:SI 4 a0)))",
                      "(set (reg:SF 16 s0) (mem/u:SF (reg:SI 5 a1)))",
                      "(set (reg:SF 40 $f8) (reg:SF 16 s0))",
                      "(set (reg:SF 62 $f30) (plus:SF (reg:SF 62 $f30) (reg:SF 40 $f8))) "
                      "(expr_list:REG_DEAD (const_int 0) (nil))",
                      "(set (reg/i:SF 32 $f0) (reg:SF 62 $f30))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    ResolvePostReloadConstZeroRegDeadNotes(&unpacked, profile_mips1);
    EXPECT_TRUE(
        TopLevelInsnHasNote(unpacked.rpo_blocks[0].instructions[3], RtlMachineMode::kRegDead, 16))
        << unpacked.rpo_blocks[0].instructions[3];
  }

  // Negative contrast 2 (`reorg.c:2889-3025` + `flow.c:2485-2515`):
  // In a `call_insn` delay slot, a callee-saved `DFmode` `reg/v` accumulator
  // (`l.d $f22 -> add.d $f22, $f22, $f0 -> mul.d $f22, $f22, $f20`) undergoing a
  // subsequent non-dead in-place RMW after the call must NOT receive a spurious
  // `(REG_DEAD (const_int 0))`, and an earlier multi-word `(REG_DEAD (reg:DF 32 $f0))`
  // must mark both `$f0` and `$f1` (`33`) as already dead so `(const_int 0)` never
  // resolves to `(reg:SI 33 $f1)`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks = {
        MakeBlock(
            0, false,
            {
                "(set (reg:DF 54 $f22) (mem/u:DF (reg:SI 5 a1)))",
                "(set (reg:DF 32 $f0) (mem/u:DF (plus:SI (reg:SI 5 a1) (const_int 8))))",
                "(set (reg:DF 6 a2) (reg:DF 32 $f0)) (expr_list:REG_DEAD (reg:DF 32 $f0) "
                "(nil))",
                "(sequence[ (call_insn (set (reg:DF 32 $f0) (call (mem:SI (symbol_ref:SI "
                "(\"callee\"))) (const_int 16)))) "
                "(insn (set (reg:DF 54 $f22) (plus:DF (reg:DF 54 $f22) (reg:DF 32 $f0)))) ] )",
                "(set (reg:DF 54 $f22) (mult:DF (reg:DF 54 $f22) (reg:DF 52 $f20)))",
                "(set (reg/i:DF 32 $f0) (plus:DF (reg:DF 32 $f0) (reg:DF 54 $f22)))",
                "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
            },
            {}),
    };
    NormalizePostReloadHardRegNotes(&cfg, profile_mips1);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[3].find("REG_DEAD (const_int 0)"), std::string::npos)
        << cfg.rpo_blocks[0].instructions[3];

    // Directly pin `already_dead_earlier` full-`RegBitMask` span check: even if
    // a `(const_int 0)` placeholder is present on the post-`REG_DEAD (reg:DF 32 $f0)`
    // insn, `$f1` (`33`) is already covered by `(reg:DF 32 $f0)` and must not
    // absorb it as `(REG_DEAD (reg:SI 33 $f1))`.
    RtlControlFlowGraph unpacked_df;
    unpacked_df.pass = RtlPassKind::kSched2;
    unpacked_df.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:DF 54 $f22) (mem/u:DF (reg:SI 5 a1)))",
                      "(set (reg:DF 32 $f0) (mem/u:DF (plus:SI (reg:SI 5 a1) (const_int 8))))",
                      "(set (reg:DF 6 a2) (reg:DF 32 $f0)) "
                      "(expr_list:REG_DEAD (reg:DF 32 $f0) (nil))",
                      "(set (reg:DF 54 $f22) (plus:DF (reg:DF 54 $f22) (reg:DF 32 $f0))) "
                      "(expr_list:REG_DEAD (const_int 0) (nil))",
                      "(set (reg:DF 54 $f22) (mult:DF (reg:DF 54 $f22) (reg:DF 52 $f20)))",
                      "(set (reg/i:DF 32 $f0) (plus:DF (reg:DF 32 $f0) (reg:DF 54 $f22)))",
                  },
                  {}),
    };
    ResolvePostReloadConstZeroRegDeadNotes(&unpacked_df, profile_mips1);
    EXPECT_FALSE(TopLevelInsnHasNote(unpacked_df.rpo_blocks[0].instructions[3],
                                     RtlMachineMode::kRegDead, 33))
        << unpacked_df.rpo_blocks[0].instructions[3];
  }
}

TEST(ReconstructHardRegLivenessTest, Sched2HoistedIndependentLoadForwardClosurePreservesMemEquiv) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips1", "-Wa,-O2"});

  // Mirrors `sched.c:1685-1703, 1854-1874` (`IsStage0Sched2HoistedIndependentLoadKillingAddr`):
  // `$f2 = mem/u[v1+16]` is followed by a hoisted independent load `$f20 = mem/u[v1+24]`
  // killing `$v1` AND its pure register consumer `$f20 = $f20 + $f24` before `$f2` dies.
  // Positive: because `$f20 = $f20 + $f24` is in the hoisted load's forward
  // closure and independent of `dead_s`, `$f2 = mem/u[v1+16]` receives `REG_EQUIV`.
  auto make_cfg = [](bool dead_s_reads_f20, bool closure_has_store) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> insns = {
        "(set (reg:SI 3 v1) (reg:SI 4 a0))",
        "(set (reg:SF 34 $f2) (mem/u:SF (plus:SI (reg:SI 3 v1) (const_int 16))))",
        "(set (reg:SF 52 $f20) (mem/u:SF (plus:SI (reg:SI 3 v1) (const_int 24))))",
        "(set (reg:SF 52 $f20) (plus:SF (reg:SF 52 $f20) (reg:SF 56 $f24)))",
    };
    if (closure_has_store) {
      insns.push_back("(set (mem:SF (reg:SI 5 a1)) (reg:SF 52 $f20))");
    }
    if (dead_s_reads_f20) {
      insns.push_back("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 34 $f2) (reg:SF 52 $f20)))");
    } else {
      insns.push_back("(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 34 $f2) (reg:SF 44 $f12)))");
      insns.push_back("(set (mem:SF (reg:SI 6 a2)) (reg:SF 52 $f20))");
    }
    insns.push_back("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks = {MakeBlock(0, false, std::move(insns), {})};
    return cfg;
  };

  // Positive: pure register consumer in closure, `dead_s` independent of `$f20`.
  {
    RtlControlFlowGraph cfg = make_cfg(/*dead_s_reads_f20=*/false, /*closure_has_store=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    NormalizePostReloadHardRegNotes(&cfg, profile_o2);
    EXPECT_NE(cfg.rpo_blocks[0].instructions[1].find("REG_EQUIV (mem/u:SF "), std::string::npos)
        << cfg.rpo_blocks[0].instructions[1];
  }
  // Negative 1: `dead_s` reads `$f20` (entering the hoisted load's forward closure).
  {
    RtlControlFlowGraph cfg = make_cfg(/*dead_s_reads_f20=*/true, /*closure_has_store=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    NormalizePostReloadHardRegNotes(&cfg, profile_o2);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[1].find("REG_EQUIV (mem/u:SF "), std::string::npos)
        << cfg.rpo_blocks[0].instructions[1];
  }
  // Negative 2: a memory store enters the hoisted load's forward closure.
  {
    RtlControlFlowGraph cfg = make_cfg(/*dead_s_reads_f20=*/false, /*closure_has_store=*/true);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    NormalizePostReloadHardRegNotes(&cfg, profile_o2);
    EXPECT_EQ(cfg.rpo_blocks[0].instructions[1].find("REG_EQUIV (mem/u:SF "), std::string::npos)
        << cfg.rpo_blocks[0].instructions[1];
  }
}

TEST(ReconstructHardRegLivenessTest, LibmUnaryBuiltinsAndInheritedFprArgGuards) {
  CompilationProfile mips1_o2;
  mips1_o2.optimize_level = 2;
  mips1_o2.c_flags = {"-O2", "-mips1", "-funsigned-char", "-Wa,-O2"};

  // Positive 42H-1: unary C libm builtins (`sqrtf`, `sqrt`) imply `$f12` SF/DF
  // `call_fusage` (`expr.c:7239-7285`), while unknown symbols do not.
  auto check_libm_call = [&](const std::string& sym, bool is_df) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.sideband.frame_size_bytes = 24;
    cfg.sideband.saved_gpr_regnos = {31};
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -24)))",
                      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 31 ra))",
                      is_df ? "(set (reg:DF 44 $f12) (mem:DF (reg:SI 4 a0)))"
                            : "(set (reg:SF 44 $f12) (mem:SF (reg:SI 4 a0)))",
                      std::string("(parallel[ (set (reg:") + (is_df ? "DF" : "SF") +
                          " 32 $f0) (call (mem:SI (symbol_ref:SI (\"" + sym +
                          "\"))) (const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                      is_df ? "(use (reg/i:DF 32 $f0))" : "(use (reg/i:SF 32 $f0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, mips1_o2);
    return cfg.rpo_blocks[0].rtl_insns[3].call_fusage;
  };
  {
    const auto fusage_sqrtf = check_libm_call("sqrtf", /*is_df=*/false);
    ASSERT_EQ(fusage_sqrtf.size(), 1u);
    EXPECT_EQ(fusage_sqrtf[0].children[0].RegNo(), 44);
    EXPECT_EQ(fusage_sqrtf[0].children[0].mode, RtlMachineMode::kSf);

    const auto fusage_sqrt = check_libm_call("sqrt", /*is_df=*/true);
    ASSERT_EQ(fusage_sqrt.size(), 1u);
    EXPECT_EQ(fusage_sqrt[0].children[0].RegNo(), 44);
    EXPECT_EQ(fusage_sqrt[0].children[0].mode, RtlMachineMode::kDf);
  }

  // Positive + Negative 42C: callee-saved FPR `$f20` arg save hoisted before
  // `sw $ra` infers `(use (reg:DF 6 a2))` when no non-copy GPR ALU precedes it,
  // but does NOT infer `(use (reg:DF 6 a2))` when a non-copy GPR ALU (`xor`)
  // precedes `s.d $f20` with `arg_bytes <= 16`, and does NOT infer O32 padding
  // slot `$a1` when `$a2` is written as `DFmode`.
  auto run_inherited_fpr_check = [&](bool add_preceding_gpr_alu) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.sideband.frame_size_bytes = 48;
    cfg.sideband.saved_gpr_regnos = {16, 31};
    cfg.sideband.saved_fpr_regnos = {52};
    std::vector<std::string> insns = {
        "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -48)))",
        "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 16))) (reg:SI 16 s0))",
        "(set (reg:SI 16 s0) (reg:SI 5 a1))",
    };
    if (add_preceding_gpr_alu) {
      insns.push_back("(set (reg:SI 4 a0) (xor:SI (reg:SI 4 a0) (const_int 7)))");
    }
    insns.push_back("(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 32))) (reg:DF 52 $f20))");
    const size_t mtc1_idx = insns.size();
    insns.push_back("(set (reg:DF 52 $f20) (reg:DF 6 a2))");
    insns.push_back("(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 20))) (reg:SI 31 ra))");
    if (!add_preceding_gpr_alu) {
      insns.push_back("(set (reg:SI 4 a0) (plus:SI (reg:SI 16 s0) (const_int 1)))");
    }
    insns.push_back(
        "(parallel[ (set (reg:DF 32 $f0) (call (mem:SI (symbol_ref:SI (\"ext_fn\"))) "
        "(const_int 16))) (clobber (reg:SI 31 ra)) ] )");
    insns.push_back("(set (reg/i:DF 32 $f0) (plus:DF (reg:DF 32 $f0) (reg:DF 52 $f20)))");
    insns.push_back("(use (reg/i:DF 32 $f0))");
    insns.push_back("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks = {MakeBlock(0, false, std::move(insns), {})};
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, mips1_o2);
    return std::make_pair(cfg.rpo_blocks[0].instructions[2],
                          cfg.rpo_blocks[0].instructions[mtc1_idx]);
  };
  {
    const auto [a1_copy_pos, mtc1_pos] = run_inherited_fpr_check(/*add_preceding_gpr_alu=*/false);
    // Positive: `$a2` stays live into `ext_fn` (so `mtc1` does NOT carry
    // `REG_DEAD (reg:DF 6 a2)`), while O32 alignment padding `$a1` dies
    // immediately at `s0 = a1`.
    EXPECT_EQ(mtc1_pos.find("REG_DEAD (reg:DF 6 a2)"), std::string::npos) << mtc1_pos;
    EXPECT_NE(a1_copy_pos.find("REG_DEAD (reg:SI 5 a1)"), std::string::npos) << a1_copy_pos;

    // Negative: when a non-copy GPR ALU precedes `s.d $f20` with `arg_bytes <= 16`,
    // `$a2` is NOT inferred as a call arg and dies immediately at `mtc1`.
    const auto [a1_copy_neg, mtc1_neg] = run_inherited_fpr_check(/*add_preceding_gpr_alu=*/true);
    EXPECT_NE(mtc1_neg.find("REG_DEAD (reg:DF 6 a2)"), std::string::npos) << mtc1_neg;
    EXPECT_NE(a1_copy_neg.find("REG_DEAD (reg:SI 5 a1)"), std::string::npos) << a1_copy_neg;
  }
}

TEST(ReconstructHardRegLivenessTest,
     SelfRmwGlobalInLocalOutChecksAllLowerRegAllocOrderGprsAndJump2CallClobberStrip) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  // 1. Generalized lower-reg-alloc-order GPR check (`local-alloc.c:2108-2182`,
  //    `#ifndef REG_ALLOC_ORDER`): when `$2 v0` is live across the local-out
  //    tail of `$6 a2`, `$3 v1 < $6 a2` being free still prevents a spurious
  //    split `REG_DEAD` on `$6 a2`; contrast when all lower caller-saved GPRs
  //    `$2..$5` are live across the local-out tail.
  auto make_lower_gpr_cfg = [](bool keep_v1_a0_a1_live) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    std::vector<std::string> b0 = {
        "(set (reg:SI 6 a2) (mem:SI (reg:SI 4 a0)))",
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 5 a1) (const_int 7)))",
    };
    if (keep_v1_a0_a1_live) {
      b0.push_back("(set (reg:SI 3 v1) (plus:SI (reg:SI 4 a0) (const_int 3)))");
    }
    b0.push_back(
        "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");

    std::vector<std::string> b1 = {
        "(set (reg:SI 6 a2) (ior:SI (reg:SI 6 a2) (const_int 65536)))",
        "(set (mem:SI (reg:SI 4 a0)) (reg:SI 6 a2))",
        "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))",
    };
    if (keep_v1_a0_a1_live) {
      b1.push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1)))");
      b1.push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 4 a0)))");
      b1.push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1)))");
    }
    cfg.rpo_blocks = {
        MakeBlock(0, false, std::move(b0), {2, 1}),
        MakeBlock(1, false, std::move(b1), {2}),
        MakeBlock(2, true,
                  {
                      "(set (reg/i:SI 2 v0) (reg:SI 2 v0))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    return cfg;
  };
  {
    RtlControlFlowGraph pos = make_lower_gpr_cfg(/*keep_v1_a0_a1_live=*/false);
    NormalizePostReloadHardRegNotes(&pos, profile_o2);
    EXPECT_FALSE(
        TopLevelInsnHasNote(pos.rpo_blocks[1].instructions[0], RtlMachineMode::kRegDead, 6))
        << pos.rpo_blocks[1].instructions[0];

    RtlControlFlowGraph neg = make_lower_gpr_cfg(/*keep_v1_a0_a1_live=*/true);
    NormalizePostReloadHardRegNotes(&neg, profile_o2);
    EXPECT_TRUE(TopLevelInsnHasNote(neg.rpo_blocks[1].instructions[0], RtlMachineMode::kRegDead, 6))
        << neg.rpo_blocks[1].instructions[0];
  }

  // 2. `StripStage0Jump2NoopMoveConsumedDeaths` (`jump.c:452-459`):
  //    - Positive 1: non-arg call-clobbered FPR `$f10` with `REG_DEAD $f10`
  //      followed by `CALL` (with delay slot) and `(set $f10 $s3)` is stripped.
  //    - Positive 2: non-arg FPR `$f6` loaded from `CONST_DOUBLE` and consumed
  //      before `(set $f6 $s4)` is stripped.
  //    - Negative: O32 call-arg GPR `$5 a1` read before direct `(set $5 $s0)`
  //      (no intervening call clobber) retains `REG_DEAD $5`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SF 42 $f10) (const_double:SF (cc0) 0 1072693248))",
                      "(set (reg:SF 44 $f12) (plus:SF (reg:SF 44 $f12) (reg:SF 42 $f10)))",
                      "(sequence [(call_insn (parallel[ (set (reg:SF 32 $f0) "
                      "(call (mem:SI (symbol_ref:SI (\"ext_f\"))) (const_int 16))) "
                      "(clobber (reg:SI 31 ra)) ] )) "
                      "(insn (set (reg:SI 5 a1) (reg:SI 16 s0)))])",
                      "(set (reg:SF 42 $f10) (reg:SF 19 s3))",
                      "(set (reg:SF 38 $f6) (const_double:SF (cc0) 0 1071644672))",
                      "(set (reg:SF 32 $f0) (mult:SF (reg:SF 32 $f0) (reg:SF 38 $f6)))",
                      "(set (reg:SF 38 $f6) (reg:SF 20 s4))",
                      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 42 $f10)))",
                      "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 32 $f0) (reg:SF 38 $f6)))",
                      "(use (reg/i:SF 32 $f0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
    EXPECT_FALSE(
        TopLevelInsnHasNote(cfg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 42))
        << cfg.rpo_blocks[0].instructions[1];
    EXPECT_FALSE(
        TopLevelInsnHasNote(cfg.rpo_blocks[0].instructions[5], RtlMachineMode::kRegDead, 38))
        << cfg.rpo_blocks[0].instructions[5];
  }
  {
    RtlControlFlowGraph neg_cfg;
    neg_cfg.pass = RtlPassKind::kDbr;
    neg_cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 4 a0) (plus:SI (reg:SI 5 a1) (reg:SI 4 a0)))",
                      "(set (reg:SI 5 a1) (reg:SI 16 s0))",
                      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"ext_i\"))) "
                      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    ReconstructPostDbrHardRegLivenessAndReturnUse(&neg_cfg, profile_o2);
    EXPECT_TRUE(
        TopLevelInsnHasNote(neg_cfg.rpo_blocks[0].instructions[0], RtlMachineMode::kRegDead, 5))
        << neg_cfg.rpo_blocks[0].instructions[0];
  }
}

TEST(ReconstructHardRegLivenessTest,
     Zone3SpillSlotMultiUseInheritedMtc1AndProvenFpSpillConstDoubleDeadStripping) {
  const CompilationProfile profile_o2 = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000"});

  // Positive (`reload1.c:1446, 3445-3508, 5186-5258, 5641-6640`):
  // 1. Zone-3 reload-loop spill slot `80($sp)` (strictly between caller-save
  //    slot `64($sp)` and prologue save `96($sp)`) proves `$f8 (40)` is a block
  //    FP spill register, so `(set $f8 (const_double ...))` feeding pure FP
  //    consumers (including an in-place RMW `(set $f8 (mult $f4 $f8))`) strips
  //    `REG_DEAD $f8`.
  // 2. Multi-use inherited callee-saved cross-bank reload `(set $f6 $s1)`
  //    consumed by two pure FP insns strips `REG_DEAD $f6` on the final consumer.
  // Negative (`reload1.c:4712`):
  // 3. An output-reload store `(set [sp+80] $f2)` alone (reusing a dying
  //    allocated pseudo's hard reg `$f2`) does NOT mark `$f2` as a spilled reg,
  //    so a later `(set $f2 (const_double ...))` consumer preserves `REG_DEAD $f2`.
  // 4. An incoming ABI arg copy `(set $f0 $a1)` does NOT mark `$f0` as a spill
  //    reg, so a later `(set $f0 (const_double ...))` consumer preserves `REG_DEAD $f0`.
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband.frame_size_bytes = 160;
  cfg.sideband.saved_gpr_regnos = {16, 17, 31};
  cfg.sideband.saved_fpr_regnos = {52, 54, 56, 58, 60, 62};
  cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -160)))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 96))) (reg:SI 16 s0))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 100))) (reg:SI 17 s1))",
                    "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 104))) (reg:SI 31 ra))",
                    "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 112))) (reg:DF 52 $f20))",
                    "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 120))) (reg:DF 54 $f22))",
                    "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 128))) (reg:DF 56 $f24))",
                    "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 136))) (reg:DF 58 $f26))",
                    "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 144))) (reg:DF 60 $f28))",
                    "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 152))) (reg:DF 62 $f30))",
                    // Caller-save slot at `68($sp)` around a call.
                    "(set (reg:SF 48 $f16) (mem:SF (reg:SI 4 a0)))",
                    "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 68))) (reg:SF 48 $f16))",
                    "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"ext_f\"))) "
                    "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                    "(set (reg:SF 48 $f16) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 68))))",
                    // Output-reload store reusing `$f2 (34)` into Zone-3 slot `84($sp)`.
                    "(set (reg:SF 34 $f2) (plus:SF (reg:SF 48 $f16) (reg:SF 32 $f0)))",
                    "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 84))) (reg:SF 34 $f2))",
                    // Zone-3 spill load into `$f8 (40)` proves `$f8` is a spill reg.
                    "(set (reg:SF 40 $f8) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 84))))",
                    "(set (reg:SF 36 $f4) (plus:SF (reg:SF 40 $f8) (reg:SF 48 $f16)))",
                    // Proven spill `$f8` const_double + RMW consumer -> strips `REG_DEAD $f8`.
                    "(set (reg:SF 40 $f8) (const_double:SF (cc0) 0 1070596096))",
                    "(set (reg:SF 40 $f8) (mult:SF (reg:SF 36 $f4) (reg:SF 40 $f8)))",
                    "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 84))) (reg:SF 40 $f8))",
                    // Multi-use inherited callee-saved `mtc1 $s1 -> $f6` -> strips `REG_DEAD $f6`.
                    "(set (reg:SF 38 $f6) (reg:SF 17 s1))",
                    "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 38 $f6)))",
                    "(set (reg:SF 48 $f16) (mult:SF (reg:SF 48 $f16) (reg:SF 38 $f6)))",
                    // Non-spill `$f2` const_double -> preserves `REG_DEAD $f2`.
                    "(set (reg:SF 34 $f2) (const_double:SF (cc0) 0 1071644672))",
                    "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 34 $f2)))",
                    // Incoming ABI arg copy `$a1 -> $f0` -> does NOT prove `$f0` is a spill reg.
                    "(set (reg:SF 32 $f0) (reg:SF 5 a1))",
                    "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 32 $f0)))",
                    "(set (reg:SF 32 $f0) (const_double:SF (cc0) 0 1072693248))",
                    "(set (reg:SF 36 $f4) (plus:SF (reg:SF 36 $f4) (reg:SF 32 $f0)))",
                    "(set (reg/i:SF 32 $f0) (plus:SF (reg:SF 36 $f4) (reg:SF 48 $f16)))",
                    "(use (reg/i:SF 32 $f0))",
                    "(set (reg:SI 16 s0) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 96))))",
                    "(set (reg:SI 17 s1) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 100))))",
                    "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 104))))",
                    "(set (reg:DF 52 $f20) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 112))))",
                    "(set (reg:DF 54 $f22) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 120))))",
                    "(set (reg:DF 56 $f24) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 128))))",
                    "(set (reg:DF 58 $f26) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 136))))",
                    "(set (reg:DF 60 $f28) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 144))))",
                    "(set (reg:DF 62 $f30) (mem:DF (plus:SI (reg:SI 29 sp) (const_int 152))))",
                    "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 160)))",
                    "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                },
                {}),
  };
  ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, profile_o2);
  // Positive 1: `$f8` const_double RMW consumer at [19] has no `REG_DEAD $f8`.
  EXPECT_FALSE(
      TopLevelInsnHasNote(cfg.rpo_blocks[0].instructions[19], RtlMachineMode::kRegDead, 40))
      << cfg.rpo_blocks[0].instructions[19];
  // Positive 2: multi-use inherited callee-saved `mtc1 $s1 -> $f6` final consumer at [23] has no
  // `REG_DEAD $f6`.
  EXPECT_FALSE(
      TopLevelInsnHasNote(cfg.rpo_blocks[0].instructions[23], RtlMachineMode::kRegDead, 38))
      << cfg.rpo_blocks[0].instructions[23];
  // Negative 1: `$f2` (only stored to spill slot, never loaded) keeps `REG_DEAD $f2` at [25].
  EXPECT_TRUE(TopLevelInsnHasNote(cfg.rpo_blocks[0].instructions[25], RtlMachineMode::kRegDead, 34))
      << cfg.rpo_blocks[0].instructions[25];
  // Negative 2: `$f0` (only copied from ABI arg `$a1`, not callee-saved GPR) keeps `REG_DEAD $f0`
  // at [29].
  EXPECT_TRUE(TopLevelInsnHasNote(cfg.rpo_blocks[0].instructions[29], RtlMachineMode::kRegDead, 32))
      << cfg.rpo_blocks[0].instructions[29];
}

TEST(ReconstructHardRegLivenessTest, Stage0Extendsidi2SignDeadAndDiLoadPtrDeadNotes) {
  const CompilationProfile o1_prof = MakeProfile(1, {"-O1", "-mips2", "-Wa,-g"});

  // Positive 1 (`flow.c:2507-2550`): non-integrated `DI` load whose base pointer
  // `$5` (`a1`) lies inside destination pair `{4, 5}` (`dst.mode != ptr.mode`)
  // receives `(REG_DEAD (reg:SI 5 a1))`, whereas an integrated `/i` `DI` load
  // (negative) does not.
  auto make_di_load_cfg = [](bool integrated_dst) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 5 a1) (plus:SI (reg:SI 4 a0) (const_int 8)))",
                      integrated_dst ? "(set (reg/i:DI 4 a0) (mem:DI (reg:SI 5 a1)))"
                                     : "(set (reg:DI 4 a0) (mem:DI (reg:SI 5 a1)))",
                      "(set (reg/i:DI 2 v0) (reg:DI 4 a0))",
                      "(use (reg/i:DI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    return cfg;
  };
  {
    RtlControlFlowGraph pos = make_di_load_cfg(/*integrated_dst=*/false);
    NormalizePostReloadHardRegNotes(&pos, o1_prof);
    EXPECT_TRUE(TopLevelInsnHasNote(pos.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 5))
        << pos.rpo_blocks[0].instructions[1];

    RtlControlFlowGraph neg = make_di_load_cfg(/*integrated_dst=*/true);
    NormalizePostReloadHardRegNotes(&neg, o1_prof);
    EXPECT_FALSE(
        TopLevelInsnHasNote(neg.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 5))
        << neg.rpo_blocks[0].instructions[1];
  }

  // Positive 2 (`flow.c:2507-2550` + `optabs.c:2495`): `extendsidi2`
  // self-RMW `ashiftrt:SI 31` (`move $3, $2 ; sra $2, $2, 31`) carrying
  // `REG_NO_CONFLICT` preserves `(REG_DEAD (reg:SI 2 v0))` ahead of
  // `REG_NO_CONFLICT`.
  {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 2 v0) (mem:SI (reg:SI 5 a1)))",
                      "(set (reg:SI 3 v1) (reg:SI 2 v0)) "
                      "(expr_list:REG_NO_CONFLICT (reg:SI 2 v0) (nil))",
                      "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 2 v0) (const_int 31))) "
                      "(expr_list:REG_NO_CONFLICT (reg:SI 2 v0) (nil))",
                      "(set (reg/i:DI 2 v0) (reg:DI 2 v0))",
                      "(use (reg/i:DI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    NormalizePostReloadHardRegNotes(&cfg, o1_prof);
    const std::string& sra = cfg.rpo_blocks[0].instructions[2];
    const size_t dead_pos = sra.find("REG_DEAD (reg:SI 2 v0)");
    const size_t nc_pos = sra.find("REG_NO_CONFLICT (reg:SI 2 v0)");
    ASSERT_NE(dead_pos, std::string::npos) << sra;
    ASSERT_NE(nc_pos, std::string::npos) << sra;
    EXPECT_LT(dead_pos, nc_pos) << sra;
  }
}

TEST(ReconstructHardRegLivenessTest,
     Stage0PinnedCallArgDefSuppressesSelfRmwDeadAndSubregPreservesDiModeDead) {
  const CompilationProfile o2_prof = MakeProfile(2, {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"});

  // 1. Positive + Negative (`flow.c:2506-2515`, `sched.c:2872`,
  //    `Stage0DefReachesPinnedCallArgUse`):
  //    `$a0` is defined in `B0` and reaches both a `call_fusage` read in `B1`
  //    and a self-RMW `(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))`
  //    in `B2`. Because the reaching def feeds a sibling `call_fusage` read of
  //    `$a0`, `$a0` is ABI-pinned (not an unpinned global pseudo), so `B2`'s
  //    self-RMW must NOT receive an illegal `(REG_DEAD (reg:SI 4 a0))` (positive).
  //    Without the sibling `call_fusage` read (negative), `$a0` enters `B2` as
  //    an unpinned global pseudo and the self-RMW feeding a local consumer DOES
  //    receive `(REG_DEAD (reg:SI 4 a0))`.
  auto make_call_arg_sibling_cfg = [](bool sibling_reads_a0_in_call_fusage) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 4 a0) (plus:SI (reg:SI 5 a1) (const_int 3)))",
                      "(set (reg:SI 2 v0) (const_int 11))",
                      "(set (reg:SI 3 v1) (const_int 13))",
                      "(set (pc) (if_then_else (eq:SI (reg:SI 6 a2) (const_int 0)) "
                      "(label_ref L2) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 3 v1)))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
        MakeBlock(2, true,
                  {
                      "(set (reg:SI 4 a0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                      "(set (reg/i:SI 2 v0) (plus:SI (reg:SI 4 a0) (reg:SI 2 v0)))",
                      "(set (reg/i:SI 2 v0) (plus:SI (reg/i:SI 2 v0) (reg:SI 3 v1)))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    cfg.rpo_blocks[1].rtl_insns[0].has_explicit_call_fusage = true;
    if (sibling_reads_a0_in_call_fusage) {
      cfg.rpo_blocks[1].rtl_insns[0].call_fusage = {
          RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 4)),
      };
    }
    return cfg;
  };
  {
    RtlControlFlowGraph pos = make_call_arg_sibling_cfg(/*sibling_reads_a0_in_call_fusage=*/true);
    NormalizePostReloadHardRegNotes(&pos, o2_prof);
    EXPECT_FALSE(
        TopLevelInsnHasNote(pos.rpo_blocks[2].instructions[0], RtlMachineMode::kRegDead, 4))
        << pos.rpo_blocks[2].instructions[0];
  }
  {
    RtlControlFlowGraph neg = make_call_arg_sibling_cfg(/*sibling_reads_a0_in_call_fusage=*/false);
    NormalizePostReloadHardRegNotes(&neg, o2_prof);
    EXPECT_TRUE(TopLevelInsnHasNote(neg.rpo_blocks[2].instructions[0], RtlMachineMode::kRegDead, 4))
        << neg.rpo_blocks[2].instructions[0];
  }

  // 2. Positive + Negative (`flow.c:2387-2406`, `sched.c:2937-2942`):
  //    reading `(subreg:SI (reg:DI 10 t2) 1)` when both words of `$t2/$t3` die
  //    attaches full `(REG_DEAD (reg:DI 10 t2))` (positive); when word 0 (`$t2`)
  //    stays live into a later consumer, attaches only `(REG_DEAD (reg:SI 11 t3))`
  //    (negative).
  auto make_di_subreg_cfg = [](bool word0_stays_live) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    std::vector<std::string> insns = {
        "(set (reg:DI 10 t2) (mem:DI (reg:SI 4 a0)))",
        "(set (reg:SI 2 v0) (plus:SI (subreg:SI (reg:DI 10 t2) 1) (reg:SI 5 a1)))",
    };
    if (word0_stays_live) {
      insns.push_back("(set (reg/i:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 10 t2)))");
    } else {
      insns.push_back("(set (reg/i:SI 2 v0) (reg:SI 2 v0))");
    }
    insns.push_back("(use (reg/i:SI 2 v0))");
    insns.push_back("(parallel[ (return) (use (reg:SI 31 ra)) ] )");
    cfg.rpo_blocks = {MakeBlock(0, false, std::move(insns), {})};
    return cfg;
  };
  {
    RtlControlFlowGraph pos = make_di_subreg_cfg(/*word0_stays_live=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&pos, o2_prof);
    EXPECT_NE(pos.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:DI 10 t2)"), std::string::npos)
        << pos.rpo_blocks[0].instructions[1];
  }
  {
    RtlControlFlowGraph neg = make_di_subreg_cfg(/*word0_stays_live=*/true);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&neg, o2_prof);
    EXPECT_NE(neg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:SI 11 t3)"), std::string::npos)
        << neg.rpo_blocks[0].instructions[1];
    EXPECT_EQ(neg.rpo_blocks[0].instructions[1].find("REG_DEAD (reg:DI 10 t2)"), std::string::npos)
        << neg.rpo_blocks[0].instructions[1];
  }
}

TEST(ReconstructHardRegLivenessTest,
     RefineCallArgumentFusageAcrossCfgDropsConsumedScratchWritesButForwardsUnread) {
  // Mirrors `calls.c:1850-1915`, `local-alloc.c:2158`: a scratch argument-reg
  // write (`$a3`) consumed inside its defining predecessor block must not leak
  // across CFG edges into a downstream call's `CALL_INSN_FUNCTION_USAGE` (which
  // would wrongly keep `$a3` live into the branch and suppress `(REG_DEAD a3)`),
  // whereas an unread predecessor-block write (`$a3` never read before the
  // branch) IS still forwarded across the edge into the call.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips2", "-mcpu=r4000"}};

  auto make_cfg = [](bool read_a3_in_b0) {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kDbr;
    std::vector<std::string> b0_insns = {
        "(set (reg:SI 7 a3) (plus:SI (reg:SI 4 a0) (const_int 16)))",
    };
    if (read_a3_in_b0) {
      b0_insns.push_back("(set (reg:SI 16 s0) (mem:SI (reg:SI 7 a3)))");
    } else {
      b0_insns.push_back("(set (reg:SI 16 s0) (mem:SI (reg:SI 4 a0)))");
    }
    b0_insns.push_back(
        "(set (pc) (if_then_else (eq:SI (reg:SI 5 a1) (const_int 0)) (label_ref L1) (pc)))");
    cfg.rpo_blocks = {
        MakeBlock(0, false, std::move(b0_insns), {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SI 4 a0) (reg:SI 16 s0))",
                      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                  },
                  {2}),
        MakeBlock(2, true,
                  {
                      "(set (reg/i:SI 2 v0) (reg:SI 16 s0))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    return cfg;
  };

  // Positive: `$a3` is read inside `B0` before branching to `B1`, so `$a3` must
  // NOT be forwarded into `B1`'s `CALL_INSN_FUNCTION_USAGE`, and `B0[1]` gets
  // `(REG_DEAD (reg:SI 7 a3))`.
  {
    RtlControlFlowGraph pos = make_cfg(/*read_a3_in_b0=*/true);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&pos, o2_prof);
    const auto& fusage = pos.rpo_blocks[1].rtl_insns[1].call_fusage;
    bool has_a3 = false;
    for (const RtlExpr& fu : fusage) {
      if (fu.ToGccPrintRtlString().find("(reg:SI 7 a3)") != std::string::npos) {
        has_a3 = true;
      }
    }
    EXPECT_FALSE(has_a3) << pos.FormatSummary();
    EXPECT_TRUE(TopLevelInsnHasNote(pos.rpo_blocks[0].instructions[1], RtlMachineMode::kRegDead, 7))
        << pos.rpo_blocks[0].instructions[1];
  }

  // Negative: `$a3` is written in `B0` and NEVER read in `B0`, so the unread
  // cross-block argument setup IS forwarded into `B1`'s `CALL_INSN_FUNCTION_USAGE`.
  {
    RtlControlFlowGraph neg = make_cfg(/*read_a3_in_b0=*/false);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&neg, o2_prof);
    const auto& fusage = neg.rpo_blocks[1].rtl_insns[1].call_fusage;
    bool has_a3 = false;
    for (const RtlExpr& fu : fusage) {
      if (fu.ToGccPrintRtlString().find("(reg:SI 7 a3)") != std::string::npos) {
        has_a3 = true;
      }
    }
    EXPECT_TRUE(has_a3) << neg.FormatSummary();
  }

  // Negative 2: when `$a1` is computed by `plus` and read as a non-address
  // store value (`sw $a1, 4($a0)`) rather than a `MEM` base pointer, it may
  // still be a live-through call argument in `B1` and IS preserved in `call_fusage`.
  {
    RtlControlFlowGraph neg_val;
    neg_val.pass = RtlPassKind::kDbr;
    neg_val.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 5 a1) (plus:SI (reg:SI 2 v0) (reg:SI 5 a1)))",
                      "(set (mem:SI (plus:SI (reg:SI 4 a0) (const_int 4))) (reg:SI 5 a1))",
                      "(set (pc) (if_then_else (eq:SI (reg:SI 3 v1) (const_int 0)) "
                      "(label_ref L1) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SI 4 a0) (mem:SI (reg:SI 4 a0)))",
                      "(parallel[ (call (mem:SI (symbol_ref:SI (\"notify\"))) (const_int 16)) "
                      "(clobber (reg:SI 31 ra)) ] )",
                  },
                  {2}),
        MakeBlock(2, true,
                  {
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    ReconstructPostDbrHardRegLivenessAndReturnUse(&neg_val, o2_prof);
    const auto& fusage = neg_val.rpo_blocks[1].rtl_insns[1].call_fusage;
    bool has_a1 = false;
    for (const RtlExpr& fu : fusage) {
      if (fu.ToGccPrintRtlString().find("(reg:SI 5 a1)") != std::string::npos) {
        has_a1 = true;
      }
    }
    EXPECT_TRUE(has_a1) << neg_val.FormatSummary();
  }

  // Negative 3 (`calls.c:1850-1915`): when `$a0` is computed via `plus` and
  // dereferenced in `B0` while `B1` sets higher O32 argument `$a1` before
  // `jal foo`, `$a0` remains in `must_valid` so O32 prefix closure retains
  // `$a0` in `call_fusage`.
  {
    RtlControlFlowGraph neg_prefix;
    neg_prefix.pass = RtlPassKind::kDbr;
    neg_prefix.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 4 a0) (plus:SI (reg:SI 16 s0) (const_int 16)))",
                      "(set (reg:SI 17 s1) (mem:SI (reg:SI 4 a0)))",
                      "(set (pc) (if_then_else (eq:SI (reg:SI 17 s1) (const_int 0)) "
                      "(label_ref L1) (pc)))",
                  },
                  {2, 1}),
        MakeBlock(1, false,
                  {
                      "(set (reg:SI 5 a1) (reg:SI 17 s1))",
                      "(parallel[ (set (reg:SI 2 v0) (call (mem:SI (symbol_ref:SI (\"foo\"))) "
                      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                  },
                  {2}),
        MakeBlock(2, true,
                  {
                      "(set (reg/i:SI 2 v0) (reg:SI 17 s1))",
                      "(use (reg/i:SI 2 v0))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    ReconstructPostDbrHardRegLivenessAndReturnUse(&neg_prefix, o2_prof);
    const auto& fusage = neg_prefix.rpo_blocks[1].rtl_insns[1].call_fusage;
    bool has_a0 = false;
    for (const RtlExpr& fu : fusage) {
      if (fu.ToGccPrintRtlString().find("(reg:SI 4 a0)") != std::string::npos) {
        has_a0 = true;
      }
    }
    EXPECT_TRUE(has_a0) << neg_prefix.FormatSummary();
  }
}

TEST(ReconstructHardRegLivenessTest,
     NormalizeStage0PostReloadRegNotesOrdersAttachDeathsBeforeSpillUpgrade) {
  // Mirrors `sched.c:2956-2986` (`attach_deaths`) + `reload1.c:6134-6140`:
  // when `RunsSched1()` is true (`-O2`), `NormalizeStage0PostReloadRegNotes`
  // orders multi-operand `REG_DEAD` notes into `attach_deaths` operand preorder
  // BEFORE upgrading a reload-scratch death to `(REG_DEAD (mem [sp+off]))`,
  // whereas `-O1` (`!RunsSched1()`) upgrades the spill death in place without
  // `attach_deaths` reordering.
  const CompilationProfile o2_prof{.optimize_level = 2,
                                   .c_flags = {"-O2", "-mips1", "-funsigned-char"}};
  const CompilationProfile o1_prof{.optimize_level = 1,
                                   .c_flags = {"-O1", "-mips1", "-funsigned-char"}};

  auto make_cfg = []() {
    RtlControlFlowGraph cfg;
    cfg.pass = RtlPassKind::kSched2;
    cfg.sideband.frame_size_bytes = 128;
    cfg.sideband.saved_gpr_regnos = {31};
    cfg.sideband.saved_fpr_regnos = {52, 54, 56, 58, 60, 62};
    cfg.rpo_blocks = {
        MakeBlock(0, false,
                  {
                      "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int -128)))",
                      "(set (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))) (reg:SI 31 ra))",
                      "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 80))) (reg:DF 52 $f20))",
                      "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 88))) (reg:DF 54 $f22))",
                      "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 96))) (reg:DF 56 $f24))",
                      "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 104))) (reg:DF 58 $f26))",
                      "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 112))) (reg:DF 60 $f28))",
                      "(set (mem:DF (plus:SI (reg:SI 29 sp) (const_int 120))) (reg:DF 62 $f30))",
                      "(set (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))) (reg:SF 44 $f12))",
                      "(parallel[ (set (reg:SF 32 $f0) (call (mem:SI (symbol_ref:SI (\"callee\"))) "
                      "(const_int 16))) (clobber (reg:SI 31 ra)) ] )",
                      "(set (reg:SF 34 $f2) (mem:SF (plus:SI (reg:SI 29 sp) (const_int 28))))",
                      "(set (reg:SF 36 $f4) (plus:SF (reg:SF 32 $f0) (reg:SF 34 $f2))) "
                      "(expr_list:REG_DEAD (reg:SF 34 $f2) "
                      "(expr_list:REG_DEAD (reg:SF 32 $f0) (nil)))",
                      "(set (mem:SF (reg:SI 4 a0)) (reg:SF 36 $f4))",
                      "(set (reg:SI 31 ra) (mem:SI (plus:SI (reg:SI 29 sp) (const_int 72))))",
                      "(set (reg:SI 29 sp) (plus:SI (reg:SI 29 sp) (const_int 128)))",
                      "(parallel[ (return) (use (reg:SI 31 ra)) ] )",
                  },
                  {}),
    };
    return cfg;
  };

  // Positive (`-O2`, `RunsSched1()`): `attach_deaths` visits `SET_SRC` right-to-left
  // (`$f2` then `$f0`) and prepends each note, placing `$f0` before `$f2`; after
  // spill-slot upgrade of `$f2` to `(mem:SF [sp+28])`, `$f0` precedes `mem:SF`.
  {
    RtlControlFlowGraph pos = make_cfg();
    NormalizePostReloadHardRegNotes(&pos, o2_prof);
    const std::string& add_insn = pos.rpo_blocks[0].instructions[11];
    const size_t f0_pos = add_insn.find("REG_DEAD (reg:SF 32 $f0)");
    const size_t mem_pos = add_insn.find("REG_DEAD (mem:SF");
    ASSERT_NE(f0_pos, std::string::npos) << add_insn;
    ASSERT_NE(mem_pos, std::string::npos) << add_insn;
    EXPECT_LT(f0_pos, mem_pos) << add_insn;
  }

  // Negative (`-O1`, `!RunsSched1()`): `attach_deaths` reordering does NOT run,
  // so the upgraded `(mem:SF [sp+28])` note remains ahead of `$f0`.
  {
    RtlControlFlowGraph neg = make_cfg();
    NormalizePostReloadHardRegNotes(&neg, o1_prof);
    const std::string& add_insn = neg.rpo_blocks[0].instructions[11];
    const size_t f0_pos = add_insn.find("REG_DEAD (reg:SF 32 $f0)");
    const size_t mem_pos = add_insn.find("REG_DEAD (mem:SF");
    ASSERT_NE(f0_pos, std::string::npos) << add_insn;
    ASSERT_NE(mem_pos, std::string::npos) << add_insn;
    EXPECT_LT(mem_pos, f0_pos) << add_insn;
  }

  // Positive 2 (`sched.c:2978-2986` `attach_deaths_insn`): on a store
  // `(set (mem:SF (reg:SI 4 a0)) (reg:SF 34 $f2))` where `$f2` is a spilled
  // reload scratch and `$a0` also dies, `SET_DEST` `MEM` address death (`$a0`)
  // precedes `SET_SRC` death (`(mem:SF [sp+28])`) under `-O2` (`RunsSched1()`).
  {
    RtlControlFlowGraph store_cfg = make_cfg();
    store_cfg.rpo_blocks[0].instructions[11] =
        "(set (mem:SF (reg:SI 4 a0)) (reg:SF 34 $f2)) "
        "(expr_list:REG_DEAD (reg:SF 34 $f2) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))";
    store_cfg.rpo_blocks[0].instructions[12] = "(set (reg:SI 4 a0) (const_int 0))";
    store_cfg.rpo_blocks[0].SyncRtlInsnsFromInstructions();
    NormalizePostReloadHardRegNotes(&store_cfg, o2_prof);
    const std::string& st_insn = store_cfg.rpo_blocks[0].instructions[11];
    const size_t a0_pos = st_insn.find("REG_DEAD (reg:SI 4 a0)");
    const size_t mem_pos = st_insn.find("REG_DEAD (mem:SF");
    ASSERT_NE(a0_pos, std::string::npos) << st_insn;
    ASSERT_NE(mem_pos, std::string::npos) << st_insn;
    EXPECT_LT(a0_pos, mem_pos) << st_insn;
  }
}

}  // namespace
}  // namespace rom_nom_nom::lifter
