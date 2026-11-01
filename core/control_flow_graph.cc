#include "core/control_flow_graph.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/division_pattern.h"
#include "core/mips.h"

namespace rom_nom_nom {

const Instruction* BasicBlock::Terminator() const {
  if (instructions.empty()) {
    return nullptr;
  }
  // If block ends with a branch/jump and delay slot, the control instruction
  // is at size - 2.
  if (instructions.size() >= 2) {
    const auto& candidate = instructions[instructions.size() - 2];
    if (candidate.HasDelaySlot()) {
      return &candidate;
    }
  }
  const auto& last = instructions.back();
  if (last.IsBranch() || last.IsJump()) {
    return &last;
  }
  return &last;
}

bool BasicBlock::HasBranch() const {
  const auto* term = Terminator();
  return term != nullptr && term->IsBranch();
}

bool BasicBlock::HasReturn() const {
  const auto* term = Terminator();
  return term != nullptr && term->IsReturn();
}

absl::StatusOr<ControlFlowGraph> ControlFlowGraph::Build(absl::Span<const Instruction> instructions,
                                                         absl::Span<const JumpTable> jump_tables) {
  if (instructions.empty()) {
    return absl::InvalidArgumentError("ControlFlowGraph: cannot build from empty instructions.");
  }

  ControlFlowGraph cfg;
  cfg.jump_tables_.assign(jump_tables.begin(), jump_tables.end());

  // 1. Index instructions by VRAM address.
  absl::flat_hash_map<uint32_t, size_t> vram_to_idx;
  vram_to_idx.reserve(instructions.size());
  for (size_t i = 0; i < instructions.size(); ++i) {
    vram_to_idx[instructions[i].vram] = i;
  }

  // 2. Identify basic block leaders.
  std::vector<bool> is_leader(instructions.size(), false);
  is_leader[0] = true;

  // Jump table targets and default targets are leaders
  absl::flat_hash_map<uint32_t, const JumpTable*> switch_vram_to_table;
  for (const auto& table : cfg.jump_tables_) {
    switch_vram_to_table[table.switch_vram] = &table;
    for (const auto& entry : table.entries) {
      auto it = vram_to_idx.find(entry.target_vram);
      if (it != vram_to_idx.end()) {
        is_leader[it->second] = true;
      }
    }
    auto it_def = vram_to_idx.find(table.default_target_vram);
    if (it_def != vram_to_idx.end()) {
      is_leader[it_def->second] = true;
    }
  }

  // Pre-detect division/modulo trap expansions so their internal branches and break traps
  // do not split the control flow graph.
  absl::flat_hash_set<size_t> division_trap_internal_indices;
  absl::flat_hash_set<size_t> division_trap_branch_indices;
  for (size_t i = 0; i < instructions.size(); ++i) {
    auto div_pat = MatchDivisionPattern(instructions, i);
    if (div_pat.has_value()) {
      for (size_t idx = div_pat->start_index + 1; idx <= div_pat->end_index; ++idx) {
        division_trap_internal_indices.insert(idx);
        if (instructions[idx].IsBranch() || instructions[idx].IsJump()) {
          division_trap_branch_indices.insert(idx);
        }
      }
    }
  }

  for (size_t i = 0; i < instructions.size(); ++i) {
    if (division_trap_branch_indices.contains(i)) {
      continue;
    }
    const auto& inst = instructions[i];
    if (inst.IsBranch()) {
      uint32_t target = inst.BranchTarget();
      auto it = vram_to_idx.find(target);
      if (it != vram_to_idx.end() && !division_trap_internal_indices.contains(it->second)) {
        is_leader[it->second] = true;
      }
      size_t next_idx = inst.HasDelaySlot() ? (i + 2) : (i + 1);
      if (next_idx < instructions.size() && !division_trap_internal_indices.contains(next_idx)) {
        is_leader[next_idx] = true;
      }
    } else if (inst.IsJump()) {
      if (inst.opcode == Opcode::kJ) {
        uint32_t target = inst.JumpTarget();
        auto it = vram_to_idx.find(target);
        if (it != vram_to_idx.end() && !division_trap_internal_indices.contains(it->second)) {
          is_leader[it->second] = true;
        }
      } else if (inst.opcode == Opcode::kJr && inst.rs.has_value() && *inst.rs != Register::kRa) {
        Register jump_reg = *inst.rs;
        for (size_t prev_i = i; prev_i > 0; --prev_i) {
          const auto& prev = instructions[prev_i - 1];
          if ((prev.opcode == Opcode::kAddiu || prev.opcode == Opcode::kAddi) &&
              prev.rt.has_value() && *prev.rt == jump_reg) {
            Register base_reg = prev.rs.value_or(Register::kZero);
            int32_t lo_imm = static_cast<int32_t>(prev.immediate);
            for (size_t hi_i = prev_i - 1; hi_i > 0; --hi_i) {
              const auto& hi_inst = instructions[hi_i - 1];
              if (hi_inst.opcode == Opcode::kLui && hi_inst.rt.has_value() &&
                  *hi_inst.rt == base_reg) {
                uint32_t hi_imm = static_cast<uint32_t>(static_cast<uint16_t>(hi_inst.immediate));
                uint32_t const_target = (hi_imm << 16) + lo_imm;
                auto it = vram_to_idx.find(const_target);
                if (it != vram_to_idx.end() &&
                    !division_trap_internal_indices.contains(it->second)) {
                  is_leader[it->second] = true;
                }
                break;
              }
            }
            break;
          } else if (prev.opcode == Opcode::kLui && prev.rt.has_value() && *prev.rt == jump_reg) {
            uint32_t hi_imm = static_cast<uint32_t>(static_cast<uint16_t>(prev.immediate));
            uint32_t const_target = hi_imm << 16;
            auto it = vram_to_idx.find(const_target);
            if (it != vram_to_idx.end() && !division_trap_internal_indices.contains(it->second)) {
              is_leader[it->second] = true;
            }
            break;
          }
        }
      }
      size_t next_idx = inst.HasDelaySlot() ? (i + 2) : (i + 1);
      if (next_idx < instructions.size() && !division_trap_internal_indices.contains(next_idx)) {
        is_leader[next_idx] = true;
      }
    }
  }

  // 3. Assemble basic blocks.
  std::vector<size_t> leader_indices;
  for (size_t i = 0; i < instructions.size(); ++i) {
    if (is_leader[i]) {
      leader_indices.push_back(i);
    }
  }

  cfg.blocks_.reserve(leader_indices.size());
  for (size_t b = 0; b < leader_indices.size(); ++b) {
    size_t start_idx = leader_indices[b];
    size_t end_idx = (b + 1 < leader_indices.size()) ? leader_indices[b + 1] : instructions.size();

    BasicBlock block;
    block.id = static_cast<uint32_t>(b);
    block.start_vram = instructions[start_idx].vram;
    block.end_vram = instructions[end_idx - 1].vram;
    block.instructions.assign(instructions.begin() + start_idx, instructions.begin() + end_idx);

    for (const auto& inst : block.instructions) {
      auto it_table = switch_vram_to_table.find(inst.vram);
      if (it_table != switch_vram_to_table.end()) {
        block.jump_table = it_table->second;
        break;
      }
    }

    cfg.vram_to_block_index_[block.start_vram] = b;
    cfg.blocks_.push_back(std::move(block));
  }

  cfg.entry_block_id_ = 0;

  // 4. Construct control flow edges.
  auto add_edge = [&](uint32_t from, uint32_t to, EdgeType type) {
    CfgEdge edge;
    edge.from_block_id = from;
    edge.to_block_id = to;
    edge.type = type;
    cfg.edges_.push_back(edge);
    cfg.blocks_[from].outgoing_edges.push_back(edge);
    cfg.blocks_[from].successors.push_back(to);
    cfg.blocks_[to].predecessors.push_back(from);
  };

  const size_t initial_block_count = cfg.blocks_.size();
  for (size_t b = 0; b < initial_block_count; ++b) {
    const auto* term = cfg.blocks_[b].Terminator();
    if (term == nullptr) continue;

    if (term->IsReturn()) {
      // Function exit block: no outgoing edges.
      continue;
    }

    if (cfg.blocks_[b].jump_table != nullptr) {
      absl::flat_hash_set<uint32_t> added_targets;
      for (const auto& entry : cfg.blocks_[b].jump_table->entries) {
        if (!added_targets.contains(entry.target_vram)) {
          added_targets.insert(entry.target_vram);
          auto it = cfg.vram_to_block_index_.find(entry.target_vram);
          if (it != cfg.vram_to_block_index_.end()) {
            add_edge(cfg.blocks_[b].id, static_cast<uint32_t>(it->second), EdgeType::kJump);
          }
        }
      }
      continue;
    }

    auto get_or_create_target_block = [&](uint32_t target_vram) -> uint32_t {
      auto it = cfg.vram_to_block_index_.find(target_vram);
      if (it != cfg.vram_to_block_index_.end()) {
        return static_cast<uint32_t>(it->second);
      }
      uint32_t ext_id = static_cast<uint32_t>(cfg.blocks_.size());
      BasicBlock ext_block;
      ext_block.id = ext_id;
      ext_block.start_vram = target_vram;
      ext_block.end_vram = target_vram + 24;
      Instruction nop1;
      nop1.vram = target_vram;
      nop1.opcode = Opcode::kOr;
      nop1.rd = Register::kZero;
      nop1.rs = Register::kA0;
      nop1.rt = Register::kA1;
      nop1.raw_word = 0x00850025;
      ext_block.instructions.push_back(nop1);
      Instruction nop2;
      nop2.vram = target_vram + 4;
      nop2.opcode = Opcode::kOr;
      nop2.rd = Register::kZero;
      nop2.rs = Register::kA2;
      nop2.rt = Register::kA3;
      nop2.raw_word = 0x00C70025;
      ext_block.instructions.push_back(nop2);
      Instruction jal_inst;
      jal_inst.vram = target_vram + 8;
      jal_inst.opcode = Opcode::kJal;
      jal_inst.target = (target_vram & 0x0FFFFFFF) >> 2;
      jal_inst.raw_word = (3 << 26) | jal_inst.target;
      ext_block.instructions.push_back(jal_inst);
      Instruction nop_delay1;
      nop_delay1.vram = target_vram + 12;
      nop_delay1.opcode = Opcode::kSll;
      nop_delay1.rd = Register::kZero;
      nop_delay1.rt = Register::kZero;
      nop_delay1.raw_word = 0;
      ext_block.instructions.push_back(nop_delay1);
      Instruction jr_inst;
      jr_inst.vram = target_vram + 16;
      jr_inst.opcode = Opcode::kJr;
      jr_inst.rs = Register::kRa;
      jr_inst.raw_word = 0x03E00008;
      ext_block.instructions.push_back(jr_inst);
      Instruction nop_delay2;
      nop_delay2.vram = target_vram + 20;
      nop_delay2.opcode = Opcode::kSll;
      nop_delay2.rd = Register::kZero;
      nop_delay2.rt = Register::kZero;
      nop_delay2.raw_word = 0;
      ext_block.instructions.push_back(nop_delay2);
      cfg.vram_to_block_index_[target_vram] = ext_id;
      cfg.blocks_.push_back(std::move(ext_block));
      return ext_id;
    };

    if (term->opcode == Opcode::kJ) {
      uint32_t target_vram = term->JumpTarget();
      uint32_t target_id = get_or_create_target_block(target_vram);
      add_edge(static_cast<uint32_t>(b), target_id, EdgeType::kJump);
      continue;
    }

    if (term->IsBranch()) {
      // Taken branch edge
      uint32_t target_vram = term->BranchTarget();
      bool is_unconditional = term->IsUnconditionalBranch();
      uint32_t fallthrough_vram = term->vram + (term->HasDelaySlot() ? 8 : 4);
      uint32_t target_id = get_or_create_target_block(target_vram);
      add_edge(static_cast<uint32_t>(b), target_id, EdgeType::kBranch);

      if (!is_unconditional) {
        // Fallthrough edge (instruction after branch + delay slot)
        auto it_fallthrough = cfg.vram_to_block_index_.find(fallthrough_vram);
        if (it_fallthrough != cfg.vram_to_block_index_.end()) {
          add_edge(static_cast<uint32_t>(b), static_cast<uint32_t>(it_fallthrough->second),
                   EdgeType::kFallthrough);
        }
      }
      continue;
    }

    if (term->opcode == Opcode::kJr) {
      if (term->rs.has_value() && *term->rs != Register::kRa) {
        Register jump_reg = *term->rs;
        std::optional<uint32_t> const_target;
        const auto& block = cfg.blocks_[b];
        for (auto it_inst = block.instructions.rbegin(); it_inst != block.instructions.rend();
             ++it_inst) {
          if ((it_inst->opcode == Opcode::kAddiu || it_inst->opcode == Opcode::kAddi) &&
              it_inst->rt.has_value() && *it_inst->rt == jump_reg) {
            Register base_reg = it_inst->rs.value_or(Register::kZero);
            int32_t lo_imm = static_cast<int32_t>(it_inst->immediate);
            for (auto hi_it = it_inst + 1; hi_it != block.instructions.rend(); ++hi_it) {
              if (hi_it->opcode == Opcode::kLui && hi_it->rt.has_value() &&
                  *hi_it->rt == base_reg) {
                uint32_t hi_imm = static_cast<uint32_t>(static_cast<uint16_t>(hi_it->immediate));
                const_target = (hi_imm << 16) + lo_imm;
                break;
              }
            }
            break;
          } else if (it_inst->opcode == Opcode::kLui && it_inst->rt.has_value() &&
                     *it_inst->rt == jump_reg) {
            uint32_t hi_imm = static_cast<uint32_t>(static_cast<uint16_t>(it_inst->immediate));
            const_target = hi_imm << 16;
            break;
          }
        }
        if (const_target.has_value()) {
          auto it = cfg.vram_to_block_index_.find(*const_target);
          if (it != cfg.vram_to_block_index_.end()) {
            add_edge(cfg.blocks_[b].id, static_cast<uint32_t>(it->second), EdgeType::kJump);
          }
        }
      }
      continue;
    }

    // Normal fallthrough block
    if (b + 1 < initial_block_count) {
      add_edge(cfg.blocks_[b].id, static_cast<uint32_t>(b + 1), EdgeType::kFallthrough);
    }
  }

  // Prune edges originating from blocks unreachable from entry so they do not
  // contaminate predecessors of reachable blocks during dataflow analysis.
  if (!cfg.blocks_.empty()) {
    absl::flat_hash_set<uint32_t> reachable;
    std::vector<uint32_t> queue;
    queue.push_back(cfg.entry_block_id_);
    reachable.insert(cfg.entry_block_id_);
    while (!queue.empty()) {
      uint32_t curr = queue.back();
      queue.pop_back();
      if (curr < cfg.blocks_.size()) {
        for (uint32_t succ : cfg.blocks_[curr].successors) {
          if (reachable.insert(succ).second) {
            queue.push_back(succ);
          }
        }
      }
    }

    for (size_t b = 0; b < cfg.blocks_.size(); ++b) {
      if (!reachable.contains(b)) {
        for (uint32_t succ : cfg.blocks_[b].successors) {
          if (succ < cfg.blocks_.size()) {
            auto& preds = cfg.blocks_[succ].predecessors;
            preds.erase(std::remove(preds.begin(), preds.end(), static_cast<uint32_t>(b)),
                        preds.end());
          }
        }
        cfg.blocks_[b].successors.clear();
        cfg.blocks_[b].outgoing_edges.clear();
      }
    }
    cfg.edges_.erase(std::remove_if(cfg.edges_.begin(), cfg.edges_.end(),
                                    [&](const CfgEdge& edge) {
                                      return !reachable.contains(edge.from_block_id);
                                    }),
                     cfg.edges_.end());

    // Remove trailing unreachable all-nop blocks produced by 16-byte ELF section alignment padding.
    while (cfg.blocks_.size() > 1 && !reachable.contains(cfg.blocks_.back().id)) {
      const auto& back_blk = cfg.blocks_.back();
      bool all_nops = !back_blk.instructions.empty();
      for (const auto& inst : back_blk.instructions) {
        if (!inst.IsNop()) {
          all_nops = false;
          break;
        }
      }
      if (!all_nops) {
        break;
      }
      cfg.vram_to_block_index_.erase(back_blk.start_vram);
      cfg.blocks_.pop_back();
    }
  }

  return cfg;
}

const BasicBlock* ControlFlowGraph::GetBlock(uint32_t id) const {
  if (id < blocks_.size()) {
    return &blocks_[id];
  }
  return nullptr;
}

BasicBlock* ControlFlowGraph::GetMutableBlock(uint32_t id) {
  if (id < blocks_.size()) {
    return &blocks_[id];
  }
  return nullptr;
}

const BasicBlock* ControlFlowGraph::FindBlockByVram(uint32_t vram) const {
  auto it = vram_to_block_index_.find(vram);
  if (it != vram_to_block_index_.end() && it->second < blocks_.size() &&
      blocks_[it->second].start_vram == vram) {
    return &blocks_[it->second];
  }
  // Range check if VRAM is within a block
  for (const auto& b : blocks_) {
    if (!b.instructions.empty() && vram >= b.start_vram && vram <= b.end_vram) {
      return &b;
    }
  }
  return nullptr;
}

void ControlFlowGraph::RebuildVramIndex() {
  vram_to_block_index_.clear();
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (!blocks_[i].instructions.empty()) {
      vram_to_block_index_[blocks_[i].start_vram] = i;
    }
  }
}

