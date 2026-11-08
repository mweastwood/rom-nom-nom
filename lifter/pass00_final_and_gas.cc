#include "lifter/pass00_final_and_gas.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"

namespace rom_nom_nom::lifter {

RtlControlFlowGraph InvertFinalAndGasFunction(absl::Span<const uint32_t> machine_words_x) {
  std::vector<AsmItem> items;
  if (!InvertGasAppendInsnToItems(machine_words_x, &items)) {
    return {};
  }

  std::vector<std::string> asm_lines = CollapseGasMacrosAndFlattenLines(items);
  if (asm_lines.empty()) {
    return {};
  }

  RtlSidebandState sideband;
  ExtractFunctionPrologueEpilogue(&asm_lines, &sideband);
  return InvertFinalScanInsnLinesToCfg(asm_lines, sideband);
}

}  // namespace rom_nom_nom::lifter
