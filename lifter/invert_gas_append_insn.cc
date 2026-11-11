#include "lifter/invert_gas_append_insn.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_gas_mips_ip.h"

namespace rom_nom_nom::lifter {
namespace {

// Instruction property flags from Binutils 2.6 `include/opcode/mips.h`
// (`lines 150-245`).
constexpr uint32_t kInsnWriteGprD = 0x00000001u;
constexpr uint32_t kInsnWriteGprT = 0x00000002u;
constexpr uint32_t kInsnWriteFprD = 0x00000004u;
constexpr uint32_t kInsnWriteFprS = 0x00000008u;
constexpr uint32_t kInsnWriteFprT = 0x00000010u;
constexpr uint32_t kInsnReadGprS = 0x00000020u;
constexpr uint32_t kInsnReadGprT = 0x00000040u;
constexpr uint32_t kInsnReadFprS = 0x00000080u;
constexpr uint32_t kInsnReadFprT = 0x00000100u;
constexpr uint32_t kInsnReadFprR = 0x00000200u;
constexpr uint32_t kInsnReadCondCode = 0x00000400u;
constexpr uint32_t kInsnWriteCondCode = 0x00000800u;
constexpr uint32_t kInsnReadLo = 0x00001000u;
constexpr uint32_t kInsnReadHi = 0x00002000u;
constexpr uint32_t kInsnWriteLo = 0x00004000u;
constexpr uint32_t kInsnWriteHi = 0x00008000u;
constexpr uint32_t kInsnCop = 0x00010000u;
constexpr uint32_t kInsnUncondBranchDelay = 0x00020000u;
constexpr uint32_t kInsnCondBranchDelay = 0x00040000u;
constexpr uint32_t kInsnCondBranchLikely = 0x00080000u;
constexpr uint32_t kInsnLoadCoprocDelay = 0x00100000u;
constexpr uint32_t kInsnCoprocMoveDelay = 0x00200000u;
constexpr uint32_t kInsnCoprocMemoryDelay = 0x00400000u;
constexpr uint32_t kInsnLoadMemoryDelay = 0x00800000u;
constexpr uint32_t kInsnWriteGpr31 = 0x01000000u;
constexpr uint32_t kInsnStoreMemory = 0x02000000u;
constexpr uint32_t kInsnTrap = 0x04000000u;

// Instructions whose `pinfo` causes `append_insn` / `mips_emit_delays` in
// `.set reorder` mode (`mips_isa = 3`, `interlocks = 0`) to insert automatic
// hazard `nop`s when followed by a dependent instruction, label, `.set
// noreorder`, or end of assembly (`tc-mips.c:1310-1425, 1768-1816`).
constexpr uint32_t kReorderHazardPinfoMask =
    kInsnLoadCoprocDelay | kInsnCoprocMoveDelay | kInsnWriteCondCode | kInsnReadLo | kInsnReadHi;

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

// Mirrors `tc-mips.c:1208-1251`: `insn_uses_reg(ip, reg, /*fpr=*/1)` (`mips_isa = 3`).
bool InsnUsesFpr(uint32_t w, uint32_t pinfo, uint32_t reg) {
  const uint32_t reg_even = reg & ~1u;
  const uint32_t fr = ((w >> 21) & 0x1fu) & ~1u;
  const uint32_t ft = ((w >> 16) & 0x1fu) & ~1u;
  const uint32_t fs = ((w >> 11) & 0x1fu) & ~1u;
  if ((pinfo & kInsnReadFprS) != 0u && fs == reg_even) {
    return true;
  }
  if ((pinfo & kInsnReadFprT) != 0u && ft == reg_even) {
    return true;
  }
  if ((pinfo & kInsnReadFprR) != 0u && fr == reg_even) {
    return true;
  }
  return false;
}

// Mirrors `tc-mips.c:1768-1816`: number of `nop` words emitted by
// `mips_emit_delays()` when turning on `.set noreorder` or defining a label.
size_t MipsEmitDelaysNops(uint32_t prev_w, uint32_t prev_prev_w) {
  const uint32_t prev_pinfo = GetInsnPinfo(prev_w);
  const uint32_t prev_prev_pinfo = GetInsnPinfo(prev_prev_w);
  if ((prev_pinfo & kReorderHazardPinfoMask) != 0u) {
    if ((prev_pinfo & (kInsnWriteCondCode | kInsnReadLo | kInsnReadHi)) != 0u) {
      return 2;
    }
    return 1;
  }
  if ((prev_prev_pinfo & (kInsnWriteCondCode | kInsnReadLo | kInsnReadHi)) != 0u) {
    return 1;
  }
  return 0;
}

// Mirrors `tc-mips.c:1295-1425`: number of `nop` words emitted by
// `append_insn` before instruction `cur_w` in `.set reorder` mode
// (`mips_isa = 3`, `interlocks = 0`, `mips_optimize = 2`).
size_t AppendInsnHazardNops(uint32_t prev_w, uint32_t prev_prev_w, uint32_t cur_w) {
  const uint32_t prev_pinfo = GetInsnPinfo(prev_w);
  const uint32_t prev_prev_pinfo = GetInsnPinfo(prev_prev_w);
  const uint32_t cur_pinfo = GetInsnPinfo(cur_w);
  size_t nops = 0;

  if ((prev_pinfo & kInsnLoadCoprocDelay) != 0u) {
    if (InsnUsesGpr(cur_w, cur_pinfo, (prev_w >> 16) & 0x1fu)) {
      ++nops;
    }
  } else if ((prev_pinfo & kInsnCoprocMoveDelay) != 0u) {
    if ((prev_pinfo & kInsnWriteFprT) != 0u) {
      if (InsnUsesFpr(cur_w, cur_pinfo, (prev_w >> 16) & 0x1fu)) {
        ++nops;
      }
    } else if ((prev_pinfo & kInsnWriteFprS) != 0u) {
      if (InsnUsesFpr(cur_w, cur_pinfo, (prev_w >> 11) & 0x1fu)) {
        ++nops;
      }
    } else {
      if ((prev_pinfo & kInsnWriteCondCode) != 0u && (cur_pinfo & kInsnReadCondCode) != 0u) {
        nops += 2;
      } else if ((cur_pinfo & kInsnCop) != 0u) {
        ++nops;
      }
    }
  } else if ((prev_pinfo & kInsnWriteCondCode) != 0u) {
    if ((cur_pinfo & kInsnReadCondCode) != 0u) {
      ++nops;
    }
  } else if ((prev_pinfo & kInsnReadLo) != 0u) {
    if ((cur_pinfo & kInsnWriteLo) != 0u) {
      nops += 2;
    }
  } else if ((prev_pinfo & kInsnReadHi) != 0u) {
    if ((cur_pinfo & kInsnWriteHi) != 0u) {
      nops += 2;
    }
  }

  if (nops == 0) {
    if (((prev_prev_pinfo & kInsnCoprocMoveDelay) != 0u &&
         (prev_prev_pinfo & kInsnWriteCondCode) != 0u && (cur_pinfo & kInsnReadCondCode) != 0u) ||
        ((prev_prev_pinfo & kInsnReadLo) != 0u && (cur_pinfo & kInsnWriteLo) != 0u) ||
        ((prev_prev_pinfo & kInsnReadHi) != 0u && (cur_pinfo & kInsnWriteHi) != 0u)) {
      ++nops;
    }
  }

  return nops;
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

uint32_t GetInsnPinfo(uint32_t w) {
  if (w == 0x00000000u) {
    return 0u;
  }

  const uint32_t op = (w >> 26) & 0x3fu;
  const uint32_t rs = (w >> 21) & 0x1fu;
  const uint32_t rt = (w >> 16) & 0x1fu;
  const uint32_t funct = w & 0x3fu;

  switch (op) {
    case 0x00u: {
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
          return kInsnUncondBranchDelay | kInsnWriteGprD | kInsnReadGprS;
        case 0x0cu:
        case 0x0du:
          return kInsnTrap;
        case 0x0fu:
          return 0u;
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
          return kInsnReadGprS | kInsnReadGprT | kInsnWriteLo | kInsnWriteHi;
        case 0x30u:
        case 0x31u:
        case 0x32u:
        case 0x33u:
        case 0x34u:
        case 0x36u:
          return kInsnTrap | kInsnReadGprS | kInsnReadGprT;
        default:
          return 0u;
      }
    }
    case 0x01u: {
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
          return kInsnTrap | kInsnReadGprS;
        case 0x10u:
        case 0x11u:
          return kInsnCondBranchDelay | kInsnWriteGpr31 | kInsnReadGprS;
        case 0x12u:
        case 0x13u:
          return kInsnCondBranchLikely | kInsnWriteGpr31 | kInsnReadGprS;
        default:
          return 0u;
      }
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
    case 0x12u:
    case 0x13u:
      switch (rs) {
        case 0x00u:
        case 0x01u:
        case 0x02u:
          return kInsnLoadCoprocDelay | kInsnWriteGprT | kInsnCop;
        case 0x04u:
        case 0x05u:
          return kInsnCoprocMoveDelay | kInsnReadGprT | kInsnCop;
        case 0x06u:
          return kInsnCoprocMoveDelay | kInsnReadGprT | kInsnWriteCondCode | kInsnCop;
        case 0x08u:
          return (rt < 0x02u) ? (kInsnCondBranchDelay | kInsnReadCondCode | kInsnCop)
                              : (kInsnCondBranchLikely | kInsnReadCondCode | kInsnCop);
        default:
          return kInsnCop;
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
          return kInsnCoprocMoveDelay | kInsnReadGprT | kInsnWriteCondCode | kInsnCop;
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

absl::Span<const uint32_t> StripSectionAlignmentPadding(
    absl::Span<const uint32_t> machine_words_x) {
  if (machine_words_x.size() < 4 || (machine_words_x.size() % 4) != 0) {
    return machine_words_x;
  }

  size_t cand_len = machine_words_x.size();
  while (cand_len > 2 && cand_len + 3 >= machine_words_x.size() &&
         machine_words_x[cand_len - 1] == 0x00000000u &&
         !IsBranchOrJumpWord(machine_words_x[cand_len - 2])) {
    --cand_len;
  }
  if (cand_len == machine_words_x.size() || !IsBranchOrJumpWord(machine_words_x[cand_len - 2])) {
    return machine_words_x;
  }

  // Ensure no branch or local jump in `0 .. cand_len - 2` targets `>= cand_len`.
  for (size_t i = 0; i < cand_len;) {
    const uint32_t w = machine_words_x[i];
    if (IsBranchOrJumpWord(w)) {
      if (i + 1 >= cand_len) {
        return machine_words_x;
      }
      const uint32_t op = (w >> 26) & 0x3fu;
      if (op == 0x02u) {
        const size_t target_idx = static_cast<size_t>(w & 0x03ffffffu);
        if (target_idx >= cand_len && target_idx < machine_words_x.size()) {
          return machine_words_x;
        }
      } else if (IsPcRelativeBranchWord(w)) {
        const int16_t simm = static_cast<int16_t>(w & 0xffffu);
        const int64_t target_idx = static_cast<int64_t>(i) + 1 + static_cast<int64_t>(simm);
        if (target_idx >= static_cast<int64_t>(cand_len) &&
            target_idx < static_cast<int64_t>(machine_words_x.size())) {
          return machine_words_x;
        }
      }
      i += 2;
    } else {
      ++i;
    }
  }

  return machine_words_x.subspan(0, cand_len);
}

namespace {

bool DoesGasSwapBranchesForProfile(const CompilationProfile& profile) {
  int opt = profile.optimize_level;
  for (std::string_view flag : profile.c_flags) {
    if (flag == "-O0") {
      opt = 0;
    } else if (flag == "-O" || flag == "-O1") {
      opt = 1;
    } else if (flag == "-O2") {
      opt = 2;
    } else if (flag == "-O3") {
      opt = 3;
    }
  }
  int mips_opt = (opt <= 0) ? 1 : 2;
  for (std::string_view flag : profile.c_flags) {
    if (absl::StartsWith(flag, "-Wa,")) {
      for (std::string_view wa_opt : absl::StrSplit(flag.substr(4), ',')) {
        if (wa_opt == "-g" || wa_opt == "-g2") {
          mips_opt = 0;
        } else if (wa_opt == "-O0") {
          mips_opt = 1;
        } else if (absl::StartsWith(wa_opt, "-O")) {
          mips_opt = 2;
        }
      }
    }
  }
  return mips_opt >= 2;
}

std::vector<std::string> InvertGasAppendInsnInternal(
    absl::Span<const uint32_t> words_x, uint32_t linked_base_vram,
    absl::Span<const size_t> extra_label_indices,
    const absl::flat_hash_set<size_t>& external_j_indices,
    absl::flat_hash_map<size_t, std::string>* out_target_labels, bool gas_swaps_branches = true) {
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

  auto resolve_local_j_target = [&](size_t insn_idx, uint32_t w, size_t* out_idx) -> bool {
    if (external_j_indices.contains(insn_idx)) {
      return false;
    }
    if (linked_base_vram != 0u) {
      const uint32_t target_vram = (linked_base_vram & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
      if (target_vram >= linked_base_vram &&
          target_vram < linked_base_vram + static_cast<uint32_t>(words_x.size()) * 4u &&
          ((target_vram - linked_base_vram) & 3u) == 0u) {
        *out_idx = static_cast<size_t>((target_vram - linked_base_vram) >> 2);
        return true;
      }
      return false;
    }
    const size_t target_idx = static_cast<size_t>(w & 0x03ffffffu);
    if (target_idx > 0 && target_idx < words_x.size()) {
      *out_idx = target_idx;
      return true;
    }
    return false;
  };

  // Pass 1: Validate branch/delay-slot structure and assign local `$L<N>`
  // labels to in-bounds branch, local `j`, and `.rodata` jump-table targets.
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
        size_t target_idx = 0;
        if (resolve_local_j_target(i, w, &target_idx) && !target_labels.contains(target_idx)) {
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
  for (size_t extra_idx : extra_label_indices) {
    if (extra_idx < words_x.size() && !target_labels.contains(extra_idx)) {
      target_labels[extra_idx] = absl::StrFormat("$L%d", next_label_id++);
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
        size_t target_idx = 0;
        if (resolve_local_j_target(i, w, &target_idx)) {
          if (auto it = target_labels.find(target_idx); it != target_labels.end()) {
            jump_line = ReplaceTargetWithLabel(jump_line, it->second);
          }
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
        if (gas_swaps_branches && !has_label_at_i && !has_label_at_delay &&
            CanSwapBranchWithInsn(w, delay_w)) {
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
        const bool prev_would_swap = gas_swaps_branches && prev_insn_valid &&
                                     !prev_insn_is_delay_slot && !has_label_at_i &&
                                     CanSwapBranchWithInsn(w, prev_insn_word);
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

  if (out_target_labels != nullptr) {
    *out_target_labels = std::move(target_labels);
  }
  return lines;
}

}  // namespace

std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) {
  return InvertGasAppendInsnInternal(words_x, /*linked_base_vram=*/0u, /*extra_label_indices=*/{},
                                     /*external_j_indices=*/{}, /*out_target_labels=*/nullptr);
}

bool BuildAsmItemsFromAppendInsn(absl::Span<const uint32_t> words,
                                 absl::Span<const std::string> raw_lines,
                                 std::vector<AsmItem>* out_items) {
  out_items->clear();
  size_t w_idx = 0;

  for (size_t i = 0; i < raw_lines.size(); ++i) {
    const std::string& line = raw_lines[i];
    if (absl::EndsWith(line, ":")) {
      AsmItem item;
      item.kind = AsmItemKind::kLabel;
      item.line = line;
      out_items->push_back(std::move(item));
      continue;
    }

    if (line == ".set\tnoreorder" || line == ".set noreorder") {
      size_t end_nr = i + 1;
      while (end_nr < raw_lines.size() && raw_lines[end_nr] != ".set\treorder" &&
             raw_lines[end_nr] != ".set reorder") {
        ++end_nr;
      }
      if (end_nr >= raw_lines.size()) {
        return false;
      }
      const size_t body_len = end_nr - (i + 1);
      if (body_len == 1) {
        // 3-line `.set noreorder / <hazard_insn> / .set reorder` emitted by
        // `InvertGasAppendInsn` for a single hazard instruction.
        if (w_idx >= words.size()) {
          return false;
        }
        AsmItem item;
        item.kind = AsmItemKind::kReorderInsn;
        item.line = raw_lines[i + 1];
        item.word_index = w_idx;
        item.word = words[w_idx++];
        out_items->push_back(std::move(item));
        i = end_nr;
        continue;
      }
      if (body_len == 2) {
        if (w_idx + 1 >= words.size()) {
          return false;
        }
        AsmItem item;
        item.kind = AsmItemKind::kNoreorderBranchSeq;
        item.line = raw_lines[i + 1];
        item.delay_line = raw_lines[i + 2];
        item.word = words[w_idx];
        item.delay_word = words[w_idx + 1];
        item.word_index = w_idx;
        item.delay_word_index = w_idx + 1;
        w_idx += 2;
        out_items->push_back(std::move(item));
        i = end_nr;
        continue;
      }
      if (body_len == 3 && absl::EndsWith(raw_lines[i + 2], ":")) {
        if (w_idx + 1 >= words.size()) {
          return false;
        }
        AsmItem item;
        item.kind = AsmItemKind::kNoreorderBranchSeq;
        item.line = raw_lines[i + 1];
        item.delay_label = raw_lines[i + 2];
        item.delay_line = raw_lines[i + 3];
        item.word = words[w_idx];
        item.delay_word = words[w_idx + 1];
        item.word_index = w_idx;
        item.delay_word_index = w_idx + 1;
        w_idx += 2;
        out_items->push_back(std::move(item));
        i = end_nr;
        continue;
      }
      return false;
    }

    if (w_idx >= words.size()) {
      return false;
    }
    const uint32_t w = words[w_idx];
    if (IsBranchOrJumpWord(w)) {
      if (w_idx + 1 >= words.size()) {
        return false;
      }
      const uint32_t delay_w = words[w_idx + 1];
      if (delay_w == 0x00000000u) {
        AsmItem item;
        item.kind = AsmItemKind::kReorderInsn;
        item.line = line;
        item.word = w;
        item.word_index = w_idx;
        item.delay_word_index = w_idx + 1;
        out_items->push_back(std::move(item));
        w_idx += 2;
      } else {
        // `InvertGasAppendInsn` emitted `[delay_line, branch_line]` in `.set reorder`.
        if (i + 1 >= raw_lines.size()) {
          return false;
        }
        AsmItem delay_item;
        delay_item.kind = AsmItemKind::kReorderInsn;
        delay_item.line = line;
        delay_item.word = delay_w;
        delay_item.word_index = w_idx + 1;
        delay_item.unswapped_from_branch = true;
        out_items->push_back(std::move(delay_item));

        AsmItem branch_item;
        branch_item.kind = AsmItemKind::kReorderInsn;
        branch_item.line = raw_lines[i + 1];
        branch_item.word = w;
        branch_item.word_index = w_idx;
        branch_item.delay_word_index = w_idx + 1;
        branch_item.unswapped_from_branch = true;
        out_items->push_back(std::move(branch_item));

        ++i;
        w_idx += 2;
      }
    } else {
      AsmItem item;
      item.kind = AsmItemKind::kReorderInsn;
      item.line = line;
      item.word = w;
      item.word_index = w_idx;
      out_items->push_back(std::move(item));
      ++w_idx;
    }
  }

  return w_idx == words.size();
}

void StripGasInsertedHazardNops(std::vector<AsmItem>* items) {
  std::vector<AsmItem> out;
  out.reserve(items->size());

  uint32_t prev_w = 0u;
  uint32_t prev_prev_w = 0u;

  for (size_t i = 0; i < items->size();) {
    // Check if consecutive `nop` (`0x00000000u`) items starting at `i` were
    // automatically inserted by `append_insn` or `mips_emit_delays()` due to
    // `(prev_w, prev_prev_w)`.
    if (((GetInsnPinfo(prev_w) & kReorderHazardPinfoMask) != 0u ||
         (GetInsnPinfo(prev_prev_w) & kReorderHazardPinfoMask) != 0u) &&
        (*items)[i].kind == AsmItemKind::kReorderInsn && (*items)[i].word == 0x00000000u) {
      size_t nop_count = 0;
      while (i + nop_count < items->size() &&
             (*items)[i + nop_count].kind == AsmItemKind::kReorderInsn &&
             (*items)[i + nop_count].word == 0x00000000u) {
        ++nop_count;
      }
      const size_t next_idx = i + nop_count;
      size_t expected_nops = 0;
      if (next_idx >= items->size() || (*items)[next_idx].kind != AsmItemKind::kReorderInsn) {
        expected_nops = MipsEmitDelaysNops(prev_w, prev_prev_w);
      } else {
        const AsmItem& next_item = (*items)[next_idx];
        expected_nops = AppendInsnHazardNops(prev_w, prev_prev_w, next_item.word);
        // If `next_item` was un-swapped from `[branch_w, delay_w]` and
        // `AppendInsnHazardNops` does not account for `nop_count` whereas
        // `MipsEmitDelaysNops` does, `[branch_w, delay_w]` was emitted as a
        // `.set noreorder` `SEQUENCE` in `RunFinal()`.
        if (expected_nops != nop_count && next_item.unswapped_from_branch &&
            next_idx + 1 < items->size() && (*items)[next_idx + 1].unswapped_from_branch &&
            IsBranchOrJumpWord((*items)[next_idx + 1].word)) {
          const size_t emit_delays_nops = MipsEmitDelaysNops(prev_w, prev_prev_w);
          if (emit_delays_nops == nop_count) {
            AsmItem seq_item;
            seq_item.kind = AsmItemKind::kNoreorderBranchSeq;
            seq_item.line = (*items)[next_idx + 1].line;
            seq_item.delay_line = next_item.line;
            seq_item.word = (*items)[next_idx + 1].word;
            seq_item.delay_word = next_item.word;
            seq_item.word_index = (*items)[next_idx + 1].word_index;
            seq_item.delay_word_index = next_item.word_index;
            (*items)[next_idx] = std::move(seq_item);
            items->erase(items->begin() + static_cast<ptrdiff_t>(next_idx + 1));
            expected_nops = emit_delays_nops;
          }
        }
        // Under `-Wa,-g` (`mips_optimize == 0`), `tc-mips.c:append_insn`
        // unconditionally emits `MipsEmitDelaysNops(prev_w, prev_prev_w)`
        // hazard `nop`s after coprocessor/HI/LO hazard instructions even when
        // `next_item` does not read the hazard register. Since `RunFinal()`
        // never emits explicit `nop`s after hazard instructions in `.set reorder`
        // mode, any `nop_count <= MipsEmitDelaysNops(prev_w, prev_prev_w)` was
        // inserted by GAS.
        if (expected_nops == 0) {
          const size_t max_emit_nops = MipsEmitDelaysNops(prev_w, prev_prev_w);
          if (nop_count <= max_emit_nops) {
            expected_nops = nop_count;
          }
        }
      }

      if (expected_nops > 0 && nop_count >= expected_nops) {
        if (nop_count == expected_nops && next_idx < items->size() &&
            (*items)[next_idx].kind == AsmItemKind::kNoreorderBranchSeq &&
            (*items)[next_idx].delay_word == 0x00000000u &&
            (*items)[next_idx].delay_label.empty() &&
            (GetInsnPinfo((*items)[next_idx].word) & kInsnCondBranchLikely) == 0u &&
            !CanSwapBranchWithInsn((*items)[next_idx].word, prev_w)) {
          (*items)[next_idx].kind = AsmItemKind::kReorderInsn;
          (*items)[next_idx].delay_line.clear();
        }
        i += expected_nops;
        if (expected_nops == 1) {
          prev_prev_w = prev_w;
          prev_w = 0u;
        } else {
          prev_prev_w = 0u;
          prev_w = 0u;
        }
        continue;
      }
    }

    AsmItem& cur = (*items)[i];
    if (cur.kind == AsmItemKind::kLabel || cur.kind == AsmItemKind::kNoreorderBranchSeq) {
      prev_w = 0u;
      prev_prev_w = 0u;
      out.push_back(std::move(cur));
      ++i;
      continue;
    }

    // `cur.kind == AsmItemKind::kReorderInsn`
    if (MipsEmitDelaysNops(prev_w, prev_prev_w) > 0) {
      cur.preceded_by_active_hazard = true;
    }
    if (IsBranchOrJumpWord(cur.word)) {
      if (cur.unswapped_from_branch) {
        const uint32_t branch_pinfo = GetInsnPinfo(cur.word);
        if ((branch_pinfo & kInsnUncondBranchDelay) != 0u) {
          prev_w = 0u;
          prev_prev_w = 0u;
        } else {
          prev_prev_w = cur.word;
        }
      } else {
        // Branch in `.set reorder` mode with `nop` delay slot.
        prev_prev_w = cur.word;
        prev_w = 0u;
      }
    } else {
      const size_t auto_nops = AppendInsnHazardNops(prev_w, prev_prev_w, cur.word);
      if (auto_nops > 0) {
        prev_prev_w = 0u;
      } else {
        prev_prev_w = prev_w;
      }
      prev_w = cur.word;
    }

    out.push_back(std::move(cur));
    ++i;
  }

  *items = std::move(out);
}

bool InvertGasAppendInsnToItems(absl::Span<const uint32_t> machine_words_x,
                                std::vector<AsmItem>* out_items) {
  out_items->clear();
  if (machine_words_x.empty()) {
    return false;
  }

  const absl::Span<const uint32_t> words = StripSectionAlignmentPadding(machine_words_x);
  if (words.size() < 2 || !IsBranchOrJumpWord(words[words.size() - 2])) {
    return false;
  }

  const std::vector<std::string> raw_lines = InvertGasAppendInsn(words);
  if (raw_lines.empty()) {
    return false;
  }

  if (!BuildAsmItemsFromAppendInsn(words, raw_lines, out_items)) {
    out_items->clear();
    return false;
  }

  StripGasInsertedHazardNops(out_items);
  return !out_items->empty();
}

bool InvertGasAppendInsnToItems(const Stage0FunctionInput& stage0_input,
                                std::vector<AsmItem>* out_items,
                                absl::flat_hash_map<size_t, std::string>* out_word_to_label) {
  out_items->clear();
  if (out_word_to_label != nullptr) {
    out_word_to_label->clear();
  }
  const absl::Span<const uint32_t> full_words(stage0_input.machine_words);
  if (full_words.empty()) {
    return false;
  }

  const uint32_t linked_base_vram =
      (stage0_input.base_vram != 0u && stage0_input.relocations.empty()) ? stage0_input.base_vram
                                                                         : 0u;

  absl::flat_hash_set<size_t> external_j_indices;
  for (const Stage0Relocation& rel : stage0_input.relocations) {
    if (rel.kind == Stage0RelocKind::kMips26 && rel.word_index < full_words.size() &&
        ((full_words[rel.word_index] >> 26) & 0x3fu) == 0x02u && rel.symbol_name != ".text") {
      external_j_indices.insert(rel.word_index);
    }
  }

  std::vector<size_t> extra_label_indices;
  for (const Stage0JumpTable& jt : stage0_input.jump_tables) {
    for (size_t target_idx : jt.target_word_indices) {
      extra_label_indices.push_back(target_idx);
    }
  }

  absl::Span<const uint32_t> words = StripSectionAlignmentPadding(full_words);
  bool keep_full = false;
  for (const Stage0Relocation& rel : stage0_input.relocations) {
    if (rel.word_index >= words.size() && rel.word_index < full_words.size()) {
      keep_full = true;
      break;
    }
  }
  for (size_t idx : extra_label_indices) {
    if (idx >= words.size() && idx < full_words.size()) {
      keep_full = true;
      break;
    }
  }
  if (linked_base_vram != 0u) {
    for (size_t i = 0; i < words.size();) {
      const uint32_t w = words[i];
      if (IsBranchOrJumpWord(w)) {
        if (((w >> 26) & 0x3fu) == 0x02u) {
          const uint32_t target_vram = (linked_base_vram & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
          if (target_vram >= linked_base_vram + static_cast<uint32_t>(words.size()) * 4u &&
              target_vram < linked_base_vram + static_cast<uint32_t>(full_words.size()) * 4u) {
            keep_full = true;
            break;
          }
        }
        i += 2;
      } else {
        ++i;
      }
    }
  }
  if (keep_full) {
    words = full_words;
  }

  if (words.size() < 2) {
    return false;
  }
  // A function may end either with `[branch_or_jump, delay_slot]` or with an
  // out-of-line loop/jump-table block whose trailing branch was stripped or
  // padded; require only that no branch is missing its architectural delay slot
  // (enforced inside `InvertGasAppendInsnInternal`) and that the function has
  // at least one return/jump when no jump tables/relocations are present.
  if (stage0_input.jump_tables.empty() && stage0_input.relocations.empty() &&
      linked_base_vram == 0u && !IsBranchOrJumpWord(words[words.size() - 2])) {
    return false;
  }

  absl::flat_hash_map<size_t, std::string> target_labels;
  const bool gas_swaps_branches = DoesGasSwapBranchesForProfile(stage0_input.profile);
  const std::vector<std::string> raw_lines =
      InvertGasAppendInsnInternal(words, linked_base_vram, extra_label_indices, external_j_indices,
                                  &target_labels, gas_swaps_branches);
  if (raw_lines.empty()) {
    return false;
  }

  if (!BuildAsmItemsFromAppendInsn(words, raw_lines, out_items)) {
    out_items->clear();
    return false;
  }

  StripGasInsertedHazardNops(out_items);
  if (out_word_to_label != nullptr) {
    *out_word_to_label = std::move(target_labels);
  }
  return !out_items->empty();
}

}  // namespace rom_nom_nom::lifter
