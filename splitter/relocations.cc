#include "splitter/relocations.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {
namespace {

struct ActiveLui {
  uint32_t pc = 0;
  uint16_t hi_imm = 0;
};

// Returns true if the opcode overwrites GPR rt (and is not lui).
bool WritesToGprRt(Opcode opcode) {
  switch (opcode) {
    case Opcode::kAddi:
    case Opcode::kAddiu:
    case Opcode::kSlti:
    case Opcode::kSltiu:
    case Opcode::kAndi:
    case Opcode::kOri:
    case Opcode::kXori:
    case Opcode::kLb:
    case Opcode::kLbu:
    case Opcode::kLh:
    case Opcode::kLhu:
    case Opcode::kLw:
    case Opcode::kLwl:
    case Opcode::kLwr:
    case Opcode::kLd:
    case Opcode::kLld:
    case Opcode::kMfc1:
    case Opcode::kDmfc1:
      return true;
    default:
      return false;
  }
}

// Returns true if the instruction is a consumer that pairs with lui via register rs.
bool IsLuiConsumer(Opcode opcode) {
  switch (opcode) {
    case Opcode::kAddiu:
    case Opcode::kAddi:
    case Opcode::kOri:
    case Opcode::kLb:
    case Opcode::kLbu:
    case Opcode::kLh:
    case Opcode::kLhu:
    case Opcode::kLw:
    case Opcode::kLwl:
    case Opcode::kLwr:
    case Opcode::kLd:
    case Opcode::kLld:
    case Opcode::kSb:
    case Opcode::kSh:
    case Opcode::kSw:
    case Opcode::kSwl:
    case Opcode::kSwr:
    case Opcode::kSd:
    case Opcode::kScd:
    case Opcode::kLwc1:
    case Opcode::kLdc1:
    case Opcode::kSwc1:
    case Opcode::kSdc1:
      return true;
    default:
      return false;
  }
}

}  // namespace

RelocationTracker::RelocationTracker(const SymbolIndex* symbols, RelocationOptions options)
    : symbols_(symbols), options_(options) {}

bool RelocationTracker::IsValidTarget(uint32_t addr) const {
  if (symbols_ != nullptr && symbols_->HasAddress(addr)) {
    return true;
  }
  if (!options_.allow_unregistered_pointers) {
    return false;
  }
  // Standard N64 KSEG0 and KSEG1 pointer ranges
  return (addr >= 0x80000000 && addr <= 0xBFFFFFFF);
}

void RelocationTracker::ResolveSymbol(uint32_t target_vram, std::string& out_name,
                                      int32_t& out_addend) const {
  if (symbols_ != nullptr) {
    const auto* entry = symbols_->FindByAddress(target_vram);
    if (entry != nullptr) {
      out_name = entry->name();
      out_addend = 0;
      return;
    }

    // Check if target falls inside a symbol with known size
    for (const auto* candidate : symbols_->SortedEntries()) {
      if (candidate->size() > 0 && target_vram >= candidate->address() &&
          target_vram < candidate->address() + candidate->size()) {
        out_name = candidate->name();
        out_addend = static_cast<int32_t>(target_vram - candidate->address());
        return;
      }
    }

    out_name = symbols_->LookupOrSynthesizeName(target_vram, SYMBOL_DATA);
    out_addend = 0;
    return;
  }

  out_name = absl::StrFormat("D_%08X", target_vram);
  out_addend = 0;
}

std::string RelocationTracker::FormatSymbolExpression(std::string_view symbol_name,
                                                      int32_t addend) {
  if (addend == 0) {
    return std::string(symbol_name);
  }
  if (addend > 0) {
    return absl::StrFormat("%s + 0x%X", symbol_name, addend);
  }
  return absl::StrFormat("%s - 0x%X", symbol_name, -addend);
}

void RelocationTracker::Clear() {
  relocations_.clear();
  pc_to_index_.clear();
}

const InstructionRelocation* RelocationTracker::FindRelocation(uint32_t pc) const {
  auto it = pc_to_index_.find(pc);
  if (it == pc_to_index_.end()) {
    return nullptr;
  }
  return &relocations_[it->second];
}

