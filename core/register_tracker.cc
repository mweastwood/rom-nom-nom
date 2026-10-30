#include "core/register_tracker.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/mips.h"

namespace rom_nom_nom {

namespace {

constexpr size_t kNumGprs = 32;

size_t RegIdx(Register reg) {
  return static_cast<size_t>(reg);
}

struct CfgReachabilityGraph {
  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  std::vector<bool> reachable;
  std::vector<std::vector<size_t>> predecessors;
  bool has_reachable_return = false;
};

CfgReachabilityGraph BuildCfgReachabilityGraph(const ControlFlowGraph& cfg,
                                               bool reachable_only_predecessors = true) {
  CfgReachabilityGraph graph;
  const size_t num_blocks = cfg.Blocks().size();
  if (num_blocks == 0) {
    return graph;
  }

  graph.id_to_index.reserve(num_blocks);
  for (size_t i = 0; i < num_blocks; ++i) {
    graph.id_to_index[cfg.Blocks()[i].id] = i;
  }

  graph.reachable.assign(num_blocks, false);
  std::vector<size_t> worklist = {0};
  graph.reachable[0] = true;
  while (!worklist.empty()) {
    size_t curr = worklist.back();
    worklist.pop_back();
    for (uint32_t succ_id : cfg.Blocks()[curr].successors) {
      auto it = graph.id_to_index.find(succ_id);
      if (it != graph.id_to_index.end() && !graph.reachable[it->second]) {
        graph.reachable[it->second] = true;
        worklist.push_back(it->second);
      }
    }
  }

  for (size_t i = 0; i < num_blocks; ++i) {
    if (graph.reachable[i] && cfg.Blocks()[i].HasReturn()) {
      graph.has_reachable_return = true;
      break;
    }
  }

  graph.predecessors.resize(num_blocks);
  for (size_t u = 0; u < num_blocks; ++u) {
    if (reachable_only_predecessors && !graph.reachable[u]) {
      continue;
    }
    for (uint32_t succ_id : cfg.Blocks()[u].successors) {
      auto it = graph.id_to_index.find(succ_id);
      if (it != graph.id_to_index.end()) {
        graph.predecessors[it->second].push_back(u);
      }
    }
  }

  return graph;
}

bool BlockUsesV0BeforeRedef(const ControlFlowGraph& cfg,
                            const absl::flat_hash_map<uint32_t, size_t>& id_to_index,
                            const BasicBlock& start_blk) {
  const BasicBlock* blk = &start_blk;
  for (int hop = 0; hop < 4 && blk != nullptr; ++hop) {
    for (size_t idx = 0; idx < blk->instructions.size(); ++idx) {
      const auto& inst = blk->instructions[idx];
      if (inst.IsCall()) {
        if (idx + 1 < blk->instructions.size()) {
          RegisterUseDef delay_ud = GetInstructionUseDef(blk->instructions[idx + 1]);
          for (Register u : delay_ud.gpr_uses) {
            if (u == Register::kV0) return true;
          }
        }
        RegisterUseDef call_ud = GetInstructionUseDef(inst);
        for (Register u : call_ud.gpr_uses) {
          if (u == Register::kV0) return true;
        }
        return false;
      }
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register u : ud.gpr_uses) {
        if (u == Register::kV0) return true;
      }
      for (Register d : ud.gpr_defs) {
        if (d == Register::kV0) return false;
      }
    }
    if (blk->HasReturn() || blk->successors.size() != 1) {
      break;
    }
    auto next_it = id_to_index.find(blk->successors[0]);
    if (next_it == id_to_index.end()) {
      break;
    }
    blk = &cfg.Blocks()[next_it->second];
  }
  return false;
}

bool ShouldStripDelaySlotV0DefOnEdge(const ControlFlowGraph& cfg, const BasicBlock& pred_block,
                                     uint32_t target_block_id,
                                     absl::Span<const Instruction> executed,
                                     const absl::flat_hash_map<uint32_t, size_t>& id_to_index) {
  const auto* term = pred_block.Terminator();
  if (term == nullptr || !term->IsBranch() || executed.size() <= 1) {
    return false;
  }

  std::optional<EdgeType> opposite_edge_type;
  for (const auto& edge : pred_block.outgoing_edges) {
    if (edge.to_block_id == target_block_id) {
      if (edge.type == EdgeType::kBranch) {
        opposite_edge_type = EdgeType::kFallthrough;
        break;
      }
      if (edge.type == EdgeType::kFallthrough && !term->IsBranchLikely()) {
        opposite_edge_type = EdgeType::kBranch;
        break;
      }
    }
  }
  if (!opposite_edge_type.has_value()) {
    return false;
  }

  RegisterUseDef delay_ud = GetInstructionUseDef(executed.back());
  bool delay_defs_v0 = false;
  for (Register d : delay_ud.gpr_defs) {
    if (d == Register::kV0) {
      delay_defs_v0 = true;
      break;
    }
  }
  if (!delay_defs_v0) {
    return false;
  }

  for (const auto& edge : pred_block.outgoing_edges) {
    if (edge.type == *opposite_edge_type) {
      auto opp_it = id_to_index.find(edge.to_block_id);
      if (opp_it != id_to_index.end()) {
        return BlockUsesV0BeforeRedef(cfg, id_to_index, cfg.Blocks()[opp_it->second]);
      }
      break;
    }
  }
  return false;
}

}  // namespace

const std::vector<size_t>& RegisterTracker::EmptyVector() {
  static const auto* empty_vector = new std::vector<size_t>();
  return *empty_vector;
}

