#ifndef LIFTER_INVERT_GAS_MIPS_IP_H_
#define LIFTER_INVERT_GAS_MIPS_IP_H_

#include <cstdint>
#include <string>

namespace rom_nom_nom {

// Stage 0 subfunction 1: right-inverse of Binutils 2.6 `mips_ip` (`tc-mips.c`).
// Given a single 32-bit big-endian MIPS I/II/III machine instruction word
// `word_x`, returns the canonical single-instruction GAS assembly string `y`
// such that `mips_ip(y) == word_x`.
std::string InvertGasMipsIp(uint32_t word_x);

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_GAS_MIPS_IP_H_
