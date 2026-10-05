#include "lifter/register_tracker.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

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
      add_gpr_use(inst.rt);
      add_gpr_def(inst.rd);
      break;

    // Shift Variable
    case Opcode::kSllv:
    case Opcode::kSrlv:
    case Opcode::kSrav:
      add_gpr_use(inst.rs);
      add_gpr_use(inst.rt);
      add_gpr_def(inst.rd);
      break;

    // Immediate Arithmetic & Logic
    case Opcode::kAddi:
    case Opcode::kAddiu:
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
    case Opcode::kLd:
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
      add_gpr_use(inst.rs);
      add_gpr_use(inst.rt);
      break;

    // Multiply / Divide
    case Opcode::kMult:
    case Opcode::kMultu:
    case Opcode::kDiv:
    case Opcode::kDivu:
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
}

void RegisterTracker::Analyze(absl::Span<const Instruction> instructions) {
  for (size_t i = 0; i < instructions.size(); ++i) {
    const auto& inst = instructions[i];
    RegisterUseDef ud = GetInstructionUseDef(inst);

    // 1. Record uses for all read registers
    for (Register r : ud.gpr_uses) {
      size_t idx = RegIdx(r);
      if (reaching_defs_[idx].has_value()) {
        def_to_uses_[reaching_defs_[idx]->instruction_index].push_back(i);
      }
    }

    // 2. Track values produced by instruction
    switch (inst.opcode) {
      case Opcode::kLui:
        if (inst.rt.has_value() && *inst.rt != Register::kZero) {
          uint32_t hi = static_cast<uint32_t>(static_cast<uint16_t>(inst.immediate)) << 16;
          gpr_values_[RegIdx(*inst.rt)] = TrackedValue::SymbolHi(hi);
        }
        break;

      case Opcode::kAddi:
      case Opcode::kAddiu:
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
              gpr_values_[dest] = TrackedValue::Constant(src_val.symbol_hi + inst.immediate);
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
              gpr_values_[dest] = TrackedValue::Constant(src_val.symbol_hi | imm_u16);
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
            } else {
              gpr_values_[dest] = TrackedValue();
            }
          }
        }
        break;

      case Opcode::kSw:
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
      reaching_defs_[idx] = Definition{inst.vram, i, r, &inst};
    }
  }
}

TrackedValue RegisterTracker::GetRegisterValue(Register reg) const {
  return gpr_values_[RegIdx(reg)];
}

bool RegisterTracker::IsConstant(Register reg) const {
  return gpr_values_[RegIdx(reg)].kind == ValueKind::kConstant;
}

std::optional<uint32_t> RegisterTracker::GetConstant(Register reg) const {
  const auto& val = gpr_values_[RegIdx(reg)];
  if (val.kind == ValueKind::kConstant) {
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

}  // namespace rom_nom_nom
