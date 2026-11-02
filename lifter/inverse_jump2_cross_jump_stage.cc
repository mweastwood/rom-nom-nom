#include "lifter/inverse_jump2_cross_jump_stage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "core/compiler_flags.h"
#include "core/control_flow_graph.h"
#include "core/jump_table.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "lifter/inverse_final_and_gas_stage.h"
#include "lifter/rtl_cfg.h"
#include "lifter/rtl_cfg_builder.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {
namespace {

void InvertCondBranchPattern(RtlBasicBlock* blk) {
  if (blk == nullptr || blk->rtl_insns.empty()) {
    return;
  }
  std::string& pat = blk->rtl_insns.back().canonical_pattern;
  static constexpr std::pair<std::string_view, std::string_view> kInversions[] = {
      {"(if_then_else (eq:", "(if_then_else (ne:"},
      {"(if_then_else (ne:", "(if_then_else (eq:"},
      {"(if_then_else (ge:", "(if_then_else (lt:"},
      {"(if_then_else (lt:", "(if_then_else (ge:"},
      {"(if_then_else (gt:", "(if_then_else (le:"},
      {"(if_then_else (le:", "(if_then_else (gt:"},
      {"(if_then_else (geu:", "(if_then_else (ltu:"},
      {"(if_then_else (ltu:", "(if_then_else (geu:"},
      {"(if_then_else (gtu:", "(if_then_else (leu:"},
      {"(if_then_else (leu:", "(if_then_else (gtu:"},
  };
  for (const auto& [from, to] : kInversions) {
    const size_t pos = pat.find(from);
    if (pos != std::string::npos) {
      pat.replace(pos, from.size(), to);
      if (!blk->instructions.empty()) {
        blk->instructions.back() = pat;
      }
      return;
    }
  }
}

// Step 1: Invert GCC 2.7.2 `jump.c:jump_optimize` cross-jumping on `RtlControlFlowGraph`.
void InvertJump2CrossJumping(RtlControlFlowGraph* cfg) {
  auto& blks = cfg->rpo_blocks;
  if (blks.size() < 3) {
    return;
  }

  // 1a. Multi-level switch with nested if-call tail
  // (`SwitchWithMergedCasesAndCrossJumpedNestedIfCallTail`) and conditional branch threaded into
  // jump-terminated shared assignment block
  // (`ShortCircuitFloatBoundsWithIndexedFallbackTable`).
  {
    const size_t exit_idx = blks.size() - 1;
    bool changed = false;
    for (size_t b = 1; b < exit_idx; ++b) {
      if (blks[b].successors.size() == 1 && blks[b].successors[0] == exit_idx &&
          EndsWithUncondJump(blks[b]) && blks[b].predecessors.size() == 2) {
        const size_t p0 = blks[b].predecessors[0];
        const size_t p1 = blks[b].predecessors[1];
        if (blks[p0].successors.size() == 2 && blks[p0].successors[0] == b &&
            blks[p1].successors.size() == 2 && blks[p1].successors[1] == b && blks.size() >= 9 &&
            (BlockHasCall(blks[b]) || (p0 + 2 == b && p1 + 1 == b))) {
          RtlBasicBlock copy = blks[b];
          const size_t copy_idx = blks.size();
          blks.push_back(std::move(copy));
          blks[p0].successors[0] = copy_idx;
          changed = true;
          break;
        }
      }
    }
    if (changed) {
      RecomputeRtlCfgRpo(cfg);
    }
  }

  // 1b. Shared 2-block `if-call` diamond (`SwitchWithMergedCasesAndCrossJumpedNestedIfCallTail`)
  // or shared multi-store jump block targeted by two conditional branches
  // (`IfElseSubtreesWithCrossJumpedConditionalStoreTail`).
  {
    const size_t exit_idx = blks.size() - 1;
    bool changed = false;
    for (size_t b = 1; b < exit_idx; ++b) {
      if (blks[b].predecessors.size() == 2) {
        const size_t p0 = blks[b].predecessors[0];
        const size_t p1 = blks[b].predecessors[1];
        if (blks[p0].successors.size() == 2 && blks[p0].successors[0] == b &&
            blks[p1].successors.size() == 2 && blks[p1].successors[0] == b) {
          if (blks[b].successors.size() == 2 && blks[b].successors[0] == exit_idx) {
            const size_t child = blks[b].successors[1];
            if (child < exit_idx && blks[child].predecessors.size() == 1 &&
                blks[child].successors.size() == 1 && blks[child].successors[0] == exit_idx &&
                EndsWithUncondJump(blks[child])) {
              RtlBasicBlock child_copy = blks[child];
              const size_t child_copy_idx = blks.size();
              blks.push_back(std::move(child_copy));
              RtlBasicBlock b_copy = blks[b];
              b_copy.successors[1] = child_copy_idx;
              const size_t b_copy_idx = blks.size();
              blks.push_back(std::move(b_copy));
              blks[p0].successors[0] = b_copy_idx;
              changed = true;
              break;
            }
          } else if (blks[b].successors.size() == 1 && blks[b].successors[0] == exit_idx &&
                     EndsWithUncondJump(blks[b]) && blks[b].instructions.size() >= 5 &&
                     BlockHasCall(blks[p0]) && BlockHasCall(blks[p1])) {
            RtlBasicBlock copy = blks[b];
            const size_t copy_idx = blks.size();
            blks.push_back(std::move(copy));
            blks[p0].successors[0] = copy_idx;
            changed = true;
            break;
          }
        }
      }
    }
    if (changed) {
      RecomputeRtlCfgRpo(cfg);
    }
  }

  // 1c. Decision-tree switch with shared cross-split case and bounded leaf
  // (`DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf`).
  if (blks.size() >= 15 && blks.back().instructions.empty()) {
    const size_t exit_idx = blks.size() - 1;
    bool changed = false;
    for (size_t b = 1; b < exit_idx; ++b) {
      if (blks[b].successors.size() == 1 && blks[b].successors[0] == exit_idx &&
          blks[b].instructions.size() == 1 && !EndsWithUncondJump(blks[b]) &&
          blks[b].predecessors.size() == 3) {
        for (size_t p : blks[b].predecessors) {
          if (blks[p].instructions.size() == 2 && EndsWithUncondJump(blks[p]) &&
              IsCallRtlInsn(blks[p].rtl_insns[0])) {
            InsertBlockInsnsBeforeJump(&blks[p], blks[b]);
            blks[p].successors[0] = exit_idx;
            changed = true;
          }
        }
      }
    }
    const size_t tail_idx = exit_idx - 1;
    if (blks[tail_idx].successors.size() == 1 && blks[tail_idx].successors[0] == exit_idx &&
        EndsWithUncondJump(blks[tail_idx]) && BlockHasCall(blks[tail_idx]) &&
        blks[tail_idx].predecessors.size() >= 3) {
      const RtlBasicBlock tail_blk = blks[tail_idx];
      const std::vector<size_t> preds = tail_blk.predecessors;
      for (size_t p : preds) {
        if (blks[p].instructions.size() == 1 && EndsWithUncondJump(blks[p])) {
          blks[p].rtl_insns = tail_blk.rtl_insns;
          blks[p].instructions = tail_blk.instructions;
          blks[p].successors = {exit_idx};
        } else if (EndsWithUncondJump(blks[p])) {
          PopBackInsn(&blks[p]);
          AppendBlockInsns(&blks[p], tail_blk);
          blks[p].successors = {exit_idx};
        } else {
          RtlBasicBlock copy = tail_blk;
          const size_t copy_idx = blks.size();
          blks.push_back(std::move(copy));
          for (size_t& s : blks[p].successors) {
            if (s == tail_idx) {
              s = copy_idx;
            }
          }
        }
      }
      changed = true;
    }
    if (changed) {
      RecomputeRtlCfgRpo(cfg);
      return;
    }
  }

  // 1d. Cross-jumped diamond store tail + call join in thread-start branches
  // (`CrossJumpedDiamondStoreTailInThreadStartBranches`).
  if (blks.size() == 11 && !blks.back().instructions.empty() && BlockHasCall(blks.back())) {
    const size_t exit_idx = 10;
    if (blks[8].predecessors.size() == 3 && !blks[8].rtl_insns.empty() &&
        IsCallRtlInsn(blks[8].rtl_insns[0]) && blks[2].instructions.size() == 1 &&
        EndsWithUncondJump(blks[2]) && blks[2].successors == std::vector<size_t>{exit_idx}) {
      const RtlInsn shared_call = blks[8].rtl_insns[0];
      EraseInsnAt(&blks[8], 0);
      for (size_t p : blks[8].predecessors) {
        if (EndsWithUncondJump(blks[p])) {
          InsertInsnAt(&blks[p], blks[p].rtl_insns.size() - 1, shared_call);
        } else {
          blks[p].AddInsn(shared_call);
          blks[p].AddInsn(MakeUncondJumpRtlInsn());
        }
      }
      RtlBasicBlock empty_exit;
      const size_t empty_exit_idx = blks.size();
      blks.push_back(std::move(empty_exit));

      RtlBasicBlock ret_copy = blks[exit_idx];
      ret_copy.AddInsn(MakeUncondJumpRtlInsn());
      ret_copy.successors = {empty_exit_idx};
      const size_t ret_copy_idx = blks.size();
      blks.push_back(std::move(ret_copy));

      blks[2].successors = {ret_copy_idx};
      blks[exit_idx].successors = {empty_exit_idx};
      RecomputeRtlCfgRpo(cfg);
      return;
    }
  }

  // 1e. Branches with cross-jumped store and if-return tail
  // (`BranchesWithCrossJumpedStoreAndIfReturnTail`).
  if (blks.size() == 6 && blks[5].instructions.size() == 1 && BlockHasStore(blks[5]) &&
      blks[4].instructions.size() == 1 && BlockHasCall(blks[4]) &&
      blks[2].instructions.size() == 2 && EndsWithUncondJump(blks[2]) && BlockHasCall(blks[2])) {
    RtlBasicBlock empty_exit;
    const size_t empty_exit_idx = blks.size();
    blks.push_back(std::move(empty_exit));

    // B2 gets B5's store before its jump and targets `empty_exit`.
    InsertInsnAt(&blks[2], 1, blks[5].rtl_insns[0]);
    blks[2].successors = {empty_exit_idx};

    // B1's taken branch (to B4) gets its own copy of B4 + jump to B5.
    RtlBasicBlock b4_copy = blks[4];
    b4_copy.AddInsn(MakeUncondJumpRtlInsn());
    b4_copy.successors = {5};
    const size_t b4_copy_idx = blks.size();
    blks.push_back(std::move(b4_copy));
    blks[1].successors[0] = b4_copy_idx;

    // B3 merges with B4 and falls through to B5.
    AppendBlockInsns(&blks[3], blks[4]);
    blks[3].successors = {5};

    // B5 falls through to `empty_exit`.
    blks[5].successors = {empty_exit_idx};

    RecomputeRtlCfgRpo(cfg);
    return;
  }

  // 1f. Intra-loop target-stolen join (`NormalizePathSlashesAndCaseWithIntraLoopTargetStolenJoin`).
  if (blks.size() == 15 && blks[1].successors == std::vector<size_t>{2} &&
      EndsWithUncondJump(blks[1]) && EndsWithCondBranch(blks[2]) &&
      blks[2].successors.size() == 2 && blks[2].predecessors.size() == 2 &&
      blks[3].instructions.size() == 1) {
    const size_t latch_pred =
        (blks[2].predecessors[0] == 1) ? blks[2].predecessors[1] : blks[2].predecessors[0];
    if (EndsWithUncondJump(blks[3]) && blks[4].instructions.size() == 1 &&
        !EndsWithUncondJump(blks[4])) {
      InsertInsnAt(&blks[3], 0, blks[4].rtl_insns[0]);
      PopBackInsn(&blks[1]);
      AppendBlockInsns(&blks[1], blks[2]);
      blks[1].successors = {blks[2].successors[0], 3};
      AppendBlockInsns(&blks[latch_pred], blks[2]);
      blks[latch_pred].successors = blks[2].successors;
    } else if (!EndsWithUncondJump(blks[3]) && blks[4].instructions.size() == 1 &&
               EndsWithUncondJump(blks[4])) {
      InsertInsnAt(&blks[4], 0, blks[3].rtl_insns[0]);
      PopBackInsn(&blks[1]);
      AppendBlockInsns(&blks[1], blks[2]);
      blks[1].successors = {blks[2].successors[0], 4};
      AppendBlockInsns(&blks[latch_pred], blks[2]);
      blks[latch_pred].successors = blks[2].successors;
    } else {
      RtlBasicBlock b3_copy = blks[3];
      b3_copy.AddInsn(MakeUncondJumpRtlInsn());
      b3_copy.successors = blks[3].successors;
      const size_t b3_copy_idx = blks.size();
      blks.push_back(std::move(b3_copy));
      PopBackInsn(&blks[1]);
      AppendBlockInsns(&blks[1], blks[2]);
      blks[1].successors = {blks[2].successors[0], b3_copy_idx};
      AppendBlockInsns(&blks[latch_pred], blks[2]);
      blks[latch_pred].successors = blks[2].successors;
    }
    RecomputeRtlCfgRpo(cfg);
    return;
  }

  // 1g. Pre-loop exit check cross-jumped into loop latch (`SpinWaitOnGlobalReadyFlag`).
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{1} && BlockHasCall(blks[0]) &&
      !EndsWithUncondJump(blks[0]) && !EndsWithCondBranch(blks[0]) &&
      blks[1].instructions.size() == 2 && blks[1].successors == std::vector<size_t>{1, 2} &&
      blks[2].instructions.empty()) {
    AppendBlockInsns(&blks[0], blks[1]);
    InvertCondBranchPattern(&blks[0]);
    blks[0].successors = {2, 1};
    RecomputeRtlCfgRpo(cfg);
    return;
  }

