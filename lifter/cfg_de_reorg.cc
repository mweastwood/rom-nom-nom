#include "lifter/cfg_de_reorg.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "lifter/abi_analyzer.h"
#include "lifter/branch_condition_builder.h"
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
    if (edge.type == EdgeType::kBranch && edge.to_block_id == old_to_block_id) {
      edge.to_block_id = new_to_block_id;
    }
  }
  for (auto& edge : cfg->MutableEdges()) {
    if (edge.from_block_id == from_block_id && edge.type == EdgeType::kBranch &&
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

  // Do not sink an instruction that consumes a local `lui` high-half address in `block`,
  // so intra-block `lui` + `addiu`/`lw`/`ori` symbol folding remains intact.
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

}  // namespace

void CfgDeReorg::UnscheduleDelaySlots(ControlFlowGraph* cfg) {
  if (cfg == nullptr || cfg->Blocks().empty()) {
    return;
  }

  const bool returns_f0 = AbiAnalyzer::DetermineReturnsF0(*cfg);
  const bool returns_v0 = !returns_f0 && AbiAnalyzer::DetermineReturnsV0(*cfg);

  DominatorTree dom_tree = DominatorTree::Compute(*cfg);
  LoopInfo loop_info = LoopInfo::Analyze(*cfg, dom_tree);

  absl::flat_hash_set<uint32_t> jump_table_bounds_vrams;
  for (const auto& jt : cfg->JumpTables()) {
    if (jt.bounds_branch_vram != 0) {
      jump_table_bounds_vrams.insert(jt.bounds_branch_vram);
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

    // 1. Same-block delay-slot hoisting (`fill_simple_delay_slots` inverse) for
    // multi-instruction pairs (unaligned memory accesses `lwl`/`lwr`, `swl`/`swr`
    // or multiply `mult` + `mflo`/`mfhi`) split across a branch or jump.
    if ((term.opcode == Opcode::kJ || (term.IsBranch() && !term.IsBranchLikely())) &&
        block.instructions.size() >= 3) {
      const Instruction& prev_inst = block.instructions[block.instructions.size() - 3];
      bool is_split_unaligned = IsPartnerUnalignedPair(prev_inst, delay_inst);
      bool is_split_mult =
          (delay_inst.opcode == Opcode::kMflo || delay_inst.opcode == Opcode::kMfhi) &&
          (prev_inst.opcode == Opcode::kMult || prev_inst.opcode == Opcode::kMultu ||
           prev_inst.opcode == Opcode::kDmult || prev_inst.opcode == Opcode::kDmultu);
      if ((is_split_unaligned || is_split_mult) &&
          !DelaySlotConflictsWithTerminator(term, delay_inst)) {
        block.instructions[block.instructions.size() - 2] = delay_inst;
        block.instructions[block.instructions.size() - 1] = term;
        block.instructions.push_back(MakeNopInstruction(delay_inst.vram));
        continue;
      }
    }

    // 2. Branch-likely (`bnezl`/`beqzl`/`bc1tl`/`bc1fl`) target-stolen delay-slot
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
        if (taken_block != nullptr && succ_taken != succ_fallthrough) {
          std::optional<uint32_t> pred_block_id;
          for (uint32_t pred_id : taken_block->predecessors) {
            if (pred_id == block.id) {
              continue;
            }
            const BasicBlock* pred_blk = cfg->GetBlock(pred_id);
            if (pred_blk != nullptr && pred_blk->instructions.size() == 1 &&
                pred_blk->successors.size() == 1 && pred_blk->successors[0] == succ_taken &&
                SameDelayInstructionEffect(delay_inst, pred_blk->instructions.front()) &&
                enclosing_loop != nullptr && enclosing_loop->Contains(block.id) &&
                !enclosing_loop->Contains(pred_id)) {
              pred_block_id = pred_id;
              break;
            }
          }
          if (pred_block_id.has_value()) {
            RetargetBranchEdge(cfg, block.id, succ_taken, *pred_block_id);
            block.instructions.back() = MakeNopInstruction(delay_inst.vram);
            continue;
          }
        }
        bool ft_identical = ft_block != nullptr && !ft_block->instructions.empty() &&
                            SameDelayInstructionEffect(delay_inst, ft_block->instructions.front());
        if (taken_block != nullptr && !ft_identical && succ_taken != succ_fallthrough &&
            succ_taken != block.id && succ_taken != cfg->EntryBlockId() &&
            taken_block->predecessors.size() == 1 && taken_block->predecessors[0] == block.id &&
            loop_info.GetLoopFor(succ_taken) == enclosing_loop &&
            (enclosing_loop == nullptr || enclosing_loop->header != block.id) &&
            IsSinkableDelaySlotInstruction(block, delay_inst) &&
            !taken_block->instructions.empty() && !taken_block->instructions.front().IsReturn() &&
            (DelaySlotBlocksFoldedComparison(block) ||
             DelaySlotEnablesSuccessorFoldedComparison(*taken_block, delay_inst) ||
             (DelaySlotConflictsWithTerminator(term, delay_inst) &&
              !taken_block->instructions.front().IsCall()))) {
          RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
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
          taken_block->instructions.insert(taken_block->instructions.begin(), delay_inst);
          block.instructions[block.instructions.size() - 2].opcode =
              ToNonLikelyBranchOpcode(term.opcode);
          block.instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
          continue;
        }
      }
    }

    // 3. Fallthrough / Taken Sinking (`fill_eager_delay_slots` inverse) for
    // non-likely conditional branches.
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
    if (enclosing_loop != nullptr && enclosing_loop->header == block.id &&
        (loop_info.GetLoopFor(succ_taken) != enclosing_loop ||
         loop_info.GetLoopFor(succ_fallthrough) != enclosing_loop)) {
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

    bool blocks_folded_comparison = DelaySlotBlocksFoldedComparison(block);

    if (dead_on_taken && !dead_on_fallthrough) {
      BasicBlock* ft_block = cfg->GetMutableBlock(succ_fallthrough);
      if (ft_block != nullptr && succ_fallthrough != cfg->EntryBlockId() &&
          ft_block->predecessors.size() == 1 && ft_block->predecessors[0] == block.id &&
          loop_info.GetLoopFor(succ_fallthrough) == enclosing_loop &&
          (blocks_folded_comparison ||
           (DelaySlotConflictsWithTerminator(term, delay_inst) &&
            DelaySlotEnablesSuccessorFoldedComparison(*ft_block, delay_inst)))) {
        ft_block->instructions.insert(ft_block->instructions.begin(), delay_inst);
        block.instructions.back() = MakeNopInstruction(delay_inst.vram, clobber_reg);
      }
    }
  }
}

}  // namespace rom_nom_nom
