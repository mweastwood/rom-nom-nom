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
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {
namespace {

absl::flat_hash_map<uint32_t, size_t> BuildBlockIdToIndex(const ControlFlowGraph& cfg) {
  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  id_to_index.reserve(cfg.Blocks().size());
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }
  return id_to_index;
}

uint32_t ComputeMaxFrameSize(const ControlFlowGraph& cfg) {
  return StackFrameAnalyzer::ComputeFrameSize(cfg);
}

bool IsLiveInOnCfg(const ControlFlowGraph& cfg,
                   const absl::flat_hash_map<uint32_t, size_t>& id_to_index,
                   const std::vector<bool>& uses_before_def, const std::vector<bool>& defines) {
  if (cfg.Blocks().empty()) return false;
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

    for (uint32_t succ_id : cfg.Blocks()[curr].successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end() && visited.insert(it->second).second) {
        queue.push_back(it->second);
      }
    }
  }
  return false;
}

bool IsFprConvertedFromInt(const ControlFlowGraph& cfg,
                           const absl::flat_hash_map<uint32_t, size_t>& id_to_index,
                           uint32_t start_block_id, size_t start_inst_idx, FpRegister fpr) {
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

      if ((inst.opcode == Opcode::kCvtSW || inst.opcode == Opcode::kCvtDW) && inst.fs.has_value() &&
          *inst.fs == fpr) {
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
}

bool IsDoublePrecisionUseOp(Opcode op) {
  return AbiAnalyzer::IsDoublePrecisionUseOp(op);
}

bool DefinesFprAsDouble(const Instruction& inst, FpRegister reg) {
  if (inst.opcode == Opcode::kLdc1 && inst.ft == reg) return true;
  if (inst.opcode == Opcode::kDmtc1 && inst.fs == reg) return true;
  if ((static_cast<int>(reg) % 2 == 0) && static_cast<int>(reg) + 1 < 32) {
    FpRegister odd_reg = static_cast<FpRegister>(static_cast<int>(reg) + 1);
    if (inst.opcode == Opcode::kMtc1 && inst.fs == odd_reg) return true;
    if (inst.opcode == Opcode::kLwc1 && inst.ft == odd_reg) return true;
  }
  if (IsDoublePrecisionWriteOp(inst.opcode) && inst.fd == reg) {
    return true;
  }
  return false;
}

void RecordReturnRegisterUses(const Instruction& inst, const RegisterUseDef& ud, bool* v0_live,
                              bool* v1_live, bool* f0_live, CallReturnInfo* info) {
  if (*v0_live) {
    for (Register u : ud.gpr_uses) {
      if (u == Register::kV0) {
        info->returns_v0 = true;
        *v0_live = false;
        break;
      }
    }
  }
  if (v1_live != nullptr && *v1_live) {
    for (Register u : ud.gpr_uses) {
      if (u == Register::kV1) {
        info->returns_s64 = true;
        *v1_live = false;
        break;
      }
    }
  }
  if (*f0_live) {
    for (FpRegister u : ud.fpr_uses) {
      if (u == FpRegister::kF0) {
        info->returns_f0 = true;
        if (IsDoublePrecisionUseOp(inst.opcode)) {
          info->returns_f0_double = true;
        }
        *f0_live = false;
        break;
      }
    }
  }
}

void RecordReturnRegisterDefs(const RegisterUseDef& ud, bool* v0_live, bool* v1_live,
                              bool* f0_live) {
  if (*v0_live) {
    for (Register d : ud.gpr_defs) {
      if (d == Register::kV0) {
        *v0_live = false;
        break;
      }
    }
  }
  if (v1_live != nullptr && *v1_live) {
    for (Register d : ud.gpr_defs) {
      if (d == Register::kV1) {
        *v1_live = false;
        break;
      }
    }
  }
  if (*f0_live) {
    for (FpRegister d : ud.fpr_defs) {
      if (d == FpRegister::kF0) {
        *f0_live = false;
        break;
      }
    }
  }
}

}  // namespace

bool AbiAnalyzer::IsDoublePrecisionUseOp(Opcode op) {
  return IsDoublePrecisionReadOp(op);
}

CallReturnInfo AbiAnalyzer::ScanCallReturnUsage(absl::Span<const Instruction> instructions,
                                                size_t start_idx, bool stop_at_branches) {
  CallReturnInfo info;
  bool v0_live = true;
  bool v1_live = true;
  bool f0_live = true;

  for (size_t next_i = start_idx; next_i < instructions.size(); ++next_i) {
    const auto& next_inst = instructions[next_i];
    if (!stop_at_branches &&
        (next_inst.opcode == Opcode::kJal || next_inst.opcode == Opcode::kJalr)) {
      break;
    }

    RegisterUseDef next_ud = GetInstructionUseDef(next_inst);
    RecordReturnRegisterUses(next_inst, next_ud, &v0_live, &v1_live, &f0_live, &info);
    RecordReturnRegisterDefs(next_ud, &v0_live, &v1_live, &f0_live);

    if (stop_at_branches &&
        (next_inst.opcode == Opcode::kJal || next_inst.opcode == Opcode::kJalr ||
         next_inst.opcode == Opcode::kJ || next_inst.opcode == Opcode::kJr ||
         next_inst.IsUnconditionalBranch())) {
      if (next_i + 1 < instructions.size()) {
        const auto& ds_inst = instructions[next_i + 1];
        RegisterUseDef ds_ud = GetInstructionUseDef(ds_inst);
        RecordReturnRegisterUses(ds_inst, ds_ud, &v0_live, &v1_live, &f0_live, &info);
      }
      break;
    }

    if (!v0_live && !v1_live && !f0_live) {
      break;
    }
  }
  return info;
}

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

  const auto id_to_index = BuildBlockIdToIndex(cfg);
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

  return IsLiveInOnCfg(cfg, id_to_index, uses_before_def, defines);
}

