#include "lifter/cfg_de_reorg.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "lifter/abi_analyzer.h"
#include "lifter/branch_condition_builder.h"
#include "lifter/cfg_analysis_manager.h"
#include "lifter/cfg_pass_pipeline.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {
namespace {

Instruction MakeNopInstruction(uint32_t vram,
                               std::optional<Register> unscheduled_clobber_reg = std::nullopt) {
  Instruction nop;
  nop.raw_word = 0;
  nop.vram = vram;
  nop.opcode = Opcode::kSll;
  nop.rd = Register::kZero;
  nop.rt = Register::kZero;
  nop.unscheduled_clobber_reg = unscheduled_clobber_reg;
  nop.shift_amount = 0;
  return nop;
}

Opcode ToNonLikelyBranchOpcode(Opcode op) {
  switch (op) {
    case Opcode::kBeql:
      return Opcode::kBeq;
    case Opcode::kBnel:
      return Opcode::kBne;
    case Opcode::kBlezl:
      return Opcode::kBlez;
    case Opcode::kBgtzl:
      return Opcode::kBgtz;
    case Opcode::kBltzl:
      return Opcode::kBltz;
    case Opcode::kBgezl:
      return Opcode::kBgez;
    case Opcode::kBc1tl:
      return Opcode::kBc1t;
    case Opcode::kBc1fl:
      return Opcode::kBc1f;
    default:
      return op;
  }
}

Opcode ToLikelyBranchOpcode(Opcode op) {
  switch (op) {
    case Opcode::kBeq:
      return Opcode::kBeql;
    case Opcode::kBne:
      return Opcode::kBnel;
    case Opcode::kBlez:
      return Opcode::kBlezl;
    case Opcode::kBgtz:
      return Opcode::kBgtzl;
    case Opcode::kBltz:
      return Opcode::kBltzl;
    case Opcode::kBgez:
      return Opcode::kBgezl;
    case Opcode::kBc1t:
      return Opcode::kBc1tl;
    case Opcode::kBc1f:
      return Opcode::kBc1fl;
    default:
      return op;
  }
}

bool SameDelayInstructionEffect(const Instruction& a, const Instruction& b) {
  return !a.IsNop() && a.opcode == b.opcode && a.rs == b.rs && a.rt == b.rt && a.rd == b.rd &&
         a.immediate == b.immediate && a.fs == b.fs && a.ft == b.ft && a.fd == b.fd &&
         a.shift_amount == b.shift_amount;
}

void RetargetBranchEdge(ControlFlowGraph* cfg, uint32_t from_block_id, uint32_t old_to_block_id,
                        uint32_t new_to_block_id) {
  BasicBlock* from_blk = cfg->GetMutableBlock(from_block_id);
  BasicBlock* old_to_blk = cfg->GetMutableBlock(old_to_block_id);
  BasicBlock* new_to_blk = cfg->GetMutableBlock(new_to_block_id);
  if (from_blk == nullptr || old_to_blk == nullptr || new_to_blk == nullptr) {
    return;
  }
  for (uint32_t& succ : from_blk->successors) {
    if (succ == old_to_block_id) {
      succ = new_to_block_id;
    }
  }
  for (auto& edge : from_blk->outgoing_edges) {
    if ((edge.type == EdgeType::kBranch || edge.type == EdgeType::kJump) &&
        edge.to_block_id == old_to_block_id) {
      edge.to_block_id = new_to_block_id;
    }
  }
  for (auto& edge : cfg->MutableEdges()) {
    if (edge.from_block_id == from_block_id &&
        (edge.type == EdgeType::kBranch || edge.type == EdgeType::kJump) &&
        edge.to_block_id == old_to_block_id) {
      edge.to_block_id = new_to_block_id;
    }
  }
  old_to_blk->predecessors.erase(
      std::remove(old_to_blk->predecessors.begin(), old_to_blk->predecessors.end(), from_block_id),
      old_to_blk->predecessors.end());
  if (std::find(new_to_blk->predecessors.begin(), new_to_blk->predecessors.end(), from_block_id) ==
      new_to_blk->predecessors.end()) {
    new_to_blk->predecessors.push_back(from_block_id);
  }
}

void RemoveBranchEdge(ControlFlowGraph* cfg, uint32_t from_block_id, uint32_t old_to_block_id) {
  BasicBlock* from_blk = cfg->GetMutableBlock(from_block_id);
  BasicBlock* old_to_blk = cfg->GetMutableBlock(old_to_block_id);
  if (from_blk == nullptr || old_to_blk == nullptr) {
    return;
  }
  from_blk->successors.erase(
      std::remove(from_blk->successors.begin(), from_blk->successors.end(), old_to_block_id),
      from_blk->successors.end());
  from_blk->outgoing_edges.erase(
      std::remove_if(from_blk->outgoing_edges.begin(), from_blk->outgoing_edges.end(),
                     [&](const CfgEdge& edge) {
                       return (edge.type == EdgeType::kBranch || edge.type == EdgeType::kJump) &&
                              edge.to_block_id == old_to_block_id;
                     }),
      from_blk->outgoing_edges.end());
  cfg->MutableEdges().erase(
      std::remove_if(cfg->MutableEdges().begin(), cfg->MutableEdges().end(),
                     [&](const CfgEdge& edge) {
                       return edge.from_block_id == from_block_id &&
                              (edge.type == EdgeType::kBranch || edge.type == EdgeType::kJump) &&
                              edge.to_block_id == old_to_block_id;
                     }),
      cfg->MutableEdges().end());
  old_to_blk->predecessors.erase(
      std::remove(old_to_blk->predecessors.begin(), old_to_blk->predecessors.end(), from_block_id),
      old_to_blk->predecessors.end());
}

bool IsInverseAdjustmentInstruction(const Instruction& delay_inst, const Instruction& comp_inst) {
  if (delay_inst.IsNop() || comp_inst.IsNop()) {
    return false;
  }
  if ((delay_inst.opcode == Opcode::kAddiu || delay_inst.opcode == Opcode::kAddi ||
       delay_inst.opcode == Opcode::kDaddiu || delay_inst.opcode == Opcode::kDaddi) &&
      comp_inst.opcode == delay_inst.opcode && delay_inst.rs.has_value() &&
      delay_inst.rt == delay_inst.rs && comp_inst.rs == delay_inst.rs &&
      comp_inst.rt == delay_inst.rt && delay_inst.immediate != 0 &&
      static_cast<int16_t>(comp_inst.immediate) == -static_cast<int16_t>(delay_inst.immediate)) {
    return true;
  }
  auto is_add_sub_pair = [](Opcode a, Opcode b) {
    return (a == Opcode::kAddu && b == Opcode::kSubu) ||
           (a == Opcode::kSubu && b == Opcode::kAddu) || (a == Opcode::kAdd && b == Opcode::kSub) ||
           (a == Opcode::kSub && b == Opcode::kAdd) ||
           (a == Opcode::kDaddu && b == Opcode::kDsubu) ||
           (a == Opcode::kDsubu && b == Opcode::kDaddu);
  };
  return is_add_sub_pair(delay_inst.opcode, comp_inst.opcode) && delay_inst.rd.has_value() &&
         delay_inst.rs == delay_inst.rd && delay_inst.rt != delay_inst.rd &&
         comp_inst.rd == delay_inst.rd && comp_inst.rs == delay_inst.rs &&
         comp_inst.rt == delay_inst.rt;
}

bool IsFpuCompareOpcode(Opcode op) {
  return op == Opcode::kCEqS || op == Opcode::kCEqD || op == Opcode::kCSeqS ||
         op == Opcode::kCSeqD || op == Opcode::kCUeqS || op == Opcode::kCUeqD ||
         op == Opcode::kCNglS || op == Opcode::kCNglD || op == Opcode::kCUnS ||
         op == Opcode::kCUnD || op == Opcode::kCLtS || op == Opcode::kCLtD ||
         op == Opcode::kCOltS || op == Opcode::kCOltD || op == Opcode::kCUltS ||
         op == Opcode::kCUltD || op == Opcode::kCNgeS || op == Opcode::kCNgeD ||
         op == Opcode::kCLeS || op == Opcode::kCLeD || op == Opcode::kCOleS ||
         op == Opcode::kCOleD || op == Opcode::kCUleS || op == Opcode::kCUleD ||
         op == Opcode::kCNgtS || op == Opcode::kCNgtD || op == Opcode::kCNgleS ||
         op == Opcode::kCNgleD;
}

bool IsMultiplyDivideOrHiLoOpcode(Opcode op) {
  switch (op) {
    case Opcode::kMult:
    case Opcode::kMultu:
    case Opcode::kDmult:
    case Opcode::kDmultu:
    case Opcode::kDiv:
    case Opcode::kDivu:
    case Opcode::kDdiv:
    case Opcode::kDdivu:
    case Opcode::kMfhi:
    case Opcode::kMthi:
    case Opcode::kMflo:
    case Opcode::kMtlo:
      return true;
    default:
      return false;
  }
}

bool IsUnalignedMemoryOpcode(Opcode op) {
  switch (op) {
    case Opcode::kLwl:
    case Opcode::kLwr:
    case Opcode::kLdl:
    case Opcode::kLdr:
    case Opcode::kSwl:
    case Opcode::kSwr:
    case Opcode::kSdl:
    case Opcode::kSdr:
      return true;
    default:
      return false;
  }
}

Opcode PartnerUnalignedOpcode(Opcode op) {
  switch (op) {
    case Opcode::kLwl:
      return Opcode::kLwr;
    case Opcode::kLwr:
      return Opcode::kLwl;
    case Opcode::kLdl:
      return Opcode::kLdr;
    case Opcode::kLdr:
      return Opcode::kLdl;
    case Opcode::kSwl:
      return Opcode::kSwr;
    case Opcode::kSwr:
      return Opcode::kSwl;
    case Opcode::kSdl:
      return Opcode::kSdr;
    case Opcode::kSdr:
      return Opcode::kSdl;
    default:
      return Opcode::kUnknown;
  }
}

bool IsPartnerUnalignedPair(const Instruction& first, const Instruction& second) {
  if (!IsUnalignedMemoryOpcode(first.opcode) ||
      second.opcode != PartnerUnalignedOpcode(first.opcode)) {
    return false;
  }
  return first.rt.has_value() && first.rs.has_value() && first.rt == second.rt &&
         first.rs == second.rs;
}

bool IsSinkableDelaySlotInstruction(const BasicBlock& block, const Instruction& delay_inst) {
  if (delay_inst.IsNop() || delay_inst.IsBranch() || delay_inst.IsJump() || delay_inst.IsCall() ||
      delay_inst.IsReturn() || delay_inst.IsStore()) {
    return false;
  }
  if (delay_inst.opcode == Opcode::kLui || delay_inst.opcode == Opcode::kMfc0 ||
      delay_inst.opcode == Opcode::kMtc0 || delay_inst.opcode == Opcode::kCtc1 ||
      delay_inst.opcode == Opcode::kCfc1 || delay_inst.opcode == Opcode::kBreak ||
      delay_inst.opcode == Opcode::kSync || delay_inst.opcode == Opcode::kSyscall ||
      IsMultiplyDivideOrHiLoOpcode(delay_inst.opcode) ||
      IsUnalignedMemoryOpcode(delay_inst.opcode) || IsFpuCompareOpcode(delay_inst.opcode)) {
    return false;
  }

  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  if (delay_ud.gpr_defs.empty() || !delay_ud.fpr_defs.empty()) {
    return false;
  }
  for (Register d : delay_ud.gpr_defs) {
    if (d == Register::kZero || d == Register::kSp || d == Register::kFp || d == Register::kRa ||
        d == Register::kAt) {
      return false;
    }
  }

  for (Register u : delay_ud.gpr_uses) {
    for (int i = static_cast<int>(block.instructions.size()) - 3; i >= 0; --i) {
      const auto& prev = block.instructions[i];
      RegisterUseDef prev_ud = GetInstructionUseDef(prev);
      bool defines_u = false;
      for (Register d : prev_ud.gpr_defs) {
        if (d == u) {
          defines_u = true;
          break;
        }
      }
      if (defines_u) {
        if (prev.opcode == Opcode::kLui) {
          return false;
        }
        break;
      }
    }
  }

  return true;
}

bool IsRegisterDeadAtBlockEntry(const ControlFlowGraph& cfg, uint32_t start_block_id, Register reg,
                                bool returns_v0) {
  if (reg == Register::kZero) {
    return true;
  }
  absl::flat_hash_set<uint32_t> visited;
  std::vector<uint32_t> worklist = {start_block_id};
  while (!worklist.empty()) {
    uint32_t blk_id = worklist.back();
    worklist.pop_back();
    if (!visited.insert(blk_id).second) {
      continue;
    }
    const auto* blk = cfg.GetBlock(blk_id);
    if (blk == nullptr) {
      return false;
    }
    bool killed = false;
    for (size_t i = 0; i < blk->instructions.size(); ++i) {
      const auto& inst = blk->instructions[i];
      if ((inst.IsCall() || inst.IsReturn() || inst.IsBranch() || inst.IsJump()) &&
          i + 1 < blk->instructions.size()) {
        const auto& delay_inst = blk->instructions[i + 1];
        bool annuls_delay = inst.IsBranchLikely();
        RegisterUseDef inst_ud = GetInstructionUseDef(inst);
        for (Register u : inst_ud.gpr_uses) {
          if (u == reg) {
            return false;
          }
        }
        RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
        for (Register u : delay_ud.gpr_uses) {
          if (u == reg) {
            return false;
          }
        }
        if (!annuls_delay) {
          for (Register d : delay_ud.gpr_defs) {
            if (d == reg) {
              killed = true;
              break;
            }
          }
        }
        if (killed) {
          break;
        }
        if (inst.IsCall()) {
          if (reg >= Register::kA0 && reg <= Register::kA3) {
            return false;
          }
          if (reg == Register::kV0 || reg == Register::kV1 || reg == Register::kAt ||
              (reg >= Register::kT0 && reg <= Register::kT7) || reg == Register::kT8 ||
              reg == Register::kT9) {
            killed = true;
            break;
          }
        } else if (inst.IsReturn()) {
          if (reg == Register::kV0 && returns_v0) {
            return false;
          }
          killed = true;
          break;
        }
        ++i;
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register u : ud.gpr_uses) {
        if (u == reg) {
          return false;
        }
      }
      for (Register d : ud.gpr_defs) {
        if (d == reg) {
          killed = true;
          break;
        }
      }
      if (killed) {
        break;
      }
    }
    if (!killed) {
      if (blk->successors.empty()) {
        if (reg == Register::kV0 && returns_v0) {
          return false;
        }
        continue;
      }
      for (uint32_t succ : blk->successors) {
        worklist.push_back(succ);
      }
    }
  }
  return true;
}

bool AllRegistersDeadAtBlockEntry(const ControlFlowGraph& cfg, uint32_t start_block_id,
                                  absl::Span<const Register> regs, bool returns_v0) {
  if (regs.empty()) {
    return false;
  }
  for (Register r : regs) {
    if (!IsRegisterDeadAtBlockEntry(cfg, start_block_id, r, returns_v0)) {
      return false;
    }
  }
  return true;
}

bool DelaySlotBlocksFoldedComparison(const BasicBlock& block) {
  if (block.instructions.size() < 3) {
    return false;
  }
  if (BranchConditionBuilder::FindFoldedComparison(block.instructions).has_value()) {
    return false;
  }
  std::vector<Instruction> with_nop = block.instructions;
  with_nop.back() = MakeNopInstruction(with_nop.back().vram);
  auto folded = BranchConditionBuilder::FindFoldedComparison(with_nop);
  return folded.has_value() && folded->can_suppress_statement;
}

bool DelaySlotEnablesSuccessorFoldedComparison(const BasicBlock& succ_block,
                                               const Instruction& delay_inst) {
  if (succ_block.instructions.size() < 2) {
    return false;
  }
  if (BranchConditionBuilder::FindFoldedComparison(succ_block.instructions).has_value()) {
    return false;
  }
  std::vector<Instruction> with_sunk;
  with_sunk.reserve(succ_block.instructions.size() + 1);
  with_sunk.push_back(delay_inst);
  with_sunk.insert(with_sunk.end(), succ_block.instructions.begin(), succ_block.instructions.end());
  auto folded = BranchConditionBuilder::FindFoldedComparison(with_sunk);
  return folded.has_value() && folded->instruction_index == 0;
}

bool DelaySlotConflictsWithTerminator(const Instruction& term, const Instruction& delay_inst) {
  RegisterUseDef term_ud = GetInstructionUseDef(term);
  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  for (Register d : delay_ud.gpr_defs) {
    for (Register u : term_ud.gpr_uses) {
      if (d == u) {
        return true;
      }
    }
  }
  return (term.opcode == Opcode::kBc1t || term.opcode == Opcode::kBc1f ||
          term.opcode == Opcode::kBc1tl || term.opcode == Opcode::kBc1fl) &&
         (IsFpuCompareOpcode(delay_inst.opcode) || delay_inst.opcode == Opcode::kCtc1);
}

bool DelaySlotHasPreBranchDependenceConflict(const BasicBlock& block, const Instruction& term,
                                             const Instruction& delay_inst) {
  return DelaySlotConflictsWithTerminator(term, delay_inst) ||
         DelaySlotBlocksFoldedComparison(block);
}

bool DelaySlotFeedsSuccessorBranch(const BasicBlock& succ_block, const Instruction& delay_inst) {
  if (DelaySlotEnablesSuccessorFoldedComparison(succ_block, delay_inst)) {
    return true;
  }
  const Instruction* succ_term = succ_block.Terminator();
  if (succ_term == nullptr || !succ_term->IsBranch() || succ_term->IsUnconditionalBranch()) {
    return false;
  }
  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  if (delay_ud.gpr_defs.size() != 1) {
    return false;
  }
  Register def_reg = delay_ud.gpr_defs[0];
  for (const auto& inst : succ_block.instructions) {
    if (&inst == succ_term) {
      break;
    }
    RegisterUseDef ud = GetInstructionUseDef(inst);
    for (Register d : ud.gpr_defs) {
      if (d == def_reg) {
        return false;
      }
    }
  }
  const bool succ_branch_reads_def = (succ_term->rs == def_reg || succ_term->rt == def_reg);
  if (!succ_branch_reads_def) {
    return false;
  }
  const bool makes_const_zero_branch =
      delay_ud.gpr_uses.empty() && succ_block.instructions.size() <= 2 &&
      (succ_term->rs == Register::kZero || succ_term->rt == Register::kZero);
  return !makes_const_zero_branch;
}

bool DelaySlotCompletesSplitInstructionPair(const BasicBlock& block,
                                            const Instruction& delay_inst) {
  if (block.instructions.size() < 3) {
    return false;
  }
  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  for (int i = static_cast<int>(block.instructions.size()) - 3; i >= 0; --i) {
    const Instruction& prev_inst = block.instructions[i];
    if (IsPartnerUnalignedPair(prev_inst, delay_inst)) {
      return true;
    }
    if ((delay_inst.opcode == Opcode::kMflo || delay_inst.opcode == Opcode::kMfhi) &&
        (prev_inst.opcode == Opcode::kMult || prev_inst.opcode == Opcode::kMultu ||
         prev_inst.opcode == Opcode::kDmult || prev_inst.opcode == Opcode::kDmultu ||
         prev_inst.opcode == Opcode::kDiv || prev_inst.opcode == Opcode::kDivu ||
         prev_inst.opcode == Opcode::kDdiv || prev_inst.opcode == Opcode::kDdivu)) {
      return true;
    }
    if ((delay_inst.opcode == Opcode::kMflo || delay_inst.opcode == Opcode::kMfhi) &&
        IsMultiplyDivideOrHiLoOpcode(prev_inst.opcode)) {
      break;
    }
    RegisterUseDef prev_ud = GetInstructionUseDef(prev_inst);
    bool hazard = prev_inst.IsCall() || prev_inst.IsBranch() || prev_inst.IsJump();
    for (Register d : prev_ud.gpr_defs) {
      for (Register u : delay_ud.gpr_uses) {
        if (d == u) hazard = true;
      }
      for (Register dd : delay_ud.gpr_defs) {
        if (d == dd) hazard = true;
      }
    }
    for (Register u : prev_ud.gpr_uses) {
      for (Register dd : delay_ud.gpr_defs) {
        if (u == dd) hazard = true;
      }
    }
    if (hazard) {
      break;
    }
  }
  return false;
}

absl::flat_hash_set<uint32_t> CollectJumpTableBoundsVrams(const ControlFlowGraph& cfg) {
  absl::flat_hash_set<uint32_t> vrams;
  for (const auto& jt : cfg.JumpTables()) {
    if (jt.bounds_branch_vram != 0) {
      vrams.insert(jt.bounds_branch_vram);
    }
  }
  return vrams;
}

}  // namespace

