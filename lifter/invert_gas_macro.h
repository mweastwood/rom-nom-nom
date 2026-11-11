#ifndef ROM_NOM_NOM_LIFTER_INVERT_GAS_MACRO_H_
#define ROM_NOM_NOM_LIFTER_INVERT_GAS_MACRO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_gas_append_insn.h"

namespace rom_nom_nom {
namespace lifter {

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

// Collapses multi-word GAS macros (`M_DIV_3`, `M_REM_3`, `M_DIVU_3`,
// `M_REMU_3`, `M_LI`, `la`, paired `l.d`/`s.d`, and symbolic loads/stores)
// across consecutive `kReorderInsn` items and flattens `raw_items` into
// pre-`macro` assembly lines for epilogue extraction and `InvertFinalScanInsn`.
std::vector<std::string> CollapseGasMacrosAndFlattenLines(absl::Span<const AsmItem> raw_items);

// Relocatable / linked-ROM Stage 0 overload of `CollapseGasMacrosAndFlattenLines`:
// resolves `jal` callees, distinct global symbols + addends, `.rodata`/`.lit4`/
// `.lit8` float/double constant pools (`li.s`/`li.d`/`$LC*`), `.rodata` switch
// jump tables, and preserves non-GAS-macro split `%hi`/`%lo` pairs as explicit
// `lui $r,%hi(sym+off)` and `%lo(sym+off)` instructions.
std::vector<std::string> CollapseGasMacrosAndFlattenLines(
    absl::Span<const AsmItem> raw_items, const Stage0FunctionInput& stage0_input,
    const absl::flat_hash_map<size_t, std::string>& word_to_label,
    std::vector<Stage0ResolvedJumpTable>* out_jump_tables = nullptr);

}  // namespace lifter

using lifter::CollapseGasMacrosAndFlattenLines;
using lifter::InvertGasMacro;

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_INVERT_GAS_MACRO_H_