absl::Status RelocationTracker::AnalyzeCode(absl::Span<const uint8_t> code, uint32_t vram_start) {
  std::vector<Instruction> instructions;
  instructions.reserve(code.size() / 4);

  for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
    uint32_t pc = vram_start + static_cast<uint32_t>(offset);
    auto inst_or = DecodeInstruction(code.subspan(offset, 4), pc);
    if (inst_or.ok()) {
      instructions.push_back(*inst_or);
    }
  }

  return Analyze(instructions);
}

absl::Status RelocationTracker::Analyze(absl::Span<const Instruction> instructions) {
  std::array<std::optional<ActiveLui>, 32> active_luis;
  active_luis.fill(std::nullopt);

  bool clear_on_next_instruction = false;

  auto add_reloc = [this](InstructionRelocation reloc) {
    auto it = pc_to_index_.find(reloc.pc);
    if (it != pc_to_index_.end()) {
      relocations_[it->second] = reloc;
    } else {
      pc_to_index_[reloc.pc] = relocations_.size();
      relocations_.push_back(std::move(reloc));
    }
  };

  for (const auto& inst : instructions) {
    if (clear_on_next_instruction) {
      active_luis.fill(std::nullopt);
      clear_on_next_instruction = false;
    }

    // Check if this instruction consumes an active LUI register
    if (inst.rs.has_value() && IsLuiConsumer(inst.opcode)) {
      size_t rs_idx = static_cast<size_t>(*inst.rs);
      if (active_luis[rs_idx].has_value()) {
        const auto& lui = *active_luis[rs_idx];
        uint32_t target_vram = 0;
        if (inst.opcode == Opcode::kOri) {
          target_vram =
              (static_cast<uint32_t>(lui.hi_imm) << 16) | static_cast<uint16_t>(inst.immediate);
        } else {
          target_vram =
              (static_cast<uint32_t>(lui.hi_imm) << 16) + static_cast<int32_t>(inst.immediate);
        }

        if (IsValidTarget(target_vram)) {
          std::string sym_name;
          int32_t addend = 0;
          ResolveSymbol(target_vram, sym_name, addend);

          // Add %hi relocation at LUI instruction
          InstructionRelocation hi_reloc;
          hi_reloc.pc = lui.pc;
          hi_reloc.type = RelocType::kHi16;
          hi_reloc.target_vram = target_vram;
          hi_reloc.symbol_name = sym_name;
          hi_reloc.addend = addend;
          add_reloc(hi_reloc);

          // Add %lo relocation at consumer instruction
          InstructionRelocation lo_reloc;
          lo_reloc.pc = inst.vram;
          lo_reloc.type = RelocType::kLo16;
          lo_reloc.target_vram = target_vram;
          lo_reloc.symbol_name = sym_name;
          lo_reloc.addend = addend;
          add_reloc(lo_reloc);
        }
      }
    }

    // Update active LUI tracking or clobber registers
    if (inst.opcode == Opcode::kLui) {
      if (inst.rt.has_value() && *inst.rt != Register::kZero) {
        size_t rt_idx = static_cast<size_t>(*inst.rt);
        active_luis[rt_idx] = ActiveLui{inst.vram, static_cast<uint16_t>(inst.immediate)};
      }
    } else {
      if (inst.rd.has_value() && *inst.rd != Register::kZero) {
        active_luis[static_cast<size_t>(*inst.rd)] = std::nullopt;
      }
      if (inst.rt.has_value() && WritesToGprRt(inst.opcode) && *inst.rt != Register::kZero) {
        active_luis[static_cast<size_t>(*inst.rt)] = std::nullopt;
      }
    }

    // Schedule reset after delay slot of unconditional jumps / returns
    if (inst.IsReturn() || inst.opcode == Opcode::kJ) {
      clear_on_next_instruction = true;
    }
  }

  return absl::OkStatus();
}

