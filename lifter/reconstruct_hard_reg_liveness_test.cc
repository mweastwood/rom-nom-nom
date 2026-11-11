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

  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& dbr_cfg_x) const override {
    return lifter::InvertPass01DbrSchedule(dbr_cfg_x);
  }

  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& jump2_cfg_x) const override {
    return lifter::InvertDoCrossJump(jump2_cfg_x);
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

  // 2. Negative `-O0` CFG (empty `basic_block_live_at_start`, no `SEQUENCE`, no `return_internal`,
  // no `REG_DEAD` / `REG_UNUSED`): `RecomputePostReloadLiveAtStart` leaves
  // `basic_block_live_at_start` empty.
  RtlControlFlowGraph o0_cfg;
  o0_cfg.pass = RtlPassKind::kSched2;
  o0_cfg.rpo_blocks = {
      MakeBlock(0, false,
                {
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 4 a0) (const_int 1)))",
                    "(use (reg/i:SI 2 v0))",
                },
                {}),
  };
  RecomputePostReloadLiveAtStart(&o0_cfg);
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

}  // namespace
}  // namespace rom_nom_nom::lifter
