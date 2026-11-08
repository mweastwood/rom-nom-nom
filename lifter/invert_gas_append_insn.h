#ifndef ROM_NOM_NOM_LIFTER_INVERT_GAS_APPEND_INSN_H_
#define ROM_NOM_NOM_LIFTER_INVERT_GAS_APPEND_INSN_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/types/span.h"

namespace rom_nom_nom {
namespace lifter {

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

// Returns the Binutils 2.6 `include/opcode/mips.h` `pinfo` bitmask for a 32-bit
// big-endian MIPS instruction word `w`.
uint32_t GetInsnPinfo(uint32_t w);

// Returns true iff `w` is an unconditional or conditional branch/jump
// instruction with a architectural delay slot (`INSN_UNCOND_BRANCH_DELAY`,
// `INSN_COND_BRANCH_DELAY`, or `INSN_COND_BRANCH_LIKELY`).
bool IsBranchOrJumpWord(uint32_t w);

// Strips 0..3 trailing `0x00000000` section-alignment padding words when
// `machine_words_x.size()` is a multiple of 4 and ends after a valid
// `[branch_or_jump, delay_slot]` pair.
absl::Span<const uint32_t> StripSectionAlignmentPadding(absl::Span<const uint32_t> machine_words_x);

// Exact 1-to-1 right-inverse of GNU Binutils 2.6 `gas/config/tc-mips.c`:
// `append_insn` (lines 1254-1745).
//
// Given a sequence of big-endian 32-bit MIPS III / VR4300 machine words
// `words_x`, recovers an ordered sequence of MIPS assembly lines (including
// `$L<N>:` local labels, `.set\tnoreorder` / `.set\treorder` directives where
// delay-slot or hazard-slot preservation requires them, and un-swapped
// `[delay_slot_insn, branch_insn]` pairs in `.set\treorder` mode where
// `append_insn` will swap them back) such that assembling the returned lines
// through `mips_ip` + `append_insn` reproduces `words_x` bit-for-bit.
//
// Returns an empty vector if `words_x` is empty, if any 32-bit word is not in
// the image of `mips_ip` (`InvertGasMipsIp(w).empty()`), or if `words_x` ends
// on a branch/jump instruction without a trailing delay-slot word.
std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x);

// Parses `InvertGasAppendInsn(words)` output into structured `AsmItem`s paired
// with their exact source machine words from `words`, unwrapping 3-line
// `.set noreorder / <hazard_insn> / .set reorder` blocks back into
// `kReorderInsn` items.
bool BuildAsmItemsFromAppendInsn(absl::Span<const uint32_t> words,
                                 absl::Span<const std::string> raw_lines,
                                 std::vector<AsmItem>* out_items);

// Strips `nop` items (`0x00000000u`) that `RunGasMdAssemble` automatically
// inserts via `append_insn` hazard checks or `mips_emit_delays()` when hazard
// instructions are emitted in `.set reorder` mode by `RunFinal()`.
void StripGasInsertedHazardNops(std::vector<AsmItem>* items);

// High-level stream inverse of `append_insn` for a whole function's machine
// words: strips trailing section-alignment padding, runs `InvertGasAppendInsn`,
// builds structured `AsmItem`s, and strips GAS-inserted `.set reorder` hazard
// `nop`s.
bool InvertGasAppendInsnToItems(absl::Span<const uint32_t> machine_words_x,
                                std::vector<AsmItem>* out_items);

}  // namespace lifter

using lifter::AsmItem;
using lifter::AsmItemKind;
using lifter::BuildAsmItemsFromAppendInsn;
using lifter::GetInsnPinfo;
using lifter::InvertGasAppendInsn;
using lifter::InvertGasAppendInsnToItems;
using lifter::IsBranchOrJumpWord;
using lifter::StripGasInsertedHazardNops;
using lifter::StripSectionAlignmentPadding;

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_INVERT_GAS_APPEND_INSN_H_
