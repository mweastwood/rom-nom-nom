#include "lifter/control_flow_structurer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "lifter/control_flow_graph.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

namespace {

std::unique_ptr<StructuredRegion> CreateBlock(uint32_t block_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kBlock;
  r->block_id = block_id;
  return r;
}

std::unique_ptr<StructuredRegion> CreateBreak(uint32_t target_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kBreak;
  r->block_id = target_id;
  return r;
}

std::unique_ptr<StructuredRegion> CreateContinue(uint32_t target_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kContinue;
  r->block_id = target_id;
  return r;
}

std::unique_ptr<StructuredRegion> CreateGoto(uint32_t target_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kGoto;
  r->block_id = target_id;
  return r;
}

std::unique_ptr<StructuredRegion> CreateSequence() {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kSequence;
  return r;
}

std::unique_ptr<StructuredRegion> CreateIfThen(uint32_t condition_block_id, bool invert = false) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kIfThen;
  r->condition_block_id = condition_block_id;
  r->invert_condition = invert;
  return r;
}

std::unique_ptr<StructuredRegion> CreateIfThenElse(uint32_t condition_block_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kIfThenElse;
  r->condition_block_id = condition_block_id;
  return r;
}

std::unique_ptr<StructuredRegion> CreateLoop(LoopType type, uint32_t header) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kLoop;
  r->loop_type = type;
  r->loop_header = header;
  return r;
}

std::unique_ptr<StructuredRegion> CreateSwitch(uint32_t block_id, const JumpTable* jt) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kSwitch;
  r->switch_block_id = block_id;
  r->jump_table = jt;
  return r;
}

}  // namespace

std::string StructuredRegion::ToString(int indent) const {
  std::string ind(indent * 2, ' ');
  switch (type) {
    case RegionType::kBlock:
      return ind + "block_" + std::to_string(block_id) + "\n";
    case RegionType::kBreak:
      return ind + "break\n";
    case RegionType::kContinue:
      return ind + "continue\n";
    case RegionType::kGoto:
      return ind + "goto block_" + std::to_string(block_id) + "\n";
    case RegionType::kSequence: {
      std::string out;
      for (const auto& child : children) {
        if (child) {
          out += child->ToString(indent);
        }
      }
      return out;
    }
    case RegionType::kIfThen: {
      std::string out = ind + "if (" + (invert_condition ? "!" : "") + "block_" +
                        std::to_string(condition_block_id) + ") {\n";
      if (!children.empty() && children[0]) {
        out += children[0]->ToString(indent + 1);
      }
      out += ind + "}\n";
      return out;
    }
    case RegionType::kIfThenElse: {
      std::string out = ind + "if (" + (invert_condition ? "!" : "") + "block_" +
                        std::to_string(condition_block_id) + ") {\n";
      if (!children.empty() && children[0]) {
        out += children[0]->ToString(indent + 1);
      }
      out += ind + "} else {\n";
      if (children.size() > 1 && children[1]) {
        out += children[1]->ToString(indent + 1);
      }
      out += ind + "}\n";
      return out;
    }
    case RegionType::kLoop: {
      std::string out;
      if (loop_type == LoopType::kDoWhile) {
        out = ind + "do {\n";
        if (!children.empty() && children[0]) {
          out += children[0]->ToString(indent + 1);
        }
        uint32_t cond_id = condition_block_id != 0 ? condition_block_id : loop_header;
        out += ind + "} while (block_" + std::to_string(cond_id) + ");\n";
      } else if (loop_type == LoopType::kInfinite) {
        out = ind + "while (1) {\n";
        if (!children.empty() && children[0]) {
          out += children[0]->ToString(indent + 1);
        }
        out += ind + "}\n";
      } else {
        out = ind + "while (block_" + std::to_string(loop_header) + ") {\n";
        if (!children.empty() && children[0]) {
          out += children[0]->ToString(indent + 1);
        }
        out += ind + "}\n";
      }
      return out;
    }
    case RegionType::kSwitch: {
      std::string out = ind + "switch (block_" + std::to_string(switch_block_id) + ") {\n";
      for (const auto& sc : cases) {
        for (int64_t val : sc.case_values) {
          out += ind + "  case " + std::to_string(val) + ":\n";
        }
        if (sc.is_default) {
          out += ind + "  default:\n";
        }
        if (sc.body) {
          out += sc.body->ToString(indent + 2);
        }
        out += ind + "    break;\n";
      }
      out += ind + "}\n";
      return out;
    }
  }
  return "";
}

