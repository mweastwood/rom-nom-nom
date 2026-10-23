#include "lifter/control_flow_pattern_matcher.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "lifter/branch_condition_builder.h"

namespace rom_nom_nom {

namespace {

bool HasNopBranchDelaySlot(const ControlFlowGraph& cfg, uint32_t block_id) {
  const auto* b = cfg.GetBlock(block_id);
  if (b == nullptr || b->instructions.size() < 2) return true;
  const auto* term = b->Terminator();
  if (term != nullptr && term->IsBranchLikely() && !b->instructions.back().IsNop()) {
    return false;
  }
  return !BranchConditionBuilder::ShouldDecoupleBranchConditionDelaySlot(cfg, block_id);
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

std::optional<uint32_t> FindSwitchJoinBlock(
    const ControlFlowGraph& cfg, const DominatorTree* post_dom_tree,
    const absl::flat_hash_set<uint32_t>& visited,
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

      const auto* blk = cfg.GetBlock(curr);
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
        if (decision_blocks.contains(succ) || visited.contains(succ)) {
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
    if (decision_blocks.contains(b) || visited.contains(b)) {
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
      const auto* current_block = cfg.GetBlock(current_block_id);
      if (current_block == nullptr) continue;
      for (uint32_t successor_block_id : current_block->successors) {
        if (decision_blocks.contains(successor_block_id) || visited.contains(successor_block_id)) {
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
          if (post_dom_tree != nullptr) {
            for (uint32_t target_block_id : targets) {
              if (!returning_targets.contains(target_block_id) &&
                  !post_dom_tree->Dominates(candidate_one, target_block_id)) {
                candidate_one_post_dominates_all_targets = false;
                break;
              }
            }
          }
          if (follow.has_value() && (current_loop == nullptr || current_loop->Contains(*follow)) &&
              successor_block_id == *follow && successor_reach_count == non_returning_count &&
              reaches[candidate_one].size() < targets.size()) {
            eliminated.insert(candidate_one);
          } else if (candidate_one_reach_count >= successor_reach_count &&
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
      const auto* blk = cfg.GetBlock(c);
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

// NOLINTNEXTLINE(google-readability-function-size)
std::optional<DecisionTreeSwitchInfo> DetectDecisionTreeSwitch(
    const ControlFlowGraph& cfg, const LoopInfo* loop_info,
    const absl::flat_hash_set<uint32_t>& visited, uint32_t root_id) {
  const auto* root_blk = cfg.GetBlock(root_id);
  if (root_blk == nullptr || root_blk->successors.size() != 2 || root_blk->instructions.empty()) {
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
    const auto* blk = cfg.GetBlock(b_id);
    if (blk == nullptr || blk->successors.size() != 2) {
      return ba;
    }

    int branch_idx = -1;
    for (int i = static_cast<int>(blk->instructions.size()) - 1; i >= 0; --i) {
      if (blk->instructions[i].IsBranch()) {
        branch_idx = i;
        break;
      }
    }
    if (branch_idx == -1) return ba;

    int scan_start = 0;
    if (b_id != root_id) {
      if (blk->instructions.size() > 6) {
        return ba;
      }
      for (const auto& inst : blk->instructions) {
        if (inst.IsCall() || inst.IsStore() || inst.IsLoad()) return ba;
      }
    } else {
      for (size_t i = static_cast<size_t>(branch_idx); i < blk->instructions.size(); ++i) {
        const auto& inst = blk->instructions[i];
        bool is_prologue_sp_save =
            (inst.opcode == Opcode::kSw && inst.rs == Register::kSp && inst.rt.has_value() &&
             (*inst.rt == Register::kRa || *inst.rt == Register::kFp ||
              (*inst.rt >= Register::kS0 && *inst.rt <= Register::kS7)));
        if (inst.IsCall() || (inst.IsStore() && !is_prologue_sp_save) || inst.IsLoad()) return ba;
      }
      scan_start = std::max(0, branch_idx - 4);
      for (int i = branch_idx - 1; i >= scan_start; --i) {
        const auto& inst = blk->instructions[i];
        if (inst.IsCall()) {
          scan_start = i + 1;
          break;
        }
      }
    }

    absl::flat_hash_map<Register, int64_t> consts = in_consts;
    absl::flat_hash_map<Register, int64_t> splits = in_splits;
    absl::flat_hash_map<Register, Register> split_sels = in_split_sels;
    consts[Register::kZero] = 0;

    absl::flat_hash_set<Register> bool_flags;
    for (int i = scan_start; i < branch_idx; ++i) {
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
      } else {
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register def_reg : ud.gpr_defs) {
          consts.erase(def_reg);
          splits.erase(def_reg);
          split_sels.erase(def_reg);
          bool_flags.erase(def_reg);
        }
      }
    }

    if (known_selector != Register::kZero) {
      for (int i = scan_start; i <= branch_idx; ++i) {
        const auto& inst = blk->instructions[i];
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
      RegisterUseDef ds_ud = GetInstructionUseDef(ds);
      bool ds_defines_callee_saved_in_root = false;
      if (b_id == root_id) {
        for (Register def_reg : ds_ud.gpr_defs) {
          if ((def_reg >= Register::kS0 && def_reg <= Register::kS7) || def_reg == Register::kFp) {
            ds_defines_callee_saved_in_root = true;
            break;
          }
        }
      }
      if (!ds.IsNop() && !ds_defines_callee_saved_in_root) {
        ba.delay_instruction = ds;
      }
      if (!ds_defines_callee_saved_in_root && ds.opcode == Opcode::kAddiu &&
          ds.rs == Register::kZero && ds.rt.has_value()) {
        ba.delay_slot_sets_const = true;
        ba.delay_slot_const_reg = *ds.rt;
        ba.delay_slot_const_val = static_cast<int16_t>(ds.immediate);
      } else if (!ds_defines_callee_saved_in_root && ds.opcode == Opcode::kAddu &&
                 ds.rs == Register::kZero && ds.rt == Register::kZero && ds.rd.has_value()) {
        ba.delay_slot_sets_const = true;
        ba.delay_slot_const_reg = *ds.rd;
        ba.delay_slot_const_val = 0;
      } else if (!ds_defines_callee_saved_in_root && ds.opcode == Opcode::kOri &&
                 ds.rs == Register::kZero && ds.rt.has_value()) {
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

    if (branch.opcode == Opcode::kBeq || branch.opcode == Opcode::kBne ||
        branch.opcode == Opcode::kBeql || branch.opcode == Opcode::kBnel) {
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

      if (branch.opcode == Opcode::kBeql || branch.opcode == Opcode::kBnel) {
        return ba;
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
      const auto* blk = cfg.GetBlock(curr);
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

    if (loop_info != nullptr) {
      if (loop_info->GetLoopFor(b_id) != loop_info->GetLoopFor(root_id)) {
        return std::nullopt;
      }
      const Loop* b_loop = loop_info->GetLoopFor(b_id);
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

      if (loop_info != nullptr) {
        const Loop* root_loop = loop_info->GetLoopFor(root_id);
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

      if (cont_ba.valid && !visited.contains(cont) && !decision_blocks.contains(cont)) {
        if (loop_info != nullptr) {
          const Loop* cont_loop = loop_info->GetLoopFor(cont);
          if (cont_loop != loop_info->GetLoopFor(root_id)) {
            cont_ba.valid = false;
          } else if (cont_loop != nullptr &&
                     (cont_loop->header == cont || cont_loop->IsLatch(cont))) {
            cont_ba.valid = false;
          }
        }
      }

      bool continuation_is_decision_block =
          (cont_ba.valid && !visited.contains(cont) && !decision_blocks.contains(cont));

      DecisionCase dc;
      dc.case_values = {ba.case_val};
      dc.target_block = ct;
      if (!continuation_is_decision_block && ba.delay_slot_sets_const) {
        const auto* target_block = cfg.GetBlock(ct);
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
        const auto* cont_blk = cfg.GetBlock(cont);
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
      if (loop_info != nullptr) {
        const Loop* root_loop = loop_info->GetLoopFor(root_id);
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

      if (ba_taken.valid && !visited.contains(ba.split_taken) &&
          !decision_blocks.contains(ba.split_taken)) {
        if (loop_info != nullptr) {
          const Loop* taken_loop = loop_info->GetLoopFor(ba.split_taken);
          if (taken_loop != loop_info->GetLoopFor(root_id)) {
            ba_taken.valid = false;
          } else if (taken_loop != nullptr && (taken_loop->header == ba.split_taken ||
                                               taken_loop->IsLatch(ba.split_taken))) {
            ba_taken.valid = false;
          }
        }
      }
      if (ba_taken.valid && !visited.contains(ba.split_taken) &&
          !decision_blocks.contains(ba.split_taken)) {
        decision_blocks.insert(ba.split_taken);
        block_consts[ba.split_taken] = succ_c;
        block_splits[ba.split_taken] = succ_s;
        block_split_sels[ba.split_taken] = succ_sels;
        queue.push_back(ba.split_taken);
      } else if (!ba_taken.valid && ba_ft.valid && root_ba.is_equality &&
                 root_ba.delay_slot_sets_split &&
                 root_ba.delay_slot_split_val == root_ba.case_val + 1 &&
                 !seen_case_vals.contains(root_ba.case_val - 1)) {
        uint32_t eff_taken = resolve_trampoline(ba.split_taken);
        if (!default_target.has_value()) {
          default_target = eff_taken;
        } else if (*default_target != eff_taken) {
          return std::nullopt;
        }
        int64_t implied_case_val = root_ba.case_val - 1;
        seen_case_vals.insert(implied_case_val);
        if (!target_to_cases.contains(eff_taken)) {
          target_order.push_back(eff_taken);
        }
        target_to_cases[eff_taken].push_back(implied_case_val);
        DecisionCase dc;
        dc.case_values = {implied_case_val};
        dc.target_block = eff_taken;
        if (ba.delay_slot_sets_const) {
          dc.const_reg = ba.delay_slot_const_reg;
          dc.const_val = ba.delay_slot_const_val;
          if (!default_const_reg.has_value()) {
            default_const_reg = ba.delay_slot_const_reg;
            default_const_val = ba.delay_slot_const_val;
          }
        }
        if (ba.delay_instruction.has_value()) {
          dc.delay_instructions.push_back(*ba.delay_instruction);
          if (default_delay_instructions.empty()) {
            default_delay_instructions.push_back(*ba.delay_instruction);
          }
        }
        decision_cases.push_back(std::move(dc));
      } else {
        return std::nullopt;
      }

      if (ba_ft.valid && !visited.contains(ba.split_ft) && !decision_blocks.contains(ba.split_ft)) {
        if (loop_info != nullptr) {
          const Loop* ft_loop = loop_info->GetLoopFor(ba.split_ft);
          if (ft_loop != loop_info->GetLoopFor(root_id)) {
            ba_ft.valid = false;
          } else if (ft_loop != nullptr &&
                     (ft_loop->header == ba.split_ft || ft_loop->IsLatch(ba.split_ft))) {
            ba_ft.valid = false;
          }
        }
      }
      if (ba_ft.valid && !visited.contains(ba.split_ft) && !decision_blocks.contains(ba.split_ft)) {
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

  bool root_has_extra_instructions = (root_blk->instructions.size() > 6);
  for (const auto& inst : root_blk->instructions) {
    if (inst.IsCall() || inst.IsStore() || inst.IsLoad()) {
      root_has_extra_instructions = true;
      break;
    }
  }
  const size_t min_cases = root_has_extra_instructions ? 3 : 2;
  if (total_cases < min_cases || total_cases > 5 || target_to_cases.size() < min_cases) {
    return std::nullopt;
  }

  for (uint32_t db : decision_blocks) {
    if (db == root_id) continue;
    const auto* blk = cfg.GetBlock(db);
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
  std::sort(target_order.begin(), target_order.end());

  std::sort(
      decision_cases.begin(), decision_cases.end(),
      [](const DecisionCase& a, const DecisionCase& b) { return a.target_block < b.target_block; });

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

std::optional<FloatToUintInfo> DetectFloatToUintDiamond(const ControlFlowGraph& cfg, uint32_t entry,
                                                        uint32_t succ_taken,
                                                        uint32_t succ_fallthrough,
                                                        std::optional<uint32_t> join) {
  const auto* b_entry = cfg.GetBlock(entry);
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

  const auto* b_taken = cfg.GetBlock(succ_taken);
  const auto* b_ft = cfg.GetBlock(succ_fallthrough);
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

std::optional<ShortCircuitChainInfo> TryCollapseShortCircuitChain(
    const ControlFlowGraph& cfg, const absl::flat_hash_set<uint32_t>& visited, uint32_t entry,
    uint32_t succ_taken, uint32_t succ_fallthrough, std::optional<uint32_t> join) {
  if (visited.contains(succ_fallthrough) || !HasNopBranchDelaySlot(cfg, entry)) {
    return std::nullopt;
  }

  const auto* b_fallthrough = cfg.GetBlock(succ_fallthrough);
  if (b_fallthrough == nullptr || b_fallthrough->successors.size() != 2 ||
      b_fallthrough->predecessors.size() != 1 || b_fallthrough->predecessors[0] != entry ||
      !IsPureConditionBlock(cfg, succ_fallthrough)) {
    return std::nullopt;
  }

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

  if (b_taken != succ_taken && b_ft != succ_taken) {
    return std::nullopt;
  }

  std::vector<uint32_t> chain_blocks;
  std::vector<bool> or_inverts;
  std::vector<bool> and_inverts;
  uint32_t curr_id = succ_fallthrough;
  uint32_t curr_taken = b_taken;
  uint32_t curr_ft = b_ft;
  std::optional<uint32_t> final_after;
  bool last_matches_taken = false;

  while (true) {
    bool matches_taken = (curr_taken == succ_taken);
    bool matches_ft = (curr_ft == succ_taken);
    if (!matches_taken && !matches_ft) {
      break;
    }

    last_matches_taken = matches_taken;
    uint32_t other = matches_taken ? curr_ft : curr_taken;
    chain_blocks.push_back(curr_id);
    or_inverts.push_back(matches_ft);
    and_inverts.push_back(matches_taken);

    const auto* next_b = cfg.GetBlock(other);
    if (HasNopBranchDelaySlot(cfg, curr_id) && next_b != nullptr &&
        next_b->successors.size() == 2 && next_b->predecessors.size() == 1 &&
        !visited.contains(other) && IsPureConditionBlock(cfg, other) &&
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

  if (!chain_blocks.empty()) {
    bool is_and_chain = false;
    if (final_after.has_value()) {
      if (join.has_value() && succ_taken == *join && *final_after != *join) {
        is_and_chain = true;
      } else if (join.has_value() && *final_after == *join && succ_taken != *join) {
        is_and_chain = false;
      } else {
        is_and_chain = last_matches_taken;
      }
    }

    ShortCircuitChainInfo info;
    if (is_and_chain) {
      info.condition_op = "&&";
      info.root_invert = true;
      info.additional_condition_blocks = std::move(chain_blocks);
      info.additional_invert_conditions = std::move(and_inverts);
      info.then_target_block = *final_after;
      info.else_target_block = succ_taken;
    } else {
      info.condition_op = "||";
      info.root_invert = false;
      info.additional_condition_blocks = std::move(chain_blocks);
      info.additional_invert_conditions = std::move(or_inverts);
      info.then_target_block = succ_taken;
      info.else_target_block = final_after;
    }
    return info;
  }

  return std::nullopt;
}

}  // namespace rom_nom_nom
