#include "lifter/call_lifter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/expression_builder.h"

namespace rom_nom_nom {
namespace {

struct CallReturnUsage {
  bool v0_used_after_call = false;
  bool v1_used_after_call = false;
  bool f0_used_after_call = false;
  bool f0_is_double = false;
};

CallReturnUsage DetermineCallReturnUsage(
    const Instruction& inst, size_t i, absl::Span<const Instruction> instructions_to_process,
    const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info) {
  CallReturnUsage usage;

  if (call_return_info != nullptr) {
    auto it = call_return_info->find(inst.vram);
    if (it != call_return_info->end()) {
      if (it->second.returns_v0) usage.v0_used_after_call = true;
      if (it->second.returns_s64) {
        usage.v0_used_after_call = true;
        usage.v1_used_after_call = true;
      }
      if (it->second.returns_f0) usage.f0_used_after_call = true;
      if (it->second.returns_f0_double) {
        usage.f0_used_after_call = true;
        usage.f0_is_double = true;
      }
    }
  }

  CallReturnInfo scanned =
      AbiAnalyzer::ScanCallReturnUsage(instructions_to_process, i + 1, /*stop_at_branches=*/false);
  if (!usage.v0_used_after_call && scanned.returns_v0) {
    usage.v0_used_after_call = true;
  }
  if (!usage.v1_used_after_call && scanned.returns_s64) {
    usage.v1_used_after_call = true;
  }
  if (!usage.f0_used_after_call && scanned.returns_f0) {
    usage.f0_used_after_call = true;
    if (scanned.returns_f0_double) {
      usage.f0_is_double = true;
    }
  }
  return usage;
}

std::string FormatCallFunctionName(const Instruction& inst, const SymbolIndex* symbol_index,
                                   const CallReturnUsage& usage) {
  if (inst.opcode == Opcode::kJalr) {
    if (usage.v0_used_after_call && usage.v1_used_after_call && !usage.f0_used_after_call) {
      return absl::StrFormat("((s64 (*)())%s)",
                             ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    }
    if (usage.f0_used_after_call && usage.f0_is_double) {
      return absl::StrFormat("((f64 (*)())%s)",
                             ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    }
    if (usage.f0_used_after_call) {
      return absl::StrFormat("((f32 (*)())%s)",
                             ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    }
    if (usage.v0_used_after_call) {
      return absl::StrFormat("((s32 (*)())%s)",
                             ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
    }
    return absl::StrFormat("((void (*)())%s)",
                           ExpressionBuilder::RegisterVarName(inst.rs.value_or(Register::kT9)));
  }
  uint32_t target_vram = (inst.opcode == Opcode::kJal) ? inst.JumpTarget() : inst.BranchTarget();
  return (symbol_index != nullptr) ? symbol_index->LookupOrSynthesizeName(target_vram, SYMBOL_FUNC)
                                   : absl::StrFormat("func_%08X", target_vram);
}

int InferCallArgumentCount(size_t i, absl::Span<const Instruction> instructions_to_process,
                           const std::string& function_name,
                           const absl::flat_hash_map<std::string, int>* function_parameter_counts,
                           const RegisterTracker& tracker, bool sp_address_taken,
                           int32_t sp_base_offset,
                           const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets,
                           const ControlFlowGraph* cfg, uint32_t current_block_id) {
  static constexpr Register kArgumentRegisters[] = {Register::kA0, Register::kA1, Register::kA2,
                                                    Register::kA3};
  int highest_defined_argument_index = -1;
  for (int arg_index = 3; arg_index >= 0; --arg_index) {
    auto def = tracker.GetReachingDefinition(kArgumentRegisters[arg_index]);
    if (def.has_value() && def->is_local) {
      highest_defined_argument_index = arg_index;
      break;
    }
  }
  bool is_entry_block = (cfg == nullptr || current_block_id == cfg->EntryBlockId());
  if (is_entry_block && highest_defined_argument_index < 3) {
    bool has_prior_call = false;
    bool has_prologue_ra_save = false;
    bool arg_defined_before_call[4] = {false, false, false, false};
    bool arg_read_before_call[4] = {false, false, false, false};
    bool f12_defined_before_call = false;
    for (size_t j = 0; j < i; ++j) {
      const auto& prev = instructions_to_process[j];
      if (prev.IsCall()) {
        has_prior_call = true;
        break;
      }
      if (prev.IsStore() && prev.rs == Register::kSp && prev.rt == Register::kRa) {
        has_prologue_ra_save = true;
      }
      RegisterUseDef ud = GetInstructionUseDef(prev);
      for (Register d : ud.gpr_defs) {
        for (int k = 0; k < 4; ++k) {
          if (d == kArgumentRegisters[k]) {
            arg_defined_before_call[k] = true;
          }
        }
      }
      for (FpRegister fd : ud.fpr_defs) {
        if (fd == FpRegister::kF12) {
          f12_defined_before_call = true;
        }
      }
      for (Register u : ud.gpr_uses) {
        for (int k = 0; k < 4; ++k) {
          if (u == kArgumentRegisters[k]) {
            bool is_s_copy_before_mtc1 = false;
            if ((prev.opcode == Opcode::kAddu || prev.opcode == Opcode::kOr ||
                 prev.opcode == Opcode::kDaddu) &&
                prev.rt == Register::kZero && prev.rd.has_value() &&
                ((*prev.rd >= Register::kS0 && *prev.rd <= Register::kS7) ||
                 *prev.rd == Register::kFp)) {
              for (size_t m_idx = j + 1; m_idx < i; ++m_idx) {
                const auto& later = instructions_to_process[m_idx];
                if (later.opcode == Opcode::kMtc1 && later.rt.has_value()) {
                  for (int m = k + 1; m < 4; ++m) {
                    if (*later.rt == kArgumentRegisters[m]) {
                      is_s_copy_before_mtc1 = true;
                      break;
                    }
                  }
                  if (is_s_copy_before_mtc1) break;
                }
              }
            }
            if (!is_s_copy_before_mtc1) {
              arg_read_before_call[k] = true;
            }
          }
        }
      }
    }
    if (!has_prior_call && has_prologue_ra_save) {
      for (int k = 0; k < 3; ++k) {
        if (k == highest_defined_argument_index + 1) {
          auto def_k = tracker.GetReachingDefinition(kArgumentRegisters[k]);
          bool k_unmodified = (!def_k.has_value() || !def_k->is_local) &&
                              !arg_defined_before_call[k] && (k > 0 || !f12_defined_before_call) &&
                              !arg_read_before_call[k];
          bool higher_arg_read = false;
          for (int m = k + 1; m < 4; ++m) {
            if (arg_read_before_call[m]) {
              higher_arg_read = true;
              break;
            }
          }
          if (k_unmodified && higher_arg_read) {
            highest_defined_argument_index = k;
          }
        }
      }
    }
  }

  if (function_parameter_counts != nullptr) {
    auto it = function_parameter_counts->find(function_name);
    if (it != function_parameter_counts->end() && it->second >= 0) {
      return it->second;
    }
  }

  bool has_prior_call_in_block = false;
  for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
    if (instructions_to_process[j].IsCall()) {
      has_prior_call_in_block = true;
      break;
    }
  }

  int32_t max_stack_arg_offset = sp_address_taken ? sp_base_offset : 80;
  auto is_stack_arg_store = [&](const Instruction& prev_inst) -> bool {
    if (!prev_inst.IsStore() || prev_inst.rs != Register::kSp || prev_inst.immediate < 16 ||
        prev_inst.immediate >= max_stack_arg_offset || (prev_inst.immediate % 4 != 0)) {
      return false;
    }
    if (prev_inst.rt.has_value()) {
      Register rt = *prev_inst.rt;
      if (rt == Register::kRa || rt == Register::kFp) {
        return false;
      }
      if (rt >= Register::kS0 && rt <= Register::kS7) {
        if (saved_reg_offsets == nullptr) {
          return false;
        }
        auto it = saved_reg_offsets->find(rt);
        if (it == saved_reg_offsets->end() || it->second <= prev_inst.immediate) {
          return false;
        }
      }
    }
    return true;
  };

  if (!has_prior_call_in_block && cfg != nullptr) {
    const BasicBlock* curr_block = cfg->GetBlock(current_block_id);
    if (curr_block != nullptr && !curr_block->predecessors.empty()) {
      for (int arg_index = 3; arg_index > highest_defined_argument_index; --arg_index) {
        Register reg = kArgumentRegisters[arg_index];
        bool used_in_curr_before_call = false;
        for (size_t j = 0; j < i; ++j) {
          RegisterUseDef ud = GetInstructionUseDef(instructions_to_process[j]);
          for (Register u : ud.gpr_uses) {
            if (u == reg) {
              used_in_curr_before_call = true;
              break;
            }
          }
          if (used_in_curr_before_call) break;
        }
        if (used_in_curr_before_call) continue;

        bool defined_for_call_in_pred = false;
        for (uint32_t pred_id : curr_block->predecessors) {
          const BasicBlock* pred = cfg->GetBlock(pred_id);
          for (int hop = 0; hop < 2 && pred != nullptr && !pred->instructions.empty(); ++hop) {
            bool read_by_non_branch = false;
            bool read_by_branch_or_slt = false;
            bool hit_stop = false;
            size_t sz = pred->instructions.size();
            std::optional<Register> branch_cond_temp;
            if (sz >= 2 && (pred->instructions[sz - 2].IsBranch() ||
                            pred->instructions[sz - 2].opcode == Opcode::kJ ||
                            pred->instructions[sz - 2].opcode == Opcode::kJr)) {
              const auto& br_inst = pred->instructions[sz - 2];
              RegisterUseDef br_ud = GetInstructionUseDef(br_inst);
              for (Register u : br_ud.gpr_uses) {
                if (u == reg) read_by_branch_or_slt = true;
              }
              if (br_inst.opcode == Opcode::kBeq || br_inst.opcode == Opcode::kBne ||
                  br_inst.opcode == Opcode::kBeql || br_inst.opcode == Opcode::kBnel) {
                if (br_inst.rt == Register::kZero && br_inst.rs.has_value() &&
                    *br_inst.rs != Register::kZero) {
                  branch_cond_temp = *br_inst.rs;
                } else if (br_inst.rs == Register::kZero && br_inst.rt.has_value() &&
                           *br_inst.rt != Register::kZero) {
                  branch_cond_temp = *br_inst.rt;
                }
              }
            }
            for (int k = static_cast<int>(sz) - 1; k >= 0; --k) {
              const auto& pinst = pred->instructions[k];
              if (pinst.IsCall() || (k > 0 && pred->instructions[k - 1].IsCall())) {
                hit_stop = true;
                break;
              }
              if (static_cast<size_t>(k) + 2 == sz && pinst.IsBranch()) {
                continue;
              }
              RegisterUseDef pud = GetInstructionUseDef(pinst);
              bool defs_reg = false;
              for (Register d : pud.gpr_defs) {
                if (d == reg) defs_reg = true;
              }
              if (defs_reg) {
                bool is_branch_cond_flag_def =
                    branch_cond_temp.has_value() && *branch_cond_temp == reg &&
                    (pinst.opcode == Opcode::kSlt || pinst.opcode == Opcode::kSltu ||
                     pinst.opcode == Opcode::kSlti || pinst.opcode == Opcode::kSltiu);
                if (!read_by_non_branch && !is_branch_cond_flag_def) {
                  if (!read_by_branch_or_slt) {
                    defined_for_call_in_pred = true;
                  } else {
                    bool lower_args_ready = true;
                    for (int low_idx = 0; low_idx < arg_index; ++low_idx) {
                      Register low_reg = kArgumentRegisters[low_idx];
                      for (Register u : pud.gpr_uses) {
                        if (u == low_reg) lower_args_ready = false;
                      }
                      bool defined_in_curr = false;
                      for (size_t cj = 0; cj < i; ++cj) {
                        for (Register d :
                             GetInstructionUseDef(instructions_to_process[cj]).gpr_defs) {
                          if (d == low_reg) defined_in_curr = true;
                        }
                      }
                      bool defined_before_k = false;
                      for (int before = k - 1; before >= 0; --before) {
                        if (pred->instructions[before].IsCall()) break;
                        for (Register d :
                             GetInstructionUseDef(pred->instructions[before]).gpr_defs) {
                          if (d == low_reg) defined_before_k = true;
                        }
                        if (defined_before_k) break;
                      }
                      if (!defined_in_curr && !defined_before_k) lower_args_ready = false;
                      if (!lower_args_ready) break;
                    }
                    if (lower_args_ready) {
                      defined_for_call_in_pred = true;
                    }
                  }
                }
                hit_stop = true;
                break;
              }
              bool is_branch_slt_use =
                  (branch_cond_temp.has_value() &&
                   (pinst.opcode == Opcode::kSlt || pinst.opcode == Opcode::kSltu ||
                    pinst.opcode == Opcode::kSlti || pinst.opcode == Opcode::kSltiu) &&
                   ((pinst.rd.has_value() && *pinst.rd == *branch_cond_temp) ||
                    (pinst.rt.has_value() && *pinst.rt == *branch_cond_temp)));
              for (Register u : pud.gpr_uses) {
                if (u == reg) {
                  if (is_branch_slt_use) {
                    read_by_branch_or_slt = true;
                  } else {
                    read_by_non_branch = true;
                  }
                }
              }
            }
            if (defined_for_call_in_pred || hit_stop || read_by_non_branch ||
                read_by_branch_or_slt || pred->predecessors.size() != 1 ||
                pred->predecessors[0] == pred->id) {
              break;
            }
            pred = cfg->GetBlock(pred->predecessors[0]);
          }
          if (defined_for_call_in_pred) break;
        }
        if (defined_for_call_in_pred) {
          highest_defined_argument_index = arg_index;
          break;
        }
      }
    }
  }

  if (highest_defined_argument_index < 0) {
    for (int arg_index = 3; arg_index >= 0; --arg_index) {
      Register reg = kArgumentRegisters[arg_index];
      auto def = tracker.GetReachingDefinition(reg);
      if (def.has_value() && !def->is_local) {
        bool used_in_block_before_call = false;
        for (size_t j = 0; j < i; ++j) {
          RegisterUseDef ud = GetInstructionUseDef(instructions_to_process[j]);
          for (Register u : ud.gpr_uses) {
            if (u == reg) {
              used_in_block_before_call = true;
              break;
            }
          }
          if (used_in_block_before_call) break;
        }
        if (used_in_block_before_call) {
          highest_defined_argument_index = arg_index;
          break;
        }
      }
    }
  }

  if (max_stack_arg_offset > 16) {
    absl::flat_hash_set<int32_t> stack_arg_offsets;
    absl::flat_hash_set<int32_t> eight_byte_stack_offsets;
    auto record_stack_arg_store = [&](const Instruction& store_inst) {
      stack_arg_offsets.insert(store_inst.immediate);
      if (store_inst.opcode == Opcode::kSdc1 || store_inst.opcode == Opcode::kSd) {
        stack_arg_offsets.insert(store_inst.immediate + 4);
        eight_byte_stack_offsets.insert(store_inst.immediate);
      } else if (store_inst.opcode == Opcode::kSwc1 && store_inst.ft.has_value() &&
                 (static_cast<int>(*store_inst.ft) % 2 == 1) && (store_inst.immediate % 8 == 0)) {
        eight_byte_stack_offsets.insert(store_inst.immediate);
      }
    };
    for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
      const auto& prev_inst = instructions_to_process[j];
      if (prev_inst.IsCall()) break;
      if (is_stack_arg_store(prev_inst)) {
        record_stack_arg_store(prev_inst);
      }
    }
    if (!has_prior_call_in_block && cfg != nullptr) {
      const BasicBlock* curr_block = cfg->GetBlock(current_block_id);
      if (curr_block != nullptr) {
        for (uint32_t pred_id : curr_block->predecessors) {
          const BasicBlock* pred = cfg->GetBlock(pred_id);
          if (pred == nullptr) continue;
          for (int k = static_cast<int>(pred->instructions.size()) - 1; k >= 0; --k) {
            if (pred->instructions[k].IsCall()) break;
            if (is_stack_arg_store(pred->instructions[k])) {
              record_stack_arg_store(pred->instructions[k]);
            }
          }
        }
      }
    }
    if (stack_arg_offsets.contains(16)) {
      highest_defined_argument_index = std::max(highest_defined_argument_index, 3);
      while (true) {
        int32_t next_offset = 16 + (highest_defined_argument_index + 1 - 4) * 4;
        if (stack_arg_offsets.contains(next_offset)) {
          ++highest_defined_argument_index;
        } else if ((next_offset % 8 != 0) && eight_byte_stack_offsets.contains(next_offset + 4) &&
                   stack_arg_offsets.contains(next_offset + 4)) {
          ++highest_defined_argument_index;
        } else {
          break;
        }
      }
    }
  }
  return highest_defined_argument_index + 1;
}

std::vector<std::unique_ptr<LiftedExpression>> BuildCallArgumentExpressions(
    const Instruction& inst, size_t i, absl::Span<const Instruction> instructions_to_process,
    const std::string& function_name, int argument_count,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
    const RegisterTracker& tracker,
    const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info) {
  static constexpr Register kArgumentRegisters[] = {Register::kA0, Register::kA1, Register::kA2,
                                                    Register::kA3};
  int fp_arg_count = 0;
  bool known_fp_arg_count = false;
  if (function_fp_parameter_counts != nullptr) {
    auto it = function_fp_parameter_counts->find(function_name);
    if (it != function_fp_parameter_counts->end()) {
      fp_arg_count = it->second;
      known_fp_arg_count = true;
    }
  }
  if (!known_fp_arg_count) {
    auto a0_def = tracker.GetReachingDefinition(Register::kA0);
    bool a0_defined_for_call = a0_def.has_value() && a0_def->is_local;
    if (!a0_defined_for_call) {
      bool defines_f12 = false;
      bool defines_f14 = false;
      auto scan_fp_defs = [&](const Instruction& candidate) {
        RegisterUseDef ud = GetInstructionUseDef(candidate);
        for (FpRegister r : ud.fpr_defs) {
          if (r == FpRegister::kF12 || r == FpRegister::kF13) defines_f12 = true;
          if (r == FpRegister::kF14 || r == FpRegister::kF15) defines_f14 = true;
        }
      };
      if (i + 1 < instructions_to_process.size()) {
        scan_fp_defs(instructions_to_process[i + 1]);
      }
      for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
        if (instructions_to_process[j].opcode == Opcode::kJal ||
            instructions_to_process[j].opcode == Opcode::kJalr) {
          break;
        }
        scan_fp_defs(instructions_to_process[j]);
      }
      if (defines_f12) {
        fp_arg_count = defines_f14 ? 2 : 1;
      }
    }
  }

  std::vector<std::unique_ptr<LiftedExpression>> call_arguments;
  if (fp_arg_count > 0) {
    bool f12_double = false;
    bool f14_double = false;
    if (call_return_info != nullptr) {
      auto it = call_return_info->find(inst.vram);
      if (it != call_return_info->end()) {
        f12_double = it->second.arg_f12_double;
        f14_double = it->second.arg_f14_double;
      }
    }
    auto check_inst_double_fp_arg = [&](const Instruction& prev_inst) {
      if (prev_inst.opcode == Opcode::kCvtDW || prev_inst.opcode == Opcode::kCvtDS ||
          prev_inst.opcode == Opcode::kMovD || prev_inst.opcode == Opcode::kMulD ||
          prev_inst.opcode == Opcode::kAddD || prev_inst.opcode == Opcode::kSubD ||
          prev_inst.opcode == Opcode::kDivD || prev_inst.opcode == Opcode::kLdc1 ||
          prev_inst.opcode == Opcode::kDmtc1) {
        if (prev_inst.fd == FpRegister::kF12 || prev_inst.fs == FpRegister::kF12 ||
            prev_inst.ft == FpRegister::kF12) {
          f12_double = true;
        }
        if (prev_inst.fd == FpRegister::kF14 || prev_inst.fs == FpRegister::kF14 ||
            prev_inst.ft == FpRegister::kF14) {
          f14_double = true;
        }
      } else if (prev_inst.opcode == Opcode::kMtc1) {
        if (prev_inst.fs == FpRegister::kF13) f12_double = true;
        if (prev_inst.fs == FpRegister::kF15) f14_double = true;
      } else if (prev_inst.opcode == Opcode::kLwc1) {
        if (prev_inst.ft == FpRegister::kF13) f12_double = true;
        if (prev_inst.ft == FpRegister::kF15) f14_double = true;
      }
    };
    if (i + 1 < instructions_to_process.size()) {
      check_inst_double_fp_arg(instructions_to_process[i + 1]);
    }
    for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
      if (instructions_to_process[j].opcode == Opcode::kJal ||
          instructions_to_process[j].opcode == Opcode::kJalr) {
        break;
      }
      check_inst_double_fp_arg(instructions_to_process[j]);
    }
    int slot = 0;
    int fp_idx = 0;
    while (slot < argument_count || fp_idx < fp_arg_count) {
      if (fp_idx < fp_arg_count) {
        bool is_double = (fp_idx == 0) ? f12_double : f14_double;
        if (is_double && (slot % 2 != 0)) {
          slot++;
        }
        std::string fp_name =
            (fp_idx == 0) ? (is_double ? "f12_d" : "f12") : (is_double ? "f14_d" : "f14");
        call_arguments.push_back(LiftedExpression::Variable(fp_name));
        slot += (is_double ? 2 : 1);
        fp_idx++;
      } else {
        if (slot < 4 && tracker.GetReachingDefinition(kArgumentRegisters[slot]).has_value()) {
          call_arguments.push_back(
              ExpressionBuilder::LiftRegisterOrConstant(kArgumentRegisters[slot], tracker));
        } else if (slot < 4) {
          call_arguments.push_back(LiftedExpression::Variable(absl::StrFormat("arg%d", slot)));
        } else {
          int32_t stack_offset = 16 + (slot - 4) * 4;
          call_arguments.push_back(
              LiftedExpression::Variable(ExpressionBuilder::StackVarName(stack_offset)));
        }
        slot++;
      }
    }
    return call_arguments;
  }

  call_arguments.reserve(argument_count);
  for (int arg_index = 0; arg_index < argument_count; ++arg_index) {
    if (arg_index < 4 && tracker.GetReachingDefinition(kArgumentRegisters[arg_index]).has_value()) {
      call_arguments.push_back(
          ExpressionBuilder::LiftRegisterOrConstant(kArgumentRegisters[arg_index], tracker));
    } else if (arg_index < 4) {
      call_arguments.push_back(LiftedExpression::Variable(absl::StrFormat("arg%d", arg_index)));
    } else {
      int32_t stack_offset = 16 + (arg_index - 4) * 4;
      call_arguments.push_back(
          LiftedExpression::Variable(ExpressionBuilder::StackVarName(stack_offset)));
    }
  }

  if (call_arguments.empty()) {
    bool defines_f12 = false;
    bool defines_f14 = false;
    for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
      RegisterUseDef ud = GetInstructionUseDef(instructions_to_process[j]);
      for (FpRegister r : ud.fpr_defs) {
        if (r == FpRegister::kF12) defines_f12 = true;
        if (r == FpRegister::kF14) defines_f14 = true;
      }
      if (instructions_to_process[j].opcode == Opcode::kJal ||
          instructions_to_process[j].opcode == Opcode::kJalr) {
        break;
      }
    }
    if (defines_f12) {
      call_arguments.push_back(LiftedExpression::Variable("f12"));
      if (defines_f14) {
        call_arguments.push_back(LiftedExpression::Variable("f14"));
      }
    }
  }
  return call_arguments;
}

}  // namespace

