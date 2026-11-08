#ifndef LIFTER_INVERT_FINAL_SCAN_INSN_H_
#define LIFTER_INVERT_FINAL_SCAN_INSN_H_

#include <string>
#include <string_view>

namespace rom_nom_nom::lifter {

// Right-inverse $g_{\text{final\_scan\_insn}}$ of GCC 2.7.2's
// `final.c:final_scan_insn` (plus `mips.c` / `mips.md` instruction output
// templates).
//
// Given a single emitted MIPS assembly instruction line `asm_line_x` (or a
// 2-line `mtc1`/`l.s` + `cvt.s.w`/`cvt.d.w` `floatsisf2`/`floatsidf2` pair),
// recovers a valid canonical GCC 2.7.2 RTL instruction pattern string $y$ such
// that `final_scan_insn(EmitPattern(y)) == asm_line_x`.
//
// Returns an empty string `""` if `asm_line_x` is empty, whitespace-only, a
// comment, an assembler directive, a label definition, or an unrecognized /
// malformed MIPS instruction.
std::string InvertFinalScanInsn(std::string_view asm_line_x);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertFinalScanInsn;
}  // namespace rom_nom_nom

#endif  // LIFTER_INVERT_FINAL_SCAN_INSN_H_