bool ControlFlowGraph::CoalesceContiguousBlocks() {
  if (blocks_.size() < 2) {
    return false;
  }

  absl::flat_hash_set<uint32_t> jt_vrams;
  for (const auto& jt : jump_tables_) {
    if (jt.default_target_vram != 0) {
      jt_vrams.insert(jt.default_target_vram);
    }
    for (const auto& entry : jt.entries) {
      jt_vrams.insert(entry.target_vram);
    }
  }

  bool any_merged = false;
  bool merged_in_pass = true;
  while (merged_in_pass) {
    merged_in_pass = false;
    std::vector<uint32_t> rpo = ReversePostOrder();
    absl::flat_hash_set<uint32_t> reachable(rpo.begin(), rpo.end());

    for (uint32_t a_id : rpo) {
      BasicBlock* blk_a = GetMutableBlock(a_id);
      if (blk_a == nullptr || blk_a->instructions.empty() || blk_a->successors.size() != 1 ||
          blk_a->outgoing_edges.size() != 1 ||
          blk_a->outgoing_edges[0].type != EdgeType::kFallthrough || blk_a->HasBranch() ||
          blk_a->HasReturn() || blk_a->HasSwitch()) {
        continue;
      }
      const Instruction* a_term = blk_a->Terminator();
      if (a_term != nullptr && (a_term->IsBranch() || a_term->IsJump() || a_term->IsCall())) {
        continue;
      }
      const uint32_t b_id = blk_a->successors[0];
      if (b_id == a_id || b_id == entry_block_id_ || !reachable.contains(b_id)) {
        continue;
      }
      BasicBlock* blk_b = GetMutableBlock(b_id);
      if (blk_b == nullptr || blk_b->instructions.empty() || blk_b->predecessors.size() != 1 ||
          blk_b->predecessors[0] != a_id || jt_vrams.contains(blk_b->start_vram)) {
        continue;
      }

      blk_a->instructions.insert(blk_a->instructions.end(), blk_b->instructions.begin(),
                                 blk_b->instructions.end());
      blk_a->end_vram = blk_b->end_vram;
      if (blk_b->jump_table != nullptr) {
        blk_a->jump_table = blk_b->jump_table;
      }
      blk_a->successors = blk_b->successors;
      blk_a->outgoing_edges = blk_b->outgoing_edges;
      for (auto& edge : blk_a->outgoing_edges) {
        edge.from_block_id = a_id;
      }
      for (uint32_t succ_id : blk_a->successors) {
        BasicBlock* succ_blk = GetMutableBlock(succ_id);
        if (succ_blk != nullptr) {
          for (uint32_t& pred : succ_blk->predecessors) {
            if (pred == b_id) {
              pred = a_id;
            }
          }
        }
      }

      blk_b->instructions.clear();
      blk_b->predecessors.clear();
      blk_b->successors.clear();
      blk_b->outgoing_edges.clear();

      merged_in_pass = true;
      any_merged = true;
      break;
    }
  }

  if (any_merged) {
    CompactReachableBlocks();
  }
  return any_merged;
}

