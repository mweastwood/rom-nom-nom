#ifndef ROM_NOM_NOM_LIFTER_INVERT_GAS_APPEND_INSN_H_
#define ROM_NOM_NOM_LIFTER_INVERT_GAS_APPEND_INSN_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/types/span.h"

namespace rom_nom_nom {

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

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_INVERT_GAS_APPEND_INSN_H_
