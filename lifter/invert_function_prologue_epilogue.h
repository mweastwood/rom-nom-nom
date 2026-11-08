#ifndef ROM_NOM_NOM_LIFTER_INVERT_FUNCTION_PROLOGUE_EPILOGUE_H_
#define ROM_NOM_NOM_LIFTER_INVERT_FUNCTION_PROLOGUE_EPILOGUE_H_

#include <string_view>

#include "core/rtl_cfg.h"

namespace rom_nom_nom {
namespace lifter {

using PrologueEpilogueFrameSpec = ::rom_nom_nom::PrologueEpilogueFrameSpec;

// 1-to-1 mathematical right-inverse of GCC 2.7.2's MIPS prologue/epilogue and
// stack-frame sizing subsystem (`mips.c:compute_frame_size`,
// `mips.c:save_restore_insns`, `mips.c:function_prologue`,
// `mips.c:mips_expand_prologue`, `mips.c:function_epilogue`, and
// `mips.c:mips_expand_epilogue`).
//
// Given emitted assembly text `asm_text_x` (either full assembly with
// `.ent`/`.frame`/`.mask`/`.fmask`/`.end` directives or instruction-only
// prologue/epilogue sequences), recovers the exact input frame specification
// `(var_size, saved_gpr_mask)` such that executing GCC's prologue/epilogue
// emitter reproduces `asm_text_x`.
//
// Returns `{.var_size = -1, .saved_gpr_mask = 0}` on malformed, unaligned, or
// contradictory inputs outside the image of `function_prologue` /
// `function_epilogue`.
PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(std::string_view asm_text_x);

}  // namespace lifter

using lifter::InvertFunctionPrologueEpilogue;

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_INVERT_FUNCTION_PROLOGUE_EPILOGUE_H_