void LiftCallInstruction(const Instruction& inst, size_t i,
                         absl::Span<const Instruction> instructions_to_process,
                         const SymbolIndex* symbol_index,
                         const absl::flat_hash_map<std::string, int>* function_parameter_counts,
                         const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
                         const absl::flat_hash_set<std::string>* functions_returning_v0,
                         const absl::flat_hash_set<std::string>* functions_returning_f0,
                         RegisterTracker& tracker, std::vector<LiftedStatement>& statements,
                         bool sp_address_taken,
                         const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info,
                         int32_t sp_base_offset,
                         const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets,
                         const ControlFlowGraph* cfg, uint32_t current_block_id) {
  CallReturnUsage usage =
      DetermineCallReturnUsage(inst, i, instructions_to_process, call_return_info);
  std::string function_name = FormatCallFunctionName(inst, symbol_index, usage);
  int argument_count = InferCallArgumentCount(
      i, instructions_to_process, function_name, function_parameter_counts, tracker,
      sp_address_taken, sp_base_offset, saved_reg_offsets, cfg, current_block_id);
  std::vector<std::unique_ptr<LiftedExpression>> call_arguments =
      BuildCallArgumentExpressions(inst, i, instructions_to_process, function_name, argument_count,
                                   function_fp_parameter_counts, tracker, call_return_info);

  bool returns_f0 = false;
  bool returns_v0 = false;
  if (inst.opcode == Opcode::kJal && functions_returning_v0 != nullptr &&
      functions_returning_f0 != nullptr) {
    if (functions_returning_f0->contains(function_name)) {
      returns_f0 = true;
    } else if (functions_returning_v0->contains(function_name)) {
      returns_v0 = true;
    }
  } else if (usage.v0_used_after_call && !usage.f0_used_after_call) {
    returns_v0 = true;
  } else if (usage.f0_used_after_call && !usage.v0_used_after_call) {
    returns_f0 = true;
  } else if (functions_returning_v0 != nullptr && functions_returning_v0->contains(function_name)) {
    returns_v0 = true;
  } else if (functions_returning_f0 != nullptr && functions_returning_f0->contains(function_name)) {
    returns_f0 = true;
  } else if (usage.v0_used_after_call) {
    returns_v0 = true;
  } else if (usage.f0_used_after_call) {
    returns_f0 = true;
  }

  if (usage.v0_used_after_call && usage.v1_used_after_call && !returns_f0) {
    LiftedStatement statement;
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = "call_res_64";
    std::string cast_func = function_name;
    if (!absl::StartsWith(cast_func, "((s64 (*)())")) {
      cast_func = absl::StrFormat("((s64 (*)())%s)", function_name);
    }
    statement.expression = LiftedExpression::Call(cast_func, std::move(call_arguments));
    statements.push_back(std::move(statement));

    LiftedStatement stmt_v0;
    stmt_v0.kind = StatementKind::kAssignment;
    stmt_v0.destination_variable = "v0";
    stmt_v0.expression = LiftedExpression::Unary(
        "(s32)", LiftedExpression::Binary(">>", LiftedExpression::Variable("call_res_64"),
                                          LiftedExpression::Integer(32)));
    statements.push_back(std::move(stmt_v0));

    LiftedStatement stmt_v1;
    stmt_v1.kind = StatementKind::kAssignment;
    stmt_v1.destination_variable = "v1";
    stmt_v1.expression =
        LiftedExpression::Unary("(s32)", LiftedExpression::Variable("call_res_64"));
    statements.push_back(std::move(stmt_v1));
    return;
  }

  LiftedStatement statement;
  if (returns_f0) {
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = usage.f0_is_double ? "f0_d" : "f0";
  } else if (returns_v0) {
    statement.kind = StatementKind::kAssignment;
    statement.destination_variable = "v0";
  } else {
    statement.kind = StatementKind::kCall;
  }
  statement.expression = LiftedExpression::Call(function_name, std::move(call_arguments));
  statements.push_back(std::move(statement));
}

}  // namespace rom_nom_nom