RegisterUseDef GetInstructionUseDef(const Instruction& inst) {
  RegisterUseDef ud;

  auto add_gpr_use = [&](std::optional<Register> r) {
    if (r.has_value() && *r != Register::kZero) {
      ud.gpr_uses.push_back(*r);
    }
  };

  auto add_gpr_def = [&](std::optional<Register> r) {
    if (r.has_value() && *r != Register::kZero) {
      ud.gpr_defs.push_back(*r);
    }
  };

  switch (inst.opcode) {
    // Arithmetic & Logic (Register)
    case Opcode::kAdd:
    case Opcode::kAddu:
    case Opcode::kSub:
    case Opcode::kSubu:
    case Opcode::kDadd:
    case Opcode::kDaddu:
    case Opcode::kDsub:
    case Opcode::kDsubu:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kNor:
    case Opcode::kSlt:
    case Opcode::kSltu:
      add_gpr_use(inst.rs);
      add_gpr_use(inst.rt);
      add_gpr_def(inst.rd);
      break;

    // Shift Immediate
    case Opcode::kSll:
    case Opcode::kSrl:
    case Opcode::kSra:
    case Opcode::kDsll:
    case Opcode::kDsrl:
    case Opcode::kDsra:
    case Opcode::kDsll32:
    case Opcode::kDsrl32:
    case Opcode::kDsra32:
      add_gpr_use(inst.rt);
      add_gpr_def(inst.rd);
      break;

    // Shift Variable
    case Opcode::kSllv:
    case Opcode::kSrlv:
    case Opcode::kSrav:
    case Opcode::kDsllv:
    case Opcode::kDsrlv:
    case Opcode::kDsrav:
      add_gpr_use(inst.rs);
      add_gpr_use(inst.rt);
      add_gpr_def(inst.rd);
      break;

    // Immediate Arithmetic & Logic
    case Opcode::kAddi:
    case Opcode::kAddiu:
    case Opcode::kDaddi:
    case Opcode::kDaddiu:
    case Opcode::kAndi:
    case Opcode::kOri:
    case Opcode::kXori:
    case Opcode::kSlti:
    case Opcode::kSltiu:
      add_gpr_use(inst.rs);
      add_gpr_def(inst.rt);
      break;

    case Opcode::kLui:
      add_gpr_def(inst.rt);
      break;

    // Loads
    case Opcode::kLw:
    case Opcode::kLh:
    case Opcode::kLhu:
    case Opcode::kLb:
    case Opcode::kLbu:
    case Opcode::kLwl:
    case Opcode::kLwr:
    case Opcode::kLwu:
    case Opcode::kLd:
    case Opcode::kLld:
    case Opcode::kLdl:
    case Opcode::kLdr:
      add_gpr_use(inst.rs);
      add_gpr_def(inst.rt);
      break;

    // Stores
    case Opcode::kSw:
    case Opcode::kSh:
    case Opcode::kSb:
    case Opcode::kSwl:
    case Opcode::kSwr:
    case Opcode::kSd:
    case Opcode::kSdl:
    case Opcode::kSdr:
      add_gpr_use(inst.rs);
      add_gpr_use(inst.rt);
      break;

    case Opcode::kScd:
      add_gpr_use(inst.rs);
      add_gpr_use(inst.rt);
      add_gpr_def(inst.rt);
      break;

    // Multiply / Divide
    case Opcode::kMult:
    case Opcode::kMultu:
    case Opcode::kDiv:
    case Opcode::kDivu:
    case Opcode::kDmult:
    case Opcode::kDmultu:
    case Opcode::kDdiv:
    case Opcode::kDdivu:
      add_gpr_use(inst.rs);
      add_gpr_use(inst.rt);
      ud.defs_hi = true;
      ud.defs_lo = true;
      break;

    case Opcode::kMfhi:
      ud.uses_hi = true;
      add_gpr_def(inst.rd);
      break;
    case Opcode::kMflo:
      ud.uses_lo = true;
      add_gpr_def(inst.rd);
      break;
    case Opcode::kMthi:
      add_gpr_use(inst.rs);
      ud.defs_hi = true;
      break;
    case Opcode::kMtlo:
      add_gpr_use(inst.rs);
      ud.defs_lo = true;
      break;

    // Conditional branches
    case Opcode::kBeq:
    case Opcode::kBne:
    case Opcode::kBeql:
    case Opcode::kBnel:
      add_gpr_use(inst.rs);
      add_gpr_use(inst.rt);
      break;

    case Opcode::kBlez:
    case Opcode::kBgtz:
    case Opcode::kBltz:
    case Opcode::kBgez:
    case Opcode::kBlezl:
    case Opcode::kBgtzl:
    case Opcode::kBltzl:
    case Opcode::kBgezl:
      add_gpr_use(inst.rs);
      break;

    case Opcode::kBltzal:
    case Opcode::kBgezal:
    case Opcode::kBltzall:
    case Opcode::kBgezall:
      add_gpr_use(inst.rs);
      add_gpr_def(Register::kRa);
      break;

    // Jumps
    case Opcode::kJal:
      add_gpr_def(Register::kRa);
      break;
    case Opcode::kJr:
      add_gpr_use(inst.rs);
      break;
    case Opcode::kJalr:
      add_gpr_use(inst.rs);
      add_gpr_def(inst.rd.value_or(Register::kRa));
      break;

    // FPU instructions
    case Opcode::kLwc1:
    case Opcode::kLdc1:
      add_gpr_use(inst.rs);
      if (inst.ft) ud.fpr_defs.push_back(*inst.ft);
      break;

    case Opcode::kSwc1:
    case Opcode::kSdc1:
      add_gpr_use(inst.rs);
      if (inst.ft) ud.fpr_uses.push_back(*inst.ft);
      break;

    case Opcode::kMfc1:
    case Opcode::kDmfc1:
      if (inst.fs) ud.fpr_uses.push_back(*inst.fs);
      add_gpr_def(inst.rt);
      break;

    case Opcode::kMtc1:
    case Opcode::kDmtc1:
      add_gpr_use(inst.rt);
      if (inst.fs) ud.fpr_defs.push_back(*inst.fs);
      break;

    case Opcode::kCfc1:
      add_gpr_def(inst.rt);
      break;

    case Opcode::kCtc1:
      add_gpr_use(inst.rt);
      break;

    // FPU 3-register arithmetic
    case Opcode::kAddS:
    case Opcode::kSubS:
    case Opcode::kMulS:
    case Opcode::kDivS:
    case Opcode::kAddD:
    case Opcode::kSubD:
    case Opcode::kMulD:
    case Opcode::kDivD:
      if (inst.fs) ud.fpr_uses.push_back(*inst.fs);
      if (inst.ft) ud.fpr_uses.push_back(*inst.ft);
      if (inst.fd) ud.fpr_defs.push_back(*inst.fd);
      break;

    // FPU 2-register unary and conversion ops
    case Opcode::kSqrtS:
    case Opcode::kAbsS:
    case Opcode::kMovS:
    case Opcode::kNegS:
    case Opcode::kSqrtD:
    case Opcode::kAbsD:
    case Opcode::kMovD:
    case Opcode::kNegD:
    case Opcode::kCvtSD:
    case Opcode::kCvtDS:
    case Opcode::kCvtSW:
    case Opcode::kCvtDW:
    case Opcode::kCvtWS:
    case Opcode::kCvtWD:
    case Opcode::kCvtLS:
    case Opcode::kCvtLD:
    case Opcode::kCvtSL:
    case Opcode::kCvtDL:
    case Opcode::kTruncWS:
    case Opcode::kTruncWD:
    case Opcode::kRoundWS:
    case Opcode::kRoundWD:
    case Opcode::kCeilWS:
    case Opcode::kCeilWD:
    case Opcode::kFloorWS:
    case Opcode::kFloorWD:
      if (inst.fs) ud.fpr_uses.push_back(*inst.fs);
      if (inst.fd) ud.fpr_defs.push_back(*inst.fd);
      break;

    // FPU Comparisons
    case Opcode::kCFS:
    case Opcode::kCUnS:
    case Opcode::kCEqS:
    case Opcode::kCUeqS:
    case Opcode::kCOltS:
    case Opcode::kCUltS:
    case Opcode::kCOleS:
    case Opcode::kCUleS:
    case Opcode::kCSfS:
    case Opcode::kCNgleS:
    case Opcode::kCSeqS:
    case Opcode::kCNglS:
    case Opcode::kCLtS:
    case Opcode::kCNgeS:
    case Opcode::kCLeS:
    case Opcode::kCNgtS:
    case Opcode::kCFD:
    case Opcode::kCUnD:
    case Opcode::kCEqD:
    case Opcode::kCUeqD:
    case Opcode::kCOltD:
    case Opcode::kCUltD:
    case Opcode::kCOleD:
    case Opcode::kCUleD:
    case Opcode::kCSfD:
    case Opcode::kCNgleD:
    case Opcode::kCSeqD:
    case Opcode::kCNglD:
    case Opcode::kCLtD:
    case Opcode::kCNgeD:
    case Opcode::kCLeD:
    case Opcode::kCNgtD:
      if (inst.fs) ud.fpr_uses.push_back(*inst.fs);
      if (inst.ft) ud.fpr_uses.push_back(*inst.ft);
      break;

    case Opcode::kMfc0:
      add_gpr_def(inst.rt);
      break;

    case Opcode::kMtc0:
      add_gpr_use(inst.rt);
      break;

    default:
      break;
  }

  return ud;
}

RegisterTracker::RegisterTracker() {
  Reset();
}

