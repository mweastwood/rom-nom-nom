#include "lifter/pass00_final_and_gas.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"
#include "lifter/invert_gas_mips_ip.h"

namespace rom_nom_nom::lifter {
namespace {

// Bitmasks matching `binutils-2.6-src/include/opcode/mips.h` (`pinfo` flags).
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

// Strips 0..3 trailing `0x00000000` section-alignment padding words when
// `machine_words_x.size()` is a multiple of 4 and ends after a valid
// `[branch_or_jump, delay_slot]` pair.
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

enum class AsmItemKind {
  kLabel,
  kReorderInsn,
  kNoreorderBranchSeq,
};

struct AsmItem {
  AsmItemKind kind = AsmItemKind::kReorderInsn;
  std::string line;
  std::string delay_line;
  std::string delay_label;
  uint32_t word = 0u;
  uint32_t delay_word = 0u;
  bool unswapped_from_branch = false;
  bool preceded_by_active_hazard = false;
};

// Parses `InvertGasAppendInsn(words)` output into structured `AsmItem`s paired
// with their exact source machine words from `words`, unwrapping 3-line
// `.set noreorder / <hazard_insn> / .set reorder` blocks back into
// `kReorderInsn` items.
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
        delay_item.unswapped_from_branch = true;
        out_items->push_back(std::move(delay_item));

        AsmItem branch_item;
        branch_item.kind = AsmItemKind::kReorderInsn;
        branch_item.line = raw_lines[i + 1];
        branch_item.word = w;
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
      out_items->push_back(std::move(item));
      ++w_idx;
    }
  }

  return w_idx == words.size();
}

// Strips `nop` items (`0x00000000u`) that `RunGasMdAssemble` automatically
// inserts via `append_insn` hazard checks or `mips_emit_delays()` when hazard
// instructions are emitted in `.set reorder` mode by `RunFinal()`.
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

int ParseLuiZeroReg(std::string_view line) {
  if ((!absl::StartsWith(line, "lui $") && !absl::StartsWith(line, "lui\t$")) ||
      !absl::EndsWith(line, ",0x0")) {
    return -1;
  }
  int reg = -1;
  if (!absl::SimpleAtoi(line.substr(5, line.size() - 9), &reg) || reg < 1 || reg > 31) {
    return -1;
  }
  return reg;
}

bool ParseIndexedAddrAddu(std::string_view line, int expected_tempreg, int* out_base_reg) {
  const size_t sep = line.find_first_of(" \t");
  if (sep == std::string_view::npos) {
    return false;
  }
  const std::string_view mnem = line.substr(0, sep);
  if (mnem != "addu" && mnem != "daddu") {
    return false;
  }
  const std::string_view rest = absl::StripAsciiWhitespace(line.substr(sep + 1));
  const std::string prefix = absl::StrFormat("$%d,$%d,$", expected_tempreg, expected_tempreg);
  if (!absl::StartsWith(rest, prefix)) {
    return false;
  }
  int base_reg = -1;
  if (!absl::SimpleAtoi(rest.substr(prefix.size()), &base_reg) || base_reg < 1 || base_reg > 31) {
    return false;
  }
  *out_base_reg = base_reg;
  return true;
}

bool ParseSameRegAddiuOffset(std::string_view line, int expected_reg, int32_t* out_off) {
  const size_t sep = line.find_first_of(" \t");
  if (sep == std::string_view::npos) {
    return false;
  }
  const std::string_view mnem = line.substr(0, sep);
  if (mnem != "addiu" && mnem != "daddiu") {
    return false;
  }
  const std::string_view rest = absl::StripAsciiWhitespace(line.substr(sep + 1));
  const std::string prefix = absl::StrFormat("$%d,$%d,", expected_reg, expected_reg);
  if (!absl::StartsWith(rest, prefix)) {
    return false;
  }
  const std::string_view off_str = rest.substr(prefix.size());
  int32_t off = 0;
  if (!absl::SimpleAtoi(off_str, &off)) {
    return false;
  }
  *out_off = off;
  return true;
}

