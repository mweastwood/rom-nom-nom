#include "lifter/abi_analyzer.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

std::string AbiAnalyzer::RegisterVarName(Register reg) {
  switch (reg) {
    case Register::kZero:
      return "0";
    case Register::kV0:
      return "v0";
    case Register::kV1:
      return "v1";
    case Register::kA0:
      return "arg0";
    case Register::kA1:
      return "arg1";
    case Register::kA2:
      return "arg2";
    case Register::kA3:
      return "arg3";
    case Register::kSp:
      return "sp";
    case Register::kFp:
      return "fp";
    case Register::kRa:
      return "ra";
    default: {
      std::string_view name = RegisterName(reg);
      if (!name.empty() && name[0] == '$') {
        return std::string("temp_") + std::string(name.substr(1));
      }
      return std::string(name);
    }
  }
}

std::string AbiAnalyzer::FpRegisterVarName(FpRegister reg, bool is_double) {
  std::string_view name = FpRegisterName(reg);
  std::string base;
  if (!name.empty() && name[0] == '$') {
    base = std::string(name.substr(1));
  } else {
    base = std::string(name);
  }
  if (is_double) {
    return absl::StrCat(base, "_d");
  }
  return base;
}

bool AbiAnalyzer::IsRegisterLiveIn(const ControlFlowGraph& cfg, Register reg) {
  if (cfg.Blocks().empty()) return false;

  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  std::vector<bool> uses_before_def(cfg.Blocks().size(), false);
  std::vector<bool> defines(cfg.Blocks().size(), false);

  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    const auto& block = cfg.Blocks()[i];
    for (size_t j = 0; j < block.instructions.size(); ++j) {
      const auto& inst = block.instructions[j];
      bool is_call = (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr);

      if (is_call && j + 1 < block.instructions.size()) {
        RegisterUseDef delay_ud = GetInstructionUseDef(block.instructions[j + 1]);
        for (Register u : delay_ud.gpr_uses) {
          if (u == reg && !defines[i]) {
            uses_before_def[i] = true;
          }
        }
        for (Register d : delay_ud.gpr_defs) {
          if (d == reg) {
            defines[i] = true;
          }
        }
      }

      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register u : ud.gpr_uses) {
        if (u == reg && !defines[i]) {
          uses_before_def[i] = true;
        }
      }
      for (Register d : ud.gpr_defs) {
        if (d == reg) {
          defines[i] = true;
        }
      }
      if (is_call) {
        defines[i] = true;
        if (j + 1 < block.instructions.size()) {
          ++j;
        }
      }
    }
  }

  std::vector<size_t> queue = {0};
  absl::flat_hash_set<size_t> visited = {0};

  while (!queue.empty()) {
    size_t curr = queue.back();
    queue.pop_back();

    if (uses_before_def[curr]) {
      return true;
    }
    if (defines[curr]) {
      continue;
    }

    const auto& block = cfg.Blocks()[curr];
    for (uint32_t succ_id : block.successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end() && visited.insert(it->second).second) {
        queue.push_back(it->second);
      }
    }
  }

  return false;
}

bool AbiAnalyzer::IsFpRegisterLiveIn(const ControlFlowGraph& cfg, FpRegister reg) {
  if (cfg.Blocks().empty()) return false;

  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  std::vector<bool> uses_before_def(cfg.Blocks().size(), false);
  std::vector<bool> defines(cfg.Blocks().size(), false);

  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    const auto& block = cfg.Blocks()[i];
    for (size_t j = 0; j < block.instructions.size(); ++j) {
      const auto& inst = block.instructions[j];
      bool is_call = (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr);

      if (is_call && j + 1 < block.instructions.size()) {
        RegisterUseDef delay_ud = GetInstructionUseDef(block.instructions[j + 1]);
        for (FpRegister u : delay_ud.fpr_uses) {
          if (u == reg && !defines[i]) {
            uses_before_def[i] = true;
          }
        }
        for (FpRegister d : delay_ud.fpr_defs) {
          if (d == reg) {
            defines[i] = true;
          }
        }
      }

      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (FpRegister u : ud.fpr_uses) {
        if (u == reg && !defines[i]) {
          uses_before_def[i] = true;
        }
      }
      for (FpRegister d : ud.fpr_defs) {
        if (d == reg) {
          defines[i] = true;
        }
      }
      if (is_call) {
        defines[i] = true;
        if (j + 1 < block.instructions.size()) {
          ++j;
        }
      }
    }
  }

  std::vector<size_t> queue = {0};
  absl::flat_hash_set<size_t> visited = {0};

  while (!queue.empty()) {
    size_t curr = queue.back();
    queue.pop_back();

    if (uses_before_def[curr]) {
      return true;
    }
    if (defines[curr]) {
      continue;
    }

    const auto& block = cfg.Blocks()[curr];
    for (uint32_t succ_id : block.successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end() && visited.insert(it->second).second) {
        queue.push_back(it->second);
      }
    }
  }

  return false;
}