bool SimpleDelaySlotUnschedulerPass::Run(ControlFlowGraph* cfg, CfgAnalysisManager* /*analyses*/) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return false;
  }
  bool changed = false;
  for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
    BasicBlock& block = cfg->MutableBlocks()[b];
    if (block.instructions.size() < 3) {
      continue;
    }
    const Instruction* term_ptr = block.Terminator();
    if (term_ptr == nullptr || term_ptr != &block.instructions[block.instructions.size() - 2]) {
      continue;
    }
    const Instruction term = *term_ptr;
    const Instruction delay_inst = block.instructions.back();
    if (delay_inst.IsNop()) {
      continue;
    }
    if ((term.opcode == Opcode::kJ || (term.IsBranch() && !term.IsBranchLikely())) &&
        !DelaySlotConflictsWithTerminator(term, delay_inst) &&
        DelaySlotCompletesSplitInstructionPair(block, delay_inst)) {
      block.instructions[block.instructions.size() - 2] = delay_inst;
      block.instructions[block.instructions.size() - 1] = term;
      block.instructions.push_back(MakeNopInstruction(delay_inst.vram));
      changed = true;
    }
  }
  return changed;
}

bool TargetHeaderDelaySlotReconcilerPass::Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return false;
  }
  CfgAnalysisManager local_analyses(cfg);
  if (analyses == nullptr) {
    analyses = &local_analyses;
  } else {
    analyses->SetCfg(cfg);
  }
  const auto jump_table_bounds_vrams = CollectJumpTableBoundsVrams(*cfg);

  bool changed = false;
  for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
    BasicBlock& block = cfg->MutableBlocks()[b];
    if (block.instructions.size() < 2) {
      continue;
    }
    const Instruction* term_ptr = block.Terminator();
    if (term_ptr == nullptr || term_ptr != &block.instructions[block.instructions.size() - 2]) {
      continue;
    }
    const Instruction term = *term_ptr;
    const Instruction delay_inst = block.instructions.back();
    if (delay_inst.IsNop() || jump_table_bounds_vrams.contains(term.vram)) {
      continue;
    }

    // Speculative loop-counter compensation (`reorg.c:fill_slots_from_thread` lines 3527-3585):
    // A non-likely conditional branch `B` targeting `B_body` has delay slot `D` (`r = r +/- C`)
    // matching the 1-instruction fallthrough predecessor `B_head` of `B_body`, and the first
    // instruction of `B`'s fallthrough block `B_ft` is the exact inverse adjustment (`r = r -/+
    // C`).
    if (term.IsBranch() && !term.IsBranchLikely() && !term.IsUnconditionalBranch() &&
        block.successors.size() == 2) {
      uint32_t succ_taken = 0;
      uint32_t succ_fallthrough = 0;
      for (const auto& edge : block.outgoing_edges) {
        if (edge.type == EdgeType::kBranch) {
          succ_taken = edge.to_block_id;
        } else if (edge.type == EdgeType::kFallthrough) {
          succ_fallthrough = edge.to_block_id;
        }
      }
      if (succ_taken == 0 && succ_fallthrough == 0) {
        succ_taken = block.successors[0];
        succ_fallthrough = block.successors[1];
      }
      const BasicBlock* taken_blk = cfg->GetBlock(succ_taken);
      BasicBlock* ft_blk = cfg->GetMutableBlock(succ_fallthrough);
      if (taken_blk != nullptr && ft_blk != nullptr && succ_taken != succ_fallthrough &&
          taken_blk->start_vram <= block.start_vram && ft_blk->predecessors.size() == 1 &&
          ft_blk->predecessors[0] == block.id && !ft_blk->instructions.empty() &&
          IsInverseAdjustmentInstruction(delay_inst, ft_blk->instructions.front())) {
        std::optional<uint32_t> head_block_id;
        for (uint32_t pred_id : taken_blk->predecessors) {
          if (pred_id == block.id) continue;
          const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
          if (pred_blk != nullptr && !pred_blk->instructions.empty() &&
              pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
              pred_blk->end_vram + 4 == taken_blk->start_vram &&
              SameDelayInstructionEffect(delay_inst, pred_blk->instructions.back())) {
            head_block_id = pred_id;
            break;
          }
        }
        if (head_block_id.has_value()) {
          // Convert the speculative non-likely branch + fallthrough inverse compensation into an
          // annulled branch-likely (`ToLikelyBranchOpcode`) and nop out the compensating
          // instruction on fallthrough. This preserves the loop header topology for `do-while`
          // loops (allowing `AstConverter::PreScanPeeledLoopRotations` to peel the header
          // rotation cleanly) while allowing `RelaxDelaySlotUnschedulerPass` to retarget
          // non-latch branches!
          block.instructions[block.instructions.size() - 2].opcode =
              ToLikelyBranchOpcode(term.opcode);
          ft_blk->instructions.front() = MakeNopInstruction(ft_blk->instructions.front().vram);
          changed = true;
          continue;
        }
      }
    }
  }
  if (changed) {
    analyses->InvalidateAll();
  }
  return changed;
}

