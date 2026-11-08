#include "lifter/invert_gas_append_insn.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "lifter/invert_gas_mips_ip.h"

namespace rom_nom_nom {
namespace {

// Instruction property flags from Binutils 2.6 `include/opcode/mips.h`
// (`lines 150-245`).
constexpr uint32_t kInsnWriteGprD = 0x00000001u;
constexpr uint32_t kInsnWriteGprT = 0x00000002u;
constexpr uint32_t kInsnWriteGpr31 = 0x00000004u;
constexpr uint32_t kInsnWriteFprD = 0x00000008u;
constexpr uint32_t kInsnWriteFprS = 0x00000010u;
constexpr uint32_t kInsnWriteFprT = 0x00000020u;
constexpr uint32_t kInsnReadGprS = 0x00000040u;
constexpr uint32_t kInsnReadGprT = 0x00000080u;
constexpr uint32_t kInsnReadFprS = 0x00000100u;
constexpr uint32_t kInsnReadFprT = 0x00000200u;
constexpr uint32_t kInsnWriteCondCode = 0x00000400u;
constexpr uint32_t kInsnReadCondCode = 0x00000800u;
constexpr uint32_t kInsnTrap = 0x00001000u;
constexpr uint32_t kInsnStoreMemory = 0x00002000u;
constexpr uint32_t kInsnLoadMemoryDelay = 0x00004000u;
constexpr uint32_t kInsnLoadCoprocDelay = 0x00008000u;
constexpr uint32_t kInsnCoprocMoveDelay = 0x00010000u;
constexpr uint32_t kInsnCoprocMemoryDelay = 0x00020000u;
constexpr uint32_t kInsnReadHi = 0x00040000u;
constexpr uint32_t kInsnReadLo = 0x00080000u;
constexpr uint32_t kInsnWriteHi = 0x00100000u;
constexpr uint32_t kInsnWriteLo = 0x00200000u;
constexpr uint32_t kInsnUncondBranchDelay = 0x00400000u;
constexpr uint32_t kInsnCondBranchDelay = 0x00800000u;
constexpr uint32_t kInsnCondBranchLikely = 0x01000000u;

// Instructions whose `pinfo` causes `append_insn` / `mips_emit_delays` in
// `.set reorder` mode (`mips_isa = 3`, `interlocks = 0`) to insert automatic
// hazard `nop`s when followed by a dependent instruction, label, `.set
// noreorder`, or end of assembly (`tc-mips.c:1310-1425, 1768-1816`).
constexpr uint32_t kReorderHazardPinfoMask =
    kInsnLoadCoprocDelay | kInsnCoprocMoveDelay | kInsnWriteCondCode | kInsnReadLo | kInsnReadHi;

uint32_t GetInsnPinfo(uint32_t w) {
  if (w == 0x00000000u) {
    return 0u;
  }
  const uint32_t op = (w >> 26) & 0x3fu;
  const uint32_t rs = (w >> 21) & 0x1fu;
  const uint32_t rt = (w >> 16) & 0x1fu;
  const uint32_t funct = w & 0x3fu;

  switch (op) {
    case 0x00u:
      switch (funct) {
        case 0x00u:
        case 0x02u:
        case 0x03u:
        case 0x38u:
        case 0x3au:
        case 0x3bu:
        case 0x3cu:
        case 0x3eu:
        case 0x3fu:
          return kInsnWriteGprD | kInsnReadGprT;
        case 0x04u:
        case 0x06u:
        case 0x07u:
        case 0x14u:
        case 0x16u:
        case 0x17u:
        case 0x20u:
        case 0x21u:
        case 0x22u:
        case 0x23u:
        case 0x24u:
        case 0x25u:
        case 0x26u:
        case 0x27u:
        case 0x2au:
        case 0x2bu:
        case 0x2cu:
        case 0x2du:
        case 0x2eu:
        case 0x2fu:
          return kInsnWriteGprD | kInsnReadGprS | kInsnReadGprT;
        case 0x08u:
          return kInsnUncondBranchDelay | kInsnReadGprS;
        case 0x09u:
          return kInsnUncondBranchDelay | kInsnReadGprS | kInsnWriteGprD;
        case 0x0cu:
        case 0x0du:
        case 0x0fu:
          return kInsnTrap;
        case 0x10u:
          return kInsnWriteGprD | kInsnReadHi;
        case 0x11u:
          return kInsnReadGprS | kInsnWriteHi;
        case 0x12u:
          return kInsnWriteGprD | kInsnReadLo;
        case 0x13u:
          return kInsnReadGprS | kInsnWriteLo;
        case 0x18u:
        case 0x19u:
        case 0x1au:
        case 0x1bu:
        case 0x1cu:
        case 0x1du:
        case 0x1eu:
        case 0x1fu:
          return kInsnReadGprS | kInsnReadGprT | kInsnWriteHi | kInsnWriteLo;
        case 0x30u:
        case 0x31u:
        case 0x32u:
        case 0x33u:
        case 0x34u:
        case 0x36u:
          return kInsnReadGprS | kInsnReadGprT | kInsnTrap;
        default:
          return 0u;
      }
    case 0x01u:
      switch (rt) {
        case 0x00u:
        case 0x01u:
          return kInsnCondBranchDelay | kInsnReadGprS;
        case 0x02u:
        case 0x03u:
          return kInsnCondBranchLikely | kInsnReadGprS;
        case 0x08u:
        case 0x09u:
        case 0x0au:
        case 0x0bu:
        case 0x0cu:
        case 0x0eu:
          return kInsnReadGprS | kInsnTrap;
        case 0x10u:
        case 0x11u:
          return kInsnCondBranchDelay | kInsnReadGprS | kInsnWriteGpr31;
        case 0x12u:
        case 0x13u:
          return kInsnCondBranchLikely | kInsnReadGprS | kInsnWriteGpr31;
        default:
          return 0u;
      }
    case 0x02u:
      return kInsnUncondBranchDelay;
    case 0x03u:
      return kInsnUncondBranchDelay | kInsnWriteGpr31;
    case 0x04u:
    case 0x05u:
      return kInsnCondBranchDelay | kInsnReadGprS | kInsnReadGprT;
    case 0x06u:
    case 0x07u:
      return kInsnCondBranchDelay | kInsnReadGprS;
    case 0x08u:
    case 0x09u:
    case 0x0au:
    case 0x0bu:
    case 0x0cu:
    case 0x0du:
    case 0x0eu:
    case 0x18u:
    case 0x19u:
    case 0x1au:
    case 0x1bu:
    case 0x37u:
      return kInsnWriteGprT | kInsnReadGprS;
    case 0x0fu:
      return kInsnWriteGprT;
    case 0x10u:
      switch (rs) {
        case 0x00u:
        case 0x01u:
        case 0x02u:
          return kInsnLoadCoprocDelay | kInsnWriteGprT;
        case 0x04u:
        case 0x05u:
        case 0x06u:
          return kInsnCoprocMoveDelay | kInsnReadGprT | kInsnWriteCondCode;
        case 0x08u:
          return (rt < 0x02u) ? (kInsnCondBranchDelay | kInsnReadCondCode)
                              : (kInsnCondBranchLikely | kInsnReadCondCode);
        default:
          return 0u;
      }
    case 0x11u:
      switch (rs) {
        case 0x00u:
        case 0x01u:
        case 0x02u:
          return kInsnLoadCoprocDelay | kInsnWriteGprT | kInsnReadFprS;
        case 0x04u:
        case 0x05u:
          return kInsnCoprocMoveDelay | kInsnReadGprT | kInsnWriteFprS;
        case 0x06u:
          return kInsnCoprocMoveDelay | kInsnReadGprT | kInsnWriteCondCode;
        case 0x08u:
          return (rt < 0x02u) ? (kInsnCondBranchDelay | kInsnReadCondCode)
                              : (kInsnCondBranchLikely | kInsnReadCondCode);
        case 0x10u:
        case 0x11u:
        case 0x14u:
        case 0x15u:
          if (funct >= 0x30u && funct <= 0x3fu) {
            return kInsnReadFprS | kInsnReadFprT | kInsnWriteCondCode;
          }
          if (funct <= 0x03u) {
            return kInsnWriteFprD | kInsnReadFprS | kInsnReadFprT;
          }
          return kInsnWriteFprD | kInsnReadFprS;
        default:
          return 0u;
      }
    case 0x14u:
    case 0x15u:
      return kInsnCondBranchLikely | kInsnReadGprS | kInsnReadGprT;
    case 0x16u:
    case 0x17u:
      return kInsnCondBranchLikely | kInsnReadGprS;
    case 0x20u:
    case 0x21u:
    case 0x22u:
    case 0x23u:
    case 0x24u:
    case 0x25u:
    case 0x26u:
    case 0x27u:
    case 0x30u:
    case 0x34u:
      return kInsnLoadMemoryDelay | kInsnReadGprS | kInsnWriteGprT;
    case 0x28u:
    case 0x29u:
    case 0x2au:
    case 0x2bu:
    case 0x2cu:
    case 0x2du:
    case 0x2eu:
    case 0x3fu:
      return kInsnStoreMemory | kInsnReadGprS | kInsnReadGprT;
    case 0x2fu:
      return kInsnReadGprS;
    case 0x31u:
    case 0x35u:
      return kInsnCoprocMemoryDelay | kInsnReadGprS | kInsnWriteFprT;
    case 0x38u:
    case 0x3cu:
      return kInsnStoreMemory | kInsnReadGprS | kInsnReadGprT | kInsnWriteGprT;
    case 0x39u:
    case 0x3du:
      return kInsnStoreMemory | kInsnReadGprS | kInsnReadFprT;
    default:
      return 0u;
  }
}

bool IsBranchOrJumpWord(uint32_t w) {
  const uint32_t pinfo = GetInsnPinfo(w);
  return (pinfo & (kInsnUncondBranchDelay | kInsnCondBranchDelay | kInsnCondBranchLikely)) != 0u;
}

bool IsPcRelativeBranchWord(uint32_t w) {
  const uint32_t op = (w >> 26) & 0x3fu;
  if (op == 0x00u || op == 0x02u || op == 0x03u) {
    return false;
  }
  return IsBranchOrJumpWord(w);
}

// Mirrors `tc-mips.c:1208-1251`: `insn_uses_reg(ip, reg, /*fpr=*/0)`.
bool InsnUsesGpr(uint32_t w, uint32_t pinfo, uint32_t reg) {
  if (reg == 0u) {
    return false;
  }
  const uint32_t rs = (w >> 21) & 0x1fu;
  const uint32_t rt = (w >> 16) & 0x1fu;
  if ((pinfo & kInsnReadGprS) != 0u && rs == reg) {
    return true;
  }
  if ((pinfo & kInsnReadGprT) != 0u && rt == reg) {
    return true;
  }
  return false;
}

// Mirrors `tc-mips.c:1530-1665`: returns true iff `append_insn` in `.set
// reorder` mode (`mips_optimize >= 2`, `mips_isa = 3`, `interlocks = 0`,
// `mips_pic = NO_PIC`, `prev_prev_insn_unreordered = 0`, `insn_label = NULL`)
// will swap a preceding instruction `delay_w` into the delay slot of
// `branch_w`.
bool CanSwapBranchWithInsn(uint32_t branch_w, uint32_t delay_w) {
  const uint32_t branch_pinfo = GetInsnPinfo(branch_w);
  const uint32_t delay_pinfo = GetInsnPinfo(delay_w);

  if ((branch_pinfo & (kInsnUncondBranchDelay | kInsnCondBranchDelay)) == 0u) {
    return false;
  }
  if ((branch_pinfo & kInsnReadCondCode) != 0u) {
    return false;
  }

  constexpr uint32_t kUnswappableDelayMask =
      kInsnLoadCoprocDelay | kInsnCoprocMoveDelay | kInsnWriteCondCode | kInsnReadLo | kInsnReadHi |
      kInsnUncondBranchDelay | kInsnCondBranchDelay | kInsnCondBranchLikely | kInsnTrap;
  if ((delay_pinfo & kUnswappableDelayMask) != 0u) {
    return false;
  }

  const uint32_t delay_rt = (delay_w >> 16) & 0x1fu;
  const uint32_t delay_rd = (delay_w >> 11) & 0x1fu;
  const uint32_t branch_rd = (branch_w >> 11) & 0x1fu;

  if ((delay_pinfo & kInsnWriteGprT) != 0u && InsnUsesGpr(branch_w, branch_pinfo, delay_rt)) {
    return false;
  }
  if ((delay_pinfo & kInsnWriteGprD) != 0u && InsnUsesGpr(branch_w, branch_pinfo, delay_rd)) {
    return false;
  }

  if ((branch_pinfo & kInsnWriteGprD) != 0u) {
    if (((delay_pinfo & kInsnWriteGprT) != 0u && delay_rt == branch_rd) ||
        ((delay_pinfo & kInsnWriteGprD) != 0u && delay_rd == branch_rd) ||
        InsnUsesGpr(delay_w, delay_pinfo, branch_rd)) {
      return false;
    }
  } else if ((branch_pinfo & kInsnWriteGpr31) != 0u) {
    if (((delay_pinfo & kInsnWriteGprT) != 0u && delay_rt == 31u) ||
        ((delay_pinfo & kInsnWriteGprD) != 0u && delay_rd == 31u) ||
        InsnUsesGpr(delay_w, delay_pinfo, 31u)) {
      return false;
    }
  }

  return true;
}

std::string ReplaceTargetWithLabel(std::string_view insn_line, std::string_view label) {
  const size_t last_comma = insn_line.rfind(',');
  if (last_comma != std::string_view::npos) {
    return absl::StrCat(insn_line.substr(0, last_comma + 1), label);
  }
  const size_t space_pos = insn_line.find_first_of(" \t");
  if (space_pos != std::string_view::npos) {
    return absl::StrCat(insn_line.substr(0, space_pos + 1), label);
  }
  return std::string(insn_line);
}

}  // namespace

std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) {
  if (words_x.empty()) {
    return {};
  }

  std::vector<std::string> base_disasm(words_x.size());
  for (size_t i = 0; i < words_x.size(); ++i) {
    base_disasm[i] = InvertGasMipsIp(words_x[i]);
    if (base_disasm[i].empty()) {
      return {};
    }
  }

  // Pass 1: Validate branch/delay-slot structure and assign local `$L<N>`
  // labels to in-bounds branch and local `j` targets.
  absl::flat_hash_map<size_t, std::string> target_labels;
  int next_label_id = 2;
  for (size_t i = 0; i < words_x.size();) {
    const uint32_t w = words_x[i];
    if (IsBranchOrJumpWord(w)) {
      if (i + 1 >= words_x.size()) {
        return {};
      }
      const uint32_t op = (w >> 26) & 0x3fu;
      if (op == 0x02u) {
        const size_t target_idx = static_cast<size_t>(w & 0x03ffffffu);
        if (target_idx > 0 && target_idx < words_x.size() && !target_labels.contains(target_idx)) {
          target_labels[target_idx] = absl::StrFormat("$L%d", next_label_id++);
        }
      } else if (IsPcRelativeBranchWord(w)) {
        const int16_t simm = static_cast<int16_t>(w & 0xffffu);
        const int64_t target_idx = static_cast<int64_t>(i) + 1 + static_cast<int64_t>(simm);
        if (target_idx >= 0 && target_idx < static_cast<int64_t>(words_x.size())) {
          const size_t u_target = static_cast<size_t>(target_idx);
          if (!target_labels.contains(u_target)) {
            target_labels[u_target] = absl::StrFormat("$L%d", next_label_id++);
          }
        }
      }
      i += 2;
    } else {
      ++i;
    }
  }

  // Pass 2: Emit pre-`append_insn` assembly lines, tracking `prev_insn_valid`,
  // `prev_insn_is_delay_slot`, and `prev_insn_word` across `.set reorder`
  // segments.
  std::vector<std::string> lines;
  lines.reserve(words_x.size() + target_labels.size());
  bool prev_insn_valid = false;
  bool prev_insn_is_delay_slot = false;
  uint32_t prev_insn_word = 0u;

  for (size_t i = 0; i < words_x.size();) {
    const bool has_label_at_i = target_labels.contains(i);
    if (has_label_at_i) {
      lines.push_back(absl::StrCat(target_labels.at(i), ":"));
      // Defining a label calls `mips_emit_delays()` (`prev_insn_valid = 0`)
      // and `mips_define_label()` (`insn_label != NULL`).
      prev_insn_valid = false;
      prev_insn_is_delay_slot = false;
    }

    const uint32_t w = words_x[i];
    if (IsBranchOrJumpWord(w)) {
      const uint32_t delay_w = words_x[i + 1];
      std::string jump_line = base_disasm[i];
      const uint32_t op = (w >> 26) & 0x3fu;
      if (op == 0x02u) {
        const size_t target_idx = static_cast<size_t>(w & 0x03ffffffu);
        if (auto it = target_labels.find(target_idx); it != target_labels.end()) {
          jump_line = ReplaceTargetWithLabel(jump_line, it->second);
        }
      } else if (IsPcRelativeBranchWord(w)) {
        const int16_t simm = static_cast<int16_t>(w & 0xffffu);
        const int64_t target_idx = static_cast<int64_t>(i) + 1 + static_cast<int64_t>(simm);
        if (target_idx >= 0 && target_idx < static_cast<int64_t>(words_x.size())) {
          if (auto it = target_labels.find(static_cast<size_t>(target_idx));
              it != target_labels.end()) {
            jump_line = ReplaceTargetWithLabel(jump_line, it->second);
          }
        }
      }

      const bool has_label_at_delay = target_labels.contains(i + 1);
      if (delay_w != 0x00000000u) {
        if (!has_label_at_i && !has_label_at_delay && CanSwapBranchWithInsn(w, delay_w)) {
          // Emit `[delay_insn, jump_insn]` in `.set reorder` mode so
          // `append_insn` swaps `delay_insn` into `jump_insn`'s delay slot.
          lines.push_back(base_disasm[i + 1]);
          lines.push_back(std::move(jump_line));
          prev_insn_valid = true;
          prev_insn_is_delay_slot = true;
          prev_insn_word = delay_w;
        } else {
          // Delay slot was filled by GCC (`dbr_schedule`) with an instruction
          // that `append_insn` would not swap (or across a label); preserve in
          // `.set noreorder`.
          lines.push_back(".set\tnoreorder");
          lines.push_back(std::move(jump_line));
          if (has_label_at_delay) {
            lines.push_back(absl::StrCat(target_labels.at(i + 1), ":"));
          }
          lines.push_back(base_disasm[i + 1]);
          lines.push_back(".set\treorder");
          prev_insn_valid = false;
          prev_insn_is_delay_slot = false;
        }
      } else {
        // Delay slot is `nop` (`0x00000000`). Check if emitting bare
        // `jump_line` in `.set reorder` mode would cause `append_insn` to
        // erroneously swap the preceding instruction into this branch's delay
        // slot.
        const bool prev_would_swap = prev_insn_valid && !prev_insn_is_delay_slot &&
                                     !has_label_at_i && CanSwapBranchWithInsn(w, prev_insn_word);
        if (has_label_at_delay || prev_would_swap) {
          lines.push_back(".set\tnoreorder");
          lines.push_back(std::move(jump_line));
          if (has_label_at_delay) {
            lines.push_back(absl::StrCat(target_labels.at(i + 1), ":"));
          }
          lines.push_back("nop");
          lines.push_back(".set\treorder");
          prev_insn_valid = false;
          prev_insn_is_delay_slot = false;
        } else {
          lines.push_back(std::move(jump_line));
          prev_insn_valid = true;
          prev_insn_is_delay_slot = true;
          prev_insn_word = 0x00000000u;
        }
      }
      i += 2;
      continue;
    }

    const uint32_t pinfo = GetInsnPinfo(w);
    if ((pinfo & kReorderHazardPinfoMask) != 0u) {
      // Wrap hazard-producing instructions in `.set noreorder` / `.set reorder`
      // so `append_insn` and `mips_emit_delays()` do not inject extra `nop`
      // words beyond those explicitly present in `words_x`.
      lines.push_back(".set\tnoreorder");
      lines.push_back(base_disasm[i]);
      lines.push_back(".set\treorder");
      prev_insn_valid = false;
      prev_insn_is_delay_slot = false;
    } else {
      lines.push_back(base_disasm[i]);
      prev_insn_valid = true;
      prev_insn_is_delay_slot = false;
      prev_insn_word = w;
    }
    ++i;
  }

  return lines;
}

}  // namespace rom_nom_nom