int AbiAnalyzer::DetermineParameterCount(absl::Span<const Instruction> instructions) {
  absl::flat_hash_set<Register> defined_registers;
  absl::flat_hash_set<Register> used_before_definition;

  int frame_size = 0;
  for (const auto& inst : instructions) {
    if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) && inst.rt.has_value() &&
        *inst.rt == Register::kSp && inst.rs == Register::kSp && inst.immediate < 0) {
      frame_size = std::max(frame_size, static_cast<int>(-inst.immediate));
    }
  }

  int max_stack_arg = -1;
  absl::flat_hash_set<int32_t> stored_stack_offsets;

  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& instruction = instructions[i];
    bool is_call = (instruction.opcode == Opcode::kJal || instruction.opcode == Opcode::kJalr);

    if (instruction.rs == Register::kSp || instruction.rs == Register::kFp) {
      if (instruction.opcode == Opcode::kSw || instruction.opcode == Opcode::kSh ||
          instruction.opcode == Opcode::kSb || instruction.opcode == Opcode::kSwc1 ||
          instruction.opcode == Opcode::kSdc1) {
        stored_stack_offsets.insert(instruction.immediate);
      } else if (instruction.opcode == Opcode::kLw || instruction.opcode == Opcode::kLh ||
                 instruction.opcode == Opcode::kLhu || instruction.opcode == Opcode::kLb ||
                 instruction.opcode == Opcode::kLbu || instruction.opcode == Opcode::kLwc1 ||
                 instruction.opcode == Opcode::kLdc1) {
        int32_t offset = instruction.immediate;
        uint32_t base_offset = frame_size + 16;
        if (offset >= static_cast<int32_t>(base_offset)) {
          int arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
          if (!stored_stack_offsets.contains(offset)) {
            max_stack_arg = std::max(max_stack_arg, arg_idx);
          }
        }
      }
    }

    if (is_call && i + 1 < instructions.size()) {
      const auto& delay_inst = instructions[i + 1];
      RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
      for (Register used_register : delay_ud.gpr_uses) {
        if (!defined_registers.contains(used_register)) {
          used_before_definition.insert(used_register);
        }
      }
      for (Register defined_register : delay_ud.gpr_defs) {
        defined_registers.insert(defined_register);
      }
    }

    RegisterUseDef use_def = GetInstructionUseDef(instruction);
    for (Register used_register : use_def.gpr_uses) {
      if (!defined_registers.contains(used_register)) {
        used_before_definition.insert(used_register);
      }
    }
    for (Register defined_register : use_def.gpr_defs) {
      defined_registers.insert(defined_register);
    }
    if (is_call) {
      // Subroutine calls may clobber argument registers; from caller perspective they are redefined
      defined_registers.insert(Register::kA0);
      defined_registers.insert(Register::kA1);
      defined_registers.insert(Register::kA2);
      defined_registers.insert(Register::kA3);
      if (i + 1 < instructions.size()) {
        ++i;  // Delay slot was already processed
      }
    }
  }

  int count = 0;
  auto cfg = ControlFlowGraph::Build(instructions);
  auto check_reg = [&](Register reg) {
    if (cfg.ok() && IsRegisterLiveIn(*cfg, reg)) return true;
    return used_before_definition.contains(reg);
  };

  if (check_reg(Register::kA3)) {
    count = 4;
  } else if (check_reg(Register::kA2)) {
    count = 3;
  } else if (check_reg(Register::kA1)) {
    count = 2;
  } else if (check_reg(Register::kA0)) {
    count = 1;
  }

  if (max_stack_arg >= 4) {
    count = std::max(count, max_stack_arg + 1);
  }
  return count;
}

