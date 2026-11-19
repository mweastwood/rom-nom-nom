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
  EXPECT_EQ(o1_load_cfg.rpo_blocks[1].instructions[0], "(use (reg:SI 24 t8))");

  RtlControlFlowGraph o2_load_cfg = make_loop_cfg(/*first_insn_is_load=*/true);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o2_load_cfg, profile_o2);
  ASSERT_EQ(o2_load_cfg.rpo_blocks[1].instructions.size(), 4u) << o2_load_cfg.FormatSummary();
  EXPECT_EQ(o2_load_cfg.rpo_blocks[1].instructions[1], "(use (reg:SI 24 t8))");

  RtlControlFlowGraph o2_alu_cfg = make_loop_cfg(/*first_insn_is_load=*/false);
  ReconstructPostDbrHardRegLivenessAndReturnUse(&o2_alu_cfg, profile_o2);
  ASSERT_EQ(o2_alu_cfg.rpo_blocks[1].instructions.size(), 4u) << o2_alu_cfg.FormatSummary();
  EXPECT_EQ(o2_alu_cfg.rpo_blocks[1].instructions[0], "(use (reg:SI 24 t8))");
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

}  // namespace
}  // namespace rom_nom_nom::lifter
