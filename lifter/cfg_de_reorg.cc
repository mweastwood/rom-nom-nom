#include "lifter/cfg_de_reorg.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "lifter/abi_analyzer.h"
#include "lifter/branch_condition_builder.h"
#include "lifter/cfg_analysis_manager.h"
#include "lifter/cfg_pass_pipeline.h"
#include "lifter/control_flow_pattern_matcher.h"
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
  nop.was_unscheduled_delay_slot = true;
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

std::optional<Opcode> InvertNonLikelyBranchOpcode(Opcode op) {
  switch (op) {
    case Opcode::kBeq:
      return Opcode::kBne;
    case Opcode::kBne:
      return Opcode::kBeq;
    case Opcode::kBlez:
      return Opcode::kBgtz;
    case Opcode::kBgtz:
      return Opcode::kBlez;
    case Opcode::kBltz:
      return Opcode::kBgez;
    case Opcode::kBgez:
      return Opcode::kBltz;
    case Opcode::kBc1t:
      return Opcode::kBc1f;
    case Opcode::kBc1f:
      return Opcode::kBc1t;
    default:
      return std::nullopt;
  }
}

bool SameDelayInstructionEffect(const Instruction& a, const Instruction& b) {
  return !a.IsNop() && a.opcode == b.opcode && a.rs == b.rs && a.rt == b.rt && a.rd == b.rd &&
         a.immediate == b.immediate && a.fs == b.fs && a.ft == b.ft && a.fd == b.fd &&
         a.shift_amount == b.shift_amount;
}