int AbiAnalyzer::DetermineFpParameterCount(absl::Span<const Instruction> instructions) {
  auto cfg = ControlFlowGraph::Build(instructions);
  if (cfg.ok()) {
    if (IsFpRegisterLiveIn(*cfg, FpRegister::kF12)) {
      if (IsFpRegisterLiveIn(*cfg, FpRegister::kF14)) {
        return 2;
      }
      return 1;
    }
    return 0;
  }

  absl::flat_hash_set<FpRegister> defined_fprs;
  absl::flat_hash_set<FpRegister> used_fprs_before_def;

  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& instruction = instructions[i];
    bool is_call = (instruction.opcode == Opcode::kJal || instruction.opcode == Opcode::kJalr);

    if (is_call && i + 1 < instructions.size()) {
      const auto& delay_inst = instructions[i + 1];
      RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
      for (FpRegister used_reg : delay_ud.fpr_uses) {
        if (!defined_fprs.contains(used_reg)) {
          used_fprs_before_def.insert(used_reg);
        }
      }
      for (FpRegister def_reg : delay_ud.fpr_defs) {
        defined_fprs.insert(def_reg);
      }
    }

    RegisterUseDef use_def = GetInstructionUseDef(instruction);
    for (FpRegister used_reg : use_def.fpr_uses) {
      if (!defined_fprs.contains(used_reg)) {
        used_fprs_before_def.insert(used_reg);
      }
    }
    for (FpRegister def_reg : use_def.fpr_defs) {
      defined_fprs.insert(def_reg);
    }
    if (is_call) {
      defined_fprs.insert(FpRegister::kF12);
      defined_fprs.insert(FpRegister::kF14);
      if (i + 1 < instructions.size()) {
        ++i;  // Delay slot was already processed
      }
    }
  }

  if (used_fprs_before_def.contains(FpRegister::kF12)) {
    if (used_fprs_before_def.contains(FpRegister::kF14)) {
      return 2;
    }
    return 1;
  }
  return 0;
}

bool AbiAnalyzer::DetermineReturnsV0(
    const ControlFlowGraph& cfg, const SymbolIndex* symbol_index,
    const absl::flat_hash_set<std::string>* functions_returning_v0) {
  return ::rom_nom_nom::DetermineReturnsV0(cfg, symbol_index, functions_returning_v0);
}

bool AbiAnalyzer::DetermineReturnsF0(const ControlFlowGraph& cfg) {
  return ::rom_nom_nom::DetermineReturnsF0(cfg);
}

bool AbiAnalyzer::DetermineReturnsF0Double(const ControlFlowGraph& cfg) {
  return ::rom_nom_nom::DetermineReturnsF0Double(cfg);
}

absl::flat_hash_set<Register> AbiAnalyzer::DetectUninitializedCalleeSavedRegisters(
    const ControlFlowGraph& cfg) {
  return ::rom_nom_nom::DetectUninitializedCalleeSavedRegisters(cfg);
}

