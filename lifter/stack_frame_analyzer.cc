#include "lifter/stack_frame_analyzer.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"

namespace rom_nom_nom {

std::string StackFrameAnalyzer::StackVarName(int32_t offset) {
  if (offset < 0) {
    return absl::StrFormat("var_sp_neg_%d", -offset);
  }
  return absl::StrFormat("var_sp_%d", offset);
}

uint32_t StackFrameAnalyzer::ComputeFrameSize(const ControlFlowGraph& cfg) {
  uint32_t frame_size = 0;
  for (const auto& block : cfg.Blocks()) {
    for (const auto& inst : block.instructions) {
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
           inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
          inst.rt == Register::kSp && inst.rs == Register::kSp && inst.immediate < 0) {
        frame_size = std::max(frame_size, static_cast<uint32_t>(-inst.immediate));
      }
    }
  }
  return frame_size;
}

void StackFrameAnalyzer::ScanInstructionsPass1(absl::Span<const Instruction> instructions,
                                               StackFrameAnalysis* analysis) {
  for (const auto& inst : instructions) {
    if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
         inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
        inst.rt.has_value() && *inst.rt == Register::kSp && inst.rs == Register::kSp &&
        inst.immediate < 0) {
      analysis->frame_size = std::max(analysis->frame_size, static_cast<int32_t>(-inst.immediate));
    }
    if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd ||
         inst.opcode == Opcode::kDaddu || inst.opcode == Opcode::kDadd) &&
        inst.rd.has_value() && *inst.rd == Register::kFp &&
        ((inst.rs == Register::kSp && inst.rt == Register::kZero) ||
         (inst.rs == Register::kZero && inst.rt == Register::kSp))) {
      analysis->has_frame_pointer = true;
    }
    if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
         inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
        inst.rt.has_value() && *inst.rt == Register::kFp && inst.rs == Register::kSp &&
        inst.immediate == 0) {
      analysis->has_frame_pointer = true;
    }
    if (inst.IsCall()) {
      analysis->has_calls = true;
    }
    if (inst.IsStore() &&
        (inst.rs == Register::kSp || (analysis->has_frame_pointer && inst.rs == Register::kFp))) {
      analysis->stack_store_offsets.insert(inst.immediate);
    }
  }
}

bool StackFrameAnalyzer::IsCallerHomedScalarAddr(
    int32_t frame_size, int32_t imm, const absl::flat_hash_set<int32_t>& stack_store_offsets) {
  const int32_t caller_frame_start = (frame_size > 0) ? frame_size : 16;
  return frame_size > 0 && imm >= caller_frame_start && imm < caller_frame_start + 16 &&
         (imm % 4) == 0 && stack_store_offsets.contains(imm);
}

void StackFrameAnalyzer::ScanInstructionsAddressTaken(
    absl::Span<const Instruction> instructions, StackFrameAnalysis* analysis,
    absl::flat_hash_set<int32_t>* addr_taken_offsets) {
  for (const auto& inst : instructions) {
    if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
         inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
        (inst.rs == Register::kSp || (analysis->has_frame_pointer && inst.rs == Register::kFp)) &&
        inst.rt.has_value() && *inst.rt != Register::kSp &&
        (!analysis->has_frame_pointer || *inst.rt != Register::kFp) && inst.immediate >= 0 &&
        (analysis->frame_size == 0 || inst.immediate < analysis->frame_size) &&
        !IsCallerHomedScalarAddr(analysis->frame_size, inst.immediate,
                                 analysis->stack_store_offsets)) {
      analysis->sp_address_taken = true;
      if (addr_taken_offsets != nullptr) {
        addr_taken_offsets->insert(inst.immediate);
      }
    }
    if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd ||
         inst.opcode == Opcode::kDaddu || inst.opcode == Opcode::kDadd ||
         inst.opcode == Opcode::kOr) &&
        (inst.rs == Register::kSp || inst.rt == Register::kSp ||
         (analysis->has_frame_pointer && (inst.rs == Register::kFp || inst.rt == Register::kFp))) &&
        inst.rd.has_value() && *inst.rd != Register::kSp &&
        (!analysis->has_frame_pointer || *inst.rd != Register::kFp)) {
      analysis->sp_address_taken = true;
    }
    if ((inst.opcode == Opcode::kLwl || inst.opcode == Opcode::kLwr ||
         inst.opcode == Opcode::kSwl || inst.opcode == Opcode::kSwr ||
         inst.opcode == Opcode::kLdl || inst.opcode == Opcode::kLdr ||
         inst.opcode == Opcode::kSdl || inst.opcode == Opcode::kSdr) &&
        (inst.rs == Register::kSp || (analysis->has_frame_pointer && inst.rs == Register::kFp))) {
      analysis->sp_address_taken = true;
    }
  }
}