bool IsPureEpilogueReturnBlock(const BasicBlock& block) {
  if (!block.successors.empty()) return false;
  const Instruction* term = block.Terminator();
  if (term == nullptr || !term->IsReturn()) return false;
  for (const auto& inst : block.instructions) {
    if (inst.IsNop() || inst.IsReturn()) continue;
    if (inst.opcode == Opcode::kAddiu && inst.rt == Register::kSp && inst.rs == Register::kSp &&
        inst.immediate >= 0) {
      continue;
    }
    if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kDaddu ||
         inst.opcode == Opcode::kOr) &&
        inst.rd == Register::kSp && inst.rs == Register::kFp && inst.rt == Register::kZero) {
      continue;
    }
    if ((inst.opcode == Opcode::kLw || inst.opcode == Opcode::kLd) &&
        (inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.rt.has_value() &&
        (*inst.rt == Register::kRa || *inst.rt == Register::kFp ||
         (*inst.rt >= Register::kS0 && *inst.rt <= Register::kS7))) {
      continue;
    }
    if ((inst.opcode == Opcode::kLwc1 || inst.opcode == Opcode::kLdc1) &&
        (inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.ft.has_value() &&
        *inst.ft >= FpRegister::kF20 && *inst.ft <= FpRegister::kF31) {
      continue;
    }
    return false;
  }
  return true;
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
  if (delay_inst.opcode == Opcode::kMfc0 || delay_inst.opcode == Opcode::kMtc0 ||
      delay_inst.opcode == Opcode::kCtc1 || delay_inst.opcode == Opcode::kCfc1 ||
      delay_inst.opcode == Opcode::kBreak || delay_inst.opcode == Opcode::kSync ||
      delay_inst.opcode == Opcode::kSyscall || IsMultiplyDivideOrHiLoOpcode(delay_inst.opcode) ||
      IsUnalignedMemoryOpcode(delay_inst.opcode) || IsFpuCompareOpcode(delay_inst.opcode)) {
    return false;
  }

  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  if (delay_ud.gpr_defs.empty() && delay_ud.fpr_defs.empty()) {
    return false;
  }
  for (Register d : delay_ud.gpr_defs) {
    if (d == Register::kZero || d == Register::kSp || d == Register::kFp || d == Register::kRa) {
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

bool DetermineReturnsV1(const ControlFlowGraph& cfg, bool returns_v0) {
  if (!returns_v0) {
    return false;
  }
  bool any_defines_v1 = false;
  for (const auto& blk : cfg.Blocks()) {
    bool blk_defs_v0 = false;
    bool blk_defs_v1 = false;
    for (const auto& inst : blk.instructions) {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register d : ud.gpr_defs) {
        if (d == Register::kV0) blk_defs_v0 = true;
        if (d == Register::kV1) {
          blk_defs_v1 = true;
          any_defines_v1 = true;
        }
      }
    }
    if (blk.HasReturn() && blk_defs_v0 && !blk_defs_v1) {
      return false;
    }
    if (!blk.HasReturn() && blk.successors.size() == 1 && blk_defs_v0 && !blk_defs_v1) {
      const BasicBlock* succ = cfg.GetBlock(blk.successors[0]);
      if (succ != nullptr && succ->HasReturn()) {
        bool succ_defs_v1 = false;
        for (const auto& inst : succ->instructions) {
          RegisterUseDef ud = GetInstructionUseDef(inst);
          if (std::find(ud.gpr_defs.begin(), ud.gpr_defs.end(), Register::kV1) !=
              ud.gpr_defs.end()) {
            succ_defs_v1 = true;
            break;
          }
        }
        if (!succ_defs_v1) {
          return false;
        }
      }
    }
  }
  return any_defines_v1;
}

bool AllDefinedRegistersDeadAtBlockEntry(const ControlFlowGraph& cfg, uint32_t start_block_id,
                                         const RegisterUseDef& ud, bool returns_v0,
                                         bool returns_f0) {
  if (ud.gpr_defs.empty() && ud.fpr_defs.empty()) {
    return false;
  }
  const bool returns_v1 = DetermineReturnsV1(cfg, returns_v0);
  for (Register r : ud.gpr_defs) {
    if (!AbiAnalyzer::IsRegisterDeadAtBlockEntry(cfg, start_block_id, r, returns_v0,
                                                 /*returns_v1=*/returns_v1)) {
      return false;
    }
  }
  for (FpRegister fr : ud.fpr_defs) {
    if (!AbiAnalyzer::IsFpRegisterDeadAtBlockEntry(cfg, start_block_id, fr, returns_f0)) {
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
  if (!delay_ud.fpr_defs.empty()) {
    if (succ_term->opcode != Opcode::kBc1t && succ_term->opcode != Opcode::kBc1f &&
        succ_term->opcode != Opcode::kBc1tl && succ_term->opcode != Opcode::kBc1fl) {
      return false;
    }
    FpRegister def_freg = delay_ud.fpr_defs[0];
    for (const auto& inst : succ_block.instructions) {
      if (&inst == succ_term) {
        break;
      }
      RegisterUseDef ud = GetInstructionUseDef(inst);
      if (IsFpuCompareOpcode(inst.opcode)) {
        for (FpRegister u : ud.fpr_uses) {
          if (u == def_freg) {
            return true;
          }
        }
      }
      for (FpRegister d : ud.fpr_defs) {
        if (d == def_freg) {
          return false;
        }
      }
    }
    return false;
  }
  if (delay_ud.gpr_defs.size() != 1) {
    return false;
  }
  Register tracked_reg = delay_ud.gpr_defs[0];
  for (const auto& inst : succ_block.instructions) {
    if (&inst == succ_term) {
      break;
    }
    if (inst.IsCall() || inst.IsStore()) {
      return false;
    }
    RegisterUseDef ud = GetInstructionUseDef(inst);
    bool reads_tracked =
        std::find(ud.gpr_uses.begin(), ud.gpr_uses.end(), tracked_reg) != ud.gpr_uses.end();
    if (reads_tracked && ud.gpr_defs.size() == 1 && ud.fpr_defs.empty()) {
      tracked_reg = ud.gpr_defs[0];
      continue;
    }
    for (Register d : ud.gpr_defs) {
      if (d == tracked_reg) {
        return false;
      }
    }
  }
  const bool succ_branch_reads_def = (succ_term->rs == tracked_reg || succ_term->rt == tracked_reg);
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

bool IsRedundantTargetHeaderInstruction(const BasicBlock& block, const Instruction& term,
                                        const Instruction& trial) {
  if (block.instructions.size() < 3 || trial.IsLoad() ||
      !IsSinkableDelaySlotInstruction(block, trial) ||
      DelaySlotHasPreBranchDependenceConflict(block, term, trial)) {
    return false;
  }
  RegisterUseDef trial_ud = GetInstructionUseDef(trial);
  for (Register d : trial_ud.gpr_defs) {
    if (std::find(trial_ud.gpr_uses.begin(), trial_ud.gpr_uses.end(), d) !=
        trial_ud.gpr_uses.end()) {
      return false;
    }
  }
  for (FpRegister fd : trial_ud.fpr_defs) {
    if (std::find(trial_ud.fpr_uses.begin(), trial_ud.fpr_uses.end(), fd) !=
        trial_ud.fpr_uses.end()) {
      return false;
    }
  }
  for (int i = static_cast<int>(block.instructions.size()) - 3; i >= 0; --i) {
    const Instruction& inst = block.instructions[i];
    if (SameDelayInstructionEffect(inst, trial)) {
      return true;
    }
    if (inst.IsCall() || inst.IsBranch() || inst.IsJump()) {
      return false;
    }
    RegisterUseDef ud = GetInstructionUseDef(inst);
    for (Register d : ud.gpr_defs) {
      if (std::find(trial_ud.gpr_defs.begin(), trial_ud.gpr_defs.end(), d) !=
              trial_ud.gpr_defs.end() ||
          std::find(trial_ud.gpr_uses.begin(), trial_ud.gpr_uses.end(), d) !=
              trial_ud.gpr_uses.end()) {
        return false;
      }
    }
    for (Register u : ud.gpr_uses) {
      if (std::find(trial_ud.gpr_defs.begin(), trial_ud.gpr_defs.end(), u) !=
          trial_ud.gpr_defs.end()) {
        return false;
      }
    }
    for (FpRegister fd : ud.fpr_defs) {
      if (std::find(trial_ud.fpr_defs.begin(), trial_ud.fpr_defs.end(), fd) !=
              trial_ud.fpr_defs.end() ||
          std::find(trial_ud.fpr_uses.begin(), trial_ud.fpr_uses.end(), fd) !=
              trial_ud.fpr_uses.end()) {
        return false;
      }
    }
    for (FpRegister fu : ud.fpr_uses) {
      if (std::find(trial_ud.fpr_defs.begin(), trial_ud.fpr_defs.end(), fu) !=
          trial_ud.fpr_defs.end()) {
        return false;
      }
    }
  }
  return false;
}

bool IsPrologueCalleeSaveStore(const Instruction& inst) {
  if (!inst.rs.has_value() || *inst.rs != Register::kSp || inst.immediate < 0) {
    return false;
  }
  if ((inst.opcode == Opcode::kSw || inst.opcode == Opcode::kSd) && inst.rt.has_value()) {
    Register rt = *inst.rt;
    return rt == Register::kRa || rt == Register::kFp ||
           (rt >= Register::kS0 && rt <= Register::kS7);
  }
  if ((inst.opcode == Opcode::kSwc1 || inst.opcode == Opcode::kSdc1) && inst.ft.has_value()) {
    return *inst.ft >= FpRegister::kF20;
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

bool IsValidDelaySlotCandidate(const Instruction& delay_inst) {
  return !delay_inst.IsNop() && !delay_inst.IsBranch() && !delay_inst.IsJump() &&
         !delay_inst.IsCall() && !delay_inst.IsReturn() && delay_inst.opcode != Opcode::kBreak &&
         delay_inst.opcode != Opcode::kSyscall && delay_inst.opcode != Opcode::kSync;
}

bool InstructionsHaveHazard(const Instruction& first, const Instruction& second) {
  if (first.IsNop() || second.IsNop()) {
    return false;
  }
  if (first.IsBranch() || first.IsJump() || first.IsCall() || first.IsReturn() ||
      second.IsBranch() || second.IsJump() || second.IsCall() || second.IsReturn() ||
      first.opcode == Opcode::kBreak || first.opcode == Opcode::kSyscall ||
      first.opcode == Opcode::kMtc0 || first.opcode == Opcode::kMfc0 ||
      second.opcode == Opcode::kBreak || second.opcode == Opcode::kSyscall ||
      second.opcode == Opcode::kMtc0 || second.opcode == Opcode::kMfc0) {
    return true;
  }
  if ((first.IsStore() && (second.IsLoad() || second.IsStore())) ||
      (second.IsStore() && (first.IsLoad() || first.IsStore()))) {
    return true;
  }
  if (IsMultiplyDivideOrHiLoOpcode(first.opcode) && IsMultiplyDivideOrHiLoOpcode(second.opcode)) {
    return true;
  }
  if ((IsFpuCompareOpcode(first.opcode) || first.opcode == Opcode::kCtc1 ||
       first.opcode == Opcode::kCfc1) &&
      (IsFpuCompareOpcode(second.opcode) || second.opcode == Opcode::kCtc1 ||
       second.opcode == Opcode::kCfc1)) {
    return true;
  }
  RegisterUseDef first_ud = GetInstructionUseDef(first);
  RegisterUseDef second_ud = GetInstructionUseDef(second);
  for (Register d : first_ud.gpr_defs) {
    if (std::find(second_ud.gpr_uses.begin(), second_ud.gpr_uses.end(), d) !=
            second_ud.gpr_uses.end() ||
        std::find(second_ud.gpr_defs.begin(), second_ud.gpr_defs.end(), d) !=
            second_ud.gpr_defs.end()) {
      return true;
    }
  }
  for (Register u : first_ud.gpr_uses) {
    if (std::find(second_ud.gpr_defs.begin(), second_ud.gpr_defs.end(), u) !=
        second_ud.gpr_defs.end()) {
      return true;
    }
  }
  for (FpRegister fd : first_ud.fpr_defs) {
    if (std::find(second_ud.fpr_uses.begin(), second_ud.fpr_uses.end(), fd) !=
            second_ud.fpr_uses.end() ||
        std::find(second_ud.fpr_defs.begin(), second_ud.fpr_defs.end(), fd) !=
            second_ud.fpr_defs.end()) {
      return true;
    }
  }
  for (FpRegister fu : first_ud.fpr_uses) {
    if (std::find(second_ud.fpr_defs.begin(), second_ud.fpr_defs.end(), fu) !=
        second_ud.fpr_defs.end()) {
      return true;
    }
  }
  return false;
}

uint32_t CreateOneInstructionFallthroughBlock(ControlFlowGraph* cfg, const Instruction& inst,
                                              uint32_t fallthrough_to_block_id) {
  const uint32_t new_id = static_cast<uint32_t>(cfg->Blocks().size());
  BasicBlock new_blk;
  new_blk.id = new_id;
  new_blk.start_vram = inst.vram;
  new_blk.end_vram = inst.vram;
  new_blk.instructions.push_back(inst);

  CfgEdge ft_edge;
  ft_edge.from_block_id = new_id;
  ft_edge.to_block_id = fallthrough_to_block_id;
  ft_edge.type = EdgeType::kFallthrough;
  new_blk.outgoing_edges.push_back(ft_edge);
  new_blk.successors.push_back(fallthrough_to_block_id);
  cfg->MutableEdges().push_back(ft_edge);
  cfg->MutableBlocks().push_back(std::move(new_blk));

  if (BasicBlock* target_blk = cfg->GetMutableBlock(fallthrough_to_block_id);
      target_blk != nullptr) {
    if (std::find(target_blk->predecessors.begin(), target_blk->predecessors.end(), new_id) ==
        target_blk->predecessors.end()) {
      target_blk->predecessors.push_back(new_id);
    }
  }
  return new_id;
}

uint32_t CreateJumpBlockWithInstruction(ControlFlowGraph* cfg, const Instruction& inst,
                                        uint32_t jump_to_block_id) {
  const uint32_t new_id = static_cast<uint32_t>(cfg->Blocks().size());
  uint32_t target_vram = inst.vram;
  if (const BasicBlock* target_blk = cfg->GetBlock(jump_to_block_id); target_blk != nullptr) {
    target_vram = target_blk->start_vram;
  }
  const uint32_t target_field = (target_vram >> 2) & 0x03FFFFFFu;
  BasicBlock new_blk;
  new_blk.id = new_id;
  new_blk.start_vram = inst.vram;
  new_blk.end_vram = inst.vram;
  new_blk.instructions.push_back(inst);

  Instruction j_inst;
  j_inst.vram = inst.vram;
  j_inst.opcode = Opcode::kJ;
  j_inst.target = target_field;
  j_inst.raw_word = (2u << 26) | target_field;
  new_blk.instructions.push_back(j_inst);
  new_blk.instructions.push_back(MakeNopInstruction(inst.vram));

  CfgEdge jmp_edge;
  jmp_edge.from_block_id = new_id;
  jmp_edge.to_block_id = jump_to_block_id;
  jmp_edge.type = EdgeType::kJump;
  new_blk.outgoing_edges.push_back(jmp_edge);
  new_blk.successors.push_back(jump_to_block_id);
  cfg->MutableEdges().push_back(jmp_edge);
  cfg->MutableBlocks().push_back(std::move(new_blk));

  if (BasicBlock* target_blk = cfg->GetMutableBlock(jump_to_block_id); target_blk != nullptr) {
    if (std::find(target_blk->predecessors.begin(), target_blk->predecessors.end(), new_id) ==
        target_blk->predecessors.end()) {
      target_blk->predecessors.push_back(new_id);
    }
  }
  return new_id;
}

bool InvertBranchOverJumpBlock(ControlFlowGraph* cfg, uint32_t block_id, Opcode inverted_op,
                               const Instruction& delay_inst, uint32_t succ_taken,
                               uint32_t succ_fallthrough) {
  const uint32_t jump_blk_id = CreateJumpBlockWithInstruction(cfg, delay_inst, succ_taken);
  RetargetBranchEdge(cfg, block_id, succ_taken, succ_fallthrough);
  BasicBlock* mut_block = cfg->GetMutableBlock(block_id);
  BasicBlock* mut_ft = cfg->GetMutableBlock(succ_fallthrough);
  BasicBlock* mut_jmp = cfg->GetMutableBlock(jump_blk_id);
  if (mut_block == nullptr || mut_ft == nullptr || mut_jmp == nullptr ||
      mut_block->instructions.size() < 2) {
    return false;
  }
  mut_block->instructions[mut_block->instructions.size() - 2].opcode = inverted_op;
  mut_block->instructions[mut_block->instructions.size() - 2].target = mut_ft->start_vram;
  mut_block->instructions.back() = MakeNopInstruction(delay_inst.vram);
  for (auto& edge : mut_block->outgoing_edges) {
    if (edge.type == EdgeType::kFallthrough && edge.to_block_id == succ_fallthrough) {
      edge.to_block_id = jump_blk_id;
    }
  }
  for (size_t i = 0; i < mut_block->successors.size(); ++i) {
    if (mut_block->outgoing_edges[i].type == EdgeType::kFallthrough) {
      mut_block->successors[i] = jump_blk_id;
    }
  }
  for (auto& edge : cfg->MutableEdges()) {
    if (edge.from_block_id == block_id && edge.type == EdgeType::kFallthrough &&
        edge.to_block_id == succ_fallthrough) {
      edge.to_block_id = jump_blk_id;
    }
  }
  mut_jmp->predecessors.push_back(block_id);
  return true;
}

bool IsEpilogueRestoreOrSpDeallocInstruction(const Instruction& inst) {
  if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
       inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
      inst.rt == Register::kSp && inst.rs == Register::kSp && inst.immediate > 0) {
    return true;
  }
  if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd ||
       inst.opcode == Opcode::kDaddu || inst.opcode == Opcode::kDadd) &&
      inst.rd == Register::kSp &&
      ((inst.rs == Register::kFp && inst.rt == Register::kZero) ||
       (inst.rs == Register::kZero && inst.rt == Register::kFp))) {
    return true;
  }
  if ((inst.opcode == Opcode::kLw || inst.opcode == Opcode::kLd) &&
      (inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.rt.has_value()) {
    const Register rt = *inst.rt;
    if (rt == Register::kRa || rt == Register::kFp ||
        (rt >= Register::kS0 && rt <= Register::kS7)) {
      return true;
    }
  }
  if ((inst.opcode == Opcode::kLwc1 || inst.opcode == Opcode::kLdc1) &&
      (inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.ft.has_value()) {
    const FpRegister ft = *inst.ft;
    if (ft >= FpRegister::kF20 && ft <= FpRegister::kF31) {
      return true;
    }
  }
  return false;
}

bool IsPartOfTwoInstructionRtlMacro(const BasicBlock& block, size_t cand_idx,
                                    const Instruction& term) {
  const Instruction& cand = block.instructions[cand_idx];
  auto prev_non_nop = [&](size_t idx) -> const Instruction* {
    while (idx > 0) {
      --idx;
      if (!block.instructions[idx].IsNop()) {
        return &block.instructions[idx];
      }
    }
    return nullptr;
  };
  auto next_non_nop = [&](size_t idx) -> const Instruction* {
    while (idx + 1 < block.instructions.size()) {
      ++idx;
      if (!block.instructions[idx].IsNop()) {
        return &block.instructions[idx];
      }
    }
    return nullptr;
  };
  const Instruction* prev = prev_non_nop(cand_idx);
  const Instruction* next = next_non_nop(cand_idx);
  // 1. Global load/store `lui $at, %hi(sym); lw/lh/lb/lwc1/ldc1/sw/sh/sb/swc1/sdc1 ...,
  // %lo(sym)($at)`
  // (`length = 2`).
  if ((cand.IsLoad() || cand.IsStore()) && cand.rs == Register::kAt && prev != nullptr &&
      prev->opcode == Opcode::kLui && prev->rt == Register::kAt) {
    return true;
  }
  if (cand.opcode == Opcode::kLui && cand.rt == Register::kAt && next != nullptr &&
      (next->IsLoad() || next->IsStore()) && next->rs == Register::kAt) {
    return true;
  }
  // 2. FPU single-precision immediate load `movsf_internal2` (`lui $at, ...; mtc1 $at, $f...`,
  // `length = 2` in `config/mips/mips.md:2081-2088`).
  if (cand.opcode == Opcode::kMtc1 && cand.rt == Register::kAt && prev != nullptr &&
      prev->opcode == Opcode::kLui && prev->rt == Register::kAt) {
    return true;
  }
  if (cand.opcode == Opcode::kLui && cand.rt == Register::kAt && next != nullptr &&
      next->opcode == Opcode::kMtc1 && next->rt == Register::kAt) {
    return true;
  }
  // 2. Integer-to-float conversion `floatsidf2` / `floatsisf2` (`mtc1/lwc1 + cvt.d.w/cvt.s.w`,
  // `length = 3` in `config/mips/mips.md:2595-2643`).
  if ((cand.opcode == Opcode::kCvtDW || cand.opcode == Opcode::kCvtSW) && prev != nullptr &&
      (prev->opcode == Opcode::kMtc1 || prev->opcode == Opcode::kLwc1)) {
    return true;
  }
  if ((cand.opcode == Opcode::kMtc1 || cand.opcode == Opcode::kLwc1) && next != nullptr &&
      (next->opcode == Opcode::kCvtDW || next->opcode == Opcode::kCvtSW)) {
    return true;
  }
  // 3. Symbol address load `la $r, sym` (`lui $r, %hi(sym); addiu $r, $r, %lo(sym)`, `length = 2`).
  if (cand.opcode == Opcode::kAddiu && cand.rt.has_value() && cand.rs == cand.rt &&
      prev != nullptr && prev->opcode == Opcode::kLui && prev->rt == cand.rt) {
    return true;
  }
  if (cand.opcode == Opcode::kLui && cand.rt.has_value() && next != nullptr &&
      next->opcode == Opcode::kAddiu && next->rt == cand.rt && next->rs == cand.rt) {
    return true;
  }
  (void)term;
  return false;
}

bool WasIneligibleForTerminatorDelaySlot(const BasicBlock& block, size_t cand_idx, size_t term_idx,
                                         const Instruction& term) {
  const Instruction& cand = block.instructions[cand_idx];
  if (cand.IsNop()) {
    return true;
  }
  if (!IsValidDelaySlotCandidate(cand) || (term.IsBranch() && cand.IsLoad()) ||
      IsFpuCompareOpcode(cand.opcode) || cand.opcode == Opcode::kSdc1 ||
      cand.opcode == Opcode::kLdc1 || cand.opcode == Opcode::kMtc1 ||
      cand.opcode == Opcode::kMfc1 || cand.opcode == Opcode::kCfc1 ||
      cand.opcode == Opcode::kCtc1 || IsPartOfTwoInstructionRtlMacro(block, cand_idx, term)) {
    return true;
  }
  // In KMC GCC 2.7.2 (`config/mips/mips.md:mulsi3_r4000`), `mult` immediately followed by `mflo`
  // is a single indivisible RTL instruction (`length = 3`).
  if (cand.opcode == Opcode::kMflo && cand_idx > 0 &&
      block.instructions[cand_idx - 1].opcode == Opcode::kMult) {
    return false;
  }
  if (DelaySlotConflictsWithTerminator(term, cand)) {
    return true;
  }
  if (term.IsCall()) {
    RegisterUseDef cand_ud = GetInstructionUseDef(cand);
    for (Register u : cand_ud.gpr_uses) {
      if (u == Register::kRa) return true;
    }
    for (Register d : cand_ud.gpr_defs) {
      if (d == Register::kRa) return true;
    }
  }
  for (size_t m = cand_idx + 1; m < term_idx; ++m) {
    if (InstructionsHaveHazard(cand, block.instructions[m])) {
      return true;
    }
  }
  return false;
}

bool ConsumesEarlierLoadOrFpOpInBlock(const BasicBlock& block, size_t before_idx,
                                      const Instruction& inst) {
  RegisterUseDef ud = GetInstructionUseDef(inst);
  if (ud.gpr_uses.empty() && ud.fpr_uses.empty()) {
    return false;
  }
  const Instruction& cand_inst = block.instructions[before_idx];
  const bool cand_is_macro = IsPartOfTwoInstructionRtlMacro(block, before_idx, cand_inst);
  for (size_t i = before_idx; i > 0; --i) {
    const Instruction& prev = block.instructions[i - 1];
    RegisterUseDef prev_ud = GetInstructionUseDef(prev);
    if (prev.IsLoad()) {
      for (Register d : prev_ud.gpr_defs) {
        if (std::find(ud.gpr_uses.begin(), ud.gpr_uses.end(), d) != ud.gpr_uses.end()) {
          return !(cand_is_macro && InstructionsHaveHazard(prev, cand_inst));
        }
      }
      for (FpRegister fd : prev_ud.fpr_defs) {
        if (std::find(ud.fpr_uses.begin(), ud.fpr_uses.end(), fd) != ud.fpr_uses.end()) {
          return !(cand_is_macro && InstructionsHaveHazard(prev, cand_inst));
        }
      }
    } else if (!prev_ud.fpr_defs.empty() && !ud.fpr_uses.empty()) {
      for (FpRegister fd : prev_ud.fpr_defs) {
        if (std::find(ud.fpr_uses.begin(), ud.fpr_uses.end(), fd) != ud.fpr_uses.end()) {
          return !(cand_is_macro && InstructionsHaveHazard(prev, cand_inst));
        }
      }
    }
  }
  return false;
}

bool IsRootOfMultiStepChainBeforeTerminator(const BasicBlock& block, size_t cand_idx,
                                            size_t term_idx, const Instruction& term) {
  const Instruction& cand = block.instructions[cand_idx];
  for (size_t i = 0; i < cand_idx; ++i) {
    if (InstructionsHaveHazard(block.instructions[i], cand)) {
      return false;
    }
  }
  for (size_t m = cand_idx + 1; m < term_idx; ++m) {
    if (InstructionsHaveHazard(cand, block.instructions[m])) {
      for (size_t k = m + 1; k < term_idx; ++k) {
        if (InstructionsHaveHazard(block.instructions[m], block.instructions[k]) &&
            (DelaySlotConflictsWithTerminator(term, block.instructions[k]) ||
             WasIneligibleForTerminatorDelaySlot(block, k, term_idx, term))) {
          return true;
        }
      }
    }
  }
  return false;
}

size_t FindSameBlockDelaySlotInsertionIndex(const BasicBlock& block, size_t term_idx,
                                            const Instruction& term,
                                            const Instruction& delay_inst) {
  size_t insert_idx = term_idx;
  if (term.IsReturn()) {
    if (!IsEpilogueRestoreOrSpDeallocInstruction(delay_inst)) {
      while (insert_idx > 0) {
        const Instruction& prev = block.instructions[insert_idx - 1];
        if (IsEpilogueRestoreOrSpDeallocInstruction(prev) &&
            !InstructionsHaveHazard(prev, delay_inst)) {
          --insert_idx;
        } else {
          break;
        }
      }
    }
    return insert_idx;
  }

  const RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  const bool delay_is_const_init =
      delay_ud.gpr_uses.empty() && delay_ud.fpr_uses.empty() && !delay_ud.gpr_defs.empty();
  const bool delay_is_ra_save =
      term.IsBranch() && IsPrologueCalleeSaveStore(delay_inst) && delay_inst.rt == Register::kRa;
  if (delay_is_ra_save) {
    bool has_sp_alloc = false;
    bool has_load_before_sp_alloc = false;
    bool has_other_sp_store = false;
    for (size_t i = 0; i < term_idx; ++i) {
      const Instruction& inst = block.instructions[i];
      if (!has_sp_alloc && inst.opcode == Opcode::kAddiu && inst.rt == Register::kSp &&
          inst.rs == Register::kSp && inst.immediate < 0) {
        has_sp_alloc = true;
      } else if (!has_sp_alloc && inst.IsLoad()) {
        has_load_before_sp_alloc = true;
      }
      if (IsPrologueCalleeSaveStore(inst)) {
        has_other_sp_store = true;
      }
    }
    if (!has_sp_alloc || has_load_before_sp_alloc || has_other_sp_store) {
      return term_idx;
    }
  }
  const bool stop_at_fp_save_before_call =
      (delay_ud.fpr_defs.empty() || delay_ud.fpr_defs[0] == FpRegister::kF12) &&
      std::find(delay_ud.gpr_uses.begin(), delay_ud.gpr_uses.end(), Register::kA0) ==
          delay_ud.gpr_uses.end();

  while (insert_idx > 0) {
    const size_t cand_idx = insert_idx - 1;
    const Instruction& prev = block.instructions[cand_idx];
    if (prev.IsNop()) {
      --insert_idx;
      continue;
    }
    if (InstructionsHaveHazard(prev, delay_inst)) {
      break;
    }
    if (delay_is_ra_save &&
        (IsPrologueCalleeSaveStore(prev) || prev.IsLoad() ||
         (prev.opcode == Opcode::kAddu && prev.rt == Register::kZero && prev.rs.has_value() &&
          *prev.rs >= Register::kA0 && *prev.rs <= Register::kA3))) {
      break;
    }
    if ((term.IsCall() || term.opcode == Opcode::kJ) && cand_idx > 0 &&
        block.instructions[cand_idx - 1].opcode == Opcode::kLui && prev.opcode == Opcode::kAddiu &&
        prev.rt == Register::kA0 && prev.rs == Register::kA0 &&
        delay_inst.opcode == Opcode::kAddu && delay_inst.rd == Register::kA1 &&
        delay_inst.rt == Register::kZero) {
      break;
    }
    if (term.IsCall() && (prev.opcode == Opcode::kMfc1 || prev.opcode == Opcode::kDmfc1 ||
                          (delay_is_const_init && prev.IsStore() && prev.rs != Register::kSp) ||
                          (stop_at_fp_save_before_call &&
                           (prev.opcode == Opcode::kMtc1 || prev.opcode == Opcode::kDmtc1 ||
                            prev.opcode == Opcode::kSdc1)) ||
                          (cand_idx == 1 && prev.opcode == Opcode::kSw &&
                           prev.rt == Register::kRa && prev.rs == Register::kSp))) {
      break;
    }
    if ((term.opcode == Opcode::kBc1t || term.opcode == Opcode::kBc1f ||
         term.opcode == Opcode::kBc1tl || term.opcode == Opcode::kBc1fl) &&
        IsFpuCompareOpcode(prev.opcode)) {
      break;
    }
    if (prev.IsLoad() && DelaySlotConflictsWithTerminator(term, prev)) {
      break;
    }
    if (!term.IsCall() && !prev.IsLoad() && !IsFpuCompareOpcode(prev.opcode) &&
        !(term.IsBranch() && (term.rs == Register::kAt || term.rt == Register::kAt)) &&
        ConsumesEarlierLoadOrFpOpInBlock(block, cand_idx, delay_inst)) {
      break;
    }
    if (delay_is_const_init && term.IsBranch() &&
        IsRootOfMultiStepChainBeforeTerminator(block, cand_idx, term_idx, term)) {
      break;
    }
    if (term.IsBranch() && (term.rs == Register::kAt || term.rt == Register::kAt)) {
      RegisterUseDef prev_ud = GetInstructionUseDef(prev);
      const bool defines_at = std::find(prev_ud.gpr_defs.begin(), prev_ud.gpr_defs.end(),
                                        Register::kAt) != prev_ud.gpr_defs.end();
      if (defines_at) {
        --insert_idx;
        continue;
      }
    }
    if (WasIneligibleForTerminatorDelaySlot(block, cand_idx, term_idx, term)) {
      --insert_idx;
    } else {
      break;
    }
  }
  return insert_idx;
}

bool UnscheduleAllSameBlockDelaySlotsInBlock(BasicBlock* block) {
  bool changed = false;
  // 1. Unschedule internal `jal` / `jalr` call delay slots.
  for (size_t i = 0; i + 1 < block->instructions.size(); ++i) {
    if (block->instructions[i].IsCall() && !block->instructions[i + 1].IsNop()) {
      const Instruction call_inst = block->instructions[i];
      const Instruction delay_inst = block->instructions[i + 1];
      const size_t insert_idx =
          FindSameBlockDelaySlotInsertionIndex(*block, i, call_inst, delay_inst);
      block->instructions[i + 1] = MakeNopInstruction(delay_inst.vram);
      block->instructions.insert(block->instructions.begin() + static_cast<ptrdiff_t>(insert_idx),
                                 delay_inst);
      ++i;
      changed = true;
    }
  }

  // 2. Unschedule block terminator (`jr`, `j`, or non-likely branch) delay slot.
  if (block->instructions.size() >= 2) {
    const size_t term_idx = block->instructions.size() - 2;
    const Instruction* term_ptr = block->Terminator();
    if (term_ptr != nullptr && term_ptr == &block->instructions[term_idx] &&
        !term_ptr->IsBranchLikely() && !block->instructions.back().IsNop()) {
      const Instruction term = *term_ptr;
      const Instruction delay_inst = block->instructions.back();
      // Check for `current_function_epilogue_delay_list` (`config/mips/mips.c:5189-5202`):
      // When `$ra` is the only saved register in the epilogue (`load_only_r31`),
      // `function_epilogue` emits `lw $ra, offset($sp)` followed by `epilogue_delay`, then `jr
      // $ra`, and puts `addiu $sp, $sp, tsize` in `jr $ra`'s delay slot.
      if (term.IsReturn() && IsEpilogueRestoreOrSpDeallocInstruction(delay_inst) &&
          delay_inst.rt == Register::kSp && term_idx >= 2) {
        const Instruction epilogue_delay = block->instructions[term_idx - 1];
        const Instruction ra_restore = block->instructions[term_idx - 2];
        if (!epilogue_delay.IsNop() && !IsEpilogueRestoreOrSpDeallocInstruction(epilogue_delay) &&
            IsEpilogueRestoreOrSpDeallocInstruction(ra_restore) && ra_restore.rt == Register::kRa &&
            !InstructionsHaveHazard(ra_restore, epilogue_delay)) {
          block->instructions[term_idx - 2] = epilogue_delay;
          block->instructions[term_idx - 1] = ra_restore;
        }
      }
      const size_t insert_idx =
          FindSameBlockDelaySlotInsertionIndex(*block, term_idx, term, delay_inst);
      block->instructions.back() = MakeNopInstruction(delay_inst.vram);
      block->instructions.insert(block->instructions.begin() + static_cast<ptrdiff_t>(insert_idx),
                                 delay_inst);
      changed = true;
    }
  }
  return changed;
}

}  // namespace

bool SimpleDelaySlotUnschedulerPass::Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return false;
  }
  bool changed = false;
  if (unschedule_all_remaining_) {
    for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
      if (UnscheduleAllSameBlockDelaySlotsInBlock(&cfg->MutableBlocks()[b])) {
        changed = true;
      }
    }
    if (cfg->CoalesceContiguousBlocks()) {
      changed = true;
    } else {
      cfg->CompactReachableBlocks();
    }
    if (changed && analyses != nullptr) {
      analyses->InvalidateAll();
    }
    return changed;
  }

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
        (DelaySlotCompletesSplitInstructionPair(block, delay_inst) ||
         (block.id == cfg->EntryBlockId() && IsPrologueCalleeSaveStore(delay_inst)))) {
      block.instructions[block.instructions.size() - 2] = delay_inst;
      block.instructions[block.instructions.size() - 1] = term;
      block.instructions.push_back(MakeNopInstruction(delay_inst.vram));
      changed = true;
    }
  }
  if (changed && analyses != nullptr) {
    analyses->InvalidateAll();
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
    if (jump_table_bounds_vrams.contains(term.vram)) {
      continue;
    }
    if (delay_inst.IsNop()) {
      if (!delay_inst.was_unscheduled_delay_slot && block.instructions.size() >= 3 &&
          ((term.opcode == Opcode::kJ && block.successors.size() == 1) ||
           (term.IsBranch() && !term.IsBranchLikely() && !term.IsUnconditionalBranch() &&
            block.successors.size() == 2))) {
        const DominatorTree& dom_tree = analyses->GetDominatorTree();
        const LoopInfo& loop_info = analyses->GetLoopInfo();
        const Loop* enclosing_loop = loop_info.GetLoopFor(block.id);
        if (enclosing_loop == nullptr || !enclosing_loop->IsLatch(block.id)) {
          uint32_t succ_taken = 0;
          uint32_t succ_fallthrough = 0;
          if (term.opcode == Opcode::kJ) {
            succ_taken = block.successors[0];
          } else {
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
          }
          const BasicBlock* taken_blk = cfg->GetBlock(succ_taken);
          if (taken_blk != nullptr && succ_taken != block.id &&
              (term.opcode == Opcode::kJ || succ_taken != succ_fallthrough) &&
              taken_blk->predecessors.size() >= 2 &&
              loop_info.GetLoopFor(succ_taken) == enclosing_loop &&
              (enclosing_loop == nullptr || succ_taken != enclosing_loop->header)) {
            std::optional<uint32_t> pred_block_id;
            for (uint32_t pred_id : taken_blk->predecessors) {
              if (pred_id == block.id ||
                  (term.opcode != Opcode::kJ && pred_id == succ_fallthrough)) {
                continue;
              }
              const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
              if (pred_blk != nullptr && pred_blk->instructions.size() == 1 &&
                  pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
                  pred_blk->end_vram + 4 == taken_blk->start_vram &&
                  loop_info.GetLoopFor(pred_id) == enclosing_loop &&
                  !dom_tree.Dominates(pred_id, block.id) &&
                  !dom_tree.Dominates(block.id, pred_id) &&
                  IsRedundantTargetHeaderInstruction(block, term, pred_blk->instructions.front())) {
                pred_block_id = pred_id;
                break;
              }
            }
            if (pred_block_id.has_value()) {
              const BasicBlock* pred_blk = cfg->GetBlock(*pred_block_id);
              RetargetBranchEdge(cfg, block.id, succ_taken, *pred_block_id);
              if (term.opcode == Opcode::kJ) {
                block.instructions[block.instructions.size() - 2].target =
                    (pred_blk->start_vram >> 2) & 0x03FFFFFF;
              }
              changed = true;
            }
          }
        }
      }
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
          // Restore the speculative target-header instruction `D` back to the target header and
          // remove both the delay-slot copy and the fallthrough inverse compensation instruction.
          BasicBlock* mut_head = cfg->GetMutableBlock(*head_block_id);
          BasicBlock* mut_taken = cfg->GetMutableBlock(succ_taken);
          if (mut_head != nullptr && mut_taken != nullptr) {
            if (mut_head->instructions.size() == 1) {
              RetargetBranchEdge(cfg, block.id, succ_taken, *head_block_id);
            } else {
              Instruction moved_inst = mut_head->instructions.back();
              mut_head->instructions.pop_back();
              mut_head->end_vram = mut_head->instructions.back().vram;
              mut_taken->instructions.insert(mut_taken->instructions.begin(), moved_inst);
              mut_taken->start_vram = moved_inst.vram;
              cfg->RebuildVramIndex();
            }
            block.instructions.back() = MakeNopInstruction(delay_inst.vram);
            ft_blk->instructions.erase(ft_blk->instructions.begin());
            if (!ft_blk->instructions.empty()) {
              ft_blk->start_vram = ft_blk->instructions.front().vram;
            }
            cfg->RebuildVramIndex();
            changed = true;
            continue;
          }
        }
      }
    }
  }
  if (changed) {
    analyses->InvalidateAll();
  }
  return changed;
}