absl::flat_hash_set<Register> AbiAnalyzer::DetectFloatParametersInGeneralRegisters(
    const ControlFlowGraph& cfg) {
  absl::flat_hash_set<Register> result;
  if (cfg.Blocks().empty()) {
    return result;
  }

  std::vector<Register> candidates;
  if (!IsFpRegisterLiveIn(cfg, FpRegister::kF12)) {
    candidates = {Register::kA1, Register::kA2, Register::kA3};
  } else {
    candidates = {Register::kA2, Register::kA3};
  }

  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  auto is_fpr_converted_from_int = [&](uint32_t start_block_id, size_t start_inst_idx,
                                       FpRegister fpr) -> bool {
    struct WorkItem {
      uint32_t block_id;
      size_t inst_idx;
    };
    std::vector<WorkItem> queue = {{start_block_id, start_inst_idx + 1}};
    absl::flat_hash_set<uint32_t> visited_blocks;

    while (!queue.empty()) {
      WorkItem item = queue.back();
      queue.pop_back();

      auto it = id_to_index.find(item.block_id);
      if (it == id_to_index.end()) continue;
      const auto& block = cfg.Blocks()[it->second];

      bool fpr_dead = false;
      for (size_t i = item.inst_idx; i < block.instructions.size(); ++i) {
        const auto& inst = block.instructions[i];

        if ((inst.opcode == Opcode::kCvtSW || inst.opcode == Opcode::kCvtDW) &&
            inst.fs.has_value() && *inst.fs == fpr) {
          return true;
        }

        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (FpRegister d : ud.fpr_defs) {
          if (d == fpr) {
            fpr_dead = true;
            break;
          }
        }
        if (fpr_dead) break;

        if (inst.IsCall() && static_cast<int>(fpr) < 20) {
          fpr_dead = true;
          break;
        }
      }

      if (!fpr_dead) {
        for (uint32_t succ_id : block.successors) {
          if (visited_blocks.insert(succ_id).second) {
            queue.push_back({succ_id, 0});
          }
        }
      }
    }
    return false;
  };

  for (Register reg : candidates) {
    if (!IsRegisterLiveIn(cfg, reg)) {
      continue;
    }

    bool has_mtc1 = false;
    bool has_integer_use = false;
    bool is_converted_from_int = false;
    bool has_def = false;

    for (const auto& block : cfg.Blocks()) {
      for (size_t j = 0; j < block.instructions.size(); ++j) {
        const auto& inst = block.instructions[j];

        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register u : ud.gpr_uses) {
          if (u == reg) {
            if (inst.opcode == Opcode::kMtc1 && inst.rt.has_value() && *inst.rt == reg &&
                inst.fs.has_value()) {
              has_mtc1 = true;
              if (is_fpr_converted_from_int(block.id, j, *inst.fs)) {
                is_converted_from_int = true;
              }
            } else {
              has_integer_use = true;
            }
          }
        }

        for (Register d : ud.gpr_defs) {
          if (d == reg) {
            has_def = true;
          }
        }
      }
    }

    if (has_mtc1 && !has_integer_use && !is_converted_from_int && !has_def) {
      result.insert(reg);
    }
  }

  return result;
}