bool ParseBaseMemInsn(std::string_view line, int expected_base_reg, std::string* out_mnem,
                      std::string* out_reg, int32_t* out_off) {
  const size_t sep = line.find_first_of(" \t");
  if (sep == std::string_view::npos) {
    return false;
  }
  std::string_view mnem = line.substr(0, sep);
  if (mnem == "lwc1") mnem = "l.s";
  if (mnem == "swc1") mnem = "s.s";
  if (mnem == "ldc1") mnem = "l.d";
  if (mnem == "sdc1") mnem = "s.d";
  if (mnem != "lb" && mnem != "lbu" && mnem != "lh" && mnem != "lhu" && mnem != "lw" &&
      mnem != "lwu" && mnem != "ld" && mnem != "sb" && mnem != "sh" && mnem != "sw" &&
      mnem != "sd" && mnem != "l.s" && mnem != "s.s" && mnem != "l.d" && mnem != "s.d") {
    return false;
  }
  const std::string_view rest = absl::StripAsciiWhitespace(line.substr(sep + 1));
  const size_t comma = rest.find(',');
  const std::string suffix = absl::StrFormat("($%d)", expected_base_reg);
  if (comma == std::string_view::npos || !absl::EndsWith(rest, suffix)) {
    return false;
  }
  const std::string_view reg = rest.substr(0, comma);
  if (expected_base_reg != 1 && reg != absl::StrFormat("$%d", expected_base_reg)) {
    return false;
  }
  const std::string_view off_str = rest.substr(comma + 1, rest.size() - comma - 1 - suffix.size());
  int32_t off = 0;
  if (!off_str.empty() && !absl::SimpleAtoi(off_str, &off)) {
    return false;
  }
  *out_mnem = std::string(mnem);
  *out_reg = std::string(reg);
  *out_off = off;
  return true;
}

std::string FormatSymOffset(int32_t off) {
  if (off == 0) {
    return "__sym_0";
  }
  if (off > 0) {
    return absl::StrFormat("__sym_0+%d", off);
  }
  return absl::StrFormat("__sym_0%d", off);
}

bool IsPairedFpDoubleSecondHalf(std::string_view next_line, int expected_base_reg,
                                std::string_view first_mnem, std::string_view first_reg,
                                int32_t first_off) {
  if ((first_mnem != "l.s" && first_mnem != "s.s") || !absl::StartsWith(first_reg, "$f")) {
    return false;
  }
  int fpreg = -1;
  if (!absl::SimpleAtoi(first_reg.substr(2), &fpreg) || (fpreg % 2) != 0) {
    return false;
  }
  std::string next_mnem;
  std::string next_reg;
  int32_t next_off = 0;
  if (!ParseBaseMemInsn(next_line, expected_base_reg, &next_mnem, &next_reg, &next_off)) {
    return false;
  }
  return next_mnem == first_mnem && next_reg == absl::StrFormat("$f%d", fpreg + 1) &&
         next_off == first_off + 4;
}

std::string NormalizeCop1LoadStoreMnemonic(std::string_view line) {
  if (absl::StartsWith(line, "lwc1 ") || absl::StartsWith(line, "lwc1\t")) {
    return absl::StrCat("l.s ", absl::StripAsciiWhitespace(line.substr(5)));
  }
  if (absl::StartsWith(line, "swc1 ") || absl::StartsWith(line, "swc1\t")) {
    return absl::StrCat("s.s ", absl::StripAsciiWhitespace(line.substr(5)));
  }
  if (absl::StartsWith(line, "ldc1 ") || absl::StartsWith(line, "ldc1\t")) {
    return absl::StrCat("l.d ", absl::StripAsciiWhitespace(line.substr(5)));
  }
  if (absl::StartsWith(line, "sdc1 ") || absl::StartsWith(line, "sdc1\t")) {
    return absl::StrCat("s.d ", absl::StripAsciiWhitespace(line.substr(5)));
  }
  return std::string(line);
}