PrologueAnalysis StackFrameAnalyzer::AnalyzePrologue(const ControlFlowGraph& cfg,
                                                     bool has_frame_pointer) {
  PrologueAnalysis result;
  if (cfg.Blocks().empty()) {
    return result;
  }

  absl::flat_hash_set<Register> defined_gprs;
  absl::flat_hash_set<FpRegister> defined_fprs;

  auto check_saved_inst = [&](const Instruction& inst) {
    if ((inst.opcode == Opcode::kSw || inst.opcode == Opcode::kSd) &&
        (inst.rs == Register::kSp || (has_frame_pointer && inst.rs == Register::kFp)) &&
        inst.rt.has_value() && inst.immediate >= 0) {
      Register r = *inst.rt;
      if (r == Register::kRa ||
          (((r >= Register::kS0 && r <= Register::kS7) || r == Register::kFp) &&
           !defined_gprs.contains(r))) {
        auto [it, inserted] = result.saved_reg_offsets.try_emplace(r, inst.immediate);
        if (inserted) {
          result.min_saved_reg_offset =
              std::min(result.min_saved_reg_offset, static_cast<uint32_t>(inst.immediate));
        }
      }
    }

    if ((inst.opcode == Opcode::kSwc1 || inst.opcode == Opcode::kSdc1) &&
        (inst.rs == Register::kSp || (has_frame_pointer && inst.rs == Register::kFp)) &&
        inst.ft.has_value() && inst.immediate >= 0) {
      FpRegister fr = *inst.ft;
      if (fr >= FpRegister::kF20 && fr <= FpRegister::kF31 && !defined_fprs.contains(fr)) {
        auto [it, inserted] = result.saved_fpr_offsets.try_emplace(fr, inst.immediate);
        if (inserted) {
          result.min_saved_reg_offset =
              std::min(result.min_saved_reg_offset, static_cast<uint32_t>(inst.immediate));
        }
      }
    }
  };

  for (uint32_t block_id : cfg.ReversePostOrder()) {
    const auto* block = cfg.GetBlock(block_id);
    if (block == nullptr) {
      continue;
    }

    bool stop_traversal = false;
    for (size_t i = 0; i < block->instructions.size(); ++i) {
      const auto& inst = block->instructions[i];
      if (inst.IsCall()) {
        if (i + 1 < block->instructions.size()) {
          check_saved_inst(block->instructions[i + 1]);
        }
        stop_traversal = true;
        break;
      }

      if (inst.opcode == Opcode::kJr && inst.rs == Register::kRa) {
        stop_traversal = true;
        break;
      }

      check_saved_inst(inst);

      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register d : ud.gpr_defs) {
        if (d != Register::kZero) {
          defined_gprs.insert(d);
        }
      }
      for (FpRegister fd : ud.fpr_defs) {
        defined_fprs.insert(fd);
      }
    }

    if (stop_traversal) {
      break;
    }
  }

  return result;
}