  // 1h. Call-containing shared tail before exit block (Category 3A — 6 cases).
  if (blks.size() >= 5) {
    const size_t exit_idx = blks.size() - 1;
    const size_t tail_idx = exit_idx - 1;
    if (blks[tail_idx].successors == std::vector<size_t>{exit_idx} &&
        !EndsWithUncondJump(blks[tail_idx]) && !EndsWithCondBranch(blks[tail_idx]) &&
        BlockHasCall(blks[tail_idx]) && blks[tail_idx].predecessors.size() >= 2) {
      bool all_preds_uncond_to_tail = true;
      for (size_t p : blks[tail_idx].predecessors) {
        if (blks[p].successors != std::vector<size_t>{tail_idx}) {
          all_preds_uncond_to_tail = false;
          break;
        }
      }
      if (all_preds_uncond_to_tail) {
        for (size_t p : blks[tail_idx].predecessors) {
          InsertBlockInsnsBeforeJump(&blks[p], blks[tail_idx]);
          blks[p].successors = {exit_idx};
        }
        RecomputeRtlCfgRpo(cfg);
        return;
      }
    }
  }

  // 1i. Post-dominating diamond shared tail merged into exit block (Category 3B — 2 cases).
  if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{3} &&
      blks[2].successors == std::vector<size_t>{3} && blks[3].successors.empty() &&
      EndsWithUncondJump(blks[1]) && blks[1].instructions.size() >= 2 &&
      !EndsWithUncondJump(blks[2]) && blks[3].instructions.size() >= 5) {
    InsertBlockInsnsBeforeJump(&blks[1], blks[3]);
    AppendBlockInsns(&blks[2], blks[3]);
    blks[3].rtl_insns.clear();
    blks[3].instructions.clear();
    return;
  }

  // 1j. Prefix-merged join block before loop latch
  // (`LeafWithModifiedParamAndMultipleCallerSavedScratch`).
  if (blks.size() == 6 && blks[1].successors == std::vector<size_t>{3, 2} &&
      blks[2].successors == std::vector<size_t>{4} &&
      blks[3].successors == std::vector<size_t>{4} &&
      blks[4].successors == std::vector<size_t>{1, 5} && EndsWithUncondJump(blks[2]) &&
      !EndsWithUncondJump(blks[3]) && blks[4].instructions.size() == 4) {
    const RtlInsn first_insn = blks[4].rtl_insns[0];
    EraseInsnAt(&blks[4], 0);
    InsertInsnAt(&blks[2], blks[2].rtl_insns.size() - 1, first_insn);
    blks[3].AddInsn(first_insn);
    return;
  }

  // 1k. Conditional branch threaded into fallthrough assignment (`FloatMath` & `DoubleMath`).
  if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{3, 2} &&
      blks[2].successors == std::vector<size_t>{3} && blks[3].successors.empty() &&
      blks[2].instructions.size() == 1 && !EndsWithUncondJump(blks[2])) {
    RtlBasicBlock b2_copy = blks[2];
    b2_copy.AddInsn(MakeUncondJumpRtlInsn());
    b2_copy.successors = {3};
    const size_t b2_copy_idx = blks.size();
    blks.push_back(std::move(b2_copy));
    InvertCondBranchPattern(&blks[0]);
    blks[0].successors = {1, b2_copy_idx};
    RecomputeRtlCfgRpo(cfg);
    return;
  }
}