absl::flat_hash_map<uint32_t, CallReturnInfo> AbiAnalyzer::ComputeCallReturnInfo(
    const ControlFlowGraph& cfg, bool /*function_returns_v0*/, bool /*function_returns_f0*/) {
  absl::flat_hash_map<uint32_t, CallReturnInfo> result;

  auto is_double_op = [](Opcode op) {
    return (op == Opcode::kAddD || op == Opcode::kSubD || op == Opcode::kMulD ||
            op == Opcode::kDivD || op == Opcode::kSqrtD || op == Opcode::kAbsD ||
            op == Opcode::kMovD || op == Opcode::kNegD || op == Opcode::kCvtSD ||
            op == Opcode::kTruncWD || op == Opcode::kRoundWD || op == Opcode::kCeilWD ||
            op == Opcode::kFloorWD || op == Opcode::kCEqD || op == Opcode::kCLtD ||
            op == Opcode::kCLeD || op == Opcode::kSdc1 || op == Opcode::kDmfc1);
  };

  for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
    const auto& block = cfg.Blocks()[b];
    for (size_t i = 0; i < block.instructions.size(); ++i) {
      const auto& call_inst = block.instructions[i];
      if (call_inst.opcode != Opcode::kJal && call_inst.opcode != Opcode::kJalr) {
        continue;
      }

      CallReturnInfo info;

      auto check_fp_arg_double = [&](FpRegister reg) -> bool {
        // Check delay slot instruction of this call if within current block
        if (i + 1 < block.instructions.size()) {
          const auto& inst = block.instructions[i + 1];
          if (inst.opcode == Opcode::kLdc1 && inst.ft == reg) return true;
          if (inst.opcode == Opcode::kDmtc1 && inst.fs == reg) return true;
          if ((inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
               inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
               inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
               inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
               inst.opcode == Opcode::kCvtDW || inst.opcode == Opcode::kCvtDS) &&
              inst.fd == reg) {
            return true;
          }
          RegisterUseDef ud = GetInstructionUseDef(inst);
          for (FpRegister d : ud.fpr_defs) {
            if (d == reg) return false;
          }
        }
        // Check instructions in the current block before the call
        for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
          const auto& inst = block.instructions[j];
          if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr) {
            break;
          }
          if (inst.opcode == Opcode::kLdc1 && inst.ft == reg) return true;
          if (inst.opcode == Opcode::kDmtc1 && inst.fs == reg) return true;
          if ((inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
               inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
               inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
               inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
               inst.opcode == Opcode::kCvtDW || inst.opcode == Opcode::kCvtDS) &&
              inst.fd == reg) {
            return true;
          }
          RegisterUseDef ud = GetInstructionUseDef(inst);
          for (FpRegister d : ud.fpr_defs) {
            if (d == reg) return false;
          }
        }
        // If not found in current block, search predecessor blocks in CFG
        absl::flat_hash_set<uint32_t> visited;
        visited.insert(block.id);
        std::vector<uint32_t> pred_queue = block.predecessors;
        while (!pred_queue.empty()) {
          uint32_t pid = pred_queue.back();
          pred_queue.pop_back();
          if (visited.contains(pid)) continue;
          visited.insert(pid);
          const auto* pblock = cfg.GetBlock(pid);
          if (pblock == nullptr) continue;
          bool def_found = false;
          for (int j = static_cast<int>(pblock->instructions.size()) - 1; j >= 0; --j) {
            const auto& inst = pblock->instructions[j];
            if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr) {
              break;
            }
            if (inst.opcode == Opcode::kLdc1 && inst.ft == reg) return true;
            if (inst.opcode == Opcode::kDmtc1 && inst.fs == reg) return true;
            if ((inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
                 inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD ||
                 inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
                 inst.opcode == Opcode::kMovD || inst.opcode == Opcode::kNegD ||
                 inst.opcode == Opcode::kCvtDW || inst.opcode == Opcode::kCvtDS) &&
                inst.fd == reg) {
              return true;
            }
            RegisterUseDef ud = GetInstructionUseDef(inst);
            for (FpRegister d : ud.fpr_defs) {
              if (d == reg) {
                def_found = true;
                break;
              }
            }
            if (def_found) break;
          }
          if (!def_found) {
            for (uint32_t next_pid : pblock->predecessors) {
              pred_queue.push_back(next_pid);
            }
          }
        }
        return false;
      };

      info.arg_f12_double = check_fp_arg_double(FpRegister::kF12);
      info.arg_f14_double = check_fp_arg_double(FpRegister::kF14);

      struct WorkItem {
        uint32_t block_id;
        size_t start_idx;
        bool v0_live;
        bool v1_live;
        bool f0_live;
        int depth;
      };

      std::vector<WorkItem> queue;
      if (i + 2 < block.instructions.size()) {
        queue.push_back({block.id, i + 2, true, true, true, 32});
      } else {
        for (uint32_t succ_id : block.successors) {
          queue.push_back({succ_id, 0, true, true, true, 32});
        }
        if (block.successors.empty()) {
          const auto* next_block = cfg.FindBlockByVram(call_inst.vram + 8);
          if (next_block != nullptr) {
            queue.push_back({next_block->id, 0, true, true, true, 32});
          }
        }
      }

      absl::flat_hash_set<uint32_t> visited_blocks;

      while (!queue.empty()) {
        auto item = queue.back();
        queue.pop_back();

        if (!item.v0_live && !item.v1_live && !item.f0_live) {
          continue;
        }

        const auto* curr_block = cfg.GetBlock(item.block_id);
        if (curr_block == nullptr) continue;

        if (item.start_idx == 0) {
          if (!visited_blocks.insert(item.block_id).second) {
            continue;
          }
        }

        bool stop_path = false;
        for (size_t k = item.start_idx; k < curr_block->instructions.size(); ++k) {
          if (item.depth <= 0) {
            stop_path = true;
            break;
          }
          --item.depth;

          const auto& inst = curr_block->instructions[k];

          if (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr) {
            RegisterUseDef call_ud = GetInstructionUseDef(inst);
            if (item.v0_live) {
              for (Register u : call_ud.gpr_uses) {
                if (u == Register::kV0) {
                  info.returns_v0 = true;
                  item.v0_live = false;
                  break;
                }
              }
            }
            if (item.v1_live) {
              for (Register u : call_ud.gpr_uses) {
                if (u == Register::kV1) {
                  info.returns_s64 = true;
                  item.v1_live = false;
                  break;
                }
              }
            }
            if (item.f0_live) {
              for (FpRegister u : call_ud.fpr_uses) {
                if (u == FpRegister::kF0) {
                  info.returns_f0 = true;
                  if (is_double_op(inst.opcode)) info.returns_f0_double = true;
                  item.f0_live = false;
                  break;
                }
              }
            }
            if (k + 1 < curr_block->instructions.size()) {
              const auto& delay_inst = curr_block->instructions[k + 1];
              RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
              if (item.v0_live) {
                for (Register u : delay_ud.gpr_uses) {
                  if (u == Register::kV0) {
                    info.returns_v0 = true;
                    item.v0_live = false;
                    break;
                  }
                }
              }
              if (item.v1_live) {
                for (Register u : delay_ud.gpr_uses) {
                  if (u == Register::kV1) {
                    info.returns_s64 = true;
                    item.v1_live = false;
                    break;
                  }
                }
              }
              if (item.f0_live) {
                for (FpRegister u : delay_ud.fpr_uses) {
                  if (u == FpRegister::kF0) {
                    info.returns_f0 = true;
                    if (is_double_op(delay_inst.opcode)) info.returns_f0_double = true;
                    item.f0_live = false;
                    break;
                  }
                }
              }
            }
            stop_path = true;
            break;
          }

          RegisterUseDef ud = GetInstructionUseDef(inst);

          // Check uses first
          if (item.v0_live) {
            for (Register u : ud.gpr_uses) {
              if (u == Register::kV0) {
                info.returns_v0 = true;
                item.v0_live = false;
                break;
              }
            }
          }
          if (item.v1_live) {
            for (Register u : ud.gpr_uses) {
              if (u == Register::kV1) {
                info.returns_s64 = true;
                item.v1_live = false;
                break;
              }
            }
          }
          if (item.f0_live) {
            for (FpRegister u : ud.fpr_uses) {
              if (u == FpRegister::kF0) {
                info.returns_f0 = true;
                if (is_double_op(inst.opcode)) {
                  info.returns_f0_double = true;
                }
                item.f0_live = false;
                break;
              }
            }
          }

          // Check defs
          if (item.v0_live) {
            for (Register d : ud.gpr_defs) {
              if (d == Register::kV0) {
                item.v0_live = false;
                break;
              }
            }
          }
          if (item.v1_live) {
            for (Register d : ud.gpr_defs) {
              if (d == Register::kV1) {
                item.v1_live = false;
                break;
              }
            }
          }
          if (item.f0_live) {
            for (FpRegister d : ud.fpr_defs) {
              if (d == FpRegister::kF0) {
                item.f0_live = false;
                break;
              }
            }
          }

          if (inst.IsReturn()) {
            stop_path = true;
            break;
          }

          if (!item.v0_live && !item.v1_live && !item.f0_live) {
            stop_path = true;
            break;
          }
        }

        if (!stop_path && (item.v0_live || item.v1_live || item.f0_live)) {
          for (uint32_t succ_id : curr_block->successors) {
            queue.push_back({succ_id, 0, item.v0_live, item.v1_live, item.f0_live, item.depth});
          }
        }
      }

      if (info.returns_s64) {
        info.returns_v0 = true;
      }
      if (info.returns_f0_double) {
        info.returns_f0 = true;
      }

      result[call_inst.vram] = info;
    }
  }

  return result;
}

