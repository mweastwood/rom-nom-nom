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

bool AllDefinedRegistersDeadAtBlockEntry(const ControlFlowGraph& cfg, uint32_t start_block_id,
                                         const RegisterUseDef& ud, bool returns_v0,
                                         bool returns_f0) {
  if (ud.gpr_defs.empty() && ud.fpr_defs.empty()) {
    return false;
  }
  for (Register r : ud.gpr_defs) {
    if (!AbiAnalyzer::IsRegisterDeadAtBlockEntry(cfg, start_block_id, r, returns_v0,
                                                 /*returns_v1=*/returns_v0)) {
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
        (DelaySlotCompletesSplitInstructionPair(block, delay_inst) ||
         (block.id == cfg->EntryBlockId() && IsPrologueCalleeSaveStore(delay_inst)))) {
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

namespace {

std::optional<uint32_t> MatchChainedFallthroughBranchToSameTarget(
    const ControlFlowGraph& cfg, const BasicBlock& block, uint32_t succ_taken,
    uint32_t succ_fallthrough, const RegisterUseDef& delay_ud, bool returns_v0, bool returns_f0) {
  const BasicBlock* ft_blk = cfg.GetBlock(succ_fallthrough);
  if (ft_blk == nullptr || ft_blk->predecessors.size() != 1 ||
      ft_blk->predecessors[0] != block.id || ft_blk->instructions.size() != 2 ||
      !ft_blk->instructions.back().IsNop() || ft_blk->successors.size() != 2) {
    return std::nullopt;
  }
  const Instruction& ft_term = ft_blk->instructions.front();
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
  RegisterUseDef ft_ud = GetInstructionUseDef(ft_term);
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
                                uint32_t succ_fallthrough) {
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
  if (!has_inner_nop_branch_to_taken) {
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
  const auto jump_table_bounds_vrams = CollectJumpTableBoundsVrams(*cfg);
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
      if (delay_inst.IsNop() || jump_table_bounds_vrams.contains(term.vram)) {
        continue;
      }
      const Loop* enclosing_loop = loop_info.GetLoopFor(block.id);
      if (enclosing_loop != nullptr && enclosing_loop->IsLatch(block.id)) {
        continue;
      }

      if (term.opcode == Opcode::kJ && block.successors.size() == 1 &&
          IsSinkableDelaySlotInstruction(block, delay_inst)) {
        const uint32_t succ_taken = block.successors[0];
        BasicBlock* taken_block = cfg->GetMutableBlock(succ_taken);
        if (taken_block != nullptr && succ_taken != block.id &&
            taken_block->predecessors.size() >= 2 &&
            (enclosing_loop == nullptr || succ_taken != enclosing_loop->header)) {
          const Loop* target_loop = loop_info.GetLoopFor(succ_taken);
          std::optional<uint32_t> pred_block_id;
          for (uint32_t pred_id : taken_block->predecessors) {
            if (pred_id == block.id) continue;
            const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
            const Loop* pred_loop = loop_info.GetLoopFor(pred_id);
            if (pred_blk != nullptr && pred_loop == enclosing_loop &&
                target_loop == enclosing_loop &&
                !jump_table_target_vrams.contains(pred_blk->start_vram) &&
                pred_blk->predecessors.size() >= 2 && pred_blk->instructions.size() == 1 &&
                pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
                pred_blk->end_vram + 4 == taken_block->start_vram &&
                SameDelayInstructionEffect(delay_inst, pred_blk->instructions.front())) {
              pred_block_id = pred_id;
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

      const bool has_pre_branch_conflict =
          term.IsBranchLikely() || DelaySlotHasPreBranchDependenceConflict(block, term, delay_inst);
      uint32_t effective_fallthrough = succ_fallthrough;
      std::optional<uint32_t> chained_ft_branch_block_id;
      if (!term.IsBranchLikely()) {
        RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
        if (!IsSinkableDelaySlotInstruction(block, delay_inst)) {
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
                                           delay_inst, succ_taken, succ_fallthrough)) {
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
      for (uint32_t pred_id : taken_block->predecessors) {
        if (pred_id == block.id || pred_id == succ_fallthrough ||
            pred_id == effective_fallthrough) {
          continue;
        }
        const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
        const Loop* pred_loop = loop_info.GetLoopFor(pred_id);
        auto uses_preserved_to_pred = [&]() {
          absl::flat_hash_set<uint32_t> can_reach_pred = {pred_id};
          bool added_reach = true;
          while (added_reach) {
            added_reach = false;
            for (const auto& candidate : cfg->Blocks()) {
              if (candidate.id == block.id || can_reach_pred.contains(candidate.id)) {
                continue;
              }
              for (uint32_t succ : candidate.successors) {
                if (!dom_tree.Dominates(succ, candidate.id) && can_reach_pred.contains(succ)) {
                  can_reach_pred.insert(candidate.id);
                  added_reach = true;
                  break;
                }
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
        const bool same_loop_non_header =
            pred_loop == enclosing_loop && target_loop == enclosing_loop &&
            (enclosing_loop == nullptr || succ_taken != enclosing_loop->header) &&
            pred_blk != nullptr && !jump_table_target_vrams.contains(pred_blk->start_vram) &&
            pred_blk->end_vram + 4 == taken_block->start_vram &&
            taken_block->predecessors.size() >= 2 &&
            IsSinkableDelaySlotInstruction(block, delay_inst) &&
            (uses_preserved_to_pred() ||
             (term.IsBranchLikely() && (pred_blk->predecessors.size() >= 2 ||
                                        post_dom_tree.Dominates(pred_id, effective_fallthrough))) ||
             (!dom_tree.Dominates(effective_fallthrough, pred_id) &&
              !pred_blk->predecessors.empty()));
        const bool cross_loop_valid =
            has_pre_branch_conflict &&
            ((enclosing_loop != nullptr && enclosing_loop->Contains(block.id) &&
              !enclosing_loop->Contains(pred_id)) ||
             (target_loop != nullptr && target_loop->header == succ_taken &&
              !target_loop->Contains(block.id) && !target_loop->Contains(pred_id)));
        if (pred_blk != nullptr && pred_blk->instructions.size() == 1 &&
            pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
            SameDelayInstructionEffect(delay_inst, pred_blk->instructions.front()) &&
            (same_loop_non_header || cross_loop_valid)) {
          pred_block_id = pred_id;
          break;
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
      } else if (!term.IsBranchLikely() &&
                 TryUnmergeHammockDelaySlot(cfg, dom_tree, post_dom_tree, loop_info, &block, term,
                                            delay_inst, succ_taken, succ_fallthrough)) {
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
  absl::flat_hash_set<uint32_t> jump_table_target_vrams;
  for (const auto& jt : cfg->JumpTables()) {
    for (const auto& entry : jt.entries) {
      if (entry.target_vram != jt.default_target_vram) {
        jump_table_target_vrams.insert(entry.target_vram);
      }
    }
  }

  auto compute_unscheduled_clobber_reg = [](const RegisterUseDef& delay_ud) {
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

  bool any_changed = false;
  for (int iter = 0; iter < 4; ++iter) {
    bool changed = false;

    // 0. Multi-predecessor target delay-slot sinking: when every incoming edge to a
    // join/target block `target_blk` ends by executing the same sinkable instruction `D`
    // (either in a branch delay slot where `D` is annulled or dead on the other edge,
    // or as the trailing instruction of a non-branch fallthrough predecessor), sink `D`
    // back to the start of `target_blk`.
    for (size_t b = 0; b < cfg->Blocks().size(); ++b) {
      BasicBlock& target_blk = cfg->MutableBlocks()[b];
      if (target_blk.id == cfg->EntryBlockId() || target_blk.predecessors.size() < 2 ||
          target_blk.instructions.empty() || target_blk.instructions.front().IsReturn() ||
          jump_table_target_vrams.contains(target_blk.start_vram)) {
        continue;
      }
      const Loop* enclosing_loop = loop_info.GetLoopFor(target_blk.id);
      if (enclosing_loop != nullptr && enclosing_loop->header == target_blk.id) {
        continue;
      }

      std::optional<Instruction> common_delay;
      std::vector<uint32_t> branch_preds;
      std::vector<uint32_t> fallthrough_body_preds;
      bool all_preds_match = true;
      bool has_motivating_conflict = false;

      for (uint32_t pred_id : target_blk.predecessors) {
        if (pred_id == target_blk.id || loop_info.GetLoopFor(pred_id) != enclosing_loop ||
            (enclosing_loop != nullptr && enclosing_loop->IsLatch(pred_id))) {
          all_preds_match = false;
          break;
        }
        const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
        if (pred_blk == nullptr || pred_blk->instructions.size() < 2) {
          all_preds_match = false;
          break;
        }

        const Instruction* pred_term_ptr = pred_blk->Terminator();
        const bool ends_with_delay_slot =
            pred_term_ptr != nullptr &&
            pred_term_ptr == &pred_blk->instructions[pred_blk->instructions.size() - 2] &&
            pred_term_ptr->HasDelaySlot();

        if (ends_with_delay_slot) {
          const Instruction pred_term = *pred_term_ptr;
          const Instruction pred_delay = pred_blk->instructions.back();
          if (pred_delay.IsNop() || !pred_term.IsBranch() || pred_term.IsUnconditionalBranch() ||
              pred_blk->successors.size() != 2 ||
              jump_table_bounds_vrams.contains(pred_term.vram) ||
              !IsSinkableDelaySlotInstruction(*pred_blk, pred_delay) ||
              (DelaySlotConflictsWithTerminator(pred_term, pred_delay) &&
               target_blk.instructions.front().IsCall())) {
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
          if (enclosing_loop != nullptr && enclosing_loop->header == pred_id &&
              (loop_info.GetLoopFor(succ_taken) != enclosing_loop ||
               loop_info.GetLoopFor(succ_fallthrough) != enclosing_loop)) {
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
            RegisterUseDef delay_ud = GetInstructionUseDef(pred_delay);
            if (!AllDefinedRegistersDeadAtBlockEntry(*cfg, other_succ, delay_ud, returns_v0,
                                                     returns_f0)) {
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
          if (pred_term.IsBranchLikely() ||
              DelaySlotHasPreBranchDependenceConflict(*pred_blk, pred_term, pred_delay) ||
              DelaySlotFeedsSuccessorBranch(target_blk, pred_delay)) {
            has_motivating_conflict = true;
          }
          branch_preds.push_back(pred_id);
        } else if (pred_blk->successors.size() == 1 && pred_blk->successors[0] == target_blk.id &&
                   pred_blk->end_vram + 4 == target_blk.start_vram) {
          const Instruction last_inst = pred_blk->instructions.back();
          if (last_inst.IsNop() || last_inst.IsBranch() || last_inst.IsJump() ||
              last_inst.IsCall() || last_inst.IsReturn() ||
              !IsSinkableDelaySlotInstruction(*pred_blk, last_inst)) {
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
        sunk_inst.vram = target_blk.start_vram;
        RegisterUseDef delay_ud = GetInstructionUseDef(sunk_inst);
        std::optional<Register> candidate_clobber_reg = compute_unscheduled_clobber_reg(delay_ud);
        target_blk.instructions.insert(target_blk.instructions.begin(), sunk_inst);
        for (uint32_t pred_id : branch_preds) {
          BasicBlock* pred_blk = cfg->GetMutableBlock(pred_id);
          if (pred_blk == nullptr || pred_blk->instructions.size() < 2) continue;
          const Instruction pred_term = pred_blk->instructions[pred_blk->instructions.size() - 2];
          const Instruction pred_delay = pred_blk->instructions.back();
          pred_blk->instructions[pred_blk->instructions.size() - 2].opcode =
              ToNonLikelyBranchOpcode(pred_term.opcode);
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
          pred_blk->instructions.back() = MakeNopInstruction(pred_blk->instructions.back().vram);
        }
        changed = true;
      }
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
          bool ft_identical =
              ft_block != nullptr && !ft_block->instructions.empty() &&
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
      if (succ_taken == succ_fallthrough || succ_taken == block.id ||
          succ_fallthrough == block.id) {
        continue;
      }

      if (!IsSinkableDelaySlotInstruction(block, delay_inst)) {
        continue;
      }

      RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
      bool dead_on_taken =
          AllDefinedRegistersDeadAtBlockEntry(*cfg, succ_taken, delay_ud, returns_v0, returns_f0);
      bool dead_on_fallthrough = AllDefinedRegistersDeadAtBlockEntry(
          *cfg, succ_fallthrough, delay_ud, returns_v0, returns_f0);
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

      const bool is_preloop_const_callee_saved_init =
          target_blk->successors.size() == 1 &&
          loop_info.GetLoopFor(target_blk->successors[0]) != enclosing_loop &&
          delay_ud.gpr_uses.empty();
      const bool is_fallthrough_stolen_callee_saved_init =
          dead_on_taken && !dead_on_fallthrough && !is_preloop_const_callee_saved_init &&
          delay_ud.gpr_defs.size() == 1 && delay_ud.gpr_defs[0] >= Register::kS0 &&
          delay_ud.gpr_defs[0] <= Register::kS7;

      if (DelaySlotHasPreBranchDependenceConflict(block, term, delay_inst) ||
          DelaySlotFeedsSuccessorBranch(*target_blk, delay_inst) ||
          is_fallthrough_stolen_callee_saved_init) {
        std::optional<Register> clobber_reg = compute_unscheduled_clobber_reg(delay_ud);
        target_blk->instructions.insert(target_blk->instructions.begin(), delay_inst);
        block.instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
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

CfgPassPipeline CfgDeReorg::BuildDefaultPipeline() {
  CfgPassPipeline pipeline;
  pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<TargetHeaderDelaySlotReconcilerPass>());
  pipeline.AddPass(std::make_unique<RelaxDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<CrossJumpTailDuplicatorPass>());
  pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>());
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