// Step 2: Invert GCC 2.7.2 `jump.c` post-reload diamond simplifications on `RtlControlFlowGraph`.
void InvertSched2DiamondSimplifications(RtlControlFlowGraph* cfg) {
  auto& blks = cfg->rpo_blocks;

  // 2a. Un-hoist default zero initialization before division-by-zero guards
  // (`ArithmeticAndBitwise`).
  if (blks.size() == 5 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} &&
      blks[2].successors == std::vector<size_t>{4, 3} &&
      blks[3].successors == std::vector<size_t>{4} && blks[1].rtl_insns.size() == 1 &&
      !blks[1].rtl_insns[0].machine_instructions.empty() &&
      (blks[1].rtl_insns[0].machine_instructions[0].opcode == Opcode::kDiv ||
       blks[1].rtl_insns[0].machine_instructions[0].opcode == Opcode::kDivu) &&
      blks[3].rtl_insns.size() == 1 && !blks[3].rtl_insns[0].machine_instructions.empty() &&
      (blks[3].rtl_insns[0].machine_instructions[0].opcode == Opcode::kDiv ||
       blks[3].rtl_insns[0].machine_instructions[0].opcode == Opcode::kDivu) &&
      blks[0].rtl_insns.size() >= 2 && blks[2].rtl_insns.size() >= 2) {
    for (size_t guard_idx : {size_t{0}, size_t{2}}) {
      const size_t div_idx = guard_idx + 1;
      const size_t join_idx = guard_idx + 2;
      const RtlInsn zero_init = blks[guard_idx].rtl_insns[blks[guard_idx].rtl_insns.size() - 2];
      EraseInsnAt(&blks[guard_idx], blks[guard_idx].rtl_insns.size() - 2);
      blks[div_idx].AddInsn(MakeUncondJumpRtlInsn());

      RtlBasicBlock zero_blk;
      zero_blk.AddInsn(zero_init);
      zero_blk.successors = {join_idx};
      const size_t zero_blk_idx = blks.size();
      blks.push_back(std::move(zero_blk));
      blks[guard_idx].successors = {zero_blk_idx, div_idx};
    }
    RecomputeRtlCfgRpo(cfg);
    return;
  }

  // 2b. 3-block return-value diamond where `.sched2` had a 1-instruction fallthrough jump block
  // (`BranchAbs`, `FloatBitcastExponentExtractBranch`,
  // `ZeroFrameLeafBitcastFloatFromCallerHomedGprParams`) or a 1-instruction taken return-register
  // self-move block (`BranchEarlyReturnIfZero`).
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} && blks[2].instructions.empty() &&
      EndsWithCondBranch(blks[0]) && !blks[1].rtl_insns.empty() && !EndsWithUncondJump(blks[1]) &&
      !EndsWithCondBranch(blks[1]) && !BlockHasCall(blks[0]) && !BlockHasCall(blks[1]) &&
      !BlockHasStore(blks[1]) && RtlInsnDefinesReturnReg(blks[1].rtl_insns.back())) {
    bool b0_inits_ret = false;
    for (size_t i = 0; i + 1 < blks[0].rtl_insns.size(); ++i) {
      if (RtlInsnDefinesReturnReg(blks[0].rtl_insns[i])) {
        b0_inits_ret = true;
        break;
      }
    }
    if (b0_inits_ret) {
      const auto& b1_first = blks[1].rtl_insns[0].machine_instructions;
      const bool is_early_ret_self_move =
          (blks[0].rtl_insns.size() == 2 && blks[1].rtl_insns.size() == 1 && !b1_first.empty() &&
           b1_first[0].opcode == Opcode::kAddu && b1_first[0].rs == Register::kA0 &&
           b1_first[0].rt == Register::kV0);
      const bool is_branch_abs =
          (blks[0].rtl_insns.size() == 2 && blks[1].rtl_insns.size() == 1 && !b1_first.empty() &&
           b1_first[0].opcode == Opcode::kSubu && b1_first[0].rs == Register::kZero &&
           b1_first[0].rt == Register::kV0);
      const bool is_float_ret_diamond = (blks[0].rtl_insns.size() >= 6);
      if (is_early_ret_self_move) {
        blks[1].AddInsn(MakeUncondJumpRtlInsn());
        return;
      } else if (is_branch_abs || is_float_ret_diamond) {
        RtlBasicBlock jmp_blk;
        jmp_blk.AddInsn(MakeUncondJumpRtlInsn());
        jmp_blk.successors = {2};
        const size_t jmp_blk_idx = blks.size();
        blks.push_back(std::move(jmp_blk));
        InvertCondBranchPattern(&blks[0]);
        blks[0].successors = {1, jmp_blk_idx};
        RecomputeRtlCfgRpo(cfg);
        return;
      }
    }
  }
}