bool AbiAnalyzer::IsFpRegisterLiveIn(const ControlFlowGraph& cfg, FpRegister reg) {
  if (cfg.Blocks().empty()) return false;

  const auto id_to_index = BuildBlockIdToIndex(cfg);
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

  return IsLiveInOnCfg(cfg, id_to_index, uses_before_def, defines);
}

int AbiAnalyzer::DetermineParameterCount(absl::Span<const Instruction> instructions) {
  absl::flat_hash_set<Register> defined_registers;
  absl::flat_hash_set<Register> used_before_definition;

  int frame_size = 0;
  for (const auto& inst : instructions) {
    if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
         inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
        inst.rt.has_value() && *inst.rt == Register::kSp && inst.rs == Register::kSp &&
        inst.immediate < 0) {
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
          instruction.opcode == Opcode::kSdc1 || instruction.opcode == Opcode::kSd) {
        stored_stack_offsets.insert(instruction.immediate);
        if (instruction.opcode == Opcode::kSdc1 || instruction.opcode == Opcode::kSd) {
          stored_stack_offsets.insert(instruction.immediate + 4);
        }
      } else if (instruction.opcode == Opcode::kLw || instruction.opcode == Opcode::kLh ||
                 instruction.opcode == Opcode::kLhu || instruction.opcode == Opcode::kLb ||
                 instruction.opcode == Opcode::kLbu || instruction.opcode == Opcode::kLwc1 ||
                 instruction.opcode == Opcode::kLdc1 || instruction.opcode == Opcode::kLd) {
        int32_t offset = instruction.immediate;
        uint32_t base_offset = frame_size + 16;
        bool valid_alignment = true;
        if (instruction.opcode == Opcode::kLw || instruction.opcode == Opcode::kLwu ||
            instruction.opcode == Opcode::kLwc1 || instruction.opcode == Opcode::kLd ||
            instruction.opcode == Opcode::kLdc1) {
          valid_alignment = (offset % 4 == 0);
        } else if (instruction.opcode == Opcode::kLh || instruction.opcode == Opcode::kLhu) {
          valid_alignment = (offset % 2 == 0);
        }
        if (offset >= static_cast<int32_t>(base_offset) && valid_alignment &&
            offset < static_cast<int32_t>(base_offset + 64)) {
          int arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
          if (!stored_stack_offsets.contains(offset)) {
            int last_arg_idx =
                (instruction.opcode == Opcode::kLdc1 || instruction.opcode == Opcode::kLd)
                    ? arg_idx + 1
                    : arg_idx;
            max_stack_arg = std::max(max_stack_arg, last_arg_idx);
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

  bool has_fp_register_params = cfg.ok() && (IsFpRegisterLiveIn(*cfg, FpRegister::kF12) ||
                                             IsFpRegisterLiveIn(*cfg, FpRegister::kF14));
  if (max_stack_arg >= 4 && (count > 0 || has_fp_register_params)) {
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

namespace {

int FirstGprFloatCandidateSlot(const ControlFlowGraph& cfg) {
  if (!AbiAnalyzer::IsFpRegisterLiveIn(cfg, FpRegister::kF12)) {
    return 1;
  }
  int next_slot = AbiAnalyzer::IsFpParameterDouble(cfg, FpRegister::kF12) ? 2 : 1;
  if (AbiAnalyzer::IsFpRegisterLiveIn(cfg, FpRegister::kF14)) {
    if (AbiAnalyzer::IsFpParameterDouble(cfg, FpRegister::kF14)) {
      return 4;
    }
    return next_slot + 1;
  }
  // If $f14 is not live-in, the parameter immediately following $f12 must be an integer
  // parameter in a GPR, so any subsequent float/double parameter in GPRs starts after it.
  return next_slot + 1;
}

}  // namespace

bool AbiAnalyzer::DetectDoubleParameterInGeneralRegisters(const ControlFlowGraph& cfg) {
  if (cfg.Blocks().empty()) {
    return false;
  }
  if (FirstGprFloatCandidateSlot(cfg) > 2) {
    return false;
  }
  if (!IsRegisterLiveIn(cfg, Register::kA2) || !IsRegisterLiveIn(cfg, Register::kA3)) {
    return false;
  }

  const auto id_to_index = BuildBlockIdToIndex(cfg);
  auto is_fpr_converted_from_int = [&](uint32_t start_block_id, size_t start_inst_idx,
                                       FpRegister fpr) -> bool {
    return IsFprConvertedFromInt(cfg, id_to_index, start_block_id, start_inst_idx, fpr);
  };

  const uint32_t frame_size = ComputeMaxFrameSize(cfg);
  const BasicBlock* entry_block = cfg.GetBlock(cfg.EntryBlockId());

  auto find_entry_first_def = [&](Register reg) -> std::optional<size_t> {
    if (entry_block == nullptr || !entry_block->predecessors.empty()) {
      return std::nullopt;
    }
    for (size_t j = 0; j < entry_block->instructions.size(); ++j) {
      if (j > 0 && entry_block->instructions[j - 1].IsBranchLikely()) {
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(entry_block->instructions[j]);
      for (Register d : ud.gpr_defs) {
        if (d == reg) {
          return j;
        }
      }
    }
    return std::nullopt;
  };

  const std::optional<size_t> a2_first_def = find_entry_first_def(Register::kA2);
  const std::optional<size_t> a3_first_def = find_entry_first_def(Register::kA3);

  absl::flat_hash_set<int> a2_double_fprs;
  absl::flat_hash_set<int> a3_double_fprs;
  bool a2_spilled_to_home = false;
  bool a3_spilled_to_home = false;
  int a2_integer_uses = 0;
  int a3_integer_uses = 0;
  int home_ldc1_reads = 0;
  int home_other_accesses = 0;

  for (const auto& block : cfg.Blocks()) {
    bool a2_defined = false;
    bool a3_defined = false;
    for (size_t j = 0; j < block.instructions.size(); ++j) {
      const auto& inst = block.instructions[j];
      RegisterUseDef ud = GetInstructionUseDef(inst);

      const bool reads_incoming_a2 = a2_first_def.has_value()
                                         ? (block.id == cfg.EntryBlockId() && j < *a2_first_def)
                                         : !a2_defined;
      const bool reads_incoming_a3 = a3_first_def.has_value()
                                         ? (block.id == cfg.EntryBlockId() && j < *a3_first_def)
                                         : !a3_defined;

      for (Register u : ud.gpr_uses) {
        if (u == Register::kA2 && reads_incoming_a2) {
          if (inst.opcode == Opcode::kMtc1 && inst.rt == Register::kA2 && inst.fs.has_value() &&
              (static_cast<int>(*inst.fs) % 2 == 1)) {
            a2_double_fprs.insert(static_cast<int>(*inst.fs) - 1);
          } else if (block.id == cfg.EntryBlockId() && inst.opcode == Opcode::kSw &&
                     inst.rt == Register::kA2 && inst.rs == Register::kSp &&
                     inst.immediate == static_cast<int32_t>(frame_size + 8)) {
            a2_spilled_to_home = true;
          } else {
            a2_integer_uses++;
          }
        }
        if (u == Register::kA3 && reads_incoming_a3) {
          if (inst.opcode == Opcode::kMtc1 && inst.rt == Register::kA3 && inst.fs.has_value() &&
              (static_cast<int>(*inst.fs) % 2 == 0)) {
            if (is_fpr_converted_from_int(block.id, j, *inst.fs)) {
              a3_integer_uses++;
            } else {
              a3_double_fprs.insert(static_cast<int>(*inst.fs));
            }
          } else if (block.id == cfg.EntryBlockId() && inst.opcode == Opcode::kSw &&
                     inst.rt == Register::kA3 && inst.rs == Register::kSp &&
                     inst.immediate == static_cast<int32_t>(frame_size + 12)) {
            a3_spilled_to_home = true;
          } else {
            a3_integer_uses++;
          }
        }
      }

      if (inst.rs == Register::kSp && (inst.immediate == static_cast<int32_t>(frame_size + 8) ||
                                       inst.immediate == static_cast<int32_t>(frame_size + 12))) {
        bool is_prologue_home_spill =
            (block.id == cfg.EntryBlockId() && inst.opcode == Opcode::kSw &&
             ((inst.rt == Register::kA2 && reads_incoming_a2 &&
               inst.immediate == static_cast<int32_t>(frame_size + 8)) ||
              (inst.rt == Register::kA3 && reads_incoming_a3 &&
               inst.immediate == static_cast<int32_t>(frame_size + 12))));
        if (!is_prologue_home_spill) {
          if (inst.opcode == Opcode::kLdc1 &&
              inst.immediate == static_cast<int32_t>(frame_size + 8)) {
            home_ldc1_reads++;
          } else if (inst.opcode != Opcode::kSdc1 ||
                     inst.immediate != static_cast<int32_t>(frame_size + 8)) {
            if (inst.IsLoad() || inst.IsStore()) {
              home_other_accesses++;
            }
          }
        }
      }

      for (Register d : ud.gpr_defs) {
        if (d == Register::kA2) a2_defined = true;
        if (d == Register::kA3) a3_defined = true;
      }
    }
  }

  if (a2_integer_uses > 0 || a3_integer_uses > 0) {
    return false;
  }

  for (int even_fpr : a2_double_fprs) {
    if (a3_double_fprs.contains(even_fpr)) {
      return true;
    }
  }

  return a2_spilled_to_home && a3_spilled_to_home && home_ldc1_reads > 0 &&
         home_other_accesses == 0;
}

absl::flat_hash_set<Register> AbiAnalyzer::DetectFloatParametersInGeneralRegisters(
    const ControlFlowGraph& cfg) {
  absl::flat_hash_set<Register> result;
  if (cfg.Blocks().empty()) {
    return result;
  }

  const bool double_in_a2_a3 = DetectDoubleParameterInGeneralRegisters(cfg);
  std::vector<Register> candidates;
  const int first_slot = FirstGprFloatCandidateSlot(cfg);
  for (int slot = first_slot; slot < 4; ++slot) {
    Register reg = static_cast<Register>(static_cast<int>(Register::kA0) + slot);
    if (double_in_a2_a3 && (reg == Register::kA2 || reg == Register::kA3)) {
      continue;
    }
    candidates.push_back(reg);
  }

  const auto id_to_index = BuildBlockIdToIndex(cfg);
  auto is_fpr_converted_from_int = [&](uint32_t start_block_id, size_t start_inst_idx,
                                       FpRegister fpr) -> bool {
    return IsFprConvertedFromInt(cfg, id_to_index, start_block_id, start_inst_idx, fpr);
  };

  absl::flat_hash_set<FpRegister> fixed_point_fprs;
  for (const auto& block : cfg.Blocks()) {
    for (const auto& inst : block.instructions) {
      if (IsFloatToIntConvertOp(inst.opcode) && inst.fd.has_value()) {
        fixed_point_fprs.insert(*inst.fd);
      }
    }
  }

  const uint32_t frame_size = ComputeMaxFrameSize(cfg);

  auto is_callee_saved_sp_access = [frame_size](const Instruction& inst, Register r) -> bool {
    if (inst.rs != Register::kSp || !inst.rt.has_value() || *inst.rt != r) return false;
    if (inst.opcode != Opcode::kSw && inst.opcode != Opcode::kSd && inst.opcode != Opcode::kLw &&
        inst.opcode != Opcode::kLd) {
      return false;
    }
    if ((inst.opcode == Opcode::kLw || inst.opcode == Opcode::kLd) && frame_size > 0 &&
        inst.immediate >= static_cast<int32_t>(frame_size)) {
      return false;
    }
    return r == Register::kFp || (r >= Register::kS0 && r <= Register::kS7);
  };

  const BasicBlock* entry_block = cfg.GetBlock(cfg.EntryBlockId());
  auto is_entry_defined_before = [&](size_t end_idx, Register r) -> bool {
    if (entry_block == nullptr) return false;
    for (size_t k = 0; k < end_idx && k < entry_block->instructions.size(); ++k) {
      RegisterUseDef kud = GetInstructionUseDef(entry_block->instructions[k]);
      for (Register d : kud.gpr_defs) {
        if (d == r) return true;
      }
    }
    return false;
  };

  // 1. Detect local GPRs (e.g. $s0-$s7, $fp, $v0-$v1, $t0-$t9) used purely as f32 carriers.
  const std::vector<Register> carrier_candidates = {
      Register::kS0, Register::kS1, Register::kS2, Register::kS3, Register::kS4, Register::kS5,
      Register::kS6, Register::kS7, Register::kFp, Register::kV0, Register::kV1, Register::kT0,
      Register::kT1, Register::kT2, Register::kT3, Register::kT4, Register::kT5, Register::kT6,
      Register::kT7, Register::kT8, Register::kT9,
  };
  auto is_live_in_ignoring_callee_save = [&](Register r) -> bool {
    std::vector<bool> uses_before_def(cfg.Blocks().size(), false);
    std::vector<bool> defines(cfg.Blocks().size(), false);
    for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
      const auto& block = cfg.Blocks()[i];
      for (size_t j = 0; j < block.instructions.size(); ++j) {
        const auto& inst = block.instructions[j];
        bool is_call = (inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr);
        if (is_call && j + 1 < block.instructions.size()) {
          const auto& delay_inst = block.instructions[j + 1];
          if (!is_callee_saved_sp_access(delay_inst, r)) {
            RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
            for (Register u : delay_ud.gpr_uses) {
              if (u == r && !defines[i]) uses_before_def[i] = true;
            }
            for (Register d : delay_ud.gpr_defs) {
              if (d == r) defines[i] = true;
            }
          }
        }
        if (!is_callee_saved_sp_access(inst, r)) {
          RegisterUseDef ud = GetInstructionUseDef(inst);
          for (Register u : ud.gpr_uses) {
            if (u == r && !defines[i]) uses_before_def[i] = true;
          }
          for (Register d : ud.gpr_defs) {
            if (d == r) defines[i] = true;
          }
        }
        if (is_call) {
          if (r != Register::kFp && (r < Register::kS0 || r > Register::kS7)) {
            defines[i] = true;
          }
          if (j + 1 < block.instructions.size()) {
            ++j;
          }
        }
      }
    }
    return IsLiveInOnCfg(cfg, id_to_index, uses_before_def, defines);
  };

  for (Register r : carrier_candidates) {
    if (is_live_in_ignoring_callee_save(r)) continue;
    int mfc1_defs = 0;
    int stack_arg_load_defs = 0;
    int entry_arg_copy_defs = 0;
    int float_const_defs = 0;
    int other_defs = 0;
    int mtc1_uses = 0;
    int sw_val_uses = 0;
    int other_uses = 0;
    for (const auto& block : cfg.Blocks()) {
      for (size_t j = 0; j < block.instructions.size(); ++j) {
        const auto& inst = block.instructions[j];
        if (is_callee_saved_sp_access(inst, r)) continue;
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register d : ud.gpr_defs) {
          if (d == r) {
            if (inst.opcode == Opcode::kMfc1 && inst.rt == r && inst.fs.has_value() &&
                (static_cast<int>(*inst.fs) % 2 == 0) && !fixed_point_fprs.contains(*inst.fs)) {
              mfc1_defs++;
            } else if (inst.opcode == Opcode::kLw && inst.rt == r && inst.rs == Register::kSp &&
                       frame_size > 0 && inst.immediate >= static_cast<int32_t>(frame_size + 16)) {
              stack_arg_load_defs++;
            } else if (block.id == cfg.EntryBlockId() &&
                       (inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kDaddu ||
                        inst.opcode == Opcode::kOr) &&
                       inst.rd == r &&
                       ((inst.rs.has_value() &&
                         std::find(candidates.begin(), candidates.end(), *inst.rs) !=
                             candidates.end() &&
                         inst.rt == Register::kZero && !is_entry_defined_before(j, *inst.rs)) ||
                        (inst.rt.has_value() &&
                         std::find(candidates.begin(), candidates.end(), *inst.rt) !=
                             candidates.end() &&
                         inst.rs == Register::kZero && !is_entry_defined_before(j, *inst.rt)))) {
              entry_arg_copy_defs++;
            } else if (inst.opcode == Opcode::kLui && inst.rt == r &&
                       ((static_cast<uint16_t>(inst.immediate) & 0x7F80u) != 0) &&
                       ((static_cast<uint16_t>(inst.immediate) & 0x7F80u) != 0x7F80u)) {
              float_const_defs++;
            } else {
              other_defs++;
            }
          }
        }
        for (Register u : ud.gpr_uses) {
          if (u == r) {
            if (inst.opcode == Opcode::kMtc1 && inst.rt == r && inst.fs.has_value() &&
                (static_cast<int>(*inst.fs) % 2 == 0) &&
                !is_fpr_converted_from_int(block.id, j, *inst.fs)) {
              mtc1_uses++;
            } else if (inst.opcode == Opcode::kSw && inst.rt == r && inst.rs != r) {
              sw_val_uses++;
            } else {
              other_uses++;
            }
          }
        }
      }
    }
    if ((mfc1_defs + stack_arg_load_defs + entry_arg_copy_defs) > 0 && other_defs == 0 &&
        mtc1_uses > 0 && other_uses == 0) {
      result.insert(r);
    }
  }

  const absl::flat_hash_set<int32_t> float_local_offsets = DetectFloatLocalStackOffsets(cfg);

  // 2. Detect incoming parameters in GPRs ($a1-$a3) that represent f32 arguments.
  for (Register reg : candidates) {
    if (!IsRegisterLiveIn(cfg, reg)) {
      continue;
    }

    std::optional<size_t> entry_first_def_idx;
    if (entry_block != nullptr && entry_block->predecessors.empty()) {
      for (size_t j = 0; j < entry_block->instructions.size(); ++j) {
        if (j > 0 && entry_block->instructions[j - 1].IsBranchLikely()) {
          continue;
        }
        RegisterUseDef ud = GetInstructionUseDef(entry_block->instructions[j]);
        for (Register d : ud.gpr_defs) {
          if (d == reg) {
            entry_first_def_idx = j;
            break;
          }
        }
        if (entry_first_def_idx.has_value()) break;
      }
    }

    int incoming_float_uses = 0;
    int incoming_integer_uses = 0;
    bool is_converted_from_int = false;
    bool has_def = false;
    bool has_non_mfc1_def = false;
    bool all_defs_are_call_args = true;
    bool has_post_def_mtc1 = false;

    for (const auto& block : cfg.Blocks()) {
      bool defined_in_block = false;
      for (size_t j = 0; j < block.instructions.size(); ++j) {
        const auto& inst = block.instructions[j];
        RegisterUseDef ud = GetInstructionUseDef(inst);

        bool reads_incoming_param = false;
        if (entry_first_def_idx.has_value()) {
          reads_incoming_param = (block.id == cfg.EntryBlockId() && j < *entry_first_def_idx);
        } else {
          reads_incoming_param = !defined_in_block;
        }

        for (Register u : ud.gpr_uses) {
          if (u == reg) {
            if (reads_incoming_param) {
              if (inst.opcode == Opcode::kMtc1 && inst.rt.has_value() && *inst.rt == reg &&
                  inst.fs.has_value() && (static_cast<int>(*inst.fs) % 2 == 0)) {
                incoming_float_uses++;
                if (is_fpr_converted_from_int(block.id, j, *inst.fs)) {
                  is_converted_from_int = true;
                }
              } else if (block.id == cfg.EntryBlockId() &&
                         (inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kDaddu ||
                          inst.opcode == Opcode::kOr) &&
                         inst.rd.has_value() && result.contains(*inst.rd) &&
                         ((inst.rs == reg && inst.rt == Register::kZero) ||
                          (inst.rt == reg && inst.rs == Register::kZero))) {
                incoming_float_uses++;
              } else if (block.id == cfg.EntryBlockId() && inst.opcode == Opcode::kSw &&
                         inst.rt == reg && inst.rs == Register::kSp &&
                         float_local_offsets.contains(inst.immediate)) {
                incoming_float_uses++;
              } else {
                incoming_integer_uses++;
              }
            } else {
              if (inst.opcode == Opcode::kMtc1 && inst.rt.has_value() && *inst.rt == reg) {
                has_post_def_mtc1 = true;
              }
              if (!entry_first_def_idx.has_value()) {
                incoming_integer_uses++;
              }
            }
          }
        }

        for (Register d : ud.gpr_defs) {
          if (d == reg) {
            has_def = true;
            defined_in_block = true;
            bool is_mfc1_float_arg =
                (inst.opcode == Opcode::kMfc1 && inst.fs.has_value() &&
                 (static_cast<int>(*inst.fs) % 2 == 0) && !fixed_point_fprs.contains(*inst.fs));
            if (!is_mfc1_float_arg) {
              has_non_mfc1_def = true;
            }
            bool is_call_arg_def = false;
            if (j > 0 && block.instructions[j - 1].IsCall()) {
              is_call_arg_def = true;
            } else {
              for (size_t k = j + 1; k < block.instructions.size(); ++k) {
                if (block.instructions[k].IsCall()) {
                  is_call_arg_def = true;
                  break;
                }
              }
            }
            if (!is_call_arg_def) {
              all_defs_are_call_args = false;
            }
          }
        }
      }
    }

    if (incoming_float_uses > 0 && incoming_integer_uses == 0 && !is_converted_from_int &&
        (!has_def || (incoming_float_uses == 1 && (!has_non_mfc1_def || all_defs_are_call_args)) ||
         (entry_first_def_idx.has_value() && !has_post_def_mtc1))) {
      result.insert(reg);
    }
  }

  return result;
}

absl::flat_hash_set<int32_t> AbiAnalyzer::DetectFloatLocalStackOffsets(
    const ControlFlowGraph& cfg) {
  absl::flat_hash_set<int32_t> result;
  if (cfg.Blocks().empty()) return result;

  const uint32_t frame_size = ComputeMaxFrameSize(cfg);
  if (frame_size == 0) return result;

  const auto id_to_index = BuildBlockIdToIndex(cfg);
  auto is_fpr_converted_from_int = [&](uint32_t start_block_id, size_t start_inst_idx,
                                       FpRegister fpr) -> bool {
    return IsFprConvertedFromInt(cfg, id_to_index, start_block_id, start_inst_idx, fpr);
  };

  absl::flat_hash_set<int32_t> saved_offsets;
  const BasicBlock* entry_block = cfg.GetBlock(cfg.EntryBlockId());
  if (entry_block != nullptr) {
    for (const auto& inst : entry_block->instructions) {
      if (inst.rs == Register::kSp && inst.immediate >= 0 &&
          inst.immediate < static_cast<int32_t>(frame_size)) {
        if ((inst.opcode == Opcode::kSw || inst.opcode == Opcode::kSd) && inst.rt.has_value()) {
          Register r = *inst.rt;
          if (r == Register::kRa || r == Register::kFp ||
              (r >= Register::kS0 && r <= Register::kS7)) {
            saved_offsets.insert(inst.immediate);
          }
        } else if ((inst.opcode == Opcode::kSwc1 || inst.opcode == Opcode::kSdc1) &&
                   inst.ft.has_value() && static_cast<int>(*inst.ft) >= 20) {
          saved_offsets.insert(inst.immediate);
        }
      }
    }
  }

  struct OffsetStats {
    int lwc1_reads = 0;
    int swc1_writes = 0;
    int prologue_gpr_spills = 0;
    int other_accesses = 0;
  };
  absl::flat_hash_map<int32_t, OffsetStats> stats;

  for (const auto& block : cfg.Blocks()) {
    absl::flat_hash_set<Register> defined_in_block;
    for (size_t j = 0; j < block.instructions.size(); ++j) {
      const auto& inst = block.instructions[j];
      if (inst.rs == Register::kSp && inst.immediate >= 0 &&
          inst.immediate < static_cast<int32_t>(frame_size + 16) && (inst.immediate % 4 == 0) &&
          !saved_offsets.contains(inst.immediate)) {
        int32_t off = inst.immediate;
        if (inst.opcode == Opcode::kLwc1 && inst.ft.has_value()) {
          if (is_fpr_converted_from_int(block.id, j, *inst.ft)) {
            stats[off].other_accesses++;
          } else {
            stats[off].lwc1_reads++;
          }
        } else if (inst.opcode == Opcode::kSwc1) {
          stats[off].swc1_writes++;
        } else if (block.id == cfg.EntryBlockId() && inst.opcode == Opcode::kSw &&
                   inst.rt.has_value() &&
                   (*inst.rt == Register::kA1 || *inst.rt == Register::kA2 ||
                    *inst.rt == Register::kA3) &&
                   !defined_in_block.contains(*inst.rt)) {
          stats[off].prologue_gpr_spills++;
        } else if (inst.IsLoad() || inst.IsStore() ||
                   ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
                     inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
                    inst.rt != Register::kSp)) {
          stats[off].other_accesses++;
        }
      }
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register d : ud.gpr_defs) {
        defined_in_block.insert(d);
      }
    }
  }

  for (const auto& [off, st] : stats) {
    if (st.lwc1_reads > 0 && st.other_accesses == 0 && st.prologue_gpr_spills <= 1 &&
        (st.swc1_writes > 0 || st.prologue_gpr_spills == 1)) {
      result.insert(off);
    }
  }
  return result;
}

absl::flat_hash_set<int> AbiAnalyzer::DetectDoubleStackParameterSlots(const ControlFlowGraph& cfg) {
  absl::flat_hash_set<int> result;
  if (cfg.Blocks().empty()) return result;

  const uint32_t frame_size = ComputeMaxFrameSize(cfg);
  const auto id_to_index = BuildBlockIdToIndex(cfg);
  auto is_fpr_converted_from_int = [&](uint32_t start_block_id, size_t start_inst_idx,
                                       FpRegister fpr) -> bool {
    return IsFprConvertedFromInt(cfg, id_to_index, start_block_id, start_inst_idx, fpr);
  };

  const int32_t base_offset = static_cast<int32_t>(frame_size + 16);
  absl::flat_hash_map<int, int> double_accesses;
  absl::flat_hash_map<int, absl::flat_hash_set<int>> split_hi_fprs;
  absl::flat_hash_map<int, absl::flat_hash_set<int>> split_lo_fprs;
  absl::flat_hash_map<int, int> non_double_accesses;
  absl::flat_hash_set<int> candidate_even_slots;

  for (const auto& block : cfg.Blocks()) {
    for (size_t j = 0; j < block.instructions.size(); ++j) {
      const auto& inst = block.instructions[j];
      if (inst.rs != Register::kSp || inst.immediate < base_offset) continue;
      int32_t rel = inst.immediate - base_offset;
      int slot = 4 + (rel / 4);
      if (rel % 4 != 0) {
        non_double_accesses[slot]++;
        continue;
      }
      if (inst.opcode == Opcode::kLdc1 || inst.opcode == Opcode::kSdc1) {
        if ((slot % 2) == 0) {
          double_accesses[slot]++;
          candidate_even_slots.insert(slot);
        } else {
          non_double_accesses[slot]++;
          non_double_accesses[slot + 1]++;
        }
      } else if (inst.opcode == Opcode::kLwc1 && inst.ft.has_value()) {
        int ft_idx = static_cast<int>(*inst.ft);
        if ((slot % 2) == 0 && (ft_idx % 2) == 1) {
          split_hi_fprs[slot].insert(ft_idx - 1);
          candidate_even_slots.insert(slot);
        } else if ((slot % 2) == 1 && (ft_idx % 2) == 0 &&
                   !is_fpr_converted_from_int(block.id, j, *inst.ft)) {
          split_lo_fprs[slot - 1].insert(ft_idx);
          candidate_even_slots.insert(slot - 1);
        } else {
          non_double_accesses[slot]++;
        }
      } else if (inst.IsLoad() || inst.IsStore() || inst.opcode == Opcode::kAddiu ||
                 inst.opcode == Opcode::kAddi || inst.opcode == Opcode::kDaddiu ||
                 inst.opcode == Opcode::kDaddi) {
        non_double_accesses[slot]++;
        if (inst.opcode == Opcode::kLd || inst.opcode == Opcode::kSd) {
          non_double_accesses[slot + 1]++;
        }
      }
    }
  }

  for (int slot : candidate_even_slots) {
    if (non_double_accesses[slot] > 0 || non_double_accesses[slot + 1] > 0) {
      continue;
    }
    bool has_split_pair = false;
    for (int even_fpr : split_hi_fprs[slot]) {
      if (split_lo_fprs[slot].contains(even_fpr)) {
        has_split_pair = true;
        break;
      }
    }
    if (!has_split_pair && (!split_hi_fprs[slot].empty() || !split_lo_fprs[slot].empty())) {
      continue;
    }
    if (double_accesses[slot] > 0 || has_split_pair) {
      result.insert(slot);
    }
  }
  return result;
}

absl::flat_hash_set<int> AbiAnalyzer::DetectFloatStackParameterSlots(const ControlFlowGraph& cfg) {
  absl::flat_hash_set<int> result;
  if (cfg.Blocks().empty()) return result;

  const uint32_t frame_size = ComputeMaxFrameSize(cfg);
  const auto id_to_index = BuildBlockIdToIndex(cfg);
  auto is_fpr_converted_from_int = [&](uint32_t start_block_id, size_t start_inst_idx,
                                       FpRegister fpr) -> bool {
    return IsFprConvertedFromInt(cfg, id_to_index, start_block_id, start_inst_idx, fpr);
  };

  const absl::flat_hash_set<Register> float_gprs = DetectFloatParametersInGeneralRegisters(cfg);
  const absl::flat_hash_set<int> double_slots = DetectDoubleStackParameterSlots(cfg);
  const int32_t base_offset = static_cast<int32_t>(frame_size + 16);

  absl::flat_hash_map<int, int> fp_accesses;
  absl::flat_hash_map<int, int> gpr_float_or_swc1_accesses;
  absl::flat_hash_map<int, int> non_fp_accesses;

  for (const auto& block : cfg.Blocks()) {
    for (size_t j = 0; j < block.instructions.size(); ++j) {
      const auto& inst = block.instructions[j];
      if (inst.rs != Register::kSp || inst.immediate < base_offset) continue;
      int32_t rel = inst.immediate - base_offset;
      int slot = 4 + (rel / 4);
      if (rel % 4 != 0) {
        non_fp_accesses[slot]++;
        continue;
      }
      if (inst.opcode == Opcode::kLwc1 && inst.ft.has_value()) {
        if (is_fpr_converted_from_int(block.id, j, *inst.ft)) {
          non_fp_accesses[slot]++;
        } else {
          fp_accesses[slot]++;
        }
      } else if (inst.opcode == Opcode::kSwc1) {
        fp_accesses[slot]++;
        gpr_float_or_swc1_accesses[slot]++;
      } else if (inst.opcode == Opcode::kLw && inst.rt.has_value() &&
                 float_gprs.contains(*inst.rt)) {
        bool is_callee_saved_float_gpr =
            (*inst.rt == Register::kFp || (*inst.rt >= Register::kS0 && *inst.rt <= Register::kS7));
        bool followed_by_mtc1 = is_callee_saved_float_gpr;
        for (size_t k = j + 1; !followed_by_mtc1 && k < block.instructions.size(); ++k) {
          const auto& next_inst = block.instructions[k];
          if (next_inst.opcode == Opcode::kMtc1 && next_inst.rt == inst.rt &&
              next_inst.fs.has_value() && !is_fpr_converted_from_int(block.id, k, *next_inst.fs)) {
            followed_by_mtc1 = true;
            break;
          }
          RegisterUseDef next_ud = GetInstructionUseDef(next_inst);
          bool touches_rt = false;
          for (Register u : next_ud.gpr_uses) {
            if (u == *inst.rt) touches_rt = true;
          }
          for (Register d : next_ud.gpr_defs) {
            if (d == *inst.rt) touches_rt = true;
          }
          if (touches_rt || next_inst.IsCall()) {
            break;
          }
        }
        if (followed_by_mtc1) {
          fp_accesses[slot]++;
          gpr_float_or_swc1_accesses[slot]++;
        } else {
          non_fp_accesses[slot]++;
        }
      } else if (inst.IsLoad() || inst.IsStore() || inst.opcode == Opcode::kAddiu ||
                 inst.opcode == Opcode::kAddi || inst.opcode == Opcode::kDaddiu ||
                 inst.opcode == Opcode::kDaddi) {
        non_fp_accesses[slot]++;
        if (inst.opcode == Opcode::kLdc1 || inst.opcode == Opcode::kSdc1 ||
            inst.opcode == Opcode::kLd || inst.opcode == Opcode::kSd) {
          non_fp_accesses[slot + 1]++;
        }
      }
    }
  }

  for (const auto& [slot, count] : fp_accesses) {
    if (double_slots.contains(slot & ~1)) {
      continue;
    }
    if (count > 0 && gpr_float_or_swc1_accesses[slot] > 0 && non_fp_accesses[slot] == 0) {
      result.insert(slot);
    }
  }
  return result;
}

absl::flat_hash_map<uint32_t, CallReturnInfo> AbiAnalyzer::ComputeCallReturnInfo(
    const ControlFlowGraph& cfg, bool function_returns_v0, bool function_returns_f0) {
  absl::flat_hash_map<uint32_t, CallReturnInfo> result;

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
          if (DefinesFprAsDouble(inst, reg)) return true;
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
          if (DefinesFprAsDouble(inst, reg)) return true;
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
            if (DefinesFprAsDouble(inst, reg)) return true;
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
        queue.push_back({block.id, i + 2, true, true, true, 128});
      } else {
        for (uint32_t succ_id : block.successors) {
          queue.push_back({succ_id, 0, true, true, true, 128});
        }
        if (block.successors.empty()) {
          const auto* next_block = cfg.FindBlockByVram(call_inst.vram + 8);
          if (next_block != nullptr) {
            queue.push_back({next_block->id, 0, true, true, true, 128});
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
            RecordReturnRegisterUses(inst, call_ud, &item.v0_live, &item.v1_live, &item.f0_live,
                                     &info);
            if (k + 1 < curr_block->instructions.size()) {
              const auto& delay_inst = curr_block->instructions[k + 1];
              RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
              RecordReturnRegisterUses(delay_inst, delay_ud, &item.v0_live, &item.v1_live,
                                       &item.f0_live, &info);
            }
            stop_path = true;
            break;
          }

          RegisterUseDef ud = GetInstructionUseDef(inst);
          RecordReturnRegisterUses(inst, ud, &item.v0_live, &item.v1_live, &item.f0_live, &info);
          RecordReturnRegisterDefs(ud, &item.v0_live, &item.v1_live, &item.f0_live);

          if (inst.IsReturn()) {
            if (k + 1 < curr_block->instructions.size()) {
              const auto& delay_inst = curr_block->instructions[k + 1];
              RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
              RecordReturnRegisterUses(delay_inst, delay_ud, &item.v0_live, &item.v1_live,
                                       &item.f0_live, &info);
              RecordReturnRegisterDefs(delay_ud, &item.v0_live, &item.v1_live, &item.f0_live);
            }
            if (item.v0_live && function_returns_v0 && !info.returns_f0) {
              info.returns_v0 = true;
            }
            if (item.f0_live && function_returns_f0 && !info.returns_v0) {
              info.returns_f0 = true;
            }
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
    for (size_t i = 0; i < block.instructions.size(); ++i) {
      const auto& inst = block.instructions[i];
      if (IsDoubleComparisonOp(inst.opcode)) {
        if (inst.fs) double_vars.insert(FpRegisterVarName(*inst.fs, true));
        if (inst.ft) double_vars.insert(FpRegisterVarName(*inst.ft, true));
        continue;
      }
      switch (inst.opcode) {
        case Opcode::kAddD:
        case Opcode::kSubD:
        case Opcode::kMulD:
        case Opcode::kDivD:
        case Opcode::kSqrtD:
        case Opcode::kAbsD:
        case Opcode::kMovD:
        case Opcode::kNegD:
        case Opcode::kRoundWD:
        case Opcode::kCeilWD:
        case Opcode::kFloorWD:
        case Opcode::kCvtLD:
        case Opcode::kCvtDL:
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
        case Opcode::kLwc1:
        case Opcode::kSwc1:
          if (inst.ft.has_value() && (static_cast<int>(*inst.ft) % 2 == 1)) {
            FpRegister even_reg = static_cast<FpRegister>(static_cast<int>(*inst.ft) - 1);
            double_vars.insert(FpRegisterVarName(even_reg, true));
          }
          break;
        case Opcode::kMfc1:
        case Opcode::kMtc1:
          if (inst.fs.has_value() && (static_cast<int>(*inst.fs) % 2 == 1)) {
            FpRegister even_reg = static_cast<FpRegister>(static_cast<int>(*inst.fs) - 1);
            double_vars.insert(FpRegisterVarName(even_reg, true));
            if (inst.opcode == Opcode::kMtc1 && inst.rt.has_value() &&
                (*inst.rt == Register::kS0 || *inst.rt == Register::kS2 ||
                 *inst.rt == Register::kS4 || *inst.rt == Register::kS6) &&
                i > 0) {
              size_t prev_idx = i - 1;
              if (prev_idx > 0 && block.instructions[prev_idx].IsNop()) {
                --prev_idx;
              }
              const auto& prev_inst = block.instructions[prev_idx];
              Register odd_gpr = static_cast<Register>(static_cast<int>(*inst.rt) + 1);
              if (prev_inst.opcode == Opcode::kMtc1 && prev_inst.fs == even_reg &&
                  prev_inst.rt == odd_gpr) {
                double_vars.insert(RegisterVarName(*inst.rt));
              }
            }
          }
          break;
        case Opcode::kDmfc1:
        case Opcode::kDmtc1:
          if (inst.fs) double_vars.insert(FpRegisterVarName(*inst.fs, true));
          break;
        case Opcode::kCvtDS:
        case Opcode::kCvtDW:
        case Opcode::kCvtLS:
          if (inst.fd) double_vars.insert(FpRegisterVarName(*inst.fd, true));
          break;
        case Opcode::kCvtSD:
        case Opcode::kCvtWD:
        case Opcode::kCvtSL:
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
  if ((static_cast<int>(reg) % 2 == 0) && static_cast<int>(reg) + 1 < 32) {
    FpRegister odd_reg = static_cast<FpRegister>(static_cast<int>(reg) + 1);
    if (IsFpRegisterLiveIn(cfg, odd_reg)) {
      return true;
    }
  }
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
          return IsDoublePrecisionUseOp(inst.opcode) && inst.opcode != Opcode::kDmfc1;
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
