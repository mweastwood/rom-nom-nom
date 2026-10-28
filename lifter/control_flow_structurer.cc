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
#include "lifter/control_flow_pattern_matcher.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

std::unique_ptr<StructuredRegion> StructuredRegion::CreateBlock(uint32_t block_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kBlock;
  r->block_id = block_id;
  return r;
}

std::unique_ptr<StructuredRegion> StructuredRegion::CreateBreak(uint32_t target_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kBreak;
  r->block_id = target_id;
  return r;
}

std::unique_ptr<StructuredRegion> StructuredRegion::CreateContinue(uint32_t target_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kContinue;
  r->block_id = target_id;
  return r;
}

std::unique_ptr<StructuredRegion> StructuredRegion::CreateGoto(uint32_t target_id) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kGoto;
  r->block_id = target_id;
  return r;
}

std::unique_ptr<StructuredRegion> StructuredRegion::CreateSequence() {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kSequence;
  return r;
}

std::unique_ptr<StructuredRegion> StructuredRegion::CreateIfThen(uint32_t condition_block_id,
                                                                 bool invert) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kIfThen;
  r->condition_block_id = condition_block_id;
  r->invert_condition = invert;
  return r;
}

std::unique_ptr<StructuredRegion> StructuredRegion::CreateIfThenElse(uint32_t condition_block_id,
                                                                     bool invert) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kIfThenElse;
  r->condition_block_id = condition_block_id;
  r->invert_condition = invert;
  return r;
}

std::unique_ptr<StructuredRegion> StructuredRegion::CreateLoop(LoopType type, uint32_t header) {
  auto r = std::make_unique<StructuredRegion>();
  r->type = RegionType::kLoop;
  r->loop_type = type;
  r->loop_header = header;
  return r;
}