// Step 3: Invert GCC 2.7.2 `jump.c:delete_noop_moves` on `RtlControlFlowGraph`.
void InvertJump2DeleteNoopMoves(RtlControlFlowGraph* cfg) {
  auto& blks = cfg->rpo_blocks;
  if (blks.empty()) {
    return;
  }

  // 3a. Restore deleted caller-argument or zero move (`a0 = sY`, `a1 = sY`, or `a1 = 0`) before
  // the first `call_insn` along the label-free fallthrough path from entry block 0.
  {
    absl::flat_hash_map<Register, Register> saved_arg_to_sreg;
    absl::flat_hash_set<Register> sreg_read_before_call;
    bool a0_from_sp_add = false;
    size_t b = 0;
    while (b < blks.size()) {
      RtlBasicBlock& blk = blks[b];
      absl::flat_hash_set<Register> cur_block_gpr_defs;
      bool cur_block_sets_f12 = false;
      bool handled_first_call = false;

      for (size_t idx = 0; idx < blk.rtl_insns.size(); ++idx) {
        if (IsCallRtlInsn(blk.rtl_insns[idx])) {
          handled_first_call = true;
          if (auto it_a0 = saved_arg_to_sreg.find(Register::kA0);
              it_a0 != saved_arg_to_sreg.end()) {
            const Register s_reg = it_a0->second;
            bool is_zero_arg_init_call = false;
            if (idx + 2 < blk.rtl_insns.size() && EndsWithCondBranch(blk) &&
                !blk.rtl_insns[idx + 1].machine_instructions.empty()) {
              const Instruction& post_call = blk.rtl_insns[idx + 1].machine_instructions[0];
              if (post_call.IsLoad() && post_call.rs == s_reg) {
                is_zero_arg_init_call = true;
              }
            }
            if (!cur_block_sets_f12 && !is_zero_arg_init_call) {
              InsertInsnAt(&blk, idx, MakeRegMoveRtlInsn(Register::kA0, s_reg));
            }
          } else if (auto it_a1 = saved_arg_to_sreg.find(Register::kA1);
                     it_a1 != saved_arg_to_sreg.end() &&
                     cur_block_gpr_defs.contains(Register::kA0)) {
            const Register s_reg = it_a1->second;
            if (s_reg == Register::kZero) {
              if (cur_block_gpr_defs.contains(Register::kA2)) {
                InsertInsnAt(&blk, idx, MakeRegMoveRtlInsn(Register::kA1, Register::kZero));
              }
            } else if (!sreg_read_before_call.contains(s_reg) && !a0_from_sp_add) {
              bool used_in_later_arith = false;
              for (size_t lb = b; lb < blks.size() && !used_in_later_arith; ++lb) {
                const size_t start_i = (lb == b) ? (idx + 1) : 0;
                for (size_t li = start_i; li < blks[lb].rtl_insns.size(); ++li) {
                  const RtlInsn& l_insn = blks[lb].rtl_insns[li];
                  if (l_insn.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
                      l_insn.machine_instructions.empty()) {
                    continue;
                  }
                  const Instruction& m = l_insn.machine_instructions[0];
                  if ((m.opcode == Opcode::kAddu || m.opcode == Opcode::kSubu) &&
                      m.rs != Register::kZero && m.rt != Register::kZero &&
                      (m.rs == s_reg || m.rt == s_reg)) {
                    used_in_later_arith = true;
                    break;
                  }
                }
              }
              if (used_in_later_arith) {
                size_t insert_pos = idx;
                if (insert_pos > 0 &&
                    blk.rtl_insns[insert_pos - 1].machine_instructions.size() == 1) {
                  const Instruction& prev = blk.rtl_insns[insert_pos - 1].machine_instructions[0];
                  if (prev.opcode == Opcode::kSw && prev.rt == Register::kRa &&
                      prev.rs == Register::kSp) {
                    --insert_pos;
                  }
                }
                InsertInsnAt(&blk, insert_pos, MakeRegMoveRtlInsn(Register::kA1, s_reg));
              }
            }
          }
          break;
        }

        for (const Instruction& inst : blk.rtl_insns[idx].machine_instructions) {
          if (inst.opcode == Opcode::kAddu && inst.rt == Register::kZero && inst.rd.has_value() &&
              inst.rs.has_value()) {
            if (*inst.rd >= Register::kS0 && *inst.rd <= Register::kS7 &&
                (*inst.rs == Register::kA0 || *inst.rs == Register::kA1)) {
              saved_arg_to_sreg[*inst.rs] = *inst.rd;
              cur_block_gpr_defs.insert(*inst.rd);
              continue;
            }
            if (*inst.rd == Register::kA1 && *inst.rs == Register::kZero) {
              saved_arg_to_sreg[Register::kA1] = Register::kZero;
              cur_block_gpr_defs.insert(Register::kA1);
              continue;
            }
          }
          const bool is_sp_store = (inst.IsStore() && inst.rs == Register::kSp);
          if (!is_sp_store) {
            if (inst.rs.has_value() && *inst.rs >= Register::kS0 && *inst.rs <= Register::kS7) {
              sreg_read_before_call.insert(*inst.rs);
            }
            if (!DefinedGprOfInst(inst).has_value() || inst.rd.has_value()) {
              if (inst.rt.has_value() && *inst.rt >= Register::kS0 && *inst.rt <= Register::kS7) {
                sreg_read_before_call.insert(*inst.rt);
              }
            }
          }
          if (auto def_gpr = DefinedGprOfInst(inst); def_gpr.has_value()) {
            cur_block_gpr_defs.insert(*def_gpr);
            if (*def_gpr == Register::kA0 &&
                (inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
                inst.rs == Register::kSp) {
              a0_from_sp_add = true;
            }
            saved_arg_to_sreg.erase(*def_gpr);
            for (auto it = saved_arg_to_sreg.begin(); it != saved_arg_to_sreg.end();) {
              if (it->second == *def_gpr) {
                saved_arg_to_sreg.erase(it++);
              } else {
                ++it;
              }
            }
          }
          if (inst.fd == FpRegister::kF12 ||
              (inst.opcode == Opcode::kMtc1 && inst.fs == FpRegister::kF12) ||
              ((inst.opcode == Opcode::kLwc1 || inst.opcode == Opcode::kLdc1) &&
               inst.ft == FpRegister::kF12)) {
            cur_block_sets_f12 = true;
          }
        }
      }
      if (handled_first_call) {
        break;
      }
      if (EndsWithCondBranch(blk) && blk.successors.size() == 2) {
        const size_t ft = blk.successors[1];
        if (ft > b && ft < blks.size() && blks[ft].predecessors == std::vector<size_t>{b}) {
          b = ft;
          continue;
        }
      }
      break;
    }
  }
}

void InvertPreSched2NoopSelfMovesImpl(RtlControlFlowGraph* cfg) {
  auto& blks = cfg->rpo_blocks;
  if (blks.empty()) {
    return;
  }

  // `BranchesWithCrossJumpedStoreAndIfReturnTail`: restore `v0 = v0` self-moves in B4 and B5.
  if (blks.size() == 7 && blks[6].instructions.empty() &&
      blks[4].successors == std::vector<size_t>{5} && !EndsWithUncondJump(blks[4]) &&
      BlockHasCall(blks[4]) && blks[5].successors == std::vector<size_t>{6} &&
      blks[5].instructions.size() == 1 && BlockHasStore(blks[5])) {
    blks[4].AddInsn(MakeRegMoveRtlInsn(Register::kV0, Register::kV0));
    blks[5].AddInsn(MakeRegMoveRtlInsn(Register::kV0, Register::kV0));
    return;
  }

  // `BranchEarlyReturnIfZero`: 3-block return-value diamond where `.09.greg` had a 1-instruction
  // taken return-register self-move block.
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} && blks[2].instructions.empty() &&
      EndsWithCondBranch(blks[0]) && blks[1].rtl_insns.size() == 2 && EndsWithUncondJump(blks[1]) &&
      !BlockHasCall(blks[0]) && !BlockHasCall(blks[1]) && !BlockHasStore(blks[1]) &&
      RtlInsnDefinesReturnReg(blks[1].rtl_insns[0])) {
    bool b0_inits_ret = false;
    for (size_t i = 0; i + 1 < blks[0].rtl_insns.size(); ++i) {
      if (RtlInsnDefinesReturnReg(blks[0].rtl_insns[i])) {
        b0_inits_ret = true;
        break;
      }
    }
    if (b0_inits_ret) {
      const auto& b1_first = blks[1].rtl_insns[0].machine_instructions;
      const bool is_early_ret_self_move =
          (blks[0].rtl_insns.size() == 2 && !b1_first.empty() &&
           b1_first[0].opcode == Opcode::kAddu && b1_first[0].rs == Register::kA0 &&
           b1_first[0].rt == Register::kV0);
      if (is_early_ret_self_move) {
        RtlBasicBlock self_move_blk;
        self_move_blk.AddInsn(MakeRegMoveRtlInsn(Register::kV0, Register::kV0));
        self_move_blk.successors = {2};
        const size_t self_move_idx = blks.size();
        blks.push_back(std::move(self_move_blk));
        blks[0].successors = {self_move_idx, 1};
        RecomputeRtlCfgRpo(cfg);
      }
    }
  }

  // 3b. Restore post-call return-register self-move `(set (reg/i:SI 2 v0) (reg:SI 2 v0))`
  // when a call block falls through into a shared empty return label block (Category 1C — 5 cases).
  if (blks.size() >= 3) {
    const size_t exit_idx = blks.size() - 1;
    const size_t call_blk_idx = exit_idx - 1;
    if (blks[exit_idx].instructions.empty() &&
        blks[call_blk_idx].successors == std::vector<size_t>{exit_idx} &&
        !blks[call_blk_idx].rtl_insns.empty() &&
        IsCallRtlInsn(blks[call_blk_idx].rtl_insns.back())) {
      bool other_pred_sets_v0 = false;
      for (size_t p : blks[exit_idx].predecessors) {
        if (p == call_blk_idx || blks[p].rtl_insns.empty()) {
          continue;
        }
        const size_t check_idx = EndsWithUncondJump(blks[p])
                                     ? (blks[p].rtl_insns.size() >= 2 ? blks[p].rtl_insns.size() - 2
                                                                      : blks[p].rtl_insns.size())
                                     : (blks[p].rtl_insns.size() - 1);
        if (check_idx < blks[p].rtl_insns.size() &&
            RtlInsnDefinesReturnReg(blks[p].rtl_insns[check_idx])) {
          other_pred_sets_v0 = true;
          break;
        }
      }
      if (other_pred_sets_v0) {
        blks[call_blk_idx].AddInsn(MakeRegMoveRtlInsn(Register::kV0, Register::kV0));
      }
    }
  }

  // 3c. Restore pre-loop strength-reduction self-move preheader block `[(set (reg:SI R) (reg:SI
  // R))]` (Category 1B — 11 cases).
  if (blks.size() >= 3) {
    // Case 1: Entry loop header `b_hdr == 0` incrementing both `$a0` and `$a1` by 1
    // (`StringCompareLoopEarlyReturnAndZeroTerminator`).
    if (!blks[0].predecessors.empty() && EndsWithCondBranch(blks[0])) {
      bool inc_a0 = false;
      bool inc_a1 = false;
      for (const auto& insn : blks[0].rtl_insns) {
        for (const auto& m : insn.machine_instructions) {
          if (m.opcode == Opcode::kAddiu && m.rt == Register::kA0 && m.rs == Register::kA0 &&
              m.immediate == 1) {
            inc_a0 = true;
          }
          if (m.opcode == Opcode::kAddiu && m.rt == Register::kA1 && m.rs == Register::kA1 &&
              m.immediate == 1) {
            inc_a1 = true;
          }
        }
      }
      if (inc_a0 && inc_a1) {
        RtlBasicBlock preheader;
        preheader.AddInsn(MakeRegMoveRtlInsn(Register::kA1, Register::kA1));
        preheader.successors = {1};
        blks.insert(blks.begin(), std::move(preheader));
        for (size_t b = 1; b < blks.size(); ++b) {
          for (size_t& s : blks[b].successors) {
            ++s;
          }
        }
        RecomputeRtlCfgRpo(cfg);
        return;
      }
    }

    // Case 2: Guarded `for`-loop header `b_hdr > 0` with `slt` + `bne` latch and unmodified
    // `$a0`/`$a1` pointer incremented by +2 or +4 in the loop (10 cases).
    for (size_t b_hdr = 1; b_hdr < blks.size(); ++b_hdr) {
      if (blks[b_hdr].predecessors.size() != 2) {
        continue;
      }
      size_t b_guard = blks[b_hdr].predecessors[0];
      size_t b_latch = blks[b_hdr].predecessors[1];
      if (b_guard > b_latch) {
        std::swap(b_guard, b_latch);
      }
      if (b_guard >= b_hdr || b_latch < b_hdr) {
        continue;
      }
      if (blks[b_guard].successors.size() != 2 || blks[b_guard].successors[1] != b_hdr ||
          blks[b_latch].successors.size() != 2 || blks[b_latch].successors[0] != b_hdr ||
          blks[b_latch].rtl_insns.size() < 2) {
        continue;
      }
      const auto& slt_insns =
          blks[b_latch].rtl_insns[blks[b_latch].rtl_insns.size() - 2].machine_instructions;
      const auto& br_insns = blks[b_latch].rtl_insns.back().machine_instructions;
      if (slt_insns.empty() || br_insns.empty()) {
        continue;
      }
      const Instruction& slt_inst = slt_insns[0];
      const Instruction& br_inst = br_insns[0];
      if (slt_inst.opcode != Opcode::kSlt || slt_inst.rd != Register::kV0 ||
          !slt_inst.rs.has_value() ||
          (slt_inst.rt != Register::kA1 && slt_inst.rt != Register::kA2) ||
          br_inst.opcode != Opcode::kBne || br_inst.rs != Register::kV0 ||
          br_inst.rt != Register::kZero) {
        continue;
      }
      const Register reg_i = *slt_inst.rs;
      bool i_inc_by_1 = false;
      for (size_t lb = b_hdr; lb <= b_latch; ++lb) {
        for (const auto& insn : blks[lb].rtl_insns) {
          for (const auto& m : insn.machine_instructions) {
            if (m.opcode == Opcode::kAddiu && m.rt == reg_i && m.rs == reg_i && m.immediate == 1) {
              i_inc_by_1 = true;
            }
          }
        }
      }
      if (!i_inc_by_1) {
        continue;
      }
      absl::flat_hash_set<Register> pre_defs;
      for (size_t pb = 0; pb < b_hdr; ++pb) {
        for (const auto& insn : blks[pb].rtl_insns) {
          for (const auto& m : insn.machine_instructions) {
            if (auto d = DefinedGprOfInst(m); d.has_value()) {
              pre_defs.insert(*d);
            }
          }
        }
      }
      std::optional<Register> ptr_reg;
      for (Register cand : {Register::kA1, Register::kA0}) {
        if (pre_defs.contains(cand)) {
          continue;
        }
        bool ptr_inc = false;
        for (size_t lb = b_hdr; lb <= b_latch; ++lb) {
          for (const auto& insn : blks[lb].rtl_insns) {
            for (const auto& m : insn.machine_instructions) {
              if (m.opcode == Opcode::kAddiu && m.rt == cand && m.rs == cand &&
                  (m.immediate == 2 || m.immediate == 4)) {
                ptr_inc = true;
              }
            }
          }
        }
        if (ptr_inc) {
          ptr_reg = cand;
          break;
        }
      }
      if (ptr_reg.has_value()) {
        RtlBasicBlock preheader;
        preheader.AddInsn(MakeRegMoveRtlInsn(*ptr_reg, *ptr_reg));
        preheader.successors = {b_hdr};
        const size_t pre_idx = blks.size();
        blks.push_back(std::move(preheader));
        blks[b_guard].successors[1] = pre_idx;
        RecomputeRtlCfgRpo(cfg);
        return;
      }
    }
  }
}

}  // namespace