namespace {

std::optional<uint32_t> MatchChainedFallthroughBranchToSameTarget(
    const ControlFlowGraph& cfg, const BasicBlock& block, uint32_t succ_taken,
    uint32_t succ_fallthrough, const RegisterUseDef& delay_ud, bool returns_v0, bool returns_f0,
    const Instruction* expected_delay = nullptr) {
  const BasicBlock* ft_blk = cfg.GetBlock(succ_fallthrough);
  if (ft_blk == nullptr || ft_blk->predecessors.size() != 1 ||
      ft_blk->predecessors[0] != block.id || ft_blk->instructions.size() < 2 ||
      (!ft_blk->instructions.back().IsNop() &&
       (expected_delay == nullptr ||
        !SameDelayInstructionEffect(*expected_delay, ft_blk->instructions.back()))) ||
      ft_blk->successors.size() != 2) {
    return std::nullopt;
  }
  const size_t ft_term_idx = ft_blk->instructions.size() - 2;
  const Instruction& ft_term = ft_blk->instructions[ft_term_idx];
  if (ft_blk->Terminator() != &ft_term || !ft_term.IsBranch() || ft_term.IsUnconditionalBranch()) {
    return std::nullopt;
  }
  uint32_t ft_taken = 0;
  uint32_t ft_fallthrough = 0;
  for (const auto& edge : ft_blk->outgoing_edges) {
    if (edge.type == EdgeType::kBranch) {
      ft_taken = edge.to_block_id;
    } else if (edge.type == EdgeType::kFallthrough) {
      ft_fallthrough = edge.to_block_id;
    }
  }
  if (ft_taken != succ_taken || ft_fallthrough == 0 || ft_fallthrough == succ_taken) {
    return std::nullopt;
  }
  for (size_t i = 0; i <= ft_term_idx; ++i) {
    RegisterUseDef ft_ud = GetInstructionUseDef(ft_blk->instructions[i]);
    for (Register d : delay_ud.gpr_defs) {
      if (std::find(ft_ud.gpr_uses.begin(), ft_ud.gpr_uses.end(), d) != ft_ud.gpr_uses.end() ||
          std::find(ft_ud.gpr_defs.begin(), ft_ud.gpr_defs.end(), d) != ft_ud.gpr_defs.end()) {
        return std::nullopt;
      }
    }
    for (FpRegister fd : delay_ud.fpr_defs) {
      if (std::find(ft_ud.fpr_uses.begin(), ft_ud.fpr_uses.end(), fd) != ft_ud.fpr_uses.end() ||
          std::find(ft_ud.fpr_defs.begin(), ft_ud.fpr_defs.end(), fd) != ft_ud.fpr_defs.end()) {
        return std::nullopt;
      }
    }
  }
  if (!AllDefinedRegistersDeadAtBlockEntry(cfg, ft_fallthrough, delay_ud, returns_v0, returns_f0)) {
    return std::nullopt;
  }
  return ft_fallthrough;
}

bool TryUnscheduleFallthroughJumpDelaySlot(
    ControlFlowGraph* cfg, const LoopInfo& loop_info,
    const absl::flat_hash_set<uint32_t>& jump_table_target_vrams, BasicBlock* block,
    const Instruction& delay_inst, uint32_t succ_taken, uint32_t succ_fallthrough,
    const RegisterUseDef& delay_ud, bool returns_v0, bool returns_f0) {
  if (!AllDefinedRegistersDeadAtBlockEntry(*cfg, succ_taken, delay_ud, returns_v0, returns_f0)) {
    return false;
  }
  BasicBlock* ft_blk = cfg->GetMutableBlock(succ_fallthrough);
  if (ft_blk == nullptr || ft_blk->predecessors.size() != 1 ||
      ft_blk->predecessors[0] != block->id || ft_blk->instructions.size() != 2 ||
      ft_blk->Terminator() != &ft_blk->instructions.front() ||
      ft_blk->instructions.front().opcode != Opcode::kJ || !ft_blk->instructions.back().IsNop() ||
      ft_blk->successors.size() != 1) {
    return false;
  }
  const uint32_t jump_target_id = ft_blk->successors[0];
  const BasicBlock* jump_target_blk = cfg->GetBlock(jump_target_id);
  const Loop* enclosing_loop = loop_info.GetLoopFor(block->id);
  if (jump_target_blk == nullptr || jump_target_id == ft_blk->id ||
      jump_target_blk->predecessors.size() < 2 ||
      (enclosing_loop != nullptr && jump_target_id == enclosing_loop->header) ||
      loop_info.GetLoopFor(jump_target_id) != enclosing_loop) {
    return false;
  }
  for (uint32_t pred_id : jump_target_blk->predecessors) {
    if (pred_id == ft_blk->id || pred_id == block->id) continue;
    const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
    if (pred_blk != nullptr && loop_info.GetLoopFor(pred_id) == enclosing_loop &&
        !jump_table_target_vrams.contains(pred_blk->start_vram) &&
        !pred_blk->predecessors.empty() && pred_blk->instructions.size() == 1 &&
        pred_blk->successors.size() == 1 && pred_blk->successors[0] == jump_target_id &&
        pred_blk->end_vram + 4 == jump_target_blk->start_vram &&
        SameDelayInstructionEffect(delay_inst, pred_blk->instructions.front())) {
      RetargetBranchEdge(cfg, ft_blk->id, jump_target_id, pred_id);
      ft_blk->instructions.front().target = (pred_blk->start_vram >> 2) & 0x03FFFFFF;
      block->instructions.back() = MakeNopInstruction(delay_inst.vram);
      return true;
    }
  }
  return false;
}

bool TryUnmergeHammockDelaySlot(ControlFlowGraph* cfg, const DominatorTree& dom_tree,
                                const DominatorTree& post_dom_tree, const LoopInfo& loop_info,
                                BasicBlock* block, const Instruction& term,
                                const Instruction& delay_inst, uint32_t succ_taken,
                                uint32_t succ_fallthrough, bool all_pre_branch_ineligible) {
  if (term.IsBranchLikely() || !IsSinkableDelaySlotInstruction(*block, delay_inst) ||
      DelaySlotHasPreBranchDependenceConflict(*block, term, delay_inst)) {
    return false;
  }
  const Loop* enclosing_loop = loop_info.GetLoopFor(block->id);
  if (loop_info.GetLoopFor(succ_taken) != enclosing_loop ||
      (enclosing_loop != nullptr && succ_taken == enclosing_loop->header) ||
      !dom_tree.Dominates(block->id, succ_taken) ||
      !post_dom_tree.Dominates(succ_taken, block->id)) {
    return false;
  }
  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  if (delay_ud.gpr_defs.size() != 1 || !delay_ud.fpr_defs.empty()) {
    return false;
  }
  const Register reg_d = delay_ud.gpr_defs[0];
  BasicBlock* taken_blk = cfg->GetMutableBlock(succ_taken);
  if (taken_blk == nullptr || taken_blk->instructions.empty() ||
      taken_blk->predecessors.size() < 2) {
    return false;
  }
  RegisterUseDef first_ud = GetInstructionUseDef(taken_blk->instructions.front());
  if (std::find(first_ud.gpr_uses.begin(), first_ud.gpr_uses.end(), reg_d) ==
      first_ud.gpr_uses.end()) {
    return false;
  }

  bool has_inner_nop_branch_to_taken = false;
  absl::flat_hash_set<uint32_t> visited = {succ_taken};
  std::vector<uint32_t> stack = {succ_fallthrough};
  while (!stack.empty()) {
    uint32_t cur_id = stack.back();
    stack.pop_back();
    if (!visited.insert(cur_id).second) continue;
    const BasicBlock* cur_blk = cfg->GetBlock(cur_id);
    if (cur_blk == nullptr || !dom_tree.Dominates(succ_fallthrough, cur_id) ||
        !post_dom_tree.Dominates(succ_taken, cur_id)) {
      return false;
    }
    if (cur_blk->instructions.size() >= 2 && cur_blk->instructions.back().IsNop()) {
      const Instruction* cur_term = cur_blk->Terminator();
      if (cur_term != nullptr && cur_term->IsBranch() && !cur_term->IsUnconditionalBranch()) {
        for (const auto& edge : cur_blk->outgoing_edges) {
          if (edge.type == EdgeType::kBranch && edge.to_block_id == succ_taken) {
            has_inner_nop_branch_to_taken = true;
            break;
          }
        }
      }
    }
    for (const auto& inst : cur_blk->instructions) {
      if (inst.IsCall()) {
        if (reg_d < Register::kS0 || reg_d > Register::kFp) return false;
        for (Register u : delay_ud.gpr_uses) {
          if (u < Register::kS0 || u > Register::kFp) return false;
        }
        for (FpRegister fu : delay_ud.fpr_uses) {
          if (fu < FpRegister::kF20) return false;
        }
      }
      RegisterUseDef ud = GetInstructionUseDef(inst);
      if (std::find(ud.gpr_uses.begin(), ud.gpr_uses.end(), reg_d) != ud.gpr_uses.end() ||
          std::find(ud.gpr_defs.begin(), ud.gpr_defs.end(), reg_d) != ud.gpr_defs.end()) {
        return false;
      }
      for (Register d : ud.gpr_defs) {
        if (std::find(delay_ud.gpr_uses.begin(), delay_ud.gpr_uses.end(), d) !=
            delay_ud.gpr_uses.end()) {
          return false;
        }
      }
      for (FpRegister fd : ud.fpr_defs) {
        if (std::find(delay_ud.fpr_uses.begin(), delay_ud.fpr_uses.end(), fd) !=
            delay_ud.fpr_uses.end()) {
          return false;
        }
      }
    }
    for (uint32_t s : cur_blk->successors) {
      if (s != succ_taken) {
        stack.push_back(s);
      }
    }
  }
  const bool is_ineligible_pre_branch_self_increment =
      all_pre_branch_ineligible && delay_ud.gpr_uses == delay_ud.gpr_defs;
  if (!has_inner_nop_branch_to_taken && !is_ineligible_pre_branch_self_increment) {
    return false;
  }

  Instruction moved_inst = delay_inst;
  moved_inst.vram = taken_blk->start_vram;
  taken_blk->instructions.insert(taken_blk->instructions.begin(), moved_inst);
  block->instructions.back() = MakeNopInstruction(delay_inst.vram);
  return true;
}

}  // namespace

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
  const bool returns_f0 = AbiAnalyzer::DetermineReturnsF0(*cfg);
  const bool returns_v0 = !returns_f0 && AbiAnalyzer::DetermineReturnsV0(*cfg);
  absl::flat_hash_set<uint32_t> jump_table_target_vrams;
  for (const auto& jt : cfg->JumpTables()) {
    for (const auto& entry : jt.entries) {
      if (entry.target_vram != jt.default_target_vram) {
        jump_table_target_vrams.insert(entry.target_vram);
      }
    }
  }

  bool any_changed = false;
  for (int iter = 0; iter < 4; ++iter) {
    const DominatorTree& dom_tree = analyses->GetDominatorTree();
    const DominatorTree& post_dom_tree = analyses->GetPostDominatorTree();
    const LoopInfo& loop_info = analyses->GetLoopInfo();
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
      const Loop* enclosing_loop = loop_info.GetLoopFor(block.id);

      if (term.opcode == Opcode::kJ && block.successors.size() == 1 &&
          IsValidDelaySlotCandidate(delay_inst)) {
        const uint32_t succ_taken = block.successors[0];
        BasicBlock* taken_block = cfg->GetMutableBlock(succ_taken);
        if (taken_block != nullptr && succ_taken != block.id &&
            taken_block->predecessors.size() >= 2) {
          std::optional<uint32_t> pred_block_id;
          std::optional<uint32_t> split_fallthrough_pred_id;
          for (uint32_t pred_id : taken_block->predecessors) {
            if (pred_id == block.id) continue;
            const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
            if (pred_blk == nullptr || jump_table_target_vrams.contains(pred_blk->start_vram) ||
                pred_blk->successors.size() != 1 || pred_blk->successors[0] != succ_taken ||
                pred_blk->end_vram + 4 != taken_block->start_vram) {
              continue;
            }
            if (!pred_blk->predecessors.empty() && pred_blk->instructions.size() == 1 &&
                SameDelayInstructionEffect(delay_inst, pred_blk->instructions.front())) {
              pred_block_id = pred_id;
              break;
            }
            if (pred_blk->instructions.size() > 1 && !pred_blk->HasBranch() &&
                !pred_blk->HasReturn() && pred_blk->outgoing_edges.size() == 1 &&
                pred_blk->outgoing_edges[0].type == EdgeType::kFallthrough &&
                taken_block->predecessors.size() == 2u &&
                SameDelayInstructionEffect(delay_inst, pred_blk->instructions.back())) {
              split_fallthrough_pred_id = pred_id;
              break;
            }
          }
          if (pred_block_id.has_value()) {
            const BasicBlock* pred_blk = cfg->GetBlock(*pred_block_id);
            RetargetBranchEdge(cfg, block.id, succ_taken, *pred_block_id);
            block.instructions[block.instructions.size() - 2].target =
                (pred_blk->start_vram >> 2) & 0x03FFFFFF;
            block.instructions.back() = MakeNopInstruction(delay_inst.vram);
            changed = true;
            continue;
          }
          if (split_fallthrough_pred_id.has_value()) {
            BasicBlock* mut_pred = cfg->GetMutableBlock(*split_fallthrough_pred_id);
            if (mut_pred != nullptr && mut_pred->instructions.size() > 1) {
              Instruction moved_inst = mut_pred->instructions.back();
              mut_pred->instructions.pop_back();
              mut_pred->end_vram = mut_pred->instructions.back().vram;
              taken_block->instructions.insert(taken_block->instructions.begin(), moved_inst);
              taken_block->start_vram = moved_inst.vram;
              block.instructions[block.instructions.size() - 2].target =
                  (taken_block->start_vram >> 2) & 0x03FFFFFF;
              block.instructions.back() = MakeNopInstruction(delay_inst.vram);
              cfg->RebuildVramIndex();
              changed = true;
              continue;
            }
          }
        }
        continue;
      }

      if (!term.IsBranch() || term.IsUnconditionalBranch() || block.successors.size() != 2) {
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

      const size_t term_idx = block.instructions.size() - 2;
      bool all_pre_branch_ineligible = term_idx > 0;
      for (size_t i = 0; i < term_idx; ++i) {
        if (!WasIneligibleForTerminatorDelaySlot(block, i, term_idx, term)) {
          all_pre_branch_ineligible = false;
          break;
        }
      }
      const bool has_pre_branch_conflict =
          term.IsBranchLikely() || DelaySlotHasPreBranchDependenceConflict(block, term, delay_inst);
      uint32_t effective_fallthrough = succ_fallthrough;
      std::optional<uint32_t> chained_ft_branch_block_id;
      if (!term.IsBranchLikely()) {
        RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
        if (!IsValidDelaySlotCandidate(delay_inst)) {
          continue;
        }
        if (!AllDefinedRegistersDeadAtBlockEntry(*cfg, succ_fallthrough, delay_ud, returns_v0,
                                                 returns_f0)) {
          if (TryUnscheduleFallthroughJumpDelaySlot(cfg, loop_info, jump_table_target_vrams, &block,
                                                    delay_inst, succ_taken, succ_fallthrough,
                                                    delay_ud, returns_v0, returns_f0)) {
            changed = true;
            continue;
          }
          auto chained_ft = MatchChainedFallthroughBranchToSameTarget(
              *cfg, block, succ_taken, succ_fallthrough, delay_ud, returns_v0, returns_f0);
          if (!chained_ft.has_value()) {
            if (TryUnmergeHammockDelaySlot(cfg, dom_tree, post_dom_tree, loop_info, &block, term,
                                           delay_inst, succ_taken, succ_fallthrough,
                                           all_pre_branch_ineligible)) {
              changed = true;
            }
            continue;
          }
          chained_ft_branch_block_id = succ_fallthrough;
          effective_fallthrough = *chained_ft;
        }
      }

      const Loop* target_loop = loop_info.GetLoopFor(succ_taken);
      std::optional<uint32_t> pred_block_id;
      std::optional<uint32_t> split_fallthrough_pred_id;
      size_t split_fallthrough_count = 1;
      for (uint32_t pred_id : taken_block->predecessors) {
        if (pred_id == block.id ||
            (chained_ft_branch_block_id.has_value() && pred_id == *chained_ft_branch_block_id)) {
          continue;
        }
        const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
        const Loop* pred_loop = loop_info.GetLoopFor(pred_id);
        auto uses_preserved_to_pred = [&]() {
          absl::flat_hash_set<uint32_t> can_reach_pred = {pred_id};
          std::vector<uint32_t> rev_stack = {pred_id};
          while (!rev_stack.empty()) {
            uint32_t succ = rev_stack.back();
            rev_stack.pop_back();
            const BasicBlock* succ_blk = cfg->GetBlock(succ);
            if (succ_blk == nullptr) continue;
            for (uint32_t cand_id : succ_blk->predecessors) {
              if (cand_id == block.id || can_reach_pred.contains(cand_id)) {
                continue;
              }
              if (!dom_tree.Dominates(succ, cand_id)) {
                can_reach_pred.insert(cand_id);
                rev_stack.push_back(cand_id);
              }
            }
          }
          if (!can_reach_pred.contains(effective_fallthrough)) {
            return false;
          }
          RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
          if (delay_ud.gpr_uses.empty() && delay_ud.fpr_uses.empty()) {
            return true;
          }
          absl::flat_hash_set<uint32_t> visited_path = {pred_id};
          std::vector<uint32_t> stack = {effective_fallthrough};
          while (!stack.empty()) {
            uint32_t cur_id = stack.back();
            stack.pop_back();
            if (!visited_path.insert(cur_id).second) continue;
            const BasicBlock* cur_blk = cfg->GetBlock(cur_id);
            if (cur_blk == nullptr) return false;
            bool skip_annulled_delay = false;
            if (cur_blk->instructions.size() >= 2 &&
                cur_blk->instructions[cur_blk->instructions.size() - 2].IsBranchLikely()) {
              uint32_t cur_taken = 0;
              for (const auto& edge : cur_blk->outgoing_edges) {
                if (edge.type == EdgeType::kBranch) {
                  cur_taken = edge.to_block_id;
                  break;
                }
              }
              if (cur_taken != 0 && !can_reach_pred.contains(cur_taken)) {
                skip_annulled_delay = true;
              }
            }
            const size_t check_count = skip_annulled_delay ? (cur_blk->instructions.size() - 1)
                                                           : cur_blk->instructions.size();
            for (size_t i = 0; i < check_count; ++i) {
              const auto& inst = cur_blk->instructions[i];
              if (inst.IsCall()) {
                for (Register u : delay_ud.gpr_uses) {
                  if (u < Register::kS0 || u > Register::kFp) return false;
                }
                for (FpRegister fu : delay_ud.fpr_uses) {
                  if (fu < FpRegister::kF20) return false;
                }
              }
              RegisterUseDef ud = GetInstructionUseDef(inst);
              for (Register d : ud.gpr_defs) {
                if (std::find(delay_ud.gpr_uses.begin(), delay_ud.gpr_uses.end(), d) !=
                    delay_ud.gpr_uses.end()) {
                  return false;
                }
              }
              for (FpRegister fd : ud.fpr_defs) {
                if (std::find(delay_ud.fpr_uses.begin(), delay_ud.fpr_uses.end(), fd) !=
                    delay_ud.fpr_uses.end()) {
                  return false;
                }
              }
            }
            for (uint32_t s : cur_blk->successors) {
              if (s != pred_id && can_reach_pred.contains(s)) {
                stack.push_back(s);
              }
            }
          }
          return true;
        };
        auto valid_relax_target = [&]() {
          if (pred_blk == nullptr || jump_table_target_vrams.contains(pred_blk->start_vram) ||
              pred_blk->end_vram + 4 != taken_block->start_vram ||
              taken_block->predecessors.size() < 2 || !IsValidDelaySlotCandidate(delay_inst)) {
            return false;
          }
          if (term.IsBranchLikely()) {
            return true;
          }
          return pred_loop == enclosing_loop && target_loop == enclosing_loop &&
                 ((!dom_tree.Dominates(effective_fallthrough, pred_id) &&
                   !pred_blk->predecessors.empty()) ||
                  uses_preserved_to_pred());
        };
        const bool cross_loop_valid =
            (has_pre_branch_conflict ||
             (pred_blk != nullptr && pred_blk->instructions.size() == 1)) &&
            pred_blk != nullptr && !jump_table_target_vrams.contains(pred_blk->start_vram) &&
            pred_blk->end_vram + 4 == taken_block->start_vram &&
            ((enclosing_loop != nullptr && enclosing_loop->Contains(block.id) &&
              !enclosing_loop->Contains(pred_id)) ||
             (target_loop != nullptr && target_loop->header == succ_taken &&
              !target_loop->Contains(block.id) && !target_loop->Contains(pred_id)));
        if (pred_id != succ_fallthrough && pred_id != effective_fallthrough &&
            pred_blk != nullptr && pred_blk->instructions.size() == 1 &&
            pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
            SameDelayInstructionEffect(delay_inst, pred_blk->instructions.front()) &&
            (valid_relax_target() || cross_loop_valid)) {
          pred_block_id = pred_id;
          break;
        }
        // `reorg.c:3918-3936`: if `trial` at `target_label` was an unconditional jump with one
        // instruction in its delay slot redundant with `block`'s delay slot, `relax_delay_slots`
        // threaded `block` across `trial` to `succ_taken`.
        if (term.IsBranchLikely() && pred_id != succ_fallthrough &&
            pred_id != effective_fallthrough && pred_blk != nullptr &&
            !jump_table_target_vrams.contains(pred_blk->start_vram) &&
            pred_blk->instructions.size() == 2 &&
            pred_blk->Terminator() == &pred_blk->instructions[0] &&
            pred_blk->instructions[0].opcode == Opcode::kJ && pred_blk->successors.size() == 1 &&
            pred_blk->successors[0] == succ_taken &&
            SameDelayInstructionEffect(delay_inst, pred_blk->instructions[1])) {
          pred_block_id = pred_id;
          break;
        }
        if (!chained_ft_branch_block_id.has_value() && pred_blk != nullptr &&
            pred_blk->instructions.size() > 1 && !pred_blk->HasBranch() && !pred_blk->HasReturn() &&
            pred_blk->outgoing_edges.size() == 1 &&
            pred_blk->outgoing_edges[0].type == EdgeType::kFallthrough &&
            pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
            taken_block->predecessors.size() == 2u && (valid_relax_target() || cross_loop_valid) &&
            (has_pre_branch_conflict || all_pre_branch_ineligible ||
             DelaySlotFeedsSuccessorBranch(*taken_block, delay_inst))) {
          size_t matched_count = 0;
          if (SameDelayInstructionEffect(delay_inst, pred_blk->instructions.back())) {
            matched_count = 1;
          } else if (block.instructions.size() >= 3) {
            const size_t pre_term_len = block.instructions.size() - 2;
            if (pred_blk->instructions.size() > pre_term_len + 1) {
              const size_t cand_start = pred_blk->instructions.size() - (pre_term_len + 1);
              if (SameDelayInstructionEffect(delay_inst, pred_blk->instructions[cand_start])) {
                bool suffix_matches = true;
                for (size_t j = 0; j < pre_term_len; ++j) {
                  const Instruction& red_inst = pred_blk->instructions[cand_start + 1 + j];
                  if (!(block.instructions[j].IsNop() && red_inst.IsNop()) &&
                      !SameDelayInstructionEffect(block.instructions[j], red_inst)) {
                    suffix_matches = false;
                    break;
                  }
                }
                if (suffix_matches) {
                  matched_count = pre_term_len + 1;
                }
              }
            }
          }
          if (matched_count > 0) {
            split_fallthrough_pred_id = pred_id;
            split_fallthrough_count = matched_count;
            break;
          }
        }
      }
      if (pred_block_id.has_value()) {
        RetargetBranchEdge(cfg, block.id, succ_taken, *pred_block_id);
        if (chained_ft_branch_block_id.has_value()) {
          RetargetBranchEdge(cfg, *chained_ft_branch_block_id, succ_taken, *pred_block_id);
        }
        block.instructions[block.instructions.size() - 2].opcode =
            ToNonLikelyBranchOpcode(term.opcode);
        block.instructions.back() = MakeNopInstruction(delay_inst.vram);
        changed = true;
      } else if (split_fallthrough_pred_id.has_value()) {
        BasicBlock* mut_pred = cfg->GetMutableBlock(*split_fallthrough_pred_id);
        if (mut_pred != nullptr && mut_pred->instructions.size() > split_fallthrough_count) {
          if (split_fallthrough_count > 1 && !taken_block->instructions.empty() &&
              taken_block->instructions.size() >= 2 &&
              taken_block->Terminator() == &taken_block->instructions.front() &&
              taken_block->instructions.front().IsBranchLikely() &&
              !taken_block->instructions[1].IsNop()) {
            const Instruction tk_term = taken_block->instructions.front();
            const Instruction tk_delay = taken_block->instructions[1];
            uint32_t tk_taken = 0;
            uint32_t tk_ft = 0;
            for (const auto& edge : taken_block->outgoing_edges) {
              if (edge.type == EdgeType::kBranch) {
                tk_taken = edge.to_block_id;
              } else if (edge.type == EdgeType::kFallthrough) {
                tk_ft = edge.to_block_id;
              }
            }
            BasicBlock* tk_taken_blk = cfg->GetMutableBlock(tk_taken);
            if (tk_taken_blk != nullptr && tk_taken != tk_ft &&
                tk_taken_blk->predecessors.size() == 2) {
              for (uint32_t tk_pred_id : tk_taken_blk->predecessors) {
                if (tk_pred_id == succ_taken) continue;
                BasicBlock* tk_pred = cfg->GetMutableBlock(tk_pred_id);
                if (tk_pred != nullptr && tk_pred->instructions.size() > 1 &&
                    tk_pred->end_vram + 4 == tk_taken_blk->start_vram &&
                    tk_pred->successors.size() == 1 && tk_pred->successors[0] == tk_taken &&
                    !tk_pred->HasBranch() && !tk_pred->HasReturn() &&
                    SameDelayInstructionEffect(tk_delay, tk_pred->instructions.back())) {
                  Instruction tk_moved = tk_pred->instructions.back();
                  tk_pred->instructions.pop_back();
                  tk_pred->end_vram = tk_pred->instructions.back().vram;
                  tk_taken_blk->instructions.insert(tk_taken_blk->instructions.begin(), tk_moved);
                  tk_taken_blk->start_vram = tk_moved.vram;
                  taken_block->instructions[0].opcode = ToNonLikelyBranchOpcode(tk_term.opcode);
                  taken_block->instructions[0].target = tk_taken_blk->start_vram;
                  taken_block->instructions[1] = MakeNopInstruction(tk_delay.vram);
                  break;
                }
              }
            }
          }
          for (size_t k = 0; k < split_fallthrough_count; ++k) {
            Instruction moved_inst = mut_pred->instructions.back();
            mut_pred->instructions.pop_back();
            taken_block->instructions.insert(taken_block->instructions.begin(), moved_inst);
            taken_block->start_vram = moved_inst.vram;
          }
          mut_pred->end_vram = mut_pred->instructions.back().vram;
          block.instructions[block.instructions.size() - 2].opcode =
              ToNonLikelyBranchOpcode(term.opcode);
          block.instructions[block.instructions.size() - 2].target = taken_block->start_vram;
          block.instructions.back() = MakeNopInstruction(delay_inst.vram);
          cfg->RebuildVramIndex();
          changed = true;
        }
      } else if (!term.IsBranchLikely() &&
                 TryUnmergeHammockDelaySlot(cfg, dom_tree, post_dom_tree, loop_info, &block, term,
                                            delay_inst, succ_taken, succ_fallthrough,
                                            all_pre_branch_ineligible)) {
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
    analyses->InvalidateAll();
    any_changed = true;
  }
  return any_changed;
}

namespace {

std::optional<Register> ComputeUnscheduledClobberReg(const RegisterUseDef& delay_ud) {
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
}

size_t FindThreadSunkInsertionIndex(const BasicBlock& target_blk, const Instruction& pred_term,
                                    const Instruction& sunk_inst, bool from_taken_thread = true) {
  (void)pred_term;
  if (target_blk.instructions.empty()) {
    return 0;
  }
  const Instruction& first = target_blk.instructions[0];
  if (first.HasDelaySlot() || first.IsNop() || InstructionsHaveHazard(first, sunk_inst)) {
    return 0;
  }
  if ((first.IsLoad() && !IsEpilogueRestoreOrSpDeallocInstruction(first)) ||
      (!from_taken_thread && first.IsStore())) {
    return 1;
  }
  return 0;
}

bool TrySinkCommonPredecessorDelaySlots(
    ControlFlowGraph* cfg, const LoopInfo& loop_info,
    const absl::flat_hash_set<uint32_t>& jump_table_target_vrams, bool returns_v0,
    bool returns_f0) {
  bool changed = false;
  for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
    BasicBlock& target_blk = cfg->MutableBlocks()[b];
    if (target_blk.id == cfg->EntryBlockId() || target_blk.predecessors.size() < 2 ||
        target_blk.instructions.empty() ||
        jump_table_target_vrams.contains(target_blk.start_vram)) {
      continue;
    }
    const Loop* enclosing_loop = loop_info.GetLoopFor(target_blk.id);

    std::optional<Instruction> common_delay;
    std::vector<uint32_t> branch_preds;
    std::vector<uint32_t> fallthrough_body_preds;
    bool all_preds_match = true;
    bool has_motivating_conflict = false;

    for (uint32_t pred_id : target_blk.predecessors) {
      if (pred_id == target_blk.id) {
        all_preds_match = false;
        break;
      }
      const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
      if (pred_blk == nullptr || pred_blk->instructions.empty()) {
        all_preds_match = false;
        break;
      }

      const Instruction* pred_term_ptr = pred_blk->Terminator();
      const bool ends_with_delay_slot =
          pred_blk->instructions.size() >= 2 && pred_term_ptr != nullptr &&
          pred_term_ptr == &pred_blk->instructions[pred_blk->instructions.size() - 2] &&
          pred_term_ptr->HasDelaySlot();

      if (ends_with_delay_slot) {
        const Instruction pred_term = *pred_term_ptr;
        const Instruction pred_delay = pred_blk->instructions.back();
        if (pred_delay.IsNop() || !pred_term.IsBranch() || pred_term.IsUnconditionalBranch() ||
            pred_blk->successors.size() != 2 || !IsValidDelaySlotCandidate(pred_delay)) {
          all_preds_match = false;
          break;
        }
        uint32_t succ_taken = 0;
        uint32_t succ_fallthrough = 0;
        for (const auto& edge : pred_blk->outgoing_edges) {
          if (edge.type == EdgeType::kBranch) {
            succ_taken = edge.to_block_id;
          } else if (edge.type == EdgeType::kFallthrough) {
            succ_fallthrough = edge.to_block_id;
          }
        }
        if (succ_taken == 0 && succ_fallthrough == 0) {
          succ_taken = pred_blk->successors[0];
          succ_fallthrough = pred_blk->successors[1];
        }
        if (succ_taken == succ_fallthrough || succ_taken == pred_id ||
            succ_fallthrough == pred_id ||
            (succ_taken != target_blk.id && succ_fallthrough != target_blk.id)) {
          all_preds_match = false;
          break;
        }
        const uint32_t other_succ = (succ_taken == target_blk.id) ? succ_fallthrough : succ_taken;
        if (pred_term.IsBranchLikely()) {
          if (succ_taken != target_blk.id) {
            all_preds_match = false;
            break;
          }
          const BasicBlock* ft_blk = cfg->GetBlock(succ_fallthrough);
          if (ft_blk != nullptr && !ft_blk->instructions.empty() &&
              SameDelayInstructionEffect(pred_delay, ft_blk->instructions.front())) {
            all_preds_match = false;
            break;
          }
        } else {
          if (!IsSinkableDelaySlotInstruction(*pred_blk, pred_delay) ||
              loop_info.GetLoopFor(pred_id) != enclosing_loop) {
            all_preds_match = false;
            break;
          }
          RegisterUseDef delay_ud = GetInstructionUseDef(pred_delay);
          if (!AllDefinedRegistersDeadAtBlockEntry(*cfg, other_succ, delay_ud, returns_v0,
                                                   returns_f0) &&
              !MatchChainedFallthroughBranchToSameTarget(*cfg, *pred_blk, target_blk.id, other_succ,
                                                         delay_ud, returns_v0, returns_f0,
                                                         &pred_delay)
                   .has_value()) {
            all_preds_match = false;
            break;
          }
        }
        if (!common_delay.has_value()) {
          common_delay = pred_delay;
        } else if (!SameDelayInstructionEffect(*common_delay, pred_delay)) {
          all_preds_match = false;
          break;
        }
        const size_t pred_term_idx = pred_blk->instructions.size() - 2;
        bool pred_all_pre_branch_ineligible = true;
        for (size_t i = 0; i < pred_term_idx; ++i) {
          if (!WasIneligibleForTerminatorDelaySlot(*pred_blk, i, pred_term_idx, pred_term)) {
            pred_all_pre_branch_ineligible = false;
            break;
          }
        }
        if (pred_term.IsBranchLikely() || pred_all_pre_branch_ineligible ||
            DelaySlotConflictsWithTerminator(pred_term, pred_delay) ||
            DelaySlotHasPreBranchDependenceConflict(*pred_blk, pred_term, pred_delay) ||
            DelaySlotFeedsSuccessorBranch(target_blk, pred_delay)) {
          has_motivating_conflict = true;
        }
        branch_preds.push_back(pred_id);
      } else if (pred_blk->successors.size() == 1 && pred_blk->successors[0] == target_blk.id &&
                 pred_blk->end_vram + 4 == target_blk.start_vram && !pred_blk->HasBranch() &&
                 !pred_blk->HasReturn()) {
        const Instruction last_inst = pred_blk->instructions.back();
        if (!IsValidDelaySlotCandidate(last_inst)) {
          all_preds_match = false;
          break;
        }
        if (!common_delay.has_value()) {
          common_delay = last_inst;
        } else if (!SameDelayInstructionEffect(*common_delay, last_inst)) {
          all_preds_match = false;
          break;
        }
        fallthrough_body_preds.push_back(pred_id);
      } else {
        all_preds_match = false;
        break;
      }
    }

    if (all_preds_match && common_delay.has_value() && !branch_preds.empty() &&
        has_motivating_conflict) {
      Instruction sunk_inst = *common_delay;
      if (!fallthrough_body_preds.empty()) {
        const BasicBlock* ft_pred = cfg->GetBlock(fallthrough_body_preds.front());
        if (ft_pred != nullptr && !ft_pred->instructions.empty()) {
          sunk_inst.vram = ft_pred->instructions.back().vram;
        } else {
          sunk_inst.vram = target_blk.start_vram;
        }
      } else {
        sunk_inst.vram = target_blk.start_vram;
      }
      RegisterUseDef delay_ud = GetInstructionUseDef(sunk_inst);
      std::optional<Register> candidate_clobber_reg = ComputeUnscheduledClobberReg(delay_ud);
      size_t insert_idx = 0;
      bool all_branch_preds_skip_first = true;
      for (uint32_t pred_id : branch_preds) {
        const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
        if (pred_blk == nullptr || pred_blk->instructions.size() < 2) {
          all_branch_preds_skip_first = false;
          break;
        }
        const Instruction pred_term = pred_blk->instructions[pred_blk->instructions.size() - 2];
        if (FindThreadSunkInsertionIndex(target_blk, pred_term, sunk_inst) == 0) {
          all_branch_preds_skip_first = false;
          break;
        }
      }
      if (all_branch_preds_skip_first) {
        insert_idx = 1;
      }
      target_blk.instructions.insert(
          target_blk.instructions.begin() + static_cast<std::ptrdiff_t>(insert_idx), sunk_inst);
      if (insert_idx == 0) {
        target_blk.start_vram = sunk_inst.vram;
      }
      for (uint32_t pred_id : branch_preds) {
        BasicBlock* pred_blk = cfg->GetMutableBlock(pred_id);
        if (pred_blk == nullptr || pred_blk->instructions.size() < 2) continue;
        const Instruction pred_term = pred_blk->instructions[pred_blk->instructions.size() - 2];
        const Instruction pred_delay = pred_blk->instructions.back();
        pred_blk->instructions[pred_blk->instructions.size() - 2].opcode =
            ToNonLikelyBranchOpcode(pred_term.opcode);
        for (const auto& edge : pred_blk->outgoing_edges) {
          if (edge.type == EdgeType::kBranch && edge.to_block_id == target_blk.id) {
            pred_blk->instructions[pred_blk->instructions.size() - 2].target =
                target_blk.start_vram;
            break;
          }
        }
        std::optional<Register> clobber_reg;
        if (candidate_clobber_reg.has_value()) {
          std::vector<Instruction> with_plain_nop = pred_blk->instructions;
          with_plain_nop.back() = MakeNopInstruction(pred_delay.vram);
          auto plain_folded = BranchConditionBuilder::FindFoldedComparison(with_plain_nop);
          if (!plain_folded.has_value() || !plain_folded->can_suppress_statement) {
            std::vector<Instruction> with_clobber_nop = pred_blk->instructions;
            with_clobber_nop.back() = MakeNopInstruction(pred_delay.vram, candidate_clobber_reg);
            auto clobber_folded = BranchConditionBuilder::FindFoldedComparison(with_clobber_nop);
            if (clobber_folded.has_value() && clobber_folded->can_suppress_statement) {
              clobber_reg = candidate_clobber_reg;
            }
          }
        }
        pred_blk->instructions.back() = MakeNopInstruction(pred_delay.vram, clobber_reg);
      }
      for (uint32_t pred_id : fallthrough_body_preds) {
        BasicBlock* pred_blk = cfg->GetMutableBlock(pred_id);
        if (pred_blk == nullptr || pred_blk->instructions.empty()) continue;
        if (pred_blk->instructions.size() > 1) {
          pred_blk->instructions.pop_back();
          pred_blk->end_vram = pred_blk->instructions.back().vram;
        } else {
          pred_blk->instructions.back() = MakeNopInstruction(pred_blk->instructions.back().vram);
        }
      }
      cfg->RebuildVramIndex();
      changed = true;
    }
  }
  return changed;
}

bool TryRestoreSharedSwitchDefaultBlocks(
    ControlFlowGraph* cfg, const absl::flat_hash_set<uint32_t>& jump_table_target_vrams) {
  bool changed = false;
  for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
    BasicBlock& target_blk = cfg->MutableBlocks()[b];
    if (target_blk.id == cfg->EntryBlockId() || target_blk.predecessors.size() < 3 ||
        target_blk.instructions.empty() ||
        jump_table_target_vrams.contains(target_blk.start_vram)) {
      continue;
    }
    bool has_synthetic_ft_pred = false;
    std::optional<uint32_t> physical_ft_pred_id;
    std::vector<uint32_t> bare_jump_preds;
    for (uint32_t pred_id : target_blk.predecessors) {
      if (pred_id == target_blk.id) continue;
      const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
      if (pred_blk == nullptr || pred_blk->instructions.empty()) continue;
      if (pred_blk->end_vram == 0 && pred_blk->outgoing_edges.size() == 1 &&
          pred_blk->outgoing_edges[0].type == EdgeType::kFallthrough) {
        has_synthetic_ft_pred = true;
        break;
      }
      if (pred_blk->end_vram + 4 == target_blk.start_vram && pred_blk->successors.size() == 1 &&
          pred_blk->successors[0] == target_blk.id && pred_blk->outgoing_edges.size() == 1 &&
          pred_blk->outgoing_edges[0].type == EdgeType::kFallthrough && !pred_blk->HasBranch() &&
          !pred_blk->HasReturn() &&
          (pred_blk->Terminator() == nullptr || pred_blk->Terminator()->opcode != Opcode::kJ)) {
        physical_ft_pred_id = pred_id;
      } else if (pred_blk->instructions.size() == 2 &&
                 pred_blk->Terminator() == &pred_blk->instructions[0] &&
                 pred_blk->instructions[0].opcode == Opcode::kJ &&
                 !pred_blk->instructions[1].IsNop() && pred_blk->successors.size() == 1 &&
                 pred_blk->successors[0] == target_blk.id &&
                 IsValidDelaySlotCandidate(pred_blk->instructions[1])) {
        bare_jump_preds.push_back(pred_id);
      }
    }
    if (has_synthetic_ft_pred || bare_jump_preds.size() < 2) {
      continue;
    }
    std::vector<uint32_t> matching_jump_preds;
    std::optional<Instruction> shared_default_inst;
    for (size_t i = 0; i < bare_jump_preds.size(); ++i) {
      const BasicBlock* jb_i = cfg->GetBlock(bare_jump_preds[i]);
      if (jb_i == nullptr || jb_i->instructions.size() != 2) continue;
      const Instruction& cand_delay = jb_i->instructions[1];
      std::vector<uint32_t> group = {bare_jump_preds[i]};
      for (size_t j = i + 1; j < bare_jump_preds.size(); ++j) {
        const BasicBlock* jb_j = cfg->GetBlock(bare_jump_preds[j]);
        if (jb_j != nullptr && jb_j->instructions.size() == 2 &&
            SameDelayInstructionEffect(cand_delay, jb_j->instructions[1])) {
          group.push_back(bare_jump_preds[j]);
        }
      }
      if (group.size() >= 2) {
        matching_jump_preds = std::move(group);
        shared_default_inst = cand_delay;
        break;
      }
    }
    if (matching_jump_preds.size() >= 2 && shared_default_inst.has_value()) {
      const uint32_t target_id = target_blk.id;
      const uint32_t target_start_vram = target_blk.start_vram;
      if (physical_ft_pred_id.has_value()) {
        BasicBlock* ft_pred_blk = cfg->GetMutableBlock(*physical_ft_pred_id);
        if (ft_pred_blk != nullptr) {
          const uint32_t target_field = (target_start_vram >> 2) & 0x03FFFFFFu;
          Instruction j_inst;
          j_inst.vram = ft_pred_blk->end_vram;
          j_inst.opcode = Opcode::kJ;
          j_inst.target = target_field;
          j_inst.raw_word = (2u << 26) | target_field;
          ft_pred_blk->instructions.push_back(j_inst);
          ft_pred_blk->instructions.push_back(MakeNopInstruction(ft_pred_blk->end_vram));
          ft_pred_blk->outgoing_edges[0].type = EdgeType::kJump;
          for (auto& edge : cfg->MutableEdges()) {
            if (edge.from_block_id == *physical_ft_pred_id && edge.to_block_id == target_id &&
                edge.type == EdgeType::kFallthrough) {
              edge.type = EdgeType::kJump;
            }
          }
        }
      }
      const uint32_t shared_default_id =
          CreateOneInstructionFallthroughBlock(cfg, *shared_default_inst, target_id);
      if (BasicBlock* def_blk = cfg->GetMutableBlock(shared_default_id); def_blk != nullptr) {
        def_blk->end_vram = 0;
      }
      const uint32_t default_target_field = (shared_default_inst->vram >> 2) & 0x03FFFFFFu;
      for (uint32_t jmp_pred_id : matching_jump_preds) {
        RetargetBranchEdge(cfg, jmp_pred_id, target_id, shared_default_id);
        BasicBlock* mut_jmp = cfg->GetMutableBlock(jmp_pred_id);
        if (mut_jmp != nullptr && mut_jmp->instructions.size() == 2) {
          mut_jmp->instructions[0].target = default_target_field;
          mut_jmp->instructions[0].raw_word = (2u << 26) | default_target_field;
          mut_jmp->instructions[1] = MakeNopInstruction(mut_jmp->instructions[1].vram);
        }
      }
      changed = true;
    }
  }
  return changed;
}