std::unique_ptr<StructuredRegion> StructuredRegion::CreateSwitch(uint32_t block_id,
                                                                 const JumpTable* jt,
                                                                 Register reg) {
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

namespace {

using CreateBlock = StructuredRegion;

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

bool RegionEndsWithReturn(const StructuredRegion& r, const ControlFlowGraph& cfg) {
  switch (r.type) {
    case RegionType::kBlock: {
      const auto* blk = cfg.GetBlock(r.block_id);
      return blk != nullptr && blk->successors.empty() && blk->Terminator() != nullptr &&
             blk->Terminator()->IsReturn();
    }
    case RegionType::kSequence:
      for (auto it = r.children.rbegin(); it != r.children.rend(); ++it) {
        if (*it != nullptr) {
          return RegionEndsWithReturn(**it, cfg);
        }
      }
      return false;
    default:
      return false;
  }
}

bool CheckNonReturningPostDominance(const ControlFlowGraph& cfg, const DominatorTree* dom_tree,
                                    const LoopInfo* loop_info,
                                    const absl::flat_hash_set<uint32_t>& visited, uint32_t entry,
                                    uint32_t start, uint32_t candidate_join,
                                    const Loop* current_loop, bool require_reaches_candidate) {
  if (candidate_join == entry || visited.contains(candidate_join)) {
    return false;
  }
  if (current_loop != nullptr && !current_loop->Contains(candidate_join)) {
    return false;
  }
  const auto* cand_blk = cfg.GetBlock(candidate_join);
  if (cand_blk == nullptr || IsPureEpilogueReturnBlock(*cand_blk)) {
    return false;
  }

  absl::flat_hash_set<uint32_t> downstream;
  std::vector<uint32_t> worklist = {candidate_join};
  while (!worklist.empty()) {
    uint32_t u = worklist.back();
    worklist.pop_back();
    const auto* u_blk = cfg.GetBlock(u);
    if (u_blk == nullptr || IsPureEpilogueReturnBlock(*u_blk)) continue;
    if (!downstream.insert(u).second) continue;
    for (uint32_t v : u_blk->successors) {
      if (dom_tree != nullptr && dom_tree->Dominates(v, u)) continue;
      worklist.push_back(v);
    }
  }

  absl::flat_hash_set<uint32_t> branch_blocks;
  bool reaches_candidate = false;
  bool reaches_return = false;
  worklist.clear();
  worklist.push_back(start);
  while (!worklist.empty()) {
    uint32_t u = worklist.back();
    worklist.pop_back();
    if (u == candidate_join) {
      reaches_candidate = true;
      continue;
    }
    const auto* u_blk = cfg.GetBlock(u);
    if (u_blk == nullptr) return false;
    if (IsPureEpilogueReturnBlock(*u_blk)) {
      reaches_return = true;
      continue;
    }
    if (u == entry || visited.contains(u) || downstream.contains(u)) {
      return false;
    }
    if (current_loop != nullptr && !current_loop->Contains(u)) {
      return false;
    }
    if (!branch_blocks.insert(u).second) continue;
    if (u_blk->successors.empty()) {
      const Instruction* term = u_blk->Terminator();
      if (term == nullptr || !term->IsReturn()) return false;
      reaches_return = true;
      continue;
    }
    for (uint32_t v : u_blk->successors) {
      worklist.push_back(v);
    }
  }

  if (!reaches_return) return false;
  if (require_reaches_candidate && !reaches_candidate) return false;

  if (reaches_candidate) {
    absl::flat_hash_set<uint32_t> can_reach_cand = {candidate_join};
    bool added = true;
    while (added) {
      added = false;
      for (uint32_t u : branch_blocks) {
        if (can_reach_cand.contains(u)) continue;
        const auto* u_blk = cfg.GetBlock(u);
        if (u_blk == nullptr) continue;
        for (uint32_t v : u_blk->successors) {
          if (can_reach_cand.contains(v)) {
            can_reach_cand.insert(u);
            added = true;
            break;
          }
        }
      }
    }
    for (uint32_t u : branch_blocks) {
      if (loop_info != nullptr && loop_info->GetLoopFor(u) != current_loop) {
        continue;
      }
      const auto* u_blk = cfg.GetBlock(u);
      if (u_blk == nullptr || u_blk->successors.size() != 2) continue;
      uint32_t s0 = u_blk->successors[0];
      uint32_t s1 = u_blk->successors[1];
      bool r0 = can_reach_cand.contains(s0);
      bool r1 = can_reach_cand.contains(s1);
      if (r0 != r1) {
        uint32_t reaching_succ = r0 ? s0 : s1;
        if (reaching_succ != candidate_join) {
          return false;
        }
      }
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

std::unique_ptr<StructuredRegion> LoopStructurer::TryStructure(ControlFlowStructurer* structurer,
                                                               uint32_t entry,
                                                               std::optional<uint32_t> follow,
                                                               const Loop* current_loop) const {
  const Loop* loop = structurer->Loops().GetLoopFor(entry);
  if (loop != nullptr && loop != current_loop && loop->header == entry) {
    return structurer->StructureLoop(loop, follow, current_loop);
  }
  return nullptr;
}

std::unique_ptr<StructuredRegion> SwitchStructurer::TryStructure(ControlFlowStructurer* structurer,
                                                                 uint32_t entry,
                                                                 std::optional<uint32_t> follow,
                                                                 const Loop* current_loop) const {
  if (auto jt_switch = structurer->StructureJumpTableSwitch(entry, follow, current_loop);
      jt_switch != nullptr) {
    return jt_switch;
  }
  return structurer->StructureDecisionTreeSwitch(entry, follow, current_loop);
}

std::unique_ptr<StructuredRegion> ShortCircuitStructurer::TryStructure(
    ControlFlowStructurer* structurer, uint32_t entry, std::optional<uint32_t> follow,
    const Loop* current_loop) const {
  return structurer->StructureShortCircuitConditional(entry, follow, current_loop);
}

std::unique_ptr<StructuredRegion> ConditionalDiamondStructurer::TryStructure(
    ControlFlowStructurer* structurer, uint32_t entry, std::optional<uint32_t> follow,
    const Loop* current_loop) const {
  return structurer->StructureConditionalBranch(entry, follow, current_loop);
}

RegionPassPipeline RegionPassPipeline::BuildDefaultPipeline() {
  return RegionPassPipeline{};
}

void RegionPassPipeline::AddPass(std::unique_ptr<RegionPass> pass) {
  if (pass != nullptr) {
    passes_.push_back(std::move(pass));
  }
}

bool RegionPassPipeline::Run(StructuredRegion* root, const ControlFlowGraph& cfg) const {
  if (root == nullptr) return false;
  bool any_changed = false;
  for (const auto& pass : passes_) {
    if (pass->Run(root, cfg)) {
      any_changed = true;
    }
  }
  return any_changed;
}

ControlFlowStructurer::ControlFlowStructurer() {
  strategies_.push_back(std::make_unique<LoopStructurer>());
  strategies_.push_back(std::make_unique<SwitchStructurer>());
  strategies_.push_back(std::make_unique<ShortCircuitStructurer>());
  strategies_.push_back(std::make_unique<ConditionalDiamondStructurer>());
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

  auto root = structurer.StructureRegion(cfg.EntryBlockId(), std::nullopt, nullptr);
  if (root != nullptr) {
    RegionPassPipeline pass_pipeline = RegionPassPipeline::BuildDefaultPipeline();
    pass_pipeline.Run(root.get(), cfg);
  }
  return root;
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::StructureLoop(
    const Loop* loop, std::optional<uint32_t> follow, const Loop* parent_loop) {
  auto loop_region = StructuredRegion::CreateLoop(loop->type, loop->header);

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
      loop_region->children.push_back(StructuredRegion::CreateBlock(loop->header));
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
      loop_region->children.push_back(StructuredRegion::CreateBlock(loop->header));
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
      auto seq = StructuredRegion::CreateSequence();
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

std::optional<uint32_t> ControlFlowStructurer::ComputeConditionalJoinBlock(
    uint32_t entry, std::optional<uint32_t> follow, const Loop* current_loop) const {
  std::optional<uint32_t> join =
      (post_dom_tree_ != nullptr) ? post_dom_tree_->ImmediateDominator(entry) : std::nullopt;
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
  return join;
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::StructureJumpTableSwitch(
    uint32_t entry, std::optional<uint32_t> follow, const Loop* current_loop) {
  const auto* block = cfg_->GetBlock(entry);
  if (block == nullptr) return nullptr;

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

  if (switch_jt == nullptr) return nullptr;

  if (has_bounds_block) {
    visited_.insert(switch_dispatch_block_id);
  }

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

  auto switch_region = StructuredRegion::CreateSwitch(switch_dispatch_block_id, switch_jt);
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
    auto seq = StructuredRegion::CreateSequence();
    seq->children.push_back(StructuredRegion::CreateBlock(entry));
    seq->children.push_back(std::move(switch_region));
    final_region = std::move(seq);
  } else {
    final_region = std::move(switch_region);
  }

  if (join.has_value() && join != follow) {
    auto after_switch = StructureRegion(*join, follow, current_loop);
    if (after_switch != nullptr) {
      auto seq = StructuredRegion::CreateSequence();
      FlattenSequence(seq.get(), std::move(final_region));
      FlattenSequence(seq.get(), std::move(after_switch));
      return seq;
    }
  }

  return final_region;
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::StructureDecisionTreeSwitch(
    uint32_t entry, std::optional<uint32_t> follow, const Loop* current_loop) {
  const auto* block = cfg_->GetBlock(entry);
  if (block == nullptr || block->successors.size() != 2) return nullptr;

  std::optional<DecisionTreeSwitchInfo> dt_switch =
      DetectDecisionTreeSwitch(*cfg_, loop_info_, visited_, entry);
  if (dt_switch.has_value() && current_switch_break_.has_value() &&
      dt_switch->target_to_cases.contains(*current_switch_break_)) {
    dt_switch = std::nullopt;
  }
  if (!dt_switch.has_value()) return nullptr;

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

  auto switch_region = StructuredRegion::CreateSwitch(entry, nullptr, dt_switch->selector_reg);
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
      auto seq = StructuredRegion::CreateSequence();
      seq->children.push_back(std::move(switch_region));
      FlattenSequence(seq.get(), std::move(after_switch));
      return seq;
    }
  }

  return switch_region;
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::StructureShortCircuitConditional(
    uint32_t entry, std::optional<uint32_t> follow, const Loop* current_loop) {
  const auto* block = cfg_->GetBlock(entry);
  if (block == nullptr || block->successors.size() != 2) return nullptr;

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

  if (current_switch_break_.has_value() &&
      (succ_taken == *current_switch_break_ || succ_fallthrough == *current_switch_break_)) {
    return nullptr;
  }
  if (current_loop != nullptr && !current_loop->IsLatch(entry) &&
      (current_loop->IsExitBlock(succ_taken) != current_loop->IsExitBlock(succ_fallthrough))) {
    return nullptr;
  }

  std::optional<uint32_t> join = ComputeConditionalJoinBlock(entry, follow, current_loop);
  if (DetectFloatToUintDiamond(entry, succ_taken, succ_fallthrough, join).has_value()) {
    return nullptr;
  }

  auto sc_chain =
      TryCollapseShortCircuitChain(*cfg_, visited_, entry, succ_taken, succ_fallthrough, join);
  if (!sc_chain.has_value()) return nullptr;

  for (uint32_t b_id : sc_chain->additional_condition_blocks) {
    visited_.insert(b_id);
  }
  if (join.has_value() && sc_chain->else_target_block.has_value() &&
      *sc_chain->else_target_block == *join) {
    auto if_then = StructuredRegion::CreateIfThen(entry, sc_chain->root_invert);
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
        auto seq = StructuredRegion::CreateSequence();
        seq->children.push_back(std::move(if_then));
        FlattenSequence(seq.get(), std::move(after_region));
        return seq;
      }
    }
    return if_then;
  }

  auto if_else = StructuredRegion::CreateIfThenElse(entry, sc_chain->root_invert);
  if_else->condition_op = sc_chain->condition_op;
  if_else->additional_condition_blocks = std::move(sc_chain->additional_condition_blocks);
  if_else->additional_invert_conditions = std::move(sc_chain->additional_invert_conditions);
  auto then_body = StructureRegion(sc_chain->then_target_block, join, current_loop);
  auto else_body = sc_chain->else_target_block.has_value()
                       ? StructureRegion(*sc_chain->else_target_block, join, current_loop)
                       : nullptr;
  if_else->children.push_back(then_body ? std::move(then_body)
                                        : StructuredRegion::CreateSequence());
  if_else->children.push_back(else_body ? std::move(else_body)
                                        : StructuredRegion::CreateSequence());

  if (join.has_value() && join != follow) {
    auto after_region = StructureRegion(*join, follow, current_loop);
    if (after_region) {
      auto seq = StructuredRegion::CreateSequence();
      seq->children.push_back(std::move(if_else));
      FlattenSequence(seq.get(), std::move(after_region));
      return seq;
    }
  }
  return if_else;
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::StructureConditionalBranch(
    uint32_t entry, std::optional<uint32_t> follow, const Loop* current_loop) {
  const auto* block = cfg_->GetBlock(entry);
  if (block == nullptr || block->successors.size() != 2) return nullptr;

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
      auto if_break = StructuredRegion::CreateIfThen(entry, /*invert=*/false);
      if_break->children.push_back(StructuredRegion::CreateBreak(succ_taken));
      auto next = StructureRegion(succ_fallthrough, follow, current_loop);
      if (next) {
        auto seq = StructuredRegion::CreateSequence();
        seq->children.push_back(std::move(if_break));
        FlattenSequence(seq.get(), std::move(next));
        return seq;
      }
      return if_break;
    }
    if (succ_fallthrough == *current_switch_break_ && succ_taken != *current_switch_break_) {
      auto if_break = StructuredRegion::CreateIfThen(entry, /*invert=*/true);
      if_break->children.push_back(StructuredRegion::CreateBreak(succ_fallthrough));
      auto next = StructureRegion(succ_taken, follow, current_loop);
      if (next) {
        auto seq = StructuredRegion::CreateSequence();
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
      const auto* target_blk = cfg_->GetBlock(target_id);
      if (target_blk != nullptr && IsPureEpilogueReturnBlock(*target_blk)) return false;
      const auto* lf_blk = cfg_->GetBlock(*loop_follow_target);
      return lf_blk != nullptr && lf_blk->successors.size() == 1 &&
             lf_blk->successors[0] == target_id;
    };
    auto is_empty_loop_follow_jump_to = [&](uint32_t target_id) {
      if (!is_successor_of_loop_follow(target_id) || current_loop->parent == nullptr ||
          target_id != current_loop->parent->header) {
        return false;
      }
      const auto* lf_blk = cfg_->GetBlock(*loop_follow_target);
      if (lf_blk->instructions.empty()) return false;
      for (const auto& inst : lf_blk->instructions) {
        if (!inst.IsNop() && !inst.IsJump() && !inst.IsUnconditionalBranch()) {
          return false;
        }
      }
      return true;
    };
    auto is_ancestor_loop_follow = [&](uint32_t target_id) {
      const auto* target_blk = cfg_->GetBlock(target_id);
      if (target_blk != nullptr && IsPureEpilogueReturnBlock(*target_blk)) return false;
      for (const Loop* anc = current_loop->parent; anc != nullptr; anc = anc->parent) {
        auto anc_it = loop_follow_map_.find(anc);
        if (anc_it != loop_follow_map_.end() && anc_it->second == target_id) {
          return true;
        }
      }
      return false;
    };
    if (current_loop->IsExitBlock(succ_taken) && !current_loop->IsExitBlock(succ_fallthrough)) {
      auto if_break = StructuredRegion::CreateIfThen(entry, /*invert=*/false);
      if ((follow.has_value() && succ_taken == *follow) ||
          (lf_it != loop_follow_map_.end() && succ_taken == lf_it->second) ||
          is_empty_loop_follow_jump_to(succ_taken)) {
        if_break->children.push_back(StructuredRegion::CreateBreak(succ_taken));
      } else if ((current_loop->parent != nullptr && visited_.contains(succ_taken) &&
                  succ_taken == current_loop->parent->header) ||
                 is_successor_of_loop_follow(succ_taken) || is_ancestor_loop_follow(succ_taken)) {
        if_break->children.push_back(StructuredRegion::CreateGoto(succ_taken));
      } else {
        auto exit_body = StructureRegion(succ_taken, loop_follow_target, nullptr);
        if (exit_body) {
          if (RegionEndsWithReturn(*exit_body, *cfg_)) {
            if_break->children.push_back(std::move(exit_body));
          } else {
            auto seq = StructuredRegion::CreateSequence();
            seq->children.push_back(std::move(exit_body));
            seq->children.push_back(StructuredRegion::CreateBreak(succ_taken));
            if_break->children.push_back(std::move(seq));
          }
        } else {
          if_break->children.push_back(StructuredRegion::CreateBreak(succ_taken));
        }
      }
      auto next = StructureRegion(succ_fallthrough, follow, current_loop);
      if (next) {
        auto seq = StructuredRegion::CreateSequence();
        seq->children.push_back(std::move(if_break));
        FlattenSequence(seq.get(), std::move(next));
        return seq;
      }
      return if_break;
    }
    if (current_loop->IsExitBlock(succ_fallthrough) && !current_loop->IsExitBlock(succ_taken)) {
      auto if_break = StructuredRegion::CreateIfThen(entry, /*invert=*/true);
      if ((follow.has_value() && succ_fallthrough == *follow) ||
          (lf_it != loop_follow_map_.end() && succ_fallthrough == lf_it->second) ||
          is_empty_loop_follow_jump_to(succ_fallthrough)) {
        if_break->children.push_back(StructuredRegion::CreateBreak(succ_fallthrough));
      } else if ((current_loop->parent != nullptr && visited_.contains(succ_fallthrough) &&
                  succ_fallthrough == current_loop->parent->header) ||
                 is_successor_of_loop_follow(succ_fallthrough) ||
                 is_ancestor_loop_follow(succ_fallthrough)) {
        if_break->children.push_back(StructuredRegion::CreateGoto(succ_fallthrough));
      } else {
        auto exit_body = StructureRegion(succ_fallthrough, loop_follow_target, nullptr);
        const auto* sf_blk = cfg_->GetBlock(succ_fallthrough);
        if (block->instructions.size() >= 2 &&
            !block->instructions[block->instructions.size() - 2].IsBranchLikely() &&
            block->instructions.back().IsNop() &&
            !block->instructions.back().was_unscheduled_delay_slot && sf_blk != nullptr &&
            sf_blk->successors.size() == 1) {
          uint32_t ret_id = sf_blk->successors[0];
          const auto* ret_blk = cfg_->GetBlock(ret_id);
          if (ret_blk != nullptr && IsPureEpilogueReturnBlock(*ret_blk) &&
              ret_blk->instructions.size() == 2) {
            visited_.insert(ret_id);
          }
        }
        if (exit_body) {
          if (RegionEndsWithReturn(*exit_body, *cfg_)) {
            if (follow.has_value() && succ_taken == *follow) {
              auto if_else = StructuredRegion::CreateIfThenElse(entry, /*invert=*/false);
              if_else->children.push_back(StructuredRegion::CreateSequence());
              if_else->children.push_back(std::move(exit_body));
              return if_else;
            }
            if_break->children.push_back(std::move(exit_body));
          } else {
            auto seq = StructuredRegion::CreateSequence();
            seq->children.push_back(std::move(exit_body));
            seq->children.push_back(StructuredRegion::CreateBreak(succ_fallthrough));
            if_break->children.push_back(std::move(seq));
          }
        } else {
          if_break->children.push_back(StructuredRegion::CreateBreak(succ_fallthrough));
        }
      }
      auto next = StructureRegion(succ_taken, follow, current_loop);
      if (next) {
        auto seq = StructuredRegion::CreateSequence();
        seq->children.push_back(std::move(if_break));
        FlattenSequence(seq.get(), std::move(next));
        return seq;
      }
      return if_break;
    }

    if (follow.has_value() && current_loop->Contains(*follow) && !current_loop->IsLatch(*follow) &&
        (succ_fallthrough == *follow || succ_taken == *follow)) {
      const uint32_t s_other = (succ_fallthrough == *follow) ? succ_taken : succ_fallthrough;
      const auto* other_blk = cfg_->GetBlock(s_other);
      const bool is_post_dom_pre_latch =
          current_loop->type == LoopType::kDoWhile && other_blk != nullptr &&
          !visited_.contains(s_other) && current_loop->Contains(s_other) &&
          !current_loop->IsLatch(s_other) && other_blk->successors.size() == 1 &&
          !other_blk->HasBranch() && current_loop->IsLatch(other_blk->successors[0]) &&
          post_dom_tree_ != nullptr && post_dom_tree_->Dominates(s_other, *follow);
      if (is_post_dom_pre_latch) {
        auto if_bypass =
            StructuredRegion::CreateIfThen(entry, /*invert=*/(s_other == succ_fallthrough));
        if_bypass->children.push_back(StructuredRegion::CreateGoto(s_other));
        return if_bypass;
      }
    }
  }

  std::optional<uint32_t> join = ComputeConditionalJoinBlock(entry, follow, current_loop);
  bool join_is_epilogue_or_empty = !join.has_value();
  if (join.has_value()) {
    const auto* join_blk = cfg_->GetBlock(*join);
    if (join_blk == nullptr || IsPureEpilogueReturnBlock(*join_blk)) {
      join_is_epilogue_or_empty = true;
    }
  }
  if (current_loop != nullptr && current_loop->Contains(entry) &&
      current_loop->Contains(succ_taken) && current_loop->Contains(succ_fallthrough)) {
    auto is_loop_guard_to_join = [&](uint32_t s_guard, uint32_t s_join) {
      if (visited_.contains(s_guard) || visited_.contains(s_join)) return false;
      const auto* guard_blk = cfg_->GetBlock(s_guard);
      for (int depth = 0; depth < 4 && guard_blk != nullptr &&
                          guard_blk->predecessors.size() == 1 && guard_blk->successors.size() == 1;
           ++depth) {
        uint32_t next_id = guard_blk->successors[0];
        if (next_id == s_join || visited_.contains(next_id) || !current_loop->Contains(next_id) ||
            current_loop->IsLatch(next_id)) {
          return false;
        }
        guard_blk = cfg_->GetBlock(next_id);
      }
      if (guard_blk == nullptr || guard_blk->predecessors.size() != 1 ||
          guard_blk->successors.size() != 2) {
        return false;
      }
      uint32_t s_other = 0;
      if (guard_blk->successors[0] == s_join) {
        s_other = guard_blk->successors[1];
      } else if (guard_blk->successors[1] == s_join) {
        s_other = guard_blk->successors[0];
      } else {
        return false;
      }
      if (current_loop->IsExitBlock(s_other)) {
        return true;
      }
      const auto* other_blk = cfg_->GetBlock(s_other);
      return current_loop->type == LoopType::kDoWhile && other_blk != nullptr &&
             !visited_.contains(s_other) && current_loop->Contains(s_other) &&
             !current_loop->IsLatch(s_other) && other_blk->successors.size() == 1 &&
             !other_blk->HasBranch() && current_loop->IsLatch(other_blk->successors[0]) &&
             s_join != other_blk->successors[0] && post_dom_tree_ != nullptr &&
             post_dom_tree_->Dominates(s_other, s_join);
    };
    if (is_loop_guard_to_join(succ_fallthrough, succ_taken)) {
      join = succ_taken;
      join_is_epilogue_or_empty = false;
    } else if (is_loop_guard_to_join(succ_taken, succ_fallthrough)) {
      join = succ_fallthrough;
      join_is_epilogue_or_empty = false;
    }
  }
  if ((join_is_epilogue_or_empty || (join != succ_fallthrough && join != succ_taken)) &&
      !DetectFloatToUintDiamond(entry, succ_taken, succ_fallthrough, join).has_value()) {
    if (join_is_epilogue_or_empty) {
      if (CheckNonReturningPostDominance(
              *cfg_, dom_tree_, loop_info_, visited_, entry, succ_fallthrough, succ_taken,
              current_loop,
              /*require_reaches_candidate=*/!(follow.has_value() && succ_taken == *follow))) {
        join = succ_taken;
      } else if (
          CheckNonReturningPostDominance(
              *cfg_, dom_tree_, loop_info_, visited_, entry, succ_taken, succ_fallthrough,
              current_loop,
              /*require_reaches_candidate=*/!(follow.has_value() && succ_fallthrough == *follow))) {
        join = succ_fallthrough;
      } else if (join != succ_fallthrough && join != succ_taken) {
        auto try_find_diamond_join = [&](uint32_t s_direct,
                                         uint32_t s_other) -> std::optional<uint32_t> {
          if (visited_.contains(s_direct) || s_direct == entry) return std::nullopt;
          const auto* direct_blk = cfg_->GetBlock(s_direct);
          if (direct_blk == nullptr || direct_blk->predecessors.size() != 1 ||
              direct_blk->successors.size() != 1 || IsPureEpilogueReturnBlock(*direct_blk)) {
            return std::nullopt;
          }
          uint32_t cand = direct_blk->successors[0];
          if (cand == s_other || cand == s_direct) return std::nullopt;
          if (CheckNonReturningPostDominance(*cfg_, dom_tree_, loop_info_, visited_, entry, s_other,
                                             cand, current_loop,
                                             /*require_reaches_candidate=*/true)) {
            return cand;
          }
          return std::nullopt;
        };
        if (auto cand = try_find_diamond_join(succ_fallthrough, succ_taken); cand.has_value()) {
          join = cand;
        } else if (auto cand = try_find_diamond_join(succ_taken, succ_fallthrough);
                   cand.has_value()) {
          join = cand;
        } else if (dom_tree_ != nullptr) {
          for (const auto& blk : cfg_->Blocks()) {
            const uint32_t cand = blk.id;
            if (cand == entry || cand == succ_fallthrough || cand == succ_taken ||
                visited_.contains(cand) || IsPureEpilogueReturnBlock(blk)) {
              continue;
            }
            if (dom_tree_->ImmediateDominator(cand) != entry) {
              continue;
            }
            auto arm_reaches_join = [&](uint32_t s_arm) {
              if (post_dom_tree_ != nullptr && post_dom_tree_->Dominates(cand, s_arm)) {
                return current_loop == nullptr || current_loop->Contains(cand);
              }
              return CheckNonReturningPostDominance(*cfg_, dom_tree_, loop_info_, visited_, entry,
                                                    s_arm, cand, current_loop,
                                                    /*require_reaches_candidate=*/true);
            };
            if (arm_reaches_join(succ_fallthrough) && arm_reaches_join(succ_taken)) {
              join = cand;
              break;
            }
          }
        }
      }
    }
  }

  auto float_diamond = DetectFloatToUintDiamond(entry, succ_taken, succ_fallthrough, join);
  if (float_diamond.has_value()) {
    auto if_else = StructuredRegion::CreateIfThenElse(entry);
    if_else->float_to_uint_info = *float_diamond;
    auto then_body = StructureRegion(succ_taken, join, current_loop);
    auto else_body = StructureRegion(succ_fallthrough, join, current_loop);
    if_else->children.push_back(then_body ? std::move(then_body)
                                          : StructuredRegion::CreateSequence());
    if_else->children.push_back(else_body ? std::move(else_body)
                                          : StructuredRegion::CreateSequence());

    if (join.has_value() && join != follow) {
      auto after_join = StructureRegion(*join, follow, current_loop);
      if (after_join) {
        auto seq = StructuredRegion::CreateSequence();
        seq->children.push_back(std::move(if_else));
        FlattenSequence(seq.get(), std::move(after_join));
        return seq;
      }
    }
    return if_else;
  }

  // Subcase 3a: If-Then (one successor directly is join)
  if (join.has_value() && succ_fallthrough == *join) {
    auto if_then = StructuredRegion::CreateIfThen(entry, /*invert=*/false);
    auto then_body = StructureRegion(succ_taken, join, current_loop);
    if (then_body) {
      if_then->children.push_back(std::move(then_body));
    }

    auto after_join = StructureRegion(*join, follow, current_loop);
    if (after_join) {
      auto seq = StructuredRegion::CreateSequence();
      seq->children.push_back(std::move(if_then));
      FlattenSequence(seq.get(), std::move(after_join));
      return seq;
    }
    return if_then;
  }
  if (join.has_value() && succ_taken == *join) {
    auto if_then = StructuredRegion::CreateIfThen(entry, /*invert=*/true);
    auto then_body = StructureRegion(succ_fallthrough, join, current_loop);
    if (then_body) {
      if_then->children.push_back(std::move(then_body));
    }

    auto after_join = StructureRegion(*join, follow, current_loop);
    if (after_join) {
      auto seq = StructuredRegion::CreateSequence();
      seq->children.push_back(std::move(if_then));
      FlattenSequence(seq.get(), std::move(after_join));
      return seq;
    }
    return if_then;
  }

  // Subcase 3b: If-Then-Else
  auto if_else = StructuredRegion::CreateIfThenElse(entry, /*invert=*/true);
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
      } else if (inst.opcode == Opcode::kOri && inst.rs == Register::kZero && inst.rt.has_value()) {
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
        if ((def_reg >= Register::kS0 && def_reg <= Register::kS7) || def_reg == Register::kFp) {
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
        auto seq = StructuredRegion::CreateSequence();
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
        auto seq = StructuredRegion::CreateSequence();
        seq->children.push_back(std::move(folded));
        FlattenSequence(seq.get(), std::move(after_join));
        return seq;
      }
    }
    return folded;
  }

  if_else->children.push_back(then_body ? std::move(then_body)
                                        : StructuredRegion::CreateSequence());
  if_else->children.push_back(else_body ? std::move(else_body)
                                        : StructuredRegion::CreateSequence());

  if (join.has_value() && join != follow) {
    auto after_join = StructureRegion(*join, follow, current_loop);
    if (after_join) {
      auto seq = StructuredRegion::CreateSequence();
      seq->children.push_back(std::move(if_else));
      FlattenSequence(seq.get(), std::move(after_join));
      return seq;
    }
  }
  return if_else;
}

std::unique_ptr<StructuredRegion> ControlFlowStructurer::StructureRegion(
    uint32_t entry, std::optional<uint32_t> follow, const Loop* current_loop) {
  if (follow.has_value() && entry == *follow) {
    return nullptr;
  }

  // Handle loop back-edges and exits
  if (current_loop != nullptr) {
    if (visited_.contains(entry) && entry == current_loop->header) {
      return StructuredRegion::CreateContinue(entry);
    }
    if (current_loop->IsExitBlock(entry)) {
      auto lf_it = loop_follow_map_.find(current_loop);
      std::optional<uint32_t> loop_follow_target =
          (lf_it != loop_follow_map_.end()) ? std::optional<uint32_t>(lf_it->second) : follow;
      auto is_empty_loop_follow_jump_to = [&](uint32_t target_id) {
        if (!loop_follow_target.has_value() || current_loop->parent == nullptr ||
            target_id != current_loop->parent->header) {
          return false;
        }
        const auto* lf_blk = cfg_->GetBlock(*loop_follow_target);
        if (lf_blk == nullptr || lf_blk->instructions.empty() || lf_blk->successors.size() != 1 ||
            lf_blk->successors[0] != target_id) {
          return false;
        }
        for (const auto& inst : lf_blk->instructions) {
          if (!inst.IsNop() && !inst.IsJump() && !inst.IsUnconditionalBranch()) {
            return false;
          }
        }
        return true;
      };
      if ((follow.has_value() && entry == *follow) ||
          (lf_it != loop_follow_map_.end() && entry == lf_it->second) ||
          is_empty_loop_follow_jump_to(entry)) {
        return StructuredRegion::CreateBreak(entry);
      }
      if (current_loop->parent != nullptr && visited_.contains(entry) &&
          entry == current_loop->parent->header) {
        return StructuredRegion::CreateGoto(entry);
      }
      const auto* entry_blk = cfg_->GetBlock(entry);
      if (entry_blk == nullptr || !IsPureEpilogueReturnBlock(*entry_blk)) {
        for (const Loop* anc = current_loop->parent; anc != nullptr; anc = anc->parent) {
          auto anc_it = loop_follow_map_.find(anc);
          if (anc_it != loop_follow_map_.end() && anc_it->second == entry) {
            return StructuredRegion::CreateGoto(entry);
          }
        }
        if (loop_follow_target.has_value()) {
          const auto* lf_blk = cfg_->GetBlock(*loop_follow_target);
          if (lf_blk != nullptr && lf_blk->successors.size() == 1 &&
              lf_blk->successors[0] == entry) {
            return StructuredRegion::CreateGoto(entry);
          }
        }
      }
      auto exit_body = StructureRegion(entry, loop_follow_target, nullptr);
      if (exit_body) {
        if (RegionEndsWithReturn(*exit_body, *cfg_)) {
          return exit_body;
        }
        auto seq = StructuredRegion::CreateSequence();
        seq->children.push_back(std::move(exit_body));
        seq->children.push_back(StructuredRegion::CreateBreak(entry));
        return seq;
      }
      return StructuredRegion::CreateBreak(entry);
    }
    if (current_loop->type == LoopType::kDoWhile && current_loop->IsLatch(entry)) {
      visited_.insert(entry);
      return StructuredRegion::CreateBlock(entry);
    }
  }

  if (current_switch_break_.has_value() && entry == *current_switch_break_) {
    return StructuredRegion::CreateBreak(entry);
  }

  if (visited_.contains(entry)) {
    if (current_switch_break_.has_value() && entry == *current_switch_break_) {
      return StructuredRegion::CreateBreak(entry);
    }
    return StructuredRegion::CreateGoto(entry);
  }

  // Strategy 0: LoopStructurer (before marking `entry` visited so loop header can be structured)
  if (!strategies_.empty()) {
    if (auto loop_region = strategies_[0]->TryStructure(this, entry, follow, current_loop);
        loop_region != nullptr) {
      return loop_region;
    }
  }

  const auto* block = cfg_->GetBlock(entry);
  if (block == nullptr) {
    return nullptr;
  }
  if (!IsPureEpilogueReturnBlock(*block)) {
    visited_.insert(entry);
  }

  // Strategy 1: Jump-table switch structuring
  if (auto jt_switch = StructureJumpTableSwitch(entry, follow, current_loop);
      jt_switch != nullptr) {
    return jt_switch;
  }

  // Case 1: Return or trap (0 successors)
  if (block->successors.empty()) {
    return StructuredRegion::CreateBlock(entry);
  }

  // Case 2: Linear sequence (1 successor)
  if (block->successors.size() == 1) {
    uint32_t succ = block->successors[0];
    auto block_region = StructuredRegion::CreateBlock(entry);

    if (follow.has_value() && succ == *follow) {
      return block_region;
    }
    if (current_switch_break_.has_value() && succ == *current_switch_break_) {
      auto seq = StructuredRegion::CreateSequence();
      seq->children.push_back(std::move(block_region));
      seq->children.push_back(StructuredRegion::CreateBreak(succ));
      return seq;
    }
    if (current_loop != nullptr) {
      if (succ == current_loop->header) {
        return block_region;
      }
      if (current_loop->IsExitBlock(succ)) {
        auto seq = StructuredRegion::CreateSequence();
        seq->children.push_back(std::move(block_region));
        seq->children.push_back(StructuredRegion::CreateBreak(succ));
        return seq;
      }
    }
    if (current_switch_break_.has_value()) {
      const auto* term = block->Terminator();
      if (term != nullptr && term->opcode == Opcode::kJ) {
        const auto* succ_blk = cfg_->GetBlock(succ);
        if (succ_blk != nullptr && !IsPureEpilogueReturnBlock(*succ_blk) &&
            succ_blk->predecessors.size() > 1) {
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
            auto seq = StructuredRegion::CreateSequence();
            seq->children.push_back(std::move(block_region));
            seq->children.push_back(StructuredRegion::CreateGoto(succ));
            return seq;
          }
        }
      }
    }

    auto next_region = StructureRegion(succ, follow, current_loop);
    if (next_region == nullptr) {
      return block_region;
    }

    auto seq = StructuredRegion::CreateSequence();
    seq->children.push_back(std::move(block_region));
    FlattenSequence(seq.get(), std::move(next_region));
    return seq;
  }

  // Case 3: Conditional branch (2 successors) -> dispatch through registered strategies
  // (`SwitchStructurer`, `ShortCircuitStructurer`, `ConditionalDiamondStructurer`).
  for (size_t i = 1; i < strategies_.size(); ++i) {
    if (auto structured = strategies_[i]->TryStructure(this, entry, follow, current_loop);
        structured != nullptr) {
      return structured;
    }
  }

  // Fallback for multi-way branches
  return StructuredRegion::CreateBlock(entry);
}

}  // namespace rom_nom_nom
