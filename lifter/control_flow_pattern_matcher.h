#ifndef LIFTER_CONTROL_FLOW_PATTERN_MATCHER_H_
#define LIFTER_CONTROL_FLOW_PATTERN_MATCHER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

struct DecisionCase {
  std::vector<int64_t> case_values;
  bool is_default = false;
  uint32_t target_block = 0;
  std::optional<Register> const_reg;
  std::optional<int64_t> const_val;
  std::vector<Instruction> delay_instructions;
};

struct DecisionTreeSwitchInfo {
  Register selector_reg = Register::kZero;
  absl::flat_hash_set<uint32_t> decision_blocks;
  std::vector<uint32_t> unique_target_blocks;
  absl::flat_hash_map<uint32_t, std::vector<int64_t>> target_to_cases;
  std::optional<uint32_t> default_target_block;
  std::vector<DecisionCase> cases;
};

struct ShortCircuitChainInfo {
  std::string condition_op;
  bool root_invert = false;
  std::vector<uint32_t> additional_condition_blocks;
  std::vector<bool> additional_invert_conditions;
  uint32_t then_target_block = 0;
  std::optional<uint32_t> else_target_block;
};

// Finds the common convergence block reached by non-returning switch cases.
std::optional<uint32_t> FindSwitchJoinBlock(
    const ControlFlowGraph& cfg, const DominatorTree* post_dom_tree,
    const absl::flat_hash_set<uint32_t>& visited,
    const absl::flat_hash_set<uint32_t>& decision_blocks,
    const absl::flat_hash_map<uint32_t, std::vector<int64_t>>& target_to_cases,
    std::optional<uint32_t> default_block_id, std::optional<uint32_t> follow,
    const Loop* current_loop = nullptr);

// Detects a small decision tree switch (<= 5 cases) starting at root_id.
std::optional<DecisionTreeSwitchInfo> DetectDecisionTreeSwitch(
    const ControlFlowGraph& cfg, const LoopInfo* loop_info,
    const absl::flat_hash_set<uint32_t>& visited, uint32_t root_id);

// Detects a 0x4F000000 float-to-uint conversion diamond starting at entry.
std::optional<FloatToUintInfo> DetectFloatToUintDiamond(const ControlFlowGraph& cfg, uint32_t entry,
                                                        uint32_t succ_taken,
                                                        uint32_t succ_fallthrough,
                                                        std::optional<uint32_t> join);

// Detects a short-circuit || or && condition chain starting at entry.
std::optional<ShortCircuitChainInfo> TryCollapseShortCircuitChain(
    const ControlFlowGraph& cfg, const absl::flat_hash_set<uint32_t>& visited, uint32_t entry,
    uint32_t succ_taken, uint32_t succ_fallthrough, std::optional<uint32_t> join = std::nullopt);

}  // namespace rom_nom_nom

#endif  // LIFTER_CONTROL_FLOW_PATTERN_MATCHER_H_