absl::flat_hash_set<std::string> AbiAnalyzer::IdentifyDoubleVariables(const ControlFlowGraph& cfg) {
  absl::flat_hash_set<std::string> double_vars;
  for (const auto& block : cfg.Blocks()) {
    for (const auto& inst : block.instructions) {
      switch (inst.opcode) {
        case Opcode::kAddD:
        case Opcode::kSubD:
        case Opcode::kMulD:
        case Opcode::kDivD:
        case Opcode::kSqrtD:
        case Opcode::kAbsD:
        case Opcode::kMovD:
        case Opcode::kNegD:
        case Opcode::kCEqD:
        case Opcode::kCLtD:
        case Opcode::kCLeD:
          if (inst.fd) double_vars.insert(FpRegisterVarName(*inst.fd, true));
          if (inst.fs) double_vars.insert(FpRegisterVarName(*inst.fs, true));
          if (inst.ft) double_vars.insert(FpRegisterVarName(*inst.ft, true));
          break;
        case Opcode::kLdc1:
        case Opcode::kSdc1:
          if (inst.ft) double_vars.insert(FpRegisterVarName(*inst.ft, true));
          if (inst.rs.has_value() && (*inst.rs == Register::kSp || *inst.rs == Register::kFp)) {
            if (inst.immediate < 0) {
              double_vars.insert(absl::StrFormat("var_sp_neg_%d", -inst.immediate));
            } else {
              double_vars.insert(absl::StrFormat("var_sp_%d", inst.immediate));
            }
          }
          break;
        case Opcode::kDmfc1:
        case Opcode::kDmtc1:
          if (inst.fs) double_vars.insert(FpRegisterVarName(*inst.fs, true));
          break;
        case Opcode::kCvtDS:
        case Opcode::kCvtDW:
          if (inst.fd) double_vars.insert(FpRegisterVarName(*inst.fd, true));
          break;
        case Opcode::kCvtSD:
        case Opcode::kTruncWD:
          if (inst.fs) double_vars.insert(FpRegisterVarName(*inst.fs, true));
          break;
        default:
          break;
      }
    }
  }
  return double_vars;
}