std::string RelocationTracker::FormatInstruction(const Instruction& inst) const {
  const auto* reloc = FindRelocation(inst.vram);
  if (reloc == nullptr) {
    return inst.Disassemble();
  }

  std::string sym_expr = FormatSymbolExpression(reloc->symbol_name, reloc->addend);

  if (reloc->type == RelocType::kHi16 && inst.opcode == Opcode::kLui && inst.rt.has_value()) {
    return absl::StrFormat("lui   %s, %%hi(%s)", RegisterName(*inst.rt), sym_expr);
  }

  if (reloc->type == RelocType::kLo16) {
    switch (inst.opcode) {
      case Opcode::kAddiu:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("addiu %s, %s, %%lo(%s)", RegisterName(*inst.rt),
                                 RegisterName(*inst.rs), sym_expr);
        }
        break;
      case Opcode::kAddi:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("addi  %s, %s, %%lo(%s)", RegisterName(*inst.rt),
                                 RegisterName(*inst.rs), sym_expr);
        }
        break;
      case Opcode::kOri:
        if (inst.rt.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("ori   %s, %s, %%lo(%s)", RegisterName(*inst.rt),
                                 RegisterName(*inst.rs), sym_expr);
        }
        break;

      // Integer Loads
      case Opcode::kLw:
      case Opcode::kLh:
      case Opcode::kLhu:
      case Opcode::kLb:
      case Opcode::kLbu:
      case Opcode::kLd:
      case Opcode::kLld:
      case Opcode::kLwl:
      case Opcode::kLwr: {
        std::string_view op_name;
        switch (inst.opcode) {
          case Opcode::kLw:
            op_name = "lw";
            break;
          case Opcode::kLh:
            op_name = "lh";
            break;
          case Opcode::kLhu:
            op_name = "lhu";
            break;
          case Opcode::kLb:
            op_name = "lb";
            break;
          case Opcode::kLbu:
            op_name = "lbu";
            break;
          case Opcode::kLd:
            op_name = "ld";
            break;
          case Opcode::kLld:
            op_name = "lld";
            break;
          case Opcode::kLwl:
            op_name = "lwl";
            break;
          case Opcode::kLwr:
            op_name = "lwr";
            break;
          default:
            break;
        }
        if (inst.rt.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("%-5s %s, %%lo(%s)(%s)", op_name, RegisterName(*inst.rt), sym_expr,
                                 RegisterName(*inst.rs));
        }
        break;
      }

      // Integer Stores
      case Opcode::kSw:
      case Opcode::kSh:
      case Opcode::kSb:
      case Opcode::kSd:
      case Opcode::kScd:
      case Opcode::kSwl:
      case Opcode::kSwr: {
        std::string_view op_name;
        switch (inst.opcode) {
          case Opcode::kSw:
            op_name = "sw";
            break;
          case Opcode::kSh:
            op_name = "sh";
            break;
          case Opcode::kSb:
            op_name = "sb";
            break;
          case Opcode::kSd:
            op_name = "sd";
            break;
          case Opcode::kScd:
            op_name = "scd";
            break;
          case Opcode::kSwl:
            op_name = "swl";
            break;
          case Opcode::kSwr:
            op_name = "swr";
            break;
          default:
            break;
        }
        if (inst.rt.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("%-5s %s, %%lo(%s)(%s)", op_name, RegisterName(*inst.rt), sym_expr,
                                 RegisterName(*inst.rs));
        }
        break;
      }

      // Coprocessor 1 (FPU) Loads & Stores
      case Opcode::kLwc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("lwc1  %s, %%lo(%s)(%s)", FpRegisterName(*inst.ft), sym_expr,
                                 RegisterName(*inst.rs));
        }
        break;
      case Opcode::kLdc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("ldc1  %s, %%lo(%s)(%s)", FpRegisterName(*inst.ft), sym_expr,
                                 RegisterName(*inst.rs));
        }
        break;
      case Opcode::kSwc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("swc1  %s, %%lo(%s)(%s)", FpRegisterName(*inst.ft), sym_expr,
                                 RegisterName(*inst.rs));
        }
        break;
      case Opcode::kSdc1:
        if (inst.ft.has_value() && inst.rs.has_value()) {
          return absl::StrFormat("sdc1  %s, %%lo(%s)(%s)", FpRegisterName(*inst.ft), sym_expr,
                                 RegisterName(*inst.rs));
        }
        break;

      default:
        break;
    }
  }

  return inst.Disassemble();
}

}  // namespace rom_nom_nom
