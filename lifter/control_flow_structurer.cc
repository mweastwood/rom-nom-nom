#include "lifter/control_flow_structurer.h"

#include <algorithm>
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

bool HasNopBranchDelaySlot(const ControlFlowGraph& cfg, uint32_t block_id) {
  const auto* b = cfg.GetBlock(block_id);
  if (b == nullptr || b->instructions.size() < 2) return true;
  if (b->instructions[b->instructions.size() - 2].IsBranch()) {
    return b->instructions.back().IsNop();
  }
  return true;
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
  absl::flat_hash_set<uint32_t> targets;
  for (const auto& [blk, _] : target_to_cases) {
    if (!decision_blocks.contains(blk)) {
      targets.insert(blk);
    }
  }
  if (default_block_id.has_value() && !decision_blocks.contains(*default_block_id)) {
    targets.insert(*default_block_id);
  }

  if (targets.size() < 2) {
    return std::nullopt;
  }

  absl::flat_hash_map<uint32_t, absl::flat_hash_set<uint32_t>> reaches;
  absl::flat_hash_set<uint32_t> returning_targets;

  for (uint32_t t : targets) {
    absl::flat_hash_set<uint32_t> visited_from_t;
    std::vector<uint32_t> queue = {t};
    visited_from_t.insert(t);
    reaches[t].insert(t);
    bool returns = false;

    while (!queue.empty()) {
      uint32_t curr = queue.back();
      queue.pop_back();

      const auto* blk = cfg_->GetBlock(curr);
      if (blk == nullptr) continue;

      bool is_return = blk->successors.empty();
      if (!is_return) {
        const auto* term = blk->Terminator();
        if (term != nullptr && term->IsReturn()) {
          is_return = true;
        }
      }
      if (is_return) {
        returns = true;
        continue;
      }

      for (uint32_t succ : blk->successors) {
        if (decision_blocks.contains(succ) || visited_.contains(succ)) {
          continue;
        }
        reaches[succ].insert(t);
        if (follow.has_value() && succ == *follow) {
          continue;
        }
        if (visited_from_t.insert(succ).second) {
          queue.push_back(succ);
        }
      }
    }
    if (returns) {
      returning_targets.insert(t);
    }
  }

  std::vector<uint32_t> candidates;
  size_t non_returning_count = targets.size() - returning_targets.size();
  absl::flat_hash_map<uint32_t, size_t> non_returning_reach_counts;

  for (const auto& [b, reached_by] : reaches) {
    if (decision_blocks.contains(b) || visited_.contains(b)) {
      continue;
    }
    if (current_loop != nullptr && !current_loop->Contains(b)) {
      continue;
    }
    size_t non_returning_reach_count = 0;
    for (uint32_t reaching_target_block_id : reached_by) {
      if (!returning_targets.contains(reaching_target_block_id)) {
        non_returning_reach_count++;
      }
    }
    non_returning_reach_counts[b] = non_returning_reach_count;

    if (reached_by.size() >= 2 || non_returning_reach_count >= 2 ||
        (!targets.contains(b) && non_returning_reach_count > 0 &&
         non_returning_reach_count == non_returning_count)) {
      candidates.push_back(b);
    }
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  absl::flat_hash_set<uint32_t> eliminated;
  for (uint32_t candidate_one : candidates) {
    std::vector<uint32_t> search_queue = {candidate_one};
    absl::flat_hash_set<uint32_t> seen_blocks = {candidate_one};
    while (!search_queue.empty()) {
      uint32_t current_block_id = search_queue.back();
      search_queue.pop_back();
      if (follow.has_value() && current_block_id == *follow) {
        continue;
      }
      const auto* current_block = cfg_->GetBlock(current_block_id);
      if (current_block == nullptr) continue;
      for (uint32_t successor_block_id : current_block->successors) {
        if (decision_blocks.contains(successor_block_id) || visited_.contains(successor_block_id)) {
          continue;
        }
        if (successor_block_id != candidate_one &&
            std::find(candidates.begin(), candidates.end(), successor_block_id) !=
                candidates.end()) {
          auto reaches_candidate_one_iterator = non_returning_reach_counts.find(candidate_one);
          auto reaches_successor_iterator = non_returning_reach_counts.find(successor_block_id);
          size_t candidate_one_reach_count =
              (reaches_candidate_one_iterator != non_returning_reach_counts.end())
                  ? reaches_candidate_one_iterator->second
                  : 0;
          size_t successor_reach_count =
              (reaches_successor_iterator != non_returning_reach_counts.end())
                  ? reaches_successor_iterator->second
                  : 0;
          bool candidate_one_post_dominates_all_targets = true;
          if (post_dom_tree_ != nullptr) {
            for (uint32_t target_block_id : targets) {
              if (!returning_targets.contains(target_block_id) &&
                  !post_dom_tree_->Dominates(candidate_one, target_block_id)) {
                candidate_one_post_dominates_all_targets = false;
                break;
              }
            }
          }
          if (candidate_one_reach_count >= successor_reach_count &&
              candidate_one_post_dominates_all_targets) {
            eliminated.insert(successor_block_id);
          } else {
            eliminated.insert(candidate_one);
          }
        }
        if (follow.has_value() && successor_block_id == *follow) {
          continue;
        }
        if (seen_blocks.insert(successor_block_id).second) {
          search_queue.push_back(successor_block_id);
        }
      }
    }
  }

  if (follow.has_value() && (current_loop == nullptr || current_loop->Contains(*follow)) &&
      !eliminated.contains(*follow)) {
    auto reaches_follow_iterator = non_returning_reach_counts.find(*follow);
    if (reaches_follow_iterator != non_returning_reach_counts.end() &&
        reaches_follow_iterator->second == non_returning_count) {
      return *follow;
    }
  }

  for (uint32_t c : candidates) {
    if (!eliminated.contains(c)) {
      const auto* blk = cfg_->GetBlock(c);
      bool is_pure_ret =
          (blk != nullptr && (blk->successors.empty() ||
                              (blk->Terminator() != nullptr && blk->Terminator()->IsReturn())));
      if (!is_pure_ret || candidates.size() == 1) {
        return c;
      }
    }
  }

  for (uint32_t c : candidates) {
    if (!eliminated.contains(c)) return c;
  }

  return candidates[0];
}

std::optional<ControlFlowStructurer::DecisionTreeSwitchInfo>
ControlFlowStructurer::DetectDecisionTreeSwitch(uint32_t root_id) {
  const auto* root_blk = cfg_->GetBlock(root_id);
  if (root_blk == nullptr || root_blk->successors.size() != 2 ||
      root_blk->instructions.size() > 6 || root_blk->instructions.empty()) {
    return std::nullopt;
  }

  struct BlockAnalysis {
    bool valid = false;
    Register selector_reg = Register::kZero;
    bool is_equality = false;
    int64_t case_val = 0;
    uint32_t case_target = 0;
    uint32_t cont_target = 0;
    bool is_range_split = false;
    uint32_t split_taken = 0;
    uint32_t split_ft = 0;

    bool delay_slot_sets_const = false;
    Register delay_slot_const_reg = Register::kZero;
    int64_t delay_slot_const_val = 0;

    bool delay_slot_sets_split = false;
    Register delay_slot_split_reg = Register::kZero;
    int64_t delay_slot_split_val = 0;
    Register delay_slot_split_selector = Register::kZero;
    std::optional<Instruction> delay_instruction;
  };

  auto analyze_block = [&](uint32_t b_id, const absl::flat_hash_map<Register, int64_t>& in_consts,
                           const absl::flat_hash_map<Register, int64_t>& in_splits,
                           const absl::flat_hash_map<Register, Register>& in_split_sels,
                           Register known_selector) -> BlockAnalysis {
    BlockAnalysis ba;
    const auto* blk = cfg_->GetBlock(b_id);
    if (blk == nullptr || blk->successors.size() != 2 || blk->instructions.size() > 6) {
      return ba;
    }
    for (const auto& inst : blk->instructions) {
      if (inst.IsCall() || inst.IsStore() || inst.IsLoad()) return ba;
    }

    absl::flat_hash_map<Register, int64_t> consts = in_consts;
    absl::flat_hash_map<Register, int64_t> splits = in_splits;
    absl::flat_hash_map<Register, Register> split_sels = in_split_sels;
    consts[Register::kZero] = 0;

    int branch_idx = -1;
    for (int i = static_cast<int>(blk->instructions.size()) - 1; i >= 0; --i) {
      if (blk->instructions[i].IsBranch()) {
        branch_idx = i;
        break;
      }
    }
    if (branch_idx == -1) return ba;

    absl::flat_hash_set<Register> bool_flags;
    for (int i = 0; i < branch_idx; ++i) {
      const auto& inst = blk->instructions[i];
      if (inst.opcode == Opcode::kAddiu && inst.rs == Register::kZero && inst.rt.has_value()) {
        consts[*inst.rt] = static_cast<int16_t>(inst.immediate);
      } else if (inst.opcode == Opcode::kOri && inst.rs == Register::kZero && inst.rt.has_value()) {
        consts[*inst.rt] = static_cast<uint16_t>(inst.immediate);
      } else if (inst.opcode == Opcode::kLui && inst.rt.has_value()) {
        consts[*inst.rt] = static_cast<int64_t>(inst.immediate) << 16;
      } else if (inst.opcode == Opcode::kSlti && inst.rs.has_value() && inst.rt.has_value()) {
        splits[*inst.rt] = static_cast<int16_t>(inst.immediate);
        split_sels[*inst.rt] = *inst.rs;
        bool_flags.insert(*inst.rt);
      } else if (inst.opcode == Opcode::kSltiu && inst.rs.has_value() && inst.rt.has_value()) {
        splits[*inst.rt] = static_cast<uint16_t>(inst.immediate);
        split_sels[*inst.rt] = *inst.rs;
        bool_flags.insert(*inst.rt);
      } else if (inst.opcode == Opcode::kSlt && inst.rs.has_value() && inst.rt.has_value() &&
                 inst.rd.has_value()) {
        bool_flags.insert(*inst.rd);
        if (consts.contains(*inst.rt)) {
          splits[*inst.rd] = consts[*inst.rt];
          split_sels[*inst.rd] = *inst.rs;
        }
      } else if (inst.opcode == Opcode::kSltu && inst.rs.has_value() && inst.rt.has_value() &&
                 inst.rd.has_value()) {
        bool_flags.insert(*inst.rd);
        if (consts.contains(*inst.rt)) {
          splits[*inst.rd] = consts[*inst.rt];
          split_sels[*inst.rd] = *inst.rs;
        }
      }
    }

    if (known_selector != Register::kZero) {
      for (const auto& inst : blk->instructions) {
        if (inst.rd.has_value() && *inst.rd == known_selector) return ba;
        if (inst.rt.has_value() &&
            (inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kOri ||
             inst.opcode == Opcode::kLui || inst.opcode == Opcode::kSlti ||
             inst.opcode == Opcode::kSltiu || inst.opcode == Opcode::kLw) &&
            *inst.rt == known_selector) {
          return ba;
        }
      }
    }

    const auto& branch = blk->instructions[branch_idx];
    uint32_t taken = 0;
    uint32_t ft = 0;
    for (const auto& edge : blk->outgoing_edges) {
      if (edge.type == EdgeType::kBranch) {
        taken = edge.to_block_id;
      } else {
        ft = edge.to_block_id;
      }
    }
    if (taken == 0 && ft == 0) {
      taken = blk->successors[0];
      ft = blk->successors[1];
    }

    if (branch_idx + 1 < static_cast<int>(blk->instructions.size())) {
      const auto& ds = blk->instructions[branch_idx + 1];
      if (!ds.IsNop()) {
        ba.delay_instruction = ds;
      }
      if (ds.opcode == Opcode::kAddiu && ds.rs == Register::kZero && ds.rt.has_value()) {
        ba.delay_slot_sets_const = true;
        ba.delay_slot_const_reg = *ds.rt;
        ba.delay_slot_const_val = static_cast<int16_t>(ds.immediate);
      } else if (ds.opcode == Opcode::kAddu && ds.rs == Register::kZero &&
                 ds.rt == Register::kZero && ds.rd.has_value()) {
        ba.delay_slot_sets_const = true;
        ba.delay_slot_const_reg = *ds.rd;
        ba.delay_slot_const_val = 0;
      } else if (ds.opcode == Opcode::kOri && ds.rs == Register::kZero && ds.rt.has_value()) {
        ba.delay_slot_sets_const = true;
        ba.delay_slot_const_reg = *ds.rt;
        ba.delay_slot_const_val = static_cast<uint16_t>(ds.immediate);
      } else if (ds.opcode == Opcode::kSlti && ds.rs.has_value() && ds.rt.has_value()) {
        ba.delay_slot_sets_split = true;
        ba.delay_slot_split_reg = *ds.rt;
        ba.delay_slot_split_val = static_cast<int16_t>(ds.immediate);
        ba.delay_slot_split_selector = *ds.rs;
        bool_flags.insert(*ds.rt);
      } else if (ds.opcode == Opcode::kSltiu && ds.rs.has_value() && ds.rt.has_value()) {
        ba.delay_slot_sets_split = true;
        ba.delay_slot_split_reg = *ds.rt;
        ba.delay_slot_split_val = static_cast<uint16_t>(ds.immediate);
        ba.delay_slot_split_selector = *ds.rs;
        bool_flags.insert(*ds.rt);
      } else if (ds.opcode == Opcode::kSlt && ds.rs.has_value() && ds.rt.has_value() &&
                 ds.rd.has_value()) {
        bool_flags.insert(*ds.rd);
        if (consts.contains(*ds.rt)) {
          ba.delay_slot_sets_split = true;
          ba.delay_slot_split_reg = *ds.rd;
          ba.delay_slot_split_val = consts[*ds.rt];
          ba.delay_slot_split_selector = *ds.rs;
        }
      } else if (ds.opcode == Opcode::kSltu && ds.rs.has_value() && ds.rt.has_value() &&
                 ds.rd.has_value()) {
        bool_flags.insert(*ds.rd);
        if (consts.contains(*ds.rt)) {
          ba.delay_slot_sets_split = true;
          ba.delay_slot_split_reg = *ds.rd;
          ba.delay_slot_split_val = consts[*ds.rt];
          ba.delay_slot_split_selector = *ds.rs;
        }
      }
    }

    if (branch.opcode == Opcode::kBeq || branch.opcode == Opcode::kBne) {
      Register rs = branch.rs.value_or(Register::kZero);
      Register rt = branch.rt.value_or(Register::kZero);

      if (splits.contains(rs) &&
          (rt == Register::kZero || (consts.contains(rt) && consts[rt] == 0))) {
        Register sel = split_sels.contains(rs) ? split_sels[rs] : known_selector;
        if (sel != Register::kZero && sel != Register::kAt && !bool_flags.contains(sel)) {
          if (known_selector == Register::kZero || sel == known_selector) {
            ba.valid = true;
            ba.selector_reg = sel;
            ba.is_range_split = true;
            ba.split_taken = taken;
            ba.split_ft = ft;
            return ba;
          }
        }
      }
      if (splits.contains(rt) &&
          (rs == Register::kZero || (consts.contains(rs) && consts[rs] == 0))) {
        Register sel = split_sels.contains(rt) ? split_sels[rt] : known_selector;
        if (sel != Register::kZero && sel != Register::kAt && !bool_flags.contains(sel)) {
          if (known_selector == Register::kZero || sel == known_selector) {
            ba.valid = true;
            ba.selector_reg = sel;
            ba.is_range_split = true;
            ba.split_taken = taken;
            ba.split_ft = ft;
            return ba;
          }
        }
      }

      if (consts.contains(rt) && !consts.contains(rs) && rs != Register::kZero &&
          rs != Register::kAt && !bool_flags.contains(rs)) {
        if (known_selector == Register::kZero || rs == known_selector) {
          ba.valid = true;
          ba.selector_reg = rs;
          ba.is_equality = true;
          ba.case_val = consts[rt];
          ba.case_target = (branch.opcode == Opcode::kBeq) ? taken : ft;
          ba.cont_target = (branch.opcode == Opcode::kBeq) ? ft : taken;
          return ba;
        }
      }
      if (consts.contains(rs) && !consts.contains(rt) && rt != Register::kZero &&
          rt != Register::kAt && !bool_flags.contains(rt)) {
        if (known_selector == Register::kZero || rt == known_selector) {
          ba.valid = true;
          ba.selector_reg = rt;
          ba.is_equality = true;
          ba.case_val = consts[rs];
          ba.case_target = (branch.opcode == Opcode::kBeq) ? taken : ft;
          ba.cont_target = (branch.opcode == Opcode::kBeq) ? ft : taken;
          return ba;
        }
      }
    }

    return ba;
  };

  BlockAnalysis root_ba = analyze_block(root_id, {}, {}, {}, Register::kZero);
  if (!root_ba.valid || root_ba.selector_reg == Register::kZero) {
    return std::nullopt;
  }

  Register selector = root_ba.selector_reg;
  absl::flat_hash_set<uint32_t> decision_blocks = {root_id};
  std::vector<uint32_t> queue = {root_id};

  absl::flat_hash_map<uint32_t, absl::flat_hash_map<Register, int64_t>> block_consts;
  absl::flat_hash_map<uint32_t, absl::flat_hash_map<Register, int64_t>> block_splits;
  absl::flat_hash_map<uint32_t, absl::flat_hash_map<Register, Register>> block_split_sels;

  absl::flat_hash_map<uint32_t, std::vector<int64_t>> target_to_cases;
  std::vector<uint32_t> target_order;
  std::optional<uint32_t> default_target;
  std::optional<Register> default_const_reg;
  std::optional<int64_t> default_const_val;
  std::vector<Instruction> default_delay_instructions;
  absl::flat_hash_set<int64_t> seen_case_vals;
  std::vector<DecisionCase> decision_cases;

  auto resolve_trampoline = [&](uint32_t b) -> uint32_t {
    uint32_t curr = b;
    absl::flat_hash_set<uint32_t> visited_trampoline;
    while (visited_trampoline.insert(curr).second) {
      const auto* blk = cfg_->GetBlock(curr);
      if (blk != nullptr && blk->successors.size() == 1) {
        bool is_pure_jump = false;
        if (blk->instructions.empty()) {
          is_pure_jump = true;
        } else if (blk->instructions.size() <= 2) {
          const auto* term = blk->Terminator();
          if (term != nullptr && term->opcode == Opcode::kJ) {
            is_pure_jump = true;
          }
        }
        if (is_pure_jump) {
          curr = blk->successors[0];
          continue;
        }
      }
      break;
    }
    return curr;
  };

  while (!queue.empty()) {
    uint32_t b_id = queue.front();
    queue.erase(queue.begin());

    if (loop_info_ != nullptr) {
      if (loop_info_->GetLoopFor(b_id) != loop_info_->GetLoopFor(root_id)) {
        return std::nullopt;
      }
      const Loop* b_loop = loop_info_->GetLoopFor(b_id);
      if (b_loop != nullptr) {
        if (b_loop->IsLatch(b_id)) return std::nullopt;
        if (b_id != root_id && b_loop->header == b_id) return std::nullopt;
      }
    }

    BlockAnalysis ba = analyze_block(b_id, block_consts[b_id], block_splits[b_id],
                                     block_split_sels[b_id], selector);
    if (!ba.valid) {
      return std::nullopt;
    }

    auto make_succ_consts = [&](const BlockAnalysis& cur_ba) {
      absl::flat_hash_map<Register, int64_t> m = block_consts[b_id];
      if (cur_ba.delay_slot_sets_const) {
        m[cur_ba.delay_slot_const_reg] = cur_ba.delay_slot_const_val;
      }
      return m;
    };
    auto make_succ_splits = [&](const BlockAnalysis& cur_ba) {
      absl::flat_hash_map<Register, int64_t> m = block_splits[b_id];
      if (cur_ba.delay_slot_sets_split) {
        m[cur_ba.delay_slot_split_reg] = cur_ba.delay_slot_split_val;
      }
      return m;
    };
    auto make_succ_split_sels = [&](const BlockAnalysis& cur_ba) {
      absl::flat_hash_map<Register, Register> m = block_split_sels[b_id];
      if (cur_ba.delay_slot_sets_split && cur_ba.delay_slot_split_selector != Register::kZero) {
        m[cur_ba.delay_slot_split_reg] = cur_ba.delay_slot_split_selector;
      }
      return m;
    };

    if (ba.is_equality) {
      uint32_t ct = ba.case_target;
      uint32_t cont = ba.cont_target;

      if (decision_blocks.contains(ct)) {
        return std::nullopt;
      }
      if (seen_case_vals.contains(ba.case_val)) {
        return std::nullopt;
      }
      seen_case_vals.insert(ba.case_val);

      if (loop_info_ != nullptr) {
        const Loop* root_loop = loop_info_->GetLoopFor(root_id);
        if (root_loop != nullptr) {
          if (root_loop->header == ct || root_loop->header == cont) {
            return std::nullopt;
          }
        }
      }

      if (!target_to_cases.contains(ct)) {
        target_order.push_back(ct);
      }
      target_to_cases[ct].push_back(ba.case_val);

      auto cont_consts = make_succ_consts(ba);
      auto cont_splits = make_succ_splits(ba);
      auto cont_sels = make_succ_split_sels(ba);
      BlockAnalysis cont_ba = analyze_block(cont, cont_consts, cont_splits, cont_sels, selector);

      if (cont_ba.valid && !visited_.contains(cont) && !decision_blocks.contains(cont)) {
        if (loop_info_ != nullptr) {
          const Loop* cont_loop = loop_info_->GetLoopFor(cont);
          if (cont_loop != loop_info_->GetLoopFor(root_id)) {
            cont_ba.valid = false;
          } else if (cont_loop != nullptr &&
                     (cont_loop->header == cont || cont_loop->IsLatch(cont))) {
            cont_ba.valid = false;
          }
        }
      }

      bool continuation_is_decision_block =
          (cont_ba.valid && !visited_.contains(cont) && !decision_blocks.contains(cont));

      DecisionCase dc;
      dc.case_values = {ba.case_val};
      dc.target_block = ct;
      if (!continuation_is_decision_block && ba.delay_slot_sets_const) {
        const auto* target_block = cfg_->GetBlock(ct);
        bool target_uses_constant_register = false;
        if (target_block != nullptr) {
          for (const auto& instruction : target_block->instructions) {
            RegisterUseDef target_instruction_use_definition = GetInstructionUseDef(instruction);
            for (Register used_register : target_instruction_use_definition.gpr_uses) {
              if (used_register == ba.delay_slot_const_reg) {
                target_uses_constant_register = true;
                break;
              }
            }
            if (target_uses_constant_register) break;
            for (Register defined_register : target_instruction_use_definition.gpr_defs) {
              if (defined_register == ba.delay_slot_const_reg) {
                break;
              }
            }
          }
        }
        if (target_uses_constant_register) {
          dc.const_reg = ba.delay_slot_const_reg;
          dc.const_val = ba.delay_slot_const_val;
        }
      }
      if (!ba.delay_slot_sets_split && ba.delay_instruction.has_value()) {
        dc.delay_instructions.push_back(*ba.delay_instruction);
      }
      decision_cases.push_back(std::move(dc));

      if (continuation_is_decision_block) {
        decision_blocks.insert(cont);
        block_consts[cont] = std::move(cont_consts);
        block_splits[cont] = std::move(cont_splits);
        block_split_sels[cont] = std::move(cont_sels);
        queue.push_back(cont);
      } else {
        uint32_t eff_cont = resolve_trampoline(cont);
        const auto* cont_blk = cfg_->GetBlock(cont);
        bool continuation_has_valid_delay_slot = false;
        if (cont_blk != nullptr && cont_blk->instructions.size() >= 2 &&
            !cont_blk->instructions.back().IsNop()) {
          const auto& ds = cont_blk->instructions.back();
          if (ds.opcode == Opcode::kAddiu && ds.rs == Register::kZero && ds.rt.has_value()) {
            default_const_reg = *ds.rt;
            default_const_val = static_cast<int16_t>(ds.immediate);
            continuation_has_valid_delay_slot = true;
          } else if (ds.opcode == Opcode::kAddu && ds.rs == Register::kZero &&
                     ds.rt == Register::kZero && ds.rd.has_value()) {
            default_const_reg = *ds.rd;
            default_const_val = 0;
            continuation_has_valid_delay_slot = true;
          } else if (ds.opcode == Opcode::kOri && ds.rs == Register::kZero && ds.rt.has_value()) {
            default_const_reg = *ds.rt;
            default_const_val = static_cast<uint16_t>(ds.immediate);
            continuation_has_valid_delay_slot = true;
          }
          if (default_delay_instructions.empty()) {
            default_delay_instructions.push_back(ds);
          }
        }
        if (!continuation_has_valid_delay_slot && ba.delay_slot_sets_const) {
          default_const_reg = ba.delay_slot_const_reg;
          default_const_val = ba.delay_slot_const_val;
        }
        if (!continuation_has_valid_delay_slot && ba.delay_instruction.has_value() &&
            default_delay_instructions.empty()) {
          default_delay_instructions.push_back(*ba.delay_instruction);
        }
        if (!default_target.has_value()) {
          default_target = eff_cont;
          if (eff_cont != cont) {
            decision_blocks.insert(cont);
          }
        } else if (*default_target != eff_cont) {
          return std::nullopt;
        } else if (eff_cont != cont) {
          decision_blocks.insert(cont);
        }
      }
    } else if (ba.is_range_split) {
      if (loop_info_ != nullptr) {
        const Loop* root_loop = loop_info_->GetLoopFor(root_id);
        if (root_loop != nullptr) {
          if (root_loop->header == ba.split_taken || root_loop->header == ba.split_ft) {
            return std::nullopt;
          }
        }
      }

      auto succ_c = make_succ_consts(ba);
      auto succ_s = make_succ_splits(ba);
      auto succ_sels = make_succ_split_sels(ba);

      BlockAnalysis ba_taken = analyze_block(ba.split_taken, succ_c, succ_s, succ_sels, selector);
      BlockAnalysis ba_ft = analyze_block(ba.split_ft, succ_c, succ_s, succ_sels, selector);

      if (ba_taken.valid && !visited_.contains(ba.split_taken) &&
          !decision_blocks.contains(ba.split_taken)) {
        if (loop_info_ != nullptr) {
          const Loop* taken_loop = loop_info_->GetLoopFor(ba.split_taken);
          if (taken_loop != loop_info_->GetLoopFor(root_id)) {
            ba_taken.valid = false;
          } else if (taken_loop != nullptr && (taken_loop->header == ba.split_taken ||
                                               taken_loop->IsLatch(ba.split_taken))) {
            ba_taken.valid = false;
          }
        }
      }
      if (ba_taken.valid && !visited_.contains(ba.split_taken) &&
          !decision_blocks.contains(ba.split_taken)) {
        decision_blocks.insert(ba.split_taken);
        block_consts[ba.split_taken] = succ_c;
        block_splits[ba.split_taken] = succ_s;
        block_split_sels[ba.split_taken] = succ_sels;
        queue.push_back(ba.split_taken);
      } else {
        return std::nullopt;
      }

      if (ba_ft.valid && !visited_.contains(ba.split_ft) &&
          !decision_blocks.contains(ba.split_ft)) {
        if (loop_info_ != nullptr) {
          const Loop* ft_loop = loop_info_->GetLoopFor(ba.split_ft);
          if (ft_loop != loop_info_->GetLoopFor(root_id)) {
            ba_ft.valid = false;
          } else if (ft_loop != nullptr &&
                     (ft_loop->header == ba.split_ft || ft_loop->IsLatch(ba.split_ft))) {
            ba_ft.valid = false;
          }
        }
      }
      if (ba_ft.valid && !visited_.contains(ba.split_ft) &&
          !decision_blocks.contains(ba.split_ft)) {
        decision_blocks.insert(ba.split_ft);
        block_consts[ba.split_ft] = succ_c;
        block_splits[ba.split_ft] = succ_s;
        block_split_sels[ba.split_ft] = succ_sels;
        queue.push_back(ba.split_ft);
      } else {
        return std::nullopt;
      }
    }
  }

  size_t total_cases = 0;
  for (const auto& [_, vals] : target_to_cases) {
    total_cases += vals.size();
  }

  if (total_cases < 2 || total_cases > 5 || target_to_cases.size() < 2) {
    return std::nullopt;
  }

  for (uint32_t db : decision_blocks) {
    if (db == root_id) continue;
    const auto* blk = cfg_->GetBlock(db);
    if (blk == nullptr) return std::nullopt;
    for (uint32_t pred : blk->predecessors) {
      if (!decision_blocks.contains(pred)) {
        return std::nullopt;
      }
    }
  }

  if (default_target.has_value() && !target_to_cases.contains(*default_target)) {
    target_order.push_back(*default_target);
  }

  std::sort(decision_cases.begin(), decision_cases.end(),
            [](const DecisionCase& a, const DecisionCase& b) {
              if (a.case_values.empty() || b.case_values.empty()) return false;
              return a.case_values[0] < b.case_values[0];
            });

  if (default_target.has_value()) {
    DecisionCase def_dc;
    def_dc.is_default = true;
    def_dc.target_block = *default_target;
    def_dc.const_reg = default_const_reg;
    def_dc.const_val = default_const_val;
    def_dc.delay_instructions = std::move(default_delay_instructions);
    decision_cases.push_back(std::move(def_dc));
  }

  DecisionTreeSwitchInfo info;
  info.selector_reg = selector;
  info.decision_blocks = std::move(decision_blocks);
  info.unique_target_blocks = std::move(target_order);
  info.target_to_cases = std::move(target_to_cases);
  info.default_target_block = default_target;
  info.cases = std::move(decision_cases);
  return info;
}

std::optional<FloatToUintInfo> ControlFlowStructurer::DetectFloatToUintDiamond(
    uint32_t entry, uint32_t succ_taken, uint32_t succ_fallthrough, std::optional<uint32_t> join) {
  const auto* b_entry = cfg_->GetBlock(entry);
  if (b_entry == nullptr || b_entry->instructions.empty()) return std::nullopt;

  const auto* term = b_entry->Terminator();
  if (term == nullptr) return std::nullopt;
  bool is_cop1_branch = (term->opcode == Opcode::kBc1t || term->opcode == Opcode::kBc1tl ||
                         term->opcode == Opcode::kBc1f || term->opcode == Opcode::kBc1fl);
  if (!is_cop1_branch) return std::nullopt;

  bool has_float_cmp = false;
  bool is_double = false;
  FpRegister src_fp = FpRegister::kF0;
  FpRegister const_fp = FpRegister::kF0;

  absl::flat_hash_set<Register> regs_with_4f00;
  for (const auto& inst : b_entry->instructions) {
    if (inst.opcode == Opcode::kLui && inst.rt.has_value()) {
      if ((inst.immediate & 0xFFFF) == 0x4F00 || (inst.immediate & 0xFFFF) == 0x41E0) {
        regs_with_4f00.insert(*inst.rt);
      }
    }
    if ((inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kDmtc1) && inst.rt.has_value() &&
        inst.fs.has_value()) {
      if (regs_with_4f00.contains(*inst.rt)) {
        const_fp = *inst.fs;
      }
    }
  }

  for (const auto& inst : b_entry->instructions) {
    if (inst.opcode == Opcode::kCLeS || inst.opcode == Opcode::kCLtS ||
        inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kCLtD) {
      has_float_cmp = true;
      is_double = (inst.opcode == Opcode::kCLeD || inst.opcode == Opcode::kCLtD);
      if (inst.fs.has_value() && inst.ft.has_value()) {
        if (*inst.fs == const_fp) {
          src_fp = *inst.ft;
        } else if (*inst.ft == const_fp) {
          src_fp = *inst.fs;
        } else {
          src_fp = *inst.ft;
        }
      }
      break;
    }
  }
  if (!has_float_cmp) return std::nullopt;

  const auto* b_taken = cfg_->GetBlock(succ_taken);
  const auto* b_ft = cfg_->GetBlock(succ_fallthrough);
  if (b_taken == nullptr || b_ft == nullptr) return std::nullopt;

  auto check_low_path = [&](const BasicBlock* blk, Register* dest_reg) -> bool {
    if (blk == nullptr) return false;
    bool has_trunc = false;
    Register curr_dest = Register::kZero;
    for (const auto& inst : blk->instructions) {
      if (inst.IsNop()) continue;
      if (inst.IsBranch() || inst.IsJump()) continue;
      if (inst.opcode == Opcode::kTruncWS || inst.opcode == Opcode::kCvtWS ||
          inst.opcode == Opcode::kTruncWD || inst.opcode == Opcode::kCvtWD) {
        has_trunc = true;
      } else if (inst.opcode == Opcode::kMfc1 && inst.rt.has_value()) {
        curr_dest = *inst.rt;
      } else if (curr_dest != Register::kZero && inst.opcode == Opcode::kAddu &&
                 inst.rd.has_value() &&
                 ((inst.rs.has_value() && *inst.rs == curr_dest && inst.rt.has_value() &&
                   *inst.rt == Register::kZero) ||
                  (inst.rt.has_value() && *inst.rt == curr_dest && inst.rs.has_value() &&
                   *inst.rs == Register::kZero))) {
        curr_dest = *inst.rd;
      } else {
        return false;
      }
    }
    if (has_trunc && curr_dest != Register::kZero) {
      *dest_reg = curr_dest;
      return true;
    }
    return false;
  };

  auto check_high_path = [&](const BasicBlock* blk, Register* dest_reg) -> bool {
    if (blk == nullptr) return false;
    bool has_sub = false;
    bool has_trunc = false;
    bool has_or_mask = false;
    Register curr_dest = Register::kZero;
    if (!b_entry->instructions.empty()) {
      const auto& delay_inst = b_entry->instructions.back();
      if (delay_inst.opcode == Opcode::kSubS || delay_inst.opcode == Opcode::kSubD) {
        has_sub = true;
      }
    }
    for (const auto& inst : blk->instructions) {
      if (inst.IsNop()) continue;
      if (inst.IsBranch() || inst.IsJump()) continue;
      if (inst.opcode == Opcode::kSubS || inst.opcode == Opcode::kSubD) {
        has_sub = true;
      } else if (inst.opcode == Opcode::kTruncWS || inst.opcode == Opcode::kCvtWS ||
                 inst.opcode == Opcode::kTruncWD || inst.opcode == Opcode::kCvtWD) {
        has_trunc = true;
      } else if (inst.opcode == Opcode::kMfc1 && inst.rt.has_value()) {
        curr_dest = *inst.rt;
      } else if (inst.opcode == Opcode::kLui && inst.rt.has_value()) {
        // High bit mask register load
      } else if (inst.opcode == Opcode::kOr || inst.opcode == Opcode::kXor) {
        has_or_mask = true;
        if (inst.rd.has_value()) {
          curr_dest = *inst.rd;
        }
      } else if (inst.opcode == Opcode::kAddu) {
        if (curr_dest != Register::kZero && inst.rd.has_value() &&
            ((inst.rs.has_value() && *inst.rs == curr_dest && inst.rt.has_value() &&
              *inst.rt == Register::kZero) ||
             (inst.rt.has_value() && *inst.rt == curr_dest && inst.rs.has_value() &&
              *inst.rs == Register::kZero))) {
          curr_dest = *inst.rd;
        } else if (inst.rs.has_value() && *inst.rs != Register::kZero && inst.rt.has_value() &&
                   *inst.rt != Register::kZero) {
          has_or_mask = true;
          if (inst.rd.has_value()) {
            curr_dest = *inst.rd;
          }
        } else {
          return false;
        }
      } else {
        return false;
      }
    }
    if (has_sub && has_trunc && has_or_mask && curr_dest != Register::kZero) {
      *dest_reg = curr_dest;
      return true;
    }
    return false;
  };

  Register dest_low = Register::kZero;
  Register dest_high = Register::kZero;

  bool taken_is_high = check_high_path(b_taken, &dest_high) && check_low_path(b_ft, &dest_low);
  bool taken_is_low = check_low_path(b_taken, &dest_low) && check_high_path(b_ft, &dest_high);

  if (!taken_is_high && !taken_is_low) return std::nullopt;
  if (dest_low == Register::kZero || dest_low != dest_high) return std::nullopt;

  uint32_t join_id = join.has_value() ? *join : 0;
  if (join_id == 0) {
    if (!b_taken->successors.empty() && !b_ft->successors.empty() &&
        b_taken->successors[0] == b_ft->successors[0]) {
      join_id = b_taken->successors[0];
    }
  }

  FloatToUintInfo info;
  info.entry_block_id = entry;
  info.join_block_id = join_id;
  info.source_float_register = src_fp;
  info.destination_integer_register = dest_low;
  info.is_double = is_double;
  return info;
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
    auto dt_switch = DetectDecisionTreeSwitch(entry);
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

    // Short-Circuit OR (||): entry branches to succ_taken; succ_fallthrough also reaches succ_taken
    if (!visited_.contains(succ_fallthrough) && HasNopBranchDelaySlot(*cfg_, entry)) {
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
            if (HasNopBranchDelaySlot(*cfg_, curr_id) && next_b != nullptr &&
                next_b->successors.size() == 2 && next_b->predecessors.size() == 1 &&
                !visited_.contains(other) && IsPureConditionBlock(*cfg_, other) &&
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
    if (!visited_.contains(succ_fallthrough) && HasNopBranchDelaySlot(*cfg_, entry)) {
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
            if (HasNopBranchDelaySlot(*cfg_, curr_id) && next_b != nullptr &&
                next_b->successors.size() == 2 && next_b->predecessors.size() == 1 &&
                !visited_.contains(other) && IsPureConditionBlock(*cfg_, other) &&
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
      // In GCC, the then-block immediately follows the branch (fallthrough),
      // and the branch jumps to the else-block on !cond.
      auto if_else = CreateIfThenElse(entry, /*invert=*/true);
      auto then_body = StructureRegion(succ_fallthrough, join, current_loop);
      auto else_body = StructureRegion(succ_taken, join, current_loop);

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