StackFrameAnalysis StackFrameAnalyzer::Analyze(const ControlFlowGraph& cfg) {
  StackFrameAnalysis analysis;
  absl::flat_hash_map<int32_t, int> stack_store_sizes;
  std::vector<std::pair<int32_t, int>> stack_loads;
  absl::flat_hash_set<int32_t> loaded_offsets;
  absl::flat_hash_set<int32_t> addr_taken_offsets;

  for (const auto& block : cfg.Blocks()) {
    ScanInstructionsPass1(block.instructions, &analysis);
  }

  const int32_t caller_frame_start = (analysis.frame_size > 0) ? analysis.frame_size : 16;
  const BasicBlock* entry_block = cfg.GetBlock(cfg.EntryBlockId());
  if (entry_block != nullptr && analysis.frame_size > 0) {
    for (const auto& inst : entry_block->instructions) {
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
           inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
          inst.rt == Register::kSp && inst.rs == Register::kSp && inst.immediate < 0) {
        break;
      }
      if (inst.IsStore() && inst.rs == Register::kSp && inst.immediate >= 0 &&
          inst.immediate < 16) {
        analysis.stack_store_offsets.insert(inst.immediate + analysis.frame_size);
      }
    }
  }

  for (const auto& block : cfg.Blocks()) {
    ScanInstructionsAddressTaken(block.instructions, &analysis, &addr_taken_offsets);
    for (const auto& inst : block.instructions) {
      if (inst.rs == Register::kSp || (analysis.has_frame_pointer && inst.rs == Register::kFp)) {
        int32_t offset = inst.immediate;
        if (inst.IsStore()) {
          analysis.stack_store_offsets.insert(offset);
        }
        if (offset >= caller_frame_start) {
          continue;
        }
        if (inst.IsStore()) {
          analysis.stack_stores.try_emplace(offset, StackVarName(offset));
        }
        if (inst.IsLoad()) {
          loaded_offsets.insert(offset);
        }
        int size = 0;
        if (inst.opcode == Opcode::kLdc1 || inst.opcode == Opcode::kSdc1 ||
            inst.opcode == Opcode::kLd || inst.opcode == Opcode::kSd) {
          size = 8;
        } else if (inst.opcode == Opcode::kLw || inst.opcode == Opcode::kSw ||
                   inst.opcode == Opcode::kLwc1 || inst.opcode == Opcode::kSwc1) {
          size = 4;
        } else if (inst.opcode == Opcode::kLh || inst.opcode == Opcode::kLhu ||
                   inst.opcode == Opcode::kSh) {
          size = 2;
        } else if (inst.opcode == Opcode::kLb || inst.opcode == Opcode::kLbu ||
                   inst.opcode == Opcode::kSb) {
          size = 1;
        }
        if (size > 0) {
          if (inst.IsStore()) {
            auto [it, inserted] = stack_store_sizes.try_emplace(inst.immediate, size);
            if (!inserted && it->second != size) {
              analysis.sp_address_taken = true;
            }
          } else if (inst.IsLoad()) {
            stack_loads.emplace_back(inst.immediate, size);
          }
        }
      }
    }
  }

  absl::flat_hash_map<int32_t, int> stack_load_sizes;
  for (const auto& [offset, load_size] : stack_loads) {
    auto store_it = stack_store_sizes.find(offset);
    if (store_it != stack_store_sizes.end()) {
      int store_size = store_it->second;
      if (load_size > store_size) {
        analysis.sp_address_taken = true;
      } else if (load_size < store_size) {
        for (int32_t sub = offset + 1; sub < offset + store_size; ++sub) {
          if (stack_store_sizes.contains(sub)) {
            analysis.sp_address_taken = true;
            break;
          }
        }
      }
      continue;
    }
    int enclosing_store_size = 0;
    if (auto it4 = stack_store_sizes.find(offset & ~3);
        it4 != stack_store_sizes.end() && it4->second == 4) {
      enclosing_store_size = 4;
    } else if (auto it8 = stack_store_sizes.find(offset & ~7);
               it8 != stack_store_sizes.end() && it8->second == 8) {
      enclosing_store_size = 8;
    }
    if (enclosing_store_size == 0 || load_size > enclosing_store_size) {
      auto [lit, inserted] = stack_load_sizes.try_emplace(offset, load_size);
      if (!inserted && lit->second != load_size) {
        analysis.sp_address_taken = true;
      }
    }
  }

  analysis.prologue = AnalyzePrologue(cfg, analysis.has_frame_pointer);

  if (analysis.has_calls) {
    analysis.outgoing_args_size = (analysis.frame_size > 16) ? 16 : 0;
    for (const auto& block : cfg.Blocks()) {
      for (size_t i = 0; i < block.instructions.size(); ++i) {
        if (!block.instructions[i].IsCall()) {
          continue;
        }

        struct ArgStore {
          int32_t offset = 0;
          int32_t size = 4;
        };
        absl::flat_hash_map<int32_t, ArgStore> call_candidate_stores;

        auto check_candidate = [&](const Instruction& inst) {
          if (!inst.IsStore()) return;
          if (inst.rs != Register::kSp &&
              (!analysis.has_frame_pointer || inst.rs != Register::kFp)) {
            return;
          }
          int32_t offset = inst.immediate;
          if (offset < 16) return;
          if (offset >= static_cast<int32_t>(analysis.prologue.min_saved_reg_offset)) return;
          if (inst.rt.has_value()) {
            Register r = *inst.rt;
            if (r == Register::kRa || (analysis.has_frame_pointer && r == Register::kFp)) {
              return;
            }
            if ((r >= Register::kS0 && r <= Register::kS7) || r == Register::kFp) {
              auto it = analysis.prologue.saved_reg_offsets.find(r);
              if (it == analysis.prologue.saved_reg_offsets.end() ||
                  static_cast<int32_t>(it->second) <= offset) {
                return;
              }
            }
          }
          if (loaded_offsets.contains(offset)) return;
          if (addr_taken_offsets.contains(offset)) return;

          int32_t sz = (inst.opcode == Opcode::kSdc1 || inst.opcode == Opcode::kSd) ? 8 : 4;
          call_candidate_stores[offset] = ArgStore{offset, sz};
        };

        bool saw_prior_call = false;
        bool defined_arg2_or_3 = false;
        auto check_arg_def = [&](const Instruction& inst) {
          RegisterUseDef ud = GetInstructionUseDef(inst);
          for (Register d : ud.gpr_defs) {
            if (d == Register::kA2 || d == Register::kA3) {
              defined_arg2_or_3 = true;
            }
          }
          for (FpRegister fd : ud.fpr_defs) {
            if (fd == FpRegister::kF14 || fd == FpRegister::kF15) {
              defined_arg2_or_3 = true;
            }
          }
        };
        for (int k = static_cast<int>(i) - 1; k >= 0; --k) {
          if (block.instructions[k].IsCall()) {
            saw_prior_call = true;
            break;
          }
          check_candidate(block.instructions[k]);
          check_arg_def(block.instructions[k]);
        }

        if (!saw_prior_call) {
          bool all_preds_end_with_call = !block.predecessors.empty();
          for (uint32_t pred_id : block.predecessors) {
            const auto* pred = cfg.GetBlock(pred_id);
            if (pred == nullptr) continue;
            if (pred->instructions.size() >= 2 &&
                pred->instructions[pred->instructions.size() - 2].IsCall()) {
              continue;
            }
            all_preds_end_with_call = false;
            for (int k = static_cast<int>(pred->instructions.size()) - 1; k >= 0; --k) {
              if (pred->instructions[k].IsCall()) break;
              check_candidate(pred->instructions[k]);
              check_arg_def(pred->instructions[k]);
            }
          }
          if (all_preds_end_with_call) {
            saw_prior_call = true;
          }
        }

        if (i + 1 < block.instructions.size()) {
          check_candidate(block.instructions[i + 1]);
          check_arg_def(block.instructions[i + 1]);
        }

        if (call_candidate_stores.contains(16) && (!saw_prior_call || defined_arg2_or_3)) {
          int32_t curr = 16;
          int32_t max_arg_end = 16;
          while (call_candidate_stores.contains(curr)) {
            int32_t sz = call_candidate_stores[curr].size;
            max_arg_end = std::max(max_arg_end, curr + sz);
            curr += sz;
            if ((curr % 8 != 0) && !call_candidate_stores.contains(curr) &&
                call_candidate_stores.contains(curr + 4) &&
                call_candidate_stores[curr + 4].size == 8) {
              curr += 4;
            }
          }
          int32_t call_args_size = (max_arg_end + 7) & ~7;
          analysis.outgoing_args_size = std::max(analysis.outgoing_args_size, call_args_size);
        }
      }
    }

    if (analysis.prologue.min_saved_reg_offset != 0xFFFFFFFF) {
      analysis.outgoing_args_size =
          std::min(analysis.outgoing_args_size,
                   static_cast<int32_t>(analysis.prologue.min_saved_reg_offset));
    }
    if (analysis.frame_size > 0) {
      analysis.outgoing_args_size = std::min(analysis.outgoing_args_size, analysis.frame_size);
    }
  }

  int32_t sp_base_offset = analysis.outgoing_args_size;
  for (const auto& block : cfg.Blocks()) {
    for (const auto& inst : block.instructions) {
      if (inst.rs == Register::kSp || (analysis.has_frame_pointer && inst.rs == Register::kFp)) {
        if (inst.rt.has_value() && (*inst.rt == Register::kRa || *inst.rt == Register::kFp ||
                                    (*inst.rt >= Register::kS0 && *inst.rt <= Register::kS7))) {
          continue;
        }
        int32_t offset = inst.immediate;
        int32_t caller_frame_start = (analysis.frame_size > 0) ? analysis.frame_size : 16;
        if (offset >= caller_frame_start) {
          continue;
        }
        if (offset >= static_cast<int32_t>(analysis.prologue.min_saved_reg_offset)) {
          continue;
        }
        if ((inst.IsLoad() || inst.IsStore()) && inst.immediate > 0) {
          int32_t adj = static_cast<int32_t>(inst.immediate) - sp_base_offset;
          if (adj > 0) {
            int32_t access_size = 4;
            if (inst.opcode == Opcode::kSdc1 || inst.opcode == Opcode::kLdc1 ||
                inst.opcode == Opcode::kSd || inst.opcode == Opcode::kLd) {
              access_size = 8;
            } else if (inst.opcode == Opcode::kLh || inst.opcode == Opcode::kLhu ||
                       inst.opcode == Opcode::kSh) {
              access_size = 2;
            } else if (inst.opcode == Opcode::kLb || inst.opcode == Opcode::kLbu ||
                       inst.opcode == Opcode::kSb) {
              access_size = 1;
            }
            analysis.max_sp_access = std::max(analysis.max_sp_access, adj + access_size);
          }
        } else if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
                    inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
                   inst.rt.has_value() && *inst.rt != Register::kSp &&
                   (!analysis.has_frame_pointer || *inst.rt != Register::kFp) &&
                   inst.immediate > 0) {
          int32_t adj = static_cast<int32_t>(inst.immediate) - sp_base_offset;
          if (adj > 0) {
            int32_t addr_access_size = ((inst.immediate & 3) == 0) ? 4 : 1;
            analysis.max_sp_access = std::max(analysis.max_sp_access, adj + addr_access_size);
          }
        }
      }
    }
  }

  if (analysis.sp_address_taken && analysis.frame_size > 0) {
    uint32_t min_saved = analysis.prologue.min_saved_reg_offset;
    if (min_saved == 0xFFFFFFFF) {
      min_saved = static_cast<uint32_t>(analysis.frame_size);
    }
    if (min_saved <= static_cast<uint32_t>(sp_base_offset)) {
      analysis.sp_buffer_size = 0;
      analysis.sp_address_taken = false;
    } else {
      analysis.sp_buffer_size =
          static_cast<int32_t>(min_saved - static_cast<uint32_t>(sp_base_offset));
      if (analysis.max_sp_access > analysis.sp_buffer_size) {
        analysis.sp_buffer_size = (analysis.max_sp_access + 15) & ~15;
      }
    }
  }

  return analysis;
}

}  // namespace rom_nom_nom