// Collapses multi-word GAS macros (`li`, `la`, and symbolic loads/stores)
// across consecutive `kReorderInsn` items and flattens `items` into assembly
// lines for epilogue extraction and `InvertFinalScanInsn`.
std::vector<std::string> CollapseGasMacrosAndFlattenLines(const std::vector<AsmItem>& raw_items) {
  // Pass 1: collapse multi-word `InvertGasMacro` expansions (`M_DIV_3`,
  // `M_REM_3`, `M_DIVU_3`, `M_REMU_3`, `M_LI`) and normalize single-instruction
  // mnemonics (`lwc1`/`swc1`/`ldc1`/`sdc1`, `jal`).
  std::vector<AsmItem> items;
  items.reserve(raw_items.size());
  const auto is_nop_delay_branch_or_reorder = [](const AsmItem& it) {
    return it.kind == AsmItemKind::kReorderInsn ||
           (it.kind == AsmItemKind::kNoreorderBranchSeq && it.delay_word == 0x00000000u &&
            it.delay_label.empty());
  };
  for (size_t i = 0; i < raw_items.size(); ++i) {
    AsmItem item = raw_items[i];
    if (item.kind == AsmItemKind::kReorderInsn) {
      // 11-word signed `div`/`rem` (`M_DIV_3` / `M_REM_3`) spanning 10 `AsmItem`s:
      if (i + 9 < raw_items.size() && is_nop_delay_branch_or_reorder(raw_items[i + 1]) &&
          raw_items[i + 2].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 2].word == 0x0007000du && raw_items[i + 3].kind == AsmItemKind::kLabel &&
          raw_items[i + 4].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 5].kind == AsmItemKind::kNoreorderBranchSeq &&
          is_nop_delay_branch_or_reorder(raw_items[i + 6]) &&
          raw_items[i + 7].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 7].word == 0x0006000du && raw_items[i + 8].kind == AsmItemKind::kLabel &&
          raw_items[i + 9].kind == AsmItemKind::kReorderInsn) {
        const uint32_t macro11[11] = {
            item.word,
            raw_items[i + 1].word,
            0x00000000u,
            raw_items[i + 2].word,
            raw_items[i + 4].word,
            raw_items[i + 5].word,
            raw_items[i + 5].delay_word,
            raw_items[i + 6].word,
            0x00000000u,
            raw_items[i + 7].word,
            raw_items[i + 9].word,
        };
        const std::string macro_asm = InvertGasMacro(macro11);
        if (!macro_asm.empty()) {
          item.line = macro_asm;
          item.unswapped_from_branch = false;
          item.preceded_by_active_hazard = false;
          items.push_back(std::move(item));
          i += 9;
          continue;
        }
      }

      // 5-word unsigned `divu`/`remu` (`M_DIVU_3` / `M_REMU_3`) spanning 5 `AsmItem`s:
      if (i + 4 < raw_items.size() && is_nop_delay_branch_or_reorder(raw_items[i + 1]) &&
          raw_items[i + 2].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 2].word == 0x0007000du && raw_items[i + 3].kind == AsmItemKind::kLabel &&
          raw_items[i + 4].kind == AsmItemKind::kReorderInsn) {
        const uint32_t macro5[5] = {
            item.word,
            raw_items[i + 1].word,
            0x00000000u,
            raw_items[i + 2].word,
            raw_items[i + 4].word,
        };
        const std::string macro_asm = InvertGasMacro(macro5);
        if (!macro_asm.empty()) {
          item.line = macro_asm;
          item.unswapped_from_branch = false;
          item.preceded_by_active_hazard = false;
          items.push_back(std::move(item));
          i += 4;
          continue;
        }
      }

      if (i + 1 < raw_items.size() && raw_items[i + 1].kind == AsmItemKind::kReorderInsn &&
          !IsBranchOrJumpWord(item.word) && !IsBranchOrJumpWord(raw_items[i + 1].word)) {
        const uint32_t pair[2] = {item.word, raw_items[i + 1].word};
        const std::string macro_asm = InvertGasMacro(pair);
        if (!macro_asm.empty() &&
            (absl::StartsWith(macro_asm, "li ") || absl::StartsWith(macro_asm, "li\t"))) {
          item.line = macro_asm;
          item.unswapped_from_branch = false;
          items.push_back(std::move(item));
          ++i;
          continue;
        }
      }
      item.line = NormalizeCop1LoadStoreMnemonic(item.line);
      if (item.line == "jal 0x0" || item.line == "jal\t0x0") {
        item.line = "jal __callee_0";
      }
    } else if (item.kind == AsmItemKind::kNoreorderBranchSeq) {
      item.line = NormalizeCop1LoadStoreMnemonic(item.line);
      if (item.line == "jal 0x0" || item.line == "jal\t0x0") {
        item.line = "jal __callee_0";
      }
      item.delay_line = NormalizeCop1LoadStoreMnemonic(item.delay_line);
    }
    items.push_back(std::move(item));
  }

  // Pass 2: collapse 2/3/4-instruction GAS symbolic macros (`lui $<tempreg>,0x0`
  // followed by symbolic load/store or `daddiu`/`addiu` for `la`).
  std::vector<AsmItem> collapsed;
  collapsed.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    AsmItem item = items[i];
    if (item.kind == AsmItemKind::kReorderInsn) {
      const int tempreg = ParseLuiZeroReg(item.line);
      if (tempreg >= 1) {
        // 1) 3-instruction (or 4-instruction paired `l.d`/`s.d`) indexed symbolic load/store:
        //    `lui $<tempreg>,0x0`
        //    `addu/daddu $<tempreg>,$<tempreg>,$<breg>`
        //    `<memop> <reg>,<off>($<tempreg>)`
        int base_reg = -1;
        if (i + 2 < items.size() && items[i + 1].kind == AsmItemKind::kReorderInsn &&
            items[i + 2].kind == AsmItemKind::kReorderInsn &&
            ParseIndexedAddrAddu(items[i + 1].line, tempreg, &base_reg)) {
          std::string mnem;
          std::string reg;
          int32_t off = 0;
          if (ParseBaseMemInsn(items[i + 2].line, tempreg, &mnem, &reg, &off)) {
            if (i + 3 < items.size() && items[i + 3].kind == AsmItemKind::kReorderInsn &&
                IsPairedFpDoubleSecondHalf(items[i + 3].line, tempreg, mnem, reg, off)) {
              item.line = absl::StrFormat("%s %s,%s($%d)", mnem == "l.s" ? "l.d" : "s.d", reg,
                                          FormatSymOffset(off), base_reg);
              item.unswapped_from_branch = false;
              collapsed.push_back(std::move(item));
              i += 3;
              continue;
            }
            item.line = absl::StrFormat("%s %s,%s($%d)", mnem, reg, FormatSymOffset(off), base_reg);
            item.unswapped_from_branch = false;
            collapsed.push_back(std::move(item));
            i += 2;
            continue;
          }
        }

        // 2) 2-instruction (or 3-instruction paired `l.d`/`s.d`) absolute symbolic load/store:
        //    `lui $<tempreg>,0x0`
        //    `<memop> <reg>,<off>($<tempreg>)`
        if (i + 1 < items.size() && items[i + 1].kind == AsmItemKind::kReorderInsn) {
          std::string mnem;
          std::string reg;
          int32_t off = 0;
          if (ParseBaseMemInsn(items[i + 1].line, tempreg, &mnem, &reg, &off)) {
            if (i + 2 < items.size() && items[i + 2].kind == AsmItemKind::kReorderInsn &&
                IsPairedFpDoubleSecondHalf(items[i + 2].line, tempreg, mnem, reg, off)) {
              item.line = absl::StrFormat("%s %s,%s", mnem == "l.s" ? "l.d" : "s.d", reg,
                                          FormatSymOffset(off));
              item.unswapped_from_branch = false;
              collapsed.push_back(std::move(item));
              i += 2;
              continue;
            }
            item.line = absl::StrFormat("%s %s,%s", mnem, reg, FormatSymOffset(off));
            item.unswapped_from_branch = false;
            collapsed.push_back(std::move(item));
            ++i;
            continue;
          }

          // 3) 2-instruction `la $<tempreg>,__sym_0+<off>` (`M_LA_AB`):
          //    `lui $<tempreg>,0x0`
          //    `daddiu/addiu $<tempreg>,$<tempreg>,<off>`
          int32_t la_off = 0;
          if (ParseSameRegAddiuOffset(items[i + 1].line, tempreg, &la_off)) {
            item.line = absl::StrFormat("la $%d,%s", tempreg, FormatSymOffset(la_off));
            item.unswapped_from_branch = false;
            collapsed.push_back(std::move(item));
            ++i;
            continue;
          }
        }
      }
    }
    collapsed.push_back(std::move(item));
  }

  // Pass 3: flatten into assembly lines, emitting any remaining non-hazard,
  // non-multi-line un-swapped `[delay_item, branch_item]` pairs as
  // `.set noreorder` `SEQUENCE`s.
  std::vector<std::string> lines;
  lines.reserve(collapsed.size() * 2);
  for (size_t i = 0; i < collapsed.size(); ++i) {
    const AsmItem& item = collapsed[i];
    if (item.kind == AsmItemKind::kLabel) {
      lines.push_back(item.line);
      continue;
    }
    if (item.kind == AsmItemKind::kNoreorderBranchSeq) {
      lines.push_back(".set\tnoreorder");
      lines.push_back(item.line);
      if (!item.delay_label.empty()) {
        lines.push_back(item.delay_label);
      }
      lines.push_back(item.delay_line);
      lines.push_back(".set\treorder");
      continue;
    }

    if (item.unswapped_from_branch && i + 1 < collapsed.size() &&
        collapsed[i + 1].kind == AsmItemKind::kReorderInsn &&
        collapsed[i + 1].unswapped_from_branch && !IsBranchOrJumpWord(item.word) &&
        IsBranchOrJumpWord(collapsed[i + 1].word)) {
      const bool part_of_prev_float_si =
          i > 0 && collapsed[i - 1].kind == AsmItemKind::kReorderInsn &&
          (absl::StartsWith(collapsed[i - 1].line, "mtc1 ") ||
           absl::StartsWith(collapsed[i - 1].line, "l.s ")) &&
          (absl::StartsWith(item.line, "cvt.s.w ") || absl::StartsWith(item.line, "cvt.d.w "));
      if (!item.preceded_by_active_hazard && !part_of_prev_float_si) {
        lines.push_back(".set\tnoreorder");
        lines.push_back(collapsed[i + 1].line);
        lines.push_back(item.line);
        lines.push_back(".set\treorder");
        ++i;
        continue;
      }
    }

    lines.push_back(item.line);
  }

  return lines;
}

