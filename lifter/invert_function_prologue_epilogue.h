#ifndef ROM_NOM_NOM_LIFTER_INVERT_FUNCTION_PROLOGUE_EPILOGUE_H_
#define ROM_NOM_NOM_LIFTER_INVERT_FUNCTION_PROLOGUE_EPILOGUE_H_

#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
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

// Returns true iff `asm_lines` contains a `move $30,$29` (`addu`/`or`
// `$30,$29,$0`) frame-pointer setup instruction in its entry basic block.
bool HasFramePointerMoveInPrologue(absl::Span<const std::string> asm_lines);

// Attempts to identify and strip a trailing `tsize > 0` epilogue emitted by
// `mips.c:function_epilogue` from `asm_lines`, populating `sideband` via
// `InvertFunctionPrologueEpilogue`.
bool TryExtractAndStripTrailingEpilogue(std::vector<std::string>* asm_lines,
                                        RtlSidebandState* sideband);

// Extracts `frame_pointer_needed`, `frame_size_bytes`, `saved_gpr_regnos`,
// `saved_fpr_regnos`, and `epilogue_delay_pattern` into `sideband` and strips
// the trailing `mips.c:function_epilogue` sequence from `asm_lines`.
void ExtractFunctionPrologueEpilogue(std::vector<std::string>* asm_lines,
                                     RtlSidebandState* sideband);

}  // namespace lifter

using lifter::ExtractFunctionPrologueEpilogue;
using lifter::HasFramePointerMoveInPrologue;
using lifter::InvertFunctionPrologueEpilogue;
using lifter::TryExtractAndStripTrailingEpilogue;

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_INVERT_FUNCTION_PROLOGUE_EPILOGUE_H_