bool RelaxDelaySlotUnschedulerPass::Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return false;
  }
  CfgAnalysisManager local_analyses(cfg);
  if (analyses == nullptr) {
    analyses = &local_analyses;
  } else {
    analyses->SetCfg(cfg);
  }
  const LoopInfo& loop_info = analyses->GetLoopInfo();
  const auto jump_table_bounds_vrams = CollectJumpTableBoundsVrams(*cfg);

  bool changed = false;
  for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
    BasicBlock& block = cfg->MutableBlocks()[b];
    if (block.instructions.size() < 2) {
      continue;
    }
    const Instruction* term_ptr = block.Terminator();
    if (term_ptr == nullptr || term_ptr != &block.instructions[block.instructions.size() - 2]) {
      continue;
    }
    const Instruction term = *term_ptr;
    const Instruction delay_inst = block.instructions.back();
    if (delay_inst.IsNop() || !term.IsBranchLikely() || block.successors.size() != 2 ||
        jump_table_bounds_vrams.contains(term.vram)) {
      continue;
    }
    const Loop* enclosing_loop = loop_info.GetLoopFor(block.id);
    if (enclosing_loop != nullptr && enclosing_loop->IsLatch(block.id)) {
      continue;
    }

    uint32_t succ_taken = 0;
    uint32_t succ_fallthrough = 0;
    for (const auto& edge : block.outgoing_edges) {
      if (edge.type == EdgeType::kBranch) {
        succ_taken = edge.to_block_id;
      } else if (edge.type == EdgeType::kFallthrough) {
        succ_fallthrough = edge.to_block_id;
      }
    }
    if (succ_taken == 0 && succ_fallthrough == 0) {
      succ_taken = block.successors[0];
      succ_fallthrough = block.successors[1];
    }
    BasicBlock* taken_block = cfg->GetMutableBlock(succ_taken);
    if (taken_block == nullptr || succ_taken == succ_fallthrough) {
      continue;
    }

    const Loop* target_loop = loop_info.GetLoopFor(succ_taken);
    std::optional<uint32_t> pred_block_id;
    for (uint32_t pred_id : taken_block->predecessors) {
      if (pred_id == block.id) {
        continue;
      }
      const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
      if (pred_blk != nullptr && pred_blk->instructions.size() == 1 &&
          pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
          SameDelayInstructionEffect(delay_inst, pred_blk->instructions.front()) &&
          ((enclosing_loop != nullptr && enclosing_loop->Contains(block.id) &&
            !enclosing_loop->Contains(pred_id)) ||
           (target_loop != nullptr && target_loop->header == succ_taken &&
            !target_loop->Contains(block.id) && !target_loop->Contains(pred_id)))) {
        pred_block_id = pred_id;
        break;
      }
    }
    if (pred_block_id.has_value()) {
      RetargetBranchEdge(cfg, block.id, succ_taken, *pred_block_id);
      block.instructions[block.instructions.size() - 2].opcode =
          ToNonLikelyBranchOpcode(term.opcode);
      block.instructions.back() = MakeNopInstruction(delay_inst.vram);
      changed = true;
    }
  }
  if (changed) {
    analyses->InvalidateAll();
  }
  return changed;
}