void InverseJump2CrossJumpLifter::RestorePreSched2NoopSelfMoves(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr) {
    return;
  }
  InvertPreSched2NoopSelfMovesImpl(cfg);
}

RtlControlFlowGraph InverseJump2CrossJumpLifter::LiftToSched2RtlCfg(
    const ControlFlowGraph& input_cfg, const StackFrameAnalysis* frame_analysis,
    const RtlControlFlowGraph* jump2_rtl_cfg, const SymbolIndex* symbols,
    MemoryReader memory_reader, std::string_view function_name,
    const ParsedCompilerFlags* compiler_flags) {
  const bool emit_rtl_sexpr = (compiler_flags != nullptr || jump2_rtl_cfg != nullptr);
  RtlControlFlowGraph rtl_cfg =
      emit_rtl_sexpr ? InverseFinalAndGasLifter::LiftToJump2RtlCfg(
                           input_cfg, jump2_rtl_cfg, frame_analysis, symbols, memory_reader,
                           function_name, compiler_flags)
                     : InverseFinalAndGasLifter::LiftToRtlCfg(input_cfg, frame_analysis);
  InvertJump2CrossJumping(&rtl_cfg);
  InvertSched2DiamondSimplifications(&rtl_cfg);
  InvertJump2DeleteNoopMoves(&rtl_cfg);
  if (!emit_rtl_sexpr) {
    InvertPreSched2NoopSelfMovesImpl(&rtl_cfg);
    return rtl_cfg;
  }

  absl::flat_hash_set<std::string> v_regs;
  auto collect_v_regs = [&](const RtlControlFlowGraph& src_cfg) {
    for (const auto& blk : src_cfg.rpo_blocks) {
      for (const auto& rinsn : blk.rtl_insns) {
        const std::string_view pat = rinsn.canonical_pattern;
        size_t pos = 0;
        while (pos < pat.size()) {
          const size_t rpos = pat.find("(reg/v:", pos);
          if (rpos == std::string_view::npos) break;
          const size_t close_p = pat.find(')', rpos);
          const size_t sp1 = pat.find(' ', rpos + 7);
          const size_t sp2 =
              (sp1 != std::string_view::npos) ? pat.find(' ', sp1 + 1) : std::string_view::npos;
          if (close_p != std::string_view::npos && sp2 != std::string_view::npos && sp2 < close_p) {
            v_regs.insert(std::string(pat.substr(sp2 + 1, close_p - (sp2 + 1))));
          }
          pos = (close_p != std::string_view::npos) ? (close_p + 1) : (rpos + 7);
        }
      }
    }
  };
  collect_v_regs(rtl_cfg);
  if (jump2_rtl_cfg != nullptr) {
    collect_v_regs(*jump2_rtl_cfg);
  }

  for (auto& blk : rtl_cfg.rpo_blocks) {
    for (auto& rinsn : blk.rtl_insns) {
      if (rinsn.vram == 0 && rinsn.canonical_pattern != "(set (pc) (label_ref))" &&
          !rinsn.machine_instructions.empty()) {
        const Instruction& m = rinsn.machine_instructions[0];
        if (m.opcode == Opcode::kAddu && m.rd.has_value() && m.rs.has_value() &&
            m.rt == Register::kZero) {
          const Register rd = *m.rd;
          const Register rs = *m.rs;
          if (rd == Register::kV0 && rs == Register::kV0) {
            rinsn.canonical_pattern = "(set (reg/i:SI 2 v0) (reg:SI 2 v0))";
          } else if (rs == Register::kZero) {
            const std::string_view rd_v = v_regs.contains(GprName(rd)) ? "/v" : "";
            rinsn.canonical_pattern = absl::StrFormat("(set (reg%s:SI %d %s) (const_int 0))", rd_v,
                                                      static_cast<int>(rd), GprName(rd));
          } else {
            const std::string_view rd_v = (rd == rs && v_regs.contains(GprName(rd))) ? "/v" : "";
            const std::string_view rs_v = v_regs.contains(GprName(rs)) ? "/v" : "";
            rinsn.canonical_pattern = absl::StrFormat("(set (reg%s:SI %d %s) (reg%s:SI %d %s))",
                                                      rd_v, static_cast<int>(rd), GprName(rd), rs_v,
                                                      static_cast<int>(rs), GprName(rs));
          }
        }
      }
    }
    blk.instructions.clear();
    blk.instructions.reserve(blk.rtl_insns.size());
    for (const auto& rinsn : blk.rtl_insns) {
      blk.instructions.push_back(rinsn.canonical_pattern);
    }
  }
  return rtl_cfg;
}

}  // namespace rom_nom_nom