bool TryUnscheduleBranchLikelyDelaySlot(ControlFlowGraph* cfg, BasicBlock* block,
                                        const Instruction& term, const Instruction& delay_inst) {
  uint32_t succ_taken = 0;
  uint32_t succ_fallthrough = 0;
  for (const auto& edge : block->outgoing_edges) {
    if (edge.type == EdgeType::kBranch) {
      succ_taken = edge.to_block_id;
    } else if (edge.type == EdgeType::kFallthrough) {
      succ_fallthrough = edge.to_block_id;
    }
  }
  if (succ_taken == 0 && succ_fallthrough == 0 && !block->successors.empty()) {
    succ_taken = block->successors[0];
    succ_fallthrough = block->successors.size() > 1 ? block->successors[1] : block->successors[0];
  }
  if (succ_taken != 0 && succ_taken == succ_fallthrough) {
    const Opcode non_likely_op = ToNonLikelyBranchOpcode(term.opcode);
    const auto inverted_op = InvertNonLikelyBranchOpcode(non_likely_op);
    if (inverted_op.has_value()) {
      const uint32_t block_id = block->id;
      const uint32_t ft_id = CreateOneInstructionFallthroughBlock(cfg, delay_inst, succ_taken);
      BasicBlock* mut_block = cfg->GetMutableBlock(block_id);
      if (mut_block != nullptr && mut_block->instructions.size() >= 2) {
        mut_block->instructions[mut_block->instructions.size() - 2].opcode = *inverted_op;
        mut_block->instructions.back() = MakeNopInstruction(delay_inst.vram);
        for (auto& edge : mut_block->outgoing_edges) {
          if (edge.type == EdgeType::kFallthrough) {
            edge.to_block_id = ft_id;
          }
        }
        if (mut_block->successors.size() == 1) {
          mut_block->successors.push_back(ft_id);
        } else if (mut_block->successors.size() >= 2) {
          mut_block->successors[1] = ft_id;
        }
        for (auto& edge : cfg->MutableEdges()) {
          if (edge.from_block_id == block_id && edge.type == EdgeType::kFallthrough) {
            edge.to_block_id = ft_id;
          }
        }
        if (BasicBlock* ft_blk = cfg->GetMutableBlock(ft_id); ft_blk != nullptr) {
          ft_blk->predecessors.push_back(block_id);
        }
        return true;
      }
    }
  }
  BasicBlock* taken_block = cfg->GetMutableBlock(succ_taken);
  const BasicBlock* ft_block = cfg->GetBlock(succ_fallthrough);
  const bool ft_identical = ft_block != nullptr && !ft_block->instructions.empty() &&
                            SameDelayInstructionEffect(delay_inst, ft_block->instructions.front());
  if (taken_block != nullptr && !ft_identical && succ_taken != succ_fallthrough) {
    RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
    std::optional<Register> clobber_reg = ComputeUnscheduledClobberReg(delay_ud);
    if (taken_block->predecessors.size() == 1 && taken_block->predecessors[0] == block->id &&
        succ_taken != block->id && succ_taken != cfg->EntryBlockId()) {
      const size_t insert_idx = FindThreadSunkInsertionIndex(*taken_block, term, delay_inst, true);
      taken_block->instructions.insert(
          taken_block->instructions.begin() + static_cast<std::ptrdiff_t>(insert_idx), delay_inst);
      block->instructions[block->instructions.size() - 2].opcode =
          ToNonLikelyBranchOpcode(term.opcode);
      block->instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
      return true;
    }
    const uint32_t block_id = block->id;
    const uint32_t taken_start_vram = taken_block->start_vram;
    bool has_synthetic_ft_pred = false;
    std::optional<uint32_t> call_ft_pred_id;
    std::optional<uint32_t> non_call_ft_pred_id;
    std::vector<uint32_t> peer_likely_branch_ids;
    if (succ_taken != block_id) {
      for (uint32_t pred_id : taken_block->predecessors) {
        if (pred_id == block_id) continue;
        BasicBlock* pred_blk = cfg->GetMutableBlock(pred_id);
        if (pred_blk == nullptr || pred_blk->instructions.empty()) continue;
        if (pred_blk->end_vram == 0 && pred_blk->outgoing_edges.size() == 1 &&
            pred_blk->outgoing_edges[0].type == EdgeType::kFallthrough) {
          has_synthetic_ft_pred = true;
        }
        if (pred_blk->instructions.size() >= 2 &&
            pred_blk->Terminator() == &pred_blk->instructions[pred_blk->instructions.size() - 2] &&
            pred_blk->instructions[pred_blk->instructions.size() - 2].IsBranchLikely() &&
            SameDelayInstructionEffect(delay_inst, pred_blk->instructions.back())) {
          for (const auto& edge : pred_blk->outgoing_edges) {
            if (edge.type == EdgeType::kBranch && edge.to_block_id == succ_taken) {
              peer_likely_branch_ids.push_back(pred_id);
              break;
            }
          }
        }
        if (pred_blk->end_vram != 0 &&
            (pred_blk->end_vram + 4 == taken_start_vram || pred_blk->instructions.size() == 1) &&
            pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
            pred_blk->outgoing_edges.size() == 1 &&
            pred_blk->outgoing_edges[0].type == EdgeType::kFallthrough && !pred_blk->HasBranch() &&
            !pred_blk->HasReturn() &&
            (pred_blk->Terminator() == nullptr || pred_blk->Terminator()->opcode != Opcode::kJ)) {
          if (pred_blk->instructions.size() >= 2 &&
              pred_blk->instructions[pred_blk->instructions.size() - 2].IsCall() &&
              SameDelayInstructionEffect(delay_inst, pred_blk->instructions.back())) {
            call_ft_pred_id = pred_id;
          } else {
            non_call_ft_pred_id = pred_id;
          }
        }
      }
    }
    if (has_synthetic_ft_pred ||
        (!peer_likely_branch_ids.empty() && non_call_ft_pred_id.has_value())) {
      const uint32_t jump_blk_id = CreateJumpBlockWithInstruction(cfg, delay_inst, succ_taken);
      std::vector<uint32_t> all_likely_ids = {block_id};
      if (!has_synthetic_ft_pred) {
        all_likely_ids.insert(all_likely_ids.end(), peer_likely_branch_ids.begin(),
                              peer_likely_branch_ids.end());
      }
      for (uint32_t br_id : all_likely_ids) {
        RetargetBranchEdge(cfg, br_id, succ_taken, jump_blk_id);
        BasicBlock* mut_br = cfg->GetMutableBlock(br_id);
        if (mut_br != nullptr && mut_br->instructions.size() >= 2) {
          const Instruction br_term = mut_br->instructions[mut_br->instructions.size() - 2];
          const Instruction br_delay = mut_br->instructions.back();
          mut_br->instructions[mut_br->instructions.size() - 2].opcode =
              ToNonLikelyBranchOpcode(br_term.opcode);
          mut_br->instructions.back() = MakeNopInstruction(br_delay.vram, clobber_reg);
        }
      }
      return true;
    }
    if (non_call_ft_pred_id.has_value()) {
      BasicBlock* pred_blk = cfg->GetMutableBlock(*non_call_ft_pred_id);
      if (pred_blk != nullptr) {
        const uint32_t target_field = (taken_start_vram >> 2) & 0x03FFFFFFu;
        Instruction j_inst;
        j_inst.vram = pred_blk->end_vram;
        j_inst.opcode = Opcode::kJ;
        j_inst.target = target_field;
        j_inst.raw_word = (2u << 26) | target_field;
        pred_blk->instructions.push_back(j_inst);
        pred_blk->instructions.push_back(MakeNopInstruction(pred_blk->end_vram));
        pred_blk->outgoing_edges[0].type = EdgeType::kJump;
        for (auto& edge : cfg->MutableEdges()) {
          if (edge.from_block_id == *non_call_ft_pred_id && edge.to_block_id == succ_taken &&
              edge.type == EdgeType::kFallthrough) {
            edge.type = EdgeType::kJump;
          }
        }
      }
    }
    const uint32_t header_id = CreateOneInstructionFallthroughBlock(cfg, delay_inst, succ_taken);
    RetargetBranchEdge(cfg, block_id, succ_taken, header_id);
    if (call_ft_pred_id.has_value()) {
      BasicBlock* call_pred = cfg->GetMutableBlock(*call_ft_pred_id);
      BasicBlock* old_taken = cfg->GetMutableBlock(succ_taken);
      BasicBlock* new_header = cfg->GetMutableBlock(header_id);
      if (call_pred != nullptr && old_taken != nullptr && new_header != nullptr &&
          !call_pred->instructions.empty()) {
        call_pred->instructions.back() = MakeNopInstruction(call_pred->instructions.back().vram);
        call_pred->successors[0] = header_id;
        call_pred->outgoing_edges[0].to_block_id = header_id;
        for (auto& edge : cfg->MutableEdges()) {
          if (edge.from_block_id == *call_ft_pred_id && edge.to_block_id == succ_taken &&
              edge.type == EdgeType::kFallthrough) {
            edge.to_block_id = header_id;
          }
        }
        old_taken->predecessors.erase(std::remove(old_taken->predecessors.begin(),
                                                  old_taken->predecessors.end(), *call_ft_pred_id),
                                      old_taken->predecessors.end());
        new_header->predecessors.push_back(*call_ft_pred_id);
      }
    }
    BasicBlock* mut_block = cfg->GetMutableBlock(block_id);
    if (mut_block != nullptr && mut_block->instructions.size() >= 2) {
      mut_block->instructions[mut_block->instructions.size() - 2].opcode =
          ToNonLikelyBranchOpcode(term.opcode);
      mut_block->instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
    }
    return true;
  }
  return false;
}

}  // namespace

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
  const bool returns_f0 = AbiAnalyzer::DetermineReturnsF0(*cfg);
  const bool returns_v0 = !returns_f0 && AbiAnalyzer::DetermineReturnsV0(*cfg);
  absl::flat_hash_set<uint32_t> jump_table_target_vrams;
  for (const auto& jt : cfg->JumpTables()) {
    for (const auto& entry : jt.entries) {
      if (entry.target_vram != jt.default_target_vram) {
        jump_table_target_vrams.insert(entry.target_vram);
      }
    }
  }

  auto compute_unscheduled_clobber_reg = ComputeUnscheduledClobberReg;
  auto find_thread_sunk_insertion_index = FindThreadSunkInsertionIndex;
  const bool unschedule_shared_branch_delay_slots =
      (mode_ == EagerThreadUnscheduleMode::kStage1Full);
  const bool allow_ineligible_pre_branch_sinking =
      (mode_ != EagerThreadUnscheduleMode::kPostCrossJump);

  bool any_changed = false;
  for (int iter = 0; iter < 4; ++iter) {
    const LoopInfo& loop_info = analyses->GetLoopInfo();
    const DominatorTree& dom_tree = analyses->GetDominatorTree();
    const DominatorTree& post_dom_tree = analyses->GetPostDominatorTree();
    bool changed = false;

    if (TrySinkCommonPredecessorDelaySlots(cfg, loop_info, jump_table_target_vrams, returns_v0,
                                           returns_f0)) {
      changed = true;
    }
    if (unschedule_shared_branch_delay_slots &&
        TryRestoreSharedSwitchDefaultBlocks(cfg, jump_table_target_vrams)) {
      changed = true;
    }

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

      if (term.IsBranchLikely()) {
        if (TryUnscheduleBranchLikelyDelaySlot(cfg, &block, term, delay_inst)) {
          changed = true;
          continue;
        }
      }

      // 2. Thread Sinking (`fill_slots_from_thread` inverse) for non-likely conditional branches.
      if (!term.IsBranch() || term.IsBranchLikely() || term.IsUnconditionalBranch() ||
          block.successors.size() != 2) {
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
      if (succ_taken == succ_fallthrough || succ_taken == block.id ||
          succ_fallthrough == block.id) {
        continue;
      }

      if (!IsValidDelaySlotCandidate(delay_inst)) {
        continue;
      }

      RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
      bool dead_on_taken =
          AllDefinedRegistersDeadAtBlockEntry(*cfg, succ_taken, delay_ud, returns_v0, returns_f0);
      bool dead_on_fallthrough = AllDefinedRegistersDeadAtBlockEntry(
          *cfg, succ_fallthrough, delay_ud, returns_v0, returns_f0);
      const BasicBlock* pre_taken_block = cfg->GetBlock(succ_taken);
      const bool is_chained_ft_to_same_taken =
          !dead_on_taken && !dead_on_fallthrough && pre_taken_block != nullptr &&
          pre_taken_block->predecessors.size() == 2 &&
          MatchChainedFallthroughBranchToSameTarget(*cfg, block, succ_taken, succ_fallthrough,
                                                    delay_ud, returns_v0, returns_f0)
              .has_value();
      if (is_chained_ft_to_same_taken) {
        dead_on_fallthrough = true;
      }
      // `reorg.c:3348-3360`: when `fill_slots_from_thread` stole `delay_inst` from the fallthrough
      // thread of `block` (`own_thread`), and `delay_inst` was redundant with a prior instruction
      // at the start of `succ_taken` (`taken_block`) that was stolen into `block`'s predecessor
      // branch delay slot and deleted from `succ_taken`, `delay_inst` is live along `succ_taken`'s
      // fallthrough path (`tk_ft`) as well as `succ_fallthrough`, but dead on `succ_taken`'s taken
      // branch (`tk_taken`).
      if (unschedule_shared_branch_delay_slots && !dead_on_taken && !dead_on_fallthrough &&
          block.instructions.size() == 2) {
        BasicBlock* tk_blk = cfg->GetMutableBlock(succ_taken);
        BasicBlock* ft_blk = cfg->GetMutableBlock(succ_fallthrough);
        if (tk_blk != nullptr && ft_blk != nullptr && tk_blk->predecessors.size() == 1 &&
            tk_blk->predecessors[0] == block.id && ft_blk->predecessors.size() == 1 &&
            ft_blk->predecessors[0] == block.id && tk_blk->instructions.size() == 2 &&
            tk_blk->successors.size() == 2 && ft_blk->successors.size() == 1) {
          const Instruction& tk_term = tk_blk->instructions.front();
          const Instruction& tk_delay = tk_blk->instructions.back();
          if (tk_blk->Terminator() == &tk_term && tk_term.IsBranch() &&
              !tk_term.IsUnconditionalBranch() &&
              !DelaySlotConflictsWithTerminator(tk_term, delay_inst)) {
            uint32_t tk_taken = 0;
            uint32_t tk_ft = 0;
            for (const auto& edge : tk_blk->outgoing_edges) {
              if (edge.type == EdgeType::kBranch) {
                tk_taken = edge.to_block_id;
              } else if (edge.type == EdgeType::kFallthrough) {
                tk_ft = edge.to_block_id;
              }
            }
            if (tk_taken != 0 && tk_ft != 0 && tk_delay.IsNop() &&
                AllDefinedRegistersDeadAtBlockEntry(*cfg, tk_taken, delay_ud, returns_v0,
                                                    returns_f0)) {
              BasicBlock* tk_ft_blk = cfg->GetMutableBlock(tk_ft);
              if (tk_ft_blk != nullptr && ft_blk != nullptr &&
                  tk_ft_blk->predecessors.size() == 1 && tk_ft_blk->predecessors[0] == succ_taken &&
                  tk_ft_blk->successors == ft_blk->successors) {
                ft_blk->instructions.insert(ft_blk->instructions.begin(), delay_inst);
                tk_ft_blk->instructions.insert(tk_ft_blk->instructions.begin(), delay_inst);
                if (BasicBlock* mut_b = cfg->GetMutableBlock(block.id);
                    mut_b != nullptr && mut_b->instructions.size() >= 2) {
                  mut_b->instructions.back() = MakeNopInstruction(delay_inst.vram);
                }
                changed = true;
                continue;
              }
            }
          }
        }
      }
      if (dead_on_taken == dead_on_fallthrough) {
        continue;
      }

      BasicBlock* ft_block = cfg->GetMutableBlock(succ_fallthrough);
      BasicBlock* taken_block = cfg->GetMutableBlock(succ_taken);
      const uint32_t target_succ = dead_on_taken ? succ_fallthrough : succ_taken;
      BasicBlock* target_blk = dead_on_taken ? ft_block : taken_block;
      if (target_blk == nullptr || target_succ == cfg->EntryBlockId()) {
        continue;
      }

      const size_t term_idx = block.instructions.size() - 2;
      const bool is_fpu_store_flag_diamond =
          !dead_on_taken && dead_on_fallthrough &&
          (term.opcode == Opcode::kBc1t || term.opcode == Opcode::kBc1f) &&
          delay_inst.opcode == Opcode::kAddiu && delay_inst.rs == Register::kZero &&
          delay_inst.immediate == 1 && delay_inst.rt.has_value() && ft_block != nullptr &&
          ft_block->instructions.size() == 1 && ft_block->successors.size() == 1 &&
          ft_block->successors[0] == succ_taken &&
          ft_block->instructions[0].opcode == Opcode::kAddu &&
          ft_block->instructions[0].rd == delay_inst.rt &&
          ft_block->instructions[0].rs == Register::kZero &&
          ft_block->instructions[0].rt == Register::kZero;
      if (is_fpu_store_flag_diamond) {
        continue;
      }

      const bool is_fixuns_trunc_branch =
          !dead_on_taken && dead_on_fallthrough && delay_inst.opcode == Opcode::kSubS &&
          !target_blk->instructions.empty() && target_blk->instructions[0].opcode == Opcode::kCvtWS;
      const bool is_zero_init_non_ret =
          delay_inst.opcode == Opcode::kAddu && delay_inst.rs == Register::kZero &&
          delay_inst.rt == Register::kZero && delay_inst.rd.has_value() &&
          *delay_inst.rd != Register::kV0 && term.opcode == Opcode::kBeq &&
          term.rt == Register::kZero;
      bool is_pre_branch_default_init_diamond = false;
      if (!dead_on_taken && dead_on_fallthrough && ft_block != nullptr && taken_block != nullptr &&
          (post_dom_tree.Dominates(succ_taken, succ_fallthrough) ||
           (is_zero_init_non_ret && dom_tree.Dominates(block.id, succ_fallthrough)))) {
        for (uint32_t pred_id : taken_block->predecessors) {
          if (pred_id == block.id) continue;
          const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
          if (pred_blk == nullptr || pred_blk->instructions.empty() ||
              pred_blk->end_vram + 4 != taken_block->start_vram ||
              pred_blk->successors.size() != 1 || pred_blk->successors[0] != succ_taken ||
              pred_blk->outgoing_edges.size() != 1 ||
              pred_blk->outgoing_edges[0].type != EdgeType::kFallthrough || pred_blk->HasReturn()) {
            continue;
          }
          const Instruction* last_ft_inst = nullptr;
          for (auto it = pred_blk->instructions.rbegin(); it != pred_blk->instructions.rend();
               ++it) {
            if (!it->IsNop()) {
              last_ft_inst = &*it;
              break;
            }
          }
          if (last_ft_inst == nullptr || last_ft_inst->HasDelaySlot()) continue;
          RegisterUseDef last_ud = GetInstructionUseDef(*last_ft_inst);
          const bool same_defs =
              (!delay_ud.gpr_defs.empty() && last_ud.gpr_defs == delay_ud.gpr_defs) ||
              (!delay_ud.fpr_defs.empty() && last_ud.fpr_defs == delay_ud.fpr_defs);
          if (!same_defs) continue;
          if (is_zero_init_non_ret) {
            is_pre_branch_default_init_diamond = true;
            break;
          }
          if (pred_id == succ_fallthrough && ft_block->predecessors.size() == 1 &&
              ft_block->predecessors[0] == block.id &&
              !(ft_block->instructions.size() == 1 &&
                ft_block->instructions[0].opcode == Opcode::kSra) &&
              !std::any_of(
                  ft_block->instructions.begin(), ft_block->instructions.end(),
                  [](const Instruction& inst) { return inst.IsCall() || inst.IsStore(); })) {
            is_pre_branch_default_init_diamond = true;
            break;
          }
        }
      }
      bool all_pre_branch_ineligible = allow_ineligible_pre_branch_sinking &&
                                       !is_fixuns_trunc_branch &&
                                       !is_pre_branch_default_init_diamond;
      if (all_pre_branch_ineligible) {
        for (size_t i = 0; i < term_idx; ++i) {
          if (!WasIneligibleForTerminatorDelaySlot(block, i, term_idx, term)) {
            all_pre_branch_ineligible = false;
            break;
          }
        }
      }
      if (all_pre_branch_ineligible && dead_on_taken && !dead_on_fallthrough) {
        if (block.id == cfg->EntryBlockId() && delay_inst.opcode == Opcode::kAddu &&
            delay_inst.rt == Register::kZero && delay_inst.rd.has_value() &&
            *delay_inst.rd >= Register::kS0 && *delay_inst.rd <= Register::kS7 &&
            delay_inst.rs.has_value() && *delay_inst.rs >= Register::kA0 &&
            *delay_inst.rs <= Register::kA3) {
          all_pre_branch_ineligible = false;
        } else if (term_idx == 0 && !DelaySlotConflictsWithTerminator(term, delay_inst) &&
                   ((delay_inst.opcode == Opcode::kAddu && delay_inst.rt == Register::kZero) ||
                    (delay_inst.opcode == Opcode::kAddiu && delay_inst.rs == Register::kZero))) {
          const Loop* ft_loop = loop_info.GetLoopFor(succ_fallthrough);
          const Loop* next_ft_loop = (ft_block != nullptr && ft_block->successors.size() == 1)
                                         ? loop_info.GetLoopFor(ft_block->successors[0])
                                         : ft_loop;
          const bool enters_loop = (ft_loop != nullptr && !ft_loop->Contains(block.id)) ||
                                   (next_ft_loop != nullptr && !next_ft_loop->Contains(block.id));
          if (enters_loop) {
            all_pre_branch_ineligible = false;
          }
        }
      }

      const bool has_thread_steal_conflict =
          all_pre_branch_ineligible || DelaySlotConflictsWithTerminator(term, delay_inst) ||
          DelaySlotHasPreBranchDependenceConflict(block, term, delay_inst) ||
          DelaySlotFeedsSuccessorBranch(*target_blk, delay_inst);

      if ((target_blk->predecessors.size() == 1 && target_blk->predecessors[0] == block.id) ||
          is_chained_ft_to_same_taken) {
        if (has_thread_steal_conflict) {
          std::optional<Register> clobber_reg = compute_unscheduled_clobber_reg(delay_ud);
          const size_t insert_idx =
              find_thread_sunk_insertion_index(*target_blk, term, delay_inst, !dead_on_taken);
          target_blk->instructions.insert(
              target_blk->instructions.begin() + static_cast<std::ptrdiff_t>(insert_idx),
              delay_inst);
          block.instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
          changed = true;
          continue;
        }
      } else if (unschedule_shared_branch_delay_slots && dead_on_taken && !dead_on_fallthrough &&
                 ft_block != nullptr && ft_block->predecessors.size() >= 2 &&
                 has_thread_steal_conflict) {
        const uint32_t block_id = block.id;
        std::optional<Register> clobber_reg = compute_unscheduled_clobber_reg(delay_ud);
        struct PeerTakenBranchInfo {
          uint32_t block_id;
          bool clear_delay_slot;
        };
        std::vector<PeerTakenBranchInfo> peer_taken_branches;
        for (uint32_t pred_id : ft_block->predecessors) {
          if (pred_id == block_id) continue;
          const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
          if (pred_blk == nullptr || pred_blk->instructions.size() < 2) continue;
          const Instruction* pterm = pred_blk->Terminator();
          if (pterm == nullptr ||
              pterm != &pred_blk->instructions[pred_blk->instructions.size() - 2] ||
              !pterm->IsBranch() || pterm->IsUnconditionalBranch() ||
              pred_blk->successors.size() != 2 ||
              !SameDelayInstructionEffect(delay_inst, pred_blk->instructions.back())) {
            continue;
          }
          uint32_t p_taken = 0;
          uint32_t p_ft = 0;
          for (const auto& edge : pred_blk->outgoing_edges) {
            if (edge.type == EdgeType::kBranch) {
              p_taken = edge.to_block_id;
            } else if (edge.type == EdgeType::kFallthrough) {
              p_ft = edge.to_block_id;
            }
          }
          if (p_taken == succ_fallthrough) {
            const bool clear_delay =
                pterm->IsBranchLikely() ||
                AllDefinedRegistersDeadAtBlockEntry(*cfg, p_ft, delay_ud, returns_v0, returns_f0);
            peer_taken_branches.push_back({pred_id, clear_delay});
          }
        }
        const uint32_t ft_header_id =
            CreateOneInstructionFallthroughBlock(cfg, delay_inst, succ_fallthrough);
        BasicBlock* mut_block = cfg->GetMutableBlock(block_id);
        BasicBlock* mut_ft = cfg->GetMutableBlock(succ_fallthrough);
        BasicBlock* mut_header = cfg->GetMutableBlock(ft_header_id);
        if (mut_block != nullptr && mut_ft != nullptr && mut_header != nullptr &&
            mut_block->instructions.size() >= 2) {
          mut_block->instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
          for (size_t i = 0; i < mut_block->outgoing_edges.size(); ++i) {
            if (mut_block->outgoing_edges[i].type == EdgeType::kFallthrough &&
                mut_block->outgoing_edges[i].to_block_id == succ_fallthrough) {
              mut_block->outgoing_edges[i].to_block_id = ft_header_id;
              if (i < mut_block->successors.size()) {
                mut_block->successors[i] = ft_header_id;
              }
            }
          }
          for (auto& edge : cfg->MutableEdges()) {
            if (edge.from_block_id == block_id && edge.to_block_id == succ_fallthrough &&
                edge.type == EdgeType::kFallthrough) {
              edge.to_block_id = ft_header_id;
            }
          }
          mut_ft->predecessors.erase(
              std::remove(mut_ft->predecessors.begin(), mut_ft->predecessors.end(), block_id),
              mut_ft->predecessors.end());
          mut_header->predecessors.push_back(block_id);
          for (const auto& peer : peer_taken_branches) {
            RetargetBranchEdge(cfg, peer.block_id, succ_fallthrough, ft_header_id);
            if (peer.clear_delay_slot) {
              if (BasicBlock* mut_peer = cfg->GetMutableBlock(peer.block_id);
                  mut_peer != nullptr && mut_peer->instructions.size() >= 2) {
                const Instruction pterm = mut_peer->instructions[mut_peer->instructions.size() - 2];
                const Instruction pdelay = mut_peer->instructions.back();
                mut_peer->instructions[mut_peer->instructions.size() - 2].opcode =
                    ToNonLikelyBranchOpcode(pterm.opcode);
                mut_peer->instructions.back() = MakeNopInstruction(pdelay.vram, clobber_reg);
              }
            }
          }
          changed = true;
          continue;
        }
      } else if (unschedule_shared_branch_delay_slots && !dead_on_taken && dead_on_fallthrough &&
                 ft_block != nullptr && taken_block != nullptr &&
                 taken_block->predecessors.size() >= 2 && has_thread_steal_conflict) {
        const uint32_t block_id = block.id;
        const uint32_t taken_start_vram = taken_block->start_vram;
        const Loop* block_loop = loop_info.GetLoopFor(block_id);
        const Loop* taken_loop = loop_info.GetLoopFor(succ_taken);
        const Loop* ft_loop = loop_info.GetLoopFor(succ_fallthrough);
        const bool ft_is_uncond_jump =
            ft_block->instructions.size() >= 2 &&
            ft_block->Terminator() == &ft_block->instructions[ft_block->instructions.size() - 2] &&
            ft_block->Terminator()->opcode == Opcode::kJ;
        if (ft_is_uncond_jump ||
            (block_loop != nullptr && ft_loop == block_loop && taken_loop != block_loop &&
             succ_taken != block_loop->header &&
             (!IsPureEpilogueReturnBlock(*taken_block) || taken_block->predecessors.size() >= 4))) {
          std::optional<Register> clobber_reg = compute_unscheduled_clobber_reg(delay_ud);
          const uint32_t jump_blk_id = CreateJumpBlockWithInstruction(cfg, delay_inst, succ_taken);
          RetargetBranchEdge(cfg, block_id, succ_taken, jump_blk_id);
          if (BasicBlock* mut_block = cfg->GetMutableBlock(block_id);
              mut_block != nullptr && mut_block->instructions.size() >= 2) {
            mut_block->instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
          }
          changed = true;
          continue;
        }
        std::optional<uint32_t> ft_pred_of_taken_id;
        std::vector<uint32_t> peer_branches_to_taken;
        for (uint32_t pred_id : taken_block->predecessors) {
          if (pred_id == block_id) continue;
          const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
          if (pred_blk != nullptr && !pred_blk->instructions.empty() &&
              pred_blk->end_vram + 4 == taken_start_vram && pred_blk->successors.size() == 1 &&
              pred_blk->successors[0] == succ_taken && pred_blk->outgoing_edges.size() == 1 &&
              pred_blk->outgoing_edges[0].type == EdgeType::kFallthrough &&
              !pred_blk->HasBranch() && !pred_blk->HasReturn() &&
              (pred_blk->Terminator() == nullptr || pred_blk->Terminator()->opcode != Opcode::kJ)) {
            ft_pred_of_taken_id = pred_id;
          }
          if (pred_blk != nullptr && pred_blk->instructions.size() >= 2 &&
              pred_blk->successors.size() == 2 &&
              (pred_blk->instructions.back().IsNop() ||
               SameDelayInstructionEffect(delay_inst, pred_blk->instructions.back()))) {
            for (const auto& edge : pred_blk->outgoing_edges) {
              if (edge.type == EdgeType::kBranch && edge.to_block_id == succ_taken) {
                peer_branches_to_taken.push_back(pred_id);
                break;
              }
            }
          }
        }
        const bool is_slti_bne_diamond =
            term.opcode == Opcode::kBne && block.instructions.size() >= 3 &&
            block.instructions[block.instructions.size() - 3].opcode == Opcode::kSlti &&
            block.instructions[block.instructions.size() - 3].immediate > 1 &&
            ft_block->instructions.size() == 1 && ft_block->successors.size() == 1 &&
            ft_block->successors[0] == succ_taken;
        const BasicBlock* ft_pred_blk_ptr =
            ft_pred_of_taken_id.has_value() ? cfg->GetBlock(*ft_pred_of_taken_id) : nullptr;
        const bool allow_call_shared_target_header =
            !IsPureEpilogueReturnBlock(*taken_block) || !peer_branches_to_taken.empty();
        const bool is_non_inverted_shared_target_header =
            ft_pred_of_taken_id.has_value() && ft_pred_blk_ptr != nullptr &&
            (is_slti_bne_diamond || jump_table_target_vrams.contains(ft_pred_blk_ptr->start_vram) ||
             ft_pred_blk_ptr->predecessors.size() >= 2 ||
             (allow_call_shared_target_header && ft_pred_blk_ptr->instructions.size() >= 2 &&
              ft_pred_blk_ptr->instructions[ft_pred_blk_ptr->instructions.size() - 2].IsCall()) ||
             (*ft_pred_of_taken_id == succ_fallthrough &&
              (ft_block->predecessors.size() != 1 || ft_block->predecessors[0] != block_id ||
               (ft_block->instructions.size() > 2 &&
                (allow_call_shared_target_header ||
                 !ft_block->instructions[ft_block->instructions.size() - 2].IsCall())) ||
               std::any_of(ft_block->instructions.begin(), ft_block->instructions.end(),
                           [&](const Instruction& inst) {
                             return inst.IsStore() ||
                                    (allow_call_shared_target_header && inst.IsCall());
                           }))));
        if (is_non_inverted_shared_target_header) {
          BasicBlock* pred_blk = cfg->GetMutableBlock(*ft_pred_of_taken_id);
          if (pred_blk != nullptr) {
            const uint32_t pred_end_vram = pred_blk->end_vram;
            const uint32_t target_field = (taken_start_vram >> 2) & 0x03FFFFFFu;
            Instruction j_inst;
            j_inst.vram = pred_end_vram;
            j_inst.opcode = Opcode::kJ;
            j_inst.target = target_field;
            j_inst.raw_word = (2u << 26) | target_field;
            const bool pred_ends_with_call =
                pred_blk->instructions.size() >= 2 &&
                pred_blk->instructions[pred_blk->instructions.size() - 2].IsCall();
            if (pred_ends_with_call) {
              const uint32_t jmp_blk_id = static_cast<uint32_t>(cfg->Blocks().size());
              BasicBlock jmp_blk;
              jmp_blk.id = jmp_blk_id;
              jmp_blk.start_vram = pred_end_vram;
              jmp_blk.end_vram = pred_end_vram;
              jmp_blk.instructions.push_back(j_inst);
              jmp_blk.instructions.push_back(MakeNopInstruction(pred_end_vram));
              CfgEdge jmp_edge;
              jmp_edge.from_block_id = jmp_blk_id;
              jmp_edge.to_block_id = succ_taken;
              jmp_edge.type = EdgeType::kJump;
              jmp_blk.outgoing_edges.push_back(jmp_edge);
              jmp_blk.successors.push_back(succ_taken);
              jmp_blk.predecessors.push_back(*ft_pred_of_taken_id);
              cfg->MutableEdges().push_back(jmp_edge);
              cfg->MutableBlocks().push_back(std::move(jmp_blk));
              BasicBlock* mut_pred = cfg->GetMutableBlock(*ft_pred_of_taken_id);
              BasicBlock* mut_taken = cfg->GetMutableBlock(succ_taken);
              if (mut_pred != nullptr && mut_taken != nullptr) {
                mut_pred->successors[0] = jmp_blk_id;
                mut_pred->outgoing_edges[0].to_block_id = jmp_blk_id;
                for (auto& edge : cfg->MutableEdges()) {
                  if (edge.from_block_id == *ft_pred_of_taken_id &&
                      edge.to_block_id == succ_taken && edge.type == EdgeType::kFallthrough) {
                    edge.to_block_id = jmp_blk_id;
                  }
                }
                for (uint32_t& p : mut_taken->predecessors) {
                  if (p == *ft_pred_of_taken_id) {
                    p = jmp_blk_id;
                  }
                }
              }
            } else {
              pred_blk->instructions.push_back(j_inst);
              pred_blk->instructions.push_back(MakeNopInstruction(pred_end_vram));
              pred_blk->outgoing_edges[0].type = EdgeType::kJump;
              for (auto& edge : cfg->MutableEdges()) {
                if (edge.from_block_id == *ft_pred_of_taken_id && edge.to_block_id == succ_taken &&
                    edge.type == EdgeType::kFallthrough) {
                  edge.type = EdgeType::kJump;
                }
              }
            }
          }
          std::optional<Register> clobber_reg = compute_unscheduled_clobber_reg(delay_ud);
          const uint32_t header_id =
              CreateOneInstructionFallthroughBlock(cfg, delay_inst, succ_taken);
          RetargetBranchEdge(cfg, block_id, succ_taken, header_id);
          for (uint32_t peer_id : peer_branches_to_taken) {
            RetargetBranchEdge(cfg, peer_id, succ_taken, header_id);
            if (BasicBlock* mut_peer = cfg->GetMutableBlock(peer_id);
                mut_peer != nullptr && mut_peer->instructions.size() >= 2 &&
                !mut_peer->instructions.back().IsNop()) {
              const Instruction pterm = mut_peer->instructions[mut_peer->instructions.size() - 2];
              const Instruction pdelay = mut_peer->instructions.back();
              mut_peer->instructions[mut_peer->instructions.size() - 2].opcode =
                  ToNonLikelyBranchOpcode(pterm.opcode);
              mut_peer->instructions.back() = MakeNopInstruction(pdelay.vram, clobber_reg);
            }
          }
          if (BasicBlock* mut_block = cfg->GetMutableBlock(block_id);
              mut_block != nullptr && mut_block->instructions.size() >= 2) {
            mut_block->instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
          }
          changed = true;
          continue;
        }
        if (ft_block->predecessors.size() == 1 && ft_block->predecessors[0] == block_id &&
            taken_block->start_vram > ft_block->start_vram &&
            post_dom_tree.Dominates(succ_taken, succ_fallthrough)) {
          const auto inverted_op = InvertNonLikelyBranchOpcode(term.opcode);
          if (inverted_op.has_value()) {
            if (InvertBranchOverJumpBlock(cfg, block_id, *inverted_op, delay_inst, succ_taken,
                                          succ_fallthrough)) {
              changed = true;
              continue;
            }
          }
        }
      }
    }
    if (!changed) {
      break;
    }
    analyses->InvalidateAll();
    any_changed = true;
  }
  if (any_changed) {
    analyses->InvalidateAll();
  }
  return any_changed;
}

