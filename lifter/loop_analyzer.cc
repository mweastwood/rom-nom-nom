#include "lifter/loop_analyzer.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "lifter/dominator_tree.h"

namespace rom_nom_nom {

namespace {

// Returns the branching predecessor block ID if `latch_id` is an unconditional
// jump trampoline back to `header_id`.
std::optional<uint32_t> DetectTrampolineBranchPredecessor(const ControlFlowGraph& cfg,
                                                          uint32_t latch_id, uint32_t header_id) {
  const auto* lb = cfg.GetBlock(latch_id);
  if (lb == nullptr || lb->instructions.size() > 2) {
    return std::nullopt;
  }

  const auto* term = lb->Terminator();
  const auto* hb = cfg.GetBlock(header_id);
  if (term == nullptr || term->opcode != Opcode::kJ || hb == nullptr) {
    return std::nullopt;
  }

  bool targets_header = (term->JumpTarget() == hb->start_vram) ||
                        (!lb->successors.empty() && lb->successors[0] == header_id);
  if (!targets_header || lb->predecessors.size() != 1) {
    return std::nullopt;
  }

  uint32_t pred_id = lb->predecessors[0];
  if (pred_id == header_id) {
    return std::nullopt;
  }

  const auto* pb = cfg.GetBlock(pred_id);
  if (pb == nullptr || pb->Terminator() == nullptr || !pb->Terminator()->IsBranch()) {
    return std::nullopt;
  }

  return pred_id;
}

}  // namespace

bool Loop::IsLatch(uint32_t block_id) const {
  return std::find(latches.begin(), latches.end(), block_id) != latches.end();
}

bool Loop::IsExitBlock(uint32_t block_id) const {
  return std::find(exit_blocks.begin(), exit_blocks.end(), block_id) != exit_blocks.end();
}

LoopInfo LoopInfo::Analyze(const ControlFlowGraph& cfg, const DominatorTree& dom_tree) {
  LoopInfo info;
  if (cfg.Blocks().empty()) {
    return info;
  }

  // 1. Identify all back-edges (u -> v where v dominates u).
  // Group back-edges by header.
  absl::flat_hash_map<uint32_t, std::vector<uint32_t>> header_to_latches;
  for (const auto& block : cfg.Blocks()) {
    if (!dom_tree.IsReachable(block.id)) {
      continue;
    }
    for (uint32_t succ : block.successors) {
      if (!dom_tree.IsReachable(succ)) {
        continue;
      }
      if (dom_tree.Dominates(succ, block.id)) {
        header_to_latches[succ].push_back(block.id);
      }
    }
  }

  if (header_to_latches.empty()) {
    return info;
  }

  // 2. Build natural loop for each header.
  for (auto& [header, latches] : header_to_latches) {
    std::vector<uint32_t> trampolines;
    for (uint32_t& latch : latches) {
      if (auto pred = DetectTrampolineBranchPredecessor(cfg, latch, header)) {
        trampolines.push_back(latch);
        latch = *pred;
      }
    }
    std::sort(latches.begin(), latches.end());
    latches.erase(std::unique(latches.begin(), latches.end()), latches.end());
    auto loop = std::make_unique<Loop>();
    loop->header = header;
    loop->latches = latches;
    loop->blocks.insert(header);
    for (uint32_t t : trampolines) {
      loop->blocks.insert(t);
    }

    // Backward reachability from all latches to header
    std::vector<uint32_t> worklist;
    for (uint32_t latch : latches) {
      if (loop->blocks.insert(latch).second) {
        worklist.push_back(latch);
      }
    }

    while (!worklist.empty()) {
      uint32_t curr = worklist.back();
      worklist.pop_back();

      const auto* block = cfg.GetBlock(curr);
      if (block == nullptr) {
        continue;
      }

      for (uint32_t pred : block->predecessors) {
        if (!dom_tree.IsReachable(pred)) {
          continue;
        }
        if (loop->blocks.insert(pred).second) {
          worklist.push_back(pred);
        }
      }
    }

    // Identify exit edges and exit blocks
    absl::flat_hash_set<uint32_t> exit_set;
    for (uint32_t b : loop->blocks) {
      const auto* block = cfg.GetBlock(b);
      if (block == nullptr) {
        continue;
      }
      for (const auto& edge : block->outgoing_edges) {
        if (!loop->blocks.contains(edge.to_block_id)) {
          loop->exit_edges.push_back(edge);
          exit_set.insert(edge.to_block_id);
        }
      }
    }

    loop->exit_blocks.assign(exit_set.begin(), exit_set.end());
    std::sort(loop->exit_blocks.begin(), loop->exit_blocks.end());

    // Classify loop type
    if (loop->exit_edges.empty()) {
      loop->type = LoopType::kInfinite;
    } else if (loop->blocks.size() == 1) {
      // Single-block loop with exit: executes body instructions before
      // the branch at the latch, which is characteristic of do-while.
      loop->type = LoopType::kDoWhile;
    } else {
      bool latch_exits =
          std::any_of(loop->exit_edges.begin(), loop->exit_edges.end(),
                      [&](const auto& edge) { return loop->IsLatch(edge.from_block_id); });
      bool header_exits =
          std::any_of(loop->exit_edges.begin(), loop->exit_edges.end(),
                      [&](const auto& edge) { return edge.from_block_id == loop->header; });

      if (latch_exits) {
        loop->type = LoopType::kDoWhile;
      } else if (header_exits) {
        loop->type = LoopType::kWhile;
      } else {
        loop->type = LoopType::kInfinite;
      }
    }

    info.all_loops_.push_back(std::move(loop));
  }

  // Sort loops deterministically by header ID
  std::sort(info.all_loops_.begin(), info.all_loops_.end(),
            [](const std::unique_ptr<Loop>& a, const std::unique_ptr<Loop>& b) {
              return a->header < b->header;
            });

  for (size_t i = 0; i < info.all_loops_.size(); ++i) {
    info.all_loops_[i]->id = static_cast<uint32_t>(i);
  }

  // 3. Build loop nesting hierarchy.
  // Loop A is sub-loop of B if A.blocks is a strict subset of B.blocks.
  // Immediate parent is the smallest containing loop.
  for (size_t i = 0; i < info.all_loops_.size(); ++i) {
    Loop* a = info.all_loops_[i].get();
    Loop* best_parent = nullptr;

    for (size_t j = 0; j < info.all_loops_.size(); ++j) {
      if (i == j) continue;
      Loop* b = info.all_loops_[j].get();

      // Check if A.blocks is a subset of B.blocks
      if (a->blocks.size() < b->blocks.size()) {
        bool is_subset = true;
        for (uint32_t blk : a->blocks) {
          if (!b->blocks.contains(blk)) {
            is_subset = false;
            break;
          }
        }
        if (is_subset) {
          if (best_parent == nullptr || b->blocks.size() < best_parent->blocks.size()) {
            best_parent = b;
          }
        }
      }
    }

    a->parent = best_parent;
    if (best_parent != nullptr) {
      best_parent->sub_loops.push_back(a);
    } else {
      info.top_level_loops_.push_back(a);
    }
  }

  // Sort sub_loops for determinism
  for (const auto& loop : info.all_loops_) {
    std::sort(loop->sub_loops.begin(), loop->sub_loops.end(),
              [](const Loop* a, const Loop* b) { return a->header < b->header; });
  }

  // 4. Compute loop depths
  auto set_depth = [](auto& self, Loop* loop, uint32_t depth) -> void {
    loop->depth = depth;
    for (Loop* sub : loop->sub_loops) {
      self(self, sub, depth + 1);
    }
  };
  for (Loop* top : info.top_level_loops_) {
    set_depth(set_depth, top, 1);
  }

  // 5. Populate block_to_innermost_loop_ mapping
  for (const auto& block : cfg.Blocks()) {
    Loop* innermost = nullptr;
    for (const auto& loop : info.all_loops_) {
      if (loop->Contains(block.id)) {
        if (innermost == nullptr || loop->depth > innermost->depth) {
          innermost = loop.get();
        }
      }
    }
    if (innermost != nullptr) {
      info.block_to_innermost_loop_[block.id] = innermost;
    }
  }

  return info;
}

const Loop* LoopInfo::GetLoopFor(uint32_t block_id) const {
  auto it = block_to_innermost_loop_.find(block_id);
  if (it != block_to_innermost_loop_.end()) {
    return it->second;
  }
  return nullptr;
}

uint32_t LoopInfo::LoopDepth(uint32_t block_id) const {
  const Loop* loop = GetLoopFor(block_id);
  return loop != nullptr ? loop->depth : 0;
}

}  // namespace rom_nom_nom