void RegisterTracker::Reset() {
  gpr_values_.assign(kNumGprs, TrackedValue());
  gpr_values_[RegIdx(Register::kZero)] = TrackedValue::Constant(0);
  gpr_values_[RegIdx(Register::kSp)] = TrackedValue::StackOffset(0);
  fpr_const_bits_.fill(std::nullopt);

  reaching_defs_.assign(kNumGprs, std::nullopt);
  def_to_uses_.clear();
  frame_info_ = StackFrameInfo();
  current_instruction_index_ = 0;
}

void RegisterTracker::Step(const Instruction& inst) {
  size_t inst_idx = current_instruction_index_++;
  RegisterUseDef ud = GetInstructionUseDef(inst);

  // 1. Record uses for all read registers
  for (Register r : ud.gpr_uses) {
    size_t idx = RegIdx(r);
    if (reaching_defs_[idx].has_value() && reaching_defs_[idx]->is_local) {
      def_to_uses_[reaching_defs_[idx]->instruction_index].push_back(inst_idx);
    }
  }

  // 2. Track FPR constant bits and caller-saved registers on calls
  if (inst.IsCall()) {
    ClearCallerSavedFprs();
    ClearCallerSavedGprs();
  } else if (inst.opcode == Opcode::kMtc1 && inst.rt.has_value() && inst.fs.has_value()) {
    if (*inst.rt == Register::kZero) {
      SetFprConstantBits(*inst.fs, 0);
    } else {
      const TrackedValue& tv = gpr_values_[RegIdx(*inst.rt)];
      if (tv.kind == ValueKind::kConstant || tv.kind == ValueKind::kSymbolAddress) {
        SetFprConstantBits(*inst.fs, tv.constant_value);
      } else if (tv.kind == ValueKind::kSymbolHi) {
        SetFprConstantBits(*inst.fs, tv.symbol_hi);
      } else {
        ClearFprConstantBits(*inst.fs);
      }
    }
  } else {
    for (FpRegister f : ud.fpr_defs) {
      size_t d = static_cast<size_t>(f);
      ClearFprConstantBits(d);
      if (d % 2 == 0 && d + 1 < fpr_const_bits_.size()) {
        ClearFprConstantBits(d + 1);
      }
    }
  }

  // 3. Track GPR values produced by instruction
  switch (inst.opcode) {
    case Opcode::kLui:
      if (inst.rt.has_value() && *inst.rt != Register::kZero) {
        uint32_t hi = static_cast<uint32_t>(static_cast<uint16_t>(inst.immediate)) << 16;
        gpr_values_[RegIdx(*inst.rt)] = TrackedValue::SymbolHi(hi, inst_idx);
      }
      break;

    case Opcode::kAddi:
    case Opcode::kAddiu:
    case Opcode::kDaddi:
    case Opcode::kDaddiu:
      if (inst.rt.has_value() && *inst.rt != Register::kZero) {
        size_t dest = RegIdx(*inst.rt);
        if (inst.rs == Register::kZero) {
          gpr_values_[dest] = TrackedValue::Constant(static_cast<uint32_t>(inst.immediate));
        } else if (inst.rs.has_value()) {
          size_t src = RegIdx(*inst.rs);
          const auto& src_val = gpr_values_[src];
          if (src_val.kind == ValueKind::kConstant) {
            gpr_values_[dest] = TrackedValue::Constant(src_val.constant_value + inst.immediate);
          } else if (src_val.kind == ValueKind::kStackOffset) {
            int32_t new_offset = src_val.stack_offset + inst.immediate;
            gpr_values_[dest] = TrackedValue::StackOffset(new_offset);
            if (*inst.rt == Register::kSp && new_offset < 0) {
              frame_info_.frame_size =
                  std::max(frame_info_.frame_size, static_cast<uint32_t>(-new_offset));
            }
          } else if (src_val.kind == ValueKind::kSymbolHi) {
            gpr_values_[dest] = TrackedValue::SymbolAddress(src_val.symbol_hi + inst.immediate);
          } else {
            gpr_values_[dest] = TrackedValue();
          }
        }
      }
      break;

    case Opcode::kAndi:
      if (inst.rt.has_value() && *inst.rt != Register::kZero) {
        size_t dest = RegIdx(*inst.rt);
        uint32_t imm_u16 = inst.UnsignedImmediate();
        if (inst.rs == Register::kZero) {
          gpr_values_[dest] = TrackedValue::Constant(0);
        } else if (inst.rs.has_value()) {
          size_t src = RegIdx(*inst.rs);
          const auto& src_val = gpr_values_[src];
          if (src_val.kind == ValueKind::kConstant) {
            gpr_values_[dest] = TrackedValue::Constant(src_val.constant_value & imm_u16);
          } else {
            gpr_values_[dest] = TrackedValue();
          }
        }
      }
      break;

    case Opcode::kOri:
      if (inst.rt.has_value() && *inst.rt != Register::kZero) {
        size_t dest = RegIdx(*inst.rt);
        uint32_t imm_u16 = inst.UnsignedImmediate();
        if (inst.rs == Register::kZero) {
          gpr_values_[dest] = TrackedValue::Constant(imm_u16);
        } else if (inst.rs.has_value()) {
          size_t src = RegIdx(*inst.rs);
          const auto& src_val = gpr_values_[src];
          if (src_val.kind == ValueKind::kConstant) {
            gpr_values_[dest] = TrackedValue::Constant(src_val.constant_value | imm_u16);
          } else if (src_val.kind == ValueKind::kSymbolHi) {
            gpr_values_[dest] = TrackedValue::SymbolAddress(src_val.symbol_hi | imm_u16);
          } else {
            gpr_values_[dest] = TrackedValue();
          }
        }
      }
      break;

    case Opcode::kXori:
      if (inst.rt.has_value() && *inst.rt != Register::kZero) {
        size_t dest = RegIdx(*inst.rt);
        uint32_t imm_u16 = inst.UnsignedImmediate();
        if (inst.rs == Register::kZero) {
          gpr_values_[dest] = TrackedValue::Constant(imm_u16);
        } else if (inst.rs.has_value()) {
          size_t src = RegIdx(*inst.rs);
          const auto& src_val = gpr_values_[src];
          if (src_val.kind == ValueKind::kConstant) {
            gpr_values_[dest] = TrackedValue::Constant(src_val.constant_value ^ imm_u16);
          } else {
            gpr_values_[dest] = TrackedValue();
          }
        }
      }
      break;

    case Opcode::kAddu:
    case Opcode::kAdd:
    case Opcode::kDaddu:
    case Opcode::kDadd:
      if (inst.rd.has_value() && *inst.rd != Register::kZero) {
        size_t dest = RegIdx(*inst.rd);
        if (inst.rs == Register::kZero && inst.rt.has_value()) {
          gpr_values_[dest] = gpr_values_[RegIdx(*inst.rt)];
        } else if (inst.rt == Register::kZero && inst.rs.has_value()) {
          gpr_values_[dest] = gpr_values_[RegIdx(*inst.rs)];
        } else if (inst.rs.has_value() && inst.rt.has_value()) {
          const auto& v1 = gpr_values_[RegIdx(*inst.rs)];
          const auto& v2 = gpr_values_[RegIdx(*inst.rt)];
          if (v1.kind == ValueKind::kConstant && v2.kind == ValueKind::kConstant) {
            gpr_values_[dest] = TrackedValue::Constant(v1.constant_value + v2.constant_value);
          } else if (v1.kind == ValueKind::kSymbolHi && v2.kind != ValueKind::kSymbolHi &&
                     v2.kind != ValueKind::kConstant && *inst.rt != Register::kZero &&
                     *inst.rt != Register::kSp && *inst.rt != Register::kFp) {
            gpr_values_[dest] =
                TrackedValue::SymbolHiIndex(v1.symbol_hi, *inst.rt, v1.hi_inst_index, inst_idx);
          } else if (v2.kind == ValueKind::kSymbolHi && v1.kind != ValueKind::kSymbolHi &&
                     v1.kind != ValueKind::kConstant && *inst.rs != Register::kZero &&
                     *inst.rs != Register::kSp && *inst.rs != Register::kFp) {
            gpr_values_[dest] =
                TrackedValue::SymbolHiIndex(v2.symbol_hi, *inst.rs, v2.hi_inst_index, inst_idx);
          } else {
            gpr_values_[dest] = TrackedValue();
          }
        }
      }
      break;

    case Opcode::kSw:
    case Opcode::kSd:
      if (inst.rs == Register::kSp) {
        int32_t offset = inst.immediate;
        if (inst.rt == Register::kRa) {
          frame_info_.saved_ra_offset = offset;
        } else if (inst.rt.has_value()) {
          frame_info_.saved_gpr_offsets[*inst.rt] = offset;
        }
      }
      break;

    case Opcode::kJal:
    case Opcode::kJalr:
    case Opcode::kBltzal:
    case Opcode::kBgezal:
    case Opcode::kBltzall:
    case Opcode::kBgezall:
      frame_info_.is_leaf = false;
      ClearCallerSavedGprs();
      break;

    default:
      // Any other instruction defining a GPR resets its value to unknown
      for (Register r : ud.gpr_defs) {
        gpr_values_[RegIdx(r)] = TrackedValue();
      }
      break;
  }

  // $zero is always 0
  gpr_values_[RegIdx(Register::kZero)] = TrackedValue::Constant(0);

  // 4. Update reaching definitions
  for (Register r : ud.gpr_defs) {
    size_t idx = RegIdx(r);
    reaching_defs_[idx] = Definition{inst.vram, inst_idx, r, inst};
  }
}

