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
    if (reaching_defs_[idx].has_value()) {
      def_to_uses_[reaching_defs_[idx]->instruction_index].push_back(inst_idx);
    }
  }

  // 2. Track values produced by instruction
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
            TrackedValue tv;
            tv.kind = ValueKind::kSymbolAddress;
            tv.constant_value = src_val.symbol_hi + inst.immediate;
            gpr_values_[dest] = tv;
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
            TrackedValue tv;
            tv.kind = ValueKind::kSymbolAddress;
            tv.constant_value = src_val.symbol_hi | imm_u16;
            gpr_values_[dest] = tv;
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
      // Function call clobbers caller-saved registers
      gpr_values_[RegIdx(Register::kV0)] = TrackedValue();
      gpr_values_[RegIdx(Register::kV1)] = TrackedValue();
      gpr_values_[RegIdx(Register::kA0)] = TrackedValue();
      gpr_values_[RegIdx(Register::kA1)] = TrackedValue();
      gpr_values_[RegIdx(Register::kA2)] = TrackedValue();
      gpr_values_[RegIdx(Register::kA3)] = TrackedValue();
      for (size_t t = RegIdx(Register::kT0); t <= RegIdx(Register::kT7); ++t) {
        gpr_values_[t] = TrackedValue();
      }
      gpr_values_[RegIdx(Register::kT8)] = TrackedValue();
      gpr_values_[RegIdx(Register::kT9)] = TrackedValue();
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

  // 3. Update reaching definitions
  for (Register r : ud.gpr_defs) {
    size_t idx = RegIdx(r);
    reaching_defs_[idx] = Definition{inst.vram, inst_idx, r, &inst};
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
  if (cfg.Blocks().empty()) {
    return false;
  }

  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  std::vector<bool> reachable(cfg.Blocks().size(), false);
  std::vector<size_t> reach_worklist = {0};
  reachable[0] = true;
  while (!reach_worklist.empty()) {
    size_t curr = reach_worklist.back();
    reach_worklist.pop_back();
    for (uint32_t succ_id : cfg.Blocks()[curr].successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end() && !reachable[it->second]) {
        reachable[it->second] = true;
        reach_worklist.push_back(it->second);
      }
    }
  }

  bool has_reachable_return = false;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    if (reachable[i] && cfg.Blocks()[i].HasReturn()) {
      has_reachable_return = true;
      break;
    }
  }
  if (!has_reachable_return) {
    return false;
  }

  auto uninit_callee_saved = DetectUninitializedCalleeSavedRegisters(cfg);

  auto eval_seq = [&](absl::Span<const Instruction> instructions) -> int {
    int last_def = -1;
    int last_call = -1;
    bool used_before_def = false;
    bool non_branch_used_after_last_def = false;
    bool branch_used_after_last_def = false;
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
            } else {
              non_branch_used_after_last_def = true;
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

  std::vector<std::vector<size_t>> predecessors(cfg.Blocks().size());
  for (size_t u = 0; u < cfg.Blocks().size(); ++u) {
    if (!reachable[u]) continue;
    for (uint32_t succ_id : cfg.Blocks()[u].successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end()) {
        predecessors[it->second].push_back(u);
      }
    }
  }

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

          bool is_taken_branch = false;
          if (term != nullptr && term->IsBranch()) {
            for (const auto& edge : p_block.outgoing_edges) {
              if (edge.to_block_id == cfg.Blocks()[b].id && edge.type == EdgeType::kBranch) {
                is_taken_branch = true;
                break;
              }
            }
          }
          if (is_taken_branch && executed.size() > 1) {
            RegisterUseDef delay_ud = GetInstructionUseDef(executed.back());
            bool delay_defs_v0 = false;
            for (Register d : delay_ud.gpr_defs) {
              if (d == Register::kV0) delay_defs_v0 = true;
            }
            if (delay_defs_v0) {
              for (const auto& edge : p_block.outgoing_edges) {
                if (edge.type == EdgeType::kFallthrough) {
                  auto ft_it = id_to_index.find(edge.to_block_id);
                  if (ft_it != id_to_index.end()) {
                    const auto& ft_block = cfg.Blocks()[ft_it->second];
                    bool ft_uses_v0 = false;
                    for (const auto& ft_inst : ft_block.instructions) {
                      RegisterUseDef ft_ud = GetInstructionUseDef(ft_inst);
                      for (Register u : ft_ud.gpr_uses) {
                        if (u == Register::kV0) {
                          ft_uses_v0 = true;
                          break;
                        }
                      }
                      if (ft_uses_v0) break;
                      bool ft_redefined_v0 = false;
                      for (Register d : ft_ud.gpr_defs) {
                        if (d == Register::kV0) {
                          ft_redefined_v0 = true;
                          break;
                        }
                      }
                      if (ft_redefined_v0) break;
                    }
                    if (ft_uses_v0) {
                      executed = executed.subspan(0, executed.size() - 1);
                    }
                  }
                  break;
                }
              }
            }
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

    if (return_avail) {
      any_return_avail = true;
    }
  }

  return any_return_avail;
}