bool EagerThreadDelaySlotUnschedulerPass::Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return false;
  }
  CfgAnalysisManager local_analyses(cfg);
  if (analyses == nullptr) {
    analyses = &local_analyses;
  } else {
    analyses->SetCfg(cfg);
  }
  const LoopInfo& loop_info = analyses->GetLoopInfo();
  const bool returns_f0 = AbiAnalyzer::DetermineReturnsF0(*cfg);
  const bool returns_v0 = !returns_f0 && AbiAnalyzer::DetermineReturnsV0(*cfg);
  const auto jump_table_bounds_vrams = CollectJumpTableBoundsVrams(*cfg);

  bool changed = false;
  for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
    BasicBlock& block = cfg->MutableBlocks()[b];
    if (block.instructions.size() < 2) {
      continue;
    }
    const Instruction* term_ptr = block.Terminator();
    if (term_ptr == nullptr || term_ptr != &block.instructions[block.instructions.size() - 2]) {
      continue;
    }
    const Instruction term = *term_ptr;
    const Instruction delay_inst = block.instructions.back();
    if (delay_inst.IsNop()) {
      continue;
    }

    auto compute_unscheduled_clobber_reg = [&](const RegisterUseDef& delay_ud) {
      std::optional<Register> clobber_reg;
      if (delay_ud.gpr_defs.size() == 1) {
        Register def_r = delay_ud.gpr_defs[0];
        bool uses_def_r = false;
        for (Register u : delay_ud.gpr_uses) {
          if (u == def_r) {
            uses_def_r = true;
            break;
          }
        }
        if (!uses_def_r) {
          clobber_reg = def_r;
        }
      }
      return clobber_reg;
    };

    // 1. Branch-likely (`bnezl`/`beqzl`/`bc1tl`/`bc1fl`) target-stolen delay-slot
    // normalization onto single-predecessor taken blocks.
    if (term.IsBranchLikely() && block.successors.size() == 2 &&
        !jump_table_bounds_vrams.contains(term.vram)) {
      const Loop* enclosing_loop = loop_info.GetLoopFor(block.id);
      if (enclosing_loop == nullptr || !enclosing_loop->IsLatch(block.id)) {
        uint32_t succ_taken = 0;
        uint32_t succ_fallthrough = 0;
        for (const auto& edge : block.outgoing_edges) {
          if (edge.type == EdgeType::kBranch) {
            succ_taken = edge.to_block_id;
          } else if (edge.type == EdgeType::kFallthrough) {
            succ_fallthrough = edge.to_block_id;
          }
        }
        if (succ_taken == 0 && succ_fallthrough == 0) {
          succ_taken = block.successors[0];
          succ_fallthrough = block.successors[1];
        }
        BasicBlock* taken_block = cfg->GetMutableBlock(succ_taken);
        const BasicBlock* ft_block = cfg->GetBlock(succ_fallthrough);
        bool ft_identical = ft_block != nullptr && !ft_block->instructions.empty() &&
                            SameDelayInstructionEffect(delay_inst, ft_block->instructions.front());
        if (taken_block != nullptr && !ft_identical && succ_taken != succ_fallthrough &&
            succ_taken != block.id && succ_taken != cfg->EntryBlockId() &&
            taken_block->predecessors.size() == 1 && taken_block->predecessors[0] == block.id &&
            loop_info.GetLoopFor(succ_taken) == enclosing_loop &&
            (enclosing_loop == nullptr || enclosing_loop->header != block.id) &&
            IsSinkableDelaySlotInstruction(block, delay_inst) &&
            !taken_block->instructions.empty() && !taken_block->instructions.front().IsReturn() &&
            (DelaySlotHasPreBranchDependenceConflict(block, term, delay_inst) ||
             DelaySlotFeedsSuccessorBranch(*taken_block, delay_inst)) &&
            !(DelaySlotConflictsWithTerminator(term, delay_inst) &&
              taken_block->instructions.front().IsCall())) {
          RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
          std::optional<Register> clobber_reg = compute_unscheduled_clobber_reg(delay_ud);
          taken_block->instructions.insert(taken_block->instructions.begin(), delay_inst);
          block.instructions[block.instructions.size() - 2].opcode =
              ToNonLikelyBranchOpcode(term.opcode);
          block.instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
          changed = true;
          continue;
        }
      }
    }

    // 2. Thread Sinking (`fill_slots_from_thread` inverse) for non-likely conditional branches.
    if (!term.IsBranch() || term.IsBranchLikely() || term.IsUnconditionalBranch() ||
        block.successors.size() != 2 || jump_table_bounds_vrams.contains(term.vram)) {
      continue;
    }

    const Loop* enclosing_loop = loop_info.GetLoopFor(block.id);
    if (enclosing_loop != nullptr && enclosing_loop->IsLatch(block.id)) {
      continue;
    }

    uint32_t succ_taken = 0;
    uint32_t succ_fallthrough = 0;
    for (const auto& edge : block.outgoing_edges) {
      if (edge.type == EdgeType::kBranch) {
        succ_taken = edge.to_block_id;
      } else if (edge.type == EdgeType::kFallthrough) {
        succ_fallthrough = edge.to_block_id;
      }
    }
    if (succ_taken == 0 && succ_fallthrough == 0) {
      succ_taken = block.successors[0];
      succ_fallthrough = block.successors[1];
    }
    if (succ_taken == succ_fallthrough || succ_taken == block.id || succ_fallthrough == block.id) {
      continue;
    }

    if (!IsSinkableDelaySlotInstruction(block, delay_inst)) {
      continue;
    }

    RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
    bool dead_on_taken =
        AllRegistersDeadAtBlockEntry(*cfg, succ_taken, delay_ud.gpr_defs, returns_v0);
    bool dead_on_fallthrough =
        AllRegistersDeadAtBlockEntry(*cfg, succ_fallthrough, delay_ud.gpr_defs, returns_v0);
    if (dead_on_taken == dead_on_fallthrough) {
      continue;
    }

    BasicBlock* ft_block = cfg->GetMutableBlock(succ_fallthrough);
    BasicBlock* taken_block = cfg->GetMutableBlock(succ_taken);
    const uint32_t target_succ = dead_on_taken ? succ_fallthrough : succ_taken;
    BasicBlock* target_blk = dead_on_taken ? ft_block : taken_block;
    if (target_blk == nullptr || target_succ == cfg->EntryBlockId() ||
        target_blk->predecessors.size() != 1 || target_blk->predecessors[0] != block.id ||
        loop_info.GetLoopFor(target_succ) != enclosing_loop) {
      continue;
    }

    // Do not sink out of a loop header unless it directly feeds a fallthrough branch inside a
    // do-while loop.
    if (enclosing_loop != nullptr && enclosing_loop->header == block.id &&
        (loop_info.GetLoopFor(succ_taken) != enclosing_loop ||
         loop_info.GetLoopFor(succ_fallthrough) != enclosing_loop) &&
        !(dead_on_taken && DelaySlotFeedsSuccessorBranch(*target_blk, delay_inst))) {
      continue;
    }

    if (!dead_on_taken &&
        (target_blk->instructions.empty() || target_blk->instructions.front().IsReturn() ||
         (DelaySlotConflictsWithTerminator(term, delay_inst) &&
          target_blk->instructions.front().IsCall()))) {
      continue;
    }

    if (DelaySlotHasPreBranchDependenceConflict(block, term, delay_inst) ||
        DelaySlotFeedsSuccessorBranch(*target_blk, delay_inst)) {
      std::optional<Register> clobber_reg = compute_unscheduled_clobber_reg(delay_ud);
      target_blk->instructions.insert(target_blk->instructions.begin(), delay_inst);
      block.instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
      changed = true;
    }
  }
  if (changed) {
    analyses->InvalidateAll();
  }
  return changed;
}