void RegisterTracker::Analyze(absl::Span<const Instruction> instructions) {
  for (const auto& inst : instructions) {
    Step(inst);
  }
}

TrackedValue RegisterTracker::GetRegisterValue(Register reg) const {
  return gpr_values_[RegIdx(reg)];
}

void RegisterTracker::SetRegisterValue(Register reg, const TrackedValue& value) {
  if (reg != Register::kZero) {
    gpr_values_[RegIdx(reg)] = value;
  }
}

bool RegisterTracker::IsConstant(Register reg) const {
  const auto& val = gpr_values_[RegIdx(reg)];
  return val.kind == ValueKind::kConstant || val.kind == ValueKind::kSymbolAddress;
}

std::optional<uint32_t> RegisterTracker::GetConstant(Register reg) const {
  const auto& val = gpr_values_[RegIdx(reg)];
  if (val.kind == ValueKind::kConstant || val.kind == ValueKind::kSymbolAddress) {
    return val.constant_value;
  }
  return std::nullopt;
}

void RegisterTracker::SetFprConstantBits(FpRegister reg, uint32_t bits) {
  SetFprConstantBits(static_cast<size_t>(reg), bits);
}

void RegisterTracker::SetFprConstantBits(size_t reg_idx, uint32_t bits) {
  if (reg_idx < fpr_const_bits_.size()) {
    fpr_const_bits_[reg_idx] = bits;
  }
}

std::optional<uint32_t> RegisterTracker::GetFprConstantBits(FpRegister reg) const {
  return GetFprConstantBits(static_cast<size_t>(reg));
}

std::optional<uint32_t> RegisterTracker::GetFprConstantBits(size_t reg_idx) const {
  if (reg_idx < fpr_const_bits_.size()) {
    return fpr_const_bits_[reg_idx];
  }
  return std::nullopt;
}

void RegisterTracker::ClearFprConstantBits(FpRegister reg) {
  ClearFprConstantBits(static_cast<size_t>(reg));
}

void RegisterTracker::ClearFprConstantBits(size_t reg_idx) {
  if (reg_idx < fpr_const_bits_.size()) {
    fpr_const_bits_[reg_idx] = std::nullopt;
  }
}

void RegisterTracker::ClearCallerSavedFprs() {
  for (size_t f = 0; f <= 19 && f < fpr_const_bits_.size(); ++f) {
    fpr_const_bits_[f] = std::nullopt;
  }
}

void RegisterTracker::ClearCallerSavedGprs() {
  Invalidate(Register::kAt);
  Invalidate(Register::kV0);
  Invalidate(Register::kV1);
  Invalidate(Register::kA0);
  Invalidate(Register::kA1);
  Invalidate(Register::kA2);
  Invalidate(Register::kA3);
  for (size_t t = RegIdx(Register::kT0); t <= RegIdx(Register::kT7); ++t) {
    gpr_values_[t] = TrackedValue();
    reaching_defs_[t] = std::nullopt;
  }
  Invalidate(Register::kT8);
  Invalidate(Register::kT9);
  Invalidate(Register::kK0);
  Invalidate(Register::kK1);
}

void RegisterTracker::PrepareForBlock() {
  current_instruction_index_ = 0;
  def_to_uses_.clear();
  for (auto& def : reaching_defs_) {
    if (def.has_value()) {
      if (def->vram == 0) {
        def->is_local = false;
      } else {
        def = std::nullopt;
      }
    }
  }
}

void RegisterTracker::SetEntryParameter(Register reg) {
  size_t idx = RegIdx(reg);
  if (idx < reaching_defs_.size()) {
    reaching_defs_[idx] = Definition{0, 0, reg, std::nullopt, true};
  }
}

bool RegisterTracker::Merge(const RegisterTracker& other) {
  bool changed = false;
  for (size_t r = 0; r < kNumGprs; ++r) {
    if (r == RegIdx(Register::kZero)) {
      continue;
    }
    if (gpr_values_[r] != other.gpr_values_[r]) {
      if (gpr_values_[r].kind != ValueKind::kUnknown) {
        gpr_values_[r] = TrackedValue();
        changed = true;
      }
    }
    bool same_def =
        reaching_defs_[r].has_value() && other.reaching_defs_[r].has_value() &&
        reaching_defs_[r]->instruction_index == other.reaching_defs_[r]->instruction_index &&
        reaching_defs_[r]->vram == other.reaching_defs_[r]->vram &&
        reaching_defs_[r]->reg == other.reaching_defs_[r]->reg;
    if (!same_def && reaching_defs_[r].has_value()) {
      reaching_defs_[r] = std::nullopt;
      changed = true;
    }
  }
  for (size_t f = 0; f < fpr_const_bits_.size(); ++f) {
    if (fpr_const_bits_[f] != other.fpr_const_bits_[f]) {
      if (fpr_const_bits_[f].has_value()) {
        fpr_const_bits_[f] = std::nullopt;
        changed = true;
      }
    }
  }
  return changed;
}

std::optional<Definition> RegisterTracker::GetReachingDefinition(Register reg) const {
  return reaching_defs_[RegIdx(reg)];
}

const std::vector<size_t>& RegisterTracker::GetUses(size_t def_instruction_index) const {
  auto it = def_to_uses_.find(def_instruction_index);
  if (it != def_to_uses_.end()) {
    return it->second;
  }
  return EmptyVector();
}

void RegisterTracker::Invalidate(Register reg) {
  size_t idx = RegIdx(reg);
  if (idx < gpr_values_.size()) {
    gpr_values_[idx] = TrackedValue();
    reaching_defs_[idx] = std::nullopt;
  }
}