bool HasFramePointerMoveInPrologue(absl::Span<const std::string> asm_lines) {
  for (const std::string& line : asm_lines) {
    if (absl::EndsWith(line, ":")) {
      break;
    }
    if (line == "move $30,$29" || line == "move\t$30,$29" || line == "addu $30,$29,$0" ||
        line == "addu\t$30,$29,$0" || line == "or $30,$29,$0" || line == "or\t$30,$29,$0") {
      return true;
    }
  }
  return false;
}

std::optional<int> ParseFprRestoreRegno(std::string_view line) {
  if (!absl::StartsWith(line, "l.d $f") && !absl::StartsWith(line, "l.d\t$f") &&
      !absl::StartsWith(line, "l.s $f") && !absl::StartsWith(line, "l.s\t$f")) {
    return std::nullopt;
  }
  const size_t comma = line.find(',');
  if (comma == std::string_view::npos || comma <= 6) {
    return std::nullopt;
  }
  int fpreg = -1;
  if (!absl::SimpleAtoi(line.substr(6, comma - 6), &fpreg) || fpreg < 20 || fpreg > 31) {
    return std::nullopt;
  }
  return 32 + fpreg;
}

bool IsSpRestoreAdjustLine(std::string_view line) {
  return absl::StartsWith(line, "addiu $29,$29,") || absl::StartsWith(line, "addiu\t$29,$29,") ||
         line == "addu $29,$29,$12" || line == "addu\t$29,$29,$12";
}

