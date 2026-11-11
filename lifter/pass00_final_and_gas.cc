#include "lifter/pass00_final_and_gas.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "lifter/invert_final_scan_insn.h"
#include "lifter/invert_function_prologue_epilogue.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_macro.h"

namespace rom_nom_nom::lifter {

RtlControlFlowGraph InvertFinalAndGasFunction(const Stage0FunctionInput& input_x) {
  std::vector<AsmItem> items;
  absl::flat_hash_map<size_t, std::string> word_to_label;
  if (!InvertGasAppendInsnToItems(input_x, &items, &word_to_label)) {
    return {};
  }

  std::vector<Stage0ResolvedJumpTable> jump_tables;
  std::vector<std::string> asm_lines =
      CollapseGasMacrosAndFlattenLines(items, input_x, word_to_label, &jump_tables);
  if (asm_lines.empty()) {
    return {};
  }

  RtlSidebandState sideband;
  ExtractFunctionPrologueEpilogue(&asm_lines, &sideband);
  return InvertFinalScanInsnLinesToCfg(asm_lines, sideband, jump_tables, input_x.constant_pool,
                                       &input_x.profile);
}

RtlControlFlowGraph InvertFinalAndGasFunction(absl::Span<const uint32_t> machine_words_x) {
  Stage0FunctionInput input;
  input.machine_words.assign(machine_words_x.begin(), machine_words_x.end());
  return InvertFinalAndGasFunction(input);
}

}  // namespace rom_nom_nom::lifter
