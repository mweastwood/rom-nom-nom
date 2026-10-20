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
        inst.rt.has_value() && inst.immediate > 0) {
      Register r = *inst.rt;
      if (((r >= Register::kS0 && r <= Register::kS7) || r == Register::kFp ||
           r == Register::kRa) &&
          !defined_gprs.contains(r)) {
        result.saved_reg_offsets.try_emplace(r, inst.immediate);
        result.min_saved_reg_offset =
            std::min(result.min_saved_reg_offset, static_cast<uint32_t>(inst.immediate));
      }
    }

    if ((inst.opcode == Opcode::kSwc1 || inst.opcode == Opcode::kSdc1) &&
        (inst.rs == Register::kSp || (has_frame_pointer && inst.rs == Register::kFp)) &&
        inst.ft.has_value() && inst.immediate > 0) {
      FpRegister fr = *inst.ft;
      if (fr >= FpRegister::kF20 && fr <= FpRegister::kF31 && !defined_fprs.contains(fr)) {
        result.saved_fpr_offsets.try_emplace(fr, inst.immediate);
        result.min_saved_reg_offset =
            std::min(result.min_saved_reg_offset, static_cast<uint32_t>(inst.immediate));
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
  absl::flat_hash_map<int32_t, int> stack_access_sizes;
  absl::flat_hash_set<int32_t> loaded_offsets;
  absl::flat_hash_set<int32_t> addr_taken_offsets;

  for (const auto& block : cfg.Blocks()) {
    for (const auto& inst : block.instructions) {
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
           inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
          inst.rt.has_value() && *inst.rt == Register::kSp && inst.rs == Register::kSp &&
          inst.immediate < 0) {
        analysis.frame_size = std::max(analysis.frame_size, static_cast<int32_t>(-inst.immediate));
      }
      if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd ||
           inst.opcode == Opcode::kDaddu || inst.opcode == Opcode::kDadd) &&
          inst.rd.has_value() && *inst.rd == Register::kFp &&
          ((inst.rs == Register::kSp && inst.rt == Register::kZero) ||
           (inst.rs == Register::kZero && inst.rt == Register::kSp))) {
        analysis.has_frame_pointer = true;
      }
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
           inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
          inst.rt.has_value() && *inst.rt == Register::kFp && inst.rs == Register::kSp &&
          inst.immediate == 0) {
        analysis.has_frame_pointer = true;
      }
      if (inst.IsCall()) {
        analysis.has_calls = true;
      }
      if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
           inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
          (inst.rs == Register::kSp || (analysis.has_frame_pointer && inst.rs == Register::kFp)) &&
          inst.rt.has_value() && *inst.rt != Register::kSp &&
          (!analysis.has_frame_pointer || *inst.rt != Register::kFp) && inst.immediate >= 0) {
        analysis.sp_address_taken = true;
        addr_taken_offsets.insert(inst.immediate);
      }
      if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd ||
           inst.opcode == Opcode::kDaddu || inst.opcode == Opcode::kDadd ||
           inst.opcode == Opcode::kOr) &&
          (inst.rs == Register::kSp || inst.rt == Register::kSp ||
           (analysis.has_frame_pointer &&
            (inst.rs == Register::kFp || inst.rt == Register::kFp))) &&
          inst.rd.has_value() && *inst.rd != Register::kSp &&
          (!analysis.has_frame_pointer || *inst.rd != Register::kFp)) {
        analysis.sp_address_taken = true;
      }
      if ((inst.opcode == Opcode::kLwl || inst.opcode == Opcode::kLwr ||
           inst.opcode == Opcode::kSwl || inst.opcode == Opcode::kSwr ||
           inst.opcode == Opcode::kLdl || inst.opcode == Opcode::kLdr ||
           inst.opcode == Opcode::kSdl || inst.opcode == Opcode::kSdr) &&
          (inst.rs == Register::kSp || (analysis.has_frame_pointer && inst.rs == Register::kFp))) {
        analysis.sp_address_taken = true;
      }
      if (inst.rs == Register::kSp || (analysis.has_frame_pointer && inst.rs == Register::kFp)) {
        int32_t offset = inst.immediate;
        if (inst.IsStore()) {
          analysis.stack_store_offsets.insert(offset);
        }
        int32_t caller_frame_start = (analysis.frame_size > 0) ? analysis.frame_size : 16;
        if (offset >= caller_frame_start) {
          continue;
        }
        if (inst.IsStore()) {
          std::string var_name = offset < 0 ? absl::StrFormat("var_sp_neg_%d", -offset)
                                            : absl::StrFormat("var_sp_%d", offset);
          analysis.stack_stores.try_emplace(offset, std::move(var_name));
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
          auto [it, inserted] = stack_access_sizes.try_emplace(inst.immediate, size);
          if (!inserted && it->second != size) {
            analysis.sp_address_taken = true;
          }
        }
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
            if (r == Register::kRa || r == Register::kFp) {
              return;
            }
            if (r >= Register::kS0 && r <= Register::kS7) {
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

        for (int k = static_cast<int>(i) - 1; k >= 0; --k) {
          if (block.instructions[k].IsCall()) break;
          check_candidate(block.instructions[k]);
        }

        if (i + 1 < block.instructions.size()) {
          check_candidate(block.instructions[i + 1]);
        }

        if (call_candidate_stores.contains(16)) {
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
            int32_t access_size = (inst.opcode == Opcode::kSdc1 || inst.opcode == Opcode::kLdc1 ||
                                   inst.opcode == Opcode::kSd || inst.opcode == Opcode::kLd)
                                      ? 8
                                      : 4;
            analysis.max_sp_access = std::max(analysis.max_sp_access, adj + access_size);
          }
        } else if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
                    inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
                   inst.rt.has_value() && *inst.rt != Register::kSp &&
                   (!analysis.has_frame_pointer || *inst.rt != Register::kFp) &&
                   inst.immediate > 0) {
          int32_t adj = static_cast<int32_t>(inst.immediate) - sp_base_offset;
          if (adj > 0) {
            analysis.max_sp_access = std::max(analysis.max_sp_access, adj + 4);
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
    analysis.sp_buffer_size =
        (min_saved > static_cast<uint32_t>(sp_base_offset))
            ? static_cast<int32_t>(min_saved - static_cast<uint32_t>(sp_base_offset))
            : analysis.frame_size;
    if (analysis.max_sp_access > analysis.sp_buffer_size) {
      analysis.sp_buffer_size = (analysis.max_sp_access + 15) & ~15;
    }
  }

  return analysis;
}

}  // namespace rom_nom_nom
