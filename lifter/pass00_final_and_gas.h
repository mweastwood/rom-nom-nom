#ifndef LIFTER_PASS00_FINAL_AND_GAS_H_
#define LIFTER_PASS00_FINAL_AND_GAS_H_

#include <cstdint>

#include "absl/types/span.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {

// Whole-stage right-inverse $g_{\text{final+gas}}$ of Stage 0 (`final` + GAS
// `md_assemble`):
//   `final.c:final` (`mips.c:function_prologue`, `final.c:final_scan_insn`,
//   `mips.c:function_epilogue`) + `tc-mips.c:md_assemble` (`macro`,
//   `append_insn`, `mips_ip`).
//
// Given a function's `Stage0FunctionInput` (`.text` machine words plus optional
// relocations, symbols, `.rodata` jump tables, constant-pool entries, and N64
// VRAM base), composes `InvertGasAppendInsn`, `InvertGasMacro`,
// `InvertFunctionPrologueEpilogue`, and `InvertFinalScanInsn` to recover a
// post-`dbr_schedule` `RtlControlFlowGraph` (`RtlPassKind::kDbr`).
//
// Returns an empty `RtlControlFlowGraph` (`rpo_blocks.empty()`) if the input is
// empty or contains invalid / malformed machine code.
RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x);
RtlControlFlowGraph InvertFinalAndGasFunction(absl::Span<const uint32_t> machine_words_x);

}  // namespace rom_nom_nom::lifter

namespace rom_nom_nom {
using lifter::InvertFinalAndGasFunction;
}  // namespace rom_nom_nom

#endif  // LIFTER_PASS00_FINAL_AND_GAS_H_