bool DetermineReturnsV0(const ControlFlowGraph& cfg, const SymbolIndex* symbol_index,
                        const absl::flat_hash_set<std::string>* functions_returning_v0) {
  CfgReachabilityGraph graph = BuildCfgReachabilityGraph(cfg);
  if (!graph.has_reachable_return) {
    return false;
  }
  const auto& id_to_index = graph.id_to_index;
  const auto& reachable = graph.reachable;
  const auto& predecessors = graph.predecessors;

  auto uninit_callee_saved = DetectUninitializedCalleeSavedRegisters(cfg);

  auto eval_seq = [&](absl::Span<const Instruction> instructions) -> int {
    int last_def = -1;
    int last_call = -1;
    bool used_before_def = false;
    bool non_branch_used_after_last_def = false;
    bool branch_used_after_last_def = false;
    bool has_return_in_seq = false;
    for (const auto& inst : instructions) {
      if (inst.opcode == Opcode::kJr && inst.rs == Register::kRa) {
        has_return_in_seq = true;
        break;
      }
    }
    for (size_t i = 0; i < instructions.size(); ++i) {
      if (instructions[i].opcode == Opcode::kJal || instructions[i].opcode == Opcode::kJalr) {
        bool callee_returns_v0 = false;
        if (instructions[i].opcode == Opcode::kJal) {
          uint32_t target_vram = instructions[i].JumpTarget();
          std::string callee_name;
          if (symbol_index != nullptr) {
            const auto* sym = symbol_index->FindByAddress(target_vram);
            if (sym != nullptr) {
              callee_name = sym->name();
            }
          }
          if (callee_name.empty()) {
            callee_name = absl::StrFormat("func_%08X", target_vram);
          }
          if (functions_returning_v0 != nullptr && functions_returning_v0->contains(callee_name)) {
            callee_returns_v0 = true;
          }
        }
        if (callee_returns_v0) {
          last_def = static_cast<int>(i);
          last_call = -1;
          non_branch_used_after_last_def = false;
          branch_used_after_last_def = false;
          continue;
        }
        last_call = static_cast<int>(i);
        last_def = -1;
        non_branch_used_after_last_def = false;
        branch_used_after_last_def = false;
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
      for (Register r : ud.gpr_uses) {
        if (r == Register::kV0) {
          if (last_def != -1) {
            if (instructions[i].IsBranch()) {
              branch_used_after_last_def = true;
            } else if (instructions[i].opcode == Opcode::kSlt ||
                       instructions[i].opcode == Opcode::kSlti ||
                       instructions[i].opcode == Opcode::kSltu ||
                       instructions[i].opcode == Opcode::kSltiu) {
              // Comparison using $v0 to evaluate a condition before a branch does not consume/kill
              // $v0.
            } else {
              bool preserved_for_return_while_computing_store = false;
              if (has_return_in_seq && instructions[last_def].IsLoad() &&
                  !instructions[i].IsStore() && !instructions[i].IsCall() &&
                  instructions[i].opcode != Opcode::kMtc1 &&
                  instructions[i].opcode != Opcode::kDmtc1 &&
                  instructions[i].opcode != Opcode::kMult &&
                  instructions[i].opcode != Opcode::kMultu &&
                  instructions[i].opcode != Opcode::kDiv &&
                  instructions[i].opcode != Opcode::kDivu && ud.gpr_defs.size() == 1 &&
                  ud.gpr_defs[0] != Register::kV0) {
                Register other_reg = ud.gpr_defs[0];
                bool other_stored_to_same_mem = false;
                bool v0_overwritten_later = false;
                for (size_t j = i + 1; j < instructions.size(); ++j) {
                  RegisterUseDef later_ud = GetInstructionUseDef(instructions[j]);
                  for (Register d : later_ud.gpr_defs) {
                    if (d == Register::kV0) {
                      v0_overwritten_later = true;
                    }
                  }
                  if (instructions[j].IsStore() && instructions[j].rt == other_reg &&
                      instructions[j].rs == instructions[last_def].rs &&
                      instructions[j].immediate == instructions[last_def].immediate) {
                    other_stored_to_same_mem = true;
                  }
                }
                if (other_stored_to_same_mem && !v0_overwritten_later) {
                  preserved_for_return_while_computing_store = true;
                }
              }
              if (!preserved_for_return_while_computing_store) {
                non_branch_used_after_last_def = true;
              }
            }
          } else {
            used_before_def = true;
          }
          break;
        }
      }
      for (Register r : ud.gpr_defs) {
        if (r == Register::kV0) {
          last_def = static_cast<int>(i);
          non_branch_used_after_last_def = false;
          branch_used_after_last_def = false;
          break;
        }
      }
    }
    if (last_def != -1) {
      if (non_branch_used_after_last_def) {
        return -1;
      }
      RegisterUseDef def_ud = GetInstructionUseDef(instructions[last_def]);
      for (Register u : def_ud.gpr_uses) {
        if (uninit_callee_saved.contains(u)) {
          return -1;
        }
      }
      Opcode op = instructions[last_def].opcode;
      bool is_comparison = (op == Opcode::kSlt || op == Opcode::kSlti || op == Opcode::kSltu ||
                            op == Opcode::kSltiu);
      if (branch_used_after_last_def) {
        return -1;
      }
      if (instructions.size() > 1 && instructions[instructions.size() - 2].IsBranch() &&
          is_comparison) {
        return -1;
      }
      return 1;
    }
    if (last_call != -1 || used_before_def) {
      return -1;
    }
    return 0;
  };

  std::vector<bool> in_avail(cfg.Blocks().size(), false);
  std::vector<bool> out_avail(cfg.Blocks().size(), false);
  in_avail[0] = false;
  int entry_effect = eval_seq(cfg.Blocks()[0].instructions);
  out_avail[0] = (entry_effect == 1);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
      if (!reachable[b]) continue;

      bool new_in = false;
      if (b == 0) {
        new_in = false;
      } else if (!predecessors[b].empty()) {
        for (size_t p : predecessors[b]) {
          const auto& p_block = cfg.Blocks()[p];
          const auto* term = p_block.Terminator();
          absl::Span<const Instruction> executed = p_block.instructions;
          bool is_likely_fallthrough = false;
          if (term != nullptr && term->IsBranchLikely()) {
            for (const auto& edge : p_block.outgoing_edges) {
              if (edge.to_block_id == cfg.Blocks()[b].id && edge.type == EdgeType::kFallthrough) {
                is_likely_fallthrough = true;
                break;
              }
            }
          }
          if (is_likely_fallthrough && executed.size() > 1) {
            executed = executed.subspan(0, executed.size() - 1);
          }

          if (ShouldStripDelaySlotV0DefOnEdge(cfg, p_block, cfg.Blocks()[b].id, executed,
                                              id_to_index)) {
            executed = executed.subspan(0, executed.size() - 1);
          }

          int effect = eval_seq(executed);
          bool edge_avail = false;
          if (effect == 1) {
            edge_avail = true;
          } else if (effect == -1) {
            edge_avail = false;
          } else {
            edge_avail = in_avail[p];
          }

          if (edge_avail) {
            new_in = true;
            break;
          }
        }
      }

      if (new_in != in_avail[b]) {
        in_avail[b] = new_in;
        changed = true;
      }

      int blk_effect = eval_seq(cfg.Blocks()[b].instructions);
      bool new_out = false;
      if (blk_effect == 1) {
        new_out = true;
      } else if (blk_effect == -1) {
        new_out = false;
      } else {
        new_out = in_avail[b];
      }

      if (new_out != out_avail[b]) {
        out_avail[b] = new_out;
        changed = true;
      }
    }
  }

  const bool has_direct_v0_return =
      (functions_returning_v0 == nullptr || functions_returning_v0->empty())
          ? true
          : DetermineReturnsV0(cfg, nullptr, nullptr);

  auto eval_pred_edge_avail = [&](size_t p, size_t b) -> bool {
    const auto& p_block = cfg.Blocks()[p];
    const auto* term = p_block.Terminator();
    absl::Span<const Instruction> executed = p_block.instructions;
    bool is_likely_fallthrough = false;
    if (term != nullptr && term->IsBranchLikely()) {
      for (const auto& edge : p_block.outgoing_edges) {
        if (edge.to_block_id == cfg.Blocks()[b].id && edge.type == EdgeType::kFallthrough) {
          is_likely_fallthrough = true;
          break;
        }
      }
    }
    if (is_likely_fallthrough && executed.size() > 1) {
      executed = executed.subspan(0, executed.size() - 1);
    }
    if (ShouldStripDelaySlotV0DefOnEdge(cfg, p_block, cfg.Blocks()[b].id, executed, id_to_index)) {
      executed = executed.subspan(0, executed.size() - 1);
    }
    int effect = eval_seq(executed);
    if (effect == 1) return true;
    if (effect == -1) return false;
    return in_avail[p];
  };

  bool any_return_avail = false;
  for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
    if (!reachable[b]) continue;
    const auto& block = cfg.Blocks()[b];
    if (!block.HasReturn()) continue;

    int effect = eval_seq(block.instructions);
    bool return_avail = false;
    if (effect == 1) {
      return_avail = true;
    } else if (effect == -1) {
      return_avail = false;
    } else {
      return_avail = in_avail[b];
    }

    if (return_avail && !has_direct_v0_return && effect == 0 && !predecessors[b].empty()) {
      for (size_t p : predecessors[b]) {
        if (eval_pred_edge_avail(p, b)) continue;
        bool pred_has_unconsumed_call = false;
        for (const auto& inst : cfg.Blocks()[p].instructions) {
          if (inst.IsCall()) {
            bool call_may_return_v0 = (functions_returning_v0 == nullptr);
            if (!call_may_return_v0 && inst.opcode == Opcode::kJal) {
              uint32_t target_vram = inst.JumpTarget();
              std::string callee_name;
              if (symbol_index != nullptr) {
                const auto* sym = symbol_index->FindByAddress(target_vram);
                if (sym != nullptr) {
                  callee_name = sym->name();
                }
              }
              if (callee_name.empty()) {
                callee_name = absl::StrFormat("func_%08X", target_vram);
              }
              call_may_return_v0 = functions_returning_v0->contains(callee_name);
            } else if (!call_may_return_v0 && inst.opcode == Opcode::kJalr) {
              call_may_return_v0 = true;
            }
            pred_has_unconsumed_call = call_may_return_v0;
            continue;
          }
          if (pred_has_unconsumed_call) {
            RegisterUseDef ud = GetInstructionUseDef(inst);
            for (Register u : ud.gpr_uses) {
              if (u == Register::kV0) {
                pred_has_unconsumed_call = false;
                break;
              }
            }
          }
        }
        if (!pred_has_unconsumed_call) {
          return_avail = false;
          break;
        }
      }
    }

    if (return_avail) {
      any_return_avail = true;
    }
  }

  return any_return_avail;
}