bool CrossJumpTailDuplicatorPass::Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return false;
  }
  // GCC 2.7.2 (`toplev.c:3025`) only runs `.jump2` cross-jumping when `optimize > 0`.
  // Functions that establish a frame pointer (`$fp = $sp`) are compiled at `-O0` and never
  // undergo `.jump2` cross-jumping.
  for (const auto& blk : cfg->Blocks()) {
    for (const auto& inst : blk.instructions) {
      if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd ||
           inst.opcode == Opcode::kDaddu || inst.opcode == Opcode::kDadd) &&
          inst.rd == Register::kFp &&
          ((inst.rs == Register::kSp && inst.rt == Register::kZero) ||
           (inst.rs == Register::kZero && inst.rt == Register::kSp))) {
        return false;
      }
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
           inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
          inst.rt == Register::kFp && inst.rs == Register::kSp && inst.immediate == 0) {
        return false;
      }
    }
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
    for (const auto& entry : jt.entries) {
      if (entry.target_vram != jt.default_target_vram) {
        jump_table_target_vrams.insert(entry.target_vram);
      }
    }
  }

  absl::flat_hash_set<uint32_t> inlined_cond_branch_preds;
  bool any_changed = false;
  for (int iter = 0; iter < 64; ++iter) {
    const LoopInfo& loop_info = analyses->GetLoopInfo();
    const DominatorTree& dom_tree = analyses->GetDominatorTree();
    const DominatorTree& post_dom_tree = analyses->GetPostDominatorTree();
    bool iter_changed = false;

    absl::flat_hash_set<uint32_t> switch_decision_blocks;
    absl::flat_hash_set<uint32_t> short_circuit_blocks;
    for (const auto& blk : cfg->Blocks()) {
      if (blk.jump_table != nullptr) {
        switch_decision_blocks.insert(blk.id);
        for (uint32_t pred_id : blk.predecessors) {
          const auto* pred_blk = cfg->GetBlock(pred_id);
          if (pred_blk != nullptr && pred_blk->successors.size() == 2) {
            switch_decision_blocks.insert(pred_id);
          }
        }
      }
      if (blk.successors.size() != 2) {
        continue;
      }
      if (auto dt = DetectDecisionTreeSwitch(*cfg, &loop_info, {}, blk.id); dt.has_value()) {
        switch_decision_blocks.insert(dt->decision_blocks.begin(), dt->decision_blocks.end());
      }
      uint32_t succ_taken = 0;
      uint32_t succ_ft = 0;
      for (const auto& edge : blk.outgoing_edges) {
        if (edge.type == EdgeType::kBranch) {
          succ_taken = edge.to_block_id;
        } else if (edge.type == EdgeType::kFallthrough) {
          succ_ft = edge.to_block_id;
        }
      }
      if ((succ_taken != 0 || succ_ft != 0) && !inlined_cond_branch_preds.contains(blk.id)) {
        std::optional<uint32_t> join = post_dom_tree.ImmediateDominator(blk.id);
        auto sc = TryCollapseShortCircuitChain(*cfg, {}, blk.id, succ_taken, succ_ft, join);
        if (!sc.has_value() && !join.has_value()) {
          sc = TryCollapseShortCircuitChain(*cfg, {}, blk.id, succ_taken, succ_ft, 0xFFFFFFFDu);
        }
        if (sc.has_value()) {
          bool uses_inlined_pred = false;
          for (uint32_t add_id : sc->additional_condition_blocks) {
            if (inlined_cond_branch_preds.contains(add_id)) {
              uses_inlined_pred = true;
              break;
            }
          }
          if (!uses_inlined_pred) {
            short_circuit_blocks.insert(blk.id);
            short_circuit_blocks.insert(sc->additional_condition_blocks.begin(),
                                        sc->additional_condition_blocks.end());
          }
        }
      }
    }

    const size_t initial_block_count = cfg->Blocks().size();
    for (size_t b = 0; b < initial_block_count; ++b) {
      const BasicBlock& tail_blk = cfg->Blocks()[b];
      const uint32_t tail_block_id = tail_blk.id;
      const uint32_t tail_start_vram = tail_blk.start_vram;
      const uint32_t tail_end_vram = tail_blk.end_vram;
      const Loop* tail_loop = loop_info.GetLoopFor(tail_block_id);
      if (tail_block_id == cfg->EntryBlockId() || tail_blk.predecessors.size() < 2 ||
          tail_blk.successors.size() > 2 || tail_blk.HasSwitch() ||
          jump_table_target_vrams.contains(tail_start_vram) ||
          (tail_loop != nullptr &&
           (tail_loop->header == tail_block_id || tail_loop->IsLatch(tail_block_id)))) {
        continue;
      }

      const Instruction* tail_term = tail_blk.Terminator();
      const bool is_jump_tail =
          tail_blk.successors.size() == 1 && tail_term != nullptr &&
          tail_term->opcode == Opcode::kJ && tail_blk.instructions.size() >= 2 &&
          tail_term == &tail_blk.instructions[tail_blk.instructions.size() - 2] &&
          !jump_table_bounds_vrams.contains(tail_term->vram);
      const bool is_fallthrough_tail =
          tail_blk.successors.size() == 1 &&
          (tail_term == nullptr || (!tail_term->IsBranch() && !tail_term->IsJump())) &&
          tail_blk.outgoing_edges.size() == 1 &&
          tail_blk.outgoing_edges[0].type == EdgeType::kFallthrough;
      const bool is_return_tail =
          tail_blk.successors.empty() && tail_term != nullptr && tail_term->IsReturn() &&
          tail_blk.instructions.size() >= 2 &&
          tail_term == &tail_blk.instructions[tail_blk.instructions.size() - 2];
      uint32_t cond_taken_succ = 0;
      uint32_t cond_ft_succ = 0;
      if (tail_blk.successors.size() == 2) {
        for (const auto& edge : tail_blk.outgoing_edges) {
          if (edge.type == EdgeType::kBranch) {
            cond_taken_succ = edge.to_block_id;
          } else if (edge.type == EdgeType::kFallthrough) {
            cond_ft_succ = edge.to_block_id;
          }
        }
      }
      const bool is_cond_branch_tail =
          tail_blk.successors.size() == 2 && tail_blk.instructions.size() == 2 &&
          tail_term != nullptr && tail_term == &tail_blk.instructions[0] && tail_term->IsBranch() &&
          !tail_term->IsUnconditionalBranch() && !tail_term->IsBranchLikely() &&
          tail_blk.instructions[1].IsNop() &&
          InvertNonLikelyBranchOpcode(tail_term->opcode).has_value() &&
          !switch_decision_blocks.contains(tail_block_id) &&
          !short_circuit_blocks.contains(tail_block_id) && cond_taken_succ != cond_ft_succ &&
          cond_taken_succ != tail_block_id && cond_ft_succ != tail_block_id &&
          (tail_loop == nullptr ||
           (cond_taken_succ != tail_loop->header && cond_ft_succ != tail_loop->header));
      if (!is_jump_tail && !is_fallthrough_tail && !is_return_tail && !is_cond_branch_tail) {
        continue;
      }

      const size_t pre_term_count =
          is_fallthrough_tail ? tail_blk.instructions.size() : (tail_blk.instructions.size() - 2);
      const bool has_non_nop_tail_delay =
          !is_fallthrough_tail && !is_cond_branch_tail && !tail_blk.instructions.back().IsNop();
      const size_t effective_body_count = pre_term_count + (has_non_nop_tail_delay ? 1 : 0);
      if (!is_cond_branch_tail && effective_body_count == 0) {
        continue;
      }

      uint32_t merge_block_id = 0;
      uint32_t new_target_field = 0;
      const BasicBlock* merge_blk = nullptr;
      if (!is_return_tail && !is_cond_branch_tail) {
        merge_block_id = tail_blk.successors[0];
        if (merge_block_id == tail_block_id) {
          continue;
        }
        const Loop* merge_loop = loop_info.GetLoopFor(merge_block_id);
        if (tail_loop != nullptr && merge_block_id == tail_loop->header) {
          continue;
        }
        if (merge_loop != nullptr && merge_loop != tail_loop &&
            (tail_loop == nullptr || !merge_loop->Contains(tail_block_id))) {
          continue;
        }
        merge_blk = cfg->GetBlock(merge_block_id);
        if (merge_blk == nullptr) {
          continue;
        }
        new_target_field =
            is_jump_tail ? tail_term->target : ((merge_blk->start_vram >> 2) & 0x03FFFFFF);
      }

      const bool merges_into_return =
          is_return_tail || (merge_blk != nullptr && merge_blk->HasReturn());
      auto is_valid_body_inst = [&](const Instruction& inst) {
        if (inst.IsBranch() || inst.IsJump()) {
          return false;
        }
        if (has_non_nop_tail_delay && inst.IsCall()) {
          return false;
        }
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register def_reg : ud.gpr_defs) {
          if (def_reg == Register::kSp || def_reg == Register::kRa) {
            return false;
          }
        }
        if (merges_into_return && inst.IsLoad() &&
            (inst.rs == Register::kSp || inst.rs == Register::kFp)) {
          for (Register def_reg : ud.gpr_defs) {
            if ((def_reg >= Register::kS0 && def_reg <= Register::kS7) ||
                def_reg == Register::kFp) {
              return false;
            }
          }
          for (FpRegister fdef : ud.fpr_defs) {
            if (fdef >= FpRegister::kF20 && fdef <= FpRegister::kF31) {
              return false;
            }
          }
        }
        return true;
      };

      bool valid_tail_body = true;
      for (size_t i = 0; i < pre_term_count; ++i) {
        if (!is_valid_body_inst(tail_blk.instructions[i])) {
          valid_tail_body = false;
          break;
        }
      }
      if (valid_tail_body && has_non_nop_tail_delay) {
        const Instruction& tail_delay = tail_blk.instructions.back();
        if (tail_delay.IsCall() || tail_delay.IsReturn() || !is_valid_body_inst(tail_delay)) {
          valid_tail_body = false;
        }
      }
      if (!valid_tail_body) {
        continue;
      }

      std::optional<uint32_t> idom = dom_tree.ImmediateDominator(tail_block_id);
      if (!idom.has_value()) {
        continue;
      }
      uint32_t post_dom_anchor = *idom;
      if (const BasicBlock* idom_blk = cfg->GetBlock(*idom); idom_blk != nullptr &&
                                                             idom_blk->jump_table != nullptr &&
                                                             idom_blk->predecessors.size() == 1) {
        const uint32_t bounds_pred = idom_blk->predecessors[0];
        if (const BasicBlock* bounds_blk = cfg->GetBlock(bounds_pred);
            bounds_blk != nullptr && bounds_blk->successors.size() == 2) {
          post_dom_anchor = bounds_pred;
        }
      }
      const bool tail_post_dominates_idom = post_dom_tree.Dominates(tail_block_id, post_dom_anchor);
      if (tail_post_dominates_idom) {
        const bool is_single_pred_jump_merge =
            is_jump_tail && merge_blk != nullptr && merge_blk->predecessors.size() == 1;
        if (!is_single_pred_jump_merge) {
          continue;
        }
      }

      auto get_idom_subtree = [&](uint32_t block_id) -> std::optional<uint32_t> {
        if (!dom_tree.Dominates(*idom, block_id)) {
          return std::nullopt;
        }
        if (block_id == *idom) {
          return *idom;
        }
        uint32_t curr = block_id;
        while (true) {
          std::optional<uint32_t> parent = dom_tree.ImmediateDominator(curr);
          if (!parent.has_value()) {
            return std::nullopt;
          }
          if (*parent == *idom) {
            return curr;
          }
          curr = *parent;
        }
      };

      struct SubtreePredGroup {
        uint32_t subtree_id = 0;
        std::vector<uint32_t> jump_preds;
        std::vector<uint32_t> branch_preds;
      };
      std::vector<uint32_t> subtree_order;
      absl::flat_hash_map<uint32_t, SubtreePredGroup> subtree_groups;
      absl::flat_hash_set<uint32_t> kept_subtrees;
      absl::flat_hash_set<uint32_t> seen_preds;
      bool has_fallthrough_pred = false;
      bool has_inlined_cond_kept_pred = false;
      bool has_kept_pred = false;

      for (uint32_t pred_id : tail_blk.predecessors) {
        if (!seen_preds.insert(pred_id).second) {
          continue;
        }
        const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
        if (pred_blk == nullptr) {
          continue;
        }
        std::optional<uint32_t> sub_opt = get_idom_subtree(pred_id);
        bool pred_has_fallthrough = false;
        bool pred_has_branch_to_tail = false;
        uint32_t pred_ft_succ = 0;
        for (const auto& edge : pred_blk->outgoing_edges) {
          if (edge.to_block_id == tail_block_id && edge.type == EdgeType::kFallthrough) {
            pred_has_fallthrough = true;
          } else if (edge.to_block_id == tail_block_id && edge.type == EdgeType::kBranch) {
            pred_has_branch_to_tail = true;
          } else if (edge.type == EdgeType::kFallthrough) {
            pred_ft_succ = edge.to_block_id;
          }
        }
        if (pred_has_fallthrough) {
          has_fallthrough_pred = true;
          has_kept_pred = true;
          if (sub_opt.has_value()) {
            kept_subtrees.insert(*sub_opt);
          }
          continue;
        }
        if (inlined_cond_branch_preds.contains(pred_id) &&
            std::find(tail_blk.predecessors.begin(), tail_blk.predecessors.end(), *idom) !=
                tail_blk.predecessors.end()) {
          has_inlined_cond_kept_pred = true;
          has_kept_pred = true;
          if (sub_opt.has_value()) {
            kept_subtrees.insert(*sub_opt);
          }
          continue;
        }
        if (!sub_opt.has_value() || pred_id == tail_block_id ||
            loop_info.GetLoopFor(pred_id) != tail_loop || pred_blk->instructions.size() < 2) {
          has_kept_pred = true;
          if (sub_opt.has_value()) {
            kept_subtrees.insert(*sub_opt);
          }
          continue;
        }

        const Instruction* pred_term = pred_blk->Terminator();
        const Instruction& pred_delay = pred_blk->instructions.back();
        const bool valid_pred_delay =
            pred_delay.IsNop() || (!pred_delay.IsBranch() && !pred_delay.IsJump() &&
                                   !pred_delay.IsCall() && !pred_delay.IsReturn());
        const bool is_valid_jump_pred =
            pred_blk->successors.size() == 1 && pred_blk->successors[0] == tail_block_id &&
            pred_term != nullptr && pred_term->opcode == Opcode::kJ &&
            pred_term == &pred_blk->instructions[pred_blk->instructions.size() - 2] &&
            !jump_table_bounds_vrams.contains(pred_term->vram) && valid_pred_delay;
        const bool pred_ft_is_tail_pred =
            std::find(tail_blk.predecessors.begin(), tail_blk.predecessors.end(), pred_ft_succ) !=
            tail_blk.predecessors.end();
        const BasicBlock* pred_ft_blk = cfg->GetBlock(pred_ft_succ);
        bool pred_ft_has_call_or_store = false;
        if (pred_ft_blk != nullptr) {
          for (const auto& inst : pred_ft_blk->instructions) {
            if (inst.IsCall() || inst.IsStore()) {
              pred_ft_has_call_or_store = true;
              break;
            }
          }
        }
        bool is_nested_if_sharing_tail = false;
        if (!short_circuit_blocks.contains(pred_id)) {
          uint32_t cur_ft = pred_ft_succ;
          for (int depth = 0; depth < 4 && cur_ft != 0 && cur_ft != tail_block_id; ++depth) {
            const BasicBlock* cur_ft_blk = cfg->GetBlock(cur_ft);
            if (cur_ft_blk == nullptr || cur_ft_blk->predecessors.size() != 1 ||
                cur_ft_blk->successors.size() != 2) {
              break;
            }
            if (inlined_cond_branch_preds.contains(cur_ft) &&
                std::find(tail_blk.predecessors.begin(), tail_blk.predecessors.end(), cur_ft) !=
                    tail_blk.predecessors.end()) {
              is_nested_if_sharing_tail = true;
              break;
            }
            uint32_t next_ft = 0;
            for (const auto& e : cur_ft_blk->outgoing_edges) {
              if (e.type == EdgeType::kFallthrough) {
                next_ft = e.to_block_id;
                break;
              }
            }
            cur_ft = next_ft;
          }
        }
        const bool allow_idom_branch_pred =
            !merges_into_return &&
            ((!pred_ft_is_tail_pred && pred_ft_has_call_or_store) || is_nested_if_sharing_tail);
        const bool is_valid_branch_pred =
            !is_cond_branch_tail && !tail_post_dominates_idom &&
            (*sub_opt != *idom || allow_idom_branch_pred) && pred_blk->successors.size() == 2 &&
            pred_has_branch_to_tail && pred_ft_succ != tail_block_id &&
            loop_info.GetLoopFor(pred_ft_succ) == tail_loop && pred_term != nullptr &&
            pred_term->IsBranch() && !pred_term->IsUnconditionalBranch() &&
            pred_term == &pred_blk->instructions[pred_blk->instructions.size() - 2] &&
            !jump_table_bounds_vrams.contains(pred_term->vram) && valid_pred_delay &&
            !switch_decision_blocks.contains(pred_id) && !short_circuit_blocks.contains(pred_id);

        if (is_valid_jump_pred) {
          if (!subtree_groups.contains(*sub_opt)) {
            subtree_order.push_back(*sub_opt);
            subtree_groups[*sub_opt].subtree_id = *sub_opt;
          }
          subtree_groups[*sub_opt].jump_preds.push_back(pred_id);
        } else if (is_valid_branch_pred) {
          if (!subtree_groups.contains(*sub_opt)) {
            subtree_order.push_back(*sub_opt);
            subtree_groups[*sub_opt].subtree_id = *sub_opt;
          }
          subtree_groups[*sub_opt].branch_preds.push_back(pred_id);
        } else {
          has_kept_pred = true;
          kept_subtrees.insert(*sub_opt);
        }
      }

      if ((is_fallthrough_tail || is_return_tail || is_cond_branch_tail) && !has_fallthrough_pred &&
          !(is_fallthrough_tail && has_inlined_cond_kept_pred)) {
        continue;
      }

      bool tail_has_store_or_call = false;
      for (size_t i = 0; i < pre_term_count; ++i) {
        if (tail_blk.instructions[i].IsStore() || tail_blk.instructions[i].IsCall()) {
          tail_has_store_or_call = true;
          break;
        }
      }
      if (has_non_nop_tail_delay &&
          (tail_blk.instructions.back().IsStore() || tail_blk.instructions.back().IsCall())) {
        tail_has_store_or_call = true;
      }
      const bool is_post_switch_compute_tail =
          is_fallthrough_tail && !tail_has_store_or_call && merge_blk != nullptr &&
          IsPureEpilogueReturnBlock(*merge_blk) && switch_decision_blocks.contains(*idom);

      std::vector<uint32_t> cand_subtrees;
      for (uint32_t sub_id : subtree_order) {
        if (kept_subtrees.contains(sub_id)) {
          has_kept_pred = true;
          continue;
        }
        const auto& grp = subtree_groups[sub_id];
        if (!grp.branch_preds.empty() && !has_fallthrough_pred && !has_inlined_cond_kept_pred) {
          has_kept_pred = true;
          continue;
        }
        if (is_cond_branch_tail && (!grp.branch_preds.empty() || grp.jump_preds.size() != 1)) {
          has_kept_pred = true;
          continue;
        }
        if (is_post_switch_compute_tail && grp.branch_preds.empty()) {
          has_kept_pred = true;
          continue;
        }
        cand_subtrees.push_back(sub_id);
      }

      if (!has_kept_pred && !cand_subtrees.empty()) {
        cand_subtrees.pop_back();
      }
      if (cand_subtrees.empty()) {
        continue;
      }

      const std::vector<Instruction> tail_all_insts = tail_blk.instructions;
      const std::vector<Instruction> tail_body_insts(
          tail_all_insts.begin(), tail_all_insts.begin() + static_cast<ptrdiff_t>(pre_term_count));

      for (uint32_t sub_id : cand_subtrees) {
        const auto& grp = subtree_groups[sub_id];
        if (grp.branch_preds.empty() && grp.jump_preds.size() == 1) {
          uint32_t pred_id = grp.jump_preds[0];
          BasicBlock* pred_blk = cfg->GetMutableBlock(pred_id);
          if (pred_blk == nullptr || pred_blk->instructions.size() < 2) {
            continue;
          }
          if (is_cond_branch_tail) {
            const Instruction pred_delay = pred_blk->instructions.back();
            Instruction inv_branch = tail_all_insts[0];
            inv_branch.opcode = *InvertNonLikelyBranchOpcode(inv_branch.opcode);
            if (const BasicBlock* ft_blk = cfg->GetBlock(cond_ft_succ); ft_blk != nullptr) {
              inv_branch.target = ft_blk->start_vram;
            }
            pred_blk->instructions.erase(pred_blk->instructions.end() - 2,
                                         pred_blk->instructions.end());
            if (!pred_delay.IsNop()) {
              pred_blk->instructions.push_back(pred_delay);
            }
            pred_blk->instructions.push_back(inv_branch);
            pred_blk->instructions.push_back(MakeNopInstruction(pred_delay.vram));
            RemoveBranchEdge(cfg, pred_id, tail_block_id);
            CfgEdge br_edge;
            br_edge.from_block_id = pred_id;
            br_edge.to_block_id = cond_ft_succ;
            br_edge.type = EdgeType::kBranch;
            CfgEdge ft_edge;
            ft_edge.from_block_id = pred_id;
            ft_edge.to_block_id = cond_taken_succ;
            ft_edge.type = EdgeType::kFallthrough;
            pred_blk->outgoing_edges.push_back(br_edge);
            pred_blk->outgoing_edges.push_back(ft_edge);
            pred_blk->successors.push_back(cond_ft_succ);
            pred_blk->successors.push_back(cond_taken_succ);
            cfg->MutableEdges().push_back(br_edge);
            cfg->MutableEdges().push_back(ft_edge);
            if (BasicBlock* mut_ft = cfg->GetMutableBlock(cond_ft_succ); mut_ft != nullptr) {
              if (std::find(mut_ft->predecessors.begin(), mut_ft->predecessors.end(), pred_id) ==
                  mut_ft->predecessors.end()) {
                mut_ft->predecessors.push_back(pred_id);
              }
            }
            if (BasicBlock* mut_taken = cfg->GetMutableBlock(cond_taken_succ);
                mut_taken != nullptr) {
              if (std::find(mut_taken->predecessors.begin(), mut_taken->predecessors.end(),
                            pred_id) == mut_taken->predecessors.end()) {
                mut_taken->predecessors.push_back(pred_id);
              }
            }
            inlined_cond_branch_preds.insert(pred_id);
            inlined_cond_branch_preds.insert(tail_block_id);
            iter_changed = true;
          } else if (is_return_tail) {
            const Instruction pred_delay = pred_blk->instructions.back();
            pred_blk->instructions.erase(pred_blk->instructions.end() - 2,
                                         pred_blk->instructions.end());
            if (!pred_delay.IsNop()) {
              pred_blk->instructions.push_back(pred_delay);
            }
            pred_blk->instructions.insert(pred_blk->instructions.end(), tail_all_insts.begin(),
                                          tail_all_insts.end());
            RemoveBranchEdge(cfg, pred_id, tail_block_id);
            iter_changed = true;
          } else if (is_jump_tail) {
            std::vector<Instruction> patched_insts = tail_all_insts;
            patched_insts[patched_insts.size() - 2].target = new_target_field;
            const Instruction pred_delay = pred_blk->instructions.back();
            pred_blk->instructions.erase(pred_blk->instructions.end() - 2,
                                         pred_blk->instructions.end());
            if (!pred_delay.IsNop()) {
              pred_blk->instructions.push_back(pred_delay);
            }
            pred_blk->instructions.insert(pred_blk->instructions.end(), patched_insts.begin(),
                                          patched_insts.end());
            RetargetBranchEdge(cfg, pred_id, tail_block_id, merge_block_id);
            iter_changed = true;
          } else {
            Instruction pred_j = pred_blk->instructions[pred_blk->instructions.size() - 2];
            const Instruction pred_delay = pred_blk->instructions.back();
            pred_j.target = new_target_field;
            pred_blk->instructions.erase(pred_blk->instructions.end() - 2,
                                         pred_blk->instructions.end());
            if (!pred_delay.IsNop()) {
              pred_blk->instructions.push_back(pred_delay);
            }
            pred_blk->instructions.insert(pred_blk->instructions.end(), tail_body_insts.begin(),
                                          tail_body_insts.end());
            pred_blk->instructions.push_back(pred_j);
            pred_blk->instructions.push_back(MakeNopInstruction(pred_delay.vram));
            RetargetBranchEdge(cfg, pred_id, tail_block_id, merge_block_id);
            iter_changed = true;
          }
        } else {
          const uint32_t clone_id = static_cast<uint32_t>(cfg->Blocks().size());
          BasicBlock clone_blk;
          clone_blk.id = clone_id;
          clone_blk.start_vram = tail_start_vram;
          clone_blk.end_vram = tail_end_vram;
          if (is_return_tail) {
            clone_blk.instructions = tail_all_insts;
          } else if (is_jump_tail) {
            clone_blk.instructions = tail_all_insts;
            clone_blk.instructions[clone_blk.instructions.size() - 2].target = new_target_field;
          } else {
            clone_blk.instructions = tail_body_insts;
            Instruction clone_j;
            clone_j.vram = tail_end_vram;
            clone_j.opcode = Opcode::kJ;
            clone_j.target = new_target_field;
            clone_j.raw_word = (2u << 26) | (new_target_field & 0x03FFFFFFu);
            clone_blk.instructions.push_back(clone_j);
            clone_blk.instructions.push_back(MakeNopInstruction(tail_end_vram));
          }
          if (!is_return_tail) {
            CfgEdge out_edge;
            out_edge.from_block_id = clone_id;
            out_edge.to_block_id = merge_block_id;
            out_edge.type = EdgeType::kJump;
            clone_blk.outgoing_edges.push_back(out_edge);
            clone_blk.successors.push_back(merge_block_id);
            cfg->MutableEdges().push_back(out_edge);
          }
          cfg->MutableBlocks().push_back(std::move(clone_blk));
          if (!is_return_tail) {
            BasicBlock* mut_merge = cfg->GetMutableBlock(merge_block_id);
            if (mut_merge != nullptr) {
              mut_merge->predecessors.push_back(clone_id);
            }
          }
          for (uint32_t pred_id : grp.jump_preds) {
            BasicBlock* pred_blk = cfg->GetMutableBlock(pred_id);
            if (pred_blk != nullptr && pred_blk->instructions.size() >= 2) {
              const Instruction pred_j = pred_blk->instructions[pred_blk->instructions.size() - 2];
              const Instruction pred_delay = pred_blk->instructions.back();
              if (!pred_delay.IsNop()) {
                pred_blk->instructions[pred_blk->instructions.size() - 2] = pred_delay;
                pred_blk->instructions[pred_blk->instructions.size() - 1] = pred_j;
                pred_blk->instructions.push_back(MakeNopInstruction(pred_delay.vram));
              }
            }
            RetargetBranchEdge(cfg, pred_id, tail_block_id, clone_id);
          }
          for (uint32_t pred_id : grp.branch_preds) {
            RetargetBranchEdge(cfg, pred_id, tail_block_id, clone_id);
          }
          iter_changed = true;
        }
      }
      if (iter_changed) {
        break;
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

CfgPassPipeline CfgDeReorg::BuildStage1Pipeline() {
  CfgPassPipeline pipeline;
  pipeline.AddPass(std::make_unique<TargetHeaderDelaySlotReconcilerPass>());
  pipeline.AddPass(std::make_unique<RelaxDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>(
      EagerThreadUnscheduleMode::kStage1Full));
  pipeline.AddPass(
      std::make_unique<SimpleDelaySlotUnschedulerPass>(/*unschedule_all_remaining=*/true));
  return pipeline;
}

CfgPassPipeline CfgDeReorg::BuildDefaultPipeline() {
  CfgPassPipeline pipeline;
  pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<TargetHeaderDelaySlotReconcilerPass>());
  pipeline.AddPass(std::make_unique<RelaxDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>(
      EagerThreadUnscheduleMode::kPreCrossJump));
  pipeline.AddPass(std::make_unique<CrossJumpTailDuplicatorPass>());
  pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>(
      EagerThreadUnscheduleMode::kPostCrossJump));
  return pipeline;
}

void CfgDeReorg::UnscheduleDelaySlotsOnly(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return;
  }
  CfgPassPipeline pipeline = BuildStage1Pipeline();
  pipeline.Run(cfg, analyses);
  if (cfg->CoalesceContiguousBlocks() && analyses != nullptr) {
    analyses->InvalidateAll();
  }
}

void CfgDeReorg::UnscheduleDelaySlots(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return;
  }
  CfgPassPipeline pipeline = BuildDefaultPipeline();
  pipeline.Run(cfg, analyses);
}

}  // namespace rom_nom_nom
