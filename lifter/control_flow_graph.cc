#include "lifter/control_flow_graph.h"

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

absl::StatusOr<ControlFlowGraph> ControlFlowGraph::Build(
    absl::Span<const Instruction> instructions) {
  if (instructions.empty()) {
    return absl::InvalidArgumentError("ControlFlowGraph: cannot build from empty instructions.");
  }

  ControlFlowGraph cfg;

  // 1. Index instructions by VRAM address.
  absl::flat_hash_map<uint32_t, size_t> vram_to_idx;
  vram_to_idx.reserve(instructions.size());
  for (size_t i = 0; i < instructions.size(); ++i) {
    vram_to_idx[instructions[i].vram] = i;
  }

  // 2. Identify basic block leaders.
  std::vector<bool> is_leader(instructions.size(), false);
  is_leader[0] = true;

  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& inst = instructions[i];
    if (inst.IsBranch()) {
      uint32_t target = inst.BranchTarget();
      auto it = vram_to_idx.find(target);
      if (it != vram_to_idx.end()) {
        is_leader[it->second] = true;
      }
      size_t next_idx = inst.HasDelaySlot() ? (i + 2) : (i + 1);
      if (next_idx < instructions.size()) {
        is_leader[next_idx] = true;
      }
    } else if (inst.IsJump()) {
      if (inst.opcode == Opcode::kJ) {
        uint32_t target = inst.JumpTarget();
        auto it = vram_to_idx.find(target);
        if (it != vram_to_idx.end()) {
          is_leader[it->second] = true;
        }
      }
      size_t next_idx = inst.HasDelaySlot() ? (i + 2) : (i + 1);
      if (next_idx < instructions.size()) {
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

  for (size_t b = 0; b < cfg.blocks_.size(); ++b) {
    auto& block = cfg.blocks_[b];
    const auto* term = block.Terminator();
    if (term == nullptr) continue;

    if (term->IsReturn()) {
      // Function exit block: no outgoing edges.
      continue;
    }

    if (term->opcode == Opcode::kJ) {
      uint32_t target_vram = term->JumpTarget();
      auto it = cfg.vram_to_block_index_.find(target_vram);
      if (it != cfg.vram_to_block_index_.end()) {
        add_edge(block.id, static_cast<uint32_t>(it->second), EdgeType::kJump);
      }
      continue;
    }

    if (term->IsBranch()) {
      // Taken branch edge
      uint32_t target_vram = term->BranchTarget();
      auto it_target = cfg.vram_to_block_index_.find(target_vram);
      if (it_target != cfg.vram_to_block_index_.end()) {
        add_edge(block.id, static_cast<uint32_t>(it_target->second), EdgeType::kBranch);
      }

      // Fallthrough edge (instruction after branch + delay slot)
      uint32_t fallthrough_vram = term->vram + (term->HasDelaySlot() ? 8 : 4);
      auto it_fallthrough = cfg.vram_to_block_index_.find(fallthrough_vram);
      if (it_fallthrough != cfg.vram_to_block_index_.end()) {
        add_edge(block.id, static_cast<uint32_t>(it_fallthrough->second), EdgeType::kFallthrough);
      }
      continue;
    }

    // Normal fallthrough block
    if (b + 1 < cfg.blocks_.size()) {
      add_edge(block.id, static_cast<uint32_t>(b + 1), EdgeType::kFallthrough);
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

const BasicBlock* ControlFlowGraph::FindBlockByVram(uint32_t vram) const {
  auto it = vram_to_block_index_.find(vram);
  if (it != vram_to_block_index_.end()) {
    return &blocks_[it->second];
  }
  // Range check if VRAM is within a block
  for (const auto& b : blocks_) {
    if (vram >= b.start_vram && vram <= b.end_vram) {
      return &b;
    }
  }
  return nullptr;
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
