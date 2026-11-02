#ifndef LIFTER_INVERSE_RTL_TO_C_AST_STAGE_H_
#define LIFTER_INVERSE_RTL_TO_C_AST_STAGE_H_

#include "absl/status/status.h"
#include "lifter/decompilation_context.h"

namespace rom_nom_nom {

// Stage 13 of the Inverse KMC GCC 2.7.2 Pipeline (`InverseRtlToCAstStage`):
// Inverts GCC Stage 0 (`c-parse.in`, `c-decl.c`, `function.c`, `stmt.c`, `expr.c`, and `calls.c`,
// C AST -> `.00.rtl`) to lift the recovered pre-optimization function representation into a
// structured `FunctionDeclaration` C AST:
//   1. `function.c:expand_function_start` & `assign_parms`: Recovers formal parameter declarations,
//      types, and MIPS O32 ABI register/stack bindings (`$a0`..`$a3`, `$f12`/`$f14`, stack args).
//   2. `stmt.c:expand_decl` & `function.c:assign_stack_local`: Recovers register-allocated
//   (`reg/v`)
//      and stack-frame (`virtual_stack_vars_rtx` / `$sp`) local variable declarations.
//   3. `stmt.c:expand_start_cond`, `expand_start_loop`, `expand_start_case` & `expr.c:do_jump`:
//      Structures the control-flow graph into hierarchical `IfElse`, `Loop` (`while`, `do-while`,
//      `for`), `Switch`, and short-circuit `&&`/`||` regions via `ControlFlowStructurer`.
//   4. `expr.c:expand_expr`, `expand_assignment`, `store_expr`, `do_store_flag`,
//   `calls.c:expand_call`
//      & `stmt.c:expand_return`: Lifts basic-block instructions into C statements and expressions
//      via `AstConverter` and `AstPassPipeline`.
class InverseRtlToCAstLifter {
 public:
  // Executes Stage 13 (`.rtl` -> C AST) on `context`, populating `context->RootRegion()` and
  // `context->Ast()`.
  static absl::Status LiftRtlToCAst(DecompilationContext* context);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_INVERSE_RTL_TO_C_AST_STAGE_H_
