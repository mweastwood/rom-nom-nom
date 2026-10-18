#include "lifter/control_flow_structurer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
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

std::unique_ptr<StructuredRegion> CreateIfThenElse(uint32_t condition_block_id,
                                                   bool invert = false) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kIfThenElse;
  r->condition_block_id = condition_block_id;
  r->invert_condition = invert;
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

bool IsPureConditionBlock(const ControlFlowGraph& cfg, uint32_t block_id) {
  const auto* b = cfg.GetBlock(block_id);
  if (b == nullptr || b->instructions.empty() || b->instructions.size() > 3) return false;
  for (const auto& inst : b->instructions) {
    if (inst.IsCall() || inst.IsStore() || inst.IsLoad()) return false;
    if (inst.IsBranch() || inst.IsNop()) continue;
    if (inst.rd.has_value() && *inst.rd != Register::kZero && *inst.rd != Register::kAt) {
      return false;
    }
    if (inst.rt.has_value() && *inst.rt != Register::kZero && *inst.rt != Register::kAt) {
      return false;
    }
  }
  return true;
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
      std::string cond_str =
          std::string(invert_condition ? "!" : "") + "block_" + std::to_string(condition_block_id);
      if (!condition_op.empty()) {
        for (size_t i = 0; i < additional_condition_blocks.size(); ++i) {
          cond_str += " " + condition_op + " ";
          if (i < additional_invert_conditions.size() && additional_invert_conditions[i]) {
            cond_str += "!";
          }
          cond_str += "block_" + std::to_string(additional_condition_blocks[i]);
        }
      }
      std::string out = ind + "if (" + cond_str + ") {\n";
      if (!children.empty() && children[0]) {
        out += children[0]->ToString(indent + 1);
      }
      out += ind + "}\n";
      return out;
    }
    case RegionType::kIfThenElse: {
      std::string cond_str =
          std::string(invert_condition ? "!" : "") + "block_" + std::to_string(condition_block_id);
      if (!condition_op.empty()) {
        for (size_t i = 0; i < additional_condition_blocks.size(); ++i) {
          cond_str += " " + condition_op + " ";
          if (i < additional_invert_conditions.size() && additional_invert_conditions[i]) {
            cond_str += "!";
          }
          cond_str += "block_" + std::to_string(additional_condition_blocks[i]);
        }
      }
      std::string out = ind + "if (" + cond_str + ") {\n";
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
  } else {
    if (loop->type == LoopType::kDoWhile) {
      for (const auto& edge : loop->exit_edges) {
        if (loop->IsLatch(edge.from_block_id)) {
          loop_follow = edge.to_block_id;
          break;
        }
      }
    } else if (loop->type == LoopType::kWhile) {
      for (const auto& edge : loop->exit_edges) {
        if (edge.from_block_id == loop->header) {
          loop_follow = edge.to_block_id;
          break;
        }
      }
    }
    if (!loop_follow.has_value() && !loop->exit_blocks.empty() && post_dom_tree_ != nullptr) {
      std::optional<uint32_t> cand = post_dom_tree_->ImmediateDominator(loop->header);
      while (cand.has_value() && loop->Contains(*cand)) {
        cand = post_dom_tree_->ImmediateDominator(*cand);
      }
      loop_follow = cand;
    }
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

      bool branch_targets_body = false;
      for (const auto& edge : header_block->outgoing_edges) {
        if (edge.type == EdgeType::kBranch && loop->Contains(edge.to_block_id)) {
          branch_targets_body = true;
          break;
        }
      }
      loop_region->invert_condition = !branch_targets_body;

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

    bool branch_targets_body = false;
    const auto* latch_block = cfg_->GetBlock(latch_id);
    if (latch_block != nullptr) {
      for (const auto& edge : latch_block->outgoing_edges) {
        if (edge.type == EdgeType::kBranch && loop->Contains(edge.to_block_id)) {
          branch_targets_body = true;
          break;
        }
      }
    }
    loop_region->invert_condition = !branch_targets_body;

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
      if (follow.has_value() && entry == *follow) {
        return CreateBreak(entry);
      }
      auto exit_body = StructureRegion(entry, follow, current_loop->parent);
      if (exit_body) {
        auto seq = CreateSequence();
        seq->children.push_back(std::move(exit_body));
        seq->children.push_back(CreateBreak(entry));
        return seq;
      }
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
    if (def_blk != nullptr) {
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

    std::unique_ptr<StructuredRegion> final_region;
    if (has_bounds_block) {
      auto seq = CreateSequence();
      seq->children.push_back(CreateBlock(entry));
      seq->children.push_back(std::move(switch_region));
      final_region = std::move(seq);
    } else {
      final_region = std::move(switch_region);
    }

    if (join.has_value() && join != follow) {
      auto after_switch = StructureRegion(*join, follow, current_loop);
      if (after_switch != nullptr) {
        auto seq = CreateSequence();
        FlattenSequence(seq.get(), std::move(final_region));
        FlattenSequence(seq.get(), std::move(after_switch));
        return seq;
      }
    }

    return final_region;
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

    // Case 3-break: If inside a loop and one successor is a loop exit before the latch
    if (current_loop != nullptr && !current_loop->IsLatch(entry)) {
      if (current_loop->IsExitBlock(succ_taken) && !current_loop->IsExitBlock(succ_fallthrough)) {
        auto if_break = CreateIfThen(entry, /*invert=*/false);
        if (follow.has_value() && succ_taken == *follow) {
          if_break->children.push_back(CreateBreak(succ_taken));
        } else {
          auto exit_body = StructureRegion(succ_taken, follow, current_loop->parent);
          if (exit_body) {
            auto seq = CreateSequence();
            seq->children.push_back(std::move(exit_body));
            seq->children.push_back(CreateBreak(succ_taken));
            if_break->children.push_back(std::move(seq));
          } else {
            if_break->children.push_back(CreateBreak(succ_taken));
          }
        }
        auto next = StructureRegion(succ_fallthrough, follow, current_loop);
        if (next) {
          auto seq = CreateSequence();
          seq->children.push_back(std::move(if_break));
          FlattenSequence(seq.get(), std::move(next));
          return seq;
        }
        return if_break;
      }
      if (current_loop->IsExitBlock(succ_fallthrough) && !current_loop->IsExitBlock(succ_taken)) {
        auto if_break = CreateIfThen(entry, /*invert=*/true);
        if (follow.has_value() && succ_fallthrough == *follow) {
          if_break->children.push_back(CreateBreak(succ_fallthrough));
        } else {
          auto exit_body = StructureRegion(succ_fallthrough, follow, current_loop->parent);
          if (exit_body) {
            auto seq = CreateSequence();
            seq->children.push_back(std::move(exit_body));
            seq->children.push_back(CreateBreak(succ_fallthrough));
            if_break->children.push_back(std::move(seq));
          } else {
            if_break->children.push_back(CreateBreak(succ_fallthrough));
          }
        }
        auto next = StructureRegion(succ_taken, follow, current_loop);
        if (next) {
          auto seq = CreateSequence();
          seq->children.push_back(std::move(if_break));
          FlattenSequence(seq.get(), std::move(next));
          return seq;
        }
        return if_break;
      }
    }

    // Determine join point using post-dominators
    std::optional<uint32_t> join = post_dom_tree_->ImmediateDominator(entry);
    if (current_loop != nullptr && current_loop->Contains(entry)) {
      if (!join.has_value() || !current_loop->Contains(*join)) {
        if (!current_loop->latches.empty() && current_loop->latches[0] != entry) {
          join = current_loop->latches[0];
        }
      }
    }
    if (follow.has_value() && (!join.has_value() || *join == *follow)) {
      join = follow;
    }

    // Short-Circuit OR (||): entry branches to succ_taken; succ_fallthrough also reaches succ_taken
    if (!visited_.contains(succ_fallthrough)) {
      const auto* b_fallthrough = cfg_->GetBlock(succ_fallthrough);
      if (b_fallthrough != nullptr && b_fallthrough->successors.size() == 2 &&
          b_fallthrough->predecessors.size() == 1 && b_fallthrough->predecessors[0] == entry &&
          IsPureConditionBlock(*cfg_, succ_fallthrough)) {
        uint32_t b_taken = 0;
        uint32_t b_ft = 0;
        for (const auto& edge : b_fallthrough->outgoing_edges) {
          if (edge.type == EdgeType::kBranch) {
            b_taken = edge.to_block_id;
          } else {
            b_ft = edge.to_block_id;
          }
        }
        if (b_taken == 0 && b_ft == 0) {
          b_taken = b_fallthrough->successors[0];
          b_ft = b_fallthrough->successors[1];
        }
        if (b_taken == succ_taken || b_ft == succ_taken) {
          std::vector<uint32_t> or_chain;
          std::vector<bool> or_inverts;
          uint32_t curr_id = succ_fallthrough;
          uint32_t curr_taken = b_taken;
          uint32_t curr_ft = b_ft;
          std::optional<uint32_t> final_after;

          while (true) {
            bool matches_taken = (curr_taken == succ_taken);
            bool matches_ft = (curr_ft == succ_taken);
            if (!matches_taken && !matches_ft) break;

            uint32_t other = matches_taken ? curr_ft : curr_taken;
            bool invert = matches_ft;
            or_chain.push_back(curr_id);
            or_inverts.push_back(invert);

            const auto* next_b = cfg_->GetBlock(other);
            if (next_b != nullptr && next_b->successors.size() == 2 &&
                next_b->predecessors.size() == 1 && !visited_.contains(other) &&
                IsPureConditionBlock(*cfg_, other) &&
                (next_b->successors[0] == succ_taken || next_b->successors[1] == succ_taken)) {
              curr_id = other;
              curr_taken = 0;
              curr_ft = 0;
              for (const auto& edge : next_b->outgoing_edges) {
                if (edge.type == EdgeType::kBranch) {
                  curr_taken = edge.to_block_id;
                } else {
                  curr_ft = edge.to_block_id;
                }
              }
              if (curr_taken == 0 && curr_ft == 0) {
                curr_taken = next_b->successors[0];
                curr_ft = next_b->successors[1];
              }
            } else {
              final_after = other;
              break;
            }
          }

          if (!or_chain.empty()) {
            for (uint32_t b_id : or_chain) {
              visited_.insert(b_id);
            }
            if (join.has_value() && final_after.has_value() && *final_after == *join) {
              auto if_then = CreateIfThen(entry, /*invert=*/false);
              if_then->condition_op = "||";
              if_then->additional_condition_blocks = or_chain;
              if_then->additional_invert_conditions = or_inverts;
              auto then_body = StructureRegion(succ_taken, join, current_loop);
              if (then_body) {
                if_then->children.push_back(std::move(then_body));
              }
              if (join.has_value() && join != follow) {
                auto after_region = StructureRegion(*join, follow, current_loop);
                if (after_region) {
                  auto seq = CreateSequence();
                  seq->children.push_back(std::move(if_then));
                  FlattenSequence(seq.get(), std::move(after_region));
                  return seq;
                }
              }
              return if_then;
            } else {
              auto if_else = CreateIfThenElse(entry, /*invert=*/false);
              if_else->condition_op = "||";
              if_else->additional_condition_blocks = or_chain;
              if_else->additional_invert_conditions = or_inverts;
              auto then_body = StructureRegion(succ_taken, join, current_loop);
              auto else_body = final_after.has_value()
                                   ? StructureRegion(*final_after, join, current_loop)
                                   : nullptr;
              if_else->children.push_back(then_body ? std::move(then_body) : CreateSequence());
              if_else->children.push_back(else_body ? std::move(else_body) : CreateSequence());

              if (join.has_value() && join != follow) {
                auto after_region = StructureRegion(*join, follow, current_loop);
                if (after_region) {
                  auto seq = CreateSequence();
                  seq->children.push_back(std::move(if_else));
                  FlattenSequence(seq.get(), std::move(after_region));
                  return seq;
                }
              }
              return if_else;
            }
          }
        }
      }
    }

    // Check for short-circuit AND (&&): entry branches to after on false; succ_fallthrough also
    // branches to after on false
    uint32_t and_after = succ_taken;
    if (!visited_.contains(succ_fallthrough)) {
      const auto* b_fallthrough = cfg_->GetBlock(succ_fallthrough);
      if (b_fallthrough != nullptr && b_fallthrough->successors.size() == 2 &&
          b_fallthrough->predecessors.size() == 1 && b_fallthrough->predecessors[0] == entry &&
          IsPureConditionBlock(*cfg_, succ_fallthrough)) {
        uint32_t b_taken = 0;
        uint32_t b_ft = 0;
        for (const auto& edge : b_fallthrough->outgoing_edges) {
          if (edge.type == EdgeType::kBranch) {
            b_taken = edge.to_block_id;
          } else {
            b_ft = edge.to_block_id;
          }
        }
        if (b_taken == 0 && b_ft == 0) {
          b_taken = b_fallthrough->successors[0];
          b_ft = b_fallthrough->successors[1];
        }
        if (b_taken == and_after || b_ft == and_after) {
          std::vector<uint32_t> and_chain;
          std::vector<bool> and_inverts;
          uint32_t curr_id = succ_fallthrough;
          uint32_t curr_taken = b_taken;
          uint32_t curr_ft = b_ft;
          std::optional<uint32_t> final_then;

          while (true) {
            bool matches_taken = (curr_taken == and_after);
            bool matches_ft = (curr_ft == and_after);
            if (!matches_taken && !matches_ft) break;

            uint32_t other = matches_taken ? curr_ft : curr_taken;
            bool invert = matches_taken;
            and_chain.push_back(curr_id);
            and_inverts.push_back(invert);

            const auto* next_b = cfg_->GetBlock(other);
            if (next_b != nullptr && next_b->successors.size() == 2 &&
                next_b->predecessors.size() == 1 && !visited_.contains(other) &&
                IsPureConditionBlock(*cfg_, other) &&
                (next_b->successors[0] == and_after || next_b->successors[1] == and_after)) {
              curr_id = other;
              curr_taken = 0;
              curr_ft = 0;
              for (const auto& edge : next_b->outgoing_edges) {
                if (edge.type == EdgeType::kBranch) {
                  curr_taken = edge.to_block_id;
                } else {
                  curr_ft = edge.to_block_id;
                }
              }
              if (curr_taken == 0 && curr_ft == 0) {
                curr_taken = next_b->successors[0];
                curr_ft = next_b->successors[1];
              }
            } else {
              final_then = other;
              break;
            }
          }

          if (!and_chain.empty() && final_then.has_value()) {
            for (uint32_t b_id : and_chain) {
              visited_.insert(b_id);
            }
            if (join.has_value() && and_after == *join) {
              auto if_then = CreateIfThen(entry, /*invert=*/true);
              if_then->condition_op = "&&";
              if_then->additional_condition_blocks = and_chain;
              if_then->additional_invert_conditions = and_inverts;
              auto then_body = StructureRegion(*final_then, join, current_loop);
              if (then_body) {
                if_then->children.push_back(std::move(then_body));
              }
              if (join.has_value() && join != follow) {
                auto after_region = StructureRegion(*join, follow, current_loop);
                if (after_region) {
                  auto seq = CreateSequence();
                  seq->children.push_back(std::move(if_then));
                  FlattenSequence(seq.get(), std::move(after_region));
                  return seq;
                }
              }
              return if_then;
            } else {
              auto if_else = CreateIfThenElse(entry, /*invert=*/true);
              if_else->condition_op = "&&";
              if_else->additional_condition_blocks = and_chain;
              if_else->additional_invert_conditions = and_inverts;
              auto then_body = StructureRegion(*final_then, join, current_loop);
              auto else_body = StructureRegion(and_after, join, current_loop);
              if_else->children.push_back(then_body ? std::move(then_body) : CreateSequence());
              if_else->children.push_back(else_body ? std::move(else_body) : CreateSequence());

              if (join.has_value() && join != follow) {
                auto after_region = StructureRegion(*join, follow, current_loop);
                if (after_region) {
                  auto seq = CreateSequence();
                  seq->children.push_back(std::move(if_else));
                  FlattenSequence(seq.get(), std::move(after_region));
                  return seq;
                }
              }
              return if_else;
            }
          }
        }
      }
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