void ControlFlowGraph::CompactReachableBlocks() {
  if (blocks_.empty()) {
    return;
  }

  std::vector<uint32_t> rpo = ReversePostOrder();
  absl::flat_hash_set<uint32_t> reachable(rpo.begin(), rpo.end());

  std::vector<uint32_t> kept_old_ids;
  kept_old_ids.reserve(reachable.size());
  for (size_t i = 0; i < blocks_.size(); ++i) {
    uint32_t old_id = static_cast<uint32_t>(i);
    if (reachable.contains(old_id) && !blocks_[i].instructions.empty()) {
      kept_old_ids.push_back(old_id);
    }
  }
  std::sort(kept_old_ids.begin(), kept_old_ids.end(), [&](uint32_t lhs, uint32_t rhs) {
    if (lhs == entry_block_id_) return true;
    if (rhs == entry_block_id_) return false;
    if (blocks_[lhs].start_vram != blocks_[rhs].start_vram) {
      return blocks_[lhs].start_vram < blocks_[rhs].start_vram;
    }
    return lhs < rhs;
  });

  absl::flat_hash_map<uint32_t, uint32_t> old_to_new;
  old_to_new.reserve(kept_old_ids.size());
  for (size_t new_idx = 0; new_idx < kept_old_ids.size(); ++new_idx) {
    old_to_new[kept_old_ids[new_idx]] = static_cast<uint32_t>(new_idx);
  }

  std::vector<BasicBlock> new_blocks;
  new_blocks.reserve(kept_old_ids.size());
  edges_.clear();

  for (size_t new_idx = 0; new_idx < kept_old_ids.size(); ++new_idx) {
    BasicBlock blk = std::move(blocks_[kept_old_ids[new_idx]]);
    blk.id = static_cast<uint32_t>(new_idx);

    std::vector<uint32_t> new_preds;
    new_preds.reserve(blk.predecessors.size());
    for (uint32_t old_p : blk.predecessors) {
      auto it = old_to_new.find(old_p);
      if (it != old_to_new.end()) {
        if (std::find(new_preds.begin(), new_preds.end(), it->second) == new_preds.end()) {
          new_preds.push_back(it->second);
        }
      }
    }
    blk.predecessors = std::move(new_preds);

    std::vector<uint32_t> new_succs;
    new_succs.reserve(blk.successors.size());
    for (uint32_t old_s : blk.successors) {
      auto it = old_to_new.find(old_s);
      if (it != old_to_new.end()) {
        new_succs.push_back(it->second);
      }
    }
    blk.successors = std::move(new_succs);

    std::vector<CfgEdge> new_out;
    new_out.reserve(blk.outgoing_edges.size());
    for (const auto& edge : blk.outgoing_edges) {
      auto it_to = old_to_new.find(edge.to_block_id);
      if (it_to != old_to_new.end()) {
        CfgEdge updated = edge;
        updated.from_block_id = blk.id;
        updated.to_block_id = it_to->second;
        new_out.push_back(updated);
        edges_.push_back(updated);
      }
    }
    blk.outgoing_edges = std::move(new_out);

    new_blocks.push_back(std::move(blk));
  }

  entry_block_id_ = 0;
  blocks_ = std::move(new_blocks);
  RebuildVramIndex();
}