bool DetermineReturnsF0(const ControlFlowGraph& cfg) {
  CfgReachabilityGraph graph = BuildCfgReachabilityGraph(cfg);
  if (!graph.has_reachable_return) {
    return false;
  }
  const auto& reachable = graph.reachable;
  const auto& predecessors = graph.predecessors;

  auto eval_seq = [](absl::Span<const Instruction> instructions) -> int {
    int last_def = -1;
    int last_call = -1;
    bool used_after_last_def = false;
    bool used_non_compare_after_last_def = false;
    bool any_f0_use = false;
    bool has_return_in_seq = false;
    for (const auto& inst : instructions) {
      if (inst.opcode == Opcode::kJr && inst.rs == Register::kRa) {
        has_return_in_seq = true;
        break;
      }
    }
    for (size_t i = 0; i < instructions.size(); ++i) {
      if (instructions[i].opcode == Opcode::kJal || instructions[i].opcode == Opcode::kJalr) {
        last_call = static_cast<int>(i);
        last_def = -1;
        used_after_last_def = false;
        used_non_compare_after_last_def = false;
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
      const bool is_cop1_compare =
          instructions[i].opcode >= Opcode::kCFS && instructions[i].opcode <= Opcode::kCNgtD;
      for (FpRegister r : ud.fpr_uses) {
        if (r == FpRegister::kF0) {
          any_f0_use = true;
          if (last_def != -1) {
            used_after_last_def = true;
            if (!is_cop1_compare) {
              used_non_compare_after_last_def = true;
            }
          }
          break;
        }
      }
      for (FpRegister r : ud.fpr_defs) {
        if (r == FpRegister::kF0) {
          last_def = static_cast<int>(i);
          used_after_last_def = false;
          used_non_compare_after_last_def = false;
          break;
        }
      }
      if (has_return_in_seq && last_def != -1 &&
          (instructions[i].opcode == Opcode::kSwc1 || instructions[i].opcode == Opcode::kSdc1) &&
          instructions[i].ft.has_value() && *instructions[i].ft != FpRegister::kF0) {
        used_after_last_def = true;
        used_non_compare_after_last_def = true;
      }
    }
    if (last_def != -1) {
      if (used_after_last_def) {
        return used_non_compare_after_last_def ? -1 : 2;
      }
      return 1;
    }
    if (last_call != -1 || any_f0_use) {
      return -1;
    }
    return 0;
  };

  std::vector<bool> in_avail(cfg.Blocks().size(), true);
  std::vector<bool> out_avail(cfg.Blocks().size(), true);
  in_avail[0] = false;
  int entry_effect = eval_seq(cfg.Blocks()[0].instructions);
  out_avail[0] = (entry_effect == 1);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
      if (!reachable[b]) continue;

      bool new_in = false;
      if (b == 0) {
        new_in = false;
      } else if (!predecessors[b].empty()) {
        new_in = true;
        for (size_t p : predecessors[b]) {
          const auto& p_block = cfg.Blocks()[p];
          const auto* term = p_block.Terminator();
          absl::Span<const Instruction> executed = p_block.instructions;
          bool is_likely_fallthrough = false;
          if (term != nullptr && term->IsBranchLikely()) {
            for (const auto& edge : p_block.outgoing_edges) {
              if (edge.to_block_id == cfg.Blocks()[b].id && edge.type == EdgeType::kFallthrough) {
                is_likely_fallthrough = true;
                break;
              }
            }
          }
          if (is_likely_fallthrough && executed.size() > 1) {
            executed = executed.subspan(0, executed.size() - 1);
          }

          int effect = eval_seq(executed);
          bool edge_avail = false;
          if (effect == 1) {
            edge_avail = true;
          } else if (effect == 2) {
            bool sibling_has_unconsumed_f0_def = false;
            for (size_t other_p : predecessors[b]) {
              if (other_p == p) continue;
              if (eval_seq(cfg.Blocks()[other_p].instructions) == 1) {
                sibling_has_unconsumed_f0_def = true;
                break;
              }
            }
            edge_avail = sibling_has_unconsumed_f0_def;
          } else if (effect == -1) {
            edge_avail = false;
          } else {
            edge_avail = in_avail[p];
          }

          if (!edge_avail) {
            new_in = false;
            break;
          }
        }
      }

      if (new_in != in_avail[b]) {
        in_avail[b] = new_in;
        changed = true;
      }

      int blk_effect = eval_seq(cfg.Blocks()[b].instructions);
      bool new_out = false;
      if (blk_effect == 1) {
        new_out = true;
      } else if (blk_effect == -1) {
        new_out = false;
      } else {
        new_out = in_avail[b];
      }

      if (new_out != out_avail[b]) {
        out_avail[b] = new_out;
        changed = true;
      }
    }
  }

  for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
    if (!reachable[b]) continue;
    const auto& block = cfg.Blocks()[b];
    if (!block.HasReturn()) continue;

    int effect = eval_seq(block.instructions);
    bool return_avail = false;
    if (effect == 1) {
      return_avail = true;
    } else if (effect == -1) {
      return_avail = false;
    } else {
      return_avail = in_avail[b];
    }

    if (!return_avail) {
      return false;
    }

    int last_def_v0 = -1;
    bool v0_used_after = false;
    int last_def_f0 = -1;
    bool f0_used_after = false;
    for (size_t i = 0; i < block.instructions.size(); ++i) {
      if (block.instructions[i].opcode == Opcode::kJal ||
          block.instructions[i].opcode == Opcode::kJalr) {
        last_def_v0 = -1;
        v0_used_after = false;
        last_def_f0 = -1;
        f0_used_after = false;
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(block.instructions[i]);
      for (Register r : ud.gpr_uses) {
        if (r == Register::kV0 && last_def_v0 != -1) v0_used_after = true;
      }
      for (Register r : ud.gpr_defs) {
        if (r == Register::kV0) {
          last_def_v0 = static_cast<int>(i);
          v0_used_after = false;
        }
      }
      for (FpRegister r : ud.fpr_uses) {
        if (r == FpRegister::kF0 && last_def_f0 != -1) f0_used_after = true;
      }
      for (FpRegister r : ud.fpr_defs) {
        if (r == FpRegister::kF0) {
          last_def_f0 = static_cast<int>(i);
          f0_used_after = false;
        }
      }
    }
    bool has_live_v0 = (last_def_v0 != -1 && !v0_used_after);
    bool has_live_f0 = (last_def_f0 != -1 && !f0_used_after);
    if (has_live_v0 && (!has_live_f0 || last_def_v0 > last_def_f0)) {
      return false;
    }
  }

  return true;
}