bool IsReturnJr31Line(std::string_view line) {
  return line == "jr $31" || line == "jr\t$31";
}

bool IsMoveSpFpLine(std::string_view line) {
  return line == "move $29,$30" || line == "move\t$29,$30" || line == "addu $29,$30,$0" ||
         line == "addu\t$29,$30,$0" || line == "or $29,$30,$0" || line == "or\t$29,$30,$0";
}

// Attempts to identify and strip a trailing `tsize > 0` epilogue emitted by
// `mips.c:function_epilogue` from `asm_lines`, populating `sideband` via
// `InvertFunctionPrologueEpilogue`.
bool TryExtractAndStripTrailingEpilogue(std::vector<std::string>* asm_lines,
                                        RtlSidebandState* sideband) {
  const size_t n = asm_lines->size();
  if (n < 2) {
    return false;
  }

  // Every `tsize > 0` epilogue ends with either:
  //   Pattern A (`.set reorder`, 2 lines at `[n - 2, n - 1]`):
  //     `addiu $29,$29,<tsize>` (or `addu $29,$29,$12`)
  //     `jr $31`
  //   Pattern B (`.set noreorder`, 4 lines at `[n - 4 .. n - 1]`):
  //     `.set\tnoreorder`
  //     `jr $31`
  //     `addiu $29,$29,<tsize>` (or `addu $29,$29,$12`)
  //     `.set\treorder`
  //   Pattern C (unscheduled `-Wa,-g` / `-O0` epilogue, 5 lines at `[n - 5 .. n - 1]`):
  //     `addiu $29,$29,<tsize>` (or `addu $29,$29,$12`)
  //     `.set\tnoreorder`
  //     `jr $31`
  //     `nop`
  //     `.set\treorder`
  size_t tail_start = n;
  std::string sp_adjust_line;
  if (IsReturnJr31Line((*asm_lines)[n - 1]) && IsSpRestoreAdjustLine((*asm_lines)[n - 2])) {
    tail_start = n - 2;
    sp_adjust_line = (*asm_lines)[n - 2];
  } else if (n >= 4 &&
             ((*asm_lines)[n - 4] == ".set\tnoreorder" ||
              (*asm_lines)[n - 4] == ".set noreorder") &&
             IsReturnJr31Line((*asm_lines)[n - 3]) && IsSpRestoreAdjustLine((*asm_lines)[n - 2]) &&
             ((*asm_lines)[n - 1] == ".set\treorder" || (*asm_lines)[n - 1] == ".set reorder")) {
    tail_start = n - 4;
    sp_adjust_line = (*asm_lines)[n - 2];
  } else if (n >= 5 && IsSpRestoreAdjustLine((*asm_lines)[n - 5]) &&
             ((*asm_lines)[n - 4] == ".set\tnoreorder" ||
              (*asm_lines)[n - 4] == ".set noreorder") &&
             IsReturnJr31Line((*asm_lines)[n - 3]) && (*asm_lines)[n - 2] == "nop" &&
             ((*asm_lines)[n - 1] == ".set\treorder" || (*asm_lines)[n - 1] == ".set reorder")) {
    tail_start = n - 5;
    sp_adjust_line = (*asm_lines)[n - 5];
  } else {
    return false;
  }

  // Normalize `addiu $29,$29,<tsize>` to `addu $29,$29,<tsize>` for
  // `InvertFunctionPrologueEpilogue`.
  std::string normalized_sp_adjust = sp_adjust_line;
  if (absl::StartsWith(normalized_sp_adjust, "addiu\t") ||
      absl::StartsWith(normalized_sp_adjust, "addiu ")) {
    normalized_sp_adjust.replace(0, 5, "addu");
  }

  const bool has_fp_in_prologue = HasFramePointerMoveInPrologue(*asm_lines);

  // Determine the earliest line index `min_start` after the prologue and after
  // the last label / directive before `tail_start`.
  size_t min_start = 1;
  for (size_t i = tail_start; i > 0; --i) {
    const std::string& l = (*asm_lines)[i - 1];
    if (absl::EndsWith(l, ":") || l.front() == '.') {
      min_start = i;
      break;
    }
  }

  // First check for `epilogue_delay != 0` with `load_only_r31`:
  // Preceding `tail_start` is `[..., lw $31,<off>($29/$13), <epilogue_delay_insn>]`.
  if (tail_start >= min_start + 2) {
    const std::string& delay_cand = (*asm_lines)[tail_start - 1];
    const std::string& lw_ra_cand = (*asm_lines)[tail_start - 2];
    if ((absl::StartsWith(lw_ra_cand, "lw $31,") || absl::StartsWith(lw_ra_cand, "lw\t$31,")) &&
        !absl::StartsWith(delay_cand, "lw ") && !absl::StartsWith(delay_cand, "lw\t") &&
        !absl::StartsWith(delay_cand, "l.d ") && !absl::StartsWith(delay_cand, "l.d\t") &&
        !absl::StartsWith(delay_cand, "l.s ") && !absl::StartsWith(delay_cand, "l.s\t")) {
      for (size_t cand_start = min_start; cand_start <= tail_start - 2; ++cand_start) {
        const std::string& first_l = (*asm_lines)[cand_start];
        if (IsMoveSpFpLine(first_l) && !has_fp_in_prologue) {
          continue;
        }
        std::vector<std::string> epilogue_lines;
        epilogue_lines.push_back(".set\tnoreorder");
        for (size_t k = cand_start; k < tail_start; ++k) {
          epilogue_lines.push_back((*asm_lines)[k]);
        }
        epilogue_lines.push_back("j\t$31");
        epilogue_lines.push_back(normalized_sp_adjust);
        epilogue_lines.push_back(".set\treorder");

        const PrologueEpilogueFrameSpec spec =
            InvertFunctionPrologueEpilogue(absl::StrJoin(epilogue_lines, "\n"));
        const std::string delay_pat = InvertFinalScanInsn(delay_cand);
        if (spec.var_size >= 0 && !delay_pat.empty()) {
          sideband->frame_size_bytes = spec.var_size;
          sideband->saved_gpr_regnos.clear();
          for (int r = 0; r < 32; ++r) {
            if ((spec.saved_gpr_mask >> r) & 1u) {
              sideband->saved_gpr_regnos.push_back(r);
            }
          }
          sideband->epilogue_delay_pattern = delay_pat;
          asm_lines->erase(asm_lines->begin() + static_cast<ptrdiff_t>(cand_start),
                           asm_lines->end());
          return true;
        }
      }
    }
  }

  // Standard `epilogue_delay == 0` epilogue: try the longest valid suffix
  // `asm_lines[cand_start .. tail_start - 1]` + `[normalized_sp_adjust, "j\t$31"]`
  // accepted by `InvertFunctionPrologueEpilogue`.
  for (size_t cand_start = min_start; cand_start <= tail_start; ++cand_start) {
    if (cand_start < tail_start) {
      const std::string& first_l = (*asm_lines)[cand_start];
      if (IsMoveSpFpLine(first_l) && !has_fp_in_prologue) {
        continue;
      }
    }
    std::vector<std::string> epilogue_lines;
    for (size_t k = cand_start; k < tail_start; ++k) {
      epilogue_lines.push_back((*asm_lines)[k]);
    }
    epilogue_lines.push_back(normalized_sp_adjust);
    epilogue_lines.push_back("j\t$31");

    const PrologueEpilogueFrameSpec spec =
        InvertFunctionPrologueEpilogue(absl::StrJoin(epilogue_lines, "\n"));
    if (spec.var_size >= 0) {
      if (has_fp_in_prologue && ((spec.saved_gpr_mask >> 30) & 1u) == 0u) {
        continue;
      }
      sideband->frame_size_bytes = spec.var_size;
      sideband->saved_gpr_regnos.clear();
      for (int r = 0; r < 32; ++r) {
        if ((spec.saved_gpr_mask >> r) & 1u) {
          sideband->saved_gpr_regnos.push_back(r);
        }
      }
      sideband->saved_fpr_regnos.clear();
      for (size_t k = cand_start; k < tail_start; ++k) {
        if (const std::optional<int> fpr = ParseFprRestoreRegno((*asm_lines)[k]); fpr.has_value()) {
          sideband->saved_fpr_regnos.push_back(*fpr);
        }
      }
      std::sort(sideband->saved_fpr_regnos.begin(), sideband->saved_fpr_regnos.end());
      asm_lines->erase(asm_lines->begin() + static_cast<ptrdiff_t>(cand_start), asm_lines->end());
      return true;
    }
  }

  return false;
}

}  // namespace