bool AbiAnalyzer::IsFpParameterDouble(const ControlFlowGraph& cfg, FpRegister reg) {
  if (cfg.Blocks().empty()) return false;
  std::vector<size_t> queue = {0};
  absl::flat_hash_set<size_t> visited = {0};

  while (!queue.empty()) {
    size_t curr = queue.front();
    queue.erase(queue.begin());
    const auto& block = cfg.Blocks()[curr];
    for (const auto& inst : block.instructions) {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (FpRegister u : ud.fpr_uses) {
        if (u == reg) {
          Opcode op = inst.opcode;
          return (op == Opcode::kAddD || op == Opcode::kSubD || op == Opcode::kMulD ||
                  op == Opcode::kDivD || op == Opcode::kSqrtD || op == Opcode::kAbsD ||
                  op == Opcode::kMovD || op == Opcode::kNegD || op == Opcode::kCvtSD ||
                  op == Opcode::kTruncWD || op == Opcode::kRoundWD || op == Opcode::kCeilWD ||
                  op == Opcode::kFloorWD || op == Opcode::kCEqD || op == Opcode::kCLtD ||
                  op == Opcode::kCLeD || op == Opcode::kSdc1);
        }
      }
      for (FpRegister d : ud.fpr_defs) {
        if (d == reg) {
          goto next_block;
        }
      }
    }
    for (uint32_t succ_id : block.successors) {
      auto it = std::find_if(cfg.Blocks().begin(), cfg.Blocks().end(),
                             [succ_id](const BasicBlock& b) { return b.id == succ_id; });
      if (it != cfg.Blocks().end()) {
        size_t succ_idx = std::distance(cfg.Blocks().begin(), it);
        if (visited.insert(succ_idx).second) {
          queue.push_back(succ_idx);
        }
      }
    }
  next_block:;
  }
  return false;
}

}  // namespace rom_nom_nom