bool CrossJumpTailDuplicatorPass::Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return false;
  }
  CfgAnalysisManager local_analyses(cfg);
  if (analyses == nullptr) {
    analyses = &local_analyses;
  } else {
    analyses->SetCfg(cfg);
  }
  const auto jump_table_bounds_vrams = CollectJumpTableBoundsVrams(*cfg);
  absl::flat_hash_set<uint32_t> jump_table_target_vrams;
  for (const auto& jt : cfg->JumpTables()) {
    if (jt.default_target_vram != 0) {
      jump_table_target_vrams.insert(jt.default_target_vram);
    }
    for (const auto& entry : jt.entries) {
      jump_table_target_vrams.insert(entry.target_vram);
    }
  }

  bool any_changed = false;
  for (int iter = 0; iter < 4; ++iter) {
    const LoopInfo& loop_info = analyses->GetLoopInfo();
    const DominatorTree& dom_tree = analyses->GetDominatorTree();
    const DominatorTree& post_dom_tree = analyses->GetPostDominatorTree();
    bool iter_changed = false;

    for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
      BasicBlock& tail_blk = cfg->MutableBlocks()[b];
      const Loop* tail_loop = loop_info.GetLoopFor(tail_blk.id);
      if (tail_blk.id == cfg->EntryBlockId() || tail_blk.predecessors.size() < 2 ||
          tail_blk.successors.size() > 1 || tail_blk.HasSwitch() ||
          jump_table_target_vrams.contains(tail_blk.start_vram) ||
          (tail_loop != nullptr && tail_loop->header == tail_blk.id)) {
        continue;
      }

      const Instruction* tail_term = tail_blk.Terminator();
      const bool is_jump_tail =
          tail_blk.successors.size() == 1 && tail_term != nullptr &&
          tail_term->opcode == Opcode::kJ &&
          tail_term == &tail_blk.instructions[tail_blk.instructions.size() - 2] &&
          tail_blk.instructions.back().IsNop() &&
          !jump_table_bounds_vrams.contains(tail_term->vram);
      const bool is_fallthrough_tail = tail_blk.successors.size() == 1 && tail_term == nullptr &&
                                       tail_blk.outgoing_edges.size() == 1 &&
                                       tail_blk.outgoing_edges[0].type == EdgeType::kFallthrough;
      const bool is_return_tail =
          tail_blk.successors.empty() && tail_term != nullptr && tail_term->IsReturn() &&
          tail_blk.instructions.size() >= 2 &&
          tail_term == &tail_blk.instructions[tail_blk.instructions.size() - 2] &&
          tail_blk.instructions.back().IsNop();
      if (!is_jump_tail && !is_fallthrough_tail && !is_return_tail) {
        continue;
      }

      const size_t body_count =
          is_fallthrough_tail ? tail_blk.instructions.size() : (tail_blk.instructions.size() - 2);
      if (body_count == 0 || body_count > 6) {
        continue;
      }
      bool valid_tail_body = true;
      for (size_t i = 0; i < body_count; ++i) {
        const Instruction& inst = tail_blk.instructions[i];
        if (inst.IsBranch() || inst.IsJump()) {
          valid_tail_body = false;
          break;
        }
        if (is_return_tail) {
          RegisterUseDef ud = GetInstructionUseDef(inst);
          for (Register def_reg : ud.gpr_defs) {
            if (def_reg == Register::kSp || def_reg == Register::kRa) {
              valid_tail_body = false;
              break;
            }
          }
          if (!valid_tail_body) break;
        }
      }
      if (!valid_tail_body) {
        continue;
      }

      if (is_fallthrough_tail || is_return_tail) {
        std::optional<uint32_t> idom = dom_tree.ImmediateDominator(tail_blk.id);
        if (!idom.has_value() || post_dom_tree.Dominates(tail_blk.id, *idom)) {
          continue;
        }
      }

      uint32_t merge_block_id = 0;
      uint32_t new_target_field = 0;
      if (!is_return_tail) {
        merge_block_id = tail_blk.successors[0];
        if (merge_block_id == tail_blk.id || loop_info.GetLoopFor(merge_block_id) != tail_loop) {
          continue;
        }
        const BasicBlock* merge_blk = cfg->GetBlock(merge_block_id);
        if (merge_blk == nullptr) {
          continue;
        }
        new_target_field =
            is_jump_tail ? tail_term->target : ((merge_blk->start_vram >> 2) & 0x03FFFFFF);
      }

      bool has_fallthrough_pred = false;
      std::vector<uint32_t> jump_preds;
      for (uint32_t pred_id : tail_blk.predecessors) {
        const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
        if (pred_blk == nullptr) continue;
        for (const auto& edge : pred_blk->outgoing_edges) {
          if (edge.to_block_id == tail_blk.id && edge.type == EdgeType::kFallthrough) {
            has_fallthrough_pred = true;
            break;
          }
        }
        if (pred_id == tail_blk.id || loop_info.GetLoopFor(pred_id) != tail_loop ||
            pred_blk->successors.size() != 1 || pred_blk->successors[0] != tail_blk.id ||
            pred_blk->instructions.size() < 2) {
          continue;
        }
        const Instruction* pred_term = pred_blk->Terminator();
        if (pred_term != nullptr && pred_term->opcode == Opcode::kJ &&
            pred_term == &pred_blk->instructions[pred_blk->instructions.size() - 2] &&
            pred_blk->instructions.back().IsNop()) {
          jump_preds.push_back(pred_id);
        }
      }

      if (jump_preds.empty() ||
          ((is_fallthrough_tail || is_return_tail) && !has_fallthrough_pred)) {
        continue;
      }

      if (is_return_tail) {
        const std::vector<Instruction> tail_all_insts = tail_blk.instructions;
        for (uint32_t pred_id : jump_preds) {
          BasicBlock* pred_blk = cfg->GetMutableBlock(pred_id);
          if (pred_blk == nullptr || pred_blk->instructions.size() < 2) {
            continue;
          }
          pred_blk->instructions.erase(pred_blk->instructions.end() - 2,
                                       pred_blk->instructions.end());
          pred_blk->instructions.insert(pred_blk->instructions.end(), tail_all_insts.begin(),
                                        tail_all_insts.end());
          RemoveBranchEdge(cfg, pred_id, tail_blk.id);
          iter_changed = true;
        }
      } else {
        const std::vector<Instruction> tail_body_insts(
            tail_blk.instructions.begin(),
            tail_blk.instructions.begin() + static_cast<ptrdiff_t>(body_count));
        for (uint32_t pred_id : jump_preds) {
          BasicBlock* pred_blk = cfg->GetMutableBlock(pred_id);
          if (pred_blk == nullptr || pred_blk->instructions.size() < 2) {
            continue;
          }
          auto insert_pos = pred_blk->instructions.end() - 2;
          pred_blk->instructions.insert(insert_pos, tail_body_insts.begin(), tail_body_insts.end());
          pred_blk->instructions[pred_blk->instructions.size() - 2].target = new_target_field;
          RetargetBranchEdge(cfg, pred_id, tail_blk.id, merge_block_id);
          iter_changed = true;
        }
      }
    }

    if (!iter_changed) {
      break;
    }
    any_changed = true;
    analyses->InvalidateAll();
  }
  return any_changed;
}

CfgPassPipeline CfgDeReorg::BuildDefaultPipeline() {
  CfgPassPipeline pipeline;
  pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<TargetHeaderDelaySlotReconcilerPass>());
  pipeline.AddPass(std::make_unique<RelaxDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<CrossJumpTailDuplicatorPass>());
  return pipeline;
}

void CfgDeReorg::UnscheduleDelaySlots(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return;
  }
  CfgPassPipeline pipeline = BuildDefaultPipeline();
  pipeline.Run(cfg, analyses);
}

}  // namespace rom_nom_nom