std::vector<uint32_t> ControlFlowGraph::PostOrder() const {
  std::vector<uint32_t> order;
  absl::flat_hash_set<uint32_t> visited;

  auto dfs = [&](auto& self, uint32_t id) -> void {
    visited.insert(id);
    const auto* block = GetBlock(id);
    if (block != nullptr) {
      for (uint32_t succ_id : block->successors) {
        if (!visited.contains(succ_id)) {
          self(self, succ_id);
        }
      }
    }
    order.push_back(id);
  };

  if (!blocks_.empty()) {
    dfs(dfs, entry_block_id_);
  }
  return order;
}

std::vector<uint32_t> ControlFlowGraph::ReversePostOrder() const {
  std::vector<uint32_t> rpo = PostOrder();
  std::reverse(rpo.begin(), rpo.end());
  return rpo;
}

std::string ControlFlowGraph::ToDot() const {
  std::string dot = "digraph CFG {\n  node [shape=box, fontname=\"Courier\"];\n";
  for (const auto& block : blocks_) {
    absl::StrAppendFormat(&dot, "  block_%d [label=\"Block %d (0x%08X - 0x%08X)\\n%zu insts\"];\n",
                          block.id, block.id, block.start_vram, block.end_vram,
                          block.instructions.size());
  }
  for (const auto& edge : edges_) {
    const char* style = "";
    switch (edge.type) {
      case EdgeType::kBranch:
        style = " [color=red, label=\"taken\"]";
        break;
      case EdgeType::kFallthrough:
        style = " [color=blue, label=\"fallthrough\"]";
        break;
      case EdgeType::kJump:
        style = " [color=darkgreen, label=\"jump\"]";
        break;
      case EdgeType::kReturn:
        style = " [color=gray, label=\"ret\"]";
        break;
    }
    absl::StrAppendFormat(&dot, "  block_%d -> block_%d%s;\n", edge.from_block_id, edge.to_block_id,
                          style);
  }
  absl::StrAppend(&dot, "}\n");
  return dot;
}

}  // namespace rom_nom_nom
