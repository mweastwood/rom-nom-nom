#ifndef LIFTER_VARIABLE_DECLARATION_BUILDER_H_
#define LIFTER_VARIABLE_DECLARATION_BUILDER_H_

#include <memory>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/symbol_registry.h"
#include "lifter/ast_converter.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

// Checks whether a caller-saved register variable `r` (such as `v0`, `v1`, `arg0..arg3`,
// `temp_t0..temp_t9`, `f0..f18`) is never live across a function call in `body`, making it safe to
// pin to a physical caller-saved register with `register ... asm("$...")`.
bool IsCallerSavedSafeAcrossCalls(const CompoundStatement& body, bool is_leaf_func,
                                  std::string_view r);

// Prepends synthesized local variable declarations (including stack frame buffers `u8 sp[N]`,
// uninitialized-use `= 0` initializers via CFG reaching-definitions dataflow, and physical register
// `asm("$...")` annotations) to `*body`.
absl::Status PrependSynthesizedLocalDeclarations(
    const ControlFlowGraph& cfg, const StackFrameAnalysis& frame_analysis,
    const absl::flat_hash_set<std::string>& double_vars, const std::vector<CParameter>& parameters,
    const CType& return_type, const SymbolIndex* symbol_index, const AstConverterOptions& options,
    std::unique_ptr<CompoundStatement>* body);

}  // namespace rom_nom_nom

#endif  // LIFTER_VARIABLE_DECLARATION_BUILDER_H_
