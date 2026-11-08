#ifndef ROM_NOM_NOM_LIFTER_INVERT_GAS_MACRO_H_
#define ROM_NOM_NOM_LIFTER_INVERT_GAS_MACRO_H_

#include <cstdint>
#include <string>

#include "absl/types/span.h"

namespace rom_nom_nom {

// Exact 1-to-1 right-inverse of GNU Binutils 2.6 `gas/config/tc-mips.c`:
// `macro` (lines 2387-4785) and `load_register` (lines 2129-2314).
//
// Given a sequence of big-endian 32-bit MIPS III / VR4300 machine words
// `macro_words_x`, recovers a single MIPS assembly line `y` such that running
// `tc-mips.c:macro` (or `mips_ip` + `append_insn` for 1-word non-macro
// instructions) reproduces `macro_words_x` bit-for-bit.
//
// Returns an empty string `""` if `macro_words_x` is empty, if any word is not
// a valid instruction in the image of `mips_ip` (`InvertGasMipsIp(w).empty()`),
// or if a multi-word sequence is not in the image of `tc-mips.c:macro`.
std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x);

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_INVERT_GAS_MACRO_H_
