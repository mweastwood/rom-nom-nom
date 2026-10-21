#ifndef ROM_NOM_NOM_LIFTER_CALL_LIFTER_H_
#define ROM_NOM_NOM_LIFTER_CALL_LIFTER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/expression_builder.h"

namespace rom_nom_nom {

// Lifts a MIPS call instruction (`jal`, `jalr`, `bgezal`, `bltzal`, etc.) into high-level
// `LiftedStatement` nodes, recovering register/stack/floating-point arguments and return value
// assignments (`v0`, `v1`, `f0`, `f0_d`, or 64-bit `call_res_64`).
void LiftCallInstruction(
    const Instruction& inst, size_t inst_index,
    absl::Span<const Instruction> instructions_to_process, const SymbolIndex* symbol_index,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
    const absl::flat_hash_set<std::string>* functions_returning_v0,
    const absl::flat_hash_set<std::string>* functions_returning_f0, RegisterTracker& tracker,
    std::vector<LiftedStatement>& statements, bool sp_address_taken = false,
    const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info = nullptr,
    int32_t sp_base_offset = 16,
    const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets = nullptr,
    const ControlFlowGraph* cfg = nullptr, uint32_t current_block_id = 0);

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_CALL_LIFTER_H_
