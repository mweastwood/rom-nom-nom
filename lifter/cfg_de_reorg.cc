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

bool IsFpRegisterDeadAtBlockEntry(const ControlFlowGraph& cfg, uint32_t start_block_id,
                                  FpRegister freg, bool returns_f0) {
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
        for (FpRegister u : inst_ud.fpr_uses) {
          if (u == freg) {
            return false;
          }
        }
        RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
        for (FpRegister u : delay_ud.fpr_uses) {
          if (u == freg) {
            return false;
          }
        }
        if (!annuls_delay) {
          for (FpRegister d : delay_ud.fpr_defs) {
            if (d == freg) {
              killed = true;
              break;
            }
          }
        }
        if (killed) {
          break;
        }
        if (inst.IsCall()) {
          if (freg >= FpRegister::kF12 && freg <= FpRegister::kF15) {
            return false;
          }
          if (freg < FpRegister::kF20) {
            killed = true;
            break;
          }
        } else if (inst.IsReturn()) {
          if ((freg == FpRegister::kF0 || freg == FpRegister::kF1) && returns_f0) {
            return false;
          }
          killed = true;
          break;
        }
        ++i;
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (FpRegister u : ud.fpr_uses) {
        if (u == freg) {
          return false;
        }
      }
      for (FpRegister d : ud.fpr_defs) {
        if (d == freg) {
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
        if ((freg == FpRegister::kF0 || freg == FpRegister::kF1) && returns_f0) {
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
          if ((reg == Register::kV0 || reg == Register::kV1) && returns_v0) {
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
        if ((reg == Register::kV0 || reg == Register::kV1) && returns_v0) {
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

bool AllDefinedRegistersDeadAtBlockEntry(const ControlFlowGraph& cfg, uint32_t start_block_id,
                                         const RegisterUseDef& ud, bool returns_v0,
                                         bool returns_f0) {
  if (ud.gpr_defs.empty() && ud.fpr_defs.empty()) {
    return false;
  }
  for (Register r : ud.gpr_defs) {
    if (!IsRegisterDeadAtBlockEntry(cfg, start_block_id, r, returns_v0)) {
      return false;
    }
  }
  for (FpRegister fr : ud.fpr_defs) {
    if (!IsFpRegisterDeadAtBlockEntry(cfg, start_block_id, fr, returns_f0)) {
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
  const DominatorTree& dom_tree = analyses->GetDominatorTree();
  const LoopInfo& loop_info = analyses->GetLoopInfo();
  const DominatorTree& post_dom_tree = analyses->GetPostDominatorTree();
  const bool returns_f0 = AbiAnalyzer::DetermineReturnsF0(*cfg);
  const bool returns_v0 = !returns_f0 && AbiAnalyzer::DetermineReturnsV0(*cfg);
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
          if (pred_blk != nullptr && pred_loop == enclosing_loop && target_loop == enclosing_loop &&
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
    if (!term.IsBranchLikely()) {
      RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
      if (!IsSinkableDelaySlotInstruction(block, delay_inst) ||
          !AllDefinedRegistersDeadAtBlockEntry(*cfg, succ_fallthrough, delay_ud, returns_v0,
                                               returns_f0)) {
        continue;
      }
    }

    const Loop* target_loop = loop_info.GetLoopFor(succ_taken);
    std::optional<uint32_t> pred_block_id;
    for (uint32_t pred_id : taken_block->predecessors) {
      if (pred_id == block.id || pred_id == succ_fallthrough) {
        continue;
      }
      const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
      const Loop* pred_loop = loop_info.GetLoopFor(pred_id);
      auto uses_preserved_to_pred = [&]() {
        RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
        if (delay_ud.gpr_uses.empty() && delay_ud.fpr_uses.empty()) {
          return true;
        }
        absl::flat_hash_set<uint32_t> visited_path = {pred_id};
        std::vector<uint32_t> stack = {succ_fallthrough};
        while (!stack.empty()) {
          uint32_t cur_id = stack.back();
          stack.pop_back();
          if (!visited_path.insert(cur_id).second) continue;
          const BasicBlock* cur_blk = cfg->GetBlock(cur_id);
          if (cur_blk == nullptr) return false;
          for (const auto& inst : cur_blk->instructions) {
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
            if (s != pred_id) stack.push_back(s);
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
          ((has_pre_branch_conflict && post_dom_tree.Dominates(pred_id, succ_fallthrough) &&
            uses_preserved_to_pred()) ||
           (term.IsBranchLikely() && pred_blk->predecessors.size() >= 2) ||
           (!dom_tree.Dominates(succ_fallthrough, pred_id) && !pred_blk->predecessors.empty()));
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
        AllDefinedRegistersDeadAtBlockEntry(*cfg, succ_taken, delay_ud, returns_v0, returns_f0);
    bool dead_on_fallthrough = AllDefinedRegistersDeadAtBlockEntry(*cfg, succ_fallthrough, delay_ud,
                                                                   returns_v0, returns_f0);
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
  for (int iter = 0; iter < 64; ++iter) {
    const LoopInfo& loop_info = analyses->GetLoopInfo();
    const DominatorTree& dom_tree = analyses->GetDominatorTree();
    const DominatorTree& post_dom_tree = analyses->GetPostDominatorTree();
    bool iter_changed = false;

    absl::flat_hash_set<uint32_t> switch_decision_blocks;
    absl::flat_hash_set<uint32_t> switch_join_blocks;
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
      if (succ_taken != 0 || succ_ft != 0) {
        std::optional<uint32_t> join = post_dom_tree.ImmediateDominator(blk.id);
        auto sc = TryCollapseShortCircuitChain(*cfg, {}, blk.id, succ_taken, succ_ft, join);
        if (!sc.has_value() && !join.has_value()) {
          sc = TryCollapseShortCircuitChain(*cfg, {}, blk.id, succ_taken, succ_ft, 0xFFFFFFFDu);
        }
        if (sc.has_value()) {
          short_circuit_blocks.insert(blk.id);
          short_circuit_blocks.insert(sc->additional_condition_blocks.begin(),
                                      sc->additional_condition_blocks.end());
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
          tail_blk.successors.size() > 1 || tail_blk.HasSwitch() ||
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
      if (!is_jump_tail && !is_fallthrough_tail && !is_return_tail) {
        continue;
      }

      const size_t pre_term_count =
          is_fallthrough_tail ? tail_blk.instructions.size() : (tail_blk.instructions.size() - 2);
      const bool has_non_nop_tail_delay =
          !is_fallthrough_tail && !tail_blk.instructions.back().IsNop();
      const size_t effective_body_count = pre_term_count + (has_non_nop_tail_delay ? 1 : 0);
      if (effective_body_count == 0) {
        continue;
      }

      uint32_t merge_block_id = 0;
      uint32_t new_target_field = 0;
      const BasicBlock* merge_blk = nullptr;
      if (!is_return_tail) {
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
      const bool tail_post_dominates_idom = post_dom_tree.Dominates(tail_block_id, *idom);
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
        bool pred_ft_has_call_or_store = false;
        if (const BasicBlock* pred_ft_blk = cfg->GetBlock(pred_ft_succ); pred_ft_blk != nullptr) {
          for (const auto& inst : pred_ft_blk->instructions) {
            if (inst.IsCall() || inst.IsStore()) {
              pred_ft_has_call_or_store = true;
              break;
            }
          }
        }
        const bool allow_idom_branch_pred =
            !merges_into_return && !pred_ft_is_tail_pred && pred_ft_has_call_or_store;
        const bool is_valid_branch_pred =
            !tail_post_dominates_idom && (*sub_opt != *idom || allow_idom_branch_pred) &&
            pred_blk->successors.size() == 2 && pred_has_branch_to_tail &&
            pred_ft_succ != tail_block_id && loop_info.GetLoopFor(pred_ft_succ) == tail_loop &&
            pred_term != nullptr && pred_term->IsBranch() && !pred_term->IsUnconditionalBranch() &&
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

      if ((is_fallthrough_tail || is_return_tail) && !has_fallthrough_pred) {
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
        if (!grp.branch_preds.empty() && !has_fallthrough_pred) {
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
          if (is_return_tail) {
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