RtlControlFlowGraph InvertFinalAndGasFunction(absl::Span<const uint32_t> machine_words_x) {
  if (machine_words_x.empty()) {
    return {};
  }

  const absl::Span<const uint32_t> words = StripSectionAlignmentPadding(machine_words_x);
  if (words.size() < 2 || !IsBranchOrJumpWord(words[words.size() - 2])) {
    return {};
  }

  const std::vector<std::string> raw_lines = InvertGasAppendInsn(words);
  if (raw_lines.empty()) {
    return {};
  }

  std::vector<AsmItem> items;
  if (!BuildAsmItemsFromAppendInsn(words, raw_lines, &items)) {
    return {};
  }

  StripGasInsertedHazardNops(&items);
  std::vector<std::string> asm_lines = CollapseGasMacrosAndFlattenLines(items);
  if (asm_lines.empty()) {
    return {};
  }

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband.frame_pointer_needed = HasFramePointerMoveInPrologue(asm_lines);
  TryExtractAndStripTrailingEpilogue(&asm_lines, &cfg.sideband);

  std::vector<RtlBasicBlock> blocks;
  absl::flat_hash_map<std::string, size_t> label_to_block;
  RtlBasicBlock cur_blk;
  cur_blk.id = 0;

  for (size_t i = 0; i < asm_lines.size(); ++i) {
    const std::string& line = asm_lines[i];
    if (line == ".set\treorder" || line == ".set reorder") {
      continue;
    }
    if (line == ".set\tnoreorder" || line == ".set noreorder") {
      if (i + 2 < asm_lines.size() && asm_lines[i + 1] == "nop" &&
          (asm_lines[i + 2] == ".set\treorder" || asm_lines[i + 2] == ".set reorder")) {
        const std::string pat = InvertFinalScanInsn(asm_lines[i + 1]);
        if (pat.empty()) {
          return {};
        }
        cur_blk.AddPatternOnly(pat);
        i += 2;
        continue;
      }
      if (i + 3 < asm_lines.size() &&
          (asm_lines[i + 3] == ".set\treorder" || asm_lines[i + 3] == ".set reorder")) {
        const std::string pat0 = InvertFinalScanInsn(asm_lines[i + 1]);
        const std::string pat1 = InvertFinalScanInsn(asm_lines[i + 2]);
        if (pat0.empty() || pat1.empty()) {
          return {};
        }
        const bool is_call_seq = absl::StrContains(pat0, "(call (mem:");
        if (is_call_seq) {
          cur_blk.AddPatternOnly(
              absl::StrFormat("(sequence [(call_insn %s) (insn %s)])", pat0, pat1));
        } else if (absl::StartsWith(pat0, "(jump_insn/u ")) {
          cur_blk.AddPatternOnly(absl::StrFormat("(sequence [%s (insn/s %s)])", pat0, pat1));
        } else {
          cur_blk.AddPatternOnly(
              absl::StrFormat("(sequence [(jump_insn %s) (insn %s)])", pat0, pat1));
        }
        i += 3;
        if (!is_call_seq && i + 1 < asm_lines.size() && !absl::EndsWith(asm_lines[i + 1], ":")) {
          blocks.push_back(std::move(cur_blk));
          cur_blk = RtlBasicBlock{};
          cur_blk.id = blocks.size();
        }
        continue;
      }
      return {};
    }

    if (absl::EndsWith(line, ":")) {
      std::string_view lbl(line.data(), line.size() - 1);
      if (absl::StartsWith(lbl, "$L")) {
        lbl.remove_prefix(1);
      }
      if (!cur_blk.instructions.empty() || cur_blk.has_label) {
        blocks.push_back(std::move(cur_blk));
        cur_blk = RtlBasicBlock{};
        cur_blk.id = blocks.size();
      }
      cur_blk.has_label = true;
      label_to_block[std::string(lbl)] = cur_blk.id;
      continue;
    }

    std::string combined_line = line;
    if (i + 1 < asm_lines.size() &&
        (absl::StartsWith(line, "mtc1\t") || absl::StartsWith(line, "mtc1 ") ||
         absl::StartsWith(line, "l.s\t") || absl::StartsWith(line, "l.s ")) &&
        (absl::StartsWith(asm_lines[i + 1], "cvt.s.w\t") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.s.w ") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.d.w\t") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.d.w "))) {
      combined_line = absl::StrCat(line, "\n", asm_lines[i + 1]);
      ++i;
    } else if (absl::StartsWith(line, "lwl\t") || absl::StartsWith(line, "lwl ")) {
      const size_t max_len = std::min<size_t>(16, asm_lines.size() - i);
      for (size_t len = max_len; len >= 3; --len) {
        const std::string cand =
            absl::StrJoin(absl::MakeConstSpan(asm_lines).subspan(i, len), "\n");
        if (!InvertFinalScanInsn(cand).empty()) {
          combined_line = cand;
          i += len - 1;
          break;
        }
      }
    }

    const std::string pat = InvertFinalScanInsn(combined_line);
    if (pat.empty()) {
      return {};
    }
    cur_blk.AddPatternOnly(pat);
    if ((pat == "(return)" || absl::StartsWith(pat, "(set (pc) ") ||
         absl::StartsWith(pat, "(jump_insn")) &&
        i + 1 < asm_lines.size() && !absl::EndsWith(asm_lines[i + 1], ":")) {
      blocks.push_back(std::move(cur_blk));
      cur_blk = RtlBasicBlock{};
      cur_blk.id = blocks.size();
    }
  }

  if (!cur_blk.instructions.empty() || cur_blk.has_label || blocks.empty()) {
    blocks.push_back(std::move(cur_blk));
  }

  for (size_t b = 0; b < blocks.size(); ++b) {
    bool modified_block = false;
    for (size_t insn_idx = 0; insn_idx < blocks[b].instructions.size(); ++insn_idx) {
      std::string& pat = blocks[b].instructions[insn_idx];
      size_t pos = 0;
      while ((pos = pat.find("(label_ref ", pos)) != std::string::npos) {
        const size_t tok_start = pos + std::string_view("(label_ref ").size();
        const size_t tok_end = pat.find(')', tok_start);
        if (tok_end == std::string::npos) {
          break;
        }
        const std::string lbl_tok(
            absl::StripAsciiWhitespace(pat.substr(tok_start, tok_end - tok_start)));
        const size_t delay_insn_pos = pat.find("(insn");
        const bool is_jump_or_branch_insn = absl::StartsWith(pat, "(set (pc) ") ||
                                            absl::StartsWith(pat, "(jump_insn") ||
                                            absl::StartsWith(pat, "(sequence [(jump_insn");
        const bool is_in_delay_slot = absl::StartsWith(pat, "(sequence [") &&
                                      delay_insn_pos != std::string::npos && pos > delay_insn_pos;
        const bool is_addr_hi_lo = !is_jump_or_branch_insn || is_in_delay_slot ||
                                   absl::StrContains(pat.substr(0, pos), "(lo_sum:") ||
                                   absl::StrContains(pat.substr(0, pos), "(high:");
        if (insn_idx + 1 < blocks[b].instructions.size() || is_addr_hi_lo) {
          auto it = label_to_block.find(lbl_tok);
          if (it != label_to_block.end()) {
            const std::string replacement = absl::StrFormat("(label_ref __blk_%zu)", it->second);
            pat.replace(pos, tok_end - pos + 1, replacement);
            pos += replacement.size();
            modified_block = true;
            continue;
          }
        }
        pos = tok_end + 1;
      }
    }
    if (modified_block) {
      blocks[b].SyncRtlInsnsFromInstructions();
    }

    if (blocks[b].instructions.empty()) {
      if (b + 1 < blocks.size()) {
        blocks[b].successors = {b + 1};
      }
      continue;
    }
    const std::string& last_pat = blocks[b].instructions.back();
    if (last_pat == "(return)" || absl::StartsWith(last_pat, "(sequence [(jump_insn (return))")) {
      blocks[b].successors.clear();
      continue;
    }
    size_t search_from = 0;
    bool matched_branch_target = false;
    while (search_from < last_pat.size()) {
      const size_t lref_pos = last_pat.find("(label_ref ", search_from);
      if (lref_pos == std::string::npos) {
        break;
      }
      const size_t tok_start = lref_pos + std::string_view("(label_ref ").size();
      const size_t tok_end = last_pat.find(')', tok_start);
      if (tok_end == std::string::npos) {
        break;
      }
      const std::string lbl_tok(
          absl::StripAsciiWhitespace(last_pat.substr(tok_start, tok_end - tok_start)));
      search_from = tok_end + 1;
      if (absl::StartsWith(lbl_tok, "__blk_")) {
        continue;
      }
      auto it = label_to_block.find(lbl_tok);
      if (it != label_to_block.end()) {
        if (absl::StrContains(last_pat, "(if_then_else ")) {
          if (b + 1 < blocks.size()) {
            blocks[b].successors = {b + 1, it->second};
          } else {
            blocks[b].successors = {it->second};
          }
        } else {
          blocks[b].successors = {it->second};
        }
        matched_branch_target = true;
        break;
      }
    }
    if (matched_branch_target) {
      continue;
    }
    if (b + 1 < blocks.size()) {
      blocks[b].successors = {b + 1};
    }
  }

  cfg.rpo_blocks = std::move(blocks);
  return cfg;
}

}  // namespace rom_nom_nom::lifter