bool DetermineReturnsF0Double(const ControlFlowGraph& cfg) {
  for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
    const auto& block = cfg.Blocks()[b];
    if (!block.HasReturn()) continue;

    std::vector<size_t> queue = {b};
    absl::flat_hash_set<size_t> visited = {b};

    while (!queue.empty()) {
      size_t curr = queue.back();
      queue.pop_back();
      const auto& curr_block = cfg.Blocks()[curr];

      bool defined_in_block = false;
      for (int i = static_cast<int>(curr_block.instructions.size()) - 1; i >= 0; --i) {
        RegisterUseDef ud = GetInstructionUseDef(curr_block.instructions[i]);
        for (FpRegister r : ud.fpr_defs) {
          if (r == FpRegister::kF0) {
            defined_in_block = true;
            Opcode op = curr_block.instructions[i].opcode;
            if (IsDoublePrecisionWriteOp(op)) {
              return true;
            }
            break;
          }
        }
        if (defined_in_block) break;
      }

      if (!defined_in_block) {
        uint32_t curr_id = curr_block.id;
        for (size_t p = 0; p < cfg.Blocks().size(); ++p) {
          for (uint32_t succ : cfg.Blocks()[p].successors) {
            if (succ == curr_id && visited.insert(p).second) {
              queue.push_back(p);
            }
          }
        }
      }
    }
  }
  return false;
}

absl::flat_hash_set<Register> DetectUninitializedCalleeSavedRegisters(const ControlFlowGraph& cfg) {
  absl::flat_hash_set<Register> uninit;
  if (cfg.Blocks().empty()) return uninit;

  auto is_callee_saved = [](Register r) {
    int idx = static_cast<int>(r);
    return (idx >= 16 && idx <= 23) || idx == 30;
  };

  uint32_t init_def_mask = (1u << 0) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 28) |
                           (1u << 29) | (1u << 31);

  CfgReachabilityGraph graph =
      BuildCfgReachabilityGraph(cfg, /*reachable_only_predecessors=*/false);
  const auto& predecessors = graph.predecessors;

  std::vector<uint32_t> in_defs(cfg.Blocks().size(), 0);
  std::vector<uint32_t> out_defs(cfg.Blocks().size(), 0xFFFFFFFF);
  in_defs[0] = init_def_mask;

  auto is_stack_spill = [](const Instruction& inst, Register r) {
    if (inst.opcode != Opcode::kSw && inst.opcode != Opcode::kSd) return false;
    return (inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.rt == r;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
      uint32_t curr_in = 0;
      if (b == 0) {
        curr_in = init_def_mask;
      } else if (!predecessors[b].empty()) {
        curr_in = 0xFFFFFFFF;
        for (size_t p : predecessors[b]) {
          curr_in &= out_defs[p];
        }
      }
      if (curr_in != in_defs[b]) {
        in_defs[b] = curr_in;
        changed = true;
      }

      uint32_t defs = in_defs[b];
      for (const auto& inst : cfg.Blocks()[b].instructions) {
        RegisterUseDef ud = GetInstructionUseDef(inst);
        for (Register d : ud.gpr_defs) {
          defs |= (1u << static_cast<int>(d));
        }
      }

      if (defs != out_defs[b]) {
        out_defs[b] = defs;
        changed = true;
      }
    }
  }

  for (size_t b = 0; b < cfg.Blocks().size(); ++b) {
    uint32_t defs = in_defs[b];
    for (const auto& inst : cfg.Blocks()[b].instructions) {
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register u : ud.gpr_uses) {
        int u_idx = static_cast<int>(u);
        if (is_callee_saved(u) && !is_stack_spill(inst, u)) {
          if ((defs & (1u << u_idx)) == 0) {
            uninit.insert(u);
          }
        }
      }
      for (Register d : ud.gpr_defs) {
        defs |= (1u << static_cast<int>(d));
      }
    }
  }

  return uninit;
}

namespace {

constexpr uint32_t kCanonicalNop = 0x10000u;
constexpr uint32_t kCanonicalMove = 0x10001u;

uint32_t CanonicalizeInstructionOp(const Instruction& inst) {
  if (inst.IsNop()) {
    return kCanonicalNop;
  }
  if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kOr ||
       inst.opcode == Opcode::kDaddu) &&
      (inst.rt == Register::kZero || inst.rs == Register::kZero)) {
    return kCanonicalMove;
  }
  if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kOri) &&
      inst.rs == Register::kZero && inst.immediate == 0) {
    return kCanonicalMove;
  }
  if (inst.opcode == Opcode::kAddi) {
    return static_cast<uint32_t>(Opcode::kAddiu);
  }
  return static_cast<uint32_t>(inst.opcode);
}