void ControlFlowStructurer::FlattenSequence(StructuredRegion* seq,
                                            std::unique_ptr<StructuredRegion> region) {
  if (!region) return;
  if (region->type == RegionType::kSequence) {
    for (auto& child : region->children) {
      if (child) {
        seq->children.push_back(std::move(child));
      }
    }
  } else {
    seq->children.push_back(std::move(region));
  }
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::Structure(
    const ControlFlowGraph& cfg, const DominatorTree& dom_tree, const DominatorTree& post_dom_tree,
    const LoopInfo& loop_info) {
  if (cfg.Blocks().empty()) {
    return nullptr;
  }

  ControlFlowStructurer structurer;
  structurer.cfg_ = &cfg;
  structurer.dom_tree_ = &dom_tree;
  structurer.post_dom_tree_ = &post_dom_tree;
  structurer.loop_info_ = &loop_info;

  return structurer.StructureRegion(cfg.EntryBlockId(), std::nullopt, nullptr);
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::StructureLoop(
    const Loop* loop, std::optional<uint32_t> follow, const Loop* parent_loop) {
  auto loop_region = CreateLoop(loop->type, loop->header);

  std::optional<uint32_t> loop_follow;
  if (loop->exit_blocks.size() == 1) {
    loop_follow = loop->exit_blocks[0];
  } else if (!loop->exit_blocks.empty()) {
    loop_follow = post_dom_tree_->ImmediateDominator(loop->header);
  }

  if (loop->type == LoopType::kWhile) {
    loop_region->condition_block_id = loop->header;
    const auto* header_block = cfg_->GetBlock(loop->header);
    if (header_block != nullptr && header_block->successors.size() == 2) {
      uint32_t body_start = 0;
      for (uint32_t succ : header_block->successors) {
        if (loop->Contains(succ)) {
          body_start = succ;
          break;
        }
      }

      if (body_start != 0) {
        visited_.insert(loop->header);
        auto body = StructureRegion(body_start, loop->header, loop);
        if (body) {
          loop_region->children.push_back(std::move(body));
        }
      }
    }
  } else if (loop->type == LoopType::kDoWhile) {
    uint32_t latch_id = 0;
    for (const auto& edge : loop->exit_edges) {
      if (loop->IsLatch(edge.from_block_id)) {
        latch_id = edge.from_block_id;
        break;
      }
    }
    if (latch_id == 0 && !loop->latches.empty()) {
      latch_id = loop->latches[0];
    }
    if (latch_id == 0) {
      latch_id = loop->header;
    }
    loop_region->condition_block_id = latch_id;

    if (loop->blocks.size() == 1) {
      loop_region->children.push_back(CreateBlock(loop->header));
      visited_.insert(loop->header);
    } else {
      auto body = StructureRegion(loop->header, loop_follow, loop);
      if (body) {
        loop_region->children.push_back(std::move(body));
      }
    }
  } else {
    // Infinite loop
    const auto* header_block = cfg_->GetBlock(loop->header);
    if (header_block != nullptr && header_block->successors.size() == 1 &&
        header_block->successors[0] == loop->header) {
      loop_region->children.push_back(CreateBlock(loop->header));
      visited_.insert(loop->header);
    } else {
      auto body = StructureRegion(loop->header, loop_follow, loop);
      if (body) {
        loop_region->children.push_back(std::move(body));
      }
    }
  }

  if (loop_follow.has_value() && loop_follow != follow) {
    auto after_loop = StructureRegion(*loop_follow, follow, parent_loop);
    if (after_loop) {
      auto seq = CreateSequence();
      seq->children.push_back(std::move(loop_region));
      FlattenSequence(seq.get(), std::move(after_loop));
      return seq;
    }
  }

  return loop_region;
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::StructureRegion(
    uint32_t entry, std::optional<uint32_t> follow, const Loop* current_loop) {
  if (follow.has_value() && entry == *follow) {
    return nullptr;
  }

  // Handle loop back-edges and exits
  if (current_loop != nullptr) {
    if (visited_.contains(entry) && entry == current_loop->header) {
      return CreateContinue(entry);
    }
    if (current_loop->IsExitBlock(entry)) {
      return CreateBreak(entry);
    }
    if (current_loop->type == LoopType::kDoWhile && current_loop->IsLatch(entry)) {
      visited_.insert(entry);
      return CreateBlock(entry);
    }
  }

  if (visited_.contains(entry)) {
    return CreateGoto(entry);
  }

  // Check if entry is the header of a new loop
  const Loop* loop = loop_info_->GetLoopFor(entry);
  if (loop != nullptr && loop != current_loop && loop->header == entry) {
    return StructureLoop(loop, follow, current_loop);
  }

  visited_.insert(entry);
  const auto* block = cfg_->GetBlock(entry);
  if (block == nullptr) {
    return nullptr;
  }

  // Case: Multi-way branch (Switch statement)
  const JumpTable* switch_jt = nullptr;
  uint32_t switch_dispatch_block_id = entry;
  bool has_bounds_block = false;

  if (block->jump_table != nullptr) {
    switch_jt = block->jump_table;
    switch_dispatch_block_id = entry;
  } else if (block->successors.size() == 2) {
    uint32_t succ_fallthrough = 0;
    for (const auto& edge : block->outgoing_edges) {
      if (edge.type == EdgeType::kFallthrough) {
        succ_fallthrough = edge.to_block_id;
        break;
      }
    }
    const auto* fallthrough_blk = cfg_->GetBlock(succ_fallthrough);
    if (fallthrough_blk != nullptr && fallthrough_blk->jump_table != nullptr) {
      switch_jt = fallthrough_blk->jump_table;
      switch_dispatch_block_id = succ_fallthrough;
      has_bounds_block = true;
    }
  }

  if (switch_jt != nullptr) {
    std::optional<uint32_t> join;
    if (post_dom_tree_ != nullptr) {
      join = post_dom_tree_->ImmediateDominator(entry);
      if (join.has_value() && (*join == entry || *join == switch_dispatch_block_id)) {
        join = post_dom_tree_->ImmediateDominator(*join);
      }
    }
    if (follow.has_value() && (!join.has_value() || *join == *follow)) {
      join = follow;
    }

    if (has_bounds_block) {
      visited_.insert(switch_dispatch_block_id);
    }

    auto switch_region = CreateSwitch(switch_dispatch_block_id, switch_jt);

    // Group jump table entries by target block ID
    std::vector<uint32_t> unique_target_blocks;
    absl::flat_hash_map<uint32_t, std::vector<int64_t>> target_to_cases;

    for (const auto& entry_item : switch_jt->entries) {
      const auto* target_blk = cfg_->FindBlockByVram(entry_item.target_vram);
      if (target_blk != nullptr) {
        uint32_t blk_id = target_blk->id;
        if (!target_to_cases.contains(blk_id)) {
          unique_target_blocks.push_back(blk_id);
        }
        target_to_cases[blk_id].push_back(entry_item.case_value);
      }
    }

    // Default target
    const auto* def_blk = cfg_->FindBlockByVram(switch_jt->default_target_vram);
    uint32_t def_blk_id = (def_blk != nullptr) ? def_blk->id : 0;
    bool has_explicit_default = false;
    if (def_blk != nullptr && (!join.has_value() || def_blk_id != *join)) {
      has_explicit_default = true;
      if (!target_to_cases.contains(def_blk_id)) {
        unique_target_blocks.push_back(def_blk_id);
      }
    }

    for (uint32_t target_blk_id : unique_target_blocks) {
      StructuredCase sc;
      auto it_cases = target_to_cases.find(target_blk_id);
      if (it_cases != target_to_cases.end()) {
        sc.case_values = it_cases->second;
      }
      if (has_explicit_default && target_blk_id == def_blk_id) {
        sc.is_default = true;
      }
      if (!join.has_value() || target_blk_id != *join) {
        sc.body = StructureRegion(target_blk_id, join, current_loop);
      }
      switch_region->cases.push_back(std::move(sc));
    }

    if (join.has_value() && join != follow) {
      auto after_switch = StructureRegion(*join, follow, current_loop);
      if (after_switch != nullptr) {
        auto seq = CreateSequence();
        seq->children.push_back(std::move(switch_region));
        FlattenSequence(seq.get(), std::move(after_switch));
        return seq;
      }
    }

    return switch_region;
  }

  // Case 1: Return or trap (0 successors)
  if (block->successors.empty()) {
    return CreateBlock(entry);
  }

  // Case 2: Linear sequence (1 successor)
  if (block->successors.size() == 1) {
    uint32_t succ = block->successors[0];
    auto block_region = CreateBlock(entry);

    if (follow.has_value() && succ == *follow) {
      return block_region;
    }
    if (current_loop != nullptr) {
      if (succ == current_loop->header) {
        return block_region;
      }
      if (current_loop->IsExitBlock(succ)) {
        auto seq = CreateSequence();
        seq->children.push_back(std::move(block_region));
        seq->children.push_back(CreateBreak(succ));
        return seq;
      }
    }

    auto next_region = StructureRegion(succ, follow, current_loop);
    if (next_region == nullptr) {
      return block_region;
    }

    auto seq = CreateSequence();
    seq->children.push_back(std::move(block_region));
    FlattenSequence(seq.get(), std::move(next_region));
    return seq;
  }

  // Case 3: Conditional branch (2 successors)
  if (block->successors.size() == 2) {
    uint32_t succ_taken = 0;
    uint32_t succ_fallthrough = 0;

    for (const auto& edge : block->outgoing_edges) {
      if (edge.type == EdgeType::kBranch) {
        succ_taken = edge.to_block_id;
      } else {
        succ_fallthrough = edge.to_block_id;
      }
    }
    if (succ_taken == 0 && succ_fallthrough == 0) {
      succ_taken = block->successors[0];
      succ_fallthrough = block->successors[1];
    }

    // Determine join point using post-dominators
    std::optional<uint32_t> join = post_dom_tree_->ImmediateDominator(entry);
    if (follow.has_value() && (!join.has_value() || *join == *follow)) {
      join = follow;
    }

    // Subcase 3a: If-Then (one successor directly is join)
    if (join.has_value() && succ_fallthrough == *join) {
      auto if_then = CreateIfThen(entry, /*invert=*/false);
      auto then_body = StructureRegion(succ_taken, join, current_loop);
      if (then_body) {
        if_then->children.push_back(std::move(then_body));
      }

      auto after_join = StructureRegion(*join, follow, current_loop);
      if (after_join) {
        auto seq = CreateSequence();
        seq->children.push_back(std::move(if_then));
        FlattenSequence(seq.get(), std::move(after_join));
        return seq;
      }
      return if_then;
    } else if (join.has_value() && succ_taken == *join) {
      auto if_then = CreateIfThen(entry, /*invert=*/true);
      auto then_body = StructureRegion(succ_fallthrough, join, current_loop);
      if (then_body) {
        if_then->children.push_back(std::move(then_body));
      }

      auto after_join = StructureRegion(*join, follow, current_loop);
      if (after_join) {
        auto seq = CreateSequence();
        seq->children.push_back(std::move(if_then));
        FlattenSequence(seq.get(), std::move(after_join));
        return seq;
      }
      return if_then;
    } else {
      // Subcase 3b: If-Then-Else
      auto if_else = CreateIfThenElse(entry);
      auto then_body = StructureRegion(succ_taken, join, current_loop);
      auto else_body = StructureRegion(succ_fallthrough, join, current_loop);

      if_else->children.push_back(then_body ? std::move(then_body) : CreateSequence());
      if_else->children.push_back(else_body ? std::move(else_body) : CreateSequence());

      if (join.has_value() && join != follow) {
        auto after_join = StructureRegion(*join, follow, current_loop);
        if (after_join) {
          auto seq = CreateSequence();
          seq->children.push_back(std::move(if_else));
          FlattenSequence(seq.get(), std::move(after_join));
          return seq;
        }
      }
      return if_else;
    }
  }

  // Fallback for switch / multi-way branches
  return CreateBlock(entry);
}

}  // namespace rom_nom_nom
