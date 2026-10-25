#include "lifter/control_flow_structurer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "core/register_tracker.h"
#include "lifter/control_flow_pattern_matcher.h"
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

std::unique_ptr<StructuredRegion> CreateSwitch(uint32_t block_id, const JumpTable* jt,
                                               Register reg = Register::kZero) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kSwitch;
  r->switch_block_id = block_id;
  r->jump_table = jt;
  r->switch_register = reg;
  if (jt != nullptr && reg == Register::kZero && jt->index_register != Register::kZero) {
    r->switch_register = jt->index_register;
  }
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
      if (float_to_uint_info.has_value()) {
        std::string type_name = float_to_uint_info->is_double ? "f64" : "f32";
        return ind + std::string(RegisterName(float_to_uint_info->destination_integer_register)) +
               " = (u32)(" + type_name + ")" +
               std::string(FpRegisterName(float_to_uint_info->source_float_register)) + ";\n";
      }
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
      std::string selector_str;
      if (switch_register != Register::kZero) {
        selector_str = std::string(RegisterName(switch_register));
      } else {
        selector_str = "block_" + std::to_string(switch_block_id);
      }
      std::string out = ind + "switch (" + selector_str + ") {\n";
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
  if (loop_follow.has_value()) {
    loop_follow_map_[loop] = *loop_follow;
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

std::optional<uint32_t> ControlFlowStructurer::FindSwitchJoinBlock(
    const absl::flat_hash_set<uint32_t>& decision_blocks,
    const absl::flat_hash_map<uint32_t, std::vector<int64_t>>& target_to_cases,
    std::optional<uint32_t> default_block_id, std::optional<uint32_t> follow,
    const Loop* current_loop) {
  return rom_nom_nom::FindSwitchJoinBlock(*cfg_, post_dom_tree_, visited_, decision_blocks,
                                          target_to_cases, default_block_id, follow, current_loop);
}

std::optional<FloatToUintInfo> ControlFlowStructurer::DetectFloatToUintDiamond(
    uint32_t entry, uint32_t succ_taken, uint32_t succ_fallthrough, std::optional<uint32_t> join) {
  return rom_nom_nom::DetectFloatToUintDiamond(*cfg_, entry, succ_taken, succ_fallthrough, join);
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
      auto lf_it = loop_follow_map_.find(current_loop);
      std::optional<uint32_t> loop_follow_target =
          (lf_it != loop_follow_map_.end()) ? std::optional<uint32_t>(lf_it->second) : follow;
      if ((follow.has_value() && entry == *follow) ||
          (lf_it != loop_follow_map_.end() && entry == lf_it->second)) {
        return CreateBreak(entry);
      }
      if (current_loop->parent != nullptr && visited_.contains(entry) &&
          entry == current_loop->parent->header) {
        return CreateGoto(entry);
      }
      if (loop_follow_target.has_value()) {
        const auto* lf_blk = cfg_->GetBlock(*loop_follow_target);
        if (lf_blk != nullptr && lf_blk->successors.size() == 1 && lf_blk->successors[0] == entry) {
          return CreateGoto(entry);
        }
      }
      auto exit_body = StructureRegion(entry, loop_follow_target, nullptr);
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

  if (current_switch_break_.has_value() && entry == *current_switch_break_) {
    return CreateBreak(entry);
  }

  if (visited_.contains(entry)) {
    if (current_switch_break_.has_value() && entry == *current_switch_break_) {
      return CreateBreak(entry);
    }
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
    if (has_bounds_block) {
      visited_.insert(switch_dispatch_block_id);
    }

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
    std::sort(unique_target_blocks.begin(), unique_target_blocks.end());

    absl::flat_hash_set<uint32_t> decision_blocks = {entry};
    if (has_bounds_block) {
      decision_blocks.insert(switch_dispatch_block_id);
    }

    std::optional<uint32_t> join = FindSwitchJoinBlock(
        decision_blocks, target_to_cases,
        has_explicit_default ? std::make_optional(def_blk_id) : std::nullopt, follow, current_loop);

    if (!join.has_value() && post_dom_tree_ != nullptr) {
      join = post_dom_tree_->ImmediateDominator(entry);
      if (join.has_value() && (*join == entry || *join == switch_dispatch_block_id)) {
        join = post_dom_tree_->ImmediateDominator(*join);
      }
    }
    if (follow.has_value() && (!join.has_value() || *join == *follow)) {
      join = follow;
    }

    auto switch_region = CreateSwitch(switch_dispatch_block_id, switch_jt);
    switch_region->switch_join_block = join;

    auto prev_switch_break = current_switch_break_;
    current_switch_break_ = join;

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
        if (current_loop == nullptr || !current_loop->IsLatch(target_blk_id)) {
          sc.body = StructureRegion(target_blk_id, join, current_loop);
        }
      }
      switch_region->cases.push_back(std::move(sc));
    }

    current_switch_break_ = prev_switch_break;

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
    if (current_switch_break_.has_value() && succ == *current_switch_break_) {
      auto seq = CreateSequence();
      seq->children.push_back(std::move(block_region));
      seq->children.push_back(CreateBreak(succ));
      return seq;
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
    if (current_switch_break_.has_value()) {
      const auto* term = block->Terminator();
      if (term != nullptr && term->opcode == Opcode::kJ) {
        const auto* succ_blk = cfg_->GetBlock(succ);
        if (succ_blk != nullptr && succ_blk->predecessors.size() > 1) {
          bool has_unvisited_fallthrough_pred = false;
          for (uint32_t pred : succ_blk->predecessors) {
            if (pred != entry && !visited_.contains(pred)) {
              const auto* pred_blk = cfg_->GetBlock(pred);
              if (pred_blk != nullptr && pred_blk->end_vram + 4 == succ_blk->start_vram) {
                const auto* pred_term = pred_blk->Terminator();
                if (pred_term == nullptr ||
                    (pred_term->opcode != Opcode::kJ && !pred_term->IsBranch())) {
                  has_unvisited_fallthrough_pred = true;
                  break;
                }
              }
            }
          }
          if (has_unvisited_fallthrough_pred) {
            auto seq = CreateSequence();
            seq->children.push_back(std::move(block_region));
            seq->children.push_back(CreateGoto(succ));
            return seq;
          }
        }
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
    std::optional<DecisionTreeSwitchInfo> dt_switch =
        DetectDecisionTreeSwitch(*cfg_, loop_info_, visited_, entry);
    if (dt_switch.has_value() && current_switch_break_.has_value() &&
        dt_switch->target_to_cases.contains(*current_switch_break_)) {
      dt_switch = std::nullopt;
    }
    if (dt_switch.has_value()) {
      std::optional<uint32_t> join =
          FindSwitchJoinBlock(dt_switch->decision_blocks, dt_switch->target_to_cases,
                              dt_switch->default_target_block, follow, current_loop);
      if (!join.has_value() && post_dom_tree_ != nullptr) {
        join = post_dom_tree_->ImmediateDominator(entry);
        if (join.has_value() && dt_switch->decision_blocks.contains(*join)) {
          join = post_dom_tree_->ImmediateDominator(*join);
        }
      }
      if (follow.has_value() && (!join.has_value() || *join == *follow)) {
        join = follow;
      }

      for (uint32_t db : dt_switch->decision_blocks) {
        visited_.insert(db);
      }

      auto switch_region = CreateSwitch(entry, nullptr, dt_switch->selector_reg);
      switch_region->owned_jump_table = std::make_unique<JumpTable>();
      switch_region->owned_jump_table->index_register = dt_switch->selector_reg;
      switch_region->jump_table = switch_region->owned_jump_table.get();
      switch_region->switch_join_block = join;

      auto prev_switch_break = current_switch_break_;
      current_switch_break_ = join;

      if (!dt_switch->cases.empty()) {
        for (const auto& dc : dt_switch->cases) {
          StructuredCase sc;
          sc.case_values = dc.case_values;
          sc.is_default = dc.is_default;
          sc.delay_slot_const_reg = dc.const_reg;
          sc.delay_slot_const_val = dc.const_val;
          sc.delay_instructions = dc.delay_instructions;
          if (!join.has_value() || dc.target_block != *join) {
            if (current_loop == nullptr || !current_loop->IsLatch(dc.target_block)) {
              sc.body = StructureRegion(dc.target_block, join, current_loop);
            }
          }
          switch_region->cases.push_back(std::move(sc));
        }
      } else {
        for (uint32_t target_blk_id : dt_switch->unique_target_blocks) {
          StructuredCase sc;
          auto it_cases = dt_switch->target_to_cases.find(target_blk_id);
          if (it_cases != dt_switch->target_to_cases.end()) {
            sc.case_values = it_cases->second;
          }
          if (dt_switch->default_target_block.has_value() &&
              target_blk_id == *dt_switch->default_target_block) {
            sc.is_default = true;
          }
          if (!join.has_value() || target_blk_id != *join) {
            if (current_loop == nullptr || !current_loop->IsLatch(target_blk_id)) {
              sc.body = StructureRegion(target_blk_id, join, current_loop);
            }
          }
          switch_region->cases.push_back(std::move(sc));
        }
      }

      current_switch_break_ = prev_switch_break;

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

    // Case 3-switch-break: If inside a switch case and one successor targets current_switch_break_
    if (current_switch_break_.has_value()) {
      if (succ_taken == *current_switch_break_ && succ_fallthrough != *current_switch_break_) {
        auto if_break = CreateIfThen(entry, /*invert=*/false);
        if_break->children.push_back(CreateBreak(succ_taken));
        auto next = StructureRegion(succ_fallthrough, follow, current_loop);
        if (next) {
          auto seq = CreateSequence();
          seq->children.push_back(std::move(if_break));
          FlattenSequence(seq.get(), std::move(next));
          return seq;
        }
        return if_break;
      }
      if (succ_fallthrough == *current_switch_break_ && succ_taken != *current_switch_break_) {
        auto if_break = CreateIfThen(entry, /*invert=*/true);
        if_break->children.push_back(CreateBreak(succ_fallthrough));
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

    // Case 3-break: If inside a loop and one successor is a loop exit before the latch
    if (current_loop != nullptr && !current_loop->IsLatch(entry)) {
      auto lf_it = loop_follow_map_.find(current_loop);
      std::optional<uint32_t> loop_follow_target =
          (lf_it != loop_follow_map_.end()) ? std::optional<uint32_t>(lf_it->second) : follow;
      auto is_successor_of_loop_follow = [&](uint32_t target_id) {
        if (!loop_follow_target.has_value()) return false;
        const auto* lf_blk = cfg_->GetBlock(*loop_follow_target);
        return lf_blk != nullptr && lf_blk->successors.size() == 1 &&
               lf_blk->successors[0] == target_id;
      };
      if (current_loop->IsExitBlock(succ_taken) && !current_loop->IsExitBlock(succ_fallthrough)) {
        auto if_break = CreateIfThen(entry, /*invert=*/false);
        if ((follow.has_value() && succ_taken == *follow) ||
            (lf_it != loop_follow_map_.end() && succ_taken == lf_it->second)) {
          if_break->children.push_back(CreateBreak(succ_taken));
        } else if ((current_loop->parent != nullptr && visited_.contains(succ_taken) &&
                    succ_taken == current_loop->parent->header) ||
                   is_successor_of_loop_follow(succ_taken)) {
          if_break->children.push_back(CreateGoto(succ_taken));
        } else {
          auto exit_body = StructureRegion(succ_taken, loop_follow_target, nullptr);
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
        if ((follow.has_value() && succ_fallthrough == *follow) ||
            (lf_it != loop_follow_map_.end() && succ_fallthrough == lf_it->second)) {
          if_break->children.push_back(CreateBreak(succ_fallthrough));
        } else if ((current_loop->parent != nullptr && visited_.contains(succ_fallthrough) &&
                    succ_fallthrough == current_loop->parent->header) ||
                   is_successor_of_loop_follow(succ_fallthrough)) {
          if_break->children.push_back(CreateGoto(succ_fallthrough));
        } else {
          auto exit_body = StructureRegion(succ_fallthrough, loop_follow_target, nullptr);
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

    auto float_diamond = DetectFloatToUintDiamond(entry, succ_taken, succ_fallthrough, join);
    if (float_diamond.has_value()) {
      auto if_else = CreateIfThenElse(entry);
      if_else->float_to_uint_info = *float_diamond;
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

    auto sc_chain =
        TryCollapseShortCircuitChain(*cfg_, visited_, entry, succ_taken, succ_fallthrough, join);
    if (sc_chain.has_value()) {
      for (uint32_t b_id : sc_chain->additional_condition_blocks) {
        visited_.insert(b_id);
      }
      if (join.has_value() && sc_chain->else_target_block.has_value() &&
          *sc_chain->else_target_block == *join) {
        auto if_then = CreateIfThen(entry, sc_chain->root_invert);
        if_then->condition_op = sc_chain->condition_op;
        if_then->additional_condition_blocks = std::move(sc_chain->additional_condition_blocks);
        if_then->additional_invert_conditions = std::move(sc_chain->additional_invert_conditions);
        auto then_body = StructureRegion(sc_chain->then_target_block, join, current_loop);
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
        auto if_else = CreateIfThenElse(entry, sc_chain->root_invert);
        if_else->condition_op = sc_chain->condition_op;
        if_else->additional_condition_blocks = std::move(sc_chain->additional_condition_blocks);
        if_else->additional_invert_conditions = std::move(sc_chain->additional_invert_conditions);
        auto then_body = StructureRegion(sc_chain->then_target_block, join, current_loop);
        auto else_body = sc_chain->else_target_block.has_value()
                             ? StructureRegion(*sc_chain->else_target_block, join, current_loop)
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
      // In GCC, the then-block immediately follows the branch (fallthrough),
      // and the branch jumps to the else-block on !cond.
      auto if_else = CreateIfThenElse(entry, /*invert=*/true);
      auto then_body = StructureRegion(succ_fallthrough, join, current_loop);
      auto else_body = StructureRegion(succ_taken, join, current_loop);

      auto try_fold_root_pivot_into_switch =
          [&](std::unique_ptr<StructuredRegion>& child_switch,
              std::unique_ptr<StructuredRegion>& pivot_arm) -> std::unique_ptr<StructuredRegion> {
        if (!child_switch || child_switch->type != RegionType::kSwitch ||
            child_switch->owned_jump_table == nullptr ||
            child_switch->switch_register == Register::kZero) {
          return nullptr;
        }
        Register selector = child_switch->switch_register;
        const auto* entry_blk = cfg_->GetBlock(entry);
        if (entry_blk == nullptr || entry_blk->instructions.empty()) {
          return nullptr;
        }
        int branch_idx = -1;
        for (int i = static_cast<int>(entry_blk->instructions.size()) - 1; i >= 0; --i) {
          if (entry_blk->instructions[i].IsBranch()) {
            branch_idx = i;
            break;
          }
        }
        if (branch_idx < 0) return nullptr;
        const auto& branch = entry_blk->instructions[branch_idx];
        if (branch.opcode != Opcode::kBeq && branch.opcode != Opcode::kBne &&
            branch.opcode != Opcode::kBeql && branch.opcode != Opcode::kBnel) {
          return nullptr;
        }
        absl::flat_hash_map<Register, int64_t> consts;
        consts[Register::kZero] = 0;
        for (int i = 0; i < branch_idx; ++i) {
          const auto& inst = entry_blk->instructions[i];
          if (inst.opcode == Opcode::kAddiu && inst.rs == Register::kZero && inst.rt.has_value()) {
            consts[*inst.rt] = static_cast<int16_t>(inst.immediate);
          } else if (inst.opcode == Opcode::kOri && inst.rs == Register::kZero &&
                     inst.rt.has_value()) {
            consts[*inst.rt] = static_cast<uint16_t>(inst.immediate);
          } else if (inst.opcode == Opcode::kLui && inst.rt.has_value()) {
            consts[*inst.rt] = static_cast<int64_t>(inst.immediate) << 16;
          } else {
            RegisterUseDef ud = GetInstructionUseDef(inst);
            for (Register def_reg : ud.gpr_defs) {
              consts.erase(def_reg);
            }
          }
        }
        Register rs = branch.rs.value_or(Register::kZero);
        Register rt = branch.rt.value_or(Register::kZero);
        std::optional<int64_t> pivot_val;
        if (rs == selector && consts.contains(rt)) {
          pivot_val = consts[rt];
        } else if (rt == selector && consts.contains(rs)) {
          pivot_val = consts[rs];
        }
        if (!pivot_val.has_value()) return nullptr;

        bool taken_is_eq = (branch.opcode == Opcode::kBeq || branch.opcode == Opcode::kBeql);
        if (!taken_is_eq) {
          return nullptr;
        }
        bool pivot_arm_is_taken = (pivot_arm.get() == else_body.get());
        if (!pivot_arm_is_taken) {
          return nullptr;
        }
        for (const auto& sc : child_switch->cases) {
          for (int64_t v : sc.case_values) {
            if (v == *pivot_val) return nullptr;
          }
        }

        StructuredCase pivot_case;
        pivot_case.case_values = {*pivot_val};
        pivot_case.body = std::move(pivot_arm);
        if (branch_idx + 1 < static_cast<int>(entry_blk->instructions.size()) &&
            branch.IsBranchLikely()) {
          const auto& ds = entry_blk->instructions[branch_idx + 1];
          RegisterUseDef ds_ud = GetInstructionUseDef(ds);
          bool ds_defines_callee_saved = false;
          for (Register def_reg : ds_ud.gpr_defs) {
            if ((def_reg >= Register::kS0 && def_reg <= Register::kS7) ||
                def_reg == Register::kFp) {
              ds_defines_callee_saved = true;
              break;
            }
          }
          if (!ds.IsNop() && !ds_defines_callee_saved) {
            pivot_case.delay_instructions.push_back(ds);
          }
        }

        auto get_region_entry_block = [](const StructuredRegion* r) -> std::optional<uint32_t> {
          const StructuredRegion* cur = r;
          while (cur != nullptr) {
            if (cur->type == RegionType::kBlock) return cur->block_id;
            if (cur->type == RegionType::kSequence && !cur->children.empty()) {
              cur = cur->children[0].get();
            } else {
              break;
            }
          }
          return std::nullopt;
        };
        std::optional<uint32_t> pivot_blk_id = get_region_entry_block(pivot_case.body.get());

        auto insert_it = child_switch->cases.end();
        for (auto it = child_switch->cases.begin(); it != child_switch->cases.end(); ++it) {
          if (it->is_default) {
            insert_it = it;
            break;
          }
          if (pivot_blk_id.has_value()) {
            std::optional<uint32_t> case_blk_id = get_region_entry_block(it->body.get());
            if (case_blk_id.has_value() && *pivot_blk_id < *case_blk_id) {
              insert_it = it;
              break;
            }
          }
        }
        child_switch->cases.insert(insert_it, std::move(pivot_case));
        child_switch->switch_block_id = entry;
        if (!child_switch->switch_join_block.has_value()) {
          child_switch->switch_join_block = join;
        }
        return std::move(child_switch);
      };

      if (auto folded = try_fold_root_pivot_into_switch(then_body, else_body); folded != nullptr) {
        if (join.has_value() && join != follow) {
          auto after_join = StructureRegion(*join, follow, current_loop);
          if (after_join) {
            auto seq = CreateSequence();
            seq->children.push_back(std::move(folded));
            FlattenSequence(seq.get(), std::move(after_join));
            return seq;
          }
        }
        return folded;
      }
      if (auto folded = try_fold_root_pivot_into_switch(else_body, then_body); folded != nullptr) {
        if (join.has_value() && join != follow) {
          auto after_join = StructureRegion(*join, follow, current_loop);
          if (after_join) {
            auto seq = CreateSequence();
            seq->children.push_back(std::move(folded));
            FlattenSequence(seq.get(), std::move(after_join));
            return seq;
          }
        }
        return folded;
      }

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