size_t ComputeCanonicalSequenceLcs(absl::Span<const uint32_t> target_ops,
                                   absl::Span<const uint32_t> candidate_ops) {
  const size_t target_len = target_ops.size();
  const size_t cand_len = candidate_ops.size();
  if (target_len == 0 || cand_len == 0) {
    return 0;
  }

  size_t prefix_len = 0;
  const size_t min_len = std::min(target_len, cand_len);
  while (prefix_len < min_len && target_ops[prefix_len] == candidate_ops[prefix_len]) {
    ++prefix_len;
  }
  if (prefix_len == target_len || prefix_len == cand_len) {
    return prefix_len;
  }

  size_t suffix_len = 0;
  while (suffix_len < (target_len - prefix_len) && suffix_len < (cand_len - prefix_len) &&
         target_ops[target_len - 1 - suffix_len] == candidate_ops[cand_len - 1 - suffix_len]) {
    ++suffix_len;
  }

  absl::Span<const uint32_t> mid_target =
      target_ops.subspan(prefix_len, target_len - prefix_len - suffix_len);
  absl::Span<const uint32_t> mid_cand =
      candidate_ops.subspan(prefix_len, cand_len - prefix_len - suffix_len);

  absl::flat_hash_map<uint32_t, std::vector<int>> target_positions;
  target_positions.reserve(mid_target.size());
  for (int i = 0; i < static_cast<int>(mid_target.size()); ++i) {
    target_positions[mid_target[i]].push_back(i);
  }

  std::vector<int> lis;
  lis.reserve(std::min(mid_target.size(), mid_cand.size()));
  for (uint32_t op : mid_cand) {
    auto it = target_positions.find(op);
    if (it == target_positions.end()) {
      continue;
    }
    const std::vector<int>& positions = it->second;
    for (auto r_it = positions.rbegin(); r_it != positions.rend(); ++r_it) {
      int pos = *r_it;
      auto lb = std::lower_bound(lis.begin(), lis.end(), pos);
      if (lb == lis.end()) {
        lis.push_back(pos);
      } else {
        *lb = pos;
      }
    }
  }

  return prefix_len + lis.size() + suffix_len;
}

}  // namespace

absl::Span<const uint32_t> TrimTrailingSectionAlignmentNops(absl::Span<const uint32_t> words,
                                                            uint32_t base_vram) {
  size_t trimmed_len = words.size();
  while (trimmed_len > 2 && words[trimmed_len - 1] == 0 && words[trimmed_len - 2] == 0) {
    --trimmed_len;
  }
  if (trimmed_len > 1 && words[trimmed_len - 1] == 0) {
    auto prev_or = DecodeInstruction(words[trimmed_len - 2],
                                     base_vram + static_cast<uint32_t>((trimmed_len - 2) * 4));
    if (prev_or.ok() && !prev_or->IsBranch() && !prev_or->IsJump() && !prev_or->IsCall() &&
        !prev_or->IsReturn()) {
      --trimmed_len;
    }
  }
  return words.first(trimmed_len);
}

FunctionStructuralProfile AnalyzeFunctionStructure(absl::Span<const uint32_t> words,
                                                   uint32_t base_vram) {
  FunctionStructuralProfile profile;
  if (words.empty()) {
    return profile;
  }

  absl::Span<const uint32_t> active_words = TrimTrailingSectionAlignmentNops(words, base_vram);
  auto insts_or = DecodeSequence(active_words, base_vram);
  if (!insts_or.ok() || insts_or->empty()) {
    return profile;
  }
  const auto& instructions = *insts_or;
  profile.instruction_count = instructions.size();

  // 1. Prologue stack frame size & callee-saved register analysis
  bool found_frame_alloc = false;
  uint32_t clobbered_gprs = 0;
  uint32_t clobbered_fprs = 0;
  size_t prologue_limit = instructions.size();
  for (size_t i = 0; i < instructions.size(); ++i) {
    const Instruction& inst = instructions[i];
    if (inst.IsBranch() || inst.IsJump() || inst.IsCall() || inst.IsReturn()) {
      prologue_limit = std::min(instructions.size(), i + 2);
      break;
    }
  }

  for (size_t i = 0; i < prologue_limit; ++i) {
    const Instruction& inst = instructions[i];
    if (!found_frame_alloc &&
        (inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
         inst.opcode == Opcode::kDaddiu || inst.opcode == Opcode::kDaddi) &&
        inst.rt == Register::kSp && inst.rs == Register::kSp && inst.immediate < 0) {
      profile.frame_size = static_cast<uint32_t>(-static_cast<int32_t>(inst.immediate));
      found_frame_alloc = true;
    }

    if ((inst.opcode == Opcode::kSw || inst.opcode == Opcode::kSd) && inst.rs == Register::kSp &&
        inst.rt.has_value() && inst.immediate >= 0) {
      uint32_t reg_idx = static_cast<uint32_t>(*inst.rt);
      bool is_callee_saved_gpr = (reg_idx >= 16 && reg_idx <= 23) || reg_idx == 30 || reg_idx == 31;
      if (is_callee_saved_gpr && (clobbered_gprs & (1u << reg_idx)) == 0) {
        profile.saved_gpr_mask |= (1u << reg_idx);
      }
    } else if (inst.opcode == Opcode::kSwc1 && inst.rs == Register::kSp && inst.ft.has_value() &&
               inst.immediate >= 0) {
      uint32_t fpr_idx = static_cast<uint32_t>(*inst.ft);
      if (fpr_idx >= 20 && fpr_idx <= 31 && (clobbered_fprs & (1u << fpr_idx)) == 0) {
        profile.saved_fpr_mask |= (1u << fpr_idx);
      }
    } else if (inst.opcode == Opcode::kSdc1 && inst.rs == Register::kSp && inst.ft.has_value() &&
               inst.immediate >= 0) {
      uint32_t fpr_idx = static_cast<uint32_t>(*inst.ft) & ~1u;
      if (fpr_idx >= 20 && fpr_idx <= 30 && (clobbered_fprs & (3u << fpr_idx)) == 0) {
        profile.saved_fpr_mask |= (3u << fpr_idx);
      }
    }

    RegisterUseDef ud = GetInstructionUseDef(inst);
    for (Register d : ud.gpr_defs) {
      clobbered_gprs |= (1u << static_cast<uint32_t>(d));
    }
    for (FpRegister fd : ud.fpr_defs) {
      clobbered_fprs |= (1u << static_cast<uint32_t>(fd));
    }
  }

  // 2. Control-flow graph block and branch topology
  auto cfg_or = ControlFlowGraph::Build(instructions);
  profile.basic_block_count = cfg_or.ok() ? cfg_or->Blocks().size() : 1;
  profile.canonical_ops.reserve(instructions.size());
  for (const auto& inst : instructions) {
    if (inst.IsBranch() || inst.opcode == Opcode::kJ ||
        (inst.opcode == Opcode::kJr && inst.rs != Register::kRa)) {
      ++profile.branch_count;
    }
    profile.canonical_ops.push_back(CanonicalizeInstructionOp(inst));
  }

  return profile;
}

FunctionStructuralComparison CompareFunctionStructure(absl::Span<const uint32_t> target_words,
                                                      uint32_t target_vram,
                                                      absl::Span<const uint32_t> candidate_words,
                                                      uint32_t candidate_vram) {
  FunctionStructuralComparison comp;
  comp.target = AnalyzeFunctionStructure(target_words, target_vram);
  comp.candidate = AnalyzeFunctionStructure(candidate_words, candidate_vram);
  comp.frame_size_matches = (comp.target.frame_size == comp.candidate.frame_size);
  comp.saved_registers_match = (comp.target.saved_gpr_mask == comp.candidate.saved_gpr_mask &&
                                comp.target.saved_fpr_mask == comp.candidate.saved_fpr_mask);
  comp.cfg_topology_matches = (comp.target.basic_block_count == comp.candidate.basic_block_count &&
                               comp.target.branch_count == comp.candidate.branch_count);
  comp.normalized_lcs_instructions =
      ComputeCanonicalSequenceLcs(comp.target.canonical_ops, comp.candidate.canonical_ops);
  return comp;
}

}  // namespace rom_nom_nom