bool DetermineReturnsF0(const ControlFlowGraph& cfg) {
  if (cfg.Blocks().empty()) {
    return false;
  }

  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  std::vector<bool> reachable(cfg.Blocks().size(), false);
  std::vector<size_t> reach_worklist = {0};
  reachable[0] = true;
  while (!reach_worklist.empty()) {
    size_t curr = reach_worklist.back();
    reach_worklist.pop_back();
    for (uint32_t succ_id : cfg.Blocks()[curr].successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end() && !reachable[it->second]) {
        reachable[it->second] = true;
        reach_worklist.push_back(it->second);
      }
    }
  }

  bool has_reachable_return = false;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    if (reachable[i] && cfg.Blocks()[i].HasReturn()) {
      has_reachable_return = true;
      break;
    }
  }
  if (!has_reachable_return) {
    return false;
  }

  auto eval_seq = [](absl::Span<const Instruction> instructions) -> int {
    int last_def = -1;
    int last_call = -1;
    bool used_after_last_def = false;
    for (size_t i = 0; i < instructions.size(); ++i) {
      if (instructions[i].opcode == Opcode::kJal || instructions[i].opcode == Opcode::kJalr) {
        last_call = static_cast<int>(i);
        last_def = -1;
        used_after_last_def = false;
        continue;
      }
      RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
      for (FpRegister r : ud.fpr_uses) {
        if (r == FpRegister::kF0) {
          if (last_def != -1) {
            used_after_last_def = true;
          }
          break;
        }
      }
      for (FpRegister r : ud.fpr_defs) {
        if (r == FpRegister::kF0) {
          last_def = static_cast<int>(i);
          used_after_last_def = false;
          break;
        }
      }
    }
    if (last_def != -1) {
      if (used_after_last_def) {
        return -1;
      }
      return 1;
    }
    if (last_call != -1) {
      return -1;
    }
    return 0;
  };

  std::vector<std::vector<size_t>> predecessors(cfg.Blocks().size());
  for (size_t u = 0; u < cfg.Blocks().size(); ++u) {
    if (!reachable[u]) continue;
    for (uint32_t succ_id : cfg.Blocks()[u].successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end()) {
        predecessors[it->second].push_back(u);
      }
    }
  }

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
            if (op == Opcode::kAddD || op == Opcode::kSubD || op == Opcode::kMulD ||
                op == Opcode::kDivD || op == Opcode::kSqrtD || op == Opcode::kAbsD ||
                op == Opcode::kMovD || op == Opcode::kNegD || op == Opcode::kCvtDS ||
                op == Opcode::kCvtDW || op == Opcode::kLdc1 || op == Opcode::kDmfc1) {
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

  absl::flat_hash_map<uint32_t, size_t> id_to_index;
  for (size_t i = 0; i < cfg.Blocks().size(); ++i) {
    id_to_index[cfg.Blocks()[i].id] = i;
  }

  std::vector<std::vector<size_t>> predecessors(cfg.Blocks().size());
  for (size_t u = 0; u < cfg.Blocks().size(); ++u) {
    for (uint32_t succ_id : cfg.Blocks()[u].successors) {
      auto it = id_to_index.find(succ_id);
      if (it != id_to_index.end()) {
        predecessors[it->second].push_back(u);
      }
    }
  }

  std::vector<uint32_t> in_defs(cfg.Blocks().size(), 0);
  std::vector<uint32_t> out_defs(cfg.Blocks().size(), 0);
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

      if (defs != out_defs[b]) {
        out_defs[b] = defs;
        changed = true;
      }
    }
  }

  return uninit;
}

}  // namespace rom_nom_nom
