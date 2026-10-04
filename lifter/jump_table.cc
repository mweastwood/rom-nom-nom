#include "lifter/jump_table.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/types/span.h"
#include "core/mips.h"

namespace rom_nom_nom {
namespace {

constexpr size_t kMaxSearchWindow = 35;
constexpr uint32_t kMaxCases = 512;

// Resolves a base address loaded into a register by searching backwards for
// `lui` or `lui` + `addiu`.
std::optional<uint32_t> ResolveBaseAddress(absl::Span<const Instruction> instructions,
                                           size_t start_index, Register reg) {
  for (size_t i = start_index; i > 0; --i) {
    const auto& inst = instructions[i - 1];

    // Check for addiu reg, base_reg, lo
    if (inst.opcode == Opcode::kAddiu && inst.rt.has_value() && *inst.rt == reg) {
      Register base_reg = inst.rs.value_or(Register::kZero);
      int32_t lo_imm = static_cast<int32_t>(inst.immediate);

      if (base_reg == reg) {
        // e.g. lui s7, 0x8012; addiu s7, s7, -4544
        for (size_t j = i - 1; j > 0; --j) {
          const auto& prev = instructions[j - 1];
          if (prev.opcode == Opcode::kLui && prev.rt.has_value() && *prev.rt == reg) {
            uint32_t hi_imm = static_cast<uint32_t>(static_cast<uint16_t>(prev.immediate));
            return (hi_imm << 16) + lo_imm;
          }
        }
      } else {
        // e.g. lui at, 0x8012; addiu s7, at, -4544
        for (size_t j = i - 1; j > 0; --j) {
          const auto& prev = instructions[j - 1];
          if (prev.opcode == Opcode::kLui && prev.rt.has_value() && *prev.rt == base_reg) {
            uint32_t hi_imm = static_cast<uint32_t>(static_cast<uint16_t>(prev.immediate));
            return (hi_imm << 16) + lo_imm;
          }
        }
      }
      return std::nullopt;
    }

    // Check for direct lui reg, hi
    if (inst.opcode == Opcode::kLui && inst.rt.has_value() && *inst.rt == reg) {
      uint32_t hi_imm = static_cast<uint32_t>(static_cast<uint16_t>(inst.immediate));
      return (hi_imm << 16);
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<JumpTable> JumpTableDetector::DetectAt(absl::Span<const Instruction> instructions,
                                                     size_t jr_index,
                                                     const MemoryReader& memory_reader) {
  if (jr_index >= instructions.size()) {
    return std::nullopt;
  }

  const auto& jr_inst = instructions[jr_index];
  if (jr_inst.opcode != Opcode::kJr || !jr_inst.rs.has_value() || *jr_inst.rs == Register::kRa) {
    return std::nullopt;
  }

  Register jump_reg = *jr_inst.rs;

  // 1. Scan backward for `lw jump_reg, offset(base_reg)`
  size_t min_idx = (jr_index > kMaxSearchWindow) ? (jr_index - kMaxSearchWindow) : 0;
  size_t lw_index = jr_index;
  int32_t load_offset = 0;
  Register base_ptr_reg = Register::kZero;
  bool found_lw = false;

  for (size_t i = jr_index; i > min_idx; --i) {
    const auto& inst = instructions[i - 1];
    if (inst.opcode == Opcode::kLw && inst.rt.has_value() && *inst.rt == jump_reg) {
      lw_index = i - 1;
      load_offset = static_cast<int32_t>(inst.immediate);
      base_ptr_reg = inst.rs.value_or(Register::kZero);
      found_lw = true;
      break;
    }
  }

  if (!found_lw || base_ptr_reg == Register::kZero) {
    return std::nullopt;
  }

  // 2. Scan backward from lw for `addu base_ptr_reg, op1, op2`
  size_t addu_index = lw_index;
  Register op1 = Register::kZero;
  Register op2 = Register::kZero;
  bool found_addu = false;

  for (size_t i = lw_index; i > min_idx; --i) {
    const auto& inst = instructions[i - 1];
    if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kAdd) && inst.rd.has_value() &&
        *inst.rd == base_ptr_reg) {
      addu_index = i - 1;
      op1 = inst.rs.value_or(Register::kZero);
      op2 = inst.rt.value_or(Register::kZero);
      found_addu = true;
      break;
    }
  }

  if (!found_addu) {
    return std::nullopt;
  }

  // 3. Identify which operand is the shifted offset: `sll offset_reg, index_reg, 2`
  size_t sll_index = addu_index;
  Register table_base_reg = Register::kZero;
  Register index_reg = Register::kZero;
  bool found_sll = false;

  for (size_t i = addu_index + 1; i > min_idx; --i) {
    // Note: sll can be in the delay slot of a branch preceding addu, or right before addu.
    const auto& inst = instructions[i - 1];
    if (inst.opcode == Opcode::kSll && inst.shift_amount == 2 && inst.rd.has_value()) {
      if (*inst.rd == op1) {
        sll_index = i - 1;
        table_base_reg = op2;
        index_reg = inst.rt.value_or(Register::kZero);
        found_sll = true;
        break;
      } else if (*inst.rd == op2) {
        sll_index = i - 1;
        table_base_reg = op1;
        index_reg = inst.rt.value_or(Register::kZero);
        found_sll = true;
        break;
      }
    }
  }

  if (!found_sll || index_reg == Register::kZero) {
    return std::nullopt;
  }

  // 4. Resolve the jump table base address
  auto base_vram_opt = ResolveBaseAddress(instructions, addu_index, table_base_reg);
  if (!base_vram_opt.has_value()) {
    return std::nullopt;
  }

  uint32_t table_vram = *base_vram_opt + load_offset;

  // 5. Scan backward for bounds check branch: `beqz cmp_reg, default_target`
  // and bounds comparison: `sltiu cmp_reg, index_reg, num_cases`
  size_t branch_search_start =
      (sll_index + 2 < instructions.size()) ? (sll_index + 2) : instructions.size();
  size_t bounds_branch_index = 0;
  uint32_t bounds_branch_vram = 0;
  uint32_t default_target_vram = 0;
  Register cmp_reg = Register::kZero;
  bool found_branch = false;

  for (size_t i = branch_search_start; i > min_idx; --i) {
    const auto& inst = instructions[i - 1];
    if (inst.opcode == Opcode::kBeq) {
      Register r1 = inst.rs.value_or(Register::kZero);
      Register r2 = inst.rt.value_or(Register::kZero);
      if (r2 == Register::kZero && r1 != Register::kZero) {
        cmp_reg = r1;
        bounds_branch_index = i - 1;
        bounds_branch_vram = inst.vram;
        default_target_vram = inst.BranchTarget();
        found_branch = true;
        break;
      } else if (r1 == Register::kZero && r2 != Register::kZero) {
        cmp_reg = r2;
        bounds_branch_index = i - 1;
        bounds_branch_vram = inst.vram;
        default_target_vram = inst.BranchTarget();
        found_branch = true;
        break;
      }
    }
  }

  if (!found_branch || cmp_reg == Register::kZero) {
    return std::nullopt;
  }

  // Find sltiu cmp_reg, index_reg, num_cases
  uint32_t num_cases = 0;
  size_t sltiu_index = 0;
  bool found_sltiu = false;

  for (size_t i = bounds_branch_index; i > min_idx; --i) {
    const auto& inst = instructions[i - 1];
    if ((inst.opcode == Opcode::kSltiu || inst.opcode == Opcode::kSlti) && inst.rt.has_value() &&
        *inst.rt == cmp_reg) {
      if (inst.immediate > 0) {
        num_cases = static_cast<uint32_t>(inst.immediate);
        sltiu_index = i - 1;
        found_sltiu = true;
        break;
      }
    }
  }

  if (!found_sltiu || num_cases == 0 || num_cases > kMaxCases) {
    return std::nullopt;
  }

  // 6. Check for lower bound normalization: `addiu index_reg, orig_var, -lower_bound`
  int64_t lower_bound = 0;
  Register final_index_reg = index_reg;
  size_t norm_search_min = (sltiu_index > 10) ? (sltiu_index - 10) : 0;

  for (size_t i = sltiu_index; i > norm_search_min; --i) {
    const auto& inst = instructions[i - 1];
    if (inst.opcode == Opcode::kAddiu && inst.rt.has_value() && *inst.rt == index_reg) {
      if (inst.immediate < 0) {
        lower_bound = -static_cast<int64_t>(inst.immediate);
        final_index_reg = inst.rs.value_or(Register::kZero);
      }
      break;
    }
  }

  // 7. Read target entries from memory
  std::vector<JumpTableEntry> entries;
  entries.reserve(num_cases);

  for (uint32_t c = 0; c < num_cases; ++c) {
    auto word = memory_reader(table_vram + c * sizeof(uint32_t));
    if (!word.has_value()) {
      return std::nullopt;
    }
    uint32_t target = *word;
    if (target < 0x80000000 || (target & 3) != 0) {
      return std::nullopt;
    }
    entries.push_back(JumpTableEntry{
        .case_index = c,
        .case_value = static_cast<int64_t>(c) + lower_bound,
        .target_vram = target,
    });
  }

  JumpTable table;
  table.switch_vram = jr_inst.vram;
  table.bounds_branch_vram = bounds_branch_vram;
  table.table_vram = table_vram;
  table.num_cases = num_cases;
  table.lower_bound = lower_bound;
  table.index_register = final_index_reg;
  table.default_target_vram = default_target_vram;
  table.entries = std::move(entries);

  return table;
}

std::vector<JumpTable> JumpTableDetector::DetectJumpTables(
    absl::Span<const Instruction> instructions, const MemoryReader& memory_reader) {
  std::vector<JumpTable> tables;
  for (size_t i = 0; i < instructions.size(); ++i) {
    if (instructions[i].opcode == Opcode::kJr) {
      if (instructions[i].rs.has_value() && *instructions[i].rs != Register::kRa) {
        auto table = DetectAt(instructions, i, memory_reader);
        if (table.has_value()) {
          tables.push_back(std::move(*table));
        }
      }
    }
  }
  return tables;
}

}  // namespace rom_nom_nom
